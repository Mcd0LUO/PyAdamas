// rt_format.c —— f-string 格式化与 repr
#include "rt_internal.h"

/* ── f-string 格式化 ─────────────────────────────────────────── */

typedef struct {
    char fill;
    char align;   /* < > ^ = */
    char sign;    /* + - 空格 或 0 */
    char type;
    int width;
    int prec;
    int grouping;     /* ',' 或 '_' */
    char group_char;
    int alternate;    /* '#' 交替形式（前缀 / 保留小数点） */
} PyAdamasFmt;

static void fmt_parse(PyAdamasStr* spec, PyAdamasFmt* f, int is_str) {
    f->fill = ' ';
    f->align = is_str ? '<' : '>';
    f->sign = 0;
    f->type = 0;
    f->width = 0;
    f->prec = -1;
    f->grouping = 0;
    f->group_char = ',';
    f->alternate = 0;
    const char* p = spec ? spec->data : "";
    int n = spec ? (int)spec->len : 0;
    int i = 0;
    int explicit_align = 0;
    if (i + 1 < n && (p[i + 1] == '<' || p[i + 1] == '>' || p[i + 1] == '^' ||
                      p[i + 1] == '=')) {
        f->fill = p[i];
        f->align = p[i + 1];
        explicit_align = 1;
        i += 2;
    } else if (i < n && (p[i] == '<' || p[i] == '>' || p[i] == '^' || p[i] == '=')) {
        f->align = p[i];
        explicit_align = 1;
        i++;
    }
    if (i < n && (p[i] == '+' || p[i] == '-' || p[i] == ' ')) f->sign = p[i++];
    if (i < n && p[i] == '#') {
        f->alternate = 1;
        i++;
    }
    if (i < n && p[i] == '0') {
        f->fill = '0';
        if (!explicit_align) f->align = '=';
        i++;
    }
    while (i < n && p[i] >= '0' && p[i] <= '9') f->width = f->width * 10 + (p[i++] - '0');
    if (i < n && (p[i] == ',' || p[i] == '_')) {
        f->grouping = 1;
        f->group_char = p[i];
        i++;
    }
    if (i < n && p[i] == '.') {
        i++;
        f->prec = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') f->prec = f->prec * 10 + (p[i++] - '0');
    }
    if (i < n) f->type = p[i++];
    if (i != n) pa_raise_c("ValueError", "Invalid format specifier");
}

/* prefix 是 '#' 交替形式的前缀（"0x" / "0o" / "0b"）：零填充必须插在
   前缀**之后**（Python 的 `format(255, '#08x')` 是 '0x0000ff'），所以
   它不能简单拼进 body。 */
static PyAdamasStr* fmt_pad_parts(const char* sign, const char* prefix,
                                  const char* body, const PyAdamasFmt* f) {
    size_t sl = sign ? 1 : 0;
    size_t pl = prefix ? strlen(prefix) : 0;
    size_t bl = strlen(body);
    size_t total = sl + pl + bl;
    char* out;
    if (f->width <= (int)total) {
        out = (char*)malloc(total + 1);
        if (!out) panic_c("out of memory");
        if (sl) out[0] = *sign;
        if (pl) memcpy(out + sl, prefix, pl);
        memcpy(out + sl + pl, body, bl);
        out[total] = '\0';
        PyAdamasStr* s = str_new(out, (int64_t)total);
        free(out);
        return s;
    }
    int pad = f->width - (int)total;
    out = (char*)malloc((size_t)f->width + 1);
    if (!out) panic_c("out of memory");
    char* q = out;
    if (f->align == '>') {
        for (int i = 0; i < pad; ++i) *q++ = f->fill;
        if (sl) *q++ = *sign;
        if (pl) { memcpy(q, prefix, pl); q += pl; }
        memcpy(q, body, bl);
        q += bl;
    } else if (f->align == '^') {
        int left = pad / 2;
        int right = pad - left;
        for (int i = 0; i < left; ++i) *q++ = f->fill;
        if (sl) *q++ = *sign;
        if (pl) { memcpy(q, prefix, pl); q += pl; }
        memcpy(q, body, bl);
        q += bl;
        for (int i = 0; i < right; ++i) *q++ = f->fill;
    } else if (f->align == '=') {
        if (sl) *q++ = *sign;
        if (pl) { memcpy(q, prefix, pl); q += pl; }
        for (int i = 0; i < pad; ++i) *q++ = f->fill;
        memcpy(q, body, bl);
        q += bl;
    } else {
        if (sl) *q++ = *sign;
        if (pl) { memcpy(q, prefix, pl); q += pl; }
        memcpy(q, body, bl);
        q += bl;
        for (int i = 0; i < pad; ++i) *q++ = f->fill;
    }
    *q = '\0';
    PyAdamasStr* s = str_new(out, (int64_t)(q - out));
    free(out);
    return s;
}

