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

PyAdamasRc* heap_head;
int64_t alloc_since_gc;
int gc_ready;
int gc_sweeping;
/* 链上的对象数（叶子不进链，不计）。增量维护，收集时不用再数一遍。 */
size_t gc_tracked_count;
/* 链上「负载需要保守扫描」的对象数（OBJ/实例）。为 0 且 exact-RC 时
   GC 可以走精确快速路径：不需要地址表。 */
size_t gc_conservative_count;

#define PYADAMAS_RC_MARK 1
#define PYADAMAS_RC_UNTRACKED 2
#define PYADAMAS_RC_YOUNG 4

/* ── 分代收集状态（分配热路径和 rc_link 都要用） ──────────────
   young_buf 是上次收集后分配、还没晋升的对象；marked 高位是它在数组里
   的下标，正常 RC 释放时 O(1) 摘除。old_bytes 是老生代存活字节，用来
   决定 major 的触发阈值。 */
static PyAdamasRc** young_buf;
static size_t young_len;
static size_t young_cap;
static int64_t old_bytes;

/* 收集阶段：非 0 时 pa_rc_dec 只调整集合内对象的计数，不直接释放。 */
#define GC_PHASE_NONE 0
#define GC_PHASE_TRIAL 1
#define GC_PHASE_RESTORE 2
#define GC_PHASE_SWEEP 3
static int gc_phase;
/* major 走经典标记-清扫，minor 走 trial-deletion。清扫时的子引用规则
   不同：标记-清扫只减可达子对象，trial 模式跳过集合内（已减过）、
   收集集合外的边。 */
static int gc_mark_sweep;

/* minor 按新生代分配量触发；major 按「上次 major 以来」的分配量触发，
   阈值随老生代存活字节增长（和旧的全堆扫描同一个摊还口径）。 */
static int64_t alloc_since_major;
static int64_t young_threshold = PYADAMAS_GC_THRESHOLD;
static int64_t major_threshold = 4 * PYADAMAS_GC_THRESHOLD;

static void gc_collect(int major);
static void gc_pend_add(int64_t v);

static void young_add(PyAdamasRc* h) {
    if (young_len == young_cap) {
        size_t ncap = young_cap ? young_cap * 2 : 1024;
        PyAdamasRc** ns = (PyAdamasRc**)realloc(young_buf, sizeof(PyAdamasRc*) * ncap);
        if (!ns) panic_c("out of memory");
        young_buf = ns;
        young_cap = ncap;
    }
    h->marked = ((int64_t)young_len << 8) | PYADAMAS_RC_YOUNG;
    young_buf[young_len++] = h;
}

static void young_del(PyAdamasRc* h) {
    size_t i = (size_t)(h->marked >> 8);
    if (i >= young_len || young_buf[i] != h) return;
    size_t last = --young_len;
    if (i != last) {
        PyAdamasRc* m = young_buf[last];
        young_buf[i] = m;
        m->marked = ((int64_t)i << 8) | (m->marked & 0xFF);
    }
    h->marked = 0;
}

/* 收集期间的子引用处理：
   - trial：集合内对象 rc--（试探性减计数）；
   - restore：集合内对象 rc++（把存活对象的出边加回去）；
   - sweep：集合内对象已经减过，跳过；集合外（老对象）的边攒进 pending，
     回合结束后按正常 RC 释放，避免清扫中重入释放。
   永生字面量在入口就跳过了；exact 模式下的叶子不在集合里，清扫时按
   普通引用释放。 */
static void gc_phase_dec(PyAdamasRc* h, void* p) {
    if (gc_phase == GC_PHASE_TRIAL) {
        if (h->marked & PYADAMAS_RC_YOUNG) h->rc--;
        return;
    }
    if (gc_phase == GC_PHASE_RESTORE) {
        if (h->marked & PYADAMAS_RC_YOUNG) h->rc++;
        return;
    }
    if (h->marked & PYADAMAS_RC_UNTRACKED) {
        if (h->rc > 0 && --h->rc == 0) free(h);
        return;
    }
    if (gc_mark_sweep) {
        if ((h->marked & PYADAMAS_RC_MARK) && h->rc > 0) h->rc--;
        return;
    }
    if (h->marked & PYADAMAS_RC_YOUNG) return;
    gc_pend_add((int64_t)(uintptr_t)p);
}

/* 存活堆索引只服务 unknown 值的保守 retain/dec（值可能是整数，必须先
   确认是堆指针）。整程序没有任何 unknown 时，入口会调用
   pa_disable_unknown_index 关掉它，省掉每个堆对象的插入/删除。 */
static int rc_index_off;

/* exact-RC 模式（= 索引关闭）下，字符串这类叶子对象不进 GC 堆链：
   RC 是精确的，不可达字符串在 rc 归零时就释放了，GC 永远不需要看到
   它们；而它们又不可能成环。标记没进链的对象用 marked=2 区分，
   清扫垃圾容器时对这类子对象直接走正常 RC 释放。 */
static int gc_untracked_leaves;

/* RC/GC 统计：PYADAMAS_RC_STATS=1 时在退出时打印。
   统计本身只是一个可预测的全局分支，不改变所有权语义。 */
static int rc_stats_on;
static int64_t stat_rc_inc;
static int64_t stat_rc_inc_unknown;
static int64_t stat_rc_dec;
static int64_t stat_rc_dec_unknown;
static int64_t stat_rc_temp;
static int64_t stat_rc_cleanup;
static int64_t stat_rc_free;
static int64_t stat_gc_runs;
static int64_t stat_gc_minor;
static int64_t stat_gc_major;
static int64_t stat_gc_freed;
static int64_t stat_gc_live;
static int64_t stat_take_calls;
static int64_t stat_take_steps;

