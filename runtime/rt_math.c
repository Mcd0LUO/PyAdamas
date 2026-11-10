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
double py23_math_atan2(double y, double x) { return atan2(y, x); }
double py23_math_hypot(double x, double y) { return hypot(x, y); }
double py23_math_degrees(double x) { return x * (180.0 / M_PI); }
double py23_math_radians(double x) { return x * (M_PI / 180.0); }
int64_t py23_math_trunc(double x) { return (int64_t)trunc(x); }
double py23_math_copysign(double x, double y) { return copysign(x, y); }
int64_t py23_math_isnan(double x) { return isnan(x) ? 1 : 0; }
int64_t py23_math_isinf(double x) { return isinf(x) ? 1 : 0; }
int64_t py23_math_isfinite(double x) { return isfinite(x) ? 1 : 0; }

int64_t py23_math_factorial(int64_t n) {
    if (n < 0) py23_raise_c("ValueError", "factorial() not defined for negative values");
    int64_t r = 1;
    for (int64_t i = 2; i <= n; ++i) r *= i;
    return r;
}

static uint64_t math_mag(int64_t v) {
    return v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
}

int64_t py23_math_gcd(int64_t a, int64_t b) {
    uint64_t x = math_mag(a), y = math_mag(b);
    while (y) {
        uint64_t t = x % y;
        x = y;
        y = t;
    }
    return (int64_t)x;
}

int64_t py23_math_lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    uint64_t g = (uint64_t)py23_math_gcd(a, b);
    return (int64_t)((math_mag(a) / g) * math_mag(b));
}

double py23_math_fsum(Py23List* l) {
    if (!l) return 0.0;
    double sum = 0.0, c = 0.0;
    for (int64_t i = 0; i < l->len; ++i) {
        double x;
        memcpy(&x, &l->data[i], sizeof x);
        double y = x - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
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

