// rt_list.c —— 列表、排序与归约
#include "rt_internal.h"

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

/* ── 列表 ─────────────────────────────────────────────────────── */

/* 列表缓冲池（0.301 扩容）：data 缓冲、heap_bits 位图、排序临时区共用，
   按 64 字节分档，覆盖 <=8192 字节（1024 元素）。小档（<=512B）每档最多
   缓存 64 块；大档（>512B）每档最多 8 块，防囤内存。空块的头 8 字节存
   下一块的指针；复用时容量只会变大，cap_io 回写真实容量，调用方按 cap
   使用。请求档为空时向上找第一个非空档（128 位非空位图，O(1)），找不到
   才 malloc。块内容不携带语义：data 只读 [0,len)（先写后读），位图由
   list_bits_reserve 复制旧字 + 清零新字，排序临时区先 memcpy 再读。
   GC 清扫（gc_free_body）不经池、直接 free；shutdown 释放所有档。 */
#define LIST_BUF_MAX 8192
#define LIST_BUF_ALIGN 64
#define LIST_BUF_CLASSES (LIST_BUF_MAX / LIST_BUF_ALIGN)
#define LIST_BUF_MAX_PER_CLASS 64
#define LIST_BUF_MAX_PER_CLASS_BIG 8
/* 大档起点：bytes > 512 即档号 >= 8。 */
#define LIST_BUF_BIG_CLASS (512 / LIST_BUF_ALIGN)
static int64_t* list_buf_pool[LIST_BUF_CLASSES];
static int list_buf_count[LIST_BUF_CLASSES];
/* 位 i = 1 表示档 i 非空。向上取档用 ctz 一次定位，避免空池线性扫。 */
static uint64_t list_buf_avail[LIST_BUF_CLASSES / 64];

static int list_buf_class(int64_t bytes) {
    if (bytes <= 0 || bytes > LIST_BUF_MAX) return -1;
    return (int)((bytes + LIST_BUF_ALIGN - 1) / LIST_BUF_ALIGN) - 1;
}

static int list_buf_class_limit(int c) {
    return c < LIST_BUF_BIG_CLASS ? LIST_BUF_MAX_PER_CLASS : LIST_BUF_MAX_PER_CLASS_BIG;
}

/* 第一个 >= c 的非空档；没有返回 -1。 */
static int list_buf_find_up(int c) {
    for (int w = c >> 6; w < LIST_BUF_CLASSES / 64; ++w) {
        uint64_t m = list_buf_avail[w];
        if (w == (c >> 6)) m &= ~UINT64_C(0) << (c & 63);
        if (m) return (w << 6) + __builtin_ctzll(m);
    }
    return -1;
}

static int64_t* list_buf_get(int64_t* cap_io) {
    int64_t cap = *cap_io < 4 ? 4 : *cap_io;
    int64_t bytes = cap * (int64_t)sizeof(int64_t);
    int c = list_buf_class(bytes);
    if (c >= 0) {
        // 请求档为空时向上取档：容量只会变大，cap_io 回写真实容量。
        if (!list_buf_pool[c]) {
            int f = list_buf_find_up(c);
            if (f >= 0) c = f;
        }
        // 同一档的块一律按档容量分配，复用后 cap 与真实容量严格一致。
        int64_t class_bytes = (int64_t)(c + 1) * LIST_BUF_ALIGN;
        int64_t* p;
        if (list_buf_pool[c]) {
            p = list_buf_pool[c];
            list_buf_pool[c] = (int64_t*)p[0];
            if (--list_buf_count[c] == 0)
                list_buf_avail[c >> 6] &= ~(UINT64_C(1) << (c & 63));
        } else {
            p = (int64_t*)malloc((size_t)class_bytes);
            if (!p) panic_c("out of memory");
        }
        *cap_io = class_bytes / (int64_t)sizeof(int64_t);
        return p;
    }
    *cap_io = cap;
    int64_t* p = (int64_t*)malloc((size_t)bytes);
    if (!p) panic_c("out of memory");
    return p;
}

static void list_buf_put(int64_t* p, int64_t cap) {
    if (!p) return;
    int64_t bytes = cap * (int64_t)sizeof(int64_t);
    int c = list_buf_class(bytes);
    if (c < 0 || list_buf_count[c] >= list_buf_class_limit(c)) {
        free(p);
        return;
    }
    p[0] = (int64_t)list_buf_pool[c];
    list_buf_pool[c] = p;
    list_buf_count[c]++;
    list_buf_avail[c >> 6] |= UINT64_C(1) << (c & 63);
}

/* 增长 data 缓冲到至少 ncap 个元素（0.301：池内尺寸先试池）。
   内联缓冲（合并分配，见 rt_internal.h）是头块内部的指针，realloc/free
   都非法 —— 内联区从此闲置，头块销毁时仍整块挂回 list_inline_pool 复用。
   <=LIST_BUF_MAX：list_buf_get 取新块（可能向上取档，cap 回写真实容量）
   + memcpy 旧内容 + 旧外部块 list_buf_put 归还。sort_struct 第二轮起首次
   增长就弹出上一轮销毁归还的大块，cap 一次到位，整轮零增长零 malloc。
   >LIST_BUF_MAX：池外大块保留原路径（内联换 malloc+memcpy，外部 realloc）。
   调用方负责随后的 heap_bits 扩容 —— 必须按增长后的 l->cap（可能被池
   抬高到 > ncap），不能按 ncap。 */
int64_t* list_buf_grow(PyAdamasList* l, int64_t ncap) {
    int64_t* nd;
    if (ncap * (int64_t)sizeof(int64_t) <= LIST_BUF_MAX) {
        int64_t gcap = ncap;
        nd = list_buf_get(&gcap);
        memcpy(nd, l->data, sizeof(int64_t) * (size_t)l->cap);
        if (l->data != list_inline_at(l)) list_buf_put(l->data, l->cap);
        l->cap = gcap;
    } else if (l->data == list_inline_at(l)) {
        nd = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        memcpy(nd, l->data, sizeof(int64_t) * (size_t)l->cap);
        l->cap = ncap;
    } else {
        nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->cap = ncap;
    }
    l->data = nd;
    return nd;
}

void list_destroy(void* p) {
    PyAdamasList* l = (PyAdamasList*)p;
    // kind 3 按约定是空列表；真出现元素（不变量被破坏）时按 2 保守释放。
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    if (kind == 1) {
        // 全堆元素：批量 dec（内联快路径 + untracked 元组释放）。
        pa_rc_dec_heap_batch(l->data, l->len);
    } else if (kind != 0)
        for (int64_t i = 0; i < l->len; ++i) {
            // kind 2 且位图可用时，按位图判断，跳过全局索引查询。
            if (kind == 2 && l->heap_bits) {
                if ((l->heap_bits[i >> 6] >> (i & 63)) & 1)
                    pa_rc_dec((void*)(uintptr_t)l->data[i]);
            } else {
                pa_own_dec(kind, l->data[i]);
            }
        }
    if (l->heap_bits) {
        list_buf_put((int64_t*)l->heap_bits, l->bits_cap);
        l->heap_bits = NULL;
        l->bits_cap = 0;
    }
    // 内联缓冲随头块一起挂回 list_inline_pool（pa_rc_dec_slow 按 size 路由），
    // 这里只还外部缓冲。非合并块不可能有 data == l+1：那个地址落在头块
    // 自己的 malloc chunk 内部，分配器不会把它交给别的块。
    if (l->data != list_inline_at(l)) list_buf_put(l->data, l->cap);
}

