// rt_diag.c —— 运行时诊断工具
//
// 目标：把「对象在哪一步丢掉」变成可以读出来的事实，而不是靠猜所有权。
//
// 0.227 只用它追异常对象就定位了 119_gen_except_suspend 的 126 字节泄漏
// （写回帧槽后 status 短路跳过了释放）—— 而在那之前盲试三轮都失败。
// 这个文件把它扩成通用的运行时诊断设施。
//
// 开关（都是环境变量，不设时零成本 —— 只有一次 getenv 和一次自增）：
//
//   PYADAMAS_RC_TRACE=1        追踪所有 RC 对象的分配/释放
//   PYADAMAS_RC_TRACE_MIN=N    只追踪分配字节数 >= N 的对象（默认 0）
//   PYADAMAS_RC_TRACE_KIND=K   只追踪某类对象（str/list/dict/obj/...）
//   PYADAMAS_EXC_TRACE=1       只追踪异常对象（含所有权转移事件）
//   PYADAMAS_DIAG_DEPTH=N      调用点归因深度（默认 3，最大 8）
//
// 输出（stderr）：
//
//   [rc#12] alloc  str 96B rc=1  at rt_json.c:308  <- pa_json_dumps
//   [rc#12] free   str 96B rc=0  at rt_core.c:611  <- pa_rc_dec
//   [rc#12] LEAK   str 96B rc=1  at exit           <- pa_json_dumps
//
// 退出时汇总：
//   [diag] 未释放对象 2 个，共 152 字节
//     [rc#12] str 96B rc=1  分配于 pa_json_dumps (rt_json.c:308)
//     [exc#5] KeyError "inner" 分配于 pa_raise (rt_exc.c:660)
//
// 设计要点：
//
//   1. 外部哈希表，不改任何结构体布局。改布局会让 ASan 报的字节数变化，
//      前后对照就失效了（试过给 PyAdamasException 加字段：126 → 142）。
//
//   2. 分配点归因用 __builtin_return_address + dladdr，拿不到符号就退回
//      模块内偏移，总能给出「是哪个函数分配的」。
//
//   3. 悬挂指针检测：free 之后把记录留在表里标成 DEAD，再被 dec/inc 就
//      报 use-after-free —— 这类错误 ASan 在运行时内部未必看得到。
//
//   4. 容量满了不静默丢弃：置 overflow 标志，退出时明确提示「结果不完整」。
//      诊断工具撒谎比不工作更糟。
//
// dlfcn.h 必须在 rt_internal.h 之前 —— 后者带平台条件分支，
// 在某些顺序下会把 _GNU_SOURCE 的定义时机弄乱，导致 Dl_info 不可见。
#if defined(__linux__)
#define _GNU_SOURCE 1
#include <dlfcn.h>
#endif

#include <unwind.h>

#include "rt_internal.h"

#define DIAG_MAX 65536
#define DIAG_DEPTH_MAX 8

typedef enum {
    DIAG_FREE = 0,   /* 记录槽位空闲 */
    DIAG_LIVE = 1,   /* 对象存活 */
    DIAG_DEAD = 2    /* 已释放，保留记录用于悬挂指针检测 */
} DiagState;

typedef struct {
    void* ptr;
    int64_t id;
    int64_t size;
    int32_t kind;
    int32_t state;
    void* alloc_pc;              /* 分配点（第一帧） */
    void* free_pc;               /* 释放点（第一帧） */
    char alloc_site[192];        /* 分配点的完整调用链（分配时算好） */
} DiagSlot;

static DiagSlot diag_tab[DIAG_MAX];
static int64_t diag_seq;
static int64_t diag_nlive;
static int64_t diag_dead;
static int64_t diag_overflow;
static int64_t diag_bytes_live;

int pa_diag_rc_on = -1;   /* 供 rt_core 的热路径做内联判断 */
static int64_t diag_min_size;
static int diag_kind_filter = -1;
static int diag_depth = 3;

/* 异常对象追踪独立开关（0.227 的能力，保留）。 */
static int diag_exc_on = -1;
typedef struct {
    PyAdamasException* e;
    int64_t id;
    void* alloc_pc;
} DiagExcSlot;
static DiagExcSlot diag_exc[DIAG_MAX];
static int64_t diag_exc_seq;
static int64_t diag_exc_nlive;
static int64_t diag_exc_overflow;

