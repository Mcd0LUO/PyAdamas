# 语言参考

py23 借 Python 的语法外壳，但**不是 CPython 的实现**：语义按静态类型的需要重新定义，不追求行为一致。这份文档说明「什么能写、写出来是什么语义、什么会被拒」。

关于和 CPython 的具体差异清单，见 [compatibility.md](compatibility.md)。

## 词法

| 项目 | 规则 |
|---|---|
| 换行 | `\r\n` / `\r` 在入口统一成 `\n`，所以 CRLF 文件的列号、字符串内容都不会错位 |
| 缩进 | 空格与制表符混用按 tabstop=8 展开；缩进栈不匹配报 `IndentationError` 风格的错 |
| 续行 | 括号内隐式续行；行尾 `\` 显式续行 |
| 注释 | `#` 到行尾；`# type: ignore` 一类注释不特殊处理 |
| 数字 | 十进制 / `0x` / `0o` / `0b`，支持 `_` 分隔；浮点支持 `.5`、`5.`、指数、`j` 后缀直接拒绝 |
| 字符串 | 单/双/三引号，前缀 `r` `b` `f` `rf` 任意顺序组合；相邻字面量自动拼接 |
| f-string | 按 PEP 701 解析：同引号嵌套、自文档化 `f"{x=}"`、spec 里再嵌 `{}` 都支持；format spec 支持 fill/align/sign/`0`/width/precision/类型 `d,b,o,x,X,f,e,E,g,G,%` 与 `!s/!r/!a` |

有几处词法层就拒绝，因为语义没有对应物：`bytes` 字面量、`complex` 字面量、超出 int64 的整数、`0x1L` 这类 Python 2 写法。

`\N{NAME}` 是个特殊处理：词法层整体吞掉（否则 `{` 会被当成 f-string 的替换字段），但不查 Unicode 名字表，字符串里保留原文。

## 语法子集

### 语句

| 分类 | 支持 |
|---|---|
| 简单语句 | 表达式语句、赋值（含链式、多重解包）、增量赋值、标注赋值、`pass`、`return`、`break`、`continue`、`raise`、`assert`、`global`、`import` / `from ... import` |
| 复合语句 | `if` / `elif` / `else`、`while`（含 `else`）、`for` / `async for`（含 `else`）、`def` / `async def`（含默认参数、`*`、`/`、仅限关键字参数、`*args` / `**kwargs`、装饰器、生成器/协程/异步生成器）、`class`（多继承、C3 MRO、方法与字段）、`with` / `async with`（多个项、`as`）、`try` / `except` / `else` / `finally` |
| 能解析、不能生成 | 类装饰器、非 `@staticmethod` / `@property` 的方法装饰器、`match` 之外的软关键字写法 |

`assert` 条件为假时抛出 `AssertionError`，第二个参数作为消息；没有消息时是 `assertion failed`。

### 表达式

支持：常量、名字、二元/一元运算、布尔短路、链式比较、条件表达式、调用（位置 + 关键字 + `*` / `**` 展开）、下标（读与写）、切片（读与赋值）、列表/元组/字典/集合字面量、列表/集合/字典推导式（含 `async for`）、生成器表达式（按 list 立即求值）、`in` / `not in`、f-string、`lambda`、海象运算符、`range()`、`yield`、`yield from`、`await`、用户类的属性读与写、用户类的方法调用。

没有代码生成的：内置类型上的属性访问（除 `list.append` / `list.pop` 两个特例）。

## 类型系统

### 表示

| 类型 | 说明 |
|---|---|
| `int` | 64 位有符号，溢出回绕（不升格为任意精度） |
| `float` | IEEE 双精度 |
| `bool` | `int` 的子类型，机器上是 0/1 |
| `str` | 不可变字节串，`len` 在对象头里（O(1)） |
| `list[T]` | 动态数组，元素 8 字节槽 |
| `tuple[...]` | 定长元组；`tuple[T, ...]` 表示变长 |
| `None` | 单值类型 |
| `Optional[T]` / `T \| None` | 可空 |
| `func(...) -> T` | 函数值 |
| `generator[T]` | 生成器对象；`for` / `next` 产出 `T`，`send` 送入的值是 `unknown` |
| `coroutine[T]` | 协程对象；`await` / `asyncio.run` 得到 `T` |
| `task[T]` | `asyncio.create_task` 调度的任务；`await` 得到 `T`，有 `done()` / `cancel()` |
| `asyncgen[T]` | `async def` + `yield` 的异步生成器；`async for` 或 `asend` / `athrow` / `__anext__` / `aclose` |
| `future[T]` | `asyncio.Future`；可多等待，`set_result` 之后可 `await`/`result` |
| `类名` | 类实例；字段布局与 vtable 在编译期确定 |
| `set[T]` / `dict[K, V]` | 堆上的哈希表；`set` 复用 `dict`（值恒 0），保留插入顺序 |
| `unknown` | 推断失败的占位，任何检查都放行 |

### 标注规则

- **函数签名必须标注**（参数与返回值），否则 `def` 里的参数类型是 `unknown`，调用处也不做检查。
- 局部变量和模块变量**可选标注**。
- 标注是**约束**：`x: int = "s"` 直接报错；`x: int = 1` 之后再 `x = "s"` 也报错（有标注的变量类型固定，这是后端按槽选 i64/f64 的前提）。
- 给有标注的变量赋 `None` 视为「清空」，不报错、也不把类型收窄成 `None`。
- 字符串注解（`"Node"` 这种前向引用）会回到解析器重新解析，深度超过 8 层降级成 `unknown`。

### 推断规则

没标注时按使用推断，原则是**宁可漏报，不可误杀**：

