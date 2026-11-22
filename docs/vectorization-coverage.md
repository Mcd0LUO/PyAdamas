# 向量化覆盖率：0 触发基准的阻塞点清单

> 2026-11-23（0.418 归档；**0.423 更新**）。实测环境：默认管线（`PYADAMAS_VECT=1`、`PYADAMAS_SIMD` 关）。
> 当前 **4 个基准 / 11 条触发记录**（日志行数口径：matmul 4、spectral_norm 4、nbody 2、sieve 1；
> 按循环计 9 个），0 触发基准从 9 个降到 **8 个** —— sieve 的**计数循环**在 0.423 已向量化（`sieve` 指令数
> 100,223,185 → 95,431,674，−4.78%）。
>
> 本文最初（0.418）是那 9 个 0 触发基准的逐条诊断：热循环、**第一道拒绝的门**（判据位置 + IR 证据）、
> 以及为什么没有「便宜的通用缺口」可修。当时的结论是「9/9 全部止步于前置门」；
> **0.423 修正了其中一条**：sieve 计数循环的「还差 Cmp/bool」判断是错的，真凶是 **pass 顺序**
> （`if_convert_accumulators` 跑在 `promote_globals_in_loops` 之前），见 §3.2 与 §5 的修正。

相关文档：[vectorization-generalization.md](vectorization-generalization.md)（泛化路线与能力边界）、
[simd-vectorization-design.md](simd-vectorization-design.md)（特化模板历史）、
[vect-planner-design.md](vect-planner-design.md)（VectPlanner 结构）。

## 1. 触发现状（默认管线，`PYADAMAS_VECT_DBG=1` 统计；0.423 后）

| 基准 | 触发记录 | 说明 |
|---|---|---|
| matmul | 4 条（= 2 个循环 × 2 行日志） | `vectorize` + `avx2 4-lane` 各一行；整数 int-map + reduction |
| spectral_norm | 4 个循环（运行时 2） | `mul_ata`/`mul_a` 内层各 1，另 2 个在未被调用的死代码副本里 |
| nbody | 2 个循环 | `advance` 内层（bb10）+ `energy` 内层（bb6） |
| sieve | **1 个循环**（0.423） | 计数循环 `while i < N: if flags[i] == 1: count += 1; i += 1`（`bb14`，SSE2 2-lane）；初始化 append 循环与跨步清零循环仍 0 |
| **其余 8 个** | **0** | 本文诊断对象（0.418 时是 9 个，sieve 计数循环已解决） |

## 2. 0 触发基准的阻塞点总表（0.423 后：sieve 计数循环已解决，其余 8 个仍 0）

| 基准 | 热循环 | 第一道拒绝的门（判据位置） | IR 证据摘要 |
|---|---|---|---|
| mandelbrot | `while i < maxiter and zr*zr+zi*zi <= 4.0` | **循环形状**（`vect_planner_loop.cpp:77 if (!shaped)`，底层 `simd_while_shape_ok`，`codegen_simd.cpp:388`） | cond 的**真分支是 `bb12`（and 短路块）而不是体块 `bb10`**；退出由运行期比较决定 |
| sieve | ① 跨步清零 `while q < N: flags[q]=0; q += p` | ① 全局提升后**循环形状**过了，但 **loop_ops**（`codegen_simd.cpp:416`）仍拒：体内 `call.rt pa_rc_dec_unknown`（全局 q 类型 unknown 的 RC 清理）+ 步长 `p`≠1 | cond 首指令已是 `load.slot`（0.420 promote 生效）；体尾是 `LoadSlot/Add p/StoreSlot`，但 `simd_const_int(step, 1)` 失败、`pa_rc_dec_unknown` 不在白名单 |
| sieve | ② 计数 `while i < N: if flags[i]==1: count+=1; i+=1` | **✅ 0.423 已向量化**（触发 1 次，SSE2 2-lane） | 0.418 时第一道门是**体块终止符**（`vect_planner_loop.cpp:53`：体块以 `Branch` 结尾）；根因其实是 **pass 顺序**（if-conversion 在 promote 之前跑，那时 `count` 还是全局被保守门拒），详见 §3.2 末尾 |
| nqueens | `for c in range(n)`（`place`） | **体块终止符**（`vect_planner_loop.cpp:53`） | for.body 以 `branch` 结尾（`if cols[c]==0 and …` 的 and 链拆成 4~5 块） |
| collatz | `while n != 1`（`steps`） | **体块终止符**（`vect_planner_loop.cpp:53`） | 条件 `cmp.ne`（数据依赖，不是 `j < n`）；体块以 if/else 的 `branch` 结尾 |
| fannkuch | 翻转 / 排列更新 / 复制 | **循环形状** + 规模 | 所有循环 trip count ≤ n=8；翻转循环数据依赖（`while temp[0] != 0`）且**双归纳变量**（i 递增、m 递减） |
| fasta | 随机序列生成 | **体块终止符**（`vect_planner_loop.cpp:53`）+ 全局写 | 体块以 if 的 `branch` 结尾；module-level 循环用全局变量 |
| wordfreq | 逐词计数 | **体块终止符**（`vect_planner_loop.cpp:53`）+ loop_ops | 体块以 `branch` 结尾；体内 `call.rt pa_dict_contains` 等调用 |
| binary_trees | —（**0 个循环块**） | 无循环可向量化 | 纯递归建树/销毁 + 类分配 |
| fib | —（**0 个循环块**） | 无循环可向量化 | 纯递归 |