int pa_debug_lookup_pc(uintptr_t pc, const char** file, const char** func, int32_t* line);

static void diag_report(void);

/* 在指定时刻快照「当前还活着」的对象。用来区分「真的没人释放」和
 * 「由 shutdown 路径统一释放」—— 后者不算泄漏。
 *
 * 已知局限：记录按**地址**索引，地址复用后旧事件会归属到新对象。
 * 排查长生命周期对象时要注意，必要时配合 PYADAMAS_EXC_TRACE 或
 * 在分配点打日志交叉验证。 */
void pa_diag_snapshot(const char* when) {
    int64_t n = 0, bytes = 0;
    for (size_t i = 0; i < DIAG_MAX; ++i)
        if (diag_tab[i].state == DIAG_LIVE) { ++n; bytes += diag_tab[i].size; }
    fprintf(stderr, "[diag] 快照 %s: 存活 %lld 个 / %lld 字节\n",
            when, (long long)n, (long long)bytes);
}

static const char* diag_kind_name(int32_t k) {
    switch (k) {
        case PYADAMAS_RC_STR: return "str";
        case PYADAMAS_RC_LIST: return "list";
        case PYADAMAS_RC_DICT: return "dict";
        case PYADAMAS_RC_OBJ: return "obj";
        case PYADAMAS_RC_OBJ_GEN: return "obj_gen";
        case PYADAMAS_RC_OBJ_DATETIME: return "obj_datetime";
        case PYADAMAS_RC_OBJ_TIMEDELTA: return "obj_timedelta";
        case PYADAMAS_RC_OBJ_PATH: return "obj_path";
        case PYADAMAS_RC_OBJ_DEQUE: return "obj_deque";
        case PYADAMAS_RC_OBJ_FILE: return "obj_file";
        case PYADAMAS_RC_OBJ_AWAIT: return "obj_await";
        case PYADAMAS_RC_OBJ_TIMEOUT: return "obj_timeout";
        case PYADAMAS_RC_OBJ_CLASS: return "obj_class";
        case PYADAMAS_RC_OBJ_COMPLEX: return "obj_complex";
        case PYADAMAS_RC_OBJ_EXCVALUE: return "obj_excvalue";
        case PYADAMAS_RC_OBJ_TB: return "obj_tb";
        case PYADAMAS_RC_OBJ_RANDOM: return "obj_random";
        case PYADAMAS_RC_OBJ_MATCH: return "obj_match";
        case PYADAMAS_RC_OBJ_REGEX: return "obj_regex";
        case PYADAMAS_RC_OBJ_PROC: return "obj_proc";
        case PYADAMAS_RC_OBJ_POPEN: return "obj_popen";
        case PYADAMAS_RC_OBJ_EVENT: return "obj_event";
        case PYADAMAS_RC_OBJ_LOCK: return "obj_lock";
        case PYADAMAS_RC_OBJ_QUEUE: return "obj_queue";
        case PYADAMAS_RC_OBJ_TASKGROUP: return "obj_taskgroup";
        case PYADAMAS_RC_OBJ_TEMPLATE: return "obj_template";
        case PYADAMAS_RC_OBJ_INTERP: return "obj_interp";
        case PYADAMAS_RC_OBJ_VT: return "obj_vt";
        case PYADAMAS_RC_OBJ_CHAN: return "obj_chan";
        case PYADAMAS_RC_OBJ_SEM: return "obj_sem";
        case PYADAMAS_RC_STRUCT: return "struct";
        case PYADAMAS_RC_JSONNULL: return "jsonnull";
        case PYADAMAS_RC_JSONBOOL: return "jsonbool";
        default: return "other";
    }
}

static int diag_kind_from_env(const char* v) {
    if (!v) return -1;
    if (!strcmp(v, "str")) return PYADAMAS_RC_STR;
    if (!strcmp(v, "list")) return PYADAMAS_RC_LIST;
    if (!strcmp(v, "dict")) return PYADAMAS_RC_DICT;
    if (!strcmp(v, "obj")) return PYADAMAS_RC_OBJ;
    if (!strcmp(v, "obj_gen")) return PYADAMAS_RC_OBJ_GEN;
    if (!strcmp(v, "struct")) return PYADAMAS_RC_STRUCT;
    return -1;
}