| 情况 | 处理 |
|---|---|
| `x = None` 占位 | 保留已有类型，不收窄 |
| `x = 1` 然后 `x = "s"` | 取公共上界，合不上就变 `unknown`，后续检查放行 |
| `if/elif/else` 各分支赋值 | 每个分支从同一起点出发，最后取上界 |
| `try` 体里改过的变量 | `except` 里按入口类型的上界看 |
| `if x is None: return` | 之后 `x` 收窄为非 None（`is not None` 两个方向都收窄） |
| 开放容器（字面量推出来的） | 元素不兼容就扩宽成 `unknown`；标注过的容器（`x: list[int]`）才报错 |

### 隐式转换

只有数值域一条：**int → float**，发生在赋值、传参、返回、列表/元组字面量构造这四个位置。

```python
x: float = 1        # 1 变成 1.0
def f(a: float) -> float: ...
f(2)                # 2 变成 2.0
```

反向（float → int）不隐式发生，要写 `int(x)`。

注意由此带来的可见差异：`x: float = 1; print(x)` 打印 `1.0`，CPython 打印 `1`。因为我们真的把值转成了 double，而且没有装箱。

## 求值语义

### 真值

`if` / `while` / `and` / `or` / `not` 的真值判定：

| 值 | 真假 |
|---|---|
| `None` | 假 |
| `0` / `0.0` / `False` | 假 |
| 空字符串 / 空列表 / 空元组（`len == 0`） | 假 |
| 其余 | 真 |
| `NaN` | **真**（与 Python 一致） |

### 算术

| 运算符 | 语义 |
|---|---|
| `+ - *` | 整数回绕；浮点 IEEE |
| `/` | **永远返回 float**，两边都是整数时先转换 |
| `//` | 向下取整（不是向零取整）：`-7 // 3 == -3` |
| `%` | 结果与除数同号：`-7 % 3 == 2`；浮点同理，`-0.0 % 3.0 == 0.0` |
| `**` | 整数底数指数走整数幂；**负指数返回 0**（CPython 返回 float） |
| `& \| ^ ~ << >>` | int64 位运算；**移位次数按 64 取模**（`1 << 70 == 64`，CPython 是任意精度） |

除零和 `INT64_MIN / -1` 会提前检查并 panic，不会变成 `#DE` 信号。

### 比较

- `int` 与 `float` 可混比（先提升再比）。
- `str` 按字节序比较，UTF-8 的字节序与码点序一致，所以结果与 Python 相同。
- `==` 对 `str` 是逐字节比较；对 `list` / `tuple` 是**指针比较**（这是与 Python 最大的语义分歧）。
- 浮点比较里 `NaN` 参与的一切 `<` `<=` `>` `>=` `==` 都为假，`!=` 为真。

### 字符串

不可变，`len` 为字节数（不是字符数）。索引支持负号，越界 panic。
`+` 拼接分配新对象。切片 `s[a:b:c]` 按**字节**切，语义与 CPython 的
str 切片一致（只是单位是字节）。方法（`upper()` 这类）还没实现。

### 列表

`list[T]` 是动态数组，`append` 均摊 O(1)，索引支持负数，越界 panic。列表字面量的元素类型由所有元素的上界推出（`[1, 2.5]` 是 `list[float]`，整数元素在构造时就被转成 double）。

`l[i] = v` 支持；切片读 `l[a:b:c]` 返回新列表并接管元素引用；
`l[i:j] = ...` 切片赋值不支持。

### 切片

`str` / `list` / `tuple` 支持读切片 `x[a:b:c]`：

- 缺省边界、负数边界、正负步长按 Python 语义在运行时归一化；
- 结果总是新对象（str 新字节串，list/tuple 新列表）；list 切片按源
  元素的堆类别 retain 元素；
- `step == 0` 运行时 panic；越界边界被夹到有效范围；
- **切片赋值** `l[a:b:c] = value` 支持 list：`step == 1` 时可以改变长度
  （替换 / 插入 / 删除），扩展切片要求右侧长度与切片长度一致，否则
  panic；右侧可以是 list/tuple，也可以是 str（按字节拆成字符列表）；
- `del l[a:b]` 支持 step=1 的删除；扩展切片删除（step≠1）仍会报 ValueError。tuple 的切片赋值仍不支持。

### 推导式

支持 list / set / dict 推导式与生成器表达式：多个 `for`、每个 `for` 的
`if` 过滤、嵌套推导式，以及 `range()` 作为迭代源。

```python
sq = [x * x for x in range(5)]
ev = [x for x in xs if x % 2 == 0]
pairs = [(x, y) for x in [1, 2] for y in [3, 4]]
index = {k: len(k) for k in ["a", "bb"]}
mods = {x % 3 for x in range(10)}
```

推导式有独立的变量作用域：循环变量不会覆盖同名的外层变量。生成器
表达式没有惰性求值，直接按 list 构造。含 `async for` 的推导式只能在
`async def` 里使用，可以混用同步/异步子句；它只能作为整条语句、赋值
或 `return` 的根表达式（挂起会清空当前表达式的临时值），元素表达式
里也不能含 `await`。推导式内部暂不支持 `lambda` 捕获循环变量、
海象运算符写回外层和异步生成器表达式。

### 函数

函数是一等值，可以作为参数、返回值、存进列表。默认参数**必须是字面量常量**（在调用点内联填充，因为签名在编译期已知）。递归直接支持。

支持**闭包**：内层函数可以读外层函数的局部变量；要写回外层变量必须写
`nonlocal`。捕获按**引用**语义工作：同一个外层调用里创建的多个闭包共享
同一份变量，外层后来赋值也能被闭包看到；不同外层调用之间互不影响。

```python
def make_counter():
    n: int = 0
    def inc() -> int:
        nonlocal n
        n = n + 1
        return n
    return inc

f = make_counter()
g = make_counter()
print(f(), f(), g())    # 1 2 1
```

实现上被捕获变量变成单元素 `list[T]`（cell），闭包对象持有 cell 指针。
嵌套函数直接引用自己的名字按递归处理，用当前闭包作为函数值。

支持 `lambda`：它会被脱糖成一个插在语句前的合成嵌套 `def`，因此捕获、
默认参数和引用计数规则与普通函数完全一致。lambda 参数不能带标注；
类体字段默认值里的 lambda 暂不支持。

