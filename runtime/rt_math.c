// rt_math.c —— math / sys / panic / pow / abs
#include "rt_internal.h"

/* ── 数学 / 内置模块 ──────────────────────────────────────────── */

double pa_math_sqrt(double x) {
    if (x < 0) pa_raise_c("ValueError", "math domain error");
    return sqrt(x);
}
/* 浮点→int64 的 C 转换对 NaN/越界是 UB（实测 PC 上得 INT64_MIN）。
   math.floor/ceil/trunc 在 CPython 里返回 int 且对 NaN/inf 抛异常，
   这里统一检查；正常值直接截断（-2^63 本身合法，不能误抛）。 */
static int64_t math_f2i_checked(double v) {
    if (v != v) pa_raise_c("ValueError", "cannot convert float NaN to integer");
    if (v >= 9223372036854775808.0 || v < -9223372036854775808.0)
        pa_raise_c("OverflowError", "cannot convert float infinity to integer");
    return (int64_t)v;
}
int64_t pa_math_floor(double x) { return math_f2i_checked(floor(x)); }
int64_t pa_math_ceil(double x) { return math_f2i_checked(ceil(x)); }
double pa_math_fabs(double x) { return fabs(x); }
double pa_math_sin(double x) { return sin(x); }
double pa_math_cos(double x) { return cos(x); }
double pa_math_tan(double x) { return tan(x); }
double pa_math_log(double x) {
    if (x <= 0) pa_raise_c("ValueError", "math domain error");
    return log(x);
}
double pa_math_log2(double x) {
    if (x <= 0) pa_raise_c("ValueError", "math domain error");
    return log2(x);
}
double pa_math_log10(double x) {
    if (x <= 0) pa_raise_c("ValueError", "math domain error");
    return log10(x);
}
double pa_math_exp(double x) { return exp(x); }
double pa_math_pow(double x, double y) { return pow(x, y); }
double pa_math_fmod(double x, double y) {
    if (y == 0) pa_raise_c("ValueError", "math domain error");
    return fmod(x, y);
}
double pa_math_atan2(double y, double x) { return atan2(y, x); }
double pa_math_hypot(double x, double y) { return hypot(x, y); }
double pa_math_degrees(double x) { return x * (180.0 / M_PI); }
double pa_math_radians(double x) { return x * (M_PI / 180.0); }
int64_t pa_math_trunc(double x) { return math_f2i_checked(trunc(x)); }
double pa_math_copysign(double x, double y) { return copysign(x, y); }
int64_t pa_math_isnan(double x) { return isnan(x) ? 1 : 0; }
int64_t pa_math_isinf(double x) { return isinf(x) ? 1 : 0; }
int64_t pa_math_isfinite(double x) { return isfinite(x) ? 1 : 0; }

int64_t pa_math_factorial(int64_t n) {
    if (n < 0) pa_raise_c("ValueError", "factorial() not defined for negative values");
    int64_t r = 1;
    for (int64_t i = 2; i <= n; ++i) r *= i;
    return r;
}

static uint64_t math_mag(int64_t v) {
    return v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
}

int64_t pa_math_gcd(int64_t a, int64_t b) {
    uint64_t x = math_mag(a), y = math_mag(b);
    while (y) {
        uint64_t t = x % y;
        x = y;
        y = t;
    }
    return (int64_t)x;
}

int64_t pa_math_lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    uint64_t g = (uint64_t)pa_math_gcd(a, b);
    return (int64_t)((math_mag(a) / g) * math_mag(b));
}

double pa_math_fsum(PyAdamasList* l) {
    if (!l || l->len == 0) return 0.0;
    // Shewchuk 部分和，和 CPython 的 math.fsum 同一套算法：
    // 每个输入与现有 partials 做无误差的 hi/lo 分解，最后从高位往低位
    // 合并并做一次舍入修正。Kahan 在灾难性抵消下不保证正确舍入。
    int64_t n = l->len;
    double* p = (double*)malloc(sizeof(double) * (size_t)n);
    if (!p) {
        fputs("pyadamas: panic: out of memory\n", stderr);
        exit(1);
    }
    int64_t m = 0;
    for (int64_t k = 0; k < n; ++k) {
        double x;
        memcpy(&x, &l->data[k], sizeof x);
        int64_t i = 0;
        for (int64_t j = 0; j < m; ++j) {
            double y = p[j];
            if (fabs(x) < fabs(y)) {
                double t = x;
                x = y;
                y = t;
            }
            double hi = x + y;
            double lo = y - (hi - x);
            if (lo != 0.0) p[i++] = lo;
            x = hi;
        }
        m = i;
        p[m++] = x;
    }
    double hi = 0.0;
    if (m > 0) {
        hi = p[--m];
        double lo = 0.0;
        while (m > 0) {
            double x = hi;
            double y = p[--m];
            hi = x + y;
            lo = y - (hi - x);
            if (lo != 0.0) break;
        }
        if (m > 0 && ((lo < 0.0 && p[m - 1] < 0.0) || (lo > 0.0 && p[m - 1] > 0.0))) {
            double y = lo * 2.0;
            double x = hi + y;
            if (y == x - hi) hi = x;
        }
    }
    free(p);
    return hi;
}

