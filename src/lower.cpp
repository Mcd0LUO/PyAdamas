// lower.cpp —— AST → IR
//
// 这个阶段只做「翻译」，不做优化：每个表达式降级成一条或几条三地址指令，
// 每个语句降级成基本块 + 跳转。类型信息全部来自 TypeChecker，不在这里
// 重新推断 —— 两套推断迟早会分叉。
//
// 覆盖范围是后端当前能吃下的子集：标量运算、字符串、list/tuple、
// if/while/for/函数/闭包/调用/print。try/with/生成器这些还没轮到，
// 遇到时记一条错误并继续，让用户一次看到全部缺口。
#include "lower.h"

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "module.h"

namespace py23 {
namespace {

// ────────────────────────── 小工具 ──────────────────────────

// 只认字面量常量。默认参数、assert 消息这些地方够用；
// 更复杂的常量表达式留给后续的常量折叠。
struct ConstVal {
    TypePtr ty;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
};

std::optional<ConstVal> as_const(const Expr* e) {
    if (!e || e->kind != ExprKind::Constant) return std::nullopt;
    const auto& c = static_cast<const Constant&>(*e);
    ConstVal v;
    switch (c.ck) {
        case Constant::CK::None: v.ty = t_none(); return v;
        case Constant::CK::Bool: v.ty = t_bool(); v.i = c.b ? 1 : 0; return v;
        case Constant::CK::Int: v.ty = t_int(); v.i = c.i; return v;
        case Constant::CK::Float: v.ty = t_float(); v.f = c.f; return v;
        case Constant::CK::Str: v.ty = t_str(); v.s = c.s; return v;
        case Constant::CK::Ellipsis: return std::nullopt;
    }
    return std::nullopt;
}

BinKind map_bin(Tok t) {
    switch (t) {
        case Tok::PLUS: return BinKind::Add;
        case Tok::MINUS: return BinKind::Sub;
        case Tok::STAR: return BinKind::Mul;
        case Tok::SLASH: return BinKind::Div;
        case Tok::DOUBLESLASH: return BinKind::FloorDiv;
        case Tok::PERCENT: return BinKind::Mod;
        case Tok::DOUBLESTAR: return BinKind::Pow;
        case Tok::AMPER: return BinKind::BitAnd;
        case Tok::VBAR: return BinKind::BitOr;
        case Tok::CIRCUMFLEX: return BinKind::BitXor;
        case Tok::LSHIFT: return BinKind::Shl;
        case Tok::RSHIFT: return BinKind::Shr;
        default: return BinKind::Add;
    }
}

CmpKind map_cmp(CmpOp op) {
    switch (op) {
        case CmpOp::Lt: return CmpKind::Lt;
        case CmpOp::Le: return CmpKind::Le;
        case CmpOp::Gt: return CmpKind::Gt;
        case CmpOp::Ge: return CmpKind::Ge;
        case CmpOp::Eq: return CmpKind::Eq;
        case CmpOp::Ne: return CmpKind::Ne;
        case CmpOp::Is: return CmpKind::Eq;
        case CmpOp::IsNot: return CmpKind::Ne;
        default: return CmpKind::Eq;
    }
}

// 复合赋值号 → 二元运算符。两套 token 是分开的（`*=` 和 `*`），
// 直接丢给 map_bin 会落到 default 分支，`acc *= 10` 被悄悄编译成 `acc += 10`。
Tok aug_to_bin(Tok t) {
    switch (t) {
        case Tok::PLUSEQUAL: return Tok::PLUS;
        case Tok::MINUSEQUAL: return Tok::MINUS;
        case Tok::STAREQUAL: return Tok::STAR;
        case Tok::SLASHEQUAL: return Tok::SLASH;
        case Tok::DOUBLESLASHEQUAL: return Tok::DOUBLESLASH;
        case Tok::PERCENTEQUAL: return Tok::PERCENT;
        case Tok::DOUBLESTAREQUAL: return Tok::DOUBLESTAR;
        case Tok::AMPEREQUAL: return Tok::AMPER;
        case Tok::VBAREQUAL: return Tok::VBAR;
        case Tok::CIRCUMFLEXEQUAL: return Tok::CIRCUMFLEX;
        case Tok::LSHIFTEQUAL: return Tok::LSHIFT;
        case Tok::RSHIFTEQUAL: return Tok::RSHIFT;
        default: return t;
    }
}

struct CaptureVar {
    std::string name;
    TypePtr ty;
};

// 一个函数的闭包形态：env 是从祖先函数捕获的变量（cell 指针在闭包
// 对象里的下标），cells 是本函数自己的局部变量里被子孙函数捕获的。
struct ClosureMeta {
    std::vector<CaptureVar> env;
    std::unordered_map<std::string, int> env_index;
    std::vector<CaptureVar> cells;
    std::unordered_map<std::string, int> cell_index;
    // 自底向上传播后的需求集合：本函数及其子孙需要从祖先拿到的名字。
    std::unordered_set<std::string> needed;
};

struct FuncCtx {
    IRFunction* fn = nullptr;
    const Scope* scope = nullptr;
    std::unordered_map<std::string, int> slots;
    std::unordered_set<std::string> globals_decl;
    // 闭包：本函数需要从外层拿的变量，以及被内层捕获、需要转成 cell 的局部名。
    const ClosureMeta* closure = nullptr;
    std::unordered_map<std::string, int> cell_slots;   // 名字 → 存 cell 指针的槽
    std::unordered_map<std::string, int> param_slots;  // 被捕获形参的原始值槽
    int env_slot = -1;                                 // 存闭包指针（%r10）的槽
    // 嵌套函数直接引用自己的名字：用当前闭包指针，不需要 self cell。
    const FunctionDef* self_def = nullptr;
    TypePtr self_type;
    int self_idx = -1;
    // {break 目标块, continue 目标块}
    std::vector<std::pair<int, int>> loops;
    // 每个循环入口处的 cleanup 嵌套深度：break/continue 要跨过的清理层数。
    std::vector<size_t> loop_cleanup_depth;    // 词法上活跃的清理动作，按从外到内排列。return/break/continue 和
    // 异常 landing 都从这里生成对应动作。
    struct Cleanup {
        enum class Kind { With, AsyncWith, Finally, Handler, ExcDone } kind;
        // With / Finally / Handler：handler 记录所在的槽
        int handler_slot = -1;
        // Handler/With/Finally：setjmp 非 0 时跳转的异常展开块
        int land = -1;
        // With
        int mgr_slot = -1;
        int exit_slot = -1;
        int nargs = 0;
        TypePtr ret;
        // AsyncWith：await __aexit__() 的结果类型
        TypePtr inner_ret;
        // Finally
        std::vector<StmtPtr>* body = nullptr;
    };
    std::vector<Cleanup> cleanups;
    // 函数内 import 的模块别名；模块顶层的别名在 Lowerer::module_aliases_。
    std::unordered_map<std::string, ModuleInfoPtr> modules;
    bool is_module_top = false;
    int blk = 0;
    // 函数入口的临时值栈水位；return 时清理到它。
    VReg fn_mark = kNoReg;
    // 模块顶层函数：退出前释放全局变量并关掉临时栈。
    bool is_entry = false;
    // 表达式内部槽（and/or、条件表达式、链式比较）只是借用临时值，
    // 生命周期由临时栈负责；出口不能 dec，否则会重复释放。
    std::unordered_set<int> borrow_slots;
    // 本函数产生过多少个临时堆值。0 表示整条 RC 脚手架都可以省掉。
    size_t rc_pushes = 0;
    int fn_mark_block = -1;
    size_t fn_mark_pos = 0;
    // 生成器：槽位在堆帧里，临时值水位在每次 resume 时重新记录到槽里
    //（vreg 不跨 yield 存活）。
    bool is_generator = false;
    int gen_mark_slot = -1;
    int gen_next_state = 1;
    // async def：同一个帧状态机，await 作为第三种挂起。
    bool is_coroutine = false;
    bool is_async_generator = false;
    TypePtr async_result;
    // 每个挂起点：续跑块 + 挂起时的清理栈快照（close 时按它跑 finally/with）。
    struct GenState {
        int id = 0;
        int cont = -1;
        std::vector<Cleanup> cleanups;
    };
    std::vector<GenState> gen_states;
};

class Lowerer {
public:
    Lowerer(IRModule& m, TypeChecker& tc, std::string file, LowerOptions options = {})
        : mod_(m), tc_(tc), file_(std::move(file)), opts_(std::move(options)) {}

    void run(Module& ast);

    std::vector<LowerError>& errors() { return errors_; }

private:
    IRModule& mod_;
    TypeChecker& tc_;
    std::string file_;
    LowerOptions opts_;
    std::vector<LowerError> errors_;

    std::vector<std::unique_ptr<FuncCtx>> frames_;
    FuncCtx* st_ = nullptr;
    // 推导式的循环变量是独立作用域，不能污染函数槽表。这里按嵌套
    // 层次保存「名字 → 槽」，查找时内层优先。
    std::vector<std::unordered_map<std::string, int>> comp_slots_;

    // 模块导入：顶层别名 + 已插入的外部全局（按链接符号去重）。
    std::unordered_map<std::string, ModuleInfoPtr> module_aliases_;
    std::unordered_map<std::string, int> extern_globals_;
    // from m import C：类不是运行时值，只登记编译期元数据。
    std::unordered_map<std::string, ImportedClass> imported_classes_;
    // from math/sys import f：原生函数只能直接调用。
    struct NativeRef {
        std::string module;
        std::string member;
    };
    std::unordered_map<std::string, NativeRef> native_aliases_;

    std::unordered_map<std::string, int> global_ids_;
    std::unordered_map<std::string, FunctionDef*> func_defs_;
    // yield 只在语句级 / 赋值右值里可用：其它位置会有临时 vreg 跨挂起，
    // 而 vreg 的栈槽不随帧保存。
    const Expr* yield_ctx_node_ = nullptr;
    const Expr* await_ctx_node_ = nullptr;
    // 含 async for 的推导式会挂起，只能作为整条语句/赋值/返回值的根表达式：
    // 嵌套在别的表达式里时，外层还有未完成的临时值，挂起会一起清掉。
    const Expr* comp_ctx_node_ = nullptr;
    // 正在编译 finally 体：跨挂起的 finally 清理需要复制控制流，暂不支持。
    int finally_body_depth_ = 0;
    // 闭包捕获分析：函数树 + 每个函数的捕获形态。
    std::unordered_map<const FunctionDef*, FunctionDef*> func_parent_;
    std::unordered_map<const FunctionDef*, std::vector<FunctionDef*>> func_children_;
    std::vector<FunctionDef*> func_roots_;
    std::unordered_map<const FunctionDef*, ClosureMeta> closure_meta_;
    // 类：ClassInfo 是 typecheck 的产物，IRClass 是它的运行时形态。
    // 用 ClassInfo 指针做键，同名类重定义时不会串。
    std::unordered_map<std::string, ClassInfoPtr> classes_;
    std::unordered_map<const ClassInfo*, int> class_ids_;

    // ── 错误 ────────────────────────────────────────────────────

    void err(int line, int col, std::string msg) {
        errors_.push_back({line, col, std::move(msg)});
    }

    void unsupported(const Stmt& s, const char* what) {
        err(s.line, s.col, std::format("{} is not supported by the code generator yet", what));
    }

    // ── 块与值 ──────────────────────────────────────────────────

    Block& cur() { return st_->fn->blocks[st_->blk]; }

    int new_block(std::string name) {
        st_->fn->blocks.push_back(Block{std::move(name), {}});
        return static_cast<int>(st_->fn->blocks.size()) - 1;
    }

    void switch_to(int b) { st_->blk = b; }

    // noreturn 调用之后把当前块收尾，避免后端把后续块当成顺序落下。
    void end_unreachable() {
        int dead = new_block("unreachable");
        Inst j;
        j.op = IrOp::Jump;
        j.target = dead;
        emit(std::move(j));
    }

    VReg alloc(TypePtr ty) {
        VReg r = static_cast<VReg>(st_->fn->vreg_ty.size());
        st_->fn->vreg_ty.push_back(ty ? std::move(ty) : t_unknown());
        return r;
    }

    VReg def(TypePtr ty, Inst in) {
        VReg r = alloc(ty);
        in.dst = r;
        in.ty = ty;
        if (!cur().terminated()) cur().insts.push_back(std::move(in));
        return r;
    }

    void emit(Inst in) {
        in.dst = kNoReg;
        if (!cur().terminated()) cur().insts.push_back(std::move(in));
    }

    bool terminated() const { return st_->fn->blocks[st_->blk].terminated(); }

    VReg const_int(int64_t v) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v;
        return def(t_int(), std::move(in));
    }

    VReg const_bool(bool v) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v ? 1 : 0;
        return def(t_bool(), std::move(in));
    }

    VReg const_float(double v) {
        Inst in;
        in.op = IrOp::ConstFloat;
        in.fimm = v;
        return def(t_float(), std::move(in));
    }

    VReg const_none() {
        Inst in;
        in.op = IrOp::ConstNone;
        return def(t_none(), std::move(in));
    }

    VReg const_str(const std::string& s) {
        Inst in;
        in.op = IrOp::ConstStr;
        in.idx = mod_.intern_string(s);
        return def(t_str(), std::move(in));
    }

    int new_temp_slot(TypePtr ty) { return st_->fn->new_slot(std::move(ty)); }

    int new_borrow_slot(TypePtr ty) {
        int s = new_temp_slot(std::move(ty));
        st_->borrow_slots.insert(s);
        return s;
    }

    void store_borrow_slot(int slot, VReg v) { store_slot_raw(slot, v); }

    VReg load_slot(int slot, TypePtr ty) {
        Inst in;
        in.op = IrOp::LoadSlot;
        in.idx = slot;
        in.ty = ty;
        return def(ty ? ty : t_unknown(), std::move(in));
    }

    VReg load_global(int gi, TypePtr ty) {
        Inst in;
        in.op = IrOp::LoadGlobal;
        in.idx = gi;
        in.ty = ty;
        return def(ty ? ty : t_unknown(), std::move(in));
    }

    void store_slot_raw(int slot, VReg v) {
        Inst in;
        in.op = IrOp::StoreSlot;
        in.idx = slot;
        in.a = v;
        emit(std::move(in));
    }

    void store_global_raw(int gi, VReg v) {
        Inst in;
        in.op = IrOp::StoreGlobal;
        in.idx = gi;
        in.a = v;
        emit(std::move(in));
    }

    // ── 引用计数 ────────────────────────────────────────────────
    //
    // 所有权约定：
    //   槽 / 全局 / 字段 / 列表元素 持有引用；
    //   load / field.get / list.get 只是借用，不增加计数；
    //   每个新堆值产生时带 rc=1 进临时栈，语句边界统一清理。
    // unknown 类型不插桩：机器表示可能是整数，按指针 dec 会崩。

    void rc_inc(VReg v) { call_void(RtFn::RcInc, {v}); }
    void rc_dec(VReg v) { call_void(RtFn::RcDec, {v}); }
    // unknown 值可能是整数也可能是堆指针，交给运行时按注册表判断。
    void rc_inc_unknown(VReg v) { call_void(RtFn::RcIncUnknown, {v}); }

    VReg rc_mark() { return call_runtime(RtFn::RcMark, t_int(), {}); }
    void rc_cleanup(VReg mark) { call_void(RtFn::RcCleanup, {mark}); }

    VReg rc_mark_at(int& block, size_t& pos) {
        VReg m = rc_mark();
        block = st_->blk;
        pos = cur().insts.size() - 1;
        return m;
    }

    void push_temp(VReg v) {
        ++st_->rc_pushes;
        call_void(RtFn::RcTemp, {v});
    }

    void erase_mark(int block, size_t pos, VReg mark) {
        auto& ins = st_->fn->blocks[block].insts;
        if (pos < ins.size() && ins[pos].op == IrOp::CallRt &&
            static_cast<RtFn>(ins[pos].imm) == RtFn::RcMark && ins[pos].dst == mark)
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    void erase_cleanup(int block, VReg mark) {
        auto& ins = st_->fn->blocks[block].insts;
        std::erase_if(ins, [&](const Inst& in) {
            return in.op == IrOp::CallRt && static_cast<RtFn>(in.imm) == RtFn::RcCleanup &&
                   in.args.size() == 1 && in.args[0] == mark;
        });
    }

    // 产生一个堆值：把它交给临时栈，调用方不需要再管这个初始引用。
    VReg def_heap(TypePtr ty, Inst in) {
        VReg v = def(ty ? ty : t_unknown(), std::move(in));
        if (ir_is_heap(ty)) push_temp(v);
        return v;
    }

    // 运行时按机器字返回的值（生成器 next/send）静态类型只能是 unknown；
    // 用 mov 补一个静态类型，后续 store_slot 才能按堆类型 retain。
    VReg retype(VReg v, TypePtr ty) {
        if (!ty || type_eq(vreg_ty(v), ty)) return v;
        Inst mv;
        mv.op = IrOp::Mov;
        mv.a = v;
        mv.ty = ty;
        return def(ty, std::move(mv));
    }

    // 列表元素所有权类别，必须和 codegen 传给 py23_list_new 的值一致。
    static int list_kind(const TypePtr& t) {
        auto one = [](const TypePtr& e) -> int {
            if (!e || e->kind == TyKind::Unknown) return 2;
            return ir_is_heap(e) ? 1 : 0;
        };
        if (!t) return 2;
        if (t->kind == TyKind::List || t->kind == TyKind::Set || t->kind == TyKind::Dict)
            return t->sub.empty() ? 2 : one(t->sub[0]);
        if (t->kind == TyKind::Tuple) {
            if (t->sub.empty()) return 2;
            int k = one(t->sub[0]);
            for (size_t i = 1; i < t->sub.size(); ++i)
                if (one(t->sub[i]) != k) return 2;
            return k;
        }
        return 2;
    }

    // kind 1/2 且源类型已知是堆，写入前 retain。kind 2 只 retain 不 release
    //（类型看不见，销毁时不敢按元素 dec），代价是泄漏。源类型是 unknown
    // 时不能直接 rc_inc（可能是整数），走注册表保守判断。
    void retain_elem(VReg v, int kind) {
        if (kind == 0) return;
        TypePtr vt = vreg_ty(v);
        if (ir_is_heap(vt)) {
            rc_inc(v);
            return;
        }
        if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
    }

    // 哈希/比较类别：0 int/bool，1 str，2 float，3 其他（按机器字比较）。
    static int key_kind(const TypePtr& t) {
        if (!t) return 3;
        switch (t->kind) {
            case TyKind::Int:
            case TyKind::Bool:
            case TyKind::None:
                return 0;
            case TyKind::Str:
                return 1;
            case TyKind::Float:
                return 2;
            default:
                return 3;
        }
    }

    // 所有权类别：0 非堆，1 堆，2 unknown（只 retain，运行时不解引用）。
    static int own_kind(const TypePtr& t) {
        if (ir_is_heap(t)) return 1;
        if (!t || t->kind == TyKind::Unknown) return 2;
        return 0;
    }

    void store_slot(int slot, VReg v) {
        TypePtr dst = st_->fn->slot_ty[slot];
        if (ir_is_heap(vreg_ty(v))) rc_inc(v);
        if (ir_is_heap(dst)) {
            VReg old = load_slot(slot, dst);
            store_slot_raw(slot, v);
            rc_dec(old);
        } else {
            store_slot_raw(slot, v);
        }
    }

    void store_global(int gi, VReg v) {
        TypePtr dst = mod_.globals[gi].ty;
        if (ir_is_heap(vreg_ty(v))) rc_inc(v);
        if (ir_is_heap(dst)) {
            VReg old = load_global(gi, dst);
            store_global_raw(gi, v);
            rc_dec(old);
        } else {
            store_global_raw(gi, v);
        }
    }

    void store_field(TypePtr fty, VReg obj, int off, VReg v) {
        if (ir_is_heap(vreg_ty(v))) rc_inc(v);
        if (ir_is_heap(fty)) {
            VReg old = field_load(obj, off, fty);
            field_store(obj, off, v);
            rc_dec(old);
        } else {
            field_store(obj, off, v);
        }
    }

    void inc_params() {
        for (auto& p : st_->fn->params)
            if (ir_is_heap(p.ty)) rc_inc(load_slot(p.slot, p.ty));
    }

    // 函数出口释放所有堆槽：形参在入口 retain 过，局部变量在写入时 retain
    // 过。返回值必须先 rc_inc，再走到这里，否则槽一释放就成悬空指针。
    void release_slots() {
        for (size_t s = 0; s < st_->fn->slot_ty.size(); ++s) {
            if (st_->borrow_slots.count(static_cast<int>(s))) continue;
            TypePtr ty = st_->fn->slot_ty[s];
            if (!ir_is_heap(ty)) continue;
            rc_dec(load_slot(static_cast<int>(s), ty));
        }
    }

    void release_globals() {
        for (size_t gi = 0; gi < mod_.globals.size(); ++gi) {
            if (mod_.globals[gi].external) continue;
            TypePtr ty = mod_.globals[gi].ty;
            if (!ir_is_heap(ty)) continue;
            rc_dec(load_global(static_cast<int>(gi), ty));
        }
    }

    void emit_return(VReg v, TypePtr vty) {
        if (v != kNoReg && ir_is_heap(vty)) rc_inc(v);
        if (st_->fn_mark != kNoReg && st_->rc_pushes > 0) rc_cleanup(st_->fn_mark);
        release_slots();
        if (st_->is_entry) {
            release_globals();
            call_void(RtFn::GcShutdown, {});
            call_void(RtFn::RcShutdown, {});
        }
        Inst in;
        in.op = IrOp::Return;
        in.a = v;
        emit(std::move(in));
    }

    // ── 名字解析 ────────────────────────────────────────────────

    int global_idx(const std::string& name) {
        auto it = global_ids_.find(name);
        if (it != global_ids_.end()) return it->second;
        TypePtr ty = t_unknown();
        if (auto* ms = tc_.module_scope()) {
            auto sit = ms->vars.find(name);
            if (sit != ms->vars.end()) ty = sit->second.ty;
        }
        return global_idx_as(name, ty);
    }

    int global_idx_as(const std::string& name, TypePtr ty) {
        auto it = global_ids_.find(name);
        if (it != global_ids_.end()) {
            if (ty && mod_.globals[it->second].ty &&
                mod_.globals[it->second].ty->kind == TyKind::Unknown)
                mod_.globals[it->second].ty = ty;
            return it->second;
        }
        int id = static_cast<int>(mod_.globals.size());
        IRGlobal g;
        g.name = name;
        g.ty = ty ? ty : t_unknown();
        if (!opts_.module_symbol.empty())
            g.symbol = module_global_symbol(opts_.module_symbol, name);
        mod_.globals.push_back(std::move(g));
        global_ids_[name] = id;
        return id;
    }

    // ── 闭包 ────────────────────────────────────────────────────

    const ClosureMeta* closure() const { return st_ ? st_->closure : nullptr; }

    bool is_cell(const std::string& name) const {
        return st_ && st_->cell_slots.count(name) > 0;
    }

    int comp_slot(const std::string& name) const {
        for (auto it = comp_slots_.rbegin(); it != comp_slots_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return -1;
    }

    bool is_env(const std::string& name) const {
        auto* m = closure();
        return m && st_->env_slot >= 0 && m->env_index.count(name) > 0;
    }

    TypePtr captured_ty(const std::string& name) const {
        auto* m = closure();
        if (!m) return nullptr;
        if (auto it = m->cell_index.find(name); it != m->cell_index.end())
            return m->cells[static_cast<size_t>(it->second)].ty;
        if (auto it = m->env_index.find(name); it != m->env_index.end())
            return m->env[static_cast<size_t>(it->second)].ty;
        return nullptr;
    }

    VReg load_cell(const std::string& name) {
        TypePtr T = captured_ty(name);
        int slot = st_->cell_slots.at(name);
        return load_slot(slot, mk_list(T ? T : t_unknown()));
    }

    // cell 是单元素 list[T]；读出来是借用引用，写进去由 list_set 管旧值。
    VReg read_cell(const std::string& name) {
        VReg c = load_cell(name);
        TypePtr T = captured_ty(name);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = c;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    }

    VReg read_env(const std::string& name) {
        auto* m = closure();
        int idx = m->env_index.at(name);
        TypePtr T = captured_ty(name);
        VReg env = load_slot(st_->env_slot, t_unknown());
        VReg c = field_load(env, 16 + 8 * idx, mk_list(T ? T : t_unknown()));
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = c;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    }

    VReg cell_for_capture(const std::string& name) {
        if (is_cell(name)) return load_cell(name);
        if (is_env(name)) {
            auto* m = closure();
            int idx = m->env_index.at(name);
            TypePtr T = captured_ty(name);
            VReg env = load_slot(st_->env_slot, t_unknown());
            return field_load(env, 16 + 8 * idx, mk_list(T ? T : t_unknown()));
        }
        return kNoReg;
    }

    // 写回捕获变量：先按 cell 的元素类别 retain，list_set 负责 dec 旧值
    //（kind 1）；kind 2 只 retain 不 release，宁可泄漏。
    void write_cell(const std::string& name, VReg v, TypePtr vty) {
        TypePtr T = captured_ty(name);
        v = coerce(v, vty, T);
        VReg cell = is_cell(name) ? load_cell(name) : kNoReg;
        if (cell == kNoReg) {
            auto* m = closure();
            int idx = m->env_index.at(name);
            VReg env = load_slot(st_->env_slot, t_unknown());
            cell = field_load(env, 16 + 8 * idx, mk_list(T ? T : t_unknown()));
        }
        retain_elem(v, list_kind(mk_list(T ? T : t_unknown())));
        Inst in;
        in.op = IrOp::IndexStore;
        in.a = cell;
        in.b = const_int(0);
        in.c = v;
        emit(std::move(in));
    }

    VReg zero_value(TypePtr T) {
        if (!T) return const_none();
        switch (T->kind) {
            case TyKind::Int:
            case TyKind::Bool:
            case TyKind::None:
                return const_int(0);
            case TyKind::Float:
                return const_float(0.0);
            default:
                return const_none();
        }
    }

    // 被内层函数捕获的局部变量在入口转成单元素 list[T]（cell）。
    // 形参先用原始槽接实参，再把值 retain 进 cell。
    void create_cells() {
        auto* m = closure();
        if (!m) return;
        for (auto& cv : m->cells) {
            TypePtr T = cv.ty;
            TypePtr cell_ty = mk_list(T ? T : t_unknown());
            int kind = list_kind(cell_ty);
            VReg init;
            if (auto it = st_->param_slots.find(cv.name); it != st_->param_slots.end())
                init = load_slot(it->second, T);
            else
                init = zero_value(T);
            retain_elem(init, kind);
            Inst in;
            in.op = IrOp::MakeList;
            in.imm = kind;
            in.args = {init};
            VReg cell = def_heap(cell_ty, std::move(in));
            store_slot(st_->cell_slots.at(cv.name), cell);
        }
    }

    void bind_name_value(const std::string& name, VReg v, TypePtr vty) {
        if (is_cell(name)) {
            write_cell(name, v, vty);
        } else if (is_local(name)) {
            store_slot(st_->slots[name], v);
        } else {
            store_global(global_idx(name), v);
        }
    }

    TypePtr name_ty(const std::string& name) {
        if (int cs = comp_slot(name); cs >= 0) return st_->fn->slot_ty[cs];
        if (auto t = captured_ty(name)) return t;
        if (st_ && st_->scope) {
            auto it = st_->scope->vars.find(name);
            if (it != st_->scope->vars.end()) return it->second.ty;
        }
        if (auto* ms = tc_.module_scope()) {
            auto it = ms->vars.find(name);
            if (it != ms->vars.end()) return it->second.ty;
        }
        return t_unknown();
    }

    bool is_local(const std::string& name) const {
        if (!st_) return false;
        if (comp_slot(name) >= 0) return true;
        return st_->slots.count(name) > 0 || is_env(name);
    }

    // 一个名字的存储类型：cell/env 看捕获类型，局部看槽，模块级看全局表。
    TypePtr storage_ty(const std::string& name) {
        if (auto t = captured_ty(name)) return t;
        if (st_->globals_decl.count(name) || !is_local(name)) return mod_.globals[global_idx(name)].ty;
        return st_->fn->slot_ty[st_->slots[name]];
    }

    // 当前作用域里有没有这个名字（局部槽、捕获变量或 global 声明）。
    bool is_bound_here(const std::string& name) const {
        if (!st_) return false;
        if (st_->globals_decl.count(name)) return true;
        return is_local(name);
    }

    // 内置名可以被用户遮蔽：模块里定义了 print，它就不再是内置。
    bool is_builtin(const std::string& name) const {
        if (is_bound_here(name)) return false;
        if (auto* ms = tc_.module_scope())
            if (ms->vars.count(name)) return false;
        return true;
    }

    // ── 语句 ────────────────────────────────────────────────────

    void stmts(std::vector<StmtPtr>& body) {
        for (auto& s : body) stmt(*s);
    }

    void stmt(Stmt& s);

    void lower_if(If& f);
    void lower_while(While& f);
    void lower_for(For& f);
    void lower_for_range(For& f, Call& c);
    void lower_for_seq(For& f);
    void lower_with(With& w);
    void lower_with_item(With& w, size_t idx);
    void emit_cleanup_exits(size_t down_to);
    int push_handler();
    VReg setjmp_handler(int slot);
    VReg emit_exit_call(const FuncCtx::Cleanup& c, bool exceptional);
    void lower_try_core(Try& t);
    void lower_finally(Try& t);
    VReg lower_exc_match(Expr& type);
    VReg raise_exception(const std::string& type, VReg msg);
    void lower_funcdef(FunctionDef& f);
    void lower_assert(Assert& a);
    void lower_try(Try& t);
    void lower_raise(Raise& r);
    void lower_import(Import& im);
    void lower_import_from(ImportFrom& im);
    ModuleInfoPtr resolve_module_alias(const std::string& name) const;
    void ensure_module_init(const ModuleInfoPtr& mod);
    int extern_global(const std::string& symbol, const std::string& debug_name, TypePtr ty);

    // 模块属性读取（m.x / m.f），返回外部全局槽；类导入留给 0.14b。
    std::optional<int> module_member(Attribute& at, ModuleInfoPtr* owner = nullptr);
    VReg construct_imported(const ImportedClass& ic, Call& c, TypePtr cls_ty);
    VReg call_native(const std::string& module, const std::string& member, Call& c,
                     const FuncInfoPtr& info);
    bool builtin_value(const std::string& module, const std::string& member, VReg& out,
                       TypePtr& ty);
    static std::optional<RtFn> native_fn(const std::string& module, const std::string& member);
    void compile_ctor_wrapper(ClassDef& clsdef, const ClassInfoPtr& ci, int cid);

    // ── 类 ──────────────────────────────────────────────────────

    void collect_classes(std::vector<StmtPtr>& body);
    void lower_classdef(ClassDef& c);

    // 把一个函数体编译成 IRFunction，返回它的下标。普通函数和方法共用。
    int compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label);
    // 含 yield / await 的函数：编译 resume 状态机 + 创建包装，返回创建包装下标。
    // frame_kind: 0 生成器 / 1 协程 / 2 异步生成器
    int compile_suspendable_function(FunctionDef& f, FuncInfoPtr info, const std::string& label,
                                     int frame_kind);
    VReg lower_yield(Expr& y);
    VReg lower_yield_from(YieldFrom& y);
    // 把 v 作为一次 yield 挂起，返回恢复后 send 进来的值。
    VReg emit_yield(VReg v, TypePtr vt);
    VReg lower_await(Expr& e);
    // 把「等待 w」编译成挂起；返回恢复后 sent 槽里的结果值（堆结果已入临时栈）。
    // active_limit 用于清理路径：只把仍然挂在运行时栈上的外层 handler 计入快照。
    VReg suspend_on_awaitable(VReg w, TypePtr result_ty, int line, int col,
                              size_t active_limit = static_cast<size_t>(-1));
    void lower_async_with_item(With& w, size_t idx);
    void lower_async_for(For& f, VReg seq, TypePtr seq_ty);
    void gen_finish(VReg v = kNoReg, TypePtr vty = nullptr);
    void cleanup_gen_mark();
    void lower_for_gen(For& f, VReg seq, TypePtr seq_ty);
    VReg lower_asyncio(const std::string& member, Call& c);

    VReg lower_construct(Call& c, const ClassInfoPtr& cls);

    // 按签名重排实参：位置、关键字、默认值、数值转换。
    std::vector<VReg> bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* defn);

