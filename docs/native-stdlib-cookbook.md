# 原生标准库 cookbook —— 用数据声明替代 C++ 硬编码

> 目标（P0）：**新增一个原生 API = 改一个 `stdlib/sigs/<模块>.pysig` + 重建，
> 零 C++ 编辑。** 本文是格式规格 + 操作手册 + 现状清单。

---

## 0. TL;DR

| 你想做的事 | 以前（硬编码） | 现在（声明式，模块已迁移） |
|---|---|---|
| 给 `time` 加一个新函数 | 改 `typecheck/builtin_sigs.cpp` + `lower/import.cpp`（2~3 处） | 在 `stdlib/sigs/time.pysig` 加一行 `def f(...) -> T: pass` |
| 接通一个已有运行期入口（`pa_*`） | 还要 `ir_rtfn*` / codegen 分组 | 同上，**一行**（符号名按 `pa_<模块>_<名字>` 约定推导） |
| 新模块 | 改 `driver/modules.cpp` 的 `kModuleTable` | 新建 `stdlib/sigs/<模块>.pysig`（模块表自动登记，见 §6.3） |
| 加一个新**运行期实现** | 必须 C（`runtime/rt_*.c`）+ RtFn 编号 + codegen 参数分组 | **仍然要**，这是运行期边界，不是签名问题（§6.4） |

已迁移：`time`（试点）、`os.path`、`math`（57 函数）、`sys`（maxsize + exit）、`os` 模块级（16 函数 + 6 常量）、`random`（模块级 + `Random` 类）、`re.Pattern`/`Match`、`datetime` 三类、`collections.deque`、`pathlib.Path`。不可迁移：`operator`（无 RtFn 入口）。未迁移：`subprocess`、`json`、`asyncio`、`python`/`c`/`dylib`（以及 collections 其余构造器）。

---

## 1. 背景：三处硬编码是怎么来的

原生模块（没有 `.pya` 源文件）在编译器里有三张手写表：

1. **签名表** `src/typecheck/builtin_sigs.cpp`：`builtin_module(name)` 返回 `ModuleInfo`，
   里面按模块名分支，把「属性名 → 类型」塞进 `globals`。类型检查（`typecheck/call_forms.cpp`）
   直接读这份表做实参检查；`from M import f` 的可调用性也靠它。
2. **成员 → RtFn 映射** `src/lower/import.cpp` 的 `Lowerer::native_fn(module, member)`：
   把 `time.sleep` 映射到枚举 `RtFn::TimeSleep`。
3. **降级装填** `src/lower/import_native.cpp` 的 `Lowerer::call_native`：
   正常路径是 `bind_args(c, info, nullptr)` + `call_runtime(fn, ret, args)`（通用，
   由 `FuncInfo` 决定每个实参怎么 coerce）；只有「可选参数/可变参/返回类型随实参变」
   的函数才需要在这里写手工分支（`time.strftime`、`subprocess.run`、`re.findall` ……）。

再加上 `src/ir/rtfn.h`（枚举编号）、`src/ir/rtfn_names.h`（编号 → `pa_*` 符号）、
`src/backend/call_rt.cpp`（按 RtFn 决定参数落在哪些寄存器类：i0/i1/i2/f1/str…）、
`src/driver/modules.cpp` 的 `kModuleTable`（模块登记）。

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

实现是 **header-only** 的 `src/cimport/native_sigs.h`：不需要在 `CMakeLists.txt` 里登记新 TU
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
| `x: int = -9223372036854775808` | INT64_MIN：字面量先按 u64 解析、再折叠一元负号，可取到 -2^63 |
| `x: float = float("nan")` | NaN / inf：用 `float("nan")`、`float("inf")`、`float("-inf")` 调用形式（Python 没有 NaN 字面量） |
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

### 4.3 返回类型特化：`@ret_specialized`

有些函数的返回类型**由实参决定**（`random.choice(seq)` 返回 `seq` 的元素类型、
`random.sample/choices` 返回 `list[元素]`）。硬编码里它们是
`FuncInfo{ret=unknown, ret_known=false}`，由 `typecheck/call_modules.cpp` 的
`random_call()` 在**调用点**按实参推导。

声明侧用装饰器表达同样语义（仍是 Python 语法，不加新关键字）：

```python
@native("pa_random_gchoice")
@ret_specialized
def choice(seq: object) -> object: pass
```

