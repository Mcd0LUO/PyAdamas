// rt_internal.h —— py23 运行时的内部接口
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
} Py23Str;

/* elem_kind: 0 = 全部非堆类型，1 = 全部堆类型，2 = 有 unknown / 混合。
   kind 1 的元素在写入时由生成代码 retain，销毁时由运行时 release；
   kind 2 只 retain 不 release（看不见的类型不敢 dec），代价是泄漏。 */
typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* data;
    int64_t elem_kind;
} Py23List;

/* range 是惰性对象：只存边界，元素按需算。和 CPython 的 range 一样，
   range(10**9) 只占几十字节，不会当场造十亿个元素。 */
typedef struct {
    int64_t start;
    int64_t stop;
    int64_t step;
    int64_t len;
} Py23Range;

/* 字典 = 密集 entries（插入顺序）+ 开放寻址索引表。index 槽存
   「entry 下标 + 1」，0 表示空；查找先走 4 字节索引，命中才读 entry。
   扩容/删除只重建索引，entries 顺序不动。set 复用同一结构，
   val_own = 0、值恒为 0。len 在偏移 0，len() 和真值判断都只读它。 */
typedef struct {
    int64_t hash;  /* 0 表示空槽；真实 hash 强制非 0 */
    int64_t key;
    int64_t val;
} Py23DictEntry;

typedef struct {
    int64_t len;
    int64_t cap;       /* entries 容量 */
    Py23DictEntry* entries;
    int32_t* index;    /* 开放寻址索引：entry 下标 + 1，0 为空 */
    int64_t icap;      /* 索引表容量（2 的幂） */
    int64_t key_kind;  /* 0 int/bool, 1 str, 2 float, 3 raw */
    int64_t key_own;   /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;   /* 0 非堆, 1 堆, 2 unknown */
    /* 类 key 的 __hash__ / __eq__ vtable 槽号；-1 表示按机器字哈希/比较。 */
    int64_t hash_slot;
    int64_t eq_slot;
} Py23Dict;


enum {
    PY23_RC_STR = 0,
    PY23_RC_LIST = 1,
    PY23_RC_OBJ = 2,
    PY23_RC_CLOSURE = 3,
    PY23_RC_DICT = 4,
    /* 用户类实例。和帧/await/异常值等 OBJ 负载分开，isinstance 才能
       安全地把 payload 第一个字解释成 vtable 指针。 */
    PY23_RC_INSTANCE = 5,
    /* range：没有堆字段（出边为零），不可能成环，按叶子对象处理。 */
    PY23_RC_RANGE = 6,
};

typedef struct Py23Rc {
    struct Py23Rc* next;
    struct Py23Rc* prev;
    int64_t rc;
    void (*dtor)(void*);
    void (*release)(void*);
    int64_t size;
    int64_t kind;
    int64_t marked;
} Py23Rc;

#define PY23_RC_IMMORTAL INT64_MIN
#define PY23_RC_HDR ((int64_t)sizeof(Py23Rc))
_Static_assert(sizeof(Py23Rc) == 64, "codegen literal headers must match Py23Rc size");

#define PY23_GC_THRESHOLD (2 * 1024 * 1024)

typedef struct Py23Handler {
    jmp_buf env;  /* 必须放在开头：指针同时作为 setjmp 的参数 */
    struct Py23Handler* prev;
    int64_t mark;
} Py23Handler;

typedef struct Py23Exception {
    char* type;     /* 异常名，如 "ValueError" / 自定义异常类名 */
    char* message;  /* 可以为空串，不能为 NULL */
    struct Py23Exception* cause;  /* raise ... from，owned */
} Py23Exception;

typedef struct Py23ExcSave {
    Py23Exception* prev;
    struct Py23ExcSave* up;
} Py23ExcSave;

/* 异常值对象：`raise ValueError("x")` 里的实例、`except as e` 绑定的 e。
   控制流仍用 Py23Exception* + longjmp；这个对象只是值语义的外壳。 */
#define PY23_EXCVAL_MAGIC 0x4558435600000001LL
typedef struct Py23ExcValue {
    int64_t magic;
    char* type;
    char* message;
    Py23List* args;             /* owned：args[0] 是 message 字符串 */
    struct Py23ExcValue* cause; /* __cause__，owned（RC 对象） */
} Py23ExcValue;

/* 复数：re/im 两个 double，RC 对象；magic 用于运行时判别。 */
#define PY23_COMPLEX_MAGIC 0x434f4d5000000001LL
typedef struct {
    int64_t magic;
    double re;
    double im;
} Py23Complex;

