# 路线图

按「收益 / 风险」排序，不是按「看起来酷」排序。每项都写了验收标准——没有验收标准的需求不算需求。

## 已完成

| 阶段 | 内容 |
|---|---|
| 词法 / 语法 | 缩进敏感的词法器、递归下降解析器、PEP 701 f-string、全语法 dump |
| 类型检查 | 标注 + 推断 + `unknown` 放行、Optional 收窄、分支合并、内置函数/方法签名 |
| 增量构建 | 拆 TU + `-MMD`，改一个文件不再全量重编 |
| IR | 基本块 + 三地址码 + vreg，类型挂在指令上 |
| 后端 | IR → x86-64 汇编，System V 调用约定，栈帧与槽位复用 |
| 寄存器分配 | 热变量提升到 callee-saved；块内别名、结果融合、常量延迟、比较连分支 |
| 对象模型 | `class` / 方法 / 字段 / 单继承 / vtable 虚调用；属性访问是常量偏移 |
| 内存管理 | 引用计数：隐藏头、临时值栈、字段/列表析构、无 RC 代码的脚手架消除 |
| 闭包 | cell 捕获、`nonlocal`、多层嵌套、自递归、方法捕获 self；函数值是一等闭包对象 |
| 匿名函数 | `lambda` 脱糖成合成嵌套 `def`，复用闭包/cell；函数值默认参数可用 |
| 循环收集 | 保守标记-清扫：堆注册表、全局/临时栈/真实栈/寄存器根、2MB 阈值、退出清空 |
| 字典 / 集合 | 开放寻址哈希表 + 插入顺序；字面量、下标读写、`len`、`in`、`for`；RC/GC |
| 容器方法与切片 | dict get/keys/values/items/update、set add/update；str/list/tuple 读切片（缺省/负数/步长） |
| 构造补齐 | 删除类方法、切片赋值、`*args`/`**kwargs`、推导式、`with`、函数装饰器 |
| 异常 | setjmp/longjmp handler 栈；try/except/else/finally、raise、with 异常路径；运行时错误可捕获 |
| 运行时 | 字符串、列表、字典、集合、对象分配、输出、panic；浮点 repr 与 CPython 一致 |
| 验证 | 四类用例（正例 / 反例 / 运行 / 未建模） + 差分测试 + 标准库基线 |

## 0.6：对象模型 + 前端去误杀

类在编译期定死布局：对象 = `[vtable 指针][字段 0][字段 1]…`，`p.x` 是一条 `movq 8(%rax), %rax`。方法进 vtable，槽号也是编译期常量；单继承下子类沿用父类的槽位前缀，所以「父类类型装子类实例」能调到子类实现。

实测 2000 万次方法调用 0.09s（CPython 3.88s，43 倍）。

踩到的坑：`ClassInfo` 的字段类型指回 `Type`、`Type` 又用 `shared_ptr` 指回 `ClassInfo`，形成环，ASan 报 2 万字节泄漏。把 `Type::cls` 和 `ClassInfo::base` 都改成裸指针（所有权统一在 `TypeChecker::all_classes_`）之后归零。

第二轮拿 CPython 标准库跑前端基线，才发现类检查把真实代码判死了一大片。按「宁可漏报，不可误杀」放宽：`class A(object)` 认成根类、外部基类标记 `open`、`self.x = ...` 就地补隐式字段、类体语句与多继承降级成「解析了但没建模」（由 `build` 拒绝）、同名方法（`@property` 的 getter/setter）按后者覆盖。详见 [CHANGELOG 0.6.1](../CHANGELOG.md) 和 [compatibility.md](compatibility.md)。

**教训**：每加一层静态检查，都要立刻跑一遍标准库基线。只跑手写用例会以为「检查很准」，实际是悄悄把上万个真实文件判死了。

## 0.7：引用计数

