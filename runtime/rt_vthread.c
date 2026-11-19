#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* pthread_getattr_np（root 主栈区间捕获，ASan 注解用） */
#endif
// rt_vthread.c —— Go 式无色虚拟线程（M=1 协作调度，先验薄原型）
//
// 设计文档：docs/vthread-design.md。核心取舍：
// - 单 OS 线程跑调度器 + 全部 vthread：RC/GC/池/temp_stack 全是单线程全局态，
//   零原子零锁。切换只换 rsp + callee-saved + rip，外加每 vthread 一份
//   「栈水位」全局（帧链/handler 链/temp 栈/异常上下文/eh 深度）。
// - 让出点只在语句边界的运行时调用（chan send/recv、sleep、join）。
//   纯标量死循环不让出（协作饥饿，文档写明；Go 的异步抢占是后续）。
// - 主流程是第一个 vthread（root，用 OS 主栈）。另有专用调度器上下文
//   （sched，独立 mmap 栈，libco 式）：所有 vthread 阻塞/死亡都切回它，
//   它从 run queue 取下一个切入。不用对称 inline 调度——那会形成循环
//   ret 链，vthread 死亡后残留指向死栈的返回点（先验第一版实测踩过）。
// - GC：运行栈走既有精确根扫描；挂起栈按保存的帧链精确扫描（scan_mask）
//   + 每 vthread temp 栈；callee-saved 寄存器存在 VT payload 里，由 OBJ
//   保守扫描覆盖。VT 之间用 all_next 链保守互钉（生命周期仍归 RC 管）。
// - 异常：setjmp 与零开销 _Unwind 两条路径都在跳板捕获，存进 VT，join 重抛；
//   未 join 的死亡异常在退出时打印（Go 语义的薄化版）。
//
// 先验限制（不做）：M>1、可增长栈、select、抢占、跨栈 CFI 完整化、Windows。
#include "rt_internal.h"

#if defined(_WIN32)

/* Win64 先验不支持：mmap/CFI LSDA 路径都是 SysV 口径。生成代码只在用了
   go/chan 语法时才调这些符号，给出明确运行期错误而不是静默错。 */
void* pa_vt_spawn(int64_t fn) { (void)fn; panic_c("vthreads are not supported on Windows yet"); }
void pa_vt_join(int64_t h) { (void)h; panic_c("vthreads are not supported on Windows yet"); }
void pa_vt_sleep(int64_t ms) { (void)ms; panic_c("vthreads are not supported on Windows yet"); }
void pa_vt_shutdown(void) {}
void pa_vt_gc_scan(void) {}
void* pa_chan_new(int64_t kind, int64_t cap) { (void)kind; (void)cap; panic_c("channels are not supported on Windows yet"); }
void pa_chan_send(void* c, int64_t v) { (void)c; (void)v; panic_c("channels are not supported on Windows yet"); }
int64_t pa_chan_recv(void* c) { (void)c; panic_c("channels are not supported on Windows yet"); }
void pa_chan_send_f(void* c, double v) { (void)c; (void)v; panic_c("channels are not supported on Windows yet"); }
double pa_chan_recv_f(void* c) { (void)c; panic_c("channels are not supported on Windows yet"); }

#else /* POSIX / SysV x86-64 */

#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__linux__)
#include <pthread.h>
#endif

/* ── 数据结构 ─────────────────────────────────────────────────────── */

#define PA_VT_MAGIC   0x5041565400000001LL  /* "PAVT" */
#define PA_CHAN_MAGIC 0x5041434800000001LL  /* "PACH" */

enum { VT_RUNNABLE = 0, VT_RUNNING = 1, VT_BLOCKED = 2, VT_DEAD = 3 };
enum { VT_WAIT_JOIN = 0, VT_WAIT_SEND, VT_WAIT_RECV, VT_WAIT_SLEEP, VT_WAIT_DRAIN };

/* 切换上下文。布局与 pa_vt_swap 的偏移一一对应：
   0 sp / 8 rbx / 16 rbp / 24 r12 / 32 r13 / 40 r14 / 48 r15 / 56 rip / 64 rdi。
   rdi 只对新生 vthread 有意义（跳板第 0 实参 = VT*）；恢复旧上下文时
   rdi 是 caller-saved 垃圾，C 代码不依赖它。 */
typedef struct {
    uintptr_t sp, rbx, rbp, r12, r13, r14, r15, rip, rdi;
} PaVtCtx;

