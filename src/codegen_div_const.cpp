// codegen_div_const.cpp —— 常数除数的强度削减（// 与 %）。
//
// 从 codegen_ops.cpp 搬出来（那段把它顶到 1248 行，超了 1200 行硬限制）。
// 逻辑零改动，只有落点变了。
//
// 两条路径：
//   * **正 2 的幂**：// 是一条 sarq（算术右移就是 floor 除法），% 是一条 andq，
//     trunc 用 `(x + ((x>>63) & (m-1))) >> k`。三条式子对全部 int64 成立，
//     不依赖任何溢出假设（本语言 int 溢出回绕，非负性证明在这里没用）。
//   * **其他常数**：魔数乘（`udiv_magic`），被除数可证非负时省掉 abs/符号还原。

#include "codegen_impl.h"

#include "codegen_div.h"

namespace pyadamas {

// idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。

void AsmGen::gen_div_const_emit(int64_t d, int mode, bool a_nonneg) {
        const uint64_t ad = d < 0 ? static_cast<uint64_t>(-(d + 1)) + 1
                                  : static_cast<uint64_t>(d);
        const int pl = pow2_log2(ad);
        uint64_t mp = 0;
        int L = 0;
        bool ok = false;
        if (pl >= 0) {
            L = pl;
            ok = true;
        } else {
            ok = udiv_magic(ad, &mp, &L);
        }
        if (!ok) { line("# internal: no magic for constant divisor"); }

        // 被除数可证非负且除数为正：|x| == x，商非负，余数天然落在
        // [0, d) —— 省掉 abs/符号还原（4 条）与余数符号修正（test+jns+
        // add，3 条）。2 的幂：商是逻辑右移，mod 退化成一条 AND。
        // 魔数本来就按无符号域算（udiv_magic），x>=0 时直接复用。
        if (a_nonneg && d > 0) {
            if (pl >= 0) {
                line("movq %r9, %rax");
                if (mode == kDivModeMod) {
                    if (pl > 0) emit_imm(MOp::kAndq, static_cast<int64_t>(ad - 1), kRax);
                } else if (pl > 0) {
                    emit_imm(MOp::kShrq, pl, kRax);
                }
                return;
            }
            emit2(MOp::kMovq, kR9, kR11);
            emit_imm_hex(MOp::kMovabsq, mp, kRcx);
            line("mulq %rcx");            // rdx = mulhu(x, m')
            emit2(MOp::kAddq, kR11, kRdx);
            emit_imm(MOp::kShrq, L, kRdx);
            if (mode != kDivModeMod) {
                line("movq %rdx, %rax");  // trunc == floor（x>=0, d>0）
                return;
            }
            emit2(MOp::kMovq, kRdx, kRcx);
            emit_imm(MOp::kImulq, d, kRcx);
            emit2(MOp::kMovq, kR9, kRax);
            emit2(MOp::kSubq, kRcx, kRax);      // r = x - q*d ∈ [0, d)
            return;
        }

        // **正 2 的幂除数**：三种模式各有更短的闭式，不再走「算商 → 算余数 →
        // 按余数符号修正」那条通用尾巴（那条要多 5 条指令 + 一个数据相关分支）。
        // 三条式子对全部 int64 都成立（含负数、含 INT64_MIN），不依赖任何
        // 溢出假设 —— 溢出的语义是回绕，所以「非负性证明」在这里用不上。
        //
        // 唯一没覆盖的是 d<0（floor/ceil 的符号换算另有分支），继续走老路。
        if (pl >= 0 && d > 0 && pl > 0) {
            const int64_t mask = static_cast<int64_t>(ad - 1);
            line("movq %r9, %rax");
            if (mode == kDivModeMod) {
                // Python 的 % 对正整数除数取「与除数同号」，配合二进制补码，
                // 结果恰好等于 x & (2^k-1)：-7 % 4 == 1 == (-7 & 3)。一条 AND。
                emit_imm(MOp::kAndq, mask, kRax);
            } else if (mode == kDivModeFloor) {
                // **算术右移本身就是 floor 除法**（二进制补码的定义），不需要
                // 任何修正：-7 >> 1 == -4 == -7 // 2。-2^63 也天然正确
                // （-2^63 能被任何 2^k 整除）。
                emit_imm(MOp::kSarq, pl, kRax);
            } else {
                // trunc(x / 2^k) = (x + ((x>>63) & (2^k-1))) >> k
                emit2(MOp::kMovq, kRax, kRcx);
                emit_imm(MOp::kSarq, 63, kRcx);
                emit_imm(MOp::kAndq, mask, kRcx);
                emit2(MOp::kAddq, kRcx, kRax);
                emit_imm(MOp::kSarq, pl, kRax);
            }
            return;
        }
        if (pl >= 0) {
            // 2 的幂（除数为负）：trunc(x / 2^L) = (x + (x>>63 & (2^L-1))) >> L
            line("movq %r9, %rax");
            if (pl > 0) {
                emit2(MOp::kMovq, kRax, kRcx);
                emit_imm(MOp::kSarq, 63, kRcx);
                emit_imm(MOp::kAndq, static_cast<int64_t>(ad - 1), kRcx);
                emit2(MOp::kAddq, kRcx, kRax);
                emit_imm(MOp::kSarq, pl, kRax);
            }
        } else {
            // ax = |x|（无符号域），q_abs = (mulhu(ax, m') + ax) >> L
            line("movq %r9, %r8");
            line("sarq $63, %r8");        // r8 = 0 或 -1
            line("movq %r9, %rax");
            line("xorq %r8, %rax");
            line("subq %r8, %rax");       // rax = ax
            emit2(MOp::kMovq, kRax, kR11);
            emit_imm_hex(MOp::kMovabsq, mp, kRcx);
            line("mulq %rcx");            // rdx:rax = ax * m', rdx = mulhu
            emit2(MOp::kAddq, kR11, kRdx);
            emit_imm(MOp::kShrq, L, kRdx);
            line("movq %rdx, %rax");
            line("xorq %r8, %rax");
            line("subq %r8, %rax");       // 按符号还原成有符号商
        }
        // 除数为负：trunc(x/d) = -trunc(x/|d|)。
        if (d < 0) line("negq %rax");
        if (mode == kDivModeTrunc) return;

        // 余数 r = x - q*d。q 先挪到 %rcx，|d| <= 2^31 所以能用 32 位立即数。
        line("movq %rax, %rcx");
        emit_imm(MOp::kImulq, d, kRcx);
        line("movq %r9, %rdx");
        line("subq %rcx, %rdx");
        std::string done = fresh_label("dcdiv");
        line("testq %rdx, %rdx");
        // r 与除数异号时才要修正。除数的符号编译期已知，省掉一次 xor。
        emit_jmp(d > 0 ? "jns" : "jle", done);
        if (mode == kDivModeFloor) {
            line("decq %rax");
            label(done);
            return;
        }
        // Python 的 % 结果与除数同号：r 异号时补一个 d。
        emit_imm(MOp::kAddq, d, kRdx);
        label(done);
        line("movq %rdx, %rax");
}

bool AsmGen::gen_div_const(const Inst& in, int mode) {
        if (in.a == kNoReg || in.b == kNoReg) return false;
        int64_t d = 0;
        if (!operand_const_imm(src_op(in.b), &d)) return false;
        // 除数 0 交给通用路径（它调 pa_panic_div0 给可读报错）；除数 -1 也要
        // 走通用路径：x * -1 的 INT64_MIN 溢出保护省不得，而魔数法算不出那个
        // 上溢（它会安静地给出 INT64_MIN）。
        if (d == 0 || d == -1 || d == INT64_MIN) return false;
        if (d < INT32_MIN || d > INT32_MAX) return false;
        if (mode == kDivModeMod && d == 1) { line("xorq %rax, %rax"); return true; }
        // 被除数已由调用方（gen_bin_int / gen_bin_int_fixed）装入 %rax，
        // 这里不要重复 emit mov src,%rax —— 每条常数除/取模的快路径都会
        // 因此多出一条冗余 mov（Collatz 内层循环每轮两次，占 12% 指令数）。
        if (d == 1) return true;

        // **正 2 的幂：直接复用 %rax 里的被除数，不经过 %r9 暂存。**
        // gen_div_const_emit 的通用路径为了还原余数要先 `movq src,%r9`，
        // 再用时又 `movq %r9,%rax` —— 这两条在幂情形纯属浪费（不需要余数）。
        if (d > 0 && (d & (d - 1)) == 0) {
            const int pl = pow2_log2(static_cast<uint64_t>(d));
            const int64_t mask = d - 1;
            // %rax 已经在调用方装好 src，直接用。
            if (mode == kDivModeMod) {
                emit_imm(MOp::kAndq, mask, kRax);
            } else if (mode == kDivModeFloor) {
                emit_imm(MOp::kSarq, pl, kRax);
            } else {
                emit2(MOp::kMovq, kRax, kRcx);
                emit_imm(MOp::kSarq, 63, kRcx);
                emit_imm(MOp::kAndq, mask, kRcx);
                emit2(MOp::kAddq, kRcx, kRax);
                emit_imm(MOp::kSarq, pl, kRax);
            }
            return true;
        }

        // %r9 暂存被除数：算完商之后还原余数还要用它。%r9 是块内临时寄存器，
        // 不受槽分配影响（vreg 只用 callee pool：rbx/r12-r15/rbp）。
        emit2(MOp::kMovq, src_mop(in.a), kR9);
        // 被除数可证非负（降级期打标）→ 无符号魔数路径。优化遍新建的
        // vreg 没有标（越界按 false），保守走有符号路径。
        bool a_nn = static_cast<size_t>(in.a) < f_->vreg_nonneg.size() &&
                    f_->vreg_nonneg[static_cast<size_t>(in.a)] != 0;
        gen_div_const_emit(d, mode, a_nn);
        return true;
}


}  // namespace pyadamas
