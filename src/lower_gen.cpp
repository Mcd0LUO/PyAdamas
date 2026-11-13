// lower_gen.cpp —— 生成器与协程：yield/await、状态机、跨挂起清理
#include "lower_impl.h"

namespace pyadamas {

// ────────────────────────── 生成器 / 协程 ──────────────────────────

void Lowerer::cleanup_gen_mark() {
    if (st_->gen_mark_slot < 0) return;
    rc_cleanup(load_slot(st_->gen_mark_slot, t_int()));
}

VReg Lowerer::slot_addr_of(int slot) {
    Inst in;
    in.op = IrOp::SlotAddr;
    in.idx = slot;
    return def(t_unknown(), std::move(in));
}

void Lowerer::exc_enter() {
    if (st_->exc_cur_slot < 0) return;
    VReg c = slot_addr_of(st_->exc_cur_slot);
    VReg t = slot_addr_of(st_->exc_top_slot);
    VReg tok = call_runtime(RtFn::ExcEnter, t_int(), {c, t});
    store_slot(st_->exc_tok_slot, tok);
}

void Lowerer::exc_leave() {
    if (st_->exc_cur_slot < 0) return;
    VReg c = slot_addr_of(st_->exc_cur_slot);
    VReg t = slot_addr_of(st_->exc_top_slot);
    VReg tok = load_slot(st_->exc_tok_slot, t_int());
    call_void(RtFn::ExcLeave, {c, t, tok});
}

void Lowerer::gen_finish(VReg v, TypePtr vty) {
    // 协程的返回值放进 out，由事件循环取走；生成器忽略。
    if (v != kNoReg && ir_is_heap(vty)) rc_inc(v);
    cleanup_gen_mark();
    release_slots();
    exc_leave();
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
    if (reject_suspend_in_close()) return const_none();
    auto& yn = static_cast<Yield&>(y);
    VReg v = yn.value ? expr(*yn.value) : const_none();
    TypePtr vt = yn.value ? tc_.type_of(yn.value.get()) : t_none();
    return emit_yield(v, vt);
}

// close 通过重放 finally 体来清理；体里还有挂起点时 CPython 会报
// RuntimeError: generator ignored GeneratorExit，而不是真的再挂起一次。
bool Lowerer::reject_suspend_in_close() {
    if (close_replay_depth_ <= 0) return false;
    const char* what = st_->is_async_generator ? "async generator ignored GeneratorExit"
                       : st_->is_coroutine     ? "coroutine ignored GeneratorExit"
                                               : "generator ignored GeneratorExit";
    call_void(RtFn::Raise, {const_str("RuntimeError"), const_str(what)});
    return true;
}

VReg Lowerer::emit_yield(VReg v, TypePtr vt) {
    // 帧里的 out 槽接管一个引用；未知类型按注册表保守 retain。
    if (ir_is_heap(vt)) rc_inc(v);
    else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
    // 挂起期间调用方继续跑，异常上下文要换回调用方那份。
    exc_leave();
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
            c.kind == FuncCtx::Cleanup::Kind::Handler ||
            c.kind == FuncCtx::Cleanup::Kind::LockRelease ||
            c.kind == FuncCtx::Cleanup::Kind::TimeoutExit ||
            c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel ||
            c.kind == FuncCtx::Cleanup::Kind::SemRelease)
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
    if (reject_suspend_in_close()) return const_none();
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
        // 子生成器本次的结果：send/throw 两条路都写它，yield 和结束共用。
        int ev_slot = new_temp_slot(et);
        // 待转发给子生成器的异常（PyAdamasException*，非堆对象）。throw
        // 分发把它写在这里，cond 看到非 0 就改走 GenThrowOpt。
        int exc_slot = new_temp_slot(t_int());
        store_slot(exc_slot, const_int(0));

