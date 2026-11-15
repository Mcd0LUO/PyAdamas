// rt_task.c —— Task 与 Future
#include "rt_internal.h"

/* pyadamas 函数值 = 闭包对象：[0] 代码地址，%r10 传闭包指针（环境）。
   C 没有隐藏寄存器参数，运行时回调用一个跳板。 */
/* 跳板本身也用 C 调用约定（它由运行时 C 代码 call），只有被跳进去的
   PyAdamas 函数体用 ABI 的实参寄存器。所以这里的两套寄存器搬运必须按
   目标区分：SysV 的 fn/arg 在 rdi/rsi，Win64 在 rcx/rdx，而函数体入口
   一律是 ABI 的第 0 个实参寄存器（SysV rdi / Win64 rcx）。 */
#if defined(_WIN32)
__asm__(
    ".text\n"
    ".globl pa_call1\n"
    PA_TYPE_FUNC(pa_call1)
    "pa_call1:\n"
    "\tmovq %rcx, %r10\n"
    "\tmovq (%rcx), %r11\n"
    "\tmovq %rdx, %rcx\n"
    "\tjmp *%r11\n"
    PA_SIZE_FUNC(pa_call1));
#else
__asm__(
    ".text\n"
    ".globl pa_call1\n"
    PA_TYPE_FUNC(pa_call1)
    "pa_call1:\n"
    "\tmovq %rdi, %r10\n"
    "\tmovq (%rdi), %r11\n"
    "\tmovq %rsi, %rdi\n"
    "\txorl %eax, %eax\n"
    "\tjmp *%r11\n"
    PA_SIZE_FUNC(pa_call1));
#endif

/* 完成时同步调用 add_done_callback 注册的函数，参数是 future 本身。
   CPython 用 call_soon 排队，这里直接调用；回调抛出的异常会向外传播。 */
static void future_run_callbacks(PyAdamasAwait* f) {
    PyAdamasList* cbs = f->callbacks;
    if (!cbs || cbs->len == 0) return;
    // 注册顺序调用。先把长度清零，回调里再注册的（future 已完成）
    // 会走立即调用分支，不会追加到本轮。
    int64_t n = cbs->len;
    cbs->len = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t cb = cbs->data[i];
        pa_call1((void*)(uintptr_t)cb, (int64_t)f);
        pa_rc_dec_unknown(cb);
    }
}

void* pa_asyncio_create_task(void* coro, int64_t result_kind) {
    if (result_kind < 0 || result_kind > 2) result_kind = 2;
    if (!frame_is_coro(coro)) {
        // 也接受 Future/sleep/fd/gather 等可等待对象：Task 直接等它完成。
        if (!is_await_obj(coro))
            pa_raise_c("TypeError", "asyncio.create_task() expects an awaitable");
        loop_ensure();
        PyAdamasAwait* t = await_new(3);
        t->result_kind = result_kind;
        t->driver_gen = (int64_t)coro;
        pa_rc_inc(coro);
        pa_rc_inc(coro);  // start_awaitable 接管这个引用
        start_awaitable(coro, t);
        return t;
    }
    loop_ensure();
    int64_t* p = (int64_t*)coro;
    if (p[1] != 0)
        pa_raise_c("RuntimeError", "coroutine was already awaited");
    PyAdamasAwait* t = await_new(3);
    t->result_kind = result_kind;
    t->task_coro = (int64_t)coro;
    pa_rc_inc(coro);
    pa_list_append(loop_pending, (int64_t)t);  // 持有 task 的引用
    p[7] = (int64_t)t;
    pa_rc_inc(t);
    p[1] = 4;
    pa_rc_inc(coro);
    ready_push(coro);
    // pending 列表持有一个引用；返回值给调用方另一个引用。
    pa_rc_inc(t);
    return t;
}

int64_t pa_asyncio_task_done(void* t) {
    return is_await_obj(t) && ((PyAdamasAwait*)t)->kind == 3 && ((PyAdamasAwait*)t)->done;
}

