# 运行时

`runtime/rt.c`，纯 C11，编成 `rt.o` 后由 `py23 build` 链接进每个生成的程序，只依赖 libc 和 libm。**py23 编译器本身不链接它**（`PY23_RT_OBJ` 只是把路径编进二进制，供 `cc` 使用）。

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
    int64_t kind;           /* STR / LIST / OBJ / CLOSURE */
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
- 删除暂不支持；负载因子超过 0.7 时翻倍 rehash，`order` 重排后仍保持
  插入顺序。
- set 就是 `val_own = 0`、值恒 0 的 dict，复用同一套函数和 GC。

### 用户对象

对象是编译器按类定义排出来的连续内存，运行时只知道「多大」和析构器：

```c
void* py23_obj_new(int64_t size, void (*dtor)(void*), void (*gc_release)(void*));
```

调用方（生成代码）紧接着把 vtable 地址写到偏移 0。分配全零：0 同时是
`0` / `None` / 假，和栈槽「未赋值就是 None」的约定一致。

析构器由 codegen 按类布局生成，只 `rc_dec` 堆类型字段；没有堆字段的类传
`NULL`。子类析构器包含父类字段，顺序与对象布局一致。

vtable 是静态数据，在 `.data.rel.ro` 里，由 codegen 按 `IRClass::vtable` 生成，运行时不需要知道它的存在。

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

## 函数表

`RtFn` 枚举见 `src/ir.h`，名字映射见 `rt_name()`。生成代码按枚举直接 `call py23_xxx`，不做符号查找。

### 列表

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_list_new` | `(i64 cap, i64 elem_kind) -> Py23List*` | 建空表，容量至少 4 |
| `py23_list_append` | `(Py23List*, i64) -> void` | 追加，满则翻倍 `realloc`；不隐式 retain |
| `py23_list_get` | `(Py23List*, i64) -> i64` | 负索引先加 `len`；越界 panic；返回借用 |
| `py23_list_set` | `(Py23List*, i64, i64) -> void` | 同上，写入；kind 1 时 dec 旧元素 |
| `py23_list_set_len` | `(Py23List*, i64) -> void` | 改 `len`；kind 1 时 dec 被截掉的元素 |

### 字典 / 集合

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_dict_new` | `(i64 key_kind, i64 key_own, i64 val_own) -> Py23Dict*` | 空表，初始 8 槽；set 传 `val_own = 0` |
| `py23_dict_set` | `(Py23Dict*, i64 key, i64 val) -> void` | 插入/更新；更新时释放新 key 引用并 dec 旧 value |
| `py23_dict_get` | `(Py23Dict*, i64 key) -> i64` | 缺失 panic KeyError；返回借用 |
| `py23_dict_contains` | `(Py23Dict*, i64 key) -> i64` | `in` 用，返回 0/1 |
| `py23_dict_key_at` | `(Py23Dict*, i64 index) -> i64` | 按插入顺序取 key，遍历用 |
| `py23_list_contains` | `(Py23List*, i64 value, i64 cmp_kind) -> i64` | list/tuple 的 `in`；0 int / 1 str / 2 float / 3 raw |
| `py23_str_contains` | `(Py23Str*, Py23Str*) -> i64` | 子串查找 |

### 对象

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_obj_new` | `(i64 size, void (*dtor)(void*)) -> void*` | `rc_alloc`，全零；`size < 8` panic |

### 引用计数

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_rc_inc` | `(void*) -> void` | 引用 +1；空指针和永生字面量跳过 |
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
| `py23_str_char_at` | `(str*, i64) -> str*` | 负索引先加 `len`；越界 panic；返回单字节新串 |
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
| `py23_panic` | `assert` 失败 | `py23: panic: <msg>` |
| `py23_panic_div0` | 整数除以 / 模以 0 | `py23: ZeroDivisionError: ...` |
| `py23_panic_overflow` | `INT64_MIN / -1` | `py23: OverflowError: ...` |

全部 `exit(1)`，不打印栈回溯——现在没有调试信息，印出来的栈只会误导。

### 不走枚举的辅助函数

| 函数 | 调用方 | 说明 |
|---|---|---|
| `py23_pow_i64` | `gen_bin_int` 的 `Pow` | 快速幂；**负指数返回 0**（CPython 返回 float） |
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
- 唯一的「回收静默性」：没有 `__del__`/弱引用，对象什么时候真正释放
  不可观察；GC 是 stop-the-world、全堆扫描，没有分代/增量。

静态类型的好处是 RC 插桩位置是确定的：赋值、传参、返回、容器写入都是
明确的增减引用点。GC 只是兜底，不替代 RC。

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
3. `src/codegen.cpp` 的 `gen_call_rt`：按参数形态把实参搬进 `%rdi/%rsi/%rdx/%xmm0`（`-Wswitch` 会强制你加）
4. `runtime/rt.c`：实现，函数名与第 2 步一致，参数用 `int64_t` / `double`
5. `docs/runtime.md`：函数表加一行

参数超过 3 个、或者要传浮点时，第 3 步最容易写错——照抄相邻的 `case`，别自己推。

## 未来

- **异常接入**：异常对象也要遵守「存储持有、读取借用、临时值入栈」的
  约定，直接复用 `rc_alloc`、临时值栈和 GC。
- **精确根/分代**：现在是保守栈扫描 + 全堆 stop-the-world。要提速可以
  给生成代码加栈映射，或做分代/增量收集。
- **kind 2 元素**：`list[unknown]` 的引用计数仍不精确，靠 GC 兜底。要
  彻底解决，得给容器元素加运行时类型标签，或者把 unknown 从语言里消掉。
