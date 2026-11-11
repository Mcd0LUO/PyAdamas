// rt_str.c —— 字符串与字符串方法
#include "rt_internal.h"

/* 元组的 repr：CPython 打印 (1, 2)，单元素带尾逗号 (1,)。
   元素按静态类型渲染：kind 0 int / 1 float / 2 str / 3 bool / 4 None。
   长度事先不知道，用可增长缓冲区拼。 */
static void tuple_buf_put(char** buf, size_t* cap, size_t* len, const char* p, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (*len + n + 1 > nc) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) panic_c("out of memory");
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, p, n);
    *len += n;
    (*buf)[*len] = 0;
}

Py23Str* py23_repr_tuple(Py23List* t, int64_t kind) {
    size_t cap = 64, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) panic_c("out of memory");
    buf[0] = 0;
    tuple_buf_put(&buf, &cap, &len, "(", 1);
    for (int64_t i = 0; i < t->len; ++i) {
        if (i) tuple_buf_put(&buf, &cap, &len, ", ", 2);
        int64_t v = t->data[i];
        Py23Str* s;
        if (kind == 1) {
            double f;
            memcpy(&f, &v, sizeof f);
            s = py23_str_from_f64(f);
        } else if (kind == 2) {
            s = py23_repr_str((Py23Str*)(uintptr_t)v);
        } else if (kind == 3) {
            s = py23_str_from_bool(v);
        } else if (kind == 4) {
            s = py23_str_from_none();
        } else {
            s = py23_str_from_i64(v);
        }
        tuple_buf_put(&buf, &cap, &len, s->data, (size_t)s->len);
    }
    if (t->len == 1) tuple_buf_put(&buf, &cap, &len, ",", 1);
    tuple_buf_put(&buf, &cap, &len, ")", 1);
    Py23Str* out = str_new(buf, (int64_t)len);
    free(buf);
    return out;
}
Py23Str* py23_str_concat(Py23Str* a, Py23Str* b) {
    Py23Str* s = str_alloc(a->len + b->len);
    char* p = (char*)s->data;
    memcpy(p, a->data, (size_t)a->len);
    memcpy(p + a->len, b->data, (size_t)b->len);
    return s;
}

Py23Str* py23_str_char_at(Py23Str* s, int64_t i) {
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) py23_raise_c("IndexError", "string index out of range");
    return str_new(s->data + i, 1);
}

Py23Str* py23_str_from_i64(int64_t v) {
    // 手写十进制：snprintf 的格式解析在 str(i) 这种热路径上占大头。
    // 从缓冲区尾部往前写，负数补符号；INT64_MIN 用 -(v+1)+1 取绝对值。
    char buf[21];
    char* end = buf + sizeof buf;
    char* p = end;
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do {
        *--p = (char)('0' + (int)(mag % 10));
        mag /= 10;
    } while (mag);
    if (v < 0) *--p = '-';
    return str_new(p, (int64_t)(end - p));
}

Py23Str* py23_str_from_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    return str_new(buf, (int64_t)strlen(buf));
}

Py23Str* py23_str_from_bool(int64_t v) {
    return v ? str_new("True", 4) : str_new("False", 5);
}

Py23Str* py23_str_from_none(void) { return str_new("None", 4); }

/* ── 字符串/数值转换 ────────────────────────────────────────────
   字符串是字节串；int()/float() 的解析按 CPython 的规则做（允许
   前后空白、下划线、0x/0o/0b 前缀），失败抛 ValueError。 */

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int64_t py23_str_to_i64(Py23Str* s, int64_t base) {
    if (base != 0 && (base < 2 || base > 36))
        py23_raise_c("ValueError", "int() base must be >= 2 and <= 36");
    const char* p = s->data;
    const char* end = s->data + s->len;
    while (p < end && is_space(*p)) ++p;
    int neg = 0;
    if (p < end && (*p == '+' || *p == '-')) {
        neg = *p == '-';
        ++p;
    }
    int b = (int)(base == 0 ? 10 : base);
    if (p + 1 < end && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O' || p[1] == 'b' ||
         p[1] == 'B')) {
        int pref = p[1] == 'x' || p[1] == 'X' ? 16 : p[1] == 'o' || p[1] == 'O' ? 8 : 2;
        if (base != 0 && base != pref)
            py23_raise_c("ValueError", "invalid literal for int()");
        b = pref;
        p += 2;
    }
    uint64_t acc = 0;
    int any = 0;
    while (p < end) {
        char c = *p;
        if (c == '_') {
            if (!any) break;
            ++p;
            continue;
        }
        int d = digit_val(c);
        if (d < 0 || d >= b) break;
        acc = acc * (uint64_t)b + (uint64_t)d;
        any = 1;
        ++p;
    }
    while (p < end && is_space(*p)) ++p;
    if (!any || p != end) py23_raise_c("ValueError", "invalid literal for int()");
    return neg ? (int64_t)(0 - acc) : (int64_t)acc;
}