static PyAdamasStr* fmt_pad(const char* sign, const char* body, const PyAdamasFmt* f) {
    return fmt_pad_parts(sign, NULL, body, f);
}

static void fmt_uint(uint64_t v, unsigned base, int upper, char* out) {
    static const char* lower = "0123456789abcdef";
    static const char* upper_digits = "0123456789ABCDEF";
    const char* digits = upper ? upper_digits : lower;
    char tmp[70];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

/* 按 group_size 位一组插分隔符（十进制 3、十六/二进制 4）。 */
static void fmt_group_n(char* body, size_t cap, char sep, int group_size) {
    char tmp[256];
    int n = (int)strlen(body);
    int j = 0;
    for (int i = 0; i < n; ++i) {
        int left = n - i;
        if (j + 2 >= (int)sizeof tmp) break;
        tmp[j++] = body[i];
        if (left > 1 && group_size > 0 && (left - 1) % group_size == 0)
            tmp[j++] = sep;
    }
    tmp[j] = '\0';
    if ((size_t)j < cap) memcpy(body, tmp, (size_t)j + 1);
}

/* 浮点：只给整数部分分组（Python 的 ',.2f'）。指数形式（1.00e+20）与
   inf/nan 不动 —— CPython 同样不分组。 */
static void fmt_group_float(char* body, size_t cap, char sep) {
    const char* e = strpbrk(body, "eE");
    size_t end = e ? (size_t)(e - body) : strlen(body);
    if (end == 0 || body[0] < '0' || body[0] > '9') return;
    size_t dot = end;
    for (size_t i = 0; i < end; ++i)
        if (body[i] == '.') {
            dot = i;
            break;
        }
    if (dot <= 3) return;
    char tmp[256];
    size_t j = 0;
    for (size_t i = 0; i < dot; ++i) {
        if (i > 0 && (dot - i) % 3 == 0) {
            if (j + 1 >= sizeof tmp) return;
            tmp[j++] = sep;
        }
        if (j + 1 >= sizeof tmp) return;
        tmp[j++] = body[i];
    }
    for (size_t i = dot; i < strlen(body); ++i) {
        if (j + 1 >= sizeof tmp) return;
        tmp[j++] = body[i];
    }
    tmp[j] = '\0';
    if (j < cap) memcpy(body, tmp, j + 1);
}

// 整数格式化的公共尾段：给定量值（无符号）与符号位，按格式说明符
// 排版。pa_fmt_i64 / pa_fmt_u64 只在「怎么得到 sign/mag」上不同。
static PyAdamasStr* fmt_int_mag(uint64_t mag, char sign, PyAdamasFmt* fp) {
    PyAdamasFmt f = *fp;
    unsigned base = 10;
    int upper = 0;
    if (f.type == 'b') base = 2;
    else if (f.type == 'o') base = 8;
    else if (f.type == 'x') base = 16;
    else if (f.type == 'X') {
        base = 16;
        upper = 1;
    }
    char body[96];
    fmt_uint(mag, base, upper, body);
    if (f.grouping) {
        if (base == 10)
            fmt_group_n(body, sizeof body, f.group_char, 3);
        else if (f.group_char == '_')
            fmt_group_n(body, sizeof body, '_', 4);
        else
            pa_raise_c("ValueError",
                       "Cannot specify ',' with 'x', 'X', 'o' or 'b'.");
    }
    /* '#' 交替形式：二进制/八进制/十六进制加前缀（十进制无前缀）。 */
    const char* prefix = NULL;
    if (f.alternate) {
        if (f.type == 'b') prefix = "0b";
        else if (f.type == 'o') prefix = "0o";
        else if (f.type == 'x') prefix = "0x";
        else if (f.type == 'X') prefix = "0X";
    }
    return fmt_pad_parts(sign ? &sign : NULL, prefix, body, &f);
}

PyAdamasStr* pa_fmt_i64(int64_t v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 0);
    if (f.type && f.type != 'd' && f.type != 'b' && f.type != 'o' && f.type != 'x' &&
        f.type != 'X' && f.type != 'n')
        pa_raise_c("ValueError", "Invalid format specifier for int");
    if (f.prec >= 0)
        pa_raise_c("ValueError", "Precision not allowed in integer format specifier");
    char sign = 0;
    uint64_t mag;
    if (v < 0) {
        sign = '-';
        mag = (uint64_t)(-(v + 1)) + 1;
    } else {
        mag = (uint64_t)v;
        if (f.sign == '+') sign = '+';
        else if (f.sign == ' ') sign = ' ';
    }
    return fmt_int_mag(mag, sign, &f);
}

