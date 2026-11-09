// rt_obj.c —— 字符串/列表/字典/切片/对象/输出/math
#include "rt_internal.h"

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

/* ── 字符串 ───────────────────────────────────────────────────── */

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
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return str_new(buf, n);
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

/* ── 列表 ─────────────────────────────────────────────────────── */

void list_destroy(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind == 1)
        for (int64_t i = 0; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    free(l->data);
}

// GC 清扫前调用：只把「指向可达对象」的引用计数减掉。kind 2 的元素类型
// 不可知，跳过（那些对象如果只被这个容器引用，会在同一轮清扫里被释放）。
void list_gc_release(void* p) {
    Py23List* l = (Py23List*)p;
    if (l->elem_kind != 1 || !l->data) return;
    for (int64_t i = 0; i < l->len; ++i) py23_gc_unref_child((void*)l->data[i]);
}

Py23List* py23_list_new(int64_t cap, int64_t elem_kind) {
    if (cap < 0) panic_c("negative list capacity");
    if (elem_kind < 0 || elem_kind > 2) panic_c("invalid list element kind");
    if (cap < 4) cap = 4;
    Py23List* l = (Py23List*)rc_alloc((int64_t)sizeof(Py23List), list_destroy,
                                      list_gc_release, PY23_RC_LIST);
    l->len = 0;
    l->cap = cap;
    l->elem_kind = elem_kind;
    l->data = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!l->data) panic_c("out of memory");
    return l;
}

/* 元素引用由生成代码在写入前 retain（kind 1/2 且源类型已知是堆），
   运行时不再重复 inc；kind 1 覆盖/截断/销毁时负责 release。 */
void py23_list_append(Py23List* l, int64_t v) {
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        int64_t* nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->data = nd;
        l->cap = ncap;
    }
    l->data[l->len++] = v;
}

int64_t py23_list_get(Py23List* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list index out of range");
    return l->data[i];
}

void py23_list_set(Py23List* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list assignment index out of range");
    int64_t old = l->data[i];
    l->data[i] = v;
    if (l->elem_kind == 1) py23_rc_dec((void*)old);
}

void py23_list_set_len(Py23List* l, int64_t n) {
    if (n < 0 || n > l->len) panic_c("invalid list length");
    if (l->elem_kind == 1)
        for (int64_t i = n; i < l->len; ++i) py23_rc_dec((void*)l->data[i]);
    l->len = n;
}

/* 把 src 的元素接到 dst 末尾，供 *args 打包和切片拷贝使用。
   元素的引用按 dst 的 elem_kind 处理：kind 1 正常 inc；kind 2 只对
   已知是堆的源元素 inc（不 release，交给 GC）。 */
void py23_list_extend(Py23List* dst, Py23List* src) {
    int64_t slen = src->len;
    int64_t* snap = NULL;
    const int64_t* sv = src->data;
    if (dst == src) {
        snap = (int64_t*)malloc(sizeof(int64_t) * (size_t)(slen ? slen : 1));
        if (!snap) panic_c("out of memory");
        if (slen) memcpy(snap, src->data, sizeof(int64_t) * (size_t)slen);
        sv = snap;
    }
    for (int64_t i = 0; i < slen; ++i) {
        int64_t v = sv[i];
        if (v && src->elem_kind == 1 &&
            (dst->elem_kind == 1 || dst->elem_kind == 2))
            py23_rc_inc((void*)v);
        py23_list_append(dst, v);
    }
    free(snap);
}

Py23List* py23_list_copy(Py23List* src) {
    Py23List* l = py23_list_new(src->len, src->elem_kind);
    py23_list_extend(l, src);
    return l;
}

Py23List* py23_list_reversed(Py23List* src) {
    int64_t n = src->len;
    Py23List* l = py23_list_new(n, src->elem_kind);
    for (int64_t i = n - 1; i >= 0; --i) {
        int64_t v = src->data[i];
        if (v && src->elem_kind == 1 && (l->elem_kind == 1 || l->elem_kind == 2))
            py23_rc_inc((void*)v);
        py23_list_append(l, v);
    }
    return l;
}

void py23_list_insert(Py23List* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0) i = 0;
    if (i > l->len) i = l->len;
    if (l->len == l->cap) {
        int64_t ncap = l->cap ? l->cap * 2 : 4;
        int64_t* nd = (int64_t*)realloc(l->data, sizeof(int64_t) * (size_t)ncap);
        if (!nd) panic_c("out of memory");
        l->data = nd;
        l->cap = ncap;
    }
    if (i < l->len)
        memmove(l->data + i + 1, l->data + i, sizeof(int64_t) * (size_t)(l->len - i));
    l->data[i] = v;
    ++l->len;
}

void py23_list_reverse(Py23List* l) {
    for (int64_t i = 0, j = l->len - 1; i < j; ++i, --j) {
        int64_t t = l->data[i];
        l->data[i] = l->data[j];
        l->data[j] = t;
    }
}

int64_t py23_int_bit_length(int64_t v) {
    uint64_t x = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int64_t n = 0;
    while (x) {
        ++n;
        x >>= 1;
    }
    return n;
}

