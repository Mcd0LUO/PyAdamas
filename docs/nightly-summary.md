# 夜间迭代与语义审计总结（2026-11-22 22:00 → 2026-11-23 13:30，0.408–0.446）

## 目标（用户授权）

1. 完成模块化拆分；2. 继续做通用优化；3. 先写路线图再按图执行；4. 每次一个代理槽位；
5. 08:00 前结束夜间段，白天继续语义审计与收尾（0.416–0.446）。

## 结果总览

| 项 | 结果 |
|---|---|
| 版本条目 | 夜间 0.408–0.415 + 白天 0.416–0.446（共 **39 条**，CHANGELOG） |
| 测试规模 | 369 → **409** 用例；difftest 348 → **386** |
| 最终闸门 | **409 ran ok / difftest 386-0 / ASan 409-0 报告 / check_size 通过** |
| 模块化 | ✅ 完成（VectPlanner 拆分，codegen_vect.cpp 1178 → 59 行） |
| 通用优化 | ✅ 向量化从 3 基准/8 循环 → **4 基准/9 循环**；nbody −45.31%、spectral_norm −61.75% |
| 修的 bug | **30 个**（多数是静默错值/崩溃；含 1 个异常控制流错误、1 个 pass 从未生效、3 个 unknown 域 dunder 身份语义、7 个 0.437–0.445 修复；另 0.421 是我引入的崩溃，已回退。计 30 时含 4 个「文档承诺/语义缺口」记录项：未初始化变量读 0、装饰后 `__iter__`、`asyncio.run`(None) 打印 0、`min/max` 混合类型） |
| 语义审计 | unknown 语义 / 算术与转换 / 切片 / 字符串 / 异常 / 迭代协议 / OOP / 容器方法 / 推导式与作用域 / 模块与导入 / 文件 I/O / 数字格式化 / 装饰器与属性 / RC·GC / stdlib 两轮，共 22 轮；迭代协议、OOP、stdlib 续集未发现代码 bug |

## 一、模块化（阶段 A）

`AsmGen` 的向量化器整体拆成协作对象 `VectPlanner`：

| 文件 | 行数 |
|---|---|
| `vect_planner_loop.cpp` | 1098（`try_loop` 主体） |
| `vect_planner.cpp` | 877 |
| `vect_planner.h` | ~330 |
| `codegen_vect.cpp` | **59**（只剩一行转发） |

**重编译收益（实测，关 ccache）**：

| 改动 | TU | 耗时 |
|---|---|---|
| `vect_planner_loop.cpp` | **1** | **7.76 s** |
| `codegen_core.h`（对照） | 31 | 55.16 s |

比例 **1 : 7**，达成设计目标。

**等价证据**：整个搬迁过程中 matmul / spectral_norm 的 -O2 汇编 md5 一次未漂移
（`6eee8414…` / 由 58be63a7 变为 3389dfe8 是后续能力的预期变化）。

## 二、通用优化（阶段 B）

### 2.1 向量化覆盖（0.423 后）

| 项 | 夜间基线 | 现在 |
|---|---|---|
| 有触发的基准 / 循环 | 3 个 / 8 个 | **4 个 / 9 个** |
| 触发记录（日志行数口径） | — | **11 条**：matmul 4、spectral_norm 4、nbody 2、sieve 1（按循环计 9 个：matmul 2、spectral_norm 4（运行时 2）、nbody 2、sieve 1） |
| 0 触发基准 | 9 个 | **8 个**（binary_trees / fib 无循环块，其余 6 个各有结构性阻塞点） |

sieve 的计数循环在 0.423 向量化（`while i < N: if flags[i] == 1: count += 1; i += 1`，SSE2 2-lane，
`sieve` 指令数 100,223,185 → 95,431,674，−4.78%）。8 个 0 触发基准的逐条阻塞点与 IR 证据见
[vectorization-coverage.md](vectorization-coverage.md)（0.418 归档、0.423 更新）：
**9/9 全部止步于前置门**（体块规模 / 终止符 / 循环形状 / loop_ops），角色分类、别名、寄存器分配一次都没走到。

### 2.2 新增向量化能力（通用判据，无基准特化）

**夜间 0.410–0.415 已落地**：`math.sqrt`（2-lane `sqrtsd` + 负数守卫）、float `Sub` 归约、
`coef==0` 广播装载、重复地址 RMW（有序合并 + 配对访问校验）、散射写 `coef>1`、跨 `coef` 别名守卫、
借用 xmm8-15（只借 RA 未占用的寄存器）、体块上限 64 → **128**。

**白天 0.416–0.423 新增**：

