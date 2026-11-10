# 与 CPython 的差异及验证方法

py23 不追求兼容 CPython：语义是按静态类型的需要重新定的。但**正确性**仍然拿 CPython 当基准——对同一段程序，两边 stdout 必须一致，除非差异在这份文档里写清楚了。

## 一、语义差异（故意的）

| 写法 | py23 | CPython | 原因 |
|---|---|---|---|
| `[1] == [1]` | `False`（比指针） | `True`（逐元素） | 没有运行时类型标签，做不到通用深比较 |
| 读未赋值的局部变量 | 得到 0 / `None` | `UnboundLocalError` | 槽位在栈上，初值是 0；做「已初始化」标记要额外一张表 |
| 整数溢出 | 回绕成 int64 | 自动升格为任意精度 | 静态类型的代价，也是性能的来源 |
| `1 << 70` | `64` | `1180591620717411303424` | x86 的移位次数按 64 取模 |
| `2 ** -1` | `0` | `0.5` | 整数幂走整数实现，负指数直接返 0 |
| `len("中文")` | `6` | `2` | 字符串是**字节串**，`len` 是字节数 |
| `"中文"[0]` | 单个字节（打印出来是乱码） | `"中"` | 索引按字节，不做 UTF-8 解码 |
| `x: float = 1; print(x)` | `1.0` | `1` | 标注决定存储类型，int 在 float 位置被真的转成 double |
| `print(a, b, end=f())` 的求值时机 | 边算边写，`f()` 最后求值 | 先把参数和关键字全算完再写 | `print` 是降级期展开的，不是函数调用 |
| `assert x, "msg"` | 抛 `AssertionError("msg")`；无消息时 `assertion failed` | 同 Python（消息文本一致） | 消息只保留字符串内容 |
| `except ... as e` | `e` 是异常值对象，`args` / `str(e)` 可用 | `e` 是异常对象，有完整 `args` / traceback | `args` 目前只保存消息；没有 traceback / 异常链 |
| 未捕获异常 | `py23: uncaught ValueError: msg`，`exit(1)` | traceback + `ValueError: msg`，退出码 1 | 没有 traceback |
| 异常类型 | 内置异常名 + 继承匹配；`ValueError("msg")` 产生异常值，`raise e`、`raise ... from`、自定义异常类（`class E(ValueError): pass`）可用 | 完整异常类层次、traceback | 自定义异常类类体只能是 `pass`/文档字符串；`args` 只保存消息；没有 traceback |
| 运行时错误 | `IndexError` / `KeyError` / `ZeroDivisionError` / `OverflowError` / `ValueError` / `AssertionError` 可捕获 | 同类异常 | 内部不变量/OOM 仍然直接退出 |
| `range(a, b, c)` 的 `c` | `for ... in range(...)` 里必须是整数字面量；作为值使用时可以是任意 int 表达式 | 任意表达式 | 循环步长在降级期就要定下来；`range()` 实体化成列表后没有这个限制 |
| 默认参数不是字面量 | `build` 报错 | 运行时求值 | 默认值在调用点内联填充 |
| `*args` | 形参是 `list[T]` | 形参是 tuple | 列表和元组在 py23 里表示相同；`len` / 下标 / 迭代一致 |
| 函数装饰器 | `@dec` 在定义点立即执行，包装函数必须用固定形参转发 | 任意可调用 | 没有运行时签名信息，`*args` 转发给 unknown 被装饰者无法绑定 |
| `with` | 只支持类实例的 `__enter__` / `__exit__`；正常、`return` / `break` / `continue` 和异常路径都调用 `__exit__` | 完整上下文管理协议 | `__exit__` 返回值只有真/假语义；异常路径传 `(type, message, None)` |
| import | 本地单层 `.py`；`m.x` 直接读写模块全局；`from m import x` 是快照；模块别名不能当值 | 任意模块/包、运行时模块对象 | 支持包/相对导入/静态 `import *`（按非下划线导出）；跨模块继承暂不支持 |
| 模块初始化 | 第一次执行 import 时惰性运行一次；循环导入编译期报错 | 支持循环导入和部分初始化 | 初始化失败的模块不会再重试；模块对象不可见 |
| 推导式 | list/set/dict 推导式按循环展开；生成器表达式立即构造成 list；含 `async for` 的推导式在协程里可挂起 | 生成器惰性求值 | 异步推导式只能作为语句/赋值/`return` 的根表达式；生成器表达式仍立即求值 |
| 生成器 | 状态机 + 堆帧；`for` / `next` / `send` / `close` / `yield from`；跨 `yield` 跑 `finally` / `__exit__` | 完整生成器协议 | `yield`/`yield from` 只能是语句或赋值右值，不能位于 `finally`/`except` 体内；`yield from` 的值固定为 None；`send` 值静态是 unknown；生成器类型跨模块降级为 unknown |
| 协程 / Task / Future | `async def` / `await` 复用生成器帧；`asyncio.run`/`sleep`/`gather`/`create_task`；`done()`/`cancel()`/`result()`/`exception()`；Future 多等待者、`set_exception`、`add_done_callback`；取消级联；`wait_readable`/`wait_writable` + poll；异步生成器 + `async for` + `async with`；异步推导式 | 完整 asyncio（Task 组/Future 回调/I/O 流） | `await` 只能是语句、赋值右值或 `return` 值；异步生成器有 `asend`/`athrow`/`__anext__`/`aclose`，`aclose` 不会 await `async with` 的 `__aexit__`，停在 `async with` 里被直接回收时不 await 退出；自定义 `__aiter__` 未支持；Future 回调同步执行；`set_exception` 接收异常值或消息；`exception()` 返回异常值；`CancelledError` 是内置名；`wait_readable` 不是 CPython 标准 API；`gather` 结果元素是 unknown；Task 单等待者 |
| 递归深度 | 受栈大小限制（万级） | 默认 1000，可调 | 没有解释器递归检查 |
| 字段初值 `x: int = []` | 每次构造重新求值，实例之间独立 | 类属性，所有实例共享 | 更安全；也避免可变默认值的经典 bug |
| 类体里的非声明语句 | 在类定义点执行；赋值按模块全局处理 | 类体是独立命名空间 | 没有类命名空间，`C.x` / `self.x` 的类属性访问未建模 |
| 类的属性 | 声明过的字段，或 `self.x = ...` 就地长出来的字段 | 可随时给实例挂任意属性 | 偏移是编译期常量，没有运行时 `__dict__` |
| `print(obj)` | 有 `__str__`（或 `__repr__`）就调，否则 `<Point object>` | 同 Python（默认文本带模块名和地址） | 默认文本没有模块名和地址 |
| 协议 dunder | `__str__`/`__repr__`/`__eq__`/`__ne__`/`__len__`/`__hash__`/`__contains__`/`__iter__`/算术与比较类 dunder 走 vtable；`in` 需要 `__contains__`，`<` 等需要比较 dunder，算术需要对应 dunder，否则编译报错 | 完整数据模型 | `__format__`/`__bool__`/`__reversed__` 未建模；`__eq__` 的 other 限 unknown 或类类型；只定义 `__eq__` 没定义 `__hash__` 的类在 dict/set 里按身份处理 |
| `random` | MT19937 与 CPython 同种子同序列；模块级函数和 `Random` 实例方法；`choice` 支持 list/tuple/str，`shuffle` 仅 list | 完整 random（`randrange`/`gauss`/bytes 等未实现） | 只实现常用方法；省略种子时时间 + pid 播种 |
| `os` / `os.path` | `mkdir`/`rmdir`/`remove`/`rename`/`getcwd`/`cpu_count`；`join`/`exists`/`isdir`/`isfile` | 完整 os | `os.path` 是独立模块别名：`import os.path as p` 或 `from os.path import ...`，`import os` 后 `os.path.x` 暂不支持 |
| `subprocess` | `run` 支持 `capture_output`/`text`/`timeout`/`check`，返回 `CompletedProcess`（`.returncode`/`.stdout`/`.stderr`）；`TimeoutExpired`/`CalledProcessError` 按内置异常名捕获 | 完整 subprocess | 没有 bytes：`text=False` 也返回 str；未捕获时 `.stdout`/`.stderr` 为空串；`input`/`cwd`/`env`/`shell` 未实现；exec 失败按退出码 127；`from subprocess import TimeoutExpired` 不支持 |
| 方法调用 | 走 vtable，代价固定 | 走 `__dict__` / 描述符 | 没有去虚化，也不做内联 |
| 内存回收 | 引用计数 + 保守标记-清扫 | 分代 GC | 环和 unknown 泄漏由 GC 延迟回收；栈/寄存器里的整数可能被误认为根，只影响回收时机；无 `__del__`/弱引用 |
| 闭包捕获 | cell 按引用捕获；嵌套函数直接自引用用当前闭包 | cell 按引用捕获，晚绑定 | 父作用域之后重绑函数自己的名字时，py23 仍指向原闭包；相互递归闭包的环由 GC 回收 |
| `lambda` | 脱糖成插在语句前的合成嵌套 `def` | 每次求值新建函数对象 | 调用/捕获语义一致；条件/循环里的 lambda 只创建一次，对象身份与创建时机略有差异 |
| `nonlocal` | 必须显式声明；赋值解析到最近外层函数 | 同 Python | 没有 `nonlocal` 时赋值就是本地变量 |
| 字典 / 集合 | 开放寻址表，保留插入顺序；key 按静态类型哈希/判等 | dict/set，任意 hashable | 空字典的 unknown key 按机器字比较；没有运行时类型标签，不能混用任意 key |
| `d[k]` 缺失 | panic `KeyError: key not found` | 抛 `KeyError(key)` | 运行时错误消息不带 key 内容 |
| `d == e` / `s == t` | 指针比较 | 深比较 | 没有运行时类型标签 |
| `print(d)` / `print(s)` | `<dict object>` / `<set object>` | 打印内容 | 没有容器 repr |
| dict/set 方法 | `get`/`keys`/`values`/`items`/`update`/`add` 和删除类方法 `pop`/`remove`/`discard`/`clear`/`popitem` 可用 | 完整方法 | `keys()` 等返回 list 而不是视图；删除按 order 重建是 O(n)；`popitem` 固定 LIFO，`set.pop` 固定取最后插入项 |
| `d.get(k)` 缺失 | panic | 返回 `None` | 静态返回类型不是 Optional；用 `get(k, default)` 避免 |
| 切片 | str 按字节；list/tuple 返回新容器，语义同 CPython | 同 Python | `step == 0` panic；未知元素类型只做保守 GC |
| 切片赋值 | 仅 list；step 1 可改变长度，扩展切片长度必须一致；右侧可为 str | 任意可变序列，任意可迭代右值 | 没有通用迭代协议；tuple 不可变，拒绝赋值 |

