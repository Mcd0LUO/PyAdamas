// ir_ops.h —— IR 算子与运行期函数编号
//
// 从 ir.h 拆出：这一半是纯枚举与名字表，数据结构在 ir_types.h。
#pragma once

#include <cstdint>

namespace pyadamas {

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
    ListAppend,  // (list*, i64 v, i64 heap) -> void  heap: 0 非堆 / 1 堆 / 2 unknown
    ListGet,     // (list*, i64) -> i64
    ListSet,     // (list*, i64, i64) -> void
    ListSetLen,  // (list*, i64) -> void
    ListPrep,    // (list*, i64 v, i64 vk, i64 owned) -> void  空列表首次写入定型
    StrConcat,   // (str*, str*) -> str*
    StrCharAt,   // (str*, i64) -> str*
    BytesAt,     // (bytes*, i64) -> i64
    StrFromI64,  // (i64) -> str*
    StrAny,      // (i64 可能是堆字符串) -> str*  运行期查注册表分派
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
    PrintI64,
    PrintAny,    // (i64) -> void
    PrintF64,    // (f64) -> void
    PrintBool,   // (i64) -> void
    PrintStr,
    PrintBytes,    // (str*) -> void
    PrintNone,   // () -> void
    PrintSep,    // () -> void  打印一个空格
    PrintEnd,    // () -> void  打印换行
    Panic,       // (str*) -> noreturn
    PanicDiv0,   // () -> noreturn  整数除以零
    PanicOverflow, // () -> noreturn  INT64_MIN / -1
    RcInc,       // (ptr) -> void
    RcDec,       // (ptr) -> void
    RcIncUnknown,// (i64) -> i64  按堆注册表保守 retain（unknown 值）
    RcDecUnknown,// (i64) -> void 按堆注册表保守 release（unknown 值）
    RcTemp,      // (ptr) -> void  表达式临时值入栈，接管产生者的引用
    RcTempUnknown, // (i64) -> void unknown 值入临时栈（先按注册表确认是堆对象）
    RcTempTake,  // (ptr) -> void  把临时栈上的新值所有权转给调用方
    RcMark,      // () -> i64      临时值栈水位
    RcCleanup,   // (i64) -> void  释放到指定水位
    RcShutdown,  // () -> void     程序退出，释放临时栈自身的存储
    GcShutdown,  // () -> void     程序退出，释放所有剩余堆对象（含环）
    ClosureNew,  // (i64 code, i64 n_caps) -> closure*
    DictNew,      // (i64 key_kind, i64 key_own, i64 val_own) -> dict*
    DictSet,      // (dict*, i64 key, i64 val) -> void
    DictGet,      // (dict*, i64 key) -> i64
    DictSetDefaultFactory, // (dict*, i64 factory) -> void
    DictContains, // (dict*, i64 key) -> i64
    DictKeyAt,    // (dict*, i64 index) -> i64
    ListContains, // (list*, i64 value, i64 cmp_kind) -> i64
    ListEq,       // (list*, list*, i64 cmp_kind) -> i64
    StrContains,  // (str*, str*) -> i64
    DictGetOr,    // (dict*, i64 key, i64 default) -> i64
    DictBump,     // (dict*, i64 key, i64 def, i64 delta) -> void  融合 get+add+set
    DictTryGet,   // (dict*, i64 key, i64* out) -> i64 found；idx=值槽（融合 contains+get）
    DictGetOpt,   // (dict*, i64 key) -> i64  缺失时返回 None
    DictKeys,     // (dict*) -> list*
    DictValues,   // (dict*) -> list*
    DictItems,    // (dict*) -> list*
    DictEq,       // (dict*, dict*, i64 val_kind) -> i64
    SetEq,        // (dict*, dict*) -> i64
    DictUpdate,   // (dict*, dict*) -> void
    DictCopy,     // (dict*) -> dict*  浅拷贝（匹配 **rest）
    IsInstance,   // (obj*, i64 class_id) -> i64
    ClassNew,     // (i64 cid, i64 ctor, i64* vtable, str* name) -> class*
    ClassCtor,    // (class*) -> fn*
    ClassMethod,  // (class*, i64 slot) -> fnptr
    ClassName,    // (class*) -> str*
    ClassRepr,    // (class*) -> str*
    IsInstanceClass, // (obj*, class*) -> i64
    ObjClassId,   // (obj*) -> i64
    IsNamedClass, // (obj*, str* qualname) -> i64  跨模块类模式按限定名匹配
    SuperLookup,  // (obj*, i64 after_cid, i64 slot, str* name) -> fnptr
    IsException,  // (obj*, str* type) -> i64
    IsSeq,        // (i64) -> i64  运行期值是不是 list/tuple
    IsDict,       // (i64) -> i64  运行期值是不是 dict/set 载体
    StrEqUnknown, // (i64, str*) -> i64  unknown 值上的字符串模式
    ComplexEqUnknown, // (i64, c*) -> i64  unknown 值上的复数模式
    IsStr,        // (i64) -> i64  unknown 值是不是 str
    IsComplex,    // (i64) -> i64  unknown 值是不是复数对象
    AttrGet,      // (obj*, str* name, i64 dflt) -> i64  实例动态属性
    AttrSet,      // (obj*, str* name, i64 value) -> void  实例动态属性
    AttrHas,      // (obj*, str* name) -> i64  实例有没有动态属性
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
    StrLenBytes,  // (str*) -> i64 字节长度（O(1)，读 +8）
    StrCharAtCursor, // (str*, i64 i, strcursor*) -> str* 顺序访问 O(1)
    BytesSlice,   // (bytes*, i64 start, i64 stop, i64 step) -> bytes*
    BytesConcat,  // (bytes*, bytes*) -> bytes*
    BytesRepeat,  // (bytes*, i64 n) -> bytes*
    StrEncode,    // (str*) -> bytes*
    BytesDecode,  // (bytes*) -> str*
    BytesMethod,  // (bytes* self, i64 op, i64 a0, i64 a1, i64 a2) -> i64
    StrCapwords,  // (str*) -> str*  单遍 capwords
    StrRepeat,    // (str*, i64 n) -> str*
    ListRepeat,   // (list*, i64 n) -> list*
    Sha256Digest, // (bytes*) -> bytes*  32 字节摘要
    Sha256Hex,    // (bytes*) -> str*    64 字符十六进制
    B64Encode,    // (bytes*, i64 urlsafe) -> bytes*
    B64Decode,    // (bytes*) -> bytes*
    BytesToList,  // (bytes*) -> list[int]（for c in b"..."）
    BytesFromList, // (list*) -> bytes*  bytes([...])
    BytesZeros,   // (i64) -> bytes*     bytes(n)
    StrWrapList,  // (str*) -> list*（单元素，subprocess 用）
    ListExtend,   // (list* dst, list* src) -> void
    ListSumI64,   // (list*, i64 start) -> i64
    ListSumF64,   // (list*, f64 start) -> f64
    ListMinI64,   // (list*) -> i64  空表抛 ValueError
    ListMaxI64,   // (list*) -> i64
    ListCopy,     // (list*) -> list*
    ListReversed, // (list*) -> list*  倒序副本
    ListSort,     // (list*, i64 kind) -> void  原地排序
    SortedList,   // (list*, i64 kind) -> list*  排序副本
    ListSortedBy, // (list* src, list* keys, i64 kind) -> list*  按键排序的副本
    ListIndex,    // (list*, i64 v, i64 kind) -> i64  找不到抛 ValueError
    ListCount,    // (list*, i64 v, i64 kind) -> i64
    ListInsert,   // (list*, i64 idx, i64 v) -> void
    ListReverse,  // (list*) -> void  原地反转
    IntBitLength, // (i64) -> i64
    EnumerateList,// (list*) -> list*  元素是 [i, v] 对
    Zip2List,     // (list*, list*) -> list*
    SetFromList,  // (list*, i64 key_kind, i64 key_own) -> dict*
    DictFromPairs, // (list*, kk, ko, vo, hslot, eslot) -> dict* 元素是 (k, v)
    RangeNew,     // (i64 start, i64 stop, i64 step) -> range*
    RangeLen,     // (range*) -> i64
    RangeFirst,   // (range*) -> i64
    RangeStep,    // (range*) -> i64
    RangeStop,    // (range*) -> i64
    RangeGet,     // (range*, i64 idx) -> i64
    RangeContains,// (range*, i64 v) -> i64
    RangeEq,      // (range*, range*) -> i64
    RangeSumI64,  // (range*, i64 start) -> i64
    RangeSumF64,  // (range*, f64 start) -> f64
    RangeMinI64,  // (range*) -> i64
    RangeMaxI64,  // (range*) -> i64
    RangeAny,     // (range*) -> i64
    RangeAll,     // (range*) -> i64
    RangeToList,  // (range*) -> list*
    RangeRepr,    // (range*) -> str*
    ReprTuple,    // (tuple*, i64 kind) -> str*
    HandlerPush,  // () -> handler*   压入异常 handler，记录临时栈水位
    HandlerLeave, // (handler*) -> void 弹出并清理临时栈
    HandlerDiscard, // (handler*) -> void 只弹出，不清理临时值
    ExcMatches,   // (str* type) -> i64
    ExcAccept,    // () -> str*       把 pending 转为 current，返回消息
    ExcAcceptValue, // () -> excval*   进入 except 体，返回异常对象
    ExcAcceptInstance, // () -> inst*     进入 except 体，返回富异常实例（无则 0）
    ExcDone,      // () -> void
    ExcEnter,     // (i64* cur, i64* top) -> i64 进入 resume：存调用方异常上下文
    ExcLeave,     // (i64* cur, i64* top, i64 token) -> void 挂起/退出：换回去
    ExcClear,     // () -> void
    ExcReraise,   // () -> void       noreturn
    ExcTakeValue, // () -> excval*    取出待处理异常（复制成异常值），清空全局
    ExcReraiseValueIf, // (excval*) -> void  非空时重新抛出（复制后 longjmp）
    ExcTypeName,  // () -> str*
    ExcMessage,   // () -> str*
    ExcValueArgs, // (excval*) -> tuple*
    ExcValueStr,  // (excval*) -> str*
    ExcValueCause, // (excval*) -> excval*|0
    ExcValueNew,  // (str* type, str* msg|0) -> excval*
    ExcValueNewArgs, // (str* type, list* args) -> excval*
    ExcRegister,  // (str* child, str* parent) -> void  自定义异常类注册
    RaiseInstance, // (inst*, str* type) -> void noreturn  抛富异常实例
      // (str* child, str* parent) -> void  自定义异常类注册
    Raise,        // (str* type, str* msg) -> void  noreturn
    RaiseArgs,    // (str* type, list* args) -> void  noreturn
    RaiseValue,   // (excval*) -> void noreturn
    RaiseValueFrom, // (excval*, excval*|0) -> void noreturn
    Reraise,      // () -> void       noreturn  裸 raise
    // ── 零开销异常（Z2-b2，--zero-cost-exc）：表驱动展开版本的抛出 ──
    // 与上面的 Raise*/Reraise 一一对应，区别只在最后一步：把异常装进
    // _Unwind_Exception 交给 _Unwind_RaiseException，而不是 longjmp。
    // 只有「函数内没有 setjmp 兜底」的零开销 try 体里才会用到它们。
    EhRaise,          // (str* type, str* msg) -> void noreturn
    EhRaiseValueFrom, // (excval*, excval*|0) -> void noreturn
    EhReraise,        // () -> void noreturn  裸 raise 的表驱动版
    EhRethrow,        // () -> void noreturn  landing 分发不匹配时重抛同一个异常
    EhLand,           // (unwind_exc*) -> void landing pad 入口：取回异常并收尾展开包装
    EhEnter,          // () -> void 进入零开销 try 保护区（exc_longjmp 据此改走 _Unwind）
    EhLeave,          // () -> void 离开保护区
                      //   实参由 codegen 特殊处理（直接从 landing pad 入口的 %rax 取）
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
    StrPercentFormat, // (str* pat, list* args, str* desc, i64 is_bytes) -> str*
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
    GenThrow,     // (gen*, excval*) -> i64  gen.throw(exc)
    GenThrowOpt,  // (gen*, exc*, i64* ok) -> i64  yield from 转发 throw 用
    GenClose,     // (gen*) -> void  跑跨挂起点的清理并结束
    HandlerSuspend, // (handler*) -> void  yield 时把记录临时摘下
    HandlerRearm,   // (handler*) -> void  恢复时重新压栈并记录水位
    HandlerFree,    // (handler*) -> void  生成器 close：释放挂起的记录
    AsyncioRun,     // (coro*) -> i64   跑事件循环直到根协程结束
    AsyncioSleep,   // (f64) -> awaitable*
    AsyncioGather,  // (list*, i64 result_kind) -> awaitable*
    AsyncioCreateTask, // (coro*, i64 result_kind) -> task*
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
    SubprocessRun,     // (list* argv, i64 capture, i64 text, f64 timeout, i64 check,
                       //  i64 shell, str* cwd) -> proc*
    SubprocessReturnCode,  // (proc*) -> i64
    SubprocessStdout,      // (proc*) -> str*
    SubprocessStderr,      // (proc*) -> str*
    PopenNew,              // (list*, i64 shell, str* cwd, i64 in, i64 out, i64 err) -> popen*
    PopenPid,              // (popen*) -> i64
    PopenReturnCode,       // (popen*) -> i64
    PopenStdout,           // (popen*) -> file*
    PopenStderr,           // (popen*) -> file*
    PopenStdin,            // (popen*) -> file*
    PopenWait,             // (popen*, f64 timeout) -> i64
    PopenKill,             // (popen*) -> void
    PopenTerminate,        // (popen*) -> void
    PopenSendSignal,       // (popen*, i64 sig) -> void
    PopenCommunicate,      // (popen*, str* input) -> tuple[str, str]
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
    IterProduct,           // (list* seqs, i64 elem_kind) -> list*；seqs 里装源列表
    IterPermutations,      // (list* pool, i64 r, i64 elem_kind) -> list*；r < 0 表示 len
    IterCombinations,      // (list* pool, i64 r, i64 elem_kind) -> list*
    IterCombinationsWR,    // (list* pool, i64 r, i64 elem_kind) -> list*
    IterZipLongest,        // (list* seqs, i64 elem_kind) -> list*；缺的补 None
    IterAccumulate,        // (list* xs, i64 is_float) -> list*
    FuncReduce,            // (fn*, list* xs, i64 init, i64 has_init) -> i64
    FuncReduceF,           // (fn*, list* xs, f64 init, i64 has_init) -> f64
    CollCounter,           // (list* items, i64 key_kind, i64 key_own) -> dict*
    CollMostCommon,        // (dict*, i64 n) -> list*
    JsonDumps,             // (i64 value, str* desc) -> str*
    JsonLoads,             // (str* text, str* desc) -> i64
    // CPython 桥：(str* module, str* attr, list* args, str* descs) -> i64 unknown
    // 懒拉起 python3 worker 子进程，JSON 帧 IPC；见 runtime/rt_cpy.c。
    CpyCall,
    RegexCompile,          // (str* pat, i64 flags) -> regex*
    RegexSearchStr,        // (str* pat, str* text, i64 flags, i64 mode) -> match*
    RegexSearch,           // (regex*, str* text, i64 mode) -> match*
    RegexFindAllStr,       // (str* pat, str* text, i64 flags) -> list*
    RegexFindAll,          // (regex*, str* text) -> list*
    RegexSubStr,           // (str* pat, str* repl, str* text, i64 count, i64 flags) -> str*
    RegexSub,              // (regex*, str* repl, str* text, i64 count) -> str*
    RegexSubFn,            // (regex*, i64 closure, str* text, i64 count) -> str*
    RegexSplitStr,         // (str* pat, str* text, i64 maxsplit, i64 flags) -> list*
    RegexSplit,            // (regex*, str* text, i64 maxsplit) -> list*
    RegexEscape,           // (str*) -> str*
    RegexPatternGroups,    // (regex*) -> i64
    RegexPatternFlags,     // (regex*) -> i64
    RegexPatternStr,       // (regex*) -> str*
    RegexFindIter,         // (regex*, str* text) -> list*
    RegexFindIterStr,      // (str* pat, str* text, i64 flags) -> list*
    RegexExpand,           // (match*, str* tmpl) -> str*
    RegexGroup,            // (match*, i64 n) -> str*
    RegexGroupStr,         // (match*, str* name) -> str*
    RegexGroups,           // (match*) -> list*
    RegexGroupDict,        // (match*) -> dict*
    RegexLastIndex,        // (match*) -> i64
    RegexLastGroup,        // (match*) -> str*
    RegexStart,            // (match*, i64 n) -> i64
    RegexEnd,              // (match*, i64 n) -> i64
    RegexSpan,             // (match*, i64 n) -> tuple[i64, i64]
    // datetime / pathlib
    DateNew,               // (y, m, d) -> datetime*
    DateTimeNew,           // (y, m, d, h, mi, s) -> datetime*
    DateTimeNow,           // (is_date) -> datetime*
    DateTimeField,         // (dt*, idx) -> i64
    DateTimeIsoformat,     // (dt*) -> str*
    DateTimeStrftime,      // (dt*, str* fmt) -> str*
    // datetime.timedelta
    TimeDeltaNew,          // (days, seconds, microseconds) -> timedelta*
    TimeDeltaField,        // (td*, idx) -> i64（0 days / 1 seconds / 2 microseconds）
    TimeDeltaTotalSeconds, // (td*) -> f64
    TimeDeltaCmp,          // (td*, td*) -> i64（-1 / 0 / 1）
    TimeDeltaAdd,          // (td*, td*) -> timedelta*
    TimeDeltaSub,          // (td*, td*) -> timedelta*
    TimeDeltaMul,          // (td*, i64) -> timedelta*
    TimeDeltaNeg,          // (td*) -> timedelta*
    TimeDeltaFloorDivInt,  // (td*, i64) -> timedelta*
    TimeDeltaTrueDivInt,   // (td*, i64) -> timedelta*
    TimeDeltaFloorDivTD,   // (td*, td*) -> i64
    TimeDeltaTrueDivTD,    // (td*, td*) -> f64
    TimeDeltaStr,          // (td*) -> str*
    DateTimeReplace,       // (dt*, list* fields) -> datetime*
    DateTimeAddDelta,      // (dt*, td*, i64 sub) -> datetime*
    DateTimeDiff,          // (dt*, dt*) -> timedelta*
    DateTimeCmp,           // (dt*, dt*) -> i64（-1 / 0 / 1）
    PathNew,               // (str*) -> path*
    PathStr,               // (path*) -> str*
    PathJoin,              // (path*, str*) -> path*
    PathName,              // (path*) -> str*
    PathStem,              // (path*) -> str*
    PathSuffix,            // (path*) -> str*
    PathParent,            // (path*) -> path*
    PathExists,            // (path*) -> i64
    PathIsFile,            // (path*) -> i64
    PathIsDir,             // (path*) -> i64
    PathReadText,          // (path*) -> str*
    PathWriteText,         // (path*, str*) -> i64
    PathIterdir,           // (path*) -> list[path]*
    PathGlob,              // (path*, str*) -> list[path]*
    PathMkdir,             // (path*, mode, parents, exist_ok) -> i64
    PathWithSuffix,        // (path*, str*) -> path*
    PathParts,             // (path*) -> list[str]*
    PathReadBytes,         // (path*) -> bytes*
    PathWriteBytes,        // (path*, bytes*) -> i64
    PathIsAbsolute,        // (path*) -> i64
    PathRelativeTo,        // (path*, str*) -> path*
    PathResolve,           // (path*) -> path*
    PathAbsolute,          // (path*) -> path*
    PathTouch,             // (path*) -> i64
    RandRange,             // (rng*, a, b, step) -> i64
    RandGetrandbits,       // (rng*, k) -> i64
    RandGauss,             // (rng*, mu, sigma) -> f64
    RandExpovariate,       // (rng*, lambd) -> f64
    RandTriangular,        // (rng*, low, high, mode) -> f64
    RandSample,            // (rng*, list*, k) -> list*
    RandChoices,           // (rng*, list*, k) -> list*
    OsGetpid,              // () -> i64
    OsChdir,               // (str*) -> void
    OsChmod,               // (str*, i64) -> void
    OsAccess,              // (str*, i64) -> i64
    OsMakedirs,            // (str*, i64 exist_ok) -> void
    OsPathRealpath,        // (str*) -> str*
    OsPathGetsize,         // (str*) -> i64
    OsPathGetmtime,        // (str*) -> f64
    OsPathSplit,           // (str*) -> list*
    OsPathSamefile,        // (str*, str*) -> i64
    DequeNew,              // (maxlen) -> deque*
    DequeFromList,         // (list*, maxlen) -> deque*
    DequeAppend,           // (dq*, v) -> void
    DequeAppendLeft,       // (dq*, v) -> void
    DequePop,              // (dq*) -> v
    DequePopleft,          // (dq*) -> v
    DequeLen,              // (dq*) -> i64
    DequeMaxlen,           // (dq*) -> i64
    DequeGet,              // (dq*, i) -> v
    DequeSet,              // (dq*, i, v) -> void
    DequeRotate,           // (dq*, n) -> void
    DequeClear,            // (dq*) -> void
    DequeExtend,           // (dq*, list*) -> void
    DequeExtendLeft,       // (dq*, list*) -> void
    DequeToList,           // (dq*) -> list*
    DequeRemove,           // (dq*, v, kind) -> void
    DequeCount,            // (dq*, v, kind) -> i64
    DequeIndex,            // (dq*, v, kind) -> i64
    DequeInsert,           // (dq*, i, v) -> void
    DequeReverse,          // (dq*) -> void
    DequeCopy,             // (dq*) -> deque*
    ListCopyRange,         // (list* dst, list* src, i64 lo, i64 hi, i64 heap) -> void
};

