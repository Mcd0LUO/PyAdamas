// ir.h —— 中级表示
//
// 形态：函数内 → 基本块 → 线性三地址码，虚拟寄存器编号。
//
// 为什么不是 SSA：Python 语义里变量天生可变（循环里反复赋值是常态），
// 一上来就构造 phi 会把 for/while/break 的降级搞得非常绕，收益却要等
// 到真正的优化遍才兑现。现在用「可变槽位 + 虚拟寄存器」：值有编号，
// 变量有固定槽位，后端直接按槽分配栈帧，语义简单且不会错。
// 后续要加 SSA 是在这层之上再做一次构造，不必回头改前端。
//
// 为什么类型挂在指令上：前端已经推完了每个表达式的类型，后端选指令
// 宽度（i64 / f64 / 指针）全靠它。丢掉类型再让后端猜是这类项目里最
// 常见的返工原因。
#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "typecheck.h"

namespace py23 {

using VReg = uint32_t;
inline constexpr VReg kNoReg = 0xFFFFFFFFu;

// 二元运算。与 Tok 分开：Tok 是词法记号，可能包含用不到的组合；
// IR 只保留真正有语义的算子，后端 switch 才不用写 default 兜底。
enum class BinKind : uint8_t {
    Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
    BitAnd, BitOr, BitXor, Shl, Shr,
};

enum class UnKind : uint8_t { Neg, Pos, BitNot, Not };

enum class CmpKind : uint8_t { Lt, Le, Gt, Ge, Eq, Ne };

inline const char* bin_name(BinKind k) {
    switch (k) {
        case BinKind::Add: return "add";
        case BinKind::Sub: return "sub";
        case BinKind::Mul: return "mul";
        case BinKind::Div: return "div";
        case BinKind::FloorDiv: return "floordiv";
        case BinKind::Mod: return "mod";
        case BinKind::Pow: return "pow";
        case BinKind::BitAnd: return "and";
        case BinKind::BitOr: return "or";
        case BinKind::BitXor: return "xor";
        case BinKind::Shl: return "shl";
        case BinKind::Shr: return "shr";
    }
    return "?";
}

inline const char* un_name(UnKind k) {
    switch (k) {
        case UnKind::Neg: return "neg";
        case UnKind::Pos: return "pos";
        case UnKind::BitNot: return "bitnot";
        case UnKind::Not: return "not";
    }
    return "?";
}

inline const char* cmp_name(CmpKind k) {
    switch (k) {
        case CmpKind::Lt: return "lt";
        case CmpKind::Le: return "le";
        case CmpKind::Gt: return "gt";
        case CmpKind::Ge: return "ge";
        case CmpKind::Eq: return "eq";
        case CmpKind::Ne: return "ne";
    }
    return "?";
}

// 运行时库函数。名字固定，后端按枚举直接生成调用，
// 不经过符号查找，也不允许用户代码直接引用。
enum class RtFn : uint16_t {
    ListNew,     // (i64 cap) -> list*
    ListAppend,  // (list*, i64) -> void
    ListGet,     // (list*, i64) -> i64
    ListSet,     // (list*, i64, i64) -> void
    ListSetLen,  // (list*, i64) -> void
    StrConcat,   // (str*, str*) -> str*
    StrCharAt,   // (str*, i64) -> str*
    StrFromI64,  // (i64) -> str*
    StrFromF64,  // (f64) -> str*
    StrFromBool, // (i64) -> str*
    StrFromNone, // () -> str*
    StrToI64,    // (str*, i64 base) -> i64  解析失败抛 ValueError
    StrToF64,    // (str*) -> f64
    Ord,         // (str*) -> i64
    Chr,         // (i64) -> str*
    IntToBase,   // (i64 v, i64 base) -> str*  hex/bin/oct 的格式
    RoundF64,    // (f64 x, i64 ndigits) -> f64
    RoundI64,    // (i64 v, i64 ndigits) -> i64
    StrMethod,   // (str* self, i64 op, i64 a0, i64 a1, i64 a2) -> i64
    FileOpen,    // (str* path|0, i64 fd, str* mode|0, i64 buffering, i64 newline, i64 closefd) -> file*
    FileConfig,  // (file*, str* enc|0, str* errors|0) -> void
    FileEncoding,// (file*) -> str*
    FileErrors,  // (file*) -> str*
    FileRead,    // (file*, i64 n) -> str*   n < 0 读全部
    FileReadLine,// (file*, i64 size) -> str*  size < 0 读一行；EOF 返回空串
    FileReadLines,// (file*, i64 hint) -> list*
    FileWrite,   // (file*, str*) -> i64
    FileWritelines, // (file*, list*) -> void
    FileSeek,    // (file*, i64 off, i64 whence) -> i64
    FileTell,    // (file*) -> i64
    FileTruncate,// (file*, i64 size) -> i64
    FileFileno,  // (file*) -> i64
    FileIsatty,  // (file*) -> i64
    FileReadable,// (file*) -> i64
    FileWritable,// (file*) -> i64
    FileSeekable,// (file*) -> i64
    FileClosed,  // (file*) -> i64
    FileName,    // (file*) -> str*
    FileMode,    // (file*) -> str*
    FileClose,   // (file*) -> void
    FileFlush,   // (file*) -> void
    Input,       // (str* prompt|0) -> str*
    SysStream,   // (i64 which 0/1/2) -> file*  sys.stdin/stdout/stderr
    PrintI64,    // (i64) -> void
    PrintF64,    // (f64) -> void
    PrintBool,   // (i64) -> void
    PrintStr,    // (str*) -> void
    PrintNone,   // () -> void
    PrintSep,    // () -> void  打印一个空格
    PrintEnd,    // () -> void  打印换行
    Panic,       // (str*) -> noreturn
    PanicDiv0,   // () -> noreturn  整数除以零
    PanicOverflow, // () -> noreturn  INT64_MIN / -1
    RcInc,       // (ptr) -> void
    RcDec,       // (ptr) -> void
    RcIncUnknown,// (i64) -> i64  按堆注册表保守 retain（unknown 值）
    RcTemp,      // (ptr) -> void  表达式临时值入栈，接管产生者的引用
    RcMark,      // () -> i64      临时值栈水位
    RcCleanup,   // (i64) -> void  释放到指定水位
    RcShutdown,  // () -> void     程序退出，释放临时栈自身的存储
    GcShutdown,  // () -> void     程序退出，释放所有剩余堆对象（含环）
    ClosureNew,  // (i64 code, i64 n_caps) -> closure*
    DictNew,      // (i64 key_kind, i64 key_own, i64 val_own) -> dict*
    DictSet,      // (dict*, i64 key, i64 val) -> void
    DictGet,      // (dict*, i64 key) -> i64
    DictContains, // (dict*, i64 key) -> i64
    DictKeyAt,    // (dict*, i64 index) -> i64
    ListContains, // (list*, i64 value, i64 cmp_kind) -> i64
    StrContains,  // (str*, str*) -> i64
    DictGetOr,    // (dict*, i64 key, i64 default) -> i64
    DictKeys,     // (dict*) -> list*
    DictValues,   // (dict*) -> list*
    DictItems,    // (dict*) -> list*
    DictUpdate,   // (dict*, dict*) -> void
    DictCopy,     // (dict*) -> dict*  浅拷贝（匹配 **rest）
    IsInstance,   // (obj*, i64 class_id) -> i64
    IsNamedClass, // (obj*, str* qualname) -> i64  跨模块类模式按限定名匹配
    SuperLookup,  // (obj*, i64 after_cid, i64 slot, str* name) -> fnptr
    IsException,  // (obj*, str* type) -> i64
    IsSeq,        // (i64) -> i64  运行期值是不是 list/tuple
    IsDict,       // (i64) -> i64  运行期值是不是 dict/set 载体
    StrEqUnknown, // (i64, str*) -> i64  unknown 值上的字符串模式
    ComplexEqUnknown, // (i64, c*) -> i64  unknown 值上的复数模式
    IsStr,        // (i64) -> i64  unknown 值是不是 str
    IsComplex,    // (i64) -> i64  unknown 值是不是复数对象
    ListSlice,    // (list*, i64 start, i64 stop, i64 step) -> list*
    StrSlice,     // (str*, i64 start, i64 stop, i64 step) -> str*
    DictPop,      // (dict*, i64 key, i64 has_default, i64 def) -> i64
    DictPopItem,  // (dict*) -> (i64, i64)
    DictPopLast,  // (dict*) -> i64
    DictRemove,   // (dict*, i64 key, i64 missing_ok) -> void
    DictClear,    // (dict*) -> void
    ListSliceAssign, // (list*, i64 start, i64 stop, i64 step, list*) -> void
    ListDelIndex, // (list*, i64 index) -> void  del list[i]
    StrToList,    // (str*) -> list*
    ListExtend,   // (list* dst, list* src) -> void
    ListSumI64,   // (list*, i64 start) -> i64
    ListSumF64,   // (list*, f64 start) -> f64
    ListMinI64,   // (list*) -> i64  空表抛 ValueError
    ListMaxI64,   // (list*) -> i64
    ListCopy,     // (list*) -> list*
    ListReversed, // (list*) -> list*  倒序副本
    ListSort,     // (list*, i64 kind) -> void  原地排序
    SortedList,   // (list*, i64 kind) -> list*  排序副本
    ListIndex,    // (list*, i64 v, i64 kind) -> i64  找不到抛 ValueError
    ListCount,    // (list*, i64 v, i64 kind) -> i64
    ListInsert,   // (list*, i64 idx, i64 v) -> void
    ListReverse,  // (list*) -> void  原地反转
    IntBitLength, // (i64) -> i64
    EnumerateList,// (list*) -> list*  元素是 [i, v] 对
    Zip2List,     // (list*, list*) -> list*
    SetFromList,  // (list*, i64 key_kind, i64 key_own) -> dict*
    RangeNew,     // (i64 start, i64 stop, i64 step) -> list*
    HandlerPush,  // () -> handler*   压入异常 handler，记录临时栈水位
    HandlerLeave, // (handler*) -> void 弹出并清理临时栈
    HandlerDiscard, // (handler*) -> void 只弹出，不清理临时值
    ExcMatches,   // (str* type) -> i64
    ExcAccept,    // () -> str*       把 pending 转为 current，返回消息
    ExcAcceptValue, // () -> excval*   进入 except 体，返回异常对象
    ExcDone,      // () -> void
    ExcClear,     // () -> void
    ExcReraise,   // () -> void       noreturn
    ExcTypeName,  // () -> str*
    ExcMessage,   // () -> str*
    ExcValueArgs, // (excval*) -> tuple*
    ExcValueStr,  // (excval*) -> str*
    ExcValueCause, // (excval*) -> excval*|0
    ExcValueNew,  // (str* type, str* msg|0) -> excval*
    ExcRegister,  // (str* child, str* parent) -> void  自定义异常类注册
    Raise,        // (str* type, str* msg) -> void  noreturn
    RaiseValue,   // (excval*) -> void noreturn
    RaiseValueFrom, // (excval*, excval*|0) -> void noreturn
    Reraise,      // () -> void       noreturn  裸 raise
    AbsI64,       // (i64) -> i64
    ComplexNew,   // (f64 re, f64 im) -> complex*
    ComplexAdd, ComplexSub, ComplexMul, ComplexDiv, // (c*,c*) -> c*
    ComplexNeg,   // (c*) -> c*
    ComplexAbs,   // (c*) -> f64
    ComplexStr,   // (c*) -> str*
    ComplexEq,    // (c*,c*) -> bool
    ComplexTruth, // (c*) -> bool
    FmtI64,       // (i64, str* spec) -> str*
    FmtBool,      // (i64, str* spec) -> str*
    FmtF64,       // (f64, str* spec) -> str*
    FmtStr,       // (str*, str* spec) -> str*
    FmtComplex,   // (c*, str* spec) -> str*
    ReprStr,      // (str*) -> str*
    MathSqrt, MathFloor, MathCeil, MathFabs, MathSin, MathCos, MathTan,
    MathLog, MathLog2, MathLog10, MathExp,
    MathPow, MathFmod,
    MathAtan2, MathHypot, MathDegrees, MathRadians, MathTrunc, MathCopysign,
    MathIsnan, MathIsinf, MathIsfinite, MathFactorial, MathGcd, MathLcm, MathFsum,
    HashValue, HashF64, MathAcos, MathAsin, MathAtan, MathAcosh, MathAsinh, MathAtanh, MathCbrt, MathCosh, MathSinh, MathTanh, MathExp2, MathExpm1, MathLog1p, MathErf, MathErfc, MathGamma, MathLgamma, MathUlp, MathNextafter, MathRemainder, MathLdexp, MathFma, MathIsclose, MathFrexp, MathModf, MathIsqrt, MathComb, MathPerm, MathProd, MathSumprod, MathDist,
    SysArgv,      // () -> list*
    SysExit,      // (i64 code) -> noreturn
    GenNext,      // (gen*) -> i64  取下一个值；结束抛 StopIteration
    GenNextOpt,   // (gen*, i64* ok) -> i64  结束时不抛，写 ok=0
    GenSend,      // (gen*, i64 sent) -> i64
    GenSendOpt,   // (gen*, i64 sent, i64* ok) -> i64  yield from 用
    GenClose,     // (gen*) -> void  跑跨挂起点的清理并结束
    HandlerSuspend, // (handler*) -> void  yield 时把记录临时摘下
    HandlerRearm,   // (handler*) -> void  恢复时重新压栈并记录水位
    HandlerFree,    // (handler*) -> void  生成器 close：释放挂起的记录
    AsyncioRun,     // (coro*) -> i64   跑事件循环直到根协程结束
    AsyncioSleep,   // (f64) -> awaitable*
    AsyncioGather,  // (list*) -> awaitable*
    AsyncioCreateTask, // (coro*) -> task*
    AsyncioTaskDone,   // (task*) -> bool
    AsyncioTaskCancel, // (task*) -> bool
    AsyncGenANext,     // (asyncgen*) -> awaitable*  __anext__ 的驱动对象
    AsyncGenAsend,     // (asyncgen*, i64 sent) -> awaitable*  asend(v)
    AsyncGenAClose,    // (asyncgen*) -> awaitable*  aclose()
    AsyncGenAthrow,    // (asyncgen*, excval*) -> awaitable*  athrow(exc)
    AsyncioFutureNew,  // () -> future*
    FutureSetResult,   // (future*, i64) -> void
    FutureSetException,// (future*, str*) -> void（失败消息）
    FutureSetExceptionValue, // (future*, excval*) -> void（失败异常对象）
    FutureAddCallback, // (future*, fn) -> void  完成回调，参数是 future
    FutureDone,        // (future*) -> bool
    FutureCancel,      // (future*) -> bool
    FutureResult,      // (future*) -> i64（未完成抛异常）
    FutureException,   // (future*) -> str*|None
    FutureCancelled,   // (future*) -> bool
    AsyncioTaskResult, // (task*) -> i64
    AsyncioTaskException, // (task*) -> str*|None
    AsyncioTaskCancelled, // (task*) -> bool
    AsyncioWaitFd,     // (i64 fd, i64 write) -> awaitable*
    AsyncioWaitFor,    // (awaitable*, f64 secs) -> awaitable*
    EventNew,          // () -> event*
    EventSet,          // (event*) -> void
    EventIsSet,        // (event*) -> bool
    EventWait,         // (event*) -> future*
    LockNew,           // () -> lock*
    LockAcquire,       // (lock*) -> future*
    LockRelease,       // (lock*) -> void
    LockLocked,        // (lock*) -> bool
    QueueNew,          // () -> queue*
    QueuePut,          // (queue*, i64) -> void
    QueueGet,          // (queue*) -> future*
    QueueQsize,        // (queue*) -> int
    QueueEmpty,        // (queue*) -> bool
    QueueFull,         // (queue*) -> bool
    QueueJoin,         // (queue*) -> future*
    QueueTaskDone,     // (queue*) -> void
    QueuePutNowait,    // (queue*, i64) -> void；满队列抛 QueueFull
    QueueGetNowait,    // (queue*) -> i64；空队列抛 QueueEmpty
    AsyncTimeoutNew,   // (f64 secs) -> timeout*
    TimeoutEnter,      // (timeout*) -> void
    TimeoutExit,       // (timeout*) -> int64 已超时?
    TimeoutExpired,    // (timeout*) -> bool
    TaskGroupNew,      // () -> taskgroup*
    TaskGroupAdd,      // (taskgroup*, coro*) -> task*
    TaskGroupExit,     // (taskgroup*) -> future*
    TaskGroupCancel,   // (taskgroup*) -> void
    SemNew,            // (i64 value) -> semaphore*
    SemAcquire,        // (semaphore*) -> future*
    SemRelease,        // (semaphore*) -> void
    SemLocked,         // (semaphore*) -> bool
    RandNew,           // (i64 seed) -> random*    INT64_MIN 表示时间种子
    RandSeed,          // (random*, i64 seed) -> void
    RandNext,          // (random*) -> f64
    RandRandint,       // (random*, i64 a, i64 b) -> i64
    RandChoice,        // (random*, list*) -> i64（借用）
    RandUniform,       // (random*, f64 a, f64 b) -> f64
    RandShuffle,       // (random*, list*) -> void
    RandGSeed,         // (i64 seed) -> void
    RandGNext,         // () -> f64
    RandGRandint,      // (i64 a, i64 b) -> i64
    RandGChoice,       // (list*) -> i64（借用）
    RandGUniform,      // (f64 a, f64 b) -> f64
    RandGShuffle,      // (list*) -> void
    OsMkdir,           // (str* path) -> void
    OsRmdir,           // (str* path) -> void
    OsRemove,          // (str* path) -> void
    OsRename,          // (str* from, str* to) -> void
    OsGetcwd,          // () -> str*
    OsCpuCount,        // () -> i64
    OsPathJoin,        // (list* parts) -> str*
    OsPathExists,      // (str* path) -> i64
    OsPathIsdir,       // (str* path) -> i64
    OsPathIsfile,      // (str* path) -> i64
    SubprocessRun,     // (list* argv, i64 capture, i64 text, f64 timeout, i64 check) -> proc*
    SubprocessReturnCode,  // (proc*) -> i64
    SubprocessStdout,      // (proc*) -> str*
    SubprocessStderr,      // (proc*) -> str*
    TimeTime,              // () -> f64
    TimeMonotonic,         // () -> f64
    TimePerfCounter,       // () -> f64
    TimeSleep,             // (f64 secs) -> void
    OsListdir,             // (str* path) -> list*
    OsGetenv,              // (str* name, str* default) -> str*
    OsPathBasename,        // (str*) -> str*
    OsPathDirname,         // (str*) -> str*
    OsPathSplitext,        // (str*) -> tuple[str, str]
    OsPathNormpath,        // (str*) -> str*
    OsPathAbspath,         // (str*) -> str*
    OsPathExpanduser,      // (str*) -> str*
    IterChain,             // (list* a, list* b, i64 elem_kind) -> list*
    IterRepeat,            // (i64 v, i64 n, i64 elem_kind) -> list*
    CollCounter,           // (list* items, i64 key_kind, i64 key_own) -> dict*
    CollMostCommon,        // (dict*, i64 n) -> list*
    JsonDumps,             // (i64 value, str* desc) -> str*
    JsonLoads,             // (str* text, str* desc) -> i64
    RegexCompile,          // (str* pat, i64 flags) -> regex*
    RegexSearchStr,        // (str* pat, str* text, i64 flags, i64 mode) -> match*
    RegexSearch,           // (regex*, str* text, i64 mode) -> match*
    RegexFindAllStr,       // (str* pat, str* text, i64 flags) -> list*
    RegexFindAll,          // (regex*, str* text) -> list*
    RegexSubStr,           // (str* pat, str* repl, str* text, i64 count, i64 flags) -> str*
    RegexSub,              // (regex*, str* repl, str* text, i64 count) -> str*
    RegexSplitStr,         // (str* pat, str* text, i64 maxsplit, i64 flags) -> list*
    RegexSplit,            // (regex*, str* text, i64 maxsplit) -> list*
    RegexEscape,           // (str*) -> str*
    RegexGroup,            // (match*, i64 n) -> str*
    RegexGroupStr,         // (match*, str* name) -> str*
    RegexGroups,           // (match*) -> list*
    RegexStart,            // (match*, i64 n) -> i64
    RegexEnd,              // (match*, i64 n) -> i64
    RegexSpan,             // (match*, i64 n) -> tuple[i64, i64]
};

inline const char* rt_name(RtFn f) {
    switch (f) {
        case RtFn::ListNew: return "py23_list_new";
        case RtFn::ListAppend: return "py23_list_append";
        case RtFn::ListGet: return "py23_list_get";
        case RtFn::ListSet: return "py23_list_set";
        case RtFn::ListSetLen: return "py23_list_set_len";
        case RtFn::StrConcat: return "py23_str_concat";
        case RtFn::StrCharAt: return "py23_str_char_at";
        case RtFn::StrFromI64: return "py23_str_from_i64";
        case RtFn::StrFromF64: return "py23_str_from_f64";
        case RtFn::StrFromBool: return "py23_str_from_bool";
        case RtFn::StrFromNone: return "py23_str_from_none";
        case RtFn::StrToI64: return "py23_str_to_i64";
        case RtFn::StrToF64: return "py23_str_to_f64";
        case RtFn::Ord: return "py23_ord";
        case RtFn::Chr: return "py23_chr";
        case RtFn::IntToBase: return "py23_int_to_base";
        case RtFn::RoundF64: return "py23_round_f64";
        case RtFn::RoundI64: return "py23_round_i64";
        case RtFn::StrMethod: return "py23_str_method";
        case RtFn::FileOpen: return "py23_file_open";
        case RtFn::FileConfig: return "py23_file_config";
        case RtFn::FileEncoding: return "py23_file_encoding";
        case RtFn::FileErrors: return "py23_file_errors";
        case RtFn::FileRead: return "py23_file_read";
        case RtFn::FileReadLine: return "py23_file_readline";
        case RtFn::FileReadLines: return "py23_file_readlines";
        case RtFn::FileWrite: return "py23_file_write";
        case RtFn::FileWritelines: return "py23_file_writelines";
        case RtFn::FileSeek: return "py23_file_seek";
        case RtFn::FileTell: return "py23_file_tell";
        case RtFn::FileTruncate: return "py23_file_truncate";
        case RtFn::FileFileno: return "py23_file_fileno";
        case RtFn::FileIsatty: return "py23_file_isatty";
        case RtFn::FileReadable: return "py23_file_readable";
        case RtFn::FileWritable: return "py23_file_writable";
        case RtFn::FileSeekable: return "py23_file_seekable";
        case RtFn::FileClosed: return "py23_file_closed";
        case RtFn::FileName: return "py23_file_name";
        case RtFn::FileMode: return "py23_file_mode";
        case RtFn::FileClose: return "py23_file_close";
        case RtFn::FileFlush: return "py23_file_flush";
        case RtFn::Input: return "py23_input";
        case RtFn::SysStream: return "py23_sys_stream";
        case RtFn::PrintI64: return "py23_print_i64";
        case RtFn::PrintF64: return "py23_print_f64";
        case RtFn::PrintBool: return "py23_print_bool";
        case RtFn::PrintStr: return "py23_print_str";
        case RtFn::PrintNone: return "py23_print_none";
        case RtFn::PrintSep: return "py23_print_sep";
        case RtFn::PrintEnd: return "py23_print_end";
        case RtFn::Panic: return "py23_panic";
        case RtFn::PanicDiv0: return "py23_panic_div0";
        case RtFn::PanicOverflow: return "py23_panic_overflow";
        case RtFn::RcInc: return "py23_rc_inc";
        case RtFn::RcDec: return "py23_rc_dec";
        case RtFn::RcIncUnknown: return "py23_rc_inc_unknown";
        case RtFn::RcTemp: return "py23_rc_temp";
        case RtFn::RcMark: return "py23_rc_mark";
        case RtFn::RcCleanup: return "py23_rc_cleanup";
        case RtFn::RcShutdown: return "py23_rc_shutdown";
        case RtFn::GcShutdown: return "py23_gc_shutdown";
        case RtFn::ClosureNew: return "py23_closure_new";
        case RtFn::DictNew: return "py23_dict_new";
        case RtFn::DictSet: return "py23_dict_set";
        case RtFn::DictGet: return "py23_dict_get";
        case RtFn::DictContains: return "py23_dict_contains";
        case RtFn::DictKeyAt: return "py23_dict_key_at";
        case RtFn::ListContains: return "py23_list_contains";
        case RtFn::StrContains: return "py23_str_contains";
        case RtFn::DictGetOr: return "py23_dict_get_or";
        case RtFn::DictKeys: return "py23_dict_keys";
        case RtFn::DictValues: return "py23_dict_values";
        case RtFn::DictItems: return "py23_dict_items";
        case RtFn::DictUpdate: return "py23_dict_update";
        case RtFn::DictCopy: return "py23_dict_copy";
        case RtFn::IsInstance: return "py23_isinstance";
        case RtFn::IsNamedClass: return "py23_is_named_class";
        case RtFn::SuperLookup: return "py23_super_lookup";
        case RtFn::IsException: return "py23_is_exc";
        case RtFn::IsSeq: return "py23_is_seq";
        case RtFn::IsDict: return "py23_is_dict";
        case RtFn::StrEqUnknown: return "py23_str_eq_unknown";
        case RtFn::ComplexEqUnknown: return "py23_complex_eq_unknown";
        case RtFn::IsStr: return "py23_is_str";
        case RtFn::IsComplex: return "py23_is_complex";
        case RtFn::ListSlice: return "py23_list_slice";
        case RtFn::StrSlice: return "py23_str_slice";
        case RtFn::DictPop: return "py23_dict_pop";
        case RtFn::DictPopItem: return "py23_dict_popitem";
        case RtFn::DictPopLast: return "py23_dict_pop_last";
        case RtFn::DictRemove: return "py23_dict_remove";
        case RtFn::DictClear: return "py23_dict_clear";
        case RtFn::ListSliceAssign: return "py23_list_slice_assign";
        case RtFn::ListDelIndex: return "py23_list_del_index";
        case RtFn::StrToList: return "py23_str_to_list";
        case RtFn::ListExtend: return "py23_list_extend";
        case RtFn::ListSumI64: return "py23_list_sum_i64";
        case RtFn::ListSumF64: return "py23_list_sum_f64";
        case RtFn::ListMinI64: return "py23_list_min_i64";
        case RtFn::ListMaxI64: return "py23_list_max_i64";
        case RtFn::ListCopy: return "py23_list_copy";
        case RtFn::ListReversed: return "py23_list_reversed";
        case RtFn::ListSort: return "py23_list_sort";
        case RtFn::SortedList: return "py23_sorted_list";
        case RtFn::ListIndex: return "py23_list_index";
        case RtFn::ListCount: return "py23_list_count";
        case RtFn::ListInsert: return "py23_list_insert";
        case RtFn::ListReverse: return "py23_list_reverse";
        case RtFn::IntBitLength: return "py23_int_bit_length";
        case RtFn::EnumerateList: return "py23_enumerate_list";
        case RtFn::Zip2List: return "py23_zip2_list";
        case RtFn::SetFromList: return "py23_set_from_list";
        case RtFn::RangeNew: return "py23_range_new";
        case RtFn::HandlerPush: return "py23_handler_push";
        case RtFn::HandlerLeave: return "py23_handler_leave";
        case RtFn::HandlerDiscard: return "py23_handler_discard";
        case RtFn::ExcMatches: return "py23_exc_matches";
        case RtFn::ExcAccept: return "py23_exc_accept";
        case RtFn::ExcAcceptValue: return "py23_exc_accept_value";
        case RtFn::ExcDone: return "py23_exc_done";
        case RtFn::ExcClear: return "py23_exc_clear";
        case RtFn::ExcReraise: return "py23_exc_reraise";
        case RtFn::ExcTypeName: return "py23_exc_type_name";
        case RtFn::ExcMessage: return "py23_exc_message";
        case RtFn::ExcValueArgs: return "py23_exc_value_args";
        case RtFn::ExcValueStr: return "py23_exc_value_str";
        case RtFn::ExcValueCause: return "py23_exc_value_cause";
        case RtFn::ExcValueNew: return "py23_exc_value_new";
        case RtFn::ExcRegister: return "py23_exc_register";
        case RtFn::Raise: return "py23_raise";
        case RtFn::RaiseValue: return "py23_raise_value";
        case RtFn::RaiseValueFrom: return "py23_raise_value_from";
        case RtFn::Reraise: return "py23_reraise";
        case RtFn::MathSqrt: return "py23_math_sqrt";
        case RtFn::MathFloor: return "py23_math_floor";
        case RtFn::MathCeil: return "py23_math_ceil";
        case RtFn::AbsI64: return "py23_abs_i64";
        case RtFn::ComplexNew: return "py23_complex_new";
        case RtFn::ComplexAdd: return "py23_complex_add";
        case RtFn::ComplexSub: return "py23_complex_sub";
        case RtFn::ComplexMul: return "py23_complex_mul";
        case RtFn::ComplexDiv: return "py23_complex_div";
        case RtFn::ComplexNeg: return "py23_complex_neg";
        case RtFn::ComplexAbs: return "py23_complex_abs";
        case RtFn::ComplexStr: return "py23_complex_str";
        case RtFn::ComplexEq: return "py23_complex_eq";
        case RtFn::ComplexTruth: return "py23_complex_truth";
        case RtFn::FmtI64: return "py23_fmt_i64";
        case RtFn::FmtBool: return "py23_fmt_bool";
        case RtFn::FmtF64: return "py23_fmt_f64";
        case RtFn::FmtStr: return "py23_fmt_str";
        case RtFn::FmtComplex: return "py23_fmt_complex";
        case RtFn::ReprStr: return "py23_repr_str";
        case RtFn::MathFabs: return "py23_math_fabs";
        case RtFn::MathSin: return "py23_math_sin";
        case RtFn::MathCos: return "py23_math_cos";
        case RtFn::MathTan: return "py23_math_tan";
        case RtFn::MathLog: return "py23_math_log";
        case RtFn::MathLog2: return "py23_math_log2";
        case RtFn::MathLog10: return "py23_math_log10";
        case RtFn::MathExp: return "py23_math_exp";
        case RtFn::MathPow: return "py23_math_pow";
        case RtFn::MathFmod: return "py23_math_fmod";
        case RtFn::MathAtan2: return "py23_math_atan2";
        case RtFn::MathHypot: return "py23_math_hypot";
        case RtFn::MathDegrees: return "py23_math_degrees";
        case RtFn::MathRadians: return "py23_math_radians";
        case RtFn::MathTrunc: return "py23_math_trunc";
        case RtFn::MathCopysign: return "py23_math_copysign";
        case RtFn::MathIsnan: return "py23_math_isnan";
        case RtFn::MathIsinf: return "py23_math_isinf";
        case RtFn::MathIsfinite: return "py23_math_isfinite";
        case RtFn::MathFactorial: return "py23_math_factorial";
        case RtFn::MathGcd: return "py23_math_gcd";
        case RtFn::MathLcm: return "py23_math_lcm";
        case RtFn::MathFsum: return "py23_math_fsum";
        case RtFn::HashValue: return "py23_hash_value";
        case RtFn::HashF64: return "py23_hash_f64";
        case RtFn::MathAcos: return "py23_math_acos";
        case RtFn::MathAsin: return "py23_math_asin";
        case RtFn::MathAtan: return "py23_math_atan";
        case RtFn::MathAcosh: return "py23_math_acosh";
        case RtFn::MathAsinh: return "py23_math_asinh";
        case RtFn::MathAtanh: return "py23_math_atanh";
        case RtFn::MathCbrt: return "py23_math_cbrt";
        case RtFn::MathCosh: return "py23_math_cosh";
        case RtFn::MathSinh: return "py23_math_sinh";
        case RtFn::MathTanh: return "py23_math_tanh";
        case RtFn::MathExp2: return "py23_math_exp2";
        case RtFn::MathExpm1: return "py23_math_expm1";
        case RtFn::MathLog1p: return "py23_math_log1p";
        case RtFn::MathErf: return "py23_math_erf";
        case RtFn::MathErfc: return "py23_math_erfc";
        case RtFn::MathGamma: return "py23_math_gamma";
        case RtFn::MathLgamma: return "py23_math_lgamma";
        case RtFn::MathUlp: return "py23_math_ulp";
        case RtFn::MathNextafter: return "py23_math_nextafter";
        case RtFn::MathRemainder: return "py23_math_remainder";
        case RtFn::MathLdexp: return "py23_math_ldexp";
        case RtFn::MathFma: return "py23_math_fma";
        case RtFn::MathIsclose: return "py23_math_isclose";
        case RtFn::MathFrexp: return "py23_math_frexp";
        case RtFn::MathModf: return "py23_math_modf";
        case RtFn::MathIsqrt: return "py23_math_isqrt";
        case RtFn::MathComb: return "py23_math_comb";
        case RtFn::MathPerm: return "py23_math_perm";
        case RtFn::MathProd: return "py23_math_prod";
        case RtFn::MathSumprod: return "py23_math_sumprod";
        case RtFn::MathDist: return "py23_math_dist";
        case RtFn::SysArgv: return "py23_sys_argv";
        case RtFn::SysExit: return "py23_sys_exit";
        case RtFn::GenNext: return "py23_gen_next";
        case RtFn::GenNextOpt: return "py23_gen_next_opt";
        case RtFn::GenSend: return "py23_gen_send";
        case RtFn::GenSendOpt: return "py23_gen_send_opt";
        case RtFn::GenClose: return "py23_gen_close";
        case RtFn::HandlerSuspend: return "py23_handler_suspend";
        case RtFn::HandlerRearm: return "py23_handler_rearm";
        case RtFn::HandlerFree: return "py23_handler_free";
        case RtFn::AsyncioRun: return "py23_asyncio_run";
        case RtFn::AsyncioSleep: return "py23_asyncio_sleep";
        case RtFn::AsyncioGather: return "py23_asyncio_gather";
        case RtFn::AsyncioCreateTask: return "py23_asyncio_create_task";
        case RtFn::AsyncioTaskDone: return "py23_asyncio_task_done";
        case RtFn::AsyncioTaskCancel: return "py23_asyncio_task_cancel";
        case RtFn::AsyncGenANext: return "py23_asyncgen_anext";
        case RtFn::AsyncGenAsend: return "py23_asyncgen_asend";
        case RtFn::AsyncGenAClose: return "py23_asyncgen_aclose";
        case RtFn::AsyncGenAthrow: return "py23_asyncgen_athrow";
        case RtFn::AsyncioFutureNew: return "py23_asyncio_future_new";
        case RtFn::FutureSetResult: return "py23_future_set_result";
        case RtFn::FutureSetException: return "py23_future_set_exception";
        case RtFn::FutureSetExceptionValue: return "py23_future_set_exception_value";
        case RtFn::FutureAddCallback: return "py23_future_add_callback";
        case RtFn::FutureDone: return "py23_future_done";
        case RtFn::FutureCancel: return "py23_future_cancel";
        case RtFn::FutureResult: return "py23_future_result";
        case RtFn::FutureException: return "py23_future_exception";
        case RtFn::FutureCancelled: return "py23_future_cancelled";
        case RtFn::AsyncioTaskResult: return "py23_asyncio_task_result";
        case RtFn::AsyncioTaskException: return "py23_asyncio_task_exception";
        case RtFn::AsyncioTaskCancelled: return "py23_asyncio_task_cancelled";
        case RtFn::AsyncioWaitFd: return "py23_asyncio_wait_fd";
        case RtFn::AsyncioWaitFor: return "py23_asyncio_wait_for";
        case RtFn::EventNew: return "py23_event_new";
        case RtFn::EventSet: return "py23_event_set";
        case RtFn::EventIsSet: return "py23_event_is_set";
        case RtFn::EventWait: return "py23_event_wait";
        case RtFn::LockNew: return "py23_lock_new";
        case RtFn::LockAcquire: return "py23_lock_acquire";
        case RtFn::LockRelease: return "py23_lock_release";
        case RtFn::LockLocked: return "py23_lock_locked";
        case RtFn::QueueNew: return "py23_queue_new";
        case RtFn::QueuePut: return "py23_queue_put";
        case RtFn::QueueGet: return "py23_queue_get";
        case RtFn::QueueQsize: return "py23_queue_qsize";
        case RtFn::QueueEmpty: return "py23_queue_empty";
        case RtFn::QueueFull: return "py23_queue_full";
        case RtFn::QueueJoin: return "py23_queue_join";
        case RtFn::QueueTaskDone: return "py23_queue_task_done";
        case RtFn::QueuePutNowait: return "py23_queue_put_nowait";
        case RtFn::QueueGetNowait: return "py23_queue_get_nowait";
        case RtFn::AsyncTimeoutNew: return "py23_timeout_new";
        case RtFn::TimeoutEnter: return "py23_timeout_enter";
        case RtFn::TimeoutExit: return "py23_timeout_exit";
        case RtFn::TimeoutExpired: return "py23_timeout_expired";
        case RtFn::TaskGroupNew: return "py23_taskgroup_new";
        case RtFn::TaskGroupAdd: return "py23_taskgroup_add";
        case RtFn::TaskGroupExit: return "py23_taskgroup_exit";
        case RtFn::TaskGroupCancel: return "py23_taskgroup_cancel";
        case RtFn::SemNew: return "py23_sem_new";
        case RtFn::SemAcquire: return "py23_sem_acquire";
        case RtFn::SemRelease: return "py23_sem_release";
        case RtFn::SemLocked: return "py23_sem_locked";
        case RtFn::RandNew: return "py23_random_new";
        case RtFn::RandSeed: return "py23_random_seed";
        case RtFn::RandNext: return "py23_random_next";
        case RtFn::RandRandint: return "py23_random_randint";
        case RtFn::RandChoice: return "py23_random_choice";
        case RtFn::RandUniform: return "py23_random_uniform";
        case RtFn::RandShuffle: return "py23_random_shuffle";
        case RtFn::RandGSeed: return "py23_random_gseed";
        case RtFn::RandGNext: return "py23_random_gnext";
        case RtFn::RandGRandint: return "py23_random_grandint";
        case RtFn::RandGChoice: return "py23_random_gchoice";
        case RtFn::RandGUniform: return "py23_random_guniform";
        case RtFn::RandGShuffle: return "py23_random_gshuffle";
        case RtFn::OsMkdir: return "py23_os_mkdir";
        case RtFn::OsRmdir: return "py23_os_rmdir";
        case RtFn::OsRemove: return "py23_os_remove";
        case RtFn::OsRename: return "py23_os_rename";
        case RtFn::OsGetcwd: return "py23_os_getcwd";
        case RtFn::OsCpuCount: return "py23_os_cpu_count";
        case RtFn::OsPathJoin: return "py23_os_path_join";
        case RtFn::OsPathExists: return "py23_os_path_exists";
        case RtFn::OsPathIsdir: return "py23_os_path_isdir";
        case RtFn::OsPathIsfile: return "py23_os_path_isfile";
        case RtFn::SubprocessRun: return "py23_subprocess_run";
        case RtFn::SubprocessReturnCode: return "py23_subprocess_returncode";
        case RtFn::SubprocessStdout: return "py23_subprocess_stdout";
        case RtFn::SubprocessStderr: return "py23_subprocess_stderr";
        case RtFn::TimeTime: return "py23_time_time";
        case RtFn::TimeMonotonic: return "py23_time_monotonic";
        case RtFn::TimePerfCounter: return "py23_time_perf_counter";
        case RtFn::TimeSleep: return "py23_time_sleep";
        case RtFn::OsListdir: return "py23_os_listdir";
        case RtFn::OsGetenv: return "py23_os_getenv";
        case RtFn::OsPathBasename: return "py23_os_path_basename";
        case RtFn::OsPathDirname: return "py23_os_path_dirname";
        case RtFn::OsPathSplitext: return "py23_os_path_splitext";
        case RtFn::OsPathNormpath: return "py23_os_path_normpath";
        case RtFn::OsPathAbspath: return "py23_os_path_abspath";
        case RtFn::OsPathExpanduser: return "py23_os_path_expanduser";
        case RtFn::IterChain: return "py23_itertools_chain";
        case RtFn::IterRepeat: return "py23_itertools_repeat";
        case RtFn::CollCounter: return "py23_collections_counter";
        case RtFn::CollMostCommon: return "py23_collections_most_common";
        case RtFn::JsonDumps: return "py23_json_dumps";
        case RtFn::JsonLoads: return "py23_json_loads";
        case RtFn::RegexCompile: return "py23_re_compile";
        case RtFn::RegexSearchStr: return "py23_re_search_str";
        case RtFn::RegexSearch: return "py23_re_search";
        case RtFn::RegexFindAllStr: return "py23_re_findall_str";
        case RtFn::RegexFindAll: return "py23_re_findall";
        case RtFn::RegexSubStr: return "py23_re_sub_str";
        case RtFn::RegexSub: return "py23_re_sub";
        case RtFn::RegexSplitStr: return "py23_re_split_str";
        case RtFn::RegexSplit: return "py23_re_split";
        case RtFn::RegexEscape: return "py23_re_escape";
        case RtFn::RegexGroup: return "py23_re_group";
        case RtFn::RegexGroupStr: return "py23_re_group_str";
        case RtFn::RegexGroups: return "py23_re_groups";
        case RtFn::RegexStart: return "py23_re_start";
        case RtFn::RegexEnd: return "py23_re_end";
        case RtFn::RegexSpan: return "py23_re_span";
    }
    return "?";
}

// 字符串方法的操作码，序号必须与 runtime/rt_obj.c 的 py23_str_method 一致。
enum class StrOp : int64_t {
    Upper, Lower, Strip, LStrip, RStrip, StripChars, LStripChars, RStripChars,
    SplitWs, SplitSep, Join, Replace, Find, RFind, StartsWith, EndsWith, Count,
    ZFill, LJust, RJust, Center, Capitalize, Title, IsDigit, IsAlpha, IsSpace,
    IsUpper, IsLower, IsAlphaNum, RemovePrefix, RemoveSuffix, SplitLines,
};

enum class IrOp : uint8_t {
    // ── 常量 ──────────────────────────────────────────────
    ConstInt,    // dst = imm
    ConstFloat,  // dst = fimm
    ConstStr,    // dst = strings[idx]
    ConstNone,   // dst = None

