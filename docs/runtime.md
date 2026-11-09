# 运行时

运行时按职责拆成 `runtime/rt_*.c`（引用计数/GC、异常、生成器、异步、Task/Future、fd、异步生成器、容器与输出），共享接口在 `runtime/rt_internal.h`；各部分编成可重定位对象后用 `ld -r` 合成 `rt.o`，由 `py23 build` 链接进每个生成的程序，只依赖 libc 和 libm。**py23 编译器本身不链接它**（`PY23_RT_OBJ` 只是把路径编进二进制，供 `cc` 使用）。

## ABI 约定

运行时函数不是给用户调的，是给生成代码调的。约定只有三条：

1. 所有参数和返回值用 `int64_t` 或 `double` 传，指针就当 `int64_t` 用。没有按值传结构体。
2. 参数个数固定，不检查类型——类型在编译期已经确定，运行时不再打标签。
3. 出错就 `exit(1)`，不返回错误码，不抛异常。

因为生成代码把每个值都放在 8 字节栈槽里，`int64_t` 和指针天然同构；浮点用 `double` 走 xmm 寄存器，由 SysV 调用约定保证。

## 对象布局

```c
typedef struct Py23Rc {
    struct Py23Rc* next;    /* GC 注册表链 */
    struct Py23Rc* prev;
    int64_t rc;             /* INT64_MIN = 永生（字面量） */
    void (*dtor)(void*);    /* rc 归零：正常释放路径 */
    void (*release)(void*); /* GC 清扫：只减掉指向可达对象的引用 */
    int64_t size;           /* 负载字节数，GC 扫描范围 */
    int64_t kind;           /* STR / LIST / OBJ / CLOSURE / DICT / INSTANCE */
    int64_t marked;         /* 标记-清扫用 */
} Py23Rc;                   /* 64 字节，位于负载之前，对象指针不指向它 */

typedef struct { int64_t len; int64_t hash; const char* data; } Py23Str;
typedef struct { int64_t len; int64_t cap; int64_t* data; int64_t elem_kind; } Py23List;
typedef struct { int64_t hash; int64_t key; int64_t val; } Py23DictEntry;
typedef struct {
    int64_t len; int64_t cap;      /* len 在偏移 0，len()/真值判断直接读 */
    Py23DictEntry* entries;        /* 开放寻址表，hash == 0 为空槽 */
    int64_t* order;                /* 插入顺序：order[i] = entries 槽号 */
    int64_t key_kind;              /* 0 int/bool, 1 str, 2 float, 3 raw */
    int64_t key_own;               /* 0 非堆, 1 堆, 2 unknown */
    int64_t val_own;
} Py23Dict;
/* 用户对象没有对应的 C struct：布局由编译器按类定义生成。
   负载 +0 是 vtable 指针，字段从 +8 起，每个 8 字节；隐藏的 Py23Rc
   头在负载前面，所以既有偏移一个都没变。 */
```

分配统一走 `rc_alloc(size, dtor, release, kind)`，返回负载指针；
`py23_rc_inc` / `py23_rc_dec` 从负载指针往前 64 字节找到头。`rc_dec`
归零时先调 `dtor`，再释放整块内存。所有堆对象同时挂进 GC 注册表。

### 字符串

`len` 放在**偏移 0**，这是刻意的：`len(s)` 编译成一条 `movq (%rax), %rax`，`if s:` 也只是读这个字段。`hash` 预留给字符串驻留，现在恒为 0。

字符串有两种来源，负载布局完全一致，运行时函数不需要区分：

| 来源 | 分配方式 | 生命周期 |
|---|---|---|
| 字面量 | codegen 在 `.data.rel.ro` 里静态构造，前置 `rc = INT64_MIN` | 与进程同寿，inc/dec 直接跳过 |
| 拼接 / 转换 | `rc_alloc(sizeof(Py23Str) + len + 1, NULL)`，字符区紧跟结构体 | 引用计数归零时释放 |

堆分配的字符串尾部补 `\0`，方便调试时直接当 C 字符串看；但长度以 `len` 为准，内嵌 `\0` 是合法的。

### 列表

`cap` 是已分配容量，`len` 是有效元素数，`data` 是 8 字节槽数组。元素统一占 8 字节：整数直接放，浮点放位模式（`memcpy` 转换），指针放地址。**没有运行时类型标签**——取值时按什么类型解释由编译期决定。

`elem_kind` 是元素所有权类别，由降级器按容器静态类型算好：

| kind | 含义 | 写入时 | 覆盖 / 截断 / 销毁时 |
|---|---|---|---|
| 0 | 全部非堆类型 | 不 retain | 不 release |
| 1 | 全部堆类型 | 生成代码 retain | 运行时 release |
| 2 | unknown 或混合 | 已知堆类型才 retain | 不 release（看不见的类型不敢 dec） |