int64_t pa_asyncio_task_cancel(void* t) {
    if (!is_await_obj(t) || ((PyAdamasAwait*)t)->kind != 3)
        pa_raise_c("TypeError", "cancel() expects a task");
    PyAdamasAwait* w = (PyAdamasAwait*)t;
    if (w->done) return 0;
    if (!w->task_coro && w->driver_gen) {
        // 等待对象型 Task：完成自己并沿等待链传播取消。
        void* sub = (void*)w->driver_gen;
        pa_rc_inc(sub);
        w->done = 1;
        w->cancelled = 1;
        PyAdamasException* e = exc_new("CancelledError", "");
        if (w->waiters && w->waiters->len > 0) {
            int64_t n = w->waiters->len;
            w->waiters->len = 0;
            for (int64_t i = 0; i < n; ++i)
                deliver_owned((void*)w->waiters->data[i], w, 0, exc_clone(e), 1);
        }
        w->exc = (int64_t)e;
        cancel_chain(sub);
        pa_rc_dec(sub);
        return 1;
    }
    void* c = (void*)w->task_coro;
    if (!c) return 0;
    w->cancelled = 1;
    int64_t* p = (int64_t*)c;
    PyAdamasException* e = exc_new("CancelledError", "");
    if (p[1] == 4) {
        // 已经排好队，等着恢复：把异常塞进去，别重复入队。
        if (p[8]) exc_free(e);
        else p[8] = (int64_t)e;
        return 1;
    }
    if (p[1] == 3) {
        cancel_suspended(c, e);
        return 1;
    }
    if (p[1] == 0) {
        coro_inject(c, e);
        return 1;
    }
    exc_free(e);
    return 0;
}

void gather_cancel(PyAdamasAwait* g) {
    if (g->done) return;
    if (g->children)
        for (int64_t i = 0; i < g->children->len; ++i)
            cancel_chain((void*)g->children->data[i]);
    g->done = 1;
    void* owned = pending_take(g);
    void* a = (void*)g->waiter;
    g->waiter = 0;
    PyAdamasException* e = exc_new("CancelledError", "");
    if (a) deliver_owned(a, g, 0, e, 1);
    else g->exc = (int64_t)e;
    if (owned) pa_rc_dec(owned);
}

void future_cancel(PyAdamasAwait* f) {
    if (f->done) return;
    f->done = 1;
    f->cancelled = 1;
    PyAdamasException* stored = exc_new("CancelledError", "");
    if (f->waiters) {
        int64_t n = f->waiters->len;
        f->waiters->len = 0;
        for (int64_t i = 0; i < n; ++i)
            deliver_owned((void*)f->waiters->data[i], f, 0, exc_clone(stored), 1);
    }
    f->exc = (int64_t)stored;
    void* owned = pending_take(f);
    if (owned) pa_rc_dec(owned);
    future_run_callbacks(f);
}

/* sleep/fd 这类没有内部链的等待对象：摘表、标记完成、通知等待者。 */
void simple_wait_cancel(PyAdamasAwait* w) {
    if (w->done) return;
    w->done = 1;
    PyAdamasList* lst = w->kind == 1 ? loop_timers : (w->kind == 6 ? loop_fds : NULL);
    int64_t list_owned = 0;
    if (lst)
        for (int64_t i = 0; i < lst->len; ++i)
            if (lst->data[i] == (int64_t)w) {
                list_owned = list_take_at(lst, i);
                break;
            }
    void* a = (void*)w->waiter;
    w->waiter = 0;
    PyAdamasException* e = exc_new("CancelledError", "");
    if (a) deliver_owned(a, w, 0, e, 1);
    else w->exc = (int64_t)e;
    if (list_owned) pa_rc_dec((void*)list_owned);
}

/* 沿 await 链把取消传下去。w 的引用由调用方保证存活。 */
void cancel_chain(void* w) {
    if (!w) return;
    if (frame_is_coro(w)) {
        cancel_suspended(w, exc_new("CancelledError", ""));
        return;
    }
    if (!is_await_obj(w)) return;
    PyAdamasAwait* aw = (PyAdamasAwait*)w;
    switch (aw->kind) {
        case 1:
        case 6: simple_wait_cancel(aw); break;
        case 2: gather_cancel(aw); break;
        case 3: pa_asyncio_task_cancel(w); break;
        case 5: future_cancel(aw); break;
        case 8: wait_for_cancel(aw); break;
        default: break;  // driver 让它自然结束
    }
}