// 无符号 64 位：定宽 u64 的 f-string / format() 走这里（≥ 2^63 的值
// 不能再按有符号打印）。格式校验与 pa_fmt_i64 完全相同。
PyAdamasStr* pa_fmt_u64(uint64_t v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 0);
    if (f.type && f.type != 'd' && f.type != 'b' && f.type != 'o' && f.type != 'x' &&
        f.type != 'X' && f.type != 'n')
        pa_raise_c("ValueError", "Invalid format specifier for int");
    if (f.prec >= 0)
        pa_raise_c("ValueError", "Precision not allowed in integer format specifier");
    char sign = 0;
    if (f.sign == '+') sign = '+';
    else if (f.sign == ' ') sign = ' ';
    return fmt_int_mag(v, sign, &f);
}

/* P3 big 的 format()：与 pa_fmt_i64 同一套说明符文法（d/b/o/x/X/n、
 * 填充/对齐/符号/宽度/#/分组），量值部分是任意长十进制/进制串。
 * 分组用 malloc 版（fmt_group_n 的 256 字节栈缓冲对大数会截断）。 */
static char* big_group_alloc(const char* body, char sep, int group_size) {
    size_t n = strlen(body);
    size_t cap = n + (group_size > 0 ? n / (size_t)group_size : 0) + 2;
    char* out = (char*)malloc(cap);
    if (!out) panic_c("out of memory");
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t left = n - i;
        out[j++] = body[i];
        if (left > 1 && group_size > 0 && (left - 1) % (size_t)group_size == 0)
            out[j++] = sep;
    }
    out[j] = '\0';
    return out;
}

PyAdamasStr* pa_big_fmt(PyAdamasBig* v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 0);
    if (f.type && f.type != 'd' && f.type != 'b' && f.type != 'o' && f.type != 'x' &&
        f.type != 'X' && f.type != 'n')
        pa_raise_c("ValueError", "Invalid format specifier for int");
    if (f.prec >= 0)
        pa_raise_c("ValueError", "Precision not allowed in integer format specifier");
    bool radix = f.type == 'b' || f.type == 'o' || f.type == 'x' || f.type == 'X';
    if (f.grouping && (f.type == 'n' || (radix && f.group_char == ',')))
        pa_raise_c("ValueError", "Invalid grouping for integer format specifier");
    char sign = 0;
    if (v->nd < 0) sign = '-';
    else if (f.sign == '+') sign = '+';
    else if (f.sign == ' ') sign = ' ';
    int base = f.type == 'b' ? 2 : f.type == 'o' ? 8
             : (f.type == 'x' || f.type == 'X') ? 16 : 10;
    PyAdamasBig* mag = pa_big_abs(v);
    pa_gc_pin(mag);
    PyAdamasStr* ds = base == 10 ? pa_big_str(mag) : pa_big_to_base(mag, base);
    pa_gc_unpin(mag);
    pa_rc_dec(mag);
    /* 进制串带 0x/0o/0b 前缀：剥掉（前缀由 # 交替形式单独管）。 */
    const char* d = ds->data;
    int64_t dn = ds->len;
    if (base != 10 && dn >= 2 && d[0] == '0') { d += 2; dn -= 2; }
    char* body0 = (char*)malloc((size_t)dn + 1);
    if (!body0) panic_c("out of memory");
    PA_FREE_SCOPE(body0);
    memcpy(body0, d, (size_t)dn);
    body0[dn] = '\0';
    pa_rc_dec(ds);
    if (f.type == 'X')
        for (char* p = body0; *p; ++p)
            if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 'a' + 'A');
    // Validate before allocation: pa_raise_c uses longjmp, not C cleanup.
    char* grp = f.grouping ? big_group_alloc(body0, f.group_char, base == 10 ? 3 : 4) : NULL;
    PA_FREE_SCOPE(grp);
    const char* body = grp ? grp : body0;
    const char* prefix = NULL;
    if (f.alternate) {
        if (f.type == 'b') prefix = "0b";
        else if (f.type == 'o') prefix = "0o";
        else if (f.type == 'x') prefix = "0x";
        else if (f.type == 'X') prefix = "0X";
    }
    return fmt_pad_parts(sign ? &sign : NULL, prefix, body, &f);
}

