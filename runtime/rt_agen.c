// rt_agen.c —— 异步生成器驱动
#include "rt_internal.h"

/* ── 异步生成器（async for 的驱动）────────────────────────────── */

void* py23_asyncgen_anext(void* gen) {
    if (!frame_is_agen(gen))
        py23_raise_c("TypeError", "__anext__() expects an async generator");
    loop_ensure();
    Py23Await* d = await_new(4);
    d->driver_gen = (int64_t)gen;
    py23_rc_inc(gen);
    return d;
}

void driver_complete(Py23Await* d, int64_t value, Py23Exception* exc) {
    if (d->done) {
        if (exc) exc_free(exc);
        else py23_rc_dec_unknown(value);
        return;
    }
    d->done = 1;
    void* owned = pending_take(d);
    if (d->driver_gen) {
        py23_rc_dec((void*)d->driver_gen);
        d->driver_gen = 0;
    }
    void* a = (void*)d->waiter;
    d->waiter = 0;
    if (a) deliver_owned(a, d, value, exc);
    else {
        d->result = value;
        d->exc = (int64_t)exc;
    }
    if (owned) py23_rc_dec(owned);
}

void agen_drive(Py23Await* d) {
    if (!d || d->done) return;
    void* g = (void*)d->driver_gen;
    if (!g) {
        driver_complete(d, 0, exc_new("RuntimeError", "anext driver has no generator"));
        return;
    }
    int64_t* p = (int64_t*)g;
    if (p[1] == 1) {
        driver_complete(d, 0, exc_new("StopAsyncIteration", ""));
        return;
    }
    if (p[1] == 2) {
        Py23Exception* e = (Py23Exception*)p[8];
        p[8] = 0;
        driver_complete(d, 0, e ? e : exc_new("RuntimeError", "async generator failed"));
        return;
    }
    if (p[1] == 3) {
        driver_complete(d, 0, exc_new("RuntimeError", "async generator is already running"));
        return;
    }

    p[1] = 0;
    p[5] = 0;
    p[9] = 0;
    Py23Handler h;
    h.prev = handler_top;
    h.mark = py23_rc_mark();
    handler_top = &h;
    if (setjmp(h.env) != 0) {
        handler_top = h.prev;
        Py23Exception* e = pending_exc;
        pending_exc = NULL;
        py23_rc_cleanup(h.mark);
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        driver_complete(d, 0, e);
        return;
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    handler_top = h.prev;
    py23_rc_cleanup(h.mark);
    p[5] = 0;
    p[2] = 0;
    int64_t status = p[1];
    if (status == 3) {
        // 生成器在等内部 awaitable：把驱动对象挂上去。
        void* w = (void*)p[3];
        p[3] = 0;
        p[9] = (int64_t)w;
        p[1] = 3;  // 标记：正在等内部对象，禁止并发 anext
        if (frame_is_coro(w)) {
            int64_t* q = (int64_t*)w;
            if (q[1] != 0) {
                py23_rc_dec(w);
                driver_complete(d, 0,
                                exc_new("RuntimeError", "coroutine was already awaited"));
                return;
            }
            q[7] = (int64_t)d;
            py23_rc_inc(d);
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
        Py23Exception* e = (Py23Exception*)p[8];
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