// GC 清扫前调用：只把「指向可达对象」的引用计数减掉。kind 2 的元素走
// 注册表判断（整数不会命中），不再整体跳过。
void list_gc_release(void* p) {
    PyAdamasList* l = (PyAdamasList*)p;
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    if (kind == 0 || !l->data) return;
    for (int64_t i = 0; i < l->len; ++i) {
        if (kind == 2 && l->heap_bits) {
            if ((l->heap_bits[i >> 6] >> (i & 63)) & 1)
                pa_gc_unref_child((void*)(uintptr_t)l->data[i]);
        } else {
            pa_own_gc_unref(kind, l->data[i]);
        }
    }
}

PyAdamasList* pa_list_new(int64_t cap, int64_t elem_kind) {
    if (cap < 0) panic_c("negative list capacity");
    if (elem_kind < 0 || elem_kind > 3) panic_c("invalid list element kind");
    if (cap < 4) cap = 4;
    PyAdamasList* l;
    if (cap <= PYADAMAS_LIST_INLINE_CAP) {
        // 合并分配：头 + 8 元素缓冲一次池操作（原来 list_pool_get +
        // list_buf_get 两次）。queue_sim 每步建/销各一个列表，这是热路径。
        l = (PyAdamasList*)rc_alloc_ex((int64_t)sizeof(PyAdamasList), list_destroy,
                                       list_gc_release, PYADAMAS_RC_LIST,
                                       PYADAMAS_LIST_INLINE_BYTES);
        l->cap = PYADAMAS_LIST_INLINE_CAP;
        l->data = list_inline_at(l);
    } else {
        l = (PyAdamasList*)rc_alloc((int64_t)sizeof(PyAdamasList), list_destroy,
                                    list_gc_release, PYADAMAS_RC_LIST);
        l->cap = cap;
        l->data = list_buf_get(&l->cap);
    }
    l->len = 0;
    l->elem_kind = elem_kind;
    l->heap_bits = NULL;
    l->bits_cap = 0;
    return l;
}
/* 一次调用构造「定长、无位图」列表/元组。
 *
 * `tuple.new{a, b}` 现在编译成 pa_list_new + N 次 pa_list_append，也就是
 * N+1 次函数调用，每次 append 还要走位图维护和容量检查。实测 queue_sim
 * 每次元组分配 265ns，其中函数调用开销占大头。
 *
 * 这里给「元素全是整型机器字（无浮点、无堆引用）」的小元组开一条快路径：
 * 一次调用、一次缓冲分配、直接填 data、完全跳过 heap_bits（kind 0/1 的
 * 位图本来就没人在读）。
 *
 * 约束（由 codegen 保证，运行时不校验）：
 *   - n <= 4，元素已按值展开在通用寄存器里；
 *   - elem_kind 只能是 0（全非堆）或 1（全堆）；
 *   - 调用方负责这些引用的所有权转移（和 pa_list_append 语义一致）。
 */
PyAdamasList* pa_list_new_filled(int64_t n, int64_t elem_kind, int64_t a, int64_t b,
                                  int64_t c, int64_t d) {
    if (n < 0 || n > 4) panic_c("bad filled list size");
    PyAdamasList* l = (PyAdamasList*)rc_alloc_ex((int64_t)sizeof(PyAdamasList), list_destroy,
                                               list_gc_release, PYADAMAS_RC_LIST,
                                               PYADAMAS_LIST_INLINE_BYTES);
    l->len = n;
    l->cap = PYADAMAS_LIST_INLINE_CAP;
    l->elem_kind = elem_kind;
    l->heap_bits = NULL;
    l->bits_cap = 0;
    l->data = list_inline_at(l);
    l->data[0] = a;
    if (n > 1) l->data[1] = b;
    if (n > 2) l->data[2] = c;
    if (n > 3) l->data[3] = d;
    return l;
}

/* 定长元组构造：与 pa_list_new_filled 同形，但 elem_kind == 0 时**不进
 * GC 堆链**。
 *
 * 依据：元组不可变（元素写成后不会再变），elem_kind == 0 表示所有元素
 * 都是非堆值 —— 于是对象没有任何出边，不可能处于引用环里，GC 完全不必
 * 看到它。RC 归零时 list_destroy 照常释放 data 缓冲。
 *
 * 实测 bench/realistic/queue_sim.py：每次 simulate 分配约 66 个
 * tuple[int, int]，rc_link/rc_unlink 合计占 profile 的 7.9%。 */
PyAdamasList* pa_tuple_new_filled(int64_t n, int64_t elem_kind, int64_t a, int64_t b,
                                  int64_t c, int64_t d) {
    if (n < 0 || n > 4) panic_c("bad filled tuple size");
    PyAdamasList* l;
    if (elem_kind == 0) {
        l = (PyAdamasList*)rc_alloc_untracked_ex((int64_t)sizeof(PyAdamasList), list_destroy,
                                               list_gc_release, PYADAMAS_RC_LIST,
                                               PYADAMAS_LIST_INLINE_BYTES);
    } else {
        l = (PyAdamasList*)rc_alloc_ex((int64_t)sizeof(PyAdamasList), list_destroy,
                                      list_gc_release, PYADAMAS_RC_LIST,
                                      PYADAMAS_LIST_INLINE_BYTES);
    }
    l->len = n;
    l->cap = PYADAMAS_LIST_INLINE_CAP;
    l->elem_kind = elem_kind;
    l->heap_bits = NULL;
    l->bits_cap = 0;
    l->data = list_inline_at(l);
    l->data[0] = a;
    if (n > 1) l->data[1] = b;
    if (n > 2) l->data[2] = c;
    if (n > 3) l->data[3] = d;
    return l;
}

/* 列表元素类别定型。kind 3 的空列表在这里采用第一个写入值的种类；静态
   种类和值冲突时降级为 2（混合，销毁走注册表）。vk 是值的静态种类，
   unknown 时只能运行期探测一次。owned 非 0 表示引用已经转给容器。
   0.303：探针改用共享的 pa_unknown_is_heap（与 dec_unknown 逐位一致，
   含 exact-RC 回退 —— 旧 list_probe 只查 rc_index，exact-RC 模式下索引
   关闭，kind0 列表存 unknown 堆值会漏升级）。0→2 升级补 GC 链：kind0
   小元组是 untracked 分配，有了堆出边就必须让 GC 看到。 */
void pa_list_prep(PyAdamasList* l, int64_t v, int64_t vk, int64_t owned) {
    int kind = (int)l->elem_kind;
    if (kind == 3) {
        kind = vk == 2 ? pa_unknown_is_heap(v) : (int)vk;
        l->elem_kind = kind;
    } else if (vk == 2) {
        /* 0.303：kind2 无需探针（种类恒 2）；冲突才升级，track 放冷分支 ——
           热路径（kind1 存堆 / kind0 存标量）与 0.302 逐指令同形态，
           无条件升级写法会让 gcc 给整个函数加 push/spill（str_build 教训）。 */
        if (kind != 2 && (kind == 1) != (pa_unknown_is_heap(v) != 0)) {
            if (kind == 0) pa_rc_track(l);
            kind = 2;
            l->elem_kind = 2;
        }
    } else if (kind != vk) {
        if (kind == 0) pa_rc_track(l);
        kind = 2;
        l->elem_kind = 2;
    }
    if (!owned) pa_own_inc(kind, v);
}