static int cmp_sort_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int cmp_sort_f64(const void* a, const void* b) {
    double x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static int cmp_sort_str(const void* a, const void* b) {
    Py23Str* x = (Py23Str*)(*(const int64_t*)a);
    Py23Str* y = (Py23Str*)(*(const int64_t*)b);
    int64_t c = py23_str_cmp(x, y);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// kind：0 int/bool、1 float、2 str、其它按机器字比较。
void py23_list_sort(Py23List* l, int64_t kind) {
    if (l->len < 2) return;
    qsort(l->data, (size_t)l->len, sizeof(int64_t),
          kind == 2 ? cmp_sort_str : kind == 1 ? cmp_sort_f64 : cmp_sort_i64);
}

Py23List* py23_sorted_list(Py23List* src, int64_t kind) {
    Py23List* l = py23_list_copy(src);
    py23_list_sort(l, kind);
    return l;
}

Py23List* py23_enumerate_list(Py23List* seq) {
    int64_t n = seq->len;
    Py23List* out = py23_list_new(n, 1);  // 外层元素是 pair（堆）
    for (int64_t i = 0; i < n; ++i) {
        int64_t ev = seq->data[i];
        Py23List* pair = py23_list_new(2, seq->elem_kind);
        pair->len = 2;
        pair->data[0] = i;
        pair->data[1] = ev;
        if (seq->elem_kind == 1 && ev) py23_rc_inc((void*)ev);
        py23_rc_inc(pair);
        py23_list_append(out, (int64_t)pair);
    }
    return out;
}

Py23List* py23_zip2_list(Py23List* a, Py23List* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int ka = a->elem_kind, kb = b->elem_kind;
    // pair 的元素种类：两边都是堆才是 1；都是非堆是 0；混合按 2 保守处理。
    int pk = ka == 1 && kb == 1 ? 1 : (ka == 0 && kb == 0 ? 0 : 2);
    Py23List* out = py23_list_new(n, 1);
    for (int64_t i = 0; i < n; ++i) {
        int64_t x = a->data[i], y = b->data[i];
        Py23List* pair = py23_list_new(2, pk);
        pair->len = 2;
        pair->data[0] = x;
        pair->data[1] = y;
        if (pk != 0) {
            if (ka == 1 && x) py23_rc_inc((void*)x);
            if (kb == 1 && y) py23_rc_inc((void*)y);
        }
        py23_rc_inc(pair);
        py23_list_append(out, (int64_t)pair);
    }
    return out;
}

/* ── 列表归约（AVX2 快路径 + 标量回退） ─────────────────────── */

static int py23_has_avx2(void) {
#if defined(__x86_64__) && defined(__GNUC__)
    static int cached = -1;
    if (cached < 0) {
        __builtin_cpu_init();
        cached = __builtin_cpu_supports("avx2") ? 1 : 0;
    }
    return cached;
#else
    return 0;
#endif
}

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2")))
static uint64_t list_sum_u64_avx2(const int64_t* d, int64_t n) {
    __m256i acc = _mm256_setzero_si256();
    int64_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = _mm256_add_epi64(acc, _mm256_loadu_si256((const __m256i*)(d + i)));
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i*)lanes, acc);
    uint64_t s = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; ++i) s += (uint64_t)d[i];
    return s;
}

__attribute__((target("avx2")))
static double list_sum_f64_avx2(const double* d, int64_t n) {
    __m256d acc = _mm256_setzero_pd();
    int64_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = _mm256_add_pd(acc, _mm256_loadu_pd(d + i));
    double lanes[4];
    _mm256_storeu_pd(lanes, acc);
    double s = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; ++i) s += d[i];
    return s;
}

__attribute__((target("avx2")))
static int64_t list_min_i64_avx2(const int64_t* d, int64_t n) {
    int64_t i = 0;
    int64_t m;
    if (n >= 4) {
        __m256i best = _mm256_loadu_si256((const __m256i*)d);
        i = 4;
        for (; i + 4 <= n; i += 4) {
            __m256i v = _mm256_loadu_si256((const __m256i*)(d + i));
            __m256i take = _mm256_cmpgt_epi64(best, v);
            best = _mm256_blendv_epi8(best, v, take);
        }
        int64_t lanes[4];
        _mm256_storeu_si256((__m256i*)lanes, best);
        m = lanes[0];
        for (int j = 1; j < 4; ++j)
            if (lanes[j] < m) m = lanes[j];
    } else {
        m = d[0];
    }
    for (; i < n; ++i)
        if (d[i] < m) m = d[i];
    return m;
}

__attribute__((target("avx2")))
static int64_t list_max_i64_avx2(const int64_t* d, int64_t n) {
    int64_t i = 0;
    int64_t m;
    if (n >= 4) {
        __m256i best = _mm256_loadu_si256((const __m256i*)d);
        i = 4;
        for (; i + 4 <= n; i += 4) {
            __m256i v = _mm256_loadu_si256((const __m256i*)(d + i));
            __m256i take = _mm256_cmpgt_epi64(v, best);
            best = _mm256_blendv_epi8(best, v, take);
        }
        int64_t lanes[4];
        _mm256_storeu_si256((__m256i*)lanes, best);
        m = lanes[0];
        for (int j = 1; j < 4; ++j)
            if (lanes[j] > m) m = lanes[j];
    } else {
        m = d[0];
    }
    for (; i < n; ++i)
        if (d[i] > m) m = d[i];
    return m;
}
#endif

int64_t py23_list_sum_i64(Py23List* l, int64_t start) {
    uint64_t s = (uint64_t)start;
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2()) s += list_sum_u64_avx2(l->data, l->len);
    else
#endif
        for (int64_t i = 0; i < l->len; ++i) s += (uint64_t)l->data[i];
    return (int64_t)s;
}

double py23_list_sum_f64(Py23List* l, double start) {
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2())
        return start + list_sum_f64_avx2((const double*)l->data, l->len);
#endif
    double s = start;
    const double* d = (const double*)l->data;
    for (int64_t i = 0; i < l->len; ++i) s += d[i];
    return s;
}

int64_t py23_list_min_i64(Py23List* l) {
    if (l->len <= 0) py23_raise_c("ValueError", "min() arg is an empty sequence");
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2()) return list_min_i64_avx2(l->data, l->len);
#endif
    int64_t m = l->data[0];
    for (int64_t i = 1; i < l->len; ++i)
        if (l->data[i] < m) m = l->data[i];
    return m;
}