0.7 把「只分配不释放」换成了引用计数。对象前面加 16 字节隐藏头
（rc + dtor），负载指针和所有字段偏移保持不变；字面量 rc = `INT64_MIN`
永生。表达式临时值进运行时临时值栈，语句 / 循环 / 函数边界按 mark 清理，
所以不需要活跃性分析，`continue` / `break` / 提前返回也不会漏。

验收结果：在 16MB 地址空间限制下，100 万次字符串拼接、100 万次对象
分配、30 万次带字符串字段的对象分配和 30 万次列表构造都正常完成，
峰值 RSS 约 8MB；`ctest --preset dev`、600 个差分程序、ASan/UBSan 全部通过。
不含临时堆值的语句 / 循环 / 函数会在降级期删掉 RC 脚手架，整数代码
性能与 0.6.1 持平；方法调用因为 self 参数的 inc/dec 慢约 30%。

**遗留问题**（0.9 已解决）：循环引用没有收集器；`unknown` 类型和
`list[unknown]` / 混合元组的元素不计数。现在由保守标记-清扫兜底，代价是
回收时机不精确。

## 0.8：闭包

0.8 支持内层函数捕获外层局部变量。被捕获变量在函数入口变成单元素
`list[T]`（cell），闭包对象负载是 `[code, n_caps, cell…]`；无捕获函数用
静态闭包，有捕获才运行时分配。`nonlocal` 解析到最近外层函数作用域。

验收：计数器工厂 `make_counter()` 能返回多个互不干扰的 `inc`；跨两层
嵌套捕获、自递归 `make_fact()`、方法里的闭包捕获 `self`、函数值存列表
/ 传参都通过。20 万次工厂创建在 16MB 地址空间下峰值 RSS 约 8MB。

**踩到的坑**：直接自递归的嵌套函数如果把自己当普通捕获变量，会形成
闭包↔cell 环，20 万次工厂创建直接 OOM。改成自引用走当前闭包（有 env 用
env，否则静态闭包）后环消失。相互递归的嵌套闭包仍会成环，留给循环收集器。

## 0.9：lambda + 循环收集

0.9 补上 `lambda` 和循环收集器：

- `lambda` 在解析后脱糖成插在语句前的合成嵌套 `def`，捕获/默认参数/RC
  全部复用普通函数；默认表达式挂到 `FuncInfo::Param`，通过函数值调用也
  能内联填充默认值。
- RC 头扩成 64 字节并挂进堆注册表；分配量超过 2MB 触发保守标记-清扫。
  根来自全局表、临时值栈、真实栈扫描和 `setjmp` 捕获的 callee-saved
  寄存器；未知类型/环都能回收，误报只延迟、不提前释放。
- 验收：16MB 限制下 20 万次对象环、20 万次相互递归闭包、20 万次
  unknown 容器泄漏、30 万次闭包垃圾全部通过，峰值 RSS 约 8MB；同时
  验证局部/全局存活列表和闭包跨 GC 不被误收。

**代价**：GC 是 stop-the-world 全堆扫描，且保守根可能延迟回收；没有
`__del__`/弱引用/分代。

## 0.10：字典 / 集合 + in

0.10 把类型层已有的 `dict`/`set` 接到运行时：开放寻址哈希表 + `order`
数组保留插入顺序，set 复用 dict（值恒 0）。支持字面量、`d[k]` 读写、
`len`、真值、`for k in d`、`k in d`，以及 list/tuple/str 的 `in`。

key/value 按静态类型接入 RC：str key 按字节哈希、逐字节判等；int/bool
按机器字，float 按 IEEE 值；unknown 交给 GC。10 万次字典/集合创建和
字典自环在 16MB 下峰值 RSS 约 8–9MB，ASan/LSan 干净。

**已知边界**：空字典的 unknown key 按机器字比较；混合 value 类型退化成
unknown；`==` 仍是指针比较。

## 0.11：容器方法 + 切片

0.11 补上 dict 的 `get`/`keys`/`values`/`items`/`update`、set 的
`add`/`update`，以及 str/list/tuple 的读切片。切片边界在运行时按
Python `slice.indices` 归一化，缺省边界用 `INT64_MIN` 哨兵；结果列表
按源 `elem_kind` 接管元素引用，str 切片按字节复制。