支持 `*args` / `**kwargs`：形参在 IR 里分别是 `list[T]` 和
`dict[str, V]`（Python 里 `*args` 是 tuple，这里两者表示相同），调用点
把多出来的实参打包成一个容器传进去。`f(1, *lst)`、`f(*t)`、`f(**d)`、
`def wrapper(*args, **kw): inner(*args, **kw)` 这类转发都支持。限制：

- `*` 位于固定形参之前时不能动态展开（编译期必须知道实参个数）；
  右侧是定长 tuple 时例外，可以静态展开；
- 通过 `unknown` 类型的函数值调用时不能带 `*` / `**`（没有运行时签名
  信息，无法绑定）；
- 同一个 `**mapping` 的键与显式命名形参重名时，不会像 CPython 那样报
  `TypeError`，后者会静默留在 `**kwargs` 里。

**装饰器**：函数装饰器 `@d1` / `@d2` 按从下到上应用，等价于 `f = d1(d2(f))`；
装饰器必须是签名已知的函数值。包装函数需要用与目标一致的固定形参来
转发（不能依赖 `*args` 转发给 unknown 被装饰者）。方法装饰器支持
`@staticmethod`（实例和 `Class.method` 调用，不传 self）、`@property`、
`@x.setter` 和 `@x.deleter`；属性访问编译成 getter/setter/deleter 的虚调用。
`@classmethod`、自定义方法装饰器和类装饰器仍被后端拒绝。

相互递归的嵌套闭包会形成 cell 环；这类环不是引用计数能处理的，由下面的
循环收集器回收。

### 生成器

函数体里出现 `yield` 就变成生成器函数：调用它不执行函数体，而是返回
`generator[T]`，其中 `T` 是所有 `yield` 值类型的公共上界。

```python
def count(n: int):
    i = 0
    while i < n:
        yield i
        i += 1

for x in count(3):      # 0 1 2
    print(x)

g = count(2)
print(next(g), next(g)) # 0 1
```

支持 `for` / `next()` / `.send(v)` / `.close()` / `.__next__()`。`send` 的
值成为 `x = yield v` 里 `x` 的值（普通 `next` 是 None）。生成器函数里
`return` 只表示结束，返回值被丢弃。

**`yield from`**：把子迭代器的值逐个转 yield 出去；委托生成器时
`send` 的值会转发给子生成器，子生成器结束（或出错）后继续执行外层
代码。可委托生成器、list / tuple / str / dict / set；其他静态类型
（unknown）按生成器协议尝试，失败抛 `TypeError`。外层生成器被
`close()` 时，子生成器的引用被释放，它的 `finally` 照常执行。表达式
的值固定为 None（异常对象不保存 `StopIteration.value`）。

```python
def inner():
    yield 1
    yield 2

def outer():
    yield from inner()
    yield from [3, 4]
```

**跨 yield 清理**：`yield` 可以出现在 `try` / `with` 体内；挂起时活跃的
handler 记录被摘下，恢复时按原顺序重挂并重新建立 setjmp。`close()`、
引用计数归零与 GC 前的 finalize 会按挂起点快照执行 `finally` 和
`with.__exit__(None, None, None)`。异常从生成器逃出时，生成代码的
landing 路径负责展开清理，帧被标记 aborted，避免销毁时重复执行。

限制：`yield` / `yield from` 只能作为语句或赋值右值（`x = yield v`），
不能位于 `finally` 或 `except` 体内；生成器表达式仍是立即求值的 list。
生成器类型跨模块接口会降级成 `unknown`。

### 协程

`async def` 调用返回 `coroutine[T]`，`T` 是 `return` 的类型；`await`
挂起当前协程，等被等待对象完成后取回结果：

```python
import asyncio

async def double(x: int) -> int:
    return x * 2

async def add(a: int, b: int) -> int:
    x = await double(a)
    await asyncio.sleep(0.01)
    return x + b

print(asyncio.run(add(3, 4)))   # 10
```

内置 `asyncio` 提供：

- `asyncio.run(coro)`：事件循环跑到根协程结束并返回结果；根协程的
  异常抛回同步调用点。
- `asyncio.sleep(seconds)`：毫秒定时器，`0` 立即就绪；
- `asyncio.gather(a, b, ...)`：并发启动，按实参顺序返回结果列表。
- `asyncio.create_task(coro)`：立即调度协程，返回 `task[T]`；`await
  task` 取结果，完成后可重复 await；`task.done()` 判断是否结束；
  `task.cancel()` 请求取消（返回 bool）。
- `asyncio.Future()`：手动完成的 future（`set_result` / `set_exception` /
  `add_done_callback` / `result` / `exception` / `cancel`）。
- `asyncio.Event()`：`await ev.wait()`、`ev.set()`、`ev.is_set()`；
  支持 `async with ev:`（进入时等 set，退出无动作）。
- `asyncio.Lock()`：`await lock.acquire()`、`lock.release()`、
  `lock.locked()`；等待者按 FIFO 唤醒，支持 `async with lock:`。
- `asyncio.Queue([maxsize])`：`await q.put(v)`、`await q.get()`、
  `q.put_nowait(v)` / `q.get_nowait()`（满/空抛 `QueueFull` /
  `QueueEmpty`，按内置异常名捕获）、`q.qsize()`、`q.empty()`、
  `q.full()`、`await q.join()`、`q.task_done()`；`maxsize > 0` 时满
  队列的 put 会阻塞，get 腾出位置后按 FIFO 放行；元素静态类型按
  unknown 处理（`get` 的 await 结果不能用字符串方法等需要具体类型的
  操作）。
- `asyncio.wait_for(aw, timeout)`：可等待对象与超时竞速；超时取消
  内部任务并抛 `TimeoutError`，内部对象可以是协程、Task 或 Future。
