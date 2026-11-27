// rt_async.c —— 协程、事件循环、sleep/gather、取消与 run
#include "rt_internal.h"

/* ── 协程与事件循环 ───────────────────────────────────────────── */

/* 运行时 awaitable：sleep / gather。magic 用来和协程帧区分。
   waiter 持有协程或上层 gather 的一个引用；完成时连同结果一起转交。 */

PyAdamasList* loop_ready;
PyAdamasList* loop_timers;
PyAdamasList* loop_pending;  /* 已启动但还没完成的 gather / Task / Future / fd */
PyAdamasList* loop_fds;
void* loop_roots[4];
int loop_roots_registered;
int loop_running;
void* loop_root;
int loop_root_state;  /* 0 pending, 1 done, 2 failed */
int64_t loop_root_result;
PyAdamasException* loop_root_exc;

int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int frame_is_coro(void* p) {
    if (!p) return 0;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PYADAMAS_RC_OBJ_GEN || h->size < (int64_t)PYADAMAS_GEN_HEADER * 8) return 0;
    return ((int64_t*)p)[6] == PYADAMAS_CORO_MAGIC;
}

int frame_is_agen(void* p) {
    if (!p) return 0;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PYADAMAS_RC_OBJ_GEN || h->size < (int64_t)PYADAMAS_GEN_HEADER * 8) return 0;
    return ((int64_t*)p)[6] == PYADAMAS_AGEN_MAGIC;
}

int is_await_obj(void* p) {
    if (!p) return 0;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PYADAMAS_RC_OBJ || h->size < (int64_t)sizeof(PyAdamasAwait)) return 0;
    return *(int64_t*)p == PYADAMAS_AWAIT_MAGIC;
}

void await_destroy(void* p) {
    PyAdamasAwait* w = (PyAdamasAwait*)p;
    pa_rc_dec_unknown(w->result);
    if (w->exc) exc_free((PyAdamasException*)w->exc);
    if (w->waiter) pa_rc_dec((void*)w->waiter);
    if (w->task_coro) pa_rc_dec((void*)w->task_coro);
    if (w->driver_gen) pa_rc_dec((void*)w->driver_gen);
    if (w->driver_sent) pa_rc_dec_unknown(w->driver_sent);
    if (w->driver_throw) exc_free((PyAdamasException*)w->driver_throw);
    if (w->waiters) pa_rc_dec(w->waiters);
    if (w->results) pa_rc_dec(w->results);
    if (w->children) pa_rc_dec(w->children);
    if (w->callbacks) pa_rc_dec(w->callbacks);
}

void await_gc_release(void* p) {
    PyAdamasAwait* w = (PyAdamasAwait*)p;
    if (w->result) pa_gc_unref_child((void*)w->result);
    if (w->waiter) pa_gc_unref_child((void*)w->waiter);
    if (w->task_coro) pa_gc_unref_child((void*)w->task_coro);
    if (w->driver_gen) pa_gc_unref_child((void*)w->driver_gen);
    if (w->driver_sent) pa_gc_unref_child((void*)w->driver_sent);
    if (w->waiters) pa_gc_unref_child(w->waiters);
    if (w->results) pa_gc_unref_child(w->results);
    if (w->children) pa_gc_unref_child(w->children);
    if (w->callbacks) pa_gc_unref_child(w->callbacks);
}

/* GC 清扫 / 进程退出不跑析构器，但异常对象是 malloc 的普通内存，
   必须在这里单独回收，否则挂在帧或 Task/Future 上的异常会泄漏。 */
void free_attached_exception(void* payload, int64_t size) {
    if (!payload || size < 8) return;
    int64_t* p = (int64_t*)payload;
    if (p[0] == PYADAMAS_EXCVAL_MAGIC && size >= (int64_t)sizeof(PyAdamasExcValue)) {
        // 异常值对象：type/message 是 malloc 的，dtor 在 GC 路径不会跑。
        PyAdamasExcValue* v = (PyAdamasExcValue*)payload;
        free(v->type);
        free(v->message);
        v->type = NULL;
        v->message = NULL;
        return;
    }
    if (p[0] == PYADAMAS_AWAIT_MAGIC && size >= (int64_t)sizeof(PyAdamasAwait)) {
        PyAdamasAwait* w = (PyAdamasAwait*)payload;
        if (w->driver_throw) {
            exc_free((PyAdamasException*)w->driver_throw);
            w->driver_throw = 0;
        }
        if (w->exc) {
            exc_free((PyAdamasException*)w->exc);
            w->exc = 0;
        }
        return;
    }
    if (size >= (int64_t)PYADAMAS_GEN_HEADER * 8 &&
        (p[6] == PYADAMAS_CORO_MAGIC || p[6] == PYADAMAS_AGEN_MAGIC)) {
        if (p[8]) {
            exc_free((PyAdamasException*)p[8]);
            p[8] = 0;
        }
    }
}

PyAdamasAwait* await_new(int64_t kind) {
    PyAdamasAwait* w = (PyAdamasAwait*)rc_alloc((int64_t)sizeof(PyAdamasAwait), await_destroy,
                                        await_gc_release, PYADAMAS_RC_OBJ);
    w->magic = PYADAMAS_AWAIT_MAGIC;
    w->kind = kind;
    // 默认结果按引用管理；asyncio.Queue[float] 的 Future 会改成 0。
    w->result_kind = 1;
    return w;
}

