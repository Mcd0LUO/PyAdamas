// rt_any.c —— unknown（动态值）的最小判别支持：float 装箱 + 动态算术/比较
//
// 设计背景见 docs/unknown-repr-design.md（0.324）。要点：
//
//   * 静态类型已知的路径**完全不变**：本文件的函数只有静态类型是 unknown
//     的表达式才会调用，数值热路径不会产生任何调用。
//   * unknown 仍然是一个 i64 机器字（ABI 不变），但 **float 进入 unknown 域
//     时统一装箱**成 PYADAMAS_RC_CPYFLOAT（复用 CPython 桥已有的 8 字节
//     payload 盒）。这样 unknown 里 int / float / None / bool / 堆指针可以
//     靠「堆探针 + 对象 kind」分开：CPYFLOAT 就是 float 标签。
//   * 装箱只在真正无法静态定型的路径发生（异构容器、object 槽/形参/返回），
//     热路径不装箱。
//
// 本文件只提供最小判别所需的三类入口：
//   pa_any_from_f64 / pa_any_to_f64 / pa_any_to_i64   —— 装箱/拆箱
//   pa_any_arith / pa_any_neg                          —— 动态算术
//   pa_any_cmp                                         —— 动态比较
// 字符串 <-> unknown 的转换复用 rt_str.c 的 pa_str_any（已扩展 CPYFLOAT）。

#include "rt_internal.h"

#include <math.h>

/* 动态算术/比较的操作码：与 src/lower_expr_ops.cpp 里的常量逐字对应。 */
enum {
    PA_ANY_ADD = 0,
    PA_ANY_SUB = 1,
    PA_ANY_MUL = 2,
    PA_ANY_DIV = 3,
    PA_ANY_FLOORDIV = 4,
    PA_ANY_MOD = 5,
};

enum {
    PA_ANY_EQ = 0,
    PA_ANY_NE = 1,
    PA_ANY_LT = 2,
    PA_ANY_LE = 3,
    PA_ANY_GT = 4,
    PA_ANY_GE = 5,
};

typedef enum {
    ANY_INT = 0,   /* 非堆标量：按 int64 解释 */
    ANY_FLOAT = 1, /* CPYFLOAT 盒 */
    ANY_STR = 2,
    ANY_OTHER = 3, /* 其它堆对象（list/dict/类实例/...） */
} AnyKind;

typedef struct {
    AnyKind kind;
    int64_t i;
    double f;
    PyAdamasStr* s;
} AnyVal;

static void any_decode(int64_t v, AnyVal* o);

/* 实参的静态域描述符（与 src/lower_expr_ops.cpp 的 any_desc 对应）：
     'i' int/bool   —— 原始 int64
     'f' float      —— double 位模式（xmm 经 movq 传进整型实参寄存器）
     's' str        —— 堆字符串指针
     'o' 其它堆对象 —— list/dict/类实例等
     'x' unknown    —— 运行期按堆探针 + kind 判别
   静态域已知的实参不需要探针，也不改变代码生成（仍是同一条 movq）。 */
static void any_decode_typed(int64_t v, int64_t desc, AnyVal* o) {
    switch (desc) {
        case 'i':
            o->kind = ANY_INT;
            o->i = v;
            o->f = 0.0;
            o->s = NULL;
            return;
        case 'f':
            o->kind = ANY_FLOAT;
            o->i = 0;
            memcpy(&o->f, &v, sizeof o->f);
            o->s = NULL;
            return;
        case 's':
            o->kind = ANY_STR;
            o->i = 0;
            o->f = 0.0;
            o->s = (PyAdamasStr*)(uintptr_t)v;
            return;
        case 'o':
            o->kind = ANY_OTHER;
            o->i = v;
            o->f = 0.0;
            o->s = NULL;
            return;
        default: break;
    }
    any_decode(v, o);
}

/* 把 unknown 机器字解码成「判别后的值」。堆探针与 RC/GC 用的是同一份
   判定（pa_unknown_is_heap），所以「是不是堆」在这里只有一次探测成本。 */
static void any_decode(int64_t v, AnyVal* o) {
    o->kind = ANY_INT;
    o->i = v;
    o->f = 0.0;
    o->s = NULL;
    if (!pa_unknown_is_heap(v)) return;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    switch (h->kind) {
        case PYADAMAS_RC_CPYFLOAT:
            o->kind = ANY_FLOAT;
            memcpy(&o->f, (void*)(uintptr_t)v, sizeof o->f);
            return;
        case PYADAMAS_RC_STR:
            o->kind = ANY_STR;
            o->s = (PyAdamasStr*)(uintptr_t)v;
            return;
        default:
            o->kind = ANY_OTHER;
            return;
    }
}

/* float -> unknown：分配一个 CPYFLOAT 盒（8 字节 payload 存 double 位模式）。
   与 rt_cpy.c 的 cpy_float_box 同款：必须 tracked 分配，unknown 探针靠
   RC 索引识别堆指针。 */
