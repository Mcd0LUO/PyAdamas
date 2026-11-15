// target_abi.cpp —— 具体目标的 ABI 取值
#include "target_abi.h"

#include <cstring>

namespace pyadamas {
namespace {

// ── x86-64 SysV（Linux/macOS） ────────────────────────────────
const char* const kSysVArgInt[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
const char* const kSysVArgFp[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3",
                                  "%xmm4", "%xmm5", "%xmm6", "%xmm7"};

// ── x86-64 Win64（Microsoft 调用约定） ───────────────────────
// 差异：整型实参只有 4 个（rcx/rdx/r8/r9），浮点 4 个；rdi/rsi 在这里是
// callee-saved（SysV 是实参寄存器）；调用点要预留 32 字节影子空间。
const char* const kWin64ArgInt[] = {"%rcx", "%rdx", "%r8", "%r9"};
const char* const kWin64ArgFp[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

// 两个 ABI 共用的池：这 6 个寄存器在 SysV 和 Win64 下都是 callee-saved。
const char* const kCalleePool[] = {"%rbx", "%r12", "%r13", "%r14", "%r15", "%rbp"};
constexpr int kNCalleePool = 6;

// 浮点常驻池：xmm8-15 在 Win64 下是 callee-saved（xmm6-15 非易失），
// 在 SysV 下虽易失，但提升只发生在没有调用的函数里，两者都安全。
const char* const kFpPool[] = {"%xmm8",  "%xmm9",  "%xmm10", "%xmm11",
                               "%xmm12", "%xmm13", "%xmm14", "%xmm15"};
constexpr int kNFpPool = 8;

// scratch：r8-r11 在 SysV 与 Win64 下都是易失寄存器。
const char* const kScratch[] = {"%r8", "%r9", "%r10", "%r11"};
constexpr int kNScratch = 4;

const TargetABI kX86_64SysV{
    "x86_64-sysv",
    kSysVArgInt, 6,
    kSysVArgFp, 8,
    "%rax", "%xmm0",
    kCalleePool, kNCalleePool,
    kFpPool, kNFpPool,
    kScratch, kNScratch,
    16,
    0,
    true,
    false,
    "%rcx",
    "%rdx",
    "%r8",
    "_setjmp",
    "_longjmp",
};

const TargetABI kX86_64Win64{
    "x86_64-win64",
    kWin64ArgInt, 4,
    kWin64ArgFp, 4,
    "%rax", "%xmm0",
    kCalleePool, kNCalleePool,
    kFpPool, kNFpPool,
    kScratch, kNScratch,
    16,
    32,
    false,
    true,
    "%r10",
    "%r11",
    "%rax",
    "pa_win_setjmp",
    "pa_win_longjmp",
};

const TargetABI* g_current = &kX86_64SysV;

}  // namespace

const TargetABI& target_abi() { return *g_current; }

void set_target_abi(const TargetABI* abi) { g_current = abi ? abi : &kX86_64SysV; }

const TargetABI* find_target_abi(const char* name) {
    if (!name || !*name) return nullptr;
    if (std::strcmp(name, "x86_64-sysv") == 0 || std::strcmp(name, "sysv") == 0 ||
        std::strcmp(name, "linux") == 0)
        return &kX86_64SysV;
    if (std::strcmp(name, "x86_64-win64") == 0 || std::strcmp(name, "win64") == 0 ||
        std::strcmp(name, "windows") == 0)
        return &kX86_64Win64;
    return nullptr;
}

}  // namespace pyadamas