void loop_ensure(void) {
    /* 先把四个全局槽登记成 GC 根，再连发 4 个 pa_list_new。原顺序是「先
       分配、后登记根」：第 2/3/4 次 pa_list_new 里如果恰好触发主收集，
       前面刚建好、只挂在全局槽上的 list 不在任何根集合里（C 全局对 major
       的标记阶段不可见），会被当垃圾清扫；后面 ready_push 之类写
       loop_ready->data 就是 use-after-free（与 0.181 异常路径同类）。
       根表登记的是「槽的地址」，提前登记后每次赋值立刻可见。 */
    if (!loop_roots_registered) {
        loop_roots[0] = (void*)&loop_ready;
        loop_roots[1] = (void*)&loop_timers;
        loop_roots[2] = (void*)&loop_pending;
        loop_roots[3] = (void*)&loop_fds;
        pa_gc_add_roots((void**)loop_roots, 4);
        loop_roots_registered = 1;
    }
    if (!loop_ready) {
        loop_ready = pa_list_new(8, 1);
        loop_timers = pa_list_new(8, 1);
        loop_pending = pa_list_new(8, 1);
        loop_fds = pa_list_new(8, 1);
    }
}

void ready_push(void* c) { pa_list_append(loop_ready, (int64_t)c, 2); }

/* 0.652：就绪队列改成「头指针 + 批量压缩」。
 *
 * 原实现每次 pop 都把整个数组前移一格（memmove O(n)）：20000 个就绪任务、
 * 40 万次切换时这是 85% 的总时间（perf：__memmove_evex_unaligned_erms）。
 * 现在是 O(1) 取头，只在「头指针过半且 ≥1024」时压缩一次，均摊 O(1)。
 *
 * 已消费的槽位显式清零：loop_ready 本身是 GC 根，槽里留旧指针会让
 * 已经调度过的任务被错误地保活到下一次压缩。清零不 dec —— 引用已经
 * 转移给调用方（与原实现一致）。 */
static int64_t ready_head;

void* ready_pop(void) {
    if (!loop_ready || ready_head >= loop_ready->len) {
        if (loop_ready) loop_ready->len = 0;
        ready_head = 0;
        return NULL;
    }
    void* c = (void*)loop_ready->data[ready_head];
    loop_ready->data[ready_head] = 0;
    ++ready_head;
    if (ready_head >= loop_ready->len) {
        loop_ready->len = 0;
        ready_head = 0;
    } else if (ready_head >= 1024 && ready_head * 2 >= loop_ready->len) {
        int64_t rest = loop_ready->len - ready_head;
        memmove(&loop_ready->data[0], &loop_ready->data[ready_head],
                sizeof(int64_t) * (size_t)rest);
        loop_ready->len = rest;
        ready_head = 0;
    }
    return c;
}

/* 从表里摘下第 i 项，引用交给调用方。 */
int64_t list_take_at(PyAdamasList* l, int64_t i) {
    int64_t v = l->data[i];
    memmove(&l->data[i], &l->data[i + 1], sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
    return v;
}

void list_clear_owned(PyAdamasList* l) {
    if (!l) return;
    int64_t n = l->len;
    l->len = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (!l->data[i]) continue;  // 0.652：就绪队列的已消费槽位清零过
        pa_rc_dec((void*)l->data[i]);
    }
}

void* pending_take(void* w) {
    if (!loop_pending) return NULL;
    for (int64_t i = 0; i < loop_pending->len; ++i)
        if (loop_pending->data[i] == (int64_t)w) {
            list_take_at(loop_pending, i);
            return w;
        }
    return NULL;
}

void deliver_owned(void* waiter, void* child, int64_t value, PyAdamasException* exc,
                   int64_t value_kind);
void start_awaitable(void* w, void* waiter);
void finish_gather(PyAdamasAwait* g);
void coro_inject(void* c, PyAdamasException* e);
void agen_drive(PyAdamasAwait* d);
void cancel_chain(void* w);
void cancel_suspended(void* c, PyAdamasException* e);
void future_cancel(PyAdamasAwait* f);
void gather_cancel(PyAdamasAwait* g);

/* gather 结果元素的所有权类别：0 裸值 / 1 堆引用 / 2 unknown。 */
static void gather_release_result(int64_t value, int64_t kind) {
    if (!value) return;
    if (kind == 1) pa_rc_dec((void*)(uintptr_t)value);
    else if (kind == 2) pa_rc_dec_unknown(value);
}

/* 完成一个 gather：把结果/异常交给它的 waiter，并释放 pending 里的持有引用。 */
void finish_gather(PyAdamasAwait* g) {
    if (g->done) return;
    g->done = 1;
    void* owned = pending_take(g);
    void* a = (void*)g->waiter;
    g->waiter = 0;
    if (g->exc) {
        PyAdamasException* e = (PyAdamasException*)g->exc;
        g->exc = 0;
        if (a) deliver_owned(a, g, 0, e, 1);
        else exc_free(e);
    } else {
        int64_t r = (int64_t)g->results;
        g->results = 0;
        if (a) deliver_owned(a, g, r, NULL, 1);
        else g->result = r;
    }
    if (owned) pa_rc_dec(owned);
}

void gather_child_fail(PyAdamasAwait* g, PyAdamasException* e) {
    if (!g->exc) g->exc = (int64_t)e;
    else exc_free(e);
    if (g->remaining > 0) --g->remaining;
}