| 版本 | 能力 |
|---|---|
| 0.416 | 特化/通用按**体块类型分流**：整数循环走通用（AVX2 4-lane），浮点循环保持特化优先；默认配置 12 基准二进制逐字节不变 |
| 0.417 | **32 位守卫快路径**：int64 索引算术「可证 < 2^31」时降到 32 位序列，spectral_norm −48.24%，通用/特化差距 2.22× → 1.15× |
| 0.419 | **if-conversion + 尾块合并**（`src/opt_ifconv.cpp`）；matmul −4.52%、spectral_norm −1.91% |
| 0.420 | **pass 顺序修复**：`if_convert_accumulators` 跑在 `promote_globals_in_loops` 之前（第二遍 ifconv），sieve 计数循环因此进入向量化 |
| 0.423 | sieve 计数循环向量化（SSE2 2-lane；`if flags[i] == 1: count += 1` 形状） |

另有 **Cmp(Eq/Ne) → 0/1 向量比较**与 **bool 作 int 参与向量化**（夜间收尾与 0.416–0.423 期间）：
这两项让 `if` 条件与布尔累加形状能进 2-lane/4-lane 序列（488 用例覆盖），也是 0.423 sieve 计数循环
能通过 `loop_ops` 门的前提之一。

### 2.3 性能（`perf stat -e instructions:u`，3 轮取最小）

**表 A：夜间基线 → 现在**

| 基准 | 夜间基线 | 现在 | 变化 | 触发数 |
|---|---|---|---|---|
| **nbody** | 199,390,637 | **109,040,119** | **−45.31%** | 1 → 2 |
| **spectral_norm** | 247,401,494 | **94,637,438** | **−61.75%** | 2 → 4（运行时 2） |

**表 B：0.418 基线（今天早上）→ 现在**

| 基准 | 0.418 基线 | 现在 | 变化 |
|---|---|---|---|
| **sieve** | 104,389,573 | **95,431,676** | **−8.58%** |
| **matmul** | 10,605,949 | **10,126,744** | **−4.52%** |
| collatz | — | — | −0.85% |
| mandelbrot | — | — | −0.53% |
| nqueens | — | — | −0.24% |
| fannkuch | — | — | −0.23% |
| 其余（binary_trees / fasta / fib） | — | — | ±0 |
| wordfreq | — | — | **+0.08%**（0.422 动态 str 键正确性代价） |

`PYADAMAS_SIMD=1` 下 matmul：14,742,616 → **10,126,743（−31.3%）**——0.416 把整数循环分流到通用 AVX2 4-lane
（特化模板是 2-lane）；spectral_norm 的通用/特化差距 2.22× → 1.15×（0.417），但仍依赖特化（见 §五）。

0.435 的 `rt_dict.c` 改动把 wordfreq 指令数从 257,707,474 降到 **251,337,491（−2.47%，5 次重采样稳定）**：
这是同一 TU 内新增函数改变了 `pa_dict_get/set` 热路径的内联/代码布局的副作用，**不作为优化承诺**
（wordfreq 的 dict 是 typed `dict[str,int]`，kind=3 分支不会执行）；其余 11 个基准在 ±0.03% 内。

12 个基准 × -O0/-O1/-O2 输出 **36/36** 与 CPython 逐字节一致；
夜间 0.416 默认配置曾验证「12 个基准二进制与夜间基线逐字节相同」，之后 0.417/0.419/0.420/0.423
按预期只改变 sieve / spectral_norm / matmul，无任何基准指令数回退 >1%。

## 三、语义审计与修复的 bug

### 3.1 bug 清单（0.416 之后新修，按严重性）

