# 前端缺口与 unknown 来源（总账）

> 本文把三轮评估合并成一张账：**628 文件基线**（0.485 测、0.498 复测）、**失败簇 top-10 与 top-3 判负**、
> **unknown 的实际来源**（0.499 实测）、以及它们对**性能与正确性**的代价。分轮记录见
> [frontend-fixes.md](frontend-fixes.md)（0.498）、[compatibility.md](compatibility.md)（基线段）、
> [numeric-types-design.md](numeric-types-design.md)（unknown 表示）。

## 0. 统一采集（0.507 起）

这个数字被三个代理各自量过，口径来回漂（628/626 个文件、默认口径 495/496/499 三个版本）。现在统一成一条命令：

```bash
bash tools/frontend_baseline.sh                 # 默认 /usr/lib/python3.14 + build/lead/pyadamas
bash tools/frontend_baseline.sh <stdlib 目录> <pyadamas 路径>
```

输出四个口径：syntax / default（当前默认，strict names）/ old（--no-imports --lenient-names）/ imports（opt-in，会误杀）。

**0.507 实测（626 个 .py）**：syntax **621/626**、default **494/626（78.9%）**、old **499/626**、imports **162/626**。
0.502 之前 --imports 会因循环 import 的 UAF 崩溃，那个 155 的下界已作废。

## 1. 基线数字（CPython 3.14.4 标准库，628 个 `.py`）

| 口径 | 通过 | 说明 |
|---|---|---|
| 语法（`check --syntax`） | 623/628（99.2%） | 5 个语法失败 |
| 旧范式（`--no-imports --lenient-names`） | 505/623（81.1%） | 与 0.171 可比 |
| **默认（0.486 起）** | **480/623（77.0%）** → **500/623（80.3%，0.498 后）** | strict names 默认开 |
| `--imports`（更严，opt-in） | 155/623（24.9%） | 会因签名表不全误杀，别用于 lint |
| 本机 lenient 上限 | 504/623 | 默认口径的理论上限 |

0.498 的类体方法别名消掉了 27 个文件的 `undefined name`（触发文件 45 → 18），其中 20 个整文件转绿。

## 2. 失败簇 top-10（旧口径 123 个失败文件，按首条错误归类）

| # | 簇 | 数量 | 代表 | 根因 |
|---|---|---|---|---|
| 1 | `cannot assign ... (annotated at line N)` | 14 | `fnmatch.py:163` | 生成器表达式被推成 `func(unknown)->None` |
| 2 | `X object has no method Y` | 11 | `concurrent/futures/process.py` | `X \| None` 无类型窄化 |
| 3 | `X takes no arguments (no __init__)` | 9 | `_colorize.py` | enum/`__new__`/运行期构造 |
| 4 | `undefined name X` | 9 | `http/client.py:337` | 条件导入/平台分支定义的名字 |
| 5 | `X object has no attribute Y` | 7 | `xmlrpc/client.py` | 动态属性 |
| 6 | `decorating dunder method ... not supported` | 4 | `numbers.py:71` | 装饰后的协议 dunder 不回填 |
| 7 | `X is not sliceable` | 4 | `email/_parseaddr.py` | 同簇 2 的窄化缺失 |
| 8 | `property X conflicts with a field` | 4 | `urllib/error.py` | 注解落成布局字段 |
| 9 | `unsupported operand type(s) for +` | 4 | `inspect.py` | unknown 参与算术 |
| 10 | `super() has no method X after class Y` | 3 | `socketserver.py` | 协作式多继承 |

**top-3 判负**（要动的层与代价）：
1. **类型窄化**（簇 2+7，≈15 文件）：需要在 `if`/`while`/`assert` 上做 refinement 并把环境沿分支传递、汇合点 join；
   当前 typecheck 没有 refinement 状态（只有作用域与 `cur_class_`）。→ **0.499 已立项**（`T \| None` 窄化）。
2. **生成器表达式类型**（簇 1，14 文件）：影响所有推导式，属类型系统一层。
3. **协作式多继承 `super()`**（簇 10，3 文件）：方法在运行期 MRO 的兄弟类里。

