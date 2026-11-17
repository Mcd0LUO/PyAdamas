// rt_json.c —— json 模块：按静态类型描述符序列化/解析
//
// 描述符语法（编译期由静态类型生成，运行时只做机械遍历）：
//   i int / b bool / f float / s str / n None
//   (l<d>) list； (d<d>) dict[str, ...]； (t<d><d>...) 定长 tuple
// 容器是自定界的（括号配对），tuple 子项靠 desc_len 逐个切分。
#include "rt_internal.h"

#include <errno.h>
#include <stdlib.h>

/* ── 描述符工具 ─────────────────────────────────────────────── */

static size_t desc_len(const char* d) {
    if (*d != '(') return 1;
    int depth = 0;
    size_t i = 0;
    do {
        if (d[i] == '(') depth++;
        else if (d[i] == ')') depth--;
        i++;
    } while (depth > 0 && d[i] != '\0');
    return i;
}

/* 类型描述符的首字符表示这个位置该解析成什么：
 *   i int / f float / b bool / n null / s str /
 *   ( 嵌套 tuple 或容器（以 ( 开头）
 *
 * **注意 `t` 也是堆类型**（tuple），原来漏了它：
 *
 *   desc_heap("(tis)") 里的第一个元素是 `t` —— tuple，是堆对象。
 *   旧实现只认 's' 和 '('，于是 `t` 被判成非堆，`all_heap` 变 0、
 *   `any_heap` 变 1，pa_list_new 拿到 elem_kind=2（混合类型）。
 *   而 exact-RC 模式下 kind=2 的元素释放走 pa_rc_dec_unknown，
 *   那里依赖 rc_index（索引已关）—— 结果那个 tuple 永远不释放。
 *
 * ASan 实测：53_json 的 json.loads("[1, \"x\"]", (0, "")) 泄漏 96 字节，
 * 79_datetime_pathlib 同类。补上 't' 之后 all_heap 正确为 1，
 * elem_kind=1（全部是堆），走 pa_rc_dec 正常释放。 */
static int desc_heap(const char* d, size_t n) {
    (void)n;
    return *d == 's' || *d == 't' || *d == '(';
}

/* ── 输出缓冲 ───────────────────────────────────────────────── */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    int64_t indent;     /* 0 = 紧凑输出 */
    int64_t sort_keys;  /* 非 0 时 dict 按 key 字符串排序 */
} JBuf;

static void jb_raw(JBuf* b, const char* p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (b->len + n + 1 > cap) cap *= 2;
        char* nb = (char*)realloc(b->buf, cap);
        if (!nb) panic_c("out of memory");
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void jb_ch(JBuf* b, char c) { jb_raw(b, &c, 1); }

static void jb_cstr(JBuf* b, const char* s) { jb_raw(b, s, strlen(s)); }

static void jb_u4(JBuf* b, uint32_t cp) {
    char tmp[8];
    /* cp 由调用方保证 <= 0xFFFF（4 位十六进制），格式串固定 6 字节；
       用 snprintf 返回值确认没截断，比让 -Wformat-truncation 去猜可靠。 */
    int wn = snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)cp);
    if (wn < 0 || wn >= (int)sizeof tmp) panic_c("json: unicode escape overflow");
    jb_raw(b, tmp, 6);
}

static void jb_utf8_cp(JBuf* b, uint32_t cp) {
    if (cp >= 0x10000) {
        uint32_t v = cp - 0x10000;
        jb_u4(b, 0xD800 + (v >> 10));
        jb_u4(b, 0xDC00 + (v & 0x3FF));
    } else {
        jb_u4(b, cp);
    }
}

