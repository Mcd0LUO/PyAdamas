// rt_functools.c —— functools 的运行时支持
#include "rt_internal.h"

/* pyadamas 函数值 = 闭包对象：[0] 代码地址，%r10 传闭包指针（环境）。
   C 没有隐藏寄存器参数，运行时回调用跳板。reduce 要两参数回调：
   整数版把 rsi/rdx 挪到 rdi/rsi，浮点版的参数已经在 xmm0/xmm1，只搬
   闭包指针。 */
__asm__(
    ".text\n"
    ".globl pa_call2\n"
    PA_TYPE_FUNC(pa_call2)
    "pa_call2:\n"
    "\tmovq %rdi, %r10\n"
    "\tmovq (%rdi), %r11\n"
    "\tmovq %rsi, %rdi\n"
    "\tmovq %rdx, %rsi\n"
    "\txorl %eax, %eax\n"
    "\tjmp *%r11\n"
    PA_SIZE_FUNC(pa_call2));

__asm__(
    ".text\n"
    ".globl pa_call2_f\n"
    PA_TYPE_FUNC(pa_call2_f)
    "pa_call2_f:\n"
    "\tmovq %rdi, %r10\n"
    "\tmovq (%rdi), %r11\n"
    "\txorl %eax, %eax\n"
    "\tjmp *%r11\n"
    PA_SIZE_FUNC(pa_call2_f));

extern int64_t pa_call2(void* fn, int64_t a, int64_t b);
extern double pa_call2_f(void* fn, double a, double b);

/* reduce(f, xs[, init])：折叠。acc 始终持有一份引用，每轮换手时释放
   旧的那份，堆元素不会在长列表上堆积。 */
int64_t pa_functools_reduce(void* fn, PyAdamasList* xs, int64_t init, int64_t has_init) {
    if (!fn) pa_raise_c("TypeError", "reduce() arg 1 must be a function");
    if (!xs || xs->len == 0) {
        if (!has_init)
            pa_raise_c("TypeError", "reduce() of empty iterable with no initial value");
        pa_rc_inc_unknown(init);
        return init;
    }
    int64_t acc;
    int64_t i;
    if (has_init) {
        acc = init;
        i = 0;
    } else {
        acc = xs->data[0];
        i = 1;
    }
    pa_rc_inc_unknown(acc);
    for (; i < xs->len; ++i) {
        int64_t nx = pa_call2(fn, acc, xs->data[i]);
        pa_rc_dec_unknown(acc);
        acc = nx;
    }
    return acc;
}

/* float 版：元素按 double 位模式存，回调走 xmm。 */
double pa_functools_reduce_f(void* fn, PyAdamasList* xs, double init, int64_t has_init) {
    if (!fn) pa_raise_c("TypeError", "reduce() arg 1 must be a function");
    if (!xs || xs->len == 0) {
        if (!has_init)
            pa_raise_c("TypeError", "reduce() of empty iterable with no initial value");
        return init;
    }
    double* d = (double*)xs->data;
    double acc;
    int64_t i;
    if (has_init) {
        acc = init;
        i = 0;
    } else {
        acc = d[0];
        i = 1;
    }
    for (; i < xs->len; ++i) acc = pa_call2_f(fn, acc, d[i]);
    return acc;
}