int64_t py23_list_max_i64(Py23List* l) {
    if (l->len <= 0) py23_raise_c("ValueError", "max() arg is an empty sequence");
#if defined(__x86_64__) && defined(__GNUC__)
    if (py23_has_avx2()) return list_max_i64_avx2(l->data, l->len);
#endif
    int64_t m = l->data[0];
    for (int64_t i = 1; i < l->len; ++i)
        if (l->data[i] > m) m = l->data[i];
    return m;
}

/* ── 字典 / 集合 ─────────────────────────────────────────────── */

uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

int64_t dict_hash_value(int64_t kind, int64_t v) {
    uint64_t h;
    if (kind == 1) {
        Py23Str* s = (Py23Str*)v;
        h = 1469598103934665603ull;
        for (int64_t i = 0; i < s->len; ++i) {
            h ^= (unsigned char)s->data[i];
            h *= 1099511628211ull;
        }
    } else if (kind == 2) {
        double d;
        memcpy(&d, &v, sizeof d);
        if (d == 0.0) v = 0;  /* 0.0 与 -0.0 同槽 */
        h = mix64((uint64_t)v);
    } else {
        h = mix64((uint64_t)v);
    }
    h = mix64(h);
    return (int64_t)(h ? h : 1);
}

int dict_key_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return py23_str_eq((Py23Str*)a, (Py23Str*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

int64_t dict_probe(Py23DictEntry* entries, int64_t cap, int64_t kind,
                          int64_t key, int64_t hash) {
    int64_t mask = cap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (entries[i].hash != 0) {
        if (entries[i].hash == hash && dict_key_eq(kind, entries[i].key, key)) return i;
        i = (i + 1) & mask;
    }
    return -1;
}

int64_t dict_empty_slot(Py23DictEntry* entries, int64_t cap, int64_t hash) {
    int64_t mask = cap - 1;
    int64_t i = (int64_t)((uint64_t)hash & (uint64_t)mask);
    while (entries[i].hash != 0) i = (i + 1) & mask;
    return i;
}

void dict_resize(Py23Dict* d, int64_t ncap) {
    Py23DictEntry* ne = (Py23DictEntry*)calloc((size_t)ncap, sizeof(Py23DictEntry));
    int64_t* no = (int64_t*)malloc(sizeof(int64_t) * (size_t)ncap);
    if (!ne || !no) panic_c("out of memory");
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        int64_t mask = ncap - 1;
        int64_t slot = (int64_t)((uint64_t)e.hash & (uint64_t)mask);
        while (ne[slot].hash != 0) slot = (slot + 1) & mask;
        ne[slot] = e;
        no[i] = slot;
    }
    free(d->entries);
    free(d->order);
    d->entries = ne;
    d->order = no;
    d->cap = ncap;
}

void dict_destroy(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    free(d->entries);
    free(d->order);
}

void dict_gc_release(void* p) {
    Py23Dict* d = (Py23Dict*)p;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_gc_unref_child((void*)e.key);
        if (d->val_own == 1) py23_gc_unref_child((void*)e.val);
    }
}

Py23Dict* py23_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own) {
    if (key_kind < 0 || key_kind > 3) panic_c("invalid dict key kind");
    if (key_own < 0 || key_own > 2 || val_own < 0 || val_own > 2)
        panic_c("invalid dict ownership kind");
    int64_t cap = 8;
    Py23Dict* d =
        (Py23Dict*)rc_alloc((int64_t)sizeof(Py23Dict), dict_destroy, dict_gc_release,
                            PY23_RC_DICT);
    d->len = 0;
    d->cap = cap;
    d->entries = (Py23DictEntry*)calloc((size_t)cap, sizeof(Py23DictEntry));
    d->order = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!d->entries || !d->order) panic_c("out of memory");
    d->key_kind = key_kind;
    d->key_own = key_own;
    d->val_own = val_own;
    return d;
}

/* 调用方已经把 key/value 的「入表引用」retain 好；更新已有 key 时
   释放这份多余的新 key 引用，字典继续保留原来的 key 对象。 */
void py23_dict_set(Py23Dict* d, int64_t key, int64_t val) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot >= 0) {
        if (d->key_own == 1) py23_rc_dec((void*)key);
        if (d->val_own == 1) py23_rc_dec((void*)d->entries[slot].val);
        d->entries[slot].val = val;
        return;
    }
    if ((d->len + 1) * 10 >= d->cap * 7) dict_resize(d, d->cap * 2);
    slot = dict_empty_slot(d->entries, d->cap, h);
    d->entries[slot].hash = h;
    d->entries[slot].key = key;
    d->entries[slot].val = val;
    d->order[d->len++] = slot;
}

int64_t py23_dict_get(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot < 0) py23_raise_c("KeyError", "key not found");
    return d->entries[slot].val;
}

int64_t py23_dict_contains(Py23Dict* d, int64_t key) {
    int64_t h = dict_hash_value(d->key_kind, key);
    return dict_probe(d->entries, d->cap, d->key_kind, key, h) >= 0 ? 1 : 0;
}

int64_t py23_dict_key_at(Py23Dict* d, int64_t i) {
    if (i < 0 || i >= d->len) panic_c("dict iteration index out of range");
    return d->entries[d->order[i]].key;
}

int64_t py23_dict_get_or(Py23Dict* d, int64_t key, int64_t def) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    return slot >= 0 ? d->entries[slot].val : def;
}

int own_to_list_kind(int64_t own) {
    return own == 1 ? 1 : (own == 0 ? 0 : 2);
}

