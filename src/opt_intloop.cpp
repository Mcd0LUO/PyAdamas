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
// 向量化器候选形态：header（CondBranch）→ body 单块直线 → Jump 回 header。
//
// thread_jump_blocks 改写 CFG 后，这种循环会失去通用向量化（实测 matmul 内层
// j 循环：10.1M → 54.8M 指令，向量化整段失效）。判据只看 IR 结构，不针对程序：
// 函数里只要存在这种「单块直线循环体」，就整体跳过线程化（保守；这类函数通常
// 正是向量化器要接管的热函数）。
static bool il_is_term(IrOp op) {
    switch (op) {
        case IrOp::Jump:
        case IrOp::Branch:
        case IrOp::Return:
        case IrOp::GenYield:
        case IrOp::GenAwait:
        case IrOp::GenRethrow:
        case IrOp::GenDone:
            return true;
        default:
            return false;
    }
}

bool has_straight_line_loop(const IRFunction& fn) {
    const int nb = static_cast<int>(fn.blocks.size());
    for (int h = 0; h < nb; ++h) {
        const auto& hb = fn.blocks[static_cast<size_t>(h)].insts;
        if (hb.empty() || hb.back().op != IrOp::Branch) continue;
        const int cand[2] = {hb.back().target, hb.back().target2};
        for (int ci = 0; ci < 2; ++ci) {
            const int b = cand[ci];
            if (b < 0 || b >= nb || b == h) continue;
            const auto& bi = fn.blocks[static_cast<size_t>(b)].insts;
            if (bi.empty() || bi.back().op != IrOp::Jump || bi.back().target != h) continue;
            bool straight = true;
            for (size_t k = 0; k + 1 < bi.size(); ++k) {
                if (il_is_term(bi[k].op)) {
                    straight = false;
                    break;
                }
            }
            if (straight) return true;
        }
    }
    return false;
}

bool thread_jump_blocks(IRFunction& fn) {
    if (il_has_eh(fn)) return false;
    if (has_straight_line_loop(fn)) return false;
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


}  // namespace optdetail
}  // namespace pyadamas
