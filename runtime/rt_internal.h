// rt_internal.h —— pyadamas 运行时的内部接口
//
// 运行时按职责拆成多个编译单元（rc/gc、异常、生成器、异步、任务/Future、
// fd、异步生成器、容器/输出）。这里集中放共享的类型、全局状态和内部函数
// 声明；各 .c 只包含这个头。CMake 用 ld -r 把各部分合成编译器链接的 rt.o。
#pragma once

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── 平台适配层 ───────────────────────────────────────────────
   Windows 与 POSIX 的差异集中在少数几个调用上，全部在这里一次性抹平，
   避免散落到各 .c 里。目前覆盖：轮询、目录创建、路径规范化、处理器个数、
   UTC 时间换算、setjmp 命名。fork/exec 系的 subprocess 是独立的大块，
   单独在 rt_subprocess.c 里按平台分叉。 */
#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>

/* Windows 没有下划线变体，且 CRT 的 _setjmp 是两参数版本（MSVC 语义），
   所以用函数式宏转发到标准 setjmp/longjmp。 */
#define _setjmp(env) setjmp(env)
#define _longjmp(env, val) longjmp((env), (val))

/* winsock2 的 pollfd 用的句柄类型是 SOCKET，nfds_t 在部分 mingw 版本里没有。 */
#ifndef nfds_t
#define nfds_t unsigned long
#endif

/* POSIX 的 *_r 变体在 Windows 上是 *_s，参数顺序与返回值语义不同。 */
static inline struct tm* pa_localtime_r(const time_t* t, struct tm* out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
static inline struct tm* pa_gmtime_r(const time_t* t, struct tm* out) {
    return gmtime_s(out, t) == 0 ? out : NULL;
}
#define localtime_r(t, out) pa_localtime_r((t), (out))
#define gmtime_r(t, out) pa_gmtime_r((t), (out))

/* POSIX 的 CLOCK_REALTIME / CLOCK_MONOTONIC。Windows 没有这两个 clockid，
   用两个自选常量分别映射到系统时间和单调计数器。 */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline int pa_clock_gettime(int clk, struct timespec* ts) {
    if (!ts) return -1;
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return 0;
    }
    {
        FILETIME ft;
        ULARGE_INTEGER u;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        /* 1601-01-01 到 1970-01-01 之间是 11644473600 秒 */
        u.QuadPart -= 116444736000000000ULL;
        ts->tv_sec = (time_t)(u.QuadPart / 10000000ULL);
        ts->tv_nsec = (long)((u.QuadPart % 10000000ULL) * 100ULL);
        return 0;
    }
}
#define clock_gettime(clk, ts) pa_clock_gettime((clk), (ts))

static inline int pa_nanosleep(const struct timespec* req, struct timespec* rem) {
    (void)rem;
    if (!req) return -1;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    if (ms == 0 && req->tv_nsec > 0) ms = 1;
    Sleep(ms);
    return 0;
}
#define nanosleep(req, rem) pa_nanosleep((req), (rem))

/* POSIX poll -> WSAPoll 的桥接。WSAPoll 只认 socket，而 asyncio.wait_readable /
   wait_writable 也要能等文件、控制台、管道这些普通句柄（POSIX 的 poll 支持）。
   这里按 POSIX 语义补齐：
     - 无效句柄（_get_osfhandle 失败）-> POLLNVAL，调用方报 OSError
     - 非 socket 的有效句柄 -> 视为始终就绪（它们不能 select，但同步读写
       不会长时间阻塞）
     - socket -> 收集起来交给 WSAPoll；若已有非 socket 就绪则不再等待
   结构体由 winsock2.h 提供（SOCKET/SHORT/SHORT），与 POSIX 逐字段对应。 */
static inline int pa_poll(struct pollfd* fds, unsigned long n, int timeout) {
    if (n == 0) {
        if (timeout > 0) Sleep((DWORD)timeout);
        return 0;
    }
    if (!fds) return 0;
    struct pollfd* socks = (struct pollfd*)malloc(sizeof(struct pollfd) * n);
    unsigned long* smap = (unsigned long*)malloc(sizeof(unsigned long) * n);
    if (!socks || !smap) {
        free(socks);
        free(smap);
        return -1;
    }
    unsigned long ns = 0;
    int ready = 0;
    for (unsigned long i = 0; i < n; ++i) {
        fds[i].revents = 0;
        /* 先按 socket 试：socket 的值不是 CRT fd，_get_osfhandle 会失败。 */
        int type = 0;
        int tlen = (int)sizeof type;
        if (getsockopt((SOCKET)(uintptr_t)fds[i].fd, SOL_SOCKET, SO_TYPE, (char*)&type,
                       &tlen) == 0) {
            socks[ns].fd = fds[i].fd;
            socks[ns].events = fds[i].events;
            socks[ns].revents = 0;
            smap[ns] = i;
            ++ns;
            continue;
        }
        /* 不是 socket：CRT fd -> 普通句柄；也接受直接的 HANDLE 值。
           句柄有效性必须真的问一次系统：随便一个整数（例如 999999）
           既不是 fd 也不是句柄，应当报 POLLNVAL。 */
        intptr_t h = _get_osfhandle(fds[i].fd);
        if (h == -1) h = (intptr_t)fds[i].fd;
        DWORD hflags = 0;
        if (h == -1 || h == 0 || !GetHandleInformation((HANDLE)h, &hflags)) {
            fds[i].revents = POLLNVAL;
            ++ready;
            continue;
        }
        short ev = (short)(fds[i].events & (POLLIN | POLLOUT));
        fds[i].revents = ev;
        if (ev) ++ready;
    }
    if (ns) {
        int tmo = ready > 0 ? 0 : timeout;
        int rc = WSAPoll((WSAPOLLFD*)socks, (ULONG)ns, (INT)tmo);
        if (rc > 0) {
            for (unsigned long k = 0; k < ns; ++k) {
                if (!socks[k].revents) continue;
                fds[smap[k]].revents |= socks[k].revents;
                ++ready;
            }
        } else if (rc == SOCKET_ERROR) {
            for (unsigned long k = 0; k < ns; ++k) {
                fds[smap[k]].revents |= POLLNVAL;
                ++ready;
            }
        }
    }
    free(socks);
    free(smap);
    return ready;
}
#define poll(fds, n, timeout) pa_poll((fds), (n), (timeout))

/* POSIX mkdir 带 mode，Win32 不带。 */
#define mkdir(path, mode) _mkdir(path)

/* timegm -> _mkgmtime */
#define timegm(tm) _mkgmtime(tm)

static inline char* pa_realpath(const char* p, char* unused) {
    (void)unused;
    char* buf = (char*)malloc(_MAX_PATH);
    if (!buf) return NULL;
    if (_fullpath(buf, p, _MAX_PATH) == NULL) { free(buf); return NULL; }
    return buf;
}
#define realpath(p, r) pa_realpath((p), (r))

static inline long pa_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)(si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1);
}
#define pa_SC_NPROCESSORS_ONLN 1
#define sysconf(x) pa_cpu_count()
#define _SC_NPROCESSORS_ONLN pa_SC_NPROCESSORS_ONLN

#else
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#endif
/* 内联汇编里的函数元数据：ELF 用 .type/.size，COFF 不接受这两个指示符。 */
#if defined(_WIN32)
#define PA_TYPE_FUNC(n)
#define PA_SIZE_FUNC(n)
#else
#define PA_TYPE_FUNC(n) ".type " #n ",@function\n"
#define PA_SIZE_FUNC(n) ".size " #n ", .-" #n "\n"
#endif
/* 生成代码走 ABI 里的 setjmp 符号；运行时这边也必须用同一对。
   Windows 用自带实现（见 rt_exc.c），POSIX 用 _setjmp/_longjmp。 */
#if defined(_WIN32)
extern int pa_win_setjmp(void* env);
extern void pa_win_longjmp(void* env, int val);
#define PA_SETJMP(env) pa_win_setjmp((void*)(env))
#define PA_LONGJMP(env, val) pa_win_longjmp((void*)(env), (val))
#else
#define PA_SETJMP(env) _setjmp(env)
#define PA_LONGJMP(env, val) _longjmp((env), (val))
#endif


/* 字符串/字节串共用表示。str 与 bytes 的区别全部由静态类型决定，
   运行期结构一样，只有 pub 的含义不同：
     pub  +0  str: 码点数（nchars）；bytes: 字节数（与 len 相等）
     len  +8  UTF-8 字节数（bytes 的字节数；既有字段语义不变）
     hash +16 字节长度 <=7 时内联字符区（SSO），否则字典哈希缓存（0=未算）
     data +24 字符区；SSO 时指向 &hash
   pub 放偏移 0 是刻意的：IrOp::Len 与真值判断都只读偏移 0，str/bytes/容器
   通用，改成码点语义后这两条指令一行都不用改，len(unknown) 也自动正确。
   不变量：str 满足 0 <= pub <= len，等号当且仅当全 ASCII；bytes 满足 pub == len。 */
typedef struct {
    int64_t pub;
    int64_t len;
    int64_t hash;      /* 预留给字符串驻留 */
    const char* data;
    /* **短串哈希缓存。**
     *
     * 上面 4 个字段的偏移不能动（codegen 直接按 +24 取 data，cimport 也依赖），
     * 所以缓存加在末尾，sizeof 从 32 变成 40。
     *
     * 为什么需要：len <= 7 的串走 SSO，字符内联在 hash 字段里，没有地方
     * 缓存哈希。而字典反复查同一个短键（词频统计这类）每次都要重跑 FNV。
     * 实测同样的 16 个键，len=8（能缓存）比 len=7（不能）快 33%
     * （174.3ms -> 116.6ms）。
     *
     * 0 表示未计算；非 0 是 mix64 之后的哈希（和 dict_hash_value 一致）。
     * 字面量在编译期就填好（见 codegen 的 .Lstr 发射），运行时创建的串懒算。
     */
    int64_t chash;
} PyAdamasStr;

/* 字符串迭代游标：cp 是上次取出的码点下标，off 是它之后的下一个字节偏移。
   顺序访问（i == cp+1）命中时只读一个码点，O(1)；否则退回从头扫。
   降级器为每个 for/推导式循环分配一对 int64 槽，循环入口重置成 -1/0。 */
typedef struct {
    int64_t cp;
    int64_t off;
} PyAdamasStrCursor;

/* elem_kind: 0 = 全部非堆类型，1 = 全部堆类型，2 = 有 unknown / 混合，
   3 = 未定（只可能出现在空列表上，第一次写入时由 pa_list_prep 定型）。
   kind 1/2 的元素在写入时 retain（堆→rc_inc，unknown→rc_inc_unknown），
   覆盖/截断/销毁时 release（kind 2 走注册表判断，整数是空操作）。 */
/* elem_kind: 0 = 全部非堆类型，1 = 全部堆类型，2 = 有 unknown / 混合，
   3 = 未定（只可能出现在空列表上，第一次写入时由 pa_list_prep 定型）。
   kind 1/2 的元素在写入时 retain（堆→rc_inc，unknown→rc_inc_unknown），
   覆盖/截断/销毁时 release（kind 2 走注册表判断，整数是空操作）。

   heap_bits：**逐元素「是否堆指针」位图**（方案 A）。

   kind=2（混合）时，元素 release 需要判断「这个机器字是不是堆指针」。
   原来只能查全局 rc_index（pa_rc_dec_unknown），那是 O(1) 哈希探测，
   但实测 bench_rc_gen 有 46.7 万次 dec_unknown、占掉 21ms（总 84ms 的
   25%）。而元素是不是堆指针在**写入那一刻**就知道了（写入点的静态
   类型），完全没必要事后到全局索引里反查。

   位图与 data 等长（cap 位），append 时按写入值的静态类型置位：
     - 静态类型是堆（str/list/dict/…）→ 置 1；
     - 静态类型是非堆（int/float/bool/None）→ 置 0；
     - 静态类型是 unknown → 无法静态确定，标记为「需要回退查索引」
       （用 heap_bits == NULL 表示整体回退，见下）。

   heap_bits == NULL 表示这个列表无法逐元素跟踪（写入点类型未知），
   release 时仍走 pa_rc_dec_unknown 的旧路径 —— 保证正确性不变。 */
typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* data;
    int64_t elem_kind;
    uint64_t* heap_bits;   /* 逐元素堆位图，NULL = 不可跟踪（回退索引） */
    int64_t bits_cap;      /* heap_bits 的字数（cap 会被缓冲池抬高，不能复用） */
} PyAdamasList;

/* 小列表合并分配：容量 <= 8 元素的列表，data 缓冲直接内联在
   「RC 头 + PyAdamasList」同一个 malloc 块的尾部（rt_core.c 的
   list_inline_pool），建/销各省一次池操作。内联缓冲是块内指针，
   **不能 realloc / free**：增长走 list_buf_grow 换外部块，销毁时
   按 data 是否等于 list_inline_at(l) 判断。 */
#define PYADAMAS_LIST_INLINE_CAP 8
#define PYADAMAS_LIST_INLINE_BYTES ((int64_t)PYADAMAS_LIST_INLINE_CAP * (int64_t)sizeof(int64_t))
static inline int64_t* list_inline_at(PyAdamasList* l) { return (int64_t*)(l + 1); }

/* range 是惰性对象：只存边界，元素按需算。和 CPython 的 range 一样，
   range(10**9) 只占几十字节，不会当场造十亿个元素。 */
typedef struct {
    int64_t start;
    int64_t stop;
    int64_t step;
    int64_t len;
} PyAdamasRange;

/* 字典 = entries + 开放寻址索引表。index 槽存「entry 下标 + 1」，0 表示
   空槽、-1 表示删除留下的墓碑（探测越过它继续）；查找先走 4 字节索引，
   命中才读 entry。存活项用 prev/next 串成插入顺序的双向链表，删除只摘链、
   条目留原地当墓碑，由 free_head 链复用——摊还 O(1)，不用 memmove。
   order 是链表的下标快照，按需重建（ord_len == len 表示有效），给按序号
   遍历的生成代码提供 O(1) 访问。set 复用同一结构，val_own = 0、值恒为 0。
   len 在偏移 0，len() 和真值判断都只读它。 */