Py23List* py23_dict_keys(Py23Dict* d) {
    int kind = own_to_list_kind(d->key_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[d->order[i]].key;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_values(Py23Dict* d) {
    int kind = own_to_list_kind(d->val_own);
    Py23List* out = py23_list_new(d->len, kind);
    for (int64_t i = 0; i < d->len; ++i) {
        int64_t v = d->entries[d->order[i]].val;
        if (kind == 1 && v) py23_rc_inc((void*)v);
        py23_list_append(out, v);
    }
    return out;
}

Py23List* py23_dict_items(Py23Dict* d) {
    // 单元素 kind 无法同时表达 key/value 两类所有权：都为堆用 1，
    // 都非堆用 0，混合用 2（已知堆的元素 retain，销毁时不解引用，靠 GC）。
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* out = py23_list_new(d->len, 1);
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        Py23List* tup = py23_list_new(2, tk);
        if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
        py23_list_append(tup, e.key);
        if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
        py23_list_append(tup, e.val);
        py23_list_append(out, (int64_t)tup);  // tuple 的初始引用交给外层 list
    }
    return out;
}

void py23_dict_update(Py23Dict* dst, Py23Dict* src) {
    if (dst == src) return;
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[src->order[i]];
        // 目标类型未知时也要保证源销毁后指针不悬空：源自己是堆所有权
        // 就 retain 一份。目标 own=2/0 不会 dec，这份引用会留给 GC。
        if ((dst->key_own == 1 || src->key_own == 1) && e.key)
            py23_rc_inc((void*)e.key);
        if ((dst->val_own == 1 || src->val_own == 1) && e.val)
            py23_rc_inc((void*)e.val);
        py23_dict_set(dst, e.key, e.val);
    }
}

/* 同 kind 的浅拷贝：所有权类别也复制过去，键值按需 retain。
   供 match 的 `**rest` 使用，随后再删掉显式匹配过的 key。 */
Py23Dict* py23_dict_copy(Py23Dict* src) {
    Py23Dict* out = py23_dict_new(src->key_kind, src->key_own, src->val_own);
    for (int64_t i = 0; i < src->len; ++i) {
        Py23DictEntry e = src->entries[src->order[i]];
        if (src->key_own == 1 && e.key) py23_rc_inc((void*)e.key);
        if (src->val_own == 1 && e.val) py23_rc_inc((void*)e.val);
        py23_dict_set(out, e.key, e.val);
    }
    return out;
}

Py23Dict* py23_set_from_list(Py23List* src, int64_t key_kind, int64_t key_own) {
    Py23Dict* d = py23_dict_new(key_kind, key_own, 0);
    for (int64_t i = 0; i < src->len; ++i) {
        int64_t k = src->data[i];
        if (key_own == 1 && k) py23_rc_inc((void*)k);
        py23_dict_set(d, k, 0);  // 重复 key 时 dict_set 会退掉这份多余引用
    }
    return d;
}

/* 删除用「移出 order + 全表重建」：线性探查没有墓碑，直接挖空会截断
   后续 key 的探查链。重建是 O(n)，删除本来也不在热路径上。 */
void dict_rebuild(Py23Dict* d) {
    Py23DictEntry* old = d->entries;
    Py23DictEntry* ne = (Py23DictEntry*)calloc((size_t)d->cap, sizeof(Py23DictEntry));
    if (!ne) panic_c("out of memory");
    int64_t mask = d->cap - 1;
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = old[d->order[i]];
        int64_t slot = (int64_t)((uint64_t)e.hash & (uint64_t)mask);
        while (ne[slot].hash != 0) slot = (slot + 1) & mask;
        ne[slot] = e;
        d->order[i] = slot;
    }
    free(old);
    d->entries = ne;
}

/* 找到并移除 key；返回 1 时 val_out 是原值。字典对 key/val 的引用：
   key 立即释放（调用方传的是借用），value 的所有权转交给调用方
   （pop 返回给 Python 层，由临时栈接管）。 */
int dict_take(Py23Dict* d, int64_t key, int64_t* val_out) {
    int64_t h = dict_hash_value(d->key_kind, key);
    int64_t slot = dict_probe(d->entries, d->cap, d->key_kind, key, h);
    if (slot < 0) return 0;
    *val_out = d->entries[slot].val;
    int64_t oi = -1;
    for (int64_t i = 0; i < d->len; ++i)
        if (d->order[i] == slot) {
            oi = i;
            break;
        }
    if (oi < 0) panic_c("dict order corrupted");
    if (d->key_own == 1) py23_rc_dec((void*)d->entries[slot].key);
    for (int64_t i = oi; i + 1 < d->len; ++i) d->order[i] = d->order[i + 1];
    --d->len;
    dict_rebuild(d);
    return 1;
}

int64_t py23_dict_pop(Py23Dict* d, int64_t key, int64_t has_default, int64_t def) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (has_default) return def;
        py23_raise_c("KeyError", "key not found");
    }
    return v;
}

Py23List* py23_dict_popitem(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "popitem(): dictionary is empty");
    int64_t slot = d->order[d->len - 1];
    Py23DictEntry e = d->entries[slot];
    int tk = (d->key_own == 1 && d->val_own == 1)
                 ? 1
                 : ((d->key_own == 0 && d->val_own == 0) ? 0 : 2);
    Py23List* tup = py23_list_new(2, tk);
    if ((tk == 1 || d->key_own == 1) && e.key) py23_rc_inc((void*)e.key);
    py23_list_append(tup, e.key);
    if ((tk == 1 || d->val_own == 1) && e.val) py23_rc_inc((void*)e.val);
    py23_list_append(tup, e.val);
    d->len--;
    if (d->key_own == 1) py23_rc_dec((void*)e.key);
    if (d->val_own == 1) py23_rc_dec((void*)e.val);
    dict_rebuild(d);
    return tup;
}

