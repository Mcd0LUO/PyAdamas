# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.42] - 2026-11-10

内置函数补齐（四）：字符串方法。

### 新增

- 字符串方法（按字节串语义，ASCII 大小写）：
  `upper` / `lower` / `casefold` / `capitalize` / `title`；
  `strip` / `lstrip` / `rstrip`（可选字符集）；`split(sep[, maxsplit])`、
  无参 `split()` 按空白、`splitlines`；`join`；`replace(old, new[, count])`；
  `find` / `rfind` / `index`（带 start/end）；`startswith` / `endswith`；
  `count`；`zfill`；`ljust` / `rjust` / `center`（可填充字符）；
  `removeprefix` / `removesuffix`；
  `isdigit` / `isalpha` / `isspace` / `isupper` / `islower` / `isalnum`。
- 运行时不加几十个 RtFn，而是用一个派发入口
  `py23_str_method(self, op, a0, a1, a2)`，操作码与 `StrOp` 枚举同序。
- 测试：`56_str_methods.py`（18 行输出与 CPython 逐字节一致）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 56 个运行用例通过；
  810 程序差分零失败

### 已知限制

- 非 ASCII 的大小写/字符类别判断按字节处理（语言整体是字节串）
- `str.format`、`encode`、`partition/rpartition`、`expandtabs`、
  `translate`、`isnumeric/isdecimal/isidentifier/isprintable/istitle`
  未实现（降级阶段明确报错）

## [0.41] - 2026-11-10

内置函数补齐（三）：list / dict / tuple 方法。

### 新增

- list：`extend` / `insert` / `remove` / `pop(i)` / `clear` / `index` /
  `count` / `copy` / `reverse` / `sort`（int/float/str 三种比较器）；
  原有 `append` / `pop()` 保留。
- dict：`setdefault` / `copy`。
- set：`copy`。
- tuple：`count` / `index`（tuple 与 list 表示相同，共用运行时）。
- int / bool：`bit_length`。
- 运行时新增 list_reverse / int_bit_length；ListIndex / ListCount /
  ListInsert / ListSort 接入降级。
- 测试：`55_container_methods.py`；另做 2000 轮字符串容器 RC 压测
  （O0/O1/O2 与 CPython 输出一致）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 55 个运行用例通过；
  810 程序差分零失败

### 已知限制

- `list.sort(key=..., reverse=...)`、set 的 union/intersection/difference/
  issubset 等集合运算尚未实现（明确报错）
- `dict.popitem` 固定 LIFO、`set.pop` 固定取最后插入项（既有语义）

## [0.40] - 2026-11-10

内置函数补齐（二）：序列构造与排序/迭代辅助。

### 新增

- `list()` / `list(seq)`、`tuple()` / `tuple(seq)`、`dict()` / `dict(**kw)`、
  `set()` / `set(seq)`；str 源按字符列表、dict/set 源按键列表规整，
  list/tuple 源做浅拷贝。
- `sorted(seq)`（int/float/str 三种比较器，返回新列表）、`reversed(seq)`、
  `enumerate(seq)`（元素是 `[下标, 值]` 对）、`zip(a, b)`（两个可迭代
  对象，长度取较短者）。
- 运行时新增 list_copy / list_reversed / list_sort / sorted_list /
  list_index / list_count / list_insert / enumerate_list / zip2_list /
  set_from_list。
- 测试：`54_builtins_seq.py`。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 54 个运行用例通过；
  810 程序差分零失败
- `54_builtins_seq.py` 与 CPython 逐字节一致

### 已知限制

- `sorted(key=..., reverse=...)`、`enumerate(start=)`、三个及以上可迭代
  对象的 `zip()`、`dict(iterable)` 暂不支持（降级阶段明确报错）
- 迭代辅助返回的都是具体列表（与生成器表达式立即求值的既有模型一致）

## [0.39] - 2026-11-10

内置函数补齐（一）：标量转换与格式化，并消灭未实现内置的段错误。

### 新增

- `int(str)` / `int(str, base)` / `float(str)`：真解析字符串（前后空白、
  正负号、0x/0o/0b 前缀、下划线、2..36 进制），失败抛 `ValueError`。
  以前走 `Convert` 把字符串指针直接当数值，`int("42")` 会静默输出
  4275552。
- `pow(x, y, m)`：`(x ** y) % m`。
- `ord` / `chr`（字节语义，`chr` 只接受 0..255）、`hex` / `bin` / `oct`
  （含负数格式 `-0x10`）、`round`（四舍六入五成双；`round(x)` 返回 int，
  `round(x, n)` 返回 float，整数配负 ndigits 按 10 的幂取整；正 digits
  走 printf 正确舍入，`round(2.675, 2)` 与 CPython 一致是 2.67）、
  `divmod`、`repr`（字符串/数值/bool/None）、`format`（数值与字符串
  格式说明符）。
- 未实现的内置名（`sorted` / `enumerate` / `list()` / `map` / `type` /
  `getattr` 等）在降级阶段给出明确诊断，不再生成“调用空指针”的代码。
- 测试：`53_builtins_scalar.py`。

### 验证

- `ctest --preset dev/release` 全绿（dev 连跑三次）；O0/O1 全量 53 个
  运行用例通过；810 程序差分零失败
- `53_builtins_scalar.py` 的 11 行输出与 CPython 逐字节一致

### 已知限制

- `int()` 解析溢出按 int64 回绕（语言整体如此）；`chr()` 不能表示
  >255 的码点（字符串是字节串）
- `repr` 不支持容器/类实例；`pow(x, y, m)` 不支持负指数求模逆元

## [0.38] - 2026-11-10

O1+：块内临时寄存器分配（%r8–%r11 池），消掉大量临时值落栈往返。

### 新增

- codegen 对「整块没有调用类指令」的基本块，用 %r8–%r11 做块内临时寄存
  器：块内定义、块内用完的整数/指针临时值按线性扫描分配寄存器，`mem(v)`
  直接返回寄存器，省掉「结果写临时栈槽、下一条再读回」的往返。
- 候选限制：浮点/`unknown`/`Optional` 值不进池（`unknown` 的机器表示可能
  是浮点位模式）、`load.slot` 保留原有槽别名、可延迟常量不占寄存器。
- 块内出现 Call/CallRt/VCall/Setjmp/ImportModule/ObjNew/MakeList、字符串
  下标、div/mod/floordiv/pow 等会调用或占用 %r8–%r11 的指令时，整块不分
  配；生成器帧跳过；块边界不保留寄存器状态。
- 区间按线性扫描分配，定义/使用不重叠的临时值复用同一寄存器。

### 验证与基准

- `ctest --preset dev/release` 全绿（dev 连跑三次）；O0/O1 全量 52 个运行
  用例通过；810 程序差分零失败；release 二进制 1350 程序（5 seeds）差分
  零失败
- `--rebuild` 同机：
  - matmul 160³：8 ms → 6 ms（约 1.3×；O1 对照 9 ms）
  - 内积 2000 万次迭代：21 ms → 17 ms（约 1.2×）
  - fib(32) 持平（调用密集，块内没有可省的临时值）

### 已知限制

- 只在「整块无调用」的块生效；含调用的块仍全部落栈（跨调用要
  spill/reload，留待真正的寄存器分配）
- 块边界的值仍走栈；没有跨块/全局寄存器分配

## [0.37] - 2026-11-10

O1+：`for k in range(len(a))` 循环体里的 `a[k]` 免边界检查。

### 新增

- 降级期识别 `for k in range(len(a))`（含 `range(0, len(a))` 和步长 1 的
  三参形式），循环体里形如 `a[k]` 的读下标在下面两个条件都满足时直接生成
  `index.get.unchecked`：
  - 循环体没有调用/del/切片赋值/属性访问/with/import/装饰器等副作用
    （复用 P2 的副作用计数）；
  - 循环体没有给 `a` 或 `k` 赋值（写这两个槽即作废提示）。
- 覆盖普通 for、嵌套 range 循环、循环内逐元素写入（`a[k] = a[k] + 1`）。
- 测试：`52_range_bounds.py`，纳入三档回归。

### 验证与基准

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 语义负例：循环内把 `a` 换成更短的列表、循环体里改 `k`、循环内
  `a.pop()` 都仍在第一次越界的迭代抛 IndexError；`a.append` 后继续读
  `a[k]` 结果与 CPython 一致
- A/B（2000 万次内积迭代）：`range(len(a))` 21 ms vs 参数上界 22 ms
  （约 5%；检查的额外指令大多被乱序执行掩盖）

### 已知限制

- 只识别 `len(简单局部变量)` 做上界；`range(n)`、`len(a.b)`、表达式上界
  拿不到证明
- 模块级全局序列不参与（提示只覆盖局部槽）

## [0.36] - 2026-11-10

O2 检查型下标外提：循环版本化。

### 新增

- O2 对循环内 base 与 index 都是循环不变量的 `index.get`，在循环入口检
  一次 `0 <= idx < len(base)`：通过就走无检查的循环副本（只做一次取址和
  装载），不通过（含负下标、越界）走原循环，越界照常在第一次执行的迭代
  里抛。
- 触发条件：循环体内没有调用、没有下标写入（长度/元素不会变），base 是
  list/tuple，index 是整数；两版循环共用槽，只有一版执行。
- 测试：`51_bounds_versioning.py`（for/while、负下标、循环内改写、零次
  循环），纳入三档回归。

### 验证与基准

- `ctest --preset dev/release` 全绿；O0/O1 全量 51 个运行用例通过；
  810 程序差分零失败
- `--rebuild` 同机 A/B：matmul 160³ 11 ms → 8 ms（约 1.3×），结果与
  CPython 一致；越界报错、负下标、零次循环行为不变

### 已知限制

- 只处理「下标在循环内不变」的装载；拿循环变量做下标（如 `row[k]`）仍
  逐次检查，需要范围分析或循环旋转
- 守卫会读 `len(base)`；若 base 运行期意外为 None 且循环零次，会比原程序
  更早崩溃（类型系统本应排除这种值）

## [0.35] - 2026-11-10

O2 循环不变量外提（LICM）。

### 新增

- O2 用支配关系识别自然循环（回边 + 自然循环体），把循环内不抛异常的
  不变量搬到新增前置块：循环外前驱先跳到前置块，前置块再跳循环头，只在
  进入循环时执行一次。
- 可外提：常量、Mov、数值 Un/Cmp/安全 Bin/数值 Convert；循环内没被写过
  的局部槽装载；循环内没有调用且没被写过的全局装载。
- 不搬：检查型下标（零次循环时外提会改变越界行为）、Div/Mod/Pow、字符串
  /容器运算；含 handler 的函数与生成器帧整体跳过；取过地址的槽不搬。
- 测试：`50_licm.py`（嵌套循环、循环内改写、零次循环、while、
  try/finally、生成器），加入三档回归。

### 验证与基准

- `ctest --preset dev/release` 全绿；O0/O1 全量 50 个运行用例通过；
  810 程序差分零失败
