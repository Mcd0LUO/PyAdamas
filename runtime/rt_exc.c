// rt_exc.c —— 异常对象与 handler 栈展开
#include "rt_internal.h"

/* ── 异常 ─────────────────────────────────────────────────────── */

/* 展开机制：try/with/finally 在运行时压一条 handler 记录，raise 设置
   全局异常后 longjmp 到最内层 setjmp 点。记录本身用 malloc，不参与
   RC/GC；被跳过的普通函数帧由 pa_frame_unwind 按帧记录释放局部引用。 */
PyAdamasHandler* handler_top;
PyAdamasFrame* pa_frame_top;
PyAdamasException* pending_exc;
PyAdamasException* current_exc;
PyAdamasExcSave* exc_save_top;
void* current_exc_inst;  /* 富异常实例（有字段/方法），RC owned；0 = 普通异常 */
PyAdamasList* pending_exc_args;  /* 多参异常的可选 args 表（RC owned）；0=无 */

/* 只释放 target 之上（更内层）的帧：target 所在函数还要继续执行，
   它的槽由自己正常退出时释放。先清空槽再 dec，析构里如果重入到用户
   代码也不会二次释放同一份引用。 */
void pa_frame_unwind(PyAdamasFrame* target) {
    for (PyAdamasFrame* f = pa_frame_top; f && f != target; f = f->prev) {
        for (int64_t i = 0; i < f->n; ++i) {
            /* 低位标记：bit0 = 槽里可能是非堆值（unknown 流过静态堆槽），
               必须走注册表判断；bit1 = 只给 GC 扫描的借来引用，这里不能
               释放。 */
            uintptr_t addr = (uintptr_t)f->slots[i];
            if (addr & 2) continue;
            int unknown = (int)(addr & 1);
            int64_t* p = (int64_t*)(addr & ~(uintptr_t)3);
            int64_t v = *p;
            if (!v) continue;
            *p = 0;
            if (unknown) pa_rc_dec_unknown(v);
            else pa_rc_dec((void*)(uintptr_t)v);
        }
    }
    pa_frame_top = target;
}

void exc_free(PyAdamasException* e) {
    if (!e) return;
    exc_free(e->cause);
    pa_trace_free(e->trace);
    free(e->type);
    free(e->message);
    free(e);
}

