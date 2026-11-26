// backend/call_rt.cpp —— 运行期调用的实参装填
//
// gen_call_rt 的 case 按实参形态（整数/浮点/混合）分组后搬到这里：
// 每个 gen_rt_* 只负责把实参搬进对应寄存器，调用序列和返回值落盘
// 仍由 backend/call.cpp 的 gen_call_rt 负责。
#include "backend/impl.h"


namespace pyadamas {

// 引用计数内联：pa_rc_inc/dec 的机器码生成

void AsmGen::gen_rc_dec(const Inst& in, const std::string& src,
                        const std::string& dst, int global) {
    const MReg p = mreg_from_name(arg_reg_i(0));
    move_to(mreg_operand(p), src_mop(in.args[0]));
    const std::string at_most_one = fresh_label("decle1");
    const std::string slow = fresh_label("decslow");
    const std::string done = fresh_label("decdone");
    const bool forward = global >= 0;
    const bool copy = forward && src != dst;
    const std::string fast = copy ? fresh_label("decforward") : done;
    emit2(MOp::kTestq, p, p);
    emit_jmp(MOp::kJe, slow);            // p == NULL
    line("cmpl $0, pa_gc_phase_fast(%rip)");
    emit_jmp(MOp::kJne, slow);           // 收集期只登记边，不能重入释放
    emit2(MOp::kMovq, op_mem(p, -48), kRax);
    TypePtr t = ty_of(in.args[0]);
    const bool check_first = t && (t->kind == TyKind::Str || t->kind == TyKind::Bytes);
    if (check_first) {
        emit_imm(MOp::kMovabsq, INT64_MIN, target_abi().scratch_len);
        emit2(MOp::kCmpq, target_abi().scratch_len, kRax);
        emit_jmp(MOp::kJe, fast);
        emit_imm(MOp::kCmpq, 1, kRax);
        emit_jmp(MOp::kJle, slow);
    }
    // sub 同时计算新值和旧 rc 与 1 的有符号比较。仅 rc > 1 时写回。
    emit_imm(MOp::kSubq, 1, kRax);
    if (!check_first) emit_jmp(MOp::kJle, at_most_one);
    emit2(MOp::kMovq, kRax, op_mem(p, -48));
    emit_jmp(MOp::kJmp, fast);
    if (!check_first) {
        label(at_most_one);
        // jle 不改标志位，OF 仍来自 sub；仅 INT64_MIN - 1 溢出，不写回。
        emit_jmp("jo", fast);  // jo 不在 MOp 表：历史发射是列 0 Raw（无 tab），保持字节不变
    }
    label(slow);
    spill_fp_slots();
    line("call pa_rc_dec_slow");
    reload_fp_slots();
    if (forward) move_to(dst, global_label(global) + "(%rip)");
    if (copy) {
        emit_jmp("jmp", done);
        label(fast);
        move_to(dst, src);
    }
    label(done);
}

// ── RC 记账的内联快路径 ──────────────────────────────────────
//
// 生成代码里 inc/dec/mark/cleanup 的出现频率极高（内层循环一轮几次），
// 但运行时那四个函数体分别只有 1~6 条指令：
//
//     pa_rc_mark()          -> return temp_len;                （1 条 load）
//     pa_rc_cleanup(m)      -> if (m == temp_len) return; ...  （1 条 cmp）
//     pa_rc_inc(p)          -> NULL/永生/rc<=0 判断 + h->rc++   （5 条）
//     pa_rc_dec(p)          -> 同上 + gc_phase 判断 + h->rc--   （6 条）
//
// call/ret + 被调用者保存 + PLT 跳板比它们干的活贵一个数量级。perf 实测
// （queue_sim / str_build 各自占比）：
//
//     pa_rc_cleanup  8.2% / 11.4%
//     pa_rc_inc      4.0% /  3.0%
//     pa_rc_dec      4.2% /  3.7%
//
// 内联结构照抄 rt_internal.h 的 pa_rc_inc_fast / pa_rc_dec_fast：慢路径
// （NULL、统计模式、GC 进行中、rc<=0、归零）仍然落回 pa_rc_*_slow，语义
// 逐条对齐。快路径不 clobber 任何寄存器，所以不需要落盘浮点槽；**慢路径
// 必须** spill_fp_slots / reload_fp_slots —— 它会真的调用运行时，可能跑
// 析构函数，而 xmm 全是 caller-saved。
//
// 返回 true 表示这条指令已经发完，gen_call_rt 不要再补 call。
bool AsmGen::gen_rc_inline(const Inst& in, RtFn fn) {
    // P3：unknown RC 三入口的 tag 判别（设计 §3.3）。tag>=3 直接
    // inc/dec/temp（免探针）；INT/FRAW 跳过；UNK 回退旧探针调用。
    if (tag_.tags_enabled &&
        (fn == RtFn::RcIncUnknown || fn == RtFn::RcDecUnknown ||
         fn == RtFn::RcTempUnknown) &&
        in.args.size() == 1 && vreg_tagged(in.args[0])) {
        gen_rc_unknown_tagged(in, fn);
        return true;
    }
    if (fn == RtFn::RcMark) {
        line("movq temp_len(%rip), %rax");
        if (!dst_unused(in)) emit2(MOp::kMovq, kRax, dst_mop(in.dst));
        return true;
    }
    if (fn == RtFn::RcCleanup && in.args.size() == 1) {
        // 只内联「水位没变就直接返回」这一条出口 —— 它是最常见的情形，
        // 其余（真的要逐个 dec）整段落回原函数。
        emit2(MOp::kMovq, src_mop(in.args[0]), kRdi);
        const std::string skip = fresh_label("cln");
        line("cmpq temp_len(%rip), %rdi");
        emit_jmp("je", skip);
        spill_fp_slots();
        line("call pa_rc_cleanup");
        reload_fp_slots();
        label(skip);
        return true;
    }
    // 字符串热点常操作驻留字符，优先检查永生哨兵；普通堆值则把哨兵
    // 判断留到冷分支。两条路径接受完全相同的计数，不依赖类型保证。
    const auto immortal_first = [&] {
        TypePtr t = ty_of(in.args[0]);
        return t && (t->kind == TyKind::Str || t->kind == TyKind::Bytes);
    };
    const auto skip_immortal = [&](const std::string& done) {
        const char* scratch = target_abi().scratch_len;
        emit_imm_hex(MOp::kMovabsq, 0x8000000000000000ULL, scratch);
        emit2(MOp::kCmpq, scratch, kRax);
        emit_jmp(MOp::kJe, done);
    };
    if (fn == RtFn::RcInc && in.args.size() == 1) {
        const MReg p = mreg_from_name(arg_reg_i(0));
        move_to(mreg_operand(p), src_mop(in.args[0]));
        const std::string nonpos = fresh_label("incnonpos");
        const std::string slow = fresh_label("incslow");
        const std::string done = fresh_label("incdone");
        emit2(MOp::kTestq, p, p);
        emit_jmp(MOp::kJe, slow);            // p == NULL
        emit2(MOp::kMovq, op_mem(p, -48), kRax); // h->rc：h = p - 64，rc 在 +16
        const bool check_first = immortal_first();
        if (check_first) skip_immortal(done);
        emit2(MOp::kTestq, kRax, kRax);
        emit_jmp(MOp::kJle, check_first ? slow : nonpos);
        emit_imm(MOp::kAddq, 1, kRax);
        emit2(MOp::kMovq, kRax, op_mem(p, -48));
        emit_jmp(MOp::kJmp, done);
        if (!check_first) {
            label(nonpos);
            // 只有 INT64_MIN（永生哨兵）取负会溢出；其余非法计数由慢路径报错。
            emit1(MOp::kNegq, kRax);
            emit_jmp("jo", done);  // 同上：列 0 Raw 兜底
        }
        label(slow);
        spill_fp_slots();
        line("call pa_rc_inc_slow");
        reload_fp_slots();
        label(done);
        return true;
    }
    if (fn == RtFn::RcDec && in.args.size() == 1) {
        gen_rc_dec(in);
        return true;
    }
    // ── 临时栈 push/take 内联（0.286）─────────────────────────────
    // str_build 逐字符循环里 pa_rc_temp 占 6.8%：字符串是永生值，push
    // 读 rc 比较 INT64_MIN 就返回，take 更要把整条栈扫一遍（永生值从
    // 不入栈）。call/ret + 扫描全是浪费。快路径把这两个形态变成 5 条
    // 指令；非永生 push 也就 12 条。stats/diag/扩容落回原函数。
    if (fn == RtFn::RcTemp && in.args.size() == 1) {
        emit2(MOp::kMovq, src_mop(in.args[0]), kRdi);
        const std::string slow = fresh_label("tmpslow");
        const std::string done = fresh_label("tmpdone");
        line("testq %rdi, %rdi");
        emit_jmp("je", done);
        line("movq -48(%rdi), %rax");
        line("movabsq $0x8000000000000000, %rcx");
        line("cmpq %rcx, %rax");
        emit_jmp("je", done);            // 永生：不入栈
        line("cmpl $0, pa_rc_stats_on_fast(%rip)");
        emit_jmp("jne", slow);
        line("cmpl $0, pa_diag_rc_on(%rip)");
        emit_jmp("jg", slow);            // diag>0 要记录（-1 视同 0，与运行时一致）
        line("movq temp_len(%rip), %rax");
        line("cmpq temp_cap(%rip), %rax");
        emit_jmp("jge", slow);           // 扩容
        line("movq temp_stack(%rip), %rcx");
        line("movq %rdi, (%rcx,%rax,8)");
        line("incq temp_len(%rip)");
        emit_jmp("jmp", done);
        label(slow);
        spill_fp_slots();
        line("call pa_rc_temp");
        reload_fp_slots();
        label(done);
        return true;
    }
    if (fn == RtFn::RcTempTake && in.args.size() == 1) {
        emit2(MOp::kMovq, src_mop(in.args[0]), kRdi);
        const std::string slow = fresh_label("tkslow");
        const std::string scan = fresh_label("tkscan");
        const std::string loop = fresh_label("tkloop");
        const std::string found = fresh_label("tkfound");
        const std::string done = fresh_label("tkdone");
        line("testq %rdi, %rdi");
        emit_jmp("je", done);
        line("movq -48(%rdi), %rax");
        line("movabsq $0x8000000000000000, %rcx");
        line("cmpq %rcx, %rax");
        emit_jmp("je", done);            // 永生：从不入栈，无可取（运行时同判）
        line("cmpl $0, pa_rc_stats_on_fast(%rip)");
        emit_jmp("jne", slow);
        line("movq temp_len(%rip), %rax");
        line("testq %rax, %rax");
        emit_jmp("jle", done);
        line("movq temp_stack(%rip), %rcx");
        line("cmpq %rdi, -8(%rcx,%rax,8)");
        emit_jmp("jne", scan);
        line("decq temp_len(%rip)");     // 栈顶命中
        emit_jmp("jmp", done);
        label(scan);
        line("subq $2, %rax");           // i = len-2，从后往前找
        label(loop);
        line("testq %rax, %rax");
        emit_jmp("jl", done);            // 找不到：与运行时一样静默返回
        line("cmpq %rdi, (%rcx,%rax,8)");
        emit_jmp("je", found);
        line("decq %rax");
        emit_jmp("jmp", loop);
        label(found);
        line("movq temp_len(%rip), %rdx");
        line("decq %rdx");
        line("movq %rdx, temp_len(%rip)");
        line("movq (%rcx,%rdx,8), %rdx");
        line("movq %rdx, (%rcx,%rax,8)");
        emit_jmp("jmp", done);
        label(slow);
        spill_fp_slots();
        line("call pa_rc_temp_take");
        reload_fp_slots();
        label(done);
        return true;
    }
    return false;
}


}  // namespace pyadamas