Py23Complex* py23_complex_new(double re, double im);
Py23Complex* py23_complex_add(Py23Complex* a, Py23Complex* b);
Py23Complex* py23_complex_sub(Py23Complex* a, Py23Complex* b);
Py23Complex* py23_complex_mul(Py23Complex* a, Py23Complex* b);
Py23Complex* py23_complex_div(Py23Complex* a, Py23Complex* b);
Py23Complex* py23_complex_neg(Py23Complex* a);
double py23_complex_abs(Py23Complex* a);
Py23Str* py23_complex_str(Py23Complex* a);
int64_t py23_complex_eq(Py23Complex* a, Py23Complex* b);
int64_t py23_complex_truth(Py23Complex* a);
Py23Str* py23_fmt_i64(int64_t v, Py23Str* spec);
Py23Str* py23_fmt_bool(int64_t v, Py23Str* spec);
Py23Str* py23_fmt_f64(double v, Py23Str* spec);
Py23Str* py23_fmt_str(Py23Str* v, Py23Str* spec);
Py23Str* py23_fmt_complex(Py23Complex* v, Py23Str* spec);
Py23Str* py23_repr_str(Py23Str* v);


typedef struct {
    int64_t magic;
    int64_t kind;      /* 1 sleep, 2 gather, 3 task, 4 asyncgen, 5 future, 6 fd, 8 wait_for */
    int64_t done;
    int64_t starting;  /* gather 正在启动子任务：完成延后到启动结束 */
    int64_t result;    /* owned */
    int64_t exc;       /* Py23Exception* */
    int64_t waiter;    /* owned ref */
    int64_t deadline_ms;
    int64_t remaining;
    int64_t task_coro; /* task：被包装的协程（owned） */
    int64_t cancelled;
    int64_t driver_gen; /* kind 4：被驱动的异步生成器（owned） */
    int64_t driver_sent; /* kind 4：asend 传入值（owned） */
    int64_t driver_close; /* kind 4：aclose 模式 */
    int64_t driver_throw; /* kind 4：athrow 注入的异常（owned，Py23Exception*） */
    int64_t pending;    /* future/fd：已经挂进 loop_pending/loop_fds */
    int64_t fd;         /* kind 6：等待的 fd */
    int64_t write;      /* kind 6：0 读 / 1 写 */
    Py23List* waiters;  /* kind 5 future：多个等待者（owned refs） */
    Py23List* results;
    Py23List* children;
    Py23List* callbacks; /* kind 5 future：add_done_callback 注册的函数（owned refs） */
} Py23Await;

#define PY23_GEN_HEADER 10
#define PY23_CORO_MAGIC 0x434f524f00000001LL
#define PY23_AGEN_MAGIC 0x4147454e00000001LL
#define PY23_AWAIT_MAGIC 0x4157414954000001LL

extern Py23Rc* heap_head;
extern int64_t alloc_since_gc;
extern int gc_ready;
extern int gc_sweeping;
extern Py23Rc** rc_index;
extern size_t rc_index_cap;
extern size_t rc_index_fill;
extern size_t rc_index_live;
extern void** temp_stack;
extern int64_t temp_len;
extern int64_t temp_cap;
extern Py23Handler* handler_top;
extern Py23Exception* pending_exc;
extern Py23Exception* current_exc;
extern Py23ExcSave* exc_save_top;
extern Py23List* loop_ready;
extern Py23List* loop_timers;
extern Py23List* loop_pending;
extern Py23List* loop_fds;
extern void* loop_roots[4];
extern int loop_roots_registered;
extern int loop_running;
extern void* loop_root;
extern int loop_root_state;
extern int64_t loop_root_result;
extern Py23Exception* loop_root_exc;
extern void** gc_globals;
extern int64_t gc_n_globals;
extern int64_t gc_globals_cap;
extern uintptr_t gc_stack_top;
extern Py23Rc** gc_table;
extern size_t gc_table_cap;
extern Py23Rc** gc_work;
extern size_t gc_work_len;
extern Py23List* sys_argv_list;

