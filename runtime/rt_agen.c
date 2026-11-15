// rt_agen.c —— 异步生成器驱动
#include "rt_internal.h"

/* ── 异步生成器（async for 的驱动）────────────────────────────── */

void* pa_asyncgen_anext(void* gen) {
    if (!frame_is_agen(gen))
        pa_raise_c("TypeError", "__anext__() expects an async generator");
    loop_ensure();
    PyAdamasAwait* d = await_new(4);
    d->driver_gen = (int64_t)gen;
    pa_rc_inc(gen);
    return d;
}

void* pa_asyncgen_asend(void* gen, int64_t sent) {
    if (!frame_is_agen(gen))
        pa_raise_c("TypeError", "asend() expects an async generator");
    loop_ensure();
    PyAdamasAwait* d = await_new(4);
    d->driver_gen = (int64_t)gen;
    pa_rc_inc(gen);
    d->driver_sent = sent;  // 调用方转交的引用
    return d;
}

void* pa_asyncgen_aclose(void* gen) {
    if (!frame_is_agen(gen))
        pa_raise_c("TypeError", "aclose() expects an async generator");
    loop_ensure();
    PyAdamasAwait* d = await_new(4);
    d->driver_gen = (int64_t)gen;
    pa_rc_inc(gen);
    d->driver_close = 1;
    return d;
}

void* pa_asyncgen_athrow(void* gen, PyAdamasExcValue* exc) {
    if (!frame_is_agen(gen))
        pa_raise_c("TypeError", "athrow() expects an async generator");
    loop_ensure();
    PyAdamasAwait* d = await_new(4);
    d->driver_gen = (int64_t)gen;
    pa_rc_inc(gen);
    d->driver_throw = (int64_t)pa_exc_from_value(exc);
    return d;
}

void driver_complete(PyAdamasAwait* d, int64_t value, PyAdamasException* exc) {
    if (d->done) {
        if (exc) exc_free(exc);
        else pa_rc_dec_unknown(value);
        return;
    }
    d->done = 1;
    void* owned = pending_take(d);
    if (d->driver_gen) {
        pa_rc_dec((void*)d->driver_gen);
        d->driver_gen = 0;
    }
    if (d->driver_sent) {
        pa_rc_dec_unknown(d->driver_sent);
        d->driver_sent = 0;
    }
    void* a = (void*)d->waiter;
    d->waiter = 0;
    if (a) deliver_owned(a, d, value, exc, 1);
    else {
        d->result = value;
        d->exc = (int64_t)exc;
    }
    if (owned) pa_rc_dec(owned);
}