    void field_store(VReg obj, int off, VReg v) {
        Inst in;
        in.op = IrOp::FieldStore;
        in.a = obj;
        in.imm = off;
        in.b = v;
        emit(std::move(in));
    }

    VReg field_load(VReg obj, int off, TypePtr ty) {
        Inst in;
        in.op = IrOp::FieldLoad;
        in.a = obj;
        in.imm = off;
        return def(ty, std::move(in));
    }

    // Optional[T] 上的属性访问必须先确认不是 None。类型收窄做不到
    // 逐条路径成立，这里用一次运行时检查兜底：解引用空指针的段错误
    // 不带任何上下文，比一条 panic 难查得多。
    VReg guard_not_none(VReg v, const std::string& what) {
        int ok = new_block("nn.ok");
        int bad = new_block("nn.bad");
        Inst br;
        br.op = IrOp::Branch;
        br.a = v;
        br.target = ok;
        br.target2 = bad;
        emit(std::move(br));
        switch_to(bad);
        call_void(RtFn::Raise,
                  {const_str("AttributeError"), const_str("NoneType object has no " + what)});
        Inst j;
        j.op = IrOp::Jump;
        j.target = ok;
        emit(std::move(j));
        switch_to(ok);
        return v;
    }

    // 剥掉 Optional 外壳，返回内层类型；不是 Optional 就原样返回。
    static TypePtr strip_optional(const TypePtr& t) {
        if (t && t->kind == TyKind::Optional && !t->sub.empty()) return t->sub[0];
        return t;
    }

    void assign_to(Expr& target, VReg v, TypePtr vty);

    // ── 表达式 ──────────────────────────────────────────────────

    VReg expr(Expr& e);

    VReg lower_constant(Constant& c);
    VReg lower_binop(BinOp& b);
    VReg lower_unary(UnaryOp& u);
    VReg lower_boolop(BoolOp& b);
    VReg lower_compare(Compare& c);
    VReg lower_call(Call& c);
    VReg lower_builtin_call(const std::string& name, Call& c);
    VReg lower_print(Call& c);
    VReg lower_method_call(Attribute& at, Call& c);
    VReg lower_subscript(Subscript& s);
    VReg lower_list_like(std::vector<ExprPtr>& elts, TypePtr ty, bool is_tuple);
    VReg lower_dict(DictExpr& d, TypePtr ty);
    VReg lower_set(SetExpr& s, TypePtr ty);
    VReg lower_comp(Expr& e);
    VReg lower_ifexp(IfExp& e);
    VReg lower_joined(JoinedStr& j);
    VReg convert_to(VReg v, TypePtr from, TypePtr to);
    VReg coerce(VReg v, TypePtr from, TypePtr to);
    TypePtr vreg_ty(VReg v) const {
        return v < st_->fn->vreg_ty.size() ? st_->fn->vreg_ty[v] : t_unknown();
    }

    VReg call_runtime(RtFn f, TypePtr ret, std::vector<VReg> args) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(f);
        in.args = std::move(args);
        return def(std::move(ret), std::move(in));
    }

    // 返回 void 的运行时函数不分配结果寄存器，省掉一条无用的 movq
    void call_void(RtFn f, std::vector<VReg> args) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(f);
        in.args = std::move(args);
        emit(std::move(in));
    }

    void print_value(VReg v, TypePtr ty, int line, int col);

    // ── 预扫描 ──────────────────────────────────────────────────

    void hoist_globals(std::vector<StmtPtr>& body);
    void collect_funcdefs(std::vector<StmtPtr>& body, FunctionDef* parent = nullptr);
    void compute_closure_meta(FunctionDef* f);
    TypePtr capture_type(const FunctionDef* f, const std::string& name);
    void bind_name(Expr& target);
    void scan_globals(Stmt& s, bool enter_functions = false);

    void push_frame(IRFunction* fn, const Scope* scope) {
        auto ctx = std::make_unique<FuncCtx>();
        ctx->fn = fn;
        ctx->scope = scope;
        frames_.push_back(std::move(ctx));
        st_ = frames_.back().get();
    }

    void pop_frame() {
        frames_.pop_back();
        st_ = frames_.empty() ? nullptr : frames_.back().get();
    }
};

// ────────────────────────── 预扫描 ──────────────────────────

void Lowerer::bind_name(Expr& target) {
    switch (target.kind) {
        case ExprKind::Name:
            global_idx(static_cast<Name&>(target).id);
            break;
        case ExprKind::Tuple:
        case ExprKind::List:
            for (auto& e : (target.kind == ExprKind::Tuple
                                ? static_cast<TupleExpr&>(target).elts
                                : static_cast<ListExpr&>(target).elts))
                if (e) bind_name(*e);
            break;
        case ExprKind::Starred:
            bind_name(*static_cast<Starred&>(target).value);
            break;
        default:
            break;
    }
}