字节串这条最容易被忽略：`len` 与索引都按字节，ASCII 之外的字符串会与 CPython 不一致。要不要改成码点语义（`len` 变成 O(n)、索引要解码）是个待定项，见 [roadmap.md](roadmap.md)。

## 二、尚未实现的构造

这些在 CPython 里合法，在 py23 里分两种情况：

**语法层就拒**（`check` 报错）：`bytes` 字面量、超出 int64 的整数、Python 2 遗留写法、方法缺 `self`、字段赋值类型不符。

**能解析、能过类型检查，但 `build` 报错**：通过 unknown 函数值的 `*` / `**` 展开、`*` 位于固定形参之前且不是定长 tuple、非异常类名当异常用、`finally` 体内跳出 finally 的 `return`/`break`/`continue`、跨模块继承、类装饰器、`@classmethod`、自定义方法装饰器、把类名当值用、函数内定义类、导入类模式的字段子模式、unknown 值上的数值/list/dict/tuple/set 内置类型模式、类体字段默认值里的 lambda，以及生成器的 `yield`/`yield from` 位于 `finally`/`except` 体内（协程的 `await` 同样）、异步生成器表达式、嵌在其它表达式里的异步推导式。

清楚这个分层很重要：`py23 check` 尽量当通用 Python 语法/类型检查器用，检查器放宽的边界都记在 [CHANGELOG 0.6.1](changelog-archive.md)；`py23 build` 才是「能不能编译」的判据。

