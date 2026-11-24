// backend/simd_nbody.cpp —— nbody 位置更新 map SIMD（0.334，PYADAMAS_SIMD 门控）
// pairwise 力循环匹配器在 backend/simd_nbody_pair.cpp（文件规模闸门）。
//
// 设计见 docs/simd-vectorization-design.md §10 P2。模式：nbody advance 的
// 位置更新内循环（stride-7 逐元素 map，无 reduction/无 sqrt）：
//   for i in range(5):
//       b[i*7]   += dt * b[i*7+3]
//       b[i*7+1] += dt * b[i*7+4]
//       b[i*7+2] += dt * b[i*7+5]
// IR 体块恰 34 条核心指令。SSE2 2-lane：沿 i 轴取 (i, i+1) 两体，同一分量
// 在两体间相距 7*8=56 字节，用两条 movsd + unpcklpd 拼对（SSE2 无 gather）。
//
// 正确性命门：
//   * 逐 lane IEEE 乘/加与标量逐位一致；元素间独立（无 reduction），不改序。
//   * 别名：同一 list，读 b[i7+0..5] 写 b[i7+0..2]；两体行距 7 > 最大偏移 5，
//     行内读偏移 {0,1,2,3,4,5} 写偏移 {0,1,2} 互不重叠，跨迭代行不相交 →
//     无需运行时别名守卫（结构判据）。
//   * 范围守卫 j>=0、7*n<=len（含溢出检查）、kind==0：movupd/movsd 不越界、
//     读裸 double；不满足整段标量 fallback，IndexError/负索引时机不变。
//   * 向量循环零调用；dt 从槽/提升寄存器直读（循环内无写）。
//
// 寄存器：只用 xmm0-4（不碰提升池 xmm8-15，任何 fp 提升形态都安全）；
// GPR scratch rax/rcx/rdx/rsi/rdi，j 提升 GPR 或 %rsi 持有+每轮写回。
#include "backend/impl.h"

