// value_info.h —— 值（vreg）的类型与来源查询（VectPlanner 抽取阶段 2）
//
// 向量化器需要三件事：这个 vreg 是什么类型、它的操作数在哪里、它是哪一个 list。
// 实测这些查询占向量化器对 AsmGen 依赖的约四成（ty_of 25 次、list_base_mop 15 次、
// src_mop 6 次、global_mop 1 次），所以单独成接口，不塞进 EmitSink。
#pragma once

#include <cstdint>

#include "ir/ir.h"
#include "backend/mach_instr.h"

namespace pyadamas {

class ValueQuery {
public:
    virtual ~ValueQuery() = default;

    // vreg 的静态类型（可能为空：unknown）。
    virtual TypePtr ty_of(VReg r) const = 0;
    // 全局 #i 的操作数。
    virtual const MOperand& global_mop(int i) const = 0;
    // vreg 的定义指令（按定义块口径）。向量化器用它判断一个值来自哪条指令
    // （仿射下标解析、j 物化、归约识别都依赖它），实测用量 14 次。
    virtual const Inst* simd_def(VReg v) const = 0;
};

}  // namespace pyadamas