## 3. unknown 从哪来（0.499 实测，**0.500 更正口径**）

> **0.500 更正**：默认 `check` 单文件**不解析模块成员**（`module_lookup_` 为空时 `import` 只声明 unknown），
> 所以下表中 `json.dumps` / `sys.argv[0]` 那两行是**模式差异**，不是接口表缺失：在 `check --imports` 下它们本来就是 `str`。
> 用 `--imports` 重新审计 16 个原生模块 358 个成员：**missing 130 → 94、unknown 27 → 28**（剩余见 §8.5）。
> 同理 `except E as e` 也不是 unknown —— 它一直是 `Exception[E]`，之前看到「放行」是因为 `TyKind::Exception` 的成员访问本来就宽。


判据：对**已知类型**做不存在的成员访问会报错；**放行**说明该处类型是 `unknown`。

| 构造 | 结果 | 性质 |
|---|---|---|
| `json.loads(...)` / `json.dumps(...)` 的返回 | **unknown** | 原生/库接口表没建模（`dumps` 应该是 `str`） |
| `sys.argv[0]` | **unknown** | 同上（应该是 `str`） |
| `*args` 的元素 | **unknown** | 语法构造盲区 |
| `except ValueError as e` 的 `e` | **unknown** | 语法构造盲区 |
| 被装饰函数的返回（`@deco def g()->int`） | **unknown** | 装饰器丢失签名 |
| `def f(x: Sequence[int])` 的形参 | **unknown** | typing 构造降级 |
| `dict.get(k)`（无默认值） | 已知 | 返回 `T \| None` |
| `dict[str,int]` 下标 / 空 list 回填 / 生成器表达式 / lambda | 已知 | 推断正常工作 |

注解覆盖率（我们自己 stdlib）：`def` 共 1123 个，带返回注解 1014 个（**90%**）——所以 stdlib 不是主要来源，
**用户代码**通常不写注解，靠推断，不收敛就 unknown。

## 4. 为什么是「大批」：传染 + 无边界

- **传染**：unknown 参与任何运算/调用/下标后仍是 unknown；函数返回 unknown → 调用方全 unknown。
  少数源头即可污染整条调用链，所以数量级远大于「真的推不出来」的点。
- **无边界**：没有 `isinstance` 窄化、没有把 unknown 当回事（`unknown.nope()`、`unknown[0]` 全放行），
  污染扩散时**一声不响**——这是它比「报错」更危险的地方。

## 5. 代价

**性能**（0.497 页位图之后，pprint 的 perf 分布）：

| 项 | 占比 |
|---|---|
| `rc_index_has`（是不是堆对象的探测） | 11.5% |
| `pa_rc_dec_slow` | 5.6% |
| `rc_alloc_core` | 4.4% |
| `rc_index_del` | 3.7% |
| **合计内存管理簿记** | **≈25%**（页位图之前是 44%） |

每操作成本：`list[int]`（已知类型）约 **25 ns**/次；`list`（unknown 元素，装箱 + RC + 索引）约 **65–75 ns**/次。

**正确性**（都是「静默错」而不是「报错」）：
- `unknown` 下标按机器语义解码 → 段错误或垃圾值（0.498 已改运行期分派，但大整数仍段错误、bytes 无标签）；
- `Fraction + 0.5`（0.487）把 double 位模式当 BigInt 指针读，每次结果不同（已回退）；
- 多继承非首基类的方法体字段偏移错（路线图 3.9）；
- `T | None` 未判空即用能过 check、运行时崩（0.499 在修）。

## 6. 修法清单（按性价比，标状态）

