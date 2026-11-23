# 原生标准库 cookbook —— 用数据声明替代 C++ 硬编码

> 目标（P0）：**新增一个原生 API = 改一个 `stdlib/sigs/<模块>.pysig` + 重建，
> 零 C++ 编辑。** 本文是格式规格 + 操作手册 + 现状清单。

---

## 0. TL;DR

| 你想做的事 | 以前（硬编码） | 现在（声明式，模块已迁移） |
|---|---|---|
| 给 `time` 加一个新函数 | 改 `builtin_sigs.cpp` + `lower_import.cpp`（2~3 处） | 在 `stdlib/sigs/time.pysig` 加一行 `def f(...) -> T: pass` |
| 接通一个已有运行期入口（`pa_*`） | 还要 `ir_rtfn*` / codegen 分组 | 同上，**一行**（符号名按 `pa_<模块>_<名字>` 约定推导） |
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

## 3. 格式：`stdlib/sigs/<模块路径>.pysig`（Python 语法）

声明文件就是 **Python 语法的 stub**：函数体只有 `pass`（可带 docstring），没有实现。
扩展名保持 `.pysig` 标明「声明不是实现」；**模块名 = 相对 `stdlib/sigs/` 的路径**
（`os/path.pysig` → 模块 `os.path`）。

```
# stdlib/sigs/time.pysig
def time() -> float: pass
def sleep(secs: float) -> None: pass
def localtime(secs: float = -1e300) -> tuple[int, ...]: pass
def strftime(*args: unknown) -> str: pass

CLOCK_MONOTONIC: int = 1        # 常量：带注解的赋值，编译期内联

@native("pa_time_time")         # 仅在「约定推导的符号」不适用时写
def now() -> float: pass
```

### 3.1 顶层只允许四类语句

| 语句 | 含义 |
|---|---|
| 模块 docstring（可选，只能一条） | 说明性文字，忽略 |
| `def f(...) -> T: pass` | 声明一个 native 函数（body 只能是 `pass`，可带 docstring） |
| `NAME: T = <字面量>` | 声明一个模块常量（编译期内联） |
| `@native("pa_xxx")` | 装饰在 def 上，覆盖运行期符号 |

其它任何语句（`class` / `import` / `x = 1` / `if` / `async def` …）一律报错并给行号。

### 3.2 运行期符号：约定优先，装饰器覆盖

- 约定：`pa_<模块名，点变下划线>_<函数名>`
  —— `time.sleep` → `pa_time_sleep`；`os.path.join` → `pa_os_path_join`。
- 例外：`@native("pa_xxx")` 覆盖（参数必须是一个非空字符串字面量）。
- 符号必须能反查到 `rt_name()` 表里的现有入口，否则硬失败（防「凭空造编号」）。

### 3.3 为什么没有 `module` 语句

路径即身份。没有声明语句，就不存在「声明名与文件名不一致」，也不会出现
「复制一个声明文件改内容」这种静默事故。`import` 解析被禁止落进 `stdlib/sigs/`
（见 §5.3），杜绝「把 stub 当实现 import」。

---

## 4. 形参与类型（复用编译器的注解解析）

函数签名与普通 Python 函数标注**完全同一套语义**：`TypeChecker::parse_annotation_for_decl()`
= `parse_annotation()`（3 行转发的 public 薄封装）。**类型语法只有一份**，
新增/修改类型（比如新加 `bytes`/`file`/`dylib`）不需要动声明格式。

### 4.1 形参（就是 Python 的形参）

| 写法 | 含义 |
|---|---|
| `x: float` | 普通位置参数 |
| `x: float = -1e300` | 带默认值（必须是字面量；调用点省略时降级阶段填它） |
| `*, x: int` | keyword-only |
| `*args: unknown` | 可变位置参数（类型是**元素**类型） |
| `**kwargs: unknown` | 可变关键字参数 |

每个形参**都必须有注解**，返回类型也**必须写**（`-> None` 表示无返回值）：
native 签名不写类型等于让调用点放弃 coerce，运行期会按错的 ABI 传参。

### 4.2 类型

直接写编译器认识的类型名（与函数标注一致），常用的：

| 类型 | 说明 |
|---|---|
| `int` `float` `bool` `str` `bytes` | 标量 |
| `None` | 无返回值 |
| `unknown`（或 `Any` / `object`） | 放行任意实参、不做 coerce |
| `list[T]` `set[T]` `dict[K, V]` `tuple[A, B]` `tuple[T, ...]` `Optional[T]` | 容器 |
| `file` `dylib` `regex` `match` `popen` | 不透明句柄 |