## 三、验证手段

四层，从便宜到贵：

| 层 | 命令 | 规模 | 防的是什么 |
|---|---|---|---|
| 正例 | `ctest --preset dev` 里 `tests/*.py` | 11 个文件 | 前端退回保守 |
| 反例 | `ctest --preset dev` 里 `tests/fail/*.py` | 32 个文件 | 检查器为了过测而放水 |
| 运行 | `ctest --preset dev` 里 `tests/run/*.py` | 38 个文件 | 后端烂掉没人知道 |
| 模块 | `ctest --preset dev` 里 `tests/modules/*/` | 8 个目录 | 分模块编译/链接、惰性初始化、缓存失效 |
| 未建模 | `ctest --preset dev` 里 `tests/unsupported/*.py` | 2 个文件 | 「标了没建模、实际却悄悄编译过去」 |
| 内存 | 16MB `RLIMIT_AS` 下跑压力程序 | 拼接 / 对象 / 闭包 / 容器 / 切片 / 删除 / 推导式 / 变长实参 / `with` / 装饰器 / 异常展开 / `yield from` / 异步推导式 / 环 / unknown 泄漏 | 引用计数或 GC 漏插桩、重复释放、误回收 |
| 差分 | `python3 tests/diff/run_diff.py` | 随机程序 | 语义漂移、边界错误 |