- `asyncio.timeout(delay)`：`async with asyncio.timeout(0.5):` 在块内
  到期时向当前协程注入 `TimeoutError`（块外的 `try/except` 能捕获），
  退出时取消定时器；`t.expired()` 查询是否已超时。
- `asyncio.Semaphore([value=1])`：`await sem.acquire()`、
  `sem.release()`、`sem.locked()`；等待者 FIFO，支持
  `async with sem:`；release 多调时名额按 CPython 语义累加。
- `asyncio.TaskGroup()`：`async with ... as tg:` 内用
  `tg.create_task(coro)` 启动子任务；退出时等待全部结束，任一失败则
  取消其余任务并把第一个异常抛给调用方（不是 CPython 的
  `ExceptionGroup`）。

也支持 `from asyncio import sleep, gather, run, create_task, Event, Lock,
Queue, wait_for, timeout, TaskGroup, Semaphore`（跨模块 `run`/`wait_for`
的结果类型会退化成 unknown）。async 方法、`with` 里的 `await`、跨 `await` 的
`finally` 都可以用；异常跨 `await` 通过在恢复时注入异常传播，
`try/except` 能正常捕获。

取消会向任务注入内置异常 `CancelledError`（父类是 `BaseException`，
`except Exception` 不会误吞）：排队中的任务直接注入，挂起中的任务先
从当前 awaitable 解绑再注入，`finally` 照常执行。被取消协程若自己
捕获 `CancelledError` 并继续运行，任务按正常完成处理。

**异步生成器**：`async def` 里同时有 `yield` 时，调用得到
`asyncgen[T]`，用 `async for` 消费：

```python
async def agen(n: int):
    i = 0
    while i < n:
        await asyncio.sleep(0)
        yield i
        i += 1

async def main() -> int:
    s = 0
    async for v in agen(3):
        s += v
    return s
```

生成器内部可以混用 `await` / `yield`；`return`、`break` 中断迭代后，
生成器对象被回收时同步执行 `finally`（`finally` 里不能再 await）。

协议方法（都返回 coroutine，需要 `await`）：

- `asend(v)`：把 `v` 送进 `x = yield ...` 的 `x` 并取下一个值；
  非 None 值可以跳转生成器的内部状态（示例见 `tests/run/31_asyncgen_proto.py`）。
- `__anext__()`：等价于 `asend(None)`。
- `athrow(exc)`：把异常值注入挂起点；生成器捕获后可以继续 `yield`，
  没捕获则向外抛出，已结束的生成器抛 `StopAsyncIteration`。
- `aclose()`：关闭生成器。停在 `yield` 上时走 close 清理分发，执行
  `finally` / `with.__exit__`；从未启动时不执行函数体；正在等内部
  awaitable 时注入 `GeneratorExit`，让 `finally` 在挂起点展开。
  `async with` 的 `__aexit__` 目前不会在 aclose 里 await。

**异步上下文管理器**：类上定义 `async def __aenter__` /
`async def __aexit__`，用 `async with` 进入；进入、正常退出、异常展开、
`return` / `break` / `continue` 路径都会 await `__aexit__`，
返回真值可吞掉异常。若协程在 `async with` 体内被取消，退出路径同样
会 await `__aexit__`；只有生成器对象被直接回收且停在 `async with`
里时无法等待退出（限制）。

**Future**：`asyncio.Future()` 创建一个可由别的协程 `set_result` 的
等待对象，可以被多个协程同时 `await`：

```python
async def setter(f: Future[int]) -> None:
    await asyncio.sleep(0.01)
    f.set_result(42)

async def main() -> int:
    f = asyncio.Future()
    asyncio.create_task(setter(f))
    x = await f          # 42
    return f.result()
```

方法：`set_result(v)`、`set_exception(exc)`、`add_done_callback(fn)`、
`cancel()`、`done()`、`cancelled()`、`result()`（未完成抛 RuntimeError，
失败/取消抛原异常）、`exception()`（成功返回 None，失败返回异常值）。
回调在完成时按注册顺序**同步**调用，参数是 future 本身；对已完成的
Future 注册会立即调用。`set_exception` 接收异常实例（也兼容消息字符串）。
Task 也有 `result()` / `exception()` / `cancelled()`。

**取消级联**：取消 Task 会沿它等待的对象向下传播：协程链继续注入
`CancelledError`，被等待的 Task 递归取消，gather 取消全部子任务，
Future/sleep/fd 等待标记取消；各层 `finally` 都会执行。被等待的
Future 被取消时，其他等待者也会收到 `CancelledError`。

**fd 等待**：`asyncio.wait_readable(fd)` / `wait_writable(fd)` 返回
可等待对象，事件循环空闲时用 `poll` 等待，超时取最近的定时器；
无效 fd 以 `OSError` 结束等待。这是 py23 自己的低级接口（CPython
用 `loop.add_reader`），没有配套的 fd 读写函数。

限制：`await` 只能作为语句、赋值右值或 `return` 值；不能出现在
`finally` / `except` 体内；异步推导式只能作为语句、赋值或 `return` 的
根表达式；不支持自定义 `__aiter__` 迭代器、fd 读写和线程安全的事件
循环；Future 回调是同步调用（CPython 用 `call_soon` 排队），
`gather` 的实参个数在源码里写死，结果元素类型按 unknown 处理；
Task 只允许一个并发等待者，没有任务名字。

### 内存与生命周期

堆值（字符串、列表、元组、字典、集合、类实例、闭包）用**引用计数**自动
回收，语言层看不到 `free` 或析构函数。

- 赋值、传参、返回、字段/列表写入都会给堆值增加一个引用；变量覆盖、
  离开作用域时释放旧引用。
- 表达式临时值由运行时临时值栈兜底，语句、循环、函数边界统一清理。
  所以 `s = s + "x"` 在长循环里不会无限增长。
- **循环引用由保守 GC 回收**：`a.next = a`、相互递归闭包形成的环会在
  分配量超过阈值（约 2MB）时被标记-清扫；程序退出时也会清空整个堆。
