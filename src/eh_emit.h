// eh_emit.h —— 零开销异常（Z2）的 CFI / LSDA 文本发射器
//
// 定位：纯函数式的「计划 → GNU as 汇编文本」翻译器。不读 IR、不持有全局
// 状态、不进编译器流程；字节布局的权威口径是 tools/lsda_ref/FINDINGS.md
// §0.1 / §6（gcc 14.2 实测标定）。
//
// 为什么拆成三段（而不是任务建议的单函数 emit_eh_function）：
//   .cfi_personality / .cfi_lsda 必须落在 .cfi_startproc 与 .cfi_endproc
//   之间；LSDA 字节必须落在 .gcc_except_table 段；DW.ref 指针槽必须整个
//   TU 只发一次。三者的插入点不同，一次调用无法同时满足，所以接口按插入
//   点拆开：
//     1) emit_eh_func_directives() —— 生成函数时，紧跟 .cfi_startproc；
//     2) emit_eh_lsda()            —— 全模块函数发完后（当前段不是 .text）；
//     3) emit_eh_dwref()           —— 每个 TU 一次。
//   对单个函数而言「1 + 2」合起来就是完整的 EH 描述，语义与任务里建议的
//   emit_eh_function 等价。
//
// call-site 的 action 字段（ABI 语义，容易踩）：0 = 该区间没有 action 记录
// （等价于纯 cleanup）；非 0 是 action 表内 **1 起的字节偏移**，不是记录序号
// —— libsupc++/libc++abi 都按 action_table + (cs_action - 1) 定位。本发射器
// 每条 action 记录固定 (filter=1, next=0) 共 2 字节，所以多条 handler 的
// action 字段值是 1, 3, 5, …（不是 1, 2, 3）。
//
// 两种 LSDA 形态（与 gcc 14.2 逐字节对齐）：
//   A. 有 handler（catch-all）：ttype_encoding = 0x9b；TType 偏移指向表尾；
//      每个 handler range 一条 action 记录 (filter=1, next=0)；TType 表只有
//      一个 4 字节 0 项且**不带重定位**（null type_info* ⇒ catch(...)）。
//      单 handler + 一条 LP 覆盖记录 = gcc 同款 20 字节基准。
//   B. 全是 cleanup：ttype_encoding = 0xff，TType 偏移字段整段消失；call-site
//      的 action 字段全为 0；没有 action 表、没有 TType 表（12 字节形态）。
//
// 两种输入模式：
//   数值模式：EhRange::begin_off/end_off/lp_off 是相对函数起点的字节偏移。
//             golden 测试和「布局已定」的场景用它；
//   标签模式：EhRange::begin_label/end_label/lp_label + func_label 给出符号名，
//             发射 .uleb128 .LEHE0-.LFB0 这类标签差，由汇编器算数值和编码
//             宽度。**codegen 集成必须用它**：发射期算不出指令字节偏移。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pyadamas {

// runtime 里的 personality routine（runtime/rt_personality.c，非 static +
// used）。.cfi_personality 用 0x9b（indirect|pcrel|sdata4）时必须经由
// DW.ref 指针槽引用它：直接写函数名会汇编/链接通过、运行期 SIGSEGV。
inline constexpr const char* kEhPersonalitySymbol = "pa_personality";
inline constexpr const char* kEhDwrefLabel = "DW.ref.pa_personality";

// 一段保护区：LSDA call-site 表里的一条记录。
//
// 位置基准：LPStart 编码是 0xff（omit），所以偏移一律相对函数起点
// （= FDE region start = .cfi_startproc 所在地址）。
struct EhRange {
    uint32_t begin_off = 0;   // 保护区起点，相对函数起点
    uint32_t end_off = 0;     // 保护区终点（开区间），相对函数起点
    uint32_t lp_off = 0;      // landing pad 偏移；0 = 该区间无 landing pad
    bool is_handler = false;  // true = catch（action 记录 filter>0）；false = cleanup

    // 标签模式（codegen 用）。begin/end 必须成对；lp_label 为空表示该区间
    // 没有 landing pad。三者与 EhFuncPlan::func_label 一起给出。
    const char* begin_label = nullptr;
    const char* end_label = nullptr;
    const char* lp_label = nullptr;
};

// 一个函数的 EH 发射计划。
struct EhFuncPlan {
    std::vector<EhRange> ranges;
    int lsda_index = 0;  // 生成 .LLSDA<N> / .LLSDACSB<N> 等唯一标签
    const char* func_label = nullptr;  // 标签模式：函数起点（首条指令）标签
};

// 计划合法性检查。emit_* 内部会再查一遍并以 std::invalid_argument 抛出；
// codegen 可以在拼计划时先调它拿到可读错误。error 可为空。
bool eh_plan_check(const EhFuncPlan& plan, std::string* error = nullptr);

// 该计划是否走标签模式（要求整个 plan 模式统一，见 eh_plan_check）。
bool eh_plan_uses_labels(const EhFuncPlan& plan);

// 函数级 CFI 指令：必须紧跟 .cfi_startproc 发射（ranges 为空则什么也不发）。
void emit_eh_func_directives(std::string& out, const EhFuncPlan& plan);

// 本函数的 .gcc_except_table LSDA：在 .text 之外发射，内部自带
// .section .gcc_except_table / .align 4；结束后停留在该段（调用方接着发
// 下一个 LSDA 或自己切段）。
void emit_eh_lsda(std::string& out, const EhFuncPlan& plan);

// personality 间接指针槽：.data.rel.local 里的 8 字节 .quad pa_personality。
// 每个 TU 只允许发一次（重复会重定义 DW.ref 标签）。
void emit_eh_dwref(std::string& out);

}  // namespace pyadamas