size_t rc_index_slot(Py23Rc* h);
void rc_index_rehash(size_t ncap);
void rc_index_put(Py23Rc* h);
void rc_index_del(Py23Rc* h);
int rc_index_has(Py23Rc* h);
int64_t py23_rc_inc_unknown(int64_t v);
void py23_rc_dec_unknown(int64_t v);
void panic_c(const char* msg);
void rc_link(Py23Rc* h);
void rc_unlink(Py23Rc* h);
void* rc_alloc(int64_t size, void (*dtor)(void*), void (*release)(void*), int64_t kind);
void py23_rc_inc(void* p);
void py23_rc_dec(void* p);
int64_t py23_rc_mark(void);
void py23_rc_temp(void* p);
void py23_rc_cleanup(int64_t mark);
void py23_rc_shutdown(void);
void py23_register_literals(void** objs, int64_t n);
void closure_destroy(void* p);
void closure_gc_release(void* p);
void* py23_closure_new(int64_t code, int64_t n_caps);
Py23Str* str_alloc(int64_t len);
Py23Str* str_new(const char* p, int64_t len);
void exc_free(Py23Exception* e);
Py23Exception* exc_new(const char* type, const char* msg);
Py23Exception* exc_clone(Py23Exception* e);
const char* exc_parent(const char* name);
int exc_is_a(const char* child, const char* want);
void exc_print_uncaught(Py23Exception* e);
_Noreturn void exc_longjmp(void);
void* py23_handler_push(void);
void py23_handler_leave(void* p);
void py23_handler_discard(void* p);
void py23_handler_suspend(void* p);
void py23_handler_rearm(void* p);
void py23_handler_free(void* p);
int64_t py23_exc_matches(Py23Str* want);
Py23Str* py23_exc_accept(void);
Py23ExcValue* py23_exc_accept_value(void);
Py23ExcValue* py23_exc_value_new(Py23Str* type, Py23Str* msg);
Py23ExcValue* py23_exc_value_from_exc(Py23Exception* e);
Py23Exception* py23_exc_from_value(Py23ExcValue* v);
void py23_raise_value(Py23ExcValue* v);
void py23_raise_value_from(Py23ExcValue* v, Py23ExcValue* cause);
Py23ExcValue* py23_exc_value_cause(Py23ExcValue* v);
void py23_exc_register(Py23Str* child, Py23Str* parent);
Py23List* py23_exc_value_args(Py23ExcValue* v);
Py23Str* py23_exc_value_str(Py23ExcValue* v);
void py23_exc_done(void);
void py23_exc_clear(void);
Py23Str* py23_exc_type_name(void);
Py23Str* py23_exc_message(void);
void py23_raise(Py23Str* type, Py23Str* msg);
void py23_reraise(void);
void py23_exc_reraise(void);
_Noreturn void py23_raise_c(const char* type, const char* msg);
void gen_destroy(void* g);
void* py23_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind);
int64_t gen_run(void* g, int64_t sent, int* done);
int64_t py23_gen_next(void* g);
int64_t py23_gen_next_opt(void* g, int64_t* ok);
int64_t py23_gen_send(void* g, int64_t sent);
int64_t py23_gen_send_opt(void* g, int64_t sent, int64_t* ok);
void py23_gen_close(void* g);
void py23_gen_finalize(void* g);
int64_t now_ms(void);
int frame_is_coro(void* p);
int frame_is_agen(void* p);
int is_await_obj(void* p);
void await_destroy(void* p);
void await_gc_release(void* p);
void free_attached_exception(void* payload, int64_t size);
Py23Await* await_new(int64_t kind);
void loop_ensure(void);
void ready_push(void* c);
void* ready_pop(void);
int64_t list_take_at(Py23List* l, int64_t i);
void list_clear_owned(Py23List* l);
void* pending_take(void* w);
/* 调用一个 py23 函数值（闭包首字是代码地址，%r10 是环境），单参数。 */
void* py23_call1(void* fn, int64_t arg);
void finish_gather(Py23Await* g);
void gather_child_fail(Py23Await* g, Py23Exception* e);
void start_gather(Py23Await* w, void* waiter);
void start_awaitable(void* obj, void* waiter);
void deliver_owned(void* waiter, void* child, int64_t value, Py23Exception* exc);
void coro_inject(void* c, Py23Exception* e);
void coro_finish(void* c, int64_t value, Py23Exception* exc);
void await_from_coro(void* c, void* w);
void coro_step(void* c);
void py23_coro_rethrow(void* g);
int fire_due_timers(int64_t now);
int64_t earliest_deadline(void);
void* py23_asyncio_sleep(double secs);
void* py23_asyncio_gather(Py23List* coros);
void detach_cur(void* c);
void cancel_suspended(void* c, Py23Exception* e);
void* py23_asyncio_create_task(void* coro);
int64_t py23_asyncio_task_done(void* t);
int64_t py23_asyncio_task_cancel(void* t);
void gather_cancel(Py23Await* g);
void future_cancel(Py23Await* f);
void simple_wait_cancel(Py23Await* w);
void cancel_chain(void* w);
void future_settle(Py23Await* f, int64_t value, Py23Exception* exc);
void* py23_asyncio_future_new(void);
void py23_future_set_result(void* fp, int64_t value);
int64_t py23_future_done(void* fp);
int64_t py23_future_cancelled(void* fp);
int64_t py23_future_cancel(void* fp);
void raise_exception_copy(Py23Exception* e, const char* fallback);
int64_t py23_future_result(void* fp);
Py23ExcValue* py23_future_exception(void* fp);
int64_t py23_asyncio_task_result(void* tp);
Py23ExcValue* py23_asyncio_task_exception(void* tp);
int64_t py23_asyncio_task_cancelled(void* tp);
void py23_list_del_index(Py23List* l, int64_t i);
void* py23_event_new(void);
void py23_event_set(void* e);
int64_t py23_event_is_set(void* e);
void* py23_event_wait(void* e);
void* py23_lock_new(void);
void* py23_lock_acquire(void* l);
void py23_lock_release(void* l);
int64_t py23_lock_locked(void* l);
void* py23_queue_new(int64_t maxsize);
void* py23_queue_put(void* q, int64_t v);
void* py23_queue_get(void* q);
int64_t py23_queue_qsize(void* q);
int64_t py23_queue_empty(void* q);
int64_t py23_queue_full(void* q);
void* py23_queue_join(void* q);
void py23_queue_task_done(void* q);
void py23_queue_put_nowait(void* q, int64_t v);
int64_t py23_queue_get_nowait(void* q);
void* py23_timeout_new(double secs);
void py23_timeout_enter(void* t);
int64_t py23_timeout_exit(void* t);
int64_t py23_timeout_expired(void* t);
void* py23_taskgroup_new(void);
void* py23_taskgroup_add(void* g, void* coro);
void* py23_taskgroup_exit(void* g);
void py23_taskgroup_cancel(void* g);
void* py23_sem_new(int64_t value);
void* py23_sem_acquire(void* s);
void py23_sem_release(void* s);
int64_t py23_sem_locked(void* s);
void* py23_asyncio_wait_for(void* aw, double secs);
void wait_for_cancel(Py23Await* w);
void* py23_asyncio_wait_fd(int64_t fd, int64_t write);
int poll_fds(int timeout_ms);
void* py23_asyncgen_anext(void* gen);
void* py23_asyncgen_asend(void* gen, int64_t sent);
void* py23_asyncgen_aclose(void* gen);
void* py23_asyncgen_athrow(void* gen, Py23ExcValue* exc);
void driver_complete(Py23Await* d, int64_t value, Py23Exception* exc);
void agen_drive(Py23Await* d);
int64_t py23_asyncio_run(void* coro);
void format_double(char* buf, size_t n, double v);
Py23Str* py23_str_concat(Py23Str* a, Py23Str* b);
Py23Str* py23_repr_tuple(Py23List* t, int64_t kind);
Py23Str* py23_str_char_at(Py23Str* s, int64_t i);
Py23Str* py23_str_from_i64(int64_t v);
Py23Str* py23_str_from_f64(double v);
Py23Str* py23_str_from_bool(int64_t v);
Py23Str* py23_str_from_none(void);
int64_t py23_str_to_i64(Py23Str* s, int64_t base);
double py23_str_to_f64(Py23Str* s);
int64_t py23_ord(Py23Str* s);
Py23Str* py23_chr(int64_t c);
Py23Str* py23_int_to_base(int64_t v, int64_t base);
double py23_round_f64(double x, int64_t ndigits);
int64_t py23_round_i64(int64_t v, int64_t ndigits);
int64_t py23_str_eq(Py23Str* a, Py23Str* b);
int64_t py23_str_cmp(Py23Str* a, Py23Str* b);
/* 字符串方法派发：op 序号与 src/ir.h 的 StrOp 一致；未用的参数传 0。 */
int64_t py23_str_method(Py23Str* s, int64_t op, int64_t a0, int64_t a1, int64_t a2);

