// lower_gen.cpp —— 生成器/协程：yield 与 for 生成器
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
    in.idx = Operand::slot(slot);
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
    int str_cur = vt->kind == TyKind::Str ? new_str_cursor() : -1;

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
    } else if (vt->kind == TyKind::Str) {
        // 生成器里同样要游标：否则每轮从头扫码点，长串退化成 O(n^2)。
        ev = str_char_at(sv, iv2, str_cur);
    } else {
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = sv;
        get.b = iv2;
        ev = def(et, std::move(get));
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
    sa.idx = Operand::slot(ok_slot);
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
    call_void(RtFn::ListAppend, {load_slot(l_slot, lt), ev, const_int(ko)});
    // 元素引用已经转给列表，产生者的那份立即还掉，否则循环里一路累积。
    // unknown 元素同口径（retain_elem 已按探针补了列表那份）。
    if (ir_is_heap(et)) call_void(RtFn::RcDec, {ev});
    else if (is_unknown(et)) call_void(RtFn::RcDecUnknown, {ev});
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
    else if (is_unknown(et)) call_void(RtFn::RcDecUnknown, {ev});
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
    else if (is_unknown(et)) push_temp_unknown(ev);
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


}  // namespace pyadamas
