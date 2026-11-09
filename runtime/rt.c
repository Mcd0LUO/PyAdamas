/* rt.c —— py23 运行时
 *
 * 只放「生成代码不该内联展开」的东西：堆分配、格式化输出、越界检查。
 * 值统一用 int64_t 传递：整数就是整数，浮点是它的位模式，指针是地址。
 *
 * 字符串有两种来源：
 *   1. 字面量 —— 由 codegen 在 .data.rel.ro 里静态构造（len/hash/data 三字段），
 *      rc 头是 INT64_MIN，永不释放；
 *   2. 拼接/转换 —— 在堆上用 rc_alloc 分配。
 * 两者负载布局一致，所以运行时函数不需要区分。
 *
 * 内存管理：引用计数。每个堆对象前面有一个隐藏的 Py23Rc 头（rc + dtor），
 * 负载指针保持原布局不变。new 出来 rc = 1，所有者是产生它的表达式；
 * 表达式临时值进入临时值栈，生成代码在语句/循环/函数边界按 mark 清理。
 * 循环引用没有收集器，会泄漏；unknown 类型不参与计数（宁可泄漏）。
 */
#include <math.h>
#include <poll.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int64_t len;
    int64_t hash;      /* 预留给字符串驻留 */
    const char* data;
} Py23Str;

/* elem_kind: 0 = 全部非堆类型，1 = 全部堆类型，2 = 有 unknown / 混合。
   kind 1 的元素在写入时由生成代码 retain，销毁时由运行时 release；
   kind 2 只 retain 不 release（看不见的类型不敢 dec），代价是泄漏。 */
typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* data;
    int64_t elem_kind;
} Py23List;

/* 字典 = 开放寻址哈希表 + 插入顺序 order 数组。set 复用同一结构，
   val_own = 0、值恒为 0。len 在偏移 0，len() 和真值判断都只读它。 */
typedef struct {
    int64_t hash;  /* 0 表示空槽；真实 hash 强制非 0 */
    int64_t key;
    int64_t val;
} Py23DictEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    Py23DictEntry* entries;
    int64_t* order;    /* order[i] = 第 i 个插入项在 entries 里的槽号 */
    int64_t key_kind;  /* 0 int/bool, 1 str, 2 float, 3 raw */
    int64_t key_own;   /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;   /* 0 非堆, 1 堆, 2 unknown */
} Py23Dict;

/* ── 引用计数 ─────────────────────────────────────────────────── */

/* 隐藏头放在负载前面，负载指针（字符串的 len、对象的 vtable）保持原样。
   字面量在 .data.rel.ro 里静态构造，rc 是 INT64_MIN，inc/dec 直接跳过。

   头同时是循环收集器的注册表节点：
   - dtor：正常引用计数归零时调用（负责 dec 子对象、释放列表 data）；
   - release：GC 判断对象不可达时调用，只把「指向可达对象的引用」减掉，
     不释放任何东西（子对象由清扫阶段统一处理）；
   - size/kind：GC 保守扫描负载时要知道范围。 */
enum {
    PY23_RC_STR = 0,
    PY23_RC_LIST = 1,
    PY23_RC_OBJ = 2,
    PY23_RC_CLOSURE = 3,
    PY23_RC_DICT = 4,
};

typedef struct Py23Rc {
    struct Py23Rc* next;
    struct Py23Rc* prev;
    int64_t rc;
    void (*dtor)(void*);
    void (*release)(void*);
    int64_t size;
    int64_t kind;
    int64_t marked;
} Py23Rc;

#define PY23_RC_IMMORTAL INT64_MIN
#define PY23_RC_HDR ((int64_t)sizeof(Py23Rc))
_Static_assert(sizeof(Py23Rc) == 64, "codegen literal headers must match Py23Rc size");

#define PY23_GC_THRESHOLD (2 * 1024 * 1024)

static Py23Rc* heap_head;
static int64_t alloc_since_gc;
static int gc_ready;
static int gc_sweeping;

static void panic_c(const char* msg);

/* 存活堆对象索引：unknown 值的静态类型看不见，不能直接 rc_inc（可能
   是整数），先按 payload 查注册表；查到才说明它真是堆对象。索引只用于
   这条保守 retain 路径，开放寻址 + 墓碑。 */
static Py23Rc** rc_index;
static size_t rc_index_cap;
static size_t rc_index_fill;  // 非空槽（存活 + 墓碑）
static size_t rc_index_live;  // 存活对象数
static Py23Rc* const RC_TOMB = (Py23Rc*)(intptr_t)-1;

static size_t rc_index_slot(Py23Rc* h) {
    uint64_t x = (uint64_t)(uintptr_t)h >> 4;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    return (size_t)x & (rc_index_cap - 1);
}

static void rc_index_rehash(size_t ncap) {
    Py23Rc** old = rc_index;
    size_t ocap = rc_index_cap;
    rc_index = (Py23Rc**)calloc(ncap, sizeof(Py23Rc*));
    if (!rc_index) panic_c("out of memory");
    rc_index_cap = ncap;
    rc_index_fill = 0;
    for (size_t i = 0; i < ocap; ++i) {
        Py23Rc* h = old[i];
        if (!h || h == RC_TOMB) continue;
        size_t s = rc_index_slot(h);
        while (rc_index[s]) s = (s + 1) & (ncap - 1);
        rc_index[s] = h;
        rc_index_fill++;
    }
    free(old);
}

static void rc_index_put(Py23Rc* h) {
    if (rc_index_cap == 0) {
        rc_index_rehash(64);
    } else if ((rc_index_fill + 1) * 10 >= rc_index_cap * 7) {
        // 存活对象少、墓碑多时原地压缩，否则翻倍。
        size_t ncap = (rc_index_live + 1) * 10 >= rc_index_cap * 4
                          ? rc_index_cap * 2
                          : rc_index_cap;
        rc_index_rehash(ncap);
    }
    size_t s = rc_index_slot(h);
    size_t tomb = (size_t)-1;
    while (rc_index[s]) {
        if (rc_index[s] == RC_TOMB && tomb == (size_t)-1) tomb = s;
        s = (s + 1) & (rc_index_cap - 1);
    }
    if (tomb != (size_t)-1) {
        rc_index[tomb] = h;  // 复用墓碑，不增加 fill
    } else {
        rc_index[s] = h;
        rc_index_fill++;
    }
    rc_index_live++;
}

static void rc_index_del(Py23Rc* h) {
    if (rc_index_cap == 0) return;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) {
            rc_index[s] = RC_TOMB;
            rc_index_live--;
            return;
        }
        s = (s + 1) & (rc_index_cap - 1);
    }
}

static int rc_index_has(Py23Rc* h) {
    if (rc_index_cap == 0) return 0;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) return 1;
        s = (s + 1) & (rc_index_cap - 1);
    }
    return 0;
}

/* 对 unknown 值做保守 retain：不是堆指针就什么也不做。查不到的对象
   包括静态字面量（永生，不需要 inc）和整数。 */
int64_t py23_rc_inc_unknown(int64_t v) {
    if (!v) return 0;
    Py23Rc* h = (Py23Rc*)(uintptr_t)((uint64_t)v - (uint64_t)PY23_RC_HDR);
    if (!rc_index_has(h)) return 0;
    if (h->rc == PY23_RC_IMMORTAL) return 1;
    if (gc_sweeping) {
        if (h->marked) h->rc++;
        return 1;
    }
    if (h->rc <= 0) return 1;
    h->rc++;
    return 1;
}

void py23_rc_dec(void* p);

/* unknown 值的保守释放：先按注册表确认是堆指针，再走正常 dec。
   用于「结果类型静态不可知」的跨 await/gather 值。 */