typedef struct {
    int64_t hash;   /* 0 表示墓碑/空槽；真实 hash 强制非 0 */
    int64_t key;
    int64_t val;
    int32_t prev;   /* 插入顺序双向链表（存 entry 下标，-1 结束） */
    int32_t next;
} PyAdamasDictEntry;

typedef struct {
    int64_t len;
    int64_t cap;       /* entries / order 容量 */
    PyAdamasDictEntry* entries;
    int32_t* order;    /* order[i] = 第 i 个存活项的 entry 下标（懒重建） */
    int32_t* index;    /* 开放寻址索引：entry 下标 + 1，0 为空，-1 墓碑 */
    int64_t icap;      /* 索引表容量（2 的幂） */
    int64_t ord_len;   /* order 有效长度；!= len 表示需要重建 */
    int64_t nent;      /* entries 高水位（含墓碑） */
    int64_t itomb;     /* 索引表里的墓碑数 */
    int64_t free_head; /* 墓碑 entry 空闲链，-1 结束，next 复用 entry.key */
    int32_t head;      /* 插入顺序链表头/尾（entry 下标，-1 为空） */
    int32_t tail;
    int64_t key_kind;  /* 0 int/bool, 1 str, 2 float, 3 raw, 9 raw or BigInt */
    int64_t key_own;   /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;   /* 0 非堆, 1 堆, 2 unknown */
    /* 类 key 的 __hash__ / __eq__ vtable 槽号；-1 表示按机器字哈希/比较。 */
    int64_t hash_slot;
    int64_t eq_slot;
    /* 迭代版本号：任何「插入新键」或「删除键」都 +1。
       迭代守卫用它捕捉「删一个再加一个」——大小不变但键集合变了，
       CPython 3.14 抛的是 "dictionary keys changed during iteration"。 */
    int64_t version;
    /* collections.defaultdict 的内置工厂；0 表示普通 dict。
       只记录工厂种类，缺省下标读取时才创建值，不影响 get()/in。 */
    int64_t default_factory;
    /* 0.302：sticky 堆标志。own==2 的 entry 写入路径探测到键/值是堆指针
       就置 1（只置不清；clear/new 归零）。key_own==val_own==2 且它为 0 时
       全部 entry 都是标量，destroy/gc_release/clear 跳过链游。追加在结构体
       尾部，既有字段偏移不动（codegen 只按偏移 0 读 len，无硬编码尺寸）。 */
    int64_t saw_heap;
} PyAdamasDict;


/* unknown 值平行 tag 的编码（与 src/tag_defs.h 的 PaTag 逐字对应）。
   P3（默认）：堆 ⇔ tag>=3。P2（PYADAMAS_TYPE_TAGS）：追加 NONE/BOOL（非堆）
   与 TUPLE/SET/BYTES/CLASS/BIG（堆），堆 ⇔ 3..5 或 >=8。运行时是同一份
   rt.o，两种编号都要认识。 */
enum {
    PA_TAG_UNK   = 0,
    PA_TAG_INT   = 1,
    PA_TAG_FRAW  = 2,
    PA_TAG_FBOX  = 3,
    PA_TAG_STR   = 4,
    PA_TAG_HEAP  = 5,
    PA_TAG_NONE  = 6,   /* 非堆：None（payload 0） */
    PA_TAG_BOOL  = 7,   /* 非堆：bool（payload 0/1） */
    PA_TAG_TUPLE = 8,   /* 堆：tuple（头 kind 仍是 LIST） */
    PA_TAG_SET   = 9,   /* 堆：set（头 kind 仍是 DICT） */
    PA_TAG_BYTES = 10,  /* 堆：bytes（头 kind 仍是 STR） */
    PA_TAG_CLASS = 11,  /* 堆：类对象（头 kind OBJ + class magic） */
    PA_TAG_BIG   = 12,  /* 预留：BigInt（堆） */
};

enum {
    PYADAMAS_RC_STR = 0,
    PYADAMAS_RC_LIST = 1,
    PYADAMAS_RC_OBJ = 2,
    PYADAMAS_RC_CLOSURE = 3,
    PYADAMAS_RC_DICT = 4,
    /* 用户类实例。和帧/await/异常值等 OBJ 负载分开，isinstance 才能
       安全地把 payload 第一个字解释成 vtable 指针。 */
    PYADAMAS_RC_INSTANCE = 5,
    /* range：没有堆字段（出边为零），不可能成环，按叶子对象处理。 */
    PYADAMAS_RC_RANGE = 6,
    /* 值类型 struct：payload 就是 C 结构体，字段只有标量，无出边。 */
    PYADAMAS_RC_STRUCT = 7,
    /* CPython 桥的 float 盒：unknown 机器字分不开 int/float，桥返回的
       float 装进 8 字节 payload（double 位模式）的无出边对象。
       print/探针按 kind 识别。见 runtime/rt_cpy.c。 */
    PYADAMAS_RC_CPYFLOAT = 8,
    /* P3 big：任意精度整数。limb 内联在 RC 块尾部（柔性数组），无堆出边、
       不可变；tracked 分配让 unknown 探针认识它。见 runtime/rt_big.c。 */
    PYADAMAS_RC_BIGINT = 9,
};

typedef struct PyAdamasRc {
    struct PyAdamasRc* next;
    struct PyAdamasRc* prev;
    int64_t rc;
    void (*dtor)(void*);
    void (*release)(void*);
    int64_t size;
    int64_t kind;
    int64_t marked;
} PyAdamasRc;

#define PYADAMAS_RC_IMMORTAL INT64_MIN
#define PYADAMAS_RC_HDR ((int64_t)sizeof(PyAdamasRc))
/* 内联池头/计数：导出给生成代码的元组分配快路径（0.280）。 */
extern PyAdamasRc* pa_list_inline_pool;
extern int pa_list_inline_pool_count;
_Static_assert(sizeof(PyAdamasRc) == 64, "codegen literal headers must match PyAdamasRc size");

#define PYADAMAS_GC_THRESHOLD (2 * 1024 * 1024)

/* 普通函数帧记录：codegen 在函数序言压栈、正常返回前弹栈。它同时服务
   两件事：
   - 异常展开：按槽地址 dec 持有引用的槽；
   - GC 保守根：含 unknown 值的帧把「可能借来 unknown 引用」的槽和临时
     区登记进来，GC 只扫这些地址，不再扫整段真实栈。
   布局与生成代码一致：prev、n、临时区起点、临时区字数、临时区位图、
   本帧 CFA、槽地址×n。
   槽地址低两位是标志：bit0 = 值可能非堆（释放走注册表），bit1 = 只给
   GC 扫描、异常展开不释放（借来的 unknown 引用）。
   临时区位图（scan_mask）：scan_words 个字里哪些字可能装着需要 GC 看的
   指针，bit i = 1 表示「可能」。位图是超集，宁多勿少；NULL 表示没有
   位图，按整段扫描（回退口径，早期目标代码也只认这一种）。 */
typedef struct PyAdamasFrame {
    struct PyAdamasFrame* prev;
    int64_t n;
    void* scan_base;    /* 临时区起点（vreg 槽），NULL 表示没有 */
    int64_t scan_words; /* 临时区字节数 / 8（整段字数，回退口径） */
    const uint64_t* scan_mask; /* 临时区位图，NULL = 整段扫描 */
    /* 本帧的规范帧地址（CFA = rsp + frame_bytes + 8，与 .cfi_def_cfa 同一
       口径）：codegen 在推帧进链之前写进来。表驱动展开时 personality 用它
       判断「哪些帧记录比 handler 帧更深」——CFA 随栈深度严格递减，同一条
       链上不会重复，等价于老世界 pa_frame_unwind(handler_top->frame) 的停
       止条件。见 docs/exc-frame-cleanup.md §5.1 与 runtime/rt_eh.c 里关于
       libgcc _Unwind_GetCFA 口径的说明（停止条件用 <=）。 */
    uintptr_t cfa;
    void* slots[];      /* 槽地址（void**），解引用得到当前值 */
} PyAdamasFrame;

typedef struct PyAdamasHandler {
    jmp_buf env;  /* 必须放在开头：指针同时作为 setjmp 的参数 */
    struct PyAdamasHandler* prev;
    int64_t mark;
    /* 压 handler 时的帧栈顶：异常展开到这一层就停，当前帧的槽
       留给函数自己正常退出时释放。 */
    struct PyAdamasFrame* frame;
    /* 生成器挂起时归属的生成器对象（NULL = 不在挂起表里）。
       pa_handler_suspend 登记、pa_handler_rearm 注销，生成器销毁时
       按 owner 找回并归还 freelist。记录本身在挂起期间保持有效 ——
       因为 rearm 用的就是同一个地址（生成代码从帧槽里取）。 */
    void* suspend_owner;
    /* 是否由 pa_handler_push 从堆上分配。栈上的记录（gen_run / pa_gen_close
       的本地 PyAdamasHandler）不能归还 freelist —— 那会往栈内存写。 */
    int64_t heap_alloc;
} PyAdamasHandler;

typedef struct PyAdamasTraceFrame {
    const char* file;
    const char* func;
    int32_t line;
    int32_t pad;
    /* P4 块3（lang_gaps）：捕获时该帧的 CFA（0 = 未知，逻辑帧/旧链）。
       except 落地时按 handler 帧 CFA 裁剪待提交链用（pa_trace_commit）。 */
    uintptr_t cfa;
    struct PyAdamasTraceFrame* next;
} PyAdamasTraceFrame;

typedef struct PyAdamasException {
    char* type;     /* 异常名，如 "ValueError" / 自定义异常类名 */
    char* message;  /* 可以为空串，不能为 NULL */
    struct PyAdamasException* cause;  /* raise ... from，owned */
    /* 生成器/协程逃出的逻辑帧（最外层在前），malloc 链表。 */
    struct PyAdamasTraceFrame* trace;
    /* P4 块3（lang_gaps 运行期门控）：抛出漏斗捕获的物理帧链（最外层在前）。
       phys = 已提交（sys.exc_info 的 tb 读它）；phys_extra = 待提交——抛出时
       还不知道会被哪个 handler 接住，先整链记下，except 落地（pa_exc_accept*）
       时按 handler 帧 CFA 裁剪（CPython 口径：traceback 从接住帧开始）再并入
       phys。两条都是 malloc 链：exc_free 释放、exc_clone 复制、ExcValue 搬运。
       门控关时恒为 NULL，捕获/提交入口第一行判 pa_lang_gaps_rt 即返回。 */
    struct PyAdamasTraceFrame* phys;
    struct PyAdamasTraceFrame* phys_extra;
    /* P4 块3：零开销展开的 handler 帧 CFA（personality phase 2 钩子写入）。
       落地提交时按它裁剪 phys_extra；0 = 未设置（setjmp 路径用帧记录地址
       锚点推 handler CFA）。提交后清零。 */
    uintptr_t handler_cfa;
} PyAdamasException;

typedef struct PyAdamasExcSave {
    PyAdamasException* prev;
    struct PyAdamasExcSave* up;
} PyAdamasExcSave;

/* 异常值对象：`raise ValueError("x")` 里的实例、`except as e` 绑定的 e。
   控制流仍用 PyAdamasException* + longjmp；这个对象只是值语义的外壳。 */
#define PYADAMAS_EXCVAL_MAGIC 0x4558435600000001LL
typedef struct PyAdamasExcValue {
    int64_t magic;
    char* type;
    char* message;
    PyAdamasList* args;             /* owned：args[0] 是 message 字符串 */
    struct PyAdamasExcValue* cause; /* __cause__，owned（RC 对象） */
    /* P4 块3（lang_gaps）：扁平异常物理帧链的副本（malloc 链，owned）。
       finally 复制（pa_exc_take_value）/ raise e 经 ExcValue 搬运时 traceback
       不丢：value_from_exc 克隆进来，exc_from_value 搬回去（move）。
       门控关时恒 NULL（源异常的 phys/phys_extra 本就是 NULL）。 */
    struct PyAdamasTraceFrame* trace;
    struct PyAdamasTraceFrame* trace_extra;
} PyAdamasExcValue;

/* 复数：re/im 两个 double，RC 对象；magic 用于运行时判别。 */
#define PYADAMAS_COMPLEX_MAGIC 0x434f4d5000000001LL
typedef struct {
    int64_t magic;
    double re;
    double im;
} PyAdamasComplex;

/* ── P3 big：任意精度整数（runtime/rt_big.c）─────────────────────
 * 布局：RC 头 + { nd（带符号 limb 数，基 2^32 小端，无最高位零 limb）; d[] }。
 * 不可变值语义：运算产生新对象（rc=1），操作数是借用。BIG tag=12（堆）。 */
typedef struct {
    int64_t nd;      /* 0 = 零；>0 正；<0 负；|nd| = limb 个数 */
    uint32_t d[];    /* 基 2^32 小端 limb，内联在同一 malloc 块 */
} PyAdamasBig;

