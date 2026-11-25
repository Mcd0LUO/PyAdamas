// backend/call_rt.cpp —— 运行期调用的实参装填
//
// gen_call_rt 的 case 按实参形态（整数/浮点/混合）分组后搬到这里：
// 每个 gen_rt_* 只负责把实参搬进对应寄存器，调用序列和返回值落盘
// 仍由 backend/call.cpp 的 gen_call_rt 负责。
#include "backend/impl.h"

namespace pyadamas {


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

void AsmGen::gen_rt_none(const Inst& in) {
        (void)in;
    }

// 按实参个数装填 SysV 整数寄存器。分组表（gen_rt_args）里同一个
// gen_rt_iN 会承接不同参数个数的 RtFn（例如 Raise 只有 2 个实参，却和
// 4 参的 RtFn 同组）。以前无条件读 args[0..N-1] 会越界读 vector ——
// ASan 一跑就炸。这里按 args.size() 发射，多余寄存器不写。

void AsmGen::gen_rt_args_n(const Inst& in, int n) {
    const int m = static_cast<int>(in.args.size());
    const int k = m < n ? m : n;
    for (int i = 0; i < k; ++i) emit_arg(i, false, i, src_mop(in.args[i]));
}

void AsmGen::gen_rt_i1(const Inst& in) { gen_rt_args_n(in, 1); }

void AsmGen::gen_rt_i2(const Inst& in) { gen_rt_args_n(in, 2); }

void AsmGen::gen_rt_i3(const Inst& in) { gen_rt_args_n(in, 3); }

void AsmGen::gen_rt_i4(const Inst& in) { gen_rt_args_n(in, 4); }

void AsmGen::gen_rt_i5(const Inst& in) { gen_rt_args_n(in, 5); }

void AsmGen::gen_rt_i6(const Inst& in) { gen_rt_args_n(in, 6); }

void AsmGen::emit_arg(int pos, bool is_fp, int cls_idx, const MOperand& v) {
    const TargetABI& abi = target_abi();
    if (abi.positional_args) {
        // Win64：第 pos 个实参固定占第 pos 个槽，与类型无关；超过 4 个落栈。
        if (pos < 4) {
            if (is_fp) emit2(MOp::kMovsd, v, abi.arg_fp[pos]);
            else emit2(MOp::kMovq, v, abi.arg_int[pos]);
            return;
        }
        const int off = out_base() + 8 * (pos - 4);
        if (is_fp) {
            emit2(MOp::kMovsd, v, op_mem(kRsp, off));
        } else if (v.kind == MOperandKind::Mem) {
            /* 栈传实参的源操作数可能在内存里（mem-to-mem 非法）：借
               Win64 的 caller-saved scratch %r10 中转。实参寄存器只到
               %r9，%r10 不会和已装填的实参冲突。 */
            emit2(MOp::kMovq, v, kR10);
            emit2(MOp::kMovq, kR10, op_mem(kRsp, off));
        } else {
            emit2(MOp::kMovq, v, op_mem(kRsp, off));
        }
        return;
    }
    // SysV：整型与浮点各一套独立计数器；该类寄存器用完后落栈。
    if (is_fp) {
        if (cls_idx < abi.n_arg_fp) { emit2(MOp::kMovsd, v, abi.arg_fp[cls_idx]); return; }
        emit2(MOp::kMovsd, v, op_mem(kRsp, out_base() + 8 * cls_idx));
        return;
    }
    if (cls_idx < abi.n_arg_int) { emit2(MOp::kMovq, v, abi.arg_int[cls_idx]); return; }
    emit2(MOp::kMovq, v, op_mem(kRsp, out_base() + 8 * cls_idx));
}

void AsmGen::gen_rt_f1(const Inst& in) { emit_arg(0, true, 0, src_mop(in.args[0])); }

void AsmGen::gen_rt_f2(const Inst& in) {
        emit_arg(0, true, 0, src_mop(in.args[0]));
        emit_arg(1, true, 1, src_mop(in.args[1]));
    }

void AsmGen::gen_rt_f3(const Inst& in) {
        emit_arg(0, true, 0, src_mop(in.args[0]));
        emit_arg(1, true, 1, src_mop(in.args[1]));
        emit_arg(2, true, 2, src_mop(in.args[2]));
    }

void AsmGen::gen_rt_i1f1(const Inst& in) {
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, true, 0, src_mop(in.args[1]));
    }

