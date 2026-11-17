// rt_core.c —— 引用计数、临时值栈、闭包、字符串分配、循环收集器
#define PA_RC_NO_INLINE   /* 本文件提供 pa_rc_inc/dec 的外部符号 */
#include "rt_internal.h"

/* ── 引用计数 ─────────────────────────────────────────────────── */

/* 隐藏头放在负载前面，负载指针（字符串的 len、对象的 vtable）保持原样。
   字面量在 .data.rel.ro 里静态构造，rc 是 INT64_MIN，inc/dec 直接跳过。

   头同时是循环收集器的注册表节点：
   - dtor：正常引用计数归零时调用（负责 dec 子对象、释放列表 data）；
   - release：GC 判断对象不可达时调用，只把「指向可达对象的引用」减掉，
     不释放任何东西（子对象由清扫阶段统一处理）；
   - size/kind：GC 保守扫描负载时要知道范围。 */

PyAdamasRc* heap_head;
int64_t alloc_since_gc;
int gc_ready;
int gc_sweeping;
/* 链上的对象数（叶子不进链，不计）。增量维护，收集时不用再数一遍。 */
size_t gc_tracked_count;
/* 链上「负载需要保守扫描」的对象数（OBJ/实例）。为 0 且 exact-RC 时
   GC 可以走精确快速路径：不需要地址表。 */
size_t gc_conservative_count;

#define PYADAMAS_RC_MARK 1
#define PYADAMAS_RC_UNTRACKED 2
#define PYADAMAS_RC_YOUNG 4
/* 诊断模式专用：该对象首次是在保守根扫描阶段被发现的（没有精确根能到
   它）。只用于统计误报保留量，正常模式下永远不置位。 */
#define PYADAMAS_RC_CONS 8

/* ── 分代收集状态（分配热路径和 rc_link 都要用） ──────────────
   young_buf 是上次收集后分配、还没晋升的对象；marked 高位是它在数组里
   的下标，正常 RC 释放时 O(1) 摘除。old_bytes 是老生代存活字节，用来
   决定 major 的触发阈值。 */
static PyAdamasRc** young_buf;
static size_t young_len;
static size_t young_cap;
static int64_t old_bytes;

/* 收集阶段：非 0 时 pa_rc_dec 只调整集合内对象的计数，不直接释放。 */
#define GC_PHASE_NONE 0
#define GC_PHASE_TRIAL 1
#define GC_PHASE_RESTORE 2
#define GC_PHASE_SWEEP 3
/* 增量标记期间：RC 的 dec 不能立即释放（标记队列可能还指向它），
   也不能像 TRIAL/RESTORE 那样只调计数 —— 统一登记到 gc_pend，
   本轮收尾时统一处理。 */
#define GC_PHASE_INCR_MARK 4
int pa_gc_phase_fast;   /* gc_phase 的公开别名，供内联 inc/dec 判断 */
#define gc_phase pa_gc_phase_fast
/* major 走经典标记-清扫，minor 走 trial-deletion。清扫时的子引用规则
   不同：标记-清扫只减可达子对象，trial 模式跳过集合内（已减过）、
   收集集合外的边。 */
static int gc_mark_sweep;

/* minor 按新生代分配量触发；major 按「上次 major 以来」的分配量触发，
   阈值随老生代存活字节增长（和旧的全堆扫描同一个摊还口径）。 */
static int64_t alloc_since_major;
static int64_t young_threshold = PYADAMAS_GC_THRESHOLD;
static int64_t major_threshold = 4 * PYADAMAS_GC_THRESHOLD;
/* 连续「major 零回收」的次数，用于退避（见 gc_collect 收尾）。 */
static int gc_zero_free_streak;

/* 每个分配点推进多少个对象的标记。Go 用「扫描字节数」算预算，
   这里简化成对象数 —— 我们的对象大小分布比 Go 集中（字符串/列表为主）。 */
/* 标记预算：每次分配点推进多少个对象的标记。 */
#define GC_MARK_BUDGET 64
/* 清扫预算：清扫比标记便宜（只是 free + 跑 dtor），可以批量更大。
 *
 * 本地化优化：Go 的 sweep 是后台 goroutine 持续做的，我们挂在分配点上，
 * 每次调用的固定开销（函数调用 + 状态判断）占比更高。实测 64 太小 ——
 * 每次分配都进一次 gc_incr_step，吞吐掉 5ms。调到 512 后固定开销摊薄。 */
#define GC_SWEEP_BUDGET 512
/* 增量阶段两次推进之间隔多少次分配。
 *
 * 本地化优化（Go 没有对应物）：Go 的后台 worker 是独立 goroutine，推进
 * 与分配完全解耦；我们挂在分配点上，每次分配都判断一次状态有固定开销。
 * 加一层倒计数器，把「每次分配一次调用」摊成「每 N 次一次」。
 *
 * 取 16：清扫预算 512 个对象 / 16 次分配 ≈ 每次分配摊 32 个对象的清扫量，
 * 单次停顿仍在亚毫秒级，但函数调用次数降到 1/16。 */
#define GC_INCR_STRIDE 16
/* 集合小于这个规模时同步清扫（不分片）。见 gc_collect 里的说明。 */
#define GC_INCR_MIN_SET 20000

/* 增量标记的状态：0 = 空闲，1 = 标记中，2 = 清扫中。 */
static int gc_incr_state;
static int64_t gc_incr_major;   /* 本轮是不是整堆收集 */
static size_t gc_incr_sweep_pos;
static int gc_incr_countdown;
static int gc_incr_remaining;
static void gc_incr_finish(void);
static void gc_incr_step(void);
static void gc_free_body(PyAdamasRc* h);
static void gc_alloc_black(PyAdamasRc* h);

static void gc_collect(int major);
static void gc_pend_add(int64_t v);

static void young_add(PyAdamasRc* h) {
    if (young_len == young_cap) {
        size_t ncap = young_cap ? young_cap * 2 : 1024;
        PyAdamasRc** ns = (PyAdamasRc**)realloc(young_buf, sizeof(PyAdamasRc*) * ncap);
        if (!ns) panic_c("out of memory");
        young_buf = ns;
        young_cap = ncap;
    }
    h->marked = ((int64_t)young_len << 8) | PYADAMAS_RC_YOUNG;
    young_buf[young_len++] = h;
}

static void young_del(PyAdamasRc* h) {
    size_t i = (size_t)(h->marked >> 8);
    if (i >= young_len || young_buf[i] != h) return;
    size_t last = --young_len;
    if (i != last) {
        PyAdamasRc* m = young_buf[last];
        young_buf[i] = m;
        m->marked = ((int64_t)i << 8) | (m->marked & 0xFF);
    }
    h->marked = 0;
}

/* 收集期间的子引用处理：
   - trial：集合内对象 rc--（试探性减计数）；
   - restore：集合内对象 rc++（把存活对象的出边加回去）；
   - sweep：集合内对象已经减过，跳过；集合外（老对象）的边攒进 pending，
     回合结束后按正常 RC 释放，避免清扫中重入释放。
   永生字面量在入口就跳过了；exact 模式下的叶子不在集合里，清扫时按
   普通引用释放。 */
static void gc_phase_dec(PyAdamasRc* h, void* p) {
    if (gc_phase == GC_PHASE_TRIAL) {
        if (h->marked & PYADAMAS_RC_YOUNG) h->rc--;
        return;
    }
    if (gc_phase == GC_PHASE_RESTORE) {
        if (h->marked & PYADAMAS_RC_YOUNG) h->rc++;
        return;
    }
    if (gc_phase == GC_PHASE_INCR_MARK) {
        // 标记期间对象归零：统一登记待释放，本轮收尾处理。
        // 不区分新老对象 —— 新对象也会进 gc_pend，而 gc_incr_finish 里
        // gc_phase 已复位，pa_rc_dec 会走正常释放路径。
        gc_pend_add((int64_t)(uintptr_t)p);
        return;
    }
    if (h->marked & PYADAMAS_RC_UNTRACKED) {
        if (h->rc > 0 && --h->rc == 0) free(h);
        return;
    }
    if (gc_mark_sweep) {
        if ((h->marked & PYADAMAS_RC_MARK) && h->rc > 0) h->rc--;
        return;
    }
    if (h->marked & PYADAMAS_RC_YOUNG) return;
    gc_pend_add((int64_t)(uintptr_t)p);
}

/* 存活堆索引只服务 unknown 值的保守 retain/dec（值可能是整数，必须先
   确认是堆指针）。整程序没有任何 unknown 时，入口会调用
   pa_disable_unknown_index 关掉它，省掉每个堆对象的插入/删除。 */
static int rc_index_off;
/* **只读模式**：索引维护关闭，但查询仍然工作（返回「不知道」）。
 *
 * 用途见 docs/roadmap.md 的方案 C：编译器能证明一个模块从不做「反查这个字
 * 是不是堆对象」的操作时（纯同步容器代码、没有生成器/协程/异常类/unknown
 * 槽），索引的插入和删除是纯开销 —— 实测 bench_rc_gen 里占 22ms（102 → 80）。
 *
 * 与 rc_index_off 的区别：rc_index_off 是「索引不存在」，所有查询都失败；
 * rc_index_readonly 是「索引不更新」，查询也返回失败，但语义上调用方应该
 * 已经知道不会有依赖索引的代码路径（由编译器保证）。 */
static int rc_index_readonly;

/* exact-RC 模式（= 索引关闭）下，字符串这类叶子对象不进 GC 堆链：
   RC 是精确的，不可达字符串在 rc 归零时就释放了，GC 永远不需要看到
   它们；而它们又不可能成环。标记没进链的对象用 marked=2 区分，
   清扫垃圾容器时对这类子对象直接走正常 RC 释放。 */
static int gc_untracked_leaves;

/* RC/GC 统计：PYADAMAS_RC_STATS=1 时在退出时打印。
   统计本身只是一个可预测的全局分支，不改变所有权语义。 */
int pa_rc_stats_on_fast;   /* rc_stats_on 的公开别名（内联快路径要读） */
#define rc_stats_on pa_rc_stats_on_fast
static int64_t stat_rc_inc;
static int64_t stat_rc_inc_unknown;
static int64_t stat_rc_dec;
static int64_t stat_rc_dec_unknown;
static int64_t stat_rc_temp;
static int64_t stat_rc_cleanup;
static int64_t stat_rc_free;
static int64_t stat_gc_runs;
static int64_t stat_gc_minor;
static int64_t stat_gc_major;
static int64_t stat_gc_freed;
static int64_t stat_gc_live;
static int64_t stat_take_calls;
static int64_t stat_take_steps;

/* ── GC 诊断（PYADAMAS_GC_STATS=1）与收窄回退（PYADAMAS_GC_CONSERVATIVE=1）
   ─────────────────────────────────────────────────────────────────
   两个开关都只在入口 pa_gc_init 里各读一次环境变量、缓存成全局 int；
   之后只在 GC 路径上判断，分配 / RC 热路径一个字节都不多读。
   诊断统计关闭时 GC 走原来的单遍扫描路径，行为与加统计前完全一致。 */
static int gc_stats_on;
static int gc_conservative;
/* 统计模式的保守根阶段：非 0 时 gc_mark/gc_mark_direct 会给「首次由
   保守根发现」的对象打上 PYADAMAS_RC_CONS 并计入本轮保留量。 */
static int gc_cons_phase;
static int gc_cons_src;  /* 0 = 临时区 / 1 = unknown 槽 / 2 = 寄存器 */
static int64_t gc_cons_objs[3];
static int64_t gc_cons_bytes[3];
/* 本轮根扫描分解（gc_stats_roots 填）。 */
static int64_t gc_stat_frames;
static int64_t gc_stat_slots;
static int64_t gc_stat_temp_words_full;
static int64_t gc_stat_temp_words_scanned;
static int64_t gc_stat_reg_words;
/* 全程累计。 */
static int64_t gc_stat_runs;
static int64_t gc_stat_minor;
static int64_t gc_stat_major;
static double gc_stat_total_ms;
static double gc_stat_max_ms;
static int64_t gc_stat_cons_objs_total[3];
static int64_t gc_stat_cons_bytes_total[3];

