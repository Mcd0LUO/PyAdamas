// backend/simd_int.cpp —— matmul 整数内循环 SIMD（0.334，PYADAMAS_SIMD 门控）
//
// 设计见 docs/simd-vectorization-design.md §9 P3。模式：整数矩阵乘的 j 内循环
//   for j in range(N): c[i*N+j] += aik * b[k*N+j]
// IR 体块恰 17 条核心指令（O2 LICM 后 i*N/k*N/aik 外提）。SSE2 2-lane i64：
//   movupd 取 c/b 相邻两元素 → pmuludq 组合乘 → paddq 累加 → movupd 写回。
//
// 正确性命门：
//   * pyadamas 整数是 i64 回绕语义（标量 imulq/addq 无溢出检查）。paddq 与
//     addq 逐位一致（模 2^64）。乘法用 pmuludq 组合精确重构 imulq 低 64 位：
//     aik ∈ [0, 2^31) 守卫下 prod = aik*b_lo32 + ((aik*(b>>32)_lo32) << 32)，
//     对任意 i64 的 b 逐位等于 imulq（推导见设计文档 §10）。守卫不满足整段
//     标量 fallback —— 不做 32 位截断乘法（pmulld 会算错）。
//   * 别名守卫：b/c 必须不同对象（同对象时写 c[iN+j] 可能覆盖后续迭代读的
//     b[kN+j']，标量顺序语义被破坏）；c 读/写基址运行时判等。
//   * 范围守卫 j>=0、base>=0、base+n<=len（含溢出检查）、kind==0：保证
//     movupd 不越界、读的是裸 i64；不满足走标量，IndexError/负索引语义与时机不变。
//   * 向量循环零调用：list/data/不变量在循环期间不可能被改写，无 GC safepoint。
//
// 寄存器约定沿用原型：scratch 只用永不提升的 GPR(rax/rcx/rdx/rsi/rdi) 与
// xmm0-7；j 原地 addq $2（提升 GPR 或 %rsi 持有+每轮写回栈）。
#include "backend/impl.h"

