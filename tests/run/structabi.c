/* structabi.c —— 按值结构体 ABI 的测试辅助符号
 *
 * pair   16 字节：两个 SSE chunk（xmm 传参/返回）
 * triple 24 字节：MEMORY 类，参数走栈、返回用隐藏指针（rdi）
 */
struct pair {
    double a;
    double b;
};

struct triple {
    long a;
    long b;
    long c;
};

struct pair pair_add(struct pair p, struct pair q) {
    struct pair r;
    r.a = p.a + q.a;
    r.b = p.b + q.b;
    return r;
}

struct triple triple_sum(struct triple p) {
    struct triple r;
    r.a = p.a + 1;
    r.b = p.b + 2;
    r.c = p.c + 3;
    return r;
}

/* 标量 + MEMORY 结构体 + 标量：验证隐藏指针与寄存器移位。 */
long triple_mix(long x, struct triple p, long y) {
    return x + p.a + p.b + p.c + y;
}

/* extern 变量宽度测试。 */
long cvar_i = 1234567890123L;
unsigned cvar_u = 4294967295u;
double cvar_f = 2.5;
_Bool cvar_b = 1;

/* 4 字节指针解引用：符号/零扩展。 */
#include <stdint.h>

int32_t *ptr_i32(void) {
    static int32_t v[2] = {-5, 7};
    return v;
}

uint32_t *ptr_u32(void) {
    static uint32_t v[2] = {4000000000u, 1u};
    return v;
}

/* 回调：C 只传函数地址，由生成的跳板转交给 PyAdamas 闭包。 */
long cb_apply(long a, long b, long (*fn)(long, long)) {
    return fn(a, b);
}

double cb_apply_f(double x, double (*fn)(double)) {
    return fn(x);
}

int cb_apply_i32(int a, int b, int (*fn)(int, int)) {
    return fn(a, b);
}

long cb_fold(int n, long (*fn)(long, long), long init) {
    long acc = init;
    for (int i = 0; i < n; ++i) acc = fn(acc, i);
    return acc;
}

_Bool cb_apply_b(_Bool x, _Bool (*fn)(_Bool)) {
    return fn(x);
}