/* 记录「只由保守根发现」的对象：同一个对象只记一次（CONS 位）。
   字节数按负载 + 隐藏头算，和 GC 自己的记账口径一致。 */
static void gc_cons_note(PyAdamasRc* h) {
    if (h->marked & PYADAMAS_RC_CONS) return;
    h->marked |= PYADAMAS_RC_CONS;
    gc_cons_objs[gc_cons_src] += 1;
    gc_cons_bytes[gc_cons_src] += h->size + PYADAMAS_RC_HDR;
}

/* 本轮根扫描分解 + 位图收窄量。只在统计模式调用：O(帧数 + 位图字数)。 */
static void gc_stats_roots(int with_regs) {
    gc_stat_frames = 0;
    gc_stat_slots = 0;
    gc_stat_temp_words_full = 0;
    gc_stat_temp_words_scanned = 0;
    for (PyAdamasFrame* f = pa_frame_top; f; f = f->prev) {
        gc_stat_frames++;
        gc_stat_slots += f->n;
        if (f->scan_words <= 0 || !f->scan_base) continue;
        gc_stat_temp_words_full += f->scan_words;
        if (f->scan_mask && !gc_conservative) {
            int64_t n = 0;
            for (int64_t w = 0; w < f->scan_words; ++w)
                n += (int64_t)((f->scan_mask[w >> 6] >> (w & 63)) & 1);
            gc_stat_temp_words_scanned += n;
        } else {
            gc_stat_temp_words_scanned += f->scan_words;
        }
    }
    // jmp_buf 按机器字整段扫，字数固定；用 / 8 避免 sizeof 数组除法的告警。
    gc_stat_reg_words = with_regs ? (int64_t)(sizeof(jmp_buf) / 8) : 0;
}

/* 每回合一行诊断：规模、耗时、存活量、根扫描分解、误报保留量。 */
static void gc_stats_report(int major, size_t n_set, int64_t freed, double ms) {
    gc_stat_runs++;
    if (major) gc_stat_major++;
    else gc_stat_minor++;
    gc_stat_total_ms += ms;
    if (ms > gc_stat_max_ms) gc_stat_max_ms = ms;
    int64_t cons_objs = 0, cons_bytes = 0;
    for (int i = 0; i < 3; ++i) {
        cons_objs += gc_cons_objs[i];
        cons_bytes += gc_cons_bytes[i];
        gc_stat_cons_objs_total[i] += gc_cons_objs[i];
        gc_stat_cons_bytes_total[i] += gc_cons_bytes[i];
    }
    fprintf(stderr,
            "[gcstats] #%lld %s %.3fms set=%zu freed=%lld live_objs=%zu "
            "live_bytes=%lld\n",
            (long long)gc_stat_runs, major ? "major" : "minor", ms, n_set,
            (long long)freed, gc_tracked_count, (long long)old_bytes);
    fprintf(stderr,
            "[gcstats]   roots frames=%lld slots=%lld temp_words=%lld/%lld "
            "reg_words=%lld mask=%s\n",
            (long long)gc_stat_frames, (long long)gc_stat_slots,
            (long long)gc_stat_temp_words_scanned,
            (long long)gc_stat_temp_words_full, (long long)gc_stat_reg_words,
            gc_conservative ? "off(conservative)"
                            : ((gc_stat_temp_words_full > gc_stat_temp_words_scanned)
                                   ? "on" : "none"));
    fprintf(stderr,
            "[gcstats]   cons_kept %lld objs / %lld bytes "
            "(temp=%lld/%lld slot=%lld/%lld reg=%lld/%lld)\n",
            (long long)cons_objs, (long long)cons_bytes,
            (long long)gc_cons_objs[0], (long long)gc_cons_bytes[0],
            (long long)gc_cons_objs[1], (long long)gc_cons_bytes[1],
            (long long)gc_cons_objs[2], (long long)gc_cons_bytes[2]);
}

/* 退出时打全程汇总。 */
static void gc_stats_dump(void) {
    fprintf(stderr,
            "[gcstats] total runs=%lld minor=%lld major=%lld total_ms=%.3f "
            "avg_ms=%.3f max_ms=%.3f\n",
            (long long)gc_stat_runs, (long long)gc_stat_minor,
            (long long)gc_stat_major, gc_stat_total_ms,
            gc_stat_runs ? gc_stat_total_ms / (double)gc_stat_runs : 0.0,
            gc_stat_max_ms);
    fprintf(stderr,
            "[gcstats] cons_kept total=%lld objs / %lld bytes "
            "(temp=%lld/%lld slot=%lld/%lld reg=%lld/%lld)\n",
            (long long)(gc_stat_cons_objs_total[0] + gc_stat_cons_objs_total[1] +
                        gc_stat_cons_objs_total[2]),
            (long long)(gc_stat_cons_bytes_total[0] + gc_stat_cons_bytes_total[1] +
                        gc_stat_cons_bytes_total[2]),
            (long long)gc_stat_cons_objs_total[0],
            (long long)gc_stat_cons_bytes_total[0],
            (long long)gc_stat_cons_objs_total[1],
            (long long)gc_stat_cons_bytes_total[1],
            (long long)gc_stat_cons_objs_total[2],
            (long long)gc_stat_cons_bytes_total[2]);
}

static void rc_stats_dump(void) {
    fprintf(stderr,
            "[rc] inc=%lld inc_unknown=%lld dec=%lld dec_unknown=%lld temp=%lld "
            "cleanup=%lld free=%lld gc_runs=%lld gc_minor=%lld gc_major=%lld "
            "gc_freed=%lld gc_live=%lld take=%lld take_steps=%lld\n",
            (long long)stat_rc_inc, (long long)stat_rc_inc_unknown,
            (long long)stat_rc_dec,
            (long long)stat_rc_dec_unknown, (long long)stat_rc_temp,
            (long long)stat_rc_cleanup, (long long)stat_rc_free,
            (long long)stat_gc_runs, (long long)stat_gc_minor,
            (long long)stat_gc_major, (long long)stat_gc_freed,
            (long long)stat_gc_live, (long long)stat_take_calls,
            (long long)stat_take_steps);
}

/* 短字符串 freelist。split / capitalize / join 这类路径会高频产生
   几个字节的小字符串，每次都 malloc/free 会把内存分配变成热点。
   按 8 字节粒度分档（<=128 字节），每档最多缓存 64 块。复用时块容量
   一定 >= 请求大小（分配时向上取整到同一档），字符串仍按自己的 len 访问。 */
#define STR_POOL_MAX_SIZE 128
#define STR_POOL_CLASSES (STR_POOL_MAX_SIZE / 8)
#define STR_POOL_MAX_PER_CLASS 64
static PyAdamasRc* str_pool[STR_POOL_CLASSES];
static int str_pool_count[STR_POOL_CLASSES];

static int str_pool_class(int64_t size) {
    if (size <= 0 || size > STR_POOL_MAX_SIZE) return -1;
    return (int)((size + 7) / 8) - 1;
}

static PyAdamasRc* str_pool_get(int64_t size) {
    int c = str_pool_class(size);
    if (c < 0 || !str_pool[c]) return NULL;
    PyAdamasRc* h = str_pool[c];
    str_pool[c] = h->next;
    str_pool_count[c]--;
    return h;
}

static void str_pool_put(PyAdamasRc* h) {
    int c = str_pool_class(h->size);
    if (c < 0 || str_pool_count[c] >= STR_POOL_MAX_PER_CLASS) {

        free(h);
        return;
    }
    h->next = str_pool[c];
    h->prev = NULL;
    str_pool[c] = h;
    str_pool_count[c]++;
}

/* 列表结构体 freelist：PyAdamasList 固定 32 字节，单独一档。data 缓冲
   由 rt_list.c 的 list_buf_pool 管理。 */
#define LIST_POOL_MAX 64
static PyAdamasRc* list_pool;
static int list_pool_count;

static PyAdamasRc* list_pool_get(void) {
    PyAdamasRc* h = list_pool;
    if (h) {
        list_pool = h->next;
        list_pool_count--;
    }
    return h;
}

static void list_pool_put(PyAdamasRc* h) {
    if (list_pool_count >= LIST_POOL_MAX) {
        free(h);
        return;
    }
    h->next = list_pool;
    h->prev = NULL;
    list_pool = h;
    list_pool_count++;
}

/* exact-RC 模式下可以完全绕开 GC 的对象：没有任何堆字段（没有出边），
   不可能处在环里。字符串是典型；没有堆字段的类实例（生成代码不给
   dtor/release）同理。它们的生死完全由 RC 决定。 */
static int rc_leaf_untracked(const PyAdamasRc* h) {
    if (!gc_untracked_leaves) return 0;
    if (h->kind == PYADAMAS_RC_STR) return 1;
    // range 只存边界，没有堆字段（出边为零），不可能处在环里。
    if (h->kind == PYADAMAS_RC_RANGE) return 1;
    // 值类型 struct：payload 只有标量字段，同样没有出边。
    if (h->kind == PYADAMAS_RC_STRUCT) return 1;
    if (h->kind == PYADAMAS_RC_INSTANCE && !h->dtor && !h->release) return 1;
    return 0;
}

void panic_c(const char* msg);

/* 存活堆对象索引：unknown 值的静态类型看不见，不能直接 rc_inc（可能
   是整数），先按 payload 查注册表；查到才说明它真是堆对象。索引只用于
   这条保守 retain 路径，开放寻址 + 墓碑。 */
PyAdamasRc** rc_index;
size_t rc_index_cap;
size_t rc_index_fill;  // 非空槽（存活 + 墓碑）
size_t rc_index_live;  // 存活对象数
PyAdamasRc* const RC_TOMB = (PyAdamasRc*)(intptr_t)-1;

size_t rc_index_slot(PyAdamasRc* h) {
    uint64_t x = (uint64_t)(uintptr_t)h >> 4;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    return (size_t)x & (rc_index_cap - 1);
}

void rc_index_rehash(size_t ncap) {
    PyAdamasRc** old = rc_index;
    size_t ocap = rc_index_cap;
    rc_index = (PyAdamasRc**)calloc(ncap, sizeof(PyAdamasRc*));
    if (!rc_index) panic_c("out of memory");
    rc_index_cap = ncap;
    rc_index_fill = 0;
    for (size_t i = 0; i < ocap; ++i) {
        PyAdamasRc* h = old[i];
        if (!h || h == RC_TOMB) continue;
        size_t s = rc_index_slot(h);
        while (rc_index[s]) s = (s + 1) & (ncap - 1);
        rc_index[s] = h;
        rc_index_fill++;
    }
    free(old);
}

void rc_index_put(PyAdamasRc* h) {
    if (rc_index_off || rc_index_readonly) return;
    if (rc_index_cap == 0) {
        rc_index_rehash(64);
    } else if ((rc_index_fill + 1) * 10 >= rc_index_cap * 7) {
        // 存活对象少、墓碑多时原地压缩，否则翻倍。
        size_t ncap = (rc_index_live + 1) * 10 >= rc_index_cap * 4
                          ? rc_index_cap * 2
                          : rc_index_cap;
        rc_index_rehash(ncap);
    }
    size_t s = rc_index_slot(h);
    size_t tomb = (size_t)-1;
    while (rc_index[s]) {
        if (rc_index[s] == RC_TOMB && tomb == (size_t)-1) tomb = s;
        s = (s + 1) & (rc_index_cap - 1);
    }
    if (tomb != (size_t)-1) {
        rc_index[tomb] = h;  // 复用墓碑，不增加 fill
    } else {
        rc_index[s] = h;
        rc_index_fill++;
    }
    rc_index_live++;
}

