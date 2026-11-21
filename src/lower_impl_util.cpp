// lower_impl_util.cpp —— Lowerer 的小工具成员（从 lower_impl.h 内联体移出）
#include "lower_impl.h"

namespace pyadamas {

void Lowerer::stmts(std::vector<StmtPtr>& body) {
    for (auto& s : body) stmt(*s);}

std::string Lowerer::py_type_name(const TypePtr& t) {
    if (!t) return "object";
    switch (t->kind) {
        case TyKind::None: return "NoneType";
        case TyKind::Bool: return "bool";
        case TyKind::Int:
        // 定宽整数在 Python 语义里仍是 int（type(x).__name__ == 'int'）。
        case TyKind::I8:
        case TyKind::I16:
        case TyKind::I32:
        case TyKind::U8:
        case TyKind::U16:
        case TyKind::U32:
        case TyKind::U64:
        case TyKind::Big: return "int";
        case TyKind::Float: return "float";
        case TyKind::Complex: return "complex";
        case TyKind::Str: return "str";
        case TyKind::Bytes: return "bytes";
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
        case TyKind::ClassObj: return "type";
        case TyKind::AsyncEvent: return "Event";
        case TyKind::AsyncLock: return "Lock";
        case TyKind::AsyncQueue: return "Queue";
        case TyKind::AsyncTimeout: return "Timeout";
        case TyKind::AsyncTaskGroup: return "TaskGroup";
        case TyKind::AsyncSemaphore: return "Semaphore";
        case TyKind::VThread: return "vthread";
        case TyKind::Chan: return "chan";
        case TyKind::Optional:
            return t->sub.empty() ? "object" : py_type_name(t->sub[0]);
        default: return "object";
    }}

VReg Lowerer::guard_not_none(VReg v, const std::string& what) {
    int ok = new_block("nn.ok");
    int bad = new_block("nn.bad");
    Inst br;
    br.op = IrOp::Branch;
    br.a = v;
    br.target = ok;
    br.target2 = bad;
    emit(std::move(br));
    switch_to(bad);
    call_void(raise_rt(RtFn::Raise),
              {const_str("AttributeError"), const_str("NoneType object has no " + what)});
    Inst j;
    j.op = IrOp::Jump;
    j.target = ok;
    emit(std::move(j));
    switch_to(ok);
    return v;}

std::pair<int64_t, int64_t> Lowerer::dunder_key_slots(const TypePtr& key_ty) const {
    if (!key_ty || key_ty->kind != TyKind::Class || !key_ty->cls)
        return {-1, -1};
    const MethodInfo* h = key_ty->cls->method("__hash__");
    const MethodInfo* e = key_ty->cls->method("__eq__");
    if (!h || h->kind != MethodKind::Normal || !h->call_sig || h->slot < 0)
        return {-1, -1};
    if (!e || e->kind != MethodKind::Normal || !e->call_sig || e->slot < 0)
        return {-1, -1};
    // 装饰后的方法不在 vtable 里：运行时没法按槽号调，退回机器字身份。
    if (h->decorated || e->decorated) return {-1, -1};
    return {h->slot, e->slot};}

int Lowerer::list_kind(const TypePtr& t) {
    auto one = [](const TypePtr& e) -> int {
        if (!e || e->kind == TyKind::Unknown) return 3;
        return ir_is_heap(e) ? 1 : 0;
    };
    if (!t) return 3;
    if (t->kind == TyKind::List || t->kind == TyKind::Set || t->kind == TyKind::Dict)
        return t->sub.empty() ? 3 : one(t->sub[0]);
    if (t->kind == TyKind::Tuple) {
        if (t->sub.empty()) return 3;
        int k = one(t->sub[0]);
        for (size_t i = 1; i < t->sub.size(); ++i)
            if (one(t->sub[i]) != k) return 2;
        return k;
    }
    return 2;}

int Lowerer::key_kind(const TypePtr& t) {
    if (!t) return 3;
    switch (t->kind) {
        case TyKind::I8:
        case TyKind::I16:
        case TyKind::I32:
        case TyKind::U8:
        case TyKind::U16:
        case TyKind::U32:
        case TyKind::U64:
        case TyKind::Int:
        case TyKind::Bool:
        case TyKind::None:
            return 0;  // 定宽整数按机器字比较（值已正常化，与 int 同域）
        case TyKind::Str:
            return 1;
        case TyKind::Float:
            return 2;
        // 4 = big：按值比较（运行时 pa_big_cmp）。不能复用 3「其它/未知」，
        // 那是按机器字比指针的语义。
        case TyKind::Big:
            return 4;
        default:
            return 3;
    }}

VReg Lowerer::zero_value(TypePtr T) {
    if (!T) return const_none();
    switch (T->kind) {
        case TyKind::Int:
        case TyKind::I8:
        case TyKind::I16:
        case TyKind::I32:
        case TyKind::U8:
        case TyKind::U16:
        case TyKind::U32:
        case TyKind::U64:
        case TyKind::Bool:
        case TyKind::None:
            return const_int(0);
        case TyKind::Float:
            return const_float(0.0);
        default:
            return const_none();
    }}

void Lowerer::create_cells() {
    auto* m = closure();
    if (!m) return;
    for (auto& cv : m->cells) {
        TypePtr T = cv.ty;
        TypePtr cell_ty = mk_list(T ? T : t_unknown());
        int kind = list_kind_filled(cell_ty);
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
    }}

TypePtr Lowerer::name_ty(const std::string& name) {
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
    return t_unknown();}

int Lowerer::global_idx_as(const std::string& name, TypePtr ty) {
    auto it = global_ids_.find(name);
    if (it != global_ids_.end()) {
        IRGlobal& g = mod_.globals[it->second];
        if (ty && g.ty && g.ty->kind == TyKind::Unknown) {
            g.ty = ty;
            // 之前可能以 unknown 值写过：旧值释放要按注册表判断。
            g.maybe_unknown = true;
        }
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
    return id;}

int Lowerer::global_idx(const std::string& name) {
    auto it = global_ids_.find(name);
    if (it != global_ids_.end()) return it->second;
    TypePtr ty = t_unknown();
    if (auto* ms = tc_.module_scope()) {
        auto sit = ms->vars.find(name);
        if (sit != ms->vars.end()) ty = sit->second.ty;
    }
    return global_idx_as(name, ty);}

VReg Lowerer::const_literal(const LiteralValue& v) {
    switch (v.kind) {
        case LiteralValue::Kind::None: return const_none();
        case LiteralValue::Kind::Bool: return const_bool(v.b);
        case LiteralValue::Kind::Int: return const_int(v.i);
        case LiteralValue::Kind::Float: return const_float(v.f);
        case LiteralValue::Kind::Str: return const_str(v.s);
        case LiteralValue::Kind::Bytes: return const_bytes(v.s);
        case LiteralValue::Kind::Big: return const_big(v.s);
    }
    return const_none();}

void Lowerer::retain_for_store(const TypePtr& dst, VReg v) {
    TypePtr src = vreg_ty(v);
    const bool dst_unk = !dst || dst->kind == TyKind::Unknown;
    const bool src_unk = !src || src->kind == TyKind::Unknown;
    if (ir_is_heap(dst)) {
        if (ir_is_heap(src)) rc_inc(v);
        else if (src_unk) rc_inc_unknown(v);
    } else if (ir_is_heap(src)) {
        // 目标是 unknown：不知道将来按什么类型读，保守 retain。
        rc_inc(v);
    } else if (dst_unk && src_unk) {
        // 两边都是 unknown：也必须 retain。
        //
        // 这里原来是空的 —— `u = ident([...])` 这种「无标注函数返回未知
        // 类型、存进未知槽」的赋值完全不增加引用，槽里拿到的是借来的
        // 指针（产生者那份随后被 rc_cleanup 放掉），对象回收后槽仍指向
        // 它 —— use-after-free。
        //
        // 实测（x01_only_u_root / gc02_uaf_junk）：堆元数据被写坏，报
        // malloc_consolidate(): unaligned fastbin chunk detected。
        rc_inc_unknown(v);
    }}

// P3-1b（0.328）：显式 object/Any 标注的局部槽禁止 store_slot 升级（标注
// 就是「故意留在 unknown 域」）。按标注 AST 精确判定：sym.annotated+unknown
// 不可靠——typecheck 对循环内 j: int 也会给出 annotated+unknown，误标会
// 改变 d4_primes 等基准 asm。形参的同类标记在 add_param（Param.ann_unknown）。
void Lowerer::mark_ann_unknown_slot(AnnAssign& a) {
    if (a.target->kind != ExprKind::Name ||
        a.annotation->kind != ExprKind::Name) return;
    const std::string& aid = static_cast<Name&>(*a.annotation).id;
    if (aid != "object" && aid != "Any") return;
    auto it = st_->slots.find(static_cast<Name&>(*a.target).id);
    if (it != st_->slots.end()) st_->ann_unknown_slots.insert(it->second);
}

}  // namespace pyadamas
