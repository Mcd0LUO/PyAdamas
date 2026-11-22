// codegen_simd.cpp —— 浮点循环 SIMD 向量化原型（0.333，PYADAMAS_SIMD 门控）
//
// 设计见 docs/simd-vectorization-design.md。本轮只认一种模式：spectral_norm
// 的 eval_A reduction 热点循环（可数 for + 浮点数组逐元素 map + 乘加累加）。
// 在 gen_block 发射体块时识别 IR 指令序列，直接发 SSE2 packed double（2 lane）
// 向量体 + 标量尾循环，不碰 IR/类型/regalloc。
//
// 正确性命门：
//   * 保序提取累加（addsd 低 lane → movhlps → addsd 高 lane），total 与标量
//     逐位一致，基准输出钉住零风险（不做改变累加顺序的水平求和）。
//   * 范围守卫 i>=0 && i+n<=46341 保证 pmuludq(低32乘)/psrlq(//2)/cvtdq2pd(int32)
//     与标量 imulq/floordiv/cvtsi2sdq 逐位一致；不满足整段标量 fallback。
//   * len>=n && kind==0 && j+1<n 守卫保证 movupd 不越界、读的是裸 double；
//     不满足走标量，IndexError 等异常语义与时机不变。
//   * 向量循环零调用：list/data/不变量在循环期间不可能被改写，无 GC safepoint。
//
// 寄存器约定：scratch 只用永不提升的 GPR(rax/rcx/rdx/rsi/rdi) 与 xmm0-7；
// total 原地累加在提升寄存器(xmm8-15)或栈(用 xmm7 工作+每轮写回)；j 原地
// addq $2 在提升 GPR。不新增 spill。与标量发射机器衔接：向量前奏发完后，
// gen_block 照常发射全部原标量指令作为尾循环/fallback（见 codegen_block.cpp）。
#include "codegen_impl.h"