- 静态类型看不见的值（`unknown`、`list[unknown]`、混合类型元组）不参与
  精确计数；GC 会保守扫描、把不可达对象回收，但「指向仍有其它根的对象」
  的计数可能虚高，要到后续轮次才回收。这是「宁可延迟回收，不可误收」的
  取舍。
- 没有 `__del__`、`weakref` 或手动释放；对象什么时候真正释放不可观察。
  GC 是 stop-the-world 的，没有分代/增量。

### 字典与集合

`dict[K, V]` 是开放寻址哈希表，`set[T]` 复用同一张表（值恒 0）。
两者都保留**插入顺序**；`len()` 和真值判断都是 O(1)。支持：

- 字面量 `{"a": 1}`、`{1, 2, 3}`（`{}` 是空字典；空集合没有字面量）；
- 下标读 `d[k]`（缺失时 panic `KeyError: key not found`）与写 `d[k] = v`；
- `len(d)`、`if d:`、`for k in d`、`for x in s`；
- `in` / `not in`：dict/set 按 key，list/tuple 线性扫，str 子串；
- 类型标注 `dict[str, int]`、`set[str]`。

key 的哈希/判等按静态类型选：int/bool 用机器字、str 用字节（FNV-1a +
逐字节比较）、float 用 IEEE 值（`0.0` 与 `-0.0` 同键）；其他类型按机器
字比较。支持的方法：

- `d.get(k)` / `d.get(k, default)`：有默认值时缺失返回默认值；没有默认值
  时缺失像 `d[k]` 一样 panic（静态返回类型不是 Optional）；
- `d.keys()` / `d.values()` / `d.items()`：返回新 list（不是视图），元素
  按插入顺序；`items()` 的元素是 `(key, value)` 元组；
- `d.update(other)`；
- `s.add(x)`、`s.update(other_set)`。

删除类方法也支持：

- `d.pop(k)` / `d.pop(k, default)`：返回值；缺失且没有默认值时 panic；
- `d.popitem()`：按**后进先出**移除最后插入的项，返回 `(key, value)`；
- `d.clear()` / `s.clear()`；
- `s.remove(x)`：缺失 panic；`s.discard(x)`：缺失忽略；
- `s.pop()`：移除最后插入的元素（CPython 是任意元素，这里选择确定性）。

开放寻址表没有墓碑，删除后按 `order` 剩余项**整表重建**，单次删除是
O(n)；删除不在热路径上，这个代价换的是实现简单和遍历顺序稳定。

空字典的 key 类型是 `unknown`，不同对象但内容相同的动态字符串 key 可能
按机器字比较不相等；写了标注或由字面量推出 `str` key 时没有这个问题。
`dict`/`set` 的 `==` 仍是指针比较，没有深比较。

### 类

`class` 是**编译期布局**：字段偏移和 vtable 槽号在编译时就定死，属性访问是一条固定偏移的 `movq`，方法调用是「取表、取槽、间接跳转」。

```python
class Point:
    x: int
    y: int

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def norm2(self) -> int:
        return self.x * self.x + self.y * self.y
```

| 项 | 约定 |
|---|---|
| 类体 | 方法定义、带标注的字段声明、`pass`、文档字符串，以及其它语句（类变量赋值、`if`、`for`、`print` 等）在类定义点执行；类命名空间未建模，赋值按模块全局处理 |
| 字段 | 标注可选：`x: int` 定死偏移；`self.x = ...` 也能长出字段 |
| 字段初值 | `n: int = 0` 在**每次构造时**求值，不是 CPython 的类属性 |
| 构造 | `Point(1, 2)` = 分配 + 初始化带初值的字段 + 调 `__init__` |
| 继承 | 单继承；子类字段接在父类之后，方法槽位前缀与父类完全一致 |
| 覆盖 | 子类同名方法占用父类同一个槽，所以父类类型的变量能调到子类实现 |
| `self` | 普通参数，名字不限；省略标注时类型就是所属类 |
| 属性读 | `p.x` → `movq 8(%rax), %rax` |
| 方法调用 | `p.m(...)` → 从对象头取 vtable，按常量槽号取地址后间接调用 |
| 打印 | `print(p)` 输出 `<Point object>`（没有 `__str__`） |

字段初值在构造点求值，比 CPython 的「类属性 + 实例回退」更简单也更安全：`items: list[int] = []` 每个实例拿到的是**独立**的空列表，不会出现所有实例共享一个 list 的经典 bug。

**不写标注的字段**。Python 的实例属性本来就是 `__init__` 里长出来的，强制先写标注会把标准库挡在门外。所以 `self.x = ...` 里 `x` 没声明过时，编译器按**首次赋值顺序**把它补进布局，类型取右侧表达式的推断结果：

```python
class Connection:
    def __init__(self, host: str) -> None:
        self.host = host      # 补出字段 host: str
        self.sock = None       # None 太弱，字段类型记 unknown
```

初值是 `None` 的字段类型记 unknown 而不是 `None`——否则后面每一处 `self.sock.send(...)` 都会被判死。推断出来的字段赋值时不检查类型（`self.x = 1` 之后再 `self.x = "s"` 只是把类型放宽成 unknown），只有写了标注的字段才严格。

**看不见的父类**。基类是外部导入的类型（`unittest.TestCase`、`OrderedDict`）时，它的字段和方法我们看不到，这个类就是「开放」的：查不到 `self.xxx` / `self.method()` 时按 unknown 放过，不报错。标准库里每个测试类都继承 `TestCase` 再调 `self.assertEqual`，不放过就是大面积误杀。

**多继承**。`class C(A, B)` 用标准 C3 线性化算 MRO，方法查找、vtable 填充和
`super()` 都沿这条链。第一个基类仍提供字段布局前缀；附加基类只能复用
主基类布局里已有的字段（共享祖先的菱形可以，新增字段不行）。
方法槽在模块内按名字全局分配，同名覆盖在任意 MRO 路径上写同一个槽。
零参 `super().m(...)` 支持协作式调用：运行期按实例真实 MRO 找下一实现；
显式 `super(C, self)` 尚未支持。类体里的非声明语句在类定义点执行；
赋值按模块全局处理，`C.x` / `self.x` 这类类属性访问仍未建模。

