// codegen_call.cpp —— 调用、对象、字段与分支/返回
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
                emit_imm_op("movq", static_cast<int64_t>(in.args.size()), arg_reg_i(0));
                emit_imm_op("movq", in.imm, arg_reg_i(1));
                for (size_t ai = 0; ai < 4; ++ai) {
                    if (ai < in.args.size()) emit2("movq", src_op(in.args[ai]), arg_reg_i(2 + ai));
                    else emit_imm_op("movq", 0, arg_reg_i(2 + ai));
                }
                spill_fp_slots();
                // 全堆元素的定长列表仍走运行时（tracked，需要 rc_link）。
                line("call pa_list_new_filled");
                reload_fp_slots();
                emit2("movq", "%rax", dst_op(in.dst));
                return;
            }
        }
        // imm 是元素所有权类别（0 非堆 / 1 堆 / 2 unknown 或混合），
        // 由降级器按容器类型算好，运行时据此决定覆盖和销毁时是否 dec。
        emit_imm_op("movq", in.args.size(), arg_reg_i(0));
        emit_imm_op("movq", in.imm, arg_reg_i(1));
        spill_fp_slots();
        line("call pa_list_new");
        reload_fp_slots();
        emit2("movq", "%rax", dst_op(in.dst));
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
            emit2("movq", dst_op(in.dst), arg_reg_i(0));
            emit2("movq", src_op(a), arg_reg_i(1));
            // P3：unknown 元素带 tag 时堆标志从 tag 导出（保住位图）。
            if (tags_enabled_ && hk == 2 && vreg_tagged(a))
                emit_heap_from_tag(a, arg_reg_i(2));
            else
                emit_imm_op("movq", hk, arg_reg_i(2));
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
            emit2("movq", src_op(in.a), arg_reg_i(0));
            emit2("movq", src_op(in.b), arg_reg_i(1));
            spill_fp_slots();
            line("call pa_str_char_at");
            reload_fp_slots();
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (opt_ < 1) {
            emit2("movq", src_op(in.a), arg_reg_i(0));
            emit2("movq", src_op(in.b), arg_reg_i(1));
            line("call pa_list_get");
            emit2("movq", "%rax", dst_op(in.dst));
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
        const std::string r0 = arg_reg_i(0), r1 = arg_reg_i(1);
        const std::string sl = target_abi().scratch_len;
        const std::string sp = target_abi().scratch_ptr;
        std::string fast = fresh_label("idxfast");
        std::string done = fresh_label("idxdone");
        emit2("movq", src_op(in.a), r0);
        emit2("movq", src_op(in.b), r1);
        line(std::format("movq 0({}), {}", r0, sl));
        line(std::format("cmpq {}, {}", sl, r1));
        emit_jmp("jb", fast);
        // 冷路径：负索引 / 越界 / 类型不确定，全交给运行时。
        line("call pa_list_get");
        if (ir_is_float(in.ty)) {
            line("movq %rax, %xmm0");
            move_fp(dst_op(in.dst), "%xmm0");
        } else {
            move_to(dst_op(in.dst), "%rax");
        }
        gen_index_load_tag(in, false);
        emit_jmp("jmp", done);
        label(fast);
        line(std::format("movq 16({}), {}", r0, sp));
        if (ir_is_float(in.ty)) {
            line(std::format("movsd ({},{},8), %xmm0", sp, r1));
            move_fp(dst_op(in.dst), "%xmm0");
        } else {
            line(std::format("movq ({},{},8), %rax", sp, r1));
            move_to(dst_op(in.dst), "%rax");
        }
        gen_index_load_tag(in, true);
        label(done);
    }

