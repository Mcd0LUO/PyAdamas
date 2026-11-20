// ra_opt.cpp —— PYADAMAS_RA_OPT：callee-saved 提升增强（默认关）
//
// 两个分配改进（docs/ra-opt-design.md），都只改「哪个值住哪个寄存器」，
// 不改指令语义，机制与现有跨块提升完全同构（整函数独占 callee-saved，
// mem_mop 直取寄存器，无区间判断、无调用点 spill）：
//
// P1 同块调用块提升：定义与使用同块、但所在块含调用的整型 vreg。
//    它们 cross_block_=false 进不了跨块提升池，块内有调用又拿不到
//    %r8-%r11 块临时池 —— fib 递归的 fib(n-1) 结果正是如此，每次调用
//    前后 spill/reload（perf 实测递归块栈 mov 占函数周期 ~38%）。
//    这里只收**活跃区间真跨过调用**的值（单定义），按 weight 排序后
//    **追加在跨块候选之后**捡剩余 callee-saved（不按权重插队，不劣化
//    已有分配）；自递归函数整函数跳过（叶子 push/pop 税，实测负收益）。
//
// P2 只读槽 load 别名：vreg 是「整函数驻留 GPR 且函数内从未被写」的槽
//    的单次 LoadSlot —— 值恒等于槽寄存器，直接别名，不占新寄存器。
//    nqueens LICM 提升出的 row/n 副本（3+3 份独立 vreg 各自溢出，
//    perf 实测 row 重载占函数周期 9%）正是如此。move_to 对 dst==src
//    跳过，提升点的 load 指令整条消失。
//
// 正确性命门（继承 assign_global_vreg_regs 的现有门槛，不放宽）：
//   - opt_>=2、非生成器（gen_）、非 has_handlers（setjmp/longjmp 会回滚
//     callee-saved；零开销展开按 CFI 恢复，与现有跨块提升同暴露面）；
//   - vthread：pa_vt_swap 保存/恢复全部 6 个 callee-saved（rt_vthread.c），
//     挂起/切换安全，与现有跨块提升同暴露面；
//   - 类型白名单只收 Int/Bool（比跨块池的黑滤更严：heap/unknown/optional/
//     float/BoundMethod/StructPtr 一律不碰，GC 根/帧记录/tag 零风险）；
//   - 单定义（def_count==1）：寄存器只被写一次（定义点），此后只读，
//     不存在原地改写/区间外过期值问题；
//   - RcMark 水位不碰（与跨块池同口径）；
//   - 被提升 vreg 的栈槽若与它值共享（线性扫描），提升后永不读写栈槽，
//     共享方不受影响；int/bool 帧临时区 GC 零扫描（build_frame_scan_info）。
#include "codegen_impl.h"