void future_settle(PyAdamasAwait* f, int64_t value, PyAdamasException* exc) {
    if (f->done) {
        if (exc) exc_free(exc);
        else if (f->result_kind) pa_rc_dec_unknown(value);
        return;
    }
    f->done = 1;
    f->result = value;
    f->exc = (int64_t)exc;
    if (f->waiters) {
        int64_t n = f->waiters->len;
        f->waiters->len = 0;
        for (int64_t i = 0; i < n; ++i) {
            int64_t copy = value;
            if (copy && f->result_kind) pa_rc_inc_unknown(copy);
            deliver_owned((void*)f->waiters->data[i], f, copy, exc_clone(exc),
                          f->result_kind);
        }
    }
    void* owned = pending_take(f);
    if (owned) pa_rc_dec(owned);
    future_run_callbacks(f);
}

void* pa_asyncio_future_new(void) {
    loop_ensure();
    PyAdamasAwait* f = await_new(5);
    f->waiters = pa_list_new(4, 1);
    f->callbacks = pa_list_new(2, 1);
    return f;
}

void pa_future_set_result(void* fp, int64_t value) {
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        pa_raise_c("TypeError", "set_result() expects a Future");
    if (f->done) pa_raise_c("RuntimeError", "future is already done");
    future_settle(f, value, NULL);
}

/* pyadamas 的异常不是一等值：set_exception 接收消息字符串，失败类型统一
   记为 Exception，与 exception() 返回消息的约定一致。 */
void pa_future_set_exception(void* fp, PyAdamasStr* msg) {
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        pa_raise_c("TypeError", "set_exception() expects a Future");
    if (f->done) pa_raise_c("RuntimeError", "future is already done");
    PyAdamasException* e = exc_new("Exception", msg ? msg->data : "");
    future_settle(f, 0, e);
}

void pa_future_set_exception_value(void* fp, PyAdamasExcValue* exc) {
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        pa_raise_c("TypeError", "set_exception() expects a Future");
    if (f->done) pa_raise_c("RuntimeError", "future is already done");
    future_settle(f, 0, pa_exc_from_value(exc));
}

void pa_future_add_callback(void* fp, int64_t cb) {
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        pa_raise_c("TypeError", "add_done_callback() expects a Future");
    if (f->done) {
        // 调用方把 cb 的引用交给了这里；同步调用完就还掉。
        pa_call1((void*)(uintptr_t)cb, (int64_t)f);
        pa_rc_dec_unknown(cb);
        return;
    }
    if (!f->callbacks) f->callbacks = pa_list_new(2, 1);
    pa_list_append(f->callbacks, cb);  // 接管 cb 的引用
}

int64_t pa_future_done(void* fp) {
    return is_await_obj(fp) && ((PyAdamasAwait*)fp)->kind == 5 && ((PyAdamasAwait*)fp)->done;
}

int64_t pa_future_cancelled(void* fp) {
    if (!is_await_obj(fp) || ((PyAdamasAwait*)fp)->kind != 5) return 0;
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    return f->done && f->cancelled;
}

int64_t pa_future_cancel(void* fp) {
    if (!is_await_obj(fp) || ((PyAdamasAwait*)fp)->kind != 5)
        pa_raise_c("TypeError", "cancel() expects a Future");
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (f->done) return 0;
    future_cancel(f);
    return 1;
}

void raise_exception_copy(PyAdamasException* e, const char* fallback) {
    PyAdamasException* copy = e ? exc_clone(e) : exc_new("RuntimeError", fallback);
    exc_free(pending_exc);
    pending_exc = copy;
    exc_longjmp();
}

int64_t pa_future_result(void* fp) {
    if (!is_await_obj(fp) || ((PyAdamasAwait*)fp)->kind != 5)
        pa_raise_c("TypeError", "result() expects a Future");
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (!f->done) pa_raise_c("RuntimeError", "future is not done");
    if (f->exc) raise_exception_copy((PyAdamasException*)f->exc, "future failed");
    int64_t v = f->result;
    if (v && f->result_kind) pa_rc_inc_unknown(v);
    return v;
}