void AsmGen::gen_index_unchecked_load(const Inst& in) {
        // 调用方（for / 推导式 / sum 等）已经证明 0 <= idx < len，
        // 这里只做一次 data 取址和元素装载。
        const std::string r0 = arg_reg_i(0), r1 = arg_reg_i(1);
        const std::string sp = target_abi().scratch_ptr;
        emit2("movq", src_op(in.a), r0);
        emit2("movq", src_op(in.b), r1);
        line(std::format("movq 16({}), {}", r0, sp));
        if (ir_is_float(in.ty)) {
            line(std::format("movsd ({},{},8), %xmm0", sp, r1));
            move_fp(dst_op(in.dst), "%xmm0");
        } else {
            line(std::format("movq ({},{},8), %rax", sp, r1));
            move_to(dst_op(in.dst), "%rax");
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
        emit2("movq", src_op(in.a), "%rax");
        if (in.fwidth == 1)
            line(std::format("movzbq {}(%rax), %rax", in.imm));
        else if (in.fwidth == 4 && in.funsigned)
            line(std::format("movl {}(%rax), %eax", in.imm));
        else if (in.fwidth == 4)
            line(std::format("movslq {}(%rax), %rax", in.imm));
        else
            line(std::format("movq {}(%rax), %rax", in.imm));
        move_to(dst_op(in.dst), "%rax");
        // P3-1b：unknown 字段装载一次探针导 tag（窄字段是 C 结构体 typed）。
        if (vreg_tagged(in.dst)) emit_tag_probe(dst_op(in.dst), tag_mem(in.dst));
    }

void AsmGen::gen_field_store(const Inst& in) {
        // 基址固定用 %rax；值另借一个 ABI 给定的 scratch（SysV 是 %rcx，
        // Win64 是 %r10 —— %rcx 在 Win64 是第 1 个实参，不能当暂存）。
        const TargetABI& abi = target_abi();
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), abi.scratch_field);
        const std::string v = abi.scratch_field;
        if (in.fwidth == 1)
            line(std::format("movb {}, {}(%rax)", reg8(v), in.imm));
        else if (in.fwidth == 4)
            line(std::format("movl {}, {}(%rax)", reg32(v), in.imm));
        else
            line(std::format("movq {}, {}(%rax)", v, in.imm));
    }

// 值类型：payload 前有隐藏的 RC 头，返回的指针就是 payload 地址。
void AsmGen::gen_struct_new(const Inst& in) {
        emit_imm_op("movq", in.imm, arg_reg_i(0));
        spill_fp_slots();
        line("call pa_struct_new");
        reload_fp_slots();
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_struct_copy(const Inst& in) {
        emit2("movq", src_op(in.a), arg_reg_i(0));
        emit_imm_op("movq", in.imm, arg_reg_i(1));
        spill_fp_slots();
        line("call pa_struct_copy");
        reload_fp_slots();
        move_to(dst_op(in.dst), "%rax");
    }

// 虚调用：self 只装入 ABI 的第一个整数实参寄存器一次，随后从中读
// vtable。槽地址直接作为间接 call 的内存操作数，不另取函数指针。
void AsmGen::gen_vcall(const Inst& in, bool keep_int_result) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size() + 1);
        types.push_back(t_unknown());  // self：指针，非浮点
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off < 0) continue;
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        move_to(arg_reg_i(0), src_op(in.a));
        line(std::format("movq ({}), %r11", arg_reg_i(0)));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line(std::format("call *{}(%r11)", in.imm * 8));
        reload_fp_slots();

        if (!keep_int_result && !dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

void AsmGen::gen_vcall_static(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        emit2("movq", src_op(in.a), "%r10");
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

void AsmGen::gen_super_vcall(const Inst& in) {
        // 运行期沿实例的动态 MRO 找 after 类之后的下一实现。查找要先做，
        // 因为它会走一次普通调用；找到的函数地址放 %r11 再装实参。
        emit2("movq", src_op(in.a), arg_reg_i(0));
        emit2("movq", src_op(in.c), arg_reg_i(1));
        emit_imm_op("movq", in.imm, arg_reg_i(2));
        emit2("movq", src_op(in.b), arg_reg_i(3));
        spill_fp_slots();
        line("call pa_super_lookup");
        reload_fp_slots();
        line("movq %rax, %r11");

        std::vector<TypePtr> types;
        types.reserve(in.args.size() + 1);
        types.push_back(t_unknown());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off < 0) continue;
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        emit2("movq", src_op(in.a), arg_reg_i(0));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
        }
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
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        // P3-1b：unknown 形参的平行 tag 数组（栈实参之后、出参区内）。
        int ptag_base = -1;
        if (ptag_words > 0) ptag_base = emit_ptag_array(in, locs);
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), arg_reg_i(l.reg));
        }
        // 不读闭包环境的 callee 不需要 %r10；有 env 槽的才清零占位。
        const IRFunction* callee =
            (in.idx >= 0 && in.idx < static_cast<int>(m_->funcs.size()))
                ? m_->funcs[static_cast<size_t>(in.idx)].get()
                : nullptr;
        if (!callee || callee->env_slot >= 0) line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        if (ptag_words > 0) {
            // tag 数组指针最后装填（此前所有指令都不碰 %r11），call tag 入口。
            line(std::format("leaq {}(%rsp), %r11", ptag_base));
            line(std::format("call {}", ptag_label(in.idx)));
        } else {
            line(std::format("call {}", func_label(in.idx)));
        }
        reload_fp_slots();
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
            // P3-1b：unknown 返回的 tag 在 %rdx（被调 gen_return 发射；
            // 旧入口调用也发，读到的一定是有效 tag 或 UNK）。
            if (vreg_tagged(in.dst) && is_unk_ty(in.ty) && callee &&
                func_ret_tag(*callee))
                emit2("movq", "%rdx", tag_mem(in.dst));
        }
    }