- `--rebuild` 同机 A/B：双重循环里的不变量乘法 5–6 ms → 4 ms（约 1.3×）；
  matmul 160³ 持平——内层循环瓶颈是检查型下标与后端跨块值落栈，外提的
  槽装载只是一个内存读换成另一个内存读

### 已知限制

- 跨块 vreg 一律落栈，外提的收益会被寄存器分配抵消；没有真正的跨块
  寄存器分配
- 检查型下标外提需要“进入循环也不越界”的证明，暂未做（要循环旋转或
  守卫块）

## [0.34] - 2026-11-10

O2 静态调用直接化、闭包调用装载精简与内联后二次折叠。

### 新增

- O2 把能静态解析的普通调用（`func.addr`，或入口模块里只赋值一次的
  全局）改成 `call.direct`：callee 不读闭包环境时直接 `call .LfN`，
  省掉全局装载和闭包解引用；有捕获的函数仍走闭包间接调用。
- 闭包调用本身少一次装载：`%r11` 全程持有闭包指针，调用用
  `call *(%r11)`，环境指针改为 `movq %r11, %r10`。
- 直接调用不再无条件清 `%r10`：callee 没有 env 槽时省掉这条指令。
- 内联后重新跑一遍常量折叠 + DCE：折掉内联带进来的实参常量，并清理
  失效的 `load.global` / `func.addr`。

### 验证与基准

- `ctest --preset dev/release` 全绿（dev 连跑三次）；O0/O1 全量 49 个
  运行用例通过；810 程序差分零失败
- `--rebuild` 同机 A/B（同一编译器，仅开关直接调用）：fib(32)
  9 ms → 7–8 ms；O1 对照 11 ms

### 已知限制

- 解析不了的调用（参数传进来的函数值、字段/容器里的函数）仍走闭包
  间接调用
- `call.direct` 仍按 SysV 全参数搬运，没有做寄存器 ABI 特化

## [0.33] - 2026-11-09

O2 静态小函数内联，以及 O2 条件分支反向跳转的助记符修正。

### 新增

- O2 IR 内联：调用点的函数值能静态解析（`func.addr`，或只赋值过一次的
  模块全局）且 callee 是普通小函数时，用 callee 的 IR 克隆替换调用。
  - 只读形参（callee 内没有 `store.slot` / `slot.addr`）直接代换为调用点
    实参，不落形参槽；
  - 只有一个 `return` 时直接写调用结果 vreg，省掉返回值中转槽；
  - 克隆块紧挨调用点插入；单块直返函数整段就地插进调用块，入口/返回
    跳转由 O2 的下落消除吃掉；
  - 生成器/协程帧、含 handler、有捕获、可变参、递归函数（静态调用图能
    回到自身）不内联；每函数上限 24 个内联点 / 256 条指令增量。
- 测试：`49_inline.py` 覆盖多返回、堆参数/返回、形参重写、循环体、
  递归与嵌套调用。

### 修复

- `CmpKind::Le` 的 O2 分支反向跳转用了不存在的 `jgt`，`-O2` 编译含
  `<=` 的代码时汇编失败；改为 `jg`，并加 `48_branch_mnemonics.py`
  覆盖六种比较的反向助记符。

### 验证与基准

- `ctest --preset dev/release` 两个测试全绿；O0/O1 全量 49 个运行用例
  全过；810 程序差分零失败
- `--rebuild` 同机：`add(a,b)` 调用 2000 万次的循环 O1 27 ms → O2 7 ms
  （约 4×）；多返回 `clamp` 的 400 万次循环 O1 10 ms → O2 8 ms

### 已知限制

- 内联只看函数体大小和调用点能否静态解析，没有调用热度信息；常量折叠
  发生在外联之前，实参常量不会传播进 callee
- 多块 callee 的克隆块按原块序插入，没有块重排/热冷分离

## [0.32] - 2026-11-09

O2 深度优化：IR 常量折叠/DCE，以及 AVX2 归约内核。

### 新增

- IR 优化遍 `optimize_ir`（仅 O2）：常量折叠（整数/浮点算术、比较、
  一元运算、数值转换）、常量条件分支化简为 jump、纯标量指令的死代码
  消除（迭代到不动点）。Div/Mod/Pow、可能抛异常的 checked index、
  容器分配和调用不参与 DCE。
- AVX2 运行时归约：O2 下 `sum(list/tuple[int|bool])`、`min/max(list[int])`
  直接调用运行时内核；`sum(list[float])` 只在显式 `--fast-math` 下走 SIMD
  （分组求和会改变末位舍入），无 AVX2 时回退标量。模块缓存区分
  `--fast-math`。
- 修正 `sum()` 的 start 参数类型提升：`sum([], 0.5)` 现在是 float 0.5。
- 测试：`46_simd_reduce.py`、`47_float_sum_order.py`，并加入
  `py23_opt_levels` 三档回归（含 `--fast-math` 专项）。

### 验证与基准

- `ctest --preset dev` 两个测试全绿；O0/O1/O2 全量套件与 810 差分零失败
- `--rebuild` 同机：
  - sum 100 万×20（float）：O1 14.5 ms → O2 13 ms（IR 遍）；
    加 `--fast-math` 后 7 ms（SIMD 分组求和，约 2×）
  - sum 100 万×20（int）：O1 12.5 ms → O2 7.5 ms（AVX2 归约，约 1.7×）
  - dot 10M：O2 约 87 ms → 74 ms（IR 遍约 1.18×）
  - matmul 160³：O2 约 10 ms（持平，内核仍是标量用户代码）
- `python3 tests/diff/run_diff.py --py23 /tmp/py23O0|O1|O2` 均 0 失败

### 已知限制

- SIMD 只覆盖内置 sum/min/max 归约，没有通用循环自动向量化；用户写的
  matmul 内层循环仍是标量
- AVX2 min/max 只支持 int64；float min/max 保持标量比较以维持 NaN/顺序语义
- float SIMD sum 仅在 `--fast-math` 下启用，结果可能有末位差异；
  默认 O2 保持从左到右的标量顺序
- IR 遍没有跨块 LICM、CSE、内联和调度

## [0.31] - 2026-11-09

生成代码优化等级 `-O0` / `-O1` / `-O2`，默认 `-O2`。

### 新增

- CLI 支持 `-O0`、`-O1`、`-O2`（以及 `--opt=N`），默认 `-O2`；模块缓存
  manifest 记录优化等级，切档不会复用旧对象。
- `-O0`：关闭 P0 下标内联、P1 槽融合/内存别名/浮点提升/临时融合、
  P2 循环免检查；保留原有的整数槽提升、常量、分支融合等基础 codegen。
- `-O1`：打开 P0/P1/P2（0.29/0.30 的完整优化流水线）。
- `-O2`：在 O1 基础上增加
  - 跳到下一个基本块的 `jmp` 省略；
  - 条件分支一边是 fallthrough 时反向条件跳转，省无条件跳转；
  - 非变参调用不再发 `xorl %eax, %eax`；
  - 序言清零裁剪：must 数据流只清零可能「读在写之前」的槽，含
    `SlotAddr` / handler 的函数保守处理。
- 测试：`tests/opt_levels.sh`（CTest `py23_opt_levels`）三档各跑容器、
  推导式、免检查、序言/跳转回归；新增 `45_opt_semantics.py`。

### 验证与基准

- `ctest --preset dev` 两个测试全绿；O0/O1/O2 各自 810 程序差分零失败
- `--rebuild` 同机基准：
  - matmul 160³：O0 21 ms / O1 10 ms / O2 10 ms
  - sum 100 万×20：O0 63 ms / O1 14.5 ms / O2 13.5 ms
- O0 接近 0.28 基线；O1 是主要收益；O2 的收益集中在短循环/小函数。

### 已知限制

- 等级只门控现有后端优化；还没有 C++ `-O1/-O2` 意义上的 IR 优化遍
  （常量折叠/传播、DCE/CSE/LICM、强度削减、内联、调度、向量化）
- 跳转消除只处理无条件跳转和整数条件分支；浮点/字符串分支仍保留
  两条跳转
- 序言裁剪对含异常 handler、生成器帧和取地址槽保守执行

## [0.30] - 2026-11-09

优化遍 P2：编译器生成的循环下标免检查。

### 新增

- 新 IR 指令 `index.get.unchecked`：调用方已证明 `0 <= idx < len` 时
  直接读 `data`，不生成边界分支/负索引归一化。
- `for x in list/tuple`：循环体降级期间没有调用、`del`、切片赋值、
  属性 setter/getter、with/import/装饰器等副作用时，元素装载改成
  unchecked。重绑循环变量不影响序列 VReg，因此不阻止优化；生成器/
  协程帧可能跨挂起，一律跳过。
- 推导式：每个生成器在自身循环体降级完后单独判断；首层 iterable
  的求值调用（如 `range()`）不计入。累加器的 ListAppend/DictSet
  不算用户副作用。
- `sum` / `min` / `max` / `any` / `all` / `print(*seq)` 的内部遍历
  对 list/tuple 直接使用 unchecked 装载。
- 测试：`44_bounds_elim.py` 覆盖纯循环、循环体重绑、append 变更、
  推导式调用副作用、内置聚合与 writeback 循环。

### 验证与基准

- `ctest --preset dev/release` 全绿；810 程序差分零失败
- 同机 `build --rebuild`（避免模块缓存串味）对比 0.29：
  - sum 100 万×20：约 14.5 ms → 13.5 ms（~1.08×）
  - dot 10M：约 87 ms → 88 ms（持平）
  - matmul 160³ / 300³：约 10.7 / 70 ms（持平，噪声内）
- P0 的下标快路径已经把检查成本压得很低，P2 的收益集中在 sum 这种
  极短循环体；用户写的 `a[k]` 仍然保留检查。

### 已知限制

- 只消除编译器生成循环的元素装载；用户下标、跨块归纳变量证明未做
- 循环体含任何用户调用即保守放弃（不跟踪实参是否可达源容器）
- 生成器/协程中的循环不消除（挂起期间容器可能被其他任务修改）

## [0.29] - 2026-11-09

优化遍 P0/P1：内联下标、槽位融合与浮点寄存器提升。

### 新增

- `IndexLoad` / `IndexStore` 内联 list/tuple 快路径：直接读 `len`/`data`，
  无符号比较挡越界，负索引归一化内联；越界慢路径调运行时抛异常且不返回。
  浮点元素直接 `movsd`，跳过 GPR 往返；索引赋值按运行期 `elem_kind`
  决定是否 dec 旧元素。
- `store.slot` 结果融合不再只限寄存器槽：表达式结果可以直接写变量栈槽，
  消除「临时 vreg 槽 → 变量槽」的搬运。
- `LoadSlot` 允许把块内单次使用的值别名到变量栈槽/寄存器；加载指令消失。
- 浮点局部变量提升到 xmm8–15（xmm0–7 留给参数/临时）。所有会正常返回的
  call 前后统一 spill/reload；含 handler 的函数与生成器帧不提升。
