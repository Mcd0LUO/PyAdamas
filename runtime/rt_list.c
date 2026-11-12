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
    Py23List* l = (Py23List*)p;
    if (l->elem_kind == 1)
        for (int64_t i = 0; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    list_buf_put(l->data, l->cap);
}

// GC 清扫前调用：只把「指向可达对象」的引用计数减掉。kind 2 的元素类型
// 不可知，跳过（那些对象如果只被这个容器引用，会在同一轮清扫里被释放）。
void list_gc_release(void* p) {
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
    l->data = list_buf_get(&l->cap);
    l->elem_kind = elem_kind;
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

Py23List* py23_list_copy(Py23List* src) {
    Py23List* l = py23_list_new(src->len, src->elem_kind);
    py23_list_extend(l, src);
    return l;
}

Py23List* py23_list_reversed(Py23List* src) {
    int64_t n = src->len;
    Py23List* l = py23_list_new(n, src->elem_kind);
    for (int64_t i = n - 1; i >= 0; --i) {
        int64_t v = src->data[i];
        if (v && src->elem_kind == 1 && (l->elem_kind == 1 || l->elem_kind == 2))
            py23_rc_inc((void*)v);
        py23_list_append(l, v);
    }
    return l;
}

void py23_list_insert(Py23List* l, int64_t i, int64_t v) {
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

void py23_list_reverse(Py23List* l) {
    for (int64_t i = 0, j = l->len - 1; i < j; ++i, --j) {
        int64_t t = l->data[i];
        l->data[i] = l->data[j];
        l->data[j] = t;
    }
}

int64_t py23_int_bit_length(int64_t v) {
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

#define PY23_TIMSORT(NAME, LESS)                                               \
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
    (py23_str_cmp((Py23Str*)(uintptr_t)(a), (Py23Str*)(uintptr_t)(b)) < 0)

PY23_TIMSORT(i64, LESS_I64)
PY23_TIMSORT(f64, LESS_F64)
PY23_TIMSORT(str, LESS_STR)

// kind：0 int/bool、1 float、2 str、其它按机器字比较。
void py23_list_sort(Py23List* l, int64_t kind) {
    if (l->len < 2) return;
    if (kind == 2) sort_str(l->data, l->len);
    else if (kind == 1) sort_f64(l->data, l->len);
    else sort_i64(l->data, l->len);
}

Py23List* py23_sorted_list(Py23List* src, int64_t kind) {
    Py23List* l = py23_list_copy(src);
    py23_list_sort(l, kind);
    return l;
}

/* 按 keys 给 vals 排序：keys 与 vals 一一对应，key 的比较类别是 kind
   （0 int/bool、1 float、2 str、其它按机器字）。用下标数组 + qsort：
   比较函数通过静态指针读键表——py23 是单线程的，不会并发踩。 */
static const int64_t* g_sort_keys = 0;
static int64_t g_sort_kind = 0;

/* 键的种类未知时（函数的返回类型推不出来），退到机器字比较。这里再加一条
   启发式：两边看起来都是字符串对象就按字符串比——键是 str 时顺序才对。 */
static int key_is_str(int64_t v) {
    if (v <= 4096) return 0;
    Py23Rc* h = (Py23Rc*)((uintptr_t)v - (uintptr_t)PY23_RC_HDR);
    return rc_index_has(h) && h->kind == PY23_RC_STR;
}

static int cmp_sort_by_idx(const void* pa, const void* pb) {
    int64_t ia = *(const int64_t*)pa, ib = *(const int64_t*)pb;
    int64_t x = g_sort_keys[ia], y = g_sort_keys[ib];
    if (g_sort_kind == 2) {
        int64_t c = py23_str_cmp((Py23Str*)x, (Py23Str*)y);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (g_sort_kind == 1) {
        double a, b;
        memcpy(&a, &x, sizeof a);
        memcpy(&b, &y, sizeof b);
        return a < b ? -1 : a > b ? 1 : 0;
    }
    if (g_sort_kind == 3 && key_is_str(x) && key_is_str(y)) {
        int64_t c = py23_str_cmp((Py23Str*)x, (Py23Str*)y);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    return x < y ? -1 : x > y ? 1 : 0;
}

Py23List* py23_list_sorted_by(Py23List* src, Py23List* keys, int64_t kind) {
    Py23List* l = py23_list_copy(src);
    int64_t n = l->len;
    if (n < 2) return l;
    /* 键对不上（不该发生）就退回普通排序，别给错结果 */
    if (!keys || keys->len != n) {
        py23_list_sort(l, kind);
        return l;
    }
    int64_t* idx = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (!idx) {
        py23_list_sort(l, kind);
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

Py23List* py23_enumerate_list(Py23List* seq) {
    int64_t n = seq->len;
    Py23List* out = py23_list_new(n, 1);  // 外层元素是 pair（堆）
    for (int64_t i = 0; i < n; ++i) {
        int64_t ev = seq->data[i];
        Py23List* pair = py23_list_new(2, seq->elem_kind);
        pair->len = 2;
        pair->data[0] = i;
        pair->data[1] = ev;
        if (seq->elem_kind == 1 && ev) py23_rc_inc((void*)ev);
        py23_rc_inc(pair);
        py23_list_append(out, (int64_t)pair);
    }
    return out;
}

Py23List* py23_zip2_list(Py23List* a, Py23List* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int ka = a->elem_kind, kb = b->elem_kind;
    // pair 的元素种类：两边都是堆才是 1；都是非堆是 0；混合按 2 保守处理。
    int pk = ka == 1 && kb == 1 ? 1 : (ka == 0 && kb == 0 ? 0 : 2);
    Py23List* out = py23_list_new(n, 1);
    for (int64_t i = 0; i < n; ++i) {
        int64_t x = a->data[i], y = b->data[i];
        Py23List* pair = py23_list_new(2, pk);
        pair->len = 2;
        pair->data[0] = x;
        pair->data[1] = y;
        if (pk != 0) {
            if (ka == 1 && x) py23_rc_inc((void*)x);
            if (kb == 1 && y) py23_rc_inc((void*)y);
        }
        py23_rc_inc(pair);
        py23_list_append(out, (int64_t)pair);
    }
    return out;
}

/* ── 列表归约（AVX2 快路径 + 标量回退） ─────────────────────── */

static int py23_has_avx2(void) {
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

int64_t py23_list_sum_i64(Py23List* l, int64_t start) {
    uint64_t s = (uint64_t)start;
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2()) s += list_sum_u64_avx2(l->data, l->len);
    else
#endif
        for (int64_t i = 0; i < l->len; ++i) s += (uint64_t)l->data[i];
    return (int64_t)s;
}

double py23_list_sum_f64(Py23List* l, double start) {
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2())
        return start + list_sum_f64_avx2((const double*)l->data, l->len);
#endif
    double s = start;
    const double* d = (const double*)l->data;
    for (int64_t i = 0; i < l->len; ++i) s += d[i];
    return s;
}

int64_t py23_list_min_i64(Py23List* l) {
    if (l->len <= 0) py23_raise_c("ValueError", "min() iterable argument is empty");
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2()) return list_min_i64_avx2(l->data, l->len);
#endif
    int64_t m = l->data[0];
    for (int64_t i = 1; i < l->len; ++i)
        if (l->data[i] < m) m = l->data[i];
    return m;
}

int64_t py23_list_max_i64(Py23List* l) {
    if (l->len <= 0) py23_raise_c("ValueError", "max() iterable argument is empty");
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2()) return list_max_i64_avx2(l->data, l->len);
#endif
    int64_t m = l->data[0];
    for (int64_t i = 1; i < l->len; ++i)
        if (l->data[i] > m) m = l->data[i];
    return m;
}

int list_value_eq(int64_t kind, int64_t a, int64_t b) {
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

/* 元素比较链：编译器按静态类型生成一串字符，每字符说明该层元素怎么比——
     i = int/bool（机器字）、s = str（按字节）、f = float（IEEE）、
     x = 其它/未知（机器字）、l = list（递归）、d = dict（递归）、t = set（递归）。
   链是编译期常量，放在数据段里；嵌套容器就靠它一层一层往下比。 */
/* level 是「本层元素的层号」：chain[level] 说明本层元素怎么比；
   元素本身还是容器时，它的元素是下一层，层号 +1。 */
int chain_elem_eq(int kind, int64_t x, int64_t y, const uint8_t* chain, int64_t len,
                  int64_t level) {
    if (kind == 's') return py23_str_eq((Py23Str*)x, (Py23Str*)y) != 0;
    if (kind == 'f') {
        double a, b;
        memcpy(&a, &x, sizeof a);
        memcpy(&b, &y, sizeof b);
        return a == b;
    }
    if (kind == 'l') {
        if (level + 1 >= len) return x == y;
        return chain_list_eq((Py23List*)x, (Py23List*)y, chain, len, level + 1);
    }
    if (kind == 'd') {
        if (level + 1 >= len) return x == y;
        return chain_dict_eq((Py23Dict*)x, (Py23Dict*)y, chain, len, level + 1);
    }
    if (kind == 't') {
        if (level + 1 >= len) return x == y;
        return chain_set_eq((Py23Dict*)x, (Py23Dict*)y, chain, len, level + 1);
    }
    return x == y;  /* i / x：按机器字比 */
}

int64_t chain_list_eq(Py23List* a, Py23List* b, const uint8_t* chain, int64_t len,
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
int64_t py23_list_eq(Py23List* a, Py23List* b, Py23Str* chain) {
    return chain_list_eq(a, b, (const uint8_t*)chain->data, chain->len, 0);
}

int64_t py23_list_index(Py23List* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return i;
    py23_raise_c("ValueError", "value is not in list");
}

int64_t py23_list_count(Py23List* l, int64_t v, int64_t kind) {
    int64_t n = 0;
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) ++n;
    return n;
}

void py23_list_del_index(Py23List* l, int64_t i) {
    if (!l) py23_raise_c("TypeError", "del: expected a list");
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list index out of range");
    int64_t v = l->data[i];
    if (l->elem_kind == 1 && v) py23_rc_dec((void*)v);
    memmove(&l->data[i], &l->data[i + 1],
            sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
}

