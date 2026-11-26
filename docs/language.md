# 语言参考

PyAdamas 借 Python 的语法外壳，但**不是 CPython 的实现**：语义按静态类型的需要重新定义，不追求行为一致。这份文档说明「什么能写、写出来是什么语义、什么会被拒」。

关于和 CPython 的具体差异清单，见 [compatibility.md](compatibility.md)。

## 词法

| 项目 | 规则 |
|---|---|
| 换行 | `\r\n` / `\r` 在入口统一成 `\n`，所以 CRLF 文件的列号、字符串内容都不会错位 |
| 缩进 | 空格与制表符混用按 tabstop=8 展开；缩进栈不匹配报 `IndentationError` 风格的错 |
| 续行 | 括号内隐式续行；行尾 `\` 显式续行 |
| 注释 | `#` 到行尾；`# type: ignore` 一类注释不特殊处理 |
| 数字 | 十进制 / `0x` / `0o` / `0b`，支持 `_` 分隔；浮点支持 `.5`、`5.`、指数；`j` 后缀是复数字面量（0.74 起支持） |
| 字符串 | 单/双/三引号，前缀 `r` `b` `f` `rb` `br` `rf` `fr` 任意顺序组合；相邻字面量自动拼接（`str` / f-string / `bytes` 各自成组，`bytes` 不能与 `str` 混拼） |
| f-string | 按 PEP 701 解析：同引号嵌套、自文档化 `f"{x=}"`、spec 里再嵌 `{}` 都支持；format spec 支持 fill/align/sign/`0`/width/precision/类型 `d,b,o,x,X,f,e,E,g,G,%` 与 `!s/!r/!a` |

有几处词法层就拒绝，因为语义没有对应物：超出 int64 的整数、`0x1L` 这类 Python 2 写法。（`bytes` 字面量 0.79 起支持，运行时复用 `str` 的表示。）

`\N{NAME}` 是个特殊处理：词法层整体吞掉（否则 `{` 会被当成 f-string 的替换字段），但不查 Unicode 名字表，字符串里保留原文。

## 语法子集

### 语句

| 分类 | 支持 |
|---|---|
| 简单语句 | 表达式语句、赋值（含链式、多重解包）、增量赋值、标注赋值、`pass`、`return`、`break`、`continue`、`raise`、`assert`、`global`、`import` / `from ... import` |
| 复合语句 | `if` / `elif` / `else`、`while`（含 `else`）、`for` / `async for`（含 `else`）、`def` / `async def`（含默认参数、`*`、`/`、仅限关键字参数、`*args` / `**kwargs`、装饰器、生成器/协程/异步生成器）、`class`（多继承、C3 MRO、方法与字段）、`with` / `async with`（多个项、`as`）、`try` / `except` / `else` / `finally` |
| 能解析、不能生成 | `match` 之外的软关键字写法 |

`assert` 条件为假时抛出 `AssertionError`，第二个参数作为消息；没有消息时是 `assertion failed`。

### 表达式

支持：常量、名字、二元/一元运算、布尔短路、链式比较、条件表达式、调用（位置 + 关键字 + `*` / `**` 展开）、下标（读与写）、切片（读与赋值）、列表/元组/字典/集合字面量、列表/集合/字典推导式（含 `async for`）、生成器表达式（惰性）、`in` / `not in`、f-string、`lambda`、海象运算符、`range()`、`yield`、`yield from`、`await`、用户类的属性读与写、用户类的方法调用。

没有代码生成的：内置类型上的属性访问（除 `list.append` / `list.pop` 两个特例）。

## 类型系统

### 表示

| 类型 | 说明 |
|---|---|
| `int` | 64 位有符号，溢出回绕（不升格为任意精度） |
| `float` | IEEE 双精度 |
| `bool` | `int` 的子类型，机器上是 0/1 |
| `str` | 不可变 UTF-8 字符串；长度按码点缓存，`len` O(1) |
| `list[T]` | 动态数组，元素 8 字节槽 |
| `range[int]` | 惰性序列：只存边界，元素按需算（`range(10**9)` 不占内存） |
| `tuple[...]` | 定长元组；`tuple[T, ...]` 表示变长 |
| `None` | 单值类型 |
| `Optional[T]` / `T \| None` | 可空 |
| `func(...) -> T` | 函数值 |
| `generator[T]` | 生成器对象；`for` / `next` 产出 `T`，`send` 送入的值是 `unknown`（写成 `generator[Y, S, R]` 时按 `S` 检查），有 `throw(exc)` / `close()` |
| `coroutine[T]` | 协程对象；`await` / `asyncio.run` 得到 `T` |
| `task[T]` | `asyncio.create_task` 调度的任务；`await` 得到 `T`，有 `done()` / `cancel()` |
| `asyncgen[T]` | `async def` + `yield` 的异步生成器；`async for` 或 `asend` / `athrow` / `__anext__` / `aclose` |
| `future[T]` | `asyncio.Future`；可多等待，`set_result` 之后可 `await`/`result` |
| `类名` | 类实例；字段布局与 vtable 在编译期确定 |
| `结构体名` | `struct` 值类型；字段按 C 规则布局，赋值/传参/返回都复制 |
| `set[T]` / `dict[K, V]` | 堆上的哈希表；`set` 复用 `dict`（值恒 0），保留插入顺序 |
| `unknown` | 推断失败的占位，任何检查都放行 |

### 标注规则

- **函数签名必须标注**（参数与返回值），否则 `def` 里的参数类型是 `unknown`，调用处也不做检查。`unknown` 值按整数表示传递：`n * 1.1`、`n / 2` 这类与 `float` 混合的算术结果仍按 `float` 计算（未知一侧补 `int`→`double` 转换），但把浮点实参传给未标注参数仍然不支持。
- `unknown` 容器里的 `float` 元素有已知表示限制（含 `float` 的静态容器传给 `list[unknown]` /
  `dict[str, unknown]` / `unknown` 时会打印成 double 位模式整数）；现象、根因与规避见
  docs/compatibility.md §一「`list[unknown]` / `dict[str, unknown]` 元素里的 float」。
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

`str` / `list` / `tuple` 与 `int` 之间的 `*` 是序列重复（两侧顺序均可，
`"ab" * 3` / `3 * [0]`），负次数得到空序列；一次分配并复制，不反复拼接。


#### 整数宽度、溢出与位运算

`int` 是**有符号 64 位**（不是 Python 的任意精度）。这决定了三件事：

- `>>` 是**算术右移**：`-8 >> 1 == -4`（C 的有符号移位），不是逻辑右移；
- 溢出**回绕**而不是抛异常：`2**63 - 1 + 1` 得到 `INT64_MIN`；
- 移位次数按 64 取模（见上表）。

要做无符号位运算（CRC32、Varint、位打包这类），有两种写法，**都实测可用**：

**A. 用定宽无符号类型**（推荐，语义直接对应 C 的 `uint32_t`/`uint64_t`）：

```python
b: u32 = 4294967295
b = cast_u32(b + 1)      # 0（按 32 位回绕）
c: u64 = 18446744073709551615
c = cast_u64(c + 1)      # 0
d: u32 = 2147483648
print(d >> 1)            # 1073741824（u32 是**逻辑**右移）
print(cast_u32(-1))      # 4294967295
```

注意定宽类型与 `int` 混合运算会先提升成 `int`，写回定宽变量要显式 `cast_u32(...)`；
这是刻意的——不隐式截断，避免静默丢高位。

**B. 保持 `int`，用掩码约束宽度**（移植自 C 代码时最省事）：

```python
MASK32: int = 0xFFFFFFFF
crc: int = (crc >> 1) & MASK32   # 先算术右移再掩码，等价于逻辑右移
```

两者都对；A 的类型信息更全（宽度参与类型检查），B 的代码与 C 逐行对应。
回归用例：`tests/run/bitops_width.py`（u32/u64 回绕、无符号右移、`cast_u32(-1)`、i64 算术右移）。
### 比较

- `int` 与 `float` 可混比（先提升再比）。
- `str` 按字节序比较，UTF-8 的字节序与码点序一致，所以结果与 Python 相同。
- `==` 对 `str` 是逐字节比较；对 `list` / `tuple` / `dict` / `set` 是**逐元素比较**——元素类型在编译期已知，怎么比也随之确定（int 按整数、str 按字节、float 按 IEEE）。
  `dict` 的比较与顺序无关。`list` 和 `tuple` 是不同类型，彼此不相等（运行时表示
  相同，但静态类型分得清）。同构的嵌套容器能逐层比下去——编译器把「元素比较链」
  编成常量串交给运行时按链递归；元素类型不同（异构容器）时退回指针比较。
- 浮点比较里 `NaN` 参与的一切 `<` `<=` `>` `>=` `==` 都为假，`!=` 为真。

### 字符串

不可变，内部按 **UTF-8 字节**存储，对外**按码点**：`len("中文") == 2`、
索引 / 切片 / 迭代 / 方法 / 格式化宽度全部按码点。`len` 仍是 O(1)（码点数
缓存在对象头偏移 0，字面量在编译期算好）。字节长度用内置函数
`len_bytes(s)`（O(1)）；标准写法 `len(s.encode())` 也等价（O(n) 拷贝）。

索引支持负号，越界 panic。`+` 拼接分配新对象。切片 `s[a:b:c]` 按码点切；
`bytes` 切片按字节。非 ASCII 的大小写与字符类别仍只认 ASCII（见
[compatibility.md](compatibility.md)）。

### bytes

`bytes` 字面量（`b"..."` / `b'...'` / `rb"..."` / `br"..."`）和 `str` 共用同一个
运行期结构体（`PyAdamasStr`），但**长度/索引/切片/方法全部按字节**，由静态类型
在降级期选择实现。`len` / 索引 / 切片 / `+` / `in` / 比较都按字节工作；
`encode()` / `decode()` 是真正的转换（全 ASCII 时零复制，非 ASCII 拷贝一次）。
`b"\xff"` 与 `b"\377"` 都是原始字节；`str` 的 `\xNN` 则是 U+00NN 编成 UTF-8。

相邻 `bytes` 字面量可以隐式拼接（`b"a" b"b"` → `b"ab"`），但不能和 `str` 混拼——
`b"a" "b"` 报 `cannot mix bytes and nonbytes literals`（CPython 同款消息）。

支持的方法：`upper` / `lower` / `strip` / `lstrip` / `rstrip` / `replace`（可带
第三个参数限制替换次数）/ `find` / `rfind` / `count` / `split` / `startswith` /
`endswith` / `join`。字节层面和 `str` 完全一致，只是结果标成 `bytes`——
`split` 返回 `list[bytes]`，`join` 要一个 `list[bytes]`。

`for c in b"..."` 逐字节迭代，元素是 0..255 的 `int`（内部先转 `list[int]`），
列表/集合/字典推导式同样适用；`list()` / `tuple()` / `sum()` / `min()` /
`max()` / `any()` / `all()` / `sorted()` / `enumerate()` 也接受 `bytes`，
元素按 int 处理。也支持 printf 风格的 `%` 格式化：

```python
b"%d-%s" % (7, b"x")     # b'7-x'
b"%05d" % 42             # b'00042'
```

`%` 支持 `d/i/u/x/X/o/f/F/e/E/g/G/s/r/c`、`%%`、`-`/`0`/`+`/空格标志、
宽度和精度；不支持 `#` 前缀、映射 key（`%(name)s`）和把 list 当多参数元组。
`bytes` 标注（`x: bytes`）和跨模块函数签名都保留 `bytes` 类型，不再降级成
`unknown`。

