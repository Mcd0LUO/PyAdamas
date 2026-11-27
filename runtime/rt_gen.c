// rt_gen.c —— 生成器帧与 next/send/close
#include "rt_internal.h"

/* b1：release 侧表登记函数（定义在文件末尾，pa_gen_new 先用）。 */
static void gen_rel_register(void* resume, void (*release)(void*));

/* ── 生成器 ───────────────────────────────────────────────────── */

/* PEP 479：StopIteration 不该从生成器/协程帧里逃到调用方（for 循环会
   把它当成「正常结束」静默提前收尾）。CPython 3.7+ 在帧边界把它转成
   RuntimeError，这里做同一件事：就地改类型与消息，保留对象身份，已经
   挂上的逻辑帧 traceback 也就跟着保留（__context__ 链本项目没建模）。 */
void pa_exc_pep479(PyAdamasException* e, const char* msg) {
    if (!e || !exc_is_a(e->type, "StopIteration")) return;
    size_t mn = strlen(msg);
    char* ty = (char*)malloc(sizeof("RuntimeError"));
    char* ms = (char*)malloc(mn + 1);
    if (!ty || !ms) panic_c("out of memory");
    memcpy(ty, "RuntimeError", sizeof("RuntimeError"));
    memcpy(ms, msg, mn + 1);
    free(e->type);
    free(e->message);
    e->type = ty;
    e->message = ms;
}

/* 帧负载：
   [state][status][sent][out][resume][closing][kind][waiter][exc][cur][slot..]
   resume 是状态机入口（void (*)(void*, int64_t)），由生成代码按 state
   分发；槽位属于帧，所以 yield/await 只需要返回，不需要保存栈。
   status: 0 未开始/挂起，1 已结束，2 异常逃出，3 awaiting，4 已排队。
   kind: 0 生成器，PYADAMAS_CORO_MAGIC 协程；waiter 是等待本对象的协程/
   gather/task；exc 是待注入的异常；cur 是协程当前等待的 awaitable
   （借用的观察指针，取消时靠它解绑）。 */

void pa_gen_finalize(void* g);
void pa_handler_discard_record(void* p);
void pa_gen_release_handlers(void* g);

/* 把 handler_top 退回到 stop，沿途的记录逐个归还 freelist。
 *
 * 生成器在 yield / 异常 / close 时，resume 内部可能压过若干 handler 记录
 * （try / finally / with 各一条）。这些记录是 pa_handler_push 从堆上分配
 * 的，生成器退出时必须还回去，否则每跑一次泄漏几百字节。
 *
 * 只处理 (stop, handler_top] 这一段：更外层（调用方）的记录不能动。 */
static void unwind_handlers_to(PyAdamasHandler* stop) {
    while (handler_top && handler_top != stop) {
        PyAdamasHandler* dead = handler_top;
        handler_top = dead->prev;
        pa_handler_discard_record(dead);
    }
    handler_top = stop;
}

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
    // finalize 之后生成器状态已是终态（p[1] != 0），不会再 resume。
    // 把它名下挂起中的 handler 记录归还 freelist。
    pa_gen_release_handlers(g);
    // out 里可能还留着生成器的 return 值：没人取走就在这里释放。
    if (p[3]) {
        pa_rc_dec_unknown(p[3]);
        p[3] = 0;
    }
}

void* pa_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind) {
    if (n_slots < 0) panic_c("negative generator slot count");
    int64_t size = (PYADAMAS_GEN_HEADER + n_slots) * 8;
    void* g = rc_alloc(size, gen_destroy, (void (*)(void*))(uintptr_t)release, PYADAMAS_RC_OBJ_GEN);
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
    gen_rel_register((void*)(uintptr_t)resume, (void (*)(void*))(uintptr_t)release);
    return g;
}

/* 释放帧里持有引用的槽：调用生成的 gcrel（逐槽 dec + 清零），
   幂等。异常从 resume 逃出时用它，避免槽引用一直挂到对象销毁。 */