/* 启动一个 awaitable。调用方把 w 的一个引用交给本函数。 */
void start_gather(PyAdamasAwait* w, void* waiter) {
    w->waiter = (int64_t)waiter;
    pa_rc_inc(waiter);
    pa_list_append(loop_pending, (int64_t)w, 2);  // 接管调用方的引用
    w->starting = 1;
    for (int64_t i = 0; i < w->children->len; ++i) {
        void* child = (void*)w->children->data[i];
        if (frame_is_coro(child)) {
            int64_t* p = (int64_t*)child;
            if (p[1] != 0) {
                gather_child_fail(w, exc_new("RuntimeError", "coroutine was already awaited"));
            } else {
                p[7] = (int64_t)w;
                pa_rc_inc(w);
                p[1] = 4;
                pa_rc_inc(child);
                ready_push(child);
            }
        } else if (is_await_obj(child)) {
            pa_rc_inc(child);
            /* start_awaitable 内部还会分配（waiters 列表等），child 只在
               本帧 C 局部变量里，先 pin 住。 */
            pa_gc_pin(child);
            start_awaitable(child, w);
            pa_gc_unpin(child);
        } else {
            gather_child_fail(w, exc_new("TypeError", "gather() argument is not awaitable"));
        }
    }
    w->starting = 0;
    if (w->remaining <= 0 && !w->done) finish_gather(w);
}

/* ── asyncio.timeout：到期向当前协程注入 TimeoutError ─────────── */

typedef struct {
    PyAdamasAwait* timer; /* 已注册的 kind 9 定时器（可能已触发） */
    int64_t expired;
    double secs;
} PyAdamasTimeout;

void* loop_active_coro; /* coro_step 执行期间指向当前协程 */

static void timeout_detach(PyAdamasTimeout* to) {
    PyAdamasAwait* t = to->timer;
    if (!t) return;
    to->timer = NULL;
    if (loop_timers)
        for (int64_t i = 0; i < loop_timers->len; ++i)
            if (loop_timers->data[i] == (int64_t)t) {
                list_take_at(loop_timers, i);
                pa_rc_dec(t); /* 定时器表的引用 */
                break;
            }
    pa_rc_dec(t); /* timeout 自己的引用；dtor 负责 dec driver_gen */
}

static void timeout_destroy(void* p) { timeout_detach((PyAdamasTimeout*)p); }

void* pa_timeout_new(double secs) {
    loop_ensure();
    PyAdamasTimeout* to = (PyAdamasTimeout*)rc_alloc((int64_t)sizeof(PyAdamasTimeout),
                                             timeout_destroy, NULL, PYADAMAS_RC_OBJ);
    to->secs = secs;
    return to;
}

void pa_timeout_enter(void* tp) {
    if (!tp) pa_raise_c("TypeError", "__aenter__() expects a Timeout");
    PyAdamasTimeout* to = (PyAdamasTimeout*)tp;
    if (to->timer) pa_raise_c("RuntimeError", "timeout is already active");
    if (!loop_active_coro)
        pa_raise_c("RuntimeError", "timeout() must be used inside a coroutine");
    PyAdamasAwait* t = await_new(9);
    // remaining 只存裸指针，不参与 RC；driver_gen 由 dtor 统一释放。
    t->remaining = (int64_t)(uintptr_t)to;
    t->driver_gen = (int64_t)(uintptr_t)loop_active_coro;
    pa_rc_inc(loop_active_coro);
    int64_t ms = 0;
    if (to->secs > 0) {
        double v = to->secs * 1000.0;
        ms = v > 9e15 ? (int64_t)9e15 : (int64_t)(v + 0.5);
    }
    t->deadline_ms = now_ms() + ms;
    pa_rc_inc(t); /* 定时器表引用 */
    pa_list_append(loop_timers, (int64_t)t, 2);
    to->timer = t;
}

int64_t pa_timeout_exit(void* tp) {
    if (!tp) return 0;
    PyAdamasTimeout* to = (PyAdamasTimeout*)tp;
    int64_t expired = to->expired;
    timeout_detach(to);
    return expired;
}

int64_t pa_timeout_expired(void* tp) {
    if (!tp) return 0;
    return ((PyAdamasTimeout*)tp)->expired;
}

/* 定时器到期：标记 timeout、向目标协程注入异常。消费 fire 拿到的两个引用。 */
static void timeout_fire(PyAdamasAwait* t) {
    PyAdamasTimeout* to = (PyAdamasTimeout*)(uintptr_t)t->remaining;
    void* target = (void*)(uintptr_t)t->driver_gen;
    if (to) {
        to->expired = 1;
        to->timer = NULL;
    }
    if (target) cancel_suspended(target, exc_new("TimeoutError", ""));
    // driver_gen 留给 await_destroy 释放；这里只释放 timer 自身的两个引用。
    pa_rc_dec(t);
    pa_rc_dec(t);
}

/* ── asyncio.wait_for：可等待对象 + 超时竞速（kind 8） ────────── */

static void timer_remove(PyAdamasAwait* w) {
    for (int64_t i = 0; i < loop_timers->len; ++i)
        if (loop_timers->data[i] == (int64_t)w) {
            list_take_at(loop_timers, i);
            pa_rc_dec(w);
            return;
        }
}

void* pa_asyncio_wait_for(void* aw, double secs) {
    loop_ensure();
    if (!frame_is_coro(aw) && !is_await_obj(aw))
        pa_raise_c("TypeError", "asyncio.wait_for() expects an awaitable");
    PyAdamasAwait* w = await_new(8);
    /* children 列表和 create_task 都要继续分配；w 只在本帧 C 局部变量里。 */
    pa_gc_pin(w);
    w->children = pa_list_new(1, 1);
    double v = secs * 1000.0;
    int64_t ms = 0;
    if (secs > 0) ms = v > 9e15 ? (int64_t)9e15 : (int64_t)(v + 0.5);
    w->deadline_ms = now_ms() + ms;
    void* t = pa_asyncio_create_task(aw, 2);  // 内部 awaitable 类型不可见，按 unknown 释放
    pa_list_append(w->children, (int64_t)t, 2);  // 接管 create_task 的引用
    pa_gc_unpin(w);
    return w;
}

