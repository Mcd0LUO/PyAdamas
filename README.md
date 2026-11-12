# py23

用 C++23 写的 Python 语法外壳编译器。**词法 → AST → 类型检查 → IR → x86-64 汇编 → 可执行文件**，整条链路自己实现，不生成 C。

目标不是兼容 CPython，而是借 Python 的语法做一门静态类型的编译型语言：类型在编译期定死，值不装箱，调用就是一条 `call`。

```python
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(30))
```

```bash
$ cmake --preset dev && cmake --build --preset dev && ./build/dev/py23 run hello.py
832040
```

同一台机器、`gcc:14` 容器内实测，比 CPython 3.13 约快 16 倍（`fib(35)`）、33 倍（2 亿次整数循环）、19 倍（2000 万次方法调用）。绝对耗时随机器和负载浮动，倍数基本稳定。代价是类型要在编译期定下来：写了标注的变量不能改类型，`x: int = 1` 之后再 `x = "s"` 会被拒。

类和字段也一样：布局在编译期定死，属性访问退化成一条固定偏移的 `movq`。

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

## 构建

```bash
cmake --preset dev                 # Debug + Ninja → build/dev/py23
cmake --build --preset dev
cmake --preset release             # Release      → build/release/py23
cmake --build --preset release
ctest --preset dev                 # 正例 + 反例 + 运行 + 模块 + 缓存
```

需要 CMake 3.28+、Ninja、g++ 13+（`<format>`、`<expected>`、`std::unreachable`）。`build/dev/compile_commands.json` 会生成给 clangd/IDE。编译器本身的运行时间只有十几毫秒，所以 dev 用 Debug；只有跑分才需要 release。模块缓存各自在 `build/<preset>/modcache`，清缓存用 `cmake --build build/dev --target clean-modcache`。

## 用法

```bash
py23 check    script.py   # 词法 + 语法 + 类型检查
py23 check --syntax script.py   # 只做词法/语法
py23 types    script.py   # 打印顶层名字的推断类型
py23 ast      script.py   # 打印语法树
py23 tokens   script.py   # 打印 token 流
py23 ir       script.py   # 打印降级后的 IR
py23 asm      script.py   # 打印生成的汇编
py23 build    script.py [-o out]   # 编译链接成可执行文件
py23 run      script.py   # 编译、链接并运行（产物是临时文件，跑完即删）
```

生成代码优化等级：`-O0` 基础 / `-O1` 常规（下标内联、槽融合、浮点提升、
循环免检查（含 `for k in range(len(a))` 的 `a[k]`）、块内临时寄存器）/
`-O2`（默认，再加 IR 常量折叠/DCE/静态小函数内联/静态调用直接化/循环不
变量外提/检查型下标外提、AVX2 int 归约、跳转消除、call 前 eax 清零裁剪、
序言清零裁剪）。`--fast-math` 额外允许 float 归约用 SIMD 分组求和（可能
改变末位舍入）。模块缓存按等级和 fast-math 区分。

`PY23_KEEP_ASM=1` 会保留中间汇编。出错时输出带源码行和插入符：

```
bad.py:1:7: error: expected parameter name, found ':'
      1 | def f(:
        |       ^
```

## 现在能做什么

标量、字符串、**bytes**、列表、元组、**字典 / 集合**（字面量、下标读写、`len`、`in`、`for`、`get`/`keys`/`values`/`items`/`update`/`add`、删除类方法）、**切片（读与赋值）**、函数（默认参数、递归、一等值、闭包、`nonlocal`、`lambda`、`*args`/`**kwargs`、装饰器）、推导式（含异步推导式）、`with`、`try/except/else/finally`、`raise`（异常值、`args`、`raise ... from`、自定义异常）、本地包/相对导入（`.py` → `.py23i` + `.o`，惰性初始化、增量缓存）、`class`（字段、方法、C3 多继承、零参 `super()`、`@staticmethod`、`@classmethod`、`@property`、类属性）、`if` / `while` / `for`（含循环 `else`）、`break` / `continue`、`assert`、`global`、`del`、`match/case`、复数字面量、f-string（含格式说明）、dunder 协议、`open`/`input`/`sys.stdin`、`print`（含 `sep=` / `end=`）。生成器支持 `yield from`；协程支持 `async def` / `await` / `asyncio.run` / `sleep` / `gather` / `create_task`、`Future`、异步生成器（`async for` / `async with`、`asend` / `athrow` / `__anext__` / `aclose`）。标准库内置 `math`/`sys`/`asyncio`/`random`/`os`/`os.path`/`subprocess`/`time`/`itertools`/`collections`/`functools`/`json`/`string`/`re`/`datetime`/`pathlib`。堆值用引用计数自动回收，循环收集器回收环和 unknown 容器泄漏；不含堆临时值的代码不会插入任何 RC 指令。