语义：加载器产出 `FuncInfo{ret=unknown, ret_known=false}`；返回类型注解只作占位
（写 `-> object`）。**代码生成与硬编码路径逐字节相同**（`tests/run/532` 覆盖，
`random.choice` 调用方栈帧与迁移前一致）。

注意：调用点的**推导逻辑**仍在 C++（`check_args` 对 `random.`/`itertools.`/
`operator.`/`collections.`/`json.` 前缀函数的分派）；`@ret_specialized` 只表达
「返回类型 unknown 且未定」这个**签名侧事实**。

装饰器约束（都有 `文件:行` 诊断）：

| 误用 | 报错 |
|---|---|
| `@ret_specialized("x")` | `@ret_specialized takes no arguments` |
| 重复写两次 | `duplicate @ret_specialized on 'choice'` |
| 用在常量/class 上 | `expected 'def' or 'class' after decorator` / 顶层语句白名单报错 |
| 未知装饰器 | `unknown decorator on 'z' (only @native("pa_symbol") and @ret_specialized are allowed …)` |

---

### 4.4 原生类声明：`class`（`Random` 起）

.pysig 里的 `class` 是**原生类**：body **只允许方法（`def`）**，不允许字段
（字段是 `.cpya` 的 struct 语义，两者上下文不同不会混）。`__init__` 是构造器，
其余是实例方法；方法首参固定为**未标注的 `self`**（接收者不进签名）。

```python
class Random:
    @native("pa_random_new")
    def __init__(self, a: int = -9223372036854775808) -> None: pass

    @native("pa_random_next")
    def random(self) -> float: pass

    @native("pa_random_choice")
    @ret_specialized
    def choice(self, seq: object) -> object: pass
```

规则与语义：

| 项 | 语义 |
|---|---|
| 类名 | 进模块命名空间 = 构造器函数（`random.Random(42)`），`from random import Random` 也能用 |
| `__init__` | 构造器；`@native("pa_random_new")` 提供构造 RtFn；返回类型注解写 `-> None`，**对象类型由编译器按类名映射**（`random.Random` → `TyKind::Random`） |
| 方法 | 需 `@native` 覆盖符号才与运行期对上（`Random` 的方法是 `pa_random_*`，不符合 `pa_<模块>_<方法>` 约定）；省略时按约定推导 |
| `@ret_specialized` | 方法也可用（`choice` 返回序列元素类型，`ret_known=false`），调用点按实参推导 |
| `FuncInfo::name` | 模块限定：构造器 `random.Random`、方法 `random.Random.<method>`（`check_args` 的 `random.` 前缀分派依赖它，裸名会让调用点类型退化 → 栈帧/GC mask 变化） |
| 继承 / 类装饰器 / 类关键字 | 一律拒绝（声明类没有基类语义） |

**类型身份是唯一留在 C++ 的一层**：`cimport/native_sigs.h` 的 `declared_class_type()`
把 `(module, class)` 映射到编译器的内建类型种类（目前只有 `random.Random` →
`TyKind::Random`）。声明负责方法表与构造签名，类型种类属于编译器；新增原生类时
在这里登记一行（下一批若做 `deque`/`Counter`/`date`/`Path` 等，建议先加一个通用的
native-object 类型种类把这层也声明化）。

**方法的降级（参数装配）仍在 C++**：与原生的模块级函数一致，`lower/method_misc.cpp`
的 `lower_method_random()` 按方法名装配实参并 `call_runtime`（§6.4）。本轮只把
**签名表**（typecheck 的硬编码方法表，~58 行）与构造器签名迁进声明；方法调用
的汇编与迁移前逐字节相同。

错误路径（都有 `文件:行`）：

| 误用 | 报错 |
|---|---|
| 类里放字段 | `native class 'Random' body may only contain methods (def); found annotated assignment (fields are .cpya struct semantics; ...)` |
| 重复方法 / 重复类 | `duplicate method 'seed' in native class 'Random'` / `duplicate declaration 'Random'` |
| 未知装饰器 | `unknown decorator on 'random' (only @native("pa_symbol") and @ret_specialized are allowed ...)` |
| 方法体非 pass | `function 'random' body must be pass (optionally preceded by a docstring); found return` |
| `@native` 指向不存在的符号 | `runtime symbol 'pa_no_such_symbol' is not in the RtFn table ...` |
| 类继承 / 类装饰器 | `native class 'Random' cannot inherit: declared native classes have no bases` |
| 调用未声明的方法 | `class 'Random' has no attribute 'random'`（改 `.pysig` 方法名即可复现，证明硬编码表真删了） |