typedef struct PaVt {
    int64_t magic;
    int64_t state;
    int64_t id;
    int64_t scheduler_ref;   /* 调度器持有的一份引用：spawn 到死亡/放弃 */
    void* stack_map;         /* mmap 基址（root 为 NULL，用 OS 主栈） */
    int64_t stack_size;      /* mmap 总长（含 guard page） */
    PaVtCtx ctx;
    /* ASan 纤维切换用的目标栈区间（root 是 OS 主栈，需实测捕获） */
    const void* swap_bottom;
    int64_t swap_size;
    /* 每 vthread 的栈水位全局（切换时 save/load） */
    PyAdamasFrame* frame_top;
    PyAdamasHandler* handler_top;
    int64_t eh_depth;
    void** temp_stack;
    int64_t temp_len;
    int64_t temp_cap;
    void* exc_node_top;
    int64_t exc_node_depth;
    PyAdamasExcSave* exc_save_top;
    PyAdamasException* current_exc;
    void* current_exc_inst;
    PyAdamasList* pending_exc_args;
    PyAdamasException* pending_exc;
    /* spawn 载荷与结果 */
    int64_t fn;              /* 包装闭包（owned） */
    PyAdamasException* exc;  /* 逃出的异常（owned，malloc） */
    int64_t joined;
    /* 阻塞链接 */
    struct PaVt* wait_next;  /* chan sendq/recvq、joinq、timers 通用 intrusive 链 */
    int64_t join_target;     /* join 等待期间对目标的一份引用 */
    int64_t chan_value;      /* 阻塞期间寄存的值（heap 种类时 owned） */
    int64_t chan_kind;
    int64_t chan_done;       /* send: 值被取走；recv: 值已送达 */
    int64_t wake_ms;
    struct PaVt* joinq;
    struct PaVt* all_next;   /* 全局 VT 链（GC 扫描挂起栈用） */
} PaVt;

typedef struct {
    int64_t magic;
    int64_t elem_kind;       /* 0 标量机器字 / 1 已知堆 / 2 unknown */
    int64_t cap;
    PyAdamasList* buf;       /* owned；FIFO 缓冲（GC 按 LIST 精确扫描） */
    PaVt* sendq;             /* 阻塞的发送者（借用：调度器引用保活） */
    PaVt* recvq;
} PaChan;

/* ── 全局态（全是单线程，零原子） ─────────────────────────────────── */

static PaVt* vt_current;
static PaVt* vt_sched;           /* 专用调度器上下文（独立 mmap 栈，libco 式） */
static PaVt* vt_drain_vt;        /* 退出 drain 的阻塞者（root） */
static PyAdamasList* vt_rq;      /* runnable 队列（借用指针，调度器引用保活） */
static PaVt* vt_timers;          /* sleep 中的 VT（intrusive） */
static PaVt* vt_all;             /* 全部存活 VT（intrusive，GC 扫描入口） */
static int64_t vt_next_id;
static int vt_roots_registered;
static void* vt_roots[3];
static size_t vt_stack_bytes = 256 * 1024;

#define VT_STACK_POOL_MAX 256
typedef struct StackBlock { struct StackBlock* next; void* map; size_t size; } StackBlock;
static StackBlock* vt_stack_pool;
static int vt_stack_pool_count;

/* ── 切换 asm ─────────────────────────────────────────────────────── */

void pa_vt_swap(PaVtCtx* from, PaVtCtx* to);
#if defined(__SANITIZE_ADDRESS__)
/* ASan 纤维切换注解：告诉插桩运行时栈换了，否则 vthread 栈上的
   longjmp/_Unwind 会触发 __asan_handle_no_return 警告（污染 expect 输出），
   且栈毒化记账错乱。非 ASan 构建不存在这两个符号。 */
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
#endif
void pa_vt_body_invoke(PaVt* vt);
void pa_vt_invoke_inner(PaVt* vt);
void pa_vt_exc_landing(void* unwound);
void pa_handler_discard_record(void* p);
void pa_exc_ctx_save(void** top, int64_t* depth);
void pa_exc_ctx_load(void* top, int64_t depth);

__asm__(
    ".text\n"
    ".globl pa_vt_swap\n"
    PA_TYPE_FUNC(pa_vt_swap)
    "pa_vt_swap:\n"
    "\t.cfi_startproc\n"
    /* 切换点之后栈不再连续：禁止 traceback/展开跨栈乱走。 */
    "\t.cfi_undefined %rip\n"
    "\tmovq %rsp, 0(%rdi)\n"
    "\tmovq %rbx, 8(%rdi)\n"
    "\tmovq %rbp, 16(%rdi)\n"
    "\tmovq %r12, 24(%rdi)\n"
    "\tmovq %r13, 32(%rdi)\n"
    "\tmovq %r14, 40(%rdi)\n"
    "\tmovq %r15, 48(%rdi)\n"
    /* sp 存「返回地址已弹出」的口径（rsp+8）：恢复用 jmp 而不是 ret，
       不弹栈。存原始 rsp 会让每次恢复后 rsp 低 8 字节，踩坏调用者帧。 */
    "\tleaq 8(%rsp), %rax\n"
    "\tmovq %rax, 0(%rdi)\n"
    "\tmovq (%rsp), %rax\n"      /* 返回地址 = 恢复时的 rip */
    "\tmovq %rax, 56(%rdi)\n"
    "\tmovq 0(%rsi), %rsp\n"
    "\tmovq 8(%rsi), %rbx\n"
    "\tmovq 16(%rsi), %rbp\n"
    "\tmovq 24(%rsi), %r12\n"
    "\tmovq 32(%rsi), %r13\n"
    "\tmovq 40(%rsi), %r14\n"
    "\tmovq 48(%rsi), %r15\n"
    "\tmovq 64(%rsi), %rdi\n"
    "\tjmp *56(%rsi)\n"
    "\t.cfi_endproc\n"
    PA_SIZE_FUNC(pa_vt_swap));

/* 跳板外壳：给 _Unwind 一个 catch-all landing pad（零开销异常路径）。
   setjmp 路径在 pa_vt_invoke_inner 里兜。LSDA 形态与 eh_emit.cpp 一致：
   LPStart omit / TType indirect|pcrel|sdata4 + null 项 / filter=1。 */
