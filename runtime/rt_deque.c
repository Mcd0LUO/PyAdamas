// rt_deque.c —— collections.deque：环形缓冲的双端队列
#include "rt_internal.h"

#define PYADAMAS_DQ_MAGIC 0x4451455500000001LL /* "DQEU" */

struct PyAdamasDeque {
    int64_t magic;
    int64_t* data;
    int64_t cap;       /* 2 的幂 */
    int64_t head;      /* 第一个元素在 data 里的下标 */
    int64_t len;
    int64_t elem_kind; /* 0 非堆、1 堆、2 保守（都按堆处理） */
    int64_t maxlen;    /* 0 表示不限制 */
};

void dq_destroy(void* p) {
    PyAdamasDeque* d = (PyAdamasDeque*)p;
    if (d->elem_kind != 0 && d->data) {
        for (int64_t i = 0; i < d->len; ++i)
            pa_own_dec(d->elem_kind, d->data[(d->head + i) & (d->cap - 1)]);
    }
    free(d->data);
}

static int64_t dq_at(const PyAdamasDeque* d, int64_t i) {
    return d->data[(d->head + i) & (d->cap - 1)];
}

static void dq_set(PyAdamasDeque* d, int64_t i, int64_t v) {
    d->data[(d->head + i) & (d->cap - 1)] = v;
}

static void dq_grow(PyAdamasDeque* d) {
    int64_t ncap = d->cap * 2;
    int64_t* nd = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
    if (!nd) panic_c("out of memory");
    for (int64_t i = 0; i < d->len; ++i) nd[i] = dq_at(d, i);
    free(d->data);
    d->data = nd;
    d->cap = ncap;
    d->head = 0;
}

PyAdamasDeque* pa_deque_new(int64_t maxlen) {
    if (maxlen < 0) pa_raise_c("ValueError", "maxlen must be non-negative");
    PyAdamasDeque* d =
        (PyAdamasDeque*)rc_alloc((int64_t)sizeof(PyAdamasDeque), dq_destroy, NULL, PYADAMAS_RC_OBJ_DEQUE);
    d->magic = PYADAMAS_DQ_MAGIC;
    d->cap = 8;
    d->data = (int64_t*)malloc(sizeof(int64_t) * (size_t)d->cap);
    if (!d->data) panic_c("out of memory");
    d->head = 0;
    d->len = 0;
    d->elem_kind = 0;
    d->maxlen = maxlen;
    return d;
}

/* 从列表构造：拷贝元素并沿用它的 elem_kind。 */
PyAdamasDeque* pa_deque_from_list(PyAdamasList* items, int64_t maxlen) {
    PyAdamasDeque* d = pa_deque_new(maxlen);
    if (items) d->elem_kind = items->elem_kind;
    pa_deque_extend(d, items);
    return d;
}

/* 追加：满了先丢最旧的（和 CPython 的 maxlen 语义一致）。 */
void pa_deque_append(PyAdamasDeque* d, int64_t v) {
    if (d->maxlen > 0 && d->len == d->maxlen) {
        int64_t old = dq_at(d, 0);
        d->head = (d->head + 1) & (d->cap - 1);
        --d->len;
        pa_own_dec(d->elem_kind, old);
    }
    if (d->len == d->cap) dq_grow(d);
    dq_set(d, d->len, v);
    ++d->len;
}

void pa_deque_appendleft(PyAdamasDeque* d, int64_t v) {
    if (d->maxlen > 0 && d->len == d->maxlen) {
        int64_t old = dq_at(d, d->len - 1);
        --d->len;
        pa_own_dec(d->elem_kind, old);
    }
    if (d->len == d->cap) dq_grow(d);
    d->head = (d->head - 1) & (d->cap - 1);
    d->data[d->head] = v;
    ++d->len;
}

/* pop / popleft 返回转移引用（调用方拿到一份）。 */
int64_t pa_deque_pop(PyAdamasDeque* d) {
    if (d->len == 0) pa_raise_c("IndexError", "pop from an empty deque");
    int64_t v = dq_at(d, d->len - 1);
    --d->len;
    return v;
}

int64_t pa_deque_popleft(PyAdamasDeque* d) {
    if (d->len == 0) pa_raise_c("IndexError", "pop from an empty deque");
    int64_t v = dq_at(d, 0);
    d->head = (d->head + 1) & (d->cap - 1);
    --d->len;
    return v;
}

int64_t pa_deque_len(PyAdamasDeque* d) { return d->len; }

int64_t pa_deque_maxlen(PyAdamasDeque* d) { return d->maxlen > 0 ? d->maxlen : -1; }

/* 下标读：返回借用引用（和 list 的 IndexLoad 一致）。 */
int64_t pa_deque_get(PyAdamasDeque* d, int64_t i) {
    if (i < 0) i += d->len;
    if (i < 0 || i >= d->len) pa_raise_c("IndexError", "deque index out of range");
    return dq_at(d, i);
}