**解析成 `unknown` 的注解会被拒绝**（除了显式写 `unknown`/`Any`/`object`）：
拼错的名字、Protocol（`Sequence` 等）、未登记的类都会报
「`'Foo' is not a supported native signature type`」——否则声明静默退化成 unknown，
调用点不再 coerce，运行期按错的 ABI 传参。

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
| Python 语法错 | `error: <file>:<line>: <msg>` + `exit(1)` |
| 顶层出现非白名单语句（`class` / `import` / `x = 1` / `if` …） | 同上，报该语句行 |
| 函数体不是 `pass`（或 `pass` 之外还有语句） | 同上，报违规语句行 |
| 未知/不支持的类型（含拼错、Protocol、未登记类） | 同上，报参数/返回所在行 |
| 形参或返回**缺注解** | 同上 |
| 模块名非法（路径穿越、空段、非法字符） | 同上，`stdlib/sigs` + 说明 |
| 运行期符号不在 `rt_name()` 表里 | 同上，报该 `def` 行 + 符号名 |
| 重名声明 | 同上 |
| 常量字面量与声明类型不符 / 缺注解 | 同上 |
| 装饰器不是 `@native("pa_xxx")` | 同上 |
| **没有** `.pysig` | 返回 `nullptr`，调用方继续走硬编码（增量迁移的基石） |
| **import 命中 `stdlib/sigs/`** | 硬失败并说明「声明目录不是实现」（§6.3 的防护） |

**为什么不静默跳过**：静默跳过会表现成「模块没有这个属性」或「实参类型不对」，
和声明文件的内容毫无关系，排查成本极高。数据是随编译器分发的，坏了就该立刻停。

---

## 6. 操作手册

### 6.1 给「已迁移模块」新增一个 API（零 C++）

前提：需要的运行期入口 `pa_*` 已经存在（在 `src/ir_rtfn_names.h` 里能查到）。

1. 在 `stdlib/sigs/<模块路径>.pysig` 加一行 Python stub：
   ```
   def uptime() -> float: pass
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
`stdlib/sigs/<模块路径>.pysig` 是否存在：存在则合成一条 `ModuleKind::Native` 的条目
（名字字符串的生命周期由函数内 `static` 容器持有）。于是：
**新模块 = 新建一个 `.pysig` 文件 + 重建**，`kModuleTable` 一行都不用加。
（这条是「原生模块定义也是数据」的最后一厘米；启用后 §6.2 第 6 步才有意义。）

> 该目录同时被 import 防护：`resolve_module_path()` 命中 `stdlib/sigs/` 时直接报错
> （`import sigs` / `from sigs import demo` / `import sigs.demo as d` 三种形态实测都拒绝），
> 普通 stdlib 模块不受影响。

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

1. **`TypeChecker::parse_annotation*` 原本是 private**：复用编译器注解解析前，先在
   `typecheck.h` 的 public 段加了一个 3 行薄封装 `parse_annotation_for_decl()`
   （转发到 `parse_annotation`，语义零变化）。**不要**在加载器里复制一份类型解析——
   那正是这次换语法的起因（两套类型语法必然漂移）。
2. **`parse_annotation` 对不认识的名字返回 `unknown` 而不是报错**（用户代码里
   前向引用/自造类型要放行）。声明文件必须自己判：注解解析成 unknown 且源表达式不是
   显式 `unknown/Any/object` → 报错。否则拼错类型名不会有人发现。
3. **默认值要走 `literal_of_expr()`**（`typecheck_types.h` 的自由函数）：它已经处理了
   `-1e300` 这种「一元负号包数字常量」的折叠，不要自己写。
4. **Python 的默认值对齐规则**：`args.defaults.size()` 作用于**最后** N 个位置参数
   （posonly + args 拼起来之后），不是从头对齐。
5. **`Arg` 没有行号**：形参相关的错误只能用函数行号（`fd.line`）；body/装饰器/
   常量有各自的行号。
6. **`RtFn` 没有 `Count` 哨兵**，`rt_name()` 对未定义编号返回 ``?`。
   反查表只能扫整个值域；好在 `RtFn` 底层是 `uint16_t`（固定底层类型 → 整值域合法转换），
   而且可以「找齐即停」，正常路径只扫表的前一小段（~2µs）。
7. **`FuncInfo::ret_known` 必须置 `true`**、**`ModuleInfo::builtin` 必须置位**：
   前者决定调用点是否把返回值当「未知」，后者是 `from M import` 可调用性/check 模式的分界。
8. **import 防护是必须的**：`stdlib/sigs/` 是声明目录，若 import 解析命中它，用户会拿到
   「全是 pass 的假模块」（静默错误）。`resolve_module_path()` 在 canonical 化之后检查路径
   前缀，命中即硬失败并说明原因（见 §5.3）。
