// lower_borrow.cpp —— 只读借用槽细化（0.338，borrow1 分支）
//
// 门控 PYADAMAS_BORROW_OPT=0/1，**默认 0**；flag-off 不跑本遍，IR/asm 逐字节
// 不变。设计/安全论证见 docs/borrow-loop-design.md。
//
// 目标形态（R3 取证定位的 RC 密集循环 churn）：
//     for i in range(n):
//         s = xs[0]          # 每轮 inc(index.get) / dec(load.slot s 旧值)
//         total += len(s)    # s 只被读取
// 槽 s 每轮「inc 新值 + dec 旧值」是所有权转移记账，但 s 其实只作借用：
// 容器 xs 持有元素的那份引用贯穿循环。把 s 识别为 borrow 后整段去掉每轮
// inc/dec（以及出口 release），计数守恒：
//   基线：首轮 inc → C+1，随后每轮 inc/dec 净零，出口 dec → C；
//   借用：恒 C（容器那份引用始终在）。对象在两条路径下同样存活，
//   析构时机不变（循环期间计数恒 ≥1，谁都不会降到 0）。
//
// **不需要跨迭代值 identity**：即使每轮借的是不同元素（s = xs[i]），
// 容器对每个元素的引用都贯穿函数体，借用同样守恒。R3 所说的 identity
// 分析是「外提 retain」的前置，不是 borrow 的前置。
//
// 判定（全部满足才改，任何一条不确定 → 整槽跳过，fail-closed）：
//  1. s 是已知堆类型槽（非 unknown、非形参、!slot_unknown、不取址）。
//  2. s 的**每一条** StoreSlot 要么精确匹配 retain 形状
//     [RcInc(v); old=LoadSlot s; StoreSlot s,v; RcDec(old)]（v 经 Mov 链
//     溯源到 IndexLoad/IndexLoadUnchecked —— list/tuple 借用读，运行时不
//     产生新引用；容器溯源到同一根 r），要么是 release_slots 的出口清零
//     （load s; dec; store s,0 三连的第三条）。del s 的 load/store0/dec
//     顺序不同、s=s 的源不是 IndexLoad、s=None 没有 inc —— 都匹配不上 →
//     整槽跳过。形状不符（move_fresh/temp_take、unknown retain……）同跳。
//  3. 根 r 稳定：
//     - PARAM：形参槽，函数体内从不覆盖（出口清零除外）、不取址。
//       实参由调用方持有，调用期间存活。
//     - GLOBAL：全局，本函数内从不写、不取址（模块顶层赋值在入口函数里，
//       入口的全局根因此保守不命中——见设计文档「剩余」）。
//     - FRESHLOCAL：本地槽，唯一一条实写来自新分配（MakeList/MakeTuple/
//       ListNew），分配值只用于该 store 的 temp/take/store，LoadSlot(r)
//       只作 IndexLoad 容器（不逃逸：不传参/不存别处/不返回/不取址/
//       不被方法调用）。新分配对象不可能与任何既有对象别名。
//  4. 全函数扫描：
//     - 生成器/协程/async/vthread/chan 整类跳过（挂起期容器可能被外部改）。
//     - 任何 Class 类型值（含嵌套元素类型）→ 跳过（析构/__str__ 等用户
//       代码时机不可控）。
//     - extern/ptr/GlobalAddr/ObjNew/ClassAddr/Gen* op → 跳过。
//     - 用户调用（Call/CallDirect/VCall*/CallPtr/ImportModule）：
//       PARAM/GLOBAL 根 → 跳过（外部别名可改容器）；FRESHLOCAL 根 → 允许
//       （容器不逃逸，用户代码够不着）。CallExtern 一律跳过。
//     - list 修改类运行时调用（append/set/sort/del/extend/shuffle…）与
//       IndexStore/FieldStore：接收者必须可证不与根别名——
//       PARAM/GLOBAL 根：接收者必须是新分配（或唯一实写为新分配的本地槽）；
//       FRESHLOCAL 根：接收者不得溯源到根本身（其余来源都够不着根）。
//       dict/set/deque/file 的修改不在此列：它们改的是 dict/set/deque/file
//       对象，不可能别名 list/tuple 根；往 dict 里存根/借元素只是 retain，
//       替换/弹出只是 dec，根槽那份引用保证对象存活。
//     - CallRt 白名单：Pure（纯计算/只读容器查询/RC 记账/EH 簿记）恒允许；
//       Dispatch（unknown 域分派/repr/哈希等可能跑用户代码）仅 FRESHLOCAL
//       根允许；其余未知编号 → 跳过（fail-closed，新增 RtFn 默认落 Bad）。
//       defaultdict 工厂（DictSetDefaultFactory）落 Bad → 整函数跳过
//       （DictGet 会调工厂，rt_dict.c）。
//  5. s 的每条 LoadSlot 结果（经 Mov 传播）只被借用安全地消费：
//     纯运算/Len/IndexLoad 容器/分支/Return（own_return_value 先 inc）/
//     存入别处（store_slot/store_global/容器写入都先 retain）/白名单 CallRt/
//     用户调用实参（仅 FRESHLOCAL；被调方 inc_params 自留引用）。
//     出现 RcDec/RcTemp*/SlotAddr(s)/未知消费 → 跳过。
//  6. 出口 release 形状 [LoadSlot s; RcDec; StoreSlot s,0] 必须逐条匹配；
//     任何其它 load+dec 形态 → 跳过（防漏删释放点 → 多 dec → UAF）。
//
// 改写：删 store 形状的 inc/old-load/dec 与出口 load/dec（**保留清零 store**，
// 内联重映射后槽有落点，清零无害且避免陈旧指针），置 slot_owned[s]=false。
// 协同：
//  - 帧释放清单（codegen_block frame_owned_）按 slot_owned 构建 → 异常展开
//    （setjmp longjmp / 零开销 _Unwind）不再 dec 借用槽；容器才是 owner。
//  - GC：借元素经容器根可达（形参根由调用方帧持有；本地根 owned 进帧记录/
//    寄存器扫描；全局根进根表），借用槽本身不被扫描也不影响可达性。
//  - 内联：slot_owned=false 随槽搬进调用方（opt_inline），内联入口对借用槽
//    只清零不释放；根槽的所有权记账原样保留。
//  - 本遍在降级完成后、opt 之前跑（与 finalize_param_borrows 同位置），
//    看到的是原始形状；内联把优化后的身体搬进调用方，收益自动传播。
#include "lower_impl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace pyadamas {
namespace {

// ── 运行时调用分类 ──────────────────────────────────────────────
enum class RtClass { Pure, Dispatch, ListMut, Susp, Bad };

RtClass classify_rt(RtFn f, int& recv) {
    recv = 0;
    switch (f) {
        // ── list 修改类（接收者 args[0]；shuffle 是 args[1]）──
        case RtFn::ListAppend: case RtFn::ListSet: case RtFn::ListSetLen:
        case RtFn::ListPrep: case RtFn::ListInsert: case RtFn::ListReverse:
        case RtFn::ListSort: case RtFn::ListExtend: case RtFn::ListInplaceRepeat:
        case RtFn::ListSliceAssign: case RtFn::ListDelIndex: case RtFn::ListCopyRange:
            return RtClass::ListMut;
        case RtFn::RandShuffle: case RtFn::RandGShuffle:
            recv = 1; return RtClass::ListMut;
        // ── 可挂起 / 并发整类 ──
        case RtFn::GenNext: case RtFn::GenNextOpt: case RtFn::GenSend:
        case RtFn::GenSendOpt: case RtFn::GenThrow: case RtFn::GenThrowOpt:
        case RtFn::GenClose: case RtFn::HandlerSuspend: case RtFn::HandlerRearm:
        case RtFn::HandlerFree:
        case RtFn::AsyncioRun: case RtFn::AsyncioSleep: case RtFn::AsyncioGather:
        case RtFn::AsyncioCreateTask: case RtFn::AsyncioTaskDone:
        case RtFn::AsyncioTaskCancel: case RtFn::AsyncioTaskResult:
        case RtFn::AsyncioTaskException: case RtFn::AsyncioTaskCancelled:
        case RtFn::AsyncioWaitFd: case RtFn::AsyncioWaitFor:
        case RtFn::AsyncGenANext: case RtFn::AsyncGenAsend:
        case RtFn::AsyncGenAClose: case RtFn::AsyncGenAthrow:
        case RtFn::AsyncioFutureNew: case RtFn::FutureSetResult:
        case RtFn::FutureSetException: case RtFn::FutureSetExceptionValue:
        case RtFn::FutureAddCallback: case RtFn::FutureDone: case RtFn::FutureCancel:
        case RtFn::FutureResult: case RtFn::FutureException: case RtFn::FutureCancelled:
        case RtFn::EventNew: case RtFn::EventSet: case RtFn::EventIsSet: case RtFn::EventWait:
        case RtFn::LockNew: case RtFn::LockAcquire: case RtFn::LockRelease: case RtFn::LockLocked:
        case RtFn::QueueNew: case RtFn::QueuePut: case RtFn::QueueGet: case RtFn::QueueQsize:
        case RtFn::QueueEmpty: case RtFn::QueueFull: case RtFn::QueueJoin:
        case RtFn::QueueTaskDone: case RtFn::QueuePutNowait: case RtFn::QueueGetNowait:
        case RtFn::AsyncTimeoutNew: case RtFn::TimeoutEnter: case RtFn::TimeoutExit:
        case RtFn::TimeoutExpired:
        case RtFn::TaskGroupNew: case RtFn::TaskGroupAdd: case RtFn::TaskGroupExit:
        case RtFn::TaskGroupCancel:
        case RtFn::SemNew: case RtFn::SemAcquire: case RtFn::SemRelease: case RtFn::SemLocked:
        case RtFn::VtSpawn: case RtFn::VtJoin: case RtFn::VtSleep:
        case RtFn::ChanNew: case RtFn::ChanSend: case RtFn::ChanRecv:
        case RtFn::ChanSendF: case RtFn::ChanRecvF:
        case RtFn::CpyCall:
            return RtClass::Susp;
        // ── 可能跑用户代码的分派入口（仅 FRESHLOCAL 根允许）──
        case RtFn::StrAny: case RtFn::PrintAny: case RtFn::PrintAnyRepr:
        case RtFn::TruthAny: case RtFn::ReprAny: case RtFn::ReprTuple:
        case RtFn::ReprDictAny: case RtFn::HashValue: case RtFn::StrPercentFormat:
        case RtFn::StrMethod: case RtFn::BytesMethod:
        case RtFn::AnyFromF64: case RtFn::AnyIsFloat: case RtFn::AnyToF64:
        case RtFn::AnyToI64: case RtFn::AnyArith: case RtFn::AnyNeg: case RtFn::AnyCmp:
        case RtFn::AnyArithT: case RtFn::AnyNegT: case RtFn::AnyCmpT:
        case RtFn::AnyIsFloatT: case RtFn::AnyToF64T: case RtFn::AnyToI64T:
        case RtFn::AnyLoadTag: case RtFn::AnyIterList: case RtFn::AnyTypeIs:
        case RtFn::AnyDictItems:
        case RtFn::FuncReduce: case RtFn::FuncReduceF: case RtFn::RegexSubFn:
        case RtFn::JsonDumps: case RtFn::DYLIBLoad: case RtFn::DYLIBClose:
            return RtClass::Dispatch;
        // ── 纯入口：不修改 list/tuple、不跑用户代码 ──
        case RtFn::ListNew: case RtFn::ListGet: case RtFn::ListContains: case RtFn::ListEq:
        case RtFn::ListIndex: case RtFn::ListCount: case RtFn::ListSumI64: case RtFn::ListSumF64:
        case RtFn::ListMinI64: case RtFn::ListMaxI64: case RtFn::ListSlice: case RtFn::ListCopy:
        case RtFn::ListReversed: case RtFn::SortedList: case RtFn::ListSortedBy:
        case RtFn::ListRepeat:
        case RtFn::StrConcat: case RtFn::StrCharAt: case RtFn::StrCharAtCursor:
        case RtFn::StrSlice: case RtFn::StrLenBytes: case RtFn::StrToI64: case RtFn::StrToF64:
        case RtFn::StrFromI64: case RtFn::StrFromF64: case RtFn::StrFromBool: case RtFn::StrFromNone:
        case RtFn::StrRepeat: case RtFn::StrEncode: case RtFn::StrToList: case RtFn::StrCapwords:
        case RtFn::StrWrapList:
        case RtFn::BytesAt: case RtFn::BytesSlice: case RtFn::BytesConcat: case RtFn::BytesRepeat:
        case RtFn::BytesDecode: case RtFn::BytesToList: case RtFn::BytesFromList: case RtFn::BytesZeros:
        case RtFn::Ord: case RtFn::Chr: case RtFn::IntToBase: case RtFn::IntBitLength:
        case RtFn::AbsI64: case RtFn::RoundF64: case RtFn::RoundI64:
        case RtFn::PrintI64: case RtFn::PrintF64: case RtFn::PrintBool: case RtFn::PrintStr:
        case RtFn::PrintBytes: case RtFn::PrintNone: case RtFn::PrintSep: case RtFn::PrintEnd:
        case RtFn::Panic: case RtFn::PanicDiv0: case RtFn::PanicOverflow:
        case RtFn::RcInc: case RtFn::RcDec: case RtFn::RcIncUnknown: case RtFn::RcDecUnknown:
        case RtFn::GcWriteBarrier: case RtFn::RcTemp: case RtFn::RcTempUnknown:
        case RtFn::RcTempTake: case RtFn::RcMark: case RtFn::RcCleanup:
        case RtFn::RcShutdown: case RtFn::GcShutdown:
        case RtFn::ClosureNew:
        case RtFn::DictNew: case RtFn::DictSet: case RtFn::DictGet: case RtFn::DictGetOr:
        case RtFn::DictGetOpt: case RtFn::DictContains: case RtFn::DictKeyAt:
        case RtFn::DictKeys: case RtFn::DictValues: case RtFn::DictItems: case RtFn::DictEq:
        case RtFn::DictCopy: case RtFn::DictFromPairs: case RtFn::DictPop: case RtFn::DictPopItem:
        case RtFn::DictPopLast: case RtFn::DictRemove: case RtFn::DictClear: case RtFn::DictUpdate:
        case RtFn::SetEq: case RtFn::SetFromList: case RtFn::SetUnion: case RtFn::SetIntersect:
        case RtFn::SetDifference: case RtFn::SetSymDiff: case RtFn::SetIntersectUpdate:
        case RtFn::SetDifferenceUpdate: case RtFn::SetSymdiffUpdate:
        case RtFn::IsInstance: case RtFn::IsInstanceClass: case RtFn::IsNamedClass:
        case RtFn::IsException: case RtFn::IsSeq: case RtFn::IsDict: case RtFn::IsStr:
        case RtFn::IsComplex: case RtFn::ObjClassId: case RtFn::ClassName: case RtFn::ClassRepr:
        case RtFn::ClassCtor: case RtFn::ClassMethod: case RtFn::SuperLookup:
        case RtFn::AttrGet: case RtFn::AttrHas: case RtFn::AttrDict: case RtFn::AttrSet:
        case RtFn::ComplexNew: case RtFn::ComplexAdd: case RtFn::ComplexSub: case RtFn::ComplexMul:
        case RtFn::ComplexDiv: case RtFn::ComplexNeg: case RtFn::ComplexAbs: case RtFn::ComplexStr:
        case RtFn::ComplexEq: case RtFn::ComplexTruth:
        case RtFn::FmtI64: case RtFn::FmtBool: case RtFn::FmtF64: case RtFn::FmtStr:
        case RtFn::FmtComplex: case RtFn::ReprStr:
        case RtFn::MathSqrt: case RtFn::MathFloor: case RtFn::MathCeil: case RtFn::MathFabs:
        case RtFn::MathSin: case RtFn::MathCos: case RtFn::MathTan: case RtFn::MathLog:
        case RtFn::MathLog2: case RtFn::MathLog10: case RtFn::MathExp: case RtFn::MathPow:
        case RtFn::MathFmod: case RtFn::MathAtan2: case RtFn::MathHypot: case RtFn::MathDegrees:
        case RtFn::MathRadians: case RtFn::MathTrunc: case RtFn::MathCopysign: case RtFn::MathIsnan:
        case RtFn::MathIsinf: case RtFn::MathIsfinite: case RtFn::MathFactorial: case RtFn::MathGcd:
        case RtFn::MathLcm: case RtFn::MathFsum: case RtFn::HashF64: case RtFn::MathAcos:
        case RtFn::MathAsin: case RtFn::MathAtan: case RtFn::MathAcosh: case RtFn::MathAsinh:
        case RtFn::MathAtanh: case RtFn::MathCbrt: case RtFn::MathCosh: case RtFn::MathSinh:
        case RtFn::MathTanh: case RtFn::MathExp2: case RtFn::MathExpm1: case RtFn::MathLog1p:
        case RtFn::MathErf: case RtFn::MathErfc: case RtFn::MathGamma: case RtFn::MathLgamma:
        case RtFn::MathUlp: case RtFn::MathNextafter: case RtFn::MathRemainder: case RtFn::MathLdexp:
        case RtFn::MathFma: case RtFn::MathIsclose: case RtFn::MathFrexp: case RtFn::MathModf:
        case RtFn::MathIsqrt: case RtFn::MathComb: case RtFn::MathPerm: case RtFn::MathProd:
        case RtFn::MathSumprod: case RtFn::MathDist:
        case RtFn::SysArgv: case RtFn::SysExit: case RtFn::SysStream:
        case RtFn::SysExecutable:
        case RtFn::LangGapsEnable: case RtFn::ExcInfo: case RtFn::TbFrames:
        case RtFn::ReprSet:
        case RtFn::HandlerPush: case RtFn::HandlerLeave: case RtFn::HandlerDiscard:
        case RtFn::ExcMatches: case RtFn::ExcAccept: case RtFn::ExcAcceptValue:
        case RtFn::ExcAcceptInstance: case RtFn::ExcDone: case RtFn::ExcEnter: case RtFn::ExcLeave:
        case RtFn::ExcClear: case RtFn::ExcReraise: case RtFn::ExcTakeValue:
        case RtFn::ExcReraiseValueIf: case RtFn::ExcTypeName: case RtFn::ExcValueTypeName:
        case RtFn::ExcMessage: case RtFn::ExcValueArgs: case RtFn::ExcValueStr:
        case RtFn::ExcValueCause: case RtFn::ExcValueNew: case RtFn::ExcValueNewArgs:
        case RtFn::ExcRegister:
        case RtFn::RaiseInstance: case RtFn::Raise: case RtFn::RaiseArgs: case RtFn::RaiseValue:
        case RtFn::RaiseValueFrom: case RtFn::Reraise:
        case RtFn::EhRaise: case RtFn::EhRaiseValueFrom: case RtFn::EhReraise: case RtFn::EhRethrow:
        case RtFn::EhLand: case RtFn::EhEnter: case RtFn::EhLeave:
        case RtFn::RangeNew: case RtFn::RangeLen: case RtFn::RangeFirst: case RtFn::RangeStep:
        case RtFn::RangeStop: case RtFn::RangeGet: case RtFn::RangeContains: case RtFn::RangeEq:
        case RtFn::RangeSumI64: case RtFn::RangeSumF64: case RtFn::RangeMinI64: case RtFn::RangeMaxI64:
        case RtFn::RangeAny: case RtFn::RangeAll: case RtFn::RangeToList: case RtFn::RangeRepr:
        case RtFn::EnumerateList: case RtFn::Zip2List:
        case RtFn::IterChain: case RtFn::IterRepeat: case RtFn::IterProduct:
        case RtFn::IterPermutations: case RtFn::IterCombinations: case RtFn::IterCombinationsWR:
        case RtFn::IterZipLongest: case RtFn::IterAccumulate:
        case RtFn::CollCounter: case RtFn::CollMostCommon: case RtFn::JsonLoads:
        case RtFn::Sha256Digest: case RtFn::Sha256Hex: case RtFn::B64Encode: case RtFn::B64Decode:
        case RtFn::FileOpen: case RtFn::FileConfig: case RtFn::FileEncoding: case RtFn::FileErrors:
        case RtFn::FileRead: case RtFn::FileReadLine: case RtFn::FileReadLines: case RtFn::FileWrite:
        case RtFn::FileWritelines: case RtFn::FileSeek: case RtFn::FileTell: case RtFn::FileTruncate:
        case RtFn::FileFileno: case RtFn::FileIsatty: case RtFn::FileReadable: case RtFn::FileWritable:
        case RtFn::FileSeekable: case RtFn::FileClosed: case RtFn::FileName: case RtFn::FileMode:
        case RtFn::FileClose: case RtFn::FileFlush: case RtFn::Input:
        case RtFn::RandNew: case RtFn::RandSeed: case RtFn::RandNext: case RtFn::RandRandint:
        case RtFn::RandChoice: case RtFn::RandUniform: case RtFn::RandGSeed: case RtFn::RandGNext:
        case RtFn::RandGRandint: case RtFn::RandGChoice: case RtFn::RandGUniform: case RtFn::RandRange:
        case RtFn::RandGetrandbits: case RtFn::RandGauss: case RtFn::RandExpovariate:
        case RtFn::RandTriangular: case RtFn::RandSample: case RtFn::RandChoices:
        case RtFn::OsMkdir: case RtFn::OsRmdir: case RtFn::OsRemove: case RtFn::OsRename:
        case RtFn::OsGetcwd: case RtFn::OsCpuCount: case RtFn::OsPathJoin: case RtFn::OsPathExists:
        case RtFn::OsPathIsdir: case RtFn::OsPathIsfile: case RtFn::OsGetpid: case RtFn::OsChdir:
        case RtFn::OsChmod: case RtFn::OsAccess: case RtFn::OsMakedirs: case RtFn::OsPathRealpath:
        case RtFn::OsPathGetsize: case RtFn::OsPathGetmtime: case RtFn::OsPathSplit:
        case RtFn::OsPathSamefile: case RtFn::OsClose: case RtFn::OsPathIslink: case RtFn::OsListdir:
        case RtFn::OsGetenv: case RtFn::OsPathBasename: case RtFn::OsPathDirname:
        case RtFn::OsPathSplitext: case RtFn::OsPathNormpath: case RtFn::OsPathAbspath:
        case RtFn::OsPathExpanduser:
        case RtFn::SubprocessRun: case RtFn::SubprocessReturnCode: case RtFn::SubprocessStdout:
        case RtFn::SubprocessStderr: case RtFn::PopenNew: case RtFn::PopenPid:
        case RtFn::PopenReturnCode: case RtFn::PopenStdout: case RtFn::PopenStderr:
        case RtFn::PopenStdin: case RtFn::PopenWait: case RtFn::PopenKill: case RtFn::PopenTerminate:
        case RtFn::PopenSendSignal: case RtFn::PopenCommunicate:
        case RtFn::TimeTime: case RtFn::TimeMonotonic: case RtFn::TimePerfCounter: case RtFn::TimeSleep:
        case RtFn::RegexCompile: case RtFn::RegexSearchStr: case RtFn::RegexSearch:
        case RtFn::RegexFindAllStr: case RtFn::RegexFindAllGroupsStr: case RtFn::RegexFindAll:
        case RtFn::RegexSubStr: case RtFn::RegexSub: case RtFn::RegexSplitStr: case RtFn::RegexSplit:
        case RtFn::RegexEscape: case RtFn::RegexPatternGroups: case RtFn::RegexPatternFlags:
        case RtFn::RegexPatternStr: case RtFn::RegexFindIter: case RtFn::RegexFindIterStr:
        case RtFn::RegexExpand: case RtFn::RegexGroup: case RtFn::RegexGroupStr: case RtFn::RegexGroups:
        case RtFn::RegexGroupDict: case RtFn::RegexLastIndex: case RtFn::RegexLastGroup:
        case RtFn::RegexStart: case RtFn::RegexEnd: case RtFn::RegexSpan:
        case RtFn::DateNew: case RtFn::DateTimeNew: case RtFn::DateTimeNow: case RtFn::DateTimeField:
        case RtFn::DateTimeIsoformat: case RtFn::DateTimeStrftime: case RtFn::TimeDeltaNew:
        case RtFn::TimeDeltaField: case RtFn::TimeDeltaTotalSeconds: case RtFn::TimeDeltaCmp:
        case RtFn::TimeDeltaAdd: case RtFn::TimeDeltaSub: case RtFn::TimeDeltaMul: case RtFn::TimeDeltaNeg:
        case RtFn::TimeDeltaFloorDivInt: case RtFn::TimeDeltaTrueDivInt: case RtFn::TimeDeltaFloorDivTD:
        case RtFn::TimeDeltaTrueDivTD: case RtFn::TimeDeltaStr: case RtFn::DateTimeReplace:
        case RtFn::DateTimeAddDelta: case RtFn::DateTimeDiff: case RtFn::DateTimeCmp:
        case RtFn::PathNew: case RtFn::PathStr: case RtFn::PathJoin: case RtFn::PathName:
        case RtFn::PathStem: case RtFn::PathSuffix: case RtFn::PathParent: case RtFn::PathExists:
        case RtFn::PathIsFile: case RtFn::PathIsDir: case RtFn::PathReadText: case RtFn::PathWriteText:
        case RtFn::PathIterdir: case RtFn::PathGlob: case RtFn::PathMkdir: case RtFn::PathWithSuffix:
        case RtFn::PathParts: case RtFn::PathReadBytes: case RtFn::PathWriteBytes:
        case RtFn::PathIsAbsolute: case RtFn::PathRelativeTo: case RtFn::PathResolve:
        case RtFn::PathAbsolute: case RtFn::PathTouch:
        case RtFn::DequeNew: case RtFn::DequeFromList: case RtFn::DequeToList: case RtFn::DequeLen:
        case RtFn::DequeMaxlen: case RtFn::DequeGet: case RtFn::DequeCount: case RtFn::DequeIndex:
        case RtFn::DequeCopy: case RtFn::DequeAppend: case RtFn::DequeAppendLeft: case RtFn::DequePop:
        case RtFn::DequePopleft: case RtFn::DequeSet: case RtFn::DequeRotate: case RtFn::DequeClear:
        case RtFn::DequeExtend: case RtFn::DequeExtendLeft: case RtFn::DequeRemove:
        case RtFn::DequeInsert: case RtFn::DequeReverse:
        case RtFn::VtShutdown:
            return RtClass::Pure;
        default:
            // DictSetDefaultFactory（defaultdict 工厂：DictGet 会调它）与
            // 未来新增的未知编号一律落这里 → 整函数跳过。
            return RtClass::Bad;
    }
}

bool is_fresh_alloc(const Inst* in) {
    if (!in) return false;
    if (in->op == IrOp::MakeList || in->op == IrOp::MakeTuple || in->op == IrOp::StructNew)
        return true;
    if (in->op == IrOp::CallRt) {
        switch (static_cast<RtFn>(in->imm)) {
            case RtFn::ListNew: case RtFn::DictNew: case RtFn::DequeNew:
            case RtFn::RangeNew: case RtFn::ClosureNew:
                return true;
            default: return false;
        }
    }
    return false;
}

bool is_container_ty(const TypePtr& t) {
    return t && (t->kind == TyKind::List || t->kind == TyKind::Tuple);
}

bool contains_class(const TypePtr& t) {
    if (!t) return false;
    if (t->kind == TyKind::Class) return true;
    for (const auto& sub : t->sub)
        if (contains_class(sub)) return true;
    return false;
}

struct Root {
    enum Kind { Bad, Param, Global, FreshLocal } kind = Bad;
    int idx = -1;
    const Inst* alloc = nullptr;  // FreshLocal：新分配定义指令
};

struct BorrowPass {
    IRFunction& fn;
    const std::vector<IRGlobal>& globals;
    std::unordered_map<VReg, const Inst*> def;
    std::unordered_map<VReg, int> nuses;
    std::unordered_set<VReg> multidef;
    std::unordered_set<int> param_slots;
    std::unordered_set<int> addr_taken;
    bool has_global_addr = false;
    bool has_user_call = false;
    bool has_hard_bad = false;
    bool has_susp_rt = false;
    bool has_dispatch_rt = false;
    bool class_free = true;
    std::vector<VReg> mut_recv;
    std::unordered_map<long long, Root> root_cache;