void rc_index_del(PyAdamasRc* h) {    if (rc_index_off || rc_index_readonly) return;
    if (rc_index_cap == 0) return;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) {
            rc_index[s] = RC_TOMB;
            rc_index_live--;
            return;
        }
        s = (s + 1) & (rc_index_cap - 1);
    }
}

int rc_index_has(PyAdamasRc* h) {
    if (rc_index_off) return 0;
    if (rc_index_cap == 0) return 0;
    // RC_TOMB 是墓碑哨兵（(PyAdamasRc*)-1）。unknown 值 v 减去 64 字节头后
    // 可能正好等于它（v == 63），探测时不能把墓碑当成命中。
    if (h == RC_TOMB) return 0;
    size_t s = rc_index_slot(h);
    while (rc_index[s]) {
        if (rc_index[s] == h) return 1;
        s = (s + 1) & (rc_index_cap - 1);
    }
    return 0;
}

/* 代码生成把静态字符串字面量指针列表放在 .Lstrtab，模块入口在跑用户
   代码前调用这里登记。字面量 rc=INT64_MIN 永生，不进 GC 堆链，但
   unknown 值的运行期类型检查要靠 RC 索引确认指针确实指向 PyAdamasStr。 */
void pa_register_literals(void** objs, int64_t n) {
    for (int64_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)objs[i] - 1;
        if (!rc_index_has(h)) rc_index_put(h);
    }
}

/* 对 unknown 值做保守 retain：不是堆指针就什么也不做。查不到的对象
   包括静态字面量（永生，不需要 inc）和整数。 */
int64_t pa_rc_inc_unknown(int64_t v) {
    if (!v) return 0;
    if (rc_stats_on) stat_rc_inc_unknown++;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (!rc_index_has(h)) return 0;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return 1;
    if (gc_sweeping) {
        if (h->marked) h->rc++;
        return 1;
    }
    if (h->rc <= 0) return 1;
    h->rc++;
    return 1;
}

void pa_rc_dec(void* p);

/* unknown 值的保守释放：先按注册表确认是堆指针，再走正常 dec。
   用于「结果类型静态不可知」的跨 await/gather 值。 */
/* 判断 unknown 值 v 是不是一个「可以安全 dec」的堆对象。
 *
 * 常规路径查 rc_index（精确）。但 exact-RC 模式下索引被关掉
 * （rc_index_off=1，省掉每个堆对象的插入/删除），此时索引恒不命中 ——
 * 混合类型容器（elem_kind=2）里的堆元素就永远 dec 不掉，整批泄漏。
 *
 * 回退判断必须**绝不误判整数**，否则 dec 会按错误的对象头解引用而段错误
 * （试过只用「对象头自洽性」，53_json / 89_bytes_final 直接崩）。
 *
 * 这里用两条硬约束：
 *   1. 对齐：堆指针 16 字节对齐（rc_alloc 走 malloc，对齐是保证的）；
 *   2. 必须**真的在 GC 堆链上** —— 遍历 heap_head 链表确认。
 *
 * 第 2 条是关键：链表上的每个节点都是 rc_link 挂进去的真实对象，
 * 整数不可能出现在链表里。代价是 O(n) 遍历，只在 exact-RC 模式下
 * 且只在 unknown dec 时走，测试规模可以接受。
 *
 * 注意 exact-RC 模式下字符串等叶子**不进链**（rc_leaf_untracked），
 * 所以这条回退对它们无效 —— 那些对象的 dec 由容器自己的 elem_kind=1
 * 路径（pa_rc_dec）负责，不经过这里。 */
static int unknown_is_heap(PyAdamasRc* h) {
    if (rc_index_has(h)) return 1;
    if (!rc_index_off) return 0;
    // **先排除小整数。**
    //
    // 裸 unknown 值可能是任意 int64：下标、长度、循环变量、布尔结果。
    // 减掉 64 字节头之后，这些小整数落在极低地址（几十到几千字节），
    // 对齐检查**挡不住**（336 就是 16 的倍数），直接读 h->marked 会段错误。
    //
    // 堆对象一律来自 malloc，Linux 用户态不可能分配到 64KB 以下；
    // 这个下限足以排除全部小整数，同时远低于任何真实堆地址。
    if ((uintptr_t)h < 65536) return 0;
    if (((uintptr_t)h & 15) != 0) return 0;
    // exact-RC 模式下，字符串 / range / struct 是 untracked leaf：
    // 它们不进 GC 堆链（rc_link 里 rc_leaf_untracked 分支把 marked 设成
    // PYADAMAS_RC_UNTRACKED 且 prev/next 置 NULL）。这类对象的 marked
    // 恰好就是 2，而链上对象的 marked 低位不含 UNTRACKED。
    //
    // 混合容器（elem_kind=2）里出现的堆元素绝大多数就是这种叶子，
    // 先按 marked 精确命中，避免 O(n) 遍历。
    if (h->marked == PYADAMAS_RC_UNTRACKED) return 1;
    // 其余（真正的容器）在链上，遍历确认。
    for (PyAdamasRc* p = heap_head; p; p = p->next)
        if (p == h) return 1;
    return 0;
}

void pa_rc_dec_unknown(int64_t v) {
    if (!v) return;
    if (rc_stats_on) stat_rc_dec_unknown++;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (!unknown_is_heap(h)) return;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    pa_rc_dec((void*)(uintptr_t)v);
}

void pa_gc_collect(void);
void pa_gc_shutdown(void);
void pa_gc_unref_child(void* p);
void pa_gc_add_roots(void** roots, int64_t n_roots);
void dict_gc_release(void* p);

PyAdamasList* pa_list_new(int64_t cap, int64_t elem_kind);
void pa_list_buf_pool_shutdown(void);
void pa_sys_argv_shutdown(void);
void pa_exc_shutdown(void);
void pa_list_append(PyAdamasList* l, int64_t v, int64_t heap);

void panic_c(const char* msg) {
    fprintf(stderr, "pyadamas: runtime error: %s\n", msg);
    exit(1);
}

void rc_link(PyAdamasRc* h) {
    if (rc_leaf_untracked(h)) {
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_UNTRACKED;
        return;
    }
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
    gc_tracked_count++;
    if (h->kind == PYADAMAS_RC_OBJ || h->kind == PYADAMAS_RC_INSTANCE)
        gc_conservative_count++;
    rc_index_put(h);
    young_add(h);
    gc_alloc_black(h);
}

void rc_unlink(PyAdamasRc* h) {
    if (h->marked & PYADAMAS_RC_UNTRACKED) return;
    // 正常 RC 释放时把新生代 O(1) 摘掉；收集期间由收集器统一收尾。
    if (!gc_phase) {
        if (h->marked & PYADAMAS_RC_YOUNG) young_del(h);
        else old_bytes -= h->size + PYADAMAS_RC_HDR;
    }
    if (h->prev) h->prev->next = h->next;
    else heap_head = h->next;
    if (h->next) h->next->prev = h->prev;
    gc_tracked_count--;
    if (h->kind == PYADAMAS_RC_OBJ || h->kind == PYADAMAS_RC_INSTANCE)
        gc_conservative_count--;
    rc_index_del(h);
}

void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind) {
    if (size < 0) panic_c("negative allocation size");
    if (gc_ready && heap_head) {
        // **Go 的 mark assist：让分配方为 GC 付账。**
        //
        // 增量标记进行中时，每次分配推进一小步（有界预算），把一次几毫秒
        // 的停顿摊成几百次几十微秒的小步。这正是 Go 用 gcBgMarkWorker +
        // gcAssistAlloc 做的事，只是我们单线程，靠分配点天然分片。
        // **本地化优化：增量状态用「剩余步数」而非每次判断。**
        //
        // gc_incr_remaining > 0 表示还有待做的增量工作，每分配一次减一，
        // 归零时才真正调 gc_incr_step。这样热路径只有「读全局 + 减 + 判断」
        // 三条指令，没有函数调用。
        //
        // 实测 180_codepoint_perf：不加这层时 GC 开销 82ms（占总 46%），
        // 全是 gc_incr_step 的调用开销（GC 实际工作时间只有 0.007ms）。
        if (gc_incr_remaining > 0) {
            if (--gc_incr_remaining == 0) gc_incr_step();
        } else if (!gc_phase) {
            if (alloc_since_major >= major_threshold) gc_collect(1);
            else if (alloc_since_gc >= young_threshold) gc_collect(0);
        }
    }
    // 短字符串按 8 字节取整，freelist 里的块容量和档位严格对应。
    int64_t alloc_size = size;
    if (kind == PYADAMAS_RC_STR && size > 0 && size <= STR_POOL_MAX_SIZE)
        alloc_size = (size + 7) & ~(int64_t)7;
    PyAdamasRc* h = NULL;
    if (kind == PYADAMAS_RC_STR) h = str_pool_get(alloc_size);
    else if (kind == PYADAMAS_RC_LIST && size == (int64_t)sizeof(PyAdamasList))
        h = list_pool_get();
    if (!h) {
        h = (PyAdamasRc*)malloc(sizeof(PyAdamasRc) + (size_t)alloc_size);
        if (!h) panic_c("out of memory");
        h->size = alloc_size;
    }
    h->rc = 1;
    h->dtor = dtor;
    h->release = release;
    h->kind = kind;
    h->marked = 0;
    rc_link(h);
    // 叶子对象不可能成环，exact-RC 模式下也不进链，不必驱动 GC 频率。
    if (!rc_leaf_untracked(h)) {
        alloc_since_gc += alloc_size + PYADAMAS_RC_HDR;
        alloc_since_major += alloc_size + PYADAMAS_RC_HDR;
    }
    void* p = (char*)h + sizeof(PyAdamasRc);
    memset(p, 0, (size_t)alloc_size);
    // size 用调用方请求的字节数（不含 RC 头），和 ASan 报的口径一致，
    // 方便两边对照。alloc_size 是池化后的容量，会偏大。
    //
    // 诊断钩子放在这里、而不是 pa_rc_inc/dec：那两个函数会被内联到极热
    // 路径，多一次调用就改变寄存器分配（实测把 88_defaultdict 的 1.5 算成
    // 垃圾值）。rc_alloc 本来就要走 malloc/池，多一次调用无影响。
    if (getenv("PYADAMAS_RETDBG") && kind == 0 && size == 32) {
        static int64_t sq;
        fprintf(stderr, "[retdbg] ALLOC#%lld p=%p\n", (long long)++sq, p);
    }
    pa_diag_rc_alloc(p, size, (int32_t)kind);
    return p;
}

// unknown 值的 print：先查 GC 的地址索引判断是不是堆对象，命中就按真实
// 类型分派，否则按整数。索引只比对地址、不解引用，所以任意 int64 都安全。
// 只认 str——int/bool/float 在 unknown 里本来就分不开，保持按整数输出。
void pa_print_any(int64_t v) {
    if (v > 4096) {
        PyAdamasRc* h = (PyAdamasRc*)((uintptr_t)v - (uintptr_t)PYADAMAS_RC_HDR);
        if (rc_index_has(h) && h->kind == PYADAMAS_RC_STR) {
            pa_print_str((PyAdamasStr*)(uintptr_t)v);
            return;
        }
    }
    pa_print_i64(v);
}

/* 生成代码调用的是这两个符号（codegen 直接 emit call pa_rc_inc）。
   头文件里另有同名 static inline 版本供运行时内部使用 —— C 语言里
   static inline 不会产生外部符号，所以这里必须再定义一次。 */
void pa_rc_inc(void* p);
void pa_rc_dec(void* p);

/* 慢路径：只在统计模式或异常计数时进入。
   正常路径由头文件里的 pa_rc_inc 内联完成。 */
void pa_rc_inc_slow(void* p) {
    if (rc_stats_on) stat_rc_inc++;
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (h->rc <= 0) panic_c("refcount increment on a freed object");
    h->rc++;
}