验收：14_dict/15_slice 运行用例通过；10 万次切片、10 万次
keys/values/items、5 万次 update 在 16MB 下峰值 RSS 约 8MB，
ASan/LSan 干净。

## 0.12：补齐构造（异常 / 生成器 / import 除外）

这一轮把路线图里除 `try/except`、生成器 / `async`、`import` 之外的构造
一次补齐：

- **dict/set 删除**：`pop` / `popitem` / `remove` / `discard` / `clear` /
  `set.pop`。开放寻址表没有墓碑，删除后按 `order` 剩余项整表重建，
  单次 O(n)；`popitem` 固定 LIFO。
- **切片赋值**：list 的替换 / 插入 / 删除和扩展切片；右侧可以是 str，
  按字节拆成字符列表；tuple 目标和 `del l[a:b]` 仍拒绝。
- **`*args` / `**kwargs`**：形参就是末尾的 list / dict 指针，调用点打包；
  定长 tuple 的 `*` 静态展开；`inner(*args, **kw)` 转发可用。
- **推导式**：list / set / dict 推导式和生成器表达式（立即求值），多
  层 `for` + `if`，独立作用域；`range()` 变成可求值的一等列表。
- **`with`**：类实例的 `__enter__` / `__exit__`；正常结束、`return`、
  `break`、`continue` 退出都补调 `__exit__`；异常路径留给异常轮。
- **函数装饰器**：按从下到上应用 `f = d1(d2(f))`，复用普通闭包调用；
  类装饰器、方法装饰器仍被拒绝。

验收：16–21 运行用例通过；10 万次删除 / 推导式 / 切片赋值 / `with` /
装饰器 / 变长实参调用在 16MB 地址空间下峰值 RSS 约 8MB；ASan/UBSan/LSan
零报告；600 个差分程序零差异。

**已知边界**：通过 unknown 函数值的 `*` / `**` 展开无法绑定；`**mapping`
与命名形参重名不报错；生成器表达式不惰性；方法/类装饰器未支持。
（`with` 的异常路径在 0.13 补齐。）

## 0.13：异常

按 0.12 末尾定下的模型实现：**setjmp/longjmp + 运行时 handler 栈**，
异常对象是 `{类型名, 消息}`。

- `try` / `with` / `finally` 压入 `{jmp_buf, prev, temp_mark}`；`raise`
  设置全局异常后 longjmp 到最内层 landing pad。
- landing pad 清理临时值栈，再分派 `except` 匹配、`with.__exit__`、
  `finally`；不匹配继续向外抛。
- 含 handler 的函数关闭热变量寄存器提升；异常跳过的普通函数帧不执行
  `release_slots`，局部引用交给保守 GC。
- `except` 支持多个 handler、tuple 类型、bare except、`as e`（消息
  字符串）和 `else`；`raise` 支持内置异常和 bare raise。
- `with` 异常路径调用 `__exit__(type, message, None)`，返回真值抑制
  异常；`__enter__` 抛异常时外层 `__exit__` 仍执行。
- 运行时错误 `IndexError` / `KeyError` / `ZeroDivisionError` /
  `OverflowError` / `ValueError` / `AssertionError` / `AttributeError`
  改为可捕获 raise；未捕获异常打印类型和消息后 `exit(1)`。

验收：`22_exception` 通过；10 万次跨帧异常 + 5 万次 handler 内重抛 +
5 万次 with 异常 + 5 万次 finally 异常在 16MB 下峰值 RSS 约 7.8MB；
ASan/UBSan/LSan 零报告；600 个差分程序零差异。

**已知边界**：只支持内置异常名，`as e` 是消息字符串，没有 `args` /
traceback / `raise ... from`；`finally` 体内跳出 finally 的
`return` / `break` / `continue` 被后端拒绝；异常路径的局部变量回收
依赖 GC 时机。

