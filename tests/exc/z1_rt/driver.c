/* driver.c —— z1 夹具的宿主：构造 struct _Unwind_Exception、调用
 * _Unwind_RaiseException，然后断言手写汇编 landing pad 真的被展开器跳到了。
 *
 * 编译要求（见 run.sh）：
 *   gcc -O2 -funwind-tables -c driver.c
 * -funwind-tables 保证 driver 自己的每一帧都有完整 .eh_frame，
 * 否则展开器从汇编帧往外走时会在这里撞墙（第 3 步“中间帧被静默穿过”
 * 的前提就是每一帧都能被 .eh_frame 描述）。
 */
#include <unwind.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 观测点：汇编 landing pad / personality 写，这里断言 ─────────── */
volatile int z1_raise_rc;              /* _Unwind_RaiseException 的返回码（没人接管时） */

volatile uintptr_t z1_step1_seen_exc;  /* 第一步 landing pad 收到的 GR0 */
volatile int z1_step1_seen_lp;

volatile uintptr_t z1_step2_seen_exc;  /* 第二步 landing pad 收到的 GR0 */
volatile int z1_step2_seen_lp;

/* 第三步：landing pad 的执行顺序 / 收到的异常指针 / B 里改过的状态 */
int z1_order[8];
volatile int z1_order_n;
volatile uintptr_t z1_step3_cleanup_exc;
volatile uintptr_t z1_step3_handler_exc;
volatile int z1_step3_b_modified;

volatile int z1_broken_lp_hit;         /* 坏 LSDA 用例里 LP 不该被执行 */

/* 第一步 stub personality 的计数器（stub_personality.c）。 */
extern volatile int z1_stub_calls;
extern volatile int z1_stub_search_calls;
extern volatile int z1_stub_cleanup_calls;
extern volatile int z1_stub_handler_frame;
extern volatile int z1_stub_found;
extern volatile int z1_stub_installs;

#ifdef PA_PERSONALITY_TRACE
/* 第二步起 pa_personality 内部观测点（runtime/rt_personality.c）。 */
extern volatile int pa_dbg_calls;
extern volatile int pa_dbg_search;
extern volatile int pa_dbg_cleanup;
extern volatile int pa_dbg_handler_frames;
extern volatile int pa_dbg_installs;
extern volatile int pa_dbg_continue;
extern volatile int pa_dbg_errors;
extern volatile int pa_dbg_force_unwind;
extern volatile int pa_dbg_setip_calls;
extern volatile int pa_dbg_verbose;
static void trace_reset(void) {
    pa_dbg_calls = pa_dbg_search = pa_dbg_cleanup = 0;
    pa_dbg_handler_frames = pa_dbg_installs = pa_dbg_continue = pa_dbg_errors = 0;
    pa_dbg_force_unwind = pa_dbg_setip_calls = 0;
}
#else
static void trace_reset(void) {}
#endif

/* rt_personality.c 的解码错误观测点（该文件自包含，没有对外头文件，
   这里镜像它的错误码枚举，改那边时记得同步）。 */
extern int pa_personality_last_error;
extern const char *pa_personality_error_str(int code);
enum {
    Z1_LSDA_ERR_ENCODING = 1,
    Z1_LSDA_ERR_TRUNCATED = 2,
    Z1_LSDA_ERR_OVERFLOW = 3,
    Z1_LSDA_ERR_NO_LSDA = 4
};

/* 汇编夹具入口。 */
extern int z1_step1_fn(struct _Unwind_Exception *exc);
extern int z1_step2_fn(struct _Unwind_Exception *exc);
extern int z1_broken_fn(struct _Unwind_Exception *exc);
extern int z1_step3_handler_fn(struct _Unwind_Exception *exc);

/* ── 异常对象 ─────────────────────────────────────────────────────── */
/* Itanium ABI 要求异常对象 16 字节对齐；struct _Unwind_Exception 本身
   32 字节（4 个 8 字节字段），自定义载荷跟在后面。 */
typedef struct {
    struct _Unwind_Exception u;
    int payload;
} z1_exc;

#define Z1_EXC_CLASS 0x5a31455843303100ULL  /* 自定义异常类魔数 */

static volatile int exc_cleanup_calls;
static void exc_cleanup(_Unwind_Reason_Code reason, struct _Unwind_Exception *e) {
    (void)reason;
    (void)e;
    ++exc_cleanup_calls;
}

static z1_exc g_exc __attribute__((aligned(16)));