`py23_list_new(cap, elem_kind)` 最小容量 4；`append` 满了就翻倍。
`py23_list_set` 只在 kind 1 时 dec 旧元素；`py23_list_set_len` 只在 kind 1
时 dec 被截掉的元素。kind 2 不做精确 dec，靠循环收集器兜底——误报只会
延迟回收，不会提前释放。

### 字典 / 集合

字典是**开放寻址哈希表 + 插入顺序数组**：

- `entries` 容量为 2 的幂，线性探测；`order` 保留插入顺序，`for k in d`
  和 `len` 都按它工作；
- key 的哈希/判等类别放在 `key_kind`：int/bool 机器字、str 逐字节
  （FNV-1a）、float IEEE 值（`0.0` / `-0.0` 同键）、其他按机器字；
- `key_own` / `val_own` 决定 RC：写入前生成代码 retain 一份，更新/销毁
  时运行时 dec；unknown 只 retain 不 dec，交给 GC；
- 更新已有 key 时保留原来的 key 对象，释放调用方新传入的 key 引用；
- 删除没有墓碑：把项移出 `order` 后按剩余项**整表重建**，单次 O(n)，
  换取线性探测链不被挖断；`pop` / `popitem` / `remove` / `discard` /
  `clear` 都走这条路径，`popitem` 取 `order` 末尾（LIFO）；
- 负载因子超过 0.7 时翻倍 rehash，`order` 重排后仍保持插入顺序。
- set 就是 `val_own = 0`、值恒 0 的 dict，复用同一套函数和 GC。
- 方法：`get` 走 get/get_or；`keys`/`values`/`items` 分配新 list（items
  的元素是二元组），元素按目标 list 的 kind retain；`update` 逐项复制，
  目标 unknown 时也会 retain 源引用，避免源临时字典释放后悬空；
  `set.add/update` 复用 dict_set/update；`set.pop` 取最后一个插入项。

### 切片

`py23_list_slice` / `py23_str_slice` 接收 `(容器, start, stop, step)`，
缺省边界用 `INT64_MIN` 哨兵，运行时按 Python `slice.indices` 归一化：

- 正/负步长、负索引、越界夹取；`step == 0` 抛 ValueError；
- list/tuple 结果新建同 `elem_kind` 的列表，kind 1 元素逐个 retain；
- str 结果新建字节串，按字节复制；
- 结果是新堆对象，正常进入 RC/GC。

`py23_list_slice_assign(dst, start, stop, step, src)` 处理写切片：
`step == 1` 时用 `memmove` 支持长度变化（替换 / 插入 / 删除），扩展切片
要求 `src->len` 与切片长度一致；写入时按 `dst->elem_kind` retain 新元素、
dec 被覆盖的旧元素，`src == dst` 先做快照。`py23_list_extend(dst, src)`
用于 `*args` 打包，按 `dst` 的 kind 接管元素引用。

### 用户对象

对象是编译器按类定义排出来的连续内存，运行时只知道「多大」和析构器：

```c
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*gc_release)(void*));
```

调用方（生成代码）紧接着把 vtable 地址写到偏移 0。分配全零：0 同时是
`0` / `None` / 假，和栈槽「未赋值就是 None」的约定一致。

析构器由 codegen 按类布局生成，只 `rc_dec` 堆类型字段；没有堆字段的类传
`NULL`。子类析构器包含父类字段，顺序与对象布局一致。

vtable 是静态数据，在 `.data.rel.ro` 里，由 codegen 按 `IRClass::vtable` 生成。
vtable 前面固定有 16 字节元数据：`.Lvtanc` 祖先编号表指针 + 类编号，对象仍指向
槽数组本身。`py23_isinstance` 先查 RC 注册表确认是 `PY23_RC_INSTANCE`，再沿
祖先表判断；因此 `case Point(...)` 对 unknown 值安全，子类和 mixin 基类都能匹配。

### 闭包

闭包负载是 `[code, n_caps, cell0, cell1…]`：

```c
void* py23_closure_new(int64_t code, int64_t n_caps);
```

- `code` 是静态代码地址，不是堆指针，析构时不 dec；
- `n_caps` 后面的每个 8 字节槽是一个 cell 指针；
- 析构器逐个 `rc_dec` cell，然后由 RC 头统一释放；
- 无捕获函数的闭包在 `.data.rel.ro` 里静态构造，rc = `INT64_MIN`。

cell 不是新的运行时类型，就是一个**单元素列表** `list[T]`：读 `l[0]`、
写 `l[0] = v`，元素所有权和析构完全复用列表规则。这样闭包不需要第二套
引用计数实现。

### 生成器

生成器帧是普通堆对象（`PY23_RC_OBJ`），负载布局：

```
[state][status][sent][out][resume][closing][kind][waiter][exc][cur][slot0..]
```

- `resume` 是状态机代码地址，`gen_run` 用 `void (*)(void*, int64_t)` 调用；
- `status`：0 未开始/挂起，1 已完成（槽位已释放），2 异常逃出（清理已跑，
  槽位交给 GC），3 awaiting，4 已排队；
