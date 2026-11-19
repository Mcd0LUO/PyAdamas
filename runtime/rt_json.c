// rt_json.c —— json 模块：按静态类型描述符序列化/解析
//
// 描述符语法（编译期由静态类型生成，运行时只做机械遍历）：
//   i int / b bool / f float / s str / n None
//   (l<d>) list； (d<k><v>) dict（k 是键类型：s/i/f/b/n）； (t<d><d>...) 定长 tuple
// 容器是自定界的（括号配对），tuple 子项靠 desc_len 逐个切分。
//
// 0.314 健壮化：dict 键按 CPython json 规则强制转 str（int→十进制、
// float→repr 含 NaN/Infinity、bool→true/false、None→null）；dumps 支持
// ensure_ascii/separators/allow_nan；indent=0/负数按 CPython 输出换行
// （INT64_MIN 哨兵表示 None=紧凑）；loads 数字按严格 JSON 文法扫描
// （拒绝 +1/01/1./.5/十六进制/小写 inf/nan），语法错误消息对齐 CPython
// JSONDecodeError 的 "what: line L column C (char N)" 格式。
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
 * **注意 t 也是堆类型**（tuple），原来漏了它：
 *
 *   desc_heap("(tis)") 里的第一个元素是 t —— tuple，是堆对象。
 *   旧实现只认 's' 和 '('，于是 t 被判成非堆，all_heap 变 0、
 *   any_heap 变 1，pa_list_new 拿到 elem_kind=2（混合类型）。
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

/* indent 哨兵：INT64_MIN = CPython 的 indent=None（紧凑输出）。
 * 0/负数不是紧凑——CPython indent=0 输出换行但不缩进。 */
#define J_INDENT_NONE INT64_MIN

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    int64_t indent;
    int64_t sort_keys;
    int ensure_ascii;
    int allow_nan;
    const char* item_sep;
    size_t item_sep_len;
    const char* key_sep;
    size_t key_sep_len;
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

/* ensure_ascii=True（默认）：非 ASCII 按 UTF-8 解码后用 uXXXX 转义
 * （含代理对），DEL(0x7f) 也要转义（CPython ESCAPE_ASCII 是 [^\ -~]，
 * 0x7f 在范围外）。ensure_ascii=False：>=0x80 的字节原样输出
 * （控制字符/引号/反斜杠仍转义）。 */
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
            if (c == 0x7f && b->ensure_ascii) jb_u4(b, 0x7f);
            else jb_ch(b, (char)c);
            i++;
        } else if (!b->ensure_ascii) {
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
        if (!b->allow_nan)
            pa_raise_c("ValueError",
                       "Out of range float values are not JSON compliant: nan");
        jb_cstr(b, "NaN");
        return;
    }
    if (v > 1.7976931348623157e308) {
        if (!b->allow_nan)
            pa_raise_c("ValueError",
                       "Out of range float values are not JSON compliant: inf");
        jb_cstr(b, "Infinity");
        return;
    }
    if (v < -1.7976931348623157e308) {
        if (!b->allow_nan)
            pa_raise_c("ValueError",
                       "Out of range float values are not JSON compliant: -inf");
        jb_cstr(b, "-Infinity");
        return;
    }
    char buf[64];
    format_double(buf, sizeof buf, v);
    jb_cstr(b, buf);
}

/* 换行 + 缩进；indent == INT64_MIN（None）时是紧凑模式，什么都不输出。
 * indent=0/负数：只换行不缩进（CPython ' ' * 负数 == ''）。 */
static void jb_nl(JBuf* b, int64_t depth) {
    if (b->indent == J_INDENT_NONE) return;
    jb_ch(b, '\n');
    int64_t n = b->indent > 0 ? b->indent * depth : 0;
    for (int64_t i = 0; i < n; ++i) jb_ch(b, ' ');
}

/* sort_keys 用：按 key 给 entry 下标排序（单线程，用静态指针传 dict）。
 * CPython 在键转 str **之前**按原键排序：int/float/bool 按数值，
 * str 按字典序。混合类型键 CPython 会 TypeError，pyadamas 静态类型
 * 保证键同型，不会遇到。float NaN 键的排序 CPython 本身未定义，这里
 * 视为相等（保持相对位置由 qsort 决定）。 */
static PyAdamasDict* g_json_sort_dict;
static char g_json_sort_kk;

