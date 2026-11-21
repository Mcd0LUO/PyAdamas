// opt_intloop.cpp —— 整数循环优化（0.335，PYADAMAS_LOOP_OPT 门控，默认关）
//
// 测量驱动（docs/intloop-design.md §1 perf annotate 取证）：
//   - nqueens ~19.6% 周期在 continue 路径的无条件跳转链（bb4→bb7→bb5→bb3），
//     ~20% 在 LICM 提升出的 3 份 row 栈重载；
//   - matmul 内循环 ~60% 在索引算术 + 栈往返（检查型下标的冷路径 call
//     迫使块内临时值全部溢出）。
//
// 本文件三个遍（位掩码独立门控，见 opt.h）：
//   bit0 thread_jump_blocks     —— 单 jump 中转块线程化（纯 CFG 等价变换）
//   bit2 dedup_block_loads      —— 同块 LoadSlot/LoadGlobal 去重（局部值编号）
//   bit1 version_bounds_affine  —— 仿射 IV 边界检查消除（见文件后半）
//
// 纪律：flag-off 不进入任何新路径；宁保守跳过不可误优化。
#include "opt_detail.h"

#include <algorithm>
#include <functional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace pyadamas {
namespace optdetail {

namespace {

bool il_has_eh(const IRFunction& fn) {
    return fn.has_handlers || fn.is_generator || !fn.eh_regions.empty();
}

}  // namespace

// ── bit0：跳转线程化 ─────────────────────────────────────────────
//
// 只含一条 jump T 的块 B 是纯中转：把所有指向 B 的显式边改指 T。
// nqueens 的 continue 路径 bb4→bb7(if.end: jump bb5)→bb5 因此少一跳
// （perf 实测 bb7 的 jmp 占 9.09% 周期）。
//
// 语义平凡等价：B 无副作用，进入 B 必然立即到 T。fallthrough 前驱
// （B-1 无终止指令）仍会顺流进 B：这种 B 保留原 jump（仍可达）。
// 无 fallthrough 的 B 清空指令但**保留块位**——块号是 regalloc 线性
// 扫描的区间坐标，重编号会扰动槽分配（实测 matmul +9.7% 假回归）。
bool thread_jump_blocks(IRFunction& fn) {
    if (il_has_eh(fn)) return false;
    const int nb = static_cast<int>(fn.blocks.size());
    auto single_jump = [&](int b) -> int {
        if (b < 0 || b >= nb) return -1;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.size() != 1 || insts[0].op != IrOp::Jump) return -1;
        int t = insts[0].target;
        if (t < 0 || t >= nb || t == b) return -1;
        return t;
    };
    // 沿单 jump 链走到最终目标；遇环返回原目标（保守不线程化）。
    auto final_target = [&](int t) -> int {
        int cur = t;
        for (int steps = 0; steps < nb; ++steps) {
            int nxt = single_jump(cur);
            if (nxt < 0) return cur;
            if (nxt == t) return t;  // 环
            cur = nxt;
        }
        return t;
    };
    // **只改多指令块的出边；单 jump 块原样冻结。**
    //
    // 两条血泪教训（实测 matmul +8~10% / fannkuch +10.7% 假回归）：
    //   1. prune 重编号会扰动 regalloc 线性扫描的区间坐标（块号×stride）；
    //   2. codegen 用「回边 jump」当循环头对齐的代理（.p2align 5），
    //      清空/改向中转块会让热 incr 块丢对齐。
    // 冻结单 jump 块（哪怕不可达）保住全部对齐标记与代码布局；
    // 多指令块的出边直接指到链尾，执行路径省掉中转 jmp。
    // fallthrough 进中转块的边不受影响（中转块原样保留，语义不变）。
    bool any = false;
    for (int b = 0; b < nb; ++b) {
        auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) continue;
        if (insts.size() == 1 && insts[0].op == IrOp::Jump) continue;
        Inst& last = insts.back();
        if (last.op == IrOp::Jump) {
            if (single_jump(last.target) >= 0) {
                int t = final_target(last.target);
                if (t != last.target) { last.target = t; any = true; }
            }
        } else if (last.op == IrOp::Branch) {
            if (single_jump(last.target) >= 0) {
                int t = final_target(last.target);
                if (t != last.target) { last.target = t; any = true; }
            }
            if (single_jump(last.target2) >= 0) {
                int t = final_target(last.target2);
                if (t != last.target2) { last.target2 = t; any = true; }
            }
        }
    }
    return any;
}