/* 文件对象（布局只在 rt_obj.c 里定义） */
typedef struct Py23File Py23File;
Py23File* py23_file_open(Py23Str* path, int64_t fd, Py23Str* mode, int64_t buffering,
                         int64_t newline, int64_t closefd);
void py23_file_config(Py23File* f, Py23Str* encoding, Py23Str* errors);
Py23Str* py23_file_encoding(Py23File* f);
Py23Str* py23_file_errors(Py23File* f);
Py23Str* py23_file_read(Py23File* f, int64_t n);
Py23Str* py23_file_readline(Py23File* f, int64_t size);
Py23List* py23_file_readlines(Py23File* f, int64_t hint);
int64_t py23_file_write(Py23File* f, Py23Str* s);
void py23_file_writelines(Py23File* f, Py23List* lines);
int64_t py23_file_seek(Py23File* f, int64_t off, int64_t whence);
int64_t py23_file_tell(Py23File* f);
int64_t py23_file_truncate(Py23File* f, int64_t size);
int64_t py23_file_fileno(Py23File* f);
int64_t py23_file_isatty(Py23File* f);
int64_t py23_file_readable(Py23File* f);
int64_t py23_file_writable(Py23File* f);
int64_t py23_file_seekable(Py23File* f);
int64_t py23_file_closed(Py23File* f);
Py23Str* py23_file_name(Py23File* f);
Py23Str* py23_file_mode(Py23File* f);
void py23_file_close(Py23File* f);
void py23_file_flush(Py23File* f);
Py23Str* py23_input(Py23Str* prompt);
Py23File* py23_sys_stream(int64_t which);
void list_destroy(void* p);
void list_gc_release(void* p);
Py23List* py23_list_new(int64_t cap, int64_t elem_kind);
Py23Range* py23_range_new(int64_t start, int64_t stop, int64_t step);
int64_t py23_range_len(Py23Range* r);
int64_t py23_range_first(Py23Range* r);
int64_t py23_range_step(Py23Range* r);
int64_t py23_range_stop(Py23Range* r);
int64_t py23_range_get(Py23Range* r, int64_t i);
int64_t py23_range_contains(Py23Range* r, int64_t v);
int64_t py23_range_eq(Py23Range* a, Py23Range* b);
int64_t py23_range_sum_i64(Py23Range* r, int64_t start);
double py23_range_sum_f64(Py23Range* r, double start);
int64_t py23_range_min_i64(Py23Range* r);
int64_t py23_range_max_i64(Py23Range* r);
int64_t py23_range_any(Py23Range* r);
int64_t py23_range_all(Py23Range* r);
Py23List* py23_range_to_list(Py23Range* r);
Py23Str* py23_range_repr(Py23Range* r);
void py23_list_append(Py23List* l, int64_t v);
int64_t py23_list_get(Py23List* l, int64_t i);
void py23_list_set(Py23List* l, int64_t i, int64_t v);
void py23_list_set_len(Py23List* l, int64_t n);
void py23_list_extend(Py23List* dst, Py23List* src);
Py23List* py23_list_copy(Py23List* src);
Py23List* py23_list_reversed(Py23List* src);
void py23_list_sort(Py23List* l, int64_t kind);
Py23List* py23_sorted_list(Py23List* src, int64_t kind);
int64_t py23_list_index(Py23List* l, int64_t v, int64_t kind);
int64_t py23_list_count(Py23List* l, int64_t v, int64_t kind);
void py23_list_insert(Py23List* l, int64_t i, int64_t v);
void py23_list_reverse(Py23List* l);
int64_t py23_int_bit_length(int64_t v);
Py23List* py23_enumerate_list(Py23List* seq);
Py23List* py23_zip2_list(Py23List* a, Py23List* b);
Py23Dict* py23_set_from_list(Py23List* src, int64_t key_kind, int64_t key_own,
                             int64_t hash_slot, int64_t eq_slot);