void AsmGen::gen_call_ptr(const Inst& in) {
        // 裸代码地址调用（类对象 vtable 分派）：没有闭包环境，%r10 清零。
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        emit2("movq", src_op(in.a), "%r11");
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

void AsmGen::gen_branch(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ir_is_float(ty)) {
            emit2("movsd", src_op(in.a), "%xmm0");
            line("xorpd %xmm1, %xmm1");
            line("ucomisd %xmm1, %xmm0");
            emit_jmp("jne", block_label(in.target));
            emit_jmp("jp", block_label(in.target));
            emit_jmp("jmp", block_label(in.target2));
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            emit2("movq", src_op(in.a), "%rax");
            line("cmpq $0, (%rax)");
            emit_jmp("jne", block_label(in.target));
            emit_jmp("jmp", block_label(in.target2));
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            emit2("movq", src_op(in.a), "%rax");
            line("xorpd %xmm1, %xmm1");
            line("ucomisd 8(%rax), %xmm1");
            emit_jmp("jne", block_label(in.target));
            line("ucomisd 16(%rax), %xmm1");
            emit_jmp("jne", block_label(in.target));
            emit_jmp("jmp", block_label(in.target2));
            return;
        }
        std::string sa = src_op(in.a);
        // 被折叠成常量的条件（如 while True 的 1）不能进 cmpq 的立即数位置。
        if (!sa.empty() && sa[0] == '$') {
            long long v = std::strtoll(sa.c_str() + 1, nullptr, 10);
            int tgt = v ? in.target : in.target2;
            if (!(opt_ >= 2 && tgt == cur_block_ + 1))
                emit_jmp("jmp", block_label(tgt));
            return;
        }
        emit2("cmpq", "$0", sa);
        if (opt_ >= 2 && in.target2 == cur_block_ + 1) {
            emit_jmp("jne", block_label(in.target));
        } else if (opt_ >= 2 && in.target == cur_block_ + 1) {
            emit_jmp("je", block_label(in.target2));
        } else {
            emit_jmp("jne", block_label(in.target));
            emit_jmp("jmp", block_label(in.target2));
        }
    }

