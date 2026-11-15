/* driver.c —— tests/exc/z2_rt 的宿主：构造异常、摆 RC 槽、断言帧释放语义。
 *
 * 验证目标（docs/exc-frame-cleanup.md §5 方案 G、§10 V1–V4）：
 *   1. 三层帧（inner 抛 → mid 有 3 个槽 → outer catch）时，被跳过的 inner/mid
 *      帧记录里的 owned 槽在 personality 的 phase 2、安装 LP 之前被释放，且
 *      恰好释放一次；handler 帧自己的记录与槽必须保留。
 *   2. bit0（owned unknown）/ bit1（只给 GC 扫的借来引用）标志的处理与
 *      pa_frame_unwind 逐字一致：bit1 不释放。
 *   3. landing pad 侧 pa_eh_landing 装出来的全局状态与 longjmp 落地后一致。
 *   4. handler 帧没有记录时（z2_norec_fn），释放停止点是「cfa 到 handler 帧」，
 *      链上第一条更外层的记录保留。
 *   5. 100000 次 throw/catch 后 RSS 不无界增长（GC 开 / 关两种模式）。
 *
 * 编译口径见 run.sh：driver 与运行时各部分一起编，personality / rt_eh 打开
 * -DPA_PERSONALITY_TRACE 以便断言观测计数器。
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

#include "rt_internal.h"

/* ── 帧记录布局的静态校验（和 z2_threelayer.S 里手摆的偏移对齐） ───── */
_Static_assert(sizeof(PyAdamasFrame) == 6 * 8, "帧记录头必须是 6 个字（含 cfa）");
_Static_assert(offsetof(PyAdamasFrame, cfa) == 5 * 8, "cfa 必须是第 6 个字");
_Static_assert(offsetof(PyAdamasFrame, slots) == 6 * 8, "槽地址从第 7 个字起");

/* ── 夹具符号（z2_threelayer.S） ───────────────────────────────────── */
extern int z2_outer(PyAdamasException* exc, void* outer_obj, void* mid_a,
                    void* mid_b, void* scan_c, void* inner_obj);
extern int z2_norec_fn(PyAdamasException* exc, void* mid_a, void* mid_b,
                       void* scan_c, void* inner_obj);
extern int z2_throw_fn(PyAdamasException* exc);
void* z2_personality_slot; /* CIE 0x9b 的间接槽：main 里填 &pa_personality */
/* 夹具探针：各帧实际 rsp（Z2_DEBUG=1 时打印，核对 CFA 口径用）。 */
extern uintptr_t z2_dbg_rsp_outer, z2_dbg_rsp_mid, z2_dbg_rsp_inner, z2_dbg_rsp_lp;

extern _Unwind_Reason_Code pa_personality(int version, _Unwind_Action actions,
                                          _Unwind_Exception_Class ex_class,
                                          struct _Unwind_Exception* exc,
                                          struct _Unwind_Context* ctx);
extern int pa_personality_last_error;
/* 定义在 rt_personality.c：方案 G 的帧清理钩子，由 rt_eh.c 的构造函数装配。 */
extern void (*pa_personality_frame_hook)(uint64_t ex_class, uintptr_t target_cfa);

/* rt_eh.c / rt_personality.c 的观测计数器（两个文件都按 -DPA_PERSONALITY_TRACE 编）。 */
#ifdef PA_PERSONALITY_TRACE
extern volatile int pa_dbg_eh_throws;
extern volatile int pa_dbg_eh_landings;
extern volatile int pa_dbg_eh_dones;
extern volatile int pa_dbg_eh_uncaught;
extern volatile int pa_dbg_eh_foreign;
extern volatile int pa_dbg_eh_wrappers_live;
extern volatile int pa_dbg_frame_release_calls;
extern volatile int pa_dbg_frames_released;
extern volatile int pa_dbg_frame_release_skipped;
extern volatile int pa_dbg_handler_frames;
extern volatile int pa_dbg_installs;
extern volatile int pa_dbg_search;
extern volatile int pa_dbg_errors;
extern volatile int pa_dbg_verbose;
#endif

/* ── 断言框架 ─────────────────────────────────────────────────────── */
static int g_checks;
static int g_fails;
static int g_quiet; /* 循环测试里静默：只在失败时打印 */
static int g_abort; /* 静默模式下失败后让循环立刻退出 */
static int g_neg_keep_going; /* 反证用：失败也把循环跑完，让泄漏反映到 RSS 上 */