static void rc_stats_dump(void) {
    fprintf(stderr,
            "[rc] inc=%lld inc_unknown=%lld dec=%lld dec_unknown=%lld temp=%lld "
            "cleanup=%lld free=%lld gc_runs=%lld gc_minor=%lld gc_major=%lld "
            "gc_freed=%lld gc_live=%lld take=%lld take_steps=%lld\n",
            (long long)stat_rc_inc, (long long)stat_rc_inc_unknown,
            (long long)stat_rc_dec,
            (long long)stat_rc_dec_unknown, (long long)stat_rc_temp,
            (long long)stat_rc_cleanup, (long long)stat_rc_free,
            (long long)stat_gc_runs, (long long)stat_gc_minor,
            (long long)stat_gc_major, (long long)stat_gc_freed,
            (long long)stat_gc_live, (long long)stat_take_calls,
            (long long)stat_take_steps);
}

/* 短字符串 freelist。split / capitalize / join 这类路径会高频产生
   几个字节的小字符串，每次都 malloc/free 会把内存分配变成热点。
   按 8 字节粒度分档（<=128 字节），每档最多缓存 64 块。复用时块容量
   一定 >= 请求大小（分配时向上取整到同一档），字符串仍按自己的 len 访问。 */
#define STR_POOL_MAX_SIZE 128
#define STR_POOL_CLASSES (STR_POOL_MAX_SIZE / 8)
#define STR_POOL_MAX_PER_CLASS 64
static PyAdamasRc* str_pool[STR_POOL_CLASSES];
static int str_pool_count[STR_POOL_CLASSES];

static int str_pool_class(int64_t size) {
    if (size <= 0 || size > STR_POOL_MAX_SIZE) return -1;
    return (int)((size + 7) / 8) - 1;
}

static PyAdamasRc* str_pool_get(int64_t size) {
    int c = str_pool_class(size);
    if (c < 0 || !str_pool[c]) return NULL;
    PyAdamasRc* h = str_pool[c];
    str_pool[c] = h->next;
    str_pool_count[c]--;
    return h;
}

static void str_pool_put(PyAdamasRc* h) {
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

/* 列表结构体 freelist：PyAdamasList 固定 32 字节，单独一档。data 缓冲
   由 rt_list.c 的 list_buf_pool 管理。 */
#define LIST_POOL_MAX 64
static PyAdamasRc* list_pool;
static int list_pool_count;

static PyAdamasRc* list_pool_get(void) {
    PyAdamasRc* h = list_pool;
    if (h) {
        list_pool = h->next;
        list_pool_count--;
    }
    return h;
}

static void list_pool_put(PyAdamasRc* h) {
    if (list_pool_count >= LIST_POOL_MAX) {
        free(h);
        return;
    }
    h->next = list_pool;
    h->prev = NULL;
    list_pool = h;
    list_pool_count++;
}

/* exact-RC 模式下可以完全绕开 GC 的对象：没有任何堆字段（没有出边），
   不可能处在环里。字符串是典型；没有堆字段的类实例（生成代码不给
   dtor/release）同理。它们的生死完全由 RC 决定。 */
static int rc_leaf_untracked(const PyAdamasRc* h) {
    if (!gc_untracked_leaves) return 0;
    if (h->kind == PYADAMAS_RC_STR) return 1;
    // range 只存边界，没有堆字段（出边为零），不可能处在环里。
    if (h->kind == PYADAMAS_RC_RANGE) return 1;
    // 值类型 struct：payload 只有标量字段，同样没有出边。
    if (h->kind == PYADAMAS_RC_STRUCT) return 1;
    if (h->kind == PYADAMAS_RC_INSTANCE && !h->dtor && !h->release) return 1;
    return 0;
}

void panic_c(const char* msg);

/* 存活堆对象索引：unknown 值的静态类型看不见，不能直接 rc_inc（可能
   是整数），先按 payload 查注册表；查到才说明它真是堆对象。索引只用于
   这条保守 retain 路径，开放寻址 + 墓碑。 */
PyAdamasRc** rc_index;
size_t rc_index_cap;
size_t rc_index_fill;  // 非空槽（存活 + 墓碑）
size_t rc_index_live;  // 存活对象数
PyAdamasRc* const RC_TOMB = (PyAdamasRc*)(intptr_t)-1;

size_t rc_index_slot(PyAdamasRc* h) {
    uint64_t x = (uint64_t)(uintptr_t)h >> 4;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    return (size_t)x & (rc_index_cap - 1);
}

void rc_index_rehash(size_t ncap) {
    PyAdamasRc** old = rc_index;
    size_t ocap = rc_index_cap;
    rc_index = (PyAdamasRc**)calloc(ncap, sizeof(PyAdamasRc*));
    if (!rc_index) panic_c("out of memory");
    rc_index_cap = ncap;
    rc_index_fill = 0;
    for (size_t i = 0; i < ocap; ++i) {
        PyAdamasRc* h = old[i];
        if (!h || h == RC_TOMB) continue;
        size_t s = rc_index_slot(h);
        while (rc_index[s]) s = (s + 1) & (ncap - 1);
        rc_index[s] = h;
        rc_index_fill++;
    }
    free(old);
}

void rc_index_put(PyAdamasRc* h) {
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

void rc_index_del(PyAdamasRc* h) {    if (rc_index_off) return;
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

int rc_index_has(PyAdamasRc* h) {
    if (rc_index_off) return 0;
    if (rc_index_cap == 0) return 0;
    // RC_TOMB 是墓碑哨兵（(PyAdamasRc*)-1）。unknown 值 v 减去 64 字节头后
    // 可能正好等于它（v == 63），探测时不能把墓碑当成命中。
    if (h == RC_TOMB) return 0;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) return 1;
        s = (s + 1) & (rc_index_cap - 1);
    }
    return 0;
}

/* 代码生成把静态字符串字面量指针列表放在 .Lstrtab，模块入口在跑用户
   代码前调用这里登记。字面量 rc=INT64_MIN 永生，不进 GC 堆链，但
   unknown 值的运行期类型检查要靠 RC 索引确认指针确实指向 PyAdamasStr。 */
void pa_register_literals(void** objs, int64_t n) {
    for (int64_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)objs[i] - 1;
        if (!rc_index_has(h)) rc_index_put(h);
    }
}