/* ensure_ascii=True：非 ASCII 按 UTF-8 解码后用 \uXXXX（含代理对）。 */
static void jb_str(JBuf* b, PyAdamasStr* s) {
    jb_ch(b, '"');
    const unsigned char* p = (const unsigned char*)s->data;
    int64_t n = s->len;
    int64_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c == '"') {
            jb_raw(b, "\\\"", 2);
            i++;
        } else if (c == '\\') {
            jb_raw(b, "\\\\", 2);
            i++;
        } else if (c == '\n') {
            jb_cstr(b, "\\n");
            i++;
        } else if (c == '\r') {
            jb_cstr(b, "\\r");
            i++;
        } else if (c == '\t') {
            jb_cstr(b, "\\t");
            i++;
        } else if (c == '\b') {
            jb_cstr(b, "\\b");
            i++;
        } else if (c == '\f') {
            jb_cstr(b, "\\f");
            i++;
        } else if (c < 0x20) {
            jb_u4(b, c);
            i++;
        } else if (c < 0x80) {
            jb_ch(b, (char)c);
            i++;
        } else {
            uint32_t cp = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                extra = 1;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                extra = 2;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                extra = 3;
            } else {
                jb_u4(b, c);
                i++;
                continue;
            }
            if (i + extra >= n) {
                jb_u4(b, c);
                i++;
                continue;
            }
            int ok = 1;
            for (int k = 1; k <= extra; ++k) {
                unsigned char cc = p[i + k];
                if ((cc & 0xC0) != 0x80) {
                    ok = 0;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok) {
                jb_u4(b, c);
                i++;
                continue;
            }
            jb_utf8_cp(b, cp);
            i += extra + 1;
        }
    }
    jb_ch(b, '"');
}

static void jb_f64(JBuf* b, double v) {
    if (v != v) {
        jb_cstr(b, "NaN");
        return;
    }
    if (v > 1.7976931348623157e308) {
        jb_cstr(b, "Infinity");
        return;
    }
    if (v < -1.7976931348623157e308) {
        jb_cstr(b, "-Infinity");
        return;
    }
    char buf[64];
    format_double(buf, sizeof buf, v);
    jb_cstr(b, buf);
}

/* 换行 + 缩进；indent <= 0 时是紧凑模式，什么都不输出。 */
static void jb_nl(JBuf* b, int64_t depth) {
    if (b->indent <= 0) return;
    jb_ch(b, '\n');
    for (int64_t i = 0; i < b->indent * depth; ++i) jb_ch(b, ' ');
}

/* sort_keys 用：按 key 字符串给 entry 下标排序（单线程，用静态指针传 dict）。 */
static PyAdamasDict* g_json_sort_dict;

