// lower/method_native.cpp —— 原生/异步类型的方法调用降级
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::lower_method_native(Attribute& at, Call& c, VReg recv, TypePtr recv_ty, bool opt) {
    if (opt) recv = guard_not_none(recv, std::format("method '{}'", at.attr));
    // Path.mkdir / Popen.wait / Popen.communicate 收关键字参数，下面单独处理。
    bool kw_ok = (recv_ty->kind == TyKind::Path && at.attr == "mkdir") ||
                 (recv_ty->kind == TyKind::DateTime && at.attr == "replace") ||
                 (recv_ty->kind == TyKind::Popen &&
                  (at.attr == "wait" || at.attr == "communicate"));
    if (!c.keywords.empty() && !kw_ok)
        err(c.line, c.col, std::format("{}() does not take keyword arguments", at.attr));
    auto count = [&](size_t n) {
        if (c.args.size() == n) return true;
        err(c.line, c.col,
            std::format("{}() takes exactly {} argument{}", at.attr, n, n == 1 ? "" : "s"));
        return false;
    };
    if (recv_ty->kind == TyKind::AsyncEvent) {
        if (at.attr == "wait") {
            if (!count(0)) return const_none();
            VReg f = call_runtime(RtFn::EventWait, mk_future(t_bool()), {recv});
            push_temp(f);
            return f;
        }
        if (at.attr == "set") {
            if (!count(0)) return const_none();
            call_void(RtFn::EventSet, {recv});
            return const_none();
        }
        if (at.attr == "is_set") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::EventIsSet, t_bool(), {recv});
        }
    }
    if (recv_ty->kind == TyKind::AsyncLock) {
        if (at.attr == "acquire") {
            if (!count(0)) return const_none();
            VReg f = call_runtime(RtFn::LockAcquire, mk_future(t_bool()), {recv});
            push_temp(f);
            return f;
        }
        if (at.attr == "release") {
            if (!count(0)) return const_none();
            call_void(RtFn::LockRelease, {recv});
            return const_none();
        }
        if (at.attr == "locked") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::LockLocked, t_bool(), {recv});
        }
    }
    if (recv_ty->kind == TyKind::AsyncQueue) {
        if (at.attr == "put") {
            if (!count(1)) return const_none();
            TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
            VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
            // put 可能阻塞（满队列），运行时返回 Future；lowering 直接
            // 把它当可等待值返回，await 与忽略都行。
            VReg f = call_runtime(RtFn::QueuePut, mk_future(t_none()), {recv, v});
            push_temp(f);
            return f;
        }
        if (at.attr == "get") {
            if (!count(0)) return const_none();
            TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
            VReg f = call_runtime(RtFn::QueueGet, mk_future(et), {recv});
            push_temp(f);
            return f;
        }
        if (at.attr == "qsize") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::QueueQsize, t_int(), {recv});
        }
        if (at.attr == "empty") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::QueueEmpty, t_bool(), {recv});
        }
        if (at.attr == "full") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::QueueFull, t_bool(), {recv});
        }
        if (at.attr == "join") {
            if (!count(0)) return const_none();
            VReg f = call_runtime(RtFn::QueueJoin, mk_future(t_none()), {recv});
            push_temp(f);
            return f;
        }
        if (at.attr == "task_done") {
            if (!count(0)) return const_none();
            call_void(RtFn::QueueTaskDone, {recv});
            return const_none();
        }
        if (at.attr == "put_nowait") {
            if (!count(1)) return const_none();
            TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
            VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
            call_void(RtFn::QueuePutNowait, {recv, v});
            return const_none();
        }
        if (at.attr == "get_nowait") {
            if (!count(0)) return const_none();
            TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
            VReg v = call_runtime(RtFn::QueueGetNowait, t_unknown(), {recv});
            return retype(v, et);
        }
    }
    if (recv_ty->kind == TyKind::DateTime) {
        if (at.attr == "isoformat") {
            if (!count(0)) return const_none();
            VReg s = call_runtime(RtFn::DateTimeIsoformat, t_str(), {recv});
            push_temp(s);
            return s;
        }
        if (at.attr == "strftime") {
            if (!count(1)) return const_none();
            VReg f = expr(*c.args[0]);
            f = coerce(f, tc_.type_of(c.args[0].get()), t_str());
            VReg s = call_runtime(RtFn::DateTimeStrftime, t_str(), {recv, f});
            push_temp(s);
            return s;
        }
        if (at.attr == "weekday") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::DateTimeField, t_int(), {recv, const_int(6)});
        }
        if (at.attr == "replace") {
            if (!count(0)) return const_none();
            static const char* kNames[6] = {"year",  "month", "day",
                                            "hour",  "minute", "second"};
            VReg fields = call_runtime(RtFn::ListNew, mk_list(t_unknown()),
                                       {const_int(6), const_int(0)});
            push_temp(fields);
            for (int i = 0; i < 6; ++i)
                call_void(RtFn::ListAppend, {fields, const_int(-1), const_int(0)});
            for (auto& kw : c.keywords) {
                int idx = -1;
                for (int i = 0; i < 6; ++i)
                    if (kw.arg == kNames[i]) idx = i;
                if (idx < 0) {
                    err(kw.value->line, kw.value->col,
                        std::format("datetime.replace() got an unexpected keyword "
                                    "argument '{}'",
                                    kw.arg));
                    expr(*kw.value);
                    continue;
                }
                VReg v = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
                call_void(RtFn::ListSet, {fields, const_int(idx), v, const_int(0)});
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::DateTime) rt = new_ty(TyKind::DateTime);
            VReg v = call_runtime(RtFn::DateTimeReplace, rt, {recv, fields});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("datetime has no method '{}'", at.attr));
        return const_none();
    }
    if (recv_ty->kind == TyKind::TimeDelta) {
        if (at.attr == "total_seconds") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::TimeDeltaTotalSeconds, t_float(), {recv});
        }
        err(c.line, c.col, std::format("timedelta has no method '{}'", at.attr));
        return const_none();
    }
    if (recv_ty->kind == TyKind::Path) {
        if (at.attr == "exists" || at.attr == "is_file" || at.attr == "is_dir") {
            if (!count(0)) return const_none();
            RtFn fn = at.attr == "exists"    ? RtFn::PathExists
                      : at.attr == "is_file" ? RtFn::PathIsFile
                                             : RtFn::PathIsDir;
            return call_runtime(fn, t_bool(), {recv});
        }
        if (at.attr == "read_text") {
            if (!count(0)) return const_none();
            VReg s = call_runtime(RtFn::PathReadText, t_str(), {recv});
            push_temp(s);
            return s;
        }
        if (at.attr == "write_text") {
            if (!count(1)) return const_none();
            VReg s = expr(*c.args[0]);
            s = coerce(s, tc_.type_of(c.args[0].get()), t_str());
            return call_runtime(RtFn::PathWriteText, t_int(), {recv, s});
        }
        if (at.attr == "read_bytes") {
            if (!count(0)) return const_none();
            VReg s = call_runtime(RtFn::PathReadBytes, t_bytes(), {recv});
            push_temp(s);
            return s;
        }
        if (at.attr == "write_bytes") {
            if (!count(1)) return const_none();
            VReg s = expr(*c.args[0]);
            s = coerce(s, tc_.type_of(c.args[0].get()), t_bytes());
            return call_runtime(RtFn::PathWriteBytes, t_int(), {recv, s});
        }
        if (at.attr == "is_absolute") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::PathIsAbsolute, t_bool(), {recv});
        }
        if (at.attr == "joinpath") {
            if (!count(1)) return const_none();
            VReg s = expr(*c.args[0]);
            s = coerce(s, tc_.type_of(c.args[0].get()), t_str());
            VReg v = call_runtime(RtFn::PathJoin, new_ty(TyKind::Path), {recv, s});
            push_temp(v);
            return v;
        }
        if (at.attr == "relative_to") {
            if (!count(1)) return const_none();
            VReg s = expr(*c.args[0]);
            s = coerce(s, tc_.type_of(c.args[0].get()), t_str());
            VReg v = call_runtime(RtFn::PathRelativeTo, new_ty(TyKind::Path), {recv, s});
            push_temp(v);
            return v;
        }
        if (at.attr == "resolve" || at.attr == "absolute") {
            if (!count(0)) return const_none();
            RtFn fn = at.attr == "resolve" ? RtFn::PathResolve : RtFn::PathAbsolute;
            VReg v = call_runtime(fn, new_ty(TyKind::Path), {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "touch") {
            if (!count(0)) return const_none();
            call_runtime(RtFn::PathTouch, t_int(), {recv});
            return const_none();
        }
        if (at.attr == "iterdir") {
            if (!count(0)) return const_none();
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::List) rt = mk_list(new_ty(TyKind::Path));
            VReg v = call_runtime(RtFn::PathIterdir, rt, {recv});
            push_temp(v);
            return v;
        }
        if (at.attr == "glob") {
            if (!count(1)) return const_none();
            VReg pat = expr(*c.args[0]);
            pat = coerce(pat, tc_.type_of(c.args[0].get()), t_str());
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::List) rt = mk_list(new_ty(TyKind::Path));
            VReg v = call_runtime(RtFn::PathGlob, rt, {recv, pat});
            push_temp(v);
            return v;
        }
        if (at.attr == "with_suffix") {
            if (!count(1)) return const_none();
            VReg suf = expr(*c.args[0]);
            suf = coerce(suf, tc_.type_of(c.args[0].get()), t_str());
            VReg v = call_runtime(RtFn::PathWithSuffix, new_ty(TyKind::Path),
                                  {recv, suf});
            push_temp(v);
            return v;
        }
        if (at.attr == "unlink") {
            if (!count(0)) return const_none();
            VReg s = call_runtime(RtFn::PathStr, t_str(), {recv});
            push_temp(s);
            call_void(RtFn::OsRemove, {s});
            return const_none();
        }
        if (at.attr == "rmdir") {
            if (!count(0)) return const_none();
            VReg s = call_runtime(RtFn::PathStr, t_str(), {recv});
            push_temp(s);
            call_void(RtFn::OsRmdir, {s});
            return const_none();
        }
        if (at.attr == "mkdir") {
            if (!c.args.empty()) {
                err(c.line, c.col, "pathlib.Path.mkdir() takes only keyword arguments");
                return const_none();
            }
            VReg mode = const_int(0777);
            VReg parents = const_int(0);
            VReg exist_ok = const_int(0);
            for (auto& kw : c.keywords) {
                if (kw.arg == "mode") {
                    mode = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
                } else if (kw.arg == "parents") {
                    parents = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
                } else if (kw.arg == "exist_ok") {
                    exist_ok = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
                } else {
                    err(kw.value->line, kw.value->col,
                        std::format("pathlib.Path.mkdir() got an unexpected keyword "
                                    "argument '{}'",
                                    kw.arg));
                }
            }
            call_void(RtFn::PathMkdir, {recv, mode, parents, exist_ok});
            return const_none();
        }
        err(c.line, c.col, std::format("pathlib.Path has no method '{}'", at.attr));
        return const_none();
    }
    if (recv_ty->kind == TyKind::Popen) {
        if (at.attr == "wait") {
            if (!c.args.empty()) {
                err(c.line, c.col, "Popen.wait() takes only the timeout keyword");
                return const_none();
            }
            VReg to = const_float(-1.0);
            for (auto& kw : c.keywords) {
                if (kw.arg == "timeout") {
                    to = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_float());
                } else {
                    err(kw.value->line, kw.value->col,
                        std::format("Popen.wait() got an unexpected keyword argument '{}'",
                                    kw.arg));
                }
            }
            return call_runtime(RtFn::PopenWait, t_int(), {recv, to});
        }
        if (at.attr == "communicate") {
            if (!c.args.empty()) {
                err(c.line, c.col, "Popen.communicate() takes only the input keyword");
                return const_none();
            }
            VReg in = const_str("");
            for (auto& kw : c.keywords) {
                if (kw.arg == "input") {
                    in = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_str());
                } else {
                    err(kw.value->line, kw.value->col,
                        std::format("Popen.communicate() got an unexpected keyword "
                                    "argument '{}'",
                                    kw.arg));
                }
            }
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::Tuple) rt = mk_tuple({t_str(), t_str()});
            VReg v = call_runtime(RtFn::PopenCommunicate, rt, {recv, in});
            push_temp(v);
            return v;
        }
        if (at.attr == "kill" || at.attr == "terminate") {
            if (!count(0)) return const_none();
            call_void(at.attr == "kill" ? RtFn::PopenKill : RtFn::PopenTerminate, {recv});
            return const_none();
        }
        if (at.attr == "send_signal") {
            if (!count(1)) return const_none();
            VReg s = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            call_void(RtFn::PopenSendSignal, {recv, s});
            return const_none();
        }
        err(c.line, c.col, std::format("Popen has no method '{}'", at.attr));
        return const_none();
    }
    if (recv_ty->kind == TyKind::Deque) {
        TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
        int ko = own_kind(et);
        int kk = key_kind(et);
        if (at.attr == "append" || at.attr == "appendleft") {
            if (!count(1)) return const_none();
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            retain_elem(v, ko);
            call_void(at.attr == "append" ? RtFn::DequeAppend : RtFn::DequeAppendLeft,
                      {recv, v});
            return const_none();
        }
        if (at.attr == "pop" || at.attr == "popleft") {
            if (!count(0)) return const_none();
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind == TyKind::Unknown) rt = et;
            VReg v = call_runtime(
                at.attr == "pop" ? RtFn::DequePop : RtFn::DequePopleft, rt, {recv});
            if (ir_is_heap(rt)) push_temp(v);
            return v;
        }
        if (at.attr == "rotate") {
            if (!count(1)) return const_none();
            VReg n = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            call_void(RtFn::DequeRotate, {recv, n});
            return const_none();
        }
        if (at.attr == "clear") {
            if (!count(0)) return const_none();
            call_void(RtFn::DequeClear, {recv});
            return const_none();
        }
        if (at.attr == "reverse") {
            if (!count(0)) return const_none();
            call_void(RtFn::DequeReverse, {recv});
            return const_none();
        }
        if (at.attr == "extend" || at.attr == "extendleft") {
            if (!count(1)) return const_none();
            VReg xs = expr(*c.args[0]);
            call_void(at.attr == "extend" ? RtFn::DequeExtend : RtFn::DequeExtendLeft,
                      {recv, xs});
            return const_none();
        }
        if (at.attr == "insert") {
            if (!count(2)) return const_none();
            VReg idx = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            VReg v = expr(*c.args[1]);
            v = coerce(v, tc_.type_of(c.args[1].get()), et);
            retain_elem(v, ko);
            call_void(RtFn::DequeInsert, {recv, idx, v});
            return const_none();
        }
        if (at.attr == "remove" || at.attr == "count" || at.attr == "index") {
            if (!count(1)) return const_none();
            VReg v = expr(*c.args[0]);
            v = coerce(v, tc_.type_of(c.args[0].get()), et);
            RtFn fn = at.attr == "remove"  ? RtFn::DequeRemove
                      : at.attr == "count" ? RtFn::DequeCount
                                           : RtFn::DequeIndex;
            TypePtr rt = at.attr == "remove" ? t_none() : t_int();
            VReg r = call_runtime(fn, rt, {recv, v, const_int(kk)});
            if (at.attr == "remove") return const_none();
            return r;
        }
        if (at.attr == "copy") {
            if (!count(0)) return const_none();
            TypePtr rt = tc_.type_of(&c);
            if (!rt || rt->kind != TyKind::Deque) rt = mk_deque(et);
            VReg v = call_runtime(RtFn::DequeCopy, rt, {recv});
            push_temp(v);
            return v;
        }
        err(c.line, c.col, std::format("collections.deque has no method '{}'", at.attr));
        return const_none();
    }
    if (recv_ty->kind == TyKind::AsyncTimeout) {
        if (at.attr == "expired") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::TimeoutExpired, t_bool(), {recv});
        }
    }
    if (recv_ty->kind == TyKind::AsyncSemaphore) {
        if (at.attr == "acquire") {
            if (!count(0)) return const_none();
            VReg f = call_runtime(RtFn::SemAcquire, mk_future(t_bool()), {recv});
            push_temp(f);
            return f;
        }
        if (at.attr == "release") {
            if (!count(0)) return const_none();
            call_void(RtFn::SemRelease, {recv});
            return const_none();
        }
        if (at.attr == "locked") {
            if (!count(0)) return const_none();
            return call_runtime(RtFn::SemLocked, t_bool(), {recv});
        }
    }
    if (recv_ty->kind == TyKind::AsyncTaskGroup) {
        if (at.attr == "create_task") {
            if (!count(1)) return const_none();
            VReg coro = expr(*c.args[0]);
            VReg t = call_runtime(RtFn::TaskGroupAdd, mk_task(t_unknown()),
                                  {recv, coro});
            push_temp(t);
            return t;
        }
    }
    err(c.line, c.col,
        std::format("'{}' has no method '{}'", type_name(recv_ty), at.attr));
    return const_none();
}

