/* rt_big.c —— P3 big：任意精度整数（BigInt）运行时
 *
 * 设计见 docs/numeric-types-design.md §1/§4 P3。要点：
 *
 *   * 表示：RC 头 + { int64_t nd; uint32_t d[]; }。nd 是**带符号 limb 数**
 *     （CPython ob_size 口径）：0 = 零，>0 正，<0 负；|nd| = limb 个数，
 *     基 2^32 小端，**无最高位零 limb**（规范化）。limb 内联在同一个
 *     malloc 块里（柔性数组），没有外部缓冲 —— 销毁/GC 清扫都只 free(h)，
 *     不需要 dtor/release，也不需要 realloc：对象**不可变**，一切运算
 *     产生新对象（与 CPython int 的值语义一致）。
 *
 *   * RC/GC：kind = PYADAMAS_RC_BIGINT，tracked 分配（rc_alloc）。
 *     无堆出边 → gc_scan_object 默认分支不标记子对象；不可能成环，
 *     但留在堆链/索引里让 unknown 探针（rc_index_has / 堆链回退）认识它。
 *     永生静态字面量走 pa_register_literals（与 str 字面量同口径）。
 *
 *   * 语义黄金标准 = CPython：// 向下取整、% 与除数同号、负数移位是
 *     算术右移、位运算按无限补码、** 负指数边界见 pa_big_pow。
 *     除零抛 ZeroDivisionError（可被 try/except 捕获，与 CPython 一致）。
 *
 *   * 所有入口：操作数是**借用**（调用方持有引用），返回值 rc=1
 *     （产生者引用，降级器交临时栈或槽位）。中间量用裸 malloc/free
 *     （PA_FREE_SCOPE）或 pa_gc_pin 保命，绝不把半成品 RC 对象裸露在
 *     C 局部变量里跨分配点。
 */

#include "rt_internal.h"

#include <math.h>
#include <string.h>

/* ── 对象布局与分配 ─────────────────────────────────────────────── */

#define BIG_MAX_LIMBS ((int64_t)1 << 28)   /* 2^28 limb = 8G bit，防病态 */

static PyAdamasBig* big_alloc(int64_t n) {
    if (n < 0 || n > BIG_MAX_LIMBS) panic_c("big: allocation too large");
    PyAdamasBig* r = (PyAdamasBig*)rc_alloc((int64_t)sizeof(PyAdamasBig) + n * 4,
                                            NULL, NULL, PYADAMAS_RC_BIGINT);
    r->nd = 0;
    return r;
}

static inline int64_t big_n(const PyAdamasBig* a) {
    return a->nd < 0 ? -a->nd : a->nd;
}
static inline int big_sign(const PyAdamasBig* a) {
    return (a->nd > 0) - (a->nd < 0);
}

/* 去掉最高位零 limb 并按 sign 定 nd（sign: -1/0/1，0 表示按零处理）。 */
static void big_set_sign(PyAdamasBig* r, int64_t n, int sign) {
    while (n > 0 && r->d[n - 1] == 0) --n;
    r->nd = n == 0 ? 0 : (sign < 0 ? -n : n);
}

/* 裸 limb 缓冲（中间量）：malloc，调用方 free。 */
static uint32_t* limbs_alloc(int64_t n) {
    if (n <= 0) n = 1;
    uint32_t* p = (uint32_t*)malloc((size_t)n * 4);
    if (!p) panic_c("out of memory");
    memset(p, 0, (size_t)n * 4);
    return p;
}

/* ──  magnitude 比较 / 加减 ─────────────────────────────────────── */

