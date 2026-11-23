// codegen_masked.cpp —— masked divergence 向量化（外层独立迭代 + 内层数据依赖早退）
//
// 形状（纯 IR 结构，无基准名/形状特判）：
//   for iv in [0, size):                 # 外层计数循环，各迭代独立
//       cr = f(iv)                       # 前奏：从外层 IV 算出的 float（可含 Convert）
//       zr = 0.0; zi = 0.0; n = 0        # 内层状态初值
//       while n < maxit and zr*zr + zi*zi <= K:
//           t = zr*zr - zi*zi + cr
//           zi = 2.0*zr*zi + ci
//           zr = t
//           n += 1
//       total += n                       # 归约
//
// 2-lane packed：两 lane 是两个相邻外层迭代（iv, iv+1）。每个 lane 的收敛条件用
// packed 比较更新 active 掩码；收敛 lane 的计数不再增加（psubq 掩码增量），
// 状态继续算但结果不被使用（packed 逐 lane 独立，互不影响）；两 lane 都收敛
// 时 movmskpd==0 提前退出。尾迭代（size 奇数/守卫失败）走原标量路径。
//
// 正确性要点（与标量逐位一致）：
//   * 内层条件与体里的乘积按同一操作数、同一顺序重算（zr*zr、zi*zi 各算两次，
//     同一输入 → 同一舍入结果；t 先减后加、zi 先 2.0*zr 再乘 zi 再 +ci）。
//   * 计数：act 为 -1 的 lane 每轮 +1（psubq），0 的 lane 不变。
//   * 外层归约 total += n 是整数加法，packed 抽取两个 lane 计数后分别相加。
#include "codegen_impl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace pyadamas {

namespace {

inline bool is_int_ty(const TypePtr& t) { return t && t->kind == TyKind::Int; }
inline bool is_float_ty(const TypePtr& t) { return t && t->kind == TyKind::Float; }

}  // namespace