PyAdamasBig* pa_big_from_i64(int64_t v);
PyAdamasBig* pa_big_from_u64(uint64_t v);
PyAdamasBig* pa_big_from_f64(double d);          /* int(float)：向零截断 */
PyAdamasBig* pa_big_from_cstr(const char* s, int64_t base);
PyAdamasBig* pa_big_from_str(PyAdamasStr* s, int64_t base);
int64_t pa_big_to_i64(PyAdamasBig* a);           /* cast 语义：低 64 位回绕 */
double pa_big_to_f64(PyAdamasBig* a);            /* 正确舍入；过大 OverflowError */
PyAdamasBig* pa_big_add(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_sub(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_mul(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_floordiv(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_mod(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_pow(PyAdamasBig* a, PyAdamasBig* b);
int64_t pa_big_cmp(PyAdamasBig* a, PyAdamasBig* b);   /* -1/0/1 */
int64_t pa_big_cmp_f64(PyAdamasBig* a, double d);     /* 精确混比；NaN=-2 */
int64_t pa_big_eq(PyAdamasBig* a, PyAdamasBig* b);
int64_t pa_big_truth(PyAdamasBig* a);
PyAdamasBig* pa_big_neg(PyAdamasBig* a);
PyAdamasBig* pa_big_abs(PyAdamasBig* a);
PyAdamasBig* pa_big_not(PyAdamasBig* a);          /* ~a = -a-1 */
double pa_big_truediv(PyAdamasBig* a, PyAdamasBig* b);  /* 正确舍入 double */
PyAdamasBig* pa_big_and(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_or(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_xor(PyAdamasBig* a, PyAdamasBig* b);
PyAdamasBig* pa_big_lshift(PyAdamasBig* a, PyAdamasBig* cnt);
PyAdamasBig* pa_big_rshift(PyAdamasBig* a, PyAdamasBig* cnt);
PyAdamasStr* pa_big_str(PyAdamasBig* a);         /* 十进制（repr 同形） */
PyAdamasStr* pa_big_to_base(PyAdamasBig* a, int64_t base);  /* 2/8/10/16 */
PyAdamasStr* pa_big_fmt(PyAdamasBig* a, PyAdamasStr* spec); /* format()（rt_format.c） */

PyAdamasComplex* pa_complex_new(double re, double im);
PyAdamasComplex* pa_complex_add(PyAdamasComplex* a, PyAdamasComplex* b);
PyAdamasComplex* pa_complex_sub(PyAdamasComplex* a, PyAdamasComplex* b);
PyAdamasComplex* pa_complex_mul(PyAdamasComplex* a, PyAdamasComplex* b);
PyAdamasComplex* pa_complex_div(PyAdamasComplex* a, PyAdamasComplex* b);
PyAdamasComplex* pa_complex_neg(PyAdamasComplex* a);
double pa_complex_abs(PyAdamasComplex* a);
PyAdamasStr* pa_complex_str(PyAdamasComplex* a);
int64_t pa_complex_eq(PyAdamasComplex* a, PyAdamasComplex* b);
int64_t pa_complex_truth(PyAdamasComplex* a);
/* unknown 值的 Python 真值/ repr 打印（rt_core.c）。 */
int64_t pa_truth_unknown(int64_t v);
/* unknown 域的最小判别支持（rt_any.c，0.324）：float 装箱/拆箱 +
   动态算术/比较。只有静态类型是 unknown 的表达式会调用这些入口。 */
int64_t pa_any_from_f64(double v);
int64_t pa_any_is_float(int64_t v);
void pa_any_float_cache_shutdown(void);
double pa_any_to_f64(int64_t v);
int64_t pa_any_to_i64(int64_t v);
int64_t pa_any_arith(int64_t op, int64_t a, int64_t b, int64_t da, int64_t db);
int64_t pa_any_neg(int64_t v, int64_t dv);
int64_t pa_any_big_binary(int64_t op, int64_t a, int64_t b, int64_t da, int64_t db);
int64_t pa_any_invert(int64_t v, int64_t dv);
int64_t pa_any_cmp(int64_t op, int64_t a, int64_t b, int64_t da, int64_t db);
/* P3 第一阶段（0.327，docs/p3-tagged-union-design.md）：shadow tag 版入口。
   tag 编码与 src/codegen_impl.h 的 PaTag 逐字对应：0 UNK / 1 INT / 2 FRAW /
   3 FBOX / 4 STR / 5 HEAP（堆 ⇔ >=3）。tag 是 RC/分派快路径判据，UNK 回退
   探针；值仍是旧 unknown i64（float 仍装箱），边界丢 tag 只损失性能。
   PaAnyPair 按 SysV {i64,i64} 返回：%rax=payload，%rdx=tag。 */
typedef struct { int64_t v; int64_t tag; } PaAnyPair;
int64_t pa_any_load_tag(int64_t v);
PyAdamasBig* pa_any_to_big(int64_t v);
PaAnyPair pa_any_arith_t(int64_t op, int64_t a, int64_t tag_a,
                         int64_t b, int64_t tag_b, int64_t reuse_ok);
PaAnyPair pa_any_neg_t(int64_t v, int64_t tag, int64_t reuse_ok);
PaAnyPair pa_any_big_binary_t(int64_t op, int64_t a, int64_t tag_a, int64_t b, int64_t tag_b);
PaAnyPair pa_any_invert_t(int64_t v, int64_t tag);
int64_t pa_any_cmp_t(int64_t op, int64_t a, int64_t tag_a,
                     int64_t b, int64_t tag_b);
int64_t pa_any_is_float_t(int64_t v, int64_t tag);
double pa_any_to_f64_t(int64_t v, int64_t tag);
int64_t pa_any_to_i64_t(int64_t v, int64_t tag);
void pa_print_any_repr(int64_t v);
/* P2 第二阶段（0.340，PYADAMAS_TYPE_TAGS）：运行期类型码 + isinstance 谓词
   + 带 tag 的 repr（rt_any.c / rt_str.c）。类型码与 src/tag_defs.h 的
   PaTypeCode 逐字对应；tag 无精确信息时按头 kind 判型（边界见实现注释）。 */
int64_t pa_any_type(int64_t v, int64_t tag);
int64_t pa_any_type_is(int64_t v, int64_t tag, int64_t code);
PyAdamasStr* pa_repr_any_t(int64_t v, int64_t tag);
void pa_print_any_repr_t(int64_t v, int64_t tag);
PyAdamasStr* pa_fmt_i64(int64_t v, PyAdamasStr* spec);
PyAdamasStr* pa_fmt_u64(uint64_t v, PyAdamasStr* spec);
PyAdamasStr* pa_fmt_bool(int64_t v, PyAdamasStr* spec);
PyAdamasStr* pa_fmt_f64(double v, PyAdamasStr* spec);
PyAdamasStr* pa_fmt_str(PyAdamasStr* v, PyAdamasStr* spec);
PyAdamasStr* pa_percent_format(PyAdamasStr* pat, PyAdamasList* args, PyAdamasStr* desc, int64_t is_bytes);
PyAdamasStr* pa_fmt_complex(PyAdamasComplex* v, PyAdamasStr* spec);
PyAdamasStr* pa_repr_str(PyAdamasStr* v);


typedef struct {
    int64_t magic;
    int64_t kind;      /* 1 sleep, 2 gather, 3 task, 4 asyncgen, 5 future, 6 fd, 8 wait_for */
    int64_t done;
    int64_t starting;  /* gather 正在启动子任务：完成延后到启动结束 */
    int64_t result;    /* owned */
    int64_t result_kind; /* 结果的所有权类别：0 裸值（float 位模式）/ 1 引用 */
    int64_t exc;       /* PyAdamasException* */
    int64_t waiter;    /* owned ref */
    int64_t deadline_ms;
    int64_t remaining;
    int64_t task_coro; /* task：被包装的协程（owned） */
    int64_t cancelled;
    int64_t driver_gen; /* kind 4：被驱动的异步生成器（owned） */
    int64_t driver_sent; /* kind 4：asend 传入值（owned） */
    int64_t driver_close; /* kind 4：aclose 模式 */
    int64_t driver_throw; /* kind 4：athrow 注入的异常（owned，PyAdamasException*） */
    int64_t driver_await_result; /* kind 4：帧槽已由内部 await 的结果/异常填充 */
    int64_t pending;    /* future/fd：已经挂进 loop_pending/loop_fds */
    int64_t fd;         /* kind 6：等待的 fd */
    int64_t write;      /* kind 6：0 读 / 1 写 */
    PyAdamasList* waiters;  /* kind 3/5 task/future：多个等待者（owned refs） */
    PyAdamasList* results;
    PyAdamasList* children;
    PyAdamasList* callbacks; /* kind 5 future：add_done_callback 注册的函数（owned refs） */
} PyAdamasAwait;

#define PYADAMAS_GEN_HEADER 10
#define PYADAMAS_CORO_MAGIC 0x434f524f00000001LL
#define PYADAMAS_AGEN_MAGIC 0x4147454e00000001LL
#define PYADAMAS_AWAIT_MAGIC 0x4157414954000001LL

extern PyAdamasRc* heap_head;
extern int64_t alloc_since_gc;
extern int gc_ready;
extern int gc_sweeping;
extern PyAdamasRc** rc_index;
extern size_t rc_index_cap;
extern size_t rc_index_fill;
extern size_t rc_index_live;
extern void** temp_stack;
extern int64_t temp_len;
extern int64_t temp_cap;
extern PyAdamasHandler* handler_top;

/* 运行时诊断（rt_diag.c）。不设环境变量时全部是空操作。 */
extern int pa_diag_rc_on;   /* -1=未初始化, 0=关闭, 1=开启 */
void pa_diag_rc_alloc(void* p, int64_t size, int32_t kind);
void pa_diag_rc_free(void* p, int32_t kind);
void pa_diag_temp(const char* what, void* p, int64_t depth);
void pa_diag_snapshot(const char* when);
void pa_diag_check_use(void* p, const char* what);
void pa_exc_trace_alloc(PyAdamasException* e);
void pa_exc_trace_move(PyAdamasException* e, const char* what, void* where);
void pa_exc_trace_free(PyAdamasException* e);
extern PyAdamasFrame* pa_frame_top;
/* 异常 longjmp 前调用：释放 target 之上（更内层）所有帧里的堆槽引用。 */
void pa_frame_unwind(PyAdamasFrame* target);
/* 表驱动展开（Z2 方案 G）：释放帧链上所有「cfa <= target_cfa」的记录——
   即比 handler 帧更深、被逻辑展开跳过、没人负责释放的那些帧。
   target_cfa 传 personality 在 phase 2 拿到的 _Unwind_GetCFA(ctx)：它等于
   handler 帧直接被调帧的 CFA（= handler 帧 call site 的 rsp），所以判据是
   <= 而不是 <（口径与实测见 runtime/rt_eh.c 的同一函数注释）。
   语义与 pa_frame_unwind(handler_top->frame) 等价，只是停止条件从「记录
   指针」换成「CFA」；由 personality 在 phase 2 的 handler 帧里、安装
   landing pad 之前调用（实现见 runtime/rt_eh.c）。 */
void pa_frame_unwind_to_cfa(uintptr_t target_cfa);
/* 调试表：codegen 在模块入口注册，未捕获异常时用来打印函数级回溯。 */
void pa_debug_register(const void* table);
void pa_print_traceback(void);
int pa_debug_lookup_pc(uintptr_t pc, const char** file, const char** func, int32_t* line);
/* 生成器/协程帧逃出异常时，把逻辑帧挂到异常上；未捕获时追加打印。 */
void pa_trace_attach(PyAdamasException* e, void* gen_frame);
void pa_trace_print(const PyAdamasException* e);
PyAdamasTraceFrame* pa_trace_clone(const PyAdamasTraceFrame* f);
void pa_trace_free(PyAdamasTraceFrame* f);
/* P4 块3（lang_gaps，0.346）：运行期门控 + 物理帧捕获/提交。
   pa_lang_gaps_enable 由入口模块体首发的 LangGapsEnable 调用（门控开才发射）；
   关时 pa_lang_gaps_rt=0，捕获/提交全是空操作，既有行为零变化。 */
extern int pa_lang_gaps_rt;
void pa_lang_gaps_enable(void);
/* 捕获模式：NEW 新抛（整链）；VALUE raise e（整链前插旧链）；
   RERAISE 裸 raise（去掉最内层重抛帧后前插——CPython 裸 raise 不加当前帧）。 */
#define PA_CAP_NEW 0
#define PA_CAP_VALUE 1
#define PA_CAP_RERAISE 2
void pa_trace_capture(PyAdamasException* e, int mode);
/* except 落地提交：按 pa_frame_top->cfa（handler 帧）裁剪 phys_extra 并入 phys。 */
void pa_trace_commit(PyAdamasException* e);
/* sys.exc_info()：(类值, 异常值, tb) 三元组；outside except → (None,None,None)。 */
PyAdamasList* pa_exc_info(void);
/* __pa_tb_frames(tb)：list[(file,line,func)]，外→内。 */
PyAdamasList* pa_tb_frames(void* tb);
extern PyAdamasException* pending_exc;
extern PyAdamasException* current_exc;
/* 富异常（有字段/方法的自定义异常类）的实例；0 = 当前异常不是富实例。 */
extern void* current_exc_inst;
extern PyAdamasExcSave* exc_save_top;
/* 多参异常的可选 args 表（RC owned）；0 = 无。定义在 rt_exc.c；
   零开销异常的抛/落地也要搬它（runtime/rt_eh.c），所以提到头里。 */
extern PyAdamasList* pending_exc_args;

/* ── 零开销异常（Z2）的运行时胶水，实现见 runtime/rt_eh.c ────────────
   抛路径不再 longjmp，而是把异常包进 Itanium ABI 的 struct
   _Unwind_Exception（首成员，16 字节对齐）交给 libgcc 的表驱动展开；
   landing pad 侧再把异常取回来、装成和 longjmp 落地后一样的全局状态，
   所以下面的 setjmp 路径逻辑一行不用改，两套共存。
   包装的 exception_class 用这个魔数（与 C++ 的 "GNUCC++" 区分）；
   personality 只对自己的类做帧清理，foreign 异常不碰我们的帧链。 */
#define PA_EH_EXC_CLASS ((uint64_t)0x504145585A320001ULL)  /* "PAEXZ2" + 版本 0 */
/* pa_eh_throw 的返回值。成功时控制已交给 landing pad，函数不会返回。 */
#define PA_EH_UNCAUGHT 1  /* _Unwind_RaiseException 返回 _URC_END_OF_STACK：没人接 */
#define PA_EH_FATAL 2     /* 展开器内部错误（FATAL_PHASE1/2_ERROR 等），不该发生 */
/* 抛出：接管 e 的所有权（e 不能同时还是 pending_exc），装进展开对象后调
   _Unwind_RaiseException。返回 PA_EH_UNCAUGHT 时由调用方打印 traceback +
   退出（与 rt_exc.c 的 exc_longjmp 无 handler 分支同一收尾，但这里不做，
   留给上层决定）。 */
int pa_eh_throw(PyAdamasException* e);

/* 表驱动展开（--zero-cost-exc）的进入/离开标记：生成代码在零开销 try 的
   保护区入口/出口调用。> 0 时 exc_longjmp 改走 _Unwind，而不是 longjmp。 */
void pa_eh_enter(void);
void pa_eh_leave(void);
extern int pa_eh_depth;
/* landing pad 入口调用（%rax = 展开对象指针，即 LP 的第一个实参）：取回异常、
   装好 pending_exc / current_exc_inst / pending_exc_args 这些全局状态。
   返回取回的 PyAdamasException*（外来异常返回 NULL：不能按我们的语义分发）。 */
PyAdamasException* pa_eh_landing(void* unwound);
/* landing pad 处理完（命中 except / 抑制）后收尾：删掉展开对象，幂等。
   异常本体不在这里释放——它已经是 pending_exc/current_exc，归 rt_exc.c 的
   exc_free 生命周期管。 */
void pa_eh_done(void);
/* 最近一次抛出、尚未收尾的展开对象（不透明指针；调试与断言用）。 */
extern void* pa_eh_current;
extern PyAdamasList* loop_ready;
extern PyAdamasList* loop_timers;
extern PyAdamasList* loop_pending;
extern PyAdamasList* loop_fds;
extern void* loop_roots[4];
extern int loop_roots_registered;
extern int loop_running;
extern void* loop_root;
extern int loop_root_state;
extern int64_t loop_root_result;
extern PyAdamasException* loop_root_exc;
extern void** gc_globals;
extern int64_t gc_n_globals;
extern int64_t gc_globals_cap;
extern uintptr_t gc_stack_top;
extern PyAdamasRc** gc_work;
extern size_t gc_work_len;
extern PyAdamasList* sys_argv_list;

size_t rc_index_slot(PyAdamasRc* h);
void rc_index_rehash(size_t ncap);
void rc_index_put(PyAdamasRc* h);
void rc_index_del(PyAdamasRc* h);
int rc_index_has(PyAdamasRc* h);
int64_t pa_rc_inc_unknown(int64_t v);
void pa_rc_dec_unknown(int64_t v);
/* 0.302：unknown 值的共享堆探针（实体在 rt_core.c）。与 pa_rc_dec_unknown
   的「该不该 dec」判定逐位一致：常规模式查 rc_index，exact-RC 模式走
   对齐 + 堆链回退。dict 的 saw_heap 与 dec_unknown 共用这一份判定，
   不复制逻辑。内联快路廉价排除小整数（与 inc/dec_unknown 同阈值），
   标量不进函数调用。 */
int pa_unknown_is_heap_slow(int64_t v);
/* exact-RC 模式标志（rt_core.c）：索引关闭时探针要走堆链回退。 */
extern int rc_index_off;
static inline int pa_unknown_is_heap(int64_t v) {
    if (!v || (uint64_t)v < 65536) return 0;
    /* 0.303：常规模式快路内联 —— 直接查索引，命中即返回，与旧 list_probe
       同形态（一层调用）。否则 prep/append 每次探针多一层 slow 调用，
       str_build 8M 次 append 实测 +1.4% 指令 / +5.9% 耗时。判定与 slow
       逐位一致：slow 入口同样先查索引，exact-RC 才进回退。 */
    PyAdamasRc* h = (PyAdamasRc*)(uintptr_t)((uint64_t)v - (uint64_t)PYADAMAS_RC_HDR);
    if (rc_index_has(h)) return 1;
    if (!rc_index_off) return 0;
    return pa_unknown_is_heap_slow(v);
}
void panic_c(const char* msg);
void rc_link(PyAdamasRc* h);
void rc_unlink(PyAdamasRc* h);
/* 0.303：把 untracked 对象（kind0 小元组）补进 GC 堆链。kind0 容器经
   unknown 写入升级成 kind2 后有了堆出边，可能成环，GC 必须能看到它。
   已 tracked 的对象是空操作。不做 retain —— 只改 GC 可见性。 */
void pa_rc_track(void* p);
void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*), int64_t kind);
void* rc_alloc_untracked(int64_t size, void (*dtor)(void*), void (*release)(void*), int64_t kind);
/* extra：payload 尾部额外内联的字节数（目前只用于小列表的 data 缓冲）。
   h->size = size + extra，释放时按 size 路由回对应的池。 */
void* rc_alloc_ex(int64_t size, void (*dtor)(void*), void (*release)(void*), int64_t kind,
                  int64_t extra);
void* rc_alloc_untracked_ex(int64_t size, void (*dtor)(void*), void (*release)(void*),
                            int64_t kind, int64_t extra);
/* ── 内联引用计数（方案 A：抄 Swift ARC 的编译期内联思路）──────
 *
 * 原来 inc/dec 都是跨编译单元的函数调用：实测 bench_rc_gen 有 30 万次
 * inc + 96 万次 dec，每次都要 call/ret + 保存寄存器，开销可观。
 *
 * Swift 的 ARC 是编译器直接内联展开 inc/dec 的（没有运行时调用），
 * CPython 的 Py_INCREF 也是 static inline。这里照做：
 *
 *   - 快路径（计数 > 1 且不在收集期）：头文件内联，几条指令完成；
 *   - 慢路径（归零要释放 / 收集期 / 统计模式）：调 pa_rc_*_slow。
 *
 * 关键设计：快路径**不碰** rc_stats_on 和 gc_phase 之外的状态，
 * 保证编译器能把整个判断序列放在调用点，不产生额外分支预测失败。 */
void pa_rc_inc_slow(void* p);
void pa_rc_dec_slow(void* p);
extern int pa_rc_stats_on_fast;   /* rc_stats_on 的别名，供内联使用 */
extern int pa_gc_phase_fast;      /* gc_phase 的别名 */
extern int pa_gc_incr_mark_fast;  /* 增量标记总开关别名（写屏障门控，0.330） */
void pa_gc_write_barrier(int64_t v);  /* Dijkstra 插入屏障：标灰新值（标记期生效） */
/* 门控插入屏障：flag-off 一次可预测分支跳过（零调用）；flag-on 标灰新值。
   非堆值（标量）进 pa_gc_write_barrier 后查集合表落空，安全 no-op。 */
static inline void pa_gc_insert_barrier(int64_t v) {
    if (__builtin_expect(pa_gc_incr_mark_fast != 0, 0)) pa_gc_write_barrier(v);
}

#ifndef PA_RC_NO_INLINE
static inline void pa_rc_inc_fast(void* p) {
    if (__builtin_expect(pa_rc_stats_on_fast != 0, 0) ||
        __builtin_expect(p == NULL, 0)) {
        pa_rc_inc_slow(p);
        return;
    }
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    // 永生对象（字面量）：rc 是 INT64_MIN，直接跳过。
    if (__builtin_expect(h->rc == PYADAMAS_RC_IMMORTAL, 0)) return;
    if (__builtin_expect(h->rc <= 0, 0)) {
        pa_rc_inc_slow(p);   /* 让慢路径报错 */
        return;
    }
    h->rc++;
}
#endif /* !PA_RC_NO_INLINE */

#ifndef PA_RC_NO_INLINE
static inline void pa_rc_dec_fast(void* p) {
    if (__builtin_expect(pa_rc_stats_on_fast != 0, 0) ||
        __builtin_expect(pa_gc_phase_fast != 0, 0) ||
        __builtin_expect(p == NULL, 0)) {
        pa_rc_dec_slow(p);
        return;
    }
    PyAdamasRc* h = (PyAdamasRc*)p - 1;
    if (__builtin_expect(h->rc == PYADAMAS_RC_IMMORTAL, 0)) return;
    // 快路径只处理「减完仍 > 0」的情况；归零必须走慢路径做释放。
    if (__builtin_expect(h->rc > 1, 1)) {
        h->rc--;
        return;
    }
    pa_rc_dec_slow(p);
}
#endif /* !PA_RC_NO_INLINE */

/* **内部调用走内联快路径。**
 *
 * 运行时自己的 .c 里有一千多处 pa_rc_inc/pa_rc_dec 调用，全部改成内联展开；
 * 只有 rt_core.c（定义实体符号的地方）例外，它要提供生成代码调用的外部符号。
 * 放在这里（而不是文件末尾）是因为下面的 pa_gc_pin 等 inline 函数也要用它。 */
#ifndef PA_RC_NO_INLINE
#define pa_rc_inc(p) pa_rc_inc_fast(p)
#define pa_rc_dec(p) pa_rc_dec_fast(p)
#else
/* PA_RC_NO_INLINE（rt_core.c）：调用外部实体符号。 */
void pa_rc_inc(void* p);
void pa_rc_dec(void* p);
#endif

int64_t pa_rc_mark(void);
void pa_rc_temp(void* p);
void pa_rc_temp_unknown(int64_t v);
void pa_rc_temp_take(void* p);
void pa_rc_cleanup(int64_t mark);

/* ── 构造期 GC pin：C 局部变量里的半成品挂成根 ─────────────────────
   背景：循环收集器的根集合只有四处——全局根表（pa_gc_add_roots）、临时值
   栈、帧记录登记区、寄存器。C 运行时函数里「刚分配、还没交回调用方」的
   对象只存在于 C 局部变量里，这四处都看不见；若两次分配之间恰好触发主
   收集（major），它会被当成垃圾清扫，函数拿到的就是悬空指针。
   0.181 修掉的异常值构造 UAF（runtime/rt_exc.c）就是这一类；本助手把那次
   手写的 pa_rc_temp / pa_rc_temp_take 组合标准化，供全 runtime 复用。

   语义（务必按此使用）：
     - pa_gc_pin(p) 临时补一个引用并把 p 挂到临时值栈（临时值栈是 GC 根），
       **不转移所有权**：pin 之前调用方持有的引用在 unpin 之后原样还在。
       所以 p 既可以是「自己刚分配、持有 rc=1」的对象，也可以是借来的参数。
     - pa_gc_unpin(p) 必须与同一对象的 pa_gc_pin(p) 成对；重复 pin 同一对象
       会各加一个引用，按相反顺序 unpin 即可（不强制 LIFO，按值配对）。
     - pin 期间 GC 会从 p 继续标记它的整棵出边子图，后代也跟着活；
       因此「容器 + 逐个填充」的模式只需 pin 容器一次。
     - 异常安全：pin 加的引用落在临时值栈上，longjmp 抛出时由 handler 的
       临时栈水位（pa_rc_cleanup）统一释放，不会泄漏、也不会少放。
     - 只用于函数内的短期保命；全局槽的长期根用 pa_gc_add_roots(槽地址)。
   典型用法（构造一个列表并在循环里继续分配）：
       PyAdamasList* out = pa_list_new(n, 1);
       pa_gc_pin(out);
       for (int64_t i = 0; i < n; ++i) sm_push_str(out, str_new(...));
       pa_gc_unpin(out);
       return out;
   pin 本身要两次 rc 操作，只在构造点用，不要放进逐元素热路径。 */
static inline void pa_gc_pin(void* p) {
    if (!p) return;
    pa_rc_inc(p);
    pa_rc_temp(p);
}


static inline void pa_gc_unpin(void* p) {
    if (!p) return;
    pa_rc_temp_take(p);
    pa_rc_dec(p);
}

/* ── RAII 作用域守卫（抄 C++/Rust 的 RAII 思路，用 GCC cleanup 属性）──
 *
 * 问题：现在每个「需要保命的对象」都要手工配对 pin/unpin。jp_value 有
 * 11 个 return，其中 6 个需要清理 —— 靠人工保证「每条路径都配对」是
 * 易错的，而且新加一条路径时很容易忘。
 *
 * RAII 的做法：让清理跟着**作用域**走，而不是跟着控制流走。
 * C 没有析构函数，但 GCC/Clang 有 __attribute__((cleanup(fn)))：
 * 变量离开作用域时（正常 return、break、甚至 longjmp 之外的任何方式）
 * 自动调用 fn(&var)。
 *
 * 用法：
 *     PyAdamasList* out = pa_list_new(0, 1);
 *     PA_PIN_SCOPE(out);        // 自动 pin，离开作用域自动 unpin
 *     ... 任意多条 return / 抛异常 ...
 *     return out;               // 若要把所有权交出去，先 PA_PIN_RELEASE(out)
 *
 * 注意 longjmp：cleanup 属性**不会**在 longjmp 时执行（那是未定义行为）。
 * 所以异常路径仍依赖临时值栈的 pa_rc_cleanup —— 这正是 pin 把引用挂到
 * 临时栈的原因：两层保障，RAII 管正常路径，临时栈管异常路径。 */

/* 内部：cleanup 回调。参数是变量地址。 */
static inline void pa_pin_cleanup_(void* pp) {
    void* p = *(void**)pp;
    if (p) pa_gc_unpin(p);
}

/* 声明一个受 RAII 管理的指针变量并立即 pin。
   变量名固定为 _pa_pin_<name>，避免与用户变量冲突。 */
#define PA_PIN_SCOPE(var)                                            \
    void* _pa_pin_##var __attribute__((cleanup(pa_pin_cleanup_), unused)) = \
        (void*)(var);                                                \
    pa_gc_pin((void*)(var))

/* 放弃 RAII 管理（所有权已经转给别人，不该再 unpin）。 */
#define PA_PIN_RELEASE(var) (_pa_pin_##var = NULL)

/* ── 通用 free 守卫：管理 malloc/calloc 出来的裸内存 ─────────────
 *
 * 和 PA_PIN_SCOPE 同样的思路，只是清理动作是 free 而不是 unpin。
 * 用途：函数里有多条 return 路径、中途还有 malloc 的数组要释放时，
 * 手工在每个出口 free 一遍容易漏（尤其是后加的出口）。 */
static inline void pa_free_cleanup_(void* pp) {
    void* p = *(void**)pp;
    free(p);
}

#define PA_FREE_SCOPE(var)                                          \
    void* _pa_free_##var __attribute__((cleanup(pa_free_cleanup_), unused)) = \
        (void*)(var)

/* 放弃 free 管理（所有权转移给调用方，或者已经被手工 free）。 */
#define PA_FREE_RELEASE(var) (_pa_free_##var = NULL)
void pa_rc_shutdown(void);
void pa_register_literals(void** objs, int64_t n);
void closure_destroy(void* p);
void closure_gc_release(void* p);
void* pa_closure_new(int64_t code, int64_t n_caps);
PyAdamasStr* str_alloc_raw(int64_t len);
PyAdamasStr* str_seal(PyAdamasStr* s);
PyAdamasStr* str_seal_n(PyAdamasStr* s, int64_t pub);
PyAdamasStr* str_new(const char* p, int64_t len);
PyAdamasStr* str_new_bytes(const char* p, int64_t len);
PyAdamasStr* str_new_ascii(const char* p, int64_t len);
int64_t str_count_cp(const char* p, int64_t n);
int str_seq_len(unsigned char b);
int64_t str_prev_cp(const PyAdamasStr* s, int64_t off);
int64_t str_cp_to_byte(const PyAdamasStr* s, int64_t cp);
uint32_t str_cp_at(const PyAdamasStr* s, int64_t off, int* plen);
int str_cp_bytes(uint32_t cp);
int str_cp_encode(uint32_t cp, char* out);
/* 迭代游标：顺序访问时把「上一个码点下标 + 下一个字节偏移」记在槽里，
   命中 i == cp+1 就是 O(1)，否则退回从头扫。 */
PyAdamasStr* pa_str_char_at_cursor(PyAdamasStr* s, int64_t i, PyAdamasStrCursor* cur);
/* cimport 外部函数返回 char* 时复制成 PyAdamasStr；NULL 对前者视为空串，
   对后者返回 NULL（PyAdamas 的 None）。 */
PyAdamasStr* pa_str_from_cstr(const char* p);
PyAdamasStr* pa_str_from_cstr_opt(const char* p);
void exc_free(PyAdamasException* e);
PyAdamasException* exc_new(const char* type, const char* msg);
PyAdamasException* exc_clone(PyAdamasException* e);
const char* exc_parent(const char* name);
int exc_is_a(const char* child, const char* want);
int pa_exc_is_registered(const char* name);
void exc_print_uncaught(PyAdamasException* e);
/* PEP 479：生成器/异步生成器/协程帧里逃出的 StopIteration 在 CPython 3.7+
   会转成 RuntimeError（StopIteration 子类同样转换）。就地改类型与消息，
   不是 StopIteration 时什么也不做。实现在 rt_gen.c。 */
void pa_exc_pep479(PyAdamasException* e, const char* msg);
_Noreturn void exc_longjmp(void);
void* pa_handler_push(void);
void pa_handler_leave(void* p);
void pa_handler_discard(void* p);
void pa_handler_suspend(void* p);
void pa_handler_rearm(void* p);
void pa_handler_free(void* p);
int64_t pa_exc_matches(PyAdamasStr* want);
PyAdamasStr* pa_exc_accept(void);
PyAdamasExcValue* pa_exc_accept_value(void);
PyAdamasExcValue* pa_exc_value_new(PyAdamasStr* type, PyAdamasStr* msg);
PyAdamasExcValue* pa_exc_value_from_exc(PyAdamasException* e);
PyAdamasException* pa_exc_from_value(PyAdamasExcValue* v);
void pa_raise_value(PyAdamasExcValue* v);
void pa_raise_value_from(PyAdamasExcValue* v, PyAdamasExcValue* cause);
PyAdamasExcValue* pa_exc_value_cause(PyAdamasExcValue* v);
void pa_exc_register(PyAdamasStr* child, PyAdamasStr* parent);
PyAdamasList* pa_exc_value_args(PyAdamasExcValue* v);
PyAdamasStr* pa_exc_value_str(PyAdamasExcValue* v);
PyAdamasStr* pa_exc_value_type_name(PyAdamasExcValue* v);
void pa_exc_done(void);
// 生成器/协程 resume 的异常上下文切换：进入时把调用方的 current_exc /
// exc_save_top 压栈并装上本帧保存的；挂起/退出时反过来。token 是进入时
// 的栈深度，leave 只弹到它（没压过就什么也不弹）。
int64_t pa_exc_enter(int64_t* cur, int64_t* top);
void pa_exc_leave(int64_t* cur, int64_t* top, int64_t token);
int64_t pa_exc_depth(void);
void pa_exc_unwind(int64_t depth);
void pa_exc_clear(void);
PyAdamasStr* pa_exc_type_name(void);
PyAdamasStr* pa_exc_message(void);
void pa_raise(PyAdamasStr* type, PyAdamasStr* msg);
/* 同 pa_raise，但接管 type/msg 的所有权（拷进异常对象后 dec 它们）。 */
void pa_raise_own(PyAdamasStr* type, PyAdamasStr* msg);
_Noreturn void pa_raise_args(PyAdamasStr* type, PyAdamasList* args);
PyAdamasExcValue* pa_exc_value_new_args(PyAdamasStr* type, PyAdamasList* args);
void pa_reraise(void);
void pa_exc_reraise(void);
PyAdamasExcValue* pa_exc_take_value(void);
void pa_raise_instance(void* inst, PyAdamasStr* type); /* 抛富异常实例（noreturn） */
void* pa_exc_accept_instance(void);                      /* except 取富实例，无则 0 */
void pa_exc_reraise_value_if(PyAdamasExcValue* v);
_Noreturn void pa_raise_c(const char* type, const char* msg);
void gen_destroy(void* g);
void* pa_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind);
int64_t gen_run(void* g, int64_t sent, int* done);
int64_t pa_gen_next(void* g);
int64_t pa_gen_next_opt(void* g, int64_t* ok);
int64_t pa_gen_send(void* g, int64_t sent);
int64_t pa_gen_send_opt(void* g, int64_t sent, int64_t* ok);
/* gen.throw(exc)：在挂起点注入异常；已结束则把异常抛给调用方。 */
int64_t pa_gen_throw(void* g, PyAdamasExcValue* exc);
/* yield from 转发用：exc 是 owned 的 PyAdamasException*（转交帧）。
   子生成器产出写 ok=1，结束写 ok=0 并返回它的 return 值。 */
int64_t pa_gen_throw_opt(void* g, int64_t exc, int64_t* ok);
void pa_gen_close(void* g);
void pa_gen_finalize(void* g);
/* 释放帧里所有持有引用的槽（调用生成的 gcrel，槽清零后可重复调用）。 */
void pa_gen_release_slots(void* g);
int64_t now_ms(void);
int frame_is_coro(void* p);
int frame_is_agen(void* p);
int is_await_obj(void* p);
void await_destroy(void* p);
void await_gc_release(void* p);
void free_attached_exception(void* payload, int64_t size);
PyAdamasAwait* await_new(int64_t kind);
void loop_ensure(void);
void ready_push(void* c);
void* ready_pop(void);
int64_t list_take_at(PyAdamasList* l, int64_t i);
void list_clear_owned(PyAdamasList* l);
void* pending_take(void* w);
/* 调用一个 pyadamas 函数值（闭包首字是代码地址，%r10 是环境），单参数。 */
void* pa_call1(void* fn, int64_t arg);
void finish_gather(PyAdamasAwait* g);
void gather_child_fail(PyAdamasAwait* g, PyAdamasException* e);
void start_gather(PyAdamasAwait* w, void* waiter);
void start_awaitable(void* obj, void* waiter);
void deliver_owned(void* waiter, void* child, int64_t value, PyAdamasException* exc,
                   int64_t value_kind);
void coro_inject(void* c, PyAdamasException* e);
void coro_finish(void* c, int64_t value, PyAdamasException* exc);
void await_from_coro(void* c, void* w);
void coro_step(void* c);
void pa_coro_rethrow(void* g);
int fire_due_timers(int64_t now);
int64_t earliest_deadline(void);
void* pa_asyncio_sleep(double secs);
void* pa_asyncio_gather(PyAdamasList* coros, int64_t result_kind);
void detach_cur(void* c);
void cancel_suspended(void* c, PyAdamasException* e);
void* pa_asyncio_create_task(void* coro, int64_t result_kind);
int64_t pa_asyncio_task_done(void* t);
int64_t pa_asyncio_task_cancel(void* t);
void gather_cancel(PyAdamasAwait* g);
void future_cancel(PyAdamasAwait* f);
void simple_wait_cancel(PyAdamasAwait* w);
void cancel_chain(void* w);
void future_settle(PyAdamasAwait* f, int64_t value, PyAdamasException* exc);
void* pa_asyncio_future_new(void);
void pa_future_set_result(void* fp, int64_t value);
int64_t pa_future_done(void* fp);
int64_t pa_future_cancelled(void* fp);
int64_t pa_future_cancel(void* fp);
void raise_exception_copy(PyAdamasException* e, const char* fallback);
int64_t pa_future_result(void* fp);
PyAdamasExcValue* pa_future_exception(void* fp);
int64_t pa_asyncio_task_result(void* tp);
PyAdamasExcValue* pa_asyncio_task_exception(void* tp);
int64_t pa_asyncio_task_cancelled(void* tp);
void pa_list_del_index(PyAdamasList* l, int64_t i);
void* pa_event_new(void);
void pa_event_set(void* e);
int64_t pa_event_is_set(void* e);
void* pa_event_wait(void* e);
void* pa_lock_new(void);
void* pa_lock_acquire(void* l);
void pa_lock_release(void* l);
int64_t pa_lock_locked(void* l);
void* pa_queue_new(int64_t maxsize, int64_t kind);
void* pa_queue_put(void* q, int64_t v);
void* pa_queue_get(void* q);
int64_t pa_queue_qsize(void* q);
int64_t pa_queue_empty(void* q);
int64_t pa_queue_full(void* q);
void* pa_queue_join(void* q);
void pa_queue_task_done(void* q);
void pa_queue_put_nowait(void* q, int64_t v);
int64_t pa_queue_get_nowait(void* q);
void* pa_timeout_new(double secs);
void pa_timeout_enter(void* t);
int64_t pa_timeout_exit(void* t);
int64_t pa_timeout_expired(void* t);
void* pa_taskgroup_new(void);
void* pa_taskgroup_add(void* g, void* coro);
void* pa_taskgroup_exit(void* g);
void pa_taskgroup_cancel(void* g);
void* pa_sem_new(int64_t value);
void* pa_sem_acquire(void* s);
void pa_sem_release(void* s);
int64_t pa_sem_locked(void* s);
void* pa_asyncio_wait_for(void* aw, double secs);
void wait_for_cancel(PyAdamasAwait* w);
void* pa_asyncio_wait_fd(int64_t fd, int64_t write);
int poll_fds(int timeout_ms);
void* pa_asyncgen_anext(void* gen);
void* pa_asyncgen_asend(void* gen, int64_t sent);
void* pa_asyncgen_aclose(void* gen);
void* pa_asyncgen_athrow(void* gen, PyAdamasExcValue* exc);
void driver_complete(PyAdamasAwait* d, int64_t value, PyAdamasException* exc);
void agen_drive(PyAdamasAwait* d);
int64_t pa_asyncio_run(void* coro);
void format_double(char* buf, size_t n, double v);
PyAdamasStr* pa_str_concat(PyAdamasStr* a, PyAdamasStr* b);
PyAdamasStr* pa_repr_tuple(PyAdamasList* t, int64_t kind);
PyAdamasStr* pa_repr_any(int64_t v);
PyAdamasStr* pa_repr_set(void* s);
PyAdamasStr* pa_repr_bytes(void* s);
PyAdamasStr* pa_repr_dict_any(PyAdamasDict* d);
PyAdamasStr* pa_str_char_at(PyAdamasStr* s, int64_t i);
PyAdamasStr* pa_str_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step);
PyAdamasStr* pa_bytes_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step);
PyAdamasStr* pa_bytes_concat(PyAdamasStr* a, PyAdamasStr* b);
PyAdamasStr* pa_bytes_repeat(PyAdamasStr* s, int64_t n);
int64_t pa_len_bytes(PyAdamasStr* s);
/* 字典/集合的迭代版本号（插入新键或删除键时 +1）。 */
int64_t pa_dict_version(PyAdamasDict* d);
PyAdamasStr* pa_str_encode(PyAdamasStr* s);
PyAdamasStr* pa_bytes_decode(PyAdamasStr* s);
PyAdamasStr* pa_str_from_i64(int64_t v);
PyAdamasStr* pa_str_from_u64(uint64_t v);
PyAdamasStr* pa_uint_to_base(uint64_t v, int64_t base);
/* unknown 值：堆字符串原样返回并补引用，其余按整数格式化。 */
PyAdamasStr* pa_str_any(int64_t v);
PyAdamasStr* pa_str_from_f64(double v);
PyAdamasStr* pa_str_from_bool(int64_t v);
PyAdamasStr* pa_str_from_none(void);
int64_t pa_str_to_i64(PyAdamasStr* s, int64_t base);
double pa_str_to_f64(PyAdamasStr* s);
int64_t pa_ord(PyAdamasStr* s);
PyAdamasStr* pa_chr(int64_t c);
PyAdamasStr* pa_int_to_base(int64_t v, int64_t base);
double pa_round_f64(double x, int64_t ndigits);
int64_t pa_round_i64(int64_t v, int64_t ndigits);
int64_t pa_str_eq(PyAdamasStr* a, PyAdamasStr* b);
int64_t pa_str_cmp(PyAdamasStr* a, PyAdamasStr* b);
/* 字符串方法派发：op 序号与 src/ir.h 的 StrOp 一致；未用的参数传 0。 */
int64_t pa_str_method(PyAdamasStr* s, int64_t op, int64_t a0, int64_t a1, int64_t a2);
int64_t pa_bytes_method(PyAdamasStr* s, int64_t op, int64_t a0, int64_t a1, int64_t a2);

/* 文件对象（布局只在 rt_obj.c 里定义） */
typedef struct PyAdamasFile PyAdamasFile;
PyAdamasFile* pa_file_open(PyAdamasStr* path, int64_t fd, PyAdamasStr* mode, int64_t buffering,
                         int64_t newline, int64_t closefd);
void pa_file_config(PyAdamasFile* f, PyAdamasStr* encoding, PyAdamasStr* errors);
PyAdamasStr* pa_file_encoding(PyAdamasFile* f);
PyAdamasStr* pa_file_errors(PyAdamasFile* f);
PyAdamasStr* pa_file_read(PyAdamasFile* f, int64_t n);
PyAdamasStr* pa_file_readline(PyAdamasFile* f, int64_t size);
PyAdamasList* pa_file_readlines(PyAdamasFile* f, int64_t hint);
int64_t pa_file_write(PyAdamasFile* f, PyAdamasStr* s);
void pa_file_writelines(PyAdamasFile* f, PyAdamasList* lines);
int64_t pa_file_seek(PyAdamasFile* f, int64_t off, int64_t whence);
int64_t pa_file_tell(PyAdamasFile* f);
int64_t pa_file_truncate(PyAdamasFile* f, int64_t size);
int64_t pa_file_fileno(PyAdamasFile* f);
int64_t pa_file_isatty(PyAdamasFile* f);
int64_t pa_file_readable(PyAdamasFile* f);
int64_t pa_file_writable(PyAdamasFile* f);
int64_t pa_file_seekable(PyAdamasFile* f);
int64_t pa_file_closed(PyAdamasFile* f);
PyAdamasStr* pa_file_name(PyAdamasFile* f);
PyAdamasStr* pa_file_mode(PyAdamasFile* f);
void pa_file_close(PyAdamasFile* f);
void pa_file_flush(PyAdamasFile* f);
PyAdamasStr* pa_input(PyAdamasStr* prompt);
PyAdamasFile* pa_sys_stream(int64_t which);
void list_destroy(void* p);
void list_gc_release(void* p);
/* kind-1 容器元素批量 dec：内联快路径 + untracked 元组释放；
   统计/收集/诊断模式自动回退逐元素原路径。 */
void pa_rc_dec_heap_batch(int64_t* vals, int64_t n);
/* 把 data 缓冲增长到 ncap 个元素并更新 l->data / l->cap。
   内联缓冲不能 realloc（块内指针），自动换成 malloc + memcpy。 */
int64_t* list_buf_grow(PyAdamasList* l, int64_t ncap);
PyAdamasList* pa_list_new(int64_t cap, int64_t elem_kind);
/* 定长元组构造（元素个数 <= 4）。elem_kind == 0 时用 untracked 分配：
   元组不可变且元素全非堆 -> 不可能成环 -> 不必进 GC 堆链。 */
PyAdamasList* pa_tuple_new_filled(int64_t n, int64_t elem_kind, int64_t a, int64_t b,
                                  int64_t c, int64_t d);
PyAdamasRange* pa_range_new(int64_t start, int64_t stop, int64_t step);
int64_t pa_range_len(PyAdamasRange* r);
int64_t pa_range_first(PyAdamasRange* r);
int64_t pa_range_step(PyAdamasRange* r);
int64_t pa_range_stop(PyAdamasRange* r);
int64_t pa_range_get(PyAdamasRange* r, int64_t i);
int64_t pa_range_contains(PyAdamasRange* r, int64_t v);
int64_t pa_range_eq(PyAdamasRange* a, PyAdamasRange* b);
int64_t pa_range_sum_i64(PyAdamasRange* r, int64_t start);
double pa_range_sum_f64(PyAdamasRange* r, double start);
int64_t pa_range_min_i64(PyAdamasRange* r);
int64_t pa_range_max_i64(PyAdamasRange* r);
int64_t pa_range_any(PyAdamasRange* r);
int64_t pa_range_all(PyAdamasRange* r);
PyAdamasList* pa_range_to_list(PyAdamasRange* r);
PyAdamasStr* pa_range_repr(PyAdamasRange* r);
void pa_list_append(PyAdamasList* l, int64_t v, int64_t heap);
/* for k in range(lo, hi): dst.append(src[k]) 的批量版（降级器惯用法识别）。
   heap 是源元素静态种类 0/1/2；语义与逐元素 prep+append 一致。 */
void pa_list_copy_range(PyAdamasList* dst, PyAdamasList* src, int64_t lo, int64_t hi,
                        int64_t heap);
/* 空列表第一次写入前定型：vk = 0 非堆 / 1 堆 / 2 unknown；owned 非 0
   表示引用已经转给容器（新值），不再 retain。 */
void pa_list_prep(PyAdamasList* l, int64_t v, int64_t vk, int64_t owned);
int64_t pa_list_get(PyAdamasList* l, int64_t i);
/* 0.303：heap 是新值的静态种类（0 非堆 / 1 堆 / 2 unknown）。写入时维护
   elem_kind / heap_bits：静态容器类型是 unknown 时降级器不做 prep，运行期
   kind 可能与新值冲突（kind1 列表存标量），不升级会在销毁时把标量当堆指针
   dec。heap==2 才运行期探针（pa_unknown_is_heap），已知标量不探针 ——
   大整数/浮点位模式可能恰好等于某个堆地址，探针会误判。 */
void pa_list_set(PyAdamasList* l, int64_t i, int64_t v, int64_t heap);
/* 下标类写入（set/insert）共用的种类/位图维护。不做 retain/release，
   所有权由调用方负责。kind 冲突升级 2；kind0 untracked 容器补进 GC 链；
   kind2 带位图时按新值堆性更新第 i 位。 */
void pa_list_store_kind(PyAdamasList* l, int64_t i, int64_t v, int64_t heap);
/* 丢弃 kind2 的逐元素位图（回退注册表判断）。切片赋值/insert/del/reverse/
   sort/shuffle 会移动或覆盖元素，位图无法逐位维护时保守丢弃。 */
void pa_list_drop_bits(PyAdamasList* l);
void pa_list_set_len(PyAdamasList* l, int64_t n);
void pa_list_extend(PyAdamasList* dst, PyAdamasList* src);
/* `l *= n`：原地重复（CPython list.__imul__ 语义：同一个对象扩容，
   别名也看得见）。n<=0 清空。 */
void pa_list_inplace_repeat(PyAdamasList* l, int64_t n);
PyAdamasList* pa_list_copy(PyAdamasList* src);
PyAdamasList* pa_list_repeat(PyAdamasList* src, int64_t n);
PyAdamasStr* pa_str_repeat(PyAdamasStr* s, int64_t n);
PyAdamasStr* pa_sha256_digest(PyAdamasStr* data);
PyAdamasStr* pa_sha256_hex(PyAdamasStr* data);
PyAdamasStr* pa_b64encode(PyAdamasStr* data, int64_t urlsafe);
PyAdamasStr* pa_b64decode(PyAdamasStr* data);
PyAdamasList* pa_list_reversed(PyAdamasList* src);
/* 0.312：kind 4 = 容器元素按 desc 字典序（描述符串常量，标量种类传空串）。 */
void pa_list_sort(PyAdamasList* l, int64_t kind, PyAdamasStr* desc);
PyAdamasList* pa_sorted_list(PyAdamasList* src, int64_t kind, PyAdamasStr* desc);
int64_t pa_list_index(PyAdamasList* l, int64_t v, int64_t kind);
int64_t pa_list_count(PyAdamasList* l, int64_t v, int64_t kind);
/* 0.303：heap 同 pa_list_set —— 新值静态种类，插入时维护种类/位图。 */
void pa_list_insert(PyAdamasList* l, int64_t i, int64_t v, int64_t heap);
void pa_list_reverse(PyAdamasList* l);
int64_t pa_int_bit_length(int64_t v);
PyAdamasList* pa_enumerate_list(PyAdamasList* seq);
PyAdamasList* pa_zip2_list(PyAdamasList* a, PyAdamasList* b);
PyAdamasDict* pa_dict_from_pairs(PyAdamasList* src, int64_t key_kind, int64_t key_own,
                              int64_t val_own, int64_t hash_slot, int64_t eq_slot);
PyAdamasDict* pa_set_from_list(PyAdamasList* src, int64_t key_kind, int64_t key_own,
                             int64_t hash_slot, int64_t eq_slot);
int64_t pa_list_sum_i64(PyAdamasList* l, int64_t start);
double pa_list_sum_f64(PyAdamasList* l, double start);
int64_t pa_list_min_i64(PyAdamasList* l);
int64_t pa_list_max_i64(PyAdamasList* l);
uint64_t mix64(uint64_t x);
int64_t dict_hash_value(int64_t kind, int64_t v);
int dict_key_eq(int64_t kind, int64_t a, int64_t b);
void dict_destroy(void* p);
void dict_gc_release(void* p);
PyAdamasDict* pa_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own,
                        int64_t hash_slot, int64_t eq_slot);
void pa_dict_set(PyAdamasDict* d, int64_t key, int64_t val);
int64_t pa_dict_get(PyAdamasDict* d, int64_t key);
void pa_dict_set_default_factory(PyAdamasDict* d, int64_t factory);
/* 0.284：contains+get 融合。命中返回 1 并把借用值写入 *out；缺失返回 0
   不写 out。值所有权留在 dict（与 pa_dict_get 的借用语义一致）。 */
int64_t pa_dict_try_get(PyAdamasDict* d, int64_t key, int64_t* out);
int64_t pa_dict_contains(PyAdamasDict* d, int64_t key);
int64_t pa_dict_key_at(PyAdamasDict* d, int64_t i);
int64_t pa_dict_val_at(PyAdamasDict* d, int64_t i);
int64_t pa_dict_get_or(PyAdamasDict* d, int64_t key, int64_t def);
int64_t pa_dict_get_opt(PyAdamasDict* d, int64_t key);
int own_to_list_kind(int64_t own);
PyAdamasList* pa_dict_keys(PyAdamasDict* d);
PyAdamasList* pa_dict_values(PyAdamasDict* d);
PyAdamasList* pa_dict_items(PyAdamasDict* d);
int64_t pa_dict_eq(PyAdamasDict* a, PyAdamasDict* b, PyAdamasStr* chain);
int64_t chain_set_eq(PyAdamasDict* a, PyAdamasDict* b, const uint8_t* chain, int64_t len,
                     int64_t depth);
int64_t pa_set_eq(PyAdamasDict* a, PyAdamasDict* b);
void pa_dict_update(PyAdamasDict* dst, PyAdamasDict* src);
/* set 的原地运算（`s &= t` / `s -= t` / `s ^= t`）：set 复用 dict。 */
void pa_set_intersect_update(PyAdamasDict* s, PyAdamasDict* t);
void pa_set_difference_update(PyAdamasDict* s, PyAdamasDict* t);
PyAdamasDict* pa_set_union(PyAdamasDict* a, PyAdamasDict* b);
PyAdamasDict* pa_set_intersection(PyAdamasDict* a, PyAdamasDict* b);
PyAdamasDict* pa_set_difference(PyAdamasDict* a, PyAdamasDict* b);
PyAdamasDict* pa_set_symdiff(PyAdamasDict* a, PyAdamasDict* b);
void pa_set_symdiff_update(PyAdamasDict* s, PyAdamasDict* t);
PyAdamasDict* pa_dict_copy(PyAdamasDict* src);
int64_t pa_isinstance(void* obj, int64_t class_id);
int64_t pa_obj_class_id(void* obj);
int64_t pa_is_named_class(void* obj, PyAdamasStr* qualname);

/* 运行期类对象：类名当值、类装饰器、isinstance 的第二个实参。
   cid 是本模块类编号；跨模块类用 -1 + 限定名。ctor 是构造包装函数
   地址（函数值闭包）；vtable 指向本类的静态 vtable，动态 classmethod
   分派用。name 是 owned 字符串。 */
#define PYADAMAS_CLASS_MAGIC 0x434c415300000001LL
typedef struct {
    int64_t magic;
    int64_t cid;
    int64_t ctor;
    int64_t* vtable;
    PyAdamasStr* name;
} PyAdamasClass;

void* pa_class_new(int64_t cid, int64_t ctor, int64_t* vtable, PyAdamasStr* name);
int64_t pa_class_cid(void* cls);
int64_t pa_class_ctor(void* cls);
void* pa_class_method(void* cls, int64_t slot);
/* unknown 域 dunder 分派：vtable 元数据第 7 字指向常量表 [__eq__, __lt__, __hash__, -1]。
   which: 0=__eq__ / 1=__lt__ / 2=__hash__；返回方法代码地址，没有实现返回 NULL。 */
void* pa_instance_dunder_fn(void* obj, int64_t which);
PyAdamasStr* pa_class_name(void* cls);
PyAdamasStr* pa_class_repr(void* cls);
int64_t pa_isinstance_class(void* obj, void* cls);
void* pa_super_lookup(void* obj, PyAdamasStr* after_qualname, int64_t slot,
                      PyAdamasStr* name);
int64_t pa_is_exc(void* obj, PyAdamasStr* type);
int64_t pa_is_seq(void* obj);
int64_t pa_is_dict(void* obj);
int64_t pa_str_eq_unknown(void* obj, PyAdamasStr* s);
int64_t pa_complex_eq_unknown(void* obj, PyAdamasComplex* c);
int64_t pa_is_str(void* obj);
/* 实例动态属性（类带动态属性槽时才有；没有槽按「不存在」处理）。 */
int64_t pa_attr_get(void* obj, PyAdamasStr* name, int64_t dflt);
void pa_attr_set(void* obj, PyAdamasStr* name, int64_t value);
int64_t pa_attr_has(void* obj, PyAdamasStr* name);
PyAdamasDict* pa_attr_dict_view(void* obj);
PyAdamasList* pa_any_iter_list(int64_t v);
/* P2（0.340）：tag 版迭代物化 + unknown dict 的 (key,value) 对（rt_class.c）。 */
PyAdamasList* pa_any_iter_list_t(int64_t v, int64_t tag);
PyAdamasList* pa_any_dict_items(int64_t v, int64_t tag);
/* P4（0.343，PYADAMAS_LANG_GAPS，rt_lang.c）：内建异常类当值 + 零参动态
   方法分派。只被门控开时发射的代码调用；门控关的程序不碰这些入口。 */
void* pa_exc_class_obj(PyAdamasStr* name);
int64_t pa_exc_class_is_a(PyAdamasStr* name, void* cls);
int64_t pa_exc_subclass(void* a, void* b);
PyAdamasExcValue* pa_exc_class_value_new(void* cls, PyAdamasStr* msg);
PyAdamasExcValue* pa_exc_class_value_new_args(void* cls, PyAdamasList* args);
void pa_method_register(PyAdamasStr* qual, PyAdamasStr* meth, int64_t slot);
int64_t pa_method_call0(void* obj, PyAdamasStr* name);
int64_t pa_is_file(void* v);
int64_t pa_is_class_obj(void* v);
/* P4 块2：file 协议泛化的实例侧分派（rt_lang.c）。rt_file.c 的 pa_file_*
   入口在值不是真 file 时委托到这里（FileLike 类实例，方法名→槽登记表）。 */
PyAdamasStr* pa_fileinst_read(void* v, int64_t n);
PyAdamasStr* pa_fileinst_readline(void* v, int64_t n);
PyAdamasList* pa_fileinst_readlines(void* v, int64_t hint);
int64_t pa_fileinst_write(void* v, PyAdamasStr* s);
void pa_fileinst_writelines(void* v, PyAdamasList* l);
int64_t pa_fileinst_seek(void* v, int64_t off, int64_t whence);
int64_t pa_fileinst_tell(void* v);
int64_t pa_fileinst_truncate(void* v, int64_t size);
void pa_fileinst_close(void* v);
void pa_fileinst_flush(void* v);
int64_t pa_fileinst_closed(void* v);
int64_t pa_fileinst_fileno(void* v);
int64_t pa_fileinst_isatty(void* v);
int64_t pa_fileinst_readable(void* v);
int64_t pa_fileinst_writable(void* v);
int64_t pa_fileinst_seekable(void* v);
PyAdamasStr* pa_fileinst_name(void* v);
PyAdamasStr* pa_fileinst_mode(void* v);
PyAdamasStr* pa_fileinst_encoding(void* v);
PyAdamasStr* pa_fileinst_errors(void* v);
/* P2：tag 数组的跨调用侧信道（生成代码在 call 前写、被调序言读并清零）。 */
extern int64_t pa_ptag_in;
extern int64_t pa_ptag_len;
int64_t pa_is_complex(void* obj);
void dict_rebuild(PyAdamasDict* d);
int dict_take(PyAdamasDict* d, int64_t key, int64_t* val_out);
int64_t pa_dict_pop(PyAdamasDict* d, int64_t key, int64_t has_default, int64_t def);
PyAdamasList* pa_dict_popitem(PyAdamasDict* d);
int64_t pa_dict_pop_last(PyAdamasDict* d);
void pa_dict_remove(PyAdamasDict* d, int64_t key, int64_t missing_ok);
void pa_dict_clear(PyAdamasDict* d);
int list_value_eq(int64_t kind, int64_t a, int64_t b);
// 元素比较链：编译器按静态类型生成，运行时按层递归。见 rt_list.c 的说明。
int chain_elem_eq(int kind, int64_t x, int64_t y, const uint8_t* chain, int64_t len,
                  int64_t depth);
int64_t chain_list_eq(PyAdamasList* a, PyAdamasList* b, const uint8_t* chain, int64_t len,
                      int64_t depth);
int64_t chain_dict_eq(PyAdamasDict* a, PyAdamasDict* b, const uint8_t* chain, int64_t len,
                      int64_t depth);
int64_t pa_list_contains(PyAdamasList* l, int64_t v, int64_t kind);
PyAdamasList* pa_list_sorted_by(PyAdamasList* src, PyAdamasList* keys, int64_t kind,
                                PyAdamasStr* desc);
int64_t pa_list_eq(PyAdamasList* a, PyAdamasList* b, PyAdamasStr* chain);
int64_t pa_str_contains(PyAdamasStr* hay, PyAdamasStr* needle);
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step, int64_t* out_start, int64_t* out_stop, int64_t* out_step);
int64_t slice_total(int64_t start, int64_t stop, int64_t step);
PyAdamasList* pa_list_slice(PyAdamasList* src, int64_t start, int64_t stop, int64_t step);
PyAdamasStr* pa_str_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step);
void pa_list_slice_assign(PyAdamasList* dst, int64_t start, int64_t stop, int64_t step, PyAdamasList* src);
void pa_list_slice_del(PyAdamasList* dst, int64_t start, int64_t stop, int64_t step);
PyAdamasList* pa_str_to_list(PyAdamasStr* s);
PyAdamasStr* pa_str_capwords(PyAdamasStr* s);
/* 静态驻留字符串：256 个单字节串以及 "True"/"False"/"None"，均为永生对象。 */
PyAdamasStr* pa_char_str(int64_t c);
PyAdamasStr* pa_cp_str(uint32_t cp);
PyAdamasStr* pa_str_true(void);
PyAdamasStr* pa_str_false(void);
PyAdamasStr* pa_str_none(void);
/* 在 base[0..n) 的 [from, n-m] 区间里找 pat[0..m)，返回起点或 -1。 */
int64_t pa_find_bytes(const char* base, int64_t n, const char* pat, int64_t m,
                        int64_t from);
