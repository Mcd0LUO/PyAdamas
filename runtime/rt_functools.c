// rt_functools.c —— functools 的运行时支持
#include "rt_internal.h"

/* pyadamas 函数值 = 闭包对象：[0] 代码地址，%r10 传闭包指针（环境）。
   C 没有隐藏寄存器参数，运行时回调用跳板。reduce 要两参数回调：
   整数版把 rsi/rdx 挪到 rdi/rsi，浮点版的参数已经在 xmm0/xmm1，只搬
   闭包指针。 */
/* 跳板按 C 调用约定接实参（它由这里的 C 代码 call），被跳进去的函数体
   按 ABI 收实参。Win64 下 fn 在 rcx、两个整型实参在 rdx/r8，浮点第 2/3
   个位置是 xmm1/xmm2（第 0 个槽被 fn 指针占了），而函数体期望 xmm0/xmm1。 */
#if defined(_WIN32)
__asm__(
    ".text\n"
    ".globl pa_call2\n"
    PA_TYPE_FUNC(pa_call2)
    "pa_call2:\n"
    "\tmovq %rcx, %r10\n"
    "\tmovq (%rcx), %r11\n"
    "\tmovq %rdx, %rcx\n"
    "\tmovq %r8, %rdx\n"
    "\tjmp *%r11\n"
    PA_SIZE_FUNC(pa_call2));


#else
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


#endif

extern int64_t pa_call2(void* fn, int64_t a, int64_t b);
extern double pa_call2_f(void* fn, double a, double b);
/* pa_call2_f 与统一调用约定对齐：闭包里 +0 指向的入口（shim 或函数本体）
   一律期望「第 i 个实参进第 i 个整型寄存器、返回值在 %rax」。C 侧传进来的
   是 double（在 xmm 里），这里搬成位模式进整型寄存器；返回后再把 %rax 搬
   回 %xmm0，维持本函数「返回 double」的 C 签名。 */
#if defined(_WIN32)
__asm__(
    ".text\n"
    ".globl pa_call2_f\n"
    PA_TYPE_FUNC(pa_call2_f)
    "pa_call2_f:\n"
    "\tsubq $8, %rsp\n"
    "\tmovq %rcx, %r10\n"
    "\tmovq (%rcx), %r11\n"
    "\tmovq %xmm1, %rcx\n"
    "\tmovq %xmm2, %rdx\n"
    "\txorl %eax, %eax\n"
    "\tcall *%r11\n"
    "\tmovq %rax, %xmm0\n"
    "\taddq $8, %rsp\n"
    "\tret\n"
    PA_SIZE_FUNC(pa_call2_f));
#else
__asm__(
    ".text\n"
    ".globl pa_call2_f\n"
    PA_TYPE_FUNC(pa_call2_f)
    "pa_call2_f:\n"
    "\tsubq $8, %rsp\n"
    "\tmovq %rdi, %r10\n"
    "\tmovq (%rdi), %r11\n"
    "\tmovq %xmm0, %rdi\n"
    "\tmovq %xmm1, %rsi\n"
    "\txorl %eax, %eax\n"
    "\tcall *%r11\n"
    "\tmovq %rax, %xmm0\n"
    "\taddq $8, %rsp\n"
    "\tret\n"
    PA_SIZE_FUNC(pa_call2_f));
#endif

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