    BorrowPass(IRFunction& f, const std::vector<IRGlobal>& g) : fn(f), globals(g) {}

    void build() {
        for (auto& p : fn.params)
            if (p.slot >= 0) param_slots.insert(p.slot);
        for (auto& bb : fn.blocks) {
            for (auto& in : bb.insts) {
                if (in.dst != kNoReg) {
                    if (def.count(in.dst)) multidef.insert(in.dst);
                    def[in.dst] = &in;
                }
                auto touch = [&](VReg v) { if (v != kNoReg) ++nuses[v]; };
                touch(in.a); touch(in.b); touch(in.c);
                for (VReg r : in.args) touch(r);
                switch (in.op) {
                    case IrOp::SlotAddr:
                        if (in.idx >= 0) addr_taken.insert(static_cast<int>(in.idx));
                        break;
                    case IrOp::GlobalAddr: has_global_addr = true; break;
                    case IrOp::Call: case IrOp::CallDirect: case IrOp::VCall:
                    case IrOp::VCallStatic: case IrOp::SuperVCall: case IrOp::CallPtr:
                    case IrOp::ImportModule:
                        has_user_call = true; break;
                    case IrOp::CallExtern: case IrOp::LoadExtern: case IrOp::StoreExtern:
                    case IrOp::PtrLoad: case IrOp::PtrStore: case IrOp::ObjNew: case IrOp::ClassAddr:
                    case IrOp::GenNew: case IrOp::GenSetSlot: case IrOp::GenGetState:
                    case IrOp::GenClosing: case IrOp::GenSent: case IrOp::GenYield:
                    case IrOp::GenAwait: case IrOp::GenGetExc: case IrOp::GenTakeExc:
                    case IrOp::GenRethrow: case IrOp::GenDone:
                        has_hard_bad = true; break;
                    case IrOp::IndexStore: case IrOp::IndexStoreUnchecked:
                    case IrOp::FieldStore:
                        mut_recv.push_back(in.a); break;
                    case IrOp::CallRt: {
                        int recv = 0;
                        RtClass c = classify_rt(static_cast<RtFn>(in.imm), recv);
                        if (c == RtClass::Susp) has_susp_rt = true;
                        else if (c == RtClass::Bad) has_hard_bad = true;
                        else if (c == RtClass::Dispatch) has_dispatch_rt = true;
                        else if (c == RtClass::ListMut) {
                            if (recv < static_cast<int>(in.args.size()))
                                mut_recv.push_back(in.args[static_cast<size_t>(recv)]);
                            else has_hard_bad = true;
                        }
                        break;
                    }
                    default: break;
                }
            }
        }
        for (auto& t : fn.slot_ty) if (contains_class(t)) class_free = false;
        for (auto& t : fn.vreg_ty) if (contains_class(t)) class_free = false;
        for (auto& p : fn.params) if (contains_class(p.ty)) class_free = false;
        if (contains_class(fn.ret)) class_free = false;
    }