/* 元素引用由生成代码在写入前 retain（kind 1/2 且源类型已知是堆），
   运行时不再重复 inc；kind 1 覆盖/截断/销毁时负责 release。 */
/* 位图容量（字数）。单独记，因为 l->cap 会被 list_buf_get 抬高到池块容量，
   与位图实际分配的字数不再一致。0.301：位图块也走缓冲池（uint64_t 字数组，
   字数即 8 字节单位 cap）。bits_cap 记池块真实字数；复用块的旧位内容不
   携带语义 —— 增长复制旧字 [0,owords)，新字 [owords,bcap) 全部清零（池块
   陈旧内容的清零范围比旧实现的 [owords,nwords) 只大不小）；新建列表首次
   位图 owords==0，整块清零，与旧 realloc(NULL)+清零 语义逐位一致。 */
static void list_bits_reserve(PyAdamasList* l, int64_t ncap) {
    int64_t nwords = (ncap + 63) >> 6;
    int64_t owords = l->bits_cap;
    if (nwords <= owords) return;
    int64_t bcap = nwords;
    uint64_t* nb = (uint64_t*)list_buf_get(&bcap);
    if (l->heap_bits) {
        memcpy(nb, l->heap_bits, sizeof(uint64_t) * (size_t)owords);
        list_buf_put((int64_t*)l->heap_bits, owords);
    }
    for (int64_t i = owords; i < bcap; ++i) nb[i] = 0;
    l->heap_bits = nb;
    l->bits_cap = bcap;
}

/* 0.303：丢弃逐元素位图，回退注册表判断（销毁/GC 释放的 kind2 无位图
   分支）。位图块归还缓冲池。用于无法逐位维护的写入/重排路径。 */
void pa_list_drop_bits(PyAdamasList* l) {
    if (l->heap_bits) {
        list_buf_put((int64_t*)l->heap_bits, l->bits_cap);
        l->heap_bits = NULL;
        l->bits_cap = 0;
    }
}

/* 0.303：下标类写入（set/insert）的种类/位图维护。heap 是新值静态种类
   （0/1/2），2 才探针 —— 已知标量绝不探针：大整数/浮点位模式可能恰好等于
   某个存活堆地址，探针会误判成堆。kind 冲突升级 2（0→2 补 GC 链）；
   kind2 带位图按新值堆性更新第 i 位；kind2 无位图无需动作（销毁走注册表）。
   不做 retain/release —— 新值引用由降级器/调用方在写入前 retain，旧值
   release 由调用方按**写入前**的 kind 做。 */
void pa_list_store_kind(PyAdamasList* l, int64_t i, int64_t v, int64_t heap) {
    int kind = (int)l->elem_kind;
    if (kind == 3) { l->elem_kind = 2; kind = 2; }  /* 防御：kind3 只应是空表 */
    if (kind == 2) {
        if (l->heap_bits) {
            int vh = heap == 2 ? pa_unknown_is_heap(v) : (int)heap;
            uint64_t m = (uint64_t)1 << (i & 63);
            if (vh) l->heap_bits[i >> 6] |= m;
            else l->heap_bits[i >> 6] &= ~m;
        }
        return;
    }
    int vh = heap == 2 ? pa_unknown_is_heap(v) : (int)heap;
    if (kind != vh) {
        l->elem_kind = 2;
        if (kind == 0) pa_rc_track(l);
    }
}

/* 追加元素。
 *
 * heap 参数由 codegen 按**写入点的静态类型**给出：
 *   0 = 确定非堆（int / float / bool / None）
 *   1 = 确定堆（str / list / dict / tuple / 实例 …）
 *   2 = unknown，静态不可知
 *
 * 前两种情况下逐元素位图是精确的，销毁时不必查全局索引；
 * 一旦出现 2，整个列表退回 heap_bits == NULL 的旧路径（正确性优先）。
 *
 * 0.303：种类维护。静态容器类型是 unknown 时降级器不调 prep，运行期
 * kind 可能与写入值冲突（kind1 列表 append 标量）；kind3 空表也要在这里
 * 定型（同 prep）。热路径（kind0+heap0 / kind1+heap1 / kind2）只多一次
 * kind 装载 + 两次比较，不探针、不写内存。 */
void pa_list_append(PyAdamasList* l, int64_t v, int64_t heap) {
    pa_gc_insert_barrier(v);   /* 插入屏障：标记期新元素标灰（0.330） */
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        list_buf_grow(l, ncap);
        // 按增长后的真实 cap 扩位图：池可能把 cap 抬高到 > ncap。
        if (l->heap_bits) list_bits_reserve(l, l->cap);
    }
    int kind = (int)l->elem_kind;
    if (kind != 2 && kind != heap) {
        int vh = heap == 2 ? pa_unknown_is_heap(v) : (int)heap;
        if (kind == 3) {
            l->elem_kind = vh;
        } else if (kind != vh) {
            l->elem_kind = 2;
            if (kind == 0) pa_rc_track(l);
        }
    }
    // 维护位图：一旦遇到 unknown，放弃逐元素跟踪。
    if (heap == 2) {
        if (l->heap_bits) {
            list_buf_put((int64_t*)l->heap_bits, l->bits_cap);
            l->heap_bits = NULL;
            l->bits_cap = 0;
        }
    } else if (l->len == 0 && l->elem_kind == 2) {
        // **只对 kind==2 的列表分配位图。**
        //
        // 位图只在销毁/GC 释放时被读，而那两个地方都是
        // `if (kind == 2 && l->heap_bits)` —— kind 0/1 永远不读。
        // 原实现无条件分配，于是 `tuple[int, int]`（kind=0）每次创建
        // 都要多一次 realloc + 清零，而这份位图从头到尾没人看。
        //
        // 实测 queue_sim 每次元组分配 265ns，其中位图这条路占一部分。
        list_bits_reserve(l, l->cap);
    }
    if (l->heap_bits) {
        uint64_t* w = &l->heap_bits[l->len >> 6];
        uint64_t m = (uint64_t)1 << (l->len & 63);
        if (heap == 1) *w |= m;
        else *w &= ~m;
    }
    l->data[l->len++] = v;
}

/* 切片拷贝惯用法：for k in range(lo, hi): dst.append(src[k]) 的批量版。
   降级器识别后一次调用替代整个循环（省逐元素 append 调用 + 容量检查 +
   生成代码循环开销）。语义与逐元素 prep+append 完全一致：

   - 快路径（0 <= lo <= hi <= src->len）：一次扩容 + memcpy；
     kind 0/heap 0 纯值拷贝，kind 1/heap 1 拷贝后批量 retain；
     其余种类（未定 3 / 混合 2 / 静态与容器冲突）逐元素 prep+append，
     定型、位图、保守 retain 全走原路径。
   - 慢路径：逐元素 pa_list_get（负索引归一化、越界抛 IndexError，
     已 append 的部分保留）——和原循环的异常行为逐位一致。
   - dst == src 安全：快路径 hi <= len 保证源区间整体在目标写入起点
     之下，memcpy 不重叠；扩容在取指针之前做；逐元素分支每轮重读
     src->data（append 可能 realloc）。
   - heap 是源元素静态种类（0/1/2），和 pa_list_append 的同名参数一致。 */
