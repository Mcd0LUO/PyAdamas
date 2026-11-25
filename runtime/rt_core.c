// rt_core.c —— 引用计数、临时值栈、闭包、字符串分配、循环收集器
#define PA_RC_NO_INLINE   /* 本文件提供 pa_rc_inc/dec 的外部符号 */
#include "rt_internal.h"

/* ══════════════════════════════════════════════════════════════════
   分配器调试模式：尾部 canary / guard page（PYADAMAS_CANARY / PYADAMAS_GUARD）

   **零开销的达成方式**：这些钩子只有在「另编一遍 runtime」的 rt_allocdbg.o
   里才展开成真正的分配器（CMake 用 -DPYADAMAS_ALLOC_DEBUG=1 重编
   runtime 目录下的每个 rt_*.c 得到它）。默认 rt.o 里 PA_DBG_MALLOC/PA_DBG_FREE 逐字
   展开成改动前的 malloc/free —— 生成程序链接的运行时对象不变，12 个
   基准的 instructions:u 逐项相同。为什么不用运行期 if：一个分支也是
   指令，验收要求关的时候指令数完全不变。
   Python 驱动在 PYADAMAS_CANARY / PYADAMAS_GUARD 非空时改链 rt_allocdbg.o。

   模式（运行期从环境变量解析一次，只在 rt_allocdbg.o 里读）：
     PYADAMAS_CANARY=1   尾部 32 字节区：前 16 字节固定 0xC5；释放时校验。
     PYADAMAS_CANARY=2   同上 + 校验头字（size/kind 与分配时记录的一致、
                         rc 在合理范围内）。
     PYADAMAS_GUARD=1    每个对象单独 mmap：数据页尾部对齐 + 紧跟一页
                         PROT_NONE；越界写立刻 SIGSEGV；释放时 munmap。

   覆盖范围：所有走 rc_alloc/rc_alloc_ex/rc_alloc_untracked(_ex) 的对象
   （str/list/dict/实例/闭包/生成器帧/异常/range/class/big/JSON 盒…）。
   **不覆盖**：容器内部的二级 malloc 缓冲（list->data、dict->entries、
   big 的 limb 在块内所以覆盖）、生成代码的栈/帧内联槽、静态字面量。
   ================================================================ */
#ifdef PYADAMAS_ALLOC_DEBUG
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif
/* 调试版不用池取块（rc_alloc_core 里绕开），5 个 pool_get 变成未使用。 */
#define PA_DBG_UNUSED __attribute__((unused))

#define PA_DBG_CANARY 16
#define PA_DBG_CANARY_TOTAL 32 /* 16 字节 canary + 16 字节头字期望值 */
#define PA_DBG_FILL 0xC5

enum {
    PA_DBG_MODE_OFF = 0,
    PA_DBG_MODE_CANARY = 1,
    PA_DBG_MODE_CANARY_HEAD = 2,
    PA_DBG_MODE_GUARD = 3,
};

static int pa_dbg_mode_cache = -1;

static int pa_dbg_mode(void) {
    int m = pa_dbg_mode_cache;
    if (__builtin_expect(m >= 0, 1)) return m;
#if defined(_WIN32)
    m = PA_DBG_MODE_CANARY; /* Windows 只支持 canary（无 mmap guard page） */
#else
    const char* g = getenv("PYADAMAS_GUARD");
    if (g && g[0] != '\0' && strcmp(g, "0") != 0) {
        m = PA_DBG_MODE_GUARD;
    } else {
        const char* c = getenv("PYADAMAS_CANARY");
        if (!c || c[0] == '\0' || strcmp(c, "0") == 0) m = PA_DBG_MODE_OFF;
        else m = (strcmp(c, "2") == 0) ? PA_DBG_MODE_CANARY_HEAD : PA_DBG_MODE_CANARY;
    }
#endif
    pa_dbg_mode_cache = m;
    return m;
}

#if !defined(_WIN32)
static size_t pa_dbg_page(void) {
    static size_t p;
    if (!p) p = (size_t)sysconf(_SC_PAGESIZE);
    if (!p) p = 4096;
    return p;
}
#endif

static void pa_dbg_panic_canary(PyAdamasRc* h, int i, unsigned char want,
                               unsigned char got) {
    char msg[320];
    snprintf(msg, sizeof msg,
             "canary 被踩：对象 kind=%lld size=%lld 偏移=%d 期望=0x%02x 实际=0x%02x",
             (long long)h->kind, (long long)h->size, i, (unsigned)want, (unsigned)got);
    panic_c(msg);
}

/* 分配：返回 RC 头指针（payload = h + sizeof(PyAdamasRc)）。 */
static PyAdamasRc* pa_dbg_alloc(int64_t alloc_size) {
    int mode = pa_dbg_mode();
    if (mode == PA_DBG_MODE_GUARD) {
#if defined(_WIN32)
        panic_c("PYADAMAS_GUARD 只支持 POSIX（Windows 用 PYADAMAS_CANARY）");
#else
        size_t page = pa_dbg_page();
        size_t total = (size_t)PYADAMAS_RC_HDR + (size_t)alloc_size;
        /* payload 起点保持 16 字节对齐（与普通 malloc 版一致）；尾部会有
           最多 15 字节尾隙，超出对象末尾的写仍会撞 guard 页。 */
        total = (total + 15) & ~(size_t)15;
        size_t data_pages = (total + page - 1) / page;
        if (data_pages == 0) data_pages = 1;
        size_t maplen = (data_pages + 1) * page;
        char* base = (char*)mmap(NULL, maplen, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) panic_c("out of memory (guard mmap)");
        char* data_end = base + data_pages * page;
        if (mprotect(data_end, page, PROT_NONE) != 0)
            panic_c("guard: mprotect(PROT_NONE) failed");
        return (PyAdamasRc*)(data_end - total);
#endif
    }
    size_t total = (size_t)PYADAMAS_RC_HDR + (size_t)alloc_size + PA_DBG_CANARY_TOTAL;
    PyAdamasRc* h = (PyAdamasRc*)malloc(total);
    if (!h) panic_c("out of memory");
    memset((char*)h + PYADAMAS_RC_HDR + alloc_size, PA_DBG_FILL,
           PA_DBG_CANARY_TOTAL);
    return h;
}

/* 头字都填好之后调用（size/kind 已就位）：mode 2 把期望值存进 canary
   区尾部，释放时对照。 */
static void pa_dbg_head_init(PyAdamasRc* h) {
    if (pa_dbg_mode() != PA_DBG_MODE_CANARY_HEAD) return;
    char* c = (char*)h + PYADAMAS_RC_HDR + (size_t)h->size + PA_DBG_CANARY;
    memcpy(c, &h->size, sizeof h->size);
    memcpy(c + sizeof h->size, &h->kind, sizeof h->kind);
}

static void pa_dbg_free(PyAdamasRc* h) {
    int mode = pa_dbg_mode();
    if (mode == PA_DBG_MODE_GUARD) {
#if !defined(_WIN32)
        size_t page = pa_dbg_page();
        size_t total = (size_t)PYADAMAS_RC_HDR + (size_t)h->size;
        total = (total + 15) & ~(size_t)15;
        char* data_end = (char*)h + total;
        if (((uintptr_t)data_end & (page - 1)) != 0) {
            char msg[192];
            snprintf(msg, sizeof msg,
                     "guard: 对象尾部未对齐到页边界（头字被踩？kind=%lld size=%lld）",
                     (long long)h->kind, (long long)h->size);
            panic_c(msg);
        }
        size_t data_pages = (total + page - 1) / page;
        char* base = data_end - data_pages * page;
        if (munmap(base, (data_pages + 1) * page) != 0)
            panic_c("guard: munmap failed");
        return;
#endif
    }
    unsigned char* c = (unsigned char*)h + PYADAMAS_RC_HDR + (size_t)h->size;
    for (int i = 0; i < PA_DBG_CANARY; ++i)
        if (c[i] != PA_DBG_FILL) pa_dbg_panic_canary(h, i, PA_DBG_FILL, c[i]);
    if (mode == PA_DBG_MODE_CANARY_HEAD) {
        int64_t exp_size = 0, exp_kind = 0;
        memcpy(&exp_size, c + PA_DBG_CANARY, sizeof exp_size);
        memcpy(&exp_kind, c + PA_DBG_CANARY + sizeof exp_size, sizeof exp_kind);
        if (exp_size != h->size || exp_kind != h->kind || h->rc < 0 ||
            h->rc > (int64_t)1 << 40) {
            char msg[320];
            snprintf(msg, sizeof msg,
                     "canary: 头字被踩：kind=%lld/%lld size=%lld/%lld rc=%lld",
                     (long long)h->kind, (long long)exp_kind, (long long)h->size,
                     (long long)exp_size, (long long)h->rc);
            panic_c(msg);
        }
    }
    free(h);
}

#define PA_DBG_MALLOC(alloc_size) pa_dbg_alloc(alloc_size)
#define PA_DBG_FREE(h) pa_dbg_free(h)
#define PA_DBG_HEAD_INIT(h) pa_dbg_head_init(h)
#else  /* !PYADAMAS_ALLOC_DEBUG：默认运行时，逐字展开成改动前的调用 */
#define PA_DBG_UNUSED
#define PA_DBG_MALLOC(alloc_size) malloc(sizeof(PyAdamasRc) + (size_t)(alloc_size))
#define PA_DBG_FREE(h) free(h)
#define PA_DBG_HEAD_INIT(h) ((void)0)
#endif

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
/* 分配热路径的快速判断标志：0 = 什么都不用做（绝大多数情况）。
   见 rc_alloc 里的说明。 */
int gc_need_check;
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
/* **索引叶子**（0.321）：对象不进 GC 堆链、不进新生代、不进本轮集合，
   但仍留在存活堆索引里（unknown 值探针靠索引识别堆指针，不能整体
   untrack，否则 unknown 槽里的字符串会漏释放）。

   适用对象：str / bytes（PYADAMAS_RC_STR）。字符串不可变、没有出边，
   不可能处在环里，生死完全由 RC 决定 —— GC 永远收不到它们。让它们
   参与 GC 记账（堆链、新生代、阈值计数、mark/sweep 遍历）是纯开销：
   b1_log_parse 实测字符串分配把 GC 触发次数从 0 抬到 8 次，堆链/新生代
   的插摘又是每次分配一笔（见 CHANGELOG 0.321 的取证）。

   与 UNTRACKED 的区别：UNTRACKED 是 exact-RC 模式（索引整体关闭）的
   叶子，连索引都不进；LEAF 保留索引成员资格，只是不参与 GC 记账。
   与 0.276「kind-0 元组 rc_alloc_untracked」同思路，区别是元组连索引
   都不需要（它们只出现在静态类型已知的槽里）。 */
#define PYADAMAS_RC_LEAF 16
/* **延迟释放位**（GC 阶段1，0.330）：增量标记/清扫期 RC 归零的对象不立即
   free，而是标灰保活、登记 gc_pend，本轮收尾统一释放。该位标记「已登记延迟
   释放」，防止同一对象重复入 pend（收尾会二次释放）。仅 gc_incr_mark_on 时
   使用；flag-off  legacy 路径不置位。收尾清空 marked 时一并清除。 */
#define PYADAMAS_RC_DEFER 32

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
/* **增量标记总开关**（GC 阶段1，0.330）：PYADAMAS_GC_INCR_MARK=1 时 major
   走「根扫描 → 分片标记（state=1）→ 分片清扫（state=2）」，配合 Yuasa 删除
   屏障 + 门控插入屏障。默认 0 = 现状同步标记，flag-off 行为/asm/leak 逐字不变。
   入口 pa_gc_init 读一次环境变量缓存成全局，热路径只读缓存。 */
static int gc_incr_mark_on;
int pa_gc_incr_mark_fast;   /* gc_incr_mark_on 的公开别名，供运行时写屏障门控 */
static int64_t gc_incr_major;   /* 本轮是不是整堆收集 */
static size_t gc_incr_sweep_pos;
static int gc_incr_countdown;
static int gc_incr_remaining;
/* **增量工作的累计耗时**（毫秒）。
 *
 * 之前 gcstats 的 total_ms 只统计了 gc_collect 的墙钟时间，而增量模式下
 * 真正的标记/清扫发生在 gc_incr_step 里 —— 由分配点驱动，散布在程序
 * 各处，从未被计时。实测 bench_rc_gen 的 GC 成本是 12ms（GC 禁用对照），
 * 但 gcstats 只报 0.644ms，差 19 倍就是这个原因。 */
static double gc_incr_ms;
/* 逐函数计时（只在 PYADAMAS_PROF=1 时启用）。用 rdtsc 风格的轻量计数：
   clock_gettime 本身约 25ns，对被计时的短函数有干扰，所以只在显式开启时用。 */


static int64_t gc_incr_calls, gc_incr_objs;
static double gc_now_ms(void);
static void gc_incr_finish(void);
static void gc_incr_step(void);
static void gc_free_body(PyAdamasRc* h);
static void gc_alloc_black(PyAdamasRc* h);
static void rc_leaf_free(PyAdamasRc* h, void* p);

