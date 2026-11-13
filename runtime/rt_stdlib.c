// rt_stdlib.c —— 标准库里按调用点静态特化的辅助函数
//
// itertools/collections 没有泛型，编译器在调用点按实参类型特化，
// 这里只提供类型无关的机械操作（元素所有权类别由调用点传入）。
#include "rt_internal.h"

PyAdamasList* pa_itertools_chain(PyAdamasList* a, PyAdamasList* b, int64_t elem_kind) {
    PyAdamasList* out = pa_list_new(a->len + b->len, elem_kind);
    pa_list_extend(out, a);
    pa_list_extend(out, b);
    return out;
}

PyAdamasList* pa_itertools_repeat(int64_t v, int64_t n, int64_t elem_kind) {
    if (n < 0) n = 0;
    PyAdamasList* out = pa_list_new(n, elem_kind);
    for (int64_t i = 0; i < n; ++i) {
        pa_own_inc(elem_kind, v);
        pa_list_append(out, v);
    }
    return out;
}

// 生成一个元组（运行时用 PyAdamasList 表示）挂到外层列表上。外层列表的
// 元素类别固定为 1（元组都是堆对象），元组内部按 elem_kind 持有引用。
static void iter_push_tuple(PyAdamasList* out, const int64_t* vals, int64_t n,
                            int64_t elem_kind) {
    PyAdamasList* tup = pa_list_new(n, elem_kind);
    for (int64_t i = 0; i < n; ++i) {
        pa_own_inc(elem_kind, vals[i]);
        pa_list_append(tup, vals[i]);
    }
    pa_list_append(out, (int64_t)tup);
}

// 源列表数组：seqs 是调用点临时构造的 kind 0 列表，data 里装源列表指针。
static PyAdamasList* iter_seq_at(PyAdamasList* seqs, int64_t i) {
    return (PyAdamasList*)seqs->data[i];
}

PyAdamasList* pa_itertools_product(PyAdamasList* seqs, int64_t elem_kind) {
    int64_t n = seqs->len;
    int64_t total = 1;
    int has_empty = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t len = iter_seq_at(seqs, i)->len;
        if (len == 0) {
            has_empty = 1;
            total = 0;
            break;
        }
        if (total > INT64_MAX / len) {
            total = 0;  // 规模不可枚举，按需增长
            break;
        }
        total *= len;
    }
    PyAdamasList* out = pa_list_new(total > (1 << 20) ? 0 : total, 1);
    if (n == 0) {
        int64_t none = 0;
        iter_push_tuple(out, &none, 0, elem_kind);
        return out;
    }
    // 任一源序列为空 → 笛卡尔积为空（混合进制进位在长度为 0 时不会终止）。
    if (has_empty) return out;
    int64_t* idx = (int64_t*)calloc((size_t)n, sizeof(int64_t));
    int64_t* vals = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!idx || !vals) panic_c("out of memory");
    while (1) {
        for (int64_t i = 0; i < n; ++i) vals[i] = iter_seq_at(seqs, i)->data[idx[i]];
        iter_push_tuple(out, vals, n, elem_kind);
        int64_t k = n - 1;
        while (k >= 0 && ++idx[k] == iter_seq_at(seqs, k)->len) {
            idx[k] = 0;
            --k;
        }
        if (k < 0) break;
    }
    free(idx);
    free(vals);
    return out;
}

static void iter_perm_rec(PyAdamasList* pool, int64_t r, int64_t depth, int64_t* used,
                          int64_t* cur, PyAdamasList* out, int64_t elem_kind) {
    if (depth == r) {
        iter_push_tuple(out, cur, r, elem_kind);
        return;
    }
    for (int64_t i = 0; i < pool->len; ++i) {
        if (used[i]) continue;
        used[i] = 1;
        cur[depth] = pool->data[i];
        iter_perm_rec(pool, r, depth + 1, used, cur, out, elem_kind);
        used[i] = 0;
    }
}

PyAdamasList* pa_itertools_permutations(PyAdamasList* pool, int64_t r,
                                        int64_t elem_kind) {
    int64_t n = pool->len;
    if (r < 0) r = n;
    PyAdamasList* out = pa_list_new(0, 1);
    if (r > n) return out;
    int64_t* used = (int64_t*)calloc((size_t)(n ? n : 1), sizeof(int64_t));
    int64_t* cur = (int64_t*)malloc((size_t)(r ? r : 1) * sizeof(int64_t));
    if (!used || !cur) panic_c("out of memory");
    iter_perm_rec(pool, r, 0, used, cur, out, elem_kind);
    free(used);
    free(cur);
    return out;
}