void Lowerer::scan_globals(Stmt& s, bool enter_functions) {
    switch (s.kind) {
        case StmtKind::Assign:
            for (auto& t : static_cast<Assign&>(s).targets) bind_name(*t);
            break;
        case StmtKind::AnnAssign:
        case StmtKind::AugAssign:
            bind_name(*static_cast<AnnAssign&>(s).target);
            break;
        case StmtKind::For: {
            auto& f = static_cast<For&>(s);
            bind_name(*f.target);
            for (auto& x : f.body) scan_globals(*x, enter_functions);
            for (auto& x : f.orelse) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::While: {
            auto& w = static_cast<While&>(s);
            for (auto& x : w.body) scan_globals(*x, enter_functions);
            for (auto& x : w.orelse) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::If: {
            auto& f = static_cast<If&>(s);
            for (auto& b : f.branches)
                for (auto& x : b.body) scan_globals(*x, enter_functions);
            for (auto& x : f.orelse) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::Try: {
            auto& t = static_cast<Try&>(s);
            for (auto& x : t.body) scan_globals(*x, enter_functions);
            for (auto& h : t.handlers) {
                if (!h.name.empty()) global_idx(h.name);
                for (auto& x : h.body) scan_globals(*x, enter_functions);
            }
            for (auto& x : t.orelse) scan_globals(*x, enter_functions);
            for (auto& x : t.finalbody) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::With: {
            auto& w = static_cast<With&>(s);
            for (auto& it : w.items)
                if (it.optional_vars) bind_name(*it.optional_vars);
            for (auto& x : w.body) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::FunctionDef: {
            auto& f = static_cast<FunctionDef&>(s);
            global_idx(f.name);
            if (enter_functions)
                for (auto& x : f.body) scan_globals(*x, enter_functions);
            break;
        }
        case StmtKind::ClassDef:
            global_idx(static_cast<ClassDef&>(s).name);
            break;
        case StmtKind::Import:
            // import m 只绑定编译期模块别名，不占运行时全局槽。
            break;
        case StmtKind::ImportFrom:
            for (auto& a : static_cast<ImportFrom&>(s).names)
                if (a.name != "*") global_idx(a.asname.empty() ? a.name : a.asname);
            break;
        default:
            break;
    }
}

void Lowerer::hoist_globals(std::vector<StmtPtr>& body) {
    for (auto& s : body) scan_globals(*s, false);
}

void Lowerer::collect_funcdefs(std::vector<StmtPtr>& body, FunctionDef* parent) {
    for (auto& s : body) {
        if (s->kind == StmtKind::FunctionDef) {
            auto& f = static_cast<FunctionDef&>(*s);
            func_defs_[f.name] = &f;
            func_parent_[&f] = parent;
            if (parent) func_children_[parent].push_back(&f);
            else func_roots_.push_back(&f);
            collect_funcdefs(f.body, &f);
        } else if (s->kind == StmtKind::ClassDef) {
            // 方法不是词法嵌套函数（类作用域不参与捕获），但方法体里的
            // def 仍是其嵌套函数，需要单独建树。
            for (auto& m : static_cast<ClassDef&>(*s).body) {
                if (m->kind != StmtKind::FunctionDef) continue;
                auto& f = static_cast<FunctionDef&>(*m);
                func_parent_[&f] = nullptr;
                func_roots_.push_back(&f);
                collect_funcdefs(f.body, &f);
            }
        } else if (s->kind == StmtKind::If) {
            for (auto& b : static_cast<If&>(*s).branches) collect_funcdefs(b.body, parent);
            collect_funcdefs(static_cast<If&>(*s).orelse, parent);
        } else if (s->kind == StmtKind::While) {
            collect_funcdefs(static_cast<While&>(*s).body, parent);
        } else if (s->kind == StmtKind::For) {
            collect_funcdefs(static_cast<For&>(*s).body, parent);
        } else if (s->kind == StmtKind::Try) {
            auto& t = static_cast<Try&>(*s);
            collect_funcdefs(t.body, parent);
            for (auto& h : t.handlers) collect_funcdefs(h.body, parent);
            collect_funcdefs(t.finalbody, parent);
        } else if (s->kind == StmtKind::With) {
            collect_funcdefs(static_cast<With&>(*s).body, parent);
        }
    }
}

TypePtr Lowerer::capture_type(const FunctionDef* f, const std::string& name) {
    const Scope* s = tc_.scope_of(f);
    for (s = s ? s->parent : nullptr; s; s = s->parent) {
        auto it = s->vars.find(name);
        if (it != s->vars.end()) return it->second.ty;
    }
    return t_unknown();
}

// 自底向上传播捕获集：子函数需要的名字，如果在本函数里有绑定，就变成
// 本函数的 cell；否则继续往上要。这样多层嵌套闭包也能拿到正确的 cell。
void Lowerer::compute_closure_meta(FunctionDef* f) {
    if (closure_meta_.count(f)) return;

    std::unordered_set<std::string> local_names;
    if (auto* sc = tc_.scope_of(f))
        for (auto& [n, sym] : sc->vars) local_names.insert(n);

    std::unordered_set<std::string> needed;
    for (auto& n : tc_.free_refs(f))
        if (n != f->name) needed.insert(n);  // 自引用走 env/静态闭包，不建 cell 环
    std::set<std::string> cell_names;
    for (auto* c : func_children_[f]) {
        compute_closure_meta(c);
        for (auto& n : closure_meta_[c].needed) {
            if (local_names.count(n)) cell_names.insert(n);
            else needed.insert(n);
        }
    }

    ClosureMeta meta;
    for (auto& n : cell_names) {
        TypePtr t = t_unknown();
        if (auto* sc = tc_.scope_of(f)) {
            auto it = sc->vars.find(n);
            if (it != sc->vars.end()) t = it->second.ty;
        }
        meta.cell_index[n] = static_cast<int>(meta.cells.size());
        meta.cells.push_back({n, t});
    }
    std::set<std::string> sorted_env(needed.begin(), needed.end());
    for (auto& n : sorted_env) {
        meta.env_index[n] = static_cast<int>(meta.env.size());
        meta.env.push_back({n, capture_type(f, n)});
    }
    meta.needed = std::move(needed);
    closure_meta_[f] = std::move(meta);
}

// 类表必须在编译任何方法体之前建好：方法里可能构造另一个类，
// 而构造指令只带一个类下标，对象大小和 vtable 位置都得先确定。
// 只扫语句块，不进函数体——函数内定义的类暂不支持。
void Lowerer::collect_classes(std::vector<StmtPtr>& body) {
    for (auto& s : body) {
        switch (s->kind) {
            case StmtKind::ClassDef: {
                auto& c = static_cast<ClassDef&>(*s);
                auto it = classes_.find(c.name);
                if (it == classes_.end()) break;
                const ClassInfoPtr& ci = it->second;
                if (class_ids_.count(ci.get())) break;

                IRClass irc;
                irc.name = c.name;
                irc.n_fields = ci->n_fields();
                // 字段偏移顺序 = 父类字段 + 本类字段；析构器按这个顺序 dec。
                {
                    std::vector<const ClassInfo*> chain;
                    for (const ClassInfo* k = ci.get(); k; k = k->base) chain.push_back(k);
                    for (auto kit = chain.rbegin(); kit != chain.rend(); ++kit)
                        for (auto& f : (*kit)->fields)
                            irc.field_heap.push_back(ir_is_heap(f.ty) ? 1 : 0);
                }
                irc.vtable.assign(static_cast<size_t>(ci->n_slots()), -1);
                class_ids_[ci.get()] = static_cast<int>(mod_.classes.size());
                mod_.classes.push_back(std::move(irc));
                break;
            }
            case StmtKind::If: {
                auto& f = static_cast<If&>(*s);
                for (auto& b : f.branches) collect_classes(b.body);
                collect_classes(f.orelse);
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(*s);
                collect_classes(w.body);
                collect_classes(w.orelse);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(*s);
                collect_classes(f.body);
                collect_classes(f.orelse);
                break;
            }
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(*s);
                collect_classes(t.body);
                for (auto& h : t.handlers) collect_classes(h.body);
                collect_classes(t.orelse);
                collect_classes(t.finalbody);
                break;
            }
            case StmtKind::With:
                collect_classes(static_cast<With&>(*s).body);
                break;
            default:
                break;
        }
    }
}

// ────────────────────────── 入口 ──────────────────────────

void Lowerer::run(Module& ast) {
    mod_.module_symbol = opts_.module_symbol;
    mod_.runtime_name = opts_.runtime_name;
    mod_.source_path = opts_.source_path.empty() ? file_ : opts_.source_path;
    mod_.is_entry = opts_.is_entry;

    hoist_globals(ast.body);
    collect_funcdefs(ast.body);
    for (auto* root : func_roots_) compute_closure_meta(root);
    classes_ = tc_.classes();
    collect_classes(ast.body);

    IRFunction* entry = mod_.add_func(opts_.is_entry ? "__py23_main" : "__py23_init");
    entry->ret = opts_.is_entry ? t_int() : t_none();
    mod_.entry = static_cast<int>(mod_.funcs.size()) - 1;

    push_frame(entry, tc_.module_scope());
    st_->is_module_top = true;
    int b = new_block("entry");
    switch_to(b);
    st_->is_entry = opts_.is_entry;
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    // __name__ / __file__：导入方也可能读它们，作为普通 str 全局存放。
    int name_gi = global_idx_as("__name__", t_str());
    store_global(name_gi, const_str(mod_.runtime_name));
    int file_gi = global_idx_as("__file__", t_str());
    store_global(file_gi, const_str(mod_.source_path));

    stmts(ast.body);

    if (!terminated())
        emit_return(opts_.is_entry ? const_int(0) : const_none(),
                    opts_.is_entry ? t_int() : t_none());
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);

    pop_frame();
}

// ────────────────────────── 语句 ──────────────────────────

void Lowerer::stmt(Stmt& s) {
    // 每条语句一个临时值栈水位：表达式产生的堆值在语句结束时统一释放。
    // 赋值/传参/返回已经各 retain 了一份，所以清理不会影响仍然存活的值。
    // 语句完全没产生临时堆值时，把 mark/cleanup 一起删掉（纯整数代码
    // 不该为 RC 付出任何运行时开销）。
    int mark_block = -1;
    size_t mark_pos = 0;
    VReg stmt_mark = st_->is_generator ? kNoReg : rc_mark_at(mark_block, mark_pos);
    size_t pushes_before = st_->rc_pushes;
    auto mark_comp = [&](Expr* v) {
        if (v && (v->kind == ExprKind::ListComp || v->kind == ExprKind::SetComp ||
                  v->kind == ExprKind::DictComp || v->kind == ExprKind::GeneratorExp))
            comp_ctx_node_ = v;
    };
    switch (s.kind) {
        case StmtKind::Expr: {
            auto& es = static_cast<ExprStmt&>(s);
            if (es.value->kind == ExprKind::Yield || es.value->kind == ExprKind::YieldFrom)
                yield_ctx_node_ = es.value.get();
            if (es.value->kind == ExprKind::Await) await_ctx_node_ = es.value.get();
            mark_comp(es.value.get());
            expr(*es.value);
            yield_ctx_node_ = nullptr;
            await_ctx_node_ = nullptr;
            comp_ctx_node_ = nullptr;
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<Assign&>(s);
            if (a.value->kind == ExprKind::Yield || a.value->kind == ExprKind::YieldFrom)
                yield_ctx_node_ = a.value.get();
            if (a.value->kind == ExprKind::Await) await_ctx_node_ = a.value.get();
            mark_comp(a.value.get());
            VReg v = expr(*a.value);
            yield_ctx_node_ = nullptr;
            await_ctx_node_ = nullptr;
            comp_ctx_node_ = nullptr;
            TypePtr vt = tc_.type_of(a.value.get());
            for (auto& t : a.targets) assign_to(*t, v, vt);
            break;
        }
        case StmtKind::AugAssign: {
            auto& a = static_cast<AugAssign&>(s);
            VReg cur_v = expr(*a.target);
            TypePtr cur_t = tc_.type_of(a.target.get());
            VReg rhs = expr(*a.value);
            TypePtr rhs_t = tc_.type_of(a.value.get());
            TypePtr ty = tc_.type_of(a.target.get());
            if (ty && ty->kind == TyKind::Float) {
                if (!ir_is_float(cur_t)) cur_v = convert_to(cur_v, cur_t, t_float());
                if (!ir_is_float(rhs_t)) rhs = convert_to(rhs, rhs_t, t_float());
            }
            Inst in;
            in.op = IrOp::Bin;
            in.bop = map_bin(aug_to_bin(a.op));
            in.a = cur_v;
            in.b = rhs;
            VReg r = def_heap(ty, std::move(in));
            // 下标目标会重新求值一次下标；Python 只求一次。
            // 纯下标表达式没有副作用，先这样，等有优化遍再消。
            assign_to(*a.target, r, ty);
            break;
        }
        case StmtKind::AnnAssign: {
            auto& a = static_cast<AnnAssign&>(s);
            if (!a.value) break;  // 纯声明：槽本来就是 0（None）
            TypePtr vt = tc_.type_of(a.value.get());
            // `x: list[float] = [1, 2]`：字面量必须按标注的元素类型构造。
            // 照字面量自己的 list[int] 构造，槽会被当成 list[float] 读，
            // 每个元素都是整数位模式冒充 double。
            TypePtr want;
            if (a.target->kind == ExprKind::Name)
                want = storage_ty(static_cast<Name&>(*a.target).id);
            VReg v;
            bool typed_literal = want && vt && want->kind == vt->kind &&
                                 (want->kind == TyKind::List || want->kind == TyKind::Tuple);
            if (a.value->kind == ExprKind::Yield || a.value->kind == ExprKind::YieldFrom)
                yield_ctx_node_ = a.value.get();
            if (a.value->kind == ExprKind::Await) await_ctx_node_ = a.value.get();
            mark_comp(a.value.get());
            if (typed_literal && a.value->kind == ExprKind::List)
                v = lower_list_like(static_cast<ListExpr&>(*a.value).elts, want, false);
            else if (typed_literal && a.value->kind == ExprKind::Tuple)
                v = lower_list_like(static_cast<TupleExpr&>(*a.value).elts, want, true);
            else
                v = expr(*a.value);
            yield_ctx_node_ = nullptr;
            await_ctx_node_ = nullptr;
            comp_ctx_node_ = nullptr;
            assign_to(*a.target, v, want ? want : vt);
            break;
        }
        case StmtKind::Return: {
            auto& r = static_cast<Return&>(s);
            if (st_->is_generator) {
                if (r.value && r.value->kind == ExprKind::Await)
                    await_ctx_node_ = r.value.get();
                mark_comp(r.value.get());
                VReg v = r.value ? expr(*r.value) : kNoReg;
                await_ctx_node_ = nullptr;
                comp_ctx_node_ = nullptr;
                if (st_->is_coroutine && !st_->is_async_generator) {
                    // 协程的返回值要送到 await 处（或 asyncio.run）。
                    if (v != kNoReg && st_->async_result)
                        v = coerce(v, vreg_ty(v), st_->async_result);
                    // 清理里可能有 await（async with 的 __aexit__）：
                    // vreg 不跨挂起，返回值先落帧槽。
                    int ret_slot = -1;
                    TypePtr rty = v != kNoReg ? vreg_ty(v) : nullptr;
                    if (v != kNoReg) {
                        ret_slot = new_temp_slot(rty);
                        store_slot(ret_slot, v);
                    }
                    emit_cleanup_exits(0);
                    if (ret_slot >= 0) v = load_slot(ret_slot, rty);
                    gen_finish(v, rty);
                    break;
                }
                // 生成器/异步生成器的 return 只表示结束，返回值不保留。
                emit_cleanup_exits(0);
                gen_finish();
                break;
            }
            VReg v = r.value ? expr(*r.value) : kNoReg;
            // `def f() -> float: return 1` 要在返回处转成 double
            if (v != kNoReg && st_->fn->ret) v = coerce(v, vreg_ty(v), st_->fn->ret);
            // return 也是退出路径：先跑完所有活跃清理动作。
            emit_cleanup_exits(0);
            emit_return(v, v != kNoReg ? vreg_ty(v) : nullptr);
            break;
        }
        case StmtKind::Break: {
            if (st_->loops.empty()) {
                unsupported(s, "'break'");
                break;
            }
            emit_cleanup_exits(st_->loop_cleanup_depth.back());
            Inst in;
            in.op = IrOp::Jump;
            in.target = st_->loops.back().first;
            emit(std::move(in));
            break;
        }
        case StmtKind::Continue: {
            if (st_->loops.empty()) {
                unsupported(s, "'continue'");
                break;
            }
            emit_cleanup_exits(st_->loop_cleanup_depth.back());
            Inst in;
            in.op = IrOp::Jump;
            in.target = st_->loops.back().second;
            emit(std::move(in));
            break;
        }
        case StmtKind::Pass:
            break;
        case StmtKind::If: lower_if(static_cast<If&>(s)); break;
        case StmtKind::While: lower_while(static_cast<While&>(s)); break;
        case StmtKind::For: lower_for(static_cast<For&>(s)); break;
        case StmtKind::FunctionDef: lower_funcdef(static_cast<FunctionDef&>(s)); break;
        case StmtKind::Assert: lower_assert(static_cast<Assert&>(s)); break;
        case StmtKind::Try: lower_try(static_cast<Try&>(s)); break;
        case StmtKind::Delete: unsupported(s, "'del'"); break;
        case StmtKind::Raise: lower_raise(static_cast<Raise&>(s)); break;
        case StmtKind::With: lower_with(static_cast<With&>(s)); break;
        case StmtKind::ClassDef: lower_classdef(static_cast<ClassDef&>(s)); break;
        case StmtKind::Import: lower_import(static_cast<Import&>(s)); break;
        case StmtKind::ImportFrom: lower_import_from(static_cast<ImportFrom&>(s)); break;
        case StmtKind::Global: {
            for (auto& n : static_cast<Global&>(s).names) st_->globals_decl.insert(n);
            break;
        }
        case StmtKind::Nonlocal: break;  // 名字解析已经由 cell/env 接管
    }
    if (st_->rc_pushes != pushes_before) {
        if (!terminated()) {
            if (st_->is_generator) cleanup_gen_mark();
            else rc_cleanup(stmt_mark);
        }
    } else if (!st_->is_generator) {
        if (!terminated() && !cur().insts.empty()) {
            const Inst& last = cur().insts.back();
            if (last.op == IrOp::CallRt && static_cast<RtFn>(last.imm) == RtFn::RcCleanup &&
                last.args.size() == 1 && last.args[0] == stmt_mark)
                cur().insts.pop_back();
        }
        erase_mark(mark_block, mark_pos, stmt_mark);
    }
}

void Lowerer::lower_if(If& f) {
    int end = new_block("if.end");

    for (size_t i = 0; i < f.branches.size(); ++i) {
        auto& br = f.branches[i];
        VReg c = expr(*br.cond);
        int then_b = new_block("if.then");
        int next_b;
        if (i + 1 < f.branches.size())
            next_b = new_block("if.next");
        else
            next_b = f.orelse.empty() ? end : new_block("if.else");

        Inst j;
        j.op = IrOp::Branch;
        j.a = c;
        j.target = then_b;
        j.target2 = next_b;
        emit(std::move(j));

        switch_to(then_b);
        stmts(br.body);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
        switch_to(next_b);
    }

    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
}

void Lowerer::lower_while(While& f) {
    int cond = new_block("while.cond");
    int body = new_block("while.body");
    int end = new_block("while.end");
    int orelse = f.orelse.empty() ? end : new_block("while.else");

    // 条件表达式每轮都会产生临时值；在 body 入口和退出路径清理到
    // 循环水位，长循环的 RSS 才不会随迭代数增长。条件不产生堆值时
    // 整条脚手架会被删掉。
    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = st_->is_generator ? kNoReg : rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(loop_mark);
    };

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    size_t pushes_before_cond = st_->rc_pushes;
    VReg c = expr(*f.cond);
    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    st_->loops.push_back({end, cond});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = cond;
        emit(std::move(jj));
    }

    switch_to(orelse);
    clean();
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
    // 循环内（条件或循环体）完全没有临时堆值时，把循环水位整条删掉。
    // 注意不能只看条件：`continue`/`break` 会跳过语句级清理，循环体里
    // 的临时值要靠循环水位兜底。
    if (!st_->is_generator && st_->rc_pushes == pushes_before_cond) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

void Lowerer::lower_for(For& f) {
    if (f.is_async) {
        VReg seq = expr(*f.iter);
        lower_async_for(f, seq, tc_.type_of(f.iter.get()));
        return;
    }
    if (f.iter->kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(*f.iter);
        if (c.func->kind == ExprKind::Name &&
            static_cast<Name&>(*c.func).id == "range" && is_builtin("range")) {
            lower_for_range(f, c);
            return;
        }
    }
    lower_for_seq(f);
}

void Lowerer::lower_for_range(For& f, Call& c) {
    if (f.target->kind != ExprKind::Name) {
        err(f.line, f.col, "for-loop variable must be a name for range()");
        return;
    }
    if (c.args.size() < 1 || c.args.size() > 3) {
        err(c.line, c.col, "range() takes 1 to 3 arguments");
        return;
    }
    if (!c.keywords.empty()) {
        err(c.line, c.col, "range() does not take keyword arguments");
        return;
    }

    VReg start, stop;
    int64_t step = 1;
    if (c.args.size() == 1) {
        start = const_int(0);
        stop = expr(*c.args[0]);
    } else {
        start = expr(*c.args[0]);
        stop = expr(*c.args[1]);
    }
    if (c.args.size() == 3) {
        auto cv = as_const(c.args[2].get());
        if (!cv || (cv->ty->kind != TyKind::Int && cv->ty->kind != TyKind::Bool)) {
            err(c.args[2]->line, c.args[2]->col,
                "range() step must be an integer literal (dynamic steps are not supported yet)");
            return;
        }
        step = cv->i;
        if (step == 0) {
            err(c.args[2]->line, c.args[2]->col, "range() step cannot be zero");
            return;
        }
    }

    // 循环计数器独立于用户变量：Python 里在循环体里给 i 重新赋值
    // 不影响迭代，计数器必须是私有的。
    int ctr = new_temp_slot(t_int());
    store_slot(ctr, start);
    // 生成器里 vreg 不跨 yield 存活：上界必须落槽，每次条件判断重新读。
    int stop_slot = -1;
    if (st_->is_generator) {
        stop_slot = new_temp_slot(t_int());
        store_slot(stop_slot, stop);
    }

    int cond = new_block("for.cond");
    int body = new_block("for.body");
    int incr = new_block("for.incr");
    int end = new_block("for.end");
    int orelse = f.orelse.empty() ? end : new_block("for.else");

    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = st_->is_generator ? kNoReg : rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(loop_mark);
    };
    size_t pushes_before_loop = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    VReg cv = load_slot(ctr, t_int());
    VReg stop_v = stop_slot >= 0 ? load_slot(stop_slot, t_int()) : stop;
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = step > 0 ? CmpKind::Lt : CmpKind::Gt;
    cmp.a = cv;
    cmp.b = stop_v;
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv = load_slot(ctr, t_int());
    assign_to(*f.target, iv, t_int());
    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    VReg iv2 = load_slot(ctr, t_int());
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = iv2;
    add.b = const_int(step);
    VReg iv3 = def(t_int(), std::move(add));
    store_slot(ctr, iv3);
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    clean();
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before_loop) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(incr, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

void Lowerer::lower_for_seq(For& f) {
    VReg seq = expr(*f.iter);
    TypePtr seq_ty = tc_.type_of(f.iter.get());
    if (seq_ty && seq_ty->kind == TyKind::Generator) {
        lower_for_gen(f, seq, seq_ty);
        return;
    }
    if (!seq_ty || (seq_ty->kind != TyKind::List && seq_ty->kind != TyKind::Tuple &&
                    seq_ty->kind != TyKind::Str && seq_ty->kind != TyKind::Dict &&
                    seq_ty->kind != TyKind::Set)) {
        err(f.iter->line, f.iter->col,
            std::format("cannot iterate over '{}' in the code generator", type_name(seq_ty)));
        return;
    }

    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    VReg n = def(t_int(), std::move(ln));
    // 生成器里 vreg 不跨 yield 存活：序列和长度都必须落槽。
    int seq_slot = -1;
    int n_slot = -1;
    if (st_->is_generator) {
        seq_slot = new_temp_slot(seq_ty);
        store_slot(seq_slot, seq);
        n_slot = new_temp_slot(t_int());
        store_slot(n_slot, n);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, seq_ty) : seq; };
    auto n_val = [&]() { return n_slot >= 0 ? load_slot(n_slot, t_int()) : n; };

    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));

    int cond = new_block("for.cond");
    int body = new_block("for.body");
    int incr = new_block("for.incr");
    int end = new_block("for.end");
    int orelse = f.orelse.empty() ? end : new_block("for.else");

    // 水位取在序列求值之后：seq 本身可能是个临时列表，不能被每轮清掉。
    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = st_->is_generator ? kNoReg : rc_mark_at(loop_mark_block, loop_mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(loop_mark);
    };
    size_t pushes_before_loop = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    VReg iv = load_slot(idx, t_int());
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = iv;
    cmp.b = n_val();
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv2 = load_slot(idx, t_int());
    TypePtr et = tc_.type_of(f.target.get());
    if (!et || et->kind == TyKind::Unknown) {
        if (seq_ty->kind == TyKind::Str)
            et = t_str();
        else if (!seq_ty->sub.empty())
            et = seq_ty->sub[0];
    }
    VReg ev;
    VReg sv = seq_val();
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set) {
        // 字典/集合按插入顺序按下标取 key；返回的是容器内部的借用引用。
        ev = call_runtime(RtFn::DictKeyAt, et, {sv, iv2});
    } else {
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = sv;
        get.b = iv2;
        // 遍历字符串时每个下标是 py23_str_char_at 新造的单字节串；
        // 遍历列表/元组时是容器内部的借用引用。
        ev = seq_ty->kind == TyKind::Str ? def_heap(et, std::move(get))
                                         : def(et, std::move(get));
    }
    assign_to(*f.target, ev, et);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    VReg iv3 = load_slot(idx, t_int());
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = iv3;
    add.b = const_int(1);
    VReg iv4 = def(t_int(), std::move(add));
    store_slot(idx, iv4);
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    clean();
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before_loop) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(incr, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

int Lowerer::push_handler() {
    st_->fn->has_handlers = true;
    VReg h = call_runtime(RtFn::HandlerPush, t_int(), {});
    int slot = new_temp_slot(t_int());
    store_slot(slot, h);
    return slot;
}

VReg Lowerer::setjmp_handler(int slot) {
    Inst in;
    in.op = IrOp::Setjmp;
    in.a = load_slot(slot, t_int());
    return def(t_int(), std::move(in));
}

VReg Lowerer::emit_exit_call(const FuncCtx::Cleanup& c, bool exceptional) {
    std::vector<VReg> args;
    for (int i = 0; i < c.nargs; ++i) {
        if (exceptional && i == 0) {
            VReg t = call_runtime(RtFn::ExcTypeName, t_str(), {});
            push_temp(t);
            args.push_back(t);
        } else if (exceptional && i == 1) {
            VReg m = call_runtime(RtFn::ExcMessage, t_str(), {});
            push_temp(m);
            args.push_back(m);
        } else {
            args.push_back(const_none());
        }
    }
    VReg mgr = load_slot(c.mgr_slot, t_unknown());
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = mgr;
    vc.imm = c.exit_slot;
    vc.args = std::move(args);
    TypePtr rt = c.ret ? c.ret : t_none();
    return def_heap(rt, std::move(vc));
}

void Lowerer::emit_cleanup_exits(size_t down_to) {
    // 只发射、不改编译期栈：break/continue/return 可能只在某个分支上，
    // 其它路径的清理仍然有效。顺序必须从内到外。
    for (size_t i = st_->cleanups.size(); i > down_to; --i) {
        const auto& c = st_->cleanups[i - 1];
        switch (c.kind) {
            case FuncCtx::Cleanup::Kind::Handler:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                break;
            case FuncCtx::Cleanup::Kind::With:
                // 先弹 handler 再调 __exit__：__exit__ 自己抛出的异常
                // 应该由外层处理，不能被同一个 with 再抓一次。
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                emit_exit_call(c, false);
                break;
            case FuncCtx::Cleanup::Kind::AsyncWith: {
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                VReg coro = emit_exit_call(c, false);
                // return/break/continue 路径也要把 __aexit__ 跑完。
                suspend_on_awaitable(coro, c.inner_ret, 1, 1, i - 1);
                break;
            }
            case FuncCtx::Cleanup::Kind::Finally:
                call_void(RtFn::HandlerDiscard, {load_slot(c.handler_slot, t_int())});
                if (c.body) stmts(*c.body);
                break;
            case FuncCtx::Cleanup::Kind::ExcDone:
                call_void(RtFn::ExcDone, {});
                break;
        }
    }
}

void Lowerer::lower_with(With& w) {
    if (w.is_async) {
        lower_async_with_item(w, 0);
        return;
    }
    lower_with_item(w, 0);
}

void Lowerer::lower_with_item(With& w, size_t idx) {
    if (idx >= w.items.size()) {
        stmts(w.body);
        return;
    }
    auto& item = w.items[idx];
    TypePtr raw = tc_.type_of(item.context.get());
    TypePtr ct = strip_optional(raw);
    VReg mgr = expr(*item.context);
    if (ct != raw && raw && raw->kind == TyKind::Optional)
        mgr = guard_not_none(mgr, std::format("with item"));
    if (!ct || ct->kind != TyKind::Class || !ct->cls) {
        err(item.context->line, item.context->col,
            "with requires a class instance with __enter__/__exit__");
        lower_with_item(w, idx + 1);
        return;
    }
    auto* en = ct->cls->method("__enter__");
    auto* ex = ct->cls->method("__exit__");
    if (!en || !ex || !en->call_sig || !ex->call_sig) {
        err(item.context->line, item.context->col,
            std::format("'{}' does not define __enter__/__exit__", ct->cls->name));
        lower_with_item(w, idx + 1);
        return;
    }
    int ms = new_temp_slot(ct);
    store_slot(ms, mgr);

    Call enter_call;
    enter_call.line = w.line;
    enter_call.col = w.col;
    std::vector<VReg> eargs = bind_args(enter_call, en->call_sig, en->def);
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = load_slot(ms, ct);
    vc.imm = en->slot;
    vc.args = std::move(eargs);
    TypePtr ert = en->call_sig->ret ? en->call_sig->ret : t_none();
    VReg ev = def_heap(ert, std::move(vc));
    if (item.optional_vars) assign_to(*item.optional_vars, ev, ert);

    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("with.body");
    int land = new_block("with.land");
    int end = new_block("with.end");
    Inst br;
    br.op = IrOp::Branch;
    br.a = sj;
    br.target = land;
    br.target2 = body;
    emit(std::move(br));
    switch_to(body);

    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::With;
    cf.handler_slot = hs;
    cf.land = land;
    cf.mgr_slot = ms;
    cf.exit_slot = ex->slot;
    cf.nargs = static_cast<int>(ex->call_sig->pos.size());
    cf.ret = ex->call_sig->ret ? ex->call_sig->ret : t_none();
    st_->cleanups.push_back(cf);

    lower_with_item(w, idx + 1);

    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        emit_exit_call(cf, false);
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
    }
    st_->cleanups.pop_back();

    switch_to(land);
    // 异常展开：先弹 handler（顺带释放受保护区临时值），再调 __exit__。
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    VReg keep = emit_exit_call(cf, true);
    int suppress = new_block("with.suppress");
    int rethrow = new_block("with.rethrow");
    Inst cb;
    cb.op = IrOp::Branch;
    cb.a = keep;
    cb.target = suppress;
    cb.target2 = rethrow;
    emit(std::move(cb));
    switch_to(suppress);
    call_void(RtFn::ExcClear, {});
    Inst j2;
    j2.op = IrOp::Jump;
    j2.target = end;
    emit(std::move(j2));
    switch_to(rethrow);
    call_void(RtFn::ExcReraise, {});
    end_unreachable();
    switch_to(end);
}

void Lowerer::lower_async_with_item(With& w, size_t idx) {
    if (idx >= w.items.size()) {
        stmts(w.body);
        return;
    }
    if (!st_->is_coroutine) {
        err(w.line, w.col, "'async with' outside async function");
        stmts(w.body);
        return;
    }
    auto& item = w.items[idx];
    TypePtr raw = tc_.type_of(item.context.get());
    TypePtr ct = strip_optional(raw);
    VReg mgr = expr(*item.context);
    if (ct != raw && raw && raw->kind == TyKind::Optional)
        mgr = guard_not_none(mgr, "async with item");
    if (!ct || ct->kind != TyKind::Class || !ct->cls) {
        err(item.context->line, item.context->col,
            "async with requires a class instance with async __aenter__/__aexit__");
        lower_async_with_item(w, idx + 1);
        return;
    }
    auto* en = ct->cls->method("__aenter__");
    auto* ex = ct->cls->method("__aexit__");
    if (!en || !en->call_sig || !en->call_sig->ret ||
        en->call_sig->ret->kind != TyKind::Coroutine) {
        err(item.context->line, item.context->col,
            std::format("'{}' does not define async __aenter__", ct->cls->name));
        lower_async_with_item(w, idx + 1);
        return;
    }
    if (!ex || !ex->call_sig || !ex->call_sig->ret ||
        ex->call_sig->ret->kind != TyKind::Coroutine) {
        err(item.context->line, item.context->col,
            std::format("'{}' does not define async __aexit__", ct->cls->name));
        lower_async_with_item(w, idx + 1);
        return;
    }
    int ms = new_temp_slot(ct);
    store_slot(ms, mgr);

    // __aenter__：调用得到协程，await 出绑定值。
    Call enter_call;
    enter_call.line = w.line;
    enter_call.col = w.col;
    std::vector<VReg> eargs = bind_args(enter_call, en->call_sig, en->def);
    Inst vc;
    vc.op = IrOp::VCall;
    vc.a = load_slot(ms, ct);
    vc.imm = en->slot;
    vc.args = std::move(eargs);
    VReg enter_coro = def_heap(en->call_sig->ret, std::move(vc));
    TypePtr ert = en->call_sig->ret->sub.empty() ? t_unknown() : en->call_sig->ret->sub[0];
    VReg ev = suspend_on_awaitable(enter_coro, ert, item.context->line, item.context->col);
    if (item.optional_vars) assign_to(*item.optional_vars, ev, ert);

    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("awith.body");
    int land = new_block("awith.land");
    int end = new_block("awith.end");
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = sj;
        br.target = land;
        br.target2 = body;
        emit(std::move(br));
    }
    switch_to(body);

    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::AsyncWith;
    cf.handler_slot = hs;
    cf.land = land;
    cf.mgr_slot = ms;
    cf.exit_slot = ex->slot;
    cf.nargs = static_cast<int>(ex->call_sig->pos.size());
    cf.ret = ex->call_sig->ret;
    cf.inner_ret = ex->call_sig->ret->sub.empty() ? t_unknown() : ex->call_sig->ret->sub[0];
    st_->cleanups.push_back(cf);

    lower_async_with_item(w, idx + 1);

    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg coro = emit_exit_call(cf, false);
        suspend_on_awaitable(coro, cf.inner_ret, w.line, w.col);
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    // 异常上下文不能带着 pending 穿过 await，先抄到槽里再清空。
    VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
    push_temp(tname);
    VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
    push_temp(tmsg);
    int ts = new_temp_slot(t_str());
    store_slot(ts, tname);
    int msgs = new_temp_slot(t_str());
    store_slot(msgs, tmsg);
    call_void(RtFn::ExcClear, {});
    {
        std::vector<VReg> args;
        for (int i = 0; i < cf.nargs; ++i) {
            if (i == 0) args.push_back(load_slot(ts, t_str()));
            else if (i == 1) args.push_back(load_slot(msgs, t_str()));
            else args.push_back(const_none());
        }
        Inst vc2;
        vc2.op = IrOp::VCall;
        vc2.a = load_slot(ms, ct);
        vc2.imm = ex->slot;
        vc2.args = std::move(args);
        VReg coro = def_heap(cf.ret, std::move(vc2));
        VReg keep = suspend_on_awaitable(coro, cf.inner_ret, w.line, w.col);
        int suppress = new_block("awith.suppress");
        int rethrow = new_block("awith.rethrow");
        Inst cb;
        cb.op = IrOp::Branch;
        cb.a = keep;
        cb.target = suppress;
        cb.target2 = rethrow;
        emit(std::move(cb));
        switch_to(suppress);
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
        switch_to(rethrow);
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
    }
    switch_to(end);
}

VReg Lowerer::lower_exc_match(Expr& type) {
    auto one = [&](Expr& e) -> VReg {
        if (e.kind == ExprKind::Name) {
            auto& n = static_cast<Name&>(e);
            if (TypeChecker::is_exception_type(n.id))
                return call_runtime(RtFn::ExcMatches, t_bool(), {const_str(n.id)});
        }
        err(e.line, e.col, "except requires a built-in exception name");
        return const_bool(false);
    };
    if (type.kind == ExprKind::Tuple) {
        auto& t = static_cast<TupleExpr&>(type);
        VReg acc = const_bool(false);
        for (auto& el : t.elts) {
            if (!el) continue;
            VReg m = one(*el);
            Inst in;
            in.op = IrOp::Bin;
            in.bop = BinKind::BitOr;
            in.a = acc;
            in.b = m;
            acc = def(t_int(), std::move(in));
        }
        return acc;
    }
    return one(type);
}

void Lowerer::lower_raise(Raise& r) {
    if (r.cause) {
        err(r.line, r.col, "'raise ... from' is not supported by the code generator yet");
        return;
    }
    if (!r.exc) {
        call_void(RtFn::Reraise, {});
        return;
    }
    std::string type;
    VReg msg = kNoReg;
    Expr& e = *r.exc;
    if (e.kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(e);
        if (!TypeChecker::is_exception_type(n.id)) {
            err(e.line, e.col, "raise requires a built-in exception name");
            return;
        }
        type = n.id;
    } else if (e.kind == ExprKind::Call) {
        auto& c = static_cast<Call&>(e);
        if (c.func->kind != ExprKind::Name) {
            err(e.line, e.col, "raise requires a built-in exception name");
            return;
        }
        auto& n = static_cast<Name&>(*c.func);
        if (!TypeChecker::is_exception_type(n.id)) {
            err(e.line, e.col, "raise requires a built-in exception name");
            return;
        }
        if (!c.keywords.empty()) {
            err(e.line, e.col, "exception constructor does not take keyword arguments");
            return;
        }
        if (c.args.size() > 1) {
            err(e.line, e.col, "exception constructor takes at most one message argument");
            return;
        }
        type = n.id;
        if (!c.args.empty()) {
            TypePtr mt = tc_.type_of(c.args[0].get());
            if (mt && (mt->kind == TyKind::List || mt->kind == TyKind::Tuple ||
                       mt->kind == TyKind::Dict || mt->kind == TyKind::Set ||
                       mt->kind == TyKind::Class || mt->kind == TyKind::Func)) {
                err(c.args[0]->line, c.args[0]->col,
                    "exception message must be a string or scalar");
                return;
            }
            VReg raw = expr(*c.args[0]);
            msg = convert_to(raw, mt, t_str());
        }
    } else {
        err(e.line, e.col, "raise requires a built-in exception name");
        return;
    }
    if (msg == kNoReg) msg = const_str("");
    call_void(RtFn::Raise, {const_str(type), msg});
}

ModuleInfoPtr Lowerer::resolve_module_alias(const std::string& name) const {
    // 显式登记的模块别名优先：from pkg import sub 会把 sub 放进别名表，
    // 类型检查的 hoist 也可能留下同名 unknown 变量，不能让它遮住别名。
    if (st_) {
        auto it = st_->modules.find(name);
        if (it != st_->modules.end()) return it->second;
    }
    auto it = module_aliases_.find(name);
    if (it != module_aliases_.end()) return it->second;
    // 真正的局部变量 / 推导式变量仍然遮蔽模块别名。
    if (st_ && st_->slots.count(name)) return nullptr;
    if (comp_slot(name) >= 0) return nullptr;
    return nullptr;
}

void Lowerer::ensure_module_init(const ModuleInfoPtr& mod) {
    if (!mod) return;
    int idx = -1;
    for (size_t i = 0; i < mod_.imports.size(); ++i)
        if (mod_.imports[i] == mod->name) {
            idx = static_cast<int>(i);
            break;
        }
    if (idx < 0) {
        idx = static_cast<int>(mod_.imports.size());
        mod_.imports.push_back(mod->name);
    }
    Inst in;
    in.op = IrOp::ImportModule;
    in.imm = idx;
    emit(std::move(in));
}

int Lowerer::extern_global(const std::string& symbol, const std::string& debug_name, TypePtr ty) {
    auto it = extern_globals_.find(symbol);
    if (it != extern_globals_.end()) {
        auto& g = mod_.globals[it->second];
        if (ty && g.ty && g.ty->kind == TyKind::Unknown) g.ty = ty;
        return it->second;
    }
    IRGlobal g;
    g.name = debug_name;
    g.ty = ty ? ty : t_unknown();
    g.symbol = symbol;
    g.external = true;
    int id = static_cast<int>(mod_.globals.size());
    mod_.globals.push_back(std::move(g));
    extern_globals_[symbol] = id;
    return id;
}

std::optional<int> Lowerer::module_member(Attribute& at, ModuleInfoPtr* owner) {    auto* n = dynamic_cast<Name*>(at.value.get());
    if (!n) return std::nullopt;
    auto mod = resolve_module_alias(n->id);
    if (!mod) return std::nullopt;
    if (owner) *owner = mod;
    auto it = mod->globals.find(at.attr);
    if (it == mod->globals.end()) {
        err(at.line, at.col,
            std::format("module '{}' has no attribute '{}'", n->id, at.attr));
        return std::nullopt;
    }
    if (it->second && it->second->kind == TyKind::Class) {
        err(at.line, at.col, std::format("class '{}' cannot be used as a value", at.attr));
        return std::nullopt;
    }
    return extern_global(module_global_symbol(mod->name, at.attr),
                         std::format("{}.{}", mod->name, at.attr), it->second);
}

VReg Lowerer::construct_imported(const ImportedClass& ic, Call& c, TypePtr cls_ty) {
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = ic.ctor_sig;
    int gi = extern_global(ic.ctor_symbol, ic.ctor_symbol, fty);
    VReg callee = load_global(gi, fty);
    std::vector<VReg> args = bind_args(c, ic.ctor_sig, nullptr);
    Inst in;
    in.op = IrOp::Call;
    in.a = callee;
    in.args = std::move(args);
    return def_heap(cls_ty, std::move(in));
}

