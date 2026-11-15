// opt_forward.cpp —— 槽位转发、分支线程化与不可达块清理
//
// `and` / `or` 的短路求值会把布尔结果写进临时槽，再由下一个块装载后分支：
//
//   store.slot s, %c
//   %d = load.slot s
//   branch %d
//
// 生成代码因此每轮要做 setcc / movzbq / 写槽 / 读槽 / cmpq 一整套。
// 这里在 IR 上把已知的槽值转发到使用点，并把「跳到只做 load+branch 的块」
// 改写成直接分支，让后端已有的 cmp+branch 融合生效；两个分支都改完后，
// 没人再读的槽写入会被删掉。
#include "opt_detail.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace pyadamas {
namespace optdetail {

namespace {

void replace_uses(Inst& in, VReg from, VReg to) {
    if (in.a == from) in.a = to;
    if (in.b == from) in.b = to;
    if (in.c == from) in.c = to;
    for (VReg& r : in.args)
        if (r == from) r = to;
}

struct Forward {
    size_t pos;
    VReg from;
    VReg to;
};

// 块内 store → load 转发。
void forward_within_blocks(IRFunction& fn, const std::unordered_set<int>& addr_taken) {
    const size_t nb = fn.blocks.size();
    std::unordered_map<VReg, std::vector<int>> use_blocks;
    for (size_t bi = 0; bi < nb; ++bi) {
        for (const auto& in : fn.blocks[bi].insts) {
            auto note = [&](VReg r) {
                if (r != kNoReg) use_blocks[r].push_back(static_cast<int>(bi));
            };
            note(in.a);
            note(in.b);
            note(in.c);
            for (VReg r : in.args) note(r);
        }
    }
    auto local_only = [&](VReg v, size_t bi) {
        auto it = use_blocks.find(v);
        if (it == use_blocks.end() || it->second.empty()) return false;
        for (int b : it->second)
            if (b != static_cast<int>(bi)) return false;
        return true;
    };

    for (size_t bi = 0; bi < nb; ++bi) {
        auto& insts = fn.blocks[bi].insts;
        std::unordered_map<int, VReg> slot_val;
        std::unordered_map<VReg, VReg> resolve;
        std::vector<Forward> fwds;
        auto resolve_v = [&](VReg v) {
            for (int guard = 0; guard < 64; ++guard) {
                auto it = resolve.find(v);
                if (it == resolve.end()) break;
                v = it->second;
            }
            return v;
        };
        for (size_t i = 0; i < insts.size(); ++i) {
            const Inst& in = insts[i];
            if (in.op == IrOp::StoreSlot && in.idx >= 0 && !addr_taken.contains(in.idx)) {
                slot_val[in.idx] = resolve_v(in.a);
            } else if (in.op == IrOp::SlotAddr && in.idx >= 0) {
                slot_val.erase(in.idx);
            } else if (in.op == IrOp::LoadSlot && in.dst != kNoReg) {
                auto it = slot_val.find(in.idx);
                if (it == slot_val.end() || it->second == kNoReg) continue;
                if (!local_only(in.dst, bi)) continue;
                VReg to = resolve_v(it->second);
                if (to == in.dst) continue;
                // 只转发类型一致的值：槽里可能存过另一种类型的机器字
                // （例如用 0 清空函数槽），换掉会让后续指令按错误的
                // 寄存器类/所有权规则处理。
                TypePtr dt = in.dst < fn.vreg_ty.size() ? fn.vreg_ty[in.dst] : nullptr;
                TypePtr st = to < fn.vreg_ty.size() ? fn.vreg_ty[to] : nullptr;
                if (!dt || !st || !type_eq(dt, st)) continue;
                // 只转发布尔临时槽：目的就是让 `and`/`or` 的短路结果直接
                // 进分支。转发浮点/堆槽会延长活跃区间，在含调用的热循环里
                // 增加跨调用溢出（nbody 实测退 20%）。
                if (dt->kind != TyKind::Bool) continue;
                resolve[in.dst] = to;
                fwds.push_back(Forward{i, in.dst, to});
            }
        }
        if (fwds.empty()) continue;
        for (const auto& f : fwds)
            for (size_t j = f.pos + 1; j < insts.size(); ++j)
                replace_uses(insts[j], f.from, f.to);
        for (auto it = fwds.rbegin(); it != fwds.rend(); ++it)
            insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(it->pos));
    }
}

// 分支线程化：C 只做 load.slot + branch，且装载值只在这个块里用；
// P 在跳转/分支之前刚把同一个值写进同一个槽时，可以直接按该值分支。
void thread_branches(IRFunction& fn) {
    const size_t nb = fn.blocks.size();
    std::unordered_map<VReg, int> use_count;
    std::unordered_map<VReg, int> first_block;
    std::unordered_map<VReg, int> last_block;
    for (size_t bi = 0; bi < nb; ++bi) {
        for (const auto& in : fn.blocks[bi].insts) {
            auto note = [&](VReg r) {
                if (r == kNoReg) return;
                ++use_count[r];
                if (!first_block.contains(r)) first_block[r] = static_cast<int>(bi);
                last_block[r] = static_cast<int>(bi);
            };
            note(in.a);
            note(in.b);
            note(in.c);
            for (VReg r : in.args) note(r);
        }
    }
    auto local_only = [&](VReg v, size_t bi) {
        return use_count[v] > 0 && first_block[v] == static_cast<int>(bi) &&
               last_block[v] == static_cast<int>(bi);
    };

    struct CInfo {
        int slot;
        int t1;
        int t2;
    };
    std::vector<char> has_info(nb, 0);
    std::vector<CInfo> info(nb);
    for (size_t ci = 0; ci < nb; ++ci) {
        const auto& insts = fn.blocks[ci].insts;
        if (insts.size() != 2) continue;
        if (insts[0].op != IrOp::LoadSlot || insts[1].op != IrOp::Branch) continue;
        if (insts[1].a != insts[0].dst || insts[0].dst == kNoReg) continue;
        if (!local_only(insts[0].dst, ci)) continue;
        if (insts[1].target < 0 || insts[1].target2 < 0) continue;
        has_info[ci] = 1;
        info[ci] = CInfo{insts[0].idx, insts[1].target, insts[1].target2};
    }

    for (size_t pi = 0; pi < nb; ++pi) {
        auto& insts = fn.blocks[pi].insts;
        if (insts.size() < 2) continue;
        Inst& last = insts.back();
        const Inst& prev = insts[insts.size() - 2];
        if (prev.op != IrOp::StoreSlot || prev.idx < 0 || prev.a == kNoReg) continue;

        if (last.op == IrOp::Jump && last.target >= 0 &&
            last.target < static_cast<int>(nb) &&
            has_info[static_cast<size_t>(last.target)]) {
            const CInfo& ci = info[static_cast<size_t>(last.target)];
            if (prev.idx != ci.slot) continue;
            Inst br;
            br.op = IrOp::Branch;
            br.a = prev.a;
            br.target = ci.t1;
            br.target2 = ci.t2;
            last = br;
            continue;
        }

        if (last.op == IrOp::Branch && prev.a == last.a) {
            if (last.target >= 0 && last.target < static_cast<int>(nb) &&
                has_info[static_cast<size_t>(last.target)] &&
                prev.idx == info[static_cast<size_t>(last.target)].slot)
                last.target = info[static_cast<size_t>(last.target)].t1;
            if (last.target2 >= 0 && last.target2 < static_cast<int>(nb) &&
                has_info[static_cast<size_t>(last.target2)] &&
                prev.idx == info[static_cast<size_t>(last.target2)].slot)
                last.target2 = info[static_cast<size_t>(last.target2)].t2;
        }
    }
}

// 删掉从入口不可达的块，避免死块里的 load 让死槽清理不敢动手。
void prune_unreachable(IRFunction& fn) {
    const size_t nb = fn.blocks.size();
    std::vector<char> reach(nb, 0);
    std::vector<int> work{0};
    reach[0] = 1;
    // 零开销异常（--zero-cost-exc）：landing pad 块只有异常边，CFG 上没有
    // 任何前驱；但它是 codegen 要发 pa_eh_land / except 分发的地方，绝不能
    // 当死块删掉。这里按标记指令认块（而不是 IRFunction::eh_regions 里记的
    // 块号）：内联等 pass 会重建块表，块号会过期，指令跟着块走才是稳的。
    for (int b = 0; b < static_cast<int>(nb); ++b)
        for (const auto& in : fn.blocks[static_cast<size_t>(b)].insts)
            if (in.op == IrOp::EhLandingPad) {
                if (!reach[static_cast<size_t>(b)]) {
                    reach[static_cast<size_t>(b)] = 1;
                    work.push_back(b);
                }
                break;
            }
    while (!work.empty()) {
        int bi = work.back();
        work.pop_back();
        const auto& insts = fn.blocks[static_cast<size_t>(bi)].insts;
        if (insts.empty()) continue;
        const Inst& last = insts.back();
        auto visit = [&](int t) {
            if (t >= 0 && t < static_cast<int>(nb) && !reach[static_cast<size_t>(t)]) {
                reach[static_cast<size_t>(t)] = 1;
                work.push_back(t);
            }
        };
        if (last.op == IrOp::Jump) {
            visit(last.target);
        } else if (last.op == IrOp::Branch) {
            visit(last.target);
            visit(last.target2);
        }
    }
    if (!std::any_of(reach.begin(), reach.end(), [](char c) { return !c; })) return;
    std::vector<int> remap(nb, -1);
    std::vector<Block> kept;
    for (size_t i = 0; i < nb; ++i) {
        if (!reach[i]) continue;
        remap[i] = static_cast<int>(kept.size());
        kept.push_back(std::move(fn.blocks[i]));
    }
    fn.blocks = std::move(kept);
    for (auto& b : fn.blocks)
        for (auto& in : b.insts) {
            if (in.target >= 0 && in.target < static_cast<int>(nb))
                in.target = remap[static_cast<size_t>(in.target)];
            if (in.target2 >= 0 && in.target2 < static_cast<int>(nb))
                in.target2 = remap[static_cast<size_t>(in.target2)];
        }
}

// 死槽写入：既没有 load 也没有取地址的槽，写入没有读者。
void drop_dead_slot_stores(IRFunction& fn) {
    std::unordered_set<int> live_slots;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts) {
            if ((in.op == IrOp::LoadSlot || in.op == IrOp::SlotAddr) && in.idx >= 0)
                live_slots.insert(in.idx);
        }
    for (auto& b : fn.blocks) {
        b.insts.erase(
            std::remove_if(b.insts.begin(), b.insts.end(),
                           [&](const Inst& in) {
                               return in.op == IrOp::StoreSlot && in.idx >= 0 &&
                                      !live_slots.contains(in.idx);
                           }),
            b.insts.end());
    }
}

}  // namespace

void forward_slot_loads(IRFunction& fn) {
    if (fn.blocks.empty()) return;

    std::unordered_set<int> addr_taken;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::SlotAddr && in.idx >= 0) addr_taken.insert(in.idx);

    forward_within_blocks(fn, addr_taken);
    thread_branches(fn);
    prune_unreachable(fn);
    drop_dead_slot_stores(fn);
}

}  // namespace optdetail
}  // namespace pyadamas