- `kind`：0 生成器，`PY23_CORO_MAGIC` 协程，`PY23_AGEN_MAGIC` 异步生成器；
- `waiter` 是等待本对象的协程/gather/task（持有一个引用）；
- `exc` 是待注入的 `Py23Exception*`；
- `cur` 是当前等待的 awaitable（借用观察指针，取消时用来解绑）；
- `out` 保存 yield/await 值或协程返回值，运行时读走后清零。

```c
void* py23_gen_new(int64_t n_slots, int64_t resume, int64_t release, int64_t kind);
int64_t py23_gen_next(void*);                  // 结束抛 StopIteration
int64_t py23_gen_next_opt(void*, int64_t* ok); // 结束写 ok=0
int64_t py23_gen_send(void*, int64_t sent);
int64_t py23_gen_send_opt(void*, int64_t sent, int64_t* ok); // yield from
void    py23_gen_close(void*);
```

`gen_run` 用一层 C 级 handler 兜住逃出 resume 的异常：标记 `status=2`
后重新转抛，避免生成器对象销毁时把已经跑过的 `finally`/`__exit__`
再跑一遍。析构器（`gen_destroy`）释放帧里挂着的异常/等待者引用，再调用
`py23_gen_finalize`：按挂起点快照执行清理并释放槽位，清理期间抛出的
异常被吞掉。GC 清扫不执行 Python 代码，只由 codegen 生成的 `gcrel`
把指向可达对象的槽引用减掉。

跨挂起的 handler 记录：yield/await 前 `py23_handler_suspend` 把它从全局栈
摘下（不 free），恢复时 `py23_handler_rearm` 重新压栈并记录临时值
水位，生成代码随后重新 `setjmp`；close 路径用 `py23_handler_free`
回收这些记录。

### 协程与事件循环

`async def` 复用生成器帧，`coro_step` 每次恢复后按 status 处理：

- `1`：把 `out` 里的返回值交给 waiter（或记为 `asyncio.run` 的结果）；
- `3`：`out` 是 awaitable，交给 `await_from_coro` 挂到事件循环；
- `2`：异常交给 waiter；等待者下次恢复时走注入分发，在帧内重新抛出。

awaitable 有六类：协程帧、`Py23Await` 的 sleep(1)/gather(2)/task(3)、
异步生成器驱动(4)、Future(5)、fd 等待(6)。每个对象只有一个 `waiter`，
Future 用 `waiters` 列表支持多个等待者、用 `callbacks` 列表保存
`add_done_callback` 注册的函数（完成时按注册顺序同步调用）；异步生成器
驱动对象用 `driver_sent`（asend 的值，跨事件循环持有引用）和
`driver_close`（aclose 模式）标记；gather 把自己的引用挂在每个
子 awaitable 上，最后一个子任务完成时汇总结果。task 由 `loop_pending`
持有到完成，保存结果/异常供后续 await；`cancel()` 沿 `cur` 向下级联
（协程链/task/gather/future/sleep/fd 各有取消路径），再注入
`CancelledError`，各层 `finally` 照常执行。

异步生成器的驱动对象挂在就绪队列上一步步推进（不递归调用 resume 栈）：
拉起生成器 → 遇到 `yield` 就完成驱动对象并把值交给等待者；遇到
`await` 则把驱动对象挂到内部 awaitable 上，等它完成后把结果写回帧的
`sent`/`exc` 槽再重新入队。生成器结束抛 `StopAsyncIteration`，
`async for` 在自己的 handler 里捕获。`asyncio.run` 的循环：

```
就绪队列非空 → 取一个协程 step
否则有定时器 → 触发到期项；没有到期项就 nanosleep 到最近 deadline
两者都空且根协程未完成 → RuntimeError("event loop stalled")
```

就绪队列、定时器表、`loop_pending` 和 `loop_fds` 都是 `list` 对象
并注册为 GC 根；`sleep` 用 `CLOCK_MONOTONIC` 毫秒定时。没有就绪任务时，
有 fd 等待就调用 `poll`（超时取最近定时器），否则 `nanosleep` 到
最近 deadline。根协程的异常在清空事件循环状态后设成 pending 并
longjmp 回同步调用点。GC 清扫和进程退出不跑析构器，但会单独释放挂在
帧 / Task / Future 上的 malloc 异常对象。

### 异常

异常不参与 RC/GC：handler 记录和异常对象都是 `malloc` 的普通内存。

- **handler 记录**：`{jmp_buf env; Py23Handler* prev; int64_t mark}`，
  `env` 必须在开头，生成代码把记录指针直接交给 `setjmp`。全局
  `handler_top` 维护 LIFO 链；`mark` 是压入时刻的临时值栈水位。
