// codegen_call.cpp —— 调用、对象、字段与分支/返回
//
// C 互操作边界编组（gen_call_extern / gen_load_extern / gen_store_extern /
// gen_ptr_load / gen_ptr_store）在 codegen_cffi.cpp（P1 收尾拆分，纯移动）。
#include "codegen_impl.h"

namespace pyadamas {

void AsmGen::gen_make_list(const Inst& in) {
        // **定长小元组的快路径。**
        //
        // `tuple.new{a, b}` 原本编译成 pa_list_new + N 次 pa_list_append：
        // N+1 次函数调用，每次 append 还要走容量检查 + 位图维护。实测
        // queue_sim 每次元组分配 265ns，函数调用开销占大头。
        //
        // 这里对「元素个数 <= 4 且全是不含浮点的整型机器字」的构造走一次
        // 调用：pa_list_new_filled(n, kind, a, b, c, d)。条件不满足就退回
        // 原路径 —— 浮点元素要走 xmm 寄存器、堆引用要逐元素传堆标记，都
        // 不适合这个固定签名的入口。
        //
        // 正确性要点：
        //   - 只在 elem_kind 是 0（全非堆）或 1（全堆）时走快路径；
        //     kind 2（unknown）需要逐元素堆标记，保持原路径；
        //   - 元素的类型必须全部是「非浮点的标量或堆指针」，浮点元素会
        //     被 move 到 xmm，混进整型实参寄存器会算错；
        //   - 所有权语义与 pa_list_append 一致：调用方已把引用转交给容器。
        if (in.op == IrOp::MakeTuple && !in.args.empty() && in.args.size() <= 4 &&
            (in.imm == 0 || in.imm == 1)) {
            bool all_word = true;
            for (VReg a : in.args) {
                TypePtr t = ty_of(a);
                if (!t || ir_is_float(t)) {
                    all_word = false;
                    break;
                }
            }
            if (all_word) {
                // kind-0 元组内联快路径（定义在 codegen_call_rt.cpp）。
                if (in.op == IrOp::MakeTuple && in.imm == 0) {
                    gen_tuple_inline(in);
                    return;
                }
                emit_imm(MOp::kMovq, static_cast<int64_t>(in.args.size()), arg_reg_i(0));
                emit_imm(MOp::kMovq, in.imm, arg_reg_i(1));
                for (size_t ai = 0; ai < 4; ++ai) {
                    if (ai < in.args.size()) emit2(MOp::kMovq, src_mop(in.args[ai]), arg_reg_i(2 + ai));
                    else emit_imm(MOp::kMovq, 0, arg_reg_i(2 + ai));
                }
                spill_fp_slots();
                // 全堆元素的定长列表仍走运行时（tracked，需要 rc_link）。
                line("call pa_list_new_filled");
                reload_fp_slots();
                emit2(MOp::kMovq, kRax, dst_mop(in.dst));
                return;
            }
        }
        // imm 是元素所有权类别（0 非堆 / 1 堆 / 2 unknown 或混合），
        // 由降级器按容器类型算好，运行时据此决定覆盖和销毁时是否 dec。
        emit_imm(MOp::kMovq, static_cast<int64_t>(in.args.size()), arg_reg_i(0));
        emit_imm(MOp::kMovq, in.imm, arg_reg_i(1));
        spill_fp_slots();
        line("call pa_list_new");
        reload_fp_slots();
        emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        // 第三个参数是**逐元素**的堆标记（0 非堆 / 1 堆 / 2 unknown），
        // 运行时用它维护列表的 heap_bits 位图，销毁时按位图 dec，
        // 不必再查全局 rc_index（实测省掉 bench_rc_gen 一半的 dec 成本）。
        //
        // in.imm 是容器整体的所有权类别，元素级别要按各自静态类型算：
        //   - imm==0（全非堆）：每个元素都是 0；
        //   - imm==1（全堆）：每个元素都是 1；
        //   - imm==2（混合/unknown）：逐元素看 vreg_ty，拿不到就退回 2。
        for (size_t ai = 0; ai < in.args.size(); ++ai) {
            VReg a = in.args[ai];
            int64_t hk = 2;
            if (in.imm == 0) hk = 0;
            else if (in.imm == 1) hk = 1;
            else {
                TypePtr et = ty_of(a);
                if (et && et->kind != TyKind::Unknown)
                    hk = ir_is_heap(et) ? 1 : 0;
            }
            emit2(MOp::kMovq, dst_mop(in.dst), arg_reg_i(0));
            emit2(MOp::kMovq, src_mop(a), arg_reg_i(1));
            // P3：unknown 元素带 tag 时堆标志从 tag 导出（保住位图）。
            if (tags_enabled_ && hk == 2 && vreg_tagged(a))
                emit_heap_from_tag(a, arg_reg_i(2));
            else
                emit_imm(MOp::kMovq, hk, arg_reg_i(2));
            spill_fp_slots();
            line("call pa_list_append");
            reload_fp_slots();
        }
    }

void AsmGen::gen_index_load(const Inst& in) {
        TypePtr ot = ty_of(in.a);
        if (ot && ot->kind == TyKind::Str) {
            // ── str 下标 ASCII 快路径内联（0.287）──────────────────
            // 0 <= i < pub 且 pub==len（全 ASCII 不变量）且字节 <0x80
            // 时直接查 pa_char_tab 驻留表（main 序言已初始化），省两层
            // call（char_at + char_str）。负索引/越界/非 ASCII 落回运行
            // 时（归一化、IndexError、UTF-8 码点路径语义不变）。
            if (opt_ >= 1) {
                gen_str_index_inline(in);
                return;
            }
            emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
            emit2(MOp::kMovq, src_mop(in.b), arg_reg_i(1));
            spill_fp_slots();
            line("call pa_str_char_at");
            reload_fp_slots();
            emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            return;
        }
        if (opt_ < 1) {
            emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
            emit2(MOp::kMovq, src_mop(in.b), arg_reg_i(1));
            line("call pa_list_get");
            emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            return;
        }
        // list/tuple 共用 PyAdamasList 布局：+0 len，+16 data。
        // 实参一律走 ABI 寄存器；中间值用 ABI 提供的 scratch（必须避开
        // 实参寄存器，Win64 下 rcx/rdx/r8/r9 正好是前四个实参）。
        //
        // **热路径只留一次无符号比较。**
        //
        // 原实现每次索引访问 19 条指令：热路径 cmp+jb，然后 testq+js 判负，
        // 负路径 addq+cmp+jb，两个分支各自 `call pa_list_get` + `ud2`。
        // 而 pa_list_get 本身就完整处理了负索引和越界：
        //     if (i < 0) i += l->len;
        //     if (i < 0 || i >= l->len) pa_raise_c("IndexError", ...);
        //     return l->data[i];
        // 也就是说内联的热路径只是它的快速副本，慢路径完全可以整段交给它。
        //
        // 改成：`cmpq len, idx; jb fast` —— 无符号比较，`jb` 成立等价于
        // `0 <= idx < len`（负数按无符号看是极大值，必然不成立）。
        // 成立走 4 条内联取址；不成立直接 `call pa_list_get`，负索引、越界
        // 全部由它处理。
        //
        // **两条路径都要按类型落位**：pa_list_get 返回 int64（浮点元素的
        // 位模式在 %rax），热路径读的是 double 位模式。第一版只在热路径
        // 做了浮点落位，冷路径直接落到 done —— 浮点列表走负索引时读到
        // 垃圾（15_slice 打出 94858846548000，43_indexing SIGSEGV）。
        const MReg r0 = mreg_from_name(arg_reg_i(0)), r1 = mreg_from_name(arg_reg_i(1));
        const MReg sl = mreg_from_name(target_abi().scratch_len);
        const MReg sp = mreg_from_name(target_abi().scratch_ptr);
        std::string fast = fresh_label("idxfast");
        std::string done = fresh_label("idxdone");
        emit2(MOp::kMovq, src_mop(in.a), r0);
        emit2(MOp::kMovq, src_mop(in.b), r1);
        emit2(MOp::kMovq, op_mem(r0, 0), sl);
        emit2(MOp::kCmpq, sl, r1);
        emit_jmp(MOp::kJb, fast);
        // 冷路径：负索引 / 越界 / 类型不确定，全交给运行时。
        line("call pa_list_get");
        if (ir_is_float(in.ty)) {
            emit2(MOp::kMovq, kRax, kXmm0);
            move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
        } else {
            move_to(dst_mop(in.dst), mreg_operand(kRax));
        }
        gen_index_load_tag(in, false);
        emit_jmp(MOp::kJmp, done);
        label(fast);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        if (ir_is_float(in.ty)) {
            emit2(MOp::kMovsd, op_mem(sp, r1, 8), kXmm0);
            move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
        } else {
            emit2(MOp::kMovq, op_mem(sp, r1, 8), kRax);
            move_to(dst_mop(in.dst), mreg_operand(kRax));
        }
        gen_index_load_tag(in, true);
        label(done);
    }

void AsmGen::gen_index_unchecked_load(const Inst& in) {
        // 调用方（for / 推导式 / sum 等）已经证明 0 <= idx < len，
        // 这里只做一次 data 取址和元素装载。
        const MReg r0 = mreg_from_name(arg_reg_i(0)), r1 = mreg_from_name(arg_reg_i(1));
        const MReg sp = mreg_from_name(target_abi().scratch_ptr);
        emit2(MOp::kMovq, src_mop(in.a), r0);
        emit2(MOp::kMovq, src_mop(in.b), r1);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        if (ir_is_float(in.ty)) {
            emit2(MOp::kMovsd, op_mem(sp, r1, 8), kXmm0);
            move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
        } else {
            emit2(MOp::kMovq, op_mem(sp, r1, 8), kRax);
            move_to(dst_mop(in.dst), mreg_operand(kRax));
        }
        gen_index_load_tag(in, true);
    }


