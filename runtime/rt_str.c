// rt_str.c —— 字符串与字符串方法
#include "rt_internal.h"

/* ── 短串内联拷贝 / 比较 ────────────────────────────────────────
   字符串方法里最常见的形态是「对每个元素做一次 memcpy/memcmp」——
   join、逐字符比较、startswith、find 都是。元素长度分布极偏：
   拼接单字符、跟字面量比单字符，n 几乎总是 <= 8。

   这种情形走 PLT 调 libc 的固定成本（保存被调用者寄存器 + IFUNC 跳板 +
   长度分派）比真正的搬运贵一个数量级。实测 str_build：
   8.8% 的采样落在 __memmove_evex_unaligned_erms 上，其中**绝大多数是
   n == 0 的空分隔符拷贝**和 n == 1 的单字符拷贝 —— 每次都在付跳板钱。
   5.1% 落在 __memcmp_evex_movbe 上，全是 1 字节比较。

   这里给 n <= 8 的内联路径：小 n 用定长循环，gcc 展开成几条 mov，
   比 PLT 调用便宜；n > 8 才回落到 libc（长串上 libc 的向量化仍然是
   最优的，不能为了短串牺牲长串）。 */
static inline void sm_copy_bytes(char* dst, const char* src, int64_t n) {
    if (n <= 0) return;
    if (n <= 8) {
        for (int64_t i = 0; i < n; ++i) dst[i] = src[i];
        return;
    }
    memcpy(dst, src, (size_t)n);
}

static inline int sm_bytes_eq(const char* a, const char* b, int64_t n) {
    if (n <= 0) return 1;
    if (n <= 8) {
        for (int64_t i = 0; i < n; ++i)
            if (a[i] != b[i]) return 0;
        return 1;
    }
    return memcmp(a, b, (size_t)n) == 0;
}

/* 返回 -1 / 0 / +1（调用方只判符号，不必是 memcmp 的原始差值）。 */
static inline int sm_bytes_cmp(const char* a, const char* b, int64_t n) {
    if (n <= 0) return 0;
    if (n <= 8) {
        for (int64_t i = 0; i < n; ++i) {
            unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
            if (x != y) return x < y ? -1 : 1;
        }
        return 0;
    }
    return memcmp(a, b, (size_t)n);
}

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

PyAdamasStr* pa_repr_tuple(PyAdamasList* t, int64_t kind) {
    size_t cap = 64, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) panic_c("out of memory");
    buf[0] = 0;
    tuple_buf_put(&buf, &cap, &len, "(", 1);
    for (int64_t i = 0; i < t->len; ++i) {
        if (i) tuple_buf_put(&buf, &cap, &len, ", ", 2);
        int64_t v = t->data[i];
        PyAdamasStr* s;
        if (kind == 1) {
            double f;
            memcpy(&f, &v, sizeof f);
            s = pa_str_from_f64(f);
        } else if (kind == 2) {
            s = pa_repr_str((PyAdamasStr*)(uintptr_t)v);
        } else if (kind == 3) {
            s = pa_str_from_bool(v);
        } else if (kind == 4) {
            s = pa_str_from_none();
        } else {
            s = pa_str_from_i64(v);
        }
        tuple_buf_put(&buf, &cap, &len, s->data, (size_t)s->len);
        // **s 是这一轮新建的临时字符串，必须还掉。**
        //
        // exact-RC 模式下字符串是 untracked leaf：不进 GC 堆链，只能靠 RC
        // 精确释放。漏掉这一句在保守模式（有全局索引）下会被 GC 兜住，
        // 但 exact-RC 下直接泄漏 —— 实测 75_tuple_repr 漏 96 字节。
        pa_rc_dec(s);
    }
    if (t->len == 1) tuple_buf_put(&buf, &cap, &len, ",", 1);
    tuple_buf_put(&buf, &cap, &len, ")", 1);
    PyAdamasStr* out = str_new(buf, (int64_t)len);
    free(buf);
    return out;
}
/* str 拼接：pub 直接相加（O(1) 计长），字节照旧整段拷贝。 */
PyAdamasStr* pa_str_concat(PyAdamasStr* a, PyAdamasStr* b) {
    PyAdamasStr* s = str_alloc_raw(a->len + b->len);
    char* p = (char*)s->data;
    memcpy(p, a->data, (size_t)a->len);
    memcpy(p + a->len, b->data, (size_t)b->len);
    return str_seal_n(s, a->pub + b->pub);
}

/* bytes 拼接：pub == len，字节语义不变。 */
PyAdamasStr* pa_bytes_concat(PyAdamasStr* a, PyAdamasStr* b) {
    PyAdamasStr* s = str_alloc_raw(a->len + b->len);
    char* p = (char*)s->data;
    memcpy(p, a->data, (size_t)a->len);
    memcpy(p + a->len, b->data, (size_t)b->len);
    return str_seal_n(s, a->len + b->len);
}

/* str 下标：按码点。负索引加 pub；ASCII 直接查驻留表，非 ASCII 现编。 */
PyAdamasStr* pa_str_char_at(PyAdamasStr* s, int64_t i) {
    if (i < 0) i += s->pub;
    if (i < 0 || i >= s->pub) pa_raise_c("IndexError", "string index out of range");
    if (s->pub == s->len) return pa_char_str((unsigned char)s->data[i]);  /* 全 ASCII 快路径 */
    int64_t off = str_cp_to_byte(s, i);
    int n = 0;
    uint32_t cp = str_cp_at(s, off, &n);
    if (cp < 0x80) return pa_char_str((int64_t)cp);
    PyAdamasStr* out = str_alloc_raw(n);
    memcpy((char*)out->data, s->data + off, (size_t)n);
    return str_seal_n(out, 1);
}