| # | 修法 | 状态 | 误杀风险 |
|---|---|---|---|
| 1 | 补齐原生/库接口表 | **已做**：0.500 补 36 项（missing 130→94）；0.502 修 `--imports` UAF（pass 155→164）；0.511–0.517 再补 sys/re/operator/random/json 共 12 项；剩余 97 项见 §8.5（都要新运行期面） | 零 |
| 2 | 装饰器保留原签名 | **判负**（0.500：628 口径 500→496）；0.517 实测 `singledispatch`/`types` 在 stdlib 里不存在、`contextmanager` 运行期已可用 → 建模暂不适用 | — |
| 3 | `except as e` 给异常类型 | **已做**（0.500） | 零 |
| 4 | `typing.Sequence/Iterable` 映射 | **判负**（fe5）：函数体内 `list[T]` 会改变降级层容器快速路径，`sum_iter({6,7})` 输出 `0 0`（CPython 13 6） | — |
| 5 | `isinstance` 窄化 | **已做**（0.501） | 低 |
| 6 | 默认参数在 `def` 点求值（6 个文件） | 判负（要新机制） | — |
| 7 | 条件导入 / `globals().update()`（4 个文件） | 判负（动态语义） | — |
| 8 | `unknown` 下标 | **运行期分派已做**（0.498 `pa_any_index`；0.516 `pa_any_setindex/delindex`）；编译期禁掉仍判负（195 处） | — |
| 9 | `unknown` 大整数下标 / bytes 标签 | 待 P2 标签默认开 | — |

## 7. 相关文档

- 表示层与 `unknown` 的机器语义：[numeric-types-design.md](numeric-types-design.md)
- 628 文件基线的采集命令与逐轮数字：[compatibility.md](compatibility.md)（前端鲁棒性基线）
- 0.498 那一轮的完整记录与判负证据：[frontend-fixes.md](frontend-fixes.md)
- `unknown` 在运行时的类型分派：[stdlib-round2.md](stdlib-round2.md) §7
- 路线图条目：3.8（类体别名，已修）、3.9（多继承字段偏移）、3.10（保守 GC 段错误）
## 8. 本轮落地：补表 / 装饰器签名 / except 绑定（分支 fe2）

### 8.1 取证方法与一个口径修正

探针：对每个成员生成 `import M; x = M.member; x.nope()`，跑 `check --imports`。
命中 `'T' object has no method 'nope'` = 类型已知；`module 'M' has no attribute`
= 表里没有；无报错 = unknown。**必须在 `--imports` 模式下量**：默认 `check` 单文件
不加载模块接口（`stmt_import` 里 `module_lookup_ == nullptr` 时直接 declare unknown），
此时**所有**模块成员都是 unknown —— 0.499 的 §3 表格把这一点混进了「接口表缺失」，
例如 `json.dumps` / `sys.argv[0]` 在 `--imports` 下本来就是 `str`，本轮实测确认：

| 表达式 | 0.499 记录 | 本轮实测（`--imports`） |
|---|---|---|
| `json.dumps({...})` | unknown | **str**（已建模） |
| `json.loads(s, sample)` | unknown | 动态：按 sample 的类型（保留，见 §8.4） |
| `sys.argv[0]` | unknown | **str**（已建模） |
| `@deco def g()->int` 的 `g()` | unknown | unknown（本轮**判负**：保留签名在 628 语料上 500→496，见 §8.3） |
| `except ValueError as e` 的 `e` | unknown | **ValueError**（本来就有类型；「unknown」是**成员访问宽松**造成的观感，见 §8.5） |

审计范围：16 个原生模块 358 个成员（sys/os/os.path/time/json/math/random/re/
collections/pathlib/datetime/subprocess/functools/operator/itertools/asyncio）。
改前 **missing 130 / unknown 27**（`os.path` 的 24 个是探针写法的假阳性：
`import os.path` 后取 `os.path.join` 不算模块成员；正确写法见 §8.2 末尾）。

### 8.2 补表：33 个成员 + 13 个运行期函数

只补能确定类型、且**运行期真的有值/实现**的成员（补类型但不实现 = 静默 0/None，
禁止）：