    // 属性访问因此是一条固定偏移的 movq，没有查表、没有哈希。
void AsmGen::gen_obj_new(const Inst& in) {
        const IRClass& c = m_->classes[static_cast<size_t>(in.imm)];
        // 动态属性槽挂在字段之后，跟着类走（派生类的偏移不同）。
        emit_imm_op("movq", 8 * (c.n_fields + 1) + (c.dict_off ? 8 : 0), arg_reg_i(0));
        bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
        // 实参按 ABI 位置装填：SysV 是 rdi/rsi/rdx，Win64 是 rcx/rdx/r8。
        // 写死 %rsi/%rdx 会让 Win64 的 dtor 收成 gcrel、release 收到垃圾。
        if (has_heap || c.dict_off != 0) {
            line(std::format("leaq .Ldtor{}(%rip), {}", in.imm, arg_reg_i(1)));
            line(std::format("leaq .Lgcrel{}(%rip), {}", in.imm, arg_reg_i(2)));
        } else {
            line(std::format("xorl {}, {}", reg32(arg_reg_i(1)), reg32(arg_reg_i(1))));
            line(std::format("xorl {}, {}", reg32(arg_reg_i(2)), reg32(arg_reg_i(2))));
        }
        spill_fp_slots();
        line("call pa_obj_new");
        reload_fp_slots();
        line(std::format("leaq .Lvt{}(%rip), %rcx", in.imm));
        line("movq %rcx, (%rax)");
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_field_load(const Inst& in) {
        emit2(MOp::kMovq, src_mop(in.a), kRax);
        if (in.fwidth == 1) {
            // signed char 字段符号扩展；Bool/无符号仍零扩展（旧行为）。
            if (in.ty && in.ty->kind == TyKind::I8)
                emit2(MOp::kMovsbq, op_mem(kRax, in.imm), kRax);
            else
                emit2(MOp::kMovzbq, op_mem(kRax, in.imm), kRax);
        } else if (in.fwidth == 2) {
            // MReg 表没有 16 位寄存器名，裸发射（与 ptr load 同口径）。
            if (in.ty && in.ty->kind == TyKind::I16)
                line(std::format("movswq {}(%rax), %rax", in.imm));
            else
                line(std::format("movzwq {}(%rax), %rax", in.imm));
        } else if (in.fwidth == 4 && in.funsigned)
            emit2(MOp::kMovl, op_mem(kRax, in.imm), kEax);
        else if (in.fwidth == 4)
            emit2(MOp::kMovslq, op_mem(kRax, in.imm), kRax);
        else
            emit2(MOp::kMovq, op_mem(kRax, in.imm), kRax);
        move_to(dst_mop(in.dst), mreg_operand(kRax));
        // P3-1b：unknown 字段装载一次探针导 tag（窄字段是 C 结构体 typed）。
        if (vreg_tagged(in.dst)) emit_tag_probe(dst_op(in.dst), tag_mem(in.dst));
    }

void AsmGen::gen_field_store(const Inst& in) {
        // 基址固定用 %rax；值另借一个 ABI 给定的 scratch（SysV 是 %rcx，
        // Win64 是 %r10 —— %rcx 在 Win64 是第 1 个实参，不能当暂存）。
        const TargetABI& abi = target_abi();
        emit2(MOp::kMovq, src_mop(in.a), kRax);
        emit2(MOp::kMovq, src_mop(in.b), abi.scratch_field);
        const MReg v = mreg_from_name(abi.scratch_field);
        if (in.fwidth == 1)
            emit2(MOp::kMovb, mreg_from_name(reg8(abi.scratch_field)), op_mem(kRax, in.imm));
        else if (in.fwidth == 2) {
            // 16 位寄存器名按 32 位名推导：%ecx -> %cx，%r10d -> %r10w。
            std::string r32 = reg32(abi.scratch_field);
            std::string w16 = r32[1] == 'e' ? "%" + r32.substr(2)
                                            : r32.substr(0, r32.size() - 1) + "w";
            line(std::format("movw {}, {}(%rax)", w16, in.imm));
        } else if (in.fwidth == 4)
            emit2(MOp::kMovl, mreg_from_name(reg32(abi.scratch_field)), op_mem(kRax, in.imm));
        else
            emit2(MOp::kMovq, v, op_mem(kRax, in.imm));
    }

// 值类型：payload 前有隐藏的 RC 头，返回的指针就是 payload 地址。
void AsmGen::gen_struct_new(const Inst& in) {
        emit_imm(MOp::kMovq, in.imm, arg_reg_i(0));
        spill_fp_slots();
        line("call pa_struct_new");
        reload_fp_slots();
        move_to(dst_mop(in.dst), mreg_operand(kRax));
    }

void AsmGen::gen_struct_copy(const Inst& in) {
        emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
        emit_imm(MOp::kMovq, in.imm, arg_reg_i(1));
        spill_fp_slots();
        line("call pa_struct_copy");
        reload_fp_slots();
        move_to(dst_mop(in.dst), mreg_operand(kRax));
    }

void AsmGen::gen_call_direct(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);
        // P3-1b：门控内直调本地非生成器且形参对齐 → tag ABI（>0 才启用）。
        const int ptag_words = call_ptag_words(in);

        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            emit2(MOp::kMovq, src_mop(in.args[i]), kR10);
            emit2(MOp::kMovq, kR10, op_mem(kRsp, l.stack_off));
        }
        // P3-1b：unknown 形参的平行 tag 数组（栈实参之后、出参区内）。
        int ptag_base = -1;
        if (ptag_words > 0) ptag_base = emit_ptag_array(in, locs);
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                emit2(MOp::kMovsd, src_mop(in.args[i]), static_cast<MReg>(kXmm0 + l.reg));
            else
                emit2(MOp::kMovq, src_mop(in.args[i]), arg_reg_i(l.reg));
        }
        // 不读闭包环境的 callee 不需要 %r10；有 env 槽的才清零占位。
        const IRFunction* callee =
            (in.idx >= 0 && in.idx < static_cast<int>(m_->funcs.size()))
                ? m_->funcs[static_cast<size_t>(in.idx)].get()
                : nullptr;
        if (!callee || callee->env_slot >= 0) line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        // P2（type_tags）：tag 数组走 pa_ptag_in 侧信道（被调序言读走并清零），
        // 不再占用 %r11 —— 与跨模块/间接调用共用同一套机制。门控关时仍是
        // 0.328 的 %r11 + $pt 入口，逐字旧路径。
        if (type_tags_ && ptag_words > 0) {
            emit2(MOp::kLeaq, op_mem(kRsp, ptag_base), kR10);
            line("movq %r10, pa_ptag_in(%rip)");
            emit_imm(MOp::kMovq, ptag_words, "pa_ptag_len(%rip)");
        } else if (type_tags_) {
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        }
        spill_fp_slots();
        if (type_tags_) {
            emit_jmp(MOp::kCall, entry_ref(in.idx));
            reload_fp_slots();
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        } else {
            if (ptag_words > 0) {
                // tag 数组指针最后装填（此前所有指令都不碰 %r11），call tag 入口。
                emit2(MOp::kLeaq, op_mem(kRsp, ptag_base), kR11);
                emit_jmp(MOp::kCall, ptag_label(in.idx));
            } else {
                emit_jmp(MOp::kCall, entry_ref(in.idx));
            }
            reload_fp_slots();
        }
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
            else emit2(MOp::kMovq, kRax, dst_mop(in.dst));
            // P3-1b：unknown 返回的 tag 在 %rdx（被调 gen_return 发射；
            // 旧入口调用也发，读到的一定是有效 tag 或 UNK）。
            if (vreg_tagged(in.dst) && is_unk_ty(in.ty) && callee &&
                func_ret_tag(*callee))
                emit2(MOp::kMovq, kRdx, tag_mop(in.dst));
        }
    }

