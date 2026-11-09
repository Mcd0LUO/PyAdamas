// opt.h —— IR 优化遍入口
#pragma once

#include "ir.h"

namespace py23 {

// opt_level >= 2 时运行常量折叠/分支化简/DCE。
void optimize_ir(IRModule& m, int opt_level);

}  // namespace py23