/* 对 unknown 值做保守 retain：不是堆指针就什么也不做。查不到的对象
   包括静态字面量（永生，不需要 inc）和整数。 */
int64_t pa_rc_inc_unknown(int64_t v) {
    if (!v) return 0;
    if (rc_stats_on) stat_rc_inc_unknown++;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (!rc_index_has(h)) return 0;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return 1;
    if (gc_sweeping) {
        if (h->marked) h->rc++;
        return 1;
    }
    if (h->rc <= 0) return 1;
    h->rc++;
    return 1;
}

void pa_rc_dec(void* p);

/* unknown 值的保守释放：先按注册表确认是堆指针，再走正常 dec。
   用于「结果类型静态不可知」的跨 await/gather 值。 */
void pa_rc_dec_unknown(int64_t v) {
    if (!v) return;
    if (rc_stats_on) stat_rc_dec_unknown++;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (!rc_index_has(h)) return;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    pa_rc_dec((void*)(uintptr_t)v);
}

void pa_gc_collect(void);
void pa_gc_shutdown(void);
void pa_gc_unref_child(void* p);
void pa_gc_add_roots(void** roots, int64_t n_roots);
void dict_gc_release(void* p);

PyAdamasList* pa_list_new(int64_t cap, int64_t elem_kind);
void pa_list_append(PyAdamasList* l, int64_t v);

void panic_c(const char* msg) {
    fprintf(stderr, "pyadamas: runtime error: %s\n", msg);
    exit(1);
}

void rc_link(PyAdamasRc* h) {
    if (rc_leaf_untracked(h)) {
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_UNTRACKED;
        return;
    }
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
    gc_tracked_count++;
    if (h->kind == PYADAMAS_RC_OBJ || h->kind == PYADAMAS_RC_INSTANCE)
        gc_conservative_count++;
    rc_index_put(h);
    young_add(h);
}

void rc_unlink(PyAdamasRc* h) {
    if (h->marked & PYADAMAS_RC_UNTRACKED) return;
    // 正常 RC 释放时把新生代 O(1) 摘掉；收集期间由收集器统一收尾。
    if (!gc_phase) {
        if (h->marked & PYADAMAS_RC_YOUNG) young_del(h);
        else old_bytes -= h->size + PYADAMAS_RC_HDR;
    }
    if (h->prev) h->prev->next = h->next;
    else heap_head = h->next;
    if (h->next) h->next->prev = h->prev;
    gc_tracked_count--;
    if (h->kind == PYADAMAS_RC_OBJ || h->kind == PYADAMAS_RC_INSTANCE)
        gc_conservative_count--;
    rc_index_del(h);
}