PyAdamasStr* pa_fmt_bool(int64_t v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 0);
    if (f.type && f.type != 's' && f.type != 'd' && f.type != 'b' && f.type != 'o' &&
        f.type != 'x' && f.type != 'X' && f.type != 'n')
        pa_raise_c("ValueError", "Invalid format specifier for bool");
    if (f.type == 'd' || f.type == 'b' || f.type == 'o' || f.type == 'x' ||
        f.type == 'X' || f.type == 'n')
        return pa_fmt_i64(v ? 1 : 0, spec);
    PyAdamasFmt sf = f;
    sf.align = f.align == '>' ? '<' : f.align;
    return fmt_pad(NULL, v ? "True" : "False", &sf);
}

PyAdamasStr* pa_fmt_f64(double v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 0);
    char type = f.type;
    if (type && type != 'f' && type != 'F' && type != 'e' && type != 'E' && type != 'g' &&
        type != 'G' && type != '%')
        pa_raise_c("ValueError", "Invalid format specifier for float");
    char sign = 0;
    double mag = v;
    if (mag < 0) {
        sign = '-';
        mag = -mag;
    } else if (f.sign == '+') {
        sign = '+';
    } else if (f.sign == ' ') {
        sign = ' ';
    }
    if (type == '%') mag *= 100.0;
    char body[256];
    int prec = f.prec;
    /* '#' 交替形式交给 printf 的 # 标志：它对 f/e/g 都保证结果里有小数点，
       对 g 还保留尾随零（CPython 的 '#.3g' → '1.00e+20'）。 */
    int iprec = prec < 0 ? 6 : prec;
    if (type == 'f' || type == 'F' || type == '%') {
        char cfmt[16];
        snprintf(cfmt, sizeof cfmt, "%%%s.*%c", f.alternate ? "#" : "",
                 type == 'F' ? 'F' : 'f');
        snprintf(body, sizeof body, cfmt, iprec, mag);
    } else if (type) {
        char cfmt[16];
        snprintf(cfmt, sizeof cfmt, "%%%s.*%c", f.alternate ? "#" : "", type);
        snprintf(body, sizeof body, cfmt, iprec, mag);
    } else if (prec >= 0) {
        char cfmt[16];
        snprintf(cfmt, sizeof cfmt, "%%%s.*g", f.alternate ? "#" : "");
        snprintf(body, sizeof body, cfmt, prec, mag);
    } else {
        format_double(body, sizeof body, mag);
        if (f.alternate && !strchr(body, '.')) {
            /* '#': repr 形态也要保证有小数点（CPython: 1e+20 → 1.e+20）。 */
            const char* e = strpbrk(body, "eE");
            size_t at = e ? (size_t)(e - body) : strlen(body);
            if (at + 1 < sizeof body) {
                memmove(body + at + 1, body + at, strlen(body + at) + 1);
                body[at] = '.';
            }
        }
    }
    if (f.grouping) fmt_group_float(body, sizeof body, f.group_char);
    if (type == '%') {
        size_t n = strlen(body);
        if (n + 2 < sizeof body) {
            body[n] = '%';
            body[n + 1] = '\0';
        }
    }
    return fmt_pad(sign ? &sign : NULL, body, &f);
}

PyAdamasStr* pa_fmt_str(PyAdamasStr* v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 1);
    if (f.type && f.type != 's')
        pa_raise_c("ValueError", "Invalid format specifier for str");
    int64_t n = v ? v->len : 0;
    if (f.prec >= 0 && n > f.prec) n = f.prec;
    char* body = (char*)malloc((size_t)n + 1);
    if (!body) panic_c("out of memory");
    if (n > 0) memcpy(body, v->data, (size_t)n);
    body[n] = '\0';
    PyAdamasStr* out = fmt_pad(NULL, body, &f);
    free(body);
    return out;
}

PyAdamasStr* pa_fmt_complex(PyAdamasComplex* v, PyAdamasStr* spec) {
    PyAdamasFmt f;
    fmt_parse(spec, &f, 1);
    PyAdamasStr* cs = pa_complex_str(v);
    /* fmt_pad 里还要 str_new；cs 只在本帧 C 局部变量里，先 pin 再取字符。 */
    pa_gc_pin(cs);
    char* body = (char*)malloc((size_t)cs->len + 1);
    if (!body) panic_c("out of memory");
    memcpy(body, cs->data, (size_t)cs->len + 1);
    PyAdamasStr* out = fmt_pad(NULL, body, &f);
    free(body);
    pa_gc_unpin(cs);
    pa_rc_dec(cs);
    return out;
}