double py23_str_to_f64(Py23Str* s) {
    char stackbuf[64];
    char* buf = stackbuf;
    if (s->len + 1 > (int64_t)sizeof stackbuf) {
        buf = (char*)malloc((size_t)s->len + 1);
        if (!buf) panic_c("out of memory");
    }
    memcpy(buf, s->data, (size_t)s->len);
    buf[s->len] = 0;
    char* endp = NULL;
    double v = strtod(buf, &endp);
    int bad = (endp == buf);
    while (endp && *endp && is_space(*endp)) ++endp;
    if (endp && *endp) bad = 1;
    if (buf != stackbuf) free(buf);
    if (bad) py23_raise_c("ValueError", "could not convert string to float");
    return v;
}

int64_t py23_ord(Py23Str* s) {
    if (s->len != 1)
        py23_raise_c("TypeError", "ord() expected a character, but string length differs");
    return (int64_t)(unsigned char)s->data[0];
}

Py23Str* py23_chr(int64_t c) {
    /* 字节串：只支持 0..255（见 compatibility.md）。 */
    if (c < 0 || c > 255) py23_raise_c("ValueError", "chr() arg not in range(256)");
    char b = (char)(unsigned char)c;
    return str_new(&b, 1);
}

Py23Str* py23_int_to_base(int64_t v, int64_t base) {
    static const char* digits = "0123456789abcdef";
    char tmp[72];
    char* q = tmp + sizeof tmp;
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int n = 0;
    do {
        *--q = digits[mag % (uint64_t)base];
        mag /= (uint64_t)base;
        ++n;
    } while (mag);
    const char* prefix = base == 16 ? "0x" : base == 8 ? "0o" : "0b";
    char out[80];
    int k = 0;
    if (v < 0) out[k++] = '-';
    for (const char* pp = prefix; *pp; ++pp) out[k++] = *pp;
    memcpy(out + k, q, (size_t)n);
    k += n;
    return str_new(out, k);
}

double py23_round_f64(double x, int64_t ndigits) {
    /* 无 digits 时 nearbyint 默认按「四舍六入五成双」，和 CPython 一致。
       正 digits 用 printf 的正确舍入（直接对二进制值的十进制表示取整），
       2.675 这种缩放会引入误差的值才能和 CPython 对上。 */
    if (ndigits == 0) return nearbyint(x);
    if (ndigits > 0 && ndigits <= 17 && fabs(x) < 1e300) {
        char buf[512];
        snprintf(buf, sizeof buf, "%.*f", (int)ndigits, x);
        return strtod(buf, NULL);
    }
    if (ndigits < 0 && ndigits >= -15) {
        double scale = pow(10.0, (double)(-ndigits));
        return nearbyint(x / scale) * scale;
    }
    return x;
}

int64_t py23_round_i64(int64_t v, int64_t ndigits) {
    if (ndigits >= 0) return v;
    int64_t p = 1;
    for (int64_t i = 0; i < -ndigits && i < 18; ++i) p *= 10;
    int64_t q = v / p;
    int64_t r = v % p;
    int64_t ar = r < 0 ? -r : r;
    int64_t half = p / 2;
    if (ar > half || (ar == half && (q % 2 != 0))) q += v < 0 ? -1 : 1;
    return q * p;
}

int64_t py23_str_eq(Py23Str* a, Py23Str* b) {
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, (size_t)a->len) == 0;
}

/* 三路比较，供 `<` / `<=` / `>` / `>=` 用。
   UTF-8 的字节序与码点序一致，所以 memcmp 的结果就是 Python 的结果。 */
