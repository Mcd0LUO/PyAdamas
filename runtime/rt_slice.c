// rt_slice.c —— 序列查找、切片与 range
#include "rt_internal.h"

int64_t py23_str_contains(Py23Str* hay, Py23Str* needle) {
    if (needle->len == 0) return 1;
    if (needle->len > hay->len) return 0;
    return py23_find_bytes(hay->data, hay->len, needle->data, needle->len, 0) >= 0;
}

/* ── 切片 ─────────────────────────────────────────────────────── */

/* 缺省边界用 INT64_MIN 当哨兵；显式写 INT64_MIN 的极端索引会被当成缺省。 */
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step,
                         int64_t* out_start, int64_t* out_stop, int64_t* out_step) {
    int has_start = start != INT64_MIN;
    int has_stop = stop != INT64_MIN;
    int has_step = step != INT64_MIN;
    if (!has_step) step = 1;
    if (step == 0) py23_raise_c("ValueError", "slice step cannot be zero");

    if (step > 0) {
        if (!has_start) start = 0;
        else {
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start > len) start = len;
        }
        if (!has_stop) stop = len;
        else {
            if (stop < 0) stop += len;
            if (stop < 0) stop = 0;
            if (stop > len) stop = len;
        }
    } else {
        if (!has_start) start = len - 1;
        else {
            if (start < 0) start += len;
            if (start < 0) start = -1;
            if (start >= len) start = len - 1;
        }
        if (!has_stop) stop = -1;
        else {
            if (stop < 0) stop += len;
            if (stop < 0) stop = -1;
            if (stop >= len) stop = len - 1;
        }
    }
    *out_start = start;
    *out_stop = stop;
    *out_step = step;
}

int64_t slice_total(int64_t start, int64_t stop, int64_t step) {
    if (step > 0) {
        if (start >= stop) return 0;
        return (stop - start + step - 1) / step;
    }
    if (start <= stop) return 0;
    return (start - stop + (-step) - 1) / (-step);
}

