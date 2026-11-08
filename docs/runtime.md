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
typedef struct { int64_t len; int64_t hash; const char* data; } Py23Str;
typedef struct { int64_t len; int64_t cap; int64_t* data; } Py23List;
/* 用户对象没有对应的 C struct：布局由编译器按类定义生成。
   +0 是 vtable 指针，字段从 +8 起，每个 8 字节。 */
```

### 字符串

`len` 放在**偏移 0**，这是刻意的：`len(s)` 编译成一条 `movq (%rax), %rax`，`if s:` 也只是读这个字段。`hash` 预留给字符串驻留，现在恒为 0。

字符串有两种来源，布局完全一致，运行时函数不需要区分：

| 来源 | 分配方式 | 生命周期 |
|---|---|---|
| 字面量 | codegen 在 `.data.rel.ro` 里静态构造 | 与进程同寿，永不释放 |
| 拼接 / 转换 | `malloc(sizeof(Py23Str) + len + 1)`，字符区紧跟结构体 | 永不释放（目前） |

堆分配的字符串尾部补 `\0`，方便调试时直接当 C 字符串看；但长度以 `len` 为准，内嵌 `\0` 是合法的。

### 列表

`cap` 是已分配容量，`len` 是有效元素数，`data` 是 8 字节槽数组。元素统一占 8 字节：整数直接放，浮点放位模式（`memcpy` 转换），指针放地址。**没有运行时类型标签**——取值时按什么类型解释由编译期决定。

`py23_list_new(cap)` 最小容量 4；`append` 满了就翻倍。

### 用户对象

对象是编译器按类定义排出来的连续内存，运行时只知道「多大」：

```c
void* py23_obj_new(int64_t size);   /* calloc(1, size)，size < 8 报错 */
```

调用方（生成代码）紧接着把 vtable 地址写到偏移 0。用 `calloc` 而不是 `malloc`：全零同时是 `0` / `None` / 假，和栈槽「未赋值就是 None」的约定一致。

vtable 是静态数据，在 `.data.rel.ro` 里，由 codegen 按 `IRClass::vtable` 生成，运行时不需要知道它的存在。

## 函数表

`RtFn` 枚举见 `src/ir.h`，名字映射见 `rt_name()`。生成代码按枚举直接 `call py23_xxx`，不做符号查找。

### 列表

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_list_new` | `(i64 cap) -> Py23List*` | 建空表，容量至少 4 |
| `py23_list_append` | `(Py23List*, i64) -> void` | 追加，满则翻倍 `realloc` |
| `py23_list_get` | `(Py23List*, i64) -> i64` | 负索引先加 `len`；越界 panic |
| `py23_list_set` | `(Py23List*, i64, i64) -> void` | 同上，写入 |
| `py23_list_set_len` | `(Py23List*, i64) -> void` | 直接改 `len`（`list.pop` 用） |

### 对象

| 函数 | 签名 | 语义 |
|---|---|---|
| `py23_obj_new` | `(i64 size) -> void*` | `calloc(1, size)`，全零；`size < 8` panic |

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

**只分配，不释放。** 没有 GC、没有引用计数、没有 `free`。

- 字面量和常量在只读段，不占堆。
- 拼接、转换、列表扩容都走 `malloc` / `realloc`。
- 对象走 `calloc`，生命周期由使用者决定（目前也是永不释放）。
- 长生命周期的程序会稳定泄漏，现在只适合跑短程序或一次性计算。

这是当前最大的技术债，见 [roadmap.md](roadmap.md)。静态类型的好处是插桩位置是确定的：每个赋值、每次传参、每个返回点都是明确的增减引用点，不需要保守扫描栈和寄存器。

## 加一个运行时函数

1. `src/ir.h`：`RtFn` 加枚举项 + 注释写签名
2. `src/ir.h` 的 `rt_name()`：加 `case` 返回符号名
3. `src/codegen.cpp` 的 `gen_call_rt`：按参数形态把实参搬进 `%rdi/%rsi/%rdx/%xmm0`（`-Wswitch` 会强制你加）
4. `runtime/rt.c`：实现，函数名与第 2 步一致，参数用 `int64_t` / `double`
5. `docs/runtime.md`：函数表加一行

参数超过 3 个、或者要传浮点时，第 3 步最容易写错——照抄相邻的 `case`，别自己推。

## 未来

引用计数会改 ABI：对象头要加 `refcount` 字段，且所有返回堆对象的运行时函数都隐含「返回一个引用」。这件事要和闭包（cell 也要引用计数）、异常（异常对象也要）一起设计，单独改运行时没有意义。
