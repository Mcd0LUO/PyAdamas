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
$ make -j && ./build/dev/py23 run hello.py
832040
```

比 CPython 3.11 快 17 倍（`fib(35)` 0.06s vs 1.00s），2 亿次整数循环快 116 倍（0.06s vs 7.0s），2000 万次方法调用快 43 倍。代价是类型要在编译期定下来：写了标注的变量不能改类型，`x: int = 1` 之后再 `x = "s"` 会被拒。

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
make -j                 # 开发构建（-O0 -g）→ build/dev/py23，全量约 4s
make MODE=release -j    # 发布构建（-O2）   → build/release/py23
make test               # 正例 + 反例 + 运行用例
```

需要 g++ 13+（`<format>`、`<expected>`、`std::unreachable`）。编译器本身的运行时间只有十几毫秒，所以默认 `-O0`；只有跑分才需要 `MODE=release`。

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

`PY23_KEEP_ASM=1` 会保留中间汇编。出错时输出带源码行和插入符：

```
bad.py:1:7: error: expected parameter name, found ':'
      1 | def f(:
        |       ^
```

## 现在能做什么

标量、字符串、列表、元组、函数（默认参数、递归、一等值）、`class`（字段、方法、单继承、虚调用）、`if` / `while` / `for`（含循环 `else`）、`break` / `continue`、`assert`、`try/finally`、`global`、f-string、`print`（含 `sep=` / `end=`）。

完整的支持清单和语义定义见 [docs/language.md](docs/language.md)。

## 目录

```
src/
  token.h  lexer.h        词法：记号定义、缩进敏感分析器
  ast.h    parser.cpp     语法：AST 节点、递归下降解析器
  typecheck.cpp           类型：规则、推断、内置函数表
  ir.h     lower.cpp      IR：三地址码、AST → IR 降级
  codegen.cpp             x86-64 指令选择、寄存器分配
  main.cpp                CLI：把各阶段串起来
runtime/rt.c              运行时：字符串、列表、对象分配、输出、panic
tests/                    正例 / 反例 / 运行用例 / 差分测试
docs/                     设计与参考文档
tools/                    一次性脚本
```

## 设计要点

一句话版，展开在文档里：

- **不是 SSA**：Python 变量天生可变，先用「可变槽位 + vreg」，SSA 留给优化遍。→ [IR](docs/ir.md#为什么不是-ssa)
- **热变量常驻寄存器**：最热的 5 个局部变量整函数待在 `rbx` / `r12`–`r15`，临时值按块做别名与融合。不做全局图着色，换来的是一条规则就能保证分支、循环、调用全都正确。→ [代码生成](docs/codegen.md#寄存器策略)
- **前端宽、后端严**：`check` 对真实 Python 文件基本不误杀；`build` 才判断能不能真编译。→ [架构](docs/architecture.md#前端宽后端严)
- **类型挂在指令上**：前端推完的类型一路带到后端，宽度选择不靠猜。
- **类布局编译期定死**：对象是 `[vtable][字段…]`，字段偏移和方法槽号都是常量，没有查表也没有哈希。→ [代码生成](docs/codegen.md#对象与虚调用)
- **运行时是纯 C**，只依赖 libc + libm，随产物一起链接。

## 已知限制（摘要）

语法层明确拒绝：`bytes` / `complex` 字面量、超出 int64 的整数、`match` 语句。

能解析、能检查，但 `build` 报错：`try/except`、`with`、闭包、`lambda`、字典/集合、切片、推导式、生成器、`async`、装饰器、`import`、`raise`、`del`、`*args`/`**kwargs`、`in`，以及类的多继承、类体里的非声明语句、带装饰器的方法。类只支持单继承，没有 `super()`、类属性、`@property`；字段可以不写标注，由 `self.x = ...` 推断。

语义上有意与 CPython 不同：列表相等比指针、整数溢出回绕、字符串按字节（`len("中文") == 6`）、读未赋值局部变量得到 0。

完整清单（含原因）在 [docs/compatibility.md](docs/compatibility.md)。

## 验证

| 组 | 数量 | 内容 |
|---|---|---|
| `tests/*.py` | 11 | 前端正例，267 条语句 / 29 个函数 / 1165 个表达式 |
| `tests/fail/*.py` | 32 | 反例，必须报出首行 `# expect:` 指定的错 |
| `tests/run/*.py` | 9 | 编译 + 运行，输出与文件末尾的 `# --- expect ---` 逐行比对 |
| `tests/unsupported/*.py` | 2 | `check` 必须打「没建模」标记，且 `build` 必须拒绝 |
| `tests/diff/` | 4 个生成器 | 随机程序与 CPython 比 stdout，默认 3 种子 600 个程序 |

```bash
make test
python3 tests/diff/run_diff.py --seeds 5     # 1000 个程序，目前零差异
```

前端鲁棒性基线（CPython 3.11 标准库 + site-packages，38582 个 `.py`）：语法通过 35693，其中类型通过 29143（81.6%）；标准库本体 1143 / 1289（88.7%）。0.6 引入类检查后口径变了——刚加上时失败 19064 个文件，逐条去误杀后降到 6550 个，明细见 [compatibility.md](docs/compatibility.md#前端鲁棒性基线)。

ASan + UBSan 下跑全部用例零报告，`-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual` 零警告。

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
| [docs/roadmap.md](docs/roadmap.md) | 下一步做什么、为什么是这个顺序 |

变更记录见 [CHANGELOG.md](CHANGELOG.md)，参与开发见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

尚未确定。在定下来之前，请当作「仅供阅读」的代码。