/* 定时器到期：取消子任务，把 TimeoutError 交给等待者。消费定时器引用。 */
static void wait_for_timeout(PyAdamasAwait* w) {
    if (!w->done) {
        w->done = 1;
        if (w->children && w->children->len > 0) {
            void* t = (void*)(uintptr_t)w->children->data[0];
            pa_rc_inc(t);
            cancel_chain(t);
            pa_rc_dec(t);
        }
        if (w->children) {
            pa_rc_dec(w->children);
            w->children = NULL;
        }
        void* owned = pending_take(w);
        void* a = (void*)w->waiter;
        w->waiter = 0;
        PyAdamasException* e = exc_new("TimeoutError", "operation timed out");
        if (a) deliver_owned(a, w, 0, e, 1);
        else w->exc = (int64_t)e;
        if (owned) pa_rc_dec(owned);
    }
    pa_rc_dec(w);
}

/* race 自身被取消：取消子任务、摘定时器，把 CancelledError 交出去。 */
void wait_for_cancel(PyAdamasAwait* w) {
    if (w->done) return;
    w->done = 1;
    if (w->children && w->children->len > 0) {
        void* t = (void*)(uintptr_t)w->children->data[0];
        pa_rc_inc(t);
        cancel_chain(t);
        pa_rc_dec(t);
    }
    if (w->children) {
        pa_rc_dec(w->children);
        w->children = NULL;
    }
    timer_remove(w);
    void* owned = pending_take(w);
    void* a = (void*)w->waiter;
    w->waiter = 0;
    if (a) deliver_owned(a, w, 0, exc_new("CancelledError", ""), 1);
    if (owned) pa_rc_dec(owned);
}

void start_awaitable(void* obj, void* waiter) {
    PyAdamasAwait* w = (PyAdamasAwait*)obj;
    if (w->kind == 6) {
        if (w->done) {
            pa_rc_inc(waiter);
            pa_rc_inc_unknown(w->result);
            PyAdamasException* e = exc_clone((PyAdamasException*)w->exc);
            deliver_owned(waiter, w, w->result, e, w->result_kind);
            pa_rc_dec(w);
            return;
        }
        if (w->waiter) {
            pa_rc_dec(w);
            coro_inject(waiter, exc_new("RuntimeError", "fd wait is already awaited"));
            return;
        }
        w->waiter = (int64_t)waiter;
        pa_rc_inc(waiter);
        pa_list_append(loop_fds, (int64_t)w, 2);  // 接管调用方的引用
        return;
    }
    if (w->kind == 5) {
        if (w->done) {
            pa_rc_inc(waiter);
            if (w->result_kind) pa_rc_inc_unknown(w->result);
            PyAdamasException* e = exc_clone((PyAdamasException*)w->exc);
            deliver_owned(waiter, w, w->result, e, w->result_kind);
            pa_rc_dec(w);
            return;
        }
        pa_rc_inc(waiter);
        pa_list_append(w->waiters, (int64_t)waiter, 2);
        if (!w->pending) {
            w->pending = 1;
            pa_list_append(loop_pending, (int64_t)w, 2);  // 接管调用方的引用
        } else {
            pa_rc_dec(w);
        }
        return;
    }
    if (w->kind == 4) {
        if (w->done) {
            pa_rc_inc(waiter);
            pa_rc_inc_unknown(w->result);
            PyAdamasException* e = exc_clone((PyAdamasException*)w->exc);
            deliver_owned(waiter, w, w->result, e, w->result_kind);
            pa_rc_dec(w);
            return;
        }
        if (w->waiter) {
            pa_rc_dec(w);
            coro_inject(waiter, exc_new("RuntimeError", "awaitable is already awaited"));
            return;
        }
        w->waiter = (int64_t)waiter;
        pa_rc_inc(waiter);
        pa_list_append(loop_pending, (int64_t)w, 2);  // 接管调用方的引用
        // 驱动不递归执行：放到就绪队列，由事件循环下一步推进，
        // 避免在别人的 resume 栈上再跑协程/生成器。
        pa_rc_inc(w);
        ready_push(w);
        return;
    }
    if (w->kind == 3) {
        if (w->done) {
            pa_rc_inc(waiter);
            pa_rc_inc_unknown(w->result);
            PyAdamasException* e = exc_clone((PyAdamasException*)w->exc);
            deliver_owned(waiter, w, w->result, e, w->result_kind);
            pa_rc_dec(w);  // 调用方交来的引用
            return;
        }
        // Task 允许被多个协程同时 await；完成时逐个交付。
        if (!w->waiters) w->waiters = pa_list_new(4, 1);
        pa_rc_inc(waiter);
        pa_list_append(w->waiters, (int64_t)waiter, 2);
        // task 由 loop_pending 持有，等它完成再清掉；这里的引用可以还掉。
        pa_rc_dec(w);
        return;
    }
    if (w->kind == 8) {
        if (w->done) {
            pa_rc_inc(waiter);
            int64_t v = w->result;
            w->result = 0;
            PyAdamasException* e = (PyAdamasException*)w->exc;
            w->exc = 0;
            deliver_owned(waiter, w, v, e, w->result_kind);
            pa_rc_dec(w);
            return;
        }
        if (w->waiter) {
            pa_rc_dec(w);
            coro_inject(waiter, exc_new("RuntimeError", "awaitable is already awaited"));
            return;
        }
        w->waiter = (int64_t)waiter;
        pa_rc_inc(waiter);
        pa_list_append(loop_pending, (int64_t)w, 2);  // 接管调用方引用
        w->pending = 1;
        pa_rc_inc(w);
        pa_list_append(loop_timers, (int64_t)w, 2);  // 定时器自己持有一份
        if (w->children && w->children->len > 0) {
            void* t = (void*)(uintptr_t)w->children->data[0];
            pa_rc_inc(t);
            start_awaitable(t, w);  // 完成时结果交给 race
        }
        return;
    }
    if (w->done) {
        pa_rc_inc(waiter);
        int64_t v = w->result;
        w->result = 0;
        PyAdamasException* e = (PyAdamasException*)w->exc;
        w->exc = 0;
        deliver_owned(waiter, w, v, e, w->result_kind);
        pa_rc_dec(w);  // 调用方交来的引用
        return;
    }
    if (w->kind == 1) {
        if (w->deadline_ms <= now_ms()) {
            w->done = 1;
            pa_rc_inc(waiter);
            deliver_owned(waiter, w, 0, NULL, 1);
            pa_rc_dec(w);
        } else {
            w->waiter = (int64_t)waiter;
            pa_rc_inc(waiter);
            pa_list_append(loop_timers, (int64_t)w, 2);  // 接管调用方的引用
        }
        return;
    }
    start_gather(w, waiter);
}