static int64_t any_float_box(double d) {
    int64_t* p = (int64_t*)rc_alloc((int64_t)sizeof(int64_t), NULL, NULL,
                                    PYADAMAS_RC_CPYFLOAT);
    memcpy(p, &d, sizeof d);
    return (int64_t)(uintptr_t)p;
}

/* ── 常量 float 盒缓存 ─────────────────────────────────────────
   异构容器里反复写入同一个字面量（counts["x"] = 1.5、append(2.5)）时，
   每次装箱都要 malloc + 索引插入。这里用 256 槽直接映射缓存「值位模式
   -> 盒」：命中只做一次哈希 + 一次 rc_inc（叶子快路径），零分配。
   槽里每个盒由缓存自己持有一个引用（rc>=1），进程退出时统一还掉。

   语义注意：命中返回的是同一个对象，所以 unknown 域里两个相等的常量
   float 用 is 比较会为 True。CPython 对 float 字面量的 interning 同样
   是实现细节（小浮点常量常被 code object 复用），项目文档把 float 的
   is 语义标为不保证。 */
#define ANY_FLOAT_CACHE_SLOTS 256
static int64_t any_fcache[ANY_FLOAT_CACHE_SLOTS];
static uint64_t any_fcache_bits[ANY_FLOAT_CACHE_SLOTS];

int64_t pa_any_from_f64(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint64_t h = bits * 0x9e3779b97f4a7c15ull;
    h ^= h >> 29;
    const size_t slot = (size_t)h & (ANY_FLOAT_CACHE_SLOTS - 1);
    if (any_fcache[slot]) {
        if (any_fcache_bits[slot] == bits) {
            pa_rc_inc((void*)(uintptr_t)any_fcache[slot]);
            return any_fcache[slot];
        }
        /* 槽被另一个常量占用：不驱逐（可能还有引用），走普通装箱。 */
        return any_float_box(v);
    }
    int64_t box = any_float_box(v);
    any_fcache[slot] = box;
    any_fcache_bits[slot] = bits;
    /* 缓存自己持有一份引用；调用方再拿一份。 */
    pa_rc_inc((void*)(uintptr_t)box);
    return box;
}

void pa_any_float_cache_shutdown(void) {
    for (size_t i = 0; i < ANY_FLOAT_CACHE_SLOTS; ++i) {
        if (any_fcache[i]) {
            int64_t box = any_fcache[i];
            any_fcache[i] = 0;
            pa_rc_dec((void*)(uintptr_t)box);
        }
    }
}

/* isinstance(unknown, float)：0.324 之后 float 在 unknown 里带 CPYFLOAT
   标签，可以精确判定（int/bool/None 仍是裸机器字，无法区分）。 */
int64_t pa_any_is_float(int64_t v) {
    if (!pa_unknown_is_heap(v)) return 0;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    return h->kind == PYADAMAS_RC_CPYFLOAT ? 1 : 0;
}

double pa_any_to_f64(int64_t v) {
    AnyVal a;
    any_decode(v, &a);
    switch (a.kind) {
        case ANY_FLOAT: return a.f;
        case ANY_INT: return (double)a.i;
        case ANY_STR:
            pa_raise_c("TypeError", "must be real number, not str");
            return 0.0;
        default:
            pa_raise_c("TypeError", "must be real number, not object");
            return 0.0;
    }
}

int64_t pa_any_to_i64(int64_t v) {
    AnyVal a;
    any_decode(v, &a);
    switch (a.kind) {
        case ANY_FLOAT:
            if (!isfinite(a.f) || a.f >= 9223372036854775808.0 ||
                a.f < -9223372036854775808.0)
                pa_raise_c("OverflowError", "cannot convert float to integer");
            return (int64_t)a.f;
        case ANY_INT: return a.i;
        case ANY_STR:
            pa_raise_c("TypeError", "int() argument must be a number, not str");
            return 0;
        default:
            pa_raise_c("TypeError", "int() argument must be a number, not object");
            return 0;
    }
}

/* Python 的 // 向下取整、% 与除数同号；x86 的 idiv 向零取整。与 codegen
   的 gen_floordiv / gen_mod 逐条同口径（含 INT64_MIN / -1 的溢出保护）。 */