int64_t py23_str_cmp(Py23Str* a, Py23Str* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, (size_t)n);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

/* ── 字符串方法（按字节串语义，op 与 src/ir.h 的 StrOp 同序） ──── */

static int sm_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
static int sm_is_digit(char c) { return c >= '0' && c <= '9'; }
static int sm_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int sm_is_alnum(char c) { return sm_is_digit(c) || sm_is_alpha(c); }
static char sm_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
static char sm_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static void sm_push_str(Py23List* l, Py23Str* s) {
    py23_rc_inc(s);
    py23_list_append(l, (int64_t)s);
}

static int sm_char_in_set(char c, Py23Str* set) {
    for (int64_t i = 0; i < set->len; ++i)
        if (set->data[i] == c) return 1;
    return 0;
}

static int64_t sm_find_from(Py23Str* s, Py23Str* sub, int64_t start, int64_t end) {
    if (start < 0) start += s->len;
    if (start < 0) start = 0;
    if (end < 0) end += s->len;
    if (end > s->len) end = s->len;
    if (end < 0) end = 0;
    if (sub->len == 0) return start <= end ? start : -1;
    for (int64_t i = start; i + sub->len <= end; ++i)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    return -1;
}

int64_t py23_str_method(Py23Str* s, int64_t op, int64_t a0, int64_t a1, int64_t a2) {
    switch (op) {
        case 0:   /* upper */
        case 1: { /* lower */
            Py23Str* out = str_alloc(s->len);
            char* p = (char*)out->data;
            for (int64_t i = 0; i < s->len; ++i)
                p[i] = op == 0 ? sm_upper(s->data[i]) : sm_lower(s->data[i]);
            return (int64_t)out;
        }
        case 2:   /* strip */
        case 3:   /* lstrip */
        case 4:   /* rstrip */
        case 5:   /* strip(chars) */
        case 6:   /* lstrip(chars) */
        case 7: { /* rstrip(chars) */
            Py23Str* chars = (Py23Str*)a0;
            int left = op == 2 || op == 3 || op == 5 || op == 6;
            int right = op == 2 || op == 4 || op == 5 || op == 7;
            int64_t b = 0, e = s->len;
            if (left)
                while (b < e && (chars ? sm_char_in_set(s->data[b], chars)
                                       : sm_is_space(s->data[b])))
                    ++b;
            if (right)
                while (e > b && (chars ? sm_char_in_set(s->data[e - 1], chars)
                                       : sm_is_space(s->data[e - 1])))
                    --e;
            return (int64_t)str_new(s->data + b, e - b);
        }
        case 8: { /* split() 按空白 */
            Py23List* out = py23_list_new(4, 1);
            int64_t i = 0;
            while (i < s->len) {
                while (i < s->len && sm_is_space(s->data[i])) ++i;
                if (i >= s->len) break;
                int64_t b = i;
                while (i < s->len && !sm_is_space(s->data[i])) ++i;
                sm_push_str(out, str_new(s->data + b, i - b));
            }
            return (int64_t)out;
        }
        case 9: { /* split(sep, maxsplit) */
            Py23Str* sep = (Py23Str*)a0;
            int64_t maxsplit = a1;
            if (sep->len == 0) py23_raise_c("ValueError", "empty separator");
            Py23List* out = py23_list_new(4, 1);
            int64_t i = 0, done = 0;
            while (i <= s->len) {
                if (maxsplit >= 0 && done >= maxsplit) break;
                int64_t j = i;
                while (j + sep->len <= s->len &&
                       memcmp(s->data + j, sep->data, (size_t)sep->len) != 0)
                    ++j;
                if (j + sep->len > s->len) break;
                sm_push_str(out, str_new(s->data + i, j - i));
                i = j + sep->len;
                ++done;
            }
            sm_push_str(out, str_new(s->data + i, s->len - i));
            return (int64_t)out;
        }
        case 10: { /* join(parts) */
            Py23List* parts = (Py23List*)a0;
            int64_t total = 0;
            for (int64_t i = 0; i < parts->len; ++i)
                total += ((Py23Str*)parts->data[i])->len;
            if (parts->len > 1) total += s->len * (parts->len - 1);
            Py23Str* out = str_alloc(total);
            char* p = (char*)out->data;
            int64_t pos = 0;
            for (int64_t i = 0; i < parts->len; ++i) {
                if (i > 0) {
                    memcpy(p + pos, s->data, (size_t)s->len);
                    pos += s->len;
                }
                Py23Str* part = (Py23Str*)parts->data[i];
                memcpy(p + pos, part->data, (size_t)part->len);
                pos += part->len;
            }
            return (int64_t)out;
        }
        case 11: { /* replace(old, new, count) */
            Py23Str* old = (Py23Str*)a0;
            Py23Str* nw = (Py23Str*)a1;
            int64_t count = a2;
            // 先数出现次数（空 old 特殊：在每对字符之间插入）
            int64_t hits = 0;
            if (old->len == 0) {
                hits = s->len + 1;
                if (count >= 0 && hits > count) hits = count;
            } else {
                int64_t i = 0;
                while (i + old->len <= s->len) {
                    if (memcmp(s->data + i, old->data, (size_t)old->len) == 0) {
                        ++hits;
                        if (count >= 0 && hits >= count) break;
                        i += old->len;
                    } else {
                        ++i;
                    }
                }
            }
            int64_t newlen = s->len + hits * (nw->len - old->len);
            if (old->len == 0 && s->len > 0) newlen = s->len + hits * nw->len;
            Py23Str* out = str_alloc(newlen);
            char* p = (char*)out->data;
            int64_t pos = 0, used = 0, i = 0;
            if (old->len == 0) {
                for (int64_t k = 0; k <= s->len; ++k) {
                    if (used < hits) {
                        memcpy(p + pos, nw->data, (size_t)nw->len);
                        pos += nw->len;
                        ++used;
                    }
                    if (k < s->len) p[pos++] = s->data[k];
                }
            } else {
                while (i < s->len) {
                    if (used < hits && i + old->len <= s->len &&
                        memcmp(s->data + i, old->data, (size_t)old->len) == 0) {
                        memcpy(p + pos, nw->data, (size_t)nw->len);
                        pos += nw->len;
                        i += old->len;
                        ++used;
                    } else {
                        p[pos++] = s->data[i++];
                    }
                }
            }
            return (int64_t)out;
        }
        case 12: { /* find(sub, start, end) */
            return sm_find_from(s, (Py23Str*)a0, a1, a2);
        }
        case 13: { /* rfind */
            Py23Str* sub = (Py23Str*)a0;
            int64_t start = a1, end = a2;
            if (start < 0) start += s->len;
            if (start < 0) start = 0;
            if (end < 0) end += s->len;
            if (end > s->len) end = s->len;
            if (end < 0) end = 0;
            if (sub->len == 0) return end >= start ? end : -1;
            for (int64_t i = end - sub->len; i >= start; --i)
                if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
            return -1;
        }
        case 14:   /* startswith */
        case 15: { /* endswith */
            Py23Str* affix = (Py23Str*)a0;
            if (affix->len > s->len) return 0;
            if (op == 14)
                return memcmp(s->data, affix->data, (size_t)affix->len) == 0 ? 1 : 0;
            return memcmp(s->data + s->len - affix->len, affix->data,
                          (size_t)affix->len) == 0
                       ? 1
                       : 0;
        }
        case 16: { /* count(sub) */
            Py23Str* sub = (Py23Str*)a0;
            if (sub->len == 0) return s->len + 1;
            int64_t n = 0, i = 0;
            while (i + sub->len <= s->len) {
                if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) {
                    ++n;
                    i += sub->len;
                } else {
                    ++i;
                }
            }
            return n;
        }
        case 17: { /* zfill(width) */
            int64_t width = a0;
            if (width <= s->len) return (int64_t)str_new(s->data, s->len);
            int64_t pad = width - s->len;
            Py23Str* out = str_alloc(width);
            char* p = (char*)out->data;
            int64_t pos = 0;
            int64_t start = 0;
            if (s->len > 0 && (s->data[0] == '+' || s->data[0] == '-')) {
                p[pos++] = s->data[0];
                start = 1;
            }
            for (int64_t i = 0; i < pad; ++i) p[pos++] = '0';
            memcpy(p + pos, s->data + start, (size_t)(s->len - start));
            return (int64_t)out;
        }
        case 18:   /* ljust(width, fill) */
        case 19:   /* rjust */
        case 20: { /* center */
            int64_t width = a0;
            Py23Str* fill = (Py23Str*)a1;
            char fc = ' ';
            if (fill) {
                if (fill->len == 0) py23_raise_c("ValueError", "empty fill character");
                fc = fill->data[0];
            }
            if (width <= s->len) return (int64_t)str_new(s->data, s->len);
            int64_t pad = width - s->len;
            int64_t left = op == 18 ? 0 : op == 19 ? pad : pad / 2;
            int64_t right = pad - left;
            Py23Str* out = str_alloc(width);
            char* p = (char*)out->data;
            int64_t pos = 0;
            for (int64_t i = 0; i < left; ++i) p[pos++] = fc;
            memcpy(p + pos, s->data, (size_t)s->len);
            pos += s->len;
            for (int64_t i = 0; i < right; ++i) p[pos++] = fc;
            return (int64_t)out;
        }
        case 21: { /* capitalize */
            Py23Str* out = str_alloc(s->len);
            char* p = (char*)out->data;
            for (int64_t i = 0; i < s->len; ++i)
                p[i] = i == 0 ? sm_upper(s->data[i]) : sm_lower(s->data[i]);
            return (int64_t)out;
        }
        case 22: { /* title */
            Py23Str* out = str_alloc(s->len);
            char* p = (char*)out->data;
            int word = 1;
            for (int64_t i = 0; i < s->len; ++i) {
                char c = s->data[i];
                p[i] = word ? sm_upper(c) : sm_lower(c);
                word = !sm_is_alpha(c);
            }
            return (int64_t)out;
        }
        case 23:   /* isdigit */
        case 24:   /* isalpha */
        case 25:   /* isspace */
        case 26:   /* isupper */
        case 27:   /* islower */
        case 28: { /* isalnum */
            if (s->len == 0) return 0;
            int cased = 0;
            for (int64_t i = 0; i < s->len; ++i) {
                char c = s->data[i];
                int ok = 0;
                switch (op) {
                    case 23: ok = sm_is_digit(c); break;
                    case 24: ok = sm_is_alpha(c); break;
                    case 25: ok = sm_is_space(c); break;
                    case 26:
                        if (sm_is_alpha(c)) {
                            ++cased;
                            ok = !(c >= 'a' && c <= 'z');
                        } else {
                            ok = 1;
                        }
                        break;
                    case 27:
                        if (sm_is_alpha(c)) {
                            ++cased;
                            ok = !(c >= 'A' && c <= 'Z');
                        } else {
                            ok = 1;
                        }
                        break;
                    default: ok = sm_is_alnum(c); break;
                }
                if (!ok) return 0;
            }
            if ((op == 26 || op == 27) && cased == 0) return 0;
            return 1;
        }
        case 29:   /* removeprefix */
        case 30: { /* removesuffix */
            Py23Str* affix = (Py23Str*)a0;
            if (affix->len > s->len) return (int64_t)str_new(s->data, s->len);
            if (op == 29) {
                if (memcmp(s->data, affix->data, (size_t)affix->len) != 0)
                    return (int64_t)str_new(s->data, s->len);
                return (int64_t)str_new(s->data + affix->len, s->len - affix->len);
            }
            if (memcmp(s->data + s->len - affix->len, affix->data,
                       (size_t)affix->len) != 0)
                return (int64_t)str_new(s->data, s->len);
            return (int64_t)str_new(s->data, s->len - affix->len);
        }
        case 31: { /* splitlines */
            Py23List* out = py23_list_new(4, 1);
            int64_t b = 0, i = 0;
            while (i < s->len) {
                char c = s->data[i];
                if (c == '\n' || c == '\r') {
                    sm_push_str(out, str_new(s->data + b, i - b));
                    if (c == '\r' && i + 1 < s->len && s->data[i + 1] == '\n') ++i;
                    ++i;
                    b = i;
                } else {
                    ++i;
                }
            }
            if (b < s->len) sm_push_str(out, str_new(s->data + b, s->len - b));
            return (int64_t)out;
        }
        default:
            py23_raise_c("ValueError", "unknown string method");
    }
    return 0;
}