namespace pyadamas {

namespace {
// 范围守卫常数：i+n<=46341 → s_max<=46340 → p=s(s+1)<2^31、d=p/2+A+1<2^31。
constexpr int64_t kSimdRangeBound = 46341;
}  // namespace

// 找 vreg 的定义指令（用 AsmGen 自己的 def_block_，不依赖可能过期的 def_of）。
static const Inst* simd_def_inst(const IRFunction& f, const std::vector<int>& def_block, VReg v) {
    if (v == kNoReg || v >= static_cast<VReg>(def_block.size())) return nullptr;
    int bi = def_block[static_cast<size_t>(v)];
    if (bi < 0 || bi >= static_cast<int>(f.blocks.size())) return nullptr;
    for (const Inst& in : f.blocks[static_cast<size_t>(bi)].insts)
        if (in.dst == v) return &in;
    return nullptr;
}

bool AsmGen::try_simd_reduction(const Block& b) {
    // ── 门控与函数级前提 ──────────────────────────────────────────
    if (!simd_enabled_ || opt_ < 2 || gen_ || tag_.tags_enabled) return false;
    if (!slot_loc_.empty()) return false;                 // 只做默认寄存器分配
    if (std::string_view(target_abi().name) != "x86_64-sysv") return false;
    if (f_->has_handlers) return false;                   // setjmp 函数形态不稳
    const int body_bi = cur_block_;
    if (body_bi < 0 || body_bi >= static_cast<int>(f_->blocks.size())) return false;
    // 体块可能把循环不变常量（const.int/const.float）内联在体内（LICM 未外提
    // 时，如 spec(n) 参数形态），也可能外提到 preheader（spectral_norm 全局 N
    // 形态）。过滤掉常量定义后必须恰为 19 条核心指令；常量值另经 simd_def_inst
    // 校验（与定义位置无关）。其余非核心指令一律不匹配（保守跳过）。
    const auto& BI = b.insts;
    if (BI.size() < 19 || BI.size() > 32) return false;
    if (BI.back().op != IrOp::Jump) return false;
    std::vector<Inst> core;
    core.reserve(BI.size());
    for (const Inst& in : BI)
        if (in.op != IrOp::ConstInt && in.op != IrOp::ConstFloat)
            core.push_back(in);
    const auto& I = core;
    if (I.size() != 19) return false;                     // 精确 19 条核心体形态

    // ── 体块指令形状匹配（op + 操作数关系）────────────────────────
    auto op_is = [&](size_t k, IrOp o) { return I[k].op == o; };
    if (!op_is(0, IrOp::LoadSlot) || !op_is(1, IrOp::StoreSlot) ||
        !op_is(2, IrOp::LoadSlot) || !op_is(3, IrOp::LoadSlot) ||
        !op_is(4, IrOp::Bin) || !op_is(5, IrOp::Bin) || !op_is(6, IrOp::Bin) ||
        !op_is(7, IrOp::Bin) || !op_is(8, IrOp::Bin) || !op_is(9, IrOp::Bin) ||
        !op_is(10, IrOp::Convert) || !op_is(11, IrOp::Bin) || !op_is(12, IrOp::Mov) ||
        !op_is(13, IrOp::LoadSlot) ||
        (I[14].op != IrOp::IndexLoad && I[14].op != IrOp::IndexLoadUnchecked) ||
        !op_is(15, IrOp::Bin) || !op_is(16, IrOp::Bin) ||
        !op_is(17, IrOp::StoreSlot) || !op_is(18, IrOp::Jump))
        return false;

    const VReg j1 = I[0].dst, tot = I[2].dst, j2 = I[3].dst;
    const VReg s = I[4].dst, s1 = I[5].dst, p = I[6].dst, p2 = I[7].dst;
    const VReg di = I[8].dst, d = I[9].dst, df = I[10].dst, a = I[11].dst, a2 = I[12].dst;
    const VReg j3 = I[13].dst, vj = I[14].dst, t = I[15].dst, tot2 = I[16].dst;
    const int sJ = I[0].idx, sC = I[1].idx, sT = I[2].idx;
    if (j1 == kNoReg || sC == sJ) return false;
    if (I[1].a != j1 || I[3].idx != sC || I[13].idx != sC) return false;
    if (I[17].idx != sT || I[17].a != tot2) return false;
    if (!ir_is_float(ty_of(tot)) || !ir_is_float(ty_of(vj)) ||
        !ir_is_float(ty_of(t)) || !ir_is_float(ty_of(tot2)) || !ir_is_float(ty_of(df)) ||
        !ir_is_float(ty_of(a)) || !ir_is_float(ty_of(a2))) return false;

    // s = A + B：之一是 j2，另一个是不变量 i。A = 第一操作数（eval_a 第一实参）。
    const VReg A = I[4].a, B = I[4].b;
    if (I[4].bop != BinKind::Add) return false;
    bool a_is_j;
    VReg inv;
    if (A == j2 && B != j2) { a_is_j = true; inv = B; }
    else if (B == j2 && A != j2) { a_is_j = false; inv = A; }
    else return false;
    if (inv == kNoReg) return false;
    if (I[5].bop != BinKind::Add || I[5].a != s) return false;
    if (I[6].bop != BinKind::Mul || I[6].a != s || I[6].b != s1) return false;
    if (I[7].bop != BinKind::FloorDiv || I[7].a != p) return false;
    if (I[8].bop != BinKind::Add || I[8].a != p2 || I[8].b != A) return false;
    if (I[9].bop != BinKind::Add || I[9].a != di) return false;
    if (I[10].a != d || !ir_is_float(I[10].ty)) return false;
    if (I[11].bop != BinKind::Div || I[11].b != df) return false;
    if (I[12].a != a) return false;
    if (I[14].b != j3) return false;
    if (I[15].bop != BinKind::Mul || I[15].a != a2 || I[15].b != vj) return false;
    if (I[16].bop != BinKind::Add || I[16].a != tot || I[16].b != t) return false;
    const VReg list = I[14].a, fone = I[11].a;
    const VReg one1 = I[5].b, two = I[7].b, one2 = I[9].b;
    const int incr_bi = I[18].target;

    // 整数运算类型必须是 int（排除 unknown/float 混入）。
    for (VReg r : {s, s1, p, p2, di, d}) {
        TypePtr ty = ty_of(r);
        if (!ty || ty->kind != TyKind::Int) return false;
    }
    // 常量校验：one1=one2=1，two=2，fone 是有限 const.float。
    auto const_int_is = [&](VReg v, int64_t want) {
        const Inst* def = simd_def_inst(*f_, def_block_, v);
        return def && def->op == IrOp::ConstInt && def->imm == want;
    };
    if (!const_int_is(one1, 1) || !const_int_is(one2, 1) || !const_int_is(two, 2)) return false;
    const Inst* fone_def = simd_def_inst(*f_, def_block_, fone);
    if (!fone_def || fone_def->op != IrOp::ConstFloat) return false;
    { double x = fone_def->fimm; if (!(x == x) || x - x != 0.0) return false; }  // 有限

    // ── cond / incr 块形状 ────────────────────────────────────────
    if (incr_bi < 0 || incr_bi >= static_cast<int>(f_->blocks.size()) || incr_bi == body_bi)
        return false;
    const auto& INC = f_->blocks[static_cast<size_t>(incr_bi)].insts;
    if (INC.size() != 4 || INC[0].op != IrOp::LoadSlot || INC[0].idx != sJ ||
        INC[1].op != IrOp::Bin || INC[1].bop != BinKind::Add || INC[1].a != INC[0].dst ||
        INC[2].op != IrOp::StoreSlot || INC[2].idx != sJ || INC[2].a != INC[1].dst ||
        INC[3].op != IrOp::Jump)
        return false;
    if (!const_int_is(INC[1].b, 1)) return false;         // 步长必须 1
    const int cond_bi = INC[3].target;
    if (cond_bi < 0 || cond_bi >= static_cast<int>(f_->blocks.size()) ||
        cond_bi == body_bi || cond_bi == incr_bi) return false;
    const auto& C = f_->blocks[static_cast<size_t>(cond_bi)].insts;
    if (C.size() != 3 || C[0].op != IrOp::LoadSlot || C[0].idx != sJ ||
        C[1].op != IrOp::Cmp || C[1].cop != CmpKind::Lt || C[1].a != C[0].dst ||
        C[2].op != IrOp::Branch || C[2].a != C[1].dst ||
        C[2].target != body_bi)
        return false;
    const VReg n = C[1].b;
    const int end_bi = C[2].target2;
    if (end_bi < 0 || end_bi == body_bi || end_bi == cond_bi || end_bi == incr_bi) return false;

    // ── 循环三块内只允许纯算术/装载/槽写/下标读，无调用/全局写/下标写 ──
    auto loop_op_ok = [](IrOp o) {
        switch (o) {
            case IrOp::LoadSlot: case IrOp::StoreSlot: case IrOp::Bin:
            case IrOp::Cmp: case IrOp::Convert: case IrOp::Mov:
            case IrOp::Branch: case IrOp::Jump: case IrOp::IndexLoad:
            case IrOp::IndexLoadUnchecked: case IrOp::ConstInt:
            case IrOp::ConstFloat: case IrOp::LoadGlobal:
                return true;
            default: return false;
        }
    };
    for (int bi : {cond_bi, body_bi, incr_bi})
        for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts)
            if (!loop_op_ok(in.op)) return false;

