# 原生标准库 cookbook —— 用数据声明替代 C++ 硬编码

> 目标（P0）：**新增一个原生 API = 改一个 `stdlib/sigs/<模块>.pysig` + 重建，
> 零 C++ 编辑。** 本文是格式规格 + 操作手册 + 现状清单。

---

## 0. TL;DR

| 你想做的事 | 以前（硬编码） | 现在（声明式，模块已迁移） |
|---|---|---|
| 给 `time` 加一个新函数 | 改 `builtin_sigs.cpp` + `lower_import.cpp`（2~3 处） | 在 `stdlib/sigs/time.pysig` 加一行 `func` |
| 接通一个已有运行期入口（`pa_*`） | 还要 `ir_rtfn*` / codegen 分组 | 同上，**一行**（符号名指向现成的 `pa_*`） |
| 新模块 | 改 `driver_modules.cpp` 的 `kModuleTable` | 新建 `stdlib/sigs/<模块>.pysig`（模块表自动登记，见 §6.3） |
| 加一个新**运行期实现** | 必须 C（`runtime/rt_*.c`）+ RtFn 编号 + codegen 参数分组 | **仍然要**，这是运行期边界，不是签名问题（§6.4） |

已迁移：`time`（试点）、`os.path`（第二个证据）。未迁移：`math/sys/os/subprocess/datetime/random/collections/json/re/pathlib/operator/asyncio/python/c/dylib`。

---

## 1. 背景：三处硬编码是怎么来的

原生模块（没有 `.pya` 源文件）在编译器里有三张手写表：

1. **签名表** `src/builtin_sigs.cpp`：`builtin_module(name)` 返回 `ModuleInfo`，
   里面按模块名分支，把「属性名 → 类型」塞进 `globals`。类型检查（`typecheck_call_forms.cpp`）
   直接读这份表做实参检查；`from M import f` 的可调用性也靠它。
2. **成员 → RtFn 映射** `src/lower_import.cpp` 的 `Lowerer::native_fn(module, member)`：
   把 `time.sleep` 映射到枚举 `RtFn::TimeSleep`。
3. **降级装填** `src/lower_import_native.cpp` 的 `Lowerer::call_native`：
   正常路径是 `bind_args(c, info, nullptr)` + `call_runtime(fn, ret, args)`（通用，
   由 `FuncInfo` 决定每个实参怎么 coerce）；只有「可选参数/可变参/返回类型随实参变」
   的函数才需要在这里写手工分支（`time.strftime`、`subprocess.run`、`re.findall` ……）。

再加上 `src/ir_rtfn.h`（枚举编号）、`src/ir_rtfn_names.h`（编号 → `pa_*` 符号）、
`src/codegen_call_rt.cpp`（按 RtFn 决定参数落在哪些寄存器类：i0/i1/i2/f1/str…）、
`src/driver_modules.cpp` 的 `kModuleTable`（模块登记）。

**关键观察**：签名（1）和映射（2）是**纯数据**——它们只回答「参数是什么类型」「调到哪个运行期入口」。
真正需要 C++ 的只有：运行期实现、RtFn 编号/参数分组、以及少数「参数装配不规则」的函数。
P0 只把纯数据部分搬进声明文件，其余保持原样。

---

## 2. 选型：运行期读取 `.pysig`（而不是构建期代码生成）

三个候选：