> 阶段编号来自 0.418 的临时诊断插桩（已回滚）：20 = 体块规模（4~128 条）、21 = 体块终止符不是 `Jump`、
> 31 = 循环形状（`simd_incr_cond_ok` / `simd_while_shape_ok`）、6 = `simd_loop_ops_ok`。
> **0.423 的插桩不同**：在每个 `return false` 前逐条编号并打印块名/块号/块指令摘要（见 §5.1），
> 因为「按 stage 编号猜门」不足以定位（上一轮就把终止符门误读成形状门）。复现方法见 §8。

## 3. 四个重点基准的详细诊断（IR 证据）

### 3.1 mandelbrot —— 数据依赖的提前退出

热循环是内层 `while i < maxiter and zr * zr + zi * zi <= 4.0`（`run_1` 的 `bb9`~`bb12`）：

```
bb9  (while.cond):
  %45 = load.slot s3            ; i
  %47 = cmp.lt %45, %46         ; i < maxiter
  branch %47, bb12, bb11        ; 真分支去 and.rhs（不是体块 bb10！）
bb12 (and.rhs):
  ... %55 = bin.add（zr*zr + zi*zi）
  %57 = cmp.le %55, %56
  branch %57, bb10, bb11        ; 运行期决定是否继续
bb10 (while.body):
  ... store.slot s3, %82        ; i += 1
  jump bb9
```

- **第一道门**：`run_1 bb10 stage=31 size=15` → `vect_planner_loop.cpp:77 if (!shaped)`，
  底层 `simd_while_shape_ok`（`codegen_simd.cpp:388`）要求 cond 的**真分支就是体块**
  （`C[2].target != body_bi` 即失败），这里真分支是 `bb12`。
- **为什么放宽分支目标也不够**：迭代次数由 `bb12` 的运行期比较决定（数据依赖提前退出），
  不是 `j < n` —— 现有「j+1<n 守卫 + 标量尾」模型算不出迭代数，需要逐 lane
  masked / early-exit 语义。
- 外层 `for y` / `for x`：`run_1 bb2/bb6 stage=31`，体块末尾是 `jump bb15`（LICM 前奏块）
  而不是 `for.incr`，且体内是嵌套 while → **嵌套循环**。

### 3.2 sieve —— 全局变量当归纳变量 + 体内 if（0.423：计数循环已向量化；跨步清零仍卡 loop_ops）

目标 1（跨步清零，~N log log N 次写）：`while q < N: flags[q] = 0; q = q + p`

```
bb10 (while.cond):
  %56 = load.global g4  ; unknown     ← q 是模块级全局，且类型是 unknown
  %57 = load.global g0  ; int
  %58 = cmp.lt %56, %57
  branch %58, bb11, bb12
bb11 (while.body):
  %61 = load.global g1  ; list[int]
  index.set %61[%62] = %60            ; flags[q] = 0
  %66 = bin.add %64, %65
  store.global g4, %66                ; q = q + p（走全局，不是槽）
  _ = call.rt pa_rc_dec_unknown(%67)
  jump bb10
```

- **0.418 时的第一道门**：`__pa_main bb11 stage=31 size=10` → `vect_planner_loop.cpp:77`。原因链（按门顺序）：
  1. `simd_while_shape_ok` 要求 cond 首指令是 `LoadSlot`（`codegen_simd.cpp:393`），
     0.418 时这里是 `LoadGlobal`；体块尾部要求 `LoadSlot / Add 1 / StoreSlot`，那时全是 global；
  2. 步长必须是 1（`simd_const_int(B[k+1].b, 1)`，`codegen_simd.cpp:404`），这里是 `p`；
  3. 即使形状过，`simd_loop_ops_ok`（`codegen_simd.cpp:416`）拒绝 `store.global` 与
     `pa_rc_dec_unknown`；
  4. 全局 q 类型 unknown → int Bin 分类要求两操作数都是 int，也会在角色阶段失败。