/* 慢路径：计数归零时的真正释放，或收集期间的特殊处理。
   正常路径（rc 从 N 减到 N-1，N-1 > 0）由头文件里的 pa_rc_dec 内联完成。 */
void pa_rc_dec_slow(void* p) {
    if (rc_stats_on) stat_rc_dec++;
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (gc_phase) {
        // 收集期间只调整计数或登记待释放的边，绝不重入释放。
        gc_phase_dec(h, p);
        return;
    }
    if (h->rc <= 0) panic_c("refcount decrement on a freed object");
    if (--h->rc == 0) {
        if (rc_stats_on) stat_rc_free++;
        pa_diag_rc_free(p, (int32_t)h->kind);
        rc_unlink(h);
        if (h->dtor) h->dtor(p);
        if (h->kind == PYADAMAS_RC_STR) str_pool_put(h);
        else if (h->kind == PYADAMAS_RC_LIST && h->size == (int64_t)sizeof(PyAdamasList))
            list_pool_put(h);
        else free(h);
    }
}

/* 表达式临时值栈：每个新堆值在产生时带着 rc=1 入栈，语句/循环/函数
   边界按 mark 统一释放。这样不需要做活跃性分析，也不会漏掉某条路径。
   入栈不增加计数——栈接管的是「产生者」本来就持有的那一个引用。 */
void** temp_stack;
int64_t temp_len;
int64_t temp_cap;

int64_t pa_rc_mark(void) { return temp_len; }


void pa_rc_temp(void* p) {
    if (rc_stats_on) stat_rc_temp++;
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (temp_len == temp_cap) {
        int64_t ncap = temp_cap ? temp_cap * 2 : 64;
        void** ns = (void**)realloc(temp_stack, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        temp_stack = ns;
        temp_cap = ncap;
    }
    temp_stack[temp_len++] = p;
    if (pa_diag_rc_on > 0) pa_diag_temp("push", p, temp_len);
}

void pa_rc_cleanup(int64_t mark) {
    if (rc_stats_on) stat_rc_cleanup++;
    if (mark < 0 || mark > temp_len) panic_c("invalid temporary stack mark");
    while (temp_len > mark) {
        void* q = temp_stack[--temp_len];
        if (pa_diag_rc_on > 0) pa_diag_temp("pop", q, temp_len);
        pa_rc_dec(q);
    }
}

/* 把临时栈上的一个新值取走：所有权转给槽/字段/返回值，cleanup 不再
   释放它。降级器只在「刚产生、未被别名」的值上调用，所以最多命中一次；
   从后往前找，允许中间还夹着别的临时值。 */
void pa_rc_temp_take(void* p) {
    if (rc_stats_on) stat_take_calls++;
    if (!p) return;
    for (int64_t i = temp_len - 1; i >= 0; --i) {
        if (rc_stats_on) stat_take_steps++;
        if (temp_stack[i] == p) {
            temp_stack[i] = temp_stack[--temp_len];
            return;
        }
    }
}

/* unknown 值入临时栈：先按 RC 索引确认是堆对象，再接管调用者的引用。
   unknown 里可能是整数，直接 pa_rc_temp 会按指针读头部。 */
void pa_rc_temp_unknown(int64_t v) {
    if (!v) return;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (!rc_index_has(h)) return;
    pa_rc_temp((void*)(uintptr_t)v);
}

/* 程序退出前由入口函数调用：临时栈此时已经清空，把栈本身的存储还掉。 */
void pa_rc_shutdown(void) {
    // 临时栈里可能还有残留：正常路径上每个 mark/cleanup 都配对，
    // 但进程退出前若栈非空（顶层表达式的临时值、异常展开中途退出等），
    // 直接 free(temp_stack) 会让那些对象的引用计数永远停在 1 ——
    // 它们是 untracked leaf（exact-RC 模式下不在 GC 堆链上），只能靠
    // RC 释放，于是永久泄漏。
    //
    // ASan 实测 53_json 泄漏 96 字节（inline 短串，栈指向 pa_json_dumps）。
    while (temp_len > 0) pa_rc_dec(temp_stack[--temp_len]);
    free(temp_stack);
    temp_stack = NULL;
    temp_len = 0;
    temp_cap = 0;
}

/* ── 闭包 ─────────────────────────────────────────────────────── */

/* 闭包负载 = [code, n_caps, cell0, cell1…]。
   code 是静态代码地址（不是堆指针）；cell 是单元素 list，闭包持有引用。 */
void closure_destroy(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) pa_rc_dec((void*)c[2 + i]);
}

void closure_gc_release(void* p) {
    int64_t* c = (int64_t*)p;
    for (int64_t i = 0; i < c[1]; ++i) pa_gc_unref_child((void*)c[2 + i]);
}

void* pa_closure_new(int64_t code, int64_t n_caps) {
    if (n_caps < 0) panic_c("negative closure capture count");
    void* p = rc_alloc(16 + 8 * n_caps, closure_destroy, closure_gc_release,
                       PYADAMAS_RC_CLOSURE);
    ((int64_t*)p)[0] = code;
    ((int64_t*)p)[1] = n_caps;
    return p;
}

/* 短串 SSO：<=7 字节直接内联在 PyAdamasStr::hash 的 8 字节里，
   data 指向这段内联区；长串仍用结构体后面的独立字符区。
   值仍然是 8 字节指针（不是 tagged value），所以对象本身仍走 RC/GC。 */
#define STR_INLINE_MAX 7

/* ── UTF-8 工具 ────────────────────────────────────────────────
   码点数 = 非续字节（(b & 0xC0) != 0x80）的个数。对合法 UTF-8 精确，
   对非法字节也安全（「每个非续字节算 1 码点」的宽松定义），永远不会
   越界或返回负值——二进制文件读进来的字节直通串靠这条兜住。 */
int64_t str_count_cp(const char* p, int64_t n) {
    int64_t k = 0;
    for (int64_t i = 0; i < n; ++i)
        if (((unsigned char)p[i] & 0xC0) != 0x80) ++k;
    return k;
}

/* 首字节 b 领起的 UTF-8 序列长度；非法首字节按 1 处理（宽松兜底）。 */
int str_seq_len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 码点下标 cp -> 字节偏移（从头扫；cp == pub 时返回 len）。 */
int64_t str_cp_to_byte(const PyAdamasStr* s, int64_t cp) {
    if (s->pub == s->len) return cp;  /* 全 ASCII：码点下标就是字节偏移 */
    int64_t off = 0;
    while (cp > 0 && off < s->len) {
        off += str_seq_len((unsigned char)s->data[off]);
        --cp;
    }
    return off;
}

/* 从字节偏移 off 起取一个码点（返回码点值，*plen 填字节数）。 */
uint32_t str_cp_at(const PyAdamasStr* s, int64_t off, int* plen) {
    const unsigned char* p = (const unsigned char*)s->data + off;
    unsigned char b = p[0];
    int n = str_seq_len(b);
    int64_t remain = s->len - off;
    if (n > remain) n = (int)(remain > 0 ? remain : 1);
    uint32_t cp;
    switch (n) {
        case 2: cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F); break;
        case 3:
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                 (uint32_t)(p[2] & 0x3F);
            break;
        case 4:
            cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            break;
        default: cp = b; n = 1; break;
    }
    if (plen) *plen = n;
    return cp;
}

/* 从字节偏移 off 回退到「前一个码点」的首字节（负步长切片用）。
   off == 0 时返回 0（调用方保证不会用到）。 */
int64_t str_prev_cp(const PyAdamasStr* s, int64_t off) {
    if (off <= 0) return 0;
    int64_t b = off - 1;
    while (b > 0 && ((unsigned char)s->data[b] & 0xC0) == 0x80) --b;
    return b;
}

