// lower/call.cpp —— 调用降级：参数绑定、内置/方法/asyncio
#include "lower/impl.h"

#include <limits>


namespace pyadamas {

// asyncio 内建调用（gather/create_task/sleep 等）的降级

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
        // 用 typecheck 推出的返回类型落 vreg：以前一律 t_unknown()，`None` 返回值
        // 会被 print 当成 int 0（实测 `print(asyncio.run(noop()))` 输出 0 而非 None）。
        // float 保持 unknown：运行期给的是 double 位模式，直接标 float 会按数值解释错。
        TypePtr rty = rt;
        if (!rty || rty->kind == TyKind::Unknown || rty->kind == TyKind::Float)
            rty = t_unknown();
        VReg v = def(rty, std::move(in));
        if (ir_is_heap(rty)) push_temp(v);
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

    if (member == "Event") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.Event() takes no arguments");
        VReg ev = call_runtime(RtFn::EventNew, rt ? rt : mk_async_event(), {});
        push_temp(ev);
        return ev;
    }

    if (member == "Lock") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.Lock() takes no arguments");
        VReg lk = call_runtime(RtFn::LockNew, rt ? rt : mk_async_lock(), {});
        push_temp(lk);
        return lk;
    }

    if (member == "Queue") {
        if (c.args.size() > 1)
            err(c.line, c.col, "asyncio.Queue() takes at most one argument");
        VReg maxsize = const_int(0);
        if (!c.args.empty())
            maxsize = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        // 构造表达式本身只推出 Queue[unknown]；带标注的赋值会把
        // Queue[float] 之类的目标类型暂存下来。
        TypePtr qt = rt;
        if (pending_queue_ty_ && pending_queue_ty_->kind == TyKind::AsyncQueue &&
            (!qt || qt->kind != TyKind::AsyncQueue || qt->sub.empty() ||
             !qt->sub[0] || qt->sub[0]->kind == TyKind::Unknown))
            qt = pending_queue_ty_;
        pending_queue_ty_ = nullptr;
        VReg q = call_runtime(RtFn::QueueNew, qt ? qt : mk_async_queue(t_unknown()),
                              {maxsize, const_int(queue_kind(qt))});
        push_temp(q);
        return q;
    }

    if (member == "Semaphore") {
        if (c.args.size() > 1)
            err(c.line, c.col, "asyncio.Semaphore() takes at most one argument");
        VReg value = const_int(1);
        if (!c.args.empty())
            value = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::SemNew, rt ? rt : mk_async_semaphore(), {value});
        push_temp(s);
        return s;
    }

    if (member == "TaskGroup") {
        if (!c.args.empty())
            err(c.line, c.col, "asyncio.TaskGroup() takes no arguments");
        VReg g = call_runtime(RtFn::TaskGroupNew, rt ? rt : mk_async_taskgroup(), {});
        push_temp(g);
        return g;
    }

    if (member == "timeout") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "asyncio.timeout() takes exactly one argument");
            return const_none();
        }
        VReg secs = expr(*c.args[0]);
        secs = coerce(secs, tc_.type_of(c.args[0].get()), t_float());
        VReg to = call_runtime(RtFn::AsyncTimeoutNew, rt ? rt : mk_async_timeout(), {secs});
        push_temp(to);
        return to;
    }

    if (member == "wait_for") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "asyncio.wait_for() takes exactly two arguments");
            return const_none();
        }
        VReg aw = expr(*c.args[0]);
        VReg secs = expr(*c.args[1]);
        secs = coerce(secs, tc_.type_of(c.args[1].get()), t_float());
        VReg w = call_runtime(RtFn::AsyncioWaitFor,
                              rt ? rt : mk_coroutine(t_unknown()), {aw, secs});
        push_temp(w);
        return w;
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
        TypePtr task_elem = (rt && rt->kind == TyKind::Task && !rt->sub.empty())
                                ? rt->sub[0]
                                : nullptr;
        VReg t = call_runtime(RtFn::AsyncioCreateTask,
                              rt ? rt : mk_task(t_unknown()),
                              {coro, const_int(own_kind(task_elem))});
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
        // gather 结果列表的元素所有权按静态结果类型选择，避免把 float
        // 位模式当堆指针去 dec。
        TypePtr res_list = (rt && rt->kind == TyKind::Coroutine && !rt->sub.empty())
                               ? rt->sub[0]
                               : nullptr;
        VReg w = call_runtime(RtFn::AsyncioGather,
                              rt ? rt : mk_coroutine(mk_list(t_unknown())),
                              {L, const_int(list_kind_filled(res_list))});
        push_temp(w);
        return w;
    }

    err(c.line, c.col,
        std::format("asyncio.{} is not supported by the code generator yet", member));
    return const_none();
}





}  // namespace pyadamas
