// lower/stmt_assign.cpp —— 增量赋值（AugAssign）的降级
//
// 从 lower/stmt.cpp 拆出（文件规模硬限 1200 行）。list 的 += / *= 是原地
// 操作（CPython __iadd__/__imul__），类实例走 dunder，其余退化到 bin op。
#include "lower/impl.h"

namespace pyadamas {

void Lowerer::lower_aug_assign(AugAssign& a) {
    VReg cur_v = expr(*a.target);
    TypePtr cur_t = tc_.type_of(a.target.get());
    VReg rhs = expr(*a.value);
    TypePtr rhs_t = tc_.type_of(a.value.get());
    TypePtr ty = tc_.type_of(a.target.get());
    if (ty && ty->kind == TyKind::Big) {
        VReg out = lower_big_binary(cur_v, cur_t, rhs, rhs_t, aug_to_bin(a.op), a.line, a.col);
        assign_to(*a.target, out, vreg_ty(out));
        return;
    }
    if (ty && ty->kind == TyKind::Float) {
        if (!ir_is_float(cur_t)) cur_v = convert_to(cur_v, cur_t, t_float());
        if (!ir_is_float(rhs_t)) rhs = convert_to(rhs, rhs_t, t_float());
    }
    // 类实例的增量赋值：__iadd__ 优先，其次普通 __add__，
    // 右侧是类时还有反射版 __radd__。
    bool cls_involved = (cur_t && cur_t->kind == TyKind::Class) ||
                        (rhs_t && rhs_t->kind == TyKind::Class);
    if (cls_involved) {
        VReg dr = lower_dunder_binop(cur_v, cur_t, rhs, rhs_t, a.op, true);
        if (dr != kNoReg) {
            assign_to(*a.target, dr, vreg_ty(dr));
            return;
        }
    }
    // **list 的增量赋值是原地操作**（CPython 调 __iadd__/__imul__），
    // 不能退化成 bin.add —— 那条路径对 list 是「后端不支持」的
    // 兜底，生成的是指针算术（N10 段错误）。
    //   l += xs  → pa_list_extend(l, xs)（xs 可为任意 iterable 的
    //              list 形态，与 typecheck 的 list.__iadd__ 口径一致）
    //   l *= n   → pa_list_inplace_repeat(l, n)
    // **set / dict 的增量赋值同样不能落到 bin op。**
    // Python 里 `s |= t` / `s &= t` / `s -= t` / `s ^= t` 是原地集合运算，
    // 后端对 set 的 bin 运算没有实现——落到 gen_bin_int 会把两个哈希表
    // 指针当整数算，结果是野指针/空表（`s |= {2}` 打出 set() 并 rc 崩）。
    // 这里显式分派到既有运行时入口：update（|= / +=）、dict 的 |=。
    if (ty && (ty->kind == TyKind::Set || ty->kind == TyKind::Dict)) {
        bool is_set = ty->kind == TyKind::Set;
        // dict 只有 |=（Python 的 d += x 是 TypeError）；set 才有 |= / += / &= / -= / ^=。
        bool ok_op = is_set ? (a.op == Tok::PLUSEQUAL || a.op == Tok::VBAREQUAL ||
                               a.op == Tok::AMPEREQUAL || a.op == Tok::MINUSEQUAL ||
                               a.op == Tok::CIRCUMFLEXEQUAL)
                            : (a.op == Tok::VBAREQUAL);
        if (ok_op) {
            TypePtr ot = rhs_t;
            bool same = ot && (ot->kind == ty->kind || ot->kind == TyKind::Unknown);
            if (!same) {
                err(a.line, a.col,
                    std::format("unsupported in-place operator on '{}'",
                                type_name(ty)));
                return;
            }
            // set 复用 dict 的键集合：|= / += 是 pa_dict_update（同 set.update），
            // 其余是专门的原地集合运算；dict 只有 |=。
            RtFn fn = RtFn::DictUpdate;  // set |= / dict |= 都复用 dict.update
            if (is_set) {
                switch (a.op) {
                    case Tok::AMPEREQUAL: fn = RtFn::SetIntersectUpdate; break;
                    case Tok::MINUSEQUAL: fn = RtFn::SetDifferenceUpdate; break;
                    case Tok::CIRCUMFLEXEQUAL: fn = RtFn::SetSymdiffUpdate; break;
                    default: break;
                }
            }
            call_void(fn, {cur_v, rhs});
            ++side_effect_seq_;
            return;
        }
    }
    if (ty && ty->kind == TyKind::List) {
        VReg obj = cur_v;
        if (a.op == Tok::PLUSEQUAL) {
            if (!rhs_t || (rhs_t->kind != TyKind::List &&
                           rhs_t->kind != TyKind::Tuple)) {
                err(a.line, a.col,
                    std::format("'{}' can only be extended with a list or tuple",
                                type_name(ty)));
                return;
            }
            call_void(RtFn::ListExtend, {obj, rhs});
        } else if (a.op == Tok::STAREQUAL) {
            VReg n = coerce(rhs, rhs_t, t_int());
            call_void(RtFn::ListInplaceRepeat, {obj, n});
        } else {
            err(a.line, a.col,
                std::format("unsupported in-place operator on '{}'",
                            type_name(ty)));
            return;
        }
        // 原地操作没有新值；目标已就地更新，不需要再赋值。
        ++side_effect_seq_;
        return;
    }
    if (opts_.needs_dynamic_big && is_unknown(ty)) {
        VReg out = lower_any_big_binary(cur_v, rhs, aug_to_bin(a.op));
        if (out != kNoReg) {
            assign_to(*a.target, out, t_unknown());
            return;
        }
    }
    // tuple 的原地运算没有运行时入口：落到下面的 IrOp::Bin 会走指针算术
    // （实测 tuple += tuple 打出野指针、bytes += bytes 段错误），必须显式拒绝。
    if (ty && (ty->kind == TyKind::Tuple)) {
        err(a.line, a.col,
            std::format("'{}' does not support in-place operator here; write it out "
                        "explicitly (e.g. res = a + b)",
                        type_name(ty)));
        return;
    }
    Inst in;
    in.op = IrOp::Bin;
    in.bop = map_bin(aug_to_bin(a.op));
    in.a = cur_v;
    in.b = rhs;
    VReg r = def_heap(ty, std::move(in));
    // 下标目标会重新求值一次下标；Python 只求一次。
    // 纯下标表达式没有副作用，先这样，等有优化遍再消。
    assign_to(*a.target, r, ty);
}

}  // namespace pyadamas
