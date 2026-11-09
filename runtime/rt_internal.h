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

/* 字典 = 开放寻址哈希表 + 插入顺序 order 数组。set 复用同一结构，
   val_own = 0、值恒为 0。len 在偏移 0，len() 和真值判断都只读它。 */
typedef struct {
    int64_t hash;  /* 0 表示空槽；真实 hash 强制非 0 */
    int64_t key;
    int64_t val;
} Py23DictEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    Py23DictEntry* entries;
    int64_t* order;    /* order[i] = 第 i 个插入项在 entries 里的槽号 */
    int64_t key_kind;  /* 0 int/bool, 1 str, 2 float, 3 raw */
    int64_t key_own;   /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;   /* 0 非堆, 1 堆, 2 unknown */
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
    int64_t kind;      /* 1 sleep, 2 gather, 3 task */
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
void exc_longjmp(void);
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
void py23_raise_c(const char* type, const char* msg);
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
void py23_call1(void* fn, int64_t arg);
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
void list_destroy(void* p);
void list_gc_release(void* p);
Py23List* py23_list_new(int64_t cap, int64_t elem_kind);
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
Py23Dict* py23_set_from_list(Py23List* src, int64_t key_kind, int64_t key_own);
int64_t py23_list_sum_i64(Py23List* l, int64_t start);
double py23_list_sum_f64(Py23List* l, double start);
int64_t py23_list_min_i64(Py23List* l);
int64_t py23_list_max_i64(Py23List* l);
uint64_t mix64(uint64_t x);
int64_t dict_hash_value(int64_t kind, int64_t v);
int dict_key_eq(int64_t kind, int64_t a, int64_t b);
int64_t dict_probe(Py23DictEntry* entries, int64_t cap, int64_t kind, int64_t key, int64_t hash);
int64_t dict_empty_slot(Py23DictEntry* entries, int64_t cap, int64_t hash);
void dict_resize(Py23Dict* d, int64_t ncap);
void dict_destroy(void* p);
void dict_gc_release(void* p);
Py23Dict* py23_dict_new(int64_t key_kind, int64_t key_own, int64_t val_own);
void py23_dict_set(Py23Dict* d, int64_t key, int64_t val);
int64_t py23_dict_get(Py23Dict* d, int64_t key);
int64_t py23_dict_contains(Py23Dict* d, int64_t key);
int64_t py23_dict_key_at(Py23Dict* d, int64_t i);
int64_t py23_dict_get_or(Py23Dict* d, int64_t key, int64_t def);
int own_to_list_kind(int64_t own);
Py23List* py23_dict_keys(Py23Dict* d);
Py23List* py23_dict_values(Py23Dict* d);
Py23List* py23_dict_items(Py23Dict* d);
void py23_dict_update(Py23Dict* dst, Py23Dict* src);
Py23Dict* py23_dict_copy(Py23Dict* src);
int64_t py23_isinstance(void* obj, int64_t class_id);
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
int64_t py23_list_contains(Py23List* l, int64_t v, int64_t kind);
int64_t py23_str_contains(Py23Str* hay, Py23Str* needle);
void slice_bounds(int64_t len, int64_t start, int64_t stop, int64_t step, int64_t* out_start, int64_t* out_stop, int64_t* out_step);
int64_t slice_total(int64_t start, int64_t stop, int64_t step);
Py23List* py23_list_slice(Py23List* src, int64_t start, int64_t stop, int64_t step);
Py23Str* py23_str_slice(Py23Str* src, int64_t start, int64_t stop, int64_t step);
void py23_list_slice_assign(Py23List* dst, int64_t start, int64_t stop, int64_t step, Py23List* src);
Py23List* py23_str_to_list(Py23Str* s);
Py23List* py23_range_new(int64_t start, int64_t stop, int64_t step);
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*release)(void*));
size_t gc_hash(uintptr_t p);
void gc_table_insert(Py23Rc* h);
Py23Rc* gc_table_find(uintptr_t payload);
void gc_mark(uintptr_t v);
void gc_scan_object(Py23Rc* h);
void py23_gc_add_roots(void** roots, int64_t n_roots);
void py23_gc_init(void** roots, int64_t n_roots, void* stack_top);
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
void py23_sys_set_args(int64_t argc, char** argv);
Py23List* py23_sys_argv(void);
void py23_sys_exit(int64_t code);
void py23_print_i64(int64_t v);
void py23_print_f64(double v);
void py23_print_bool(int64_t v);
void py23_print_str(Py23Str* s);
void py23_print_none(void);
void py23_print_sep(void);
void py23_print_end(void);
void py23_panic(Py23Str* msg);
void py23_panic_div0(void);
void py23_panic_overflow(void);
int64_t py23_pow_i64(int64_t base, int64_t exp);