`bytes(...)` 可以把 `list[int]` 转成字节串（元素必须在 0..255，越界抛
`ValueError`），`bytes(n)` 生成 n 个零字节，`bytes()` 是空字节串；
传 `bytes` 原样返回。逆向往 `list[int]` 用 `list(b)`。

### 列表

`list[T]` 是动态数组，`append` 均摊 O(1)，索引支持负数，越界 panic。列表字面量的元素类型由所有元素的上界推出（`[1, 2.5]` 是 `list[float]`，整数元素在构造时就被转成 double）。

`l[i] = v` 支持；切片读 `l[a:b:c]` 返回新列表并接管元素引用；
切片赋值 `l[i:j] = ...` 支持（细节见下文「切片」）。

### 切片

`str` / `list` / `tuple` 支持读切片 `x[a:b:c]`：

- 缺省边界、负数边界、正负步长按 Python 语义在运行时归一化；
- 结果总是新对象（str 新字节串，list/tuple 新列表）；list 切片按源
  元素的堆类别 retain 元素；
- `step == 0` 运行时 panic；越界边界被夹到有效范围；
- **切片赋值** `l[a:b:c] = value` 支持 list：`step == 1` 时可以改变长度
  （替换 / 插入 / 删除），扩展切片要求右侧长度与切片长度一致，否则
  panic；右侧可以是 list/tuple，也可以是 str（按码点拆成字符列表）；
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

推导式有独立的变量作用域：循环变量不会覆盖同名的外层变量。

生成器表达式 `(elt for x in xs)` 是**惰性**的：脱糖阶段把它展开成一个
合成生成器函数加一次调用，最外层迭代源在创建时求值（Python 语义），
其余部分等 `next()` 才执行。`sum` / `min` / `max` / `any` / `all` 直接
流式消费它（`any`/`all` 命中即停），`list` / `tuple` / `set` / `sorted` /
`enumerate` / `zip` / `str.join` 会把它抽干成列表，`for` 循环逐元素拉取。
生成器表达式只能消费一次。

