// lower/vthread.cpp —— 虚拟线程先验语法降级（go / join / sleep / make_chan / chan 方法）
//
// 设计：docs/vthread-design.md。go f(args) 合成一个包装闭包（cell 捕获 f 与
// 全部实参），交给 pa_vt_spawn；包装体在 vthread 自己的 mmap 栈上跑，异常
// 逃出由运行时跳板捕获（setjmp + 零开销 catch-all LP 两条路径），join 重抛。
// 被调函数不需要任何标注——无色。
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::lower_go(Expr& e) {
    auto& g = static_cast<GoExpr&>(e);
    auto* call = dynamic_cast<Call*>(g.call.get());
    if (!call) {
        err(e.line, e.col, "go expects a function call, e.g. 'go f(args)'");
        return const_none();
    }
    TypePtr fty = tc_.type_of(call->func.get());
    FuncInfoPtr fn = (fty && fty->kind == TyKind::Func) ? fty->fn : nullptr;
    if (!fn || fn->unchecked) {
        err(e.line, e.col, "go expects a user-defined function with a known signature");
        return const_none();
    }
    // 求值顺序与普通调用一致：先 f，再从左到右实参。
    VReg fnv = expr(*call->func);
    std::vector<VReg> vals;
    std::vector<TypePtr> tys;
    size_t npos = fn->pos.size();
    for (size_t i = 0; i < call->args.size(); ++i) {
        TypePtr want = i < npos ? fn->pos[i].ty : fn->kwonly[i - npos].ty;
        VReg v = expr(*call->args[i]);
        v = coerce(v, tc_.type_of(call->args[i].get()), want);
        vals.push_back(v);
        tys.push_back(want ? want : t_unknown());
    }

    // 包装函数：无显式形参，env cell = [f, a0, a1, …]。体 = 调 f、丢弃返回值。
    // 与 build_lru_wrapper / build_partial_wrapper 同款合成形态。
    std::string label = std::format("__pa_go_{}", mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = t_none();
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);

    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    auto read_cap = [&](int cap, TypePtr T) {
        TypePtr cell_ty = mk_list(T ? T : t_unknown());
        VReg env = load_slot(es, t_unknown());
        VReg cell = field_load(env, 16 + 8 * cap, cell_ty);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = cell;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    };

    VReg callee = read_cap(0, fty);
    std::vector<VReg> call_args;
    call_args.reserve(vals.size());
    for (size_t i = 0; i < vals.size(); ++i)
        call_args.push_back(read_cap(static_cast<int>(i) + 1, tys[i]));
    Inst ci;
    ci.op = IrOp::Call;
    ci.a = callee;
    ci.args = std::move(call_args);
    TypePtr rty = fn->ret ? fn->ret : t_unknown();
    VReg r = def(rty, std::move(ci));
    // go 丢弃返回值：callee 交出的那份引用在这里释放。
    if (ir_is_heap(rty)) rc_dec(r);
    else if (!rty || rty->kind == TyKind::Unknown) call_void(RtFn::RcDecUnknown, {r});
    emit_return(const_none(), t_none());
    pop_frame();

    // 闭包 + cell（cell 是单元素 list，闭包析构统一 dec）。
    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(RtFn::ClosureNew, fty,
                                {code, const_int(static_cast<int64_t>(1 + vals.size()))});
    push_temp(closure);
    VReg fcell = make_cell_value(fnv, fty);
    rc_inc(fcell);
    field_store(closure, 16, fcell);
    for (size_t i = 0; i < vals.size(); ++i) {
        VReg cell = make_cell_value(vals[i], tys[i]);
        rc_inc(cell);
        field_store(closure, 16 + 8 * (static_cast<int>(i) + 1), cell);
    }

    // spawn 接管闭包引用；handle 是新堆值（rc=1 归调用方）。
    Inst si;
    si.op = IrOp::CallRt;
    si.imm = static_cast<int64_t>(RtFn::VtSpawn);
    si.args = {closure};
    VReg h = def_heap(mk_vthread(), std::move(si));
    rc_temp_take(closure);
    return h;
}

VReg Lowerer::lower_builtin_vthread(const std::string& name, Call& c) {
    if (name == "join") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "join() expects exactly one vthread handle");
            return const_none();
        }
        VReg h = expr(*c.args[0]);
        call_void(RtFn::VtJoin, {h});
        return const_none();
    }
    if (name == "sleep") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "sleep() expects exactly one millisecond count");
            return const_none();
        }
        VReg ms = expr(*c.args[0]);
        call_void(RtFn::VtSleep, {ms});
        return const_none();
    }
    if (name == "make_chan") {
        if (c.args.size() != 2 || !c.keywords.empty()) {
            err(c.line, c.col, "make_chan() expects (elem_type, capacity)");
            return const_none();
        }
        auto* nm = dynamic_cast<Name*>(c.args[0].get());
        std::string tn = nm ? nm->id : "";
        int kind;
        TypePtr elem;
        if (tn == "int" || tn == "bool") { kind = 0; elem = t_int(); }
        else if (tn == "float") { kind = 0; elem = t_float(); }
        else if (tn == "str") { kind = 1; elem = t_str(); }
        else { kind = 2; elem = t_unknown(); }
        VReg cap = expr(*c.args[1]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ChanNew);
        in.args = {const_int(kind), cap};
        return def_heap(mk_chan(elem), std::move(in));
    }
    return kNoReg;
}

VReg Lowerer::lower_method_chan(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
    TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
    if (at.attr == "send") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "send() expects exactly one value");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        v = coerce(v, tc_.type_of(c.args[0].get()), et);
        if (et && et->kind == TyKind::Float) {
            call_void(RtFn::ChanSendF, {recv, v});
            return const_none();
        }
        // 通道接管一份引用：heap/unknown 先 retain，标量裸字转移。
        retain_elem(v, own_kind(et));
        call_void(RtFn::ChanSend, {recv, v});
        return const_none();
    }
    if (at.attr == "recv") {
        if (!c.args.empty() || !c.keywords.empty()) {
            err(c.line, c.col, "recv() takes no arguments");
            return const_none();
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.args = {recv};
        if (et && et->kind == TyKind::Float) {
            in.imm = static_cast<int64_t>(RtFn::ChanRecvF);
            return call_runtime(RtFn::ChanRecvF, t_float(), {recv});
        }
        if (ir_is_heap(et)) {
            in.imm = static_cast<int64_t>(RtFn::ChanRecv);
            return def_heap(et, std::move(in));
        }
        if (!et || et->kind == TyKind::Unknown) {
            // unknown 可能是堆指针：按注册表探针入临时栈（整数安全）。
            in.imm = static_cast<int64_t>(RtFn::ChanRecv);
            VReg v = def(t_unknown(), std::move(in));
            ++st_->rc_pushes;
            call_void(RtFn::RcTempUnknown, {v});
            return v;
        }
        in.imm = static_cast<int64_t>(RtFn::ChanRecv);
        return def(et, std::move(in));
    }
    err(c.line, c.col, std::format("'chan' object has no method '{}'", at.attr));
    return const_none();
}

}  // namespace pyadamas