不支持：`@classmethod`、自定义方法装饰器、类装饰器、`super(C, self)`、
`__str__` / `__eq__` 等协议方法、`isinstance` 的导入类目标（本地类与内置
类型支持）、把类本身当值用（`f = Point`）。
`__init__` 之外的构造协议（`__new__`）也没有。

没有 `__init__` 时只能零参构造——基类在外部的类除外，`__init__` 可能在那边。`__init__` 的默认参数和关键字参数与普通函数规则相同。

### with

`with expr as name:` 要求 `expr` 是用户类实例，并定义 `__enter__` /
`__exit__`：进入时调用 `__enter__`，返回值绑定到 `as` 目标；正常退出时
调用 `__exit__(None, None, None)`，异常退出时调用
`__exit__(type_name, message, None)`。正常结束、`return`、`break`、
`continue` 都会执行 `__exit__`；`__exit__` 返回真值会抑制异常，否则异常
继续向外抛。多个 `with` 项按嵌套处理，退出顺序相反；`__enter__` 抛异常
时，外层已进入的 `__exit__` 仍会执行。
`open(path[, mode])` 返回文件对象（独立的 `file` 类型），可以直接
`with open(...) as f:`；正常、异常、`return`、`break`/`continue` 路径都会
关闭文件（见下方「文件」）。

### 文件

`open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None,
closefd=True)` 返回文件对象，位置/关键字参数按 CPython 的名字绑定：
`file` 是路径字符串或文件描述符；`mode` 校验 `r/w/a/x` 与 `+`/`b`/`t`
的组合；`buffering` 的 0/1/>1 分别映射无缓冲/行缓冲/全缓冲（文本模式不允许
0）；`encoding` 支持任意 glibc iconv 认识的编码名（大小写与 `-`/`_` 不敏感，
`latin-1`、`utf-16` 等别名可解析），文本读写经 iconv 在源编码与 UTF-8 之间
转换，字符串内容始终是 UTF-8 字节；未知编码抛 `LookupError`；`errors` 支持
`strict`（默认，解码/编码失败抛 `UnicodeDecodeError` / `UnicodeEncodeError`）、
`ignore`、`replace`；`newline` 支持 `None`（读时通用换行：`\r\n` 和 `\r`
折成 `\n`，跨读块也拼接正确）、`""`（不转换）、`"\n"`、`"\r"`、`"\r\n"`
（后两者在写时把 `\n` 换成对应形式）；`closefd` 只对 fd 生效；`opener=`
明确不支持。
方法：`read([n])` / `readline([size])` / `readlines([hint])` / `write(str)`
/ `writelines(list[str])` / `seek(offset[, whence])` / `tell()` /
`truncate([size])` / `fileno()` / `isatty()` / `readable()` / `writable()` /
`seekable()` / `flush()` / `close()`；属性 `closed` / `name` / `mode` /
`encoding` / `errors`。`tell()` 在文本模式下返回解码后的逻辑位置，`seek()`
接受同样的值（从文件头重解码定位，O(n)）；二进制模式按原始字节。读预读后
的写入会先把底层句柄退回逻辑位置，`r+` 下 `seek` 后 `write` 的结果与
CPython 一致。
`with open(...) as f:` 在正常、异常、`return`、`break`/`continue` 路径都会
关闭文件；`for line in f:` 按行迭代，`list(f)` / `sorted(f)` /
`enumerate(f)` / `zip(f)` 走 readlines 语义；`iter(f)` 返回文件本身，
`next(f)` 读下一行、EOF 抛 `StopIteration`；`print(..., file=f)` 和
`print(..., file=f, flush=True)` 可把输出重定向到文件。
`sys.stdin` / `sys.stdout` / `sys.stderr` 是文件对象（直接包住 C 标准流，
和 `print` 共用缓冲，对象永生）；支持 `sys.stdout.write(...)`、
`print(..., file=sys.stderr)`、`for line in sys.stdin:` 等写法。
打开失败抛 `OSError`，关闭后读写抛 `ValueError`。

### print

`print` 不是函数，是降级期展开的一串输出调用：

- 支持 `sep=` / `end=`（任意字符串表达式）/ `file=`（文件对象）/ `flush=`。
- 值按静态类型输出：list/tuple 逐元素递归打印（`[1, 2]`，容器里的字符串
  按 repr 带引号）；dict/set 仍只输出 `<dict object>` 占位。
- 求值顺序：边算边写，`sep` 在第一个逗号处求值，`end` 最后求值。CPython 是先全算完再写。

### import

支持本地包与相对导入（包目录必须有 `__init__.py`）：

- `import m` / `import m as n` / `import pkg.mod as m`：`m` 是编译期模块别名。
- `from m import x as y`：快照绑定；`x` 也可以是子模块，绑定为模块别名。
- `from pkg.mod import x`、`from . import mod`、`from .mod import x`、
  `from ..pkg import y`：相对层级按当前包解析。
- `from m import *`：静态展开非下划线名字（暂不解析 `__all__`）。
- 点号 import 不带 `as` 要求改用 `as` 或 `from ... import`；包初始化只在
  显式 `import pkg`/`from pkg import ...` 时执行，不会因为导入子模块自动
  先跑 `__init__.py`。
- 每个模块有自己的 `__name__` / `__file__`；入口模块的 `__name__` 是
  `"__main__"`。
