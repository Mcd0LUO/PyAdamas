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

/* 异常全局状态里的 GC 根。pending_exc_args（多参异常的 args 表）和
   current_exc_inst（富异常实例）是 RC owned 的普通全局：它们不在临时值栈、
   不在帧记录登记区、也不在寄存器里，major GC 的标记阶段看不见。raise
   把它挂上、accept 才取走/释放；这中间只要有分配触发主收集，它就会被当成
   垃圾清扫，accept 里再读就是 use-after-free（10 万次循环实测 SIGSEGV：
   pa_exc_accept_value -> pa_rc_inc 打在已复用的堆块上，rdi 正是消息串内容）。
   根表登记的是「槽的地址」，所以这两个指针之后的每次赋值都会被扫到。 */
static void* exc_root_slots[2];
static int exc_roots_ready;
static void exc_gc_roots_ensure(void) {
    if (exc_roots_ready) return;
    exc_roots_ready = 1;
    exc_root_slots[0] = (void*)&pending_exc_args;
    exc_root_slots[1] = (void*)&current_exc_inst;
    pa_gc_add_roots(exc_root_slots, 2);
}

/* 构造期根：多步构造出来的异常值，从「刚分配」到「交回调用方（临时栈/槽）」
   之间没有任何 GC 根。major GC 的根集合里不含 C 局部变量，而构造过程中还会
   继续分配（args 列表、消息串、cause 递归调用），中途触发的主收集会把半成品
   当垃圾清扫，函数返回的就是悬空指针。临时值栈本身是 GC 根，而且它接管的
   正是对象自带的那个引用：end 时取回，所有权原样交还调用方。 */
static void exc_construct_begin(void* p) { pa_rc_temp(p); }
static void exc_construct_end(void* p) { pa_rc_temp_take(p); }

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

/* 表驱动展开（--zero-cost-exc）的嵌套深度。由生成代码在进入/离开零开销
   try 时通过 pa_eh_enter()/pa_eh_leave() 维护；> 0 表示当前正处于这样的
   保护区里，此时抛出必须走 _Unwind（setjmp 路径的 handler 栈抓不到它）。
   用计数器而不是布尔：嵌套 try 时内层退出不能把外层的标记清掉。 */
int pa_eh_depth;

void pa_eh_enter(void) { ++pa_eh_depth; }
void pa_eh_leave(void) { if (pa_eh_depth > 0) --pa_eh_depth; }

_Noreturn void exc_longjmp(void) {
    /* 表驱动区里抛出：交给 _Unwind 展开。这里不释放帧记录 —— 展开器会逐帧
       走 personality，由方案 G 在 handler 帧上一次性把被跳过的帧清掉
       （见 rt_eh.c / rt_personality.c）。 */
    if (pa_eh_depth > 0) {
        (void)pa_eh_throw(pending_exc);
        /* 走到这里说明没人接（_URC_END_OF_STACK）或展开器内部出错。
           与下面 setjmp 路径的「没 handler」分支同样处理：打印并退出。 */
        exc_print_uncaught(pending_exc);
        exit(1);
    }
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

typedef struct SuspendedNode {
    PyAdamasHandler* h;
    struct SuspendedNode* next;
} SuspendedNode;

static SuspendedNode* suspended_head;
static SuspendedNode* suspended_pool;

/* 把记录从挂起表里摘掉。leave / discard / rearm 都要调 —— 记录一旦
 * 回到保护区（或直接被归还 freelist），挂起表里就不能再留着它，否则
 * 生成器销毁时会去读一条已经复用/释放的记录（ASan 实测 heap-use-after-free）。 */
static void unsuspend(PyAdamasHandler* h) {
    // **按指针查找，不能先看 suspend_owner。**
    //
    // 记录被 handler_release 归还后会被 pa_handler_push 复用给别的
    // handler，那时 suspend_owner 已经被重置成 NULL。如果这里以
    // `!h->suspend_owner` 提前返回，旧节点就永远留在挂起表里 ——
    // 生成器销毁时遍历到它，读 n->h->suspend_owner 就是读已释放内存
    // （ASan 实测 heap-use-after-free，123_finally_yield / 119）。
    SuspendedNode** pp = &suspended_head;
    while (*pp) {
        SuspendedNode* n = *pp;
        if (n->h == h) {
            *pp = n->next;
            n->next = suspended_pool;
            suspended_pool = n;
            return;
        }
        pp = &n->next;
    }
    h->suspend_owner = NULL;
}

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
    h->heap_alloc = 1;
    h->prev = handler_top;
    h->mark = pa_rc_mark();
    h->frame = pa_frame_top;
    h->suspend_owner = NULL;
    handler_top = h;
    return h;
}

