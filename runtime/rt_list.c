// rt_list.c —— 列表、排序与归约
#include "rt_internal.h"

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

/* ── 列表 ─────────────────────────────────────────────────────── */

void list_destroy(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind == 1)
        for (int64_t i = 0; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    free(l->data);
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

static int cmp_sort_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int cmp_sort_f64(const void* a, const void* b) {
    double x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static int cmp_sort_str(const void* a, const void* b) {
    Py23Str* x = (Py23Str*)(*(const int64_t*)a);
    Py23Str* y = (Py23Str*)(*(const int64_t*)b);
    int64_t c = py23_str_cmp(x, y);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// kind：0 int/bool、1 float、2 str、其它按机器字比较。
void py23_list_sort(Py23List* l, int64_t kind) {
    if (l->len < 2) return;
    qsort(l->data, (size_t)l->len, sizeof(int64_t),
          kind == 2 ? cmp_sort_str : kind == 1 ? cmp_sort_f64 : cmp_sort_i64);
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