| 版本 | 类别 | 问题 | 修复 |
|---|---|---|---|
| **0.445** | **stdlib·math** | `math.frexp` / `math.modf` 一调用就段错误（文档承诺返回二元组，实际完全不可用） | 位模式不能按 unknown 追加：`pa_list_append` 第三参数 2→0（裸值），frexp 指数同理 |
| **0.444** | **GC·生命周期** | `PYADAMAS_GC_INCR_MARK=1` 下退出时 `pa_gc_shutdown` UAF / `refcount decrement on a freed object`（增量标记在途 + pend 先 free 后 dec） | shutdown 先 `gc_incr_finish()` 收尾在途增量收集；`gc_pend` drain 移到 free 堆链之前 |
| **0.442** | **格式化** | `%#x`/`%#o` 忽略进制前缀（`ff` vs `0xff`）；`%10s` 不补宽且默认对齐错（format 的 str 默认左对齐） | 新增 `alt` 标志 + `pct_alt_join`（前缀计入宽度/zero 落在前缀后）；`%s`/`%r` 默认补 `>` |
| **0.441** | **文件编码** | strict 解码丢弃文件尾残缺多字节序列（不抛 `UnicodeDecodeError`）；`errors="replace"` 编码按字节补 `?`（`caf??` vs `caf?`） | EOF 检查 `rawleft` 按 errors 策略处理；编码/解码 EILSEQ 按完整字符（最大非法子部件）跳 |
| **0.440** | **模块导入** | 包 `__init__.py` 的 `from . import mod` 被误判循环导入（标准包写法编译失败） | `driver_build.cpp`：req.name == 当前单元且 from_names 都是子模块时只排队子模块 |
| **0.438** | **容器比较** | 容器大小比较未实现却按指针静默比（`[2] < [1,9]` True、`set([1]) <= a` False、混合类型不抛 TypeError） | 静态类型任一侧为 list/tuple/dict/set 时编译期拒绝（`==`/`!=` 不受影响） |
| **0.437** | **生成器·async** | 跨挂起 spill 的活性分析按 vreg 编号误判（vreg 非 SSA），对从未定义的 vreg 做 `rc_inc` 读到栈垃圾 → async「await 已取消的 task」panic | `lower_gen_spill.cpp` 加支配集过滤：只溢出「定义块支配挂起点」的 vreg |
| **0.436** | **类型检查·异常** | `except ... as e` 处理器里对外层变量赋值创建影子符号（静默错值：int 槽存 str 指针按 int 打印） | handler 体结束后把绑定回写父作用域（函数级作用域），顺带恢复 annotated 检查 |
| **0.435** | **unknown 域哈希** | 无类型 dict/set 的实例键按机器字哈希 + 身份比较：等值不同对象查不到、set 不去重；`hash(unknown)` 编译期报错 | `rt_dict.c` 的 kind=3 增 `__hash__`/`__eq__` 槽分派；只有 `__eq__` 无 `__hash__` → unhashable TypeError；`hash()` 用 `kind=-1` 运行期探针 |
| **0.434** | **unknown 域比较** | 实例 `==`/`<` 在 unknown 域按身份；`sorted(实例)` 只是抛 TypeError；typed `list[P]` 的 `in`/`index` 按身份 | 元数据第 7 字改为槽表指针 `[eq,lt,hash,-1]` + `pa_instance_dunder_fn`；`__lt__` 打通、`sort_objlt` 稳定排序；typed list 元素比较走槽分派 |
| **0.433** | **unknown 域相等** | 实例从混合容器取出后 `==` 按同一性（CPython 调 `__eq__`） | vtable 元数据 6→7 字加 eq_slot，`any_unknown_eq` 按「实例→vtable→槽」调用 `__eq__` |
| **0.431** | **EH 控制流** | finally 里 raise 在早退路径（return/break/continue/异常）上逃出调用方 except，未捕获 exit(1) | 根因「LSDA 覆盖是代码位置不是逻辑状态」；方案 A：`fin.early` 新块 + 函数末尾块重排（逐条重映射跳转目标） |
| **0.430** | **DCE** | -O2 把会抛异常的 `int(float)` 语句整条删掉，异常静默消失（0.427 的回归） | `inst_is_pure(Convert)` 只对 Bool/Float 目标算纯 |
| 0.422 | 无类型 dict | 动态 str 键按指针比较（KeyError / 静默查不到） | 键比较走内容 |
| 0.424 | 混合容器 | `list[unknown]` 的 `in` / `.index` / `==` 按指针身份比较（3 个分派点） | str/big 内容比较 + 容器递归 |
| 0.425 | 混合容器排序 | `sort_kind=3` 元素族混杂时静默按机器字排乱 | 元素族分类 + 族混杂抛 TypeError |
| 0.432 | 类实例排序 | `sorted([P(...)])` 静默返回未排序（按堆指针） | 全 OTHER 族里检测 `RC_INSTANCE` → 显式 TypeError（文档化限制） |
| 0.428 | 切片 | 负区间切片赋值插入点错（穷举 484 组发现 **99 行**与 CPython 不同） | 空切片插入点改用 `start`（1 行） |
| 0.428 | 切片 | `del a[::2]` / `del a[::-2]` 无法工作（撞扩展切片长度检查） | 新增 `pa_list_slice_del` 独立入口 |
| 0.427 | 转换 | `int(inf)` / `int(nan)` 静默得 −2^63（`cvttsd2siq` 哨兵值） | 哨兵检查 + `jp` 先于 `je` + 运行期抛 ValueError/OverflowError |
| 0.429 | 字符串 | `str`/`bytes` 的 `replace(..., count=0)` 替换一次 | 计数循环前 `count==0 → hits=0`（两处实现都改） |
| 0.426 | 类型检查 | 一元 `+`/`-` 作用于 bool 结果类型应为 int（`print(+True)` 打 True） | 一元分支结果类型改 int |
| 0.420 | 优化 pass | `promote_globals_in_loops` 判据只数 body→exit 边，**自写入以来从未生效** | 出口边改在 header 上判定；sieve −3.99% |
| 0.419 | 优化 pass | `fuse_dict_bump` 判据过窄（0.419 审计修复又放宽过度，0.421 回退） | 判据重写；回退记录见 CHANGELOG 0.421 |