    // Mov 链溯源；多定义/无定义/链过长 → nullptr（保守）。
    const Inst* trace(VReg v) const {
        for (int depth = 0; v != kNoReg && depth < 32; ++depth) {
            if (multidef.count(v)) return nullptr;
            auto it = def.find(v);
            if (it == def.end()) return nullptr;
            const Inst* in = it->second;
            if (in->op != IrOp::Mov) return in;
            v = in->a;
        }
        return nullptr;
    }

    bool is_zero(const Inst* in) const {
        return in && in->op == IrOp::ConstInt && in->imm == 0;
    }

    // release_slots 出口形状：load.slot s; rc_dec(r); [const.int 0]; store.slot s, 0。
    // 清零常量在第一个被释放的槽处才物化，可能夹在 dec 与 store 之间，也可能
    // 早已定义（共用）——两种都要认。del s 是 load/store0/dec（顺序不同），
    // 不会误匹配。
    bool is_exit_zero_store(const std::vector<Inst>& ins, size_t i, int s) const {
        if (i >= ins.size()) return false;
        if (ins[i].op != IrOp::StoreSlot || ins[i].idx != s || !is_zero(trace(ins[i].a)))
            return false;
        size_t j = i;
        while (j > 0 && ins[j - 1].op == IrOp::ConstInt && ins[j - 1].imm == 0) --j;
        if (j < 2) return false;
        const Inst& dec = ins[j - 1];
        const Inst& ld = ins[j - 2];
        return ld.op == IrOp::LoadSlot && ld.idx == s && ld.dst != kNoReg &&
               dec.op == IrOp::CallRt && static_cast<RtFn>(dec.imm) == RtFn::RcDec &&
               dec.args.size() == 1 && dec.args[0] == ld.dst;
    }