    // ── 值 ────────────────────────────────────────────────
    FuncAddr,  // dst = &static_closure[imm]（无条件/无捕获函数值）
    FuncCode,  // dst = &funcs[imm]（裸代码地址，只给 ClosureNew 用）
    Mov,       // dst = a

    // ── 运算 ──────────────────────────────────────────────
    Bin,  // dst = a <bop> b
    Un,   // dst = <uop> a
    Cmp,  // dst = a <cop> b   （结果是 bool）
    Convert,  // dst = a 转成 ty（int/float/bool/str）

    // ── 变量 ──────────────────────────────────────────────
    LoadSlot,    // dst = slots[idx]
    StoreSlot,   // slots[idx] = a
    LoadGlobal,  // dst = globals[idx]
    StoreGlobal, // globals[idx] = a

    // ── 容器 ──────────────────────────────────────────────
    MakeList,   // dst = <ty>[](args...)
    MakeTuple,  // dst = (args...)
    IndexLoad,  // dst = a[b]
    IndexLoadUnchecked, // dst = a[b]，调用方已证明 0 <= b < len(a)，不检查
    IndexStore, // a[b] = c
    Len,        // dst = len(a)

    // ── 对象 ──────────────────────────────────────────────
    ObjNew,     // dst = new <class imm>[n_fields]，头部写 vtable 指针
    FieldLoad,  // dst = a->field[imm]（imm 是字节偏移）
    FieldStore, // a->field[imm] = b
    VCall,      // dst = a->vtable[imm](a, args...)  第一个实参固定是 self
    VCallStatic,// dst = a->vtable[imm](args...)  静态方法不传 self
    SuperVCall, // dst = super_lookup(a, idx, imm)(a, args...)  沿运行期 MRO
    CallDirect, // dst = funcs[idx](args...)  直接调用裸函数（静态方法）