void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind) {
    if (size < 0) panic_c("negative allocation size");
    if (gc_ready && !gc_phase && heap_head) {
        if (alloc_since_major >= major_threshold) gc_collect(1);
        else if (alloc_since_gc >= young_threshold) gc_collect(0);
    }
    // 短字符串按 8 字节取整，freelist 里的块容量和档位严格对应。
    int64_t alloc_size = size;
    if (kind == PYADAMAS_RC_STR && size > 0 && size <= STR_POOL_MAX_SIZE)
        alloc_size = (size + 7) & ~(int64_t)7;
    PyAdamasRc* h = NULL;
    if (kind == PYADAMAS_RC_STR) h = str_pool_get(alloc_size);
    else if (kind == PYADAMAS_RC_LIST && size == (int64_t)sizeof(PyAdamasList))
        h = list_pool_get();
    if (!h) {
        h = (PyAdamasRc*)malloc(sizeof(PyAdamasRc) + (size_t)alloc_size);
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
    if (!rc_leaf_untracked(h)) {
        alloc_since_gc += alloc_size + PYADAMAS_RC_HDR;
        alloc_since_major += alloc_size + PYADAMAS_RC_HDR;
    }
    void* p = (char*)h + sizeof(PyAdamasRc);
    memset(p, 0, (size_t)alloc_size);
    return p;
}

// unknown 值的 print：先查 GC 的地址索引判断是不是堆对象，命中就按真实
// 类型分派，否则按整数。索引只比对地址、不解引用，所以任意 int64 都安全。
// 只认 str——int/bool/float 在 unknown 里本来就分不开，保持按整数输出。
void pa_print_any(int64_t v) {
    if (v > 4096) {
        PyAdamasRc* h = (PyAdamasRc*)((uintptr_t)v - (uintptr_t)PYADAMAS_RC_HDR);
        if (rc_index_has(h) && h->kind == PYADAMAS_RC_STR) {
            pa_print_str((PyAdamasStr*)(uintptr_t)v);
            return;
        }
    }
    pa_print_i64(v);
}

void pa_rc_inc(void* p) {
    if (rc_stats_on) stat_rc_inc++;
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (h->rc <= 0) panic_c("refcount increment on a freed object");
    h->rc++;
}

void pa_rc_dec(void* p) {
    if (rc_stats_on) stat_rc_dec++;
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (gc_phase) {
        // 收集期间只调整计数或登记待释放的边，绝不重入释放。
        gc_phase_dec(h, p);
        return;
    }
    if (h->rc <= 0) panic_c("refcount decrement on a freed object");
    if (--h->rc == 0) {
        if (rc_stats_on) stat_rc_free++;
        rc_unlink(h);
        if (h->dtor) h->dtor(p);
        if (h->kind == PYADAMAS_RC_STR) str_pool_put(h);
        else if (h->kind == PYADAMAS_RC_LIST && h->size == (int64_t)sizeof(PyAdamasList))
            list_pool_put(h);
        else free(h);
    }
}

/* 表达式临时值栈：每个新堆值在产生时带着 rc=1 入栈，语句/循环/函数
   边界按 mark 统一释放。这样不需要做活跃性分析，也不会漏掉某条路径。
   入栈不增加计数——栈接管的是「产生者」本来就持有的那一个引用。 */
void** temp_stack;
int64_t temp_len;
int64_t temp_cap;

int64_t pa_rc_mark(void) { return temp_len; }

void pa_rc_temp(void* p) {
    if (rc_stats_on) stat_rc_temp++;
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (temp_len == temp_cap) {
        int64_t ncap = temp_cap ? temp_cap * 2 : 64;
        void** ns = (void**)realloc(temp_stack, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        temp_stack = ns;
        temp_cap = ncap;
    }
    temp_stack[temp_len++] = p;
}

void pa_rc_cleanup(int64_t mark) {
    if (rc_stats_on) stat_rc_cleanup++;
    if (mark < 0 || mark > temp_len) panic_c("invalid temporary stack mark");
    while (temp_len > mark) pa_rc_dec(temp_stack[--temp_len]);
}

/* 把临时栈上的一个新值取走：所有权转给槽/字段/返回值，cleanup 不再
   释放它。降级器只在「刚产生、未被别名」的值上调用，所以最多命中一次；
   从后往前找，允许中间还夹着别的临时值。 */
void pa_rc_temp_take(void* p) {
    if (rc_stats_on) stat_take_calls++;
    if (!p) return;
    for (int64_t i = temp_len - 1; i >= 0; --i) {
        if (rc_stats_on) stat_take_steps++;
        if (temp_stack[i] == p) {
            temp_stack[i] = temp_stack[--temp_len];
            return;
        }
    }
}

/* unknown 值入临时栈：先按 RC 索引确认是堆对象，再接管调用者的引用。
   unknown 里可能是整数，直接 pa_rc_temp 会按指针读头部。 */
void pa_rc_temp_unknown(int64_t v) {
    if (!v) return;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (!rc_index_has(h)) return;
    pa_rc_temp((void*)(uintptr_t)v);
}

/* 程序退出前由入口函数调用：临时栈此时已经清空，把栈本身的存储还掉。 */
void pa_rc_shutdown(void) {
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
    for (int64_t i = 0; i < c[1]; ++i) pa_rc_dec((void*)c[2 + i]);
}

void closure_gc_release(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) pa_gc_unref_child((void*)c[2 + i]);
}

void* pa_closure_new(int64_t code, int64_t n_caps) {
    if (n_caps < 0) panic_c("negative closure capture count");
    void* p = rc_alloc(16 + 8 * n_caps, closure_destroy, closure_gc_release,
                       PYADAMAS_RC_CLOSURE);
    ((int64_t*)p)[0] = code;
    ((int64_t*)p)[1] = n_caps;
    return p;
}

/* 短串 SSO：<=7 字节直接内联在 PyAdamasStr::hash 的 8 字节里，
   data 指向这段内联区；长串仍用结构体后面的独立字符区。
   值仍然是 8 字节指针（不是 tagged value），所以对象本身仍走 RC/GC。 */
#define STR_INLINE_MAX 7

PyAdamasStr* str_alloc(int64_t len) {
    if (len < 0) panic_c("negative string length");
    if (len <= STR_INLINE_MAX) {
        PyAdamasStr* s =
            (PyAdamasStr*)rc_alloc((int64_t)sizeof(PyAdamasStr), NULL, NULL, PYADAMAS_RC_STR);
        s->len = len;
        s->data = (const char*)&s->hash;
        ((char*)s->data)[len] = '\0';
        return s;
    }
    PyAdamasStr* s =
        (PyAdamasStr*)rc_alloc((int64_t)sizeof(PyAdamasStr) + len + 1, NULL, NULL, PYADAMAS_RC_STR);
    s->len = len;
    s->hash = 0;
    char* p = (char*)(s + 1);
    p[len] = '\0';
    s->data = p;
    return s;
}

PyAdamasStr* str_new(const char* p, int64_t len) {
    PyAdamasStr* s = str_alloc(len);
    memcpy((char*)s->data, p, (size_t)len);
    return s;
}

PyAdamasStr* pa_str_from_cstr(const char* p) {
    if (!p) p = "";
    return str_new(p, (int64_t)strlen(p));
}

PyAdamasStr* pa_str_from_cstr_opt(const char* p) {
    if (!p) return NULL;
    return str_new(p, (int64_t)strlen(p));
}

/* ── 静态驻留字符串 ─────────────────────────────────────────────
   字符串迭代/下标每取一个字符都会造一个单字节串；bool/None 转字符串
   也在 print/repr 里反复出现。字符串不可变，这些结果可以做成永生对象：
   256 个单字节串 + "True"/"False"/"None"，首次用到时初始化并登记进
   RC 索引（unknown 值的运行期类型检查要靠它）。 */
typedef struct {
    PyAdamasRc h;
    PyAdamasStr s;
    char c[8];
} PyAdamasStaticStr;

static PyAdamasStaticStr char_tab[256];
static PyAdamasStaticStr kw_true_s, kw_false_s, kw_none_s;
static int static_strs_ready;

static void static_str_init(PyAdamasStaticStr* e, const char* p, int64_t n) {
    e->h.next = NULL;
    e->h.prev = NULL;
    e->h.rc = PYADAMAS_RC_IMMORTAL;
    e->h.dtor = NULL;
    e->h.release = NULL;
    e->h.size = (int64_t)sizeof(PyAdamasStr);
    e->h.kind = PYADAMAS_RC_STR;
    e->h.marked = PYADAMAS_RC_UNTRACKED;
    e->s.len = n;
    e->s.hash = 0;
    memcpy(e->c, p, (size_t)n);
    e->c[n] = '\0';
    e->s.data = e->c;
    if (!rc_index_off) rc_index_put(&e->h);
}

static void static_strs_init(void) {
    for (int i = 0; i < 256; ++i) {
        char ch = (char)i;
        static_str_init(&char_tab[i], &ch, 1);
    }
    static_str_init(&kw_true_s, "True", 4);
    static_str_init(&kw_false_s, "False", 5);
    static_str_init(&kw_none_s, "None", 4);
    static_strs_ready = 1;
}

static void static_strs_ensure(void) {
    if (!static_strs_ready) static_strs_init();
}

PyAdamasStr* pa_char_str(int64_t c) {
    static_strs_ensure();
    return &char_tab[(int)(c & 0xff)].s;
}

PyAdamasStr* pa_str_true(void) {
    static_strs_ensure();
    return &kw_true_s.s;
}

PyAdamasStr* pa_str_false(void) {
    static_strs_ensure();
    return &kw_false_s.s;
}

PyAdamasStr* pa_str_none(void) {
    static_strs_ensure();
    return &kw_none_s.s;
}

/* ── 分代循环收集器 ─────────────────────────────────────────────
   对象按「是否在当前收集集合」分两代：上次收集后分配、还没晋升的是
   新生代（young_buf），其余存活对象是老生代。minor 只对新生代做
   trial-deletion；老对象的引用按真实 rc 计入外部引用，天然是根，
   所以不需要写屏障。老生代增长到阈值时做 major（集合 = 整堆）。

   一个回合（stop-the-world）分四步：
   1. 试探减计数：对集合内每个对象执行 release 回调，把每条出边从
      子对象的 rc 减掉。减完 rc > 0 的对象被集合外引用，是根。
   2. 标记：从根出发沿出边走一遍（gc_scan_object），未标记的集合成员
      就是环垃圾。
   3. 恢复：标记对象再走一遍 release，把出边计数加回去并晋升。
   4. 清扫：垃圾对象先走 release（指向集合外的边攒进 pending），再
      统一释放；回合结束后 pending 按正常 RC 释放。

   marked 低 8 位是标志（1 本轮可达 / 2 叶子不在链上 / 4 在集合里），
   高位是 young_buf 下标，正常 RC 释放时 O(1) 摘除。
   ─────────────────────────────────────────────────────────────── */

void** gc_globals;
int64_t gc_n_globals;
int64_t gc_globals_cap;
uintptr_t gc_stack_top;
PyAdamasRc** gc_work;
size_t gc_work_len;
/* 工作栈、本轮集合、集合地址表、清扫后待释放的老对象，全部跨回合复用。 */
static PyAdamasRc** gc_work_buf;
static size_t gc_work_buf_cap;
static PyAdamasRc** gc_set;
static size_t gc_set_len;
static PyAdamasRc** gc_set_buf;
static size_t gc_set_buf_cap;
static PyAdamasRc** gc_tab;
static size_t gc_tab_cap;
static PyAdamasRc** gc_tab_buf;
static size_t gc_tab_buf_cap;
static void** gc_pend;
static size_t gc_pend_len;
static size_t gc_pend_cap;

#if defined(__SANITIZE_ADDRESS__)
#define PYADAMAS_NO_ASAN __attribute__((no_sanitize_address))
#else
#define PYADAMAS_NO_ASAN
#endif

size_t gc_hash(uintptr_t p) {
    return (size_t)((p >> 4) * 11400714819323198485ull);
}

static void gc_reserve_set(size_t need) {
    if (need <= gc_set_buf_cap) return;
    size_t ncap = gc_set_buf_cap ? gc_set_buf_cap : 1024;
    while (ncap < need) ncap *= 2;
    PyAdamasRc** ns = (PyAdamasRc**)realloc(gc_set_buf, sizeof(PyAdamasRc*) * ncap);
    if (!ns) panic_c("out of memory");
    gc_set_buf = ns;
    gc_set_buf_cap = ncap;
}

static void gc_pend_add(int64_t v) {
    if (gc_pend_len == gc_pend_cap) {
        size_t ncap = gc_pend_cap ? gc_pend_cap * 2 : 256;
        void** ns = (void**)realloc(gc_pend, sizeof(void*) * ncap);
        if (!ns) panic_c("out of memory");
        gc_pend = ns;
        gc_pend_cap = ncap;
    }
    gc_pend[gc_pend_len++] = (void*)(uintptr_t)v;
}

/* 集合地址表：键是负载地址，值是对应头部。minor 时集合通常远小于整堆，
   比旧的全堆地址表便宜；major 时和旧表的规模一致。 */
static void gc_tab_build(void) {
    size_t cap = 64;
    while (cap < gc_set_len * 2) cap <<= 1;
    if (cap > gc_tab_buf_cap) {
        free(gc_tab_buf);
        gc_tab_buf = (PyAdamasRc**)calloc(cap, sizeof(PyAdamasRc*));
        if (!gc_tab_buf) panic_c("out of memory");
        gc_tab_buf_cap = cap;
    } else {
        memset(gc_tab_buf, 0, cap * sizeof(PyAdamasRc*));
    }
    gc_tab = gc_tab_buf;
    gc_tab_cap = cap;
    size_t mask = cap - 1;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        size_t s = gc_hash((uintptr_t)h + (uintptr_t)PYADAMAS_RC_HDR) & mask;
        while (gc_tab[s]) s = (s + 1) & mask;
        gc_tab[s] = h;
    }
}