void pa_list_copy_range(PyAdamasList* dst, PyAdamasList* src, int64_t lo, int64_t hi,
                        int64_t heap) {
    if (lo >= 0 && lo <= hi && hi <= src->len) {
        int64_t n = hi - lo;
        if (n == 0) return;
        if (dst->len + n > dst->cap) {
            int64_t ncap = dst->cap ? dst->cap : 4;
            while (ncap < dst->len + n) ncap *= 2;
            list_buf_grow(dst, ncap);
            if (dst->heap_bits) list_bits_reserve(dst, dst->cap);
        }
        int kind = (int)dst->elem_kind;
        /* 0.303：快路径还要求源的**运行期**种类与静态 heap 一致 ——
           unknown 别名能让 static list[str] 的变量装着 kind2 混合表，
           此时 memcpy+批量 inc 会把标量当堆指针。不一致落逐元素慢路径。 */
        if (kind == 0 && heap == 0 && src->elem_kind == 0) {
            memcpy(dst->data + dst->len, src->data + lo, (size_t)n * sizeof(int64_t));
            dst->len += n;
        } else if (kind == 1 && heap == 1 && src->elem_kind == 1) {
            int64_t* d = dst->data + dst->len;
            memcpy(d, src->data + lo, (size_t)n * sizeof(int64_t));
            for (int64_t i = 0; i < n; ++i) pa_rc_inc((void*)(uintptr_t)d[i]);
            dst->len += n;
        } else {
            for (int64_t i = 0; i < n; ++i) {
                int64_t v = src->data[lo + i];
                pa_list_prep(dst, v, heap, 0);
                pa_list_append(dst, v, heap);
            }
        }
        return;
    }
    for (int64_t k = lo; k < hi; ++k) {
        int64_t v = pa_list_get(src, k);
        pa_list_prep(dst, v, heap, 0);
        pa_list_append(dst, v, heap);
    }
}

int64_t pa_list_get(PyAdamasList* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) pa_raise_c("IndexError", "list index out of range");
    return l->data[i];
}

/* 0.303：写入时维护 elem_kind / heap_bits（pa_list_store_kind）。旧值按
   **写入前**的 kind 释放；新值引用由调用方在写入前 retain（降级器）。
   heap 是新值静态种类，见 rt_internal.h 的说明。 */
void pa_list_set(PyAdamasList* l, int64_t i, int64_t v, int64_t heap) {
    pa_gc_insert_barrier(v);   /* 插入屏障：标记期新值标灰（0.330） */
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) pa_raise_c("IndexError", "list assignment index out of range");
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    int64_t old = l->data[i];
    l->data[i] = v;
    pa_list_store_kind(l, i, v, heap);
    pa_own_dec(kind, old);
}

void pa_list_set_len(PyAdamasList* l, int64_t n) {
    if (n < 0 || n > l->len) panic_c("invalid list length");
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    for (int64_t i = n; i < l->len; ++i) pa_own_dec(kind, l->data[i]);
    l->len = n;
}

/* 把 src 的元素接到 dst 末尾，供 *args 打包和切片拷贝使用。
   所有权统一走 pa_list_prep：目标未定型（3）时采用源的种类，与源冲突时
   降级为 2（销毁走注册表），元素引用按目标种类 retain。 */
void pa_list_extend(PyAdamasList* dst, PyAdamasList* src) {
    int64_t slen = src->len;
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    if (dst == src) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    int sk = (int)src->elem_kind;
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        // 源是未定型（3）时只可能是空表，这里统一按 unknown 传给 prep。
        pa_list_prep(dst, v, sk == 3 ? 2 : sk, 0);
        // v 来自源列表，类型动态：按源 kind 决定。
        pa_list_append(dst, v, sk == 1 ? 1 : (sk == 0 ? 0 : 2));
    }
    free(snap);
}

/* `l *= n`：原地重复。CPython 的 list.__imul__ 改的是同一个对象
   （`a = l; l *= 2` 之后 a 也变长），所以不能走 pa_list_repeat + 重新
   赋值。元素引用按元素种类给新增位置补一份；n<=0 清空。 */
void pa_list_inplace_repeat(PyAdamasList* l, int64_t n) {
    int64_t len = l->len;
    if (n <= 0) {
        pa_list_set_len(l, 0);
        return;
    }
    if (len == 0 || n == 1) return;
    if (len > INT64_MAX / n) panic_c("list repeat: result too large");
    int64_t total = len * n;
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    if (total > l->cap) list_buf_grow(l, total);
    for (int64_t rep = 1; rep < n; ++rep) {
        int64_t base = rep * len;
        for (int64_t i = 0; i < len; ++i) {
            int64_t v = l->data[i];
            pa_own_inc(kind, v);
            l->data[base + i] = v;
        }
    }
    l->len = total;
    /* 位图（kind2 的逐元素跟踪）重复后无法逐位对应，保守丢弃回退注册表。 */
    if (kind == 2) pa_list_drop_bits(l);
}

PyAdamasList* pa_list_copy(PyAdamasList* src) {
    PyAdamasList* l = pa_list_new(src->len, src->elem_kind);
    pa_list_extend(l, src);
    return l;
}

PyAdamasList* pa_list_reversed(PyAdamasList* src) {
    int64_t n = src->len;
    PyAdamasList* l = pa_list_new(n, src->elem_kind);
    for (int64_t i = n - 1; i >= 0; --i) {
        int64_t v = src->data[i];
        if (l->elem_kind != 0) pa_own_inc(src->elem_kind, v);
        // 同上：源元素类型动态。
        pa_list_append(l, v, src->elem_kind == 1 ? 1 : (src->elem_kind == 0 ? 0 : 2));
    }
    return l;
}

/* 0.303：insert 与 set 同族 —— 维护种类（heap 是新值静态种类），且插入
   会整体右移元素，kind2 位图无法逐位跟随，保守丢弃回退注册表。 */
void pa_list_insert(PyAdamasList* l, int64_t i, int64_t v, int64_t heap) {
    if (i < 0) i += l->len;
    if (i < 0) i = 0;
    if (i > l->len) i = l->len;
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        list_buf_grow(l, ncap);
    }
    if (l->elem_kind == 2) pa_list_drop_bits(l);
    pa_list_store_kind(l, i, v, heap);
    if (i < l->len)
        memmove(l->data + i + 1, l->data + i, sizeof(int64_t) * (size_t)(l->len - i));
    l->data[i] = v;
    ++l->len;
}

void pa_list_reverse(PyAdamasList* l) {
    /* 0.303：反转交换元素位置，kind2 位图无法跟随，保守丢弃。 */
    if (l->elem_kind == 2) pa_list_drop_bits(l);
    for (int64_t i = 0, j = l->len - 1; i < j; ++i, --j) {
        int64_t t = l->data[i];
        l->data[i] = l->data[j];
        l->data[j] = t;
    }
}