/* 把一个结果/异常交给等待者。waiter 的引用由本函数消费。
   value_kind=0 表示结果是不带引用计数的裸值（如 Queue[float] 的 double
   位模式），这条路径上所有 inc/dec 都要跳过。 */
void deliver_owned(void* waiter, void* child, int64_t value, PyAdamasException* exc,
                   int64_t value_kind) {
    if (frame_is_coro(waiter)) {
        int64_t* p = (int64_t*)waiter;
        if (p[1] == 1 || p[1] == 2) {
            // 协程已经结束（例如刚被取消），迟到的结果直接丢弃。
            if (exc) exc_free(exc);
            else if (value_kind) pa_rc_dec_unknown(value);
            pa_rc_dec(waiter);
            return;
        }
        p[9] = 0;
        if (exc) p[8] = (int64_t)exc;
        else p[2] = value;
        p[1] = 4;
        ready_push(waiter);
        return;
    }
    if (!is_await_obj(waiter)) {
        if (exc) exc_free(exc);
        else if (value_kind) pa_rc_dec_unknown(value);
        pa_rc_dec(waiter);
        return;
    }
    PyAdamasAwait* g = (PyAdamasAwait*)waiter;
    if (g->kind == 4) {
        // 异步生成器正在等内部 awaitable：把结果送回去，下一轮事件循环
        // 再驱动一步（避免递归展开）。
        if (!g->done && g->driver_gen) {
            void* gen = (void*)g->driver_gen;
            int64_t* p = (int64_t*)gen;
            p[9] = 0;
            if (exc) p[8] = (int64_t)exc;
            else p[2] = value;
            // 结果已经在帧槽里，下一轮 agen_drive 不能再拿 driver_sent 覆盖。
            g->driver_await_result = 1;
            p[1] = 0;
            ready_push(waiter);  // 把 child 的 waiter 引用转交给就绪队列
        } else {
            if (value_kind) pa_rc_dec_unknown(value);
            if (exc) exc_free(exc);
            pa_rc_dec(waiter);
        }
        return;
    }
    if (g->kind == 3) {
        // Task：保存结果供后续 await；所有等待者各拿一份独立的引用/副本。
        if (!g->done) {
            g->done = 1;
            void* owned = pending_take(g);
            if (g->task_coro) {
                pa_rc_dec((void*)g->task_coro);
                g->task_coro = 0;
            }
            g->result = value;
            g->exc = (int64_t)exc;
            if (g->waiters && g->waiters->len > 0) {
                int64_t n = g->waiters->len;
                g->waiters->len = 0;
                for (int64_t i = 0; i < n; ++i) {
                    int64_t copy = value;
                    if (copy && g->result_kind) pa_rc_inc_unknown(copy);
                    deliver_owned((void*)g->waiters->data[i], g, copy,
                                  exc_clone(exc), g->result_kind);
                }
            }
            if (owned) pa_rc_dec(owned);
        } else {
            if (value_kind) pa_rc_dec_unknown(value);
            if (exc) exc_free(exc);
        }
        pa_rc_dec(waiter);
        return;
    }
    if (g->kind == 8) {
        if (g->done) {
            if (exc) exc_free(exc);
            else if (value_kind) pa_rc_dec_unknown(value);
            pa_rc_dec(waiter);
            return;
        }
        g->done = 1;
        timer_remove(g);
        void* owned = pending_take(g);
        if (g->children) {
            pa_rc_dec(g->children);
            g->children = NULL;
        }
        void* a = (void*)g->waiter;
        g->waiter = 0;
        if (a) deliver_owned(a, g, value, exc, value_kind);
        else {
            g->result = value;
            g->exc = (int64_t)exc;
        }
        if (owned) pa_rc_dec(owned);
        pa_rc_dec(waiter);
        return;
    }
    if (g->kind != 2) {
        if (exc) exc_free(exc);
        else if (value_kind) pa_rc_dec_unknown(value);
        pa_rc_dec(waiter);
        return;
    }
    if (g->done) {
        // gather 已被取消/完成：迟到的子结果直接丢弃。
        if (exc) exc_free(exc);
        else gather_release_result(value, g->results ? g->results->elem_kind : 2);
        pa_rc_dec(waiter);
        return;
    }
    int64_t idx = -1;
    if (g->children)
        for (int64_t i = 0; i < g->children->len; ++i)
            if (g->children->data[i] == (int64_t)child) {
                idx = i;
                break;
            }
    if (exc) {
        if (!g->exc) g->exc = (int64_t)exc;
        else exc_free(exc);
    } else if (value) {
        if (g->exc || idx < 0 || !g->results) {
            gather_release_result(value, g->results ? g->results->elem_kind : 2);
        } else {
            g->results->data[idx] = value;  // results 持有这个引用
        }
    }
    if (g->remaining > 0) --g->remaining;
    int last = (g->remaining == 0);
    if (last && !g->starting && !g->done) finish_gather(g);
    pa_rc_dec(waiter);  // 消费 child 的 waiter 引用
}