---

## 5. 加载器与接线

### 5.1 `src/cimport/native_sigs.h` 对外接口

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

**A. 签名表（`src/typecheck/builtin_sigs.cpp`，`builtin_module()` 开头，`cpy_proxy` 分支之后）：**

```c++
// 声明式签名与硬编码**按名字合并**：.pysig 里的 func/const 覆盖同名硬编码，
// 硬编码独有的成员（如 sys.argv / sys.exc_info 这类只能由 builtin_value
// 生成运行期值的成员）保留。放在 builtin_module() 的末尾。
if (auto declared = pyadamas::nativesig::module_info(name)) {
    for (auto& [gname, gty] : declared->globals) info->globals[gname] = gty;
}
```

**B. 成员 → RtFn（`src/lower/import.cpp` 的 `Lowerer::native_fn()` 开头）：**

```c++
if (auto f = nativesig::func_rtfn(module, member)) return *f;
```

**C.（可选，迁移带常量的模块时用）`src/lower/import.cpp` 的 `Lowerer::builtin_value()` 开头：**

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

**没有第三处。** 类型检查（`typecheck/call_modules.cpp` / `typecheck/call_forms.cpp`）不需要改：
它对原生模块的普通函数调用走的是通用路径 `check_args(mod->globals[attr]->fn, c)`，
只要 `ModuleInfo` 里有正确的 `FuncInfo` 就够了。
降级也不需要改：`call_native` 的通用尾巴（`bind_args` + `call_runtime`）由 `FuncInfo` 驱动。

> 迁移一个模块**不需要**动：`typecheck/call_modules.cpp`（除非它对该模块有专门分派）、
> `ir/rtfn.h` / `ir/rtfn_names.h` / `backend/call_rt.cpp`（RtFn 已存在时）、
> `runtime/*.c`（运行期实现已存在时）。
> `driver/modules.cpp` 也只是「新模块」才需要（§6.3）。

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

**合并语义（P0 第二轮）**：声明模块与硬编码**按名字合并**，不是整模块替换——
声明里的 `func`/`const` 优先（同名覆盖），声明里没有的成员保留硬编码的签名与降级映射。
因此任何模块都不需要 C++ 侧的专用 merge 分支，迁移可以一个 API 一个 API 地做，
甚至永久保持「声明一部分 + 硬编码一部分」的混合状态。

**为什么不静默跳过**：静默跳过会表现成「模块没有这个属性」或「实参类型不对」，
和声明文件的内容毫无关系，排查成本极高。数据是随编译器分发的，坏了就该立刻停。

---

## 6. 操作手册

### 6.1 给「已迁移模块」新增一个 API（零 C++）

前提：需要的运行期入口 `pa_*` 已经存在（在 `src/ir/rtfn_names.h` 里能查到）。

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

1. **抄签名**：从 `typecheck/builtin_sigs.cpp` 的 `name == "<模块>"` 分支抄出每个函数的形参/返回类型；
   从 `lower/import.cpp` 的 `native_fn()` 抄出「成员 → `RtFn::X`」；
   `RtFn::X` 的 `pa_*` 符号从 `src/ir/rtfn_names.h` 查。
2. **写 `stdlib/sigs/<模块>.pysig`**（§3）。
3. **接线**（§5.2 A/B/C，若还没接）。
4. **删硬编码（可选）**：hook 是**按名字合并**，迁移可以分步做——只声明一部分成员时，其余成员
   继续由硬编码提供（`sys` 就是半迁移状态：`maxsize/exit` 在 .pysig，
   `argv/stdout/exc_info` 等 6 个无法声明的运行期成员留在硬编码）。整个模块都能用声明
   表达后，再删硬编码：`typecheck/builtin_sigs.cpp` 的分支整段删、`lower/import.cpp` 里该模块的
   `if (member == ...)` 行全删（注意 `lower/import_native.cpp` 的**手工装配分支要留着**，
   它不是签名，是参数装配）。
5. **重建 + 三档等价性**（§7）。
6. 把该模块从 `kModuleTable` 的 Native 行删掉（若启用 §6.3 的自动登记）。

