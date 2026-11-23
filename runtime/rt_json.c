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

/* 增长是冷路径（容量翻倍），单独 outline，让追加热路径留在调用点内联。
 * 追加不再每次写 NUL：str_alloc_raw 分配时自带终止符，中间态没有人把
 * 缓冲当 C 串读（旧实现每字符一次 NUL store 是白付的）。 */
static __attribute__((noinline)) void jb_grow(JBuf* b, size_t need) {
    size_t cap = b->cap ? b->cap : 128;
    while (need > cap) cap *= 2;
    char* nb = (char*)realloc(b->buf, cap);
    if (!nb) panic_c("out of memory");
    b->buf = nb;
    b->cap = cap;
}

static inline void jb_raw(JBuf* b, const char* p, size_t n) {
    if (b->len + n + 1 > b->cap) jb_grow(b, b->len + n + 1);
    char* dst = b->buf + b->len;
    if (n <= 8) {
        for (size_t i = 0; i < n; ++i) dst[i] = p[i];
    } else {
        memcpy(dst, p, n);
    }
    b->len += n;
}

static inline void jb_ch(JBuf* b, char c) {
    if (b->len + 2 > b->cap) jb_grow(b, b->len + 2);
    b->buf[b->len++] = c;
}

static void jb_cstr(JBuf* b, const char* s) { jb_raw(b, s, strlen(s)); }