static void py23_rc_dec_unknown(int64_t v) {
    if (!v) return;
    Py23Rc* h = (Py23Rc*)(uintptr_t)((uint64_t)v - (uint64_t)PY23_RC_HDR);
    if (!rc_index_has(h)) return;
    if (h->rc == PY23_RC_IMMORTAL) return;
    py23_rc_dec((void*)(uintptr_t)v);
}

void py23_gc_collect(void);
void py23_gc_shutdown(void);
void py23_gc_unref_child(void* p);
void py23_gc_add_roots(void** roots, int64_t n_roots);
static void dict_gc_release(void* p);

Py23List* py23_list_new(int64_t cap, int64_t elem_kind);
void py23_list_append(Py23List* l, int64_t v);

static void panic_c(const char* msg) {
    fprintf(stderr, "py23: runtime error: %s\n", msg);
    exit(1);
}

static void rc_link(Py23Rc* h) {
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
    rc_index_put(h);
}

static void rc_unlink(Py23Rc* h) {
    if (h->prev) h->prev->next = h->next;
    else heap_head = h->next;
    if (h->next) h->next->prev = h->prev;
    rc_index_del(h);
}

static void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind) {
    if (size < 0) panic_c("negative allocation size");
    if (gc_ready && !gc_sweeping && heap_head &&
        alloc_since_gc >= PY23_GC_THRESHOLD)
        py23_gc_collect();
    Py23Rc* h = (Py23Rc*)malloc(sizeof(Py23Rc) + (size_t)size);
    if (!h) panic_c("out of memory");
    h->rc = 1;
    h->dtor = dtor;
    h->release = release;
    h->size = size;
    h->kind = kind;
    h->marked = 0;
    rc_link(h);
    alloc_since_gc += size + PY23_RC_HDR;
    void* p = (char*)h + sizeof(Py23Rc);
    memset(p, 0, (size_t)size);
    return p;
}

void py23_rc_inc(void* p) {
    if (!p) return;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (h->rc == PY23_RC_IMMORTAL) return;
    if (h->rc <= 0) panic_c("refcount increment on a freed object");
    h->rc++;
}

void py23_rc_dec(void* p) {
    if (!p) return;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (h->rc == PY23_RC_IMMORTAL) return;
    if (gc_sweeping) {
        // 清扫阶段只调整可达对象的计数，绝不在 GC 内部触发释放。
        if (h->marked && h->rc > 0) h->rc--;
        return;
    }
    if (h->rc <= 0) panic_c("refcount decrement on a freed object");
    if (--h->rc == 0) {
        rc_unlink(h);
        if (h->dtor) h->dtor(p);
        free(h);
    }
}

/* 表达式临时值栈：每个新堆值在产生时带着 rc=1 入栈，语句/循环/函数
   边界按 mark 统一释放。这样不需要做活跃性分析，也不会漏掉某条路径。
   入栈不增加计数——栈接管的是「产生者」本来就持有的那一个引用。 */
static void** temp_stack;
static int64_t temp_len;
static int64_t temp_cap;

int64_t py23_rc_mark(void) { return temp_len; }

void py23_rc_temp(void* p) {
    if (!p) return;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (h->rc == PY23_RC_IMMORTAL) return;
    if (temp_len == temp_cap) {
        int64_t ncap = temp_cap ? temp_cap * 2 : 64;
        void** ns = (void**)realloc(temp_stack, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        temp_stack = ns;
        temp_cap = ncap;
    }
    temp_stack[temp_len++] = p;
}

void py23_rc_cleanup(int64_t mark) {
    if (mark < 0 || mark > temp_len) panic_c("invalid temporary stack mark");
    while (temp_len > mark) py23_rc_dec(temp_stack[--temp_len]);
}

/* 程序退出前由入口函数调用：临时栈此时已经清空，把栈本身的存储还掉。 */
void py23_rc_shutdown(void) {
    free(temp_stack);
    temp_stack = NULL;
    temp_len = 0;
    temp_cap = 0;
}

/* ── 闭包 ─────────────────────────────────────────────────────── */

/* 闭包负载 = [code, n_caps, cell0, cell1…]。
   code 是静态代码地址（不是堆指针）；cell 是单元素 list，闭包持有引用。 */
static void closure_destroy(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) py23_rc_dec((void*)c[2 + i]);
}

static void closure_gc_release(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) py23_gc_unref_child((void*)c[2 + i]);
}

void* py23_closure_new(int64_t code, int64_t n_caps) {
    if (n_caps < 0) panic_c("negative closure capture count");
    void* p = rc_alloc(16 + 8 * n_caps, closure_destroy, closure_gc_release,
                       PY23_RC_CLOSURE);
    ((int64_t*)p)[0] = code;
    ((int64_t*)p)[1] = n_caps;
    return p;
}

static Py23Str* str_alloc(int64_t len) {
    if (len < 0) panic_c("negative string length");
    Py23Str* s =
        (Py23Str*)rc_alloc((int64_t)sizeof(Py23Str) + len + 1, NULL, NULL, PY23_RC_STR);
    s->len = len;
    s->hash = 0;
    char* p = (char*)(s + 1);
    p[len] = '\0';
    s->data = p;
    return s;
}

static Py23Str* str_new(const char* p, int64_t len) {
    Py23Str* s = str_alloc(len);
    memcpy((char*)s->data, p, (size_t)len);
    return s;
}

/* ── 异常 ─────────────────────────────────────────────────────── */

/* 展开机制：try/with/finally 在运行时压一条 handler 记录，raise 设置
   全局异常后 longjmp 到最内层 setjmp 点。记录本身用 malloc，不参与
   RC/GC；跳过的普通函数帧里的局部引用由保守 GC 回收。 */
typedef struct Py23Handler {
    jmp_buf env;  /* 必须放在开头：指针同时作为 setjmp 的参数 */
    struct Py23Handler* prev;
    int64_t mark;
} Py23Handler;

typedef struct Py23Exception {
    char* type;     /* 内置异常名，如 "ValueError" */
    char* message;  /* 可以为空串，不能为 NULL */
} Py23Exception;

typedef struct Py23ExcSave {
    Py23Exception* prev;
    struct Py23ExcSave* up;
} Py23ExcSave;

static Py23Handler* handler_top;
static Py23Exception* pending_exc;
static Py23Exception* current_exc;
static Py23ExcSave* exc_save_top;

static void exc_free(Py23Exception* e) {
    if (!e) return;
    free(e->type);
    free(e->message);
    free(e);
}

