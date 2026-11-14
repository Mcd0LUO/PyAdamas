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
        if (pa_debug_lookup_pc(w.pcs[i], &file, &func, &line))
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