/* 码点 -> UTF-8 字节数。 */
int str_cp_bytes(uint32_t cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

/* 码点 -> UTF-8 字节串（写入 out，返回字节数）。 */
int str_cp_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ── 构造 API ──────────────────────────────────────────────────
   契约：str_alloc_raw 只分配并把 pub 置 -1（哨兵），调用方填完字符区
   必须调 str_seal（扫描算码点数）或 str_seal_n（已知码点数，O(1)）。
   漏了 seal 的对象 pub 会停在 -1，len() 立刻能看出来；PYADAMAS_STR_DEBUG=1
   时消费者会 panic 而不是静默算错。 */
PyAdamasStr* str_alloc_raw(int64_t len) {
    if (len < 0) panic_c("negative string length");
    if (len <= STR_INLINE_MAX) {
        PyAdamasStr* s =
            (PyAdamasStr*)rc_alloc((int64_t)sizeof(PyAdamasStr), NULL, NULL, PYADAMAS_RC_STR);
        s->len = len;
        s->pub = -1;
        s->data = (const char*)&s->hash;
        ((char*)s->data)[len] = '\0';
        return s;
    }
    PyAdamasStr* s =
        (PyAdamasStr*)rc_alloc((int64_t)sizeof(PyAdamasStr) + len + 1, NULL, NULL, PYADAMAS_RC_STR);
    s->len = len;
    s->pub = -1;
    s->hash = 0;
    char* p = (char*)(s + 1);
    p[len] = '\0';
    s->data = p;
    return s;
}

/* 扫描字符区算码点数写进 pub。 */
PyAdamasStr* str_seal(PyAdamasStr* s) {
    s->pub = str_count_cp(s->data, s->len);
    return s;
}

/* 已知码点数直接写（O(1)）；debug 下校验范围。 */
PyAdamasStr* str_seal_n(PyAdamasStr* s, int64_t pub) {
    if (pub < 0 || pub > s->len) panic_c("string seal: bad codepoint count");
    s->pub = pub;
    return s;
}

/* 默认 str 语义：复制后扫一遍算码点数（最安全，调用点无需知道内容）。 */
PyAdamasStr* str_new(const char* p, int64_t len) {
    PyAdamasStr* s = str_alloc_raw(len);
    memcpy((char*)s->data, p, (size_t)len);
    return str_seal(s);
}

/* bytes 语义：pub == len == 字节数，O(1)。 */
PyAdamasStr* str_new_bytes(const char* p, int64_t len) {
    PyAdamasStr* s = str_alloc_raw(len);
    memcpy((char*)s->data, p, (size_t)len);
    s->pub = len;
    return s;
}

/* 已知全 ASCII：pub == len，O(1)。 */
PyAdamasStr* str_new_ascii(const char* p, int64_t len) {
    PyAdamasStr* s = str_alloc_raw(len);
    memcpy((char*)s->data, p, (size_t)len);
    s->pub = len;
    return s;
}

PyAdamasStr* pa_str_from_cstr(const char* p) {
    if (!p) p = "";
    return str_new(p, (int64_t)strlen(p));
}

PyAdamasStr* pa_str_from_cstr_opt(const char* p) {
    if (!p) return NULL;
    return str_new(p, (int64_t)strlen(p));
}

/* ── 静态驻留字符串 ─────────────────────────────────────────────
   字符串迭代/下标每取一个字符都会造一个单字节串；bool/None 转字符串
   也在 print/repr 里反复出现。字符串不可变，这些结果可以做成永生对象：
   256 个单字节串 + "True"/"False"/"None"，首次用到时初始化并登记进
   RC 索引（unknown 值的运行期类型检查要靠它）。 */
typedef struct {
    PyAdamasRc h;
    PyAdamasStr s;
    char c[8];
} PyAdamasStaticStr;

static PyAdamasStaticStr char_tab[256];
static PyAdamasStaticStr kw_true_s, kw_false_s, kw_none_s;
static int static_strs_ready;

static void static_str_init(PyAdamasStaticStr* e, const char* p, int64_t n, int64_t pub) {
    e->h.next = NULL;
    e->h.prev = NULL;
    e->h.rc = PYADAMAS_RC_IMMORTAL;
    e->h.dtor = NULL;
    e->h.release = NULL;
    e->h.size = (int64_t)sizeof(PyAdamasStr);
    e->h.kind = PYADAMAS_RC_STR;
    e->h.marked = PYADAMAS_RC_UNTRACKED;
    e->s.len = n;
    e->s.pub = pub;
    e->s.hash = 0;
    memcpy(e->c, p, (size_t)n);
    e->c[n] = '\0';
    e->s.data = e->c;
    if (!rc_index_off) rc_index_put(&e->h);
}

static void static_strs_init(void) {
    /* 0x00-0x7F 是合法单码点 UTF-8；0x80-0xFF 的单字节序列不是合法 UTF-8，
       不能当 str 暴露（bytes 不需要它们：bytes 下标是 int，bytes 切片走字节
       实现）。这些槽仍然初始化成 pub=1 的对象，只为让 pa_char_str 的越界
       调用有一个确定的对象可返回，代码里不再从 str 路径取到它们。 */
    for (int i = 0; i < 256; ++i) {
        char ch = (char)i;
        static_str_init(&char_tab[i], &ch, 1, i < 0x80 ? 1 : 0);
    }
    static_str_init(&kw_true_s, "True", 4, 4);
    static_str_init(&kw_false_s, "False", 5, 5);
    static_str_init(&kw_none_s, "None", 4, 4);
    static_strs_ready = 1;
}

static void static_strs_ensure(void) {
    if (!static_strs_ready) static_strs_init();
}

/* 单码点串：ASCII 走 256 项驻留表（零分配），其余码点现编 1-4 字节。
   0x80-0xFF 不再是合法的单字节 str，ASCII 之外的码点必须走 pa_cp_str。 */
PyAdamasStr* pa_char_str(int64_t c) {
    static_strs_ensure();
    return &char_tab[(int)(c & 0xff)].s;
}

PyAdamasStr* pa_cp_str(uint32_t cp) {
    if (cp < 0x80) return pa_char_str((int64_t)cp);
    char buf[4];
    int n = str_cp_encode(cp, buf);
    PyAdamasStr* s = str_alloc_raw(n);
    memcpy((char*)s->data, buf, (size_t)n);
    s->pub = 1;
    return s;
}

PyAdamasStr* pa_str_true(void) {
    static_strs_ensure();
    return &kw_true_s.s;
}

PyAdamasStr* pa_str_false(void) {
    static_strs_ensure();
    return &kw_false_s.s;
}

PyAdamasStr* pa_str_none(void) {
    static_strs_ensure();
    return &kw_none_s.s;
}

/* ── 分代循环收集器 ─────────────────────────────────────────────
   对象按「是否在当前收集集合」分两代：上次收集后分配、还没晋升的是
   新生代（young_buf），其余存活对象是老生代。minor 只对新生代做
   trial-deletion；老对象的引用按真实 rc 计入外部引用，天然是根，
   所以不需要写屏障。老生代增长到阈值时做 major（集合 = 整堆）。

   一个回合（stop-the-world）分四步：
   1. 试探减计数：对集合内每个对象执行 release 回调，把每条出边从
      子对象的 rc 减掉。减完 rc > 0 的对象被集合外引用，是根。
   2. 标记：从根出发沿出边走一遍（gc_scan_object），未标记的集合成员
      就是环垃圾。
   3. 恢复：标记对象再走一遍 release，把出边计数加回去并晋升。
   4. 清扫：垃圾对象先走 release（指向集合外的边攒进 pending），再
      统一释放；回合结束后 pending 按正常 RC 释放。

   marked 低 8 位是标志（1 本轮可达 / 2 叶子不在链上 / 4 在集合里），
   高位是 young_buf 下标，正常 RC 释放时 O(1) 摘除。
   ─────────────────────────────────────────────────────────────── */

void** gc_globals;
int64_t gc_n_globals;
int64_t gc_globals_cap;
uintptr_t gc_stack_top;
PyAdamasRc** gc_work;
size_t gc_work_len;
/* 工作栈、本轮集合、集合地址表、清扫后待释放的老对象，全部跨回合复用。 */
static PyAdamasRc** gc_work_buf;
static size_t gc_work_buf_cap;
static PyAdamasRc** gc_set;
static size_t gc_set_len;
static PyAdamasRc** gc_set_buf;
static size_t gc_set_buf_cap;
static PyAdamasRc** gc_tab;
static size_t gc_tab_cap;
static PyAdamasRc** gc_tab_buf;
static size_t gc_tab_buf_cap;
static void** gc_pend;
static size_t gc_pend_len;
static size_t gc_pend_cap;

#if defined(__SANITIZE_ADDRESS__)
#define PYADAMAS_NO_ASAN __attribute__((no_sanitize_address))
#else
#define PYADAMAS_NO_ASAN
#endif

size_t gc_hash(uintptr_t p) {
    return (size_t)((p >> 4) * 11400714819323198485ull);
}

static void gc_reserve_set(size_t need) {
    if (need <= gc_set_buf_cap) return;
    size_t ncap = gc_set_buf_cap ? gc_set_buf_cap : 1024;
    while (ncap < need) ncap *= 2;
    PyAdamasRc** ns = (PyAdamasRc**)realloc(gc_set_buf, sizeof(PyAdamasRc*) * ncap);
    if (!ns) panic_c("out of memory");
    gc_set_buf = ns;
    gc_set_buf_cap = ncap;
}

static void gc_pend_add(int64_t v) {
    if (gc_pend_len == gc_pend_cap) {
        size_t ncap = gc_pend_cap ? gc_pend_cap * 2 : 256;
        void** ns = (void**)realloc(gc_pend, sizeof(void*) * ncap);
        if (!ns) panic_c("out of memory");
        gc_pend = ns;
        gc_pend_cap = ncap;
    }
    gc_pend[gc_pend_len++] = (void*)(uintptr_t)v;
}

/* 集合地址表：键是负载地址，值是对应头部。minor 时集合通常远小于整堆，
   比旧的全堆地址表便宜；major 时和旧表的规模一致。 */
static void gc_tab_build(void) {
    size_t cap = 64;
    while (cap < gc_set_len * 2) cap <<= 1;
    if (cap > gc_tab_buf_cap) {
        free(gc_tab_buf);
        gc_tab_buf = (PyAdamasRc**)calloc(cap, sizeof(PyAdamasRc*));
        if (!gc_tab_buf) panic_c("out of memory");
        gc_tab_buf_cap = cap;
    } else {
        memset(gc_tab_buf, 0, cap * sizeof(PyAdamasRc*));
    }
    gc_tab = gc_tab_buf;
    gc_tab_cap = cap;
    size_t mask = cap - 1;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        size_t s = gc_hash((uintptr_t)h + (uintptr_t)PYADAMAS_RC_HDR) & mask;
        while (gc_tab[s]) s = (s + 1) & mask;
        gc_tab[s] = h;
    }
}

static PyAdamasRc* gc_tab_find(uintptr_t v) {
    if (!gc_tab_cap) return NULL;
    size_t mask = gc_tab_cap - 1;
    size_t s = gc_hash(v) & mask;
    while (gc_tab[s]) {
        if ((uintptr_t)gc_tab[s] + (uintptr_t)PYADAMAS_RC_HDR == v) return gc_tab[s];
        s = (s + 1) & mask;
    }
    return NULL;
}

void gc_mark(uintptr_t v) {
    if (!v) return;
    PyAdamasRc* h = gc_tab_find(v);
    if (!h || (h->marked & PYADAMAS_RC_MARK)) return;
    h->marked |= PYADAMAS_RC_MARK;
    if (gc_cons_phase) gc_cons_note(h);
    gc_work[gc_work_len++] = h;
}

/* 静态类型已知的堆指针：直接读头部，不查表。字面量（永生）和不在本轮
   集合里的老对象都不用标记。 */
static void gc_mark_direct(uintptr_t v) {
    if (!v) return;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)(v - (uintptr_t)PYADAMAS_RC_HDR);
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (!(h->marked & PYADAMAS_RC_YOUNG)) return;
    if (h->marked & PYADAMAS_RC_MARK) return;
    h->marked |= PYADAMAS_RC_MARK;
    if (gc_cons_phase) gc_cons_note(h);
    gc_work[gc_work_len++] = h;
}

PYADAMAS_NO_ASAN static void gc_scan_range(const void* begin, const void* end) {
    uintptr_t a = (uintptr_t)begin;
    uintptr_t b = (uintptr_t)end;
    if (a >= b) return;
    for (; a + sizeof(uintptr_t) <= b; a += sizeof(uintptr_t)) {
        uintptr_t v;
        memcpy(&v, (const void*)a, sizeof v);
        gc_mark(v);
    }
}

/* 位图扫描：只读置位的字。位图由 codegen 按静态类型算出，是超集
   （宁多勿少）——少扫一个真正含指针的字就会提前释放，所以只有确实
   「证明不可能装指针」的字才允许不置位。 */
PYADAMAS_NO_ASAN static void gc_scan_masked(const void* base, const uint64_t* mask,
                                            int64_t words) {
    for (int64_t w = 0; w < words; ++w) {
        if (!(mask[w >> 6] & ((uint64_t)1 << (w & 63)))) continue;
        uintptr_t v;
        memcpy(&v, (const char*)base + (size_t)w * sizeof(uintptr_t), sizeof v);
        gc_mark(v);
    }
}

// setjmp 会把 callee-saved 寄存器存进 jmp_buf；生成代码里常驻寄存器的
// 堆指针（槽位提升、全局 vreg）就靠这一段兜住。
PYADAMAS_NO_ASAN static void gc_scan_registers(void) {
    jmp_buf regs;
    if (_setjmp(regs) == 0) gc_scan_range(&regs, (char*)&regs + sizeof regs);
}

/* 帧记录扫描的拆分方式：正常路径一次扫完（GC_FRAMES_ALL）；诊断模式
   把「已知堆槽」「保守槽」「临时区」拆成三遍，分别统计是哪种根在兜底。 */
#define GC_FRAMES_ALL 0
#define GC_FRAMES_TEMP 1
#define GC_FRAMES_SLOTS_EXACT 2
#define GC_FRAMES_SLOTS_CONS 3

/* 保守根只扫帧记录登记过的地址：
   - 借来的 unknown 槽（bit1，只扫描不释放）；
   - 带 unknown 标志的拥有槽（bit0）；
   - 可能装指针的帧的临时区（scan_base/scan_words，按 scan_mask 位图
     收窄；位图为 NULL 或 PYADAMAS_GC_CONSERVATIVE=1 时整段扫描）。
   纯类型化帧和栈上的其它内容（返回地址、保存的寄存器、填充）一律
   不扫。 */
static void gc_scan_frames(int mode) {
    for (PyAdamasFrame* f = pa_frame_top; f; f = f->prev) {
        if (mode == GC_FRAMES_ALL || mode == GC_FRAMES_TEMP) {
            if (f->scan_words > 0 && f->scan_base) {
                if (f->scan_mask && !gc_conservative)
                    gc_scan_masked(f->scan_base, f->scan_mask, f->scan_words);
                else
                    gc_scan_range(f->scan_base,
                                  (char*)f->scan_base +
                                      sizeof(uintptr_t) * (size_t)f->scan_words);
            }
        }
        if (mode == GC_FRAMES_TEMP) continue;
        for (int64_t i = 0; i < f->n; ++i) {
            uintptr_t addr = (uintptr_t)f->slots[i];
            uintptr_t low = addr & 3;
            if (mode == GC_FRAMES_SLOTS_EXACT && low) continue;
            if (mode == GC_FRAMES_SLOTS_CONS && !low) continue;
            int64_t v = *(int64_t*)(addr & ~(uintptr_t)3);
            if (!v) continue;
            if (low) gc_mark((uintptr_t)v);        // unknown / 只扫描
            else gc_mark_direct((uintptr_t)v);     // 已知堆槽
        }
    }
}

