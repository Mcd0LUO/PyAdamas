// rt_eh.c —— 零开销异常（Z2）的运行时胶水：抛出、落地、方案 G 的帧清理
//
// 设计文档：docs/zero-cost-exceptions.md（Z2 路线）、
//           docs/exc-frame-cleanup.md（方案 G：在 handler 帧的 personality 里批量
//           释放被逻辑展开跳过的帧记录，而**不是**在 landing pad 里回头走帧链）。
//
// 与 rt_exc.c 的分工（两套机制共存，互不改写对方的逻辑）：
//   - rt_exc.c 仍是异常对象（PyAdamasException）、类型匹配、handler 记录栈、
//     setjmp/longjmp 路径的全部所有者；
//   - 本文件只负责「用 libgcc 的表驱动展开代替 longjmp」这一小段胶水：
//       pa_eh_throw   把异常装进 _Unwind_Exception 抛出去；
//       pa_eh_landing landing pad 入口把它取回来、装成与 longjmp 落地后完全一样
//                     的全局状态（所以 LP 里的分派代码一行不用改）；
//       pa_eh_done    处理完（命中 except / 抑制）后删掉展开包装，幂等；
//       pa_frame_unwind_to_cfa  方案 G 的批量帧释放，由 personality 在 phase 2
//                     的 _UA_HANDLER_FRAME 那次调用里、安装 LP 之前调用。
//
// 所有权约定（谁 malloc 谁 free；写清楚是为了避免两套路径互相 double free）：
//   - 展开包装 PaEhExc：本文件 malloc，exception_cleanup 里 free（由
//     _Unwind_DeleteException 触发，正常路径上就是 pa_eh_done 调的那次）。
//     它跨整个展开过程存活，绝不挂 RC/GC 链、不进 GC 扫描。
//   - 包装里的 exc / inst / args：**借住**。抛出时从 pending_exc /
//     current_exc_inst / pending_exc_args 三个全局搬进包装并发空全局，落地时
//     原样搬回去；所有权始终属于「异常机制」，由 rt_exc.c 原有的 exc_free /
//     pa_rc_dec 生命周期释放。本文件只在「没人接」的路径上把 exc 留给
//     pending_exc（调用方打印 traceback 后退出），绝不 exc_free 包装里的对象。
//
// 平台：零开销异常只在 gnu_cfi 目标（Linux x86-64）发射，Win64 走 SEH、不接这条
// 路径；但本文件随 rt.o 一起在所有平台编译，内容保持平台无关。

#include "rt_internal.h"

#include <stddef.h>
#include <unwind.h>

/* ── 展开包装 ─────────────────────────────────────────────────────
   Itanium ABI 要求传给 _Unwind_RaiseException 的对象以 struct _Unwind_Exception
   开头且按最大对齐（x86-64 = 16 字节）对齐；malloc 在 glibc 上天然满足。 */
#define PA_EH_WRAPPER_MAGIC 0x5041454857524150ULL /* "PAEHWRAP" */

typedef struct PaEhExc {
    struct _Unwind_Exception unwind; /* 必须首成员：libgcc 只认这个前缀 */
    uint64_t magic;                  /* PA_EH_WRAPPER_MAGIC，防误用 */
    PyAdamasException* exc;          /* owned(借住)：见文件头所有权约定 */
    void* inst;                      /* owned(借住)：富异常实例（RC 对象） */
    PyAdamasList* args;              /* owned(借住)：多参异常 args（RC 对象） */
} PaEhExc;

_Static_assert(offsetof(PaEhExc, unwind) == 0, "展开对象必须与 _Unwind_Exception 同址");
_Static_assert(_Alignof(PaEhExc) >= 16, "libgcc 要求展开对象 16 字节对齐");

/* 最近一次抛出、尚未收尾的展开包装（不透明；pa_eh_throw/landing/done 共用）。 */
void* pa_eh_current;

/* personality 里的帧清理钩子（定义在 runtime/rt_personality.c，那里自包含，
   所以这里自己声明）。构造函数把它装成本文件的实现（见文件尾）。 */
extern void (*pa_personality_frame_hook)(uint64_t ex_class, uintptr_t target_cfa);

#ifdef PA_PERSONALITY_TRACE
/* 测试观测点（与 rt_personality.c 的 pa_dbg_* 同一套开关）：z2_rt 夹具据此断言
   「抛出/落地/收尾次数」「释放了多少条帧记录」「包装有没有泄漏」。生产构建没有
   这些计数。 */