/* 把返回地址翻译成「函数名 (file:line)」。拿不到符号就只给模块偏移。 */
static void diag_sym(void* pc, char* out, size_t n) {
    out[0] = '\0';
    if (!pc) {
        snprintf(out, n, "?");
        return;
    }
#if defined(__linux__)
    Dl_info info;
    if (dladdr(pc, &info) && info.dli_sname) {
        snprintf(out, n, "%s", info.dli_sname);
        return;
    }
#endif
    snprintf(out, n, "%p", pc);
}

/* 调用点归因。
 *
 * __builtin_return_address(n>0) 需要编译期常量、还会被 -Werror=frame-address
 * 拦下，走不通。改用 _Unwind_Backtrace（rt_debug.c 的 traceback 已经在用
 * 同一套机制）：按 .eh_frame 逐帧还原，不依赖帧指针。
 *
 * 只保留**生成代码**的帧（查得到 PyAdamas 的 PC→(函数,行号) 表），因为
 * 运行时自己的帧（rc_alloc / pa_rc_dec）对定位没有信息量。 */
typedef struct {
    char buf[512];
    size_t used;
    int skip;      /* 跳过最前面几帧（本函数自己 + 调用者） */
    int max_frames;
    int n;
} DiagWalk;

static _Unwind_Reason_Code diag_walk_cb(struct _Unwind_Context* ctx, void* arg) {
    DiagWalk* w = (DiagWalk*)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (!ip) return _URC_END_OF_STACK;
    if (w->skip > 0) {
        --w->skip;
        return _URC_NO_REASON;
    }
    const char* file;
    const char* func;
    int32_t line;
    /* 优先用生成代码的符号表（带行号），退回 dladdr（运行时符号）。 */
    char sym[192];
    if (pa_debug_lookup_pc(ip, &file, &func, &line)) {
        const char* base = strrchr(file, '/');
        snprintf(sym, sizeof sym, "%s (%s:%d)", func, base ? base + 1 : file, (int)line);
    } else {
        char raw[96];
        diag_sym((void*)ip, raw, sizeof raw);
        if (!strcmp(raw, "?") || raw[0] == '0') return _URC_NO_REASON;
        snprintf(sym, sizeof sym, "%s", raw);
    }
    int w2 = snprintf(w->buf + w->used, sizeof(w->buf) - w->used,
                      "%s%s", w->used ? " <- " : "", sym);
    if (w2 < 0 || (size_t)w2 >= sizeof(w->buf) - w->used) return _URC_END_OF_STACK;
    w->used += (size_t)w2;
    if (++w->n >= w->max_frames) return _URC_END_OF_STACK;
    return _URC_NO_REASON;
}

static void diag_callsite(char* out, size_t n) {
    if (getenv("PYADAMAS_DIAG_NOWALK")) { snprintf(out, n, "?"); return; }
    DiagWalk w;
    w.used = 0;
    w.buf[0] = '\0';
    w.skip = 3;      /* diag_walk_cb <- _Unwind_Backtrace <- diag_callsite <- caller */
    w.max_frames = diag_depth;
    w.n = 0;
    _Unwind_Backtrace(diag_walk_cb, &w);
    if (!w.used) snprintf(w.buf, sizeof w.buf, "?");
    snprintf(out, n, "%s", w.buf);
}

static int diag_rc_enabled(void) {
    if (pa_diag_rc_on < 0) {
        const char* v = getenv("PYADAMAS_RC_TRACE");
        pa_diag_rc_on = (v && *v && *v != '0') ? 1 : 0;
        const char* m = getenv("PYADAMAS_RC_TRACE_MIN");
        diag_min_size = m ? atoll(m) : 0;
        diag_kind_filter = diag_kind_from_env(getenv("PYADAMAS_RC_TRACE_KIND"));
        const char* d = getenv("PYADAMAS_DIAG_DEPTH");
        diag_depth = d ? (int)atoll(d) : 3;
        if (diag_depth < 1) diag_depth = 1;
        if (diag_depth > DIAG_DEPTH_MAX) diag_depth = DIAG_DEPTH_MAX;
        if (pa_diag_rc_on) atexit(diag_report);
    }
    return pa_diag_rc_on;
}