void AsmGen::gen_branch(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ir_is_float(ty)) {
            emit2(MOp::kMovsd, src_mop(in.a), kXmm0);
            emit2(MOp::kXorpd, kXmm1, kXmm1);
            emit2(MOp::kUcomisd, kXmm1, kXmm0);
            emit_jmp(MOp::kJne, block_label(in.target));
            emit_jmp(MOp::kJp, block_label(in.target));
            emit_jmp(MOp::kJmp, block_label(in.target2));
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            emit2(MOp::kMovq, src_mop(in.a), kRax);
            emit_imm(MOp::kCmpq, 0, op_mem(kRax));
            emit_jmp(MOp::kJne, block_label(in.target));
            emit_jmp(MOp::kJmp, block_label(in.target2));
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            emit2(MOp::kMovq, src_mop(in.a), kRax);
            emit2(MOp::kXorpd, kXmm1, kXmm1);
            emit2(MOp::kUcomisd, op_mem(kRax, 8), kXmm1);
            emit_jmp(MOp::kJne, block_label(in.target));
            emit2(MOp::kUcomisd, op_mem(kRax, 16), kXmm1);
            emit_jmp(MOp::kJne, block_label(in.target));
            emit_jmp(MOp::kJmp, block_label(in.target2));
            return;
        }
        const MOperand& sa = src_mop(in.a);
        // 被折叠成常量的条件（如 while True 的 1）不能进 cmpq 的立即数位置。
        if (sa.kind == MOperandKind::Imm) {
            long long v = static_cast<long long>(sa.imm);
            int tgt = v ? in.target : in.target2;
            if (!(opt_ >= 2 && tgt == cur_block_ + 1))
                emit_jmp(MOp::kJmp, block_label(tgt));
            return;
        }
        emit_imm(MOp::kCmpq, 0, sa);
        if (opt_ >= 2 && in.target2 == cur_block_ + 1) {
            emit_jmp(MOp::kJne, block_label(in.target));
        } else if (opt_ >= 2 && in.target == cur_block_ + 1) {
            emit_jmp(MOp::kJe, block_label(in.target2));
        } else {
            emit_jmp(MOp::kJne, block_label(in.target));
            emit_jmp(MOp::kJmp, block_label(in.target2));
        }
    }

