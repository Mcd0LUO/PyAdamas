// codegen.h —— IR → x86-64 汇编
#pragma once

#include <string>

#include "ir.h"

namespace py23 {

// 生成 GNU as 语法的 x86-64 汇编（AT&T）。
// 调用约定：System V AMD64。整型/指针走 rdi,rsi,rdx,rcx,r8,r9，
// 浮点走 xmm0-xmm7，超出部分压栈。
// disable_unknown_index：整程序不用 unknown 时关掉存活堆索引维护，
// 入口函数会多发一条 py23_disable_unknown_index 调用。
std::string generate_asm(const IRModule& m, bool disable_unknown_index = false);

}  // namespace py23