PyAdamasList* pa_bytes_to_list(PyAdamasStr* s);
PyAdamasStr* pa_bytes_from_list(PyAdamasList* items);
PyAdamasStr* pa_bytes_zeros(int64_t n);
PyAdamasList* pa_str_wrap_list(PyAdamasStr* s);
void* pa_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*));
/* 值类型 struct：[隐藏 RC 头][C 结构体 payload]，返回 payload 地址。 */
void* pa_struct_new(int64_t size);
void* pa_struct_copy(void* src, int64_t size);
size_t gc_hash(uintptr_t p);
void gc_mark(uintptr_t v);
void gc_scan_object(PyAdamasRc* h);
void pa_gc_add_roots(void** roots, int64_t n_roots);
void pa_gc_init(void** roots, int64_t n_roots, void* stack_top);
void pa_rc_stats_init(void);
void pa_disable_unknown_index(void);
void pa_set_index_readonly(void);
void pa_gc_unref_child(void* p);
/* 生成器帧的堆槽释放：own 1 已知堆 / 2 可能是未知值。 */
void pa_gen_slot_release(int64_t* slot, int64_t own);
void gc_drain(void);
void pa_gc_collect(void);
void pa_gc_shutdown(void);
void pa_import_module(int64_t* ready, void (*init)(void));
double pa_math_sqrt(double x);
int64_t pa_math_floor(double x);
int64_t pa_math_ceil(double x);
double pa_math_fabs(double x);
double pa_math_sin(double x);
double pa_math_cos(double x);
double pa_math_tan(double x);
double pa_math_log(double x);
double pa_math_log2(double x);
double pa_math_log10(double x);
double pa_math_exp(double x);
double pa_math_pow(double x, double y);
double pa_math_fmod(double x, double y);
double pa_math_atan2(double y, double x);
double pa_math_hypot(double x, double y);
double pa_math_degrees(double x);
double pa_math_radians(double x);
int64_t pa_math_trunc(double x);
double pa_math_copysign(double x, double y);
int64_t pa_math_isnan(double x);
int64_t pa_math_isinf(double x);
int64_t pa_math_isfinite(double x);
int64_t pa_math_factorial(int64_t n);
int64_t pa_math_gcd(int64_t a, int64_t b);
int64_t pa_math_lcm(int64_t a, int64_t b);
double pa_math_fsum(PyAdamasList* l);
int64_t pa_hash_value(int64_t kind, int64_t v);
int64_t pa_hash_f64(double v);
double pa_math_acos(double x);
double pa_math_asin(double x);
double pa_math_atan(double x);
double pa_math_acosh(double x);
double pa_math_asinh(double x);
double pa_math_atanh(double x);
double pa_math_cbrt(double x);
double pa_math_cosh(double x);
double pa_math_sinh(double x);
double pa_math_tanh(double x);
double pa_math_exp2(double x);
double pa_math_expm1(double x);
double pa_math_log1p(double x);
double pa_math_erf(double x);
double pa_math_erfc(double x);
double pa_math_gamma(double x);
double pa_math_lgamma(double x);
double pa_math_ulp(double x);
double pa_math_nextafter(double x, double y);
double pa_math_remainder(double x, double y);
double pa_math_ldexp(double x, int64_t i);
double pa_math_fma(double x, double y, double z);
int64_t pa_math_isclose(double a, double b);
PyAdamasList* pa_math_frexp(double x);
PyAdamasList* pa_math_modf(double x);
int64_t pa_math_isqrt(int64_t n);
int64_t pa_math_comb(int64_t n, int64_t k);
int64_t pa_math_perm(int64_t n, int64_t k);
int64_t pa_math_prod(PyAdamasList* l);
int64_t pa_math_sumprod(PyAdamasList* a, PyAdamasList* b);
double pa_math_dist(PyAdamasList* a, PyAdamasList* b);
void pa_sys_set_args(int64_t argc, char** argv);
PyAdamasList* pa_sys_argv(void);
PyAdamasStr* pa_sys_executable(void);
void pa_sys_exit(int64_t code);
void pa_print_i64(int64_t v);
void pa_print_u64(uint64_t v);
void pa_print_any(int64_t v);
/* CPython 桥结果的打印分派（float 盒 / list / dict 的 repr），rt_cpy.c。 */
void pa_print_cpy_value(int64_t v);
/* CPython 桥调用：(str* module, str* attr, list* args, str* descs) -> unknown。
   args 是 kind 0 借用打包列表；descs 是逐参数拼接的自定界类型描述符。 */