void AsmGen::gen_rt_i1f2(const Inst& in) {
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, true, 0, src_mop(in.args[1]));
        emit_arg(2, true, 1, src_mop(in.args[2]));
    }

void AsmGen::gen_rt_i1f3(const Inst& in) {
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, true, 0, src_mop(in.args[1]));
        emit_arg(2, true, 1, src_mop(in.args[2]));
        emit_arg(3, true, 2, src_mop(in.args[3]));
    }

void AsmGen::gen_rt_f1i1(const Inst& in) {
        emit_arg(0, true, 0, src_mop(in.args[0]));
        emit_arg(1, false, 0, src_mop(in.args[1]));
    }

void AsmGen::gen_rt_i4opt5(const Inst& in) {
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, false, 1, src_mop(in.args[1]));
        emit_arg(2, false, 2, src_mop(in.args[2]));
        emit_arg(3, false, 3, src_mop(in.args[3]));
        if (in.args.size() > 4)
            emit_arg(4, false, 4, src_mop(in.args[4]));
    }

void AsmGen::gen_rt_reduce_f(const Inst& in) {
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, false, 1, src_mop(in.args[1]));
        emit_arg(2, true, 0, src_mop(in.args[2]));
        emit_arg(3, false, 2, src_mop(in.args[3]));
    }

void AsmGen::gen_rt_subprocess_run(const Inst& in) {
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, false, 1, src_mop(in.args[1]));
        emit_arg(2, false, 2, src_mop(in.args[2]));
        emit_arg(3, true, 0, src_mop(in.args[3]));
        emit_arg(4, false, 3, src_mop(in.args[4]));
        emit_arg(5, false, 4, src_mop(in.args[5]));
        emit_arg(6, false, 5, src_mop(in.args[6]));
    }