__asm__(
    ".text\n"
    ".globl pa_vt_body_invoke\n"
    PA_TYPE_FUNC(pa_vt_body_invoke)
    "pa_vt_body_invoke:\n"
    "\t.cfi_startproc\n"
    "\tsubq $8, %rsp\n"
    "\t.cfi_def_cfa_offset 16\n"
    "\t.cfi_lsda 0x1b, .Lvt_lsda\n"
    ".Lvt_body_begin:\n"
    "\tcall pa_vt_invoke_inner\n"
    ".Lvt_body_end:\n"
    "\taddq $8, %rsp\n"
    "\t.cfi_def_cfa_offset 8\n"
    "\tret\n"
    ".Lvt_lp:\n"
    "\tmovq %rax, %rdi\n"
    "\tcall pa_vt_exc_landing\n"
    "\tud2\n"
    "\t.cfi_endproc\n"
    PA_SIZE_FUNC(pa_vt_body_invoke)
    ".section .gcc_except_table,\"a\",@progbits\n"
    ".Lvt_lsda:\n"
    "\t.byte 0xff\n"
    "\t.byte 0x9b\n"
    "\t.uleb128 .Lvt_tt_end - .Lvt_ttype_off\n"
    ".Lvt_ttype_off:\n"
    "\t.byte 0x01\n"
    "\t.uleb128 .Lvt_cs_end - .Lvt_cs_begin\n"
    ".Lvt_cs_begin:\n"
    "\t.uleb128 .Lvt_body_begin - pa_vt_body_invoke\n"
    "\t.uleb128 .Lvt_body_end - .Lvt_body_begin\n"
    "\t.uleb128 .Lvt_lp - pa_vt_body_invoke\n"
    "\t.uleb128 1\n"
    ".Lvt_cs_end:\n"
    "\t.byte 1\n"
    "\t.byte 0\n"
    "\t.balign 4\n"
    "\t.long 0\n"
    ".Lvt_tt_end:\n"
    ".text\n");

/* ── 小工具 ───────────────────────────────────────────────────────── */

static int64_t vt_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void vt_nanosleep_ms(int64_t ms) {
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {}
}

static void vt_unwind_handlers(PyAdamasHandler* stop) {
    while (handler_top && handler_top != stop) {
        PyAdamasHandler* d = handler_top;
        handler_top = d->prev;
        pa_handler_discard_record(d);
    }
    handler_top = stop;
}

/* 尾插：chan 等待队列按 FIFO 唤醒（Go 语义）；joinq/timers 顺序无关。 */
static void q_link(PaVt** head, PaVt* v) {
    v->wait_next = NULL;
    PaVt** pp = head;
    while (*pp) pp = &(*pp)->wait_next;
    *pp = v;
}
static void q_unlink(PaVt** head, PaVt* v) {
    PaVt** pp = head;
    while (*pp) {
        if (*pp == v) { *pp = v->wait_next; v->wait_next = NULL; return; }
        pp = &(*pp)->wait_next;
    }
}

/* ── 栈池 ─────────────────────────────────────────────────────────── */

static void vt_stack_underflow(void) {
    panic_c("vthread trampoline returned (stack underflow)");
}

static void vt_pool_cleanup(void) {
    while (vt_stack_pool) {
        StackBlock* b = vt_stack_pool;
        vt_stack_pool = b->next;
        munmap(b->map, b->size);
        free(b);
    }
    vt_stack_pool_count = 0;
}

