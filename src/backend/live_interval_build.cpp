// backend/live_interval_build.cpp —— 从 IR 构造精确活跃区间
//
// **算法**：标准的数据流活跃性 + 逐指令反向扫描。
//
// 与现有 build_slot_liveness() 的区别：
//   - 现有：块级位图（每块一个 bitset），块内无位置信息；
//   - 这里：精确到「指令的读点/写点」，能表达洞。
//
// **位置编码**：全局指令序号 × 2 + is_def。全局序号按块顺序累加，
// 所以位置差 = 真实指令距离（这是包络长度/冲突判定正确的前提）。
#include "backend/live_interval.h"

#include <unordered_map>

#include "ir/types.h"

namespace pyadamas {

namespace {

struct BlockLiveness {
    int nblocks = 0;
    int nslots = 0;
    int words = 0;
    std::vector<std::vector<uint64_t>> live_in;
    std::vector<std::vector<uint64_t>> live_out;
};

std::vector<std::vector<int>> build_succ(const IRFunction& f) {
    const int nb = static_cast<int>(f.blocks.size());
    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    auto add = [&](int b, int t) {
        if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
    };
    for (int b = 0; b < nb; ++b) {
        const auto& insts = f.blocks[static_cast<size_t>(b)].insts;
        if (insts.empty()) { add(b, b + 1); continue; }
        switch (insts.back().op) {
            case IrOp::Jump: add(b, insts.back().target); break;
            case IrOp::Branch:
                add(b, insts.back().target);
                add(b, insts.back().target2);
                break;
            case IrOp::Return:
            case IrOp::GenYield:
            case IrOp::GenAwait:
            case IrOp::GenRethrow:
            case IrOp::GenDone:
                break;
            default: add(b, b + 1); break;
        }
    }
    return succ;
}

BlockLiveness compute_block_liveness(const IRFunction& f) {
    BlockLiveness L;
    L.nblocks = static_cast<int>(f.blocks.size());
    L.nslots = static_cast<int>(f.slot_ty.size());
    if (L.nblocks == 0 || L.nslots == 0) return L;
    L.words = (L.nslots + 63) / 64;

    std::vector<std::vector<uint64_t>> use(static_cast<size_t>(L.nblocks)),
        def(static_cast<size_t>(L.nblocks));
    for (int b = 0; b < L.nblocks; ++b) {
        use[static_cast<size_t>(b)].assign(static_cast<size_t>(L.words), 0);
        def[static_cast<size_t>(b)].assign(static_cast<size_t>(L.words), 0);
        for (const auto& in : f.blocks[static_cast<size_t>(b)].insts) {
            if (in.idx < 0 || in.idx >= L.nslots) continue;
            const size_t w = static_cast<size_t>(in.idx >> 6);
            const uint64_t bit = 1ULL << (in.idx & 63);
            if (in.op == IrOp::LoadSlot) {
                if (!(def[static_cast<size_t>(b)][w] & bit))
                    use[static_cast<size_t>(b)][w] |= bit;
            } else if (in.op == IrOp::StoreSlot) {
                def[static_cast<size_t>(b)][w] |= bit;
            }
        }
    }

    auto succ = build_succ(f);
    L.live_in.assign(static_cast<size_t>(L.nblocks),
                     std::vector<uint64_t>(static_cast<size_t>(L.words), 0));
    L.live_out.assign(static_cast<size_t>(L.nblocks),
                      std::vector<uint64_t>(static_cast<size_t>(L.words), 0));

    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = L.nblocks - 1; b >= 0; --b) {
            std::vector<uint64_t> out(static_cast<size_t>(L.words), 0);
            for (int s : succ[static_cast<size_t>(b)])
                for (int w = 0; w < L.words; ++w)
                    out[static_cast<size_t>(w)] |=
                        L.live_in[static_cast<size_t>(s)][static_cast<size_t>(w)];
            std::vector<uint64_t> in(static_cast<size_t>(L.words), 0);
            for (int w = 0; w < L.words; ++w)
                in[static_cast<size_t>(w)] =
                    use[static_cast<size_t>(b)][static_cast<size_t>(w)] |
                    (out[static_cast<size_t>(w)] &
                     ~def[static_cast<size_t>(b)][static_cast<size_t>(w)]);
            if (out != L.live_out[static_cast<size_t>(b)] ||
                in != L.live_in[static_cast<size_t>(b)]) {
                L.live_out[static_cast<size_t>(b)] = std::move(out);
                L.live_in[static_cast<size_t>(b)] = std::move(in);
                changed = true;
            }
        }
    }
    return L;
}

}  // namespace