PyAdamasExcValue* pa_future_exception(void* fp) {
    if (!is_await_obj(fp) || ((PyAdamasAwait*)fp)->kind != 5)
        pa_raise_c("TypeError", "exception() expects a Future");
    PyAdamasAwait* f = (PyAdamasAwait*)fp;
    if (!f->done) pa_raise_c("RuntimeError", "future is not done");
    if (!f->exc) return NULL;
    return pa_exc_value_from_exc((PyAdamasException*)f->exc);
}

int64_t pa_asyncio_task_result(void* tp) {
    if (!is_await_obj(tp) || ((PyAdamasAwait*)tp)->kind != 3)
        pa_raise_c("TypeError", "result() expects a Task");
    PyAdamasAwait* t = (PyAdamasAwait*)tp;
    if (!t->done) pa_raise_c("RuntimeError", "task is not done");
    if (t->exc) raise_exception_copy((PyAdamasException*)t->exc, "task failed");
    int64_t v = t->result;
    if (v) pa_rc_inc_unknown(v);
    return v;
}

PyAdamasExcValue* pa_asyncio_task_exception(void* tp) {
    if (!is_await_obj(tp) || ((PyAdamasAwait*)tp)->kind != 3)
        pa_raise_c("TypeError", "exception() expects a Task");
    PyAdamasAwait* t = (PyAdamasAwait*)tp;
    if (!t->done) pa_raise_c("RuntimeError", "task is not done");
    if (!t->exc) return NULL;
    return pa_exc_value_from_exc((PyAdamasException*)t->exc);
}

int64_t pa_asyncio_task_cancelled(void* tp) {
    if (!is_await_obj(tp) || ((PyAdamasAwait*)tp)->kind != 3) return 0;
    PyAdamasAwait* t = (PyAdamasAwait*)tp;
    if (!t->done || !t->exc) return 0;
    return strcmp(((PyAdamasException*)t->exc)->type, "CancelledError") == 0;
}

/* ── asyncio 同步原语（Event / Lock / Queue） ─────────────────── */

typedef struct {
    int64_t set;
    PyAdamasList* waiters; /* Future* */
} PyAdamasEvent;

typedef struct {
    int64_t locked;
    PyAdamasList* waiters; /* Future* */
} PyAdamasLock;

typedef struct {
    PyAdamasList* items;    /* 元素容器：kind 0 裸值 / 2 引用 */
    PyAdamasList* getters;  /* Future* */
    PyAdamasList* putters;  /* 满队列时等待的 put Future* */
    PyAdamasList* pvalues;  /* 与 putters 一一对应的值（与 items 同类别） */
    PyAdamasList* joiners;  /* join() 的 Future* */
    int64_t maxsize;
    int64_t unfinished;
    int64_t kind;           /* 元素类别：0 裸值（float 位模式）/ 1 引用 */
} PyAdamasQueue;

static void event_destroy(void* p) {
    PyAdamasEvent* e = (PyAdamasEvent*)p;
    if (e->waiters) pa_rc_dec(e->waiters);
}
static void lock_destroy(void* p) {
    PyAdamasLock* l = (PyAdamasLock*)p;
    if (l->waiters) pa_rc_dec(l->waiters);
}
static void queue_destroy(void* p) {
    PyAdamasQueue* q = (PyAdamasQueue*)p;
    if (q->items) pa_rc_dec(q->items);
    if (q->getters) pa_rc_dec(q->getters);
    if (q->putters) pa_rc_dec(q->putters);
    if (q->pvalues) pa_rc_dec(q->pvalues);
    if (q->joiners) pa_rc_dec(q->joiners);
}

void* pa_event_new(void) {
    loop_ensure();
    PyAdamasEvent* e = (PyAdamasEvent*)rc_alloc((int64_t)sizeof(PyAdamasEvent), event_destroy, NULL,
                                        PYADAMAS_RC_OBJ);
    e->waiters = pa_list_new(4, 1);
    return e;
}