static void jb_u4(JBuf* b, uint32_t cp) {
    /* cp 由调用方保证 <= 0xFFFF。手写 4 位小写十六进制（与 %04x
     * 逐字节一致），省掉每个转义字符一次 snprintf（ensure_ascii
     * 的非 ASCII 密集路径上 perf 可见 __printf_buffer 残留）。 */
    static const char hex[] = "0123456789abcdef";
    char tmp[6];
    tmp[0] = '\\';
    tmp[1] = 'u';
    tmp[2] = hex[(cp >> 12) & 0xF];
    tmp[3] = hex[(cp >> 8) & 0xF];
    tmp[4] = hex[(cp >> 4) & 0xF];
    tmp[5] = hex[cp & 0xF];
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
        /* 快路径：成段搬运无需转义的 ASCII（ensure_ascii=False 时
         * >=0x80 的字节也原样输出，一并入段）。逐字符 jb_ch 的容量
         * 检查/调用开销在字符串密集 payload 里是大头（perf 实测
         * jb_raw 15% + jb_str 9%）。 */
        int64_t run = i;
        if (b->ensure_ascii) {
            while (run < n) {
                unsigned char c = p[run];
                if (c < 0x20 || c == '"' || c == '\\' || c >= 0x7f) break;
                run++;
            }
        } else {
            while (run < n) {
                unsigned char c = p[run];
                if (c < 0x20 || c == '"' || c == '\\') break;
                run++;
            }
        }
        if (run > i) {
            jb_raw(b, (const char*)p + i, (size_t)(run - i));
            i = run;
            if (i >= n) break;
        }
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

/* int64 → 十进制，手写 itoa（snprintf 的 %lld 在 int 密集 payload 里
 * 占 ~17% cycles：__printf_buffer + _itoa_word + 驱动开销）。
 * INT64_MIN 用 -(v+1)+1 取绝对值，避免 -INT64_MIN 溢出。 */
static void jb_i64(JBuf* b, int64_t v) {
    char tmp[24];
    char* p = tmp + sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    jb_raw(b, p, (size_t)(tmp + sizeof tmp - p));
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
    if (n > 0) {
        if (b->len + (size_t)n + 1 > b->cap) jb_grow(b, b->len + (size_t)n + 1);
        memset(b->buf + b->len, ' ', (size_t)n);
        b->len += (size_t)n;
    }
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
        jb_i64(b, kv);
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

/* 动态（描述符 'u'）值的运行期判型序列化，定义在文件末尾「动态 JSON」段。
   前向声明：jb_value 的 'u' 分支要用它。 */
static void jb_any_value(JBuf* b, int64_t v, int64_t depth);

static void jb_value(JBuf* b, int64_t v, const char* d, size_t n, int64_t depth) {
    char t = d[0];
    /* 'u'：静态类型是 unknown —— 运行期按值的实际种类判型。
       json.stringify() 走这条；dumps 的静态描述符永远不产生 'u'。 */
    if (t == 'u') {
        jb_any_value(b, v, depth);
        return;
    }
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
        jb_i64(b, v);
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

/* allow_nan=False 的预检：在写任何输出之前按描述符走一遍值树，
 * 遇到非有限 float（值或键）立即抛 ValueError。输出缓冲是 malloc
 * 的裸内存，longjmp 不会释放它（cleanup 属性不在 longjmp 时执行）——
 * 预检保证抛异常时还没有分配。只在 allow_nan=False（罕见）时多走
 * 一遍，默认路径零开销。 */
static void jb_check_nan(int64_t v, const char* d, size_t n) {
    char t = d[0];
    if (t == '(') {
        char k = d[1];
        if (k == 'l') {
            PyAdamasList* l = (PyAdamasList*)v;
            for (int64_t i = 0; i < l->len; ++i)
                jb_check_nan(l->data[i], d + 2, n - 3);
            return;
        }
        if (k == 'd') {
            PyAdamasDict* m = (PyAdamasDict*)v;
            char kk = d[2];
            for (int64_t i = 0; i < m->len; ++i) {
                if (kk == 'f') {
                    union { int64_t i; double d; } u;
                    u.i = pa_dict_key_at(m, i);
                    if (u.d != u.d || u.d > 1.7976931348623157e308 ||
                        u.d < -1.7976931348623157e308) {
                        const char* what = u.d != u.d ? "nan"
                                           : (u.d > 0 ? "inf" : "-inf");
                        char msg[96];
                        snprintf(msg, sizeof msg,
                                 "Out of range float values are not JSON compliant: %s",
                                 what);
                        pa_raise_c("ValueError", msg);
                    }
                }
                jb_check_nan(pa_dict_val_at(m, i), d + 3, n - 4);
            }
            return;
        }
        if (k == 't') {
            PyAdamasList* l = (PyAdamasList*)v;
            size_t pos = 2;
            int64_t idx = 0;
            while (pos + 1 < n) {
                size_t cl = desc_len(d + pos);
                jb_check_nan(l->data[idx], d + pos, cl);
                pos += cl;
                idx++;
            }
            return;
        }
        return;
    }
    if (t == 'f') {
        union { int64_t i; double d; } u;
        u.i = v;
        if (u.d != u.d || u.d > 1.7976931348623157e308 ||
            u.d < -1.7976931348623157e308) {
            const char* what = u.d != u.d ? "nan" : (u.d > 0 ? "inf" : "-inf");
            char msg[96];
            snprintf(msg, sizeof msg,
                     "Out of range float values are not JSON compliant: %s", what);
            pa_raise_c("ValueError", msg);
        }
    }
}

/* flags: bit0 sort_keys / bit1 ensure_ascii / bit2 allow_nan。
 * seps: NULL = 默认分隔符（紧凑 ", "/": "，缩进 ","/": "）；
 *       否则是 tuple[str, str]（运行期 PyAdamasList，借用不 retain）。 */
PyAdamasStr* pa_json_dumps(int64_t v, PyAdamasStr* desc, int64_t indent,
                           int64_t flags, PyAdamasList* seps) {
    if (!(flags & 4)) jb_check_nan(v, desc->data, (size_t)desc->len);
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
    /* ensure_ascii=True 且默认分隔符时输出保证全 ASCII（jb_str 把
     * >=0x7f 全转义），pub==len 免掉 str_seal 的整串码点扫描。
     * 自定义分隔符可能含非 ASCII（CPython 原样插入），走常规扫描。 */
    PyAdamasStr* out;
    if (b.ensure_ascii && !seps)
        out = str_new_ascii(b.buf ? b.buf : "", (int64_t)b.len);
    else
        out = str_new(b.buf ? b.buf : "", (int64_t)b.len);
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

static int jp_hexval(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

/* key_ctx：dict 键位置——期望双引号的失败消息是
 * "Expecting property name enclosed in double quotes"（CPython 口径）。
 *
 * 两遍结构（0.314）：第一遍只校验 + 统计解码长度，**不分配**——
 * 旧实现边解码边建 JBuf，中途遇到非法转义抛异常时 malloc 的缓冲
 * 随 longjmp 丢失（cleanup 属性不在 longjmp 时执行，ASan 实测 232
 * 漏 3×128B）。校验通过后第二遍才分配（一次 jb_grow 到精确容量）
 * 并解码，解码路径不再可能抛异常。无转义串直接从输入切片建 str，
 * 全 ASCII 用 str_new_ascii 免码点扫描。 */
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
    const char* body = s->p;
    /* ── 第一遍：校验 ── */
    const char* q = body;
    int has_esc = 0;
    int has_high = 0;
    size_t dlen = 0;
    while (q < s->end) {
        unsigned char c = (unsigned char)*q;
        if (c == '"') break;
        if (c < 0x20) jp_fail_at(s, q, "Invalid control character at");
        if (c == '\\') {
            has_esc = 1;
            /* 类型口径错误（孤立代理等）的位置指向转义的反斜杠。 */
            s->p = q;
            if (q + 1 >= s->end)
                jp_fail_at(s, strstart, "Unterminated string starting at");
            char e = q[1];
            if (e == 'u') {
                const char* upos = q + 1;
                if (s->end - (q + 2) < 4)
                    jp_fail_at(s, upos, "Invalid \\uXXXX escape");
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    int d = jp_hexval(q[2 + i]);
                    if (d < 0) jp_fail_at(s, upos, "Invalid \\uXXXX escape");
                    cp = cp * 16 + (uint32_t)d;
                }
                q += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (s->end - q >= 6 && q[0] == '\\' && q[1] == 'u') {
                        const char* lpos = q + 1;
                        uint32_t lo = 0;
                        for (int i = 0; i < 4; ++i) {
                            int d = jp_hexval(q[2 + i]);
                            if (d < 0)
                                jp_fail_at(s, lpos, "Invalid \\uXXXX escape");
                            lo = lo * 16 + (uint32_t)d;
                        }
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            jp_fail_type(s, "invalid surrogate pair");
                        q += 6;
                        dlen += 4;
                    } else {
                        /* CPython 接受孤立代理（返回含代理码点的 str）；
                         * pyadamas str 是 UTF-8 字节串，无法表示，拒绝。 */
                        jp_fail_type(s, "unpaired surrogate");
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    jp_fail_type(s, "unpaired surrogate");
                } else {
                    dlen += cp < 0x80 ? 1 : (cp < 0x800 ? 2 : 3);
                }
            } else if (e == '"' || e == '\\' || e == '/' || e == 'b' ||
                       e == 'f' || e == 'n' || e == 'r' || e == 't') {
                q += 2;
                dlen += 1;
            } else {
                jp_fail_at(s, q, "Invalid \\escape");
            }
            continue;
        }
        if (c >= 0x80) has_high = 1;
        dlen += 1;
        q++;
    }
    if (q >= s->end) jp_fail_at(s, strstart, "Unterminated string starting at");
    s->p = q + 1;
    /* ── 无转义：切片直建 ── */
    if (!has_esc) {
        return has_high ? str_new(body, (int64_t)(q - body))
                        : str_new_ascii(body, (int64_t)(q - body));
    }
    /* ── 第二遍：解码（已校验，不会抛异常）── */
    JBuf b = {0};
    jb_grow(&b, dlen + 1);
    const char* r = body;
    while (r < q) {
        if ((unsigned char)*r != '\\') {
            const char* run = r + 1;
            while (run < q && (unsigned char)*run != '\\') run++;
            jb_raw(&b, r, (size_t)(run - r));
            r = run;
            continue;
        }
        char e = r[1];
        r += 2;
        switch (e) {
            case '"': jb_ch(&b, '"'); break;
            case '\\': jb_ch(&b, '\\'); break;
            case '/': jb_ch(&b, '/'); break;
            case 'b': jb_ch(&b, '\b'); break;
            case 'f': jb_ch(&b, '\f'); break;
            case 'n': jb_ch(&b, '\n'); break;
            case 'r': jb_ch(&b, '\r'); break;
            case 't': jb_ch(&b, '\t'); break;
            default: {
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i)
                    cp = cp * 16 + (uint32_t)jp_hexval(r[i]);
                r += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo = 0;
                    for (int i = 0; i < 4; ++i)
                        lo = lo * 16 + (uint32_t)jp_hexval(r[2 + i]);
                    r += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                jp_utf8_put(&b, cp);
                break;
            }
        }
    }
    PyAdamasStr* out = str_new(b.buf, (int64_t)b.len);
    free(b.buf);
    return out;
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
     * pyadamas 是 int64 语言，按口径报 ValueError 而不是回绕）。
     * 手写累加代替 strtoll（perf 实测 strtoll 4.3% + token 拷贝）：
     * 19 位数字 < 2^64，uint64 累加不会回绕，边界用 limit 判。 */
    int neg = start[0] == '-';
    size_t digits = n - (neg ? 1u : 0u);
    if (digits > 19) jp_fail_type(s, "integer out of int64 range");
    uint64_t u = 0;
    for (size_t i = neg ? 1u : 0u; i < n; ++i)
        u = u * 10 + (uint64_t)(start[i] - '0');
    uint64_t limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    if (u > limit) jp_fail_type(s, "integer out of int64 range");
    s->p = start + n;
    /* neg 且 u==2^63 时 -(int64_t)(u-1)-1 == INT64_MIN，全程无溢出。 */
    return neg ? -(int64_t)(u - 1) - 1 : (int64_t)u;
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
                /* 描述符静态已知元素堆性（kind 0/1），传准确 heap 提示
                 * 免掉逐元素 pa_unknown_is_heap 探针（0.303 内部 append
                 * 同款口径）。 */
                pa_list_append(out, v, kind);
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
                    pa_list_append(out, v, kind);
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

/* ══════════════════════════════════════════════════════════════════
   动态 JSON（0.447）：json.parse / json.stringify + json.* 辅助函数
   ══════════════════════════════════════════════════════════════════

   静态描述符（pa_json_loads/pa_json_dumps）要求调用点给出 sample 的完整
   静态类型；LSP 这类「消息形状由对端决定」的协议拿不到 sample。这里让
   解析器按 JSON 文本自身构造值，值统一落在 unknown 机器字里：

     null           → PYADAMAS_RC_JSONNULL 盒    （payload 无用）
     true / false   → PYADAMAS_RC_JSONBOOL 盒    （payload 0/1）
     JSON 整数      → 裸 int64（非堆，不装盒）
     JSON 小数/指数 → CPYFLOAT 盒（pa_any_from_f64，和 unknown 域 float 同形）
     JSON 字符串    → PyAdamasStr*
     JSON 数组      → PyAdamasList（elem_kind=2，元素是 unknown）
     JSON 对象      → PyAdamasDict（key_kind=1 str / key_own=1 / val_own=2）

   为什么 null/bool 要装盒：unknown 值域里 None/bool/int 全是机器字，容器
   元素又没有逐字 tag（P2 边界，见 docs/type-tags-landing.md §5.1）。若不
   装盒，json.parse("null") 再 stringify 会输出 0、true 会输出 1 —— 动态
   JSON 的往返（本功能的核心承诺）就没了。两个盒都和 CPYFLOAT 一样是
   「无堆出边、tracked 分配」的小对象，探针 + 头 kind 即可判型。

   判型统一走 json_classify()：非堆 → int；堆 → 头 kind。不带 tag 的
   unknown 值（从容器取出的嵌套值）也能正确判型，这是 helper 与
   stringify 都依赖的口径。

   已知边界（文档同步）：
     - 用户代码在 unknown 位置手写的 True/False/None 仍是裸 0/1（语言既有
       语义），json.stringify 会把它们序列化成 1/0。要构造 JSON 的
       true/false/null，用 json.bool()/json.null() 显式拿盒。
     - int64 放不下的 JSON 整数抛 ValueError（语言 int 是 i64；CPython 是
       任意精度 bigint）。
     - 嵌套深度上限 512（避免解析/序列化递归爆 C 栈），超限抛 ValueError。
*/

enum {
    /* 与 src/lower_import_misc.cpp 的 JSONOP_* 逐项一致。 */
    JSONOP_GET = 1,
    JSONOP_GET_OR = 2,
    JSONOP_HAS = 3,
    JSONOP_LEN = 4,
    JSONOP_KEYS = 5,
    JSONOP_TYPE = 6,
    JSONOP_IS_NULL = 7,
    JSONOP_AS_STR = 8,
    JSONOP_AS_INT = 9,
    JSONOP_AS_FLOAT = 10,
    JSONOP_AS_BOOL = 11,
    JSONOP_NULL = 12,
    JSONOP_BOOL = 13,
    JSONOP_AS_DICT = 14,
    JSONOP_AS_LIST = 15,
    JSONOP_PATH = 16,
};

#define JSON_DYN_MAX_DEPTH 512

/* 值的运行期种类。JC_NONHEAP 是「裸机器字」：动态 JSON 里只可能是整数
   （float 已装盒）；用户手写的 None/bool 也落在这里，和 CPython 的
   None==0 / True==1 口径一致。 */
enum {
    JC_NONHEAP = 0,
    JC_NULL,
    JC_BOOL,
    JC_STR,
    JC_FLOAT,
    JC_LIST,
    JC_DICT,
    JC_BIG,
    JC_OTHER,
};

static int json_classify(int64_t v) {
    if (!pa_unknown_is_heap(v)) return JC_NONHEAP;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    switch (h->kind) {
        case PYADAMAS_RC_JSONNULL: return JC_NULL;
        case PYADAMAS_RC_JSONBOOL: return JC_BOOL;
        case PYADAMAS_RC_STR: return JC_STR;
        case PYADAMAS_RC_CPYFLOAT: return JC_FLOAT;
        case PYADAMAS_RC_LIST: return JC_LIST;
        case PYADAMAS_RC_DICT: return JC_DICT;
        case PYADAMAS_RC_BIGINT: return JC_BIG;
        default: return JC_OTHER;
    }
}

/* 8 字节无出边盒（同 CPYFLOAT 的分配方式：tracked 分配，unknown 探针靠
   RC 索引认得出它）。 */
static int64_t json_box_new(int64_t kind, int64_t payload) {
    int64_t* p = (int64_t*)rc_alloc((int64_t)sizeof(int64_t), NULL, NULL, kind);
    *p = payload;
    return (int64_t)(uintptr_t)p;
}

static int64_t json_null_new(void) { return json_box_new(PYADAMAS_RC_JSONNULL, 0); }

static int64_t json_bool_new(int64_t v) {
    return json_box_new(PYADAMAS_RC_JSONBOOL, v ? 1 : 0);
}

static int64_t json_bool_payload(int64_t v) { return *(int64_t*)(uintptr_t)v; }

/* ── 解析：按文本自身构造动态值 ─────────────────────────────── */

static int64_t jp_dyn_value(JP* s, int64_t depth);

static int64_t jp_dyn_object(JP* s, int64_t depth) {
    PyAdamasDict* out = pa_dict_new(1, 1, 2, -1, -1);
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
        /* 键可能在本轮后续（缺 ':'、value 语法错）抛错时还没交给 dict：
           rc=1 的字符串只被 C 局部变量引用，异常展开时没人 dec → 泄漏
           （ASan 实测每次错误解析漏一个键）。挂进临时栈：正常路径转移
           所有权时 take 掉，异常路径由调用方 landing pad 的 cleanup 释放。 */
        pa_rc_temp(key);
        jp_ws(s);
        if (s->p >= s->end || *s->p != ':')
            jp_fail_at(s, s->p, "Expecting ':' delimiter");
        s->p++;
        int64_t v = jp_dyn_value(s, depth + 1);
        /* 插入路径不 retain：所有权从本帧转给 dict（与 jp_value 静态版
           本同口径）。重复键时 dict 内部会 dec 掉这份新键、保留旧键。 */
        pa_dict_set(out, (int64_t)key, v);
        pa_rc_temp_take(key);
        jp_ws(s);
        if (s->p < s->end && *s->p == ',') {
            const char* comma = s->p;
            s->p++;
            jp_ws(s);
            if (s->p < s->end && *s->p == '}')
                jp_fail_at(s, comma, "Illegal trailing comma before end of object");
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

static int64_t jp_dyn_array(JP* s, int64_t depth) {
    PyAdamasList* out = pa_list_new(0, 2);
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
        int64_t v = jp_dyn_value(s, depth + 1);
        /* append 不 retain：所有权转给列表。elem_kind=2 让销毁按未知
           种类逐元素 dec（heap_bits 整体回退）。 */
        pa_list_append(out, v, 2);
        jp_ws(s);
        if (s->p < s->end && *s->p == ',') {
            const char* comma = s->p;
            s->p++;
            jp_ws(s);
            if (s->p < s->end && *s->p == ']')
                jp_fail_at(s, comma, "Illegal trailing comma before end of array");
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

/* 严格 JSON 数字：有小数/指数部分是 float（CPython json 口径），否则整数。
   整数溢出 int64 直接抛 ValueError（同 json.loads 静态版本）。 */
static int64_t jp_dyn_number(JP* s) {
    const char* start = s->p;
    int isfloat = 0;
    size_t n = jp_num_scan(s, &isfloat);
    if (n == 0) jp_fail_at(s, s->p, "Expecting value");
    if (!isfloat) {
        int neg = start[0] == '-';
        size_t digits = n - (neg ? 1u : 0u);
        if (digits > 19) jp_fail_type(s, "integer out of int64 range");
        uint64_t u = 0;
        for (size_t i = neg ? 1u : 0u; i < n; ++i)
            u = u * 10 + (uint64_t)(start[i] - '0');
        uint64_t limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
        if (u > limit) jp_fail_type(s, "integer out of int64 range");
        s->p = start + n;
        return neg ? -(int64_t)(u - 1) - 1 : (int64_t)u;
    }
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
    double d = strtod(tok, NULL);
    free(big);
    s->p = start + n;
    return pa_any_from_f64(d);
}

/* 字面量：true/false/null。按最长已知词匹配后交给外层判 Extra data
   （与 CPython 的 "Expecting value" 位置口径一致即可）。 */
static int64_t jp_dyn_literal(JP* s) {
    if (s->end - s->p >= 4 && strncmp(s->p, "true", 4) == 0) {
        s->p += 4;
        return json_bool_new(1);
    }
    if (s->end - s->p >= 5 && strncmp(s->p, "false", 5) == 0) {
        s->p += 5;
        return json_bool_new(0);
    }
    if (s->end - s->p >= 4 && strncmp(s->p, "null", 4) == 0) {
        s->p += 4;
        return json_null_new();
    }
    jp_fail_at(s, s->p, "Expecting value");
}

static int64_t jp_dyn_value(JP* s, int64_t depth) {
    if (depth > JSON_DYN_MAX_DEPTH) {
        char msg[96];
        snprintf(msg, sizeof msg, "maximum nesting depth exceeded (%d)",
                 (int)JSON_DYN_MAX_DEPTH);
        pa_raise_c("ValueError", msg);
    }
    jp_ws(s);
    if (s->p >= s->end) jp_fail_at(s, s->p, "Expecting value");
    char c = *s->p;
    if (c == '{') return jp_dyn_object(s, depth);
    if (c == '[') return jp_dyn_array(s, depth);
    if (c == '"') return (int64_t)jp_string(s, 0);
    if (c == 't' || c == 'f' || c == 'n') return jp_dyn_literal(s);
    /* NaN/Infinity 扩展：和静态 loads 的 float 样例、CPython json.loads
       默认口径一致（parse_constant）。标准 JSON 不会有，但往返要保真。 */
    if (c == 'N' || c == 'I' || (c == '-' && s->end - s->p >= 9 &&
                                 strncmp(s->p, "-Infinity", 9) == 0))
        return pa_any_from_f64(jp_float(s));
    if (c == '-' || (c >= '0' && c <= '9')) return jp_dyn_number(s);
    jp_fail_at(s, s->p, "Expecting value");
}

int64_t pa_json_parse(PyAdamasStr* text) {
    JP s = {text->data, text->data + text->len, text->data};
    int64_t v = jp_dyn_value(&s, 0);
    jp_ws(&s);
    if (s.p != s.end) jp_fail_at(&s, s.p, "Extra data");
    return v;
}

/* ── 序列化：unknown 值按运行期种类判型 ─────────────────────── */

static void jb_any_string(JBuf* b, PyAdamasStr* s) { jb_str(b, s); }

static void jb_any_value(JBuf* b, int64_t v, int64_t depth) {
    if (depth > JSON_DYN_MAX_DEPTH) {
        char msg[96];
        snprintf(msg, sizeof msg, "maximum nesting depth exceeded (%d)",
                 (int)JSON_DYN_MAX_DEPTH);
        pa_raise_c("ValueError", msg);
    }
    int jc = json_classify(v);
    switch (jc) {
        case JC_NONHEAP:
            jb_i64(b, v);
            return;
        case JC_NULL:
            jb_cstr(b, "null");
            return;
        case JC_BOOL:
            jb_cstr(b, json_bool_payload(v) ? "true" : "false");
            return;
        case JC_STR:
            jb_any_string(b, (PyAdamasStr*)(uintptr_t)v);
            return;
        case JC_FLOAT: {
            double d;
            memcpy(&d, (void*)(uintptr_t)v, sizeof d);
            jb_f64(b, d);
            return;
        }
        case JC_BIG: {
            PyAdamasStr* t = pa_big_str((PyAdamasBig*)(uintptr_t)v);
            jb_raw(b, t->data, (size_t)t->len);
            pa_rc_dec(t);
            return;
        }
        case JC_LIST: {
            PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
            jb_ch(b, '[');
            for (int64_t i = 0; i < l->len; ++i) {
                if (i) jb_raw(b, b->item_sep, b->item_sep_len);
                jb_nl(b, depth + 1);
                jb_any_value(b, l->data[i], depth + 1);
            }
            if (l->len) jb_nl(b, depth);
            jb_ch(b, ']');
            return;
        }
        case JC_DICT: {
            PyAdamasDict* m = (PyAdamasDict*)(uintptr_t)v;
            jb_ch(b, '{');
            for (int64_t i = 0; i < m->len; ++i) {
                if (i) jb_raw(b, b->item_sep, b->item_sep_len);
                jb_nl(b, depth + 1);
                int64_t kv = pa_dict_key_at(m, i);
                /* 动态对象来自 json.parse 时键恒为 str；用户传进来的
                   dict 键按 CPython json 的强制转字符串规则处理。 */
                if (m->key_kind == 1 ||
                    (m->key_kind == 3 && pa_unknown_is_heap(kv) &&
                     json_classify(kv) == JC_STR)) {
                    jb_str(b, (PyAdamasStr*)(uintptr_t)kv);
                } else if (m->key_kind == 2) {
                    double kd;
                    memcpy(&kd, &kv, sizeof kd);
                    jb_ch(b, '"');
                    jb_f64(b, kd);
                    jb_ch(b, '"');
                } else {
                    jb_ch(b, '"');
                    jb_i64(b, kv);
                    jb_ch(b, '"');
                }
                jb_raw(b, b->key_sep, b->key_sep_len);
                jb_any_value(b, pa_dict_val_at(m, i), depth + 1);
            }
            if (m->len) jb_nl(b, depth);
            jb_ch(b, '}');
            return;
        }
        default:
            pa_raise_c("TypeError", "json.stringify: object is not JSON serializable");
    }
}

/* ── json.* 辅助（全部经 pa_json_op 一个入口）────────────────── */

static _Noreturn void json_type_error(const char* fname, const char* what) {
    char msg[160];
    snprintf(msg, sizeof msg, "json.%s(): %s", fname, what);
    pa_raise_c("TypeError", msg);
}

/* 取元素：dict（str/int 键）或 list（int 下标）。*found 为 0 时返回 0，
   由调用方决定抛 KeyError/IndexError 还是给默认值；命中值按 +1 交回。 */
static int64_t json_get_value(int64_t v, int64_t key, int* found, int has_def,
                              int64_t def) {
    (void)def; /* 默认值只在 json_get_op 的未命中路径用，这里只回传 found */
    int jc = json_classify(v);
    if (jc == JC_DICT) {
        PyAdamasDict* d = (PyAdamasDict*)(uintptr_t)v;
        int kh = (int)d->key_kind;
        if (kh == 1) {
            if (json_classify(key) != JC_STR) {
                if (has_def) { *found = 0; return 0; }
                json_type_error("get", "object keys are strings");
            }
        } else if (kh == 0 || kh == 2) {
            if (pa_unknown_is_heap(key)) {
                if (has_def) { *found = 0; return 0; }
                json_type_error("get", "object keys are ints");
            }
        }
        int64_t out = 0;
        if (pa_dict_try_get(d, key, &out)) {
            *found = 1;
            pa_rc_inc_unknown(out);
            return out;
        }
        *found = 0;
        return 0;
    }
    if (jc == JC_LIST) {
        if (pa_unknown_is_heap(key)) {
            if (has_def) { *found = 0; return 0; }
            json_type_error("get", "array index must be an int");
        }
        PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
        int64_t i = key;
        if (i < 0) i += l->len;
        if (i < 0 || i >= l->len) { *found = 0; return 0; }
        *found = 1;
        int64_t out = pa_list_get(l, i);
        pa_rc_inc_unknown(out);
        return out;
    }
    if (jc == JC_STR) {
        if (pa_unknown_is_heap(key)) {
            if (has_def) { *found = 0; return 0; }
            json_type_error("get", "string index must be an int");
        }
        PyAdamasStr* s = (PyAdamasStr*)(uintptr_t)v;
        if (key < -s->pub || key >= s->pub) { *found = 0; return 0; }
        *found = 1;
        return (int64_t)pa_str_char_at(s, key);
    }
    if (has_def) { *found = 0; return 0; }
    json_type_error("get", "value is not an object or array");
}

static int64_t json_get_op(int64_t v, int64_t key, int has_def, int64_t def,
                           int fname_kind) {
    int found = 0;
    int64_t r = json_get_value(v, key, &found, has_def, def);
    if (found) return r;
    if (has_def) {
        pa_rc_inc_unknown(def);
        return def;
    }
    if (fname_kind == 1) {
        /* has()：缺失返回 0，不抛。 */
        return 0;
    }
    if (json_classify(v) == JC_LIST) {
        pa_raise_c("IndexError", "json.get(): array index out of range");
    }
    if (json_classify(v) == JC_DICT) {
        pa_raise_c("KeyError", "json.get(): key not found");
    }
    json_type_error("get", "value is not an object or array");
}

static int64_t json_as_int(int64_t v) {
    int jc = json_classify(v);
    if (jc == JC_NONHEAP) return v;
    if (jc == JC_BOOL) return json_bool_payload(v);
    json_type_error("as_int", "value is not an int");
}

static double json_as_double(int64_t v) {
    int jc = json_classify(v);
    if (jc == JC_NONHEAP) return (double)v;
    if (jc == JC_BOOL) return (double)json_bool_payload(v);
    if (jc == JC_FLOAT) {
        double d;
        memcpy(&d, (void*)(uintptr_t)v, sizeof d);
        return d;
    }
    json_type_error("as_float", "value is not a number");
}

static int64_t json_truthy(int64_t v) {
    int jc = json_classify(v);
    switch (jc) {
        case JC_NONHEAP: return v != 0;
        case JC_NULL: return 0;
        case JC_BOOL: return json_bool_payload(v) != 0;
        case JC_STR: return ((PyAdamasStr*)(uintptr_t)v)->len != 0;
        case JC_LIST: return ((PyAdamasList*)(uintptr_t)v)->len != 0;
        case JC_DICT: return ((PyAdamasDict*)(uintptr_t)v)->len != 0;
        case JC_FLOAT: {
            double d;
            memcpy(&d, (void*)(uintptr_t)v, sizeof d);
            return d != 0.0;
        }
        case JC_BIG: return pa_big_truth((PyAdamasBig*)(uintptr_t)v);
        default: return 1;
    }
}

static int64_t json_type_name(int64_t v) {
    const char* name = "unknown";
    switch (json_classify(v)) {
        case JC_NONHEAP: name = "int"; break;
        case JC_NULL: name = "null"; break;
        case JC_BOOL: name = "bool"; break;
        case JC_STR: name = "str"; break;
        case JC_FLOAT: name = "float"; break;
        case JC_LIST: name = "list"; break;
        case JC_DICT: name = "dict"; break;
        case JC_BIG: name = "int"; break;
        default: name = "unknown"; break;
    }
    return (int64_t)str_new_ascii(name, (int64_t)strlen(name));
}

static int64_t json_len_op(int64_t v) {
    switch (json_classify(v)) {
        case JC_STR: return ((PyAdamasStr*)(uintptr_t)v)->pub;
        case JC_LIST: return ((PyAdamasList*)(uintptr_t)v)->len;
        case JC_DICT: return ((PyAdamasDict*)(uintptr_t)v)->len;
        case JC_BIG: return 1; /* 数字没有长度；兜底成 1 不静默给 0 */
        default: json_type_error("len", "value has no len()");
    }
}

int64_t pa_json_op(int64_t op, int64_t a, int64_t b, int64_t c) {
    switch (op) {
        case JSONOP_GET:
            return json_get_op(a, b, 0, 0, 0);
        case JSONOP_GET_OR:
            return json_get_op(a, b, 1, c, 0);
        case JSONOP_HAS:
            return json_get_op(a, b, 0, 0, 1);
        case JSONOP_LEN:
            return json_len_op(a);
        case JSONOP_KEYS:
            if (json_classify(a) != JC_DICT)
                json_type_error("keys", "value is not an object");
            return (int64_t)pa_dict_keys((PyAdamasDict*)(uintptr_t)a);
        case JSONOP_TYPE:
            return json_type_name(a);
        case JSONOP_IS_NULL:
            return json_classify(a) == JC_NULL;
        case JSONOP_AS_STR:
            if (json_classify(a) != JC_STR)
                json_type_error("as_str", "value is not a string");
            pa_rc_inc((void*)(uintptr_t)a);
            return a;
        case JSONOP_AS_INT:
            return json_as_int(a);
        case JSONOP_AS_FLOAT: {
            double d = json_as_double(a);
            int64_t out;
            memcpy(&out, &d, sizeof out);
            return out;
        }
        case JSONOP_AS_BOOL:
            return json_truthy(a);
        case JSONOP_NULL:
            return json_null_new();
        case JSONOP_BOOL:
            return json_bool_new(a != 0);
        case JSONOP_AS_DICT:
            if (json_classify(a) != JC_DICT)
                json_type_error("as_dict", "value is not an object");
            pa_rc_inc((void*)(uintptr_t)a);
            return a;
        case JSONOP_AS_LIST:
            if (json_classify(a) != JC_LIST)
                json_type_error("as_list", "value is not an array");
            pa_rc_inc((void*)(uintptr_t)a);
            return a;
        case JSONOP_PATH: {
            /* a=根值（借用），b=list[str] 键路径。逐层 get；每层结果持有
               一份，最后一层交回。 */
            if (!b) json_type_error("path", "path must be a list of str");
            PyAdamasList* keys = (PyAdamasList*)(uintptr_t)b;
            int64_t cur = a;
            int owned = 0;
            for (int64_t i = 0; i < keys->len; ++i) {
                int64_t key = keys->data[i];
                if (json_classify(key) != JC_STR)
                    json_type_error("path", "path must be a list of str");
                int found = 0;
                int64_t next = json_get_value(cur, key, &found, 0, 0);
                if (!found) {
                    if (owned) pa_rc_dec_unknown(cur);
                    pa_raise_c("KeyError", "json.path(): key not found");
                }
                if (owned) pa_rc_dec_unknown(cur);
                cur = next;
                owned = 1;
            }
            if (!owned) pa_rc_inc_unknown(cur);
            return cur;
        }
        default:
            pa_raise_c("ValueError", "json: bad helper opcode");
    }
}