VReg Lowerer::lower_method_generator(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
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
        else if (is_unknown(et)) push_temp_unknown(v);
        return v;
    }
    if (at.attr == "send") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "generator.send() takes exactly one argument");
            return const_none();
        }
        // P3-1b：send 值进 unknown 域要装箱（float 裸位模式会被 gen.sent
        // 当整数，0.327 基线已存在）；同步 resume 内消费，调用方临时份够用。
        VReg sv = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_unknown());
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::GenSend);
        in.args = {recv, sv};
        VReg v = def(t_unknown(), std::move(in));
        v = retype(v, et);
        if (ir_is_heap(et)) push_temp(v);
        else if (is_unknown(et)) push_temp_unknown(v);
        return v;
    }
    if (at.attr == "throw") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "generator.throw() takes exactly one argument");
            return const_none();
        }
        VReg ev = kNoReg;
        std::string cn;
        if (exc_class_name(*c.args[0], cn)) {
            // throw(ValueError) 等价于 throw(ValueError())。
            Inst ctor;
            ctor.op = IrOp::CallRt;
            ctor.imm = static_cast<int64_t>(RtFn::ExcValueNew);
            ctor.args = {const_str(cn), const_int(0)};
            ev = def_heap(mk_exception(cn), std::move(ctor));
        } else {
            ev = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_unknown());
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::GenThrow);
        in.args = {recv, ev};
        VReg v = def(t_unknown(), std::move(in));
        v = retype(v, et);
        if (ir_is_heap(et)) push_temp(v);
        else if (is_unknown(et)) push_temp_unknown(v);
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

