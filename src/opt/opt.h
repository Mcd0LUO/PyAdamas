// opt/opt.h —— IR 优化遍入口
#pragma once

#include "ir/ir.h"

namespace pyadamas {

// opt_level >= 2 时运行常量折叠/分支化简/DCE。
// loop_opt：整数循环优化位掩码（PYADAMAS_LOOP_OPT，默认 0 全关）。
// bit0=跳转线程化 bit1=仿射IV边界检查消除 bit2=同块load去重。
// inline_opt：内联/devirt 深化（PYADAMAS_INLINE_OPT，默认 false）。
void optimize_ir(IRModule& m, int opt_level, int loop_opt = 0, bool inline_opt = false);

// 方案 A 第二步（opt/exc_direct.cpp）：同一帧零开销 try 里的 raise 直接跳
// land_direct，绕开 _Unwind。门控 PYADAMAS_EXC_DIRECT（默认关）。
bool exc_direct_function(IRFunction& fn);

// 间接调用去虚拟化（opt/devirt.cpp）：把「经全局/槽函数变量调用、且能证明
// 目标唯一」的 call 改写成 call.direct。判据只看 IR 结构（全局/槽只写一次 +
// 值是已知函数的 func.addr + 实参/返回类型与被调逐项一致），与优化等级无关，
// lower_module 在每个 -O 等级都调用它（-O0/-O1 没有别的优化遍）。
bool devirt_module(IRModule& m);

}  // namespace pyadamas