static int mag_cmp(const uint32_t* a, int64_t na, const uint32_t* b, int64_t nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (int64_t i = na; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* out[0..na] = a + b（na >= nb），返回进位（0/1）。out 可与 a 别名。 */
static uint32_t mag_add(uint32_t* out, const uint32_t* a, int64_t na,
                        const uint32_t* b, int64_t nb) {
    uint64_t carry = 0;
    int64_t i = 0;
    for (; i < nb; ++i) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    for (; i < na && carry; ++i) {
        uint64_t s = (uint64_t)a[i] + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (out != a)
        for (; i < na; ++i) out[i] = a[i];
    return (uint32_t)carry;
}

/* out[0..na) = a - b（要求 a >= b，na >= nb）。out 可与 a 别名。 */
static void mag_sub(uint32_t* out, const uint32_t* a, int64_t na,
                    const uint32_t* b, int64_t nb) {
    int64_t borrow = 0;
    int64_t i = 0;
    for (; i < nb; ++i) {
        int64_t t = (int64_t)a[i] - b[i] - borrow;
        out[i] = (uint32_t)t;
        borrow = t < 0 ? 1 : 0;
    }
    for (; i < na; ++i) {
        int64_t t = (int64_t)a[i] - borrow;
        out[i] = (uint32_t)t;
        borrow = t < 0 ? 1 : 0;
    }
}

/* ── 乘法（schoolbook，uint64 中间量）───────────────────────────── */

static void mag_mul(uint32_t* out, const uint32_t* a, int64_t na,
                    const uint32_t* b, int64_t nb) {
    /* out 容量 na+nb，已清零。 */
    for (int64_t i = 0; i < na; ++i) {
        uint64_t carry = 0;
        uint64_t ai = a[i];
        if (ai == 0) continue;
        for (int64_t j = 0; j < nb; ++j) {
            uint64_t cur = (uint64_t)out[i + j] + ai * b[j] + carry;
            out[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        out[i + nb] = (uint32_t)carry;
    }
}

/* ── 除法：Knuth D（多 limb 除数）+ 单 limb 快路径 ────────────────
 * q = trunc(|a| / |b|)，r = |a| - q*|b|。调用方保证 |b| > 0。
 * qbuf/rbuf 是裸缓冲（容量 na / nb），由调用方提供。 */

static void mag_divmod(const uint32_t* a, int64_t na, const uint32_t* b, int64_t nb,
                       uint32_t* q, uint32_t* r) {
    if (mag_cmp(a, na, b, nb) < 0) {
        memcpy(r, a, (size_t)na * 4);
        return;   /* q 已清零 */
    }
    if (nb == 1) {
        uint64_t rem = 0;
        uint64_t d = b[0];
        for (int64_t i = na; i-- > 0;) {
            uint64_t cur = (rem << 32) | a[i];
            q[i] = (uint32_t)(cur / d);
            rem = cur % d;
        }
        r[0] = (uint32_t)rem;
        return;
    }
    /* 规范化：左移 s 使除数最高位 >= 2^31。 */
    int s = 0;
    uint32_t top = b[nb - 1];
    while ((top & 0x80000000u) == 0) { top <<= 1; ++s; }
    int64_t m = nb, n = na;
    uint32_t* u = limbs_alloc(n + 1);
    uint32_t* v = limbs_alloc(m);
    PA_FREE_SCOPE(u);
    PA_FREE_SCOPE(v);
    if (s) {
        uint64_t carry = 0;
        for (int64_t i = 0; i < n; ++i) {
            uint64_t cur = ((uint64_t)a[i] << s) | carry;
            u[i] = (uint32_t)cur;
            carry = cur >> 32;
        }
        u[n] = (uint32_t)carry;
        carry = 0;
        for (int64_t i = 0; i < m; ++i) {
            uint64_t cur = ((uint64_t)b[i] << s) | carry;
            v[i] = (uint32_t)cur;
            carry = cur >> 32;
        }
    } else {
        memcpy(u, a, (size_t)n * 4);
        u[n] = 0;
        memcpy(v, b, (size_t)m * 4);
    }
    for (int64_t j = n - m; j >= 0; --j) {
        uint64_t num = ((uint64_t)u[j + m] << 32) | u[j + m - 1];
        uint64_t qhat = num / v[m - 1];
        uint64_t rhat = num % v[m - 1];
        while (qhat > 0xFFFFFFFFull ||
               (unsigned __int128)qhat * v[m - 2] >
                   (((unsigned __int128)rhat << 32) | u[j + m - 2])) {
            --qhat;
            rhat += v[m - 1];
            if (rhat > 0xFFFFFFFFull) break;
        }
        /* u[j..j+m] -= qhat * v */
        uint64_t carry = 0;
        int64_t borrow = 0;
        for (int64_t i = 0; i < m; ++i) {
            uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)u[j + i] - (int64_t)(uint32_t)p - borrow;
            u[j + i] = (uint32_t)t;
            borrow = t < 0 ? 1 : 0;
        }
        int64_t t = (int64_t)u[j + m] - (int64_t)carry - borrow;
        u[j + m] = (uint32_t)t;
        if (t < 0) {
            /* 加回修正 */
            --qhat;
            uint64_t c = 0;
            for (int64_t i = 0; i < m; ++i) {
                uint64_t sm = (uint64_t)u[j + i] + v[i] + c;
                u[j + i] = (uint32_t)sm;
                c = sm >> 32;
            }
            u[j + m] = (uint32_t)((uint64_t)u[j + m] + c);
        }
        q[j] = (uint32_t)qhat;
    }
    /* 余数 = u[0..m) >> s */
    if (s) {
        for (int64_t i = m; i-- > 0;) {
            /* u[i+1] 的低 s 位拼到 u[i] 的高 s 位 */
            uint64_t cur = (u[i] >> s) |
                           ((uint64_t)(u[i + 1] & ((1u << s) - 1)) << (32 - s));
            r[i] = (uint32_t)cur;
        }
    } else {
        memcpy(r, u, (size_t)m * 4);
    }
}

/* ── i64 / u64 / f64 ↔ big ─────────────────────────────────────── */

PyAdamasBig* pa_big_from_i64(int64_t v) {
    if (v == 0) return big_alloc(0);
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int64_t n = mag > 0xFFFFFFFFull ? 2 : 1;
    PyAdamasBig* r = big_alloc(n);
    r->d[0] = (uint32_t)mag;
    if (n == 2) r->d[1] = (uint32_t)(mag >> 32);
    r->nd = v < 0 ? -n : n;
    return r;
}

PyAdamasBig* pa_big_from_u64(uint64_t mag) {
    if (mag == 0) return big_alloc(0);
    int64_t n = mag > 0xFFFFFFFFull ? 2 : 1;
    PyAdamasBig* r = big_alloc(n);
    r->d[0] = (uint32_t)mag;
    if (n == 2) r->d[1] = (uint32_t)(mag >> 32);
    r->nd = n;
    return r;
}

int64_t pa_big_to_i64(PyAdamasBig* a) {
    /* cast 语义：取低 64 位（回绕），与 cast_i64 的 C 式截断同口径。 */
    if (a->nd == 0) return 0;
    int64_t n = big_n(a);
    uint64_t lo = n > 0 ? a->d[0] : 0;
    if (n > 1) lo |= (uint64_t)a->d[1] << 32;
    /* 负数按补码低 64 位（-1 → 0xFFFF…FFFF）。 */
    return a->nd < 0 ? (int64_t)(0u - lo) : (int64_t)lo;
}

/* big 的 bit 长度（|a| 的最高有效位 + 1；零为 0）。 */
static int64_t big_bitlen(const PyAdamasBig* a) {
    int64_t n = big_n(a);
    if (n == 0) return 0;
    uint32_t top = a->d[n - 1];
    int64_t bits = (n - 1) * 32;
    while (top) { ++bits; top >>= 1; }
    return bits;
}

double pa_big_to_f64(PyAdamasBig* a) {
    int64_t n = big_n(a);
    if (n == 0) return 0.0;
    int sign = big_sign(a);
    double mag;
    if (n <= 2) {
        uint64_t v = a->d[0];
        if (n == 2) v |= (uint64_t)a->d[1] << 32;
        mag = (double)v;   /* uint64 → double 是正确舍入 */
    } else {
        /* 取最高 64 位 + sticky（低位或进 bit0），再乘 2^shift。
         * sticky 保证「恰好半程」按偶数舍入、非半程不误入（CPython
         * float(big) 同口径的正确舍入）。 */
        int64_t bl = big_bitlen(a);
        int64_t shift = bl - 64;
        uint64_t top = 0;
        int64_t limb_off = shift / 32;
        int bit_off = (int)(shift % 32);
        for (int k = 0; k < 2; ++k) {
            int64_t idx = limb_off + k;
            uint64_t part = idx < n ? ((uint64_t)a->d[idx] >> bit_off) : 0;
            if (bit_off && idx + 1 < n)
                part |= (uint64_t)a->d[idx + 1] << (32 - bit_off);
            top |= (part & 0xFFFFFFFFull) << (32 * k);
        }
        /* sticky：shift 之下的所有位 */
        int sticky = 0;
        for (int64_t i = 0; i < limb_off && !sticky; ++i)
            if (a->d[i]) sticky = 1;
        if (!sticky && bit_off && (a->d[limb_off] & ((1u << bit_off) - 1)))
            sticky = 1;
        top |= (uint64_t)sticky;
        mag = ldexp((double)top, (int)shift);
    }
    if (isinf(mag))
        pa_raise_c("OverflowError", "int too large to convert to float");
    return sign < 0 ? -mag : mag;
}

PyAdamasBig* pa_big_from_f64(double d) {
    /* int(float)：向零截断（CPython 口径）。 */
    if (isnan(d)) pa_raise_c("ValueError", "cannot convert float NaN to integer");
    if (isinf(d)) pa_raise_c("OverflowError", "cannot convert float infinity to integer");
    int sign = d < 0 ? -1 : 1;
    double ad = d < 0 ? -d : d;
    if (ad < 9223372036854775808.0)   /* < 2^63：i64 路径（含 INT64_MIN 幅值） */
        return pa_big_from_i64(sign * (int64_t)ad);
    int e = 0;
    double m = frexp(ad, &e);        /* ad = m * 2^e, 0.5 <= m < 1 */
    uint64_t mant = (uint64_t)(m * 18446744073709551616.0);  /* m * 2^64，精确 */
    /* ad = mant * 2^(e-64)，e >= 64（ad >= 2^63 时 e >= 64）。 */
    int64_t sh = e - 64;
    int64_t limb_sh = sh / 32;
    int bit_sh = (int)(sh % 32);
    int64_t n = limb_sh + 2 + 1;
    PyAdamasBig* r = big_alloc(n);
    if (bit_sh) {
        uint64_t lo = mant << bit_sh;
        r->d[limb_sh] = (uint32_t)lo;
        r->d[limb_sh + 1] = (uint32_t)(lo >> 32);
        r->d[limb_sh + 2] = (uint32_t)(mant >> (64 - bit_sh));
    } else {
        r->d[limb_sh] = (uint32_t)mant;
        r->d[limb_sh + 1] = (uint32_t)(mant >> 32);
    }
    big_set_sign(r, n, sign);
    return r;
}

/* ── 字符串 → big（cast_big(str) / int(str, base) 的 big 版）────── */

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* acc = acc * mul + add（裸 limb 缓冲，容量 cap，*n 是当前 limb 数）。 */
static void mag_muladd_small(uint32_t* acc, int64_t* n, int64_t cap,
                             uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    for (int64_t i = 0; i < *n; ++i) {
        uint64_t cur = (uint64_t)acc[i] * mul + carry;
        acc[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    while (carry) {
        if (*n >= cap) panic_c("big: parse overflow");
        acc[(*n)++] = (uint32_t)carry;
        carry >>= 32;
    }
}

static bool big_parse_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static PyAdamasBig* big_from_text(const char* s, size_t len, int64_t base) {
    if (base != 0 && (base < 2 || base > 36))
        pa_raise_c("ValueError", "int() base must be >= 2 and <= 36, or 0");
    const char* end = s + len;
    while (s != end && big_parse_space(*s)) ++s;
    while (s != end && big_parse_space(end[-1])) --end;
    int sign = 1;
    if (s != end && (*s == '+' || *s == '-')) { if (*s == '-') sign = -1; ++s; }
    bool leading_zero = base == 0 && s != end && *s == '0';
    if (end - s >= 2 && s[0] == '0') {
        int prefix = (s[1] == 'x' || s[1] == 'X') ? 16 :
                     (s[1] == 'o' || s[1] == 'O') ? 8 :
                     (s[1] == 'b' || s[1] == 'B') ? 2 : 0;
        if (prefix && (base == 0 || base == prefix)) {
            base = prefix;
            leading_zero = false;
            s += 2;
            if (s != end && *s == '_') ++s;
        }
    }
    if (base == 0) base = 10;
    // Validate before allocating: pa_raise_c can bypass C scope cleanup.
    bool digit = false;
    for (const char* p = s; p != end; ++p) {
        int dv = digit_val(*p);
        if (*p == '_' && digit) digit = false;
        else if (dv >= 0 && dv < base && (!leading_zero || dv == 0)) digit = true;
        else pa_raise_c("ValueError", "invalid literal for big()");
    }
    if (!digit) pa_raise_c("ValueError", "invalid literal for big()");
    int64_t cap = (int64_t)(end - s) * 6 / 32 + 2;
    uint32_t* acc = limbs_alloc(cap);
    PA_FREE_SCOPE(acc);
    int64_t n = 0;
    for (const char* p = s; p != end; ++p)
        if (*p != '_') mag_muladd_small(acc, &n, cap, (uint32_t)base, (uint32_t)digit_val(*p));
    PyAdamasBig* r = big_alloc(n);
    memcpy(r->d, acc, (size_t)n * 4);
    big_set_sign(r, n, sign);
    return r;
}

PyAdamasBig* pa_big_from_cstr(const char* s, int64_t base) {
    return big_from_text(s, strlen(s), base);
}

PyAdamasBig* pa_big_from_str(PyAdamasStr* s, int64_t base) {
    return big_from_text(s->data, (size_t)s->len, base);
}

/* ── 加减乘 ─────────────────────────────────────────────────────── */

PyAdamasBig* pa_big_add(PyAdamasBig* a, PyAdamasBig* b) {
    int sa = big_sign(a), sb = big_sign(b);
    int64_t na = big_n(a), nb = big_n(b);
    if (sa == 0) { pa_rc_inc(b); return b; }
    if (sb == 0) { pa_rc_inc(a); return a; }
    if (sa == sb) {
        const uint32_t* x = na >= nb ? a->d : b->d;
        const uint32_t* y = na >= nb ? b->d : a->d;
        int64_t nx = na >= nb ? na : nb, ny = na >= nb ? nb : na;
        PyAdamasBig* r = big_alloc(nx + 1);
        uint32_t carry = mag_add(r->d, x, nx, y, ny);
        r->d[nx] = carry;
        big_set_sign(r, nx + 1, sa);
        return r;
    }
    /* 异号：大幅值减小幅值，符号跟大幅值。 */
    int c = mag_cmp(a->d, na, b->d, nb);
    if (c == 0) return big_alloc(0);
    const uint32_t* x = c > 0 ? a->d : b->d;
    const uint32_t* y = c > 0 ? b->d : a->d;
    int64_t nx = c > 0 ? na : nb, ny = c > 0 ? nb : na;
    int sign = c > 0 ? sa : sb;
    PyAdamasBig* r = big_alloc(nx);
    mag_sub(r->d, x, nx, y, ny);
    big_set_sign(r, nx, sign);
    return r;
}

PyAdamasBig* pa_big_sub(PyAdamasBig* a, PyAdamasBig* b) {
    PyAdamasBig* nb = pa_big_neg(b);
    pa_gc_pin(nb);
    PyAdamasBig* r = pa_big_add(a, nb);
    pa_gc_unpin(nb);
    pa_rc_dec(nb);
    return r;
}

PyAdamasBig* pa_big_mul(PyAdamasBig* a, PyAdamasBig* b) {
    int64_t na = big_n(a), nb = big_n(b);
    if (na == 0 || nb == 0) return big_alloc(0);
    PyAdamasBig* r = big_alloc(na + nb);
    mag_mul(r->d, a->d, na, b->d, nb);
    big_set_sign(r, na + nb, big_sign(a) * big_sign(b));
    return r;
}

/* ── 除 / 模（CPython：// 向下取整，% 与除数同号）───────────────── */

static void big_divmod_raw(PyAdamasBig* a, PyAdamasBig* b,
                           PyAdamasBig** qout, PyAdamasBig** rout) {
    if (big_n(b) == 0)
        pa_raise_c("ZeroDivisionError", "integer division or modulo by zero");
    int64_t na = big_n(a), nb = big_n(b);
    uint32_t* q = limbs_alloc(na > 0 ? na : 1);
    uint32_t* r = limbs_alloc(nb);
    PA_FREE_SCOPE(q);
    PA_FREE_SCOPE(r);
    mag_divmod(a->d, na, b->d, nb, q, r);
    PyAdamasBig* qq = big_alloc(na);
    memcpy(qq->d, q, (size_t)na * 4);
    big_set_sign(qq, na, 1);
    pa_gc_pin(qq);
    PyAdamasBig* rr = big_alloc(nb);
    pa_gc_unpin(qq);
    memcpy(rr->d, r, (size_t)nb * 4);
    big_set_sign(rr, nb, 1);
    *qout = qq;
    *rout = rr;
}

PyAdamasBig* pa_big_floordiv(PyAdamasBig* a, PyAdamasBig* b) {
    PyAdamasBig *q, *r;
    big_divmod_raw(a, b, &q, &r);
    int sa = big_sign(a), sb = big_sign(b);
    int sign = sa * sb;
    bool adjust = big_n(r) != 0 && sign < 0;
    pa_rc_dec(r);  /* r is not rooted during the allocations for adjustment. */
    if (adjust) {
        /* 向下取整：异号且有余数 → q = -(q+1) */
        pa_gc_pin(q);
        PyAdamasBig* one = pa_big_from_i64(1);
        pa_gc_pin(one);
        PyAdamasBig* q1 = pa_big_add(q, one);
        pa_gc_unpin(one);
        pa_rc_dec(one);
        pa_gc_unpin(q);
        pa_rc_dec(q);
        q = q1;
        sign = -1;
    }
    q->nd = big_n(q) == 0 ? 0 : (sign < 0 ? -big_n(q) : big_n(q));
    return q;
}

PyAdamasBig* pa_big_mod(PyAdamasBig* a, PyAdamasBig* b) {
    PyAdamasBig *q, *r;
    big_divmod_raw(a, b, &q, &r);
    pa_rc_dec(q);
    int sa = big_sign(a), sb = big_sign(b);
    if (big_n(r) != 0 && sa != sb) {
        /* 余数与除数同号：r_signed = sign(a)*|r|，异号时补一个 b。 */
        r->nd = sa < 0 ? -big_n(r) : big_n(r);
        pa_gc_pin(r);
        PyAdamasBig* r2 = pa_big_add(r, b);
        pa_gc_unpin(r);
        pa_rc_dec(r);
        return r2;
    }
    r->nd = big_n(r) == 0 ? 0 : (sb < 0 ? -big_n(r) : big_n(r));
    return r;
}

/* ── 幂（平方乘；负指数是文档化边界）────────────────────────────── */

PyAdamasBig* pa_big_pow(PyAdamasBig* a, PyAdamasBig* b) {
    int sb = big_sign(b);
    if (sb < 0) {
        if (big_n(a) == 0)
            pa_raise_c("ZeroDivisionError", "0.0 cannot be raised to a negative power");
        /* CPython 返回 float；pa 的静态结果类型是 big，无法携带 float。
         * 文档化边界：抛 ValueError（docs/big-f32-landing.md）。 */
        pa_raise_c("ValueError", "big ** negative exponent is not supported (CPython returns float)");
    }
    if (big_bitlen(b) > 31)
        pa_raise_c("ValueError", "big exponent too large");
    int64_t e = pa_big_to_i64(b);
    PyAdamasBig* acc = pa_big_from_i64(1);
    pa_gc_pin(acc);
    PyAdamasBig* base = a;
    pa_rc_inc(base);
    pa_gc_pin(base);
    while (e > 0) {
        if (e & 1) {
            PyAdamasBig* t = pa_big_mul(acc, base);
            pa_gc_unpin(acc);
            pa_rc_dec(acc);
            acc = t;
            pa_gc_pin(acc);
        }
        e >>= 1;
        if (e > 0) {
            PyAdamasBig* t = pa_big_mul(base, base);
            pa_gc_unpin(base);
            pa_rc_dec(base);
            base = t;
            pa_gc_pin(base);
        }
    }
    pa_gc_unpin(base);
    pa_rc_dec(base);
    pa_gc_unpin(acc);
    return acc;
}


/* ── round(big, ndigits) 与三参 pow（0.495）───────────────────────
 *
 * round：ndigits >= 0 直接返回自身（big 已是整数）；ndigits < 0 按 10^k
 *   （k = -ndigits）做**半程到偶数**取整，与 CPython 的 10 进制 digits 口径
 *   一致：round(15, -1) = 20、round(25, -1) = 20、round(-25, -1) = -20。
 *
 * pow(b, e, m)：平方乘 + 逐步取模。m == 0 抛 ValueError（CPython 同）；
 *   负指数在 CPython 3.8+ 走模逆，这里显式拒绝（要扩就得先写 big 的扩展欧几里得）。
 *   m 为负时结果与 CPython 同：r = b^e mod |m|，再取 r - |m|（负数一侧）。 */

static PyAdamasBig* big_pow10(int64_t k) {
    PyAdamasBig* acc = pa_big_from_i64(1);
    pa_gc_pin(acc);
    PyAdamasBig* base = pa_big_from_i64(10);
    pa_gc_pin(base);
    int64_t e = k;
    while (e > 0) {
        if (e & 1) {
            PyAdamasBig* t = pa_big_mul(acc, base);
            pa_gc_unpin(acc);
            pa_rc_dec(acc);
            acc = t;
            pa_gc_pin(acc);
        }
        e >>= 1;
        if (e > 0) {
            PyAdamasBig* t = pa_big_mul(base, base);
            pa_gc_unpin(base);
            pa_rc_dec(base);
            base = t;
            pa_gc_pin(base);
        }
    }
    pa_gc_unpin(base);
    pa_rc_dec(base);
    pa_gc_unpin(acc);
    return acc;
}

static PyAdamasBig* big_copy(PyAdamasBig* a) {
    int64_t n = big_n(a);
    PyAdamasBig* r = big_alloc(n);
    if (n > 0) memcpy(r->d, a->d, (size_t)n * 4);
    r->nd = a->nd;
    return r;
}

PyAdamasBig* pa_big_round(PyAdamasBig* a, int64_t nd) {
    if (!a) return pa_big_from_i64(0);
    if (nd >= 0) return big_copy(a);
    if (nd == INT64_MIN) pa_raise_c("ValueError", "round() ndigits too large");
    int64_t k = -nd;
    /* 10^k 的 limb 数约 0.415*k；1e6 位已经 415KB，够用且防病态。 */
    if (k > 1000000) pa_raise_c("ValueError", "round() ndigits too large");
    PyAdamasBig* d = big_pow10(k);
    pa_gc_pin(d);
    PyAdamasBig* q = pa_big_floordiv(a, d);  /* 向下取整 */
    pa_gc_pin(q);
    PyAdamasBig* r = pa_big_mod(a, d);       /* 0 <= r < d */
    pa_gc_pin(r);
    PyAdamasBig* twice = pa_big_add(r, r);
    pa_gc_pin(twice);
    const int c = (int)pa_big_cmp(twice, d);
    int bump = (c > 0);
    if (c == 0) bump = (big_n(q) != 0) && ((q->d[0] & 1u) != 0);  /* 半程到偶数 */
    pa_gc_unpin(twice);
    pa_rc_dec(twice);
    if (bump) {
        PyAdamasBig* one = pa_big_from_i64(1);
        pa_gc_pin(one);
        PyAdamasBig* q1 = pa_big_add(q, one);
        pa_gc_unpin(one);
        pa_rc_dec(one);
        pa_gc_unpin(q);
        pa_rc_dec(q);
        q = q1;
        pa_gc_pin(q);
    }
    PyAdamasBig* out = pa_big_mul(q, d);
    pa_gc_unpin(q);
    pa_rc_dec(q);
    pa_gc_unpin(r);
    pa_rc_dec(r);
    pa_gc_unpin(d);
    pa_rc_dec(d);
    return out;
}

PyAdamasBig* pa_big_pow_mod(PyAdamasBig* base, PyAdamasBig* exp, PyAdamasBig* mod) {
    if (big_n(mod) == 0) pa_raise_c("ValueError", "pow() 3rd argument cannot be 0");
    if (big_sign(exp) < 0)
        pa_raise_c("ValueError",
                   "pow() with negative exponent and modulus is not supported "
                   "(CPython 3.8+ computes the modular inverse)");
    PyAdamasBig* m = big_copy(mod);
    m->nd = big_n(mod);  /* |mod| */
    pa_gc_pin(m);
    PyAdamasBig* b = pa_big_mod(base, m);   /* [0, m) */
    pa_gc_pin(b);
    PyAdamasBig* one = pa_big_from_i64(1);
    pa_gc_pin(one);
    PyAdamasBig* acc = pa_big_mod(one, m);  /* m == 1 时是 0（CPython 同） */
    pa_gc_pin(acc);
    int64_t bits = 0;
    if (big_n(exp) > 0) {
        bits = big_bitlen(exp);
    }
    for (int64_t i = bits - 1; i >= 0; --i) {
        PyAdamasBig* sq = pa_big_mul(acc, acc);
        pa_gc_pin(sq);
        PyAdamasBig* red = pa_big_mod(sq, m);
        pa_gc_unpin(sq);
        pa_rc_dec(sq);
        pa_gc_unpin(acc);
        pa_rc_dec(acc);
        acc = red;
        pa_gc_pin(acc);
        const uint32_t bit = (exp->d[i >> 5] >> (i & 31)) & 1u;
        if (bit) {
            PyAdamasBig* mul = pa_big_mul(acc, b);
            pa_gc_pin(mul);
            PyAdamasBig* red2 = pa_big_mod(mul, m);
            pa_gc_unpin(mul);
            pa_rc_dec(mul);
            pa_gc_unpin(acc);
            pa_rc_dec(acc);
            acc = red2;
            pa_gc_pin(acc);
        }
    }
    /* 结果符号跟 m：m < 0 且结果非零 → acc - |m|。 */
    if (big_sign(mod) < 0 && big_n(acc) != 0) {
        PyAdamasBig* neg = pa_big_sub(acc, m);
        pa_gc_unpin(acc);
        pa_rc_dec(acc);
        acc = neg;
        pa_gc_pin(acc);
    }
    pa_gc_unpin(b);
    pa_rc_dec(b);
    pa_gc_unpin(one);
    pa_rc_dec(one);
    pa_gc_unpin(m);
    pa_rc_dec(m);
    pa_gc_unpin(acc);
    return acc;
}
/* ── 比较 / 真值 / 一元 ─────────────────────────────────────────── */

int64_t pa_big_cmp(PyAdamasBig* a, PyAdamasBig* b) {
    int sa = big_sign(a), sb = big_sign(b);
    if (sa != sb) return sa < sb ? -1 : 1;
    int c = mag_cmp(a->d, big_n(a), b->d, big_n(b));
    return sa < 0 ? -c : c;
}

int64_t pa_big_eq(PyAdamasBig* a, PyAdamasBig* b) {
    return pa_big_cmp(a, b) == 0;
}

int64_t pa_big_truth(PyAdamasBig* a) { return a->nd != 0; }

PyAdamasBig* pa_big_neg(PyAdamasBig* a) {
    int64_t n = big_n(a);
    PyAdamasBig* r = big_alloc(n);
    memcpy(r->d, a->d, (size_t)n * 4);
    r->nd = -a->nd;
    return r;
}

PyAdamasBig* pa_big_abs(PyAdamasBig* a) {
    if (a->nd >= 0) { pa_rc_inc(a); return a; }
    return pa_big_neg(a);
}

/* ~a = -a - 1（CPython 无限补码口径）。 */
PyAdamasBig* pa_big_not(PyAdamasBig* a) {
    PyAdamasBig* n = pa_big_neg(a);
    pa_gc_pin(n);
    PyAdamasBig* one = pa_big_from_i64(1);
    pa_gc_pin(one);
    PyAdamasBig* r = pa_big_sub(n, one);
    pa_gc_unpin(one);
    pa_rc_dec(one);
    pa_gc_unpin(n);
    pa_rc_dec(n);
    return r;
}

/* big 与 double 的**精确**比较（CPython int/float 混比不走 double 近似）。
 * 返回 -1/0/1；NaN 返回 -2（调用方：除 != 外全 false）。 */
int64_t pa_big_cmp_f64(PyAdamasBig* a, double d) {
    if (isnan(d)) return -2;
    if (isinf(d)) return d > 0.0 ? -1 : 1;
    int sa = big_sign(a);
    int sd = d > 0 ? 1 : (d < 0 ? -1 : 0);
    if (sa == 0 && sd == 0) return 0;
    if (sa != sd) {
        /* 零与非零：符号即结论（d==0 时 sa!=0 → 按符号）。 */
        if (sa == 0) return sd > 0 ? -1 : 1;
        if (sd == 0) return sa > 0 ? 1 : -1;
        return sa < sd ? -1 : 1;
    }
    /* 同号：比幅值。|d| = mant * 2^(e-64)（mant 是 64 位精确尾数）。 */
    int e = 0;
    double m = frexp(d < 0 ? -d : d, &e);
    uint64_t mant = (uint64_t)(m * 18446744073709551616.0);
    int64_t na = big_n(a);
    int c;
    if (e >= 64) {
        /* rhs = mant << (e-64) */
        int64_t sh = e - 64;
        int64_t limb_sh = sh / 32;
        int bit_sh = (int)(sh % 32);
        int64_t rn = limb_sh + 3;
        uint32_t* r = limbs_alloc(rn);
        PA_FREE_SCOPE(r);
        uint64_t lo = mant << bit_sh;
        r[limb_sh] = (uint32_t)lo;
        r[limb_sh + 1] = (uint32_t)(lo >> 32);
        r[limb_sh + 2] = bit_sh ? (uint32_t)(mant >> (64 - bit_sh)) : 0;
        while (rn > 0 && r[rn - 1] == 0) --rn;
        c = mag_cmp(a->d, na, r, rn);
    } else {
        /* lhs = |a| << (64-e)，与 mant 比 */
        int64_t sh = 64 - e;
        int64_t limb_sh = sh / 32;
        int bit_sh = (int)(sh % 32);
        int64_t ln = na + limb_sh + 2;
        uint32_t* l = limbs_alloc(ln);
        PA_FREE_SCOPE(l);
        uint64_t carry = 0;
        for (int64_t i = 0; i < na; ++i) {
            uint64_t cur = ((uint64_t)a->d[i] << bit_sh) | carry;
            l[i + limb_sh] = (uint32_t)cur;
            carry = bit_sh ? cur >> 32 : 0;
        }
        if (carry) l[na + limb_sh] = (uint32_t)carry;
        while (ln > 0 && l[ln - 1] == 0) --ln;
        uint32_t mb[2] = {(uint32_t)mant, (uint32_t)(mant >> 32)};
        int64_t mn = mb[1] ? 2 : (mb[0] ? 1 : 0);
        c = mag_cmp(l, ln, mb, mn);
    }
    return sa < 0 ? -c : c;
}

/* ── 真除法（CPython int/int → 正确舍入的 double）─────────────────
 * 把 |a| 移位到商恰好 64/65 bit，整除后 sticky 或进 bit0，再 ldexp。
 * 与 to_f64 的 sticky 论证同款：半程按偶数舍入、非半程不误入。 */

/* 裸 limb 左移 k bit（返回新缓冲，*n 更新）。 */
static uint32_t* mag_lshift_buf(const uint32_t* a, int64_t n, int64_t k, int64_t* on) {
    int64_t limb_sh = k / 32;
    int bit_sh = (int)(k % 32);
    int64_t rn = n + limb_sh + 1;
    uint32_t* r = limbs_alloc(rn);
    uint64_t carry = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint64_t cur = ((uint64_t)a[i] << bit_sh) | carry;
        r[i + limb_sh] = (uint32_t)cur;
        carry = bit_sh ? cur >> 32 : 0;
    }
    if (carry) r[n + limb_sh] = (uint32_t)carry;
    while (rn > 0 && r[rn - 1] == 0) --rn;
    *on = rn;
    return r;
}

double pa_big_truediv(PyAdamasBig* a, PyAdamasBig* b) {
    if (big_n(b) == 0)
        pa_raise_c("ZeroDivisionError", "division by zero");
    int64_t na = big_n(a);
    int sign = big_sign(a) * big_sign(b);
    if (na == 0) return big_sign(b) < 0 ? -0.0 : 0.0;   /* CPython：0/-x 是 -0.0 */
    int64_t abl = big_bitlen(a), bbl = big_bitlen(b);
    if (abl - bbl < -1075) return sign < 0 ? -0.0 : 0.0;
    if (abl - bbl > 1024)
        pa_raise_c("OverflowError", "integer division result too large for a float");
    /* 亚正规结果（|商| < 2^-1021）：ldexp 会二次舍入，sticky 技巧失效。
     * 直接算 round(|a/b| * 2^1074)（最近偶数），再精确缩放到亚正规网格。 */
    if (abl - bbl < -1021) {
        int64_t asn2 = 0;
        uint32_t* as2 = mag_lshift_buf(a->d, na, 1074, &asn2);
        PA_FREE_SCOPE(as2);
        int64_t nb0 = big_n(b);
        uint32_t* q2 = limbs_alloc(asn2 > 0 ? asn2 : 1);
        uint32_t* r2 = limbs_alloc(nb0);
        PA_FREE_SCOPE(q2);
        PA_FREE_SCOPE(r2);
        mag_divmod(as2, asn2, b->d, nb0, q2, r2);
        int64_t qn2 = asn2;
        while (qn2 > 0 && q2[qn2 - 1] == 0) --qn2;
        int64_t rn2 = nb0;
        while (rn2 > 0 && r2[rn2 - 1] == 0) --rn2;
        int cmp = -1;   /* 2r 与 b 比：r=0 → 2r<b */
        if (rn2 > 0) {
            int64_t rxn = 0;
            uint32_t* rx = mag_lshift_buf(r2, rn2, 1, &rxn);
            cmp = mag_cmp(rx, rxn, b->d, nb0);
            free(rx);
        }
        uint64_t qv = qn2 > 0 ? q2[0] : 0;
        if (qn2 > 1) qv |= (uint64_t)q2[1] << 32;
        if (cmp > 0 || (cmp == 0 && (qv & 1))) ++qv;
        double mag = (double)qv * 0x1p-1074;
        return sign < 0 ? -mag : mag;
    }
    int64_t shift = 64 - (abl - bbl);
    int sticky = 0;
    uint32_t* as;
    int64_t asn;
    if (shift >= 0) {
        as = mag_lshift_buf(a->d, na, shift, &asn);
    } else {
        /* 右移 -shift：sticky 收移出位；长度重数。 */
        int64_t k = -shift;
        int64_t limb_sh = k / 32;
        int bit_sh = (int)(k % 32);
        for (int64_t i = 0; i < limb_sh && i < na && !sticky; ++i)
            if (a->d[i]) sticky = 1;
        if (!sticky && bit_sh && limb_sh < na && (a->d[limb_sh] & ((1u << bit_sh) - 1)))
            sticky = 1;
        asn = limb_sh >= na ? 0 : na - limb_sh;
        as = limbs_alloc(asn > 0 ? asn : 1);
        for (int64_t i = 0; i < asn; ++i) {
            uint64_t cur = a->d[i + limb_sh] >> bit_sh;
            if (bit_sh && i + limb_sh + 1 < na)
                cur |= (uint64_t)a->d[i + limb_sh + 1] << (32 - bit_sh);
            as[i] = (uint32_t)cur;
        }
        while (asn > 0 && as[asn - 1] == 0) --asn;
    }
    PA_FREE_SCOPE(as);
    int64_t nb = big_n(b);
    uint32_t* q = limbs_alloc(asn > 0 ? asn : 1);
    uint32_t* r = limbs_alloc(nb);
    PA_FREE_SCOPE(q);
    PA_FREE_SCOPE(r);
    mag_divmod(as, asn, b->d, nb, q, r);
    int64_t qn = asn;
    while (qn > 0 && q[qn - 1] == 0) --qn;
    for (int64_t i = 0; i < nb && !sticky; ++i)
        if (r[i]) sticky = 1;
    int64_t e = -shift;   /* a/b ≈ q * 2^e */
    if (qn > 2) {
        /* 商 65 bit：右移 1 位归一到 64 bit，sticky 收最低位。 */
        sticky |= (int)(q[0] & 1);
        uint64_t carry = 0;
        for (int64_t i = qn; i-- > 0;) {
            uint64_t cur = (carry << 32) | q[i];
            q[i] = (uint32_t)(cur >> 1);
            carry = cur & 1;
        }
        while (qn > 0 && q[qn - 1] == 0) --qn;
        e += 1;
    }
    uint64_t top = qn > 0 ? q[0] : 0;
    if (qn > 1) top |= (uint64_t)q[1] << 32;
    top |= (uint64_t)sticky;
    double mag = ldexp((double)top, (int)e);
    if (isinf(mag)) {
        free(as);
        free(q);
        free(r);
        pa_raise_c("OverflowError", "integer division result too large for a float");
    }
    return sign < 0 ? -mag : mag;
}

/* ── 位运算（无限补码语义，CPython 口径）────────────────────────── */

/* 取 a 的无限补码第 i 个 limb。负数 = 幅值按位取反 + 1（进位从低位传）。
 * carry 是 +1 的进位状态，调用方从 0 开始逐 limb 推进。 */
static uint32_t tc_limb(PyAdamasBig* a, int64_t i, uint64_t* carry) {
    int64_t n = big_n(a);
    if (a->nd >= 0) return i < n ? a->d[i] : 0;
    uint64_t mag = i < n ? a->d[i] : 0;
    uint64_t v = (~mag & 0xFFFFFFFFull) + *carry;
    *carry = v >> 32;
    return (uint32_t)v;
}

static PyAdamasBig* big_bitop(PyAdamasBig* a, PyAdamasBig* b, int op) {
    /* op: 0 and, 1 or, 2 xor。结果符号 = 符号位（负=...111）按位运算。 */
    int sa = big_sign(a) < 0, sb = big_sign(b) < 0;
    int sr = op == 0 ? (sa & sb) : op == 1 ? (sa | sb) : (sa ^ sb);
    int64_t na = big_n(a), nb = big_n(b);
    int64_t n = (na > nb ? na : nb) + 1;
    uint32_t* r = limbs_alloc(n);
    PA_FREE_SCOPE(r);
    uint64_t ca = 1, cb = 1;   /* 负数补码 +1 的初始进位 */
    for (int64_t i = 0; i < n; ++i) {
        uint32_t xa = tc_limb(a, i, &ca);
        uint32_t xb = tc_limb(b, i, &cb);
        r[i] = op == 0 ? (xa & xb) : op == 1 ? (xa | xb) : (xa ^ xb);
    }
    if (sr) {
        /* 补码结果 → 幅值：取反 + 1 */
        uint64_t carry = 1;
        for (int64_t i = 0; i < n; ++i) {
            uint64_t v = (~r[i] & 0xFFFFFFFFull) + carry;
            r[i] = (uint32_t)v;
            carry = v >> 32;
        }
    }
    PyAdamasBig* out = big_alloc(n);
    memcpy(out->d, r, (size_t)n * 4);
    big_set_sign(out, n, sr ? -1 : 1);
    return out;
}

PyAdamasBig* pa_big_and(PyAdamasBig* a, PyAdamasBig* b) { return big_bitop(a, b, 0); }
PyAdamasBig* pa_big_or(PyAdamasBig* a, PyAdamasBig* b) { return big_bitop(a, b, 1); }
PyAdamasBig* pa_big_xor(PyAdamasBig* a, PyAdamasBig* b) { return big_bitop(a, b, 2); }

/* ── 移位（计数是 big；负计数 ValueError，CPython 口径）─────────── */

static int64_t shift_count(PyAdamasBig* cnt) {
    if (big_sign(cnt) < 0)
        pa_raise_c("ValueError", "negative shift count");
    if (big_bitlen(cnt) > 31)
        pa_raise_c("ValueError", "shift count too large");
    return pa_big_to_i64(cnt);
}

PyAdamasBig* pa_big_lshift(PyAdamasBig* a, PyAdamasBig* cnt) {
    int64_t k = shift_count(cnt);
    int64_t na = big_n(a);
    if (na == 0 || k == 0) { pa_rc_inc(a); return a; }
    int64_t limb_sh = k / 32;
    int bit_sh = (int)(k % 32);
    int64_t n = na + limb_sh + 1;
    PyAdamasBig* r = big_alloc(n);
    uint64_t carry = 0;
    for (int64_t i = 0; i < na; ++i) {
        uint64_t cur = ((uint64_t)a->d[i] << bit_sh) | carry;
        r->d[i + limb_sh] = (uint32_t)cur;
        carry = bit_sh ? cur >> 32 : 0;
    }
    if (carry) r->d[na + limb_sh] = (uint32_t)carry;
    big_set_sign(r, n, big_sign(a));
    return r;
}

PyAdamasBig* pa_big_rshift(PyAdamasBig* a, PyAdamasBig* cnt) {
    int64_t k = shift_count(cnt);
    int64_t na = big_n(a);
    if (na == 0 || k == 0) { pa_rc_inc(a); return a; }
    int64_t limb_sh = k / 32;
    int bit_sh = (int)(k % 32);
    if (limb_sh >= na) {
        /* 全部移出：负数按算术右移是 -1（floor），非负是 0。 */
        return pa_big_from_i64(big_sign(a) < 0 ? -1 : 0);
    }
    int64_t n = na - limb_sh;
    PyAdamasBig* r = big_alloc(n);
    for (int64_t i = 0; i < n; ++i) {
        uint64_t cur = a->d[i + limb_sh] >> bit_sh;
        if (bit_sh && i + limb_sh + 1 < na)
            cur |= (uint64_t)a->d[i + limb_sh + 1] << (32 - bit_sh);
        r->d[i] = (uint32_t)cur;
    }
    big_set_sign(r, n, 1);   /* 先按正幅值持有 */
    if (big_sign(a) < 0) {
        /* 算术右移 = floor：移出的位非零 → 幅值 +1（向 -inf）。 */
        int sticky = 0;
        for (int64_t i = 0; i < limb_sh && !sticky; ++i)
            if (a->d[i]) sticky = 1;
        if (!sticky && bit_sh && (a->d[limb_sh] & ((1u << bit_sh) - 1)))
            sticky = 1;
        if (sticky) {
            pa_gc_pin(r);
            PyAdamasBig* one = pa_big_from_i64(1);
            pa_gc_pin(one);
            PyAdamasBig* r2 = pa_big_add(r, one);
            pa_gc_unpin(one);
            pa_rc_dec(one);
            pa_gc_unpin(r);
            pa_rc_dec(r);
            r2->nd = -big_n(r2);
            return r2;
        }
        r->nd = -big_n(r);   /* 零保持零 */
    }
    return r;
}

/* ── str / repr / 进制输出 ─────────────────────────────────────── */

PyAdamasStr* pa_big_str(PyAdamasBig* a) {
    int64_t n = big_n(a);
    if (n == 0) return pa_char_str('0');
    /* 反复除以 10^9，收集 9 位十进制块（O(n^2)，测试规模足够）。 */
    int64_t cap = n * 10 + 2;   /* 十进制位数上界 ~ n*32/log2(10) < n*10 */
    char* buf = (char*)malloc((size_t)cap + 1);
    if (!buf) panic_c("out of memory");
    PA_FREE_SCOPE(buf);
    uint32_t* cur = limbs_alloc(n);
    PA_FREE_SCOPE(cur);
    memcpy(cur, a->d, (size_t)n * 4);
    int64_t cn = n;
    int64_t pos = cap;
    while (cn > 0) {
        uint64_t rem = 0;
        for (int64_t i = cn; i-- > 0;) {
            uint64_t v = (rem << 32) | cur[i];
            cur[i] = (uint32_t)(v / 1000000000u);
            rem = v % 1000000000u;
        }
        while (cn > 0 && cur[cn - 1] == 0) --cn;
        if (cn > 0) {
            for (int k = 0; k < 9; ++k) {
                buf[--pos] = (char)('0' + (int)(rem % 10));
                rem /= 10;
            }
        } else {
            while (rem) {
                buf[--pos] = (char)('0' + (int)(rem % 10));
                rem /= 10;
            }
        }
    }
    if (a->nd < 0) buf[--pos] = '-';
    return str_new_ascii(buf + pos, cap - pos);
}

PyAdamasStr* pa_big_to_base(PyAdamasBig* a, int64_t base) {
    if (base == 10) return pa_big_str(a);
    if (base != 2 && base != 8 && base != 16)
        pa_raise_c("ValueError", "big base conversion supports 2/8/10/16");
    int64_t n = big_n(a);
    if (n == 0) {
        const char* z = base == 16 ? "0x0" : base == 8 ? "0o0" : "0b0";
        return str_new_ascii(z, 3);
    }
    int bits_per = base == 16 ? 4 : base == 8 ? 3 : 1;
    int64_t bl = big_bitlen(a);
    int64_t ndig = (bl + bits_per - 1) / bits_per;
    int64_t cap = ndig + 4;
    char* buf = (char*)malloc((size_t)cap + 1);
    if (!buf) panic_c("out of memory");
    PA_FREE_SCOPE(buf);
    int64_t pos = cap;
    const char* digs = "0123456789abcdef";
    /* 连续位提取：oct 的 3 bit 数字跨 limb 边界（32 % 3 != 0），
     * 不能按 limb 独立切块。第 j 个数字 = (value >> j*bits_per) & mask。 */
    for (int64_t j = 0; j < ndig; ++j) {
        int64_t bitpos = j * bits_per;
        int64_t idx = bitpos / 32;
        int off = (int)(bitpos % 32);
        uint64_t w = idx < n ? ((uint64_t)a->d[idx] >> off) : 0;
        if (off && idx + 1 < n) w |= (uint64_t)a->d[idx + 1] << (32 - off);
        uint32_t v = (uint32_t)(w & ((1u << bits_per) - 1));
        buf[--pos] = digs[v];
    }
    if (base == 16) { buf[--pos] = 'x'; buf[--pos] = '0'; }
    else if (base == 8) { buf[--pos] = 'o'; buf[--pos] = '0'; }
    else { buf[--pos] = 'b'; buf[--pos] = '0'; }
    if (a->nd < 0) buf[--pos] = '-';
    return str_new_ascii(buf + pos, cap - pos);
}