void gc_scan_object(PyAdamasRc* h) {
    void* p = (char*)h + PYADAMAS_RC_HDR;
    switch (h->kind) {
        case PYADAMAS_RC_STR:
            break;
        case PYADAMAS_RC_LIST: {
            PyAdamasList* l = (PyAdamasList*)p;
            if (!l->data) break;
            // exact-RC 模式：0 是纯数值元素；1 是静态类型已知的堆指针；
            // 2/3 是推不出来的元素，保守查集合表标记。
            if (gc_untracked_leaves) {
                if (l->elem_kind == 1)
                    for (int64_t i = 0; i < l->len; ++i)
                        gc_mark_direct((uintptr_t)l->data[i]);
                else if (l->elem_kind >= 2)
                    for (int64_t i = 0; i < l->len; ++i)
                        gc_mark((uintptr_t)l->data[i]);
            } else {
                for (int64_t i = 0; i < l->len; ++i) gc_mark((uintptr_t)l->data[i]);
            }
            break;
        }
        case PYADAMAS_RC_DICT: {
            PyAdamasDict* d = (PyAdamasDict*)p;
            int precise = gc_untracked_leaves;
            for (int64_t i = 0; i < d->len; ++i) {
                int64_t k = pa_dict_key_at(d, i);
                int64_t v = pa_dict_val_at(d, i);
                if (precise && d->key_own == 1)
                    gc_mark_direct((uintptr_t)k);
                else if (!precise || d->key_own == 2)
                    gc_mark((uintptr_t)k);
                if (precise && d->val_own == 1)
                    gc_mark_direct((uintptr_t)v);
                else if (!precise || d->val_own == 2)
                    gc_mark((uintptr_t)v);
            }
            break;
        }
        case PYADAMAS_RC_RANGE:
            // range 的负载是四个整数，没有子对象要标记。
            break;
        case PYADAMAS_RC_CLOSURE:
        case PYADAMAS_RC_OBJ:
        case PYADAMAS_RC_INSTANCE: {
            // 闭包负载 = [code, n_caps, cell…]，cell 都是堆上的 list；
            // exact-RC 模式下可以精确标记。OBJ / INSTANCE 形态不固定
            // （异常、文件、类实例），仍按保守扫描。
            if (h->kind == PYADAMAS_RC_CLOSURE && gc_untracked_leaves) {
                int64_t* c = (int64_t*)p;
                for (int64_t i = 0; i < c[1]; ++i) gc_mark_direct((uintptr_t)c[2 + i]);
                break;
            }
            for (int64_t off = 0; off + (int64_t)sizeof(uintptr_t) <= h->size;
                 off += (int64_t)sizeof(uintptr_t)) {
                uintptr_t v;
                memcpy(&v, (const char*)p + off, sizeof v);
                gc_mark(v);
            }
            break;
        }
        default:
            break;
    }
}

void pa_gc_add_roots(void** roots, int64_t n_roots) {
    if (!roots || n_roots <= 0) return;
    if (gc_n_globals + n_roots > gc_globals_cap) {
        int64_t ncap = gc_globals_cap ? gc_globals_cap : 64;
        while (ncap < gc_n_globals + n_roots) ncap *= 2;
        void** ns = (void**)realloc(gc_globals, sizeof(void*) * (size_t)ncap);
        if (!ns) panic_c("out of memory");
        gc_globals = ns;
        gc_globals_cap = ncap;
    }
    // 根表本身是静态存储，这里只复制槽地址。
    for (int64_t i = 0; i < n_roots; ++i) gc_globals[gc_n_globals++] = roots[i];
}

void pa_gc_init(void** roots, int64_t n_roots, void* stack_top) {
    /* 诊断 / 收窄回退开关：整个进程只在这里各读一次环境变量，之后全都
       走缓存全局，分配热路径不碰。 */
    if (getenv("PYADAMAS_GC_STATS")) {
        gc_stats_on = 1;
        atexit(gc_stats_dump);
    }
    if (getenv("PYADAMAS_GC_CONSERVATIVE")) gc_conservative = 1;
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    pa_gc_add_roots(roots, n_roots);
    gc_stack_top = (uintptr_t)stack_top;
    // 嵌入/基准场景可以关掉循环收集，只靠引用计数（会漏检环）。
    if (getenv("PYADAMAS_GC_DISABLE")) return;
    gc_ready = 1;
}

/* 入口调用：按环境变量打开 RC/GC 统计。统计关闭时热路径只有一个
   可预测的全局分支。 */
void pa_rc_stats_init(void) {
    if (!getenv("PYADAMAS_RC_STATS")) return;
    rc_stats_on = 1;
    atexit(rc_stats_dump);
}

/* 整程序无 unknown 值、不涉及 asyncio/生成器时，入口调用这里关掉
   存活堆索引；此后 rc_index_put/del/has 都是空操作。RC 变精确后，
   字符串这类叶子对象也移出 GC 堆链（不可能成环，RC 归零即释放）。 */
/* 编译器证明本模块不需要「反查这个字是不是堆对象」时调用。
 *
 * 与 pa_disable_unknown_index 的区别：
 *   - disable：索引彻底不建，所有查询失败（exact-RC，要求代码里没有任何
 *     unknown 值参与 dec —— 由编译器静态证明）；
 *   - readonly：索引不维护，查询也返回失败，但**允许**代码里存在 unknown
 *     值，只是那些 unknown 必须走容器位图 / 帧记录等不依赖索引的路径。
 *
 * 实测收益：bench_rc_gen 这类「大量短命容器 + 大存活堆」的负载，索引的
 * 插入删除占 22ms（102ms → 80ms），而它一个查询都没用到。 */
void pa_set_index_readonly(void) {
    rc_index_readonly = 1;
}

void pa_disable_unknown_index(void) {
    rc_index_off = 1;
    gc_untracked_leaves = 1;
}

void pa_gc_unref_child(void* p) {
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (gc_phase) {
        gc_phase_dec(h, p);
        return;
    }
    // release 回调只在收集期间调用；这里兜底按正常引用释放。
    pa_rc_dec(p);
}

/* 生成器帧的堆槽释放（GC release 与异常逃出共用）：清扫阶段顺带清零，
   之后重复调用是空操作；trial/restore 阶段只调整计数、不动槽。 */
typedef struct PyAdamasExcSave PyAdamasExcSave;
void pa_exc_free_save_chain(PyAdamasExcSave* s);

void pa_gen_slot_release(int64_t* slot, int64_t own) {
    int64_t v = *slot;
    if (!v) return;
    if (gc_phase == GC_PHASE_SWEEP) *slot = 0;
    // own: 1 = 堆、2 = 可能非堆（注册表）、3 = 异常对象、4 = save 链。
    //
    // 3/4 是生成器 except 块里挂起时 exc_leave 写进帧槽的非 RC 对象：
    // 3 是 PyAdamasException*（裸 malloc），4 是 PyAdamasExcSave* 链。
    // 必须走各自的释放函数，不能 pa_rc_dec（那会按 RC 头读到 malloc 块前面）。
    if (own == 3) {
        exc_free((PyAdamasException*)(uintptr_t)v);
        *slot = 0;
        return;
    }
    if (own == 4) {
        pa_exc_free_save_chain((PyAdamasExcSave*)(uintptr_t)v);
        *slot = 0;
        return;
    }
    if (own == 2) pa_rc_dec_unknown(v);
    else pa_rc_dec((void*)(uintptr_t)v);
}

void gc_drain(void) {
    while (gc_work_len) gc_scan_object(gc_work[--gc_work_len]);
}

/* 增量收集收尾：晋升存活对象、释放清扫期攒下的引用、复位状态。 */
static void gc_incr_finish(void) {
    // **重算 old_bytes，不要在旧值上累加。**
    //
    // gc_set 里的对象是本轮**全部**存活对象（major 时就是整堆），它们的
    // 字节数必须**覆盖**而不是叠加到 old_bytes 上。原来写成 `old_bytes +=`
    // 会让每轮翻倍，major_threshold = old_bytes * 2 随之暴涨……但更糟的是
    // 分片清扫路径下 gc_set 被反复遍历，old_bytes 累加到失真，阈值反而
    // 变得比实际堆小，触发 28 万次 GC（实测 gc_runs=283676）。
    int64_t live_bytes = 0;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (!h) continue;
        h->marked = 0;
        live_bytes += h->size + PYADAMAS_RC_HDR;
    }
    old_bytes = live_bytes;
    // **先把 gc_phase 复位，再处理 gc_pend。**
    //
    // 顺序反了会死循环：pa_rc_dec 看到 gc_phase != 0 就把对象重新登记进
    // gc_pend（gc_phase_dec 的 INCR_MARK 分支），而这里正在遍历 gc_pend ——
    // 对象永远释放不掉。实测 174_gc_construct_roots 漏 21KB 就是这个原因。
    gc_phase = GC_PHASE_NONE;
    gc_mark_sweep = 0;
    gc_incr_state = 0;
    gc_incr_remaining = 0;
    size_t n_set = gc_set_len;
    gc_set = NULL;
    gc_set_len = 0;
    gc_tab = NULL;
    gc_tab_cap = 0;
    gc_work = NULL;
    gc_work_len = 0;
    gc_sweeping = 0;
    // 现在 gc_phase 是 0，pa_rc_dec 会走正常释放路径。
    // 注意 pa_rc_dec 可能级联释放更多对象（改 gc_pend 吗？不会 —— 此时
    // gc_phase 已为 0，走的是直接 free 路径）。
    int64_t pend_n = gc_pend_len;
    gc_pend_len = 0;
    for (int64_t i = 0; i < pend_n; ++i) pa_rc_dec(gc_pend[i]);
    stat_gc_live = old_bytes;
    alloc_since_gc = 0;
    // 按 Go pacer 公式重算下一轮 minor 阈值（同 gc_collect 里的逻辑）。
    {
        int64_t live = old_bytes;
        int64_t roots = (int64_t)temp_len * (int64_t)sizeof(void*);
        int64_t target = live + (live + roots);
        if (target < (int64_t)PYADAMAS_GC_THRESHOLD)
            target = (int64_t)PYADAMAS_GC_THRESHOLD;
        young_threshold = target;
    }
    if (gc_incr_major) {
        alloc_since_major = 0;
        int64_t mt = old_bytes * 2;
        if (mt < 4 * (int64_t)PYADAMAS_GC_THRESHOLD)
            mt = 4 * (int64_t)PYADAMAS_GC_THRESHOLD;
        major_threshold = mt;
    }
    (void)n_set;
}

/* 推进增量标记/清扫。**有界步数**，由分配点驱动（Go 的 mark assist）。 */
/* ── 写屏障（照抄 Go 的 Dijkstra 插入屏障）────────────────────
 *
 * Go 的写屏障（runtime/mwbbuf.go）：标记期间任何指针写入，都把**新值**
 * 记进每 P 的缓冲区；缓冲区满了就 flush，把里面指向白色对象的指针标记
 * 成灰色并放进标记队列。这样「标记开始后才建立的可达关系」不会漏标。
 *
 * 我们的情况：增量标记期间对象图会变（13_gc 里 a.next = b 就是例子）。
 * 没有屏障的话：a 在标记前已被扫描（标记成黑色），之后 a.next = b 写入，
 * b 永远扫不到 → 被误判为垃圾回收掉 → 段错误。
 *
 * 本地化：不做每 P 缓冲区（我们单线程、没有 P），直接「写入即标灰」。
 * 代价是每次写入多一次判断 + 可能的入队，换来正确性。 */
/* Go 的 allocate-black：标记期间新分配的对象直接标黑。
 *
 * 理由（Go 的设计文档同样如此）：新分配的对象一定可达（刚被创建，
 * 调用方还持有引用），所以它不可能在本轮被回收；而且它可能引用更老的
 * 对象 —— 但那些老对象要么已经被扫描过、要么会在后续队列里被扫到。
 *
 * 不标黑会怎样：新对象在 gc_set 里没有位置（gc_set 是标记开始时拍的
 * 快照），清扫阶段扫不到它 —— 它会被当成垃圾 free 掉，而调用方还拿着
 * 指针。这正是 13_gc 段错误的原因。
 *
 * 注意：标黑只对**本轮集合内**的对象有意义，但新对象不在集合里，所以
 * 这里只是把它从「可能被扫到」变成「明确跳过」—— 实际实现是：让
 * gc_free_body 不碰它，靠 rc 正常释放。 */