void pa_event_set(void* ep) {
    if (!ep) pa_raise_c("TypeError", "set() expects an Event");
    PyAdamasEvent* e = (PyAdamasEvent*)ep;
    e->set = 1;
    int64_t n = e->waiters->len;
    for (int64_t i = 0; i < n; ++i)
        pa_future_set_result((void*)(uintptr_t)e->waiters->data[i], 1);
    pa_list_set_len(e->waiters, 0);
}

int64_t pa_event_is_set(void* ep) {
    if (!ep) return 0;
    return ((PyAdamasEvent*)ep)->set;
}

void* pa_event_wait(void* ep) {
    if (!ep) pa_raise_c("TypeError", "wait() expects an Event");
    PyAdamasEvent* e = (PyAdamasEvent*)ep;
    void* f = pa_asyncio_future_new();
    if (e->set) {
        pa_future_set_result(f, 1);
        return f;
    }
    pa_rc_inc(f);
    pa_list_append(e->waiters, (int64_t)f);
    return f;
}

void* pa_lock_new(void) {
    loop_ensure();
    PyAdamasLock* l = (PyAdamasLock*)rc_alloc((int64_t)sizeof(PyAdamasLock), lock_destroy, NULL,
                                      PYADAMAS_RC_OBJ);
    l->waiters = pa_list_new(4, 1);
    return l;
}

void* pa_lock_acquire(void* lp) {
    if (!lp) pa_raise_c("TypeError", "acquire() expects a Lock");
    PyAdamasLock* l = (PyAdamasLock*)lp;
    void* f = pa_asyncio_future_new();
    if (!l->locked) {
        l->locked = 1;
        pa_future_set_result(f, 1);
        return f;
    }
    pa_rc_inc(f);
    pa_list_append(l->waiters, (int64_t)f);
    return f;
}

void pa_lock_release(void* lp) {
    if (!lp) pa_raise_c("TypeError", "release() expects a Lock");
    PyAdamasLock* l = (PyAdamasLock*)lp;
    if (!l->locked) pa_raise_c("RuntimeError", "release unlocked lock");
    if (l->waiters->len > 0) {
        void* f = (void*)(uintptr_t)l->waiters->data[0];
        pa_future_set_result(f, 1);
        pa_list_del_index(l->waiters, 0);
        return;  /* 所有权直接转给下一个等待者，保持 locked */
    }
    l->locked = 0;
}

int64_t pa_lock_locked(void* lp) {
    if (!lp) return 0;
    return ((PyAdamasLock*)lp)->locked;
}

void* pa_queue_new(int64_t maxsize, int64_t kind) {
    loop_ensure();
    PyAdamasQueue* q = (PyAdamasQueue*)rc_alloc((int64_t)sizeof(PyAdamasQueue), queue_destroy, NULL,
                                        PYADAMAS_RC_OBJ);
    q->kind = kind ? 1 : 0;
    int64_t lk = q->kind ? 2 : 0;
    q->items = pa_list_new(4, lk);
    q->getters = pa_list_new(4, 1);
    q->putters = pa_list_new(4, 1);
    q->pvalues = pa_list_new(4, lk);
    q->joiners = pa_list_new(4, 1);
    q->maxsize = maxsize > 0 ? maxsize : 0;
    return q;
}

static void* queue_ready_future(void) {
    void* f = pa_asyncio_future_new();
    pa_future_set_result(f, 0);
    return f;
}

static void queue_push_item(PyAdamasQueue* q, int64_t v) {
    ++q->unfinished;
    if (q->kind) pa_rc_inc_unknown(v);
    pa_list_append(q->items, v);
}

void* pa_queue_put(void* qp, int64_t v) {
    if (!qp) pa_raise_c("TypeError", "put() expects a Queue");
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    if (q->getters->len > 0) {
        void* f = (void*)(uintptr_t)q->getters->data[0];
        pa_future_set_result(f, v);
        pa_list_del_index(q->getters, 0);
        return queue_ready_future();
    }
    if (q->maxsize > 0 && q->items->len >= q->maxsize) {
        void* f = pa_asyncio_future_new();
        pa_rc_inc(f);
        pa_list_append(q->putters, (int64_t)f);
        if (q->kind) pa_rc_inc_unknown(v);
        pa_list_append(q->pvalues, v);
        return f;
    }
    queue_push_item(q, v);
    return queue_ready_future();
}

