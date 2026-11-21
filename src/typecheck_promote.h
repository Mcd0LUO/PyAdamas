// typecheck_promote.h —— 定宽整数分类与提升规则
//
// typecheck_types.h 的组成部分（纯代码移动，声明顺序不变）：
// 由 typecheck_types.h 末尾统一包含，不要单独 include。
#pragma once

#include "typecheck_types.h"

namespace pyadamas {

// ── 定宽整数分类（P1）────────────────────────────────────────────────
//
// 判定与提升规则集中在这里：**类型检查、降级、代码生成必须共用同一套
// 判断**。提升规则是静默算错的重灾区（C 的 signed/unsigned 坑），
// 三处各自写一份 if 迟早会漂移。

struct IntInfo {
    int width = 64;      // 8 / 16 / 32 / 64
    bool is_signed = true;
};

// 定宽（非默认 Int）整数种类。
inline bool is_fixed_int_kind(TyKind k) {
    switch (k) {
        case TyKind::I8:
        case TyKind::I16:
        case TyKind::I32:
        case TyKind::U8:
        case TyKind::U16:
        case TyKind::U32:
        case TyKind::U64:
            return true;
        default:
            return false;
    }
}

// 整数种类：默认 Int(i64) + 全部定宽。Bool 不算（单独判）。
inline bool is_int_kind(TyKind k) {
    return k == TyKind::Int || is_fixed_int_kind(k);
}

inline std::optional<IntInfo> int_info_of(TyKind k) {
    switch (k) {
        case TyKind::Int: return IntInfo{64, true};
        case TyKind::I8: return IntInfo{8, true};
        case TyKind::I16: return IntInfo{16, true};
        case TyKind::I32: return IntInfo{32, true};
        case TyKind::U8: return IntInfo{8, false};
        case TyKind::U16: return IntInfo{16, false};
        case TyKind::U32: return IntInfo{32, false};
        case TyKind::U64: return IntInfo{64, false};
        default: return std::nullopt;
    }
}

inline std::optional<IntInfo> int_info_of(const TypePtr& t) {
    if (!t) return std::nullopt;
    return int_info_of(t->kind);
}

// 任意整数（含 bool）。索引/长度/计数的位置用它。
inline bool is_int_like(const TypePtr& t) {
    return t && (is_int_kind(t->kind) || t->kind == TyKind::Bool);
}

inline bool is_unsigned_int(const TypePtr& t) {
    auto i = int_info_of(t);
    return i && !i->is_signed;
}

inline bool is_fixed_int(const TypePtr& t) {
    return t && is_fixed_int_kind(t->kind);
}

// 定宽整数的值域。字面量 coerce 的范围检查与 cast 语义都用它。
inline void int_range_of(TyKind k, int64_t& lo, int64_t& hi) {
    auto i = int_info_of(k);
    if (!i) { lo = INT64_MIN; hi = INT64_MAX; return; }
    if (i->is_signed) {
        lo = -static_cast<int64_t>(static_cast<uint64_t>(1) << (i->width - 1));
        hi = static_cast<int64_t>((static_cast<uint64_t>(1) << (i->width - 1)) - 1);
    } else {
        lo = 0;
        hi = i->width == 64 ? INT64_MAX
                            : static_cast<int64_t>((static_cast<uint64_t>(1) << i->width) - 1);
    }
}

// 把 64 位宽的原始结果按定宽类型截断回绕成规范值（符号扩展/零扩展）。
// 常量折叠（opt.cpp）与正常化路径共用同一套语义：回绕是 pa 的确定行为。
inline int64_t int_narrow_value(const TypePtr& t, uint64_t v) {
    auto ii = int_info_of(t);
    if (!ii || ii->width == 64) return static_cast<int64_t>(v);
    const uint64_t mask = (static_cast<uint64_t>(1) << ii->width) - 1;
    uint64_t w = v & mask;
    if (ii->is_signed && ii->width < 64) {
        const uint64_t sign = static_cast<uint64_t>(1) << (ii->width - 1);
        if (w & sign) w |= ~mask;
    }
    return static_cast<int64_t>(w);
}

// 字面量 v 能否隐式放进定宽类型 t（不截断、不回绕）。
inline bool int_literal_fits(const TypePtr& t, int64_t v) {
    if (!t || !is_int_kind(t->kind)) return true;
    int64_t lo = 0, hi = 0;
    int_range_of(t->kind, lo, hi);
    return v >= lo && v <= hi;
}

// ── 提升规则（§1.2，定死）──────────────────────────────────────────
//
// 1. 任一 Float → float（F32 未实现，全部 f64）。
// 2. Bool 参与 → 提升为对方的整数类型（bool+bool → int，CPython True+True==2）。
// 3. 纯整数：取较宽宽度；**同宽 signed+unsigned → unsigned**（C 口径）；
//    默认 int(i64) 与定宽混 → i64 最宽；i64+u64 同宽 → u64。
// 4. 非数值域（unknown/Optional/None 混进来）→ int（旧行为，保守）。
//
// **下标/宽度/signedness 全部按提升后的类型定**，运算两侧在降级期被
// 转换（normalize）到该类型，codegen 只按一个类型选指令。
inline TypePtr int_promote(const TypePtr& l, const TypePtr& r) {
    if (!l || !r) return t_int();
    // P3 §1.2 规则 1：任一 Big → Big（格顶层，含 big+float：float 侧
    // 按 int(float) 截断转 big——pa 格口径，与 CPython 的 int+float→float
    // 不同，文档化边界，见 docs/big-landing.md）。
    if (l->kind == TyKind::Big || r->kind == TyKind::Big) return t_big();
    if (l->kind == TyKind::Float || r->kind == TyKind::Float) return t_float();
    if (l->kind == TyKind::Bool && r->kind == TyKind::Bool) return t_int();
    if (l->kind == TyKind::Bool) return is_int_kind(r->kind) ? r : t_int();
    if (r->kind == TyKind::Bool) return is_int_kind(l->kind) ? l : t_int();
    auto li = int_info_of(l->kind);
    auto ri = int_info_of(r->kind);
    if (!li || !ri) return t_int();
    if (li->width > ri->width) return l;
    if (ri->width > li->width) return r;
    // 同宽：signed+unsigned → unsigned。宽度 64 的无符号只有 U64，
    // 其余宽度 r 与 l 的种类相同，取 r 即可。
    if (!li->is_signed || !ri->is_signed) return ri->width == 64 ? t_u64() : r;
    return l;
}

// 移位：结果类型跟随**左**操作数（C：结果取提升后的左操作数类型）。
// 计数不参与定宽：u8 << 3 仍是 u8（超宽移位按回绕/清零语义）。
inline TypePtr int_shift_result(const TypePtr& l) {
    if (!l) return t_int();
    if (is_int_kind(l->kind)) return l;
    if (l->kind == TyKind::Big) return l;   // P3：big << n 仍是 big
    return t_int();
}


}  // namespace pyadamas
