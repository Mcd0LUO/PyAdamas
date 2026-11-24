// opt/vect_planner_stride.cpp —— 不变量步长（q += p）归纳变量的向量化支撑
//
// 从 opt/vect_planner_loop.cpp 拆出（0.449）：文件规模纪律 1200 行。两段逻辑：
//   1. stride_loop_check：形状校验（步长槽 int / 循环内不写不取址；体块恰一条
//      IndexStore 且下标是归纳变量装载；归纳变量只用于下标与增量两处）；
//   2. stride_loop_prologue：入口守卫（p>=1、p<=n、J1=q+p 在界内）与每轮推进量
//      2p 的寄存器准备。
//
// 发射时两 lane 是 q 与 q+p（散射写：movsd 低 lane + movhlps 抽高 lane + movsd），
// 循环推进 q += 2p、条件 J1 < n；余 1 次走标量尾（IndexError 时机不变）。
#include "opt/vect_planner.h"

#include <cstdlib>

#include "backend/impl.h"

namespace pyadamas {

bool VectPlanner::stride_loop_check(Loop& L) {
    L.has_stride = true;
    L.stride_slot = L.step_slot_hint;
    if (L.stride_slot < 0 || L.stride_slot >= static_cast<int>(gen_.f_->slot_ty.size()))
        return false;
    TypePtr ts = gen_.f_->slot_ty[static_cast<size_t>(L.stride_slot)];
    if (!ts || ts->kind != TyKind::Int) return false;
    if (!gen_.simd_slot_unaddressed(L.stride_slot)) return false;
    int idx_stores = 0;
    int j_loads = 0;
    for (int bi : {L.cond_bi, L.body_bi})
        for (const Inst& in : gen_.f_->blocks[static_cast<size_t>(bi)].insts) {
            if (in.op == IrOp::StoreSlot && in.idx == L.stride_slot) return false;
            if (bi != L.body_bi) continue;
            switch (in.op) {
                case IrOp::LoadSlot:
                    if (in.idx == L.stride_slot) continue;
                    if (in.idx != L.sJ) return false;
                    ++j_loads;
                    break;
                case IrOp::StoreSlot:
                    if (in.idx != L.sJ) return false;
                    break;
                case IrOp::IndexStore: ++idx_stores; break;
                case IrOp::ConstInt: case IrOp::ConstNone: case IrOp::Bin:
                case IrOp::Jump:
                    break;
                default: return false;
            }
        }
    return idx_stores == 1 && j_loads == 2;
}

void VectPlanner::stride_loop_prologue(Loop& L) {
    // 放在所有入口守卫/基址装载**之后**发射，避免 S2REG/J1REG 被守卫代码当
    // scratch（rcx/rdx）覆写。任一守卫失败 → 标量尾。
    gen_.emit2(MOp::kMovq, gen_.slot_mop(L.stride_slot), kRax);   // rax = p
    gen_.emit_imm(MOp::kCmpq, 1, kRax);
    gen_.emit_jmp(MOp::kJl, L.tail);                              // p < 1 → 标量
    gen_.emit2(MOp::kCmpq, L.n_op, kRax);
    gen_.emit_jmp("jg", L.tail);                                  // p > n → 标量
    gen_.emit2(MOp::kLeaq, AsmGen::op_mem(kRax, L.JREG, 1), L.J1REG);  // J1 = p + q
    gen_.emit2(MOp::kTestq, L.J1REG, L.J1REG);
    gen_.emit_jmp(MOp::kJs, L.tail);
    gen_.emit2(MOp::kCmpq, L.n_op, L.J1REG);
    gen_.emit_jmp(MOp::kJge, L.tail);                             // last lane 越界
    gen_.emit2(MOp::kAddq, kRax, kRax);                           // rax = 2p
    gen_.emit2(MOp::kMovq, kRax, L.S2REG);
}

}  // namespace pyadamas