static int jb_cmp_order(const void* pa, const void* pb) {
    int64_t ia = *(const int64_t*)pa;
    int64_t ib = *(const int64_t*)pb;
    int64_t ka = pa_dict_key_at(g_json_sort_dict, ia);
    int64_t kb = pa_dict_key_at(g_json_sort_dict, ib);
    if (g_json_sort_kk == 's') {
        int64_t c = pa_str_cmp((PyAdamasStr*)ka, (PyAdamasStr*)kb);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (g_json_sort_kk == 'f') {
        union { int64_t i; double d; } ua, ub;
        ua.i = ka;
        ub.i = kb;
        return ua.d < ub.d ? -1 : ua.d > ub.d ? 1 : 0;
    }
    if (g_json_sort_kk == 'n') return 0;
    /* i / b：裸 int64 比较 */
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

/* 非 str 键的强制转换（CPython json.encoder 的键规则）：
 *   int → 十进制 repr；float → floatstr（repr，NaN/Infinity/-Infinity）；
 *   True/False → "true"/"false"；None → "null"。
 * 输出不带引号，调用方补。 */
static void jb_key_scalar(JBuf* b, int64_t kv, char kk) {
    if (kk == 'i') {
        char buf[24];
        int len = snprintf(buf, sizeof buf, "%lld", (long long)kv);
        jb_raw(b, buf, (size_t)len);
        return;
    }
    if (kk == 'b') {
        jb_cstr(b, kv ? "true" : "false");
        return;
    }
    if (kk == 'n') {
        jb_cstr(b, "null");
        return;
    }
    if (kk == 'f') {
        union { int64_t i; double d; } u;
        u.i = kv;
        jb_f64(b, u.d);
        return;
    }
    pa_raise_c("ValueError", "json: bad type descriptor");
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
                if (i) jb_raw(b, b->item_sep, b->item_sep_len);
                jb_nl(b, depth + 1);
                jb_value(b, l->data[i], cd, cn, depth + 1);
            }
            if (l->len) jb_nl(b, depth);
            jb_ch(b, ']');
            return;
        }
        if (k == 'd') {
            PyAdamasDict* m = (PyAdamasDict*)v;
            char kk = d[2];
            const char* cd = d + 3;
            size_t cn = n - 4;
            int64_t* order = NULL;
            if (b->sort_keys && m->len > 1) {
                order = (int64_t*)malloc(sizeof(int64_t) * (size_t)m->len);
                if (!order) panic_c("out of memory");
                for (int64_t i = 0; i < m->len; ++i) order[i] = i;
                g_json_sort_dict = m;
                g_json_sort_kk = kk;
                qsort(order, (size_t)m->len, sizeof(int64_t), jb_cmp_order);
            }
            jb_ch(b, '{');
            for (int64_t i = 0; i < m->len; ++i) {
                int64_t e = order ? order[i] : i;
                if (i) jb_raw(b, b->item_sep, b->item_sep_len);
                jb_nl(b, depth + 1);
                int64_t kv = pa_dict_key_at(m, e);
                if (kk == 's') {
                    jb_str(b, (PyAdamasStr*)kv);
                } else {
                    jb_ch(b, '"');
                    jb_key_scalar(b, kv, kk);
                    jb_ch(b, '"');
                }
                jb_raw(b, b->key_sep, b->key_sep_len);
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
                if (idx) jb_raw(b, b->item_sep, b->item_sep_len);
                jb_nl(b, depth + 1);
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

/* flags: bit0 sort_keys / bit1 ensure_ascii / bit2 allow_nan。
 * seps: NULL = 默认分隔符（紧凑 ", "/": "，缩进 ","/": "）；
 *       否则是 tuple[str, str]（运行期 PyAdamasList，借用不 retain）。 */
PyAdamasStr* pa_json_dumps(int64_t v, PyAdamasStr* desc, int64_t indent,
                           int64_t flags, PyAdamasList* seps) {
    JBuf b = {0};
    b.indent = indent;
    b.sort_keys = (flags & 1) != 0;
    b.ensure_ascii = (flags & 2) != 0;
    b.allow_nan = (flags & 4) != 0;
    if (seps) {
        if (seps->len < 2) {
            char msg[96];
            snprintf(msg, sizeof msg,
                     "not enough values to unpack (expected 2, got %lld)",
                     (long long)seps->len);
            pa_raise_c("ValueError", msg);
        }
        if (seps->len > 2) {
            char msg[96];
            snprintf(msg, sizeof msg,
                     "too many values to unpack (expected 2, got %lld)",
                     (long long)seps->len);
            pa_raise_c("ValueError", msg);
        }
        PyAdamasStr* it = (PyAdamasStr*)seps->data[0];
        PyAdamasStr* ks = (PyAdamasStr*)seps->data[1];
        b.item_sep = it->data;
        b.item_sep_len = (size_t)it->len;
        b.key_sep = ks->data;
        b.key_sep_len = (size_t)ks->len;
    } else {
        b.item_sep = indent == J_INDENT_NONE ? ", " : ",";
        b.item_sep_len = indent == J_INDENT_NONE ? 2 : 1;
        b.key_sep = ": ";
        b.key_sep_len = 2;
    }
    if (indent != J_INDENT_NONE && indent > 65536)
        pa_raise_c("ValueError", "json: indent too large");
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

/* 语法错误：对齐 CPython JSONDecodeError 的消息与位置
 * （"what: line L column C (char N)"，N 是 0 基字节偏移）。
 * pyadamas 没有 JSONDecodeError 内置名，按桥的映射口径抛 ValueError。 */
static _Noreturn void jp_fail_at(JP* s, const char* pos, const char* what) {
    long long idx = (long long)(pos - s->start);
    int line = 1;
    long long lastnl = -1;
    for (const char* q = s->start; q < pos; ++q) {
        if (*q == '\n') {
            ++line;
            lastnl = (long long)(q - s->start);
        }
    }
    long long col = idx - lastnl;
    char msg[320];
    snprintf(msg, sizeof msg, "%s: line %d column %lld (char %lld)", what, line,
             col, idx);
    pa_raise_c("ValueError", msg);
}

/* 类型导向的失配（样例类型 vs 文本类型）：CPython 无对应错误（它返回
 * 异构值），属 pyadamas 静态样例口径，保留 "json: ... at offset" 格式。 */
static _Noreturn void jp_fail_type(JP* s, const char* what) {
    char msg[256];
    snprintf(msg, sizeof msg, "json: %s at offset %lld", what,
             (long long)(s->p - s->start));
    pa_raise_c("ValueError", msg);
}

/* 除数字/bool 外的合法值起始字符——用于区分「类型失配」与「语法错误」。 */
static int jp_other_value_start(char c) {
    return c == '"' || (c >= '0' && c <= '9') || c == '-' || c == '[' ||
           c == '{' || c == 'n';
}

/* 数字扫描失败时的分类：'-' 开头但扫不出数字是语法错误（CPython
 * "Expecting value"），不是「别的类型」。'n' 只有真构成 "null" 才算
 * 异类值——"nan"/"nonsense" 是语法错误（CPython 同口径）。 */
static int jp_nonnum_value_start(JP* s) {
    if (s->p >= s->end) return 0;
    char c = *s->p;
    if (c == 'n')
        return s->end - s->p >= 4 && strncmp(s->p, "null", 4) == 0;
    return c == '"' || (c >= '0' && c <= '9') || c == '[' || c == '{';
}

static void jp_ws(JP* s) {
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

/* 容器开括号：匹配则消费；文本是别的合法值 → 类型失配；否则语法错误。 */
static void jp_open(JP* s, char c, const char* typed_what) {
    jp_ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return;
    }
    if (s->p < s->end && jp_other_value_start(*s->p)) jp_fail_type(s, typed_what);
    jp_fail_at(s, s->p, "Expecting value");
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

/* key_ctx：dict 键位置——期望双引号的失败消息是
 * "Expecting property name enclosed in double quotes"（CPython 口径）。 */
static PyAdamasStr* jp_string(JP* s, int key_ctx) {
    jp_ws(s);
    const char* strstart = s->p;
    if (s->p >= s->end || *s->p != '"') {
        if (key_ctx)
            jp_fail_at(s, s->p,
                       "Expecting property name enclosed in double quotes");
        if (s->p < s->end && jp_other_value_start(*s->p))
            jp_fail_type(s, "expected string");
        jp_fail_at(s, s->p, "Expecting value");
    }
    s->p++;
    JBuf b = {0};
    while (s->p < s->end) {
        unsigned char c = (unsigned char)*s->p++;
        if (c == '"') {
            PyAdamasStr* r = str_new(b.buf ? b.buf : "", (int64_t)b.len);
            free(b.buf);
            return r;
        }
        if (c == '\\') {
            if (s->p >= s->end)
                jp_fail_at(s, strstart, "Unterminated string starting at");
            const char* escpos = s->p - 1;
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
                    const char* upos = s->p - 1;
                    if (s->end - s->p < 4)
                        jp_fail_at(s, upos, "Invalid \\uXXXX escape");
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s->p[i];
                        int d;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        else jp_fail_at(s, upos, "Invalid \\uXXXX escape");
                        cp = cp * 16 + (uint32_t)d;
                    }
                    s->p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                            s->p += 2;
                            if (s->end - s->p < 4)
                                jp_fail_at(s, upos, "Invalid \\uXXXX escape");
                            uint32_t lo = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = s->p[i];
                                int d;
                                if (h >= '0' && h <= '9') d = h - '0';
                                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                                else jp_fail_at(s, upos, "Invalid \\uXXXX escape");
                                lo = lo * 16 + (uint32_t)d;
                            }
                            s->p += 4;
                            if (lo < 0xDC00 || lo > 0xDFFF)
                                jp_fail_type(s, "invalid surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            /* CPython 接受孤立代理（返回含代理码点的 str）；
                             * pyadamas str 是 UTF-8 字节串，无法表示，拒绝。 */
                            jp_fail_type(s, "unpaired surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        jp_fail_type(s, "unpaired surrogate");
                    }
                    jp_utf8_put(&b, cp);
                    break;
                }
                default: jp_fail_at(s, escpos, "Invalid \\escape");
            }
            continue;
        }
        if (c < 0x20) jp_fail_at(s, s->p - 1, "Invalid control character at");
        jb_ch(&b, (char)c);
    }
    jp_fail_at(s, strstart, "Unterminated string starting at");
}

/* 严格 JSON 数字文法的最长前缀扫描：
 *   -? (0 | [1-9][0-9]*) ('.' [0-9]+)? ([eE] [+-]? [0-9]+)?
 * 返回前缀长度（0 = 不构成数字），*isfloat 标记有无小数/指数部分。
 * 不消费非法尾缀："1." 只扫出 "1"，剩下的 '.' 由外层报 Extra data /
 * Expecting ',' delimiter —— 与 CPython 扫描器的接受集逐一对齐。 */
static size_t jp_num_scan(JP* s, int* isfloat) {
    const char* p = s->p;
    const char* end = s->end;
    const char* start = p;
    *isfloat = 0;
    if (p < end && *p == '-') p++;
    if (p >= end) return 0;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < end && *p >= '0' && *p <= '9') p++;
    } else {
        return 0;
    }
    if (p < end && *p == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
        *isfloat = 1;
        p += 2;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q < end && (*q == '+' || *q == '-')) q++;
        if (q < end && *q >= '0' && *q <= '9') {
            *isfloat = 1;
            p = q + 1;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
    }
    return (size_t)(p - start);
}