PyAdamasStr* pa_repr_str(PyAdamasStr* v) {
    int quote = '\'';
    int has_single = 0, has_double = 0;
    for (int64_t i = 0; i < v->len; ++i) {
        if (v->data[i] == '\'') has_single = 1;
        else if (v->data[i] == '"') has_double = 1;
    }
    if (has_single && !has_double) quote = '"';
    char* out = (char*)malloc((size_t)v->len * 2 + 3);
    if (!out) panic_c("out of memory");
    int j = 0;
    out[j++] = (char)quote;
    for (int64_t i = 0; i < v->len; ++i) {
        char c = v->data[i];
        if (c == '\\' || c == (char)quote) {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\t') {
            out[j++] = '\\';
            out[j++] = 't';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else {
            out[j++] = c;
        }
    }
    out[j++] = (char)quote;
    PyAdamasStr* s = str_new(out, j);
    free(out);
    return s;
}

/* ── printf 风格 % 格式化 ──────────────────────────────────────── */

typedef struct {
    char* p;
    int64_t len;
    int64_t cap;
} PBuf;

static void pb_append(PBuf* b, const char* s, int64_t n) {
    if (b->len + n + 1 > b->cap) {
        int64_t ncap = b->cap ? b->cap * 2 : 64;
        while (ncap < b->len + n + 1) ncap *= 2;
        char* np = (char*)realloc(b->p, (size_t)ncap);
        if (!np) panic_c("out of memory");
        b->p = np;
        b->cap = ncap;
    }
    if (n > 0) memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = '\0';
}

// % 风格整数格式化：CPython 的 precision 是「最少位数」（左侧补 0），且
// 给了 precision 时 0 标志**仍然生效**（C printf 会忽略 0 标志，两者不同：
// "%08.5d" % -42 在 CPython 是 "-0000042"，printf 是 "   -00042"）。
// 符号与 precision 的 0 都算在宽度之外（"%.5d" % -42 -> "-00042"）。
// 只给 % 路径用：format()/f-string 的整数说明符给 precision 是 ValueError。
static PyAdamasStr* pct_fmt_int(char conv, uint64_t mag, char sign, int left, int zero,
                                int width, int prec) {
    char digits[80];
    unsigned base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : 10);
    fmt_uint(mag, base, conv == 'X', digits);
    int dlen = (int)strlen(digits);
    int ndig = prec > dlen ? prec : dlen;
    int total = ndig + (sign ? 1 : 0);
    int pad = width > total ? width - total : 0;
    char* buf = (char*)malloc((size_t)(total + pad) + 1);
    if (!buf) panic_c("out of memory");
    int k = 0;
    if (left) {
        if (sign) buf[k++] = sign;
        for (int i = ndig - dlen; i > 0; --i) buf[k++] = '0';
        memcpy(buf + k, digits, (size_t)dlen);
        k += dlen;
        for (int i = 0; i < pad; ++i) buf[k++] = ' ';
    } else if (zero) {
        if (sign) buf[k++] = sign;
        for (int i = 0; i < pad; ++i) buf[k++] = '0';
        for (int i = ndig - dlen; i > 0; --i) buf[k++] = '0';
        memcpy(buf + k, digits, (size_t)dlen);
        k += dlen;
    } else {
        for (int i = 0; i < pad; ++i) buf[k++] = ' ';
        if (sign) buf[k++] = sign;
        for (int i = ndig - dlen; i > 0; --i) buf[k++] = '0';
        memcpy(buf + k, digits, (size_t)dlen);
        k += dlen;
    }
    PyAdamasStr* out = str_new(buf, k);
    free(buf);
    return out;
}

static PyAdamasStr* fmt_with(const char* spec, int n, int64_t raw, double d, PyAdamasStr* s, char kind) {
    PyAdamasStr sp;
    sp.len = n;
    sp.hash = 0;
    sp.data = spec;
    switch (kind) {
        case 'i': return pa_fmt_i64(raw, &sp);
        // 'u'：定宽 u64（P1）。无符号量值，不带负号。
        case 'u': return pa_fmt_u64((uint64_t)raw, &sp);
        case 'f': return pa_fmt_f64(d, &sp);
        case 's': return pa_fmt_str(s, &sp);
        default: return NULL;
    }
}

/* desc 是编译期生成的类型描述：i=int/bool，f=float，s=str/bytes。
   数字格式（d/i/u/x/X/o/f/F/e/E/g/G）要求对应数值类型；s/r 可以
   格式化任意受支持的类型。返回新字符串，bytes 与 str 共用表示。 */
/* big 的整数转换：把已拼好的 spec 加上转换字符交给 pa_big_fmt，
   format() 与 % 共用同一套格式化（宽度/符号/补零/精度都在里面）。 */
static PyAdamasStr* pct_big_int(int64_t raw, const char* spec, int si, char conv) {
    char buf[80];
    int n = si < 76 ? si : 76;
    for (int i = 0; i < n; ++i) buf[i] = spec[i];
    buf[n] = conv;
    PyAdamasStr* sp = str_new(buf, (int64_t)n + 1);
    PyAdamasStr* r = pa_big_fmt((PyAdamasBig*)(uintptr_t)raw, sp);
    pa_rc_dec(sp);
    return r;
}