- 浮点二元运算就地写目标 xmm；结果只被下一条浮点二元指令使用时暂留
  `%xmm1`，省掉临时槽写入/读回。
- 测试：`43_indexing.py` 覆盖正负索引、越界错误、字符串/浮点/元组下标。

### 验证与基准

- `ctest --preset dev/release` 全绿；810 程序差分零失败
- 同机对比 0.28（`build --rebuild`，避免模块缓存串味）：
  - matmul 160³：约 21 ms → 10.7 ms（~1.9×）
  - matmul 300³：约 137 ms → 70 ms（~2.0×）
  - dot 1000 万：约 102 ms → 87 ms（~1.17×）
  - sum 100 万×20：约 61 ms → 14.5 ms（~4.2×）

### 已知限制

- 未做 P2 越界检查消除、循环旋转/LICM、直接调用与序言清零裁剪
- 浮点提升按静态热度选择；调用极密集且浮点变量少复用的函数可能被
  spill 拖慢，后续需要按循环/调用频率调整

## [0.28] - 2026-11-09

C3 MRO、零参 super()、方法装饰器与 match 边角收尾。

### 新增

- C3 线性化：`ClassInfo::mro` 保存标准 C3 顺序；方法查找、vtable 填充和
  `super()` 都沿 MRO。附加基类可以复用主基类布局里的已有字段（共享祖先的
  菱形可行），新增字段仍拒绝；同名方法的种类（普通/静态/属性）冲突会报错。
- 方法槽改为模块内按名字全局分配：多继承两个无关基类给同名方法分过不同
  槽的问题消失，所有类 vtable 等长。
- 零参 `super().m(...)` / `super().__init__(...)`：运行期按实例真实 MRO
  找下一实现，支持协作式构造函数链；显式 `super(C, self)` 仍未支持。
- `@staticmethod`：实例调用（vcall.static，不传 self）和 `Class.method`
  直接调用（call.direct）都支持，按 MRO 覆盖。
- `@property` + `@x.setter` + `@x.deleter`：属性读写删分别编译成
  getter/setter/deleter 虚调用，沿 MRO 继承。
- vtable 前元数据扩成 5 个字：MRO 元数据指针表、类编号、vtable、本类声明的
  槽表、模块限定类名；新增 `py23_is_named_class` / `py23_super_lookup`。
- 静态字符串字面量在模块入口通过 `py23_register_literals` 登记进 RC 索引，
  unknown 值上的字符串模式可以安全比较字面量。
- match 边角：捕获名在 case 结束后仍可见（各分支类型 join）；unknown 值上的
  字符串/复数模式；导入类点分类名（按限定名检查 MRO，字段子模式仍不支持）；
  映射常量 key 重复检查；unknown 值上的 `object()` / `str()` / `complex()`。
- 测试：`40_mro_super.py`、`41_decorators.py`、`42_match_corners.py`、
  `f37_match_dup_key.py`、模块用例 `tests/modules/matchclasses/`。

### 验证

- `ctest --preset dev` 全绿（42 个运行用例 + 新模块用例）
- 差分测试 810 个程序零差异

### 已知限制

- `@classmethod`、自定义方法装饰器、类装饰器未支持
- `super(C, self)` 与 staticmethod/property 上的 super 调用未支持
- unknown 原始值无法区分 int/float；list/dict/tuple/set 的内置类型模式仍拒绝
- 导入类模式只做类检查，不读取字段；额外基类仍不能新增字段

## [0.27] - 2026-11-09

match 收尾：类模式、值模式、`as`、OR 捕获与映射 `**rest`。

### 新增

- 类模式：`case Point(x=0)` 关键字子模式、`Point(0, y)` 位置子模式
  （类体 `__match_args__` 定义字段顺序，沿继承链查找）；子类实例按父类字段前缀匹配。
- 值模式：`case Color.RED` / `case math.pi`，点分名字求值后按 `==` 比较。
- `as` 捕获：`case Point(0, 0) as p`；`case [a, b] as pair`。
- OR 模式支持捕获，要求每个分支绑定相同的名字；共享槽由 OR 统一分配。
- 映射模式支持多 key 与 `**rest`（浅拷贝原字典后删掉已匹配 key）。
- 内置/自定义异常类模式：`case ValueError():`，按运行时异常类型和父子关系匹配。
- 内置类型模式（`case int():` 等）在 subject 静态类型已知时编译期判定。
- 运行期安全检查：unknown 值上的序列模式先查 list/tuple 载体，映射模式先查
  dict 载体；类模式用 `py23_isinstance` 查 RC 注册表和 vtable 祖先表，
  原始整数等非堆值安全返回 false。
- 运行时：新增 `PY23_RC_INSTANCE` 类实例 kind；vtable 前增加 16 字节元数据
  （祖先编号表 + 类编号），对象布局与虚调用槽位不变。
- 测试：`39_match_advanced.py`、`f35_match_no_match_args.py`、
  `f36_match_or_names.py`。

### 验证

- `ctest --preset dev` 全绿（39 个运行用例）
- 差分测试 810 个程序零差异

### 已知限制

- 捕获名仍是 case 作用域，case 结束后不可见
- 点分类名（`module.Point(...)`）和导入类的类模式未支持
- 映射模式不做重复 key 检查；unknown 值不能使用字符串/复数/内置类型模式
- 多继承仍不做完整 C3 MRO

## [0.26] - 2026-11-09

语法面补齐：del、print 展开、复数、f-string、match、多继承。

### 新增

- `del`：删除局部/全局/cell、字典键、列表下标、step=1 列表切片和类字段。
- `print(*iterable)`：支持 list/tuple/str/dict/set 动态展开，sep/end 正常。
- 复数字面量（`3j`）与 `complex()`；`+ - * /`、`abs`、`==/!=`、真值、
  print 和 f-string。
- f-string 格式化：fill/align/sign/`0`/width/precision、`d b o x X f e E g G %`、
  `!s/!r/!a` 和嵌套格式字段。
- `match/case`：字面量、捕获、通配、序列（含 `*rest`）、OR、映射、守卫；
  捕获绑定在 case 作用域；不做穷尽性检查（Python 语义）。
- 多继承：第一个基类提供布局，附加基类按 mixin 提供方法（不能有字段）。
- 类体非声明语句在类定义点执行（赋值按模块全局处理）。
- 测试：`35_syntax_extras.py`、`36_fstring.py`、`37_mi_classbody.py`、
  `38_match.py`。

### 验证

- `ctest --preset dev` 全绿（38 个运行用例）
- 差分测试 810 个程序零差异
- ASan/UBSan/LSan 目标用例零报告

### 已知限制

- match 的类模式/值模式与映射 `**rest` 未支持；没有穷尽性检查
- 多继承不做完整 C3 MRO；附加基类不能有字段
- 类体没有独立命名空间，类属性按模块全局处理
- f-string 的浮点分组/`n` 等边角格式有限

## [0.25] - 2026-11-09

异常模型收尾与 any/all。

### 新增

- 自定义异常类：`class MyError(ValueError): pass`，支持多级继承；
  `except MyError` 按运行时注册的父子关系匹配。类体目前只允许
  `pass` / 文档字符串。
- `raise X from cause` 和 `e.__cause__`；`raise ... from None` 清空原因。
- 内置函数 `any` / `all`：短路，支持 list/tuple/str/dict/set。
- 测试提速：`run_tests.sh` 支持 `PY23_TEST_FILTER` / `PY23_TEST_JOBS`
  并并行跑运行/模块用例；`run_diff.py` 加 `--jobs`。全量套件约 3s，
  810 程序差分约 8s。
- 修复 `py23_raise` 手工构造异常时未初始化 `cause` 的崩溃。

### 验证

- 定向用例 22/32/33/34 + dev CTest；`ctest --preset release` 全绿
- 差分测试 810 个程序零差异
- ASan/UBSan/LSan：异常值、`raise ... from`、自定义异常路径零报告

### 已知限制

- 异常 `args` 只保存消息；没有 traceback
- 自定义异常类不能定义字段或方法
- `min`/`max`/`sum`/`any`/`all` 只迭代 list/tuple/str/dict/set

## [0.24] - 2026-11-09

异常值模型与常用内置函数。

### 新增

- 异常值对象：`ValueError("msg")` 可作为表达式值；`raise e` 抛掷实例；
  `except ... as e` 绑定带 `args` 的异常值，`str(e)` / `print(e)` 返回消息，
  `e` 参与字符串拼接按消息处理（兼容旧代码）。
- `Future.set_exception(exc)` 接受异常值（仍兼容消息字符串）；
  `Future.exception()` / `Task.exception()` 返回异常值。
- 异步生成器 `athrow(exc)`：在挂起点注入异常，捕获后可以继续产出。
- 内置函数：`abs` / `min` / `max` / `sum` / `pow`。`min`/`max` 支持
  多实参和 list/tuple/str/dict/set 迭代，空序列抛 `ValueError`；`sum`
  支持起始值；int/float 混合按数值域提升。
- 用例 `tests/run/32_exception_value.py`、`tests/run/33_builtins.py`。

### 验证

- `ctest --preset dev` / `release` 全绿（33 个运行用例）
- 差分测试 810 个程序零差异；ASan/UBSan/LSan 零报告

### 已知限制

- 异常 `args` 只保存消息；没有 traceback、异常链、`raise ... from`
  和自定义异常类
- `min`/`max`/`sum` 只迭代 list/tuple/str/dict/set，不接受生成器

## [0.23] - 2026-11-09

异步生成器协议：`asend` / `__anext__` / `aclose`。

### 新增

- `agen.asend(v)`：把值送进 `x = yield ...` 并取下一个值；驱动对象
  用 `driver_sent` 持有值到恢复，支持跨事件循环。
- `agen.__anext__()`：等价于 `asend(None)`。
- `agen.aclose()`：停在 `yield` 上时走 close 清理分发（finally / with）；
  从未启动时不执行函数体；正在等内部 awaitable 时注入 `GeneratorExit`。
- 用例 `tests/run/31_asyncgen_proto.py`。

### 验证

- `ctest --preset dev` / `release` 全绿（31 个运行用例）
- 差分测试 810 个程序零差异；ASan/UBSan/LSan 零报告

### 已知限制

- 没有 `athrow`（异常不是一等值）
- `aclose` 不会 await `async with` 的 `__aexit__`；同一生成器上并发的
  asend 驱动在 aclose 后会挂起

## [0.22] - 2026-11-09

Future 完成回调与失败结果。

### 新增

- `Future.add_done_callback(fn)`：完成时按注册顺序同步调用 `fn(future)`；
  对已完成的 Future 注册会立即调用；取消也会触发回调。
- `Future.set_exception(msg)`：把 Future 标记为失败，`await` 时抛出
  `Exception(msg)`，`exception()` 返回该消息。py23 的异常不是一等值，
  所以参数是消息字符串而不是异常对象。