### 四类测试的写法

- `tests/*.py`：必须通过 `check`。文件里写清这段覆盖什么。
- `tests/fail/*.py`：首行 `# expect: <错误片段>`，`check` 必须报出包含该片段的错。
- `tests/run/*.py`：必须编译成可执行文件并运行，末尾 `# --- expect ---` 之后的注释行是期望输出，逐行比对。
- `tests/unsupported/*.py`：首行 `# unsupported: <标记>`，`check -v` 必须打出该标记，且 `build` 必须失败。

反例和运行用例是重点：只测「能通过」会让检查器越来越松，只测前端会让后端烂掉。

### 差分测试

`tests/diff/` 下六个生成器，覆盖不同侧面：

| 生成器 | 内容 | 每个种子用例数 |
|---|---|---|
| `expr` | 纯表达式：整数/浮点四则、位运算、整除取模 | 60 |
| `struct` | 函数、循环、分支、变量复用 | 40 |
| `float` | 浮点 `//` `%` 的符号修正、`print` 的 `sep`/`end` | 60 |
| `repr` | 随机位模式的 `double`，专测浮点转字符串 | 40 |
| `gen` | 生成器迭代、`next`/`StopIteration`、`close` 清理 | 40 |
| `async` | `asyncio.run`/`sleep(0)`/`gather`/`create_task`、异常跨 await、取消级联、Future、fd poll、异步生成器 + `async for`/`async with` | 30 |

```bash
python3 tests/diff/run_diff.py                 # 3 个种子 = 810 个程序
python3 tests/diff/run_diff.py --seeds 5       # 1350 个程序
python3 tests/diff/run_diff.py --seeds 10      # 2700 个程序，建议每次改后端都跑
python3 tests/diff/run_diff.py --only repr     # 只跑浮点格式化
```

判定标准：py23 退出码必须为 0（编译期错误算失败），stdout 与 CPython **逐字节相同**，stderr 不比（panic 消息是我们自己的）。

### 内存验收

引用计数没有单测框架能直接断言的地方，用受限地址空间跑压力程序：

```bash
./build/dev/py23 build stress.py -o stress
python3 - <<'PY'
import resource, subprocess
def lim():
    resource.setrlimit(resource.RLIMIT_AS, (16 << 20, 16 << 20))
p = subprocess.run(["./stress"], preexec_fn=lim)
print(p.returncode)
PY
```

`tests/run/10_rc.py` 负责别名、字段、列表、返回值、`continue` 跳过清理等
正确性；`tests/run/11_closure.py` 覆盖闭包捕获、`nonlocal`、多实例、多层
嵌套和自递归；`tests/run/12_lambda.py` 覆盖 lambda；`tests/run/14_dict.py`
覆盖字典/集合/`in`/方法；`tests/run/15_slice.py` 覆盖切片；
`tests/run/16_dict_del.py` ~ `tests/run/21_decorator.py` 分别覆盖删除类
方法、切片赋值、变长实参、推导式、`with` 和装饰器；
`tests/run/22_exception.py` 覆盖 try/except/else/finally、raise、层级
匹配、`with` 异常路径和运行时错误转换；`tests/run/23_generator.py` ~
`tests/run/32_exception_value.py` ~ `tests/run/34_exception_chain.py` 覆盖
异常值、内置函数、自定义异常与 `raise ... from`，并连同前面的用例覆盖
生成器、协程、Task、异步语言面、Future/取消/fd/回调、`yield from`、
异步推导式与异步生成器协议。
`tests/run/13_gc.py` 会真正触发收集器：对象环、字典自环、闭包垃圾和
unknown 容器。RSS 压力程序负责「长循环里内存不随迭代数增长」。当前实测
100 万次拼接 / 100 万次对象分配 / 30 万次带字符串字段的对象分配 /
20 万次闭包工厂 / 20 万次对象环 / 20 万次相互递归闭包 / 10 万次字典/集合
创建 / 10 万次切片与容器方法 / 本轮 50 万次新构造循环 + 25 万次异常展开，
峰值 RSS 约 8–9MB。