int64_t py23_list_sum_i64(Py23List* l, int64_t start);
double py23_list_sum_f64(Py23List* l, double start);
int64_t py23_list_min_i64(Py23List* l);
int64_t py23_list_max_i64(Py23List* l);
uint64_t mix64(uint64_t x);
int64_t dict_hash_value(int64_t kind, int64_t v);
int dict_key_eq(int64_t kind, int64_t a, int64_t b);
void dict_destroy(void* p);
void dict_gc_release(void* p);
Py23Dict* py23_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own,
                        int64_t hash_slot, int64_t eq_slot);
void py23_dict_set(Py23Dict* d, int64_t key, int64_t val);
int64_t py23_dict_get(Py23Dict* d, int64_t key);
int64_t py23_dict_contains(Py23Dict* d, int64_t key);
int64_t py23_dict_key_at(Py23Dict* d, int64_t i);
int64_t py23_dict_get_or(Py23Dict* d, int64_t key, int64_t def);
int64_t py23_dict_get_opt(Py23Dict* d, int64_t key);
int own_to_list_kind(int64_t own);
Py23List* py23_dict_keys(Py23Dict* d);
Py23List* py23_dict_values(Py23Dict* d);
Py23List* py23_dict_items(Py23Dict* d);
int64_t py23_dict_eq(Py23Dict* a, Py23Dict* b, Py23Str* chain);
int64_t chain_set_eq(Py23Dict* a, Py23Dict* b, const uint8_t* chain, int64_t len,
                     int64_t depth);