static void vt_stack_alloc(PaVt* vt) {
    size_t page = 4096;
    size_t total = vt_stack_bytes + page;
    void* m = NULL;
    if (vt_stack_pool) {
        StackBlock* b = vt_stack_pool;
        vt_stack_pool = b->next;
        vt_stack_pool_count--;
        m = b->map;
        total = b->size;
        free(b);
    } else {
        m = mmap(NULL, total, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (m == MAP_FAILED) panic_c("vthread stack mmap failed");
        if (mprotect(m, page, PROT_NONE) != 0) panic_c("vthread guard page failed");
    }
    vt->stack_map = m;
    vt->stack_size = (int64_t)total;
    vt->swap_bottom = (const char*)m + page;
    vt->swap_size = (int64_t)(total - page);
    uintptr_t top = ((uintptr_t)m + total) & ~(uintptr_t)15;
    /* 伪造返回地址：跳板永不 ret，真 ret 了说明栈被踩穿。 */
    *(uintptr_t*)(top - 8) = (uintptr_t)vt_stack_underflow;
    vt->ctx.sp = top - 8;               /* 函数入口口径 rsp%16==8 */
    vt->ctx.rip = (uintptr_t)pa_vt_body_invoke;
    vt->ctx.rdi = (uintptr_t)vt;
}

/* 死亡/放弃时把栈还池。只挂 freelist 不 munmap：单线程下切换完成前
   不会有别的 vthread 复用这块内存，swap 读旧栈是安全的。 */
static void vt_stack_retire(PaVt* vt) {
    if (!vt->stack_map) return;
    void* m = vt->stack_map;
    size_t sz = (size_t)vt->stack_size;
    vt->stack_map = NULL;
    vt->stack_size = 0;
    if (vt_stack_pool_count < VT_STACK_POOL_MAX) {
        StackBlock* b = (StackBlock*)malloc(sizeof(StackBlock));
        if (b) {
            b->map = m; b->size = sz;
            b->next = vt_stack_pool;
            vt_stack_pool = b;
            vt_stack_pool_count++;
            return;
        }
    }
    munmap(m, sz);
}

/* 切换包装：ASan 下先注解「要去哪个栈」，切回后收尾。save  lives 在
   源栈的 C 帧里，源栈冻结期间一直有效。root/sched 之外的目标栈给出
   guard page 之上的真实区间。 */
static void vt_do_swap(PaVtCtx* from, PaVtCtx* to, PaVt* to_vt) {
#if defined(__SANITIZE_ADDRESS__)
    void* save = NULL;
    __sanitizer_start_switch_fiber(&save, to_vt->swap_bottom, (size_t)to_vt->swap_size);
    pa_vt_swap(from, to);
    __sanitizer_finish_switch_fiber(save, NULL, NULL);
#else
    (void)to_vt;
    pa_vt_swap(from, to);
#endif
}

/* ── 栈水位全局的 save/load ───────────────────────────────────────── */

static void vt_save_globals(PaVt* vt) {
    vt->frame_top = pa_frame_top;
    vt->handler_top = handler_top;
    vt->eh_depth = pa_eh_depth;
    vt->temp_stack = temp_stack;
    vt->temp_len = temp_len;
    vt->temp_cap = temp_cap;
    pa_exc_ctx_save(&vt->exc_node_top, &vt->exc_node_depth);
    vt->exc_save_top = exc_save_top;
    vt->current_exc = current_exc;
    vt->current_exc_inst = current_exc_inst;
    vt->pending_exc_args = pending_exc_args;
    vt->pending_exc = pending_exc;
}

static void vt_load_globals(PaVt* vt) {
    pa_frame_top = vt->frame_top;
    handler_top = vt->handler_top;
    pa_eh_depth = (int)vt->eh_depth;
    temp_stack = vt->temp_stack;
    temp_len = vt->temp_len;
    temp_cap = vt->temp_cap;
    pa_exc_ctx_load(vt->exc_node_top, vt->exc_node_depth);
    exc_save_top = vt->exc_save_top;
    current_exc = vt->current_exc;
    current_exc_inst = vt->current_exc_inst;
    pending_exc_args = vt->pending_exc_args;
    pending_exc = vt->pending_exc;
}

/* ── 调度器 ───────────────────────────────────────────────────────── */

static void vt_destroy(void* p);

static void vt_ensure(void) {
    if (!vt_roots_registered) {
        /* 先登记根再分配（loop_ensure 同款教训）：根表登记槽地址，
           之后每次赋值立刻对 GC 可见。 */
        vt_roots[0] = (void*)&vt_current;
        vt_roots[1] = (void*)&vt_rq;
        vt_roots[2] = (void*)&vt_all;
        pa_gc_add_roots(vt_roots, 3);
        vt_roots_registered = 1;
        const char* sk = getenv("PYADAMAS_VT_STACK_KB");
        if (sk) {
            long v = atol(sk);
            if (v >= 16 && v <= 65536) vt_stack_bytes = (size_t)v * 1024;
        }
        atexit(vt_pool_cleanup);
    }
    if (!vt_rq) vt_rq = pa_list_new(8, 1);
    if (!vt_current) {
        /* root vthread：主流程本身，用 OS 主栈，没有 mmap 栈。 */
        PaVt* root = (PaVt*)rc_alloc((int64_t)sizeof(PaVt), vt_destroy, NULL,
                                     PYADAMAS_RC_OBJ);
        root->magic = PA_VT_MAGIC;
        root->state = VT_RUNNING;
        root->scheduler_ref = 1;
#if defined(__linux__)
        /* root 用 OS 主栈：捕获真实区间给 ASan 纤维注解。传 NULL 会把
           ASan 的主栈记录清零，root 栈上 longjmp 时触发 no_return 警告。 */
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* lo = NULL; size_t sz = 0;
            if (pthread_attr_getstack(&attr, &lo, &sz) == 0) {
                root->swap_bottom = lo;
                root->swap_size = (int64_t)sz;
            }
            pthread_attr_destroy(&attr);
        }
#endif
        vt_current = root;
        root->all_next = vt_all;
        vt_all = root;
    }
}

static void rq_push(PaVt* v) { pa_list_append(vt_rq, (int64_t)v, 1); }
static PaVt* rq_pop(void) {
    if (!vt_rq || vt_rq->len == 0) return NULL;
    return (PaVt*)(uintptr_t)list_take_at(vt_rq, 0);
}
/* 被唤醒的 vthread 可能已被 wake 方 rq_push；它自己的 vt_block 循环经
   woken 检查直接返回时必须把自己从队列摘掉，否则留下幽灵条目，之后被
   rq_pop 出来「resume 正在运行/阻塞的自己」→ 自切换踩栈。 */
static void rq_discard(PaVt* v) {
    if (!vt_rq) return;
    for (int64_t i = 0; i < vt_rq->len; ++i) {
        if (vt_rq->data[i] == (int64_t)v) {
            list_take_at(vt_rq, i);
            return;
        }
    }
}