## 0.14：import（模块、类、内置库已实现）

按联调结论：**分模块编译 + 链接 + 二进制接口 + 增量缓存**。

**已完成（0.14a）**
- 模块图/搜索路径（入口目录 + `-I`）、循环导入检测、单层模块
  `import` / `import as` / `from ... import ... as`。
- 二进制 `.py23i`：全局/函数签名（含默认字面量）、FNV-1a 校验；
  文本 manifest 记录源码哈希和依赖接口哈希。
- 每模块独立 `.o`，入口 + 依赖 + `rt.o` 链接；入口也走缓存。
- 模块符号 `py23$m$g$x` / `ready` / `init` / `roots`；惰性初始化，
  模块 init 注册 GC 根；`__name__` / `__file__`。
- `import m` 编译期别名，`m.x` 直接读写外部全局、`m.f()` 调用外部闭包；
  `from m import x` 快照绑定。
- 测试 `tests/modules/`：basic / builtins / lazy / chain / values / gcroot / classes，`ctest --preset dev`
  验证输出和二次构建缓存命中。

**已完成（0.14b）**
- `.py23i` class 记录：扁平字段（根→派生）、方法名/槽号/调用签名、
  `has_init`、构造签名和 `ctor_symbol`。
- 定义模块为每个类生成隐藏构造包装 `py23$m$n$C`（ObjNew + 字段初值 +
  `__init__`），模块 init 把它绑定到隐藏全局。
- 导入方：`import m; m.C(...)` / `from m import C` 构造实例，字段读写和
  方法调用走导出的偏移/槽位；`C` 本身仍不能当值。
- 跨模块继承不支持；类类型出现在跨模块函数签名里降级成 unknown。
- 用例 `tests/modules/classes` 覆盖构造、默认字段、关键字参数、继承拍平
  后的字段/方法。

**已完成（0.14c）**
- 内置 `math` / `sys`（已完成）：`math` 常量与函数、`sys.argv`（`main(argc, argv)`）、
  `sys.exit` / `maxsize`；原生函数 `from math import sqrt` 直接调用。

**0.14 已知边界**：跨模块继承被拒绝；
类类型在跨模块签名里是 unknown；模块导出类的 `__init__` 带
`*args`/`**kwargs` 会被拒；模块 init 失败不会重试；编译器 ABI 变化或
缓存格式升级时用 `--rebuild` / 清理 `modcache`。

## 0.15：包、相对导入与 import *

- 包目录带 `__init__.py`；点号模块路径逐级解析，`import pkg.mod as m`、
  `from pkg.mod import x` 可用；点号 import 不带 `as` 要求改写。
- 相对导入 `from . import mod`、`from .mod import x`、`from ..pkg import y`
  在构建图阶段按当前包改写成完整模块名。
- `from pkg import mod` 中 `mod` 是子模块时绑定模块别名；普通名字仍是
  快照绑定。
- `from m import *` 静态展开非下划线全局/类/原生符号；`__all__` 暂不解析。
- 用例 `tests/modules/packages` 覆盖包、相对导入、子模块别名和星号导入。
- 已知边界：包 `__init__` 不会因为导入子模块自动执行；点号 import 必须
  带 `as`；`__all__`、命名空间包、跨模块继承未支持。

## 0.16：生成器（已完成）

**模型：状态机 + 堆帧，不用独立栈。**

- 含 `yield` 的函数拆成「创建包装 + resume」：局部槽位放在堆帧里，
  `yield` 只写 state/out 并返回；入口按 state 分发到续跑块。
- 类型 `Generator[T]`：yield 值取公共上界，`for` / `next` / `send` /
  `close` / `__next__` 可用；返回生成器的普通函数不会被误判。
- 跨 `yield` 清理：yield 时 `py23_handler_suspend` 摘下活跃 handler
  记录，恢复时重挂并重新 setjmp；`close()`、引用计数归零和 finalize
  按挂起点快照执行 `finally` / `with.__exit__`。
