// rt_core.c —— 引用计数、临时值栈、闭包、字符串分配、循环收集器
#include "rt_internal.h"

/* ── 引用计数 ─────────────────────────────────────────────────── */

/* 隐藏头放在负载前面，负载指针（字符串的 len、对象的 vtable）保持原样。
   字面量在 .data.rel.ro 里静态构造，rc 是 INT64_MIN，inc/dec 直接跳过。

   头同时是循环收集器的注册表节点：
   - dtor：正常引用计数归零时调用（负责 dec 子对象、释放列表 data）；
   - release：GC 判断对象不可达时调用，只把「指向可达对象的引用」减掉，
     不释放任何东西（子对象由清扫阶段统一处理）；
   - size/kind：GC 保守扫描负载时要知道范围。 */

Py23Rc* heap_head;
int64_t alloc_since_gc;
int gc_ready;
int gc_sweeping;
/* 链上的对象数（叶子不进链，不计）。增量维护，收集时不用再数一遍。 */
size_t gc_tracked_count;
/* 链上「负载需要保守扫描」的对象数（OBJ/实例）。为 0 且 exact-RC 时
   GC 可以走精确快速路径：不需要地址表。 */
size_t gc_conservative_count;

/* 下一次触发全堆扫描的分配量。固定 2MB 会让大堆反复全扫——1M 个
   存活对象约 88MB，按 2MB 触发要扫四十多遍，总成本接近 O(n²)。
   每次扫描完按「本次存活字节数」重置，存活堆翻倍才扫一次，总扫描
   量是 O(live)；垃圾多、存活少时回落到 2MB 下限，扫描本身也便宜。 */
static int64_t gc_next_threshold = PY23_GC_THRESHOLD;

/* 存活堆索引只服务 unknown 值的保守 retain/dec（值可能是整数，必须先
   确认是堆指针）。整程序没有任何 unknown 时，入口会调用
   py23_disable_unknown_index 关掉它，省掉每个堆对象的插入/删除。 */
static int rc_index_off;

/* exact-RC 模式（= 索引关闭）下，字符串这类叶子对象不进 GC 堆链：
   RC 是精确的，不可达字符串在 rc 归零时就释放了，GC 永远不需要看到
   它们；而它们又不可能成环。标记没进链的对象用 marked=2 区分，
   清扫垃圾容器时对这类子对象直接走正常 RC 释放。 */
static int gc_untracked_leaves;

#define PY23_RC_UNTRACKED 2

/* 短字符串 freelist。split / capitalize / join 这类路径会高频产生
   几个字节的小字符串，每次都 malloc/free 会把内存分配变成热点。
   按 8 字节粒度分档（<=128 字节），每档最多缓存 64 块。复用时块容量
   一定 >= 请求大小（分配时向上取整到同一档），字符串仍按自己的 len 访问。 */
#define STR_POOL_MAX_SIZE 128
#define STR_POOL_CLASSES (STR_POOL_MAX_SIZE / 8)
#define STR_POOL_MAX_PER_CLASS 64
static Py23Rc* str_pool[STR_POOL_CLASSES];
static int str_pool_count[STR_POOL_CLASSES];

static int str_pool_class(int64_t size) {
    if (size <= 0 || size > STR_POOL_MAX_SIZE) return -1;
    return (int)((size + 7) / 8) - 1;
}

static Py23Rc* str_pool_get(int64_t size) {
    int c = str_pool_class(size);
    if (c < 0 || !str_pool[c]) return NULL;
    Py23Rc* h = str_pool[c];
    str_pool[c] = h->next;
    str_pool_count[c]--;
    return h;
}

static void str_pool_put(Py23Rc* h) {
    int c = str_pool_class(h->size);
    if (c < 0 || str_pool_count[c] >= STR_POOL_MAX_PER_CLASS) {
        free(h);
        return;
    }
    h->next = str_pool[c];
    h->prev = NULL;
    str_pool[c] = h;
    str_pool_count[c]++;
}

/* exact-RC 模式下可以完全绕开 GC 的对象：没有任何堆字段（没有出边），
   不可能处在环里。字符串是典型；没有堆字段的类实例（生成代码不给
   dtor/release）同理。它们的生死完全由 RC 决定。 */