std::optional<RtFn> Lowerer::native_fn(const std::string& module, const std::string& member) {
    if (module == "asyncio") {
        // 只为 from-import 登记别名；真正降级走 lower_asyncio。
        if (member == "run") return RtFn::AsyncioRun;
        if (member == "sleep") return RtFn::AsyncioSleep;
        if (member == "gather") return RtFn::AsyncioGather;
        if (member == "create_task") return RtFn::AsyncioCreateTask;
        if (member == "Future") return RtFn::AsyncioFutureNew;
        if (member == "wait_readable" || member == "wait_writable")
            return RtFn::AsyncioWaitFd;
        return std::nullopt;
    }
    if (module == "math") {
        if (member == "sqrt") return RtFn::MathSqrt;
        if (member == "floor") return RtFn::MathFloor;
        if (member == "ceil") return RtFn::MathCeil;
        if (member == "fabs") return RtFn::MathFabs;
        if (member == "sin") return RtFn::MathSin;
        if (member == "cos") return RtFn::MathCos;
        if (member == "tan") return RtFn::MathTan;
        if (member == "log") return RtFn::MathLog;
        if (member == "log2") return RtFn::MathLog2;
        if (member == "log10") return RtFn::MathLog10;
        if (member == "exp") return RtFn::MathExp;
        if (member == "pow") return RtFn::MathPow;
        if (member == "fmod") return RtFn::MathFmod;
    } else if (module == "sys" && member == "exit") {
        return RtFn::SysExit;
    }
    return std::nullopt;
}

bool Lowerer::builtin_value(const std::string& module, const std::string& member, VReg& out,
                            TypePtr& ty) {
    if (module == "math") {
        if (member == "pi") {
            out = const_float(3.14159265358979323846);
            ty = t_float();
            return true;
        }
        if (member == "e") {
            out = const_float(2.71828182845904523536);
            ty = t_float();
            return true;
        }
        if (member == "tau") {
            out = const_float(6.28318530717958647692);
            ty = t_float();
            return true;
        }
    } else if (module == "sys") {
        if (member == "maxsize") {
            out = const_int(std::numeric_limits<int64_t>::max());
            ty = t_int();
            return true;
        }
        if (member == "argv") {
            ty = mk_list(t_str());
            out = call_runtime(RtFn::SysArgv, ty, {});
            return true;
        }
    }
    return false;
}

VReg Lowerer::call_native(const std::string& module, const std::string& member, Call& c,
                          const FuncInfoPtr& info) {
    auto fn = native_fn(module, member);
    if (!fn) {
        err(c.line, c.col, std::format("{}.{} is not a supported native function", module, member));
        return const_none();
    }
    std::vector<VReg> args = bind_args(c, info, nullptr);
    if (*fn == RtFn::SysExit) {
        call_void(*fn, args);
        return const_none();
    }
    TypePtr rt = info && info->ret ? info->ret : t_float();
    return call_runtime(*fn, rt, std::move(args));
}

void Lowerer::lower_import(Import& im) {
    for (auto& al : im.names) {
        if (al.name.find('.') != std::string::npos && al.asname.empty()) {
            err(im.line, im.col, "dotted import needs 'as name' (or use from ... import)");
            continue;
        }
        std::string bind = al.asname.empty() ? al.name : al.asname;
        if (!opts_.module_lookup) {
            err(im.line, im.col, "import requires a multi-module build");
            continue;
        }
        auto mod = opts_.module_lookup(al.name);
        if (!mod) {
            err(im.line, im.col, std::format("cannot resolve module '{}'", al.name));
            continue;
        }
        if (!mod->builtin) ensure_module_init(mod);
        if (st_->is_module_top) module_aliases_[bind] = mod;
        else st_->modules[bind] = mod;
    }
}

void Lowerer::lower_import_from(ImportFrom& im) {
    if (im.level != 0 || im.module.empty()) {
        err(im.line, im.col, "relative imports are not supported yet");
        return;
    }
    if (!opts_.module_lookup) {
        err(im.line, im.col, "import requires a multi-module build");
        return;
    }
    auto mod = opts_.module_lookup(im.module);
    if (!mod) {
        err(im.line, im.col, std::format("cannot resolve module '{}'", im.module));
        return;
    }
    if (!mod->builtin) ensure_module_init(mod);
    if (im.names.size() == 1 && im.names[0].name == "*") {
        for (auto& [name, ty] : mod->globals) {
            if (name.empty() || name[0] == '_') continue;
            if (mod->builtin && native_fn(mod->name, name)) {
                native_aliases_[name] = NativeRef{mod->name, name};
                continue;
            }
            VReg bv;
            TypePtr bty;
            if (mod->builtin && builtin_value(mod->name, name, bv, bty)) {
                bind_name_value(name, bv, bty);
                continue;
            }
            int gi = extern_global(module_global_symbol(mod->name, name),
                                   std::format("{}.{}", mod->name, name), ty);
            VReg v = load_global(gi, ty);
            if (!ty || ty->kind == TyKind::Unknown) rc_inc_unknown(v);
            bind_name_value(name, v, ty);
        }
        for (auto& [name, ic] : mod->classes)
            if (!name.empty() && name[0] != '_') imported_classes_[name] = ic;
        return;
    }
    for (auto& al : im.names) {
        std::string bind = al.asname.empty() ? al.name : al.asname;
        if (mod->builtin) {
            if (native_fn(mod->name, al.name)) {
                native_aliases_[bind] = NativeRef{mod->name, al.name};
                continue;
            }
            VReg bv;
            TypePtr bty;
            if (builtin_value(mod->name, al.name, bv, bty)) {
                bind_name_value(bind, bv, bty);
                continue;
            }
            err(im.line, im.col,
                std::format("module '{}' has no attribute '{}'", im.module, al.name));
            continue;
        }
        if (auto ci = mod->classes.find(al.name); ci != mod->classes.end()) {
            imported_classes_[bind] = ci->second;
            continue;
        }
        auto it = mod->globals.find(al.name);
        if (it == mod->globals.end()) {
            if (auto child = opts_.module_lookup(im.module + "." + al.name)) {
                if (st_->is_module_top) module_aliases_[bind] = child;
                else st_->modules[bind] = child;
                continue;
            }
            err(im.line, im.col,
                std::format("module '{}' has no attribute '{}'", im.module, al.name));
            continue;
        }
        int gi = extern_global(module_global_symbol(mod->name, al.name),
                               std::format("{}.{}", mod->name, al.name), it->second);
        VReg v = load_global(gi, it->second);
        // 快照绑定：本地/全局槽持有一份引用。
        if (!it->second || it->second->kind == TyKind::Unknown) rc_inc_unknown(v);
        bind_name_value(bind, v, it->second);
    }
}

void Lowerer::lower_try(Try& t) {
    if (!t.finalbody.empty()) {
        lower_finally(t);
        return;
    }
    lower_try_core(t);
}

void Lowerer::lower_try_core(Try& t) {
    if (t.handlers.empty()) {
        stmts(t.body);
        stmts(t.orelse);
        return;
    }

    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("try.body");
    int land = new_block("try.land");
    int end = new_block("try.end");
    int else_b = t.orelse.empty() ? end : new_block("try.else");
    Inst br;
    br.op = IrOp::Branch;
    br.a = sj;
    br.target = land;
    br.target2 = body;
    emit(std::move(br));

    switch_to(body);
    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::Handler;
    cf.handler_slot = hs;
    cf.land = land;
    st_->cleanups.push_back(cf);
    stmts(t.body);
    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        Inst j;
        j.op = IrOp::Jump;
        j.target = else_b;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    struct HBody {
        ExceptHandler* h;
        int block;
    };
    std::vector<HBody> bodies;
    bool bare = false;
    for (auto& h : t.handlers) {
        int hb = new_block("except.body");
        bodies.push_back({&h, hb});
        if (bare) continue;
        if (h.type) {
            VReg m = lower_exc_match(*h.type);
            int next = new_block("except.next");
            Inst cb;
            cb.op = IrOp::Branch;
            cb.a = m;
            cb.target = hb;
            cb.target2 = next;
            emit(std::move(cb));
            switch_to(next);
        } else {
            bare = true;
            Inst j;
            j.op = IrOp::Jump;
            j.target = hb;
            emit(std::move(j));
        }
    }
    if (!bare) {
        call_void(RtFn::ExcReraise, {});
        end_unreachable();
    }

    for (auto& hb : bodies) {
        switch_to(hb.block);
        VReg msg = call_runtime(RtFn::ExcAccept, t_str(), {});
        push_temp(msg);
        int shadow = -1;
        if (!hb.h->name.empty()) {
            shadow = st_->fn->new_slot(t_str());
            comp_slots_.emplace_back();
            comp_slots_.back()[hb.h->name] = shadow;
            store_slot(shadow, msg);
        }

        // except 体再抛异常时，也要先 ExcDone 再继续展开，避免
        // current 异常上下文和 save 节点累积。
        int hh = push_handler();
        VReg sj2 = setjmp_handler(hh);
        int run = new_block("except.run");
        int hland = new_block("except.land");
        int hend = new_block("except.end");
        Inst b2;
        b2.op = IrOp::Branch;
        b2.a = sj2;
        b2.target = hland;
        b2.target2 = run;
        emit(std::move(b2));
        switch_to(run);

        FuncCtx::Cleanup hc;
        hc.kind = FuncCtx::Cleanup::Kind::Handler;
        hc.handler_slot = hh;
        st_->cleanups.push_back(hc);
        FuncCtx::Cleanup done;
        done.kind = FuncCtx::Cleanup::Kind::ExcDone;
        st_->cleanups.push_back(done);
        stmts(hb.h->body);
        st_->cleanups.pop_back();
        st_->cleanups.pop_back();
        if (shadow >= 0) comp_slots_.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hh, t_int())});
            call_void(RtFn::ExcDone, {});
            Inst j;
            j.op = IrOp::Jump;
            j.target = hend;
            emit(std::move(j));
        }

        switch_to(hland);
        call_void(RtFn::HandlerLeave, {load_slot(hh, t_int())});
        call_void(RtFn::ExcDone, {});
        call_void(RtFn::ExcReraise, {});
        end_unreachable();
        switch_to(hend);
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
    }

    if (else_b != end) {
        switch_to(else_b);
        stmts(t.orelse);
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
    }
    switch_to(end);
}

/* finally 体里如果含跳出这个 finally 的 return/break/continue，目前
   没有复制控制流的机制，直接拒绝；循环内部的 break/continue 是合法的。 */
static bool outer_control(const std::vector<StmtPtr>& body, int loop_depth) {
    for (auto& s : body) {
        switch (s->kind) {
            case StmtKind::Return: return true;
            case StmtKind::Break:
            case StmtKind::Continue: return loop_depth == 0;
            case StmtKind::If: {
                auto& f = static_cast<If&>(*s);
                for (auto& b : f.branches)
                    if (outer_control(b.body, loop_depth)) return true;
                if (outer_control(f.orelse, loop_depth)) return true;
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(*s);
                if (outer_control(w.body, loop_depth + 1)) return true;
                if (outer_control(w.orelse, loop_depth)) return true;
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(*s);
                if (outer_control(f.body, loop_depth + 1)) return true;
                if (outer_control(f.orelse, loop_depth)) return true;
                break;
            }
            case StmtKind::With: {
                auto& w = static_cast<With&>(*s);
                if (outer_control(w.body, loop_depth)) return true;
                break;
            }
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(*s);
                if (outer_control(t.body, loop_depth)) return true;
                for (auto& h : t.handlers)
                    if (outer_control(h.body, loop_depth)) return true;
                if (outer_control(t.orelse, loop_depth)) return true;
                if (outer_control(t.finalbody, loop_depth)) return true;
                break;
            }
            default: break;
        }
    }
    return false;
}

void Lowerer::lower_finally(Try& t) {
    if (outer_control(t.finalbody, 0)) {
        err(t.line, t.col,
            "return/break/continue inside finally is not supported by the code generator yet");
        return;
    }
    int hs = push_handler();
    VReg sj = setjmp_handler(hs);
    int body = new_block("finally.body");
    int land = new_block("finally.land");
    int end = new_block("finally.end");
    Inst br;
    br.op = IrOp::Branch;
    br.a = sj;
    br.target = land;
    br.target2 = body;
    emit(std::move(br));

    switch_to(body);
    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::Finally;
    cf.handler_slot = hs;
    cf.land = land;
    cf.body = &t.finalbody;
    st_->cleanups.push_back(cf);
    lower_try_core(t);
    st_->cleanups.pop_back();
    if (!terminated()) {
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        ++finally_body_depth_;
        stmts(t.finalbody);
        --finally_body_depth_;
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
    ++finally_body_depth_;
    stmts(t.finalbody);
    --finally_body_depth_;
    call_void(RtFn::ExcReraise, {});
    end_unreachable();
    switch_to(end);
}

int Lowerer::compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label) {
    if (f.is_async) {
        int kind = (info && info->is_async_generator) ? 2 : 1;
        return compile_suspendable_function(f, info, label, kind);
    }
    if (info && info->is_generator) return compile_suspendable_function(f, info, label, 0);

    IRFunction* irf = mod_.add_func(label);
    int fidx = static_cast<int>(mod_.funcs.size()) - 1;

    TypePtr ret = t_none();
    if (info && info->ret) ret = info->ret;
    else if (info) ret = t_unknown();
    irf->ret = ret;

    const Scope* scope = tc_.scope_of(&f);
    push_frame(irf, scope);
    if (auto it = closure_meta_.find(&f); it != closure_meta_.end()) st_->closure = &it->second;

    int entry = new_block("entry");
    switch_to(entry);

    const ClosureMeta* meta = st_->closure;

    // 被内层捕获的变量不占普通值槽，槽里放的是 cell（单元素 list[T]）。
    // 形参例外：IRParam 需要原始值槽接实参，入口再 retain 进 cell。
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = irf->new_slot(T);
        irf->params.push_back(IRParam{p.name, T, s});
        if (meta && meta->cell_index.count(p.name)) {
            int cs = irf->new_slot(mk_list(T));
            st_->cell_slots[p.name] = cs;
            st_->param_slots[p.name] = s;
            st_->slots[p.name] = cs;
        } else {
            st_->slots[p.name] = s;
        }
    };
    if (info) {
        for (auto& p : info->pos) add_param(p);
        for (auto& p : info->kwonly) add_param(p);
        if (info->varargs) {
            TypePtr et = info->vararg_ty ? info->vararg_ty : t_unknown();
            Param p;
            p.name = info->vararg_name;
            p.ty = mk_list(et);
            add_param(p);
        }
        if (info->kwargs) {
            TypePtr vt = info->kwarg_ty ? info->kwarg_ty : t_unknown();
            Param p;
            p.name = info->kwarg_name;
            p.ty = mk_dict(t_str(), vt);
            add_param(p);
        }
    }

    // 其余局部名按符号表分配。符号表是 typecheck 的最终结果，
    // 这里不再自己推断，避免两套规则分叉。
    if (scope) {
        for (auto& [name, sym] : scope->vars) {
            if (st_->slots.count(name)) continue;
            if (st_->globals_decl.count(name)) continue;
            TypePtr T = sym.ty ? sym.ty : t_unknown();
            if (meta && meta->cell_index.count(name)) {
                int cs = irf->new_slot(mk_list(T));
                st_->cell_slots[name] = cs;
                st_->slots[name] = cs;
            } else {
                st_->slots[name] = irf->new_slot(T);
            }
        }
    }

    // 闭包环境指针由 codegen 在入口从 %r10 存进这个槽；只是借用，
    // 不参与引用计数，出口也不释放。
    if (meta && !meta->env.empty()) {
        int es = irf->new_slot(t_unknown());
        st_->env_slot = es;
        irf->env_slot = es;
        st_->borrow_slots.insert(es);
    }

    // 嵌套函数里的自引用：如果它需要闭包（有其它捕获），env_slot 里就是
    // 自己；没有捕获则是静态闭包。两种情况都不需要 self cell。
    if (auto pit = func_parent_.find(&f); pit != func_parent_.end() && pit->second) {
        st_->self_def = &f;
        st_->self_idx = fidx;
        st_->self_type = new_ty(TyKind::Func);
        st_->self_type->fn = info;
    }

    // 形参按值借用，入口统一 retain 一份归形参槽所有；出口 release_slots
    // 负责释放。这样函数体内覆盖形参时也能按普通槽的规则 dec 旧值。
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);
    inc_params();
    create_cells();

    stmts(f.body);
    if (!terminated()) emit_return(kNoReg, nullptr);
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);

    pop_frame();
    return fidx;
}

// ────────────────────────── 生成器 / 协程 ──────────────────────────

void Lowerer::cleanup_gen_mark() {
    if (st_->gen_mark_slot < 0) return;
    rc_cleanup(load_slot(st_->gen_mark_slot, t_int()));
}

void Lowerer::gen_finish(VReg v, TypePtr vty) {
    // 协程的返回值放进 out，由事件循环取走；生成器忽略。
    if (v != kNoReg && ir_is_heap(vty)) rc_inc(v);
    cleanup_gen_mark();
    release_slots();
    Inst d;
    d.op = IrOp::GenDone;
    d.a = v;
    emit(std::move(d));
}

VReg Lowerer::lower_yield(Expr& y) {
    if (!st_->is_generator) {
        err(y.line, y.col, "'yield' outside generator");
        return const_none();
    }
    if (yield_ctx_node_ != &y) {
        err(y.line, y.col,
            "'yield' is only supported as a statement or assignment value in this version");
    }
    if (finally_body_depth_ > 0)
        err(y.line, y.col, "'yield' inside finally is not supported yet");
    // except 体里的 yield 需要跨挂起保存异常上下文，暂不支持。
    for (auto& c : st_->cleanups) {
        if (c.kind == FuncCtx::Cleanup::Kind::ExcDone) {
            err(y.line, y.col, "'yield' inside except is not supported yet");
            break;
        }
    }    auto& yn = static_cast<Yield&>(y);
    VReg v = yn.value ? expr(*yn.value) : const_none();
    TypePtr vt = yn.value ? tc_.type_of(yn.value.get()) : t_none();
    return emit_yield(v, vt);
}

VReg Lowerer::emit_yield(VReg v, TypePtr vt) {
    // 帧里的 out 槽接管一个引用；未知类型按注册表保守 retain。
    if (ir_is_heap(vt)) rc_inc(v);
    else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
    // 挂起前把本次 resume 的临时值清干净：临时栈是全局的，不清会把
    // 调用方的临时值连同本帧的一起留给下一次 resume。
    cleanup_gen_mark();
    // handler 记录不能留在全局栈上：resume 帧已经返回，longjmp 进去
    // 就是野跳。从内到外摘下来，恢复时按原顺序重挂并重新 setjmp。
    for (size_t i = st_->cleanups.size(); i > 0; --i) {
        const auto& c = st_->cleanups[i - 1];
        if (c.kind == FuncCtx::Cleanup::Kind::With ||
            c.kind == FuncCtx::Cleanup::Kind::AsyncWith ||
            c.kind == FuncCtx::Cleanup::Kind::Finally ||
            c.kind == FuncCtx::Cleanup::Kind::Handler)
            call_void(RtFn::HandlerSuspend, {load_slot(c.handler_slot, t_int())});
    }
    int id = st_->gen_next_state++;
    int cont = new_block("g.resume");
    FuncCtx::GenState gs;
    gs.id = id;
    gs.cont = cont;
    gs.cleanups = st_->cleanups;  // close 时需要按这份快照跑清理
    st_->gen_states.push_back(std::move(gs));
    Inst gy;
    gy.op = IrOp::GenYield;
    gy.a = v;
    gy.imm = id;
    emit(std::move(gy));
    switch_to(cont);
    // yield 表达式的值是 send() 传进来的（普通 next 是 None），静态不可知。
    Inst gs2;
    gs2.op = IrOp::GenSent;
    return def(t_unknown(), std::move(gs2));
}

// yield from：把子迭代器的值逐个转 yield 出去；子生成器额外支持
// send 转发。close 时外层帧释放槽位会把子生成器 finalize。
VReg Lowerer::lower_yield_from(YieldFrom& y) {
    if (!st_->is_generator) {
        err(y.line, y.col, "'yield from' outside generator");
        return const_none();
    }
    if (st_->is_coroutine) {
        err(y.line, y.col, "'yield from' inside an async function is not supported");
        expr(*y.value);
        return const_none();
    }
    if (finally_body_depth_ > 0)
        err(y.line, y.col, "'yield from' inside finally is not supported yet");
    for (auto& c : st_->cleanups) {
        if (c.kind == FuncCtx::Cleanup::Kind::ExcDone) {
            err(y.line, y.col, "'yield from' inside except is not supported yet");
            break;
        }
    }
    TypePtr vt = tc_.type_of(y.value.get());
    VReg sub = expr(*y.value);
    Inst cn;
    cn.op = IrOp::ConstNone;
    VReg result = def(t_unknown(), std::move(cn));

    auto is_seq = [&](const TypePtr& t) {
        return t && (t->kind == TyKind::List || t->kind == TyKind::Tuple ||
                     t->kind == TyKind::Str || t->kind == TyKind::Dict ||
                     t->kind == TyKind::Set);
    };

    if (!vt || vt->kind == TyKind::Unknown || vt->kind == TyKind::Generator) {
        TypePtr styp = vt ? vt : t_unknown();
        TypePtr et = t_unknown();
        if (vt && vt->kind == TyKind::Generator && !vt->sub.empty()) et = vt->sub[0];
        int sub_slot = new_temp_slot(styp);
        store_slot(sub_slot, sub);
        int ok_slot = new_temp_slot(t_int());
        store_slot(ok_slot, const_int(0));

        int cond = new_block("yfrom.cond");
        int body = new_block("yfrom.body");
        int end = new_block("yfrom.end");
        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));

        switch_to(cond);
        // 恢复后 p[2] 是调用方 send 进来的值；第一次 resume 时它是初始
        // 值（next 为 None），直接转发给子生成器。
        Inst se;
        se.op = IrOp::GenSent;
        VReg sent = def(t_unknown(), std::move(se));
        Inst sa;
        sa.op = IrOp::SlotAddr;
        sa.idx = ok_slot;
        VReg okp = def(t_unknown(), std::move(sa));
        Inst call;
        call.op = IrOp::CallRt;
        call.imm = static_cast<int64_t>(RtFn::GenSendOpt);
        call.args = {load_slot(sub_slot, styp), sent, okp};
        VReg ev = def(t_unknown(), std::move(call));
        if (ir_is_heap(et)) push_temp(ev);
        VReg ok = load_slot(ok_slot, t_int());
        Inst br;
        br.op = IrOp::Branch;
        br.a = ok;
        br.target = body;
        br.target2 = end;
        emit(std::move(br));

        switch_to(body);
        emit_yield(ev, et);
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));
        switch_to(end);
        return result;
    }

    if (!is_seq(vt)) {
        err(y.value->line, y.value->col,
            std::format("cannot 'yield from' '{}'", type_name(vt)));
        return result;
    }

    TypePtr et = t_unknown();
    if (vt->kind == TyKind::Str) et = t_str();
    else if (!vt->sub.empty()) et = vt->sub[0];

    int seq_slot = new_temp_slot(vt);
    store_slot(seq_slot, sub);
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = sub;
    VReg n = def(t_int(), std::move(ln));
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, n);
    int idx_slot = new_temp_slot(t_int());
    store_slot(idx_slot, const_int(0));

    int cond = new_block("yfrom.seq.cond");
    int body = new_block("yfrom.seq.body");
    int incr = new_block("yfrom.seq.incr");
    int end = new_block("yfrom.seq.end");
    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    VReg iv = load_slot(idx_slot, t_int());
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = iv;
    cmp.b = load_slot(n_slot, t_int());
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    cleanup_gen_mark();
    VReg sv = load_slot(seq_slot, vt);
    VReg iv2 = load_slot(idx_slot, t_int());
    VReg ev;
    if (vt->kind == TyKind::Dict || vt->kind == TyKind::Set) {
        ev = call_runtime(RtFn::DictKeyAt, et, {sv, iv2});
    } else {
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = sv;
        get.b = iv2;
        ev = vt->kind == TyKind::Str ? def_heap(et, std::move(get))
                                     : def(et, std::move(get));
    }
    emit_yield(ev, et);
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = incr;
    emit(std::move(jb));

    switch_to(incr);
    cleanup_gen_mark();
    VReg iv3 = load_slot(idx_slot, t_int());
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = iv3;
    add.b = const_int(1);
    VReg iv4 = def(t_int(), std::move(add));
    store_slot(idx_slot, iv4);
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(end);
    return result;
}

VReg Lowerer::suspend_on_awaitable(VReg w, TypePtr result_ty, int line, int col,
                                   size_t active_limit) {
    (void)line;
    (void)col;
    TypePtr vt = vreg_ty(w);
    // 把 awaitable 的所有权交给 out，事件循环接管。
    if (ir_is_heap(vt)) rc_inc(w);
    else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(w);
    cleanup_gen_mark();
    // handler 记录和 yield 一样要在挂起时摘下来。
    size_t n_active = std::min(st_->cleanups.size(), active_limit);
    for (size_t i = n_active; i > 0; --i) {
        const auto& c = st_->cleanups[i - 1];
        if (c.kind == FuncCtx::Cleanup::Kind::With ||
            c.kind == FuncCtx::Cleanup::Kind::AsyncWith ||
            c.kind == FuncCtx::Cleanup::Kind::Finally ||
            c.kind == FuncCtx::Cleanup::Kind::Handler)
            call_void(RtFn::HandlerSuspend, {load_slot(c.handler_slot, t_int())});
    }
    int id = st_->gen_next_state++;
    int cont = new_block("g.await");
    FuncCtx::GenState gs;
    gs.id = id;
    gs.cont = cont;
    gs.cleanups.assign(st_->cleanups.begin(),
                       st_->cleanups.begin() + static_cast<std::ptrdiff_t>(n_active));
    st_->gen_states.push_back(std::move(gs));
    Inst ga;
    ga.op = IrOp::GenAwait;
    ga.a = w;
    ga.imm = id;
    emit(std::move(ga));
    switch_to(cont);
    // 恢复时 sent 里是 await 的结果（或由 throw 分发注入异常）。
    TypePtr at = result_ty;
    Inst gs2;
    gs2.op = IrOp::GenSent;
    VReg sent = def(at ? at : t_unknown(), std::move(gs2));
    // 事件循环把结果的引用转移给这里；堆结果交给临时栈，语句末释放。
    if (ir_is_heap(at)) push_temp(sent);
    return sent;
}