- 帧槽参与 RC/GC；异常逃出生成器时标 aborted，避免清理重复执行。
- 用例 `tests/run/23_generator.py`。

已知限制：`yield` 只能作为语句或赋值右值；不支持 `yield from`、
`yield` 位于 `finally`/`except` 体内、生成器表达式惰性求值；
`send` 的值和跨模块生成器类型都是 unknown。

**0.17 async 复用同一套状态机（已完成）**：

- `async def` 产出的帧和生成器帧同构（kind 标记区分），`await` 是
  awaiting 状态；协程的返回值写进 out，交给等待者。
- 事件循环：就绪队列 + 毫秒定时器 + `loop_pending`（运行中的 gather/Task），
  三者都是 GC 根；`asyncio.run` / `sleep` / `gather` / `create_task`
  已可用；Task 有 `done()` / `cancel()`，取消会注入 `CancelledError`。
- 异常跨 `await` 通过帧内注入传播：恢复时重挂 handler 后 rethrow，
  `try/except` 正常工作；根协程异常抛回同步调用点。
- 用例 `tests/run/24_async.py`；差分测试新增 `async` 生成器。

已知限制：`await` 只能作为语句、赋值右值或 `return` 值；`gather` 实参
个数写死，结果元素类型是 unknown；Task 单等待者、取消不级联。

## 0.19：异步语言面（已完成）

- 异步生成器：`async def` + `yield` → `asyncgen[T]`，可混用 `await`；
  `async for` 通过 `__anext__` 驱动对象消费，结束抛
  `StopAsyncIteration` 并在循环边界捕获。
- `async with`：类的 `async def __aenter__/__aexit__`，进入/退出、
  异常展开、`return`/`break`/`continue` 都会 await 退出；异常上下文
  先落槽再清 pending，避免跨挂起污染。
- 驱动对象改为就绪队列调度，不再递归调用 resume 栈；嵌套异步生成器
  （async gen 里 async for）可用。
- 用例 `tests/run/26_async_lang.py`。

已知限制：异步生成器没有 `asend`/`athrow`/`aclose`；生成器对象停在
`async with` 里被直接回收时不 await 退出；异步推导式、自定义
`__aiter__` 迭代器、Future/取消级联/fd 级 I/O 未支持。

## 0.20：运行时补齐（已完成）

- Future：`asyncio.Future()`，多等待者、`set_result`/`cancel`/`done`/
  `cancelled`/`result`/`exception`；Task 同步补齐状态查询。
- 取消级联：Task 取消沿 await 链向下传播（协程链/task/gather/future/
  sleep/fd），各层 `finally` 执行；修复协程挂起时 state 被清零的问题。
- fd 轮询：`wait_readable`/`wait_writable` + 事件循环 `poll`（超时取最近
  定时器），无效 fd 以 `OSError` 结束；fd 等待对象登记为 GC 根。
- `create_task` 接受 Future/sleep/fd 等等待对象；类型标注支持
  `Future[T]`/`Task[T]`/`Coroutine[T]`/`Generator[T]`/`AsyncGenerator[T]`。
- GC 清扫和退出时回收挂在帧/Task/Future 上的 malloc 异常。
- 用例 `tests/run/27_runtime.py`。

已知限制：没有 `Future.set_exception`/`add_done_callback`；`exception()`
返回消息字符串；`wait_readable` 不是 CPython 标准 API；没有 fd 读写、
子进程和线程安全事件循环。

## 0.21：yield from 与异步推导式（已完成）

- `yield from` 支持生成器与 list/tuple/str/dict/set 委托；生成器委托
  转发 `send`，子生成器异常向外传播，外层 `close()` 释放子生成器并
  执行其 `finally`；表达式值固定为 None。
- 异步推导式：list/set/dict 推导式里的 `async for` 子句，可混用同步/
  异步子句与 `if`；累加器和循环状态落帧槽，每个异步子句用一层
  handler 捕获 `StopAsyncIteration`。
