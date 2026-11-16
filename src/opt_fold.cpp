// opt_fold.cpp —— 常量判定辅助、纯指令判定与 DCE
#include "opt_detail.h"

namespace pyadamas {
namespace optdetail {

bool bin_is_safe_int(BinKind b) {
    switch (b) {
        case BinKind::Add:
        case BinKind::Sub:
        case BinKind::Mul:
        case BinKind::BitAnd:
        case BinKind::BitOr:
        case BinKind::BitXor:
            return true;
        default:
            return false;
    }
}

bool bin_is_safe_float(BinKind b) {
    return b == BinKind::Add || b == BinKind::Sub || b == BinKind::Mul;
}

void set_const_int(Inst& in, int64_t v) {
    in.op = IrOp::ConstInt;
    in.imm = v;
    in.fimm = 0.0;
    in.a = in.b = in.c = kNoReg;
    in.args.clear();
}

void set_const_float(Inst& in, double v) {
    in.op = IrOp::ConstFloat;
    in.imm = 0;
    in.fimm = v;
    in.a = in.b = in.c = kNoReg;
    in.args.clear();
}

bool inst_is_pure(const Inst& in) {
    switch (in.op) {
        case IrOp::ConstInt:
        case IrOp::ConstFloat:
        case IrOp::ConstStr:
        case IrOp::ConstNone:
        case IrOp::FuncAddr:
        case IrOp::FuncCode:
        case IrOp::Mov:
        case IrOp::LoadSlot:
        case IrOp::LoadGlobal:
        case IrOp::LoadExtern:
        case IrOp::Un:
        case IrOp::Cmp:
        case IrOp::Len:
        case IrOp::IndexLoadUnchecked:
        case IrOp::FieldLoad:
        case IrOp::PtrLoad:
            return true;
        case IrOp::Bin: {
            TypePtr t = in.ty;
            if (t && (t->kind == TyKind::Str || t->kind == TyKind::Complex)) return false;
            if (ir_is_float(t)) return bin_is_safe_float(in.bop);
            return bin_is_safe_int(in.bop);
        }
        case IrOp::Convert:
            return in.ty && (in.ty->kind == TyKind::Bool || in.ty->kind == TyKind::Int ||
                             in.ty->kind == TyKind::Float);
        default:
            return false;
    }
}

void dce_function(IRFunction& fn) {
    if (fn.vreg_ty.empty()) return;
    bool changed = true;
    while (changed) {
        changed = false;
        // 用函数级的 use-def 索引，不再每轮手工扫一遍全函数统计使用次数。
        //
        // 语义等价：uses_of[v].size() 就是 v 被读的次数。索引由
        // rebuild_use_def() 一次 O(指令数) 重建，这里每轮复用；
        // 删掉指令后重新建一次（删的是纯计算，不影响其他指令的 use）。
        fn.rebuild_use_def();
        auto use_count = [&](VReg r) -> size_t {
            auto it = fn.uses_of.find(r);
            return it == fn.uses_of.end() ? 0 : it->second.size();
        };
        for (auto& b : fn.blocks) {
            auto& v = b.insts;
            for (size_t i = 0; i < v.size();) {
                const Inst& in = v[i];
                bool dead = in.dst != kNoReg && use_count(in.dst) == 0 &&
                            inst_is_pure(in);
                if (!dead) {
                    ++i;
                    continue;
                }
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                // uses 的计数已经过期；下一轮外层 while 重新统计。
                // 这里继续扫后面的指令是保守的：最多晚一轮删除。
            }
        }
    }
}

}  // namespace optdetail
}  // namespace pyadamas