int64_t py23_set_eq(Py23Dict* a, Py23Dict* b);
void py23_dict_update(Py23Dict* dst, Py23Dict* src);
Py23Dict* py23_dict_copy(Py23Dict* src);
int64_t py23_isinstance(void* obj, int64_t class_id);
int64_t py23_obj_class_id(void* obj);
int64_t py23_is_named_class(void* obj, Py23Str* qualname);
void* py23_super_lookup(void* obj, int64_t after_cid, int64_t slot, Py23Str* name);
int64_t py23_is_exc(void* obj, Py23Str* type);
int64_t py23_is_seq(void* obj);
int64_t py23_is_dict(void* obj);
int64_t py23_str_eq_unknown(void* obj, Py23Str* s);
int64_t py23_complex_eq_unknown(void* obj, Py23Complex* c);
int64_t py23_is_str(void* obj);
int64_t py23_is_complex(void* obj);
void dict_rebuild(Py23Dict* d);
int dict_take(Py23Dict* d, int64_t key, int64_t* val_out);
int64_t py23_dict_pop(Py23Dict* d, int64_t key, int64_t has_default, int64_t def);
Py23List* py23_dict_popitem(Py23Dict* d);
int64_t py23_dict_pop_last(Py23Dict* d);
void py23_dict_remove(Py23Dict* d, int64_t key, int64_t missing_ok);
void py23_dict_clear(Py23Dict* d);
int list_value_eq(int64_t kind, int64_t a, int64_t b);
// 元素比较链：编译器按静态类型生成，运行时按层递归。见 rt_list.c 的说明。
int chain_elem_eq(int kind, int64_t x, int64_t y, const uint8_t* chain, int64_t len,
                  int64_t depth);
int64_t chain_list_eq(Py23List* a, Py23List* b, const uint8_t* chain, int64_t len,
                      int64_t depth);
int64_t chain_dict_eq(Py23Dict* a, Py23Dict* b, const uint8_t* chain, int64_t len,
                      int64_t depth);
int64_t py23_list_contains(Py23List* l, int64_t v, int64_t kind);
Py23List* py23_list_sorted_by(Py23List* src, Py23List* keys, int64_t kind);
int64_t py23_list_eq(Py23List* a, Py23List* b, Py23Str* chain);
int64_t py23_str_contains(Py23Str* hay, Py23Str* needle);
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step, int64_t* out_start, int64_t* out_stop, int64_t* out_step);
int64_t slice_total(int64_t start, int64_t stop, int64_t step);
Py23List* py23_list_slice(Py23List* src, int64_t start, int64_t stop, int64_t step);
Py23Str* py23_str_slice(Py23Str* src, int64_t start, int64_t stop, int64_t step);
void py23_list_slice_assign(Py23List* dst, int64_t start, int64_t stop, int64_t step, Py23List* src);
Py23List* py23_str_to_list(Py23Str* s);
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*));
size_t gc_hash(uintptr_t p);
void gc_table_insert(Py23Rc* h);
Py23Rc* gc_table_find(uintptr_t payload);
void gc_mark(uintptr_t v);
void gc_scan_object(Py23Rc* h);
void py23_gc_add_roots(void** roots, int64_t n_roots);
void py23_gc_init(void** roots, int64_t n_roots, void* stack_top);
void py23_disable_unknown_index(void);
void py23_gc_unref_child(void* p);
void gc_drain(void);
void py23_gc_collect(void);
void py23_gc_shutdown(void);
void py23_import_module(int64_t* ready, void (*init)(void));
double py23_math_sqrt(double x);
int64_t py23_math_floor(double x);
int64_t py23_math_ceil(double x);
double py23_math_fabs(double x);
double py23_math_sin(double x);
double py23_math_cos(double x);
double py23_math_tan(double x);
double py23_math_log(double x);
double py23_math_log2(double x);
double py23_math_log10(double x);
double py23_math_exp(double x);
double py23_math_pow(double x, double y);
double py23_math_fmod(double x, double y);
double py23_math_atan2(double y, double x);
double py23_math_hypot(double x, double y);
double py23_math_degrees(double x);
double py23_math_radians(double x);
int64_t py23_math_trunc(double x);
double py23_math_copysign(double x, double y);
int64_t py23_math_isnan(double x);
int64_t py23_math_isinf(double x);
int64_t py23_math_isfinite(double x);
int64_t py23_math_factorial(int64_t n);
int64_t py23_math_gcd(int64_t a, int64_t b);
int64_t py23_math_lcm(int64_t a, int64_t b);
double py23_math_fsum(Py23List* l);
int64_t py23_hash_value(int64_t kind, int64_t v);
int64_t py23_hash_f64(double v);
double py23_math_acos(double x);
double py23_math_asin(double x);
double py23_math_atan(double x);
double py23_math_acosh(double x);
double py23_math_asinh(double x);
double py23_math_atanh(double x);
double py23_math_cbrt(double x);
double py23_math_cosh(double x);
double py23_math_sinh(double x);
double py23_math_tanh(double x);
double py23_math_exp2(double x);
double py23_math_expm1(double x);
double py23_math_log1p(double x);
double py23_math_erf(double x);
double py23_math_erfc(double x);
double py23_math_gamma(double x);
double py23_math_lgamma(double x);
double py23_math_ulp(double x);
double py23_math_nextafter(double x, double y);
double py23_math_remainder(double x, double y);
double py23_math_ldexp(double x, int64_t i);
double py23_math_fma(double x, double y, double z);
int64_t py23_math_isclose(double a, double b);
Py23List* py23_math_frexp(double x);
Py23List* py23_math_modf(double x);
int64_t py23_math_isqrt(int64_t n);
int64_t py23_math_comb(int64_t n, int64_t k);
int64_t py23_math_perm(int64_t n, int64_t k);
int64_t py23_math_prod(Py23List* l);
int64_t py23_math_sumprod(Py23List* a, Py23List* b);
double py23_math_dist(Py23List* a, Py23List* b);
void py23_sys_set_args(int64_t argc, char** argv);
Py23List* py23_sys_argv(void);
void py23_sys_exit(int64_t code);
void py23_print_i64(int64_t v);
void py23_print_any(int64_t v);
int rc_index_has(Py23Rc* h);
void py23_print_f64(double v);
void py23_print_bool(int64_t v);
void py23_print_str(Py23Str* s);
void py23_print_bytes(Py23Str* s);
int64_t py23_bytes_at(Py23Str* s, int64_t i);
void py23_print_none(void);
void py23_print_sep(void);
void py23_print_end(void);
void py23_panic(Py23Str* msg);
void py23_panic_div0(void);
void py23_panic_overflow(void);
int64_t py23_pow_i64(int64_t base, int64_t exp);