namespace pyadamas {

bool AsmGen::try_simd_int_map(const Block& b) {
    if (!simd_func_ok()) return false;
    const int body_bi = cur_block_;
    if (body_bi < 0 || body_bi >= static_cast<int>(f_->blocks.size())) return false;
    std::vector<Inst> core;
    if (!simd_core_body(b, 17, 32, core)) return false;
    const auto& I = core;

    // ── 体块形状：c[cbr+j] += aik * b[bb+j] ────────────────────────
    auto op_is = [&](size_t k, IrOp o) { return I[k].op == o; };
    if (!op_is(0, IrOp::LoadSlot) || !op_is(1, IrOp::StoreSlot) ||
        !op_is(2, IrOp::LoadSlot) || !op_is(3, IrOp::LoadSlot) ||
        !op_is(4, IrOp::Bin) ||
        (I[5].op != IrOp::IndexLoad && I[5].op != IrOp::IndexLoadUnchecked) ||
        !op_is(6, IrOp::LoadSlot) || !op_is(7, IrOp::LoadSlot) ||
        !op_is(8, IrOp::Bin) ||
        (I[9].op != IrOp::IndexLoad && I[9].op != IrOp::IndexLoadUnchecked) ||
        !op_is(10, IrOp::Bin) || !op_is(11, IrOp::Bin) ||
        !op_is(12, IrOp::LoadSlot) || !op_is(13, IrOp::LoadSlot) ||
        !op_is(14, IrOp::Bin) ||
        (I[15].op != IrOp::IndexStore && I[15].op != IrOp::IndexStoreUnchecked) ||
        !op_is(16, IrOp::Jump))
        return false;

    const VReg j1 = I[0].dst, j2 = I[3].dst, j3 = I[7].dst, j4 = I[13].dst;
    const VReg cl1 = I[2].dst, ci_r = I[4].dst, cv = I[5].dst;
    const VReg bl = I[6].dst, bi = I[8].dst, bv = I[9].dst;
    const VReg prod = I[10].dst, nv = I[11].dst, cl2 = I[12].dst, ci_w = I[14].dst;
    const int sJ = I[0].idx, sC = I[1].idx, sCL = I[2].idx, sBL = I[6].idx;
    if (j1 == kNoReg || sC == sJ || sCL == sBL) return false;
    if (I[1].a != j1 || I[1].idx != sC) return false;
    if (I[3].idx != sC || I[7].idx != sC || I[13].idx != sC) return false;
    if (I[12].idx != sCL) return false;                   // c 读/写同一槽
    if (I[4].bop != BinKind::Add || I[4].b != j2) return false;
    if (I[5].a != cl1 || I[5].b != ci_r) return false;
    if (I[8].bop != BinKind::Add || I[8].b != j3) return false;
    if (I[9].a != bl || I[9].b != bi) return false;
    if (I[10].bop != BinKind::Mul || I[10].b != bv) return false;
    if (I[11].bop != BinKind::Add || I[11].a != cv || I[11].b != prod) return false;
    if (I[14].bop != BinKind::Add || I[14].b != j4) return false;
    if (I[15].a != cl2 || I[15].b != ci_w || I[15].c != nv) return false;
    const VReg cbr = I[4].a, bb = I[8].a, aik = I[10].a, cbw = I[14].a;
    const int incr_bi = I[16].target;

    // 整数类型（排除 float/unknown 混入；list[float] kind0 的 cv 是 float → 拒）。
    for (VReg r : {cv, bv, prod, nv, cbr, bb, cbw, aik}) {
        TypePtr ty = ty_of(r);
        if (!ty || ty->kind != TyKind::Int) return false;
    }

    // ── cond / incr / 白名单 / 槽纪律 ──────────────────────────────
    int cond_bi = -1, end_bi = -1;
    VReg n = kNoReg;
    if (!simd_incr_cond_ok(incr_bi, body_bi, sJ, cond_bi, end_bi, n)) return false;
    if (!simd_loop_ops_ok(cond_bi, body_bi, incr_bi, false, true)) return false;
    if (simd_slot_writers(sJ, cond_bi, body_bi, incr_bi) != 1) return false;
    if (simd_slot_writers(sC, cond_bi, body_bi, incr_bi) != 1) return false;
    if (simd_slot_writers(sCL, cond_bi, body_bi, incr_bi) != 0) return false;
    if (simd_slot_writers(sBL, cond_bi, body_bi, incr_bi) != 0) return false;
    if (sJ < 0 || sJ >= static_cast<int>(f_->slot_ty.size())) return false;
    { TypePtr tj = f_->slot_ty[static_cast<size_t>(sJ)];
      if (!tj || tj->kind != TyKind::Int) return false; }
    if (!simd_slot_unaddressed(sJ) || !simd_slot_unaddressed(sC) ||
        !simd_slot_unaddressed(sCL) || !simd_slot_unaddressed(sBL)) return false;

    // 不变量定义在循环外；load.slot 来源的槽循环内无写。
    for (VReg v : {cbr, bb, cbw, aik, n}) {
        if (!simd_outside(v, cond_bi, body_bi, incr_bi)) return false;
        if (!simd_slot_stable(v, cond_bi, body_bi, incr_bi)) return false;
    }

    // ── 物理位置 ──────────────────────────────────────────────────
    const MOperand& jloc = slot_mop(sJ);
    const bool j_stack = jloc.kind != MOperandKind::Reg;
    if (!j_stack && (jloc.reg < kRax || jloc.reg > kR15)) return false;
    // c/b 列表指针从槽取（体块内 load.slot 的 vreg 此刻尚未装载，不能 src_mop）。
    const MOperand cl_op = slot_mop(sCL), bl_op = slot_mop(sBL);
    const MOperand n_op = src_mop(n), cbr_op = src_mop(cbr), bb_op = src_mop(bb);
    const MOperand cbw_op = src_mop(cbw), aik_op = src_mop(aik);
    if (!simd_scratch_safe(cl_op) || !simd_scratch_safe(bl_op) ||
        !simd_scratch_safe(n_op) || !simd_scratch_safe(cbr_op) ||
        !simd_scratch_safe(bb_op) || !simd_scratch_safe(cbw_op) ||
        !simd_scratch_safe(aik_op)) return false;

    // ── 发射向量前奏 ──────────────────────────────────────────────
    const MReg JREG = j_stack ? kRsi : jloc.reg;
    const std::string tail = fresh_label("simdtail");
    const std::string vec = fresh_label("simdvec");
    if (std::getenv("PYADAMAS_SIMD_DBG"))
        std::fprintf(stderr, "[simd] vectorize int map loop body bb%d (sJ=%d sCL=%d sBL=%d)\n",
                     body_bi, sJ, sCL, sBL);
    comment("SIMD int map (PYADAMAS_SIMD): SSE2 2-lane i64, guarded pmuludq");
    if (j_stack) emit2(MOp::kMovq, jloc, JREG);

    // 守卫：j+1<n、j>=0、aik∈[0,2^31)、cbr==cbw、base>=0 且 base+n<=len、
    // b!=c 对象、kind==0。任一失败走标量尾（异常语义/时机不变）。
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJge, tail);
    emit2(MOp::kTestq, JREG, JREG);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kMovq, aik_op, kRax);
    emit2(MOp::kMovq, kRax, kRcx);
    emit_imm(MOp::kShrq, 31, kRcx);                       // aik<0 或 >=2^31 → 非零
    emit_jmp(MOp::kJne, tail);
    if (cbr != cbw) {                                     // 读/写基址运行时判等
        emit2(MOp::kMovq, cbr_op, kRax);
        emit2(MOp::kCmpq, cbw_op, kRax);
        emit_jmp(MOp::kJne, tail);
    }
    emit2(MOp::kMovq, cbr_op, kRax);
    emit2(MOp::kTestq, kRax, kRax);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kAddq, n_op, kRax);
    emit_jmp(MOp::kJs, tail);                             // base+n 溢出
    emit2(MOp::kMovq, cl_op, kRdi);
    emit2(MOp::kCmpq, op_mem(kRdi, 0), kRax);
    emit_jmp(MOp::kJg, tail);
    emit2(MOp::kMovq, bb_op, kRax);
    emit2(MOp::kTestq, kRax, kRax);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kAddq, n_op, kRax);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kMovq, bl_op, kRcx);
    emit2(MOp::kCmpq, op_mem(kRcx, 0), kRax);
    emit_jmp(MOp::kJg, tail);
    emit2(MOp::kCmpq, kRdi, kRcx);                        // b 与 c 必须不同对象
    emit_jmp(MOp::kJe, tail);
    emit_imm(MOp::kCmpq, 0, op_mem(kRdi, 24));
    emit_jmp(MOp::kJne, tail);
    emit_imm(MOp::kCmpq, 0, op_mem(kRcx, 24));
    emit_jmp(MOp::kJne, tail);

    // setup：rdi = cdata + cbr*8，rcx = bdata + bb*8，xmm6 = [aik,aik]。
    emit2(MOp::kMovq, op_mem(kRdi, 16), kRdx);
    emit2(MOp::kMovq, cbr_op, kRax);
    emit2(MOp::kLeaq, op_mem(kRdx, kRax, 8), kRdi);
    emit2(MOp::kMovq, op_mem(kRcx, 16), kRdx);
    emit2(MOp::kMovq, bb_op, kRax);
    emit2(MOp::kLeaq, op_mem(kRdx, kRax, 8), kRcx);
    emit2(MOp::kMovq, aik_op, kRax);
    emit2(MOp::kMovq, kRax, kXmm6);
    emit2(MOp::kMovlhps, kXmm6, kXmm6);

    // 向量循环体（每轮 2 元素）。prod = aik*b 精确重构 imulq 低 64 位：
    //   aik*b = aik*b_lo + (aik*b_hi mod 2^32) << 32  (mod 2^64)
    label(vec);
    emit2(MOp::kMovupd, op_mem(kRdi, JREG, 8), kXmm1);    // c = [c0,c1]
    emit2(MOp::kMovupd, op_mem(kRcx, JREG, 8), kXmm0);    // b = [b0,b1]
    emit2(MOp::kMovapd, kXmm0, kXmm2);
    emit_imm(MOp::kPsrlq, 32, kXmm2);                     // b_hi（低32 = b_hi mod 2^32）
    emit2(MOp::kPmuludq, kXmm6, kXmm2);                   // aik * b_hi_lo32
    emit_imm(MOp::kPsllq, 32, kXmm2);                     // 交叉项 << 32
    emit2(MOp::kPmuludq, kXmm6, kXmm0);                   // aik * b_lo32（精确，<2^63）
    emit2(MOp::kPaddq, kXmm2, kXmm0);                     // prod（回绕 = imulq）
    emit2(MOp::kPaddq, kXmm0, kXmm1);                     // c + prod（回绕 = addq）
    emit2(MOp::kMovupd, kXmm1, op_mem(kRdi, JREG, 8));    // 写回 c（kind0 裸 i64）
    emit_imm(MOp::kAddq, 2, JREG);
    if (j_stack) emit2(MOp::kMovq, JREG, jloc);
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJl, vec);
    // 退出：j==n-1 走标量尾处理余 1；j==n 吃尽跳 cond（j 保持 n，与标量一致）。
    emit2(MOp::kCmpq, n_op, JREG);
    emit_jmp(MOp::kJl, tail);
    emit_jmp(MOp::kJmp, block_label(cond_bi));
    label(tail);                                          // 其后 gen_block 照常发标量体
    return true;
}

}  // namespace pyadamas