VReg Lowerer::lower_await(Expr& e) {
    if (!st_->is_coroutine) {
        err(e.line, e.col, "'await' outside async function");
        return const_none();
    }
    if (await_ctx_node_ != &e) {
        err(e.line, e.col,
            "'await' is only supported as a statement, assignment value or return value "
            "in this version");
    }
    if (finally_body_depth_ > 0)
        err(e.line, e.col, "'await' inside finally is not supported yet");
    for (auto& c : st_->cleanups) {
        if (c.kind == FuncCtx::Cleanup::Kind::ExcDone) {
            err(e.line, e.col, "'await' inside except is not supported yet");
            break;
        }
    }
    auto& aw = static_cast<Await&>(e);
    VReg v = expr(*aw.value);
    return suspend_on_awaitable(v, tc_.type_of(&e), e.line, e.col);
}

void Lowerer::lower_for_gen(For& f, VReg seq, TypePtr seq_ty) {
    TypePtr et = seq_ty->sub.empty() ? t_unknown() : seq_ty->sub[0];
    // 生成器里 vreg 不跨 yield：序列落槽，每次重新读。
    int seq_slot = -1;
    if (st_->is_generator) {
        seq_slot = new_temp_slot(seq_ty);
        store_slot(seq_slot, seq);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, seq_ty) : seq; };

    int ok_slot = new_temp_slot(t_int());
    store_slot(ok_slot, const_int(0));

    int cond = new_block("for.gen.cond");
    int body = new_block("for.gen.body");
    int end = new_block("for.gen.end");
    int orelse = f.orelse.empty() ? end : new_block("for.gen.else");

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    Inst sa;
    sa.op = IrOp::SlotAddr;
    sa.idx = ok_slot;
    VReg okp = def(t_unknown(), std::move(sa));
    Inst call;
    call.op = IrOp::CallRt;
    call.imm = static_cast<int64_t>(RtFn::GenNextOpt);
    call.args = {seq_val(), okp};
    // 返回的是机器字（浮点也是位模式），vreg 类型只能记 unknown。
    VReg ev = def(t_unknown(), std::move(call));
    ev = retype(ev, et);
    if (ir_is_heap(et)) push_temp(ev);
    VReg ok = load_slot(ok_slot, t_int());
    Inst br;
    br.op = IrOp::Branch;
    br.a = ok;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    assign_to(*f.target, ev, et);
    st_->loops.push_back({end, cond});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = cond;
        emit(std::move(jj));
    }

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = end;
            emit(std::move(jj));
        }
    }
    switch_to(end);
}

void Lowerer::lower_async_for(For& f, VReg seq, TypePtr seq_ty) {
    TypePtr et = t_unknown();
    if (seq_ty && seq_ty->kind == TyKind::AsyncGenerator) {
        et = seq_ty->sub.empty() ? t_unknown() : seq_ty->sub[0];
    } else if (seq_ty && seq_ty->kind != TyKind::Unknown) {
        err(f.iter->line, f.iter->col,
            std::format("async for requires an async generator, got '{}'",
                        type_name(seq_ty)));
        return;
    }
    // 挂起会丢 vreg：序列落帧槽。
    int seq_slot = -1;
    TypePtr sst = seq_ty ? seq_ty : t_unknown();
    if (st_->is_generator) {
        seq_slot = new_temp_slot(sst);
        store_slot(seq_slot, seq);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sst) : seq; };

    int hs_slot = new_temp_slot(t_int());
    int next_try = new_block("afor.next");
    int fetch = new_block("afor.fetch");
    int land = new_block("afor.land");
    int reraise_b = new_block("afor.reraise");
    int body = new_block("afor.body");
    int end = new_block("afor.end");
    int orelse = f.orelse.empty() ? end : new_block("afor.else");

    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = next_try;
        emit(std::move(j));
    }

    switch_to(next_try);
    VReg h = call_runtime(RtFn::HandlerPush, t_int(), {});
    store_slot(hs_slot, h);
    VReg sj = setjmp_handler(hs_slot);
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = sj;
        br.target = land;
        br.target2 = fetch;
        emit(std::move(br));
    }

    // 这一层 handler 保护「__anext__ + await」，异常（含 StopAsyncIteration）
    // 由 landing 分派；挂起时它和普通 try 一样会被摘下/重挂。
    FuncCtx::Cleanup cf;
    cf.kind = FuncCtx::Cleanup::Kind::Handler;
    cf.handler_slot = hs_slot;
    cf.land = land;
    st_->cleanups.push_back(cf);

    switch_to(fetch);
    Inst call;
    call.op = IrOp::CallRt;
    call.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
    call.args = {seq_val()};
    VReg w = def_heap(mk_coroutine(et), std::move(call));
    VReg val = suspend_on_awaitable(w, et, f.line, f.col);

    st_->cleanups.pop_back();
    call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
    assign_to(*f.target, val, et);
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = body;
        emit(std::move(j));
    }

    switch_to(land);
    call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
    VReg m = call_runtime(RtFn::ExcMatches, t_bool(), {const_str("StopAsyncIteration")});
    int stop_ok = new_block("afor.stop");
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = m;
        br.target = stop_ok;
        br.target2 = reraise_b;
        emit(std::move(br));
    }
    switch_to(reraise_b);
    call_void(RtFn::ExcReraise, {});
    end_unreachable();
    switch_to(stop_ok);
    call_void(RtFn::ExcClear, {});
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = orelse;
        emit(std::move(j));
    }

    switch_to(body);
    st_->loops.push_back({end, next_try});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst j;
        j.op = IrOp::Jump;
        j.target = next_try;
        emit(std::move(j));
    }

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
    }
    switch_to(end);
}

// 生成器/协程 = resume 状态机 + 创建包装。
//   resume: 槽位在堆帧里，入口按 state 分发到续跑块；
//   包装:   普通函数，按原签名收参，分配帧、把实参 retain 进帧槽、
//           绑定闭包环境，返回生成器/协程对象。函数值指向包装。
int Lowerer::compile_suspendable_function(FunctionDef& f, FuncInfoPtr info,
                                          const std::string& label, int frame_kind) {
    bool is_coro = frame_kind != 0;   // 协程和异步生成器都能 await
    bool is_agen = frame_kind == 2;
    IRFunction* irf = mod_.add_func(label + "$resume");
    int ridx = static_cast<int>(mod_.funcs.size()) - 1;
    IRFunction* cw = mod_.add_func(label);
    int cidx = static_cast<int>(mod_.funcs.size()) - 1;

    irf->ret = t_none();
    irf->is_generator = true;
    TypePtr gen_ty = (info && info->ret)
                         ? info->ret
                         : (is_agen ? mk_async_generator(t_unknown())
                                    : (is_coro ? mk_coroutine(t_unknown())
                                               : mk_generator(t_unknown())));
    cw->ret = gen_ty;

    const Scope* scope = tc_.scope_of(&f);
    push_frame(irf, scope);
    if (auto it = closure_meta_.find(&f); it != closure_meta_.end()) st_->closure = &it->second;
    st_->is_generator = true;
    st_->is_coroutine = is_coro;
    st_->is_async_generator = is_agen;
    st_->async_result = (is_coro && !is_agen && info) ? info->async_result : nullptr;
    const ClosureMeta* meta = st_->closure;

    int entry = new_block("g.entry");
    int start = new_block("g.start");
    switch_to(start);

    // 形参：原值放普通槽；被内层捕获的参数另外有一个 cell 槽，
    // create_cells 在第一次启动时建 cell。
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = irf->new_slot(T);
        irf->params.push_back(IRParam{p.name, T, s});
        if (meta && meta->cell_index.count(p.name)) {
            int cs = irf->new_slot(mk_list(T));
            st_->cell_slots[p.name] = cs;
            st_->param_slots[p.name] = s;
            st_->slots[p.name] = cs;
        } else {
            st_->slots[p.name] = s;
        }
    };
    if (info) {
        for (auto& p : info->pos) add_param(p);
        for (auto& p : info->kwonly) add_param(p);
        if (info->varargs) {
            TypePtr et = info->vararg_ty ? info->vararg_ty : t_unknown();
            Param p;
            p.name = info->vararg_name;
            p.ty = mk_list(et);
            add_param(p);
        }
        if (info->kwargs) {
            TypePtr vt = info->kwarg_ty ? info->kwarg_ty : t_unknown();
            Param p;
            p.name = info->kwarg_name;
            p.ty = mk_dict(t_str(), vt);
            add_param(p);
        }
    }
    if (scope) {
        for (auto& [name, sym] : scope->vars) {
            if (st_->slots.count(name)) continue;
            if (st_->globals_decl.count(name)) continue;
            TypePtr T = sym.ty ? sym.ty : t_unknown();
            if (meta && meta->cell_index.count(name)) {
                int cs = irf->new_slot(mk_list(T));
                st_->cell_slots[name] = cs;
                st_->slots[name] = cs;
            } else {
                st_->slots[name] = irf->new_slot(T);
            }
        }
    }

    // 闭包环境是帧里的一个普通堆槽：创建时 retain 闭包，帧销毁时 dec。
    TypePtr env_ty = new_ty(TyKind::Func);
    int env_slot = -1;
    if (meta && !meta->env.empty()) {
        env_slot = irf->new_slot(env_ty);
        st_->env_slot = env_slot;
    }
    irf->env_slot = -1;  // %r10 只在创建包装里用

    if (auto pit = func_parent_.find(&f); pit != func_parent_.end() && pit->second) {
        st_->self_def = &f;
        st_->self_idx = cidx;
        st_->self_type = new_ty(TyKind::Func);
        st_->self_type->fn = info;
    }

    // 临时值水位每次 resume 重新记录（vreg 不跨 yield）。
    int mark_slot = irf->new_slot(t_int());
    st_->gen_mark_slot = mark_slot;
    st_->fn_mark = kNoReg;

    // 创建包装已经 retain 过实参，这里不再 inc_params。
    create_cells();
    stmts(f.body);
    if (!terminated()) gen_finish();

    // ── close 路径：按挂起点快照跑 finally / __exit__，再释放帧槽 ──
    auto has_runtime_handler = [](const FuncCtx::Cleanup& c) {
        return c.kind == FuncCtx::Cleanup::Kind::Handler ||
               c.kind == FuncCtx::Cleanup::Kind::With ||
               c.kind == FuncCtx::Cleanup::Kind::AsyncWith ||
               c.kind == FuncCtx::Cleanup::Kind::Finally;
    };
    std::vector<std::pair<int, int>> close_targets;
    for (auto& gs : st_->gen_states) {
        // 只要挂起点有 handler 记录，就要一条 close 块：即使没有 with/finally
        // 动作，也必须把摘下来的记录 free 掉。
        bool has_action = false;
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c)) has_action = true;
        if (!has_action) continue;
        int cb = new_block("g.close");
        switch_to(cb);
        for (size_t i = gs.cleanups.size(); i > 0; --i) {
            const auto& c = gs.cleanups[i - 1];
            if (c.kind == FuncCtx::Cleanup::Kind::With) {
                emit_exit_call(c, false);
            } else if (c.kind == FuncCtx::Cleanup::Kind::Finally && c.body) {
                ++finally_body_depth_;
                stmts(*c.body);
                --finally_body_depth_;
            }
        }
        // 挂起时摘下来的 handler 记录不会有人再弹栈，close 路径负责释放。
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c))
                call_void(RtFn::HandlerFree, {load_slot(c.handler_slot, t_int())});
        cleanup_gen_mark();
        release_slots();
        Inst d;
        d.op = IrOp::GenDone;
        emit(std::move(d));
        close_targets.push_back({gs.id, cb});
    }
    int close_default = new_block("g.close.default");
    switch_to(close_default);
    cleanup_gen_mark();
    release_slots();
    {
        Inst d;
        d.op = IrOp::GenDone;
        emit(std::move(d));
    }

    // ── 入口分发 ────────────────────────────────────────────────
    // 每次 resume 先取临时栈水位；close 走清理分发，否则按 state 恢复。
    // try/with/finally 的 handler 在 yield 时被摘掉，恢复时必须按原顺序
    // 重新压栈并重新 setjmp（jmp_buf 属于已经返回的旧 resume 帧）。
    switch_to(entry);
    VReg m0 = call_runtime(RtFn::RcMark, t_int(), {});
    store_slot(mark_slot, m0);
    Inst gsit;
    gsit.op = IrOp::GenGetState;
    VReg state = def(t_int(), std::move(gsit));
    Inst gcl;
    gcl.op = IrOp::GenClosing;
    VReg closing = def(t_int(), std::move(gcl));
    int close_disp = new_block("g.close.dispatch");
    int normal_disp = new_block("g.dispatch");
    int throw_disp = is_coro ? new_block("g.throw.dispatch") : -1;
    int after_close = new_block("g.not.closing");
    {
        Inst br;
        br.op = IrOp::Branch;
        br.a = closing;
        br.target = close_disp;
        br.target2 = after_close;
        emit(std::move(br));
    }
    switch_to(after_close);
    if (is_coro) {
        Inst ge;
        ge.op = IrOp::GenGetExc;
        VReg exc = def(t_int(), std::move(ge));
        Inst br;
        br.op = IrOp::Branch;
        br.a = exc;
        br.target = throw_disp;
        br.target2 = normal_disp;
        emit(std::move(br));
    } else {
        Inst j;
        j.op = IrOp::Jump;
        j.target = normal_disp;
        emit(std::move(j));
    }

    int cur = normal_disp;
    for (auto& gs : st_->gen_states) {
        switch_to(cur);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Eq;
        cmp.a = state;
        cmp.b = const_int(gs.id);
        VReg cc = def(t_bool(), std::move(cmp));
        std::vector<const FuncCtx::Cleanup*> hs;
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c)) hs.push_back(&c);
        int target = gs.cont;
        if (!hs.empty()) target = new_block("g.arm");
        int nextb = new_block("g.disp");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = target;
        br.target2 = nextb;
        emit(std::move(br));
        if (!hs.empty()) {
            switch_to(target);
            for (const auto* c : hs) {
                call_void(RtFn::HandlerRearm, {load_slot(c->handler_slot, t_int())});
                VReg sj = setjmp_handler(c->handler_slot);
                int nb = new_block("g.arm.next");
                Inst ab;
                ab.op = IrOp::Branch;
                ab.a = sj;
                ab.target = c->land;
                ab.target2 = nb;
                emit(std::move(ab));
                switch_to(nb);
            }
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = gs.cont;
            emit(std::move(jj));
        }
        cur = nextb;
    }
    switch_to(cur);
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = start;
        emit(std::move(j));
    }

    // ── 异常注入分发（协程）────────────────────────────────────
    // 被 await 的对象失败时，事件循环把异常放进帧里并重新调度本协程。
    // 恢复路径先重挂 handler 再抛：try/except 才能抓到跨 await 的异常。
    if (is_coro) {
        int tcur = throw_disp;
        for (auto& gs : st_->gen_states) {
            switch_to(tcur);
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Eq;
            cmp.a = state;
            cmp.b = const_int(gs.id);
            VReg cc = def(t_bool(), std::move(cmp));
            std::vector<const FuncCtx::Cleanup*> hs;
            for (auto& c : gs.cleanups)
                if (has_runtime_handler(c)) hs.push_back(&c);
            int arm = new_block("g.throw.arm");
            int nextb = new_block("g.throw.disp");
            Inst br;
            br.op = IrOp::Branch;
            br.a = cc;
            br.target = arm;
            br.target2 = nextb;
            emit(std::move(br));
            switch_to(arm);
            for (const auto* c : hs)
                call_void(RtFn::HandlerRearm, {load_slot(c->handler_slot, t_int())});
            Inst rt;
            rt.op = IrOp::GenRethrow;
            emit(std::move(rt));
            tcur = nextb;
        }
        switch_to(tcur);
        Inst rt;
        rt.op = IrOp::GenRethrow;
        emit(std::move(rt));
    }

    switch_to(close_disp);
    int ccur = st_->blk;
    for (auto& [id, blk] : close_targets) {
        switch_to(ccur);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Eq;
        cmp.a = state;
        cmp.b = const_int(id);
        VReg cc = def(t_bool(), std::move(cmp));
        int nextb = new_block("g.close.disp");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = blk;
        br.target2 = nextb;
        emit(std::move(br));
        ccur = nextb;
    }
    switch_to(ccur);
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = close_default;
        emit(std::move(j));
    }

    IRGenerator gin;
    gin.resume = ridx;
    gin.n_slots = static_cast<int>(irf->slot_ty.size());
    gin.frame_kind = frame_kind;
    gin.slot_heap.assign(static_cast<size_t>(gin.n_slots), 0);
    for (int s = 0; s < gin.n_slots; ++s)
        if (!st_->borrow_slots.count(s) && ir_is_heap(irf->slot_ty[static_cast<size_t>(s)]))
            gin.slot_heap[static_cast<size_t>(s)] = 1;
    int gidx = static_cast<int>(mod_.generators.size());
    mod_.generators.push_back(std::move(gin));

    pop_frame();

    // 创建包装：参数槽与 resume 完全一致，帧里的槽位按同一编号填。
    cw->params = irf->params;
    cw->slot_ty = irf->slot_ty;
    cw->env_slot = env_slot;
    push_frame(cw, nullptr);
    int b0 = new_block("entry");
    switch_to(b0);
    Inst gn;
    gn.op = IrOp::GenNew;
    gn.imm = gidx;
    VReg gen = def(gen_ty, std::move(gn));
    for (auto& p : cw->params) {
        VReg v = load_slot(p.slot, p.ty);
        if (ir_is_heap(p.ty)) rc_inc(v);
        Inst si;
        si.op = IrOp::GenSetSlot;
        si.idx = p.slot;
        si.args = {gen, v};
        emit(std::move(si));
    }
    if (env_slot >= 0) {
        VReg ev = load_slot(env_slot, env_ty);
        rc_inc(ev);
        Inst si;
        si.op = IrOp::GenSetSlot;
        si.idx = env_slot;
        si.args = {gen, ev};
        emit(std::move(si));
    }
    Inst r;
    r.op = IrOp::Return;
    r.a = gen;
    emit(std::move(r));
    pop_frame();
    return cidx;
}

void Lowerer::lower_funcdef(FunctionDef& f) {
    FuncInfoPtr info = tc_.func_info(&f);
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = info;

    std::string label = std::format("{}_{}", f.name, mod_.funcs.size());
    int fidx = compile_function_body(f, info, label);

    auto mit = closure_meta_.find(&f);
    const ClosureMeta* meta = mit == closure_meta_.end() ? nullptr : &mit->second;

    // 无捕获：函数值指向静态闭包（rc = INT64_MIN，永生）。
    // 有捕获：运行时分配闭包对象，填 code 和各 cell 指针。
    VReg fv;
    if (!meta || meta->env.empty()) {
        Inst fa;
        fa.op = IrOp::FuncAddr;
        fa.imm = fidx;
        fv = def(fty, std::move(fa));
    } else {
        Inst fc;
        fc.op = IrOp::FuncCode;
        fc.imm = fidx;
        VReg code = def(t_int(), std::move(fc));
        fv = call_runtime(RtFn::ClosureNew, fty,
                          {code, const_int(static_cast<int64_t>(meta->env.size()))});
        push_temp(fv);
    }

    // def 执行时才绑定名字。先绑定再填 env：自递归闭包要靠 cell 里
    // 已经写好的闭包指针找到自己（这会形成环，环目前靠泄漏处理）。
    bind_name_value(f.name, fv, fty);

    if (meta && !meta->env.empty()) {
        for (size_t i = 0; i < meta->env.size(); ++i) {
            const auto& cv = meta->env[i];
            VReg cell = cell_for_capture(cv.name);
            if (cell == kNoReg) {
                err(f.line, f.col,
                    std::format("closure capture of '{}' is not available here", cv.name));
                continue;
            }
            rc_inc(cell);
            field_store(fv, 16 + 8 * static_cast<int>(i), cell);
        }
    }

    // 装饰器按从下到上的顺序调用：f = d1(d2(f))。函数值已经绑定，
    // 每一步都重新绑定同名，后一个装饰器拿到前一步的结果。
    for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
        ExprPtr dec = std::move(*it);
        int line = dec->line;
        int col = dec->col;
        VReg callee = expr(*dec);
        TypePtr dt = tc_.type_of(dec.get());
        FuncInfoPtr dinfo = (dt && dt->kind == TyKind::Func) ? dt->fn : nullptr;
        if (!dinfo) {
            err(line, col,
                "decorator must be a function value with a known signature");
            continue;
        }
        Call dc;
        dc.line = line;
        dc.col = col;
        auto arg = std::make_unique<Name>(f.name);
        arg->line = line;
        arg->col = col;
        dc.args.push_back(std::move(arg));
        std::vector<VReg> dargs = bind_args(dc, dinfo, nullptr);
        TypePtr rty = dinfo->ret ? dinfo->ret : t_unknown();
        Inst in;
        in.op = IrOp::Call;
        in.a = callee;
        in.args = std::move(dargs);
        VReg dv = def_heap(rty, std::move(in));
        bind_name_value(f.name, dv, rty);
    }
}

void Lowerer::lower_classdef(ClassDef& c) {
    if (!c.decorators.empty()) {
        err(c.line, c.col, "class decorators are not supported by the code generator yet");
        return;
    }
    auto it = classes_.find(c.name);
    if (it == classes_.end()) return;
    const ClassInfoPtr& ci = it->second;
    auto cid_it = class_ids_.find(ci.get());
    if (cid_it == class_ids_.end()) {
        unsupported(c, "'class' inside a function");
        return;
    }
    int cid = cid_it->second;
    if (ci->base && !class_ids_.count(ci->base)) {
        err(c.line, c.col, "inheriting from an imported class is not supported yet");
        return;
    }

    // 类型检查阶段把这两类标成「没建模」而不是错误（标准库里太常见），
    // 到这儿必须拦住：布局只跟一个基类走、类体语句会被静默丢弃，
    // 放过去就是生成一个语义不对的二进制。
    if (c.bases.size() > 1)
        err(c.line, c.col, "multiple inheritance is not supported by the code generator yet");
    for (auto& s : c.body) {
        if (s->kind == StmtKind::FunctionDef || s->kind == StmtKind::AnnAssign ||
            s->kind == StmtKind::Pass)
            continue;
        if (s->kind == StmtKind::Expr) {
            auto& e = static_cast<ExprStmt&>(*s);
            if (e.value->kind == ExprKind::Constant &&
                static_cast<Constant&>(*e.value).ck == Constant::CK::Str)
                continue;
        }
        err(s->line, s->col, "class body statements are not supported by the code generator yet");
    }

    // 先把父类的 vtable 抄过来。父类一定在本类之前定义，此时它的槽
    // 已经填好；子类覆盖同名方法只改自己那几个槽，前缀保持不变。
    if (ci->base) {
        auto bit = class_ids_.find(ci->base);
        if (bit != class_ids_.end()) {
            const auto& bv = mod_.classes[bit->second].vtable;
            auto& cv = mod_.classes[cid].vtable;
            for (size_t i = 0; i < bv.size() && i < cv.size(); ++i)
                cv[i] = bv[i];
        }
    }

    for (auto& m : ci->methods) {
        if (!m.def) continue;
        if (!m.def->decorators.empty()) {
            err(m.def->line, m.def->col, "decorators on methods are not supported yet");
            continue;
        }
        std::string label = std::format("{}_{}_{}", c.name, m.name, mod_.funcs.size());
        int fidx = compile_function_body(*m.def, m.sig, label);
        if (m.slot >= 0 && m.slot < static_cast<int>(mod_.classes[cid].vtable.size()))
            mod_.classes[cid].vtable[static_cast<size_t>(m.slot)] = fidx;
    }

    if (!opts_.module_symbol.empty() && !opts_.is_entry)
        compile_ctor_wrapper(c, ci, cid);
}

void Lowerer::compile_ctor_wrapper(ClassDef& clsdef, const ClassInfoPtr& ci, int cid) {
    (void)clsdef;
    const MethodInfo* init = ci->method("__init__");
    FuncInfoPtr sig;
    if (init && init->call_sig) {
        if (init->call_sig->varargs || init->call_sig->kwargs) return;
        sig = init->call_sig;
    }
    auto ctor = std::make_shared<FuncInfo>();
    ctor->name = "__new_" + ci->name;
    if (sig) {
        ctor->pos = sig->pos;
        ctor->n_posonly = sig->n_posonly;
        ctor->kwonly = sig->kwonly;
    }
    TypePtr cls_ty = mk_class(ci.get());
    TypePtr fty = new_ty(TyKind::Func);
    fty->fn = ctor;

    std::string label = std::format("__py23_new_{}_{}", ci->name, mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = cls_ty;
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    std::vector<int> arg_slots;
    auto add_param = [&](const Param& p) {
        TypePtr T = p.ty ? p.ty : t_unknown();
        int s = wf->new_slot(T);
        wf->params.push_back(IRParam{p.name, T, s});
        st_->slots[p.name] = s;
        arg_slots.push_back(s);
    };
    if (sig) {
        for (auto& p : sig->pos) add_param(p);
        for (auto& p : sig->kwonly) add_param(p);
    }
    inc_params();

    Inst on;
    on.op = IrOp::ObjNew;
    on.imm = cid;
    VReg obj = def_heap(cls_ty, std::move(on));

    std::vector<const ClassInfo*> chain;
    for (auto* p = ci.get(); p; p = p->base) chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (auto& f : (*it)->fields) {
            if (!f.has_default || !f.default_expr) continue;
            VReg v = expr(*f.default_expr);
            v = coerce(v, vreg_ty(v), f.ty);
            int off = static_cast<int>(8 * (1 + ci->field_index(f.name)));
            store_field(f.ty, obj, off, v);
        }
    }

    if (sig && init->slot >= 0) {
        std::vector<VReg> args;
        for (int s : arg_slots) args.push_back(load_slot(s, wf->slot_ty[s]));
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = obj;
        vc.imm = init->slot;
        vc.args = std::move(args);
        TypePtr rt = sig->ret ? sig->ret : t_none();
        VReg r = def_heap(rt, std::move(vc));
        (void)r;
    }
    emit_return(obj, cls_ty);
    if (st_->rc_pushes == 0)
        erase_mark(st_->fn_mark_block, st_->fn_mark_pos, st_->fn_mark);
    pop_frame();

    // 模块顶层绑定隐藏全局：导入方按 ctor_symbol 加载闭包。
    IRGlobal g;
    g.name = "__py23$new$" + ci->name;
    g.symbol = module_class_new_symbol(opts_.module_symbol, ci->name);
    g.ty = fty;
    int gi = static_cast<int>(mod_.globals.size());
    mod_.globals.push_back(std::move(g));
    global_ids_["__py23$new$" + ci->name] = gi;
    Inst fa;
    fa.op = IrOp::FuncAddr;
    fa.imm = widx;
    VReg fv = def(fty, std::move(fa));
    store_global(gi, fv);
}

