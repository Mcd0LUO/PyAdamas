// rt_list.c —— 列表、排序与归约
#include "rt_internal.h"

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

/* ── 列表 ─────────────────────────────────────────────────────── */

/* 列表 data 缓冲按 64 字节分档（<=512 字节），每档最多缓存 64 块。
   空块的头 8 字节存下一块的指针；复用时容量只会变大，调用方按 cap 使用。 */
#define LIST_BUF_MAX 512
#define LIST_BUF_ALIGN 64
#define LIST_BUF_CLASSES (LIST_BUF_MAX / LIST_BUF_ALIGN)
#define LIST_BUF_MAX_PER_CLASS 64
static int64_t* list_buf_pool[LIST_BUF_CLASSES];
static int list_buf_count[LIST_BUF_CLASSES];

static int list_buf_class(int64_t bytes) {
    if (bytes <= 0 || bytes > LIST_BUF_MAX) return -1;
    return (int)((bytes + LIST_BUF_ALIGN - 1) / LIST_BUF_ALIGN) - 1;
}

static int64_t* list_buf_get(int64_t* cap_io) {
    int64_t cap = *cap_io < 4 ? 4 : *cap_io;
    int64_t bytes = cap * (int64_t)sizeof(int64_t);
    int c = list_buf_class(bytes);
    if (c >= 0) {
        // 同一档的块一律按档容量分配，复用后 cap 与真实容量严格一致。
        int64_t class_bytes = (int64_t)(c + 1) * LIST_BUF_ALIGN;
        int64_t* p;
        if (list_buf_pool[c]) {
            p = list_buf_pool[c];
            list_buf_pool[c] = (int64_t*)p[0];
            list_buf_count[c]--;
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
    if (c < 0 || list_buf_count[c] >= LIST_BUF_MAX_PER_CLASS) {
        free(p);
        return;
    }
    p[0] = (int64_t)list_buf_pool[c];
    list_buf_pool[c] = p;
    list_buf_count[c]++;
}

void list_destroy(void* p) {
    PyAdamasList* l = (PyAdamasList*)p;
    // kind 3 按约定是空列表；真出现元素（不变量被破坏）时按 2 保守释放。
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    if (kind != 0)
        for (int64_t i = 0; i < l->len; ++i) pa_own_dec(kind, l->data[i]);
    list_buf_put(l->data, l->cap);
}

// GC 清扫前调用：只把「指向可达对象」的引用计数减掉。kind 2 的元素走
// 注册表判断（整数不会命中），不再整体跳过。
void list_gc_release(void* p) {
    PyAdamasList* l = (PyAdamasList*)p;
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    if (kind == 0 || !l->data) return;
    for (int64_t i = 0; i < l->len; ++i) pa_own_gc_unref(kind, l->data[i]);
}

PyAdamasList* pa_list_new(int64_t cap, int64_t elem_kind) {
    if (cap < 0) panic_c("negative list capacity");
    if (elem_kind < 0 || elem_kind > 3) panic_c("invalid list element kind");
    if (cap < 4) cap = 4;
    PyAdamasList* l = (PyAdamasList*)rc_alloc((int64_t)sizeof(PyAdamasList), list_destroy,
                                      list_gc_release, PYADAMAS_RC_LIST);
    l->len = 0;
    l->cap = cap;
    l->data = list_buf_get(&l->cap);
    l->elem_kind = elem_kind;
    return l;
}

/* 探测 unknown 值是不是堆指针：只查存活索引、不解引用，任意 int64 都安全。
   和 pa_rc_inc_unknown 一致，静态字符串字面量（永生）也算堆。 */
static int list_probe(int64_t v) {
    if (v <= 4096) return 0;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    return rc_index_has(h) ? 1 : 0;
}

/* 列表元素类别定型。kind 3 的空列表在这里采用第一个写入值的种类；静态
   种类和值冲突时降级为 2（混合，销毁走注册表）。vk 是值的静态种类，
   unknown 时只能运行期探测一次。owned 非 0 表示引用已经转给容器。 */
void pa_list_prep(PyAdamasList* l, int64_t v, int64_t vk, int64_t owned) {
    int kind = (int)l->elem_kind;
    if (kind == 3) {
        kind = vk == 2 ? list_probe(v) : (int)vk;
    } else if (vk == 2) {
        if ((kind == 1) != (list_probe(v) != 0)) kind = 2;
    } else if (kind != vk) {
        kind = 2;
    }
    l->elem_kind = kind;
    if (!owned) pa_own_inc(kind, v);
}

/* 元素引用由生成代码在写入前 retain（kind 1/2 且源类型已知是堆），
   运行时不再重复 inc；kind 1 覆盖/截断/销毁时负责 release。 */
void pa_list_append(PyAdamasList* l, int64_t v) {
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        int64_t* nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->data = nd;
        l->cap = ncap;
    }
    l->data[l->len++] = v;
}

int64_t pa_list_get(PyAdamasList* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) pa_raise_c("IndexError", "list index out of range");
    return l->data[i];
}

