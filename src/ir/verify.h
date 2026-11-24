// ir/verify.h —— IR 不变量校验（调试开关用）
//
// 为什么需要它：优化遍之间没有契约检查，一个 pass 把 IR 改坏之后，症状
// 往往出现在后面很远的地方（后端错、或者运行期段错误）。LLVM 的做法是
// 每个 pass 之后跑 verifier，失败就立刻指出"哪条指令违反了哪条不变量"。
//
// 这里的检查**故意保守**：只查那些"违反了就一定是 bug"的性质，宁可少报
// 也不要把合法形态当错误。新增检查前先用 tests/run 全量跑一遍，确认零
// 误报再合入。
#pragma once

#include <string>
#include <vector>

#include "ir/types.h"

namespace pyadamas {

// 校验整个模块；返回空表示全部通过。每条违规是一行可直接打印的文本。
std::vector<std::string> verify_module(const IRModule& m);

// 校验单个函数（verify_module 内部用）。fn 不带模块上下文时也能跑。
std::vector<std::string> verify_function(const IRFunction& fn);

// 环境变量 PYADAMAS_VERIFY_IR：0/未设=关闭，1=报告但继续，2=报告并退出(70)。
int verify_ir_mode();

// 按 verify_ir_mode() 决定是否执行；失败时按模式打印/退出。
// tag 用于区分调用点（"pre-opt" / "post-opt"）。
void verify_module_or_die(const IRModule& m, const char* tag);

}  // namespace pyadamas