static struct _Unwind_Exception *exc_init(void) {
    memset(&g_exc, 0, sizeof g_exc);
    g_exc.u.exception_class = Z1_EXC_CLASS;
    g_exc.u.exception_cleanup = exc_cleanup;
    g_exc.payload = 0x1234;
    /* private_1/private_2 必须清零：展开器用 private_1 记录“是否已经在
       phase 1 找到 handler”，残留值会让 phase 2 行为错乱。 */
    g_exc.u.private_1 = 0;
    g_exc.u.private_2 = 0;
    return &g_exc.u;
}

/* C 侧抛出入口：汇编保护区里 call 它。_Unwind_RaiseException 成功找到
   handler 时不会返回；只有没人接管才返回 _URC_END_OF_STACK。
   单独包一层而不是直接在汇编里 call _Unwind_RaiseException，是为了让
   “抛点”这一帧是普通 C 帧（有 .eh_frame、无 personality），顺带验证
   展开器能跨过它。 */
void z1_raise_c(struct _Unwind_Exception *exc) {
    z1_raise_rc = (int)_Unwind_RaiseException(exc);
}

/* 供第二步夹具在保护区里调用：验证 _Unwind_Backtrace（强制展开）穿过
   带 personality 的汇编帧时，personality 必须返回 CONTINUE_UNWIND、
   不能安装 landing pad。真实运行时的 traceback（rt_debug.c）依赖这条路。 */
static int bt_frames;
static int bt_lp_seen;
static int bt_force_calls;      /* 强制展开期间走到 _UA_FORCE_UNWIND 分支的次数 */
static int bt_setip_calls;      /* 强制展开期间 _Unwind_SetIP 被调用的次数（必须为 0） */
static int fu_frames;           /* _Unwind_ForcedUnwind 的 stop 回调次数 */
static int fu_rc;               /* _Unwind_ForcedUnwind 的返回值 */
static int fu_force_calls;      /* 强制展开期间 personality 走 FORCE_UNWIND 的次数 */
static int fu_setip_calls;      /* 强制展开期间 _Unwind_SetIP 的次数（必须为 0） */
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg) {
    (void)ctx;
    (void)arg;
    ++bt_frames;
    if (z1_step2_seen_lp)
        bt_lp_seen = 1;
    return _URC_NO_REASON;
}
/* _Unwind_ForcedUnwind 的 stop 回调：一直返回继续，让展开走到底。
   我们只要它把沿路带 personality 的帧都过一遍（FORCE_UNWIND 路径）。 */
static _Unwind_Reason_Code fu_stop(int version, _Unwind_Action actions,
                                   _Unwind_Exception_Class ex_class,
                                   struct _Unwind_Exception *exc,
                                   struct _Unwind_Context *ctx, void *arg) {
    (void)version; (void)actions; (void)ex_class; (void)exc; (void)ctx; (void)arg;
    ++fu_frames;
    return _URC_NO_REASON;
}

/* 从汇编保护区里调用：触发真正的强制展开（_Unwind_Backtrace 不调用
   personality，只有 _Unwind_ForcedUnwind 才会带 _UA_FORCE_UNWIND 调）。
   期望：personality 对每个带 personality 的帧返回 CONTINUE，不安装 LP，
   所以整个函数返回 _URC_END_OF_STACK。 */
void z1_forced_probe(uintptr_t exc_addr) {
    struct _Unwind_Exception *e = (struct _Unwind_Exception *)exc_addr;
    fu_frames = 0;
    fu_rc = (int)_Unwind_ForcedUnwind(e, fu_stop, NULL);
#ifdef PA_PERSONALITY_TRACE
    fu_force_calls = pa_dbg_force_unwind;
    fu_setip_calls = pa_dbg_setip_calls;
#endif
}

void z1_trace_probe(void) {
    bt_frames = 0;
    bt_lp_seen = 0;
    _Unwind_Backtrace(bt_cb, NULL);
#ifdef PA_PERSONALITY_TRACE
    /* 快照：强制展开结束、真正抛异常之前的计数 */
    bt_force_calls = pa_dbg_force_unwind;
    bt_setip_calls = pa_dbg_setip_calls;
#endif
}

/* 汇编 landing pad 调用的小工具：记录执行顺序、收到的 exception 指针。 */
void z1_note(int tag, uintptr_t exc) {
    if (z1_order_n < (int)(sizeof z1_order / sizeof z1_order[0]))
        z1_order[z1_order_n] = tag;
    ++z1_order_n;
    if (tag == 1) {          /* cleanup 层：顺手改一点状态，证明 resume 前做过事 */
        z1_step3_cleanup_exc = exc;
        ++z1_step3_b_modified;
    } else if (tag == 2) {   /* handler 层 */
        z1_step3_handler_exc = exc;
    }
}