static void iter_comb_rec(PyAdamasList* pool, int64_t r, int64_t start, int64_t depth,
                          int64_t* cur, PyAdamasList* out, int64_t elem_kind,
                          int64_t replacement) {
    if (depth == r) {
        iter_push_tuple(out, cur, r, elem_kind);
        return;
    }
    for (int64_t i = start; i < pool->len; ++i) {
        cur[depth] = pool->data[i];
        iter_comb_rec(pool, r, replacement ? i : i + 1, depth + 1, cur, out, elem_kind,
                      replacement);
    }
}

PyAdamasList* pa_itertools_combinations(PyAdamasList* pool, int64_t r,
                                        int64_t elem_kind) {
    PyAdamasList* out = pa_list_new(0, 1);
    if (r < 0 || r > pool->len) return out;
    int64_t* cur = (int64_t*)malloc((size_t)(r ? r : 1) * sizeof(int64_t));
    if (!cur) panic_c("out of memory");
    iter_comb_rec(pool, r, 0, 0, cur, out, elem_kind, 0);
    free(cur);
    return out;
}

PyAdamasList* pa_itertools_combinations_wr(PyAdamasList* pool, int64_t r,
                                           int64_t elem_kind) {
    PyAdamasList* out = pa_list_new(0, 1);
    if (r < 0) return out;
    int64_t* cur = (int64_t*)malloc((size_t)(r ? r : 1) * sizeof(int64_t));
    if (!cur) panic_c("out of memory");
    iter_comb_rec(pool, r, 0, 0, cur, out, elem_kind, 1);
    free(cur);
    return out;
}

PyAdamasList* pa_itertools_zip_longest(PyAdamasList* seqs, int64_t elem_kind) {
    int64_t n = seqs->len;
    int64_t maxlen = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t len = iter_seq_at(seqs, i)->len;
        if (len > maxlen) maxlen = len;
    }
    PyAdamasList* out = pa_list_new(maxlen, 1);
    int64_t* vals = (int64_t*)malloc((size_t)(n ? n : 1) * sizeof(int64_t));
    if (!vals) panic_c("out of memory");
    for (int64_t row = 0; row < maxlen; ++row) {
        for (int64_t i = 0; i < n; ++i) {
            PyAdamasList* s = iter_seq_at(seqs, i);
            // 短的那个补 None（0）；iter_push_tuple 会跳过 0 的 retain。
            vals[i] = row < s->len ? s->data[row] : 0;
        }
        iter_push_tuple(out, vals, n, elem_kind);
    }
    free(vals);
    return out;
}

PyAdamasList* pa_itertools_accumulate(PyAdamasList* xs, int64_t is_float) {
    PyAdamasList* out = pa_list_new(xs->len, is_float ? 0 : xs->elem_kind);
    if (is_float) {
        double acc = 0.0;
        const double* d = (const double*)xs->data;
        for (int64_t i = 0; i < xs->len; ++i) {
            acc += d[i];
            double* o = (double*)out->data;
            o[i] = acc;
            out->len = i + 1;
        }
        return out;
    }
    int64_t acc = 0;
    for (int64_t i = 0; i < xs->len; ++i) {
        acc += xs->data[i];
        // 元素类别非 0 时列表销毁会 dec，写入前必须配一次 inc。
        pa_own_inc(out->elem_kind, acc);
        pa_list_append(out, acc);
    }
    return out;
}

/* str * n / list * n：一次分配、批量拷贝（CPython 的重复语义）。 */
PyAdamasStr* pa_str_repeat(PyAdamasStr* s, int64_t n) {
    int64_t len = s ? s->len : 0;
    if (n < 0) n = 0;
    if (len == 0 || n == 0) return str_new("", 0);
    if (len > INT64_MAX / n) panic_c("string repeat: result too large");
    PyAdamasStr* out = str_alloc(len * n);
    char* d = (char*)out->data;
    for (int64_t i = 0; i < n; ++i) memcpy(d + i * len, s->data, (size_t)len);
    return out;
}