static int vt_woken(PaVt* self, int reason) {
    switch (reason) {
        case VT_WAIT_JOIN: return ((PaVt*)(uintptr_t)self->join_target)->state == VT_DEAD;
        case VT_WAIT_SEND:
        case VT_WAIT_RECV: return self->chan_done != 0;
        case VT_WAIT_SLEEP: return vt_now_ms() >= self->wake_ms;
        case VT_WAIT_DRAIN: return vt_rq->len == 0 && !vt_timers;
        default: return 1;
    }
}

/* 专用调度器循环（libco 式）：所有 vthread 阻塞/死亡都切回这一个上下文，
   不存在「resume 者各自的调度循环」——那样会形成循环 ret 链，vthread 死亡
   后链上残留指向死栈的 ret，再切换就踩空（先验第一版实测踩过的坑）。
   调度器跑在自己的 mmap 栈上，纯 C，无 pyadamas 帧；第一次阻塞时懒创建，
   之后永远冻结在 pa_vt_swap 里等下一次切换，不会递归增长。 */
static void vt_sched_loop(void) {
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);   /* 新生栈首次进入 */
#endif
    for (;;) {
        PaVt* next = rq_pop();
        if (!next) {
            if (vt_timers) {
                int64_t now = vt_now_ms();
                int64_t earliest = INT64_MAX;
                for (PaVt* t = vt_timers; t; t = t->wait_next)
                    if (t->wake_ms < earliest) earliest = t->wake_ms;
                if (earliest > now) vt_nanosleep_ms(earliest - now);
                now = vt_now_ms();
                PaVt** pp = &vt_timers;
                while (*pp) {
                    PaVt* t = *pp;
                    if (t->wake_ms <= now) {
                        *pp = t->wait_next;
                        t->wait_next = NULL;
                        rq_push(t);
                    } else {
                        pp = &t->wait_next;
                    }
                }
                continue;
            }
            if (vt_drain_vt && vt_rq->len == 0) {
                /* drain 完成：直接 resume 阻塞者（不入队，避免幽灵条目）。 */
                next = vt_drain_vt;
                vt_drain_vt = NULL;
            } else {
                panic_c("deadlock: all vthreads are blocked");
            }
        }
        next->state = VT_RUNNING;
        vt_current = next;
        vt_save_globals(vt_sched);
        vt_load_globals(next);
        vt_do_swap(&vt_sched->ctx, &next->ctx, next);
        /* next 阻塞/死亡切回这里：全局水位已由切换方恢复成 sched 的。 */
        if (next->state == VT_DEAD) {
            /* 还栈 + 释放调度器引用都在调度器栈上做：池满 munmap 与
               dec 级联（可能跑用户代码）都不能在将死的栈上执行。 */
            vt_stack_retire(next);
            next->scheduler_ref = 0;
            pa_rc_dec(next);
        }
    }
}

static void vt_sched_ensure(void) {
    if (vt_sched) return;
    PaVt* s = (PaVt*)rc_alloc((int64_t)sizeof(PaVt), vt_destroy, NULL, PYADAMAS_RC_OBJ);
    s->magic = PA_VT_MAGIC;
    s->state = VT_RUNNING;
    s->scheduler_ref = 1;
    pa_gc_pin(s);
    vt_stack_alloc(s);
    s->ctx.rip = (uintptr_t)vt_sched_loop;   /* 覆盖跳板入口：直接进调度循环 */
    s->all_next = vt_all;
    vt_all = s;
    vt_sched = s;
    pa_gc_unpin(s);
}

static void vt_block(int reason) {
    PaVt* self = vt_current;
    self->state = VT_BLOCKED;
    if (reason == VT_WAIT_DRAIN) vt_drain_vt = self;
    vt_sched_ensure();
    for (;;) {
        if (vt_woken(self, reason)) {
            if (reason == VT_WAIT_SLEEP) q_unlink(&vt_timers, self);
            rq_discard(self);
            self->state = VT_RUNNING;
            return;
        }
        vt_current = vt_sched;
        vt_save_globals(self);
        vt_load_globals(vt_sched);
        vt_do_swap(&self->ctx, &vt_sched->ctx, vt_sched);
        /* 调度器 resume 了 self：全局水位已由 sched 的 resume 路径恢复。 */
    }
}

/* ── 跳板 ─────────────────────────────────────────────────────────── */

/* 死亡收尾：在将死 vthread 自己的栈上执行，切回 resume 者后不再回来。 */
static void vt_finish(PaVt* self, PyAdamasException* esc) {
    self->exc = esc;
    self->state = VT_DEAD;
    while (self->joinq) {
        PaVt* w = self->joinq;
        self->joinq = w->wait_next;
        w->wait_next = NULL;
        rq_push(w);
    }
    if (temp_len != 0) pa_rc_cleanup(0);
    /* 帧链/handler 链随栈退役作废：先清掉，GC 不会扫死栈。
       **栈不在这里还**：池满时 vt_stack_retire 会 munmap，而此刻还
       跑在自己的栈上（10k 规模实测踩空）。还栈推迟到调度器循环里做。 */
    self->frame_top = NULL;
    self->handler_top = NULL;
    vt_save_globals(self);
    vt_load_globals(vt_sched);
    vt_current = vt_sched;
    vt_do_swap(&self->ctx, &vt_sched->ctx, vt_sched);
    panic_c("vt_finish: dead vthread resumed");
}

/* setjmp 兜底 + 正常调用。零开销异常从 body 逃出时 _Unwind 穿过本函数
   （无 LSDA），落在 pa_vt_body_invoke 的 catch-all LP。 */
