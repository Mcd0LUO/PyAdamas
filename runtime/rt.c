/* rt.c —— py23 运行时
 *
 * 只放「生成代码不该内联展开」的东西：堆分配、格式化输出、越界检查。
 * 值统一用 int64_t 传递：整数就是整数，浮点是它的位模式，指针是地址。
 *
 * 字符串有两种来源：
 *   1. 字面量 —— 由 codegen 在 .rodata 里静态构造（len/hash/data 三字段），
 *      永远不会被修改，也不需要释放；
 *   2. 拼接/转换 —— 在堆上一次分配 header + 字符数据。
 * 两者布局一致，所以运行时函数不需要区分。
 *
 * 内存目前只分配不释放。等 GC 或引用计数做好之前，编译器只适合跑
 * 短生命周期的程序 —— 这一点在 README 里写清楚了。
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t len;
    int64_t hash;      /* 预留给字符串驻留 */
    const char* data;
} Py23Str;

typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* data;
} Py23List;

/* ── 内部工具 ─────────────────────────────────────────────────── */

static void panic_c(const char* msg) {
    fprintf(stderr, "py23: runtime error: %s\n", msg);
    exit(1);
}

static Py23Str* str_alloc(int64_t len) {
    if (len < 0) panic_c("negative string length");
    Py23Str* s = (Py23Str*)malloc(sizeof(Py23Str) + (size_t)len + 1);
    if (!s) panic_c("out of memory");
    s->len = len;
    s->hash = 0;
    char* p = (char*)(s + 1);
    p[len] = '\0';
    s->data = p;
    return s;
}

static Py23Str* str_new(const char* p, int64_t len) {
    Py23Str* s = str_alloc(len);
    memcpy((char*)s->data, p, (size_t)len);
    return s;
}

/* Python 的 float 打印规则：最短的、能精确往返的十进制表示，整数值补 .0 */
/* 按 CPython repr 的规则排版：先找最短的能原值读回的数字串，
   再决定用定点还是科学计数法。

   不能直接用 %g 的原因：%g 在「指数 >= 精度」时就切到科学计数法，
   而最短数字串的精度往往很小，于是 10.0 会被排成 1e+01。Python 的
   界限是「小数点位置 <= -4 或 > 16」，跟数字串长度无关。 */
static void format_double(char* buf, size_t n, double v) {
    if (isnan(v)) {
        snprintf(buf, n, "nan");
        return;
    }
    if (isinf(v)) {
        snprintf(buf, n, v > 0 ? "inf" : "-inf");
        return;
    }

    /* 从最短开始找第一个能 strtod 回来的精度。反过来（17 → 1）会停在
       「第一个失败的精度之上」，吐出一串多余的末位数字。 */
    char sci[64];
    int prec = 0;
    for (prec = 1; prec <= 17; ++prec) {
        snprintf(sci, sizeof sci, "%.*e", prec - 1, v);
        if (strtod(sci, NULL) == v) break;
    }

    const char* s = sci;
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        ++s;
    }
    char digits[32];
    int nd = 0;
    while (*s && *s != 'e') {
        if (*s >= '0' && *s <= '9') digits[nd++] = *s;
        ++s;
    }
    int e10 = (*s == 'e') ? atoi(s + 1) : 0;
    while (nd > 1 && digits[nd - 1] == '0') --nd; /* 只剩 "0" 时不砍，0.0 要用 */

    int decpt = e10 + 1; /* value = 0.<digits> × 10^decpt */
    char out[64];
    int k = 0;
    if (neg) out[k++] = '-';
    if (decpt <= -4 || decpt > 16) {
        out[k++] = digits[0];
        if (nd > 1) {
            out[k++] = '.';
            for (int i = 1; i < nd; ++i) out[k++] = digits[i];
        }
        int ex = decpt - 1 < 0 ? 1 - decpt : decpt - 1;
        out[k++] = 'e';
        out[k++] = decpt - 1 < 0 ? '-' : '+';
        if (ex < 10) out[k++] = '0';
        k += snprintf(out + k, sizeof out - (size_t)k, "%d", ex);
    } else if (decpt <= 0) {
        out[k++] = '0';
        out[k++] = '.';
        for (int i = 0; i < -decpt; ++i) out[k++] = '0';
        for (int i = 0; i < nd; ++i) out[k++] = digits[i];
    } else if (decpt >= nd) {
        for (int i = 0; i < nd; ++i) out[k++] = digits[i];
        for (int i = nd; i < decpt; ++i) out[k++] = '0';
        out[k++] = '.';
        out[k++] = '0';
    } else {
        for (int i = 0; i < decpt; ++i) out[k++] = digits[i];
        out[k++] = '.';
        for (int i = decpt; i < nd; ++i) out[k++] = digits[i];
    }
    out[k] = '\0';
    snprintf(buf, n, "%s", out);
}

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
    if (i < 0 || i >= s->len) panic_c("string index out of range");
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

/* ── 列表 ─────────────────────────────────────────────────────── */

Py23List* py23_list_new(int64_t cap) {
    if (cap < 0) panic_c("negative list capacity");
    if (cap < 4) cap = 4;
    Py23List* l = (Py23List*)malloc(sizeof(Py23List));
    if (!l) panic_c("out of memory");
    l->len = 0;
    l->cap = cap;
    l->data = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    if (!l->data) panic_c("out of memory");
    return l;
}

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
    if (i < 0 || i >= l->len) panic_c("list index out of range");
    return l->data[i];
}

void py23_list_set(Py23List* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) panic_c("list assignment index out of range");
    l->data[i] = v;
}

void py23_list_set_len(Py23List* l, int64_t n) {
    if (n < 0 || n > l->len) panic_c("invalid list length");
    l->len = n;
}

/* ── 对象 ─────────────────────────────────────────────────────── */

/* 对象 = [vtable 指针][字段 0][字段 1]…。分配时全部清零：0 同时是
   None / 0 / 假，和栈槽的约定一致，所以「声明了但没赋值的字段」
   天然就是 None。vtable 指针由生成代码紧接着写入。 */
void* py23_obj_new(int64_t size) {
    if (size < 8) panic_c("object size too small");
    void* p = calloc(1, (size_t)size);
    if (!p) panic_c("out of memory");
    return p;
}

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
    fprintf(stderr, "py23: panic: %.*s\n", (int)msg->len, msg->data);
    exit(1);
}

void py23_panic_div0(void) {
    fprintf(stderr, "py23: ZeroDivisionError: integer division or modulo by zero\n");
    exit(1);
}

void py23_panic_overflow(void) {
    fprintf(stderr, "py23: OverflowError: integer division result too large for int64\n");
    exit(1);
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