### 6.3 新模块（零 C++，可选能力）

`driver/modules.cpp` 的 `find_module_entry()` 在查表失败后会再看
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
| 新的 `pa_*` 入口 + RtFn 编号 | 要加 `ir/rtfn.h`（枚举）+ `ir/rtfn_names.h`（符号名）——但这是**运行期边界**，一次性的 |
| codegen 参数分组 | `backend/call_rt.cpp::gen_rt_args` 按 RtFn 决定实参进 i0/f1/str* 等哪个类；新增 RtFn 时**必须**在这里加 case（漏了会生成错调用） |
| 参数装配不规则 | 可选参数需要「不传时填什么」以外的逻辑（`strftime` 的 9 元组、`subprocess.run` 的关键字、`re.findall` 的返回类型随捕获组数变）→ `call_native` 里的手工分支。注意：**签名侧**的「返回类型随实参特化」现在能声明了（`@ret_specialized`，§4.3），调用点推导逻辑仍在 C++ |
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
   `typecheck/typecheck.h` 的 public 段加了一个 3 行薄封装 `parse_annotation_for_decl()`
   （转发到 `parse_annotation`，语义零变化）。**不要**在加载器里复制一份类型解析——
   那正是这次换语法的起因（两套类型语法必然漂移）。
2. **`parse_annotation` 对不认识的名字返回 `unknown` 而不是报错**（用户代码里
   前向引用/自造类型要放行）。声明文件必须自己判：注解解析成 unknown 且源表达式不是
   显式 `unknown/Any/object` → 报错。否则拼错类型名不会有人发现。
3. **默认值要走 `literal_of_expr()`**（`typecheck/types.h` 的自由函数）：它已经处理了
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
| `time` | ✅ 已迁移（`stdlib/sigs/time.pysig`，Python 语法） | 8 个函数 + 2 个常量；`strftime` 的参数装配仍在 `lower/import_native.cpp`（签名已声明） |
| `os.path` | ✅ 已迁移（`stdlib/sigs/os/path.pysig`，Python 语法） | 16 个函数（路径即模块名）；常量 `sep/altsep/curdir/pardir` 属于 `os` 模块，不在本文件 |
| `math` | ✅ 已迁移（`stdlib/sigs/math.pysig`，Python 语法） | 57 个函数 + 5 个常量；`inf`/`nan` 用 `float("inf")`/`float("nan")` 调用形式声明，**不再有 C++ 特判**；asm 等价 |
| `sys` | ✅ 已迁移（`stdlib/sigs/sys.pysig`） | `maxsize` + `exit` 走声明；`argv`/`executable`/`stdin`/`stdout`/`stderr`/`exc_info` 是运行期调用或含 ClassObj 返回类型，保留 C++（在 `typecheck/builtin_sigs.cpp` 的 sys merge 分支补类型）；asm 等价 13/13 |
| `operator` | ⛔ **不可迁移** | 31 个函数全部走 `operator_call()` 类型推导 + `lower_operator()` 专门降级，无 `pa_operator_*` RtFn 入口；pysig 的符号反查会硬失败。判据：**只有存在 `pa_<模块>_<名字>` RtFn 的函数才能声明**。 |
| `os` | ✅ 已迁移模块级函数（`stdlib/sigs/os.pysig`） | 16 函数 + 6 标量常量；`unlink` 用 `@native("pa_os_remove")`；保留 C++：`environ`（运行期 dict）、`walk`（返回 list[tuple]）、`os.path.*`（独立模块已迁）。asm 等价 13/13 |
| `random` | ✅ 已迁移**全部模块级函数 + `Random` 类**（`stdlib/sigs/random.pysig`） | 17 函数 + `class Random`（构造器 + 6 个实例方法，`@native` 覆盖 `pa_random_*`；`choice` 用 `@ret_specialized`）。保留 C++：方法的参数装配（`lower/method_misc.cpp`，§6.4）、类型身份映射 `declared_class_type`。asm 与迁移前逐字节相同 |
| `re` | ✅ 已迁移 `Pattern`/`Match` 方法表（`stdlib/sigs/re.pysig`） | 2 个原生类共 15 个方法（`@native` 覆盖 `pa_re_*`）；模块级函数/flag 常量保留硬编码（`-> Regex`/`Optional[Match]` 等类型 pysig 无法表达，迁了会破坏 `re.compile` 的返回类型）；`lower_method_regex/match` 参数装配不动；asm 38/38 逐字节相同 |
| `datetime` | ✅ 已迁移 3 个原生类（`stdlib/sigs/datetime.pysig`） | `date`/`datetime`/`timedelta` 构造器（默认值表达重载）+ 6 实例方法；模块函数 today/now 保留 C++（返回 TyKind::DateTime 无法声明）；属性读/运算符/参数装配不动；asm 38/38 逐字节相同 |
| `collections` | ✅ 已迁移 deque 方法表（`stdlib/sigs/collections.pysig`） | 10 个实例方法声明化；构造器保留 C++（默认值 `[]` 非 pysig 字面量、`list[unknown]` 静态类型影响 asm）；pop/popleft/copy 的接收者元素类型返回保留 typecheck 口径；asm 38/38 逐字节相同 |
| `json` | ⬜ 未迁移 | 硬编码仍在；加载器已就绪，可按 §6.2 逐个搬 |
| `pathlib` | ✅ 已迁移 `Path` 方法表（`stdlib/sigs/pathlib.pysig`） | 构造器 + 18 方法；`/` 运算符、构造器降级、`mkdir` 关键字白名单、参数装配保留 C++；`unlink`/`rmdir` 无 `pa_path_*` 入口保留 method_call；asm 36/36 逐字节相同 |
| `operator` `asyncio` | ⬜ 未迁移（优先级低） | `operator` 无 RtFn 入口；`asyncio` 返回类型/装配多在 C++ |
| `subprocess` | ⬜ 未迁移（优先级低） | 参数装配/返回类型随实参变的分支多 |
| `python`（CPython 桥）、`c` / `dylib` | ⛔ 不适合声明式 | 名字集合是开集（`from c import <任意符号>`），签名在调用点推断 |