Py23List* py23_list_slice(Py23List* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    Py23List* out = py23_list_new(count, src->elem_kind);
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) {
        int64_t v = src->data[i];
        if (src->elem_kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23Str* py23_str_slice(Py23Str* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    /* 整串切片和单元素切片都很常见：前者共享原串，后者直接取驻留字符。 */
    if (st == 1 && lo == 0 && hi == src->len) {
        py23_rc_inc(src);
        return src;
    }
    if (count == 1) return py23_char_str((unsigned char)src->data[lo]);
    Py23Str* out = str_alloc(count);
    if (st == 1) {
        if (count) memcpy((char*)out->data, src->data + lo, (size_t)count);
        return out;
    }
    char* p = (char*)out->data;
    int64_t j = 0;
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) p[j++] = src->data[i];
    return out;
}

/* 切片赋值。src 的元素引用由本函数接管：kind 1 时对新元素 inc、
   对被移除的旧元素 dec；src == dst 时先做快照，避免边读边写。 */
void py23_list_slice_assign(Py23List* dst, int64_t start, int64_t stop, int64_t step,
                            Py23List* src) {
    int64_t lo, hi, st;
    slice_bounds(dst->len, start, stop, step, &lo, &hi, &st);
    int64_t n = slice_total(lo, hi, st);
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    int64_t slen = src->len;
    if (src == dst) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    int kind = (int)dst->elem_kind;
    if (st != 1) {
        if (slen != n) py23_raise_c("ValueError", "cannot assign sequence of different size to extended slice");
        for (int64_t i = 0; i < n; ++i) {
            int64_t idx = lo + i * st;
            int64_t v = sv[i];
            int64_t old = dst->data[idx];
            if (kind == 1 && v) py23_rc_inc((void*)v);
            else if (kind == 2 && src->elem_kind == 1 && v) py23_rc_inc((void*)v);
            dst->data[idx] = v;
            if (kind == 1 && old) py23_rc_dec((void*)old);
        }
        free(snap);
        return;
    }
    int64_t delta = slen - n;
    if (delta > 0) {
        if (dst->len + delta > dst->cap) {
            int64_t ncap = dst->cap ? dst->cap : 4;
            while (ncap < dst->len + delta) ncap *= 2;
            int64_t* nd = (int64_t*)realloc(dst->data, sizeof(int64_t) * (size_t)ncap);
            if (!nd) panic_c("out of memory");
            dst->data = nd;
            dst->cap = ncap;
        }
        memmove(&dst->data[hi + delta], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    } else if (delta < 0) {
        // [lo+slen, hi) 的旧元素会被下面的 memmove 覆盖，先释放。
        if (kind == 1)
            for (int64_t i = lo + slen; i < hi; ++i)
                if (dst->data[i]) py23_rc_dec((void*)dst->data[i]);
        memmove(&dst->data[lo + slen], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        int64_t old = dst->data[lo + i];
        if (kind == 1 && v) py23_rc_inc((void*)v);
        else if (kind == 2 && src->elem_kind == 1 && v) py23_rc_inc((void*)v);
        dst->data[lo + i] = v;
        // 增长时 memmove 会把尾部元素复制到 [hi, hi+delta)，这些副本
        // 没有独立的引用计数；只有原区间 [lo, hi) 的旧元素归本列表所有。
        if (kind == 1 && old && (delta <= 0 || lo + i < hi))
            py23_rc_dec((void*)old);
    }
    free(snap);
}

Py23List* py23_str_to_list(Py23Str* s) {
    Py23List* out = py23_list_new(s->len, 1);
    for (int64_t i = 0; i < s->len; ++i)
        py23_list_append(out, (int64_t)py23_str_char_at(s, i));
    return out;
}

/* bytes 迭代：每个元素是 0..255 的 int，不是单字节串。 */
Py23List* py23_bytes_to_list(Py23Str* s) {
    Py23List* out = py23_list_new(s->len, 0);
    for (int64_t i = 0; i < s->len; ++i)
        py23_list_append(out, (int64_t)(unsigned char)s->data[i]);
    return out;
}

/* 把单个字符串包成 [s]：subprocess.Popen("cmd", shell=True) 这种写法用。 */
Py23List* py23_str_wrap_list(Py23Str* s) {
    Py23List* out = py23_list_new(1, 1);
    py23_rc_inc(s);
    py23_list_append(out, (int64_t)s);
    return out;
}

/* ── range ─────────────────────────────────────────────────────── */

/* range 是惰性对象：只存边界，元素按需算。CPython 的 range 也是这样——
   range(10**9) 只占几十字节，不会当场造十亿个元素。
   以前这里是把 range 实体化成整数列表，range(10**9) 就是 8GB 内存。 */
Py23Range* py23_range_new(int64_t start, int64_t stop, int64_t step) {
    if (step == 0) py23_raise_c("ValueError", "range() arg 3 must not be zero");
    int64_t n;
    // 写成 (stop - start - 1) / step + 1：比 (stop - start + step - 1) / step
    // 少一次加法，边界上不容易溢出。
    if (step > 0)
        n = stop > start ? (stop - start - 1) / step + 1 : 0;
    else
        n = stop < start ? (start - stop - 1) / (-step) + 1 : 0;
    Py23Range* r = (Py23Range*)rc_alloc((int64_t)sizeof(Py23Range), NULL, NULL,
                                        PY23_RC_RANGE);
    r->start = start;
    r->stop = stop;
    r->step = step;
    r->len = n;
    return r;
}

int64_t py23_range_len(Py23Range* r) { return r->len; }

int64_t py23_range_first(Py23Range* r) { return r->start; }

int64_t py23_range_step(Py23Range* r) { return r->step; }

int64_t py23_range_stop(Py23Range* r) { return r->stop; }

int64_t py23_range_get(Py23Range* r, int64_t i) {
    if (i < 0) i += r->len;
    if (i < 0 || i >= r->len)
        py23_raise_c("IndexError", "range object index out of range");
    return r->start + i * r->step;
}

int64_t py23_range_contains(Py23Range* r, int64_t v) {
    if (r->len == 0) return 0;
    if (r->step > 0) {
        if (v < r->start || v >= r->stop) return 0;
    } else {
        if (v > r->start || v <= r->stop) return 0;
    }
    return ((v - r->start) % r->step) == 0 ? 1 : 0;
}

int64_t py23_range_eq(Py23Range* a, Py23Range* b) {
    if (a == b) return 1;
    return (a->start == b->start && a->stop == b->stop && a->step == b->step) ? 1 : 0;
}

int64_t py23_range_sum_i64(Py23Range* r, int64_t start) {
    int64_t s = start;
    for (int64_t i = 0; i < r->len; ++i) s += r->start + i * r->step;
    return s;
}

double py23_range_sum_f64(Py23Range* r, double start) {
    double s = start;
    for (int64_t i = 0; i < r->len; ++i) s += (double)(r->start + i * r->step);
    return s;
}

int64_t py23_range_min_i64(Py23Range* r) {
    if (r->len == 0) py23_raise_c("ValueError", "min() iterable argument is empty");
    return r->step > 0 ? r->start : r->start + (r->len - 1) * r->step;
}

int64_t py23_range_max_i64(Py23Range* r) {
    if (r->len == 0) py23_raise_c("ValueError", "max() iterable argument is empty");
    return r->step > 0 ? r->start + (r->len - 1) * r->step : r->start;
}

int64_t py23_range_any(Py23Range* r) {
    for (int64_t i = 0; i < r->len; ++i)
        if (r->start + i * r->step) return 1;
    return 0;
}

int64_t py23_range_all(Py23Range* r) {
    for (int64_t i = 0; i < r->len; ++i)
        if (!(r->start + i * r->step)) return 0;
    return 1;
}

/* 需要一个真列表时（list(range(n)) / sorted(range(n))）才物化。 */
Py23List* py23_range_to_list(Py23Range* r) {
    Py23List* out = py23_list_new(r->len, 0);
    for (int64_t i = 0; i < r->len; ++i)
        py23_list_append(out, r->start + i * r->step);
    return out;
}

Py23Str* py23_range_repr(Py23Range* r) {
    char buf[96];
    int n;
    if (r->step == 1)
        n = snprintf(buf, sizeof buf, "range(%lld, %lld)", (long long)r->start,
                     (long long)r->stop);
    else
        n = snprintf(buf, sizeof buf, "range(%lld, %lld, %lld)", (long long)r->start,
                     (long long)r->stop, (long long)r->step);
    return str_new(buf, (int64_t)n);
}