完整的支持清单和语义定义见 [docs/language.md](docs/language.md)。

## 目录

```
src/
  token.h  lexer.h        词法：记号定义、缩进敏感分析器
  ast.h    parser.cpp     语法：AST 节点、递归下降解析器
  desugar.cpp             lambda 脱糖成合成嵌套 def
  typecheck*.cpp          类型：规则、推断、内置函数表（按语句/表达式/调用/内置拆分）
  ir.h     lower*.cpp     IR：三地址码、AST → IR 降级（按语句/表达式/调用/内置/方法/打印/推导式/导入/匹配/生成器拆分）
  opt*.cpp                O2 优化：折叠/内联/循环不变外提/下标版本化
  codegen*.cpp            x86-64：寄存器分配、基本块、指令选择、调用
  main.cpp                CLI 分派与前端入口
  driver_build.cpp        多模块解析、增量缓存、汇编与链接
runtime/
  rt_internal.h           运行时内部接口：类型、全局状态、内部函数声明
  rt_core.c rt_exc.c      引用计数/GC/闭包、异常与 handler 栈
  rt_gen.c rt_agen.c      生成器帧、异步生成器驱动
  rt_async.c rt_task.c    事件循环/协程、Task 与 Future、fd 等待
  rt_str.c rt_list.c      字符串/字符串方法、列表/排序/归约
  rt_dict.c rt_slice.c    字典/集合、查找/切片/range
  rt_class.c rt_complex.c 对象布局/类型检查、复数
  rt_math.c rt_format.c   math/sys/panic、f-string 格式化与 repr
  rt_file.c               文件对象与文本编码
  rt_random.c rt_os.c     MT19937、目录/环境
  rt_subprocess.c         子进程 run
  rt_time.c               时间与 sleep
  rt_stdlib.c             itertools/collections
  rt_functools.c          functools.reduce 与回调跳板
  rt_json.c               json dumps/loads
  rt_re.c                 re 回溯 VM、Pattern/Match
tests/                    正例 / 反例 / 运行用例 / 差分测试
docs/                     设计与参考文档（旧文档在 docs/archive/）
tools/                    一次性脚本
```

## 设计要点

一句话版，展开在文档里：