VReg Lowerer::lower_method_async_generator(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
    TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
    if (at.attr == "__anext__") {
        if (!c.args.empty())
            err(c.line, c.col, "async generator.__anext__() takes no arguments");
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::AsyncGenANext);
        in.args = {recv};
        return def_heap(mk_coroutine(et), std::move(in));
    }
    if (at.attr == "asend") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "async generator.asend() takes exactly one argument");
            return const_none();
        }
        VReg sv = expr(*c.args[0]);
        TypePtr vt = vreg_ty(sv);
        if (ir_is_heap(vt)) rc_inc(sv);
        else if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(sv);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::AsyncGenAsend);
        in.args = {recv, sv};
        return def_heap(mk_coroutine(et), std::move(in));
    }
    if (at.attr == "athrow") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "async generator.athrow() takes exactly one argument");
            return const_none();
        }
        VReg ev = kNoReg;
        std::string cn;
        if (exc_class_name(*c.args[0], cn)) {
            // athrow(ValueError) 等价于 athrow(ValueError())。
            Inst ctor;
            ctor.op = IrOp::CallRt;
            ctor.imm = static_cast<int64_t>(RtFn::ExcValueNew);
            ctor.args = {const_str(cn), const_int(0)};
            ev = def_heap(mk_exception(cn), std::move(ctor));
        } else {
            ev = expr(*c.args[0]);
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::AsyncGenAthrow);
        in.args = {recv, ev};
        return def_heap(mk_coroutine(et), std::move(in));
    }
    if (at.attr == "aclose") {
        if (!c.args.empty())
            err(c.line, c.col, "async generator.aclose() takes no arguments");
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::AsyncGenAClose);
        in.args = {recv};
        return def_heap(mk_coroutine(t_none()), std::move(in));
    }
    err(c.line, c.col,
        std::format("'async generator' object has no method '{}'", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_task(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
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
        else if (is_unknown(et)) push_temp_unknown(v);
        return v;
    }
    if (at.attr == "exception") {
        VReg v = call_runtime(RtFn::AsyncioTaskException,
                              mk_optional(mk_exception("Exception")), {recv});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("'task' object has no method '{}'", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_future(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
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
    if (at.attr == "set_exception") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "future.set_exception() takes exactly one argument");
            return const_none();
        }
        VReg msg = expr(*c.args[0]);
        TypePtr mt = vreg_ty(msg);
        if (mt && mt->kind == TyKind::Exception)
            call_void(RtFn::FutureSetExceptionValue, {recv, msg});
        else
            call_void(RtFn::FutureSetException, {recv, msg});
        return const_none();
    }
    if (at.attr == "add_done_callback") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "future.add_done_callback() takes exactly one argument");
            return const_none();
        }
        VReg cb = expr(*c.args[0]);
        TypePtr ct = vreg_ty(cb);
        if (ir_is_heap(ct)) rc_inc(cb);
        else if (!ct || ct->kind == TyKind::Unknown) rc_inc_unknown(cb);
        call_void(RtFn::FutureAddCallback, {recv, cb});
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
        else if (is_unknown(et)) push_temp_unknown(v);
        return v;
    }
    if (at.attr == "exception") {
        if (!c.args.empty()) err(c.line, c.col, "future.exception() takes no arguments");
        VReg v = call_runtime(RtFn::FutureException,
                              mk_optional(mk_exception("Exception")), {recv});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("'future' object has no method '{}'", at.attr));
    return const_none();
}

}  // namespace pyadamas
