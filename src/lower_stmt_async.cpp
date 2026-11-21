// lower_stmt_async.cpp —— async with 的降级
#include "lower_impl.h"

namespace pyadamas {

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
    if (ct && ct->kind == TyKind::AsyncLock) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        VReg fut = call_runtime(RtFn::LockAcquire, mk_future(t_bool()),
                                {load_slot(ms, ct)});
        push_temp(fut);
        VReg ev = suspend_on_awaitable(fut, t_bool(), item.context->line, item.context->col);
        if (item.optional_vars) assign_to(*item.optional_vars, ev, t_bool());

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
        cf.kind = FuncCtx::Cleanup::Kind::LockRelease;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            call_void(RtFn::LockRelease, {load_slot(ms, ct)});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::LockRelease, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncEvent) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        VReg fut = call_runtime(RtFn::EventWait, mk_future(t_bool()),
                                {load_slot(ms, ct)});
        push_temp(fut);
        VReg ev = suspend_on_awaitable(fut, t_bool(), item.context->line, item.context->col);
        if (item.optional_vars) assign_to(*item.optional_vars, ev, t_bool());
        lower_async_with_item(w, idx + 1);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncSemaphore) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        VReg fut = call_runtime(RtFn::SemAcquire, mk_future(t_bool()),
                                {load_slot(ms, ct)});
        push_temp(fut);
        VReg ev = suspend_on_awaitable(fut, t_bool(), item.context->line, item.context->col);
        if (item.optional_vars) assign_to(*item.optional_vars, ev, t_bool());
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
        cf.kind = FuncCtx::Cleanup::Kind::SemRelease;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            call_void(RtFn::SemRelease, {load_slot(ms, ct)});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::SemRelease, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncTimeout) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        if (st_->is_generator) cleanup_gen_mark();
        call_void(RtFn::TimeoutEnter, {load_slot(ms, ct)});
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
        cf.kind = FuncCtx::Cleanup::Kind::TimeoutExit;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            call_void(RtFn::TimeoutExit, {load_slot(ms, ct)});
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::TimeoutExit, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
    if (ct && ct->kind == TyKind::AsyncTaskGroup) {
        int ms = new_temp_slot(ct);
        store_slot(ms, mgr);
        if (st_->is_generator) cleanup_gen_mark();
        if (item.optional_vars) assign_to(*item.optional_vars, load_slot(ms, ct), ct);
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
        cf.kind = FuncCtx::Cleanup::Kind::TaskGroupCancel;
        cf.handler_slot = hs;
        cf.land = land;
        cf.mgr_slot = ms;
        st_->cleanups.push_back(cf);
        lower_async_with_item(w, idx + 1);
        st_->cleanups.pop_back();
        if (!terminated()) {
            call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
            VReg g = call_runtime(RtFn::TaskGroupExit, mk_coroutine(mk_list(t_unknown())),
                                  {load_slot(ms, ct)});
            push_temp(g);
            suspend_on_awaitable(g, mk_list(t_unknown()), w.line, w.col);
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(land);
        call_void(RtFn::HandlerLeave, {load_slot(hs, t_int())});
        VReg tname = call_runtime(RtFn::ExcTypeName, t_str(), {});
        push_temp(tname);
        VReg tmsg = call_runtime(RtFn::ExcMessage, t_str(), {});
        push_temp(tmsg);
        int ts = new_temp_slot(t_str());
        store_slot(ts, tname);
        int msgs = new_temp_slot(t_str());
        store_slot(msgs, tmsg);
        call_void(RtFn::ExcClear, {});
        call_void(RtFn::TaskGroupCancel, {load_slot(ms, ct)});
        call_void(RtFn::Raise, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        end_unreachable();
        switch_to(end);
        return;
    }
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
    // 装饰后的 __aenter__/__aexit__ 走类属性里的包装函数：先确认包装函数能
    // 接住 (self, ...) 并且在定义时就被标注成协程。
    auto wrapped_ret = [&](const MethodInfo* m, const char* nm) -> TypePtr {
        if (!m->decorated) return m->call_sig->ret;
        const ClassAttrInfo* ca = ct->cls->class_attr(nm);
        TypePtr wty = ca ? ca->ty : nullptr;
        FuncInfoPtr wf = (wty && wty->kind == TyKind::Func) ? wty->fn : nullptr;
        if (!wf || wf->varargs || wf->kwargs ||
            wf->pos.size() != m->call_sig->pos.size() + 1) {
            err(item.context->line, item.context->col,
                std::format("decorated {} of '{}' has an incompatible signature", nm,
                            ct->cls->name));
            return nullptr;
        }
        return wf->ret;
    };
    TypePtr en_ret = wrapped_ret(en, "__aenter__");
    TypePtr ex_ret = wrapped_ret(ex, "__aexit__");
    if (!en_ret || en_ret->kind != TyKind::Coroutine || !ex_ret ||
        ex_ret->kind != TyKind::Coroutine) {
        err(item.context->line, item.context->col,
            std::format("async with '{}': __aenter__/__aexit__ must return a coroutine",
                        ct->cls->name));
        en_ret = en->call_sig->ret;
        ex_ret = ex->call_sig->ret;
    }
    int ms = new_temp_slot(ct);
    store_slot(ms, mgr);

    // __aenter__：调用得到协程，await 出绑定值。
    Call enter_call;
    enter_call.line = w.line;
    enter_call.col = w.col;
    std::vector<VReg> eargs = bind_args(enter_call, en->call_sig, en->def);
    VReg enter_coro = kNoReg;
    if (en->decorated) {
        enter_coro = dunder_call(load_slot(ms, ct), ct, "__aenter__", std::move(eargs), nullptr);
        if (enter_coro == kNoReg) enter_coro = const_none();
    } else {
        Inst vc;
        vc.op = IrOp::VCall;
        vc.a = load_slot(ms, ct);
        vc.imm = en->slot;
        vc.args = std::move(eargs);
        enter_coro = def_heap(en_ret, std::move(vc));
    }
    TypePtr ert = en_ret->sub.empty() ? t_unknown() : en_ret->sub[0];
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
    cf.ret = ex_ret;
    cf.inner_ret = ex_ret->sub.empty() ? t_unknown() : ex_ret->sub[0];
    if (ex->decorated) cf.exit_cls = ct->cls;
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
        VReg coro = kNoReg;
        if (cf.exit_cls) {
            coro = dunder_call(load_slot(ms, ct), ct, "__aexit__", std::move(args), nullptr);
            if (coro == kNoReg) coro = const_none();
        } else {
            Inst vc2;
            vc2.op = IrOp::VCall;
            vc2.a = load_slot(ms, ct);
            vc2.imm = ex->slot;
            vc2.args = std::move(args);
            coro = def_heap(cf.ret, std::move(vc2));
        }
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
        // 异常被 __aexit__ 压住：两个串还得在这里释放，否则 suppress 路径
        // 一样漏（ASan 实测 224 字节）。
        VReg d1 = load_slot(ts, t_str());
        rc_dec(d1);
        store_slot(ts, const_int(0));
        VReg d2 = load_slot(msgs, t_str());
        rc_dec(d2);
        store_slot(msgs, const_int(0));
        Inst j;
        j.op = IrOp::Jump;
        j.target = end;
        emit(std::move(j));
        switch_to(rethrow);
        // 用 RaiseOwn：这两个串存在临时槽里，Raise 是 noreturn，longjmp 之后
        // 槽永远走不到释放点（挂起前 push 的临时栈也随挂起丢了）→ 每次泄漏
        // type+message 两个串。RaiseOwn 拷进异常对象后自己 dec 掉它们。
        call_void(RtFn::RaiseOwn, {load_slot(ts, t_str()), load_slot(msgs, t_str())});
        store_slot(ts, const_int(0));
        store_slot(msgs, const_int(0));
        end_unreachable();
    }
    switch_to(end);
}



}  // namespace pyadamas