void agen_drive(PyAdamasAwait* d) {
    if (!d || d->done) return;
    void* g = (void*)d->driver_gen;
    if (!g) {
        driver_complete(d, 0, exc_new("RuntimeError", "anext driver has no generator"));
        return;
    }
    int64_t* p = (int64_t*)g;
    int closing = d->driver_close != 0;
    int throwing = d->driver_throw != 0;
    // 上一轮内部 await 已经把结果/异常写进帧槽；这一轮不能再用
    // driver_sent（普通 anext 是 0）覆盖 p[2]。
    int internal = d->driver_await_result != 0;
    d->driver_await_result = 0;

    if (closing) {
        if (p[1] == 1 || p[1] == 2) {
            driver_complete(d, 0, NULL);
            return;
        }
        if (p[1] == 3) {
            // 正在等内部对象：把 GeneratorExit 注入挂起点，让 finally 展开。
            if (p[8]) exc_free((PyAdamasException*)p[8]);
            p[8] = (int64_t)exc_new("GeneratorExit", "");
            p[1] = 0;
        } else if (p[0] == 0) {
            // 从未启动：不执行函数体。
            p[1] = 1;
            driver_complete(d, 0, NULL);
            return;
        } else {
            p[5] = 1;  // 走生成代码的 close 清理分发
        }
    } else if (throwing) {
        if (p[1] == 1 || p[1] == 2) {
            exc_free((PyAdamasException*)d->driver_throw);
            d->driver_throw = 0;
            driver_complete(d, 0, exc_new("StopAsyncIteration", ""));
            return;
        }
        // 把异常放到帧里，走到挂起点或未启动入口后由 throw 分发重新抛出。
        if (p[8]) exc_free((PyAdamasException*)p[8]);
        p[8] = d->driver_throw;
        d->driver_throw = 0;
        p[1] = 0;
    } else {
        if (p[1] == 1) {
            driver_complete(d, 0, exc_new("StopAsyncIteration", ""));
            return;
        }
        if (p[1] == 2) {
            PyAdamasException* e = (PyAdamasException*)p[8];
            p[8] = 0;
            driver_complete(d, 0, e ? e : exc_new("RuntimeError", "async generator failed"));
            return;
        }
        if (p[1] == 3) {
            driver_complete(d, 0, exc_new("RuntimeError", "async generator is already running"));
            return;
        }
    }

    p[1] = 0;
    if (!closing) p[5] = 0;
    p[9] = 0;
    int64_t sent = d->driver_sent;
    d->driver_sent = 0;
    if (!internal) p[2] = sent;
    else if (sent) pa_rc_dec_unknown(sent);  // 内部恢复不应携带 send 值
    if (internal && (closing || throwing)) {
        // close/throw 抢在结果被消费之前：注入的异常取代刚送达的值，
        // 帧不会再读 p[2]，当场释放。
        if (p[2]) {
            pa_rc_dec_unknown(p[2]);
            p[2] = 0;
        }
    }
    PyAdamasHandler h;
    h.prev = handler_top;
    h.mark = pa_rc_mark();
    h.frame = pa_frame_top;
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_depth();
    if (PA_SETJMP(h.env) != 0) {
        handler_top = h.prev;
        PyAdamasException* e = pending_exc;
        pending_exc = NULL;
        pa_rc_cleanup(h.mark);
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        if (sent) pa_rc_dec_unknown(sent);
        // 异常跳过了 resume 的出口清理：帧里的堆槽当场释放。
        pa_gen_release_slots(g);
        pa_exc_unwind(exc_depth0);
        /* PEP 479：StopIteration 逃出异步生成器帧 -> RuntimeError。 */
        pa_exc_pep479(e, "async generator raised StopIteration");
        pa_trace_attach(e, g);
        if (closing && e && strcmp(e->type, "GeneratorExit") == 0) {
            exc_free(e);
            driver_complete(d, 0, NULL);
        } else {
            driver_complete(d, 0, e);
        }
        return;
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, sent);
    handler_top = h.prev;
    pa_exc_unwind(exc_depth0);
    pa_rc_cleanup(h.mark);
    p[5] = 0;
    p[2] = 0;
    if (sent) pa_rc_dec_unknown(sent);
    int64_t status = p[1];
    if (closing) {
        // close 分发正常走完，或注入的 GeneratorExit 被 finally 吞掉。
        driver_complete(d, 0, NULL);
        return;
    }
    if (status == 3) {
        // 生成器在等内部 awaitable：把驱动对象挂上去。
        void* w = (void*)p[3];
        p[3] = 0;
        p[9] = (int64_t)w;
        p[1] = 3;  // 标记：正在等内部对象，禁止并发 anext
        if (frame_is_coro(w)) {
            int64_t* q = (int64_t*)w;
            if (q[1] != 0) {
                pa_rc_dec(w);
                driver_complete(d, 0,
                                exc_new("RuntimeError", "coroutine was already awaited"));
                return;
            }
            q[7] = (int64_t)d;
            pa_rc_inc(d);
            q[1] = 4;
            ready_push(w);
        } else if (is_await_obj(w)) {
            start_awaitable(w, d);
        } else {
            driver_complete(d, 0, exc_new("TypeError", "object is not awaitable"));
            return;
        }
        return;
    }
    if (status == 1) {
        driver_complete(d, 0, exc_new("StopAsyncIteration", ""));
        return;
    }
    if (status == 2) {
        PyAdamasException* e = (PyAdamasException*)p[8];
        p[8] = 0;
        driver_complete(d, 0, e);
        return;
    }
    // status 0：产出了一个值
    {
        int64_t v = p[3];
        p[3] = 0;
        driver_complete(d, v, NULL);
    }
}