/* set.pop()：移除并返回最后一个插入项（py23 的确定性选择）。 */
int64_t py23_dict_pop_last(Py23Dict* d) {
    if (d->len == 0) py23_raise_c("KeyError", "pop from an empty set");
    int64_t slot = d->order[d->len - 1];
    int64_t key = d->entries[slot].key;
    d->len--;
    dict_rebuild(d);
    return key;  /* 所有权转交给调用方 */
}

void py23_dict_remove(Py23Dict* d, int64_t key, int64_t missing_ok) {
    int64_t v;
    if (!dict_take(d, key, &v)) {
        if (!missing_ok) py23_raise_c("KeyError", "key not found");
        return;
    }
    if (d->val_own == 1) py23_rc_dec((void*)v);
}

void py23_dict_clear(Py23Dict* d) {
    for (int64_t i = 0; i < d->len; ++i) {
        Py23DictEntry e = d->entries[d->order[i]];
        if (d->key_own == 1) py23_rc_dec((void*)e.key);
        if (d->val_own == 1) py23_rc_dec((void*)e.val);
    }
    d->len = 0;
    memset(d->entries, 0, sizeof(Py23DictEntry) * (size_t)d->cap);
}

int list_value_eq(int64_t kind, int64_t a, int64_t b) {
    if (kind == 1) return py23_str_eq((Py23Str*)a, (Py23Str*)b) != 0;
    if (kind == 2) {
        double x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        return x == y;
    }
    return a == b;
}

int64_t py23_list_contains(Py23List* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return 1;
    return 0;
}

int64_t py23_list_index(Py23List* l, int64_t v, int64_t kind) {
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) return i;
    py23_raise_c("ValueError", "value is not in list");
}

int64_t py23_list_count(Py23List* l, int64_t v, int64_t kind) {
    int64_t n = 0;
    for (int64_t i = 0; i < l->len; ++i)
        if (list_value_eq(kind, l->data[i], v)) ++n;
    return n;
}

int64_t py23_str_contains(Py23Str* hay, Py23Str* needle) {
    if (needle->len == 0) return 1;
    if (needle->len > hay->len) return 0;
    for (int64_t i = 0; i + needle->len <= hay->len; ++i)
        if (memcmp(hay->data + i, needle->data, (size_t)needle->len) == 0) return 1;
    return 0;
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
    Py23Str* out = str_alloc(count);
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

/* range(...) 作为值使用时实体化成整数列表。for 循环走的是专门的
   计数循环，不经过这里。 */
Py23List* py23_range_new(int64_t start, int64_t stop, int64_t step) {
    if (step == 0) py23_raise_c("ValueError", "range() arg 3 must not be zero");
    int64_t n;
    if (step > 0)
        n = stop > start ? (stop - start + step - 1) / step : 0;
    else
        n = stop < start ? (start - stop + (-step) - 1) / (-step) : 0;
    Py23List* out = py23_list_new(n, 0);
    for (int64_t i = 0, v = start; i < n; ++i, v += step)
        py23_list_append(out, v);
    return out;
}

/* ── 对象 ─────────────────────────────────────────────────────── */

/* 对象 = [vtable 指针][字段 0][字段 1]…。分配时全部清零：0 同时是
   None / 0 / 假，和栈槽的约定一致，所以「声明了但没赋值的字段」
   天然就是 None。vtable 指针由生成代码紧接着写入。
   dtor 由代码生成按类布局生成，只 dec 堆类型字段。 */
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*)) {
    if (size < 8) panic_c("object size too small");
    return rc_alloc(size, dtor, release, PY23_RC_INSTANCE);
}

/* 类模式的运行期类型检查。只认用户类实例；原始整数、str、list 等
   都不是类实例。查注册表保证对任意 unknown 机器字都安全。
   vtable 前 5 个字：MRO 元数据指针表、类编号、vtable、自身槽表、类名。 */
static const int64_t* inst_vtable(void* obj) {
    if (!obj) return NULL;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h)) return NULL;
    if (h->kind != PY23_RC_INSTANCE) return NULL;
    return *(int64_t**)obj;
}

int64_t py23_isinstance(void* obj, int64_t class_id) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return 0;
    const int64_t* meta = vt - 5;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    if (!mro) return 0;
    int64_t n = mro[0];
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        if (m && m[1] == class_id) return 1;
    }
    return 0;
}

/* 跨模块类模式：类编号只在模块内唯一，用「模块名.类名」比较。 */
int64_t py23_is_named_class(void* obj, Py23Str* qualname) {
    if (!qualname) return 0;
    const int64_t* vt = inst_vtable(obj);
    if (!vt) return 0;
    const int64_t* meta = vt - 5;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    if (!mro) return 0;
    int64_t n = mro[0];
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        const char* cn = m ? (const char*)(uintptr_t)m[4] : NULL;
        if (!cn) continue;
        size_t l = strlen(cn);
        if (l == (size_t)qualname->len &&
            memcmp(cn, qualname->data, l) == 0)
            return 1;
    }
    return 0;
}

/* super()：从实例的动态 MRO 里找 after 类之后的第一个「自己声明了
   该槽」的类，返回它的实现。没有就按 AttributeError 抛。 */