PyAdamasList* pa_list_repeat(PyAdamasList* l, int64_t n) {
    int64_t len = l ? l->len : 0;
    int kind = l ? (int)l->elem_kind : 0;
    if (n < 0) n = 0;
    if (len == 0 || n == 0) return pa_list_new(0, kind);
    if (len > INT64_MAX / n) panic_c("list repeat: result too large");
    int64_t total = len * n;
    PyAdamasList* out = pa_list_new(total > (1 << 20) ? 0 : total, kind);
    for (int64_t rep = 0; rep < n; ++rep) {
        for (int64_t i = 0; i < len; ++i) {
            int64_t v = l->data[i];
            pa_own_inc(kind, v);
            pa_list_append(out, v);
        }
    }
    return out;
}

/* base64：stdlib/base64.py 的 b64 编解码走 C intrinsic。 */
static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

PyAdamasStr* pa_b64encode(PyAdamasStr* data, int64_t urlsafe) {
    static const char kStd[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char kUrl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* tab = urlsafe ? kUrl : kStd;
    int64_t n = data ? data->len : 0;
    const unsigned char* p = (const unsigned char*)(data && data->data ? data->data : "");
    int64_t rem = n % 3;
    PyAdamasStr* out = str_alloc((n / 3) * 4 + (rem ? 4 : 0));
    char* d = (char*)out->data;
    int64_t i = 0, oi = 0;
    while (i + 2 < n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        d[oi++] = tab[(v >> 18) & 63];
        d[oi++] = tab[(v >> 12) & 63];
        d[oi++] = tab[(v >> 6) & 63];
        d[oi++] = tab[v & 63];
        i += 3;
    }
    if (rem == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        d[oi++] = tab[(v >> 18) & 63];
        d[oi++] = tab[(v >> 12) & 63];
        d[oi++] = '=';
        d[oi++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        d[oi++] = tab[(v >> 18) & 63];
        d[oi++] = tab[(v >> 12) & 63];
        d[oi++] = tab[(v >> 6) & 63];
        d[oi++] = '=';
    }
    return out;
}

PyAdamasStr* pa_b64decode(PyAdamasStr* data) {
    int64_t n = data ? data->len : 0;
    const unsigned char* p = (const unsigned char*)(data && data->data ? data->data : "");
    PyAdamasStr* out = str_alloc((n / 4 + 1) * 3);
    char* d = (char*)out->data;
    int64_t oi = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (int64_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (c == '=') continue;
        int v = b64val(c);
        if (v < 0) pa_raise_c("ValueError", "invalid base64 character");
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            d[oi++] = (char)((acc >> bits) & 255);
            acc &= (1u << bits) - 1;
        }
    }
    out->len = oi;
    return out;
}

PyAdamasDict* pa_collections_counter(PyAdamasList* src, int64_t key_kind, int64_t key_own) {
    if (src->elem_kind > key_own) key_own = src->elem_kind;
    PyAdamasDict* d = pa_dict_new(key_kind, key_own, 0, -1, -1);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        int64_t c = pa_dict_get_or(d, k, 0);
        // dict_set 的约定：key_own 非 0 时它「消费」传入的 key 引用。
        pa_own_inc(key_own, k);
        pa_dict_set(d, k, c + 1);
    }
    return d;
}

typedef struct {
    int64_t key;
    int64_t val;
    int64_t idx;
} CountPair;

static int cmp_pair(const void* x, const void* y) {
    const CountPair* a = (const CountPair*)x;
    const CountPair* b = (const CountPair*)y;
    if (a->val != b->val) return a->val > b->val ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

PyAdamasList* pa_collections_most_common(PyAdamasDict* d, int64_t n) {
    int64_t m = d->len;
    CountPair* arr = (CountPair*)malloc(sizeof(CountPair) * (size_t)(m ? m : 1));
    if (!arr) panic_c("out of memory");
    for (int64_t i = 0; i < m; ++i) {
        arr[i].key = pa_dict_key_at(d, i);
        arr[i].val = pa_dict_val_at(d, i);
        arr[i].idx = i;
    }
    qsort(arr, (size_t)m, sizeof(CountPair), cmp_pair);
    int64_t take = m;
    if (n >= 0 && n < take) take = n;
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    PyAdamasList* out = pa_list_new(take, 1);
    for (int64_t i = 0; i < take; ++i) {
        PyAdamasList* tup = pa_list_new(2, tk);
        pa_own_inc(d->key_own, arr[i].key);
        pa_list_append(tup, arr[i].key);
        pa_own_inc(d->val_own, arr[i].val);
        pa_list_append(tup, arr[i].val);
        pa_list_append(out, (int64_t)tup);
    }
    free(arr);
    return out;
}
