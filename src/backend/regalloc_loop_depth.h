// backend/regalloc_loop_depth.h —— 基本块循环嵌套深度（寄存器分配共用工具）
//
// 原为 backend/regalloc.cpp 内的 static 函数；文件拆分后
// backend/regalloc_scan.cpp 与 backend/regalloc_temp.cpp 两处都要用，
// 移到这里改 inline（函数体逐字移动，语义零变化）。
#pragma once

#include <cstdint>
#include <vector>

#include "ir/ir.h"

namespace pyadamas {

// 每个基本块所在的循环嵌套深度：块被多少个自然循环体包含。
//
// 跨块 vreg 提升只看「使用次数」时，循环外只执行一次的常量（循环不变量
// 的装载、2.0/4.0 这类外提常量）会和每轮迭代都要读的内层循环值打成平手，
// 然后靠 vreg 编号小抢走寄存器，把内层循环的热值挤回栈上。按使用点所在
// 块的深度加权后，内层循环的热值优先留在寄存器。
inline std::vector<int> block_loop_depth(const IRFunction& f) {
    const int nb = static_cast<int>(f.blocks.size());
    std::vector<int> depth(static_cast<size_t>(nb), 0);
    if (nb == 0) return depth;

    std::vector<std::vector<int>> succ(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        const auto& insts = f.blocks[static_cast<size_t>(b)].insts;
        auto add = [&](int t) {
            if (t >= 0 && t < nb) succ[static_cast<size_t>(b)].push_back(t);
        };
        if (insts.empty()) {
            if (b + 1 < nb) add(b + 1);
            continue;
        }
        switch (insts.back().op) {
            case IrOp::Jump: add(insts.back().target); break;
            case IrOp::Branch:
                add(insts.back().target);
                add(insts.back().target2);
                break;
            case IrOp::Return:
            case IrOp::GenYield:
            case IrOp::GenAwait:
            case IrOp::GenRethrow:
            case IrOp::GenDone:
                break;
            default:
                if (b + 1 < nb) add(b + 1);
                break;
        }
    }

    // reach[b][c]：b 沿 CFG 走至少一条边能否到达 c。
    std::vector<std::vector<uint8_t>> reach(
        static_cast<size_t>(nb), std::vector<uint8_t>(static_cast<size_t>(nb), 0));
    for (int b = 0; b < nb; ++b) {
        std::vector<int> work(succ[static_cast<size_t>(b)]);
        while (!work.empty()) {
            int c = work.back();
            work.pop_back();
            if (reach[static_cast<size_t>(b)][static_cast<size_t>(c)]) continue;
            reach[static_cast<size_t>(b)][static_cast<size_t>(c)] = 1;
            for (int s : succ[static_cast<size_t>(c)])
                if (!reach[static_cast<size_t>(b)][static_cast<size_t>(s)]) work.push_back(s);
        }
    }

    // 支配集（迭代到不动点），用来识别回边：u→v 且 v 支配 u。
    // 只考虑从入口可达的块，不可达块的支配集没有意义。
    std::vector<char> live(static_cast<size_t>(nb), 0);
    live[0] = 1;
    for (int b = 1; b < nb; ++b) live[static_cast<size_t>(b)] = reach[0][static_cast<size_t>(b)];
    std::vector<std::vector<char>> dom(static_cast<size_t>(nb),
                                       std::vector<char>(static_cast<size_t>(nb), 1));
    dom[0].assign(static_cast<size_t>(nb), 0);
    dom[0][0] = 1;
    std::vector<std::vector<int>> pred(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b)
        for (int s : succ[static_cast<size_t>(b)]) pred[static_cast<size_t>(s)].push_back(b);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < nb; ++b) {
            if (!live[static_cast<size_t>(b)] || pred[static_cast<size_t>(b)].empty()) continue;
            std::vector<char> nd;
            for (int p : pred[static_cast<size_t>(b)]) {
                if (nd.empty()) {
                    nd = dom[static_cast<size_t>(p)];
                    continue;
                }
                for (size_t i = 0; i < nd.size(); ++i)
                    nd[i] = nd[i] && dom[static_cast<size_t>(p)][i];
            }
            nd[static_cast<size_t>(b)] = 1;
            if (nd != dom[static_cast<size_t>(b)]) {
                dom[static_cast<size_t>(b)] = std::move(nd);
                changed = true;
            }
        }
    }
    std::vector<char> header(static_cast<size_t>(nb), 0);
    for (int u = 0; u < nb; ++u) {
        if (!live[static_cast<size_t>(u)]) continue;
        for (int v : succ[static_cast<size_t>(u)])
            if (live[static_cast<size_t>(v)] && dom[static_cast<size_t>(u)][static_cast<size_t>(v)])
                header[static_cast<size_t>(v)] = 1;
    }

    // 循环头 v 的自然循环体：v 支配、且能回到 v 的块。只用「双向可达」
    // 会误判：内层循环的块能退出到外层、再从外层绕回来，看着和内层循环
    // 头互相可达，其实不在同一个自然循环里。
    for (int v = 0; v < nb; ++v) {
        if (!header[static_cast<size_t>(v)]) continue;
        for (int b = 0; b < nb; ++b)
            if (b == v || (dom[static_cast<size_t>(b)][static_cast<size_t>(v)] &&
                           reach[static_cast<size_t>(b)][static_cast<size_t>(v)]))
                ++depth[static_cast<size_t>(b)];
    }
    return depth;
}

}  // namespace pyadamas