- **异常对象**：`{char* type; char* message}`，只保存内置异常名和消息。
  `pending_exc` 是待处理异常，`current_exc` 是当前 `except` 体正在处理
  的异常；`py23_exc_accept` / `py23_exc_done` 维护这层上下文。
- **匹配**：`py23_exc_matches` 按内置异常继承表（KeyError → LookupError
  → Exception 等）判断；未知名字默认按 Exception 子类处理。
- **展开**：landing pad 先 `py23_handler_leave` 弹出记录并
  `py23_rc_cleanup(mark)`，再分派。异常路径不执行被跳过函数帧的
  `release_slots`，那些局部引用由保守 GC 回收。
- **未捕获**：打印 `py23: uncaught <type>: <message>` 后 `exit(1)`。

## 函数表

`RtFn` 枚举见 `src/ir.h`，名字映射见 `rt_name()`。生成代码按枚举直接 `call py23_xxx`，不做符号查找。

### 列表

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_list_new` | `(i64 cap, i64 elem_kind) -> Py23List*` | 建空表，容量至少 4 |
| `py23_list_append` | `(Py23List*, i64) -> void` | 追加，满则翻倍 `realloc`；不隐式 retain |
| `py23_list_get` | `(Py23List*, i64) -> i64` | 负索引先加 `len`；越界抛 IndexError；返回借用 |
| `py23_list_set` | `(Py23List*, i64, i64) -> void` | 同上，写入；kind 1 时 dec 旧元素 |
| `py23_list_set_len` | `(Py23List*, i64) -> void` | 改 `len`；kind 1 时 dec 被截掉的元素 |
| `py23_list_extend` | `(Py23List* dst, Py23List* src) -> void` | 追加 src 全部元素；按 dst kind retain，`src == dst` 先快照 |
| `py23_list_slice_assign` | `(Py23List*, i64, i64, i64, Py23List*) -> void` | 切片赋值；step 1 支持长度变化 |
| `py23_str_to_list` | `(Py23Str*) -> Py23List*` | 按字节拆成 list[str]，每字符新串 |
| `py23_range_new` | `(i64 start, i64 stop, i64 step) -> Py23List*` | range() 实体化成 list[int]；step 0 抛 ValueError |

### 字典 / 集合

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_dict_new` | `(i64 key_kind, i64 key_own, i64 val_own) -> Py23Dict*` | 空表，初始 8 槽；set 传 `val_own = 0` |
| `py23_dict_set` | `(Py23Dict*, i64 key, i64 val) -> void` | 插入/更新；更新时释放新 key 引用并 dec 旧 value |
| `py23_dict_get` | `(Py23Dict*, i64 key) -> i64` | 缺失抛 KeyError；返回借用 |
| `py23_dict_contains` | `(Py23Dict*, i64 key) -> i64` | `in` 用，返回 0/1 |
| `py23_dict_key_at` | `(Py23Dict*, i64 index) -> i64` | 按插入顺序取 key，遍历用 |
| `py23_dict_get_or` | `(Py23Dict*, i64 key, i64 def) -> i64` | 缺失返回默认值 |
| `py23_dict_keys` | `(Py23Dict*) -> Py23List*` | 新 list，按插入顺序装 key |
| `py23_dict_values` | `(Py23Dict*) -> Py23List*` | 新 list，按插入顺序装 value |
| `py23_dict_items` | `(Py23Dict*) -> Py23List*` | 新 list，元素是 `(key, value)` 二元组 |
| `py23_dict_update` | `(Py23Dict*, Py23Dict*) -> void` | 逐项复制；先 retain，再 dict_set |
| `py23_dict_copy` | `(Py23Dict*) -> Py23Dict*` | 同所有权类别的浅拷贝（match 的 `**rest`） |
| `py23_dict_pop` | `(Py23Dict*, i64 key, i64 has_default, i64 def) -> i64` | 删除并返回值；缺失且有默认值返回默认值，否则抛 KeyError |
| `py23_dict_popitem` | `(Py23Dict*) -> Py23List*` | 删除最后插入项，返回 `(key, value)` 二元组 |
| `py23_dict_pop_last` | `(Py23Dict*) -> i64` | 删除最后插入项并返回 key（set.pop） |
| `py23_dict_remove` | `(Py23Dict*, i64 key, i64 missing_ok) -> void` | remove/discard；缺失时抛 KeyError 或按 missing_ok 忽略 |
| `py23_dict_clear` | `(Py23Dict*) -> void` | 释放全部 key/value 引用并清空表 |
| `py23_list_contains` | `(Py23List*, i64 value, i64 cmp_kind) -> i64` | list/tuple 的 `in`；0 int / 1 str / 2 float / 3 raw |
| `py23_str_contains` | `(Py23Str*, Py23Str*) -> i64` | 子串查找 |
| `py23_list_slice` | `(Py23List*, i64 start, i64 stop, i64 step) -> Py23List*` | 新列表；kind 1 元素 retain |
| `py23_str_slice` | `(Py23Str*, i64 start, i64 stop, i64 step) -> Py23Str*` | 新字节串 |