void pa_gen_release_slots(void* g) {
    if (!g) return;
    /* 0.695：release 已由 resume 侧表提供（b1），RC 头不再存指针。 */
    pa_gen_release_indirect(g);
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
    h.suspend_owner = NULL;
    h.heap_alloc = 0;  /* 栈记录：不能进 freelist */
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_node_depth;
    /* 生成器体**永远**走 setjmp 路径（它有 C 级兜底，抓不到 _Unwind）。
       如果调用方正在零开销 try 的保护区里（pa_eh_depth > 0），生成器内部
       抛出的异常会被 exc_longjmp 误判成「走 _Unwind」，从而穿过这层 setjmp
       直接展开到调用方 —— 语义错乱（实测：throw 之后生成器状态不对）。
       所以进出生成器时把 pa_eh_depth 摘掉，出来再装回。 */
    int saved_eh_depth = pa_eh_depth;
    pa_eh_depth = 0;
    if (PA_SETJMP(h.env) != 0) {
        // 异常从 resume 里一路 longjmp 出来：resume 内部压过的 handler
        // 记录（try/finally/with 各一条）留在链上，但 handler_top 现在
        // 直接跳回 h.prev —— 那些记录既不在链上、也没人归还，永久泄漏。
        // ASan 实测 162_pep479 / 119_gen_except_suspend 各泄漏一条。
        // 从 h.prev 到 handler_top 之间（不含两端）的记录都是这次 resume
        // 压的，逐个归还。
        // 先固定住「gen_run 自己的 handler」：h 是本函数的栈变量，
        // 循环里不能反复读 h.prev —— handler_top 一变，那个比较就失去意义，
        // 而且 h 自己也可能被误判成要回收的记录（stack-use-after-return）。
        unwind_handlers_to(h.prev);
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
        /* PEP 479：StopIteration 逃出生成器帧 -> RuntimeError。转换放在
           trace 挂载之前，未捕获时打印的仍是这一层的逻辑帧。 */
        pa_exc_pep479(pending_exc, "generator raised StopIteration");
        pa_trace_attach(pending_exc, g);
        pa_eh_depth = saved_eh_depth;
        exc_longjmp();
    }
    pa_gen_set_handler_owner(g);
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, sent);
    pa_gen_set_handler_owner(NULL);
    handler_top = h.prev;
    pa_eh_depth = saved_eh_depth;
    /* 快路径：resume 没改变异常上下文/临时值栈时不必跨 TU 调用清理函数。 */
    if (pa_exc_node_depth != exc_depth0) pa_exc_unwind(exc_depth0);
    if (temp_len != h.mark) pa_rc_cleanup(h.mark);
    if (p[1]) {
        pa_eh_depth = saved_eh_depth;
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
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ_GEN ||
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
    if (!rc_index_has(h) || h->kind != PYADAMAS_RC_OBJ_GEN ||
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
    h.suspend_owner = NULL;
    h.heap_alloc = 0;  /* 栈记录：不能进 freelist */
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_node_depth;
    /* close() 重放 finally 时同样走 setjmp：调用方若在零开销保护区里，
       必须临时摘掉 pa_eh_depth，否则 finally 里抛的异常会绕过这层 setjmp。 */
    int saved_eh_depth = pa_eh_depth;
    pa_eh_depth = 0;
    if (PA_SETJMP(h.env) != 0) {
        unwind_handlers_to(h.prev);
        pa_gen_release_slots(g);
        pa_rc_cleanup(h.mark);
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        pa_exc_unwind(exc_depth0);
        /* close() 重放 finally 时逃出的 StopIteration 同样按 PEP 479 转。 */
        pa_exc_pep479(pending_exc, "generator raised StopIteration");
        pa_eh_depth = saved_eh_depth;
        exc_longjmp();
    }
    p[5] = 1;
    pa_gen_set_handler_owner(g);
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    pa_gen_set_handler_owner(NULL);
    handler_top = h.prev;
    if (pa_exc_node_depth != exc_depth0) pa_exc_unwind(exc_depth0);
    pa_eh_depth = saved_eh_depth;
    if (temp_len != h.mark) pa_rc_cleanup(h.mark);
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
    h.suspend_owner = NULL;
    h.heap_alloc = 0;  /* 栈记录：不能进 freelist */
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_node_depth;
    if (PA_SETJMP(h.env) == 0) {
        p[5] = 1;
        pa_gen_set_handler_owner(g);
        ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
        pa_gen_set_handler_owner(NULL);
    } else {
        pa_exc_clear();
    }
    if (pa_exc_node_depth != exc_depth0) pa_exc_unwind(exc_depth0);
    unwind_handlers_to(h.prev);
    if (temp_len != h.mark) pa_rc_cleanup(h.mark);
    /* 正常跑完时槽已释放（空操作）；清理中途抛异常则在这里补一次。 */
    pa_gen_release_slots(g);
    p[1] = 1;
    p[5] = 0;
    p[2] = 0;
}


/* ── b1：GEN release 侧表（静态 4096，线性探测，满则 panic；不 malloc）──
   设计：release 指针按生成器函数（resume 地址）唯一 —— 同一代码地址永远是同一个
   .Lgengcrel。pa_gen_new 登记，释放时 pa_gen_release_indirect 从 payload p[4]（resume）
   反查；表大小 4096，远超「生成器函数数」（实测见报告）。 */
#define GEN_REL_TBL_CAP 4096
struct PaGenRelSlot { void* resume; void (*release)(void*); };
static struct PaGenRelSlot gen_rel_tbl[GEN_REL_TBL_CAP];
static int64_t gen_rel_count;
static void gen_rel_register(void* resume, void (*release)(void*)) {
    if (!resume || !release) return;
    uint64_t idx = ((uintptr_t)resume >> 4) & (GEN_REL_TBL_CAP - 1);
    for (int64_t probe = 0; probe < GEN_REL_TBL_CAP; ++probe) {
        struct PaGenRelSlot* e = &gen_rel_tbl[idx];
        if (e->resume == NULL) { e->resume = resume; e->release = release; ++gen_rel_count; return; }
        if (e->resume == resume) return;  /* 同一函数：release 必相同，幂等 */
        idx = (idx + 1) & (GEN_REL_TBL_CAP - 1);
    }
    panic_c("generator release table full (4096)");
}
static void (*gen_rel_find(void* resume))(void*) {
    if (!resume) return NULL;
    uint64_t idx = ((uintptr_t)resume >> 4) & (GEN_REL_TBL_CAP - 1);
    for (int64_t probe = 0; probe < GEN_REL_TBL_CAP; ++probe) {
        if (gen_rel_tbl[idx].resume == NULL) return NULL;
        if (gen_rel_tbl[idx].resume == resume) return gen_rel_tbl[idx].release;
        idx = (idx + 1) & (GEN_REL_TBL_CAP - 1);
    }
    return NULL;
}
void pa_gen_release_indirect(void* p) {
    int64_t* g = (int64_t*)p;
    void (*rel)(void*) = gen_rel_find((void*)(uintptr_t)g[4]);
    if (!rel) panic_c("generator release: resume not registered");
    rel(p);
}
void pa_gen_release_report(void) {
    const char* e = getenv("PYADAMAS_GEN_TBL_STATS");
    if (e && *e && e[0] != '0')
        fprintf(stderr, "[gen-tbl] entries=%lld cap=%d\n", (long long)gen_rel_count, GEN_REL_TBL_CAP);
}