> 另有 0.421 事故：我放宽 `fuse_dict_bump` 判据时把 key 参数改成临时 vreg 并删掉 `rc_inc`，
> 「循环内 key 来自列表取值」形状 -O2 SIGSEGV；已完整回退（0.421 记录）。

### 3.2 审计区域与方法

22 轮语义审计（每轮：审计表 + 最小复现 → 修 1–2 个最严重 → 新用例 + 全量闸门；后 15 轮在 0.436–0.446）：

| 轮次 | 区域 | 结果 |
|---|---|---|
| 0.422–0.425 | unknown 语义（dict / list / any_cmp / sort） | 4 个 bug |
| 0.426–0.427 | 算术与转换（bool 一元 / int(inf,nan)） | 2 个 bug |
| 0.428 | 切片（负区间赋值 / del 扩展切片） | 2 个 bug |
| 0.429 | 字符串（replace count=0，str/bytes 两份实现） | 1 个 bug |
| 0.430–0.431 | 优化交互（DCE）与异常（finally 早退 raise） | 2 个 bug |
| 迭代协议（for/enumerate/zip/reversed/range/sorted/genexpr/break-else/闭包捕获） | 诊断 | **未发现 bug**（未实现项：enumerate(start=)、zip 3+、lambda 默认值非字面量） |
| OOP（类/继承/super/isinstance/魔术方法/属性边界/生命周期） | 诊断 | **未发现 bug**；两个待决项后续处理：`del obj.attr` 语义写进 compatibility.md（0.432），类实例排序先显式报错（0.432）后随槽表打通改回按 `__lt__` 排序（0.434）；unknown 域 dunder（`==`/`<`/`hash`）在 0.433–0.435 补齐（后续轮次，非 OOP 审计发现的 bug） |
| 容器方法（list/dict/set/tuple 方法、浅拷贝、集合运算、相等） | 诊断+修复 | **1 个 bug**（容器大小比较静默按指针比，0.438 改编译期拒绝）；`dict.keys()` 返回 list 等文档化差异 |
| 推导式与作用域（多 for/过滤/嵌套、作用域隔离、闭包捕获、global/nonlocal、默认参数） | 诊断 | **未发现 bug**；记录缺口：未初始化变量读取返回 0（应 UnboundLocalError/NameError） |
| 模块与导入（import/from/别名、共享状态、快照绑定、包相对导入、循环导入） | 诊断+修复 | **1 个 bug**（包 `__init__` 的 `from . import mod` 误报循环导入，0.440）；循环导入是设计上显式拒绝 |
| 文件 I/O 与上下文管理器（读写/追加/seek/with 自动关闭/自定义 `__enter__`/编码） | 诊断+修复 | **2 个 bug**（strict 解码丢尾部残缺序列、replace 按字节替换，0.441）；with/异常路径全对 |
| 数字格式化与转换（进制/int·float 解析/最短 repr/f-string/format/%/round/divmod） | 诊断+修复 | **2 个 bug**（`%#x` 前缀、`%s` 宽度与默认对齐，0.442）；`min/max` 混合类型返回 float 属类型系统差异 |
| 装饰器与属性协议（函数/方法/类装饰器、property/setter/deleter、staticmethod/classmethod、方法绑定） | 诊断 | **未发现代码 bug**；文档承诺缺口：装饰后的 `__iter__` 不可用；`__name__`/类装饰器加属性/`super().<property>` 显式未实现 |
| RC·GC 生命周期（深嵌套/大容器/环/临时对象/10 万实例/闭包；环境矩阵 × ASan） | 压力+修复 | **2 个 bug**（spill 活性误判 0.437、GC 增量标记 shutdown UAF 0.444）；环形引用由分代 GC 回收（实测 `freed=11928/live_objs=10`）；无环形 RC 泄漏 |
| stdlib 文档承诺（math/json/random/os·sys/datetime 第一轮；re/collections/itertools/functools/statistics/pathlib/hashlib 续集） | 诊断+修复 | 第一轮 **1 个 bug**（math.frexp/modf 段错误，0.445）；续集 **未发现代码 bug**，补 3 处 compatibility 差异；random 序列与 CPython 逐值一致 |