void AsmGen::gen_return(const Inst& in) {
        if (in.a == kNoReg) {
            emit2(MOp::kXorl, kEax, kEax);
        } else if (is_fp(in.a)) {
            emit2(MOp::kMovsd, src_mop(in.a), kXmm0);
        } else {
            emit2(MOp::kMovq, src_mop(in.a), kRax);
        }
        // P3-1b：unknown 返回经 %rdx 带 tag（门控关/生成器空操作）。
        emit_ret_tag(in);
        emit_epilogue();
    }

// ── 调用 ────────────────────────────────────────────────────
void AsmGen::gen_call(const Inst& in) {
        // 间接调用走「统一调用约定」：第 i 个实参放第 i 个整型实参寄存器，
        // 返回值从 %rax 取。闭包 +0 指向的入口要么是 shim（签名含浮点时由
        // shim 重排到真实位置），要么是签名不含浮点的函数本体（统一约定与它
        // 的 ABI 恰好一致）。这样调用点不需要知道被调函数的真实签名 ——
        // 而在间接调用点上，实参类型常常是 unknown，本来就无从得知。
        //
        // 浮点实参按位模式搬进整型寄存器（movq），不改变位，由 shim 那边
        // 读回后解释为 double。
        emit2(MOp::kMovq, src_mop(in.a), kR11);
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& ty = ty_of(in.args[i]);
            if (i < static_cast<size_t>(kMaxIntArgs())) {
                if (ir_is_float(ty)) {
                    // 浮点值在 %xmm 里，先搬到位模式再进整型寄存器。
                    emit2(MOp::kMovq, src_mop(in.args[i]), kXmm15);
                    emit2(MOp::kMovq, kXmm15, arg_reg_i(static_cast<int>(i)));
                } else {
                    emit2(MOp::kMovq, src_mop(in.args[i]), arg_reg_i(static_cast<int>(i)));
                }
            } else {
                // 超出寄存器个数：走栈。统一约定下按位置放在 out_base 之后。
                emit2(MOp::kMovq, src_mop(in.args[i]), kR10);
                emit2(MOp::kMovq, kR10,
                      op_mem(kRsp, out_base() + static_cast<int>(i - kMaxIntArgs()) * 8));
            }
        }
        if (type_tags_) {
            // P2：间接调用（跨模块导入的函数 / 函数值 / 闭包）的 tag 数组
            // 走 pa_ptag_in 侧信道（%r11 仍是闭包指针，ABI 逐字不变）。
            // 布局按静态函数签名算；无 tag 或签名不可推导时写 0（回退 UNK）。
            const int ntags = call_ptag_words(in);
            if (ntags > 0) {
                const int nstack = in.args.size() > static_cast<size_t>(kMaxIntArgs())
                                       ? static_cast<int>(in.args.size()) - kMaxIntArgs()
                                       : 0;
                const int base = out_base() + 8 * nstack;
                int k = 0;
                TypePtr ft = ty_of(in.a);
                const FuncInfo* fi = (ft && ft->kind == TyKind::Func) ? ft->fn.get() : nullptr;
                for (size_t i = 0; i < in.args.size(); ++i) {
                    if (!fi || i >= fi->pos.size()) continue;
                    // **每个形参一个字**：被调按「第 i 个形参 = 第 i 个字」
                    // 读，静态形参也要写（占位，被调按需忽略）。漏写会让
                    // 后面的 unknown 形参整体错位（错 tag → 错误堆性 RC）。
                    // tag_src 可能是内存操作数（槽 tag 字）→ move_to 走
                    // scratch，不能直接 emit 成 mem→mem 的 movq。
                    move_to(MOperand(std::format("{}(%rsp)", base + 8 * k)),
                            MOperand(tag_src(in.args[i])));
                    ++k;
                }
                emit2(MOp::kLeaq, op_mem(kRsp, base), kR10);
                line("movq %r10, pa_ptag_in(%rip)");
                emit_imm(MOp::kMovq, static_cast<int64_t>(in.args.size()),
                         "pa_ptag_len(%rip)");
            } else {
                line("movq $0, pa_ptag_in(%rip)");
                line("movq $0, pa_ptag_len(%rip)");
            }
        }
        line("movq %r11, %r10");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *(%r11)");
        reload_fp_slots();
        if (type_tags_) {
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        }

        if (!dst_unused(in)) {
            // 统一约定下返回值在 %rax（浮点也是位模式）。
            if (ir_is_float(in.ty)) {
                line("movq %rax, %xmm0");
                move_fp(dst_op(in.dst), "%xmm0");
            } else {
                emit2("movq", "%rax", dst_op(in.dst));
            }
            // P2：unknown 返回值的 tag 在被调的 %rdx（type_tags 构建里
            // 所有非生成器函数都发）。间接调用点没有 IRFunction 可查，
            // 用**静态签名**判断：签名不含浮点（闭包 +0 直指函数本体，
            // 不走重排 shim）且返回 unknown 才读 —— shim 的重排会碰 %rdx。
            if (type_tags_ && vreg_tagged(in.dst) && is_unk_ty(in.ty)) {
                TypePtr ft = ty_of(in.a);
                const FuncInfo* fi = (ft && ft->kind == TyKind::Func) ? ft->fn.get() : nullptr;
                bool fp = false;
                if (fi) {
                    fp = ir_is_float(fi->ret);
                    for (const auto& p : fi->pos)
                        if (ir_is_float(p.ty)) fp = true;
                }
                if (fi && !fp && is_unk_ty(fi->ret))
                    emit2("movq", "%rdx", tag_mop(in.dst));
            }
        }
    }