double pa_math_acos(double x) {
    if (x < -1.0 || x > 1.0) pa_raise_c("ValueError", "math domain error");
    return acos(x);
}
double pa_math_asin(double x) {
    if (x < -1.0 || x > 1.0) pa_raise_c("ValueError", "math domain error");
    return asin(x);
}
double pa_math_atan(double x) { return atan(x); }
double pa_math_acosh(double x) {
    if (x < 1.0) pa_raise_c("ValueError", "math domain error");
    return acosh(x);
}
double pa_math_asinh(double x) { return asinh(x); }
double pa_math_atanh(double x) {
    if (x <= -1.0 || x >= 1.0) pa_raise_c("ValueError", "math domain error");
    return atanh(x);
}
double pa_math_cbrt(double x) { return cbrt(x); }
double pa_math_cosh(double x) { return cosh(x); }
double pa_math_sinh(double x) { return sinh(x); }
double pa_math_tanh(double x) { return tanh(x); }
double pa_math_exp2(double x) { return exp2(x); }
double pa_math_expm1(double x) { return expm1(x); }
double pa_math_log1p(double x) {
    if (x <= -1.0) pa_raise_c("ValueError", "math domain error");
    return log1p(x);
}
double pa_math_erf(double x) { return erf(x); }
double pa_math_erfc(double x) { return erfc(x); }

static int math_is_pole(double x) { return x == 0.0 || (x < 0.0 && x == floor(x)); }

double pa_math_gamma(double x) {
    if (math_is_pole(x)) pa_raise_c("ValueError", "math domain error");
    return tgamma(x);
}
double pa_math_lgamma(double x) {
    if (math_is_pole(x)) pa_raise_c("ValueError", "math domain error");
    return lgamma(x);
}

double pa_math_ulp(double x) {
    if (isnan(x)) return x;
    if (isinf(x)) return fabs(x);
    if (x == 0.0) return nextafter(0.0, 1.0);
    double up = nextafter(x, INFINITY);
    return fabs(up - x);
}
double pa_math_nextafter(double x, double y) { return nextafter(x, y); }
double pa_math_remainder(double x, double y) {
    if (y == 0.0) pa_raise_c("ValueError", "math domain error");
    return remainder(x, y);
}
double pa_math_ldexp(double x, int64_t i) { return ldexp(x, (int)i); }
double pa_math_fma(double x, double y, double z) { return fma(x, y, z); }

int64_t pa_math_isclose(double a, double b) {
    if (a == b) return 1;
    if (isinf(a) || isinf(b)) return 0;
    double diff = fabs(a - b);
    double rel = 1e-9 * fmax(fabs(a), fabs(b));
    return diff <= rel ? 1 : 0;
}

static void math_tuple_push_f64(PyAdamasList* l, double v) {
    int64_t bits;
    memcpy(&bits, &v, sizeof bits);
    /* elem_kind=0：裸值（float 位模式 / 整数），**不能**按 unknown 追加。
       传 2 会走 pa_unknown_is_heap(bit_pattern)：把 double 的位模式当指针
       查堆索引 → 越界读 → 段错误（实测 math.frexp/modf 一调用就崩）。 */
    pa_list_append(l, bits, 0);
}

PyAdamasList* pa_math_frexp(double x) {
    int e = 0;
    double m = frexp(x, &e);
    PyAdamasList* l = pa_list_new(2, 0);
    math_tuple_push_f64(l, m);
    pa_list_append(l, (int64_t)e, 0);
    return l;
}

PyAdamasList* pa_math_modf(double x) {
    double ip = 0.0;
    double fp = modf(x, &ip);
    PyAdamasList* l = pa_list_new(2, 0);
    math_tuple_push_f64(l, fp);
    math_tuple_push_f64(l, ip);
    return l;
}

int64_t pa_math_isqrt(int64_t n) {
    if (n < 0) pa_raise_c("ValueError", "isqrt() argument must be nonnegative");
    if (n < 2) return n;
    uint64_t r = (uint64_t)sqrt((double)n);
    while (r > 0 && r > (uint64_t)n / r) --r;
    while ((r + 1) <= (uint64_t)n / (r + 1)) ++r;
    return (int64_t)r;
}

