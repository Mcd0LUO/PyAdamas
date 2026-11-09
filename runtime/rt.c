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
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void py23_gc_collect(void);
void py23_gc_shutdown(void);
void py23_gc_unref_child(void* p);

static void panic_c(const char* msg) {
    fprintf(stderr, "py23: runtime error: %s\n", msg);
    exit(1);
}

static void rc_link(Py23Rc* h) {
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
}

static void rc_unlink(Py23Rc* h) {
    if (h->prev) h->prev->next = h->next;
    else heap_head = h->next;
    if (h->next) h->next->prev = h->prev;
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
    if (i < 0 || i >= s->len) panic_c("string index out of range");
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
    if (i < 0 || i >= l->len) panic_c("list index out of range");
    return l->data[i];
}

void py23_list_set(Py23List* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) panic_c("list assignment index out of range");
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

void py23_gc_init(void** roots, int64_t n_roots, void* stack_top) {
    gc_globals = roots;
    gc_n_globals = n_roots;
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
            if (h->kind == PY23_RC_LIST) {
                Py23List* l = (Py23List*)((char*)h + PY23_RC_HDR);
                free(l->data);
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
        if (h->kind == PY23_RC_LIST) {
            Py23List* l = (Py23List*)((char*)h + PY23_RC_HDR);
            free(l->data);
        }
        free(h);
    }
    gc_sweeping = 0;
    alloc_since_gc = 0;
}

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
    fprintf(stderr, "py23: panic: %.*s\n", (int)msg->len, msg->data);
    exit(1);
}

void py23_panic_div0(void) {
    fprintf(stderr, "py23: ZeroDivisionError: integer division or modulo by zero\n");
    exit(1);
}

void py23_panic_overflow(void) {
    fprintf(stderr, "py23: OverflowError: integer division result too large for int64\n");
    exit(1);
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
