/* 随机对拍驱动：stdin 每行 "op a b"（十进制串），stdout 输出结果。 */
#include "rt_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void out_big(PyAdamasBig* b) {
    /* 模拟生成代码：结果先挂根（temp 栈），再调可能分配的 str 入口。 */
    pa_gc_pin(b);
    PyAdamasStr* s = pa_big_str(b);
    fwrite(s->data, 1, (size_t)s->len, stdout);
    fputc('\n', stdout);
    pa_rc_dec(s);
    pa_gc_unpin(b);
    pa_rc_dec(b);
}
int main(void) {
    char op[32], abuf[8192], bbuf[8192];
    int gc_mode = getenv("FUZZ_GC") != NULL;
    volatile int stack_top_marker = 0;
    if (gc_mode) pa_gc_init(NULL, 0, (void*)&stack_top_marker);
    int64_t line_no = 0;
    while (scanf("%31s %8191s %8191s", op, abuf, bbuf) == 3) {
        if (gc_mode && (line_no++ % 64) == 0) pa_gc_collect();
        if (getenv("FUZZ_DEBUG")) fprintf(stderr, "LINE %s %.60s %.20s\n", op, abuf, bbuf);
        if (!strcmp(op, "parse")) {
            PyAdamasBig* bb = pa_big_from_cstr(bbuf, 10);
            pa_gc_pin(bb);
            PyAdamasBig* p = pa_big_from_cstr(abuf, pa_big_to_i64(bb));
            pa_gc_unpin(bb);
            pa_rc_dec(bb);
            out_big(p);
            continue;
        }
        PyAdamasBig* a = pa_big_from_cstr(abuf, 10);
        pa_gc_pin(a);
        if (!strcmp(op, "cmpf")) {
            /* b 是 double 的十进制串，不是 big 字面量。 */
            printf("%lld\n", (long long)pa_big_cmp_f64(a, strtod(bbuf, NULL)));
            pa_gc_unpin(a);
            pa_rc_dec(a);
            continue;
        }
        PyAdamasBig* b = pa_big_from_cstr(bbuf, 10);
        pa_gc_pin(b);
        if (!strcmp(op, "add")) out_big(pa_big_add(a, b));
        else if (!strcmp(op, "sub")) out_big(pa_big_sub(a, b));
        else if (!strcmp(op, "mul")) out_big(pa_big_mul(a, b));
        else if (!strcmp(op, "floordiv")) out_big(pa_big_floordiv(a, b));
        else if (!strcmp(op, "mod")) out_big(pa_big_mod(a, b));
        else if (!strcmp(op, "pow")) out_big(pa_big_pow(a, b));
        else if (!strcmp(op, "and")) out_big(pa_big_and(a, b));
        else if (!strcmp(op, "or")) out_big(pa_big_or(a, b));
        else if (!strcmp(op, "xor")) out_big(pa_big_xor(a, b));
        else if (!strcmp(op, "shl")) out_big(pa_big_lshift(a, b));
        else if (!strcmp(op, "shr")) out_big(pa_big_rshift(a, b));
        else if (!strcmp(op, "neg")) out_big(pa_big_neg(a));
        else if (!strcmp(op, "not")) out_big(pa_big_not(a));
        else if (!strcmp(op, "truediv")) printf("%.17g\n", pa_big_truediv(a, b));
        else if (!strcmp(op, "abs")) out_big(pa_big_abs(a));
        else if (!strcmp(op, "cmp")) printf("%lld\n", (long long)pa_big_cmp(a, b));
        else if (!strcmp(op, "eq")) printf("%lld\n", (long long)pa_big_eq(a, b));
        else if (!strcmp(op, "truth")) printf("%lld\n", (long long)pa_big_truth(a));
        else if (!strcmp(op, "to_i64")) printf("%lld\n", (long long)pa_big_to_i64(a));
        else if (!strcmp(op, "to_f64")) printf("%.17g\n", pa_big_to_f64(a));
        else if (!strcmp(op, "hex")) {
            PyAdamasStr* s = pa_big_to_base(a, 16);
            fwrite(s->data, 1, (size_t)s->len, stdout); fputc('\n', stdout);
            pa_rc_dec(s);
        } else if (!strcmp(op, "bin")) {
            PyAdamasStr* s = pa_big_to_base(a, 2);
            fwrite(s->data, 1, (size_t)s->len, stdout); fputc('\n', stdout);
            pa_rc_dec(s);
        } else if (!strcmp(op, "oct")) {
            PyAdamasStr* s = pa_big_to_base(a, 8);
            fwrite(s->data, 1, (size_t)s->len, stdout); fputc('\n', stdout);
            pa_rc_dec(s);
        } else { printf("BADOP\n"); }
        pa_gc_unpin(a);
        pa_gc_unpin(b);
        pa_rc_dec(a);
        pa_rc_dec(b);
    }
    return 0;
}
