// lower_impl.h —— Lowerer 的实现细节（只给 lower_*.cpp 用）
//
// 降级器按职责拆成多个实现文件：驱动/预扫描、语句、表达式、调用、
// 生成器与协程。这里放共享的 FuncCtx/ClosureMeta 和 Lowerer 声明。
#pragma once

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lower.h"
#include "module.h"

namespace py23 {

// ────────────────────────── 小工具 ──────────────────────────

// 只认字面量常量。默认参数、assert 消息这些地方够用；
// 更复杂的常量表达式留给后续的常量折叠。
struct ConstVal {
    TypePtr ty;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
};

inline std::optional<ConstVal> as_const(const Expr* e) {
    if (!e || e->kind != ExprKind::Constant) return std::nullopt;
    const auto& c = static_cast<const Constant&>(*e);
    ConstVal v;
    switch (c.ck) {
        case Constant::CK::None: v.ty = t_none(); return v;
        case Constant::CK::Bool: v.ty = t_bool(); v.i = c.b ? 1 : 0; return v;
        case Constant::CK::Int: v.ty = t_int(); v.i = c.i; return v;
        case Constant::CK::Float: v.ty = t_float(); v.f = c.f; return v;
        case Constant::CK::Complex: return std::nullopt;
        case Constant::CK::Str: v.ty = t_str(); v.s = c.s; return v;
        case Constant::CK::Bytes: v.ty = t_bytes(); v.s = c.s; return v;
        case Constant::CK::Ellipsis: return std::nullopt;
    }
    return std::nullopt;
}

inline BinKind map_bin(Tok t) {
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

inline CmpKind map_cmp(CmpOp op) {
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
inline Tok aug_to_bin(Tok t) {
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
    // match 捕获要在 case 结束后继续可见：case 里绑定时同时写一份到
    // 函数帧/全局。kind: 0 局部槽 / 1 cell / 2 全局。
    struct MatchEscape {
        int kind = 0;
        int idx = -1;
        TypePtr ty;
    };
    std::unordered_map<std::string, MatchEscape> match_escape;
    std::unordered_map<std::string, TypePtr> extra_locals;
    // 闭包：本函数需要从外层拿的变量，以及被内层捕获、需要转成 cell 的局部名。
    const ClosureMeta* closure = nullptr;
    std::unordered_map<std::string, int> cell_slots;   // 名字 → 存 cell 指针的槽
    std::unordered_map<std::string, int> param_slots;  // 被捕获形参的原始值槽
    int env_slot = -1;                                 // 存闭包指针（%r10）的槽
    // @classmethod 的 cls：槽位里放的是类 id（整数，不参与 rc），表达式
    // 类型仍是那个类，所以 cls(...) 走静态构造、cls.attr 走类属性。
    std::string class_self_name;
    const ClassInfo* class_self_ty = nullptr;
    // 类体命名空间：类属性名 → 隐藏全局下标。只在降级类体语句时非空，
    // 让 `y = x + 1` 里的 x 解析到前面定义的类属性。
    std::unordered_map<std::string, int> class_body_attrs;
    // for k in range(len(a))：循环体里 a[k] 天然在界内。body 里一旦给 a/k
    // 赋值（写槽）就作废；调用等副作用由 side_effect_seq_ 统一挡。
    struct BoundsHint {
        int seq_slot = -1;
        int idx_slot = -1;
        bool valid = true;
        std::vector<std::pair<int, size_t>> patches;  // 待改成 unchecked 的位置
    };
    std::vector<BoundsHint*> bounds_hints;
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
        enum class Kind { With, AsyncWith, Finally, Handler, ExcDone, FileClose, LockRelease, TimeoutExit, TaskGroupCancel, SemRelease } kind;
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
    // (声明类, 属性名) → 模块隐藏全局下标。子类读基类属性时用同一份。
    std::unordered_map<const ClassInfo*, std::unordered_map<std::string, int>> class_attr_globals_;
    // 没有任何本模块类继承的类（跨模块继承不支持，所以这就是「终态」）：
    // 方法调用可以绕过 vtable 直接调用具体实现。
    std::unordered_set<const ClassInfo*> sealed_classes_;
    // 方法帧 → 运行时类。零参 super() 只在这个方法自己的帧里有效。
    std::unordered_map<const IRFunction*, const ClassInfo*> fn_owner_class_;
    // 降级期副作用计数：调用、del、切片赋值等会自增。for/推导式用它
    // 判断循环体是否可能改变容器长度，进而决定下标免检查是否安全。
    int side_effect_seq_ = 0;

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
        // 槽位类型是唯一真相：调用方给 unknown 时回退到槽位表，
        // 否则 call.rt 结果经临时槽后会在后端退化成整数比较。
        TypePtr actual = ty;
        if ((!actual || actual->kind == TyKind::Unknown) && slot >= 0 &&
            slot < static_cast<int>(st_->fn->slot_ty.size()))
            actual = st_->fn->slot_ty[slot];
        Inst in;
        in.op = IrOp::LoadSlot;
        in.idx = slot;
        in.ty = actual;
        return def(actual ? actual : t_unknown(), std::move(in));
    }

    VReg load_global(int gi, TypePtr ty) {
        Inst in;
        in.op = IrOp::LoadGlobal;
        in.idx = gi;
        in.ty = ty;
        return def(ty ? ty : t_unknown(), std::move(in));
    }

    void store_slot_raw(int slot, VReg v) {
        for (auto* h : st_->bounds_hints)
            if (h->seq_slot == slot || h->idx_slot == slot) h->valid = false;
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

    // 排序比较器的种类：0 int/bool、1 float、2 str、其它按机器字。
    static int sort_kind(const TypePtr& t) {
        if (!t) return 3;
        if (t->kind == TyKind::Int || t->kind == TyKind::Bool) return 0;
        if (t->kind == TyKind::Float) return 1;
        if (t->kind == TyKind::Str) return 2;
        return 3;
    }

    // 内置序列构造（list/sorted/enumerate/...）的入参规整成运行时列表：
    // str → 字符列表，dict/set → 键列表，list/tuple 原样，生成器顺序消费。
    VReg seq_as_list(Expr* e, TypePtr& elem_out);
    // 生成器物化成 list：惰性值没有 len/下标，只能顺序消费一次。
    VReg gen_to_list(VReg gen, TypePtr gen_ty);
    // 生成器流式迭代：每个元素执行一次 body(ev, elem_ty, cond_blk, end_blk)。
    // ev 是借用引用；body 里跳 cond_blk 继续下一轮、跳 end_blk 提前结束。
    void emit_gen_for_each(VReg gen, TypePtr gen_ty,
                           const std::function<void(VReg, TypePtr, int, int)>& body);

    // 槽/全局/字段按**目标类型**决定是否 retain：出口按目标类型释放，
    // 只按源类型加引用会在「unknown 值写进带标注的堆槽」时失衡
    // （源类型未知就漏掉 inc，出口却照常 dec）。unknown 源用运行时
    // 保守 inc，整数在这里是安全的 no-op。
    void retain_for_store(const TypePtr& dst, VReg v) {
        TypePtr src = vreg_ty(v);
        if (ir_is_heap(dst)) {
            if (ir_is_heap(src)) rc_inc(v);
            else if (!src || src->kind == TyKind::Unknown) rc_inc_unknown(v);
        } else if (ir_is_heap(src)) {
            // 目标槽是 unknown：只 retain 不 release（看不见的类型不敢 dec），
            // 和容器元素 kind 2 的策略一致。
            rc_inc(v);
        }
    }

    void store_slot(int slot, VReg v) {
        TypePtr dst = st_->fn->slot_ty[slot];
        TypePtr src = vreg_ty(v);
        // 未知槽位第一次收到已知类型的值时补上类型；后续 load.slot
        // 才能按 float/int 正确选指令。
        if (dst && dst->kind == TyKind::Unknown && src && src->kind != TyKind::Unknown)
            st_->fn->slot_ty[slot] = dst = src;
        retain_for_store(dst, v);
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
        retain_for_store(dst, v);
        if (ir_is_heap(dst)) {
            VReg old = load_global(gi, dst);
            store_global_raw(gi, v);
            rc_dec(old);
        } else {
            store_global_raw(gi, v);
        }
    }

    void store_field(TypePtr fty, VReg obj, int off, VReg v) {
        retain_for_store(fty, v);
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

    // 只读形参不需要「入口 retain + 出口 release」：实参在调用期间由
    // 调用方持有，函数体既没覆盖形参槽、也没把它存进别处时，这对 RC
    // 纯属多余。方法调用的 self 绝大多数是这种形态，省掉的是每次
    // 调用两条运行时调用。判定在函数降级完成后做：
    //   - 入口 retain 是入口块开头按形参顺序排的两条指令（load + rc_inc）；
    //   - 出口 release 只由 release_slots 发出，形态是 load(param) + rc_dec，
    //     只读形参意味着函数体里不会覆盖它，所以这种形态只可能是出口释放。
    // 不记录指令下标：降低过程中语句/循环的 mark 擦除会让下标失效。
    void finalize_param_borrows() {
        if (st_->is_generator) return;
        IRFunction* fn = st_->fn;
        const size_t ns = fn->slot_ty.size();
        std::vector<bool> written(ns, false);
        std::unordered_map<VReg, int> uses;
        auto touch = [&](VReg v) {
            if (v != kNoReg) uses[v]++;
        };
        for (auto& b : fn->blocks)
            for (auto& in : b.insts) {
                if ((in.op == IrOp::StoreSlot || in.op == IrOp::SlotAddr) && in.idx >= 0 &&
                    in.idx < static_cast<int>(ns))
                    written[static_cast<size_t>(in.idx)] = true;
                touch(in.a);
                touch(in.b);
                touch(in.c);
                for (VReg r : in.args) touch(r);
            }
        std::vector<bool> read_only(ns, false);
        bool any = false;
        for (auto& p : fn->params)
            if (ir_is_heap(p.ty) && p.slot >= 0 && p.slot < static_cast<int>(ns) &&
                !written[static_cast<size_t>(p.slot)]) {
                read_only[static_cast<size_t>(p.slot)] = true;
                any = true;
            }
        if (!any) return;

        std::vector<std::pair<int, size_t>> dels;
        // 入口 retain：只匹配入口块开头的连续序列，形态不符就停。
        if (!fn->blocks.empty()) {
            auto& ins = fn->blocks.front().insts;
            size_t i = 0;
            if (i < ins.size() && ins[i].op == IrOp::CallRt &&
                static_cast<RtFn>(ins[i].imm) == RtFn::RcMark)
                ++i;
            for (auto& p : fn->params) {
                if (!ir_is_heap(p.ty)) continue;
                if (i + 1 >= ins.size()) break;
                if (ins[i].op != IrOp::LoadSlot || ins[i].idx != p.slot) break;
                const Inst& inc = ins[i + 1];
                if (inc.op != IrOp::CallRt || static_cast<RtFn>(inc.imm) != RtFn::RcInc ||
                    inc.args.size() != 1 || inc.args[0] != ins[i].dst)
                    break;
                if (read_only[static_cast<size_t>(p.slot)]) {
                    dels.push_back({0, i + 1});
                    if (ins[i].dst != kNoReg && uses[ins[i].dst] == 1)
                        dels.push_back({0, i});
                }
                i += 2;
            }
        }
        // 出口 release：read-only 槽的 load + rc_dec 只可能是 release_slots。
        for (size_t b = 0; b < fn->blocks.size(); ++b) {
            auto& ins = fn->blocks[b].insts;
            for (size_t i = 0; i + 1 < ins.size(); ++i) {
                if (ins[i].op != IrOp::LoadSlot || ins[i].idx < 0 ||
                    ins[i].idx >= static_cast<int>(ns) ||
                    !read_only[static_cast<size_t>(ins[i].idx)])
                    continue;
                const Inst& dec = ins[i + 1];
                if (dec.op != IrOp::CallRt || static_cast<RtFn>(dec.imm) != RtFn::RcDec ||
                    dec.args.size() != 1 || dec.args[0] != ins[i].dst)
                    continue;
                dels.push_back({static_cast<int>(b), i + 1});
                if (ins[i].dst != kNoReg && uses[ins[i].dst] == 1)
                    dels.push_back({static_cast<int>(b), i});
                ++i;
            }
        }
        if (dels.empty()) return;
        std::ranges::sort(dels, [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second > b.second;
        });
        for (auto& [b, idx] : dels) {
            auto& ins = fn->blocks[static_cast<size_t>(b)].insts;
            if (idx < ins.size()) ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(idx));
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
        if (auto it = st_->extra_locals.find(name); it != st_->extra_locals.end())
            return it->second;
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
    void lower_for_range_obj(For& f, VReg r);
    void lower_for_range_core(For& f, VReg start, VReg stop, VReg step_v, bool dyn_step,
                               int64_t step, const Call* c);
    void lower_for_seq(For& f);
    // for i, v in enumerate(xs) / for a, b in zip(xs, ys)：直接计数循环，
    // 不物化 [下标, 值] 的元组表。
    void lower_for_enumerate(For& f, Call& c);
    void lower_for_zip(For& f, Call& c);
    void lower_for_file(For& f);
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
    // @functools.wraps(...)：没有运行期语义，装饰器链里整层跳过。
    bool is_wraps_decorator(const Expr* e) const;
    void lower_assert(Assert& a);
    void lower_delete(Delete& d);
    void lower_match(Match& m);
    void lower_pattern(Pattern& p, VReg value, TypePtr ty, int fail, bool in_or = false,
                       std::unordered_set<std::string>* or_local = nullptr);
    void delete_target(Expr& target);
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
    int compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label,
                              const ClassInfo* owner = nullptr);
    // 含 yield / await 的函数：编译 resume 状态机 + 创建包装，返回创建包装下标。
    // frame_kind: 0 生成器 / 1 协程 / 2 异步生成器
    int compile_suspendable_function(FunctionDef& f, FuncInfoPtr info, const std::string& label,
                                     int frame_kind, const ClassInfo* owner = nullptr);
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
    VReg lower_itertools(const std::string& member, Call& c);
    VReg lower_collections(const std::string& member, Call& c);
    VReg lower_functools(const std::string& member, Call& c);
    VReg lower_datetime(const std::string& member, Call& c);
    VReg lower_pathlib(const std::string& member, Call& c);
    // random 里带 rng 首参的几个函数：模块级传 0（运行时走全局 rng）。
    VReg lower_random_extra(const std::string& member, Call& c);
    VReg lower_json(const std::string& member, Call& c);
    // 把静态类型编码成 json 运行时描述符；不支持的类型就地报错。
    bool json_desc(const TypePtr& t, std::string& out, int line, int col);

    VReg lower_construct(Call& c, const ClassInfoPtr& cls);

    // 按签名重排实参：位置、关键字、默认值、数值转换。
    std::vector<VReg> bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* defn);

    // 容器元素跨数值域（list[int] ↔ list[float]）需要逐元素转换，
    // 现阶段一律报错，避免按错误的机器表示读槽位。
    bool bad_elem(const TypePtr& from, const TypePtr& to);

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

    // type(x) 用的 Python 侧类型名。
    static std::string py_type_name(const TypePtr& t) {
        if (!t) return "object";
        switch (t->kind) {
            case TyKind::None: return "NoneType";
            case TyKind::Bool: return "bool";
            case TyKind::Int: return "int";
            case TyKind::Float: return "float";
            case TyKind::Complex: return "complex";
            case TyKind::Str: return "str";
            case TyKind::List: return "list";
            case TyKind::Range: return "range";
            case TyKind::Set: return "set";
            case TyKind::Dict: return "dict";
            case TyKind::Tuple: return "tuple";
            case TyKind::Func: return "function";
            case TyKind::Generator: return "generator";
            case TyKind::Coroutine: return "coroutine";
            case TyKind::Task: return "task";
            case TyKind::AsyncGenerator: return "async_generator";
            case TyKind::Future: return "future";
            case TyKind::File: return "file";
            case TyKind::Random: return "random.Random";
            case TyKind::CompletedProcess: return "subprocess.CompletedProcess";
            case TyKind::Exception: return t->name.empty() ? "Exception" : t->name;
            case TyKind::Class: return t->cls ? t->cls->name : "object";
            case TyKind::TypeObj: return "type";
            case TyKind::AsyncEvent: return "Event";
            case TyKind::AsyncLock: return "Lock";
            case TyKind::AsyncQueue: return "Queue";
            case TyKind::AsyncTimeout: return "Timeout";
            case TyKind::AsyncTaskGroup: return "TaskGroup";
            case TyKind::AsyncSemaphore: return "Semaphore";
            case TyKind::Optional:
                return t->sub.empty() ? "object" : py_type_name(t->sub[0]);
            default: return "object";
        }
    }

    void assign_to(Expr& target, VReg v, TypePtr vty);

    // 类属性：每个 (声明类, 名字) 在模块里占一个隐藏全局，子类共用同一份。
    struct ClassAttrStorage {
        int slot = -1;    // 存储槽
        int flag = -1;    // 子类「已拥有自己一份」的标志；-1 表示无需判断
        int parent = -1;  // 标志为 0 时读哪里
    };
    int class_attr_global(const ClassInfo* owner, const std::string& name, TypePtr ty);
    ClassAttrStorage class_attr_storage(const ClassInfo* recv, const ClassAttrInfo* ca);
    int class_attr_slot(const ClassInfo* recv, const ClassAttrInfo* ca);
    VReg class_attr_load(const ClassInfo* recv, const ClassAttrInfo* ca);
    VReg class_attr_load_dyn(VReg obj, const ClassInfo* stat, const ClassAttrInfo* ca);
    void class_attr_store(const ClassInfo* recv, const ClassAttrInfo* ca, VReg v, TypePtr vty);
    // 接收者是类本身（`C.x` / @classmethod 的 `cls.x`）而不是实例
    bool is_class_receiver(const Attribute& a);
    const ClassInfo* class_receiver_cls(const Attribute& a, TypePtr ot);

    // ── 表达式 ──────────────────────────────────────────────────

    VReg expr(Expr& e);

    VReg lower_constant(Constant& c);
    VReg lower_binop(BinOp& b);
    VReg lower_unary(UnaryOp& u);
    VReg lower_boolop(BoolOp& b);
    VReg lower_compare(Compare& c);
    // 容器相等：list/tuple 逐元素比，dict 按键值对（顺序无关），set 按键。
    // 元素类型在编译期已知，所以比较方式也能定下来；不适用时返回 kNoReg。
    VReg lower_container_eq(VReg l, TypePtr lt, VReg r, TypePtr rt);
    // 容器元素的比较链：每个字符说明该层元素怎么比（i/s/f/x/l/d/t）。
    void append_elem_chain(TypePtr container, std::string& chain);
    VReg lower_call(Call& c);
    VReg lower_builtin_call(const std::string& name, Call& c);
    VReg lower_minmax(Call& c, bool is_min);
    VReg lower_sum(Call& c);
    VReg lower_anyall(Call& c, bool is_any);
    VReg lower_print(Call& c);
    VReg lower_method_call(Attribute& at, Call& c);
    VReg lower_super_call(Attribute& at, Call& c);
    // 协议 dunder：__str__/__repr__/__eq__/__ne__/__len__。
    // 静态类型是用户类且 MRO 里有可用实现时构造 VCall（子类覆盖生效），
    // 否则返回 kNoReg，由调用点退回默认行为。
    VReg dunder_call(VReg recv, TypePtr recv_ty, const char* name,
                     std::vector<VReg> args, TypePtr want_ret,
                     const std::vector<TypePtr>* arg_tys = nullptr);
    VReg dunder_str(VReg v, TypePtr ty, bool prefer_repr);
    VReg dunder_len(VReg v, TypePtr ty);
    VReg lower_dunder_eq(VReg l, TypePtr lt, VReg r, TypePtr rt, bool ne);
    // 类作为 dict/set key：__hash__ 和 __eq__ 都存在时返回两个 vtable
    // 槽号，否则 (-1, -1)（运行时按机器字哈希/比较）。
    std::pair<int64_t, int64_t> dunder_key_slots(const TypePtr& key_ty) const {
        if (!key_ty || key_ty->kind != TyKind::Class || !key_ty->cls)
            return {-1, -1};
        const MethodInfo* h = key_ty->cls->method("__hash__");
        const MethodInfo* e = key_ty->cls->method("__eq__");
        if (!h || h->kind != MethodKind::Normal || !h->call_sig || h->slot < 0)
            return {-1, -1};
        if (!e || e->kind != MethodKind::Normal || !e->call_sig || e->slot < 0)
            return {-1, -1};
        return {h->slot, e->slot};
    }
    // 算术/比较 dunder：左操作数（或右操作数的反射版）有实现时返回调用
    // 结果；没有可用实现返回 kNoReg。aug=true 时先试 __iadd__ 这类。
    VReg lower_dunder_binop(VReg l, TypePtr lt, VReg r, TypePtr rt, Tok op, bool aug);
    VReg lower_dunder_cmp(VReg l, TypePtr lt, VReg r, TypePtr rt, CmpOp op);
    // 真值：类实例有 __len__ 时返回 len(...) != 0 的 bool vreg，
    // 否则原样返回（后端按类型做指针/数值判断）。
    VReg truth_bool(VReg v, TypePtr ty);
    VReg class_default_str(TypePtr ty);
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
    // print(..., file=f)：值先转成字符串再 FileWrite；print_file_slot_ >= 0
    // 时由 print_value 分派到这里。
    void print_value_to_file(VReg v, TypePtr ty, int line, int col);
    // 列表/元组：按静态元素类型逐个打印，元素递归走上面的两条路径。
    void print_seq(VReg v, TypePtr ty, bool tuple, bool to_file, int line, int col);
    // 容器里的元素按 repr 语义打印（CPython 打印容器用元素 repr）。
    void print_elem(VReg ev, TypePtr et, bool to_file, int line, int col);
    // 遍历一个列表逐元素打印，元素之间用 ", " 分隔；回调拿到元素和下标。
    void print_each(VReg list, TypePtr et, bool to_file,
                    const std::function<void(VReg, VReg)>& body);
    // dict：{key: value, ...}；set：{elem, ...}。都按 repr 语义。
    void print_dict(VReg v, TypePtr ty, bool to_file, int line, int col);
    // 空容器打印 empty_text，非空但内容看不清时打印 placeholder。
    void print_empty_or_placeholder(VReg v, const std::string& empty_text,
                                     const std::string& placeholder, bool to_file);
    void print_set(VReg v, TypePtr ty, bool to_file, int line, int col);
    // 往 stdout 或 print(..., file=f) 写一段字面量。
    void print_write(const std::string& s, bool to_file);
    int print_file_slot_ = -1;

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


}  // namespace py23
