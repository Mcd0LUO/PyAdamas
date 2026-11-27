// rt_complex.c —— 复数运算与打印
#include "rt_internal.h"

/* ── 复数 ─────────────────────────────────────────────────────── */

PyAdamasComplex* pa_complex_new(double re, double im) {
    PyAdamasComplex* c = (PyAdamasComplex*)rc_alloc((int64_t)sizeof(PyAdamasComplex), NULL, NULL,
                                            PYADAMAS_RC_OBJ_COMPLEX);
    c->magic = PYADAMAS_COMPLEX_MAGIC;
    c->re = re;
    c->im = im;
    return c;
}

PyAdamasComplex* pa_complex_add(PyAdamasComplex* a, PyAdamasComplex* b) {
    return pa_complex_new(a->re + b->re, a->im + b->im);
}

PyAdamasComplex* pa_complex_sub(PyAdamasComplex* a, PyAdamasComplex* b) {
    return pa_complex_new(a->re - b->re, a->im - b->im);
}

PyAdamasComplex* pa_complex_mul(PyAdamasComplex* a, PyAdamasComplex* b) {
    return pa_complex_new(a->re * b->re - a->im * b->im,
                            a->re * b->im + a->im * b->re);
}

PyAdamasComplex* pa_complex_div(PyAdamasComplex* a, PyAdamasComplex* b) {
    double d = b->re * b->re + b->im * b->im;
    if (d == 0.0) pa_raise_c("ZeroDivisionError", "complex division by zero");
    return pa_complex_new((a->re * b->re + a->im * b->im) / d,
                            (a->im * b->re - a->re * b->im) / d);
}

PyAdamasComplex* pa_complex_neg(PyAdamasComplex* a) {
    return pa_complex_new(-a->re, -a->im);
}

/* complex 访问器（0.534）：re/im 是两个 double；conjugate 造新对象。 */
double pa_complex_real(PyAdamasComplex* a) { return a ? a->re : 0.0; }
double pa_complex_imag(PyAdamasComplex* a) { return a ? a->im : 0.0; }
PyAdamasComplex* pa_complex_conj(PyAdamasComplex* a) {
    return pa_complex_new(a ? a->re : 0.0, a ? -a->im : 0.0);
}

double pa_complex_abs(PyAdamasComplex* a) { return hypot(a->re, a->im); }

int64_t pa_complex_eq(PyAdamasComplex* a, PyAdamasComplex* b) {
    return a->re == b->re && a->im == b->im;
}

int64_t pa_complex_truth(PyAdamasComplex* a) { return a->re != 0.0 || a->im != 0.0; }

static void complex_part(char* buf, size_t n, double v) {
    if (v == (double)(int64_t)v && v > -1e15 && v < 1e15)
        snprintf(buf, n, "%lld", (long long)v);
    else
        format_double(buf, n, v);
}

PyAdamasStr* pa_complex_str(PyAdamasComplex* a) {
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