- 运行时新增 `py23_call1` 跳板，按闭包 ABI（`%r10` 环境 + `%rdi` 参数）
  调用函数值；回调列表参与引用计数和 GC。
- 用例 `tests/run/30_future_cb.py`。

### 重构

- 降级器按职责拆成 `lower*.cpp`：驱动/预扫描、语句、异常、表达式、调用、
  生成器与协程；共享的 `Lowerer` 声明、`FuncCtx`、`ClosureMeta` 移到
  `lower_impl.h`。
- 类型检查按职责拆成 `typecheck*.cpp`：驱动与作用域、语句、表达式、
  调用与内置、函数与类。
- 解析器按职责拆成 `parser*.cpp`：驱动、语句、函数与类、表达式。
- 代码生成拆成 `codegen*.cpp`：驱动/数据段、寄存器分配、基本块、
  指令选择、调用与对象；`AsmGen` 声明移入 `codegen_impl.h`。
- 运行时拆成 `runtime/rt_*.c` + `rt_internal.h`（RC/GC、异常、生成器、
  异步、Task/Future、fd、异步生成器、容器与输出），各单元独立编译后
  由 CMake 用 `ld -r` 合成 `rt.o`，对外 ABI 不变。
- 文档同步新的文件布局与 sanitizer 构建步骤。

### 验证

- `ctest --preset dev` / `release` 全绿（11 正例 + 33 反例 + 30 运行 +
  4 未建模 + 8 模块目录），干净重建零警告
- 差分测试 6 个生成器、810 个程序零差异
- ASan/UBSan/LSan：生成器、协程、Future 回调、`yield from`、异步推导式
  路径零报告

### 已知限制

- 回调是同步调用（CPython 用 `call_soon` 排队）；回调抛出的异常向外
  传播，并跳过本 Future 剩余的回调
- `set_exception` 只接收消息字符串，失败类型固定为 `Exception`
- 回调参数没有类型标注时，函数体里访问 `future.done()` 等会因为 unknown
  接收者无法降级；需要标注成 `Future[T]`

## [0.21] - 2026-11-09

语言面补齐：`yield from` 与异步推导式。

### 新增

- **`yield from`**：委托给生成器或其他可迭代对象（list/tuple/str/dict/set）。
  生成器委托支持 `send` 转发；生成器完成时循环自然退出，外层
  `close()` 释放子生成器引用并让它执行 `finally`；委托期间子生成器
  抛出的异常向外传播。表达式值固定为 None（异常对象不携带
  `StopIteration.value`）。
- **异步推导式**：`[x async for x in agen()]`、集合与字典推导式都支持，
  可混用同步/异步 `for` 子句与 `if` 过滤；累加器和循环状态放在帧槽里，
  挂起恢复后继续，`StopAsyncIteration` 在循环边界捕获。
- 修正生成器元素为堆类型时 `for` / `next` / `send` 结果的静态类型：
  运行时按机器字返回值，之前按 unknown 落槽会漏 retain。
- 用例 `tests/run/28_yield_from.py`、`tests/run/29_async_comp.py`；
  差分生成器的 gen / async 两组补上委托与异步推导式路径。

### 验证

- `ctest --preset dev` / `release` 全绿（11 正例 + 33 反例 + 29 运行 +
  4 未建模 + 8 模块目录）
- 差分测试 6 个生成器、810 个程序零差异
- ASan/UBSan/LSan：yield from 关闭/异常路径、异步推导式挂起路径零报告

### 已知限制

- `yield from` 的表达式值固定为 None，不能把异常 `throw` 进生成器
- `yield from` 只静态识别生成器与 list/tuple/str/dict/set；静态类型是
  unknown 时按生成器协议尝试，失败抛 TypeError
- 异步推导式只能作为整条语句、赋值或 `return` 的根表达式，不能嵌在
  `print(...)` 等其它表达式里；元素表达式里不能含 `await`；不支持
  异步生成器表达式

## [0.20] - 2026-11-09

运行时补齐：Future 协议、取消级联、fd 轮询。

### 新增

- `asyncio.Future()`：支持 `set_result` / `cancel` / `done` /
  `cancelled` / `result` / `exception`，可被多个协程同时 await；
  完成后 `result()` 返回保存值，失败/取消时抛出异常副本。
- Task 补齐 `result()` / `exception()` / `cancelled()`；Future 与 Task
  的 `exception()` 返回消息字符串（成功返回 None），和现有“异常只有
  类型名 + 消息”的模型一致。
- **取消级联**：取消 Task 会沿它等待的对象向下传播——协程链继续注入
  `CancelledError`，被等待的 Task 递归取消，gather 取消全部子任务，
  Future/sleep/fd 等待对象标记取消；各层的 `finally` 都会执行。
- **fd 轮询**：`asyncio.wait_readable(fd)` / `wait_writable(fd)` 返回
  可等待对象，事件循环在没有就绪任务时调用 `poll`（超时取最近定时器），
  无效 fd 以 `OSError` 结束等待；fd 等待对象挂在 GC 根表里。
- `create_task` 也接受 Future/sleep/fd/等可等待对象（内部包装成 Task）。
- 类型标注支持 `Future[T]` / `Task[T]` / `Coroutine[T]` /
  `Generator[T]` / `AsyncGenerator[T]` 的解析。
- `print(Optional[T])` 对 None 打印 `None`，不再落到整数/指针分支。
- 修复协程挂起时 state 被清零导致取消无法沿链传播；GC 清扫和进程退出
  现在会回收挂在帧/Task/Future 上的 malloc 异常对象。
- 用例 `tests/run/27_runtime.py`。

### 验证

- `ctest --preset dev` / `release` 全绿（11 正例 + 33 反例 + 27 运行 +
  4 未建模 + 8 模块目录）
- 差分测试 6 个生成器、810 个程序零差异
- ASan/UBSan/LSan：Future 多等待者、取消级联、fd 轮询、异常清理零报告

### 已知限制

- 没有 `Future.set_exception`（异常不是一等值）和回调 `add_done_callback`
- `exception()` 返回消息字符串而不是异常对象；协程未来可等对象仍用单
  等待者模型（Future 支持多个）
- `wait_readable` / `wait_writable` 不是 CPython 标准 API（标准 asyncio
  用 `loop.add_reader`）；没有配套的 fd 读写函数，只暴露等待能力
- 事件循环仍是非线程安全的单循环；没有信号处理/子进程

## [0.19] - 2026-11-09

语言面异步收尾：异步生成器、`async for`、`async with`。

### 新增

- `async def` + `yield` 编译成异步生成器（帧 kind=2）；`async for`
  通过 `__anext__` 驱动对象逐个取值，结束时抛 `StopAsyncIteration`
  并在循环边界捕获。
- 异步生成器里可以混用 `await` 和 `yield`；内部 await 的结果由驱动
  对象回填后继续推进，不递归展开调用栈（驱动对象走就绪队列）。
- `async with` 支持类实例的 `async def __aenter__/__aexit__`：进入、
  正常退出、异常展开、`return`/`break`/`continue` 路径都会 await
  `__aexit__`；退出期间的异常上下文先抄进槽再清 pending，避免跨
  挂起污染全局异常状态。
- 类型系统新增 `asyncgen[T]`；`async for` 只接受异步生成器（或
  unknown），`async with` 要求两个协议方法都是协程。
- 用例 `tests/run/26_async_lang.py`；差分测试的 `async` 生成器加入
  async for / async with / 异步生成器场景。

### 验证

- `ctest --preset dev` / `release` 全绿（11 正例 + 33 反例 + 26 运行 +
  4 未建模 + 8 模块目录）
- 差分测试 6 个生成器、810 个程序零差异
- ASan/UBSan/LSan：异步生成器嵌套、async for 中断、async with 的
  异常/取消/嵌套清理零报告
- 16MB 地址空间下 20 轮 × 50 次异步迭代通过

### 已知限制

- 异步生成器只支持 `async for`；没有 `asend` / `athrow` / `aclose`
  方法，GC 回收时同步跑 `finally`（`finally` 里不能有 await）
- 异步生成器被中途抛弃且正停在 `async with` 里时，close 路径无法
  await `__aexit__`（取消仍会正常 await，只有“不再恢复直接回收”没有）
- `async for` / `async with` 只支持异步生成器和类协议，不支持
  `__aiter__` 返回自定义迭代器；异步推导式仍拒绝

## [0.18] - 2026-11-09

asyncio Task：`create_task`、`done()` / `cancel()` 与 `CancelledError`。

### 新增

- `asyncio.create_task(coro)` 立即把协程挂到事件循环，返回 `task[T]`；
  `await task` 取结果（完成后可重复 await），也支持放进 `gather`。
- Task 额外持有协程和结果：完成时保存 result/exception，等待者拿到
  独立引用/异常副本；`loop_pending` 负责让运行中的 Task 活到完成。
- `task.done()` 返回是否结束；`task.cancel()` 请求取消：排队中的任务
  直接注入 `CancelledError`，挂起中的任务先从当前 awaitable
  （协程/sleep/gather/task）解绑再注入，避免迟到的结果写进已结束帧。
- `CancelledError` 加入内置异常表，父类是 `BaseException`（`except
  Exception` 不会误吞取消），`finally` 在取消路径正常执行。
- 帧头增加 `cur`（当前等待的 awaitable，借用观察指针）；Task 的
  magic/kind 复用运行时 awaitable 结构。
- 用例 `tests/run/25_task.py`；差分测试的 `async` 生成器加入
  create_task / cancel 场景。

### 验证

- `ctest --preset dev` 全绿（11 正例 + 33 反例 + 25 运行 + 4 未建模 +
  8 模块目录）
- 差分测试 6 个生成器，810 个程序零差异（`async` 种子 5 单独跑到 150）
- ASan/UBSan/LSan：Task 完成/重复 await/取消（sleep、gather、finally）
  零报告

### 已知限制

- Task 只允许一个并发等待者；重复 await 已完成任务会返回保存的结果
- 取消是协作式的：被取消协程若在 except 里继续 await/return，Task
  会按正常完成处理（与 CPython 一致）；不会级联取消它等待的 gather
- 没有 `Task.result()` / `exception()`、`current_task`、任务名字、
  取消消息，也没有 `asyncio.CancelledError` 的模块限定写法
  （直接用内置名 `CancelledError`）

## [0.17] - 2026-11-09

协程：`async def` / `await` 复用生成器状态机，事件循环 + 定时器 + gather。

### 新增

- `async def` 编译成协程帧（与生成器同构，kind 字段区分）；调用返回
  `coroutine[T]`，`await` 是第三种挂起状态，结果通过帧的 sent 槽送回。
- `asyncio` 内置模块：`run(coro)`（返回协程结果）、`sleep(seconds)`
  （毫秒定时器）、`gather(*awaitables)`（并发启动、按参数顺序收集结果）。
  模块限定调用有精确结果类型；`from asyncio import sleep` 也可用。
