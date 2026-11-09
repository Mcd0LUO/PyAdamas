// codegen_call.cpp —— 调用、对象、字段与分支/返回
#include "codegen_impl.h"

namespace py23 {

void AsmGen::gen_make_list(const Inst& in) {
        // imm 是元素所有权类别（0 非堆 / 1 堆 / 2 unknown 或混合），
        // 由降级器按容器类型算好，运行时据此决定覆盖和销毁时是否 dec。
        line(std::format("movq ${}, %rdi", in.args.size()));
        line(std::format("movq ${}, %rsi", in.imm));
        spill_fp_slots();
        line("call py23_list_new");
        reload_fp_slots();
        line(std::format("movq %rax, {}", dst_op(in.dst)));
        for (VReg a : in.args) {
            line(std::format("movq {}, %rdi", dst_op(in.dst)));
            line(std::format("movq {}, %rsi", src_op(a)));
            spill_fp_slots();
            line("call py23_list_append");
            reload_fp_slots();
        }
    }

void AsmGen::gen_index_load(const Inst& in) {
        TypePtr ot = ty_of(in.a);
        if (ot && ot->kind == TyKind::Str) {
            line(std::format("movq {}, %rdi", src_op(in.a)));
            line(std::format("movq {}, %rsi", src_op(in.b)));
            spill_fp_slots();
            line("call py23_str_char_at");
            reload_fp_slots();
            line(std::format("movq %rax, {}", dst_op(in.dst)));
            return;
        }
        // list/tuple 共用 Py23List 布局：+0 len，+16 data。
        // 快路径用无符号比较一次挡掉 0..len-1 之外的值；负索引归一化
        // 也在内联路径里完成。慢路径只处理越界，运行时直接抛异常且
        // 不返回，因此不会破坏常驻 xmm 的浮点槽。
        std::string fast = fresh_label("idxfast");
        std::string neg = fresh_label("idxneg");
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line(std::format("movq {}, %rsi", src_op(in.b)));
        line("movq 0(%rdi), %rcx");
        line("cmpq %rcx, %rsi");
        line(std::format("jb {}", fast));
        line("testq %rsi, %rsi");
        line(std::format("js {}", neg));
        line("call py23_list_get");
        line("ud2");
        label(neg);
        line("addq %rcx, %rsi");
        line("cmpq %rcx, %rsi");
        line(std::format("jb {}", fast));
        line("movq %rcx, %rsi");
        line("call py23_list_get");
        line("ud2");
        label(fast);
        line("movq 16(%rdi), %rdx");
        if (ir_is_float(in.ty)) {
            line("movsd (%rdx,%rsi,8), %xmm0");
            line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
        } else {
            line("movq (%rdx,%rsi,8), %rax");
            move_to(dst_op(in.dst), "%rax");
        }
    }

void AsmGen::gen_index_unchecked_load(const Inst& in) {
        // 调用方（for / 推导式 / sum 等）已经证明 0 <= idx < len，
        // 这里只做一次 data 取址和元素装载。
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line(std::format("movq {}, %rsi", src_op(in.b)));
        line("movq 16(%rdi), %rdx");
        if (ir_is_float(in.ty)) {
            line("movsd (%rdx,%rsi,8), %xmm0");
            line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
        } else {
            line("movq (%rdx,%rsi,8), %rax");
            move_to(dst_op(in.dst), "%rax");
        }
    }

void AsmGen::gen_index_store(const Inst& in) {
        std::string fast = fresh_label("idxsf");
        std::string neg = fresh_label("idxsneg");
        std::string done = fresh_label("idxsdone");
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line(std::format("movq {}, %rsi", src_op(in.b)));
        line("movq 0(%rdi), %rcx");
        line("cmpq %rcx, %rsi");
        line(std::format("jb {}", fast));
        line("testq %rsi, %rsi");
        line(std::format("js {}", neg));
        line("xorl %edx, %edx");
        line("call py23_list_set");
        line("ud2");
        label(neg);
        line("addq %rcx, %rsi");
        line("cmpq %rcx, %rsi");
        line(std::format("jb {}", fast));
        line("movq %rcx, %rsi");
        line("xorl %edx, %edx");
        line("call py23_list_set");
        line("ud2");
        label(fast);
        line("movq 16(%rdi), %rdx");
        line("movq (%rdx,%rsi,8), %r8");
        if (is_fp(in.c)) {
            line(std::format("movsd {}, %xmm0", src_op(in.c)));
            line("movsd %xmm0, (%rdx,%rsi,8)");
        } else {
            line(std::format("movq {}, %rax", src_op(in.c)));
            line("movq %rax, (%rdx,%rsi,8)");
        }
        // 旧值的所有权按容器运行期 elem_kind 决定，不能只看静态类型：
        // 空列表推出来的类型可能会在后续赋值里变窄/变宽。
        line("cmpq $1, 24(%rdi)");
        line(std::format("jne {}", done));
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
        line(std::format("movq ${}, %rdi", 8 * (c.n_fields + 1)));
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
        line(std::format("movq {}, %rax", src_op(in.a)));
        line(std::format("movq {}(%rax), %rax", in.imm));
        move_to(dst_op(in.dst), "%rax");
    }

void AsmGen::gen_field_store(const Inst& in) {
        line(std::format("movq {}, %rax", src_op(in.a)));
        line(std::format("movq {}, %rcx", src_op(in.b)));
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

        line(std::format("movq {}, %r10", src_op(in.a)));
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        line(std::format("movq {}, %rdi", src_op(in.a)));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line("xorl %r10d, %r10d");
        line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (alloc) line(std::format("addq ${}, %rsp", alloc));

        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
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

        line(std::format("movq {}, %r10", src_op(in.a)));
        line("movq (%r10), %r11");
        line(std::format("movq {}(%r11), %r11", in.imm * 8));

        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line("xorl %r10d, %r10d");
        line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (alloc) line(std::format("addq ${}, %rsp", alloc));
        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }

void AsmGen::gen_super_vcall(const Inst& in) {
        // 运行期沿实例的动态 MRO 找 after 类之后的下一实现。查找要先做，
        // 因为它会走一次普通调用；找到的函数地址放 %r11 再装实参。
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line(std::format("movq ${}, %rsi", in.idx));
        line(std::format("movq ${}, %rdx", in.imm));
        line(std::format("movq {}, %rcx", src_op(in.b)));
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

        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        line(std::format("movq {}, %rdi", src_op(in.a)));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line("xorl %r10d, %r10d");
        line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (alloc) line(std::format("addq ${}, %rsp", alloc));

        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
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

        line(std::format("leaq {}(%rip), %r11", func_label(in.idx)));
        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", l.stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line("xorl %r10d, %r10d");
        line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (alloc) line(std::format("addq ${}, %rsp", alloc));
        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }

void AsmGen::gen_branch(const Inst& in) {
        TypePtr ty = ty_of(in.a);
        if (ir_is_float(ty)) {
            line(std::format("movsd {}, %xmm0", src_op(in.a)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd %xmm1, %xmm0");
            line(std::format("jne {}", block_label(in.target)));
            line(std::format("jp {}", block_label(in.target)));
            line(std::format("jmp {}", block_label(in.target2)));
            return;
        }
        if (ty && (ty->kind == TyKind::Str || ty->kind == TyKind::List ||
                   ty->kind == TyKind::Tuple || ty->kind == TyKind::Dict ||
                   ty->kind == TyKind::Set)) {
            line(std::format("movq {}, %rax", src_op(in.a)));
            line("cmpq $0, (%rax)");
            line(std::format("jne {}", block_label(in.target)));
            line(std::format("jmp {}", block_label(in.target2)));
            return;
        }
        if (ty && ty->kind == TyKind::Complex) {
            line(std::format("movq {}, %rax", src_op(in.a)));
            line("xorpd %xmm1, %xmm1");
            line("ucomisd 8(%rax), %xmm1");
            line(std::format("jne {}", block_label(in.target)));
            line("ucomisd 16(%rax), %xmm1");
            line(std::format("jne {}", block_label(in.target)));
            line(std::format("jmp {}", block_label(in.target2)));
            return;
        }
        std::string sa = src_op(in.a);
        // 被折叠成常量的条件（如 while True 的 1）不能进 cmpq 的立即数位置。
        if (!sa.empty() && sa[0] == '$') {
            long long v = std::strtoll(sa.c_str() + 1, nullptr, 10);
            line(std::format("jmp {}", block_label(v ? in.target : in.target2)));
            return;
        }
        line(std::format("cmpq $0, {}", sa));
        line(std::format("jne {}", block_label(in.target)));
        line(std::format("jmp {}", block_label(in.target2)));
    }

void AsmGen::gen_return(const Inst& in) {
        if (in.a == kNoReg) {
            line("xorl %eax, %eax");
        } else if (is_fp(in.a)) {
            line(std::format("movsd {}, %xmm0", src_op(in.a)));
        } else {
            line(std::format("movq {}, %rax", src_op(in.a)));
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

        // 函数值是闭包对象指针：+0 是代码地址。先取代码到 r11，
        // 参数装完后再把闭包指针放进 r10 传给被调函数当环境。
        line(std::format("movq {}, %r11", src_op(in.a)));
        line("movq (%r11), %r11");
        if (alloc) line(std::format("subq ${}, %rsp", alloc));
        for (size_t i = 0; i < in.args.size(); ++i) {
            if (locs[i].stack_off < 0) continue;
            line(std::format("movq {}, %r10", src_op(in.args[i])));
            line(std::format("movq %r10, {}(%rsp)", locs[i].stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), l.reg));
            else
                line(std::format("movq {}, {}", src_op(in.args[i]), kIntArgRegs[l.reg]));
        }
        line(std::format("movq {}, %r10", src_op(in.a)));
        line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (alloc) line(std::format("addq ${}, %rsp", alloc));

        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }

void AsmGen::gen_setjmp(const Inst& in) {
        // handler 记录的第一个字段就是 jmp_buf，所以记录指针可以直接
        // 作为 setjmp 的参数。longjmp 回来后 $rax 非 0。
        line(std::format("movq {}, %rdi", src_op(in.a)));
        line("call setjmp");
        line("movslq %eax, %rax");
        line(std::format("movq %rax, {}", dst_op(in.dst)));
    }

void AsmGen::gen_call_rt(const Inst& in) {
        auto fn = static_cast<RtFn>(in.imm);
        switch (fn) {
            case RtFn::ListNew:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
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
            case RtFn::IsInstance:
            case RtFn::IsNamedClass:
            case RtFn::IsException:
            case RtFn::StrEqUnknown:
            case RtFn::ComplexEqUnknown:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::GenSendOpt:
            case RtFn::ListSet:
            case RtFn::DictNew:
            case RtFn::DictSet:
            case RtFn::ListContains:
            case RtFn::DictGetOr:
            case RtFn::DictRemove:
            case RtFn::RangeNew:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                line(std::format("movq {}, %rdx", src_op(in.args[2])));
                break;
            case RtFn::ListSlice:
            case RtFn::StrSlice:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                line(std::format("movq {}, %rdx", src_op(in.args[2])));
                line(std::format("movq {}, %rcx", src_op(in.args[3])));
                break;
            case RtFn::DictPop:
            case RtFn::ListSliceAssign:
            case RtFn::SuperLookup:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                line(std::format("movq {}, %rdx", src_op(in.args[2])));
                line(std::format("movq {}, %rcx", src_op(in.args[3])));
                if (in.args.size() > 4)
                    line(std::format("movq {}, %r8", src_op(in.args[4])));
                break;
            case RtFn::Raise:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::StrFromI64:
            case RtFn::StrFromBool:
            case RtFn::PrintI64:
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
            case RtFn::StrToList:
            case RtFn::HandlerLeave:
            case RtFn::HandlerDiscard:
            case RtFn::ExcMatches:
            case RtFn::ExcValueArgs:
            case RtFn::ExcValueStr:
            case RtFn::ExcValueCause:
            case RtFn::RaiseValue:
            case RtFn::AbsI64:
            case RtFn::ComplexNeg:
            case RtFn::ComplexAbs:
            case RtFn::ComplexStr:
            case RtFn::ComplexTruth:
            case RtFn::ReprStr:
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
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                break;
            case RtFn::AsyncioWaitFd:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
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
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::GenNextOpt:
            case RtFn::GenSend:
            case RtFn::AsyncGenAsend:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                line(std::format("movq {}, %rsi", src_op(in.args[1])));
                break;
            case RtFn::FmtF64:
                line(std::format("movsd {}, %xmm0", src_op(in.args[0])));
                line(std::format("movq {}, %rdi", src_op(in.args[1])));
                break;
            case RtFn::ComplexNew:
                line(std::format("movsd {}, %xmm0", src_op(in.args[0])));
                line(std::format("movsd {}, %xmm1", src_op(in.args[1])));
                break;
            case RtFn::AsyncioSleep:
                line(std::format("movsd {}, %xmm0", src_op(in.args[0])));
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
                line(std::format("movsd {}, %xmm0", src_op(in.args[0])));
                break;
            case RtFn::MathPow:
            case RtFn::MathFmod:
                line(std::format("movsd {}, %xmm0", src_op(in.args[0])));
                line(std::format("movsd {}, %xmm1", src_op(in.args[1])));
                break;
            case RtFn::SysExit:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
                break;
            case RtFn::PrintStr:
                line(std::format("movq {}, %rdi", src_op(in.args[0])));
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
        }
        spill_fp_slots();
        line(std::format("call {}", rt_name(fn)));
        reload_fp_slots();
        if (in.dst != kNoReg) {
            if (ir_is_float(in.ty)) line(std::format("movsd %xmm0, {}", dst_op(in.dst)));
            else line(std::format("movq %rax, {}", dst_op(in.dst)));
        }
    }

}  // namespace py23