inline const char* rt_name(RtFn f) {
    switch (f) {
        case RtFn::ListNew: return "pa_list_new";
        case RtFn::ListAppend: return "pa_list_append";
        case RtFn::ListCopyRange: return "pa_list_copy_range";
        case RtFn::ListGet: return "pa_list_get";
        case RtFn::ListSet: return "pa_list_set";
        case RtFn::ListSetLen: return "pa_list_set_len";
        case RtFn::ListPrep: return "pa_list_prep";
        case RtFn::StrConcat: return "pa_str_concat";
        case RtFn::StrCharAt: return "pa_str_char_at";
        case RtFn::BytesAt: return "pa_bytes_at";
        case RtFn::StrFromI64: return "pa_str_from_i64";
        case RtFn::StrAny: return "pa_str_any";
        case RtFn::StrFromF64: return "pa_str_from_f64";
        case RtFn::StrFromBool: return "pa_str_from_bool";
        case RtFn::StrFromNone: return "pa_str_from_none";
        case RtFn::StrToI64: return "pa_str_to_i64";
        case RtFn::StrToF64: return "pa_str_to_f64";
        case RtFn::Ord: return "pa_ord";
        case RtFn::Chr: return "pa_chr";
        case RtFn::IntToBase: return "pa_int_to_base";
        case RtFn::RoundF64: return "pa_round_f64";
        case RtFn::RoundI64: return "pa_round_i64";
        case RtFn::StrMethod: return "pa_str_method";
        case RtFn::FileOpen: return "pa_file_open";
        case RtFn::FileConfig: return "pa_file_config";
        case RtFn::FileEncoding: return "pa_file_encoding";
        case RtFn::FileErrors: return "pa_file_errors";
        case RtFn::FileRead: return "pa_file_read";
        case RtFn::FileReadLine: return "pa_file_readline";
        case RtFn::FileReadLines: return "pa_file_readlines";
        case RtFn::FileWrite: return "pa_file_write";
        case RtFn::FileWritelines: return "pa_file_writelines";
        case RtFn::FileSeek: return "pa_file_seek";
        case RtFn::FileTell: return "pa_file_tell";
        case RtFn::FileTruncate: return "pa_file_truncate";
        case RtFn::FileFileno: return "pa_file_fileno";
        case RtFn::FileIsatty: return "pa_file_isatty";
        case RtFn::FileReadable: return "pa_file_readable";
        case RtFn::FileWritable: return "pa_file_writable";
        case RtFn::FileSeekable: return "pa_file_seekable";
        case RtFn::FileClosed: return "pa_file_closed";
        case RtFn::FileName: return "pa_file_name";
        case RtFn::FileMode: return "pa_file_mode";
        case RtFn::FileClose: return "pa_file_close";
        case RtFn::FileFlush: return "pa_file_flush";
        case RtFn::Input: return "pa_input";
        case RtFn::SysStream: return "pa_sys_stream";
        case RtFn::PrintI64: return "pa_print_i64";
        case RtFn::PrintAny: return "pa_print_any";
        case RtFn::PrintF64: return "pa_print_f64";
        case RtFn::PrintBool: return "pa_print_bool";
        case RtFn::PrintStr: return "pa_print_str";
        case RtFn::PrintBytes: return "pa_print_bytes";
        case RtFn::PrintNone: return "pa_print_none";
        case RtFn::PrintSep: return "pa_print_sep";
        case RtFn::PrintEnd: return "pa_print_end";
        case RtFn::Panic: return "pa_panic";
        case RtFn::PanicDiv0: return "pa_panic_div0";
        case RtFn::PanicOverflow: return "pa_panic_overflow";
        case RtFn::RcInc: return "pa_rc_inc";
        case RtFn::RcDec: return "pa_rc_dec";
        case RtFn::RcIncUnknown: return "pa_rc_inc_unknown";
        case RtFn::RcDecUnknown: return "pa_rc_dec_unknown";
        case RtFn::RcTemp: return "pa_rc_temp";
        case RtFn::RcTempUnknown: return "pa_rc_temp_unknown";
        case RtFn::RcTempTake: return "pa_rc_temp_take";
        case RtFn::RcMark: return "pa_rc_mark";
        case RtFn::RcCleanup: return "pa_rc_cleanup";
        case RtFn::RcShutdown: return "pa_rc_shutdown";
        case RtFn::GcShutdown: return "pa_gc_shutdown";
        case RtFn::ClosureNew: return "pa_closure_new";
        case RtFn::DictNew: return "pa_dict_new";
        case RtFn::DictSet: return "pa_dict_set";
        case RtFn::DictGet: return "pa_dict_get";
        case RtFn::DictSetDefaultFactory: return "pa_dict_set_default_factory";
        case RtFn::DictContains: return "pa_dict_contains";
        case RtFn::DictKeyAt: return "pa_dict_key_at";
        case RtFn::ListContains: return "pa_list_contains";
        case RtFn::ListEq: return "pa_list_eq";
        case RtFn::DictEq: return "pa_dict_eq";
        case RtFn::SetEq: return "pa_set_eq";
        case RtFn::StrContains: return "pa_str_contains";
        case RtFn::DictGetOr: return "pa_dict_get_or";
        case RtFn::DictBump: return "pa_dict_bump";
        case RtFn::DictTryGet: return "pa_dict_try_get";
        case RtFn::DictGetOpt: return "pa_dict_get_opt";
        case RtFn::DictKeys: return "pa_dict_keys";
        case RtFn::DictValues: return "pa_dict_values";
        case RtFn::DictItems: return "pa_dict_items";
        case RtFn::DictUpdate: return "pa_dict_update";
        case RtFn::DictCopy: return "pa_dict_copy";
        case RtFn::IsInstance: return "pa_isinstance";
        case RtFn::ClassNew: return "pa_class_new";
        case RtFn::ClassCtor: return "pa_class_ctor";
        case RtFn::ClassMethod: return "pa_class_method";
        case RtFn::ClassName: return "pa_class_name";
        case RtFn::ClassRepr: return "pa_class_repr";
        case RtFn::IsInstanceClass: return "pa_isinstance_class";
        case RtFn::ObjClassId: return "pa_obj_class_id";
        case RtFn::IsNamedClass: return "pa_is_named_class";
        case RtFn::SuperLookup: return "pa_super_lookup";
        case RtFn::IsException: return "pa_is_exc";
        case RtFn::IsSeq: return "pa_is_seq";
        case RtFn::IsDict: return "pa_is_dict";
        case RtFn::StrEqUnknown: return "pa_str_eq_unknown";
        case RtFn::ComplexEqUnknown: return "pa_complex_eq_unknown";
        case RtFn::IsStr: return "pa_is_str";
        case RtFn::IsComplex: return "pa_is_complex";
        case RtFn::AttrGet: return "pa_attr_get";
        case RtFn::AttrSet: return "pa_attr_set";
        case RtFn::AttrHas: return "pa_attr_has";
        case RtFn::ListSlice: return "pa_list_slice";
        case RtFn::StrSlice: return "pa_str_slice";
        case RtFn::DictPop: return "pa_dict_pop";
        case RtFn::DictPopItem: return "pa_dict_popitem";
        case RtFn::DictPopLast: return "pa_dict_pop_last";
        case RtFn::DictRemove: return "pa_dict_remove";
        case RtFn::DictClear: return "pa_dict_clear";
        case RtFn::ListSliceAssign: return "pa_list_slice_assign";
        case RtFn::ListDelIndex: return "pa_list_del_index";
        case RtFn::StrToList: return "pa_str_to_list";
        case RtFn::StrLenBytes: return "pa_len_bytes";
        case RtFn::StrCharAtCursor: return "pa_str_char_at_cursor";
        case RtFn::BytesSlice: return "pa_bytes_slice";
        case RtFn::BytesConcat: return "pa_bytes_concat";
        case RtFn::BytesRepeat: return "pa_bytes_repeat";
        case RtFn::StrEncode: return "pa_str_encode";
        case RtFn::BytesDecode: return "pa_bytes_decode";
        case RtFn::BytesMethod: return "pa_bytes_method";
        case RtFn::StrCapwords: return "pa_str_capwords";
        case RtFn::StrRepeat: return "pa_str_repeat";
        case RtFn::ListRepeat: return "pa_list_repeat";
        case RtFn::Sha256Digest: return "pa_sha256_digest";
        case RtFn::Sha256Hex: return "pa_sha256_hex";
        case RtFn::B64Encode: return "pa_b64encode";
        case RtFn::B64Decode: return "pa_b64decode";
        case RtFn::BytesToList: return "pa_bytes_to_list";
        case RtFn::BytesFromList: return "pa_bytes_from_list";
        case RtFn::BytesZeros: return "pa_bytes_zeros";
        case RtFn::StrWrapList: return "pa_str_wrap_list";
        case RtFn::ListExtend: return "pa_list_extend";
        case RtFn::ListSumI64: return "pa_list_sum_i64";
        case RtFn::ListSumF64: return "pa_list_sum_f64";
        case RtFn::ListMinI64: return "pa_list_min_i64";
        case RtFn::ListMaxI64: return "pa_list_max_i64";
        case RtFn::ListCopy: return "pa_list_copy";
        case RtFn::ListReversed: return "pa_list_reversed";
        case RtFn::ListSort: return "pa_list_sort";
        case RtFn::SortedList: return "pa_sorted_list";
        case RtFn::ListSortedBy: return "pa_list_sorted_by";
        case RtFn::ListIndex: return "pa_list_index";
        case RtFn::ListCount: return "pa_list_count";
        case RtFn::ListInsert: return "pa_list_insert";
        case RtFn::ListReverse: return "pa_list_reverse";
        case RtFn::IntBitLength: return "pa_int_bit_length";
        case RtFn::EnumerateList: return "pa_enumerate_list";
        case RtFn::Zip2List: return "pa_zip2_list";
        case RtFn::SetFromList: return "pa_set_from_list";
        case RtFn::DictFromPairs: return "pa_dict_from_pairs";
        case RtFn::RangeNew: return "pa_range_new";
        case RtFn::RangeLen: return "pa_range_len";
        case RtFn::RangeFirst: return "pa_range_first";
        case RtFn::RangeStep: return "pa_range_step";
        case RtFn::RangeStop: return "pa_range_stop";
        case RtFn::RangeGet: return "pa_range_get";
        case RtFn::RangeContains: return "pa_range_contains";
        case RtFn::RangeEq: return "pa_range_eq";
        case RtFn::RangeSumI64: return "pa_range_sum_i64";
        case RtFn::RangeSumF64: return "pa_range_sum_f64";
        case RtFn::RangeMinI64: return "pa_range_min_i64";
        case RtFn::RangeMaxI64: return "pa_range_max_i64";
        case RtFn::RangeAny: return "pa_range_any";
        case RtFn::RangeAll: return "pa_range_all";
        case RtFn::RangeToList: return "pa_range_to_list";
        case RtFn::RangeRepr: return "pa_range_repr";
        case RtFn::ReprTuple: return "pa_repr_tuple";
        case RtFn::HandlerPush: return "pa_handler_push";
        case RtFn::HandlerLeave: return "pa_handler_leave";
        case RtFn::HandlerDiscard: return "pa_handler_discard";
        case RtFn::ExcMatches: return "pa_exc_matches";
        case RtFn::ExcAccept: return "pa_exc_accept";
        case RtFn::ExcAcceptValue: return "pa_exc_accept_value";
        case RtFn::ExcAcceptInstance: return "pa_exc_accept_instance";
        case RtFn::ExcDone: return "pa_exc_done";
        case RtFn::ExcEnter: return "pa_exc_enter";
        case RtFn::ExcLeave: return "pa_exc_leave";
        case RtFn::ExcClear: return "pa_exc_clear";
        case RtFn::ExcReraise: return "pa_exc_reraise";
        case RtFn::ExcTakeValue: return "pa_exc_take_value";
        case RtFn::ExcReraiseValueIf: return "pa_exc_reraise_value_if";
        case RtFn::ExcTypeName: return "pa_exc_type_name";
        case RtFn::ExcMessage: return "pa_exc_message";
        case RtFn::ExcValueArgs: return "pa_exc_value_args";
        case RtFn::ExcValueStr: return "pa_exc_value_str";
        case RtFn::ExcValueCause: return "pa_exc_value_cause";
        case RtFn::ExcValueNew: return "pa_exc_value_new";
        case RtFn::ExcValueNewArgs: return "pa_exc_value_new_args";
        case RtFn::ExcRegister: return "pa_exc_register";
        case RtFn::RaiseInstance: return "pa_raise_instance";
        case RtFn::Raise: return "pa_raise";
        case RtFn::RaiseArgs: return "pa_raise_args";
        case RtFn::RaiseValue: return "pa_raise_value";
        case RtFn::RaiseValueFrom: return "pa_raise_value_from";
        case RtFn::Reraise: return "pa_reraise";
        case RtFn::EhRaise: return "pa_eh_raise";
        case RtFn::EhRaiseValueFrom: return "pa_eh_raise_value_from";
        case RtFn::EhReraise: return "pa_eh_reraise";
        case RtFn::EhRethrow: return "pa_eh_rethrow";
        case RtFn::EhLand: return "pa_eh_land";
        case RtFn::EhEnter: return "pa_eh_enter";
        case RtFn::EhLeave: return "pa_eh_leave";
        case RtFn::MathSqrt: return "pa_math_sqrt";
        case RtFn::MathFloor: return "pa_math_floor";
        case RtFn::MathCeil: return "pa_math_ceil";
        case RtFn::AbsI64: return "pa_abs_i64";
        case RtFn::ComplexNew: return "pa_complex_new";
        case RtFn::ComplexAdd: return "pa_complex_add";
        case RtFn::ComplexSub: return "pa_complex_sub";
        case RtFn::ComplexMul: return "pa_complex_mul";
        case RtFn::ComplexDiv: return "pa_complex_div";
        case RtFn::ComplexNeg: return "pa_complex_neg";
        case RtFn::ComplexAbs: return "pa_complex_abs";
        case RtFn::ComplexStr: return "pa_complex_str";
        case RtFn::ComplexEq: return "pa_complex_eq";
        case RtFn::ComplexTruth: return "pa_complex_truth";
        case RtFn::FmtI64: return "pa_fmt_i64";
        case RtFn::FmtBool: return "pa_fmt_bool";
        case RtFn::FmtF64: return "pa_fmt_f64";
        case RtFn::FmtStr: return "pa_fmt_str";
        case RtFn::StrPercentFormat: return "pa_percent_format";
        case RtFn::FmtComplex: return "pa_fmt_complex";
        case RtFn::ReprStr: return "pa_repr_str";
        case RtFn::MathFabs: return "pa_math_fabs";
        case RtFn::MathSin: return "pa_math_sin";
        case RtFn::MathCos: return "pa_math_cos";
        case RtFn::MathTan: return "pa_math_tan";
        case RtFn::MathLog: return "pa_math_log";
        case RtFn::MathLog2: return "pa_math_log2";
        case RtFn::MathLog10: return "pa_math_log10";
        case RtFn::MathExp: return "pa_math_exp";
        case RtFn::MathPow: return "pa_math_pow";
        case RtFn::MathFmod: return "pa_math_fmod";
        case RtFn::MathAtan2: return "pa_math_atan2";
        case RtFn::MathHypot: return "pa_math_hypot";
        case RtFn::MathDegrees: return "pa_math_degrees";
        case RtFn::MathRadians: return "pa_math_radians";
        case RtFn::MathTrunc: return "pa_math_trunc";
        case RtFn::MathCopysign: return "pa_math_copysign";
        case RtFn::MathIsnan: return "pa_math_isnan";
        case RtFn::MathIsinf: return "pa_math_isinf";
        case RtFn::MathIsfinite: return "pa_math_isfinite";
        case RtFn::MathFactorial: return "pa_math_factorial";
        case RtFn::MathGcd: return "pa_math_gcd";
        case RtFn::MathLcm: return "pa_math_lcm";
        case RtFn::MathFsum: return "pa_math_fsum";
        case RtFn::HashValue: return "pa_hash_value";
        case RtFn::HashF64: return "pa_hash_f64";
        case RtFn::MathAcos: return "pa_math_acos";
        case RtFn::MathAsin: return "pa_math_asin";
        case RtFn::MathAtan: return "pa_math_atan";
        case RtFn::MathAcosh: return "pa_math_acosh";
        case RtFn::MathAsinh: return "pa_math_asinh";
        case RtFn::MathAtanh: return "pa_math_atanh";
        case RtFn::MathCbrt: return "pa_math_cbrt";
        case RtFn::MathCosh: return "pa_math_cosh";
        case RtFn::MathSinh: return "pa_math_sinh";
        case RtFn::MathTanh: return "pa_math_tanh";
        case RtFn::MathExp2: return "pa_math_exp2";
        case RtFn::MathExpm1: return "pa_math_expm1";
        case RtFn::MathLog1p: return "pa_math_log1p";
        case RtFn::MathErf: return "pa_math_erf";
        case RtFn::MathErfc: return "pa_math_erfc";
        case RtFn::MathGamma: return "pa_math_gamma";
        case RtFn::MathLgamma: return "pa_math_lgamma";
        case RtFn::MathUlp: return "pa_math_ulp";
        case RtFn::MathNextafter: return "pa_math_nextafter";
        case RtFn::MathRemainder: return "pa_math_remainder";
        case RtFn::MathLdexp: return "pa_math_ldexp";
        case RtFn::MathFma: return "pa_math_fma";
        case RtFn::MathIsclose: return "pa_math_isclose";
        case RtFn::MathFrexp: return "pa_math_frexp";
        case RtFn::MathModf: return "pa_math_modf";
        case RtFn::MathIsqrt: return "pa_math_isqrt";
        case RtFn::MathComb: return "pa_math_comb";
        case RtFn::MathPerm: return "pa_math_perm";
        case RtFn::MathProd: return "pa_math_prod";
        case RtFn::MathSumprod: return "pa_math_sumprod";
        case RtFn::MathDist: return "pa_math_dist";
        case RtFn::SysArgv: return "pa_sys_argv";
        case RtFn::SysExit: return "pa_sys_exit";
        case RtFn::GenNext: return "pa_gen_next";
        case RtFn::GenNextOpt: return "pa_gen_next_opt";
        case RtFn::GenSend: return "pa_gen_send";
        case RtFn::GenSendOpt: return "pa_gen_send_opt";
        case RtFn::GenThrow: return "pa_gen_throw";
        case RtFn::GenThrowOpt: return "pa_gen_throw_opt";
        case RtFn::GenClose: return "pa_gen_close";
        case RtFn::HandlerSuspend: return "pa_handler_suspend";
        case RtFn::HandlerRearm: return "pa_handler_rearm";
        case RtFn::HandlerFree: return "pa_handler_free";
        case RtFn::AsyncioRun: return "pa_asyncio_run";
        case RtFn::AsyncioSleep: return "pa_asyncio_sleep";
        case RtFn::AsyncioGather: return "pa_asyncio_gather";
        case RtFn::AsyncioCreateTask: return "pa_asyncio_create_task";
        case RtFn::AsyncioTaskDone: return "pa_asyncio_task_done";
        case RtFn::AsyncioTaskCancel: return "pa_asyncio_task_cancel";
        case RtFn::AsyncGenANext: return "pa_asyncgen_anext";
        case RtFn::AsyncGenAsend: return "pa_asyncgen_asend";
        case RtFn::AsyncGenAClose: return "pa_asyncgen_aclose";
        case RtFn::AsyncGenAthrow: return "pa_asyncgen_athrow";
        case RtFn::AsyncioFutureNew: return "pa_asyncio_future_new";
        case RtFn::FutureSetResult: return "pa_future_set_result";
        case RtFn::FutureSetException: return "pa_future_set_exception";
        case RtFn::FutureSetExceptionValue: return "pa_future_set_exception_value";
        case RtFn::FutureAddCallback: return "pa_future_add_callback";
        case RtFn::FutureDone: return "pa_future_done";
        case RtFn::FutureCancel: return "pa_future_cancel";
        case RtFn::FutureResult: return "pa_future_result";
        case RtFn::FutureException: return "pa_future_exception";
        case RtFn::FutureCancelled: return "pa_future_cancelled";
        case RtFn::AsyncioTaskResult: return "pa_asyncio_task_result";
        case RtFn::AsyncioTaskException: return "pa_asyncio_task_exception";
        case RtFn::AsyncioTaskCancelled: return "pa_asyncio_task_cancelled";
        case RtFn::AsyncioWaitFd: return "pa_asyncio_wait_fd";
        case RtFn::AsyncioWaitFor: return "pa_asyncio_wait_for";
        case RtFn::EventNew: return "pa_event_new";
        case RtFn::EventSet: return "pa_event_set";
        case RtFn::EventIsSet: return "pa_event_is_set";
        case RtFn::EventWait: return "pa_event_wait";
        case RtFn::LockNew: return "pa_lock_new";
        case RtFn::LockAcquire: return "pa_lock_acquire";
        case RtFn::LockRelease: return "pa_lock_release";
        case RtFn::LockLocked: return "pa_lock_locked";
        case RtFn::QueueNew: return "pa_queue_new";
        case RtFn::QueuePut: return "pa_queue_put";
        case RtFn::QueueGet: return "pa_queue_get";
        case RtFn::QueueQsize: return "pa_queue_qsize";
        case RtFn::QueueEmpty: return "pa_queue_empty";
        case RtFn::QueueFull: return "pa_queue_full";
        case RtFn::QueueJoin: return "pa_queue_join";
        case RtFn::QueueTaskDone: return "pa_queue_task_done";
        case RtFn::QueuePutNowait: return "pa_queue_put_nowait";
        case RtFn::QueueGetNowait: return "pa_queue_get_nowait";
        case RtFn::AsyncTimeoutNew: return "pa_timeout_new";
        case RtFn::TimeoutEnter: return "pa_timeout_enter";
        case RtFn::TimeoutExit: return "pa_timeout_exit";
        case RtFn::TimeoutExpired: return "pa_timeout_expired";
        case RtFn::TaskGroupNew: return "pa_taskgroup_new";
        case RtFn::TaskGroupAdd: return "pa_taskgroup_add";
        case RtFn::TaskGroupExit: return "pa_taskgroup_exit";
        case RtFn::TaskGroupCancel: return "pa_taskgroup_cancel";
        case RtFn::SemNew: return "pa_sem_new";
        case RtFn::SemAcquire: return "pa_sem_acquire";
        case RtFn::SemRelease: return "pa_sem_release";
        case RtFn::SemLocked: return "pa_sem_locked";
        case RtFn::RandNew: return "pa_random_new";
        case RtFn::RandSeed: return "pa_random_seed";
        case RtFn::RandNext: return "pa_random_next";
        case RtFn::RandRandint: return "pa_random_randint";
        case RtFn::RandChoice: return "pa_random_choice";
        case RtFn::RandUniform: return "pa_random_uniform";
        case RtFn::RandShuffle: return "pa_random_shuffle";
        case RtFn::RandGSeed: return "pa_random_gseed";
        case RtFn::RandGNext: return "pa_random_gnext";
        case RtFn::RandGRandint: return "pa_random_grandint";
        case RtFn::RandGChoice: return "pa_random_gchoice";
        case RtFn::RandGUniform: return "pa_random_guniform";
        case RtFn::RandGShuffle: return "pa_random_gshuffle";
        case RtFn::OsMkdir: return "pa_os_mkdir";
        case RtFn::OsRmdir: return "pa_os_rmdir";
        case RtFn::OsRemove: return "pa_os_remove";
        case RtFn::OsRename: return "pa_os_rename";
        case RtFn::OsGetcwd: return "pa_os_getcwd";
        case RtFn::OsCpuCount: return "pa_os_cpu_count";
        case RtFn::OsPathJoin: return "pa_os_path_join";
        case RtFn::OsPathExists: return "pa_os_path_exists";
        case RtFn::OsPathIsdir: return "pa_os_path_isdir";
        case RtFn::OsPathIsfile: return "pa_os_path_isfile";
        case RtFn::SubprocessRun: return "pa_subprocess_run";
        case RtFn::SubprocessReturnCode: return "pa_subprocess_returncode";
        case RtFn::SubprocessStdout: return "pa_subprocess_stdout";
        case RtFn::SubprocessStderr: return "pa_subprocess_stderr";
        case RtFn::PopenNew: return "pa_popen_new";
        case RtFn::PopenPid: return "pa_popen_pid";
        case RtFn::PopenReturnCode: return "pa_popen_returncode";
        case RtFn::PopenStdout: return "pa_popen_stdout";
        case RtFn::PopenStderr: return "pa_popen_stderr";
        case RtFn::PopenStdin: return "pa_popen_stdin";
        case RtFn::PopenWait: return "pa_popen_wait";
        case RtFn::PopenKill: return "pa_popen_kill";
        case RtFn::PopenTerminate: return "pa_popen_terminate";
        case RtFn::PopenSendSignal: return "pa_popen_send_signal";
        case RtFn::PopenCommunicate: return "pa_popen_communicate";
        case RtFn::TimeTime: return "pa_time_time";
        case RtFn::TimeMonotonic: return "pa_time_monotonic";
        case RtFn::TimePerfCounter: return "pa_time_perf_counter";
        case RtFn::TimeSleep: return "pa_time_sleep";
        case RtFn::OsListdir: return "pa_os_listdir";
        case RtFn::OsGetenv: return "pa_os_getenv";
        case RtFn::OsPathBasename: return "pa_os_path_basename";
        case RtFn::OsPathDirname: return "pa_os_path_dirname";
        case RtFn::OsPathSplitext: return "pa_os_path_splitext";
        case RtFn::OsPathNormpath: return "pa_os_path_normpath";
        case RtFn::OsPathAbspath: return "pa_os_path_abspath";
        case RtFn::OsPathExpanduser: return "pa_os_path_expanduser";
        case RtFn::IterChain: return "pa_itertools_chain";
        case RtFn::IterRepeat: return "pa_itertools_repeat";
        case RtFn::IterProduct: return "pa_itertools_product";
        case RtFn::IterPermutations: return "pa_itertools_permutations";
        case RtFn::IterCombinations: return "pa_itertools_combinations";
        case RtFn::IterCombinationsWR: return "pa_itertools_combinations_wr";
        case RtFn::IterZipLongest: return "pa_itertools_zip_longest";
        case RtFn::IterAccumulate: return "pa_itertools_accumulate";
        case RtFn::FuncReduce: return "pa_functools_reduce";
        case RtFn::FuncReduceF: return "pa_functools_reduce_f";
        case RtFn::CollCounter: return "pa_collections_counter";
        case RtFn::CollMostCommon: return "pa_collections_most_common";
        case RtFn::JsonDumps: return "pa_json_dumps";
        case RtFn::JsonLoads: return "pa_json_loads";
        case RtFn::CpyCall: return "pa_cpy_call";
        case RtFn::RegexCompile: return "pa_re_compile";
        case RtFn::RegexSearchStr: return "pa_re_search_str";
        case RtFn::RegexSearch: return "pa_re_search";
        case RtFn::RegexFindAllStr: return "pa_re_findall_str";
        case RtFn::RegexFindAll: return "pa_re_findall";
        case RtFn::RegexSubStr: return "pa_re_sub_str";
        case RtFn::RegexSub: return "pa_re_sub";
        case RtFn::RegexSubFn: return "pa_re_sub_fn";
        case RtFn::RegexSplitStr: return "pa_re_split_str";
        case RtFn::RegexSplit: return "pa_re_split";
        case RtFn::RegexEscape: return "pa_re_escape";
        case RtFn::RegexPatternGroups: return "pa_re_pattern_groups";
        case RtFn::RegexPatternFlags: return "pa_re_pattern_flags";
        case RtFn::RegexPatternStr: return "pa_re_pattern_str";
        case RtFn::RegexFindIter: return "pa_re_finditer";
        case RtFn::RegexFindIterStr: return "pa_re_finditer_str";
        case RtFn::RegexExpand: return "pa_re_expand";
        case RtFn::RegexGroup: return "pa_re_group";
        case RtFn::RegexGroupStr: return "pa_re_group_str";
        case RtFn::RegexGroups: return "pa_re_groups";
        case RtFn::RegexGroupDict: return "pa_re_groupdict";
        case RtFn::RegexLastIndex: return "pa_re_lastindex";
        case RtFn::RegexLastGroup: return "pa_re_lastgroup";
        case RtFn::RegexStart: return "pa_re_start";
        case RtFn::RegexEnd: return "pa_re_end";
        case RtFn::RegexSpan: return "pa_re_span";
        case RtFn::DateNew: return "pa_date_new";
        case RtFn::DateTimeNew: return "pa_datetime_new";
        case RtFn::DateTimeNow: return "pa_datetime_now";
        case RtFn::DateTimeField: return "pa_datetime_field";
        case RtFn::DateTimeIsoformat: return "pa_datetime_isoformat";
        case RtFn::DateTimeStrftime: return "pa_datetime_strftime";
        case RtFn::TimeDeltaNew: return "pa_timedelta_new";
        case RtFn::TimeDeltaField: return "pa_timedelta_field";
        case RtFn::TimeDeltaTotalSeconds: return "pa_timedelta_total_seconds";
        case RtFn::TimeDeltaCmp: return "pa_timedelta_cmp";
        case RtFn::TimeDeltaAdd: return "pa_timedelta_add";
        case RtFn::TimeDeltaSub: return "pa_timedelta_sub";
        case RtFn::TimeDeltaMul: return "pa_timedelta_mul";
        case RtFn::TimeDeltaNeg: return "pa_timedelta_neg";
        case RtFn::TimeDeltaFloorDivInt: return "pa_timedelta_floordiv_int";
        case RtFn::TimeDeltaTrueDivInt: return "pa_timedelta_truediv_int";
        case RtFn::TimeDeltaFloorDivTD: return "pa_timedelta_floordiv_td";
        case RtFn::TimeDeltaTrueDivTD: return "pa_timedelta_truediv_td";
        case RtFn::TimeDeltaStr: return "pa_timedelta_str";
        case RtFn::DateTimeReplace: return "pa_datetime_replace";
        case RtFn::DateTimeAddDelta: return "pa_datetime_add_delta";
        case RtFn::DateTimeDiff: return "pa_datetime_diff";
        case RtFn::DateTimeCmp: return "pa_datetime_cmp";
        case RtFn::PathNew: return "pa_path_new";
        case RtFn::PathStr: return "pa_path_str";
        case RtFn::PathJoin: return "pa_path_join";
        case RtFn::PathName: return "pa_path_name";
        case RtFn::PathStem: return "pa_path_stem";
        case RtFn::PathSuffix: return "pa_path_suffix";
        case RtFn::PathParent: return "pa_path_parent";
        case RtFn::PathExists: return "pa_path_exists";
        case RtFn::PathIsFile: return "pa_path_is_file";
        case RtFn::PathIsDir: return "pa_path_is_dir";
        case RtFn::PathReadText: return "pa_path_read_text";
        case RtFn::PathWriteText: return "pa_path_write_text";
        case RtFn::PathIterdir: return "pa_path_iterdir";
        case RtFn::PathGlob: return "pa_path_glob";
        case RtFn::PathMkdir: return "pa_path_mkdir";
        case RtFn::PathWithSuffix: return "pa_path_with_suffix";
        case RtFn::PathParts: return "pa_path_parts";
        case RtFn::PathReadBytes: return "pa_path_read_bytes";
        case RtFn::PathWriteBytes: return "pa_path_write_bytes";
        case RtFn::PathIsAbsolute: return "pa_path_is_absolute";
        case RtFn::PathRelativeTo: return "pa_path_relative_to";
        case RtFn::PathResolve: return "pa_path_resolve";
        case RtFn::PathAbsolute: return "pa_path_absolute";
        case RtFn::PathTouch: return "pa_path_touch";
        case RtFn::RandRange: return "pa_random_randrange";
        case RtFn::RandGetrandbits: return "pa_random_getrandbits";
        case RtFn::RandGauss: return "pa_random_gauss";
        case RtFn::RandExpovariate: return "pa_random_expovariate";
        case RtFn::RandTriangular: return "pa_random_triangular";
        case RtFn::RandSample: return "pa_random_sample";
        case RtFn::RandChoices: return "pa_random_choices";
        case RtFn::OsGetpid: return "pa_os_getpid";
        case RtFn::OsChdir: return "pa_os_chdir";
        case RtFn::OsChmod: return "pa_os_chmod";
        case RtFn::OsAccess: return "pa_os_access";
        case RtFn::OsMakedirs: return "pa_os_makedirs";
        case RtFn::OsPathRealpath: return "pa_os_path_realpath";
        case RtFn::OsPathGetsize: return "pa_os_path_getsize";
        case RtFn::OsPathGetmtime: return "pa_os_path_getmtime";
        case RtFn::OsPathSplit: return "pa_os_path_split";
        case RtFn::OsPathSamefile: return "pa_os_path_samefile";
        case RtFn::DequeNew: return "pa_deque_new";
        case RtFn::DequeFromList: return "pa_deque_from_list";
        case RtFn::DequeAppend: return "pa_deque_append";
        case RtFn::DequeAppendLeft: return "pa_deque_appendleft";
        case RtFn::DequePop: return "pa_deque_pop";
        case RtFn::DequePopleft: return "pa_deque_popleft";
        case RtFn::DequeLen: return "pa_deque_len";
        case RtFn::DequeMaxlen: return "pa_deque_maxlen";
        case RtFn::DequeGet: return "pa_deque_get";
        case RtFn::DequeSet: return "pa_deque_set";
        case RtFn::DequeRotate: return "pa_deque_rotate";
        case RtFn::DequeClear: return "pa_deque_clear";
        case RtFn::DequeExtend: return "pa_deque_extend";
        case RtFn::DequeExtendLeft: return "pa_deque_extendleft";
        case RtFn::DequeToList: return "pa_deque_to_list";
        case RtFn::DequeRemove: return "pa_deque_remove";
        case RtFn::DequeCount: return "pa_deque_count";
        case RtFn::DequeIndex: return "pa_deque_index";
        case RtFn::DequeInsert: return "pa_deque_insert";
        case RtFn::DequeReverse: return "pa_deque_reverse";
        case RtFn::DequeCopy: return "pa_deque_copy";
    }
    return "?";
}

