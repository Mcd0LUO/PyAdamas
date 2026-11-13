// rt_gen.c —— 生成器帧与 next/send/close
#include "rt_internal.h"

/* ── 生成器 ───────────────────────────────────────────────────── */

/* 帧负载：
   [state][status][sent][out][resume][closing][kind][waiter][exc][cur][slot..]
   resume 是状态机入口（void (*)(void*, int64_t)），由生成代码按 state
   分发；槽位属于帧，所以 yield/await 只需要返回，不需要保存栈。
   status: 0 未开始/挂起，1 已结束，2 异常逃出，3 awaiting，4 已排队。
   kind: 0 生成器，PYADAMAS_CORO_MAGIC 协程；waiter 是等待本对象的协程/
   gather/task；exc 是待注入的异常；cur 是协程当前等待的 awaitable
   （借用的观察指针，取消时靠它解绑）。 */

void pa_gen_finalize(void* g);

void gen_destroy(void* g) {
    /* 被引用计数抛弃：跑跨 yield 的 finally/with 清理。清理期间抛出的
       异常（__exit__ 自己失败）在 finalize 里吞掉。 */
    int64_t* p = (int64_t*)g;
    if (p[8]) {
        exc_free((PyAdamasException*)p[8]);
        p[8] = 0;
    }
    if (p[7]) {
        pa_rc_dec((void*)p[7]);
        p[7] = 0;
    }
    pa_gen_finalize(g);
    // out 里可能还留着生成器的 return 值：没人取走就在这里释放。
    if (p[3]) {
        pa_rc_dec_unknown(p[3]);
        p[3] = 0;
    }
}

void* pa_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind) {
    if (n_slots < 0) panic_c("negative generator slot count");
    int64_t size = (PYADAMAS_GEN_HEADER + n_slots) * 8;
    void* g = rc_alloc(size, gen_destroy, (void (*)(void*))(uintptr_t)release, PYADAMAS_RC_OBJ);
    int64_t* p = (int64_t*)g;
    p[0] = 0;      /* state */
    p[1] = 0;      /* status */
    p[2] = 0;      /* sent */
    p[3] = 0;      /* out */
    p[4] = resume; /* 代码地址，不是堆对象 */
    p[5] = 0;      /* closing */
    p[6] = kind == 1 ? PYADAMAS_CORO_MAGIC : (kind == 2 ? PYADAMAS_AGEN_MAGIC : 0);
    p[7] = 0;      /* waiter */
    p[8] = 0;      /* 待注入异常（PyAdamasException*，malloc） */
    p[9] = 0;      /* 当前等待的 awaitable（借用观察指针） */
    return g;
}

/* 释放帧里持有引用的槽：调用生成的 gcrel（逐槽 dec + 清零），
   幂等。异常从 resume 逃出时用它，避免槽引用一直挂到对象销毁。 */
void pa_gen_release_slots(void* g) {
    if (!g) return;
    PyAdamasRc* h = (PyAdamasRc*)g - 1;
    if (h->release) h->release(g);
}

int64_t gen_run(void* g, int64_t sent, int* done) {
    int64_t* p = (int64_t*)g;
    if (p[1]) {
        *done = 1;
        return 0;
    }
    p[2] = sent;
    p[5] = 0;
    /* 异常可能从 resume 里一路 longjmp 出去：这里兜一层，把它标记成
       aborted（2），否则生成器对象销毁时会把已经跑过的 finally/__exit__
       再跑一遍。异常本身照常转抛给调用方。 */
    PyAdamasHandler h;
    h.prev = handler_top;
    h.mark = pa_rc_mark();
    h.frame = pa_frame_top;
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_depth();
    if (setjmp(h.env) != 0) {
        handler_top = h.prev;
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        /* 异常跳过了 resume 的出口清理：帧里的堆槽现在就要释放，
           否则只要生成器对象还被引用着，这些值就永远不回收。 */
        pa_gen_release_slots(g);
        pa_rc_cleanup(h.mark);
        /* resume 里换上的异常上下文也要退掉，恢复成调用方那一份。 */
        pa_exc_unwind(exc_depth0);
        exc_longjmp();
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, sent);
    handler_top = h.prev;
    pa_exc_unwind(exc_depth0);
    pa_rc_cleanup(h.mark);
    if (p[1]) {
        *done = 1;
        return 0;
    }
    *done = 0;
    int64_t v = p[3];
    p[3] = 0;  /* 引用转移给调用方 */
    return v;
}

int64_t pa_gen_next(void* g) {
    int done = 0;
    int64_t v = gen_run(g, 0, &done);
    if (done) pa_raise_c("StopIteration", "");
    return v;
}

/* for 循环用：结束不抛异常，写 ok=0 让循环自然走出。 */
int64_t pa_gen_next_opt(void* g, int64_t* ok) {
    int done = 0;
    int64_t v = gen_run(g, 0, &done);
    if (ok) *ok = done ? 0 : 1;
    return v;
}

int64_t pa_gen_send(void* g, int64_t sent) {
    int done = 0;
    int64_t v = gen_run(g, sent, &done);
    if (done) pa_raise_c("StopIteration", "");
    return v;
}

/* yield from：驱动子生成器一步，结束时写 ok=0 而不抛异常。
   unknown 静态类型在这里兜底：不是生成器帧就报 TypeError。 */
