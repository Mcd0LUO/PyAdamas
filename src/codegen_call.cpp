// codegen_call.cpp —— 调用、对象、字段与分支/返回
#include "codegen_impl.h"

namespace py23 {

void AsmGen::gen_make_list(const Inst& in) {
        // imm 是元素所有权类别（0 非堆 / 1 堆 / 2 unknown 或混合），
        // 由降级器按容器类型算好，运行时据此决定覆盖和销毁时是否 dec。
        emit_imm_op("movq", in.args.size(), "%rdi");
        emit_imm_op("movq", in.imm, "%rsi");
        spill_fp_slots();
        line("call py23_list_new");
        reload_fp_slots();
        emit2("movq", "%rax", dst_op(in.dst));
        for (VReg a : in.args) {
            emit2("movq", dst_op(in.dst), "%rdi");
            emit2("movq", src_op(a), "%rsi");
            spill_fp_slots();
            line("call py23_list_append");
            reload_fp_slots();
        }
    }

void AsmGen::gen_index_load(const Inst& in) {
        TypePtr ot = ty_of(in.a);
        if (ot && ot->kind == TyKind::Str) {
            emit2("movq", src_op(in.a), "%rdi");
            emit2("movq", src_op(in.b), "%rsi");
            spill_fp_slots();
            line("call py23_str_char_at");
            reload_fp_slots();
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        if (opt_ < 1) {
            emit2("movq", src_op(in.a), "%rdi");
            emit2("movq", src_op(in.b), "%rsi");
            line("call py23_list_get");
            emit2("movq", "%rax", dst_op(in.dst));
            return;
        }
        // list/tuple 共用 Py23List 布局：+0 len，+16 data。
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
        line("call py23_list_get");
        line("ud2");
        label(neg);
        line("addq %rcx, %rsi");
        line("cmpq %rcx, %rsi");
        emit_jmp("jb", fast);
        line("movq %rcx, %rsi");
        line("call py23_list_get");
        line("ud2");
        label(fast);
        line("movq 16(%rdi), %rdx");
        if (ir_is_float(in.ty)) {
            line("movsd (%rdx,%rsi,8), %xmm0");
            emit2("movsd", "%xmm0", dst_op(in.dst));
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
            emit2("movsd", "%xmm0", dst_op(in.dst));
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
            line("call py23_list_set");
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
        line("call py23_list_set");
        line("ud2");
        label(neg);
        line("addq %rcx, %rsi");
        line("cmpq %rcx, %rsi");
        emit_jmp("jb", fast);
        line("movq %rcx, %rsi");
        line("xorl %edx, %edx");
        line("call py23_list_set");
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
        line("cmpq $1, 24(%rdi)");
        emit_jmp("jne", done);
        line("movq %r8, %rdi");
        spill_fp_slots();
        line("call py23_rc_dec");
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
        line("call py23_obj_new");
        reload_fp_slots();
        line(std::format("leaq .Lvt{}(%rip), %rcx", in.imm));
        line("movq %rcx, (%rax)");
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_field_load(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        line(std::format("movq {}(%rax), %rax", in.imm));
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_field_store(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), "%rcx");
        line(std::format("movq %rcx, {}(%rax)", in.imm));
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

        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int stack_bytes = stack_args * 8;
        int alloc = (stack_bytes + 15) & ~15;

        emit2("movq", src_op(in.a), "%r10");
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        if (alloc) emit_imm_op("subq", alloc, "%rsp");
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
        if (alloc) emit_imm_op("addq", alloc, "%rsp");

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) emit2("movsd", "%xmm0", dst_op(in.dst));
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

void AsmGen::gen_vcall_static(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);
        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int alloc = (stack_args * 8 + 15) & ~15;

        emit2("movq", src_op(in.a), "%r10");
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        if (alloc) emit_imm_op("subq", alloc, "%rsp");
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
        if (alloc) emit_imm_op("addq", alloc, "%rsp");
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) emit2("movsd", "%xmm0", dst_op(in.dst));
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

void AsmGen::gen_super_vcall(const Inst& in) {
        // 运行期沿实例的动态 MRO 找 after 类之后的下一实现。查找要先做，
        // 因为它会走一次普通调用；找到的函数地址放 %r11 再装实参。
        emit2("movq", src_op(in.a), "%rdi");
        emit_imm_op("movq", in.idx, "%rsi");
        emit_imm_op("movq", in.imm, "%rdx");
        emit2("movq", src_op(in.b), "%rcx");
        spill_fp_slots();
        line("call py23_super_lookup");
        reload_fp_slots();
        line("movq %rax, %r11");

        std::vector<TypePtr> types;
        types.reserve(in.args.size() + 1);
        types.push_back(t_unknown());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);
        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int alloc = (stack_args * 8 + 15) & ~15;

        if (alloc) emit_imm_op("subq", alloc, "%rsp");
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
        if (alloc) emit_imm_op("addq", alloc, "%rsp");

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) emit2("movsd", "%xmm0", dst_op(in.dst));
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

void AsmGen::gen_call_direct(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);
        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int alloc = (stack_args * 8 + 15) & ~15;