// ── bit2：同块 LoadSlot/LoadGlobal 去重 ──────────────────────────
//
// LICM 会把同一槽的多次读提升成多个独立 vreg（nqueens licm.pre 里
// 3× load.slot s0），各自溢出到不同栈槽；CSE 只把 LoadSlot 编码成 Bin
// 的 Slot 操作数，不合并 LoadSlot 指令本身，LoadGlobal 操作数更是按
// 原始 vreg 编码（matmul 三份 load.global g0 → 三份 i*N 不合并）。
//
// 这里做块内局部值编号：同块、同槽/同全局、中间无写（全局还要无调用）、
// 类型一致、两个 vreg 都是单定义 ⇒ 后一个 load 的所有使用改读前一个。
// 单定义保证使用点被定义块支配，前一个 load 在同块更早处，支配关系
// 保持，语义等价。合并后 CSE 能进一步合并以它们为操作数的表达式。
bool dedup_block_loads(IRFunction& fn) {
    const size_t nv = fn.vreg_ty.size();
    std::vector<int> ndefs(nv, 0);
    std::unordered_set<int> addr_taken;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts) {
            if (in.dst != kNoReg && in.dst < nv) ++ndefs[in.dst];
            if (in.op == IrOp::SlotAddr && in.idx >= 0) addr_taken.insert(in.idx);
        }
    auto is_call = [](IrOp op) {
        switch (op) {
            case IrOp::Call:
            case IrOp::CallRt:
            case IrOp::CallExtern:
            case IrOp::CallDirect:
            case IrOp::VCall:
            case IrOp::VCallStatic:
            case IrOp::SuperVCall:
                return true;
            default:
                return false;
        }
    };
    std::unordered_map<VReg, VReg> sub;
    bool any = false;
    for (auto& b : fn.blocks) {
        std::unordered_map<int, VReg> slot_val;
        std::unordered_map<int, VReg> global_val;
        std::vector<size_t> drop;
        for (size_t i = 0; i < b.insts.size(); ++i) {
            const Inst& in = b.insts[i];
            if (is_call(in.op)) {
                global_val.clear();  // 调用可能写全局；局部槽不受影响
                continue;
            }
            switch (in.op) {
                case IrOp::StoreSlot:
                    if (in.idx >= 0) slot_val.erase(in.idx);
                    break;
                case IrOp::StoreGlobal:
                    if (in.idx >= 0) global_val.erase(in.idx);
                    break;
                case IrOp::SlotAddr:
                    if (in.idx >= 0) slot_val.erase(in.idx);
                    break;
                case IrOp::LoadSlot:
                case IrOp::LoadGlobal: {
                    if (in.dst == kNoReg || in.dst >= nv || in.idx < 0) break;
                    if (in.op == IrOp::LoadSlot && addr_taken.contains(in.idx)) break;
                    auto& val = in.op == IrOp::LoadSlot ? slot_val : global_val;
                    auto it = val.find(in.idx);
                    if (it == val.end()) {
                        val[in.idx] = in.dst;
                        break;
                    }
                    VReg prev = it->second;
                    if (prev >= nv || ndefs[in.dst] != 1 || ndefs[prev] != 1) break;
                    TypePtr dt = fn.vreg_ty[in.dst];
                    TypePtr pt = fn.vreg_ty[prev];
                    if (!dt || !pt || !type_eq(dt, pt)) break;
                    sub[in.dst] = prev;
                    drop.push_back(i);
                    any = true;
                    break;
                }
                default:
                    break;
            }
        }
        for (auto it = drop.rbegin(); it != drop.rend(); ++it)
            b.insts.erase(b.insts.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    if (!any) return false;
    for (auto& b : fn.blocks)
        for (auto& in : b.insts) {
            auto fix = [&](VReg& r) {
                auto it = sub.find(r);
                if (it != sub.end()) r = it->second;
            };
            fix(in.a);
            fix(in.b);
            fix(in.c);
            for (VReg& r : in.args) fix(r);
        }
    fn.rebuild_use_def();
    return true;
}

// ── bit1：仿射 IV 边界检查消除 ───────────────────────────────────
//
// 形状：for v in range(bound) 的自然循环（单 IV、初值 0、步长 +1、
// bound 循环不变），体内下标 idx = IV + off（off 不变）或 idx = off。
// 入口守卫 off>=0 && (bound-1)+off < len(base)（max>=off 检测回绕），
// 通过走 unchecked 克隆循环，不通过走原检查循环。正确性论证见
// docs/intloop-design.md §2 P2。与旧 version_bounds 的关键区别：
//   - 不外提 load（无 cand_dsts/redirect 混用——旧 attempt 死循环根因），
//     克隆是纯复制 + 候选 op 翻 unchecked；
//   - 允许 IndexStore（只改元素不改 len；不外提就没有读旧值问题）；
//   - 只版本化最内层循环（避免嵌套克隆爆炸）。

namespace {

struct IlCfg {
    std::vector<std::vector<int>> succ;
    std::vector<std::vector<int>> pred;
    std::vector<char> reach;
};

IlCfg il_build_cfg(const IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    IlCfg g;
    g.succ.assign(static_cast<size_t>(nb), {});
    g.pred.assign(static_cast<size_t>(nb), {});
    for (int b = 0; b < nb; ++b) {
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) {
            if (b + 1 < nb) g.succ[static_cast<size_t>(b)].push_back(b + 1);
            continue;
        }
        const Inst& last = insts.back();
        if (last.op == IrOp::Jump) {
            if (last.target >= 0 && last.target < nb)
                g.succ[static_cast<size_t>(b)].push_back(last.target);
        } else if (last.op == IrOp::Branch) {
            if (last.target >= 0 && last.target < nb)
                g.succ[static_cast<size_t>(b)].push_back(last.target);
            if (last.target2 >= 0 && last.target2 < nb)
                g.succ[static_cast<size_t>(b)].push_back(last.target2);
        } else if (!fn.blocks[static_cast<size_t>(b)].terminated() && b + 1 < nb) {
            g.succ[static_cast<size_t>(b)].push_back(b + 1);
        }
    }
    for (int b = 0; b < nb; ++b)
        for (int s : g.succ[static_cast<size_t>(b)])
            g.pred[static_cast<size_t>(s)].push_back(b);
    g.reach.assign(static_cast<size_t>(nb), 0);
    if (nb > 0) {
        std::vector<int> work{0};
        g.reach[0] = 1;
        while (!work.empty()) {
            int b = work.back();
            work.pop_back();
            for (int s : g.succ[static_cast<size_t>(b)])
                if (!g.reach[static_cast<size_t>(s)]) {
                    g.reach[static_cast<size_t>(s)] = 1;
                    work.push_back(s);
                }
        }
    }
    return g;
}

std::vector<std::vector<char>> il_compute_dom(const IlCfg& g) {
    const int nb = static_cast<int>(g.succ.size());
    std::vector<std::vector<char>> dom(static_cast<size_t>(nb),
                                       std::vector<char>(static_cast<size_t>(nb), 0));
    for (int b = 0; b < nb; ++b)
        if (g.reach[static_cast<size_t>(b)])
            dom[static_cast<size_t>(b)].assign(static_cast<size_t>(nb), 1);
    if (nb == 0) return dom;
    dom[0].assign(static_cast<size_t>(nb), 0);
    dom[0][0] = 1;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < nb; ++b) {
            if (!g.reach[static_cast<size_t>(b)]) continue;
            std::vector<char> nd(static_cast<size_t>(nb), 1);
            bool first = true;
            for (int p : g.pred[static_cast<size_t>(b)]) {
                if (!g.reach[static_cast<size_t>(p)]) continue;
                if (first) {
                    nd = dom[static_cast<size_t>(p)];
                    first = false;
                    continue;
                }
                const auto& dp = dom[static_cast<size_t>(p)];
                for (size_t i = 0; i < nd.size(); ++i) nd[i] = nd[i] && dp[i];
            }
            nd[static_cast<size_t>(b)] = 1;
            if (nd != dom[static_cast<size_t>(b)]) {
                dom[static_cast<size_t>(b)] = std::move(nd);
                changed = true;
            }
        }
    }
    return dom;
}

