// ir_rtfn.h —— 运行时库函数编号（RtFn）与名字表
//
// ir_ops.h 的组成部分（纯代码移动）：在 ir_ops.h 的 namespace pyadamas
// 体内原位置 #include 展开，内容与顺序逐字不变。不要单独 include。

// 运行时库函数。名字固定，后端按枚举直接生成调用，
// 不经过符号查找，也不允许用户代码直接引用。
enum class RtFn : uint16_t {
    ListNew,     // (i64 cap) -> list*
    ListAppend,  // (list*, i64 v, i64 heap) -> void  heap: 0 非堆 / 1 堆 / 2 unknown
    ListGet,     // (list*, i64) -> i64
    ListSet,     // (list*, i64, i64, i64 heap) -> void（0.303 种类维护）
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
    PrintAnyRepr,  // (i64) -> void  容器内 unknown 元素：repr 语义
    TruthAny,      // (i64) -> i64   unknown 值的 Python 真值
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
    GcWriteBarrier, // (i64) -> void 增量标记插入屏障：标灰新值（0.330，门控发射）
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
    DictVersion,  // (dict*) -> i64  迭代版本号（键集合变化的计数器）
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
    ListSliceDel, // (list*, i64 start, i64 stop, i64 step) -> void  del list[a:b:c]
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
    ListInplaceRepeat,  // (list* l, i64 n) -> void；`l *= n` 原地重复（CPython __imul__）
    SetUnion,             // (set*, set*) -> set*
    SetIntersect,         // (set*, set*) -> set*
    SetDifference,        // (set*, set*) -> set*
    SetSymDiff,           // (set*, set*) -> set*
    SetIntersectUpdate,  // (set* s, set* t) -> void；`s &= t`
    SetDifferenceUpdate, // (set* s, set* t) -> void；`s -= t`
    SetSymdiffUpdate,    // (set* s, set* t) -> void；`s ^= t`
    ListSumI64,   // (list*, i64 start) -> i64
    ListSumF64,   // (list*, f64 start) -> f64
    ListMinI64,   // (list*) -> i64  空表抛 ValueError
    ListMaxI64,   // (list*) -> i64
    ListCopy,     // (list*) -> list*
    ListReversed, // (list*) -> list*  倒序副本
    ListSort,     // (list*, i64 kind, str* desc) -> void  原地排序
    SortedList,   // (list*, i64 kind, str* desc) -> list*  排序副本
    ListSortedBy, // (list* src, list* keys, i64 kind, str* desc) -> list*  按键排序的副本
                  // 0.312：kind 4 = 容器元素按 desc 字典序；标量种类 desc 传空串
    ListIndex,    // (list*, i64 v, i64 kind) -> i64  找不到抛 ValueError
    ListCount,    // (list*, i64 v, i64 kind) -> i64
    ListInsert,   // (list*, i64 idx, i64 v, i64 heap) -> void（0.303）
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
    ReprDictAny,  // (dict*, ) -> str*  异构 dict 的通用 repr（key/value 走 unknown repr）
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
    ExcValueTypeName, // (exc_value*) -> str*  （type(e).__name__ 用）
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
    // 同 Raise，但接管两个串的所有权：拷贝进异常对象后立刻 dec 它们。
    // 给「字符串存在临时槽里、抛出去之后没人能释放」的路径用（异步 __aexit__
    // 的异常分支：longjmp 之后槽既不会被清、临时栈也随挂起丢了）。
    RaiseOwn,     // (str* type, str* msg) -> void  noreturn
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
    JsonDumps,             // (i64 value, str* desc, i64 indent, i64 flags, list* seps) -> str*
    JsonLoads,             // (str* text, str* desc) -> i64
    // CPython 桥：(str* module, str* attr, list* args, str* descs) -> i64 unknown
    // 懒拉起 python3 worker 子进程，JSON 帧 IPC；见 runtime/rt_cpy.c。
    CpyCall,
    RegexCompile,          // (str* pat, i64 flags) -> regex*
    RegexSearchStr,        // (str* pat, str* text, i64 flags, i64 mode) -> match*
    RegexSearch,           // (regex*, str* text, i64 mode) -> match*
    RegexFindAllStr,       // (str* pat, str* text, i64 flags) -> list*
    RegexFindAllGroupsStr, // (str* pat, str* text, i64 flags) -> list[tuple] （>=2 捕获组）
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
    DYLIBLoad,             // (str* path) -> i64 dlopen 句柄
    DYLIBClose,            // (i64 handle) -> i64 dlclose 结果
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
    // 虚拟线程先验（runtime/rt_vthread.c，docs/vthread-design.md）
    VtSpawn,               // (i64 closure) -> vt*；接管闭包引用
    VtJoin,                // (vt*) -> void；阻塞让出，异常重抛
    VtSleep,               // (i64 ms) -> void；阻塞让出
    VtShutdown,            // () -> void；入口退出 drain
    ChanNew,               // (i64 elem_kind, i64 cap) -> chan*
    ChanSend,              // (chan*, i64 v) -> void；heap 种类接管引用
    ChanRecv,              // (chan*) -> i64；heap 种类交出引用
    ChanSendF,             // (chan*, f64 v) -> void
    ChanRecvF,             // (chan*) -> f64
    // unknown 域最小判别（runtime/rt_any.c，0.324）——只有静态类型是
    // unknown 的表达式会用这些入口，静态类型路径零调用。
    AnyFromF64,            // (f64) -> i64  float 装进 CPYFLOAT 盒，返回 unknown 字
    AnyIsFloat,            // (i64) -> i64  isinstance(unknown, float)
    AnyToF64,              // (i64) -> f64  unknown -> double（解盒或 int 提升）
    AnyToI64,              // (i64) -> i64  unknown -> int（解盒后截断）
    AnyArith,              // (op, a, b, desc_a, desc_b) -> i64 动态算术
    AnyNeg,                // (a, desc_a) -> i64 动态一元负
    AnyCmp,                // (op, a, b, desc_a, desc_b) -> i64 动态比较
    // 动态属性 dict 视图（vars() 合并用，stdlib1）——追加在末尾，不改既有编号。
    AttrDict,              // (obj*) -> dict*  实例动态属性表（可能 NULL，借用）
    AnyIterList,           // (i64) -> list*  unknown 物化成可迭代列表（+1）
    ReprAny,               // (i64) -> str*  repr(unknown)（pa_repr_any，+1）
    // P3 第一阶段（0.327）：shadow tag 版（docs/p3-tagged-union-design.md）。
    // tag 由 codegen 从平行 tag 区/静态类型装填；UNK(0) 回退探针。
    AnyArithT,             // (op, a, tag_a, b, tag_b, reuse_ok) -> {rax=i64, rdx=tag}
    AnyNegT,               // (a, tag_a, reuse_ok) -> {rax=i64, rdx=tag}
    AnyCmpT,               // (op, a, tag_a, b, tag_b) -> i64 bool
    AnyIsFloatT,           // (a, tag_a) -> i64
    AnyToF64T,             // (a, tag_a) -> f64
    AnyToI64T,             // (a, tag_a) -> i64
    AnyLoadTag,            // (i64) -> i64 tag  无 tag 来源的 unknown 字一次探针定 tag
    // stdlib2（0.329）：os.close(fd)——tempfile.mkstemp 的裸 fd 关闭。
    // 追加在末尾，不改既有编号（集成时按落地顺序重排）。
    OsClose,               // (i64 fd) -> void
    OsPathIslink,          // (str* path) -> i64 bool  lstat S_ISLNK（stdlib2）
    // P2（0.340，PYADAMAS_TYPE_TAGS）：isinstance(unknown, 内置类型)。
    // IR 实参 {v, code}：code 是 PaTypeCode（src/tag_defs.h）；codegen 把
    // tag 从平行 tag 区装填，实际调用 pa_any_type_is(v, tag, code)。
    AnyTypeIs,             // (a, tag_a, code) -> i64 bool
    AnyDictItems,          // (a, tag_a) -> list*  unknown dict 的 (k,v) 对（+1）
    // P1 定宽整数（0.341）：u64 的十进制输出。u64 的值 ≥ 2^63 时，
    // 有符号版本的 print/str 会打出负数——必须走无符号入口。
    // 追加在末尾，不改既有编号（集成时按落地顺序重排）。
    StrFromU64,            // (u64) -> str*
    PrintU64,              // (u64) -> void
    FmtU64,                // (u64, str* spec) -> str*
    UIntToBase,            // (u64, base) -> str*  hex()/bin()/oct() 的无符号版
    // P4 语言缺口（0.343，PYADAMAS_LANG_GAPS）：异常类当值 + 动态方法分派。
    // 追加在末尾，不改既有编号（集成时按落地顺序重排）。
    ExcClassObj,           // (str* name) -> class*  内建异常类单例（+1，CLASS tag）
    ExcClassIsA,           // (str* name, class*) -> i64  异常名是否属于该类（suppress）
    ExcSubclass,           // (class* a, class* b) -> i64  异常类对象间 issubclass
    ExcClassValueNew,      // (class*, str* msg|0) -> excval*  类对象值构造异常值
    ExcClassValueNewArgs,  // (class*, list* args) -> excval*  多参版
    MethodRegister,        // (str* qual, str* method, i64 slot) -> void  方法名→槽登记
    MethodCall0,           // (obj*, str* method) -> i64  动态零参方法调用（close 等）
    // P4 块3（0.346，PYADAMAS_LANG_GAPS）：traceback 对象 / sys.exc_info。
    // 追加在末尾，不改既有编号（集成时按落地顺序重排）。
    SysExecutable,         // () -> str*  sys.executable（argv[0]，编译产物路径）
    LangGapsEnable,        // () -> void  运行期门控置 1（入口模块体首发，门控开才发射）
    ExcInfo,               // () -> list*  sys.exc_info() 三元组（+1；outside except 全 None）
    TbFrames,              // (tb*) -> list*  __pa_tb_frames：list[(file,line,func)]（+1）
    ReprSet,               // (set*) -> str*  静态 set 的 repr（探针分不出 set/dict）
    ReprBytes,             // (bytes*) -> str*  静态 bytes 的 repr（bytes/str 同表示）
    // ── P3 big（0.357，docs/numeric-types-design.md §1/§4）──────────
    // 追加在末尾，不改既有编号。操作数是借用，返回值 rc=1（产生者引用）。
    BigFromI64,            // (i64) -> big*
    BigFromF64,            // (f64) -> big*  int(float) 向零截断
    BigFromStr,            // (str*, i64 base) -> big*
    BigToI64,              // (big*) -> i64  cast 语义：低 64 位回绕
    BigToF64,              // (big*) -> f64  正确舍入；过大 OverflowError
    BigAdd,                // (big*, big*) -> big*
    BigSub,                // (big*, big*) -> big*
    BigMul,                // (big*, big*) -> big*
    BigFloorDiv,           // (big*, big*) -> big*  向下取整；除零 ZeroDivisionError
    BigMod,                // (big*, big*) -> big*  余数与除数同号
    BigPow,                // (big*, big*) -> big*  负指数 ValueError（文档化边界）
    BigTrueDiv,            // (big*, big*) -> f64  正确舍入
    BigCmp,                // (big*, big*) -> i64  -1/0/1
    BigCmpF64,             // (big*, f64) -> i64  精确混比；NaN=-2
    BigEq,                 // (big*, big*) -> i64 bool
    BigTruth,              // (big*) -> i64 bool
    BigNeg,                // (big*) -> big*
    BigAbs,                // (big*) -> big*
    BigNot,                // (big*) -> big*  ~a = -a-1（无限补码）
    BigAnd,                // (big*, big*) -> big*
    BigOr,                 // (big*, big*) -> big*
    BigXor,                // (big*, big*) -> big*
    BigLShift,             // (big*, big* cnt) -> big*
    BigRShift,             // (big*, big* cnt) -> big*  算术右移（floor）
    BigStr,                // (big*) -> str*  十进制（repr 同形）
    BigToBase,             // (big*, i64 base) -> str*  2/8/10/16
    BigFmt,                // (big*, str* spec) -> str*  format()/f-string
    BigFromU64,            // (u64) -> big*
    AnyToBig,              // (i64 unknown) -> big*  探针分派：big 原样/int 加宽/float 截断
    AnyBigBinary,          // (op, a, b, desc_a, desc_b) -> unknown
    AnyInvert,             // (a, desc_a) -> unknown
};
#include "ir_rtfn_names.h"