源是 `range(...)` 时也是惰性的：脱糖把边界值传进合成生成器函数，用计数
循环逐个产出，所以 `(x for x in range(10**9))` 既不建中间列表，第一次
`next()` 就出 0。`range` 本身也是惰性对象（类型 `range[int]`，只存边界），
`range(10**9)` 只占几十字节，`len` / 下标 / `in` / 推导式都不物化；
`list(r)` / `tuple(r)` 这类需要真列表的操作才实体化，详见
[compatibility.md](compatibility.md#range-是惰性对象)。

```python
total = sum(x * x for x in range(1000))     # 不建中间列表
first = next(x for x in xs if x > 0)
```

限制：类体里的生成器表达式仍按 list 立即构造（类体只支持简单值）；
推导式内部不能写引用推导式循环变量的生成器表达式（合成函数被提到语句
之前，那时循环变量还不存在）。异步生成器表达式（`(x async for ...)`）
会脱糖成异步生成器函数，可以出现在任意表达式位置。

含 `async for` 的推导式只能在
`async def` 里使用，可以混用同步/异步子句；它只能作为整条语句、赋值
或 `return` 的根表达式（挂起会清空当前表达式的临时值），元素表达式
里也不能含 `await`。推导式里出现 `lambda` 时，脱糖阶段会把它改写成
「生成器表达式 + `list()` / `set()` / `dict()`」：循环变量成为合成生成器
函数的局部变量，闭包捕获走常规路径，语义和 CPython 一致（所有 lambda
共享循环结束时的值）。不含 lambda 的推导式仍按原来的就地展开编译。
推导式内部的海象运算符写回外层仍不支持。

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
`@classmethod` 也支持（entry 模块内 `cls` 静态绑定所属类；非入口模块和
跨模块方法用运行期类对象，见下面「类属性与 `@classmethod`」）；
自定义方法装饰器在类创建点按从下到上应用，结果存成类属性，实例调用
`obj.m(...)` 会把 `obj` 作为第一个实参调用包装函数。类接收者
`C.m(obj, ...)` 也按包装函数的未绑定签名显式接收 `self`；
`super().m(...)` 会在声明该方法的类上取包装函数并自动补 `self`。

**方法值**：`obj.m` / `C.m` 不加括号时得到绑定接收者的函数值，可以存进
变量、放进容器、当实参传出去，之后按普通函数值调用。实例上的普通方法
绑定该实例（调用时不再传 `self`），运行期仍按对象 vtable 分派；静态
方法/类方法按原语义绑定；装饰方法绑定实例并调用类属性里的包装函数。
类接收者上只支持取静态方法/类方法的值（普通方法请从实例取）；跨模块 /
动态 classmethod 的方法值暂不支持。optional 接收者上取方法值会先做 None 检查。
装饰器必须返回签名已知的函数；走 `dunder_call` 的那批 dunder 都可以装饰
（算术/比较/`__str__`/`__len__`/`__hash__`/`__iter__`/`__contains__`），
构造（`__init__`）、`with` 协议（`__enter__`/`__exit__`）、
`async with`（`__aenter__`/`__aexit__`）和实例调用（`__call__`）也一样：
装饰后的实现不填 vtable 槽，调用点按名字从类属性取包装函数并显式补上
实例。`as` 目标/表达式结果的类型取包装函数声明的返回值。包装函数必须用
与目标一致的固定形参（不能靠 `*args` 转发）。类装饰器在类定义点拿到
运行期类对象，返回值继续作为该类名的类对象；返回替换类时构造仍走原类。

### enum / abc：编译期类语义

`enum` 与 `abc` 的标准库子集由**编译器在 class 语句上拦截**实现（不是运行期元类）：

| 写法 | 语义 |
|---|---|
| `class Color(Enum): RED = 1` | 成员对象：`Color.RED.name` / `.value`；`Color(1)` 按值反查；`for c in Color` / `list(Color)`；成员是单例（`Color.RED is Color(1)`） |
| `@abstractmethod` 方法 | 编译期标记抽象方法；含未实现抽象方法的类**实例化时报错**（`cannot instantiate abstract class ...`），子类实现后可用 |

```python
from abc import ABC, abstractmethod
from enum import Enum

class Color(Enum):
    RED = 1
    GREEN = 2

class Shape(ABC):
    @abstractmethod
    def area(self) -> int:
        return 0

class Square(Shape):
    def area(self) -> int:
        return 1

print(Color.RED.name, Color.RED.value)   # RED 1
print(Color(2).name)                     # GREEN
print(len(list(Color)))                  # 2
Square()                                 # OK
```

限制：`class Color(Enum)` 的别名（重复值）、`auto()`、混合 int/str 成员值、
`__members__`、自定义 `__init__`/`__new__`、`metaclass=` 都不支持；
`IntEnum`/`IntFlag`/`StrEnum` 保持 int/str 子类语义，`Flag` 保持类属性常量语义；
`abc` 的 `ABCMeta` 不能当 metaclass 用。

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

支持 `for` / `next()` / `.send(v)` / `.throw(exc)` / `.close()` / `.__next__()`。
`send` 的值成为 `x = yield v` 里 `x` 的值（普通 `next` 是 None）；`throw`
把异常值注入当前挂起点，生成器自己的 `try` / `except` 能接住它。生成器函数的
`return` 值会被外层 `yield from` 取走；直接用 `for` / `next()` 迭代时
丢弃（同 CPython）。

`x = yield v` 里 `x` 的静态类型默认是 unknown。函数返回类型写成
`Generator[Y, S, R]`（`typing` 不用导入，裸名即可）后：`yield` 表达式的类型是
`S`，`.send(v)` 按 `S` 检查实参并返回 `Y`；`R` 目前只做记录，
`StopIteration.value` 不参与类型检查。只写 `Generator[Y]` 时 `send` 不检查。
`AsyncGenerator` 仍只接受一个参数。

```python
def g():
    try:
        x = yield 1
    except ValueError as e:
        print("caught", e)
    yield 2

it = g()
next(it)                 # 1
it.throw(ValueError("x"))  # caught x -> 2
```

`throw` 注入的异常若没人接住，生成器随之结束，异常抛给调用方；对已经结束的
生成器 `throw` 时异常直接抛给调用方，对从未启动的生成器则从函数入口抛出
（函数体不执行）。实参可以是异常实例，也可以是异常类：`throw(ValueError)`
等价于 `throw(ValueError())`；`athrow()` 同理。

**`yield from`**：把子迭代器的值逐个转 yield 出去；委托生成器时
`send` 的值会转发给子生成器，子生成器结束（或出错）后继续执行外层
代码。可委托生成器、list / tuple / str / dict / set；其他静态类型
（unknown）按生成器协议尝试，失败抛 `TypeError`。外层生成器被
`close()` 时，子生成器的引用被释放，它的 `finally` 照常执行。表达式
的值：委托生成器时是子生成器的 `return` 值，委托序列时是 None。

`throw()` 也按同样路径转发：外层停在委托挂起点时，注入的异常交给子生成器的
`throw`；子生成器接住后继续 yield 的值由外层转 yield，子生成器因此结束则
`yield from` 取它的 `return` 值，子生成器没接住则异常在 `yield from` 处向外层
传播（外层自己的 `try` 能抓住）。委托序列时不转发，异常直接在外层挂起点抛出。

```python
def inner():
    yield 1
    yield 2

def outer():
    yield from inner()
    yield from [3, 4]
```

**跨 yield 清理**：`yield` 可以出现在 `try` / `with` / `except` / `finally`
体内；挂起时活跃的 handler 记录被摘下，恢复时按原顺序重挂并重新建立 setjmp。
`except` 体里的挂起还要求异常上下文跟着走：resume 入口把调用方的
`current_exc` / 保存节点换进来，挂起时换回去，所以生成器在 except 体
里挂起期间，调用方自己的 except 状态不受影响，恢复后裸 `raise` 仍然
重抛生成器当前处理的异常。`close()`、引用计数归零与 GC 前的 finalize
会按挂起点快照执行 `finally` 和 `with.__exit__(None, None, None)`。
异常从生成器逃出时，生成代码的 landing 路径负责展开清理，帧被标记
aborted，避免销毁时重复执行。

`finally` 体内挂起时，正常路径与异常 landing 汇聚到同一份 finalbody，
所以体内每个 `yield` 只有一套恢复状态；异常路径在进入 finalbody 前把
待重抛异常转成异常值存进帧槽，体内再 `raise` / 嵌套 `try/except` 不会
覆盖它，finalbody 结束后照原样重抛。`close()` 重放 finally 体时若再次
遇到挂起点，按 CPython 报 `RuntimeError: generator ignored GeneratorExit`
（协程为 `coroutine ignored GeneratorExit`）。

限制：`yield` / `yield from` / `await` 可以出现在任意子表达式里
（`f(g(), yield x)`、`10 + (yield x)` 都行）：降级后按 resume 函数的
vreg 活跃性找出跨挂起的值，挂起前 retain 一份写进生成器堆帧槽，恢复
后再装回。`finally` 体内的 `return` / `break` / `continue` 可以跳出
（含生成器 `close()` 重放路径）。`throw()` / `athrow()` 接受异常实例，
也接受异常类（等价于无消息实例）。生成器 / 协程 / 异步生成器的元素与
结果类型跨模块接口保留（模块格式 7 起 `Generator` 还带 `send` 值与 `return`
值的类型）。

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
  task` 取结果，完成后可重复 await；同一个 Task 可以被多个协程同时等待，
  每个等待者都拿到结果/异常；`task.done()` 判断是否结束；
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
  队列的 put 会阻塞，get 腾出位置后按 FIFO 放行；`asyncio.Queue[float]`
  这类标注会把元素类型带进构造，float 元素按裸值存取（不再要求引用
  计数）；未标注的 `asyncio.Queue()` 元素按 unknown 处理（`get` 的
  await 结果不能用字符串方法等需要具体类型的操作）。
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
Queue, wait_for, timeout, TaskGroup, Semaphore`（同一模块内保留调用点
特化；跨模块导出的 `run`/`wait_for` 结果类型仍可能退化成 unknown）。
async 方法、`with` 里的 `await`、跨 `await` 的
`finally` 都可以用；异常跨 `await` 通过在恢复时注入异常传播，
`try/except` 能正常捕获。`await` 同样可以出现在任意子表达式、
`except` 体和 `finally` 体内（finalbody 只编译一份，异常路径的待重抛
异常存在帧槽里跨挂起保留）。

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

生成器内部可以混用 `await` / `yield`；`await` 的结果（包括 `str` / `list`
这类堆值）会正确送回生成器帧槽，内部 awaitable 抛出的异常也走注入分发，
可以被生成器自己的 `try/except` 接住。`return`、`break` 中断迭代后，
生成器对象被回收时同步执行 `finally`（`finally` 里的 `await` 同样按挂起点
处理：正常路径与异常 landing 汇聚到同一份 finalbody）。

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
无效 fd 以 `OSError` 结束等待。这是 PyAdamas 自己的低级接口（CPython
用 `loop.add_reader`），没有配套的 fd 读写函数。

限制：异步推导式只能作为语句、赋值或 `return` 的根表达式；`async for`
支持 `AsyncGenerator` 以及定义了 `__aiter__` 的类实例（`__aiter__` 返回
`AsyncGenerator`，或返回自身/另一个定义了 `async __anext__` 的对象）；
fd 读写和线程安全的事件循环未支持；Future 回调是同步调用（CPython 用
`call_soon` 排队），`gather` 的实参个数在源码里写死；结果列表的元素类型
按实参结果的上界推出，所有权类别随静态类型带入运行时；Task 没有任务名字。

### 内存与生命周期

堆值（字符串、列表、元组、字典、集合、类实例、闭包）用**引用计数**自动
回收，语言层看不到 `free` 或析构函数。

- 赋值、传参、返回、字段/列表写入都会给堆值增加一个引用；变量覆盖、
  离开作用域时释放旧引用。
- 表达式临时值由运行时临时值栈兜底，语句、循环、函数边界统一清理。
  所以 `s = s + "x"` 在长循环里不会无限增长。
- **循环引用由保守 GC 回收**：`a.next = a`、相互递归闭包形成的环会在
  分配量超过阈值（上次收集后的存活堆大小，下限 2MB）时被标记-清扫；
  程序退出时也会清空整个堆。字符串和没有堆字段的实例不可能成环，在
  完全标注（没有 `unknown`、不涉及 asyncio/生成器）的程序里不参与
  循环收集，只由引用计数管理。
- 静态类型看不见的值（`unknown`、`list[unknown]`、混合类型元组）不参与
  精确计数；GC 会保守扫描、把不可达对象回收，但「指向仍有其它根的对象」
  的计数可能虚高，要到后续轮次才回收。这是「宁可延迟回收，不可误收」的
  取舍。
- 没有 `__del__`、`weakref` 或手动释放；对象什么时候真正释放不可观察。
  GC 是 stop-the-world 的分代收集（minor trial-deletion + major 标记-清扫），
  根由帧记录登记，没有增量/并发。

### 字典与集合

`dict[K, V]` 用「密集 entries（插入顺序）+ 4 字节开放寻址索引表」，
`set[T]` 复用同一张表（值恒 0）。两者都保留**插入顺序**；`len()` 和
真值判断都是 O(1)。`==` 是逐元素比较：dict 按「键值对在对方里找得到」
（与顺序无关），set 按「每个元素都在对方里」。支持：

- 字面量 `{"a": 1}`、`{1, 2, 3}`（`{}` 是空字典；空集合没有字面量）；
- 下标读 `d[k]`（缺失时 panic `KeyError: key not found`）与写 `d[k] = v`；
- `len(d)`、`if d:`、`for k in d`、`for x in s`；
- `in` / `not in`：dict/set 按 key，list/tuple 线性扫，str 子串；
- 类型标注 `dict[str, int]`、`set[str]`。

key 的哈希/判等按静态类型选：int/bool 用机器字、str 用字节（FNV-1a +
逐字节比较）、float 用 IEEE 值（`0.0` 与 `-0.0` 同键）；类实例实现了
`__hash__` 和 `__eq__` 时走 vtable，否则按指针；其他类型按机器字比较。
支持的方法：

- `d.get(k)` / `d.get(k, default)`：缺失时返回默认值；没有默认值时返回
  `None`（静态返回类型是 `Optional[值类型]`，`get(k, default)` 才是值类型）。
  注意 PyAdamas 里 `None` 就是 `0`——值为 `0` 的项和缺失分不出来；
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

删除用墓碑：链表摘链，entry 原地变墓碑挂进空闲链，索引槽置 -1，插入时
优先复用墓碑，单次删除摊还 O(1)。`order` 是链表的下标快照，删除后失效，
下一次按序访问时重建；顺序遍历本身是 O(n)，不增加摊还代价。
扩容只重建 4 字节索引表，entries 本身不搬家；顺序遍历直接扫 `order`。

空字典字面量自己推不出 key 类型。`d: dict[str, int] = {}` 这种标注赋值
会按标注类型构造，动态字符串 key 正常；没有标注（或先声明、之后再
`d = {}`）时空字典的 key 类型仍是 `unknown`，不同对象但内容相同的动态
字符串 key 可能按机器字比较不相等。写了标注或由字面量推出 `str` key
时没有这个问题。`dict`/`set` 的 `==` 是逐元素比较（见上文「比较」），
同构嵌套容器可以逐层比下去。


#### `__del__`（0.539）

定义了 `__del__` 的类，在**引用计数归零、对象释放之前**被调用一次（经 vtable，`self` 传入）。

```python
class V:
    def __init__(self, x: int) -> None:
        self.x = x
    def __del__(self) -> None:
        print("destroyed", self.x)

v: V = V(1)
print("before")      # before → destroyed 1
```

保证：

- RC 归零的释放路径会调用；`__del__` 里读自身字段、`del self.x` 都合法；
- `__del__` 抛异常**不会终止程序**：往 stderr 打一行 `__del__ raised <类型>: <消息>` 然后丢弃，继续释放；
- 同一个对象最多调一次。

不保证（与 CPython 的差异）：

- **GC 环回收不调用**：互相引用的对象由循环收集器释放时，`__del__` **不会**执行
  （CPython 3.4+ 的 PEP 442 会执行）；需要确定性释放就不要让对象进环；
- **`__del__` 里触发另一个对象的析构时，那一次的 `__del__` 会被跳过**（防重入的保护），
  这是为了避免析构期间的重入释放；
- 释放时机与 RC 归零时机一致，不保证在解释器退出前完成。
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
| 类体 | 方法定义、带标注的字段声明、`pass`、文档字符串；普通赋值是**类属性**；`if`/`for`/`print` 等其它语句在类定义点执行（按模块全局处理） |
| 类属性 | `x = 1` 存在模块的隐藏全局里，所有实例共享；`C.x` / `cls.x` / `self.x` 都能读；子类赋值走写时复制 |
| 字段 | 标注可选：`x: int` 定死偏移；`self.x = ...` 也能长出字段；同名类属性在构造时拷进字段 |
| 字段初值 | `n: int = 0` 在**每次构造时**求值，不是 CPython 的类属性 |
| 构造 | `Point(1, 2)` = 分配 + 初始化带初值的字段 + 调 `__init__` |
| 继承 | 多继承（C3 MRO）；主基类的字段/方法槽位前缀保持不变 |
| 覆盖 | 子类同名方法占用父类同一个槽，所以父类类型的变量能调到子类实现 |
| `self` | 普通参数，名字不限；省略标注时类型就是所属类 |
| 属性读 | `p.x` → `movq 8(%rax), %rax` |
| 方法调用 | `p.m(...)` → 从对象头取 vtable，按常量槽号取地址后间接调用 |
| 打印 | `print(p)` 有 `__str__` 就调它，否则输出 `<Point object>` |

**协议方法**（vtable 分发，子类覆盖生效；静态类型的 MRO 里没有实现时退回默认行为）：

| 方法 | 要求 | 触发点 |
|---|---|---|
| `__str__(self) -> str` | 普通方法，只有 self | `print`、`str()`、f-string 默认转换；没有时回退 `__repr__` |
| `__repr__(self) -> str` | 普通方法，只有 self | `repr()`、`!r`、list/tuple 等容器内部元素；没有时回退 `<类名 object>` |
| `__eq__(self, other) -> bool` | `other` 可省略标注（unknown）或标成类类型 | `==`；两边都没有实现时保持指针比较 |
| `__ne__(self, other) -> bool` | 同上 | `!=`；没定义时用 `__eq__` 取反 |
| `__hash__(self) -> int` | 普通方法，只有 self，返回 int | `hash()`；类作 dict/set 键时和 `__eq__` 一起走 vtable |
| `__len__(self) -> int` | 普通方法，只有 self | `len()`、真值（`if`/`while`/`not`/`and`/`or`/三元/`bool()`/`assert`/推导式过滤）；真值先看 `__bool__`，没有才看 `__len__`；没有 `__len__` 的类调 `len()` 直接编译报错 |
| `__bool__(self) -> bool` | 普通方法，只有 self，返回 bool | 真值：`bool()`、`if`/`while`、`and`/`or`/`not`、`assert`、推导式过滤；没有时退回 `__len__` 是否非零 |
| `__format__(self, spec) -> str` | 普通方法，self + 格式说明（str），返回 str | `format(obj, spec)` 和 f-string 的普通格式说明（`f"{obj:>5}"`），优先于 `__str__` |
| `__reversed__(self) -> ...` | 普通方法，只有 self；返回可迭代对象 | `reversed(obj)`：调用后物化成 list，元素类型取返回值的元素 |
| `__contains__(self, other) -> bool` | `other` 可省略标注或标成类类型，返回 bool | `x in obj` / `not in`；没有可用实现时编译报错 |
| `__iter__(self) -> ...` | 普通方法，只有 self；返回类型不限 | `for x in obj`：先调 `__iter__`，再按返回值（list/tuple/str/generator）走现有迭代。返回 list 也接受（CPython 要求返回迭代器，这里有意放宽） |
| `__add__` / `__sub__` / `__mul__` / `__truediv__` / `__floordiv__` / `__mod__` / `__pow__(self, other)` | 普通方法，两个参数；参数和返回类型任意 | 二元算术 |
| `__radd__` 等反射版 | 同上 | 右操作数是类、左边不是类时（`3 * obj`） |
| `__iadd__` 等原地版 | 同上 | `obj += x` 优先 `__iadd__`，没有再走 `__add__`，最后才是右操作数反射版 |
| `__lt__` / `__le__` / `__gt__` / `__ge__(self, other) -> bool` | 两个参数；`other` 必须是 unknown 或类类型；返回 bool | `<` / `<=` / `>` / `>=`；左侧没有时用右操作数的镜像版（`a < b` 可调 `b.__gt__(a)`） |

二参数协议的实参规则：方法的 `other` 标成具体类时，调用点只在实参静态类型可赋值给它（或为 unknown）时走协议，不匹配就换另一侧或按下面的规则报错，不会把错误类型的值送进方法体。容器打印用**元素 repr**（和字符串元素一致）；类实例的默认文本是 `<类名 object>`，没有 CPython 的模块名和地址。f-string 带格式说明时，类实例有 `__format__` 就调它；没有才按 `__str__` 转成字符串再套用说明。

**类作 dict/set 键**：`__hash__` 和 `__eq__` 都有实现时，哈希和相等判断走 vtable（不同实例内容相等即可命中同一个键）。两个都缺时退回机器字（指针）哈希 + 指针比较，等价于 CPython 里没定义 `__eq__` 的类的身份语义。只定义 `__eq__` 而没有 `__hash__` 的类在 CPython 里不可哈希；这里 dict/set 仍按身份处理，而 `hash()` 会在编译期报错。

**错误的运算符组合不再静默算指针**：类实例参与算术但两侧都没有匹配的 dunder（含反射版）时编译报错 `unsupported operand type(s)`；大小比较（`<`/`<=`/`>`/`>=`）同理，缺少比较 dunder 直接报 `not orderable`。`==`/`!=` 例外：没有 `__eq__` 时按指针比较（和 CPython 的默认身份比较一致）。`in` 目前要求类实现 `__contains__`，不做 CPython 的 `__iter__` 回退。

字段初值在构造点求值，比 CPython 的「类属性 + 实例回退」更简单也更安全：`items: list[int] = []` 每个实例拿到的是**独立**的空列表，不会出现所有实例共享一个 list 的经典 bug。

**不写标注的字段**。Python 的实例属性本来就是 `__init__` 里长出来的，强制先写标注会把标准库挡在门外。所以 `self.x = ...` 里 `x` 没声明过时，编译器按**首次赋值顺序**把它补进布局，类型取右侧表达式的推断结果：

```python
class Connection:
    def __init__(self, host: str) -> None:
        self.host = host      # 补出字段 host: str
        self.sock = None       # None 太弱，字段类型记 unknown
```

初值是 `None` 的字段类型记 unknown 而不是 `None`——否则后面每一处 `self.sock.send(...)` 都会被判死。推断出来的字段赋值时不检查类型（`self.x = 1` 之后再 `self.x = "s"` 只是把类型放宽成 unknown），只有写了标注的字段才严格。

**看不见的父类**。基类是非 PyAdamas 模块的外部类型（`unittest.TestCase`、
`OrderedDict`）时，它的字段和方法我们看不到，这个类就是「开放」的：查不到
`self.xxx` / `self.method()` 时按 unknown 放过，不报错。标准库里每个测试类都
继承 `TestCase` 再调 `self.assertEqual`，不放过就是大面积误杀。

**多继承**。`class C(A, B)` 用标准 C3 线性化算 MRO，方法查找、vtable 填充和
`super()` 都沿这条链。第一个基类仍提供字段布局前缀；附加基类只能复用
主基类布局里已有的字段（共享祖先的菱形可以，新增字段不行）。
方法槽在模块内按名字全局分配，同名覆盖在任意 MRO 路径上写同一个槽。
零参 `super().m(...)` 支持协作式调用：运行期按实例真实 MRO 找下一实现；
显式 `super(C, self)` 尚未支持。导入的 PyAdamas 模块类既可以作为主基类，
也可以作为多继承的附加基类：字段布局、方法覆盖、`super()` 和 `isinstance`
都按跨模块接口工作；同名方法在不同模块占不同槽时会填别名槽，不同名抢同一
槽在编译期报错。导入基类的非字面量字段默认值不支持。`super()` 调导入基类的方法在 **check 阶段从宽**：定义类的 MRO 里有导入基类（或开放类）时，那个方法可能定义在另一份接口里，check 看不见就当作 unknown 放过，不报「找不到方法」——只有纯本地继承链都找不到才判错（build 仍沿运行期 MRO 分派，所以能跑对）。

**类属性与 `@classmethod`**。类体里的普通赋值是类属性，存进模块里的一个
隐藏全局，所有实例共享同一份：

```python
class Config:
    name = "base"
    tags = []                 # 可变：父子类看到的是同一个 list

    @classmethod
    def describe(cls):
        return cls.name       # 本地静态路径绑定调用类；跨模块 cls 是运行期类对象
```

`C.x` / `cls.x` / `self.x` 都能读到类属性；实例字段同名时字段优先。
子类给继承来的类属性赋值（`Sub.x = v` 或类方法里的 `cls.x = v`）走
**写时复制**：子类从此有自己的一份，基类那份不受影响，赋值之前读到的
仍是基类的值。本地继承的 `@classmethod` 会在子类里重新特化一份；跨模块
继承的 classmethod 用运行期类对象（类对象带 vtable 与构造器），两种路径
都让 `Sub.make()` 里的 `cls(...)` 构造 `Sub`。跨模块 classmethod 里的
`cls.x` 类属性读写仍按声明类解析，且子类 `__init__` 签名需要与基类兼容。

`self.x` 读类属性按运行期类号派发：静态类型是基类、值是子类时读的是
子类覆盖的那份（只有确实有子类覆盖时才生成比较链，否则退化成一次直接读）。

不支持：给实例上的类属性赋值（`obj.x = v` 会要求新建实例属性，而对象
布局是编译期定死的）、`super(C, self)`。`__init__` 之外的构造协议
（`__new__`）也没有；容器/迭代协议个别面（如 `__del__`、弱引用）未建模。

类本身可以当值用：`f = Point`、传参、`f.__name__`、`print(f)`、
`isinstance(x, f)` 都支持，类名在运行期是一个类对象单例；导入类也可
（`C = mod.Point`，`__name__` 是最后一段）。类装饰器在类定义点执行，
返回值继续作为该类名的类对象使用。限制：类装饰器必须返回类；返回
替换类时后续 `C(...)` 仍按原类构造；异常类不支持类装饰器；静态类型
未知的类值不能当构造器调用。

没有 `__init__` 时只能零参构造——基类在外部的类除外，`__init__` 可能在那边。`__init__` 的默认参数和关键字参数与普通函数规则相同。

定义 `__call__` 的类，其**实例**可以像函数一样调用：`obj(1, 2)` 走
`__call__` 的调用签名（参数里不含 self），返回值就是 `__call__` 的返回
值，链式写法 `Maker(10)(1, 1)` 也支持。类名本身仍是构造；被局部变量遮住
的类名按实例处理。`__call__` 可以套自定义装饰器。

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
closefd=True)` 返回文件对象，位置/关键字参数按 CPython 的名字绑定。文本模式
默认 UTF-8。文件对象目前只支持 `str` 读写——`write()` 传 `bytes` 会被拒
（`bytes` 类型 0.79 才有，文件接口还没接）：
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

**流式读取（管道 / tty，0.447）**：非普通文件（管道、终端、socket）上的
文本预读改成一次 `read(2)`「有多少拿多少」，不再像 glibc 的 `fread` 那样
为凑满 8KB 请求量一直等到 EOF：

- `sys.stdin.readline()` / `for line in sys.stdin:` 在写端还开着时就返回
  已到达的整行（写端不关不再阻塞）；EOF 时 `readline()` 返回空串；
- `read(n)` 精确读 n 个**字节**（UTF-8 按字节计，和 `Content-Length` 一致），
  数据分块到达时继续等，凑满 n 或 EOF 才返回；`read()` 一直读到 EOF；
- 普通文件重定向（`prog < file`）行为不变（`fread` 大块读）；
- `input()` 也走同一条 `sys.stdin` 路径（不再单独用 stdio 的 `fgetc`），
  EOF 抛 `EOFError`。

LSP / REPL 这类「一问一答」的协议在 `sys.stdout` 上写完一帧后要
`sys.stdout.flush()`（`write` 本身不自动 flush）。需要逐字节读二进制帧时用
`open(0, "rb")`（返回二进制 `file`，`read(n)` 精确按字节、`readline()` 保留
`\r\n`），再交给 `json.parse` 解析帧体；`subprocess.Popen` 的 `.stdout` /
`.stderr` 也是这种二进制 `file` 对象。
打开失败抛 `OSError`，关闭后读写抛 `ValueError`。

`file` 也可以作为形参/字段标注；跨模块接口里它按 unknown 编码（调用点不再
检查实参类型），但模块内的方法调用仍按 file 正常生成。

### print

`print` 不是函数，是降级期展开的一串输出调用：

- 支持 `sep=` / `end=`（任意字符串表达式）/ `file=`（文件对象）/ `flush=`。
- 值按静态类型输出：list/tuple 逐元素递归打印（`[1, 2]`，容器里的字符串
  按 repr 带引号）；dict/set 也打印真实内容（`{'a': 1}` / `{1, 2}`），键和值
  同样按 repr 语义。元素类型未知时（异构容器）仍输出 `<dict object>` /
  `<set object>` 占位——运行时没有类型标签，分不清 float 和 int，宁可不给
  也不给错值。空集合打印成 `set()`（`{}` 在 Python 里是空字典）。
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
  等长 `list[float]`；`frexp`/`modf` 返回二元组。`fsum` 只接受
  `list[float]`（`list[int]` 在 build 阶段报错），使用 Shewchuk 部分和，
  与 CPython 一致。
- 内置 `random`：`Random(seed)` 对象和模块级 `random()` / `seed()` /
  `randint(a, b)` / `uniform(a, b)` / `choice(seq)` / `shuffle(list)`，
  实例方法同名。随机引擎按 CPython 的 MT19937 实现，同一个整数种子产生
  与 CPython **完全相同**的序列；省略 `seed` 时用时间 + pid 播种。
  `choice` 接受 list/tuple/str 并返回元素类型，`shuffle` 只接受 list。
- 内置 `os`：`mkdir` / `rmdir` / `remove` / `rename` / `getcwd` /
  `cpu_count`。内置 `os.path`：`join`（变长参数，posix 语义）、
  `exists` / `isdir` / `isfile`。子模块三种写法都可以：`import os.path as osp`、
  `from os.path import join, exists`、以及 0.484 起的 `import os` 后直接
  `os.path.join(...)`（属性链会展开成完整模块名，按**内置子模块**的声明式签名降级）。
  注意这只覆盖内置子模块；用户包的子模块链（`import pkg` 之后的 `pkg.sub.f()`）仍不支持，
  要写 `import pkg.sub as s` 或 `from pkg.sub import f`。
- 内置 `subprocess`：`run(args, capture_output=False, text=False,
  timeout=-1.0, check=False, shell=False, cwd="")` 返回 `CompletedProcess`
  对象，属性 `.returncode` / `.stdout` / `.stderr`。`args` 可以是
  `list[str]`，也可以是一个字符串（配 `shell=True`）。`timeout` 为负表示
  不超时；超时抛 `TimeoutExpired`，`check=True` 且退出码非零抛
  `CalledProcessError`。这两个异常按 PyAdamas 的内置异常名匹配，直接写
  `except TimeoutExpired`；`from subprocess import TimeoutExpired` 暂不
  支持。
- 内置 `subprocess.Popen(args, shell=False, cwd="", stdin=None,
  stdout=None, stderr=None)`：流参数取 `subprocess.PIPE` / `DEVNULL` /
  `STDOUT`（`STDOUT` 只对 `stderr` 有意义）。方法 `wait(timeout=…)` /
  `communicate(input=…)` / `kill()` / `terminate()` / `send_signal(n)`，
  属性 `pid` / `returncode` / `stdout` / `stderr` / `stdin`（后三个是
  `file` 对象）。`communicate()` 返回 `(stdout, stderr)` 两个字符串。
  没有 `poll()`：PyAdamas 里 `None` 和 `0` 同形，`poll() is None` 会把正常
  退出（rc=0）误判成还在跑，要轮询就用 `wait(timeout=…)` 捕获
  `TimeoutExpired`。
- 与 CPython 的偏差：不返回 bytes，`text=False` 也返回 `str`；没有捕获时
  `.stdout` / `.stderr` 是空串（CPython 是 `None`）；`input` / `env` /
  `preexec_fn` 未实现；exec 失败按退出码 127 返回；`Popen` 句柄被回收时
  会 SIGKILL 子进程（CPython 不会）。
- 内置 `time`：`time` / `monotonic` / `perf_counter` / `sleep`，秒为单位。
- 内置 `string`：`stdlib/string.py` 随编译器分发，提供
  `ascii_lowercase` / `ascii_uppercase` / `ascii_letters` / `digits` /
  `hexdigits` / `octdigits` / `punctuation` / `whitespace` / `printable`
  和 `capwords()`。常量是纯 PyAdamas；`capwords` 调内部 intrinsic，走单遍 C
  实现，不建中间词表。
- 内置 `itertools`：`chain(a, b)` / `repeat(v, n)` / `islice(x, stop)`，以及
  `product` / `permutations` / `combinations` / `combinations_with_replacement` /
  `zip_longest` / `accumulate`。全部按实参静态特化，返回 list（立即求值）；
  `zip_longest` 用 `Optional[T]` 给缺列补 `None`。
- 内置 `collections`：`Counter(items)` 返回 dict，`most_common(n)` 返回
  `(key, count)` 列表。
- 内置 `functools`：`reduce(f, xs[, init])` 折叠（int/str 走整数跳板，
  float 要求 f 的形参是 float，或由调用点按元素类型回填）；`wraps(f)`
  只作装饰器用——PyAdamas 不建模 `__name__`/`__doc__`，这一层在装饰器链里
  直接跳过。`partial(f, *args, **kwargs)` 把前面的实参绑定进合成闭包，
  返回签名缩减后的函数值（位置和关键字都能绑定；不支持 `*args`/`**kwargs`
  函数）；`lru_cache` 作为装饰器使用，签名不变，按形参逐层建 dict 缓存，
  未命中再调用原函数并写回。缓存无上限（`maxsize` 只求值不淘汰），键类型
  限 `int`/`bool`/`float`/`str`（`unknown` 按机器字，和 dict 一致），
  不支持方法和零参函数。
- 内置 `operator`：`add`/`sub`/`mul`/`truediv`/`floordiv`/`mod`/`pow`/
  `and_`/`or_`/`xor`/`lshift`/`rshift`/`concat`、比较 `eq`/`ne`/`lt`/`le`/
  `gt`/`ge`、`is_`/`is_not`、`neg`/`pos`/`abs`/`invert`、`truth`/`not_`、
  `contains`、`getitem`、`itemgetter`（int 单键；多键返回元组）。
  包装函数在调用点按实参类型特化；`attrgetter` 未实现。
- 内置 `datetime`：`date(y, m, d)` / `datetime(y, m, d[, h, mi, s])` 构造，
  `now()` / `today()` 取当前时间；`.year` / `.month` / `.day` / `.hour` /
  `.minute` / `.second` / `.weekday()` 读字段，`.isoformat()` /
  `.strftime(fmt)` 格式化，`print()` 按 ISO 输出；`timedelta` 支持构造
  （`days`/`seconds`/`microseconds`/`milliseconds`/`minutes`/`hours`/`weeks`）、
  字段、`total_seconds()`、加减、乘/除整数、取负、比较与 ISO 风格打印，
  `date`/`datetime` 也支持 `replace()`。没有时区和 `fromtimestamp`；
  `datetime.datetime.now()` 这种链式类方法暂不支持，用 `datetime.now()`。
- 内置 `pathlib`：`Path(s)` 构造，`p / "x"` 拼接，`str(p)` 取路径字符串；
  `.name` / `.stem` / `.suffix` / `.parent` / `.parts` 取成分，`.exists()` /
  `.is_file()` / `.is_dir()` / `.is_absolute()` 查文件系统，`.read_text()` /
  `.write_text(s)` / `.read_bytes()` / `.write_bytes(b)` 读写，
  `.iterdir()` / `.glob(pattern)` 列目录（`*`、`?`、`**` 递归），
  `.mkdir(mode=0o777, parents=False, exist_ok=False)` 建目录，
  `.joinpath(...)` / `.relative_to(...)` / `.resolve()` / `.absolute()` 处理路径，
  `.touch()` 建空文件，`.unlink()` / `.rmdir()` 删除，`.with_suffix(suffix)` 替换后缀。
- 内置 `collections`：`Counter(iterable)` 计数 + `.most_common(n)`；
  `OrderedDict()`（PyAdamas 的 dict 本来就保序）；`deque([iterable[, maxlen]])`
  环形队列，支持 `append`/`appendleft`/`pop`/`popleft`/`rotate`/`clear`/
  `extend`/`extendleft`/`remove`/`count`/`index`/`insert`/`reverse`/`copy`、
  `len`、下标、迭代和 `.maxlen`；`defaultdict([factory[, initial]])`
  在缺省下标读取时按工厂造值并插入，工厂只支持 `int`/`float`/`bool`/`str`/
  `list`/`dict`/`set`/`tuple`（任意 callable 还不支持），`get` / `in` 不触发
  工厂。没有 `namedtuple`。
- 内置模块的构造器可以用 `from X import Y` 直接引入：`from pathlib import Path`、
  `from collections import deque, OrderedDict, Counter, defaultdict`、
  `from datetime import date, datetime`。
- 内置 `json`：`dumps(obj)` 按静态类型生成 JSON；`loads(text, sample)`
  用 sample 的静态类型决定结果结构，覆盖 int/bool/float/str/None/list/
  dict/定长 tuple；默认分隔符 `", "` / `": "`。
- 内置 `json` 的动态路径（0.447，LSP/对端决定形状的协议用）：
  `json.parse(text)` 不要 sample，返回 **unknown**——运行期是
  `dict[str, unknown]` / `list[unknown]` / int / float / str，其中 JSON 的
  `null`/`true`/`false` 用无出边小盒表示（所以 `parse`→`stringify` 往返
  不丢 true/false/null）；整数超出 int64 抛 `ValueError`，嵌套深度上限 512；
  和 `json.loads` 的 float 样例、CPython 默认口径一致，也接受 `NaN` /
  `Infinity` / `-Infinity`（stringify 原样写回）。
  `json.stringify(v)` 接受任意静态类型：静态已知的部分按类型精确输出，
  unknown 位置在运行期按值的实际种类判型（盒 / CPYFLOAT / str / list /
  dict），默认分隔符与 `dumps` 相同。访问辅助（取值后都是 unknown，除非
  注明）：
  `get(v, key)`（对象按 str 键 / 数组按 int 下标，缺失抛 KeyError/IndexError）、
  `get_or(v, key, default)`、`has(v, key)`、`path(v, list[str])`、
  `len(v)`、`keys(v) -> list[str]`、`type(v) -> str`（`"null"`/`"bool"`/
  `"int"`/`"float"`/`"str"`/`"list"`/`"dict"`）、`is_null(v)`、
  `as_str` / `as_int` / `as_float` / `as_bool`（类型不符抛 TypeError）、
  `as_dict(v) -> dict[str, unknown]`、`as_list(v) -> list[unknown]`；
  构造用 `json.bool(b)` / `json.null()` 拿盒（float 转 unknown 本来就自动
  装盒，不需要额外 helper）。
  从 `as_dict`/`as_list` 拿到的容器可以直接 `v["k"]` / `v[i]` / `len` /
  `for` 迭代（静态容器类型已知）。
  **边界**：语言里 `None`/bool 在 unknown 域就是裸 0/1、容器元素没有逐字
  类型标签，所以「异构容器里的裸 True/None」和「静态 `list[float]` 塞进
  未知位置」在动态 stringify 时会退化成 0/1 或位模式；要精确构造 JSON
  的 true/false/null 用 `json.bool()`/`json.null()`（解析得到的值逐元素
  已装盒，不受影响）。
- 内置 `csv`（`stdlib/csv.py` 随编译器分发）：`reader(f, delimiter=",",
  quotechar='"')` 一次读完文件并返回 `list[list[str]]`；
  `writer(f, delimiter=",", quotechar='"', quoting=0, lineterminator="\r\n")`
  是带 `writerow(list[str])` / `writerows(list[list[str]])` 的类。常量
  `QUOTE_MINIMAL` / `QUOTE_ALL` / `QUOTE_NONNUMERIC` / `QUOTE_NONE`；
  `Error` 是 `ValueError` 子类。只接受 `file`（不接受任意可迭代对象），
  `reader` 只收 `delimiter`/`quotechar`，`writer` 另收 `quoting`/`lineterminator`；
  `delimiter`/`quotechar` 按字节，必须单字节；`QUOTE_NONNUMERIC` 等同
  `QUOTE_ALL`（字段静态类型都是 str）。没有 dialect / DictReader / DictWriter。
- 标准库 `logging`（`stdlib/logging.py` 随编译器分发）：等级常量
  `DEBUG`/`INFO`/`WARNING`/`ERROR`/`CRITICAL`，`basicConfig(level=...)`、
  `getLogger(name)`、`getLevelName`、`isEnabledFor`，以及 `Logger` 上的
  `debug`/`info`/`warning`/`error`/`critical`/`log`/`setLevel`。默认输出到
  `sys.stderr`，格式 `LEVEL:name:message`。没有 handler/filter/Formatter/
  文件输出/`%` 参数展开；支持 `getLogger(...).info(...)` 这类链式调用。
- 标准库 `statistics`（`stdlib/statistics.py`）：`mean`/`fmean`/
  `median`/`median_low`/`median_high`/`mode`/`pvariance`/`variance`/
  `pstdev`/`stdev`/`harmonic_mean`/`geometric_mean`，输入为 `list[float]`
  （`mode` 为 `list[int]`），空数据抛 `StatisticsError`。
- 标准库 `textwrap`（`stdlib/textwrap.py`）：`wrap`/`fill`（贪心装行，
  超长单词不拆，接受 `initial_indent`/`subsequent_indent`）、`dedent`、
  `indent`、`shorten`；没有 `TextWrapper` 类。
- 标准库 `base64`（`stdlib/base64.py`）：`b64encode` / `b64decode` /
  `urlsafe_b64encode` / `urlsafe_b64decode` / `standard_b64encode` /
  `standard_b64decode` / `b16encode` / `b16decode` / `b32encode` /
  `b32decode` / `encodebytes` / `decodebytes`，输入输出都是 `bytes`；
  解码同时接受标准与 URL-safe 字母表，非法字符抛 `ValueError`；
  b64 / urlsafe 编解码走 C 内建（`__pa_b64encode` / `__pa_b64decode`），
  b16 / b32 为纯 PyAdamas；不支持 `validate=` / `altchars=` 与 `str` 输入。
- 标准库 `binascii`（`stdlib/binascii.py`）：`hexlify` / `unhexlify` /
  `b2a_hex` / `a2b_hex`，非十六进制字符抛 `binascii.Error`。
- 标准库 `heapq`（`stdlib/heapq.py`）：`heappush` / `heappop` / `heapify` /
  `heappushpop` / `heapreplace` / `nlargest` / `nsmallest`，按 `<` 比较。
- 标准库 `bisect`（`stdlib/bisect.py`）：`bisect_left` / `bisect_right` /
  `bisect` / `insort_left` / `insort_right` / `insort`，当前只覆盖有序
  `list[int]`，`hi` 省略或 0 表示列表末尾。
- 标准库 `hashlib`（`stdlib/hashlib.py`）：`md5` / `sha1` / `sha256`，
  返回带 `update(bytes)` / `digest() -> bytes` / `hexdigest() -> str`
  的对象；SHA-256 的压缩函数走 C 内建（`__pa_sha256` / `__pa_sha256_hex`），
  md5 / sha1 为纯 PyAdamas 实现，结果与 CPython 一致，跨多个 64 字节块也正确。
  不支持 `usedforsecurity=`、`copy()`、`pbkdf2_hmac`/`scrypt` 等派生函数。
- 标准库 `hmac`（`stdlib/hmac.py`）：`hmac.new(key, msg=b"", digestmod="sha256")`
  返回带 `update(bytes)` / `digest()` / `hexdigest()` 的对象。支持
  `"md5"` / `"sha1"` / `"sha256"` 字符串名（与 CPython 的
  `hmac.new(key, msg, "sha256")` 一致），>64 字节的 key 会先做一次摘要；
  不接受传入 hash 构造函数或模块对象。
- 内置 `re`：`compile` / `search` / `match` / `fullmatch` / `findall` /
  `sub` / `split` / `escape`，以及 `Pattern` 上的同名方法和 `Match` 的
  `group`/`groups`/`start`/`end`/`span`。支持字面量、`.`、字符类、
  `\d\w\s\D\W\S`、`\b`、`^ $`、分组（`(...)`/`(?:...)`/`(?P<name>...)`）、
  交替、`* + ? {m,n}`（含惰性）和 `I`/`M`/`S` 标志；匹配按 UTF-8 字节引擎
  （合法 UTF-8 自同步，匹配位置必在码点边界），`.`/字符类/起点按码点步进，
  `\w` 与大小写折叠仍只认 ASCII。
  v2 支持反向引用（`\1`..`\99`、`(?P=name)`）、四向断言、`(?>)` 原子组、
  占有量词、内联标志（`(?i)` / `(?im-sx)` / `(?i:...)`）与 `re.X`；
  `findall` 只有一个捕获组时返回组内容（多个时返回整段匹配），`split`
  按序插入捕获组。仍不支持条件组与 Unicode 属性类。

### sorted 的 key

`sorted(xs, key=f)` 按 `f(x)` 排。`f` 要是一个用户函数（取一个参数）；内建
函数里只有 `len` 能用（其它内建函数没有闭包对象，调用会跳飞，明确报错）。

键的比较方式由 `f` 的返回类型决定。返回类型推不出来时（常见），按运行期键族
比较，与 `list.sort()` 的 kind-3 口径相同：全数值（int/bool/float/big）按数值、
全 str 按字节、类实例按 `__lt__` 槽；族混杂或实例没有 `__lt__` 抛 TypeError。
元组/列表键需要给 `f` 标注返回类型（编译器据此生成字典序描述符），推不出来时
明确报错而不是静默按地址排：

```python
def by_name(u: User) -> str:
    return u.name