static int64_t any_int_floordiv(int64_t a, int64_t b) {
    if (b == 0) {
        pa_panic_div0();
        return 0;
    }
    if (a == INT64_MIN && b == -1) {
        pa_panic_overflow();
        return 0;
    }
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

static int64_t any_int_mod(int64_t a, int64_t b) {
    if (b == 0) {
        pa_panic_div0();
        return 0;
    }
    if (a == INT64_MIN && b == -1) {
        pa_panic_overflow();
        return 0;
    }
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

static double any_floor_div_d(double a, double b) {
    double q = floor(a / b);
    /* -0.0 的符号按除数归一（CPython: -0.0 // 3.0 == 0.0，-0.0 // -3.0 == 0.0） */
    if (q == 0.0) return 0.0;
    return q;
}

static double any_mod_d(double a, double b) {
    double r = fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    return r;
}

/* 动态算术：op 见上面的 PA_ANY_*。返回值遵循 unknown 域的约定：
   int 结果直接返回机器字，float 结果返回新分配的 CPYFLOAT 盒（rc=1），
   str 结果返回新字符串（rc=1）。调用方（降级器）负责把这个初始引用交给
   临时栈（RcTempUnknown）。 */
int64_t pa_any_arith(int64_t op, int64_t a, int64_t b, int64_t desc_a, int64_t desc_b) {
    AnyVal x, y;
    any_decode_typed(a, desc_a, &x);
    any_decode_typed(b, desc_b, &y);

    /* str 拼接：与 CPython 一致，只允许两边都是 str（str + 非 str 报错）。 */
    if (x.kind == ANY_STR || y.kind == ANY_STR) {
        if (op != PA_ANY_ADD || x.kind != ANY_STR || y.kind != ANY_STR)
            pa_raise_c("TypeError", "unsupported operand type(s) for str");
        PyAdamasStr* s = pa_str_concat(x.s, y.s);
        return (int64_t)(uintptr_t)s;
    }

    bool numeric = (x.kind == ANY_INT || x.kind == ANY_FLOAT) &&
                   (y.kind == ANY_INT || y.kind == ANY_FLOAT);
    if (!numeric) pa_raise_c("TypeError", "unsupported operand type(s)");

    int use_float = (x.kind == ANY_FLOAT || y.kind == ANY_FLOAT);
    if (op == PA_ANY_DIV) use_float = 1;

    if (!use_float) {
        switch (op) {
            case PA_ANY_ADD: return x.i + y.i; /* int64 回绕语义，与静态 int 路径一致 */
            case PA_ANY_SUB: return x.i - y.i;
            case PA_ANY_MUL: return x.i * y.i;
            case PA_ANY_FLOORDIV: return any_int_floordiv(x.i, y.i);
            case PA_ANY_MOD: return any_int_mod(x.i, y.i);
            default: break;
        }
    }

    double da = x.kind == ANY_FLOAT ? x.f : (double)x.i;
    double db = y.kind == ANY_FLOAT ? y.f : (double)y.i;
    switch (op) {
        case PA_ANY_ADD: return any_float_box(da + db);
        case PA_ANY_SUB: return any_float_box(da - db);
        case PA_ANY_MUL: return any_float_box(da * db);
        case PA_ANY_DIV:
            if (db == 0.0) pa_raise_c("ZeroDivisionError", "float division by zero");
            return any_float_box(da / db);
        case PA_ANY_FLOORDIV:
            if (db == 0.0) pa_raise_c("ZeroDivisionError", "float floor division by zero");
            return any_float_box(any_floor_div_d(da, db));
        case PA_ANY_MOD:
            if (db == 0.0) pa_raise_c("ZeroDivisionError", "float modulo");
            return any_float_box(any_mod_d(da, db));
        default: break;
    }
    return 0;
}

int64_t pa_any_neg(int64_t v, int64_t dv) {
    AnyVal a;
    any_decode_typed(v, dv, &a);
    if (a.kind == ANY_FLOAT) return any_float_box(-a.f);
    if (a.kind == ANY_INT) return (int64_t)(0u - (uint64_t)a.i); /* 回绕语义 */
    pa_raise_c("TypeError", "bad operand type for unary -");
    return 0;
}

/* 动态比较：op 见 PA_ANY_EQ..GE。数值按 double 提升比较（int/float 混比
   与 CPython 一致），str 用字节序 cmp，其它堆对象只支持 ==/!=（按同一性，
   与项目既有的容器比较口径一致）。返回 0/1。 */
int64_t pa_any_cmp(int64_t op, int64_t a, int64_t b, int64_t desc_a, int64_t desc_b) {
    AnyVal x, y;
    any_decode_typed(a, desc_a, &x);
    any_decode_typed(b, desc_b, &y);
    int eq = 0, lt = 0, gt = 0;

    bool x_num = (x.kind == ANY_INT || x.kind == ANY_FLOAT);
    bool y_num = (y.kind == ANY_INT || y.kind == ANY_FLOAT);
    if (x_num && y_num) {
        double da = x.kind == ANY_FLOAT ? x.f : (double)x.i;
        double db = y.kind == ANY_FLOAT ? y.f : (double)y.i;
        eq = da == db;
        lt = da < db;
        gt = da > db;
    } else if (x.kind == ANY_STR && y.kind == ANY_STR) {
        int64_t c = pa_str_cmp(x.s, y.s);
        eq = c == 0;
        lt = c < 0;
        gt = c > 0;
    } else {
        if (op != PA_ANY_EQ && op != PA_ANY_NE)
            pa_raise_c("TypeError", "unsupported comparison between different types");
        eq = (a == b);
    }
    switch (op) {
        case PA_ANY_EQ: return eq;
        case PA_ANY_NE: return !eq;
        case PA_ANY_LT: return lt;
        case PA_ANY_LE: return lt || eq;
        case PA_ANY_GT: return gt;
        case PA_ANY_GE: return gt || eq;
        default: return 0;
    }
}