void AsmGen::gen_setjmp(const Inst& in) {
        // handler 记录的第一个字段就是 jmp_buf，所以记录指针可以直接
        // 作为 setjmp 的参数。longjmp 回来后 $rax 非 0。
        emit2("movq", src_op(in.a), arg_reg_i(0));
        // 用 _setjmp 而不是 setjmp：glibc 的 setjmp 每次都存信号掩码
        //（一次 rt_sigprocmask 系统调用），实测 117ns vs 2.5ns。
        // pyadamas 从不改信号掩码，没有要恢复的东西。
        line(std::format("call {}", target_abi().setjmp_sym));
        line("movslq %eax, %rax");
        emit2("movq", "%rax", dst_op(in.dst));
        // P2（type_tags）：longjmp 回到这里时清 tag 侧信道（理由同 landing pad）。
        if (type_tags_) {
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        }
    }

void AsmGen::gen_call_rt(const Inst& in) {
        auto fn = static_cast<RtFn>(in.imm);
        // ── math.sqrt 内联 fast path ──────────────────────────────
        //
        // pa_math_sqrt 的语义是 x<0 抛 ValueError，其余返回 sqrt(x)。
        // 现状每次都 call，实测 nbody 的内层循环里还要额外 3 条浮点
        // spill/reload（调用点保存），而 Go 的 math.Sqrt 直接内联成
        // 一条 SQRTSD + 冷路径 panic —— go tool objdump 确认 Go 的
        // advance 里 0 次函数调用（20 个 CALL 全是冷路径 panicBounds）。
        //
        // 这里照同样结构做：comisd 判负 -> 非负走 sqrtsd（无调用、
        // 无 spill），负数才落到原调用序列。语义完全一致。
        //
        // **必须整条路径自己发完**：gen_rt_args 只装参数，真正的 call
        // 和结果写回在这个函数下面。若只在 gen_rt_args 里特判，外层
        // 仍会再发一次 call 把结果覆盖掉 —— 实测 nbody 输出从
        // -928502048 变成 -319441880。
        if (fn == RtFn::MathSqrt && !in.args.empty() && ir_is_float(in.ty)) {
            const std::string& x = src_op(in.args[0]);
            // x 可能在内存里（栈槽），先装进 xmm 寄存器再比较。
            if (x.find('(') != std::string::npos)
                line(std::format("movsd {}, %xmm0", x));
            else
                line(std::format("movapd {}, %xmm0", x));
            line("xorpd %xmm1, %xmm1");
            line("comisd %xmm0, %xmm1");
            const std::string cold = fresh_label("sqrtneg");
            const std::string done = fresh_label("sqrtdone");
            emit_jmp("ja", cold);
            line("sqrtsd %xmm0, %xmm0");
            emit_jmp("jmp", done);
            label(cold);
            spill_fp_slots();
            line("call pa_math_sqrt");
            reload_fp_slots();
            label(done);
            move_fp(dst_op(in.dst), "%xmm0");
            return;
        }
        // ── RC 记账的内联快路径 ──────────────────────────────────
        //
        // pa_rc_inc / pa_rc_dec / pa_rc_mark / pa_rc_cleanup 原来每次都
        // 走 call。实测（perf 采样，queue_sim / str_build）：
        //
        //     pa_rc_cleanup  8.2% / 11.4%
        //     pa_rc_inc      4.0% /  3.0%
        //     pa_rc_dec      4.2% /  3.7%
        //
        // 这几个函数体本身只有 3~6 条指令，call/ret + 被调用者保存
        // 的开销比它们干的活还大。内联实现放在 codegen_call_rt.cpp 的
        // gen_rc_inline（那边是「实参装填」专门文件，本来就集中放这类
        // 逐指令发射）。返回 true 表示已发完，外层不再补 call。
        if (gen_rc_inline(in, fn)) return;
        // ── P3：门控内拦截旧 Any 算术/比较/一元负，改发 tag 版 ────────
        // tag 从平行 tag 区/静态类型装填；双字返回 {rax=payload,rdx=tag}。
        // 门控关（含 26 个热基准）不进这里，逐字旧路径。
        if (tags_enabled_ &&
            (fn == RtFn::AnyArith || fn == RtFn::AnyCmp || fn == RtFn::AnyNeg)) {
            gen_any_tagged(in, fn);
            return;
        }
        // P3：单操作数 Any 入口同样改 tag 版（免探针解码）。
        if (tags_enabled_ && in.args.size() == 1 && vreg_tagged(in.args[0]) &&
            (fn == RtFn::AnyIsFloat || fn == RtFn::AnyToF64 || fn == RtFn::AnyToI64)) {
            gen_any_tagged(in, fn);
            return;
        }
        // ── P2：类型内省入口带 tag（PYADAMAS_TYPE_TAGS）────────────────
        // repr/print_repr/isinstance 的 unknown 分支：tag 从平行 tag 区装填，
        // 运行期按类型码分派（免探针，且 None/bool/bytes/tuple/set 可区分）。
        // 门控关时 lowering 不发这些新入口、旧入口逐字旧路径。
        if (type_tags_ && (fn == RtFn::ReprAny || fn == RtFn::PrintAnyRepr) &&
            in.args.size() == 1 && vreg_tagged(in.args[0])) {
            emit_arg(0, false, 0, src_mop(in.args[0]));
            emit_arg(1, false, 1, tag_src(in.args[0]));
            spill_fp_slots();
            line(fn == RtFn::ReprAny ? "call pa_repr_any_t"
                                     : "call pa_print_any_repr_t");
            reload_fp_slots();
            if (!dst_unused(in) && fn == RtFn::ReprAny)
                emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (type_tags_ && fn == RtFn::AnyIterList && in.args.size() == 1 &&
            vreg_tagged(in.args[0])) {
            // P2：unknown 迭代物化带 tag（标量元组未进 RC 索引，探针会漏）。
            emit_arg(0, false, 0, src_mop(in.args[0]));
            emit_arg(1, false, 1, tag_src(in.args[0]));
            spill_fp_slots();
            line("call pa_any_iter_list_t");
            reload_fp_slots();
            if (!dst_unused(in)) emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (type_tags_ && fn == RtFn::AnyDictItems && in.args.size() == 2 &&
            vreg_tagged(in.args[0])) {
            // P2：unknown dict 的 (key,value) 对物化（pprint 取键值用）。
            // 门控关时 IR 的第二个实参是常量 0（UNK），走旧探针路径。
            emit_arg(0, false, 0, src_mop(in.args[0]));
            emit_arg(1, false, 1, tag_src(in.args[0]));
            spill_fp_slots();
            line("call pa_any_dict_items");
            reload_fp_slots();
            if (!dst_unused(in)) emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (type_tags_ && fn == RtFn::AnyTypeIs && in.args.size() == 2 &&
            vreg_tagged(in.args[0])) {
            emit_arg(0, false, 0, src_mop(in.args[0]));
            emit_arg(1, false, 1, tag_src(in.args[0]));
            emit_arg(2, false, 2, src_mop(in.args[1]));   // 类型码常量
            spill_fp_slots();
            line("call pa_any_type_is");
            reload_fp_slots();
            if (!dst_unused(in)) emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        // ── list.append 内联快路径（0.282）─────────────────────────
        // 无扩容 + 无位图 + 非 kind2 时就是 data[len++]=v 一条 store，
        // call/ret + 装填 + 运行时检查比活本身还贵（str_build 14.6%）。
        if (fn == RtFn::ListAppend && opt_ >= 1 && in.args.size() == 3) {
            // P3：unknown 值带 tag 时堆标志从 tag 导出（保住 kind2 位图）。
            if (tags_enabled_ && vreg_tagged(in.args[1])) {
                gen_append_tagged(in);
                return;
            }
            gen_append_inline(in);
            return;
        }
        // ── DictTryGet：一次查找两个结果（0.284）──────────────────
        // found 在 rax（0/1）落 dst（原 contains 的 bool）；值由运行时
        // 写入 idx 指定的帧槽（借用，无 RC）。槽地址 leaq 作第三实参。
        if (fn == RtFn::DictTryGet && in.args.size() == 2) {
            emit_arg(0, false, 0, src_mop(in.args[0]));
            emit_arg(1, false, 1, src_mop(in.args[1]));
            line(std::format("leaq {}, {}", smem(static_cast<int>(in.idx)), arg_reg_i(2)));
            spill_fp_slots();
            line("call pa_dict_try_get");
            reload_fp_slots();
            if (!dst_unused(in)) emit2("movq", "%rax", dst_op(in.dst));
            // P3-1b：一次探针导槽 tag（miss 槽留旧值，探针与旧 payload 一致）。
            if (slot_tagged(static_cast<int>(in.idx)))
                emit_tag_probe(smem(static_cast<int>(in.idx)),
                               slot_tag_op(static_cast<int>(in.idx)));
            return;
        }
        gen_rt_args(in, fn);
        spill_fp_slots();
        line(std::format("call {}", rt_name(fn)));
        reload_fp_slots();
        if (!dst_unused(in)) {
            // 有些运行时入口**总是返回机器字**（%rax），哪怕静态类型是 float
            // —— 它们把 double 的位模式塞进 int64 返回（多态返回值、
            // 按位取元素等）。这类不能按 double 去读 %xmm0。
            //
            //   * DictKeyAt：dict 的 key 一律机器字，float key 的位模式也
            //     从 rax 取；
            //   * dict 取值全家（DictGet/DictGetOr/DictGetOpt/DictPop/
            //     DictPopLast）：运行时签名统一 int64_t pa_dict_*(...)，
            //     float 值按位模式存在 entry.val、原样从 rax 返回。
            //     dict[str,float] 的下标读 / get / pop / set.pop 全走这里；
            //   * JsonLoads：json.loads(text, sample) 的返回值类型由 sample
            //     决定，运行时统一返回 int64 位模式（见 ir_ops.h 的
            //     `(str* text, str* desc) -> i64`）。
            //
            // JsonLoads 漏在这里曾导致 `json.loads("2.5", 0.0)` 读出垃圾
            // 浮点（-1.688e+260）：codegen 读 %xmm0，而实际值在 %rax。
            // 这个 bug 在普通构建下被寄存器残留值掩盖，ASan 改变内存布局
            // 后才稳定复现 —— 由 `PYADAMAS_TEST_ASAN=1` 的套件扫描发现。
            //
            // dict 取值漏在这里是同款 bug 的更大面积版本（0.313 修复）：
            // d.get(k, 0.0) 对存在键也返默认值（const.float 默认值经
            // %xmm0 物化，调用后 %xmm0 恰好还是它）；d[k] 读回 %xmm0
            // 残留（上次浮点物化/运行时调用的 leftovers）。非字面量 float
            // 「存储后读回损坏/累加失效」的存储本身逐位正确，坏的是读路径；
            // O0/O2 同病，只是残留值不同。DictTryGet 融合路径不受影响：
            // 值由运行时 movq 直写帧槽，load.slot 按位读回。
            bool raw_ret = fn == RtFn::DictKeyAt || fn == RtFn::JsonLoads ||
                           fn == RtFn::DictGet || fn == RtFn::DictGetOr ||
                           fn == RtFn::DictGetOpt || fn == RtFn::DictPop ||
                           fn == RtFn::DictPopLast;
            if (raw_ret) {
                // 位模式在 %rax 里。静态类型是 float 时把它搬到 %xmm0，
                // 否则直接按整数落位。
                if (ir_is_float(in.ty)) {
                    // movsd 不能从通用寄存器取源操作数：先把位模式搬到
                    // %xmm0（movq 的 GPR->XMM 形式），再按浮点落位。
                    line("movq %rax, %xmm0");
                    move_fp(dst_op(in.dst), "%xmm0");
                } else {
                    emit2("movq", "%rax", dst_op(in.dst));
                }
            } else if (ir_is_float(in.ty)) {
                move_fp(dst_op(in.dst), "%xmm0");
            } else {
                emit2("movq", "%rax", dst_op(in.dst));
            }
            // P3：unknown dst 落 tag（AnyFromF64→FBOX、StrAny→STR、其余 UNK）。
            emit_call_dst_tag(in);
        }
    }

}  // namespace pyadamas
