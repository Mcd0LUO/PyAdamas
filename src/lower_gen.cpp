// lower_gen.cpp —— 生成器与协程：yield/await、状态机、跨挂起清理
#include "lower_impl.h"

namespace py23 {

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


}  // namespace py23