    // ── 调用 ──────────────────────────────────────────────
    Call,    // dst = a(args...)（a 是函数指针）
    CallRt,  // dst = <RtFn(imm)>(args...)
    Setjmp,  // dst = setjmp(a)（a 是 handler 记录，异常跳回时返回非 0）
    ImportModule, // imm 是 imports 下标：确保模块已初始化
    SlotAddr,     // dst = &slots[idx]（栈槽或生成器帧槽的地址）

    // ── 生成器 ────────────────────────────────────────────────
    GenNew,     // dst = 新生成器帧；imm 是 generators 下标
    GenSetSlot, // 创建期把参数写进帧槽；imm 是槽号，args = {gen, value}
    GenGetState,// dst = 帧 state
    GenClosing, // dst = 帧 closing 标志（close/finalize 期间非 0）
    GenSent,    // dst = 恢复时传入的值
    GenYield,   // 把 a 写进 out、state=imm，然后返回（终止指令）
    GenAwait,   // 把 a 写进 out、state=imm、status=3，然后返回（终止指令）
    GenGetExc,  // dst = 帧里待注入的异常指针
    GenRethrow, // 在 resume 帧内抛出注入的异常（终止指令）
    GenDone,    // status=1；a 非空时先把返回值写进 out（终止指令）