- 完整事件循环：就绪队列 + 定时器列表，都是注册过 GC 根的对象表；
  `loop_pending` 持有尚在运行的 gather。`sleep` 用 `CLOCK_MONOTONIC`
  定时，零延时立即完成。
- 异常跨 `await`：被等待协程失败时，把异常注入等待者帧，恢复路径先
  重挂 handler 再抛出，`try/except` 能抓到；根协程失败会抛回
  `asyncio.run` 的同步调用点。
- async 方法、返回 `await`、`with` 内的 `await`、跨 `await` 的
  `finally` 都支持；协程帧参与 RC/GC，未被 await 的协程由 finalize 清理。
- 用例 `tests/run/24_async.py`；差分测试新增 `async` 生成器。

### 验证

- `ctest --preset dev` 全绿（11 正例 + 33 反例 + 24 运行 + 4 未建模 +
  8 模块目录）
- 差分测试 6 个生成器，3 种子 810 个程序零差异
- ASan/UBSan/LSan：asyncio.run/sleep/gather/异常注入/with/finally 零报告
- 3 次 50ms `sleep` 实际耗时约 0.15s；16MB 地址空间下 500 次 await 通过

### 已知限制

- `await` 只能作为语句、赋值右值或 `return` 值；`yield`/`await` 不能
  出现在 `finally`/`except` 体内（`finally` 可用于跨 await 清理）
- 不支持 `async for`、`async with`、异步生成器、`create_task`、取消、
  Future 协议和 fd 级 I/O 轮询；`gather` 参数个数在源码里写死
- `gather` 结果的元素类型按 `unknown` 处理（容器只 retain 不 release）
- 跨模块协程类型降级成 unknown

## [0.16] - 2026-11-09

生成器：状态机 + 堆帧，支持跨 `yield` 的异常与清理。

### 新增

- 含 `yield` 的函数编译成「创建包装 + resume 状态机」：调用返回
  生成器对象；局部槽位放在堆帧里，`yield` 只保存 state 并返回。
- 类型系统新增 `Generator[T]`：yield 值类型取公共上界，`for` / `next` /
  `.send` / `.close` / `.__next__` 接入；返回生成器的普通工厂函数不再
  被误判成生成器函数。
- `for x in gen` 走 `py23_gen_next_opt`，结束时不建异常；`next(gen)`
  在结束时抛 `StopIteration`；`send()` 把值送入 `x = yield v`。
- **跨 yield 清理**：`yield` 时把活跃 handler 记录从全局栈摘下，恢复时
  按原顺序重挂并重新 setjmp；`close()`、引用计数归零和 GC 前的
  finalize 会按挂起点的快照执行 `finally` 与 `with.__exit__`。
- 生成器帧参与 RC/GC：堆槽在帧销毁时 dec，槽位由 GC 保守扫描；
  异常逃出生成器时标记 aborted，避免清理跑第二遍。
- 用例 `tests/run/23_generator.py`（基本迭代、send、with/finally、
  handler 重挂、闭包捕获、丢弃清理）。

### 验证

- `ctest --preset dev` 全绿（11 正例 + 33 反例 + 23 运行 + 4 未建模 +
  8 模块目录）
- 差分测试新增 `gen` 生成器，3 种子 720 个程序零差异
- ASan/UBSan/LSan：生成器用例与异常/清理组合零报告
- 严格警告零报告

### 已知限制

- `yield` 只能作为语句或赋值右值出现；不支持 `yield from`、生成器
  表达式惰性求值、`yield` 位于 `finally` 体内
- `yield` 位于 `except` 体内暂不支持（跨挂起保存异常上下文）
- 跨模块接口里生成器类型降级成 unknown，生成器函数跨模块调用未支持

## [0.15] - 2026-11-09

包、相对导入与静态 `from m import *`。

### 新增

- 包目录（必须带 `__init__.py`）和点号模块名解析：
  `import pkg`、`import pkg.mod as m`、`from pkg.mod import x`。
- 相对导入按当前包重写：`from . import mod`、`from .mod import x`、
  `from ..pkg import y`。
- `from pkg import mod` 中子模块绑定为模块别名；普通名字保持快照绑定。
- `from m import *` 静态展开非下划线全局/类/原生符号。
- 模块符号前缀统一转义点号；缓存路径按完整模块名 + 源路径哈希。
- 用例 `tests/modules/packages`。

### 验证

- `ctest --preset dev` 全绿（8 个模块目录）
- 差分测试 600 个程序零差异

### 已知限制

- 包 `__init__` 不会因为导入子模块自动执行，需要显式 `import pkg`
- 点号 import 必须带 `as`
- `__all__`、命名空间包、跨模块继承未支持

## [0.14c] - 2026-11-09

内置最小标准库：`math` / `sys`。

### 新增

- `math` 常量 `pi`/`e`/`tau` 编译期折叠；`sqrt/floor/ceil/fabs/sin/cos/
  tan/log/log2/log10/exp/pow/fmod` 映射到运行时包装，`floor`/`ceil`
  返回 int，`ValueError` 覆盖 sqrt/log*/fmod 的典型域错误。
- `sys.argv`：生成的 `main(argc, argv)` 初始化字符串列表，并立即注册
  为 GC 根；通过 `from sys import argv` 也能快照到本地。
- `sys.exit(code)` 直接结束进程；`sys.maxsize` 是 int64 最大值。
- `import math as m` / `from math import sqrt as s` 都支持；原生函数
  只能直接调用，当值传递会报错。
- 用例 `tests/modules/builtins`。

### 验证

- `make test`：11 正例 + 33 反例 + 22 运行 + 4 未建模 + 7 模块目录
- 差分测试 600 个程序零差异；严格警告零报告；release 构建通过
- 编译出的程序带参数运行，`sys.argv` 含程序名和实参；`sys.exit(3)`
  退出码为 3

### 构建迁移

- 构建系统从 Makefile 迁到 **CMake 3.28 + Ninja**：`CMakePresets.json`
  提供 dev/release 两个 preset，默认生成 `compile_commands.json`。
- 运行时 `rt.o` 由 CMake 自定义命令生成，编译器宏里的绝对路径由
  `CMAKE_BINARY_DIR` 注入；`clean-modcache` 目标清理模块缓存。
- 原 Makefile 的测试 shell 逻辑抽到 `tests/run_tests.sh`，由 CTest
  注册为 `py23_suite`：`ctest --preset dev`。

## [0.14b] - 2026-11-09

跨模块类：构造包装、扁平类接口、字段/方法跨模块访问。

### 新增

- `.py23i` class 记录：扁平字段（根→派生顺序）、方法名/槽号/调用签名、
  `has_init`、构造签名、`ctor_symbol`；格式版本升到 2。
- 定义模块为每个类生成构造包装 `py23$m$n$C`（ObjNew + 字段初值 +
  `__init__`），模块 init 绑定到隐藏全局。
- 导入方支持 `import m; m.C(...)`、`from m import C`，字段读写和方法
  调用使用导出的偏移/槽位；继承类按最终布局拍平。
- 构造函数支持默认参数和关键字参数；导出类 `__init__` 的
  `*args`/`**kwargs` 暂不支持。
- 类类型出现在跨模块函数签名里降级成 unknown；跨模块继承明确拒绝。
- 用例 `tests/modules/classes`。

### 验证

- `make test`：11 正例 + 33 反例 + 22 运行 + 4 未建模 + 6 模块目录
- 差分测试 600 个程序零差异
- ASan/UBSan/LSan：6 个模块用例零报告
- 严格警告零报告；release 构建通过

## [0.14a] - 2026-11-09

分模块 import 的第一阶段：模块图、二进制接口、独立 `.o` 与增量缓存。

### 新增

- **模块图**：入口目录 + `-I` 搜索路径；单层模块 `import m` /
  `import m as n` / `from m import x as y`；点号/相对/星号 import
  明确报错；循环导入编译期检测。
- **二进制 `.py23i`**：模块全局名 + 递归类型编码 + 函数签名（含
  `posonly`、`kwonly`、`*args`/`**kwargs`、字面量默认值）+ FNV-1a
  校验；文本 manifest 记录源码哈希、依赖接口哈希和 ABI 版本。
- **独立编译与链接**：每个模块汇编成独立 `.o`，入口、依赖和 `rt.o`
  链接；入口也走缓存。模块符号 `py23$m$g$x` / `ready` / `init` /
  `roots`。
- **惰性初始化**：`import` 在语句点调用 `py23_import_module`，模块顶层
  只执行一次；模块 init 先 `py23_gc_add_roots` 注册全局根表，不释放
  全局也不 shutdown。
- **跨模块名称**：`m.x` 直接读写外部全局，`m.f()` 调用外部闭包；
  `from m import x` 在导入点做快照绑定；`__name__` / `__file__` 按模块
  设置，入口是 `"__main__"`。
- **缓存**：manifest 命中时复用 `.o` / `.py23i`；源码体变化但接口不变
  只重编模块本身，接口变化才重编依赖方；`--rebuild` 强制全量。
- 用例 `tests/modules/{basic,lazy,chain,values,gcroot}`；`make test`
  同时验证运行输出和二次构建缓存命中。

### 验证

- `make test`：11 正例 + 33 反例 + 22 运行 + 4 未建模 + 5 模块目录
- 差分测试 600 个程序零差异
- ASan + UBSan + LSan：单文件运行用例 + 5 个模块用例零报告
- 严格警告零报告；release 构建通过
- 模块缓存：源码体变化不重编导入方，接口变化重编导入方

### 已知限制

- 只支持本地单层 `.py`；不支持点号模块、相对导入、`import *`
- 跨模块类（0.14b）和内置 `math` / `sys`（0.14c）尚未接入
- 模块导出类型含 class 时 `.py23i` 无法编码
- 模块初始化失败后 ready 已置位，不会重试
- 缓存不跟踪编译器版本；编译器 ABI 变化时需要 `--rebuild` 或清理
  `modcache`

## [0.13] - 2026-11-09

这一版实现异常：`try/except/else/finally`、`raise`，以及 `with` 的异常
退出路径。模型按 0.12 末尾定下的方案：setjmp/longjmp + 运行时 handler
栈，异常对象是 `{类型名, 消息}`。

### 新增

- **handler 栈**：`try` / `with` / `finally` 在运行时压入
  `{jmp_buf, prev, temp_mark}`；`raise` 设置全局异常后 `longjmp` 到最内层
  landing pad。landing pad 先清临时值栈到记录水位，再执行 `except` 匹配、
  `with.__exit__` 或 `finally`。
- **异常对象**：内置异常名 + 消息字符串。`except` 按名称和层级匹配
  （`KeyError → LookupError → Exception` 等）；`as e` 绑定的是**消息
  字符串**。未捕获异常打印 `py23: uncaught ValueError: msg` 后 `exit(1)`。
- **try/except/else**：多个 handler、tuple 类型 `except (A, B)`、bare
  `except:`、`else`；handler 体再次抛异常时先 `ExcDone` 再继续展开。
- **try/finally**：正常、异常、`return`、`break`、`continue` 都会执行
  finally；`try/except/finally` 组合按嵌套语义工作。
