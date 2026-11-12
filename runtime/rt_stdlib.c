// rt_stdlib.c —— 标准库里按调用点静态特化的辅助函数
//
// itertools/collections 没有泛型，编译器在调用点按实参类型特化，
// 这里只提供类型无关的机械操作（元素所有权类别由调用点传入）。
#include "rt_internal.h"

Py23List* py23_itertools_chain(Py23List* a, Py23List* b, int64_t elem_kind) {
    Py23List* out = py23_list_new(a->len + b->len, elem_kind);
    py23_list_extend(out, a);
    py23_list_extend(out, b);
    return out;
}

Py23List* py23_itertools_repeat(int64_t v, int64_t n, int64_t elem_kind) {
    if (n < 0) n = 0;
    Py23List* out = py23_list_new(n, elem_kind);
    for (int64_t i = 0; i < n; ++i) {
        if (v && elem_kind == 1) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23Dict* py23_collections_counter(Py23List* src, int64_t key_kind, int64_t key_own) {
    Py23Dict* d = py23_dict_new(key_kind, key_own, 0, -1, -1);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        int64_t c = py23_dict_get_or(d, k, 0);
        // dict_set 的约定：key_own==1 时它「消费」传入的 key 引用。
        if (key_own == 1 && k) py23_rc_inc((void*)k);
        py23_dict_set(d, k, c + 1);
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

Py23List* py23_collections_most_common(Py23Dict* d, int64_t n) {
    int64_t m = d->len;
    CountPair* arr = (CountPair*)malloc(sizeof(CountPair) * (size_t)(m ? m : 1));
    if (!arr) panic_c("out of memory");
    for (int64_t i = 0; i < m; ++i) {
        arr[i].key = py23_dict_key_at(d, i);
        arr[i].val = py23_dict_val_at(d, i);
        arr[i].idx = i;
    }
    qsort(arr, (size_t)m, sizeof(CountPair), cmp_pair);
    int64_t take = m;
    if (n >= 0 && n < take) take = n;
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* out = py23_list_new(take, 1);
    for (int64_t i = 0; i < take; ++i) {
        Py23List* tup = py23_list_new(2, tk);
        if ((tk == 1 || d->key_own == 1) && arr[i].key)
            py23_rc_inc((void*)arr[i].key);
        py23_list_append(tup, arr[i].key);
        if ((tk == 1 || d->val_own == 1) && arr[i].val)
            py23_rc_inc((void*)arr[i].val);
        py23_list_append(tup, arr[i].val);
        py23_list_append(out, (int64_t)tup);
    }
    free(arr);
    return out;
}