### 模式匹配

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_isinstance` | `(void*, i64 class_id) -> i64` | unknown 值是不是该类或子类；查 RC 注册表，原始整数安全返回 0 |
| `py23_is_exc` | `(void*, str* type) -> i64` | unknown 值是不是异常值且类型匹配（含自定义异常父子关系） |
| `py23_is_seq` | `(void*) -> i64` | 是不是 list/tuple 载体（序列模式的前置检查） |
| `py23_is_dict` | `(void*) -> i64` | 是不是 dict/set 载体（映射模式的前置检查） |
| `py23_dict_copy` | `(Py23Dict*) -> Py23Dict*` | `**rest` 的浅拷贝，随后用 `dict_remove` 删掉已匹配 key |

类模式的字段仍按编译期偏移直接 `field.get`：子类沿用父类字段前缀，所以模式类
自身的字段偏移对所有子类实例都成立。

### 异常

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_handler_push` | `() -> handler*` | 压入 handler 记录，保存临时栈水位 |
| `py23_handler_leave` | `(handler*) -> void` | 弹出记录并 `rc_cleanup` 到水位 |
| `py23_handler_discard` | `(handler*) -> void` | 只弹出记录，不清临时值（提前退出用） |
| `py23_exc_matches` | `(str* type) -> i64` | 当前异常是否属于该类型（含继承） |
| `py23_exc_accept` | `() -> str*` | pending 转 current，返回消息新字符串 |
| `py23_exc_accept_value` | `() -> excval*` | pending 转 current，返回异常值（`except as e`） |
| `py23_exc_value_new` | `(str* type, str* msg) -> excval*` | 构造异常值，msg 为 NULL 表示无参数 |
| `py23_exc_value_from_exc` | `(Py23Exception*) -> excval*` | 从控制流异常复制一个值对象 |
| `py23_exc_from_value` | `(excval*) -> Py23Exception*` | 转成 malloc 的 pending 异常 |
| `py23_exc_value_args` | `(excval*) -> tuple*` | `e.args`（借用，args[0] 是消息） |
| `py23_exc_value_str` | `(excval*) -> str*` | `str(e)` / `print(e)` 的消息字符串 |
| `py23_raise_value` | `(excval*) -> void` | 抛异常值（noreturn） |
| `py23_raise_value_from` | `(excval*, excval*|0) -> void` | 抛异常值并设置 cause（noreturn） |
| `py23_exc_value_cause` | `(excval*) -> excval*` | `e.__cause__`，没有返回 NULL |
| `py23_exc_register` | `(str* child, str* parent) -> void` | 自定义异常类的父子关系注册 |
| `py23_exc_done` | `() -> void` | 释放 current，恢复上一层异常上下文 |
| `py23_exc_clear` | `() -> void` | 抑制并释放 pending |
| `py23_exc_reraise` | `() -> void` | 继续向外交互展开（noreturn） |
| `py23_exc_type_name` | `() -> str*` | pending 的类型名新字符串 |
| `py23_exc_message` | `() -> str*` | pending 的消息新字符串 |
| `py23_raise` | `(str* type, str* msg) -> void` | 设置 pending 并 longjmp（noreturn） |
| `py23_reraise` | `() -> void` | bare raise：重抛 current（noreturn） |
| `py23_raise_c` | `(const char*, const char*) -> void` | 运行时错误入口（noreturn） |

### 模块

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_import_module` | `(i64* ready, void (*init)(void)) -> void` | 惰性初始化：ready 为 0 时置 1 并调用模块 init |
| `py23_gc_add_roots` | `(void** roots, i64 n) -> void` | 追加根表；入口先 `gc_init`，被导入模块在 init 入口追加 |

模块全局是普通静态槽，由生成代码导出；`init` 是模块顶层生成的
`void(void)`，返回前不释放模块全局、不 shutdown。入口函数负责
`GcShutdown` / `RcShutdown`。

### 内置模块 math / sys

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_math_sqrt` 等 | `(double) -> double` | sqrt/fabs/sin/cos/tan/log/log2/log10/exp；域错误抛 ValueError |
| `py23_math_floor/ceil` | `(double) -> i64` | Python 的 floor/ceil 返回 int |
| `py23_math_pow/fmod` | `(double, double) -> double` | libm 包装；fmod 除零抛 ValueError |
| `py23_sys_set_args` | `(i64 argc, char** argv) -> void` | 生成 `main` 初始化 argv，列表立即可注册为 GC 根 |
| `py23_sys_argv` | `() -> list*` | 借用全局 argv 列表 |
| `py23_sys_exit` | `(i64 code) -> void` | 直接 `exit(code)`，noreturn |

