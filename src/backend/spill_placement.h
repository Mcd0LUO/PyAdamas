// backend/spill_placement.h —— 最优 spill 边界选择（Hopfield 网络）
//
// **来源**：LLVM 的 SpillPlacement.cpp。设计动机见
// docs/regalloc-architecture.md。
//
// **要解决的问题**：0.262 的 live range splitting 是「每个基本块独立决定
// 用不用寄存器」，没有任何代价模型 —— 结果 nbody 慢 56ms、spectral_norm
// 慢 56ms。LLVM 的做法是先算「在哪 spill 最划算」，再切。
//
// **核心思想**：把 CFG 的边做等价类（EdgeBundles），每个捆一个节点，
// 节点值 ∈ {-1, 0, +1} 表示「变量在这个边界上应在栈 / 无所谓 / 寄存器」。
// 用 Hopfield 网络迭代求能量最小：
//
//     E = -sum_n V_n * ( B_n + sum_{n,m linked by b} V_m * F_b )
//
// 能量就是「预期 spill 代码执行频率」。节点偏置 B_n 来自块内的读写次数，
// 链接 F_b 来自「变量穿过但不使用」的块（transparent block），
// 权重是块执行频率。
//
// **为什么用 Hopfield 而不是贪心**：一个块的决策会影响邻居 ——
// 如果相邻块都决定把变量放寄存器，那本块跟着放就省掉两次 spill/reload；
// 反之如果邻居都 spill，本块单独放寄存器反而多付边界搬运。贪心看不到
// 这种耦合，Hopfield 迭代能收敛到局部最优。
//
// **复杂度**：O(捆数 × 迭代次数)。捆数 ≤ 2 × 块数，迭代通常 < 10 轮。
#pragma once

#include <cstdint>
#include <vector>

namespace pyadamas {

// 块约束：某个块对「变量在此处放寄存器」的倾向。
enum class BlockPref : uint8_t {
    DontCare = 0,  // 变量不活跃或不需要寄存器
    PrefReg = 1,   // 块内读多写少，倾向寄存器
    PrefSpill = 2, // 块内写多读少，倾向栈
    MustSpill = 3, // 必须 spill（寄存器池耗尽等硬约束）
};

struct BlockConstraint {
    int block = -1;
    BlockPref entry = BlockPref::DontCare;
    BlockPref exit = BlockPref::DontCare;
};

// 最优 spill 边界求解器。
//
// 用法：
//   1. 构造时传入 CFG（后继表 + 块频率）；
//   2. 对每个要分配的变量，调用 solve() 传入它的块约束；
//   3. 用 shouldUseReg(block, is_exit) 查询该块边界上是否用寄存器。
class SpillPlacement {
public:
    // succ: 每个块的后继列表（越界/负值自动忽略）
    // freq: 每个块的执行频率权重（1 = 冷块，越大越热）
    SpillPlacement(const std::vector<std::vector<int>>& succ,
                   const std::vector<int64_t>& freq);

    // 求解一个变量的最优边界。constraints 里的块必须是合法块号。
    // 返回 false 表示该变量不值得用寄存器（全 spill）。
    bool solve(const std::vector<BlockConstraint>& constraints);

    // 查询：块 b 的入口/出口边界上，该变量是否应该在寄存器里。
    bool useRegAtEntry(int b) const;
    bool useRegAtExit(int b) const;

    // 诊断：返回参与求解的捆数（用于验证是否真的在做事）。
    int numBundles() const { return static_cast<int>(bundle_of_.size()) / 2; }

private:
    struct Node {
        int64_t bias_n = 0;  // 倾向 spill 的权重和
        int64_t bias_p = 0;  // 倾向寄存器的权重和
        int64_t sum_link_w = 0;
        int value = 0;  // -1 spill / 0 无所谓 / 1 寄存器
        std::vector<std::pair<int64_t, int>> links;  // (权重, 邻居捆号)
        void clear();
        void addBias(int64_t freq, BlockPref pref);
        void addLink(int b, int64_t w);
        bool update(const std::vector<Node>& nodes, int64_t threshold);
        bool preferReg() const { return value > 0; }
        bool mustSpill() const { return bias_n >= bias_p + sum_link_w; }
    };

    std::vector<std::vector<int>> succ_;
    std::vector<int64_t> freq_;
    // bundle_of_[2*b] = 块 b 的入捆，bundle_of_[2*b+1] = 出捆。
    std::vector<int> bundle_of_;
    std::vector<Node> nodes_;
    std::vector<char> active_;
};

}  // namespace pyadamas