9. **解析成本**：`Lexer + Parser + TypeChecker` 解析一个 30 行的 stub 约 0.1–0.4ms，
   只对**被 import 且存在声明文件**的模块发生一次（进程内缓存）。改声明文件不需要重编编译器。

---

## 10. 现状清单

| 模块 | 状态 | 备注 |
|---|---|---|
| `time` | ✅ 已迁移（`stdlib/sigs/time.pysig`，Python 语法） | 8 个函数 + 2 个常量；`strftime` 的参数装配仍在 `lower_import_native.cpp`（签名已声明） |
| `os.path` | ✅ 已迁移（`stdlib/sigs/os/path.pysig`，Python 语法） | 16 个函数（路径即模块名）；常量 `sep/altsep/curdir/pardir` 属于 `os` 模块，不在本文件 |
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

1. **给已迁移模块加 API**：往 `stdlib/sigs/time.pysig` 追加两行 Python stub
   ```
   CLOCK_REALTIME: int = 0
   CLOCK_MONOTONIC: int = 1
   ```
   → `cmake --build` 输出 `ninja: no work to do`（数据不进二进制）→
   `import time; print(time.CLOCK_MONOTONIC)` 输出 `1`，与 CPython 一致。
2. **加一个全新原生模块**：只新建 `stdlib/sigs/clock.pysig`
   ```
   @native("pa_time_time")
   def now() -> float: pass

   TICKS_PER_SEC: int = 100
   ```
   不碰 `kModuleTable`、不碰任何 `src/`（注意：`now` 不在约定符号
   `pa_clock_now` 上，所以用 `native` 装饰器指到已有的 `pa_time_time`）→
   `import clock; print(clock.now() > 0); print(clock.TICKS_PER_SEC)` 输出 `True` / `100`。
   验证后删除该演示文件。

两例期间 `git status --porcelain -- src/` 的 md5 与操作前完全一致（`8316add7...`）。

### 11.4 已知边界

- `os` 模块的 `environ` / `walk` / `system` 与 `time` 的 `strftime` 仍保留 C++ 降级分支：
  它们不是签名问题，是「参数装配/返回类型随实参变」的问题（§6.4）。
- `driver_modules.cpp` 的自动登记只覆盖 `stdlib/sigs/*.pysig`；纯 `.py` 新模块（`codecs` 这类）仍然要登记 `kModuleTable`，
  两件事不要混。

### 11.5 第二轮：`.pysig` 换成 Python 语法（本机实测）

动机：自定义 lexer + 类型名映射表 = 第二套类型语法，和 `parse_annotation_impl` 并行维护会漂移。
换成 Python 语法后，声明文件用编译器自己的 `Lexer` + `Parser` 解析，注解走
`TypeChecker::parse_annotation_for_decl()`（转发到 `parse_annotation`，语义同一份）。

| 项 | 结果 |
|---|---|
| 加载器代码量 | `src/native_sigs.h` 670 行 → **642 行**：删掉自写 lexer（~190 行）+ 类型名映射表（~90 行），换成 AST 白名单 + 注解复用（新增 ~120 行校验/诊断） |
| 前置验证（3 段语法） | `def f(*args: unknown) -> str: pass` / `def g(secs: float = -1e300) -> tuple[int, ...]: pass` / `@native("pa_time_time")` + `def h() -> float: pass` —— **全部一次通过，parser 零改动** |
| 声明文件 | `stdlib/sigs/time.pysig`（8 func + 2 const）、`stdlib/sigs/os/path.pysig`（16 func，路径即模块名 `os.path`） |
| 12 基准 × 3 档 asm vs HEAD `61c2c57` | **36/36 `cmp` 逐字节相同** |
| 迁移模块自身 asm（time 8 函数 + 常量、os.path 16 函数，O0/O2） | **4/4 逐字节相同** |
| 全量套件 | **426 ran ok / 0 失败**（425 + 新增 524） |
| `tools/checkall.sh --full` | 全绿：suite 426/0、difftest 398/0、bench 36/36、check_size、lsp-smoke、simd、**perf warn=0 fail=0（指令数变化 ≤0.06%）** |
| 9 类错误路径 | 全部带 `文件:行号` 实测（语法错 / 未知语句 / body 非 pass / 未知类型 / 缺标注 / 非法模块名 / 符号不在 RtFn 表 / 重名 / 常量类型不符），另含 `@native` 参数非法、未知装饰器、`async def` |
| import 防护 | `import sigs` / `from sigs import demo` / `import sigs.demo as d` 三种形态都命中并给出清晰报错；普通 stdlib 模块不受影响 |
| 编译耗时 | 与 HEAD 同法测量：差异在噪声内（机制净成本 < 0.4ms，远低于 5ms） |