- **常量（编译期字面量，`stdlib/sigs/*.pysig`）**：
  sys `platform`/`version`/`byteorder`/`maxunicode`/`hexversion`/`api_version`/
  `dont_write_bytecode`；os `name`/`curdir`/`pardir`/`altsep`/`pathsep`/`extsep`/
  `devnull`；re `A`/`ASCII`/`U`/`UNICODE`/`NOFLAG`；datetime `MINYEAR`/`MAXYEAR`。
  值按目标平台（Linux x86-64）与项目对标的 CPython 3.14.4 固定（`python3` 实测核对）。
  `sys.version_info` 是五元组，pysig 常量语法只收标量，走 `builtin_value` 里
  的 `MakeTuple`（(3, 14, 4, "final", 0)）。
- **运行期函数（新 RtFn + C 实现 + pysig）**：os `getuid`/`geteuid`/`getppid`/
  `urandom`/`strerror`/`replace`（复用 rename 符号）；time `time_ns`/`monotonic_ns`/
  `perf_counter_ns`/`process_time`；random `randbytes`（与 CPython 同一字消耗，
  同种子逐字节一致）；sys `intern`；re `purge`（no-op，语义等价）。
- **stdlib 源**：`itertools.batched`（eager list 版本，元素按 CPython 口径是 tuple），
  与其它 itertools 扩展函数同一模型。

改后：**missing 130 → 97，unknown 27 → 28**（`os.altsep` 是 None，探针对 None 接收者
天然放行 —— 不是 unknown）。`os.path` 用正确写法复核全部已知：
`from os import path as osp; x = osp.join; x.nope()` → `'func(...) -> str' object has
no method 'nope'`。

### 8.3 装饰器保留原签名 —— 判负

试过的两个版本（都在 `typecheck/func.cpp` 的装饰器链里）：

1. **宽版本**：装饰器是未知 callable、或返回未标注包装函数时，沿用被装饰函数自己的
   签名（参数 + 返回类型）；能静态看出具体返回类型时仍按它替换。
2. **窄版本**：只对未知 callable 保留。

两个版本在 **628 文件语料上都 500 → 496**，回归文件与根因：

| 文件 | 报错 | 根因 |
|---|---|---|
| `asyncio/tasks.py` | `'generator[None]' object is not awaitable`（`await __sleep0()`） | `@types.coroutine` 把生成器函数变成 awaitable |
| `importlib/resources/_common.py` | `'func(...) -> unknown' object has no method 'register'` | `@singledispatch` 的装饰结果带 `.register`，不是原函数 |
| `pkgutil.py` | 同上（`iter_importer_modules.register`） | 同上 |
| `test/support/import_helper.py` | `'generator' object has no method '__enter__'` | `@contextmanager` 把生成器函数变成上下文管理器 |

结论：**「装饰器不改变签名」这个前提在本项目的语料里不成立**。标准库/用户代码里
`contextmanager`、`types.coroutine`、`singledispatch`、`functools.cached_property`
一类装饰器**确实**改变签名或返回类型；保留原签名 = 把「全部放行（unknown）」变成
「按一个不成立的前提检查」，必然新增拒绝，触红线。

**要做什么才能安全落地**：给每个已知装饰器建模返回类型（`contextmanager` 的返回
是 `_GeneratorContextManager`，`singledispatch` 返回带 `register` 的包装对象，
`types.coroutine` 返回 awaitable…），即 §6 清单第 1 项（补表）在装饰器上的延伸；
未知装饰器只能继续 unknown。代价：每个装饰器一条规则 + 类型系统里要有对应的
class/可调用模型；收益是「被装饰函数仍有类型」，但不是零误杀，需要逐条语料验证。

现状（master 行为）保留：装饰后的符号是 unknown；`@lru_cache` 仍是恒等。


### 8.4 `except E as e`：本来就绑定了 E