volatile int pa_dbg_eh_throws;
volatile int pa_dbg_eh_landings;
volatile int pa_dbg_eh_dones;
volatile int pa_dbg_eh_uncaught;
volatile int pa_dbg_eh_foreign;            /* 落到我们 LP 里的外来异常（不该有） */
volatile int pa_dbg_eh_wrappers_live;      /* 当前存活的展开包装数（泄漏判据） */
volatile int pa_dbg_frame_release_calls;   /* personality 触发批量释放的次数 */
volatile int pa_dbg_frames_released;       /* 累计释放的帧记录条数 */
volatile int pa_dbg_frame_release_skipped; /* 因 ex_class 不是我们的而跳过 */
#endif

/* ── 展开包装的分配与释放 ─────────────────────────────────────── */

static PaEhExc* pa_eh_wrapper_new(void) {
    PaEhExc* w = (PaEhExc*)malloc(sizeof(PaEhExc));
    if (!w) panic_c("out of memory");
    memset(w, 0, sizeof(PaEhExc));
    w->unwind.exception_class = PA_EH_EXC_CLASS;
    w->unwind.exception_cleanup = NULL; /* 下面填，保持先清零再赋值 */
    w->magic = PA_EH_WRAPPER_MAGIC;
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_eh_wrappers_live;
#endif
    return w;
}

/* _Unwind_Exception 的清理回调：只释放包装本身。
   包装里的 exc/inst/args 要么已经搬回全局（落地过），要么仍归 pending_exc
   （没人接），都不在这里释放——见文件头所有权约定。 */
static void pa_eh_exception_cleanup(_Unwind_Reason_Code reason,
                                    struct _Unwind_Exception* unwound) {
    PaEhExc* w = (PaEhExc*)unwound;
    (void)reason;
    if (!w || w->magic != PA_EH_WRAPPER_MAGIC) return;
    w->magic = 0;
    free(w);
#ifdef PA_PERSONALITY_TRACE
    --pa_dbg_eh_wrappers_live;
#endif
}

/* ── 抛出 ───────────────────────────────────────────────────────── */

int pa_eh_throw(PyAdamasException* e) {
    _Unwind_Reason_Code rc;
    PaEhExc* w;

    if (!e) panic_c("pa_eh_throw: 空异常对象");

    /* 上一个还没收尾的包装：先删掉（它要么已经落地、载荷已搬回全局，要么是
       没人接的上一轮），避免包装泄漏。 */
    if (pa_eh_current) pa_eh_done();

    w = pa_eh_wrapper_new();
    w->unwind.exception_cleanup = pa_eh_exception_cleanup;
#if !defined(__SEH__)
    /* 展开器内部用这两个字（phase 1 记 handler 帧 CFA 等）：抛前必须清零。
       Win64 的 SEH 版本没有 private_1/private_2（是 private_[6]），整结构体
       已经 memset 过，零值同样成立；这条路径在 Windows 上本来也不会走
       （零开销异常只在 gnu_cfi 目标发射）。 */
    w->unwind.private_1 = 0;
    w->unwind.private_2 = 0;
#endif

    /* 接管异常本体。两种调用习惯都接受：调用方已经把它设进 pending_exc，或者
       只把所有权传进来（这里替它设）。前一种不要重复 free。 */
    if (pending_exc == e) {
        pending_exc = NULL;
    } else {
        exc_free(pending_exc);
    }
    pending_exc = e;
    w->exc = e;

    /* 富异常实例 / 多参 args 是 raise 时挂在全局上的旁路载荷：搬进包装，落地时
       再搬回去。这样展开过程中即使别处读写这两个全局（理论上不会），落地后
       状态也只由包装决定。 */
    w->inst = current_exc_inst;
    current_exc_inst = NULL;
    w->args = pending_exc_args;
    pending_exc_args = NULL;

    pa_eh_current = w;
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_eh_throws;
#endif

    rc = _Unwind_RaiseException(&w->unwind);

    /* 走到这里说明没有 landing pad 被安装：没人接，或者展开器内部报错。
       成功时控制已经跳到 LP，本函数不会返回。 */
    if (rc == _URC_END_OF_STACK) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_eh_uncaught;
#endif
        return PA_EH_UNCAUGHT;
    }
    return PA_EH_FATAL;
}