- 修复生成器元素为堆类型时 `for`/`next`/`send` 结果按 unknown 落槽
  漏 retain 的问题（运行时按机器字返回，降级器补静态类型）。
- 用例 `tests/run/28_yield_from.py`、`tests/run/29_async_comp.py`；
  差分测试的 gen/async 生成器补上对应路径。

已知限制：`yield from` 不能 throw、表达式值为 None；异步推导式只能
作为语句/赋值/return 的根表达式，元素里不能含 `await`，没有异步
生成器表达式。

## 0.22：Future 回调与 set_exception（已完成）

- `Future.add_done_callback(fn)`：完成/取消时按注册顺序同步调用
  `fn(future)`；对已完成的 Future 注册会立即调用。
- `Future.set_exception(msg)`：把 Future 标记为失败，`await` 抛出
  `Exception(msg)`；`exception()` 返回消息。
- 运行时新增 `py23_call1` 跳板，按闭包 ABI 调用函数值；回调列表参与
  RC 和 GC。
- 用例 `tests/run/30_future_cb.py`。
- 同时完成工程重构：lower/typecheck/parser/runtime 按职责拆分。

已知限制：回调同步执行（CPython 用 `call_soon`）；回调异常向外传播
并跳过剩余回调；`set_exception` 接收消息字符串而不是异常对象。

## 0.23：异步生成器协议（已完成）

- `asend(v)`：向 `x = yield ...` 送值并取下一个值；驱动的 `driver_sent`
  跨事件循环持有引用，恢复时写回帧并转发。
- `__anext__()`：等价于 `asend(None)`。
- `aclose()`：停在 `yield` 上走 close 清理分发；从未启动时不执行
  函数体；正在等内部 awaitable 时注入 `GeneratorExit`。
- 用例 `tests/run/31_asyncgen_proto.py`。

已知限制：没有 `athrow`（异常不是一等值）；`aclose` 不 await
`async with` 的 `__aexit__`；并发 asend + aclose 时另一个驱动对象会挂起。

## 0.24：异常值模型与内置函数（已完成）

- 异常值对象：`ValueError("msg")` 可作为表达式值，`raise e` 抛实例，
  `except ... as e` 绑定带 `args` 的异常值；`str(e)`/`print(e)` 是消息，
  `e` 参与字符串拼接按消息处理。
- `Future.set_exception` 接受异常值；`Future/Task.exception()` 返回异常值；
  异步生成器补 `athrow(exc)`。
- 内置函数：`abs` / `min` / `max` / `sum` / `pow`，覆盖多实参、容器迭代、
  空序列 ValueError 与 int/float 混合。
- 用例 `tests/run/32_exception_value.py`、`tests/run/33_builtins.py`。

已知限制：`args` 只保存消息；`raise ... from`、traceback、异常链和自定义
异常类仍未支持。

## 0.25：异常收尾与 any/all（已完成）

- 自定义异常类：`class MyError(ValueError): pass`，支持多级继承；
  类体目前只允许 `pass`/文档字符串。
- `raise ... from cause` 与 `e.__cause__`；`raise ... from None` 清空原因。
- 内置函数 `any` / `all`，支持短路和 list/tuple/str/dict/set。
- 测试脚本并行化：`tests/run_tests.sh` 支持 `PY23_TEST_FILTER` /
  `PY23_TEST_JOBS`，`run_diff.py` 支持 `--jobs`；全量测试从约 9s 降到 3s，
  810 程序差分约 8s。

已知限制：`args` 只保存消息；没有 traceback；自定义异常类不能带字段/方法。

## 下一步