/* 正常/异常展开用：弹出记录并把受保护区产生的临时值释放掉。 */
void pa_handler_leave(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on leave");
    handler_top = h->prev;
    pa_rc_cleanup(h->mark);
    unsuspend(h);
    handler_release(h);
}

/* 早期退出（return/break/continue）用：只弹记录，临时值留给外层
   fn/loop 水位统一清理，避免把返回值临时值提前释放。 */
void pa_handler_discard(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on discard");
    handler_top = h->prev;
    unsuspend(h);
    handler_release(h);
}

/* 生成器在 yield 处返回：handler 记录必须从全局栈上摘下来，否则后面
   任何异常都会 longjmp 进已经返回的 resume 帧。重新 resume 时按原顺序
   压回去，并重新 setjmp（由生成代码的 Setjmp 指令完成）。 */
/* 当前正在 resume 的生成器对象（gen_run / pa_gen_close / pa_gen_finalize
   在调用 resume 前后设置）。suspend 用它给记录打上归属，销毁时按归属
   找回并归还 freelist。 */
static void* gen_owner;

void pa_gen_set_handler_owner(void* g) { gen_owner = g; }

/* 生成器销毁时调用：把它名下所有挂起中的 handler 记录归还 freelist。
 *
 * 记录在挂起期间**必须保持有效**（rearm 用的就是同一个地址，生成代码从
 * 帧槽里取），所以不能提前 free。但生成器可能再也不会 resume —— 被
 * close() 后丢弃、或 finally 里抛异常直接跳出 —— 那些记录就永久泄漏。
 * ASan 实测 93_gen_exc_rc 里 300 个生成器泄漏 67200 字节。
 *
 * 归属信息直接存在记录里（suspend_owner），所以这里要扫整个 freelist
 * 之外的所有记录 —— 用一个全局登记链维护「当前处于挂起状态的记录」。 */


void pa_gen_release_handlers(void* g) {
    if (!g) return;
    SuspendedNode** pp = &suspended_head;
    while (*pp) {
        SuspendedNode* n = *pp;
        if (n->h->suspend_owner != g) {
            pp = &n->next;
            continue;
        }
        *pp = n->next;
        n->h->suspend_owner = NULL;
        handler_release(n->h);
        n->next = suspended_pool;
        suspended_pool = n;
    }
}


void pa_handler_suspend(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (handler_top != h) panic_c("handler stack corrupted on suspend");
    handler_top = h->prev;

    /* 记录已经和 handler_top 脱钩了，必须**无条件**登记到挂起表 ——
     *
     * 原来只在 gen_owner 非空时登记，但实测很多 suspend 发生在
     * gen_owner 为 NULL 的上下文（生成器被 pa_gen_next_opt 驱动时，
     * owner 已经在上一轮 gen_run 结束时清掉了）。那些记录摘链之后
     * 既不在 handler_top 上、也不在挂起表里 —— 永久丢失。
     *
     * ASan 实测 162_pep479：owner=NULL 的 suspend 有 10+ 次，泄漏 240 字节。
     *
     * owner 为 NULL 时挂在一个全局「孤儿」owner 上，由 pa_exc_shutdown
     * 兜底清理；生成器销毁时按自己的 owner 找，不会误删孤儿记录。 */
    h->suspend_owner = gen_owner ? gen_owner : (void*)&handler_top;
    {
        SuspendedNode* n = suspended_pool;
        if (n)
            suspended_pool = n->next;
        else {
            n = (SuspendedNode*)malloc(sizeof(SuspendedNode));
            if (!n) panic_c("out of memory");
        }
        n->h = h;
        n->next = suspended_head;
        suspended_head = n;
    }
}

void pa_handler_rearm(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    unsuspend(h);  // 重新进保护区，之后由 leave/discard 归还
    h->prev = handler_top;
    h->mark = pa_rc_mark();
    h->frame = pa_frame_top;
    handler_top = h;
}

/* 生成器 close：挂起期间记录不在栈上，直接 free。 */
/* 生成器 close / 销毁时释放挂起的 handler 记录。
 *
 * 原来是裸 `free(p)`，有两个问题：
 *   1. 不归还 freelist —— 每次 close 都让池子少一个、多一次 malloc；
 *   2. **不从挂起表摘除** —— 表里留着一条指向已 free 内存的节点，
 *      生成器销毁时遍历到它就 heap-use-after-free。
 *      （ASan 实测 123_finally_yield / 119_gen_except_suspend）
 *
 * 改成归还 freelist 并摘除节点。归还前确认它确实不在保护区里：
 * 仍在 handler_top 链上的记录属于还没退出的 try 块，由 leave/discard 管。 */
/* 归还一条已经从全局链上摘下来的记录（异常展开路径用）。
   与 pa_handler_free 的区别：调用方已经把它从链上摘好了，这里只负责
   摘挂起表 + 归还 freelist。 */