/* Go 的 allocate-black：标记期间新分配的对象**直接视为存活**。
 *
 * 关键点：新对象不在本轮 gc_set 快照里，所以清扫阶段扫不到它 ——
 * 它天然不会被 GC 回收。**不能**给它打 MARK 位：MARK 位的语义是
 * 「本轮集合内、已标记存活」，而新对象根本不在集合里，打上去会让
 * 后续逻辑（gc_incr_finish 的晋升循环、增量清扫）把它当成集合成员
 * 处理，反而漏掉它 —— 实测 174_gc_construct_roots 漏 21KB。
 *
 * 所以这里是**空操作**，只作为语义占位：新对象靠 RC 正常释放，
 * 不参与本轮 GC。真正需要保证的是「新对象引用的老对象不被误回收」，
 * 那由写屏障负责（见 gc_write_barrier）。 */
static void gc_alloc_black(PyAdamasRc* h) {
    (void)h;
}

static void gc_write_barrier(int64_t v) {
    // 只在标记阶段生效；清扫/空闲阶段写入不需要屏障。
    if (gc_incr_state != 1) return;
    if (!v) return;
    // 直接标记新值：标灰并入队，后续 gc_incr_step 会扫描它。
    gc_mark((uintptr_t)v);
}

void pa_gc_write_barrier(int64_t v) { gc_write_barrier(v); }

static void gc_incr_step(void) {
    if (!gc_incr_state) return;
    // 本轮做完后重新装填，直到整个清扫完成。
    gc_incr_remaining = GC_INCR_STRIDE;
    int budget = GC_MARK_BUDGET;
    if (gc_incr_state == 1) {
        while (gc_work_len && budget-- > 0) gc_scan_object(gc_work[--gc_work_len]);
        if (gc_work_len == 0) {
            gc_incr_state = 2;
            gc_phase = GC_PHASE_SWEEP;
            gc_sweeping = 1;
            gc_incr_sweep_pos = 0;
        }
        return;
    }
    budget = GC_SWEEP_BUDGET;
    while (gc_incr_sweep_pos < gc_set_len && budget-- > 0) {
        size_t pos = gc_incr_sweep_pos++;
        PyAdamasRc* h = gc_set[pos];
        if (!h) continue;
        if (!(h->marked & PYADAMAS_RC_MARK)) {
            if (rc_stats_on) stat_gc_freed++;
            gc_free_body(h);
            gc_set[pos] = NULL;
        }
    }
    if (gc_incr_sweep_pos >= gc_set_len) gc_incr_finish();
}

/* 诊断模式的根扫描顺序：先精确根（已知堆槽），再保守根（临时区、
   unknown 槽、寄存器），每一段都排空工作栈。根集合最终仍是并集，
   与一次扫完等价；区别只是能统计「第二、三段新标记的对象」——它们
   就是没有精确根能到、只被保守根维持的那批（误报保留的直接度量）。 */
static void gc_scan_roots_split(void) {
    gc_scan_frames(GC_FRAMES_SLOTS_EXACT);
    gc_drain();
    gc_cons_phase = 1;
    gc_cons_src = 0;
    gc_scan_frames(GC_FRAMES_TEMP);
    gc_drain();
    gc_cons_src = 1;
    gc_scan_frames(GC_FRAMES_SLOTS_CONS);
    gc_drain();
    gc_cons_src = 2;
    gc_scan_registers();
    gc_drain();
    gc_cons_phase = 0;
}

static double gc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void gc_free_body(PyAdamasRc* h) {
    rc_unlink(h);
    if (h->kind == PYADAMAS_RC_OBJ)
        free_attached_exception((char*)h + PYADAMAS_RC_HDR, h->size);
    if (h->kind == PYADAMAS_RC_LIST) {
        PyAdamasList* l = (PyAdamasList*)((char*)h + PYADAMAS_RC_HDR);
        free(l->data);
        // 逐元素堆位图也是堆内存，必须一起还掉（GC 清扫路径不走 list_destroy）。
        free(l->heap_bits);
        l->heap_bits = NULL;
    } else if (h->kind == PYADAMAS_RC_DICT) {
        PyAdamasDict* d = (PyAdamasDict*)((char*)h + PYADAMAS_RC_HDR);
        free(d->entries);
        free(d->order);
        free(d->index);
    }
    free(h);
}

/* ── 增量标记（照抄 Go 的并发标记结构，本地化为 RC+GC 混合）─────
 *
 * Go 的做法（src/runtime/mgcmark.go 的 gcDrain）：
 *   1. 标记工作放在全局队列（gcWork）+ 每 P 本地队列；
 *   2. 后台 worker goroutine 不断从队列取对象、扫描、把子对象入队；
 *   3. 写屏障保证「标记期间新写入的指针」也被记录，不会漏标；
 *   4. 分配速率太快时，分配方（mutator）自己「辅助」标记（mark assist）。
 *
 * 我们的情况：单线程、没有 goroutine 调度，但**有天然的分片点** ——
 * 每次 rc_alloc 都是一次「回到运行时的机会」。所以：
 *
 *   - gc_collect 不再一口气标完，只做根扫描 + 启动增量标记；
 *   - 每次 rc_alloc 推进固定预算（GC_MARK_BUDGET 个对象）；
 *   - 队列清空后进入清扫阶段，同样按预算分片；
 *   - 全程 gc_phase 非 0，rc_alloc 的触发逻辑要尊重它。
 *
 * 这就是 Go 的 mark assist 思想：**让分配方为 GC 付账**，把一次几毫秒的
 * 停顿摊成几百次几十微秒的小步。 */


