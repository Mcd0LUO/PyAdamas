// codegen_ops.cpp —— 指令选择：算术/比较/转换
#include "codegen_impl.h"

namespace pyadamas {

// idiv 的除数必须在 %rcx。SysV 直接用它；Win64 下 %rcx 是第 1 个实参
// 寄存器，而除零检查里的 panic 是调用、会破坏它，所以统一用 %r10 中转。
static const char* div_reg() { return target_abi().gnu_cfi ? "%rcx" : "%r10"; }


// ── 指令 ────────────────────────────────────────────────────
void AsmGen::set_loc(int line) {
    if (line <= 0 || line == cur_loc_line_ || !m_ || m_->source_path.empty()) return;
    cur_loc_line_ = line;
    // 行变化处放一个本地标签，运行时表把 PC 映射回 (函数, 行)。
    std::string lab = std::format(".Ldbg{}", dbg_seq_++);
    out_ += lab + ":\n";
    out_ += std::format("\t.loc 1 {}\n", line);
    dbg_entries_.push_back({lab, fi_, line});
}

void AsmGen::gen_inst(const Inst& in) {
        set_loc(in.line);
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
            case IrOp::StoreSlot: {
                // split 模式下位置按块查表（slot_op 内部处理）。
                //
                // **不做双写。** 块内只写寄存器，栈的同步交给块出口的
                // spill（见 codegen_block.cpp 的 slot_spill_out_）。
                //
                // 双写方案实测有严重退化：spectral_norm 从 573ms 涨到
                // 1006ms（+76%）。原因是双写把一次 store 变成两次内存操作，
                // 而且打断 store-to-load forwarding。
                const bool sp = !slot_loc_.empty();
                const std::string& loc = slot_op(in.idx);
                move_to(loc, src_op(in.a));
                (void)sp;
                invalidate_slot(in.idx);
                break;
            }
            case IrOp::LoadGlobal:
                out_ += "\tmovq ";
                out_ += global_label(in.idx);
                out_ += "(%rip), %rax\n";
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::StoreGlobal:
                emit2("movq", src_op(in.a), "%rax");
                out_ += "\tmovq %rax, ";
                out_ += global_label(in.idx);
                out_ += "(%rip)\n";
                break;
            case IrOp::GlobalAddr:
                out_ += "\tleaq ";
                out_ += global_label(in.idx);
                out_ += "(%rip), %rax\n";
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::ClassAddr:
                out_ += std::format("\tleaq .Lvt{}(%rip), %rax\n", in.imm);
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::MakeList: gen_make_list(in); break;
            case IrOp::MakeTuple: gen_make_list(in); break;
            case IrOp::IndexLoad: gen_index_load(in); break;
            case IrOp::IndexLoadUnchecked: gen_index_unchecked_load(in); break;
            case IrOp::IndexStore: gen_index_store(in); break;
            case IrOp::Len:
                emit2("movq", src_op(in.a), "%rax");
                line("movq (%rax), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::ObjNew: gen_obj_new(in); break;
            case IrOp::StructNew: gen_struct_new(in); break;
            case IrOp::StructCopy: gen_struct_copy(in); break;
            case IrOp::FieldLoad: gen_field_load(in); break;
            case IrOp::FieldStore: gen_field_store(in); break;
            case IrOp::PtrLoad: gen_ptr_load(in); break;
            case IrOp::PtrStore: gen_ptr_store(in); break;
            case IrOp::VCall: gen_vcall(in); break;
            case IrOp::VCallStatic: gen_vcall_static(in); break;
            case IrOp::SuperVCall: gen_super_vcall(in); break;
            case IrOp::CallDirect: gen_call_direct(in); break;
            case IrOp::CallPtr: gen_call_ptr(in); break;
            case IrOp::Call: gen_call(in); break;
            case IrOp::CallRt: gen_call_rt(in); break;
            case IrOp::CallExtern: gen_call_extern(in); break;
            case IrOp::LoadExtern: gen_load_extern(in); break;
            case IrOp::StoreExtern: gen_store_extern(in); break;
            case IrOp::Setjmp: gen_setjmp(in); break;
            case IrOp::EhBegin: gen_eh_begin(); break;
            case IrOp::EhEnd: gen_eh_end(); break;
            case IrOp::EhLandingPad: gen_eh_landing_pad(); break;
            case IrOp::ImportModule: {
                const std::string& name = m_->imports[static_cast<size_t>(in.imm)];
                line(std::format("leaq {}(%rip), {}", module_ready_symbol(name), arg_reg_i(0)));
                line(std::format("leaq {}(%rip), {}", module_init_symbol(name), arg_reg_i(1)));
                spill_fp_slots();
                line("call pa_import_module");
                reload_fp_slots();
                break;
            }
            case IrOp::Jump:
                if (!(opt_ >= 2 && in.target == cur_block_ + 1))
                    emit_jmp("jmp", block_label(in.target));
                break;
            case IrOp::Branch: gen_branch(in); break;
            case IrOp::Return: gen_return(in); break;
            case IrOp::SlotAddr: {
                int slot = in.idx;
                emit2("leaq", gen_ ? gen_smem(slot) : smem(slot), "%rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            }
            case IrOp::GenNew: {
                const auto& g = m_->generators[static_cast<size_t>(in.imm)];
                // 四个实参按 ABI 位置走：n_slots / resume / gcrel / frame_kind。
                // Win64 只有 4 个整型实参寄存器，这里正好用满；第 4 个在
                // SysV 是 %rcx，在 Win64 是 %r9。
                emit_imm_op("movq", g.n_slots, arg_reg_i(0));
                line(std::format("leaq {}(%rip), {}", func_label(g.resume), arg_reg_i(1)));
                line(std::format("leaq .Lgengcrel{}(%rip), {}", in.imm, arg_reg_i(2)));
                emit_imm_op("movq", g.frame_kind, arg_reg_i(3));
                line("call pa_gen_new");
                move_to(dst_op(in.dst), "%rax");
                break;
            }
            case IrOp::GenSetSlot:
                // 基址 %rax + 值暂存：值不能借实参寄存器（Win64 下 arg_reg_i(3)
                // 是 %r9，而这里根本没有调用），走 ABI 的 scratch_field。
                emit2("movq", src_op(in.args[0]), "%rax");
                emit2("movq", src_op(in.args[1]), target_abi().scratch_field);
                line(std::format("movq {}, {}(%rax)", target_abi().scratch_field,
                                 8 * (10 + in.idx)));
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
            case IrOp::GenTakeExc:
                line("movq 64(%r15), %rax");
                line("movq $0, 64(%r15)");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenSent:
                line("movq 16(%r15), %rax");
                move_to(dst_op(in.dst), "%rax");
                break;
            case IrOp::GenYield:
                if (in.a != kNoReg) emit2("movq", src_op(in.a), "%rax");
                else line("xorl %eax, %eax");
                line("movq %rax, 24(%r15)");
                emit_imm_mem("0(%r15)", in.imm);
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
            case IrOp::GenAwait:
                if (in.a != kNoReg) emit2("movq", src_op(in.a), "%rax");
                else line("xorl %eax, %eax");
                line("movq %rax, 24(%r15)");
                emit_imm_mem("0(%r15)", in.imm);
                line("movq $3, 8(%r15)");
                line("xorl %eax, %eax");
                emit_epilogue();
                break;
            case IrOp::GenRethrow:
                line(std::format("movq %r15, {}", arg_reg_i(0)));
                line("call pa_coro_rethrow");
                line("ud2");
                break;
            case IrOp::GenDone:
                if (in.a != kNoReg) {
                    emit2("movq", src_op(in.a), "%rax");
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
        // split 模式下位置按块查表；默认模式保持原行为。
        std::string sop = slot_op(in.idx);
        // 融合目标非空时**必须**真的写过去，不能走别名——别名只是
        // 让 %v 指向变量的位置，而 `store.slot s, %v` 已经被吞掉，
        // 变量的位置不会有任何变化，值就悄悄丢了。
        // split 模式下禁用别名优化：别名让 vreg 直接指向槽的位置，
        // 而 split 的位置是**按块**的，跨块或块内 store 之后会失效。
        if (slot_loc_.empty() && dst_loc_.empty() && in.dst != kNoReg &&
            in.dst < use_count_.size() &&
            use_count_[in.dst] == 1 && !cross_block_[in.dst] &&
            (opt_ >= 1 || sop[0] == '%')) {
            // 读之后、第一次真正使用之前不能有对该变量的赋值。
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
        const std::string& d = dst_op(in.dst);
        if (in.imm >= INT32_MIN && in.imm <= INT32_MAX) {
            emit_imm_op("movq", in.imm, d);
        } else {
            char buf[40];
            auto r = std::to_chars(buf, buf + sizeof buf, static_cast<uint64_t>(in.imm), 16);
            out_ += "\tmovabsq $0x";
            out_.append(buf, r.ptr);
            out_ += ", %rax\n";
            emit2("movq", "%rax", d);
        }
    }

void AsmGen::gen_const_float(const Inst& in) {
        uint64_t bits = 0;
        std::memcpy(&bits, &in.fimm, sizeof(bits));
        line(std::format("movabsq $0x{:x}, %rax", bits));
        line("movq %rax, %xmm0");
        move_fp(dst_op(in.dst), "%xmm0");
    }

void AsmGen::gen_bin(const Inst& in) {
        TypePtr ty = in.ty;
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::Bytes)) {
            if (in.bop != BinKind::Add) {
                line("# unsupported string operator");
                return;
            }
            emit2("movq", src_op(in.a), arg_reg_i(0));
            emit2("movq", src_op(in.b), arg_reg_i(1));
            spill_fp_slots();
            // str 的 pub 相加、bytes 的 pub = 字节数，两个入口分开
            line(ty->kind == TyKind::Bytes ? "call pa_bytes_concat" : "call pa_str_concat");
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
                case BinKind::Add: emit2("addq", sb, d); return;
                case BinKind::Sub: emit2("subq", sb, d); return;
                case BinKind::Mul: emit2("imulq", sb, d); return;
                case BinKind::BitAnd: emit2("andq", sb, d); return;
                case BinKind::BitOr: emit2("orq", sb, d); return;
                case BinKind::BitXor: emit2("xorq", sb, d); return;
                // 移位量必须落在 %cl（x86 硬性要求），不能按实参位置装填：
                // Win64 的 arg_reg_i(3) 是 %r9。SysV 下两者同为 %rcx。
                case BinKind::Shl:
                    emit2("movq", sb, "%rcx");
                    emit2("shlq", "%cl", d);
                    return;
                case BinKind::Shr:
                    emit2("movq", sb, "%rcx");
                    emit2("sarq", "%cl", d);
                    return;
                default: break;  // 除法/取模/幂要用 rax/rdx，走通用路径
            }
        }

        emit2("movq", sa, "%rax");
        switch (in.bop) {
            case BinKind::Add: emit2("addq", sb, "%rax"); break;
            case BinKind::Sub: emit2("subq", sb, "%rax"); break;
            case BinKind::Mul: emit2("imulq", sb, "%rax"); break;
            case BinKind::BitAnd: emit2("andq", sb, "%rax"); break;
            case BinKind::BitOr: emit2("orq", sb, "%rax"); break;
            case BinKind::BitXor: emit2("xorq", sb, "%rax"); break;
            case BinKind::Shl:
                emit2("movq", sb, "%rcx");
                line("shlq %cl, %rax");
                break;
            case BinKind::Shr:
                emit2("movq", sb, "%rcx");
                line("sarq %cl, %rax");
                break;
            case BinKind::Div:
                emit2("movq", sb, div_reg());
                gen_div_guard();
                line("cqto");
                if (!target_abi().gnu_cfi) line("movq %r10, %rcx");
                line("idivq %rcx");
                break;
            case BinKind::Mod: gen_mod(in); break;
            case BinKind::FloorDiv: gen_floordiv(in); break;
            case BinKind::Pow:
                emit2("movq", sa, arg_reg_i(0));
                emit2("movq", sb, arg_reg_i(1));
                spill_fp_slots();
                line("call pa_pow_i64");
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
        line(std::format("testq {}, {}", div_reg(), div_reg()));
        emit_jmp("jne", ok);
        line("call pa_panic_div0");
        label(ok);
        line(std::format("cmpq $-1, {}", div_reg()));
        emit_jmp("jne", ok2);
        line("movabsq $0x8000000000000000, %r8");
        line("cmpq %r8, %rax");
        emit_jmp("jne", ok2);
        line("call pa_panic_overflow");
        label(ok2);
    }

// x86 的 idiv 向零取整，Python 的 // 向下取整，余数符号也不同。
    // 差 1 的修正必须做对，否则负数的 // 和 % 全是错的。
void AsmGen::gen_floordiv(const Inst& in) {
        std::string done = fresh_label("fdiv");
        emit2("movq", src_op(in.b), div_reg());
        gen_div_guard();
        line("cqto");
        if (!target_abi().gnu_cfi) line("movq %r10, %rcx");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        emit_jmp("je", done);
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        emit_jmp("jns", done);
        line("decq %rax");
        label(done);
    }

// Python 的 % 结果与除数同号（-7 % 3 == 2），x86 的余数与被除数同号，
    // 差一个除数，需要补回来。
void AsmGen::gen_mod(const Inst& in) {
        std::string done = fresh_label("mod");
        emit2("movq", src_op(in.b), div_reg());
        gen_div_guard();
        line("cqto");
        if (!target_abi().gnu_cfi) line("movq %r10, %rcx");
        line("idivq %rcx");
        line("testq %rdx, %rdx");
        emit_jmp("je", done);
        line("movq %rdx, %r8");
        line("xorq %rcx, %r8");
        emit_jmp("jns", done);
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
        if (opt_ >= 1 && simple && d.starts_with("%xmm")) {
            std::string sa = src_op(in.a);
            if (sa != d) move_fp(d, sa);
            switch (in.bop) {
                case BinKind::Add: emit2("addsd", src_op(in.b), d); break;
                case BinKind::Sub: emit2("subsd", src_op(in.b), d); break;
                case BinKind::Mul: emit2("mulsd", src_op(in.b), d); break;
                case BinKind::Div: emit2("divsd", src_op(in.b), d); break;
                default: break;
            }
            return;
        }
        move_fp("%xmm0", src_op(in.a));
        switch (in.bop) {
            case BinKind::Add: emit2("addsd", src_op(in.b), "%xmm0"); break;
            case BinKind::Sub: emit2("subsd", src_op(in.b), "%xmm0"); break;
            case BinKind::Mul: emit2("mulsd", src_op(in.b), "%xmm0"); break;
            case BinKind::Div: emit2("divsd", src_op(in.b), "%xmm0"); break;
            case BinKind::FloorDiv:
                // floor(a / b)：先除再向下取整。roundsd 要 SSE4.1，
                // 为了兼容性走 libm。
                emit2("divsd", src_op(in.b), "%xmm0");
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
                emit2("movsd", src_op(in.b), "%xmm1");
                spill_fp_slots();
                line("call fmod");
                reload_fp_slots();
                emit2("movsd", src_op(in.b), "%xmm1");
                line("xorpd %xmm2, %xmm2");
                line("ucomisd %xmm2, %xmm0");
                emit_jmp("jp", done);  // NaN 原样返回
                emit_jmp("jne", nz);
                line("movq %xmm1, %rdx");
                line("movabsq $0x8000000000000000, %rax");
                line("andq %rdx, %rax");
                line("movq %rax, %xmm0");  // ±0.0，符号跟除数
                emit_jmp("jmp", done);
                label(nz);
                line("movq %xmm0, %rax");
                line("movq %xmm1, %rdx");
                line("xorq %rdx, %rax");
                emit_jmp("jns", done);  // 同号，无需修正
                line("addsd %xmm1, %xmm0");
                label(done);
                break;
            }
            case BinKind::Pow:
                emit2("movsd", src_op(in.b), "%xmm1");
                spill_fp_slots();
                line("call pow");
                reload_fp_slots();
                break;
            default:
                line("# unsupported float operator");
                break;
        }
        move_fp(dst_op(in.dst), "%xmm0");
    }

void AsmGen::gen_un(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (in.uop == UnKind::Not) {
            emit_truth(in.a, "%al");
            line("xorb $1, %al");
            line("movzbq %al, %rax");
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (ir_is_float(ty)) {
            if (in.uop == UnKind::Neg) {
                move_fp("%xmm0", src_op(in.a));
                line("movabsq $0x8000000000000000, %rax");
                line("movq %rax, %xmm1");
                line("xorpd %xmm1, %xmm0");
                move_fp(dst_op(in.dst), "%xmm0");
            }
            return;
        }
        emit2("movq", src_op(in.a), "%rax");
        switch (in.uop) {
            case UnKind::Neg: line("negq %rax"); break;
            case UnKind::BitNot: line("notq %rax"); break;
            default: break;
        }
        emit2("movq", "%rax", dst_op(in.dst));
    }

// 把一个值算成 0/1 放进 %al。浮点的 NaN 视为真（Python 语义），
    // 所以用 setne + setp 的组合。
void AsmGen::emit_truth(VReg r, const char* byte_reg) {
        (void)byte_reg;
        TypePtr ty = ty_of(r);
        if (ir_is_float(ty)) {
            move_fp("%xmm0", src_op(r));
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
            emit2("movq", src_op(r), "%rax");
            line("cmpq $0, (%rax)");
            line("setne %al");
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            emit2("movq", src_op(r), "%rax");
            line("xorpd %xmm1, %xmm1");
            line("ucomisd 8(%rax), %xmm1");
            line("setne %al");
            line("ucomisd 16(%rax), %xmm1");
            line("setne %cl");
            line("orb %cl, %al");
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            emit2("movq", src_op(r), "%rax");
            line("xorpd %xmm1, %xmm1");
            line("ucomisd 8(%rax), %xmm1");
            line("setne %al");
            line("ucomisd 16(%rax), %xmm1");
            line("setne %cl");
            line("orb %cl, %al");
            return;
        }
        std::string s = src_op(r);
        // 立即数不能直接进 cmpq 的源位置：先落 %rax。常量条件一般会被
        // 分支折叠吃掉，这里只兜底（如 bool(x) 的 x 是常量）。
        if (!s.empty() && s[0] == '$') {
            emit2("movq", s, "%rax");
            s = "%rax";
        }
        emit2("cmpq", "$0", s);
        line("setne %al");
    }

void AsmGen::gen_cmp(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::Bytes)) {
            emit2("movq", src_op(in.a), arg_reg_i(0));
            emit2("movq", src_op(in.b), arg_reg_i(1));
            if (in.cop == CmpKind::Eq || in.cop == CmpKind::Ne) {
                spill_fp_slots();
                line("call pa_str_eq");
                reload_fp_slots();
                if (in.cop == CmpKind::Ne) line("xorq $1, %rax");
                emit2("movq", "%rax", dst_op(in.dst));
                return;
            }
            // 大小比较走三路比较。早先这里只留了一句注释、什么都不生成，
            // 于是 `a < b` 静默拿到槽里的残留值——恒为 False。
            spill_fp_slots();
            line("call pa_str_cmp");
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
            emit2("movq", "%rax", dst_op(in.dst));
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
            emit2("movq", sa, "%rax");
            emit2("cmpq", sb, "%rax");
        } else if (sa_mem || sa[0] == '%') {
            emit2("cmpq", sb, sa);
        } else {
            emit2("movq", sa, "%rax");
            emit2("cmpq", sb, "%rax");
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
        move_fp("%xmm0", src_op(in.a));
        emit2("ucomisd", src_op(in.b), "%xmm0");
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
        emit2("movq", "%rax", dst_op(in.dst));
    }

void AsmGen::gen_convert(const Inst& in) {
        TypePtr from = ty_of(in.a);
        TypePtr to = in.ty;
        if (!to) return;
        if (to->kind == TyKind::Bool) {
            emit_truth(in.a, "%al");
            line("movzbq %al, %rax");
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (to->kind == TyKind::Float) {
            if (ir_is_float(from)) {
                // 已经是 double，只搬一次
                move_fp(dst_op(in.dst), src_op(in.a));
                return;
            }
            // cvtsi2sd 的源必须是寄存器或内存，立即数不能直接转
            std::string sa = src_op(in.a);
            if (!is_mem_operand(sa) && sa[0] != '%') {
                emit2("movq", sa, "%rax");
                sa = "%rax";
            }
            emit2("cvtsi2sdq", sa, "%xmm0");
            move_fp(dst_op(in.dst), "%xmm0");
            return;
        }
        if (to->kind == TyKind::Int) {
            if (ir_is_float(from)) {
                emit2("cvttsd2siq", src_op(in.a), "%rax");
                emit2("movq", "%rax", dst_op(in.dst));
            } else {
                emit2("movq", src_op(in.a), "%rax");
                emit2("movq", "%rax", dst_op(in.dst));
            }
            return;
        }
        if (to->kind == TyKind::Str) {
            // 源类型已经是 str：什么都不用做，只补一条搬运
            if (from && from->kind == TyKind::Str) {
                emit2("movq", src_op(in.a), "%rax");
                emit2("movq", "%rax", dst_op(in.dst));
                return;
            }
            if (ir_is_float(from)) {
                move_fp("%xmm0", src_op(in.a));
                spill_fp_slots();
                line("call pa_str_from_f64");
                reload_fp_slots();
            } else if (from && from->kind == TyKind::Bool) {
                emit2("movq", src_op(in.a), arg_reg_i(0));
                spill_fp_slots();
                line("call pa_str_from_bool");
                reload_fp_slots();
            } else if (from && from->kind == TyKind::None) {
                spill_fp_slots();
                line("call pa_str_from_none");
                reload_fp_slots();
            } else {
                emit2("movq", src_op(in.a), arg_reg_i(0));
                spill_fp_slots();
                line("call pa_str_from_i64");
                reload_fp_slots();
            }
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
    }


// ── 零开销异常（Z2-b2）：保护区 / landing pad 标记的发射 ─────────────
void AsmGen::gen_eh_begin() {
    // 保护区起点：标签地址 = 紧随其后的第一条指令。
    //
    // 同时标记「进入表驱动展开区」：C 运行时抛的异常（IndexError /
    // ZeroDivisionError 等）走的是 exc_longjmp，它据此改走 _Unwind，
    // 否则 setjmp 路径的 handler 栈抓不到这些异常。
    if (eh_begin_seq_ < eh_begin_labels_.size())
        label(eh_begin_labels_[eh_begin_seq_++]);
    line("call pa_eh_enter");
}

void AsmGen::gen_eh_end() {
    // 保护区终点。先退标记再落标签：标签之后的代码已不在保护区里。
    line("call pa_eh_leave");
    if (eh_end_seq_ < eh_end_labels_.size())
        label(eh_end_labels_[eh_end_seq_++]);
}

void AsmGen::gen_eh_landing_pad() {
    // landing pad 入口：%rax 是展开包装指针（personality 的 GR0）；紧随其后的
    // CallRt EhLand 会把它作为第一个实参传给 pa_eh_land。
    if (eh_lp_seq_ < eh_lp_labels_.size()) label(eh_lp_labels_[eh_lp_seq_++]);
}

}  // namespace pyadamas
