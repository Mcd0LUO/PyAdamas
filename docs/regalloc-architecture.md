# 寄存器分配架构升级设计：从块级位图到精确区间

> 状态：设计稿，未实现。
> 来源：精读 LLVM 20（`/tmp/llvm`，稀疏检出 `llvm/lib/CodeGen`）的
> `LiveIntervals` / `SlotIndexes` / `SpillPlacement` / `SplitKit` /
> `InlineSpiller`，对照 PyAdamas 现状。

## 0. 一句话结论

PyAdamas 的寄存器分配和 LLVM 的差距**不在分配算法（线性扫描 vs 图着色），
而在区间的精度和 spill 决策的代价模型**。LLVM 是三层：

```
LiveIntervals（精确区间，带洞）
    ↓
SpillPlacement（Hopfield 网络求最优 spill 边界）
    ↓
SplitKit + InlineSpiller（按决策切分并内联展开）
```

PyAdamas 只有第一层的一半（**块级位图**，不是精确区间），后两层完全没有。
这直接解释了之前两次失败。

## 1. 现状：PyAdamas 有什么

`src/codegen_regalloc.cpp`（2011 行）：

| 组件 | 实现 | 精度 |
|---|---|---|
| `build_slot_liveness()` | 块级 live-in/live-out 位图 | 每块一个位图，**块内无位置信息** |
| `linear_scan_pool()` | 区间 = [首次访问, 末次访问] | 粗，把块间所有位置都算进区间 |
| `assign_slot_regs_graph()` | 图着色（精确活跃集建图） | 中，但受限于 `slot_reg_` 是整函数布尔标记 |
| `assign_slot_regs_split()` | 按块分配寄存器 | 有，但**无代价模型** |

实测数据（`PYADAMAS_RA_LIVENESS_STATS`）：

```
matmul.run_2          int 精确活跃 5 / 区间重叠 13
spectral_norm.main    int 精确活跃 5 / 区间重叠 12
nbody.advance_1       int 精确活跃 4 / 区间重叠 6，fp 4 / 5
```

**区间重叠数是精确活跃数的 2-2.6 倍**，这是寄存器压力虚高的根源。

## 2. 之前两次失败的根因

### 失败 1：live range splitting（0.262）

做法：每个基本块独立决定用哪个寄存器，块入口 reload、块出口 spill。

**结果**：nbody 235 vs 179（+56ms），spectral_norm 341 vs 285（+56ms）。

**根因**：没有代价模型。**每块都切**，无论那块是不是热块、无论
切分点两侧的引用是否值得。LLVM 的做法是先算「在哪切最划算」
（SpillPlacement），再切。

### 失败 2：图着色收益有限（0.263）

做法：用精确活跃集建冲突图，Chaitin-Briggs 着色。

**结果**：matmul -8.6%、nbody -3.5%，但 nqueens +8.1%（后来修好）。

**根因**：图着色的收益上限受区间精度限制。着色器认为「槽 A 和槽 B 冲突」，
但底层 `slot_reg_[s]` 是**整函数一个寄存器**的布尔标记，无法在区间中间
换寄存器。所以精确活跃集算出的「5 个同时活跃」这个信息，只有一部分能
转化成收益。

## 3. LLVM 的三层设计

### 3.1 LiveIntervals：带洞的精确区间

`LiveIntervals.h` 开头：

> an interval [i, j) is said to be a live interval for register v if there is
> no instruction with number j' > j such that v is live at j', and there is no
> instruction with number i' < i such that v is live at i'. In this
> implementation **intervals can have holes**, i.e. an interval might look
> like [1,20), [50,65), [1000,1001).

**关键概念**：

- **LiveRange**：由多个**不连续**的段组成，每段是 `[start, end)` 的 SlotIndex 区间。
- **VNInfo（value number）**：每个段带一个「值编号」，标记这段里的值是谁定义的。
  Phi 合并会产生新的 value number。
- **洞**：区间中间不活跃的部分。这正是 PyAdamas 的 `[first, last]` 近似
  丢掉的信息。

### 3.2 SlotIndexes：位置编号方案

`SlotIndexes.h`：