int64_t pa_cpy_call(PyAdamasStr* module, PyAdamasStr* attr, PyAdamasList* args,
                    PyAdamasStr* descs);
int rc_index_has(PyAdamasRc* h);
void pa_print_f64(double v);
void pa_print_bool(int64_t v);
void pa_print_str(PyAdamasStr* s);
void pa_print_bytes(PyAdamasStr* s);
int64_t pa_bytes_at(PyAdamasStr* s, int64_t i);
void pa_print_none(void);
void pa_print_sep(void);
void pa_print_end(void);
void pa_panic(PyAdamasStr* msg);
void pa_panic_div0(void);
void pa_panic_overflow(void);
int64_t pa_pow_i64(int64_t base, int64_t exp);
void pa_int_from_f64_check(double v);

/* ── random：CPython 兼容的 MT19937 ─────────────────────────────── */
typedef struct PyAdamasRandom PyAdamasRandom;
/* deque：环形缓冲的双端队列。pop/popleft 返回转移引用。 */
typedef struct PyAdamasDeque PyAdamasDeque;
PyAdamasDeque* pa_deque_new(int64_t maxlen);
PyAdamasDeque* pa_deque_from_list(PyAdamasList* items, int64_t maxlen);
void pa_deque_append(PyAdamasDeque* d, int64_t v);
void pa_deque_appendleft(PyAdamasDeque* d, int64_t v);
int64_t pa_deque_pop(PyAdamasDeque* d);
int64_t pa_deque_popleft(PyAdamasDeque* d);
int64_t pa_deque_len(PyAdamasDeque* d);
int64_t pa_deque_maxlen(PyAdamasDeque* d);
int64_t pa_deque_get(PyAdamasDeque* d, int64_t i);
void pa_deque_set(PyAdamasDeque* d, int64_t i, int64_t v);
void pa_deque_rotate(PyAdamasDeque* d, int64_t n);
void pa_deque_clear(PyAdamasDeque* d);
void pa_deque_extend(PyAdamasDeque* d, PyAdamasList* items);
void pa_deque_extendleft(PyAdamasDeque* d, PyAdamasList* items);
PyAdamasList* pa_deque_to_list(PyAdamasDeque* d);
void pa_deque_remove(PyAdamasDeque* d, int64_t v, int64_t kind);
int64_t pa_deque_count(PyAdamasDeque* d, int64_t v, int64_t kind);
int64_t pa_deque_index(PyAdamasDeque* d, int64_t v, int64_t kind);
void pa_deque_insert(PyAdamasDeque* d, int64_t i, int64_t v);
void pa_deque_reverse(PyAdamasDeque* d);
PyAdamasDeque* pa_deque_copy(PyAdamasDeque* d);