- 语言面：方法/类装饰器的剩余形态、生成器 `throw()`、异步生成器 `athrow`
- 运行时：fd 读写/子进程、线程安全事件循环
- 工程面：按 [技术债](#技术债) 顺序处理（优化遍、列表 repr、异常对象等）

## 待决策

这些不是「做不做」，是「选哪条路」，需要先定下来再动手：

| 问题 | 选项 | 影响 |
|---|---|---|
| 字符串语义 | 字节串（现状）/ UTF-8 码点 | 改码点则 `len` 变成 O(n)，索引要解码；不改则非 ASCII 的 `len` 与 CPython 不一致 |
| 整数溢出 | 回绕（现状）/ 溢出检查 / 升格 bignum | 检查要每条算术加一条 `jo`；bignum 要一整套运行时 |
| `print` 求值顺序 | 边算边写（现状）/ 对齐 CPython | 只有副作用可见时才看得出来 |
| 许可证 | 未定 | 发布前必须定 |

## 技术债

按「什么时候会疼」排序：

1. **GC 是保守且 stop-the-world**：栈/寄存器里的整数可能被误认为根，
   回收时机不确定；没有分代/增量/栈映射
2. **unknown 引用的计数不精确**：unknown 容器里指向仍有其它根的对象的
   引用不做 rc 递减，要等下一轮不可达才回收
3. **Optional 的收窄能力不足**：`x: Optional[str]` 在 `if x is None: ...` 之后、跨语句块或跨函数调用时仍按 Optional 处理，真实代码里有约 6 个文件因此报「Optional 上没有这个方法」。这属于「误杀」，与「宁可漏报」的原则相悖，但修它需要正经的数据流分析
4. **没有优化遍**：常量折叠、CSE、死代码消除一个都没有。IR 的形状是为它们准备的，但一个都没写
5. **`tuple` 与 `list` 表示相同**：现在 tuple 也是可变的动态数组，语义上不严谨
6. **诊断不带列区间**：只指向起点，长表达式不好定位
7. **没有增量重编 Python 源码**：每次 `build` 都是全量编译，没有缓存
8. **浮点变量不进寄存器**：`xmm` 全是 caller-saved，跨调用要自己 save/restore，所以只提升了整数/指针变量。纯浮点循环（5000 万次累加）0.25s，整数同样次数 0.06s。可行的修法是「函数内没有任何调用时」才提升浮点变量，这样不需要保存；代价是 `load.slot` / `store.slot` 要按类型选 `movsd` 而不是 `movq`
9. **dict/set 删除是 O(n)**：没有墓碑，每次删除按 `order` 整表重建；小表无所谓，大表高频删除会退化。后续可以换墓碑 + 定期重哈希，或惰性删除
10. **异常路径的 RC 不精确**：longjmp 跳过普通函数帧时局部变量不执行 `release_slots`，要等 GC；如果要确定性回收，需要给每个函数登记展开清理回调
11. **异常对象仍偏轻**：现在有异常值、`args`、自定义异常类和 `raise ... from`，但 `args` 只保存消息，没有 traceback
12. **含 handler 的函数没有寄存器提升**：longjmp 会回滚 callee-saved 寄存器，只能全走内存槽；异常密集且热点的函数会吃亏
13. **生成器/协程的表达式能力不全**：`yield`/`yield from`/`await` 只能作为语句或赋值右值，`yield`/`await` 位于 `finally`/`except` 体内都还没接；生成器表达式仍是立即求值的 list；`send`/await 跨挂起值的静态类型不精确；`yield from` 的值固定为 None，也不能 throw
14. **生成器/协程异常逃出时槽位不精确释放**：异常从 resume longjmp 出去时只把帧标成 aborted，堆槽引用交给 GC 下一轮回收，不做确定性 `rc_dec`
15. **跨模块生成器/协程不支持**：`.py23i` 里 `Generator[T]` / `Coroutine[T]` 降级成 unknown，导入模块的生成器/协程无法 `for` / `next` / `await`
16. **asyncio 只是最小集合**：自定义 `__aiter__`、fd 读写/子进程/线程安全循环未支持；`aclose` 不 await `async with` 的 `__aexit__`，停在 `async with` 里被直接回收时不等待退出；`wait_readable` 不是 CPython 标准 API；Future 回调同步执行；`gather` 结果按 unknown 容器处理；Task 只允许一个并发等待者；异步推导式只能出现在语句/赋值/return 的根位置
