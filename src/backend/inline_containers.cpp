// Inline fast paths for common container operations.
#include "backend/impl.h"

namespace pyadamas {


// kind-0 元组分配内联快路径（0.280）：池命中 + gc_need_check==0 + diag 关
// 时直接在生成代码弹池初始化，省 call/ret + 装填 + 运行时检查；否则落回
// pa_tuple_new_filled。池块 dtor/release/kind/size 恒定（0.276 不变量）。
void AsmGen::gen_tuple_inline(const Inst& in) {
        const size_t n = in.args.size();
        std::string slow = fresh_label("tupslow");
        std::string done = fresh_label("tupdone");
        for (size_t ai = 0; ai < n; ++ai)
            emit2(MOp::kMovq, src_mop(in.args[ai]), arg_reg_i(2 + ai));
        // 注意 cmpl：gc_need_check / pa_diag_rc_on 是 4 字节 int，
        // cmpq 会连同相邻 .bss 变量一起读，高 4 字节非零时永真。
        line("cmpl $0, gc_need_check(%rip)");
        emit_jmp("jne", slow);
        line("cmpl $0, pa_diag_rc_on(%rip)");
        emit_jmp("jne", slow);
        line("movq pa_list_inline_pool(%rip), %rax");
        line("testq %rax, %rax");
        emit_jmp("jz", slow);
        line("movq (%rax), %r10");
        line("movq %r10, pa_list_inline_pool(%rip)");
        line("decl pa_list_inline_pool_count(%rip)");
        // RC 头：next(+0)/prev(+8)/rc(+16)/marked(+56)=UNTRACKED(2)
        line("movq $0, (%rax)");
        line("movq $0, 8(%rax)");
        line("movq $1, 16(%rax)");
        line("movq $2, 56(%rax)");
        // payload(+64..)：len/cap=8/data=h+112/elem_kind=0/
        // heap_bits=0/bits_cap=0
        emit_imm(MOp::kMovq, static_cast<int64_t>(n), op_mem(kRax, 64));
        line("movq $8, 72(%rax)");
        line("leaq 112(%rax), %r10");
        line("movq %r10, 80(%rax)");
        line("movq $0, 88(%rax)");
        line("movq $0, 96(%rax)");
        line("movq $0, 104(%rax)");
        for (size_t ai = 0; ai < n; ++ai) {
            const MReg areg = mreg_from_name(arg_reg_i(2 + ai));
            if (areg != kMRegNone) {
                emit2(MOp::kMovq, areg, op_mem(kRax, 112 + 8 * ai));
            } else {
                std::string aop = arg_reg_i(2 + ai);
                emit2("movq", aop, "%r10");
                emit2(MOp::kMovq, kR10, op_mem(kRax, 112 + 8 * ai));
            }
        }
        // 返回值是 payload（h+64），不是 RC 头 —— 运行时
        // rc_alloc_core 返回 h+1，内联路径必须一致。漏了这
        // 步会把头指针当对象值存进槽，dec 时 p-1 落到前一个
        // 对象内部，池链被Interior指针污染（0.280 调试实录）。
        line("leaq 64(%rax), %rax");
        emit_jmp("jmp", done);
        label(slow);
        emit_imm(MOp::kMovq, static_cast<int64_t>(n), arg_reg_i(0));
        emit_imm(MOp::kMovq, 0, arg_reg_i(1));
        for (size_t ai = n; ai < 4; ++ai)
            emit_imm(MOp::kMovq, 0, arg_reg_i(2 + ai));
        spill_fp_slots();
        line("call pa_tuple_new_filled");
        reload_fp_slots();
        label(done);
        emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        return;
}


// list.append 内联快路径（0.282）：len<cap（不扩容）、heap_bits==NULL
// （无位图维护）、elem_kind!=2（kind2 空表运行时会分配位图）时，append
// 就是 data[len++]=v。RC retain 不在这条指令里 —— 生成代码在 append 前
// 单独发 pa_rc_inc（见 lower_method_containers），语义不变。
// 守卫任一不满足落回 pa_list_append 原调用（扩容/位图/定型全在那边）。
// 0.303：静态 heap 0/1 时还要求运行期 elem_kind 与它一致 —— 不一致时
// 运行时要升级种类（kind1 存标量 / kind0 存堆值），内联路径不做探针，
// 直接落回。heap 参数是编译期常量（heap_arg），按立即数比较，热路径
// （kind0+0 / kind1+1）只多一条 cmp+jne。
// heap==2（unknown）不需要额外守卫：降级器对 heap2 的 append 必先调
// pa_list_prep（store_list_elem 的 lk==3 / vk==2 分支；其余 heap2 发射点
// 的目标表建表即 kind2），prep 已把 kind0/1 与新值堆性调和一致，kind2/3
// 由下面的 jae 守卫落回运行时（str_build 的 parts.append(str(...)) 全靠
// 这条快路径，恒落回会 +5.8%）。运行时 pa_list_append 的种类维护仍兜底
// 非 prep 调用方。
void AsmGen::gen_append_inline(const Inst& in) {
    const MReg lr = mreg_from_name(arg_reg_i(0));
    const MReg vr = mreg_from_name(arg_reg_i(1));
    // heap 参数形态：src_mop 对常量 vreg 返回 Imm 单元。
    int64_t heap = -1;
    const MOperand& hs = src_mop(in.args[2]);
    if (hs.kind == MOperandKind::Imm) heap = hs.imm;
    std::string slow = fresh_label("appslow");
    std::string done = fresh_label("appdone");
    emit2(MOp::kMovq, src_mop(in.args[0]), lr);
    emit2(MOp::kMovq, src_mop(in.args[1]), vr);
    emit2(MOp::kMovq, op_mem(lr), kRax);
    emit2(MOp::kCmpq, op_mem(lr, 8), kRax);
    emit_jmp(MOp::kJge, slow);
    emit_imm(MOp::kCmpq, 0, op_mem(lr, 32));
    emit_jmp(MOp::kJne, slow);
    if (heap == 0 || heap == 1) {
        // kind 必须与静态 heap 完全一致 —— 一条比较同时排除 kind2（位图）、
        // kind3（未定型）与冲突种类（0.303 种类维护）。与 0.282 守卫同指令
        // 数（只是立即数从 $2 变成 $heap），str_build 热路径零额外开销。
        emit_imm(MOp::kCmpq, heap, op_mem(lr, 24));
        emit_jmp(MOp::kJne, slow);
    } else {
        // heap==2（unknown）：kind0/1 已由紧邻的 prep 与新值堆性调和
        // （见函数头注释），kind2/3 落回运行时。
        emit_imm(MOp::kCmpq, 2, op_mem(lr, 24));
        emit_jmp(MOp::kJae, slow);
        if (heap != 2) {
            // 非常量 heap 参数（正常降级不会发生）：运行期比较。
            emit2(MOp::kMovq, src_mop(in.args[2]), kR10);
            emit2(MOp::kCmpq, op_mem(lr, 24), kR10);
            emit_jmp(MOp::kJne, slow);
        }
    }
    emit2(MOp::kMovq, op_mem(lr, 16), kR10);
    emit2(MOp::kMovq, vr, op_mem(kR10, kRax, 8));
    emit1(MOp::kIncq, op_mem(lr));
    emit_jmp(MOp::kJmp, done);
    label(slow);
    emit_arg(2, false, 2, src_mop(in.args[2]));
    spill_fp_slots();
    line("call pa_list_append");
    reload_fp_slots();
    label(done);
}


// str 下标 ASCII 快路径（0.287，gen_index_load 的 Str 分支）：
// 0<=i<pub 且 pub==len（全 ASCII）且字节<0x80 → 查 pa_char_tab 驻留表；
// 负索引/越界/非 ASCII 落回 pa_str_char_at（语义不变）。
void AsmGen::gen_str_index_inline(const Inst& in) {
        const MReg sr = mreg_from_name(arg_reg_i(0));
        const MReg ir = mreg_from_name(arg_reg_i(1));
        std::string slow = fresh_label("idxstrslow");
        std::string done = fresh_label("idxstrdone");
        emit2(MOp::kMovq, src_mop(in.a), sr);
        emit2(MOp::kMovq, src_mop(in.b), ir);
        emit2(MOp::kTestq, ir, ir);
        emit_jmp(MOp::kJs, slow);
        emit2(MOp::kMovq, op_mem(sr), kRax);
        emit2(MOp::kCmpq, kRax, ir);
        emit_jmp(MOp::kJae, slow);
        emit2(MOp::kCmpq, op_mem(sr, 8), kRax);
        emit_jmp(MOp::kJne, slow);
        emit2(MOp::kMovq, op_mem(sr, 24), kR10);
        emit2(MOp::kMovzbq, op_mem(kR10, ir), kRax);
        line("cmpq $0x80, %rax");
        emit_jmp("jae", slow);
        line("leaq pa_char_tab(%rip), %r10");
        line("movq (%r10,%rax,8), %rax");
        emit_jmp("jmp", done);
        label(slow);
        spill_fp_slots();
        line("call pa_str_char_at");
        reload_fp_slots();
        label(done);
        emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        return;
}

}  // namespace pyadamas
