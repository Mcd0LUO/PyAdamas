// target_abi.h —— 目标 ABI 描述
//
// 把「哪个物理寄存器承担什么角色」从代码生成里抽出来。没有这张表，寄存器
// 角色会散落在几千行汇编字符串拼接里，既无法审计也无法换目标。
//
// 目前有两个实例：x86-64 SysV（默认）与 x86-64 Win64。两者的寄存器池
// （callee-saved 提升池、scratch）恰好兼容，差异集中在实参寄存器顺序、
// 影子空间和汇编指示符。
#pragma once

namespace pyadamas {

// 一个目标的 ABI 事实。全部是编译期常量，运行期零开销。
struct TargetABI {
    const char* name;

    // 整型实参寄存器，按调用约定顺序
    const char* const* arg_int;
    int n_arg_int;

    // 浮点实参寄存器（与整型分属两个寄存器序列）
    const char* const* arg_fp;
    int n_arg_fp;

    // 返回值寄存器
    const char* ret_int;
    const char* ret_fp;

    // callee-saved 提升池：跨调用要保值的整型变量放这里
    const char* const* callee_pool;
    int n_callee_pool;

    // 浮点常驻池。只用各 ABI 下 callee-saved 的那一段。
    const char* const* fp_pool;
    int n_fp_pool;

    // 指令生成中途可随意用的 scratch（caller-saved，与 callee_pool 不相交）
    const char* const* scratch;
    int n_scratch;

    // 栈对齐要求（字节）
    int stack_align;

    // 调用点必须为被调方预留的影子空间（字节）。Win64 是 32，SysV 是 0。
    int shadow_space;

    // 汇编器是否接受 GNU 的 .cfi_* 展开指示符。Windows 用 SEH，不走 .eh_frame。
    bool gnu_cfi;

    // setjmp/longjmp 的符号名。glibc 有下划线变体 _setjmp/_longjmp，它们不
    // 保存信号掩码（实测 2.5ns vs 117ns）；Windows 只有标准名。生成代码和
    // 运行时必须用同一个名字，所以放进 ABI。
    // 实参寄存器分配是「按位置」还是「按类型分计数」。
    // SysV：整型和浮点各有一套独立计数器（rdi,rsi,... / xmm0,xmm1,...）。
    // Win64：第 i 个实参固定用第 i 个槽（rcx/rdx/r8/r9 或 xmm0-3），与类型
    // 无关；第 5 个起一律落栈。两者不能混用。
    bool positional_args;

    // 内联下标检查用的三个易失 scratch：长度、data 指针、旧值暂存。
    // 必须避开该 ABI 的实参寄存器。SysV 可以直接借 rcx/rdx/r8（这三个
    // 虽然也是实参寄存器，但这段序列里没有调用）；Win64 下 rcx/rdx/r8/r9
    // 正好是前四个实参，只能改用 r10/r11/rax。
    const char* scratch_len;
    const char* scratch_ptr;
    const char* scratch_val;

    const char* setjmp_sym;
    const char* longjmp_sym;
};

// 当前编译目标（默认 x86-64 SysV）。
const TargetABI& target_abi();

// 切换目标。传空指针恢复默认。
void set_target_abi(const TargetABI* abi);

// 具名目标：失败返回 nullptr。
const TargetABI* find_target_abi(const char* name);

}  // namespace pyadamas
