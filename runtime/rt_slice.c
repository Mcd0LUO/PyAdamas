// rt_slice.c —— 序列查找、切片与 range
#include "rt_internal.h"

int64_t pa_str_contains(PyAdamasStr* hay, PyAdamasStr* needle) {
    if (needle->len == 0) return 1;
    if (needle->len > hay->len) return 0;
    return pa_find_bytes(hay->data, hay->len, needle->data, needle->len, 0) >= 0;
}

/* ── 切片 ─────────────────────────────────────────────────────── */

/* 缺省边界用 INT64_MIN 当哨兵；显式写 INT64_MIN 的极端索引会被当成缺省。 */
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step,
                         int64_t* out_start, int64_t* out_stop, int64_t* out_step) {
    int has_start = start != INT64_MIN;
    int has_stop = stop != INT64_MIN;
    int has_step = step != INT64_MIN;
    if (!has_step) step = 1;
    if (step == 0) pa_raise_c("ValueError", "slice step cannot be zero");

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

PyAdamasList* pa_list_slice(PyAdamasList* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    PyAdamasList* out = pa_list_new(count, src->elem_kind);
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) {
        int64_t v = src->data[i];
        pa_own_inc(src->elem_kind, v);
        pa_list_append(out, v);
    }
    return out;
}

PyAdamasStr* pa_str_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    /* 整串切片和单元素切片都很常见：前者共享原串，后者直接取驻留字符。 */
    if (st == 1 && lo == 0 && hi == src->len) {
        pa_rc_inc(src);
        return src;
    }
    if (count == 1) return pa_char_str((unsigned char)src->data[lo]);
    PyAdamasStr* out = str_alloc(count);
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
void pa_list_slice_assign(PyAdamasList* dst, int64_t start, int64_t stop, int64_t step,
                            PyAdamasList* src) {
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
    // 源是 unknown/混合（2）时目标也升级为 2：旧元素按注册表释放安全，
    // 新元素按来源种类 retain，销毁/覆盖时统一走保守释放。
    if (src->elem_kind == 2) dst->elem_kind = 2;
    int kind = (int)dst->elem_kind;
    if (st != 1) {
        if (slen != n) pa_raise_c("ValueError", "cannot assign sequence of different size to extended slice");
        for (int64_t i = 0; i < n; ++i) {
            int64_t idx = lo + i * st;
            int64_t v = sv[i];
            int64_t old = dst->data[idx];
            if (kind != 0) pa_own_inc(src->elem_kind, v);
            dst->data[idx] = v;
            pa_own_dec(kind, old);
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
        if (kind != 0)
            for (int64_t i = lo + slen; i < hi; ++i)
                pa_own_dec(kind, dst->data[i]);
        memmove(&dst->data[lo + slen], &dst->data[hi],
                sizeof(int64_t) * (size_t)(dst->len - hi));
        dst->len += delta;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        int64_t old = dst->data[lo + i];
        if (kind != 0) pa_own_inc(src->elem_kind, v);
        dst->data[lo + i] = v;
        // 增长时 memmove 会把尾部元素复制到 [hi, hi+delta)，这些副本
        // 没有独立的引用计数；只有原区间 [lo, hi) 的旧元素归本列表所有。
        if (kind != 0 && (delta <= 0 || lo + i < hi)) pa_own_dec(kind, old);
    }
    free(snap);
}

PyAdamasList* pa_str_to_list(PyAdamasStr* s) {
    PyAdamasList* out = pa_list_new(s->len, 1);
    for (int64_t i = 0; i < s->len; ++i)
        pa_list_append(out, (int64_t)pa_str_char_at(s, i));
    return out;
}

/* bytes 迭代：每个元素是 0..255 的 int，不是单字节串。 */
PyAdamasList* pa_bytes_to_list(PyAdamasStr* s) {
    PyAdamasList* out = pa_list_new(s->len, 0);
    for (int64_t i = 0; i < s->len; ++i)
        pa_list_append(out, (int64_t)(unsigned char)s->data[i]);
    return out;
}

/* list[int] -> bytes；越界元素与 CPython 一样抛 ValueError。 */
PyAdamasStr* pa_bytes_from_list(PyAdamasList* items) {
    int64_t n = items ? items->len : 0;
    PyAdamasStr* s = str_alloc(n);
    char* out = (char*)s->data;
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = pa_list_get(items, i);
        if (v < 0 || v > 255) pa_raise_c("ValueError", "bytes must be in range(0, 256)");
        out[i] = (char)(unsigned char)v;
    }
    return s;
}