- **raise**：`raise ValueError`、`raise ValueError(msg)`、bare `raise`
  （重抛当前异常）。`raise ... from` 和自定义异常类暂不支持。
- **with 异常路径**：异常时 `__exit__(type_name, message, None)`；返回真值
  抑制异常，否则重新抛。多个 `with` 项按逆序退出；`__enter__` 抛异常时
  外层 `__exit__` 仍会执行。
- **运行时错误可捕获**：`IndexError`、`KeyError`、`ZeroDivisionError`、
  `OverflowError`、`ValueError`、`AssertionError`、`AttributeError`
  （None 属性访问）不再直接 `exit(1)`。
- **寄存器安全**：含 handler 的函数关闭槽位寄存器提升，避免 longjmp 把
  callee-saved 寄存器回滚到 setjmp 时刻。
- 运行用例 `22_exception`；异常压力程序覆盖跨帧 raise、handler 内重抛、
  with 异常与 finally 异常。

### 验证

- `make test`：11 正例 + 33 反例 + 22 运行用例 + 4 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：全部 13 个内存相关运行用例零报告
- 严格警告零报告；release 构建通过
- 16MB `RLIMIT_AS`：10 万次跨帧异常、5 万次 handler 内重抛、5 万次
  with 异常、5 万次 finally 异常，峰值 RSS 约 7.8MB

### 已知限制

- 异常对象只有类型名和消息；`as e` 是字符串，没有 `e.args`、traceback、
  `raise ... from` 或异常链
- 只支持内置异常名；自定义异常类和方法装饰器一样仍被后端拒绝
- `finally` 体内如果含跳出 finally 的 `return` / `break` / `continue`
  会直接报错；循环内部的跳转是允许的
- 异常跳过普通函数帧时不执行 `release_slots`，局部引用交给保守 GC；
  异常路径的回收时机不如正常路径精确
- handler 记录和异常对象是 malloc 的，不参与 RC/GC；未捕获异常直接退出

## [0.12] - 2026-11-09

这一版补齐路线图里除 `try/except`、生成器 / `async`、`import` 之外的
构造：删除类方法、切片赋值、变长实参、推导式、`with`、函数装饰器。

### 新增

- **dict/set 删除方法**：`pop(k[, default])`、`popitem()`、`remove(k)`、
  `discard(k)`、`clear()`、`set.pop()`。开放寻址表没有墓碑，删除后按
  `order` 剩余项整表重建（单次 O(n)）；`popitem` / `set.pop` 固定取最后
  插入项，遍历顺序和 RC 归属保持稳定
- **切片赋值**：`l[a:b:c] = value`。`step == 1` 支持替换 / 插入 / 删除
  （`memmove` 处理长度变化），扩展切片要求右侧长度一致；右侧允许
  list / tuple / str（str 按字节拆成字符列表）；写入按目标 `elem_kind`
  retain 新元素、dec 旧元素，`src == dst` 先快照
- **`*args` / `**kwargs` 形参与实参**：形参在 IR 里是末尾的 `list[T]` /
  `dict[str, V]` 指针；调用点把多余位置实参打包进 list、关键字实参打包
  进 dict。支持 `f(1, *lst)`、`f(*t)`、`f(**d)`、`inner(*args, **kw)`
  转发；定长 tuple 的 `*` 静态展开；方法、构造器、闭包捕获同样可用
- **推导式**：list / set / dict 推导式与生成器表达式（立即按 list 求值），
  支持多层 `for`、每层 `if`、嵌套推导和独立变量作用域；`range()` 现在
  可以作为值求值（实体化成 list[int]，步长可以是任意 int 表达式）
- **`with`**：类实例的 `__enter__` / `__exit__`，支持 `as`、多个项、
  嵌套；正常结束、`return`、`break`、`continue` 都会按逆序补调
  `__exit__(None, None, None)`
- **函数装饰器**：`@d1 @d2 def f` 按从下到上执行 `f = d1(d2(f))`，
  复用普通闭包调用；装饰器必须是签名已知的函数值
- **类型检查**：定长 tuple 的常量下标返回精确元素类型
  （`d.popitem()[0]` 不再退化成 unknown）；装饰后的函数名绑定到装饰器
  返回的函数签名，调用点可以正常做参数检查与数值转换
- **unknown 保守 retain**：unknown 值写入 cell / 容器前调用
  `py23_rc_inc_unknown`，运行时按堆注册表判断是否堆指针再 +1。修复了
  装饰器参数 `f` 这类 unknown 捕获在源引用释放后悬空的 UAF；注册表索引
  用墓碑复用 + 按存活数压缩，长循环下不膨胀
- 运行时新增 `py23_dict_pop/popitem/pop_last/remove/clear`、
  `py23_list_slice_assign`、`py23_list_extend`、`py23_str_to_list`、
  `py23_range_new`、`py23_rc_inc_unknown`
- 运行用例 `16_dict_del` ~ `21_decorator`；未建模用例
  `decorated_method` / `class_decorator`

### 验证

- `make test`：11 正例 + 33 反例 + 21 运行用例 + 4 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：全部 12 个内存相关运行用例零报告
- 严格警告（`-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual`）零报告
- 16MB `RLIMIT_AS`：10 万次删除 / 推导式 / 切片赋值 / `with` /
  装饰器 / 变长实参调用（共 50 万轮）峰值 RSS 约 7.8MB

### 已知限制

- 删除是 O(n) 整表重建；大表高频删除会退化，见 roadmap 技术债
- 通过 unknown 类型函数值调用时不能带 `*` / `**`（没有运行时签名）；
  `*` 位于固定形参之前时只支持定长 tuple 的静态展开
- 同一个 `**mapping` 的键与命名形参重名时不会报 `TypeError`，会静默
  留在 `**kwargs` 里
- `print(*args)` 仍未展开；生成器表达式立即求值；推导式里的 `lambda`
  暂不能捕获循环变量
- 类装饰器、方法装饰器（`@staticmethod` / `@property` 等）仍被后端拒绝
- `with` 的 `__exit__` 只在正常和 `return` / `break` / `continue` 路径
  调用；异常路径要等 `try/except` 的展开机制

## [0.11] - 2026-11-09

这一版补齐容器常用方法和**切片（读）**。

### 新增

- **dict 方法**：
  - `get(k[, default])`：有默认值走 `py23_dict_get_or`；没有默认值时
    缺失像 `[]` 一样 panic（静态返回类型不是 Optional）
  - `keys()` / `values()` / `items()`：返回新 list；`items()` 的元素是
    `(key, value)` 元组，按插入顺序；元素按 key_own/val_own 接入 RC
  - `update(other)`：逐项复制，保留目标字典的 key 所有权规则；目标
    unknown 时会 retain 源引用，避免源临时字典销毁后悬空
- **set 方法**：`add(x)`、`update(other_set)`（复用同一张 dict 表）
- **切片（读）**：`s[a:b:c]` 支持 str / list / tuple，缺省边界、负索引、
  正负步长都按 Python 语义；长度和边界在运行时归一化，结果列表按源
  `elem_kind` 接管元素引用
- `py23_list_slice` / `py23_str_slice`、`py23_dict_get_or/keys/values/
  items/update`
- `tests/run/15_slice.py`；`tests/run/14_dict.py` 扩展方法用例

### 验证

- `make test`：11 正例 + 33 反例 + 15 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：全部运行用例 + 方法/切片压力程序零报告
- 严格警告零报告
- 16MB `RLIMIT_AS`：10 万次列表/字符串切片、10 万次 keys/values/items、
  5 万次 dict.update，峰值 RSS 约 8MB

### 已知限制

- 删除类方法（dict/set 的 `pop`、`remove`、`discard`、`clear`、
  `popitem`）仍未生成代码；开放寻址表需要墓碑或重哈希
- `keys()` / `values()` / `items()` 返回 list，不是 CPython 的视图；
  修改原字典不会反映到已取出的列表
- `get(k)` 缺省时 panic；带默认值的 `get(k, default)` 按默认值返回
- 切片赋值未实现；`step == 0` 运行时 panic
- 未知类型列表的切片元素只做保守 GC，不做精确所有权转移

## [0.10] - 2026-11-09

这一版补上**字典 / 集合**和 `in` 运算符。类型检查层早就有 `dict`/`set`
类型，这一轮把运行时表示、降级和 GC 接上。

### 新增

- **开放寻址哈希表**：`Py23Dict {len, cap, entries, order, key_kind,
  key_own, val_own}`，`order` 保留插入顺序；`len` 在偏移 0，`len()` 和
  真值判断都是一条读
- **key 类别**：int/bool、str（FNV-1a）、float（规范化 -0.0）、其他按
  机器字比较；字符串 key 用 `py23_str_eq` 判等
- **key/value 所有权**：`key_own` / `val_own` 区分非堆、堆、unknown；
  插入时降级器 retain，更新/销毁时运行时 dec；unknown 靠 GC 兜底
- **set 复用 dict**：`val_own = 0`、值恒 0；集合字面量、`in`、`for`
  都走同一张表
- **下标读写**：`d[k]` → `py23_dict_get`（缺失 panic KeyError），
  `d[k] = v` → `py23_dict_set`；key 按字典声明类型做数值转换
- **迭代**：`for k in d` / `for x in s` 按下标 `py23_dict_key_at` 取 key
- **`in`**：dict/set 用 `py23_dict_contains`；list/tuple 用
  `py23_list_contains`（int/str/float/raw 四种比较）；str 子串用
  `py23_str_contains`；支持 `not in`
- **GC**：`PY23_RC_DICT` 扫描条目 key/value，清扫时用 `dict_gc_release`
  只减可达对象的引用；`entries` / `order` 在 dtor/GC/退出时释放
- `tests/run/14_dict.py`；`tests/run/13_gc.py` 增加字典自环

### 修复

- **插入空槽的经典 bug**：`dict_probe` 查不到返回 -1，早期直接拿 -1
  当插入槽，写坏 `entries[-1]`；增加 `dict_empty_slot` 专门找空槽
- 字典/集合现在属于堆类型：槽、全局、字段、列表元素、闭包 cell 都按
  引用计数管理，GC 也能扫描/释放

### 验证

- `make test`：11 正例 + 33 反例 + 14 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：全部运行用例 + 字典/集合压力程序零报告
- 严格警告零报告
- 16MB `RLIMIT_AS`：10 万次字典创建、10 万次字典自环、10 万次集合、
  10 万次字典-列表创建，峰值 RSS 约 8–9MB

### 已知限制

- dict/set 的**方法**（`get`/`keys`/`items`/`add`/`update` 等）类型检查
  放行，但 `build` 仍拒绝；当前支持的是字面量、下标、`len`、`in`、`for`
- 空字典 `{}` 的 key 类型是 unknown；不同对象但内容相同的动态字符串
  key 按 unknown 的机器字比较，可能不相等。标注/推出来的 str key 没有
  这个问题
