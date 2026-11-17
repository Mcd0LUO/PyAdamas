// codegen_call_rt.cpp —— 运行期调用的实参装填
//
// gen_call_rt 的 case 按实参形态（整数/浮点/混合）分组后搬到这里：
// 每个 gen_rt_* 只负责把实参搬进对应寄存器，调用序列和返回值落盘
// 仍由 codegen_call.cpp 的 gen_call_rt 负责。
#include "codegen_impl.h"

namespace pyadamas {


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
    if (fn == RtFn::RcMark) {
        line("movq temp_len(%rip), %rax");
        if (!dst_unused(in)) emit2("movq", "%rax", dst_op(in.dst));
        return true;
    }
    if (fn == RtFn::RcCleanup && in.args.size() == 1) {
        // 只内联「水位没变就直接返回」这一条出口 —— 它是最常见的情形，
        // 其余（真的要逐个 dec）整段落回原函数。
        emit2("movq", src_op(in.args[0]), "%rdi");
        const std::string skip = fresh_label("cln");
        line("cmpq temp_len(%rip), %rdi");
        emit_jmp("je", skip);
        spill_fp_slots();
        line("call pa_rc_cleanup");
        reload_fp_slots();
        label(skip);
        return true;
    }
    if (fn == RtFn::RcInc && in.args.size() == 1) {
        emit2("movq", src_op(in.args[0]), "%rdi");
        const std::string slow = fresh_label("incslow");
        const std::string done = fresh_label("incdone");
        line("testq %rdi, %rdi");
        emit_jmp("je", slow);            // p == NULL
        line("movq -48(%rdi), %rax");    // h->rc：h = p - 64，rc 在 +16
        line("movabsq $0x8000000000000000, %rcx");
        line("cmpq %rcx, %rax");
        emit_jmp("je", done);            // 永生对象（字面量）直接跳过
        line("testq %rax, %rax");
        emit_jmp("jle", slow);           // rc <= 0：让慢路径报错
        line("addq $1, %rax");
        line("movq %rax, -48(%rdi)");
        emit_jmp("jmp", done);
        label(slow);
        spill_fp_slots();
        line("call pa_rc_inc_slow");
        reload_fp_slots();
        label(done);
        return true;
    }
    if (fn == RtFn::RcDec && in.args.size() == 1) {
        emit2("movq", src_op(in.args[0]), "%rdi");
        const std::string slow = fresh_label("decslow");
        const std::string done = fresh_label("decdone");
        line("testq %rdi, %rdi");
        emit_jmp("je", slow);            // p == NULL
        line("cmpl $0, pa_gc_phase_fast(%rip)");
        emit_jmp("jne", slow);           // 收集期只登记边，不能重入释放
        line("movq -48(%rdi), %rax");
        line("movabsq $0x8000000000000000, %rcx");
        line("cmpq %rcx, %rax");
        emit_jmp("je", done);            // 永生对象
        line("cmpq $1, %rax");
        emit_jmp("jle", slow);           // 归零/非法：慢路径真正释放
        line("subq $1, %rax");
        line("movq %rax, -48(%rdi)");
        emit_jmp("jmp", done);
        label(slow);
        spill_fp_slots();
        line("call pa_rc_dec_slow");
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
    for (int i = 0; i < k; ++i) emit_arg(i, false, i, src_op(in.args[i]));
}

void AsmGen::gen_rt_i1(const Inst& in) { gen_rt_args_n(in, 1); }

void AsmGen::gen_rt_i2(const Inst& in) { gen_rt_args_n(in, 2); }

void AsmGen::gen_rt_i3(const Inst& in) { gen_rt_args_n(in, 3); }

void AsmGen::gen_rt_i4(const Inst& in) { gen_rt_args_n(in, 4); }

void AsmGen::gen_rt_i5(const Inst& in) { gen_rt_args_n(in, 5); }

void AsmGen::gen_rt_i6(const Inst& in) { gen_rt_args_n(in, 6); }

void AsmGen::emit_arg(int pos, bool is_fp, int cls_idx, const std::string& v) {
    const TargetABI& abi = target_abi();
    if (abi.positional_args) {
        // Win64：第 pos 个实参固定占第 pos 个槽，与类型无关；超过 4 个落栈。
        if (pos < 4) {
            if (is_fp) emit2("movsd", v, abi.arg_fp[pos]);
            else emit2("movq", v, abi.arg_int[pos]);
            return;
        }
        const int off = out_base() + 8 * (pos - 4);
        if (is_fp) {
            line(std::format("movsd {}, {}(%rsp)", v, off));
        } else if (v.find('(') != std::string::npos) {
            /* 栈传实参的源操作数可能在内存里（mem-to-mem 非法）：借
               Win64 的 caller-saved scratch %r10 中转。实参寄存器只到
               %r9，%r10 不会和已装填的实参冲突。 */
            emit2("movq", v, "%r10");
            emit2("movq", "%r10", std::format("{}(%rsp)", off));
        } else {
            emit2("movq", v, std::format("{}(%rsp)", off));
        }
        return;
    }
    // SysV：整型与浮点各一套独立计数器；该类寄存器用完后落栈。
    if (is_fp) {
        if (cls_idx < abi.n_arg_fp) { emit2("movsd", v, abi.arg_fp[cls_idx]); return; }
        line(std::format("movsd {}, {}(%rsp)", v, out_base() + 8 * cls_idx));
        return;
    }
    if (cls_idx < abi.n_arg_int) { emit2("movq", v, abi.arg_int[cls_idx]); return; }
    emit2("movq", v, std::format("{}(%rsp)", out_base() + 8 * cls_idx));
}

void AsmGen::gen_rt_f1(const Inst& in) { emit_arg(0, true, 0, src_op(in.args[0])); }

void AsmGen::gen_rt_f2(const Inst& in) {
        emit_arg(0, true, 0, src_op(in.args[0]));
        emit_arg(1, true, 1, src_op(in.args[1]));
    }

void AsmGen::gen_rt_f3(const Inst& in) {
        emit_arg(0, true, 0, src_op(in.args[0]));
        emit_arg(1, true, 1, src_op(in.args[1]));
        emit_arg(2, true, 2, src_op(in.args[2]));
    }

void AsmGen::gen_rt_i1f1(const Inst& in) {
        emit_arg(0, false, 0, src_op(in.args[0]));
        emit_arg(1, true, 0, src_op(in.args[1]));
    }

void AsmGen::gen_rt_i1f2(const Inst& in) {
        emit_arg(0, false, 0, src_op(in.args[0]));
        emit_arg(1, true, 0, src_op(in.args[1]));
        emit_arg(2, true, 1, src_op(in.args[2]));
    }

void AsmGen::gen_rt_i1f3(const Inst& in) {
        emit_arg(0, false, 0, src_op(in.args[0]));
        emit_arg(1, true, 0, src_op(in.args[1]));
        emit_arg(2, true, 1, src_op(in.args[2]));
        emit_arg(3, true, 2, src_op(in.args[3]));
    }

void AsmGen::gen_rt_f1i1(const Inst& in) {
        emit_arg(0, true, 0, src_op(in.args[0]));
        emit_arg(1, false, 0, src_op(in.args[1]));
    }

void AsmGen::gen_rt_i4opt5(const Inst& in) {
        emit_arg(0, false, 0, src_op(in.args[0]));
        emit_arg(1, false, 1, src_op(in.args[1]));
        emit_arg(2, false, 2, src_op(in.args[2]));
        emit_arg(3, false, 3, src_op(in.args[3]));
        if (in.args.size() > 4)
            emit_arg(4, false, 4, src_op(in.args[4]));
    }

void AsmGen::gen_rt_reduce_f(const Inst& in) {
        emit_arg(0, false, 0, src_op(in.args[0]));
        emit_arg(1, false, 1, src_op(in.args[1]));
        emit_arg(2, true, 0, src_op(in.args[2]));
        emit_arg(3, false, 2, src_op(in.args[3]));
    }

void AsmGen::gen_rt_subprocess_run(const Inst& in) {
        emit_arg(0, false, 0, src_op(in.args[0]));
        emit_arg(1, false, 1, src_op(in.args[1]));
        emit_arg(2, false, 2, src_op(in.args[2]));
        emit_arg(3, true, 0, src_op(in.args[3]));
        emit_arg(4, false, 3, src_op(in.args[4]));
        emit_arg(5, false, 4, src_op(in.args[5]));
        emit_arg(6, false, 5, src_op(in.args[6]));
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
        case RtFn::RandGNext:
        case RtFn::OsGetcwd:
        case RtFn::OsCpuCount:
        case RtFn::TimeTime:
        case RtFn::TimeMonotonic:
        case RtFn::TimePerfCounter:
        case RtFn::OsGetpid:
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
        case RtFn::StrFromI64:
        case RtFn::StrAny:
        case RtFn::StrFromBool:
        case RtFn::PrintI64:
        case RtFn::PrintAny:
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
        case RtFn::SysExit:
        case RtFn::PrintStr:
        case RtFn::PrintBytes:
        case RtFn::RandNew:
        case RtFn::RandGSeed:
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
            gen_rt_i1(in);
            break;
        case RtFn::ListNew:
        case RtFn::ListSort:
        case RtFn::SortedList:
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
        case RtFn::EhRaise:
        case RtFn::EhRaiseValueFrom:
            gen_rt_i2(in);
            break;
        case RtFn::FileSeek:
        case RtFn::RegexSearch:
        case RtFn::RegexFindAllStr:
        case RtFn::RegexFindIterStr:
        case RtFn::RegexSplit:
        case RtFn::GenSendOpt:
        case RtFn::GenThrowOpt:
        case RtFn::ListSet:
        case RtFn::DictSet:
        case RtFn::ListContains:
        case RtFn::ListEq:
        case RtFn::DictEq:
        case RtFn::ListSortedBy:
        case RtFn::DictBump:
            gen_rt_i4(in);
            break;
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
        case RtFn::IterPermutations:
        case RtFn::IterCombinations:
        case RtFn::IterCombinationsWR:
        case RtFn::CollCounter:
        case RtFn::ExcLeave:
        case RtFn::RandRandint:
        case RtFn::AttrGet:
        case RtFn::AttrSet:
        case RtFn::AttrHas:
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
        case RtFn::ListAppend:   /* (list*, v, heap) */
            gen_rt_i3(in);
            break;
        case RtFn::ClassNew:
        case RtFn::ListPrep:
        case RtFn::ListGet:
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
        case RtFn::DictKeyAt:
        case RtFn::IsInstanceClass:
        case RtFn::StrContains:
        case RtFn::StrLenBytes:
        case RtFn::StrEncode:
        case RtFn::BytesDecode:
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
        case RtFn::FuncReduce:
        case RtFn::StrPercentFormat:
        case RtFn::RegexSearchStr:
        case RtFn::RegexSub:
        case RtFn::RegexSubFn:
        case RtFn::RegexSplitStr:
        case RtFn::ListSlice:
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
            gen_rt_i5(in);
            break;
        case RtFn::JsonLoads:
        case RtFn::RegexSubStr:
        case RtFn::DictFromPairs:
        case RtFn::DateTimeNew:
        case RtFn::PopenNew:
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
            gen_rt_f2(in);
            break;
        case RtFn::MathFma:
            gen_rt_f3(in);
            break;
        case RtFn::ListSumF64:
        case RtFn::RangeSumF64:
        case RtFn::AsyncioWaitFor:
        case RtFn::RandExpovariate:
        case RtFn::PopenWait:
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
    }
}

}  // namespace pyadamas
