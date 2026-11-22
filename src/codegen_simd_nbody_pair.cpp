// codegen_simd_nbody_pair.cpp —— nbody pairwise 力循环 SIMD（0.334，门控）
//
// 设计见 docs/simd-vectorization-design.md §10 P2。模式：nbody advance 的
// pairwise 内循环（j 从 i+1 到 5，体块恰 113 条核心指令）：
//   dx = b[i7]-b[j7]; dy = b[i7+1]-b[j7+1]; dz = b[i7+2]-b[j7+2]
//   d2 = dx*dx+dy*dy+dz*dz; mag = dt/(d2*sqrt(d2))
//   b[i7+3..5] -= (dx,dy,dz)*b[j7+6]*mag   （i 侧累加器，跨 j 串行）
//   b[j7+3..5] += (dx,dy,dz)*b[i7+6]*mag   （j 侧逐元素）
//
// 向量化轴：**分量轴 [x,y] + z 标量**（不是 j 对轴）。每轮向量迭代 = 1 个
// 标量 j 迭代（步长 1，无尾循环）：dx/dy 用 movupd+subpd 成对算，d2 水平
// 求和**保序**（movhlps+addsd 先 dx²+dy² 再 +dz²），sqrt/mag 标量
// sqrtsd/divsd（与标量 fast path 逐位一致），i 侧 acc_x/acc_y 是**独立**
// 累加器 → subpd 双 lane 各自减，无跨 lane 改序；跨 j 的串行顺序逐轮保持。
// 全部运算逐 lane IEEE 同标量 → 输出逐位一致，不改 reduction 顺序。
//
// 为什么不用 j 对轴（sqrtpd 2 体）：需要 16 个 xmm（9 不变量+7 scratch），
// advance 有 4 个 fp 提升槽占 xmm8-15，只剩 12 个，放不下；分量轴只需 11 个。
// sqrt/div 延迟未摊销是已知代价，实测收益仍显著（见设计文档 §10）。
//
// 正确性命门：
//   * i 侧累加器 b[i7+3..5] 提升到 xmm 跨轮持有，**退出/负数守卫路径先写回**
//     再进标量尾/cond，标量读到最新值。
//   * 别名结构判据：j7>=i7+7 守卫（j>i）→ i 行偏移 0..6 与 j 行偏移 0..6
//     不相交（行距 7）；i 侧写偏移 3..5 与 i 侧读偏移 0..2,6 不相交；
//     j 侧读偏移 0..2,6 与写偏移 3..5 不相交；跨迭代 j 行不相交。
//     b[i7+0..2,6] 因此循环不变，setup 一次装载。
//   * sqrt 负数守卫：d2<0 → 写回 acc → 标量尾（pa_math_sqrt 抛 ValueError
//     时机不变）。d2 是平方和理论上非负，守卫是双保险；NaN 走 sqrtsd=NaN
//     与 pa_math_sqrt(NaN) 一致（comisd 无序不跳）。
//   * 范围守卫 i7>=0、j7>=i7+7、7n<=len、i7+6<len、kind==0；不满足整段标量。
//   * 向量循环零调用（sqrt 内联 sqrtsd，负数冷路径跳标量尾才调用）。
//
// 寄存器：11 个 xmm = 5 不变量（INV_XY/ZI/MIXY/ACCXY/ACCZ）+ 6 scratch
// （DXY/DZ/MJ/T/T2/TZ）。不变量优先占**空闲** xmm8-15（扫描 ra_.slot_reg/
// vreg_reg_ 排除提升占用），不够补 xmm7 往下；绝不碰提升寄存器。
// GPR：JREG（j 提升 GPR 或 %rsi+写回）、rdi=j7、rdx=data、rcx=i7（setup
// 后常驻，退出写回用）、rax scratch。
#include "codegen_impl.h"