/* 带游标的 str 下标：命中「紧接着上一次取出的码点」时 O(1)。
   降级器在顺序迭代（for ch in s / enumerate / zip / 推导式）里挂游标，
   否则朴素实现每轮从头扫，长串会退化成 O(n^2)。 */
PyAdamasStr* pa_str_char_at_cursor(PyAdamasStr* s, int64_t i, PyAdamasStrCursor* cur) {
    if (i < 0) i += s->pub;
    if (i < 0 || i >= s->pub) pa_raise_c("IndexError", "string index out of range");
    /* 全 ASCII 快路径：码点下标就是字节偏移，也不需要维护游标。
       ASCII 是绝对主流，这条路径的成本与改动前完全一致。 */
    if (s->pub == s->len) return pa_char_str((unsigned char)s->data[i]);
    int64_t off;
    if (cur && i == cur->cp + 1 && cur->off >= 0 && cur->off <= s->len) {
        off = cur->off;
    } else {
        off = str_cp_to_byte(s, i);
    }
    int n = 0;
    uint32_t cp = str_cp_at(s, off, &n);
    if (cur) {
        cur->cp = i;
        cur->off = off + n;
    }
    if (cp < 0x80) return pa_char_str((int64_t)cp);
    PyAdamasStr* out = str_alloc_raw(n);
    memcpy((char*)out->data, s->data + off, (size_t)n);
    return str_seal_n(out, 1);
}

/* 字节长度出口（O(1)）：len_bytes(s)。 */
int64_t pa_len_bytes(PyAdamasStr* s) { return s ? s->len : 0; }

/* str.encode()：bytes 语义的副本。全 ASCII 时 pub == len，两种语义同值，
   直接共享同一个对象（零复制）；非 ASCII 复制字节并把 pub 改成字节数。 */
PyAdamasStr* pa_str_encode(PyAdamasStr* s) {
    if (!s) return str_new_bytes("", 0);
    if (s->pub == s->len) {
        pa_rc_inc(s);
        return s;
    }
    return str_new_bytes(s->data, s->len);
}

/* bytes.decode()：str 语义的副本。全 ASCII 零复制，否则扫描算码点数。
   注意不能拿 pub == len 当「全 ASCII」判据——bytes 的 pub 恒等于 len，
   对非 ASCII 内容同样成立，必须真的检查每个字节。 */
PyAdamasStr* pa_bytes_decode(PyAdamasStr* s) {
    if (!s) return str_new("", 0);
    int all_ascii = 1;
    for (int64_t i = 0; i < s->len; ++i)
        if ((unsigned char)s->data[i] >= 0x80) { all_ascii = 0; break; }
    if (all_ascii) {
        pa_rc_inc(s);
        return s;
    }
    return str_new(s->data, s->len);
}

PyAdamasStr* pa_str_from_i64(int64_t v) {
    // **个位数直接返回驻留单字节串。**
    //
    // str() 的结果里单个数字占比极高（str(i % 10)、str(len(x)) 之类），
    // 而 pa_char_str 对 0x00-0x7F 是 256 项驻留表：零分配、永生、rc_inc/dec
    // 都是空操作。少一次 malloc + 一次 free + 一串 RC 记账。
    // 实测 str_build 每次外层迭代 60 次 str(i%10)，一共 396 万次分配，
    // 占该用例 __memmove/rc_alloc/str_alloc_raw 的相当一部分。
    if ((uint64_t)v < 10u) return pa_char_str(v);
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
    // **用 str_new_ascii 而不是 str_new。**
    //
    // str_new 末尾调 str_seal，它会 str_count_cp 扫一遍字符区数码点数。
    // 而这里产出的必然是纯 ASCII（数字和可选负号），码点数恒等于字节数，
    // 直接 pub = len 就行 —— str_new_ascii 正是这个 O(1) 版本。
    //
    // 实测 str(i) 单次 31.8ns，其中这次多余的扫描占相当比例（字符串短，
    // 但 str_count_cp 的调用 + 分支开销是固定的）。
    return str_new_ascii(p, (int64_t)(end - p));
}

/* unknown 值的 str()：先按 RC 索引确认是不是堆字符串；是就原样返回
   （补一份引用，调用方拿到的是新引用），否则按整数格式化。判定和
   pa_print_any 一致——unknown 里分不开 int 和指针，只能查注册表。 */
PyAdamasStr* pa_str_any(int64_t v) {
    if (v > 4096) {
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v -
                                                (uint64_t)PYADAMAS_RC_HDR);
        if (rc_index_has(h) && h->kind == PYADAMAS_RC_STR) {
            pa_rc_inc((void*)(uintptr_t)v);
            return (PyAdamasStr*)(uintptr_t)v;
        }
    }
    return pa_str_from_i64(v);
}

PyAdamasStr* pa_str_from_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    return str_new(buf, (int64_t)strlen(buf));
}

PyAdamasStr* pa_str_from_bool(int64_t v) {
    return v ? pa_str_true() : pa_str_false();
}