int64_t pa_int_bit_length(int64_t v) {
    uint64_t x = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int64_t n = 0;
    while (x) {
        ++n;
        x >>= 1;
    }
    return n;
}

/* ── 排序：Timsort ────────────────────────────────────────────
   和 CPython 的 list.sort / sorted 同款算法：稳定、自适应。
   - 先找自然 run（降序的就地反转）；
   - 短 run 用二分插入排序补到 minrun；
   - 按栈不变式合并相邻 run，合并时复制较短的一半到临时区。
   近乎有序的数据（真实代码里最常见）基本不触发归并，退化成 O(n)。
   稳定性：相等元素保持原相对顺序，和 Python 一致。 */

/* float 按位模式存在 int64_t 槽里，用 memcpy 取出来比较：编译期认得这个
   惯用法，不会真的搬内存，也避开了 (double*) 强转的严格别名问题。 */
static inline double f64_of(int64_t v) {
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
}

#define PYADAMAS_TIMSORT(NAME, LESS)                                               \
    /* 二分插入排序：把 a[lo,hi) 排好，稳定。 */                                \
    static void bin_ins_##NAME(int64_t* a, int64_t lo, int64_t hi) {           \
        for (int64_t i = lo + 1; i < hi; ++i) {                                \
            int64_t v = a[i];                                                  \
            int64_t left = lo, right = i;                                      \
            while (left < right) {                                             \
                int64_t m = left + (right - left) / 2;                         \
                if (LESS(v, a[m])) right = m;                                  \
                else left = m + 1;                                             \
            }                                                                  \
            for (int64_t j = i; j > left; --j) a[j] = a[j - 1];                \
            a[left] = v;                                                       \
        }                                                                      \
    }                                                                          \
    /* 从 lo 起找一段自然 run；降序的就地反转，返回 run 长度。 */                 \
    static int64_t count_run_##NAME(int64_t* a, int64_t n, int64_t lo) {       \
        if (lo + 1 >= n) return 1;                                             \
        int64_t hi = lo + 1;                                                   \
        if (LESS(a[hi], a[lo])) {                                              \
            while (hi + 1 < n && LESS(a[hi + 1], a[hi])) ++hi;                 \
            for (int64_t i = lo, j = hi; i < j; ++i, --j) {                    \
                int64_t t = a[i];                                              \
                a[i] = a[j];                                                   \
                a[j] = t;                                                      \
            }                                                                  \
            return hi - lo + 1;                                                \
        }                                                                      \
        while (hi + 1 < n && !LESS(a[hi + 1], a[hi])) ++hi;                    \
        return hi - lo + 1;                                                    \
    }                                                                          \
    /* 归并 [lo,mid) 和 [mid,hi)：复制较短的一半，稳定。 */                      \
    static void merge_##NAME(int64_t* a, int64_t lo, int64_t mid, int64_t hi,   \
                             int64_t* tmp) {                                    \
        int64_t n1 = mid - lo, n2 = hi - mid;                                  \
        if (n1 <= n2) {                                                        \
            memcpy(tmp, a + lo, (size_t)n1 * sizeof(int64_t));                 \
            int64_t i = 0, j = mid, k = lo;                                    \
            while (i < n1 && j < hi) {                                         \
                if (LESS(a[j], tmp[i])) a[k++] = a[j++];                       \
                else a[k++] = tmp[i++];                                        \
            }                                                                  \
            while (i < n1) a[k++] = tmp[i++];                                  \
        } else {                                                               \
            memcpy(tmp, a + mid, (size_t)n2 * sizeof(int64_t));                \
            int64_t i = mid - 1, j = n2 - 1, k = hi - 1;                       \
            while (i >= lo && j >= 0) {                                        \
                if (LESS(tmp[j], a[i])) a[k--] = a[i--];                       \
                else a[k--] = tmp[j--];                                        \
            }                                                                  \
            while (j >= 0) a[k--] = tmp[j--];                                  \
        }                                                                      \
    }                                                                          \
    typedef struct {                                                           \
        int64_t base;                                                          \
        int64_t len;                                                           \
    } run_##NAME;                                                              \
    static void merge_at_##NAME(int64_t* a, run_##NAME* st, int* sp, int i,     \
                                int64_t* tmp) {                                \
        int64_t lo = st[i - 1].base;                                           \
        int64_t mid = st[i].base;                                              \
        int64_t hi = st[i].base + st[i].len;                                   \
        merge_##NAME(a, lo, mid, hi, tmp);                                     \
        st[i - 1].len = hi - lo;                                               \
        for (int j = i; j < *sp - 1; ++j) st[j] = st[j + 1];                   \
        --*sp;                                                                 \
    }                                                                          \
    /* minrun：让 run 数量接近 2 的幂，归并时长度尽量对半。 */                   \
    static int64_t minrun_##NAME(int64_t n) {                                  \
        int64_t r = 0;                                                         \
        while (n >= 64) {                                                      \
            r |= (n & 1);                                                      \
            n >>= 1;                                                           \
        }                                                                      \
        return n + r;                                                          \
    }                                                                          \
    static void sort_##NAME(int64_t* a, int64_t n) {                           \
        if (n < 2) return;                                                     \
        /* 小数组只有一个 run：找完自然序再补一段插入排序，不用临时区。 */        \
        if (n <= 64) {                                                         \
            count_run_##NAME(a, n, 0);                                         \
            bin_ins_##NAME(a, 0, n);                                           \
            return;                                                            \
        }                                                                      \
        int64_t mr = minrun_##NAME(n);                                         \
        /* 0.301：临时区入缓冲池。排序期间不执行用户代码：plain sort 的比较   \
           是运行时宏（LESS_STR/LESS_CMP 只调 pa_str_cmp/纯 C 描述符比较，见  \
           0.312 sort_desc_cmp）；sorted(key=) 的 key 在生成循环里预算好       \
           （lower_builtin_seq.cpp），ListSortedBy 只收 keys 列表；            \
           list.sort(key=) 不支持（lower_method_containers.cpp 报错）。        \
           get/put 是纯缓冲操作，无 RC/GC 副作用，即使重入也安全。OOM 走运行   \
           时统一 panic（与 list_buf_grow 一致），不再退化为插入排序。 */      \
        int64_t tcap = n / 2 + 1;                                              \
        int64_t* tmp = list_buf_get(&tcap);                                    \
        run_##NAME st[128];                                                    \
        int sp = 0;                                                            \
        int64_t lo = 0;                                                        \
        while (lo < n) {                                                       \
            int64_t r = count_run_##NAME(a, n, lo);                            \
            if (r < mr) {                                                      \
                int64_t hi = lo + mr;                                          \
                if (hi > n) hi = n;                                            \
                bin_ins_##NAME(a, lo, hi);                                     \
                r = hi - lo;                                                   \
            }                                                                  \
            st[sp].base = lo;                                                  \
            st[sp].len = r;                                                    \
            ++sp;                                                              \
            /* 栈不变式：长度单调递减，且相邻三个满足                        \
               len[i-2] > len[i-1] + len[i]。 */                               \
            for (;;) {                                                         \
                if (sp >= 3 &&                                                 \
                    st[sp - 3].len <= st[sp - 2].len + st[sp - 1].len) {       \
                    int i = (st[sp - 3].len < st[sp - 1].len) ? sp - 1 : sp - 2; \
                    merge_at_##NAME(a, st, &sp, i, tmp);                       \
                } else if (sp >= 2 && st[sp - 2].len <= st[sp - 1].len) {      \
                    merge_at_##NAME(a, st, &sp, sp - 1, tmp);                  \
                } else {                                                       \
                    break;                                                     \
                }                                                              \
            }                                                                  \
            lo += r;                                                           \
        }                                                                      \
        while (sp > 1) merge_at_##NAME(a, st, &sp, sp - 1, tmp);               \
        list_buf_put(tmp, tcap);                                               \
    }