void pa_list_set(PyAdamasList* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) pa_raise_c("IndexError", "list assignment index out of range");
    int kind = l->elem_kind == 3 ? 2 : (int)l->elem_kind;
    int64_t old = l->data[i];
    l->data[i] = v;
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
        pa_list_append(dst, v);
    }
    free(snap);
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
        pa_list_append(l, v);
    }
    return l;
}

void pa_list_insert(PyAdamasList* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0) i = 0;
    if (i > l->len) i = l->len;
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        int64_t* nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->data = nd;
        l->cap = ncap;
    }
    if (i < l->len)
        memmove(l->data + i + 1, l->data + i, sizeof(int64_t) * (size_t)(l->len - i));
    l->data[i] = v;
    ++l->len;
}

void pa_list_reverse(PyAdamasList* l) {
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
        int64_t* tmp = (int64_t*)malloc((size_t)(n / 2 + 1) * sizeof(int64_t)); \
        if (!tmp) {                                                            \
            bin_ins_##NAME(a, 0, n);                                           \
            return;                                                            \
        }                                                                      \
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
        free(tmp);                                                             \
    }

#define LESS_I64(a, b) ((a) < (b))
#define LESS_F64(a, b) (f64_of(a) < f64_of(b))
#define LESS_STR(a, b) \
    (pa_str_cmp((PyAdamasStr*)(uintptr_t)(a), (PyAdamasStr*)(uintptr_t)(b)) < 0)

PYADAMAS_TIMSORT(i64, LESS_I64)
PYADAMAS_TIMSORT(f64, LESS_F64)
PYADAMAS_TIMSORT(str, LESS_STR)

// kind：0 int/bool、1 float、2 str、其它按机器字比较。
void pa_list_sort(PyAdamasList* l, int64_t kind) {
    if (l->len < 2) return;
    if (kind == 2) sort_str(l->data, l->len);
    else if (kind == 1) sort_f64(l->data, l->len);
    else sort_i64(l->data, l->len);
}

PyAdamasList* pa_sorted_list(PyAdamasList* src, int64_t kind) {
    PyAdamasList* l = pa_list_copy(src);
    pa_list_sort(l, kind);
    return l;
}

/* 按 keys 给 vals 排序：keys 与 vals 一一对应，key 的比较类别是 kind
   （0 int/bool、1 float、2 str、其它按机器字）。用下标数组 + qsort：
   比较函数通过静态指针读键表——pyadamas 是单线程的，不会并发踩。 */
static const int64_t* g_sort_keys = 0;
static int64_t g_sort_kind = 0;

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

PyAdamasList* pa_list_sorted_by(PyAdamasList* src, PyAdamasList* keys, int64_t kind) {
    PyAdamasList* l = pa_list_copy(src);
    int64_t n = l->len;
    if (n < 2) return l;
    /* 键对不上（不该发生）就退回普通排序，别给错结果 */
    if (!keys || keys->len != n) {
        pa_list_sort(l, kind);
        return l;
    }
    int64_t* idx = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (!idx) {
        pa_list_sort(l, kind);
        return l;
    }
    for (int64_t i = 0; i < n; ++i) idx[i] = i;
    g_sort_keys = keys->data;
    g_sort_kind = kind;
    qsort(idx, (size_t)n, sizeof(int64_t), cmp_sort_by_idx);
    g_sort_keys = 0;
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
        pa_list_append(out, (int64_t)pair);
    }
    return out;
}

PyAdamasList* pa_zip2_list(PyAdamasList* a, PyAdamasList* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int ka = a->elem_kind, kb = b->elem_kind;
    // pair 的元素种类：两边都是堆才是 1；都是非堆是 0；混合按 2 保守处理。
    int pk = ka == 1 && kb == 1 ? 1 : (ka == 0 && kb == 0 ? 0 : 2);
    PyAdamasList* out = pa_list_new(n, 1);
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
        pa_list_append(out, (int64_t)pair);
    }
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

__attribute__((target("avx2")))
static double list_sum_f64_avx2(const double* d, int64_t n) {
    __m256d acc = _mm256_setzero_pd();
    int64_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = _mm256_add_pd(acc, _mm256_loadu_pd(d + i));
    double lanes[4];
    _mm256_storeu_pd(lanes, acc);
    double s = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; ++i) s += d[i];
    return s;
}

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

double pa_list_sum_f64(PyAdamasList* l, double start) {
#if defined(__x86_64__) && defined(__GNUC__)
    if (pa_has_avx2())
        return start + list_sum_f64_avx2((const double*)l->data, l->len);
#endif
    double s = start;
    const double* d = (const double*)l->data;
    for (int64_t i = 0; i < l->len; ++i) s += d[i];
    return s;
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
    int64_t v = l->data[i];
    pa_own_dec(l->elem_kind, v);
    memmove(&l->data[i], &l->data[i + 1],
            sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
}

