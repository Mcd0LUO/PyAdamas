// rt_internal.h —— pyadamas 运行时的内部接口
//
// 运行时按职责拆成多个编译单元（rc/gc、异常、生成器、异步、任务/Future、
// fd、异步生成器、容器/输出）。这里集中放共享的类型、全局状态和内部函数
// 声明；各 .c 只包含这个头。CMake 用 ld -r 把各部分合成编译器链接的 rt.o。
#pragma once

#include <math.h>
#include <poll.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int64_t len;
    int64_t hash;      /* 预留给字符串驻留 */
    const char* data;
} PyAdamasStr;

/* elem_kind: 0 = 全部非堆类型，1 = 全部堆类型，2 = 有 unknown / 混合。
   kind 1/2 的元素在写入时 retain（堆→rc_inc，unknown→rc_inc_unknown），
   覆盖/截断/销毁时 release（kind 2 走注册表判断，整数是空操作）。 */
typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* data;
    int64_t elem_kind;
} PyAdamasList;

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
    int64_t key_kind;  /* 0 int/bool, 1 str, 2 float, 3 raw */
    int64_t key_own;   /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;   /* 0 非堆, 1 堆, 2 unknown */
    /* 类 key 的 __hash__ / __eq__ vtable 槽号；-1 表示按机器字哈希/比较。 */
    int64_t hash_slot;
    int64_t eq_slot;
    /* collections.defaultdict 的内置工厂；0 表示普通 dict。
       只记录工厂种类，缺省下标读取时才创建值，不影响 get()/in。 */
    int64_t default_factory;
} PyAdamasDict;


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
_Static_assert(sizeof(PyAdamasRc) == 64, "codegen literal headers must match PyAdamasRc size");

#define PYADAMAS_GC_THRESHOLD (2 * 1024 * 1024)

/* 普通函数帧记录：codegen 在函数序言压栈、正常返回前弹栈。每个条目是
   帧里一个「持有引用」的堆槽地址，longjmp 跳过这些帧时按地址读值并
   dec，避免异常路径漏掉局部变量的引用。记录本身在目标帧的栈上，布局
   与生成代码一致：prev、n、槽地址×n。 */
typedef struct PyAdamasFrame {
    struct PyAdamasFrame* prev;
    int64_t n;
    void* slots[];  /* 槽地址（void**），解引用得到当前值 */
} PyAdamasFrame;

typedef struct PyAdamasHandler {
    jmp_buf env;  /* 必须放在开头：指针同时作为 setjmp 的参数 */
    struct PyAdamasHandler* prev;
    int64_t mark;
    /* 压 handler 时的帧栈顶：异常展开到这一层就停，当前帧的槽
       留给函数自己正常退出时释放。 */
    struct PyAdamasFrame* frame;
} PyAdamasHandler;

typedef struct PyAdamasException {
    char* type;     /* 异常名，如 "ValueError" / 自定义异常类名 */
    char* message;  /* 可以为空串，不能为 NULL */
    struct PyAdamasException* cause;  /* raise ... from，owned */
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
} PyAdamasExcValue;

/* 复数：re/im 两个 double，RC 对象；magic 用于运行时判别。 */
#define PYADAMAS_COMPLEX_MAGIC 0x434f4d5000000001LL
typedef struct {
    int64_t magic;
    double re;
    double im;
} PyAdamasComplex;

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
PyAdamasStr* pa_fmt_i64(int64_t v, PyAdamasStr* spec);
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
    PyAdamasList* waiters;  /* kind 5 future：多个等待者（owned refs） */
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
extern PyAdamasFrame* pa_frame_top;
/* 异常 longjmp 前调用：释放 target 之上（更内层）所有帧里的堆槽引用。 */
void pa_frame_unwind(PyAdamasFrame* target);
extern PyAdamasException* pending_exc;
extern PyAdamasException* current_exc;
extern PyAdamasExcSave* exc_save_top;
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
extern PyAdamasRc** gc_table;
extern size_t gc_table_cap;
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
void panic_c(const char* msg);
void rc_link(PyAdamasRc* h);
void rc_unlink(PyAdamasRc* h);
void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*), int64_t kind);
void pa_rc_inc(void* p);
void pa_rc_dec(void* p);
int64_t pa_rc_mark(void);
void pa_rc_temp(void* p);
void pa_rc_temp_unknown(int64_t v);
void pa_rc_cleanup(int64_t mark);
void pa_rc_shutdown(void);
void pa_register_literals(void** objs, int64_t n);
void closure_destroy(void* p);
void closure_gc_release(void* p);
void* pa_closure_new(int64_t code, int64_t n_caps);
PyAdamasStr* str_alloc(int64_t len);
PyAdamasStr* str_new(const char* p, int64_t len);
/* cimport 外部函数返回 char* 时复制成 PyAdamasStr；NULL 对前者视为空串，
   对后者返回 NULL（py23 的 None）。 */
