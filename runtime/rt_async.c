// rt_async.c —— 协程、事件循环、sleep/gather、取消与 run
#include "rt_internal.h"

/* ── 协程与事件循环 ───────────────────────────────────────────── */

/* 运行时 awaitable：sleep / gather。magic 用来和协程帧区分。
   waiter 持有协程或上层 gather 的一个引用；完成时连同结果一起转交。 */

Py23List* loop_ready;
Py23List* loop_timers;
Py23List* loop_pending;  /* 已启动但还没完成的 gather / Task / Future / fd */
Py23List* loop_fds;
void* loop_roots[4];
int loop_roots_registered;
int loop_running;
void* loop_root;
int loop_root_state;  /* 0 pending, 1 done, 2 failed */
int64_t loop_root_result;
Py23Exception* loop_root_exc;

int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int frame_is_coro(void* p) {
    if (!p) return 0;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)PY23_GEN_HEADER * 8) return 0;
    return ((int64_t*)p)[6] == PY23_CORO_MAGIC;
}

int frame_is_agen(void* p) {
    if (!p) return 0;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)PY23_GEN_HEADER * 8) return 0;
    return ((int64_t*)p)[6] == PY23_AGEN_MAGIC;
}

int is_await_obj(void* p) {
    if (!p) return 0;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)sizeof(Py23Await)) return 0;
    return *(int64_t*)p == PY23_AWAIT_MAGIC;
}

void await_destroy(void* p) {
    Py23Await* w = (Py23Await*)p;
    py23_rc_dec_unknown(w->result);
    if (w->exc) exc_free((Py23Exception*)w->exc);
    if (w->waiter) py23_rc_dec((void*)w->waiter);
    if (w->task_coro) py23_rc_dec((void*)w->task_coro);
    if (w->driver_gen) py23_rc_dec((void*)w->driver_gen);
    if (w->waiters) py23_rc_dec(w->waiters);
    if (w->results) py23_rc_dec(w->results);
    if (w->children) py23_rc_dec(w->children);
    if (w->callbacks) py23_rc_dec(w->callbacks);
}

void await_gc_release(void* p) {
    Py23Await* w = (Py23Await*)p;
    if (w->result) py23_gc_unref_child((void*)w->result);
    if (w->waiter) py23_gc_unref_child((void*)w->waiter);
    if (w->task_coro) py23_gc_unref_child((void*)w->task_coro);
    if (w->driver_gen) py23_gc_unref_child((void*)w->driver_gen);
    if (w->waiters) py23_gc_unref_child(w->waiters);
    if (w->results) py23_gc_unref_child(w->results);
    if (w->children) py23_gc_unref_child(w->children);
    if (w->callbacks) py23_gc_unref_child(w->callbacks);
}

/* GC 清扫 / 进程退出不跑析构器，但异常对象是 malloc 的普通内存，
   必须在这里单独回收，否则挂在帧或 Task/Future 上的异常会泄漏。 */
void free_attached_exception(void* payload, int64_t size) {
    if (!payload || size < 8) return;
    int64_t* p = (int64_t*)payload;
    if (p[0] == PY23_AWAIT_MAGIC && size >= (int64_t)sizeof(Py23Await)) {
        Py23Await* w = (Py23Await*)payload;
        if (w->exc) {
            exc_free((Py23Exception*)w->exc);
            w->exc = 0;
        }
        return;
    }
    if (size >= (int64_t)PY23_GEN_HEADER * 8 &&
        (p[6] == PY23_CORO_MAGIC || p[6] == PY23_AGEN_MAGIC)) {
        if (p[8]) {
            exc_free((Py23Exception*)p[8]);
            p[8] = 0;
        }
    }
}

Py23Await* await_new(int64_t kind) {
    Py23Await* w = (Py23Await*)rc_alloc((int64_t)sizeof(Py23Await), await_destroy,
                                        await_gc_release, PY23_RC_OBJ);
    w->magic = PY23_AWAIT_MAGIC;
    w->kind = kind;
    return w;
}