bool AsmGen::try_masked_outer_loop(const Block& b) {
    if (!vect_func_ok()) return false;
    const bool dbg = std::getenv("PYADAMAS_MASKED_DBG") != nullptr;
    const int body_bi = cur_block_;
    if (dbg) std::fprintf(stderr, "[masked] enter body_bb=%d\n", body_bi);
    if (body_bi < 0 || body_bi >= static_cast<int>(f_->blocks.size())) return false;
    if (b.insts.size() < 4 || b.insts.back().op != IrOp::Jump) return false;
    const auto& blocks = f_->blocks;
    int iv_alias = -1;  // 外层 IV 的拷贝槽（prelude: store alias = load iv）

    // vreg 非 SSA：同号 vreg 可能在别的块被重新定义。只有全函数唯一一次定义才可
    // 用 simd_def，否则可能拿到别的块的定义（静默算错）。
    auto def = [&](VReg v) -> const Inst* {
        if (v == kNoReg) return nullptr;
        const Inst* found = nullptr;
        for (const auto& b2 : blocks)
            for (const auto& in : b2.insts)
                if (in.dst == v) { if (found) return nullptr; found = &in; }
        return found ? found : simd_def(v);
    };
    auto def_block = [&](VReg v) -> int {
        int found = -2;
        for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
            for (const auto& in : blocks[static_cast<size_t>(i)].insts)
                if (in.dst == v) {
                    if (found >= 0 && found != i) return -1;
                    found = i;
                }
        }
        return found;
    };
    auto blk = [&](int i) -> const std::vector<Inst>& {
        return blocks[static_cast<size_t>(i)].insts;
    };
    auto ty = [&](VReg v) { return v == kNoReg ? TypePtr() : ty_of(v); };
    auto sl_ty = [&](int slot) -> TypePtr {
        return (slot >= 0 && slot < static_cast<int>(f_->slot_ty.size()))
                   ? f_->slot_ty[static_cast<size_t>(slot)] : TypePtr();
    };

    // ── 1. 外层 cond：唯一一条 Branch 以 body_bi 为真分支 ───────────────
    int bc = -1;
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
        const auto& ins = blk(i);
        if (ins.empty() || ins.back().op != IrOp::Branch) continue;
        if (ins.back().target == body_bi) {
            if (bc >= 0) return false;  // 多个真前驱 → 拒绝
            bc = i;
        }
    }
    if (dbg) std::fprintf(stderr, "[masked] bc=%d\n", bc);
    if (bc < 0) return false;
    const auto& C = blk(bc);
    if (C.size() != 3 || C[0].op != IrOp::LoadSlot || C[1].op != IrOp::Cmp ||
        C[1].cop != CmpKind::Lt || C[1].a != C[0].dst || C[2].op != IrOp::Branch ||
        C[2].a != C[1].dst || C[2].target != body_bi)
        return false;
    const int iv_slot = C[0].idx;
    const VReg outer_bound = C[1].b;
    if (dbg) std::fprintf(stderr, "[masked] outer cond ok iv=%d bound=%u\n", iv_slot, outer_bound);
    if (!is_int_ty(sl_ty(iv_slot)) || !is_int_ty(ty(outer_bound))) return false;

    // ── 2. 内层 while：body 末尾 jump 到 cond1（n < maxit）───────────────
    const int t0 = b.insts.back().target;
    if (t0 < 0 || t0 >= static_cast<int>(blocks.size()) || t0 == body_bi) return false;
    const auto& C1 = blk(t0);
    if (C1.size() != 3 || C1[0].op != IrOp::LoadSlot || C1[1].op != IrOp::Cmp ||
        C1[1].cop != CmpKind::Lt || C1[1].a != C1[0].dst || C1[2].op != IrOp::Branch ||
        C1[2].a != C1[1].dst)
        return false;
    const int n_slot = C1[0].idx;
    const VReg maxit_v = C1[1].b;
    const int c2 = C1[2].target;      // 条件为真 → mag 条件块
    const int be = C1[2].target2;     // 条件为假 → 退出块
    if (!is_int_ty(sl_ty(n_slot)) || !is_int_ty(ty(maxit_v))) return false;
    if (dbg) std::fprintf(stderr, "[masked] c1 ok n=%d c2=%d be=%d\n", n_slot, c2, be);
    if (c2 < 0 || be < 0 || c2 == be) return false;

    // cond2：load zr,load zr,mul, load zi,load zi,mul, add, cmp.le const, branch
    const auto& C2 = blk(c2);
    if (C2.size() != 9) return false;
    if (C2[0].op != IrOp::LoadSlot || C2[1].op != IrOp::LoadSlot ||
        C2[2].op != IrOp::Bin || C2[2].bop != BinKind::Mul ||
        C2[3].op != IrOp::LoadSlot || C2[4].op != IrOp::LoadSlot ||
        C2[5].op != IrOp::Bin || C2[5].bop != BinKind::Mul ||
        C2[6].op != IrOp::Bin || C2[6].bop != BinKind::Add ||
        C2[7].op != IrOp::Cmp || C2[7].cop != CmpKind::Le ||
        C2[8].op != IrOp::Branch)
        return false;
    const int zr_slot = C2[0].idx;
    const int zi_slot = C2[3].idx;
    if (C2[1].idx != zr_slot || C2[4].idx != zi_slot) return false;
    if (C2[2].a != C2[0].dst || C2[2].b != C2[1].dst) return false;
    if (C2[5].a != C2[3].dst || C2[5].b != C2[4].dst) return false;
    if (C2[6].a != C2[2].dst || C2[6].b != C2[5].dst) return false;
    if (C2[7].a != C2[6].dst) return false;
    const VReg lim_v = C2[7].b;
    const Inst* lim_def = def(lim_v);
    if (!lim_def || lim_def->op != IrOp::ConstFloat) return false;
    const int bw = C2[8].target;
    if (C2[8].target2 != be) return false;
    if (dbg) std::fprintf(stderr, "[masked] c2 ok bw=%d\n", bw);
    if (bw < 0 || bw >= static_cast<int>(blocks.size())) return false;
    if (!is_float_ty(sl_ty(zr_slot)) || !is_float_ty(sl_ty(zi_slot))) return false;

    // ── 3. 内层体：t=zr*zr-zi*zi+cr; zi=2*zr*zi+ci; zr=t; n+=1 ──────────
    const auto& BW = blk(bw);
    if (dbg) {
        std::fprintf(stderr, "[masked] BW size=%zu ops:", BW.size());
        for (const auto& x : BW) std::fprintf(stderr, " %d", static_cast<int>(x.op));
        std::fprintf(stderr, "\n");
    }
    if (BW.size() != 15 || BW.back().op != IrOp::Jump || BW.back().target != t0)
        return false;
    // [0] sub(prod1,prod2)  [1] add(t, cr_load)  [2] store temp
    if (BW[0].op != IrOp::Bin || BW[0].bop != BinKind::Sub || BW[0].a != C2[2].dst ||
        BW[0].b != C2[5].dst)
        return false;
    if (BW[1].op != IrOp::Bin || BW[1].bop != BinKind::Add || BW[1].a != BW[0].dst ||
        BW[2].op != IrOp::StoreSlot || BW[2].a != BW[1].dst)
        return false;
    const int temp_slot = BW[2].idx;
    if (!is_float_ty(sl_ty(temp_slot))) return false;
    const Inst* cr_def = def(BW[1].b);
    if (!cr_def || cr_def->op != IrOp::LoadSlot) return false;
    const int cr_slot = cr_def->idx;
    if (!is_float_ty(sl_ty(cr_slot))) return false;
    // [3] load zr [4] mul const2 [5] load zi [6] mul [7] add ci_load [8] store zi
    if (BW[3].op != IrOp::LoadSlot || BW[3].idx != zr_slot ||
        BW[4].op != IrOp::Bin || BW[4].bop != BinKind::Mul ||
        BW[5].op != IrOp::LoadSlot || BW[5].idx != zi_slot ||
        BW[6].op != IrOp::Bin || BW[6].bop != BinKind::Mul ||
        BW[7].op != IrOp::Bin || BW[7].bop != BinKind::Add ||
        BW[8].op != IrOp::StoreSlot || BW[8].idx != zi_slot)
        return false;
    const Inst* two_def = def(BW[4].a);
    if (!two_def || two_def->op != IrOp::ConstFloat) return false;
    if (BW[4].b != BW[3].dst || BW[6].a != BW[4].dst || BW[6].b != BW[5].dst ||
        BW[7].a != BW[6].dst || BW[8].a != BW[7].dst)
        return false;
    const Inst* ci_def = def(BW[7].b);
    if (!ci_def || ci_def->op != IrOp::LoadSlot) return false;
    const int ci_slot = ci_def->idx;
    if (!is_float_ty(sl_ty(ci_slot)) || ci_slot == cr_slot) return false;
    // [9] load temp [10] store zr
    if (BW[9].op != IrOp::LoadSlot || BW[9].idx != temp_slot ||
        BW[10].op != IrOp::StoreSlot || BW[10].idx != zr_slot || BW[10].a != BW[9].dst)
        return false;
    // [11] load n [12] add const1 [13] store n
    if (BW[11].op != IrOp::LoadSlot || BW[11].idx != n_slot ||
        BW[12].op != IrOp::Bin || BW[12].bop != BinKind::Add ||
        BW[12].a != BW[11].dst || BW[13].op != IrOp::StoreSlot ||
        BW[13].idx != n_slot || BW[13].a != BW[12].dst)
        return false;
    const Inst* one_def = def(BW[12].b);
    if (dbg) std::fprintf(stderr, "[masked] BW pattern done\n");
    if (!one_def || one_def->op != IrOp::ConstInt || one_def->imm != 1) return false;
    // 内层体里 ci/cr 槽不能被写（循环不变量）。
    for (const auto& in : BW)
        if (in.op == IrOp::StoreSlot && (in.idx == ci_slot || in.idx == cr_slot))
            return false;

    // ── 4. 退出块：total += n; jump 外层 incr ───────────────────────────
    const auto& BE = blk(be);
    if (BE.size() != 5 || BE[0].op != IrOp::LoadSlot || BE[1].op != IrOp::LoadSlot ||
        BE[1].idx != n_slot || BE[2].op != IrOp::Bin || BE[2].bop != BinKind::Add ||
        BE[2].a != BE[0].dst || BE[2].b != BE[1].dst || BE[3].op != IrOp::StoreSlot ||
        BE[3].a != BE[2].dst || BE[4].op != IrOp::Jump)
        return false;
    const int total_slot = BE[0].idx;
    if (!is_int_ty(sl_ty(total_slot))) return false;
    const int bi = BE[4].target;
    if (bi < 0 || bi >= static_cast<int>(blocks.size())) return false;
    // 外层 incr：iv += 1; jump cond
    const auto& BI = blk(bi);
    if (BI.size() != 4 || BI[0].op != IrOp::LoadSlot || BI[0].idx != iv_slot ||
        BI[1].op != IrOp::Bin || BI[1].bop != BinKind::Add || BI[1].a != BI[0].dst ||
        BI[2].op != IrOp::StoreSlot || BI[2].idx != iv_slot || BI[2].a != BI[1].dst ||
        BI[3].op != IrOp::Jump || BI[3].target != bc)
        return false;
    const Inst* iv_one = def(BI[1].b);
    if (!iv_one || iv_one->op != IrOp::ConstInt || iv_one->imm != 1) return false;

    // ── 5. body 前奏：cr = f(iv)（float DAG），状态初值 0.0/0.0/0 ───────
    const size_t np = b.insts.size() - 1;
    int cr_store = -1;
    // 外层 IV 的拷贝槽：prelude 里 store alias = load iv_slot。evaluator 把该槽
    // 当作 lane 值（cvtsi2sdq lane）而不是读内存里的旧值。
    for (size_t i = 0; i < np; ++i) {
        const auto& in = b.insts[i];
        if (in.op != IrOp::StoreSlot || in.idx == iv_slot) continue;
        const Inst* vd = def(in.a);
        if (vd && vd->op == IrOp::LoadSlot && vd->idx == iv_slot) iv_alias = in.idx;
    }
    if (dbg) std::fprintf(stderr, "[masked] prelude np=%zu iv_alias=%d cr=%d zr=%d zi=%d n=%d\n", np, iv_alias, cr_slot, zr_slot, zi_slot, n_slot);
    for (size_t i = 0; i < np; ++i) {
        const auto& in = b.insts[i];
        switch (in.op) {
            case IrOp::LoadSlot: case IrOp::ConstInt: case IrOp::ConstFloat:
            case IrOp::Bin: case IrOp::Convert: case IrOp::Mov:
                break;
            case IrOp::StoreSlot:
                if (in.idx == cr_slot) {
                    if (cr_store >= 0) return false;
                    cr_store = static_cast<int>(i);
                } else if (in.idx == iv_alias) {
                    // 外层 IV 的拷贝槽：允许（evaluator 按 lane 值处理）。
                } else if (in.idx == zr_slot || in.idx == zi_slot) {
                    const Inst* vd = def(in.a);
                    if (!vd || vd->op != IrOp::ConstFloat || vd->fimm != 0.0) return false;
                } else if (in.idx == n_slot) {
                    const Inst* vd = def(in.a);
                    if (!vd || vd->op != IrOp::ConstInt || vd->imm != 0) return false;
                } else {
                    if (dbg) std::fprintf(stderr, "[masked] prelude reject store slot=%d op=%d\n", static_cast<int>(in.idx), static_cast<int>(in.op));
                    return false;  // 其它槽写：拒绝（窄形状）
                }
                break;
            default: return false;
        }
    }
    if (dbg) std::fprintf(stderr, "[masked] pattern ok cr_store=%d\n", cr_store);
    if (cr_store < 0) return false;
    const VReg cr_val = b.insts[static_cast<size_t>(cr_store)].a;

    // ── 6. 前驱唯一性（内层块只能从期望位置进入，防止共享块）───────────
    auto preds = [&](int target) {
        std::vector<int> v;
        for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
            const auto& ins = blk(i);
            if (ins.empty()) continue;
            const auto& last = ins.back();
            if (last.op != IrOp::Jump && last.op != IrOp::Branch) continue;
            if (last.target == target || last.target2 == target) v.push_back(i);
        }
        return v;
    };
    auto pt0 = preds(t0);
    auto pc2 = preds(c2);
    auto pbw = preds(bw);
    bool preds_ok = pt0.size() == 2 && pt0[0] == body_bi && pt0[1] == bw &&
                    pc2.size() == 1 && pc2[0] == t0 && pbw.size() == 1 && pbw[0] == c2;
    if (dbg) {
        std::fprintf(stderr, "[masked] preds t0:");
        for (int x : pt0) std::fprintf(stderr, " %d", x);
        std::fprintf(stderr, " | c2:");
        for (int x : pc2) std::fprintf(stderr, " %d", x);
        std::fprintf(stderr, " | bw:");
        for (int x : pbw) std::fprintf(stderr, " %d", x);
        std::fprintf(stderr, " body=%d\n", body_bi);
    }
    if (!preds_ok) return false;

    // ── 7. 发射前准备：常量/不变量/守卫的可行性 ─────────────────────────
    // 常量：内层比较上限 K 与 2.0 各占一个高位 xmm（提升槽之外的空闲寄存器）。
    auto pick_hi = [&](MReg avoid) -> MReg {
        for (MReg r : {kXmm15, kXmm14, kXmm13, kXmm12, kXmm11, kXmm10, kXmm9, kXmm8})
            if (r != avoid && xmm_high_free(r)) return r;
        return kMRegNone;
    };
    MReg XK = pick_hi(kMRegNone);
    MReg X2 = pick_hi(XK);
    if (dbg) std::fprintf(stderr, "[masked] hi regs XK=%d X2=%d\n", static_cast<int>(XK), static_cast<int>(X2));
    if (XK == kMRegNone || X2 == kMRegNone) return false;

    // cr 的 DAG 求值器（标量、逐 lane）：允许 ConstFloat / float LoadSlot /
    // Convert(int) / 四则。invariant int 装载用 rax 中转。
    std::vector<MReg> scratch{kXmm6, kXmm7, kXmm5, kXmm4};
    std::function<bool(VReg, MReg, MReg, int)> eval_cr =
        [&](VReg v, MReg lane, MReg out, int depth) -> bool {
        if (depth > 8) return false;
        const Inst* d = def(v);
        if (!d) return false;
        switch (d->op) {
            case IrOp::ConstFloat: {
                uint64_t bits = 0;
                std::memcpy(&bits, &d->fimm, sizeof(bits));
                emit_imm_hex(MOp::kMovabsq, bits, kRax);
                emit2(MOp::kMovq, kRax, out);
                return true;
            }
            case IrOp::LoadSlot: {
                if (d->idx != iv_slot && d->idx != iv_alias &&
                    def_block(v) >= 0 && def_block(v) != body_bi) {
                    const MOperand& sm = src_mop(v);
                    if (sm.kind == MOperandKind::Reg) emit2(MOp::kMovapd, sm, out);
                    else emit2(MOp::kMovsd, sm, out);
                    return true;
                }
                if (d->idx == iv_slot || d->idx == iv_alias) {
                    emit2(MOp::kCvtsi2sdq, lane, out);
                    return true;
                }
                if (is_float_ty(sl_ty(d->idx))) {
                    const MOperand& sm = slot_mop(d->idx);
                    if (sm.kind == MOperandKind::Reg)
                        emit2(MOp::kMovapd, sm, out);
                    else
                        emit2(MOp::kMovsd, sm, out);
                    return true;
                }
                return false;
            }
            case IrOp::Convert: {
                if (def_block(v) >= 0 && def_block(v) != body_bi) {
                    const MOperand& sm = src_mop(v);
                    if (sm.kind == MOperandKind::Reg) emit2(MOp::kMovapd, sm, out);
                    else emit2(MOp::kMovsd, sm, out);
                    return true;
                }
                const Inst* cd = def(d->a);
                if (!cd) return false;
                if (cd->op == IrOp::LoadSlot && (cd->idx == iv_slot || cd->idx == iv_alias)) {
                    emit2(MOp::kCvtsi2sdq, lane, out);
                    return true;
                }
                if (cd->op == IrOp::LoadSlot && is_int_ty(sl_ty(cd->idx))) {
                    if (std::getenv("PYADAMAS_MASKED_DBG")) { const MOperand& dm = slot_mop(cd->idx); std::fprintf(stderr, "[masked] conv int slot=%d kind=%d reg=%d\n", static_cast<int>(cd->idx), static_cast<int>(dm.kind), static_cast<int>(dm.reg)); }
                    emit2(MOp::kMovq, slot_mop(cd->idx), kRax);
                    emit2(MOp::kCvtsi2sdq, kRax, out);
                    return true;
                }
                if (cd->op == IrOp::ConstInt) {
                    emit_imm(MOp::kMovq, cd->imm, kRax);
                    emit2(MOp::kCvtsi2sdq, kRax, out);
                    return true;
                }
                return false;
            }
            case IrOp::Mov:
                return eval_cr(d->a, lane, out, depth);
            case IrOp::Bin: {
                MOp op = MOp::kRaw;
                switch (d->bop) {
                    case BinKind::Add: op = MOp::kAddpd; break;
                    case BinKind::Sub: op = MOp::kSubpd; break;
                    case BinKind::Mul: op = MOp::kMulpd; break;
                    case BinKind::Div: op = MOp::kDivpd; break;
                    default: return false;
                }
                if (!is_float_ty(ty(v))) return false;
                if (def_block(v) >= 0 && def_block(v) != body_bi) {
                    const MOperand& sm = src_mop(v);
                    if (sm.kind == MOperandKind::Reg) emit2(MOp::kMovapd, sm, out);
                    else emit2(MOp::kMovsd, sm, out);
                    return true;
                }
                if (depth >= static_cast<int>(scratch.size())) return false;
                MReg sc = scratch[static_cast<size_t>(depth)];
                if (sc == out) return false;
                if (!eval_cr(d->a, lane, out, depth + 1)) return false;
                if (!eval_cr(d->b, lane, sc, depth + 1)) return false;
                emit2(op, sc, out);
                return true;
            }
            default: return false;
        }
    };
    // 先做一次“可发射性”检查（不写字节），失败就整段不发。
    {
        // 用一个不发射的模拟：这里直接复用发射路径但先保存输出长度？简单做法：
        // 真实发射在最后，若中途失败已经发了字节会破坏代码；所以用 dry-run：
        // 判断 DAG 结构可行性（与发射同一判据，但不动 emit_）。
        std::function<bool(VReg, int)> ok = [&](VReg v, int depth) -> bool {
            if (depth > 8) return false;
            const Inst* d = def(v);
            if (!d) return false;
            switch (d->op) {
                case IrOp::ConstFloat: return true;
                case IrOp::LoadSlot:
                    return d->idx == iv_slot || d->idx == iv_alias || is_float_ty(sl_ty(d->idx));
                case IrOp::Convert: {
                    const Inst* cd = def(d->a);
                    if (!cd) return false;
                    return (cd->op == IrOp::LoadSlot &&
                            (cd->idx == iv_slot || cd->idx == iv_alias || is_int_ty(sl_ty(cd->idx)))) ||
                           (cd->op == IrOp::ConstInt);
                }
                case IrOp::Mov: return ok(d->a, depth);
                case IrOp::Bin:
                    if (!is_float_ty(ty(v))) return false;
                    return ok(d->a, depth + 1) && ok(d->b, depth + 1);
                default: return false;
            }
        };
        if (!ok(cr_val, 0)) return false;
    }
    // 不变量 int（外层上界 / 内层上界）可发射性。
    std::function<bool(VReg, int)> inv_ok = [&](VReg v, int depth) -> bool {
        if (depth > 8) return false;
        const Inst* d = def(v);
        if (!d) return false;
        switch (d->op) {
            case IrOp::ConstInt: return true;
            case IrOp::LoadSlot: return is_int_ty(sl_ty(d->idx));
            case IrOp::Mov: return inv_ok(d->a, depth + 1);
            default: return false;
        }
    };
    if (dbg) std::fprintf(stderr, "[masked] inv ok=%d/%d\n", inv_ok(outer_bound, 0) ? 1 : 0, inv_ok(maxit_v, 0) ? 1 : 0);
    if (!inv_ok(outer_bound, 0) || !inv_ok(maxit_v, 0)) return false;

    // ── 8. 发射 ─────────────────────────────────────────────────────────
    //
    // 只用 rax/rcx/rdx/rsi/rdi 作 scratch（与通用向量化同口径：r8-r11 可能被 RA
    // 分配给跨块活跃值，实测 outer bound 就在 %r8，覆写会让循环多跑一圈）。
    // 上界/上界比较直接用 src_mop 的当前操作数（寄存器/栈），不搬进 scratch。
    auto raw2 = [&](const char* mn, MReg a, MReg c) {
        emit2(mn, std::string(mreg_name(a)), std::string(mreg_name(c)));
    };
    auto load_f64 = [&](int slot, MReg out) {
        const MOperand& sm = slot_mop(slot);
        if (sm.kind == MOperandKind::Reg) emit2(MOp::kMovapd, sm, out);
        else emit2(MOp::kMovsd, sm, out);
    };
    auto bcast_const = [&](double val, MReg out) {
        uint64_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        emit_imm_hex(MOp::kMovabsq, bits, kRax);
        emit2(MOp::kMovq, kRax, out);
        emit2(MOp::kMovlhps, out, out);
    };

    const std::string fallback = fresh_label("masked_scalar");
    const std::string vec = fresh_label("masked_vec");

    emit2(MOp::kMovq, slot_mop(iv_slot), kRdx);        // rdx = iv（当前 x）
    emit2(MOp::kTestq, kRdx, kRdx);
    emit_jmp(MOp::kJs, fallback);                      // iv < 0 → 标量
    emit2(MOp::kLeaq, AsmGen::op_mem(kRdx, 1), kRax);  // rax = iv+1
    emit2(MOp::kCmpq, src_mop(outer_bound), kRax);
    emit_jmp(MOp::kJge, fallback);                     // iv+1 >= size → 标量
    bcast_const(lim_def->fimm, XK);                    // 内层比较上限 K 广播
    bcast_const(two_def->fimm, X2);                    // 2.0 广播
    load_f64(ci_slot, kXmm3);
    emit2(MOp::kMovlhps, kXmm3, kXmm3);                // ci 广播

    if (std::getenv("PYADAMAS_VECT_DBG"))
        std::fprintf(stderr, "[vect] masked divergence loop body bb%d\n", body_bi);
    label(vec);
    // crv：lane0 用 x 求值 → xmm0；rdx 临时 +1 求 lane1 → xmm1；拼 [cr0, cr1]。
    if (!eval_cr(cr_val, kRdx, kXmm0, 0)) return false;
    emit_imm(MOp::kAddq, 1, kRdx);
    if (!eval_cr(cr_val, kRdx, kXmm1, 0)) return false;
    emit_imm(MOp::kSubq, 1, kRdx);
    emit2(MOp::kUnpcklpd, kXmm1, kXmm0);
    emit2(MOp::kMovapd, kXmm0, kXmm2);                 // xmm2 = crv
    emit2(MOp::kXorpd, kXmm0, kXmm0);                  // zr = 0
    emit2(MOp::kXorpd, kXmm1, kXmm1);                  // zi = 0
    emit2(MOp::kPcmpeqd, kXmm4, kXmm4);                // act = 全 1
    emit2(MOp::kPxor, kXmm5, kXmm5);                   // cnt = 0
    emit_imm(MOp::kMovq, 0, kRcx);                     // k = 0
    const std::string inner = fresh_label("masked_inner");
    const std::string done = fresh_label("masked_done");
    label(inner);
    emit2(MOp::kCmpq, src_mop(maxit_v), kRcx);
    emit_jmp(MOp::kJge, done);                         // k >= maxit
    emit2(MOp::kMovapd, kXmm0, kXmm6);
    emit2(MOp::kMulpd, kXmm6, kXmm6);
    emit2(MOp::kMovapd, kXmm1, kXmm7);
    emit2(MOp::kMulpd, kXmm7, kXmm7);
    emit2(MOp::kAddpd, kXmm7, kXmm6);
    raw2("cmplepd", XK, kXmm6);                        // xmm6 = mag <= K
    raw2("andpd", kXmm6, kXmm4);                       // act &= mask
    emit2("movmskpd", std::string(mreg_name(kXmm4)), std::string("%eax"));
    emit2("testl", std::string("%eax"), std::string("%eax"));
    emit_jmp(MOp::kJe, done);                          // 两 lane 都收敛
    emit2(MOp::kMovapd, kXmm0, kXmm6);
    emit2(MOp::kMulpd, kXmm6, kXmm6);
    emit2(MOp::kMovapd, kXmm1, kXmm7);
    emit2(MOp::kMulpd, kXmm7, kXmm7);
    emit2(MOp::kSubpd, kXmm7, kXmm6);
    emit2(MOp::kAddpd, kXmm2, kXmm6);                  // xmm6 = t
    emit2(MOp::kMovapd, X2, kXmm7);
    emit2(MOp::kMulpd, kXmm0, kXmm7);                  // 2.0*zr
    emit2(MOp::kMulpd, kXmm1, kXmm7);                  // (2.0*zr)*zi
    emit2(MOp::kAddpd, kXmm3, kXmm7);                  // + ci
    emit2(MOp::kMovapd, kXmm6, kXmm0);
    emit2(MOp::kMovapd, kXmm7, kXmm1);
    emit2(MOp::kPsubq, kXmm4, kXmm5);                  // 活跃 lane 计数 +1
    emit1(MOp::kIncq, kRcx);
    emit_jmp(MOp::kJmp, inner);
    label(done);
    emit2(MOp::kMovq, kXmm5, kRax);
    emit2(MOp::kMovhlps, kXmm5, kXmm6);
    emit2(MOp::kMovq, kXmm6, kRcx);
    emit2(MOp::kAddq, kRcx, kRax);
    emit2(MOp::kMovq, slot_mop(total_slot), kRdi);
    emit2(MOp::kAddq, kRax, kRdi);
    emit2(MOp::kMovq, kRdi, slot_mop(total_slot));
    emit_imm(MOp::kAddq, 2, kRdx);
    emit2(MOp::kLeaq, AsmGen::op_mem(kRdx, 1), kRax);
    emit2(MOp::kCmpq, src_mop(outer_bound), kRax);
    emit_jmp(MOp::kJl, vec);
    emit2(MOp::kMovq, kRdx, slot_mop(iv_slot));        // 写回剩余 iv
    emit_jmp(MOp::kJmp, block_label(bc));
    label(fallback);                                    // 其后是原标量体
    return true;
}

}  // namespace pyadamas