static int diag_exc_enabled(void) {
    if (diag_exc_on < 0) {
        const char* v = getenv("PYADAMAS_EXC_TRACE");
        diag_exc_on = (v && *v && *v != '0') ? 1 : 0;
        if (diag_exc_on) atexit(diag_report);
    }
    return diag_exc_on;
}

/* 查找/插入都用**线性扫描**。
 *
 * 之前用开放寻址哈希，踩了两个坑：
 *   1. 复用「地址不同」的 DEAD 槽时，新记录落在 hash(旧地址)+k 的位置，
 *      而 diag_find(新地址) 从 hash(新地址) 开始探测 —— 找不到。表现是
 *      「刚 alloc 完，紧接着的 temp-push 报 UNTRACKED」。
 *   2. 地址复用后旧记录残留，事件归属错乱。
 *
 * 活跃对象只有几十个，O(n) 线性扫描完全够，而且没有这些不变量问题。
 * 只有诊断开启时才走这条路，不影响正常运行。 */
static DiagSlot* diag_find(void* p, int want_dead) {
    for (size_t i = 0; i < DIAG_MAX; ++i) {
        DiagSlot* s = &diag_tab[i];
        if (s->state == DIAG_FREE) continue;
        if (s->ptr != p) continue;
        if (s->state == DIAG_DEAD && !want_dead) continue;
        return s;
    }
    return NULL;
}

/* 找槽位插入：同地址的旧记录（无论 LIVE/DEAD）原地复用，否则用第一个空位。
 * 绝不覆盖「地址不同的 LIVE」——那会让真实泄漏从报告里消失。 */
static DiagSlot* diag_insert(void* p) {
    DiagSlot* empty = NULL;
    DiagSlot* dead_other = NULL;
    for (size_t i = 0; i < DIAG_MAX; ++i) {
        DiagSlot* s = &diag_tab[i];
        if (s->state == DIAG_FREE) {
            if (!empty) empty = s;
            continue;
        }
        if (s->ptr == p) {
            /* 同地址：只有 DEAD 才能原地复用（地址被 malloc 回收再分配）。
             * LIVE 说明**上一个同地址对象还没释放** —— 那是运行时 bug，
             * 但工具不能因此把旧记录覆盖掉，否则两个对象混成一条，
             * 事件归属全乱。给它新槽，让两条记录并存。 */
            if (s->state == DIAG_DEAD) {
                --diag_dead;
                return s;
            }
            continue;
        }
        if (s->state == DIAG_DEAD && !dead_other) dead_other = s;
    }
    if (empty) return empty;
    if (dead_other) {
        ++diag_overflow;
        return dead_other;
    }
    return NULL;
}

void pa_diag_rc_alloc(void* p, int64_t size, int32_t kind) {
    if (!p || !diag_rc_enabled()) return;
    if (size < diag_min_size) return;
    if (diag_kind_filter >= 0 && kind != diag_kind_filter) return;
    DiagSlot* s = diag_insert(p);
    if (!s) {
        ++diag_overflow;
        return;
    }
    if (s->state == DIAG_DEAD) --diag_dead;
    s->ptr = p;
    s->id = ++diag_seq;
    s->size = size;
    s->kind = kind;
    s->state = DIAG_LIVE;
    s->alloc_pc = __builtin_return_address(0);
    s->free_pc = NULL;
    ++diag_nlive;
    diag_bytes_live += size;
    diag_callsite(s->alloc_site, sizeof s->alloc_site);
    fprintf(stderr, "[rc#%lld] alloc  %-6s %lldB rc=%lld marked=%lld  <- %s\n",
            (long long)s->id, diag_kind_name(kind), (long long)size,
            (long long)((PyAdamasRc*)p - 1)->rc,
            (long long)((PyAdamasRc*)p - 1)->marked, s->alloc_site);
}