static int jb_cmp_order(const void* pa, const void* pb) {
    int64_t ia = *(const int64_t*)pa;
    int64_t ib = *(const int64_t*)pb;
    PyAdamasStr* x = (PyAdamasStr*)pa_dict_key_at(g_json_sort_dict, ia);
    PyAdamasStr* y = (PyAdamasStr*)pa_dict_key_at(g_json_sort_dict, ib);
    int64_t c = pa_str_cmp(x, y);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

static void jb_value(JBuf* b, int64_t v, const char* d, size_t n, int64_t depth) {
    char t = d[0];
    if (t == '(') {
        char k = d[1];
        if (k == 'l') {
            PyAdamasList* l = (PyAdamasList*)v;
            const char* cd = d + 2;
            size_t cn = n - 3;
            jb_ch(b, '[');
            for (int64_t i = 0; i < l->len; ++i) {
                if (i) jb_ch(b, ',');
                jb_nl(b, depth + 1);
                if (b->indent <= 0 && i) jb_ch(b, ' ');
                jb_value(b, l->data[i], cd, cn, depth + 1);
            }
            if (l->len) jb_nl(b, depth);
            jb_ch(b, ']');
            return;
        }
        if (k == 'd') {
            PyAdamasDict* m = (PyAdamasDict*)v;
            const char* cd = d + 2;
            size_t cn = n - 3;
            int64_t* order = NULL;
            if (b->sort_keys && m->len > 1) {
                order = (int64_t*)malloc(sizeof(int64_t) * (size_t)m->len);
                if (!order) panic_c("out of memory");
                for (int64_t i = 0; i < m->len; ++i) order[i] = i;
                g_json_sort_dict = m;
                qsort(order, (size_t)m->len, sizeof(int64_t), jb_cmp_order);
            }
            jb_ch(b, '{');
            for (int64_t i = 0; i < m->len; ++i) {
                int64_t e = order ? order[i] : i;
                if (i) jb_ch(b, ',');
                jb_nl(b, depth + 1);
                if (b->indent <= 0 && i) jb_ch(b, ' ');
                jb_str(b, (PyAdamasStr*)pa_dict_key_at(m, e));
                jb_cstr(b, ": ");
                jb_value(b, pa_dict_val_at(m, e), cd, cn, depth + 1);
            }
            if (m->len) jb_nl(b, depth);
            jb_ch(b, '}');
            free(order);
            return;
        }
        if (k == 't') {
            PyAdamasList* l = (PyAdamasList*)v;
            size_t pos = 2;
            int64_t idx = 0;
            jb_ch(b, '[');
            while (pos + 1 < n) {
                size_t cl = desc_len(d + pos);
                if (idx) jb_ch(b, ',');
                jb_nl(b, depth + 1);
                if (b->indent <= 0 && idx) jb_ch(b, ' ');
                jb_value(b, l->data[idx], d + pos, cl, depth + 1);
                pos += cl;
                idx++;
            }
            if (idx) jb_nl(b, depth);
            jb_ch(b, ']');
            return;
        }
        pa_raise_c("ValueError", "json: bad type descriptor");
    }
    if (t == 'i') {
        char buf[24];
        int len = snprintf(buf, sizeof buf, "%lld", (long long)v);
        jb_raw(b, buf, (size_t)len);
        return;
    }
    if (t == 'b') {
        jb_cstr(b, v ? "true" : "false");
        return;
    }
    if (t == 'f') {
        union {
            int64_t i;
            double d;
        } u;
        u.i = v;
        jb_f64(b, u.d);
        return;
    }
    if (t == 's') {
        jb_str(b, (PyAdamasStr*)v);
        return;
    }
    if (t == 'n') {
        jb_cstr(b, "null");
        return;
    }
    pa_raise_c("ValueError", "json: bad type descriptor");
}

PyAdamasStr* pa_json_dumps(int64_t v, PyAdamasStr* desc, int64_t indent, int64_t sort_keys) {

    JBuf b = {NULL, 0, 0, indent, sort_keys};
    jb_value(&b, v, desc->data, (size_t)desc->len, 0);
    PyAdamasStr* out = str_new(b.buf ? b.buf : "", (int64_t)b.len);
    free(b.buf);

    return out;
}

/* ── 解析 ───────────────────────────────────────────────────── */

typedef struct {
    const char* p;
    const char* end;
    const char* start;
} JP;

static _Noreturn void jp_fail(JP* s, const char* what) {
    char msg[256];
    snprintf(msg, sizeof msg, "json: %s at offset %lld", what,
             (long long)(s->p - s->start));
    pa_raise_c("ValueError", msg);
}

static void jp_ws(JP* s) {
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

static void jp_expect(JP* s, char c) {
    jp_ws(s);
    if (s->p >= s->end || *s->p != c) {
        char what[32];
        snprintf(what, sizeof what, "expected '%c'", c);
        jp_fail(s, what);
    }
    s->p++;
}

static int jp_hex4(JP* s) {
    if (s->end - s->p < 4) jp_fail(s, "truncated \\u escape");
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = s->p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else jp_fail(s, "invalid \\u escape");
        v = v * 16 + d;
    }
    s->p += 4;
    return v;
}

static void jp_utf8_put(JBuf* b, uint32_t cp) {
    if (cp < 0x80) {
        jb_ch(b, (char)cp);
    } else if (cp < 0x800) {
        jb_ch(b, (char)(0xC0 | (cp >> 6)));
        jb_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        jb_ch(b, (char)(0xE0 | (cp >> 12)));
        jb_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        jb_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        jb_ch(b, (char)(0xF0 | (cp >> 18)));
        jb_ch(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        jb_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        jb_ch(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static PyAdamasStr* jp_string(JP* s) {
    jp_expect(s, '"');
    JBuf b = {NULL, 0, 0, 0, 0};
    while (s->p < s->end) {
        unsigned char c = (unsigned char)*s->p++;
        if (c == '"') {
            PyAdamasStr* r = str_new(b.buf ? b.buf : "", (int64_t)b.len);
            free(b.buf);
            return r;
        }
        if (c == '\\') {
            if (s->p >= s->end) jp_fail(s, "unterminated escape");
            char e = *s->p++;
            switch (e) {
                case '"': jb_ch(&b, '"'); break;
                case '\\': jb_ch(&b, '\\'); break;
                case '/': jb_ch(&b, '/'); break;
                case 'b': jb_ch(&b, '\b'); break;
                case 'f': jb_ch(&b, '\f'); break;
                case 'n': jb_ch(&b, '\n'); break;
                case 'r': jb_ch(&b, '\r'); break;
                case 't': jb_ch(&b, '\t'); break;
                case 'u': {
                    uint32_t cp = (uint32_t)jp_hex4(s);
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                            uint32_t lo;
                            s->p += 2;
                            lo = (uint32_t)jp_hex4(s);
                            if (lo < 0xDC00 || lo > 0xDFFF)
                                jp_fail(s, "invalid surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            jp_fail(s, "unpaired surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        jp_fail(s, "unpaired surrogate");
                    }
                    jp_utf8_put(&b, cp);
                    break;
                }
                default: jp_fail(s, "invalid escape");
            }
            continue;
        }
        if (c < 0x20) jp_fail(s, "control character in string");
        jb_ch(&b, (char)c);
    }
    jp_fail(s, "unterminated string");
}

static void jp_number_token(JP* s, char* buf, size_t cap) {
    const char* start = s->p;
    if (s->p < s->end && (*s->p == '-' || *s->p == '+')) s->p++;
    while (s->p < s->end) {
        char c = *s->p;
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
            c == '-')
            s->p++;
        else
            break;
    }
    size_t n = (size_t)(s->p - start);
    if (n == 0 || n >= cap) jp_fail(s, "invalid number");
    memcpy(buf, start, n);
    buf[n] = '\0';
}

static int64_t jp_int(JP* s) {
    char buf[64];
    jp_number_token(s, buf, sizeof buf);
    for (char* q = buf; *q; ++q)
        if (*q == '.' || *q == 'e' || *q == 'E') jp_fail(s, "expected integer");
    errno = 0;
    char* endp = NULL;
    long long v = strtoll(buf, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0') jp_fail(s, "invalid integer");
    return (int64_t)v;
}

static double jp_float(JP* s) {
    jp_ws(s);
    if (s->end - s->p >= 3 && strncmp(s->p, "NaN", 3) == 0) {
        s->p += 3;
        return (double)NAN;
    }
    if (s->end - s->p >= 8 && strncmp(s->p, "Infinity", 8) == 0) {
        s->p += 8;
        return (double)INFINITY;
    }
    if (s->end - s->p >= 9 && strncmp(s->p, "-Infinity", 9) == 0) {
        s->p += 9;
        return -(double)INFINITY;
    }
    char buf[64];
    jp_number_token(s, buf, sizeof buf);
    errno = 0;
    char* endp = NULL;
    double v = strtod(buf, &endp);
    if (!endp || *endp != '\0') jp_fail(s, "invalid number");
    return v;
}

static int64_t jp_bool(JP* s) {
    jp_ws(s);
    if (s->end - s->p >= 4 && strncmp(s->p, "true", 4) == 0) {
        s->p += 4;
        return 1;
    }
    if (s->end - s->p >= 5 && strncmp(s->p, "false", 5) == 0) {
        s->p += 5;
        return 0;
    }
    jp_fail(s, "expected true/false");
}

static int64_t jp_value(JP* s, const char* d, size_t n) {
    jp_ws(s);
    char t = d[0];
    if (t == '(') {
        char k = d[1];
        if (k == 'l') {
            const char* cd = d + 2;
            size_t cn = n - 3;
            int kind = desc_heap(cd, cn) ? 1 : 0;
            PyAdamasList* out = pa_list_new(0, kind);
            // **RAII：pin 跟着作用域走，不再手工配对。**
            //
            // 原来要手工保证「每条 return 路径都 unpin + temp_take」——
            // 这个函数有 11 个 return，靠人工保证容易漏。现在：
            //   - pa_rc_temp 挂临时栈（管异常路径）；
            //   - PA_PIN_SCOPE 挂作用域守卫（管正常路径的任意出口）；
            //   - 交出所有权时 PA_PIN_RELEASE + pa_rc_temp_take。
            pa_rc_temp(out);
            PA_PIN_SCOPE(out);
            jp_expect(s, '[');
            jp_ws(s);
            if (s->p < s->end && *s->p == ']') {
                s->p++;
                PA_PIN_RELEASE(out);
                pa_rc_temp_take(out);
                return (int64_t)out;
            }
            for (;;) {
                int64_t v = jp_value(s, cd, cn);
                pa_list_append(out, v, 2);
                jp_ws(s);
                if (s->p < s->end && *s->p == ',') {
                    s->p++;
                    continue;
                }
                break;
            }
            jp_expect(s, ']');
            PA_PIN_RELEASE(out);
            pa_rc_temp_take(out);
            return (int64_t)out;
        }
        if (k == 'd') {
            const char* cd = d + 2;
            size_t cn = n - 3;
            int vown = desc_heap(cd, cn) ? 1 : 0;
            PyAdamasDict* out = pa_dict_new(1, 1, vown, -1, -1);
            // RAII：字典本体和每个 key 都由作用域守卫管理，不再手工配对。
            // key 在循环体内声明，每轮离开作用域自动 unpin —— 比手工在
            // 三条路径上各写一次 pa_gc_unpin(key) 可靠。
            pa_rc_temp(out);
            PA_PIN_SCOPE(out);
            jp_expect(s, '{');
            jp_ws(s);
            if (s->p < s->end && *s->p == '}') {
                s->p++;
                PA_PIN_RELEASE(out);
                pa_rc_temp_take(out);
                return (int64_t)out;
            }
            for (;;) {
                PyAdamasStr* key = jp_string(s);
                PA_PIN_SCOPE(key);
                jp_expect(s, ':');
                int64_t v = jp_value(s, cd, cn);
                pa_dict_set(out, (int64_t)key, v);
                jp_ws(s);
                if (s->p < s->end && *s->p == ',') {
                    s->p++;
                    continue;
                }
                break;
            }
            jp_expect(s, '}');
            PA_PIN_RELEASE(out);
            pa_rc_temp_take(out);
            return (int64_t)out;
        }
        if (k == 't') {
            size_t pos = 2;
            int64_t count = 0;
            int any_heap = 0;
            int all_heap = 1;
            while (pos + 1 < n) {
                size_t cl = desc_len(d + pos);
                count++;
                if (desc_heap(d + pos, cl))
                    any_heap = 1;
                else
                    all_heap = 0;
                pos += cl;
            }
            int kind = all_heap ? 1 : (any_heap ? 2 : 0);
            PyAdamasList* out = pa_list_new(count, kind);
            // **和 list/dict 分支一样入临时栈。**
            //
            // 下面 jp_expect / jp_fail（长度不匹配、缺 ']'）都可能抛异常，
            // 异常展开跳过 return，out 只有局部变量持有 —— 永久泄漏。
            // 之前 list/dict 分支都加了保护，唯独 tuple 漏了。
            pa_rc_temp(out);
            PA_PIN_SCOPE(out);
            jp_expect(s, '[');
            pos = 2;
            int64_t idx = 0;
            jp_ws(s);
            if (s->p < s->end && *s->p == ']') {
                if (count != 0) jp_fail(s, "tuple length mismatch");
                s->p++;
                PA_PIN_RELEASE(out);
                pa_rc_temp_take(out);
                return (int64_t)out;
            }
            for (;;) {
                if (idx >= count) jp_fail(s, "tuple length mismatch");
                {
                    size_t cl = desc_len(d + pos);
                    int64_t v = jp_value(s, d + pos, cl);
                    pa_list_append(out, v, 2);
                    pos += cl;
                }
                idx++;
                jp_ws(s);
                if (s->p < s->end && *s->p == ',') {
                    s->p++;
                    continue;
                }
                break;
            }
            jp_expect(s, ']');
            if (idx != count) jp_fail(s, "tuple length mismatch");
            PA_PIN_RELEASE(out);
            pa_rc_temp_take(out);
            return (int64_t)out;
        }
        pa_raise_c("ValueError", "json: bad type descriptor");
    }
    if (t == 's') return (int64_t)jp_string(s);
    if (t == 'i') return jp_int(s);
    if (t == 'f') {
        union {
            int64_t i;
            double d;
        } u;
        u.d = jp_float(s);
        return u.i;
    }
    if (t == 'b') return jp_bool(s);
    if (t == 'n') {
        jp_ws(s);
        if (s->end - s->p >= 4 && strncmp(s->p, "null", 4) == 0) {
            s->p += 4;
            return 0;
        }
        jp_fail(s, "expected null");
    }
    pa_raise_c("ValueError", "json: bad type descriptor");
}

int64_t pa_json_loads(PyAdamasStr* text, PyAdamasStr* desc) {
    JP s = {text->data, text->data + text->len, text->data};
    int64_t v = jp_value(&s, desc->data, (size_t)desc->len);
    jp_ws(&s);
    if (s.p != s.end) jp_fail(&s, "extra data");
    return v;
}
