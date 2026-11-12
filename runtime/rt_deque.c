// rt_deque.c —— collections.deque：环形缓冲的双端队列
#include "rt_internal.h"

#define PY23_DQ_MAGIC 0x4451455500000001LL /* "DQEU" */

struct Py23Deque {
    int64_t magic;
    int64_t* data;
    int64_t cap;       /* 2 的幂 */
    int64_t head;      /* 第一个元素在 data 里的下标 */
    int64_t len;
    int64_t elem_kind; /* 0 非堆、1 堆、2 保守（都按堆处理） */
    int64_t maxlen;    /* 0 表示不限制 */
};

static void dq_destroy(void* p) {
    Py23Deque* d = (Py23Deque*)p;
    if (d->elem_kind == 1 && d->data) {
        for (int64_t i = 0; i < d->len; ++i) {
            int64_t v = d->data[(d->head + i) & (d->cap - 1)];
            if (v) py23_rc_dec((void*)v);
        }
    }
    free(d->data);
}

static int64_t dq_at(const Py23Deque* d, int64_t i) {
    return d->data[(d->head + i) & (d->cap - 1)];
}

static void dq_set(Py23Deque* d, int64_t i, int64_t v) {
    d->data[(d->head + i) & (d->cap - 1)] = v;
}

static void dq_grow(Py23Deque* d) {
    int64_t ncap = d->cap * 2;
    int64_t* nd = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
    if (!nd) panic_c("out of memory");
    for (int64_t i = 0; i < d->len; ++i) nd[i] = dq_at(d, i);
    free(d->data);
    d->data = nd;
    d->cap = ncap;
    d->head = 0;
}

Py23Deque* py23_deque_new(int64_t maxlen) {
    if (maxlen < 0) py23_raise_c("ValueError", "maxlen must be non-negative");
    Py23Deque* d =
        (Py23Deque*)rc_alloc((int64_t)sizeof(Py23Deque), dq_destroy, NULL, PY23_RC_OBJ);
    d->magic = PY23_DQ_MAGIC;
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
Py23Deque* py23_deque_from_list(Py23List* items, int64_t maxlen) {
    Py23Deque* d = py23_deque_new(maxlen);
    if (items) d->elem_kind = items->elem_kind;
    py23_deque_extend(d, items);
    return d;
}

/* 追加：满了先丢最旧的（和 CPython 的 maxlen 语义一致）。 */
void py23_deque_append(Py23Deque* d, int64_t v) {
    if (d->maxlen > 0 && d->len == d->maxlen) {
        int64_t old = dq_at(d, 0);
        d->head = (d->head + 1) & (d->cap - 1);
        --d->len;
        if (d->elem_kind == 1 && old) py23_rc_dec((void*)old);
    }
    if (d->len == d->cap) dq_grow(d);
    dq_set(d, d->len, v);
    ++d->len;
}

void py23_deque_appendleft(Py23Deque* d, int64_t v) {
    if (d->maxlen > 0 && d->len == d->maxlen) {
        int64_t old = dq_at(d, d->len - 1);
        --d->len;
        if (d->elem_kind == 1 && old) py23_rc_dec((void*)old);
    }
    if (d->len == d->cap) dq_grow(d);
    d->head = (d->head - 1) & (d->cap - 1);
    d->data[d->head] = v;
    ++d->len;
}

/* pop / popleft 返回转移引用（调用方拿到一份）。 */
int64_t py23_deque_pop(Py23Deque* d) {
    if (d->len == 0) py23_raise_c("IndexError", "pop from an empty deque");
    int64_t v = dq_at(d, d->len - 1);
    --d->len;
    return v;
}

int64_t py23_deque_popleft(Py23Deque* d) {
    if (d->len == 0) py23_raise_c("IndexError", "pop from an empty deque");
    int64_t v = dq_at(d, 0);
    d->head = (d->head + 1) & (d->cap - 1);
    --d->len;
    return v;
}

int64_t py23_deque_len(Py23Deque* d) { return d->len; }

int64_t py23_deque_maxlen(Py23Deque* d) { return d->maxlen > 0 ? d->maxlen : -1; }

/* 下标读：返回借用引用（和 list 的 IndexLoad 一致）。 */
int64_t py23_deque_get(Py23Deque* d, int64_t i) {
    if (i < 0) i += d->len;
    if (i < 0 || i >= d->len) py23_raise_c("IndexError", "deque index out of range");
    return dq_at(d, i);
}

void py23_deque_set(Py23Deque* d, int64_t i, int64_t v) {
    if (i < 0) i += d->len;
    if (i < 0 || i >= d->len) py23_raise_c("IndexError", "deque assignment index out of range");
    int64_t old = dq_at(d, i);
    dq_set(d, i, v);
    if (d->elem_kind == 1 && old) py23_rc_dec((void*)old);
}

/* rotate(n)：正数右旋（尾部转到头部），负数左旋。 */
void py23_deque_rotate(Py23Deque* d, int64_t n) {
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

void py23_deque_clear(Py23Deque* d) {
    if (d->elem_kind == 1) {
        for (int64_t i = 0; i < d->len; ++i) {
            int64_t v = dq_at(d, i);
            if (v) py23_rc_dec((void*)v);
        }
    }
    d->head = 0;
    d->len = 0;
}

/* extend：尾部逐个追加（保持顺序）。 */
void py23_deque_extend(Py23Deque* d, Py23List* items) {
    if (!items) return;
    for (int64_t i = 0; i < items->len; ++i) {
        int64_t v = items->data[i];
        if (items->elem_kind == 1 && v) py23_rc_inc((void*)v);
        py23_deque_append(d, v);
    }
}

/* extendleft：逐个 appendleft，结果是逆序（和 CPython 一致）。 */
void py23_deque_extendleft(Py23Deque* d, Py23List* items) {
    if (!items) return;
    for (int64_t i = 0; i < items->len; ++i) {
        int64_t v = items->data[i];
        if (items->elem_kind == 1 && v) py23_rc_inc((void*)v);
        py23_deque_appendleft(d, v);
    }
}

/* to_list：按当前顺序复制成列表。 */
Py23List* py23_deque_to_list(Py23Deque* d) {
    Py23List* out = py23_list_new(d->len, d->elem_kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = dq_at(d, i);
        if (d->elem_kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}