#define LESS_I64(a, b) ((a) < (b))
#define LESS_F64(a, b) (f64_of(a) < f64_of(b))
#define LESS_STR(a, b) \
    (pa_str_cmp((PyAdamasStr*)(uintptr_t)(a), (PyAdamasStr*)(uintptr_t)(b)) < 0)

PYADAMAS_TIMSORT(i64, LESS_I64)
PYADAMAS_TIMSORT(f64, LESS_F64)
PYADAMAS_TIMSORT(str, LESS_STR)

/* ── 0.312：元组/容器元素的字典序排序 ─────────────────────────────
   此前元组落 kind 3（机器字比较）：比的是堆地址，池分配地址≈分配
   顺序≈原顺序，sort 看起来是 no-op（sort_struct / 204_list_buf_pool
   的顺序无关校验和长期掩盖了这个洞）。现在容器元素走 kind 4，编译
   器按静态类型附描述符（见 Lowerer::append_sort_desc）——自定界前
   缀树：
     i / f / s / x = int / float / str / 机器字；
     l + 子描述符 = list / 变长元组：逐元素字典序，前缀相等短者小；
     t + arity 字节 + 逐位置描述符 = 定长元组：逐位置比。
   描述符是编译期串常量（永生），无 RC/GC 问题；比较是纯 C（pa_str_cmp
   不分配），排序期间不执行用户代码，与上面宏注释的纪律一致。 */

static const uint8_t* g_cmp_desc = 0;  /* plain sort 描述符，单线程，同 g_sort_keys 模式 */

static int sort_word_cmp(int64_t a, int64_t b) { return (a > b) - (a < b); }

/* 跳过一个描述符（不比较），用于定位子描述符末尾。 */
static void sort_desc_skip(const uint8_t** pp) {
    int c = *(*pp)++;
    if (c == 'l') {
        sort_desc_skip(pp);
    } else if (c == 't') {
        int n = *(*pp)++;
        for (int i = 0; i < n; ++i) sort_desc_skip(pp);
    }
}

/* 按描述符三路比较。*pp 是游标：返回 0 时保证已消费完当前描述符；
   非 0 时整条比较逐层回退，游标状态不再重要。depth 防病态嵌套（超
   限退机器字，但仍消费描述符，保持外层游标对齐）。 */
