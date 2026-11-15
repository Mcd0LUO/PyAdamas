/* driver.c —— z1_rt 夹具的宿主：构造 struct _Unwind_Exception、调用
 * _Unwind_RaiseException，断言手写汇编 landing pad 与 ABI 语义都符合预期。
 *
 * 编译要求（见 run.sh）：
 *   gcc -O2 -funwind-tables -c driver.c
 * -funwind-tables 保证 driver 自己的每一帧都有完整 .eh_frame，否则展开器
 * 从汇编帧往外走时会在这里撞墙。
 *
 * personality 指针槽：夹具的 CIE 用 .cfi_personality 0x9b 指向 z1_personality_slot
 * （运行期填充）。这里填 pa_personality；crosscheck_gxx.cpp 填 __gxx_personality_v0。
 */
#include <unwind.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 0x9b personality 间接槽（夹具 CIE 的 pcrel 目标） ─────────────── */
void *z1_personality_slot;

/* ── 观测点：汇编 landing pad / personality 写，这里断言 ─────────── */
volatile int z1_raise_rc;
volatile uintptr_t z1_step1_seen_exc;
volatile int z1_step1_seen_lp;
volatile uintptr_t z1_step2_seen_exc;
volatile int z1_step2_seen_lp;

int z1_order[8];
volatile int z1_order_n;
volatile uintptr_t z1_step3_cleanup_exc;
volatile uintptr_t z1_step3_handler_exc;
volatile int z1_step3_b_modified;

volatile int z1_broken_lp_hit;
volatile int z1_spec_lp_hit;

/* 第一步 stub personality 的计数器（stub_personality.c）。 */
extern volatile int z1_stub_calls;
extern volatile int z1_stub_search_calls;
extern volatile int z1_stub_cleanup_calls;
extern volatile int z1_stub_handler_frame;
extern volatile int z1_stub_found;
extern volatile int z1_stub_installs;

#ifdef PA_PERSONALITY_TRACE
extern volatile int pa_dbg_calls;
extern volatile int pa_dbg_search;
extern volatile int pa_dbg_cleanup;
extern volatile int pa_dbg_handler_frames;
extern volatile int pa_dbg_installs;
extern volatile int pa_dbg_continue;
extern volatile int pa_dbg_errors;
extern volatile int pa_dbg_force_unwind;
extern volatile int pa_dbg_setip_calls;
extern volatile int pa_dbg_cleanup_entries;
extern volatile int pa_dbg_catch_all;
extern volatile int pa_dbg_typed_catch;
extern volatile int pa_dbg_spec;
extern volatile int pa_dbg_verbose;
static void trace_reset(void) {
    pa_dbg_calls = pa_dbg_search = pa_dbg_cleanup = 0;
    pa_dbg_handler_frames = pa_dbg_installs = pa_dbg_continue = pa_dbg_errors = 0;
    pa_dbg_force_unwind = pa_dbg_setip_calls = 0;
    pa_dbg_cleanup_entries = pa_dbg_catch_all = pa_dbg_typed_catch = pa_dbg_spec = 0;
}
#else
static void trace_reset(void) {}
#endif

/* rt_personality.c 的解码错误观测点（该文件自包含，没有对外头文件，
   这里镜像它的错误码枚举，改那边时记得同步）。 */
extern _Unwind_Reason_Code pa_personality(int version, _Unwind_Action actions,
                                          _Unwind_Exception_Class ex_class,
                                          struct _Unwind_Exception *exc,
                                          struct _Unwind_Context *ctx);
extern int pa_personality_last_error;
extern const char *pa_personality_error_str(int code);
enum {
    Z1_LSDA_ERR_ENCODING = 1,
    Z1_LSDA_ERR_TRUNCATED = 2,
    Z1_LSDA_ERR_OVERFLOW = 3,
    Z1_LSDA_ERR_NO_LSDA = 4,
    Z1_LSDA_ERR_ACTION = 5,
    Z1_LSDA_ERR_TTYPE = 6
};

extern int z1_step1_fn(struct _Unwind_Exception *exc);
extern int z1_step2_fn(struct _Unwind_Exception *exc);
extern int z1_step3_handler_fn(struct _Unwind_Exception *exc);
extern int z1_broken_fn(struct _Unwind_Exception *exc);
extern int z1_broken_table_fn(struct _Unwind_Exception *exc);
extern int z1_spec_fn(struct _Unwind_Exception *exc);

typedef struct {
    struct _Unwind_Exception u;
    int payload;
} z1_exc;

#define Z1_EXC_CLASS 0x5a31455843303100ULL

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
    g_exc.u.private_1 = 0;
    g_exc.u.private_2 = 0;
    return &g_exc.u;
}