// 字符串方法的操作码，序号必须与 runtime/rt_obj.c 的 pa_str_method 一致。
enum class StrOp : int64_t {
    Upper, Lower, Strip, LStrip, RStrip, StripChars, LStripChars, RStripChars,
    SplitWs, SplitSep, Join, Replace, Find, RFind, StartsWith, EndsWith, Count,
    ZFill, LJust, RJust, Center, Capitalize, Title, IsDigit, IsAlpha, IsSpace,
    IsUpper, IsLower, IsAlphaNum, RemovePrefix, RemoveSuffix, SplitLines,
    Encode, Decode,
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
    GlobalAddr,  // dst = &globals[idx]（外部符号时是符号地址，不解引用）
    ClassAddr,   // dst = &classes[idx].vtable

    // ── 容器 ──────────────────────────────────────────────
    MakeList,   // dst = <ty>[](args...)
    MakeTuple,  // dst = (args...)
    IndexLoad,  // dst = a[b]
    IndexLoadUnchecked, // dst = a[b]，调用方已证明 0 <= b < len(a)，不检查
    IndexStore, // a[b] = c
    IndexStoreUnchecked, // a[b] = c，调用方已证明 0 <= b < len(a)，不检查
    Len,        // dst = len(a)

    // ── 对象 ──────────────────────────────────────────────
    ObjNew,     // dst = new <class imm>[n_fields]，头部写 vtable 指针
    FieldLoad,  // dst = a->field[imm]（imm 是字节偏移）
    FieldStore, // a->field[imm] = b
    PtrLoad,    // dst = *(a + b)；imm 是元素宽度（2/4/8 走比例寻址）
    PtrStore,   // *(a + b) = c；imm 是元素宽度
    VCall,      // dst = a->vtable[imm](a, args...)  第一个实参固定是 self
    VCallStatic,// dst = a->vtable[imm](args...)  静态方法不传 self
    SuperVCall, // dst = super_lookup(a, idx, imm)(a, args...)  沿运行期 MRO
    CallDirect, // dst = funcs[idx](args...)  直接调用裸函数（静态方法）
    CallPtr,    // dst = a(args...)  a 是裸代码地址（类对象 vtable 分派）