void pa_deque_set(PyAdamasDeque* d, int64_t i, int64_t v) {
    if (i < 0) i += d->len;
    if (i < 0 || i >= d->len) pa_raise_c("IndexError", "deque assignment index out of range");
    int64_t old = dq_at(d, i);
    dq_set(d, i, v);
    pa_own_dec(d->elem_kind, old);
}

/* rotate(n)：正数右旋（尾部转到头部），负数左旋。 */
void pa_deque_rotate(PyAdamasDeque* d, int64_t n) {
    if (d->len < 2) return;
    n %= d->len;
    if (n < 0) n += d->len;
    if (n == 0) return;
    int64_t* tmp = (int64_t*)malloc(sizeof(int64_t) * (size_t)d->len);
    if (!tmp) panic_c("out of memory");
    int64_t start = d->len - n;
    for (int64_t i = 0; i < d->len; ++i) tmp[i] = dq_at(d, (start + i) % d->len);
    for (int64_t i = 0; i < d->len; ++i) dq_set(d, i, tmp[i]);
    free(tmp);
}

void pa_deque_clear(PyAdamasDeque* d) {
    if (d->elem_kind != 0) {
        for (int64_t i = 0; i < d->len; ++i)
            pa_own_dec(d->elem_kind, dq_at(d, i));
    }
    d->head = 0;
    d->len = 0;
}

/* extend：尾部逐个追加（保持顺序）。 */
void pa_deque_extend(PyAdamasDeque* d, PyAdamasList* items) {
    if (!items) return;
    if (items->elem_kind > d->elem_kind) d->elem_kind = items->elem_kind;
    for (int64_t i = 0; i < items->len; ++i) {
        int64_t v = items->data[i];
        pa_own_inc(items->elem_kind, v);
        pa_deque_append(d, v);
    }
}

/* extendleft：逐个 appendleft，结果是逆序（和 CPython 一致）。 */
void pa_deque_extendleft(PyAdamasDeque* d, PyAdamasList* items) {
    if (!items) return;
    if (items->elem_kind > d->elem_kind) d->elem_kind = items->elem_kind;
    for (int64_t i = 0; i < items->len; ++i) {
        int64_t v = items->data[i];
        pa_own_inc(items->elem_kind, v);
        pa_deque_appendleft(d, v);
    }
}

/* to_list：按当前顺序复制成列表。 */
PyAdamasList* pa_deque_to_list(PyAdamasDeque* d) {
    PyAdamasList* out = pa_list_new(d->len, d->elem_kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = dq_at(d, i);
        pa_own_inc(d->elem_kind, v);
        pa_list_append(out, v, 2);
    }
    return out;
}

int64_t pa_deque_count(PyAdamasDeque* d, int64_t v, int64_t kind) {
    int64_t n = 0;
    for (int64_t i = 0; i < d->len; ++i)
        if (list_value_eq(kind, dq_at(d, i), v)) ++n;
    return n;
}

int64_t pa_deque_index(PyAdamasDeque* d, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < d->len; ++i)
        if (list_value_eq(kind, dq_at(d, i), v)) return i;
    pa_raise_c("ValueError", "value is not in deque");
}

void pa_deque_remove(PyAdamasDeque* d, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < d->len; ++i) {
        if (!list_value_eq(kind, dq_at(d, i), v)) continue;
        int64_t old = dq_at(d, i);
        for (int64_t j = i; j + 1 < d->len; ++j) dq_set(d, j, dq_at(d, j + 1));
        --d->len;
        pa_own_dec(d->elem_kind, old);
        return;
    }
    pa_raise_c("ValueError", "deque.remove(x): x not in deque");
}

void pa_deque_insert(PyAdamasDeque* d, int64_t i, int64_t v) {
    if (d->maxlen > 0 && d->len == d->maxlen)
        pa_raise_c("IndexError", "deque already at its maximum size");
    if (i < 0) i += d->len;
    if (i < 0) i = 0;
    if (i > d->len) i = d->len;
    if (d->len == d->cap) dq_grow(d);
    for (int64_t j = d->len; j > i; --j) dq_set(d, j, dq_at(d, j - 1));
    dq_set(d, i, v);
    ++d->len;
}

void pa_deque_reverse(PyAdamasDeque* d) {
    for (int64_t i = 0, j = d->len - 1; i < j; ++i, --j) {
        int64_t a = dq_at(d, i);
        int64_t b = dq_at(d, j);
        dq_set(d, i, b);
        dq_set(d, j, a);
    }
}

PyAdamasDeque* pa_deque_copy(PyAdamasDeque* d) {
    PyAdamasDeque* out = pa_deque_new(d->maxlen);
    out->elem_kind = d->elem_kind;
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = dq_at(d, i);
        pa_own_inc(d->elem_kind, v);
        pa_deque_append(out, v);
    }
    return out;
}