print(sorted(users, key=by_name))   # 键是 str，按字符串比
```

### 内置函数

已生成代码：`print` / `len` / `int` / `float` / `str` / `bool` / `range` /
`next` / `abs` / `min` / `max` / `sum` / `pow` / `any` / `all` / `complex` /
`ord` / `chr` / `hex` / `bin` / `oct` / `round` / `divmod` / `repr` /
`format` / `list` / `tuple` / `dict` / `set` / `frozenset` / `sorted` / `reversed` /
`enumerate` / `zip` / `input` / `isinstance` / `getattr` / `setattr` /
`hasattr` / `map` / `filter` / `callable` / `issubclass` / `hash`。
`min` / `max` 支持多实参和 list/tuple/str/dict/set/range 迭代，空序列抛
`ValueError`；`sum` 支持可选的起始值；`any` / `all` 支持短路；`abs` /
`pow` 支持 int 与 float，`pow(x, y, m)` 是 `(x ** y) % m`；
`int("42")` / `int("ff", 16)` / `float("1.5")` 按 CPython 规则解析字符串
（失败抛 `ValueError`）；`ord` / `chr` 按**字节**处理（`chr` 只接受
0..255）；`round` 用「四舍六入五成双」，`round(x)` 返回 int、
`round(x, n)` 返回 float，整数配负 ndigits 也按 10 的幂取整；
`divmod` 返回 `(商, 余数)`，符号规则同 CPython；`repr` 支持字符串/
数值/bool/None/元组（元组按 CPython 的形式，单元素带尾逗号 `(1,)`；
异构元组逐元素按各自静态类型渲染，嵌套元组递归处理）；
`str(元组)` 与 `repr(元组)` 相同。`format` 支持数值和字符串的格式说明符。
`isinstance(x, T)` 的第二个参数支持内置类型名（int/float/str/bool/list/
tuple/dict/set/complex/object）和本地类；静态类型确定时编译期折叠，
unknown 值上对带运行期类型标签的类型（str/list/dict/complex）走运行时
检查，类走 MRO 检查；导入类目标按运行期限定名沿实例 MRO 匹配。
`getattr` / `setattr` / `hasattr` 支持两种形式：
`getattr`/`setattr` 支持字段和 property（property setter 暂不支持），静态字段编译期翻成固定偏移的字段读写或 property 虚调用，零运行时开销；
静态布局里没有的字段或属性名不是字面量则落到实例末尾的惰性 `dict[str, unknown]`（只有真反射过的类才开槽）；
`hasattr` 对静态存在的字段/方法/property 编译期返回 True/False。
`hasattr(module, ...)` / `getattr(func, ...)` / unknown 接收者在 check 阶段打 unsupported 放过（静态证明不了有还是无），build 用模块当值才拒绝。
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
用字符串键、`dict(iterable)` 要求元素是 `(key, value)` 二元组；`sorted` 支持 int/float/str 元素，`key=`（按键排）和
`reverse=True` 都支持；`enumerate` 产出的元素是 `[下标, 值]` 对，`zip` 支持两个可迭代
对象；这些迭代辅助都返回具体列表（生成器实参会被抽干），生成器表达式
本身仍是惰性的。
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
其余内置名（`delattr` 等）类型层面放行，降级阶段报「not supported by the
code generator yet」，不会再退化成调用空指针。

`slice(a, b, c)` 只在**下标位置**支持：脱糖阶段折成切片语法 `xs[a:b:c]`，
所以 `xs[slice(1, 3)]` / `xs[slice(3)]` / `xs[slice(None, 2)]` 都能用，
切片赋值和 `del` 同理。当独立值用（`s = slice(1, 3)`）会报错——PyAdamas 没有
运行期的 slice 对象。切片里的 `None` 表示「这一端不设限」。

`vars(obj)` 返回实例字段的 dict：key 是字段名（含继承来的），value 统一
是 `unknown`（字段类型各不相同，dict 装不下异构的具体类型）。
`dir(obj)` 返回字段名 + 方法名 + 类属性名 + property 名（排序去重）；
`dir()` 无参返回当前作用域的局部名，模块级是全局名。

`range(...)` 返回的是**惰性对象**（类型 `range[int]`）：只存起点、终点、步长，
元素按需算。`len(r)` / `r[i]` / `x in r` / `for x in r` / 推导式都不物化，
`range(10**9)` 只占几十字节。`list(r)` / `tuple(r)` / `sorted(r)` 这类需要
真列表时才物化。range 是不可变的：`r[0] = 1` 报错，也没有 list 的那些
方法。`repr(r)` 是 `"range(0, 10)"`，和 CPython 一样。

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
`raise ... from None` 清空原因。未捕获异常会打印函数级 traceback（文件:行
+ 函数名）；异常值本身仍没有 traceback 对象，`args` 只保存消息。
`finally` 在正常、异常、`return`、`break`、`continue` 路径都会执行；
`finally` 体内跳出 finally 的 `return` / `break` / `continue` 也支持：
跳出路径会内联一份 finalbody，最终返回值/跳转以 finally 里的为准，
异常被 finally 里的跳转吞掉。生成器 `close()` 重放 finally 体时同样
支持 `break` / `continue`（重放会恢复 finally 词法上所属的循环）。

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
  做运行期类检查（按模块限定名匹配 MRO），字段子模式（位置/关键字）走模块
  接口里的字段布局与 `__match_args__`，和本地类一致。
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

## 值类型 struct

模块顶层用 `struct` 定义值类型，字段只有固定标量：`int`、`float`、`bool`。
`struct` 是软关键字，只有 `struct NAME:` 换行缩进块的写法才是定义，变量名、
模块名照常可用。

```python
struct Point:
    x: float
    y: float