static PyAdamasRc* gc_tab_find(uintptr_t v) {
    if (!gc_tab_cap) return NULL;
    size_t mask = gc_tab_cap - 1;
    size_t s = gc_hash(v) & mask;
    while (gc_tab[s]) {
        if ((uintptr_t)gc_tab[s] + (uintptr_t)PYADAMAS_RC_HDR == v) return gc_tab[s];
        s = (s + 1) & mask;
    }
    return NULL;
}

void gc_mark(uintptr_t v) {
    if (!v) return;
    PyAdamasRc* h = gc_tab_find(v);
    if (!h || (h->marked & PYADAMAS_RC_MARK)) return;
    h->marked |= PYADAMAS_RC_MARK;
    gc_work[gc_work_len++] = h;
}

/* 静态类型已知的堆指针：直接读头部，不查表。字面量（永生）和不在本轮
   集合里的老对象都不用标记。 */
static void gc_mark_direct(uintptr_t v) {
    if (!v) return;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)(v - (uintptr_t)PYADAMAS_RC_HDR);
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (!(h->marked & PYADAMAS_RC_YOUNG)) return;
    if (h->marked & PYADAMAS_RC_MARK) return;
    h->marked |= PYADAMAS_RC_MARK;
    gc_work[gc_work_len++] = h;
}