- **不是 SSA**：Python 变量天生可变，先用「可变槽位 + vreg」，SSA 留给优化遍。→ [IR](docs/ir.md#为什么不是-ssa)
- **热变量常驻寄存器**：最热的 5 个整型/指针变量整函数待在 `rbx` / `r12`–`r15`，无调用函数里的浮点槽用 `xmm8`–`xmm15`；临时值按块做别名与融合。不做全局图着色，换来的是一条规则就能保证分支、循环、调用全都正确。→ [代码生成](docs/codegen.md#寄存器策略)
- **前端宽、后端严**：`check` 对真实 Python 文件基本不误杀；`build` 才判断能不能真编译。→ [架构](docs/architecture.md#前端宽后端严)
- **类型挂在指令上**：前端推完的类型一路带到后端，宽度选择不靠猜。
- **类布局编译期定死**：对象是 `[RC 头][vtable][字段…]`，字段偏移和方法槽号都是常量，没有查表也没有哈希。→ [代码生成](docs/codegen.md#对象与虚调用)
- **引用计数不查栈**：所有权由静态类型决定，表达式临时值进运行时临时值栈；纯整数语句、循环、函数的 RC 脚手架在降级期删掉。→ [运行时](docs/runtime.md#内存模型)
- **闭包是 cell + 闭包对象**：被捕获变量转成单元素列表，闭包存代码和 cell 指针；函数值是一等堆值，静态闭包永生。`lambda` 脱糖成嵌套 `def`。→ [代码生成](docs/codegen.md#闭包)
- **RC + 保守循环收集**：堆对象进注册表，分配超过 2MB 触发标记-清扫；根来自全局表、临时栈、真实栈和 callee-saved 寄存器。误报只延迟回收，不提前释放。→ [运行时](docs/runtime.md#循环收集器)
- **字典是开放寻址表**：`order` 数组保留插入顺序，`set` 复用同一结构；key/value 按静态类型接入 RC，未知类型交给 GC。字符串 key 按字节哈希、逐字节判等。删除按 `order` 剩余项整表重建。→ [运行时](docs/runtime.md#字典--集合)
- **语法糖各归其位**：`lambda` 在类型检查前脱糖成嵌套 `def`，推导式在降级期展开成独立作用域的循环，`with` 在正常/跳转/异常出口补 `__exit__`，装饰器按逆序做普通闭包调用。→ [IR](docs/ir.md#降级规则)
- **异常用 setjmp/longjmp 展开**：`try`/`with`/`finally` 压运行时 handler 记录，`raise` 一次 `longjmp` 到最内层 landing pad；含 handler 的函数关闭热变量寄存器提升，异常跳过的局部引用交给保守 GC。→ [代码生成](docs/codegen.md#异常展开)
- **生成器是状态机 + 堆帧**：含 `yield` 的函数拆成创建包装和 resume，局部槽位在堆帧里；yield 时把 handler 记录摘下，恢复时重新 setjmp；`close()` 和引用计数归零会按挂起点快照跑 `finally` / `__exit__`。→ [运行时](docs/runtime.md#生成器)
- **协程复用同一套状态机**：`async def` 的帧多一个 kind 标记，`await` 是第三种挂起；`asyncio.run` 驱动就绪队列 + 定时器，`gather` 并发启动，`create_task` 立即调度并可 `cancel()`（沿 await 链级联取消），`Future` 支持多等待者、`set_result`/`set_exception`/`add_done_callback`，`wait_readable`/`wait_writable` 把 fd 交给事件循环 `poll`；异常通过帧内注入跨 await 传播；异步生成器 + `async for` / `async with` / `asend` / `athrow` / `aclose` 也已接通。→ [运行时](docs/runtime.md#协程与事件循环)
- **模块独立编译 + 链接**：本地 `.py` 模块与内置 `math`/`sys`；每个 `.py` 编译成独立 `.o` 和二进制 `.py23i` 接口，入口与依赖按 import 图链接；初始化惰性且只执行一次，源码哈希 + 依赖接口哈希决定是否复用缓存。→ [路线图](docs/roadmap.md)
- **运行时是纯 C**，只依赖 libc + libm，随产物一起链接。

## 已知限制（摘要）

语法层明确拒绝：超出 int64 的整数、`0x1L` 这类 Python 2 写法、PEP 695 泛型语法等少数边角。

能解析、能检查，但 `build` 报错：通过 unknown 函数值的 `*` / `**` 展开、非异常类名当异常用、导入类模式的字段子模式和 unknown 值上的数值/list/dict 内置类型模式、类装饰器与自定义方法装饰器。`finally` 体里跳出 finally 的 `return` / `break` / `continue` 也会被拒。生成器已支持 `yield` 与 `yield from`，但 `yield`/`yield from` 只能作为语句或赋值右值，不能位于 `finally`/`except` 体内；生成器表达式是惰性的（脱糖成合成生成器函数，`sum`/`min`/`max`/`any`/`all` 流式消费），只有类体里仍按 list 构造。协程已支持 `async def` / `await` / `asyncio.run` / `sleep` / `gather` / `create_task`（`done()` / `cancel()`，取消级联）、`Future`、异步生成器（`async for`、`async with`、`asend`/`__anext__`/`aclose`）、`wait_readable`/`wait_writable`、Future 回调与 `set_exception`（回调同步执行，`set_exception` 接收异常实例或消息）、`athrow`，但 `await` 只能作为语句、赋值右值或 `return` 值，异步推导式只能作为语句、赋值或 `return` 的根表达式。类支持多继承（C3 MRO；附加基类可复用主基类已有字段）、零参 `super().m()` / `super().__init__()`、`@staticmethod`、`@property`/setter/deleter、类属性、`@classmethod`；没有类装饰器；字段可以不写标注，由 `self.x = ...` 推断。闭包、`lambda`、`*args`/`**kwargs`、推导式、`with`、函数装饰器、`try/except/else/finally`、`raise`、`del`、`match/case`、复数字面量、字典/集合、`in`、容器方法与删除、切片（读与赋值）已支持；循环收集器是保守的，可能延迟回收，但没有 `__del__`/弱引用。异常值是对象：`except ... as e` 得到一个带 `args` 的异常值，支持 `raise e`、`raise ... from`、`__cause__`、`str(e)` 和 `ValueError("msg")` 构造；自定义异常类继承内置异常（类体目前只允许 `pass`）；`args` 只保存消息，没有 traceback。import 支持本地包与相对导入（需要 `__init__.py`）：`import pkg`、`import pkg.mod as m`、`from pkg import mod`、`from .mod import x`、`from ..pkg import y`，以及静态展开的 `from m import *`（按非下划线名字导出，暂不解析 `__all__`）；跨模块继承还没接；已内置 `math`/`sys`/`asyncio`/`random`/`os`/`os.path`/`subprocess`/`time`/`itertools`/`collections`/`functools`/`json`/`string`/`re`/`datetime`/`pathlib`，`csv` 尚未实现；导入类可以构造、访问字段和调用方法，但类类型出现在跨模块函数签名里会降级成 unknown。空字典的 key 类型是 unknown，内容相同的动态字符串 key 可能按机器字不等；混合 value 类型会退化成 unknown；`keys()` / `values()` / `items()` 返回列表而不是视图。

语义上有意与 CPython 不同：列表相等比指针、整数溢出回绕、字符串按字节（`len("中文") == 6`）、读未赋值局部变量得到 0。引用计数处理不了的环和 `unknown` / `list[unknown]` / 混合元组引用由保守 GC 兜底；误报只延迟回收，没有 `__del__`/弱引用。

完整清单（含原因）在 [docs/compatibility.md](docs/compatibility.md)。

## 验证

| 组 | 数量 | 内容 |
|---|---|---|
| `tests/*.py` | 11 | 前端正例，275 条语句 / 37 个函数 / 1203 个表达式 |
| `tests/fail/*.py` | 37 | 反例，必须报出首行 `# expect:` 指定的错 |
| `tests/run/*.py` | 81 | 编译 + 运行，输出与文件末尾的 `# --- expect ---` 逐行比对 |
| `tests/unsupported/*.py` | 1 | `check` 必须打「没建模」标记，且 `build` 必须拒绝 |
| `tests/diff/` | 6 个生成器 | 随机程序与 CPython 比 stdout，默认 3 种子 810 个程序 |
| `tests/modules/` | 9 个目录 | 多模块编译 + 运行 + 二次构建缓存命中 |
| 内存验收 | — | 16MB 地址空间限制下跑拼接 / 对象 / 闭包 / 容器 / 切片 / 删除 / 推导式 / 变长实参 / `with` / 装饰器 / 异常展开 / 生成器 / `yield from` / 协程 / Task / Future / fd 等待 / 异步生成器 / 异步推导式 / 环 / unknown 泄漏 |

```bash
ctest --preset dev
./build/dev/py23 build -o /tmp/run_diff.prog tests/diff/run_diff.py
/tmp/run_diff.prog --py23 build/dev/py23 --cpython python3 --seeds 3   # 810 个程序
```

前端鲁棒性基线（CPython 3.11 标准库 + site-packages，38582 个 `.py`）：语法通过 35693，其中类型通过 29143（81.6%）；标准库本体 1143 / 1289（88.7%）。0.6 引入类检查后口径变了——刚加上时失败 19064 个文件，逐条去误杀后降到 6550 个，明细见 [compatibility.md](docs/compatibility.md#前端鲁棒性基线)。

ASan + UBSan + LSan 下跑全部用例零报告（含生成代码的 RC、闭包、GC、字典、切片、删除、推导式、`with`、装饰器、变长实参、异常展开、生成器挂起/恢复、`yield from` 委托、协程调度、Task 取消级联、Future 多等待者、fd 轮询与异步推导式路径），`-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual` 零警告。内存验收在 16MB `RLIMIT_AS` 下完成，对象环 / 字典环 / unknown 泄漏 / 闭包垃圾 / 容器与切片 / 异常展开的峰值 RSS 约 8MB。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/index.md](docs/index.md) | 文档地图、术语、维护规则 |
| [docs/architecture.md](docs/architecture.md) | 流水线、模块职责、错误处理、关键取舍 |
| [docs/language.md](docs/language.md) | 语法子集、类型系统、求值语义 |
| [docs/ir.md](docs/ir.md) | IR 指令表、不变量、降级规则 |
| [docs/codegen.md](docs/codegen.md) | 栈帧、调用约定、指令选择、汇编坑 |
| [docs/runtime.md](docs/runtime.md) | 对象布局、运行时函数表、内存模型 |
| [docs/compatibility.md](docs/compatibility.md) | 与 CPython 的差异、验证手段与基线 |
| [docs/development.md](docs/development.md) | 构建、测试、调试、扩展 checklist |
| [docs/roadmap.md](docs/roadmap.md) | 当前状态、下一步、技术债 |
| [docs/archive/README.md](docs/archive/README.md) | 已轮转出去的旧文档 |
| [docs/roadmap.md](docs/roadmap.md) | 下一步做什么、为什么是这个顺序 |

变更记录见 [CHANGELOG.md](CHANGELOG.md)，参与开发见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

Apache License 2.0，见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。