PyAdamasStr* pa_str_from_none(void) { return pa_str_none(); }

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

int64_t pa_str_to_i64(PyAdamasStr* s, int64_t base) {
    if (base != 0 && (base < 2 || base > 36))
        pa_raise_c("ValueError", "int() base must be >= 2 and <= 36");
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
            pa_raise_c("ValueError", "invalid literal for int()");
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
    if (!any || p != end) pa_raise_c("ValueError", "invalid literal for int()");
    return neg ? (int64_t)(0 - acc) : (int64_t)acc;
}

double pa_str_to_f64(PyAdamasStr* s) {
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
    if (bad) pa_raise_c("ValueError", "could not convert string to float");
    return v;
}

int64_t pa_ord(PyAdamasStr* s) {
    if (s->pub != 1)
        pa_raise_c("TypeError", "ord() expected a character, but string length differs");
    int n = 0;
    return (int64_t)str_cp_at(s, 0, &n);
}

PyAdamasStr* pa_chr(int64_t c) {
    /* 码点语义：0..0x10FFFF；代理区无法编成 UTF-8，按 CPython 的
       ValueError 口径拒绝（这是有意的差异，见 compatibility.md）。 */
    if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        pa_raise_c("ValueError", "chr() arg not in range(0x110000)");
    return pa_cp_str((uint32_t)c);
}