void Lowerer::lower_assert(Assert& a) {
    VReg c = expr(*a.test);
    int ok = new_block("assert.ok");
    int bad = new_block("assert.fail");
    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = ok;
    br.target2 = bad;
    emit(std::move(br));

    switch_to(bad);
    VReg msg;
    if (a.msg) {
        TypePtr mt = tc_.type_of(a.msg.get());
        VReg raw = expr(*a.msg);
        msg = convert_to(raw, mt, t_str());
    } else {
        msg = const_str("assertion failed");
    }
    call_void(RtFn::Raise, {const_str("AssertionError"), msg});
    Inst jj;
    jj.op = IrOp::Jump;
    jj.target = ok;
    emit(std::move(jj));

    switch_to(ok);
}

void Lowerer::assign_to(Expr& target, VReg v, TypePtr vty) {
    // 以 vreg 上记录的类型为准：调用方传进来的 vty 有时是目标侧的类型
    TypePtr src_ty = vreg_ty(v);
    if (!src_ty || src_ty->kind == TyKind::Unknown) src_ty = vty;
    // 元素跨数值域的容器（list[int] → list[float]）要逐元素转换并重新分配，
    // 现在没做。宁可报错，也不要让 double 槽里躺整数位模式。
    auto bad_elem = [](const TypePtr& from, const TypePtr& to) {
        if (!from || !to || from->kind != to->kind) return false;
        if (from->kind != TyKind::List && from->kind != TyKind::Tuple) return false;
        if (from->sub.empty() || to->sub.empty() || !from->sub[0] || !to->sub[0]) return false;
        if (from->sub[0]->kind == TyKind::Unknown || to->sub[0]->kind == TyKind::Unknown)
            return false;
        return (from->sub[0]->kind == TyKind::Float) != (to->sub[0]->kind == TyKind::Float);
    };
    switch (target.kind) {
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(target);
            if (int cs = comp_slot(n.id); cs >= 0) {
                TypePtr want = st_->fn->slot_ty[cs];
                if (bad_elem(src_ty, want))
                    err(target.line, target.col,
                        std::format("'{}' cannot be stored into '{}': element type conversion "
                                    "is not supported by the code generator yet",
                                    type_name(src_ty), type_name(want)));
                v = coerce(v, src_ty, want);
                store_slot(cs, v);
                break;
            }
            if (is_cell(n.id) || is_env(n.id)) {
                write_cell(n.id, v, src_ty);
                break;
            }
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                int gi = global_idx(n.id);
                TypePtr want = mod_.globals[gi].ty;
                if (bad_elem(src_ty, want))
                    err(target.line, target.col,
                        std::format("'{}' cannot be stored into '{}': element type conversion "
                                    "is not supported by the code generator yet",
                                    type_name(src_ty), type_name(want)));
                v = coerce(v, src_ty, want);
                store_global(gi, v);
            } else {
                int slot = st_->slots[n.id];
                TypePtr want = st_->fn->slot_ty[slot];
                if (bad_elem(src_ty, want))
                    err(target.line, target.col,
                        std::format("'{}' cannot be stored into '{}': element type conversion "
                                    "is not supported by the code generator yet",
                                    type_name(src_ty), type_name(want)));
                v = coerce(v, src_ty, want);
                store_slot(slot, v);
            }
            break;
        }
        case ExprKind::Tuple:
        case ExprKind::List: {
            auto& elts = (target.kind == ExprKind::Tuple ? static_cast<TupleExpr&>(target).elts
                                                         : static_cast<ListExpr&>(target).elts);
            for (size_t i = 0; i < elts.size(); ++i) {
                if (!elts[i]) continue;
                if (elts[i]->kind == ExprKind::Starred) {
                    err(elts[i]->line, elts[i]->col,
                        "starred unpacking is not supported by the code generator yet");
                    continue;
                }
                TypePtr et = tc_.type_of(elts[i].get());
                Inst in;
                in.op = IrOp::IndexLoad;
                in.a = v;
                in.b = const_int(static_cast<int64_t>(i));
                // 从字符串解包出的每个元素都是新串，必须入临时栈。
                VReg ev = (src_ty && src_ty->kind == TyKind::Str)
                              ? def_heap(et, std::move(in))
                              : def(et, std::move(in));
                assign_to(*elts[i], ev, et);
            }
            break;
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<Subscript&>(target);
            if (s.slice->kind == ExprKind::Slice) {
                auto& sl = static_cast<SliceExpr&>(*s.slice);
                TypePtr ot = tc_.type_of(s.value.get());
                if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Tuple &&
                            ot->kind != TyKind::Unknown)) {
                    err(target.line, target.col,
                        std::format("'{}' does not support slice assignment", type_name(ot)));
                    break;
                }
                if (src_ty && src_ty->kind != TyKind::List && src_ty->kind != TyKind::Tuple &&
                    src_ty->kind != TyKind::Str && src_ty->kind != TyKind::Unknown) {
                    err(target.line, target.col,
                        std::format("cannot assign '{}' to a slice", type_name(src_ty)));
                    break;
                }
                VReg obj = expr(*s.value);
                constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
                auto bound = [&](ExprPtr& e) -> VReg {
                    if (!e) return const_int(kNone);
                    VReg b = expr(*e);
                    return coerce(b, tc_.type_of(e.get()), t_int());
                };
                VReg start = bound(sl.lower);
                VReg stop = bound(sl.upper);
                VReg step = bound(sl.step);
                VReg src = v;
                if (src_ty && src_ty->kind == TyKind::Str) {
                    src = call_runtime(RtFn::StrToList, mk_list(t_str()), {v});
                    push_temp(src);
                }
                call_void(RtFn::ListSliceAssign, {obj, start, stop, step, src});
                break;
            }
            VReg obj = expr(*s.value);
            VReg idx = expr(*s.slice);
            TypePtr ot = tc_.type_of(s.value.get());
            if (ot && ot->kind == TyKind::Dict) {
                TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
                TypePtr vt = ot->sub.size() > 1 ? ot->sub[1] : t_unknown();
                idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
                v = coerce(v, src_ty, vt);
                retain_elem(idx, own_kind(kt));
                retain_elem(v, own_kind(vt));
                call_void(RtFn::DictSet, {obj, idx, v});
                break;
            }
            if (ot && ot->kind == TyKind::Set) {
                err(target.line, target.col, "set does not support item assignment");
                break;
            }
            // 列表有元素类型，存进去的值要跟着转
            if (ot && ot->kind == TyKind::List && !ot->sub.empty())
                v = coerce(v, src_ty, ot->sub[0]);
            // 运行时 list_set 只负责 dec 旧值；新值的 retain 在这里做。
            retain_elem(v, list_kind(strip_optional(ot)));
            Inst in;
            in.op = IrOp::IndexStore;
            in.a = obj;
            in.b = idx;
            in.c = v;
            emit(std::move(in));
            break;
        }
        case ExprKind::Attribute: {
            auto& at = static_cast<Attribute&>(target);
            if (auto* n = dynamic_cast<Name*>(at.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    err(target.line, target.col,
                        std::format("cannot assign to builtin module attribute '{}.{}'", n->id,
                                    at.attr));
                    break;
                }
            }
            if (auto gi = module_member(at)) {
                TypePtr mt = mod_.globals[*gi].ty;
                v = coerce(v, src_ty, mt);
                store_global(*gi, v);
                break;
            }
            TypePtr raw = tc_.type_of(at.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                auto* f = ot->cls->field(at.attr);
                if (!f) {
                    err(target.line, target.col,
                        std::format("'{}' object has no field '{}'", ot->cls->name, at.attr));
                    break;
                }
                VReg obj = expr(*at.value);
                if (opt) obj = guard_not_none(obj, std::format("field '{}'", at.attr));
                v = coerce(v, src_ty, f->ty);
                int off = static_cast<int>(8 * (1 + ot->cls->field_index(at.attr)));
                store_field(f->ty, obj, off, v);
                break;
            }
            err(target.line, target.col, "unsupported assignment target");
            break;
        }
        default:
            err(target.line, target.col, "unsupported assignment target");
            break;
    }
}

// ────────────────────────── 表达式 ──────────────────────────

VReg Lowerer::expr(Expr& e) {
    switch (e.kind) {
        case ExprKind::Constant: return lower_constant(static_cast<Constant&>(e));
        case ExprKind::Name: {
            auto& n = static_cast<Name&>(e);
            if (int cs = comp_slot(n.id); cs >= 0)
                return load_slot(cs, st_->fn->slot_ty[cs]);
            // 嵌套函数直接引用自己：取当前闭包，而不是外层绑定。
            if (st_->self_def && n.id == st_->self_def->name && !is_cell(n.id) &&
                !is_env(n.id) && !st_->slots.count(n.id)) {
                if (st_->env_slot >= 0) return load_slot(st_->env_slot, st_->self_type);
                Inst fa;
                fa.op = IrOp::FuncAddr;
                fa.imm = st_->self_idx;
                return def(st_->self_type, std::move(fa));
            }
            if (is_cell(n.id)) return read_cell(n.id);
            if (is_env(n.id)) return read_env(n.id);
            if (imported_classes_.count(n.id)) {
                err(e.line, e.col,
                    std::format("class '{}' cannot be used as a value", n.id));
                return const_none();
            }
            if (native_aliases_.count(n.id)) {
                err(e.line, e.col,
                    std::format("native function '{}' cannot be used as a value", n.id));
                return const_none();
            }
            if (resolve_module_alias(n.id)) {
                err(e.line, e.col, std::format("module '{}' cannot be used as a value", n.id));
                return const_none();
            }
            // 类名不是一个运行时可求值的值：没有类对象，构造必须在调用点
            // 直接展开。悄悄读一个空全局槽只会得到 0，比报错难查得多。
            if (!is_local(n.id) && classes_.count(n.id)) {
                err(e.line, e.col,
                    std::format("class '{}' cannot be used as a value yet", n.id));
                return const_none();
            }
            TypePtr ty = name_ty(n.id);
            if (st_->globals_decl.count(n.id) || !is_local(n.id)) {
                Inst in;
                in.op = IrOp::LoadGlobal;
                in.idx = global_idx(n.id);
                in.ty = ty;
                return def(ty, std::move(in));
            }
            return load_slot(st_->slots[n.id], ty);
        }
        case ExprKind::BinOp: return lower_binop(static_cast<BinOp&>(e));
        case ExprKind::UnaryOp: return lower_unary(static_cast<UnaryOp&>(e));
        case ExprKind::BoolOp: return lower_boolop(static_cast<BoolOp&>(e));
        case ExprKind::Compare: return lower_compare(static_cast<Compare&>(e));
        case ExprKind::Call: return lower_call(static_cast<Call&>(e));
        case ExprKind::Subscript: return lower_subscript(static_cast<Subscript&>(e));
        case ExprKind::List: {
            auto& l = static_cast<ListExpr&>(e);
            return lower_list_like(l.elts, tc_.type_of(&e), false);
        }
        case ExprKind::Tuple: {
            auto& t = static_cast<TupleExpr&>(e);
            return lower_list_like(t.elts, tc_.type_of(&e), true);
        }
        case ExprKind::Set:
            return lower_set(static_cast<SetExpr&>(e), tc_.type_of(&e));
        case ExprKind::Dict:
            return lower_dict(static_cast<DictExpr&>(e), tc_.type_of(&e));
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::DictComp:
        case ExprKind::GeneratorExp:
            return lower_comp(e);
        case ExprKind::IfExp: return lower_ifexp(static_cast<IfExp&>(e));
        case ExprKind::JoinedStr: return lower_joined(static_cast<JoinedStr&>(e));
        case ExprKind::NamedExpr: {
            auto& n = static_cast<NamedExpr&>(e);
            VReg v = expr(*n.value);
            assign_to(*n.target, v, tc_.type_of(n.value.get()));
            return v;
        }
        case ExprKind::Yield: return lower_yield(e);
        case ExprKind::Await: return lower_await(e);
        case ExprKind::YieldFrom:
            return lower_yield_from(static_cast<YieldFrom&>(e));
        case ExprKind::FormattedValue: {
            // 顶层 FormattedValue 只会出现在 f-string 内部，单独出现说明
            // 上游构造有问题；按表达式处理即可。
            auto& fv = static_cast<FormattedValue&>(e);
            return convert_to(expr(*fv.value), tc_.type_of(fv.value.get()), t_str());
        }
        case ExprKind::Attribute: {
            auto& a = static_cast<Attribute&>(e);
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
                    VReg bv;
                    TypePtr bty;
                    if (builtin_value(mod->name, a.attr, bv, bty)) return bv;
                    err(e.line, e.col,
                        std::format("native function '{}.{}' must be called", n->id, a.attr));
                    return const_none();
                }
            }
            if (auto* n = dynamic_cast<Name*>(a.value.get())) {
                if (auto mod = resolve_module_alias(n->id)) {
                    if (mod->classes.count(a.attr)) {
                        err(e.line, e.col,
                            std::format("class '{}.{}' cannot be used as a value", n->id, a.attr));
                        return const_none();
                    }
                }
            }
            if (auto gi = module_member(a))
                return load_global(*gi, mod_.globals[*gi].ty);
            TypePtr raw = tc_.type_of(a.value.get());
            TypePtr ot = strip_optional(raw);
            bool opt = ot != raw;
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                if (auto* f = ot->cls->field(a.attr)) {
                    VReg obj = expr(*a.value);
                    if (opt) obj = guard_not_none(obj, std::format("attribute '{}'", a.attr));
                    int off = static_cast<int>(8 * (1 + ot->cls->field_index(a.attr)));
                    return field_load(obj, off, f->ty);
                }
                if (ot->cls->method(a.attr))
                    err(e.line, e.col,
                        std::format("method '{}.{}' must be called with ()", ot->cls->name, a.attr));
                else
                    err(e.line, e.col,
                        std::format("'{}' object has no attribute '{}'", ot->cls->name, a.attr));
                return const_none();
            }
            err(e.line, e.col, "attribute access is only supported as a method call");
            return const_none();
        }
        case ExprKind::Starred: {
            err(e.line, e.col, "starred expression is not allowed here");
            return const_none();
        }
        case ExprKind::Slice:
            err(e.line, e.col, "slice is not supported by the code generator yet");
            return const_none();
        case ExprKind::Lambda:
            err(e.line, e.col,
                "lambda in a class-body value is not supported by the code generator yet");
            return const_none();
        default:
            err(e.line, e.col, std::format("expression kind {} is not supported by the code generator",
                                          static_cast<int>(e.kind)));
            return const_none();
    }
}

VReg Lowerer::lower_constant(Constant& c) {
    switch (c.ck) {
        case Constant::CK::None: return const_none();
        case Constant::CK::Bool: return const_bool(c.b);
        case Constant::CK::Int: return const_int(c.i);
        case Constant::CK::Float: return const_float(c.f);
        case Constant::CK::Str: return const_str(c.s);
        case Constant::CK::Ellipsis:
            err(c.line, c.col, "Ellipsis is not supported by the code generator");
            return const_none();
    }
    return const_none();
}

VReg Lowerer::lower_binop(BinOp& b) {
    TypePtr ty = tc_.type_of(&b);
    TypePtr lt = tc_.type_of(b.left.get());
    TypePtr rt = tc_.type_of(b.right.get());
    // 类型检查允许但后端还做不了的组合，在这里一次性拦住。
    // 放过去的话 codegen 会走「按整数处理」的兜底路径，指针相加不报错，
    // 只是算出一个野值——这类静默错误比编译失败难查得多。
    if (ty) {
        bool bad = false;
        switch (ty->kind) {
            case TyKind::Str: bad = b.op != Tok::PLUS; break;
            case TyKind::List:
            case TyKind::Tuple:
            case TyKind::Set:
            case TyKind::Dict: bad = true; break;
            default: break;
        }
        if (bad) {
            err(b.line, b.col,
                std::format("operator '{}' on '{}' is not supported by the code generator yet",
                            tok_name(b.op), type_name(ty)));
            return const_none();
        }
    }
    VReg l = expr(*b.left);
    VReg r = expr(*b.right);
    // int / int 的结果是 float，两个操作数都得先转；int + float 同理。
    // 放在这里做，codegen 就不用每条浮点指令都猜操作数是什么类型。
    if (ty && ty->kind == TyKind::Float) {
        if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
        if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
    }
    Inst in;
    in.op = IrOp::Bin;
    in.bop = map_bin(b.op);
    in.a = l;
    in.b = r;
    return def_heap(ty, std::move(in));
}

VReg Lowerer::lower_unary(UnaryOp& u) {
    TypePtr ty = tc_.type_of(&u);
    VReg v = expr(*u.operand);
    Inst in;
    in.op = IrOp::Un;
    switch (u.op) {
        case Tok::MINUS: in.uop = UnKind::Neg; break;
        case Tok::PLUS: in.uop = UnKind::Pos; break;
        case Tok::TILDE: in.uop = UnKind::BitNot; break;
        case Tok::KW_NOT: in.uop = UnKind::Not; break;
        default:
            err(u.line, u.col, "unsupported unary operator");
            in.uop = UnKind::Pos;
            break;
    }
    in.a = v;
    return def(ty, std::move(in));
}

VReg Lowerer::lower_boolop(BoolOp& b) {
    TypePtr ty = tc_.type_of(&b);
    int tmp = new_borrow_slot(ty);

    bool is_and = b.op == Tok::KW_AND;
    int end = new_block(is_and ? "and.end" : "or.end");

    VReg acc = expr(*b.values[0]);
    store_borrow_slot(tmp, acc);

    for (size_t i = 1; i < b.values.size(); ++i) {
        VReg cur_v = load_slot(tmp, ty);
        int rhs = new_block(is_and ? "and.rhs" : "or.rhs");
        int next = (i + 1 < b.values.size()) ? new_block(is_and ? "and.next" : "or.next") : end;

        Inst br;
        br.op = IrOp::Branch;
        br.a = cur_v;
        // and: 真才继续算右边；or: 假才继续算右边
        br.target = is_and ? rhs : next;
        br.target2 = is_and ? next : rhs;
        emit(std::move(br));

        switch_to(rhs);
        VReg v = expr(*b.values[i]);
        store_borrow_slot(tmp, v);
        Inst j;
        j.op = IrOp::Jump;
        j.target = next;
        emit(std::move(j));

        if (next == end) break;
        switch_to(next);
    }

    switch_to(end);
    return load_slot(tmp, ty);
}

VReg Lowerer::lower_compare(Compare& c) {
    if (c.ops.size() == 1) {
        VReg l = expr(*c.left);
        VReg r = expr(*c.comparators[0]);
        CmpOp op = c.ops[0];
        TypePtr lt = tc_.type_of(c.left.get());
        TypePtr rt = tc_.type_of(c.comparators[0].get());
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            VReg flag;
            if (rt && (rt->kind == TyKind::Dict || rt->kind == TyKind::Set)) {
                TypePtr kt = rt->sub.empty() ? t_unknown() : rt->sub[0];
                l = coerce(l, lt, kt);
                flag = call_runtime(RtFn::DictContains, t_bool(), {r, l});
            } else if (rt && (rt->kind == TyKind::List || rt->kind == TyKind::Tuple)) {
                TypePtr et = rt->sub.empty() ? t_unknown() : rt->sub[0];
                l = coerce(l, lt, et);
                int ck = key_kind(et);
                if (rt->kind == TyKind::Tuple) {
                    for (auto& sub : rt->sub)
                        if (key_kind(sub) != ck) { ck = 3; break; }
                }
                flag = call_runtime(RtFn::ListContains, t_bool(), {r, l, const_int(ck)});
            } else if (rt && rt->kind == TyKind::Str) {
                if (lt && lt->kind != TyKind::Str && lt->kind != TyKind::Unknown) {
                    err(c.line, c.col,
                        std::format("'in <str>' requires a string, got '{}'", type_name(lt)));
                    return const_bool(false);
                }
                flag = call_runtime(RtFn::StrContains, t_bool(), {r, l});
            } else {
                err(c.line, c.col,
                    std::format("'in' on '{}' is not supported by the code generator yet",
                                type_name(rt)));
                return const_bool(false);
            }
            if (op == CmpOp::NotIn) {
                Inst un;
                un.op = IrOp::Un;
                un.uop = UnKind::Not;
                un.a = flag;
                flag = def(t_bool(), std::move(un));
            }
            return flag;
        }
        // 混合数值比较统一到 float：codegen 按左操作数的类型选指令
        if ((ir_is_float(lt) || ir_is_float(rt)) && is_num(lt) && is_num(rt)) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rt)) r = convert_to(r, rt, t_float());
        }
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = map_cmp(op);
        in.a = l;
        in.b = r;
        return def(t_bool(), std::move(in));
    }

    for (auto op : c.ops) {
        if (op == CmpOp::In || op == CmpOp::NotIn) {
            err(c.line, c.col, "chained 'in' comparisons are not supported");
            return const_bool(false);
        }
    }

    // 链式比较：需要短路，用槽保存中间值。
    int res = new_temp_slot(t_bool());
    // 槽类型取左操作数：链式比较里存的是同一个值，类型决定真值判断。
    // 它只是借用在表达式内传递，生命周期由临时栈负责。
    int cur_slot = new_borrow_slot(tc_.type_of(c.left.get()));
    store_slot(res, const_bool(true));
    store_borrow_slot(cur_slot, expr(*c.left));

    int fail = new_block("cmp.fail");
    int done = new_block("cmp.end");

    for (size_t i = 0; i < c.ops.size(); ++i) {
        VReg l = load_slot(cur_slot, t_unknown());
        VReg r = expr(*c.comparators[i]);
        int cont = new_block("cmp.next");
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = map_cmp(c.ops[i]);
        cmp.a = l;
        cmp.b = r;
        VReg cr = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cr;
        br.target = cont;
        br.target2 = fail;
        emit(std::move(br));

        switch_to(cont);
        store_borrow_slot(cur_slot, r);
        if (i + 1 == c.ops.size()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = done;
            emit(std::move(j));
        }
    }

    switch_to(fail);
    store_slot(res, const_bool(false));
    Inst j;
    j.op = IrOp::Jump;
    j.target = done;
    emit(std::move(j));

    switch_to(done);
    return load_slot(res, t_bool());
}

VReg Lowerer::lower_subscript(Subscript& s) {
    VReg obj = expr(*s.value);
    TypePtr ot = tc_.type_of(s.value.get());
    TypePtr ty = tc_.type_of(&s);

    if (s.slice->kind == ExprKind::Slice) {
        auto& sl = static_cast<SliceExpr&>(*s.slice);
        constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
        auto bound = [&](ExprPtr& e) -> VReg {
            if (!e) return const_int(kNone);
            VReg v = expr(*e);
            return coerce(v, tc_.type_of(e.get()), t_int());
        };
        VReg start = bound(sl.lower);
        VReg stop = bound(sl.upper);
        VReg step = bound(sl.step);
        if (ot && ot->kind == TyKind::Str) {
            VReg v = call_runtime(RtFn::StrSlice, ty, {obj, start, stop, step});
            push_temp(v);
            return v;
        }
        if (ot && (ot->kind == TyKind::List || ot->kind == TyKind::Tuple)) {
            VReg v = call_runtime(RtFn::ListSlice, ty, {obj, start, stop, step});
            push_temp(v);
            return v;
        }
        err(s.line, s.col, std::format("'{}' is not sliceable", type_name(ot)));
        return const_none();
    }

    VReg idx = expr(*s.slice);
    if (ot && ot->kind == TyKind::Dict) {
        TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
        idx = coerce(idx, tc_.type_of(s.slice.get()), kt);
        return call_runtime(RtFn::DictGet, ty, {obj, idx});
    }
    if (ot && ot->kind == TyKind::Set) {
        err(s.line, s.col, "set is not subscriptable");
        return const_none();
    }
    Inst in;
    in.op = IrOp::IndexLoad;
    in.a = obj;
    in.b = idx;
    // 只有「字符串下标」产生新串；list[str]/tuple[str] 返回的是容器内部的
    // 借用引用，不能入临时栈，否则会比容器多减一次引用。
    if (ty && ty->kind == TyKind::Str && ot && ot->kind == TyKind::Str)
        return def_heap(ty, std::move(in));
    return def(ty, std::move(in));
}

VReg Lowerer::lower_list_like(std::vector<ExprPtr>& elts, TypePtr ty, bool is_tuple) {
    std::vector<VReg> args;
    args.reserve(elts.size());
    // 元素所有权类别：0 非堆 / 1 堆 / 2 unknown 或混合。
    // kind 1/2 的元素在写入前 retain；kind 1 的容器销毁时由运行时 release。
    int kind = list_kind(ty);
    for (size_t i = 0; i < elts.size(); ++i) {
        auto& e = elts[i];
        if (e->kind == ExprKind::Starred) {
            err(e->line, e->col, "starred element is not supported by the code generator yet");
            continue;
        }
        VReg v = expr(*e);
        // 字面量的元素类型是按全部元素推出来的（`[1, 2.5]` 是 list[float]），
        // 前面的整数元素得在构造时就转成 double。
        TypePtr want;
        if (ty && !is_tuple && !ty->sub.empty()) want = ty->sub[0];
        else if (ty && is_tuple && !ty->sub.empty() && !ty->variadic && i < ty->sub.size())
            want = ty->sub[i];
        else if (ty && is_tuple && ty->variadic && !ty->sub.empty())
            want = ty->sub[0];
        v = coerce(v, tc_.type_of(e.get()), want);
        retain_elem(v, kind);
        args.push_back(v);
    }
    Inst in;
    in.op = is_tuple ? IrOp::MakeTuple : IrOp::MakeList;
    in.imm = kind;
    in.args = std::move(args);
    return def_heap(ty, std::move(in));
}

VReg Lowerer::lower_dict(DictExpr& d, TypePtr ty) {
    TypePtr kt = t_unknown(), vt = t_unknown();
    if (ty && ty->kind == TyKind::Dict) {
        if (!ty->sub.empty()) kt = ty->sub[0];
        if (ty->sub.size() > 1) vt = ty->sub[1];
    }
    TypePtr dtype = (ty && ty->kind == TyKind::Dict) ? ty : mk_dict(kt, vt);
    int kk = key_kind(kt);
    int ko = own_kind(kt);
    int vo = own_kind(vt);

    VReg dict = call_runtime(RtFn::DictNew, dtype,
                             {const_int(kk), const_int(ko), const_int(vo)});
    push_temp(dict);
    for (size_t i = 0; i < d.values.size(); ++i) {
        if (!d.keys[i]) {
            err(d.line, d.col, "'**' dict unpacking is not supported by the code generator yet");
            continue;
        }
        VReg k = expr(*d.keys[i]);
        k = coerce(k, tc_.type_of(d.keys[i].get()), kt);
        retain_elem(k, ko);
        VReg v = expr(*d.values[i]);
        v = coerce(v, tc_.type_of(d.values[i].get()), vt);
        retain_elem(v, vo);
        call_void(RtFn::DictSet, {dict, k, v});
    }
    return dict;
}