    // 槽写唯一性：sJ 只被 incr[2] 写，sC 只被 body[1] 写，sT 只被 body[17] 写。
    auto slot_writers = [&](int slot) {
        int cnt = 0;
        for (int bi : {cond_bi, body_bi, incr_bi})
            for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts)
                if (in.op == IrOp::StoreSlot && in.idx == slot) ++cnt;
        return cnt;
    };
    if (slot_writers(sJ) != 1 || slot_writers(sC) != 1 || slot_writers(sT) != 1) return false;
    // 槽类型 + 不被取地址（全函数扫 SlotAddr）。
    if (sJ < 0 || sJ >= static_cast<int>(f_->slot_ty.size()) ||
        sT < 0 || sT >= static_cast<int>(f_->slot_ty.size())) return false;
    { TypePtr tj = f_->slot_ty[static_cast<size_t>(sJ)];
      if (!tj || tj->kind != TyKind::Int) return false; }
    if (!ir_is_float(f_->slot_ty[static_cast<size_t>(sT)])) return false;
    for (const auto& bb : f_->blocks)
        for (const Inst& in : bb.insts)
            if (in.op == IrOp::SlotAddr &&
                (in.idx == sJ || in.idx == sC || in.idx == sT)) return false;

    // 不变量输入定义在循环外；若是 load.slot，其槽在循环内无写。
    auto outside = [&](VReg v) {
        if (v == kNoReg || v >= static_cast<VReg>(def_block_.size())) return false;
        int db = def_block_[static_cast<size_t>(v)];
        return db >= 0 && db != cond_bi && db != body_bi && db != incr_bi;
    };
    if (!outside(inv) || !outside(n) || !outside(list) || !outside(fone)) return false;
    auto slot_stable = [&](VReg v) {
        const Inst* def = simd_def_inst(*f_, def_block_, v);
        if (!def) return false;
        if (def->op != IrOp::LoadSlot) return true;       // 全局/常量：循环无 store.global，稳定
        int sl = def->idx;
        for (int bi : {cond_bi, body_bi, incr_bi})
            for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts)
                if (in.op == IrOp::StoreSlot && in.idx == sl) return false;
        return true;
    };
    if (!slot_stable(inv) || !slot_stable(n) || !slot_stable(list)) return false;

    // ── 物理位置：j 必须提升进 GPR；total 提升 xmm 或栈 ───────────
    const MOperand& jloc = slot_mop(sJ);
    // j 提升进 GPR → 原地递增；j 在栈 → 用 scratch %rsi 持有，每轮写回栈
    // （标量尾/incr 从栈读 j，必须最新）。%rsi 永不提升，安全。
    const bool j_stack = jloc.kind != MOperandKind::Reg;
    if (!j_stack && (jloc.reg < kRax || jloc.reg > kR15)) return false;
    const MOperand& tloc = slot_mop(sT);
    const bool total_stack = tloc.kind != MOperandKind::Reg;
    if (!total_stack && (tloc.reg < kXmm8 || tloc.reg > kXmm15)) return false;
    const MOperand n_op = src_mop(n), inv_op = src_mop(inv);
    const MOperand list_op = src_mop(list), fone_op = src_mop(fone);
    // 输入操作数绝不能落在要 clobber 的 scratch 上（提升池不含这些，双保险）。
    auto scratch_safe = [](const MOperand& o) {
        if (o.kind != MOperandKind::Reg) return true;
        MReg r = o.reg;
        if (r == kRax || r == kRcx || r == kRdx || r == kRsi || r == kRdi) return false;
        if (r >= kXmm0 && r <= kXmm7) return false;
        return true;
    };
    if (!scratch_safe(n_op) || !scratch_safe(inv_op) ||
        !scratch_safe(list_op) || !scratch_safe(fone_op)) return false;

    // ── 发射向量前奏 ──────────────────────────────────────────────
    const MReg JREG = j_stack ? kRsi : jloc.reg;
    const MReg XT = total_stack ? kXmm7 : tloc.reg;
    const std::string tail = fresh_label("simdtail");
    const std::string vec = fresh_label("simdvec");
    if (std::getenv("PYADAMAS_SIMD_DBG"))
        std::fprintf(stderr, "[simd] vectorize reduction loop body bb%d (sJ=%d sT=%d a_is_j=%d)\n",
                     body_bi, sJ, sT, static_cast<int>(a_is_j));
    comment("SIMD reduction (PYADAMAS_SIMD): SSE2 2-lane, ordered accumulate");
    if (j_stack) emit2(MOp::kMovq, jloc, JREG);           // 栈 j → %rsi

    // 守卫：j+1<n、j>=0、i>=0 且 i+n<=46341、len>=n、kind==0。任一失败走标量尾。
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJge, tail);
    emit2(MOp::kTestq, JREG, JREG);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kMovq, inv_op, kRax);
    emit2(MOp::kTestq, kRax, kRax);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kAddq, n_op, kRax);
    emit_imm(MOp::kCmpq, kSimdRangeBound, kRax);
    emit_jmp(MOp::kJg, tail);
    emit2(MOp::kMovq, list_op, kRdi);
    emit2(MOp::kMovq, op_mem(kRdi, 0), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJl, tail);
    emit_imm(MOp::kCmpq, 0, op_mem(kRdi, 24));
    emit_jmp(MOp::kJne, tail);

    // setup：data 指针 + 循环不变向量（I01/IP1或I00/ONEI/ONEF）+ total 初值。
    emit2(MOp::kMovq, op_mem(kRdi, 16), kRdx);            // rdx = data
    emit2(MOp::kMovq, inv_op, kRax);                      // rax = i
    emit2(MOp::kLeaq, op_mem(kRax, 1), kRcx);             // rcx = i+1
    emit2(MOp::kMovq, kRax, kXmm4);
    emit2(MOp::kMovlhps, kXmm4, kXmm4);                   // xmm4 = I00 = [i,i]
    emit2(MOp::kMovq, kRcx, kXmm6);
    emit2(MOp::kMovlhps, kXmm6, kXmm6);                   // xmm6 = IP1 = [i+1,i+1]
    emit2(MOp::kMovapd, kXmm4, kXmm2);
    emit2(MOp::kUnpcklpd, kXmm6, kXmm2);                  // xmm2 = [i,i+1]
    emit2(MOp::kMovapd, kXmm2, kXmm4);                    // xmm4 = I01 = [i,i+1]
    if (a_is_j) {                                         // mul_a：xmm6 改放 I00=[i,i]
        emit2(MOp::kMovq, kRax, kXmm6);
        emit2(MOp::kMovlhps, kXmm6, kXmm6);
    }
    emit_imm(MOp::kMovq, 1, kRax);
    emit2(MOp::kMovq, kRax, kXmm5);
    emit2(MOp::kMovlhps, kXmm5, kXmm5);                   // xmm5 = ONEI = [1,1]
    emit2(MOp::kMovsd, fone_op, kXmm3);
    emit2(MOp::kMovlhps, kXmm3, kXmm3);                   // xmm3 = ONEF = [fone,fone]
    if (total_stack) emit2(MOp::kMovsd, tloc, kXmm7);     // 栈 total → xmm7 工作累加器

    // 向量循环体（每轮 2 元素，保序累加）。
    label(vec);
    emit2(MOp::kMovq, JREG, kXmm0);
    emit2(MOp::kMovlhps, kXmm0, kXmm0);                   // J = [j,j]
    emit2(MOp::kPaddq, kXmm4, kXmm0);                     // s = [i+j, i+j+1]
    if (a_is_j) emit2(MOp::kMovapd, kXmm0, kXmm2);        // 存 s（mul_a 要还原 A_vec）
    emit2(MOp::kMovapd, kXmm0, kXmm1);
    emit2(MOp::kPaddq, kXmm5, kXmm1);                     // s+1
    emit2(MOp::kPmuludq, kXmm1, kXmm0);                   // p = s*(s+1)（低32乘，精确）
    emit_imm(MOp::kPsrlq, 1, kXmm0);                      // p2 = p//2（p 非负且偶）
    if (!a_is_j) {
        emit2(MOp::kPaddq, kXmm6, kXmm0);                 // d = p2 + [i+1,i+1]
    } else {
        emit2(MOp::kPsubq, kXmm6, kXmm2);                 // xmm2 = s - [i,i] = [j,j+1] = A_vec
        emit2(MOp::kPaddq, kXmm2, kXmm0);                 // d = p2 + A_vec
        emit2(MOp::kPaddq, kXmm5, kXmm0);                 // d += [1,1]
    }
    {                                                     // pshufd $8, xmm0, xmm0（三操作数）
        MInstr ins; ins.op = MOp::kPshufd;
        ins.ops.push_back(op_imm(8));
        ins.ops.push_back(op_reg(kXmm0));
        ins.ops.push_back(op_reg(kXmm0));
        emit_.out.push_instr(std::move(ins));                  // 挤低 dword：[d0_lo,d1_lo]
    }
    emit2(MOp::kCvtdq2pd, kXmm0, kXmm0);                  // df = [d0,d1]（int32→double）
    emit2(MOp::kMovapd, kXmm3, kXmm1);
    emit2(MOp::kDivpd, kXmm0, kXmm1);                     // a = fone/df
    emit2(MOp::kMovupd, op_mem(kRdx, JREG, 8), kXmm0);    // v = [v[j],v[j+1]]
    emit2(MOp::kMulpd, kXmm0, kXmm1);                     // t = a*v
    emit2(MOp::kAddsd, kXmm1, XT);                        // total += t0（低 lane，保序）
    emit2(MOp::kMovhlps, kXmm1, kXmm1);                   // 高 lane → 低
    emit2(MOp::kAddsd, kXmm1, XT);                        // total += t1
    if (total_stack) emit2(MOp::kMovsd, kXmm7, tloc);     // 写回栈 total
    emit_imm(MOp::kAddq, 2, JREG);                        // j += 2
    if (j_stack) emit2(MOp::kMovq, JREG, jloc);           // 写回栈 j
    emit2(MOp::kLeaq, op_mem(JREG, 1), kRax);
    emit2(MOp::kCmpq, n_op, kRax);
    emit_jmp(MOp::kJl, vec);                              // j+1<n → 继续
    // 退出：j==n-1 走标量尾处理余 1；j==n 吃尽直接进 incr。
    emit2(MOp::kCmpq, n_op, JREG);
    emit_jmp(MOp::kJl, tail);
    emit_jmp(MOp::kJmp, block_label(incr_bi));
    label(tail);                                          // 其后 gen_block 照常发标量体
    return true;
}