/* %#x / %#X / %#o：在数字前插入进制前缀，并保持总宽度语义（前缀计入宽度；
   zero 填充的 0 落在前缀之后）。piece 是已经按 width/sign/zero 排好的串。 */
static PyAdamasStr* pct_alt_join(const char* pre, int64_t plen, PyAdamasStr* piece, int width,
                                 int zero, int left) {
    int64_t i = 0;
    char sign = 0;
    if (i < piece->len &&
        (piece->data[i] == '-' || piece->data[i] == '+' || piece->data[i] == ' '))
        sign = piece->data[i++];
    while (i < piece->len && piece->data[i] == ' ') ++i;
    int64_t dstart = i;
    while (zero && dstart < piece->len - 1 && piece->data[dstart] == '0') ++dstart;
    /* 左对齐时 piece 已带尾部空格填充：先去掉，再统一按宽度重排。 */
    int64_t dend = piece->len;
    while (!zero && dend > dstart && piece->data[dend - 1] == ' ') --dend;
    int64_t dlen = dend - dstart;
    if (dlen < 0) dlen = 0;
    int64_t total = (sign ? 1 : 0) + plen + dlen;
    int64_t pad = width > total ? width - total : 0;
    int64_t outlen = total + pad;
    char* buf = (char*)malloc((size_t)outlen);
    if (!buf) panic_c("out of memory");
    int64_t o = 0;
    if (zero) {
        if (sign) buf[o++] = sign;
        for (int64_t k = 0; k < plen; ++k) buf[o++] = pre[k];
        for (int64_t k = 0; k < pad; ++k) buf[o++] = '0';
        memcpy(buf + o, piece->data + dstart, (size_t)dlen);
        o += dlen;
    } else if (left) {
        if (sign) buf[o++] = sign;
        for (int64_t k = 0; k < plen; ++k) buf[o++] = pre[k];
        memcpy(buf + o, piece->data + dstart, (size_t)dlen);
        o += dlen;
        for (int64_t k = 0; k < pad; ++k) buf[o++] = ' ';
    } else {
        for (int64_t k = 0; k < pad; ++k) buf[o++] = ' ';
        if (sign) buf[o++] = sign;
        for (int64_t k = 0; k < plen; ++k) buf[o++] = pre[k];
        memcpy(buf + o, piece->data + dstart, (size_t)dlen);
        o += dlen;
    }
    PyAdamasStr* out = str_new(buf, outlen);
    free(buf);
    return out;
}