namespace pyadamas {

// 与 slot_range_has_call 同口径：这些指令破坏 caller-saved，所在块进不了
// %r8-%r11 块临时池（temp_reg_op_ok），值只能走栈或 callee-saved。
static bool ra_opt_is_call(IrOp op) {
    switch (op) {
        case IrOp::Call:
        case IrOp::CallRt:
        case IrOp::CallExtern:
        case IrOp::CallPtr:
        case IrOp::CallDirect:
        case IrOp::VCall:
        case IrOp::VCallStatic:
        case IrOp::SuperVCall:
        case IrOp::Setjmp:
            return true;
        default:
            return false;
    }
}

// 类型白名单：只收「确定是整型机器表示」的值。比跨块候选池的黑滤
// （排 Float/Unknown/Optional）更严 —— heap 值涉及 GC 根/帧记录，
// BoundMethod/StructPtr 涉及保守扫描，本轮一律不碰，宁少勿错。
static bool ra_opt_int_ty(const TypePtr& t) {
    if (!t) return false;
    return t->kind == TyKind::Int || t->kind == TyKind::Bool;
}

// 一次扫描收集两个改进需要的全部事实。def_* 只记第一次定义（调用方
// 都要求 def_count==1，多定义直接出局）。
//
// slot_write：idx 字段**只有这些 op 当作帧槽用**（ir_ops.h 全表核对）：
//   StoreSlot（写）/ SlotAddr（取地址）/ CallRt（DictTryGet 等运行时
//   拿槽地址直写；任何带 idx 的 CallRt 一律保守取消资格）。
// 其余 op 的 idx 是全局/外部符号/函数/类下标（LoadGlobal、CallDirect、
// CallExtern……），与帧槽无关 —— 曾误把 CallDirect f1 的 idx=1 当成
// 「写过槽 1」，nqueens 的 n（s1）别名整个漏掉。LoadSlot 只读，不算。
struct RaOptInfo {
    std::vector<int> def_cnt;
    std::vector<IrOp> def_op;
    std::vector<int> def_idx;
    std::vector<int64_t> def_imm;
    std::vector<char> blk_call;
    std::vector<char> slot_write;
    // P1 用：同块 vreg 的活跃区间是否真跨过一条调用（定义在前、使用在后，
    // 中间隔着 call）。块内位置编码。只收跨调用值 —— 实测教训：把调用块里
    // 所有同块值都提升（fib 5 个），n_saved 从 1 涨到 6，**每次调用（含
    // n<2 的叶子）多付 5 对 push/pop**，序言尾声开销吃掉全部收益还倒亏
    // 6%（perf 实测 push 占 27%、pop 占 15%）。跨调用值才是非提升不可的
    // （caller-saved 池在调用块不可用）；不跨调用的单使用值往返虽浪费，
    // 但提升它们的寄存器保存成本摊到所有路径上，得不偿失。
    std::vector<char> crosses_call;
};

static RaOptInfo ra_opt_scan(const IRFunction& f) {
    const size_t nv = f.vreg_ty.size();
    const size_t ns = f.slot_ty.size();
    const size_t nb = f.blocks.size();
    RaOptInfo info;
    info.def_cnt.assign(nv, 0);
    info.def_op.assign(nv, IrOp::ConstInt);  // 占位，def_cnt==0 时不读
    info.def_idx.assign(nv, -1);
    info.def_imm.assign(nv, 0);
    info.blk_call.assign(nb, 0);
    info.slot_write.assign(ns, 0);
    info.crosses_call.assign(nv, 0);
    std::vector<int> def_pos(nv, -1);
    std::vector<int> def_blk(nv, -1);
    for (size_t b = 0; b < nb; ++b) {
        // last_call：当前位置之前（不含）最近一条调用的块内位置。
        int last_call = -1;
        int pos = 0;
        for (const auto& in : f.blocks[b].insts) {
            if (ra_opt_is_call(in.op)) info.blk_call[b] = 1;
            if (in.idx >= 0 && in.idx < static_cast<int>(ns) &&
                (in.op == IrOp::StoreSlot || in.op == IrOp::SlotAddr ||
                 in.op == IrOp::CallRt))
                info.slot_write[static_cast<size_t>(in.idx)] = 1;
            auto use = [&](VReg r) {
                if (r == kNoReg || r >= static_cast<VReg>(nv)) return;
                const size_t v = static_cast<size_t>(r);
                // 定义在本块、在使用之前，且中间隔着调用 → 跨调用。
                if (def_blk[v] == static_cast<int>(b) && def_pos[v] >= 0 &&
                    def_pos[v] < pos && last_call > def_pos[v])
                    info.crosses_call[v] = 1;
            };
            use(in.a); use(in.b); use(in.c);
            for (VReg r : in.args) use(r);
            if (in.dst != kNoReg && in.dst < static_cast<VReg>(nv)) {
                const size_t v = static_cast<size_t>(in.dst);
                if (info.def_cnt[v] == 0) {
                    info.def_op[v] = in.op;
                    info.def_idx[v] = in.idx;
                    info.def_imm[v] = in.imm;
                    def_pos[v] = pos;
                    def_blk[v] = static_cast<int>(b);
                }
                ++info.def_cnt[v];
            }
            if (ra_opt_is_call(in.op)) last_call = pos;
            ++pos;
        }
    }
    return info;
}

// P2：只读槽 load 别名。返回按 vreg 索引的「已别名」标记（调用方据此
// 把它们从候选池剔除，避免再占一个寄存器）。直接写 vreg_reg_，不占
// 新寄存器，n_saved_regs_ 不变。
std::vector<char> AsmGen::ra_opt_readonly_alias() {
    const size_t nv = f_->vreg_ty.size();
    const size_t ns = f_->slot_ty.size();
    std::vector<char> aliased(nv, 0);
    if (nv == 0 || ns == 0) return aliased;
    const RaOptInfo info = ra_opt_scan(*f_);
    // **寄存器必须全函数单射：没有第二个槽用同一个寄存器。**
    //
    // 命门 bug（实测 c2_inventory rows 600→1、204_list_buf_pool 算错）：
    // linear_scan_pool 会让**活跃区间不重叠的槽复用同一寄存器** ——
    // slot_reg_[s] 非空只表示「s 活跃期间住这里」，不是整函数独占！
    // gen_rows 的 s0（n）在入口 hoist load 之后就死了，%rbx 随即被
    // 循环里的 LCG 状态槽接管；别名让循环上界 cmp 读到 LCG 值，
    // 600 行变 1 行。跨块 vreg 提升没这个问题：那些寄存器从
    // n_saved_regs_ 起独占分配，不与槽复用。P2 要安全，必须确认
    // 该寄存器没有别的槽认领（参数槽/整函数活跃槽天然满足）。
    std::vector<int> reg_users(static_cast<size_t>(kMaxSlotRegs()), 0);
    for (size_t s2 = 0; s2 < ns; ++s2) {
        if (slot_reg_[s2].empty()) continue;
        for (int r = 0; r < kMaxSlotRegs(); ++r)
            if (slot_reg_[s2].text == kSlotRegs()[r]) ++reg_users[static_cast<size_t>(r)];
    }
    for (size_t v = 0; v < nv; ++v) {
        if (!vreg_reg_[v].empty()) continue;
        if (use_count_[v] == 0) continue;
        if (info.def_cnt[v] != 1) continue;
        if (info.def_op[v] != IrOp::LoadSlot) continue;
        const int s = info.def_idx[v];
        if (s < 0 || s >= static_cast<int>(ns)) continue;
        if (info.slot_write[static_cast<size_t>(s)]) continue;
        if (slot_reg_[static_cast<size_t>(s)].empty()) continue;
        if (slot_reg_[static_cast<size_t>(s)].kind != MOperandKind::Reg) continue;
        {
            bool exclusive = false;
            for (int r = 0; r < kMaxSlotRegs(); ++r)
                if (slot_reg_[static_cast<size_t>(s)].text == kSlotRegs()[r] &&
                    reg_users[static_cast<size_t>(r)] == 1)
                    exclusive = true;
            if (!exclusive) continue;
        }
        if (!ra_opt_int_ty(f_->slot_ty[static_cast<size_t>(s)])) continue;
        if (!ra_opt_int_ty(f_->vreg_ty[v])) continue;
        if (s < static_cast<int>(gc_scan_slot_.size()) &&
            gc_scan_slot_[static_cast<size_t>(s)])
            continue;
        // 值恒等：槽整函数驻留且从未被写，load 出来的值任何时刻都等于
        // 槽寄存器。别名整函数有效，跨调用/跨块都安全（callee-saved）。
        vreg_reg_[v] = slot_reg_[static_cast<size_t>(s)];
        aliased[v] = 1;
    }
    return aliased;
}

// P1：同块跨调用候选。返回 (vreg, weight)，由调用方排序后**追加在
// 跨块候选之后**（leftover-only，见 codegen_regalloc.cpp 钩子注释）。
// weight 沿用跨块池的「使用次数×10^循环深度」。
std::vector<std::pair<int, int>> AsmGen::ra_opt_call_block_cands(
        const std::vector<int64_t>& weight) {
    std::vector<std::pair<int, int>> out;
    const size_t nv = f_->vreg_ty.size();
    if (nv == 0) return out;
    // **自递归函数整函数跳过 P1**（实测负面结论，docs/ra-opt-design.md §1.1）：
    // 提升跨调用值要多压一个 callee-saved，而递归函数的**每次调用（含
    // 叶子）都付这对 push/pop**。fib 实测：只提升 fib(n-1) 结果（1 个
    // 寄存器），叶子调用白付税，净回归 3.8%；提升全部 5 个同块值回归
    // 6.2%（perf：push 27% / pop 15%）。递归调用次数 ≫ 表达式求值次数，
    // 保存成本摊不平。非递归函数调用次数有限，跨调用提升才是纯赚。
    // 局限：互递归（A↔B）没有 call graph 分析，不在此 guard 内 —— 实验
    // 门控默认关，残留风险如实记录。
    for (const auto& b : f_->blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::CallDirect && in.idx == fi_) return out;
    const RaOptInfo info = ra_opt_scan(*f_);
    for (size_t v = 0; v < nv; ++v) {
        if (cross_block_[v]) continue;  // 跨块池已收
        if (info.def_cnt[v] != 1) continue;
        if (def_block_[v] < 0) continue;
        if (!info.crosses_call[v]) continue;  // 只收真跨调用的值
        if (use_count_[v] == 0) continue;
        if (global_imm_.contains(static_cast<VReg>(v))) continue;
        if (!ra_opt_int_ty(f_->vreg_ty[v])) continue;
        // 与 assign_block_temp_regs 同口径：常量走立即数延迟，LoadSlot
        // 有自己的别名机制（P2 已覆盖只读槽的多使用副本）。
        const IrOp op = info.def_op[v];
        if (op == IrOp::LoadSlot || op == IrOp::ConstInt || op == IrOp::ConstNone)
            continue;
        // RcMark 水位（单定义下查定义指令即可，与 vreg_is_rc_mark 等价）。
        if (op == IrOp::CallRt &&
            info.def_imm[v] == static_cast<int64_t>(RtFn::RcMark))
            continue;
        out.emplace_back(static_cast<int>(v), static_cast<int>(weight[v]));
    }
    return out;
}

}  // namespace pyadamas