/* datetime / pathlib：都是不透明句柄，字段读取和字符串操作走下面的函数。 */
typedef struct PyAdamasDateTime PyAdamasDateTime;
typedef struct PyAdamasTimeDelta PyAdamasTimeDelta;
typedef struct PyAdamasPath PyAdamasPath;
PyAdamasDateTime* pa_date_new(int64_t y, int64_t mo, int64_t d);
PyAdamasDateTime* pa_datetime_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                                int64_t s);
PyAdamasDateTime* pa_datetime_now(int64_t is_date);
int64_t pa_datetime_field(PyAdamasDateTime* dt, int64_t idx);
PyAdamasStr* pa_datetime_isoformat(PyAdamasDateTime* dt);
PyAdamasStr* pa_datetime_strftime(PyAdamasDateTime* dt, PyAdamasStr* fmt);
PyAdamasTimeDelta* pa_timedelta_new(int64_t days, int64_t seconds, int64_t microseconds);
int64_t pa_timedelta_field(PyAdamasTimeDelta* td, int64_t idx);
double pa_timedelta_total_seconds(PyAdamasTimeDelta* td);
int64_t pa_timedelta_cmp(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b);
PyAdamasTimeDelta* pa_timedelta_add(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b);
PyAdamasTimeDelta* pa_timedelta_sub(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b);
PyAdamasTimeDelta* pa_timedelta_mul(PyAdamasTimeDelta* td, int64_t k);
PyAdamasTimeDelta* pa_timedelta_neg(PyAdamasTimeDelta* td);
PyAdamasTimeDelta* pa_timedelta_floordiv_int(PyAdamasTimeDelta* td, int64_t k);
PyAdamasTimeDelta* pa_timedelta_truediv_int(PyAdamasTimeDelta* td, int64_t k);
int64_t pa_timedelta_floordiv_td(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b);
double pa_timedelta_truediv_td(PyAdamasTimeDelta* a, PyAdamasTimeDelta* b);
PyAdamasStr* pa_timedelta_str(PyAdamasTimeDelta* td);
PyAdamasDateTime* pa_datetime_replace(PyAdamasDateTime* dt, PyAdamasList* fields);
PyAdamasDateTime* pa_datetime_add_delta(PyAdamasDateTime* dt, PyAdamasTimeDelta* td,
                                        int64_t sub);