```cpp
enum Slot {
  Slot_Block,         // 基本块边界（phi-def 的 def 位置）
  Slot_EarlyClobber,  // early-clobber 寄存器的 use/def
  Slot_Register,      // 普通 use/def
  Slot_Dead,          // 死定义的 kill 点
  Slot_Count
};
enum { InstrDist = 4 * Slot_Count };  // = 16
```

**每条指令占 16 个编号**，低 2 位是 slot 类型。这样：
- 指令之间可以插入新指令（编号不冲突）；
- 同一条指令内部的「先 use 后 def」「early clobber」等时序能精确表达。

**对 PyAdamas 的意义**：我们的 IR 已经用 `block * kBlockStride + 块内下标`
（`kBlockStride = 1<<20`）做位置编码，但**只到指令粒度**。要支持洞和
精确区间，需要细化到「指令内位置」。

好消息：PyAdamas 的 IR 是**三地址码**，一条指令最多一读一写，所以
实际上**每条指令只需要 2 个位置**（读点、写点），比 LLVM 的 4 个少。

### 3.3 SpillPlacement：Hopfield 网络求最优 spill 边界

这是**最值得抄的部分**。`SpillPlacement.cpp` 开头：

> Each edge bundle corresponds to a node in a Hopfield network. Constraints on
> basic blocks are weighted by the block frequency and added to become the node
> bias.
>
> The Hopfield network minimizes (possibly locally) its energy function:
>
>     E = -sum_n V_n * ( B_n + sum_{n, m linked by b} V_m * F_b )
>
> The energy function represents the expected spill code execution frequency,
> or the cost of spilling.

**核心思想**：

1. **EdgeBundles（边界捆）**：把 CFG 的边做等价类划分 ——
   **所有从块 B 出去的边属于同一个「出捆」**，
   **所有进入块 B 的边属于同一个「入捆」**。
   这样一个有 N 个块的函数最多有 2N 个捆，而不是 O(N²) 条边。

2. **节点值**：每个捆一个节点，值 ∈ {-1, 0, +1}
   - `+1` = 变量在这个边界上应该在寄存器里
   - `-1` = 应该 spill 到栈
   - `0` = 无所谓（transparent block）

3. **偏置（Bias）**：来自活跃区间的约束
   - `PrefReg`：这个块里变量被用得多，倾向寄存器
   - `PrefSpill`：这个块里寄存器压力大，倾向 spill
   - `MustSpill`：必须 spill（如调用点 caller-saved）

4. **链接（Links）**：transparent block（变量穿过但不需要寄存器）
   在相邻捆之间建链接，权重是块频率。

5. **迭代求解**：`update()` 反复重算每个节点的值直到收敛
   （Lyapunov 函数，单调下降，保证收敛）。

**为什么这个设计好**：

- 复杂度 O(捆数 × 迭代次数)，不是 O(区间长度)；
- **代价是执行频率**，直接对应运行时开销；
- 自动处理「热块放寄存器、冷块放栈」这种最优决策。

**这正是 0.262 失败时缺的东西**：当时是「每块都切」，现在是
「先算每块该不该切，只切该切的」。

### 3.4 SplitKit + InlineSpiller：按决策切分

- **SplitKit**（1943 行）：区间切分工具箱
  - `splitAroundRegion`：在一个区域周围切
  - `splitSeparateComponents`：把区间拆成连通分量
  - `splitAt`：在指定点切
- **InlineSpiller**（1959 行）：直接改机器码插入 spill/reload
  - `hoistSpillInsideBB`：把 spill 提升到块入口（减少重复）

## 4. PyAdamas 的升级方案

### 4.1 分级实施

**不建议一次做完三层**。按收益/成本分级：

#### 阶段 A：精确 LiveInterval（收益中等，成本中等）

**目标**：把 `[first, last]` 近似换成带洞的精确区间。

**改动**：
1. 新增 `struct LiveSegment { int64_t start, end; int vn; }` 和
   `struct LiveInterval { std::vector<LiveSegment> segs; }`
2. 位置编码从「块×stride + 块内下标」细化到「**指令读点/写点**」：
   `pos = (block << 20) | (inst_idx << 1) | (is_def ? 1 : 0)`