void z1_raise_c(struct _Unwind_Exception *exc) {
    z1_raise_rc = (int)_Unwind_RaiseException(exc);
}

void z1_note(int tag, uintptr_t exc) {
    if (z1_order_n < (int)(sizeof z1_order / sizeof z1_order[0]))
        z1_order[z1_order_n] = tag;
    ++z1_order_n;
    if (tag == 1) {
        z1_step3_cleanup_exc = exc;
        ++z1_step3_b_modified;
    } else if (tag == 2) {
        z1_step3_handler_exc = exc;
    }
}

static int bt_frames;
static int bt_lp_seen;
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg) {
    (void)ctx;
    (void)arg;
    ++bt_frames;
    if (z1_step2_seen_lp)
        bt_lp_seen = 1;
    return _URC_NO_REASON;
}
void z1_trace_probe(void) {
    bt_frames = 0;
    bt_lp_seen = 0;
    _Unwind_Backtrace(bt_cb, NULL);
}

static int fu_frames;
static int fu_rc;
static int fu_force_calls;
static int fu_setip_calls;
static _Unwind_Reason_Code fu_stop(int version, _Unwind_Action actions,
                                   _Unwind_Exception_Class ex_class,
                                   struct _Unwind_Exception *exc,
                                   struct _Unwind_Context *ctx, void *arg) {
    (void)version; (void)actions; (void)ex_class; (void)exc; (void)ctx; (void)arg;
    ++fu_frames;
    return _URC_NO_REASON;
}

void z1_forced_probe(uintptr_t exc_addr) {
    struct _Unwind_Exception *e = (struct _Unwind_Exception *)exc_addr;
    fu_frames = 0;
    fu_rc = (int)_Unwind_ForcedUnwind(e, fu_stop, NULL);
#ifdef PA_PERSONALITY_TRACE
    fu_force_calls = pa_dbg_force_unwind;
    fu_setip_calls = pa_dbg_setip_calls;
#endif
}

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

static void test_end_of_stack(void) {
    struct _Unwind_Exception *e = exc_init();
    z1_raise_rc = 0;
    z1_raise_c(e);
    check(z1_raise_rc == _URC_END_OF_STACK,
          "无 handler 时 _Unwind_RaiseException 返回 _URC_END_OF_STACK（实际 %d）",
          z1_raise_rc);
    _Unwind_DeleteException(e);
}

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

static void test_step2_real_lsda(void) {
    struct _Unwind_Exception *e = exc_init();
    exc_cleanup_calls = 0;
    z1_step2_seen_lp = 0;
    z1_step2_seen_exc = 0;
    z1_order_n = 0;
    pa_personality_last_error = 0;
    trace_reset();

    int rc = z1_step2_fn(e);
    check(rc == 7, "第二步：landing pad 返回 7（实际 %d）", rc);
    check(z1_step2_seen_lp == 1, "第二步：LSDA 指定的 landing pad 执行了一次");
    check(z1_step2_seen_exc == (uintptr_t)e,
          "第二步：landing pad 收到 GR0 传进来的 exception 指针");
    check(pa_personality_last_error == 0, "第二步：LSDA 解码无错误");
    check(bt_frames >= 5, "第二步：_Unwind_Backtrace 从保护区穿过 %d 帧", bt_frames);
    check(bt_lp_seen == 0, "第二步：_Unwind_Backtrace 期间没有安装 landing pad");
    check(fu_frames >= 5, "第二步：_Unwind_ForcedUnwind 的 stop 回调走了 %d 帧", fu_frames);
    check(fu_rc == _URC_END_OF_STACK,
          "第二步：_Unwind_ForcedUnwind 因没人安装上下文返回 _URC_END_OF_STACK（实际 %d）", fu_rc);
#ifdef PA_PERSONALITY_TRACE
    check(fu_force_calls >= 1,
          "第二步：personality 在 _UA_FORCE_UNWIND 下被调用 %d 次", fu_force_calls);
    check(fu_setip_calls == 0,
          "第二步：_UA_FORCE_UNWIND 下 personality 没有调用 _Unwind_SetIP（traceback 安全）");
    check(pa_dbg_catch_all >= 2,
          "第二步：action filter>0 + null TType 项被判定为 catch-all（%d 次）", pa_dbg_catch_all);
    check(pa_dbg_typed_catch == 0, "第二步：没有把 typed catch 当 catch-all（Z3 前的简化不触发）");
    check(pa_dbg_cleanup_entries == 0, "第二步：handler 条目的 action 下标 1 没有被当 cleanup");
    check(pa_dbg_spec == 0, "第二步：没有遇到 filter<0（exception spec）");
    check(pa_dbg_handler_frames == 1, "第二步：恰好一帧被标记 _UA_HANDLER_FRAME");
    check(pa_dbg_installs == 1, "第二步：恰好安装一次 landing pad 上下文");
    check(pa_dbg_errors == 0, "第二步：personality 没有报告解码错误");
#endif
    _Unwind_DeleteException(e);
    check(exc_cleanup_calls == 1, "第二步：exception_cleanup 被调用一次");
}