复核：`except ValueError as e` 的 `e` 静态类型是 `Exception[ValueError]`（富异常类是
真实例），所以 `need_int(e)` 会报 `got 'ValueError'`（`tests/strict/
except_e_is_exception_type.py`）；富异常类的方法面**已经**是严格的
（`e.bogus()` 报 `'MyErr' object has no method 'bogus'`）；`e.args` / `str(e)` 可用
（内置异常的 args 是 `tuple[str]`）。0.499 观察到的「e 是 unknown」来自**成员访问
宽松**：`call_method.cpp` 对 `TyKind::Exception` 的方法调用一律放行成 unknown。
**不收紧**（负结论）：CPython 允许给异常实例挂任意属性（`e.user_id = 1` 是常见写法），
把内置异常值的成员表做成封闭集会拒绝合法代码 —— 与「不许新增拒绝」红线冲突。

本轮只补了 `except (A, B) as e` 的类型解析：所有分支解析成同一个类型时用那个类型
（含富异常类），否则退到 `Exception` 基类（多类型的方法面是交集，静态上不硬造 union）。


### 8.5 仍然缺的（没有运行期实现，补类型会变成静默 0/None，所以不补）

原表 97 项，**0.511–0.517 已补 12 项**（`sys.getrecursionlimit/setrecursionlimit`、`re.subn`、
`operator.index/length_hint/setitem/delitem`、`random.normalvariate/betavariate` 等六个分布、`json.dump/load`），
**剩 85 项**；其中 fd / 系统对象 / sys 常量三块**进行中**（0.518 一批）。

| 类 | 例子 | 要动什么 | 状态 |
|---|---|---|---|
| 文件描述符/系统调用 | os `open`/`read`/`write`/`lseek`/`dup`/`fdopen`/`waitpid`/`kill`/`times`/`getloadavg`/`scandir`/`popen`、`O_*` 常量 | 新运行期面（fd 抽象） | **进行中** |
| 结构性返回值 | os `stat`/`lstat`/`uname`；time `struct_time`/`strptime` | 新 struct/namedtuple 模型 | **进行中** |
| 解释器身份 | sys `prefix`/`path`/`implementation`/`flags`/`float_info`/`modules`（`getsizeof` 已判负、`getrecursionlimit` 已做） | 运行期需要「解释器」概念 | **进行中（常量类）** |
| 惰性对象 | itertools `count`/`cycle`；`chain.from_iterable` | 本项目 itertools 是 eager list 模型 | 未做 |
| 类/异常对象 | collections `namedtuple`/`ChainMap`/`UserDict`；pathlib `PurePath`；datetime `time`/`timezone`/`UTC`；re `error`；asyncio `Timeout` | 运行期类工厂 | 未做 |
| 子进程面 | subprocess `call`/`check_call`/`check_output`/`getoutput`/异常类 | 运行期子进程协议 | 未做 |
| 其它 | operator `methodcaller`；functools `total_ordering`/`singledispatch`；sys trace 钩子 | 各自的小实现，收益低（`methodcaller`/`total_ordering` 已判负） | 未做 |

`json.dumps` 是 `str`；`json.loads` 保持「按 sample 推断」（`json.loads(text, sample)` 的 sample 决定返回类型）。
`os.environ` 是只读快照 `dict[str,str]`，写入在降级层显式报错（既有行为）。

### 8.6 语料实测（改前 / 改后）

| 语料 | 口径 | 改前 | 改后 | 回归 |
|---|---|---|---|---|
| CPython 3.14.4 标准库 626 文件 | `check --syntax` | 621 | 621 | — |
| 同上 | `check`（默认） | 495/626 | **499/626** | 0 新拒绝（`tools/frontend_baseline.sh` 口径） |
| 本仓库 907 个 `.py`（不含 build/） | `check`（默认） | 780 | **789** | 0 |

改前/改后用**同一份文件清单**、同一台机器分别跑 master（ec4918b 独立构建）与
本分支的二进制，`comm` 逐文件比对：两边通过集合完全相同（没有新增拒绝，也没有
新通过——本轮的补表只影响 `--imports`/build 模式，默认 `check` 不解析模块成员）。

装饰器保留签名的两次尝试**都**会让标准库 500 → 496（4 个文件），证据见 §8.3。
### 8.7 0.506 复测：装饰器建模 / 协议标注 / 可变参数