static void check(int cond, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (cond) {
        if (g_quiet) return; /* 循环里的断言只当“必须成立”，不打印也不计数 */
        ++g_checks;
        printf("PASS: %s\n", msg);
        return;
    }
    ++g_fails;
    g_abort = 1;
    if (g_quiet && g_fails > 20) return; /* 反证模式：只打前 20 条，别刷屏 */
    printf("FAIL: %s\n", msg);
}

/* ── RC 对象与释放计数 ─────────────────────────────────────────────── */
/* 对象第一个字存「计数器地址」；dtor 里 +1。释放两次会先撞 pa_rc_dec 的
   refcount 断言（panic），计数器还能量化次数。 */
static int rel_outer, rel_mid_a, rel_mid_b, rel_scan, rel_inner;

static void obj_dtor(void* p) {
    int* ctr = *(int**)p;
    if (ctr) ++*ctr;
}

static void* obj_new(int* ctr) {
    int** p = (int**)rc_alloc((int64_t)sizeof(int*), obj_dtor, NULL, PYADAMAS_RC_OBJ);
    *p = ctr;
    return p;
}

static void rel_reset(void) {
    rel_outer = rel_mid_a = rel_mid_b = rel_scan = rel_inner = 0;
}

/* 夹具正常出口用：释放一个 owned 槽。 */
void z2_release_one_owned(void* obj) { pa_rc_dec(obj); }

/* ── 驱动侧的帧记录（模拟 codegen 的压帧） ─────────────────────────── */
/* 只在 driver 的 C 帧里摆一条记录：两个作用——(a) 循环期间把待传对象登记成
   GC 根（和生成代码一样，别让 major GC 把只在 C 局部里的对象当垃圾收走）；
   (b) 给「handler 帧没有记录」的用例提供一条更外层的记录，验证停止点。
   记录的 cfa 只要求「比任何被调帧都大」：它永远不会被释放（handler 帧比它深）。 */
#define DRV_MAX_SLOTS 6
typedef struct {
    uint64_t words[6 + DRV_MAX_SLOTS]; /* 6 字头 + 槽地址×n */
} DrvRec;

static PyAdamasFrame* g_drv_frame; /* 最近一条驱动帧记录（断言用） */

static PyAdamasFrame* drv_rec_push(DrvRec* r, int n, void** addrs) {
    PyAdamasFrame* f = (PyAdamasFrame*)r->words;
    memset(r, 0, sizeof *r);
    f->prev = pa_frame_top;
    f->n = n;
    f->cfa = (uintptr_t)__builtin_frame_address(0); /* 比所有被调帧大即可 */
    for (int i = 0; i < n; ++i) f->slots[i] = addrs[i];
    pa_frame_top = f;
    g_drv_frame = f;
    return f;
}

static void drv_rec_pop(PyAdamasFrame* f) {
    if (pa_frame_top != f) panic_c("驱动帧记录链被破坏");
    pa_frame_top = f->prev;
    g_drv_frame = NULL;
}

/* ── 环境状态（landing 后要逐项对齐的全局） ───────────────────────── */
static PyAdamasException* g_expected;      /* 本次抛出的异常对象 */
static PyAdamasException* g_ambient_cur;   /* 抛出前就有的 current_exc */
static PyAdamasExcSave* g_ambient_save;    /* 抛出前就有的 exc_save_top */
static void* g_inst;                       /* 富异常实例（RC） */
static PyAdamasList* g_args;               /* 多参 args（RC） */
static int g_lp_calls;

/* ── landing pad 里被汇编调用的两个 handler ────────────────────────── */

/* 有帧记录的 catch-all handler：
 *   %rdi = 展开包装指针（GR0），%rsi = 本帧记录地址，%rdx = 本帧 owned 槽对象 */
