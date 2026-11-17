// live_interval.h —— 精确活跃区间（带洞）
//
// **来源**：LLVM 的 LiveIntervals / SlotIndexes。设计动机见
// docs/regalloc-architecture.md。
//
// **要解决的问题**：现有寄存器分配的区间是 [首次访问, 末次访问] 的粗近似，
// 实测（PYADAMAS_RA_LIVENESS_STATS）：
//
//     matmul.run_2          int 精确活跃 5 / 区间重叠 13
//     spectral_norm.main    int 精确活跃 5 / 区间重叠 12
//     nbody.advance_1       int 精确活跃 4 / 区间 6，fp 4 / 5
//
// 区间重叠数是精确活跃数的 2-2.6 倍，这是寄存器压力虚高的根源。
//
// **核心概念**（对齐 LLVM）：
//
//   LiveRange   —— 一个**连续**的活跃段 [start, end)，位置用 Pos 编码。
//   LiveInterval —— 一个变量/槽的全部段，**可以带洞**（不连续）。
//                   LLVM 的例子：[1,20), [50,65), [1000,1001)。
//   VNInfo      —— 值编号。每个段标记「这段里的值是谁定义的」。
//                   同一变量在不同段可能有不同的定义点（phi 合并）。
//
// **位置编码**：沿用现有的 `block * (1<<20) + inst_idx`，但**细化到指令内的
// 读/写点**：
//
//     Pos = (block << 20) | (inst_idx << 1) | (is_def ? 1 : 0)
//
// 为什么需要区分读写点：一条三地址码指令可能同时「读 A、写 A」
// （`s = s + 1`）。如果读写用同一个位置，区间会在指令内部自相交，
// 冲突图会误判。LLVM 用 4 个 slot（Block/EarlyClobber/Register/Dead），
// 但 PyAdamas 的 IR 是纯三地址码，2 个位置（use/def）足够。
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pyadamas {

// 位置编码：**全局指令序号** × 2 + 读写位。
//
// 为什么不用「块号 << 20 | 块内下标」：那样跨块区间的「位置差」会是
// (块差 << 20) 而不是真实指令距离，包络长度/浪费率全部算错
// （实测浪费率虚高到 100%，包络 4.6 亿）。
//
// 全局序号按块顺序累加：base[b] = sum(ni[0..b-1])，块 b 的第 i 条指令
// 序号是 base[b] + i。这样位置差就是真实指令距离，所有长度/重叠计算
// 都直接成立。
//
// 代价：位置不能反推块号。构建器里保存 base[] 即可（见 live_interval_build.cpp）。
inline int64_t pos_make(int64_t global_inst, bool is_def) {
    return (global_inst << 1) | (is_def ? 1 : 0);
}
inline int64_t pos_inst_of(int64_t p) { return p >> 1; }
inline bool pos_is_def(int64_t p) { return (p & 1) != 0; }

// 一个连续活跃段。
struct LiveRange {
    int64_t start = 0;  // 含
    int64_t end = 0;    // 不含
    int vn = 0;         // 值编号（0 = 单一值，>0 = phi 合并后的值）
    bool empty() const { return end <= start; }
    bool contains(int64_t p) const { return p >= start && p < end; }
    bool overlaps(const LiveRange& o) const { return start < o.end && o.start < end; }
};

// 一个变量/槽的完整活跃区间（可带洞）。
//
// 段按 start 升序排列且互不相接（相接的会被合并）。
struct LiveInterval {
    int slot = -1;  // 槽号（-1 表示临时 vreg）
    std::vector<LiveRange> ranges;

    bool empty() const { return ranges.empty(); }

    // 加入一段并保持「升序 + 不重叠 + 相接合并」的不变量。
    void add(int64_t start, int64_t end, int vn = 0);

    // 这个位置是否活跃。
    bool liveAt(int64_t p) const;

    // 区间总长度（所有段长度之和）。
    int64_t totalLength() const;

    // 第一个/最后一个位置（用于快速判定区间包络）。
    int64_t firstPos() const { return ranges.empty() ? 0 : ranges.front().start; }
    int64_t lastPos() const { return ranges.empty() ? 0 : ranges.back().end; }

    // 两个区间是否有重叠（用于冲突判定）。
    //
    // **这是精确判定的核心**：带洞的区间只在真正重叠时才冲突。
    // 粗近似（[first, last]）会把「A 在洞里的位置」也算成冲突。
    bool conflictsWith(const LiveInterval& o) const;

    // 与粗近似 [first, last] 对比：返回被粗近似多算的「假冲突」位置数。
    // 仅用于诊断。
    int64_t wastedByEnvelope() const;
};

// 从 IR 构造所有槽的精确活跃区间。
//
// 返回的 vector 按槽号索引；空区间表示该槽从不活跃（只被 store 过）。
// 实现在 live_interval_build.cpp。
struct IRFunction;
std::vector<LiveInterval> build_precise_intervals(const IRFunction& f);

}  // namespace pyadamas