namespace pyadamas {

bool AsmGen::try_simd_nbody_pos(const Block& b) {
    if (!simd_func_ok()) return false;
    const int body_bi = cur_block_;
    if (body_bi < 0 || body_bi >= static_cast<int>(f_->blocks.size())) return false;
    std::vector<Inst> core;
    if (!simd_core_body(b, 34, 64, core)) return false;
    const auto& I = core;

    // ── 体块形状 ──────────────────────────────────────────────────
    auto op_is = [&](size_t k, IrOp o) { return I[k].op == o; };
    auto ld_is = [&](size_t k) {
        return I[k].op == IrOp::IndexLoad || I[k].op == IrOp::IndexLoadUnchecked;
    };
    auto st_is = [&](size_t k) {
        return I[k].op == IrOp::IndexStore || I[k].op == IrOp::IndexStoreUnchecked;
    };
    if (!op_is(0, IrOp::LoadSlot) || !op_is(1, IrOp::StoreSlot) ||
        !op_is(2, IrOp::LoadSlot) || !op_is(3, IrOp::LoadSlot) ||
        !op_is(4, IrOp::Bin) || !ld_is(5) ||
        !op_is(6, IrOp::LoadSlot) || !op_is(7, IrOp::Bin) || !ld_is(8) ||
        !op_is(9, IrOp::Bin) || !op_is(10, IrOp::Bin) ||
        !op_is(11, IrOp::LoadSlot) || !st_is(12) ||
        !op_is(13, IrOp::LoadSlot) || !op_is(14, IrOp::Bin) || !ld_is(15) ||
        !op_is(16, IrOp::LoadSlot) || !op_is(17, IrOp::Bin) || !ld_is(18) ||
        !op_is(19, IrOp::Bin) || !op_is(20, IrOp::Bin) ||
        !op_is(21, IrOp::LoadSlot) || !st_is(22) ||
        !op_is(23, IrOp::LoadSlot) || !op_is(24, IrOp::Bin) || !ld_is(25) ||
        !op_is(26, IrOp::LoadSlot) || !op_is(27, IrOp::Bin) || !ld_is(28) ||
        !op_is(29, IrOp::Bin) || !op_is(30, IrOp::Bin) ||
        !op_is(31, IrOp::LoadSlot) || !st_is(32) || !op_is(33, IrOp::Jump))
        return false;

    const VReg i1 = I[0].dst, i2 = I[3].dst, i7 = I[4].dst;
    const int sJ = I[0].idx, sC = I[1].idx, sB = I[2].idx;
    if (i1 == kNoReg || sC == sJ) return false;
    if (I[1].a != i1 || I[3].idx != sC) return false;
    if (I[4].bop != BinKind::Mul || I[4].a != i2) return false;
    const VReg seven = I[4].b;
    // 所有 list 装载同一槽 sB。
    for (size_t k : {2, 6, 11, 13, 16, 21, 23, 26, 31})
        if (I[k].idx != sB) return false;
    // 三个分量：pos 偏移 {0,1,2}，vel 偏移 {3,4,5}，dt 同一槽。
    const VReg p0 = I[5].dst, v0i = I[7].dst, v0 = I[8].dst, t0 = I[9].dst, n0 = I[10].dst;
    const VReg p1i = I[14].dst, p1 = I[15].dst, v1i = I[17].dst, v1 = I[18].dst;
    const VReg t1 = I[19].dst, n1 = I[20].dst;
    const VReg p2i = I[24].dst, p2 = I[25].dst, v2i = I[27].dst, v2 = I[28].dst;
    const VReg t2 = I[29].dst, n2 = I[30].dst;
    const VReg dt0 = I[9].a, dt1 = I[19].a, dt2 = I[29].a;
    if (I[5].a != I[2].dst || I[5].b != i7) return false;
    if (I[7].bop != BinKind::Add || I[7].a != i7) return false;
    if (I[8].a != I[6].dst || I[8].b != v0i) return false;
    if (I[9].bop != BinKind::Mul || I[9].b != v0) return false;
    if (I[10].bop != BinKind::Add || I[10].a != p0 || I[10].b != t0) return false;
    if (I[12].a != I[11].dst || I[12].b != i7 || I[12].c != n0) return false;
    if (I[14].bop != BinKind::Add || I[14].a != i7) return false;
    if (I[15].a != I[13].dst || I[15].b != p1i) return false;
    if (I[17].bop != BinKind::Add || I[17].a != i7) return false;
    if (I[18].a != I[16].dst || I[18].b != v1i) return false;
    if (I[19].bop != BinKind::Mul || I[19].b != v1) return false;
    if (I[20].bop != BinKind::Add || I[20].a != p1 || I[20].b != t1) return false;
    if (I[22].a != I[21].dst || I[22].b != p1i || I[22].c != n1) return false;
    if (I[24].bop != BinKind::Add || I[24].a != i7) return false;
    if (I[25].a != I[23].dst || I[25].b != p2i) return false;
    if (I[27].bop != BinKind::Add || I[27].a != i7) return false;
    if (I[28].a != I[26].dst || I[28].b != v2i) return false;
    if (I[29].bop != BinKind::Mul || I[29].b != v2) return false;
    if (I[30].bop != BinKind::Add || I[30].a != p2 || I[30].b != t2) return false;
    if (I[32].a != I[31].dst || I[32].b != p2i || I[32].c != n2) return false;
    // 常量偏移：vel +3/+4/+5，pos +1/+2；乘数 7。
    if (!simd_const_int(I[7].b, 3) || !simd_const_int(I[14].b, 1) ||
        !simd_const_int(I[17].b, 4) || !simd_const_int(I[24].b, 2) ||
        !simd_const_int(I[27].b, 5) || !simd_const_int(seven, 7)) return false;
    // dt 三个 vreg 必须同源同一槽（load.slot sDT）。
    const Inst* d0 = simd_def(dt0);
    if (!d0 || d0->op != IrOp::LoadSlot) return false;
    const int sDT = d0->idx;
    for (VReg dv : {dt1, dt2}) {
        const Inst* dd = simd_def(dv);
        if (!dd || dd->op != IrOp::LoadSlot || dd->idx != sDT) return false;
    }
    // 类型：浮点结果 + 整数索引。
    for (VReg r : {p0, v0, t0, n0, p1, v1, t1, n1, p2, v2, t2, n2})
        if (!ir_is_float(ty_of(r))) return false;
    for (VReg r : {i7, i2}) {
        TypePtr ty = ty_of(r);
        if (!ty || ty->kind != TyKind::Int) return false;
    }
    const int incr_bi = I[33].target;

    // ── cond / incr / 白名单 / 槽纪律 ──────────────────────────────
    int cond_bi = -1, end_bi = -1;
    VReg n = kNoReg;
    if (!simd_incr_cond_ok(incr_bi, body_bi, sJ, cond_bi, end_bi, n)) return false;
    if (!simd_loop_ops_ok(cond_bi, body_bi, incr_bi, false, true)) return false;
    if (simd_slot_writers(sJ, cond_bi, body_bi, incr_bi) != 1) return false;
    if (simd_slot_writers(sC, cond_bi, body_bi, incr_bi) != 1) return false;
    if (simd_slot_writers(sB, cond_bi, body_bi, incr_bi) != 0) return false;
    if (simd_slot_writers(sDT, cond_bi, body_bi, incr_bi) != 0) return false;
    if (sJ < 0 || sJ >= static_cast<int>(f_->slot_ty.size())) return false;
    { TypePtr tj = f_->slot_ty[static_cast<size_t>(sJ)];
      if (!tj || tj->kind != TyKind::Int) return false; }
    if (!simd_slot_unaddressed(sJ) || !simd_slot_unaddressed(sC) ||
        !simd_slot_unaddressed(sB) || !simd_slot_unaddressed(sDT)) return false;
    if (!simd_outside(n, cond_bi, body_bi, incr_bi) ||
        !simd_slot_stable(n, cond_bi, body_bi, incr_bi)) return false;
    for (VReg dv : {dt0, dt1, dt2})
        if (!simd_outside(dv, cond_bi, body_bi, incr_bi)) return false;

    // ── 物理位置 ──────────────────────────────────────────────────
    const MOperand& jloc = slot_mop(sJ);
    const bool j_stack = jloc.kind != MOperandKind::Reg;
    if (!j_stack && (jloc.reg < kRax || jloc.reg > kR15)) return false;
    const MOperand b_op = slot_mop(sB), dt_op = slot_mop(sDT), n_op = src_mop(n);
    if (!simd_scratch_safe(b_op) || !simd_scratch_safe(dt_op) ||
        !simd_scratch_safe(n_op)) return false;

    // ── 发射向量前奏 ──────────────────────────────────────────────
    const MReg JREG = j_stack ? kRsi : jloc.reg;
    const std::string tail = fresh_label("simdtail");
    const std::string vec = fresh_label("simdvec");
    if (std::getenv("PYADAMAS_SIMD_DBG"))
        std::fprintf(stderr, "[simd] vectorize nbody pos loop body bb%d (sJ=%d sB=%d)\n",
                     body_bi, sJ, sB);
    comment("SIMD nbody pos (PYADAMAS_SIMD): SSE2 2-lane stride-7 map");
    if (j_stack) emit2(MOp::kMovq, jloc, JREG);

    // 守卫：j+1<n、j>=0、7*n<=len（溢出检查）、kind==0。
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJge, tail);
    emit2(MOp::kTestq, JREG, JREG);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kMovq, n_op, kRax);
    emit_imm(MOp::kImulq, 7, kRax);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kMovq, b_op, kRdi);
    emit2(MOp::kCmpq, op_mem(kRdi, 0), kRax);
    emit_jmp(MOp::kJg, tail);
    emit_imm(MOp::kCmpq, 0, op_mem(kRdi, 24));
    emit_jmp(MOp::kJne, tail);

    // setup：rdx = data，rdi = i7 = j*7，xmm4 = [dt,dt]。
    emit2(MOp::kMovq, op_mem(kRdi, 16), kRdx);
    emit2(MOp::kMovq, JREG, kRdi);
    emit_imm(MOp::kImulq, 7, kRdi);
    emit2(MOp::kMovsd, dt_op, kXmm4);
    emit2(MOp::kMovlhps, kXmm4, kXmm4);

    // 向量循环体（每轮 2 体 × 3 分量）。disp = 分量*8，+56 = 下一体同分量。
    label(vec);
    for (int comp = 0; comp < 3; ++comp) {
        const int64_t pd = comp * 8, vd = (comp + 3) * 8;
        emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, pd), kXmm0);
        emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, pd + 56), kXmm1);
        emit2(MOp::kUnpcklpd, kXmm1, kXmm0);              // POS = [p_i, p_i+1]
        emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, vd), kXmm1);
        emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, vd + 56), kXmm2);
        emit2(MOp::kUnpcklpd, kXmm2, kXmm1);              // VEL
        emit2(MOp::kMulpd, kXmm4, kXmm1);                 // dt*vel（逐 lane 同标量）
        emit2(MOp::kAddpd, kXmm1, kXmm0);                 // pos + t
        emit2(MOp::kMovsd, kXmm0, op_mem(kRdx, kRdi, 8, pd));
        emit2(MOp::kMovhlps, kXmm0, kXmm0);
        emit2(MOp::kMovsd, kXmm0, op_mem(kRdx, kRdi, 8, pd + 56));
    }
    emit_imm(MOp::kAddq, 14, kRdi);                       // i7 += 14
    emit_imm(MOp::kAddq, 2, JREG);
    if (j_stack) emit2(MOp::kMovq, JREG, jloc);
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJl, vec);
    // 退出：j==n-1 标量尾余 1；j==n 吃尽跳 cond（j 保持 n）。
    emit2(MOp::kCmpq, n_op, JREG);
    emit_jmp(MOp::kJl, tail);
    emit_jmp(MOp::kJmp, block_label(cond_bi));
    label(tail);
    return true;
}

}  // namespace pyadamas
