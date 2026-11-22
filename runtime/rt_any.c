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
    ANY_BIG = 4,
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
        case 'b':
        case 'o':
            o->kind = desc == 'b' ? ANY_BIG : ANY_OTHER;
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
        case PYADAMAS_RC_BIGINT:
            o->kind = ANY_BIG;
            return;
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
        case ANY_BIG: return pa_big_to_f64((PyAdamasBig*)(uintptr_t)a.i);
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
        case ANY_BIG: return pa_big_to_i64((PyAdamasBig*)(uintptr_t)a.i);
        case ANY_STR:
            pa_raise_c("TypeError", "int() argument must be a number, not str");
            return 0;
        default:
            pa_raise_c("TypeError", "int() argument must be a number, not object");
            return 0;
    }
}

static PyAdamasBig* any_big_ptr(AnyVal a) {
    return (PyAdamasBig*)(uintptr_t)a.i;
}

PyAdamasBig* pa_any_to_big(int64_t v) {
    AnyVal a;
    any_decode(v, &a);
    switch (a.kind) {
        case ANY_BIG:
            pa_rc_inc(any_big_ptr(a));
            return any_big_ptr(a);
        case ANY_INT: return pa_big_from_i64(a.i);
        case ANY_FLOAT: return pa_big_from_f64(a.f);
        default: pa_raise_c("TypeError", "cannot convert object to big");
    }
    return NULL;
}

/* Fresh conversions transfer their initial reference to the temporary stack,
   which protects them both against collection and exceptional exits. */