VReg Lowerer::lower_set(SetExpr& s, TypePtr ty) {
    TypePtr et = t_unknown();
    if (ty && ty->kind == TyKind::Set && !ty->sub.empty()) et = ty->sub[0];
    TypePtr stype = (ty && ty->kind == TyKind::Set) ? ty : mk_set(et);
    int ek = key_kind(et);
    int eo = own_kind(et);

    VReg set = call_runtime(RtFn::DictNew, stype,
                            {const_int(ek), const_int(eo), const_int(0)});
    push_temp(set);
    for (auto& e : s.elts) {
        VReg v = expr(*e);
        v = coerce(v, tc_.type_of(e.get()), et);
        retain_elem(v, eo);
        call_void(RtFn::DictSet, {set, v, const_int(0)});
    }
    return set;
}

VReg Lowerer::lower_comp(Expr& e) {
    std::vector<ExprPtr>* gens = nullptr;
    Expr* elt_expr = nullptr;
    Expr* key_expr = nullptr;
    Expr* val_expr = nullptr;
    bool is_set = false;
    bool is_dict = false;
    switch (e.kind) {
        case ExprKind::ListComp: {
            auto& c = static_cast<ListComp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            break;
        }
        case ExprKind::GeneratorExp: {
            auto& c = static_cast<GeneratorExp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            break;
        }
        case ExprKind::SetComp: {
            auto& c = static_cast<SetComp&>(e);
            gens = &c.generators;
            elt_expr = c.elt.get();
            is_set = true;
            break;
        }
        case ExprKind::DictComp: {
            auto& c = static_cast<DictComp&>(e);
            gens = &c.generators;
            key_expr = c.key.get();
            val_expr = c.value.get();
            is_dict = true;
            break;
        }
        default: return const_none();
    }

    TypePtr rty = tc_.type_of(&e);
    TypePtr elem_ty = t_unknown();
    TypePtr val_ty = t_unknown();

    bool any_async = false;
    for (auto& g : *gens)
        if (static_cast<Comprehension&>(*g).is_async) any_async = true;
    bool async_mode = false;
    if (any_async) {
        if (!st_->is_coroutine) {
            err(e.line, e.col, "'async for' in a comprehension requires an async function");
        } else if (comp_ctx_node_ != &e) {
            err(e.line, e.col,
                "'async for' in a comprehension is only supported as a statement, "
                "assignment or return value");
        } else if (e.kind == ExprKind::GeneratorExp) {
            err(e.line, e.col, "async generator expressions are not supported yet");
        } else {
            async_mode = true;
        }
        if (!async_mode) return const_none();
    }

    VReg acc;
    if (is_dict) {
        if (!rty || rty->kind != TyKind::Dict) rty = mk_dict(t_unknown(), t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        if (rty->sub.size() > 1) val_ty = rty->sub[1];
        acc = call_runtime(RtFn::DictNew, rty,
                           {const_int(key_kind(elem_ty)), const_int(own_kind(elem_ty)),
                            const_int(own_kind(val_ty))});
        push_temp(acc);
    } else if (is_set) {
        if (!rty || rty->kind != TyKind::Set) rty = mk_set(t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        acc = call_runtime(RtFn::DictNew, rty,
                           {const_int(key_kind(elem_ty)), const_int(own_kind(elem_ty)),
                            const_int(0)});
        push_temp(acc);
    } else {
        if (!rty || rty->kind != TyKind::List) rty = mk_list(t_unknown());
        if (!rty->sub.empty()) elem_ty = rty->sub[0];
        Inst mk;
        mk.op = IrOp::MakeList;
        mk.imm = list_kind(rty);
        acc = def_heap(rty, std::move(mk));
    }

    // 挂起后 vreg 不存活：累加器落帧槽，循环内外都从槽里读。
    int acc_slot = -1;
    if (async_mode) {
        acc_slot = new_temp_slot(rty);
        store_slot(acc_slot, acc);
    }
    auto acc_val = [&]() { return acc_slot >= 0 ? load_slot(acc_slot, rty) : acc; };

    auto emit_result = [&]() {
        if (is_dict) {
            VReg k = expr(*key_expr);
            k = coerce(k, tc_.type_of(key_expr), elem_ty);
            retain_elem(k, own_kind(elem_ty));
            VReg v = expr(*val_expr);
            v = coerce(v, tc_.type_of(val_expr), val_ty);
            retain_elem(v, own_kind(val_ty));
            call_void(RtFn::DictSet, {acc_val(), k, v});
            return;
        }
        VReg v = expr(*elt_expr);
        v = coerce(v, tc_.type_of(elt_expr), elem_ty);
        if (is_set) {
            retain_elem(v, own_kind(elem_ty));
            call_void(RtFn::DictSet, {acc_val(), v, const_int(0)});
        } else {
            retain_elem(v, list_kind(rty));
            call_void(RtFn::ListAppend, {acc_val(), v});
        }
    };

    comp_slots_.emplace_back();

    std::function<void(size_t)> gen = [&](size_t gi) {
        if (gi >= gens->size()) {
            emit_result();
            return;
        }
        auto& g = static_cast<Comprehension&>(*(*gens)[gi]);
        VReg seq = expr(*g.iter);
        TypePtr sty = tc_.type_of(g.iter.get());

        if (g.is_async) {
            TypePtr sst = sty ? sty : t_unknown();
            TypePtr ety = t_unknown();
            if (sty && sty->kind == TyKind::AsyncGenerator && !sty->sub.empty())
                ety = sty->sub[0];
            int seq_slot = new_temp_slot(sst);
            store_slot(seq_slot, seq);
            int hs_slot = new_temp_slot(t_int());

            int next_try = new_block("comp.anext");
            int fetch = new_block("comp.afetch");
            int land = new_block("comp.aland");
            int reraise_b = new_block("comp.areaise");
            int end = new_block("comp.aend");

            Inst j;
            j.op = IrOp::Jump;
            j.target = next_try;
            emit(std::move(j));

            switch_to(next_try);
            VReg h = call_runtime(RtFn::HandlerPush, t_int(), {});
            store_slot(hs_slot, h);
            VReg sj = setjmp_handler(hs_slot);
            {
                Inst br;
                br.op = IrOp::Branch;
                br.a = sj;
                br.target = land;
                br.target2 = fetch;
                emit(std::move(br));
            }
            FuncCtx::Cleanup cf;
            cf.kind = FuncCtx::Cleanup::Kind::Handler;
            cf.handler_slot = hs_slot;
            cf.land = land;
            st_->cleanups.push_back(cf);

            switch_to(fetch);
            Inst call;
            call.op = IrOp::CallRt;
            call.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
            call.args = {load_slot(seq_slot, sst)};
            VReg w = def_heap(mk_coroutine(ety), std::move(call));
            VReg val = suspend_on_awaitable(w, ety, g.line, g.col);
            st_->cleanups.pop_back();
            call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});

            if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
                int slot = comp_slot(tn->id);
                if (slot < 0) {
                    slot = st_->fn->new_slot(ety);
                    comp_slots_.back()[tn->id] = slot;
                }
                store_slot(slot, val);
            } else {
                assign_to(*g.target, val, ety);
            }

            for (auto& cond_expr : g.ifs) {
                VReg c = expr(*cond_expr);
                int ok_b = new_block("comp.aif");
                Inst cb;
                cb.op = IrOp::Branch;
                cb.a = c;
                cb.target = ok_b;
                cb.target2 = next_try;
                emit(std::move(cb));
                switch_to(ok_b);
            }

            gen(gi + 1);
            if (!terminated()) {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = next_try;
                emit(std::move(jj));
            }

            switch_to(land);
            call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
            VReg m = call_runtime(RtFn::ExcMatches, t_bool(),
                                  {const_str("StopAsyncIteration")});
            int stop_ok = new_block("comp.astop");
            {
                Inst br;
                br.op = IrOp::Branch;
                br.a = m;
                br.target = stop_ok;
                br.target2 = reraise_b;
                emit(std::move(br));
            }
            switch_to(reraise_b);
            call_void(RtFn::ExcReraise, {});
            end_unreachable();
            switch_to(stop_ok);
            call_void(RtFn::ExcClear, {});
            {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = end;
                emit(std::move(jj));
            }
            switch_to(end);
            return;
        }

        bool ok = sty && (sty->kind == TyKind::List || sty->kind == TyKind::Tuple ||
                          sty->kind == TyKind::Str || sty->kind == TyKind::Dict ||
                          sty->kind == TyKind::Set);
        if (!ok) {
            err(g.iter->line, g.iter->col,
                std::format("cannot iterate over '{}' in a comprehension", type_name(sty)));
            return;
        }
        TypePtr ety = t_unknown();
        if (sty->kind == TyKind::Str) ety = t_str();
        else if (!sty->sub.empty()) ety = sty->sub[0];

        Inst ln;
        ln.op = IrOp::Len;
        ln.a = seq;
        VReg n = def(t_int(), std::move(ln));

        int seq_slot = -1;
        int n_slot = -1;
        if (async_mode) {
            seq_slot = new_temp_slot(sty);
            store_slot(seq_slot, seq);
            n_slot = new_temp_slot(t_int());
            store_slot(n_slot, n);
        }
        auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sty) : seq; };
        auto n_val = [&]() { return n_slot >= 0 ? load_slot(n_slot, t_int()) : n; };

        int idx = new_temp_slot(t_int());
        store_slot(idx, const_int(0));

        int cond = new_block("comp.cond");
        int body = new_block("comp.body");
        int incr = new_block("comp.incr");
        int end = new_block("comp.end");

        // 水位取在序列求值之后：seq 本身可能是临时容器，不能被每轮清掉。
        // async 模式下临时值随挂起统一清理，不需要局部水位。
        int mark_block = -1;
        size_t mark_pos = 0;
        VReg mark = async_mode ? kNoReg : rc_mark_at(mark_block, mark_pos);
        auto clean = [&]() {
            if (async_mode) cleanup_gen_mark();
            else rc_cleanup(mark);
        };
        size_t pushes_before = st_->rc_pushes;

        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));

        switch_to(cond);
        VReg iv = load_slot(idx, t_int());
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Lt;
        cmp.a = iv;
        cmp.b = n_val();
        VReg cc = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = body;
        br.target2 = end;
        emit(std::move(br));

        switch_to(body);
        clean();
        VReg iv2 = load_slot(idx, t_int());
        VReg ev;
        if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
            ev = call_runtime(RtFn::DictKeyAt, ety, {seq_val(), iv2});
        } else {
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = seq_val();
            get.b = iv2;
            ev = sty->kind == TyKind::Str ? def_heap(ety, std::move(get))
                                          : def(ety, std::move(get));
        }
        if (auto* tn = dynamic_cast<Name*>(g.target.get())) {
            int slot = comp_slot(tn->id);
            if (slot < 0) {
                slot = st_->fn->new_slot(ety);
                comp_slots_.back()[tn->id] = slot;
            }
            store_slot(slot, ev);
        } else {
            assign_to(*g.target, ev, ety);
        }

        // 过滤器：任何一条不成立就跳到自增块。
        for (auto& cond_expr : g.ifs) {
            VReg c = expr(*cond_expr);
            int ok_b = new_block("comp.if");
            Inst cb;
            cb.op = IrOp::Branch;
            cb.a = c;
            cb.target = ok_b;
            cb.target2 = incr;
            emit(std::move(cb));
            switch_to(ok_b);
        }

        gen(gi + 1);
        if (!terminated()) {
            Inst jj;
            jj.op = IrOp::Jump;
            jj.target = incr;
            emit(std::move(jj));
        }

        switch_to(incr);
        clean();
        VReg iv3 = load_slot(idx, t_int());
        Inst add;
        add.op = IrOp::Bin;
        add.bop = BinKind::Add;
        add.a = iv3;
        add.b = const_int(1);
        VReg iv4 = def(t_int(), std::move(add));
        store_slot(idx, iv4);
        Inst ji;
        ji.op = IrOp::Jump;
        ji.target = cond;
        emit(std::move(ji));

        switch_to(end);
        clean();
        if (!async_mode && !st_->is_generator && st_->rc_pushes == pushes_before) {
            erase_cleanup(body, mark);
            erase_cleanup(incr, mark);
            erase_cleanup(end, mark);
            erase_mark(mark_block, mark_pos, mark);
        }
    };

    gen(0);
    comp_slots_.pop_back();
    return async_mode ? acc_val() : acc;
}

VReg Lowerer::lower_ifexp(IfExp& e) {
    TypePtr ty = tc_.type_of(&e);
    int tmp = new_borrow_slot(ty);
    VReg c = expr(*e.test);
    int then_b = new_block("ifexp.then");
    int else_b = new_block("ifexp.else");
    int end = new_block("ifexp.end");

    Inst br;
    br.op = IrOp::Branch;
    br.a = c;
    br.target = then_b;
    br.target2 = else_b;
    emit(std::move(br));

    switch_to(then_b);
    store_borrow_slot(tmp, expr(*e.body));
    Inst j1;
    j1.op = IrOp::Jump;
    j1.target = end;
    emit(std::move(j1));

    switch_to(else_b);
    store_borrow_slot(tmp, expr(*e.orelse));
    Inst j2;
    j2.op = IrOp::Jump;
    j2.target = end;
    emit(std::move(j2));

    switch_to(end);
    return load_slot(tmp, ty);
}

VReg Lowerer::convert_to(VReg v, TypePtr from, TypePtr to) {
    if (!to) return v;
    if (from && type_eq(from, to)) return v;
    Inst in;
    in.op = IrOp::Convert;
    in.a = v;
    VReg r = def(to, std::move(in));
    // 数值转换产出标量；转成字符串才是一个新的堆对象。
    if (to->kind == TyKind::Str && (!from || from->kind != TyKind::Str))
        push_temp(r);
    return r;
}

// 存储位置（槽 / 全局 / 形参 / 列表元素）有静态类型，值可能落在另一个
// 数值域上——典型是 `x: float = 1`。不补这条 convert，整数 1 的位模式
// 会被当成 double 读出来（5e-324），而且错得毫无声响。
// 只在数值域之间转：指针类之间的差异属于类型检查该报的错，这里不猜。
VReg Lowerer::coerce(VReg v, TypePtr from, TypePtr to) {
    if (!from || !to) return v;
    if (type_eq(from, to)) return v;
    auto num = [](TyKind k) { return k == TyKind::Int || k == TyKind::Bool || k == TyKind::Float; };
    if (!num(from->kind) || !num(to->kind)) return v;
    bool from_int = from->kind != TyKind::Float;
    bool to_int = to->kind != TyKind::Float;
    if (from_int == to_int) return v;  // int/bool 互换不用动，机器表示一样
    return convert_to(v, from, to);
}

VReg Lowerer::lower_joined(JoinedStr& j) {
    VReg acc = const_str("");
    bool first = true;
    for (auto& part : j.values) {
        VReg piece;
        if (part->kind == ExprKind::Constant) {
            auto& c = static_cast<Constant&>(*part);
            if (c.ck != Constant::CK::Str) {
                err(part->line, part->col, "non-string literal inside f-string");
                continue;
            }
            if (c.s.empty()) continue;
            piece = const_str(c.s);
        } else if (part->kind == ExprKind::FormattedValue) {
            auto& fv = static_cast<FormattedValue&>(*part);
            if (fv.format_spec) {
                bool empty_spec = false;
                if (fv.format_spec->kind == ExprKind::JoinedStr) {
                    auto& spec = static_cast<JoinedStr&>(*fv.format_spec);
                    empty_spec = spec.values.empty() ||
                                 (spec.values.size() == 1 &&
                                  spec.values[0]->kind == ExprKind::Constant &&
                                  static_cast<Constant&>(*spec.values[0]).s.empty());
                }
                if (!empty_spec) {
                    err(part->line, part->col,
                        "f-string format spec is not supported by the code generator yet");
                    continue;
                }
            }
            TypePtr vt = tc_.type_of(fv.value.get());
            VReg raw = expr(*fv.value);
            piece = convert_to(raw, vt, t_str());
        } else {
            err(part->line, part->col, "unsupported f-string part");
            continue;
        }

        if (first) {
            acc = piece;
            first = false;
        } else {
            acc = call_runtime(RtFn::StrConcat, t_str(), {acc, piece});
            push_temp(acc);
        }
    }
    return acc;
}

// 按签名重排实参：位置实参、关键字实参、缺省值、形参类型的数值转换。
// info 为空（签名未知）时按源码顺序直传，关键字参数直接报错。
std::vector<VReg> Lowerer::bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* defn) {
    std::vector<VReg> args;
    if (!info) {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "keyword arguments require a known function signature");
            return args;
        }
        for (auto& a : c.args) {
            if (a->kind == ExprKind::Starred) {
                err(a->line, a->col, "'*args' is not supported by the code generator yet");
                continue;
            }
            args.push_back(expr(*a));
        }
        return args;
    }

    size_t npos = info->pos.size();
    size_t nkw = info->kwonly.size();
    std::vector<VReg> bound(npos + nkw, kNoReg);

    // ── *args ────────────────────────────────────────────────
    // 变长实参在调用点打包成一个 list，作为最后一个（或倒数第二个）
    // 形参传进去；callee 里它就是一个普通 list 形参。
    TypePtr vty = info->varargs ? (info->vararg_ty ? info->vararg_ty : t_unknown()) : nullptr;
    TypePtr lty = info->varargs ? mk_list(vty) : nullptr;
    int vk = info->varargs ? list_kind(lty) : 0;
    VReg vlist = kNoReg;
    auto vararg_list = [&]() -> VReg {
        if (vlist == kNoReg) {
            vlist = call_runtime(RtFn::ListNew, lty, {const_int(0), const_int(vk)});
            push_temp(vlist);
        }
        return vlist;
    };
    auto append_vararg = [&](VReg v, TypePtr vt) {
        v = coerce(v, vt, vty);
        retain_elem(v, vk);
        call_void(RtFn::ListAppend, {vararg_list(), v});
    };

    // 已知长度的 tuple 可以静态展开，即使 callee 没有 *args。
    auto is_fixed_tuple = [&](TypePtr t) {
        return t && t->kind == TyKind::Tuple && !t->variadic;
    };

    size_t pos_i = 0;
    for (auto& ap : c.args) {
        Expr& a = *ap;
        if (a.kind == ExprKind::Starred) {
            Expr& inner = *static_cast<Starred&>(a).value;
            TypePtr tt = tc_.type_of(&inner);
            if (is_fixed_tuple(tt)) {
                VReg tv = expr(inner);
                for (size_t j = 0; j < tt->sub.size(); ++j) {
                    TypePtr et = tt->sub[j];
                    Inst in;
                    in.op = IrOp::IndexLoad;
                    in.a = tv;
                    in.b = const_int(static_cast<int64_t>(j));
                    VReg ev = def(et, std::move(in));
                    if (pos_i < npos) {
                        bound[pos_i++] = ev;
                    } else if (info->varargs) {
                        append_vararg(ev, et);
                    } else {
                        err(a.line, a.col,
                            std::format("too many positional arguments (expected at most {})", npos));
                        break;
                    }
                }
                continue;
            }
            if (pos_i < npos) {
                expr(inner);
                err(a.line, a.col,
                    "'*' cannot be expanded before the fixed positional parameters are filled; "
                    "pass the iterable after them or make the parameter count static");
                continue;
            }
            if (!info->varargs) {
                expr(inner);
                err(a.line, a.col,
                    std::format("function '{}' does not accept a variable number of arguments",
                                info->name));
                continue;
            }
            VReg s = expr(inner);
            TypePtr st = tc_.type_of(&inner);
            if (st && st->kind == TyKind::Str) {
                VReg l = call_runtime(RtFn::StrToList, mk_list(t_str()), {s});
                push_temp(l);
                s = l;
            } else if (st && st->kind != TyKind::List && st->kind != TyKind::Tuple &&
                       st->kind != TyKind::Unknown) {
                err(a.line, a.col,
                    std::format("cannot expand '{}' into '*args'", type_name(st)));
                continue;
            }
            call_void(RtFn::ListExtend, {vararg_list(), s});
            continue;
        }
        VReg v = expr(a);
        if (pos_i < npos) {
            bound[pos_i++] = v;
        } else if (info->varargs) {
            append_vararg(v, tc_.type_of(&a));
        } else {
            err(a.line, a.col,
                std::format("too many positional arguments (expected at most {})", npos));
        }
    }

    // ── **kwargs ─────────────────────────────────────────────
    TypePtr kwty = info->kwargs ? (info->kwarg_ty ? info->kwarg_ty : t_unknown()) : nullptr;
    TypePtr kdty = info->kwargs ? mk_dict(t_str(), kwty) : nullptr;
    int kown = info->kwargs ? own_kind(kwty) : 0;
    VReg kwd = kNoReg;
    auto kwarg_dict = [&]() -> VReg {
        if (kwd == kNoReg) {
            kwd = call_runtime(RtFn::DictNew, kdty,
                               {const_int(1), const_int(1), const_int(kown)});
            push_temp(kwd);
        }
        return kwd;
    };

    for (auto& k : c.keywords) {
        if (k.arg.empty()) {
            if (!info->kwargs) {
                expr(*k.value);
                err(k.value->line, k.value->col,
                    std::format("'{}' does not accept '**kwargs'", info->name));
                continue;
            }
            VReg m = expr(*k.value);
            TypePtr mt = tc_.type_of(k.value.get());
            if (mt && mt->kind != TyKind::Dict && mt->kind != TyKind::Unknown) {
                err(k.value->line, k.value->col,
                    "cannot unpack a non-dict value into '**kwargs'");
                continue;
            }
            if (mt && mt->kind == TyKind::Dict && !mt->sub.empty() &&
                mt->sub[0]->kind != TyKind::Str && mt->sub[0]->kind != TyKind::Unknown) {
                err(k.value->line, k.value->col, "'**kwargs' keys must be strings");
                continue;
            }
            call_void(RtFn::DictUpdate, {kwarg_dict(), m});
            continue;
        }
        int idx = -1;
        for (size_t i = info->n_posonly; i < npos; ++i)
            if (info->pos[i].name == k.arg) idx = static_cast<int>(i);
        if (idx < 0) {
            for (size_t i = 0; i < nkw; ++i)
                if (info->kwonly[i].name == k.arg) idx = static_cast<int>(npos + i);
        }
        if (idx < 0) {
            if (info->kwargs) {
                VReg v = expr(*k.value);
                v = coerce(v, tc_.type_of(k.value.get()), kwty);
                retain_elem(v, kown);
                call_void(RtFn::DictSet, {kwarg_dict(), const_str(k.arg), v});
                continue;
            }
            err(k.value->line, k.value->col,
                std::format("unexpected keyword argument '{}'", k.arg));
            continue;
        }
        if (bound[idx] != kNoReg) {
            err(k.value->line, k.value->col,
                std::format("got multiple values for argument '{}'", k.arg));
            continue;
        }
        bound[idx] = expr(*k.value);
    }

    // 缺省参数：只支持字面量常量默认值
    auto lit_to_const = [](const LiteralValue& lv) {
        ConstVal cv;
        switch (lv.kind) {
            case LiteralValue::Kind::None: cv.ty = t_none(); break;
            case LiteralValue::Kind::Bool: cv.ty = t_bool(); cv.i = lv.b ? 1 : 0; break;
            case LiteralValue::Kind::Int: cv.ty = t_int(); cv.i = lv.i; break;
            case LiteralValue::Kind::Float: cv.ty = t_float(); cv.f = lv.f; break;
            case LiteralValue::Kind::Str: cv.ty = t_str(); cv.s = lv.s; break;
        }
        return cv;
    };
    auto default_of = [&](size_t pos_index) -> std::optional<ConstVal> {
        // 跨模块接口只有 default_lit，没有 AST。
        if (info && pos_index < info->pos.size() && info->pos[pos_index].default_lit)
            return lit_to_const(*info->pos[pos_index].default_lit);
        // 签名里的默认表达式优先：通过函数值 / lambda 调用时没有
        // FunctionDef 可查，只能靠 FuncInfo 带着的 AST 指针。
        if (info && pos_index < info->pos.size() && info->pos[pos_index].default_expr)
            return as_const(info->pos[pos_index].default_expr);
        if (!defn) return std::nullopt;
        size_t nd = defn->args.defaults.size();
        if (pos_index + nd < npos) return std::nullopt;
        size_t di = pos_index - (npos - nd);
        if (di >= nd) return std::nullopt;
        return as_const(defn->args.defaults[di].get());
    };
    auto emit_const = [&](const ConstVal& cv) -> VReg {
        switch (cv.ty->kind) {
            case TyKind::None: return const_none();
            case TyKind::Bool: return const_bool(cv.i != 0);
            case TyKind::Int: return const_int(cv.i);
            case TyKind::Float: return const_float(cv.f);
            case TyKind::Str: return const_str(cv.s);
            default: return const_none();
        }
    };

    for (size_t i = 0; i < npos; ++i) {
        if (bound[i] != kNoReg) continue;
        if (!info->pos[i].has_default) {
            err(c.line, c.col,
                std::format("missing required argument '{}'", info->pos[i].name));
            bound[i] = const_none();
            continue;
        }
        auto cv = default_of(i);
        if (!cv) {
            err(c.line, c.col,
                std::format("default value of '{}' is not a literal constant; "
                            "the code generator cannot fill it in yet",
                            info->pos[i].name));
            bound[i] = const_none();
            continue;
        }
        bound[i] = emit_const(*cv);
    }
    for (size_t i = 0; i < nkw; ++i) {
        size_t bi = npos + i;
        if (bound[bi] != kNoReg) continue;
        if (!info->kwonly[i].has_default) {
            err(c.line, c.col,
                std::format("missing required keyword argument '{}'", info->kwonly[i].name));
            bound[bi] = const_none();
            continue;
        }
        // kwonly 默认值：签名里带了默认值就直接用，否则回退到 def。
        std::optional<ConstVal> cv;
        if (info && i < info->kwonly.size() && info->kwonly[i].default_lit)
            cv = lit_to_const(*info->kwonly[i].default_lit);
        if (!cv && info && i < info->kwonly.size() && info->kwonly[i].default_expr)
            cv = as_const(info->kwonly[i].default_expr);
        if (!cv && defn && i < defn->args.kw_defaults.size() && defn->args.kw_defaults[i])
            cv = as_const(defn->args.kw_defaults[i].get());
        if (!cv) {
            err(c.line, c.col,
                std::format("default value of '{}' is not a literal constant", info->kwonly[i].name));
            bound[bi] = const_none();
            continue;
        }
        bound[bi] = emit_const(*cv);
    }

    args = std::move(bound);
    // 实参按形参类型做数值转换：`def f(x: float)` 传 int 进来要变成 double，
    // 否则形参槽里躺的是整数位模式。
    for (size_t i = 0; i < args.size() && i < npos + nkw; ++i) {
        TypePtr want = i < npos ? info->pos[i].ty : info->kwonly[i - npos].ty;
        if (args[i] != kNoReg) args[i] = coerce(args[i], vreg_ty(args[i]), want);
    }
    // 变长形参即使没收到实参也要传空容器：callee 的形参槽必须有值。
    if (info->varargs) args.push_back(vararg_list());
    if (info->kwargs) args.push_back(kwarg_dict());
    return args;
}