/* ── 断言小框架 ───────────────────────────────────────────────────── */
static int g_checks;
static int g_fails;

static void check(int cond, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ++g_checks;
    if (cond) {
        printf("PASS: %s\n", msg);
        return;
    }
    ++g_fails;
    printf("FAIL: %s\n", msg);
}

static void reset_step3(void) {
    memset(z1_order, 0, sizeof z1_order);
    z1_order_n = 0;
    z1_step3_cleanup_exc = 0;
    z1_step3_handler_exc = 0;
    z1_step3_b_modified = 0;
}

/* ── 测试 0：反例，没有 handler 时必须返回 _URC_END_OF_STACK ─────── */
static void test_end_of_stack(void) {
    struct _Unwind_Exception *e = exc_init();
    z1_stub_calls = z1_stub_search_calls = z1_stub_cleanup_calls = 0;
    z1_stub_handler_frame = z1_stub_found = z1_stub_installs = 0;
    z1_raise_rc = 0;
    z1_raise_c(e);  /* 栈上没有任何认领的帧 */
    check(z1_raise_rc == _URC_END_OF_STACK,
          "无 handler 时 _Unwind_RaiseException 返回 _URC_END_OF_STACK（实际 %d）",
          z1_raise_rc);
    check(z1_stub_cleanup_calls == 0,
          "无 handler 时 phase 2 根本不会开始（stub cleanup 调用 %d 次）",
          z1_stub_cleanup_calls);
    _Unwind_DeleteException(e);
}

/* ── 测试 1：第一步，无 LSDA 的 plumbing ─────────────────────────── */
static void test_step1_plumbing(void) {
    struct _Unwind_Exception *e = exc_init();
    exc_cleanup_calls = 0;
    z1_step1_seen_lp = 0;
    z1_step1_seen_exc = 0;
    z1_stub_calls = z1_stub_search_calls = z1_stub_cleanup_calls = 0;
    z1_stub_handler_frame = z1_stub_found = z1_stub_installs = 0;

    int rc = z1_step1_fn(e);
    check(rc == 42, "第一步：landing pad 返回 42（实际 %d）", rc);
    check(z1_step1_seen_lp == 1, "第一步：汇编 landing pad 执行了一次");
    check(z1_step1_seen_exc == (uintptr_t)e,
          "第一步：landing pad 收到 _Unwind_SetGR(GR0) 传入的 exception 指针");
    check(z1_stub_search_calls >= 1, "第一步：personality 在 _UA_SEARCH_PHASE 被调用");
    check(z1_stub_found == 1, "第一步：personality 恰好返回一次 _URC_HANDLER_FOUND");
    check(z1_stub_cleanup_calls == 1, "第一步：personality 在 _UA_CLEANUP_PHASE 恰好被调用一次");
    check(z1_stub_handler_frame == 1, "第一步：cleanup phase 带 _UA_HANDLER_FRAME");
    check(z1_stub_installs == 1, "第一步：personality 返回过一次 _URC_INSTALL_CONTEXT");
    _Unwind_DeleteException(e);
    check(exc_cleanup_calls == 1, "第一步：_Unwind_DeleteException 调用了 exception_cleanup");
}

/* ── 测试 2：第二步，真 LSDA + 真 pa_personality ─────────────────── */
static void test_step2_real_lsda(void) {
    struct _Unwind_Exception *e = exc_init();
    exc_cleanup_calls = 0;
    z1_step2_seen_lp = 0;
    z1_step2_seen_exc = 0;
    pa_personality_last_error = 0;
    trace_reset();

    int rc = z1_step2_fn(e);
    check(rc == 7, "第二步：landing pad 返回 7（实际 %d）", rc);
    check(z1_step2_seen_lp == 1, "第二步：LSDA 指定的 landing pad 执行了一次");
    check(z1_step2_seen_exc == (uintptr_t)e,
          "第二步：landing pad 收到 GR0 传进来的 exception 指针");
    check(pa_personality_last_error == 0, "第二步：LSDA 解码无错误");
    check(bt_frames >= 5, "第二步：_Unwind_Backtrace 从保护区穿过 %d 帧（强制展开路径）",
          bt_frames);
    check(bt_lp_seen == 0, "第二步：_Unwind_Backtrace 期间 personality 没有安装 landing pad");
    _Unwind_DeleteException(e);
    check(exc_cleanup_calls == 1, "第二步：exception_cleanup 被调用一次");

#ifdef PA_PERSONALITY_TRACE
    check(fu_frames >= 5, "第二步：_Unwind_ForcedUnwind 的 stop 回调走了 %d 帧", fu_frames);
    check(fu_rc == _URC_END_OF_STACK,
          "第二步：_Unwind_ForcedUnwind 因没人安装上下文返回 _URC_END_OF_STACK（实际 %d）", fu_rc);
    check(fu_force_calls >= 1,
          "第二步：personality 在 _UA_FORCE_UNWIND 下被调用 %d 次", fu_force_calls);
    check(fu_setip_calls == 0,
          "第二步：_UA_FORCE_UNWIND 下 personality 没有调用 _Unwind_SetIP（traceback 安全）");
    (void)bt_force_calls;
    (void)bt_setip_calls;
    check(pa_dbg_search >= 1, "第二步：pa_personality 走过 SEARCH phase");
    check(pa_dbg_handler_frames == 1, "第二步：恰好一帧被标记 _UA_HANDLER_FRAME");
    check(pa_dbg_installs == 1, "第二步：恰好安装一次 landing pad 上下文");
    check(pa_dbg_errors == 0, "第二步：personality 没有报告解码错误");
#endif
}