    // ── 控制流 ────────────────────────────────────────────
    Jump,    // -> target
    Branch,  // a ? target : target2
    Return,  // return a（a == kNoReg 表示 None）
};

inline const char* irop_name(IrOp op) {
    switch (op) {
        case IrOp::ConstInt: return "const.int";
        case IrOp::ConstFloat: return "const.float";
        case IrOp::ConstStr: return "const.str";
        case IrOp::ConstNone: return "const.none";
        case IrOp::FuncAddr: return "func.addr";
        case IrOp::FuncCode: return "func.code";
        case IrOp::Mov: return "mov";
        case IrOp::Bin: return "bin";
        case IrOp::Un: return "un";
        case IrOp::Cmp: return "cmp";
        case IrOp::Convert: return "convert";
        case IrOp::LoadSlot: return "load.slot";
        case IrOp::StoreSlot: return "store.slot";
        case IrOp::LoadGlobal: return "load.global";
        case IrOp::StoreGlobal: return "store.global";
        case IrOp::MakeList: return "list.new";
        case IrOp::MakeTuple: return "tuple.new";
        case IrOp::IndexLoad: return "index.get";
        case IrOp::IndexLoadUnchecked: return "index.get.unchecked";
        case IrOp::IndexStore: return "index.set";
        case IrOp::Len: return "len";
        case IrOp::ObjNew: return "obj.new";
        case IrOp::FieldLoad: return "field.get";
        case IrOp::FieldStore: return "field.set";
        case IrOp::VCall: return "vcall";
        case IrOp::VCallStatic: return "vcall.static";
        case IrOp::SuperVCall: return "vcall.super";
        case IrOp::CallDirect: return "call.direct";
        case IrOp::Call: return "call";
        case IrOp::CallRt: return "call.rt";
        case IrOp::Setjmp: return "setjmp";
        case IrOp::ImportModule: return "import.module";
        case IrOp::SlotAddr: return "slot.addr";
        case IrOp::GenNew: return "gen.new";
        case IrOp::GenSetSlot: return "gen.set";
        case IrOp::GenGetState: return "gen.state";
        case IrOp::GenClosing: return "gen.closing";
        case IrOp::GenSent: return "gen.sent";
        case IrOp::GenYield: return "gen.yield";
        case IrOp::GenAwait: return "gen.await";
        case IrOp::GenGetExc: return "gen.exc";
        case IrOp::GenRethrow: return "gen.rethrow";
        case IrOp::GenDone: return "gen.done";
        case IrOp::Jump: return "jump";
        case IrOp::Branch: return "branch";
        case IrOp::Return: return "return";
    }
    return "?";
}

struct Inst {
    IrOp op = IrOp::ConstNone;
    BinKind bop = BinKind::Add;
    UnKind uop = UnKind::Neg;
    CmpKind cop = CmpKind::Eq;

