// backend/index.cpp —— 列表下标写入的指令选择（0.303 从 backend/call.cpp 拆出）
//
// 写入路径必须和运行时 pa_list_set 一样维护 elem_kind / heap_bits：
// 静态容器类型是 unknown 时降级器不做 prep（store_list_elem 只 retain），
// 运行期 kind 可能与新值冲突 —— kind1 列表存进标量后种类不升级，销毁时
// pa_rc_dec_heap_batch 把标量当堆指针 dec，直接 SIGSEGV（0.302 复现）。
//
// 热路径纪律（0.298/0.302 教训）：
//   - kind0 + 标量（含 nbody/spectral/matmul 的 list[float] 快路径）几条
//     指令跳过：不读旧值、不探针、不写元数据；
//   - 种类冲突升级、unknown 探针、kind2 位图维护全在冷分支或运行时；
//   - 静态已知的标量绝不探针：大整数/浮点位模式可能恰好等于某个存活堆
//     地址，探针会误判成堆（与运行时 pa_list_store_kind 同一口径）。
//
// 寄存器约定与 gen_index_load 一致：实参走 ABI 寄存器，scratch 避开实参。
// 旧值占 scratch_val；新值临时 nv 与 kind 临时 kr 在 Win64 借 sl/sp（len
// 与 data 在快路径后已死），SysV 都是 %rax（顺序复用，逐字节输出不变）。
// 位图指针临时 bp 不能与 kr 冲突：Win64 借 nv，SysV 借 sp。
//
// 发射全走结构化 MOperand/MOp（块B2b）：寄存器编号 + op_mem 直建，
// 免「拼字符串→回解析」。jz/jc 故意留文本 emit_jmp：历史兜底是列 0 Raw。
#include "backend/impl.h"

