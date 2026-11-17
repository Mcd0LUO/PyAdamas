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
| [stack-alignment.md](stack-alignment.md) | 栈对齐违规查到了哪两处 ABI 根因？`-mstackrealign` 为什么能撤掉？ | `src/codegen_block.cpp` `src/codegen.cpp` |
| [runtime.md](runtime.md) | 编译产物运行时依赖什么？对象在内存里怎么摆？ | `runtime/rt_*.c` |
| [compatibility.md](compatibility.md) | 和 CPython 的差异清单，以及正确性是怎么验证的 | `tests/` |
| [development.md](development.md) | 怎么构建、怎么测、怎么调试、加功能要动哪几处 | `CMakeLists.txt` `tests/` |
| [roadmap.md](roadmap.md) | 现在做到哪、下一步做什么、为什么是这个顺序 | — |
| [zero-cost-exceptions.md](zero-cost-exceptions.md) | 零开销异常怎么做？LSDA / personality 的实测格式与 Z1-Z5 路线 | `runtime/rt_personality.c` `tests/exc/` `tools/lsda_ref/` |
| [exc-semantics.md](exc-semantics.md) | 现行 try/except/finally/with 语义到底是什么？改成表驱动会丢什么？ | `runtime/rt_exc.c` `src/lower_exc.cpp` |
| [exc-frame-cleanup.md](exc-frame-cleanup.md) | 展开 / longjmp 时被跳过帧的局部引用怎么回收？ | `runtime/rt_exc.c` `runtime/rt_core.c` |
| [regalloc.md](regalloc.md) | 寄存器分配为什么没带来性能收益？下一步两条路线怎么选？ | `src/codegen_regalloc.cpp` |
| [regalloc-architecture.md](regalloc-architecture.md) | 精确 LiveInterval / SpillPlacement / SplitKit 三层怎么设计？池容量实验后为什么第三层正式放弃？ | `src/codegen_regalloc.cpp` |
| [bounds-elimination-attempt.md](bounds-elimination-attempt.md) | 边界检查消除（ranged loop versioning）做到哪一步、克隆阶段为什么回退？上界收益是多少？ | `src/opt_loop.cpp` |
| [heap-slot-promotion.md](heap-slot-promotion.md) | 堆槽进寄存器的 GC 障碍假设为什么是错的？放行后为什么反而慢 5.5%？ | `src/lower_impl_slots.cpp` `runtime/rt_core.c` |
| [ivsr-negative-result.md](ivsr-negative-result.md) | 归纳变量强度削减（IVSR）为什么在 nbody 上运行时间退步 15.7%？ | `src/opt_loop.cpp` |
| [str-codepoints.md](str-codepoints.md) | 字符串码点语义 S1/S2 做了什么、S3 还剩什么？ | `runtime/rt_str.c` `src/lower_builtin.cpp` |
| [stdlib-gaps.md](stdlib-gaps.md) | 51 个常用 CPython 模块哪些可用、哪些卡在编译器能力上？ | `stdlib/*.py` `src/driver_build.cpp` |
| [archive/README.md](archive/README.md) | 已轮转出去的旧文档索引 | — |

其中 [regalloc-architecture.md](regalloc-architecture.md) 的 §8、[bounds-elimination-attempt.md](bounds-elimination-attempt.md)、
[heap-slot-promotion.md](heap-slot-promotion.md)、[ivsr-negative-result.md](ivsr-negative-result.md) 是**有意保留的负面结论**
（方案未落地或已回退），不要当成待办去「补完」。

变更记录在根目录 [CHANGELOG.md](../CHANGELOG.md)（0.41–0.99 归档在
[archive/changelog-0.41-0.99.md](archive/changelog-0.41-0.99.md)，0.40 及更早见
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
| **GC / 循环收集** | 分代收集（minor trial-deletion + major 标记-清扫），回收 RC 处理不了的环和 unknown 引用；按存活堆大小自适应阈值触发（下限 2MB）；完全标注的程序里字符串/无堆字段实例不进链，容器子节点按静态类型直接标记，纯类型化帧不扫整段栈 |
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