void pa_diag_rc_free(void* p, int32_t kind) {
    if (!p || !diag_rc_enabled()) return;
    DiagSlot* s = diag_find(p, 0);
    if (!s) {
        /* 没追踪到（被 MIN/KIND 过滤掉），但可能是释放一个从未分配过的
         * 指针 —— 这种情况值得报，说明表漏了。 */
        DiagSlot* d = diag_find(p, 1);
        if (d && d->state == DIAG_DEAD) {
            fprintf(stderr, "[rc#%lld] DOUBLE-FREE %s at %p（已在此前释放）\n",
                    (long long)d->id, diag_kind_name(d->kind), p);
        }
        return;
    }
    s->state = DIAG_DEAD;
    s->free_pc = __builtin_return_address(0);
    --diag_nlive;
    ++diag_dead;
    diag_bytes_live -= s->size;
    char sym[256];
    diag_callsite(sym, sizeof sym);
    fprintf(stderr, "[rc#%lld] free   %-6s %lldB rc=0  <- %s\n",
            (long long)s->id, diag_kind_name(s->kind), (long long)s->size, sym);
    (void)kind;
}

/* 悬挂指针检测：对已释放对象做 inc/dec 时调用。 */
void pa_diag_check_use(void* p, const char* what) {
    if (!p || !diag_rc_enabled()) return;
    DiagSlot* s = diag_find(p, 1);
    if (s && s->state == DIAG_DEAD) {
        char alloc_sym[128];
        diag_sym(s->alloc_pc, alloc_sym, sizeof alloc_sym);
        fprintf(stderr, "[rc#%lld] USE-AFTER-FREE %s %s at %p（分配于 %s）\n",
                (long long)s->id, what, diag_kind_name(s->kind), p, alloc_sym);
    }
}

/* 临时栈事件：配合 RC 追踪看「进了栈却没被 pop」的对象。
 * 只在 PYADAMAS_RC_TRACE 开启时调用（rt_core 侧有全局标志短路）。 */
/* 临时栈事件。
 *
 * **不能靠地址反查归属**：地址会被立刻复用，于是「旧对象的 push」会被算到
 * 「刚在同一地址分配的新对象」头上，得出「push 了两次、只 pop 一次」这种
 * 假象（实测 53_json 的 rc#20）。
 *
 * 正确做法是在 push 的那一刻就记下当时的分配序号，随栈项一起保存 ——
 * 但 temp_stack 是 int64_t* 数组，塞不下额外字段。所以这里改为
 * **按栈深+序号配对打印**：push 时记 (depth, id)，pop 时用同样的 depth
 * 找最近一次同深的 push。两边都带 id，归属就不会错。 */
typedef struct {
    int64_t depth;
    int64_t id;
    int32_t kind;
    int32_t used;
} TempMark;
#define TEMP_MARK_MAX 256
static TempMark temp_marks[TEMP_MARK_MAX];

void pa_diag_temp(const char* what, void* p, int64_t depth) {
    if (!p) return;
    DiagSlot* s = diag_find(p, 0);
    if (!s) {
        /* 不在表里：打印 RC 头，区分「被 MIN/KIND 过滤掉」（正常）和
         * 「根本没走 rc_alloc」（真 bug）。 */
        PyAdamasRc* h = (PyAdamasRc*)p - 1;
        fprintf(stderr, "[diag] temp-%-4s UNTRACKED ptr=%p depth=%lld "
                        "kind=%lld rc=%lld size=%lld\n",
                what, p, (long long)depth, (long long)h->kind,
                (long long)h->rc, (long long)h->size);
        return;
    }
    if (what[1] == 'u') {   /* push（"push" 第 2 个字符是 u，"pop" 是 o） */
        for (int i = 0; i < TEMP_MARK_MAX; ++i) {
            if (temp_marks[i].used) continue;
            temp_marks[i].used = 1;
            temp_marks[i].depth = depth;
            temp_marks[i].id = s->id;
            temp_marks[i].kind = s->kind;
            break;
        }
        fprintf(stderr, "[rc#%lld] temp-push %s depth=%lld\n",
                (long long)s->id, diag_kind_name(s->kind), (long long)depth);
    } else {                /* pop */
        int64_t id = s->id;
        for (int i = 0; i < TEMP_MARK_MAX; ++i) {
            if (!temp_marks[i].used || temp_marks[i].depth != depth) continue;
            id = temp_marks[i].id;
            temp_marks[i].used = 0;
            break;
        }
        fprintf(stderr, "[rc#%lld] temp-pop  %s depth=%lld%s\n",
                (long long)id, diag_kind_name(s->kind), (long long)depth,
                id != s->id ? "  [地址已复用，按 push 时的序号归属]" : "");
    }
}

/* ── 异常对象追踪（0.227 能力，保留并加上分配点归因） ── */