static int rc_leaf_untracked(const Py23Rc* h) {
    if (!gc_untracked_leaves) return 0;
    if (h->kind == PY23_RC_STR) return 1;
    // range 只存边界，没有堆字段（出边为零），不可能处在环里。
    if (h->kind == PY23_RC_RANGE) return 1;
    if (h->kind == PY23_RC_INSTANCE && !h->dtor && !h->release) return 1;
    return 0;
}

void panic_c(const char* msg);

/* 存活堆对象索引：unknown 值的静态类型看不见，不能直接 rc_inc（可能
   是整数），先按 payload 查注册表；查到才说明它真是堆对象。索引只用于
   这条保守 retain 路径，开放寻址 + 墓碑。 */
Py23Rc** rc_index;
size_t rc_index_cap;
size_t rc_index_fill;  // 非空槽（存活 + 墓碑）
size_t rc_index_live;  // 存活对象数
Py23Rc* const RC_TOMB = (Py23Rc*)(intptr_t)-1;

size_t rc_index_slot(Py23Rc* h) {
    uint64_t x = (uint64_t)(uintptr_t)h >> 4;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    return (size_t)x & (rc_index_cap - 1);
}

void rc_index_rehash(size_t ncap) {
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

void rc_index_put(Py23Rc* h) {
    if (rc_index_off) return;
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

void rc_index_del(Py23Rc* h) {    if (rc_index_off) return;
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

int rc_index_has(Py23Rc* h) {
    if (rc_index_off) return 0;
    if (rc_index_cap == 0) return 0;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) return 1;
        s = (s + 1) & (rc_index_cap - 1);
    }
    return 0;
}

/* 代码生成把静态字符串字面量指针列表放在 .Lstrtab，模块入口在跑用户
   代码前调用这里登记。字面量 rc=INT64_MIN 永生，不进 GC 堆链，但
   unknown 值的运行期类型检查要靠 RC 索引确认指针确实指向 Py23Str。 */
void py23_register_literals(void** objs, int64_t n) {
    for (int64_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        Py23Rc* h = (Py23Rc*)(uintptr_t)objs[i] - 1;
        if (!rc_index_has(h)) rc_index_put(h);
    }
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
void py23_rc_dec_unknown(int64_t v) {
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
void dict_gc_release(void* p);

Py23List* py23_list_new(int64_t cap, int64_t elem_kind);
void py23_list_append(Py23List* l, int64_t v);

void panic_c(const char* msg) {
    fprintf(stderr, "py23: runtime error: %s\n", msg);
    exit(1);
}

void rc_link(Py23Rc* h) {
    if (rc_leaf_untracked(h)) {
        h->prev = NULL;
        h->next = NULL;
        h->marked = PY23_RC_UNTRACKED;
        return;
    }
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
    gc_tracked_count++;
    if (h->kind == PY23_RC_OBJ || h->kind == PY23_RC_INSTANCE)
        gc_conservative_count++;
    rc_index_put(h);
}

void rc_unlink(Py23Rc* h) {
    if (h->marked == PY23_RC_UNTRACKED) return;
    if (h->prev) h->prev->next = h->next;
    else heap_head = h->next;
    if (h->next) h->next->prev = h->prev;
    gc_tracked_count--;
    if (h->kind == PY23_RC_OBJ || h->kind == PY23_RC_INSTANCE)
        gc_conservative_count--;
    rc_index_del(h);
}

void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind) {
    if (size < 0) panic_c("negative allocation size");
    if (gc_ready && !gc_sweeping && heap_head &&
        alloc_since_gc >= gc_next_threshold)
        py23_gc_collect();
    // 短字符串按 8 字节取整，freelist 里的块容量和档位严格对应。
    int64_t alloc_size = size;
    if (kind == PY23_RC_STR && size > 0 && size <= STR_POOL_MAX_SIZE)
        alloc_size = (size + 7) & ~(int64_t)7;
    Py23Rc* h = NULL;
    if (kind == PY23_RC_STR) h = str_pool_get(alloc_size);
    if (!h) {
        h = (Py23Rc*)malloc(sizeof(Py23Rc) + (size_t)alloc_size);
        if (!h) panic_c("out of memory");
        h->size = alloc_size;
    }
    h->rc = 1;
    h->dtor = dtor;
    h->release = release;
    h->kind = kind;
    h->marked = 0;
    rc_link(h);
    // 叶子对象不可能成环，exact-RC 模式下也不进链，不必驱动 GC 频率。
    if (!rc_leaf_untracked(h)) alloc_since_gc += alloc_size + PY23_RC_HDR;
    void* p = (char*)h + sizeof(Py23Rc);
    memset(p, 0, (size_t)alloc_size);
    return p;
}

// unknown 值的 print：先查 GC 的地址索引判断是不是堆对象，命中就按真实
// 类型分派，否则按整数。索引只比对地址、不解引用，所以任意 int64 都安全。
// 只认 str——int/bool/float 在 unknown 里本来就分不开，保持按整数输出。
void py23_print_any(int64_t v) {
    if (v > 4096) {
        Py23Rc* h = (Py23Rc*)((uintptr_t)v - (uintptr_t)PY23_RC_HDR);
        if (rc_index_has(h) && h->kind == PY23_RC_STR) {
            py23_print_str((Py23Str*)(uintptr_t)v);
            return;
        }
    }
    py23_print_i64(v);
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
    if (gc_sweeping && !(gc_untracked_leaves && h->marked == PY23_RC_UNTRACKED)) {
        // 清扫阶段只调整可达对象的计数，绝不在 GC 内部触发释放。
        if (h->marked && h->rc > 0) h->rc--;
        return;
    }
    if (h->rc <= 0) panic_c("refcount decrement on a freed object");
    if (--h->rc == 0) {
        rc_unlink(h);
        if (h->dtor) h->dtor(p);
        if (h->kind == PY23_RC_STR) str_pool_put(h);
        else free(h);
    }
}

/* 表达式临时值栈：每个新堆值在产生时带着 rc=1 入栈，语句/循环/函数
   边界按 mark 统一释放。这样不需要做活跃性分析，也不会漏掉某条路径。
   入栈不增加计数——栈接管的是「产生者」本来就持有的那一个引用。 */
void** temp_stack;
int64_t temp_len;
int64_t temp_cap;

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
void closure_destroy(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) py23_rc_dec((void*)c[2 + i]);
}

void closure_gc_release(void* p) {
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

Py23Str* str_alloc(int64_t len) {
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

Py23Str* str_new(const char* p, int64_t len) {
    Py23Str* s = str_alloc(len);
    memcpy((char*)s->data, p, (size_t)len);
    return s;
}

/* ── 循环收集器 ───────────────────────────────────────────────── */

void** gc_globals;
int64_t gc_n_globals;
int64_t gc_globals_cap;
uintptr_t gc_stack_top;
Py23Rc** gc_table;
size_t gc_table_cap;
Py23Rc** gc_work;
size_t gc_work_len;
/* 表和工作栈跨收集复用：大堆反复 calloc/free 十几 MB 很伤。 */
static Py23Rc** gc_table_buf;
static size_t gc_table_buf_cap;
static Py23Rc** gc_work_buf;
static size_t gc_work_buf_cap;

#if defined(__SANITIZE_ADDRESS__)
#define PY23_NO_ASAN __attribute__((no_sanitize_address))
#else
#define PY23_NO_ASAN
#endif

size_t gc_hash(uintptr_t p) {
    return (size_t)((p >> 4) * 11400714819323198485ull);
}

void gc_table_insert(Py23Rc* h) {
    size_t mask = gc_table_cap - 1;
    size_t i = gc_hash((uintptr_t)h + (uintptr_t)PY23_RC_HDR) & mask;
    while (gc_table[i]) i = (i + 1) & mask;
    gc_table[i] = h;
}

Py23Rc* gc_table_find(uintptr_t payload) {
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

/* ── 精确路径的根部候选集合 ────────────────────────────────────
   整堆都是 list/dict/closure（没有需要保守扫描的 OBJ/实例）时，唯一
   需要「地址 → 头部」反查的地方是保守的栈/寄存器根。与其给整堆建
   地址表，不如把这些根里的机器字去重成一张小表，再扫一遍堆看谁的
   负载地址命中——小表常驻 L2，比几 MB 的整堆表便宜得多。 */
static uintptr_t* gc_cand;
static size_t gc_cand_cap;  // 2 的幂
static size_t gc_cand_len;

static void gc_cand_rehash(size_t ncap) {
    uintptr_t* old = gc_cand;
    size_t ocap = gc_cand_cap;
    gc_cand = (uintptr_t*)calloc(ncap, sizeof(uintptr_t));
    if (!gc_cand) panic_c("out of memory");
    gc_cand_cap = ncap;
    gc_cand_len = 0;
    for (size_t i = 0; i < ocap; ++i) {
        if (!old[i]) continue;
        size_t s = gc_hash(old[i]) & (ncap - 1);
        while (gc_cand[s]) s = (s + 1) & (ncap - 1);
        gc_cand[s] = old[i];
        gc_cand_len++;
    }
    free(old);
}

static void gc_cand_reset(void) {
    if (gc_cand_cap) memset(gc_cand, 0, gc_cand_cap * sizeof(uintptr_t));
    gc_cand_len = 0;
}

static void gc_cand_add(uintptr_t v) {
    if (!v) return;
    if (gc_cand_cap == 0) gc_cand_rehash(1024);
    else if ((gc_cand_len + 1) * 10 >= gc_cand_cap * 7)
        gc_cand_rehash(gc_cand_cap * 2);
    size_t s = gc_hash(v) & (gc_cand_cap - 1);
    while (gc_cand[s]) {
        if (gc_cand[s] == v) return;
        s = (s + 1) & (gc_cand_cap - 1);
    }
    gc_cand[s] = v;
    gc_cand_len++;
}

static int gc_cand_has(uintptr_t v) {
    if (!gc_cand_len || !v) return 0;
    size_t s = gc_hash(v) & (gc_cand_cap - 1);
    while (gc_cand[s]) {
        if (gc_cand[s] == v) return 1;
        s = (s + 1) & (gc_cand_cap - 1);
    }
    return 0;
}

PY23_NO_ASAN static void gc_cand_add_range(uintptr_t a, uintptr_t b) {
    if (a >= b) return;
    for (; a + sizeof(uintptr_t) <= b; a += sizeof(uintptr_t)) {
        uintptr_t v;
        memcpy(&v, (const void*)a, sizeof v);
        gc_cand_add(v);
    }
}

PY23_NO_ASAN static void gc_cand_add_registers(void) {
    jmp_buf regs;
    if (setjmp(regs) == 0)
        gc_cand_add_range((uintptr_t)&regs, (uintptr_t)&regs + sizeof regs);
}

void gc_mark(uintptr_t v) {
    if (!v) return;
    Py23Rc* h = gc_table_find(v);
    if (!h || h->marked) return;
    h->marked = 1;
    gc_work[gc_work_len++] = h;
}

/* exact-RC 模式专用：子对象是静态类型确定的堆指针（列表元素、
   dict 的 own key/val、闭包 cell），不用查表，直接读头部标记。
   静态字面量 rc 永生、marked 也不该被写（.data.rel.ro 只读）。 */
static void gc_mark_direct(uintptr_t v) {
    if (!v) return;
    Py23Rc* h = (Py23Rc*)(v - (uintptr_t)PY23_RC_HDR);
    if (h->rc == PY23_RC_IMMORTAL || h->marked) return;
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

void gc_scan_object(Py23Rc* h) {
    void* p = (char*)h + PY23_RC_HDR;
    switch (h->kind) {
        case PY23_RC_STR:
            break;
        case PY23_RC_LIST: {
            Py23List* l = (Py23List*)p;
            if (!l->data) break;
            // exact-RC 模式下 elem_kind 只可能是 0/1：0 是纯数值元素，
            // 没有任何引用要标记；1 是静态类型已知的堆指针，直接标记。
            if (gc_untracked_leaves) {
                if (l->elem_kind == 1)
                    for (int64_t i = 0; i < l->len; ++i)
                        gc_mark_direct((uintptr_t)l->data[i]);
            } else {
                for (int64_t i = 0; i < l->len; ++i) gc_mark((uintptr_t)l->data[i]);
            }
            break;
        }
        case PY23_RC_DICT: {
            Py23Dict* d = (Py23Dict*)p;
            int precise = gc_untracked_leaves;
            for (int64_t i = 0; i < d->len; ++i) {
                Py23DictEntry e = d->entries[i];
                if (precise && d->key_own == 1)
                    gc_mark_direct((uintptr_t)e.key);
                else if (!precise || d->key_own == 2)
                    gc_mark((uintptr_t)e.key);
                if (precise && d->val_own == 1)
                    gc_mark_direct((uintptr_t)e.val);
                else if (!precise || d->val_own == 2)
                    gc_mark((uintptr_t)e.val);
            }
            break;
        }
        case PY23_RC_RANGE:
            // range 的负载是四个整数，没有子对象要标记。
            break;
        case PY23_RC_CLOSURE:
        case PY23_RC_OBJ:
        case PY23_RC_INSTANCE: {
            // 闭包负载 = [code, n_caps, cell…]，cell 都是堆上的 list；
            // exact-RC 模式下可以精确标记。OBJ / INSTANCE 形态不固定
            // （异常、文件、类实例），仍按保守扫描。
            if (h->kind == PY23_RC_CLOSURE && gc_untracked_leaves) {
                int64_t* c = (int64_t*)p;
                for (int64_t i = 0; i < c[1]; ++i) gc_mark_direct((uintptr_t)c[2 + i]);
                break;
            }
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
    // 嵌入/基准场景可以关掉循环收集，只靠引用计数（会漏检环）。
    if (getenv("PY23_GC_DISABLE")) return;
    gc_ready = 1;
}

/* 整程序无 unknown 值、不涉及 asyncio/生成器时，入口调用这里关掉
   存活堆索引；此后 rc_index_put/del/has 都是空操作。RC 变精确后，
   字符串这类叶子对象也移出 GC 堆链（不可能成环，RC 归零即释放）。 */
void py23_disable_unknown_index(void) {
    rc_index_off = 1;
    gc_untracked_leaves = 1;
}

void py23_gc_unref_child(void* p) {
    if (!p) return;
    if (gc_untracked_leaves) {
        // exact-RC 模式：子对象一定是堆对象或静态字面量，直接读头部。
        // marked=1 是「本轮可达」，减计数；UNTRACKED 的叶子不在链上，
        // 必须在这里按正常引用释放；marked=0 是同样会被清扫的垃圾。
        Py23Rc* u = (Py23Rc*)p - 1;
        if (u->rc == PY23_RC_IMMORTAL) return;
        if (u->marked == PY23_RC_UNTRACKED) {
            if (u->rc > 0 && --u->rc == 0) free(u);
            return;
        }
        if (u->marked == 1 && u->rc > 0) u->rc--;
        return;
    }
    Py23Rc* h = gc_table_find((uintptr_t)p);
    if (h && h->marked && h->rc > 0) h->rc--;
}

void gc_drain(void) {
    while (gc_work_len) gc_scan_object(gc_work[--gc_work_len]);
}

static double gc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

void py23_gc_collect(void) {
    if (!gc_ready || !heap_head || !gc_stack_top) {
        alloc_since_gc = 0;
        return;
    }
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    if (getenv("PY23_GC_TIMING")) t0 = gc_now_ms();
    size_t n = gc_tracked_count;
    if (n == 0 || !heap_head) {
        alloc_since_gc = 0;
        gc_next_threshold = PY23_GC_THRESHOLD;
        return;
    }
    if (n > gc_work_buf_cap) {
        free(gc_work_buf);
        gc_work_buf = (Py23Rc**)malloc(n * sizeof(Py23Rc*));
        if (!gc_work_buf) panic_c("out of memory");
        gc_work_buf_cap = n;
    }
    gc_work = gc_work_buf;
    gc_work_len = 0;

    int fast = gc_untracked_leaves && gc_conservative_count == 0;
    if (fast) {
        // 根部候选集合 → 一遍扫堆命中标记。没有地址表。
        gc_cand_reset();
        for (int64_t i = 0; i < gc_n_globals; ++i)
            gc_cand_add((uintptr_t)*(void**)gc_globals[i]);
        for (int64_t i = 0; i < temp_len; ++i)
            gc_cand_add((uintptr_t)temp_stack[i]);
        gc_cand_add_range(gc_current_sp(), gc_stack_top);
        gc_cand_add_registers();
        for (Py23Rc* h = heap_head; h; h = h->next) {
            h->marked = 0;
            if (gc_cand_has((uintptr_t)h + (uintptr_t)PY23_RC_HDR)) {
                h->marked = 1;
                gc_work[gc_work_len++] = h;
            }
        }
        if (t0) t1 = gc_now_ms();
        gc_drain();
        if (t0) t2 = gc_now_ms();
    } else {
        size_t cap = 64;
        while (cap < n * 2) cap <<= 1;
        if (cap > gc_table_buf_cap) {
            free(gc_table_buf);
            gc_table_buf = (Py23Rc**)calloc(cap, sizeof(Py23Rc*));
            if (!gc_table_buf) panic_c("out of memory");
            gc_table_buf_cap = cap;
        } else {
            memset(gc_table_buf, 0, cap * sizeof(Py23Rc*));
        }
        gc_table = gc_table_buf;
        gc_table_cap = cap;
        for (Py23Rc* h = heap_head; h; h = h->next) {
            h->marked = 0;
            gc_table_insert(h);
        }
        if (t0) t1 = gc_now_ms();

        for (int64_t i = 0; i < gc_n_globals; ++i) {
            void* v = *(void**)gc_globals[i];
            gc_mark((uintptr_t)v);
        }
        for (int64_t i = 0; i < temp_len; ++i) gc_mark((uintptr_t)temp_stack[i]);
        gc_scan_range((const void*)gc_current_sp(), (const void*)gc_stack_top);
        gc_scan_registers();
        gc_drain();
        if (t0) t2 = gc_now_ms();
    }

    // 第一遍：只处理不可达对象指向可达对象的引用。此时所有头都还活着，
    // 查表安全；不调用 dtor，避免释放顺序问题。
    gc_sweeping = 1;
    for (Py23Rc* h = heap_head; h; h = h->next) {
        if (!h->marked && h->release)
            h->release((char*)h + PY23_RC_HDR);
    }
    if (t0) t3 = gc_now_ms();

    // 第二遍：释放不可达对象，清理可达对象的 mark 位。
    int64_t live = 0;
    Py23Rc* h = heap_head;
    while (h) {
        Py23Rc* next = h->next;
        if (h->marked) {
            live += h->size + PY23_RC_HDR;
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
                free(d->index);
            }
            free(h);
        }
        h = next;
    }
    gc_sweeping = 0;
    if (t0) t4 = gc_now_ms();

    gc_table = NULL;
    gc_table_cap = 0;
    gc_work = NULL;
    gc_work_len = 0;
    alloc_since_gc = 0;
    gc_next_threshold = live > (int64_t)PY23_GC_THRESHOLD ? live : PY23_GC_THRESHOLD;
    if (t0)
        fprintf(stderr,
                "[gc] n=%zu live=%lld table=%.2f mark=%.2f release=%.2f sweep=%.2f ms\n",
                n, (long long)live, t1 - t0, t2 - t1, t3 - t2, t4 - t3);
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
            free(d->index);
        }
        free(h);
    }
    gc_sweeping = 0;
    alloc_since_gc = 0;
    free(gc_globals);
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    free(gc_table_buf);
    gc_table_buf = NULL;
    gc_table_buf_cap = 0;
    free(gc_work_buf);
    gc_work_buf = NULL;
    gc_work_buf_cap = 0;
    free(gc_cand);
    gc_cand = NULL;
    gc_cand_cap = 0;
    gc_cand_len = 0;
}

/* 分模块编译：init 是模块顶层生成的 void(void)，ready 是模块 .bss 里的
   一次性标志。标志在调用前设置，初始化失败不会重试（与 Python 移除
   失败模块的行为不同，但至少不会死循环）。 */
void py23_import_module(int64_t* ready, void (*init)(void)) {
    if (!ready || !init || *ready) return;
    *ready = 1;
    init();
}

