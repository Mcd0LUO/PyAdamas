// codegen_div.h —— 整数除法的公共约定（codegen_ops.cpp / codegen_int_fixed.cpp）
//
// idiv/div 的除数必须在 %rcx。SysV 直接用它；Win64 下 %rcx 是第 1 个实参
// 寄存器，而除零检查里的 panic 是调用、会破坏它，所以统一用 %r10 中转。
// 魔数除法（gen_div_const）的模式码也放这里：两个发射文件共用同一份定义，
// 避免「同一语义两份常量」漂移。
#pragma once

#include "target_abi.h"

namespace pyadamas {

inline const char* div_reg() { return target_abi().gnu_cfi ? "%rcx" : "%r10"; }

constexpr int kDivModeTrunc = 0;  // 向零取整商（/ 的整数语义）
constexpr int kDivModeFloor = 1;  // Python 的 //（向下取整）
constexpr int kDivModeMod = 2;    // Python 的 %（结果与除数同号）

}  // namespace pyadamas