    bool is_exit_load(const std::vector<Inst>& ins, size_t i, int s) const {
        if (i + 1 >= ins.size()) return false;
        if (ins[i].op != IrOp::LoadSlot || ins[i].idx != s || ins[i].dst == kNoReg) return false;
        const Inst& dec = ins[i + 1];
        if (dec.op != IrOp::CallRt || static_cast<RtFn>(dec.imm) != RtFn::RcDec ||
            dec.args.size() != 1 || dec.args[0] != ins[i].dst) return false;
        size_t j = i + 2;
        while (j < ins.size() && ins[j].op == IrOp::ConstInt && ins[j].imm == 0) ++j;
        return j < ins.size() && ins[j].op == IrOp::StoreSlot && ins[j].idx == s &&
               is_zero(trace(ins[j].a));
    }

    // 访问 v 的全部消费点（Mov 传播）。visit 返回 false 即失败。
    bool walk_uses(VReg v, const std::function<bool(const Inst&, VReg)>& visit) const {
        std::vector<VReg> work{v};
        std::unordered_set<VReg> seen;
        while (!work.empty()) {
            VReg x = work.back(); work.pop_back();
            if (!seen.insert(x).second) continue;
            for (auto& bb : fn.blocks) {
                for (auto& in : bb.insts) {
                    bool used = in.a == x || in.b == x || in.c == x;
                    if (!used)
                        for (VReg r : in.args) if (r == x) { used = true; break; }
                    if (!used) continue;
                    if (in.op == IrOp::Mov && in.dst != kNoReg) { work.push_back(in.dst); continue; }
                    if (!visit(in, x)) return false;
                }
            }
        }
        return true;
    }