- **0.420/0.423 之后的现状**：全局提升把 `q`/`flags` 变成槽后，cond 首指令已是 `load.slot`、
  体尾是 `LoadSlot / Add p / StoreSlot`；此时 **loop_ops** 成为第一道门（`pa_rc_dec_unknown` 调用 +
  `store.global` 已随提升消失，但步长 `p`≠1 仍不满足 `simd_const_int(step, 1)`）。即该循环的剩余
  阻塞是「步长非 1」+「体内 RC 调用」，不是形状。

目标 2（计数，N=500k）：`while i < N: if flags[i] == 1: count += 1; i += 1`

```
bb13 (while.cond):  load.global g2 ; cmp.lt ; branch bb14, bb15
bb14 (while.body):
  %83 = load.global g2  ; int
  %84 = index.get %82[%83]
  %86 = cmp.eq %84, %85
  branch %86, bb17, bb16            ← 体块以 Branch 结尾
```

- **0.418 时的判断**：第一道门 = 体块终止符（`vect_planner_loop.cpp:53 if (L.BI.back().op != IrOp::Jump)`），
  体块以 `branch` 结尾（体内 if 把体块拆成多块）→ 需要 if-conversion。
- **0.423 修正（本文唯一一条被推翻的门序判断）**：0.420 的全局提升修好后 `count`/`i` 已是槽，
  if-conversion 本可压平这个菱形，但它跑在 `promote_globals_in_loops` **之前**（`optimize_ir` 的调度顺序），
  那时 `count` 还是全局，被「槽累加器 ±1」保守门拒绝；之后没有第二遍 if-conversion。补第二遍（纯调度、
  放宽任何判据）后体块单块化。钉死证据（临时 stage 插桩 + 块 dump）：
  ```
  [shapedbg] body=14 name=while.body n=4
        %83 = load.slot s1
        %84 = index.get %82[%83]
        %86 = cmp.eq %84, %85
        branch %86, bb17, bb16        ← 终止符是 Branch（菱形没压平）
  ```
  即先被**终止符门**拒（不是 while 形状门、更不是 Cmp 分类门）。单块化之后还差两道：
  `Cmp(Eq/Ne) → VecI(0/1 lane)` 与 `bool 作为 int 0/1 参与算术/归约`（0.423 一并补上；含 Cmp 的循环
  强制 SSE2 2-lane，因为 AVX2 发射器没有 Cmp 序列）。结果：触发 **1 次**，
  sieve 指令数 **100,223,185 → 95,431,674（−4.78%）**。
- 初始化 append 循环（N=500k）：`bb2` 仍被拒；体内 `call.rt pa_list_append` 会被 loop_ops 拒
  （调用就是 work 本身）。跨步清零循环见上：全局提升后形状能过，但 `store.global`→槽后仍卡
  「步长必须 1」（`p`）与 `pa_rc_dec_unknown` 调用。

### 3.3 nqueens —— 体内 if 链 + 递归调用

热循环：`place` 的 `for c in range(n)`

```
bb3 (for.cond):  ... branch bb4, bb6
bb4 (for.body):
  %26 = index.get %24[%25]      ; cols[c]
  %28 = cmp.eq %26, %27
  branch %28, bb8, bb7          ← 体块以 Branch 结尾
bb8 (and.rhs) → bb9 (and.rhs) → bb10 (if.then):
  index.set ×3                  ; cols[c]=1, d1[...]=1, d2[...]=1
  %76 = call.direct f1(...)     ← 递归调用 place(row+1, …)
bb7 (if.end) → jump bb5 (for.incr)
```

- **第一道门**：`place_1 bb4 stage=21 size=7` → `vect_planner_loop.cpp:53`：
  体块以 Branch 结尾（`if cols[c]==0 and d1[...]==0 and d2[...]==0` 的 and 链拆成多块）。
- 即使 if 单块化：体内 `call.direct f1`（递归）+ 6 个 `index.set` → `simd_loop_ops_ok`
  拒绝调用；递归本身不可向量化。
- `count_2` 的初始化循环：`bb6 stage=6`（loop_ops：`pa_list_append`）。

