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

/* ── unknown 值的通用 repr（异形容器打印用）──────────────────────
   unknown 里分不开 int/float，只能按堆探针分派：堆对象按真实种类递归
   （str 带引号、list/dict 递归），非堆值一律按 int。这是 0.317 记录过的
   固有边界（没有运行期类型标签）；对常见的「int/str 混合」已足够。 */
typedef struct {
    char* p;
    size_t n, cap;
} AnyReprBuf;

static void ar_put(AnyReprBuf* b, const char* p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (b->n + n + 1 > cap) cap *= 2;
        char* q = (char*)realloc(b->p, cap);
        if (!q) panic_c("out of memory");
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void ar_cstr(AnyReprBuf* b, const char* s) { ar_put(b, s, strlen(s)); }

static void repr_any_into(AnyReprBuf* b, int64_t v);
static void repr_any_bytes_into(AnyReprBuf* b, PyAdamasStr* s);

/* dict 的 unknown repr：{key: value, ...}。键值都可能又是容器。 */
static void repr_any_dict_into(AnyReprBuf* b, PyAdamasDict* d) {
    ar_cstr(b, "{");
    for (int64_t i = 0; i < d->len; ++i) {
        if (i) ar_cstr(b, ", ");
        repr_any_into(b, pa_dict_key_at(d, i));
        ar_cstr(b, ": ");
        repr_any_into(b, pa_dict_val_at(d, i));
    }
    ar_cstr(b, "}");
}

static void repr_any_into(AnyReprBuf* b, int64_t v) {
    if (!pa_unknown_is_heap(v)) {
        PyAdamasStr* s = pa_str_from_i64(v);
        ar_put(b, s->data, (size_t)s->len);
        pa_rc_dec(s);
        return;
    }
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    switch (h->kind) {
        case PYADAMAS_RC_STR: {
            PyAdamasStr* s = pa_repr_str((PyAdamasStr*)(uintptr_t)v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
        case PYADAMAS_RC_BIGINT: {
            PyAdamasStr* s = pa_big_str((PyAdamasBig*)(uintptr_t)v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
        case PYADAMAS_RC_LIST: {
            PyAdamasList* l = (PyAdamasList*)(uintptr_t)v;
            ar_cstr(b, "[");
            for (int64_t i = 0; i < l->len; ++i) {
                if (i) ar_cstr(b, ", ");
                repr_any_into(b, l->data[i]);
            }
            ar_cstr(b, "]");
            return;
        }
        case PYADAMAS_RC_DICT:
            repr_any_dict_into(b, (PyAdamasDict*)(uintptr_t)v);
            return;
        case PYADAMAS_RC_CPYFLOAT: {
            double d;
            memcpy(&d, (void*)(uintptr_t)v, sizeof d);
            char tmp[64];
            format_double(tmp, sizeof tmp, d);
            ar_cstr(b, tmp);
            return;
        }
        /* 动态 JSON 的 null/bool 盒：repr 按 Python 值语义印（None/True/False），
           和裸 unknown 域里 None/bool 退化成 int 的旧边界不同 —— 它们有盒，
           能精确还原。 */
        case PYADAMAS_RC_JSONNULL:
            ar_cstr(b, "None");
            return;
        case PYADAMAS_RC_JSONBOOL:
            ar_cstr(b, *(int64_t*)(uintptr_t)v ? "True" : "False");
            return;
        /* BUG-3：类实例（异构容器里的元素）。以前落到 default 按机器字印
           指针 —— 调试时看到一串数字。这里按 repr 语义调用户实现的
           __repr__；没有实现退回 <Name object>（与静态路径同形）。 */
        case PYADAMAS_RC_INSTANCE: {
            PyAdamasStr* s = pa_instance_str((void*)(uintptr_t)v, 1);
            if (!s) s = pa_instance_default_str((void*)(uintptr_t)v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
        default: {
            /* P4（lang_gaps）：类对象（异常类当值）→ <class '...'>。
               门控关时类对象值不存在，探针不会命中魔数。 */
            if (pa_is_class_obj((void*)(uintptr_t)v)) {
                PyAdamasStr* s = pa_class_repr((void*)(uintptr_t)v);
                ar_put(b, s->data, (size_t)s->len);
                pa_rc_dec(s);
                return;
            }
            /* P4 块3（lang_gaps 门控开）：异常值过 unknown 的 repr →
               CPython 口径 Name('arg', ...)。门控关逐字旧样（印机器字）。 */
            if (pa_lang_gaps_rt && h->kind == PYADAMAS_RC_OBJ_EXCVALUE &&
                ((PyAdamasExcValue*)(uintptr_t)v)->magic == PYADAMAS_EXCVAL_MAGIC) {
                PyAdamasExcValue* ev = (PyAdamasExcValue*)(uintptr_t)v;
                ar_cstr(b, ev->type);
                ar_cstr(b, "(");
                int64_t n = ev->args ? ev->args->len : 0;
                /* 单空串 args 按无参印（raise E() 的 pa 表示是空消息串，
                   与 CPython 无参构造同形；E("") 的 repr 差异文档化）。 */
                if (n == 1) {
                    PyAdamasStr* a0 = (PyAdamasStr*)(uintptr_t)ev->args->data[0];
                    if (a0 && a0->len == 0) n = 0;
                }
                for (int64_t i = 0; i < n; ++i) {
                    if (i) ar_cstr(b, ", ");
                    PyAdamasStr* a = (PyAdamasStr*)(uintptr_t)ev->args->data[i];
                    PyAdamasStr* r = pa_repr_str(a);
                    ar_put(b, r->data, (size_t)r->len);
                    pa_rc_dec(r);
                }
                ar_cstr(b, ")");
                return;
            }
            PyAdamasStr* s = pa_str_from_i64(v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
    }
}

PyAdamasStr* pa_repr_any(int64_t v) {
    AnyReprBuf b = {NULL, 0, 0};
    ar_cstr(&b, "");
    repr_any_into(&b, v);
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return out;
}

/* P4 块4（lang_gaps）：静态 bytes 的 repr。bytes/str 同运行期表示（无标志
   位），探针口径分不出，静态已知是 bytes 时走这里（b'...' + \xNN 转义，
   CPython 口径；与带 tag 路径的 repr_any_bytes_into 同一实现）。 */
PyAdamasStr* pa_repr_bytes(void* s) {
    AnyReprBuf b = {NULL, 0, 0};
    repr_any_bytes_into(&b, (PyAdamasStr*)s);
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return out;
}

/* P4 块4（lang_gaps）：静态 set 的 repr。set 的运行期表示是 dict（值恒 0），
   探针口径（repr_any_into）分不出 set 与 {k: 0} 字典，会印成 {1: 0, ...}；
   静态类型已知是 set 时走这里：按键迭代、元素按 unknown 探针 repr，
   空集印 set()（CPython 口径）。元素是嵌套 set 时仍按字典印（探针边界，
   与 P2 容器元素无 tag 口径一致）。 */
PyAdamasStr* pa_repr_set(void* s) {
    AnyReprBuf b = {NULL, 0, 0};
    PyAdamasDict* d = (PyAdamasDict*)s;
    if (!d || d->len == 0) {
        ar_cstr(&b, "set()");
    } else {
        ar_cstr(&b, "{");
        for (int64_t i = 0; i < d->len; ++i) {
            if (i) ar_cstr(&b, ", ");
            repr_any_into(&b, pa_dict_key_at(d, i));
        }
        ar_cstr(&b, "}");
    }
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return out;
}


/* ══════════════════════════════════════════════════════════════════
   P2（0.340）：带 tag 的 unknown repr（docs/numeric-types-design.md §2）
   与 repr_any_into 的探针路径并存：有精确 tag 时按 tag 分派（None/True/
   False/bytes/tuple/set/类对象都能印对），UNK 仍回退旧探针路径。
   嵌套元素没有 tag 来源（容器只存 8B 值），仍按探针口径 —— 文档化边界。
   ══════════════════════════════════════════════════════════════════ */

/* bytes 的 repr：CPython 口径 b'...'（可打印 ASCII 原样，\t\n\r 用转义，
   单引号/反斜杠转义，其余字节 \xNN）。 */
static void repr_any_bytes_into(AnyReprBuf* b, PyAdamasStr* s) {
    /* CPython 的定界符选择：内容含单引号且不含双引号时用双引号，否则用
       单引号（内部单引号转义）。 */
    const unsigned char* p = (const unsigned char*)s->data;
    int has_single = 0, has_double = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        if (p[i] == '\'') has_single = 1;
        else if (p[i] == '"') has_double = 1;
    }
    const char quote = (has_single && !has_double) ? '"' : '\'';
    ar_cstr(b, "b");
    ar_put(b, &quote, 1);
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = p[i];
        if (c == '\\') { ar_cstr(b, "\\\\"); continue; }
        if (c == (unsigned char)quote) {
            ar_cstr(b, quote == '"' ? "\\\"" : "\\'");
            continue;
        }
        if (c == '\t') { ar_cstr(b, "\\t"); continue; }
        if (c == '\n') { ar_cstr(b, "\\n"); continue; }
        if (c == '\r') { ar_cstr(b, "\\r"); continue; }
        if (c >= 0x20 && c < 0x7f) { ar_put(b, (const char*)&p[i], 1); continue; }
        char tmp[8];
        snprintf(tmp, sizeof tmp, "\\x%02x", (unsigned)c);
        ar_cstr(b, tmp);
    }
    ar_put(b, &quote, 1);
}

/* tuple 的 repr：与 pa_repr_tuple 同口径（单元素带尾逗号），元素按未知
   值探针渲染（容器里没有逐元素 tag）。 */
static void repr_any_tuple_into(AnyReprBuf* b, PyAdamasList* t) {
    ar_cstr(b, "(");
    for (int64_t i = 0; i < t->len; ++i) {
        if (i) ar_cstr(b, ", ");
        repr_any_into(b, t->data[i]);
    }
    if (t->len == 1) ar_cstr(b, ",");
    ar_cstr(b, ")");
}

/* set 的 repr：set 复用 dict（值恒 0），按键迭代；空集是 "set()"。 */
static void repr_any_set_into(AnyReprBuf* b, PyAdamasDict* d) {
    if (d->len == 0) { ar_cstr(b, "set()"); return; }
    ar_cstr(b, "{");
    for (int64_t i = 0; i < d->len; ++i) {
        if (i) ar_cstr(b, ", ");
        repr_any_into(b, pa_dict_key_at(d, i));
    }
    ar_cstr(b, "}");
}

static void repr_any_tag_into(AnyReprBuf* b, int64_t v, int64_t tag) {
    switch (tag) {
        case PA_TAG_NONE: ar_cstr(b, "None"); return;
        case PA_TAG_BOOL: ar_cstr(b, v ? "True" : "False"); return;
        case PA_TAG_INT: {
            PyAdamasStr* s = pa_str_from_i64(v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
        case PA_TAG_FRAW: {
            double d;
            memcpy(&d, &v, sizeof d);
            char tmp[64];
            format_double(tmp, sizeof tmp, d);
            ar_cstr(b, tmp);
            return;
        }
        case PA_TAG_FBOX: {
            double d;
            memcpy(&d, (void*)(uintptr_t)v, sizeof d);
            char tmp[64];
            format_double(tmp, sizeof tmp, d);
            ar_cstr(b, tmp);
            return;
        }
        case PA_TAG_STR: {
            PyAdamasStr* s = pa_repr_str((PyAdamasStr*)(uintptr_t)v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
        case PA_TAG_BYTES:
            repr_any_bytes_into(b, (PyAdamasStr*)(uintptr_t)v);
            return;
        case PA_TAG_TUPLE:
            repr_any_tuple_into(b, (PyAdamasList*)(uintptr_t)v);
            return;
        case PA_TAG_SET:
            repr_any_set_into(b, (PyAdamasDict*)(uintptr_t)v);
            return;
        case PA_TAG_CLASS: {
            PyAdamasStr* s = pa_class_repr((void*)(uintptr_t)v);
            ar_put(b, s->data, (size_t)s->len);
            pa_rc_dec(s);
            return;
        }
        default:
            /* HEAP / UNK：头 kind 分派（str 带引号、list/dict 递归、
               CPYFLOAT 按 double；其余按旧口径）。 */
            repr_any_into(b, v);
            return;
    }
}

PyAdamasStr* pa_repr_any_t(int64_t v, int64_t tag) {
    AnyReprBuf b = {NULL, 0, 0};
    ar_cstr(&b, "");
    repr_any_tag_into(&b, v, tag);
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return out;
}

void pa_print_any_repr_t(int64_t v, int64_t tag) {
    PyAdamasStr* s = pa_repr_any_t(v, tag);
    pa_print_str(s);
    pa_rc_dec(s);
}
PyAdamasStr* pa_repr_dict_any(PyAdamasDict* d) {
    AnyReprBuf b = {NULL, 0, 0};
    ar_cstr(&b, "");
    repr_any_dict_into(&b, d);
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return out;
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
    return pa_cp_str(cp);
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
    /* 非 ASCII 单码点走 BMP 驻留表（首次分配、之后零分配），
       和 ASCII 路径一样返回永生对象，调用方的 rc_dec 是空操作。 */
    return pa_cp_str(cp);
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

/* 两位十进制查表（"00".."99" 的连续排布）：一次写两位，除法次数减半。 */
static const char pa_dec2[] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

/* 十进制位数（1..20）：逐级缩小量级再把缩小倍数加回去，比逐个比较幂次
   少了近一半分支，且大部分数值在前几级就退完。 */
static inline int u64_dec_digits(uint64_t m) {
    int n = 1;
    if (m >= 10000000000ull) { m /= 10000000000ull; n += 10; }
    if (m >= 100000) { m /= 100000; n += 5; }
    if (m >= 1000) { m /= 1000; n += 3; }
    if (m >= 100) { m /= 100; n += 2; }
    if (m >= 10) n += 1;
    return n;
}

/* 把 mag 的十进制数字倒着写进以 end 结尾的缓冲区（end 本身不写），
   返回到写入区间的起点。 */
static inline char* u64_write_dec(char* end, uint64_t mag) {
    char* p = end;
    if (mag < 10u) {
        *--p = (char)('0' + (int)mag);
        return p;
    }
    do {
        unsigned r = (unsigned)(mag % 100u);
        mag /= 100u;
        p -= 2;
        p[0] = pa_dec2[r * 2];
        p[1] = pa_dec2[r * 2 + 1];
    } while (mag >= 100u);
    if (mag >= 10u) {
        p -= 2;
        p[0] = pa_dec2[(unsigned)mag * 2];
        p[1] = pa_dec2[(unsigned)mag * 2 + 1];
    } else if (mag) {
        *--p = (char)('0' + (int)mag);
    }
    return p;
}

/* 十进制渲染到调用方缓冲（不分配）：print() 直接 fwrite 用，返回字节数。 */
int64_t pa_i64_to_buf(char* dst, int64_t v) {
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int len = u64_dec_digits(mag) + (v < 0);
    char* p = u64_write_dec(dst + len, mag);
    if (v < 0) *--p = '-';
    return len;
}

int64_t pa_u64_to_buf(char* dst, uint64_t v) {
    int len = u64_dec_digits(v);
    u64_write_dec(dst + len, v);
    return len;
}

PyAdamasStr* pa_str_from_i64(int64_t v) {
    // **个位数直接返回驻留单字节串。**
    //
    // str() 的结果里单个数字占比极高（str(i % 10)、str(len(x)) 之类），
    // 而 pa_char_str 对 0x00-0x7F 是 256 项驻留表：零分配、永生、rc_inc/dec
    // 都是空操作。少一次 malloc + 一次 free + 一串 RC 记账。
    // 实测 str_build 每次外层迭代 60 次 str(i%10)，一共 396 万次分配，
    // 占该用例 __memmove/rc_alloc/str_alloc_raw 的相当一部分。
    // 注意 pa_char_str 收的是**字符编码**不是数值：str(7) 要的是 '7'（0x37），
    // 传 7 会拿到 BEL 控制字符。（这个 bug 差分测试没抓到 —— 那 187 个用例
    // 里没有 str(单个数字) 后再比较/打印的，正好是用例盲区。）
    if ((uint64_t)v < 10u) return pa_char_str((int64_t)('0' + v));
    // 手写十进制：snprintf 的格式解析在 str(i) 这种热路径上占大头。
    // **两位数查表 + 直接写进目标串**：一次出两位把除法次数减半；先算位数再
    // 分配，数字从目标串尾部倒着写，省掉「栈缓冲 → str_new_ascii memcpy」的
    // 往返（perf 显示那段 2 字节栈写 + 8 字节回读有 store-forwarding 停顿，
    // 占本函数 30%）。
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int len = u64_dec_digits(mag) + (v < 0);
    PyAdamasStr* s = str_alloc_raw(len);
    char* p = u64_write_dec((char*)s->data + len, mag);
    if (v < 0) *--p = '-';
    s->pub = len;  // 全 ASCII：码点数恒等于字节数（O(1)，免 str_count_cp）
    return s;
}

/* 无符号 64 位 → 十进制串（定宽 u64 的 str()/print() 用）。 */
PyAdamasStr* pa_str_from_u64(uint64_t v) {
    if (v < 10u) return pa_char_str((int64_t)('0' + v));
    int len = u64_dec_digits(v);
    PyAdamasStr* s = str_alloc_raw(len);
    u64_write_dec((char*)s->data + len, v);
    s->pub = len;
    return s;
}

/* unknown 值的 str()：先按 RC 索引确认是不是堆字符串；是就原样返回
   （补一份引用，调用方拿到的是新引用），否则按整数格式化。判定和
   pa_print_any 一致——unknown 里分不开 int 和指针，只能查注册表。 */
PyAdamasStr* pa_str_any(int64_t v) {
    if (v > 4096) {
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v -
                                                (uint64_t)PYADAMAS_RC_HDR);
        if (pa_unknown_is_heap(v) && h->kind == PYADAMAS_RC_BIGINT)
            return pa_big_str((PyAdamasBig*)(uintptr_t)v);
        if (rc_index_has(h)) {
            if (h->kind == PYADAMAS_RC_STR) {
                pa_rc_inc((void*)(uintptr_t)v);
                return (PyAdamasStr*)(uintptr_t)v;
            }
            /* 0.324：unknown 域的 float 用 CPYFLOAT 盒携带标签。
               str(float) 与 CPython 一致走双精度格式化。 */
            if (h->kind == PYADAMAS_RC_CPYFLOAT) {
                double d;
                memcpy(&d, (void*)(uintptr_t)v, sizeof d);
                return pa_str_from_f64(d);
            }
            /* str(list) == repr(list)，str(dict) == repr(dict)。 */
            if (h->kind == PYADAMAS_RC_LIST || h->kind == PYADAMAS_RC_DICT)
                return pa_repr_any(v);
            /* P4（lang_gaps）：str(类对象) == repr(类对象)（CPython 口径）。 */
            if (pa_is_class_obj((void*)(uintptr_t)v))
                return pa_class_repr((void*)(uintptr_t)v);
        }
        /* P4 块3（lang_gaps 门控开）：str(异常值) == 消息（CPython 口径）。
           用 pa_unknown_is_heap 而非 rc_index_has：exact-RC 模式（无索引）
           也认得。门控关逐字旧样。 */
        if (pa_lang_gaps_rt && pa_unknown_is_heap(v)) {
            PyAdamasRc* h2 = (PyAdamasRc*)(uintptr_t)((uint64_t)v -
                                                    (uint64_t)PYADAMAS_RC_HDR);
            if (h2->kind == PYADAMAS_RC_OBJ_EXCVALUE &&
                ((PyAdamasExcValue*)(uintptr_t)v)->magic == PYADAMAS_EXCVAL_MAGIC)
                return pa_exc_value_str((PyAdamasExcValue*)(uintptr_t)v);
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

/* CPython 口径：invalid literal for int() with base %d: %r。
   exc_new 会复制消息，临时缓冲在 longjmp 前释放，不留泄漏。 */
static _Noreturn void int_literal_error(PyAdamasStr* s, int base) {
    PyAdamasStr* r = pa_repr_str(s);
    char head[64];
    int hn = snprintf(head, sizeof head, "invalid literal for int() with base %d: ", base);
    if (hn < 0) hn = 0;
    size_t total = (size_t)hn + (size_t)r->len + 1;
    char* msg = (char*)malloc(total);
    if (!msg) panic_c("out of memory");
    memcpy(msg, head, (size_t)hn);
    memcpy(msg + hn, r->data, (size_t)r->len);
    msg[total - 1] = '\0';
    PyAdamasException* e = exc_new("ValueError", msg);
    free(msg);
    pa_rc_dec(r);
    exc_free(pending_exc);
    pending_exc = e;
    exc_longjmp();
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
            int_literal_error(s, (int)base);
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
    if (!any || p != end) int_literal_error(s, b);
    return neg ? (int64_t)(0 - acc) : (int64_t)acc;
}

/* Clinger 快速路径：|尾数| < 2^53 且 |十进制指数| <= 22 时，10^k 可被 double
   精确表示，mant * 10^exp（或 /）只发生一次舍入，结果与正确舍入的 strtod
   完全一致。f-string 的 "%.6f" 之类输出都落在这里，避开 glibc strtod 的
   大数慢路径（str_to_mpn / round_and_return）。不满足条件返回 0 回落 strtod。 */
static int f64_fast_path(const char* p, int64_t n, double* out) {
    static const double pow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };
    int64_t i = 0;
    while (i < n && is_space(p[i])) ++i;
    int neg = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) {
        neg = p[i] == '-';
        ++i;
    }
    uint64_t mant = 0;
    int digits = 0, any = 0, exp10 = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        if (mant || p[i] != '0') {
            if (++digits > 15) return 0;  /* 超过 2^53 可精确表示的范围 */
            mant = mant * 10 + (uint64_t)(p[i] - '0');
        }
        any = 1;
        ++i;
    }
    if (i < n && p[i] == '.') {
        ++i;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            if (mant || p[i] != '0') {
                if (++digits > 15) return 0;
                mant = mant * 10 + (uint64_t)(p[i] - '0');
            }
            any = 1;
            --exp10;
            ++i;
        }
    }
    if (!any) return 0;  /* "", ".", "inf", "nan", 十六进制浮点等交给 strtod */
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        ++i;
        int eneg = 0;
        if (i < n && (p[i] == '+' || p[i] == '-')) {
            eneg = p[i] == '-';
            ++i;
        }
        if (i >= n || p[i] < '0' || p[i] > '9') return 0;  /* "1e" 等非法尾巴 */
        int ev = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            if (ev < 1000) ev = ev * 10 + (p[i] - '0');
            ++i;
        }
        exp10 += eneg ? -ev : ev;
    }
    while (i < n && is_space(p[i])) ++i;
    if (i != n) return 0;  /* 有多余字符（含 '_' 等），交回 strtod 报错 */
    if (exp10 > 22 || exp10 < -22) return 0;
    double v = (double)mant;
    if (exp10 > 0) v *= pow10[exp10];
    else if (exp10 < 0) v /= pow10[-exp10];
    *out = neg ? -v : v;
    return 1;
}

double pa_str_to_f64(PyAdamasStr* s) {
    double fast = 0.0;
    if (f64_fast_path(s->data, s->len, &fast)) return fast;
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
    if (bad) {
        /* CPython 口径：could not convert string to float: %r */
        PyAdamasStr* r = pa_repr_str(s);
        const char* head = "could not convert string to float: ";
        size_t hn = strlen(head);
        size_t total = hn + (size_t)r->len + 1;
        char* msg = (char*)malloc(total);
        if (!msg) panic_c("out of memory");
        memcpy(msg, head, hn);
        memcpy(msg + hn, r->data, (size_t)r->len);
        msg[total - 1] = '\0';
        PyAdamasException* e = exc_new("ValueError", msg);
        free(msg);
        pa_rc_dec(r);
        exc_free(pending_exc);
        pending_exc = e;
        exc_longjmp();
    }
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

/* 2/8/16 的分组查表：一次写一组（2 位十六进制 = 1 字节，8 位二进制 = 1 字节，
   3 位八进制 = 9 位），把位数循环压缩到原来的 1/8、1/2、1/3。
   hex/bin/oct 每次都要重建短串，位数循环 + 栈缓冲拷贝曾是主要成本。 */
static const char pa_hex2[] =
    "000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f"
    "303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f"
    "505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f"
    "707172737475767778797a7b7c7d7e7f"
    "808182838485868788898a8b8c8d8e8f"
    "909192939495969798999a9b9c9d9e9f"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
    "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
    "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
    "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
    "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

static const char pa_bin8[] =
    "00000000" "00000001" "00000010" "00000011"
    "00000100" "00000101" "00000110" "00000111"
    "00001000" "00001001" "00001010" "00001011"
    "00001100" "00001101" "00001110" "00001111"
    "00010000" "00010001" "00010010" "00010011"
    "00010100" "00010101" "00010110" "00010111"
    "00011000" "00011001" "00011010" "00011011"
    "00011100" "00011101" "00011110" "00011111"
    "00100000" "00100001" "00100010" "00100011"
    "00100100" "00100101" "00100110" "00100111"
    "00101000" "00101001" "00101010" "00101011"
    "00101100" "00101101" "00101110" "00101111"
    "00110000" "00110001" "00110010" "00110011"
    "00110100" "00110101" "00110110" "00110111"
    "00111000" "00111001" "00111010" "00111011"
    "00111100" "00111101" "00111110" "00111111"
    "01000000" "01000001" "01000010" "01000011"
    "01000100" "01000101" "01000110" "01000111"
    "01001000" "01001001" "01001010" "01001011"
    "01001100" "01001101" "01001110" "01001111"
    "01010000" "01010001" "01010010" "01010011"
    "01010100" "01010101" "01010110" "01010111"
    "01011000" "01011001" "01011010" "01011011"
    "01011100" "01011101" "01011110" "01011111"
    "01100000" "01100001" "01100010" "01100011"
    "01100100" "01100101" "01100110" "01100111"
    "01101000" "01101001" "01101010" "01101011"
    "01101100" "01101101" "01101110" "01101111"
    "01110000" "01110001" "01110010" "01110011"
    "01110100" "01110101" "01110110" "01110111"
    "01111000" "01111001" "01111010" "01111011"
    "01111100" "01111101" "01111110" "01111111"
    "10000000" "10000001" "10000010" "10000011"
    "10000100" "10000101" "10000110" "10000111"
    "10001000" "10001001" "10001010" "10001011"
    "10001100" "10001101" "10001110" "10001111"
    "10010000" "10010001" "10010010" "10010011"
    "10010100" "10010101" "10010110" "10010111"
    "10011000" "10011001" "10011010" "10011011"
    "10011100" "10011101" "10011110" "10011111"
    "10100000" "10100001" "10100010" "10100011"
    "10100100" "10100101" "10100110" "10100111"
    "10101000" "10101001" "10101010" "10101011"
    "10101100" "10101101" "10101110" "10101111"
    "10110000" "10110001" "10110010" "10110011"
    "10110100" "10110101" "10110110" "10110111"
    "10111000" "10111001" "10111010" "10111011"
    "10111100" "10111101" "10111110" "10111111"
    "11000000" "11000001" "11000010" "11000011"
    "11000100" "11000101" "11000110" "11000111"
    "11001000" "11001001" "11001010" "11001011"
    "11001100" "11001101" "11001110" "11001111"
    "11010000" "11010001" "11010010" "11010011"
    "11010100" "11010101" "11010110" "11010111"
    "11011000" "11011001" "11011010" "11011011"
    "11011100" "11011101" "11011110" "11011111"
    "11100000" "11100001" "11100010" "11100011"
    "11100100" "11100101" "11100110" "11100111"
    "11101000" "11101001" "11101010" "11101011"
    "11101100" "11101101" "11101110" "11101111"
    "11110000" "11110001" "11110010" "11110011"
    "11110100" "11110101" "11110110" "11110111"
    "11111000" "11111001" "11111010" "11111011"
    "11111100" "11111101" "11111110" "11111111";

static inline int u64_bit_length(uint64_t m) {
    return m ? 64 - __builtin_clzll(m) : 0;
}

/* base 位数（2/8/16 用位长算，其余走除法循环）。 */
static int u64_base_digits(uint64_t mag, int64_t base) {
    if (base == 16) {
        int b = u64_bit_length(mag);
        return b ? (b + 3) / 4 : 1;
    }
    if (base == 2) {
        int b = u64_bit_length(mag);
        return b ? b : 1;
    }
    if (base == 8) {
        int b = u64_bit_length(mag);
        return b ? (b + 2) / 3 : 1;
    }
    int n = 1;
    while (mag >= (uint64_t)base) { mag /= (uint64_t)base; ++n; }
    return n;
}

/* 把 mag 按 base 写成数字（从 dst 尾部往前），返回字符数。
   2/8/16 一次写一组：2 位十六进制 = 1 字节、8 位二进制 = 1 字节、
   3 位八进制 = 9 位；其余 base 保留通用除法路径。 */
static int u64_to_base_rev(char* dst_end, uint64_t mag, int64_t base) {
    static const char* digits = "0123456789abcdef";
    char* q = dst_end;
    int n = 0;
    switch (base) {
        case 2:
            while (mag >= 256u) {
                unsigned g = (unsigned)(mag & 255u);
                mag >>= 8;
                q -= 8;
                memcpy(q, pa_bin8 + (size_t)g * 8, 8);
                n += 8;
            }
            do {
                *--q = (char)('0' + (int)(mag & 1));
                mag >>= 1;
                ++n;
            } while (mag);
            break;
        case 8:
            while (mag >= 512u) {
                unsigned g = (unsigned)(mag & 511u);
                mag >>= 9;
                q -= 3;
                q[0] = (char)('0' + (int)(g >> 6));
                q[1] = (char)('0' + (int)((g >> 3) & 7));
                q[2] = (char)('0' + (int)(g & 7));
                n += 3;
            }
            if (mag >= 64u) {
                *--q = (char)('0' + (int)(mag & 7));
                *--q = (char)('0' + (int)((mag >> 3) & 7));
                *--q = (char)('0' + (int)(mag >> 6));
                n += 3;
                break;
            }
            do {
                *--q = (char)('0' + (int)(mag & 7));
                mag >>= 3;
                ++n;
            } while (mag);
            break;
        case 16:
            while (mag >= 256u) {
                unsigned g = (unsigned)(mag & 255u);
                mag >>= 8;
                q -= 2;
                q[0] = pa_hex2[g * 2];
                q[1] = pa_hex2[g * 2 + 1];
                n += 2;
            }
            do {
                *--q = digits[mag & 15];
                mag >>= 4;
                ++n;
            } while (mag);
            break;
        default:
            do {
                *--q = digits[mag % (uint64_t)base];
                mag /= (uint64_t)base;
                ++n;
            } while (mag);
            break;
    }
    return n;
}

PyAdamasStr* pa_int_to_base(int64_t v, int64_t base) {
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int n = u64_base_digits(mag, base);
    int k = n + 2 + (v < 0);
    PyAdamasStr* s = str_alloc_raw(k);
    char* out = (char*)s->data;
    int o = 0;
    if (v < 0) out[o++] = '-';
    out[o++] = '0';
    out[o++] = base == 16 ? 'x' : base == 8 ? 'o' : 'b';
    u64_to_base_rev(out + k, mag, base);
    /* 结果必然是 ASCII：prefix + 数字，码点数 == 字节数，省掉 seal 扫描。 */
    s->pub = k;
    return s;
}

/* 无符号 64 位 → 0x/0o/0b 串（定宽 u64 的 hex()/bin()/oct()）。
   u64 的 ≥ 2^63 值在 pa_int_to_base 里会被解释成负数并加 '-'，语义错。 */
PyAdamasStr* pa_uint_to_base(uint64_t v, int64_t base) {
    int n = u64_base_digits(v, base);
    PyAdamasStr* s = str_alloc_raw(n + 2);
    char* out = (char*)s->data;
    out[0] = '0';
    out[1] = base == 16 ? 'x' : base == 8 ? 'o' : 'b';
    u64_to_base_rev(out + n + 2, v, base);
    s->pub = n + 2;
    return s;
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
            } else if (count == 0) {
                /* CPython 语义：count == 0 表示一次都不替换（负数是全部）。
                   以前这里先 ++hits 再比 count，count==0 会替换一次（静默错值）。 */
                hits = 0;
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
        case 35: { /* rindex：rfind + 找不到抛 ValueError */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
            int64_t start = a1, end = a2;
            if (start < 0) start += s->pub;
            if (start < 0) start = 0;
            if (end < 0) end += s->pub;
            if (end > s->pub) end = s->pub;
            if (end < 0) end = 0;
            if (sub->len == 0 && end >= start) return end;
            if (start <= end) {
                int64_t bstart = str_cp_to_byte(s, start);
                int64_t bend = str_cp_to_byte(s, end);
                for (int64_t i = bend - sub->len; i >= bstart; --i) {
                    if (((unsigned char)s->data[i] & 0xC0) == 0x80) continue;
                    if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0)
                        return cp_index_of(s, i);
                }
            }
            pa_raise_c("ValueError", "substring not found");
            return -1;
        }
        case 34: { /* isascii：所有码点 < 0x80 */
            int64_t off = 0;
            while (off < s->len) {
                int n = 0;
                if (str_cp_at(s, off, &n) >= 0x80) return 0;
                off += n;
            }
            return 1;
        }
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
            } else if (count == 0) {
                /* CPython 语义：count == 0 表示一次都不替换（负数是全部）。
                   以前这里先 ++hits 再比 count，count==0 会替换一次（静默错值）。 */
                hits = 0;
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
        case 35: { /* rindex：字节下标版 */
            PyAdamasStr* sub = (PyAdamasStr*)a0;
            int64_t start = a1, end = a2;
            if (start < 0) start += s->len;
            if (start < 0) start = 0;
            if (end < 0) end += s->len;
            if (end > s->len) end = s->len;
            if (end < 0) end = 0;
            if (sub->len == 0 && end >= start) return end;
            if (start <= end) {
                for (int64_t i = end - sub->len; i >= start; --i) {
                    if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
                }
            }
            pa_raise_c("ValueError", "subsection not found");
            return -1;
        }
        case 34: { /* isascii：所有字节 < 0x80 */
            for (int64_t i = 0; i < s->len; ++i)
                if ((unsigned char)s->data[i] >= 0x80) return 0;
            return 1;
        }
        default:
            pa_raise_c("ValueError", "unknown bytes method");
    }
    return 0;
}

/* sys.intern(s)：返回同一份字符串（+1）。pa 没有驻留表：语义上 interning
   是优化，相等性不变；唯一差异是「动态构造的相等串」不保证同一对象
   （CPython 会返回同一份）。见 docs/frontend-gaps.md。 */
PyAdamasStr* pa_sys_intern(PyAdamasStr* s) {
    pa_rc_inc(s);
    return s;
}
