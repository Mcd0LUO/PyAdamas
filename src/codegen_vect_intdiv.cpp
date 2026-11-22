// codegen_vect_intdiv.cpp —— 通用向量化：整数 // 与 % 的 2 的幂除数（P5）
//
// 政策：这是**通用操作支持**，不是针对某个程序的模式匹配。判据只有一条 ——
// 「除数是编译期正的 2 的幂常量」。LLVM 的 InstCombine 把 sdiv x, 2^k 降成
// 移位 + 符号修正，GCC 的 tree-vect 对同样的运算做同样的强度削减；这里发的
// 是同一件事的 packed 版本。本文件（以及调用它的分类器）不含、也不许出现任何
// 基准名、基准字面量或表达式形状的特判 —— 只看「除数是不是 2 的幂常量」。
//
// 标量参照（codegen_div_const.cpp，任意 int64 成立）：
//   * x // 2^m  == `sarq $m`（算术右移就是 floor 除法，二进制补码的定义；
//     -7 // 2 == -4、INT64_MIN // 2 == -2^62 都天然正确，不需要修正项）；
//   * x % 2^m   == `andq $(2^m-1)`（Python 的 % 与除数同号，正除数下就是取
//     低 m 位；负数同样成立：-7 % 4 == 1 == (-7 & 3)）。
// 两条都是「除数正 2 的幂」时的闭式。除数不是 2 的幂、或除数不是编译期常量
// （运行期值 / 循环不变量槽）→ 分类器维持 Fail，整段标量 fallback。
//
// packed 版本为什么不是一条指令：SSE2 / AVX2 都**没有 64 位 lane 的算术右移**
//（vpsraq 要 AVX-512），所以按 2 的补码恒等式用逻辑移位合成：
//
//     x >>a m  ==  (x >>u m) | (mask & ~((1<<(64-m)) - 1))
//                  mask = 0（x >= 0）或全 1（x < 0）
//
// 即「逻辑右移 + 负数时把高 m 位灌满 1」，与 sarq 的补 1 逐位相同：
//   * x >= 0：mask == 0，右式退化成 x >>u m == x >>a m；
//   * x <  0：mask 全 1，~(0 << (64-m)) 之外的部分为 0，恰好只补高 m 位；
//   * x == INT64_MIN：mask 全 1，逻辑右移高位补 0 再或上高 m 位 → 与 sarq
//     一致（-2^63 >>a 1 == -2^62）。
// 因此不需要任何值域守卫（「被除数非负」在向量路径里也证不出来）。
//
// 符号掩码怎么来：SSE2 没有 pcmpgtq（那是 SSE4.2），所以走 32 位 psrad +
// pshufd —— psrad $31 让每个 dword 变成自己的符号掩码，pshufd $0xF5 取
// [d1,d1,d3,d3]，把每 64 位 lane 的高 dword 掩码复制满整条 lane。注意
// 「psrlq $63 再 psllq $(64-m)」是**错的**：那只置 1 位，不是高 m 位
//（m >= 2 时结果差 2^63，实测过）。
//
// m 的范围：0 <= m <= 62。m == 0（除数 1）是恒等（x // 1 == x）/ 清零
//（x % 1 == 0）；上界 62 是因为 int64 正数域里最大的 2 的幂就是 2^62
//（2^63 不是正 i64），于是 64-m ∈ [2, 63]，psllq 的移位量不会被 64 取模
// 退化成 0。m == 64 永远非法。
//
// 寄存器：2-lane 的 floor 合成要 1 个 scratch（SEQ1 —— 与 int 乘 / Convert
// 序列共用，序列在单条 Bin 内原子完成、跨指令不 live）；% 用被除数自己的
// 寄存器倒两次（psllq 再 psrlq），**不要** scratch。4-lane 版本在
// codegen_vect_avx2.cpp 里用 VEX 三操作数形式发射（vpsrlq/vpsllq/vpor）。
#include "codegen_impl.h"

namespace pyadamas {

namespace {

// 正 2 的幂 → m（除数 == 2^m，0 <= m <= 62）；其余（<= 0、非幂、负数）→ -1。
// 负数除数（-2^m）不走这条：标量那边 floor/ceil 的符号换算另有一套式子，
// 它的结果不是简单的 sarq，不能照搬 —— 保守维持 Fail。
}  // namespace

// 除数 vreg → 移位量 m。只认**编译期常量**（ConstInt，或穿过 Mov 链的
// ConstInt）；运行期才定的除数（含循环不变量槽、参数、全局）一律不认 ——
// 那要么需要真正的除法器，要么需要运行期守卫，都不属于「通用操作」。
// 判据里没有任何形状/程序信息，只有「这个值是 2 的幂常量吗」。
// 2-lane（SSE2）发射：rd 已持有被除数 x（2 lane i64），结果原地写回 rd。
// tmp 是 scratch（seq 寄存器，与 rd 不同）。语义与标量 sarq / andq 逐位一致。
void AsmGen::vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh) {
    if (sh <= 0) {
        // m == 0：x // 1 == x（rd 已经是 x，什么都不发）；x % 1 == 0。
        if (is_mod) emit2(MOp::kPxor, rd, rd);
        return;
    }
    if (is_mod) {
        // 低 m 位 = 余数。左移丢高位再逻辑右移回来，等价于 andq $(2^m-1)。
        emit_imm(MOp::kPsllq, 64 - sh, rd);
        emit_imm(MOp::kPsrlq, 64 - sh, rd);
        return;
    }
    // 合成算术右移（推导见文件头）：
    //   mask = 每 lane 的符号掩码（0 或全 1）；tmp = mask << (64-m) = 高 m 位全 1
    //   rd   = (x >>u m) | tmp
    // 符号掩码不能用「psrlq $63 再 psllq $63」（那只置 1 位，不是 m 位）；
    // SSE2 也没有 pcmpgtq（SSE4.2），所以走 32 位 psrad + pshufd：
    //   psrad $31 让每个 dword 变成自己的符号掩码（[m(lo0),m(hi0),m(lo1),m(hi1)]），
    //   pshufd $0xF5 取 [d1,d1,d3,d3] → 每 64 位 lane 的高 dword 掩码复制满整 lane。
    emit2(MOp::kMovapd, rd, tmp);            // tmp = x
    emit_imm(MOp::kPsrad, 31, tmp);          // 每 dword 符号掩码
    {
        MInstr ins;                          // pshufd 本 GAS 只认三操作数写法
        ins.op = MOp::kPshufd;
        ins.ops.push_back(op_imm(0xF5));
        ins.ops.push_back(op_reg(tmp));
        ins.ops.push_back(op_reg(tmp));
        emit_.out.push_instr(std::move(ins));
    }
    emit_imm(MOp::kPsllq, 64 - sh, tmp);     // 0 / 高 m 位全 1
    emit_imm(MOp::kPsrlq, sh, rd);           // x >>u m（x 还在 rd 里）
    line("por " + std::string(mreg_name(tmp)) + ", " + std::string(mreg_name(rd)));
}

}  // namespace pyadamas
