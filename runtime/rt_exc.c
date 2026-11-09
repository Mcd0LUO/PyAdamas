// rt_exc.c —— 异常对象与 handler 栈展开
#include "rt_internal.h"

/* ── 异常 ─────────────────────────────────────────────────────── */

/* 展开机制：try/with/finally 在运行时压一条 handler 记录，raise 设置
   全局异常后 longjmp 到最内层 setjmp 点。记录本身用 malloc，不参与
   RC/GC；跳过的普通函数帧里的局部引用由保守 GC 回收。 */
Py23Handler* handler_top;
Py23Exception* pending_exc;
Py23Exception* current_exc;
Py23ExcSave* exc_save_top;

void exc_free(Py23Exception* e) {
    if (!e) return;
    free(e->type);
    free(e->message);
    free(e);
}

Py23Exception* exc_new(const char* type, const char* msg) {
    Py23Exception* e = (Py23Exception*)malloc(sizeof(Py23Exception));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc(strlen(type) + 1);
    e->message = (char*)malloc(strlen(msg) + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    strcpy(e->type, type);
    strcpy(e->message, msg);
    return e;
}

Py23Exception* exc_clone(Py23Exception* e) {
    if (!e) return NULL;
    return exc_new(e->type, e->message);
}

const char* exc_parent(const char* name) {
    static const struct {
        const char* name;
        const char* parent;
    } table[] = {
        {"Exception", "BaseException"},
        {"SystemExit", "BaseException"},
        {"KeyboardInterrupt", "BaseException"},
        {"GeneratorExit", "BaseException"},
        {"CancelledError", "BaseException"},
        {"BaseExceptionGroup", "BaseException"},
        {"ArithmeticError", "Exception"},
        {"AssertionError", "Exception"},
        {"AttributeError", "Exception"},
        {"BufferError", "Exception"},
        {"EOFError", "Exception"},
        {"ImportError", "Exception"},
        {"ModuleNotFoundError", "ImportError"},
        {"LookupError", "Exception"},
        {"IndexError", "LookupError"},
        {"KeyError", "LookupError"},
        {"MemoryError", "Exception"},
        {"NameError", "Exception"},
        {"OSError", "Exception"},
        {"IOError", "OSError"},
        {"EnvironmentError", "OSError"},
        {"WindowsError", "OSError"},
        {"FileNotFoundError", "OSError"},
        {"FileExistsError", "OSError"},
        {"IsADirectoryError", "OSError"},
        {"NotADirectoryError", "OSError"},
        {"PermissionError", "OSError"},
        {"TimeoutError", "OSError"},
        {"BlockingIOError", "OSError"},
        {"ChildProcessError", "OSError"},
        {"ConnectionError", "OSError"},
        {"BrokenPipeError", "ConnectionError"},
        {"ConnectionAbortedError", "ConnectionError"},
        {"ConnectionRefusedError", "ConnectionError"},
        {"ConnectionResetError", "ConnectionError"},
        {"InterruptedError", "OSError"},
        {"ProcessLookupError", "OSError"},
        {"ReferenceError", "Exception"},
        {"RuntimeError", "Exception"},
        {"NotImplementedError", "RuntimeError"},
        {"RecursionError", "RuntimeError"},
        {"StopIteration", "Exception"},
        {"StopAsyncIteration", "Exception"},
        {"SyntaxError", "Exception"},
        {"IndentationError", "SyntaxError"},
        {"TabError", "IndentationError"},
        {"SystemError", "Exception"},
        {"TypeError", "Exception"},
        {"ValueError", "Exception"},
        {"UnicodeError", "ValueError"},
        {"UnicodeDecodeError", "UnicodeError"},
        {"UnicodeEncodeError", "UnicodeError"},
        {"UnicodeTranslateError", "UnicodeError"},
        {"FloatingPointError", "ArithmeticError"},
        {"OverflowError", "ArithmeticError"},
        {"ZeroDivisionError", "ArithmeticError"},
        {"Warning", "Exception"},
        {"DeprecationWarning", "Warning"},
        {"UserWarning", "Warning"},
        {"RuntimeWarning", "Warning"},
        {"FutureWarning", "Warning"},
        {"PendingDeprecationWarning", "Warning"},
        {"ResourceWarning", "Warning"},
        {"SyntaxWarning", "Warning"},
        {"BytesWarning", "Warning"},
        {"ImportWarning", "Warning"},
        {"UnicodeWarning", "Warning"},
        {"EncodingWarning", "Warning"},
        {"ExceptionGroup", "Exception"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (strcmp(table[i].name, name) == 0) return table[i].parent;
    /* 不在表里的名字按 Exception 的子类处理：代码生成层只放行
       内置异常名，这里主要是给别名和新增名字留一条保守路径。 */
    if (strcmp(name, "BaseException") == 0) return NULL;
    return "Exception";
}

int exc_is_a(const char* child, const char* want) {
    while (child) {
        if (strcmp(child, want) == 0) return 1;
        child = exc_parent(child);
    }
    return 0;
}

void exc_print_uncaught(Py23Exception* e) {
    fflush(stdout);
    if (e->message[0])
        fprintf(stderr, "py23: uncaught %s: %s\n", e->type, e->message);
    else
        fprintf(stderr, "py23: uncaught %s\n", e->type);
}

void exc_longjmp(void) {
    if (!handler_top) {
        exc_print_uncaught(pending_exc);
        exit(1);
    }
    longjmp(handler_top->env, 1);
}

void* py23_handler_push(void) {
    Py23Handler* h = (Py23Handler*)malloc(sizeof(Py23Handler));
    if (!h) panic_c("out of memory");
    h->prev = handler_top;
    h->mark = py23_rc_mark();
    handler_top = h;
    return h;
}

/* 正常/异常展开用：弹出记录并把受保护区产生的临时值释放掉。 */
void py23_handler_leave(void* p) {
    Py23Handler* h = (Py23Handler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on leave");
    handler_top = h->prev;
    py23_rc_cleanup(h->mark);
    free(h);
}

/* 早期退出（return/break/continue）用：只弹记录，临时值留给外层
   fn/loop 水位统一清理，避免把返回值临时值提前释放。 */
void py23_handler_discard(void* p) {
    Py23Handler* h = (Py23Handler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on discard");
    handler_top = h->prev;
    free(h);
}

/* 生成器在 yield 处返回：handler 记录必须从全局栈上摘下来，否则后面
   任何异常都会 longjmp 进已经返回的 resume 帧。重新 resume 时按原顺序
   压回去，并重新 setjmp（由生成代码的 Setjmp 指令完成）。 */
void py23_handler_suspend(void* p) {
    Py23Handler* h = (Py23Handler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on suspend");
    handler_top = h->prev;
}

void py23_handler_rearm(void* p) {
    Py23Handler* h = (Py23Handler*)p;
    h->prev = handler_top;
    h->mark = py23_rc_mark();
    handler_top = h;
}

/* 生成器 close：挂起期间记录不在栈上，直接 free。 */
void py23_handler_free(void* p) { free(p); }

int64_t py23_exc_matches(Py23Str* want) {
    if (!pending_exc) return 0;
    char name[128];
    int64_t n = want->len;
    if (n < 0 || n >= (int64_t)sizeof(name)) return 0;
    memcpy(name, want->data, (size_t)n);
    name[n] = '\0';
    return exc_is_a(pending_exc->type, name) ? 1 : 0;
}

Py23Str* py23_exc_accept(void) {
    Py23ExcSave* s = (Py23ExcSave*)malloc(sizeof(Py23ExcSave));
    if (!s) panic_c("out of memory");
    s->prev = current_exc;
    s->up = exc_save_top;
    exc_save_top = s;
    current_exc = pending_exc;
    pending_exc = NULL;
    const char* msg = current_exc ? current_exc->message : "";
    return str_new(msg, (int64_t)strlen(msg));
}

void py23_exc_done(void) {
    exc_free(current_exc);
    current_exc = NULL;
    if (exc_save_top) {
        Py23ExcSave* s = exc_save_top;
        exc_save_top = s->up;
        current_exc = s->prev;
        free(s);
    }
}

void py23_exc_clear(void) {
    exc_free(pending_exc);
    pending_exc = NULL;
}

Py23Str* py23_exc_type_name(void) {
    const char* t = pending_exc ? pending_exc->type : "";
    return str_new(t, (int64_t)strlen(t));
}

Py23Str* py23_exc_message(void) {
    const char* m = pending_exc ? pending_exc->message : "";
    return str_new(m, (int64_t)strlen(m));
}

void py23_raise(Py23Str* type, Py23Str* msg) {
    char tn[128];
    int64_t tn_len = type->len;
    if (tn_len < 0 || tn_len >= (int64_t)sizeof(tn)) panic_c("exception type name too long");
    memcpy(tn, type->data, (size_t)tn_len);
    tn[tn_len] = '\0';
    exc_free(pending_exc);
    Py23Exception* e = (Py23Exception*)malloc(sizeof(Py23Exception));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc((size_t)tn_len + 1);
    e->message = (char*)malloc((size_t)msg->len + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    memcpy(e->type, tn, (size_t)tn_len + 1);
    memcpy(e->message, msg->data, (size_t)msg->len);
    e->message[msg->len] = '\0';
    pending_exc = e;
    exc_longjmp();
}

void py23_reraise(void) {
    if (current_exc) {
        exc_free(pending_exc);
        pending_exc = current_exc;
        current_exc = NULL;
        exc_longjmp();
    }
    exc_free(pending_exc);
    pending_exc = exc_new("RuntimeError", "No active exception to reraise");
    exc_longjmp();
}

void py23_exc_reraise(void) {
    if (!pending_exc) panic_c("reraise without a pending exception");
    exc_longjmp();
}

/* 运行时错误入口：固定类型名 + C 字符串消息。 */
void py23_raise_c(const char* type, const char* msg) {
    exc_free(pending_exc);
    pending_exc = exc_new(type, msg);
    exc_longjmp();
}