void AsmGen::gen_rt_args(const Inst& in, RtFn fn) {
    switch (fn) {
        case RtFn::EhLand:
            // landing pad 入口：展开包装指针由 personality 放进 %rax（GR0），
            // 不占普通实参位；直接搬到第一个整型实参寄存器。
            emit2("movq", "%rax", arg_reg_i(0));
            break;
        case RtFn::EhEnter:
        case RtFn::EhLeave:
        case RtFn::StrFromNone:
        case RtFn::PanicDiv0:
        case RtFn::PanicOverflow:
        case RtFn::PrintNone:
        case RtFn::PrintSep:
        case RtFn::PrintEnd:
        case RtFn::RcMark:
        case RtFn::EhReraise:
        case RtFn::EhRethrow:
        case RtFn::RcShutdown:
        case RtFn::GcShutdown:
        case RtFn::HandlerPush:
        case RtFn::AsyncioFutureNew:
        case RtFn::EventNew:
        case RtFn::LockNew:
        case RtFn::TaskGroupNew:
        case RtFn::ExcAccept:
        case RtFn::ExcAcceptValue:
        case RtFn::ExcAcceptInstance:
        case RtFn::ExcDone:
        case RtFn::ExcClear:
        case RtFn::ExcReraise:
        case RtFn::ExcTakeValue:
        case RtFn::ExcTypeName:
        case RtFn::ExcMessage:
        case RtFn::Reraise:
        case RtFn::SysArgv:
        case RtFn::SysGetRecursionLimit:  // () -> i64
        case RtFn::SysExecutable:   // () -> str*（sys.executable）
        case RtFn::LangGapsEnable:  // P4 块3：() -> void，运行期门控
        case RtFn::ExcInfo:         // P4 块3：() -> list*（sys.exc_info）
        case RtFn::RandGNext:
        case RtFn::OsGetcwd:
        case RtFn::OsCpuCount:
        case RtFn::TimeTime:
        case RtFn::TimeMonotonic:
        case RtFn::TimePerfCounter:
        case RtFn::OsGetpid:
        case RtFn::OsGetuid:
        case RtFn::OsGeteuid:
        case RtFn::OsGetppid:
        case RtFn::TimeTimeNs:
        case RtFn::TimeMonotonicNs:
        case RtFn::TimePerfCounterNs:
        case RtFn::TimeProcessTime:
        case RtFn::RegexPurge:
        case RtFn::OsEnviron:
        case RtFn::VtShutdown:
            gen_rt_none(in);
            break;
        case RtFn::RegexGroups:
        case RtFn::RegexGroupDict:
        case RtFn::RegexLastIndex:
        case RtFn::RegexLastGroup:
        case RtFn::RegexPatternGroups:
        case RtFn::RegexPatternFlags:
        case RtFn::RegexPatternStr:
        case RtFn::RegexEscape:
        case RtFn::OsUrandom:
        case RtFn::OsStrerror:
        case RtFn::RandBytes:
        case RtFn::SysIntern:
        case RtFn::StrFromI64:
        case RtFn::StrFromU64:  // P1 u64：同样只发第 0 个字，零扩展值
        case RtFn::StrAny:
        case RtFn::StrFromBool:
        case RtFn::PrintI64:
        case RtFn::PrintU64:  // P1 u64：单个整型实参，无需额外编组
        case RtFn::PrintAny:
        case RtFn::PrintAnyRepr:
        case RtFn::TruthAny:
        case RtFn::PrintBool:
        case RtFn::Panic:
        case RtFn::RcInc:
        case RtFn::RcDec:
        case RtFn::RcTemp:
        case RtFn::RcTempUnknown:
        case RtFn::RcTempTake:
        case RtFn::RcCleanup:
        case RtFn::RcIncUnknown:
        case RtFn::RcDecUnknown:
        case RtFn::GcWriteBarrier:
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
        case RtFn::StrCapwords:
        case RtFn::Sha256Digest:
        case RtFn::Sha256Hex:
        case RtFn::B64Decode:
        case RtFn::BytesToList:
        case RtFn::BytesFromList:
        case RtFn::BytesZeros:
        case RtFn::StrWrapList:
        case RtFn::ListCopy:
        case RtFn::ListReversed:
        case RtFn::EnumerateList:
        case RtFn::ListReverse:
        case RtFn::IntBitLength:
        case RtFn::VtSpawn:
        case RtFn::VtJoin:
        case RtFn::VtSleep:
        case RtFn::ChanRecv:
        case RtFn::ChanRecvF:
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
        case RtFn::ExcValueTypeName:
        case RtFn::ExcValueArgs:
        case RtFn::ExcValueStr:
        case RtFn::ExcValueCause:
        case RtFn::RaiseValue:
        case RtFn::ExcReraiseValueIf:
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
        case RtFn::ClassCtor:
        case RtFn::ClassName:
        case RtFn::ClassRepr:
        case RtFn::ExcClassObj:   // P4：(name) -> class*
        case RtFn::TbFrames:      // P4 块3：(tb*) -> list*
        case RtFn::ReprSet:       // P4 块4：(set*) -> str*
        case RtFn::ReprBytes:     // P4 块4：(bytes*) -> str*
        case RtFn::SysSetRecursionLimit:
        case RtFn::SysExit:
        case RtFn::PrintStr:
        case RtFn::PrintBytes:
        case RtFn::RandNew:
        case RtFn::RandGSeed:
        case RtFn::RandNext:
        case RtFn::SysFloatInfoI:
        case RtFn::SysFlagField:
        case RtFn::OsUname:
        case RtFn::OsUnameField:
        case RtFn::OsStat:
        case RtFn::OsLstat:
        case RtFn::OsDup:
        case RtFn::OsMkdir:
        case RtFn::OsRmdir:
        case RtFn::OsRemove:
        case RtFn::OsClose:
        case RtFn::OsPathIslink:
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
        case RtFn::RandGChoice:
        case RtFn::RandGShuffle:
        case RtFn::DequeNew:
        case RtFn::DequePop:
        case RtFn::DequePopleft:
        case RtFn::DequeLen:
        case RtFn::DequeMaxlen:
        case RtFn::DequeClear:
        case RtFn::DequeToList:
        case RtFn::DequeReverse:
        case RtFn::DequeCopy:
        case RtFn::OsChdir:
        case RtFn::DYLIBLoad:
        case RtFn::DYLIBClose:
        case RtFn::OsPathRealpath:
        case RtFn::OsPathGetsize:
        case RtFn::OsPathGetmtime:
        case RtFn::OsPathSplit:
        case RtFn::DateTimeNow:
        case RtFn::DateTimeIsoformat:
        case RtFn::TimeDeltaTotalSeconds:
        case RtFn::TimeDeltaNeg:
        case RtFn::TimeDeltaStr:
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
        case RtFn::PathReadBytes:
        case RtFn::PathIsAbsolute:
        case RtFn::PathResolve:
        case RtFn::PathAbsolute:
        case RtFn::PathTouch:
        case RtFn::PathIterdir:
        case RtFn::PathParts:
        case RtFn::PopenPid:
        case RtFn::PopenReturnCode:
        case RtFn::PopenStdout:
        case RtFn::PopenStderr:
        case RtFn::PopenStdin:
        case RtFn::PopenKill:
        case RtFn::PopenTerminate:
        case RtFn::AnyToF64:
        case RtFn::AnyToI64:
        case RtFn::AnyIsFloat:
        case RtFn::AttrDict:
        case RtFn::AnyIterList:
        case RtFn::ReprAny:
        // P3 tag 版：实际在 gen_call_rt 里由 gen_any_tagged 整段发射
        // （tag 装填 + 双字返回），这里只为 switch 穷尽。
        case RtFn::AnyToF64T:
        case RtFn::AnyToI64T:
        case RtFn::AnyIsFloatT:
        case RtFn::AnyLoadTag:
        case RtFn::OsWalk:
        case RtFn::OsSystem:
        case RtFn::TimeMktime:
        case RtFn::AnyNegT:
            gen_rt_i1(in);
            break;
        case RtFn::ListNew:
        case RtFn::ClassMethod:
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
        case RtFn::StrRepeat:
        case RtFn::ListRepeat:
        case RtFn::B64Encode:
        case RtFn::IterProduct:
        case RtFn::IterZipLongest:
        case RtFn::IterAccumulate:
        case RtFn::BytesAt:
        case RtFn::RegexGroupStr:
        case RtFn::RegexExpand:
        case RtFn::RegexFindIter:
        case RtFn::Raise:
        case RtFn::RaiseOwn:
        case RtFn::RaiseArgs:
        case RtFn::ExcEnter:
        case RtFn::AsyncioWaitFd:
        case RtFn::ComplexAdd:
        case RtFn::ComplexSub:
        case RtFn::ComplexMul:
        case RtFn::ComplexDiv:
        case RtFn::ComplexEq:
        case RtFn::ExcValueNew:
        case RtFn::ExcValueNewArgs:
        case RtFn::ExcRegister:
        case RtFn::RaiseValueFrom:
        case RtFn::RaiseInstance:
        case RtFn::FutureSetResult:
        case RtFn::FutureSetException:
        case RtFn::FutureSetExceptionValue:
        case RtFn::FutureAddCallback:
        case RtFn::FmtI64:
        case RtFn::FmtU64:  // P1 u64：(值, spec) 两个实参，与 FmtI64 同形
        case RtFn::FmtBool:
        case RtFn::FmtStr:
        case RtFn::FmtComplex:
        case RtFn::AsyncGenAthrow:
        case RtFn::GenNextOpt:
        case RtFn::GenSend:
        case RtFn::GenThrow:
        case RtFn::AsyncGenAsend:
        case RtFn::SetEq:
        case RtFn::DictGetOpt:
        case RtFn::RangeGet:
        case RtFn::ReprDictAny:
        case RtFn::ReprTuple:
        case RtFn::RangeContains:
        case RtFn::RangeEq:
        case RtFn::RangeSumI64:
        case RtFn::RandSeed:
        case RtFn::RandChoice:
        case RtFn::RandShuffle:
        case RtFn::RandGRandint:
        case RtFn::DequeFromList:
        case RtFn::DequeAppend:
        case RtFn::DequeAppendLeft:
        case RtFn::DequeGet:
        case RtFn::DequeRotate:
        case RtFn::DequeExtend:
        case RtFn::DequeExtendLeft:
        case RtFn::OsChmod:
        case RtFn::OsAccess:
        case RtFn::OsMakedirs:
        case RtFn::OsPathSamefile:
        case RtFn::RandGetrandbits:
        case RtFn::OsRename:
        case RtFn::TimeStrptime:
        case RtFn::OsRead:
        case RtFn::OsWrite:
        case RtFn::OsFdopen:
        case RtFn::OsGetenv:
        case RtFn::DateTimeField:
        case RtFn::DateTimeStrftime:
        case RtFn::TimeDeltaField:
        case RtFn::TimeDeltaCmp:
        case RtFn::TimeDeltaAdd:
        case RtFn::TimeDeltaSub:
        case RtFn::TimeDeltaMul:
        case RtFn::TimeDeltaFloorDivInt:
        case RtFn::TimeDeltaTrueDivInt:
        case RtFn::TimeDeltaFloorDivTD:
        case RtFn::TimeDeltaTrueDivTD:
        case RtFn::DateTimeReplace:
        case RtFn::DateTimeDiff:
        case RtFn::DateTimeCmp:
        case RtFn::PathJoin:
        case RtFn::PathWriteText:
        case RtFn::PathWriteBytes:
        case RtFn::PathGlob:
        case RtFn::PathRelativeTo:
        case RtFn::PathWithSuffix:
        case RtFn::PopenSendSignal:
        case RtFn::PopenCommunicate:
        case RtFn::AsyncioGather:
        case RtFn::AsyncioCreateTask:
        case RtFn::ChanNew:
        case RtFn::ChanSend:
        case RtFn::EhRaise:
        case RtFn::EhRaiseValueFrom:
        case RtFn::ExcClassIsA:        // P4：(name, cls)
        case RtFn::ExcSubclass:        // P4：(cls_a, cls_b)
        case RtFn::ExcClassValueNew:   // P4：(cls, msg|0)
        case RtFn::ExcClassValueNewArgs: // P4：(cls, args)
        case RtFn::TimeStrftime:
        case RtFn::MethodCall0:        // P4：(obj, method)
            gen_rt_i2(in);
            break;
        case RtFn::FileSeek:
        case RtFn::RegexSearch:
        case RtFn::RegexFindAllGroupsStr:
        case RtFn::RegexFindAllStr:
        case RtFn::RegexFindIterStr:
        case RtFn::RegexSplit:
        case RtFn::GenSendOpt:
        case RtFn::GenThrowOpt:
        case RtFn::ListSet:
        case RtFn::ListInsert:   /* 0.303：(list*, i, v, heap) */
        case RtFn::DictSet:
        case RtFn::ListContains:
        case RtFn::ListContainsI64:
        case RtFn::ListContainsStr:
        case RtFn::ListEq:
        case RtFn::DictEq:
        case RtFn::ListSortedBy:
        case RtFn::DictBump:
        case RtFn::DictTryGet:  // 实际在 gen_call_rt 特判发射，这里只为穷尽
            gen_rt_i4(in);
            break;
        case RtFn::AnyNeg:
        case RtFn::AnyInvert:
            gen_rt_i2(in);
            break;
        case RtFn::AnyTypeIs:  // P2：gen_call_rt 特判装填 (v, tag, code)，这里只为穷尽
        case RtFn::AnyDictItems:  // P2：特判装填 (v, tag)
            gen_rt_i2(in);
            break;
        case RtFn::DictGetOr:
        case RtFn::DictRemove:
        case RtFn::RangeNew:
        case RtFn::ListIndex:
        case RtFn::ListCount:
        case RtFn::RegexCompile:
        case RtFn::RegexFindAll:
        case RtFn::RegexGroup:
        case RtFn::RegexStart:
        case RtFn::RegexEnd:
        case RtFn::RegexSpan:
        case RtFn::FileConfig:
        case RtFn::IterChain:
        case RtFn::IterRepeat:
        case RtFn::IterPermutations:
        case RtFn::IterCombinations:
        case RtFn::IterCombinationsWR:
        case RtFn::CollCounter:
        case RtFn::ExcLeave:
        case RtFn::RandRandint:
        case RtFn::AttrGet:
        case RtFn::AttrSet:
        case RtFn::AttrHas:
        case RtFn::MethodRegister:  // P4：(qual, method, slot)
        case RtFn::DequeSet:
        case RtFn::DequeRemove:
        case RtFn::DequeCount:
        case RtFn::DequeIndex:
        case RtFn::DequeInsert:
        case RtFn::RandSample:
        case RtFn::RandChoices:
        case RtFn::TimeDeltaNew:
        case RtFn::DateTimeAddDelta:
        case RtFn::DateNew:
        case RtFn::BigPowMod:    /* (big*, big*, big*) */
        case RtFn::ListAppend:   /* (list*, v, heap) */
        case RtFn::ListSort:     /* 0.312：(list*, kind, desc) */
        case RtFn::SortedList:   /* 0.312：(list*, kind, desc) */
        // P3 tag 版（IR 实参 {op,a,b}）：gen_any_tagged 整段发射。
        case RtFn::AnyArithT:
        case RtFn::AnyCmpT:
            gen_rt_i3(in);
            break;
        case RtFn::AnyBigBinary:
        case RtFn::AnyArith:     /* (op, a, b, desc_a, desc_b) */
        case RtFn::AnyCmp:
            gen_rt_i5(in);
            break;
        case RtFn::ClassNew:
        case RtFn::ListPrep:
        case RtFn::ListGet:
        case RtFn::AnyIndex:
        case RtFn::AnyDelIndex:
        case RtFn::ListSetLen:
        case RtFn::StrConcat:
        case RtFn::StrCharAt:
        case RtFn::StrCharAtCursor:
        case RtFn::BytesConcat:
        case RtFn::BytesRepeat:
        case RtFn::ClosureNew:
        case RtFn::DictGet:
        case RtFn::DictSetDefaultFactory:
        case RtFn::DictContains:
        case RtFn::DictVersion:
        case RtFn::DictKeyAt:
        case RtFn::IsInstanceClass:
        case RtFn::StrContains:
        case RtFn::StrLenBytes:
        case RtFn::StrEncode:
        case RtFn::BytesDecode:
        case RtFn::DictUpdate:
        case RtFn::ListExtend:
        case RtFn::ListInplaceRepeat:
        case RtFn::SetUnion:
        case RtFn::SetIntersect:
        case RtFn::SetDifference:
        case RtFn::SetSymDiff:
        case RtFn::SetIntersectUpdate:
        case RtFn::SetDifferenceUpdate:
        case RtFn::SetSymdiffUpdate:
        case RtFn::ListDelIndex:
        case RtFn::ListSumI64:
        case RtFn::IsInstance:
        case RtFn::ObjClassId:
        case RtFn::IsNamedClass:
        case RtFn::IsInstanceOf:
        case RtFn::IsException:
        case RtFn::StrEqUnknown:
        case RtFn::ComplexEqUnknown:
        case RtFn::StrToI64:
        case RtFn::IntToBase:
        case RtFn::UIntToBase:  // P1 u64：与 IntToBase 同形（值 + 进制）
        case RtFn::RoundI64:
        case RtFn::CpyCall:
        case RtFn::FuncReduce:
        case RtFn::StrPercentFormat:
        case RtFn::RegexSearchStr:
        case RtFn::RegexSub:
        case RtFn::RegexSubn:
        case RtFn::RegexSubnStr:
        case RtFn::RegexSubFn:
        case RtFn::RegexSplitStr:
        case RtFn::ListSlice:
        case RtFn::ListSliceDel:
        case RtFn::StrSlice:
        case RtFn::BytesSlice:
        case RtFn::RandRange:
        case RtFn::PathMkdir:
            gen_rt_i4(in);
            break;
        case RtFn::DictNew:
        case RtFn::SetFromList:
        case RtFn::StrMethod:
        case RtFn::BytesMethod:
        case RtFn::ListCopyRange:
        case RtFn::JsonDumps:
        case RtFn::JsonDump:
            gen_rt_i5(in);
            break;
        case RtFn::JsonLoad:
            gen_rt_i1(in);
            break;
        case RtFn::JsonLoads:
        case RtFn::RegexSubStr:
        case RtFn::DictFromPairs:
        case RtFn::DateTimeNew:
        case RtFn::PopenNew:
            gen_rt_i6(in);
            break;
        case RtFn::JsonParse:  // (text) -> unknown
            gen_rt_i1(in);
            break;
        case RtFn::JsonOp:  // (op, a, b, c) -> i64
            gen_rt_i4(in);
            break;
        case RtFn::FileOpen:
            // (path, fd, mode, buffering, newline, closefd) —— 六个实参。
            // 原来误放在 i4 组：newline/closefd 没进寄存器，运行时读到的是
            // 上一次调用留下的垃圾（newline 恒 0 → 写侧换行翻译从不生效，
            // closefd 随机）—— 这就是 N2。
            gen_rt_i6(in);
            break;
        case RtFn::AsyncioSleep:
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
        case RtFn::AnyFromF64:
        case RtFn::MathErf:
        case RtFn::MathErfc:
        case RtFn::MathGamma:
        case RtFn::MathLgamma:
        case RtFn::MathUlp:
        case RtFn::MathFrexp:
        case RtFn::MathModf:
        case RtFn::HashF64:
        case RtFn::TimeSleep:
        case RtFn::TimeLocaltime:
        case RtFn::TimeGmtime:
        case RtFn::SysFloatInfoF:
        case RtFn::RandGParetovariate:
        case RtFn::AsyncTimeoutNew:
            gen_rt_f1(in);
            break;
        case RtFn::ComplexNew:
        case RtFn::MathPow:
        case RtFn::MathFmod:
        case RtFn::MathAtan2:
        case RtFn::MathHypot:
        case RtFn::MathCopysign:
        case RtFn::MathNextafter:
        case RtFn::MathRemainder:
        case RtFn::MathIsclose:
        case RtFn::RandGUniform:
        case RtFn::RandGNormalvariate:
        case RtFn::RandGGammavariate:
        case RtFn::RandGBetavariate:
        case RtFn::RandGLognormvariate:
        case RtFn::RandGWeibullvariate:
            gen_rt_f2(in);
            break;
        case RtFn::MathFma:
            gen_rt_f3(in);
            break;
        case RtFn::ListContainsF64:
        case RtFn::ListSumF64:
        case RtFn::RangeSumF64:
        case RtFn::AsyncioWaitFor:
        case RtFn::RandExpovariate:
        case RtFn::PopenWait:
        case RtFn::ChanSendF:
            gen_rt_i1f1(in);
            break;
        case RtFn::RandUniform:
        case RtFn::RandGauss:
            gen_rt_i1f2(in);
            break;
        case RtFn::RandTriangular:
            gen_rt_i1f3(in);
            break;
        case RtFn::FmtF64:
        case RtFn::RoundF64:
        case RtFn::MathLdexp:
            gen_rt_f1i1(in);
            break;
        case RtFn::DictPop:
        case RtFn::ListSliceAssign:
        case RtFn::SuperLookup:
            gen_rt_i4opt5(in);
            break;
        case RtFn::FuncReduceF:
            gen_rt_reduce_f(in);
            break;
        case RtFn::SubprocessRun:
            gen_rt_subprocess_run(in);
            break;
        // ── P3 big：指针/整数实参走整型寄存器，f64 实参走 xmm ──────
        case RtFn::BigFromU64:
        case RtFn::BigFromI64:
        case RtFn::BigToI64:
        case RtFn::BigToF64:
        case RtFn::BigTruth:
        case RtFn::BigNeg:
        case RtFn::BigAbs:
        case RtFn::BigNot:
        case RtFn::BigStr:
        case RtFn::AnyToBig:
            gen_rt_i1(in);
            break;
        case RtFn::BigFromStr:
        case RtFn::BigAdd:
        case RtFn::BigSub:
        case RtFn::BigMul:
        case RtFn::BigFloorDiv:
        case RtFn::BigMod:
        case RtFn::BigPow:
        case RtFn::BigRound:   // (big*, int64)
        case RtFn::BigTrueDiv:
        case RtFn::BigCmp:
        case RtFn::BigEq:
        case RtFn::BigAnd:
        case RtFn::BigOr:
        case RtFn::BigXor:
        case RtFn::BigLShift:
        case RtFn::BigRShift:
        case RtFn::BigToBase:
        case RtFn::AnySetIndex:
            gen_rt_i3(in);
            break;
        case RtFn::OsOpen:
        case RtFn::OsLseek:
            gen_rt_i3(in);
            break;
        case RtFn::BigFmt:
            gen_rt_i2(in);
            break;
        case RtFn::BigFromF64:
            gen_rt_f1(in);
            break;
        case RtFn::BigCmpF64:
            gen_rt_i1f1(in);
            break;
    }
}

}  // namespace pyadamas
