// rt_task.c —— Task 与 Future
#include "rt_internal.h"

/* py23 函数值 = 闭包对象：[0] 代码地址，%r10 传闭包指针（环境）。
   C 没有隐藏寄存器参数，运行时回调用一个跳板。 */
__asm__(
    ".text\n"
    ".globl py23_call1\n"
    ".type py23_call1,@function\n"
    "py23_call1:\n"
    "\tmovq %rdi, %r10\n"
    "\tmovq (%rdi), %r11\n"
    "\tmovq %rsi, %rdi\n"
    "\txorl %eax, %eax\n"
    "\tjmp *%r11\n"
    ".size py23_call1, .-py23_call1\n");

/* 完成时同步调用 add_done_callback 注册的函数，参数是 future 本身。
   CPython 用 call_soon 排队，这里直接调用；回调抛出的异常会向外传播。 */
static void future_run_callbacks(Py23Await* f) {
    Py23List* cbs = f->callbacks;
    if (!cbs || cbs->len == 0) return;
    // 注册顺序调用。先把长度清零，回调里再注册的（future 已完成）
    // 会走立即调用分支，不会追加到本轮。
    int64_t n = cbs->len;
    cbs->len = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t cb = cbs->data[i];
        py23_call1((void*)(uintptr_t)cb, (int64_t)f);
        py23_rc_dec_unknown(cb);
    }
}

void* py23_asyncio_create_task(void* coro) {
    if (!frame_is_coro(coro)) {
        // 也接受 Future/sleep/fd/gather 等可等待对象：Task 直接等它完成。
        if (!is_await_obj(coro))
            py23_raise_c("TypeError", "asyncio.create_task() expects an awaitable");
        loop_ensure();
        Py23Await* t = await_new(3);
        t->driver_gen = (int64_t)coro;
        py23_rc_inc(coro);
        py23_rc_inc(coro);  // start_awaitable 接管这个引用
        start_awaitable(coro, t);
        return t;
    }
    loop_ensure();
    int64_t* p = (int64_t*)coro;
    if (p[1] != 0)
        py23_raise_c("RuntimeError", "coroutine was already awaited");
    Py23Await* t = await_new(3);
    t->task_coro = (int64_t)coro;
    py23_rc_inc(coro);
    py23_list_append(loop_pending, (int64_t)t);  // 持有 task 的引用
    p[7] = (int64_t)t;
    py23_rc_inc(t);
    p[1] = 4;
    py23_rc_inc(coro);
    ready_push(coro);
    // pending 列表持有一个引用；返回值给调用方另一个引用。
    py23_rc_inc(t);
    return t;
}

int64_t py23_asyncio_task_done(void* t) {
    return is_await_obj(t) && ((Py23Await*)t)->kind == 3 && ((Py23Await*)t)->done;
}

