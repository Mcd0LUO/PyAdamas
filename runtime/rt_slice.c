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
    /* 0.303：heap 提示用源的运行期种类（准确），避免 append 的种类维护
       对 kind0/1 列表逐元素探针。 */
    int sk = src->elem_kind == 1 ? 1 : (src->elem_kind == 0 ? 0 : 2);
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) {
        int64_t v = src->data[i];
        pa_own_inc(src->elem_kind, v);
        pa_list_append(out, v, sk);
    }
    return out;
}

/* 从码点首字节 off 起推进 st 个码点（st 可负），返回新的码点首字节偏移。
   调用方保证结果不越界（count 已由 slice_total 算好）。 */
static int64_t cp_step(const PyAdamasStr* s, int64_t off, int64_t st) {
    if (st > 0) {
        for (int64_t k = 0; k < st && off < s->len; ++k)
            off += str_seq_len((unsigned char)s->data[off]);
        return off;
    }
    for (int64_t k = 0; k < -st && off > 0; ++k) off = str_prev_cp(s, off);
    return off;
}

/* str 切片：边界在码点域归一化（slice_bounds 用 pub），再换算成字节偏移。
   步长 1 用一次单调扫描（最多扫到 hi 对应的字节位置）；步长 != 1 按码点
   逐个推进。复杂度 O(字节长) 扫描 + O(结果字节) 拷贝。 */
PyAdamasStr* pa_str_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->pub, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    /* 整串切片和单元素切片都很常见：前者共享原串，后者直接取驻留字符。 */
    if (st == 1 && lo == 0 && hi == src->pub) {
        pa_rc_inc(src);
        return src;
    }
    if (count == 1) {
        int64_t off = str_cp_to_byte(src, lo);
        int n = 0;
        uint32_t cp = str_cp_at(src, off, &n);
        if (cp < 0x80) return pa_char_str((int64_t)cp);
        PyAdamasStr* one = str_alloc_raw(n);
        memcpy((char*)one->data, src->data + off, (size_t)n);
        return str_seal_n(one, 1);
    }
    if (count == 0) return str_new("", 0);
    /* 先求两端字节偏移：一次从 lo 扫到 hi（st>0 时单调），避免每码点重扫。 */
    int64_t blo = str_cp_to_byte(src, lo);
    if (st == 1) {
        int64_t bhi = str_cp_to_byte(src, hi);
        PyAdamasStr* out = str_alloc_raw(bhi - blo);
        memcpy((char*)out->data, src->data + blo, (size_t)(bhi - blo));
        return str_seal_n(out, count);
    }
    /* 步长 != 1：先把结果字节数算出来（一遍），再逐码点拷（一遍）。
       每个选中码点之间要跳过 |st| 个码点，不是 1 个。 */
    int64_t need = 0;
    {
        int64_t off = blo;
        for (int64_t k = 0; k < count; ++k) {
            need += str_seq_len((unsigned char)src->data[off]);
            off = cp_step(src, off, st);
        }
    }
    PyAdamasStr* out = str_alloc_raw(need);
    char* p = (char*)out->data;
    int64_t j = 0;
    int64_t off = blo;
    for (int64_t k = 0; k < count; ++k) {
        int n = str_seq_len((unsigned char)src->data[off]);
        memcpy(p + j, src->data + off, (size_t)n);
        j += n;
        off = cp_step(src, off, st);
    }
    return str_seal_n(out, count);
}

/* bytes 切片：纯字节语义，与改动前完全一致（pub == len）。 */
PyAdamasStr* pa_bytes_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(src->len, start, stop, step, &lo, &hi, &st);
    int64_t count = slice_total(lo, hi, st);
    if (st == 1 && lo == 0 && hi == src->len) {
        pa_rc_inc(src);
        return src;
    }
    PyAdamasStr* out = str_alloc_raw(count);
    if (st == 1) {
        if (count) memcpy((char*)out->data, src->data + lo, (size_t)count);
        return str_seal_n(out, count);
    }
    char* p = (char*)out->data;
    int64_t j = 0;
    for (int64_t i = lo; st > 0 ? i < hi : i > hi; i += st) p[j++] = src->data[i];
    return str_seal_n(out, count);
}

/* 切片赋值。src 的元素引用由本函数接管：kind 1 时对新元素 inc、
   对被移除的旧元素 dec；src == dst 时先做快照，避免边读边写。 */
