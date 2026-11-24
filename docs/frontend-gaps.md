# 前端缺口与 unknown 来源（总账）

> 本文把三轮评估合并成一张账：**628 文件基线**（0.485 测、0.498 复测）、**失败簇 top-10 与 top-3 判负**、
> **unknown 的实际来源**（0.499 实测）、以及它们对**性能与正确性**的代价。分轮记录见
> [frontend-fixes.md](frontend-fixes.md)（0.498）、[compatibility.md](compatibility.md)（基线段）、
> [numeric-types-design.md](numeric-types-design.md)（unknown 表示）。

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

## 3. unknown 从哪来（0.499 实测）

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
| 1 | **补齐原生/库接口表**（`json`/`sys`/`os` 等返回类型） | 待做 | 零（只是补表） |
| 2 | **装饰器保留原签名**（`@deco` 不改变被装饰函数签名） | 待做 | 零 |
| 3 | **`except as e` 给异常类型** | 待做 | 零 |
| 4 | **`typing.Sequence/Iterable` 映射成受检查的容器** | 待做 | 低 |
| 5 | **`isinstance` 窄化**（与 `T \| None` 窄化共用 refinement 层） | 待做（0.499 先做后者） | 低 |
| 6 | 默认参数在 `def` 点求值（6 个文件卡在这） | 判负（要新机制） | — |
| 7 | 条件导入 / `globals().update()`（4 个文件） | 判负（动态语义） | — |
| 8 | `unknown` 下标编译期禁掉 | 判负（195 处，含 5 个 stdlib 模块）→ 改运行期分派 | — |
| 9 | `unknown` 大整数下标 / bytes 标签 | 待 P2 标签默认开 | — |

## 7. 相关文档

- 表示层与 `unknown` 的机器语义：[numeric-types-design.md](numeric-types-design.md)
- 628 文件基线的采集命令与逐轮数字：[compatibility.md](compatibility.md)（前端鲁棒性基线）
- 0.498 那一轮的完整记录与判负证据：[frontend-fixes.md](frontend-fixes.md)
- `unknown` 在运行时的类型分派：[stdlib-round2.md](stdlib-round2.md) §7
- 路线图条目：3.8（类体别名，已修）、3.9（多继承字段偏移）、3.10（保守 GC 段错误）