int64_t py23_asyncio_task_cancel(void* t) {
    if (!is_await_obj(t) || ((Py23Await*)t)->kind != 3)
        py23_raise_c("TypeError", "cancel() expects a task");
    Py23Await* w = (Py23Await*)t;
    if (w->done) return 0;
    if (!w->task_coro && w->driver_gen) {
        // 等待对象型 Task：完成自己并沿等待链传播取消。
        void* sub = (void*)w->driver_gen;
        py23_rc_inc(sub);
        w->done = 1;
        w->cancelled = 1;
        Py23Exception* e = exc_new("CancelledError", "");
        void* a = (void*)w->waiter;
        w->waiter = 0;
        if (a) deliver_owned(a, w, 0, e);
        else w->exc = (int64_t)e;
        cancel_chain(sub);
        py23_rc_dec(sub);
        return 1;
    }
    void* c = (void*)w->task_coro;
    if (!c) return 0;
    w->cancelled = 1;
    int64_t* p = (int64_t*)c;
    Py23Exception* e = exc_new("CancelledError", "");
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

void gather_cancel(Py23Await* g) {
    if (g->done) return;
    if (g->children)
        for (int64_t i = 0; i < g->children->len; ++i)
            cancel_chain((void*)g->children->data[i]);
    g->done = 1;
    void* owned = pending_take(g);
    void* a = (void*)g->waiter;
    g->waiter = 0;
    Py23Exception* e = exc_new("CancelledError", "");
    if (a) deliver_owned(a, g, 0, e);
    else g->exc = (int64_t)e;
    if (owned) py23_rc_dec(owned);
}

void future_cancel(Py23Await* f) {
    if (f->done) return;
    f->done = 1;
    f->cancelled = 1;
    Py23Exception* stored = exc_new("CancelledError", "");
    if (f->waiters) {
        int64_t n = f->waiters->len;
        f->waiters->len = 0;
        for (int64_t i = 0; i < n; ++i)
            deliver_owned((void*)f->waiters->data[i], f, 0, exc_clone(stored));
    }
    f->exc = (int64_t)stored;
    void* owned = pending_take(f);
    if (owned) py23_rc_dec(owned);
    future_run_callbacks(f);
}

/* sleep/fd 这类没有内部链的等待对象：摘表、标记完成、通知等待者。 */
void simple_wait_cancel(Py23Await* w) {
    if (w->done) return;
    w->done = 1;
    Py23List* lst = w->kind == 1 ? loop_timers : (w->kind == 6 ? loop_fds : NULL);
    int64_t list_owned = 0;
    if (lst)
        for (int64_t i = 0; i < lst->len; ++i)
            if (lst->data[i] == (int64_t)w) {
                list_owned = list_take_at(lst, i);
                break;
            }
    void* a = (void*)w->waiter;
    w->waiter = 0;
    Py23Exception* e = exc_new("CancelledError", "");
    if (a) deliver_owned(a, w, 0, e);
    else w->exc = (int64_t)e;
    if (list_owned) py23_rc_dec((void*)list_owned);
}

/* 沿 await 链把取消传下去。w 的引用由调用方保证存活。 */
void cancel_chain(void* w) {
    if (!w) return;
    if (frame_is_coro(w)) {
        cancel_suspended(w, exc_new("CancelledError", ""));
        return;
    }
    if (!is_await_obj(w)) return;
    Py23Await* aw = (Py23Await*)w;
    switch (aw->kind) {
        case 1:
        case 6: simple_wait_cancel(aw); break;
        case 2: gather_cancel(aw); break;
        case 3: py23_asyncio_task_cancel(w); break;
        case 5: future_cancel(aw); break;
        default: break;  // driver 让它自然结束
    }
}

void future_settle(Py23Await* f, int64_t value, Py23Exception* exc) {
    if (f->done) {
        if (exc) exc_free(exc);
        else py23_rc_dec_unknown(value);
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
            if (copy) py23_rc_inc_unknown(copy);
            deliver_owned((void*)f->waiters->data[i], f, copy, exc_clone(exc));
        }
    }
    void* owned = pending_take(f);
    if (owned) py23_rc_dec(owned);
    future_run_callbacks(f);
}

void* py23_asyncio_future_new(void) {
    loop_ensure();
    Py23Await* f = await_new(5);
    f->waiters = py23_list_new(4, 1);
    f->callbacks = py23_list_new(2, 1);
    return f;
}