void AsmGen::gen_return(const Inst& in) {
        if (in.a == kNoReg) {
            line("xorl %eax, %eax");
        } else if (is_fp(in.a)) {
            emit2("movsd", src_op(in.a), "%xmm0");
        } else {
            emit2("movq", src_op(in.a), "%rax");
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
        emit2("movq", src_op(in.a), "%r11");
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& ty = ty_of(in.args[i]);
            if (i < static_cast<size_t>(kMaxIntArgs())) {
                if (ir_is_float(ty)) {
                    // 浮点值在 %xmm 里，先搬到位模式再进整型寄存器。
                    line(std::format("movq {}, %xmm15", src_op(in.args[i])));
                    emit2("movq", "%xmm15", arg_reg_i(static_cast<int>(i)));
                } else {
                    emit2("movq", src_op(in.args[i]), arg_reg_i(static_cast<int>(i)));
                }
            } else {
                // 超出寄存器个数：走栈。统一约定下按位置放在 out_base 之后。
                emit2("movq", src_op(in.args[i]), "%r10");
                line(std::format("movq %r10, {}(%rsp)",
                                 out_base() + static_cast<int>(i - kMaxIntArgs()) * 8));
            }
        }
        line("movq %r11, %r10");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *(%r11)");
        reload_fp_slots();

        if (!dst_unused(in)) {
            // 统一约定下返回值在 %rax（浮点也是位模式）。
            if (ir_is_float(in.ty)) {
                line("movq %rax, %xmm0");
                move_fp(dst_op(in.dst), "%xmm0");
            } else {
                emit2("movq", "%rax", dst_op(in.dst));
            }
        }
    }

