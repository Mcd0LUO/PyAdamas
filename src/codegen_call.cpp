// codegen_call.cpp —— 调用、对象、字段与分支/返回
#include "codegen_impl.h"

namespace pyadamas {

void AsmGen::gen_make_list(const Inst& in) {
        // imm 是元素所有权类别（0 非堆 / 1 堆 / 2 unknown 或混合），
        // 由降级器按容器类型算好，运行时据此决定覆盖和销毁时是否 dec。
        emit_imm_op("movq", in.args.size(), "%rdi");
        emit_imm_op("movq", in.imm, "%rsi");
        spill_fp_slots();
        line("call pa_list_new");
        reload_fp_slots();
        emit2("movq", "%rax", dst_op(in.dst));
        for (VReg a : in.args) {
            emit2("movq", dst_op(in.dst), "%rdi");
            emit2("movq", src_op(a), "%rsi");
            spill_fp_slots();
            line("call pa_list_append");
            reload_fp_slots();
        }
    }

void AsmGen::gen_index_load(const Inst& in) {
        TypePtr ot = ty_of(in.a);
        if (ot && ot->kind == TyKind::Str) {
            emit2("movq", src_op(in.a), "%rdi");
            emit2("movq", src_op(in.b), "%rsi");
            spill_fp_slots();
            line("call pa_str_char_at");
            reload_fp_slots();
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (opt_ < 1) {
            emit2("movq", src_op(in.a), "%rdi");
            emit2("movq", src_op(in.b), "%rsi");
            line("call pa_list_get");
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        // list/tuple 共用 PyAdamasList 布局：+0 len，+16 data。
        // 快路径用无符号比较一次挡掉 0..len-1 之外的值；负索引归一化
        // 也在内联路径里完成。慢路径只处理越界，运行时直接抛异常且
        // 不返回，因此不会破坏常驻 xmm 的浮点槽。
        std::string fast = fresh_label("idxfast");
        std::string neg = fresh_label("idxneg");
        emit2("movq", src_op(in.a), "%rdi");
        emit2("movq", src_op(in.b), "%rsi");
        line("movq 0(%rdi), %rcx");
        line("cmpq %rcx, %rsi");
        emit_jmp("jb", fast);
        line("testq %rsi, %rsi");
        emit_jmp("js", neg);
        line("call pa_list_get");
        line("ud2");
        label(neg);
        line("addq %rcx, %rsi");
        line("cmpq %rcx, %rsi");
        emit_jmp("jb", fast);
        line("movq %rcx, %rsi");
        line("call pa_list_get");
        line("ud2");
        label(fast);
        line("movq 16(%rdi), %rdx");
        if (ir_is_float(in.ty)) {
            line("movsd (%rdx,%rsi,8), %xmm0");
            move_fp(dst_op(in.dst), "%xmm0");
        } else {
            line("movq (%rdx,%rsi,8), %rax");
            move_to(dst_op(in.dst), "%rax");
        }
    }

void AsmGen::gen_index_unchecked_load(const Inst& in) {
        // 调用方（for / 推导式 / sum 等）已经证明 0 <= idx < len，
        // 这里只做一次 data 取址和元素装载。
        emit2("movq", src_op(in.a), "%rdi");
        emit2("movq", src_op(in.b), "%rsi");
        line("movq 16(%rdi), %rdx");
        if (ir_is_float(in.ty)) {
            line("movsd (%rdx,%rsi,8), %xmm0");
            move_fp(dst_op(in.dst), "%xmm0");
        } else {
            line("movq (%rdx,%rsi,8), %rax");
            move_to(dst_op(in.dst), "%rax");
        }
    }

void AsmGen::gen_index_store(const Inst& in) {
        if (opt_ < 1) {
            emit2("movq", src_op(in.a), "%rdi");
            emit2("movq", src_op(in.b), "%rsi");
            emit2("movq", src_op(in.c), "%rdx");
            line("call pa_list_set");
            return;
        }
        std::string fast = fresh_label("idxsf");
        std::string neg = fresh_label("idxsneg");
        std::string done = fresh_label("idxsdone");
        emit2("movq", src_op(in.a), "%rdi");
        emit2("movq", src_op(in.b), "%rsi");
        line("movq 0(%rdi), %rcx");
        line("cmpq %rcx, %rsi");
        emit_jmp("jb", fast);
        line("testq %rsi, %rsi");
        emit_jmp("js", neg);
        line("xorl %edx, %edx");
        line("call pa_list_set");
        line("ud2");
        label(neg);
        line("addq %rcx, %rsi");
        line("cmpq %rcx, %rsi");
        emit_jmp("jb", fast);
        line("movq %rcx, %rsi");
        line("xorl %edx, %edx");
        line("call pa_list_set");
        line("ud2");
        label(fast);
        line("movq 16(%rdi), %rdx");
        line("movq (%rdx,%rsi,8), %r8");
        if (is_fp(in.c)) {
            emit2("movsd", src_op(in.c), "%xmm0");
            line("movsd %xmm0, (%rdx,%rsi,8)");
        } else {
            emit2("movq", src_op(in.c), "%rax");
            line("movq %rax, (%rdx,%rsi,8)");
        }
        // 旧值的所有权按容器运行期 elem_kind 决定，不能只看静态类型：
        // 空列表推出来的类型可能会在后续赋值里变窄/变宽。
        // kind 1 直接 dec；kind 2 可能装着整数，走注册表判断。
        std::string decheap = fresh_label("idxsdec");
        line("movq 24(%rdi), %rax");
        line("cmpq $1, %rax");
        emit_jmp("je", decheap);
        line("cmpq $2, %rax");
        emit_jmp("jne", done);
        line("movq %r8, %rdi");
        spill_fp_slots();
        line("call pa_rc_dec_unknown");
        reload_fp_slots();
        emit_jmp("jmp", done);
        label(decheap);
        line("movq %r8, %rdi");
        spill_fp_slots();
        line("call pa_rc_dec");
        reload_fp_slots();
        label(done);
    }

// ── 对象 ────────────────────────────────────────────────────

    // 对象布局：+0 是 vtable 指针，字段从 +8 开始，每个 8 字节。
    // 属性访问因此是一条固定偏移的 movq，没有查表、没有哈希。
void AsmGen::gen_obj_new(const Inst& in) {
        const IRClass& c = m_->classes[static_cast<size_t>(in.imm)];
        emit_imm_op("movq", 8 * (c.n_fields + 1), "%rdi");
        bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
        if (has_heap) {
            line(std::format("leaq .Ldtor{}(%rip), %rsi", in.imm));
            line(std::format("leaq .Lgcrel{}(%rip), %rdx", in.imm));
        } else {
            line("xorl %esi, %esi");
            line("xorl %edx, %edx");
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
    }

void AsmGen::gen_field_store(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), "%rcx");
        if (in.fwidth == 1)
            line(std::format("movb %cl, {}(%rax)", in.imm));
        else if (in.fwidth == 4)
            line(std::format("movl %ecx, {}(%rax)", in.imm));
        else
            line(std::format("movq %rcx, {}(%rax)", in.imm));
    }

// 值类型：payload 前有隐藏的 RC 头，返回的指针就是 payload 地址。
void AsmGen::gen_struct_new(const Inst& in) {
        emit_imm_op("movq", in.imm, "%rdi");
        spill_fp_slots();
        line("call pa_struct_new");
        reload_fp_slots();
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_struct_copy(const Inst& in) {
        emit2("movq", src_op(in.a), "%rdi");
        emit_imm_op("movq", in.imm, "%rsi");
        spill_fp_slots();
        line("call pa_struct_copy");
        reload_fp_slots();
        move_to(dst_op(in.dst), "%rax");
    }

// 虚调用：self 固定占第一个整数参数（%rdi），其余实参照常分类。
    // 函数地址在装参数之前取到 %r11 —— %r11 不在参数序列里，
    // 装参数的过程不会碰它。
void AsmGen::gen_vcall(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size() + 1);
        types.push_back(t_unknown());  // self：指针，非浮点
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);


        emit2("movq", src_op(in.a), "%r10");
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off < 0) continue;
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        emit2("movq", src_op(in.a), "%rdi");
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), kIntArgRegs[l.reg]);
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
                emit2("movq", src_op(in.args[i]), kIntArgRegs[l.reg]);
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
        emit2("movq", src_op(in.a), "%rdi");
        emit2("movq", src_op(in.c), "%rsi");
        emit_imm_op("movq", in.imm, "%rdx");
        emit2("movq", src_op(in.b), "%rcx");
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
        emit2("movq", src_op(in.a), "%rdi");
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), kIntArgRegs[l.reg]);
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
                emit2("movq", src_op(in.args[i]), kIntArgRegs[l.reg]);
        }
        // 不读闭包环境的 callee 不需要 %r10；有 env 槽的才清零占位。
        const IRFunction* callee =
            (in.idx >= 0 && in.idx < static_cast<int>(m_->funcs.size()))
                ? m_->funcs[static_cast<size_t>(in.idx)].get()
                : nullptr;
        if (!callee || callee->env_slot >= 0) line("xorl %r10d, %r10d");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line(std::format("call {}", func_label(in.idx)));
        reload_fp_slots();
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
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
                emit2("movq", src_op(in.args[i]), kIntArgRegs[l.reg]);
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
        emit_epilogue();
    }