- 混合 value 类型的字典值会退化成 unknown（类型系统限制），此时再嵌套
  下标会按未知容器处理
- dict/set 相等比较仍是指针比较；没有深比较
- 键类型没有运行时标签，不能像 CPython 那样混用任意 hashable

## [0.9] - 2026-11-09

这一版补上两件事：**lambda** 和**循环泄漏收集器**。闭包从「无环才能回收」
变成「不可达的环也能回收」，`lambda` 直接复用同一套闭包机制。

### 新增

- **lambda 脱糖**：解析后把 `lambda` 展开成插在语句前的合成嵌套 `def`
  （名字形如 `__py23$lambda_N`），表达式位置替换成这个名字。类型检查、
  闭包捕获、cell、引用计数全部复用普通函数路径
- **签名携带默认表达式**：`Param::default_expr` 指向 AST 默认值，通过
  函数值 / lambda 调用时也能内联填充默认参数（以前只认 `def` 名字）
- **保守标记-清扫收集器**：
  - RC 头扩成 64 字节：注册表链、dtor、GC release、负载大小、类型、标记位
  - 所有 `rc_alloc` 对象进堆注册表；分配量超过 2MB 自动触发一轮收集
  - 根：全局根表（生成代码在 `main` 里注册）、临时值栈、真实栈扫描、
    `setjmp` 捕获的 callee-saved 寄存器
  - 标记阶段保守扫描列表元素、对象字段、闭包 cell；清扫阶段先只对
    「不可达对象 → 可达对象」的引用做 rc 递减（`release` 回调），
    再统一释放，避免释放顺序问题
  - 入口函数退出时 `py23_gc_shutdown` 清空剩余堆对象（含环），
    LSan 零报告
- 生成代码：全局根表 `.Lgroots`、`py23_gc_init`、每个有堆字段的类多一个
  `.LgcrelN`（GC 用 release），对象分配传 `dtor + release`
- `tests/run/12_lambda.py`、`tests/run/13_gc.py`

### 修复

- **循环引用泄漏**：对象自环、相互递归的嵌套闭包、`a.next = a` 不再
  无限增长；16MB 限制下 20 万次环创建 / 30 万次闭包垃圾通过
- **unknown 容器泄漏**：`list[unknown]` 里的字符串/对象由 GC 扫到，
  不可达时回收（不再依赖 kind 2 的逐个 dec）
- **函数值默认参数**：`f = lambda x, y=3: ...`、把函数存进变量再调用
  默认参数不再报「不是字面量常量」
- **保守扫描与 ASan**：用真实 `%rsp` 而不是局部变量地址作为栈扫描起点，
  ASan 的 fake stack 不再被误扫

### 验证

- `make test`：11 正例 + 33 反例 + 13 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan + LSan：编译器全量、全部运行用例、GC 压力程序零报告
- 严格警告零报告
- 16MB `RLIMIT_AS`：对象环 20 万次、相互递归闭包 20 万次、unknown 容器
  20 万次、闭包垃圾 30 万次，峰值 RSS 约 8MB；同时验证局部/全局存活列表
  和闭包跨 GC 不被误收
- 性能：纯整数路径不分配堆，GC 不触发，`fib(35)` / 2 亿循环与 0.8 持平

### 已知限制

- GC 是**保守**的：栈/寄存器里的整数如果恰好等于堆地址会被误认为根，
  只会延迟回收，不会提前释放
- unknown 容器里指向「仍有其它根」的对象的引用不会做 rc 递减；这些
  计数可能虚高，对象要等下一轮不可达后由 GC 回收
- 没有 `__del__`、弱引用、分代/增量；每次收集是 stop-the-world 全堆扫描
- 类体字段默认值里的 `lambda` 暂不脱糖（类体只放声明，`build` 会拒）
- `lambda` 在条件/循环表达式里会在包含它的语句前创建一次，而不是每次
  求值都新建；调用语义一致，函数对象身份/创建时机与 CPython 略有差异

## [0.8] - 2026-11-09

这一版加的是**闭包**：内层函数捕获外层局部变量，支持 `nonlocal`、
多实例（计数器工厂）、多层嵌套、自递归和方法里捕获 `self`。

### 新增

- **cell 模型**：被内层引用的局部变量在函数入口转成单元素 `list[T]`，
  cell 指针槽、闭包 env 和变量访问都复用现有列表/引用计数机制
- **`nonlocal`**：类型检查把声明预收集到函数级；赋值解析到最近的外层
  函数作用域，不再误当成本地变量
- **捕获分析**：类型检查在 `lookup` 命中祖先函数作用域时记录自由变量，
  降级期自底向上传播；子函数要的名字如果在本函数有绑定就生成 cell，
  否则继续往上要，多层嵌套也能拿到正确 cell
- **闭包对象**：负载 `[code, n_caps, cell…]`；无捕获函数用
  `.data.rel.ro` 静态闭包（rc = INT64_MIN），有捕获才运行时分配
- **函数值 ABI**：`Func` 变成堆类型；调用时从闭包 +0 取代码，闭包指针
  经 `%r10` 传给被调函数，入口存进 env 槽；方法调用显式清 `%r10`
- **自递归优化**：嵌套函数直接引用自己的名字时使用当前闭包/env，不生成
  self cell 环；`make_fact()` 这类递归工厂不再泄漏
- `tests/run/11_closure.py`、`tests/fail/f34_nonlocal.py`
- 运行时 `py23_closure_new` / 闭包析构器

### 修复

- **`global` / `nonlocal` 预收集**：两者都作用于整个函数，先收集再 hoist；
  避免局部预声明盖住外层绑定
- **函数值 RC**：函数值不再当成裸代码地址，存储/传参/返回/容器都按堆值
  参与引用计数；静态闭包永生，运行时跳过 inc/dec
- **`list[T]` 下标赋值漏 retain**：0.7 里 `l[i] = v` 只在运行时 dec 旧值，
  没有 retain 新值；存字符串/函数值时会悬空或提前释放。补上 retain
- IR dump 的 `func.addr` / `func.code` 打印 `imm` 而不是未使用的 `idx`

### 验证

- `make test`：11 正例 + 33 反例 + 11 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan：编译器全量 + 全部运行用例零报告
- 严格警告零报告
- 16MB `RLIMIT_AS` 下 20 万次闭包工厂创建（含字符串捕获和自递归）
  峰值 RSS 约 8MB
- 性能与 0.7 持平：`fib(35)` 约 0.06s（同机 CPython 3.13 约 0.6–1.0s）

### 已知限制

- `lambda` 仍未实现（语法能解析，`build` 拒绝）
- 相互递归的嵌套闭包会形成 cell 环，环不回收
- 嵌套函数直接引用自己的名字按「调用当前闭包」处理；父作用域之后重绑
  这个名字不会影响它（与 CPython 的晚绑定略有差异）
- 闭包捕获的是函数作用域变量；类作用域、生成器作用域不参与捕获

## [0.7] - 2026-11-08

这一版加的是**内存管理**：引用计数。所有权由静态类型决定，表达式临时值
进运行时临时值栈，语句 / 循环 / 函数边界按 mark 清理，不需要做活跃性分析。

### 新增

- **隐藏 RC 头**：每个堆对象前面 16 字节 `{rc, dtor}`，负载指针和所有字段
  偏移保持不变（字符串 `len` 仍在 +0，对象 vtable 仍在 +0）
- **字面量永生**：`.data.rel.ro` 里的字符串前置 `rc = INT64_MIN`，inc/dec 跳过
- **临时值栈**：`py23_rc_mark` / `py23_rc_temp` / `py23_rc_cleanup`；语句、
  `while` / `for`、函数返回值都按水位清理
- **所有权规则**：槽 / 全局 / 字段 / kind 1 列表元素持有引用；load /
  `field.get` / `list.get` 借用；形参入口 retain、出口释放；返回值 retain
- **容器元素类别**：`py23_list_new(cap, elem_kind)`；kind 1 的元素在覆盖、
  截断、销毁时由运行时 dec；kind 2（unknown / 混合）只 retain 不 release
- **对象析构器**：codegen 按 `IRClass::field_heap` 生成 `.LdtorN`，只 dec
  堆字段；`py23_obj_new(size, dtor)`
- **RC 脚手架消除**：语句 / 循环 / 函数完全没有临时堆值时，mark 和 cleanup
  在降级期删掉；纯整数代码的生成结果与 0.6.1 完全相同
- `tests/run/10_rc.py`：覆盖别名、字段、列表、返回值、字符串迭代、
  `continue` 跳过清理等场景
- 入口函数返回前释放全局变量并 `py23_rc_shutdown`，保持 ASan 零泄漏基线

### 修复

- **`list[str]` 下标被当成字符串下标**：结果类型都是 `str`，误判后把借用
  元素入临时栈，ASan 报 heap-use-after-free；改为看容器类型
- **`and`/`or` 在 unknown 类型下泄漏**：内部临时槽 retain 了堆值却永不释放；
  这类表达式内部槽改成纯借用，生命周期交给临时栈
- **`continue` 跳过语句清理**：`if ...: continue` 会把条件临时值留到下一轮；
  `while` / `for` 都加循环水位，body / incr / 退出路径兜底
- **遍历字符串泄漏单字节串**：`for ch in "abc"` 的下标是 `str_char_at`
  新建的串，之前按借用处理；现在入临时栈
- `-Wshadow`：类字段继承链的循环变量重名

### 验证

- `make test`：11 正例 + 32 反例 + 10 运行用例 + 2 未建模
- 差分测试 3 种子 600 个程序零差异
- ASan + UBSan：编译器全量检查 + 全部运行用例（含生成代码走 ASan 运行时）
  零报告
- 严格警告零报告
- 16MB `RLIMIT_AS` 下完成 100 万次拼接、100 万次对象分配、30 万次带字符串
  字段的对象分配、30 万次列表构造，峰值 RSS 约 8MB
- 性能（同机容器）：整数 `fib(35)` 与 2 亿次循环与 0.6.1 持平；2000 万次
  方法调用因 self 形参 inc/dec 慢约 30%

### 已知限制

- **循环引用不回收**：`a.next = a` 会一直活着，没有标记-清扫
- **unknown 值泄漏**：unknown 标量、`list[unknown]`、混合类型元组元素不参与
  计数
- 入口函数退出时会释放全局变量；程序中途退出（panic）不保证清理

## [0.6.1] - 2026-11-08

对象模型落地后跑标准库基线，发现类的检查把真实代码判死了一大片（35693 个语法通过的文件里 19064 个类型报错）。这一版全是**放宽**：按「宁可漏报，不可误杀」把静态上看不见的东西降级成 unknown。

### 修复

