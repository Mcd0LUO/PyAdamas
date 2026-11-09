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
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

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
};

class Lowerer {
public:
    Lowerer(IRModule& m, TypeChecker& tc, std::string file)
        : mod_(m), tc_(tc), file_(std::move(file)) {}

    void run(Module& ast);

    std::vector<LowerError>& errors() { return errors_; }

private:
    IRModule& mod_;
    TypeChecker& tc_;
    std::string file_;
    std::vector<LowerError> errors_;

    std::vector<std::unique_ptr<FuncCtx>> frames_;
    FuncCtx* st_ = nullptr;

    std::unordered_map<std::string, int> global_ids_;
    std::unordered_map<std::string, FunctionDef*> func_defs_;
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
    //（类型看不见，销毁时不敢按元素 dec），代价是泄漏。
    void retain_elem(VReg v, int kind) {
        if (kind != 0 && ir_is_heap(vreg_ty(v))) rc_inc(v);
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
        int id = static_cast<int>(mod_.globals.size());
        mod_.globals.push_back(IRGlobal{name, ty});
        global_ids_[name] = id;
        return id;
    }

    // ── 闭包 ────────────────────────────────────────────────────

    const ClosureMeta* closure() const { return st_ ? st_->closure : nullptr; }

    bool is_cell(const std::string& name) const {
        return st_ && st_->cell_slots.count(name) > 0;
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
    void lower_funcdef(FunctionDef& f);
    void lower_assert(Assert& a);
    void lower_try(Try& t);

    // ── 类 ──────────────────────────────────────────────────────

    void collect_classes(std::vector<StmtPtr>& body);
    void lower_classdef(ClassDef& c);

    // 把一个函数体编译成 IRFunction，返回它的下标。普通函数和方法共用。
    int compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label);

    VReg lower_construct(Call& c, const ClassInfoPtr& cls);

