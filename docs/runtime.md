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
typedef struct {
    int64_t rc;             /* INT64_MIN = 永生（字面量） */
    void (*dtor)(void*);    /* rc 归零时回调，字符串为 NULL */
} Py23Rc;                   /* 位于负载之前 16 字节，对象指针不指向它 */

typedef struct { int64_t len; int64_t hash; const char* data; } Py23Str;
typedef struct { int64_t len; int64_t cap; int64_t* data; int64_t elem_kind; } Py23List;
/* 用户对象没有对应的 C struct：布局由编译器按类定义生成。
   负载 +0 是 vtable 指针，字段从 +8 起，每个 8 字节；隐藏的 Py23Rc
   头在负载前面，所以既有偏移一个都没变。 */
```

分配统一走 `rc_alloc(size, dtor)`，返回负载指针；`py23_rc_inc` / `py23_rc_dec`
从负载指针往前 16 字节找到头。`rc_dec` 归零时先调 `dtor`，再释放整块内存。

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
时 dec 被截掉的元素。kind 2 会泄漏元素引用，这是「宁可泄漏，不可误杀」的代价。

### 用户对象

对象是编译器按类定义排出来的连续内存，运行时只知道「多大」和析构器：

```c
void* py23_obj_new(int64_t size, void (*dtor)(void*));
```

调用方（生成代码）紧接着把 vtable 地址写到偏移 0。分配全零：0 同时是
`0` / `None` / 假，和栈槽「未赋值就是 None」的约定一致。

析构器由 codegen 按类布局生成，只 `rc_dec` 堆类型字段；没有堆字段的类传
`NULL`。子类析构器包含父类字段，顺序与对象布局一致。

vtable 是静态数据，在 `.data.rel.ro` 里，由 codegen 按 `IRClass::vtable` 生成，运行时不需要知道它的存在。

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

**引用计数。** 每个堆对象带隐藏的 `Py23Rc{rc, dtor}` 头；所有权约定：

- 槽 / 全局 / 字段 / kind 1 列表元素持有引用；
- `load` / `field.get` / `list.get` 只借用，不增加计数；
- 函数形参在入口统一 retain 一份，出口释放；
- 返回值在返回处 retain，调用方拿到的是新引用；
- 每个新堆值带着 `rc=1` 进临时值栈，语句 / 循环 / 函数边界按 mark 清理。

临时值栈让降级器不用做活跃性分析：就算某个分支、`continue`、`break`
跳过一条清理路径，下一层边界也会兜底。代价是纯堆操作会多几条调用；
完全不含临时堆值的语句、循环和函数会把 mark/cleanup 在降级期删掉，
所以整数代码的生成结果与 0.6.1 完全相同。

不是所有东西都会回收：

- **循环引用没人收**：`a.next = a` 这种环会一直活着；还没有标记-清扫。
- **unknown 类型不插桩**：机器表示可能是整数，按指针 dec 会崩。unknown
  值以及 `list[unknown]` / 混合类型元组的元素会泄漏。
- 进程退出时入口函数会释放全局变量并关掉临时栈，ASan 的泄漏检查
  因此仍能保持零报告（有环的情况除外）。

静态类型的好处是插桩位置是确定的：赋值、传参、返回、容器写入都是明确
的增减引用点，不需要保守扫描栈和寄存器。后续做闭包和异常时，cell 与
异常对象也要遵守同一套所有权约定。

## 加一个运行时函数

1. `src/ir.h`：`RtFn` 加枚举项 + 注释写签名
2. `src/ir.h` 的 `rt_name()`：加 `case` 返回符号名
3. `src/codegen.cpp` 的 `gen_call_rt`：按参数形态把实参搬进 `%rdi/%rsi/%rdx/%xmm0`（`-Wswitch` 会强制你加）
4. `runtime/rt.c`：实现，函数名与第 2 步一致，参数用 `int64_t` / `double`
5. `docs/runtime.md`：函数表加一行

参数超过 3 个、或者要传浮点时，第 3 步最容易写错——照抄相邻的 `case`，别自己推。

## 未来

- **循环收集**：现在只有引用计数，环永远不释放。要做得加一个试验性的
  标记-清扫，至少覆盖列表和用户对象。
- **闭包 / 异常接入**：cell 和异常对象也要遵守「存储持有、读取借用、
  临时值入栈」的约定，直接复用 `rc_alloc` 与临时值栈。
- **kind 2 元素**：`list[unknown]` 现在只 retain 不 release。要彻底解决，
  得给容器元素加运行时类型标签，或者把 unknown 从语言里消掉。
