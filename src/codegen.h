// codegen.h —— IR → x86-64 汇编
#pragma once

#include <string>

#include "ir.h"

namespace pyadamas {

// 生成 GNU as 语法的 x86-64 汇编（AT&T）。
// 调用约定：System V AMD64。整型/指针走 rdi,rsi,rdx,rcx,r8,r9，
// 浮点走 xmm0-xmm7，超出部分压栈。
// disable_unknown_index：整程序不用 unknown 时关掉存活堆索引维护，
// 入口函数会多发一条 pa_disable_unknown_index 调用。
// zero_cost_exc：--zero-cost-exc 总开关。开启后只有 IR 里带 EH 区域标记
// （EhBegin/EhEnd/EhLandingPad）的函数才会发 personality/LSDA；关闭时输出
// 与原来的 setjmp 路径逐字节一致。
std::string generate_asm(const IRModule& m, bool disable_unknown_index = false,
                         bool zero_cost_exc = true, bool index_readonly = false,
                         bool rc_split = false,
                         bool simd = false, bool ra_opt = false,
                         bool inline_opt = false, bool type_tags = false,
                         bool vect = false, bool needs_dynamic_big = false);

}  // namespace pyadamas