void py23_future_set_result(void* fp, int64_t value) {
    Py23Await* f = (Py23Await*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        py23_raise_c("TypeError", "set_result() expects a Future");
    if (f->done) py23_raise_c("RuntimeError", "future is already done");
    future_settle(f, value, NULL);
}

/* py23 的异常不是一等值：set_exception 接收消息字符串，失败类型统一
   记为 Exception，与 exception() 返回消息的约定一致。 */
void py23_future_set_exception(void* fp, Py23Str* msg) {
    Py23Await* f = (Py23Await*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        py23_raise_c("TypeError", "set_exception() expects a Future");
    if (f->done) py23_raise_c("RuntimeError", "future is already done");
    Py23Exception* e = exc_new("Exception", msg ? msg->data : "");
    future_settle(f, 0, e);
}

void py23_future_add_callback(void* fp, int64_t cb) {
    Py23Await* f = (Py23Await*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        py23_raise_c("TypeError", "add_done_callback() expects a Future");
    if (f->done) {
        // 调用方把 cb 的引用交给了这里；同步调用完就还掉。
        py23_call1((void*)(uintptr_t)cb, (int64_t)f);
        py23_rc_dec_unknown(cb);
        return;
    }
    if (!f->callbacks) f->callbacks = py23_list_new(2, 1);
    py23_list_append(f->callbacks, cb);  // 接管 cb 的引用
}

int64_t py23_future_done(void* fp) {
    return is_await_obj(fp) && ((Py23Await*)fp)->kind == 5 && ((Py23Await*)fp)->done;
}

int64_t py23_future_cancelled(void* fp) {
    if (!is_await_obj(fp) || ((Py23Await*)fp)->kind != 5) return 0;
    Py23Await* f = (Py23Await*)fp;
    return f->done && f->cancelled;
}

int64_t py23_future_cancel(void* fp) {
    if (!is_await_obj(fp) || ((Py23Await*)fp)->kind != 5)
        py23_raise_c("TypeError", "cancel() expects a Future");
    Py23Await* f = (Py23Await*)fp;
    if (f->done) return 0;
    future_cancel(f);
    return 1;
}

void raise_exception_copy(Py23Exception* e, const char* fallback) {
    Py23Exception* copy = e ? exc_clone(e) : exc_new("RuntimeError", fallback);
    exc_free(pending_exc);
    pending_exc = copy;
    exc_longjmp();
}

int64_t py23_future_result(void* fp) {
    if (!is_await_obj(fp) || ((Py23Await*)fp)->kind != 5)
        py23_raise_c("TypeError", "result() expects a Future");
    Py23Await* f = (Py23Await*)fp;
    if (!f->done) py23_raise_c("RuntimeError", "future is not done");
    if (f->exc) raise_exception_copy((Py23Exception*)f->exc, "future failed");
    int64_t v = f->result;
    if (v) py23_rc_inc_unknown(v);
    return v;
}

Py23Str* py23_future_exception(void* fp) {
    if (!is_await_obj(fp) || ((Py23Await*)fp)->kind != 5)
        py23_raise_c("TypeError", "exception() expects a Future");
    Py23Await* f = (Py23Await*)fp;
    if (!f->done) py23_raise_c("RuntimeError", "future is not done");
    if (!f->exc) return NULL;
    Py23Exception* e = (Py23Exception*)f->exc;
    const char* msg = e->message[0] ? e->message : e->type;
    return str_new(msg, (int64_t)strlen(msg));
}

int64_t py23_asyncio_task_result(void* tp) {
    if (!is_await_obj(tp) || ((Py23Await*)tp)->kind != 3)
        py23_raise_c("TypeError", "result() expects a Task");
    Py23Await* t = (Py23Await*)tp;
    if (!t->done) py23_raise_c("RuntimeError", "task is not done");
    if (t->exc) raise_exception_copy((Py23Exception*)t->exc, "task failed");
    int64_t v = t->result;
    if (v) py23_rc_inc_unknown(v);
    return v;
}

Py23Str* py23_asyncio_task_exception(void* tp) {
    if (!is_await_obj(tp) || ((Py23Await*)tp)->kind != 3)
        py23_raise_c("TypeError", "exception() expects a Task");
    Py23Await* t = (Py23Await*)tp;
    if (!t->done) py23_raise_c("RuntimeError", "task is not done");
    if (!t->exc) return NULL;
    Py23Exception* e = (Py23Exception*)t->exc;
    const char* msg = e->message[0] ? e->message : e->type;
    return str_new(msg, (int64_t)strlen(msg));
}

int64_t py23_asyncio_task_cancelled(void* tp) {
    if (!is_await_obj(tp) || ((Py23Await*)tp)->kind != 3) return 0;
    Py23Await* t = (Py23Await*)tp;
    if (!t->done || !t->exc) return 0;
    return strcmp(((Py23Exception*)t->exc)->type, "CancelledError") == 0;
}

