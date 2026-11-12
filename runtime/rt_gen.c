// rt_gen.c —— 生成器帧与 next/send/close
#include "rt_internal.h"

/* ── 生成器 ───────────────────────────────────────────────────── */

/* 帧负载：
   [state][status][sent][out][resume][closing][kind][waiter][exc][cur][slot..]
   resume 是状态机入口（void (*)(void*, int64_t)），由生成代码按 state
   分发；槽位属于帧，所以 yield/await 只需要返回，不需要保存栈。
   status: 0 未开始/挂起，1 已结束，2 异常逃出，3 awaiting，4 已排队。
   kind: 0 生成器，PY23_CORO_MAGIC 协程；waiter 是等待本对象的协程/
   gather/task；exc 是待注入的异常；cur 是协程当前等待的 awaitable
   （借用的观察指针，取消时靠它解绑）。 */

void py23_gen_finalize(void* g);

void gen_destroy(void* g) {
    /* 被引用计数抛弃：跑跨 yield 的 finally/with 清理。清理期间抛出的
       异常（__exit__ 自己失败）在 finalize 里吞掉。 */
    int64_t* p = (int64_t*)g;
    if (p[8]) {
        exc_free((Py23Exception*)p[8]);
        p[8] = 0;
    }
    if (p[7]) {
        py23_rc_dec((void*)p[7]);
        p[7] = 0;
    }
    py23_gen_finalize(g);
}

void* py23_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind) {
    if (n_slots < 0) panic_c("negative generator slot count");
    int64_t size = (PY23_GEN_HEADER + n_slots) * 8;
    void* g = rc_alloc(size, gen_destroy, (void (*)(void*))(uintptr_t)release, PY23_RC_OBJ);
    int64_t* p = (int64_t*)g;
    p[0] = 0;      /* state */
    p[1] = 0;      /* status */
    p[2] = 0;      /* sent */
    p[3] = 0;      /* out */
    p[4] = resume; /* 代码地址，不是堆对象 */
    p[5] = 0;      /* closing */
    p[6] = kind == 1 ? PY23_CORO_MAGIC : (kind == 2 ? PY23_AGEN_MAGIC : 0);
    p[7] = 0;      /* waiter */
    p[8] = 0;      /* 待注入异常（Py23Exception*，malloc） */
    p[9] = 0;      /* 当前等待的 awaitable（借用观察指针） */
    return g;
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
    Py23Handler h;
    h.prev = handler_top;
    h.mark = py23_rc_mark();
    h.frame = py23_frame_top;
    handler_top = &h;
    if (setjmp(h.env) != 0) {
        handler_top = h.prev;
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        py23_rc_cleanup(h.mark);
        exc_longjmp();
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, sent);
    handler_top = h.prev;
    py23_rc_cleanup(h.mark);
    if (p[1]) {
        *done = 1;
        return 0;
    }
    *done = 0;
    int64_t v = p[3];
    p[3] = 0;  /* 引用转移给调用方 */
    return v;
}

int64_t py23_gen_next(void* g) {
    int done = 0;
    int64_t v = gen_run(g, 0, &done);
    if (done) py23_raise_c("StopIteration", "");
    return v;
}

/* for 循环用：结束不抛异常，写 ok=0 让循环自然走出。 */
int64_t py23_gen_next_opt(void* g, int64_t* ok) {
    int done = 0;
    int64_t v = gen_run(g, 0, &done);
    if (ok) *ok = done ? 0 : 1;
    return v;
}

int64_t py23_gen_send(void* g, int64_t sent) {
    int done = 0;
    int64_t v = gen_run(g, sent, &done);
    if (done) py23_raise_c("StopIteration", "");
    return v;
}

/* yield from：驱动子生成器一步，结束时写 ok=0 而不抛异常。
   unknown 静态类型在这里兜底：不是生成器帧就报 TypeError。 */
int64_t py23_gen_send_opt(void* g, int64_t sent, int64_t* ok) {
    if (!g) py23_raise_c("TypeError", "'yield from' expects an iterable");
    Py23Rc* h = (Py23Rc*)g - 1;
    if (!rc_index_has(h) || h->kind != PY23_RC_OBJ ||
        h->size < (int64_t)PY23_GEN_HEADER * 8 || *(int64_t*)g == PY23_AWAIT_MAGIC)
        py23_raise_c("TypeError", "'yield from' expects a generator");
    int done = 0;
    int64_t v = gen_run(g, sent, &done);
    if (ok) *ok = done ? 0 : 1;
    return v;
}

/* 显式 close()：跑挂起点之后的 finally / __exit__，异常照常向外传。 */
void py23_gen_close(void* g) {
    int64_t* p = (int64_t*)g;
    if (!p || p[1]) return;
    p[5] = 1;
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    p[1] = 1;
    p[5] = 0;
    p[2] = 0;
}

/* 引用计数归零时的清理：与 close 相同，但把清理期间抛出的异常吞掉
   （CPython 在 GC 里也忽略 close 的异常）。 */
void py23_gen_finalize(void* g) {
    int64_t* p = (int64_t*)g;
    if (!p || p[1] || !p[4]) return;
    Py23Handler h;
    h.prev = handler_top;
    h.mark = py23_rc_mark();
    h.frame = py23_frame_top;
    handler_top = &h;
    if (setjmp(h.env) == 0) {
        p[5] = 1;
        ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    } else {
        py23_exc_clear();
    }
    handler_top = h.prev;
    py23_rc_cleanup(h.mark);
    p[1] = 1;
    p[5] = 0;
    p[2] = 0;
}