        // 调用返回的是新引用：入临时栈接管这个引用（与原来一致）。
        auto own_result = [&](VReg v) {
            if (ir_is_heap(et)) push_temp(v);
            else if (!et || et->kind == TyKind::Unknown)
                call_void(RtFn::RcTempUnknown, {v});
        };
        auto branch_ok = [&](int body, int end) {
            VReg ok = load_slot(ok_slot, t_int());
            Inst br;
            br.op = IrOp::Branch;
            br.a = ok;
            br.target = body;
            br.target2 = end;
            emit(std::move(br));
        };

        int cond = new_block("yfrom.cond");
        int scond = new_block("yfrom.send");
        int tcond = new_block("yfrom.throw");
        int body = new_block("yfrom.body");
        int end = new_block("yfrom.end");
        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));

        switch_to(cond);
        VReg pend = load_slot(exc_slot, t_int());
        Inst pb;
        pb.op = IrOp::Branch;
        pb.a = pend;
        pb.target = tcond;
        pb.target2 = scond;
        emit(std::move(pb));

        // 正常：恢复后 p[2] 是调用方 send 进来的值；第一次 resume 时它是初始
        // 值（next 为 None），直接转发给子生成器。
        switch_to(scond);
        Inst se;
        se.op = IrOp::GenSent;
        VReg sent = def(t_unknown(), std::move(se));
        Inst call;
        call.op = IrOp::CallRt;
        call.imm = static_cast<int64_t>(RtFn::GenSendOpt);
        call.args = {load_slot(sub_slot, styp), sent, slot_addr_of(ok_slot)};
        VReg ev = def(t_unknown(), std::move(call));
        own_result(ev);
        store_slot(ev_slot, ev);
        branch_ok(body, end);

        // throw 转发：把注入的异常交给子生成器，结果同样按「产出 / 结束」
        // 分流；子生成器自己没接住时异常从它里面逃出，沿外层 handler 传播。
        switch_to(tcond);
        VReg excp = load_slot(exc_slot, t_int());
        store_slot(exc_slot, const_int(0));
        Inst call2;
        call2.op = IrOp::CallRt;
        call2.imm = static_cast<int64_t>(RtFn::GenThrowOpt);
        call2.args = {load_slot(sub_slot, styp), excp, slot_addr_of(ok_slot)};
        VReg ev2 = def(t_unknown(), std::move(call2));
        own_result(ev2);
        store_slot(ev_slot, ev2);
        branch_ok(body, end);

        switch_to(body);
        emit_yield(load_slot(ev_slot, et), et);
        // 这个挂起点处于委托中：调用方 throw() 时异常要转给子生成器，
        // 而不是在本帧重新抛出。
        st_->gen_states.back().delegate_cond = cond;
        st_->gen_states.back().delegate_exc_slot = exc_slot;
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));

        switch_to(end);
        // 子生成器结束时 ev_slot 里是它的 return 值（send_opt/throw_opt
        // 已把所有权转移过来）。槽持有引用，直接借出即可。
        return load_slot(ev_slot, et);
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
                                   size_t active_limit, bool push_result_temp) {
    (void)line;
    (void)col;
    TypePtr vt = vreg_ty(w);
    // 把 awaitable 的所有权交给 out，事件循环接管。
    if (ir_is_heap(vt)) rc_inc(w);
    else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(w);
    exc_leave();
    cleanup_gen_mark();
    // handler 记录和 yield 一样要在挂起时摘下来。
    size_t n_active = std::min(st_->cleanups.size(), active_limit);
    for (size_t i = n_active; i > 0; --i) {
        const auto& c = st_->cleanups[i - 1];
        if (c.kind == FuncCtx::Cleanup::Kind::With ||
            c.kind == FuncCtx::Cleanup::Kind::AsyncWith ||
            c.kind == FuncCtx::Cleanup::Kind::Finally ||
            c.kind == FuncCtx::Cleanup::Kind::Handler ||
            c.kind == FuncCtx::Cleanup::Kind::LockRelease ||
            c.kind == FuncCtx::Cleanup::Kind::TimeoutExit ||
            c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel ||
            c.kind == FuncCtx::Cleanup::Kind::SemRelease)
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
    // 调用方若还要在本表达式内先 handler_leave（如 async for 的 StopAsyncIteration
    // 保护），必须延后入栈，否则 leave 会按旧水位把这个刚到手的值释放掉。
    if (push_result_temp && ir_is_heap(at)) push_temp(sent);
    return sent;
}

VReg Lowerer::lower_await(Expr& e) {
    if (!st_->is_coroutine) {
        err(e.line, e.col, "'await' outside async function");
        return const_none();
    }
    if (reject_suspend_in_close()) return const_none();
    auto& aw = static_cast<Await&>(e);
    VReg v = expr(*aw.value);
    return suspend_on_awaitable(v, tc_.type_of(&e), e.line, e.col);
}

// 生成器 → list：惰性值没有 len/下标，只能顺序抽干一次。
VReg Lowerer::gen_to_list(VReg gen, TypePtr gen_ty) {
    TypePtr et = (gen_ty && !gen_ty->sub.empty()) ? gen_ty->sub[0] : t_unknown();
    TypePtr lt = mk_list(et);
    int ko = list_kind_filled(lt);
    VReg l = call_runtime(RtFn::ListNew, lt, {const_int(0), const_int(ko)});
    push_temp(l);
    int l_slot = new_temp_slot(lt);
    store_slot(l_slot, l);
    TypePtr gt = gen_ty ? gen_ty : t_unknown();
    int g_slot = new_temp_slot(gt);
    store_slot(g_slot, gen);
    int ok_slot = new_temp_slot(t_int());
    store_slot(ok_slot, const_int(0));

    int cond = new_block("genlist.cond");
    int body = new_block("genlist.body");
    int end = new_block("genlist.end");

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
    call.args = {load_slot(g_slot, gt), okp};
    // 返回的是机器字（浮点也是位模式），vreg 类型只能记 unknown。
    VReg ev = def(t_unknown(), std::move(call));
    ev = retype(ev, et);
    Inst br;
    br.op = IrOp::Branch;
    br.a = load_slot(ok_slot, t_int());
    br.target = body;
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    retain_elem(ev, ko);
    call_void(RtFn::ListAppend, {load_slot(l_slot, lt), ev});
    // 元素引用已经转给列表，产生者的那份立即还掉，否则循环里一路累积。
    if (ir_is_heap(et)) call_void(RtFn::RcDec, {ev});
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = cond;
    emit(std::move(jb));

    switch_to(end);
    return load_slot(l_slot, lt);
}

// 生成器流式迭代：每取一个元素就交给 body，不建中间列表。
void Lowerer::emit_gen_for_each(VReg gen, TypePtr gen_ty,
                                const std::function<void(VReg, TypePtr, int, int)>& body) {
    TypePtr et = (gen_ty && !gen_ty->sub.empty()) ? gen_ty->sub[0] : t_unknown();
    TypePtr gt = gen_ty ? gen_ty : t_unknown();
    int g_slot = new_temp_slot(gt);
    store_slot(g_slot, gen);
    int ok_slot = new_temp_slot(t_int());
    store_slot(ok_slot, const_int(0));

    int cond = new_block("genfor.cond");
    int b = new_block("genfor.body");
    int end = new_block("genfor.end");

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
    call.args = {load_slot(g_slot, gt), okp};
    VReg ev = def(t_unknown(), std::move(call));
    ev = retype(ev, et);
    Inst br;
    br.op = IrOp::Branch;
    br.a = load_slot(ok_slot, t_int());
    br.target = b;
    br.target2 = end;
    emit(std::move(br));

    switch_to(b);
    // 元素引用转进槽里，下一轮覆盖时自动释放；body 借槽里的值，
    // 提前跳出（any/all 短路）也不会漏掉这一份。
    int ev_slot = new_temp_slot(et);
    store_slot(ev_slot, ev);
    if (ir_is_heap(et)) call_void(RtFn::RcDec, {ev});
    body(load_slot(ev_slot, et), et, cond, end);
    if (!terminated()) {
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));
    }
    switch_to(end);
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
    // 自定义 __aiter__：先调用拿到异步迭代器，再按它的类型驱动。
    if (seq_ty && seq_ty->kind == TyKind::Class && seq_ty->cls) {
        const MethodInfo* ai = seq_ty->cls->method("__aiter__");
        if (!ai) {
            err(f.iter->line, f.iter->col,
                std::format("async for requires an async generator or a class with "
                            "__aiter__, got '{}'",
                            type_name(seq_ty)));
            return;
        }
        TypePtr rt = t_unknown();
        if (ai->call_sig && ai->call_sig->ret) rt = ai->call_sig->ret;
        if (ai->decorated && ai->decorated_ty && ai->decorated_ty->kind == TyKind::Func &&
            ai->decorated_ty->fn && ai->decorated_ty->fn->ret)
            rt = ai->decorated_ty->fn->ret;
        VReg iter = kNoReg;
        if (ai->decorated) {
            iter = dunder_call(seq, seq_ty, "__aiter__", {}, nullptr);
        } else if (ai->slot >= 0) {
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = seq;
            vc.imm = ai->slot;
            iter = def_heap(rt, std::move(vc));
        }
        if (iter == kNoReg) {
            err(f.iter->line, f.iter->col,
                std::format("__aiter__ on '{}' must be an instance method",
                            seq_ty->cls->name));
            return;
        }
        seq = iter;
        seq_ty = rt;
    }
    TypePtr et = t_unknown();
    const MethodInfo* ne = nullptr;
    bool use_anext = false;
    if (seq_ty && seq_ty->kind == TyKind::AsyncGenerator) {
        et = seq_ty->sub.empty() ? t_unknown() : seq_ty->sub[0];
    } else if (seq_ty && seq_ty->kind == TyKind::Class && seq_ty->cls &&
               (ne = seq_ty->cls->method("__anext__")) != nullptr) {
        // __aiter__ 返回自身（或另一个异步迭代器）：每轮 await __anext__()。
        TypePtr nt = ne->call_sig ? ne->call_sig->ret : nullptr;
        if (ne->decorated && ne->decorated_ty && ne->decorated_ty->kind == TyKind::Func &&
            ne->decorated_ty->fn)
            nt = ne->decorated_ty->fn->ret;
        if (nt && nt->kind == TyKind::Coroutine)
            et = nt->sub.empty() ? t_unknown() : nt->sub[0];
        use_anext = true;
    } else if (seq_ty && seq_ty->kind != TyKind::Unknown) {
        err(f.iter->line, f.iter->col,
            std::format("async for requires an async generator or a class with "
                        "__aiter__, got '{}'",
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
    VReg w = kNoReg;
    if (use_anext) {
        if (ne->decorated) {
            w = dunder_call(seq_val(), seq_ty, "__anext__", {}, nullptr);
        } else if (ne->slot >= 0) {
            Inst vc;
            vc.op = IrOp::VCall;
            vc.a = seq_val();
            vc.imm = ne->slot;
            w = def_heap(mk_coroutine(et), std::move(vc));
        }
        if (w == kNoReg) {
            err(f.iter->line, f.iter->col,
                std::format("__anext__ on '{}' must be an async instance method",
                            seq_ty->cls->name));
            return;
        }
    } else {
        Inst call;
        call.op = IrOp::CallRt;
        call.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
        call.args = {seq_val()};
        w = def_heap(mk_coroutine(et), std::move(call));
    }
    VReg val = suspend_on_awaitable(w, et, f.line, f.col,
                                    static_cast<size_t>(-1), false);
    st_->cleanups.pop_back();
    call_void(RtFn::HandlerLeave, {load_slot(hs_slot, t_int())});
    // handler 已离开，现在才能把到手的堆值挂到临时栈（语句末释放）。
    if (ir_is_heap(et)) push_temp(val);
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
                                          const std::string& label, int frame_kind,
                                          const ClassInfo* owner) {
    bool is_coro = frame_kind != 0;   // 协程和异步生成器都能 await
    bool is_agen = frame_kind == 2;
    IRFunction* irf = mod_.add_func(label + "$resume");
    int ridx = static_cast<int>(mod_.funcs.size()) - 1;
    IRFunction* cw = mod_.add_func(label);
    int cidx = static_cast<int>(mod_.funcs.size()) - 1;
    if (owner) fn_owner_class_[irf] = owner;

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
    // 异常上下文三件套：值是 malloc 指针 / 整数，不参与 RC。
    st_->exc_cur_slot = irf->new_slot(t_int());
    st_->exc_top_slot = irf->new_slot(t_int());
    st_->exc_tok_slot = irf->new_slot(t_int());
    st_->fn_mark = kNoReg;

    // 创建包装已经 retain 过实参，这里不再 inc_params。
    create_cells();
    stmts(f.body);
    // 挂起点位置已经全部确定：把跨挂起仍活跃的 vreg 落进帧槽。
    spill_across_suspends();
    if (!terminated()) gen_finish();

    // ── close 路径：按挂起点快照跑 finally / __exit__，再释放帧槽 ──
    auto has_runtime_handler = [](const FuncCtx::Cleanup& c) {
        return c.kind == FuncCtx::Cleanup::Kind::Handler ||
               c.kind == FuncCtx::Cleanup::Kind::With ||
               c.kind == FuncCtx::Cleanup::Kind::AsyncWith ||
               c.kind == FuncCtx::Cleanup::Kind::Finally ||
               c.kind == FuncCtx::Cleanup::Kind::LockRelease ||
               c.kind == FuncCtx::Cleanup::Kind::TimeoutExit ||
               c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel ||
               c.kind == FuncCtx::Cleanup::Kind::SemRelease;
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
            } else if (c.kind == FuncCtx::Cleanup::Kind::LockRelease) {
                call_void(RtFn::LockRelease, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::TimeoutExit) {
                call_void(RtFn::TimeoutExit, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::TaskGroupCancel) {
                call_void(RtFn::TaskGroupCancel, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::SemRelease) {
                call_void(RtFn::SemRelease, {load_slot(c.mgr_slot, t_unknown())});
            } else if (c.kind == FuncCtx::Cleanup::Kind::Finally && c.body) {
                // 重放发生在函数体降级完之后，loops 已空；恢复 finally
                // 词法上所属的循环，break/continue 才能找到目标块。
                bool pushed_loop = false;
                if (st_->loops.empty() && c.loop_brk >= 0) {
                    st_->loops.push_back({c.loop_brk, c.loop_cont});
                    st_->loop_cleanup_depth.push_back(c.loop_depth);
                    pushed_loop = true;
                }
                ++close_replay_depth_;
                stmts(*c.body);
                --close_replay_depth_;
                if (pushed_loop) {
                    st_->loops.pop_back();
                    st_->loop_cleanup_depth.pop_back();
                }
            }
        }
        // 挂起时摘下来的 handler 记录不会有人再弹栈，close 路径负责释放。
        for (auto& c : gs.cleanups)
            if (has_runtime_handler(c))
                call_void(RtFn::HandlerFree, {load_slot(c.handler_slot, t_int())});
        cleanup_gen_mark();
        release_slots();
        exc_leave();
        Inst d;
        d.op = IrOp::GenDone;
        emit(std::move(d));
        close_targets.push_back({gs.id, cb});
    }
    int close_default = new_block("g.close.default");
    switch_to(close_default);
    cleanup_gen_mark();
    release_slots();
    exc_leave();
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
    // 每次 resume 都换成本帧保存的异常上下文；挂起/退出时换回去。
    exc_enter();
    Inst gsit;
    gsit.op = IrOp::GenGetState;
    VReg state = def(t_int(), std::move(gsit));
    Inst gcl;
    gcl.op = IrOp::GenClosing;
    VReg closing = def(t_int(), std::move(gcl));
    int close_disp = new_block("g.close.dispatch");
    int normal_disp = new_block("g.dispatch");
    // 协程/异步生成器的失败注入和生成器的 throw() 共用这条分发。
    int throw_disp = new_block("g.throw.dispatch");
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
    {
        Inst ge;
        ge.op = IrOp::GenGetExc;
        VReg exc = def(t_int(), std::move(ge));
        Inst br;
        br.op = IrOp::Branch;
        br.a = exc;
        br.target = throw_disp;
        br.target2 = normal_disp;
        emit(std::move(br));
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

    // ── 异常注入分发 ────────────────────────────────────────────
    // 被 await 的对象失败（协程）或调用方 throw()（生成器）时，把异常放回
    // 挂起点。恢复路径先重挂 handler 再抛：try/except 才能抓到跨挂起的异常。
    // 处于 yield from 委托中的挂起点不在这里抛，而是把异常交给子生成器。
    {
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
            // handler 的 setjmp 环境必须在本帧重新捕获：注入的异常可能是
            // 由另一个调用点（next/send/throw）触发的，直接 longjmp 到上次
            // resume 留下的环境会跨越不同的栈帧深度。
            for (const auto* c : hs) {
                call_void(RtFn::HandlerRearm, {load_slot(c->handler_slot, t_int())});
                VReg sj = setjmp_handler(c->handler_slot);
                int nb = new_block("g.throw.arm.next");
                Inst ab;
                ab.op = IrOp::Branch;
                ab.a = sj;
                ab.target = c->land;
                ab.target2 = nb;
                emit(std::move(ab));
                switch_to(nb);
            }
            if (gs.delegate_cond >= 0) {
                // 委托给子生成器：取走帧里的异常（所有权转移），交回委托
                // 循环的 cond，由它改走 GenThrowOpt。
                Inst te;
                te.op = IrOp::GenTakeExc;
                VReg excp = def(t_int(), std::move(te));
                store_slot(gs.delegate_exc_slot, excp);
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = gs.delegate_cond;
                emit(std::move(jj));
            } else {
                Inst rt;
                rt.op = IrOp::GenRethrow;
                emit(std::move(rt));
            }
            tcur = nextb;
        }
        switch_to(tcur);
        // 未启动的生成器没有挂起点：入口即抛出。
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
    for (int s = 0; s < gin.n_slots; ++s) {
        if (st_->borrow_slots.count(s)) continue;
        if (!ir_is_heap(irf->slot_ty[static_cast<size_t>(s)])) continue;
        // 2 = 可能装着非堆值：帧销毁/异常逃出释放时走注册表。
        gin.slot_heap[static_cast<size_t>(s)] =
            (static_cast<size_t>(s) < irf->slot_unknown.size() &&
             irf->slot_unknown[static_cast<size_t>(s)])
                ? 2
                : 1;
    }
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


// ── 跨挂起的 vreg 溢出 ─────────────────────────────────────────
//
// resume 每次被调用都重建 C 栈帧，vreg 落在栈上的那份值在 yield/await
// 返回后就没了。续跑块的 live-in 恰好就是「挂起期间必须继续活着」的
// 值：挂起前 retain 一份写进堆帧槽，恢复后再装回 vreg 并交给临时栈。
// 这样 yield/await 能出现在任意子表达式里，复合表达式不用逐个改。
void Lowerer::spill_across_suspends() {
    IRFunction* fn = st_->fn;
    if (!fn || st_->gen_states.empty()) return;
    const int nb = static_cast<int>(fn->blocks.size());
    const int nv = static_cast<int>(fn->vreg_ty.size());
    if (nb == 0 || nv == 0) return;

    auto for_each_read = [](const Inst& in, auto&& f) {
        if (in.a != kNoReg) f(in.a);
        if (in.b != kNoReg) f(in.b);
        if (in.c != kNoReg) f(in.c);
        for (VReg r : in.args)
            if (r != kNoReg) f(r);
    };

    // 块内 use/def：块首未定义就读取的才算 live-in 需求。
    std::vector<std::vector<uint8_t>> use(nb, std::vector<uint8_t>(nv, 0));
    std::vector<std::vector<uint8_t>> def(nb, std::vector<uint8_t>(nv, 0));
    for (int b = 0; b < nb; ++b) {
        for (const Inst& in : fn->blocks[static_cast<size_t>(b)].insts) {
            for_each_read(in, [&](VReg r) {
                if (static_cast<int>(r) < nv &&
                    !def[static_cast<size_t>(b)][static_cast<size_t>(r)])
                    use[static_cast<size_t>(b)][static_cast<size_t>(r)] = 1;
            });
            if (in.dst != kNoReg && static_cast<int>(in.dst) < nv)
                def[static_cast<size_t>(b)][static_cast<size_t>(in.dst)] = 1;
        }
    }

    // 挂起是终止指令：GenYield/GenAwait 没有后继，续跑块只从入口分派进。
    std::vector<std::vector<int>> succ(nb);
    for (int b = 0; b < nb; ++b) {
        const auto& ins = fn->blocks[static_cast<size_t>(b)].insts;
        if (ins.empty()) {
            if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
            continue;
        }
        auto add = [&](int t) {
            if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
        };
        switch (ins.back().op) {
            case IrOp::Jump: add(ins.back().target); break;
            case IrOp::Branch:
                add(ins.back().target);
                add(ins.back().target2);
                break;
            case IrOp::Return:
            case IrOp::GenYield:
            case IrOp::GenAwait:
            case IrOp::GenRethrow:
            case IrOp::GenDone: break;
            default:
                if (b + 1 < nb) succ[static_cast<size_t>(b)].push_back(b + 1);
                break;
        }
    }

    std::vector<std::vector<uint8_t>> live_in(nb, std::vector<uint8_t>(nv, 0));
    std::vector<std::vector<uint8_t>> live_out(nb, std::vector<uint8_t>(nv, 0));
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = nb - 1; b >= 0; --b) {
            std::vector<uint8_t> out(nv, 0);
            for (int s : succ[static_cast<size_t>(b)])
                for (int v = 0; v < nv; ++v)
                    out[static_cast<size_t>(v)] |=
                        live_in[static_cast<size_t>(s)][static_cast<size_t>(v)];
            std::vector<uint8_t> in(nv, 0);
            for (int v = 0; v < nv; ++v)
                in[static_cast<size_t>(v)] = static_cast<uint8_t>(
                    use[static_cast<size_t>(b)][static_cast<size_t>(v)] |
                    (out[static_cast<size_t>(v)] &
                     ~def[static_cast<size_t>(b)][static_cast<size_t>(v)]));
            if (out != live_out[static_cast<size_t>(b)] ||
                in != live_in[static_cast<size_t>(b)]) {
                live_out[static_cast<size_t>(b)] = std::move(out);
                live_in[static_cast<size_t>(b)] = std::move(in);
                changed = true;
            }
        }
    }

    std::unordered_map<int, int> cont_of_state;
    for (auto& gs : st_->gen_states) cont_of_state[gs.id] = gs.cont;

    // 同一个 vreg 在多个挂起点之间复用一个溢出槽。
    std::unordered_map<VReg, int> spill_slot;
    auto slot_for = [&](VReg v) {
        auto it = spill_slot.find(v);
        if (it != spill_slot.end()) return it->second;
        int s = fn->new_slot(fn->vreg_ty[static_cast<size_t>(v)]);
        spill_slot.emplace(v, s);
        return s;
    };

    for (int b = 0; b < nb; ++b) {
        auto& ins = fn->blocks[static_cast<size_t>(b)].insts;
        if (ins.empty()) continue;
        const IrOp term_op = ins.back().op;
        if (term_op != IrOp::GenYield && term_op != IrOp::GenAwait) continue;
        const int state_id = static_cast<int>(ins.back().imm);
        auto cit = cont_of_state.find(state_id);
        if (cit == cont_of_state.end()) continue;
        const int c = cit->second;
        if (c < 0 || c >= nb) continue;

        std::vector<VReg> live;
        for (int v = 0; v < nv; ++v)
            if (live_in[static_cast<size_t>(c)][static_cast<size_t>(v)])
                live.push_back(static_cast<VReg>(v));

        // 挂起后的续跑块：先把值从堆帧槽装回 vreg，所有权转给临时栈。
        if (!live.empty()) {
            std::vector<Inst> post;
            for (VReg v : live) {
                TypePtr ty = fn->vreg_ty[static_cast<size_t>(v)];
                int slot = slot_for(v);
                Inst ld;
                ld.op = IrOp::LoadSlot;
                ld.idx = slot;
                ld.ty = ty;
                ld.dst = v;
                post.push_back(std::move(ld));
                if (ir_is_heap(ty)) {
                    Inst tp;
                    tp.op = IrOp::CallRt;
                    tp.imm = static_cast<int64_t>(RtFn::RcTemp);
                    tp.args = {v};
                    post.push_back(std::move(tp));
                    // 槽已经不再持有这份引用；不清零的话帧销毁会二次释放。
                    Inst zero;
                    zero.op = IrOp::ConstInt;
                    zero.imm = 0;
                    VReg z = static_cast<VReg>(fn->vreg_ty.size());
                    fn->vreg_ty.push_back(t_int());
                    zero.dst = z;
                    zero.ty = t_int();
                    post.push_back(std::move(zero));
                    Inst cl;
                    cl.op = IrOp::StoreSlot;
                    cl.idx = slot;
                    cl.a = z;
                    post.push_back(std::move(cl));
                } else if (!ty || ty->kind == TyKind::Unknown) {
                    Inst tp;
                    tp.op = IrOp::CallRt;
                    tp.imm = static_cast<int64_t>(RtFn::RcTempUnknown);
                    tp.args = {v};
                    post.push_back(std::move(tp));
                }
            }
            auto& cont_insts = fn->blocks[static_cast<size_t>(c)].insts;
            cont_insts.insert(cont_insts.begin(), post.begin(), post.end());
        }

        // 挂起前：retain 一份再写进堆帧槽。必须赶在 RcCleanup 之前，
        // 否则临时栈先把产生者那份引用放掉了，写进去的就是悬空指针。
        if (live.empty()) continue;
        int cleanup_pos = -1;
        for (int i = static_cast<int>(ins.size()) - 2; i >= 0; --i) {
            if (ins[static_cast<size_t>(i)].op == IrOp::CallRt &&
                static_cast<RtFn>(ins[static_cast<size_t>(i)].imm) == RtFn::RcCleanup) {
                cleanup_pos = i;
                break;
            }
        }
        size_t at = cleanup_pos >= 0 ? static_cast<size_t>(cleanup_pos) : ins.size() - 1;
        std::vector<Inst> pre;
        for (VReg v : live) {
            TypePtr ty = fn->vreg_ty[static_cast<size_t>(v)];
            int slot = slot_for(v);
            if (ir_is_heap(ty)) {
                Inst inc;
                inc.op = IrOp::CallRt;
                inc.imm = static_cast<int64_t>(RtFn::RcInc);
                inc.args = {v};
                pre.push_back(std::move(inc));
            } else if (!ty || ty->kind == TyKind::Unknown) {
                Inst inc;
                inc.op = IrOp::CallRt;
                inc.imm = static_cast<int64_t>(RtFn::RcIncUnknown);
                inc.args = {v};
                pre.push_back(std::move(inc));
            }
            Inst st;
            st.op = IrOp::StoreSlot;
            st.idx = slot;
            st.a = v;
            pre.push_back(std::move(st));
        }
        ins.insert(ins.begin() + static_cast<std::ptrdiff_t>(at), pre.begin(), pre.end());
    }
}


}  // namespace pyadamas