// ── 调用 ────────────────────────────────────────────────────
void AsmGen::gen_call(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);


        // 函数值是闭包对象指针：+0 是代码地址。整个参数装配期间 %r11
        // 一直握着闭包，最后既当环境指针传给被调函数，也从 +0 取地址调用。
        emit2("movq", src_op(in.a), "%r11");
        for (size_t i = 0; i < in.args.size(); ++i) {
            if (locs[i].stack_off < 0) continue;
            emit2("movq", src_op(in.args[i]), "%r10");
            line(std::format("movq %r10, {}(%rsp)", locs[i].stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                emit2("movq", src_op(in.args[i]), kIntArgRegs[l.reg]);
        }
        line("movq %r11, %r10");
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *(%r11)");
        reload_fp_slots();

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
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
                bool fp = kind_at(i) == CKind::Float;
                p.fp = fp;
                if (fp && fi < kMaxFpArgs) {
                    p.reg = fi++;
                } else if (!fp && gi < kMaxIntArgs) {
                    p.reg = gi++;
                } else {
                    p.stack_off = sp++ * 8;
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
            if (p.size > 16 || gi + ni > kMaxIntArgs || fi + nf > kMaxFpArgs) {
                p.stack_off = sp * 8;
                sp += static_cast<int>(p.cfp.size());
                continue;
            }
            p.creg.assign(p.cfp.size(), -1);
            for (size_t c = 0; c < p.cfp.size(); ++c) p.creg[c] = p.cfp[c] ? fi++ : gi++;
        }

        int stack_bytes = sp * 8;
        int int_tmp0 = stack_bytes + 8, int_tmp1 = stack_bytes + 16;
        int fp_tmp0 = stack_bytes + 24, fp_tmp1 = stack_bytes + 32;
        int sret_tmp = stack_bytes + 40;

        // 取实参的机器值。str/可选 str 要先从 PyAdamasStr 里取出 data 字段
        // （偏移 16）；Buf* 从 list/bytes 的 data 取指针；普通指针按整数直接传。
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
                line("movq 16(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::BufF64 || k == CKind::BufI64 || k == CKind::BufBytes) {
                // list/bytes 的 data 都在 +16（PyAdamasList / PyAdamasStr）。
                emit2("movq", src, "%r10");
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 16(%r10), %r10");
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
            emit_imm_op("movq", ex.ret_abi.size, "%rdi");
            spill_fp_slots();
            line("call pa_struct_new");
            reload_fp_slots();
            emit2("movq", "%rax", std::format("{}(%rsp)", sret_tmp));
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
                    emit2("movq", "%r10", kIntArgRegs[p.creg[c]]);
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
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), p.reg));
            } else {
                arg_into(i, kIntArgRegs[p.reg]);
            }
        }
        if (hidden) emit2("movq", std::format("{}(%rsp)", sret_tmp), "%rdi");
        // 可变参数函数要求 %al = 使用的向量寄存器数；非可变参数沿用
        // 原有的清空做法。
        if (ex.varargs) line(std::format("movl ${}, %eax", fi));
        else if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call " + ex.symbol);
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
                    emit_imm_op("movq", ra.size, "%rdi");
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
                        emit2("movq", "%rax", "%rdi");
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
            line("movsd (%rax), %xmm0");
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
            line("movsd %xmm0, (%rax)");
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
        emit2("movq", src_op(in.a), "%rdi");
        line("call setjmp");
        line("movslq %eax, %rax");
        emit2("movq", "%rax", dst_op(in.dst));
    }

void AsmGen::gen_call_rt(const Inst& in) {
        auto fn = static_cast<RtFn>(in.imm);
        gen_rt_args(in, fn);
        spill_fp_slots();
        line(std::format("call {}", rt_name(fn)));
        reload_fp_slots();
        if (!dst_unused(in)) {
            // pa_dict_key_at 一律返回机器字；float 元素的位模式也从 rax
            // 取，不能按 double 去读 xmm0。
            bool raw_ret = fn == RtFn::DictKeyAt;
            if (ir_is_float(in.ty) && !raw_ret) move_fp(dst_op(in.dst), "%xmm0");
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

}  // namespace pyadamas
