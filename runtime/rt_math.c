// rt_math.c —— math / sys / panic / pow / abs
#include "rt_internal.h"

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