PyAdamasStr* pa_percent_format(PyAdamasStr* pat, PyAdamasList* args, PyAdamasStr* desc, int64_t is_bytes) {
    (void)is_bytes;
    if (!pat) return str_new("", 0);
    if (!args) args = pa_list_new(0, 0);
    PBuf out = {0};
    const char* p = pat->data;
    int64_t n = pat->len;
    int64_t ai = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (p[i] != '%') {
            pb_append(&out, &p[i], 1);
            continue;
        }
        ++i;
        if (i >= n) pa_raise_c("ValueError", "incomplete format");
        if (p[i] == '%') {
            pb_append(&out, "%", 1);
            continue;
        }
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (; i < n; ++i) {
            char c = p[i];
            if (c == '-') left = 1;
            else if (c == '0') zero = 1;
            else if (c == '+') plus = 1;
            else if (c == ' ') space = 1;
            else if (c == '#') alt = 1;
            else break;
        }
        int width = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            width = width * 10 + (p[i] - '0');
            ++i;
        }
        int prec = -1;
        if (i < n && p[i] == '.') {
            ++i;
            prec = 0;
            while (i < n && p[i] >= '0' && p[i] <= '9') {
                prec = prec * 10 + (p[i] - '0');
                ++i;
            }
        }
        if (i >= n) pa_raise_c("ValueError", "incomplete format");
        char conv = p[i];
        if (ai >= args->len)
            pa_raise_c("TypeError", "not enough arguments for format string");
        char dt = (desc && ai < desc->len) ? desc->data[ai] : 'x';
        int64_t raw = args->data[ai++];
        double dv = 0.0;
        if (dt == 'i' || dt == 'b') dv = (double)raw;
        else if (dt == 'f') memcpy(&dv, &raw, sizeof dv);

        char spec[64];
        int si = 0;
        if (left) spec[si++] = '<';
        /* %s/%r 的默认对齐是右（format 的 str 默认左），显式宽度时补 '>'。 */
        else if ((conv == 's' || conv == 'r') && width > 0) spec[si++] = '>';
        if (plus) spec[si++] = '+';
        else if (space) spec[si++] = ' ';
        int numeric = conv != 's' && conv != 'r' && conv != 'c';
        if (zero && numeric) spec[si++] = '0';
        if (width > 0) {
            int m = snprintf(spec + si, sizeof spec - (size_t)si, "%d", width);
            if (m > 0) si += m;
        }
        if (prec >= 0 && (conv == 's' || conv == 'r' || conv == 'f' || conv == 'F' ||
                          conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G')) {
            int m = snprintf(spec + si, sizeof spec - (size_t)si, ".%d", prec);
            if (m > 0) si += m;
        }

        PyAdamasStr* piece = NULL;
        PyAdamasStr* tmp = NULL;
        int tmp_owned = 0;
        switch (conv) {
            case 'd':
            case 'i':
            case 'u':
            case 'n':
                if (dt == 'B') {
                    piece = pct_big_int(raw, spec, si, 'd');
                    break;
                }
                if (prec >= 0) {
                    uint64_t mag;
                    char sign = 0;
                    if (dt == 'u') {
                        mag = (uint64_t)raw;
                    } else if (dt == 'i' || dt == 'b' || dt == 'f') {
                        int64_t iv = dt == 'f' ? (int64_t)dv : raw;
                        if (iv < 0) {
                            sign = '-';
                            mag = (uint64_t)(-(iv + 1)) + 1;
                        } else {
                            mag = (uint64_t)iv;
                            if (plus) sign = '+';
                            else if (space) sign = ' ';
                        }
                    } else {
                        pa_raise_c("TypeError", "%%d format: a number is required");
                    }
                    piece = pct_fmt_int('d', mag, sign, left, zero, width, prec);
                    break;
                }
                spec[si] = 'd';
                if (dt == 'u')
                    piece = fmt_with(spec, si + 1, raw, 0.0, NULL, 'u');
                else if (dt == 'i' || dt == 'b')
                    piece = fmt_with(spec, si + 1, raw, 0.0, NULL, 'i');
                else if (dt == 'f')
                    piece = fmt_with(spec, si + 1, (int64_t)dv, 0.0, NULL, 'i');
                else
                    pa_raise_c("TypeError", "%%d format: a number is required");
                break;
            case 'x':
            case 'X':
            case 'o':
                if (dt == 'B') {
                    piece = pct_big_int(raw, spec, si, conv == 'o' ? 'o' : conv);
                    break;
                }
                if (dt != 'i' && dt != 'b' && dt != 'u')
                    pa_raise_c("TypeError", "%%x format: an integer is required");
                if (prec >= 0) {
                    uint64_t mag;
                    char sign = 0;
                    if (dt == 'u') {
                        mag = (uint64_t)raw;
                    } else if (raw < 0) {
                        sign = '-';
                        mag = (uint64_t)(-(raw + 1)) + 1;
                    } else {
                        mag = (uint64_t)raw;
                        if (plus) sign = '+';
                        else if (space) sign = ' ';
                    }
                    piece = pct_fmt_int(conv, mag, sign, left, zero, width, prec);
                    break;
                }
                spec[si] = conv;
                piece = fmt_with(spec, si + 1, raw, 0.0, NULL, dt == 'u' ? 'u' : 'i');
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                if (dt != 'f' && dt != 'i' && dt != 'b' && dt != 'u')
                    pa_raise_c("TypeError", "%%f format: a number is required");
                spec[si] = conv;
                piece = fmt_with(spec, si + 1, 0, dv, NULL, 'f');
                break;
            case 'c': {
                spec[si] = 's';
                if (dt == 'i' || dt == 'b') {
                    if (raw < 0 || raw > 255)
                        pa_raise_c("ValueError", "%%c requires a character code in range(256)");
                    char ch = (char)raw;
                    tmp = str_new(&ch, 1);
                    tmp_owned = 1;
                } else if (dt == 's') {
                    tmp = (PyAdamasStr*)raw;
                    if (!tmp || tmp->len != 1)
                        pa_raise_c("ValueError", "%%c requires a single character");
                } else {
                    pa_raise_c("TypeError", "%%c requires an int or a single character");
                }
                /* tmp 是 %c 的临时单字符串；fmt_with 里还会 str_new。 */
                pa_gc_pin(tmp);
                piece = fmt_with(spec, si + 1, 0, 0.0, tmp, 's');
                pa_gc_unpin(tmp);
                break;
            }
            case 's':
            case 'r': {
                spec[si] = 's';
                PyAdamasStr* sv = NULL;
                if (dt == 's') {
                    sv = (PyAdamasStr*)raw;
                    if (conv == 'r') {
                        tmp = pa_repr_str(sv);
                        sv = tmp;
                        tmp_owned = 1;
                    }
                } else if (dt == 'B') {
                    /* big：%s / %r 用十进制表示（repr 同形）。 */
                    tmp = pa_big_str((PyAdamasBig*)(uintptr_t)raw);
                    sv = tmp;
                    tmp_owned = 1;
                } else if (dt == 'i') {
                    tmp = pa_str_from_i64(raw);
                    sv = tmp;
                    tmp_owned = 1;
                } else if (dt == 'b') {
                    tmp = pa_str_from_bool(raw);
                    sv = tmp;
                    tmp_owned = 1;
                } else if (dt == 'n') {
                    tmp = pa_str_from_none();
                    sv = tmp;
                    tmp_owned = 1;
                } else if (dt == 'f') {
                    tmp = pa_str_from_f64(dv);
                    sv = tmp;
                    tmp_owned = 1;
                } else {
                    pa_raise_c("TypeError", "unsupported format argument type");
                }
                /* sv 常是 pa_str_from_* / pa_repr_str 刚产生的临时串，
                   只在本帧 C 局部变量里；fmt_with 内部还要再分配。 */
                pa_gc_pin(tmp);
                piece = fmt_with(spec, si + 1, 0, 0.0, sv, 's');
                pa_gc_unpin(tmp);
                break;
            }
            default:
                pa_raise_c("ValueError", "unsupported format character");
        }
        if (!piece) pa_raise_c("ValueError", "format failed");
        if (alt && (conv == 'x' || conv == 'X' || conv == 'o')) {
            const char* pre = conv == 'x' ? "0x" : (conv == 'X' ? "0X" : "0o");
            PyAdamasStr* joined = pct_alt_join(pre, 2, piece, width, zero, left);
            pa_rc_dec(piece);
            piece = joined;
        }
        pb_append(&out, piece->data, piece->len);
        pa_rc_dec(piece);
        if (tmp_owned && tmp && tmp != piece) pa_rc_dec(tmp);
    }
    if (ai < args->len)
        pa_raise_c("TypeError", "not all arguments converted during string formatting");
    PyAdamasStr* res = str_new(out.p ? out.p : "", out.len);
    free(out.p);
    return res;
}