static int64_t math_binom(int64_t n, int64_t k) {
    if (k < 0 || k > n) return 0;
    if (k > n - k) k = n - k;
    int64_t r = 1;
    for (int64_t i = 1; i <= k; ++i) r = r * (n - k + i) / i;
    return r;
}

int64_t pa_math_comb(int64_t n, int64_t k) {
    if (n < 0 || k < 0) pa_raise_c("ValueError", "comb() arguments must be nonnegative");
    return math_binom(n, k);
}

int64_t pa_math_perm(int64_t n, int64_t k) {
    if (n < 0 || k < 0) pa_raise_c("ValueError", "perm() arguments must be nonnegative");
    if (k > n) return 0;
    int64_t r = 1;
    for (int64_t i = 0; i < k; ++i) r *= n - i;
    return r;
}

int64_t pa_math_prod(PyAdamasList* l) {
    if (!l) return 1;
    int64_t r = 1;
    for (int64_t i = 0; i < l->len; ++i) r *= l->data[i];
    return r;
}

int64_t pa_math_sumprod(PyAdamasList* a, PyAdamasList* b) {
    if (!a || !b) return 0;
    if (a->len != b->len)
        pa_raise_c("ValueError", "sumprod() inputs must have the same length");
    int64_t r = 0;
    for (int64_t i = 0; i < a->len; ++i) r += a->data[i] * b->data[i];
    return r;
}

double pa_math_dist(PyAdamasList* a, PyAdamasList* b) {
    if (!a || !b) return 0.0;
    if (a->len != b->len)
        pa_raise_c("ValueError", "dist() requires two points of the same dimension");
    double sum = 0.0;
    for (int64_t i = 0; i < a->len; ++i) {
        double x, y;
        memcpy(&x, &a->data[i], sizeof x);
        memcpy(&y, &b->data[i], sizeof y);
        double d = x - y;
        sum += d * d;
    }
    return sqrt(sum);
}

PyAdamasList* sys_argv_list;

/* sys_argv_list 的根只登记一次；登记的是「槽的地址」。 */
static int sys_argv_root_registered;

/* 进程退出时释放 sys.argv。它挂在全局根上、永远不 dec，而 exact-RC 模式下
   字符串不进 GC 堆链（rc_link 直接返回），所以 pa_gc_shutdown 的
   `while (heap_head)` 扫不到它们 —— ASan 会把它们报成 Direct leak
   （实测每个用例 112 字节左右，来自 argv[0]）。
   退出前显式 dec 一次，把引用交还给 RC。 */
void pa_sys_argv_shutdown(void) {
    if (!sys_argv_list) return;
    PyAdamasRc* h = (PyAdamasRc*)sys_argv_list - 1;
    void* p = sys_argv_list;
    sys_argv_list = NULL;
    if (h->rc != PYADAMAS_RC_IMMORTAL) pa_rc_dec(p);
}

void pa_sys_set_args(int64_t argc, char** argv) {
    /* 先登记根再分配：pa_list_new / str_new 都走 rc_alloc，可能触发主收集。
       原顺序是「先分配列表、再登记根」，那一步的分配本身不会回收刚返回的
       列表，但随后的 str_new 循环里任何一次主收集都会看不见只挂在
       sys_argv_list 上的列表（以及已 append 进去的字符串，列表是它们的
       唯一持有者），造成 use-after-free。 */
    if (!sys_argv_root_registered) {
        void* root_slot[1] = {(void*)&sys_argv_list};
        pa_gc_add_roots(root_slot, 1);
        sys_argv_root_registered = 1;
    }
    PyAdamasList* out = pa_list_new(argc > 0 ? argc : 0, 1);
    sys_argv_list = out;
    for (int64_t i = 0; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        PyAdamasStr* str = str_new(s, (int64_t)strlen(s));
        pa_list_append(out, (int64_t)str, 2);
    }
}

PyAdamasList* pa_sys_argv(void) {
    if (!sys_argv_list) sys_argv_list = pa_list_new(0, 1);
    return sys_argv_list;
}

/* sys.executable：pa 口径是编译产物自身的路径（argv[0]），与 CPython
   「解释器可执行文件」同概念——自执行重入（153/350 的 print_exc 子进程检查）
   在两种运行时下都能用同一套分支逻辑。与 pa_sys_argv 同口径：借用全局
   argv 列表里的字符串（活到 pa_sys_argv_shutdown），不 inc，调用方按
   builtin_value 的既有约定消费（存槽时自己 retain）。 */
