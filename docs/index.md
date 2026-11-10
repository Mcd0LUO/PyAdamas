# py23 文档

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
| [development.md](development.md) | 怎么构建、怎么测、怎么调试、加功能要动哪几处 | `Makefile` `tests/` |
| [roadmap.md](roadmap.md) | 现在做到哪、下一步做什么、为什么是这个顺序 | — |

变更记录在根目录 [CHANGELOG.md](../CHANGELOG.md)（0.40 及更早归档在
[changelog-archive.md](changelog-archive.md)），参与开发的规矩见
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
| **提升（promotion）** | 把局部变量从栈槽挪到 callee-saved 寄存器，整函数有效，不做活跃性分析 |
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
| **order 数组** | 字典里按插入顺序记录槽号的数组；`for k in d` 和 `len` 用 |
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

对应 CHANGELOG 的 0.38（块内临时寄存器、range(len) 下标免检查、检查型下标外提、循环不变量外提、静态调用直接化、小函数内联、IR 遍与 AVX2 归约、-O0/O1/O2、C3 MRO、super、方法装饰器与 match 边角）：流水线 `源码 → 词法 → AST → lambda 脱糖 → 类型检查 → IR → x86-64 汇编 → 可执行文件` 已打通，支持标量、字符串、列表、元组、字典、集合、切片（读与赋值）、删除类方法、函数（默认参数、递归、闭包、`nonlocal`、`lambda`、`*args`/`**kwargs`、装饰器）、生成器（`yield`/`yield from`/`send`/`close`/跨挂起清理）、协程（`async def`/`await`/`asyncio.run`/`sleep`/`gather`/`create_task`/`Future`（多等待者、`set_exception`、完成回调），Task 支持 `done()`/`cancel()`/`result()`/`exception()` 和取消级联）、异步生成器协议（`asend`/`athrow`/`__anext__`/`aclose`）与 `async for`/`async with`、fd 等待与 poll、推导式（含异步推导式）、`with`、`try/except/else/finally`、`raise`、类（字段、方法、C3 多继承、零参 super、静态方法、属性）、控制流、`print`。异常是带 `args` 的值对象（控制流仍用 setjmp/longjmp + handler 栈展开）；内置函数支持 abs/min/max/sum/pow/any/all/complex；支持 del、print(*args)、复数字面量、match/case（类模式、值模式、`as`、`**rest`、异常类模式、导入类、捕获赛后可见、运行期 isinstance）、f-string 格式化、多继承与类体语句；自定义异常类与 `raise ... from` 已接通；堆值用引用计数回收，环和 unknown 泄漏由保守 GC 兜底。本地包/相对导入、内置 math/sys/asyncio 已支持；自定义 `__aiter__`、fd 读写/子进程尚未生成代码；GC 的保守性和回收时机见 [compatibility.md](compatibility.md)。