void* py23_super_lookup(void* obj, int64_t after_cid, int64_t slot, Py23Str* name) {
    const int64_t* vt = inst_vtable(obj);
    if (!vt) {
        py23_raise_c("AttributeError", "'super' object has no attribute");
        return NULL;
    }
    const int64_t* meta = vt - 5;
    const int64_t* mro = (const int64_t*)(uintptr_t)meta[0];
    int64_t n = mro ? mro[0] : 0;
    int found = 0;
    for (int64_t i = 1; i <= n; ++i) {
        const int64_t* m = (const int64_t*)(uintptr_t)mro[i];
        if (!m) continue;
        if (!found) {
            if (m[1] == after_cid) found = 1;
            continue;
        }
        const int64_t* own = (const int64_t*)(uintptr_t)m[3];
        if (!own) continue;
        int64_t k = own[0];
        for (int64_t j = 1; j <= k; ++j) {
            if (own[j] != slot) continue;
            const int64_t* ovt = (const int64_t*)(uintptr_t)m[2];
            if (ovt) return (void*)(uintptr_t)ovt[slot];
        }
    }
    char buf[192];
    if (name && name->len >= 0 && name->len < 150) {
        snprintf(buf, sizeof buf, "'super' object has no attribute '%.*s'",
                 (int)name->len, name->data);
    } else {
        snprintf(buf, sizeof buf, "'super' object has no attribute");
    }
    py23_raise_c("AttributeError", buf);
    return NULL;
}

int64_t py23_is_exc(void* obj, Py23Str* type) {
    if (!obj || !type) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    if (h->kind != PY23_RC_OBJ || h->size < (int64_t)sizeof(Py23ExcValue)) return 0;
    Py23ExcValue* v = (Py23ExcValue*)obj;
    if (v->magic != PY23_EXCVAL_MAGIC) return 0;
    char want[128];
    int64_t n = type->len;
    if (n < 0 || n >= (int64_t)sizeof(want)) return 0;
    memcpy(want, type->data, (size_t)n);
    want[n] = '\0';
    return exc_is_a(v->type, want) ? 1 : 0;
}

int64_t py23_is_seq(void* obj) {
    if (!obj) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    return h->kind == PY23_RC_LIST ? 1 : 0;
}

int64_t py23_is_dict(void* obj) {
    if (!obj) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    return h->kind == PY23_RC_DICT ? 1 : 0;
}

/* unknown 值上的字符串/复数模式：先按 RC 注册表确认载荷类型，再比较。 */
int64_t py23_str_eq_unknown(void* obj, Py23Str* s) {
    if (!obj || !s) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PY23_RC_STR) return 0;
    return py23_str_eq((Py23Str*)obj, s);
}

int64_t py23_complex_eq_unknown(void* obj, Py23Complex* c) {
    if (!obj || !c) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PY23_RC_OBJ ||
        h->size < (int64_t)sizeof(Py23Complex))
        return 0;
    Py23Complex* x = (Py23Complex*)obj;
    if (x->magic != PY23_COMPLEX_MAGIC) return 0;
    return py23_complex_eq(x, c);
}

int64_t py23_is_str(void* obj) {
    if (!obj) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h)) return 0;
    return h->kind == PY23_RC_STR ? 1 : 0;
}

int64_t py23_is_complex(void* obj) {
    if (!obj) return 0;
    Py23Rc* h = (Py23Rc*)obj - 1;
    if (!rc_index_has(h) || h->kind != PY23_RC_OBJ ||
        h->size < (int64_t)sizeof(Py23Complex))
        return 0;
    return ((Py23Complex*)obj)->magic == PY23_COMPLEX_MAGIC ? 1 : 0;
}

/* ── 数学 / 内置模块 ──────────────────────────────────────────── */

double py23_math_sqrt(double x) {
    if (x < 0) py23_raise_c("ValueError", "math domain error");
    return sqrt(x);
}
int64_t py23_math_floor(double x) { return (int64_t)floor(x); }
int64_t py23_math_ceil(double x) { return (int64_t)ceil(x); }
double py23_math_fabs(double x) { return fabs(x); }
double py23_math_sin(double x) { return sin(x); }
double py23_math_cos(double x) { return cos(x); }
double py23_math_tan(double x) { return tan(x); }
double py23_math_log(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log(x);
}
double py23_math_log2(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log2(x);
}
double py23_math_log10(double x) {
    if (x <= 0) py23_raise_c("ValueError", "math domain error");
    return log10(x);
}
double py23_math_exp(double x) { return exp(x); }
double py23_math_pow(double x, double y) { return pow(x, y); }
double py23_math_fmod(double x, double y) {
    if (y == 0) py23_raise_c("ValueError", "math domain error");
    return fmod(x, y);
}

Py23List* sys_argv_list;

void py23_sys_set_args(int64_t argc, char** argv) {
    Py23List* out = py23_list_new(argc > 0 ? argc : 0, 1);
    sys_argv_list = out;
    // 列表要立刻成为 GC 根；根表元素是「槽的地址」，不是槽里的值。
    void* root_slot[1] = {(void*)&sys_argv_list};
    py23_gc_add_roots(root_slot, 1);
    for (int64_t i = 0; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        Py23Str* str = str_new(s, (int64_t)strlen(s));
        py23_list_append(out, (int64_t)str);
    }
}

Py23List* py23_sys_argv(void) {
    if (!sys_argv_list) sys_argv_list = py23_list_new(0, 1);
    return sys_argv_list;
}

void py23_sys_exit(int64_t code) { exit((int)code); }

/* ── 输出 ─────────────────────────────────────────────────────── */

void py23_print_i64(int64_t v) { printf("%lld", (long long)v); }

void py23_print_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    fputs(buf, stdout);
}

void py23_print_bool(int64_t v) { fputs(v ? "True" : "False", stdout); }

void py23_print_str(Py23Str* s) { fwrite(s->data, 1, (size_t)s->len, stdout); }

void py23_print_none(void) { fputs("None", stdout); }

void py23_print_sep(void) { fputc(' ', stdout); }

void py23_print_end(void) { fputc('\n', stdout); }