VReg Lowerer::lower_construct(Call& c, const ClassInfoPtr& cls) {
    auto cid_it = class_ids_.find(cls.get());
    if (cid_it == class_ids_.end()) {
        err(c.line, c.col,
            std::format("class '{}' is not available here (nested classes are not supported yet)",
                        cls->name));
        return const_none();
    }

    Inst in;
    in.op = IrOp::ObjNew;
    in.imm = cid_it->second;
    VReg obj = def_heap(mk_class(cls.get()), std::move(in));

    // 字段初值：从最根的类往下按布局顺序求值。放在构造点而不是
    // __init__ 里，是为了让「子类覆盖 __init__ 但父类有字段初值」
    // 这种情况也能初始化到。
    std::vector<std::pair<int, const FieldInfo*>> defaults;
    {
        std::vector<const ClassInfo*> chain;
        for (const ClassInfo* k = cls.get(); k; k = k->base) chain.push_back(k);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (auto& f : (*it)->fields) {
                if (!f.has_default || !f.default_expr) continue;
                defaults.push_back({static_cast<int>(8 * (1 + (*it)->field_index(f.name))), &f});
            }
        }
    }
    for (auto& [off, fi] : defaults) {
        VReg v = expr(*fi->default_expr);
        v = coerce(v, vreg_ty(v), fi->ty);
        store_field(fi->ty, obj, off, v);
    }

    if (cls->has_init) {
        if (auto* im = cls->method("__init__"); im && im->call_sig) {
            std::vector<VReg> args = bind_args(c, im->call_sig, im->def);
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = obj;
            vc.imm = im->slot;
            vc.args = std::move(args);
            TypePtr rt = im->call_sig->ret ? im->call_sig->ret : t_none();
            def_heap(rt, std::move(vc));
        }
    }
    return obj;
}

VReg Lowerer::lower_call(Call& c) {
    // 内建函数
    if (c.func->kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(*c.func);
        if (is_builtin(n.id)) {
            static const std::unordered_set<std::string> kBuiltins = {
                "print", "len", "int", "float", "str", "bool", "range", "next",
            };
            if (kBuiltins.count(n.id)) return lower_builtin_call(n.id, c);
        }
        // 类名直接调用 = 构造。类名不是可求值的表达式，必须在这里拦，
        // 否则会退化成 LoadGlobal 读到一个空槽。
        if (auto it = classes_.find(n.id); it != classes_.end())
            return lower_construct(c, it->second);
        if (auto it = imported_classes_.find(n.id); it != imported_classes_.end())
            return construct_imported(it->second, c, mk_class(it->second.cls.get()));
        if (auto it = native_aliases_.find(n.id); it != native_aliases_.end()) {
            if (it->second.module == "asyncio") return lower_asyncio(it->second.member, c);
            TypePtr ft = tc_.type_of(c.func.get());
            FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;
            return call_native(it->second.module, it->second.member, c, info);
        }
    }

    // 方法调用
    if (c.func->kind == ExprKind::Attribute)
        return lower_method_call(static_cast<Attribute&>(*c.func), c);

    // 普通调用：按签名重排参数
    VReg callee = expr(*c.func);
    TypePtr ft = tc_.type_of(c.func.get());
    FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;

    FunctionDef* fdef = nullptr;
    if (c.func->kind == ExprKind::Name) {
        auto it = func_defs_.find(static_cast<Name&>(*c.func).id);
        if (it != func_defs_.end()) fdef = it->second;
    }

    std::vector<VReg> args = bind_args(c, info, fdef);

    Inst in;
    in.op = IrOp::Call;
    in.a = callee;
    in.args = std::move(args);
    return def_heap(tc_.type_of(&c), std::move(in));
}

VReg Lowerer::lower_builtin_call(const std::string& name, Call& c) {
    // print 的关键字是 sep / end，得单独处理，不能走下面这条通用拦截
    if (name == "print") return lower_print(c);

    if (!c.keywords.empty()) {
        err(c.line, c.col, std::format("{}() does not take keyword arguments here", name));
    }

    if (name == "len") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "len() takes exactly one argument");
            return const_int(0);
        }
        VReg v = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::Len;
        in.a = v;
        return def(t_int(), std::move(in));
    }

    if (name == "next") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "next() takes exactly one argument");
            return const_none();
        }
        TypePtr gt = tc_.type_of(c.args[0].get());
        if (!gt || gt->kind != TyKind::Generator) {
            err(c.line, c.col, std::format("next() requires a generator, got '{}'", type_name(gt)));
            return const_none();
        }
        TypePtr et = gt->sub.empty() ? t_unknown() : gt->sub[0];
        VReg g = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::GenNext);
        in.args = {g};
        // 运行时按位返回（浮点也是整数寄存器），vreg 类型记为 unknown；
        // 堆元素才入临时栈。
        VReg v = def(t_unknown(), std::move(in));
        v = retype(v, et);
        if (ir_is_heap(et)) push_temp(v);
        return v;
    }

    if (name == "range") {
        if (c.args.empty() || c.args.size() > 3) {
            err(c.line, c.col, "range() takes 1 to 3 arguments");
            return const_none();
        }
        auto arg = [&](size_t i) {
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), t_int());
        };
        VReg start = const_int(0);
        VReg stop = arg(0);
        VReg step = const_int(1);
        if (c.args.size() >= 2) {
            start = stop;
            stop = arg(1);
        }
        if (c.args.size() == 3) step = arg(2);
        VReg v = call_runtime(RtFn::RangeNew, mk_list(t_int()), {start, stop, step});
        push_temp(v);
        return v;
    }

    if (name == "int" || name == "float" || name == "str" || name == "bool") {
        TypePtr to = name == "int"     ? t_int()
                     : name == "float" ? t_float()
                     : name == "str"   ? t_str()
                                       : t_bool();
        if (c.args.empty()) return to->kind == TyKind::Int ? const_int(0)
                                       : to->kind == TyKind::Float ? const_float(0)
                                       : to->kind == TyKind::Str   ? const_str("")
                                                                   : const_bool(false);
        if (c.args.size() > 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        return convert_to(v, tc_.type_of(c.args[0].get()), to);
    }

    err(c.line, c.col, std::format("builtin '{}' is not supported by the code generator yet", name));
    return const_none();
}

VReg Lowerer::lower_asyncio(const std::string& member, Call& c) {
    TypePtr rt = tc_.type_of(&c);
    if (!c.keywords.empty()) {
        err(c.line, c.col, std::format("asyncio.{}() does not take keyword arguments", member));
    }

    if (member == "run") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.run() takes exactly one coroutine");
            return const_none();
        }
        VReg coro = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::AsyncioRun);
        in.args = {coro};
        // 结果是协程的返回值，运行时按机器字返回（浮点也是位模式）。
        VReg v = def(t_unknown(), std::move(in));
        if (ir_is_heap(rt)) push_temp(v);
        return v;
    }

    if (member == "Future") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.Future() takes no arguments");
        VReg f = call_runtime(RtFn::AsyncioFutureNew,
                              rt ? rt : mk_future(t_unknown()), {});
        push_temp(f);
        return f;
    }

    if (member == "wait_readable" || member == "wait_writable") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("asyncio.{}() takes exactly one fd", member));
            return const_none();
        }
        VReg fd = expr(*c.args[0]);
        fd = coerce(fd, tc_.type_of(c.args[0].get()), t_int());
        VReg w = call_runtime(RtFn::AsyncioWaitFd,
                              rt ? rt : mk_coroutine(t_none()),
                              {fd, const_int(member == "wait_writable" ? 1 : 0)});
        push_temp(w);
        return w;
    }

    if (member == "create_task") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.create_task() takes exactly one coroutine");
            return const_none();
        }
        VReg coro = expr(*c.args[0]);
        VReg t = call_runtime(RtFn::AsyncioCreateTask,
                              rt ? rt : mk_task(t_unknown()), {coro});
        push_temp(t);
        return t;
    }

    if (member == "sleep") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.sleep() takes exactly one argument");
            return const_none();
        }
        VReg secs = expr(*c.args[0]);
        secs = coerce(secs, tc_.type_of(c.args[0].get()), t_float());
        VReg w = call_runtime(RtFn::AsyncioSleep,
                              rt ? rt : mk_coroutine(t_none()), {secs});
        push_temp(w);
        return w;
    }

    if (member == "gather") {
        std::vector<VReg> vals;
        for (auto& a : c.args) {
            VReg v = expr(*a);
            TypePtr vt = vreg_ty(v);
            if (ir_is_heap(vt)) rc_inc(v);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
            vals.push_back(v);
        }
        Inst lst;
        lst.op = IrOp::MakeList;
        lst.imm = 1;
        lst.args = std::move(vals);
        VReg L = def_heap(mk_list(t_unknown()), std::move(lst));
        VReg w = call_runtime(RtFn::AsyncioGather,
                              rt ? rt : mk_coroutine(mk_list(t_unknown())), {L});
        push_temp(w);
        return w;
    }

    err(c.line, c.col,
        std::format("asyncio.{} is not supported by the code generator yet", member));
    return const_none();
}

VReg Lowerer::lower_print(Call& c) {
    // print 没有对应的运行时函数，在降级期就地展开成一串输出调用。
    // sep / end 是字符串值，直接交给 PrintStr 原样写出，不再经过格式化。
    Keyword* kw_sep = nullptr;
    Keyword* kw_end = nullptr;
    for (auto& kw : c.keywords) {
        if (kw.arg.empty()) {
            err(kw.value->line, kw.value->col, "print() does not take '**kwargs'");
            continue;
        }
        if (kw.arg != "sep" && kw.arg != "end") {
            if (kw.arg == "file" || kw.arg == "flush") {
                // 语法和类型都合法，只是这里没有重定向输出流的机制。
                // 前端放行，到这里才拒，免得 check 把真实代码误杀。
                err(kw.value->line, kw.value->col,
                    std::format("print() '{}' is not supported by the code generator yet",
                                kw.arg));
                continue;
            }
            err(kw.value->line, kw.value->col,
                std::format("print() got an unexpected keyword argument '{}'", kw.arg));
            continue;
        }
        TypePtr kt = tc_.type_of(kw.value.get());
        if (kt && kt->kind != TyKind::Str && kt->kind != TyKind::Unknown) {
            err(kw.value->line, kw.value->col,
                std::format("print() '{}' must be a string, got '{}'", kw.arg, type_name(kt)));
            continue;
        }
        (kw.arg == "sep" ? kw_sep : kw_end) = &kw;
    }

    // 求值顺序按源码从左到右：sep 第一次用到时才求值，end 在参数都输出完才求值。
    VReg sep = kNoReg;
    bool sep_ready = false;
    for (size_t i = 0; i < c.args.size(); ++i) {
        if (i) {
            if (kw_sep) {
                if (!sep_ready) {
                    sep = expr(*kw_sep->value);
                    sep_ready = true;
                }
                call_void(RtFn::PrintStr, {sep});
            } else {
                call_void(RtFn::PrintSep, {});
            }
        }
        if (c.args[i]->kind == ExprKind::Starred) {
            err(c.args[i]->line, c.args[i]->col,
                "'*args' is not supported by the code generator yet");
            continue;
        }
        TypePtr at = tc_.type_of(c.args[i].get());
        VReg v = expr(*c.args[i]);
        print_value(v, at, c.args[i]->line, c.args[i]->col);
    }
    if (kw_end) call_void(RtFn::PrintStr, {expr(*kw_end->value)});
    else call_void(RtFn::PrintEnd, {});
    return const_none();
}

VReg Lowerer::lower_method_call(Attribute& at, Call& c) {
    // 内置模块的原生函数/常量。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id); mod && mod->builtin) {
            if (mod->name == "asyncio") return lower_asyncio(at.attr, c);
            auto it = mod->globals.find(at.attr);
            if (it != mod->globals.end() && it->second && it->second->kind == TyKind::Func)
                return call_native(mod->name, at.attr, c, it->second->fn);
            err(c.line, c.col,
                std::format("module attribute '{}.{}' is not callable", n->id, at.attr));
            return const_none();
        }
    }
    // m.C()：导入类的构造包装。
    if (auto* n = dynamic_cast<Name*>(at.value.get())) {
        if (auto mod = resolve_module_alias(n->id)) {
            if (auto ci = mod->classes.find(at.attr); ci != mod->classes.end())
                return construct_imported(ci->second, c, mk_class(ci->second.cls.get()));
        }
    }
    // m.f()：外部全局里存的是函数闭包，加载后按普通 call 调用。
    if (auto gi = module_member(at)) {
        TypePtr ft = mod_.globals[*gi].ty;
        FuncInfoPtr info = (ft && ft->kind == TyKind::Func) ? ft->fn : nullptr;
        if (!info) {
            err(c.line, c.col, "module attribute is not callable");
            return const_none();
        }
        VReg callee = load_global(*gi, ft);
        std::vector<VReg> args = bind_args(c, info, nullptr);
        Inst in;
        in.op = IrOp::Call;
        in.a = callee;
        in.args = std::move(args);
        TypePtr rt = info->ret ? info->ret : t_unknown();
        return def_heap(rt, std::move(in));
    }
    TypePtr raw = tc_.type_of(at.value.get());
    TypePtr recv_ty = strip_optional(raw);
    bool opt = recv_ty != raw;
    VReg recv = expr(*at.value);

    // 用户类的方法调用：从对象的 vtable 里取函数地址。槽号在编译期
    // 就定了，运行时只有一次间接跳转，不需要按名字查表。
    if (recv_ty && recv_ty->kind == TyKind::Class && recv_ty->cls) {
        if (opt) recv = guard_not_none(recv, std::format("method '{}'", at.attr));
        auto* m = recv_ty->cls->method(at.attr);
        if (!m) {
            err(c.line, c.col,
                std::format("'{}' object has no method '{}'", recv_ty->cls->name, at.attr));
            return const_none();
        }
        if (!m->call_sig) return const_none();
        std::vector<VReg> args = bind_args(c, m->call_sig, m->def);
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = recv;
        vc.imm = m->slot;
        vc.args = std::move(args);
        TypePtr rt = m->call_sig->ret ? m->call_sig->ret : t_none();
        return def_heap(rt, std::move(vc));
    }

    // 内置类型的方法表是静态的，Optional 上没有；走到这里说明没做
    // None 检查。报错比让 py23_list_append 拿到 0 更可查。
    if (opt) {
        err(c.line, c.col,
            std::format("method '{}' called on a maybe-None value; add a None check first",
                        at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Generator) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "__next__") {
            if (!c.args.empty())
                err(c.line, c.col, "generator.__next__() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenNext);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "send") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "generator.send() takes exactly one argument");
                return const_none();
            }
            VReg sv = expr(*c.args[0]);
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::GenSend);
            in.args = {recv, sv};
            VReg v = def(t_unknown(), std::move(in));
            v = retype(v, et);
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "close") {
            if (!c.args.empty())
                err(c.line, c.col, "generator.close() takes no arguments");
            call_void(RtFn::GenClose, {recv});
            return const_none();
        }
        err(c.line, c.col, std::format("'generator' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Task) {
        if (!c.args.empty()) {
            err(c.line, c.col, std::format("task.{}() takes no arguments", at.attr));
        }
        if (at.attr == "done" || at.attr == "cancel" || at.attr == "cancelled") {
            RtFn fn = at.attr == "done"       ? RtFn::AsyncioTaskDone
                      : at.attr == "cancel"   ? RtFn::AsyncioTaskCancel
                                              : RtFn::AsyncioTaskCancelled;
            return call_runtime(fn, t_bool(), {recv});
        }
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "result") {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::AsyncioTaskResult);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "exception") {
            VReg v = call_runtime(RtFn::AsyncioTaskException, mk_optional(t_str()), {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("'task' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Future) {
        if (at.attr == "done" || at.attr == "cancel" || at.attr == "cancelled") {
            if (!c.args.empty())
                err(c.line, c.col, std::format("future.{}() takes no arguments", at.attr));
            RtFn fn = at.attr == "done"       ? RtFn::FutureDone
                      : at.attr == "cancel"   ? RtFn::FutureCancel
                                              : RtFn::FutureCancelled;
            return call_runtime(fn, t_bool(), {recv});
        }
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (at.attr == "set_result") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "future.set_result() takes exactly one argument");
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            TypePtr vt = vreg_ty(v);
            if (ir_is_heap(vt)) rc_inc(v);
            else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
            call_void(RtFn::FutureSetResult, {recv, v});
            return const_none();
        }
        if (at.attr == "result") {
            if (!c.args.empty()) err(c.line, c.col, "future.result() takes no arguments");
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::FutureResult);
            in.args = {recv};
            VReg v = def(t_unknown(), std::move(in));
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "exception") {
            if (!c.args.empty()) err(c.line, c.col, "future.exception() takes no arguments");
            VReg v = call_runtime(RtFn::FutureException, mk_optional(t_str()), {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("'future' object has no method '{}'", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Dict) {
        TypePtr kt = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        TypePtr vt = recv_ty->sub.size() > 1 ? recv_ty->sub[1] : t_unknown();
        if (at.attr == "get") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "dict.get() does not take keyword arguments");
                return const_none();
            }
            if (c.args.size() != 1 && c.args.size() != 2) {
                err(c.line, c.col, "dict.get() takes 1 or 2 arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            if (c.args.size() == 1)
                return call_runtime(RtFn::DictGet, vt, {recv, key});
            VReg def = expr(*c.args[1]);
            def = coerce(def, tc_.type_of(c.args[1].get()), vt);
            return call_runtime(RtFn::DictGetOr, vt, {recv, key, def});
        }
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("dict.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        if (at.attr == "keys" || at.attr == "values" || at.attr == "items") {
            if (!c.args.empty()) {
                err(c.line, c.col, std::format("dict.{}() takes no arguments", at.attr));
                return const_none();
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::List) {
                rt = at.attr == "keys" ? mk_list(kt)
                     : at.attr == "values" ? mk_list(vt)
                                           : mk_list(mk_tuple({kt, vt}));
            }
            RtFn fn = at.attr == "keys" ? RtFn::DictKeys
                      : at.attr == "values" ? RtFn::DictValues
                                            : RtFn::DictItems;
            VReg v = call_runtime(fn, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "update") {
            if (c.args.empty()) return const_none();
            if (c.args.size() != 1) {
                err(c.line, c.col, "dict.update() takes at most one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::Dict && ot->kind != TyKind::Unknown)) {
                err(c.args[0]->line, c.args[0]->col,
                    "dict.update() expects a dict argument");
                return const_none();
            }
            call_void(RtFn::DictUpdate, {recv, other});
            return const_none();
        }
        if (at.attr == "pop") {
            if (c.args.size() != 1 && c.args.size() != 2) {
                err(c.line, c.col, "dict.pop() takes 1 or 2 arguments");
                return const_none();
            }
            VReg key = expr(*c.args[0]);
            key = coerce(key, tc_.type_of(c.args[0].get()), kt);
            VReg has = const_int(0);
            VReg def = const_int(0);
            if (c.args.size() == 2) {
                has = const_int(1);
                def = expr(*c.args[1]);
                def = coerce(def, tc_.type_of(c.args[1].get()), vt);
                // 运行时把默认值原样返回；补一份引用，让返回值统一按
                // 「新引用」进临时栈。
                retain_elem(def, own_kind(vt));
            }
            VReg v = call_runtime(RtFn::DictPop, vt, {recv, key, has, def});
            if (ir_is_heap(vt)) push_temp(v);
            return v;
        }
        if (at.attr == "popitem") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.popitem() takes no arguments");
                return const_none();
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::Tuple) rt = mk_tuple({kt, vt});
            VReg v = call_runtime(RtFn::DictPopItem, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "clear") {
            if (!c.args.empty()) {
                err(c.line, c.col, "dict.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::DictClear, {recv});
            return const_none();
        }
        err(c.line, c.col,
            std::format("dict method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::Set) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("set.{}() does not take keyword arguments", at.attr));
            return const_none();
        }
        if (at.attr == "add") {
            if (c.args.size() != 1) {
                err(c.line, c.col, "set.add() takes exactly one argument");
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            retain_elem(v, own_kind(et));
            call_void(RtFn::DictSet, {recv, v, const_int(0)});
            return const_none();
        }
        if (at.attr == "update") {
            if (c.args.empty()) return const_none();
            if (c.args.size() != 1) {
                err(c.line, c.col, "set.update() takes at most one argument");
                return const_none();
            }
            VReg other = expr(*c.args[0]);
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (!ot || (ot->kind != TyKind::Set && ot->kind != TyKind::Unknown)) {
                err(c.args[0]->line, c.args[0]->col, "set.update() expects a set argument");
                return const_none();
            }
            call_void(RtFn::DictUpdate, {recv, other});
            return const_none();
        }
        if (at.attr == "remove" || at.attr == "discard") {
            if (c.args.size() != 1) {
                err(c.line, c.col,
                    std::format("set.{}() takes exactly one argument", at.attr));
                return const_none();
            }
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            call_void(RtFn::DictRemove,
                      {recv, v, const_int(at.attr == "discard" ? 1 : 0)});
            return const_none();
        }
        if (at.attr == "pop") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.pop() takes no arguments");
                return const_none();
            }
            VReg v = call_runtime(RtFn::DictPopLast, et, {recv});
            if (ir_is_heap(et)) push_temp(v);
            return v;
        }
        if (at.attr == "clear") {
            if (!c.args.empty()) {
                err(c.line, c.col, "set.clear() takes no arguments");
                return const_none();
            }
            call_void(RtFn::DictClear, {recv});
            return const_none();
        }
        err(c.line, c.col,
            std::format("set method '{}' is not supported by the code generator yet", at.attr));
        return const_none();
    }

    if (recv_ty && recv_ty->kind == TyKind::List && at.attr == "append") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "list.append() takes exactly one argument");
            return const_none();
        }
        if (!c.keywords.empty()) {
            err(c.line, c.col, "list.append() does not take keyword arguments");
        }
        VReg v = expr(*c.args[0]);
        // 运行时不再做隐式 retain：容器元素的所有权由降级器按静态类型决定。
        retain_elem(v, list_kind(recv_ty));
        call_void(RtFn::ListAppend, {recv, v});
        return const_none();
    }
    if (recv_ty && recv_ty->kind == TyKind::List && at.attr == "pop") {
        if (!c.args.empty()) {
            err(c.line, c.col, "list.pop(index) is not supported yet");
            return const_none();
        }
        Inst len;
        len.op = IrOp::Len;
        len.a = recv;
        VReg n = def(t_int(), std::move(len));
        Inst sub;
        sub.op = IrOp::Bin;
        sub.bop = BinKind::Sub;
        sub.a = n;
        sub.b = const_int(1);
        VReg last = def(t_int(), std::move(sub));
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = recv;
        get.b = last;
        VReg v = def(et, std::move(get));
        // kind 1 的列表持有元素引用，截断时会 dec；把返回值领出来，
        // 作为新的临时值入栈，否则返回值会随截断变成悬空指针。
        if (list_kind(recv_ty) == 1 && ir_is_heap(et)) {
            rc_inc(v);
            push_temp(v);
        }
        Inst setlen;
        setlen.op = IrOp::CallRt;
        setlen.imm = static_cast<int64_t>(RtFn::ListSetLen);
        setlen.args = {recv, last};
        emit(std::move(setlen));
        return v;
    }

    err(c.line, c.col, std::format("method '{}' is not supported by the code generator yet",
                                   at.attr));
    return const_none();
}

void Lowerer::print_value(VReg v, TypePtr ty, int line, int col) {
    if (!ty) ty = t_unknown();
    switch (ty->kind) {
        case TyKind::None: call_void(RtFn::PrintNone, {}); break;
        case TyKind::Bool: call_void(RtFn::PrintBool, {v}); break;
        case TyKind::Float: call_void(RtFn::PrintF64, {v}); break;
        case TyKind::Str: call_void(RtFn::PrintStr, {v}); break;
        case TyKind::Int: call_void(RtFn::PrintI64, {v}); break;
        case TyKind::Dict: call_void(RtFn::PrintStr, {const_str("<dict object>")}); break;
        case TyKind::Set: call_void(RtFn::PrintStr, {const_str("<set object>")}); break;
        case TyKind::Optional: {
            TypePtr inner = ty->sub.empty() ? t_unknown() : ty->sub[0];
            int some = new_block("print.opt.some");
            int none = new_block("print.opt.none");
            int end = new_block("print.opt.end");
            Inst br;
            br.op = IrOp::Branch;
            br.a = v;
            br.target = some;
            br.target2 = none;
            emit(std::move(br));
            switch_to(none);
            call_void(RtFn::PrintNone, {});
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(some);
            print_value(v, inner, line, col);
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(end);
            break;
        }
        case TyKind::Class: {
            // 没有 __str__，就按 CPython 的形状打一个 `<Point object>`。
            // 类名进字符串池，和字面量走同一条路。
            std::string name = ty->cls ? ty->cls->name : "object";
            call_void(RtFn::PrintStr, {const_str("<" + name + " object>")});
            break;
        }
        default:
            // 推断不出来时按整数输出。值没有运行时标签，这里没得选；
            // 与其拒绝编译，不如让程序跑起来，类型问题留给 check。
            (void)line;
            (void)col;
            call_void(RtFn::PrintI64, {v});
            break;
    }
}

}  // namespace

LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file,
                         const LowerOptions& options) {
    LowerResult res;
    Lowerer lw(res.mod, tc, file, options);
    lw.run(ast);
    res.errors = std::move(lw.errors());
    return res;
}

}  // namespace py23