### 3.4 collatz —— 数据依赖条件 + if/else 多块

热循环：`steps` 的 `while n != 1`

```
bb1 (while.cond):
  %5 = load.slot s0 ; int
  %7 = cmp.ne %5, %6        ; n != 1（数据依赖，不是 j < n）
  branch %7, bb2, bb3
bb2 (while.body):
  %11 = bin.mod %9, %10 ; %13 = cmp.eq %11, %12
  branch %13, bb5, bb6      ← 体块以 Branch 结尾（if/else）
bb5 (if.then): ... store.slot s0, %17 ; jump bb4
bb6 (if.else): ... store.slot s0, %23 ; jump bb4
bb4 (if.end):  %27 = bin.add（计数 c）; store.slot s1 ; jump bb1
```

- **第一道门**：`steps_1 bb2 stage=21 size=4` → `vect_planner_loop.cpp:53`（体块以 Branch 结尾）。
- 双重根本原因：条件 `n != 1` 是数据依赖（trip count 不可知，计数循环模型不适用）；
  体内 if/else 把体块拆成多块。
- 外层 `__pa_main` 的 `while i < N`：`stage=31`，体内 `call steps_1` + if。

## 4. 其余 5 个基准（一句话结论 + 证据）

| 基准 | 结论 |
|---|---|
| **fannkuch** | 所有循环 trip count ≤ n=8（初始化 8 次、复制 8 次、翻转 m≤8、排列更新 i≤8），2-lane 的守卫+标量尾固定开销大于收益；翻转循环还是数据依赖（`while temp[0] != 0`）且**双归纳变量**（i 递增、m 递减），形状门必拒（`bb2 (while.body)` 体含 `pa_list_append` 等调用）。**不建议做**。 |
| **fasta** | 热循环体是 RNG（`bin.mul/add/mod`）+ 字符串 append（`convert … str`、`pa_rc_*` 调用），module-level 循环用全局变量；体块以 Branch 结尾 → 终止符门。 |
| **wordfreq** | 热循环体是 dict 查找/更新与字符串拼接（`call.rt pa_dict_contains`、`pa_rc_*`），module-level 全局变量；体块以 Branch 结尾。 |
| **binary_trees** | 整个程序 **0 个循环块**（纯递归建树/销毁 + 类分配）——永远不会有触发。 |
| **fib** | **0 个循环块**（纯递归）——永远不会有触发。 |

## 5. 为什么没有「便宜的通用缺口」

诊断的关键结论（**0.418 口径**）：**9/9 的拒绝都发生在前置门**（体块规模 / 终止符 / 循环形状 /
loop_ops，即 §2 表里的 stage 20/21/31/6）。角色分类（`classify`）、别名方案（`vect_alias_plan`）、
寄存器分配（`vect_alloc_temps`）这些中后端判据**一次都没走到**。
（0.423 后更新：sieve 计数循环已解决，9/9 → **8/9**；且它的门序判断被修正，见 §5.1。）

因此「int 乘法/比较链分类缺失」「某个 role 没认」这类便宜缺口在本轮 9 个基准里**不存在**：
不是分类器不认某种操作，而是循环本身还没进到分类器。每个阻塞点都对应一项新能力，见 §6。

### 5.1 0.423 的重要修正：sieve 计数循环「还差 Cmp/bool」的判断是错的

0.420 全局提升修好后，我曾判断 sieve 计数循环只差两件事：向量化器 `classify` 没有 `Cmp` 分支、
`bool` 操作数不满足 int 分类。**这个判断错了。** 0.423 用 10 分钟插桩（`simd_incr_cond_ok` /
`simd_while_shape_ok` 每个 `return false` 前逐条编号 + 进入时 dump 两个块的指令）看到：
计数循环体在进入形状检查**之前**就被**终止符门**拒了 —— 体块仍以 `Branch` 结尾（菱形没压平）。
真凶是 **pass 顺序**：`if_convert_accumulators` 跑在 `promote_globals_in_loops` 之前，那时 `count`
还是全局、被「槽累加器 ±1」判据拒绝；补第二遍 if-conversion（纯调度）后形状门直接通过。

`Cmp`/`bool` 是**第二道门**（形状通过后才轮到角色分类），并不是第一道。

**教训（写进 [optimization-roadmap.md](optimization-roadmap.md) 的「pass 顺序依赖」一节）**：

1. **先用插桩钉死门序，再决定做什么** —— 按文档/直觉猜「还差哪项能力」会浪费时间甚至做错方向；
2. 插桩粒度要到「哪个函数、哪个块、哪一条 `return false`，以及该块的指令摘要」，
   stage 编号本身不够（上一轮只看 stage 编号把终止符门误读成形状门）；