方法论沉淀在 [semantics-audit-playbook.md](semantics-audit-playbook.md)（12 节；§11 pass 有效性验证、§12 环境矩阵 × ASan）：穷举对拍 > 手写用例、先查文档、
静态设计差异不是 bug、测试可能钉住 UB（0.427 修 `int(NaN)` 时 288 用例的 UB 期望要改）、
修复在语义层不在现象层、**找全同类点位**（0.429 的 str/bytes 两份实现）、探针按方法组拆分、
**改语义 → 必须审计优化器假设**（0.427 → 0.430 的连锁）。

## 四、测试规模与验证

| 项 | 夜间基线 | 现在 |
|---|---|---|
| `tests/run_tests.sh` 用例 | 369 | **409 ran ok**（夜间段 369 → 378，白天段 378 → 409） |
| `tools/difftest.sh` 用例 | 348 | **386-0 不一致**（夜间段 348 → 357，白天段 357 → 386） |
| ASan 全量 | 369 ran ok | **409 ran ok，0 sanitizer 报告** |
| check_size | 通过 | 通过 |

新增用例集中在 474–511 段（0.416–0.435 期间 482–500，含 477/482 bug 回归与 487/489/490/492/493/494/495/496/497/498/499/500 语义用例、`tests/modules/dunder_cross/` 跨模块 dunder；0.436–0.446 期间 501–511：生成器、async 取消、容器方法、推导式与作用域、文件 I/O、数字格式化、装饰器与属性、RC 生命周期、stdlib 两轮，以及 `tests/modules/import_basic/` 包导入用例）。

## 五、没做到 / 下一轮

| 项 | 状态 |
|---|---|
| 8 个基准的向量化 | 未做：分别缺 masked 向量化（mandelbrot 数据依赖退出）、嵌套循环形状（nbody 位置更新/外层）、调用屏障（wordfreq 体内 dict 调用）；逐条见 [vectorization-coverage.md](vectorization-coverage.md) |
| nbody lane 映射 | 未做：通用路径与特化仍有 **2×** 差距（nbody 是 2-lane 特化 vs 4-lane 通用之外的结构差异） |
| 类实例 dunder 在 unknown 域 | ✅ **已关闭（0.433–0.435）**：`__eq__`/`__lt__`/`__hash__` 按 vtable dunder 槽表分派；剩余文案项：unknown 域 unhashable 文案无具体类名，`__le__`/`__ge__` 仍按 typed 静态判据（见 [unknown-semantics-audit.md](unknown-semantics-audit.md) §4） |
| P4 删特化（`codegen_simd*`） | 未做：spectral_norm 仍约 **1.13×**（0.417 记录 1.15×）依赖特化 |
| AVX2 的散射/广播装载 | 未做：含这些访问的循环强制走 SSE2 |
| `1.0/0.0` 返回 `inf`（CPython 抛 ZeroDivisionError） | 既有语义差异，已记录进 [compatibility.md](compatibility.md)，未修 |
| 装饰后的 `__iter__` | 未修：文档承诺可装饰 dunder 含 `__iter__`，但 `for`/`list()` 的内建迭代路径不查类属性包装函数（0.443 记录） |
| 未初始化变量读取 | 未修：读到 0 而不是 `UnboundLocalError`/`NameError`（0.439 记录；需运行期哨兵或数据流分析） |
| `min`/`max` 混合 int/float | 未修：返回 float（`1.0`）而非 CPython 的原对象（`1`），属静态类型统一限制（0.442 记录） |
| 装饰器 `__name__` / 类装饰器加属性 / `super().<property>` | 未修：均显式未实现（编译期拒绝，0.443 记录） |

## 六、方法上的经验

1. **一次只搬一组、编译验证再走** 可行；**一次搬文件 / 批量脚本改接口** 必败（夜间两次失败都因此回滚）。
2. **代理复用**（同一子代理连续接活）显著降低沟通与试错成本；每轮我独立复核「构建 + 闸门 + 产物等价」三项。
3. **语义审计的边际收益是递减但真实的**：unknown 域与算术/切片各挖出多个 bug；迭代协议与 OOP 全绿说明
   「文档化的静态设计」已经稳定，剩下的缺口都在明确的能力清单里（§五）。
4. **改语义必须连带审计优化器假设与既有测试的 UB 期望**：0.427 改 Convert 语义 → 0.430 抓到 DCE 吞异常；
   0.427 同时需要更新 288 用例里钉住 UB 垃圾值的 expect。
