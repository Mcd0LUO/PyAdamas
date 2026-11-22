// vect_planner.cpp —— 见 docs/vect-planner-design.md
#include "vect_planner.h"

#include "codegen_impl.h"   // 只为 AsmGen::op_imm / op_reg / mreg_operand 这些静态工厂

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

void VectPlanner::vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh) {
    if (sh <= 0) {
        // m == 0：x // 1 == x（rd 已经是 x，什么都不发）；x % 1 == 0。
        if (is_mod) emit_.emit2(MOp::kPxor, mreg_operand(rd), mreg_operand(rd));
        return;
    }
    if (is_mod) {
        // x % 2^m = x & (2^m - 1)：左移把高 m 位推出去，再逻辑右移回来。
        emit_.emit_imm(MOp::kPsllq, 64 - sh, mreg_operand(rd));
        emit_.emit_imm(MOp::kPsrlq, 64 - sh, mreg_operand(rd));
        return;
    }
    // floor：sign = (x >> 31 每 dword 取符号) → pshufd 复制到 64 位 lane →
    // 左移 (64-m) 得到「高 m 位全 1 或 0」→ 与 (x 逻辑右移 m) 相或。
    emit_.emit2(MOp::kMovapd, mreg_operand(rd), mreg_operand(tmp));
    emit_.emit_imm(MOp::kPsrad, 31, mreg_operand(tmp));
    {
        MInstr ins;                          // pshufd 本 GAS 只认三操作数写法
        ins.op = MOp::kPshufd;
        ins.ops.push_back(AsmGen::op_imm(0xF5));
        ins.ops.push_back(AsmGen::op_reg(tmp));
        ins.ops.push_back(AsmGen::op_reg(tmp));
        emit_.emit_instr(std::move(ins));
    }
    emit_.emit_imm(MOp::kPsllq, 64 - sh, mreg_operand(tmp));
    emit_.emit_imm(MOp::kPsrlq, sh, mreg_operand(rd));
    emit_.line("por " + std::string(mreg_name(tmp)) + ", " + std::string(mreg_name(rd)));
}

void VectPlanner::vect_affine_kinit(MReg k, MReg jreg, int64_t coef) {
    emit_.emit2(MOp::kMovq, mreg_operand(jreg), mreg_operand(k));
    emit_.emit_imm(MOp::kImulq, coef * 8, mreg_operand(k));
}

void VectPlanner::vect_affine_step(MReg k, int64_t coef) {
    emit_.emit_imm(MOp::kAddq, coef * 16, mreg_operand(k));
}

void VectPlanner::vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                                   MReg dst, MReg scratch) {
    if (coef == 1) {
        emit_.emit2(MOp::kMovupd, AsmGen::op_mem(base, j, 8, off * 8), mreg_operand(dst));
        return;
    }
    emit_.emit2(MOp::kMovsd, AsmGen::op_mem(base, k, 1, off * 8), mreg_operand(dst));
    emit_.emit2(MOp::kMovsd, AsmGen::op_mem(base, k, 1, (off + coef) * 8), mreg_operand(scratch));
    emit_.emit2(MOp::kUnpcklpd, mreg_operand(scratch), mreg_operand(dst));
}

void VectPlanner::vect_affine_store(MReg base, MReg j, int64_t off, MReg src) {
    emit_.emit2(MOp::kMovupd, mreg_operand(src), AsmGen::op_mem(base, j, 8, off * 8));
}

}  // namespace pyadamas
