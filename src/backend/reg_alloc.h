// backend/reg_alloc.h —— 寄存器/槽提升状态（从 AsmGen 拆出的第四个协作对象）
//
// 这批字段回答同一个问题：「哪些命名槽或跨块 vreg 常驻在寄存器里，栈槽作废」。
// 分配是函数级的、固定的：不做活跃性分析，一个变量要么整函数在寄存器里，要么
// 整函数在栈上，规则简单到不会出隐蔽 bug。
//
// 边界同前三块：只搬状态，分配算法（assign_* / color_slot_pool / linear_scan_pool /
// slot_range_has_call 等）留在 AsmGen，读 ra_.*。
#pragma once

#include <cstdint>
#include <vector>

#include "backend/mach_instr.h"

namespace pyadamas {

class RegAlloc {
public:
    // ── 槽提升 ────────────────────────────────────────────────────
    // slot_reg[s] 非空表示变量 s 常驻在这个寄存器里，栈槽作废。
    // 结构化单元：empty()=未提升，text=寄存器名。**在每条指令的操作数取值路径上**，
    // 所以这里只是数据，查询仍然是 AsmGen::slot_mop 的一次下标访问。
    std::vector<MOperand> slot_reg;
    // 提升到 xmm8–15 的浮点槽。call 会破坏 caller-saved xmm，调用前后只把
    // 「调用后仍存活」的槽写回/读出（块级活跃性按指令取 live-after 掩码）。
    std::vector<int> fp_promoted_slots;
    // 提升到 xmm 的跨块 vreg（临时值，不是命名槽）。槽用 smem(slot) 做栈副本，
    // vreg 用 vreg_mem_[v]；两者在调用点都要 spill/reload —— SysV 下 xmm8–15
    // 是 caller-saved。
    std::vector<int> fp_promoted_vregs;
    std::vector<int> slot_fpidx;      // slot -> fp_promoted_slots 下标
    // 块级活跃掩码：**每个提升槽一个字节**（不是位图）。
    // 位图会隐含「提升槽 ≤ 8 个」的假设，而 R4 的线性扫描会按活跃区间复用寄存器
    // （13 个浮点槽跑在 8 个 xmm 上是常态），位图第 8 位之后静默丢失，调用点不
    // spill 被调方要踩的槽（0.322 的 N19b 余半：amortize 的 pay 读成 0）。
    std::vector<std::vector<uint8_t>> fp_live_in;   // [块][提升槽]
    std::vector<std::vector<uint8_t>> fp_live_out;

    // ── 分配候选与 spill 掩码 ─────────────────────────────────────
    std::vector<int> slot_reg_cand;        // 参与分配的槽号（升序）
    std::vector<char> slot_cand_is_fp;     // 与 slot_reg_cand 对齐：是否浮点槽
    std::vector<int> slot_reg_cand_idx;    // 槽号 -> slot_reg_cand 下标，-1 = 不参与
    std::vector<char> fp_spill_mask;       // 当前指令需要保存的槽
    bool fp_mask_valid = false;
    bool fp_live_off = false;  // PYADAMAS_NO_FPLIVE：退回全量 spill（A/B 用）

    void reset() { *this = RegAlloc{}; }
};

}  // namespace pyadamas