static PyAdamasBig* any_big_operand(AnyVal a) {
    if (a.kind == ANY_BIG) return any_big_ptr(a);
    PyAdamasBig* b = a.kind == ANY_INT ? pa_big_from_i64(a.i) : pa_big_from_f64(a.f);
    pa_rc_temp(b);
    return b;
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

static bool any_num(AnyVal a) {
    return a.kind == ANY_INT || a.kind == ANY_FLOAT || a.kind == ANY_BIG;
}

/* A big operand selects the same integer promotion as the static big path. */
static PaAnyPair any_big_arith(int64_t op, AnyVal x, AnyVal y) {
    if (!any_num(x) || !any_num(y)) pa_raise_c("TypeError", "unsupported operand type(s)");
    int64_t mark = pa_rc_mark();
    PyAdamasBig* a = any_big_operand(x);
    PyAdamasBig* b = any_big_operand(y);
    PyAdamasBig* r = NULL;
    switch (op) {
        case PA_ANY_ADD: r = pa_big_add(a, b); break;
        case PA_ANY_SUB: r = pa_big_sub(a, b); break;
        case PA_ANY_MUL: r = pa_big_mul(a, b); break;
        case PA_ANY_FLOORDIV: r = pa_big_floordiv(a, b); break;
        case PA_ANY_MOD: r = pa_big_mod(a, b); break;
        case PA_ANY_DIV: {
            double d = pa_big_truediv(a, b);
            pa_rc_cleanup(mark);
            return (PaAnyPair){any_float_box(d), PA_TAG_FBOX};
        }
        default: pa_raise_c("TypeError", "unsupported big arithmetic operation");
    }
    pa_rc_cleanup(mark);
    return (PaAnyPair){(int64_t)(uintptr_t)r, PA_TAG_BIG};
}

static int64_t any_big_cmp(AnyVal x, AnyVal y) {
    if (y.kind == ANY_FLOAT) return pa_big_cmp_f64(any_big_ptr(x), y.f);
    if (x.kind == ANY_FLOAT) {
        int64_t c = pa_big_cmp_f64(any_big_ptr(y), x.f);
        return c == -2 ? c : -c;
    }
    int64_t mark = pa_rc_mark();
    PyAdamasBig* a = any_big_operand(x);
    PyAdamasBig* b = any_big_operand(y);
    int64_t c = pa_big_cmp(a, b);
    pa_rc_cleanup(mark);
    return c;
}

/* 动态算术：op 见上面的 PA_ANY_*。返回值遵循 unknown 域的约定：
   int 结果直接返回机器字，float 结果返回新分配的 CPYFLOAT 盒（rc=1），
   str 结果返回新字符串（rc=1）。调用方（降级器）负责把这个初始引用交给
   临时栈（RcTempUnknown）。 */
int64_t pa_any_arith(int64_t op, int64_t a, int64_t b, int64_t desc_a, int64_t desc_b) {
    AnyVal x, y;
    any_decode_typed(a, desc_a, &x);
    any_decode_typed(b, desc_b, &y);

    if (x.kind == ANY_BIG || y.kind == ANY_BIG) return any_big_arith(op, x, y).v;

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
    if (a.kind == ANY_BIG) return (int64_t)(uintptr_t)pa_big_neg(any_big_ptr(a));
    if (a.kind == ANY_FLOAT) return any_float_box(-a.f);
    if (a.kind == ANY_INT) return (int64_t)(0u - (uint64_t)a.i); /* 回绕语义 */
    pa_raise_c("TypeError", "bad operand type for unary -");
    return 0;
}

/* 动态比较：op 见 PA_ANY_EQ..GE。数值按 double 提升比较（int/float 混比
   与 CPython 一致），str 用字节序 cmp，其它堆对象只支持 ==/!=（按同一性，
   与项目既有的容器比较口径一致）。返回 0/1。 */
/* 未知域（desc != 具体种类）的相等判定：运行时认出堆对象后按内容比。
   覆盖 str（字节）、big（数值）、list/tuple（长度 + 逐元素递归，未知元素递归用本函数）；
   dict/set/类实例保持同一性（与既有容器比较口径一致）。
   注意：运行时无法区分 list 与 tuple（头 kind 都是 LIST），未知域下 [..] == (..)
   的跨类型语义会按内容判等 —— 已知保守性缺口，静态类型已知时由 lowerer 提前定。 */
static int any_unknown_eq(int64_t a, int64_t b, int depth) {
    if (a == b) return 1;
    if (depth > 32) return 0;  /* 自引用容器防御 */
    if (!pa_unknown_is_heap(a) || !pa_unknown_is_heap(b)) return 0;
    PyAdamasRc* ha = (PyAdamasRc*)(uintptr_t)((uint64_t)a - (uint64_t)PYADAMAS_RC_HDR);
    PyAdamasRc* hb = (PyAdamasRc*)(uintptr_t)((uint64_t)b - (uint64_t)PYADAMAS_RC_HDR);
    if (ha->kind == PYADAMAS_RC_STR && hb->kind == PYADAMAS_RC_STR)
        return pa_str_eq((PyAdamasStr*)a, (PyAdamasStr*)b) != 0;
    if (ha->kind == PYADAMAS_RC_BIGINT && hb->kind == PYADAMAS_RC_BIGINT)
        return pa_big_cmp((PyAdamasBig*)a, (PyAdamasBig*)b) == 0;
    if (ha->kind == PYADAMAS_RC_LIST && hb->kind == PYADAMAS_RC_LIST) {
        PyAdamasList* la = (PyAdamasList*)a;
        PyAdamasList* lb = (PyAdamasList*)b;
        if (la->len != lb->len) return 0;
        for (int64_t i = 0; i < la->len; ++i)
            if (!any_unknown_eq(la->data[i], lb->data[i], depth + 1)) return 0;
        return 1;
    }
    return 0;
}

int64_t pa_any_cmp(int64_t op, int64_t a, int64_t b, int64_t desc_a, int64_t desc_b) {
    AnyVal x, y;
    any_decode_typed(a, desc_a, &x);
    any_decode_typed(b, desc_b, &y);
    int eq = 0, lt = 0, gt = 0;

    bool x_num = (x.kind == ANY_INT || x.kind == ANY_FLOAT);
    bool y_num = (y.kind == ANY_INT || y.kind == ANY_FLOAT);
    if (any_num(x) && any_num(y) && (x.kind == ANY_BIG || y.kind == ANY_BIG)) {
        int64_t c = any_big_cmp(x, y);
        eq = c == 0;
        lt = c == -1;
        gt = c == 1;
    } else if (x_num && y_num) {
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
        eq = any_unknown_eq(a, b, 0);
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

/* ══════════════════════════════════════════════════════════════════
   P3 第一阶段（0.327）：shadow tag 跟值走 + float 盒原位复用。
   设计见 docs/p3-tagged-union-design.md。要点：
     * 值仍是旧 unknown i64（float 仍装箱），tag 是 RC/分派快路径的判据；
       tag==UNK(0) 一律回退上面的探针路径 —— 正确性不依赖 tag 传播完备。
     * 只有「封闭活跃域」模块（含动态算术/比较，codegen 门控）才会调这些
       入口；静态已知路径与 26 个热基准不产生任何调用。
   ══════════════════════════════════════════════════════════════════ */

/* tag 编码 PA_TAG_* 定义在 rt_internal.h（rt_str.c 的 repr 也要用）：
   P3 默认堆 ⇔ tag>=3；P2（PYADAMAS_TYPE_TAGS）堆 ⇔ 3..5 或 >=8。 */

/* P2（type_tags 构建）：unknown 形参 tag 数组的**跨调用侧信道**。
   调用方在 call 前写这里（数组指针或 0），被调序言读走并立即清零。
   用它而不是 %r11 是因为跨模块/间接调用点上 %r11 已被目标地址占用，
   而运行时回调（vthread/生成器 resume）根本不会设置 %r11 —— 侧信道
   在「每次生成代码的调用都显式写 0 或数组」下永远是干净的，回调里读到
   0 即回退 UNK。vthread 是协作式单线程，所以进程级一个变量足够。 */
int64_t pa_ptag_in;
/* 侧信道数组长度（= 被调形参个数）。被调按自己的形参个数校验，不匹配
   就整体忽略 —— 函数值/动态分派上签名不一致时宁可回退 UNK 也不能读错
   tag（错 tag 会按堆指针 inc/dec，直接 UAF/leak）。 */
int64_t pa_ptag_len;

/* 双字返回：SysV 下 {i64,i64} 按 INTEGER/INTEGER 分类，v 进 %rax、tag 进
   %rdx —— 生成代码按这个约定落盘（codegen_call_rt.cpp）。
   PaAnyPair 定义在 rt_internal.h（此处勿重复 typedef，匿名 struct 会冲突）。 */

/* 按 tag 解码。tag 健全性由传播规则保证（设计 §3.3）：>=3 必为活堆指针，
   可以直接解引用头/kind，不需要探针。UNK 回退 any_decode（探针）。 */
static void any_decode_tag(int64_t v, int64_t tag, AnyVal* o) {
    o->i = v;
    o->f = 0.0;
    o->s = NULL;
    switch (tag) {
        case PA_TAG_INT:
            o->kind = ANY_INT;
            return;
        case PA_TAG_FRAW:
            o->kind = ANY_FLOAT;
            memcpy(&o->f, &v, sizeof o->f);
            return;
        case PA_TAG_FBOX:
            o->kind = ANY_FLOAT;
            memcpy(&o->f, (void*)(uintptr_t)v, sizeof o->f);
            return;
        case PA_TAG_STR:
            o->kind = ANY_STR;
            o->s = (PyAdamasStr*)(uintptr_t)v;
            return;
        case PA_TAG_NONE:
            /* 算术域保持 P3 语义（None 就是裸 0）。类型内省另走 pa_any_type。 */
            o->kind = ANY_INT;
            o->i = 0;
            return;
        case PA_TAG_BOOL:
            o->kind = ANY_INT;
            return;
        case PA_TAG_BYTES:
            /* bytes 与 str 共用表示；算术域按 str 处理（拼接/比较口径一致）。 */
            o->kind = ANY_STR;
            o->s = (PyAdamasStr*)(uintptr_t)v;
            return;
        case PA_TAG_TUPLE:
        case PA_TAG_SET:
        case PA_TAG_CLASS:
            o->kind = ANY_OTHER;
            return;
        case PA_TAG_BIG:
            o->kind = ANY_BIG;
            return;
        case PA_TAG_HEAP: {
            /* 位图/容器只知道「是堆」，种类读头（一次加载，免探针）。 */
            PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
            if (h->kind == PYADAMAS_RC_CPYFLOAT) {
                o->kind = ANY_FLOAT;
                memcpy(&o->f, (void*)(uintptr_t)v, sizeof o->f);
            } else if (h->kind == PYADAMAS_RC_BIGINT) {
                o->kind = ANY_BIG;
            } else if (h->kind == PYADAMAS_RC_STR) {
                o->kind = ANY_STR;
                o->s = (PyAdamasStr*)(uintptr_t)v;
            } else {
                o->kind = ANY_OTHER;
            }
            return;
        }
        default:
            any_decode(v, o);
            return;
    }
}

/* i64 -> tag：容器/字段等无 tag 来源的 unknown 字，一次探针 + kind 得到
   tag（比「每次使用都探针」少）。非堆一律 INT（unknown 域里 float 已装箱，
   裸位模式不会从这些来源出现）。堆子类（tuple/set/bytes/类对象）在头 kind
   里分不开，回落 PA_TAG_HEAP（形状由头 kind 判定，见 pa_any_type）。 */
int64_t pa_any_load_tag(int64_t v) {
    if (!pa_unknown_is_heap(v)) return PA_TAG_INT;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (h->kind == PYADAMAS_RC_CPYFLOAT) return PA_TAG_FBOX;
    if (h->kind == PYADAMAS_RC_STR) return PA_TAG_STR;
    if (h->kind == PYADAMAS_RC_BIGINT) return PA_TAG_BIG;
    return PA_TAG_HEAP;
}

/* float 盒原位复用（设计 §4）：结果应为 float、reuse_ok（codegen 已证明
   该实参就是即将被结果覆盖的槽的当前值）、实参 tag==FBOX（tag 健全性保证
   可安全解引用）、盒 rc==1（唯一持有者就是那个槽；常量缓存盒/列表共享盒
   rc>=2 一律不复用）→ 原位写新 double，返回同指针并 rc++（调用方拿独立
   一份，槽的那份不动）。引用计数证明见设计 §4。 */
static int64_t any_reuse_float_box(int64_t v, int64_t tag, double d, int64_t reuse_ok) {
    if (!reuse_ok || tag != PA_TAG_FBOX) return 0;
    if (pa_rc_stats_on_fast || gc_sweeping) return 0;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (h->kind != PYADAMAS_RC_CPYFLOAT || h->rc != 1) return 0;
    memcpy((void*)(uintptr_t)v, &d, sizeof d);
    h->rc = 2; /* 槽的 1 + 产生者的 1 */
    return v;
}

static PaAnyPair any_pair_i64(int64_t v) { PaAnyPair p = {v, PA_TAG_INT}; return p; }
// Matches lower_any_big_binary. Do not use these opcodes with pa_any_arith.
enum { PA_ANY_BIT_AND, PA_ANY_BIT_OR, PA_ANY_BIT_XOR, PA_ANY_BIT_POW,
       PA_ANY_BIT_SHL, PA_ANY_BIT_SHR };

static int64_t any_word_bitop(int64_t op, int64_t a, int64_t b) {
    unsigned n = (unsigned)((uint64_t)b & 63);
    switch (op) {
        case PA_ANY_BIT_AND: return a & b;
        case PA_ANY_BIT_OR: return a | b;
        case PA_ANY_BIT_XOR: return a ^ b;
        // x86 shlq/sarq mask the count, even when its signed value is negative.
        case PA_ANY_BIT_SHL: return (int64_t)((uint64_t)a << n);
        case PA_ANY_BIT_SHR:
            return a >= 0 ? (int64_t)((uint64_t)a >> n)
                          : ~(int64_t)(~(uint64_t)a >> n);
        case PA_ANY_BIT_POW: return pa_pow_i64(a, b);
        default: panic_c("invalid dynamic bit operator"); return 0;
    }
}

static PaAnyPair any_big_binary(int64_t op, int64_t a, int64_t b, AnyVal x, AnyVal y,
                                bool u64_a, bool u64_b) {
    if (x.kind != ANY_BIG && y.kind != ANY_BIG)
        return any_pair_i64(any_word_bitop(op, a, b));
    if (!any_num(x) || !any_num(y) ||
        (op != PA_ANY_BIT_POW && (x.kind == ANY_FLOAT || y.kind == ANY_FLOAT)))
        pa_raise_c("TypeError", "unsupported operand type(s)");
    int64_t mark = pa_rc_mark();
    PyAdamasBig* p = u64_a ? pa_big_from_u64((uint64_t)a) : any_big_operand(x);
    if (u64_a) pa_rc_temp(p);
    PyAdamasBig* q = u64_b ? pa_big_from_u64((uint64_t)b) : any_big_operand(y);
    if (u64_b) pa_rc_temp(q);
    PyAdamasBig* r = NULL;
    switch (op) {
        case PA_ANY_BIT_AND: r = pa_big_and(p, q); break;
        case PA_ANY_BIT_OR: r = pa_big_or(p, q); break;
        case PA_ANY_BIT_XOR: r = pa_big_xor(p, q); break;
        case PA_ANY_BIT_POW: r = pa_big_pow(p, q); break;
        case PA_ANY_BIT_SHL: r = pa_big_lshift(p, q); break;
        case PA_ANY_BIT_SHR: r = pa_big_rshift(p, q); break;
        default: panic_c("invalid dynamic big operator");
    }
    pa_rc_cleanup(mark);
    return (PaAnyPair){(int64_t)(uintptr_t)r, PA_TAG_BIG};
}

static PaAnyPair any_invert(int64_t v, AnyVal x) {
    if (x.kind == ANY_BIG)
        return (PaAnyPair){(int64_t)(uintptr_t)pa_big_not(any_big_ptr(x)), PA_TAG_BIG};
    return any_pair_i64(~v);
}

int64_t pa_any_big_binary(int64_t op, int64_t a, int64_t b, int64_t da, int64_t db) {
    AnyVal x, y;
    any_decode_typed(a, da == 'u' ? 'i' : da, &x);
    any_decode_typed(b, db == 'u' ? 'i' : db, &y);
    return any_big_binary(op, a, b, x, y, da == 'u', db == 'u').v;
}

int64_t pa_any_invert(int64_t v, int64_t desc) {
    AnyVal x;
    any_decode_typed(v, desc, &x);
    return any_invert(v, x).v;
}

PaAnyPair pa_any_big_binary_t(int64_t op, int64_t a, int64_t tag_a, int64_t b, int64_t tag_b) {
    // -1 is an argument-only hint for statically u64 operands, not a stored tag.
    AnyVal x, y;
    any_decode_tag(a, tag_a == -1 ? PA_TAG_INT : tag_a, &x);
    any_decode_tag(b, tag_b == -1 ? PA_TAG_INT : tag_b, &y);
    return any_big_binary(op, a, b, x, y, tag_a == -1, tag_b == -1);
}

PaAnyPair pa_any_invert_t(int64_t v, int64_t tag) {
    AnyVal x;
    any_decode_tag(v, tag, &x);
    return any_invert(v, x);
}

static PaAnyPair any_pair_f64(double d) {
    PaAnyPair p = {any_float_box(d), PA_TAG_FBOX};
    return p;
}

/* 动态算术（tag 版）：op/a/tag_a/b/tag_b/reuse_ok。返回 {%rax=payload,
   %rdx=tag}。语义与 pa_any_arith 逐条一致（int 回绕、//、%、str 拼接、
   异类型报错），只是解码免探针、float 结果可原位复用。 */
PaAnyPair pa_any_arith_t(int64_t op, int64_t a, int64_t tag_a,
                         int64_t b, int64_t tag_b, int64_t reuse_ok) {
    AnyVal x, y;
    any_decode_tag(a, tag_a, &x);
    any_decode_tag(b, tag_b, &y);

    if (x.kind == ANY_BIG || y.kind == ANY_BIG) return any_big_arith(op, x, y);

    if (x.kind == ANY_STR || y.kind == ANY_STR) {
        if (op != PA_ANY_ADD || x.kind != ANY_STR || y.kind != ANY_STR)
            pa_raise_c("TypeError", "unsupported operand type(s) for str");
        PyAdamasStr* s = pa_str_concat(x.s, y.s);
        PaAnyPair p = {(int64_t)(uintptr_t)s, PA_TAG_STR};
        return p;
    }

    bool numeric = (x.kind == ANY_INT || x.kind == ANY_FLOAT) &&
                   (y.kind == ANY_INT || y.kind == ANY_FLOAT);
    if (!numeric) pa_raise_c("TypeError", "unsupported operand type(s)");

    int use_float = (x.kind == ANY_FLOAT || y.kind == ANY_FLOAT);
    if (op == PA_ANY_DIV) use_float = 1;

    if (!use_float) {
        switch (op) {
            case PA_ANY_ADD: return any_pair_i64(x.i + y.i);
            case PA_ANY_SUB: return any_pair_i64(x.i - y.i);
            case PA_ANY_MUL: return any_pair_i64(x.i * y.i);
            case PA_ANY_FLOORDIV: return any_pair_i64(any_int_floordiv(x.i, y.i));
            case PA_ANY_MOD: return any_pair_i64(any_int_mod(x.i, y.i));
            default: break;
        }
    }

    double da = x.kind == ANY_FLOAT ? x.f : (double)x.i;
    double db = y.kind == ANY_FLOAT ? y.f : (double)y.i;
    double r = 0.0;
    switch (op) {
        case PA_ANY_ADD: r = da + db; break;
        case PA_ANY_SUB: r = da - db; break;
        case PA_ANY_MUL: r = da * db; break;
        case PA_ANY_DIV:
            if (db == 0.0) pa_raise_c("ZeroDivisionError", "float division by zero");
            r = da / db; break;
        case PA_ANY_FLOORDIV:
            if (db == 0.0) pa_raise_c("ZeroDivisionError", "float floor division by zero");
            r = any_floor_div_d(da, db); break;
        case PA_ANY_MOD:
            if (db == 0.0) pa_raise_c("ZeroDivisionError", "float modulo");
            r = any_mod_d(da, db); break;
        default: break;
    }
    /* 原位复用：先试左实参（acc + x 的 acc），再试右实参（x + acc）。 */
    int64_t reused = any_reuse_float_box(a, tag_a, r, reuse_ok);
    if (!reused) reused = any_reuse_float_box(b, tag_b, r, reuse_ok);
    if (reused) { PaAnyPair p = {reused, PA_TAG_FBOX}; return p; }
    return any_pair_f64(r);
}

PaAnyPair pa_any_neg_t(int64_t v, int64_t tag, int64_t reuse_ok) {
    AnyVal a;
    any_decode_tag(v, tag, &a);
    if (a.kind == ANY_FLOAT) {
        int64_t reused = any_reuse_float_box(v, tag, -a.f, reuse_ok);
        if (reused) { PaAnyPair p = {reused, PA_TAG_FBOX}; return p; }
        return any_pair_f64(-a.f);
    }
    if (a.kind == ANY_BIG)
        return (PaAnyPair){(int64_t)(uintptr_t)pa_big_neg(any_big_ptr(a)), PA_TAG_BIG};
    if (a.kind == ANY_INT) return any_pair_i64((int64_t)(0u - (uint64_t)a.i));
    pa_raise_c("TypeError", "bad operand type for unary -");
    return any_pair_i64(0);
}

/* 动态比较（tag 版）：语义与 pa_any_cmp 逐条一致。 */
int64_t pa_any_cmp_t(int64_t op, int64_t a, int64_t tag_a,
                     int64_t b, int64_t tag_b) {
    AnyVal x, y;
    any_decode_tag(a, tag_a, &x);
    any_decode_tag(b, tag_b, &y);
    int eq = 0, lt = 0, gt = 0;
    bool x_num = (x.kind == ANY_INT || x.kind == ANY_FLOAT);
    bool y_num = (y.kind == ANY_INT || y.kind == ANY_FLOAT);
    if (any_num(x) && any_num(y) && (x.kind == ANY_BIG || y.kind == ANY_BIG)) {
        int64_t c = any_big_cmp(x, y);
        eq = c == 0;
        lt = c == -1;
        gt = c == 1;
    } else if (x_num && y_num) {
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
        eq = any_unknown_eq(a, b, 0);
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

int64_t pa_any_is_float_t(int64_t v, int64_t tag) {
    if (tag == PA_TAG_FBOX || tag == PA_TAG_FRAW) return 1;
    if (tag == PA_TAG_HEAP) {
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
        return h->kind == PYADAMAS_RC_CPYFLOAT ? 1 : 0;
    }
    if (tag == PA_TAG_UNK) return pa_any_is_float(v);
    return 0;
}

double pa_any_to_f64_t(int64_t v, int64_t tag) {
    AnyVal a;
    any_decode_tag(v, tag, &a);
    switch (a.kind) {
        case ANY_FLOAT: return a.f;
        case ANY_INT: return (double)a.i;
        case ANY_BIG: return pa_big_to_f64((PyAdamasBig*)(uintptr_t)a.i);
        case ANY_STR:
            pa_raise_c("TypeError", "must be real number, not str");
            return 0.0;
        default:
            pa_raise_c("TypeError", "must be real number, not object");
            return 0.0;
    }
}

int64_t pa_any_to_i64_t(int64_t v, int64_t tag) {
    AnyVal a;
    any_decode_tag(v, tag, &a);
    switch (a.kind) {
        case ANY_FLOAT:
            if (!isfinite(a.f) || a.f >= 9223372036854775808.0 ||
                a.f < -9223372036854775808.0)
                pa_raise_c("OverflowError", "cannot convert float to integer");
            return (int64_t)a.f;
        case ANY_INT: return a.i;
        case ANY_BIG: return pa_big_to_i64((PyAdamasBig*)(uintptr_t)a.i);
        case ANY_STR:
            pa_raise_c("TypeError", "int() argument must be a number, not str");
            return 0;
        default:
            pa_raise_c("TypeError", "int() argument must be a number, not object");
            return 0;
    }
}


/* ══════════════════════════════════════════════════════════════════
   P2（0.340）：运行期类型码与 isinstance 谓词
   设计见 docs/numeric-types-design.md §2。类型码与 src/tag_defs.h 的
   PaTypeCode 逐字对应。
     * 有精确 tag（NONE/BOOL/FBOX/STR/BYTES/TUPLE/SET/CLASS）就直接用；
     * tag==HEAP 读对象头 kind（list/dict/str/CPYFLOAT/INSTANCE/OBJ）；
     * tag==UNK 先探针，再走头 kind。
   边界（文档化，不是 bug）：容器元素/字段等无 tag 来源的堆值只能按头
   kind 判型 —— tuple 与 list 同为 LIST、set 与 dict 同为 DICT、bytes 与
   全 ASCII str 同为 STR，此时报 LIST/DICT/STR（与探针口径一致，不假装
   知道）。非堆值无 tag 来源时一律 INT（None/bool/int 同形）。
   ══════════════════════════════════════════════════════════════════ */
enum {
    PA_TYPE_NONE   = 0,
    PA_TYPE_BOOL   = 1,
    PA_TYPE_INT    = 2,
    PA_TYPE_FLOAT  = 3,
    PA_TYPE_STR    = 4,
    PA_TYPE_BYTES  = 5,
    PA_TYPE_LIST   = 6,
    PA_TYPE_DICT   = 7,
    PA_TYPE_TUPLE  = 8,
    PA_TYPE_SET    = 9,
    PA_TYPE_OBJECT = 10,
    PA_TYPE_CLASS  = 11,
    PA_TYPE_OTHER  = 12,
    PA_TYPE_BIG    = 13,
};

/* 堆指针 -> 类型码（一次头加载，不探针：调用方已确认它是活堆指针）。 */
static int64_t any_type_from_heap(int64_t v) {
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    switch (h->kind) {
        case PYADAMAS_RC_STR: return PA_TYPE_STR;
        case PYADAMAS_RC_LIST: return PA_TYPE_LIST;
        case PYADAMAS_RC_DICT: return PA_TYPE_DICT;
        case PYADAMAS_RC_CPYFLOAT: return PA_TYPE_FLOAT;
        case PYADAMAS_RC_BIGINT: return PA_TYPE_BIG;
        case PYADAMAS_RC_INSTANCE: return PA_TYPE_OBJECT;
        case PYADAMAS_RC_OBJ: {
            /* 类对象藏在 OBJ 里（头有 magic/cid/ctor/vtable）；异常值等
               其它 OBJ 负载不是类对象，按 OBJECT（异常类的 isinstance
               由 pa_is_exc 的静态名字路径处理）。 */
            if (h->size >= (int64_t)sizeof(PyAdamasClass)) {
                PyAdamasClass* c = (PyAdamasClass*)(uintptr_t)v;
                if (c->magic == PYADAMAS_CLASS_MAGIC) return PA_TYPE_CLASS;
            }
            return PA_TYPE_OBJECT;
        }
        default: return PA_TYPE_OBJECT;
    }
}

int64_t pa_any_type(int64_t v, int64_t tag) {
    switch (tag) {
        case PA_TAG_NONE: return PA_TYPE_NONE;
        case PA_TAG_BOOL: return PA_TYPE_BOOL;
        case PA_TAG_INT: return PA_TYPE_INT;
        case PA_TAG_FRAW: case PA_TAG_FBOX: return PA_TYPE_FLOAT;
        case PA_TAG_STR: return PA_TYPE_STR;
        case PA_TAG_BYTES: return PA_TYPE_BYTES;
        case PA_TAG_TUPLE: return PA_TYPE_TUPLE;
        case PA_TAG_SET: return PA_TYPE_SET;
        case PA_TAG_CLASS: return PA_TYPE_CLASS;
        case PA_TAG_BIG: return PA_TYPE_BIG;
        case PA_TAG_HEAP: return any_type_from_heap(v);
        default: /* PA_TAG_UNK：探针 + 头 kind（旧口径） */
            if (!pa_unknown_is_heap(v)) return PA_TYPE_INT;
            return any_type_from_heap(v);
    }
}

/* isinstance(unknown, 内置类型)：code 是 PA_TYPE_*。CPython 口径：
   object 匹配一切；int 接受 bool（bool 是 int 的子类）。 */
int64_t pa_any_type_is(int64_t v, int64_t tag, int64_t code) {
    if (code == PA_TYPE_OBJECT) return 1;
    int64_t t = pa_any_type(v, tag);
    if (code == PA_TYPE_INT) return t == PA_TYPE_INT || t == PA_TYPE_BOOL || t == PA_TYPE_BIG;
    return t == code ? 1 : 0;
}