PyAdamasStr* pa_int_to_base(int64_t v, int64_t base) {
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

double pa_round_f64(double x, int64_t ndigits) {
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

int64_t pa_round_i64(int64_t v, int64_t ndigits) {
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

int64_t pa_str_eq(PyAdamasStr* a, PyAdamasStr* b) {
    // 同一对象（字面量自比、驻留单字符自比）直接命中。
    if (a == b) return 1;
    const int64_t n = a->len;
    if (n != b->len) return 0;
    // 单字节是最常见的情形：下标取字符后跟单字符字面量比较（s[i] == "0"
    // 这类），一次字节比较就够，连循环都不用建。str_build 里这条路径
    // 每轮迭代命中 60 次。
    if (n == 1) return a->data[0] == b->data[0];
    return sm_bytes_eq(a->data, b->data, n);
}

/* 三路比较，供 `<` / `<=` / `>` / `>=` 用。
   UTF-8 的字节序与码点序一致，所以 memcmp 的结果就是 Python 的结果。 */
int64_t pa_str_cmp(PyAdamasStr* a, PyAdamasStr* b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int c = sm_bytes_cmp(a->data, b->data, n);
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

/* join 的公共填充逻辑（str / bytes 两版共用）。
   返回写入的字节数；码点数由调用方另外累加。

   **两个关键点：**
   1. 空分隔符（"".join 是最常见的写法）要整个跳过拷贝 —— 原来对第 2..n 个
      元素都做一次 memcpy(dst, sep->data, 0)，即 n-1 次纯 PLT 往返。
      str_build 每轮 120 次，是 8.8% memmove 采样的主因。
   2. 元素拷贝走 sm_copy_bytes：单字符元素占绝对多数，内联几次 mov 即可，
      不必每次调 libc。 */
static int64_t sm_join_fill(PyAdamasStr* out, PyAdamasList* parts, PyAdamasStr* sep) {
    char* p = (char*)out->data;
    const int64_t slen = sep->len;
    int64_t pos = 0;
    for (int64_t i = 0; i < parts->len; ++i) {
        if (i > 0 && slen > 0) {
            sm_copy_bytes(p + pos, sep->data, slen);
            pos += slen;
        }
        PyAdamasStr* part = (PyAdamasStr*)parts->data[i];
        sm_copy_bytes(p + pos, part->data, part->len);
        pos += part->len;
    }
    return pos;
}

static void sm_push_str(PyAdamasList* l, PyAdamasStr* s) {
    // str_new 的初始引用直接转交给列表，列表析构时统一 dec。
    // 容量够时直接写槽，避开跨 TU 的 pa_list_append 调用。
    if (l->len < l->cap) {
        l->data[l->len++] = (int64_t)s;
        return;
    }
    pa_list_append(l, (int64_t)s, 2);
}

static int sm_char_in_set(char c, PyAdamasStr* set) {
    for (int64_t i = 0; i < set->len; ++i)
        if (set->data[i] == c) return 1;
    return 0;
}

/* 码点是否在 chars 集合里（按码点比较，不按字节）。 */
static int cp_in_cp_set(uint32_t cp, PyAdamasStr* set) {
    int64_t off = 0;
    while (off < set->len) {
        int n = 0;
        uint32_t c = str_cp_at(set, off, &n);
        if (c == cp) return 1;
        off += n;
    }
    return 0;
}

/* 在 base[0..n) 的 [from, n-m] 区间里找 pat[0..m)。先用 memchr 定位首字节，
   只对首字节命中的位置做整段 memcmp；m==1 时 memchr 的结果就是答案。 */
int64_t pa_find_bytes(const char* base, int64_t n, const char* pat, int64_t m,
                        int64_t from) {
    if (m <= 0) return from <= n ? from : -1;
    if (from < 0) from = 0;
    int64_t last = n - m;
    int64_t i = from;
    while (i <= last) {
        const char* hit = (const char*)memchr(base + i, pat[0], (size_t)(last - i + 1));
        if (!hit) return -1;
        i = (int64_t)(hit - base);
        if (m == 1 || memcmp(hit, pat, (size_t)m) == 0) return i;
        ++i;
    }
    return -1;
}

/* 字节偏移 -> 码点下标。off 必须落在码点边界上（匹配结果天然满足）。 */
static int64_t cp_index_of(const PyAdamasStr* s, int64_t off) {
    if (s->pub == s->len) return off;
    return str_count_cp(s->data, off);
}

/* find/rfind 的 start/end 按 CPython 是码点下标：先夹到 [0, pub]，再换算
   成字节偏移去匹配；匹配到的字节下标再换算回码点下标返回。 */
static int64_t sm_find_from(PyAdamasStr* s, PyAdamasStr* sub, int64_t start, int64_t end) {
    if (start < 0) start += s->pub;
    if (start < 0) start = 0;
    if (end < 0) end += s->pub;
    if (end > s->pub) end = s->pub;
    if (end < 0) end = 0;
    if (sub->len == 0) return start <= end ? start : -1;
    if (start > end) return -1;
    int64_t bstart = str_cp_to_byte(s, start);
    int64_t bend = str_cp_to_byte(s, end);
    int64_t hit = pa_find_bytes(s->data, bend, sub->data, sub->len, bstart);
    if (hit < 0) return -1;
    return cp_index_of(s, hit);
}

/* 单遍 capwords：不建中间词表，直接写进一个输出字符串。
   语义与 string.capwords 一致：按 ASCII 空白切词，词首大写、其余小写。 */
PyAdamasStr* pa_str_capwords(PyAdamasStr* s) {
    PyAdamasStr* out = str_alloc_raw(s->len);
    char* p = (char*)out->data;
    int64_t i = 0;
    int64_t pos = 0;
    int first = 1;
    while (i < s->len) {
        while (i < s->len && sm_is_space(s->data[i])) ++i;
        if (i >= s->len) break;
        if (!first) p[pos++] = ' ';
        first = 0;
        int lead = 1;
        while (i < s->len && !sm_is_space(s->data[i])) {
            char c = s->data[i++];
            if (lead) {
                c = sm_upper(c);
                lead = 0;
            } else {
                c = sm_lower(c);
            }
            p[pos++] = c;
        }
    }
    out->len = pos;
    p[pos] = '\0';
    return str_seal(out);  /* 空白被折叠后码点数会变，扫描算 */
}

/* ── str 方法：码点语义 ────────────────────────────────────────
   字节级的匹配（find/replace/split 的匹配本身）继续按字节做——对合法
   UTF-8，合法 needle 的字节匹配位置必然落在码点边界上（多字节序列的
   首字节不会是 0x80-0xBF）。需要换算的是「对外暴露/接收的下标」与
   「输出串的 pub」。大小写/字符类别仍只认 ASCII（已知差异，见
   compatibility.md）。 */

int64_t pa_str_method(PyAdamasStr* s, int64_t op, int64_t a0, int64_t a1, int64_t a2) {
    switch (op) {
        case 0:   /* upper */
        case 1: { /* lower */
            PyAdamasStr* out = str_alloc_raw(s->len);
            char* p = (char*)out->data;
            for (int64_t i = 0; i < s->len; ++i)
                p[i] = op == 0 ? sm_upper(s->data[i]) : sm_lower(s->data[i]);
            return (int64_t)str_seal_n(out, s->pub);
        }
        case 2:   /* strip */
        case 3:   /* lstrip */
        case 4:   /* rstrip */
        case 5:   /* strip(chars) */
        case 6:   /* lstrip(chars) */
        case 7: { /* rstrip(chars) */
            PyAdamasStr* chars = (PyAdamasStr*)a0;
            int left = op == 2 || op == 3 || op == 5 || op == 6;
            int right = op == 2 || op == 4 || op == 5 || op == 7;
            int64_t b = 0, e = s->len, nb = 0, ne = s->pub;
            /* chars 按码点集合匹配：否则 strip("中") 永远剥不掉多字节字符。
               默认空白仍只认 ASCII 六种（CPython 还认 Unicode 空白，属已知差异）。 */
            if (left) {
                while (b < e) {
                    int n = 0;
                    uint32_t cp = str_cp_at(s, b, &n);
                    if (chars ? cp_in_cp_set(cp, chars) : (cp < 0x80 && sm_is_space((char)cp))) {
                        b += n;
                        ++nb;
                    } else {
                        break;
                    }
                }
            }
            if (right) {
                while (e > b) {
                    int64_t off = str_prev_cp(s, e);
                    int n = 0;
                    uint32_t cp = str_cp_at(s, off, &n);
                    if (chars ? cp_in_cp_set(cp, chars) : (cp < 0x80 && sm_is_space((char)cp))) {
                        e = off;
                        --ne;
                    } else {
                        break;
                    }
                }
            }
            /* 没有可剥离的字符时按 CPython 的做法直接共享原串。 */
            if (b == 0 && e == s->len) {
                pa_rc_inc(s);
                return (int64_t)s;
            }
            PyAdamasStr* out = str_alloc_raw(e - b);
            if (e > b) memcpy((char*)out->data, s->data + b, (size_t)(e - b));
            return (int64_t)str_seal_n(out, ne - nb);
        }
        case 8: { /* split() 按空白 */
            // 先数一遍词数，按精确容量建表，避免 append 过程中的 realloc。
            int64_t count = 0;
            int64_t i = 0;
            while (i < s->len) {
                while (i < s->len && sm_is_space(s->data[i])) ++i;
                if (i >= s->len) break;
                ++count;
                while (i < s->len && !sm_is_space(s->data[i])) ++i;
            }
            PyAdamasList* out = pa_list_new(count, 1);
            // 循环里每个 str_new 都是一次 GC 分配点，out 只在这个 C 局部
            // 变量里 —— RAII 守卫挂根，不再手工配对 unpin。
            PA_PIN_SCOPE(out);
            i = 0;
            while (i < s->len) {
                while (i < s->len && sm_is_space(s->data[i])) ++i;
                if (i >= s->len) break;
                int64_t b = i;
                while (i < s->len && !sm_is_space(s->data[i])) ++i;
                sm_push_str(out, str_new(s->data + b, i - b));  /* str_new 扫描算 pub */
            }
            PA_PIN_RELEASE(out);
            return (int64_t)out;
        }
        case 9: { /* split(sep, maxsplit) */
            PyAdamasStr* sep = (PyAdamasStr*)a0;
            int64_t maxsplit = a1;
            if (sep->len == 0) pa_raise_c("ValueError", "empty separator");
            // 短分隔符走 memchr；单遍填充，容量从 8 起（常见 2-6 段不 realloc）。
            PyAdamasList* out = pa_list_new(8, 1);
            // 同 split() 按空白：str_new 在循环里触发主收集时 out 必须可见。
            PA_PIN_SCOPE(out);
            int64_t i = 0;
            int64_t done = 0;
            while (i <= s->len) {
                if (maxsplit >= 0 && done >= maxsplit) break;
                int64_t j = pa_find_bytes(s->data, s->len, sep->data, sep->len, i);
                if (j < 0) break;
                sm_push_str(out, str_new(s->data + i, j - i));
                i = j + sep->len;
                ++done;
            }
            sm_push_str(out, str_new(s->data + i, s->len - i));
            PA_PIN_RELEASE(out);
            return (int64_t)out;
        }
        case 10: { /* join(parts) */
            PyAdamasList* parts = (PyAdamasList*)a0;
            /* 空分隔符拼单个元素等价于原串；CPython 也返回同一对象。 */
            if (parts->len == 1 && s->len == 0) {
                PyAdamasStr* only = (PyAdamasStr*)parts->data[0];
                pa_rc_inc(only);
                return (int64_t)only;
            }
            int64_t total = 0, total_cp = 0;
            for (int64_t i = 0; i < parts->len; ++i) {
                PyAdamasStr* part = (PyAdamasStr*)parts->data[i];
                total += part->len;
                total_cp += part->pub;
            }
            if (parts->len > 1) {
                total += s->len * (parts->len - 1);
                total_cp += s->pub * (parts->len - 1);
            }
            PyAdamasStr* out = str_alloc_raw(total);
            sm_join_fill(out, parts, s);
            return (int64_t)str_seal_n(out, total_cp);
        }
        case 11: { /* replace(old, new, count) */
            PyAdamasStr* old = (PyAdamasStr*)a0;
            PyAdamasStr* nw = (PyAdamasStr*)a1;
            int64_t count = a2;
            // 先数出现次数（空 old 特殊：在每对码点之间插入，所以是 pub+1 个位置）
            int64_t hits = 0;
            if (old->len == 0) {
                hits = s->pub + 1;
                if (count >= 0 && hits > count) hits = count;
            } else {
                int64_t i = 0;
                while (i + old->len <= s->len) {
                    int64_t j = pa_find_bytes(s->data, s->len, old->data,
                                                old->len, i);
                    if (j < 0) break;
                    ++hits;
                    if (count >= 0 && hits >= count) break;
                    i = j + old->len;
                }
            }
            int64_t newlen = s->len + hits * (nw->len - old->len);
            if (old->len == 0 && s->len > 0) newlen = s->len + hits * nw->len;
            /* 码点数：非空 old 时命中次数与码点数无关，直接算；空 old 是逐码点
               插入，码点数 = s->pub + hits×nw->pub。 */
            int64_t newcp;
            if (old->len == 0) newcp = s->pub + hits * nw->pub;
            else newcp = s->pub + hits * (nw->pub - old->pub);
            PyAdamasStr* out = str_alloc_raw(newlen);
            char* p = (char*)out->data;
            int64_t pos = 0, used = 0, i = 0;
            if (old->len == 0) {
                /* 空 old：在每对码点之间插入 new（以前是逐字节，会把多字节
                   序列切开）。 */
                int64_t off = 0;
                while (off < s->len) {
                    if (used < hits) {
                        memcpy(p + pos, nw->data, (size_t)nw->len);
                        pos += nw->len;
                        ++used;
                    }
                    int n = str_seq_len((unsigned char)s->data[off]);
                    memcpy(p + pos, s->data + off, (size_t)n);
                    pos += n;
                    off += n;
                }
                if (used < hits) {
                    memcpy(p + pos, nw->data, (size_t)nw->len);
                    pos += nw->len;
                    ++used;
                }
            } else {
                while (i < s->len) {
                    if (used >= hits || i + old->len > s->len) {
                        memcpy(p + pos, s->data + i, (size_t)(s->len - i));
                        pos += s->len - i;
                        break;
                    }
                    int64_t j = pa_find_bytes(s->data, s->len, old->data,
                                                old->len, i);
                    if (j < 0) {
                        memcpy(p + pos, s->data + i, (size_t)(s->len - i));
                        pos += s->len - i;
                        break;
                    }
                    if (j > i) {
                        memcpy(p + pos, s->data + i, (size_t)(j - i));
                        pos += j - i;
                        i = j;
                    }
                    if (memcmp(s->data + i, old->data, (size_t)old->len) == 0) {
                        memcpy(p + pos, nw->data, (size_t)nw->len);
                        pos += nw->len;
                        i += old->len;
                        ++used;
                    } else {
                        p[pos++] = s->data[i++];
                    }
                }
            }
            return (int64_t)str_seal_n(out, newcp);
        }
        case 12: { /* find(sub, start, end) */
            return sm_find_from(s, (PyAdamasStr*)a0, a1, a2);
        }
        case 13: { /* rfind */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
            int64_t start = a1, end = a2;
            if (start < 0) start += s->pub;
            if (start < 0) start = 0;
            if (end < 0) end += s->pub;
            if (end > s->pub) end = s->pub;
            if (end < 0) end = 0;
            if (sub->len == 0) return end >= start ? end : -1;
            if (start > end) return -1;
            int64_t bstart = str_cp_to_byte(s, start);
            int64_t bend = str_cp_to_byte(s, end);
            for (int64_t i = bend - sub->len; i >= bstart; --i) {
                /* 起点必须落在码点边界上，否则会从续字节里开始比。 */
                if (((unsigned char)s->data[i] & 0xC0) == 0x80) continue;
                if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0)
                    return cp_index_of(s, i);
            }
            return -1;
        }
        case 14:   /* startswith */
        case 15: { /* endswith */
            PyAdamasStr* affix = (PyAdamasStr*)a0;
            if (affix->len > s->len) return 0;
            if (op == 14)
                return memcmp(s->data, affix->data, (size_t)affix->len) == 0 ? 1 : 0;
            return memcmp(s->data + s->len - affix->len, affix->data,
                          (size_t)affix->len) == 0
                       ? 1
                       : 0;
        }
        case 16: { /* count(sub) */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
            if (sub->len == 0) return s->pub + 1;
            int64_t n = 0, i = 0;
            for (;;) {
                int64_t j = pa_find_bytes(s->data, s->len, sub->data, sub->len, i);
                if (j < 0) break;
                ++n;
                i = j + sub->len;
            }
            return n;
        }
        case 17: { /* zfill(width) */
            int64_t width = a0;
            if (width <= s->pub) return (int64_t)str_new(s->data, s->len);
            int64_t pad = width - s->pub;
            PyAdamasStr* out = str_alloc_raw(s->len + pad);
            char* p = (char*)out->data;
            int64_t pos = 0;
            int64_t start = 0;
            if (s->len > 0 && (s->data[0] == '+' || s->data[0] == '-')) {
                p[pos++] = s->data[0];
                start = 1;
            }
            for (int64_t i = 0; i < pad; ++i) p[pos++] = '0';
            memcpy(p + pos, s->data + start, (size_t)(s->len - start));
            return (int64_t)str_seal_n(out, width);
        }
        case 18:   /* ljust(width, fill) */
        case 19:   /* rjust */
        case 20: { /* center */
            int64_t width = a0;
            PyAdamasStr* fill = (PyAdamasStr*)a1;
            char fbuf[4];
            int flen = 1;
            fbuf[0] = ' ';
            if (fill) {
                if (fill->pub != 1)
                    pa_raise_c("TypeError", "The fill character must be exactly one character long");
                flen = 0;
                uint32_t cp = str_cp_at(fill, 0, &flen);
                str_cp_encode(cp, fbuf);
            }
            if (width <= s->pub) return (int64_t)str_new(s->data, s->len);
            int64_t pad = width - s->pub;
            int64_t left = op == 18 ? 0 : op == 19 ? pad : pad / 2;
            int64_t right = pad - left;
            PyAdamasStr* out = str_alloc_raw(s->len + (int64_t)(left + right) * flen);
            char* p = (char*)out->data;
            int64_t pos = 0;
            for (int64_t i = 0; i < left; ++i) {
                memcpy(p + pos, fbuf, (size_t)flen);
                pos += flen;
            }
            memcpy(p + pos, s->data, (size_t)s->len);
            pos += s->len;
            for (int64_t i = 0; i < right; ++i) {
                memcpy(p + pos, fbuf, (size_t)flen);
                pos += flen;
            }
            return (int64_t)str_seal_n(out, width);
        }
        case 21: { /* capitalize */
            /* 只有 ASCII 字节会被改大小写；多字节序列的续字节 sm_upper 原样返回，
               所以按字节循环不会破坏 UTF-8。pub 不变。 */
            PyAdamasStr* out = str_alloc_raw(s->len);
            char* p = (char*)out->data;
            for (int64_t i = 0; i < s->len; ++i)
                p[i] = i == 0 ? sm_upper(s->data[i]) : sm_lower(s->data[i]);
            return (int64_t)str_seal_n(out, s->pub);
        }
        case 22: { /* title */
            /* 必须逐码点判词边界：按字节时非 ASCII 的续字节会被当成「非字母」
               反复重置词边界，多字节字符会被拆着改。 */
            PyAdamasStr* out = str_alloc_raw(s->len);
            char* p = (char*)out->data;
            int word = 1;
            int64_t off = 0;
            while (off < s->len) {
                int n = str_seq_len((unsigned char)s->data[off]);
                if (n == 1) {
                    char c = s->data[off];
                    p[off] = word ? sm_upper(c) : sm_lower(c);
                    word = !sm_is_alpha(c);
                } else {
                    memcpy(p + off, s->data + off, (size_t)n);
                    word = 0;  /* 非 ASCII 码点按「非字母」处理（已知差异） */
                }
                off += n;
            }
            return (int64_t)str_seal_n(out, s->pub);
        }
        case 23:   /* isdigit */
        case 24:   /* isalpha */
        case 25:   /* isspace */
        case 26:   /* isupper */
        case 27:   /* islower */
        case 28: { /* isalnum */
            if (s->pub == 0) return 0;
            int cased = 0;
            int64_t off = 0;
            while (off < s->len) {
                int n = 0;
                uint32_t cp = str_cp_at(s, off, &n);
                /* 非 ASCII 码点按「非字母/非数字/非空白」处理（已知差异，
                   见 compatibility.md）；对 isupper/islower 它不影响 cased。 */
                char c = cp < 0x80 ? (char)cp : (char)1;
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
                off += n;
            }
            if ((op == 26 || op == 27) && cased == 0) return 0;
            return 1;
        }
        case 29:   /* removeprefix */
        case 30: { /* removesuffix */
            PyAdamasStr* affix = (PyAdamasStr*)a0;
            if (affix->len > s->len) return (int64_t)str_new(s->data, s->len);
            if (op == 29) {
                if (memcmp(s->data, affix->data, (size_t)affix->len) != 0)
                    return (int64_t)str_new(s->data, s->len);
                PyAdamasStr* out = str_alloc_raw(s->len - affix->len);
                if (s->len > affix->len)
                    memcpy((char*)out->data, s->data + affix->len,
                           (size_t)(s->len - affix->len));
                return (int64_t)str_seal_n(out, s->pub - affix->pub);
            }
            if (memcmp(s->data + s->len - affix->len, affix->data,
                       (size_t)affix->len) != 0)
                return (int64_t)str_new(s->data, s->len);
            PyAdamasStr* out = str_alloc_raw(s->len - affix->len);
            if (s->len > affix->len)
                memcpy((char*)out->data, s->data, (size_t)(s->len - affix->len));
            return (int64_t)str_seal_n(out, s->pub - affix->pub);
        }
        case 31: { /* splitlines */
            PyAdamasList* out = pa_list_new(4, 1);
            // 每个换行点一次 str_new：out 只在本帧 C 局部里，用 RAII 挂根。
            PA_PIN_SCOPE(out);
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
            PA_PIN_RELEASE(out);
            return (int64_t)out;
        }
        case 32: /* encode：str -> bytes 的真实转换（S2） */
            return (int64_t)pa_str_encode(s);
        default:
            pa_raise_c("ValueError", "unknown string method");
    }
    return 0;
}

/* ── bytes 方法：字节语义（与 str 分家） ────────────────────────
   bytes 的 len/索引/切片/方法全部按字节，与改动前逐字节一致；pub 恒等于
   字节数。大小写/字符类别本来就只有 ASCII 映射，字节版与码点版在 ASCII
   上等价，但 strip(chars)/find 下标/宽度这些必须走字节口径。 */
int64_t pa_bytes_method(PyAdamasStr* s, int64_t op, int64_t a0, int64_t a1, int64_t a2) {
    switch (op) {
        case 0:   /* upper */
        case 1: { /* lower */
            PyAdamasStr* out = str_alloc_raw(s->len);
            char* p = (char*)out->data;
            for (int64_t i = 0; i < s->len; ++i)
                p[i] = op == 0 ? sm_upper(s->data[i]) : sm_lower(s->data[i]);
            return (int64_t)str_seal_n(out, s->len);
        }
        case 2:   /* strip */
        case 3:   /* lstrip */
        case 4:   /* rstrip */
        case 5:   /* strip(chars) */
        case 6:   /* lstrip(chars) */
        case 7: { /* rstrip(chars) */
            PyAdamasStr* chars = (PyAdamasStr*)a0;
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
            if (b == 0 && e == s->len) {
                pa_rc_inc(s);
                return (int64_t)s;
            }
            return (int64_t)str_new_bytes(s->data + b, e - b);
        }
        case 8: { /* split() 按空白 */
            int64_t count = 0;
            int64_t i = 0;
            while (i < s->len) {
                while (i < s->len && sm_is_space(s->data[i])) ++i;
                if (i >= s->len) break;
                ++count;
                while (i < s->len && !sm_is_space(s->data[i])) ++i;
            }
            PyAdamasList* out = pa_list_new(count, 1);
            PA_PIN_SCOPE(out);
            i = 0;
            while (i < s->len) {
                while (i < s->len && sm_is_space(s->data[i])) ++i;
                if (i >= s->len) break;
                int64_t b = i;
                while (i < s->len && !sm_is_space(s->data[i])) ++i;
                sm_push_str(out, str_new_bytes(s->data + b, i - b));
            }
            PA_PIN_RELEASE(out);
            return (int64_t)out;
        }
        case 9: { /* split(sep, maxsplit) */
            PyAdamasStr* sep = (PyAdamasStr*)a0;
            int64_t maxsplit = a1;
            if (sep->len == 0) pa_raise_c("ValueError", "empty separator");
            PyAdamasList* out = pa_list_new(8, 1);
            PA_PIN_SCOPE(out);
            int64_t i = 0;
            int64_t done = 0;
            while (i <= s->len) {
                if (maxsplit >= 0 && done >= maxsplit) break;
                int64_t j = pa_find_bytes(s->data, s->len, sep->data, sep->len, i);
                if (j < 0) break;
                sm_push_str(out, str_new_bytes(s->data + i, j - i));
                i = j + sep->len;
                ++done;
            }
            sm_push_str(out, str_new_bytes(s->data + i, s->len - i));
            PA_PIN_RELEASE(out);
            return (int64_t)out;
        }
        case 10: { /* join(parts) */
            PyAdamasList* parts = (PyAdamasList*)a0;
            if (parts->len == 1 && s->len == 0) {
                PyAdamasStr* only = (PyAdamasStr*)parts->data[0];
                pa_rc_inc(only);
                return (int64_t)only;
            }
            int64_t total = 0;
            for (int64_t i = 0; i < parts->len; ++i)
                total += ((PyAdamasStr*)parts->data[i])->len;
            if (parts->len > 1) total += s->len * (parts->len - 1);
            PyAdamasStr* out = str_alloc_raw(total);
            int64_t pos = sm_join_fill(out, parts, s);
            return (int64_t)str_seal_n(out, pos);
        }
        case 11: { /* replace(old, new, count) */
            PyAdamasStr* old = (PyAdamasStr*)a0;
            PyAdamasStr* nw = (PyAdamasStr*)a1;
            int64_t count = a2;
            int64_t hits = 0;
            if (old->len == 0) {
                hits = s->len + 1;
                if (count >= 0 && hits > count) hits = count;
            } else {
                int64_t i = 0;
                while (i + old->len <= s->len) {
                    int64_t j = pa_find_bytes(s->data, s->len, old->data, old->len, i);
                    if (j < 0) break;
                    ++hits;
                    if (count >= 0 && hits >= count) break;
                    i = j + old->len;
                }
            }
            int64_t newlen = s->len + hits * (nw->len - old->len);
            if (old->len == 0 && s->len > 0) newlen = s->len + hits * nw->len;
            PyAdamasStr* out = str_alloc_raw(newlen);
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
                    if (used >= hits || i + old->len > s->len) {
                        memcpy(p + pos, s->data + i, (size_t)(s->len - i));
                        pos += s->len - i;
                        break;
                    }
                    int64_t j = pa_find_bytes(s->data, s->len, old->data, old->len, i);
                    if (j < 0) {
                        memcpy(p + pos, s->data + i, (size_t)(s->len - i));
                        pos += s->len - i;
                        break;
                    }
                    if (j > i) {
                        memcpy(p + pos, s->data + i, (size_t)(j - i));
                        pos += j - i;
                        i = j;
                    }
                    if (memcmp(s->data + i, old->data, (size_t)old->len) == 0) {
                        memcpy(p + pos, nw->data, (size_t)nw->len);
                        pos += nw->len;
                        i += old->len;
                        ++used;
                    } else {
                        p[pos++] = s->data[i++];
                    }
                }
            }
            return (int64_t)str_seal_n(out, pos);
        }
        case 12: { /* find(sub, start, end)：字节下标 */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
            int64_t start = a1, end = a2;
            if (start < 0) start += s->len;
            if (start < 0) start = 0;
            if (end < 0) end += s->len;
            if (end > s->len) end = s->len;
            if (end < 0) end = 0;
            if (sub->len == 0) return start <= end ? start : -1;
            return pa_find_bytes(s->data, end, sub->data, sub->len, start);
        }
        case 13: { /* rfind：字节下标 */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
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
            PyAdamasStr* affix = (PyAdamasStr*)a0;
            if (affix->len > s->len) return 0;
            if (op == 14)
                return memcmp(s->data, affix->data, (size_t)affix->len) == 0 ? 1 : 0;
            return memcmp(s->data + s->len - affix->len, affix->data,
                          (size_t)affix->len) == 0
                       ? 1
                       : 0;
        }
        case 16: { /* count(sub) */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
            if (sub->len == 0) return s->len + 1;
            int64_t n = 0, i = 0;
            for (;;) {
                int64_t j = pa_find_bytes(s->data, s->len, sub->data, sub->len, i);
                if (j < 0) break;
                ++n;
                i = j + sub->len;
            }
            return n;
        }
        case 31: { /* splitlines */
            PyAdamasList* out = pa_list_new(4, 1);
            PA_PIN_SCOPE(out);
            int64_t b = 0, i = 0;
            while (i < s->len) {
                char c = s->data[i];
                if (c == '\n' || c == '\r') {
                    sm_push_str(out, str_new_bytes(s->data + b, i - b));
                    if (c == '\r' && i + 1 < s->len && s->data[i + 1] == '\n') ++i;
                    ++i;
                    b = i;
                } else {
                    ++i;
                }
            }
            if (b < s->len) sm_push_str(out, str_new_bytes(s->data + b, s->len - b));
            PA_PIN_RELEASE(out);
            return (int64_t)out;
        }
        case 32: /* decode：bytes -> str 的真实转换（S2） */
            return (int64_t)pa_bytes_decode(s);
        default:
            pa_raise_c("ValueError", "unknown bytes method");
    }
    return 0;
}
