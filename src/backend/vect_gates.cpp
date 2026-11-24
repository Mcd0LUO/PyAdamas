// backend/vect_gates.cpp —— 通用向量化器（VectPlanner）共享的结构判据
//
// 0.480：原 backend/simd.cpp 是特化 SIMD 模板（PYADAMAS_SIMD，默认关）。通用
// 向量化（backend/vect.cpp + opt/vect_planner*.cpp）落地后那些模板对当前 IR
// 一次都没再匹配上（12 基准 asm 零 SIMD 标记、指令数与「关掉向量化」逐位相同），
// 模板与其开关整段删除，只保留 VectPlanner 复用的纯 IR 形状判据：
//   块形状（for 的 incr/cond、while 的两块形状）、循环三块 op 白名单、
//   槽写者/取址/逃逸/稳定性、scratch 操作数安全、vreg 定义查找。
// 这些判据只看 IR 结构，不发射任何指令，与具体基准形状无关（不是特化）。
//
// 设计见 docs/vect-planner-design.md；删除决定见 CHANGELOG 0.480 与
// docs/simd-vectorization-design.md。
#include "backend/impl.h"

namespace pyadamas {

static const Inst* simd_def_inst(const IRFunction& f, const std::vector<int>& def_block, VReg v) {
    if (v == kNoReg || v >= static_cast<VReg>(def_block.size())) return nullptr;
    int bi = def_block[static_cast<size_t>(v)];
    if (bi < 0 || bi >= static_cast<int>(f.blocks.size())) return nullptr;
    for (const Inst& in : f.blocks[static_cast<size_t>(bi)].insts)
        if (in.dst == v) return &in;
    return nullptr;
}
const Inst* AsmGen::simd_def(VReg v) const {
    return simd_def_inst(*f_, def_block_, v);
}
bool AsmGen::simd_const_int(VReg v, int64_t want) const {
    const Inst* d = simd_def(v);
    return d && d->op == IrOp::ConstInt && d->imm == want;
}
bool AsmGen::simd_incr_cond_ok(int incr_bi, int body_bi, int sJ,
                               int& cond_bi, int& end_bi, VReg& n) const {
    if (incr_bi < 0 || incr_bi >= static_cast<int>(f_->blocks.size()) || incr_bi == body_bi)
        return false;
    const auto& INC = f_->blocks[static_cast<size_t>(incr_bi)].insts;
    if (INC.size() != 4 || INC[0].op != IrOp::LoadSlot || INC[0].idx != sJ ||
        INC[1].op != IrOp::Bin || INC[1].bop != BinKind::Add || INC[1].a != INC[0].dst ||
        INC[2].op != IrOp::StoreSlot || INC[2].idx != sJ || INC[2].a != INC[1].dst ||
        INC[3].op != IrOp::Jump)
        return false;
    if (!simd_const_int(INC[1].b, 1)) return false;       // 步长必须 1
    cond_bi = INC[3].target;
    if (cond_bi < 0 || cond_bi >= static_cast<int>(f_->blocks.size()) ||
        cond_bi == body_bi || cond_bi == incr_bi)
        return false;
    const auto& C = f_->blocks[static_cast<size_t>(cond_bi)].insts;
    if (C.size() != 3 || C[0].op != IrOp::LoadSlot || C[0].idx != sJ ||
        C[1].op != IrOp::Cmp || C[1].cop != CmpKind::Lt || C[1].a != C[0].dst ||
        C[2].op != IrOp::Branch || C[2].a != C[1].dst || C[2].target != body_bi)
        return false;
    n = C[1].b;
    end_bi = C[2].target2;
    return end_bi >= 0 && end_bi != body_bi && end_bi != cond_bi && end_bi != incr_bi;
}
bool AsmGen::simd_while_shape_ok(int cond_bi, int body_bi, int& sJ,
                                 int& end_bi, VReg& n, int& jupd_pos,
                                 int& step_slot) const {
    if (cond_bi < 0 || cond_bi >= static_cast<int>(f_->blocks.size()) || cond_bi == body_bi)
        return false;
    const auto& C = f_->blocks[static_cast<size_t>(cond_bi)].insts;
    if (C.size() != 3 || C[0].op != IrOp::LoadSlot || C[1].op != IrOp::Cmp ||
        C[1].cop != CmpKind::Lt || C[1].a != C[0].dst || C[2].op != IrOp::Branch ||
        C[2].a != C[1].dst || C[2].target != body_bi)
        return false;
    const auto& B = f_->blocks[static_cast<size_t>(body_bi)].insts;
    if (B.size() < 6) return false;                      // 体 + 增量三条 + jump（不变量步长少一条常量）
    const size_t k = B.size() - 4;
    if (B[k].op != IrOp::LoadSlot || B[k].idx != C[0].idx) return false;
    if (B[k + 1].op != IrOp::Bin || B[k + 1].bop != BinKind::Add ||
        B[k + 1].a != B[k].dst)
        return false;
    step_slot = -1;
    if (!simd_const_int(B[k + 1].b, 1)) {
        // 不变量步长：b 来自一条 LoadSlot（非归纳变量自身的槽）。步长的装载
        // 通常在循环前（提升后的 p 只读一次），所以扫函数全部块找定义。
        int cand = -1;
        for (const auto& blk : f_->blocks)
            for (const Inst& li : blk.insts)
                if (li.op == IrOp::LoadSlot && li.dst == B[k + 1].b) { cand = li.idx; break; }
        if (cand < 0 || cand == C[0].idx) return false;
        step_slot = cand;
    }
    if (B[k + 2].op != IrOp::StoreSlot || B[k + 2].idx != C[0].idx ||
        B[k + 2].a != B[k + 1].dst)
        return false;
    if (B[k + 3].op != IrOp::Jump || B[k + 3].target != cond_bi) return false;
    sJ = C[0].idx;
    n = C[1].b;
    end_bi = C[2].target2;
    jupd_pos = static_cast<int>(k);
    return end_bi >= 0 && end_bi != body_bi && end_bi != cond_bi;
}
bool AsmGen::simd_loop_ops_ok(int cond_bi, int body_bi, int incr_bi,
                              bool allow_sqrt, bool allow_idxset) const {
    for (int bi : {cond_bi, body_bi, incr_bi})
        for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts) {
            switch (in.op) {
                case IrOp::LoadSlot: case IrOp::StoreSlot: case IrOp::Bin:
                case IrOp::Cmp: case IrOp::Convert: case IrOp::Mov:
                case IrOp::Branch: case IrOp::Jump: case IrOp::IndexLoad:
                case IrOp::IndexLoadUnchecked: case IrOp::ConstInt:
                case IrOp::ConstFloat: case IrOp::LoadGlobal:
                    break;
                case IrOp::CallRt:
                    // 只放行数学 sqrt（体块形状另钉死位置与个数）；其余运行时
                    // 调用可能碰容器/GC，一律拒绝。
                    if (!allow_sqrt || static_cast<RtFn>(in.imm) != RtFn::MathSqrt)
                        return false;
                    break;
                case IrOp::IndexStore: case IrOp::IndexStoreUnchecked:
                    if (!allow_idxset) return false;
                    break;
                default:
                    return false;
            }
        }
    return true;
}
bool AsmGen::simd_slot_unaddressed(int slot) const {
    for (const auto& bb : f_->blocks)
        for (const Inst& in : bb.insts)
            if (in.op == IrOp::SlotAddr && in.idx == slot) return false;
    return true;
}
bool AsmGen::simd_outside(VReg v, int cond_bi, int body_bi, int incr_bi) const {
    if (v == kNoReg || v >= static_cast<VReg>(def_block_.size())) return false;
    int db = def_block_[static_cast<size_t>(v)];
    return db >= 0 && db != cond_bi && db != body_bi && db != incr_bi;
}
bool AsmGen::simd_slot_stable(VReg v, int cond_bi, int body_bi, int incr_bi) const {
    const Inst* d = simd_def(v);
    if (!d) return false;
    if (d->op != IrOp::LoadSlot) return true;             // SSA 跨块值：循环内无写
    int sl = d->idx;
    for (int bi : {cond_bi, body_bi, incr_bi})
        for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts)
            if (in.op == IrOp::StoreSlot && in.idx == sl) return false;
    return true;
}
bool AsmGen::simd_scratch_safe(const MOperand& o) {
    if (o.kind != MOperandKind::Reg) return true;
    MReg r = o.reg;
    if (r == kRax || r == kRcx || r == kRdx || r == kRsi || r == kRdi) return false;
    if (r >= kXmm0 && r <= kXmm7) return false;
    return true;
}
}  // namespace pyadamas