PyAdamasTimeDelta* pa_datetime_diff(PyAdamasDateTime* a, PyAdamasDateTime* b);
int64_t pa_datetime_cmp(PyAdamasDateTime* a, PyAdamasDateTime* b);
PyAdamasPath* pa_path_new(PyAdamasStr* s);
PyAdamasStr* pa_path_str(PyAdamasPath* p);
PyAdamasPath* pa_path_join(PyAdamasPath* p, PyAdamasStr* s);
PyAdamasStr* pa_path_name(PyAdamasPath* p);
PyAdamasStr* pa_path_suffix(PyAdamasPath* p);
PyAdamasPath* pa_path_with_suffix(PyAdamasPath* p, PyAdamasStr* suffix);
PyAdamasList* pa_path_parts(PyAdamasPath* p);
PyAdamasStr* pa_path_stem(PyAdamasPath* p);
PyAdamasPath* pa_path_parent(PyAdamasPath* p);
PyAdamasStr* pa_path_read_bytes(PyAdamasPath* p);
int64_t pa_path_write_bytes(PyAdamasPath* p, PyAdamasStr* data);
int64_t pa_path_is_absolute(PyAdamasPath* p);
PyAdamasPath* pa_path_relative_to(PyAdamasPath* p, PyAdamasStr* base);
PyAdamasPath* pa_path_resolve(PyAdamasPath* p);
PyAdamasPath* pa_path_absolute(PyAdamasPath* p);
int64_t pa_path_touch(PyAdamasPath* p);
int64_t pa_path_exists(PyAdamasPath* p);
int64_t pa_path_is_file(PyAdamasPath* p);
int64_t pa_path_is_dir(PyAdamasPath* p);
PyAdamasStr* pa_path_read_text(PyAdamasPath* p);
int64_t pa_path_write_text(PyAdamasPath* p, PyAdamasStr* text);
PyAdamasList* pa_path_iterdir(PyAdamasPath* p);
PyAdamasList* pa_path_glob(PyAdamasPath* p, PyAdamasStr* pattern);
int64_t pa_path_mkdir(PyAdamasPath* p, int64_t mode, int64_t parents, int64_t exist_ok);