/* bytes(n)：n 个零字节。rc_alloc 已把 payload 清零。 */
PyAdamasStr* pa_bytes_zeros(int64_t n) {
    if (n <= 0) return str_alloc(0);
    return str_alloc(n);
}

/* 把单个字符串包成 [s]：subprocess.Popen("cmd", shell=True) 这种写法用。 */
PyAdamasList* pa_str_wrap_list(PyAdamasStr* s) {
    PyAdamasList* out = pa_list_new(1, 1);
    pa_rc_inc(s);
    pa_list_append(out, (int64_t)s);
    return out;
}

/* ── range ─────────────────────────────────────────────────────── */

/* range 是惰性对象：只存边界，元素按需算。CPython 的 range 也是这样——
   range(10**9) 只占几十字节，不会当场造十亿个元素。
   以前这里是把 range 实体化成整数列表，range(10**9) 就是 8GB 内存。 */
PyAdamasRange* pa_range_new(int64_t start, int64_t stop, int64_t step) {
    if (step == 0) pa_raise_c("ValueError", "range() arg 3 must not be zero");
    int64_t n;
    // 写成 (stop - start - 1) / step + 1：比 (stop - start + step - 1) / step
    // 少一次加法，边界上不容易溢出。
    if (step > 0)
        n = stop > start ? (stop - start - 1) / step + 1 : 0;
    else
        n = stop < start ? (start - stop - 1) / (-step) + 1 : 0;
    PyAdamasRange* r = (PyAdamasRange*)rc_alloc((int64_t)sizeof(PyAdamasRange), NULL, NULL,
                                        PYADAMAS_RC_RANGE);
    r->start = start;
    r->stop = stop;
    r->step = step;
    r->len = n;
    return r;
}

int64_t pa_range_len(PyAdamasRange* r) { return r->len; }

int64_t pa_range_first(PyAdamasRange* r) { return r->start; }

int64_t pa_range_step(PyAdamasRange* r) { return r->step; }

int64_t pa_range_stop(PyAdamasRange* r) { return r->stop; }

int64_t pa_range_get(PyAdamasRange* r, int64_t i) {
    if (i < 0) i += r->len;
    if (i < 0 || i >= r->len)
        pa_raise_c("IndexError", "range object index out of range");
    return r->start + i * r->step;
}

int64_t pa_range_contains(PyAdamasRange* r, int64_t v) {
    if (r->len == 0) return 0;
    if (r->step > 0) {
        if (v < r->start || v >= r->stop) return 0;
    } else {
        if (v > r->start || v <= r->stop) return 0;
    }
    return ((v - r->start) % r->step) == 0 ? 1 : 0;
}

int64_t pa_range_eq(PyAdamasRange* a, PyAdamasRange* b) {
    if (a == b) return 1;
    return (a->start == b->start && a->stop == b->stop && a->step == b->step) ? 1 : 0;
}

int64_t pa_range_sum_i64(PyAdamasRange* r, int64_t start) {
    int64_t s = start;
    for (int64_t i = 0; i < r->len; ++i) s += r->start + i * r->step;
    return s;
}

double pa_range_sum_f64(PyAdamasRange* r, double start) {
    double s = start;
    for (int64_t i = 0; i < r->len; ++i) s += (double)(r->start + i * r->step);
    return s;
}

int64_t pa_range_min_i64(PyAdamasRange* r) {
    if (r->len == 0) pa_raise_c("ValueError", "min() iterable argument is empty");
    return r->step > 0 ? r->start : r->start + (r->len - 1) * r->step;
}

int64_t pa_range_max_i64(PyAdamasRange* r) {
    if (r->len == 0) pa_raise_c("ValueError", "max() iterable argument is empty");
    return r->step > 0 ? r->start + (r->len - 1) * r->step : r->start;
}

int64_t pa_range_any(PyAdamasRange* r) {
    for (int64_t i = 0; i < r->len; ++i)
        if (r->start + i * r->step) return 1;
    return 0;
}

int64_t pa_range_all(PyAdamasRange* r) {
    for (int64_t i = 0; i < r->len; ++i)
        if (!(r->start + i * r->step)) return 0;
    return 1;
}

/* 需要一个真列表时（list(range(n)) / sorted(range(n))）才物化。 */
PyAdamasList* pa_range_to_list(PyAdamasRange* r) {
    PyAdamasList* out = pa_list_new(r->len, 0);
    for (int64_t i = 0; i < r->len; ++i)
        pa_list_append(out, r->start + i * r->step);
    return out;
}

PyAdamasStr* pa_range_repr(PyAdamasRange* r) {
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