bool il_rtfn_pure_scalar(RtFn f) {
    switch (f) {
        case RtFn::MathSqrt: case RtFn::MathSin: case RtFn::MathCos:
        case RtFn::MathTan: case RtFn::MathAsin: case RtFn::MathAcos:
        case RtFn::MathAtan: case RtFn::MathAtan2: case RtFn::MathExp:
        case RtFn::MathLog: case RtFn::MathLog2: case RtFn::MathLog10:
        case RtFn::MathPow: case RtFn::MathFabs: case RtFn::MathFloor:
        case RtFn::MathCeil: case RtFn::MathTrunc: case RtFn::MathFmod:
        case RtFn::MathHypot: case RtFn::MathCopysign:
            return true;
        case RtFn::RcInc:
        case RtFn::RcTemp: case RtFn::RcTempTake:
        case RtFn::RcMark:
            return true;
        default:
            return false;
    }
}

// 循环体指令白名单：任何可能改容器长度/逃逸槽地址/跑用户代码的 op 都拒绝。
bool il_loop_op_safe(const Inst& in) {
    switch (in.op) {
        case IrOp::ConstInt: case IrOp::ConstFloat: case IrOp::ConstStr:
        case IrOp::ConstNone: case IrOp::ConstBig: case IrOp::FuncAddr:
        case IrOp::Mov: case IrOp::LoadSlot: case IrOp::StoreSlot:
        case IrOp::LoadGlobal:
        case IrOp::Len:
        case IrOp::IndexLoad: case IrOp::IndexLoadUnchecked:
        case IrOp::IndexStore: case IrOp::IndexStoreUnchecked:
        case IrOp::Jump: case IrOp::Branch:
            return true;
        case IrOp::Bin: case IrOp::Un: case IrOp::Cmp: case IrOp::Convert:
            return in.ty && (in.ty->kind == TyKind::Int ||
                             in.ty->kind == TyKind::Float ||
                             in.ty->kind == TyKind::Bool);
        case IrOp::CallRt:
            return il_rtfn_pure_scalar(static_cast<RtFn>(in.imm));
        default:
            return false;
    }
}

struct IlDef {
    int block = -1;
    size_t pos = 0;
    const Inst* in = nullptr;
};

struct IlAffine {
    bool ok = false;
    int coeff = 0;       // 0 = 不变下标，1 = IV + off
    VReg off = kNoReg;   // kNoReg = 0
};