PyAdamasRandom* pa_random_new(int64_t seed);void pa_random_seed(PyAdamasRandom* r, int64_t seed);
double pa_random_next(PyAdamasRandom* r);
int64_t pa_random_randint(PyAdamasRandom* r, int64_t a, int64_t b);
int64_t pa_random_choice(PyAdamasRandom* r, PyAdamasList* seq);
double pa_random_uniform(PyAdamasRandom* r, double a, double b);
void pa_random_shuffle(PyAdamasRandom* r, PyAdamasList* seq);
void pa_random_gseed(int64_t seed);
double pa_random_gnext(void);
int64_t pa_random_grandint(int64_t a, int64_t b);
int64_t pa_random_gchoice(PyAdamasList* seq);
double pa_random_guniform(double a, double b);
void pa_random_gshuffle(PyAdamasList* seq);
/* 补充的分布与采样：r 为 NULL 表示走全局 rng。 */
int64_t pa_random_randrange(PyAdamasRandom* r, int64_t a, int64_t b, int64_t step);
int64_t pa_random_getrandbits(PyAdamasRandom* r, int64_t k);
double pa_random_gauss(PyAdamasRandom* r, double mu, double sigma);
double pa_random_expovariate(PyAdamasRandom* r, double lambd);
double pa_random_triangular(PyAdamasRandom* r, double low, double high, double mode);
PyAdamasList* pa_random_sample(PyAdamasRandom* r, PyAdamasList* seq, int64_t k);
PyAdamasList* pa_random_choices(PyAdamasRandom* r, PyAdamasList* seq, int64_t k);

/* ── os：目录与路径 ─────────────────────────────────────────────── */
void pa_os_mkdir(PyAdamasStr* path);
void pa_os_rmdir(PyAdamasStr* path);
void pa_os_remove(PyAdamasStr* path);
void pa_os_close(int64_t fd);
int64_t pa_os_path_islink(PyAdamasStr* path);
void pa_os_rename(PyAdamasStr* from, PyAdamasStr* to);
PyAdamasStr* pa_os_getcwd(void);
int64_t pa_os_cpu_count(void);
PyAdamasStr* pa_os_path_join(PyAdamasList* parts);
int64_t pa_os_path_exists(PyAdamasStr* path);
int64_t pa_os_path_isdir(PyAdamasStr* path);
int64_t pa_os_path_isfile(PyAdamasStr* path);
PyAdamasList* pa_os_listdir(PyAdamasStr* path);
PyAdamasStr* pa_os_getenv(PyAdamasStr* name, PyAdamasStr* def);
PyAdamasStr* pa_os_path_basename(PyAdamasStr* path);
PyAdamasStr* pa_os_path_dirname(PyAdamasStr* path);
PyAdamasList* pa_os_path_splitext(PyAdamasStr* path);
PyAdamasStr* pa_os_path_normpath(PyAdamasStr* path);
PyAdamasStr* pa_os_path_abspath(PyAdamasStr* path);
PyAdamasStr* pa_os_path_expanduser(PyAdamasStr* path);
int64_t pa_os_getpid(void);
void pa_os_chdir(PyAdamasStr* path);
void pa_os_chmod(PyAdamasStr* path, int64_t mode);
int64_t pa_os_access(PyAdamasStr* path, int64_t mode);
void pa_os_makedirs(PyAdamasStr* path, int64_t exist_ok);
PyAdamasStr* pa_os_path_realpath(PyAdamasStr* s);
int64_t pa_os_path_getsize(PyAdamasStr* s);
double pa_os_path_getmtime(PyAdamasStr* s);
PyAdamasList* pa_os_path_split(PyAdamasStr* s);
int64_t pa_os_path_samefile(PyAdamasStr* a, PyAdamasStr* b);

/* ── cffi：免写直调 / 动态库句柄（rt_cffi.c）───────────────────── */
int64_t pa_dylib_load(PyAdamasStr* path);
int64_t pa_dylib_close(int64_t handle);
int64_t pa_c_sym_default(const char* name);
int64_t pa_c_sym_handle(int64_t handle, const char* name);

/* ── time ───────────────────────────────────────────────────────── */
double pa_time_time(void);
double pa_time_monotonic(void);
double pa_time_perf_counter(void);
void pa_time_sleep(double seconds);

/* ── itertools / collections 的静态特化辅助 ─────────────────────── */
PyAdamasList* pa_itertools_chain(PyAdamasList* a, PyAdamasList* b, int64_t elem_kind);
PyAdamasList* pa_itertools_repeat(int64_t v, int64_t n, int64_t elem_kind);
/* seqs 是一个 kind 0 的临时列表，data 里直接装源列表指针（不持所有权）。 */
PyAdamasList* pa_itertools_product(PyAdamasList* seqs, int64_t elem_kind);
PyAdamasList* pa_itertools_permutations(PyAdamasList* pool, int64_t r, int64_t elem_kind);
PyAdamasList* pa_itertools_combinations(PyAdamasList* pool, int64_t r, int64_t elem_kind);
PyAdamasList* pa_itertools_combinations_wr(PyAdamasList* pool, int64_t r,
                                           int64_t elem_kind);
PyAdamasList* pa_itertools_zip_longest(PyAdamasList* seqs, int64_t elem_kind);
PyAdamasList* pa_itertools_accumulate(PyAdamasList* xs, int64_t is_float);
int64_t pa_functools_reduce(void* fn, PyAdamasList* xs, int64_t init, int64_t has_init);
double pa_functools_reduce_f(void* fn, PyAdamasList* xs, double init, int64_t has_init);
PyAdamasDict* pa_collections_counter(PyAdamasList* src, int64_t key_kind, int64_t key_own);
PyAdamasList* pa_collections_most_common(PyAdamasDict* d, int64_t n);

/* ── json：静态类型描述符 ───────────────────────────────────────── */
/* flags: bit0 sort_keys / bit1 ensure_ascii / bit2 allow_nan；
 * indent: INT64_MIN = None（紧凑）；seps: NULL 或 tuple[str,str]（借用）。 */
PyAdamasStr* pa_json_dumps(int64_t value, PyAdamasStr* desc, int64_t indent,
                           int64_t flags, PyAdamasList* seps);
int64_t pa_json_loads(PyAdamasStr* text, PyAdamasStr* desc);

/* ── re：回溯 VM 与 Pattern/Match ──────────────────────────────── */
typedef struct PyAdamasRegex PyAdamasRegex;
typedef struct PyAdamasMatch PyAdamasMatch;
int64_t pa_re_compile(PyAdamasStr* pat, int64_t flags);
int64_t pa_re_search_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags, int64_t mode);
int64_t pa_re_search(PyAdamasRegex* re, PyAdamasStr* text, int64_t mode);
int64_t pa_re_group(PyAdamasMatch* m, int64_t n);
int64_t pa_re_group_str(PyAdamasMatch* m, PyAdamasStr* name);
int64_t pa_re_groups(PyAdamasMatch* m);
int64_t pa_re_groupdict(PyAdamasMatch* m);
int64_t pa_re_lastindex(PyAdamasMatch* m);
int64_t pa_re_lastgroup(PyAdamasMatch* m);
int64_t pa_re_start(PyAdamasMatch* m, int64_t n);
int64_t pa_re_end(PyAdamasMatch* m, int64_t n);
int64_t pa_re_span(PyAdamasMatch* m, int64_t n);
int64_t pa_re_findall_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags);
int64_t pa_re_findall_groups_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags);
int64_t pa_re_findall(PyAdamasRegex* re, PyAdamasStr* text);
int64_t pa_re_sub_str(PyAdamasStr* pat, PyAdamasStr* repl, PyAdamasStr* text, int64_t count,
                        int64_t flags);
int64_t pa_re_sub(PyAdamasRegex* re, PyAdamasStr* repl, PyAdamasStr* text, int64_t count);
int64_t pa_re_split_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t maxsplit, int64_t flags);
int64_t pa_re_split(PyAdamasRegex* re, PyAdamasStr* text, int64_t maxsplit);
int64_t pa_re_escape(PyAdamasStr* s);
int64_t pa_re_pattern_groups(PyAdamasRegex* re);
int64_t pa_re_pattern_flags(PyAdamasRegex* re);
int64_t pa_re_pattern_str(PyAdamasRegex* re);
int64_t pa_re_finditer(PyAdamasRegex* re, PyAdamasStr* text);
int64_t pa_re_finditer_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags);
int64_t pa_re_expand(PyAdamasMatch* m, PyAdamasStr* tmpl);
int64_t pa_re_sub_fn(PyAdamasRegex* re, int64_t closure, PyAdamasStr* text, int64_t count);

/* ── subprocess：CompletedProcess 与 run() ─────────────────────── */
typedef struct PyAdamasProc PyAdamasProc;
PyAdamasProc* pa_subprocess_run(PyAdamasList* argv, int64_t capture, int64_t text,
                              double timeout, int64_t check, int64_t shell, PyAdamasStr* cwd);
int64_t pa_subprocess_returncode(PyAdamasProc* p);
PyAdamasStr* pa_subprocess_stdout(PyAdamasProc* p);
PyAdamasStr* pa_subprocess_stderr(PyAdamasProc* p);

/* ── subprocess.Popen ─────────────────────────────────────────── */
typedef struct PyAdamasPopen PyAdamasPopen;
PyAdamasPopen* pa_popen_new(PyAdamasList* argv, int64_t shell, PyAdamasStr* cwd, int64_t stdin_mode,
                          int64_t stdout_mode, int64_t stderr_mode);
int64_t pa_popen_pid(PyAdamasPopen* p);
int64_t pa_popen_returncode(PyAdamasPopen* p);
PyAdamasFile* pa_popen_stdout(PyAdamasPopen* p);
PyAdamasFile* pa_popen_stderr(PyAdamasPopen* p);
PyAdamasFile* pa_popen_stdin(PyAdamasPopen* p);
int64_t pa_popen_wait(PyAdamasPopen* p, double timeout);
void pa_popen_kill(PyAdamasPopen* p);
void pa_popen_terminate(PyAdamasPopen* p);
void pa_popen_send_signal(PyAdamasPopen* p, int64_t sig);
PyAdamasList* pa_popen_communicate(PyAdamasPopen* p, PyAdamasStr* input);

/* ── 引用所有权辅助（kind 2 = unknown，可能非堆）──────────────── */
/* 容器/槽的引用所有权类别：0 非堆，1 堆指针，2 unknown（可能是整数，
   也可能是堆指针）。kind 2 的位置在写入时统一 retain（堆指针走 rc_inc，
   unknown 值走注册表判断），释放时统一走注册表判断。 */
static inline void pa_own_inc(int64_t own, int64_t v) {
    if (!v) return;
    if (own == 1) pa_rc_inc((void*)v);
    else if (own == 2) pa_rc_inc_unknown(v);
}

static inline void pa_own_dec(int64_t own, int64_t v) {
    if (!v) return;
    if (own == 1) pa_rc_dec((void*)v);
    else if (own == 2) pa_rc_dec_unknown(v);
}

/* GC 清扫期：kind 2 的元素同样只能按注册表判断；rc_dec 在清扫期只调
   可达对象的计数，语义与 gc_unref_child 一致。 */
static inline void pa_own_gc_unref(int64_t own, int64_t v) {
    if (!v) return;
    if (own == 1) pa_gc_unref_child((void*)v);
    else if (own == 2) pa_rc_dec_unknown(v);
}

/* ── 虚拟线程（Go 式无色并发，M=1 协作调度；docs/vthread-design.md）──
   handle/chan 都是 RC OBJ，magic 校验。spawn 接管闭包的一份引用；
   chan send 接管 heap 种类值的一份引用，recv 交出一份。 */
void* pa_vt_spawn(int64_t fn);
void pa_vt_join(int64_t h);
void pa_vt_sleep(int64_t ms);
void pa_vt_shutdown(void);
void pa_vt_gc_scan(void);
void* pa_chan_new(int64_t kind, int64_t cap);
void pa_chan_send(void* c, int64_t v);
int64_t pa_chan_recv(void* c);
void pa_chan_send_f(void* c, double v);
double pa_chan_recv_f(void* c);
/* GC 跨栈扫描导出（rt_core.c）：挂起 vthread 的帧链精确扫描 + 单字标记。 */
struct PyAdamasFrame;
void pa_gc_scan_frames_from(PyAdamasFrame* top);
void pa_gc_mark_word(uintptr_t v);
/* 异常上下文水位 save/load（rt_exc.c，vthread 切换用）。 */
void pa_exc_ctx_save(void** top, int64_t* depth);
void pa_exc_ctx_load(void* top, int64_t depth);