/* unknown 值的真值（Python 语义）：0/0.0/空串/空容器/None 都是假。
   emit_truth 对静态类型 unknown 的操作数走这里 —— 原来按整数比较，
   空串（非空指针）因此被当成真，filter(None, 异构列表) 会保留空串（N7）。 */
int64_t pa_truth_unknown(int64_t v) {
    if (!v) return 0;
    if (!pa_unknown_is_heap(v)) return 1; /* 非零标量 */
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    switch (h->kind) {
        case PYADAMAS_RC_STR: return ((PyAdamasStr*)(uintptr_t)v)->len > 0 ? 1 : 0;
        case PYADAMAS_RC_LIST: return ((PyAdamasList*)(uintptr_t)v)->len > 0 ? 1 : 0;
        case PYADAMAS_RC_DICT: return ((PyAdamasDict*)(uintptr_t)v)->len > 0 ? 1 : 0;
        case PYADAMAS_RC_BIGINT: return pa_big_truth((PyAdamasBig*)(uintptr_t)v);
        case PYADAMAS_RC_CPYFLOAT: {
            double d;
            memcpy(&d, (void*)(uintptr_t)v, sizeof d);
            return d != 0.0 ? 1 : 0;
        }
        /* 动态 JSON 的 null/bool 盒：None 假、bool 按 payload。 */
        case PYADAMAS_RC_JSONNULL: return 0;
        case PYADAMAS_RC_JSONBOOL: return *(int64_t*)(uintptr_t)v != 0;
        default: return 1;
    }
}

/* unknown 值的 repr 打印：容器内部的元素按 repr 语义（字符串带引号），
   与 print_elem 对已知 str 的处理一致。 */
void pa_print_any_repr(int64_t v) {
    if (pa_unknown_is_heap(v)) {
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
        if (h->kind == PYADAMAS_RC_STR) {
            PyAdamasStr* r = pa_repr_str((PyAdamasStr*)(uintptr_t)v);
            pa_print_str(r);
            pa_rc_dec(r);
            return;
        }
        if (h->kind == PYADAMAS_RC_BIGINT) {
            pa_print_any(v);
            return;
        }
        if (h->kind == PYADAMAS_RC_CPYFLOAT || h->kind == PYADAMAS_RC_LIST ||
            h->kind == PYADAMAS_RC_DICT) {
            pa_print_cpy_value(v);
            return;
        }
    }
    pa_print_i64(v);
}