    VReg dst = kNoReg;
    VReg a = kNoReg, b = kNoReg, c = kNoReg;

    int64_t imm = 0;
    double fimm = 0.0;
    int32_t idx = -1;     // 槽号 / 全局号 / 字符串号 / 函数号
    int32_t target = -1;  // 跳转目标块
    int32_t target2 = -1;

    TypePtr ty;  // 结果类型；无结果时为 nullptr

    std::vector<VReg> args;  // MakeList/MakeTuple/Call 的操作数

    std::string text() const;
};

struct Block {
    std::string name;
    std::vector<Inst> insts;

    bool terminated() const {
        return !insts.empty() && (insts.back().op == IrOp::Jump ||
                                  insts.back().op == IrOp::Branch ||
                                  insts.back().op == IrOp::Return ||
                                  insts.back().op == IrOp::GenYield ||
                                  insts.back().op == IrOp::GenAwait ||
                                  insts.back().op == IrOp::GenRethrow ||
                                  insts.back().op == IrOp::GenDone);
    }
};

struct IRParam {
    std::string name;
    TypePtr ty;
    int slot = -1;
};

// 一个全局变量。数据段里就是一个 .quad，初始为 0 —— 0 正好也是 None
// 的机器表示，所以「尚未赋值」天然就是 None。
struct IRGlobal {
    std::string name;
    TypePtr ty;
    // 模块编译时的链接符号；为空表示本地 .LgN。
    std::string symbol;
    // 外部模块的全局：只引用符号，不在这里分配存储，也不进根表。
    bool external = false;
};

struct IRFunction {
    std::string name;
    std::vector<IRParam> params;
    TypePtr ret;
    // 局部槽位（含形参），下标即槽号。参数槽排在最前面，后端按这个
    // 顺序把寄存器 / 栈上的实参搬进来。
    std::vector<TypePtr> slot_ty;
    // 每个虚拟寄存器的类型，后端决定用 rax 还是 xmm 时查它
    std::vector<TypePtr> vreg_ty;
    std::vector<Block> blocks;
    // 有捕获的函数：入口把 %r10 里的闭包指针存进这个槽。
    int env_slot = -1;
    // 含 setjmp 的函数关闭槽位寄存器提升：longjmp 会把 callee-saved
    // 寄存器恢复成 setjmp 时刻的值，寄存器里的变量会回退。
    bool has_handlers = false;
    // O2 内联候选：无捕获、无 handler、非生成器/异步、签名固定。
    bool can_inline = false;
    // 生成器 resume：槽位不在栈上，而在堆帧里（%r15 指向帧负载）。
    // 入口按帧里的 state 分发到续跑块，参数已由创建包装写进帧。
    bool is_generator = false;