void coro_inject(void* c, PyAdamasException* e) {
    int64_t* p = (int64_t*)c;
    if (p[8]) exc_free(e);
    else p[8] = (int64_t)e;
    p[1] = 4;
    pa_rc_inc(c);
    ready_push(c);
}

void coro_finish(void* c, int64_t value, PyAdamasException* exc) {
    int64_t* p = (int64_t*)c;
    p[1] = 1;
    p[9] = 0;
    void* waiter = (void*)p[7];
    p[7] = 0;
    if (waiter) {
        deliver_owned(waiter, c, value, exc, 1);
    } else if (c == loop_root) {
        loop_root_state = exc ? 2 : 1;
        loop_root_result = value;
        loop_root_exc = exc;
    } else {
        if (exc) exc_free(exc);
        else pa_rc_dec_unknown(value);
    }
}

/* 协程暂停在 await 上：把 awaitable 挂到事件循环。 */
void await_from_coro(void* c, void* w) {
    if (frame_is_coro(w)) {
        int64_t* p = (int64_t*)w;
        if (p[1] != 0) {
            pa_rc_dec(w);
            coro_inject(c, exc_new("RuntimeError", "coroutine was already awaited"));
            return;
        }
        p[7] = (int64_t)c;
        pa_rc_inc(c);
        p[1] = 4;
        ((int64_t*)c)[9] = (int64_t)w;
        ready_push(w);  // 接管 c->out 的引用
        return;
    }
    if (is_await_obj(w)) {
        ((int64_t*)c)[9] = (int64_t)w;
        start_awaitable(w, c);
        return;
    }
    coro_inject(c, exc_new("TypeError", "object is not awaitable"));
}

void coro_step(void* c) {
    int64_t* p = (int64_t*)c;
    p[1] = 0;
    p[5] = 0;
    p[9] = 0;  // 正在运行：没有等待中的 awaitable
    PyAdamasHandler h;
    h.prev = handler_top;
    h.mark = pa_rc_mark();
    h.frame = pa_frame_top;
    h.suspend_owner = NULL;
    h.heap_alloc = 0;  /* 栈记录：不能进 freelist */
    handler_top = &h;
    void* prev_active = loop_active_coro;
    loop_active_coro = c;
    int64_t exc_depth0 = pa_exc_depth();
    /* 协程体永远走 setjmp 路径（C 级兜底抓不到 _Unwind）。调用方若在零开销
       try 的保护区里（pa_eh_depth > 0），协程内抛的异常会被 exc_longjmp 误判
       成走 _Unwind，穿过这层 setjmp 展开到调用方。进出时摘掉/装回。 */
    int saved_eh_depth = pa_eh_depth;
    pa_eh_depth = 0;
    if (PA_SETJMP(h.env) != 0) {
        loop_active_coro = prev_active;
        handler_top = h.prev;
        PyAdamasException* e = pending_exc;
        pending_exc = NULL;
        // PEP 479：StopIteration 逃出协程帧 -> RuntimeError（CPython 同款）。
        pa_exc_pep479(e, "coroutine raised StopIteration");
        pa_trace_attach(e, c);
        pa_rc_cleanup(h.mark);
        p[1] = 2;
        p[5] = 0;
        p[2] = 0;
        p[3] = 0;
        p[8] = 0;
        // 异常跳过了 resume 的出口清理：帧里的堆槽当场释放。
        pa_gen_release_slots(c);
        pa_exc_unwind(exc_depth0);
        pa_eh_depth = saved_eh_depth;
        coro_finish(c, 0, e);  // 异常逃出：交给等待者或事件循环根
        return;
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(c, 0);
    loop_active_coro = prev_active;
    pa_exc_unwind(exc_depth0);
    pa_eh_depth = saved_eh_depth;
    handler_top = h.prev;
    pa_rc_cleanup(h.mark);
    p[5] = 0;
    p[2] = 0;
    int64_t status = p[1];
    if (status != 3) p[1] = 0;
    if (status == 1) {
        int64_t v = p[3];
        p[3] = 0;
        coro_finish(c, v, NULL);
    } else if (status == 2) {
        PyAdamasException* e = (PyAdamasException*)p[8];
        p[8] = 0;
        coro_finish(c, 0, e);
    } else if (status == 3) {
        void* w = (void*)p[3];
        p[3] = 0;
        p[1] = 3;  // 挂起在 await 上：取消时据此决定是否沿链传播
        await_from_coro(c, w);
    } else {
        coro_finish(c, 0, NULL);
    }
}

/* 供生成的 throw 分发调用：在 resume 帧里抛出注入的异常。 */
void pa_coro_rethrow(void* g) {
    int64_t* p = (int64_t*)g;
    PyAdamasException* e = (PyAdamasException*)p[8];
    p[8] = 0;
    if (!e) e = exc_new("RuntimeError", "coroutine resumed with no exception");
    exc_free(pending_exc);
    pending_exc = e;
    exc_longjmp();
}

/* 触发所有到期定时器；返回是否触发过。 */
int fire_due_timers(int64_t now) {
    int fired = 0;
    int64_t i = 0;
    while (i < loop_timers->len) {
        PyAdamasAwait* w = (PyAdamasAwait*)loop_timers->data[i];
        if (w->deadline_ms <= now) {
            list_take_at(loop_timers, i);
            if (w->kind == 8) {
                wait_for_timeout(w);
                fired = 1;
                continue;
            }
            if (w->kind == 9) {
                timeout_fire(w);
                fired = 1;
                continue;
            }
            w->done = 1;
            void* a = (void*)w->waiter;
            w->waiter = 0;
            if (a) deliver_owned(a, w, 0, NULL, 1);
            pa_rc_dec(w);
            fired = 1;
        } else {
            ++i;
        }
    }
    return fired;
}

int64_t earliest_deadline(void) {
    int64_t best = 0;
    for (int64_t i = 0; i < loop_timers->len; ++i) {
        PyAdamasAwait* w = (PyAdamasAwait*)loop_timers->data[i];
        if (i == 0 || w->deadline_ms < best) best = w->deadline_ms;
    }
    return best;
}

void* pa_asyncio_sleep(double secs) {
    loop_ensure();
    PyAdamasAwait* w = await_new(1);
    int64_t ms = 0;
    if (secs > 0) {
        double v = secs * 1000.0;
        ms = v > 9e15 ? (int64_t)9e15 : (int64_t)(v + 0.5);
    }
    w->deadline_ms = now_ms() + ms;
    return w;
}

void* pa_asyncio_gather(PyAdamasList* coros, int64_t result_kind) {
    loop_ensure();
    PyAdamasAwait* w = await_new(2);
    /* 后面还要建 children / results 两个列表；w 只在本帧 C 局部变量里。 */
    pa_gc_pin(w);
    int64_t n = coros ? coros->len : 0;
    w->children = pa_list_new(n, 1);
    // 结果元素所有权由调用点的静态类型决定：0 裸值 / 1 堆引用 / 2 unknown。
    if (result_kind < 0 || result_kind > 2) result_kind = 2;
    w->results = pa_list_new(n, result_kind);
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = coros->data[i];
        pa_rc_inc((void*)v);
        pa_list_append(w->children, v, 2);
        pa_list_append(w->results, 0, 2);
    }
    w->remaining = n;
    pa_gc_unpin(w);
    return w;
}

