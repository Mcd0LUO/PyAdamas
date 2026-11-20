// opt.h —— IR 优化遍入口
#pragma once

#include "ir.h"

namespace pyadamas {

// opt_level >= 2 时运行常量折叠/分支化简/DCE。
// loop_opt：整数循环优化位掩码（PYADAMAS_LOOP_OPT，默认 0 全关）。
// bit0=跳转线程化 bit1=仿射IV边界检查消除 bit2=同块load去重。
void optimize_ir(IRModule& m, int opt_level, int loop_opt = 0);

}  // namespace pyadamas