static int sort_desc_cmp(int64_t a, int64_t b, const uint8_t** pp, int depth) {
    int c = *(*pp)++;
    if (c == 'f') {
        double x = f64_of(a), y = f64_of(b);
        return (x > y) - (x < y);  /* NaN 与一切相等，同 LESS_F64 口径 */
    }
    if (c == 's') {
        int64_t r = pa_str_cmp((PyAdamasStr*)(uintptr_t)a, (PyAdamasStr*)(uintptr_t)b);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if (c != 'l' && c != 't') return sort_word_cmp(a, b);  /* i / x / 防御 */
    int n = -1;                  /* t：arity；l：-1 */
    const uint8_t* child = *pp;  /* l：子描述符起点，逐元素复用 */
    if (c == 't') n = *(*pp)++;
    else sort_desc_skip(pp);
    if (depth >= 32 || !a || !b) {
        if (c == 't') for (int i = 0; i < n; ++i) sort_desc_skip(pp);
        return sort_word_cmp(a, b);
    }
    PyAdamasList* A = (PyAdamasList*)(uintptr_t)a;
    PyAdamasList* B = (PyAdamasList*)(uintptr_t)b;
    int64_t la = A->len, lb = B->len;
    int64_t m = la < lb ? la : lb;
    if (A == B) {
        if (c == 't') for (int i = 0; i < n; ++i) sort_desc_skip(pp);
        return 0;
    }
    if (c == 't') {
        int64_t k = m < n ? m : n;
        for (int64_t i = 0; i < k; ++i) {
            int r = sort_desc_cmp(A->data[i], B->data[i], pp, depth + 1);
            if (r) return r;
        }
        for (int64_t i = k; i < n; ++i) sort_desc_skip(pp);
        /* 防御：运行期长度超出静态 arity 时多余位置按机器字比。 */
        for (int64_t i = n; i < m; ++i) {
            int r = sort_word_cmp(A->data[i], B->data[i]);
            if (r) return r;
        }
    } else {
        for (int64_t i = 0; i < m; ++i) {
            const uint8_t* p = child;
            int r = sort_desc_cmp(A->data[i], B->data[i], &p, depth + 1);
            if (r) return r;
        }
    }
    return (la > lb) - (la < lb);  /* 前缀相等：短者小（CPython 口径） */
}

static int cmp_desc_lt(int64_t a, int64_t b) {
    const uint8_t* p = g_cmp_desc;
    return sort_desc_cmp(a, b, &p, 0) < 0;
}
#define LESS_CMP(a, b) cmp_desc_lt(a, b)
PYADAMAS_TIMSORT(cmp, LESS_CMP)

// kind：0 int/bool、1 float、2 str、4 容器元素（desc 字典序）、其它机器字。
// desc：编译期描述符串常量，仅 kind 4 读取；标量种类传空串，不读。
void pa_list_sort(PyAdamasList* l, int64_t kind, PyAdamasStr* desc) {
    if (l->len < 2) return;
    /* 0.303：排序重排元素，kind2 位图无法跟随，保守丢弃。 */
    if (l->elem_kind == 2) pa_list_drop_bits(l);
    if (kind == 2) sort_str(l->data, l->len);
    else if (kind == 1) sort_f64(l->data, l->len);
    else if (kind == 4 && desc && desc->len > 0) {
        g_cmp_desc = (const uint8_t*)desc->data;
        sort_cmp(l->data, l->len);
        g_cmp_desc = 0;
    } else sort_i64(l->data, l->len);
}

PyAdamasList* pa_sorted_list(PyAdamasList* src, int64_t kind, PyAdamasStr* desc) {
    PyAdamasList* l = pa_list_copy(src);
    pa_list_sort(l, kind, desc);
    return l;
}

/* 按 keys 给 vals 排序：keys 与 vals 一一对应，key 的比较类别是 kind
   （0 int/bool、1 float、2 str、4 容器按 desc 字典序、其它按机器字）。
   用下标数组 + qsort：比较函数通过静态指针读键表——pyadamas 是单线程
   的，不会并发踩。 */
static const int64_t* g_sort_keys = 0;
static int64_t g_sort_kind = 0;
static const uint8_t* g_sort_desc = 0;  /* 0.312：key 是容器（kind 4）时的描述符 */

/* 键的种类未知时（函数的返回类型推不出来），退到机器字比较。这里再加一条
   启发式：两边看起来都是字符串对象就按字符串比——键是 str 时顺序才对。 */
static int key_is_str(int64_t v) {
    if (v <= 4096) return 0;
    PyAdamasRc* h = (PyAdamasRc*)((uintptr_t)v - (uintptr_t)PYADAMAS_RC_HDR);
    return rc_index_has(h) && h->kind == PYADAMAS_RC_STR;
}

static int cmp_sort_by_idx(const void* pa, const void* pb) {
    int64_t ia = *(const int64_t*)pa, ib = *(const int64_t*)pb;
    int64_t x = g_sort_keys[ia], y = g_sort_keys[ib];
    if (g_sort_kind == 4 && g_sort_desc) {
        const uint8_t* p = g_sort_desc;
        int c = sort_desc_cmp(x, y, &p, 0);
        /* qsort 不保证稳定：相等 key 退回原下标，钉住稳定性（CPython 口径）。 */
        return c ? c : sort_word_cmp(ia, ib);
    }
    if (g_sort_kind == 2) {
        int64_t c = pa_str_cmp((PyAdamasStr*)x, (PyAdamasStr*)y);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (g_sort_kind == 1) {
        double a, b;
        memcpy(&a, &x, sizeof a);
        memcpy(&b, &y, sizeof b);
        return a < b ? -1 : a > b ? 1 : 0;
    }
    if (g_sort_kind == 3 && key_is_str(x) && key_is_str(y)) {
        int64_t c = pa_str_cmp((PyAdamasStr*)x, (PyAdamasStr*)y);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    return x < y ? -1 : x > y ? 1 : 0;
}

PyAdamasList* pa_list_sorted_by(PyAdamasList* src, PyAdamasList* keys, int64_t kind,
                                PyAdamasStr* desc) {
    PyAdamasList* l = pa_list_copy(src);
    int64_t n = l->len;
    if (n < 2) return l;
    /* 键对不上（不该发生）就退回普通排序，别给错结果 */
    if (!keys || keys->len != n) {
        pa_list_sort(l, kind, desc);
        return l;
    }
    int64_t* idx = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (!idx) {
        pa_list_sort(l, kind, desc);
        return l;
    }
    for (int64_t i = 0; i < n; ++i) idx[i] = i;
    g_sort_keys = keys->data;
    g_sort_kind = kind;
    g_sort_desc = (kind == 4 && desc && desc->len > 0) ? (const uint8_t*)desc->data : 0;
    qsort(idx, (size_t)n, sizeof(int64_t), cmp_sort_by_idx);
    g_sort_keys = 0;
    g_sort_desc = 0;
    int64_t* tmp = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (tmp) {
        for (int64_t i = 0; i < n; ++i) tmp[i] = l->data[idx[i]];
        for (int64_t i = 0; i < n; ++i) l->data[i] = tmp[i];
        free(tmp);
    }
    free(idx);
    return l;
}

PyAdamasList* pa_enumerate_list(PyAdamasList* seq) {
    int64_t n = seq->len;
    PyAdamasList* out = pa_list_new(n, 1);  // 外层元素是 pair（堆）
    /* 循环里每个 pa_list_new(pair) 都可能触发主收集；out 只在本帧 C 局部
       变量里，不在任何根上，先 pin 住（子 pair 一旦交给 out 就随之可达）。 */
    pa_gc_pin(out);
    // pair 里 index 是 int、元素种类跟着源走，混合存放统一按 kind 2 保守释放。
    int pk = seq->elem_kind == 0 ? 0 : 2;
    for (int64_t i = 0; i < n; ++i) {
        int64_t ev = seq->data[i];
        PyAdamasList* pair = pa_list_new(2, pk);
        pair->len = 2;
        pair->data[0] = i;
        pair->data[1] = ev;
        pa_own_inc(seq->elem_kind, ev);
        pa_rc_inc(pair);
        pa_list_append(out, (int64_t)pair, 1);
    }
    pa_gc_unpin(out);
    return out;
}

PyAdamasList* pa_zip2_list(PyAdamasList* a, PyAdamasList* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int ka = a->elem_kind, kb = b->elem_kind;
    // pair 的元素种类：两边都是堆才是 1；都是非堆是 0；混合按 2 保守处理。
    int pk = ka == 1 && kb == 1 ? 1 : (ka == 0 && kb == 0 ? 0 : 2);
    PyAdamasList* out = pa_list_new(n, 1);
    /* 同 pa_enumerate_list：循环内每次 pa_list_new 都是主收集触发点。 */
    pa_gc_pin(out);
    for (int64_t i = 0; i < n; ++i) {
        int64_t x = a->data[i], y = b->data[i];
        PyAdamasList* pair = pa_list_new(2, pk);
        pair->len = 2;
        pair->data[0] = x;
        pair->data[1] = y;
        if (pk != 0) {
            pa_own_inc(ka, x);
            pa_own_inc(kb, y);
        }
        pa_rc_inc(pair);
        pa_list_append(out, (int64_t)pair, 1);
    }
    pa_gc_unpin(out);
    return out;
}

/* ── 列表归约（AVX2 快路径 + 标量回退） ─────────────────────── */

static int pa_has_avx2(void) {
#if defined(__x86_64__) && defined(__GNUC__)
    static int cached = -1;
    if (cached < 0) {
        __builtin_cpu_init();
        cached = __builtin_cpu_supports("avx2") ? 1 : 0;
    }
    return cached;
#else
    return 0;
#endif
}

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2")))
static uint64_t list_sum_u64_avx2(const int64_t* d, int64_t n) {
    __m256i acc = _mm256_setzero_si256();
    int64_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = _mm256_add_epi64(acc, _mm256_loadu_si256((const __m256i*)(d + i)));
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i*)lanes, acc);
    uint64_t s = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; ++i) s += (uint64_t)d[i];
    return s;
}

/* 浮点 sum 不再用 AVX2：CPython 的 Neumaier 补偿求和是串行的，
   SIMD 分批累加会在病态数据上给出不同结果（对拍分叉）。 */

__attribute__((target("avx2")))
static int64_t list_min_i64_avx2(const int64_t* d, int64_t n) {
    int64_t i = 0;
    int64_t m;
    if (n >= 4) {
        __m256i best = _mm256_loadu_si256((const __m256i*)d);
        i = 4;
        for (; i + 4 <= n; i += 4) {
            __m256i v = _mm256_loadu_si256((const __m256i*)(d + i));
            __m256i take = _mm256_cmpgt_epi64(best, v);
            best = _mm256_blendv_epi8(best, v, take);
        }
        int64_t lanes[4];
        _mm256_storeu_si256((__m256i*)lanes, best);
        m = lanes[0];
        for (int j = 1; j < 4; ++j)
            if (lanes[j] < m) m = lanes[j];
    } else {
        m = d[0];
    }
    for (; i < n; ++i)
        if (d[i] < m) m = d[i];
    return m;
}