        if (alloc) emit_imm_op("subq", alloc, "%rsp");
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
        if (alloc) emit_imm_op("addq", alloc, "%rsp");
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) emit2("movsd", "%xmm0", dst_op(in.dst));
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

        int stack_args = 0;
        for (auto& l : locs)
            if (l.stack_off >= 0) stack_args = (l.stack_off / 8) + 1;
        int stack_bytes = stack_args * 8;
        int alloc = (stack_bytes + 15) & ~15;

        // 函数值是闭包对象指针：+0 是代码地址。整个参数装配期间 %r11
        // 一直握着闭包，最后既当环境指针传给被调函数，也从 +0 取地址调用。
        emit2("movq", src_op(in.a), "%r11");
        if (alloc) emit_imm_op("subq", alloc, "%rsp");
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
        if (alloc) emit_imm_op("addq", alloc, "%rsp");

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) emit2("movsd", "%xmm0", dst_op(in.dst));
            else emit2("movq", "%rax", dst_op(in.dst));
        }
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
        switch (fn) {
            case RtFn::ListNew:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::ListAppend:
            case RtFn::ListGet:
            case RtFn::ListSetLen:
            case RtFn::StrConcat:
            case RtFn::StrCharAt:
            case RtFn::ClosureNew:
            case RtFn::DictGet:
            case RtFn::DictContains:
            case RtFn::DictKeyAt:
            case RtFn::StrContains:
            case RtFn::DictUpdate:
            case RtFn::ListExtend:
            case RtFn::ListDelIndex:
            case RtFn::ListSumI64:
            case RtFn::IsInstance:
            case RtFn::ObjClassId:
            case RtFn::IsNamedClass:
            case RtFn::IsException:
            case RtFn::StrEqUnknown:
            case RtFn::ComplexEqUnknown:
            case RtFn::StrToI64:
            case RtFn::IntToBase:
            case RtFn::RoundI64:
            case RtFn::FileOpen:
            case RtFn::JsonDumps:
            case RtFn::JsonLoads:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                emit2("movq", src_op(in.args[4]), "%r8");
                emit2("movq", src_op(in.args[5]), "%r9");
                break;
            case RtFn::FileSeek:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                break;
            case RtFn::ListSort:
            case RtFn::SortedList:
            case RtFn::Zip2List:
            case RtFn::MathGcd:
            case RtFn::MathLcm:
            case RtFn::MathComb:
            case RtFn::MathPerm:
            case RtFn::MathSumprod:
            case RtFn::MathDist:
            case RtFn::HashValue:
            case RtFn::QueuePut:
            case RtFn::QueuePutNowait:
            case RtFn::TaskGroupAdd:
            case RtFn::FileRead:
            case RtFn::FileWrite:
            case RtFn::FileWritelines:
            case RtFn::FileReadLine:
            case RtFn::FileReadLines:
            case RtFn::FileTruncate:
            case RtFn::CollMostCommon:
            case RtFn::BytesAt:
            case RtFn::RegexGroupStr:
            case RtFn::RegexExpand:
            case RtFn::RegexFindIter:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::RegexSearch:
            case RtFn::RegexFindAllStr:
            case RtFn::RegexFindIterStr:
            case RtFn::RegexSplit:
            case RtFn::GenSendOpt:
            case RtFn::ListSet:
            case RtFn::DictSet:
            case RtFn::ListContains:
            case RtFn::ListEq:
            case RtFn::DictEq:
            case RtFn::ListSortedBy:
            case RtFn::DictGetOr:
            case RtFn::DictRemove:
            case RtFn::RangeNew:
            case RtFn::ListIndex:
            case RtFn::ListCount:
            case RtFn::ListInsert:
            case RtFn::RegexCompile:
            case RtFn::RegexFindAll:
            case RtFn::RegexGroup:
            case RtFn::RegexStart:
            case RtFn::RegexEnd:
            case RtFn::RegexSpan:
            case RtFn::FileConfig:
            case RtFn::IterChain:
            case RtFn::IterRepeat:
            case RtFn::CollCounter:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                break;
            case RtFn::FuncReduce:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                break;
            case RtFn::FuncReduceF:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movsd", src_op(in.args[2]), "%xmm0");
                emit2("movq", src_op(in.args[3]), "%rdx");
                break;
            case RtFn::RegexSubStr:
            case RtFn::DictNew:
            case RtFn::SetFromList:
                // (kind/own 三个 + 类 key 的 hash/eq 槽号)
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                emit2("movq", src_op(in.args[4]), "%r8");
                break;
            case RtFn::RegexSearchStr:
            case RtFn::RegexSub:
            case RtFn::RegexSubFn:
            case RtFn::RegexSplitStr:
            case RtFn::ListSlice:
            case RtFn::StrSlice:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                break;
            case RtFn::DictPop:
            case RtFn::ListSliceAssign:
            case RtFn::SuperLookup:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                if (in.args.size() > 4)
                    emit2("movq", src_op(in.args[4]), "%r8");
                break;
            case RtFn::StrMethod:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                emit2("movq", src_op(in.args[4]), "%r8");
                break;
            case RtFn::Raise:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::RegexGroups:
            case RtFn::RegexGroupDict:
            case RtFn::RegexLastIndex:
            case RtFn::RegexLastGroup:
            case RtFn::RegexPatternGroups:
            case RtFn::RegexPatternFlags:
            case RtFn::RegexPatternStr:
            case RtFn::RegexEscape:
            case RtFn::StrFromI64:
            case RtFn::StrFromBool:
            case RtFn::PrintI64:
            case RtFn::PrintAny:
            case RtFn::PrintBool:
            case RtFn::Panic:
            case RtFn::RcInc:
            case RtFn::RcDec:
            case RtFn::RcTemp:
            case RtFn::RcCleanup:
            case RtFn::RcIncUnknown:
            case RtFn::DictKeys:
            case RtFn::DictValues:
            case RtFn::DictItems:
            case RtFn::DictPopItem:
            case RtFn::DictPopLast:
            case RtFn::DictClear:
            case RtFn::DictCopy:
            case RtFn::IsSeq:
            case RtFn::IsDict:
            case RtFn::IsStr:
            case RtFn::IsComplex:
            case RtFn::ListMinI64:
            case RtFn::ListMaxI64:
            case RtFn::StrToList:
            case RtFn::ListCopy:
            case RtFn::ListReversed:
            case RtFn::EnumerateList:
            case RtFn::ListReverse:
            case RtFn::IntBitLength:
            case RtFn::FileClose:
            case RtFn::FileFlush:
            case RtFn::FileTell:
            case RtFn::FileFileno:
            case RtFn::FileIsatty:
            case RtFn::FileReadable:
            case RtFn::FileWritable:
            case RtFn::FileSeekable:
            case RtFn::FileClosed:
            case RtFn::FileName:
            case RtFn::FileMode:
            case RtFn::FileEncoding:
            case RtFn::FileErrors:
            case RtFn::Input:
            case RtFn::SysStream:
            case RtFn::HandlerLeave:
            case RtFn::HandlerDiscard:
            case RtFn::ExcMatches:
            case RtFn::ExcValueArgs:
            case RtFn::ExcValueStr:
            case RtFn::ExcValueCause:
            case RtFn::RaiseValue:
            case RtFn::AbsI64:
            case RtFn::StrToF64:
            case RtFn::Ord:
            case RtFn::Chr:
            case RtFn::ComplexNeg:
            case RtFn::ComplexAbs:
            case RtFn::ComplexStr:
            case RtFn::ComplexTruth:
            case RtFn::ReprStr:
            case RtFn::MathFactorial:
            case RtFn::MathFsum:
            case RtFn::MathIsqrt:
            case RtFn::MathProd:
            case RtFn::EventSet:
            case RtFn::EventIsSet:
            case RtFn::EventWait:
            case RtFn::LockAcquire:
            case RtFn::LockRelease:
            case RtFn::LockLocked:
            case RtFn::QueueGet:
            case RtFn::QueueQsize:
            case RtFn::QueueEmpty:
            case RtFn::QueueFull:
            case RtFn::QueueJoin:
            case RtFn::QueueTaskDone:
            case RtFn::QueueNew:
            case RtFn::QueueGetNowait:
            case RtFn::TimeoutEnter:
            case RtFn::TimeoutExit:
            case RtFn::TimeoutExpired:
            case RtFn::TaskGroupExit:
            case RtFn::TaskGroupCancel:
            case RtFn::SemAcquire:
            case RtFn::SemRelease:
            case RtFn::SemLocked:
            case RtFn::SemNew:
            case RtFn::GenNext:
            case RtFn::GenClose:
            case RtFn::HandlerSuspend:
            case RtFn::HandlerRearm:
            case RtFn::HandlerFree:
            case RtFn::AsyncioRun:
            case RtFn::AsyncioGather:
            case RtFn::AsyncioCreateTask:
            case RtFn::AsyncioTaskDone:
            case RtFn::AsyncioTaskCancel:
            case RtFn::AsyncGenANext:
            case RtFn::AsyncGenAClose:
            case RtFn::FutureDone:
            case RtFn::FutureCancel:
            case RtFn::FutureResult:
            case RtFn::FutureException:
            case RtFn::FutureCancelled:
            case RtFn::AsyncioTaskResult:
            case RtFn::AsyncioTaskException:
            case RtFn::AsyncioTaskCancelled:
            case RtFn::RangeLen:
            case RtFn::RangeFirst:
            case RtFn::RangeStep:
            case RtFn::RangeStop:
            case RtFn::RangeMinI64:
            case RtFn::RangeMaxI64:
            case RtFn::RangeAny:
            case RtFn::RangeAll:
            case RtFn::RangeToList:
            case RtFn::RangeRepr:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::AsyncioWaitFd:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::ComplexAdd:
            case RtFn::ComplexSub:
            case RtFn::ComplexMul:
            case RtFn::ComplexDiv:
            case RtFn::ComplexEq:
            case RtFn::ExcValueNew:
            case RtFn::ExcRegister:
            case RtFn::RaiseValueFrom:
            case RtFn::FutureSetResult:
            case RtFn::FutureSetException:
            case RtFn::FutureSetExceptionValue:
            case RtFn::FutureAddCallback:
            case RtFn::FmtI64:
            case RtFn::FmtBool:
            case RtFn::FmtStr:
            case RtFn::FmtComplex:
            case RtFn::AsyncGenAthrow:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::GenNextOpt:
            case RtFn::GenSend:
            case RtFn::AsyncGenAsend:
            case RtFn::SetEq:
            case RtFn::DictGetOpt:
            case RtFn::RangeGet:
            case RtFn::ReprTuple:
            case RtFn::RangeContains:
            case RtFn::RangeEq:
            case RtFn::RangeSumI64:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::FmtF64:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movq", src_op(in.args[1]), "%rdi");
                break;
            case RtFn::RoundF64:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movq", src_op(in.args[1]), "%rdi");
                break;
            case RtFn::ListSumF64:
            case RtFn::RangeSumF64:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movsd", src_op(in.args[1]), "%xmm0");
                break;
            case RtFn::ComplexNew:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movsd", src_op(in.args[1]), "%xmm1");
                break;
            case RtFn::AsyncioSleep:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                break;
            case RtFn::StrFromF64:
            case RtFn::PrintF64:
            case RtFn::MathSqrt:
            case RtFn::MathFloor:
            case RtFn::MathCeil:
            case RtFn::MathFabs:
            case RtFn::MathSin:
            case RtFn::MathCos:
            case RtFn::MathTan:
            case RtFn::MathLog:
            case RtFn::MathLog2:
            case RtFn::MathLog10:
            case RtFn::MathExp:
            case RtFn::MathDegrees:
            case RtFn::MathRadians:
            case RtFn::MathTrunc:
            case RtFn::MathIsnan:
            case RtFn::MathIsinf:
            case RtFn::MathIsfinite:
            case RtFn::MathAcos:
            case RtFn::MathAsin:
            case RtFn::MathAtan:
            case RtFn::MathAcosh:
            case RtFn::MathAsinh:
            case RtFn::MathAtanh:
            case RtFn::MathCbrt:
            case RtFn::MathCosh:
            case RtFn::MathSinh:
            case RtFn::MathTanh:
            case RtFn::MathExp2:
            case RtFn::MathExpm1:
            case RtFn::MathLog1p:
            case RtFn::MathErf:
            case RtFn::MathErfc:
            case RtFn::MathGamma:
            case RtFn::MathLgamma:
            case RtFn::MathUlp:
            case RtFn::MathFrexp:
            case RtFn::MathModf:
            case RtFn::HashF64:
            case RtFn::TimeSleep:
            case RtFn::AsyncTimeoutNew:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                break;
            case RtFn::MathLdexp:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movq", src_op(in.args[1]), "%rdi");
                break;
            case RtFn::AsyncioWaitFor:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movsd", src_op(in.args[1]), "%xmm0");
                break;
            case RtFn::MathFma:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movsd", src_op(in.args[1]), "%xmm1");
                emit2("movsd", src_op(in.args[2]), "%xmm2");
                break;
            case RtFn::MathPow:
            case RtFn::MathFmod:
            case RtFn::MathAtan2:
            case RtFn::MathHypot:
            case RtFn::MathCopysign:
            case RtFn::MathNextafter:
            case RtFn::MathRemainder:
            case RtFn::MathIsclose:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movsd", src_op(in.args[1]), "%xmm1");
                break;
            case RtFn::SysExit:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::PrintStr:
            case RtFn::PrintBytes:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::StrFromNone:
            case RtFn::PanicDiv0:
            case RtFn::PanicOverflow:
            case RtFn::PrintNone:
            case RtFn::PrintSep:
            case RtFn::PrintEnd:
            case RtFn::RcMark:
            case RtFn::RcShutdown:
            case RtFn::GcShutdown:
            case RtFn::HandlerPush:
            case RtFn::AsyncioFutureNew:
            case RtFn::EventNew:
            case RtFn::LockNew:
            case RtFn::TaskGroupNew:
            case RtFn::ExcAccept:
            case RtFn::ExcAcceptValue:
            case RtFn::ExcDone:
            case RtFn::ExcClear:
            case RtFn::ExcReraise:
            case RtFn::ExcTypeName:
            case RtFn::ExcMessage:
            case RtFn::Reraise:
            case RtFn::SysArgv:
                break;
            case RtFn::RandNew:
            case RtFn::RandGSeed:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::RandSeed:
            case RtFn::RandChoice:
            case RtFn::RandShuffle:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::RandRandint:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                break;
            case RtFn::RandNext:
            case RtFn::OsMkdir:
            case RtFn::OsRmdir:
            case RtFn::OsRemove:
            case RtFn::OsPathJoin:
            case RtFn::OsPathExists:
            case RtFn::OsPathIsdir:
            case RtFn::OsPathIsfile:
            case RtFn::OsListdir:
            case RtFn::OsPathBasename:
            case RtFn::OsPathDirname:
            case RtFn::OsPathSplitext:
            case RtFn::OsPathNormpath:
            case RtFn::OsPathAbspath:
            case RtFn::OsPathExpanduser:
            case RtFn::SubprocessReturnCode:
            case RtFn::SubprocessStdout:
            case RtFn::SubprocessStderr:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::RandUniform:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movsd", src_op(in.args[1]), "%xmm0");
                emit2("movsd", src_op(in.args[2]), "%xmm1");
                break;
            case RtFn::RandGNext:
            case RtFn::OsGetcwd:
            case RtFn::OsCpuCount:
            case RtFn::TimeTime:
            case RtFn::TimeMonotonic:
            case RtFn::TimePerfCounter:
                break;
            case RtFn::RandGRandint:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::RandGChoice:
            case RtFn::RandGShuffle:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::RandGUniform:
                emit2("movsd", src_op(in.args[0]), "%xmm0");
                emit2("movsd", src_op(in.args[1]), "%xmm1");
                break;
            case RtFn::OsRename:
            case RtFn::OsGetenv:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::DateNew:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                break;
            case RtFn::DateTimeNew:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movq", src_op(in.args[3]), "%rcx");
                emit2("movq", src_op(in.args[4]), "%r8");
                emit2("movq", src_op(in.args[5]), "%r9");
                break;
            case RtFn::DateTimeField:
            case RtFn::DateTimeStrftime:
            case RtFn::PathJoin:
            case RtFn::PathWriteText:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                break;
            case RtFn::DateTimeNow:
            case RtFn::DateTimeIsoformat:
            case RtFn::PathNew:
            case RtFn::PathStr:
            case RtFn::PathName:
            case RtFn::PathStem:
            case RtFn::PathSuffix:
            case RtFn::PathParent:
            case RtFn::PathExists:
            case RtFn::PathIsFile:
            case RtFn::PathIsDir:
            case RtFn::PathReadText:
                emit2("movq", src_op(in.args[0]), "%rdi");
                break;
            case RtFn::SubprocessRun:
                emit2("movq", src_op(in.args[0]), "%rdi");
                emit2("movq", src_op(in.args[1]), "%rsi");
                emit2("movq", src_op(in.args[2]), "%rdx");
                emit2("movsd", src_op(in.args[3]), "%xmm0");
                emit2("movq", src_op(in.args[4]), "%rcx");
                break;
        }
        spill_fp_slots();
        line(std::format("call {}", rt_name(fn)));
        reload_fp_slots();
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) emit2("movsd", "%xmm0", dst_op(in.dst));
            else emit2("movq", "%rax", dst_op(in.dst));
        }
    }

}  // namespace py23