void pa_handler_discard_record(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (!h) return;
    unsuspend(h);
    if (h->heap_alloc) handler_release(h);
}

void pa_handler_free(void* p) {
    PyAdamasHandler* h = (PyAdamasHandler*)p;
    if (!h) return;
    for (PyAdamasHandler* q = handler_top; q; q = q->prev)
        if (q == h) return;  /* 还在保护区里 */
    unsuspend(h);
    // 栈上的记录（gen_run / pa_gen_close 的本地变量）不能进 freelist ——
    // 那会把栈地址当成可复用块，后续 handler_push 从池里取到它再写就是
    // stack-use-after-return（ASan 实测 123_finally_yield）。栈记录本来
    // 就随函数返回自然消失，不需要归还。
    if (h->heap_alloc) handler_release(h);
}

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
    if (getenv("PYADAMAS_ACCDBG"))
        fprintf(stderr, "[done] %p\n", __builtin_return_address(0));
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

/* 释放一条 PyAdamasExcSave 链。
 *
 * 用在「恢复异常上下文时当前链要被换掉」的地方：被换掉的那条如果还有
 * 节点，就没人再引用它们了（pa_exc_accept_value 压的、或 pa_exc_enter 存的）。
 * 不释放就是每层嵌套泄漏 sizeof(PyAdamasExcSave) 字节。
 * ASan 实测 32_exception_value 泄漏 16 字节。 */
static void free_save_chain(PyAdamasExcSave* s) {
    while (s) {
        PyAdamasExcSave* nx = s->up;
        free(s);
        s = nx;
    }
}