int z2_outer_handler(void* unwound, PyAdamasFrame* own_rec, void* own_obj) {
    PyAdamasException* e;
    int rel_calls0;

    ++g_lp_calls;

    if (getenv("Z2_DEBUG")) {
        PyAdamasFrame* q;
        printf("[dbg] top=%p own=%p own_cfa=%#lx own_prev=%p drv=%p\n",
               (void*)pa_frame_top, (void*)own_rec, (unsigned long)own_rec->cfa,
               (void*)own_rec->prev, (void*)g_drv_frame);
        printf("[dbg] rsp outer=%#lx mid=%#lx inner=%#lx lp=%#lx\n",
               (unsigned long)z2_dbg_rsp_outer, (unsigned long)z2_dbg_rsp_mid,
               (unsigned long)z2_dbg_rsp_inner, (unsigned long)z2_dbg_rsp_lp);
        for (q = pa_frame_top; q; q = q->prev)
            printf("[dbg]   rec=%p cfa=%#lx n=%ld prev=%p\n",
                   (void*)q, (unsigned long)q->cfa, (long)q->n, (void*)q->prev);
    }

    /* V4：链一致性 —— 更深的记录全没了，只剩 handler 帧自己的记录，
       它的 prev 是驱动帧记录（如果压了）。 */
    check(pa_frame_top == own_rec,
          "LP 入口 pa_frame_top 就是 handler 帧自己的记录（被跳过的记录已释放）");
    check(own_rec->prev == g_drv_frame,
          "handler 帧记录的 prev 是更外层的驱动帧记录（没有被误释放）");
    check(own_rec->cfa == (uintptr_t)own_rec + 64,
          "handler 帧记录的 cfa 与汇编手摆的 CFA 一致（rec+64，即 rsp+112）");
    check(own_rec->n == 1, "handler 帧记录 n = 1（只有它自己的 owned 槽）");
    check(*(void**)own_rec->slots[0] == own_obj, "handler 帧 owned 槽的值还在");

    /* 被跳过帧的槽：恰好释放一次；bit1 不释放；handler 帧自己的槽不动。 */
    check(rel_inner == 1, "内层帧 owned 槽被展开释放且只释放一次（%d）", rel_inner);
    check(rel_mid_a == 1, "中间帧 owned 堆槽被释放且只释放一次（%d）", rel_mid_a);
    check(rel_mid_b == 1, "中间帧 owned unknown 槽（bit0）被释放且只释放一次（%d）", rel_mid_b);
    check(rel_scan == 0, "bit1（只给 GC 扫的借来引用）没有被释放（%d）", rel_scan);
    check(rel_outer == 0, "handler 帧自己的 owned 槽没有被展开释放（%d）", rel_outer);

#ifdef PA_PERSONALITY_TRACE
    rel_calls0 = pa_dbg_frame_release_calls;
    check(rel_calls0 >= 1, "personality 在 phase 2 handler 帧里调用过帧释放（%d 次）", rel_calls0);
    check(pa_dbg_eh_foreign == 0, "没有被当成外来异常（%d）", pa_dbg_eh_foreign);
#endif

    /* 落地：装好全局状态 */
    e = pa_eh_landing(unwound);
    check(e == g_expected, "pa_eh_landing 取回的就是抛出的那个 PyAdamasException");
    check(pending_exc == g_expected, "pending_exc 已装好（分派代码读它）");
    check(current_exc == g_ambient_cur,
          "current_exc 保持抛出时的值（与 longjmp 落地后一致，展开不改它）");
    check(exc_save_top == g_ambient_save, "exc_save_top 保持抛出时的值");
    check(current_exc_inst == g_inst, "富异常实例搬回 current_exc_inst");
    check(pending_exc_args == g_args, "多参 args 搬回 pending_exc_args");

    /* 模拟 landing pad 分派结束后的收尾：异常本体（pending_exc）+ 展开包装。 */
    pa_exc_clear();
    pa_eh_done();
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_eh_wrappers_live == 0, "展开包装已回收（存活 %d）", pa_dbg_eh_wrappers_live);
#endif

    /* 正常出口该做的事：先清槽值再 dec（与展开路径同一约定）。 */
    *(void**)own_rec->slots[0] = NULL;
    pa_rc_dec(own_obj);
    check(rel_outer == 1, "handler 帧自己的槽由正常出口释放（恰好一次，%d）", rel_outer);
    return 0;
}

/* 不压帧记录的 catch-all handler：%rdi = 展开包装指针。 */
int z2_norec_handler(void* unwound) {
    PyAdamasException* e;

    ++g_lp_calls;
    /* handler 帧没有记录：释放到 handler 帧 CFA 为止，链上第一条更外层的
       记录（驱动帧记录）保留 —— 这正是方案 G 的停止条件。 */
    check(pa_frame_top == g_drv_frame,
          "handler 帧无记录时，释放后链顶是更外层的驱动帧记录");
    check(rel_inner == 1, "norec：内层帧 owned 槽被释放一次（%d）", rel_inner);
    check(rel_mid_a == 1 && rel_mid_b == 1, "norec：中间帧两个 owned 槽各释放一次");
    check(rel_scan == 0, "norec：bit1 槽没有被释放");

    e = pa_eh_landing(unwound);
    check(e == g_expected, "norec：pa_eh_landing 取回抛出的异常");
    pa_exc_clear();
    pa_eh_done();
    return 0;
}