static Py23Exception* exc_new(const char* type, const char* msg) {
    Py23Exception* e = (Py23Exception*)malloc(sizeof(Py23Exception));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc(strlen(type) + 1);
    e->message = (char*)malloc(strlen(msg) + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    strcpy(e->type, type);
    strcpy(e->message, msg);
    return e;
}

static Py23Exception* exc_clone(Py23Exception* e) {
    if (!e) return NULL;
    return exc_new(e->type, e->message);
}

static const char* exc_parent(const char* name) {
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

static int exc_is_a(const char* child, const char* want) {
    while (child) {
        if (strcmp(child, want) == 0) return 1;
        child = exc_parent(child);
    }
    return 0;
}

static void exc_print_uncaught(Py23Exception* e) {
    fflush(stdout);
    if (e->message[0])
        fprintf(stderr, "py23: uncaught %s: %s\n", e->type, e->message);
    else
        fprintf(stderr, "py23: uncaught %s\n", e->type);
}

static void exc_longjmp(void) {
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

/* ── 生成器 ───────────────────────────────────────────────────── */

/* 帧负载：
   [state][status][sent][out][resume][closing][kind][waiter][exc][cur][slot..]
   resume 是状态机入口（void (*)(void*, int64_t)），由生成代码按 state
   分发；槽位属于帧，所以 yield/await 只需要返回，不需要保存栈。
   status: 0 未开始/挂起，1 已结束，2 异常逃出，3 awaiting，4 已排队。
   kind: 0 生成器，PY23_CORO_MAGIC 协程；waiter 是等待本对象的协程/
   gather/task；exc 是待注入的异常；cur 是协程当前等待的 awaitable
   （借用的观察指针，取消时靠它解绑）。 */
#define PY23_GEN_HEADER 10
#define PY23_CORO_MAGIC 0x434f524f00000001LL
#define PY23_AGEN_MAGIC 0x4147454e00000001LL
#define PY23_AWAIT_MAGIC 0x4157414954000001LL

void py23_gen_finalize(void* g);

static void gen_destroy(void* g) {
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

static int64_t gen_run(void* g, int64_t sent, int* done) {
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

/* ── 协程与事件循环 ───────────────────────────────────────────── */

/* 运行时 awaitable：sleep / gather。magic 用来和协程帧区分。
   waiter 持有协程或上层 gather 的一个引用；完成时连同结果一起转交。 */
typedef struct {
    int64_t magic;
    int64_t kind;      /* 1 sleep, 2 gather, 3 task */
    int64_t done;
    int64_t starting;  /* gather 正在启动子任务：完成延后到启动结束 */
    int64_t result;    /* owned */
    int64_t exc;       /* Py23Exception* */
    int64_t waiter;    /* owned ref */
    int64_t deadline_ms;
    int64_t remaining;
    int64_t task_coro; /* task：被包装的协程（owned） */
    int64_t cancelled;
    int64_t driver_gen; /* kind 4：被驱动的异步生成器（owned） */
    int64_t pending;    /* future/fd：已经挂进 loop_pending/loop_fds */
    int64_t fd;         /* kind 6：等待的 fd */
    int64_t write;      /* kind 6：0 读 / 1 写 */
    Py23List* waiters;  /* kind 5 future：多个等待者（owned refs） */
    Py23List* results;
    Py23List* children;
} Py23Await;

static Py23List* loop_ready;
static Py23List* loop_timers;
static Py23List* loop_pending;  /* 已启动但还没完成的 gather / Task / Future / fd */
static Py23List* loop_fds;
static void* loop_roots[4];
static int loop_roots_registered;
static int loop_running;
static void* loop_root;
static int loop_root_state;  /* 0 pending, 1 done, 2 failed */
static int64_t loop_root_result;
static Py23Exception* loop_root_exc;

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int frame_is_coro(void* p) {
    if (!p) return 0;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)PY23_GEN_HEADER * 8) return 0;
    return ((int64_t*)p)[6] == PY23_CORO_MAGIC;
}

static int frame_is_agen(void* p) {
    if (!p) return 0;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)PY23_GEN_HEADER * 8) return 0;
    return ((int64_t*)p)[6] == PY23_AGEN_MAGIC;
}

static int is_await_obj(void* p) {
    if (!p) return 0;
    Py23Rc* h = (Py23Rc*)p - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)sizeof(Py23Await)) return 0;
    return *(int64_t*)p == PY23_AWAIT_MAGIC;
}

static void await_destroy(void* p) {
    Py23Await* w = (Py23Await*)p;
    py23_rc_dec_unknown(w->result);
    if (w->exc) exc_free((Py23Exception*)w->exc);
    if (w->waiter) py23_rc_dec((void*)w->waiter);
    if (w->task_coro) py23_rc_dec((void*)w->task_coro);
    if (w->driver_gen) py23_rc_dec((void*)w->driver_gen);
    if (w->waiters) py23_rc_dec(w->waiters);
    if (w->results) py23_rc_dec(w->results);
    if (w->children) py23_rc_dec(w->children);
}

static void await_gc_release(void* p) {
    Py23Await* w = (Py23Await*)p;
    if (w->result) py23_gc_unref_child((void*)w->result);
    if (w->waiter) py23_gc_unref_child((void*)w->waiter);
    if (w->task_coro) py23_gc_unref_child((void*)w->task_coro);
    if (w->driver_gen) py23_gc_unref_child((void*)w->driver_gen);
    if (w->waiters) py23_gc_unref_child(w->waiters);
    if (w->results) py23_gc_unref_child(w->results);
    if (w->children) py23_gc_unref_child(w->children);
}

/* GC 清扫 / 进程退出不跑析构器，但异常对象是 malloc 的普通内存，
   必须在这里单独回收，否则挂在帧或 Task/Future 上的异常会泄漏。 */
