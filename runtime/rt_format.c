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
    int grouping;
} PyAdamasFmt;

static void fmt_parse(PyAdamasStr* spec, PyAdamasFmt* f, int is_str) {
    f->fill = ' ';
    f->align = is_str ? '<' : '>';
    f->sign = 0;
    f->type = 0;
    f->width = 0;
    f->prec = -1;
    f->grouping = 0;
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
    if (i < n && p[i] == '#') i++;
    if (i < n && p[i] == '0') {
        f->fill = '0';
        if (!explicit_align) f->align = '=';
        i++;
    }
    while (i < n && p[i] >= '0' && p[i] <= '9') f->width = f->width * 10 + (p[i++] - '0');
    if (i < n && (p[i] == ',' || p[i] == '_')) {
        f->grouping = 1;
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

static PyAdamasStr* fmt_pad(const char* sign, const char* body, const PyAdamasFmt* f) {
    size_t sl = sign ? 1 : 0;
    size_t bl = strlen(body);
    size_t total = sl + bl;
    char* out;
    if (f->width <= (int)total) {
        out = (char*)malloc(total + 1);
        if (!out) panic_c("out of memory");
        if (sl) out[0] = *sign;
        memcpy(out + sl, body, bl);
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
        memcpy(q, body, bl);
        q += bl;
    } else if (f->align == '^') {
        int left = pad / 2;
        int right = pad - left;
        for (int i = 0; i < left; ++i) *q++ = f->fill;
        if (sl) *q++ = *sign;
        memcpy(q, body, bl);
        q += bl;
        for (int i = 0; i < right; ++i) *q++ = f->fill;
    } else if (f->align == '=') {
        if (sl) *q++ = *sign;
        for (int i = 0; i < pad; ++i) *q++ = f->fill;
        memcpy(q, body, bl);
        q += bl;
    } else {
        if (sl) *q++ = *sign;
        memcpy(q, body, bl);
        q += bl;
        for (int i = 0; i < pad; ++i) *q++ = f->fill;
    }
    *q = '\0';
    PyAdamasStr* s = str_new(out, (int64_t)(q - out));
    free(out);
    return s;
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

static void fmt_group(char* body) {
    char tmp[96];
    int n = (int)strlen(body);
    int j = 0;
    for (int i = 0; i < n; ++i) {
        int left = n - i;
        tmp[j++] = body[i];
        if (left > 1 && (left - 1) % 3 == 0) tmp[j++] = ',';
    }
    tmp[j] = '\0';
    strcpy(body, tmp);
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
    if (f.grouping && base == 10) fmt_group(body);
    return fmt_pad(sign ? &sign : NULL, body, &f);
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
    if (type == 'f' || type == 'F' || type == '%') {
        snprintf(body, sizeof body, "%.*f", prec < 0 ? 6 : prec, mag);
    } else if (type == 'e' || type == 'E') {
        snprintf(body, sizeof body, type == 'e' ? "%.*e" : "%.*E", prec < 0 ? 6 : prec, mag);
    } else if (type == 'g' || type == 'G') {
        snprintf(body, sizeof body, type == 'g' ? "%.*g" : "%.*G", prec < 0 ? 6 : prec, mag);
    } else if (prec >= 0) {
        snprintf(body, sizeof body, "%.*g", prec, mag);
    } else {
        format_double(body, sizeof body, mag);
    }
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
    char* body = (char*)malloc((size_t)cs->len + 1);
    if (!body) panic_c("out of memory");
    memcpy(body, cs->data, (size_t)cs->len + 1);
    PyAdamasStr* out = fmt_pad(NULL, body, &f);
    free(body);
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

static PyAdamasStr* fmt_with(const char* spec, int n, int64_t raw, double d, PyAdamasStr* s, char kind) {
    PyAdamasStr sp;
    sp.len = n;
    sp.hash = 0;
    sp.data = spec;
    switch (kind) {
        case 'i': return pa_fmt_i64(raw, &sp);
        case 'f': return pa_fmt_f64(d, &sp);
        case 's': return pa_fmt_str(s, &sp);
        default: return NULL;
    }
}

/* desc 是编译期生成的类型描述：i=int/bool，f=float，s=str/bytes。
   数字格式（d/i/u/x/X/o/f/F/e/E/g/G）要求对应数值类型；s/r 可以
   格式化任意受支持的类型。返回新字符串，bytes 与 str 共用表示。 */
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
        int left = 0, zero = 0, plus = 0, space = 0;
        for (; i < n; ++i) {
            char c = p[i];
            if (c == '-') left = 1;
            else if (c == '0') zero = 1;
            else if (c == '+') plus = 1;
            else if (c == ' ') space = 1;
            else if (c == '#') { /* 前缀 # 暂不支持 */ }
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
                spec[si] = 'd';
                if (dt == 'i' || dt == 'b')
                    piece = fmt_with(spec, si + 1, raw, 0.0, NULL, 'i');
                else if (dt == 'f')
                    piece = fmt_with(spec, si + 1, (int64_t)dv, 0.0, NULL, 'i');
                else
                    pa_raise_c("TypeError", "%%d format: a number is required");
                break;
            case 'x':
            case 'X':
            case 'o':
                if (dt != 'i' && dt != 'b')
                    pa_raise_c("TypeError", "%%x format: an integer is required");
                spec[si] = conv;
                piece = fmt_with(spec, si + 1, raw, 0.0, NULL, 'i');
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                if (dt != 'f' && dt != 'i' && dt != 'b')
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
                piece = fmt_with(spec, si + 1, 0, 0.0, tmp, 's');
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
                piece = fmt_with(spec, si + 1, 0, 0.0, sv, 's');
                break;
            }
            default:
                pa_raise_c("ValueError", "unsupported format character");
        }
        if (!piece) pa_raise_c("ValueError", "format failed");
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