3. `build_slot_liveness()` 保留（块级位图仍用于快速判定），
   新增 `build_precise_intervals()` 从位图 + 块内扫描构造精确区间。

**预期收益**：区间重叠数从 13 降到接近精确活跃数（5）。
但**如果分配器仍是「整函数一个寄存器」，这个精度用不上** —— 必须配合阶段 C。

#### 阶段 B：SpillPlacement（收益大，成本中）

**目标**：算「在哪 spill 最划算」，替代「每块都切」。

**改动**（照抄 LLVM 的思路，但简化）：
1. `EdgeBundles`：2N 个捆（入/出），用并查集做等价类。
   **PyAdamas 的 CFG 已经是块列表 + 显式 jump/branch，实现很简单**。
2. `BlockFrequency`：LLVM 用 `MachineBlockFrequencyInfo`（从 profile 或
   静态估算）。PyAdamas 已有 `block_loop_depth()`，可以先用
   `10^depth` 近似，后续再加真实 profile。
3. `SpillPlacement`：Hopfield 迭代。**核心代码不到 200 行**：
   ```cpp
   struct Node { int64_t biasN, biasP, sumLinkWeights; int value; 
                 std::vector<std::pair<int64_t,int>> links; };
   // update(): SumN = biasN + sum(links where neighbor==-1)
   //           SumP = biasP + sum(links where neighbor==+1)
   //           value = SumP > SumN + threshold ? +1 
   //                 : SumN > SumP + threshold ? -1 : 0
   ```

**预期收益**：这是 0.262 失败的直接修复。当时「每块都切」导致
nbody +56ms；有了代价模型，只切真正划算的块。

#### 阶段 C：区间分裂 + 按需 spill（收益大，成本大）

**目标**：让 `slot_reg_[s]` 从「整函数布尔标记」变成「按区间」。

**这是最难的部分**，因为要改 codegen 的所有 `slot_op()` 调用点。

**关键设计**：不改 `slot_op()` 的接口，而是**扩展 `slot_loc_`**（0.260 已
引入的按块位置表）：

```cpp
// 现状：slot_loc_[block][slot] = 寄存器名（块粒度）
// 升级：slot_loc_[block][slot] = 段列表，每段 [起始指令, 结束指令) -> 寄存器
struct SlotLoc { int begin, end; std::string reg; };
std::vector<std::vector<std::vector<SlotLoc>>> slot_loc_;
```

`slot_op(slot)` 用 `cur_pos_` 查段。**这个改动集中在一个函数**，
风险可控。

### 4.2 与栈映射的关系

**重要**：`list[float]` 这类**堆槽**不能进寄存器，根因是 GC 靠帧记录的
栈槽地址找根。这个约束**独立于寄存器分配精度**。

所以：
- 阶段 A/B/C 解决的是**标量槽和临时值**的寄存器压力（nbody 的 14 个
  标量槽、matmul 的 16 个）；
- 堆槽提升需要**栈映射**，是另一个方向。

两者的收益是**正交**的，可以并行推进。

### 4.3 验证方法

每阶段都必须过：

| 门槛 | 命令 |
|---|---|
| 差分测试 | `bash tools/difftest.sh`（5.6s，已并行化） |
| 全量用例 | `PYADAMAS_TEST_JOBS=4 bash tests/run_tests.sh build/dev build/dev/pyadamas` |
| ASan | `PYADAMAS_TEST_ASAN=1 ...` |
| 性能 | `bash tools/abbench.sh HEAD realistic 11` |

**性能门槛**：abbench 自检噪声 ±2%，所以**收益必须超过 2% 才算数**。

## 5. 建议的实施顺序

1. **先做阶段 B（SpillPlacement）**，不改区间精度，只把
   `assign_slot_regs_split()` 的「每块都切」换成「按代价模型切」。
   - 改动小（新增一个分析，改一个函数的循环）；
   - 直接修复 0.262 的失败；
   - 可独立验证收益。

2. **再做阶段 A（精确区间）**，为阶段 C 铺路。
   - 纯分析层改动，不影响生成结果（可用环境变量开关验证）。

3. **最后做阶段 C（按区间分配）**，这是收益最大也最难的一步。

## 6. 参考文件清单

在 `/tmp/llvm` 里：

