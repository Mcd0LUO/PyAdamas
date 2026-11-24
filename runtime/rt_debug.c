// rt_debug.c —— 运行时 traceback：PC -> (函数, 行号) 表与标准栈展开
#include "rt_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>

/* 与 codegen 发出的表布局一致：
   { pc, func, line, pad }；表头 { file, entries, n }。
   函数结束哨兵的 func == NULL，用来截断越界 PC。 */
typedef struct {
    const void* pc;
    const char* func;
    int32_t line;
    int32_t pad;
} PaDbgEntry;

typedef struct {
    const char* file;
    const PaDbgEntry* entries;
    int64_t n;
} PaDbgTable;

#define PA_DBG_MAX_TABLES 128
static const PaDbgTable* dbg_tables[PA_DBG_MAX_TABLES];
static int dbg_table_count;

void pa_debug_register(const void* table) {
    if (!table || dbg_table_count >= PA_DBG_MAX_TABLES) return;
    dbg_tables[dbg_table_count++] = (const PaDbgTable*)table;
}

static const PaDbgEntry* dbg_find(const PaDbgTable* t, uintptr_t pc) {
    int64_t lo = 0, hi = t->n;
    while (lo < hi) {
        int64_t mid = (lo + hi) / 2;
        if ((uintptr_t)t->entries[mid].pc <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return NULL;
    const PaDbgEntry* e = &t->entries[lo - 1];
    return e->func ? e : NULL;
}

int pa_debug_lookup_pc(uintptr_t pc, const char** file, const char** func, int32_t* line) {
    const PaDbgEntry* best = NULL;
    const PaDbgTable* best_t = NULL;
    for (int i = 0; i < dbg_table_count; ++i) {
        const PaDbgTable* t = dbg_tables[i];
        const PaDbgEntry* e = dbg_find(t, pc);
        if (!e) continue;
        if (!best || (uintptr_t)e->pc > (uintptr_t)best->pc) {
            best = e;
            best_t = t;
        }
    }
    if (!best) return 0;
    *file = best_t->file;
    *func = best->func;
    *line = best->line;
    return 1;
}

/* 栈回溯走标准展开器（libgcc 的 _Unwind_Backtrace）：它按 .eh_frame
   逐帧还原，不依赖帧指针，所以运行时 C 代码和生成代码都不必保留 rbp。

   回调拿到的 IP 是返回地址（call 的下一条指令），和以前沿 rbp 链读到的
   是同一个值，查表口径不变。只有生成代码登记过 PC → (函数, 行号) 表；
   运行时自己的帧和 libc 的帧查不到，直接跳过。 */
#define PA_TRACE_MAX 128

typedef struct {
    uintptr_t pcs[PA_TRACE_MAX];
    int n;
} PaTraceWalk;

static _Unwind_Reason_Code pa_trace_walk_cb(struct _Unwind_Context* ctx, void* arg) {
    PaTraceWalk* w = (PaTraceWalk*)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (!ip) return _URC_END_OF_STACK;
    const char *file, *func;
    int32_t line;
    if (pa_debug_lookup_pc(ip, &file, &func, &line)) {
        w->pcs[w->n++] = ip;
        if (w->n == PA_TRACE_MAX) return _URC_END_OF_STACK;
    }
    return _URC_NO_REASON;
}

void pa_print_traceback(void) {
    PaTraceWalk w;
    w.n = 0;
    _Unwind_Backtrace(pa_trace_walk_cb, &w);
    if (w.n == 0) return;
    fprintf(stderr, "Traceback (most recent call last):\n");
    for (int i = w.n - 1; i >= 0; --i) {
        const char *file, *func;
        int32_t line;
        // 0.494: 返回地址归因用 ip-1（与抛出点捕获同口径）。LdbgEnd 哨兵紧跟
        // call 指令时，ip 正好落在哨兵上（func == NULL → 查不到），旧写法只能
        // 回落到函数起点项，traceback 就显示 line 0。
        // 先试 ip（内联后 ip 正好命中内联点的 .Ldbg，函数名与行号都准），
        // 再退回 ip-1（LdbgEnd 哨兵紧跟 call 时 ip 落在哨兵上）。
        const char *file2, *func2;
        int32_t line2 = 0;
        if (pa_debug_lookup_pc(w.pcs[i], &file2, &func2, &line2) && line2 > 0) {
            file = file2;
            func = func2;
            line = line2;
            fprintf(stderr, "  File \"%s\", line %d, in %s\n", file, (int)line, func);
            continue;
        }
        if (pa_debug_lookup_pc(w.pcs[i] - 1, &file, &func, &line))
            fprintf(stderr, "  File \"%s\", line %d, in %s\n", file, (int)line, func);
    }
}



void pa_trace_attach(PyAdamasException* e, void* gen_frame) {
    if (!e || !gen_frame) return;
    const int64_t* p = (const int64_t*)gen_frame;
    const char* file = "<unknown>";
    const char* func = "<generator>";
    int32_t line = 0;
    pa_debug_lookup_pc((uintptr_t)p[4], &file, &func, &line);
    PyAdamasTraceFrame* f = (PyAdamasTraceFrame*)malloc(sizeof(PyAdamasTraceFrame));
    if (!f) return;
    f->file = file;
    f->func = func;
    f->line = line;
    f->pad = 0;
    f->cfa = 0;  /* 逻辑帧没有物理 CFA（P4 块3 字段；不参与裁剪） */
    f->next = e->trace;
    e->trace = f;
}

PyAdamasTraceFrame* pa_trace_clone(const PyAdamasTraceFrame* f) {
    PyAdamasTraceFrame* head = NULL;
    PyAdamasTraceFrame** tail = &head;
    for (; f; f = f->next) {
        PyAdamasTraceFrame* n = (PyAdamasTraceFrame*)malloc(sizeof(PyAdamasTraceFrame));
        if (!n) break;
        *n = *f;
        n->next = NULL;
        *tail = n;
        tail = &n->next;
    }
    return head;
}

void pa_trace_free(PyAdamasTraceFrame* f) {
    while (f) {
        PyAdamasTraceFrame* n = f->next;
        free(f);
        f = n;
    }
}

void pa_trace_print(const PyAdamasException* e) {
    if (!e) return;
    for (const PyAdamasTraceFrame* f = e->trace; f; f = f->next) {
        if (f->line > 0)
            fprintf(stderr, "  File \"%s\", line %d, in %s\n", f->file, (int)f->line, f->func);
        else
            fprintf(stderr, "  File \"%s\", in %s\n", f->file, f->func);
    }
}

/* ── P4 块3（lang_gaps）：抛出点物理帧捕获 ────────────────────────
 *
 * 门控 pa_lang_gaps_rt 由入口模块体首发 LangGapsEnable 置 1；关时下面两个
 * 入口第一行即返回，不捕获、不分配，既有程序零行为/性能变化。
 *
 * 口径（CPython 3.14 实测钉住，PYTHONNODEBUGRANGES=1 去锚点对拍）：
 *   - traceback 从**接住异常的帧**开始，到抛出帧结束。抛出时还不知道会被
 *     谁接住，所以先把整条物理链记进 phys_extra，except 落地（pa_exc_accept*
 *     → pa_trace_commit）时按 handler 帧 CFA 裁剪：cfa > handler 的帧（更外
 *     层，异常还没走到就被接住了）丢弃，其余并入 phys。
 *   - 裸 raise 不加当前帧（CPython 实测：重抛帧不进 tb，只加它向外展开时
 *     经过的帧）→ RERAISE 模式去掉链尾（重抛帧）再前插。
 *   - raise e（显式异常值）整链前插（CPython 实测：显式 raise 加当前帧）。
 *   - 生成器兜底重抛 / finally 重抛 / 零开销 rethrow 不捕获：它们是同一次
 *     展开的延续，phys_extra 原样保留，落地时裁剪即可。
 *
 * 行号归因：_Unwind_Backtrace 给的是返回地址（call 的下一条指令），查表用
 * ip-1 落回 call 指令所在行——否则 call 恰好是某行最后一条指令时，返回地址
 * 顶着下一行的 .Ldbg 标签，行号归因错一行。旧 pa_print_traceback 的口径不
 * 动（门控关行为不变闸门），新捕获独立用 ip-1。 */

typedef struct {
    uintptr_t pcs[PA_TRACE_MAX];
    uintptr_t cfas[PA_TRACE_MAX];
    int n;
} PaCapWalk;

static _Unwind_Reason_Code pa_cap_walk_cb(struct _Unwind_Context* ctx, void* arg) {
    PaCapWalk* w = (PaCapWalk*)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (!ip) return _URC_END_OF_STACK;
    const char *file, *func;
    int32_t line;
    if (pa_debug_lookup_pc(ip - 1, &file, &func, &line)) {
        /* 内部包装帧不进 traceback：构造包装（__pa_new_Cls）、绑定方法
           （__pa_bound_m）、lru/partial 包装等是 pa 的机器层，CPython 没有
           对应帧（构造异常直接显示 __init__）。旧 pa_print_traceback 不受
           影响（门控关行为不变闸门）。 */
        if (func[0] == '_' && func[1] == '_' && func[2] == 'p' && func[3] == 'a' &&
            func[4] == '_')
            return _URC_NO_REASON;
        w->pcs[w->n] = ip;
        w->cfas[w->n] = (uintptr_t)_Unwind_GetCFA(ctx);
        w->n++;
        if (w->n == PA_TRACE_MAX) return _URC_END_OF_STACK;
    }
    return _URC_NO_REASON;
}

static int cap_walk_subset(PyAdamasException* e, PaCapWalk* w, int first);

void pa_trace_capture(PyAdamasException* e, int mode) {
    if (!pa_lang_gaps_rt || !e) return;
    PaCapWalk w;
    w.n = 0;
    _Unwind_Backtrace(pa_cap_walk_cb, &w);
    if (w.n == 0) return;
    /* w 是内→外序（_Unwind_Backtrace 从最内层帧开始）。裸 raise 的最内层
       帧 w[0] 是重抛帧本身，CPython 不把它加进 tb → 从 w[1] 起建链。 */
    int first = (mode == PA_CAP_RERAISE) ? 1 : 0;
    if (first >= w.n) return;
    if (cap_walk_subset(e, &w, first)) return;
    PyAdamasTraceFrame* head = NULL;
    PyAdamasTraceFrame** tail = &head;
    for (int i = w.n - 1; i >= first; --i) {
        const char *file, *func;
        int32_t line;
        if (!pa_debug_lookup_pc(w.pcs[i] - 1, &file, &func, &line)) continue;
        PyAdamasTraceFrame* f = (PyAdamasTraceFrame*)malloc(sizeof(PyAdamasTraceFrame));
        if (!f) break;
        f->file = file;
        f->func = func;
        f->line = line;
        f->pad = 0;
        f->cfa = w.cfas[i];
        f->next = NULL;
        *tail = f;
        tail = &f->next;
    }
    /* 前插到待提交链之前（VALUE/RERAISE 时旧 extra 通常为空；非空时旧链更深，
       拼在新链后面，落地裁剪按 CFA 一视同仁）。 */
    *tail = e->phys_extra;
    e->phys_extra = head;
}

/* 待提交链已含本次要走的全部帧（按 CFA 判）→ 同一次展开的延续（with 的
   __exit__ 结束后重抛、生成器兜底重抛等），不重复前插。 */
static int cap_walk_subset(PyAdamasException* e, PaCapWalk* w, int first) {
    if (!e->phys_extra) return 0;
    for (int i = first; i < w->n; ++i) {
        int found = 0;
        for (const PyAdamasTraceFrame* f = e->phys_extra; f; f = f->next)
            if (f->cfa == w->cfas[i]) { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}

void pa_trace_commit(PyAdamasException* e) {
    if (!pa_lang_gaps_rt || !e || !e->phys_extra) return;
    /* handler 帧 CFA 的两条来源：
       - 零开销路径：personality phase 2 钩子写的 handler_cfa（展开器口径，
         与捕获链的 CFA 逐字同源）。落地时 handler 自己的帧记录已被方案 G
         释放（cfa <= target 含等号），pa_frame_top 是调用方的记录，不能直接用。
       - setjmp 路径：pa_frame_unwind 停在 handler 记录上，pa_frame_top 就是
         handler 的记录。记录 cfa = rsp+frame_bytes+8，比 DWARF CFA 少压栈
         寄存器那一段，不能直接比——用地址锚点：记录地址落在 handler 帧内
         （handler_rsp <= rec < handler_CFA），比它大的最小节点 CFA 就是
         handler 帧（更内层帧的 CFA 都 <= handler_rsp < rec）。 */
    uintptr_t hcfa = e->handler_cfa;
    e->handler_cfa = 0;
    if (!hcfa && pa_frame_top) {
        uintptr_t rec = (uintptr_t)pa_frame_top;
        for (const PyAdamasTraceFrame* f = e->phys_extra; f; f = f->next)
            if (f->cfa > rec && (!hcfa || f->cfa < hcfa)) hcfa = f->cfa;
    }
    /* 锚点校验：handler 帧自己在待提交链里（抛出时它在栈上）→ 按 CFA 裁剪。
       不在链里分两种：
       - 全部节点都比 handler 更外层（cfa > hcfa）：裸 raise 被**重抛帧自己**
         的包围 try 接住（CPython 实测：异常没离开当前帧，外层帧一个不加）
         → 整链丢弃。
       - 其余（CFA 混杂，典型：生成器挂起后恢复到另一条栈，旧 CFA 失效）
         → 不裁剪整链保留，宁可多几帧也不按陈旧 CFA 误删。 */
    int anchored = 0, all_outer = 1;
    if (hcfa) {
        for (const PyAdamasTraceFrame* f = e->phys_extra; f; f = f->next) {
            if (f->cfa == hcfa) anchored = 1;
            if (f->cfa <= hcfa) all_outer = 0;
        }
    }
    int keep_all = !hcfa || (!anchored && !all_outer);
    PyAdamasTraceFrame* keep = NULL;
    PyAdamasTraceFrame** ktail = &keep;
    PyAdamasTraceFrame* f = e->phys_extra;
    while (f) {
        PyAdamasTraceFrame* nx = f->next;
        if (keep_all || f->cfa <= hcfa) {
            f->next = NULL;
            *ktail = f;
            ktail = &f->next;
        } else {
            free(f);  /* 比 handler 更外层的帧：异常没走到那里就被接住了 */
        }
        f = nx;
    }
    *ktail = e->phys;
    e->phys = keep;
    e->phys_extra = NULL;
}