PyAdamasStr* pa_str_from_cstr(const char* p);
PyAdamasStr* pa_str_from_cstr_opt(const char* p);
void exc_free(PyAdamasException* e);
PyAdamasException* exc_new(const char* type, const char* msg);
PyAdamasException* exc_clone(PyAdamasException* e);
const char* exc_parent(const char* name);
int exc_is_a(const char* child, const char* want);
void exc_print_uncaught(PyAdamasException* e);
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
void pa_reraise(void);
void pa_exc_reraise(void);
PyAdamasExcValue* pa_exc_take_value(void);
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
void* pa_asyncio_gather(PyAdamasList* coros);
void detach_cur(void* c);
void cancel_suspended(void* c, PyAdamasException* e);
void* pa_asyncio_create_task(void* coro);
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
PyAdamasStr* pa_str_char_at(PyAdamasStr* s, int64_t i);
PyAdamasStr* pa_str_from_i64(int64_t v);
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
PyAdamasList* pa_list_new(int64_t cap, int64_t elem_kind);
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
void pa_list_append(PyAdamasList* l, int64_t v);
int64_t pa_list_get(PyAdamasList* l, int64_t i);
void pa_list_set(PyAdamasList* l, int64_t i, int64_t v);
void pa_list_set_len(PyAdamasList* l, int64_t n);
void pa_list_extend(PyAdamasList* dst, PyAdamasList* src);
PyAdamasList* pa_list_copy(PyAdamasList* src);
PyAdamasList* pa_list_reversed(PyAdamasList* src);
void pa_list_sort(PyAdamasList* l, int64_t kind);
PyAdamasList* pa_sorted_list(PyAdamasList* src, int64_t kind);
int64_t pa_list_index(PyAdamasList* l, int64_t v, int64_t kind);
int64_t pa_list_count(PyAdamasList* l, int64_t v, int64_t kind);
void pa_list_insert(PyAdamasList* l, int64_t i, int64_t v);
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
PyAdamasDict* pa_dict_copy(PyAdamasDict* src);
int64_t pa_isinstance(void* obj, int64_t class_id);
int64_t pa_obj_class_id(void* obj);
int64_t pa_is_named_class(void* obj, PyAdamasStr* qualname);

/* 运行期类对象：类名当值、类装饰器、isinstance 的第二个实参。
   cid 是本模块类编号；跨模块类用 -1 + 限定名。ctor 是构造包装函数
   地址（动态调用用，暂未启用）。name 是 owned 字符串。 */
#define PYADAMAS_CLASS_MAGIC 0x434c415300000001LL
typedef struct {
    int64_t magic;
    int64_t cid;
    int64_t ctor;
    PyAdamasStr* name;
} PyAdamasClass;