| 方案 | 做法 | 优点 | 缺点 | 结论 |
|---|---|---|---|---|
| (a) 构建期代码生成 | CMake 从声明生成 `*_gen.h`（签名表/枚举） | 零运行期开销；可静态检查 | 要给 CMakeLists 加自定义命令 + 生成文件；声明改动会触发大范围重编；构建图变两步；**而且 P0 期间 CMakeLists 正被别人改** | 否 |
| (b) 运行期读取 | 编译器启动/首次 import 时读 `stdlib/sigs/*.pysig` | 和 `stdlib/*.py` 完全同构（同一份随编译器分发的数据）；无需改构建；header-only 即可；改声明**不重编编译器** | 每次编译多一次 stat+解析（实测 < 0.3ms，§8） | **选它** |
| (c) IR 带符号名（extern 风格） | `call.rt` 直接写 `pa_time_sleep` 字符串 | 彻底摆脱 RtFn 枚举 | 要动 IR + 后端 + 参数分组表（而那张表正是「哪个参数在哪个寄存器」的唯一来源）；等于把 RtFn 换成字符串键，收益小风险大 | 否 |
| (c') (c) 的轻量版：**声明里写 `pa_*` 符号，加载器反查 `rt_name()` 得到 RtFn** | — | 声明文件表达的是**运行期符号**（文档/可读性最好），但下游仍拿到同一个 `RtFn` | 反查要扫一遍表（有界，见 §8） | **选它（作为 (b) 的落地细节）** |

也就是说：**声明格式用 (c) 的符号名表达，实现用 (b) 的运行期加载，下游仍走 (a) 的 RtFn 枚举**。
这样「未迁移模块的生成代码」天然逐字节不变（同一个 RtFn、同一个 FuncInfo 形状），
迁移风险被压到「声明是否等价」这一件事上。

实现是 **header-only** 的 `src/native_sigs.h`：不需要在 `CMakeLists.txt` 里登记新 TU
（P0 期间那里有别人在改分配器调试开关），任何 TU `#include` 即可；
所有函数 `inline`、缓存用函数内 `static`（C++11 magic static，无线程问题、无初始化顺序问题）。

---

## 3. 格式：`stdlib/sigs/<模块>.pysig`

行式语法，`#` 到行尾是注释。**三条语句**：

### 3.1 `module`

```
module time
```

必须出现且只出现一次，且必须与文件名一致（`os.path` → `stdlib/sigs/os.path.pysig`）。
这条检查的用处：防止 `cp time.pysig foo.pysig` 后得到一个内容与名字不符、静默生效的声明。

### 3.2 `func`

```
func <Python 名> <运行期符号> (<形参列表>) -> <返回类型>
```

| 字段 | 含义 | 例子 |
|---|---|---|
| Python 名 | 模块内属性名（`import time; time.sleep` 里的 `sleep`；也是 `from time import sleep` 的名字） | `sleep` |
| 运行期符号 | C 侧入口，必须是 `src/ir_rtfn_names.h` 里 `rt_name()` 表里的现有符号 | `pa_time_sleep` |
| 形参列表 | 见 §4 | `(secs: float)` |
| 返回类型 | 返回类型；无返回值写 `None` | `-> None` |

### 3.3 `const`

```
const <名字> <类型> = <字面量>
const CLOCKS_PER_SEC int = 1000000
const sep str = "/"
```

字面量种类必须与类型匹配（`int/float/str/bool/None`），否则加载期硬失败。
常量进 `ModuleInfo::globals`（类型检查可见），降级由 `builtin_value` 的通用 hook
按字面量直接产出（`const_int` / `const_str` ……）。

---

## 4. 形参与类型

### 4.1 形参

| 写法 | 含义 |
|---|---|
| `x: float` | 普通位置参数 |
| `x: float = -1e300` | 带默认值（调用点不传时降级阶段填字面量） |
| `*, x: int` | `*` 之后是 keyword-only |
| `*args: unknown` | 可变位置参数（类型是**元素**类型） |
| `**kwargs: unknown` | 可变关键字参数 |

### 4.2 类型

| 语法 | 对应 `pyadamas::Type` |
|---|---|
| `int` `float` `bool` `str` `bytes` | 标量 |
| `None` / `none` | `TyKind::None` |
| `unknown` / `Any` / `object` | `TyKind::Unknown`（放行任意实参，不做 coerce） |
| `file` `dylib` `regex` `match` `popen` | 对应不透明句柄类型 |
| `list[T]` `set[T]` `deque[T]` `range[T]` | 容器 |
| `dict[K, V]` | 字典 |
| `tuple[A, B]` | 定长元组 |
| `tuple[T, ...]` | 可变长元组（`mk_tuple_rest`） |
| `optional[T]` | `mk_optional`（T 或 None） |

不在表里的类型名 → 加载期报错并给出行号（**不会**退化成 unknown）。

---

## 5. 加载器与接线

### 5.1 `src/native_sigs.h` 对外接口

```c++
namespace pyadamas::nativesig {
  const ModuleSig* load(const std::string& module);          // 无文件 → nullptr（调用方退回硬编码）
  ModuleInfoPtr    module_info(const std::string& module);   // ModuleSig → 签名表；缓存
  std::optional<RtFn> func_rtfn(module, member);             // 成员 → RtFn；缓存
  const SigConst*  find_const(module, member);               // 声明的常量
  bool             is_declared(module);                      // 该模块是否声明驱动
  std::string      describe(const ModuleSig&);               // 人类可读转储（调试）
}
```

内部：`lex()`（带行号的 token 流）→ `Parser`（递归下降，三条语句）→
`resolve_rt_symbols()`（`pa_*` → `RtFn`，一次扫描、找齐即停）→ 缓存。

### 5.2 接线只有两处（各 3~4 行）

**A. 签名表（`src/builtin_sigs.cpp`，`builtin_module()` 开头，`cpy_proxy` 分支之后）：**

```c++
// 声明式签名优先：stdlib/sigs/<module>.pysig 存在时整模块由它驱动。
if (auto m = nativesig::module_info(name)) {
    cache[name] = m;
    return m;
}
```

**B. 成员 → RtFn（`src/lower_import.cpp` 的 `Lowerer::native_fn()` 开头）：**

```c++
if (auto f = nativesig::func_rtfn(module, member)) return *f;
```

**C.（可选，迁移带常量的模块时用）`src/lower_import.cpp` 的 `Lowerer::builtin_value()` 开头：**

```c++
if (const auto* c = nativesig::find_const(module, member)) {
    switch (c->val.kind) {
        case LiteralValue::Kind::Int:   out = const_int(c->val.i); break;
        case LiteralValue::Kind::Float: out = const_float(c->val.f); break;
        case LiteralValue::Kind::Bool:  out = const_bool(c->val.b); break;
        case LiteralValue::Kind::Str:   out = const_str(c->val.s); break;
        default:                        out = const_none(); break;
    }
    ty = c->ty;
    return true;
}
```

**没有第三处。** 类型检查（`typecheck_call_modules.cpp` / `typecheck_call_forms.cpp`）不需要改：
它对原生模块的普通函数调用走的是通用路径 `check_args(mod->globals[attr]->fn, c)`，
只要 `ModuleInfo` 里有正确的 `FuncInfo` 就够了。
降级也不需要改：`call_native` 的通用尾巴（`bind_args` + `call_runtime`）由 `FuncInfo` 驱动。

> 迁移一个模块**不需要**动：`typecheck_call_modules.cpp`（除非它对该模块有专门分派）、
> `ir_rtfn.h` / `ir_rtfn_names.h` / `codegen_call_rt.cpp`（RtFn 已存在时）、
> `runtime/*.c`（运行期实现已存在时）。
> `driver_modules.cpp` 也只是「新模块」才需要（§6.3）。

### 5.3 失败路径（刻意硬失败）

| 情况 | 行为 |
|---|---|
| `.pysig` 语法错 / 未知语句 / 未知类型 | `error: <file>:<line>: <msg>` + `exit(1)` |
| `module` 名与文件名不一致 | 同上，报第 1 行 |
| 运行期符号不在 `rt_name()` 表里 | 同上，报该 `func` 行 + 符号名 |
| 常量字面量与声明类型不符 | 同上 |
| 重名声明 | 同上 |
| **没有** `.pysig` | 返回 `nullptr`，调用方继续走硬编码（增量迁移的基石） |

**为什么不静默跳过**：静默跳过会表现成「模块没有这个属性」或「实参类型不对」，
和声明文件的内容毫无关系，排查成本极高。数据是随编译器分发的，坏了就该立刻停。

---

## 6. 操作手册

### 6.1 给「已迁移模块」新增一个 API（零 C++）

前提：需要的运行期入口 `pa_*` 已经存在（在 `src/ir_rtfn_names.h` 里能查到）。

1. 在 `stdlib/sigs/<模块>.pysig` 加一行：
   ```
   func uptime pa_time_monotonic () -> float
   ```
2. 重建：`cmake --build build/lead -j16`（只有数据变了，**不会**重编任何 `src/*.cpp`）。
3. 用：
   ```python
   import time
   print(time.uptime() > 0)
   from time import uptime
   print(uptime() > 0)
   ```
4. 验证 `git status`：除了 `stdlib/sigs/<模块>.pysig` 没有任何 `src/` 改动。

> 同一份声明同时管三件事：类型检查（形参/返回类型）、`from M import f`、降级到哪个 `pa_*`。

### 6.2 把一个原生模块迁到声明式

以 `time` 为例（本次试点）：

1. **抄签名**：从 `builtin_sigs.cpp` 的 `name == "<模块>"` 分支抄出每个函数的形参/返回类型；
   从 `lower_import.cpp` 的 `native_fn()` 抄出「成员 → `RtFn::X`」；
   `RtFn::X` 的 `pa_*` 符号从 `src/ir_rtfn_names.h` 查。
2. **写 `stdlib/sigs/<模块>.pysig`**（§3）。
3. **接线**（§5.2 A/B/C，若还没接）。
4. **删硬编码**：`builtin_sigs.cpp` 的分支整段删、`lower_import.cpp` 里该模块的
   `if (member == ...)` 行全删（注意 `lower_import_native.cpp` 的**手工装配分支要留着**，
   它不是签名，是参数装配）。
5. **重建 + 三档等价性**（§7）。
6. 把该模块从 `kModuleTable` 的 Native 行删掉（若启用 §6.3 的自动登记）。

### 6.3 新模块（零 C++，可选能力）

`driver_modules.cpp` 的 `find_module_entry()` 在查表失败后会再看
`stdlib/sigs/<name>.pysig` 是否存在：存在则合成一条 `ModuleKind::Native` 的条目
（名字字符串的生命周期由函数内 `static` 容器持有）。于是：
**新模块 = 新建一个 `.pysig` 文件 + 重建**，`kModuleTable` 一行都不用加。
（这条是「原生模块定义也是数据」的最后一厘米；启用后 §6.2 第 6 步才有意义。）

### 6.4 什么**不能**只靠声明（仍然要 C++）

| 需求 | 为什么 |
|---|---|
| 新的运行期函数 | `runtime/*.c` 要实现；声明只是名字 |
| 新的 `pa_*` 入口 + RtFn 编号 | 要加 `ir_rtfn.h`（枚举）+ `ir_rtfn_names.h`（符号名）——但这是**运行期边界**，一次性的 |
| codegen 参数分组 | `codegen_call_rt.cpp::gen_rt_args` 按 RtFn 决定实参进 i0/f1/str* 等哪个类；新增 RtFn 时**必须**在这里加 case（漏了会生成错调用） |
| 参数装配不规则 | 可选参数需要「不传时填什么」以外的逻辑（`strftime` 的 9 元组、`subprocess.run` 的关键字、`re.findall` 的返回类型随捕获组数变）→ `call_native` 里的手工分支 |
| 构造器（`collections.Counter`、`pathlib.Path` …） | 需要类/对象语义，声明格式暂不表达（`module_ctor`） |
| 属性读降级 | `builtin_value`：常量已支持（§5.2 C），其它（如 `os.environ` 是调用不是读）走各自 hook |

---

## 7. 验证方法（迁移一个模块的验收脚本）

1. **套件**：`PYADAMAS_TEST_JOBS=16 bash tests/run_tests.sh build/gfix-p0 build/lead/pyadamas`
2. **12 基准三档输出一致**。
3. **归一化符号后汇编 36/36 逐字节相同**：迁移只应改变「签名从哪来」，不应改变任何生成指令。
   若某条汇编变了 → 声明与硬编码不等价（最常见：`ret_known` 语义、形参写成 `unknown`
   导致不做 coerce、或漏了某个 `RtFn` 的 codegen 分组）。
4. **全闸门**：`bash tools/checkall.sh --full`（`--perf` 报红先判定是真回归还是噪声）。

---

## 8. 性能（本机实测）

| 项 | 实测 |
|---|---|
| `time.pysig`（8 个 func，1.6 KB）首次 load+解析（含文件 stat + 词法 + 语法 + 符号反查） | **0.1 ~ 0.35 ms**（一次编译一次；进程内缓存） |
| 缓存命中 `func_rtfn()` | ~0.02 µs |
| `pa_*` → `RtFn` 反查 | 一次扫描、找齐即停（`time` 的 8 个符号都在 RtFn 表前半段，~2 µs） |
| 未迁移模块 | **0**（不 stat、不解析） |
| **端到端**：编译 `import time` 的程序 vs 编译 `import math`（无声明）的程序，各 15 次取最小 | **738 ms vs 737 ms**（差 ≈ 1 ms，在测量噪声内；机制本身的净成本 < 0.35 ms） |

只读被 `import` 的模块：`load()` 只在该模块第一次被 `module_info` / `func_rtfn` 问到时才触发。
`time` 迁移后单次编译的净增开销 < 0.35 ms，远低于 5 ms 的预算。

> 附带好处：**改声明文件不需要重新构建编译器**——`stdlib/sigs/*.pysig` 不是编译进二进制的数据。
> 实测改完 `time.pysig` 后 `cmake --build` 输出 `ninja: no work to do`。

---

## 9. 踩坑记录

1. **`typecheck_types.h` 不能单独 include**：它用了 `CKind`（定义在 `cimport.h`），
   单独 include 会报一堆 `CKind does not name a type`。加载器要 include `module.h`（它带全依赖链）。
2. **`RtFn` 没有 `Count` 哨兵**，`rt_name()` 对未定义编号返回 `?`。
   反查表只能扫整个值域；好在 `RtFn` 底层是 `uint16_t`（固定底层类型 → 整值域合法转换），
   而且可以「找齐即停」，正常路径只扫表的前一小段。
3. **形参名不要照抄** `builtin_fn()` 的 `x0/x1`。硬编码表用自动名，声明用真名（`secs`）是
   **超集**（`time.sleep(secs=0.01)` 从「关键字参数不支持」变成可用），不影响位置调用生成的指令。
4. **`FuncInfo::ret_known` 必须置 `true`**，否则调用点会把返回值当「还没推断出来」处理。
5. **`ModuleInfo::builtin = true` 必须置位**：`merge_extension_module`、`from M import` 的
   可调用性判断、check 模式都靠它区分「原生」和「有源码的模块」。
6. 声明文件里的 `module` 名可以带点（`os.path`），文件名也跟着带点（`os.path.pysig`）；
   但类型名不允许带点。
7. **`*` / `...` 的歧义**：`*` 出现在形参位置要根据后面是不是 `name :` 区分
   「`*args: T`」与「keyword-only 分界」；`...` 只在 `tuple[T, ...]` 里有意义。

---

## 10. 现状清单

| 模块 | 状态 | 备注 |
|---|---|---|
| `time` | ✅ 已迁移（`stdlib/sigs/time.pysig`） | 8 个函数 + 2 个常量；`strftime` 的参数装配仍在 `lower_import_native.cpp`（签名已声明） |
| `os.path` | ✅ 已迁移（`stdlib/sigs/os.path.pysig`） | 16 个函数；常量 `sep/altsep/curdir/pardir` 属于 `os` 模块，不在本文件 |
| `math` `sys` `os` `random` `collections` `json` `re` | ⬜ 未迁移 | 硬编码仍在；加载器已就绪，可按 §6.2 逐个搬 |
| `pathlib` `operator` `asyncio` | ⬜ 未迁移（优先级低） | 构造器/类语义多，声明格式表达不了的部分留 C++ |
| `datetime` `subprocess` | ⬜ 未迁移（优先级低） | 参数装配/返回类型随实参变的分支多 |
| `python`（CPython 桥）、`c` / `dylib` | ⛔ 不适合声明式 | 名字集合是开集（`from c import <任意符号>`），签名在调用点推断 |

### 后续迁移建议（按性价比）

1. `os.path`：纯字符串/布尔，几乎全是 1 参数函数，是最好的第二个证据（顺便把
   `sep/altsep/curdir/pardir` 用 `const` 表达）。
2. `sys`：函数只有 `exit/exc_info`，但有一堆常量（`argv/maxsize/stdout`）——
   先在 §5.2 C 的常量 hook 上验证。
3. `math`：函数多但形状最规整（`f(float...) -> float`），一次性搬完收益最大。
4. `random` / `collections` / `json` / `re`：带「返回类型随实参变」的分支，
   先只搬签名、保留手工降级分支。
5. 最后再考虑把 `os` / `datetime` 用 `.pya` + `.cpya`（cimport libc）重写成纯标准库——
   那时声明文件只留「薄封装」。

---

## 11. P0 落地记录（本机实测证据）

### 11.1 接线 diff 规模

| 文件 | 变化 | 说明 |
|---|---|---|
| `src/native_sigs.h` | **新增 670 行** | 加载器（格式/词法/语法/符号反查/缓存/失败路径），一次性成本 |
| `src/builtin_sigs.cpp` | +18 / −26 | 7 行 hook + include；删 `time` 段 40 行、`os.path` 段 21 行 |
| `src/lower_import.cpp` | +25 / −22 | 2 行 hook + 常量 hook；删 `time` 段 12 行、`os.path` 段 20 行 |
| `src/driver_modules.cpp` | +63 / −1 | `.pysig` 自动登记（新模块零 C++） |
| `stdlib/sigs/time.pysig` | 33 行 | 8 func + 2 const |
| `stdlib/sigs/os.path.pysig` | 25 行 | 16 func |

迁移 `time` 净删 **52 行**硬编码（40 + 12），迁移 `os.path` 净删 **41 行**（21 + 20）；
两模块合计 93 行 C++ 硬编码被 58 行声明替代。

### 11.2 等价性证据

| 检查 | 结果 |
|---|---|
| 12 基准 × 3 档（O0/O1/O2）输出 | 36/36 与 CPython 逐字节相同 |
| 12 基准 × 3 档 `asm` 输出（vs 改动前二进制 `build/gfix-stdlib-b/pyadamas`） | **36/36 `cmp` 逐字节相同**（无需归一化符号：`asm` 输出里没有随模块名/版本变化的符号） |
| **迁移模块自身的 asm**（`time` 的 4 个原有函数 + `os.path` 的 16 个函数，O0/O2） | **4/4 逐字节相同**（与改动前二进制 `build/gfix-stdlib-b/pyadamas`）——声明与硬编码完全等价 |
| **硬编码确实删了**：把 `time.pysig` 的 `func sleep` 行临时删掉 | `time.sleep` 立刻变成 `error: module 'time' has no attribute 'sleep'`（若硬编码还在，它会照旧可用）；恢复声明后编译通过 |
| 全量套件 | **425 ran ok / 0 失败**（基线 422 + 520/522/523 三个新用例） |
| `tools/checkall.sh --full` | **全绿**：suite 425/0、difftest 397/0、bench 36/36、check_size、lsp-smoke、simd、perf-baseline（指令数变化 ≤0.02%，warn=0 fail=0） |
| 类型检查真的用声明 | `time.sleep("x")` → `error: argument 'secs' of 'sleep' expects 'float', got 'str'`（参数名来自 .pysig，硬编码时代是 `x0`） |
| 关键字参数（声明带来） | `time.sleep(secs=0.001)` 可用（硬编码签名没有真参数名，做不到） |

### 11.3 「零 C++」演示（两例，`git status -- src/` 的 diff 指纹不变）

1. **给已迁移模块加 API**：往 `stdlib/sigs/time.pysig` 追加
   ```
   const CLOCK_REALTIME  int = 0
   const CLOCK_MONOTONIC int = 1
   ```
   → `cmake --build` 输出 `ninja: no work to do`（数据不进二进制）→
   `import time; print(time.CLOCK_MONOTONIC)` 输出 `1`，与 CPython 一致。
2. **加一个全新原生模块**：只新建 `stdlib/sigs/clock.pysig`
   （`module clock` / `func now pa_time_time () -> float` / `const TICKS_PER_SEC int = 100`），
   不碰 `kModuleTable`、不碰任何 `src/` →
   `import clock; print(clock.now() > 0); print(clock.TICKS_PER_SEC)` 输出 `True` / `100`。
   验证后删除该演示文件。

两例期间 `git status --porcelain -- src/` 的 md5 与操作前完全一致（`8316add7...`）。

### 11.4 已知边界

- `os` 模块的 `environ` / `walk` / `system` 与 `time` 的 `strftime` 仍保留 C++ 降级分支：
  它们不是签名问题，是「参数装配/返回类型随实参变」的问题（§6.4）。
- `driver_modules.cpp` 的自动登记只覆盖 `stdlib/sigs/*.pysig`；纯 `.py` 新模块（`codecs` 这类）仍然要登记 `kModuleTable`，
  两件事不要混。