static void gc_collect(int major);
static void gc_pend_add(int64_t v);
void gc_mark(uintptr_t v);   /* 删除屏障 shade 用（定义在本文件后段） */

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
        if (gc_incr_mark_on) {
            /* **Yuasa 删除屏障（shade-on-delete）**——GC 阶段1 核心。
               标记期 mutator 的 RC 把对象最后一个引用删掉（rc 归零）时：
               不能立即 free（标记队列/快照可能还指向它 → 13_gc UAF），
               也不能只延后 free 而不标灰（白色对象会被清扫误收，且与
               gc_pend 收尾二次释放 → 174 漏 21KB）。协议：
                 1. rc>1 的 dec 正常减计数（对象仍活，无需屏障）；
                 2. rc==1 的归零 dec：shade（标灰入 gc_work，标记器会扫到
                    它并保住其子树），置 DEFER 位防重复入队，登记 gc_pend，
                    **rc 保持 1 保活**到本轮标记排空 + gc_incr_finish 收尾。
               收尾时 gc_phase 已复位，pa_rc_dec 把 rc 1→0 正常释放（跑 dtor
               级联）。shade 用 gc_mark：集合内对象标灰入队；新对象不在快照
               查表落空（no-op），靠 DEFER+pend 收尾释放，不被清扫（不在 set）。
               归零对象必无环（环会保 rc>=1），故 shade 的子树与活对象共享时
               由标记器保住，收尾 dtor 再正常 dec —— 不会误收活对象。 */
            if (h->rc > 1) { h->rc--; return; }
            if (!(h->marked & PYADAMAS_RC_DEFER)) {
                h->marked |= PYADAMAS_RC_DEFER;
                gc_mark((uintptr_t)p);
                gc_pend_add((int64_t)(uintptr_t)p);
            }
            return;   /* rc 保持 1，保活到收尾 */
        }
        // legacy（flag-off）：标记期间对象归零统一登记待释放，本轮收尾处理。
        // 不区分新老对象 —— 新对象也会进 gc_pend，而 gc_incr_finish 里
        // gc_phase 已复位，pa_rc_dec 会走正常释放路径。
        gc_pend_add((int64_t)(uintptr_t)p);
        return;
    }
    if (h->marked & PYADAMAS_RC_UNTRACKED) {
        if (h->rc > 0 && --h->rc == 0) PA_DBG_FREE(h);
        return;
    }
    // 索引叶子：不在本轮集合里，trial/restore 不会碰它（marked 没有
    // YOUNG 位）；清扫阶段容器交出引用时按普通 RC 就地归零释放 ——
    // 与 exact-RC 的 untracked 叶子同一套论证，只是还要维护索引成员资格、
    // 走 freelist 回收（str_pool）。
    if (h->marked & PYADAMAS_RC_LEAF) {
        if (h->rc > 0 && --h->rc == 0) rc_leaf_free(h, p);
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
   pa_disable_unknown_index 关掉它，省掉每个堆对象的插入/删除。
   0.303：非 static —— rt_internal.h 的共享探针快路要读它判断是否
   exact-RC 模式（常规模式 rc_index_has 命中即返回，省一层函数调用）。 */
int rc_index_off;
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

/* **索引叶子模式**（PYADAMAS_RC_LEAF，0.321）：索引维护开启时（非
   exact-RC），str/bytes 也不进 GC 记账，只在索引里留成员资格。
   PYADAMAS_GC_CONSERVATIVE=1 时关掉，回到旧的「字符串全跟踪」口径，
   作为行为对照 / 排障回退。入口 pa_gc_init 里一次性设定。 */
static int gc_leaf_mode;

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
    // **增量工作的单独统计。** total_ms 只含 gc_collect 的墙钟时间，
    // 增量模式下真正的标记/清扫在 gc_incr_step 里，必须单独报。
    fprintf(stderr, "[gcstats] incremental_ms=%.3f calls=%lld objs=%lld (真正的标记/清扫耗时)\n",
            gc_incr_ms, (long long)gc_incr_calls, (long long)gc_incr_objs);
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
/* 每档最多缓存多少个块。
 *
 * 原来统一 64。实测 `text.split()` 场景：每轮创建 160 个内联短串
 * （sizeof(PyAdamasStr)=32 字节，落在 class 3），池只能存 64 个 ——
 * 360021 次放入里 **232082 次（64%）溢出**直接 free。
 *
 * 这里按档位区分：小档（<= 40 字节，覆盖内联串和短堆串）给更大容量，
 * 大档保持 64（大对象缓存太多不值，内存代价高）。
 *
 * 内存代价：小档多缓存 (512-64) 个 × ~48 字节 ≈ 22KB/档，可接受。 */
#define STR_POOL_MAX_PER_CLASS 64
/* 小档（含内联字符串 32 字节）的容量上限。 */
#define STR_POOL_MAX_SMALL 512
/* 小档的上界字节数（含内联串的 32 + 短堆串的几十字节）。 */
#define STR_POOL_SMALL_LIMIT 40
static PyAdamasRc* str_pool[STR_POOL_CLASSES];
static int str_pool_count[STR_POOL_CLASSES];

static int str_pool_class(int64_t size) {
    if (size <= 0 || size > STR_POOL_MAX_SIZE) return -1;
    return (int)((size + 7) / 8) - 1;
}

static PA_DBG_UNUSED PyAdamasRc* str_pool_get(int64_t size) {
    int c = str_pool_class(size);
    if (c < 0 || !str_pool[c]) return NULL;
    PyAdamasRc* h = str_pool[c];
    str_pool[c] = h->next;
    str_pool_count[c]--;
    return h;
}

/* 非 ASCII 串的懒码点边界索引（建表在 str_cp_index，见下）。
   索引数组用的是裸 malloc，生命周期跟字符串走 —— 这里在块回收前还掉。
   SSO 短串（hash 是字符区）和只读字面量（rc == IMMORTAL，.data.rel.ro
   不可写，hash 存的是编译期字典哈希）从不缓存，直接跳过。 */
static void str_cp_index_free(PyAdamasStr* s) {
    if (s->data == (const char*)&s->hash) return;
    PyAdamasRc* h = (PyAdamasRc*)((char*)s - PYADAMAS_RC_HDR);
    if (h->rc == PYADAMAS_RC_IMMORTAL || h->kind != PYADAMAS_RC_STR) return;
    if (s->hash) {
        free((void*)(uintptr_t)s->hash);
        s->hash = 0;
    }
}

static void str_pool_put(PyAdamasRc* h) {
    str_cp_index_free((PyAdamasStr*)((char*)h + PYADAMAS_RC_HDR));
    /* 调试版（canary/guard）不缓存：直接释放（canary 在这里校验）。 */
#ifdef PYADAMAS_ALLOC_DEBUG
    PA_DBG_FREE(h);
    return;
#endif
    int c = str_pool_class(h->size);
    int64_t cap = ((c + 1) * 8 <= STR_POOL_SMALL_LIMIT) ? STR_POOL_MAX_SMALL
                                                       : STR_POOL_MAX_PER_CLASS;
    if (c < 0 || str_pool_count[c] >= cap) {

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

/* ── dict 对象池 ───────────────────────────────────────────────
 *
 * `list_pool` 只收 `size == sizeof(PyAdamasList)` 的对象，而
 * `sizeof(PyAdamasDict) == 136` —— dict 从来不进池，每次创建都 malloc、
 * 每次销毁都 free。
 *
 * 实测 `bench_rc_literals`：每轮创建 1 list + 1 tuple + 1 dict，共 3 万次
 * 容器分配；其中 1 万次 dict 全部走 malloc/free。单开一个 dict 池。
 *
 * 容量取 256：dict 对象大（136 字节），缓存 256 个约 35KB，可接受；
 * 而热循环里同时存活的 dict 通常只有个位数。 */
#define DICT_POOL_MAX 256
/* 0.281：小 dict 的三块缓冲（entries/order/index）随头一起池化。
   rec_memo 每轮新建 memo dict：3 次 malloc + 增长 reindex + 销毁 free，
   malloc/free/reindex 合计占运行时 14%。缓冲保留后池命中零 malloc、
   零增长（缓冲已是上一世的高水位）。缓冲指针寄存在池节点里 ——
   payload 会被 rc_alloc_core 的 memset 清零，不能留在 payload；
   cap>64 / icap>128 的大 dict 缓冲直接 free 不保留，防池囤内存。 */
typedef struct {
    PyAdamasRc* h;
    PyAdamasDictEntry* entries;
    int32_t* order;
    int32_t* index;
    int32_t cap, icap;
} DictPoolNode;
static DictPoolNode dict_pool[DICT_POOL_MAX];
static int dict_pool_count;
static DictPoolNode dict_last_got;

static PA_DBG_UNUSED PyAdamasRc* dict_pool_get(void) {
    if (!dict_pool_count) return NULL;
    DictPoolNode n = dict_pool[--dict_pool_count];
    dict_last_got = n;
    return n.h;
}

static void dict_pool_put(PyAdamasRc* h) {
#ifdef PYADAMAS_ALLOC_DEBUG
    /* 调试版不缓存 dict 缓冲：dtor（dict_destroy）不解二级缓冲，这里
       由 put 负责还掉，再释放头块（canary 在这里校验）。 */
    PyAdamasDict* dd = (PyAdamasDict*)(h + 1);
    free(dd->entries);
    free(dd->order);
    free(dd->index);
    PA_DBG_FREE(h);
    return;
#endif
    PyAdamasDict* d = (PyAdamasDict*)(h + 1);
    DictPoolNode n;
    n.h = h;
    if (d->entries && d->cap <= 64 && d->icap <= 128) {
        n.entries = d->entries;
        n.order = d->order;
        n.index = d->index;
        n.cap = (int32_t)d->cap;
        n.icap = (int32_t)d->icap;
    } else {
        free(d->entries);
        free(d->order);
        free(d->index);
        n.entries = NULL;
        n.order = NULL;
        n.index = NULL;
        n.cap = 0;
        n.icap = 0;
    }
    if (dict_pool_count >= DICT_POOL_MAX) {
        free(n.entries);
        free(n.order);
        free(n.index);
        free(h);
        return;
    }
    dict_pool[dict_pool_count++] = n;
}

static PA_DBG_UNUSED PyAdamasRc* list_pool_get(void) {
    PyAdamasRc* h = list_pool;
    if (h) {
        list_pool = h->next;
        list_pool_count--;
    }
    return h;
}

/* CPYFLOAT（8 字节 double payload）盒子池：unknown 域的动态 float 结果
   每算一次就要一个盒。盒子无出边、大小固定，回收时挂 freelist，复用
   时由 rc_alloc_core 重新 rc_link/rc_index_put（与 list/dict 池同款）。 */
#define FLOAT_BOX_POOL_MAX 1024
static PyAdamasRc* float_box_pool;
static int float_box_pool_count;

static PA_DBG_UNUSED PyAdamasRc* float_box_pool_get(void) {
    PyAdamasRc* h = float_box_pool;
    if (h) {
        float_box_pool = h->next;
        float_box_pool_count--;
    }
    return h;
}

static void float_box_pool_put(PyAdamasRc* h) {
#ifdef PYADAMAS_ALLOC_DEBUG
    PA_DBG_FREE(h);
    return;
#endif
    if (float_box_pool_count >= FLOAT_BOX_POOL_MAX) {
        free(h);
        return;
    }
    h->next = float_box_pool;
    h->prev = NULL;
    float_box_pool = h;
    float_box_pool_count++;
}

static void list_pool_put(PyAdamasRc* h) {
#ifdef PYADAMAS_ALLOC_DEBUG
    PA_DBG_FREE(h);
    return;
#endif
    if (list_pool_count >= LIST_POOL_MAX) {
        free(h);
        return;
    }
    h->next = list_pool;
    h->prev = NULL;
    list_pool = h;
    list_pool_count++;
}

/* 小列表合并分配 freelist：RC 头 + PyAdamasList + 8 元素内联缓冲一块
   （64 + 48 + 64 = 176 字节）。建表时一次池操作同时拿到头和缓冲，
   销毁时整块挂回（缓冲是否还内联由 list_destroy 按指针判断）。
   增长过的列表缓冲已换外部块，但头块仍是 176 字节，照样挂回这里，
   下次复用时重新启用内联缓冲。
   0.301：上限 64 -> 512。sort_struct 每轮 500 个元组同时存活，旧上限
   64 让每轮 ~436 个头块走裸 malloc/free（perf：_int_malloc 9% 全在
   rc_alloc_core、_int_free 族 15% 全在 pa_rc_dec_heap_batch）——列表
   data 缓冲入池后它们才是 malloc 大头。512 覆盖 500 元素表，第二轮起
   头块零 malloc；512×176B≈90KB 缓存上限，shutdown 全部释放。 */
#define LIST_INLINE_POOL_MAX 512
/* 导出给生成代码：kind-0 元组分配内联快路径直接弹这个池（0.280）。
   不变量见 rc_alloc_core：池块的 dtor/release/kind/size 恒定。 */
PyAdamasRc* pa_list_inline_pool;
int pa_list_inline_pool_count;

static PA_DBG_UNUSED PyAdamasRc* list_inline_pool_get(void) {
    PyAdamasRc* h = pa_list_inline_pool;
    if (h) {
        pa_list_inline_pool = h->next;
        pa_list_inline_pool_count--;
    }
    return h;
}

static void list_inline_pool_put(PyAdamasRc* h) {
#ifdef PYADAMAS_ALLOC_DEBUG
    PA_DBG_FREE(h);
    return;
#endif
    if (pa_list_inline_pool_count >= LIST_INLINE_POOL_MAX) {
        free(h);
        return;
    }
    h->next = pa_list_inline_pool;
    h->prev = NULL;
    pa_list_inline_pool = h;
    pa_list_inline_pool_count++;
}

/* 索引叶子的「归零释放」：普通 RC 路径走 dec_slow 的 rc==0 分支，
   收集期（容器交出引用）走这里。口径与 dec_slow 一致 ——
   撤销索引成员资格 → dtor → freelist 回收。 */
static void rc_leaf_free(PyAdamasRc* h, void* p) {
    if (rc_stats_on) stat_rc_free++;
    if (__builtin_expect(pa_diag_rc_on != 0, 0)) pa_diag_rc_free(p, (int32_t)h->kind);
    rc_unlink(h);
    if (h->dtor) h->dtor(p);
    if (h->kind == PYADAMAS_RC_STR) str_pool_put(h);
    else if (h->kind == PYADAMAS_RC_CPYFLOAT) float_box_pool_put(h);
    else PA_DBG_FREE(h);
}

/* exact-RC 模式下可以完全绕开 GC 的对象：没有任何堆字段（没有出边），
   不可能处在环里。字符串是典型；没有堆字段的类实例（生成代码不给
   dtor/release）同理。它们的生死完全由 RC 决定。 */
static int rc_leaf_untracked(const PyAdamasRc* h) {
    if (!gc_untracked_leaves) return 0;
    if (h->kind == PYADAMAS_RC_STR) return 1;
    // 0.324：unknown 域的 float 盒无出边、不可变，按叶子处理。
    if (h->kind == PYADAMAS_RC_CPYFLOAT) return 1;
    // range 只存边界，没有堆字段（出边为零），不可能处在环里。
    if (h->kind == PYADAMAS_RC_RANGE) return 1;
    // 值类型 struct：payload 只有标量字段，同样没有出边。
    if (h->kind == PYADAMAS_RC_STRUCT) return 1;
    if (h->kind == PYADAMAS_RC_INSTANCE && !h->dtor && !h->release) return 1;
    return 0;
}

void panic_c(const char* msg);

/* ── 索引取证统计（PYADAMAS_RC_INDEX_STATS=1，仅诊断用）─────────────
 * 回答三件事：命中率、哈希平均探测长度、结构规模（峰值容量 / 页数 /
 * 地址跨度）。关闭时每个统计点只多读一个全局 int（可预测分支），与
 * PYADAMAS_RC_STATS 同一套路；数字本身不进任何决策路径。 */
static int rc_istats_on;
static int64_t ist_has, ist_hash_hit, ist_leaf_hit, ist_miss;
static int64_t ist_has_al16, ist_has_unal16, ist_has_leafmode;
static int64_t ist_page_hit, ist_page_peak, ist_page_cap_peak;
/* 页位图的验收计数：put 进来的**非对齐**对象只能是静态字面量（rc=IMMORTAL，
   payload-1 是奇数地址）。非对齐的**真堆对象**会让位图漏判（假阴性 -> 漏
   回收），所以 unaligned_heap 必须在全套件 + ASan 下恒为 0。 */
static int64_t ist_put_page, ist_put_lit, ist_put_unaligned_heap;
static int64_t ist_miss_al, ist_miss_unal;
static int64_t ist_probes;          /* 主表槽位读取次数（含终止空槽） */
static int64_t ist_probes_hit, ist_probes_miss;
static int64_t ist_probe_hist[9];   /* 1..8+ 步 */
static int64_t ist_put, ist_del;
static int64_t ist_peak_cap, ist_peak_live, ist_peak_fill;
static int64_t ist_leaf_pages, ist_leaf_pages_peak;
static uintptr_t ist_min_page = (uintptr_t)-1, ist_max_page;

static void rc_istats_dump(void) {
    fprintf(stderr,
            "[rcidx] has=%lld al16=%lld unal16=%lld hash_hit=%lld "
            "leaf_queries=%lld leaf_hit=%lld miss=%lld\n",
            (long long)ist_has, (long long)ist_has_al16,
            (long long)ist_has_unal16, (long long)ist_hash_hit,
            (long long)ist_has_leafmode, (long long)ist_leaf_hit,
            (long long)ist_miss);
    double avg = ist_has ? (double)ist_probes / (double)ist_has : 0.0;
    fprintf(stderr, "[rcidx] main probes=%lld avg=%.3f hist=",
            (long long)ist_probes, avg);
    for (int i = 0; i < 8; ++i)
        fprintf(stderr, "%s%d:%lld", i ? " " : "", i + 1,
                (long long)ist_probe_hist[i]);
    fprintf(stderr, " 9+:%lld\n", (long long)ist_probe_hist[8]);
    fprintf(stderr,
            "[rcidx] hash: hit=%lld (%lld probes)  hash_miss_probes=%lld "
            "page_hit=%lld  leaf_hit=%lld  miss al/unal=%lld/%lld\n",
            (long long)ist_hash_hit, (long long)ist_probes_hit,
            (long long)ist_probes_miss, (long long)ist_page_hit,
            (long long)ist_leaf_hit, (long long)ist_miss_al,
            (long long)ist_miss_unal);
    fprintf(stderr,
            "[rcidx] put=%lld del=%lld peak cap=%lld live=%lld fill=%lld "
            "(load=%s)\n",
            (long long)ist_put, (long long)ist_del, (long long)ist_peak_cap,
            (long long)ist_peak_live, (long long)ist_peak_fill,
            ist_peak_cap ? "" : "-");
    if (ist_peak_cap)
        fprintf(stderr, "[rcidx] peak fill/cap=%.1f%%  live/cap=%.1f%%\n",
                100.0 * (double)ist_peak_fill / (double)ist_peak_cap,
                100.0 * (double)ist_peak_live / (double)ist_peak_cap);
    fprintf(stderr, "[rcidx] page_map: peak_live_pages=%lld peak_cap=%lld "
            "put(page/literal)=%lld/%lld unaligned_heap_put=%lld\n",
            (long long)ist_page_peak, (long long)ist_page_cap_peak,
            (long long)ist_put_page, (long long)ist_put_lit,
            (long long)ist_put_unaligned_heap);
    fprintf(stderr, "[rcidx] leaf pages live=%lld peak=%lld  "
            "page_span=%llu pages (min=%llu max=%llu)\n",
            (long long)ist_leaf_pages, (long long)ist_leaf_pages_peak,
            (unsigned long long)(ist_min_page == (uintptr_t)-1
                                     ? 0
                                     : ist_max_page - ist_min_page + 1),
            (unsigned long long)(ist_min_page == (uintptr_t)-1 ? 0 : ist_min_page),
            (unsigned long long)ist_max_page);
}

static void __attribute__((constructor(102))) rc_istats_init(void) {
    if (!getenv("PYADAMAS_RC_INDEX_STATS")) return;
    rc_istats_on = 1;
    atexit(rc_istats_dump);
}

/* ── 索引叶子的分页登记表（0.321）─────────────────────────────
 *
 * 字符串是索引叶子（PYADAMAS_RC_LEAF）：不进 GC 记账，但必须留在「这个
 * 机器字是不是堆对象」的注册表里 —— unknown 值的 rc_inc_unknown /
 * dec_unknown 靠它识别堆指针。
 *
 * **为什么不放主索引**：主索引是「每对象一个槽」的开放寻址哈希表，字符
 * 串密集脚本每分配/释放一次字符串就要探一次哈希（b1 实测 rc_index_put
 * 6.6% + rc_index_del 5.1%，是运行时最大的一块）。字符串地址高度聚集，
 * 用「页 + 位图」表示同样精确：4KB 页 → 256 个 16 字节槽 → 32 字节
 * 位图，整个工作集只有几十 KB（主表要几百 KB），put/del 退化成
 * 「页哈希 + 置/清一位」，命中判定不需要解引用对象。
 *
 * **精确性**（与主索引逐元素相同的集合，不是概率结构）：
 *   - 位图标记的是「这个 16 字节槽位上恰好有一个活着的字符串头」；
 *   - 堆对象头都来自 malloc（ABI 保证 16 字节对齐），两个堆块不会落在
 *     同一 16 字节槽里，所以位与对象一一对应；
 *   - 释放时清位，之后落在这个地址的整数（v = addr + 64）与主索引一样
 *     不会命中（主索引靠删除 + 墓碑保证同一件事）；
 *   - 8 字节对齐的静态驻留串（表项按 104 字节排布，一半落在 8 mod 16）
 *     无法用 16 字节槽表示，仍走主索引 —— 路由判据只依赖地址对齐与
 *     kind，put/del/has 三处一致。
 *
 * 开关：gc_leaf_mode（PYADAMAS_GC_CONSERVATIVE=1 时关闭，回到全跟踪 +
 * 全量主索引口径）。 */

#define LEAF_PAGE_SHIFT 12
#define LEAF_PAGE_BYTES ((uintptr_t)1 << LEAF_PAGE_SHIFT)
#define LEAF_PAGE_MASK (LEAF_PAGE_BYTES - 1)
#define LEAF_PAGE_SLOTS (LEAF_PAGE_BYTES / 16)        /* 256 个 16 字节槽 */
#define LEAF_PAGE_WORDS (LEAF_PAGE_SLOTS / 64)        /* 4 个 64 位字 */
#define LEAF_BUCKET_BITS 11
#define LEAF_BUCKETS ((size_t)1 << LEAF_BUCKET_BITS)
#define LEAF_BUCKET_MASK (LEAF_BUCKETS - 1)
/* 空页节点回收上限：字符串在页之间来回时避免反复 malloc/free。 */
#define LEAF_PAGE_CACHE_MAX 64

typedef struct PyAdamasLeafPage {
    struct PyAdamasLeafPage* next;
    uintptr_t base;                     /* 页基址（addr & ~4095） */
    int32_t live;                       /* 本页活着的字符串个数 */
    uint64_t bits[LEAF_PAGE_WORDS];     /* 16 字节槽位图 */
} PyAdamasLeafPage;

static PyAdamasLeafPage* leaf_page_tab[LEAF_BUCKETS];
static PyAdamasLeafPage* leaf_page_cache;
static int leaf_page_cache_count;

static inline size_t leaf_bucket(uintptr_t base) {
    uint64_t x = (uint64_t)(base >> LEAF_PAGE_SHIFT);
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 32;
    return (size_t)x & LEAF_BUCKET_MASK;
}

static PyAdamasLeafPage* leaf_page_new(void) {
    PyAdamasLeafPage* p = leaf_page_cache;
    if (p) {
        leaf_page_cache = p->next;
        leaf_page_cache_count--;
        return p;
    }
    p = (PyAdamasLeafPage*)malloc(sizeof(PyAdamasLeafPage));
    if (!p) panic_c("out of memory");
    return p;
}

static void leaf_page_release(PyAdamasLeafPage* p) {
    if (leaf_page_cache_count >= LEAF_PAGE_CACHE_MAX) {
        free(p);
        return;
    }
    p->next = leaf_page_cache;
    leaf_page_cache = p;
    leaf_page_cache_count++;
}

/* 字符串头 h 能不能用 16 字节槽位表示（见上面的精确性说明）。 */
static inline int leaf_slot_ok(uintptr_t h) { return (h & 15) == 0; }

static void leaf_index_put(PyAdamasRc* h) {
    uintptr_t a = (uintptr_t)h;
    uintptr_t base = a & ~LEAF_PAGE_MASK;
    size_t b = leaf_bucket(base);
    PyAdamasLeafPage* pg = leaf_page_tab[b];
    while (pg && pg->base != base) pg = pg->next;
    if (!pg) {
        pg = leaf_page_new();
        pg->base = base;
        pg->live = 0;
        memset(pg->bits, 0, sizeof pg->bits);
        pg->next = leaf_page_tab[b];
        leaf_page_tab[b] = pg;
        if (rc_istats_on && ++ist_leaf_pages > ist_leaf_pages_peak)
            ist_leaf_pages_peak = ist_leaf_pages;
    }
    size_t slot = (a & LEAF_PAGE_MASK) >> 4;
    uint64_t bit = (uint64_t)1 << (slot & 63);
    if (!(pg->bits[slot >> 6] & bit)) {
        pg->bits[slot >> 6] |= bit;
        pg->live++;
    }
}

static void leaf_index_del(PyAdamasRc* h) {
    uintptr_t a = (uintptr_t)h;
    uintptr_t base = a & ~LEAF_PAGE_MASK;
    size_t b = leaf_bucket(base);
    PyAdamasLeafPage** pp = &leaf_page_tab[b];
    PyAdamasLeafPage* pg = *pp;
    while (pg && pg->base != base) {
        pp = &pg->next;
        pg = pg->next;
    }
    if (!pg) return;
    size_t slot = (a & LEAF_PAGE_MASK) >> 4;
    uint64_t bit = (uint64_t)1 << (slot & 63);
    if (!(pg->bits[slot >> 6] & bit)) return;
    pg->bits[slot >> 6] &= ~bit;
    if (--pg->live == 0) {
        *pp = pg->next;
        leaf_page_release(pg);
        if (rc_istats_on) ist_leaf_pages--;
    }
}

/* 注意：h 可能是任意整数减过头的地址，**不能解引用**。 */
static int leaf_index_has(const PyAdamasRc* h) {
    uintptr_t a = (uintptr_t)h;
    uintptr_t base = a & ~LEAF_PAGE_MASK;
    PyAdamasLeafPage* pg = leaf_page_tab[leaf_bucket(base)];
    while (pg && pg->base != base) pg = pg->next;
    if (!pg) return 0;
    size_t slot = (a & LEAF_PAGE_MASK) >> 4;
    return (int)((pg->bits[slot >> 6] >> (slot & 63)) & 1);
}

/* 进程退出时还掉页节点（atexit：必须晚于 pa_rc_shutdown —— 它可能还会
   释放临时栈上的字符串，触发 leaf_index_del）。 */
static void leaf_index_shutdown(void) {
    for (size_t i = 0; i < LEAF_BUCKETS; ++i) {
        PyAdamasLeafPage* pg = leaf_page_tab[i];
        while (pg) {
            PyAdamasLeafPage* nx = pg->next;
            free(pg);
            pg = nx;
        }
        leaf_page_tab[i] = NULL;
    }
    while (leaf_page_cache) {
        PyAdamasLeafPage* p = leaf_page_cache;
        leaf_page_cache = p->next;
        free(p);
    }
    leaf_page_cache_count = 0;
}

/* ── 统一页位图索引（PYADAMAS_RC_INDEX=bitmap，默认实现）───────────
 *
 * **为什么换掉主哈希表**：perf 取证（pprint 基准）显示 rc_index_has 是最大
 * 热点，但代价不在哈希计算，而在**失败的线性探测**：字符串/浮点盒不在
 * 主表里（它们在叶子位图），每次查询却先探一遍主表 —— 表装载 67%、
 * 又因容器 churn 积满墓碑，失败探测平均 10.1 个槽（= 10 次随机 cache
 * line 读）。6.76M 次查询共 57M 次槽位读。
 *
 * **结构**：堆对象头一律来自 malloc（x86-64 保证 16 字节对齐），所以
 * 「是不是对象起点」可以按 16 字节槽位精确表示：4KB 页 = 256 槽 = 4 个
 * 64 位字。页表项 {base, live, bits[4]} 共 48 字节，**正好一条 cache line**，
 * 定位页表项用开放寻址（装载 <= 50%，无链、无 per-page malloc）：
 *
 *     has(h):  i = hash(h >> 12) & mask;  查表项 -> base 相等 -> 测一位
 *
 * 命中 1 次访存（旧实现：哈希 1 次 + 线性探测 2..10 次 + 位图 2 次）。
 * 表本身也小得多：每 16 字节 1 位（= 每 8 字节 1 位）+ 50% 空槽，
 * 相比「每对象 8 字节 + 43% 余量」，64 字节对象省 10 倍。
 *
 * **精确性**（与主索引逐元素相同，不是概率结构）：
 *   - 位标记「这个 16 字节槽位上恰好有一个活对象头」，清位在释放路径上；
 *   - 两个活堆块不可能落在同一 16 字节槽（malloc 16 字节对齐 + 块不重叠），
 *     所以位与对象一一对应，无假阳性；
 *   - put/del/has 三处用**同一个判据**（地址 & 15）路由，所以查询命中当且仅当
 *     对象在集合里，无假阴性；
 *   - 8 字节对齐的静态字面量（pa_register_literals 注册的 payload-1，奇数地址）
 *     进主哈希表 —— 那是哈希表**唯一**的居民，通常为空（cap==0 直接返回）。
 *
 * **生命周期置位/清位点**：rc_index_put 在 rc_link（对象首次可见）与
 * rc_alloc_leaf 分支调用；rc_index_del 在 rc_unlink / pa_rc_track 调用 ——
 * 与旧索引完全同一批调用点，没有新增钩子。
 *
 * 回退：PYADAMAS_RC_INDEX=hash 走旧实现（主哈希 + 字符串叶子位图），
 * 逐字节等价于改动前；两者集合相同，只有代价不同。 */

#define RC_PAGE_EMPTY ((uintptr_t)0)   /* 空槽（页基址不可能为 0） */
#define RC_PAGE_TOMB ((uintptr_t)1)    /* 墓碑：本页已无活对象 */

typedef struct {
    uintptr_t base;                  /* 页基址；0 空 / 1 墓碑 */
    int32_t live;                    /* 本页存活对象数 */
    uint32_t pad_;
    uint64_t bits[LEAF_PAGE_WORDS];  /* 16 字节槽位图（256 位） */
} RcPageEnt;

static RcPageEnt* rc_page_tab;
static size_t rc_page_cap;   /* 2 的幂 */
static size_t rc_page_fill;  /* 非空槽（活 + 墓碑） */
static size_t rc_page_live;  /* 活页数 */

static inline size_t rc_page_slot(uintptr_t base) {
    uint64_t x = (uint64_t)(base >> LEAF_PAGE_SHIFT);
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 32;
    return (size_t)x & (rc_page_cap - 1);
}

static void rc_page_rehash(size_t ncap) {
    RcPageEnt* old = rc_page_tab;
    size_t ocap = rc_page_cap;
    RcPageEnt* nt = (RcPageEnt*)calloc(ncap, sizeof(RcPageEnt));
    if (!nt) panic_c("out of memory");
    rc_page_tab = nt;
    rc_page_cap = ncap;
    rc_page_fill = 0;
    if (rc_istats_on && (size_t)ist_page_cap_peak < ncap)
        ist_page_cap_peak = (int64_t)ncap;
    for (size_t i = 0; i < ocap; ++i) {
        if (old[i].base == RC_PAGE_EMPTY || old[i].base == RC_PAGE_TOMB) continue;
        size_t s = rc_page_slot(old[i].base);
        while (rc_page_tab[s].base != RC_PAGE_EMPTY)
            s = (s + 1) & (ncap - 1);
        rc_page_tab[s] = old[i];
        rc_page_fill++;
    }
    free(old);
}

/* 取/建一个页表项；返回 NULL 表示只查不建（has 用）。 */
static inline RcPageEnt* rc_page_find(uintptr_t base) {
    size_t s = rc_page_slot(base);
    for (;;) {
        RcPageEnt* e = &rc_page_tab[s];
        if (e->base == base) return e;
        if (e->base == RC_PAGE_EMPTY) return NULL;
        s = (s + 1) & (rc_page_cap - 1);
    }
}

static inline RcPageEnt* rc_page_obtain(uintptr_t base) {
    size_t s = rc_page_slot(base);
    size_t tomb = (size_t)-1;
    for (;;) {
        RcPageEnt* e = &rc_page_tab[s];
        if (e->base == base) return e;
        if (e->base == RC_PAGE_EMPTY) break;
        if (e->base == RC_PAGE_TOMB && tomb == (size_t)-1) tomb = s;
        s = (s + 1) & (rc_page_cap - 1);
    }
    if (tomb != (size_t)-1) s = tomb;   /* 复用墓碑，不增加 fill */
    else rc_page_fill++;
    RcPageEnt* e = &rc_page_tab[s];
    e->base = base;
    e->live = 0;
    memset(e->bits, 0, sizeof e->bits);
    rc_page_live++;
    return e;
}

static void rc_page_put(PyAdamasRc* h) {
    uintptr_t a = (uintptr_t)h;
    uintptr_t base = a & ~LEAF_PAGE_MASK;
    if (rc_page_cap == 0) {
        rc_page_rehash(64);
    } else if ((rc_page_fill + 1) * 2 >= rc_page_cap) {
        // 活页少、墓碑多时原地压缩，否则翻倍（与主哈希同策略）。
        size_t ncap = (rc_page_live + 1) * 4 >= rc_page_cap ? rc_page_cap * 2
                                                           : rc_page_cap;
        rc_page_rehash(ncap);
    }
    RcPageEnt* e = rc_page_obtain(base);
    size_t slot = (a & LEAF_PAGE_MASK) >> 4;
    uint64_t bit = (uint64_t)1 << (slot & 63);
    if (!(e->bits[slot >> 6] & bit)) {
        e->bits[slot >> 6] |= bit;
        e->live++;
    }
    if (rc_istats_on && (size_t)ist_page_peak < rc_page_live)
        ist_page_peak = (int64_t)rc_page_live;
}

static void rc_page_del(PyAdamasRc* h) {
    if (rc_page_cap == 0) return;
    uintptr_t a = (uintptr_t)h;
    RcPageEnt* e = rc_page_find(a & ~LEAF_PAGE_MASK);
    if (!e) return;
    size_t slot = (a & LEAF_PAGE_MASK) >> 4;
    uint64_t bit = (uint64_t)1 << (slot & 63);
    if (!(e->bits[slot >> 6] & bit)) return;
    e->bits[slot >> 6] &= ~bit;
    if (--e->live == 0) {
        /* 页空了：位图清零、页表项转墓碑（从查找路径摘除）。清零是防御性
           的 —— 墓碑永远不会被 rc_page_find 返回，但地址复用后旧位留在
           表里是最难查的一类假阳性，干脆不留。 */
        memset(e->bits, 0, sizeof e->bits);
        e->base = RC_PAGE_TOMB;
        rc_page_live--;
    }
}

/* 注意：h 可能是任意整数减过头的地址，**不能解引用**。 */
static inline int rc_page_has(const PyAdamasRc* h) {
    if (rc_page_cap == 0) return 0;
    uintptr_t a = (uintptr_t)h;
    const RcPageEnt* e = rc_page_find(a & ~LEAF_PAGE_MASK);
    if (!e) return 0;
    size_t slot = (a & LEAF_PAGE_MASK) >> 4;
    return (int)((e->bits[slot >> 6] >> (slot & 63)) & 1);
}

/* 进程退出时还掉页表（atexit：必须晚于 pa_rc_shutdown —— 它可能还会
   释放临时栈上的对象，触发 rc_index_del）。 */
static void rc_page_shutdown(void) {
    free(rc_page_tab);
    rc_page_tab = NULL;
    rc_page_cap = rc_page_fill = rc_page_live = 0;
}

/* 索引实现选择：默认页位图，PYADAMAS_RC_INDEX=hash 回退旧实现。
   构造函数里读一次（早于 main 里的字面量注册）；静态初值就是默认值，
   所以即使构造函数没跑也自洽（put/has 看到同一个值）。 */
static int rc_index_bitmap = 1;

static void __attribute__((constructor(101))) rc_index_mode_init(void) {
    const char* m = getenv("PYADAMAS_RC_INDEX");
    if (m && m[0] && (m[0] == 'h' || m[0] == '0')) rc_index_bitmap = 0;
    /* 页表必须活到 pa_rc_shutdown 之后（它还会释放临时栈上的对象，
       触发 rc_index_del），所以用 atexit 而不是 shutdown 钩子。 */
    if (rc_index_bitmap) atexit(rc_page_shutdown);
}

/* ── 索引自检（PYADAMAS_RC_INDEX_SELFTEST=1）────────────────────
 * 入口 pa_gc_init 里跑一次，验收三条最容易埋雷的不变量：
 *   1. 批量登记后每个地址精确命中（无假阴性）；
 *   2. 全部撤销后**每个地址都不再命中**（地址复用不留假阳性），
 *      活页数与存活对象数回到测试前；
 *   3. 上述两条在**真实对象**（rc_alloc -> rc_link -> rc_unlink -> free）
 *      的完整链路上同样成立。
 * 失败直接 panic（CI 里跑套件就会红），成功打一行 PASS。 */
static void rc_selftest_dtor(void* p) { (void)p; }

static size_t rc_page_live_entries(void) {
    size_t n = 0;
    for (size_t i = 0; i < rc_page_cap; ++i) {
        uintptr_t b = rc_page_tab[i].base;
        if (b != RC_PAGE_EMPTY && b != RC_PAGE_TOMB) n++;
    }
    return n;
}

/* verbose = 0：静默（只 panic），1：打印一行 PASS 摘要（=2 时开启，
   因为套件会逐字比对程序 stderr，静默模式才能全量跑）。 */
static void rc_index_selftest(int verbose) {
    if (rc_index_off || rc_index_readonly) {
        if (verbose) fprintf(stderr, "[rcidx-selftest] skipped (index off)\n");
        return;
    }
    const size_t pages0 = rc_page_live;
    const size_t objs0 = rc_index_live;
    /* 1) 合成地址：16 页 × 128 槽。只当地址用，不 deref。仅位图模式 ——
       哈希模式的 put/del 要读 kind 做叶子路由，假地址会直接崩。 */
    enum { NSYNTH = 2048 };
    uintptr_t synth = (uintptr_t)0x6a6a00000000ull;
    int miss = 0, stale = 0;
    if (rc_index_bitmap) {
        for (int i = 0; i < NSYNTH; ++i)
            rc_index_put((PyAdamasRc*)(synth + (uintptr_t)i * 16));
        for (int i = 0; i < NSYNTH; ++i)
            if (!rc_index_has((PyAdamasRc*)(synth + (uintptr_t)i * 16))) miss++;
        for (int i = 0; i < NSYNTH; ++i)
            rc_index_del((PyAdamasRc*)(synth + (uintptr_t)i * 16));
        for (int i = 0; i < NSYNTH; ++i)
            if (rc_index_has((PyAdamasRc*)(synth + (uintptr_t)i * 16))) stale++;
    }
    /* 2) 真实对象：整条 RC 链（link/unlink + 分配器）。 */
    enum { NREAL = 512 };
    PyAdamasRc* objs[NREAL];
    for (int i = 0; i < NREAL; ++i)
        objs[i] = (PyAdamasRc*)rc_alloc(8, rc_selftest_dtor, NULL,
                                        PYADAMAS_RC_OBJ) - 1;
    int rmiss = 0;
    for (int i = 0; i < NREAL; ++i)
        if (!rc_index_has(objs[i])) rmiss++;
    for (int i = 0; i < NREAL; ++i)
        pa_rc_dec((char*)objs[i] + PYADAMAS_RC_HDR);
    int rstale = 0;
    for (int i = 0; i < NREAL; ++i)
        if (rc_index_has(objs[i])) rstale++;
    /* 3) 计数归零：页表项与存活对象都回到测试前。 */
    const size_t pages1 = rc_page_live;
    const size_t objs1 = rc_index_live;
    const int ok = (miss == 0 && stale == 0 && rmiss == 0 && rstale == 0 &&
                    pages1 == pages0 && objs1 == objs0);
    if (verbose >= 1)
    fprintf(stderr,
            "[rcidx-selftest] %s mode=%s synth(%d) miss=%d stale=%d  real(%d) "
            "miss=%d stale=%d  live_pages %zu->%zu  live_objs %zu->%zu  "
            "entries=%zu\n",
            ok ? "PASS" : "FAIL", rc_index_bitmap ? "bitmap" : "hash", NSYNTH,
            miss, stale, NREAL, rmiss, rstale, pages0, pages1, objs0, objs1,
            rc_page_live_entries());
    if (!ok) panic_c("rc_index self-test failed");
}

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

/* 注册表路由：索引叶子（16 字节对齐的 str/bytes）进分页位图，其余
   （容器 / 闭包 / 异常 / 8 字节对齐的静态驻留串…）进主索引。判据只依赖
   地址对齐 + kind，put/del 严格一致；has 不能读 kind（h 可能是整数减出来
   的地址），所以主索引未命中时无条件再查一次位图。 */
static inline int rc_index_is_leaf_obj(const PyAdamasRc* h) {
    return gc_leaf_mode &&
           (h->kind == PYADAMAS_RC_STR || h->kind == PYADAMAS_RC_CPYFLOAT) &&
           leaf_slot_ok((uintptr_t)h);
}

static void rc_hash_put(PyAdamasRc* h) {
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
    if (rc_istats_on) {
        if (rc_index_cap > (size_t)ist_peak_cap) ist_peak_cap = rc_index_cap;
        if (rc_index_live > (size_t)ist_peak_live) ist_peak_live = rc_index_live;
        if (rc_index_fill > (size_t)ist_peak_fill) ist_peak_fill = rc_index_fill;
    }
}

void rc_index_put(PyAdamasRc* h) {
    if (rc_index_off || rc_index_readonly) return;
    if (rc_istats_on) {
        ist_put++;
        uintptr_t pg = (uintptr_t)h >> 12;
        if (pg < ist_min_page) ist_min_page = pg;
        if (pg > ist_max_page) ist_max_page = pg;
    }
    /* **对齐路由**（新实现）：16 字节对齐的对象一律进页位图（见上面的
       结构说明）—— 那是对齐对象的全集，所以 put/del/has 三处可以只看
       对齐就决定去哪个结构。非对齐的只剩静态字面量（payload-1），进哈希。 */
    if (rc_index_bitmap) {
        if (((uintptr_t)h & 15) == 0) {
            if (rc_istats_on) ist_put_page++;
            rc_page_put(h);
        } else {
            /* 对齐保证见 rc_page_* 的说明：malloc 16B 对齐 + 64B 头，
               所以真堆对象的头一定 16 对齐。这里把「非对齐却非字面量」
               的登记记下来 —— 那是位图会漏判的唯一入口。 */
            if (rc_istats_on && h->rc == PYADAMAS_RC_IMMORTAL) ist_put_lit++;
            /* **硬断言**：非对齐的只能是永生静态串（BSS 表 / 字面量表）。
               真堆对象一旦走到这里，页位图就会漏判（假阴性 -> 漏回收），
               所以当场 panic 而不是静默记一笔。这条分支只对字面量执行。 */
            if (h->rc != PYADAMAS_RC_IMMORTAL) {
                if (rc_istats_on) ist_put_unaligned_heap++;
                panic_c("rc_index: 非 16 字节对齐的堆对象进入页位图索引");
            }
            rc_hash_put(h);
        }
        return;
    }
    /* 旧实现：索引叶子（str/float 盒）进分页位图，其余进主哈希。 */
    if (rc_index_is_leaf_obj(h)) {
        leaf_index_put(h);
        return;
    }
    rc_hash_put(h);
}

static void rc_hash_del(PyAdamasRc* h) {
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

void rc_index_del(PyAdamasRc* h) {
    if (rc_index_off || rc_index_readonly) return;
    if (rc_istats_on) ist_del++;
    if (rc_index_bitmap) {
        if (((uintptr_t)h & 15) == 0) rc_page_del(h);
        else rc_hash_del(h);
        return;
    }
    if (rc_index_is_leaf_obj(h)) {
        leaf_index_del(h);
        return;
    }
    rc_hash_del(h);
}

/* 主哈希探测（bitmap 模式下只剩静态字面量会走到这里）。 */
static int rc_hash_has(PyAdamasRc* h) {
    if (rc_index_cap == 0) return 0;
    size_t s = rc_index_slot(h);
    size_t n = 0;
    while (rc_index[s]) {
        n++;
        if (rc_index[s] == h) {
            if (rc_istats_on) {
                ist_hash_hit++;
                ist_probes += n;
                ist_probes_hit += n;
                ist_probe_hist[n < 9 ? n - 1 : 8]++;
            }
            return 1;
        }
        s = (s + 1) & (rc_index_cap - 1);
    }
    if (rc_istats_on) {
        ist_probes += n + 1;
        ist_probes_miss += n + 1;
        ist_probe_hist[n < 8 ? n : 8]++;
    }
    return 0;
}

int rc_index_has(PyAdamasRc* h) {
    if (rc_index_off) return 0;
    // RC_TOMB 是墓碑哨兵（(PyAdamasRc*)-1）。unknown 值 v 减去 64 字节头后
    // 可能正好等于它（v == 63），探测时不能把墓碑当成命中。
    if (h == RC_TOMB) return 0;
    const int al16 = ((uintptr_t)h & 15) == 0;
    if (rc_istats_on) {
        ist_has++;
        if (al16) ist_has_al16++;
        else ist_has_unal16++;
    }
    int r;
    if (rc_index_bitmap) {
        /* 对齐路由与 put/del 逐字一致：对齐对象在页位图里，非对齐在哈希。 */
        r = al16 ? rc_page_has(h) : rc_hash_has(h);
        if (rc_istats_on && r && al16) ist_page_hit++;
    } else {
        r = rc_hash_has(h);
        // 索引叶子（字符串）不在主索引里，位图是精确的第二半。
        if (!r && gc_leaf_mode) {
            if (rc_istats_on) ist_has_leafmode++;
            r = leaf_index_has(h);
            if (rc_istats_on && r) ist_leaf_hit++;
        }
    }
    if (rc_istats_on && !r) {
        ist_miss++;
        if (al16) ist_miss_al++;
        else ist_miss_unal++;
    }
    return r;
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
    // **廉价排除小整数，再做哈希探测。**
    //
    // unknown 值里绝大多数是标量（下标、长度、循环变量、布尔）。它们减去
    // RC 头之后落在极低地址，一条比较就能否掉，不必走 rc_index_has 的
    // 哈希 + 线性探测。
    //
    // 实测 bench_heapq：60 万次 inc_unknown，元素是 0-999 的整数，
    // 全部被这道短路拦下。
    if ((uint64_t)v < 65536) return 0;
    if (rc_stats_on) stat_rc_inc_unknown++;
    /* P3-1b（0.328）：与 dec_unknown 同一份判定（pa_unknown_is_heap，含
       exact-RC 回退）。原来只查 rc_index_has：exact-RC 模式下索引关闭，
       leaf（str/bytes/float 盒）的 inc 恒空操作而 dec 走回退会真 dec ——
       保留/释放不对称（stdlib-batch1 §四半移交的 leaf×unknown 洞）。
       常规模式行为逐字不变（索引命中即返回，未命中且索引开着同样否掉）。 */
    if (!pa_unknown_is_heap(v)) return 0;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
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
/* always_inline（0.302，0.285 同款教训）：共享探针 pa_unknown_is_heap_slow
   让它多了第二个调用点，gcc 把它从 dec_unknown 里 outline 成公共函数，
   大值 dec_unknown 每次多一层 call/ret。强制内联恢复紧形态。 */
static inline __attribute__((always_inline))
int unknown_is_heap(PyAdamasRc* h) {
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

/* 0.302：共享探针实体 —— 与 dec_unknown 的「该不该 dec」判定逐位一致
   （含 exact-RC 回退）。dict 的 saw_heap 用它判断「这个值销毁时是否值得
   RC 做工」：探针说 0 的值，dec_unknown 必然空操作，跳过链游安全。
   头文件里的内联包装先排除小整数，标量不进这个调用。 */
int pa_unknown_is_heap_slow(int64_t v) {
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    return unknown_is_heap(h);
}

void pa_rc_dec_unknown(int64_t v) {
    if (!v) return;
    // 同 inc_unknown：先廉价排除小整数（见那里的说明）。
    if ((uint64_t)v < 65536) return;
    if (rc_stats_on) stat_rc_dec_unknown++;
    if (!pa_unknown_is_heap(v)) return;
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
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

/* 分配一个「不可能成环」的对象：不进 GC 堆链，只由 RC 管理。
 *
 * 适用前提（由调用方保证）：对象没有任何堆字段，也不是可达环的一部分。
 * 例子：元素全是非堆值的元组 —— 元组不可变，元素类别又固定为 0，
 * 所以它永远不会有出边，谈不上环。
 *
 * 走这条路省掉 rc_link/rc_unlink、新生代登记；对象在 rc 归零时照常由
 * dtor 释放，GC 永远看不到它。
 *
 * 实现上直接按 UNTRACKED 状态分配（复用 freelist 与 GC 记账逻辑），
 * 不进堆链、不进 RC 索引、不进新生代 —— 与「先 link 再立刻 unlink」的
 * 旧实现观察态完全一致（对象存活期间本来就不在索引/链上），但省掉
 * 链插摘、索引 put/del（各一次哈希探测）、young add/del 的往返。
 * 旧实现每个元组出生还在索引里留一个墓碑，现在连墓碑都没有。 */
static void* rc_alloc_core(int64_t size, void (*dtor)(void*), void (*release)(void*),
                           int64_t kind, int64_t extra, int untracked, int leaf);

void* rc_alloc_untracked_ex(int64_t size, void (*dtor)(void*), void (*release)(void*),
                            int64_t kind, int64_t extra) {
    return rc_alloc_core(size, dtor, release, kind, extra, 1, 0);
}

/* 叶子形状但需要 unknown 探针识别的对象（dict[str, 标量]）：不进 GC
   堆链/新生代，但保留 rc_index 成员资格。见 rc_alloc_core 的 leaf 分支。 */
void* rc_alloc_leaf(int64_t size, void (*dtor)(void*), void (*release)(void*),
                    int64_t kind) {
    return rc_alloc_core(size, dtor, release, kind, 0, 0, 1);
}

void* rc_alloc_untracked(int64_t size, void (*dtor)(void*), void (*release)(void*),
                         int64_t kind) {
    return rc_alloc_untracked_ex(size, dtor, release, kind, 0);
}

void rc_link(PyAdamasRc* h) {
    if (rc_leaf_untracked(h)) {
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_UNTRACKED;
        return;
    }
    // 索引叶子（str/bytes）：不进链、不进新生代、不参与 GC 记账，但必须
    // 留在存活堆索引里（unknown 探针的识别依据）。见 PYADAMAS_RC_LEAF。
    if (gc_leaf_mode &&
        (h->kind == PYADAMAS_RC_STR || h->kind == PYADAMAS_RC_CPYFLOAT)) {
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_LEAF;
        if (__builtin_expect(rc_index_off == 0 && rc_index_readonly == 0, 1))
            rc_index_put(h);
        return;
    }
    h->prev = NULL;
    h->next = heap_head;
    if (heap_head) heap_head->prev = h;
    heap_head = h;
    gc_tracked_count++;
    if (h->kind == PYADAMAS_RC_OBJ || h->kind == PYADAMAS_RC_INSTANCE)
        gc_conservative_count++;
    // **内联判断 rc_index_off，避免热路径上的函数调用。**
    //
    // rc_index_put 内部第一件事就是 `if (rc_index_off || rc_index_readonly) return;`，
    // 但函数调用本身的代价（call/ret + 寄存器保存）还在。exact-RC 模式下
    // 索引全程关闭，这个调用纯属浪费 —— 实测 bench_rc_gen 有 65.6 万次分配，
    // rc_link 占 gprof 采样的 50%。
    if (__builtin_expect(rc_index_off == 0 && rc_index_readonly == 0, 1))
        rc_index_put(h);
    young_add(h);
    gc_alloc_black(h);
}

void rc_unlink(PyAdamasRc* h) {
    if (h->marked & PYADAMAS_RC_UNTRACKED) return;
    // 索引叶子不在堆链 / 新生代上，只需要撤销索引成员资格。
    if (h->marked & PYADAMAS_RC_LEAF) {
        rc_index_del(h);
        return;
    }
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

/* 0.303：把 untracked 对象补进 GC 堆链（kind0 小元组经 unknown 写入升级
   成 kind2 后有了堆出边，可能成环）。与 rc_alloc_core 的 tracked 分支同
   形态：清 UNTRACKED 位再 rc_link（堆链 + 索引 + 新生代）。young_add 不
   改 rc，gc_alloc_black 是占位空操作 —— 存活对象的引用计数不受影响。
   收集期间用户代码不运行，写入路径不会在 gc_phase 里调到这里；即使增量
   标记进行中，新入链对象不在本轮 gc_set 快照里，不会被清扫（与新生对象
   同一安全性论证，见 gc_alloc_black 注释）。已 tracked 时空操作。 */
void pa_rc_track(void* p) {
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (h->marked & PYADAMAS_RC_LEAF) {
        // 叶子形状的 dict 被写进堆值 / own 升级：先从索引里摘掉，再按
        // tracked 重新入链（rc_link 会重新 rc_index_put）。
        rc_index_del(h);
        h->marked = 0;
        rc_link(h);
        return;
    }
    if (!(h->marked & PYADAMAS_RC_UNTRACKED)) return;
    h->marked = 0;
    rc_link(h);
}

void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind) {
    return rc_alloc_core(size, dtor, release, kind, 0, 0, 0);
}

void* rc_alloc_ex(int64_t size, void (*dtor)(void*), void (*release)(void*),
                      int64_t kind, int64_t extra) {
    return rc_alloc_core(size, dtor, release, kind, extra, 0, 0);
}

static void* rc_alloc_core(int64_t size, void (*dtor)(void*), void (*release)(void*),
                           int64_t kind, int64_t extra, int untracked, int leaf) {
    if (size < 0) panic_c("negative allocation size");
    // **热路径优化：把 GC 相关判断合并成一个全局标志。**
    //
    // 原来这里要读 gc_ready、heap_head、gc_incr_remaining、alloc_since_major、
    // alloc_since_gc、major_threshold、young_threshold —— 7 个全局变量。
    // bench_rc_gen 有 65 万次分配，这就是 455 万次内存访问。
    //
    // 合并成 gc_need_check：值为 0 时直接跳过整块（绝大多数分配的路径），
    // 非 0 时才进慢路径做完整判断。这个标志在「增量有待办」或「分配量接近
    // 阈值」时置 1，其余时候保持 0。
    if (__builtin_expect(gc_need_check, 0) && gc_ready && heap_head) {
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
            else gc_need_check = 0;
            // 无动作就清标志：唯一的旧清除点是 gc_incr_finish，小堆同步
            // 收集从不走增量 → 标志首次置 1 后永久滞留，每次分配都白跑
            // 检查块（也挡住生成代码的元组内联快路径）。清掉是安全的：
            // 记账在每次被跟踪分配接近 7/8 阈值时重新置 1，增量启动也会
            // 置 1，触发时机不变。
        }
    }
    // 短字符串按 8 字节取整，freelist 里的块容量和档位严格对应。
    // extra 是尾部内联缓冲（小列表合并分配），计入块容量与 GC 记账。
    int64_t alloc_size = size + extra;
    if (kind == PYADAMAS_RC_STR && size > 0 && size <= STR_POOL_MAX_SIZE)
        alloc_size = (size + 7) & ~(int64_t)7;
    PyAdamasRc* h = NULL;
#ifndef PYADAMAS_ALLOC_DEBUG
    /* 调试版（canary/guard）不复用池块：guard 的块是 mmap 的、不能进
       freelist；canary 版统一走 malloc 路径让每个块的 canary 位置一致。 */
    if (kind == PYADAMAS_RC_STR) h = str_pool_get(alloc_size);
    else if (kind == PYADAMAS_RC_CPYFLOAT && size == (int64_t)sizeof(int64_t))
        h = float_box_pool_get();
    else if (kind == PYADAMAS_RC_LIST && size == (int64_t)sizeof(PyAdamasList))
        h = extra == PYADAMAS_LIST_INLINE_BYTES ? list_inline_pool_get() : list_pool_get();
    else if (kind == PYADAMAS_RC_DICT && size == (int64_t)sizeof(PyAdamasDict))
        h = dict_pool_get();
#endif
    const int pooled = h != NULL;
    if (!h) {
        h = (PyAdamasRc*)PA_DBG_MALLOC(alloc_size);
        if (!h) panic_c("out of memory");
        h->size = alloc_size;
        h->dtor = dtor;
        h->release = release;
        h->kind = kind;
        /* 调试版：mode 2 在 canary 区尾部记录 size/kind 期望值。
           默认 rt.o 里这个宏展开成空操作。 */
        PA_DBG_HEAD_INIT(h);
    }
    // **池命中不重写 dtor/release/kind/size**：每个池按 kind 路由
    // （dec_slow / gc_free_body），同池块的这四个字段恒定 ——
    //   str_pool:        STR / NULL / NULL（str 分配点全在 rt_core，恒 NULL）
    //   list_pool(48):   LIST / list_destroy / list_gc_release
    //   list_inline(112):同上
    //   dict_pool:       DICT / dict_destroy / dict_gc_release
    // 运行期没有任何代码改写活对象的这四个头字段（已 grep 核实），
    // GC 清扫直接 free 不回池，所以复用块带着上一世的正确值。
    // 新增池化分配点必须维持「同池同 dtor/release/kind」不变量。
    h->rc = 1;
    if (untracked) {
        // 调用方保证无出边、不成环：直接置 UNTRACKED，跳过 link/unlink
        // 往返（见 rc_alloc_untracked_ex 的说明）。
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_UNTRACKED;
    } else if (kind == PYADAMAS_RC_STR && gc_untracked_leaves) {
        // exact-RC 默认模式：str 是叶子（与 rc_leaf_untracked 的判定一致），
        // 直接置 UNTRACKED。rc_link 里必然走到同一分支，这里内联掉那次
        // 函数调用 —— 40_map / str(int) / repr / format 分配字符串数千次。
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_UNTRACKED;
    } else if (leaf && gc_leaf_mode) {
        // 调用方保证出边全是叶子/标量（dict[str, 标量]）：不可能成环，
        // 不进堆链/新生代；但要留在 rc_index 里，unknown 域的探针靠它
        // 识别「这是堆对象」。rc_alloc_leaf 的调用方负责在形状变化
        // （own 升级 / 装堆值）时用 pa_rc_track 重新入链。
        h->prev = NULL;
        h->next = NULL;
        h->marked = PYADAMAS_RC_LEAF;
        if (__builtin_expect(rc_index_off == 0 && rc_index_readonly == 0, 1))
            rc_index_put(h);
    } else {
        h->marked = 0;
        rc_link(h);
    }
    // 叶子对象不可能成环，exact-RC 模式下也不进链，不必驱动 GC 频率。
    // **分配计数只在 GC 启用时累加。**
    //
    // 原来这里无条件累加两个全局变量（每次分配 2 次内存读 + 2 次写 + 1 次
    // rc_leaf_untracked 调用）。实测 bench_rc_gen 有 65 万次分配，这块占
    // rc_alloc 总耗时的显著比例。
    //
    // gc_ready 为 0 时（GC 禁用）这些计数根本不会有人读 —— 直接跳过。
    //
    // **untracked 对象不入 GC 记账**：调用方保证无出边、不成环，生死全由
    // RC 管理，GC 永远收不到它们。让它们驱动 GC 频率纯属浪费 —— queue_sim
    // 实测 gc_runs=231 而 gc_freed=0，全是元组churn 触发的空收集。排除后
    // GC 只由被跟踪对象的分配量驱动（环只可能出现在被跟踪对象之间）。
    // 入口的 gc_need_check 检查块不受影响：untracked 分配点仍是 mark
    // assist / 收集的触发点，只是不再累加阈值计数。
    // 叶子（exact-RC 的 UNTRACKED / 非 exact 的 LEAF）都不驱动 GC 频率：
    // 它们不可能成环，GC 永远收不到，计数它们只会制造空收集。
    if (gc_ready && !untracked &&
        !(h->marked & (PYADAMAS_RC_UNTRACKED | PYADAMAS_RC_LEAF))) {
        alloc_since_gc += alloc_size + PYADAMAS_RC_HDR;
        alloc_since_major += alloc_size + PYADAMAS_RC_HDR;
        // 接近阈值时打开快速判断标志。用 7/8 作为提前量：真正的阈值比较
        // 只在最后 1/8 的分配上做。
        if (alloc_since_major >= (major_threshold - major_threshold / 8) ||
            alloc_since_gc >= (young_threshold - young_threshold / 8))
            gc_need_check = 1;
    }
    void* p = (char*)h + sizeof(PyAdamasRc);
    // LIST 不清零：三个构造点（pa_list_new / pa_list_new_filled /
    // pa_tuple_new_filled）都显式写满全部 6 个字段；data 缓冲只读
    // [0, len)，append/filled 先写后读，池块里的陈旧内容永远不会被
    // 读到。省掉每次列表/元组分配的一次 memset（queue_sim 热路径）。
    // STR 同样不清零：str_alloc_raw 显式写 pub/len/chash/data，
    // SSO 缓冲写到 data[len]（含结尾 NUL），之后只按 len 读；堆串路径
    // 还额外写 hash。短串分配在 str(int)/repr/format 上极热，省掉一次
    // memset@plt 调用。
    // DICT 也不清零：pa_dict_new 会写满全部字段，这里只需保证 entries
    // 为 NULL（新块/无缓冲池命中时它据此走 malloc 分支）。
    if (kind != PYADAMAS_RC_LIST && kind != PYADAMAS_RC_STR &&
        kind != PYADAMAS_RC_DICT)
        memset(p, 0, (size_t)alloc_size);
    // dict 池命中：把寄存的缓冲写回 payload。entries 非空是 pa_dict_new
    // 识别「带缓冲池命中」的判据；否则显式置空。
    if (kind == PYADAMAS_RC_DICT && pooled && dict_last_got.entries) {
        PyAdamasDict* d = (PyAdamasDict*)p;
        d->entries = dict_last_got.entries;
        d->order = dict_last_got.order;
        d->index = dict_last_got.index;
        d->cap = dict_last_got.cap;
        d->icap = dict_last_got.icap;
    } else if (kind == PYADAMAS_RC_DICT) {
        ((PyAdamasDict*)p)->entries = NULL;
    }
    // size 用调用方请求的字节数（不含 RC 头），和 ASan 报的口径一致，
    // 方便两边对照。alloc_size 是池化后的容量，会偏大。
    //
    // 诊断钩子放在这里、而不是 pa_rc_inc/dec：那两个函数会被内联到极热
    // 路径，多一次调用就改变寄存器分配（实测把 88_defaultdict 的 1.5 算成
    // 垃圾值）。rc_alloc 本来就要走 malloc/池，多一次调用无影响。
    // **热路径内联判断，避免每次分配都进 pa_diag_rc_alloc。**
    //
    // 原来无条件调用 pa_diag_rc_alloc，它内部再调 diag_rc_enabled() 读
    // 环境变量并判断。gprof 实测：bench_rc_gen 有 65.6 万次分配，
    // diag_rc_enabled 被调用 130.6 万次（alloc + free 各一次）。
    //
    // pa_diag_rc_on 是公开变量（-1 = 未初始化，0 = 关，1 = 开）。
    // 这里一次比较就能跳过，省掉函数调用 + 内部判断。 */
    if (__builtin_expect(pa_diag_rc_on != 0, 0)) pa_diag_rc_alloc(p, size, (int32_t)kind);
    return p;
}

// unknown 值的 print：先查堆探针判断是不是堆对象，命中就按真实类型
// 分派，否则按整数。探针与 dec_unknown 同一份判定（含 exact-RC 回退）。
// str 原样输出；CPython 桥的 float 盒 / list / dict 走 pa_print_cpy_value
// （rt_cpy.c）；int/bool 在 unknown 里分不开，保持按整数输出。
void pa_print_any(int64_t v) {
    if (pa_unknown_is_heap(v)) {
        PyAdamasRc* h = (PyAdamasRc*)((uintptr_t)v - (uintptr_t)PYADAMAS_RC_HDR);
        if (h->kind == PYADAMAS_RC_STR) {
            pa_print_str((PyAdamasStr*)(uintptr_t)v);
            return;
        }
        if (h->kind == PYADAMAS_RC_BIGINT) {
            PyAdamasStr* s = pa_big_str((PyAdamasBig*)(uintptr_t)v);
            pa_print_str(s);
            pa_rc_dec(s);
            return;
        }
        if (h->kind == PYADAMAS_RC_CPYFLOAT || h->kind == PYADAMAS_RC_LIST ||
            h->kind == PYADAMAS_RC_DICT) {
            pa_print_cpy_value(v);
            return;
        }
        /* 动态 JSON 的 null/bool 盒：print 按 Python 值语义印。 */
        if (h->kind == PYADAMAS_RC_JSONNULL) {
            fputs("None", stdout);
            return;
        }
        if (h->kind == PYADAMAS_RC_JSONBOOL) {
            fputs(*(int64_t*)(uintptr_t)v ? "True" : "False", stdout);
            return;
        }
        /* P4（lang_gaps）：print(类对象) 按 <class '...'> 印（CPython 口径）。 */
        if (pa_is_class_obj((void*)(uintptr_t)v)) {
            PyAdamasStr* s = pa_class_repr((void*)(uintptr_t)v);
            pa_print_str(s);
            pa_rc_dec(s);
            return;
        }
        /* P4 块3（lang_gaps 门控开）：print(异常值) 印消息（CPython 口径）。 */
        if (pa_lang_gaps_rt && h->kind == PYADAMAS_RC_OBJ &&
            ((PyAdamasExcValue*)(uintptr_t)v)->magic == PYADAMAS_EXCVAL_MAGIC) {
            PyAdamasStr* s = pa_exc_value_str((PyAdamasExcValue*)(uintptr_t)v);
            pa_print_str(s);
            pa_rc_dec(s);
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
        if (__builtin_expect(pa_diag_rc_on != 0, 0)) pa_diag_rc_free(p, (int32_t)h->kind);
        // **untracked 小元组释放快路径**：kind 0 + 无位图 + 内联缓冲时
        // list_destroy 是空操作（无元素可 dec、无位图可 free、无外部
        // 缓冲可还），整块直接挂回内联池，省一次函数调用 + 三个分支。
        // 条件不满足（增长过 / 混合种类）照走 dtor 原路径。
        if (h->kind == PYADAMAS_RC_LIST && (h->marked & PYADAMAS_RC_UNTRACKED) &&
            h->size == (int64_t)sizeof(PyAdamasList) + PYADAMAS_LIST_INLINE_BYTES) {
            PyAdamasList* l = (PyAdamasList*)p;
            if (l->elem_kind == 0 && !l->heap_bits && l->data == list_inline_at(l)) {
                list_inline_pool_put(h);
                return;
            }
        }
        // **untracked leaf 不在堆链上，跳过 rc_unlink 的函数调用。**
        //
        // rc_unlink 的第一行就是 `if (h->marked & UNTRACKED) return;`，
        // 但函数调用本身（call/ret + 寄存器保存）已经付了。exact-RC 模式下
        // 字符串/range/struct 全是 untracked leaf —— bench_rc_gen 的 65 万次
        // 释放里绝大多数是字符串，每次省一次 61 条指令的函数调用。
        if (!(h->marked & PYADAMAS_RC_UNTRACKED)) rc_unlink(h);
        if (h->dtor) h->dtor(p);
        if (h->kind == PYADAMAS_RC_STR) str_pool_put(h);
        else if (h->kind == PYADAMAS_RC_CPYFLOAT) float_box_pool_put(h);
        else if (h->kind == PYADAMAS_RC_LIST && h->size == (int64_t)sizeof(PyAdamasList))
            list_pool_put(h);
        else if (h->kind == PYADAMAS_RC_LIST &&
                 h->size == (int64_t)sizeof(PyAdamasList) + PYADAMAS_LIST_INLINE_BYTES)
            list_inline_pool_put(h);
        else if (h->kind == PYADAMAS_RC_DICT && h->size == (int64_t)sizeof(PyAdamasDict))
            dict_pool_put(h);
        else PA_DBG_FREE(h);
    }
}

/* kind-1 容器元素的批量释放：元素全是堆指针（静态类型保证），把
   pa_rc_dec_fast 的判断序列和 untracked 小元组的释放快路径内联进循环，
   模式开关（统计 / 收集期 / 诊断）提到循环外 —— 任一开启就逐元素走
   pa_rc_dec 原路径，语义与旧实现逐位一致。queue_sim 的 pending 列表
   每步销毁都要 dec 一批 rc==1 的元组，原来每个元素两层调用
  （pa_rc_dec_fast 内联 + pa_rc_dec_slow），现在常见情形零调用。 */
void pa_rc_dec_heap_batch(int64_t* vals, int64_t n) {
    if (__builtin_expect(pa_rc_stats_on_fast != 0 || pa_gc_phase_fast != 0 ||
                         pa_diag_rc_on != 0, 0)) {
        for (int64_t i = 0; i < n; ++i) pa_rc_dec((void*)(uintptr_t)vals[i]);
        return;
    }
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = vals[i];
        if (!v) continue;
        PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)v - 1;
        if (h->rc == PYADAMAS_RC_IMMORTAL) continue;
        if (__builtin_expect(h->rc > 1, 1)) {
            h->rc--;
            continue;
        }
        if (h->rc <= 0) panic_c("refcount decrement on a freed object");
        // rc == 1：untracked 小元组直接挂回内联池（条件与 dec_slow 一致）。
        if (h->kind == PYADAMAS_RC_LIST && (h->marked & PYADAMAS_RC_UNTRACKED) &&
            h->size == (int64_t)sizeof(PyAdamasList) + PYADAMAS_LIST_INLINE_BYTES) {
            PyAdamasList* t = (PyAdamasList*)(h + 1);
            if (t->elem_kind == 0 && !t->heap_bits && t->data == list_inline_at(t)) {
                // 必须归零再入池：池块的不变量是 rc==0（dec_slow 的
                // 「dec on freed」panic 靠它挡二次释放；留着 rc==1 会让
                //  stray dec 静默二次 free → 池里出现重复块 → 两个活对象
                // 共享一块内存）。0.274 引入时漏了这一步。
                h->rc = 0;
                list_inline_pool_put(h);
                continue;
            }
        }
        pa_rc_dec_slow((void*)(uintptr_t)v);
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
    // **快速返回：栈水位没变时什么都不用做。**
    //
    // 实测 bench_rc_gen：cleanup 被调用 65.6 万次，但 temp 只 push 了 55 万次
    // —— 至少 10 万次 cleanup 是空操作（`temp_len == mark`）。
    // 原来这些也要走边界检查 + while 判断；提到最前面一次比较就能返回。
    //
    // 用 unsigned 比较同时覆盖 mark < 0（转成巨大值）和 mark > temp_len 两种
    // 非法情况 —— 但为了保留原来的 panic 语义，正常路径只比较相等。
    if (__builtin_expect((uint64_t)mark == (uint64_t)temp_len, 0)) return;
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
    // 0.286：永生值从不入栈（pa_rc_temp 跳过），无可取 —— 省掉整条
    // 栈扫描。str_build 的字符串 take 原来每次白扫一遍。
    PyAdamasRc* hh = (PyAdamasRc*)p - 1;
    if (hh->rc == PYADAMAS_RC_IMMORTAL) return;
    // **栈顶快速路径**：最常见的模式是「刚 push 就 take」（容器构造时
    // 每个元素都这样），这时 p 就在栈顶，一次比较就能命中。
    //
    // 实测 bench_rc_gen：30 万次 take 共 75 万步，平均 2.5 步 —— 说明
    // 有相当一部分不在栈顶。拆出栈顶检查后，那部分仍走原循环，但栈顶
    // 命中的不用进循环（省掉循环变量初始化、边界判断）。
    if (temp_len > 0 && temp_stack[temp_len - 1] == p) {
        --temp_len;
        if (rc_stats_on) stat_take_steps++;
        return;
    }
    for (int64_t i = temp_len - 2; i >= 0; --i) {
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
    /* 与 inc/dec_unknown 同一份判定（P3-1b）：exact-RC 模式下 leaf 也要
       能入临时栈，否则产生者那份引用无人接管（同族所有权洞）。 */
    if (!pa_unknown_is_heap(v)) return;
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
/* 数 UTF-8 码点数。
 *
 * 原实现逐字节判断「是否续字节（0b10xxxxxx）」，n 次循环 + n 次分支。
 * 实测这是 `text.split()` 的主要开销：text 有 880 字节、拆成 160 个词，
 * 每个词都调一次 str_new → str_seal → 这里，累计扫 880 次。
 *
 * **本地化优化：先按机器字批量判断「整块是否全 ASCII」。**
 * 纯 ASCII 时码点数 == 字节数，直接跳过逐字节扫描。英文文本绝大多数
 * 是这种情况（基准里 text.split() 的每个词都是）。
 *
 * 判断方法：取 8 字节，检查最高位是否全为 0。`(w & 0x8080808080808080) == 0`
 * 一次位运算就能排除 8 个字节。命中后整块直接算 8 个码点。 */
int64_t str_count_cp(const char* p, int64_t n) {
    int64_t k = 0;
    int64_t i = 0;
    // 8 字节批量：整块全 ASCII 时直接 +8。
    while (i + 8 <= n) {
        uint64_t w;
        memcpy(&w, p + i, sizeof w);
        if (w & 0x8080808080808080ULL) break;   /* 有非 ASCII，退回逐字节 */
        k += 8;
        i += 8;
    }
    // 剩余（或遇到非 ASCII 之后）逐字节处理。
    for (; i < n; ++i)
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

/* 非 ASCII 串的懒码点边界表：ix[k] = 第 k 个码点的起始字节偏移，ix[pub] = len。
   第一次随机下标 / 切片时建一次 O(len)，之后 O(1)。返回 NULL 表示不可缓存，
   调用方退回逐字节扫描：
     - SSO 短串：hash 字段就是字符区，没有地方放指针（本来也只需扫 <=7 字节）；
     - 只读字面量（rc == IMMORTAL）：.data.rel.ro 不可写；
     - 超过 STR_CP_INDEX_MAX 的超长串：索引（8 字节/码点）可能比串本身大很多。
   索引指针挂在 PyAdamasStr::hash —— 运行期创建的串该字段恒为 0，且全仓库
   没有读取方（字典哈希走的是 chash）。 */
#define STR_CP_INDEX_MAX (64 * 1024)

static const int64_t* str_cp_index(PyAdamasStr* s) {
    if (s->data == (const char*)&s->hash) return NULL;
    PyAdamasRc* h = (PyAdamasRc*)((char*)s - PYADAMAS_RC_HDR);
    if (h->rc == PYADAMAS_RC_IMMORTAL || h->kind != PYADAMAS_RC_STR) return NULL;
    if (s->hash) return (const int64_t*)(uintptr_t)s->hash;
    if (s->len > STR_CP_INDEX_MAX) return NULL;
    int64_t n = s->pub;
    int64_t* ix = (int64_t*)malloc((size_t)(n + 1) * sizeof(int64_t));
    if (!ix) return NULL;
    /* 与 str_cp_to_byte 的扫描语义逐字节一致：非法 UTF-8 时 seq_len 可能
       越过 len，剩余项统一填末尾偏移。 */
    int64_t off = 0;
    int64_t k = 0;
    while (k < n && off < s->len) {
        ix[k++] = off;
        off += str_seq_len((unsigned char)s->data[off]);
    }
    for (; k <= n; ++k) ix[k] = off;
    s->hash = (int64_t)(uintptr_t)ix;
    return ix;
}

/* 码点下标 cp -> 字节偏移（cp == pub 时返回 len）。 */
int64_t str_cp_to_byte(const PyAdamasStr* s, int64_t cp) {
    if (s->pub == s->len) return cp;  /* 全 ASCII：码点下标就是字节偏移 */
    const int64_t* ix = str_cp_index((PyAdamasStr*)(uintptr_t)s);
    if (ix && cp >= 0 && cp <= s->pub) return ix[cp];
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
        s->chash = 0;
        s->data = (const char*)&s->hash;
        ((char*)s->data)[len] = '\0';
        return s;
    }
    PyAdamasStr* s =
        (PyAdamasStr*)rc_alloc((int64_t)sizeof(PyAdamasStr) + len + 1, NULL, NULL, PYADAMAS_RC_STR);
    s->len = len;
    s->pub = -1;
    s->hash = 0;
    s->chash = 0;
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

/* 驻留表的**载荷指针**数组（256 项），供生成代码直接索引。
 *
 * 单字节 ASCII 字符串字面量会被 codegen 直接映射到这里 —— 于是
 * `s[i] == "0"` 两边是同一个对象，pa_str_eq 的第一行 `a == b` 直接命中，
 * 连长度检查和字节比较都不用做。这也和 CPython 的 latin1 驻留行为一致。
 *
 * 生成代码在 main 的序言里调 pa_static_strs_init() 保证这张表已填好，
 * 所以函数体里不需要再判 static_strs_ready。 */
PyAdamasStr* pa_char_tab[256];

/* 非 ASCII 单码点串的惰性驻留表（BMP，0x80-0xFFFF）。字符串不可变，
   和 ASCII 的 char_tab 一样做成永生对象：CJK 文本的下标/迭代每轮都要
   取单字符，原来每次 malloc + memcpy + RC 记账，是 52_index_cjk 的主要
   成本。表是 BSS，只有实际用到的页才会被映射。 */
static PyAdamasStr* cp_str_cache[0x10000];

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
        pa_char_tab[i] = &char_tab[i].s;
    }
    static_str_init(&kw_true_s, "True", 4, 4);
    static_str_init(&kw_false_s, "False", 5, 5);
    static_str_init(&kw_none_s, "None", 4, 4);
    static_strs_ready = 1;
}

static void static_strs_ensure(void) {
    if (!static_strs_ready) static_strs_init();
}

/* 生成代码在 main 序言里调用一次，保证 pa_char_tab 在函数体执行前可用。 */
void pa_static_strs_init(void) { static_strs_ensure(); }

/* 单码点串：ASCII 走 256 项驻留表（零分配），其余码点现编 1-4 字节。
   0x80-0xFF 不再是合法的单字节 str，ASCII 之外的码点必须走 pa_cp_str。 */
PyAdamasStr* pa_char_str(int64_t c) {
    static_strs_ensure();
    return &char_tab[(int)(c & 0xff)].s;
}

PyAdamasStr* pa_cp_str(uint32_t cp) {
    if (cp < 0x80) return pa_char_str((int64_t)cp);
    if (cp < 0x10000) {
        PyAdamasStr* hit = cp_str_cache[cp];
        if (hit) return hit;
    }
    char buf[4];
    int n = str_cp_encode(cp, buf);
    if (cp < 0x10000) {
        /* 首次遇到：分配一个永生对象并登记进 RC 索引（unknown 探针要靠它
           识别类型），之后整个进程生命周期内复用。 */
        PyAdamasStaticStr* e = (PyAdamasStaticStr*)malloc(sizeof *e);
        if (!e) panic_c("out of memory");
        static_str_init(e, buf, n, 1);
        cp_str_cache[cp] = &e->s;
        return &e->s;
    }
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

/* 从指定帧链头做完整扫描（临时区位图 + 槽）。vthread 挂起栈的精确根
   扫描复用这一份逻辑（rt_vthread.c 的 pa_vt_gc_scan 逐 VT 调用）。 */
void pa_gc_scan_frames_from(PyAdamasFrame* top) {
    for (PyAdamasFrame* f = top; f; f = f->prev) {
        if (f->scan_words > 0 && f->scan_base) {
            if (f->scan_mask && !gc_conservative)
                gc_scan_masked(f->scan_base, f->scan_mask, f->scan_words);
            else
                gc_scan_range(f->scan_base,
                              (char*)f->scan_base +
                                  sizeof(uintptr_t) * (size_t)f->scan_words);
        }
        for (int64_t i = 0; i < f->n; ++i) {
            uintptr_t addr = (uintptr_t)f->slots[i];
            int64_t v = *(int64_t*)(addr & ~(uintptr_t)3);
            if (!v) continue;
            if (addr & 3) gc_mark((uintptr_t)v);
            else gc_mark_direct((uintptr_t)v);
        }
    }
}

/* 保守标记一个机器字（vthread temp 栈扫描用）。 */
void pa_gc_mark_word(uintptr_t v) { gc_mark(v); }

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
        case PYADAMAS_RC_BUFFER:  // 0.536：连续内存，元素只有标量，无出边
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
    /* 增量标记总开关（0.330）：=1 启用分片标记 + 删除/插入屏障；默认 0
       同步标记（现状）。只读 "1"，"0"/未设都关，flag-off 零变化。 */
    { const char* im = getenv("PYADAMAS_GC_INCR_MARK"); if (im && im[0] == '1') gc_incr_mark_on = 1; }
    pa_gc_incr_mark_fast = gc_incr_mark_on;
    // 索引叶子模式：保守回退关掉（字符串回到旧的全跟踪口径）。
    gc_leaf_mode = !gc_conservative;
    if (gc_leaf_mode) atexit(leaf_index_shutdown);
    /* 索引自检：PYADAMAS_RC_INDEX_SELFTEST=1 静默跑（失败 panic），
       =2 额外打印一行 PASS 摘要。放在建根之前，此时堆是空的。 */
    { const char* stenv = getenv("PYADAMAS_RC_INDEX_SELFTEST");
      if (stenv) rc_index_selftest(stenv[0] == '2'); }
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
    // **非 trial/restore 阶段一律清零。**
    //
    // 原来只在 sweep 阶段清槽：普通销毁路径（协程异常收尾 + 正常收尾都跑
    // gcrel）上槽里会留一个已释放的指针，第二次释放就撞野指针 ——
    // `refcount decrement on a freed object`。
    //
    // 不能在 trial/restore 阶段清：那时只是试算计数，帧的内容之后还要用。
    if (gc_phase != GC_PHASE_TRIAL && gc_phase != GC_PHASE_RESTORE) *slot = 0;
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
    gc_need_check = 0;   /* 本轮增量做完，清标志 */
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
    double _t0 = gc_stats_on ? gc_now_ms() : 0.0;
    if (gc_stats_on) ++gc_incr_calls;
    // 本轮做完后重新装填，直到整个清扫完成。
    gc_incr_remaining = GC_INCR_STRIDE;
    gc_need_check = 1;   /* 增量有待办 */
    int budget = GC_MARK_BUDGET;
    if (gc_incr_state == 1) {
        while (gc_work_len && budget-- > 0) { gc_scan_object(gc_work[--gc_work_len]); if (gc_stats_on) ++gc_incr_objs; }
        if (gc_work_len == 0) {
            /* **标记排空 → 同步清扫**（GC 阶段1）。M=1 单线程：本次 gc_incr_step
               调用内 mutator 不运行，清扫是原子的。阶段1 只把昂贵的标记分片
               （~1.5ms 停顿摊薄），清扫保持原子 —— 避免「清扫切片间 mutator 把
               已标记活对象 rc 减到 0、收尾再误释」的逻辑时序坑（实测 174 漏
               8816B 即此）。先 release 交出垃圾→活边（mark_sweep 口径），再
               gc_free_body，最后 gc_incr_finish 收尾释放 DEFER 保活对象。
               清扫的进一步分片（与 mutator 交错）留待阶段2/3 线程安全边界。 */
            gc_phase = GC_PHASE_SWEEP;
            gc_sweeping = 1;
            for (size_t i = 0; i < gc_set_len; ++i) {
                PyAdamasRc* h = gc_set[i];
                if (h && !(h->marked & PYADAMAS_RC_MARK) && h->release)
                    h->release((char*)h + PYADAMAS_RC_HDR);
            }
            for (size_t i = 0; i < gc_set_len; ++i) {
                PyAdamasRc* h = gc_set[i];
                if (!h) continue;
                /* IMMORTAL 永不回收。 */
                if (h->rc == PYADAMAS_RC_IMMORTAL) continue;
                if (!(h->marked & PYADAMAS_RC_MARK)) {
                    if (rc_stats_on) stat_gc_freed++;
                    gc_free_body(h);
                    gc_set[i] = NULL;
                }
            }
            gc_incr_sweep_pos = gc_set_len;
            gc_incr_finish();
        }
        return;
    }
    budget = GC_SWEEP_BUDGET;
    while (gc_incr_sweep_pos < gc_set_len && budget-- > 0) {
        size_t pos = gc_incr_sweep_pos++;
        PyAdamasRc* h = gc_set[pos];
        if (gc_stats_on) ++gc_incr_objs;
        if (!h) continue;
        /* IMMORTAL 永不回收：即使标记被意外清掉也兜住。 */
        if (h->rc == PYADAMAS_RC_IMMORTAL) continue;
        if (!(h->marked & PYADAMAS_RC_MARK)) {
            if (rc_stats_on) stat_gc_freed++;
            gc_free_body(h);
            gc_set[pos] = NULL;
        }
    }
    if (gc_incr_sweep_pos >= gc_set_len) gc_incr_finish();
    if (gc_stats_on) gc_incr_ms += gc_now_ms() - _t0;
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

static double gc_now_ms(void);
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
        // 合并分配的小列表：data 内联在头块尾部，随 free(h) 一起还掉。
        if (l->data != list_inline_at(l)) free(l->data);
        // 逐元素堆位图也是堆内存，必须一起还掉（GC 清扫路径不走 list_destroy）。
        free(l->heap_bits);
        l->heap_bits = NULL;
    } else if (h->kind == PYADAMAS_RC_DICT) {
        PyAdamasDict* d = (PyAdamasDict*)((char*)h + PYADAMAS_RC_HDR);
        free(d->entries);
        free(d->order);
        free(d->index);
    } else if (h->kind == PYADAMAS_RC_STR) {
        // 非 ASCII 串可能挂着懒码点索引（GC 清扫不走 str_pool_put）。
        str_cp_index_free((PyAdamasStr*)((char*)h + PYADAMAS_RC_HDR));
    } else if (h->kind == PYADAMAS_RC_BUFFER) {
        // 0.537：buffer 的连续内存块（GC 清扫路径不走 buffer_destroy）。
        pa_buffer_release_data((char*)h + PYADAMAS_RC_HDR);
    }
    PA_DBG_FREE(h);
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
    /* IMMORTAL（rc = INT64_MIN）对象语义上是永久根，但它们既不在
       gc_globals 里、也不在栈/寄存器上（例如 sys_stream_files 只是
       C 静态数组），根扫描扫不到。必须在集合装配后显式标活并入队，
       否则 minor 的 trial-deletion 会把它当垃圾清扫掉 —— 实测
       sys.stdout/sys.stderr 的文件对象被 minor GC 误 free。 */
    for (size_t i = 0; i < gc_set_len; ++i) {
        PyAdamasRc* ih = gc_set[i];
        if (ih->rc == PYADAMAS_RC_IMMORTAL)
            gc_mark((uintptr_t)ih + (uintptr_t)PYADAMAS_RC_HDR);
    }
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
        pa_vt_gc_scan();   /* 挂起 vthread 的帧链 + temp 栈 */
        if (t0) t2 = gc_now_ms();
        if (gc_incr_mark_on) {
            /* **增量标记接线（GC 阶段1，0.330）**：根扫描已一次性拍好一致
               快照（M=1 单线程，切片间 mutator 才跑，根扫描天然原子）。这里
               不再 gc_drain 同步标完，而是启动 state=1 分片标记：
                 - gc_phase=INCR_MARK → 删除屏障（shade-on-delete）生效；
                 - gc_incr_state=1 → 插入屏障（gc_write_barrier）生效；
                 - 标记/清扫由 gc_incr_step 在分配点按 GC_MARK_BUDGET 推进，
                   队列排空 → state1→2 转换（补 release pass）→ 分片清扫。
               flag-off 不进此分支，下方 legacy 同步标记原样保留。 */
            gc_incr_state = 1;
            gc_incr_major = 1;
            gc_incr_sweep_pos = 0;
            gc_incr_countdown = GC_INCR_STRIDE;
            gc_incr_remaining = GC_INCR_STRIDE;
            gc_need_check = 1;
            gc_phase = GC_PHASE_INCR_MARK;
            if (t0)
                fprintf(stderr, "[gc] major incr-mark roots=%.2f ms set=%zu\n",
                        t2 - t1, gc_set_len);
            return;
        }
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
                /* IMMORTAL 永不回收。 */
                if (sh->rc == PYADAMAS_RC_IMMORTAL) continue;
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
        gc_need_check = 1;   /* 增量有待办 */
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
            /* IMMORTAL 的 rc 是 INT64_MIN，用 `rc > 0` 判定会把它当死对象；
               它已经在集合装配后被标活，这里再放行一次作为防御。 */
            if ((h->rc > 0 || h->rc == PYADAMAS_RC_IMMORTAL) &&
                !(h->marked & PYADAMAS_RC_MARK)) {
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
            pa_vt_gc_scan();   /* 挂起 vthread 的帧链 + temp 栈 */
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
        /* IMMORTAL 永不回收（集合装配时已标活，这里再兜一次）。 */
        if (h->rc == PYADAMAS_RC_IMMORTAL) continue;
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
    // 增量标记进行到一半就退出时（gc_incr_state != 0），先收尾：完成晋升/
    // 清扫并把 gc_pend 按正常 RC 还掉。否则 shutdown 的「整堆皆垃圾」扫描
    // 会看到半成品状态，pend 里混着链上对象 → 先 free 再 dec 读已释放头
    // （ASan heap-use-after-free），或 dec 到已释放对象（refcount decrement
    // on a freed object）。实测 PYADAMAS_GC_INCR_MARK=1 下两个探针都触发。
    if (gc_incr_state != 0) gc_incr_finish();
    // 先放掉全局根：sys.argv 的字符串在 exact-RC 模式下不在 GC 堆链上，
    // 只能靠 RC 释放（见 pa_sys_argv_shutdown 的注释）。
    pa_sys_argv_shutdown();
    // 常量 float 盒缓存持有引用：必须**在堆链清扫之前**归还。默认（索引
    // 叶子模式）盒不在链上，但 PYADAMAS_GC_CONSERVATIVE=1 时盒是 tracked
    // 对象、随链 free；清扫后再 dec 会读已释放的头。池本身留到最后清。
    pa_any_float_cache_shutdown();
    // **先按「整堆都是垃圾」的 sweep 口径把出边交出来，再统一 free。**
    //
    // 字符串（以及 exact-RC 模式的其它叶子）不在堆链上，只靠 RC 释放：
    // 如果直接 free 容器，它们引用着的字符串永远到不了 rc==0，LeakSanitizer
    // 会报 Direct leak。以前字符串全在链上、随链一起 free 所以看不出来；
    // 0.321 起字符串是索引叶子，必须在释放容器之前减掉这些边。
    //
    // 语义与 major sweep 完全一致（gc_mark_sweep=1：集合内子对象本来就要
    // 跟着 free，不参与 dec；集合外叶子由 gc_phase_dec 的就地释放 / gc_pend
    // 处理），只是这里没有 trial/mark —— 退出时一切皆垃圾。
    gc_sweeping = 1;
    gc_mark_sweep = 1;
    gc_phase = GC_PHASE_SWEEP;
    for (PyAdamasRc* h = heap_head; h; h = h->next) {
        if (h->release) h->release((char*)h + PYADAMAS_RC_HDR);
    }
    gc_phase = GC_PHASE_NONE;
    gc_mark_sweep = 0;
    // **先还清扫期攒下的「集合外」引用，再 free 整条堆链。**
    //
    // 增量标记打开时（PYADAMAS_GC_INCR_MARK=1）gc_pend 里可能混进「链上、
    // 但仍属集合内」的对象（gc_phase_dec 的 INCR_MARK 分支不区分新老对象）。
    // 原来顺序是先 `while (heap_head) gc_free_body()` 再 dec pend：链上对象
    // 已被 free，pend 里还指着它们 → ASan heap-use-after-free（读已释放的
    // rc 头）。先 dec 会让这些对象的引用归零、顺手从链上摘掉，后续 free 循环
    // 只剩真正没人引用的对象；集合外叶子也照旧在此处级联释放。
    {
        size_t pn = gc_pend_len;
        gc_pend_len = 0;
        for (size_t i = 0; i < pn; ++i) pa_rc_dec(gc_pend[i]);
    }
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
    while (pa_list_inline_pool) {
        PyAdamasRc* h = pa_list_inline_pool;
        pa_list_inline_pool = h->next;
        free(h);
    }
    pa_list_inline_pool_count = 0;
    // float 盒 freelist：缓存引用已在入口归还，清扫过程也可能把盒挂进来。
    while (float_box_pool) {
        PyAdamasRc* h = float_box_pool;
        float_box_pool = h->next;
        free(h);
    }
    float_box_pool_count = 0;
    // dict 池也要清空（头块和寄存的缓冲都是裸 malloc，必须 free）。
    for (int i = 0; i < dict_pool_count; ++i) {
        free(dict_pool[i].entries);
        free(dict_pool[i].order);
        free(dict_pool[i].index);
        free(dict_pool[i].h);
    }
    dict_pool_count = 0;
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