void* pa_queue_get(void* qp) {
    if (!qp) pa_raise_c("TypeError", "get() expects a Queue");
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    if (q->items->len > 0) {
        int64_t v = list_take_at(q->items, 0);
        // 摘除时保留引用：值的所有权转交给 Future / 调用方。
        if (q->putters->len > 0) {
            // 队列腾出位置：把一个等待中的 put 值搬进来。
            void* pf = (void*)(uintptr_t)q->putters->data[0];
            int64_t pv = list_take_at(q->pvalues, 0);
            pa_list_del_index(q->putters, 0);
            pa_future_set_result(pf, 0);
            queue_push_item(q, pv);  // pvalues 的引用直接转交给 items
        }
        void* f = pa_asyncio_future_new();
        ((PyAdamasAwait*)f)->result_kind = q->kind;
        pa_future_set_result(f, v);
        return f;
    }
    void* f = pa_asyncio_future_new();
    ((PyAdamasAwait*)f)->result_kind = q->kind;
    pa_rc_inc(f);
    pa_list_append(q->getters, (int64_t)f);
    return f;
}

int64_t pa_queue_qsize(void* qp) {
    if (!qp) return 0;
    return ((PyAdamasQueue*)qp)->items->len;
}

int64_t pa_queue_empty(void* qp) {
    if (!qp) return 1;
    return ((PyAdamasQueue*)qp)->items->len == 0 ? 1 : 0;
}

int64_t pa_queue_full(void* qp) {
    if (!qp) return 0;
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    return q->maxsize > 0 && q->items->len >= q->maxsize ? 1 : 0;
}

void* pa_queue_join(void* qp) {
    if (!qp) pa_raise_c("TypeError", "join() expects a Queue");
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    if (q->unfinished == 0) return queue_ready_future();
    void* f = pa_asyncio_future_new();
    pa_rc_inc(f);
    pa_list_append(q->joiners, (int64_t)f);
    return f;
}

void pa_queue_task_done(void* qp) {
    if (!qp) pa_raise_c("TypeError", "task_done() expects a Queue");
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    if (q->unfinished <= 0)
        pa_raise_c("ValueError", "task_done() called too many times");
    --q->unfinished;
    if (q->unfinished == 0 && q->joiners->len > 0) {
        int64_t n = q->joiners->len;
        for (int64_t i = 0; i < n; ++i)
            pa_future_set_result((void*)(uintptr_t)q->joiners->data[i], 0);
        pa_list_set_len(q->joiners, 0);
    }
}

void pa_queue_put_nowait(void* qp, int64_t v) {
    if (!qp) pa_raise_c("TypeError", "put_nowait() expects a Queue");
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    if (q->maxsize > 0 && q->items->len >= q->maxsize)
        pa_raise_c("QueueFull", "queue is full");
    if (q->getters->len > 0) {
        void* f = (void*)(uintptr_t)q->getters->data[0];
        pa_future_set_result(f, v);
        pa_list_del_index(q->getters, 0);
        return;
    }
    queue_push_item(q, v);
}

int64_t pa_queue_get_nowait(void* qp) {
    if (!qp) pa_raise_c("TypeError", "get_nowait() expects a Queue");
    PyAdamasQueue* q = (PyAdamasQueue*)qp;
    if (q->items->len == 0) pa_raise_c("QueueEmpty", "queue is empty");
    int64_t v = list_take_at(q->items, 0);
    if (q->putters->len > 0) {
        void* pf = (void*)(uintptr_t)q->putters->data[0];
        int64_t pv = list_take_at(q->pvalues, 0);
        pa_list_del_index(q->putters, 0);
        pa_future_set_result(pf, 0);
        queue_push_item(q, pv);
    }
    return v;
}

/* ── asyncio.TaskGroup ───────────────────────────────────────── */

typedef struct {
    PyAdamasList* tasks; /* Task*（owned） */
} PyAdamasTaskGroup;

