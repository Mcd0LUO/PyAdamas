// codegen_int_fixed.cpp —— 定宽整数（P1）的算术与正常化
//
// 从 codegen_ops.cpp 拆出（文件规模硬限 1200 行）。默认 int 的代码路径
// 逐字沿用 gen_bin_int；本文件只在 in.ty 是定宽整数（i8/i16/i32/u8/
// u16/u32/u64）时被调用。
#include "codegen_impl.h"
#include "codegen_div.h"

namespace pyadamas {

void AsmGen::norm_int_rax(const TypePtr& t) {
        auto ii = int_info_of(t);
        if (!ii || ii->width == 64) return;
        switch (ii->width) {
            case 8:
                emit2(ii->is_signed ? MOp::kMovsbq : MOp::kMovzbq, kAl, kRax);
                break;
            case 16:
                // MReg 表里没有 16 位寄存器名。用「左移 48 位再算术/逻辑
                // 右移 48 位」做符号/零扩展：两条 64 位指令，效果与
                // movswq %ax / movzwq %ax 完全相同。
                emit_imm(MOp::kShlq, 48, kRax);
                emit_imm(ii->is_signed ? MOp::kSarq : MOp::kShrq, 48, kRax);
                break;
            case 32:
                if (ii->is_signed) emit2(MOp::kMovslq, kEax, kRax);
                else emit2(MOp::kMovl, kEax, kEax);
                break;
            default: break;
        }
    }

void AsmGen::gen_udiv64(const Inst& in) {
        // 无符号 64 位除法：divq 的语义正好是 C 的截断除法 +
        // 余数与被除数同号（u64 下即非负），不需要 Python 的符号修正。
        emit2(MOp::kMovq, src_mop(in.b), div_reg());
        gen_div_guard(false);  // 只查除零：INT64_MIN/-1 对无符号不是溢出
        line("xorq %rdx, %rdx");
        emit1(MOp::kDivq, mreg_from_name(div_reg()));
    }

void AsmGen::gen_bin_int_fixed(const Inst& in) {
        const MOperand& sa = src_mop(in.a);
        const MOperand& sb = src_mop(in.b);
        const MOperand& d = dst_mop(in.dst);
        auto ii = int_info_of(in.ty);
        if (!ii) return;
        const bool u64 = !ii->is_signed && ii->width == 64;
        // 宽度 < 64 的无符号值在槽里恒非负（零扩展不变量），
        // 所以除 u64 之外的运算都能直接复用有符号指令。
        emit2(MOp::kMovq, sa, kRax);
        // 定宽整数槽内的值一律非负（正常化保证；u64 是无符号域）→ 正 2 的
        // 幂除数的 // 与 % 各自退化成一条 shrq / andq。走通用 idivq/divq 要
        // 十几条指令加一次除法，这里是一条。
        int64_t cdiv = 0;
        const bool pow2_const = operand_const_imm(src_op(in.b), &cdiv) && cdiv > 1 &&
                                (cdiv & (cdiv - 1)) == 0;
        switch (in.bop) {
            case BinKind::Add: emit2(MOp::kAddq, sb, kRax); break;
            case BinKind::Sub: emit2(MOp::kSubq, sb, kRax); break;
            case BinKind::Mul: emit2(MOp::kImulq, sb, kRax); break;
            case BinKind::BitAnd: emit2(MOp::kAndq, sb, kRax); break;
            case BinKind::BitOr: emit2(MOp::kOrq, sb, kRax); break;
            case BinKind::BitXor: emit2(MOp::kXorq, sb, kRax); break;
            case BinKind::Shl:
                emit2(MOp::kMovq, sb, kRcx);
                emit2(MOp::kShlq, kCl, kRax);
                break;
            case BinKind::Shr:
                emit2(MOp::kMovq, sb, kRcx);
                // u64 逻辑右移；其余（含 u8/u16/u32）槽内值非负，sar == shr。
                emit2(u64 ? MOp::kShrq : MOp::kSarq, kCl, kRax);
                break;
            case BinKind::Div:
                if (pow2_const) {
                    emit_imm(MOp::kShrq, pow2_log2(static_cast<uint64_t>(cdiv)), kRax);
                    break;
                }
                if (u64) {
                    gen_udiv64(in);
                    break;
                }
                // 定宽（< 64）的魔数除法与 idivq 都在 64 位域里算，值域由
                // 正常化保证，结果最后再截断 —— 与按宽度算逐位等价。
                if (gen_div_const(in, kDivModeTrunc)) break;
                emit2(MOp::kMovq, sb, div_reg());
                gen_div_guard(ii->width == 64);
                emit0(MOp::kCqto);
                if (!target_abi().gnu_cfi) emit2(MOp::kMovq, kR10, kRcx);
                emit1(MOp::kIdivq, kRcx);
                break;
            case BinKind::Mod:
                if (pow2_const) {
                    emit_imm(MOp::kAndq, cdiv - 1, kRax);
                    break;
                }
                if (u64) {
                    gen_udiv64(in);
                    emit2(MOp::kMovq, kRdx, kRax);
                    break;
                }
                gen_mod(in);
                break;
            case BinKind::FloorDiv:
                if (pow2_const) {
                    emit_imm(MOp::kShrq, pow2_log2(static_cast<uint64_t>(cdiv)), kRax);
                    break;
                }
                if (u64) {
                    gen_udiv64(in);
                    break;  // 商在 %rax
                }
                gen_floordiv(in);
                break;
            case BinKind::Pow:
                emit2(MOp::kMovq, sa, arg_reg_i(0));
                emit2(MOp::kMovq, sb, arg_reg_i(1));
                spill_fp_slots();
                line("call pa_pow_i64");
                reload_fp_slots();
                break;
            default: break;
        }
        norm_int_rax(in.ty);
        move_to(d, mreg_operand(kRax));
    }

void AsmGen::gen_div_guard(bool check_overflow) {
        std::string ok = fresh_label("divok");
        std::string ok2 = fresh_label("divok2");
        emit2(MOp::kTestq, div_reg(), div_reg());
        emit_jmp(MOp::kJne, ok);
        line("call pa_panic_div0");
        label(ok);
        // 无符号（u64）没有 INT64_MIN / -1 溢出：那个组合在无符号域里
        // 是 2^63 / 2^64-1 == 0，绝不能误报溢出 panic。
        if (!check_overflow) return;
        emit_imm(MOp::kCmpq, -1, div_reg());
        emit_jmp(MOp::kJne, ok2);
        line("movabsq $0x8000000000000000, %r8");
        line("cmpq %r8, %rax");
        emit_jmp("jne", ok2);
        line("call pa_panic_overflow");
        label(ok2);
    }

// ── 常数除数的除法：魔数乘法替代 idivq ─────────────────────────
//
// 生成后 %rax = 向零取整商（与 idivq 的约定一致）。mode 1/2 再据此算余数
// 并按 Python 的语义修正符号。%r9 全程保存被除数。

}  // namespace pyadamas