/* ── 落地 ───────────────────────────────────────────────────────── */

PyAdamasException* pa_eh_landing(void* unwound) {
    PaEhExc* w = (PaEhExc*)unwound;

    if (!w) panic_c("pa_eh_landing: 空展开对象指针");
    /* 先看 exception_class 再碰 magic：外来异常（C++ 等）只有 _Unwind_Exception
       前缀保证存在，后面的 payload 不是我们能读的。 */
    if (w->unwind.exception_class != PA_EH_EXC_CLASS) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_eh_foreign;
#endif
        return NULL;
    }
    if (w->magic != PA_EH_WRAPPER_MAGIC) panic_c("pa_eh_landing: 包装魔数不对");

    /* 把载荷搬回全局，构成与 longjmp 落地后逐字一致的状态：
         pending_exc       = 刚抛出的异常（分派代码 pa_exc_matches/accept 读它）
         current_exc_inst  = 富异常实例（except as e 的绑定对象）
         pending_exc_args  = 多参异常 args
         current_exc       = NULL
         exc_save_top      = NULL
       */
    /* current_exc / exc_save_top 不动：展开过程中没有执行任何生成代码，
       它们仍是抛出时的值 —— 正是 longjmp 落地后 LP 应该看到的那份。
       （z2_rt 夹具对这两个全局有断言，改动会破坏它。） */
    if (pending_exc && pending_exc != w->exc) exc_free(pending_exc);
    pending_exc = w->exc;
    w->exc = NULL;
    if (current_exc_inst) pa_rc_dec(current_exc_inst);
    current_exc_inst = w->inst;
    w->inst = NULL;
    if (pending_exc_args) pa_rc_dec(pending_exc_args);
    pending_exc_args = w->args;
    w->args = NULL;

    pa_eh_current = w;
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_eh_landings;
#endif
    return pending_exc;
}

void pa_eh_done(void) {
    PaEhExc* w = (PaEhExc*)pa_eh_current;
    if (!w) return; /* 幂等：没抛过 / 已经收过尾 */
    pa_eh_current = NULL;
    if (w->magic != PA_EH_WRAPPER_MAGIC) panic_c("pa_eh_done: 包装魔数不对");
#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_eh_dones;
#endif
    /* 载荷此时必然已经搬回全局（或从未搬出），exception_cleanup 只 free 包装。 */
    _Unwind_DeleteException(&w->unwind);
}

/* ── 方案 G：帧清理 ─────────────────────────────────────────────── */

static int in_frame_release;

/* 单帧释放：从 rt_exc.c 的 pa_frame_unwind 内层循环逐字搬过来（不要在这里新写
   语义）。槽地址低两位是标志：bit1 = 只给 GC 扫描的借来引用，不释放；bit0 =
   值可能非堆，释放走注册表。先清零再 dec：析构器若触发 GC，未处理的槽仍是
   GC 可见的根，同一份引用也不会被二次释放。 */
static void pa_eh_release_frame_slots(PyAdamasFrame* f) {
    for (int64_t i = 0; i < f->n; ++i) {
        uintptr_t addr = (uintptr_t)f->slots[i];
        if (addr & 2) continue;
        int unknown = (int)(addr & 1);
        int64_t* p = (int64_t*)(addr & ~(uintptr_t)3);
        int64_t v = *p;
        if (!v) continue;
        *p = 0;
        if (unknown)
            pa_rc_dec_unknown(v);
        else
            pa_rc_dec((void*)(uintptr_t)v);
    }
}