void py23_panic(Py23Str* msg) {
    py23_raise_c("RuntimeError", msg->data);
}

void py23_panic_div0(void) {
    py23_raise_c("ZeroDivisionError", "integer division or modulo by zero");
}

void py23_panic_overflow(void) {
    py23_raise_c("OverflowError", "integer division result too large for int64");
}

/* ── 数学 ─────────────────────────────────────────────────────── */

int64_t py23_pow_i64(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}

/* abs(int)：回绕语义和整数加减一致。 */
int64_t py23_abs_i64(int64_t v) { return v < 0 ? -v : v; }

void py23_list_del_index(Py23List* l, int64_t i) {
    if (!l) py23_raise_c("TypeError", "del: expected a list");
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) py23_raise_c("IndexError", "list index out of range");
    int64_t v = l->data[i];
    if (l->elem_kind == 1 && v) py23_rc_dec((void*)v);
    memmove(&l->data[i], &l->data[i + 1],
            sizeof(int64_t) * (size_t)(l->len - i - 1));
    l->len--;
}

/* ── 复数 ─────────────────────────────────────────────────────── */

Py23Complex* py23_complex_new(double re, double im) {
    Py23Complex* c = (Py23Complex*)rc_alloc((int64_t)sizeof(Py23Complex), NULL, NULL,
                                            PY23_RC_OBJ);
    c->magic = PY23_COMPLEX_MAGIC;
    c->re = re;
    c->im = im;
    return c;
}

Py23Complex* py23_complex_add(Py23Complex* a, Py23Complex* b) {
    return py23_complex_new(a->re + b->re, a->im + b->im);
}

Py23Complex* py23_complex_sub(Py23Complex* a, Py23Complex* b) {
    return py23_complex_new(a->re - b->re, a->im - b->im);
}

Py23Complex* py23_complex_mul(Py23Complex* a, Py23Complex* b) {
    return py23_complex_new(a->re * b->re - a->im * b->im,
                            a->re * b->im + a->im * b->re);
}

Py23Complex* py23_complex_div(Py23Complex* a, Py23Complex* b) {
    double d = b->re * b->re + b->im * b->im;
    if (d == 0.0) py23_raise_c("ZeroDivisionError", "complex division by zero");
    return py23_complex_new((a->re * b->re + a->im * b->im) / d,
                            (a->im * b->re - a->re * b->im) / d);
}

Py23Complex* py23_complex_neg(Py23Complex* a) {
    return py23_complex_new(-a->re, -a->im);
}

double py23_complex_abs(Py23Complex* a) { return hypot(a->re, a->im); }

int64_t py23_complex_eq(Py23Complex* a, Py23Complex* b) {
    return a->re == b->re && a->im == b->im;
}

int64_t py23_complex_truth(Py23Complex* a) { return a->re != 0.0 || a->im != 0.0; }

static void complex_part(char* buf, size_t n, double v) {
    if (v == (double)(int64_t)v && v > -1e15 && v < 1e15)
        snprintf(buf, n, "%lld", (long long)v);
    else
        format_double(buf, n, v);
}

Py23Str* py23_complex_str(Py23Complex* a) {
    char re[64], im[64], buf[160];
    complex_part(re, sizeof re, a->re);
    complex_part(im, sizeof im, a->im);
    if (a->re == 0.0) {
        snprintf(buf, sizeof buf, "%sj", im);
    } else if (a->im < 0.0) {
        snprintf(buf, sizeof buf, "(%s%sj)", re, im);
    } else {
        snprintf(buf, sizeof buf, "(%s+%sj)", re, im);
    }
    return str_new(buf, (int64_t)strlen(buf));
}

/* ── f-string 格式化 ─────────────────────────────────────────── */

typedef struct {
    char fill;
    char align;   /* < > ^ = */
    char sign;    /* + - 空格 或 0 */
    char type;
    int width;
    int prec;
    int grouping;
} Py23Fmt;

static void fmt_parse(Py23Str* spec, Py23Fmt* f, int is_str) {
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
    if (i != n) py23_raise_c("ValueError", "Invalid format specifier");
}