p = Point(1.0, 2.0)
print(p.x, sizeof(Point))    # 1.0 16
p.x += 0.5
```

布局与 C 一致：字段按声明顺序，`int` 对应 `int64_t`、`float` 对应 `double`、
`bool` 对应 `_Bool`；每个字段从自身对齐的偏移开始，整体大小按最大对齐补齐。
所以上面的 `Point` 与 C 的 `struct Point { double x; double y; }` 同布局，
`addr(p)` 可以直接传给 C。

- `Point(...)` 按字段顺序构造，参数个数和类型必须匹配。
- `sizeof(T)` / `sizeof(v)` 是编译期常量。
- `addr(p)` 得到 `ptr[Point]`（就是 `Point*`），字段读写自动解引用。
- **值语义**：赋值、传参、返回都复制整块数据，改副本不影响原值；放进
  `list`/`dict` 等容器时仍按引用存放（与类实例一致）。
- 字段读写是编译期固定偏移；`bool` 字段只占 1 字节。

限制：字段只能是标量；不支持方法、继承、嵌套结构体、`str` 字段、定长数组；
`print(p)` 直接报错，请打印字段。

## C 互操作（cimport）

`cimport` 在**模块顶层**声明要调用的 C 库，不产生运行时对象；把它当值用会报错。

```
cimport m
cimport mylib as ml, other.lib

