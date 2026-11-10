// rt_complex.c —— 复数运算与打印
#include "rt_internal.h"

/* ── 复数 ─────────────────────────────────────────────────────── */

Py23Complex* py23_complex_new(double re, double im) {
    Py23Complex* c = (Py23Complex*)rc_alloc((int64_t)sizeof(Py23Complex), NULL, NULL,
                                            PY23_RC_OBJ);
    c->magic = PY23_COMPLEX_MAGIC;
    c->re = re;
    c->im = im;
    return c;
}

Py23Complex* py23_complex_add(Py23Complex* a, Py23Complex* b) {
    return py23_complex_new(a->re + b->re, a->im + b->im);
}

Py23Complex* py23_complex_sub(Py23Complex* a, Py23Complex* b) {
    return py23_complex_new(a->re - b->re, a->im - b->im);
}

Py23Complex* py23_complex_mul(Py23Complex* a, Py23Complex* b) {
    return py23_complex_new(a->re * b->re - a->im * b->im,
                            a->re * b->im + a->im * b->re);
}

Py23Complex* py23_complex_div(Py23Complex* a, Py23Complex* b) {
    double d = b->re * b->re + b->im * b->im;
    if (d == 0.0) py23_raise_c("ZeroDivisionError", "complex division by zero");
    return py23_complex_new((a->re * b->re + a->im * b->im) / d,
                            (a->im * b->re - a->re * b->im) / d);
}

Py23Complex* py23_complex_neg(Py23Complex* a) {
    return py23_complex_new(-a->re, -a->im);
}

double py23_complex_abs(Py23Complex* a) { return hypot(a->re, a->im); }

int64_t py23_complex_eq(Py23Complex* a, Py23Complex* b) {
    return a->re == b->re && a->im == b->im;
}

int64_t py23_complex_truth(Py23Complex* a) { return a->re != 0.0 || a->im != 0.0; }

static void complex_part(char* buf, size_t n, double v) {
    if (v == (double)(int64_t)v && v > -1e15 && v < 1e15)
        snprintf(buf, n, "%lld", (long long)v);
    else
        format_double(buf, n, v);
}

Py23Str* py23_complex_str(Py23Complex* a) {
    char re[64], im[64], buf[160];
    complex_part(re, sizeof re, a->re);
    complex_part(im, sizeof im, a->im);
    if (a->re == 0.0) {
        snprintf(buf, sizeof buf, "%sj", im);
    } else if (a->im < 0.0) {
        snprintf(buf, sizeof buf, "(%s%sj)", re, im);
    } else {
        snprintf(buf, sizeof buf, "(%s+%sj)", re, im);
    }
    return str_new(buf, (int64_t)strlen(buf));
}