void pa_frame_unwind_to_cfa(uintptr_t target_cfa) {
    int64_t released = 0;

    /* 重入 guard（docs/exc-frame-cleanup.md §5.3）：释放过程中若又发生一次展开，
       会读到半释放状态的帧链，结果不可预期。当前语义下不会发生（析构路径不跑
       用户代码、不 raise），所以重入一律按「前提被破坏」报错，不静默。 */
    if (in_frame_release)
        panic_c("pa_frame_unwind_to_cfa: 帧释放过程中又发生异常展开（重入）");
    in_frame_release = 1;

    /* CFA 随栈深度严格递减（越深越小）：比 handler 帧深、且还在链上的记录就是
       被逻辑展开跳过、没人负责释放的那些。
       **停止条件是 <= 而不是 <**：personality 在 phase 2 收到的
       _Unwind_GetCFA(ctx) 不是 handler 帧自己的 DWARF CFA，而是 libgcc 的
       context->cfa —— 它此时还是「handler 帧的直接被调帧（更深的那个帧）的
       CFA」，数值上等于 handler 帧 call site 的 rsp。本夹具 2026-11-16 实测：
       handler 帧真 CFA = rec+112，personality 拿到的是 rec+0；z1 夹具帧只有
       8 字节时两者恰好差 16，所以以前看起来「像」自己的 CFA。于是：
         - handler 帧自己的记录 cfa > target（保留，槽由 handler 正常退出释放）；
         - handler 帧直接调用的那个帧的记录 cfa == target（必须释放）；
         - 更深帧的记录 cfa < target（释放）。
       即「cfa <= target 的记录恰好就是『比 handler 帧记录更深』的全部记录」，
       与老世界 pa_frame_unwind(handler_top->frame) 的释放集合逐项相同。 */
    while (pa_frame_top && pa_frame_top->cfa <= target_cfa) {
        PyAdamasFrame* f = pa_frame_top;
        pa_eh_release_frame_slots(f);
        /* 一帧全部槽处理完再摘链：见 pa_frame_unwind 的同一注释。 */
        pa_frame_top = f->prev;
        ++released;
    }

    in_frame_release = 0;

    /* 语义不变量：释放完要么链空，要么栈顶是 handler 帧自己的记录或更外层的记录
       （两者的 cfa 都严格大于 target）。出现 cfa <= target 的记录说明 cfa 记账坏
       了，不能静默——那意味着有槽没释放，异常路径会无界泄漏。 */
    if (pa_frame_top && pa_frame_top->cfa <= target_cfa)
        panic_c("pa_frame_unwind_to_cfa: 帧链 cfa 未单调（释放不完整）");

#ifdef PA_PERSONALITY_TRACE
    ++pa_dbg_frame_release_calls;
    pa_dbg_frames_released += (int)released;
#endif
}

/* personality 装进来的钩子：先做 ex_class 过滤，再释放。
   只对自己的异常碰 pa_frame_top 链：C++/foreign 异常穿过我们的帧时，帧记录不是
   为它准备的（虽然链本身是我们的，但释放时机与 Z4 的 cleanup pad 语义还没接），
   保守跳过并计数，等 Z3/Z4 再处理。 */
static void pa_eh_phase2_frame_release(uint64_t ex_class, uintptr_t target_cfa) {
    if (ex_class != PA_EH_EXC_CLASS) {
#ifdef PA_PERSONALITY_TRACE
        ++pa_dbg_frame_release_skipped;
#endif
        return;
    }
    pa_frame_unwind_to_cfa(target_cfa);
}

/* personality 是本文件之外自包含编译的（tests/exc/z1_rt 单独链接它），所以用
   构造函数把实现装进它导出的钩子变量，而不是弱符号/链接期绑定：没链接本文件
   时钩子保持 NULL，personality 只做 ABI 安装、不做清理。构造函数在 main 之前
   跑，任何一次 raise 都晚于它。 */
static void __attribute__((constructor)) pa_eh_install_personality_hook(void) {
    pa_personality_frame_hook = pa_eh_phase2_frame_release;
}

/* ── codegen 接线（Z2-b2，--zero-cost-exc）─────────────────────────────
 *
 * 生成代码只在「零开销 try 体」里调用下面这些入口：
 *   pa_eh_raise / pa_eh_raise_value_from / pa_eh_reraise  抛（_Unwind 版）
 *   pa_eh_rethrow   landing 分发没有 handler 匹配时重抛同一个异常（重启 phase 1）
 *   pa_eh_land      landing pad 入口：取回异常并收掉展开包装
 *
 * 为什么必须有这一层：旧的长跳版本 pa_raise* 最后一步是 exc_longjmp，而零开销
 * 函数里没有 setjmp 兜底 —— 一旦 longjmp 就会穿过一个没有 handler 记录的帧，
 * 落点是错的（甚至直接按未捕获退出）。所以这里逐项复刻 rt_exc.c 的「构造异常
 * 对象 + 设全局」部分，只把最后一步换成 pa_eh_throw。异常对象的所有权 /
 * 生命周期规则完全不变（见 rt_exc.c 文件头）。
 *
 * 已知限制（第一刀，见 tests/exc/z2_codegen/README.md）：只有**生成代码里的
 * raise** 会走这里；C 运行时自己的 pa_raise_c 仍走 exc_longjmp（rt_exc.c 不在
 * 本文件职责内），所以新路径 try 还抓不到 IndexError / ZeroDivisionError 这类
 * 「运行时 C 代码内部抛出来」的异常。 */