PyAdamasStr* pa_sys_executable(void) {
    PyAdamasList* l = pa_sys_argv();
    if (l->len == 0) return str_new("", 0);
    return (PyAdamasStr*)(uintptr_t)l->data[0];
}

void pa_sys_exit(int64_t code) { exit((int)code); }

static int64_t pa_recursion_limit = 1000;
int64_t pa_sys_getrecursionlimit(void) { return pa_recursion_limit; }
int64_t pa_sys_setrecursionlimit(int64_t n) {
    if (n < 1) panic_c("recursion limit must be >= 1");
    int64_t old = pa_recursion_limit;
    pa_recursion_limit = n;
    return old;
}

/* ── 输出 ─────────────────────────────────────────────────────── */

/* print() 每次调用都走这里：printf 的格式解析 + 可变参数 + 锁在
   48_print_sep / 01_print_int 这类循环里占 30%+。直接用共享的十进制
   渲染器写进栈缓冲，再 fwrite 一次。 */
void pa_print_i64(int64_t v) {
    char buf[24];
    int64_t n = pa_i64_to_buf(buf, v);
    fwrite(buf, 1, (size_t)n, stdout);
}

/* 无符号 64 位（定宽 u64）：print() 用。 */
void pa_print_u64(uint64_t v) {
    char buf[24];
    int64_t n = pa_u64_to_buf(buf, v);
    fwrite(buf, 1, (size_t)n, stdout);
}

void pa_print_f64(double v) {
    char buf[64];
    format_double(buf, sizeof buf, v);
    fputs(buf, stdout);
}

void pa_print_bool(int64_t v) { fputs(v ? "True" : "False", stdout); }

void pa_print_str(PyAdamasStr* s) { fwrite(s->data, 1, (size_t)s->len, stdout); }

// bytes 下标：返回那个字节的整数值（0..255），不是单字节切片。
int64_t pa_bytes_at(PyAdamasStr* s, int64_t i) {
    if (!s) return 0;
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) pa_raise_c("IndexError", "bytes index out of range");
    return (unsigned char)s->data[i];
}

// bytes 的 repr：b'...'，不可打印字节转成 \xNN。CPython 的引号选择规则：
// 内容里有单引号且没有双引号时改用双引号，避免转义。
void pa_print_bytes(PyAdamasStr* s) {
    int has_s = 0, has_d = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        if (s->data[i] == '\'') has_s = 1;
        if (s->data[i] == '"') has_d = 1;
    }
    char q = (has_s && !has_d) ? '"' : '\'';
    putchar('b');
    putchar(q);
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (c == (unsigned char)q) {
            putchar('\\');
            putchar(q);
        } else if (c == '\\') {
            fputs("\\\\", stdout);
        } else if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c == '\r') {
            fputs("\\r", stdout);
        } else if (c == '\t') {
            fputs("\\t", stdout);
        } else if (c >= 0x20 && c < 0x7f) {
            putchar((int)c);
        } else {
            printf("\\x%02x", (unsigned)c);
        }
    }
    putchar(q);
}

void pa_print_none(void) { fputs("None", stdout); }

void pa_print_sep(void) { fputc(' ', stdout); }

void pa_print_end(void) { fputc('\n', stdout); }

void pa_panic(PyAdamasStr* msg) {
    pa_raise_c("RuntimeError", msg->data);
}

void pa_panic_div0(void) {
    pa_raise_c("ZeroDivisionError", "integer division or modulo by zero");
}

void pa_panic_overflow(void) {
    pa_raise_c("OverflowError", "integer division result too large for int64");
}

/* ── 数学 ─────────────────────────────────────────────────────── */

/* int(float) 的域检查：cvttsd2siq 对 NaN / 超出 int64 的值返回 INT64_MIN 哨兵，
   codegen 的快路径只比一次哨兵（3 条），撞上才调这里。慢路径已用 ucomisd 排除
   了「输入恰好是 -2^63（合法 MIN）」的情况，所以到这里必是 NaN / ±inf / 有限越界。
   异常类型与 CPython 一致：NaN -> ValueError，±inf -> OverflowError。 */
void pa_int_from_f64_check(double v) {
    if (isnan(v)) pa_raise_c("ValueError", "cannot convert float NaN to integer");
    if (isinf(v)) pa_raise_c("OverflowError", "cannot convert float infinity to integer");
    pa_raise_c("OverflowError", "integer conversion overflow: value out of int64 range");
}

int64_t pa_pow_i64(int64_t base, int64_t exp) {
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
int64_t pa_abs_i64(int64_t v) { return v < 0 ? -v : v; }