void pa_exc_leave(int64_t* cur, int64_t* top, int64_t token) {
    *cur = (int64_t)(uintptr_t)current_exc;
    *top = (int64_t)(uintptr_t)exc_save_top;
    int64_t before = exc_node_depth;
    while (exc_node_depth > token && exc_node_top) {
        PyAdamasExcNode* n = exc_node_top;
        exc_node_top = n->prev;
        --exc_node_depth;
        // 恢复前把「当前链」还掉：它和 n->top 是两条独立的链，直接覆盖
        // exc_save_top 会让当前那条永久丢失。
        if (exc_save_top && exc_save_top != n->top) free_save_chain(exc_save_top);
        exc_free(current_exc);
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
        if (exc_save_top && exc_save_top != n->top) free_save_chain(exc_save_top);
        exc_free(current_exc);
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

void pa_exc_drop_save(void);

void pa_reraise(void) {
    /* 裸 `raise` 会直接 longjmp，跳过 except 块的正常收尾（pa_exc_done）。
     * pa_exc_accept_value 在进 except 体时压了一个 PyAdamasExcSave，必须
     * 在这里还掉 —— 否则每执行一次 `except ...: raise` 泄漏
     * sizeof(PyAdamasExcSave) 字节。
     *
     * 注意 pending_exc 不能动（它就是马上要重抛的那个），只清 save 节点。
     * ASan 实测 32_exception_value 泄漏 16 字节。 */
    pa_exc_drop_save();
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

/* 裸 raise 前调用：把 except 块入口压的 PyAdamasExcSave 还掉，
 * 并把 current_exc 恢复成外层值。
 *
 * 两条 raise 路径（setjmp 的 pa_exc_reraise、零开销的 pa_eh_rethrow）
 * 共用，保证行为一致。 */
void pa_exc_drop_save(void) {
    if (!exc_save_top) return;
    PyAdamasExcSave* s = exc_save_top;
    exc_save_top = s->up;
    // **不动 current_exc**：裸 raise 正要重抛它。
    //
    // 原实现把它 exc_free 掉并恢复成 s->prev，结果 pa_reraise 拿到的
    // current_exc 已经是 NULL/外层值，于是报
    // "No active exception to reraise" 或抛错对象。
    // 实测引入 119_gen_except_suspend / 32_exception_value 两个回归。
    free(s);
}

void pa_exc_reraise(void) {
    if (!pending_exc) panic_c("reraise without a pending exception");
    /* 裸 `raise` 会直接 longjmp 到外层 handler，**跳过 except 块的正常收尾**
     * （也就是 pa_exc_done）。而 pa_exc_accept_value 在进 except 体时压了一个
     * PyAdamasExcSave 节点，那个节点必须在这里还掉 —— 否则每执行一次
     * `except ...: raise` 就泄漏 16 字节（正好 sizeof(PyAdamasExcSave)）。
     *
     * ASan 实测 32_exception_value：`except ValueError as e: ... raise` 泄漏
     * 16 字节，栈指向 pa_exc_accept_value。
     *
     * 恢复 current_exc 到外层值再抛：语义上等价于 pa_exc_done（除了不动
     * pending_exc —— 它马上要重新抛出去）。 */
    pa_exc_drop_save();
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
    v->type = NULL;
    v->message = NULL;
    v->args = NULL;
    v->cause = NULL;
    /* 下面的 pa_list_new 走 rc_alloc，可能触发主收集；先把半成品挂成根。 */
    exc_construct_begin(v);
    size_t tn = strlen(type);
    size_t mn = strlen(msg);
    v->type = (char*)malloc(tn + 1);
    v->message = (char*)malloc(mn + 1);
    if (!v->type || !v->message) panic_c("out of memory");
    memcpy(v->type, type, tn + 1);
    memcpy(v->message, msg, mn + 1);
    v->args = pa_list_new(2, 1);
    exc_construct_end(v);
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
    exc_gc_roots_ensure();
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
    /* v 构造完了，但还没交回调用方；下面的 str_new / cause 递归都可能
       在 rc_alloc 里触发主收集，先把 v 挂成根。 */
    exc_construct_begin(v);
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
    exc_construct_end(v);
    return v;
}

PyAdamasException* pa_exc_from_value(PyAdamasExcValue* v) {
    exc_gc_roots_ensure();
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
    exc_gc_roots_ensure();
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
    if (getenv("PYADAMAS_ACCDBG"))
        fprintf(stderr, "[acc] %p\n", __builtin_return_address(0));
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

/* 进程退出时还掉异常子系统的全局内存。
 *
 * 三处都是**纯缓存/登记表**，不影响正确性（进程都要退出了），但会让
 * LeakSanitizer 报 Direct leak，淹没真正有价值的信号：
 *
 *   * handler_freelist —— pa_handler_push 的复用池；
 *   * suspended_head/pool —— 生成器挂起中的 handler 记录（0.219 引入）；
 *   * exc_regs —— 异常继承关系登记（pa_exc_register）；
 *   * exc_node_top —— 异常上下文嵌套节点。
 *
 * 顺序：先还挂起表里的记录（它们是从 freelist 之外单独 malloc 的），
 * 再还 freelist 本身，最后还登记表。 */
void pa_exc_shutdown(void) {
    // 挂起表里的 handler 记录：整条链还掉。
    SuspendedNode* sn = suspended_head;
    while (sn) {
        SuspendedNode* nx = sn->next;
        if (sn->h && sn->h->heap_alloc) {
            sn->h->prev = handler_freelist;
            handler_freelist = sn->h;
        }
        free(sn);
        sn = nx;
    }
    suspended_head = NULL;
    // 空闲节点池。
    sn = suspended_pool;
    while (sn) {
        SuspendedNode* nx = sn->next;
        free(sn);
        sn = nx;
    }
    suspended_pool = NULL;
    // **还挂在 handler_top 链上的记录**：进程退出时生成器可能还停在某个
    // 保护区里（`list(g_inner())` 消费到一半、生成器对象没销毁），那些
    // 记录既不在挂起表、也不在 freelist，是最后一块泄漏。
    // 它们都是堆分配的（栈记录的 heap_alloc=0，但栈记录不在这条链上 ——
    // 链上的都是 pa_handler_push 压的）。
    while (handler_top) {
        PyAdamasHandler* h = handler_top;
        handler_top = h->prev;
        if (h->heap_alloc) free(h);
    }
    // handler freelist 本体。
    while (handler_freelist) {
        PyAdamasHandler* h = handler_freelist;
        handler_freelist = h->prev;
        free(h);
    }
    // 异常继承登记表（child/parent 是 malloc 的字符串）。
    while (exc_regs) {
        ExcReg* r = exc_regs;
        exc_regs = r->next;
        free(r->child);
        free(r->parent);
        free(r);
    }
    // 异常上下文嵌套节点。每个节点还**挂着一条 exc_save_top 链**
    // （pa_exc_enter 把它从全局摘下来存进 n->top）。协程挂起后如果没恢复
    // （被丢弃、或程序直接退出），那条链上的 PyAdamasExcSave 就丢了 ——
    // 既不在 exc_save_top 上、也不在别处。
    // ASan 实测 32_exception_value 泄漏 16 字节（正好 sizeof(PyAdamasExcSave)）。
    while (exc_node_top) {
        PyAdamasExcNode* n = exc_node_top;
        exc_node_top = n->prev;
        while (n->top) {
            PyAdamasExcSave* s = n->top;
            n->top = s->up;
            free(s);
        }
        free(n);
    }
    // 全局那条链本身也要清（正常路径下它和节点链是互斥的两条）。
    while (exc_save_top) {
        PyAdamasExcSave* s = exc_save_top;
        exc_save_top = s->up;
        free(s);
    }
    exc_node_depth = 0;
}
