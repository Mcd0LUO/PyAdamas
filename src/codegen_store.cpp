// codegen_store.cpp —— 把值写进变量槽。
//
// 唯一要特别处理的是「提升到 xmm 的槽 + 立即数」：`movq $imm, %xmmN` 不是
// 合法编码，as 直接报 operand type mismatch。立即数进 xmm 只能过一下 %rax，
// 语义不变（都是把立即数当整数位模式写进 xmm）。
//
// 0.383 的 B 就是这么发出来的：循环体内初始化的累加器槽被提升到 xmm8，
// 而 store 的是「延迟常量 0」的位模式，编译期直接失败。

#include "codegen_impl.h"

namespace pyadamas {

void AsmGen::store_slot_checked(const Inst& in) {
    const MOperand& loc = slot_mop(in.idx);
    const MOperand& val = src_mop(in.a);
    if (loc.kind == MOperandKind::Reg && loc.reg >= kXmm0 && loc.reg <= kXmm15 &&
        val.kind == MOperandKind::Imm) {
        emit2(MOp::kMovq, val, kRax);
        emit2(MOp::kMovq, kRax, loc);
        return;
    }
    move_to(loc, val);
}

}  // namespace pyadamas