bool version_bounds_affine_loop(IRFunction& fn, int h, const std::vector<char>& body,
                                const std::vector<std::vector<char>>& dom,
                                const std::vector<char>& is_header) {
    const int nb = static_cast<int>(fn.blocks.size());
    const auto in_loop = [&](int b) { return body[static_cast<size_t>(b)] != 0; };

    // 只版本化最内层循环：体内含别的循环头就跳过（防嵌套克隆爆炸）。
    for (int b = 0; b < nb; ++b)
        if (b != h && in_loop(b) && is_header[static_cast<size_t>(b)]) return false;

    // ── 头块形状：load.slot S; cmp.lt iv, bound; branch ──
    const auto& hi = fn.blocks[static_cast<size_t>(h)].insts;
    if (hi.size() < 3) return false;
    const Inst& br = hi.back();
    if (br.op != IrOp::Branch || br.a == kNoReg) return false;
    const Inst& cmp = hi[hi.size() - 2];
    if (cmp.op != IrOp::Cmp || cmp.cop != CmpKind::Lt || cmp.dst != br.a) return false;
    int iv_load_pos = -1;
    VReg bound_v = kNoReg;
    for (size_t k = 0; k + 2 < hi.size(); ++k) {
        if (hi[k].op != IrOp::LoadSlot || hi[k].dst == kNoReg) continue;
        if (hi[k].dst == cmp.a) { iv_load_pos = static_cast<int>(k); bound_v = cmp.b; break; }
        if (hi[k].dst == cmp.b) { iv_load_pos = static_cast<int>(k); bound_v = cmp.a; break; }
    }
    if (iv_load_pos < 0 || bound_v == kNoReg) return false;
    const int iv_slot = hi[static_cast<size_t>(iv_load_pos)].idx;
    if (iv_slot < 0 || iv_slot >= static_cast<int>(fn.slot_ty.size())) return false;
    if (!fn.slot_ty[static_cast<size_t>(iv_slot)] ||
        fn.slot_ty[static_cast<size_t>(iv_slot)]->kind != TyKind::Int) return false;
    // branch 恰一个目标在体内。
    bool t_in = in_loop(br.target), t2_in = in_loop(br.target2);
    if (t_in == t2_in) return false;

    // 全函数定义表 + 槽写/取地址事实。
    const size_t nv = fn.vreg_ty.size();
    std::vector<IlDef> defs(nv);
    std::vector<int> ndefs(nv, 0);
    const size_t ns = fn.slot_ty.size();
    std::vector<int> slot_stores_loop(ns, 0), slot_stores_out(ns, 0);
    std::vector<int> slot_store_out_block(ns, -1);
    std::vector<VReg> slot_store_out_val(ns, kNoReg);
    std::vector<char> slot_addr(ns, 0);
    std::vector<int> slot_store_loop_block(ns, -1);
    std::vector<int> slot_store_loop_pos(ns, -1);
    std::vector<VReg> slot_store_loop_val(ns, kNoReg);
    for (int b = 0; b < nb; ++b) {
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        for (size_t k = 0; k < insts.size(); ++k) {
            const Inst& in = insts[k];
            if (in.dst != kNoReg && in.dst < nv) {
                ++ndefs[in.dst];
                defs[in.dst] = IlDef{b, k, &in};
            }
            if (in.op == IrOp::SlotAddr && in.idx >= 0 && in.idx < static_cast<int>(ns))
                slot_addr[static_cast<size_t>(in.idx)] = 1;
            if (in.op == IrOp::StoreSlot && in.idx >= 0 && in.idx < static_cast<int>(ns)) {
                size_t s = static_cast<size_t>(in.idx);
                if (in_loop(b)) {
                    ++slot_stores_loop[s];
                    slot_store_loop_block[s] = b;
                    slot_store_loop_pos[s] = static_cast<int>(k);
                    slot_store_loop_val[s] = in.a;
                } else {
                    ++slot_stores_out[s];
                    slot_store_out_block[s] = b;
                    slot_store_out_val[s] = in.a;
                }
            }
        }
    }
    auto const_val = [&](VReg v, int64_t want) -> bool {
        if (v == kNoReg || v >= nv || ndefs[v] != 1) return false;
        const IlDef& d = defs[v];
        return d.in && d.in->op == IrOp::ConstInt && d.in->imm == want;
    };
    auto invariant_v = [&](VReg v) -> bool {
        if (v == kNoReg || v >= nv || ndefs[v] != 1) return false;
        int db = defs[v].block;
        return !in_loop(db) && dom[static_cast<size_t>(h)][static_cast<size_t>(db)] != 0;
    };

    // ── IV 认定：体内恰一处 S += 1，体外恰一处 S = 0 且支配头 ──
    if (slot_addr[static_cast<size_t>(iv_slot)]) return false;
    if (slot_stores_loop[static_cast<size_t>(iv_slot)] != 1) return false;
    if (slot_stores_out[static_cast<size_t>(iv_slot)] != 1) return false;
    {
        size_t s = static_cast<size_t>(iv_slot);
        VReg ov = slot_store_out_val[s];
        if (!const_val(ov, 0)) return false;
        if (!dom[static_cast<size_t>(h)][static_cast<size_t>(slot_store_out_block[s])])
            return false;
        int lb = slot_store_loop_block[s];
        VReg av = slot_store_loop_val[s];
        if (av == kNoReg || av >= nv || ndefs[av] != 1) return false;
        const IlDef& ad = defs[av];
        if (ad.block != lb || !ad.in || ad.in->op != IrOp::Bin ||
            ad.in->bop != BinKind::Add) return false;
        VReg la = ad.in->a, lb2 = ad.in->b;
        auto is_iv_load = [&](VReg v) {
            if (v == kNoReg || v >= nv || ndefs[v] != 1) return false;
            const IlDef& d = defs[v];
            return d.block == lb && d.in && d.in->op == IrOp::LoadSlot &&
                   d.in->idx == iv_slot;
        };
        bool ok = (is_iv_load(la) && const_val(lb2, 1)) ||
                  (is_iv_load(lb2) && const_val(la, 1));
        if (!ok) return false;
    }

    // ── bound 分类：不变 vreg（含常量，体外单定义支配头）/ 头块槽读 ──
    // 守卫在循环入口执行，bound 必须在那时已有定义：体外定义且支配头
    // ⇒ 支配每个循环外前驱 ⇒ 守卫里可直接读。头块槽读则在守卫里重读
    // （槽在体内无写，入口值 == 头块首轮读到的值）。
    int bound_reload_slot = -1;
    if (!invariant_v(bound_v)) {
        if (bound_v < nv && ndefs[bound_v] == 1 && defs[bound_v].block == h &&
            defs[bound_v].in && defs[bound_v].in->op == IrOp::LoadSlot) {
            int bs = defs[bound_v].in->idx;
            if (bs >= 0 && bs < static_cast<int>(ns) &&
                !slot_addr[static_cast<size_t>(bs)] &&
                slot_stores_loop[static_cast<size_t>(bs)] == 0)
                bound_reload_slot = bs;
        }
        if (bound_reload_slot < 0) return false;
    }

    // ── 循环体 op 白名单 + 多定义 vreg ⇒ 拒绝 + 体内定义被体外使用 ⇒ 拒绝 ──
    for (int b = 0; b < nb; ++b) {
        if (!in_loop(b)) continue;
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            if (!il_loop_op_safe(in)) return false;
            // 克隆按 dst 重映射 vreg：多定义会让映射互相覆盖，拒绝。
            if (in.dst != kNoReg && in.dst < nv && ndefs[in.dst] != 1) return false;
        }
    }
    for (int b = 0; b < nb; ++b) {
        if (in_loop(b)) continue;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        for (const auto& in : insts) {
            auto chk = [&](VReg v) -> bool {
                if (v == kNoReg || v >= nv || ndefs[v] != 1) return false;
                return in_loop(defs[v].block);
            };
            if (chk(in.a) || chk(in.b) || chk(in.c)) return false;
            for (VReg r : in.args) if (chk(r)) return false;
        }
    }

    // ── 副本槽链：T 体内唯一写 ← load.slot(IV 或已知副本) ──
    std::vector<char> is_iv_copy(ns, 0);
    is_iv_copy[static_cast<size_t>(iv_slot)] = 1;
    bool chg = true;
    while (chg) {
        chg = false;
        for (size_t s = 0; s < ns; ++s) {
            if (is_iv_copy[s] || slot_addr[s] || slot_stores_loop[s] != 1) continue;
            if (!fn.slot_ty[s] || fn.slot_ty[s]->kind != TyKind::Int) continue;
            VReg sv = slot_store_loop_val[s];
            if (sv == kNoReg || sv >= nv || ndefs[sv] != 1) continue;
            const IlDef& d = defs[sv];
            if (!d.in || d.in->op != IrOp::LoadSlot || d.in->idx < 0 ||
                d.in->idx >= static_cast<int>(ns)) continue;
            if (d.block != slot_store_loop_block[s]) continue;
            if (is_iv_copy[static_cast<size_t>(d.in->idx)]) {
                is_iv_copy[s] = 1;
                chg = true;
            }
        }
    }

    // ── 仿射下标分析 ──
    // off 必须守卫时已有定义：体外单定义且支配头（含常量）。体内定义
    // 的值守卫读不到，一律不认（宁保守）。
    auto off_ok = [&](VReg v) -> bool {
        if (v == kNoReg) return true;
        return invariant_v(v);
    };
    // affine(v)：coeff 0/1 + off。只认 IV+off / off 两种形状。
    std::function<IlAffine(VReg, int)> affine = [&](VReg v, int depth) -> IlAffine {
        IlAffine fail;
        if (v == kNoReg) return IlAffine{true, 0, kNoReg};
        if (v >= nv || ndefs[v] != 1 || depth > 8) return fail;
        const IlDef& d = defs[v];
        if (!d.in) return fail;
        if (d.in->op == IrOp::ConstInt) return IlAffine{true, 0, v};
        if (!in_loop(d.block)) {
            if (dom[static_cast<size_t>(h)][static_cast<size_t>(d.block)])
                return IlAffine{true, 0, v};
            return fail;
        }
        if (d.in->op == IrOp::LoadSlot && d.in->idx >= 0 &&
            d.in->idx < static_cast<int>(ns) &&
            is_iv_copy[static_cast<size_t>(d.in->idx)])
            return IlAffine{true, 1, kNoReg};
        if (d.in->op == IrOp::Mov) return affine(d.in->a, depth + 1);
        if (d.in->op == IrOp::Bin && d.in->bop == BinKind::Add) {
            IlAffine x = affine(d.in->a, depth + 1);
            IlAffine y = affine(d.in->b, depth + 1);
            if (!x.ok || !y.ok) return fail;
            if (x.coeff + y.coeff > 1) return fail;
            if (x.off != kNoReg && y.off != kNoReg) return fail;  // 双 off 不折
            VReg off = x.off != kNoReg ? x.off : y.off;
            if (!off_ok(off)) return fail;
            return IlAffine{true, x.coeff + y.coeff, off};
        }
        return fail;
    };

    struct Cand {
        int block;
        size_t pos;
        int base_slot;
        int coeff;
        VReg off;
        bool is_store;
    };
    // 存储值静态所有权种类（与 codegen idx_store_heap_kind 同口径）。
    auto store_hk = [&](VReg v) -> int {
        TypePtr ct = v < nv ? fn.vreg_ty[v] : nullptr;
        if (ir_is_heap(ct)) return 1;
        if (!ct || ct->kind == TyKind::Unknown) return 2;
        return 0;
    };
    // kind0 证明前提：循环里**每个**存储都是 hk0 标量（否则 kind 可能在
    // 循环中改变，入口 kind==0 守卫保不住全程）。存储基址槽必须稳定。
    std::set<int> kind_slots;
    int n_checked_stores = 0;
    for (int b = 0; b < nb; ++b) {
        if (!in_loop(b)) continue;
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts) {
            if (in.op != IrOp::IndexStore && in.op != IrOp::IndexStoreUnchecked) continue;
            if (store_hk(in.c) != 0) return false;
            if (in.op == IrOp::IndexStore) ++n_checked_stores;
            VReg base = in.a;
            if (base == kNoReg || base >= nv || ndefs[base] != 1) return false;
            const IlDef& bd = defs[base];
            if (!bd.in || bd.in->op != IrOp::LoadSlot || bd.in->idx < 0 ||
                bd.in->idx >= static_cast<int>(ns)) return false;
            size_t bs = static_cast<size_t>(bd.in->idx);
            if (!fn.slot_ty[bs] ||
                (fn.slot_ty[bs]->kind != TyKind::List &&
                 fn.slot_ty[bs]->kind != TyKind::Tuple)) return false;
            if (slot_addr[bs] || slot_stores_loop[bs] != 0) return false;
            kind_slots.insert(static_cast<int>(bs));
        }
    }
    std::vector<Cand> cands;
    for (int b = 0; b < nb; ++b) {
        if (!in_loop(b)) continue;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        for (size_t k = 0; k < insts.size(); ++k) {
            const Inst& in = insts[k];
            if (in.op != IrOp::IndexLoad && in.op != IrOp::IndexStore) continue;
            VReg base = in.a;
            if (base == kNoReg || base >= nv || ndefs[base] != 1) continue;
            const IlDef& bd = defs[base];
            if (!bd.in || bd.in->op != IrOp::LoadSlot || bd.in->idx < 0 ||
                bd.in->idx >= static_cast<int>(ns)) continue;
            size_t bs = static_cast<size_t>(bd.in->idx);
            if (!fn.slot_ty[bs] ||
                (fn.slot_ty[bs]->kind != TyKind::List &&
                 fn.slot_ty[bs]->kind != TyKind::Tuple)) continue;
            if (slot_addr[bs] || slot_stores_loop[bs] != 0) continue;
            IlAffine af = affine(in.b, 0);
            if (!af.ok) continue;
            // 副本槽的写必须**支配读到它的那条 load**：既要在支配意义上
            // 先于它，同块内还要在它前面。只按块支配检查是不够的 ——
            //     for i in range(10):
            //         a[j] = i      # 这里的 j 还是上一轮（或循环外）的值
            //         j = i
            // 里 store(j) 与 load(j) 同块、写在后，块级支配恒真，于是守卫
            // 证明的是 i（本轮 IV）而不是实际下标的旧值，unchecked 越界写。
            // 同块必须 pos(store) < pos(load)。
            if (af.coeff == 1) {
                VReg ivv = in.b;
                std::function<bool(VReg, int)> dom_check = [&](VReg v, int depth) -> bool {
                    if (v == kNoReg || depth > 8) return true;
                    if (v >= nv || ndefs[v] != 1) return false;
                    const IlDef& d = defs[v];
                    if (!d.in || !in_loop(d.block)) return true;
                    if (d.in->op == IrOp::LoadSlot && d.in->idx >= 0 &&
                        d.in->idx < static_cast<int>(ns)) {
                        size_t s = static_cast<size_t>(d.in->idx);
                        if (is_iv_copy[s] && s != static_cast<size_t>(iv_slot)) {
                            const int sb = slot_store_loop_block[s];
                            const int sp = slot_store_loop_pos[s];
                            if (sb < 0 || sp < 0) return false;
                            if (sb != d.block) {
                                // 不同块：写块必须支配 load 所在块。
                                if (!dom[static_cast<size_t>(d.block)][static_cast<size_t>(sb)])
                                    return false;
                            } else if (sp >= static_cast<int>(d.pos)) {
                                // 同块：读在写前（或正是写自己）→ 读到的是旧值。
                                return false;
                            }
                        }
                        return true;
                    }
                    if (d.in->op == IrOp::Mov) return dom_check(d.in->a, depth + 1);
                    if (d.in->op == IrOp::Bin && d.in->bop == BinKind::Add)
                        return dom_check(d.in->a, depth + 1) && dom_check(d.in->b, depth + 1);
                    return true;
                };
                if (!dom_check(ivv, 0)) continue;
            }
            cands.push_back(Cand{b, k, static_cast<int>(bs), af.coeff, af.off,
                                 in.op == IrOp::IndexStore});
        }
    }
    if (cands.empty()) return false;
    // 每个检查型存储都必须是候选：否则它的冷路径 call 还在，块内临时值
    // 依旧溢出，克隆白做（实测这是 BCE 收益的关键——不是去 cmp，是去 call）。
    int n_store_cands = 0;
    for (const auto& c : cands) if (c.is_store) ++n_store_cands;
    if (n_store_cands != n_checked_stores) return false;

    // 摊销门槛：守卫成本 ≈ 8+6×去重守卫数 条指令/每次进入循环，每轮收益
    // ≈ 3×候选数。fannkuch 的 perm 旋转循环平均 ~3.5 轮，摊不平守卫
    // （实测 BCE-only +1.7%）。常量 bound>=8 保证摊销；运行时 bound 未知，
    // 保守要求候选数>=3（matmul 内循环形态）。宁保守跳过。
    {
        bool const_bound = bound_v < nv && ndefs[bound_v] == 1 && defs[bound_v].in &&
                           defs[bound_v].in->op == IrOp::ConstInt;
        if (const_bound) {
            if (defs[bound_v].in->imm < 8) return false;
        } else if (cands.size() < 3) {
            return false;
        }
    }

    // ── 循环外前驱（守卫块插在这些边上） ──
    std::vector<int> outside;
    for (int b = 0; b < nb; ++b) {
        if (in_loop(b)) continue;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        bool to_h = false;
        if (!insts.empty()) {
            const Inst& last = insts.back();
            if (last.op == IrOp::Jump) to_h = last.target == h;
            else if (last.op == IrOp::Branch) to_h = last.target == h || last.target2 == h;
            else if (!fn.blocks[static_cast<size_t>(b)].terminated() && b + 1 == h) to_h = true;
        } else if (b + 1 == h) {
            to_h = true;
        }
        if (to_h) outside.push_back(b);
    }
    if (outside.empty()) return false;

    // ── 克隆映射：紧跟循环体之后插入（保住槽区间紧凑） ──
    // 追加到函数末尾会把循环槽的线性扫描区间拉成跨全函数，热槽丢寄存器
    // （实测 matmul +10%）。紧跟体块插入让克隆与原循环同区间邻域，重叠
    // 结构与原循环一致。块号平移用 rotate_one_loop 同款 bump 逻辑。
    std::vector<int> body_sorted;
    for (int b = 0; b < nb; ++b) if (in_loop(b)) body_sorted.push_back(b);
    const int insert_pos = body_sorted.back() + 1;
    const int n_clones = static_cast<int>(body_sorted.size());

    fn.vreg_nonneg.resize(fn.vreg_ty.size(), 0);
    auto alloc_v = [&](TypePtr t, uint8_t nn) {
        VReg v = static_cast<VReg>(fn.vreg_ty.size());
        fn.vreg_ty.push_back(t ? t : t_unknown());
        fn.vreg_nonneg.push_back(nn);
        return v;
    };
    std::vector<VReg> vmap(nv, kNoReg);
    for (int b : body_sorted)
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts)
            if (in.dst != kNoReg && in.dst < nv) {
                uint8_t nn = in.dst < fn.vreg_nonneg.size()
                                 ? fn.vreg_nonneg[in.dst] : 0;
                vmap[in.dst] = alloc_v(in.ty, nn);
            }
    auto remap_v = [&](VReg r) -> VReg {
        if (r == kNoReg || r >= nv) return r;
        return vmap[r] != kNoReg ? vmap[r] : r;
    };
    std::unordered_set<uint64_t> cand_keys;
    for (const auto& c : cands)
        cand_keys.insert((static_cast<uint64_t>(static_cast<uint32_t>(c.block)) << 32) |
                         static_cast<uint32_t>(c.pos));

    // ── 守卫：计算块 + 短路检查块链 ──
    // 每个检查独占一块（cmp+branch，codegen 融合成 cmp+jcc，不物化
    // bool、不 setcc/spill）；任一失败跳回原检查循环 h，异常时机不变。
    std::vector<Inst> gcompute;
    auto emitc = [&](Inst in) { gcompute.push_back(std::move(in)); };
    auto new_const = [&](int64_t v) {
        VReg d = alloc_v(t_int(), v >= 0 ? 1 : 0);
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v;
        in.dst = d;
        in.ty = t_int();
        emitc(in);
        return d;
    };
    VReg zero = new_const(0);
    VReg one = new_const(1);
    VReg gbound = bound_v;
    if (bound_reload_slot >= 0) {
        gbound = alloc_v(t_int(), 0);
        Inst in;
        in.op = IrOp::LoadSlot;
        in.idx = Operand::slot(bound_reload_slot);
        in.dst = gbound;
        in.ty = t_int();
        emitc(in);
    }
    struct Check { VReg a; VReg b; CmpKind cop; };
    std::vector<Check> checks;
    std::set<std::tuple<int, int, VReg>> guard_done;
    for (const auto& c : cands) {
        auto gk = std::make_tuple(c.base_slot, c.coeff, c.off);
        if (!guard_done.insert(gk).second) continue;
        VReg base = alloc_v(fn.slot_ty[static_cast<size_t>(c.base_slot)], 0);
        {
            Inst in;
            in.op = IrOp::LoadSlot;
            in.idx = Operand::slot(c.base_slot);
            in.dst = base;
            in.ty = fn.slot_ty[static_cast<size_t>(c.base_slot)];
            emitc(in);
        }
        VReg len = alloc_v(t_int(), 1);
        {
            Inst in;
            in.op = IrOp::Len;
            in.a = base;
            in.dst = len;
            in.ty = t_int();
            emitc(in);
        }
        VReg off = c.off;
        if (off == kNoReg) off = zero;
        VReg max = off;
        if (c.coeff == 1) {
            VReg bm1 = alloc_v(t_int(), 0);
            {
                Inst in;
                in.op = IrOp::Bin;
                in.bop = BinKind::Sub;
                in.a = gbound;
                in.b = one;
                in.dst = bm1;
                in.ty = t_int();
                emitc(in);
            }
            max = alloc_v(t_int(), 0);
            Inst in;
            in.op = IrOp::Bin;
            in.bop = BinKind::Add;
            in.a = bm1;
            in.b = off;
            in.dst = max;
            in.ty = t_int();
            emitc(in);
        }
        checks.push_back(Check{off, zero, CmpKind::Ge});   // off >= 0
        checks.push_back(Check{max, off, CmpKind::Ge});    // 回绕检测
        checks.push_back(Check{max, len, CmpKind::Lt});    // max < len
    }
    // kind==0 守卫：存储基址的 elem_kind（偏移 24）。入口 kind0 + 循环内
    // 全是 hk0 标量存储 ⇒ kind 恒 0 ⇒ 克隆存储免 kind 检查（imm=1 快路径）。
    for (int ks : kind_slots) {
        VReg base = alloc_v(fn.slot_ty[static_cast<size_t>(ks)], 0);
        {
            Inst in;
            in.op = IrOp::LoadSlot;
            in.idx = Operand::slot(ks);
            in.dst = base;
            in.ty = fn.slot_ty[static_cast<size_t>(ks)];
            emitc(in);
        }
        VReg kind = alloc_v(t_int(), 0);
        Inst in;
        in.op = IrOp::FieldLoad;
        in.a = base;
        in.imm = 24;
        in.fwidth = 8;
        in.dst = kind;
        in.ty = t_int();
        emitc(in);
        checks.push_back(Check{kind, zero, CmpKind::Eq});
    }

    const int n_checks = static_cast<int>(checks.size());
    const int shift = 1 + n_checks + n_clones;
    const int guard_idx = insert_pos;
    const int check_base = insert_pos + 1;
    const int clone_base = insert_pos + 1 + n_checks;
    std::unordered_map<int, int> block_map;
    for (size_t k = 0; k < body_sorted.size(); ++k)
        block_map[body_sorted[k]] = clone_base + static_cast<int>(k);
    auto bump = [&](int t) { return t >= insert_pos ? t + shift : t; };

    // 计算块尾跳第一个检查块。
    {
        Inst j;
        j.op = IrOp::Jump;
        j.target = check_base;
        gcompute.push_back(std::move(j));
    }
    std::vector<Block> gblocks;
    {
        Block gb;
        gb.name = "ilv.guard";
        gb.insts = std::move(gcompute);
        gblocks.push_back(std::move(gb));
    }
    for (int i = 0; i < n_checks; ++i) {
        Block cb;
        cb.name = "ilv.chk";
        VReg cv = alloc_v(t_bool(), 0);
        Inst cm;
        cm.op = IrOp::Cmp;
        cm.cop = checks[static_cast<size_t>(i)].cop;
        cm.a = checks[static_cast<size_t>(i)].a;
        cm.b = checks[static_cast<size_t>(i)].b;
        cm.dst = cv;
        cm.ty = t_bool();
        cb.insts.push_back(std::move(cm));
        Inst brn;
        brn.op = IrOp::Branch;
        brn.a = cv;
        brn.target = (i + 1 < n_checks) ? check_base + i + 1 : block_map[h];
        brn.target2 = h;
        cb.insts.push_back(std::move(brn));
        gblocks.push_back(std::move(cb));
    }

    // ── 克隆循环体（纯复制 + 候选翻 unchecked，无外提/重定向） ──
    std::vector<Block> clones;
    for (int b : body_sorted) {
        Block cb;
        cb.name = "ilv." + fn.blocks[static_cast<size_t>(b)].name;
        const auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        for (size_t k = 0; k < insts.size(); ++k) {
            Inst out = insts[k];
            out.id = 0;  // 新指令重新分配 id
            out.a = remap_v(out.a);
            out.b = remap_v(out.b);
            out.c = remap_v(out.c);
            for (VReg& r : out.args) r = remap_v(r);
            if (out.dst != kNoReg && out.dst < nv) out.dst = vmap[out.dst];
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32) |
                           static_cast<uint32_t>(k);
            if (cand_keys.contains(key)) {
                if (out.op == IrOp::IndexLoad) out.op = IrOp::IndexLoadUnchecked;
                else if (out.op == IrOp::IndexStore) {
                    out.op = IrOp::IndexStoreUnchecked;
                    out.imm = 1;  // kind0 已证（守卫），走纯 mov 快路径
                }
            } else if (out.op == IrOp::IndexStoreUnchecked) {
                out.imm = 1;  // 降级器已证下标；kind0 由本轮守卫补证
            }
            if (out.op == IrOp::Jump) {
                auto m = block_map.find(out.target);
                out.target = m != block_map.end() ? m->second : bump(out.target);
            } else if (out.op == IrOp::Branch) {
                auto m = block_map.find(out.target);
                out.target = m != block_map.end() ? m->second : bump(out.target);
                auto m2 = block_map.find(out.target2);
                out.target2 = m2 != block_map.end() ? m2->second : bump(out.target2);
            }
            cb.insts.push_back(std::move(out));
        }
        clones.push_back(std::move(cb));
    }

    // ── 原块目标平移（>= insert_pos 的块整体后移 shift 格） ──
    for (auto& blk : fn.blocks) {
        if (blk.insts.empty()) continue;
        Inst& last = blk.insts.back();
        if (last.op == IrOp::Jump) {
            last.target = bump(last.target);
        } else if (last.op == IrOp::Branch) {
            last.target = bump(last.target);
            last.target2 = bump(last.target2);
        }
    }
    // ── 循环外前驱改指守卫 ──
    // 注意：此时块还没插入，前驱仍在**原下标** b（bump 只用于目标引用，
    // 不用于块位置——用平移后的下标会越界，实测 fannkuch 编译期 SEGV）。
    // h < insert_pos，目标 h 不受平移影响。
    for (int b : outside) {
        auto& insts = fn.blocks[static_cast<size_t>(b)].insts;
        bool redirected = false;
        if (!insts.empty()) {
            Inst& last = insts.back();
            if (last.op == IrOp::Jump && last.target == h) {
                last.target = guard_idx;
                redirected = true;
            } else if (last.op == IrOp::Branch) {
                if (last.target == h) { last.target = guard_idx; redirected = true; }
                if (last.target2 == h) { last.target2 = guard_idx; redirected = true; }
            }
        }
        if (!redirected && !fn.blocks[static_cast<size_t>(b)].terminated() && b + 1 == h) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = guard_idx;
            insts.push_back(std::move(j));
        }
    }

    fn.blocks.insert(fn.blocks.begin() + insert_pos,
                     std::make_move_iterator(gblocks.begin()),
                     std::make_move_iterator(gblocks.end()));
    fn.blocks.insert(fn.blocks.begin() + clone_base,
                     std::make_move_iterator(clones.begin()),
                     std::make_move_iterator(clones.end()));
    fn.rebuild_use_def();
    return true;
}

}  // namespace