### 4.3 补充：@ret_specialized 与「接收者元素类型」

`@ret_specialized`（无参数）表示「返回类型按第一实参特化」（random.choice）。
deque 的 pop/popleft/copy 需要的是「接收者元素类型」（`recv->sub[0]`），
现有形态表达不了。**本轮取舍**：这三个方法的返回类型口径保留在
`typecheck/call_method.cpp` 按方法名硬编码（与 datetime.replace 同模式），
pysig 里只声明方法存在性 + argc/arg 类型（返回写 `object` 占位）。
这样不需要给 `@ret_specialized` 加新形态（那要改 cimport/pydecl.h 的装饰器解析，超出本轮边界），
asm 也逐字节相同。若要全声明化，可加 `@ret_specialized("element")`
字符串参数形态（decorator_of 已支持字符串实参），在 SigFunc 加 kind 字段并
在 typecheck 里按 kind 分派——但这是机制扩展，不是签名迁移。

### 后续迁移评估：deque / Path（datetime 批次附带盘点）

**collections.deque**（中等，1~2 天）：方法表共 10 个（append/appendleft/extend/extendleft/pop/popleft/rotate/clear/reverse/remove/count/index/insert/copy），
全部走通用 native_fn（RtFn::Deque*），与 Random 同构。额外难点：
- `pop()`/`popleft()` 返回 `elem()` = `recv->sub[0]`（元素类型随接收者变）→ 需要 `@ret_specialized` 扩展支持「接收者元素类型」而不仅是「第一实参类型」；
- `copy()` 返回 `mk_deque(elem())` 同理；
- 构造器 `deque(iterable, maxlen=0)` 的 maxlen 语义在降级阶段检查（保留 C++）。
**改哪层**：cimport/native_sigs.h 的 declared_class_type 加一行（deque → TyKind::Deque）+ typecheck/call_method.cpp 声明查表 + stdlib/sigs/collections.pysig 新建。
**判断依据**：类型身份映射仍要 C++（同 Random/re/datetime），但方法表本身可声明。