- 内置 `math` / `sys`：`math` 常量 `pi`/`e`/`tau`/`inf`/`nan`，函数
  `sqrt`/`floor`/`ceil`/`fabs`/`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/
  `sinh`/`cosh`/`tanh`/`asinh`/`acosh`/`atanh`/`log`/`log2`/`log10`/
  `log1p`/`exp`/`exp2`/`expm1`/`pow`/`fmod`/`remainder`/`atan2`/`hypot`/
  `degrees`/`radians`/`trunc`/`copysign`/`nextafter`/`ulp`/`ldexp`/`fma`/
  `isnan`/`isinf`/`isfinite`/`isclose`/`erf`/`erfc`/`gamma`/`lgamma`/
  `factorial`/`gcd`/`lcm`/`comb`/`perm`/`isqrt`/`fsum`/`prod`/`sumprod`/
  `dist`/`frexp`/`modf`；`sys` 提供 `argv`/`exit`/`maxsize`/`stdin`/
  `stdout`/`stderr`；原生函数只能直接调用。签名收窄的地方：`hypot`、
  `isclose` 固定两个参数（`isclose` 用 CPython 默认容差），`prod` 只接受
  `list[int]`，`sumprod` 只接受两个等长 `list[int]`，`dist` 只接受两个
  等长 `list[float]`；`frexp`/`modf` 返回二元组。

### 内置函数

已生成代码：`print` / `len` / `int` / `float` / `str` / `bool` / `range` /
`next` / `abs` / `min` / `max` / `sum` / `pow` / `any` / `all` / `complex` /
`ord` / `chr` / `hex` / `bin` / `oct` / `round` / `divmod` / `repr` /
`format` / `list` / `tuple` / `dict` / `set` / `frozenset` / `sorted` / `reversed` /
`enumerate` / `zip` / `input` / `isinstance` / `getattr` / `setattr` /
`hasattr` / `map` / `filter` / `callable` / `issubclass` / `hash`。
`min` / `max` 支持多实参和 list/tuple/str/dict/set 迭代，空序列抛
`ValueError`；`sum` 支持可选的起始值；`any` / `all` 支持短路；`abs` /
`pow` 支持 int 与 float，`pow(x, y, m)` 是 `(x ** y) % m`；
`int("42")` / `int("ff", 16)` / `float("1.5")` 按 CPython 规则解析字符串
（失败抛 `ValueError`）；`ord` / `chr` 按**字节**处理（`chr` 只接受
0..255）；`round` 用「四舍六入五成双」，`round(x)` 返回 int、
`round(x, n)` 返回 float，整数配负 ndigits 也按 10 的幂取整；
`divmod` 返回 `(商, 余数)`，符号规则同 CPython；`repr` 支持字符串/
数值/bool/None；`format` 支持数值和字符串的格式说明符。
`isinstance(x, T)` 的第二个参数支持内置类型名（int/float/str/bool/list/
tuple/dict/set/complex/object）和本地类；静态类型确定时编译期折叠，
unknown 值上对带运行期类型标签的类型（str/list/dict/complex）走运行时
检查，类走 MRO 检查；导入类目标暂不支持。
`getattr` / `setattr` / `hasattr` 只支持类实例，属性名必须是字符串字面量：
`getattr`/`setattr` 支持字段和 property（property setter 暂不支持），
`hasattr` 对存在的字段/方法/property 编译期返回 True/False。
`map(f, xs)` / `filter(f, xs)` 在脱糖阶段改写成列表推导式（结果立即求值）：
`f` 必须是名字（普通函数、`int`/`str` 这类内置转换、或 lambda）；写成
任意表达式会保留普通调用并报 codegen 不支持。`filter(None, xs)` 保留
真值元素。模块里绑定过 `map`/`filter` 名字时不做改写（按普通调用处理）。
`callable(x)` / `issubclass(A, B)` 按静态类型编译期折叠（`callable(int)`
这类内置名也认；unknown 值上的 `callable` 暂不支持）。
`hash(x)` 支持 int/bool/float/str，和字典键共用同一套哈希（同值同哈希，
数值与 CPython 不同；list/dict/set 等不可哈希类型报错）。
`type(x)` 返回静态类型令牌：`print(type(x))` 输出 `<class 'int'>`，
`type(x) == int` / `is` / `type(x) == type(y)` 编译期折叠，
`type(x).__name__` 得到类型名；运行时令牌是驻留字符串，没有动态类对象。
`id(x)` 返回机器字身份（堆指针 / 整数本身 / 浮点位模式），同进程内稳定，
数值与 CPython 不同。
`list(seq)` / `tuple(seq)` 做浅拷贝，`set(seq)` / `frozenset(seq)` 按键
去重（frozenset 复用 set 的运行时，不强制不可变），`dict(**kw)`
用字符串键；`sorted` 支持 int/float/str 元素（`key=` / `reverse=` 尚未
支持）；`enumerate` 产出的元素是 `[下标, 值]` 对，`zip` 支持两个可迭代
对象；这些迭代辅助都返回具体列表，与生成器表达式立即求值的模型一致。
容器方法：list 有 `append` / `extend` / `insert` / `remove` / `pop(i)` /
`clear` / `index` / `count` / `copy` / `reverse` / `sort`；dict 有 `get` /
`keys` / `values` / `items` / `update` / `pop` / `popitem` / `setdefault` /
`clear` / `copy`；set 有 `add` / `update` / `remove` / `discard` / `pop` /
`clear` / `copy`；tuple 有 `count` / `index`；int 有 `bit_length`。
字符串方法（字节串语义、ASCII 大小写）：`upper` / `lower` / `capitalize` /
`title` / `strip` / `lstrip` / `rstrip` / `split` / `splitlines` / `join` /
`replace` / `find` / `rfind` / `index` / `startswith` / `endswith` /
`count` / `zfill` / `ljust` / `rjust` / `center` / `removeprefix` /
`removesuffix` / `isdigit` / `isalpha` / `isspace` / `isupper` / `islower` /
`isalnum`。
其余内置名（`slice` / `delattr` / `vars` / `dir` 等）类型层面放行，
降级阶段报「not supported by the code generator yet」，不会再退化成调用
空指针。

### 跳转与异常

`break` / `continue` 只作用于字面最近的循环（`for ... else` 的 `else` 在被 `break` 跳过时执行）。

`try/except/else/finally` 支持：多个 `except`、`except (A, B)`、bare
`except:`、`as e` 和 `else`。异常按内置异常的继承关系匹配（`KeyError`
能被 `LookupError` / `Exception` 捕获）；`as e` 绑定一个**异常值对象**：
`e.args` 是参数元组，`print(e)` / `str(e)` 是消息，`e` 也能直接拼进
字符串（按消息处理）。`else` 只在 try 体正常结束时执行；handler 体里
再抛出的异常会继续向外展开。

异常构造器可以当值用：`e = ValueError("bad")`；`raise e` 抛这个实例，
`raise ValueError("bad")` 等价。自定义异常类支持继承内置异常或另一个
自定义异常，类体目前只允许 `pass` / 文档字符串：

```python
class MyError(ValueError):
    pass