static int64_t diag_exc_id_of(PyAdamasException* e) {
    for (int64_t i = 0; i < diag_exc_nlive; ++i)
        if (diag_exc[i].e == e) return diag_exc[i].id;
    return 0;
}

void pa_exc_trace_alloc(PyAdamasException* e) {
    if (!e || !diag_exc_enabled()) return;
    int64_t id = ++diag_exc_seq;
    if (diag_exc_nlive < DIAG_MAX) {
        diag_exc[diag_exc_nlive].e = e;
        diag_exc[diag_exc_nlive].id = id;
        diag_exc[diag_exc_nlive].alloc_pc = __builtin_return_address(0);
        ++diag_exc_nlive;
    } else {
        ++diag_exc_overflow;
    }
    char sym[128];
    diag_sym(__builtin_return_address(0), sym, sizeof sym);
    fprintf(stderr, "[exc#%lld] alloc  %-16s \"%s\"  <- %s\n",
            (long long)id, e->type, e->message, sym);
}

/* 所有权转移（写回帧槽、交给别的持有者），不是释放。 */
void pa_exc_trace_move(PyAdamasException* e, const char* what, void* where) {
    if (!e || !diag_exc_enabled()) return;
    int64_t id = diag_exc_id_of(e);
    if (id == 0) return;
    fprintf(stderr, "[exc#%lld] %-6s %-16s at %p\n",
            (long long)id, what, e->type ? e->type : "?", where);
}

void pa_exc_trace_free(PyAdamasException* e) {
    if (!e || !diag_exc_enabled()) return;
    int64_t id = 0;
    for (int64_t i = 0; i < diag_exc_nlive; ++i) {
        if (diag_exc[i].e == e) {
            id = diag_exc[i].id;
            diag_exc[i] = diag_exc[--diag_exc_nlive];
            break;
        }
    }
    if (id == 0) {
        fprintf(stderr, "[exc#?] free of UNTRACKED %-16s at %p（重复释放？）\n",
                e->type ? e->type : "?", (void*)e);
        return;
    }
    fprintf(stderr, "[exc#%lld] free   %-16s at %p\n",
            (long long)id, e->type ? e->type : "?", (void*)e);
}

/* ── 退出汇总 ── */

static void diag_report(void) {
    int64_t nlive = 0, bytes = 0;
    for (size_t i = 0; i < DIAG_MAX; ++i)
        if (diag_tab[i].state == DIAG_LIVE) {
            ++nlive;
            bytes += diag_tab[i].size;
        }

    if (diag_overflow)
        fprintf(stderr, "[diag] RC 表溢出 %lld 次，以下结果不完整\n",
                (long long)diag_overflow);
    if (diag_exc_overflow)
        fprintf(stderr, "[diag] 异常表溢出 %lld 次，以下结果不完整\n",
                (long long)diag_exc_overflow);

    if (nlive == 0 && diag_exc_nlive == 0) {
        fprintf(stderr, "[diag] 无未释放对象\n");
        return;
    }

    if (nlive > 0) {
        fprintf(stderr, "[diag] 未释放 RC 对象 %lld 个，共 %lld 字节：\n",
                (long long)nlive, (long long)bytes);
        for (size_t i = 0; i < DIAG_MAX; ++i) {
            DiagSlot* s = &diag_tab[i];
            if (s->state != DIAG_LIVE) continue;
            fprintf(stderr, "  [rc#%lld] %-6s %lldB at %p\n        分配于 %s\n",
                    (long long)s->id, diag_kind_name(s->kind),
                    (long long)s->size, s->ptr, s->alloc_site);
        }
    }
    if (diag_exc_nlive > 0) {
        fprintf(stderr, "[diag] 未释放异常对象 %lld 个：\n",
                (long long)diag_exc_nlive);
        for (int64_t i = 0; i < diag_exc_nlive; ++i) {
            PyAdamasException* e = diag_exc[i].e;
            char sym[128];
            diag_sym(diag_exc[i].alloc_pc, sym, sizeof sym);
            fprintf(stderr, "  [exc#%lld] %-16s \"%s\"  at %p  分配于 %s\n",
                    (long long)diag_exc[i].id, e->type ? e->type : "?",
                    e->message ? e->message : "", (void*)e, sym);
        }
    }
}