    int new_slot(TypePtr ty) {
        slot_ty.push_back(std::move(ty));
        return static_cast<int>(slot_ty.size()) - 1;
    }
};

// 一个类的运行时形态。布局在降级阶段就完全确定：
//   vtable 是静态表，每个槽一个函数地址（未填的槽是 -1，正常不会出现）；
//   n_fields 含继承来的字段，对象大小 = 8 * (1 + n_fields)。
struct IRClass {
    std::string name;
    std::vector<int> vtable;  // 槽 → 函数下标
    int n_fields = 0;
    // 按字段偏移顺序（父类在前），1 表示该字段是堆类型，析构时要 dec。
    std::vector<uint8_t> field_heap;
    // 运行期类元数据：模块限定类名、C3 MRO（类下标，self 在前）和
    // 本类自己声明的方法槽。isinstance / super() 都靠这张表。
    std::string qualname;
    std::vector<int> mro;
    std::vector<int> own_slots;
};

// 一个生成器的运行时形态。帧负载布局与运行时 rt.c 一致：
//   [state][status][sent][out][slot0..slotN-1]
// resume 是状态机函数；slot_heap 决定帧销毁时要 dec 哪些槽。
struct IRGenerator {
    int resume = -1;
    std::vector<uint8_t> slot_heap;
    int n_slots = 0;
    // 0 生成器 / 1 协程 / 2 异步生成器
    int frame_kind = 0;
};

struct IRModule {
    // 用 unique_ptr 持有：降级过程中会不断新增函数（嵌套 def），
    // vector<IRFunction> 扩容会让外面持有的指针失效。
    std::vector<std::unique_ptr<IRFunction>> funcs;
    std::vector<IRGlobal> globals;
    std::vector<std::string> strings;
    std::vector<IRClass> classes;
    std::vector<IRGenerator> generators;
    // 模块编译：符号前缀、运行期 __name__、源路径、是否入口。
    std::string module_symbol;
    std::string runtime_name = "__main__";
    std::string source_path;
    bool is_entry = true;
    // 生成代码优化等级（0/1/2），由 LowerOptions 传入 codegen。
    int opt_level = 2;
    // import.module 指令引用的模块名（按出现顺序去重）。
    std::vector<std::string> imports;
    int entry = -1;  // 模块顶层代码编译成的函数下标

