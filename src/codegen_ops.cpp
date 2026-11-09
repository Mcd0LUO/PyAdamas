// codegen_ops.cpp —— 指令选择：算术/比较/转换
#include "codegen_impl.h"

namespace py23 {

// ── 指令 ────────────────────────────────────────────────────
void AsmGen::gen_inst(const Inst& in) {
        comment(in.text());
        switch (in.op) {
            case IrOp::ConstInt: gen_const_int(in); break;
            case IrOp::ConstFloat: gen_const_float(in); break;
            case IrOp::ConstStr:
                line(std::format("leaq .Lstr{}(%rip), %rax", in.idx));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::ConstNone:
                move_to(dst_op(in.dst), "$0");
                break;
            case IrOp::FuncAddr:
                line(std::format("leaq .Lclos{}(%rip), %rax", in.imm));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::FuncCode:
                line(std::format("leaq {}(%rip), %rax", func_label(static_cast<int>(in.imm))));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::Mov:
                move_to(dst_op(in.dst), src_op(in.a));
                break;
            case IrOp::Bin: gen_bin(in); break;
            case IrOp::Un: gen_un(in); break;
            case IrOp::Cmp: gen_cmp(in); break;
            case IrOp::Convert: gen_convert(in); break;
            case IrOp::LoadSlot: gen_load_slot(in); break;
            case IrOp::StoreSlot:
                move_to(slot_op(in.idx), src_op(in.a));
                invalidate_slot(in.idx);
                break;
            case IrOp::LoadGlobal:
                line(std::format("movq {}(%rip), %rax", global_label(in.idx)));
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::StoreGlobal:
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}(%rip)", global_label(in.idx)));
                break;
            case IrOp::MakeList: gen_make_list(in); break;
            case IrOp::MakeTuple: gen_make_list(in); break;
            case IrOp::IndexLoad: gen_index_load(in); break;
            case IrOp::IndexStore: gen_index_store(in); break;
            case IrOp::Len:
                line(std::format("movq {}, %rax", src_op(in.a)));
                line("movq (%rax), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::ObjNew: gen_obj_new(in); break;
            case IrOp::FieldLoad: gen_field_load(in); break;
            case IrOp::FieldStore: gen_field_store(in); break;
            case IrOp::VCall: gen_vcall(in); break;
            case IrOp::VCallStatic: gen_vcall_static(in); break;
            case IrOp::SuperVCall: gen_super_vcall(in); break;
            case IrOp::CallDirect: gen_call_direct(in); break;
            case IrOp::Call: gen_call(in); break;
            case IrOp::CallRt: gen_call_rt(in); break;
            case IrOp::Setjmp: gen_setjmp(in); break;
            case IrOp::ImportModule: {
                const std::string& name = m_->imports[static_cast<size_t>(in.imm)];
                line(std::format("leaq {}(%rip), %rdi", module_ready_symbol(name)));
                line(std::format("leaq {}(%rip), %rsi", module_init_symbol(name)));
                spill_fp_slots();
                line("call py23_import_module");
                reload_fp_slots();
                break;
            }
            case IrOp::Jump: line(std::format("jmp {}", block_label(in.target))); break;
            case IrOp::Branch: gen_branch(in); break;
            case IrOp::Return: gen_return(in); break;
            case IrOp::SlotAddr: {
                int slot = in.idx;
                line(std::format("leaq {}, %rax", gen_ ? gen_smem(slot) : smem(slot)));
                move_to(dst_op(in.dst), "%rax");
                break;
            }
            case IrOp::GenNew: {
                const auto& g = m_->generators[static_cast<size_t>(in.imm)];
                line(std::format("movq ${}, %rdi", g.n_slots));
                line(std::format("leaq {}(%rip), %rsi", func_label(g.resume)));
                line(std::format("leaq .Lgengcrel{}(%rip), %rdx", in.imm));
                line(std::format("movq ${}, %rcx", g.frame_kind));
                line("call py23_gen_new");
                move_to(dst_op(in.dst), "%rax");
                break;
            }
            case IrOp::GenSetSlot:
                line(std::format("movq {}, %rax", src_op(in.args[0])));
                line(std::format("movq {}, %rcx", src_op(in.args[1])));
                line(std::format("movq %rcx, {}(%rax)", 8 * (10 + in.idx)));
                break;
            case IrOp::GenGetState:
                line("movq 0(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenClosing:
                line("movq 40(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenGetExc:
                line("movq 64(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenSent:
                line("movq 16(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenYield:
                if (in.a != kNoReg) line(std::format("movq {}, %rax", src_op(in.a)));
                else line("xorl %eax, %eax");
                line("movq %rax, 24(%r15)");
                line(std::format("movq ${}, 0(%r15)", in.imm));
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
            case IrOp::GenAwait:
                if (in.a != kNoReg) line(std::format("movq {}, %rax", src_op(in.a)));
                else line("xorl %eax, %eax");
                line("movq %rax, 24(%r15)");
                line(std::format("movq ${}, 0(%r15)", in.imm));
                line("movq $3, 8(%r15)");
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
            case IrOp::GenRethrow:
                line("movq %r15, %rdi");
                line("call py23_coro_rethrow");
                line("ud2");
                break;
            case IrOp::GenDone:
                if (in.a != kNoReg) {
                    line(std::format("movq {}, %rax", src_op(in.a)));
                    line("movq %rax, 24(%r15)");
                }
                line("movq $1, 8(%r15)");
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
        }
    }

// x = y 这种加载能不能省掉，看两件事：
    //   1. 变量 x 在寄存器里（否则读出来就得写回一个栈槽，省不了）
    //   2. 这个临时值只被读一次，且读的时候中间没有对 x 的赋值
    // 都满足时，临时值直接指向 x 的寄存器，整条指令消失。
void AsmGen::gen_load_slot(const Inst& in) {
        std::string sop = slot_op(in.idx);
        // 融合目标非空时**必须**真的写过去，不能走别名——别名只是
        // 让 %v 指向变量的位置，而 `store.slot s, %v` 已经被吞掉，
        // 变量的位置不会有任何变化，值就悄悄丢了。
        if (dst_loc_.empty() && in.dst != kNoReg && in.dst < use_count_.size() &&
            use_count_[in.dst] == 1 && !cross_block_[in.dst]) {
            // 读之后、第一次真正使用之前不能有对该变量的赋值。
            // 允许别名指向栈槽：后续使用者可以直接以内存操作数读它，
            // 省掉「变量 → 临时 vreg 槽」的一次搬运。
            bool clobbered = false;
            auto stv = block_stores_.find(in.idx);
            auto us = block_first_use_.find(in.dst);
            if (stv != block_stores_.end() && us != block_first_use_.end()) {
                auto it = std::upper_bound(stv->second.begin(), stv->second.end(), cur_pos_);
                if (it != stv->second.end() && *it < us->second) clobbered = true;
            }
            if (!clobbered) {
                alias_[in.dst] = sop;
                return;
            }
        }
        move_to(dst_op(in.dst), sop);
    }

void AsmGen::gen_const_int(const Inst& in) {
        std::string d = dst_op(in.dst);
        if (in.imm >= INT32_MIN && in.imm <= INT32_MAX) {
            line(std::format("movq ${}, {}", in.imm, d));
        } else {
            line(std::format("movabsq ${}, %rax", in.imm));
            line(std::format("movq %rax, {}", d));
        }
    }

void AsmGen::gen_const_float(const Inst& in) {
        uint64_t bits = 0;
        std::memcpy(&bits, &in.fimm, sizeof(bits));
        line(std::format("movabsq $0x{:x}, %rax", bits));
        line("movq %rax, %xmm0");
        line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
    }

void AsmGen::gen_bin(const Inst& in) {
        TypePtr ty = in.ty;
        if (ty && ty->kind == TyKind::Str) {
            if (in.bop != BinKind::Add) {
                line("# unsupported string operator");
                return;
            }
            line(std::format("movq {}, %rdi", src_op(in.a)));
            line(std::format("movq {}, %rsi", src_op(in.b)));
            spill_fp_slots();
            line("call py23_str_concat");
            reload_fp_slots();
            move_to(dst_op(in.dst), "%rax");
            return;
        }
        if (ir_is_float(ty)) {
            gen_bin_fp(in);
        } else {
            gen_bin_int(in);
        }
    }

void AsmGen::gen_bin_int(const Inst& in) {
        const std::string sa = src_op(in.a);
        const std::string sb = src_op(in.b);
        std::string d = dst_op(in.dst);

        // 目标位置恰好装着左操作数时，可以就地算：`x = x + y` 变成
        // 一条 `addq y, x`。这是变量提升之后最常见的形状，省掉
        // 「搬到 rax、算、搬回去」两条指令。
        if (d[0] == '%' && sa == d) {
            switch (in.bop) {
                case BinKind::Add: line(std::format("addq {}, {}", sb, d)); return;
                case BinKind::Sub: line(std::format("subq {}, {}", sb, d)); return;
                case BinKind::Mul: line(std::format("imulq {}, {}", sb, d)); return;
                case BinKind::BitAnd: line(std::format("andq {}, {}", sb, d)); return;
                case BinKind::BitOr: line(std::format("orq {}, {}", sb, d)); return;
                case BinKind::BitXor: line(std::format("xorq {}, {}", sb, d)); return;
                case BinKind::Shl:
                    line(std::format("movq {}, %rcx", sb));
                    line(std::format("shlq %cl, {}", d));
                    return;
                case BinKind::Shr:
                    line(std::format("movq {}, %rcx", sb));
                    line(std::format("sarq %cl, {}", d));
                    return;
                default: break;  // 除法/取模/幂要用 rax/rdx，走通用路径
            }
        }

        line(std::format("movq {}, %rax", sa));
        switch (in.bop) {
            case BinKind::Add: line(std::format("addq {}, %rax", sb)); break;
            case BinKind::Sub: line(std::format("subq {}, %rax", sb)); break;
            case BinKind::Mul: line(std::format("imulq {}, %rax", sb)); break;
            case BinKind::BitAnd: line(std::format("andq {}, %rax", sb)); break;
            case BinKind::BitOr: line(std::format("orq {}, %rax", sb)); break;
            case BinKind::BitXor: line(std::format("xorq {}, %rax", sb)); break;
            case BinKind::Shl:
                line(std::format("movq {}, %rcx", sb));
                line("shlq %cl, %rax");
                break;
            case BinKind::Shr:
                line(std::format("movq {}, %rcx", sb));
                line("sarq %cl, %rax");
                break;
            case BinKind::Div:
                line(std::format("movq {}, %rcx", sb));
                gen_div_guard();
                line("cqto");
                line("idivq %rcx");
                break;
            case BinKind::Mod: gen_mod(in); break;
            case BinKind::FloorDiv: gen_floordiv(in); break;
            case BinKind::Pow:
                line(std::format("movq {}, %rdi", sa));
                line(std::format("movq {}, %rsi", sb));
                spill_fp_slots();
                line("call py23_pow_i64");
                reload_fp_slots();
                break;
        }
        move_to(d, "%rax");
    }

// idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。
void AsmGen::gen_div_guard() {
        std::string ok = fresh_label("divok");
        std::string ok2 = fresh_label("divok2");
        line("testq %rcx, %rcx");
        line(std::format("jne {}", ok));
        line("call py23_panic_div0");
        label(ok);
        line("cmpq $-1, %rcx");
        line(std::format("jne {}", ok2));
        line("movabsq $0x8000000000000000, %r8");
        line("cmpq %r8, %rax");
        line(std::format("jne {}", ok2));
        line("call py23_panic_overflow");
        label(ok2);
    }

// x86 的 idiv 向零取整，Python 的 // 向下取整，余数符号也不同。
    // 差 1 的修正必须做对，否则负数的 // 和 % 全是错的。
void AsmGen::gen_floordiv(const Inst& in) {
        std::string done = fresh_label("fdiv");
        line(std::format("movq {}, %rcx", src_op(in.b)));
        gen_div_guard();
        line("cqto");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        line(std::format("je {}", done));
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        line(std::format("jns {}", done));
        line("decq %rax");
        label(done);
    }

// Python 的 % 结果与除数同号（-7 % 3 == 2），x86 的余数与被除数同号，
    // 差一个除数，需要补回来。
void AsmGen::gen_mod(const Inst& in) {
        std::string done = fresh_label("mod");
        line(std::format("movq {}, %rcx", src_op(in.b)));
        gen_div_guard();
        line("cqto");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        line(std::format("je {}", done));
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        line(std::format("jns {}", done));
        line("addq %rcx, %rdx");
        label(done);
        line("movq %rdx, %rax");
    }

void AsmGen::gen_bin_fp(const Inst& in) {
        std::string d = dst_op(in.dst);
        bool simple = in.bop == BinKind::Add || in.bop == BinKind::Sub ||
                      in.bop == BinKind::Mul || in.bop == BinKind::Div;
        // 结果直接落在提升过的浮点寄存器时，把左操作数装进目标寄存器
        // 就地算。累加 `s = s + ...` 因此只剩一条 addsd。
        if (simple && d.starts_with("%xmm")) {
            std::string sa = src_op(in.a);
            if (sa != d) line(std::format("movsd {}, {}", sa, d));
            switch (in.bop) {
                case BinKind::Add: line(std::format("addsd {}, {}", src_op(in.b), d)); break;
                case BinKind::Sub: line(std::format("subsd {}, {}", src_op(in.b), d)); break;
                case BinKind::Mul: line(std::format("mulsd {}, {}", src_op(in.b), d)); break;
                case BinKind::Div: line(std::format("divsd {}, {}", src_op(in.b), d)); break;
                default: break;
            }
            return;
        }
        line(std::format("movsd {}, %xmm0", src_op(in.a)));
        switch (in.bop) {
            case BinKind::Add: line(std::format("addsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::Sub: line(std::format("subsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::Mul: line(std::format("mulsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::Div: line(std::format("divsd {}, %xmm0", src_op(in.b))); break;
            case BinKind::FloorDiv:
                // floor(a / b)：先除再向下取整。roundsd 要 SSE4.1，
                // 为了兼容性走 libm。
                line(std::format("divsd {}, %xmm0", src_op(in.b)));
                spill_fp_slots();
                line("call floor");
                reload_fp_slots();
                break;
            case BinKind::Mod: {
                // C 的 fmod 余数与被除数同号，Python 的 % 与除数同号，
                // 两者在异号时差一个 |b|，这里补上。xmm1 是调用者保存的，
                // fmod 回来必须重新装 b。余数为 0 时 Python 取除数的符号，
                // 所以 -0.0 % 3.0 是 0.0 而不是 -0.0。
                std::string done = fresh_label("fmod");
                std::string nz = fresh_label("fmodnz");
                line(std::format("movsd {}, %xmm1", src_op(in.b)));
                spill_fp_slots();
                line("call fmod");
                reload_fp_slots();
                line(std::format("movsd {}, %xmm1", src_op(in.b)));
                line("xorpd %xmm2, %xmm2");
                line("ucomisd %xmm2, %xmm0");
                line(std::format("jp {}", done));  // NaN 原样返回
                line(std::format("jne {}", nz));
                line("movq %xmm1, %rdx");
                line("movabsq $0x8000000000000000, %rax");
                line("andq %rdx, %rax");
                line("movq %rax, %xmm0");  // ±0.0，符号跟除数
                line(std::format("jmp {}", done));
                label(nz);
                line("movq %xmm0, %rax");
                line("movq %xmm1, %rdx");
                line("xorq %rdx, %rax");
                line(std::format("jns {}", done));  // 同号，无需修正
                line("addsd %xmm1, %xmm0");
                label(done);
                break;
            }
            case BinKind::Pow:
                line(std::format("movsd {}, %xmm1", src_op(in.b)));
                spill_fp_slots();
                line("call pow");
                reload_fp_slots();
                break;
            default:
                line("# unsupported float operator");
                break;
        }
        line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
    }

void AsmGen::gen_un(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (in.uop == UnKind::Not) {
            emit_truth(in.a, "%al");
            line("xorb $1, %al");
            line("movzbq %al, %rax");
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        if (ir_is_float(ty)) {
            if (in.uop == UnKind::Neg) {
                line(std::format("movsd {}, %xmm0", src_op(in.a)));
                line("movabsq $0x8000000000000000, %rax");
                line("movq %rax, %xmm1");
                line("xorpd %xmm1, %xmm0");
                line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            }
            return;
        }
        line(std::format("movq {}, %rax", src_op(in.a)));
        switch (in.uop) {
            case UnKind::Neg: line("negq %rax"); break;
            case UnKind::BitNot: line("notq %rax"); break;
            default: break;
        }
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

// 把一个值算成 0/1 放进 %al。浮点的 NaN 视为真（Python 语义），
    // 所以用 setne + setp 的组合。
void AsmGen::emit_truth(VReg r, const char* byte_reg) {
        (void)byte_reg;
        TypePtr ty = ty_of(r);
        if (ir_is_float(ty)) {
            line(std::format("movsd {}, %xmm0", src_op(r)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd %xmm1, %xmm0");
            line("setne %al");
            line("setp %cl");
            line("orb %cl, %al");
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            line(std::format("movq {}, %rax", src_op(r)));
            line("cmpq $0, (%rax)");
            line("setne %al");
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            line(std::format("movq {}, %rax", src_op(r)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd 8(%rax), %xmm1");
            line("setne %al");
            line("ucomisd 16(%rax), %xmm1");
            line("setne %cl");
            line("orb %cl, %al");
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            line(std::format("movq {}, %rax", src_op(r)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd 8(%rax), %xmm1");
            line("setne %al");
            line("ucomisd 16(%rax), %xmm1");
            line("setne %cl");
            line("orb %cl, %al");
            return;
        }
        line(std::format("cmpq $0, {}", src_op(r)));
        line("setne %al");
    }

void AsmGen::gen_cmp(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ty && ty->kind == TyKind::Str) {
            line(std::format("movq {}, %rdi", src_op(in.a)));
            line(std::format("movq {}, %rsi", src_op(in.b)));
            if (in.cop == CmpKind::Eq || in.cop == CmpKind::Ne) {
                spill_fp_slots();
                line("call py23_str_eq");
                reload_fp_slots();
                if (in.cop == CmpKind::Ne) line("xorq $1, %rax");
                line(std::format("movq %rax, {}", dst_op(in.dst)));
                return;
            }
            // 大小比较走三路比较。早先这里只留了一句注释、什么都不生成，
            // 于是 `a < b` 静默拿到槽里的残留值——恒为 False。
            spill_fp_slots();
            line("call py23_str_cmp");
            reload_fp_slots();
            line("cmpq $0, %rax");
            switch (in.cop) {
                case CmpKind::Lt: line("setl %al"); break;
                case CmpKind::Le: line("setle %al"); break;
                case CmpKind::Gt: line("setg %al"); break;
                case CmpKind::Ge: line("setge %al"); break;
                default: break;
            }
            line("movzbq %al, %rax");
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        if (ir_is_float(ty)) {
            gen_cmp_fp(in);
            return;
        }
        // cmp 不写坏操作数，所以左操作数已经在寄存器/栈上时不必先搬进
        // rax。但有两个限制：cmp 不能两个操作数都在内存；立即数只能
        // 放在第一操作数，直接写会把减法方向搞反。
        std::string sa = src_op(in.a);
        std::string sb = src_op(in.b);
        bool sa_mem = is_mem_operand(sa);
        bool sb_mem = is_mem_operand(sb);
        if (sa_mem && sb_mem) {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        } else if (sa_mem || sa[0] == '%') {
            line(std::format("cmpq {}, {}", sb, sa));
        } else {
            line(std::format("movq {}, %rax", sa));
            line(std::format("cmpq {}, %rax", sb));
        }
        switch (in.cop) {
            case CmpKind::Lt: line("setl %al"); break;
            case CmpKind::Le: line("setle %al"); break;
            case CmpKind::Gt: line("setg %al"); break;
            case CmpKind::Ge: line("setge %al"); break;
            case CmpKind::Eq: line("sete %al"); break;
            case CmpKind::Ne: line("setne %al"); break;
        }
        line("movzbq %al, %rax");
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_cmp_fp(const Inst& in) {
        line(std::format("movsd {}, %xmm0", src_op(in.a)));
        line(std::format("ucomisd {}, %xmm0", src_op(in.b)));
        switch (in.cop) {
            case CmpKind::Lt:
                line("setb %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Le:
                line("setbe %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Gt:
                line("seta %al");
                break;
            case CmpKind::Ge:
                line("setae %al");
                break;
            case CmpKind::Eq:
                line("sete %al");
                line("setnp %cl");
                line("andb %cl, %al");
                break;
            case CmpKind::Ne:
                line("setne %al");
                line("setp %cl");
                line("orb %cl, %al");
                break;
        }
        line("movzbq %al, %rax");
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

void AsmGen::gen_convert(const Inst& in) {
        TypePtr from = ty_of(in.a);
        TypePtr to = in.ty;
        if (!to) return;
        if (to->kind == TyKind::Bool) {
            emit_truth(in.a, "%al");
            line("movzbq %al, %rax");
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        if (to->kind == TyKind::Float) {
            if (ir_is_float(from)) {
                // 已经是 double，只搬一次
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
                return;
            }
            // cvtsi2sd 的源必须是寄存器或内存，立即数不能直接转
            std::string sa = src_op(in.a);
            if (!is_mem_operand(sa) && sa[0] != '%') {
                line(std::format("movq {}, %rax", sa));
                sa = "%rax";
            }
            line(std::format("cvtsi2sdq {}, %xmm0", sa));
            line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            return;
        }
        if (to->kind == TyKind::Int) {
            if (ir_is_float(from)) {
                line(std::format("cvttsd2siq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
            } else {
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
            }
            return;
        }
        if (to->kind == TyKind::Str) {
            // 源类型已经是 str：什么都不用做，只补一条搬运
            if (from && from->kind == TyKind::Str) {
                line(std::format("movq {}, %rax", src_op(in.a)));
                line(std::format("movq %rax, {}", dst_op(in.dst)));
                return;
            }
            if (ir_is_float(from)) {
                line(std::format("movsd {}, %xmm0", src_op(in.a)));
                spill_fp_slots();
                line("call py23_str_from_f64");
                reload_fp_slots();
            } else if (from && from->kind == TyKind::Bool) {
                line(std::format("movq {}, %rdi", src_op(in.a)));
                spill_fp_slots();
                line("call py23_str_from_bool");
                reload_fp_slots();
            } else if (from && from->kind == TyKind::None) {
                spill_fp_slots();
                line("call py23_str_from_none");
                reload_fp_slots();
            } else {
                line(std::format("movq {}, %rdi", src_op(in.a)));
                spill_fp_slots();
                line("call py23_str_from_i64");
                reload_fp_slots();
            }
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
    }

}  // namespace py23
