#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/ast.h"
#include "ir/ops.h"

namespace pyadamas {

struct ConstVal {
    TypePtr ty;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
};

inline std::optional<ConstVal> as_const(const Expr* e) {
    if (!e) return std::nullopt;
    // 负数字面量默认值（def f(size=-1)）：与 literal_of_expr 同口径。
    if (e->kind == ExprKind::UnaryOp) {
        const auto& u = static_cast<const UnaryOp&>(*e);
        if (u.op != Tok::MINUS && u.op != Tok::PLUS) return std::nullopt;
        auto inner = as_const(u.operand.get());
        if (!inner) return std::nullopt;
        if (u.op == Tok::PLUS) return inner;
        if (inner->ty->kind == TyKind::Int) {
            inner->i = -inner->i;
            return inner;
        }
        if (inner->ty->kind == TyKind::Float) {
            inner->f = -inner->f;
            return inner;
        }
        if (inner->ty->kind == TyKind::Big) {
            if (!inner->s.empty() && inner->s[0] == '-') inner->s.erase(0, 1);
            else inner->s.insert(inner->s.begin(), '-');
            return inner;
        }
        return std::nullopt;
    }
    if (e->kind != ExprKind::Constant) return std::nullopt;
    const auto& c = static_cast<const Constant&>(*e);
    ConstVal v;
    switch (c.ck) {
        case Constant::CK::None: v.ty = t_none(); return v;
        case Constant::CK::Bool: v.ty = t_bool(); v.i = c.b ? 1 : 0; return v;
        case Constant::CK::Int: v.ty = t_int(); v.i = c.i; return v;
        case Constant::CK::Float: v.ty = t_float(); v.f = c.f; return v;
        case Constant::CK::Complex: return std::nullopt;
        case Constant::CK::Str: v.ty = t_str(); v.s = c.s; return v;
        case Constant::CK::Bytes: v.ty = t_bytes(); v.s = c.s; return v;
        case Constant::CK::Big: v.ty = t_big(); v.s = c.s; return v;
        case Constant::CK::Ellipsis: return std::nullopt;
    }
    return std::nullopt;
}

inline BinKind map_bin(Tok t) {
    switch (t) {
        case Tok::PLUS: return BinKind::Add;
        case Tok::MINUS: return BinKind::Sub;
        case Tok::STAR: return BinKind::Mul;
        case Tok::SLASH: return BinKind::Div;
        case Tok::DOUBLESLASH: return BinKind::FloorDiv;
        case Tok::PERCENT: return BinKind::Mod;
        case Tok::DOUBLESTAR: return BinKind::Pow;
        case Tok::AMPER: return BinKind::BitAnd;
        case Tok::VBAR: return BinKind::BitOr;
        case Tok::CIRCUMFLEX: return BinKind::BitXor;
        case Tok::LSHIFT: return BinKind::Shl;
        case Tok::RSHIFT: return BinKind::Shr;
        default: return BinKind::Add;
    }
}

inline CmpKind map_cmp(CmpOp op) {
    switch (op) {
        case CmpOp::Lt: return CmpKind::Lt;
        case CmpOp::Le: return CmpKind::Le;
        case CmpOp::Gt: return CmpKind::Gt;
        case CmpOp::Ge: return CmpKind::Ge;
        case CmpOp::Eq: return CmpKind::Eq;
        case CmpOp::Ne: return CmpKind::Ne;
        case CmpOp::Is: return CmpKind::Eq;
        case CmpOp::IsNot: return CmpKind::Ne;
        default: return CmpKind::Eq;
    }
}

// 复合赋值号 → 二元运算符。两套 token 是分开的（`*=` 和 `*`），
// 直接丢给 map_bin 会落到 default 分支，`acc *= 10` 被悄悄编译成 `acc += 10`。
inline Tok aug_to_bin(Tok t) {
    switch (t) {
        case Tok::PLUSEQUAL: return Tok::PLUS;
        case Tok::MINUSEQUAL: return Tok::MINUS;
        case Tok::STAREQUAL: return Tok::STAR;
        case Tok::SLASHEQUAL: return Tok::SLASH;
        case Tok::DOUBLESLASHEQUAL: return Tok::DOUBLESLASH;
        case Tok::PERCENTEQUAL: return Tok::PERCENT;
        case Tok::DOUBLESTAREQUAL: return Tok::DOUBLESTAR;
        case Tok::AMPEREQUAL: return Tok::AMPER;
        case Tok::VBAREQUAL: return Tok::VBAR;
        case Tok::CIRCUMFLEXEQUAL: return Tok::CIRCUMFLEX;
        case Tok::LSHIFTEQUAL: return Tok::LSHIFT;
        case Tok::RSHIFTEQUAL: return Tok::RSHIFT;
        default: return t;
    }
}

struct CaptureVar {
    std::string name;
    TypePtr ty;
};

// 一个函数的闭包形态：env 是从祖先函数捕获的变量（cell 指针在闭包
// 对象里的下标），cells 是本函数自己的局部变量里被子孙函数捕获的。
struct ClosureMeta {
    std::vector<CaptureVar> env;
    std::unordered_map<std::string, int> env_index;
    std::vector<CaptureVar> cells;
    std::unordered_map<std::string, int> cell_index;
    // 自底向上传播后的需求集合：本函数及其子孙需要从祖先拿到的名字。
    std::unordered_set<std::string> needed;
};

}  // namespace pyadamas