int64_t pa_gen_send_opt(void* g, int64_t sent, int64_t* ok) {
    if (!g) pa_raise_c("TypeError", "'yield from' expects an iterable");
    PyAdamasRc* h = (PyAdamasRc*)g - 1;
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ ||
        h->size < (int64_t)PYADAMAS_GEN_HEADER * 8 || *(int64_t*)g == PYADAMAS_AWAIT_MAGIC)
        pa_raise_c("TypeError", "'yield from' expects a generator");
    int done = 0;
    int64_t v = gen_run(g, sent, &done);
    if (ok) *ok = done ? 0 : 1;
    if (done) {
        // 子生成器结束：out 里是它的 return 值，引用转移给外层。
        int64_t* p = (int64_t*)g;
        int64_t rv = p[3];
        p[3] = 0;
        return rv;
    }
    return v;
}

/* throw / yield from 转发共用：确认 g 是生成器帧，否则按 TypeError 报错。 */
static int64_t* gen_frame_or_raise(void* g, const char* what) {
    if (!g) pa_raise_c("TypeError", what);
    PyAdamasRc* h = (PyAdamasRc*)g - 1;
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ ||
        h->size < (int64_t)PYADAMAS_GEN_HEADER * 8 || *(int64_t*)g == PYADAMAS_AWAIT_MAGIC)
        pa_raise_c("TypeError", what);
    return (int64_t*)g;
}

/* gen.throw(exc)：把异常注入当前挂起点，由生成代码的 throw 分发重新抛出，
   生成器自己的 try/except 能接住。已经结束的生成器没有挂起点可注入，
   按 CPython 语义把异常直接抛给调用方。 */
int64_t pa_gen_throw(void* g, PyAdamasExcValue* exc) {
    int64_t* p = gen_frame_or_raise(g, "'throw' expects a generator");
    if (!exc) pa_raise_c("TypeError", "throw() expects an exception");
    if (p[1]) pa_raise_value(exc);
    if (p[8]) {
        exc_free((PyAdamasException*)p[8]);
        p[8] = 0;
    }
    p[8] = (int64_t)pa_exc_from_value(exc);
    int done = 0;
    int64_t v = gen_run(g, 0, &done);
    if (done) pa_raise_c("StopIteration", "");
    return v;
}

/* yield from 的 throw 转发：exc 是 owned 的 PyAdamasException*，交给子生成器
   的挂起点。子生成器结束（StopIteration）时把它的 return 值交给外层作为
   yield from 的值；其余异常从子生成器逃出后原样向外层传播。 */
int64_t pa_gen_throw_opt(void* g, int64_t exc, int64_t* ok) {
    int64_t* p = gen_frame_or_raise(g, "'yield from' expects a generator");
    if (p[1]) {
        // 子生成器早已结束：异常在 yield from 处重新抛出。
        PyAdamasException* e = (PyAdamasException*)(uintptr_t)exc;
        if (!e) e = exc_new("RuntimeError", "throw() with no exception");
        exc_free(pending_exc);
        pending_exc = e;
        exc_longjmp();
    }
    if (p[8]) {
        exc_free((PyAdamasException*)p[8]);
        p[8] = 0;
    }
    p[8] = exc;
    int done = 0;
    int64_t v = gen_run(g, 0, &done);
    if (ok) *ok = done ? 0 : 1;
    if (done) {
        int64_t rv = p[3];
        p[3] = 0;
        return rv;
    }
    return v;
}

/* 显式 close()：跑挂起点之后的 finally / __exit__。清理期间抛出的异常
   同样要先把帧槽释放掉，再原样向外传。 */
void pa_gen_close(void* g) {
    int64_t* p = (int64_t*)g;
    if (!p || p[1]) return;
    PyAdamasHandler h;
    h.prev = handler_top;
    h.mark = pa_rc_mark();
    h.frame = pa_frame_top;
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_depth();
    if (setjmp(h.env) != 0) {
        handler_top = h.prev;
        pa_gen_release_slots(g);
        pa_rc_cleanup(h.mark);
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        pa_exc_unwind(exc_depth0);
        exc_longjmp();
    }
    p[5] = 1;
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    handler_top = h.prev;
    pa_exc_unwind(exc_depth0);
    pa_rc_cleanup(h.mark);
    pa_gen_release_slots(g);
    p[1] = 1;
    p[5] = 0;
    p[2] = 0;
}

/* 引用计数归零时的清理：与 close 相同，但把清理期间抛出的异常吞掉
   （CPython 在 GC 里也忽略 close 的异常）。 */
void pa_gen_finalize(void* g) {
    int64_t* p = (int64_t*)g;
    if (!p || p[1] || !p[4]) return;
    PyAdamasHandler h;
    h.prev = handler_top;
    h.mark = pa_rc_mark();
    h.frame = pa_frame_top;
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_depth();
    if (setjmp(h.env) == 0) {
        p[5] = 1;
        ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    } else {
        pa_exc_clear();
    }
    pa_exc_unwind(exc_depth0);
    handler_top = h.prev;
    pa_rc_cleanup(h.mark);
    /* 正常跑完时槽已释放（空操作）；清理中途抛异常则在这里补一次。 */
    pa_gen_release_slots(g);
    p[1] = 1;
    p[5] = 0;
    p[2] = 0;
}