PyAdamasException* exc_new(const char* type, const char* msg) {
    PyAdamasException* e = (PyAdamasException*)malloc(sizeof(PyAdamasException));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc(strlen(type) + 1);
    e->message = (char*)malloc(strlen(msg) + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    strcpy(e->type, type);
    strcpy(e->message, msg);
    e->cause = NULL;
    e->trace = NULL;
    return e;
}

PyAdamasException* exc_clone(PyAdamasException* e) {
    if (!e) return NULL;
    PyAdamasException* c = exc_new(e->type, e->message);
    c->cause = exc_clone(e->cause);
    c->trace = pa_trace_clone(e->trace);
    return c;
}

/* 自定义异常类的父类表：按模块初始化顺序注册。名字都是 malloc 的，
   生命周期到进程结束。 */
typedef struct ExcReg {
    char* child;
    char* parent;
    struct ExcReg* next;
} ExcReg;

static ExcReg* exc_regs;

void pa_exc_register(PyAdamasStr* child, PyAdamasStr* parent) {
    if (!child || !parent) return;
    ExcReg* r = (ExcReg*)malloc(sizeof(ExcReg));
    if (!r) panic_c("out of memory");
    r->child = (char*)malloc((size_t)child->len + 1);
    r->parent = (char*)malloc((size_t)parent->len + 1);
    if (!r->child || !r->parent) panic_c("out of memory");
    memcpy(r->child, child->data, (size_t)child->len);
    r->child[child->len] = '\0';
    memcpy(r->parent, parent->data, (size_t)parent->len);
    r->parent[parent->len] = '\0';
    r->next = exc_regs;
    exc_regs = r;
}

const char* exc_parent(const char* name) {
    for (ExcReg* r = exc_regs; r; r = r->next)
        if (strcmp(r->child, name) == 0) return r->parent;
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
        {"UnboundLocalError", "NameError"},
        {"QueueEmpty", "Exception"},
        {"QueueFull", "Exception"},
        {"SubprocessError", "Exception"},
        {"TimeoutExpired", "SubprocessError"},
        {"CalledProcessError", "SubprocessError"},
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

void exc_print_uncaught(PyAdamasException* e) {
    fflush(stdout);
    pa_print_traceback();
    pa_trace_print(e);
    if (e->message[0])
        fprintf(stderr, "pyadamas: uncaught %s: %s\n", e->type, e->message);
    else
        fprintf(stderr, "pyadamas: uncaught %s\n", e->type);
}

_Noreturn void exc_longjmp(void) {
    if (!handler_top) {
        exc_print_uncaught(pending_exc);
        exit(1);
    }
    /* 跳过的帧不会再执行自己的出口清理：按记录把里面的堆引用释放掉。 */
    pa_frame_unwind(handler_top->frame);
    /* 必须和生成代码里的 _setjmp 配对：glibc 的 setjmp 存信号掩码，
       _setjmp 不存，两者混用是未定义行为。 */
    PA_LONGJMP(handler_top->env, 1);
}

/* handler 记录的空闲链表。try 在热循环里每次进入都要一条记录，用
   malloc/free 的话每轮两次堆操作 —— 实测 300 万次 try 因此多花 400ms，
   占整个循环的 99%。记录严格是栈式的，空闲块复用即可，链表长度只取决于
   最大嵌套深度，不会无限增长。 */
static PyAdamasHandler* handler_freelist = NULL;

static void handler_release(PyAdamasHandler* h) {
    h->prev = handler_freelist;
    handler_freelist = h;
}


#if defined(_WIN32)
/* Win64 上不能用 CRT 的 setjmp/longjmp：msvcrt 的 longjmp 会走 SEH 展开，
   而我们生成的汇编函数没有 .pdata/.xdata 条目（那段代码不走 GCC），
   展开会失败。这里自带一对：只保存/恢复非易失寄存器 + rsp + 返回地址，
   完全不依赖展开信息。布局 Must 与生成代码一致（都用 handler 记录首字段）。
   布局（与 pa_win_longjmp 共用，调用点只把它当不透明 240 字节）：
     0 rsp / 8 rbx / 16 rbp / 24 rdi / 32 rsi / 40 r12 / 48 r13 / 56 r14
     / 64 r15 / 72 返回地址 / 80+16*i xmm(6+i)，i=0..9 共 160 字节。
   注意这里**不**保存 xmm6–15。Win64 的 ABI 确实把它们算作非易失，但：
   本编译器的浮点槽提升只在「函数内没有任何调用」时发生，而带 try 的函数
   一定会有调用，所以生成代码里不会有跨 setjmp 存活的 xmm 常驻值；运行时
   的 C 代码由 GCC 编译，它自己会保证非易失寄存器。实测给这里加上 xmm
   保存/恢复反而会让 27_runtime / 30_future_cb / 62_re_subfn 等用例从通过
   变成失败（probable 原因：longjmp 时恢复 xmm 与 resume 路径上仍然活着的
   浮点临时值冲突）。故保持与 POSIX 分支一致的寄存器集合。 */
__asm__(
    ".text\n"
    ".globl pa_win_setjmp\n"
    "pa_win_setjmp:\n"
    "\tleaq 8(%rsp), %rax\n"
    "\tmovq %rax, 0(%rcx)\n"
    "\tmovq %rbx, 8(%rcx)\n"
    "\tmovq %rbp, 16(%rcx)\n"
    "\tmovq %rdi, 24(%rcx)\n"
    "\tmovq %rsi, 32(%rcx)\n"
    "\tmovq %r12, 40(%rcx)\n"
    "\tmovq %r13, 48(%rcx)\n"
    "\tmovq %r14, 56(%rcx)\n"
    "\tmovq %r15, 64(%rcx)\n"
    "\tmovq (%rsp), %rax\n"
    "\tmovq %rax, 72(%rcx)\n"
    "\txorl %eax, %eax\n"
    "\tret\n");

__asm__(
    ".text\n"
    ".globl pa_win_longjmp\n"
    "pa_win_longjmp:\n"
    "\tmovq 72(%rcx), %r10\n"
    "\tmovq 0(%rcx), %rsp\n"
    "\tmovq 8(%rcx), %rbx\n"
    "\tmovq 16(%rcx), %rbp\n"
    "\tmovq 24(%rcx), %rdi\n"
    "\tmovq 32(%rcx), %rsi\n"
    "\tmovq 40(%rcx), %r12\n"
    "\tmovq 48(%rcx), %r13\n"
    "\tmovq 56(%rcx), %r14\n"
    "\tmovq 64(%rcx), %r15\n"
    "\tmovl %edx, %eax\n"
    "\ttestl %eax, %eax\n"
    "\tjne 1f\n"
    "\tmovl $1, %eax\n"
    "1:\tjmp *%r10\n");
#endif

void* pa_handler_push(void) {
    PyAdamasHandler* h;
    if (handler_freelist) {
        h = handler_freelist;
        handler_freelist = h->prev;
    } else {
        h = (PyAdamasHandler*)malloc(sizeof(PyAdamasHandler));
        if (!h) panic_c("out of memory");
    }
    h->prev = handler_top;
    h->mark = pa_rc_mark();
    h->frame = pa_frame_top;
    handler_top = h;
    return h;
}

/* 正常/异常展开用：弹出记录并把受保护区产生的临时值释放掉。 */
void pa_handler_leave(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on leave");
    handler_top = h->prev;
    pa_rc_cleanup(h->mark);
    handler_release(h);
}

/* 早期退出（return/break/continue）用：只弹记录，临时值留给外层
   fn/loop 水位统一清理，避免把返回值临时值提前释放。 */
void pa_handler_discard(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on discard");
    handler_top = h->prev;
    handler_release(h);
}

/* 生成器在 yield 处返回：handler 记录必须从全局栈上摘下来，否则后面
   任何异常都会 longjmp 进已经返回的 resume 帧。重新 resume 时按原顺序
   压回去，并重新 setjmp（由生成代码的 Setjmp 指令完成）。 */
void pa_handler_suspend(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on suspend");
    handler_top = h->prev;
}

void pa_handler_rearm(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    h->prev = handler_top;
    h->mark = pa_rc_mark();
    h->frame = pa_frame_top;
    handler_top = h;
}

/* 生成器 close：挂起期间记录不在栈上，直接 free。 */
void pa_handler_free(void* p) { free(p); }

int64_t pa_exc_matches(PyAdamasStr* want) {
    if (!pending_exc) return 0;
    char name[128];
    int64_t n = want->len;
    if (n < 0 || n >= (int64_t)sizeof(name)) return 0;
    memcpy(name, want->data, (size_t)n);
    name[n] = '\0';
    return exc_is_a(pending_exc->type, name) ? 1 : 0;
}

PyAdamasStr* pa_exc_accept(void) {
    PyAdamasExcSave* s = (PyAdamasExcSave*)malloc(sizeof(PyAdamasExcSave));
    if (!s) panic_c("out of memory");
    s->prev = current_exc;
    s->up = exc_save_top;
    exc_save_top = s;
    current_exc = pending_exc;
    pending_exc = NULL;
    const char* msg = current_exc ? current_exc->message : "";
    return str_new(msg, (int64_t)strlen(msg));
}

void pa_exc_done(void) {
    exc_free(current_exc);
    current_exc = NULL;
    if (current_exc_inst) { pa_rc_dec(current_exc_inst); current_exc_inst = NULL; }
    if (exc_save_top) {
        PyAdamasExcSave* s = exc_save_top;
        exc_save_top = s->up;
        current_exc = s->prev;
        free(s);
    }
}

/* 生成器/协程挂起时要把异常上下文（当前异常 + 保存节点栈）一起带走：
   恢复执行后 except 体里的 current_exc 仍然是挂起前那一个，而调用方
   在生成器返回期间看到的是它自己的上下文。 */
typedef struct PyAdamasExcNode {
    PyAdamasException* cur;
    PyAdamasExcSave* top;
    struct PyAdamasExcNode* prev;
} PyAdamasExcNode;

static PyAdamasExcNode* exc_node_top;
static int64_t exc_node_depth;

int64_t pa_exc_depth(void) { return exc_node_depth; }

int64_t pa_exc_enter(int64_t* cur, int64_t* top) {
    PyAdamasException* gc = (PyAdamasException*)(uintptr_t)*cur;
    PyAdamasExcSave* gt = (PyAdamasExcSave*)(uintptr_t)*top;
    // 两边都空时不用压节点：绝大多数 resume 都走这条快路径。
    if (current_exc || exc_save_top || gc || gt) {
        PyAdamasExcNode* n = (PyAdamasExcNode*)malloc(sizeof(PyAdamasExcNode));
        if (!n) panic_c("out of memory");
        n->cur = current_exc;
        n->top = exc_save_top;
        n->prev = exc_node_top;
        exc_node_top = n;
        ++exc_node_depth;
    }
    current_exc = gc;
    exc_save_top = gt;
    *cur = 0;
    *top = 0;
    return exc_node_depth;
}

void pa_exc_leave(int64_t* cur, int64_t* top, int64_t token) {
    *cur = (int64_t)(uintptr_t)current_exc;
    *top = (int64_t)(uintptr_t)exc_save_top;
    int64_t before = exc_node_depth;
    while (exc_node_depth > token && exc_node_top) {
        PyAdamasExcNode* n = exc_node_top;
        exc_node_top = n->prev;
        --exc_node_depth;
        current_exc = n->cur;
        exc_save_top = n->top;
        free(n);
    }
    // 进入时没有压节点：调用方本来就没有异常上下文，恢复成空。
    if (before == token) {
        current_exc = NULL;
        exc_save_top = NULL;
    }
}

void pa_exc_unwind(int64_t depth) {
    while (exc_node_depth > depth && exc_node_top) {
        PyAdamasExcNode* n = exc_node_top;
        exc_node_top = n->prev;
        --exc_node_depth;
        current_exc = n->cur;
        exc_save_top = n->top;
        free(n);
    }
}

void pa_exc_clear(void) {
    exc_free(pending_exc);
    pending_exc = NULL;
}

PyAdamasStr* pa_exc_type_name(void) {
    const char* t = pending_exc ? pending_exc->type : "";
    return str_new(t, (int64_t)strlen(t));
}

PyAdamasStr* pa_exc_message(void) {
    const char* m = pending_exc ? pending_exc->message : "";
    return str_new(m, (int64_t)strlen(m));
}

void pa_raise(PyAdamasStr* type, PyAdamasStr* msg) {
    char tn[128];
    int64_t tn_len = type->len;
    if (tn_len < 0 || tn_len >= (int64_t)sizeof(tn)) panic_c("exception type name too long");
    memcpy(tn, type->data, (size_t)tn_len);
    tn[tn_len] = '\0';
    exc_free(pending_exc);
    PyAdamasException* e = (PyAdamasException*)malloc(sizeof(PyAdamasException));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc((size_t)tn_len + 1);
    e->message = (char*)malloc((size_t)msg->len + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    memcpy(e->type, tn, (size_t)tn_len + 1);
    memcpy(e->message, msg->data, (size_t)msg->len);
    e->message[msg->len] = '\0';
    e->cause = NULL;
    e->trace = NULL;
    pending_exc = e;
    exc_longjmp();
}

void pa_reraise(void) {
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

void pa_exc_reraise(void) {
    if (!pending_exc) panic_c("reraise without a pending exception");
    exc_longjmp();
}

/* 运行时错误入口：固定类型名 + C 字符串消息。 */
void pa_raise_c(const char* type, const char* msg) {
    exc_free(pending_exc);
    pending_exc = exc_new(type, msg);
    exc_longjmp();
}


/* ── 异常值对象 ───────────────────────────────────────────────── */

static void exc_value_destroy(void* p) {
    PyAdamasExcValue* v = (PyAdamasExcValue*)p;
    free(v->type);
    free(v->message);
    if (v->args) pa_rc_dec(v->args);
    if (v->cause) pa_rc_dec(v->cause);
}

static void exc_value_gc_release(void* p) {
    PyAdamasExcValue* v = (PyAdamasExcValue*)p;
    if (v->args) pa_gc_unref_child(v->args);
    if (v->cause) pa_gc_unref_child(v->cause);
}

static PyAdamasExcValue* exc_value_alloc(const char* type, const char* msg) {
    PyAdamasExcValue* v = (PyAdamasExcValue*)rc_alloc((int64_t)sizeof(PyAdamasExcValue),
                                              exc_value_destroy, exc_value_gc_release,
                                              PYADAMAS_RC_OBJ);
    v->magic = PYADAMAS_EXCVAL_MAGIC;
    size_t tn = strlen(type);
    size_t mn = strlen(msg);
    v->type = (char*)malloc(tn + 1);
    v->message = (char*)malloc(mn + 1);
    if (!v->type || !v->message) panic_c("out of memory");
    memcpy(v->type, type, tn + 1);
    memcpy(v->message, msg, mn + 1);
    v->args = pa_list_new(2, 1);
    v->cause = NULL;
    return v;
}

PyAdamasExcValue* pa_exc_value_new(PyAdamasStr* type, PyAdamasStr* msg) {
    if (!type) pa_raise_c("TypeError", "exception type must be a string");
    char tn[128];
    int64_t n = type->len;
    if (n < 0 || n >= (int64_t)sizeof(tn)) pa_raise_c("ValueError", "exception type too long");
    memcpy(tn, type->data, (size_t)n);
    tn[n] = '\0';
    PyAdamasExcValue* v = exc_value_alloc(tn, msg ? msg->data : "");
    if (msg) {
        // args 拥有一个引用；消息字符串本身由调用方/临时栈持有。
        pa_rc_inc(msg);
        pa_list_append(v->args, (int64_t)msg);
    }
    return v;
}


/* 多参异常：args 是已字符串化的实参列表（PyAdamasStr*）。message 取
   args 的逗号join，符合 CPython 对多参异常 str() 的直观；args 原样存。 */
/* 多参异常的消息 = args 的元组 repr，如 (a, b, c)。*/
static void args_buf_put(char** buf, size_t* cap, size_t* len, const char* p, int64_t n) {
    while (*len + (size_t)n + 1 > *cap) { *cap *= 2; *buf = (char*)realloc(*buf, *cap); }
    memcpy(*buf + *len, p, (size_t)n); *len += (size_t)n; (*buf)[*len] = 0;
}
static char* join_args_cstr(PyAdamasList* args) {
    size_t cap = 64, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) panic_c("out of memory");
    buf[0] = 0;
    args_buf_put(&buf, &cap, &len, "(", 1);
    for (int64_t i = 0; i < (args ? args->len : 0); ++i) {
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)args->data[i];
        const char* p = s ? s->data : "";
        int64_t n = s ? s->len : 0;
        if (i) args_buf_put(&buf, &cap, &len, ", ", 2);
        args_buf_put(&buf, &cap, &len, p, n);
    }
    if (args && args->len == 1) args_buf_put(&buf, &cap, &len, ",", 1);
    args_buf_put(&buf, &cap, &len, ")", 1);
    return buf;
}

/* 抛多参异常：args 为字符串化实参列表。 */
_Noreturn void pa_raise_args(PyAdamasStr* type, PyAdamasList* args) {
    char tn[128];
    int64_t tn_len = type->len;
    if (tn_len < 0 || tn_len >= (int64_t)sizeof(tn)) panic_c("exception type name too long");
    memcpy(tn, type->data, (size_t)tn_len); tn[tn_len] = '\0';
    char* msg = join_args_cstr(args);
    exc_free(pending_exc);
    PyAdamasException* e = (PyAdamasException*)malloc(sizeof(PyAdamasException));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc((size_t)tn_len + 1);
    e->message = (char*)malloc(strlen(msg) + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    memcpy(e->type, tn, (size_t)tn_len + 1);
    strcpy(e->message, msg);
    e->cause = NULL;
    e->trace = NULL;
    free(msg);
    if (pending_exc_args) { pa_rc_dec(pending_exc_args); pending_exc_args = NULL; }
    if (args) { pa_rc_inc(args); pending_exc_args = args; }
    pending_exc = e;
    exc_longjmp();
}

/* 多参异常值对象：args 是字符串化实参列表，message 取 join。 */
PyAdamasExcValue* pa_exc_value_new_args(PyAdamasStr* type, PyAdamasList* args) {
    if (!type) pa_raise_c("TypeError", "exception type must be a string");
    char tn[128];
    int64_t n = type->len;
    if (n < 0 || n >= (int64_t)sizeof(tn)) pa_raise_c("ValueError", "exception type too long");
    memcpy(tn, type->data, (size_t)n); tn[n] = '\0';
    char* msg = join_args_cstr(args);
    PyAdamasExcValue* v = exc_value_alloc(tn, msg);
    free(msg);
    for (int64_t i = 0; i < (args ? args->len : 0); ++i) {
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)args->data[i];
        if (!s) continue;
        pa_rc_inc(s);
        pa_list_append(v->args, (int64_t)s);
    }
    return v;
}

PyAdamasExcValue* pa_exc_value_from_exc(PyAdamasException* e) {
    if (!e) return NULL;
    PyAdamasExcValue* v = exc_value_alloc(e->type, e->message);
    /* 多参异常：优先用 raise 时保留的 args 表重建 e.args；否则退化为单消息。 */
    if (pending_exc_args && pending_exc_args->len >= 1) {
        for (int64_t i = 0; i < pending_exc_args->len; ++i) {
            PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)pending_exc_args->data[i];
            if (!s) continue;
            pa_rc_inc(s);
            pa_list_append(v->args, (int64_t)s);
        }
    } else {
        PyAdamasStr* msg = str_new(e->message, (int64_t)strlen(e->message));
        pa_list_append(v->args, (int64_t)msg);
    }
    if (e->cause) v->cause = pa_exc_value_from_exc(e->cause);
    return v;
}

PyAdamasException* pa_exc_from_value(PyAdamasExcValue* v) {
    if (!v) pa_raise_c("TypeError", "expected an exception instance");
    if (v->magic != PYADAMAS_EXCVAL_MAGIC) pa_raise_c("TypeError", "expected an exception instance");
    PyAdamasException* e = exc_new(v->type, v->message);
    /* 把 ExcValue 的 args 存下来，except as e 重建时能还原多参。 */
    if (v->args && v->args->len >= 1) {
        if (pending_exc_args) { pa_rc_dec(pending_exc_args); pending_exc_args = NULL; }
        pa_rc_inc(v->args);
        pending_exc_args = v->args;
    }
    if (v->cause) e->cause = pa_exc_from_value(v->cause);
    return e;
}

void pa_raise_value_from(PyAdamasExcValue* v, PyAdamasExcValue* cause) {
    exc_free(pending_exc);
    pending_exc = pa_exc_from_value(v);
    if (cause) {
        exc_free(pending_exc->cause);
        pending_exc->cause = pa_exc_from_value(cause);
    }
    exc_longjmp();
}

void pa_raise_value(PyAdamasExcValue* v) { pa_raise_value_from(v, NULL); }

/* 抛富异常实例：控制流仍用扁平异常做类型匹配，实例单独存起来。
   只记录类名做 except 匹配；实例在 except as e 时取回。 */
void pa_raise_instance(void* inst, PyAdamasStr* type) {
    if (!inst) pa_raise_c("TypeError", "exception instance is null");
    char tn[128];
    int64_t n = type ? type->len : 0;
    if (n < 0 || n >= (int64_t)sizeof(tn)) pa_raise_c("ValueError", "exception type too long");
    if (n > 0) memcpy(tn, type->data, (size_t)n);
    tn[n] = 0;
    exc_free(pending_exc);
    if (current_exc_inst) pa_rc_dec(current_exc_inst);
    pa_rc_inc(inst);
    current_exc_inst = inst;
    pending_exc = exc_new(tn, "");
    exc_longjmp();
}

/* except 富异常 as e：取回实例，清空全局。若无富实例返回 0。
   调用方（except as e 绑定富类时）用它替代 pa_exc_accept_value。 */
void* pa_exc_accept_instance(void) {
    void* inst = current_exc_inst;
    current_exc_inst = NULL;
    return inst;
}

/* finally 体要跨挂起保留待重抛异常：先把 pending 复制成异常值（堆对象，
   由帧槽持有），再清空全局指针。挂起期间调用方的 raise/clear 不会碰到它。 */
PyAdamasExcValue* pa_exc_take_value(void) {
    PyAdamasExcValue* v = pa_exc_value_from_exc(pending_exc);
    exc_free(pending_exc);
    pending_exc = NULL;
    return v;
}

/* finally 体结束：槽里还有异常就重新抛出（复制一份再 longjmp），
   没有就正常返回，继续执行 try 之后的代码。 */
void pa_exc_reraise_value_if(PyAdamasExcValue* v) {
    if (!v) return;
    pa_raise_value(v);
}

PyAdamasExcValue* pa_exc_value_cause(PyAdamasExcValue* v) {
    if (!v || v->magic != PYADAMAS_EXCVAL_MAGIC)
        pa_raise_c("TypeError", "expected an exception instance");
    return v->cause;
}

PyAdamasExcValue* pa_exc_accept_value(void) {
    PyAdamasExcSave* s = (PyAdamasExcSave*)malloc(sizeof(PyAdamasExcSave));
    if (!s) panic_c("out of memory");
    s->prev = current_exc;
    s->up = exc_save_top;
    exc_save_top = s;
    current_exc = pending_exc;
    pending_exc = NULL;
    if (!current_exc) return exc_value_alloc("Exception", "");
    PyAdamasExcValue* v = pa_exc_value_from_exc(current_exc);
    /* args 已被 ExcValue 各 rc_inc 复制走，释放暂存的列表。 */
    if (pending_exc_args) { pa_rc_dec(pending_exc_args); pending_exc_args = NULL; }
    return v;
}

PyAdamasList* pa_exc_value_args(PyAdamasExcValue* v) {
    if (!v || v->magic != PYADAMAS_EXCVAL_MAGIC)
        pa_raise_c("TypeError", "expected an exception instance");
    return v->args;
}

PyAdamasStr* pa_exc_value_str(PyAdamasExcValue* v) {
    if (!v || v->magic != PYADAMAS_EXCVAL_MAGIC)
        pa_raise_c("TypeError", "expected an exception instance");
    return str_new(v->message, (int64_t)strlen(v->message));
}