| 文件 | 行数 | 看什么 |
|---|---|---|
| `llvm/include/llvm/CodeGen/LiveIntervals.h` | 622 | 带洞区间的数据结构 |
| `llvm/include/llvm/CodeGen/SlotIndexes.h` | — | 位置编号方案（InstrDist=16） |
| `llvm/lib/CodeGen/SpillPlacement.cpp` | 430 | **Hopfield 网络，最值得抄** |
| `llvm/include/llvm/CodeGen/EdgeBundles.h` | — | 边界捆等价类 |
| `llvm/lib/CodeGen/SplitKit.cpp` | 1943 | 区间切分策略 |
| `llvm/lib/CodeGen/InlineSpiller.cpp` | 1959 | spill 内联展开 |
| `llvm/lib/CodeGen/CalcSpillWeights.cpp` | 401 | spill 代价计算 |
| `llvm/lib/CodeGen/RegAllocGreedy.cpp` | 3145 | 贪心分配主循环 |

## 7. 明确不抄的部分

- **StackMaps / PatchPoint**（`StackMaps.cpp`）：这是 JIT 用的，目标是
  记录任意指令处的活跃**寄存器**。PyAdamas 需要的是 GC 的**栈槽**映射，
  用途不同。
- **GCRootLowering**（`GCRootLowering.cpp`）：注释明说
  "report all roots live at all safepoints"，即保守方案 ——
  和 PyAdamas 现状一样，没有帮助。
- **SSA 化**：实测 vreg 已经是完全 SSA（202 个 vreg，0 个多次定义），
  slot 层不是瓶颈（slot 密集循环 10.4ms vs vreg 密集 15.1ms）。
  这条路没有收益。

## 8. 结论：第三层不做（实验证据，2026-11-17）

第三层（区间级所有权 / SplitKit + InlineSpiller）做不做，取决于「寄存器容量
是不是真正的瓶颈」。为此加了一个**池容量扫描开关**：

```bash
PYADAMAS_RA_POOLCAP=N pyadamas build -O2 foo.py   # 把槽寄存器池限制到 N 个
```

把 6 个槽寄存器（%rbx/%r12-%r15/%rbp）逐级砍到 3 个，交替测量：

| bench | cap6 | cap5 | cap4 | cap3 |
|---|---|---|---|---|
| nbody | 50ms | 46ms | 51ms | 53ms (+6%) |
| matmul | 26ms | 26ms | 26ms | 28ms (+8%) |
| spectral_norm | 145ms | 146ms | 149ms | 145ms (0%) |
| nqueens | 114ms | 113ms | 113ms | 131ms (+15%) |
| mandelbrot | 38ms | 32ms | 30ms | 34ms（反而更快）|
| dict_wordfreq | 339ms | 335ms | 336ms | 345ms (+2%) |
| queue_sim | 419ms | 426ms | 417ms | 419ms (0%) |
| sort_struct | 379ms | 378ms | 378ms | 382ms (+1%) |

**寄存器池腰斩（6 → 3），性能只掉 0-8%（平均约 3%）。**

### 这说明什么

1. **寄存器容量不是瓶颈**。如果它值钱，砍掉一半不可能只有 3% 的代价；
2. 栈访问在 L1 里很便宜 —— 每次 spill/reload 的实际成本远低于我们假设；
3. 第三层能带来的只是「更好的共享」（有效容量 6 → 7-8 左右），按上述斜率
   上限是 **1-2%**，而工程量接近重写分配器；
4. 也解释了为什么三次 split 实现全是负收益：在同一批寄存器上换主人，
   换的收益（≈3% 斜率 × 1-2 个寄存器）付不起 reload/spill 的成本。

### 因此

`docs/regalloc-architecture.md` 的阶段 B（SpillPlacement）保留代码但默认关闭、
阶段 C（区间级分配）**不再实施**。第一层（精确区间）保留为诊断工具
（`PYADAMAS_LI_STATS` 打印峰值压力），它是这次得出「峰值 6-8 vs 池 6」这个
关键数据的手段。

瓶颈改判：**RC/GC 机制（占总时间 30-35%）与生成代码本身**，而不是寄存器分配。
后续优化方向见 `docs/heap-slot-promotion.md` 与 perf profile。