PYADAMAS_NO_ASAN static void gc_scan_range(const void* begin, const void* end) {
    uintptr_t a = (uintptr_t)begin;
    uintptr_t b = (uintptr_t)end;
    if (a >= b) return;
    for (; a + sizeof(uintptr_t) <= b; a += sizeof(uintptr_t)) {
        uintptr_t v;
        memcpy(&v, (const void*)a, sizeof v);
        gc_mark(v);
    }
}

// setjmp 会把 callee-saved 寄存器存进 jmp_buf；生成代码里常驻寄存器的
// 堆指针（槽位提升、全局 vreg）就靠这一段兜住。
PYADAMAS_NO_ASAN static void gc_scan_registers(void) {
    jmp_buf regs;
    if (_setjmp(regs) == 0) gc_scan_range(&regs, (char*)&regs + sizeof regs);
}

/* 保守根只扫帧记录登记过的地址：
   - 借来的 unknown 槽（bit1，只扫描不释放）；
   - 带 unknown 标志的拥有槽（bit0）；
   - 含 unknown vreg 的帧的临时区（scan_base/scan_words）。
   纯类型化帧和栈上的其它内容（返回地址、保存的寄存器、填充）一律
   不扫。 */
static void gc_scan_frames(void) {
    for (PyAdamasFrame* f = pa_frame_top; f; f = f->prev) {
        if (f->scan_words > 0 && f->scan_base)
            gc_scan_range(f->scan_base,
                          (char*)f->scan_base + sizeof(uintptr_t) * (size_t)f->scan_words);
        for (int64_t i = 0; i < f->n; ++i) {
            uintptr_t addr = (uintptr_t)f->slots[i];
            int64_t v = *(int64_t*)(addr & ~(uintptr_t)3);
            if (!v) continue;
            if (addr & 3) gc_mark((uintptr_t)v);   // unknown / 只扫描
            else gc_mark_direct((uintptr_t)v);     // 已知堆槽
        }
    }
}

