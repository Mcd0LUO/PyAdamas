// desugar_fold.h —— 常量整数表达式的编译期精确求值（脱糖期专用）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast.h"

namespace pyadamas {

static std::string dec_mul(const std::string& a, const std::string& b);
static std::string dec_pow(const std::string& base, int64_t exp);
static std::string i128_to_dec(__int128 v);

// ── `x: big = <常量整数表达式>` 的精确折叠 ─────────────────────────────
//
// 默认 int 是 i64，`2 ** 70` 在 int 域里回绕成 0（docs/compatibility.md 的规则）。
// 但目标是 `big` 时把回绕后的值塞进去毫无意义：
//
//     a: big = 2 ** 70      # 以前静默得 0；CPython 是 1180591620717411303424
//
// 全常量表达式在编译期能精确求值，直接折成 CK::Big 字面量 —— 与「写超 u64 的
// 十进制字面量」走同一条路径。范围用 __int128 兜住（到 2**126），再大就交给
// 原来"表达式太大"的处理（不在这里静默回绕）。
static bool ann_is_big(const ExprPtr& ann) {
    auto* n = dynamic_cast<const Name*>(ann.get());
    return n && n->id == "big";
}

// 折叠结果：0 = 不是常量整数表达式；1 = 已精确求出；2 = 超出 __int128 求值范围。
//
// `any_i64` 记录「求值过程中是否有中间值超出 int64」：即便最终结果落回 int64
// （`2 ** 70 // 2 ** 69` 就是 2），也不能按 int 域求值 —— 子表达式会在**运行期**
// 回绕（2**70 → 0、2**69 → 0 → 0//0 抛 ZeroDivisionError）。
static int fold_int_const(const Expr* e, __int128& out, bool& any_i64,
                          std::string* dec = nullptr);

static void fold_note(__int128 v, bool& any_i64) {
    if (v < static_cast<__int128>(INT64_MIN) || v > static_cast<__int128>(INT64_MAX))
        any_i64 = true;
}

static int fold_int_const(const Expr* e, __int128& out, bool& any_i64,
                          std::string* dec) {
    if (!e) return 0;
    if (auto* c = dynamic_cast<const Constant*>(e)) {
        if (c->ck != Constant::CK::Int) return 0;
        // u64_lit：> INT64_MAX 的十进制字面量，i 里存的是无符号位模式。
        // 这里按无符号读，否则 `-9223372036854775808` 会被算成 +2^63。
        if (c->u64_lit) {
            out = static_cast<__int128>(static_cast<uint64_t>(c->i));
            return 1;
        }
        out = static_cast<__int128>(c->i);
        return 1;
    }
    if (auto* u = dynamic_cast<const UnaryOp*>(e)) {
        __int128 v = 0;
        int st = fold_int_const(u->operand.get(), v, any_i64, dec);
        if (st != 1) return st;
        if (u->op == Tok::MINUS) { out = -v; fold_note(out, any_i64); return 1; }
        if (u->op == Tok::PLUS) { out = v; fold_note(out, any_i64); return 1; }
        return 0;
    }
    auto* b = dynamic_cast<const BinOp*>(e);
    if (!b) return 0;
    __int128 l = 0, r = 0;
    int sl = fold_int_const(b->left.get(), l, any_i64, dec);
    int sr = fold_int_const(b->right.get(), r, any_i64, dec);
    if (sl == 0 || sr == 0) return 0;
    if (sl == 2 || sr == 2) return 2;
    switch (b->op) {
        case Tok::PLUS:
            if (__builtin_add_overflow(l, r, &out)) return 2;
            fold_note(out, any_i64);
            return 1;
        case Tok::MINUS:
            if (__builtin_sub_overflow(l, r, &out)) return 2;
            fold_note(out, any_i64);
            return 1;
        case Tok::STAR:
            if (__builtin_mul_overflow(l, r, &out)) return 2;
            fold_note(out, any_i64);
            return 1;
        case Tok::DOUBLESLASH: {
            if (r == 0) return 0;   // 常量除零：留给运行期按 Python 语义抛
            // **Python 是向下取整，C++ 是向零取整**：负数要减一。
            __int128 q = l / r;
            if ((l % r) != 0 && ((l < 0) != (r < 0))) --q;
            out = q;
            fold_note(out, any_i64);
            return 1;
        }
        case Tok::PERCENT: {
            if (r == 0) return 0;
            __int128 q = l / r;
            if ((l % r) != 0 && ((l < 0) != (r < 0))) --q;
            out = l - q * r;   // 余数与除数同号（Python 口径）
            fold_note(out, any_i64);
            return 1;
        }
        case Tok::DOUBLESTAR: {
            if (r < 0 || r > 4096) return 0;
            __int128 acc = 1;
            for (__int128 i = 0; i < r; ++i)
                if (__builtin_mul_overflow(acc, l, &acc)) {
                    // 超 __int128：改走十进制串求幂，结果交给 dec 带出去。
                    if (!dec || l < 0) return 2;
                    *dec = dec_pow(i128_to_dec(l), static_cast<int64_t>(r));
                    return 2;
                }
            out = acc;
            fold_note(out, any_i64);
            return 1;
        }
        case Tok::LSHIFT:
            if (r < 0) return 2;
            if (r < 127) {
                out = l << static_cast<int>(r);
                if (l == 0 || (out >> static_cast<int>(r)) == l) {
                    fold_note(out, any_i64);
                    return 1;
                }
            }
            // 超 __int128：l * 2**r 走十进制。
            if (!dec || l < 0) return 2;
            *dec = dec_mul(i128_to_dec(l), dec_pow("2", static_cast<int64_t>(r)));
            return 2;
        case Tok::RSHIFT:
            if (r < 0 || r > 127) return 2;
            out = l >> static_cast<int>(r);
            fold_note(out, any_i64);
            return 1;
        case Tok::AMPER: out = l & r; fold_note(out, any_i64); return 1;
        case Tok::VBAR: out = l | r; fold_note(out, any_i64); return 1;
        case Tok::CIRCUMFLEX: out = l ^ r; fold_note(out, any_i64); return 1;
        default: return 0;   // 真除等：结果未必是整数，交回原路径
    }
}

// 十进制串乘法（schoolbook）。只在 `**` 溢出 __int128 时用，规模很小。
static std::string dec_mul(const std::string& a, const std::string& b) {
    if (a == "0" || b == "0") return "0";
    std::vector<int> acc(a.size() + b.size(), 0);
    for (size_t i = a.size(); i-- > 0;) {
        int da = a[i] - '0';
        for (size_t j = b.size(); j-- > 0;) {
            int db = b[j] - '0';
            acc[i + j + 1] += da * db;
        }
    }
    for (size_t k = acc.size(); k-- > 1;) {
        acc[k - 1] += acc[k] / 10;
        acc[k] %= 10;
    }
    std::string s;
    size_t k = 0;
    while (k + 1 < acc.size() && acc[k] == 0) ++k;
    for (; k < acc.size(); ++k) s.push_back(static_cast<char>('0' + acc[k]));
    return s;
}

// base(十进制串) ** exp（非负），结果按十进制串返回。
static std::string dec_pow(const std::string& base, int64_t exp) {
    std::string acc = "1";
    for (int64_t i = 0; i < exp; ++i) acc = dec_mul(acc, base);
    return acc;
}

static std::string i128_to_dec(__int128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 m = neg ? static_cast<unsigned __int128>(-v)
                               : static_cast<unsigned __int128>(v);
    std::string s;
    while (m > 0) {
        s.push_back(static_cast<char>('0' + static_cast<int>(m % 10)));
        m /= 10;
    }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    return s;
}

}  // namespace pyadamas