- **`class A(object)` 被判死**：`object` 在内置表里是函数（`object()` 造实例），不是 `ClassInfo`。基类是内建类型名时按根类处理
- **外部基类的方法全丢**：`class T(unittest.TestCase)` 之后 `self.assertEqual(...)` 报「没有此方法」。`ClassInfo` 新增 `open` 标记（整条继承链生效），查不到的成员按 unknown 放过
- **`self.x = ...` 要求先写标注**：现在就地补成隐式字段，类型按首次赋值推断；`None` 与 unknown 不参与推断（否则后面每次 `self.x.foo()` 都被判死）
- **类体里的普通语句被拒**：`x = 1` / `if` / `for` 改成标记「没建模」，并把类标成 open（类属性查不到）；`build` 时才拒绝
- **多继承被拒**：改成标记「没建模」+ `build` 拒绝。标准库里的测试类大量是 `class T(Mixin, TestCase)`
- **`self.method()` 拼错也报错 → 现在放过**（mixin 组合出来的成员静态看不见），类外的 `p.m()` 仍然检查
- **骨架类被当成空类**：类定义在引用点之后时（`def f(): return C(1)` 写在 `class C` 前面），骨架的 `complete == false`，构造参数和成员查找一律放宽
- **嵌套函数里的 `self`**：`def last(): self.x` 这类闭包捕获，之前因为 `cur_self_name_` 被 `std::move` 移空而失效
- **隐式字段的类型被后来的赋值覆盖**：`self.x = f()` 后 `self.x = None` 把 unknown 改成了 None，`join` 语义修正为 unknown 吸收
- **`@staticmethod` 的零参方法**报「缺 self」：带装饰器的方法不检查 self，也不给调用签名（实参不检查）
- **`MyList[int]` 泛型下标**被报「不可下标」：类对象下标返回 unknown
- **`duplicate method`**：`@property` 的 getter / setter 是同名两个 `def`，Python 里合法（后者覆盖）。现在两者都检查、共用一个槽，vtable 里后者胜出
- **`self.x = v` 撞上同名方法**：可能是 `@property` 的 setter，静态分不清，放过并标记；类外的 `p.x = v` 仍然报错
- **`c.__annotations__ = d`**：给 dunder 属性赋值不再报「不能给方法赋值」
- **类实例的运算符 / 解包 / 比较 / `len()`**：`Fraction - Fraction`、`enum.FlagBoundary` 解包这类全被误判。类可能重载了 dunder 协议（重载还可能在外部基类里），我们没建模就一律放过
- **`set[a] < set[b]`**：集合子集比较放行
- `@property` 返回的数值参与一元运算（`bad operand type for unary operator`）同样放过

### 新增

- `tests/unsupported/` 测试类：`check` 必须打标记、`build` 必须拒绝，防止「标了没建模却悄悄编译过去」
- `tests/run/09_class.py` 增加无标注字段的用例

## [0.6] - 2026-11-08

这一版加的是**对象模型**：类、字段、方法、单继承、虚调用。类在编译期定死布局，属性访问退化成一条固定偏移的读。

### 新增

- `class` 语句：方法定义 + 带标注的字段声明 + `pass` + 文档字符串
- 字段偏移在编译期确定：对象 = `[vtable 指针][字段…]`，`p.x` 是 `movq 8(%rax), %rax`
- 单继承：子类字段接在父类之后，vtable 槽位前缀与父类一致；覆盖沿用同一槽
- 虚调用 `vcall`：从对象头取 vtable，按常量槽号间接跳转，父类类型能调到子类实现
- 构造：`Cls(...)` = 分配 + 初始化带初值的字段 + 调 `__init__`；支持默认参数与关键字参数
- 字段初值（`x: int = 0`）在每次构造时求值，实例之间互不影响
- 类型系统新增 `TyKind::Class`：子类可赋给父类、`join` 取较浅的公共上界、`isinstance` 之外的多态检查
- `Optional[Cls]` 上的字段访问与方法调用带运行时空指针检查
- `print(obj)` 输出 `<Point object>`
- IR 新指令：`obj.new` / `field.get` / `field.set` / `vcall`；`IRModule::classes`
- 运行时 `py23_obj_new(size)`（`calloc`，全零）
- `tests/09_class.py`（类型正例）、`tests/run/09_class.py`（运行用例）、6 个类相关反例

### 修复

- **类型系统与类信息的 shared_ptr 环**：`ClassInfo` 的字段类型指回 `Type`，`Type` 又指回 `ClassInfo`，ASan 报 2 万字节泄漏。`Type::cls` 与 `ClassInfo::base` 改成裸指针，所有权归 `TypeChecker::all_classes_`
- 顶层函数签名引用「后面才定义的类」时解析成 `unknown`：`collect_toplevel` 现在先给每个类建骨架，`classdef` 再往同一个对象里填
- `Optional[Cls]` 上的属性赋值（`n.next.next = x`）被误报成「没有可赋值的属性」

### 验证

- `make test`：11 正例 + 33 反例 + 9 个运行用例
- 差分测试 8 个种子、1600 个程序，零差异
- ASan + UBSan 零报告（含零泄漏）
- 严格警告（`-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual`）零报告
- 性能：2000 万次方法调用 0.09s（CPython 3.88s）

## [0.5] - 2026-11-08

这一版只做一件事：**把值从栈上搬进寄存器**。IR 一行没改，全部在 `codegen.cpp`。

### 新增

- 变量提升：按 `load.slot` + `store.slot` 次数排序，最热的 5 个局部变量整函数常驻 `rbx` / `r12`–`r15`（callee-saved，跨调用不需要 spill）
- 块内别名：`%v = load.slot x` 且 %v 只被读一次时，%v 直接指向 x 的寄存器，load 整条消失
- 结果融合：`%v = ...` 紧跟 `store.slot x, %v` 时，%v 就地写进 x 的寄存器，store 整条消失
- 常量延迟：`%v = const.int N` 只被读一次时不开指令，使用时直接当立即数
- 比较连分支：`%v = cmp` 紧跟 `branch %v` 时直接发条件跳转，省掉 `setCC` + `movzbq` + 一次内存往返
- 非参数变量在序言清零（`x: int` 这种纯声明原先会读到栈上的垃圾）
- `tests/run/08_regalloc.py`：钉住 callee-saved 保护、多变量溢出、多参数、六种比较、嵌套循环

### 修复

- **局部区起点压住了 callee-saved 保存槽**：变量槽从 `-8(%rbp)` 开始，正好是被 `push` 下来的 `%rbx`；函数返回时把它弹回调用方，调用方的变量凭空变成被调函数的中间值。差分测试抓到的现象是「调完 `f0` 之后，`v3` 从 0 变成 -19」
- **融合目标与 LoadSlot 别名冲突**：`store.slot s, %v` 被融合吞掉时，如果 %v 走别名路径，变量寄存器根本不会被写，赋值静默丢失。`gen_load_slot` 现在在 `dst_loc_` 非空时强制真的写过去
- `cmp` 出现 mem-to-mem 操作数（x86 不允许），`cvtsi2sd` 拿到立即数（x86 不允许）
- push 奇数个 callee-saved 时 `call` 前 `rsp` 不是 16 对齐

### 性能

| 程序 | 0.4 | 0.5 | CPython 3.11 |
|---|---|---|---|
| `fib(35)` | 0.08s | 0.06s | 1.00s |
| 2 亿次 `while` 累加 | 0.34s | 0.06s | 7.00s |

`fib` 只快 1.4 倍——它几乎全是函数调用，热变量本来就只有 `n` 一个。整数循环快 5.7 倍，因为循环变量和累加器现在整轮都待在寄存器里。

### 验证

- `make test`：10 正例 + 27 反例 + 8 个运行用例
- 差分测试 10 个种子、2000 个程序，零差异
- ASan + UBSan + `-Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual` 零报告

## [0.4] - 2026-11-08

这一版的主线是**把 IR 到可执行文件这条路走通并验证**，同时补齐文档体系。

### 新增

- IR 层：基本块 + 三地址码 + 虚拟寄存器，类型挂在指令上（`src/ir.h`、`src/ir.cpp`）
- 降级器：`src/lower.cpp`，覆盖标量、字符串、列表、元组、函数、控制流、`print`、f-string
- 代码生成：`src/codegen.cpp`，System V AMD64 调用约定，栈帧 + vreg 槽位线性扫描复用
- 运行时：`runtime/rt.c`，字符串/列表/输出/panic，纯 C11
- CLI：`ir` / `asm` / `build` / `run` 四个模式
- `print` 的 `sep=` / `end=`（降级期展开，支持任意字符串表达式）
- 字符串大小比较（`<` `<=` `>` `>=`，按 UTF-8 字节序，与码点序一致）
- 标注的类型强制：`x: int = 1` 之后再 `x = "s"` 会报错
- 数值隐式转换：int → float 在赋值、传参、返回、容器字面量四处生效
- `tests/run/` 运行用例（7 个）、`tests/diff/` 差分测试（4 个生成器 + 统一 runner）
- `docs/` 文档体系（架构、语言、IR、后端、运行时、兼容性、开发、路线图）

### 修复

- 浮点 `%` 的符号：`-7.5 % 2` 得到 `-1.5`（`fmod` 语义），应为 `0.5`；零的符号也要取除数的
- 浮点转字符串：`10.0` 被排成 `1e+01`（`%g` 的切换阈值与 CPython 不同）
- `"a" < "b"` 生成空指令、恒为 `False`
- `x: float = 1` 把整数位模式当 double 读，打印出 `5e-324`
- 预扫描先占位的符号吃掉标注标志，导致标注在后续赋值中失效
- `Constant::boolean/none/ellipsis` 没有行列号，诊断指向 1:1
- 内置调用的关键字值被求值两次，同一个未定义名报两遍
- 复合赋值 `*=` 被降级成 `+=`（`Tok` 的两套记号没映射）
- f-string 里的表达式从未被类型检查
- vreg 槽位复用忽略控制流回边，循环第二轮读到垃圾（段错误）
- 整数除零与 `INT64_MIN / -1` 触发 `SIGFPE`，改为带消息的 panic
- `py23 run` 在源码旁边留下可执行文件，改为临时文件、跑完即删
- 字符串常量放 `.rodata` 触发 PIE 重定位警告，改 `.data.rel.ro`

### 移除

- 死指令 `IrOp::Print`（降级器从不产出，codegen 里是空 `case`）
- 无人调用的 `py23_pow_f64` / `py23_fmod_f64` 包装

## [0.3] - 阶段

- 增量构建：源码拆成 6 个翻译单元，`-MMD -MP` 依赖跟踪
- 前端大规模质检：标准库 + site-packages 38582 个文件作为基线
- 类型检查补强：分支合并、`is None` 收窄、`try` 体类型处理、容器元素扩宽

## [0.2] - 阶段

- 类型检查器：标注解析、推断、`unknown` 放行策略、内置函数与方法签名表
- 反例测试集（`tests/fail/`）

## [0.1] - 阶段

- 缩进敏感的词法分析器，覆盖 Python 3 全部关键字与运算符
- 递归下降解析器 + 完整 AST + 缩进式 dump
- PEP 701 f-string 解析（同引号嵌套、自文档化、spec 嵌套）
- 正例测试集（`tests/`）