    Root validate_slot_root(int p, int s) {
        // 根不能是候选槽自身（先于缓存判定：缓存键不含 s）。
        if (p == s) return Root{};
        long long key = (static_cast<long long>(p) << 1) | 0;
        auto it = root_cache.find(key);
        if (it != root_cache.end()) return it->second;
        Root r; r.idx = p;
        root_cache[key] = r;
        if (p < 0 || p >= static_cast<int>(fn.slot_ty.size())) return r;
        if (addr_taken.count(p)) return r;
        if (!is_container_ty(fn.slot_ty[static_cast<size_t>(p)])) return r;
        // 实写 = 非出口清零的 store（del/覆盖写都算实写 → 根不稳定）。
        std::vector<std::pair<int, size_t>> real;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            auto& ins = fn.blocks[b].insts;
            for (size_t i = 0; i < ins.size(); ++i) {
                if (ins[i].op != IrOp::StoreSlot || ins[i].idx != p) continue;
                if (is_exit_zero_store(ins, i, p)) continue;
                real.push_back({static_cast<int>(b), i});
            }
        }
        if (param_slots.count(p)) {
            if (real.empty()) { r.kind = Root::Param; root_cache[key] = r; }
            return r;
        }
        if (real.size() != 1) return r;
        auto [b0, i0] = real[0];
        const Inst& st = fn.blocks[static_cast<size_t>(b0)].insts[i0];
        const Inst* al = trace(st.a);
        if (!al || !(al->op == IrOp::MakeList || al->op == IrOp::MakeTuple ||
                     (al->op == IrOp::CallRt && static_cast<RtFn>(al->imm) == RtFn::ListNew)))
            return r;
        // 分配值只用于 temp/take/存入 p（不逃逸）。
        if (!walk_uses(al->dst, [&](const Inst& in, VReg x) {
                if (in.op == IrOp::StoreSlot && in.idx == p && in.a == x) return true;
                if (in.op == IrOp::CallRt && in.args.size() == 1 && in.args[0] == x) {
                    RtFn f = static_cast<RtFn>(in.imm);
                    return f == RtFn::RcTemp || f == RtFn::RcTempTake || f == RtFn::RcTempUnknown;
                }
                return false;
            }))
            return r;
        // LoadSlot(p) 只作 IndexLoad 容器或 Len 实参（都是纯读，不逃逸；
        // store 旧值装载与出口释放除外）。`xs[i % len(xs)]` 是日常语料常见形态。
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            auto& ins = fn.blocks[b].insts;
            for (size_t i = 0; i < ins.size(); ++i) {
                if (ins[i].op != IrOp::LoadSlot || ins[i].idx != p || ins[i].dst == kNoReg) continue;
                if (i + 1 < ins.size() && ins[i + 1].op == IrOp::StoreSlot && ins[i + 1].idx == p &&
                    i + 2 < ins.size() && ins[i + 2].op == IrOp::CallRt &&
                    static_cast<RtFn>(ins[i + 2].imm) == RtFn::RcDec &&
                    ins[i + 2].args.size() == 1 && ins[i + 2].args[0] == ins[i].dst)
                    continue;  // store 旧值装载
                if (is_exit_load(ins, i, p)) continue;
                if (!walk_uses(ins[i].dst, [&](const Inst& in, VReg x) {
                        if (in.op == IrOp::Len && in.a == x) return true;
                        return (in.op == IrOp::IndexLoad || in.op == IrOp::IndexLoadUnchecked) &&
                               in.a == x;
                    }))
                    return r;
            }
        }
        r.kind = Root::FreshLocal; r.alloc = al;
        root_cache[key] = r;
        return r;
    }

    Root validate_global_root(int g) {
        long long key = (static_cast<long long>(g) << 1) | 1;
        auto it = root_cache.find(key);
        if (it != root_cache.end()) return it->second;
        Root r; r.idx = g;
        root_cache[key] = r;
        if (g < 0 || g >= static_cast<int>(globals.size())) return r;
        if (!is_container_ty(globals[static_cast<size_t>(g)].ty)) return r;
        if (has_global_addr) return r;
        for (auto& bb : fn.blocks)
            for (auto& in : bb.insts)
                if ((in.op == IrOp::StoreGlobal || in.op == IrOp::GlobalAddr) && in.idx == g)
                    return r;
        r.kind = Root::Global;
        root_cache[key] = r;
        return r;
    }

    // 修改类操作的接收者不得与根别名。
    bool receiver_ok(VReg recv, const Root& root) const {
        const Inst* src = trace(recv);
        if (!src) return false;
        if (root.kind == Root::FreshLocal) {
            if (src->op == IrOp::LoadSlot && src->idx == root.idx) return false;
            if (src == root.alloc) return false;
            return true;  // 根不逃逸：其余来源不可能持有根对象
        }
        if (is_fresh_alloc(src)) return true;
        if (src->op == IrOp::LoadSlot && src->idx >= 0 &&
            src->idx < static_cast<int>(fn.slot_ty.size()) && !param_slots.count(src->idx)) {
            int q = static_cast<int>(src->idx);
            if (addr_taken.count(q)) return false;
            int count = 0;
            const Inst* al = nullptr;
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                auto& ins = fn.blocks[b].insts;
                for (size_t i = 0; i < ins.size(); ++i) {
                    if (ins[i].op != IrOp::StoreSlot || ins[i].idx != q) continue;
                    if (is_exit_zero_store(ins, i, q)) continue;
                    ++count;
                    al = trace(ins[i].a);
                }
            }
            if (count == 1 && is_fresh_alloc(al)) return true;
        }
        return false;
    }

    bool use_ok(const Inst& in, VReg x, int s, const Root& root) const {
        switch (in.op) {
            case IrOp::Bin: case IrOp::Un: case IrOp::Cmp: case IrOp::Convert: case IrOp::Len:
                return true;
            case IrOp::IndexLoad: case IrOp::IndexLoadUnchecked: case IrOp::FieldLoad:
            case IrOp::Branch: case IrOp::Return:
                return in.a == x;
            case IrOp::StoreSlot:
                return in.idx != s && in.a == x;
            case IrOp::StoreGlobal:
                return in.a == x;
            case IrOp::IndexStore: case IrOp::IndexStoreUnchecked:
                return in.c == x;
            case IrOp::FieldStore:
                return in.b == x;
            case IrOp::MakeList: case IrOp::MakeTuple:
                return true;
            case IrOp::CallRt: {
                RtFn f = static_cast<RtFn>(in.imm);
                // 释放/临时栈记账消费借用值 → 绝不允许多 dec/误 take。
                if (f == RtFn::RcDec || f == RtFn::RcDecUnknown || f == RtFn::RcTempTake ||
                    f == RtFn::RcTemp || f == RtFn::RcTempUnknown || f == RtFn::RcCleanup)
                    return false;
                int recv = 0;
                RtClass c = classify_rt(f, recv);
                if (c == RtClass::Pure || c == RtClass::ListMut) return true;
                if (c == RtClass::Dispatch) return root.kind == Root::FreshLocal;
                return false;
            }
            case IrOp::Call: case IrOp::CallDirect: case IrOp::VCall:
            case IrOp::VCallStatic: case IrOp::SuperVCall: case IrOp::CallPtr:
                return root.kind == Root::FreshLocal;
            default:
                return false;
        }
    }

    bool analyze_slot(int s, std::vector<std::pair<int, size_t>>& dels) {
        if (s < 0 || s >= static_cast<int>(fn.slot_ty.size())) return false;
        TypePtr ty = fn.slot_ty[static_cast<size_t>(s)];
        if (!ir_is_heap(ty) || ty->kind == TyKind::Unknown) return false;
        if (s < static_cast<int>(fn.slot_unknown.size()) && fn.slot_unknown[static_cast<size_t>(s)])
            return false;
        if (param_slots.count(s) || addr_taken.count(s)) return false;

        Root root;
        bool root_set = false;
        std::unordered_set<long long> store_pos;  // 已验证的 borrow store 位置
        bool any_store = false;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            auto& ins = fn.blocks[b].insts;
            for (size_t i = 0; i < ins.size(); ++i) {
                if (ins[i].op != IrOp::StoreSlot || ins[i].idx != s) continue;
                // 出口清零（release_slots 出口形状的 store）豁免。
                if (is_exit_zero_store(ins, i, s)) continue;
                any_store = true;
                if (i < 2 || i + 1 >= ins.size()) return false;
                const Inst& inc = ins[i - 2];
                const Inst& ld = ins[i - 1];
                const Inst& dec = ins[i + 1];
                VReg v = ins[i].a;
                if (inc.op != IrOp::CallRt || static_cast<RtFn>(inc.imm) != RtFn::RcInc ||
                    inc.args.size() != 1 || inc.args[0] != v) return false;
                if (ld.op != IrOp::LoadSlot || ld.idx != s || ld.dst == kNoReg) return false;
                auto nu = nuses.find(ld.dst);
                if (nu == nuses.end() || nu->second != 1) return false;
                if (dec.op != IrOp::CallRt || static_cast<RtFn>(dec.imm) != RtFn::RcDec ||
                    dec.args.size() != 1 || dec.args[0] != ld.dst) return false;
                const Inst* il = trace(v);
                if (!il || (il->op != IrOp::IndexLoad && il->op != IrOp::IndexLoadUnchecked))
                    return false;
                if (!ir_is_heap(il->ty) || il->ty->kind == TyKind::Unknown) return false;
                const Inst* csrc = trace(il->a);
                if (!csrc) return false;
                Root rr;
                if (csrc->op == IrOp::LoadSlot && csrc->idx >= 0)
                    rr = validate_slot_root(static_cast<int>(csrc->idx), s);
                else if (csrc->op == IrOp::LoadGlobal && csrc->idx >= 0)
                    rr = validate_global_root(static_cast<int>(csrc->idx));
                else return false;
                if (rr.kind == Root::Bad) return false;
                if (!root_set) { root = rr; root_set = true; }
                else if (root.kind != rr.kind || root.idx != rr.idx) return false;
                store_pos.insert((static_cast<long long>(b) << 32) | static_cast<long long>(i));
            }
        }
        if (!any_store || !root_set) return false;
        if (root.kind != Root::FreshLocal && (has_user_call || has_dispatch_rt)) return false;
        for (VReg recv : mut_recv)
            if (!receiver_ok(recv, root)) return false;

        // 装载点分类：store 旧值（豁免）/ 出口释放（删 load+dec）/ 普通读（use 分析）。
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            auto& ins = fn.blocks[b].insts;
            for (size_t i = 0; i < ins.size(); ++i) {
                if (ins[i].op != IrOp::LoadSlot || ins[i].idx != s || ins[i].dst == kNoReg) continue;
                if (i + 1 < ins.size() &&
                    store_pos.count((static_cast<long long>(b) << 32) | static_cast<long long>(i + 1)))
                    continue;  // 已验证 borrow store 的旧值装载
                if (is_exit_load(ins, i, s)) {
                    auto nu = nuses.find(ins[i].dst);
                    if (nu == nuses.end() || nu->second != 1) return false;
                    dels.push_back({static_cast<int>(b), i});
                    dels.push_back({static_cast<int>(b), i + 1});
                    continue;
                }
                if (!walk_uses(ins[i].dst, [&](const Inst& in, VReg x) {
                        return use_ok(in, x, s, root);
                    }))
                    return false;
            }
        }
        // 记录 store 形状的 inc/old-load/dec 删除点。
        for (long long pos : store_pos) {
            int b = static_cast<int>(pos >> 32);
            size_t i = static_cast<size_t>(pos & 0xFFFFFFFFll);
            dels.push_back({b, i - 2});
            dels.push_back({b, i - 1});
            dels.push_back({b, i + 1});
        }
        return true;
    }
};