**可变参数（已满足，无改动）**：`types` 显示未标注的是 `func(*unknown)`、标注的是 `func(*int)`，
函数体里 `*a` 声明为 `tuple[元素, ...]`（`len(a)`、`a[0]` 有边界），`**kw` 声明为
`dict[str, 元素]`。三条要求都已成立。

**协议标注（判负，已回退）**：试过「签名保持 unknown（任何可迭代实参都放行）+ 函数体参数按
`list[T]` 声明（`for x in seq` / `seq[i]` 拿到 T）」。类型检查层是成功的（`s: str = v` 会报
`annotated as 'str' but assigned 'int'`），但**降级层跟着变了**：`sum_iter({6, 7})` 与生成器
实参走 list 快速路径，实测输出 `0 0`（CPython `13 6`）——静默错值，比 unknown 更糟。
结论：函数体内参数类型不是纯类型检查层的量，它会改变 IR/降级。要做需要独立的「协议类型」
（typecheck 侧有元素类型、lowering 侧按 unknown 处理），或只给 `for`/下标提供元素类型而不改
容器表示。已在 0.506 回退，接口/语料均无改动。

**装饰器建模（未做）**：0.505 上这 4 个文件（`asyncio/tasks.py`、`importlib/resources/_common.py`、
`pkgutil.py`、`test/support/import_helper.py`）`check` **已经全部通过** —— 未知装饰器 → 符号
unknown，全程放宽。0.500 的判负结论仍成立（保留签名会误杀 `@contextmanager`/`@singledispatch`/
`@types.coroutine`）。要真正建模需要三个合成包装类：CM（`__enter__` 返回原生成器的 yield 类型、
`__exit__`）、singledispatch 包装（`register` + `__call__`）、`types.coroutine`（awaitable），
本轮没有落地。

### 8.8 方法引用（bound method）——check 过、build 报错

```python
xs: list[float] = [1.0]
push = xs.append        # check ok
push(2.0)               # build: error: attribute access is only supported as a method call
```

- 现状：把方法引用存进变量/容器在 `check` 阶段放行，`build` 阶段被拒——两个口径不一致；
- 为什么没在 check 拦截：实测拦截会让 628 口径从 **499 掉到 484**（15 个文件真的这么写），
  属于不可接受的新增拒绝；
- 正确修法：降级支持 bound method 值（`(recv, slot)` 二元组 + 调用点拆包），不是一轮能安全做完的；
- 规避写法：直接 `xs.append(2.0)`。

**B1-min 实验结论（分支 `.wt/b1min` `9856751`，未合并）**：用「AST 指针 + 调用点重放」实现了
「局部变量 + 直接调用」，`push = xs.append; push(2.0)` 可编译且对拍正确，但**接收者在调用点求值**，
于是

```python
push = xs.append
xs = ys
push(2.0)      # CPython 改 xs；重放改 ys —— 静默错值
```

属于不可接受的静默分歧，未合并。628 口径收益为零（499 → 499，那 15 个文件都超出「局部变量 + 直接调用」）。
正确做法：绑定点把接收者**求值一次并存进隐藏槽**，调用点用槽里的值；这样不新增堆对象种类、
只多一个槽，工作量中等（要给 `lower_method_call` 一个「接收者来自槽」的入口，不能靠 `type_of` 反查）。

### 8.9 跨模块调用被装饰方法（0.536 前后）

- **0.535 及以前**：段错误（接口丢了 `decorated` 标记，导入方按普通方法走 vtable 空槽）；
- **0.536 起**：check 阶段报 `decorated method 'add' of imported class 'Calc' is not available across modules`；
- 同模块内装饰方法一直正常；
- 未做：跨模块**支持**它（需要把类属性里的闭包值跨模块导出，属于接口格式的第二类扩展，暂不做）；
- 另：函数值不能挂属性（`wrapper.calls = get_calls`）：check 过、build 报 `unsupported assignment target`，
  函数值是纯编译期实体；用闭包 cell 或模块级 dict 替代。