    // 按签名重排实参：位置、关键字、默认值、数值转换。
    std::vector<VReg> bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* def);

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
        call_void(RtFn::Panic, {const_str("NoneType object has no " + what)});
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
            for (auto& a : static_cast<Import&>(s).names)
                global_idx(a.asname.empty() ? a.name : a.asname);
            break;
        case StmtKind::ImportFrom:
            for (auto& a : static_cast<ImportFrom&>(s).names)
                global_idx(a.asname.empty() ? a.name : a.asname);
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
    hoist_globals(ast.body);
    collect_funcdefs(ast.body);
    for (auto* root : func_roots_) compute_closure_meta(root);
    classes_ = tc_.classes();
    collect_classes(ast.body);

    IRFunction* entry = mod_.add_func("__py23_main");
    entry->ret = t_int();
    mod_.entry = static_cast<int>(mod_.funcs.size()) - 1;

    push_frame(entry, tc_.module_scope());
    int b = new_block("entry");
    switch_to(b);
    st_->is_entry = true;
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    stmts(ast.body);

    if (!terminated()) emit_return(const_int(0), t_int());
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
    VReg stmt_mark = rc_mark_at(mark_block, mark_pos);
    size_t pushes_before = st_->rc_pushes;
    switch (s.kind) {
        case StmtKind::Expr: {
            expr(*static_cast<ExprStmt&>(s).value);
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<Assign&>(s);
            VReg v = expr(*a.value);
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
            if (typed_literal && a.value->kind == ExprKind::List)
                v = lower_list_like(static_cast<ListExpr&>(*a.value).elts, want, false);
            else if (typed_literal && a.value->kind == ExprKind::Tuple)
                v = lower_list_like(static_cast<TupleExpr&>(*a.value).elts, want, true);
            else
                v = expr(*a.value);
            assign_to(*a.target, v, want ? want : vt);
            break;
        }
        case StmtKind::Return: {
            auto& r = static_cast<Return&>(s);
            VReg v = r.value ? expr(*r.value) : kNoReg;
            // `def f() -> float: return 1` 要在返回处转成 double
            if (v != kNoReg && st_->fn->ret) v = coerce(v, vreg_ty(v), st_->fn->ret);
            emit_return(v, v != kNoReg ? vreg_ty(v) : nullptr);
            break;
        }
        case StmtKind::Break: {
            if (st_->loops.empty()) {
                unsupported(s, "'break'");
                break;
            }
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
        case StmtKind::Raise: unsupported(s, "'raise'"); break;
        case StmtKind::With: unsupported(s, "'with'"); break;
        case StmtKind::ClassDef: lower_classdef(static_cast<ClassDef&>(s)); break;
        case StmtKind::Import: unsupported(s, "'import'"); break;
        case StmtKind::ImportFrom: unsupported(s, "'from ... import'"); break;
        case StmtKind::Global: {
            for (auto& n : static_cast<Global&>(s).names) st_->globals_decl.insert(n);
            break;
        }
        case StmtKind::Nonlocal: break;  // 名字解析已经由 cell/env 接管
    }
    if (st_->rc_pushes != pushes_before) {
        if (!terminated()) rc_cleanup(stmt_mark);
    } else {
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
    VReg loop_mark = rc_mark_at(loop_mark_block, loop_mark_pos);

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
    rc_cleanup(loop_mark);
    st_->loops.push_back({end, cond});
    stmts(f.body);
    st_->loops.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = cond;
        emit(std::move(jj));
    }

    switch_to(orelse);
    rc_cleanup(loop_mark);
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
    if (st_->rc_pushes == pushes_before_cond) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

void Lowerer::lower_for(For& f) {
    if (f.is_async) {
        unsupported(f, "'async for'");
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

    int cond = new_block("for.cond");
    int body = new_block("for.body");
    int incr = new_block("for.incr");
    int end = new_block("for.end");
    int orelse = f.orelse.empty() ? end : new_block("for.else");

    int loop_mark_block = -1;
    size_t loop_mark_pos = 0;
    VReg loop_mark = rc_mark_at(loop_mark_block, loop_mark_pos);
    size_t pushes_before_loop = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    VReg cv = load_slot(ctr, t_int());
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = step > 0 ? CmpKind::Lt : CmpKind::Gt;
    cmp.a = cv;
    cmp.b = stop;
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    rc_cleanup(loop_mark);
    VReg iv = load_slot(ctr, t_int());
    assign_to(*f.target, iv, t_int());
    st_->loops.push_back({end, incr});
    stmts(f.body);
    st_->loops.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    rc_cleanup(loop_mark);
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
    rc_cleanup(loop_mark);
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
    rc_cleanup(loop_mark);
    if (st_->rc_pushes == pushes_before_loop) {
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
    VReg loop_mark = rc_mark_at(loop_mark_block, loop_mark_pos);
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
    cmp.b = n;
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    rc_cleanup(loop_mark);
    VReg iv2 = load_slot(idx, t_int());
    TypePtr et = tc_.type_of(f.target.get());
    if (!et || et->kind == TyKind::Unknown) {
        if (seq_ty->kind == TyKind::Str)
            et = t_str();
        else if (!seq_ty->sub.empty())
            et = seq_ty->sub[0];
    }
    VReg ev;
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set) {
        // 字典/集合按插入顺序按下标取 key；返回的是容器内部的借用引用。
        ev = call_runtime(RtFn::DictKeyAt, et, {seq, iv2});
    } else {
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = seq;
        get.b = iv2;
        // 遍历字符串时每个下标是 py23_str_char_at 新造的单字节串；
        // 遍历列表/元组时是容器内部的借用引用。
        ev = seq_ty->kind == TyKind::Str ? def_heap(et, std::move(get))
                                         : def(et, std::move(get));
    }
    assign_to(*f.target, ev, et);

    st_->loops.push_back({end, incr});
    stmts(f.body);
    st_->loops.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    rc_cleanup(loop_mark);
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
    rc_cleanup(loop_mark);
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
    rc_cleanup(loop_mark);
    if (st_->rc_pushes == pushes_before_loop) {
        erase_cleanup(body, loop_mark);
        erase_cleanup(incr, loop_mark);
        erase_cleanup(orelse, loop_mark);
        erase_cleanup(end, loop_mark);
        erase_mark(loop_mark_block, loop_mark_pos, loop_mark);
    }
}

int Lowerer::compile_function_body(FunctionDef& f, FuncInfoPtr info, const std::string& label) {
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

void Lowerer::lower_funcdef(FunctionDef& f) {
    if (!f.decorators.empty()) {
        unsupported(f, "decorator");
        return;
    }
    if (f.is_async) {
        unsupported(f, "'async def'");
        return;
    }

    FuncInfoPtr info = tc_.func_info(&f);
    if (info && (info->varargs || info->kwargs)) {
        err(f.line, f.col, "*args / **kwargs parameters are not supported by the code generator yet");
        return;
    }

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
}

void Lowerer::lower_classdef(ClassDef& c) {
    auto it = classes_.find(c.name);
    if (it == classes_.end()) return;
    const ClassInfoPtr& ci = it->second;
    auto cid_it = class_ids_.find(ci.get());
    if (cid_it == class_ids_.end()) {
        unsupported(c, "'class' inside a function");
        return;
    }
    int cid = cid_it->second;

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
        if (m.def->is_async) {
            err(m.def->line, m.def->col, "'async def' methods are not supported yet");
            continue;
        }
        if (m.sig && (m.sig->varargs || m.sig->kwargs)) {
            err(m.def->line, m.def->col,
                "*args / **kwargs parameters are not supported by the code generator yet");
            continue;
        }
        std::string label = std::format("{}_{}_{}", c.name, m.name, mod_.funcs.size());
        int fidx = compile_function_body(*m.def, m.sig, label);
        if (m.slot >= 0 && m.slot < static_cast<int>(mod_.classes[cid].vtable.size()))
            mod_.classes[cid].vtable[static_cast<size_t>(m.slot)] = fidx;
    }
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
    call_void(RtFn::Panic, {msg});
    Inst jj;
    jj.op = IrOp::Jump;
    jj.target = ok;
    emit(std::move(jj));

    switch_to(ok);
}

void Lowerer::lower_try(Try& t) {
    // 异常机制还没做。try/finally 在无异常时语义等价于顺序执行，
    // 只有 finally 里的 return/break 会改变控制流，这里先只放行最简单的情形。
    if (!t.handlers.empty()) {
        unsupported(t, "'try/except'");
        return;
    }
    stmts(t.body);
    stmts(t.finalbody);
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
                err(target.line, target.col,
                    "slice assignment is not supported by the code generator yet");
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
        case ExprKind::IfExp: return lower_ifexp(static_cast<IfExp&>(e));
        case ExprKind::JoinedStr: return lower_joined(static_cast<JoinedStr&>(e));
        case ExprKind::NamedExpr: {
            auto& n = static_cast<NamedExpr&>(e);
            VReg v = expr(*n.value);
            assign_to(*n.target, v, tc_.type_of(n.value.get()));
            return v;
        }
        case ExprKind::FormattedValue: {
            // 顶层 FormattedValue 只会出现在 f-string 内部，单独出现说明
            // 上游构造有问题；按表达式处理即可。
            auto& fv = static_cast<FormattedValue&>(e);
            return convert_to(expr(*fv.value), tc_.type_of(fv.value.get()), t_str());
        }
        case ExprKind::Attribute: {
            auto& a = static_cast<Attribute&>(e);
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
std::vector<VReg> Lowerer::bind_args(Call& c, const FuncInfoPtr& info, FunctionDef* def) {
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

    if (c.args.size() > npos) {
        err(c.line, c.col,
            std::format("too many positional arguments (expected at most {})", npos));
    }
    for (size_t i = 0; i < c.args.size() && i < npos; ++i) {
        if (c.args[i]->kind == ExprKind::Starred) {
            err(c.args[i]->line, c.args[i]->col,
                "'*args' is not supported by the code generator yet");
            continue;
        }
        bound[i] = expr(*c.args[i]);
    }
    for (auto& k : c.keywords) {
        int idx = -1;
        for (size_t i = info->n_posonly; i < npos; ++i)
            if (info->pos[i].name == k.arg) idx = static_cast<int>(i);
        if (idx < 0) {
            for (size_t i = 0; i < nkw; ++i)
                if (info->kwonly[i].name == k.arg) idx = static_cast<int>(npos + i);
        }
        if (idx < 0) {
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
    auto default_of = [&](size_t pos_index) -> std::optional<ConstVal> {
        // 签名里的默认表达式优先：通过函数值 / lambda 调用时没有
        // FunctionDef 可查，只能靠 FuncInfo 带着的 AST 指针。
        if (info && pos_index < info->pos.size() && info->pos[pos_index].default_expr)
            return as_const(info->pos[pos_index].default_expr);
        if (!def) return std::nullopt;
        size_t nd = def->args.defaults.size();
        if (pos_index + nd < npos) return std::nullopt;
        size_t di = pos_index - (npos - nd);
        if (di >= nd) return std::nullopt;
        return as_const(def->args.defaults[di].get());
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
        // kwonly 默认值：签名里带了默认表达式就直接用，否则回退到 def。
        std::optional<ConstVal> cv;
        if (info && i < info->kwonly.size() && info->kwonly[i].default_expr)
            cv = as_const(info->kwonly[i].default_expr);
        if (!cv && def && i < def->args.kw_defaults.size() && def->args.kw_defaults[i])
            cv = as_const(def->args.kw_defaults[i].get());
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
    if (!c.keywords.empty()) {
        for (auto& k : c.keywords) {
            if (k.arg.empty()) {
                err(c.line, c.col, "'**kwargs' is not supported by the code generator yet");
                return const_none();
            }
        }
    }

    // 内建函数
    if (c.func->kind == ExprKind::Name) {
        auto& n = static_cast<Name&>(*c.func);
        if (is_builtin(n.id)) {
            static const std::unordered_set<std::string> kBuiltins = {
                "print", "len", "int", "float", "str", "bool",
            };
            if (kBuiltins.count(n.id)) return lower_builtin_call(n.id, c);
        }
        // 类名直接调用 = 构造。类名不是可求值的表达式，必须在这里拦，
        // 否则会退化成 LoadGlobal 读到一个空槽。
        if (auto it = classes_.find(n.id); it != classes_.end())
            return lower_construct(c, it->second);
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

LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file) {
    LowerResult res;
    Lowerer lw(res.mod, tc, file);
    lw.run(ast);
    res.errors = std::move(lw.errors());
    return res;
}

}  // namespace py23