void pa_vt_invoke_inner(PaVt* vt) {
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);   /* 新生栈首次进入 */
#endif
    PyAdamasHandler h;
    h.prev = handler_top;
    h.mark = pa_rc_mark();
    h.frame = pa_frame_top;
    h.suspend_owner = NULL;
    h.heap_alloc = 0;
    handler_top = &h;
    int64_t exc_depth0 = pa_exc_depth();
    int saved_eh = pa_eh_depth;
    pa_eh_depth = 0;
    PyAdamasException* esc = NULL;
    if (PA_SETJMP(h.env) != 0) {
        /* exc_longjmp 已按 h.frame 释放 body 帧槽；这里归还 handler 记录。 */
        vt_unwind_handlers(h.prev);
        esc = pending_exc;
        pending_exc = NULL;
        pa_rc_cleanup(h.mark);
        pa_exc_unwind(exc_depth0);
    } else {
        pa_call1((void*)(uintptr_t)vt->fn, 0);
        handler_top = h.prev;
        pa_exc_unwind(exc_depth0);
        pa_rc_cleanup(h.mark);
    }
    pa_eh_depth = saved_eh;
    vt_finish(vt, esc);
}

/* 零开销路径的 catch-all landing pad（pa_vt_body_invoke 的 LSDA 指到这里）。
   personality 的方案 G 钩子已在安装 LP 前释放 body 帧记录。 */
void pa_vt_exc_landing(void* unwound) {
    PaVt* vt = vt_current;
    (void)pa_eh_landing(unwound);
    pa_eh_done();
    vt_unwind_handlers(NULL);
    pa_rc_cleanup(0);
    pa_exc_unwind(0);
    if (current_exc_inst) { pa_rc_dec(current_exc_inst); current_exc_inst = NULL; }
    if (pending_exc_args) { pa_rc_dec(pending_exc_args); pending_exc_args = NULL; }
    PyAdamasException* esc = pending_exc;
    pending_exc = NULL;
    vt_finish(vt, esc);
}

/* ── spawn / join / sleep ─────────────────────────────────────────── */

void* pa_vt_spawn(int64_t fn) {
    vt_ensure();
    PaVt* vt = (PaVt*)rc_alloc((int64_t)sizeof(PaVt), vt_destroy, NULL, PYADAMAS_RC_OBJ);
    vt->magic = PA_VT_MAGIC;
    vt->state = VT_RUNNABLE;
    vt->id = ++vt_next_id;
    vt->fn = fn;                 /* 调用方把闭包引用转移进来（temp_take） */
    pa_gc_pin(vt);
    vt_stack_alloc(vt);
    vt->scheduler_ref = 1;
    pa_rc_inc(vt);               /* 调度器引用：spawn → 死亡/放弃 */
    vt->all_next = vt_all;
    vt_all = vt;
    rq_push(vt);
    pa_gc_unpin(vt);
    return vt;                   /* rc=1 的那份归调用方（handle） */
}

static PaVt* vt_check(int64_t h, const char* what) {
    if (!pa_unknown_is_heap(h)) pa_raise_c("TypeError", what);
    PyAdamasRc* hdr = (PyAdamasRc*)(uintptr_t)h - 1;
    if (hdr->kind != PYADAMAS_RC_OBJ || ((PaVt*)(uintptr_t)h)->magic != PA_VT_MAGIC)
        pa_raise_c("TypeError", what);
    return (PaVt*)(uintptr_t)h;
}

void pa_vt_join(int64_t h) {
    PaVt* vt = vt_check(h, "join() expects a vthread handle");
    vt->joined = 1;
    int blocked = 0;
    if (vt->state != VT_DEAD) {
        PaVt* self = vt_current;
        if (self == vt) pa_raise_c("RuntimeError", "cannot join the current vthread");
        pa_rc_inc(vt);           /* 等待期间保活目标 */
        self->join_target = (int64_t)vt;
        q_link(&vt->joinq, self);
        vt_block(VT_WAIT_JOIN);
        self->join_target = 0;
        blocked = 1;
    }
    /* 先克隆再释放等待引用：dec 可能触发 vt_destroy 释放 exc 本体。 */
    PyAdamasException* e = vt->exc ? exc_clone(vt->exc) : NULL;
    if (blocked) pa_rc_dec(vt);
    if (e) {
        exc_free(pending_exc);
        pending_exc = e;
        exc_longjmp();
    }
}

void pa_vt_sleep(int64_t ms) {
    if (ms < 0) pa_raise_c("ValueError", "sleep length must be non-negative");
    if (!vt_current) {
        /* 还没有任何 vthread：退化成普通 nanosleep，不引入调度器。 */
        vt_nanosleep_ms(ms);
        return;
    }
    if (ms == 0) return;
    PaVt* self = vt_current;
    self->wake_ms = vt_now_ms() + ms;
    q_link(&vt_timers, self);
    vt_block(VT_WAIT_SLEEP);
}

/* ── channel ──────────────────────────────────────────────────────── */

static void chan_destroy(void* p) {
    PaChan* c = (PaChan*)p;
    /* 通道被丢弃时还阻塞在上面的 vthread 永远等不到对端（退出时按
       放弃处理）。先把寄存的发送值释放掉，避免重复释放。 */
    for (PaVt* v = c->sendq; v; v = v->wait_next) {
        if (v->chan_kind) pa_rc_dec_unknown(v->chan_value);
        v->chan_value = 0;
        v->chan_kind = 0;
    }
    if (c->buf) pa_rc_dec(c->buf);
}