static void free_attached_exception(void* payload, int64_t size) {
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

static Py23Await* await_new(int64_t kind) {
    Py23Await* w = (Py23Await*)rc_alloc((int64_t)sizeof(Py23Await), await_destroy,
                                        await_gc_release, PY23_RC_OBJ);
    w->magic = PY23_AWAIT_MAGIC;
    w->kind = kind;
    return w;
}

static void loop_ensure(void) {
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

static void ready_push(void* c) { py23_list_append(loop_ready, (int64_t)c); }

static void* ready_pop(void) {
    if (!loop_ready || loop_ready->len == 0) return NULL;
    void* c = (void*)loop_ready->data[0];
    memmove(&loop_ready->data[0], &loop_ready->data[1],
            sizeof(int64_t) * (size_t)(loop_ready->len - 1));
    loop_ready->len--;
    return c;
}

/* 从表里摘下第 i 项，引用交给调用方。 */
static int64_t list_take_at(Py23List* l, int64_t i) {
    int64_t v = l->data[i];
    memmove(&l->data[i], &l->data[i + 1], sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
    return v;
}

static void list_clear_owned(Py23List* l) {
    if (!l) return;
    int64_t n = l->len;
    l->len = 0;
    for (int64_t i = 0; i < n; ++i) py23_rc_dec((void*)l->data[i]);
}

static void* pending_take(void* w) {
    if (!loop_pending) return NULL;
    for (int64_t i = 0; i < loop_pending->len; ++i)
        if (loop_pending->data[i] == (int64_t)w) {
            list_take_at(loop_pending, i);
            return w;
        }
    return NULL;
}

static void deliver_owned(void* waiter, void* child, int64_t value, Py23Exception* exc);
static void start_awaitable(void* w, void* waiter);
static void finish_gather(Py23Await* g);
static void coro_inject(void* c, Py23Exception* e);
static void agen_drive(Py23Await* d);
static void cancel_chain(void* w);
static void cancel_suspended(void* c, Py23Exception* e);
static void future_cancel(Py23Await* f);
static void gather_cancel(Py23Await* g);

/* 完成一个 gather：把结果/异常交给它的 waiter，并释放 pending 里的持有引用。 */
static void finish_gather(Py23Await* g) {
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

static void gather_child_fail(Py23Await* g, Py23Exception* e) {
    if (!g->exc) g->exc = (int64_t)e;
    else exc_free(e);
    if (g->remaining > 0) --g->remaining;
}

/* 启动一个 awaitable。调用方把 w 的一个引用交给本函数。 */
static void start_gather(Py23Await* w, void* waiter) {
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

static void start_awaitable(void* obj, void* waiter) {
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
static void deliver_owned(void* waiter, void* child, int64_t value, Py23Exception* exc) {
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

static void coro_inject(void* c, Py23Exception* e) {
    int64_t* p = (int64_t*)c;
    if (p[8]) exc_free(e);
    else p[8] = (int64_t)e;
    p[1] = 4;
    py23_rc_inc(c);
    ready_push(c);
}

static void coro_finish(void* c, int64_t value, Py23Exception* exc) {
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
static void await_from_coro(void* c, void* w) {
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

static void coro_step(void* c) {
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
static int fire_due_timers(int64_t now) {
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

static int64_t earliest_deadline(void) {
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
static void detach_cur(void* c) {
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
static void cancel_suspended(void* c, Py23Exception* e) {
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

static void gather_cancel(Py23Await* g) {
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

static void future_cancel(Py23Await* f) {
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
}

/* sleep/fd 这类没有内部链的等待对象：摘表、标记完成、通知等待者。 */
static void simple_wait_cancel(Py23Await* w) {
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
static void cancel_chain(void* w) {
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

static void future_settle(Py23Await* f, int64_t value, Py23Exception* exc) {
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
}

void* py23_asyncio_future_new(void) {
    loop_ensure();
    Py23Await* f = await_new(5);
    f->waiters = py23_list_new(4, 1);
    return f;
}

void py23_future_set_result(void* fp, int64_t value) {
    Py23Await* f = (Py23Await*)fp;
    if (!is_await_obj(fp) || f->kind != 5)
        py23_raise_c("TypeError", "set_result() expects a Future");
    if (f->done) py23_raise_c("RuntimeError", "future is already done");
    future_settle(f, value, NULL);
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

static void raise_exception_copy(Py23Exception* e, const char* fallback) {
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

/* ── fd 等待与 poll 轮询 ─────────────────────────────────────── */

void* py23_asyncio_wait_fd(int64_t fd, int64_t write) {
    loop_ensure();
    Py23Await* w = await_new(6);
    w->fd = fd;
    w->write = write;
    return w;
}

/* 对 loop_fds 里注册的 fd 做一次 poll。返回是否完成了等待对象。 */
static int poll_fds(int timeout_ms) {
    if (!loop_fds->len) return 0;
    int64_t n = loop_fds->len;
    struct pollfd* pfds = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)n);
    int64_t* map = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (!pfds || !map) panic_c("out of memory");
    for (int64_t i = 0; i < n; ++i) {
        Py23Await* w = (Py23Await*)loop_fds->data[i];
        pfds[i].fd = (int)w->fd;
        pfds[i].events = (short)(w->write ? POLLOUT : POLLIN);
        pfds[i].revents = 0;
        map[i] = (int64_t)w;
    }
    int rc = poll(pfds, (nfds_t)n, timeout_ms);
    int fired = 0;
    if (rc > 0) {
        for (int64_t i = 0; i < n; ++i) {
            if (!pfds[i].revents) continue;
            int64_t owned = 0;
            for (int64_t j = 0; j < loop_fds->len; ++j)
                if (loop_fds->data[j] == map[i]) {
                    owned = list_take_at(loop_fds, j);
                    break;
                }
            if (!owned) continue;
            Py23Await* w = (Py23Await*)owned;
            w->done = 1;
            Py23Exception* e = NULL;
            if (pfds[i].revents & (POLLERR | POLLNVAL))
                e = exc_new("OSError", "fd poll error");
            void* a = (void*)w->waiter;
            w->waiter = 0;
            if (a) deliver_owned(a, w, 0, e);
            else {
                w->result = 0;
                w->exc = (int64_t)e;
            }
            py23_rc_dec(w);  // 轮询表的引用
            fired++;
        }
    }
    free(pfds);
    free(map);
    return fired;
}

/* ── 异步生成器（async for 的驱动）────────────────────────────── */

void* py23_asyncgen_anext(void* gen) {
    if (!frame_is_agen(gen))
        py23_raise_c("TypeError", "__anext__() expects an async generator");
    loop_ensure();
    Py23Await* d = await_new(4);
    d->driver_gen = (int64_t)gen;
    py23_rc_inc(gen);
    return d;
}

static void driver_complete(Py23Await* d, int64_t value, Py23Exception* exc) {
    if (d->done) {
        if (exc) exc_free(exc);
        else py23_rc_dec_unknown(value);
        return;
    }
    d->done = 1;
    void* owned = pending_take(d);
    if (d->driver_gen) {
        py23_rc_dec((void*)d->driver_gen);
        d->driver_gen = 0;
    }
    void* a = (void*)d->waiter;
    d->waiter = 0;
    if (a) deliver_owned(a, d, value, exc);
    else {
        d->result = value;
        d->exc = (int64_t)exc;
    }
    if (owned) py23_rc_dec(owned);
}

static void agen_drive(Py23Await* d) {
    if (!d || d->done) return;
    void* g = (void*)d->driver_gen;
    if (!g) {
        driver_complete(d, 0, exc_new("RuntimeError", "anext driver has no generator"));
        return;
    }
    int64_t* p = (int64_t*)g;
    if (p[1] == 1) {
        driver_complete(d, 0, exc_new("StopAsyncIteration", ""));
        return;
    }
    if (p[1] == 2) {
        Py23Exception* e = (Py23Exception*)p[8];
        p[8] = 0;
        driver_complete(d, 0, e ? e : exc_new("RuntimeError", "async generator failed"));
        return;
    }
    if (p[1] == 3) {
        driver_complete(d, 0, exc_new("RuntimeError", "async generator is already running"));
        return;
    }

    p[1] = 0;
    p[5] = 0;
    p[9] = 0;
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
        driver_complete(d, 0, e);
        return;
    }
    ((void (*)(void*, int64_t))(uintptr_t)p[4])(g, 0);
    handler_top = h.prev;
    py23_rc_cleanup(h.mark);
    p[5] = 0;
    p[2] = 0;
    int64_t status = p[1];
    if (status == 3) {
        // 生成器在等内部 awaitable：把驱动对象挂上去。
        void* w = (void*)p[3];
        p[3] = 0;
        p[9] = (int64_t)w;
        p[1] = 3;  // 标记：正在等内部对象，禁止并发 anext
        if (frame_is_coro(w)) {
            int64_t* q = (int64_t*)w;
            if (q[1] != 0) {
                py23_rc_dec(w);
                driver_complete(d, 0,
                                exc_new("RuntimeError", "coroutine was already awaited"));
                return;
            }
            q[7] = (int64_t)d;
            py23_rc_inc(d);
            q[1] = 4;
            ready_push(w);
        } else if (is_await_obj(w)) {
            start_awaitable(w, d);
        } else {
            driver_complete(d, 0, exc_new("TypeError", "object is not awaitable"));
            return;
        }
        return;
    }
    if (status == 1) {
        driver_complete(d, 0, exc_new("StopAsyncIteration", ""));
        return;
    }
    if (status == 2) {
        Py23Exception* e = (Py23Exception*)p[8];
        p[8] = 0;
        driver_complete(d, 0, e);
        return;
    }
    // status 0：产出了一个值
    {
        int64_t v = p[3];
        p[3] = 0;
        driver_complete(d, v, NULL);
    }
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
static void format_double(char* buf, size_t n, double v) {
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

/* ── 字符串 ───────────────────────────────────────────────────── */

Py23Str* py23_str_concat(Py23Str* a, Py23Str* b) {
    Py23Str* s = str_alloc(a->len + b->len);
    char* p = (char*)s->data;
    memcpy(p, a->data, (size_t)a->len);
    memcpy(p + a->len, b->data, (size_t)b->len);
    return s;
}

Py23Str* py23_str_char_at(Py23Str* s, int64_t i) {
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) py23_raise_c("IndexError", "string index out of range");
    return str_new(s->data + i, 1);
}

Py23Str* py23_str_from_i64(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return str_new(buf, n);
}

Py23Str* py23_str_from_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    return str_new(buf, (int64_t)strlen(buf));
}

Py23Str* py23_str_from_bool(int64_t v) {
    return v ? str_new("True", 4) : str_new("False", 5);
}

Py23Str* py23_str_from_none(void) { return str_new("None", 4); }

int64_t py23_str_eq(Py23Str* a, Py23Str* b) {
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, (size_t)a->len) == 0;
}

/* 三路比较，供 `<` / `<=` / `>` / `>=` 用。
   UTF-8 的字节序与码点序一致，所以 memcmp 的结果就是 Python 的结果。 */
int64_t py23_str_cmp(Py23Str* a, Py23Str* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, (size_t)n);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

/* ── 列表 ─────────────────────────────────────────────────────── */

static void list_destroy(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind == 1)
        for (int64_t i = 0; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    free(l->data);
}

// GC 清扫前调用：只把「指向可达对象」的引用计数减掉。kind 2 的元素类型
// 不可知，跳过（那些对象如果只被这个容器引用，会在同一轮清扫里被释放）。
static void list_gc_release(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind != 1 || !l->data) return;
    for (int64_t i = 0; i < l->len; ++i) py23_gc_unref_child((void*)l->data[i]);
}

Py23List* py23_list_new(int64_t cap, int64_t elem_kind) {
    if (cap < 0) panic_c("negative list capacity");
    if (elem_kind < 0 || elem_kind > 2) panic_c("invalid list element kind");
    if (cap < 4) cap = 4;
    Py23List* l = (Py23List*)rc_alloc((int64_t)sizeof(Py23List), list_destroy,
                                      list_gc_release, PY23_RC_LIST);
    l->len = 0;
    l->cap = cap;
    l->elem_kind = elem_kind;
    l->data = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!l->data) panic_c("out of memory");
    return l;
}

/* 元素引用由生成代码在写入前 retain（kind 1/2 且源类型已知是堆），
   运行时不再重复 inc；kind 1 覆盖/截断/销毁时负责 release。 */
void py23_list_append(Py23List* l, int64_t v) {
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        int64_t* nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->data = nd;
        l->cap = ncap;
    }
    l->data[l->len++] = v;
}

int64_t py23_list_get(Py23List* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list index out of range");
    return l->data[i];
}

void py23_list_set(Py23List* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list assignment index out of range");
    int64_t old = l->data[i];
    l->data[i] = v;
    if (l->elem_kind == 1) py23_rc_dec((void*)old);
}

void py23_list_set_len(Py23List* l, int64_t n) {
    if (n < 0 || n > l->len) panic_c("invalid list length");
    if (l->elem_kind == 1)
        for (int64_t i = n; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    l->len = n;
}

/* 把 src 的元素接到 dst 末尾，供 *args 打包和切片拷贝使用。
   元素的引用按 dst 的 elem_kind 处理：kind 1 正常 inc；kind 2 只对
   已知是堆的源元素 inc（不 release，交给 GC）。 */
void py23_list_extend(Py23List* dst, Py23List* src) {
    int64_t slen = src->len;
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    if (dst == src) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        if (v && src->elem_kind == 1 &&
            (dst->elem_kind == 1 || dst->elem_kind == 2))
            py23_rc_inc((void*)v);
        py23_list_append(dst, v);
    }
    free(snap);
}

/* ── 字典 / 集合 ─────────────────────────────────────────────── */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static int64_t dict_hash_value(int64_t kind, int64_t v) {
    uint64_t h;
    if (kind == 1) {
        Py23Str* s = (Py23Str*)v;
        h = 1469598103934665603ull;
        for (int64_t i = 0; i < s->len; ++i) {
            h ^= (unsigned char)s->data[i];
            h *= 1099511628211ull;
        }
    } else if (kind == 2) {
        double d;
        memcpy(&d, &v, sizeof d);
        if (d == 0.0) v = 0;  /* 0.0 与 -0.0 同槽 */
        h = mix64((uint64_t)v);
    } else {
        h = mix64((uint64_t)v);
    }
    h = mix64(h);
    return (int64_t)(h ? h : 1);
}

static int dict_key_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return py23_str_eq((Py23Str*)a, (Py23Str*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

static int64_t dict_probe(Py23DictEntry* entries, int64_t cap, int64_t kind,
                          int64_t key, int64_t hash) {
    int64_t mask = cap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (entries[i].hash != 0) {
        if (entries[i].hash == hash && dict_key_eq(kind, entries[i].key, key)) return i;
        i = (i + 1) & mask;
    }
    return -1;
}

static int64_t dict_empty_slot(Py23DictEntry* entries, int64_t cap, int64_t hash) {
    int64_t mask = cap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (entries[i].hash != 0) i = (i + 1) & mask;
    return i;
}

static void dict_resize(Py23Dict* d, int64_t ncap) {
    Py23DictEntry* ne = (Py23DictEntry*)calloc((size_t)ncap, sizeof(Py23DictEntry));
    int64_t* no = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
    if (!ne || !no) panic_c("out of memory");
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        int64_t mask = ncap - 1;
        int64_t slot = (int64_t)((uint64_t)e.hash & (uint64_t)mask);
        while (ne[slot].hash != 0) slot = (slot + 1) & mask;
        ne[slot] = e;
        no[i] = slot;
    }
    free(d->entries);
    free(d->order);
    d->entries = ne;
    d->order = no;
    d->cap = ncap;
}

static void dict_destroy(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    free(d->entries);
    free(d->order);
}

static void dict_gc_release(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_gc_unref_child((void*)e.key);
        if (d->val_own == 1) py23_gc_unref_child((void*)e.val);
    }
}

Py23Dict* py23_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own) {
    if (key_kind < 0 || key_kind > 3) panic_c("invalid dict key kind");
    if (key_own < 0 || key_own > 2 || val_own < 0 || val_own > 2)
        panic_c("invalid dict ownership kind");
    int64_t cap = 8;
    Py23Dict* d =
        (Py23Dict*)rc_alloc((int64_t)sizeof(Py23Dict), dict_destroy, dict_gc_release,
                            PY23_RC_DICT);
    d->len = 0;
    d->cap = cap;
    d->entries = (Py23DictEntry*)calloc((size_t)cap, sizeof(Py23DictEntry));
    d->order = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!d->entries || !d->order) panic_c("out of memory");
    d->key_kind = key_kind;
    d->key_own = key_own;
    d->val_own = val_own;
    return d;
}

/* 调用方已经把 key/value 的「入表引用」retain 好；更新已有 key 时
   释放这份多余的新 key 引用，字典继续保留原来的 key 对象。 */
void py23_dict_set(Py23Dict* d, int64_t key, int64_t val) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot >= 0) {
        if (d->key_own == 1) py23_rc_dec((void*)key);
        if (d->val_own == 1) py23_rc_dec((void*)d->entries[slot].val);
        d->entries[slot].val = val;
        return;
    }
    if ((d->len + 1) * 10 >= d->cap * 7) dict_resize(d, d->cap * 2);
    slot = dict_empty_slot(d->entries, d->cap, h);
    d->entries[slot].hash = h;
    d->entries[slot].key = key;
    d->entries[slot].val = val;
    d->order[d->len++] = slot;
}

int64_t py23_dict_get(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot < 0) py23_raise_c("KeyError", "key not found");
    return d->entries[slot].val;
}

int64_t py23_dict_contains(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_value(d->key_kind, key);
    return dict_probe(d->entries, d->cap, d->key_kind, key, h) >= 0 ? 1 : 0;
}

int64_t py23_dict_key_at(Py23Dict* d, int64_t i) {
    if (i < 0 || i >= d->len) panic_c("dict iteration index out of range");
    return d->entries[d->order[i]].key;
}

int64_t py23_dict_get_or(Py23Dict* d, int64_t key, int64_t def) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    return slot >= 0 ? d->entries[slot].val : def;
}

static int own_to_list_kind(int64_t own) {
    return own == 1 ? 1 : (own == 0 ? 0 : 2);
}

Py23List* py23_dict_keys(Py23Dict* d) {
    int kind = own_to_list_kind(d->key_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[d->order[i]].key;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_values(Py23Dict* d) {
    int kind = own_to_list_kind(d->val_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[d->order[i]].val;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_items(Py23Dict* d) {
    // 单元素 kind 无法同时表达 key/value 两类所有权：都为堆用 1，
    // 都非堆用 0，混合用 2（已知堆的元素 retain，销毁时不解引用，靠 GC）。
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* out = py23_list_new(d->len, 1);
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        Py23List* tup = py23_list_new(2, tk);
        if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
        py23_list_append(tup, e.key);
        if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
        py23_list_append(tup, e.val);
        py23_list_append(out, (int64_t)tup);  // tuple 的初始引用交给外层 list
    }
    return out;
}

void py23_dict_update(Py23Dict* dst, Py23Dict* src) {
    if (dst == src) return;
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[src->order[i]];
        // 目标类型未知时也要保证源销毁后指针不悬空：源自己是堆所有权
        // 就 retain 一份。目标 own=2/0 不会 dec，这份引用会留给 GC。
        if ((dst->key_own == 1 || src->key_own == 1) && e.key)
            py23_rc_inc((void*)e.key);
        if ((dst->val_own == 1 || src->val_own == 1) && e.val)
            py23_rc_inc((void*)e.val);
        py23_dict_set(dst, e.key, e.val);
    }
}

/* 删除用「移出 order + 全表重建」：线性探查没有墓碑，直接挖空会截断
   后续 key 的探查链。重建是 O(n)，删除本来也不在热路径上。 */
static void dict_rebuild(Py23Dict* d) {
    Py23DictEntry* old = d->entries;
    Py23DictEntry* ne = (Py23DictEntry*)calloc((size_t)d->cap, sizeof(Py23DictEntry));
    if (!ne) panic_c("out of memory");
    int64_t mask = d->cap - 1;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = old[d->order[i]];
        int64_t slot = (int64_t)((uint64_t)e.hash & (uint64_t)mask);
        while (ne[slot].hash != 0) slot = (slot + 1) & mask;
        ne[slot] = e;
        d->order[i] = slot;
    }
    free(old);
    d->entries = ne;
}

/* 找到并移除 key；返回 1 时 val_out 是原值。字典对 key/val 的引用：
   key 立即释放（调用方传的是借用），value 的所有权转交给调用方
   （pop 返回给 Python 层，由临时栈接管）。 */
static int dict_take(Py23Dict* d, int64_t key, int64_t* val_out) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot < 0) return 0;
    *val_out = d->entries[slot].val;
    int64_t oi = -1;
    for (int64_t i = 0; i < d->len; ++i)
        if (d->order[i] == slot) {
            oi = i;
            break;
        }
    if (oi < 0) panic_c("dict order corrupted");
    if (d->key_own == 1) py23_rc_dec((void*)d->entries[slot].key);
    for (int64_t i = oi; i + 1 < d->len; ++i) d->order[i] = d->order[i + 1];
    --d->len;
    dict_rebuild(d);
    return 1;
}

int64_t py23_dict_pop(Py23Dict* d, int64_t key, int64_t has_default, int64_t def) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (has_default) return def;
        py23_raise_c("KeyError", "key not found");
    }
    return v;
}

Py23List* py23_dict_popitem(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "popitem(): dictionary is empty");
    int64_t slot = d->order[d->len - 1];
    Py23DictEntry e = d->entries[slot];
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* tup = py23_list_new(2, tk);
    if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
    py23_list_append(tup, e.key);
    if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
    py23_list_append(tup, e.val);
    d->len--;
    if (d->key_own == 1) py23_rc_dec((void*)e.key);
    if (d->val_own == 1) py23_rc_dec((void*)e.val);
    dict_rebuild(d);
    return tup;
}

/* set.pop()：移除并返回最后一个插入项（py23 的确定性选择）。 */
int64_t py23_dict_pop_last(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "pop from an empty set");
    int64_t slot = d->order[d->len - 1];
    int64_t key = d->entries[slot].key;
    d->len--;
    dict_rebuild(d);
    return key;  /* 所有权转交给调用方 */
}

void py23_dict_remove(Py23Dict* d, int64_t key, int64_t missing_ok) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (!missing_ok) py23_raise_c("KeyError", "key not found");
        return;
    }
    if (d->val_own == 1) py23_rc_dec((void*)v);
}