    // ── 值类型 ────────────────────────────────────────────
    StructNew,  // dst = pa_struct_new(imm)；imm 是 payload 字节数
    StructCopy, // dst = pa_struct_copy(a, imm)；按值语义复制一份 payload

    // ── 调用 ──────────────────────────────────────────────
    Call,    // dst = a(args...)（a 是函数指针）
    CallRt,  // dst = <RtFn(imm)>(args...)
    CallExtern, // dst = <externs[idx].symbol>(args...)；参数按 C ABI 编组
    LoadExtern, // dst = <extern_vars[idx].symbol>；按声明宽度读取 C 全局变量
    StoreExtern, // <extern_vars[idx].symbol> = a；按声明宽度写回
    Setjmp,  // dst = setjmp(a)（a 是 handler 记录，异常跳回时返回非 0）
    // ── 零开销异常（Z2-b2）：保护区 / landing pad 的位置标记 ───────
    // 都是「不产出机器码（只发标签）/ 只发 landing pad 入口序列」的标记
    // 指令，供 codegen 拼 .gcc_except_table 的 call-site 区间。它们没有
    // dst、没有副作用，不在 terminated() 里。
    EhBegin,  // 保护区起点：发 <begin> 标签
    EhEnd,    // 保护区终点（开区间）：发 <end> 标签
    EhLandingPad, // landing pad 入口：发 <lp> 标签；之后紧跟 CallRt EhLand
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
    GenTakeExc, // dst = 帧里待注入的异常指针，并把帧槽清零（所有权转移出去）
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
        case IrOp::GlobalAddr: return "global.addr";
        case IrOp::ClassAddr: return "class.addr";
        case IrOp::MakeList: return "list.new";
        case IrOp::MakeTuple: return "tuple.new";
        case IrOp::IndexLoad: return "index.get";
        case IrOp::IndexLoadUnchecked: return "index.get.unchecked";
        case IrOp::IndexStore: return "index.set";
        case IrOp::IndexStoreUnchecked: return "index.set.unchecked";
        case IrOp::Len: return "len";
        case IrOp::ObjNew: return "obj.new";
        case IrOp::FieldLoad: return "field.get";
        case IrOp::FieldStore: return "field.set";
        case IrOp::PtrLoad: return "ptr.load";
        case IrOp::PtrStore: return "ptr.store";
        case IrOp::VCall: return "vcall";
        case IrOp::VCallStatic: return "vcall.static";
        case IrOp::SuperVCall: return "vcall.super";
        case IrOp::CallDirect: return "call.direct";
        case IrOp::CallPtr: return "call.ptr";
        case IrOp::StructNew: return "struct.new";
        case IrOp::StructCopy: return "struct.copy";
        case IrOp::Call: return "call";
        case IrOp::CallRt: return "call.rt";
        case IrOp::CallExtern: return "call.extern";
        case IrOp::LoadExtern: return "load.extern";
        case IrOp::StoreExtern: return "store.extern";
        case IrOp::Setjmp: return "setjmp";
        case IrOp::EhBegin: return "eh.begin";
        case IrOp::EhEnd: return "eh.end";
        case IrOp::EhLandingPad: return "eh.landing.pad";
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
        case IrOp::GenTakeExc: return "gen.take.exc";
        case IrOp::GenRethrow: return "gen.rethrow";
        case IrOp::GenDone: return "gen.done";
        case IrOp::Jump: return "jump";
        case IrOp::Branch: return "branch";
        case IrOp::Return: return "return";
    }
    return "?";
}


}  // namespace pyadamas