/* 抛出去；没人接（_URC_END_OF_STACK）时复刻 exc_longjmp 的无 handler 分支：
   打印 traceback 后 exit(1)，不返回。 */
static _Noreturn void pa_eh_throw_or_exit(PyAdamasException* e) {
    int rc = pa_eh_throw(e);
    if (rc == PA_EH_UNCAUGHT) {
        exc_print_uncaught(pending_exc ? pending_exc : e);
        exit(1);
    }
    panic_c("pa_eh_throw: _Unwind_RaiseException 内部错误");
    exit(1); /* panic_c 的头文件声明不是 noreturn，补一条不可达兜底 */
}

/* 与 rt_exc.c 的 pa_raise 逐句对应，只把 exc_longjmp 换成抛展开。 */
_Noreturn void pa_eh_raise(PyAdamasStr* type, PyAdamasStr* msg) {
    char tn[128];
    int64_t tn_len = type->len;
    PyAdamasException* e;
    if (tn_len < 0 || tn_len >= (int64_t)sizeof(tn)) panic_c("exception type name too long");
    memcpy(tn, type->data, (size_t)tn_len);
    tn[tn_len] = '\0';
    exc_free(pending_exc);
    e = (PyAdamasException*)malloc(sizeof(PyAdamasException));
    if (!e) panic_c("out of memory");
    e->type = (char*)malloc((size_t)tn_len + 1);
    e->message = (char*)malloc((size_t)msg->len + 1);
    if (!e->type || !e->message) panic_c("out of memory");
    memcpy(e->type, tn, (size_t)tn_len + 1);
    memcpy(e->message, msg->data, (size_t)msg->len);
    e->message[msg->len] = '\0';
    e->cause = NULL;
    e->trace = NULL;
    pending_exc = e;
    pa_eh_throw_or_exit(e);
}

/* raise <异常值> / raise ... from ...：与 rt_exc.c 的 pa_raise_value_from 对应。
   pa_exc_from_value 自己会登记异常载荷的 GC 根，这里只搬控制流。 */
_Noreturn void pa_eh_raise_value_from(PyAdamasExcValue* v, PyAdamasExcValue* cause) {
    exc_free(pending_exc);
    pending_exc = pa_exc_from_value(v);
    if (cause) {
        exc_free(pending_exc->cause);
        pending_exc->cause = pa_exc_from_value(cause);
    }
    pa_eh_throw_or_exit(pending_exc);
}

/* 裸 raise：与 rt_exc.c 的 pa_reraise 逐句对应。 */
_Noreturn void pa_eh_reraise(void) {
    if (current_exc) {
        exc_free(pending_exc);
        pending_exc = current_exc;
        current_exc = NULL;
    } else {
        exc_free(pending_exc);
        pending_exc = exc_new("RuntimeError", "No active exception to reraise");
    }
    pa_eh_throw_or_exit(pending_exc);
}

/* landing 分发没有匹配上（例如 except Exception 遇到 SystemExit）：把已经搬回
   pending_exc 的同一个异常重新交给展开器，phase 1 会继续往外找 handler。 */
_Noreturn void pa_eh_rethrow(void) {
    if (!pending_exc) panic_c("pa_eh_rethrow: 没有待重抛的异常");
    pa_eh_throw_or_exit(pending_exc);
}

/* landing pad 入口：personality 把展开包装指针放进 GR0（x86-64 = %rax）。
   取回异常（装成与 longjmp 落地后逐项一致的全局状态）后立刻收掉包装：载荷已经
   搬回 pending_exc / current_exc_inst / pending_exc_args，包装只剩壳，删掉不会
   影响后续 ExcMatches / ExcAcceptValue / ExcDone 的任何行为。 */
void pa_eh_land(void* unwound) {
    PyAdamasException* e = pa_eh_landing(unwound);
    if (!e) panic_c("pa_eh_land: 外来异常落到 PyAdamas landing pad（Z2 不支持）");
    pa_eh_done();
}