__attribute__((target("avx2")))
static int64_t list_max_i64_avx2(const int64_t* d, int64_t n) {
    int64_t i = 0;
    int64_t m;
    if (n >= 4) {
        __m256i best = _mm256_loadu_si256((const __m256i*)d);
        i = 4;
        for (; i + 4 <= n; i += 4) {
            __m256i v = _mm256_loadu_si256((const __m256i*)(d + i));
            __m256i take = _mm256_cmpgt_epi64(v, best);
            best = _mm256_blendv_epi8(best, v, take);
        }
        int64_t lanes[4];
        _mm256_storeu_si256((__m256i*)lanes, best);
        m = lanes[0];
        for (int j = 1; j < 4; ++j)
            if (lanes[j] > m) m = lanes[j];
    } else {
        m = d[0];
    }
    for (; i < n; ++i)
        if (d[i] > m) m = d[i];
    return m;
}
#endif

int64_t pa_list_sum_i64(PyAdamasList* l, int64_t start) {
    uint64_t s = (uint64_t)start;
#if defined(__x86_64__) && defined(__GNUC__)
    if (pa_has_avx2()) s += list_sum_u64_avx2(l->data, l->len);
    else
#endif
        for (int64_t i = 0; i < l->len; ++i) s += (uint64_t)l->data[i];
    return (int64_t)s;
}

/* sum(list[float], start)：CPython 3.12+ 的 Neumaier（改进 Kahan-Babuska）
   补偿求和。朴素求和在病态数据上与 CPython 逐位不同（sum([0.1]*10) 差 1 ulp、
   sum([1e16,1,-1e16]) 直接变 0），破坏对拍。注意 CPython 只对**浮点元素**
   做补偿，这里元素本来就是 float，口径一致；start 折进 f 初值。 */
double pa_list_sum_f64(PyAdamasList* l, double start) {
    double f = start;
    double c = 0.0;
    const double* d = (const double*)l->data;
    for (int64_t i = 0; i < l->len; ++i) {
        double x = d[i];
        double t = f + x;
        if (fabs(f) >= fabs(x)) c += (f - t) + x;
        else c += (x - t) + f;
        f = t;
    }
    return f + c;
}

int64_t pa_list_min_i64(PyAdamasList* l) {
    if (l->len <= 0) pa_raise_c("ValueError", "min() iterable argument is empty");
#if defined(__x86_64__) && defined(__GNUC__)
    if (pa_has_avx2()) return list_min_i64_avx2(l->data, l->len);
#endif
    int64_t m = l->data[0];
    for (int64_t i = 1; i < l->len; ++i)
        if (l->data[i] < m) m = l->data[i];
    return m;
}

int64_t pa_list_max_i64(PyAdamasList* l) {
    if (l->len <= 0) pa_raise_c("ValueError", "max() iterable argument is empty");
#if defined(__x86_64__) && defined(__GNUC__)
    if (pa_has_avx2()) return list_max_i64_avx2(l->data, l->len);
#endif
    int64_t m = l->data[0];
    for (int64_t i = 1; i < l->len; ++i)
        if (l->data[i] > m) m = l->data[i];
    return m;
}

int list_value_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return pa_str_eq((PyAdamasStr*)a, (PyAdamasStr*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

int64_t pa_list_contains(PyAdamasList* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return 1;
    return 0;
}

/* 元素比较链：编译器按静态类型生成一串字符，每字符说明该层元素怎么比——
     i = int/bool（机器字）、s = str（按字节）、f = float（IEEE）、
     x = 其它/未知（机器字）、l = list（递归）、d = dict（递归）、t = set（递归）。
   链是编译期常量，放在数据段里；嵌套容器就靠它一层一层往下比。 */
/* level 是「本层元素的层号」：chain[level] 说明本层元素怎么比；
   元素本身还是容器时，它的元素是下一层，层号 +1。 */
int chain_elem_eq(int kind, int64_t x, int64_t y, const uint8_t* chain, int64_t len,
                  int64_t level) {
    if (kind == 's') return pa_str_eq((PyAdamasStr*)x, (PyAdamasStr*)y) != 0;
    if (kind == 'f') {
        double a, b;
        memcpy(&a, &x, sizeof a);
        memcpy(&b, &y, sizeof b);
        return a == b;
    }
    if (kind == 'l') {
        if (level + 1 >= len) return x == y;
        return chain_list_eq((PyAdamasList*)x, (PyAdamasList*)y, chain, len, level + 1);
    }
    if (kind == 'd') {
        if (level + 1 >= len) return x == y;
        return chain_dict_eq((PyAdamasDict*)x, (PyAdamasDict*)y, chain, len, level + 1);
    }
    if (kind == 't') {
        if (level + 1 >= len) return x == y;
        return chain_set_eq((PyAdamasDict*)x, (PyAdamasDict*)y, chain, len, level + 1);
    }
    return x == y;  /* i / x：按机器字比 */
}

int64_t chain_list_eq(PyAdamasList* a, PyAdamasList* b, const uint8_t* chain, int64_t len,
                      int64_t level) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    int kind = (level < len) ? chain[level] : 'x';
    for (int64_t i = 0; i < a->len; ++i)
        if (!chain_elem_eq(kind, a->data[i], b->data[i], chain, len, level)) return 0;
    return 1;
}

/* 逐元素比较两个列表/元组。chain 说明元素怎么比（见上面的说明）。 */
int64_t pa_list_eq(PyAdamasList* a, PyAdamasList* b, PyAdamasStr* chain) {
    return chain_list_eq(a, b, (const uint8_t*)chain->data, chain->len, 0);
}

int64_t pa_list_index(PyAdamasList* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return i;
    pa_raise_c("ValueError", "value is not in list");
}

int64_t pa_list_count(PyAdamasList* l, int64_t v, int64_t kind) {
    int64_t n = 0;
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) ++n;
    return n;
}

void pa_list_del_index(PyAdamasList* l, int64_t i) {
    if (!l) pa_raise_c("TypeError", "del: expected a list");
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) pa_raise_c("IndexError", "list index out of range");
    /* 0.303：删除左移元素，kind2 位图无法跟随，保守丢弃。kind3 防御性
       按 2 释放（与 set/destroy 同口径）。 */
    if (l->elem_kind == 2) pa_list_drop_bits(l);
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    int64_t v = l->data[i];
    pa_own_dec(kind, v);
    memmove(&l->data[i], &l->data[i + 1],
            sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
}

/* 进程退出时还掉列表缓冲池。纯缓存：回收对象时不立刻 free 而是挂进池里，
   退出时这些块既不在 GC 堆链上也没有所有者 —— ASan 会报成 Direct leak。
   不影响正确性，但会淹没真正有价值的泄漏信号。 */
void pa_list_buf_pool_shutdown(void) {
    for (int i = 0; i < LIST_BUF_CLASSES; ++i) {
        while (list_buf_pool[i]) {
            int64_t* p = list_buf_pool[i];
            memcpy(&list_buf_pool[i], p, sizeof(int64_t*));
            free(p);
        }
        list_buf_count[i] = 0;
    }
    for (int i = 0; i < LIST_BUF_CLASSES / 64; ++i) list_buf_avail[i] = 0;
}