static void taskgroup_destroy(void* p) {
    PyAdamasTaskGroup* g = (PyAdamasTaskGroup*)p;
    if (g->tasks) pa_rc_dec(g->tasks);
}

void* pa_taskgroup_new(void) {
    loop_ensure();
    PyAdamasTaskGroup* g = (PyAdamasTaskGroup*)rc_alloc((int64_t)sizeof(PyAdamasTaskGroup),
                                                taskgroup_destroy, NULL, PYADAMAS_RC_OBJ);
    g->tasks = pa_list_new(4, 1);
    return g;
}

void* pa_taskgroup_add(void* gp, void* coro) {
    if (!gp) pa_raise_c("TypeError", "create_task() expects a TaskGroup");
    PyAdamasTaskGroup* g = (PyAdamasTaskGroup*)gp;
    void* t = pa_asyncio_create_task(coro, 2);  // TaskGroup 不消费结果，按 unknown 释放
    pa_rc_inc(t);                       // 调用方返回值持有一份
    pa_list_append(g->tasks, (int64_t)t);  // 转移 create_task 的那份
    return t;
}

/* 退出：把组内任务交给一个 gather 等待；gather 的异常会原样抛给调用方。 */
void* pa_taskgroup_exit(void* gp) {
    if (!gp) pa_raise_c("TypeError", "__aexit__() expects a TaskGroup");
    PyAdamasTaskGroup* g = (PyAdamasTaskGroup*)gp;
    int64_t n = g->tasks->len;
    PyAdamasList* L = pa_list_new(n > 0 ? n : 1, 1);
    for (int64_t i = 0; i < n; ++i) {
        int64_t t = g->tasks->data[i];
        pa_rc_inc((void*)t);
        pa_list_append(L, t);
    }
    void* fut = pa_asyncio_gather(L, 2);  // TaskGroup 结果类型不可见，按 unknown 释放
    pa_rc_dec(L);
    return fut;
}

void pa_taskgroup_cancel(void* gp) {
    if (!gp) return;
    PyAdamasTaskGroup* g = (PyAdamasTaskGroup*)gp;
    for (int64_t i = 0; i < g->tasks->len; ++i)
        pa_asyncio_task_cancel((void*)(uintptr_t)g->tasks->data[i]);
}

/* ── asyncio.Semaphore ───────────────────────────────────────── */

typedef struct {
    int64_t value;
    PyAdamasList* waiters; /* Future* */
} PyAdamasSem;

static void sem_destroy(void* p) {
    PyAdamasSem* s = (PyAdamasSem*)p;
    if (s->waiters) pa_rc_dec(s->waiters);
}

void* pa_sem_new(int64_t value) {
    loop_ensure();
    PyAdamasSem* s = (PyAdamasSem*)rc_alloc((int64_t)sizeof(PyAdamasSem), sem_destroy, NULL,
                                    PYADAMAS_RC_OBJ);
    s->value = value > 0 ? value : 0;
    s->waiters = pa_list_new(4, 1);
    return s;
}

void* pa_sem_acquire(void* sp) {
    if (!sp) pa_raise_c("TypeError", "acquire() expects a Semaphore");
    PyAdamasSem* s = (PyAdamasSem*)sp;
    void* f = pa_asyncio_future_new();
    if (s->value > 0) {
        --s->value;
        pa_future_set_result(f, 1);
        return f;
    }
    pa_rc_inc(f);
    pa_list_append(s->waiters, (int64_t)f);
    return f;
}

void pa_sem_release(void* sp) {
    if (!sp) pa_raise_c("TypeError", "release() expects a Semaphore");
    PyAdamasSem* s = (PyAdamasSem*)sp;
    if (s->waiters->len > 0) {
        void* f = (void*)(uintptr_t)s->waiters->data[0];
        pa_future_set_result(f, 1);
        pa_list_del_index(s->waiters, 0);
        return; /* 名额直接转给等待者 */
    }
    ++s->value;
}

int64_t pa_sem_locked(void* sp) {
    if (!sp) return 0;
    return ((PyAdamasSem*)sp)->value == 0 ? 1 : 0;
}