/* ── random：CPython 兼容的 MT19937 ─────────────────────────────── */
typedef struct Py23Random Py23Random;
/* datetime / pathlib：都是不透明句柄，字段读取和字符串操作走下面的函数。 */
typedef struct Py23DateTime Py23DateTime;
typedef struct Py23Path Py23Path;
Py23DateTime* py23_date_new(int64_t y, int64_t mo, int64_t d);
Py23DateTime* py23_datetime_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi,
                                int64_t s);
Py23DateTime* py23_datetime_now(int64_t is_date);
int64_t py23_datetime_field(Py23DateTime* dt, int64_t idx);
Py23Str* py23_datetime_isoformat(Py23DateTime* dt);
Py23Str* py23_datetime_strftime(Py23DateTime* dt, Py23Str* fmt);
Py23Path* py23_path_new(Py23Str* s);
Py23Str* py23_path_str(Py23Path* p);
Py23Path* py23_path_join(Py23Path* p, Py23Str* s);
Py23Str* py23_path_name(Py23Path* p);
Py23Str* py23_path_suffix(Py23Path* p);
Py23Str* py23_path_stem(Py23Path* p);
Py23Path* py23_path_parent(Py23Path* p);
int64_t py23_path_exists(Py23Path* p);
int64_t py23_path_is_file(Py23Path* p);
int64_t py23_path_is_dir(Py23Path* p);
Py23Str* py23_path_read_text(Py23Path* p);
int64_t py23_path_write_text(Py23Path* p, Py23Str* text);

Py23Random* py23_random_new(int64_t seed);void py23_random_seed(Py23Random* r, int64_t seed);
double py23_random_next(Py23Random* r);
int64_t py23_random_randint(Py23Random* r, int64_t a, int64_t b);
int64_t py23_random_choice(Py23Random* r, Py23List* seq);
double py23_random_uniform(Py23Random* r, double a, double b);
void py23_random_shuffle(Py23Random* r, Py23List* seq);
void py23_random_gseed(int64_t seed);
double py23_random_gnext(void);
int64_t py23_random_grandint(int64_t a, int64_t b);
int64_t py23_random_gchoice(Py23List* seq);
double py23_random_guniform(double a, double b);
void py23_random_gshuffle(Py23List* seq);