void AsmGen::gen_call_extern(const Inst& in) {
        const IRExtern& ex = m_->externs[static_cast<size_t>(in.idx)];

        // 同一符号的不同调用点可能有不同编组（ptr 接地址或 list/bytes），
        // 优先用调用点记录的 cargs。
        auto kind_at = [&](size_t i) -> CKind {
            if (i < in.cargs.size()) return static_cast<CKind>(in.cargs[i]);
            if (i < ex.params.size()) return ex.params[i];
            return CKind::Int;  // 可变参数多出来的实参
        };
        // 按值结构体的 SysV 分类；非结构体参数返回 nullptr。
        auto abi_at = [&](size_t i) -> const IRStructABI* {
            if (kind_at(i) != CKind::Struct || i >= ex.param_abi.size()) return nullptr;
            const IRStructABI& a = ex.param_abi[i];
            return a.size > 0 ? &a : nullptr;
        };

        // 一个实参的落点：标量占一个寄存器或 8 字节栈槽；结构体按 chunk
        // 分散到整数/xmm 寄存器，放不下时整块走栈。
        struct Place {
            bool is_struct = false;
            bool fp = false;
            int reg = -1;
            int stack_off = -1;
            int64_t size = 0;
            std::vector<uint8_t> cfp;
            std::vector<int> creg;
        };

        // 返回 >16 字节结构体时 %rdi 是隐藏的结果指针，整数参数整体后移。
        const bool hidden = ex.ret == CKind::Struct && ex.ret_abi.size > 16;
        // 可变参数调用可能比声明的 params 多，落点表按实参个数分配。
        size_t nargs = in.args.size() > ex.params.size() ? in.args.size() : ex.params.size();
        std::vector<Place> ps(nargs);
        int gi = hidden ? 1 : 0, fi = 0, sp = 0;
        for (size_t i = 0; i < nargs; ++i) {
            Place& p = ps[i];
            const IRStructABI* abi = i < ex.params.size() ? abi_at(i) : nullptr;
            if (!abi) {
                CKind kk = kind_at(i);
                bool fp = kk == CKind::Float || kk == CKind::Float32;
                p.fp = fp;
                if (fp && fi < kMaxFpArgs()) {
                    p.reg = fi++;
                } else if (!fp && gi < kMaxIntArgs()) {
                    p.reg = gi++;
                } else {
                    p.stack_off = out_base() + sp++ * 8;
                }
                continue;
            }
            p.is_struct = true;
            p.size = abi->size;
            p.cfp = abi->chunk_fp;
            int ni = 0, nf = 0;
            for (uint8_t c : p.cfp) {
                if (c) ++nf;
                else ++ni;
            }
            if (p.size > 16 || gi + ni > kMaxIntArgs() || fi + nf > kMaxFpArgs()) {
                p.stack_off = out_base() + sp * 8;
                sp += static_cast<int>(p.cfp.size());
                continue;
            }
            p.creg.assign(p.cfp.size(), -1);
            for (size_t c = 0; c < p.cfp.size(); ++c) p.creg[c] = p.cfp[c] ? fi++ : gi++;
        }

        int stack_bytes = sp * 8;
        int int_tmp0 = out_base() + stack_bytes + 8, int_tmp1 = out_base() + stack_bytes + 16;
        int fp_tmp0 = out_base() + stack_bytes + 24, fp_tmp1 = out_base() + stack_bytes + 32;
        int sret_tmp = out_base() + stack_bytes + 40;
        int fn_tmp = out_base() + stack_bytes + 48;

        // 取实参的机器值。str/可选 str 要先从 PyAdamasStr 里取出 data 字段
        // （偏移 24：pub/len/hash 之后的第四个机器字）；Buf* 从 list/bytes 的
        // data 取指针；普通指针按整数直接传。
        auto arg_into = [&](size_t i, const std::string& reg) {
            CKind k = kind_at(i);
            const std::string src = src_op(in.args[i]);
            if (k == CKind::Str || k == CKind::OptStr) {
                emit2("movq", src, "%r10");
                // 可空/可能为 NULL 的字符串：NULL 原样传，不能解引用。
                // 非可空 str 在类型检查阶段已拒绝 Optional[str]，这里只是
                // 防御运行期出现的空值。
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 24(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::BufF64 || k == CKind::BufI64) {
                // list 的 data 在 +16（PyAdamasList）。
                emit2("movq", src, "%r10");
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 16(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::BufBytes) {
                // bytes 的 data 在 +24（PyAdamasStr 的第四个机器字）。
                emit2("movq", src, "%r10");
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 24(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::Callback) {
                // 先把闭包登记到签名槽，再把跳板地址作为 C 函数指针传入。
                int sig = (i < ex.param_cb.size()) ? ex.param_cb[i] : -1;
                emit2("movq", src, "%r10");
                if (sig >= 0) {
                    line(std::format("movq %r10, .Lcbslot{}(%rip)", sig));
                    line(std::format("leaq .Lcbtramp{}(%rip), %r10", sig));
                }
                emit2("movq", "%r10", reg);
            } else if (k == CKind::Float32) {
                // 栈上的 C float 实参：double 收窄成单精度，位模式进 8 字节槽。
                line(std::format("cvtsd2ss {}, %xmm0", src));
                emit2("movq", "%xmm0", reg);
            } else {
                emit2("movq", src, reg);
            }
        };

        // 结构体 chunk 搬运：不足 8 字节的尾部按实际宽度读写，避免碰
        // payload 之外的内存；读出的高字节零扩展即可（C 不要求 padding 值）。
        auto load_chunk = [&](const std::string& base, int64_t off, int64_t bytes) {
            if (bytes >= 8) line(std::format("movq {}({}), %r10", off, base));
            else if (bytes >= 4) line(std::format("movl {}({}), %r10d", off, base));
            else if (bytes >= 2) line(std::format("movzwl {}({}), %r10d", off, base));
            else line(std::format("movzbl {}({}), %r10d", off, base));
        };
        auto store_chunk = [&](const std::string& base, int64_t off, int64_t bytes) {
            if (bytes >= 8) line(std::format("movq %r10, {}({})", off, base));
            else if (bytes >= 4) line(std::format("movl %r10d, {}({})", off, base));
            else if (bytes >= 2) line(std::format("movw %r10w, {}({})", off, base));
            else line(std::format("movb %r10b, {}({})", off, base));
        };


        // 隐藏返回指针要在装配实参之前分配好对象（调用 pa_struct_new 会
        // 破坏参数寄存器），指针暂存在栈上，最后再装进 %rdi。
        if (hidden) {
            emit_imm_op("movq", ex.ret_abi.size, arg_reg_i(0));
            spill_fp_slots();
            line("call pa_struct_new");
            reload_fp_slots();
            emit2("movq", "%rax", std::format("{}(%rsp)", sret_tmp));
        }

        // 免写直调：先把 C 函数地址解析到 fn_tmp，再装配实参（解析调用
        // 会破坏参数寄存器）。lazy==1 走进程默认符号表并缓存到静态槽；
        // lazy==2 每次按运行期句柄 dlsym（句柄在 in.a）。
        if (ex.lazy) {
            spill_fp_slots();
            if (ex.lazy == 1) {
                line(std::format("movq .Lcsym{}(%rip), %r11", ex.sym_name));
                line("testq %r11, %r11");
                std::string hit = fresh_label("csymhit");
                emit_jmp("jne", hit);
                line(std::format("leaq .Lcsymstr{}(%rip), {}", ex.sym_name, arg_reg_i(0)));
                line("call pa_c_sym_default");
                line("movq %rax, %r11");
                line(std::format("movq %r11, .Lcsym{}(%rip)", ex.sym_name));
                label(hit);
            } else {
                emit2("movq", src_op(in.a), arg_reg_i(0));
                line(std::format("leaq .Lcsymstr{}(%rip), {}", ex.sym_name, arg_reg_i(1)));
                line("call pa_c_sym_handle");
                line("movq %rax, %r11");
            }
            line(std::format("movq %r11, {}(%rsp)", fn_tmp));
            reload_fp_slots();
        }

        // slot_off >= 0 表示整块走栈，否则按 chunk 分散到寄存器。
        auto struct_arg = [&](size_t i, const Place& p, int slot_off) {
            emit2("movq", src_op(in.args[i]), "%r11");
            for (size_t c = 0; c < p.cfp.size(); ++c) {
                int64_t off = static_cast<int64_t>(c) * 8;
                int64_t bytes = p.size - off < 8 ? p.size - off : 8;
                if (slot_off >= 0) {
                    if (p.cfp[c]) {
                        line(std::format("movsd {}(%r11), %xmm0", off));
                        line(std::format("movsd %xmm0, {}(%rsp)", slot_off + off));
                    } else {
                        load_chunk("%r11", off, bytes);
                        line(std::format("movq %r10, {}(%rsp)", slot_off + off));
                    }
                } else if (p.cfp[c]) {
                    line(std::format("movsd {}(%r11), %xmm{}", off, p.creg[c]));
                } else {
                    load_chunk("%r11", off, bytes);
                    emit2("movq", "%r10", arg_reg_i(p.creg[c]));
                }
            }
        };

        for (size_t i = 0; i < in.args.size(); ++i) {
            if (ps[i].stack_off < 0) continue;
            if (ps[i].is_struct) {
                struct_arg(i, ps[i], ps[i].stack_off);
            } else {
                arg_into(i, "%r10");
                line(std::format("movq %r10, {}(%rsp)", ps[i].stack_off));
            }
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& p = ps[i];
            if (p.stack_off >= 0) continue;
            if (p.is_struct) {
                struct_arg(i, p, -1);
            } else if (p.fp) {
                if (kind_at(i) == CKind::Float32)
                    line(std::format("cvtsd2ss {}, %xmm{}", src_op(in.args[i]), p.reg));
                else
                    line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), p.reg));
            } else {
                arg_into(i, arg_reg_i(p.reg));
            }
        }
        if (hidden) emit2("movq", std::format("{}(%rsp)", sret_tmp), arg_reg_i(0));
        // 可变参数函数要求 %al = 使用的向量寄存器数；非可变参数沿用
        // 原有的清空做法。
        // %al 是「用到的向量寄存器数」，ABI 上限 8：溢出的浮点实参走栈，
        // 不能把 fi 原样塞进去（>8 会让 va_arg 的寄存器保存区读越界）。
        if (ex.varargs) line(std::format("movl ${}, %eax", fi > 8 ? 8 : fi));
        else if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        if (ex.lazy) {
            line(std::format("movq {}(%rsp), %r11", fn_tmp));
            line("call *%r11");
        } else {
            line("call " + ex.symbol);
        }
        reload_fp_slots();

        if (!dst_unused(in)) {
            std::string dst = dst_op(in.dst);
            if (ex.ret == CKind::Struct && ex.ret_abi.size > 0) {
                const IRStructABI& ra = ex.ret_abi;
                if (ra.size > 16) {
                    emit2("movq", std::format("{}(%rsp)", sret_tmp), "%rax");
                    move_to(dst, "%rax");
                } else {
                    // SysV：INTEGER chunk 依次在 rax/rdx，SSE chunk 在
                    // xmm0/xmm1。先暂存返回值，再分配对象并写回 payload。
                    int ni = 0, nf = 0;
                    std::vector<int> ic(ra.chunk_fp.size(), -1), fc(ra.chunk_fp.size(), -1);
                    for (size_t c = 0; c < ra.chunk_fp.size(); ++c) {
                        if (ra.chunk_fp[c]) fc[c] = nf++;
                        else ic[c] = ni++;
                    }
                    if (ni > 0) emit2("movq", "%rax", std::format("{}(%rsp)", int_tmp0));
                    if (ni > 1) emit2("movq", "%rdx", std::format("{}(%rsp)", int_tmp1));
                    if (nf > 0) line(std::format("movsd %xmm0, {}(%rsp)", fp_tmp0));
                    if (nf > 1) line(std::format("movsd %xmm1, {}(%rsp)", fp_tmp1));
                    emit_imm_op("movq", ra.size, arg_reg_i(0));
                    spill_fp_slots();
                    line("call pa_struct_new");
                    reload_fp_slots();
                    emit2("movq", "%rax", "%r11");
                    for (size_t c = 0; c < ra.chunk_fp.size(); ++c) {
                        int64_t off = static_cast<int64_t>(c) * 8;
                        int64_t bytes = ra.size - off < 8 ? ra.size - off : 8;
                        if (ra.chunk_fp[c]) {
                            int t = fc[c] == 0 ? fp_tmp0 : fp_tmp1;
                            line(std::format("movsd {}(%rsp), %xmm0", t));
                            line(std::format("movsd %xmm0, {}(%r11)", off));
                        } else {
                            int t = ic[c] == 0 ? int_tmp0 : int_tmp1;
                            line(std::format("movq {}(%rsp), %r10", t));
                            store_chunk("%r11", off, bytes);
                        }
                    }
                    move_to(dst, "%r11");
                }
            } else {
                switch (ex.ret) {
                    case CKind::None:
                        break;
                    case CKind::Float:
                        emit2("movsd", "%xmm0", dst);
                        break;
                    case CKind::Float32:
                        line("cvtss2sd %xmm0, %xmm0");
                        emit2("movsd", "%xmm0", dst);
                        break;
                    case CKind::Int32:
                        line("movslq %eax, %rax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::UInt32:
                        line("movl %eax, %eax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Bool:
                        line("testq %rax, %rax");
                        line("setne %al");
                        line("movzbl %al, %eax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Str:
                    case CKind::OptStr: {
                        // C 返回的 char* 不能直接交给 PyAdamas：需要复制成
                        // PyAdamasStr。NULL 对 str 视为空串，对 Optional[str]
                        // 保持 None。
                        spill_fp_slots();
                        emit2("movq", "%rax", arg_reg_i(0));
                        line(ex.ret == CKind::OptStr ? "call pa_str_from_cstr_opt"
                                                     : "call pa_str_from_cstr");
                        reload_fp_slots();
                        emit2("movq", "%rax", dst);
                        break;
                    }
                    case CKind::Struct:
                    case CKind::Callback:
                        // 理论上不会作为返回值出现；按指针透传。
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Int:
                    case CKind::Ptr:
                    case CKind::BufF64:
                    case CKind::BufI64:
                    case CKind::BufBytes:
                        emit2("movq", "%rax", dst);
                        break;
                }
            }
        }
    }

// C 全局变量：按声明宽度从符号地址加载。可执行文件里的数据符号用
// RIP 相对直接寻址（PIE 下由 copy relocation 处理）。
void AsmGen::gen_load_extern(const Inst& in) {
        const IRExternVar& v = m_->extern_vars[static_cast<size_t>(in.idx)];
        const std::string dst = dst_op(in.dst);
        const std::string sym = v.symbol + "(%rip)";
        switch (v.kind) {
            case CKind::Float:
                line("movsd " + sym + ", %xmm0");
                emit2("movsd", "%xmm0", dst);
                break;
            case CKind::Float32:
                line("movss " + sym + ", %xmm0");
                line("cvtss2sd %xmm0, %xmm0");
                emit2("movsd", "%xmm0", dst);
                break;
            case CKind::Int32:
                line("movslq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
            case CKind::UInt32:
                line("movl " + sym + ", %eax");
                move_to(dst, "%rax");
                break;
            case CKind::Bool:
                line("movzbl " + sym + ", %eax");
                move_to(dst, "%rax");
                break;
            case CKind::Str:
            case CKind::OptStr:
                // 全局 char* 同样要复制成 PyAdamasStr。
                line("movq " + sym + ", %rdi");
                spill_fp_slots();
                line(v.kind == CKind::OptStr ? "call pa_str_from_cstr_opt"
                                             : "call pa_str_from_cstr");
                reload_fp_slots();
                move_to(dst, "%rax");
                break;
            default:
                line("movq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
        }
    }

void AsmGen::gen_store_extern(const Inst& in) {
        const IRExternVar& v = m_->extern_vars[static_cast<size_t>(in.idx)];
        const std::string sym = v.symbol + "(%rip)";
        switch (v.kind) {
            case CKind::Float:
                emit2("movsd", src_op(in.a), "%xmm0");
                line("movsd %xmm0, " + sym);
                break;
            case CKind::Float32:
                emit2("movsd", src_op(in.a), "%xmm0");
                line("cvtsd2ss %xmm0, %xmm0");
                line("movss %xmm0, " + sym);
                break;
            case CKind::Int32:
            case CKind::UInt32:
                emit2("movq", src_op(in.a), "%rax");
                line("movl %eax, " + sym);
                break;
            case CKind::Bool:
                emit2("movq", src_op(in.a), "%rax");
                line("movb %al, " + sym);
                break;
            default:
                emit2("movq", src_op(in.a), "%rax");
                line("movq %rax, " + sym);
                break;
        }
    }

// C 指针解引用：地址 = a + b * 元素宽度。宽度是 1/2/4/8 时直接作为
// leaq 的比例因子。
void AsmGen::gen_ptr_load(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), "%r11");
        line(std::format("leaq (%rax,%r11,{}), %rax", in.imm));
        const std::string dst = dst_op(in.dst);
        if (ir_is_float(in.ty)) {
            if (in.fwidth == 4) {
                // ptr[float32]：单精度装载后提升成 double。
                line("movss (%rax), %xmm0");
                line("cvtss2sd %xmm0, %xmm0");
            } else {
                line("movsd (%rax), %xmm0");
            }
            emit2("movsd", "%xmm0", dst);
            return;
        }
        if (in.fwidth == 4) {
            if (in.funsigned) line("movl (%rax), %eax");
            else line("movslq (%rax), %rax");
        } else if (in.fwidth == 1) {
            line("movzbl (%rax), %eax");
        } else if (in.fwidth == 2) {
            line("movzwl (%rax), %eax");
        } else {
            line("movq (%rax), %rax");
        }
        move_to(dst, "%rax");
    }

void AsmGen::gen_ptr_store(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), "%r11");
        line(std::format("leaq (%rax,%r11,{}), %rax", in.imm));
        if (ir_is_float(in.ty)) {
            emit2("movsd", src_op(in.c), "%xmm0");
            if (in.fwidth == 4) {
                line("cvtsd2ss %xmm0, %xmm0");
                line("movss %xmm0, (%rax)");
            } else {
                line("movsd %xmm0, (%rax)");
            }
            return;
        }
        emit2("movq", src_op(in.c), "%r10");
        if (in.fwidth == 4) line("movl %r10d, (%rax)");
        else if (in.fwidth == 1) line("movb %r10b, (%rax)");
        else if (in.fwidth == 2) line("movw %r10w, (%rax)");
        else line("movq %r10, (%rax)");
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
            emit_arg(0, false, 0, src_op(in.args[0]));
            emit_arg(1, false, 1, src_op(in.args[1]));
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
