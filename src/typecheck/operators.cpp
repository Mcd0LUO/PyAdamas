// typecheck/expr.cpp —— 表达式类型检查：运算、容器、推导式、迭代
#include "typecheck/typecheck.h"

#include "ir/module.h"


namespace pyadamas {

// 运算符类型规则：dunder 返回类型推导、二元运算、比较

TypePtr TypeChecker::dunder_binop_ret(Tok op, TypePtr l, TypePtr r) {
    struct Ref {
        Tok op;
        const char* fwd;
        const char* rev;
    };
    static const Ref tbl[] = {
        {Tok::PLUS, "__add__", "__radd__"},
        {Tok::MINUS, "__sub__", "__rsub__"},
        {Tok::STAR, "__mul__", "__rmul__"},
        {Tok::SLASH, "__truediv__", "__rtruediv__"},
        {Tok::DOUBLESLASH, "__floordiv__", "__rfloordiv__"},
        {Tok::PERCENT, "__mod__", "__rmod__"},
        {Tok::DOUBLESTAR, "__pow__", "__rpow__"},
    };
    const Ref* ref = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            ref = &e;
            break;
        }
    if (!ref) return nullptr;
    auto try_side = [&](TypePtr self, TypePtr other, const char* name) -> TypePtr {
        if (!self || self->kind != TyKind::Class || !self->cls) return nullptr;
        const MethodInfo* m = self->cls->method(name);
        if (!m || m->kind != MethodKind::Normal || !m->call_sig) return nullptr;
        if (m->call_sig->varargs || m->call_sig->kwargs || m->call_sig->pos.size() != 1)
            return nullptr;
        TypePtr pt = m->call_sig->pos[0].ty;
        if (pt && pt->kind != TyKind::Unknown && other && other->kind != TyKind::Unknown &&
            !assignable(pt, other))
            return nullptr;
        return m->call_sig->ret ? m->call_sig->ret : t_unknown();
    };
    if (TypePtr t = try_side(l, r, ref->fwd)) return t;
    return try_side(r, l, ref->rev);
}

// 单参数 dunder（self 之外没有形参）的声明返回类型。签名不可用时返回
// nullptr，调用点退回「没有实现」的默认行为。
TypePtr TypeChecker::dunder_unary_ret(const char* name, TypePtr t) {
    if (!name || !t || t->kind != TyKind::Class || !t->cls) return nullptr;
    const MethodInfo* m = t->cls->method(name);
    if (!m || m->kind != MethodKind::Normal || !m->call_sig) return nullptr;
    if (m->call_sig->varargs || m->call_sig->kwargs || !m->call_sig->pos.empty())
        return nullptr;
    return m->call_sig->ret ? m->call_sig->ret : t_unknown();
}

TypePtr TypeChecker::dunder_aug_ret(Tok op, TypePtr l, TypePtr r) {
    struct Ref {
        Tok op;
        const char* iaug;
    };
    static const Ref tbl[] = {
        {Tok::PLUSEQUAL, "__iadd__"},      {Tok::MINUSEQUAL, "__isub__"},
        {Tok::STAREQUAL, "__imul__"},      {Tok::SLASHEQUAL, "__itruediv__"},
        {Tok::DOUBLESLASHEQUAL, "__ifloordiv__"}, {Tok::PERCENTEQUAL, "__imod__"},
        {Tok::DOUBLESTAREQUAL, "__ipow__"},
    };
    const char* name = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            name = e.iaug;
            break;
        }
    if (!name) return nullptr;
    if (!l || l->kind != TyKind::Class || !l->cls) return nullptr;
    const MethodInfo* m = l->cls->method(name);
    if (!m || m->kind != MethodKind::Normal || !m->call_sig) return nullptr;
    if (m->call_sig->varargs || m->call_sig->kwargs || m->call_sig->pos.size() != 1)
        return nullptr;
    TypePtr pt = m->call_sig->pos[0].ty;
    if (pt && pt->kind != TyKind::Unknown && r && r->kind != TyKind::Unknown &&
        !assignable(pt, r))
        return nullptr;
    return m->call_sig->ret ? m->call_sig->ret : t_unknown();
}

bool TypeChecker::dunder_cmp_ok(CmpOp op, TypePtr l, TypePtr r) {
    struct Ref {
        CmpOp op;
        const char* fwd;
        const char* rev;
    };
    static const Ref tbl[] = {
        {CmpOp::Lt, "__lt__", "__gt__"},
        {CmpOp::Le, "__le__", "__ge__"},
        {CmpOp::Gt, "__gt__", "__lt__"},
        {CmpOp::Ge, "__ge__", "__le__"},
    };
    const char* fwd = nullptr;
    const char* rev = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            fwd = e.fwd;
            rev = e.rev;
            break;
        }
    if (!fwd) return false;
    auto side_ok = [&](TypePtr self, TypePtr other, const char* name) {
        if (!self || self->kind != TyKind::Class || !self->cls) return false;
        const MethodInfo* m = self->cls->method(name);
        if (!m || m->kind != MethodKind::Normal || !m->call_sig) return false;
        if (m->call_sig->varargs || m->call_sig->kwargs || m->call_sig->pos.size() != 1)
            return false;
        TypePtr pt = m->call_sig->pos[0].ty;
        if (pt && pt->kind != TyKind::Unknown && other && other->kind != TyKind::Unknown &&
            !assignable(pt, other))
            return false;
        return true;
    };
    return side_ok(l, r, fwd) || side_ok(r, l, rev);
}