bool version_bounds_affine_function(IRFunction& fn) {
    if (fn.has_handlers || fn.is_generator || !fn.eh_regions.empty()) return false;
    bool any = false;
    std::unordered_set<int> done;
    for (int guard = 0; guard < 8; ++guard) {
        const int nb = static_cast<int>(fn.blocks.size());
        if (nb < 2) break;
        IlCfg g = il_build_cfg(fn);
        auto dom = il_compute_dom(g);
        std::vector<std::vector<char>> bodies(static_cast<size_t>(nb));
        std::vector<char> is_header(static_cast<size_t>(nb), 0);
        bool found = false;
        for (int n = 0; n < nb; ++n) {
            if (!g.reach[static_cast<size_t>(n)]) continue;
            for (int h : g.succ[static_cast<size_t>(n)]) {
                if (h < 0 || h >= nb ||
                    !dom[static_cast<size_t>(n)][static_cast<size_t>(h)]) continue;
                is_header[static_cast<size_t>(h)] = 1;
                auto& body = bodies[static_cast<size_t>(h)];
                if (body.empty()) {
                    body.assign(static_cast<size_t>(nb), 0);
                    body[static_cast<size_t>(h)] = 1;
                }
                std::vector<int> work{n};
                while (!work.empty()) {
                    int x = work.back();
                    work.pop_back();
                    if (x == h || body[static_cast<size_t>(x)]) continue;
                    body[static_cast<size_t>(x)] = 1;
                    for (int p : g.pred[static_cast<size_t>(x)]) work.push_back(p);
                }
                found = true;
            }
        }
        if (!found) break;
        bool changed = false;
        for (int h = 0; h < nb; ++h) {
            if (bodies[static_cast<size_t>(h)].empty() || done.contains(h)) continue;
            done.insert(h);
            if (version_bounds_affine_loop(fn, h, bodies[static_cast<size_t>(h)], dom,
                                           is_header)) {
                changed = true;
                any = true;
                break;  // 块表变了，重建 CFG 再来
            }
        }
        if (!changed) break;
    }
    return any;
}

}  // namespace optdetail
}  // namespace pyadamas