    IRFunction* add_func(std::string name) {
        auto f = std::make_unique<IRFunction>();
        f->name = std::move(name);
        funcs.push_back(std::move(f));
        return funcs.back().get();
    }

    int intern_string(const std::string& s) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return static_cast<int>(i);
        strings.push_back(s);
        return static_cast<int>(strings.size()) - 1;
    }
};

// ── 类型到机器类的映射 ────────────────────────────────────────────

inline bool ir_is_float(const TypePtr& t) { return t && t->kind == TyKind::Float; }

// 引用计数管理的堆类型。unknown 故意不算：它的机器表示可能是整数，
// 按指针 dec 会直接段错误。宁可泄漏，不可误杀。
inline bool ir_is_heap(const TypePtr& t) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::Str:
        case TyKind::List:
        case TyKind::Tuple:
        case TyKind::Class:
        case TyKind::Func:  // 静态闭包永生，堆闭包引用计数
        case TyKind::Dict:
        case TyKind::Set:
        case TyKind::Generator:
        case TyKind::Coroutine:
        case TyKind::Task:
        case TyKind::AsyncGenerator:
        case TyKind::Future:
        case TyKind::AsyncEvent:
        case TyKind::AsyncLock:
        case TyKind::AsyncQueue:
        case TyKind::TypeObj:
        case TyKind::AsyncTimeout:
        case TyKind::AsyncTaskGroup:
        case TyKind::AsyncSemaphore:
        case TyKind::Exception:  // 异常值：堆对象，args 列表归它所有
        case TyKind::Complex:
        case TyKind::File:
        case TyKind::Random:
        case TyKind::CompletedProcess:
        case TyKind::Regex:
        case TyKind::Match:
            return true;
        case TyKind::Optional:
            return !t->sub.empty() && ir_is_heap(t->sub[0]);
        default:
            return false;
    }
}

