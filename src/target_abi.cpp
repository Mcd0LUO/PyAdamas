// target_abi.cpp —— x86-64 SysV ABI 的具体取值
#include "target_abi.h"

namespace pyadamas {
namespace {

// SysV AMD64 整型实参顺序。
const char* const kArgInt[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
constexpr int kNArgInt = 6;

// SysV AMD64 浮点实参顺序（只用低 8 个 xmm）。
const char* const kArgFp[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3",
                              "%xmm4", "%xmm5", "%xmm6", "%xmm7"};
constexpr int kNArgFp = 8;

// 整型 callee-saved 提升池。%rbp 排在最后：它是帧指针，只有确认不用帧
// 指针时才轮到它。
const char* const kCalleePool[] = {"%rbx", "%r12", "%r13", "%r14", "%r15", "%rbp"};
constexpr int kNCalleePool = 6;

// 浮点常驻池：xmm8-15（xmm0-7 是参数/scratch，不参与常驻）。
const char* const kFpPool[] = {"%xmm8",  "%xmm9",  "%xmm10", "%xmm11",
                               "%xmm12", "%xmm13", "%xmm14", "%xmm15"};
constexpr int kNFpPool = 8;

// 指令生成期间的 scratch：与 callee_pool 不相交，调用点之外可随意用。
const char* const kScratch[] = {"%r8", "%r9", "%r10", "%r11"};
constexpr int kNScratch = 4;

const TargetABI kX86_64SysV{
    "x86_64-sysv",
    kArgInt, kNArgInt,
    kArgFp,  kNArgFp,
    "%rax",  "%xmm0",
    kCalleePool, kNCalleePool,
    kFpPool,     kNFpPool,
    kScratch,    kNScratch,
    16,
};

}  // namespace

const TargetABI& target_abi() { return kX86_64SysV; }

}  // namespace pyadamas