void gc_scan_object(PyAdamasRc* h) {
    void* p = (char*)h + PYADAMAS_RC_HDR;
    switch (h->kind) {
        case PYADAMAS_RC_STR:
            break;
        case PYADAMAS_RC_LIST: {
            PyAdamasList* l = (PyAdamasList*)p;
            if (!l->data) break;
            // exact-RC 模式：0 是纯数值元素；1 是静态类型已知的堆指针；
            // 2/3 是推不出来的元素，保守查集合表标记。
            if (gc_untracked_leaves) {
                if (l->elem_kind == 1)
                    for (int64_t i = 0; i < l->len; ++i)
                        gc_mark_direct((uintptr_t)l->data[i]);
                else if (l->elem_kind >= 2)
                    for (int64_t i = 0; i < l->len; ++i)
                        gc_mark((uintptr_t)l->data[i]);
            } else {
                for (int64_t i = 0; i < l->len; ++i) gc_mark((uintptr_t)l->data[i]);
            }
            break;
        }
        case PYADAMAS_RC_DICT: {
            PyAdamasDict* d = (PyAdamasDict*)p;
            int precise = gc_untracked_leaves;
            for (int64_t i = 0; i < d->len; ++i) {
                int64_t k = pa_dict_key_at(d, i);
                int64_t v = pa_dict_val_at(d, i);
                if (precise && d->key_own == 1)
                    gc_mark_direct((uintptr_t)k);
                else if (!precise || d->key_own == 2)
                    gc_mark((uintptr_t)k);
                if (precise && d->val_own == 1)
                    gc_mark_direct((uintptr_t)v);
                else if (!precise || d->val_own == 2)
                    gc_mark((uintptr_t)v);
            }
            break;
        }
        case PYADAMAS_RC_RANGE:
            // range 的负载是四个整数，没有子对象要标记。
            break;
        case PYADAMAS_RC_CLOSURE:
        case PYADAMAS_RC_OBJ:
        case PYADAMAS_RC_INSTANCE: {
            // 闭包负载 = [code, n_caps, cell…]，cell 都是堆上的 list；
            // exact-RC 模式下可以精确标记。OBJ / INSTANCE 形态不固定
            // （异常、文件、类实例），仍按保守扫描。
            if (h->kind == PYADAMAS_RC_CLOSURE && gc_untracked_leaves) {
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

void pa_gc_add_roots(void** roots, int64_t n_roots) {
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

void pa_gc_init(void** roots, int64_t n_roots, void* stack_top) {
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    pa_gc_add_roots(roots, n_roots);
    gc_stack_top = (uintptr_t)stack_top;
    // 嵌入/基准场景可以关掉循环收集，只靠引用计数（会漏检环）。
    if (getenv("PYADAMAS_GC_DISABLE")) return;
    gc_ready = 1;
}

/* 入口调用：按环境变量打开 RC/GC 统计。统计关闭时热路径只有一个
   可预测的全局分支。 */
void pa_rc_stats_init(void) {
    if (!getenv("PYADAMAS_RC_STATS")) return;
    rc_stats_on = 1;
    atexit(rc_stats_dump);
}

/* 整程序无 unknown 值、不涉及 asyncio/生成器时，入口调用这里关掉
   存活堆索引；此后 rc_index_put/del/has 都是空操作。RC 变精确后，
   字符串这类叶子对象也移出 GC 堆链（不可能成环，RC 归零即释放）。 */
void pa_disable_unknown_index(void) {
    rc_index_off = 1;
    gc_untracked_leaves = 1;
}

void pa_gc_unref_child(void* p) {
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (gc_phase) {
        gc_phase_dec(h, p);
        return;
    }
    // release 回调只在收集期间调用；这里兜底按正常引用释放。
    pa_rc_dec(p);
}

/* 生成器帧的堆槽释放（GC release 与异常逃出共用）：清扫阶段顺带清零，
   之后重复调用是空操作；trial/restore 阶段只调整计数、不动槽。 */
void pa_gen_slot_release(int64_t* slot, int64_t own) {
    int64_t v = *slot;
    if (!v) return;
    if (gc_phase == GC_PHASE_SWEEP) *slot = 0;
    if (own == 2) pa_rc_dec_unknown(v);
    else pa_rc_dec((void*)(uintptr_t)v);
}

void gc_drain(void) {
    while (gc_work_len) gc_scan_object(gc_work[--gc_work_len]);
}

static double gc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void gc_free_body(PyAdamasRc* h) {
    rc_unlink(h);
    if (h->kind == PYADAMAS_RC_OBJ)
        free_attached_exception((char*)h + PYADAMAS_RC_HDR, h->size);
    if (h->kind == PYADAMAS_RC_LIST) {
        PyAdamasList* l = (PyAdamasList*)((char*)h + PYADAMAS_RC_HDR);
        free(l->data);
    } else if (h->kind == PYADAMAS_RC_DICT) {
        PyAdamasDict* d = (PyAdamasDict*)((char*)h + PYADAMAS_RC_HDR);
        free(d->entries);
        free(d->order);
        free(d->index);
    }
    free(h);
}

static void gc_collect(int major) {
    if (!gc_ready || !heap_head || !gc_stack_top) {
        alloc_since_gc = 0;
        return;
    }
    if (rc_stats_on) {
        stat_gc_runs++;
        if (major) stat_gc_major++;
        else stat_gc_minor++;
    }
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0;
    if (getenv("PYADAMAS_GC_TIMING")) t0 = gc_now_ms();

    // 1. 组装本轮集合。
    if (major) {
        gc_reserve_set(gc_tracked_count);
        gc_set_len = 0;
        for (PyAdamasRc* h = heap_head; h; h = h->next) {
            h->marked = PYADAMAS_RC_YOUNG;
            gc_set_buf[gc_set_len++] = h;
        }
        young_len = 0;
        old_bytes = 0;
    } else {
        gc_reserve_set(young_len);
        if (young_len) memcpy(gc_set_buf, young_buf, sizeof(PyAdamasRc*) * young_len);
        gc_set_len = young_len;
        young_len = 0;
    }
    gc_set = gc_set_buf;
    if (gc_set_len == 0) {
        alloc_since_gc = 0;
        return;
    }
    if (gc_set_len > gc_work_buf_cap) {
        free(gc_work_buf);
        gc_work_buf = (PyAdamasRc**)malloc(sizeof(PyAdamasRc*) * gc_set_len);
        if (!gc_work_buf) panic_c("out of memory");
        gc_work_buf_cap = gc_set_len;
    }
    gc_work = gc_work_buf;
    gc_work_len = 0;
    gc_tab_build();
    if (t0) t1 = gc_now_ms();

    gc_mark_sweep = major;
    if (major) {
        // 整堆收集沿用经典标记-清扫：根是全局、临时栈、帧记录登记区和寄存器，
        // 不做试探减计数，少两遍边遍历。
        for (int64_t i = 0; i < gc_n_globals; ++i)
            gc_mark((uintptr_t)*(void**)gc_globals[i]);
        for (int64_t i = 0; i < temp_len; ++i) gc_mark((uintptr_t)temp_stack[i]);
        gc_scan_frames();
        gc_scan_registers();
        if (t0) t2 = gc_now_ms();
        gc_drain();
        if (t0) t3 = gc_now_ms();
        if (t0) t4 = gc_now_ms();
    } else {
        // 2. 试探减计数：集合内对象之间的出边全部减掉。
        gc_phase = GC_PHASE_TRIAL;
        for (size_t i = 0; i < gc_set_len; ++i) {
            PyAdamasRc* h = gc_set[i];
            if (h->release) h->release((char*)h + PYADAMAS_RC_HDR);
        }
        if (t0) t2 = gc_now_ms();

        // 3. 根：减完仍 > 0 的说明被集合外引用；再保守扫帧记录和寄存器兜住
        //    unknown 局部槽这类「借来的」引用（它们不计入 rc）。exact-RC 模式下
        //    所有引用都归某个槽/临时值/容器所有，真实 rc 已经够用。
        for (size_t i = 0; i < gc_set_len; ++i) {
            PyAdamasRc* h = gc_set[i];
            if (h->rc > 0 && !(h->marked & PYADAMAS_RC_MARK)) {
                h->marked |= PYADAMAS_RC_MARK;
                gc_work[gc_work_len++] = h;
            }
        }
        if (!gc_untracked_leaves) {
            gc_scan_frames();
            gc_scan_registers();
        }
        gc_drain();
        if (t0) t3 = gc_now_ms();

        // 4. 恢复存活对象的出边计数。
        gc_phase = GC_PHASE_RESTORE;
        for (size_t i = 0; i < gc_set_len; ++i) {
            PyAdamasRc* h = gc_set[i];
            if ((h->marked & PYADAMAS_RC_MARK) && h->release)
                h->release((char*)h + PYADAMAS_RC_HDR);
        }
        if (t0) t4 = gc_now_ms();
    }

    // 5. 清扫：先让垃圾对象把「指向集合外的边」交出来，再统一释放。
    gc_phase = GC_PHASE_SWEEP;
    gc_sweeping = 1;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (!(h->marked & PYADAMAS_RC_MARK) && h->release)
            h->release((char*)h + PYADAMAS_RC_HDR);
    }
    int64_t freed = 0;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (h->marked & PYADAMAS_RC_MARK) continue;
        if (rc_stats_on) stat_gc_freed++;
        freed++;
        gc_free_body(h);
        /* 释放过的槽清空：后面的晋升阶段还要遍历这张表，而这里的 free
           可能把整页还给系统（Windows 的堆管理器会直接 decommit），
           再去读 h->marked 就是访问已释放内存。放在这里清空，晋升阶段
           只需要判空，语义上与「marked 里没有 MARK」完全等价。 */
        gc_set[i] = NULL;
    }
    gc_sweeping = 0;
    gc_phase = GC_PHASE_NONE;
    gc_mark_sweep = 0;
    if (t0) t5 = gc_now_ms();

    // 6. 晋升：本轮存活的新生代变成老生代。被清扫掉的槽已经是 NULL，
    // 这里不能再解引用它们。
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (!h) continue;
        h->marked = 0;
        old_bytes += h->size + PYADAMAS_RC_HDR;
    }
    // 7. 清扫期攒下的老对象引用，现在按正常 RC 释放（可能级联）。
    for (size_t i = 0; i < gc_pend_len; ++i) pa_rc_dec(gc_pend[i]);
    gc_pend_len = 0;
    if (t0) t6 = gc_now_ms();

    size_t n_set = gc_set_len;
    gc_set = NULL;
    gc_set_len = 0;
    gc_tab = NULL;
    gc_tab_cap = 0;
    gc_work = NULL;
    gc_work_len = 0;
    stat_gc_live = old_bytes;
    alloc_since_gc = 0;
    if (major) {
        alloc_since_major = 0;
        // major 比 minor 稀疏：老生代翻倍（至少 8MB）才全扫一次，含环的
        // 老对象最多多留一个 major 周期。
        int64_t mt = old_bytes * 2;
        if (mt < 4 * (int64_t)PYADAMAS_GC_THRESHOLD)
            mt = 4 * (int64_t)PYADAMAS_GC_THRESHOLD;
        major_threshold = mt;
    }
    if (t0)
        fprintf(stderr,
                "[gc] %s n=%zu freed=%lld live=%lld tab=%.2f trial=%.2f mark=%.2f "
                "restore=%.2f sweep=%.2f pend=%.2f ms\n",
                major ? "major" : "minor", n_set, (long long)freed,
                (long long)old_bytes, t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4,
                t6 - t5);
}

void pa_gc_collect(void) { gc_collect(1); }

// 程序退出：所有还活着的堆对象（包括环）一次性还给系统。
void pa_gc_shutdown(void) {
    gc_sweeping = 1;
    while (heap_head) gc_free_body(heap_head);
    gc_sweeping = 0;
    alloc_since_gc = 0;
    alloc_since_major = 0;
    old_bytes = 0;
    young_len = 0;
    free(young_buf);
    young_buf = NULL;
    young_cap = 0;
    free(gc_globals);
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    free(gc_work_buf);
    gc_work_buf = NULL;
    gc_work_buf_cap = 0;
    free(gc_set_buf);
    gc_set_buf = NULL;
    gc_set_buf_cap = 0;
    free(gc_tab_buf);
    gc_tab_buf = NULL;
    gc_tab_buf_cap = 0;
    free(gc_pend);
    gc_pend = NULL;
    gc_pend_len = 0;
    gc_pend_cap = 0;
}

/* 分模块编译：init 是模块顶层生成的 void(void)，ready 是模块 .bss 里的
   一次性标志。标志在调用前设置，初始化失败不会重试（与 Python 移除
   失败模块的行为不同，但至少不会死循环）。 */
void pa_import_module(int64_t* ready, void (*init)(void)) {
    if (!ready || !init || *ready) return;
    *ready = 1;
    init();
}