raise MyError("bad")
```

`raise X from cause` 记录 `__cause__`（`e.__cause__` 是异常值或 None）；
`raise ... from None` 清空原因。traceback 仍未支持，`args` 目前只保存
消息。
`finally` 在正常、异常、`return`、`break`、`continue` 路径都会执行；
但 `finally` 体内**跳出 finally** 的 `return` / `break` / `continue`
会被后端拒绝（finally 里嵌套循环自己的跳转是允许的）。

`with` 的 `__exit__` 在 `return` / `break` / `continue` 和异常路径都会执行。

## match / case

支持结构化模式匹配的实用子集：

```python
class Point:
    x: int
    y: int
    __match_args__ = ("x", "y")

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

match value:
    case 0:
        ...
    case 1 | 2:
        ...
    case [a, b] if a == b:
        ...
    case [first, *rest]:
        ...
    case {"key": v, **rest}:
        ...
    case Point(0, y) as p:
        ...
    case Color.RED:
        ...
    case _:
        ...
```

- 字面量（数字/字符串/True/False/None/复数）、捕获名、`_`、序列（list/tuple，含 `*rest`）、
  OR（各分支绑定相同的名字）、映射（固定 key + `**rest`）、`as`、`if` 守卫。
- 类模式：位置子模式按类体里的 `__match_args__` 顺序映射到字段；关键字子模式
  （`Point(x=0)`）按字段名。匹配子类时沿用父类字段偏移。异常类（内置或自定义）
  写成 `case ValueError():`，按运行时异常类型判断。点分导入类（`shapes.Circle()`）
  只做运行期类检查（按模块限定名匹配 MRO），暂不支持字段子模式。
- 值模式：点分名字（`Color.RED`、`math.pi`）求值后用 `==` 与 subject 比较；
  支持 None/数值/字符串/复数。unknown 值上的字符串/复数模式走运行期安全比较。
- 内置类型模式（`case int():` 等）：静态类型已知时编译期判定；unknown 值上
  `object()` / `str()` / `complex()` 可安全判定，原始 int/float 无法区分，
  list/dict/tuple/set 的载体也没有标签，这些写法仍报错。
- 捕获名在分支结束后仍可见（按各分支类型 join；未命中的分支近似读到 0/None）；
  同一模式重复捕获要求值相等。
- 映射模式的常量 key 做静态重复检查。
- 没有穷尽性检查（Python 语义）；所有 case 不匹配时直接继续。

## 复数

`3j` / `1.5j` 字面量、`complex(re, im)` 构造、`+ - * /`、`abs()`、
`==` / `!=`、真值判断、`print` / f-string 已支持。复数是有 RC 头的堆对象；
没有 `**`、排序或与 int/float 的隐式存储转换（运算时允许提升）。

## 会被拒绝的写法

**语法层**（`check` 就报错）：

- `bytes` / `complex` 字面量，超出 int64 的整数
- `await` 在函数外、`break` 在循环外
- Python 2 遗留写法（`0x1L`、`lambda (a, b):`、`except X, e`）
- 类：方法缺 `self`、字段赋值类型不符、访问不存在的字段或方法（`self.xxx` 与外部基类的成员除外）

**代码生成层**（`check` 通过，`build` 报错）：类装饰器、`@classmethod`、自定义方法装饰器、通过 unknown 函数值的 `*` / `**` 展开、`*` 位于固定形参之前且不是定长 tuple、非异常类名当异常用、`finally` 体内跳出 finally 的控制流、跨模块继承、把类名当值用（`f = Point`）、函数内定义类、异步生成器表达式、嵌在其它表达式里的异步推导式。生成器的额外限制：`yield` / `yield from` 只能作为语句或赋值右值，不能出现在 `finally` / `except` 体内；协程的 `await` 同样。异步生成器有 `asend`/`athrow`/`__anext__`/`aclose`。自定义异常类目前只支持 `pass` 类体（字段/方法会被拒绝）。类体字段默认值里的 `lambda` 也在这一层拒绝。match 额外拒绝：导入类模式的字段子模式、unknown 值上的数值/list/dict/tuple/set 内置类型模式。

为什么这么分层：前端宽、后端严。`check` 的定位是「语法和类型有没有明显毛病」，可以对任意 Python 文件跑；`build` 才是「这门语言现在能不能编译它」。

## 类型检查查得出什么

| 类别 | 例子 |
|---|---|
| 未定义名字 | 引用不存在的名字 |
| 调用签名 | 参数个数、参数类型、位置限定参数、未知关键字、缺参、重复赋值 |
| 运算类型 | `int + str`、`list * str`、`str < int` |
| 下标 | 不支持下标的对象、字典键类型不符 |
| 赋值 | 标注变量赋错类型、容器元素类型不符 |
| 返回值 | 声明的返回类型与 `return` 表达式不符 |

**查不出的**：属性是否存在（`x.foo` 一律放行）、可调用性以外的协议、控制流可达性、变量是否一定被赋值。这些在静态上代价太高，或者需要更完整的对象模型，现阶段一律放行。