/* ── 测试 3：负向，LSDA 里改坏一个字节 ───────────────────────────── */
static void test_broken_lsda(void) {
    struct _Unwind_Exception *e = exc_init();
    z1_raise_rc = 0;
    z1_broken_lp_hit = 0;
    pa_personality_last_error = 0;
    trace_reset();

    int rc = z1_broken_fn(e);
    check(rc == 0, "坏表用例：判错后函数沿正常路径返回 0（实际 %d）", rc);
    check(z1_raise_rc == _URC_FATAL_PHASE1_ERROR,
          "坏表用例：_Unwind_RaiseException 返回 _URC_FATAL_PHASE1_ERROR（实际 %d）",
          z1_raise_rc);
    check(pa_personality_last_error == Z1_LSDA_ERR_ENCODING,
          "坏表用例：pa_personality 报出解码错误码 %d（%s）",
          pa_personality_last_error,
          pa_personality_error_str(pa_personality_last_error));
    check(z1_broken_lp_hit == 0, "坏表用例：landing pad 没有被执行");
    _Unwind_DeleteException(e);

#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_errors == 1, "坏表用例：personality 报错一次");
#endif
}

/* ── 测试 4：第三步，两阶段展开（cleanup 先、handler 后） ────────── */
static void test_step3_two_phase(void) {
    struct _Unwind_Exception *e = exc_init();
    exc_cleanup_calls = 0;
    pa_personality_last_error = 0;
    reset_step3();
    trace_reset();

    int rc = z1_step3_handler_fn(e);
    check(rc == 100, "第三步：外层 handler 的 landing pad 正常返回 100（实际 %d）", rc);
    check(z1_order_n == 2, "第三步：恰好两个 landing pad 执行（实际 %d 个）", z1_order_n);
    check(z1_order[0] == 1 && z1_order[1] == 2,
          "第三步：cleanup 帧 (B) 先跑、handler 帧 (A) 后跑，顺序正确");
    check(z1_step3_cleanup_exc == (uintptr_t)e,
          "第三步：cleanup landing pad 收到 exception 指针（_Unwind_Resume 用它继续展开）");
    check(z1_step3_handler_exc == (uintptr_t)e,
          "第三步：handler landing pad 收到 exception 指针");
    check(z1_step3_b_modified == 1,
          "第三步：cleanup landing pad 里改了状态再 _Unwind_Resume");
    check(pa_personality_last_error == 0, "第三步：LSDA 解码无错误");
    _Unwind_DeleteException(e);
    check(exc_cleanup_calls == 1, "第三步：exception_cleanup 被调用一次");

#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_installs == 2, "第三步：恰好安装两次上下文（cleanup 一次 + handler 一次）");
    check(pa_dbg_handler_frames == 1, "第三步：只有外层 handler 帧带 _UA_HANDLER_FRAME");
    check(pa_dbg_continue >= 2, "第三步：没有 landing pad 的中间帧被静默穿过");
    check(pa_dbg_errors == 0, "第三步：personality 没有报告解码错误");
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);  /* 卡死/崩溃时也能看到已完成的断言 */
#ifdef PA_PERSONALITY_TRACE
    pa_dbg_verbose = getenv("Z1_TRACE") != NULL;  /* Z1_TRACE=1 打印逐帧日志 */
#endif
    test_end_of_stack();
    test_step1_plumbing();
    test_step2_real_lsda();
    test_broken_lsda();
    test_step3_two_phase();
    printf("SUMMARY: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