/* ── 读 RSS ────────────────────────────────────────────────────────── */
static long rss_kb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    long total = 0, res = -1;
    long page = sysconf(_SC_PAGESIZE);
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &total, &res) != 2) res = -1;
    fclose(f);
    if (res < 0 || page <= 0) return -1;
    return res * (page / 1024);
}

/* ── 用例 1：未捕获路径 ────────────────────────────────────────────── */
static void test_uncaught(void) {
    PyAdamasException* e = exc_new("ValueError", "z2-uncaught");
    int rc;

    g_quiet = 0;
#ifdef PA_PERSONALITY_TRACE
    int thr0 = pa_dbg_eh_throws, unc0 = pa_dbg_eh_uncaught;
    int rel0 = pa_dbg_frame_release_calls;
#endif
    rc = z2_throw_fn(e);
    check(rc == PA_EH_UNCAUGHT, "没人接时 pa_eh_throw 返回 PA_EH_UNCAUGHT（实际 %d）", rc);
    check(pending_exc == e, "未捕获路径上 pending_exc 仍是抛出的异常（上层据此打印）");
    check(pa_eh_current != NULL, "未捕获路径上展开包装还挂着（等 pa_eh_done 收尾）");
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_eh_throws == thr0 + 1, "抛出计数 +1");
    check(pa_dbg_eh_uncaught == unc0 + 1, "未捕获计数 +1");
    check(pa_dbg_frame_release_calls == rel0,
          "没人接时 phase 2 不跑：帧释放 0 次（前后都是 %d）", rel0);
    check(pa_dbg_frame_release_skipped == 0, "没有走 foreign 跳过分支");
#endif
    pa_exc_clear();
    pa_eh_done();
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_eh_wrappers_live == 0, "收尾后展开包装存活数 0");
#endif
    check(pa_frame_top == NULL, "未捕获路径不动帧链（top 仍是 NULL）");
}

/* ── 用例 2：三层帧（核心） ────────────────────────────────────────── */
static void test_three_layers(void) {
    DrvRec rec;
    PyAdamasFrame* f;
    PyAdamasException* e;
    void *oa = NULL, *ma = NULL, *mb = NULL, *sc = NULL, *in = NULL;
    void* roots[5];
    int rc;

    g_quiet = 0;
    g_expected = NULL;
    rel_reset();
    g_lp_calls = 0;
    g_ambient_cur = exc_new("RuntimeError", "ambient");
    g_ambient_save = (PyAdamasExcSave*)malloc(sizeof(PyAdamasExcSave));
    if (!g_ambient_save) panic_c("out of memory");
    g_ambient_save->prev = NULL;
    g_ambient_save->up = NULL;
    g_inst = rc_alloc((int64_t)sizeof(int64_t), NULL, NULL, PYADAMAS_RC_OBJ);
    g_args = pa_list_new(2, 1);
    current_exc = g_ambient_cur;
    exc_save_top = g_ambient_save;
    current_exc_inst = g_inst;
    pending_exc_args = g_args;

    e = exc_new("ValueError", "z2-three");
    g_expected = e;
    roots[0] = &oa;
    roots[1] = &ma;
    roots[2] = &mb;
    roots[3] = &sc;
    roots[4] = &in;
    f = drv_rec_push(&rec, 5, roots);
    oa = obj_new(&rel_outer);
    ma = obj_new(&rel_mid_a);
    mb = obj_new(&rel_mid_b);
    sc = obj_new(&rel_scan);
    in = obj_new(&rel_inner);

    rc = z2_outer(e, oa, ma, mb, sc, in);
    check(rc == 100, "三层帧：外层 handler LP 正常返回 100（实际 %d）", rc);
    check(g_lp_calls == 1, "三层帧：landing pad 恰好执行一次（%d）", g_lp_calls);
    check(pa_frame_top == f, "退出后帧链回到驱动帧记录（handler 记录已弹）");

    /* 被释放的对象指针已经悬空，先清掉再从驱动帧记录里摘出去。 */
    oa = ma = mb = in = NULL;
    drv_rec_pop(f);
    check(pa_frame_top == NULL, "驱动帧记录弹出后帧链为空");

    check(rel_scan == 0, "bit1 槽对象在展开期间没有被释放（%d）", rel_scan);
    pa_rc_dec(sc);
    sc = NULL;
    check(rel_scan == 1, "bit1 槽对象由正常清理释放一次（%d）", rel_scan);

    /* 落地后装配的旁路载荷：谁装谁卸（这里模拟分派结束的清理）。 */
    check(current_exc_inst == g_inst, "富异常实例仍在全局上（landing 装回来的）");
    check(pending_exc_args == g_args, "多参 args 仍在全局上（landing 装回来的）");
    current_exc_inst = NULL;
    pa_rc_dec(g_inst);
    pending_exc_args = NULL;
    pa_rc_dec(g_args);
    check(current_exc == g_ambient_cur, "current_exc 仍是抛出时的环境异常");
    check(exc_save_top == g_ambient_save, "exc_save_top 仍是抛出时的环境保存节点");
    exc_free(g_ambient_cur);
    free(g_ambient_save);
    current_exc = NULL;
    exc_save_top = NULL;
    g_ambient_cur = NULL;
    g_ambient_save = NULL;
    g_inst = NULL;
    g_args = NULL;
    g_expected = NULL;

#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_eh_landings >= 1, "落地计数 >= 1（%d）", pa_dbg_eh_landings);
    check(pa_dbg_eh_dones >= 1, "收尾计数 >= 1（%d）", pa_dbg_eh_dones);
    check(pa_dbg_handler_frames >= 1, "恰好一帧带 _UA_HANDLER_FRAME（%d）", pa_dbg_handler_frames);
    check(pa_dbg_errors == 0, "personality 没有 LSDA 解码错误（%d）", pa_dbg_errors);
#endif
}