### 对象

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_obj_new` | `(i64 size, dtor, gc_release) -> void*` | `rc_alloc`，全零；`size < 8` panic |

### 生成器

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_gen_new` | `(i64 n_slots, i64 resume, i64 release, i64 kind) -> void*` | 分配堆帧；kind=1 表示协程 |
| `py23_gen_next` | `(void*) -> i64` | 恢复并取下一个值；结束抛 StopIteration |
| `py23_gen_next_opt` | `(void*, i64* ok) -> i64` | for 循环用：结束写 `ok=0`，不抛异常 |
| `py23_gen_send` | `(void*, i64) -> i64` | 把值送到 `x = yield v` |
| `py23_gen_send_opt` | `(void*, i64, i64* ok) -> i64` | `yield from` 用：转发 send，结束写 `ok=0`；非生成器帧报 TypeError |
| `py23_gen_close` | `(void*) -> void` | 跑挂起点清理；异常向外传 |
| `py23_gen_finalize` | `(void*) -> void` | 引用计数归零时清理；异常吞掉 |
| `py23_handler_suspend` | `(void*) -> void` | yield/await：把 handler 记录从全局栈摘下 |
| `py23_handler_rearm` | `(void*) -> void` | 恢复：重压记录并更新临时值水位 |
| `py23_handler_free` | `(void*) -> void` | close：释放挂起的记录 |
| `py23_coro_rethrow` | `(void*) -> void` | 在 resume 帧里抛出注入异常（noreturn） |
| `py23_asyncio_run` | `(void* coro) -> i64` | 跑事件循环到根协程结束，返回结果或抛异常 |
| `py23_asyncio_sleep` | `(double) -> void*` | 创建毫秒定时 awaitable |
| `py23_asyncio_gather` | `(list*) -> void*` | 创建 gather awaitable（不接管参数列表） |
| `py23_asyncio_create_task` | `(void* coro) -> void*` | 立即调度协程，返回 task awaitable |
| `py23_asyncio_task_done` | `(void*) -> i64` | 任务是否结束 |
| `py23_asyncio_task_cancel` | `(void*) -> i64` | 请求取消；解绑当前 awaitable 并注入 CancelledError |
| `py23_asyncgen_anext` | `(void*) -> void*` | 创建异步生成器的 `__anext__` 驱动对象 |
| `py23_asyncgen_asend` | `(void*, i64) -> void*` | `asend(v)` 驱动对象，v 的引用转交驱动 |
| `py23_asyncgen_aclose` | `(void*) -> void*` | `aclose()` 驱动对象，走 close 分发或注入 GeneratorExit |
| `py23_asyncgen_athrow` | `(void*, excval*) -> void*` | `athrow(exc)` 驱动对象，在挂起点注入异常 |
| `py23_asyncio_future_new` | `() -> void*` | 创建 Future（多等待者、回调列表） |
| `py23_future_set_result` | `(void*, i64) -> void` | 设置结果并唤醒全部等待者 |
| `py23_future_set_exception` | `(void*, str*) -> void` | 设置失败（类型 `Exception`、消息字符串），唤醒等待者并跑回调 |
| `py23_future_add_callback` | `(void*, i64 fn) -> void` | 注册完成回调；已完成则立即同步调用 |
| `py23_call1` | `(void* fn, i64 arg) -> void` | 跳板：按闭包 ABI 调用单参数 py23 函数值 |
| `py23_future_result` | `(void*) -> i64` | 取结果；未完成抛 RuntimeError，失败抛异常副本 |
| `py23_future_exception` | `(void*) -> str*` | 失败消息；成功返回 NULL |
| `py23_future_cancel/done/cancelled` | `(void*) -> i64/bool` | Future 状态 |
| `py23_asyncio_task_result/exception/cancelled` | `(void*) -> i64/str*` | Task 结果与状态 |
| `py23_asyncio_wait_fd` | `(i64 fd, i64 write) -> void*` | 创建 fd 等待对象，事件循环用 poll 唤醒 |

### 引用计数

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_rc_inc` | `(void*) -> void` | 引用 +1；空指针和永生字面量跳过 |
| `py23_rc_inc_unknown` | `(i64) -> i64` | unknown 值按堆注册表保守 retain；不是堆指针就什么也不做 |
| `py23_rc_dec` | `(void*) -> void` | 引用 -1；归零时调 `dtor` 并释放整块 |
| `py23_rc_temp` | `(void*) -> void` | 表达式临时值入栈，接管产生者的引用 |
| `py23_rc_mark` | `() -> i64` | 返回临时值栈当前水位 |
| `py23_rc_cleanup` | `(i64 mark) -> void` | 释放到指定水位 |
| `py23_rc_shutdown` | `() -> void` | 入口函数退出时释放临时栈自身存储 |
| `py23_closure_new` | `(i64 code, i64 n_caps) -> closure*` | 分配闭包负载并挂 cell 析构器 |

### 循环收集

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_gc_init` | `(void** roots, i64 n, void* stack_top) -> void` | 注册全局根表与栈顶，生成的 `main` 调一次 |
| `py23_gc_collect` | `() -> void` | 分配量超过 2MB 时由 `rc_alloc` 自动触发 |
| `py23_gc_unref_child` | `(void*) -> void` | GC 清扫用：对象可达时把引用计数减一 |
| `py23_gc_shutdown` | `() -> void` | 程序退出，释放所有剩余堆对象（含环） |