**pathlib.Path**（高，3~5 天）：方法表共 ~17 个（mkdir/exists/is_file/is_dir/read_text/write_text/iterdir/glob/unlink/rmdir/touch/with_suffix/read_bytes/write_bytes/is_absolute/joinpath/relative_to/resolve/absolute），
其中 5 个返回 `new_ty(TyKind::Path)`（iterdir/glob 返回 list[Path]、with_suffix/joinpath/relative_to/resolve/absolute 返回 Path）。
额外难点：
- `mkdir` 只收关键字（mode/parents/exist_ok）→ pysig 的 `def mkdir(self, *, mode: int = 0, parents: bool = False, exist_ok: bool = False)` 需验证关键字-only 形参在原生类方法上的装配（`lower/method_native.cpp:9` 的 kw_ok 白名单按方法名硬编码，不能直接改）；
- `Path` 的 `/` 运算符（`p / "sub"`）在 typecheck/expr.cpp 里特判，不属方法表，保留 C++；
- `declared_class_type` 加一行（pathlib.Path → TyKind::Path），并在 module_info 里把 Path 构造器注册（当前 `func("Path", {t_str()}, p)` 在 typecheck/builtin_sigs.cpp）。
**改哪层**：与 deque 相同，外加运算符与 kw_ok 白名单保持 C++ 不动。

**共同结论**：两个类都能用现有原生类机制迁移（与 Random/re/datetime 同构），
剩余成本集中在「返回类型随接收者变」（deque.pop/copy）和「关键字-only + 运算符」（Path），
都不是新机制，只是声明格式需要覆盖更多形态。

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
| `src/cimport/native_sigs.h` | **新增 670 行** | 加载器（格式/词法/语法/符号反查/缓存/失败路径），一次性成本 |
| `src/typecheck/builtin_sigs.cpp` | +18 / −26 | 7 行 hook + include；删 `time` 段 40 行、`os.path` 段 21 行 |
| `src/lower/import.cpp` | +25 / −22 | 2 行 hook + 常量 hook；删 `time` 段 12 行、`os.path` 段 20 行 |
| `src/driver/modules.cpp` | +63 / −1 | `.pysig` 自动登记（新模块零 C++） |
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
- `driver/modules.cpp` 的自动登记只覆盖 `stdlib/sigs/*.pysig`；纯 `.py` 新模块（`codecs` 这类）仍然要登记 `kModuleTable`，
  两件事不要混。

### 11.5 第二轮：`.pysig` 换成 Python 语法（本机实测）

动机：自定义 lexer + 类型名映射表 = 第二套类型语法，和 `parse_annotation_impl` 并行维护会漂移。
换成 Python 语法后，声明文件用编译器自己的 `Lexer` + `Parser` 解析，注解走
`TypeChecker::parse_annotation_for_decl()`（转发到 `parse_annotation`，语义同一份）。

| 项 | 结果 |
|---|---|
| 加载器代码量 | `src/cimport/native_sigs.h` 670 行 → **642 行**：删掉自写 lexer（~190 行）+ 类型名映射表（~90 行），换成 AST 白名单 + 注解复用（新增 ~120 行校验/诊断） |
| 前置验证（3 段语法） | `def f(*args: unknown) -> str: pass` / `def g(secs: float = -1e300) -> tuple[int, ...]: pass` / `@native("pa_time_time")` + `def h() -> float: pass` —— **全部一次通过，parser 零改动** |
| 声明文件 | `stdlib/sigs/time.pysig`（8 func + 2 const）、`stdlib/sigs/os/path.pysig`（16 func，路径即模块名 `os.path`） |
| 12 基准 × 3 档 asm vs HEAD `61c2c57` | **36/36 `cmp` 逐字节相同** |
| 迁移模块自身 asm（time 8 函数 + 常量、os.path 16 函数，O0/O2） | **4/4 逐字节相同** |
| 全量套件 | **426 ran ok / 0 失败**（425 + 新增 524） |
| `tools/checkall.sh --full` | 全绿：suite 426/0、difftest 398/0、bench 36/36、check_size、lsp-smoke、simd、**perf warn=0 fail=0（指令数变化 ≤0.06%）** |
| 9 类错误路径 | 全部带 `文件:行号` 实测（语法错 / 未知语句 / body 非 pass / 未知类型 / 缺标注 / 非法模块名 / 符号不在 RtFn 表 / 重名 / 常量类型不符），另含 `@native` 参数非法、未知装饰器、`async def` |
| import 防护 | `import sigs` / `from sigs import demo` / `import sigs.demo as d` 三种形态都命中并给出清晰报错；普通 stdlib 模块不受影响 |
| 编译耗时 | 与 HEAD 同法测量：差异在噪声内（机制净成本 < 0.4ms，远低于 5ms） |