namespace pyadamas {

// 新值的静态所有权种类：0 非堆 / 1 堆 / 2 unknown。与降级器 own_kind 同口径。
static int idx_store_heap_kind(TypePtr ct) {
    if (ir_is_heap(ct)) return 1;
    if (!ct || ct->kind == TyKind::Unknown) return 2;
    return 0;
}

void AsmGen::gen_index_store(const Inst& in) {
    const int hk = idx_store_heap_kind(ty_of(in.c));
    // P3：unknown 新值带 tag → 堆标志从 tag 导出交运行时（位图精确维护）。
    if (tag_.tags_enabled && hk == 2 && vreg_tagged(in.c)) {
        gen_index_store_tagged(in, false);
        return;
    }
    if (opt_ < 1) {
        emit2(MOp::kMovq, src_mop(in.a), arg_reg_i(0));
        emit2(MOp::kMovq, src_mop(in.b), arg_reg_i(1));
        emit2(MOp::kMovq, src_mop(in.c), arg_reg_i(2));
        emit_imm(MOp::kMovq, hk, arg_reg_i(3));
        line("call pa_list_set");
        return;
    }
    const MReg r0 = mreg_from_name(arg_reg_i(0)), r1 = mreg_from_name(arg_reg_i(1));
    const MReg sl = mreg_from_name(target_abi().scratch_len);
    const MReg sp = mreg_from_name(target_abi().scratch_ptr);
    const MReg sv = mreg_from_name(target_abi().scratch_val);
    const bool sv_is_rax = sv == kRax;
    const MReg nv = sv_is_rax ? sl : kRax;  // 新值临时
    const MReg kr = sv_is_rax ? sp : kRax;  // kind 临时
    const MReg bp = sv_is_rax ? nv : sp;    // 位图指针临时
    const MReg a2d = mreg_from_name(reg32(arg_reg_i(2)));
    std::string fast = fresh_label("idxsf");
    std::string neg = fresh_label("idxsneg");
    std::string done = fresh_label("idxsdone");
    std::string slow = fresh_label("idxsslow");
    std::string k23 = fresh_label("idxsk23");
    std::string k0cold = fresh_label("idxsk0c");
    std::string decunk = fresh_label("idxsdu");
    std::string decheap = fresh_label("idxsdh");
    emit2(MOp::kMovq, src_mop(in.a), r0);
    emit2(MOp::kMovq, src_mop(in.b), r1);
    emit2(MOp::kMovq, op_mem(r0, 0), sl);
    emit2(MOp::kCmpq, sl, r1);
    emit_jmp(MOp::kJb, fast);
    emit2(MOp::kTestq, r1, r1);
    emit_jmp(MOp::kJs, neg);
    emit2(MOp::kXorl, a2d, a2d);
    emit_imm(MOp::kMovq, hk, arg_reg_i(3));
    line("call pa_list_set");
    line("ud2");
    label(neg);
    emit2(MOp::kAddq, sl, r1);
    emit2(MOp::kCmpq, sl, r1);
    emit_jmp(MOp::kJb, fast);
    emit2(MOp::kMovq, sl, r1);
    emit2(MOp::kXorl, a2d, a2d);
    emit_imm(MOp::kMovq, hk, arg_reg_i(3));
    line("call pa_list_set");
    line("ud2");
    label(fast);
    if (is_fp(in.c)) {
        // ── list[float] kind0 快路径（0.298，逐字节保留）──────────
        // 标量进 kind0：种类不变、无位图、旧值必是标量 —— 不读旧值不释放。
        std::string owned = fresh_label("idxsowned");
        emit_imm(MOp::kCmpq, 0, op_mem(r0, 24));
        emit_jmp(MOp::kJne, owned);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        emit2(MOp::kMovsd, src_mop(in.c), kXmm0);
        emit2(MOp::kMovsd, kXmm0, op_mem(sp, r1, 8));
        emit_jmp(MOp::kJmp, done);
        label(owned);
        // kind1/2：整段交运行时（升级/位图/旧值释放都在 pa_list_set）。
        // 浮点按位模式走整型实参寄存器。
        emit2(MOp::kMovsd, src_mop(in.c), kXmm0);
        emit2(MOp::kMovq, kXmm0, arg_reg_i(2));
        emit_imm(MOp::kMovq, hk, arg_reg_i(3));
        spill_fp_slots();
        line("call pa_list_set");
        reload_fp_slots();
        emit_jmp(MOp::kJmp, done);
        label(done);
        return;
    }
    if (hk == 0) {
        // kind0 热路径：一次比较判 kind。1/2/3 全走冷分发（冷路径重读 kind），
        // 比「mov kind + cmp1 + je + cmp2 + jae」少 3 条指令。
        emit_imm(MOp::kCmpq, 0, op_mem(r0, 24));
        emit_jmp(MOp::kJne, k0cold);
    } else {
        emit2(MOp::kMovq, op_mem(r0, 24), kr);
        if (hk == 2) {
            // 静态 unknown 新值：只有 kind2 且无位图能留在内联（kind 恒 2，
            // 销毁走注册表，没有位要维护，不必探针）；其余交运行时探针。
            emit_imm(MOp::kCmpq, 2, kr);
            emit_jmp(MOp::kJne, slow);
            emit_imm(MOp::kCmpq, 0, op_mem(r0, 32));
            emit_jmp(MOp::kJne, slow);
        } else {
            // 静态已知堆值：冲突种类（kind0+堆）交运行时升级；
            // kind3 防御性交运行时（kind3 只应是空表，不变量破坏时按 2 处理）。
            emit_imm(MOp::kCmpq, 0, kr);
            emit_jmp(MOp::kJe, slow);
            emit_imm(MOp::kCmpq, 2, kr);
            emit_jmp(MOp::kJae, k23);
        }
    }
    // kind 与新值一致（或 hk2 的 kind2 无位图）：写新值。
    // hk==0（非堆标量）时旧值必是标量、Consumer 不用它做 dec ——
    // 不读旧值，省一条 load（热路径纪律：非堆标量不读旧值）。
    emit2(MOp::kMovq, op_mem(r0, 16), sp);
    if (hk != 0) emit2(MOp::kMovq, op_mem(sp, r1, 8), sv);
    emit2(MOp::kMovq, src_mop(in.c), nv);
    emit2(MOp::kMovq, nv, op_mem(sp, r1, 8));
    if (hk == 2) emit_jmp(MOp::kJmp, decunk);
    else if (hk == 1) emit_jmp(MOp::kJmp, decheap);
    else emit_jmp(MOp::kJmp, done);
    if (hk == 0) {
        // kind0 冷分发：1 -> 运行时（升级/释放），>=2 -> kind2/3 路径。
        label(k0cold);
        emit2(MOp::kMovq, op_mem(r0, 24), kr);
        emit_imm(MOp::kCmpq, 1, kr);
        emit_jmp(MOp::kJe, slow);
        emit_jmp(MOp::kJmp, k23);
    }
    if (hk != 2) {
        // kind2：读旧值、写新值、按新值堆性更新位图（有位图时）。
        label(k23);
        emit_imm(MOp::kCmpq, 3, kr);
        emit_jmp(MOp::kJe, slow);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        emit2(MOp::kMovq, op_mem(sp, r1, 8), sv);
        emit2(MOp::kMovq, src_mop(in.c), nv);
        emit2(MOp::kMovq, nv, op_mem(sp, r1, 8));
        emit2(MOp::kMovq, op_mem(r0, 32), bp);
        emit2(MOp::kTestq, bp, bp);
        emit_jmp("jz", decunk);
        // bts/btr 内存位操作：位偏移取 r1（元素下标），自动按 64 位字寻址。
        emit2(hk == 1 ? MOp::kBtsq : MOp::kBtrq, r1, op_mem(bp));
        emit_jmp(MOp::kJmp, decunk);
    }
    label(slow);
    emit2(MOp::kMovq, src_mop(in.c), arg_reg_i(2));
    emit_imm(MOp::kMovq, hk, arg_reg_i(3));
    spill_fp_slots();
    line("call pa_list_set");
    reload_fp_slots();
    emit_jmp(MOp::kJmp, done);
    label(decunk);
    emit2(MOp::kMovq, sv, arg_reg_i(0));
    spill_fp_slots();
    line("call pa_rc_dec_unknown");
    reload_fp_slots();
    emit_jmp(MOp::kJmp, done);
    if (hk == 1) {
        label(decheap);
        emit2(MOp::kMovq, sv, arg_reg_i(0));
        spill_fp_slots();
        line("call pa_rc_dec");
        reload_fp_slots();
    }
    label(done);
}

void AsmGen::gen_index_store_unchecked(const Inst& in) {
    // 调用方已证明 0 <= idx < len，跳过边界比较与负下标归一化，
    // 种类/位图维护与 gen_index_store 完全一致（0.303）。
    const int hk = idx_store_heap_kind(ty_of(in.c));
    // P3：同 gen_index_store 的 tag 路由。
    if (tag_.tags_enabled && hk == 2 && vreg_tagged(in.c)) {
        gen_index_store_tagged(in, true);
        return;
    }
    const MReg r0 = mreg_from_name(arg_reg_i(0)), r1 = mreg_from_name(arg_reg_i(1));
    const MReg sl = mreg_from_name(target_abi().scratch_len);
    const MReg sp = mreg_from_name(target_abi().scratch_ptr);
    const MReg sv = mreg_from_name(target_abi().scratch_val);
    const bool sv_is_rax = sv == kRax;
    const MReg nv = sv_is_rax ? sl : kRax;
    const MReg kr = sv_is_rax ? sp : kRax;
    const MReg bp = sv_is_rax ? nv : sp;
    // BCE kind0 证明快路径（0.335，PYADAMAS_LOOP_OPT）：imm==1 表示优化器
    // 已证循环入口 elem_kind==0 且循环内全是 hk0 标量存储（kind 恒 0）。
    // 纯 mov：无 kind 检查、无旧值读（kind0 旧值必是标量，无 RC）、无 call
    // ——块内临时值因此不再被冷路径 call 迫使溢出（matmul 内循环实测）。
    // 降级器发的 unchecked 存储 imm 恒 0，不走这里（flag-off 逐字节不变）。
    if (in.imm == 1 && hk == 0) {
        emit2(MOp::kMovq, src_mop(in.a), r0);
        emit2(MOp::kMovq, src_mop(in.b), r1);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        if (is_fp(in.c)) {
            emit2(MOp::kMovsd, src_mop(in.c), kXmm0);
            emit2(MOp::kMovsd, kXmm0, op_mem(sp, r1, 8));
        } else {
            emit2(MOp::kMovq, src_mop(in.c), nv);
            emit2(MOp::kMovq, nv, op_mem(sp, r1, 8));
        }
        return;
    }
    std::string done = fresh_label("idxudone");
    std::string slow = fresh_label("idxuslow");
    std::string k23 = fresh_label("idxuk23");
    std::string decunk = fresh_label("idxudu");
    std::string decheap = fresh_label("idxudh");
    emit2(MOp::kMovq, src_mop(in.a), r0);
    emit2(MOp::kMovq, src_mop(in.b), r1);
    if (is_fp(in.c)) {
        std::string owned = fresh_label("idxuowned");
        emit_imm(MOp::kCmpq, 0, op_mem(r0, 24));
        emit_jmp(MOp::kJne, owned);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        emit2(MOp::kMovsd, src_mop(in.c), kXmm0);
        emit2(MOp::kMovsd, kXmm0, op_mem(sp, r1, 8));
        emit_jmp(MOp::kJmp, done);
        label(owned);
        emit2(MOp::kMovsd, src_mop(in.c), kXmm0);
        emit2(MOp::kMovq, kXmm0, arg_reg_i(2));
        emit_imm(MOp::kMovq, hk, arg_reg_i(3));
        spill_fp_slots();
        line("call pa_list_set");
        reload_fp_slots();
        emit_jmp(MOp::kJmp, done);
        label(done);
        return;
    }
    emit2(MOp::kMovq, op_mem(r0, 24), kr);
    if (hk == 2) {
        emit_imm(MOp::kCmpq, 2, kr);
        emit_jmp(MOp::kJne, slow);
        emit_imm(MOp::kCmpq, 0, op_mem(r0, 32));
        emit_jmp(MOp::kJne, slow);
    } else {
        emit_imm(MOp::kCmpq, hk == 0 ? 1 : 0, kr);
        emit_jmp(MOp::kJe, slow);
        emit_imm(MOp::kCmpq, 2, kr);
        emit_jmp(MOp::kJae, k23);
    }
    emit2(MOp::kMovq, op_mem(r0, 16), sp);
    emit2(MOp::kMovq, op_mem(sp, r1, 8), sv);
    emit2(MOp::kMovq, src_mop(in.c), nv);
    emit2(MOp::kMovq, nv, op_mem(sp, r1, 8));
    if (hk == 2) emit_jmp(MOp::kJmp, decunk);
    else if (hk == 1) emit_jmp(MOp::kJmp, decheap);
    else emit_jmp(MOp::kJmp, done);
    if (hk != 2) {
        label(k23);
        emit_imm(MOp::kCmpq, 3, kr);
        emit_jmp(MOp::kJe, slow);
        emit2(MOp::kMovq, op_mem(r0, 16), sp);
        emit2(MOp::kMovq, op_mem(sp, r1, 8), sv);
        emit2(MOp::kMovq, src_mop(in.c), nv);
        emit2(MOp::kMovq, nv, op_mem(sp, r1, 8));
        emit2(MOp::kMovq, op_mem(r0, 32), bp);
        emit2(MOp::kTestq, bp, bp);
        emit_jmp("jz", decunk);
        emit2(hk == 1 ? MOp::kBtsq : MOp::kBtrq, r1, op_mem(bp));
        emit_jmp(MOp::kJmp, decunk);
    }
    label(slow);
    emit2(MOp::kMovq, src_mop(in.c), arg_reg_i(2));
    emit_imm(MOp::kMovq, hk, arg_reg_i(3));
    spill_fp_slots();
    line("call pa_list_set");
    reload_fp_slots();
    emit_jmp(MOp::kJmp, done);
    label(decunk);
    emit2(MOp::kMovq, sv, arg_reg_i(0));
    spill_fp_slots();
    line("call pa_rc_dec_unknown");
    reload_fp_slots();
    emit_jmp(MOp::kJmp, done);
    if (hk == 1) {
        label(decheap);
        emit2(MOp::kMovq, sv, arg_reg_i(0));
        spill_fp_slots();
        line("call pa_rc_dec");
        reload_fp_slots();
    }
    label(done);
}

}  // namespace pyadamas