namespace pyadamas {


bool AsmGen::try_simd_nbody_pair(const Block& b) {
    if (!simd_func_ok()) return false;
    const int body_bi = cur_block_;
    if (body_bi < 0 || body_bi >= static_cast<int>(f_->blocks.size())) return false;
    std::vector<Inst> core;
    if (!simd_core_body(b, 113, 143, core)) return false;
    const auto& I = core;

    // ── 体块形状（113 条精确匹配）──────────────────────────────────
    auto op_is = [&](size_t k, IrOp o) { return I[k].op == o; };
    auto ld_is = [&](size_t k) {
        return I[k].op == IrOp::IndexLoad || I[k].op == IrOp::IndexLoadUnchecked;
    };
    auto st_is = [&](size_t k) {
        return I[k].op == IrOp::IndexStore || I[k].op == IrOp::IndexStoreUnchecked;
    };
    static const IrOp kShape[113] = {
        IrOp::LoadSlot, IrOp::StoreSlot, IrOp::LoadSlot, IrOp::IndexLoad,   // 0-3
        IrOp::LoadSlot, IrOp::LoadSlot, IrOp::Bin, IrOp::IndexLoad,         // 4-7
        IrOp::Bin, IrOp::StoreSlot, IrOp::LoadSlot, IrOp::IndexLoad,        // 8-11
        IrOp::LoadSlot, IrOp::Bin, IrOp::IndexLoad, IrOp::Bin,              // 12-15
        IrOp::StoreSlot, IrOp::LoadSlot, IrOp::IndexLoad, IrOp::LoadSlot,   // 16-19
        IrOp::Bin, IrOp::IndexLoad, IrOp::Bin, IrOp::StoreSlot,             // 20-23
        IrOp::LoadSlot, IrOp::LoadSlot, IrOp::Bin, IrOp::LoadSlot,          // 24-27
        IrOp::LoadSlot, IrOp::Bin, IrOp::Bin, IrOp::LoadSlot,               // 28-31
        IrOp::LoadSlot, IrOp::Bin, IrOp::Bin, IrOp::StoreSlot,              // 32-35
        IrOp::LoadSlot, IrOp::LoadSlot, IrOp::CallRt, IrOp::Bin,            // 36-39
        IrOp::Bin, IrOp::StoreSlot, IrOp::LoadSlot, IrOp::IndexLoad,        // 40-43
        IrOp::LoadSlot, IrOp::LoadSlot, IrOp::Bin, IrOp::IndexLoad,         // 44-47
        IrOp::Bin, IrOp::LoadSlot, IrOp::Bin, IrOp::Bin,                    // 48-51
        IrOp::LoadSlot, IrOp::IndexStore, IrOp::LoadSlot, IrOp::IndexLoad,  // 52-55
        IrOp::LoadSlot, IrOp::LoadSlot, IrOp::IndexLoad, IrOp::Bin,         // 56-59
        IrOp::LoadSlot, IrOp::Bin, IrOp::Bin, IrOp::LoadSlot,               // 60-63
        IrOp::IndexStore, IrOp::LoadSlot, IrOp::IndexLoad, IrOp::LoadSlot,  // 64-67
        IrOp::LoadSlot, IrOp::IndexLoad, IrOp::Bin, IrOp::LoadSlot,         // 68-71
        IrOp::Bin, IrOp::Bin, IrOp::LoadSlot, IrOp::IndexStore,             // 72-75
        IrOp::LoadSlot, IrOp::Bin, IrOp::IndexLoad, IrOp::LoadSlot,         // 76-79
        IrOp::LoadSlot, IrOp::IndexLoad, IrOp::Bin, IrOp::LoadSlot,         // 80-83
        IrOp::Bin, IrOp::Bin, IrOp::LoadSlot, IrOp::IndexStore,             // 84-87
        IrOp::LoadSlot, IrOp::Bin, IrOp::IndexLoad, IrOp::LoadSlot,         // 88-91
        IrOp::LoadSlot, IrOp::IndexLoad, IrOp::Bin, IrOp::LoadSlot,         // 92-95
        IrOp::Bin, IrOp::Bin, IrOp::LoadSlot, IrOp::IndexStore,             // 96-99
        IrOp::LoadSlot, IrOp::Bin, IrOp::IndexLoad, IrOp::LoadSlot,         // 100-103
        IrOp::LoadSlot, IrOp::IndexLoad, IrOp::Bin, IrOp::LoadSlot,         // 104-107
        IrOp::Bin, IrOp::Bin, IrOp::LoadSlot, IrOp::IndexStore,             // 108-111
        IrOp::Jump,                                                         // 112
    };
    for (int k = 0; k < 113; ++k) {
        if (kShape[k] == IrOp::IndexLoad) { if (!ld_is(static_cast<size_t>(k))) return false; }
        else if (kShape[k] == IrOp::IndexStore) { if (!st_is(static_cast<size_t>(k))) return false; }
        else if (!op_is(static_cast<size_t>(k), kShape[k])) return false;
    }

    // ── 操作数关系 ────────────────────────────────────────────────
    const VReg j1 = I[0].dst, j2 = I[5].dst, j7 = I[6].dst;
    const int sJ = I[0].idx, sJC = I[1].idx, sB = I[2].idx;
    const int sDX = I[9].idx, sDY = I[16].idx, sDZ = I[23].idx, sD2 = I[35].idx, sMAG = I[41].idx;
    if (j1 == kNoReg || sJC == sJ) return false;
    if (I[1].a != j1 || I[5].idx != sJC) return false;
    // 所有 list 装载同一槽 sB（24 处）。
    for (size_t k : {2, 4, 10, 12, 17, 19, 42, 45, 52, 54, 57, 63, 65, 68, 74, 76, 80, 86, 88, 92, 98, 100, 104, 110})
        if (I[k].idx != sB) return false;
    const VReg xi = I[3].dst, xj = I[7].dst, dx = I[8].dst;
    const VReg yi = I[11].dst, j7p1 = I[13].dst, yj = I[14].dst, dy = I[15].dst;
    const VReg zi = I[18].dst, j7p2 = I[20].dst, zj = I[21].dst, dz = I[22].dst;
    const VReg i7 = I[3].b;                                 // b[i7] 的索引（不变量）
    if (I[3].a != I[2].dst) return false;
    if (I[6].bop != BinKind::Mul || I[6].a != j2) return false;
    const VReg seven = I[6].b;
    if (I[7].a != I[4].dst || I[7].b != j7) return false;
    if (I[8].bop != BinKind::Sub || I[8].a != xi || I[8].b != xj) return false;
    if (I[9].a != dx) return false;
    if (I[11].a != I[10].dst) return false;
    const VReg i7p1 = I[11].b;
    if (I[13].bop != BinKind::Add || I[13].a != j7) return false;
    if (I[14].a != I[12].dst || I[14].b != j7p1) return false;
    if (I[15].bop != BinKind::Sub || I[15].a != yi || I[15].b != yj) return false;
    if (I[16].a != dy) return false;
    if (I[18].a != I[17].dst) return false;
    const VReg i7p2 = I[18].b;
    if (I[20].bop != BinKind::Add || I[20].a != j7) return false;
    if (I[21].a != I[19].dst || I[21].b != j7p2) return false;
    if (I[22].bop != BinKind::Sub || I[22].a != zi || I[22].b != zj) return false;
    if (I[23].a != dz) return false;
    // d2 = (dx*dx + dy*dy) + dz*dz，槽读回形状。
    if (I[24].idx != sDX || I[25].idx != sDX || I[26].bop != BinKind::Mul ||
        I[26].a != I[24].dst || I[26].b != I[25].dst) return false;
    if (I[27].idx != sDY || I[28].idx != sDY || I[29].bop != BinKind::Mul ||
        I[29].a != I[27].dst || I[29].b != I[28].dst) return false;
    if (I[30].bop != BinKind::Add || I[30].a != I[26].dst || I[30].b != I[29].dst) return false;
    if (I[31].idx != sDZ || I[32].idx != sDZ || I[33].bop != BinKind::Mul ||
        I[33].a != I[31].dst || I[33].b != I[32].dst) return false;
    if (I[34].bop != BinKind::Add || I[34].a != I[30].dst || I[34].b != I[33].dst) return false;
    if (I[35].a != I[34].dst) return false;
    // sqrt + mag。
    if (I[36].idx != sD2 || I[37].idx != sD2) return false;
    if (static_cast<RtFn>(I[38].imm) != RtFn::MathSqrt || I[38].args.size() != 1 ||
        I[38].args[0] != I[37].dst || !ir_is_float(I[38].ty)) return false;
    if (I[39].bop != BinKind::Mul || I[39].a != I[36].dst || I[39].b != I[38].dst) return false;
    if (I[40].bop != BinKind::Div || I[40].b != I[39].dst) return false;
    const VReg dt = I[40].a, mag = I[41].a;
    if (I[41].a != I[40].dst) return false;
    // i 侧三分量：acc -= (d * b[j7+6]) * mag；读/写索引 i7+3/4/5。
    const VReg j7p6 = I[46].dst;
    if (I[46].bop != BinKind::Add || I[46].a != j7) return false;
    // x 侧（base=43）自带 j7+6 索引加法（off=4 指向 mj 装载）；y/z 侧
    // （base=55/66）复用同一 j7p6 索引 vreg（off=3）。
    auto i_side = [&](size_t base, int sD, size_t off, VReg& iidx_r, VReg& iidx_w) -> bool {
        if (I[base].a != I[base - 1].dst) return false;     // list 装载前一条
        iidx_r = I[base].b;
        if (I[base + 1].idx != sD) return false;
        if (off == 4) {
            if (I[base + 3].bop != BinKind::Add || I[base + 3].a != j7) return false;
            if (I[base + 4].b != I[base + 3].dst) return false;
        } else {
            if (I[base + 3].b != j7p6) return false;
        }
        if (I[base + off].a != I[base + 2].dst) return false;
        if (I[base + off + 1].bop != BinKind::Mul || I[base + off + 1].a != I[base + 1].dst ||
            I[base + off + 1].b != I[base + off].dst) return false;
        if (I[base + off + 2].idx != sMAG) return false;
        if (I[base + off + 3].bop != BinKind::Mul || I[base + off + 3].a != I[base + off + 1].dst ||
            I[base + off + 3].b != I[base + off + 2].dst) return false;
        if (I[base + off + 4].bop != BinKind::Sub || I[base + off + 4].a != I[base].dst ||
            I[base + off + 4].b != I[base + off + 3].dst) return false;
        if (I[base + off + 6].a != I[base + off + 5].dst ||
            I[base + off + 6].c != I[base + off + 4].dst) return false;
        iidx_w = I[base + off + 6].b;
        return true;
    };
    VReg i7p3r, i7p3w, i7p4r, i7p4w, i7p5r, i7p5w;
    if (!i_side(43, sDX, 4, i7p3r, i7p3w)) return false;
    if (!i_side(55, sDY, 3, i7p4r, i7p4w)) return false;
    if (!i_side(66, sDZ, 3, i7p5r, i7p5w)) return false;
    // j 侧三分量：b[j7+3..5] += (d * b[i7+6]) * mag；读/写共享索引 vreg。
    auto j_side = [&](size_t base, int sD, VReg& jidx, VReg& iidx6) -> bool {
        if (I[base].bop != BinKind::Add || I[base].a != j7) return false;
        jidx = I[base].dst;
        if (I[base + 1].a != I[base - 1].dst || I[base + 1].b != jidx) return false;
        if (I[base + 2].idx != sD) return false;
        if (I[base + 4].a != I[base + 3].dst) return false;
        iidx6 = I[base + 4].b;
        if (I[base + 5].bop != BinKind::Mul || I[base + 5].a != I[base + 2].dst ||
            I[base + 5].b != I[base + 4].dst) return false;
        if (I[base + 6].idx != sMAG) return false;
        if (I[base + 7].bop != BinKind::Mul || I[base + 7].a != I[base + 5].dst ||
            I[base + 7].b != I[base + 6].dst) return false;
        if (I[base + 8].bop != BinKind::Add || I[base + 8].a != I[base + 1].dst ||
            I[base + 8].b != I[base + 7].dst) return false;
        if (I[base + 10].a != I[base + 9].dst || I[base + 10].b != jidx ||
            I[base + 10].c != I[base + 8].dst) return false;
        return true;
    };
    VReg j7p3, i7p6a, j7p4, i7p6b, j7p5, i7p6c;
    if (!j_side(77, sDX, j7p3, i7p6a)) return false;
    if (!j_side(89, sDY, j7p4, i7p6b)) return false;
    if (!j_side(101, sDZ, j7p5, i7p6c)) return false;
    // 常量：j 侧偏移 3/4/5，j7+6 的 6，j7+1/+2 的 1/2，乘数 7。
    if (!simd_const_int(I[77].b, 3) || !simd_const_int(I[89].b, 4) ||
        !simd_const_int(I[101].b, 5) || !simd_const_int(I[46].b, 6) ||
        !simd_const_int(I[13].b, 1) || !simd_const_int(I[20].b, 2) ||
        !simd_const_int(seven, 7)) return false;

    // ── 不变量结构：i7 = load.slot(sI)*7；i7+k = bin.add(i7, const k) ──
    const Inst* i7d = simd_def(i7);
    if (!i7d || i7d->op != IrOp::Bin || i7d->bop != BinKind::Mul) return false;
    const Inst* i_src = simd_def(i7d->a);
    if (!i_src || i_src->op != IrOp::LoadSlot || !simd_const_int(i7d->b, 7)) return false;
    const int sI = i_src->idx;
    auto idx_add_is = [&](VReg v, int64_t k) {
        const Inst* d = simd_def(v);
        return d && d->op == IrOp::Bin && d->bop == BinKind::Add &&
               d->a == i7 && simd_const_int(d->b, k);
    };
    if (!idx_add_is(i7p1, 1) || !idx_add_is(i7p2, 2) ||
        !idx_add_is(i7p3r, 3) || !idx_add_is(i7p3w, 3) ||
        !idx_add_is(i7p4r, 4) || !idx_add_is(i7p4w, 4) ||
        !idx_add_is(i7p5r, 5) || !idx_add_is(i7p5w, 5) ||
        !idx_add_is(i7p6a, 6) || !idx_add_is(i7p6b, 6) || !idx_add_is(i7p6c, 6))
        return false;
    // dt 来自稳定槽。
    const Inst* dtd = simd_def(dt);
    if (!dtd || dtd->op != IrOp::LoadSlot) return false;
    const int sDT = dtd->idx;

    // ── 类型 ──────────────────────────────────────────────────────
    for (VReg r : {xi, xj, dx, yi, yj, dy, zi, zj, dz, I[34].dst, I[38].dst, mag,
                   I[43].dst, I[55].dst, I[66].dst, I[47].dst, I[81].dst,
                   I[78].dst, I[90].dst, I[102].dst})
        if (!ir_is_float(ty_of(r))) return false;
    for (VReg r : {j7, i7}) {
        TypePtr ty = ty_of(r);
        if (!ty || ty->kind != TyKind::Int) return false;
    }
    const int incr_bi = I[112].target;

    // ── cond / incr / 白名单 / 槽纪律 ──────────────────────────────
    int cond_bi = -1, end_bi = -1;
    VReg n = kNoReg;
    if (!simd_incr_cond_ok(incr_bi, body_bi, sJ, cond_bi, end_bi, n)) return false;
    if (!simd_loop_ops_ok(cond_bi, body_bi, incr_bi, true, true)) return false;
    if (simd_slot_writers(sJ, cond_bi, body_bi, incr_bi) != 1) return false;
    for (int s : {sJC, sDX, sDY, sDZ, sD2, sMAG})
        if (simd_slot_writers(s, cond_bi, body_bi, incr_bi) != 1) return false;
    for (int s : {sB, sDT, sI})
        if (simd_slot_writers(s, cond_bi, body_bi, incr_bi) != 0) return false;
    if (sJ < 0 || sJ >= static_cast<int>(f_->slot_ty.size())) return false;
    { TypePtr tj = f_->slot_ty[static_cast<size_t>(sJ)];
      if (!tj || tj->kind != TyKind::Int) return false; }
    for (int s : {sJ, sJC, sDX, sDY, sDZ, sD2, sMAG, sB, sDT, sI})
        if (!simd_slot_unaddressed(s)) return false;
    if (!simd_outside(n, cond_bi, body_bi, incr_bi) ||
        !simd_slot_stable(n, cond_bi, body_bi, incr_bi)) return false;
    if (!simd_outside(i7, cond_bi, body_bi, incr_bi) ||
        !simd_outside(dt, cond_bi, body_bi, incr_bi)) return false;

    // ── 物理位置 + 空闲 xmm 发现 ───────────────────────────────────
    const MOperand& jloc = slot_mop(sJ);
    const bool j_stack = jloc.kind != MOperandKind::Reg;
    if (!j_stack && (jloc.reg < kRax || jloc.reg > kR15)) return false;
    const MOperand b_op = slot_mop(sB), dt_op = slot_mop(sDT);
    const MOperand i7_op = src_mop(i7), n_op = src_mop(n);
    if (!simd_scratch_safe(b_op) || !simd_scratch_safe(dt_op) ||
        !simd_scratch_safe(i7_op) || !simd_scratch_safe(n_op)) return false;
    // xmm8-15 中被提升槽/跨块 vreg 占用的不能用（标量机器依赖它们）。
    std::vector<MReg> pool;
    for (int r = kXmm8; r <= kXmm15; ++r) {
        bool used = false;
        for (const auto& op : ra_.slot_reg)
            if (op.kind == MOperandKind::Reg && op.reg == r) used = true;
        for (const auto& op : vreg_reg_)
            if (op.kind == MOperandKind::Reg && op.reg == r) used = true;
        if (!used) pool.push_back(static_cast<MReg>(r));
    }
    for (int r = kXmm7; r >= kXmm0; --r) pool.push_back(static_cast<MReg>(r));
    if (pool.size() < 11) return false;                     // 保守：寄存器不够跳过
    const MReg INV_XY = pool[0], ZI = pool[1], MIXY = pool[2], ACCXY = pool[3], ACCZ = pool[4];
    const MReg DXY = pool[5], DZ = pool[6], MJ = pool[7], T = pool[8], T2 = pool[9], TZ = pool[10];

    // ── 发射向量前奏 ──────────────────────────────────────────────
    const MReg JREG = j_stack ? kRsi : jloc.reg;
    const std::string tail = fresh_label("simdtail");
    const std::string vec = fresh_label("simdvec");
    const std::string sqneg = fresh_label("simdsqrtneg");
    if (std::getenv("PYADAMAS_SIMD_DBG"))
        std::fprintf(stderr, "[simd] vectorize nbody pair loop body bb%d (sJ=%d sB=%d)\n",
                     body_bi, sJ, sB);
    comment("SIMD nbody pair (PYADAMAS_SIMD): SSE2 [x,y]+z, ordered d2/sqrt, acc in regs");
    if (j_stack) emit2(MOp::kMovq, jloc, JREG);
    emit2(MOp::kMovq, JREG, kRdi);
    emit_imm(MOp::kImulq, 7, kRdi);                       // rdi = j7 = j*7

    // 守卫：i7>=0、j7>=i7+7（j>i，行不相交）、7n<=len、i7+6<len、kind==0。
    emit2(MOp::kMovq, i7_op, kRax);
    emit2(MOp::kTestq, kRax, kRax);
    emit_jmp(MOp::kJs, tail);
    emit_imm(MOp::kAddq, 7, kRax);
    emit2(MOp::kCmpq, kRax, kRdi);
    emit_jmp(MOp::kJl, tail);
    emit2(MOp::kMovq, n_op, kRax);
    emit_imm(MOp::kImulq, 7, kRax);
    emit_jmp(MOp::kJs, tail);
    emit2(MOp::kMovq, b_op, kRcx);
    emit2(MOp::kCmpq, op_mem(kRcx, 0), kRax);
    emit_jmp(MOp::kJg, tail);
    emit2(MOp::kMovq, i7_op, kRax);
    emit_imm(MOp::kAddq, 6, kRax);
    emit2(MOp::kCmpq, op_mem(kRcx, 0), kRax);
    emit_jmp(MOp::kJge, tail);
    emit_imm(MOp::kCmpq, 0, op_mem(kRcx, 24));
    emit_jmp(MOp::kJne, tail);

    // setup：rdx=data，rcx=i7（常驻到退出写回），不变量装载。
    emit2(MOp::kMovq, op_mem(kRcx, 16), kRdx);
    emit2(MOp::kMovq, i7_op, kRcx);
    emit2(MOp::kMovupd, op_mem(kRdx, kRcx, 8, 0), INV_XY);   // [xi,yi]
    emit2(MOp::kMovsd, op_mem(kRdx, kRcx, 8, 16), ZI);       // zi
    emit2(MOp::kMovsd, op_mem(kRdx, kRcx, 8, 48), MIXY);     // mi
    emit2(MOp::kMovlhps, MIXY, MIXY);                        // [mi,mi]
    emit2(MOp::kMovupd, op_mem(kRdx, kRcx, 8, 24), ACCXY);   // [acc_x,acc_y]
    emit2(MOp::kMovsd, op_mem(kRdx, kRcx, 8, 40), ACCZ);     // acc_z

    // 向量循环体（每轮 1 个 j 迭代，步长 1，无尾循环）。
    label(vec);
    emit2(MOp::kMovupd, op_mem(kRdx, kRdi, 8, 0), T);        // [xj,yj]
    emit2(MOp::kMovapd, INV_XY, DXY);
    emit2(MOp::kSubpd, T, DXY);                              // [dx,dy]
    emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, 16), T);        // zj
    emit2(MOp::kMovapd, ZI, DZ);
    emit2(MOp::kSubsd, T, DZ);                               // dz
    emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, 48), MJ);       // mj
    // d2 = (dx²+dy²)+dz²（保序水平求和）
    emit2(MOp::kMovapd, DXY, T);
    emit2(MOp::kMulpd, T, T);                                // [dx²,dy²]
    emit2(MOp::kMovhlps, T, T2);
    emit2(MOp::kAddsd, T2, T);                               // dx²+dy²
    emit2(MOp::kMovapd, DZ, T2);
    emit2(MOp::kMulsd, T2, T2);                              // dz²
    emit2(MOp::kAddsd, T2, T);                               // d2
    // sqrt 负数守卫（双保险）：d2<0 → 写回 acc → 标量尾抛 ValueError。
    emit2(MOp::kPxor, T2, T2);
    emit2(MOp::kComisd, T, T2);
    emit_jmp(MOp::kJa, sqneg);
    emit2(MOp::kSqrtsd, T, T2);                              // sqrt(d2)
    emit2(MOp::kMulsd, T2, T);                               // d2*sqrt
    emit2(MOp::kMovsd, dt_op, T2);
    emit2(MOp::kDivsd, T, T2);                               // mag = dt/(d2*sqrt)
    // i 侧 z：acc_z -= (dz*mj)*mag
    emit2(MOp::kMovapd, DZ, TZ);
    emit2(MOp::kMulsd, MJ, TZ);
    emit2(MOp::kMulsd, T2, TZ);
    emit2(MOp::kSubsd, TZ, ACCZ);
    // 广播 mag/mj，i 侧 xy：[acc_x,acc_y] -= [(dx*mj)*mag,(dy*mj)*mag]
    emit2(MOp::kMovapd, T2, T);
    emit2(MOp::kMovlhps, T, T);                              // MAGXY
    emit2(MOp::kMovlhps, MJ, MJ);                            // MJXY
    emit2(MOp::kMovapd, DXY, T2);
    emit2(MOp::kMulpd, MJ, T2);
    emit2(MOp::kMulpd, T, T2);
    emit2(MOp::kSubpd, T2, ACCXY);
    // j 侧 xy：b[j7+3..4] += [(dx*mi)*mag,(dy*mi)*mag]
    emit2(MOp::kMovupd, op_mem(kRdx, kRdi, 8, 24), T2);      // old
    emit2(MOp::kMovapd, DXY, MJ);
    emit2(MOp::kMulpd, MIXY, MJ);
    emit2(MOp::kMulpd, T, MJ);
    emit2(MOp::kAddpd, MJ, T2);
    emit2(MOp::kMovupd, T2, op_mem(kRdx, kRdi, 8, 24));
    // j 侧 z：b[j7+5] += (dz*mi)*mag
    emit2(MOp::kMovsd, op_mem(kRdx, kRdi, 8, 40), T2);
    emit2(MOp::kMovapd, DZ, TZ);
    emit2(MOp::kMulsd, MIXY, TZ);
    emit2(MOp::kMulsd, T, TZ);
    emit2(MOp::kAddsd, T2, TZ);
    emit2(MOp::kMovsd, TZ, op_mem(kRdx, kRdi, 8, 40));
    // 控制：j7+=7，j+=1（步长 1，与标量同迭代数）。
    emit_imm(MOp::kAddq, 7, kRdi);
    emit_imm(MOp::kAddq, 1, JREG);
    if (j_stack) emit2(MOp::kMovq, JREG, jloc);
    emit2(MOp::kCmpq, n_op, JREG);
    emit_jmp(MOp::kJl, vec);
    // 正常退出（j==n）：写回 i 侧累加器，跳 cond（与标量出口状态一致）。
    emit2(MOp::kMovupd, ACCXY, op_mem(kRdx, kRcx, 8, 24));
    emit2(MOp::kMovsd, ACCZ, op_mem(kRdx, kRcx, 8, 40));
    emit_jmp(MOp::kJmp, block_label(cond_bi));
    // sqrt 负数冷路径：本轮尚未写任何内存，写回前几轮 acc 后走标量尾。
    label(sqneg);
    emit2(MOp::kMovupd, ACCXY, op_mem(kRdx, kRcx, 8, 24));
    emit2(MOp::kMovsd, ACCZ, op_mem(kRdx, kRcx, 8, 40));
    emit_jmp(MOp::kJmp, tail);
    label(tail);                                          // 其后 gen_block 照常发标量体
    return true;
}

}  // namespace pyadamas