### 字符串

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_str_concat` | `(str*, str*) -> str*` | 新建对象，两次 `memcpy` |
| `py23_str_char_at` | `(str*, i64) -> str*` | 负索引先加 `len`；越界抛 IndexError；返回单字节新串 |
| `py23_str_from_i64` | `(i64) -> str*` | `%lld` |
| `py23_str_from_f64` | `(double) -> str*` | CPython `repr` 规则，见下 |
| `py23_str_from_bool` | `(i64) -> str*` | `"True"` / `"False"` |
| `py23_str_from_none` | `() -> str*` | `"None"` |
| `py23_str_eq` | `(str*, str*) -> i64` | 逐字节相等，返回 0/1 |
| `py23_str_cmp` | `(str*, str*) -> i64` | 三路比较，返回 -1/0/1 |

`py23_str_cmp` 用 `memcmp`，前提是 **UTF-8 的字节序与码点序一致**（这是 UTF-8 的设计性质），所以结果与 Python 的字符串比较相同。

### 输出

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_print_i64` | `(i64)` | `%lld` |
| `py23_print_f64` | `(double)` | 走 `format_double` |
| `py23_print_bool` | `(i64)` | `True` / `False` |
| `py23_print_str` | `(str*)` | 原样写出 `len` 个字节，不加换行 |
| `py23_print_none` | `()` | `None` |
| `py23_print_sep` | `()` | 一个空格 |
| `py23_print_end` | `()` | 一个换行 |

`print(sep=..., end=...)` 复用 `py23_print_str`——自定义分隔符就是「原样写一个字符串」，没必要单开函数。

### panic

| 函数 | 触发场景 | 输出 |
|---|---|---|
| `py23_panic` | 用户级 panic / 剩余内部错误 | 抛 `RuntimeError`；未捕获时 `py23: uncaught ...` |
| `py23_panic_div0` | 整数除以 / 模以 0 | 抛 `ZeroDivisionError` |
| `py23_panic_overflow` | `INT64_MIN / -1` | 抛 `OverflowError` |

这些错误在有 handler 时可以被 `except` 捕获；没有 handler 时打印
`py23: uncaught <type>: <message>` 并 `exit(1)`，不打印栈回溯——现在没有
调试信息，印出来的栈只会误导。内部不变量错误和 OOM 仍走 `panic_c`
直接退出。

### 不走枚举的辅助函数

| 函数 | 调用方 | 说明 |
|---|---|---|
| `py23_pow_i64` | `gen_bin_int` 的 `Pow` | 快速幂；**负指数返回 0**（CPython 返回 float） |
| `py23_abs_i64` | `abs(int)` | 整数绝对值，回绕语义 |
| `py23_str_eq` / `py23_str_cmp` | `gen_cmp` | 参数形态固定，没必要进枚举 |
| `floor` / `fmod` / `pow` | libm | 浮点 `//`、`%`、`**` 直接用 libm |

## 浮点转字符串

`format_double` 是运行时里最长的一段，规矩跟 CPython 的 `repr` 一致：

1. 从精度 1 往上试，找**最短的、能 `strtod` 读回原值**的数字串。
2. 按小数点位置决定排版：`decpt` 在 `-4 < decpt <= 16` 用定点，否则科学计数法（指数至少两位、带符号）。
3. 整数值补 `.0`。

两个反例说明为什么不能偷懒：

- 直接用 `%g`：`%g` 是「指数 ≥ 精度」就切科学计数法，而最短数字串精度常常只有一两位，于是 `10.0` 被排成 `1e+01`。
- 精度从 17 往下找：会停在「第一个失败的精度之上」，多吐末位数字（`-84.992391262976952` 而不是 `-84.99239126297695`）。

## 内存模型

**引用计数。** 每个堆对象（字符串、列表、元组、字典、集合、类实例、闭包）带隐藏的
`Py23Rc` 头；所有权约定：

- 槽 / 全局 / 字段 / kind 1 列表元素持有引用；
- `load` / `field.get` / `list.get` 只借用，不增加计数；
- 函数形参在入口统一 retain 一份，出口释放；
- 返回值在返回处 retain，调用方拿到的是新引用；
- 每个新堆值带着 `rc=1` 进临时值栈，语句 / 循环 / 函数边界按 mark 清理。

临时值栈让降级器不用做活跃性分析：就算某个分支、`continue`、`break`
跳过一条清理路径，下一层边界也会兜底。代价是纯堆操作会多几条调用；
完全不含临时堆值的语句、循环和函数会把 mark/cleanup 在降级期删掉，
所以整数代码的生成结果与 0.6.1 完全相同。