std::vector<LiveInterval> build_precise_intervals(const IRFunction& f) {
    std::vector<LiveInterval> out(f.slot_ty.size());
    for (size_t s = 0; s < out.size(); ++s) out[s].slot = static_cast<int>(s);
    const BlockLiveness L = compute_block_liveness(f);
    if (L.nblocks == 0 || L.nslots == 0) return out;

    // 全局指令序号：base[b] = 前面所有块的指令总数。
    std::vector<int64_t> base(static_cast<size_t>(L.nblocks), 0);
    int64_t acc = 0;
    for (int b = 0; b < L.nblocks; ++b) {
        base[static_cast<size_t>(b)] = acc;
        acc += static_cast<int64_t>(f.blocks[static_cast<size_t>(b)].insts.size());
    }
    const int64_t total_insts = acc;

    for (int b = 0; b < L.nblocks; ++b) {
        const auto& insts = f.blocks[static_cast<size_t>(b)].insts;
        const int ni = static_cast<int>(insts.size());
        if (ni == 0) continue;

        // 反向扫描：live 从块出口开始。
        std::vector<uint64_t> live = L.live_out[static_cast<size_t>(b)];

        // 块内最后一条指令之后的「块尾位置」：用下一条全局指令的 use 位。
        // 如果这是最后一块，用 total_insts。
        const int64_t blk_end_inst =
            b + 1 < L.nblocks ? base[static_cast<size_t>(b + 1)] : total_insts;

        for (int i = ni - 1; i >= 0; --i) {
            const auto& in = insts[static_cast<size_t>(i)];
            const int64_t gi = base[static_cast<size_t>(b)] + i;
            const int64_t use_p = pos_make(gi, false);
            const int64_t def_p = pos_make(gi, true);

            if (in.idx >= 0 && in.idx < L.nslots &&
                (in.op == IrOp::LoadSlot || in.op == IrOp::StoreSlot)) {
                out[static_cast<size_t>(in.idx)].add(use_p, def_p);
            }

            // 当前 live 集里所有槽，在这条指令的 use 点都活跃。
            for (int s = 0; s < L.nslots; ++s) {
                const size_t w = static_cast<size_t>(s >> 6);
                if (!(live[w] & (1ULL << (s & 63)))) continue;
                out[static_cast<size_t>(s)].add(use_p, def_p);
            }

            if (in.idx >= 0 && in.idx < L.nslots) {
                const size_t w = static_cast<size_t>(in.idx >> 6);
                const uint64_t bit = 1ULL << (in.idx & 63);
                if (in.op == IrOp::LoadSlot) live[w] |= bit;
                else if (in.op == IrOp::StoreSlot) live[w] &= ~bit;
            }
        }

        // 块入口 live-in：从块首延伸到第一条指令。
        if (ni > 0) {
            const int64_t first_p = pos_make(base[static_cast<size_t>(b)], false);
            for (int s = 0; s < L.nslots; ++s) {
                const size_t w = static_cast<size_t>(s >> 6);
                if (!(L.live_in[static_cast<size_t>(b)][w] & (1ULL << (s & 63)))) continue;
                out[static_cast<size_t>(s)].add(first_p, pos_make(base[static_cast<size_t>(b)], true));
            }
        }

        // 块出口 live-out：从最后一条指令延伸到块尾。
        if (ni > 0 && blk_end_inst > base[static_cast<size_t>(b)]) {
            const int64_t last_p = pos_make(blk_end_inst - 1, true);
            for (int s = 0; s < L.nslots; ++s) {
                const size_t w = static_cast<size_t>(s >> 6);
                if (!(L.live_out[static_cast<size_t>(b)][w] & (1ULL << (s & 63)))) continue;
                out[static_cast<size_t>(s)].add(last_p, pos_make(blk_end_inst, false));
            }
        }
    }
    return out;
}

}  // namespace pyadamas