print(m.sqrt(2.0))
```

`a.b` 对应声明文件 `a/b.cpya`（相对源文件目录或 `-I` 路径查找）。没有 `as` 时绑定名取最后一段（`cimport a.b` 之后用 `b.fn`）。

**`.cpya` 是 Python 语法的 stub**：没有实现，函数体只有 `pass`；编译器把它当「C 声明」读。
格式与 `stdlib/sigs/*.pysig` 一致（同一套 Lexer/Parser/注解），只是注解词汇是 C 边界类型：

```python
# tests/run/clib.cpya
cmodule("clib")                 # 可省略：默认从 cimport 的名字推导
link("m")                       # -> -lm；给路径（含 / 或 .c/.a/.so 结尾）或 "-" 开头则原样
PI: float = 3.141592653589793   # 常量：带注解的赋值，编译期内联

class FILE: pass                # 不透明句柄：class 体只有 pass
class pair:                     # 带字段的 C struct：注解无值赋值 = 字段（按 C 规则布局）
    a: float
    b: float

def sqrt(x: float) -> float: pass                  # C 符号 = Python 名
@extern("strlen")                                   # 改名时才要装饰器
def slen(s: str) -> int: pass
@extern("qsort")
def qsort(base: ptr, n: int, size: int, cmp: Callable[[ptr[int], ptr[int]], int]) -> None: pass

stdout: ptr[FILE]                                  # 无值 = extern var（C 名 = py 名）
optind: int32 = extern_var("optind")               # 改名：extern_var("c_name")
```

规则：

- 顶层只允许：模块 docstring、`cmodule("名")`、`link("参数")`、带注解的常量、`class`、`def`、注解无值的 extern var。其它语句报错并给行号。
- `cmodule(...)` 可省略；写了就必须等于 `cimport` 的名字（写错是硬错误）。
- `link(...)` 参数去重后追加到链接命令；`link("z")` → `-lz`，`link("tests/run/x.c")` 原样。
- `def` 的参数与返回值**必须有注解**；`*args` 表示 C 变参（如 `printf`）；`None` 表示无返回值。
- 默认 C 符号 = Python 名；改名用 `@extern("c_name")`。`def` 体只能是 `pass`（可带 docstring）。
- `class NAME: pass` = 不透明句柄（只能 `ptr[NAME]`）；`class NAME:` + 注解无值赋值 = 带字段的 C struct（按 C 规则布局，`ptr[NAME]` 可读写字段，也可按值传参/返回）。
- extern var：`NAME: T`（无值，C 名 = py 名）或 `NAME: T = extern_var("c_name")`（改名）。
- 旧的行式语法（`cmodule m` / `extern def "x" as y(...)` / `struct NAME` …）**已删除**，不兼容。

`extern var` 读取 C 全局变量，赋值时按同一宽度写回：

```python
optind: int32
program_invocation_name: str = extern_var("program_invocation_name")
```

```python
cimport clib as c
print(c.optind)   # 初始为 1
c.optind = 0      # 写回 C 全局变量
```

变量类型支持 `int`/`int32`/`uint32`/`float`/`bool`/`ptr`/`ptr[Struct]`/`str`/`Optional[str]`。`str` 变量只读（写入会报错），因为读取时已经把 `char*` 复制成 `str`；按值结构体变量不支持，请声明为 `ptr[Struct]` 后读写字段。

| `.cpya` 类型 | C 类型 | 说明 |
|---|---|---|
| `int` | `long` | 64 位 |
| `int32` / `uint32` | `int` / `unsigned` | 返回值分别符号/零扩展 |
| `char`/`int8`、`short`/`int16`、`uchar`/`uint8`、`ushort`/`uint16`、`uint64`/`ulong` | 对应 C 窄/无符号整数 | 边界按宽度符号/零扩展 |
| `float` | `double` | |
| `float32` | `float` | 边界做 double↔float 转换 |
| `bool` | `_Bool` | 返回值统一为 0/1 |
| `str` | `char*` | 传入 `data`；返回时复制成 `str` |
| `ptr` / `ptr[T]` | `void*` / `T*` | 类型系统中是 `Optional[int]`，0 表示 NULL |
| `ptr[Struct]` | `Struct*` | 结构体有字段声明时是 `Optional[ptr[Struct]]`，可读写字段；否则同 `Optional[int]` |
| `Struct` | `struct Struct` | 带字段的结构体按值传参/返回，见「结构体按值传递」 |
| `Callable[[P...], R]` | 函数指针 `R (*)(P...)` | C 回调形参；只允许标量/指针，且不转发栈参数（整数 ≤6、浮点 ≤8） |

| `Struct` / `union` 数组字段 | `T[N]` / `T[A][B]` | 定长数组（行优先）；元素可以是标量/ptr/struct/union。`p.buf[i]` 读写下标，`p.buf` 本身可以当 `ptr` 传给 C |
| `union`（类装饰器） | `union U { ... }` | `@union` + `class U:`，所有成员偏移 0，size = max、align = max |
| `@packed` | `__attribute__((packed))` | 无填充布局（epoll_event 这类），不能与按值 ABI 混用 |
| `None` | `void` | |
| `Optional[str]` | 可空 `char*` | NULL 返回 `None` |

不支持：在 `cimport` 时直接解析 C 头文件（可先用 `pyadamas cdecl` 离线生成 `.cpya`）。`.cpya` 不参与 `.pya` 模块缓存。

`str` 参数要求非空，不能把 `Optional[str]` 直接传给它（会有 NULL）；需要接受 NULL 时声明为 `Optional[str]`。`ptr` 是不透明地址，0 表示 `NULL`，不能解引用或做数组访问。

### 缓冲区传参

`ptr[T]` 除了原始地址，还可以直接接收底层布局兼容的 py23 缓冲区，编译器会取连续 `data` 指针传给 C：

| 形参 | 允许的实参 | 传给 C |
|---|---|---|
| `ptr[float]` | `list[float]` | `double*` |
| `ptr[int]` | `list[int]` | `long*` |
| `ptr` | `list[float]`、`list[int]`、`bytes` | 连续 `data` |
| `ptr` / `ptr[T]` | `int` / `Optional[int]` / `None` | 原样，`None` 为 NULL |

`list[float]` 的槽位就是 8 字节 double，`list[int]` 是 8 字节 long；`bytes` 的 `data` 连续且 NUL 结尾。不匹配的组合会被拒绝，例如 `list[int]` 不能传给 `ptr[float]`，`bytes` 不能传给 `ptr[float]`/`ptr[int]`（元素宽度不同）。空 list 的 `data` 可能为 NULL，原样传 0。

指针只在本次 C 调用期间有效：C 函数不得保存它，调用期间也不要改变 list 长度。C 写回 `data` 后，py23 侧的 list 元素立即可见。多维数组请自己拍平并显式传 stride。

### 指针解引用

带元素类型的指针（`ptr[int]`、`ptr[float]`、`ptr[int32]`、`ptr[uint32]`、`ptr[bool]`）支持 `p[i]` 读写，地址按元素宽度计算，没有边界检查：

```
extern def "malloc" as malloc_i(n: int) -> ptr[int]
extern def "malloc" as malloc_f(n: int) -> ptr[float]
```

```python
cimport clib as c

p = c.malloc_i(24)
p[0] = 42
p[1] = -7
print(p[0], p[1])   # 42 -7
c.free(p)

q = c.malloc_f(16)
q[0] = 1.5
print(q[0])          # 1.5
c.free(q)
```

`int32` 读取符号扩展、`uint32` 零扩展、`float` 走 `movsd`。指针为 `None` 时解引用会抛 `AttributeError`；不透明 `ptr`（没有 `[...]`）不能下标。元素类型会跟着变量传递，但经过手写的 `Optional[int]` 标注后会丢失。

### 可变参数

参数表末尾写 `...` 表示 C 可变参数函数：

```
extern var "stdout" as stdout: ptr[FILE]
extern def "printf" as printf(fmt: str, ...) -> int32
```

```python
cimport clib as c

n = c.printf("x=%d y=%.2f s=%s\n", 3, 1.5, "ok")
print(n)   # 16
```

额外实参按 py23 类型选择编组：`int`/`bool` 走整数寄存器，`float` 走 `xmm`，`str` 传 `data`，`Optional[str]` 可空，`ptr`/`Optional[int]` 按地址传。调用前 `%al` 会被设成使用的向量寄存器数（SysV 要求）。`list`、结构体等不能作为可变参数传递。

### 回调

参数类型写成 `fn(T1, T2, ...) -> R` 表示 C 函数指针，调用时直接传 py23 函数或闭包：

```
extern def "qsort" as qsort(base: ptr, n: int, size: int, cmp: fn(ptr[int], ptr[int]) -> int) -> None
```

```python
cimport clib as c

p = c.malloc_i(40)
for i in range(5):
    p[i] = 5 - i


def cmp(a: ptr[int], b: ptr[int]) -> int:
    if a[0] < b[0]:
        return -1
    if a[0] > b[0]:
        return 1
    return 0


c.qsort(p, 5, 8, cmp)
c.free(p)
```

回调签名只允许标量（`int`/`int32`/`uint32`/`float`/`bool`）和 `ptr`/`ptr[T]`；返回 `None` 表示 `void`。编译器为每个回调参数生成一个跳板和闭包槽：调用前把闭包登记到槽里，C 调用跳板时恢复闭包环境（`%r10`）再进入 py23 函数，因此**捕获环境**的闭包也能用。32 位/布尔参数会按声明宽度提升到 64 位。

限制：C 必须在本次调用期间同步调用回调，不能保存函数指针；同一个声明位置的回调槽是共享的，回调里再触发同一个 C 函数（重入）会覆盖闭包。回调参数最多 6 个整数 + 8 个浮点，超出会拒绝声明。

`ptr[int]` 这类标注在类型系统里仍是 `Optional[int]`，但保留元素种类，函数体内可以 `a[0]` 解引用。

### 从头文件生成声明（cdecl）

`pyadamas cdecl` 可以把 C 头文件的声明子集离线转换成 `.cpya`，省去手抄函数原型：

```
pyadamas cdecl /usr/include/foo.h -o foo.cpya --link "-lfoo"
```

支持：

- `#define NAME 42` / `1.5` / `"text"` / `true` → `const`（表达式宏和函数式宏跳过）
- `typedef`（标量别名、`typedef struct {...} Name;`、`typedef struct Tag Alias;`）
- `struct` 定义（定宽标量 / 指针字段）；union、位域、数组、嵌套结构体字段降级为不透明 `struct Name`
- `enum { A, B = 5 }` → `const A: int = ...`（常量表达式支持 `+ - * / % << >> & | ^ ~ !` 和已定义的枚举/宏）
- 函数原型 → `extern def`，参数自动命名为 `p0`/`p1`…；`const char*` → `str`，`T*` → `ptr[T]`，`void*` → `ptr`，函数指针参数 → `fn(...)`
- `extern` 变量 → `extern var`；`static`/`inline` 函数定义跳过
- `--link ARG` 写入 `link "ARG"`，适合直接给 `.c`/`.o`/`-lfoo`

已知限制：不展开宏函数与条件编译（`#ifdef` 内容仍按原文处理）；`float`（C 的 4 字节浮点）、`union` 按值、定长数组不生成；生成结果建议人工复核后再提交。样例见 `tests/cdecl/sample.h`。

### 不透明结构体指针

`struct NAME` 只登记类型名，不描述布局；`ptr[NAME]` 表示 `NAME*`，用于 `FILE*`、`sqlite3*` 这类句柄：

```
cmodule cstdio
struct FILE
extern def "tmpfile" as tmpfile() -> ptr[FILE]
extern def "fopen" as fopen(path: str, mode: str) -> ptr[FILE]
extern def "fputs" as fputs(s: str, f: ptr[FILE]) -> int32
extern def "fclose" as fclose(f: ptr[FILE]) -> int32
```

```python
cimport cstdio

f = cstdio.tmpfile()
if f is not None:
    cstdio.fputs("hello", f)
    cstdio.fclose(f)
```

结构体名可以先使用后声明（解析时会先扫描全部 `struct` 行）。不透明 `ptr[Struct]` 在类型系统里是 `Optional[int]`，可以判 `is None`；它**不接受** `list`/`bytes`，避免把缓冲区误当句柄。

### 带字段的 C 结构体

`struct NAME:` 加缩进字段会按 C 规则算好布局，`ptr[NAME]` 的字段可以直接读写：

```
cmodule clib
struct tm:
    tm_sec: int32
    tm_min: int32
    tm_hour: int32
    tm_mday: int32
    tm_mon: int32
    tm_year: int32
    tm_wday: int32
    tm_yday: int32
    tm_isdst: int32
    tm_gmtoff: int
    tm_zone: ptr
extern def "gmtime" as gmtime(t: ptr[int]) -> ptr[tm]
```

```python
cimport clib as c

p = c.malloc(8)
c.memset(p, 0, 8)
tm = c.gmtime(p)
print(tm.tm_year, tm.tm_mon, tm.tm_mday)
tm.tm_year = 70
c.free(p)
```

字段类型只允许定宽标量：`int`（8 字节）、`int32` / `uint32`（4 字节）、`float`（8 字节 double）、`bool`（1 字节）、`ptr`（8 字节）。布局算法与 C 相同：字段按声明顺序，offset 按字段对齐向上取整，整体大小按最大对齐补齐。`int32` 读取符号扩展，`uint32` 零扩展；字段读写都是编译期固定偏移。`ptr` 字段在类型系统里是 `Optional[int]`，可以判空。

这类结构体也能在 py23 里按字段构造，并作为值类型按值传给 C 或从 C 返回；字段访问会做空指针检查。结构体在 C 侧的实际布局仍以头文件为准，声明写错会读到错误数据。


### union、定长数组与 packed 结构体

```python
@union
class epoll_data:          # 所有成员偏移 0，size = max(成员)
    ptr: ptr
    fd: int32
    u32: uint32
    u64: uint64

@packed                    # 无填充（C 的 __attribute__((packed))），epoll_event 用
class epoll_event:
    events: uint32
    data: epoll_data

class sockaddr_un:
    sun_family: uint16
    sun_path: char[108]    # C 的 char sun_path[108]

class matrix:
    grid: int32[2][3]      # C 的 int32_t grid[2][3]（行优先）
```

数组字段的访问：`p.sun_path[i]` 读写下标（`char` 元素是 `i8`，写入要 `cast_i8(...)`），
`p.sun_path` 本身就是元素起始地址，可以直接当 `ptr` 传给 C；多维数组 `p.grid[i][j]`
逐层取地址、最后一层读元素；struct/union 元素数组 `p.rows[k].a` 先取元素地址再读写字段。
union 字段各按自己的偏移（0）与宽度读写：写 `u.u64` 后读 `u.u32` 得到低 32 位，与 C 一致。
`@packed` 只影响布局；含 `union`/数组的 struct 按值传给 C 的 ABI 分类暂不支持（见 docs/cffi-design.md）。

### 结构体按值传递

带字段的 `struct` 可以直接写在 `extern def` 的参数和返回类型上，不需要 `ptr[...]`：

```
struct div_t:
    quot: int32
    rem: int32
struct in_addr:
    s_addr: uint32

extern def "div" as div(n: int32, d: int32) -> div_t
extern def "inet_ntoa" as inet_ntoa(a: in_addr) -> str
```

```python
cimport clib as c

d = c.div(7, 3)          # C 返回的结构体直接是 py23 值
print(d.quot, d.rem)     # 2 1
a = in_addr(c.htonl(3232235777))
print(c.inet_ntoa(a))    # 192.168.1.1
```

传参和返回按 SysV x86-64 聚合规则编组：≤16 字节的结构体按 8 字节 chunk 分类，只含 `double` 的 chunk 走 `xmm`，其余走整数寄存器；寄存器不够时整块走栈；大于 16 字节时参数在栈上，返回值通过隐藏指针（`rdi`）传回。py23 侧的结构体值与 C 结构体二进制一致，赋值、传参、返回都会复制整块 payload（值语义），因此改一个变量不会影响另一个。

不透明 `struct NAME`（无字段）仍然只能以 `ptr[NAME]` 使用，按值出现会报 `struct 'NAME' is opaque and cannot be passed by value`。

BLAS 示例（`cblas.cpya`，需系统安装 OpenBLAS；开发容器已安装 `libopenblas-dev`）：

```
cmodule cblas
link openblas

const ROW_MAJOR: int32 = 101
const NO_TRANS: int32 = 111

extern def "cblas_dgemm" as dgemm(layout: int32, trans_a: int32, trans_b: int32, m: int32, n: int32, k: int32, alpha: float, a: ptr[float], lda: int32, b: ptr[float], ldb: int32, beta: float, c: ptr[float], ldc: int32) -> None
```

```python
cimport cblas

a = [1.0, 2.0, 3.0, 4.0]
b = [5.0, 6.0, 7.0, 8.0]
c = [0.0, 0.0, 0.0, 0.0]
cblas.dgemm(cblas.ROW_MAJOR, cblas.NO_TRANS, cblas.NO_TRANS,
            2, 2, 2, 1.0, a, 2, b, 2, 0.0, c, 2)
print(c)  # [19.0, 22.0, 43.0, 50.0]
```

stdlib 已封装常用操作（`stdlib/linalg.py`），不用手写 14 个参数：

```python
import linalg

c = linalg.matmul([1.0, 2.0, 3.0, 4.0], [5.0, 6.0, 7.0, 8.0], 2, 2, 2)
print(linalg.dot([1.0, 2.0], [3.0, 4.0]))  # 11.0
```

常用接口：

- `matmul(a, b, m, n, k, trans_a=False, trans_b=False)`：计算
  `op(A)(m×k)·op(B)(k×n)`，返回长度 `m*n` 的 `list[float]`；
  `trans_a` 时 A 以 `k×m` 存储，`trans_b` 时 B 以 `n×k` 存储。
- `matmul_into(c, a, b, m, n, k, ...)`：同 `matmul`，结果写入已有 `c`，
  避免重复分配。
- `matvec(a, x, m, n, trans_a=False)`：计算 `op(A)·x`。
- `axpy(alpha, x, y)`：原地 `y += alpha*x`；`scal(alpha, x)`：原地
  `x *= alpha`；`norm(x)`：欧几里得范数。
- `transpose(a, m, n)`：把 m×n 行主序数据转成 n×m。
- `dot(x, y)`：等长向量点积。

数据按行主序拍平传入；维度非法或输入过短会抛 `ValueError`。



### 字节数 vs 元素数：C 函数只认字节

所有 C 函数的 size/length 参数都是**字节数**，PyAdamas 不会替你乘元素宽度（它也猜不出 C 侧怎么解释）：

| 值 | len(x) | 传给 C 的字节数 |
|---|---|---|
| list[int] / list[float] | 元素个数 | len(x) * 8 |
| bytes | 字节数 | len(x) |
| buffer(n) | 元素个数 | len(x) * 8 |
| buffer_i32(n) / buffer_f32(n) | 元素个数 | len(x) * 4 |

```python
cimport clib as c
xs: list[int] = [1, 2, 3]
c.memcmp(xs, ys, len(xs) * 8)      # 正确：24 字节
c.memcmp(xs, ys, len(xs))          # 错：只比较了前 8 个字节（一个 int）
```


`sizeof` 能给你这些宽度，不用写魔法数字（都是**编译期常量**，零运行时开销）：

```python
sizeof(int)        # 8    sizeof(int32)   # 4
sizeof(int16)      # 2    sizeof(int8)    # 1
sizeof(uint64)     # 8    sizeof(bool)    # 1
sizeof(float)      # 8    sizeof(float32) # 4
sizeof(ptr)        # 8    sizeof(ptr[float])  # 8
sizeof(Point)      # 值类型 struct 的实际布局大小
```

于是上面的调用可以写成 `c.memcmp(xs, ys, len(xs) * sizeof(int))`，宽度跟着类型走，改类型不用改数字。

这条**不会**在编译期报错 —— len() 的结果就是个 int，编译器无从判断它该被当成元素数还是字节数。
写 C 调用时请把 * 8 / * 4 显式写出来，或者用 buffer（它的宽度是类型的一部分：8 字节的 buffer
可以传 ptr[float]，4 字节的只能传裸 ptr）。

### `malloc`/`free` 与裸指针：参数是字节数

`cimport` 里的 `malloc_f(n)` / `malloc_i(n)` 直接对应 C 的 `malloc(size_t)`——
**`n` 是字节数，不是元素个数**，编译器不会替你乘元素宽度：

```python
cimport clib as c
p = c.malloc_f(40)     # 5 个 double（5 * 8 字节）
i: int = 0
while i < 5:
    p[i] = i * 1.0     # 下标按元素宽度寻址，越界写是自己负责
    i = i + 1
print(p[0], p[4])      # 0.0 4.0
c.free(p)
```

写成 `malloc_f(5)` 只分配 5 字节却写 5 个 double 会**踩坏堆**（`malloc(): corrupted top size`），
而且**与下标是常量还是变量无关**——这是使用方的错误，不是编译器的越界检查缺失。
要连续内存优先用 `buffer(n)`（见上一节），它有长度、有越界检查、有 RAII，不需要手工 `free`。

### 生成器/协程的类型注解要带下标

`Generator`、`AsyncGenerator`、`Coroutine`、`Future`、`Task` 这些**只有下标形式**能表达
元素类型；裸名注解推导不出产出类型，保持 `unknown`（调用 `send`/`next` 会报「需要生成器」）：

```python
def count(n: int) -> Generator[int]:   # 正确
    ...

def bad(n: int) -> Generator:          # 裸名：产出类型是 unknown
    ...
```
## 会被拒绝的写法

**语法层**（`check` 就报错）：

- 超出 int64 的整数
- `await` 在函数外、`break` 在循环外
- Python 2 遗留写法（`0x1L`、`lambda (a, b):`、`except X, e`）
- PEP 695 泛型语法（`class C[T]:`）
- 类：方法缺 `self`、字段赋值类型不符、访问不存在的字段或方法（`self.xxx` 与外部基类的成员除外）

**代码生成层**（`check` 通过，`build` 报错）：容器转标量（`int([1, 2])`、`str({})`）、通过 unknown 函数值的 `*` / `**` 展开、`*` 位于固定形参之前且不是定长 tuple、非异常类名当异常用、动态 classmethod 的方法值、导入基类的非字面量字段默认值。异步生成器有 `asend`/`athrow`/`__anext__`/`aclose`。自定义异常类目前只支持 `pass` 类体（字段/方法会被拒绝）。match 额外拒绝：unknown 值上的数值/list/dict/tuple/set 内置类型模式。

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

## 连续内存：`buffer(n)`

`buffer(n)` 分配一块 **RC 管理的连续内存**（`runtime/rt_buffer.c` 的 `pa_buffer`），
元素是 8 字节标量（`float`/`int`），零初始化；`buffer_i32(n)` / `buffer_f32(n)` 是
4 字节元素版本。支持 `len(buf)` 与下标读写 `buf[i]` / `buf[i] = v`（越界 panic，
与 `list` 同口径）；**不支持**切片、迭代、`in`。

生命周期：`buffer` 是普通 RC 对象 —— 赋值给局部变量即持有引用，离开作用域/被重新
赋值时释放，GC 清扫路径也会释放它的 data。**不需要也不能手工 free**。

传给 C：

```python
cimport clib as c
b = buffer(4)          # 4 个 double
c.memset(b, 0, 32)     # ptr[float] 形参直接收 buffer
```

宽度规则（0.537）：`buffer(n)` 的元素是 8 字节，可以传给 `ptr[float]`；
`buffer_i32` / `buffer_f32` 是 **4 字节**，**只能传给裸 `ptr`**（由调用方自己
给正确的字节数）—— 不允许冒充 `ptr[float]`/`ptr[int]`，否则 C 侧按 8 字节步长
读会越界。传错宽度是**编译期错误**，不会静默重解释。
