# PyAdamas 文档

这套文档回答四个问题：**这门语言是什么**、**编译器怎么实现的**、**为什么这么做**、**怎么改它**。

如果你是第一次打开这个仓库，先读根目录的 [README](../README.md)；README 只有门面，细节都在这里。

## 文档地图

| 文档 | 回答什么问题 | 主要对应的源码 |
|---|---|---|
| [architecture.md](architecture.md) | 整体怎么切分？各阶段之间靠什么契约衔接？ | `src/main.cpp` `src/driver_build.cpp` |
| [language.md](language.md) | 支持哪些语法？类型规则是什么？语义和 Python 哪里不一样？ | `lexer.h` `parser.cpp` `typecheck*.cpp` |
| [ir.md](ir.md) | IR 长什么样？每条指令什么语义？AST 怎么降级成它？ | `ir.h` `lower*.cpp` |
| [codegen.md](codegen.md) | 汇编怎么生成？栈帧和调用约定怎么定的？ | `codegen*.cpp` |
| [runtime.md](runtime.md) | 编译产物运行时依赖什么？对象在内存里怎么摆？ | `runtime/rt_*.c` |
| [compatibility.md](compatibility.md) | 和 CPython 的差异清单，以及正确性是怎么验证的 | `tests/` |
| [development.md](development.md) | 怎么构建、怎么测、怎么调试、加功能要动哪几处 | `CMakeLists.txt` `tests/` |
| [roadmap.md](roadmap.md) | 现在做到哪、下一步做什么、为什么是这个顺序 | — |
| [archive/README.md](archive/README.md) | 已轮转出去的旧文档索引 | — |

变更记录在根目录 [CHANGELOG.md](../CHANGELOG.md)（0.40 及更早归档在
[archive/changelog-0.40.md](archive/changelog-0.40.md)），参与开发的规矩见
[CONTRIBUTING.md](../CONTRIBUTING.md)。

## 按角色选读

**只想用它写程序**：language.md → compatibility.md（看哪些写法会被拒）→ README 的用法一节。

**想改编译器**：architecture.md → 你要动的那一层的文档 → development.md 的「扩展 checklist」。

**想看设计取舍**：architecture.md 的「关键取舍」→ ir.md 的「为什么不是 SSA」→ codegen.md 的「为什么不做全局寄存器分配」。

## 术语

文档里反复出现这几个词，先说清楚：

| 术语 | 含义 |
|---|---|
| **槽位（slot）** | 一个局部变量的固定归属：栈帧里的某个偏移，或被提升后占的常驻寄存器。形参也是槽 |
| **提升（promotion）** | 把局部变量从栈槽挪到常驻寄存器：整型/指针用 callee-saved，浮点只在无调用函数里用 `xmm8`–`xmm15` |
| **vreg** | 虚拟寄存器，三地址码里每个中间值一个编号（dump 里写成 `%3`） |
| **降级（lower）** | AST → IR 的过程，把树形结构拍平成基本块 + 线性指令 |
| **机器类** | 后端关心的粗分类：整数类（i64）、浮点类（f64）、指针类 |
| **unknown** | 类型推断失败时的占位类型；任何涉及它的检查都放行 |
| **vtable** | 每个类一张静态方法表，对象头第一个字段指向它；槽号在编译期确定 |
| **RC 头** | 堆对象前面 64 字节的隐藏头（注册表、rc、dtor/release、GC 元数据）；负载指针和字段偏移不变 |
| **临时值栈** | 运行时保存表达式临时堆值的栈，按 mark 在语句 / 循环 / 函数边界清理 |
| **cell** | 被闭包捕获的局部变量的存储，就是一个单元素 `list[T]` |
| **闭包对象** | 函数值的运行时形态：`[code, n_caps, cell…]`；无捕获函数用静态闭包 |
| **GC / 循环收集** | 保守标记-清扫，回收 RC 处理不了的环和 unknown 引用；按存活堆大小自适应阈值触发（下限 2MB）；完全标注的程序里字符串/无堆字段实例不进链，容器堆走无表快速路径 |
| **脱糖** | 解析后把 `lambda` 改写成合成嵌套 `def` 再进入类型检查 |
| **字典索引表** | 字典 = 密集 entries（插入顺序）+ int32 开放寻址索引；查找先走 4 字节索引，扩容只重建索引 |
| **切片哨兵** | 缺省切片边界传 `INT64_MIN` 给运行时，由 `slice.indices` 归一化 |
| **基线** | 拿 CPython 标准库当输入跑前端得到的通过率，用来防回归 |

## 文档的维护规则

文档和代码不同步比没有文档更糟。以下改动**必须**同步对应文档，否则视为没做完：

| 改动 | 必须更新 |
|---|---|
| 新增 / 修改 IR 指令、`RtFn`、降级规则 | ir.md（指令表 + 降级规则） |
| 新增 / 修改运行时函数、对象布局 | runtime.md（函数表 + 布局） |
| 改调用约定、栈帧布局、槽位复用策略 | codegen.md |
| 改语言语义、类型规则、拒绝清单 | language.md；影响兼容性的一并更新 compatibility.md |
| 改测试的数量或格式 | development.md 的测试一节、README 的验证表 |
| 完成 roadmap 上的一项 | roadmap.md 移到「已完成」，CHANGELOG.md 记一条 |

写文档的硬要求：**不确定的数字不要写**，能跑一遍验证的就跑一遍。README 和 compatibility.md 里的每个数字都应该能由文档中给出的命令复现。

## 当前状态

对应 CHANGELOG 的 0.153：流水线 `源码 → 词法 → AST → lambda 脱糖 → 类型检查 → IR → x86-64 汇编 → 可执行文件` 已打通。语言面支持标量、字符串、`bytes`（含迭代与 `%` 格式化）、列表/元组/字典/集合、切片读写、闭包/lambda/`*args`/`**kwargs`/装饰器、推导式、生成器表达式（惰性）、`with`、异常值（`args`/`raise ... from`/自定义异常）、类（字段/方法/C3 多继承/零参 `super()`/`@staticmethod`/`@classmethod`/`@property`/类属性/类装饰器/类名当值）、`match/case`、`del`、复数字面量、f-string 格式说明、dunder 协议（含装饰 dunder、`__call__` 与方法值）、`Generator[Y, S, R]` 标注、`async for` 自定义 `__aiter__`、`open`/`input`/`sys.stdin`、`struct` 值类型与 `cimport`/`cdecl` C 互操作（含 `ptr[T]` 缓冲区、回调、可变参数、结构体按值）；标准库内置 `math`/`sys`/`asyncio`/`random`/`os`/`os.path`/`subprocess`/`time`/`itertools`/`collections`/`functools`/`json`/`string`/`re`/`datetime`/`pathlib`/`csv`/`base64`/`binascii`/`heapq`/`bisect`/`hashlib`/`hmac`/`logging`/`statistics`/`textwrap`/`operator`，以及封装 OpenBLAS 的 `linalg`。差分 runner 已自举（840 例）；运行用例 151 个、反例 52 个、未建模 1 个、模块目录 12 个，另有 `tests/bench/` 标准库基准。剩余主要缺口：asyncio 边角、调试信息与 traceback。GC 的保守性和回收时机见 [compatibility.md](compatibility.md)。