void loop_ensure(void) {
    if (!loop_ready) {
        loop_ready = py23_list_new(8, 1);
        loop_timers = py23_list_new(8, 1);
        loop_pending = py23_list_new(8, 1);
        loop_fds = py23_list_new(8, 1);
    }
    if (!loop_roots_registered) {
        loop_roots[0] = (void*)&loop_ready;
        loop_roots[1] = (void*)&loop_timers;
        loop_roots[2] = (void*)&loop_pending;
        loop_roots[3] = (void*)&loop_fds;
        py23_gc_add_roots((void**)loop_roots, 4);
        loop_roots_registered = 1;
    }
}

void ready_push(void* c) { py23_list_append(loop_ready, (int64_t)c); }

void* ready_pop(void) {
    if (!loop_ready || loop_ready->len == 0) return NULL;
    void* c = (void*)loop_ready->data[0];
    memmove(&loop_ready->data[0], &loop_ready->data[1],
            sizeof(int64_t) * (size_t)(loop_ready->len - 1));
    loop_ready->len--;
    return c;
}

/* 从表里摘下第 i 项，引用交给调用方。 */
int64_t list_take_at(Py23List* l, int64_t i) {
    int64_t v = l->data[i];
    memmove(&l->data[i], &l->data[i + 1], sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
    return v;
}

void list_clear_owned(Py23List* l) {
    if (!l) return;
    int64_t n = l->len;
    l->len = 0;
    for (int64_t i = 0; i < n; ++i) py23_rc_dec((void*)l->data[i]);
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

void deliver_owned(void* waiter, void* child, int64_t value, Py23Exception* exc);
void start_awaitable(void* w, void* waiter);
void finish_gather(Py23Await* g);
void coro_inject(void* c, Py23Exception* e);
void agen_drive(Py23Await* d);
void cancel_chain(void* w);
void cancel_suspended(void* c, Py23Exception* e);
void future_cancel(Py23Await* f);
void gather_cancel(Py23Await* g);

/* 完成一个 gather：把结果/异常交给它的 waiter，并释放 pending 里的持有引用。 */
void finish_gather(Py23Await* g) {
    if (g->done) return;
    g->done = 1;
    void* owned = pending_take(g);
    void* a = (void*)g->waiter;
    g->waiter = 0;
    if (g->exc) {
        Py23Exception* e = (Py23Exception*)g->exc;
        g->exc = 0;
        if (a) deliver_owned(a, g, 0, e);
        else exc_free(e);
    } else {
        int64_t r = (int64_t)g->results;
        g->results = 0;
        if (a) deliver_owned(a, g, r, NULL);
        else g->result = r;
    }
    if (owned) py23_rc_dec(owned);
}

void gather_child_fail(Py23Await* g, Py23Exception* e) {
    if (!g->exc) g->exc = (int64_t)e;
    else exc_free(e);
    if (g->remaining > 0) --g->remaining;
}

/* 启动一个 awaitable。调用方把 w 的一个引用交给本函数。 */
void start_gather(Py23Await* w, void* waiter) {
    w->waiter = (int64_t)waiter;
    py23_rc_inc(waiter);
    py23_list_append(loop_pending, (int64_t)w);  // 接管调用方的引用
    w->starting = 1;
    for (int64_t i = 0; i < w->children->len; ++i) {
        void* child = (void*)w->children->data[i];
        if (frame_is_coro(child)) {
            int64_t* p = (int64_t*)child;
            if (p[1] != 0) {
                gather_child_fail(w, exc_new("RuntimeError", "coroutine was already awaited"));
            } else {
                p[7] = (int64_t)w;
                py23_rc_inc(w);
                p[1] = 4;
                py23_rc_inc(child);
                ready_push(child);
            }
        } else if (is_await_obj(child)) {
            py23_rc_inc(child);
            start_awaitable(child, w);
        } else {
            gather_child_fail(w, exc_new("TypeError", "gather() argument is not awaitable"));
        }
    }
    w->starting = 0;
    if (w->remaining <= 0 && !w->done) finish_gather(w);
}

void start_awaitable(void* obj, void* waiter) {
    Py23Await* w = (Py23Await*)obj;
    if (w->kind == 6) {
        if (w->done) {
            py23_rc_inc(waiter);
            py23_rc_inc_unknown(w->result);
            Py23Exception* e = exc_clone((Py23Exception*)w->exc);
            deliver_owned(waiter, w, w->result, e);
            py23_rc_dec(w);
            return;
        }
        if (w->waiter) {
            py23_rc_dec(w);
            coro_inject(waiter, exc_new("RuntimeError", "fd wait is already awaited"));
            return;
        }
        w->waiter = (int64_t)waiter;
        py23_rc_inc(waiter);
        py23_list_append(loop_fds, (int64_t)w);  // 接管调用方的引用
        return;
    }
    if (w->kind == 5) {
        if (w->done) {
            py23_rc_inc(waiter);
            py23_rc_inc_unknown(w->result);
            Py23Exception* e = exc_clone((Py23Exception*)w->exc);
            deliver_owned(waiter, w, w->result, e);
            py23_rc_dec(w);
            return;
        }
        py23_rc_inc(waiter);
        py23_list_append(w->waiters, (int64_t)waiter);
        if (!w->pending) {
            w->pending = 1;
            py23_list_append(loop_pending, (int64_t)w);  // 接管调用方的引用
        } else {
            py23_rc_dec(w);
        }
        return;
    }
    if (w->kind == 4) {
        if (w->done) {
            py23_rc_inc(waiter);
            py23_rc_inc_unknown(w->result);
            Py23Exception* e = exc_clone((Py23Exception*)w->exc);
            deliver_owned(waiter, w, w->result, e);
            py23_rc_dec(w);
            return;
        }
        if (w->waiter) {
            py23_rc_dec(w);
            coro_inject(waiter, exc_new("RuntimeError", "awaitable is already awaited"));
            return;
        }
        w->waiter = (int64_t)waiter;
        py23_rc_inc(waiter);
        py23_list_append(loop_pending, (int64_t)w);  // 接管调用方的引用
        // 驱动不递归执行：放到就绪队列，由事件循环下一步推进，
        // 避免在别人的 resume 栈上再跑协程/生成器。
        py23_rc_inc(w);
        ready_push(w);
        return;
    }
    if (w->kind == 3) {
        if (w->done) {
            py23_rc_inc(waiter);
            py23_rc_inc_unknown(w->result);
            Py23Exception* e = exc_clone((Py23Exception*)w->exc);
            deliver_owned(waiter, w, w->result, e);
            py23_rc_dec(w);  // 调用方交来的引用
            return;
        }
        if (w->waiter) {
            py23_rc_dec(w);
            coro_inject(waiter, exc_new("RuntimeError", "task is already awaited"));
            return;
        }
        // task 由 loop_pending 持有，等它完成再清掉；这里的引用可以还掉。
        w->waiter = (int64_t)waiter;
        py23_rc_inc(waiter);
        py23_rc_dec(w);
        return;
    }
    if (w->done) {
        py23_rc_inc(waiter);
        int64_t v = w->result;
        w->result = 0;
        Py23Exception* e = (Py23Exception*)w->exc;
        w->exc = 0;
        deliver_owned(waiter, w, v, e);
        py23_rc_dec(w);  // 调用方交来的引用
        return;
    }
    if (w->kind == 1) {
        if (w->deadline_ms <= now_ms()) {
            w->done = 1;
            py23_rc_inc(waiter);
            deliver_owned(waiter, w, 0, NULL);
            py23_rc_dec(w);
        } else {
            w->waiter = (int64_t)waiter;
            py23_rc_inc(waiter);
            py23_list_append(loop_timers, (int64_t)w);  // 接管调用方的引用
        }
        return;
    }
    start_gather(w, waiter);
}

/* 把一个结果/异常交给等待者。waiter 的引用由本函数消费。 */
void deliver_owned(void* waiter, void* child, int64_t value, Py23Exception* exc) {
    if (frame_is_coro(waiter)) {
        int64_t* p = (int64_t*)waiter;
        if (p[1] == 1 || p[1] == 2) {
            // 协程已经结束（例如刚被取消），迟到的结果直接丢弃。
            if (exc) exc_free(exc);
            else py23_rc_dec_unknown(value);
            py23_rc_dec(waiter);
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
        else py23_rc_dec_unknown(value);
        py23_rc_dec(waiter);
        return;
    }
    Py23Await* g = (Py23Await*)waiter;
    if (g->kind == 4) {
        // 异步生成器正在等内部 awaitable：把结果送回去，下一轮事件循环
        // 再驱动一步（避免递归展开）。
        if (!g->done && g->driver_gen) {
            void* gen = (void*)g->driver_gen;
            int64_t* p = (int64_t*)gen;
            p[9] = 0;
            if (exc) p[8] = (int64_t)exc;
            else p[2] = value;
            p[1] = 0;
            ready_push(waiter);  // 把 child 的 waiter 引用转交给就绪队列
        } else {
            py23_rc_dec_unknown(value);
            if (exc) exc_free(exc);
            py23_rc_dec(waiter);
        }
        return;
    }
    if (g->kind == 3) {
        // Task：保存结果供后续 await，当前等待者拿到一份独立的引用/副本。
        if (!g->done) {
            g->done = 1;
            void* owned = pending_take(g);
            if (g->task_coro) {
                py23_rc_dec((void*)g->task_coro);
                g->task_coro = 0;
            }
            g->result = value;
            g->exc = (int64_t)exc;
            void* a = (void*)g->waiter;
            g->waiter = 0;
            if (a) {
                py23_rc_inc_unknown(value);
                deliver_owned(a, g, value, exc_clone(exc));
            }
            if (owned) py23_rc_dec(owned);
        } else {
            py23_rc_dec_unknown(value);
            if (exc) exc_free(exc);
        }
        py23_rc_dec(waiter);
        return;
    }
    if (g->kind != 2) {
        if (exc) exc_free(exc);
        else py23_rc_dec_unknown(value);
        py23_rc_dec(waiter);
        return;
    }
    if (g->done) {
        // gather 已被取消/完成：迟到的子结果直接丢弃。
        if (exc) exc_free(exc);
        else py23_rc_dec_unknown(value);
        py23_rc_dec(waiter);
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
        if (g->exc || idx < 0 || !g->results) py23_rc_dec_unknown(value);
        else g->results->data[idx] = value;  // results 持有这个引用
    }
    if (g->remaining > 0) --g->remaining;
    int last = (g->remaining == 0);
    if (last && !g->starting && !g->done) finish_gather(g);
    py23_rc_dec(waiter);  // 消费 child 的 waiter 引用
}

void coro_inject(void* c, Py23Exception* e) {
    int64_t* p = (int64_t*)c;
    if (p[8]) exc_free(e);
    else p[8] = (int64_t)e;
    p[1] = 4;
    py23_rc_inc(c);
    ready_push(c);
}

void coro_finish(void* c, int64_t value, Py23Exception* exc) {
    int64_t* p = (int64_t*)c;
    p[1] = 1;
    p[9] = 0;
    void* waiter = (void*)p[7];
    p[7] = 0;
    if (waiter) {
        deliver_owned(waiter, c, value, exc);
    } else if (c == loop_root) {
        loop_root_state = exc ? 2 : 1;
        loop_root_result = value;
        loop_root_exc = exc;
    } else {
        if (exc) exc_free(exc);
        else py23_rc_dec_unknown(value);
    }
}

/* 协程暂停在 await 上：把 awaitable 挂到事件循环。 */
void await_from_coro(void* c, void* w) {
    if (frame_is_coro(w)) {
        int64_t* p = (int64_t*)w;
        if (p[1] != 0) {
            py23_rc_dec(w);
            coro_inject(c, exc_new("RuntimeError", "coroutine was already awaited"));
            return;
        }
        p[7] = (int64_t)c;
        py23_rc_inc(c);
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
        p[8] = 0;
        coro_finish(c, 0, e);  // 异常逃出：交给等待者或事件循环根
        return;
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(c, 0);
    handler_top = h.prev;
    py23_rc_cleanup(h.mark);
    p[5] = 0;
    p[2] = 0;
    int64_t status = p[1];
    if (status != 3) p[1] = 0;
    if (status == 1) {
        int64_t v = p[3];
        p[3] = 0;
        coro_finish(c, v, NULL);
    } else if (status == 2) {
        Py23Exception* e = (Py23Exception*)p[8];
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
void py23_coro_rethrow(void* g) {
    int64_t* p = (int64_t*)g;
    Py23Exception* e = (Py23Exception*)p[8];
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
        Py23Await* w = (Py23Await*)loop_timers->data[i];
        if (w->deadline_ms <= now) {
            list_take_at(loop_timers, i);
            w->done = 1;
            void* a = (void*)w->waiter;
            w->waiter = 0;
            if (a) deliver_owned(a, w, 0, NULL);
            py23_rc_dec(w);
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
        Py23Await* w = (Py23Await*)loop_timers->data[i];
        if (i == 0 || w->deadline_ms < best) best = w->deadline_ms;
    }
    return best;
}

void* py23_asyncio_sleep(double secs) {
    loop_ensure();
    Py23Await* w = await_new(1);
    int64_t ms = 0;
    if (secs > 0) {
        double v = secs * 1000.0;
        ms = v > 9e15 ? (int64_t)9e15 : (int64_t)(v + 0.5);
    }
    w->deadline_ms = now_ms() + ms;
    return w;
}

void* py23_asyncio_gather(Py23List* coros) {
    loop_ensure();
    Py23Await* w = await_new(2);
    int64_t n = coros ? coros->len : 0;
    w->children = py23_list_new(n, 1);
    // 结果元素类型静态不可知：按 unknown 容器处理，只存不释放
    //（kind 2），和 list[unknown] 的约定一致。
    w->results = py23_list_new(n, 2);
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = coros->data[i];
        py23_rc_inc((void*)v);
        py23_list_append(w->children, v);
        py23_list_append(w->results, 0);
    }
    w->remaining = n;
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
            py23_rc_dec(c);
        }
        return;
    }
    if (!is_await_obj(w)) return;
    Py23Await* aw = (Py23Await*)w;
    if (aw->waiter == (int64_t)c) {
        aw->waiter = 0;
        py23_rc_dec(c);
    }
    if (aw->kind == 1 && !aw->done) {
        // sleep：定时器表持有它的引用，摘掉并释放
        for (int64_t i = 0; i < loop_timers->len; ++i)
            if (loop_timers->data[i] == (int64_t)w) {
                list_take_at(loop_timers, i);
                py23_rc_dec(w);
                break;
            }
    } else if (aw->kind == 5) {
        // future：等待者在 waiters 列表里，摘掉并释放它的引用
        if (aw->waiters)
            for (int64_t i = 0; i < aw->waiters->len; ++i)
                if (aw->waiters->data[i] == (int64_t)c) {
                    list_take_at(aw->waiters, i);
                    py23_rc_dec(c);
                    break;
                }
    } else if (aw->kind == 6) {
        // fd 等待：从轮询表里摘掉
        for (int64_t i = 0; i < loop_fds->len; ++i)
            if (loop_fds->data[i] == (int64_t)w) {
                list_take_at(loop_fds, i);
                py23_rc_dec(w);
                break;
            }
    }
    // gather/task/task-driver：让它继续跑，完成时 waiter 已经是 0
}

/* 取消一个正在挂起的协程：先沿 await 链向下传播，再向自己注入异常。 */
void cancel_suspended(void* c, Py23Exception* e) {
    int64_t* p = (int64_t*)c;
    if (p[1] == 1 || p[1] == 2) {
        exc_free(e);
        return;
    }
    void* w = (void*)p[9];
    int heap = w && (frame_is_coro(w) || is_await_obj(w));
    int detach_owns = 0;
    if (heap) {
        py23_rc_inc(w);
        if (is_await_obj(w)) {
            int64_t kind = ((Py23Await*)w)->kind;
            detach_owns = (kind == 1 || kind == 6);  // detach_cur 会摘掉并释放它
        }
    }
    detach_cur(c);
    if (heap) {
        if (!detach_owns) cancel_chain(w);
        py23_rc_dec(w);
    }
    coro_inject(c, e);
}

int64_t py23_asyncio_run(void* coro) {
    if (!frame_is_coro(coro))
        py23_raise_c("TypeError", "asyncio.run() expects a coroutine");
    if (loop_running)
        py23_raise_c("RuntimeError", "event loop is already running");
    int64_t* p = (int64_t*)coro;
    if (p[1] != 0)
        py23_raise_c("RuntimeError", "coroutine was already awaited");
    loop_ensure();
    loop_running = 1;
    loop_root = coro;
    loop_root_state = 0;
    loop_root_result = 0;
    loop_root_exc = NULL;
    p[1] = 4;
    py23_rc_inc(coro);
    ready_push(coro);

    while (loop_root_state == 0) {
        void* c = ready_pop();
        if (c) {
            // 就绪队列同时放协程帧和异步生成器驱动对象。
            if (is_await_obj(c) && ((Py23Await*)c)->kind == 4)
                agen_drive((Py23Await*)c);
            else
                coro_step(c);
            py23_rc_dec(c);
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
    Py23Exception* err = loop_root_exc;
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