/* ── 用例 3：handler 帧无记录（等价性另一半） ──────────────────────── */
static void test_norec(void) {
    DrvRec rec;
    PyAdamasFrame* f;
    PyAdamasException* e;
    void *ma = NULL, *mb = NULL, *sc = NULL, *in = NULL;
    void* roots[4];
    int rc;

    g_quiet = 0;
    rel_reset();
    g_lp_calls = 0;
    current_exc = NULL;
    exc_save_top = NULL;
    current_exc_inst = NULL;
    pending_exc_args = NULL;
    g_ambient_cur = NULL;
    g_ambient_save = NULL;
    g_inst = NULL;
    g_args = NULL;

    e = exc_new("KeyError", "z2-norec");
    g_expected = e;
    roots[0] = &ma;
    roots[1] = &mb;
    roots[2] = &sc;
    roots[3] = &in;
    f = drv_rec_push(&rec, 4, roots);
    ma = obj_new(&rel_mid_a);
    mb = obj_new(&rel_mid_b);
    sc = obj_new(&rel_scan);
    in = obj_new(&rel_inner);

    rc = z2_norec_fn(e, ma, mb, sc, in);
    check(rc == 200, "norec：handler 帧无记录时 LP 正常返回 200（实际 %d）", rc);
    check(g_lp_calls == 1, "norec：landing pad 恰好执行一次");
    check(pa_frame_top == f, "norec：链顶仍是驱动帧记录");

    ma = mb = in = NULL;
    drv_rec_pop(f);
    check(rel_scan == 0, "norec：bit1 槽没有被展开释放");
    pa_rc_dec(sc);
    sc = NULL;
    check(rel_scan == 1, "norec：bit1 槽由正常清理释放一次");
    g_expected = NULL;
}