/* 把协程从它当前等待的 awaitable 上解绑（取消用）。 */
void detach_cur(void* c) {
    int64_t* p = (int64_t*)c;
    void* w = (void*)p[9];
    p[9] = 0;
    if (!w) return;
    if (frame_is_coro(w)) {
        int64_t* q = (int64_t*)w;
        if (q[7] == (int64_t)c) {
            q[7] = 0;
            pa_rc_dec(c);
        }
        return;
    }
    if (!is_await_obj(w)) return;
    PyAdamasAwait* aw = (PyAdamasAwait*)w;
    if (aw->waiter == (int64_t)c) {
        aw->waiter = 0;
        pa_rc_dec(c);
    }
    if (aw->kind == 1 && !aw->done) {
        // sleep：定时器表持有它的引用，摘掉并释放
        for (int64_t i = 0; i < loop_timers->len; ++i)
            if (loop_timers->data[i] == (int64_t)w) {
                list_take_at(loop_timers, i);
                pa_rc_dec(w);
                break;
            }
    } else if (aw->kind == 5) {
        // future：等待者在 waiters 列表里，摘掉并释放它的引用
        if (aw->waiters)
            for (int64_t i = 0; i < aw->waiters->len; ++i)
                if (aw->waiters->data[i] == (int64_t)c) {
                    list_take_at(aw->waiters, i);
                    pa_rc_dec(c);
                    break;
                }
    } else if (aw->kind == 3) {
        // task：多个等待者同样在 waiters 列表里
        if (aw->waiters)
            for (int64_t i = 0; i < aw->waiters->len; ++i)
                if (aw->waiters->data[i] == (int64_t)c) {
                    list_take_at(aw->waiters, i);
                    pa_rc_dec(c);
                    break;
                }
    } else if (aw->kind == 6) {
        // fd 等待：从轮询表里摘掉
        for (int64_t i = 0; i < loop_fds->len; ++i)
            if (loop_fds->data[i] == (int64_t)w) {
                list_take_at(loop_fds, i);
                pa_rc_dec(w);
                break;
            }
    }
    // gather/task/task-driver：让它继续跑，完成时 waiter 已经是 0
}

/* 取消一个正在挂起的协程：先沿 await 链向下传播，再向自己注入异常。 */
void cancel_suspended(void* c, PyAdamasException* e) {
    int64_t* p = (int64_t*)c;
    if (p[1] == 1 || p[1] == 2) {
        exc_free(e);
        return;
    }
    void* w = (void*)p[9];
    int heap = w && (frame_is_coro(w) || is_await_obj(w));
    int detach_owns = 0;
    if (heap) {
        pa_rc_inc(w);
        if (is_await_obj(w)) {
            int64_t kind = ((PyAdamasAwait*)w)->kind;
            detach_owns = (kind == 1 || kind == 6);  // detach_cur 会摘掉并释放它
        }
    }
    detach_cur(c);
    if (heap) {
        if (!detach_owns) cancel_chain(w);
        pa_rc_dec(w);
    }
    coro_inject(c, e);
}