3. pass 之间的隐式顺序假设要有守护（统计或回归），不能靠「两个 pass 各自正确」推整体正确。

## 6. 阻塞模式 → 需要的能力 → 量级

| # | 阻塞模式 | 涉及基准 | 需要的能力 | 量级 |
|---|---|---|---|---|
| 1 | **数据依赖的提前退出**（非计数循环） | mandelbrot 内层、collatz、fannkuch 翻转 | masked / early-exit 向量化（每 lane 独立退出 + 掩码存储，语义最难） | 大 |
| 2 | **模块级全局变量当归纳变量/数组** | sieve（**0.420 已解决**：计数/清零循环的全局已提升为槽）、fasta、wordfreq | 全局量的 j-alias、全局写分析、全局 int 类型推断（当前只认槽） | 中-大（sieve 已完成） |
| 3 | **循环体内 if**（多块） | sieve 计数（**0.423 已解决**）、nqueens、collatz、fasta、wordfreq | if-conversion / 多块循环体 | 大（最小版菱形已在 sieve 生效） |
| 4 | **嵌套循环** | mandelbrot 外层 | 外层向量化 / loop interchange | 大 |
| 5 | **体内调用**（append/dict/RNG/递归） | sieve、nqueens、fasta、wordfreq、fannkuch | 调用屏障 / 部分向量化 | 大（多数调用就是 work 本身） |
| 6 | **trip count 太小**（≤8） | fannkuch | —（不划算） | — |
| 7 | **0 个循环**（纯递归） | binary_trees、fib | —（永远不会有触发） | — |

## 7. 结论与优先级建议

**结论**：剩下 8 个基准不是「判据太保守」，而是**每一条都缺一项结构性能力**。
按性价比排序，如果后续还要继续：

1. **全局变量当归纳变量 + 数组**（优先级 1，**0.420 已落地**）：sieve 的计数/清零循环已把全局
   提升为槽。剩余口径变为：清零循环还差 step≠1 与 `pa_rc_dec_unknown` 调用放行；fasta/wordfreq
   还差调用屏障。
2. **if-conversion / 多块循环体**（优先级 2，**0.423 最小版已落地**）：sieve 计数循环受益；
   剩余 nqueens 检查链、collatz if/else、fasta、wordfreq 的多块体仍超出「then 块只含累加」
   的最小版判据，需要更一般的菱形/多块转换（与 masked 语义有部分重叠）。
3. **masked / early-exit**（优先级 3，最大）：mandelbrot 内层、collatz；
   做完收益最大但正确性最贵。
4. **嵌套循环**（优先级 4，最大）：mandelbrot 外层；可考虑 loop interchange，
   让内层先被单独向量化（但内层正是数据依赖的 while，收益仍被 3 挡住）。
5. **fannkuch 不建议做**（trip count ≤8，2-lane 固定开销就吃掉收益）。
6. **binary_trees / fib 永远不会有触发**（0 个循环块，纯递归）——别再花时间。

## 8. 复现方法

```bash
# 看哪些循环触发了通用向量化（只打印成功行）
PYADAMAS_VECT_DBG=1 build/lead/pyadamas build --rebuild -o /tmp/x bench/compare/<c>.py 2>&1 | grep "\[vect\]"
# 看热循环 IR
build/lead/pyadamas ir -O2 bench/compare/<c>.py
# 看门：临时在 try_loop 里加阶段打印（本轮诊断用的临时插桩，已回滚）
```

诊断插桩的做法（供下次复用）：`try_loop` 包一层 `try_loop_inner`，在每个门之前设
`static int g_stage`，失败时在 `PYADAMAS_VECT_DBG` 下打印
`[vect] reject <函数> bb<块号> stage=<门> size=<体块规模>`，再按 IR 块号映射到热循环。

## 9. 与其它文档的分工

- 能力边界（已支持/不支持）与收益表：见 [vectorization-generalization.md](vectorization-generalization.md) §2；
- 已有覆盖的 **4 个基准 / 11 条触发记录**（matmul、spectral_norm、nbody、sieve）怎么做到的：
  同文档 §2、§4 P0–P5；
- 特化模板与通用的关系：同文档 §4 P4、[simd-vectorization-design.md](simd-vectorization-design.md)；
- 通用浮点路径的长远方向（nbody 为什么还慢 2×）：见 [optimization-roadmap.md](optimization-roadmap.md)。