/* ── 用例 4：10 万次抛接，RSS 不无界增长 ───────────────────────────── */
static int loop_run(long n) {
    long i;
    for (i = 0; i < n; ++i) {
        DrvRec rec;
        PyAdamasFrame* f;
        PyAdamasException* e;
        void *oa = NULL, *ma = NULL, *mb = NULL, *sc = NULL, *in = NULL;
        void* roots[5];
        int rc;

        rel_reset();
        g_lp_calls = 0;
        e = exc_new("ValueError", "z2-loop");
        g_expected = e;
        roots[0] = &oa;
        roots[1] = &ma;
        roots[2] = &mb;
        roots[3] = &sc;
        roots[4] = &in;
        f = drv_rec_push(&rec, 5, roots);
        oa = obj_new(&rel_outer);
        ma = obj_new(&rel_mid_a);
        mb = obj_new(&rel_mid_b);
        sc = obj_new(&rel_scan);
        in = obj_new(&rel_inner);

        rc = z2_outer(e, oa, ma, mb, sc, in);
        if (rc != 100) {
            printf("FAIL: 循环第 %ld 次抛接没有正常返回（rc=%d）\n", i, rc);
            ++g_fails;
            return 1;
        }
        if (g_abort) {
            if (!g_neg_keep_going) return 1;
            g_abort = 0; /* 反证模式：继续跑，让 RSS 判据自己报警 */
        }
        oa = ma = mb = in = NULL; /* 已释放，清掉悬空指针再弹记录 */
        e = NULL;
        g_expected = NULL;
        drv_rec_pop(f);
        pa_rc_dec(sc);
        sc = NULL;
        if (rel_scan != 1) {
            printf("FAIL: 循环第 %ld 次 bit1 槽释放计数 %d != 1\n", i, rel_scan);
            ++g_fails;
            return 1;
        }
    }
    return 0;
}

static void test_loop_rss(long n) {
    long r0, r1;
#ifdef PA_PERSONALITY_TRACE
    int rel0, thr0;
#endif

    g_quiet = 1;
    g_abort = 0;
    if (loop_run(1000) != 0) { /* 预热：让 malloc 的 arena / 池子稳定 */
        g_quiet = 0;
        check(0, "10 万次循环：预热阶段就失败");
        return;
    }
#ifdef PA_PERSONALITY_TRACE
    /* 基准取在预热之后：窗口正好是下面这 n 次。 */
    rel0 = pa_dbg_frames_released;
    thr0 = pa_dbg_eh_throws;
#endif
    r0 = rss_kb();
    if (loop_run(n) != 0) {
        g_quiet = 0;
        check(0, "10 万次循环：%ld 次抛接过程中失败", n);
        return;
    }
    r1 = rss_kb();
    g_quiet = 0;

    check(r0 > 0 && r1 > 0, "能读到进程 RSS（预热后 %ld KB，循环后 %ld KB）", r0, r1);
    printf("[z2] RSS: 预热后 %ld KB -> 再 %ld 次 throw/catch 后 %ld KB（增量 %+ld KB）\n",
           r0, n, r1, r1 - r0);
    printf("[z2] rss_delta_kb=%ld\n", r1 - r0); /* run.sh 解析用（机器可读） */
    check(r1 - r0 <= 1024, "10 万次抛接后 RSS 增量 %+ld KB <= 1024 KB（没有无界增长）", r1 - r0);
#ifdef PA_PERSONALITY_TRACE
    check(pa_dbg_frames_released - rel0 == (int)(2 * n),
          "累计释放帧记录 %d 条 = 2×%ld（inner+mid 各一条）",
          pa_dbg_frames_released - rel0, n);
    check(pa_dbg_eh_throws - thr0 == (int)n, "抛出计数增量 = %ld", n);
    check(pa_dbg_eh_wrappers_live == 0, "循环结束时展开包装存活数 0");
#endif
    check(pa_frame_top == NULL, "循环结束后帧链为空");
}

/* ── main ──────────────────────────────────────────────────────────── */
int main(void) {
    int dummy = 0;
    const char* only_loop = getenv("Z2_ONLY_LOOP");

    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef PA_PERSONALITY_TRACE
    if (getenv("Z2_TRACE")) pa_dbg_verbose = 1;
#endif
    z2_personality_slot = (void*)&pa_personality;

    /* 和生成代码的入口一样打开 GC（gc_stack_top 非 0 才真的跑收集）；
       PYADAMAS_GC_DISABLE=1 时 pa_gc_init 自己会保持关闭。 */
    pa_gc_init(NULL, 0, &dummy);
    if (getenv("Z2_NEG_KEEP_GOING")) g_neg_keep_going = 1;

    if (only_loop) {
        test_loop_rss(100000);
    } else {
        printf("==== z2_rt（Z2-b1：运行时胶水 + 方案 G 帧清理）====\n");
        check(pa_personality_frame_hook != NULL,
              "rt_eh.c 的构造函数已把帧清理钩子装进 personality");
        check(pa_frame_top == NULL, "启动时帧链为空");
        test_uncaught();
        test_three_layers();
        test_norec();
        test_loop_rss(100000);
        printf("SUMMARY: %d checks, %d failures\n", g_checks, g_fails);
    }
    if (only_loop && g_fails == 0) printf("SUMMARY: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