void pa_list_slice_assign(PyAdamasList* dst, int64_t start, int64_t stop, int64_t step,
                            PyAdamasList* src) {
    int64_t lo, hi, st;
    slice_bounds(dst->len, start, stop, step, &lo, &hi, &st);
    /* 步长 1 且 start > stop 时切片为空，插入点是 start（CPython 口径）。
       以前直接用 hi（可能被 clamp 到 0）做插入点：l[-3:-8] = [90] 会把整个
       前缀右移、再写到 lo，结果列表多出一个元素（静默错值）。 */
    if (st > 0 && lo > hi) hi = lo;
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
    // 目标和源的种类合并：目标未定型（3）时采用源的种类；任一方是
    // unknown/混合（2）或两者冲突（0/1）时降级为 2，旧元素和新元素都按
    // 注册表保守释放。
    int sk = (int)src->elem_kind;
    int dk = (int)dst->elem_kind;
    if (sk != 3) {
        if (dk == 3) dst->elem_kind = sk;
        else if (dk != sk) dst->elem_kind = 2;
    }
    /* 0.303：切片赋值移动/覆盖元素，kind2 位图无法逐位维护，保守丢弃
       回退注册表（源是空表时也成立 —— 收缩同样移动尾部元素）。
       kind0 untracked 容器升级成 2 后补进 GC 链（可能成环）。 */
    if (dst->elem_kind == 2) {
        pa_list_drop_bits(dst);
        if (dk == 0) pa_rc_track(dst);
    }
    int kind = (int)dst->elem_kind;
    if (st != 1) {
        if (slen != n) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "attempt to assign sequence of size %lld to extended slice of size %lld",
                     (long long)slen, (long long)n);
            pa_raise_c("ValueError", msg);
        }
        for (int64_t i = 0; i < n; ++i) {
            int64_t idx = lo + i * st;
            int64_t v = sv[i];
            int64_t old = dst->data[idx];
            if (kind != 0) pa_own_inc(sk, v);
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
            list_buf_grow(dst, ncap);
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
        if (kind != 0) pa_own_inc(sk, v);
        dst->data[lo + i] = v;
        // 增长时 memmove 会把尾部元素复制到 [hi, hi+delta)，这些副本
        // 没有独立的引用计数；只有原区间 [lo, hi) 的旧元素归本列表所有。
        if (kind != 0 && (delta <= 0 || lo + i < hi)) pa_own_dec(kind, old);
    }
    free(snap);
}

/* del 切片（del a[start:stop:step]）。以前 del 走"赋空序列"的路径：
   扩展步长会撞上长度不匹配检查，直接抛 ValueError（del a[::2] 无法工作）。
   这里按 CPython 语义先选出被删下标，再原地压缩保留元素。 */
void pa_list_slice_del(PyAdamasList* dst, int64_t start, int64_t stop, int64_t step) {
    int64_t lo, hi, st;
    slice_bounds(dst->len, start, stop, step, &lo, &hi, &st);
    if (st > 0 ? lo >= hi : lo <= hi) return;  /* 空区间 */
    int kind = (int)dst->elem_kind;
    int64_t w = 0;
    for (int64_t i = 0; i < dst->len; ++i) {
        int hit = st > 0 ? (i >= lo && i < hi && (i - lo) % st == 0)
                         : (i <= lo && i > hi && (lo - i) % (-st) == 0);
        if (hit) {
            if (kind != 0) pa_own_dec(kind, dst->data[i]);
            continue;
        }
        if (w != i) dst->data[w] = dst->data[i];
        ++w;
    }
    dst->len = w;
    if (kind == 2) pa_list_drop_bits(dst);  /* 移动元素后位图不可信 */
}

/* str -> list[str]：按码点拆。内部走单遍字节游标（O(n)），不要逐个
   pa_str_char_at（那会每轮从头扫，退化 O(n^2)）。 */
PyAdamasList* pa_str_to_list(PyAdamasStr* s) {
    PyAdamasList* out = pa_list_new(s->pub, 1);
    pa_gc_pin(out);
    int64_t off = 0;
    while (off < s->len) {
        int n = 0;
        uint32_t cp = str_cp_at(s, off, &n);
        PyAdamasStr* ch;
        if (cp < 0x80) {
            ch = pa_char_str((int64_t)cp);
        } else {
            ch = str_alloc_raw(n);
            memcpy((char*)ch->data, s->data + off, (size_t)n);
            ch->pub = 1;
        }
        pa_list_append(out, (int64_t)ch, 1);  /* 0.303：字符必是堆串 */
        off += n;
    }
    pa_gc_unpin(out);
    return out;
}

/* bytes 迭代：每个元素是 0..255 的 int，不是单字节串。 */
PyAdamasList* pa_bytes_to_list(PyAdamasStr* s) {
    PyAdamasList* out = pa_list_new(s->len, 0);
    for (int64_t i = 0; i < s->len; ++i)
        pa_list_append(out, (int64_t)(unsigned char)s->data[i], 0);  /* 0.303 */
    return out;
}

/* list[int] -> bytes；越界元素与 CPython 一样抛 ValueError。 */
PyAdamasStr* pa_bytes_from_list(PyAdamasList* items) {
    int64_t n = items ? items->len : 0;
    PyAdamasStr* s = str_alloc_raw(n);
    char* out = (char*)s->data;
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = pa_list_get(items, i);
        if (v < 0 || v > 255) pa_raise_c("ValueError", "bytes must be in range(0, 256)");
        out[i] = (char)(unsigned char)v;
    }
    return str_seal_n(s, n);  /* bytes：pub == 字节数 */
}

/* bytes(n)：n 个零字节。 */
PyAdamasStr* pa_bytes_zeros(int64_t n) {
    if (n <= 0) return str_new_bytes("", 0);
    PyAdamasStr* s = str_alloc_raw(n);
    /* str_alloc_raw 不清零 payload（契约：调用方负责填满字符区），
       零字节缓冲区在这里显式清零。 */
    memset((char*)s->data, 0, (size_t)n);
    return str_seal_n(s, n);
}

/* 把单个字符串包成 [s]：subprocess.Popen("cmd", shell=True) 这种写法用。 */
PyAdamasList* pa_str_wrap_list(PyAdamasStr* s) {
    PyAdamasList* out = pa_list_new(1, 1);
    pa_rc_inc(s);
    pa_list_append(out, (int64_t)s, 1);  /* 0.303 */
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
        pa_list_append(out, r->start + i * r->step, 0);  /* 0.303 */
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