TypePtr TypeChecker::binop_type(Tok op, TypePtr l, TypePtr r, int line, int col,
                                const Expr* le, const Expr* re) {
        // unknown 计数可能不是整数，不能走机器字移位或一般的 big 转换。
        if ((op == Tok::LSHIFT || op == Tok::RSHIFT) && l && l->kind == TyKind::Big &&
            is_unknown(r)) {
            err(line, col, "big shift requires a statically typed integer count; use cast_big(count)");
            return t_unknown();
        }
        if (is_unknown(l) || is_unknown(r)) {
            // 一侧 unknown（没标注的参数最常见）、另一侧是 float 时，结果
            // 仍然是 float：Python 先把另一侧提升成 double。以前这里一律
            // 返回 unknown，codegen 兜底按整数算，算出来是位模式参与整数
            // 乘除的野值，而且不报错。unknown 一侧按机器整数表示补 convert；
            // 浮点实参本来就不支持传给未标注参数，这里不改变那个限制。
            bool lf = l && l->kind == TyKind::Float;
            bool rf = r && r->kind == TyKind::Float;
            switch (op) {
                // / 永远产出 float，哪怕另一侧是 int
                case Tok::SLASH: return t_float();
                case Tok::PLUS:
                case Tok::MINUS:
                case Tok::STAR:
                case Tok::DOUBLESLASH:
                case Tok::PERCENT:
                case Tok::DOUBLESTAR:
                    if (lf != rf) return t_float();
                    break;
                default: break;
            }
            return t_unknown();
        }
        // 类实例的算术 dunder：有实现就按方法声明的返回类型走。
        if (l->kind == TyKind::Class || r->kind == TyKind::Class) {
            if (TypePtr t = dunder_binop_ret(op, l, r)) return t;
        }
        // BoundMethod 上做算术没有静态规则，放过；类实例一侧没有匹配的
        // dunder 时后端只能按整数算指针，宁可在这里报错。
        if (l->kind == TyKind::BoundMethod || r->kind == TyKind::BoundMethod)
            return t_unknown();
        // Optional / None 需要先判空才能运算：窄化（0.498）在分支内把类型换成
        // 内层类型，走到这里仍是 Optional 就说明没判空 → 报错并给可操作提示。
        if (l->kind == TyKind::Optional) {
            maybe_none(le, l, line, col);
            return t_unknown();
        }
        if (r->kind == TyKind::Optional) {
            maybe_none(re, r, line, col);
            return t_unknown();
        }
        if (l->kind == TyKind::None || r->kind == TyKind::None) return t_unknown();
        // Path / str：路径拼接（CPython 的 Path.__truediv__）
        if (l->kind == TyKind::Path && op == Tok::SLASH) {
            if (r->kind != TyKind::Str && !is_unknown(r)) {
                err(line, col,
                    std::format("unsupported operand type(s) for /: 'pathlib.Path' and '{}'",
                                type_name(r)));
                return t_unknown();
            }
            return new_ty(TyKind::Path);
        }
        auto bad = [&]() {
            err(line, col,
                std::format("unsupported operand type(s) for {}: '{}' and '{}'", tok_name(op),
                            type_name(l), type_name(r)));
            return t_unknown();
        };
        // 元组重复的结果长度运行期才知道，静态类型记成变长；异构元素按 unknown。
        auto tuple_rest_of = [](const TypePtr& t) {
            TypePtr et = t->sub.empty() ? t_unknown() : t->sub[0];
            for (size_t i = 1; i < t->sub.size(); ++i) {
                if (!type_eq(et, t->sub[i])) {
                    et = t_unknown();
                    break;
                }
            }
            return mk_tuple_rest(et);
        };

        // datetime / timedelta 的算术：只有下面这几种组合有定义。
        if (l->kind == TyKind::DateTime || r->kind == TyKind::DateTime ||
            l->kind == TyKind::TimeDelta || r->kind == TyKind::TimeDelta) {
            auto td = [](const TypePtr& t) { return t->kind == TyKind::TimeDelta; };
            auto dt = [](const TypePtr& t) { return t->kind == TyKind::DateTime; };
            switch (op) {
                case Tok::MINUS:
                    if (dt(l) && dt(r)) return new_ty(TyKind::TimeDelta);
                    if (dt(l) && td(r)) return new_ty(TyKind::DateTime);
                    if (td(l) && td(r)) return new_ty(TyKind::TimeDelta);
                    break;
                case Tok::PLUS:
                    if (dt(l) && td(r)) return new_ty(TyKind::DateTime);
                    if (td(l) && dt(r)) return new_ty(TyKind::DateTime);
                    if (td(l) && td(r)) return new_ty(TyKind::TimeDelta);
                    break;
                case Tok::STAR:
                    if (td(l) && r->kind == TyKind::Int) return new_ty(TyKind::TimeDelta);
                    if (l->kind == TyKind::Int && td(r)) return new_ty(TyKind::TimeDelta);
                    break;
                case Tok::SLASH:
                    if (td(l) && r->kind == TyKind::Int) return new_ty(TyKind::TimeDelta);
                    if (td(l) && td(r)) return t_float();
                    break;
                case Tok::DOUBLESLASH:
                    if (td(l) && r->kind == TyKind::Int) return new_ty(TyKind::TimeDelta);
                    if (td(l) && td(r)) return t_int();
                    break;
                default: break;
            }
            return bad();
        }

        if (l->kind == TyKind::Complex || r->kind == TyKind::Complex) {
            auto ok = [](const TypePtr& t) {
                return t && (t->kind == TyKind::Complex || is_num(t));
            };
            switch (op) {
                case Tok::PLUS:
                case Tok::MINUS:
                case Tok::STAR:
                case Tok::SLASH:
                    if (ok(l) && ok(r)) return t_complex();
                    return bad();
                default:
                    return bad();
            }
        }

        switch (op) {
            case Tok::PLUS:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Str && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::Bytes && r->kind == TyKind::Bytes) return t_bytes();
                // 异常值参与字符串拼接时按消息处理，兼容旧代码 "msg:" + e
                if (l->kind == TyKind::Str && r->kind == TyKind::Exception) return t_str();
                if (l->kind == TyKind::Exception && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::List && r->kind == TyKind::List)
                    return mk_list(join(l->sub[0], r->sub[0]));
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Tuple) {
                    std::vector<TypePtr> v = l->sub;
                    v.insert(v.end(), r->sub.begin(), r->sub.end());
                    return mk_tuple(std::move(v));
                }
                return bad();
            case Tok::MINUS:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return l;
                return bad();
            case Tok::STAR:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Str && r->kind == TyKind::Int) return t_str();
                if (l->kind == TyKind::Int && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::Bytes && r->kind == TyKind::Int) return t_bytes();
                if (l->kind == TyKind::Int && r->kind == TyKind::Bytes) return t_bytes();
                if (l->kind == TyKind::List && r->kind == TyKind::Int) return l;
                if (l->kind == TyKind::Int && r->kind == TyKind::List) return r;
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Int)
                    return tuple_rest_of(l);
                if (l->kind == TyKind::Int && r->kind == TyKind::Tuple)
                    return tuple_rest_of(r);
                return bad();
            case Tok::SLASH:
                if (is_num(l) && is_num(r)) return t_float();
                return bad();
            case Tok::DOUBLESLASH:
            case Tok::PERCENT:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (op == Tok::PERCENT && l->kind == TyKind::Str) return t_str();
                if (op == Tok::PERCENT && l->kind == TyKind::Bytes) return t_bytes();
                return bad();
            case Tok::DOUBLESTAR:
                if (is_num(l) && is_num(r)) return promote(l, r);
                return bad();
            case Tok::AMPER:
            case Tok::VBAR:
            case Tok::CIRCUMFLEX:
                if (l->kind == TyKind::Bool && r->kind == TyKind::Bool) return t_bool();
                // P3：big 参与位运算（无限补码语义，rt_big.c）。
                if ((is_int_like(l) || l->kind == TyKind::Big) &&
                    (is_int_like(r) || r->kind == TyKind::Big))
                    return promote(l, r);
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return l;
                if (l->kind == TyKind::Dict && r->kind == TyKind::Dict && op == Tok::VBAR)
                    return l;
                return bad();
            case Tok::LSHIFT:
            case Tok::RSHIFT:
                // 移位结果跟随左操作数；超宽按 pa 的回绕/清零语义（非 C UB）。
                // P3：big << n / big >> n（计数也可以是 big，rt_big 校验非负）。
                if (r->kind == TyKind::Big && l->kind != TyKind::Big) {
                    err(line, col, "a big shift count requires a big left operand");
                    return t_unknown();
                }
                if ((is_int_like(l) || l->kind == TyKind::Big) &&
                    (is_int_like(r) || r->kind == TyKind::Big))
                    return int_shift_result(l);
                return bad();
            default:
                return bad();
        }
    }


}  // namespace pyadamas
