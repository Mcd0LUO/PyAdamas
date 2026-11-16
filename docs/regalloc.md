# 全局寄存器分配 —— 设计与实测

状态：**R1–R4 已落地（0.203–0.204）**。正确性收益明确，**性能收益为零**。
下一步路线**待决策**——原归因（「顶层调用密度太高」）已被 0.233 后的实测推翻，
见[重新实测](#重新实测0233-后归因需要修正)与[两条候选路线](#两条候选路线待决策)。

## 落地前的现状与瓶颈（历史背景）

落地前后端**没有真正的寄存器分配**：变量（槽位）默认全部落在栈上，
只有两类局部优化把它们搬进寄存器：

1. **整函数常驻**（`assign_slot_regs`）：按访问频次挑前 N 个槽，整函数常驻在
   callee-saved 寄存器里。池是 **6 个**（`rbx` `r12`–`r15` `rbp`）。
2. **块内借用**（`assign_block_temp_regs`）：整块没有调用指令时，把 `r8`–`r11`
   借给**块内临时值**（vreg），块结束全部归还。不覆盖跨块变量。

浮点另有 8 个常驻池（`xmm8`–`xmm15`），但只在「无调用的整型/浮点函数」里用。

### 当时的实测天花板

`bench/compare/matmul.py`（**0.204 之前**的版本，三重循环在 `__pa_main` 里）：

| 指标 | 数值 |
|---|---|
| 栈访问（`load.slot`/`store.slot`） | 44 处 |
| 总 `(%rsp)` 访问 | **120 处** |
| 6 个 callee 寄存器 | 全部用满 |
| 性能 | 16 ms（Go 2.7 ms，**5.9×**） |

当时的判断：`matmul` 内层循环里**没有任何调用**，那 4 个 scratch 寄存器
（`r8`–`r11`）本可以借给变量用，但只借给块内临时值，跨块的循环变量拿不到。
同时活跃变量有 7 个（`i k j aik` + `a b c` 基址），6 个 callee 寄存器无论如何
都不够 —— **排序不是瓶颈，池大小和「能不能借 caller-saved」才是**。

> 这份数字描述的是 0.204 之前的 matmul。0.204 已把热循环包进 `run()`，
> 现在它的 `__pa_main` 只有 3 个块 / 17 个 `call.rt`。引用时必须带上这个前提。

### 为什么以前不能借 caller-saved

`r8`–`r11` 是 caller-saved：任何 `call` 都会破坏它们。所以变量要常驻在这些
寄存器里，必须在**每个调用点**做保存/恢复，或者只在「不跨调用」的区间里用。

后者正是块内借用做的事 —— 但它要求「整块无调用」，条件太严，覆盖不到
「循环体内有调用、但循环变量跨迭代活着」这种最常见的情形。

## 目标与范围

**目标**：给槽位（变量）做**基于活跃区间**的分配，允许跨块复用寄存器，
并在调用点按活跃性决定保存/恢复。

**不在范围**：
- 不做 SSA 形式的分配（IR 是三地址码 + 可变槽；0.205–0.209 用「稳定 ID +
  反向索引 + tag 操作数」拿到了 SSA 的大部分好处，见 [ir.md](ir.md)）
- 不做指令调度、不做强度削减

## R1–R4 落地结果

| 步 | 实现 | 版本 |
|---|---|---|
| R1 | `build_slot_liveness`：块级 live-in/out 位图，`PYADAMAS_RA_DEBUG=1` 可 dump | 0.203 |
| R2 | `assign_slot_regs_linear`：按区间起点线性扫描，溢出「终点最远」的区间 | 0.203 |
| R3 | caller 池：块内临时池缩到 `r8`/`r9`，`r10`/`r11` 只给不跨调用的槽，调用点 spill/reload | 0.203 |
| R4 | 抽出通用 `linear_scan_pool()`（整型/浮点共用），`slot_reg_cand_` 收两类候选、`slot_cand_is_fp_` 标记，GPR / xmm 两套池分别扫描 | 0.204 |

回退开关：`PYADAMAS_NO_RA=1` 走旧分配器。

**R3 与原计划的偏差**：原设计写「把 `r8`–`r11` 也纳入分配池」，实现时缩到
`r10`/`r11`。原因是 SysV 下 `r8`/`r9` 是**参数寄存器**、`r10`/`r11` 是
**闭包/环境指针**，全当 caller 池会让 matmul 算错（43198080 → 420883）。

### 途中修掉的真实 bug

- **区间计算用「首次/末次出现」**在 CFG 顺序 + 循环回边下是错的（3 处挂起 +
  4 处段错误），改为扩展到 live-in/out 覆盖的整块。
- **R4 自引入**：区间计算用全局候选下标而传入的是子集，导致 nbody 输出错；
  改为按 `cands` 建「全局 → 局部」映射。
- **0.211**：两个被分配策略变化顶出来的**既有** bug —— 堆槽的 `load.slot` 被
  LICM 外提（堆槽内容可被循环里的 `IndexStore` 间接改写）、`RcMark` 返回值被
  寄存器化（临时栈水位跨调用被冲掉 → `invalid temporary stack mark`）。
- **0.216**：unknown 槽被提升导致 **GC 根缺失**（帧记录登记的是栈槽地址，槽自
  序言清零后没再写过 → GC 读到 0 不标记 → 对象被回收 → 从已释放列表取元素）。
  三处修复：unknown 槽禁止提升、`gc_scan_slot_` 改在分配**之前**算、被 GC 扫描
  的槽禁止 `can_fuse` 融合。

### 回退的实验（有数据，不要盲目重试）

**块内 vreg 进 callee 池**（0.211）：nbody 差 306、binary_trees 输出 25（应 8191）。
根因是 `vreg_off()` 仍然指向那个栈槽，任何读栈槽的路径（spill/reload、RC 调用）
都拿到过期值。要安全地做，必须**先重写 RC 临时栈与 vreg 栈槽的耦合**，
那是另一个量级的改动。

### 一个误导性数字的教训（0.212）

`assign_slot_regs_linear()` 末尾原来统计 `slot_reg_` 里全部非空项，但**浮点槽是
在调用方随后才分配的**，此刻浮点项还是空的。于是长期输出
`advance_1: 7/14 slots promoted`，看起来「只提升了一半、还有大量空间」——
实际是整型 7/8、浮点 6/6，**已接近饱和**。

这个错误数字直接导致「寄存器分配还有很大优化空间」的误判，进而在 vreg 分配上
连续多轮投入失败。修正后输出 `[ra] advance_1: int 7/8 promoted, 6 regs`。

**判断剩余空间必须看整型/浮点分开统计的输出。** 同版还删掉了 R3 实验残留的
死代码（`spill_int_caller_slots` / `reload_int_caller_slots` 依赖的
`int_caller_slots_` 只被 clear 从未 push_back，永远空操作；
`build_int_caller_mask` 只有声明没有实现）。

## 重新实测（0.233 后）：归因需要修正

原结论写的是「`__pa_main` 有 106 个 call / 42 个块，活跃区间几乎都跨调用点，
caller 池条件命中不了」。**重新实测后这个归因不准确**：

| 基准 | blocks | call.rt | call.direct | load.global | store.global | load.slot | store.slot |
|---|---|---|---|---|---|---|---|
| spectral_norm | 40 | 43 | 0 | 39 | 16 | 34 | 24 |
| fasta | 10 | 34 | 1 | 19 | 9 | 2 | 4 |
| fannkuch | 5 | 30 | 1 | 13 | 6 | 2 | 4 |
| nbody | 8 | 26 | 2 | 49 | 7 | 4 | 4 |
| matmul | 3 | 17 | 1 | 8 | 5 | 1 | 2 |

复现命令（逐个换文件名即可）：

```bash
./build/dev/pyadamas ir bench/compare/spectral_norm.py | awk '
  /^func / { m = ($0 ~ /__pa_main/) }
  m && /^bb/ { b++ } m && /= call\.rt/ { rt++ } m && /= call\.direct/ { d++ }
  m && /load\.global/ { lg++ } m && /store\.global/ { sg++ }
  m && /load\.slot/ { ls++ } m && /store\.slot/ { ss++ }
  END { printf "blocks=%d call.rt=%d call.direct=%d load.global=%d store.global=%d load.slot=%d store.slot=%d\n", b, rt, d, lg, sg, ls, ss }'
```

两点结论：

1. **顶层的调用几乎全是 `call.rt`，用户调用 0–2 个。** `pa_*` 是 C 运行时，
   碰不到用户全局。所以「调用密度高 → caller 池命中不了」不是真障碍。
2. **真障碍是这些值不在分配器候选集里。** R1–R4 只处理**槽位（slot）**，
   而顶层变量是**全局（global）**。spectral_norm 顶层热循环
   （`bench/compare/spectral_norm.py:38`）的 `v_bv` / `v_vv` / `i` / `N`
   全是标量全局，一个寄存器都拿不到，每轮迭代都走内存。

仍有顶层热循环的是 spectral_norm / fannkuch / fasta；nbody 的顶层只是初始化，
热循环在 `advance()` 里；matmul / mandelbrot / fib / nqueens / binary_trees
的热循环都已在函数内。

0.204 手工把 matmul 包进 `run()` 就见效，侧面印证症结是「变量是不是局部槽」，
而不是「调用有多少」。

## 两条候选路线（待决策）

| 路线 | 做法 | 代价与风险 |
|---|---|---|
| **A. 标量全局提升** | 把候选集从 slot 扩到**标量** global，复用现有活跃区间 + 线性扫描 | 不改程序结构，语义 / traceback / 异常路径零风险。标量不是 GC 根，提升到 callee-saved 后不需要在 `call.rt` 前写回，绕开下面列的 GC / 展开耦合。**主要工作量是审计一份「哪些 `call.rt` 会回调用户代码」的白名单**（sort 比较器、生成器 resume、`__exit__`、C 回调跳板），这些必须保守写回。堆全局先不碰。收益上限仍受 6 个 callee 寄存器约束，且只覆盖顶层 |
| **B. 顶层热循环 outlining** | 把顶层循环包成合成函数，诱导变量变局部槽（自动化 0.204 对 matmul 的手工改造） | 更通用，覆盖未来任意顶层代码。但要处理 try 保护区切割、traceback 的 PC→函数表、GC 帧记录链与控制流边界，风险与工作量高一档 |

**倾向 A 先做**：风险确定低、直接打实测到的主要成本（spectral_norm 的 39 次
`load.global`），可带 `PYADAMAS_NO_GLOBAL_RA=1` 之类回退开关。B 留到 A 实测
证明 callee 池仍是瓶颈之后再评估。**尚未定案。**

## 与现有机制的耦合点（改动时必须逐条确认）

1. **帧记录与 GC**：帧记录登记的是**栈槽地址**，GC 按地址读值。变量提升到
   寄存器后栈槽里是旧值 —— GC 会读到过期指针。0.216 已用「unknown 槽禁止提升 +
   `gc_scan_slot_` 先算 + 禁融合」挡住这一类；**caller-saved 里的值在调用点会
   写回栈槽**，要保证写回时机早于可能触发 GC 的调用。路线 A 只做标量全局正是
   为了绕开这一条（标量不是根）。
2. **异常展开**：`pa_frame_unwind` / `pa_frame_unwind_to_cfa` 按帧记录释放堆
   引用，寄存器里的值不在记录里。现有代码用 `has_handlers` 降级；Z5 之后非可
   挂起函数不再有 setjmp，但**表驱动展开仍逐帧走 personality**，这个门禁需要
   重新审视。
3. **长跳转回滚**：setjmp/longjmp 会把 callee-saved 恢复到 setjmp 时刻。
   生成器/协程仍走 setjmp（设计上的永久降级），它们的 callee-saved 用法维持现状。
4. **`SlotAddr`（取地址）**：取过地址的槽不能再提升，现有 `addr_taken` 已挡掉。
5. **`RcMark` 水位**：`pa_rc_mark()` 的返回值必须独占栈槽、区间拉满整个函数
   （0.211 的 `vreg_is_rc_mark()`），任何让它进寄存器的改动都会 panic。
6. **热路径上的浮点寄存器假设很脆弱**：0.229 在 `pa_rc_inc`/`pa_rc_dec` 里加一个
   普通函数调用就把 `88_defaultdict` 的浮点值污染成 `-1.68e+260`。任何在热路径
   新增调用的改动都要重跑浮点用例。

## 度量方法与回归门槛

改动前后必须用同一套指标对比，不能只看「感觉快了」：

```bash
# 栈访问 / 全局访问数（编译期指标，最直接）
./build/release/pyadamas asm bench/compare/spectral_norm.py | grep -cE 'load\.slot|store\.slot'
./build/dev/pyadamas ir bench/compare/spectral_norm.py | grep -cE 'load\.global|store\.global'

# 真实性能（四语言基准）
python3 bench/compare/run_compare.py --repeat 5 matmul nbody spectral_norm
```

性能数字必须同机 A/B、`--rebuild`、交替多轮取中位数（本机跑间方差 ±10–20%）。

**回归门槛（0.233 起的完整口径）**：

- `tests/run_tests.sh` 全量 **205 ran ok**（188 运行 + 15 模块 + 1 cdecl + 1 cfi）
- `PYADAMAS_TEST_ASAN=1` 全量 205 ran ok
- `tools/difftest.sh` O0/O2 差分 188 用例 0 不一致
- 泄漏扫描 **0 处 / 188 用例**（0.233 刚清零，任何回退都要立刻发现）
- 四套异常夹具：z1_rt 116、z2_rt 134、z2_codegen 209、tests/eh 33；sem_baseline 42/42

## 已否决的方案

- **只做循环深度加权**（试过，已回退）：槽位 load/store 数 47 → 47，性能无变化。
  原因是 `matmul` 活跃变量 7 个而池只有 6 个，**排序无法解决数量不足**。
- **块内 vreg 进 callee 池**（0.211，已回退）：见上文，`vreg_off()` 与栈槽的
  耦合没重写之前做不安全。
- **全量 tag / NaN-boxing**：与本次无关，且已在 [roadmap.md](roadmap.md) 明确不做。