static void test_broken_lsda(void) {
    struct _Unwind_Exception *e = exc_init();
    z1_raise_rc = 0;
    z1_broken_lp_hit = 0;
    pa_personality_last_error = 0;
    trace_reset();

    int rc = z1_broken_fn(e);
    check(rc == 0, "坏表 A：判错后函数沿正常路径返回 0（实际 %d）", rc);
    check(z1_raise_rc == _URC_FATAL_PHASE1_ERROR,
          "坏表 A：_Unwind_RaiseException 返回 _URC_FATAL_PHASE1_ERROR（实际 %d）",
          z1_raise_rc);
    check(pa_personality_last_error == Z1_LSDA_ERR_ENCODING
          || pa_personality_last_error == Z1_LSDA_ERR_TRUNCATED,
          "坏表 A：报出「编码非法 / 表越界」类错误（err=%d：%s）",
          pa_personality_last_error,
          pa_personality_error_str(pa_personality_last_error));
    check(z1_broken_lp_hit == 0, "坏表 A：landing pad 没有被执行");
    _Unwind_DeleteException(e);
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_errors == 1, "坏表 A：personality 报错一次");
#endif
}

static void test_broken_table(void) {
    struct _Unwind_Exception *e = exc_init();
    z1_raise_rc = 0;
    pa_personality_last_error = 0;
    trace_reset();

    int rc = z1_broken_table_fn(e);
    check(rc == 0, "坏表 B：判错后函数沿正常路径返回 0（实际 %d）", rc);
    check(z1_raise_rc == _URC_FATAL_PHASE1_ERROR,
          "坏表 B：_Unwind_RaiseException 返回 _URC_FATAL_PHASE1_ERROR（实际 %d）",
          z1_raise_rc);
    check(pa_personality_last_error == Z1_LSDA_ERR_TRUNCATED,
          "坏表 B：报出「表越界」（err=%d：%s）",
          pa_personality_last_error,
          pa_personality_error_str(pa_personality_last_error));
    _Unwind_DeleteException(e);
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_errors == 1, "坏表 B：personality 报错一次");
#endif
}

static void test_spec_filter(void) {
    struct _Unwind_Exception *e = exc_init();
    z1_raise_rc = 0;
    z1_spec_lp_hit = 0;
    pa_personality_last_error = 0;
    trace_reset();

    int rc = z1_spec_fn(e);
    check(rc == 0, "spec 用例：继续展开到底，函数沿正常路径返回 0（实际 %d）", rc);
    check(z1_raise_rc == _URC_END_OF_STACK,
          "spec 用例：filter<0 不被当 handler，_Unwind_RaiseException 返回 _URC_END_OF_STACK（实际 %d）",
          z1_raise_rc);
    check(z1_spec_lp_hit == 0, "spec 用例：landing pad 没有被执行");
    check(pa_personality_last_error == 0, "spec 用例：不是解码错误（是我们主动跳过）");
    _Unwind_DeleteException(e);
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_spec >= 1, "spec 用例：personality 记录了 %d 次 filter<0 诊断", pa_dbg_spec);
    check(pa_dbg_catch_all == 0, "spec 用例：没有把 spec 当 catch-all");
#endif
}

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
    check(pa_dbg_cleanup_entries >= 2, "第三步：cleanup 条目按 filter==0/action 下标 0 判定（%d 次）",
          pa_dbg_cleanup_entries);
    check(pa_dbg_catch_all >= 2, "第三步：handler 条目按 filter>0 + null TType 判定为 catch-all（%d 次）",
          pa_dbg_catch_all);
    check(pa_dbg_spec == 0, "第三步：没有遇到 filter<0");
    check(pa_dbg_errors == 0, "第三步：personality 没有报告解码错误");
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef PA_PERSONALITY_TRACE
    pa_dbg_verbose = getenv("Z1_TRACE") != NULL;
#endif
    z1_personality_slot = (void *)&pa_personality;

    test_end_of_stack();
    test_step1_plumbing();
    test_step2_real_lsda();
    test_broken_lsda();
    test_broken_table();
    test_spec_filter();
    test_step3_two_phase();
    printf("SUMMARY: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