// ── 0.334 共享判据与分发 ─────────────────────────────────────────────
// 各模式（int map / nbody pos / nbody pair）复用的安全判据，口径与
// try_simd_reduction 的内联检查一致。宁保守跳过，不误向量化。

bool AsmGen::simd_func_ok() const {
    if (!simd_enabled_ || opt_ < 2 || gen_ || tag_.tags_enabled) return false;
    if (!slot_loc_.empty()) return false;                 // 只做默认寄存器分配
    if (std::string_view(target_abi().name) != "x86_64-sysv") return false;
    if (f_->has_handlers) return false;                   // setjmp 函数形态不稳
    return true;
}

const Inst* AsmGen::simd_def(VReg v) const {
    return simd_def_inst(*f_, def_block_, v);
}

bool AsmGen::simd_const_int(VReg v, int64_t want) const {
    const Inst* d = simd_def(v);
    return d && d->op == IrOp::ConstInt && d->imm == want;
}

bool AsmGen::simd_core_body(const Block& b, size_t core_n, size_t max_total,
                            std::vector<Inst>& core) const {
    const auto& BI = b.insts;
    if (BI.size() < core_n || BI.size() > max_total) return false;
    if (BI.back().op != IrOp::Jump) return false;
    core.clear();
    core.reserve(BI.size());
    for (const Inst& in : BI)
        if (in.op != IrOp::ConstInt && in.op != IrOp::ConstFloat)
            core.push_back(in);
    return core.size() == core_n;
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

// while 形状：body 直接跳回 cond（没有独立的 incr 块），增量是体块尾部三条
// （load sJ / add 1 / store sJ）后面跟一条回边 jump。手写循环最常见的写法，
// 以前整段被跳过 —— for 形状才能向量化。
bool AsmGen::simd_while_shape_ok(int cond_bi, int body_bi, int& sJ,
                                 int& end_bi, VReg& n, int& jupd_pos) const {
    if (cond_bi < 0 || cond_bi >= static_cast<int>(f_->blocks.size()) || cond_bi == body_bi)
        return false;
    const auto& C = f_->blocks[static_cast<size_t>(cond_bi)].insts;
    if (C.size() != 3 || C[0].op != IrOp::LoadSlot || C[1].op != IrOp::Cmp ||
        C[1].cop != CmpKind::Lt || C[1].a != C[0].dst || C[2].op != IrOp::Branch ||
        C[2].a != C[1].dst || C[2].target != body_bi)
        return false;
    const auto& B = f_->blocks[static_cast<size_t>(body_bi)].insts;
    if (B.size() < 7) return false;                      // 体 + 增量三条 + jump
    const size_t k = B.size() - 4;
    if (B[k].op != IrOp::LoadSlot || B[k].idx != C[0].idx) return false;
    if (B[k + 1].op != IrOp::Bin || B[k + 1].bop != BinKind::Add ||
        B[k + 1].a != B[k].dst)
        return false;
    if (!simd_const_int(B[k + 1].b, 1)) return false;    // 步长必须 1
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

int AsmGen::simd_slot_writers(int slot, int cond_bi, int body_bi, int incr_bi) const {
    int cnt = 0;
    for (int bi : {cond_bi, body_bi, incr_bi})
        for (const Inst& in : f_->blocks[static_cast<size_t>(bi)].insts)
            if (in.op == IrOp::StoreSlot && in.idx == slot) ++cnt;
    return cnt;
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

bool AsmGen::try_simd_vectorize(const Block& b) {
    if (!simd_enabled_) return false;
    if (try_simd_reduction(b)) return true;
    if (try_simd_int_map(b)) return true;
    if (try_simd_nbody_pos(b)) return true;
    if (try_simd_nbody_pair(b)) return true;
    return false;
}

}  // namespace pyadamas
