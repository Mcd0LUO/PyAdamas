// slot_info.h —— 命名栈槽的查询（VectPlanner 抽取阶段 2）
//
// 向量化器要判「这个槽是不是归纳变量的别名 / 是不是临时值 / 是不是归约累加器」，
// 还要拿槽的操作数与位置。这些是 SlotInfo 的职责，与 EmitSink（只管发指令）无关。
#pragma once

#include <cstdint>

#include "backend/mach_instr.h"

namespace pyadamas {

class SlotQuery {
public:
    virtual ~SlotQuery() = default;

    // 命名槽的栈位置操作数（已被提升到寄存器时返回寄存器操作数）。
    virtual const MOperand& slot_mop(int slot) const = 0;
};

}  // namespace pyadamas