生成器刻意避开第一节列出的语义差异——那些是已知的、有意的，测了只会制造噪音。差分测试抓的是**我们以为一致、实际不一致**的地方，历史上抓到过：浮点 `%` 的符号、`10.0` 打成 `1e+01`、`-0.0 % 3.0` 的零符号。

### 前端鲁棒性基线

拿 CPython 标准库当输入，看 `check` 的通过率。这不是功能测试，是「别把真实代码判错」的护栏。

```bash
cmake --preset dev && cmake --build --preset dev   # 先构建 build/dev/py23
export PY23="$PWD/build/dev/py23"
cd "$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')"
find . -name '*.py' -print0 | xargs -0 -P 8 -n 1 sh -c \
  '"$PY23" check --syntax "$0" >/dev/null 2>&1 && \
   { "$PY23" check "$0" >/dev/null 2>&1 || echo "FAIL $0"; }' > /tmp/fail.txt

echo "语法通过:"; find . -name '*.py' -print0 | xargs -0 -P 8 -n 1 sh -c \
  '"$PY23" check --syntax "$0" >/dev/null 2>&1 && echo ok' | wc -l
echo "类型失败:"; wc -l < /tmp/fail.txt
```

`check --syntax` 只做语法，`check` 再做类型；统计口径是「语法通过的那些文件里，类型检查也通过的比例」。全量约 7 分钟（本机 8 并发）。

实测（CPython 3.11.1 + 本机 site-packages，0.6.1）：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 1740 | 1289 | 1143 / 1289（88.7%） |
| 标准库 + site-packages | 38582 | 35693 | 29143 / 35693（81.6%） |

**这个数字不能直接和 0.5 的 96.1% 比。** 0.6 之前根本没有类检查，`class` 语句只解析不分析，「通过」是虚高。0.6.0 刚加类检查时失败 **19064** 个文件（通过率 46.6%），把误杀逐条放宽（见 [CHANGELOG 0.6.1](changelog-archive.md)）之后降到 **6550** 个（81.6%）。也就是说，剩下的下降主要是「多了一层以前没有的检查」，不是前端变严。

语法层的失败**全部**是明确拒绝的那几类，没有解析器 bug：

```
bytes 字面量 2038   complex 字面量 620   超过 int64 的整数 138   match 91
Python 2 的 long 后缀 1   旧式 except 参数 1
```

合计 2889 = 38582 − 35693，对得上。

剩下的类型失败按错误类型看（抽 800 个失败文件统计）：

| 错误 | 条数 | 性质 |
|---|---|---|
| `undefined name` | 1700 | 运行时注入：`get_ipython()`、`globals().update(...)`、`exec` 动态生成、测试文件里故意写错的标识符 |
| 对象没有该属性 / 方法 | 201 | 外部类型的成员表看不见（已按 `open` 放过大部分） |
| 实参数目不符 | 117 | 装饰器改变函数签名、元类构造 |
| 其余 | — | 故意写错的测试样本、位运算类型推断、跨函数 `Optional` 收窄 |

`undefined name` 占七成，这是静态分析的固有盲区，再放宽就得牺牲「拼错变量名」这种本来该报的检查。CPython 的标准库测试里专门有一批文件是**故意写错**的（测 NameError / traceback / 解包失败），它们必然进失败清单。

### 语法对照

另有一次性的实验：拿 91 条语法边角用例同时喂给 `py23 check --syntax` 和 CPython 的 `compile()`，5 条不一致，全部是设计性拒绝（bytes / complex / match）或 PEP 701 已经允许而 CPython 3.11 还不认的 f-string 嵌套。这批用例没有进仓库（价值随时间下降），记在这里供参考。

## 四、当前性能

`-O0` 构建的 py23 生成 `-O0` 风格的代码，对手是同机容器内的 CPython 3.13.5：

| 程序 | 相对 CPython 3.13 |
|---|---|
| `fib(35)`（递归 + 调用） | 约 16× |
| 2 亿次 `while` 累加 | 约 33× |
| 2000 万次方法调用 `a.add(1)` | 约 19× |

只看数量级就好：py23 没有任何优化遍，赢在「静态类型 + 直接出机器码」。
0.7 加入引用计数后，整数路径与 0.6.1 持平（不含临时堆值的语句/循环/
函数会把 RC 脚手架删掉）；方法调用因为 self 形参的 inc/dec 慢约 30%。
