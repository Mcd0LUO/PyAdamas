// codegen_call_abi.cpp —— 间接/虚调用发射 + 出参区估算
//
// 从 codegen_call.cpp / codegen_block.cpp 拆出（文件规模硬限制 1200 行）。
// 本文件只放 AsmGen 的调用 ABI 发射：vtable 静态/动态分派、super() 查找后
// 的间接调用、裸代码地址调用，以及按「每个实参至多 8 字节 + 隐藏指针/暂存」
// 估算的出参区大小（outgoing_bytes）。P2（PYADAMAS_TYPE_TAGS）的 tag 侧信道
// 清零/装填就发生在这些调用点上。
#include "codegen_impl.h"

namespace pyadamas {

// 出参区：调用点把装不下的实参写在 rsp 下面那块地方。以前每个调用点
// 自己 subq/addq，栈指针一动，槽位地址（相对 rsp）就跟着漂；现在整函数
// 按最大用量一次性留在帧里，rsp 全程不动。这里只按「每个实参至多一个
// 八字节、结构体按 ABI 尺寸、再加隐藏指针/暂存」估上界，宁大勿小。
int AsmGen::outgoing_bytes(const IRFunction& f) const {
    int worst = 16 + target_abi().shadow_space;  // 至少留两个槽：sret 隐藏指针 + 对齐；再加影子空间
    for (const auto& b : f.blocks) {
        for (const auto& in : b.insts) {
            int need = 0;
            switch (in.op) {
                case IrOp::Call:
                    // 寄存器用完的实参逐个占 8 字节；+16 兜住隐藏指针和对齐。
                    // P2：间接调用在栈实参之后还要放 tag 数组（门控关恒 0）。
                    need = 8 * (static_cast<int>(in.args.size()) + 2) +
                           8 * call_ptag_words(in);
                    break;
                case IrOp::CallPtr:
                case IrOp::CallRt:
                case IrOp::VCall:
                case IrOp::VCallStatic:
                case IrOp::SuperVCall:
                    // 寄存器用完的实参逐个占 8 字节；+16 兜住隐藏指针和对齐。
                    need = 8 * (static_cast<int>(in.args.size()) + 2);
                    break;
                case IrOp::CallDirect:
                    // P3-1b：tag ABI 调用还要在栈实参之后放 unknown 形参
                    // 的平行 tag 数组（门控关恒 0，逐字旧布局）。
                    need = 8 * (static_cast<int>(in.args.size()) + 2) +
                           8 * call_ptag_words(in);
                    break;
                case IrOp::CallExtern: {
                    // C 调用还多一块暂存：返回值分块暂存 + sret 指针 +
                    // 免写直调的函数地址暂存，共 56 字节。
                    const IRExtern& ex = m_->externs[static_cast<size_t>(in.idx)];
                    need = 56 + 16;
                    for (size_t i = 0; i < in.args.size(); ++i) {
                        int64_t sz = 8;
                        if (i < ex.param_abi.size() && ex.param_abi[i].size > sz)
                            sz = ex.param_abi[i].size;
                        need += static_cast<int>((sz + 7) & ~static_cast<int64_t>(7));
                    }
                    break;
                }
                default:
                    continue;
            }
            if (need > worst) worst = need;
        }
    }
    return (worst + 15) & ~15;
}


void AsmGen::gen_vcall_static(const Inst& in) {
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        emit2(MOp::kMovq, src_mop(in.a), kR10);
        emit2(MOp::kMovq, op_mem(kR10), kR11);
        emit2(MOp::kMovq, op_mem(kR11, in.imm * 8), kR11);

        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            emit2(MOp::kMovq, src_mop(in.args[i]), kR10);
            emit2(MOp::kMovq, kR10, op_mem(kRsp, l.stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                emit2(MOp::kMovsd, src_mop(in.args[i]), static_cast<MReg>(kXmm0 + l.reg));
            else
                emit2(MOp::kMovq, src_mop(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (tag_.type_tags) {
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        }
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();
        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
            else emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        }
    }

void AsmGen::gen_super_vcall(const Inst& in) {
        // 运行期沿实例的动态 MRO 找 after 类之后的下一实现。查找要先做，
        // 因为它会走一次普通调用；找到的函数地址放 %r11 再装实参。
        emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
        emit2(MOp::kMovq, src_mop(in.c), arg_reg_i(1));
        emit_imm(MOp::kMovq, in.imm, arg_reg_i(2));
        emit2(MOp::kMovq, src_mop(in.b), arg_reg_i(3));
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
            emit2(MOp::kMovq, src_mop(in.args[i]), kR10);
            emit2(MOp::kMovq, kR10, op_mem(kRsp, l.stack_off));
        }
        emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i + 1];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                emit2(MOp::kMovsd, src_mop(in.args[i]), static_cast<MReg>(kXmm0 + l.reg));
            else
                emit2(MOp::kMovq, src_mop(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (tag_.type_tags) {
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        }
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
            else emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        }
    }


void AsmGen::gen_call_ptr(const Inst& in) {
        // 裸代码地址调用（类对象 vtable 分派）：没有闭包环境，%r10 清零。
        std::vector<TypePtr> types;
        types.reserve(in.args.size());
        for (VReg a : in.args) types.push_back(ty_of(a));
        auto locs = classify(types);

        emit2(MOp::kMovq, src_mop(in.a), kR11);
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off < 0) continue;
            emit2(MOp::kMovq, src_mop(in.args[i]), kR10);
            emit2(MOp::kMovq, kR10, op_mem(kRsp, l.stack_off));
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& l = locs[i];
            if (l.stack_off >= 0) continue;
            if (l.fp)
                emit2(MOp::kMovsd, src_mop(in.args[i]), static_cast<MReg>(kXmm0 + l.reg));
            else
                emit2(MOp::kMovq, src_mop(in.args[i]), arg_reg_i(l.reg));
        }
        line("xorl %r10d, %r10d");
        if (tag_.type_tags) {
            line("movq $0, pa_ptag_in(%rip)");
            line("movq $0, pa_ptag_len(%rip)");
        }
        if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        line("call *%r11");
        reload_fp_slots();

        if (!dst_unused(in)) {
            if (ir_is_float(in.ty)) move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
            else emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        }
    }


}  // namespace pyadamas