/* ── os：目录与路径 ─────────────────────────────────────────────── */
void py23_os_mkdir(Py23Str* path);
void py23_os_rmdir(Py23Str* path);
void py23_os_remove(Py23Str* path);
void py23_os_rename(Py23Str* from, Py23Str* to);
Py23Str* py23_os_getcwd(void);
int64_t py23_os_cpu_count(void);
Py23Str* py23_os_path_join(Py23List* parts);
int64_t py23_os_path_exists(Py23Str* path);
int64_t py23_os_path_isdir(Py23Str* path);
int64_t py23_os_path_isfile(Py23Str* path);
Py23List* py23_os_listdir(Py23Str* path);
Py23Str* py23_os_getenv(Py23Str* name, Py23Str* def);
Py23Str* py23_os_path_basename(Py23Str* path);
Py23Str* py23_os_path_dirname(Py23Str* path);
Py23List* py23_os_path_splitext(Py23Str* path);
Py23Str* py23_os_path_normpath(Py23Str* path);
Py23Str* py23_os_path_abspath(Py23Str* path);
Py23Str* py23_os_path_expanduser(Py23Str* path);

/* ── time ───────────────────────────────────────────────────────── */
double py23_time_time(void);
double py23_time_monotonic(void);
double py23_time_perf_counter(void);
void py23_time_sleep(double seconds);

/* ── itertools / collections 的静态特化辅助 ─────────────────────── */
Py23List* py23_itertools_chain(Py23List* a, Py23List* b, int64_t elem_kind);
Py23List* py23_itertools_repeat(int64_t v, int64_t n, int64_t elem_kind);
int64_t py23_functools_reduce(void* fn, Py23List* xs, int64_t init, int64_t has_init);
double py23_functools_reduce_f(void* fn, Py23List* xs, double init, int64_t has_init);
Py23Dict* py23_collections_counter(Py23List* src, int64_t key_kind, int64_t key_own);
Py23List* py23_collections_most_common(Py23Dict* d, int64_t n);

/* ── json：静态类型描述符 ───────────────────────────────────────── */
Py23Str* py23_json_dumps(int64_t value, Py23Str* desc);
int64_t py23_json_loads(Py23Str* text, Py23Str* desc);

/* ── re：回溯 VM 与 Pattern/Match ──────────────────────────────── */
typedef struct Py23Regex Py23Regex;
typedef struct Py23Match Py23Match;
int64_t py23_re_compile(Py23Str* pat, int64_t flags);
int64_t py23_re_search_str(Py23Str* pat, Py23Str* text, int64_t flags, int64_t mode);
int64_t py23_re_search(Py23Regex* re, Py23Str* text, int64_t mode);
int64_t py23_re_group(Py23Match* m, int64_t n);
int64_t py23_re_group_str(Py23Match* m, Py23Str* name);
int64_t py23_re_groups(Py23Match* m);
int64_t py23_re_groupdict(Py23Match* m);
int64_t py23_re_lastindex(Py23Match* m);
int64_t py23_re_lastgroup(Py23Match* m);
int64_t py23_re_start(Py23Match* m, int64_t n);
int64_t py23_re_end(Py23Match* m, int64_t n);
int64_t py23_re_span(Py23Match* m, int64_t n);
int64_t py23_re_findall_str(Py23Str* pat, Py23Str* text, int64_t flags);
int64_t py23_re_findall(Py23Regex* re, Py23Str* text);
int64_t py23_re_sub_str(Py23Str* pat, Py23Str* repl, Py23Str* text, int64_t count,
                        int64_t flags);
int64_t py23_re_sub(Py23Regex* re, Py23Str* repl, Py23Str* text, int64_t count);
int64_t py23_re_split_str(Py23Str* pat, Py23Str* text, int64_t maxsplit, int64_t flags);
int64_t py23_re_split(Py23Regex* re, Py23Str* text, int64_t maxsplit);
int64_t py23_re_escape(Py23Str* s);
int64_t py23_re_pattern_groups(Py23Regex* re);
int64_t py23_re_pattern_flags(Py23Regex* re);
int64_t py23_re_pattern_str(Py23Regex* re);
int64_t py23_re_finditer(Py23Regex* re, Py23Str* text);
int64_t py23_re_finditer_str(Py23Str* pat, Py23Str* text, int64_t flags);
int64_t py23_re_expand(Py23Match* m, Py23Str* tmpl);
int64_t py23_re_sub_fn(Py23Regex* re, int64_t closure, Py23Str* text, int64_t count);

/* ── subprocess：CompletedProcess 与 run() ─────────────────────── */
typedef struct Py23Proc Py23Proc;
Py23Proc* py23_subprocess_run(Py23List* argv, int64_t capture, int64_t text,
                              double timeout, int64_t check);
int64_t py23_subprocess_returncode(Py23Proc* p);
Py23Str* py23_subprocess_stdout(Py23Proc* p);
Py23Str* py23_subprocess_stderr(Py23Proc* p);
