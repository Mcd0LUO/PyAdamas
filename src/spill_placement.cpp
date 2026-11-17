// spill_placement.cpp —— 最优 spill 边界选择（Hopfield 网络）
//
// 设计说明见 spill_placement.h 与 docs/regalloc-architecture.md。
#include "spill_placement.h"

#include <algorithm>
#include <unordered_map>

namespace pyadamas {

namespace {

// Hopfield 迭代的收敛上限。LLVM 用的也是固定上限（不依赖不动点证明），
// 因为 Lyapunov 函数单调下降，实际通常 3-5 轮就稳定。
constexpr int kMaxIterations = 32;

// 死区阈值：|SumP - SumN| 小于它时判为「无所谓」（value = 0）。
//
// LLVM 用 BlockFrequency 的精度单位做阈值；这里用「最大块频率的 1/64」，
// 效果等价：避免在权重接近时来回抖动，也避免全零链接时的任意偏置。
int64_t deadZone(const std::vector<int64_t>& freq) {
    int64_t mx = 0;
    for (int64_t f : freq) mx = std::max(mx, f);
    return mx > 0 ? std::max<int64_t>(1, mx / 64) : 1;
}

}  // namespace

void SpillPlacement::Node::clear() {
    bias_n = 0;
    bias_p = 0;
    value = 0;
    sum_link_w = 0;
    links.clear();
}

void SpillPlacement::Node::addBias(int64_t freq, BlockPref pref) {
    switch (pref) {
        case BlockPref::PrefReg: bias_p += freq; break;
        case BlockPref::PrefSpill: bias_n += freq; break;
        case BlockPref::MustSpill:
            // 饱和到最大值：mustSpill() 的比较依赖 bias_n >= bias_p + sum_link_w。
            bias_n = INT64_MAX / 4;
            break;
        case BlockPref::DontCare: break;
    }
}

void SpillPlacement::Node::addLink(int b, int64_t w) {
    sum_link_w += w;
    for (auto& l : links)
        if (l.second == b) {
            l.first += w;
            return;
        }
    links.emplace_back(w, b);
}

bool SpillPlacement::Node::update(const std::vector<Node>& nodes, int64_t threshold) {
    int64_t sum_n = bias_n;
    int64_t sum_p = bias_p;
    for (const auto& l : links) {
        const int v = nodes[static_cast<size_t>(l.second)].value;
        if (v == -1) sum_n += l.first;
        else if (v == 1) sum_p += l.first;
    }
    const bool before = preferReg();
    // 死区：只有明显偏向一侧才改值，避免抖动。
    if (sum_n >= sum_p + threshold) value = -1;
    else if (sum_p >= sum_n + threshold) value = 1;
    else value = 0;
    return before != preferReg();
}

SpillPlacement::SpillPlacement(const std::vector<std::vector<int>>& succ,
                               const std::vector<int64_t>& freq)
    : succ_(succ), freq_(freq) {
    const int nb = static_cast<int>(succ.size());
    // **EdgeBundles**：所有进入块 b 的边归入「入捆」，所有离开块 b 的边
    // 归入「出捆」。于是捆号 = 2*b（入）/ 2*b+1（出）。
    //
    // 这是 LLVM 的关键抽象：把 O(边数) 的状态压缩成 O(块数) 个节点。
    // 一个变量在某个边界上是「在寄存器」还是「在栈」，只需要一个比特，
    // 而这个比特对「所有进入同一个块的边」是同一个值（因为块内要么用
    // 寄存器要么不用，不分入口）。
    bundle_of_.assign(static_cast<size_t>(nb) * 2, -1);
    nodes_.assign(static_cast<size_t>(nb) * 2, Node{});
    active_.assign(static_cast<size_t>(nb) * 2, 0);
    if (freq_.size() < static_cast<size_t>(nb)) freq_.resize(static_cast<size_t>(nb), 1);
    for (int b = 0; b < nb; ++b) {
        bundle_of_[static_cast<size_t>(b) * 2] = b * 2;
        bundle_of_[static_cast<size_t>(b) * 2 + 1] = b * 2 + 1;
    }
}

bool SpillPlacement::solve(const std::vector<BlockConstraint>& constraints) {
    if (nodes_.empty()) return false;
    const int nb = static_cast<int>(succ_.size());
    std::fill(active_.begin(), active_.end(), 0);
    for (auto& n : nodes_) n.clear();

    // 1) 施加块约束为偏置。
    for (const auto& c : constraints) {
        if (c.block < 0 || c.block >= nb) continue;
        const int64_t f = freq_[static_cast<size_t>(c.block)];
        const int ib = bundle_of_[static_cast<size_t>(c.block) * 2];
        const int ob = bundle_of_[static_cast<size_t>(c.block) * 2 + 1];
        if (c.entry != BlockPref::DontCare) {
            active_[static_cast<size_t>(ib)] = 1;
            nodes_[static_cast<size_t>(ib)].addBias(f, c.entry);
        }
        if (c.exit != BlockPref::DontCare) {
            active_[static_cast<size_t>(ob)] = 1;
            nodes_[static_cast<size_t>(ob)].addBias(f, c.exit);
        }
    }

    // 2) 建链接：块 b 的出捆与每个后继的入捆相连。
    //
    // 权重用「两块频率的较小值」—— 变量从 b 流向 succ 时，这条边的实际
    // 执行次数受两端共同限制。LLVM 用的是 edge frequency（需要 profile），
    // 这里用保守估计。
    for (int b = 0; b < nb; ++b) {
        const int ob = bundle_of_[static_cast<size_t>(b) * 2 + 1];
        for (int s : succ_[static_cast<size_t>(b)]) {
            if (s < 0 || s >= nb) continue;
            const int is = bundle_of_[static_cast<size_t>(s) * 2];
            if (ob == is) continue;  // 自环
            const int64_t w = std::min(freq_[static_cast<size_t>(b)],
                                       freq_[static_cast<size_t>(s)]);
            active_[static_cast<size_t>(ob)] = 1;
            active_[static_cast<size_t>(is)] = 1;
            nodes_[static_cast<size_t>(ob)].addLink(is, w);
            nodes_[static_cast<size_t>(is)].addLink(ob, w);
        }
    }

    // 3) 迭代到收敛。
    //
    // 初值：所有活跃节点取 +1（倾向寄存器）。这是保守的起点 ——
    // spill 的代价是运行时内存访问，而寄存器不够时迭代会自己把
    // 收益低的节点翻成 -1。
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (active_[i]) nodes_[i].value = 1;

    const int64_t th = deadZone(freq_);
    for (int it = 0; it < kMaxIterations; ++it) {
        bool changed = false;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (!active_[i]) continue;
            // mustSpill 的节点不会变，跳过（LLVM 也这么做）。
            if (nodes_[i].mustSpill()) {
                nodes_[i].value = -1;
                continue;
            }
            if (nodes_[i].update(nodes_, th)) changed = true;
        }
        if (!changed) break;
    }

    // 4) 判断「这个变量值不值得用寄存器」。
    //
    // 如果有任何一个捆最后判为 +1，说明至少有一段值得放寄存器。
    // 全 -1 或全 0 则整条区间都该留在栈上。
    bool any_reg = false;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (active_[i] && nodes_[i].preferReg()) {
            any_reg = true;
            break;
        }
    return any_reg;
}

bool SpillPlacement::useRegAtEntry(int b) const {
    if (b < 0 || static_cast<size_t>(b) * 2 >= nodes_.size()) return false;
    const size_t i = static_cast<size_t>(b) * 2;
    return active_[i] && nodes_[i].preferReg();
}

bool SpillPlacement::useRegAtExit(int b) const {
    if (b < 0 || static_cast<size_t>(b) * 2 + 1 >= nodes_.size()) return false;
    const size_t i = static_cast<size_t>(b) * 2 + 1;
    return active_[i] && nodes_[i].preferReg();
}

}  // namespace pyadamas