static Py23Str* fmt_pad(const char* sign, const char* body, const Py23Fmt* f) {
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
        Py23Str* s = str_new(out, (int64_t)total);
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
    Py23Str* s = str_new(out, (int64_t)(q - out));
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

Py23Str* py23_fmt_i64(int64_t v, Py23Str* spec) {
    Py23Fmt f;
    fmt_parse(spec, &f, 0);
    if (f.type && f.type != 'd' && f.type != 'b' && f.type != 'o' && f.type != 'x' &&
        f.type != 'X' && f.type != 'n')
        py23_raise_c("ValueError", "Invalid format specifier for int");
    if (f.prec >= 0)
        py23_raise_c("ValueError", "Precision not allowed in integer format specifier");
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

Py23Str* py23_fmt_bool(int64_t v, Py23Str* spec) {
    Py23Fmt f;
    fmt_parse(spec, &f, 0);
    if (f.type && f.type != 's' && f.type != 'd' && f.type != 'b' && f.type != 'o' &&
        f.type != 'x' && f.type != 'X' && f.type != 'n')
        py23_raise_c("ValueError", "Invalid format specifier for bool");
    if (f.type == 'd' || f.type == 'b' || f.type == 'o' || f.type == 'x' ||
        f.type == 'X' || f.type == 'n')
        return py23_fmt_i64(v ? 1 : 0, spec);
    Py23Fmt sf = f;
    sf.align = f.align == '>' ? '<' : f.align;
    return fmt_pad(NULL, v ? "True" : "False", &sf);
}

Py23Str* py23_fmt_f64(double v, Py23Str* spec) {
    Py23Fmt f;
    fmt_parse(spec, &f, 0);
    char type = f.type;
    if (type && type != 'f' && type != 'F' && type != 'e' && type != 'E' && type != 'g' &&
        type != 'G' && type != '%')
        py23_raise_c("ValueError", "Invalid format specifier for float");
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

Py23Str* py23_fmt_str(Py23Str* v, Py23Str* spec) {
    Py23Fmt f;
    fmt_parse(spec, &f, 1);
    if (f.type && f.type != 's')
        py23_raise_c("ValueError", "Invalid format specifier for str");
    int64_t n = v ? v->len : 0;
    if (f.prec >= 0 && n > f.prec) n = f.prec;
    char* body = (char*)malloc((size_t)n + 1);
    if (!body) panic_c("out of memory");
    if (n > 0) memcpy(body, v->data, (size_t)n);
    body[n] = '\0';
    Py23Str* out = fmt_pad(NULL, body, &f);
    free(body);
    return out;
}

Py23Str* py23_fmt_complex(Py23Complex* v, Py23Str* spec) {
    Py23Fmt f;
    fmt_parse(spec, &f, 1);
    Py23Str* cs = py23_complex_str(v);
    char* body = (char*)malloc((size_t)cs->len + 1);
    if (!body) panic_c("out of memory");
    memcpy(body, cs->data, (size_t)cs->len + 1);
    Py23Str* out = fmt_pad(NULL, body, &f);
    free(body);
    py23_rc_dec(cs);
    return out;
}

Py23Str* py23_repr_str(Py23Str* v) {
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
    Py23Str* s = str_new(out, j);
    free(out);
    return s;
}

/* ── 文件对象 ─────────────────────────────────────────────────── */

struct Py23File {
    FILE* fp;
    int64_t closed;
};

static void file_destroy(void* p) {
    Py23File* f = (Py23File*)p;
    if (f->fp) {
        fclose(f->fp);
        f->fp = NULL;
    }
}

static char* file_cstr(Py23Str* s) {
    char* p = (char*)malloc((size_t)s->len + 1);
    if (!p) panic_c("out of memory");
    memcpy(p, s->data, (size_t)s->len);
    p[s->len] = 0;
    return p;
}

Py23File* py23_file_open(Py23Str* path, Py23Str* mode) {
    char* p = file_cstr(path);
    char* m = mode ? file_cstr(mode) : NULL;
    FILE* fp = fopen(p, m ? m : "r");
    free(p);
    free(m);
    if (!fp) py23_raise_c("OSError", "cannot open file");
    Py23File* f =
        (Py23File*)rc_alloc((int64_t)sizeof(Py23File), file_destroy, NULL, PY23_RC_OBJ);
    f->fp = fp;
    f->closed = 0;
    return f;
}

static FILE* file_check(Py23File* f) {
    if (!f || !f->fp || f->closed)
        py23_raise_c("ValueError", "I/O operation on closed file");
    return f->fp;
}

Py23Str* py23_file_read(Py23File* f, int64_t n) {
    FILE* fp = file_check(f);
    char* buf = NULL;
    int64_t len = 0, cap = 0;
    int64_t remain = n < 0 ? INT64_MAX : n;
    char chunk[4096];
    while (remain > 0) {
        size_t want = remain < (int64_t)sizeof chunk ? (size_t)remain : sizeof chunk;
        size_t got = fread(chunk, 1, want, fp);
        if (got == 0) break;
        if (len + (int64_t)got > cap) {
            int64_t ncap = cap ? cap : 8192;
            while (ncap < len + (int64_t)got) ncap *= 2;
            char* nb = (char*)realloc(buf, (size_t)ncap);
            if (!nb) panic_c("out of memory");
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, chunk, got);
        len += (int64_t)got;
        if (n >= 0) remain -= (int64_t)got;
        if (got < want) break;
    }
    Py23Str* s = str_new(buf ? buf : "", len);
    free(buf);
    return s;
}

Py23Str* py23_file_readline(Py23File* f) {
    FILE* fp = file_check(f);
    char* buf = NULL;
    int64_t len = 0, cap = 0;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 > cap) {
            int64_t ncap = cap ? cap * 2 : 128;
            char* nb = (char*)realloc(buf, (size_t)ncap);
            if (!nb) panic_c("out of memory");
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }
    Py23Str* s = str_new(buf ? buf : "", len);
    free(buf);
    return s;
}

Py23List* py23_file_readlines(Py23File* f) {
    Py23List* out = py23_list_new(4, 1);
    for (;;) {
        Py23Str* line = py23_file_readline(f);
        if (line->len == 0) break;
        py23_rc_inc(line);
        py23_list_append(out, (int64_t)line);
    }
    return out;
}

int64_t py23_file_write(Py23File* f, Py23Str* s) {
    FILE* fp = file_check(f);
    return (int64_t)fwrite(s->data, 1, (size_t)s->len, fp);
}

void py23_file_close(Py23File* f) {
    if (!f || !f->fp) return;
    fclose(f->fp);
    f->fp = NULL;
    f->closed = 1;
}

void py23_file_flush(Py23File* f) {
    FILE* fp = file_check(f);
    fflush(fp);
}

Py23Str* py23_input(Py23Str* prompt) {
    if (prompt && prompt->len > 0) {
        fwrite(prompt->data, 1, (size_t)prompt->len, stdout);
        fflush(stdout);
    }
    char* buf = NULL;
    int64_t len = 0, cap = 0;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (len + 1 > cap) {
            int64_t ncap = cap ? cap * 2 : 128;
            char* nb = (char*)realloc(buf, (size_t)ncap);
            if (!nb) panic_c("out of memory");
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len - 1] == '\r') --len;
    Py23Str* s = str_new(buf ? buf : "", len);
    free(buf);
    return s;
}