static PaChan* chan_check(void* cp, const char* what) {
    if (!cp || !pa_unknown_is_heap((int64_t)cp)) pa_raise_c("TypeError", what);
    PyAdamasRc* hdr = (PyAdamasRc*)cp - 1;
    if (hdr->kind != PYADAMAS_RC_OBJ || ((PaChan*)cp)->magic != PA_CHAN_MAGIC)
        pa_raise_c("TypeError", what);
    return (PaChan*)cp;
}

void* pa_chan_new(int64_t kind, int64_t cap) {
    if (kind < 0 || kind > 2) kind = 2;
    if (cap < 0) pa_raise_c("ValueError", "channel capacity must be non-negative");
    vt_ensure();
    PaChan* c = (PaChan*)rc_alloc((int64_t)sizeof(PaChan), chan_destroy, NULL,
                                  PYADAMAS_RC_OBJ);
    c->magic = PA_CHAN_MAGIC;
    c->elem_kind = kind;
    c->cap = cap;
    pa_gc_pin(c);
    /* 缓冲用 PyAdamasList：GC 按 LIST 精确扫描，所有权语义现成。
       0 容量也建空表（统一路径，FIFO take_at(0) 在先验规模下够用）。 */
    c->buf = pa_list_new(cap > 0 && cap < 8 ? cap : 8, kind == 0 ? 0 : 2);
    pa_gc_unpin(c);
    return c;
}

static int chan_heap_hint(PaChan* c) { return (int)c->elem_kind; }

/* send 接管 heap 种类值的一份引用（调用方 retain 后转入）。 */
void pa_chan_send(void* cp, int64_t v) {
    PaChan* c = chan_check(cp, "send() expects a channel");
    if (c->recvq) {
        /* 直接交给第一个等待的接收者：引用转移，发送者不阻塞。 */
        PaVt* r = c->recvq;
        c->recvq = r->wait_next;
        r->wait_next = NULL;
        r->chan_value = v;
        r->chan_kind = c->elem_kind;
        r->chan_done = 1;
        rq_push(r);
        return;
    }
    if (c->buf->len < c->cap) {
        pa_list_append(c->buf, v, chan_heap_hint(c));
        return;
    }
    /* 满/无缓冲且无接收者：寄存值并阻塞。值挂在 VT payload 上，
       GC 的 OBJ 保守扫描可见。 */
    PaVt* self = vt_current;
    self->chan_value = v;
    self->chan_kind = c->elem_kind;
    self->chan_done = 0;
    q_link(&c->sendq, self);
    vt_block(VT_WAIT_SEND);
}

/* recv 返回一份 owned 引用（heap 种类）/裸机器字（标量）。 */
int64_t pa_chan_recv(void* cp) {
    PaChan* c = chan_check(cp, "recv() expects a channel");
    if (c->buf->len > 0) {
        int64_t v = list_take_at(c->buf, 0);
        if (c->sendq) {
            /* 腾出位置：把第一个等待发送者的寄存值搬进缓冲。 */
            PaVt* s = c->sendq;
            c->sendq = s->wait_next;
            s->wait_next = NULL;
            int64_t sv = s->chan_value;
            s->chan_value = 0;
            s->chan_kind = 0;
            s->chan_done = 1;
            pa_list_append(c->buf, sv, chan_heap_hint(c));
            rq_push(s);
        }
        return v;
    }
    if (c->sendq) {
        PaVt* s = c->sendq;
        c->sendq = s->wait_next;
        s->wait_next = NULL;
        int64_t v = s->chan_value;
        s->chan_value = 0;
        s->chan_kind = 0;
        s->chan_done = 1;
        rq_push(s);
        return v;
    }
    PaVt* self = vt_current;
    self->chan_value = 0;
    self->chan_kind = 0;
    self->chan_done = 0;
    q_link(&c->recvq, self);
    vt_block(VT_WAIT_RECV);
    int64_t v = self->chan_value;
    self->chan_value = 0;
    self->chan_kind = 0;
    return v;
}

void pa_chan_send_f(void* cp, double d) {
    int64_t w;
    memcpy(&w, &d, 8);
    pa_chan_send(cp, w);
}

double pa_chan_recv_f(void* cp) {
    int64_t w = pa_chan_recv(cp);
    double d;
    memcpy(&d, &w, 8);
    return d;
}

/* ── GC 跨栈扫描 ──────────────────────────────────────────────────── */

/* gc_collect 的根扫描阶段调用（rt_core.c）。运行栈走既有路径；这里补
   挂起栈：帧链精确扫描（scan_mask）+ 每 vthread temp 栈。callee-saved
   寄存器/fn/寄存值在 VT payload 里，OBJ 保守扫描已覆盖；VT 之间经
   all_next 保守互钉，生命周期仍由 RC（调度器引用 + handle）管理。 */
void pa_vt_gc_scan(void) {
    for (PaVt* vt = vt_all; vt; vt = vt->all_next) {
        if (vt == vt_current || vt->state == VT_DEAD) continue;
        if (vt->frame_top) pa_gc_scan_frames_from(vt->frame_top);
        for (int64_t i = 0; i < vt->temp_len; ++i)
            pa_gc_mark_word((uintptr_t)vt->temp_stack[i]);
    }
}

