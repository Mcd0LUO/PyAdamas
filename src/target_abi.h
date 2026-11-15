// target_abi.h —— 目标 ABI 描述
//
// 把「哪个物理寄存器承担什么角色」从代码生成里抽出来。没有这张表，寄存器
// 角色会散落在几千行汇编字符串拼接里，既无法审计也无法换目标。
//
// 当前只有 x86-64 SysV 一个实例；引入这一层是为 MachineInstr 层和后续
// 多目标后端留接缝——MachineInstr 需要知道寄存器类（整型/浮点）、调用
// 约定寄存器、callee-saved 集合，这些必须先有单一出处。
#pragma once

#include <array>

namespace pyadamas {

// 一个目标的 ABI 事实。全部是编译期常量，运行期零开销。
struct TargetABI {
    const char* name;

    // 整型实参寄存器，按调用约定顺序。SysV：rdi rsi rdx rcx r8 r9
    const char* const* arg_int;
    int n_arg_int;

    // 浮点实参寄存器。SysV：xmm0-xmm7
    const char* const* arg_fp;
    int n_arg_fp;

    // 返回值寄存器
    const char* ret_int;
    const char* ret_fp;

    // callee-saved 提升池：跨调用要保值的整型变量放这里
    const char* const* callee_pool;
    int n_callee_pool;

    // 浮点常驻池。只用 xmm8-15：xmm0-7 是参数/临时寄存器，所有指令生成器
    // 都只把它们当 scratch。
    const char* const* fp_pool;
    int n_fp_pool;

    // 指令生成中途可随意用的 scratch（caller-saved，与 callee_pool 不相交）
    const char* const* scratch;
    int n_scratch;

    // 栈对齐要求（字节）
    int stack_align;
};

// 当前编译目标。目前恒为 x86-64 SysV。
const TargetABI& target_abi();

}  // namespace pyadamas