int64_t pa_asyncio_run(void* coro) {
    if (!frame_is_coro(coro))
        pa_raise_c("TypeError", "asyncio.run() expects a coroutine");
    if (loop_running)
        pa_raise_c("RuntimeError", "event loop is already running");
    int64_t* p = (int64_t*)coro;
    if (p[1] != 0)
        pa_raise_c("RuntimeError", "coroutine was already awaited");
    loop_ensure();
    loop_running = 1;
    loop_root = coro;
    loop_root_state = 0;
    loop_root_result = 0;
    loop_root_exc = NULL;
    p[1] = 4;
    pa_rc_inc(coro);
    ready_push(coro);

    while (loop_root_state == 0) {
        void* c = ready_pop();
        if (c) {
            /* 刚从就绪队列摘下，只在 C 局部变量里；下面跑的是用户代码（还会
               大量分配），主收集看不见它，先 pin 住这一整步。 */
            pa_gc_pin(c);
            // 就绪队列同时放协程帧和异步生成器驱动对象。
            if (is_await_obj(c) && ((PyAdamasAwait*)c)->kind == 4)
                agen_drive((PyAdamasAwait*)c);
            else
                coro_step(c);
            pa_gc_unpin(c);
            pa_rc_dec(c);
            continue;
        }
        int64_t timeout = -1;
        if (loop_timers->len) {
            int64_t now = now_ms();
            if (fire_due_timers(now)) continue;
            int64_t best = earliest_deadline();
            timeout = best > now ? best - now : 0;
        }
        if (loop_fds->len) {
            // 有 fd 在等：poll 带上最近的定时器超时。
            poll_fds(timeout > 0x7fffffff ? 0x7fffffff : (int)timeout);
            continue;
        }
        if (timeout >= 0) {
            struct timespec ts;
            ts.tv_sec = (time_t)(timeout / 1000);
            ts.tv_nsec = (long)((timeout % 1000) * 1000000);
            nanosleep(&ts, NULL);
            continue;
        }
        loop_root_state = 2;
        loop_root_exc = exc_new("RuntimeError", "event loop stalled: root coroutine is pending");
    }

    int64_t result = loop_root_result;
    PyAdamasException* err = loop_root_exc;
    loop_root_result = 0;
    loop_root_exc = NULL;
    loop_root = NULL;
    loop_running = 0;
    int failed = (loop_root_state == 2);
    loop_root_state = 0;
    list_clear_owned(loop_ready);
    list_clear_owned(loop_timers);
    list_clear_owned(loop_pending);
    list_clear_owned(loop_fds);
    if (failed) {
        if (!err) err = exc_new("RuntimeError", "event loop failed");
        exc_free(pending_exc);
        pending_exc = err;
        exc_longjmp();
    }
    return result;
}

/* Python 的 float 打印规则：最短的、能精确往返的十进制表示，整数值补 .0 */
/* 按 CPython repr 的规则排版：先找最短的能原值读回的数字串，
   再决定用定点还是科学计数法。

   不能直接用 %g 的原因：%g 在「指数 >= 精度」时就切到科学计数法，
   而最短数字串的精度往往很小，于是 10.0 会被排成 1e+01。Python 的
   界限是「小数点位置 <= -4 或 > 16」，跟数字串长度无关。 */
void format_double(char* buf, size_t n, double v) {
    if (isnan(v)) {
        snprintf(buf, n, "nan");
        return;
    }
    if (isinf(v)) {
        snprintf(buf, n, v > 0 ? "inf" : "-inf");
        return;
    }

    /* 从最短开始找第一个能 strtod 回来的精度。反过来（17 → 1）会停在
       「第一个失败的精度之上」，吐出一串多余的末位数字。 */
    char sci[64];
    int prec = 0;
    for (prec = 1; prec <= 17; ++prec) {
        snprintf(sci, sizeof sci, "%.*e", prec - 1, v);
        if (strtod(sci, NULL) == v) break;
    }

    const char* s = sci;
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        ++s;
    }
    char digits[32];
    int nd = 0;
    while (*s && *s != 'e') {
        if (*s >= '0' && *s <= '9') digits[nd++] = *s;
        ++s;
    }
    int e10 = (*s == 'e') ? atoi(s + 1) : 0;
    while (nd > 1 && digits[nd - 1] == '0') --nd; /* 只剩 "0" 时不砍，0.0 要用 */

    int decpt = e10 + 1; /* value = 0.<digits> × 10^decpt */
    char out[64];
    int k = 0;
    if (neg) out[k++] = '-';
    if (decpt <= -4 || decpt > 16) {
        out[k++] = digits[0];
        if (nd > 1) {
            out[k++] = '.';
            for (int i = 1; i < nd; ++i) out[k++] = digits[i];
        }
        int ex = decpt - 1 < 0 ? 1 - decpt : decpt - 1;
        out[k++] = 'e';
        out[k++] = decpt - 1 < 0 ? '-' : '+';
        if (ex < 10) out[k++] = '0';
        k += snprintf(out + k, sizeof out - (size_t)k, "%d", ex);
    } else if (decpt <= 0) {
        out[k++] = '0';
        out[k++] = '.';
        for (int i = 0; i < -decpt; ++i) out[k++] = '0';
        for (int i = 0; i < nd; ++i) out[k++] = digits[i];
    } else if (decpt >= nd) {
        for (int i = 0; i < nd; ++i) out[k++] = digits[i];
        for (int i = nd; i < decpt; ++i) out[k++] = '0';
        out[k++] = '.';
        out[k++] = '0';
    } else {
        for (int i = 0; i < decpt; ++i) out[k++] = digits[i];
        out[k++] = '.';
        for (int i = decpt; i < nd; ++i) out[k++] = digits[i];
    }
    out[k] = '\0';
    snprintf(buf, n, "%s", out);
}