void py23_dict_clear(Py23Dict* d) {
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    d->len = 0;
    memset(d->entries, 0, sizeof(Py23DictEntry) * (size_t)d->cap);
}

static int list_value_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return py23_str_eq((Py23Str*)a, (Py23Str*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

int64_t py23_list_contains(Py23List* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return 1;
    return 0;
}

int64_t py23_str_contains(Py23Str* hay, Py23Str* needle) {
    if (needle->len == 0) return 1;
    if (needle->len > hay->len) return 0;
    for (int64_t i = 0; i + needle->len <= hay->len; ++i)
        if (memcmp(hay->data + i, needle->data, (size_t)needle->len) == 0) return 1;
    return 0;
}

/* ── 切片 ─────────────────────────────────────────────────────── */

/* 缺省边界用 INT64_MIN 当哨兵；显式写 INT64_MIN 的极端索引会被当成缺省。 */
static void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step,
                         int64_t* out_start, int64_t* out_stop, int64_t* out_step) {
    int has_start = start != INT64_MIN;
    int has_stop = stop != INT64_MIN;
    int has_step = step != INT64_MIN;
    if (!has_step) step = 1;
    if (step == 0) py23_raise_c("ValueError", "slice step cannot be zero");

    if (step > 0) {
        if (!has_start) start = 0;
        else {
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start > len) start = len;
        }
        if (!has_stop) stop = len;
        else {
            if (stop < 0) stop += len;
            if (stop < 0) stop = 0;
            if (stop > len) stop = len;
        }
    } else {
        if (!has_start) start = len - 1;
        else {
            if (start < 0) start += len;
            if (start < 0) start = -1;
            if (start >= len) start = len - 1;
        }
        if (!has_stop) stop = -1;
        else {
            if (stop < 0) stop += len;
            if (stop < 0) stop = -1;
            if (stop >= len) stop = len - 1;
        }
    }
    *out_start = start;
    *out_stop = stop;
    *out_step = step;
}