// 指针类：需要按地址传递 / 取字段的类型
inline bool ir_is_ref(const TypePtr& t) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::Str:
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Dict:
        case TyKind::Tuple:
        case TyKind::Func:
        case TyKind::BoundMethod:
        case TyKind::Class:
        case TyKind::Generator:
        case TyKind::Coroutine:
        case TyKind::Task:
        case TyKind::AsyncGenerator:
        case TyKind::Future:
        case TyKind::AsyncEvent:
        case TyKind::AsyncLock:
        case TyKind::AsyncQueue:
        case TyKind::TypeObj:
        case TyKind::AsyncTimeout:
        case TyKind::AsyncTaskGroup:
        case TyKind::AsyncSemaphore:
        case TyKind::Exception:
        case TyKind::Complex:
        case TyKind::File:
        case TyKind::Random:
        case TyKind::CompletedProcess:
        case TyKind::Regex:
        case TyKind::Match:
            return true;
        default:
            return false;
    }
}

inline bool ir_is_int_like(const TypePtr& t) {
    if (!t) return true;
    switch (t->kind) {
        case TyKind::Bool:
        case TyKind::Int:
        case TyKind::None:
        case TyKind::Optional:
            return true;
        default:
            return false;
    }
}

void dump_ir(const IRModule& m, std::ostream& os);

// 程序是否需要「存活堆索引」：unknown 值的保守 retain 靠它判断一个机器字
// 是不是堆指针。没有任何 unknown 类型、没有编译器发出的 RcIncUnknown、
// 也不涉及 asyncio/生成器运行时的程序用不到索引，可以在入口关掉维护，
// 省掉每个堆对象分配/释放时的哈希表插入和删除。
bool ir_type_contains_unknown(const TypePtr& t);
bool ir_module_needs_unknown_index(const IRModule& m);

}  // namespace py23
