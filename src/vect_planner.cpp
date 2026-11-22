// vect_planner.cpp —— 见 docs/vect-planner-design.md
#include "vect_planner.h"

namespace pyadamas {
namespace {

// 正 2 的幂 → m（除数 == 2^m，0 <= m <= 62）；其余（<= 0、非幂、负数）→ -1。
int pow2_shift_i64(int64_t d) {
    if (d < 1) return -1;
    if ((d & (d - 1)) != 0) return -1;
    int m = 0;
    while ((d >> m) != 1) ++m;
    return m;
}

}  // namespace

bool VectPlanner::vect_div_pow2(VReg b, int* sh) const {
    if (b == kNoReg || sh == nullptr) return false;
    const Inst* def = nullptr;
    for (int hop = 0; hop < 8 && b != kNoReg; ++hop) {
        def = val_.simd_def(b);
        if (def == nullptr) return false;
        if (def->op == IrOp::ConstInt) break;
        if (def->op != IrOp::Mov) return false;
        b = def->a;
    }
    if (def == nullptr || def->op != IrOp::ConstInt) return false;
    const int m = pow2_shift_i64(def->imm);
    if (m < 0 || m > 62) return false;
    *sh = m;
    return true;
}

}  // namespace pyadamas