static void gc_collect(int major) {
    if (!gc_ready || !heap_head || !gc_stack_top) {
        alloc_since_gc = 0;
        return;
    }
    if (rc_stats_on) {
        stat_gc_runs++;
        if (major) stat_gc_major++;
        else stat_gc_minor++;
    }
    /* 诊断模式：单独计本回合耗时，并把「保守根兜底」计数清零。 */
    double st0 = 0;
    if (gc_stats_on) {
        st0 = gc_now_ms();
        for (int i = 0; i < 3; ++i) {
            gc_cons_objs[i] = 0;
            gc_cons_bytes[i] = 0;
        }
    }
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0;
    if (getenv("PYADAMAS_GC_TIMING")) t0 = gc_now_ms();

    // 1. 组装本轮集合。
    if (major) {
        gc_reserve_set(gc_tracked_count);
        gc_set_len = 0;
        for (PyAdamasRc* h = heap_head; h; h = h->next) {
            h->marked = PYADAMAS_RC_YOUNG;
            gc_set_buf[gc_set_len++] = h;
        }
        young_len = 0;
        old_bytes = 0;
    } else {
        gc_reserve_set(young_len);
        if (young_len) memcpy(gc_set_buf, young_buf, sizeof(PyAdamasRc*) * young_len);
        gc_set_len = young_len;
        young_len = 0;
    }
    gc_set = gc_set_buf;
    if (gc_set_len == 0) {
        alloc_since_gc = 0;
        return;
    }
    if (gc_set_len > gc_work_buf_cap) {
        free(gc_work_buf);
        gc_work_buf = (PyAdamasRc**)malloc(sizeof(PyAdamasRc*) * gc_set_len);
        if (!gc_work_buf) panic_c("out of memory");
        gc_work_buf_cap = gc_set_len;
    }
    gc_work = gc_work_buf;
    gc_work_len = 0;
    gc_tab_build();
    if (t0) t1 = gc_now_ms();

    gc_mark_sweep = major;
    if (major) {
        // **整堆收集改成增量式（照抄 Go 的并发标记结构）。**
        //
        // Go 的做法（mgcmark.go 的 gcDrain）：标记工作放队列，后台 worker
        // 不断取对象扫描、子对象入队；分配方在标记期「辅助」推进
        // （gcAssistAlloc），把一次长时间停顿摊成许多小步。
        //
        // 这里只做**根扫描**（必须一次性完成，否则根会变化），然后启动
        // 增量状态；剩下的标记/清扫由每次 rc_alloc 推进 GC_MARK_BUDGET 步。
        for (int64_t i = 0; i < gc_n_globals; ++i)
            gc_mark((uintptr_t)*(void**)gc_globals[i]);
        for (int64_t i = 0; i < temp_len; ++i) gc_mark((uintptr_t)temp_stack[i]);
        if (gc_stats_on) {
            gc_stats_roots(1);
            gc_scan_roots_split();
        } else {
            gc_scan_frames(GC_FRAMES_ALL);
            gc_scan_registers();
        }
        if (t0) t2 = gc_now_ms();
        // **标记同步做完，只把清扫分片。**
        //
        // 试过把标记也分片（Go 的 gcDrain 那样），但 RC + 增量标记有硬冲突：
        // 标记期间对象可能被 RC 释放，标记队列里还留着指向它的指针 ——
        // 即使登记 gc_pend 延后释放，仍有对象在收尾之后才归零（实测
        // 174_gc_construct_roots 漏 21KB）。
        //
        // 根因是 RC 的释放时机不受 GC 控制，而 Go 根本没有 RC。要真正实现
        // 并发标记，必须先让所有释放都经过 GC 的「灰对象」协议 —— 那是
        // 另一套所有权模型（类似 Go 的 span + mbitmap），不是这里能补的。
        //
        // 折中：标记一次做完（根扫描 + drain，实测 1.5ms），清扫分片
        // （gc_free_body 才是真正耗时的部分，要跑 dtor 级联）。
        gc_drain();
        // **本地化优化：小集合直接同步清扫完，不分片。**
        //
        // Go 的后台 worker 是独立 goroutine，分片没有额外代价；我们挂在
        // 分配点上，分片会打断分配路径的缓存局部性 —— 实测分片本身让
        // bench_rc_gen 慢 5ms（73 → 78），而集合小时根本不需要分片。
        //
        // 判据：集合规模小于 GC_INCR_MIN_SET 时，同步清扫的停顿本来就在
        // 亚毫秒级（实测 4 万对象 sweep=0.18ms），分片纯属自找麻烦。
        // 只有大集合才值得分片摊薄。
        if (gc_set_len < GC_INCR_MIN_SET) {
            // **同步清扫分支也必须设 gc_incr_major。**
            //
            // gc_incr_finish 靠这个标志决定是否归零 alloc_since_major 并重算
            // major_threshold。漏设会让 alloc_since_major 永远停在阈值之上，
            // 之后每次分配都触发一次 major —— 实测 180_codepoint_perf 触发了
            // 28 万次 GC，GC 开销 72ms（占程序 46%）。
            gc_incr_major = 1;
            gc_phase = GC_PHASE_SWEEP;
            gc_sweeping = 1;
            for (size_t si = 0; si < gc_set_len; ++si) {
                PyAdamasRc* sh = gc_set[si];
                if (!sh) continue;
                if (!(sh->marked & PYADAMAS_RC_MARK)) {
                    if (rc_stats_on) stat_gc_freed++;
                    gc_free_body(sh);
                    gc_set[si] = NULL;
                }
            }
            gc_incr_sweep_pos = gc_set_len;
            gc_incr_finish();
            return;
        }
        gc_incr_state = 2;
        gc_incr_major = 1;
        gc_incr_sweep_pos = 0;
        gc_incr_countdown = GC_INCR_STRIDE;
        gc_incr_remaining = GC_INCR_STRIDE;
        gc_phase = GC_PHASE_SWEEP;
        gc_sweeping = 1;
        // **标记期间必须让 gc_phase 非 0**，否则 pa_rc_dec 会立即释放对象 ——
        // 而标记队列里可能还有指向它的指针，gc_scan_object 就撞上已释放内存
        // （ASan 实测 heap-use-after-free，13_gc 必崩）。
        //
        // Go 没这个问题是因为它不做 RC；我们必须在标记期间把 RC 释放推迟，
        // 用 gc_phase_dec 登记到 gc_pend，等本轮收尾时统一处理。
        gc_phase = GC_PHASE_INCR_MARK;
        if (t0) t3 = gc_now_ms();
        if (t0) t4 = gc_now_ms();
    } else {
        // 2. 试探减计数：集合内对象之间的出边全部减掉。
        gc_phase = GC_PHASE_TRIAL;
        for (size_t i = 0; i < gc_set_len; ++i) {
            PyAdamasRc* h = gc_set[i];
            if (h->release) h->release((char*)h + PYADAMAS_RC_HDR);
        }
        if (t0) t2 = gc_now_ms();

        // 3. 根：减完仍 > 0 的说明被集合外引用；再保守扫帧记录和寄存器兜住
        //    unknown 局部槽这类「借来的」引用（它们不计入 rc）。exact-RC 模式下
        //    所有引用都归某个槽/临时值/容器所有，真实 rc 已经够用。
        for (size_t i = 0; i < gc_set_len; ++i) {
            PyAdamasRc* h = gc_set[i];
            if (h->rc > 0 && !(h->marked & PYADAMAS_RC_MARK)) {
                h->marked |= PYADAMAS_RC_MARK;
                gc_work[gc_work_len++] = h;
            }
        }
        if (!gc_untracked_leaves) {
            if (gc_stats_on) {
                gc_stats_roots(1);
                gc_scan_roots_split();
            } else {
                gc_scan_frames(GC_FRAMES_ALL);
                gc_scan_registers();
            }
        } else if (gc_stats_on) {
            // exact-RC 模式：所有引用都归槽/临时值/容器所有，没有保守根。
            gc_stats_roots(0);
        }
        gc_drain();
        if (t0) t3 = gc_now_ms();

        // 4. 恢复存活对象的出边计数。
        gc_phase = GC_PHASE_RESTORE;
        for (size_t i = 0; i < gc_set_len; ++i) {
            PyAdamasRc* h = gc_set[i];
            if ((h->marked & PYADAMAS_RC_MARK) && h->release)
                h->release((char*)h + PYADAMAS_RC_HDR);
        }
        if (t0) t4 = gc_now_ms();
    }

    // **增量模式：清扫交给 gc_incr_step 分片做，这里直接返回。**
    if (gc_incr_state) {
        if (t0) {
            fprintf(stderr, "[gc] major incremental roots=%.2f ms set=%zu\n",
                    t2 - t1, gc_set_len);
        }
        return;
    }

    // 5. 清扫：先让垃圾对象把「指向集合外的边」交出来，再统一释放。
    gc_phase = GC_PHASE_SWEEP;
    gc_sweeping = 1;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (!(h->marked & PYADAMAS_RC_MARK) && h->release)
            h->release((char*)h + PYADAMAS_RC_HDR);
    }
    int64_t freed = 0;
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (h->marked & PYADAMAS_RC_MARK) continue;
        if (rc_stats_on) stat_gc_freed++;
        freed++;
        gc_free_body(h);
        /* 释放过的槽清空：后面的晋升阶段还要遍历这张表，而这里的 free
           可能把整页还给系统（Windows 的堆管理器会直接 decommit），
           再去读 h->marked 就是访问已释放内存。放在这里清空，晋升阶段
           只需要判空，语义上与「marked 里没有 MARK」完全等价。 */
        gc_set[i] = NULL;
    }
    gc_sweeping = 0;
    gc_phase = GC_PHASE_NONE;
    gc_mark_sweep = 0;
    if (t0) t5 = gc_now_ms();

    // 6. 晋升：本轮存活的新生代变成老生代。被清扫掉的槽已经是 NULL，
    // 这里不能再解引用它们。
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* h = gc_set[i];
        if (!h) continue;
        h->marked = 0;
        old_bytes += h->size + PYADAMAS_RC_HDR;
    }
    // 7. 清扫期攒下的老对象引用，现在按正常 RC 释放（可能级联）。
    for (size_t i = 0; i < gc_pend_len; ++i) pa_rc_dec(gc_pend[i]);
    gc_pend_len = 0;
    if (t0) t6 = gc_now_ms();

    size_t n_set = gc_set_len;
    gc_set = NULL;
    gc_set_len = 0;
    gc_tab = NULL;
    gc_tab_cap = 0;
    gc_work = NULL;
    gc_work_len = 0;
    stat_gc_live = old_bytes;
    alloc_since_gc = 0;
    // **按 Go pacer 的思路算下一轮 minor 的触发阈值。**
    //
    // 原来 young_threshold 是固定 2MB，与存活堆大小无关：小堆上太保守
    // （频繁扫描），大堆上又太激进（扫得贵却没换来回収量）。
    //
    // Go 的目标堆公式（docs/gc-guide 的 GOGC 一节）：
    //     Target heap = Live heap + (Live heap + GC roots) * GOGC / 100
    // 即「下次收集前允许再分配的新内存 ≈ 存活堆 * GOGC%」。
    //
    // 取 GOGC=100（新分配量达到存活堆一倍时再收集），下限 PYADAMAS_GC_THRESHOLD
    // （2MB）—— 与 Go 的 4MiB 最小堆同理，避免小堆上阈值退化到几百字节、
    // 每次分配都触发收集。
    {
        int64_t live = old_bytes;
        int64_t roots = (int64_t)temp_len * (int64_t)sizeof(void*);
        int64_t target = live + (live + roots);   /* GOGC=100 */
        if (target < (int64_t)PYADAMAS_GC_THRESHOLD)
            target = (int64_t)PYADAMAS_GC_THRESHOLD;
        young_threshold = target;
    }
    if (major) {
        alloc_since_major = 0;
        // major 比 minor 稀疏：老生代翻倍（至少 8MB）才全扫一次，含环的
        // 老对象最多多留一个 major 周期。
        int64_t mt = old_bytes * 2;
        if (mt < 4 * (int64_t)PYADAMAS_GC_THRESHOLD)
            mt = 4 * (int64_t)PYADAMAS_GC_THRESHOLD;
        // **回收率退避**：上一轮 major 一个对象都没回收，说明存活集稳定、
        // 没有环在积累，再按原频率全扫纯属浪费 —— 阈值翻倍，最多到 8 倍。
        //
        // 实测 bench_rc_gen：10 万存活字符串 + 15 万短命容器，4 次 major
        // 每次扫 10 万对象、freed=0，合计 20ms —— 占整个程序 92ms 的 22%。
        // 退避后同样负载只扫 1 次，其余交给 minor。
        if (freed == 0 && n_set > 0) {
            // **零回收退避。**
            //
            // 上一轮 major 一个对象都没回收，说明存活集稳定、没有环在积累，
            // 再按原频率全扫纯属浪费。实测 bench_rc_gen 的容器段：
            // 15 万次 `['x'*16, str(i), i*2]` 触发了 8208 次 major，每次
            // set=5、freed=0 —— 全是空扫。
            //
            // 关键是**必须有绝对下限**：这里 live_bytes 只有 520 字节，
            // 按 `old_bytes * 8` 算出来的阈值才 4KB，比一次分配还小，
            // 于是每次分配都触发 major。下限取 4*THRESHOLD（8MB）。
            if (gc_zero_free_streak < 3) ++gc_zero_free_streak;
            mt = old_bytes * 8;
            if (mt < 4 * (int64_t)PYADAMAS_GC_THRESHOLD)
                mt = 4 * (int64_t)PYADAMAS_GC_THRESHOLD;
        } else {
            gc_zero_free_streak = 0;
        }
        major_threshold = mt;
    }
    if (gc_stats_on) gc_stats_report(major, n_set, freed, gc_now_ms() - st0);
    if (t0)
        fprintf(stderr,
                "[gc] %s n=%zu freed=%lld live=%lld tab=%.2f trial=%.2f mark=%.2f "
                "restore=%.2f sweep=%.2f pend=%.2f ms\n",
                major ? "major" : "minor", n_set, (long long)freed,
                (long long)old_bytes, t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4,
                t6 - t5);
}

void pa_gc_collect(void) { gc_collect(1); }

// 程序退出：所有还活着的堆对象（包括环）一次性还给系统。
void pa_gc_shutdown(void) {
    // 先放掉全局根：sys.argv 的字符串在 exact-RC 模式下不在 GC 堆链上，
    // 只能靠 RC 释放（见 pa_sys_argv_shutdown 的注释）。
    pa_sys_argv_shutdown();
    gc_sweeping = 1;
    while (heap_head) gc_free_body(heap_head);
    gc_sweeping = 0;
    alloc_since_gc = 0;
    alloc_since_major = 0;
    old_bytes = 0;
    young_len = 0;
    free(young_buf);
    young_buf = NULL;
    young_cap = 0;
    free(gc_globals);
    gc_globals = NULL;
    gc_n_globals = 0;
    gc_globals_cap = 0;
    free(gc_work_buf);
    gc_work_buf = NULL;
    gc_work_buf_cap = 0;
    free(gc_set_buf);
    gc_set_buf = NULL;
    gc_set_buf_cap = 0;
    free(gc_tab_buf);
    gc_tab_buf = NULL;
    gc_tab_buf_cap = 0;
    free(gc_pend);
    gc_pend = NULL;
    gc_pend_len = 0;
    gc_pend_cap = 0;
    // 分配器缓存也要还掉。字符串池和列表 freelist 是**纯缓存**：
    // 回收对象时不立刻 free，而是挂进池里等复用。进程退出时这些块
    // 还挂在池里，既不在 GC 堆链上、也没有所有者 ——
    // ASan 的 LeakSanitizer 会把它们报成「Direct leak」（实测每个用例
    // 120-464 字节，来自 sys.argv 等全局字符串）。
    //
    // 这不影响正确性（进程都要退出了），但会让 ASan 的泄漏报告淹没有
    // 价值的信号。顺手还掉，也顺便验证 shutdown 路径是完整的。
    for (int i = 0; i < STR_POOL_CLASSES; ++i) {
        while (str_pool[i]) {
            PyAdamasRc* h = str_pool[i];
            str_pool[i] = h->next;
            free(h);
        }
        str_pool_count[i] = 0;
    }
    while (list_pool) {
        PyAdamasRc* h = list_pool;
        list_pool = h->next;
        free(h);
    }
    list_pool_count = 0;
    pa_list_buf_pool_shutdown();
    // 异常子系统的全局表（handler freelist / 挂起表 / 登记表 / 嵌套节点）。
    // 见 pa_exc_shutdown 的说明：都是缓存，但不清会让 LeakSanitizer 报一堆
    // 没有信息量的 Direct leak。
    pa_exc_shutdown();
}

/* 分模块编译：init 是模块顶层生成的 void(void)，ready 是模块 .bss 里的
   一次性标志。标志在调用前设置，初始化失败不会重试（与 Python 移除
   失败模块的行为不同，但至少不会死循环）。 */
void pa_import_module(int64_t* ready, void (*init)(void)) {
    if (!ready || !init || *ready) return;
    *ready = 1;
    init();
}

/* 生成代码调用的入口（codegen 直接 emit call pa_rc_inc）。
   运行时内部的其他 .c 用头文件里的 static inline 版本，不经过这里。 */
void pa_rc_inc(void* p) {
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (h->rc <= 0) { pa_rc_inc_slow(p); return; }
    if (pa_rc_stats_on_fast) { pa_rc_inc_slow(p); return; }
    h->rc++;
}

void pa_rc_dec(void* p) {
    if (!p) return;
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->rc == PYADAMAS_RC_IMMORTAL) return;
    if (pa_rc_stats_on_fast || pa_gc_phase_fast || h->rc <= 1) {
        pa_rc_dec_slow(p);
        return;
    }
    h->rc--;
}