void* pa_class_new(int64_t cid, int64_t ctor, PyAdamasStr* name);
int64_t pa_class_cid(void* cls);
PyAdamasStr* pa_class_name(void* cls);
PyAdamasStr* pa_class_repr(void* cls);
int64_t pa_isinstance_class(void* obj, void* cls);
void* pa_super_lookup(void* obj, int64_t after_cid, int64_t slot, PyAdamasStr* name);
int64_t pa_is_exc(void* obj, PyAdamasStr* type);
int64_t pa_is_seq(void* obj);
int64_t pa_is_dict(void* obj);
int64_t pa_str_eq_unknown(void* obj, PyAdamasStr* s);
int64_t pa_complex_eq_unknown(void* obj, PyAdamasComplex* c);
int64_t pa_is_str(void* obj);
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
PyAdamasList* pa_list_sorted_by(PyAdamasList* src, PyAdamasList* keys, int64_t kind);
int64_t pa_list_eq(PyAdamasList* a, PyAdamasList* b, PyAdamasStr* chain);
int64_t pa_str_contains(PyAdamasStr* hay, PyAdamasStr* needle);
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step, int64_t* out_start, int64_t* out_stop, int64_t* out_step);
int64_t slice_total(int64_t start, int64_t stop, int64_t step);
PyAdamasList* pa_list_slice(PyAdamasList* src, int64_t start, int64_t stop, int64_t step);
PyAdamasStr* pa_str_slice(PyAdamasStr* src, int64_t start, int64_t stop, int64_t step);
void pa_list_slice_assign(PyAdamasList* dst, int64_t start, int64_t stop, int64_t step, PyAdamasList* src);
PyAdamasList* pa_str_to_list(PyAdamasStr* s);
PyAdamasStr* pa_str_capwords(PyAdamasStr* s);
/* 静态驻留字符串：256 个单字节串以及 "True"/"False"/"None"，均为永生对象。 */
PyAdamasStr* pa_char_str(int64_t c);
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
void gc_table_insert(PyAdamasRc* h);
PyAdamasRc* gc_table_find(uintptr_t payload);
void gc_mark(uintptr_t v);
void gc_scan_object(PyAdamasRc* h);
void pa_gc_add_roots(void** roots, int64_t n_roots);
void pa_gc_init(void** roots, int64_t n_roots, void* stack_top);
void pa_disable_unknown_index(void);
void pa_gc_unref_child(void* p);
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
void pa_sys_exit(int64_t code);
void pa_print_i64(int64_t v);
void pa_print_any(int64_t v);
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

/* datetime / pathlib：都是不透明句柄，字段读取和字符串操作走下面的函数。 */
typedef struct PyAdamasDateTime PyAdamasDateTime;
typedef struct PyAdamasPath PyAdamasPath;
PyAdamasDateTime* pa_date_new(int64_t y, int64_t mo, int64_t d);
PyAdamasDateTime* pa_datetime_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                                int64_t s);
PyAdamasDateTime* pa_datetime_now(int64_t is_date);
int64_t pa_datetime_field(PyAdamasDateTime* dt, int64_t idx);
PyAdamasStr* pa_datetime_isoformat(PyAdamasDateTime* dt);
PyAdamasStr* pa_datetime_strftime(PyAdamasDateTime* dt, PyAdamasStr* fmt);
PyAdamasPath* pa_path_new(PyAdamasStr* s);
PyAdamasStr* pa_path_str(PyAdamasPath* p);
PyAdamasPath* pa_path_join(PyAdamasPath* p, PyAdamasStr* s);
PyAdamasStr* pa_path_name(PyAdamasPath* p);
PyAdamasStr* pa_path_suffix(PyAdamasPath* p);
PyAdamasStr* pa_path_stem(PyAdamasPath* p);
PyAdamasPath* pa_path_parent(PyAdamasPath* p);
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

/* ── time ───────────────────────────────────────────────────────── */
double pa_time_time(void);
double pa_time_monotonic(void);
double pa_time_perf_counter(void);
void pa_time_sleep(double seconds);

/* ── itertools / collections 的静态特化辅助 ─────────────────────── */
PyAdamasList* pa_itertools_chain(PyAdamasList* a, PyAdamasList* b, int64_t elem_kind);
PyAdamasList* pa_itertools_repeat(int64_t v, int64_t n, int64_t elem_kind);
int64_t pa_functools_reduce(void* fn, PyAdamasList* xs, int64_t init, int64_t has_init);
double pa_functools_reduce_f(void* fn, PyAdamasList* xs, double init, int64_t has_init);
PyAdamasDict* pa_collections_counter(PyAdamasList* src, int64_t key_kind, int64_t key_own);
PyAdamasList* pa_collections_most_common(PyAdamasDict* d, int64_t n);

/* ── json：静态类型描述符 ───────────────────────────────────────── */
PyAdamasStr* pa_json_dumps(int64_t value, PyAdamasStr* desc, int64_t indent, int64_t sort_keys);
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