static int64_t jp_int(JP* s) {
    jp_ws(s);
    const char* start = s->p;
    int isfloat = 0;
    size_t n = jp_num_scan(s, &isfloat);
    if (n == 0) {
        if (jp_nonnum_value_start(s))
            jp_fail_type(s, "expected integer");
        jp_fail_at(s, s->p, "Expecting value");
    }
    if (isfloat) jp_fail_type(s, "expected integer");
    /* int64 最多 19 位数字 + 符号；超长必然溢出（CPython 返回大整数，
     * pyadamas 是 int64 语言，按口径报 ValueError 而不是回绕）。 */
    if (n >= 21) jp_fail_type(s, "integer out of int64 range");
    char buf[24];
    memcpy(buf, start, n);
    buf[n] = '\0';
    errno = 0;
    char* endp = NULL;
    long long v = strtoll(buf, &endp, 10);
    if (errno == ERANGE || !endp || *endp != '\0')
        jp_fail_type(s, "integer out of int64 range");
    s->p = start + n;
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
    const char* start = s->p;
    int isfloat = 0;
    size_t n = jp_num_scan(s, &isfloat);
    if (n == 0) {
        if (jp_nonnum_value_start(s))
            jp_fail_type(s, "expected float");
        jp_fail_at(s, s->p, "Expecting value");
    }
    /* 超长数字串（如 400 位整数）CPython float() 给 inf/0，不报错；
     * 栈缓冲不够时临时堆分配（冷路径）。 */
    char buf[64];
    char* big = NULL;
    char* tok = buf;
    if (n >= sizeof buf) {
        big = (char*)malloc(n + 1);
        if (!big) panic_c("out of memory");
        tok = big;
    }
    memcpy(tok, start, n);
    tok[n] = '\0';
    double v = strtod(tok, NULL);
    free(big);
    s->p = start + n;
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
    if (s->p < s->end && jp_other_value_start(*s->p))
        jp_fail_type(s, "expected bool");
    jp_fail_at(s, s->p, "Expecting value");
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
            jp_open(s, '[', "expected list");
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
                    const char* comma = s->p;
                    s->p++;
                    jp_ws(s);
                    if (s->p < s->end && *s->p == ']')
                        jp_fail_at(s, comma,
                                   "Illegal trailing comma before end of array");
                    continue;
                }
                break;
            }
            if (s->p >= s->end || *s->p != ']')
                jp_fail_at(s, s->p, "Expecting ',' delimiter");
            s->p++;
            PA_PIN_RELEASE(out);
            pa_rc_temp_take(out);
            return (int64_t)out;
        }
        if (k == 'd') {
            if (d[2] != 's') pa_raise_c("ValueError", "json: bad type descriptor");
            const char* cd = d + 3;
            size_t cn = n - 4;
            int vown = desc_heap(cd, cn) ? 1 : 0;
            PyAdamasDict* out = pa_dict_new(1, 1, vown, -1, -1);
            // RAII：字典本体和每个 key 都由作用域守卫管理，不再手工配对。
            // key 在循环体内声明，每轮离开作用域自动 unpin —— 比手工在
            // 三条路径上各写一次 pa_gc_unpin(key) 可靠。
            pa_rc_temp(out);
            PA_PIN_SCOPE(out);
            jp_open(s, '{', "expected dict");
            jp_ws(s);
            if (s->p < s->end && *s->p == '}') {
                s->p++;
                PA_PIN_RELEASE(out);
                pa_rc_temp_take(out);
                return (int64_t)out;
            }
            for (;;) {
                PyAdamasStr* key = jp_string(s, 1);
                PA_PIN_SCOPE(key);
                jp_ws(s);
                if (s->p >= s->end || *s->p != ':')
                    jp_fail_at(s, s->p, "Expecting ':' delimiter");
                s->p++;
                int64_t v = jp_value(s, cd, cn);
                pa_dict_set(out, (int64_t)key, v);
                jp_ws(s);
                if (s->p < s->end && *s->p == ',') {
                    const char* comma = s->p;
                    s->p++;
                    jp_ws(s);
                    if (s->p < s->end && *s->p == '}')
                        jp_fail_at(s, comma,
                                   "Illegal trailing comma before end of object");
                    continue;
                }
                break;
            }
            if (s->p >= s->end || *s->p != '}')
                jp_fail_at(s, s->p, "Expecting ',' delimiter");
            s->p++;
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
            // 下面长度不匹配、缺 ']' 都可能抛异常，异常展开跳过 return，
            // out 只有局部变量持有 —— 永久泄漏。
            // 之前 list/dict 分支都加了保护，唯独 tuple 漏了。
            pa_rc_temp(out);
            PA_PIN_SCOPE(out);
            jp_open(s, '[', "expected tuple");
            pos = 2;
            int64_t idx = 0;
            jp_ws(s);
            if (s->p < s->end && *s->p == ']') {
                if (count != 0) jp_fail_type(s, "tuple length mismatch");
                s->p++;
                PA_PIN_RELEASE(out);
                pa_rc_temp_take(out);
                return (int64_t)out;
            }
            for (;;) {
                if (idx >= count) jp_fail_type(s, "tuple length mismatch");
                {
                    size_t cl = desc_len(d + pos);
                    int64_t v = jp_value(s, d + pos, cl);
                    pa_list_append(out, v, 2);
                    pos += cl;
                }
                idx++;
                jp_ws(s);
                if (s->p < s->end && *s->p == ',') {
                    const char* comma = s->p;
                    s->p++;
                    jp_ws(s);
                    if (s->p < s->end && *s->p == ']')
                        jp_fail_at(s, comma,
                                   "Illegal trailing comma before end of array");
                    continue;
                }
                break;
            }
            if (s->p >= s->end || *s->p != ']')
                jp_fail_at(s, s->p, "Expecting ',' delimiter");
            s->p++;
            if (idx != count) jp_fail_type(s, "tuple length mismatch");
            PA_PIN_RELEASE(out);
            pa_rc_temp_take(out);
            return (int64_t)out;
        }
        pa_raise_c("ValueError", "json: bad type descriptor");
    }
    if (t == 's') return (int64_t)jp_string(s, 0);
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
        if (s->p < s->end && jp_other_value_start(*s->p))
            jp_fail_type(s, "expected null");
        jp_fail_at(s, s->p, "Expecting value");
    }
    pa_raise_c("ValueError", "json: bad type descriptor");
}

int64_t pa_json_loads(PyAdamasStr* text, PyAdamasStr* desc) {
    JP s = {text->data, text->data + text->len, text->data};
    int64_t v = jp_value(&s, desc->data, (size_t)desc->len);
    jp_ws(&s);
    if (s.p != s.end) jp_fail_at(&s, s.p, "Extra data");
    return v;
}