引用计数处理不了两类东西：**环**和**类型看不见的引用**。它们交给下面的
循环收集器：

- 对象自环、相互递归闭包形成的 cell 环：分配量超过 2MB 时被标记-清扫；
  进程退出时也会清空整个堆。
- `unknown` / `list[unknown]` / 混合元组：正常 RC 不精确 dec，但 GC 会
  保守扫描并回收不可达对象。若某个 unknown 元素指向「仍有其它根」的
  对象，它的计数可能虚高，要到后续轮次才回收。
- unknown 值写入容器或 cell 前，生成代码调用 `py23_rc_inc_unknown`：
  运行时按注册表判断它是不是堆指针，是则 +1。unknown 容器不会 dec，
  这份引用最终由 GC 回收，但保证被引用者不会先于引用者释放。注册表
  维护一份开放寻址索引（墓碑复用、按存活数决定扩容或原地压缩），
  索引本身只随存活对象数增长。
- 唯一的「回收静默性」：没有 `__del__`/弱引用，对象什么时候真正释放
  不可观察；GC 是 stop-the-world、全堆扫描，没有分代/增量。

静态类型的好处是 RC 插桩位置是确定的：赋值、传参、返回、容器写入都是
明确的增减引用点。GC 只是兜底，不替代 RC。

**异常路径**：longjmp 跳过普通函数帧时不会执行 `release_slots`，这些帧
里已经 retain 的局部引用会留在堆上，直到下一轮 GC 按不可达回收。handler
记录本身保存了受保护区的临时值栈水位，landing pad 会把这段临时值精确
清掉；所以异常路径只会延迟局部变量的回收，不会泄漏临时表达式。

## 循环收集器

`rc_alloc` 把每个堆对象挂进双向注册表；`alloc_since_gc` 超过 2MB 就在
下一次分配前触发一轮收集：

1. **建表**：按负载地址建开放寻址哈希表，所有对象 `marked = 0`。
2. **标记**：根包括全局根表（生成代码在 `main` 里注册）、临时值栈、
   真实栈扫描（从当前 `%rsp` 到 `main` 的栈顶）和 `setjmp` 捕获的
   callee-saved 寄存器。标记对象时保守扫描：
   - 列表：`data[0..len)` 每个 8 字节槽；
   - 字典/集合：按 `order` 扫每个条目的 key/value；
   - 用户对象 / 闭包：整个负载按 8 字节扫；
   - 字符串：没有子指针，跳过。
3. **释放引用**：对每个不可达对象调用它的 `release`：
   - 列表 kind 1 逐个 `gc_unref_child` 元素；
   - 字典/集合按 `key_own` / `val_own` 对已知堆 key/value 做 unref；
   - 闭包逐个 cell；
   - 用户对象用 codegen 生成的 `.LgcrelN`，只处理堆字段；
   - `gc_unref_child` 只在子对象 **可达** 时 `rc--`，绝不释放。
   这一步在没有任何对象被 free 之前完成，避免释放顺序造成悬空读取。
4. **清扫**：释放所有不可达对象（列表顺带 `free(data)`，字典释放
   `entries` / `order`），清掉可达对象的 mark 位，释放临时哈希表/工作栈。

保守扫描的代价是「误报只会延迟回收」：栈或寄存器里的整数如果恰好等于
某个堆地址，会把它当根留下；不会把可达对象误判成垃圾。
ASan 下用内联汇编取真实 `%rsp`，避免扫描 ASan fake stack 的未映射区域。

## 加一个运行时函数

1. `src/ir.h`：`RtFn` 加枚举项 + 注释写签名
2. `src/ir.h` 的 `rt_name()`：加 `case` 返回符号名
3. `src/codegen_call.cpp` 的 `gen_call_rt`：按参数形态把实参搬进 `%rdi/%rsi/%rdx/%xmm0`（`-Wswitch` 会强制你加）
4. `runtime/rt_*.c`（先看 `rt_internal.h` 选文件）：实现，函数名与第 2 步一致，参数用 `int64_t` / `double`
5. `docs/runtime.md`：函数表加一行

参数超过 3 个、或者要传浮点时，第 3 步最容易写错——照抄相邻的 `case`，别自己推。

## 未来

- **异常对象接入 RC/GC**：现在是 malloc 的轻量对象；如果以后要支持
  `args`、traceback 或自定义异常类，需要把异常对象改成正常的堆类型。
- **精确根/分代**：现在是保守栈扫描 + 全堆 stop-the-world。要提速可以
  给生成代码加栈映射，或做分代/增量收集。
- **kind 2 元素**：`list[unknown]` 的引用计数仍不精确，靠 GC 兜底。要
  彻底解决，得给容器元素加运行时类型标签，或者把 unknown 从语言里消掉。