/* ── 退出收尾 ─────────────────────────────────────────────────────── */

/* 放弃一个永久阻塞的 vthread：释放它持有的所有权状态，还栈。
   在 root 栈上执行，dec 级联有 sane 上下文。 */
static void vt_abandon(PaVt* vt) {
    for (PyAdamasHandler* h = vt->handler_top; h; h = h->prev)
        pa_handler_discard_record(h);
    vt->handler_top = NULL;
    for (int64_t i = 0; i < vt->temp_len; ++i)
        if (vt->temp_stack[i]) pa_rc_dec(vt->temp_stack[i]);
    vt->temp_len = 0;
    if (vt->chan_kind) pa_rc_dec_unknown(vt->chan_value);
    vt->chan_value = 0;
    vt->chan_kind = 0;
    if (vt->join_target) {
        pa_rc_dec((void*)(uintptr_t)vt->join_target);
        vt->join_target = 0;
    }
    /* 异常上下文：临时装上它的再 unwind 到 0，然后恢复 root 的。 */
    void* save_node = NULL; int64_t save_depth = 0;
    pa_exc_ctx_save(&save_node, &save_depth);
    PyAdamasExcSave* save_s = exc_save_top;
    PyAdamasException* save_cur = current_exc;
    void* save_inst = current_exc_inst;
    PyAdamasList* save_args = pending_exc_args;
    PyAdamasException* save_pend = pending_exc;
    pa_exc_ctx_load(vt->exc_node_top, vt->exc_node_depth);
    exc_save_top = vt->exc_save_top;
    current_exc = vt->current_exc;
    current_exc_inst = vt->current_exc_inst;
    pending_exc_args = vt->pending_exc_args;
    pending_exc = vt->pending_exc;
    pa_exc_unwind(0);
    if (current_exc_inst) pa_rc_dec(current_exc_inst);
    if (pending_exc_args) pa_rc_dec(pending_exc_args);
    exc_free(pending_exc);
    pa_exc_ctx_load(save_node, save_depth);
    exc_save_top = save_s;
    current_exc = save_cur;
    current_exc_inst = save_inst;
    pending_exc_args = save_args;
    pending_exc = save_pend;
    vt->frame_top = NULL;
    vt->state = VT_DEAD;
    vt_stack_retire(vt);
    vt->scheduler_ref = 0;
    pa_rc_dec(vt);
}

/* 入口退出时调用（release_globals 之前）：把剩余 vthread 跑完（drain），
   打印未 join 的死亡异常，放弃永久阻塞者。与 Go 的「main 返回即杀」不同：
   先验取确定性 drain，方便 expect 钉输出（文档写明）。 */
void pa_vt_shutdown(void) {
    if (!vt_current) return;
    PaVt* root = vt_current;
    if (vt_rq->len > 0 || vt_timers) vt_block(VT_WAIT_DRAIN);
    for (PaVt* vt = vt_all; vt; vt = vt->all_next) {
        if (vt == root) continue;
        if (vt->state == VT_DEAD && vt->exc && !vt->joined) {
            fflush(stdout);
            fprintf(stderr, "pyadamas: uncaught %s in unjoined vthread %lld: %s\n",
                    vt->exc->type, (long long)vt->id, vt->exc->message);
            vt->joined = 1;
        }
    }
    for (PaVt* vt = vt_all; vt; vt = vt->all_next) {
        if (vt == root || vt->state != VT_BLOCKED) continue;
        vt_abandon(vt);
    }
    /* root 的 temp 数组就是进程全局那一份（主流程一直在用）：以当前
       全局为准收进 root，清掉全局指针，vt_destroy(root) 释放它，
       pa_rc_shutdown 随后 free(NULL) —— 否则双重释放（ASan 实测）。 */
    root->temp_stack = temp_stack;
    root->temp_len = temp_len;
    root->temp_cap = temp_cap;
    pa_rc_cleanup(0);
    temp_stack = NULL;
    temp_len = 0;
    temp_cap = 0;
    /* 调度器上下文：循环永远冻结在 swap 里，不会再被 resume，栈可还。 */
    if (vt_sched) {
        PaVt* s = vt_sched;
        vt_sched = NULL;
        vt_stack_retire(s);
        s->scheduler_ref = 0;
        pa_rc_dec(s);
    }
    /* root 自己的调度器引用 */
    vt_current = NULL;
    root->scheduler_ref = 0;
    pa_rc_dec(root);
}

static void vt_destroy(void* p) {
    PaVt* vt = (PaVt*)p;
    if (vt->exc) {
        if (!vt->joined) {
            fflush(stdout);
            fprintf(stderr, "pyadamas: uncaught %s in unjoined vthread %lld: %s\n",
                    vt->exc->type, (long long)vt->id, vt->exc->message);
        }
        exc_free(vt->exc);
    }
    if (vt->fn) pa_rc_dec((void*)(uintptr_t)vt->fn);
    if (vt->temp_stack) free(vt->temp_stack);
    if (vt->stack_map) vt_stack_retire(vt);
    PaVt** pp = &vt_all;
    while (*pp && *pp != vt) pp = &(*pp)->all_next;
    if (*pp) *pp = vt->all_next;
}

#endif /* _WIN32 */