void borrow_readonly_slots(IRFunction& fn, const std::vector<IRGlobal>& globals) {
    if (fn.is_generator || fn.blocks.empty()) return;
    BorrowPass pass(fn, globals);
    pass.build();
    if (pass.has_susp_rt || pass.has_hard_bad || !pass.class_free) return;
    std::vector<std::pair<int, size_t>> dels;
    std::vector<int> borrowed;
    for (size_t s = 0; s < fn.slot_ty.size(); ++s) {
        std::vector<std::pair<int, size_t>> local;
        if (pass.analyze_slot(static_cast<int>(s), local)) {
            dels.insert(dels.end(), local.begin(), local.end());
            borrowed.push_back(static_cast<int>(s));
        }
    }
    if (borrowed.empty()) return;
    for (int s : borrowed)
        if (static_cast<size_t>(s) < fn.slot_owned.size()) fn.slot_owned[static_cast<size_t>(s)] = false;
    std::sort(dels.begin(), dels.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    dels.erase(std::unique(dels.begin(), dels.end()), dels.end());
    for (auto& [b, i] : dels) {
        auto& ins = fn.blocks[static_cast<size_t>(b)].insts;
        if (i < ins.size()) ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (std::getenv("PYADAMAS_BORROW_OPT_STATS"))
        std::fprintf(stderr, "[borrowopt] %s: %zu slots borrowed, %zu insts removed\n",
                     fn.name.c_str(), borrowed.size(), dels.size());
}

}  // namespace

void Lowerer::finalize_borrow_slots() {
    if (!opts_.borrow_opt) return;
    borrow_readonly_slots(*st_->fn, mod_.globals);
}

}  // namespace pyadamas