static int64_t slice_total(int64_t start, int64_t stop, int64_t step) {
    if (step > 0) {
        if (start >= stop) return 0;
        return (stop - start + step - 1) / step;
    }
    if (start <= stop) return 0;
    return (start - stop + (-step) - 1) / (-step);
}

Py23List* py23_list_slice(Py23List* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    Py23List* out = py23_list_new(count, src->elem_kind);
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) {
        int64_t v = src->data[i];
        if (src->elem_kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23Str* py23_str_slice(Py23Str* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    Py23Str* out = str_alloc(count);
    char* p = (char*)out->data;
    int64_t j = 0;
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) p[j++] = src->data[i];
    return out;
}

/* 切片赋值。src 的元素引用由本函数接管：kind 1 时对新元素 inc、
   对被移除的旧元素 dec；src == dst 时先做快照，避免边读边写。 */
void py23_list_slice_assign(Py23List* dst, int64_t start, int64_t stop, int64_t step,
                            Py23List* src) {
    int64_t lo, hi, st;
    slice_bounds(dst->len, start, stop, step, &lo, &hi, &st);
    int64_t n = slice_total(lo, hi, st);
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    int64_t slen = src->len;
    if (src == dst) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    int kind = (int)dst->elem_kind;
    if (st != 1) {
        if (slen != n) py23_raise_c("ValueError", "cannot assign sequence of different size to extended slice");
        for (int64_t i = 0; i < n; ++i) {
            int64_t idx = lo + i * st;
            int64_t v = sv[i];
            int64_t old = dst->data[idx];
            if (kind == 1 && v) py23_rc_inc((void*)v);
            else if (kind == 2 && src->elem_kind == 1 && v) py23_rc_inc((void*)v);
            dst->data[idx] = v;
            if (kind == 1 && old) py23_rc_dec((void*)old);
        }
        free(snap);
        return;
    }
    int64_t delta = slen - n;
    if (delta > 0) {
        if (dst->len + delta > dst->cap) {
            int64_t ncap = dst->cap ? dst->cap : 4;
            while (ncap < dst->len + delta) ncap *= 2;
            int64_t* nd = (int64_t*)realloc(dst->data, sizeof(int64_t) * (size_t)ncap);
            if (!nd) panic_c("out of memory");
            dst->data = nd;
            dst->cap = ncap;
        }
        memmove(&dst->data[hi + delta], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    } else if (delta < 0) {
        // [lo+slen, hi) 的旧元素会被下面的 memmove 覆盖，先释放。
        if (kind == 1)
            for (int64_t i = lo + slen; i < hi; ++i)
                if (dst->data[i]) py23_rc_dec((void*)dst->data[i]);
        memmove(&dst->data[lo + slen], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        int64_t old = dst->data[lo + i];
        if (kind == 1 && v) py23_rc_inc((void*)v);
        else if (kind == 2 && src->elem_kind == 1 && v) py23_rc_inc((void*)v);
        dst->data[lo + i] = v;
        // 增长时 memmove 会把尾部元素复制到 [hi, hi+delta)，这些副本
        // 没有独立的引用计数；只有原区间 [lo, hi) 的旧元素归本列表所有。
        if (kind == 1 && old && (delta <= 0 || lo + i < hi))
            py23_rc_dec((void*)old);
    }
    free(snap);
}

Py23List* py23_str_to_list(Py23Str* s) {
    Py23List* out = py23_list_new(s->len, 1);
    for (int64_t i = 0; i < s->len; ++i)
        py23_list_append(out, (int64_t)py23_str_char_at(s, i));
    return out;
}

/* range(...) 作为值使用时实体化成整数列表。for 循环走的是专门的
   计数循环，不经过这里。 */
Py23List* py23_range_new(int64_t start, int64_t stop, int64_t step) {
    if (step == 0) py23_raise_c("ValueError", "range() arg 3 must not be zero");
    int64_t n;
    if (step > 0)
        n = stop > start ? (stop - start + step - 1) / step : 0;
    else
        n = stop < start ? (start - stop + (-step) - 1) / (-step) : 0;
    Py23List* out = py23_list_new(n, 0);
    for (int64_t i = 0, v = start; i < n; ++i, v += step)
        py23_list_append(out, v);
    return out;
}

/* ── 对象 ─────────────────────────────────────────────────────── */

/* 对象 = [vtable 指针][字段 0][字段 1]…。分配时全部清零：0 同时是
   None / 0 / 假，和栈槽的约定一致，所以「声明了但没赋值的字段」
   天然就是 None。vtable 指针由生成代码紧接着写入。
   dtor 由代码生成按类布局生成，只 dec 堆类型字段。 */
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*)) {
    if (size < 8) panic_c("object size too small");
    return rc_alloc(size, dtor, release, PY23_RC_OBJ);
}

/* ── 循环收集器 ───────────────────────────────────────────────── */

static void** gc_globals;
static int64_t gc_n_globals;
static int64_t gc_globals_cap;
static uintptr_t gc_stack_top;
static Py23Rc** gc_table;
static size_t gc_table_cap;
static Py23Rc** gc_work;
static size_t gc_work_len;

#if defined(__SANITIZE_ADDRESS__)
#define PY23_NO_ASAN __attribute__((no_sanitize_address))
#else
#define PY23_NO_ASAN
#endif

static size_t gc_hash(uintptr_t p) {
    return (size_t)((p >> 4) * 11400714819323198485ull);
}

static void gc_table_insert(Py23Rc* h) {
    size_t mask = gc_table_cap - 1;
    size_t i = gc_hash((uintptr_t)h + (uintptr_t)PY23_RC_HDR) & mask;
    while (gc_table[i]) i = (i + 1) & mask;
    gc_table[i] = h;
}

static Py23Rc* gc_table_find(uintptr_t payload) {
    if (!gc_table_cap) return NULL;
    size_t mask = gc_table_cap - 1;
    size_t i = gc_hash(payload) & mask;
    while (gc_table[i]) {
        if ((uintptr_t)gc_table[i] + (uintptr_t)PY23_RC_HDR == payload)
            return gc_table[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

static void gc_mark(uintptr_t v) {
    if (!v) return;
    Py23Rc* h = gc_table_find(v);
    if (!h || h->marked) return;
    h->marked = 1;
    gc_work[gc_work_len++] = h;
}

PY23_NO_ASAN static void gc_scan_range(const void* begin, const void* end) {
    uintptr_t a = (uintptr_t)begin;
    uintptr_t b = (uintptr_t)end;
    if (a >= b) return;
    for (; a + sizeof(uintptr_t) <= b; a += sizeof(uintptr_t)) {
        uintptr_t v;
        memcpy(&v, (const void*)a, sizeof v);
        gc_mark(v);
    }
}

static void gc_scan_object(Py23Rc* h) {
    void* p = (char*)h + PY23_RC_HDR;
    switch (h->kind) {
        case PY23_RC_STR:
            break;
        case PY23_RC_LIST: {
            Py23List* l = (Py23List*)p;
            if (l->data)
                for (int64_t i = 0; i < l->len; ++i) gc_mark((uintptr_t)l->data[i]);
            break;
        }
        case PY23_RC_DICT: {
            Py23Dict* d = (Py23Dict*)p;
            for (int64_t i = 0; i < d->len; ++i) {
                Py23DictEntry e = d->entries[d->order[i]];
                gc_mark((uintptr_t)e.key);
                gc_mark((uintptr_t)e.val);
            }
            break;
        }
        case PY23_RC_OBJ:
        case PY23_RC_CLOSURE: {
            for (int64_t off = 0; off + (int64_t)sizeof(uintptr_t) <= h->size;
                 off += (int64_t)sizeof(uintptr_t)) {
                uintptr_t v;
                memcpy(&v, (const char*)p + off, sizeof v);
                gc_mark(v);
            }
            break;
        }
        default:
            break;
    }
}

// 取真实栈指针：ASan 会把带地址的局部变量搬到 fake stack，用 &local
// 当扫描起点会跨到未映射区域。生成的代码帧本身在真实栈上。
PY23_NO_ASAN static uintptr_t gc_current_sp(void) {
    uintptr_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    return sp;
}

// setjmp 会把 callee-saved 寄存器存进 jmp_buf；生成代码里常驻寄存器的
// 堆指针就靠这一段兜住。
PY23_NO_ASAN static void gc_scan_registers(void) {
    jmp_buf regs;
    if (setjmp(regs) == 0) gc_scan_range(&regs, (char*)&regs + sizeof regs);
}

void py23_gc_add_roots(void** roots, int64_t n_roots) {
    if (!roots || n_roots <= 0) return;
    if (gc_n_globals + n_roots > gc_globals_cap) {
        int64_t ncap = gc_globals_cap ? gc_globals_cap : 64;
        while (ncap < gc_n_globals + n_roots) ncap *= 2;
        void** ns = (void**)realloc(gc_globals, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        gc_globals = ns;
        gc_globals_cap = ncap;
    }
    // 根表本身是静态存储，这里只复制槽地址。
    for (int64_t i = 0; i < n_roots; ++i) gc_globals[gc_n_globals++] = roots[i];
}

void py23_gc_init(void** roots, int64_t n_roots, void* stack_top) {
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    py23_gc_add_roots(roots, n_roots);
    gc_stack_top = (uintptr_t)stack_top;
    gc_ready = 1;
}

void py23_gc_unref_child(void* p) {
    if (!p) return;
    Py23Rc* h = gc_table_find((uintptr_t)p);
    if (h && h->marked && h->rc > 0) h->rc--;
}

static void gc_drain(void) {
    while (gc_work_len) gc_scan_object(gc_work[--gc_work_len]);
}

void py23_gc_collect(void) {
    if (!gc_ready || !heap_head || !gc_stack_top) {
        alloc_since_gc = 0;
        return;
    }
    size_t n = 0;
    for (Py23Rc* h = heap_head; h; h = h->next) ++n;
    if (n == 0) {
        alloc_since_gc = 0;
        return;
    }
    gc_table_cap = 1;
    while (gc_table_cap < n * 2) gc_table_cap <<= 1;
    gc_table = (Py23Rc**)calloc(gc_table_cap, sizeof(Py23Rc*));
    gc_work = (Py23Rc**)malloc(n * sizeof(Py23Rc*));
    if (!gc_table || !gc_work) panic_c("out of memory");
    for (Py23Rc* h = heap_head; h; h = h->next) {
        h->marked = 0;
        gc_table_insert(h);
    }
    gc_work_len = 0;

    for (int64_t i = 0; i < gc_n_globals; ++i) {
        void* v = *(void**)gc_globals[i];
        gc_mark((uintptr_t)v);
    }
    for (int64_t i = 0; i < temp_len; ++i) gc_mark((uintptr_t)temp_stack[i]);
    gc_scan_range((const void*)gc_current_sp(), (const void*)gc_stack_top);
    gc_scan_registers();
    gc_drain();

    // 第一遍：只处理不可达对象指向可达对象的引用。此时所有头都还活着，
    // 查表安全；不调用 dtor，避免释放顺序问题。
    gc_sweeping = 1;
    for (Py23Rc* h = heap_head; h; h = h->next) {
        if (!h->marked && h->release)
            h->release((char*)h + PY23_RC_HDR);
    }

    // 第二遍：释放不可达对象，清理可达对象的 mark 位。
    Py23Rc* h = heap_head;
    while (h) {
        Py23Rc* next = h->next;
        if (h->marked) {
            h->marked = 0;
        } else {
            rc_unlink(h);
            if (h->kind == PY23_RC_OBJ)
                free_attached_exception((char*)h + PY23_RC_HDR, h->size);
            if (h->kind == PY23_RC_LIST) {
                Py23List* l = (Py23List*)((char*)h + PY23_RC_HDR);
                free(l->data);
            } else if (h->kind == PY23_RC_DICT) {
                Py23Dict* d = (Py23Dict*)((char*)h + PY23_RC_HDR);
                free(d->entries);
                free(d->order);
            }
            free(h);
        }
        h = next;
    }
    gc_sweeping = 0;

    free(gc_table);
    free(gc_work);
    gc_table = NULL;
    gc_work = NULL;
    gc_table_cap = 0;
    gc_work_len = 0;
    alloc_since_gc = 0;
}

// 程序退出：所有还活着的堆对象（包括环）一次性还给系统。
void py23_gc_shutdown(void) {
    gc_sweeping = 1;
    while (heap_head) {
        Py23Rc* h = heap_head;
        rc_unlink(h);
        if (h->kind == PY23_RC_OBJ)
            free_attached_exception((char*)h + PY23_RC_HDR, h->size);
        if (h->kind == PY23_RC_LIST) {
            Py23List* l = (Py23List*)((char*)h + PY23_RC_HDR);
            free(l->data);
        } else if (h->kind == PY23_RC_DICT) {
            Py23Dict* d = (Py23Dict*)((char*)h + PY23_RC_HDR);
            free(d->entries);
            free(d->order);
        }
        free(h);
    }
    gc_sweeping = 0;
    alloc_since_gc = 0;
    free(gc_globals);
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
}

/* 分模块编译：init 是模块顶层生成的 void(void)，ready 是模块 .bss 里的
   一次性标志。标志在调用前设置，初始化失败不会重试（与 Python 移除
   失败模块的行为不同，但至少不会死循环）。 */
void py23_import_module(int64_t* ready, void (*init)(void)) {
    if (!ready || !init || *ready) return;
    *ready = 1;
    init();
}

/* ── 数学 / 内置模块 ──────────────────────────────────────────── */

double py23_math_sqrt(double x) {
    if (x < 0) py23_raise_c("ValueError", "math domain error");
    return sqrt(x);
}
int64_t py23_math_floor(double x) { return (int64_t)floor(x); }
int64_t py23_math_ceil(double x) { return (int64_t)ceil(x); }
double py23_math_fabs(double x) { return fabs(x); }
double py23_math_sin(double x) { return sin(x); }
double py23_math_cos(double x) { return cos(x); }
double py23_math_tan(double x) { return tan(x); }
double py23_math_log(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log(x);
}
double py23_math_log2(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log2(x);
}
double py23_math_log10(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log10(x);
}
double py23_math_exp(double x) { return exp(x); }
double py23_math_pow(double x, double y) { return pow(x, y); }
double py23_math_fmod(double x, double y) {
    if (y == 0) py23_raise_c("ValueError", "math domain error");
    return fmod(x, y);
}

static Py23List* sys_argv_list;

void py23_sys_set_args(int64_t argc, char** argv) {
    Py23List* out = py23_list_new(argc > 0 ? argc : 0, 1);
    sys_argv_list = out;
    // 列表要立刻成为 GC 根；根表元素是「槽的地址」，不是槽里的值。
    void* root_slot[1] = {(void*)&sys_argv_list};
    py23_gc_add_roots(root_slot, 1);
    for (int64_t i = 0; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        Py23Str* str = str_new(s, (int64_t)strlen(s));
        py23_list_append(out, (int64_t)str);
    }
}

Py23List* py23_sys_argv(void) {
    if (!sys_argv_list) sys_argv_list = py23_list_new(0, 1);
    return sys_argv_list;
}

void py23_sys_exit(int64_t code) { exit((int)code); }

/* ── 输出 ─────────────────────────────────────────────────────── */

void py23_print_i64(int64_t v) { printf("%lld", (long long)v); }

void py23_print_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    fputs(buf, stdout);
}

void py23_print_bool(int64_t v) { fputs(v ? "True" : "False", stdout); }

void py23_print_str(Py23Str* s) { fwrite(s->data, 1, (size_t)s->len, stdout); }

void py23_print_none(void) { fputs("None", stdout); }

void py23_print_sep(void) { fputc(' ', stdout); }

void py23_print_end(void) { fputc('\n', stdout); }

void py23_panic(Py23Str* msg) {
    py23_raise_c("RuntimeError", msg->data);
}

void py23_panic_div0(void) {
    py23_raise_c("ZeroDivisionError", "integer division or modulo by zero");
}

void py23_panic_overflow(void) {
    py23_raise_c("OverflowError", "integer division result too large for int64");
}

/* ── 数学 ─────────────────────────────────────────────────────── */

int64_t py23_pow_i64(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}
