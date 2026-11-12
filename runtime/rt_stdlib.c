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
        if (v && elem_kind == 1) pa_rc_inc((void*)v);
        pa_list_append(out, v);
    }
    return out;
}

PyAdamasDict* pa_collections_counter(PyAdamasList* src, int64_t key_kind, int64_t key_own) {
    PyAdamasDict* d = pa_dict_new(key_kind, key_own, 0, -1, -1);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        int64_t c = pa_dict_get_or(d, k, 0);
        // dict_set 的约定：key_own==1 时它「消费」传入的 key 引用。
        if (key_own == 1 && k) pa_rc_inc((void*)k);
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
        if ((tk == 1 || d->key_own == 1) && arr[i].key)
            pa_rc_inc((void*)arr[i].key);
        pa_list_append(tup, arr[i].key);
        if ((tk == 1 || d->val_own == 1) && arr[i].val)
            pa_rc_inc((void*)arr[i].val);
        pa_list_append(tup, arr[i].val);
        pa_list_append(out, (int64_t)tup);
    }
    free(arr);
    return out;
}
