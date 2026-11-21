/* rt_big 独立单元测试：链接 rt.o，直接调 pa_big_*。 */
#include "rt_internal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
static void ck_str(const char* what, PyAdamasBig* b, const char* want) {
    PyAdamasStr* s = pa_big_str(b);
    char buf[4096];
    int64_t n = s->len < 4095 ? s->len : 4095;
    memcpy(buf, s->data, (size_t)n); buf[n] = 0;
    if (strcmp(buf, want) != 0) {
        printf("FAIL %s: got %s want %s\n", what, buf, want);
        ++fails;
    }
    pa_rc_dec(s);
}
static PyAdamasBig* B(const char* s) { return pa_big_from_cstr(s, 10); }
static void ck_i64(const char* what, int64_t got, int64_t want) {
    if (got != want) { printf("FAIL %s: got %lld want %lld\n", what, (long long)got, (long long)want); ++fails; }
}
static void ck_d(const char* what, double got, double want) {
    if (got != want) { printf("FAIL %s: got %.17g want %.17g\n", what, got, want); ++fails; }
}

int main(void) {
    /* i64 往返 */
    int64_t vs[] = {0, 1, -1, 127, -128, 2147483647, -2147483648LL,
                    9223372036854775807LL, -9223372036854775807LL - 1};
    for (unsigned i = 0; i < sizeof vs / sizeof vs[0]; ++i) {
        PyAdamasBig* b = pa_big_from_i64(vs[i]);
        ck_i64("i64 roundtrip", pa_big_to_i64(b), vs[i]);
        pa_rc_dec(b);
    }
    /* 大数 str */
    PyAdamasBig* b2_64 = pa_big_pow(pa_big_from_i64(2), pa_big_from_i64(64));
    ck_str("2^64", b2_64, "18446744073709551616");
    PyAdamasBig* b2_100 = pa_big_pow(pa_big_from_i64(2), pa_big_from_i64(100));
    ck_str("2^100", b2_100, "1267650600228229401496703205376");
    PyAdamasBig* b10_30 = pa_big_pow(pa_big_from_i64(10), pa_big_from_i64(30));
    ck_str("10^30", b10_30, "1000000000000000000000000000000");
    /* 30! */
    PyAdamasBig* f = pa_big_from_i64(1);
    for (int64_t i = 2; i <= 30; ++i) {
        PyAdamasBig* t = pa_big_mul(f, pa_big_from_i64(i));
        pa_rc_dec(f); f = t;
    }
    ck_str("30!", f, "265252859812191058636308480000000");
    /* 加减乘 */
    PyAdamasBig *x = B("123456789012345678901234567890"),
                *y = B("987654321098765432109876543210");
    PyAdamasBig* s = pa_big_add(x, y);
    ck_str("x+y", s, "1111111110111111111011111111100");
    PyAdamasBig* d = pa_big_sub(y, x);
    ck_str("y-x", d, "864197532086419753208641975320");
    PyAdamasBig* m = pa_big_mul(x, y);
    ck_str("x*y", m, "121932631137021795226185032733622923332237463801111263526900");
    /* 负数乘 */
    PyAdamasBig* nx = pa_big_neg(x);
    PyAdamasBig* nm = pa_big_mul(nx, y);
    ck_str("-x*y", nm, "-121932631137021795226185032733622923332237463801111263526900");
    /* floordiv / mod（CPython 口径） */
    struct { const char *a, *b, *q, *r; } dv[] = {
        {"7", "3", "2", "1"}, {"-7", "3", "-3", "2"},
        {"7", "-3", "-3", "-2"}, {"-7", "-3", "2", "-1"},
        {"1000000000000000000000000000000", "7", "142857142857142857142857142857", "1"},
        {"-1000000000000000000000000000000", "7", "-142857142857142857142857142858", "6"},
        {"123456789012345678901234567890", "987654321098765432109876543210", "0", "123456789012345678901234567890"},
        {"987654321098765432109876543210", "123456789012345678901234567890", "8", "987654321098765432109876543210"},
    };
    /* 最后一组手算修正：8*123456789012345678901234567890 = 987654312098765431209876543120 */
    dv[7].r = "9000000000900000000090";
    for (unsigned i = 0; i < sizeof dv / sizeof dv[0]; ++i) {
        PyAdamasBig *a = B(dv[i].a), *b = B(dv[i].b);
        PyAdamasBig* q = pa_big_floordiv(a, b);
        PyAdamasBig* r = pa_big_mod(a, b);
        ck_str("floordiv", q, dv[i].q);
        ck_str("mod", r, dv[i].r);
        pa_rc_dec(a); pa_rc_dec(b); pa_rc_dec(q); pa_rc_dec(r);
    }
    /* 多 limb 除数（Knuth D）：(2^200+12345) / (2^100+7) */
    PyAdamasBig* p200 = pa_big_pow(pa_big_from_i64(2), pa_big_from_i64(200));
    PyAdamasBig* num = pa_big_add(p200, pa_big_from_i64(12345));
    PyAdamasBig* p100 = pa_big_pow(pa_big_from_i64(2), pa_big_from_i64(100));
    PyAdamasBig* den = pa_big_add(p100, pa_big_from_i64(7));
    PyAdamasBig* q = pa_big_floordiv(num, den);
    PyAdamasBig* r = pa_big_mod(num, den);
    /* q = 2^100 - 7, r = 12345 + 49 = 12394（(2^100+7)(2^100-7)=2^200-49） */
    PyAdamasBig* qw = pa_big_sub(p100, pa_big_from_i64(7));
    ck_str("knuth q", q, "1267650600228229401496703205369");
    ck_str("knuth r", r, "12394");
    (void)qw;
    /* pow */
    PyAdamasBig* p3_200 = pa_big_pow(pa_big_from_i64(3), pa_big_from_i64(200));
    ck_str("3^200", p3_200, "265613988875874769338781322035779626829233452653394495974574961739092490901302182994384699044001");
    ck_str("0^0", pa_big_pow(pa_big_from_i64(0), pa_big_from_i64(0)), "1");
    /* 位运算（负数补码） */
    struct { const char *a, *b; int op; const char* w; } bo[] = {
        {"-1", "5", 0, "5"}, {"-8", "3", 1, "-5"}, {"-1", "-1", 2, "0"},
        {"12345678901234567890", "-98765432109876543210", 0, "11623967127208787986"},
        {"-2", "-3", 0, "-4"}, {"-2", "3", 2, "-3"},
        {"123456789012345678901234567890", "987654321098765432109876543210", 1, "1109167149926620841576048328442"},
    };
    /* 期望值全部由 CPython 生成 */
    for (unsigned i = 0; i < sizeof bo / sizeof bo[0]; ++i) {
        PyAdamasBig *a = B(bo[i].a), *b = B(bo[i].b);
        PyAdamasBig* r = bo[i].op == 0 ? pa_big_and(a, b) : bo[i].op == 1 ? pa_big_or(a, b) : pa_big_xor(a, b);
        ck_str("bitop", r, bo[i].w);
        pa_rc_dec(a); pa_rc_dec(b); pa_rc_dec(r);
    }
    /* 移位 */
    PyAdamasBig* sh = pa_big_lshift(pa_big_from_i64(1), pa_big_from_i64(200));
    ck_str("1<<200", sh, "1606938044258990275541962092341162602522202993782792835301376");
    PyAdamasBig* rs = pa_big_rshift(pa_big_from_i64(-5), pa_big_from_i64(1));
    ck_str("-5>>1", rs, "-3");
    ck_str("-1>>100", pa_big_rshift(pa_big_from_i64(-1), pa_big_from_i64(100)), "-1");
    ck_str("-8>>2", pa_big_rshift(pa_big_from_i64(-8), pa_big_from_i64(2)), "-2");
    ck_str("5>>2", pa_big_rshift(pa_big_from_i64(5), pa_big_from_i64(2)), "1");
    /* 大数移位往返 */
    PyAdamasBig* big = B("12345678901234567890123456789012345678901234567890");
    PyAdamasBig* ls = pa_big_lshift(big, pa_big_from_i64(77));
    PyAdamasBig* back = pa_big_rshift(ls, pa_big_from_i64(77));
    ck_str("shift roundtrip", back, "12345678901234567890123456789012345678901234567890");
    PyAdamasBig* nbig = pa_big_neg(big);
    PyAdamasBig* nls = pa_big_lshift(nbig, pa_big_from_i64(77));
    PyAdamasBig* nback = pa_big_rshift(nls, pa_big_from_i64(77));
    ck_str("neg shift roundtrip", nback, "-12345678901234567890123456789012345678901234567890");
    /* 比较 */
    ck_i64("cmp", pa_big_cmp(B("12345678901234567890"), B("12345678901234567891")), -1);
    ck_i64("cmp neg", pa_big_cmp(B("-12345678901234567890"), B("-12345678901234567891")), 1);
    ck_i64("cmp zero", pa_big_cmp(B("0"), B("-0")), 0);
    /* f64 转换 */
    ck_d("2^53+1 -> f64", pa_big_to_f64(B("9007199254740993")), 9007199254740992.0);
    ck_d("2^53+3 -> f64", pa_big_to_f64(B("9007199254740995")), 9007199254740996.0);
    ck_d("big -> f64", pa_big_to_f64(B("1267650600228229401496703205376")), 1.2676506002282294e30);
    ck_d("neg big -> f64", pa_big_to_f64(B("-1267650600228229401496703205376")), -1.2676506002282294e30);
    ck_str("from_f64 1e30", pa_big_from_f64(1e30), "1000000000000000019884624838656");
    ck_str("from_f64 -2.5", pa_big_from_f64(-2.5), "-2");
    ck_str("from_f64 2^64", pa_big_from_f64(18446744073709551616.0), "18446744073709551616");
    /* cmp_f64 精确 */
    ck_i64("big vs 5.5", pa_big_cmp_f64(B("5"), 5.5), -1);
    ck_i64("big vs 5.0", pa_big_cmp_f64(B("5"), 5.0), 0);
    ck_i64("big vs -5.5", pa_big_cmp_f64(B("-6"), -5.5), -1);
    ck_i64("2^100 vs 1e30", pa_big_cmp_f64(B("1267650600228229401496703205376"), 1e30), 1);
    /* 解析 */
    ck_str("parse hex", pa_big_from_cstr("0xffffffffffffffffffffffff", 0), "79228162514264337593543950335");
    ck_str("parse base16", pa_big_from_cstr("ff", 16), "255");
    ck_str("parse neg", pa_big_from_cstr("-123456789012345678901234567890", 10), "-123456789012345678901234567890");
    ck_str("parse underscore", pa_big_from_cstr("1_000_000", 10), "1000000");
    /* 进制输出 */
    PyAdamasStr* hx = pa_big_to_base(B("-255"), 16);
    char hb[64]; memcpy(hb, hx->data, (size_t)hx->len); hb[hx->len] = 0;
    if (strcmp(hb, "-0xff")) { printf("FAIL hex: %s\n", hb); ++fails; }
    PyAdamasStr* bn = pa_big_to_base(B("10"), 2);
    char bb[64]; memcpy(bb, bn->data, (size_t)bn->len); bb[bn->len] = 0;
    if (strcmp(bb, "0b1010")) { printf("FAIL bin: %s\n", bb); ++fails; }
    PyAdamasStr* oc = pa_big_to_base(B("-8"), 8);
    char ob[64]; memcpy(ob, oc->data, (size_t)oc->len); ob[oc->len] = 0;
    if (strcmp(ob, "-0o10")) { printf("FAIL oct: %s\n", ob); ++fails; }
    /* 大数 hex 往返 */
    PyAdamasBig* hh = pa_big_from_cstr("123456789abcdef0123456789abcdef0", 16);
    ck_str("hex roundtrip", hh, "24197857203266734864793317670504947440");
    printf(fails ? "UNIT FAIL %d\n" : "UNIT OK\n", fails);
    return fails ? 1 : 0;
}