对应 CHANGELOG 的 0.242：流水线 `源码 → 词法 → AST → lambda 脱糖 → 类型检查 → IR → x86-64 汇编 → 可执行文件` 已打通。语言面支持标量、字符串、`bytes`、列表/元组/字典/集合、切片读写、闭包/lambda/`*args`/`**kwargs`/装饰器、推导式、生成器表达式（惰性）、`with`、异常值、类（字段/方法/C3 多继承/`super()`/各类装饰器/类名当值）、`match/case`、`del`、复数、f-string 格式说明、dunder 协议、`Generator[Y, S, R]` 标注、`async for` 自定义 `__aiter__`、`open`/`input`/`sys.stdin`、`struct` 值类型与 `cimport`/`cdecl` C 互操作；标准库内置 28+ 模块及封装 OpenBLAS 的 `linalg`。差分 runner 已自举（840 例）；运行用例 188 个、正例 12 个、反例 53 个、未建模 1 个、严格用例 6 个、模块目录 15 个，`tests/run_tests.sh` 全量 **205 ran ok**（188 运行 + 15 模块 + 1 cdecl + 1 cfi）；另有 `tools/difftest.sh` 的 O0/O2 自差分、`tests/bench/` 标准库基准和 [四语言经典基准](bench/compare/RESULTS-release.md)。**内存层面：ASan 全量 205 ran ok，泄漏扫描 0 处 / 188 用例**（0.217 首次扫描为 23 处，0.233 清零），诊断工具见 `runtime/rt_diag.c` 与 [development.md](development.md)。剩余主要缺口：asyncio 边角、异常对象 traceback / 列号 / `.debug_info`、字符串码点语义 S3、依赖动态属性的 stdlib（`argparse`/`dataclasses`/`contextlib`，见 [stdlib-gaps.md](stdlib-gaps.md)）。性能瓶颈的归因已按实测修正为**顶层变量是全局、不参与寄存器分配**（不是「调用密度太高」），两条候选路线见 [regalloc.md](regalloc.md) 与 [roadmap.md](roadmap.md)。GC 的保守性和回收时机见 [compatibility.md](compatibility.md)。

历次里程碑（完整版见 [CHANGELOG.md](../CHANGELOG.md)）：0.158–0.171 前端基线 281 → 501 / 596；0.172 修 handler 记录空闲链表；0.173 新增 Windows 目标；0.174–0.197 打通零开销异常 Z1–Z4；另有 GC 根扫描收窄、字符串码点语义（0.186）、间接调用统一 ABI（0.188）；0.198–0.202 完成 **Z5**：表驱动成为默认路径，`_setjmp` 670 → 361（-46%），可挂起函数永久保留 setjmp 是设计上的降级，`--no-zero-cost-exc` 可回退；0.203–0.204 落地寄存器分配 R1–R4（活跃性 + 线性扫描 + caller 池 + 浮点线性扫描），正确性收益明确但性能收益为零；同期恢复四语言经典基准（[结果](bench/compare/RESULTS-release.md)）：9 用例全部标注、输出与 CPython 逐字节一致（详见 [zero-cost-exceptions.md](zero-cost-exceptions.md)）。0.205–0.209 IR 重构四步（稳定指令/块 ID、use-def 索引、tag 操作数、DCE 改用索引）并记录「暂不上 SSA」的取舍；0.210–0.213 修四个正确性 bug（>6 参数含浮点的间接调用丢实参、堆槽 LICM 外提、RcMark 寄存器化、切片赋值缺类型检查）；0.214 `join(T, None)` 返回 `Optional[T]`（热循环 4.7×）；0.215–0.216 根治两类 unknown 值 use-after-free；0.217–0.218 ASan 成为一等公民并覆盖运行时内部；0.219–0.233 **泄漏从 23 处清到 0**，新增 `rt_diag.c` 诊断模块；0.234–0.242 **RC/GC 性能优化**：GC 零回收退避（容器段 major 8211 → 22）、容器逐元素堆位图、按位置放宽 exact-RC（用例 41 → 50）、按需关闭索引维护（`bench_rc_gen` 0.75x → 0.90x）、Go pacer 公式（GC 次数 34 → 7）、增量清扫（最大停顿 5.09ms → 1.68ms，-67%）、inc/dec 内联（符号调用 239 → 0）、RAII 作用域守卫（`jp_value` 手工 pin 4 → 0）。**性能归因修正**：原写的「`__pa_main` 调用密度过高（42 块 / 106 call）」描述的是 0.204 之前的 matmul，实测顶层调用几乎全是 `call.rt`（用户调用 0–2 个），真障碍是顶层变量为全局、不在只处理 slot 的分配器候选集里——见 [regalloc.md](regalloc.md)。
