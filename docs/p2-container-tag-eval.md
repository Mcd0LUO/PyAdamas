# P2 容器级域 tag 评估（0.325，p2tag 分支）

> 范围：评估 docs/unknown-repr-design.md §5 的 P2「容器级 2-bit 域 tag /
> dict entry tag，仅供容器内批量操作 + 取出时装箱」能否在**约束 ①（静态
> 已知路径 asm 逐字节不变）**下落地，并显著压低 0.324 记录的 u1 +316% /
> u3 +373% 冷路径成本。基线 0.324，HEAD fdeea25。独立 worktree
> /tmp/wt-p2tag（分支 p2tag），不推 master、不动主树。
>
> **结论（先说）：P2 容器域 tag 单独落地，收益不抵复杂度，建议直接 P3
> 封闭 tagged union。** 两条硬证据：
> 1. u1/u3 的成本主体在**标量 unknown 侧**（循环变量/累加器取出后的探针
>    与装箱 churn），不在容器侧；容器 tag 管不到取出后的标量（设计文档
>    §4.3 早已指出「tag 属于容器，取出即丢」）。
> 2. 真正能降装箱频率的「裸 float 内联 + 2-bit 域 tag」必须改 kind2 下标
>    写入的**生成代码**（内联 1-bit 位图维护），而 26 个热基准里 **8 个**
>    含这条内联路径，改它就破坏「asm 逐字节不变」。

## 1. 成本剖析（perf，绑核 24，0.324 基线）

u1_hetero_list（30 万轮 append float+int + 遍历 isinstance/累加）：

| 符号 | 占比 | 来源 |
|---|---|---|
| rc_index_has.part.0 | **39.6%** | 堆探针（主索引 miss + 叶子位图两段） |
| pyadamas_fn_0__pa_main | 11.2% | 生成代码循环 |
| rc_alloc_core | 4.6% | float 盒分配 |
| pa_rc_inc_unknown | 4.3% | unknown 槽 retain（探针） |
| rc_index_put | 3.9% | 盒入索引 |
| pa_rc_cleanup | 3.8% | 临时栈清理 |
| pa_rc_dec_unknown | 3.7% | unknown 槽 release（探针） |
| pa_any_arith | 3.6% | 动态算术 |
| rc_index_del | 2.9% | 盒出索引 |
| pa_any_is_float | 2.3% | isinstance 探针 |

调用栈显示 rc_index_has 的 40% 里，pa_rc_inc_unknown / pa_rc_temp_unknown /
pa_rc_dec_unknown / any_decode 各占一块——**全是「标量 unknown 槽的
retain/release」和「动态算子解码」**，不是容器 append 本身。

u3_unknown_arith（40 万轮 acc = acc + xs[i%5]）：

| 符号 | 占比 |
|---|---|
| rc_index_has.part.0 | **24.6%** |
| pyadamas_fn_0__pa_main | 21.7% |
| rc_index_del | 6.3% |
| pa_rc_dec_slow | 5.9% |
| pa_any_arith | 5.8% |
| any_decode | 5.7% |
| rc_index_put | 4.4% |
| rc_alloc_core | 4.0% |
| rc_link | 3.1% |

u3 的 rc_index_del/put + rc_alloc_core + rc_link + pa_rc_dec_slow 合计
**~24% 是累加器 acc 每轮产生新 double 盒的分配/回收 churn**；rc_index_has
25% 是 acc 槽与元素的探针。xs[i%5] 元素侧只占一小部分。

**解读**：u1 的 v（for 循环变量，模块级全局 unknown）和 total（累加器全局
unknown）、u3 的 acc（累加器全局 unknown）都是**标量 unknown 槽**。每轮
retain/release 走探针、float 结果装箱。容器域 tag 即使完美落地，也只能省
「元素从容器取出那一瞬」的探针/装箱；取出后落进标量槽，tag 丢失（§4.3），
探针与装箱照旧。这正是设计文档 §7.3 把 u3 最坏形态归因于「累加器每轮产生
新 double」的原因——它是标量问题，不是容器问题。

## 2. 实现尝试与硬阻塞

### 2.1 已尝试：heap_bits 升级 2-bit 域 tag（裸 float 内联）

把 list 的 heap_bits 从 1-bit「是否堆」升级为 2-bit 域
（INT/FLOAT/HEAP/UNK），FLOAT 域裸 double 位模式内联存储（不装箱），
destroy/gc/set/del 按域释放，重排路径（insert/reverse/repeat/sort/del/
slice_assign）先装箱裸 float 再丢 tag 回退探针。运行时改完可编译，但
**tests/run/206_list_store_kind.py SIGSEGV**。

根因（gdb + 诊断定位）：**kind2 下标写入的生成代码内联维护 1-bit 位图**
（src/backend/index.cpp gen_index_store / gen_index_store_unchecked 的
`btsq/btrq` 内存位操作），与运行时 2-bit 域语义冲突——内联 bts/btr 按
1-bit 置位，把 2-bit 域字写花（实测 m2=[1,"x",3] 经 m2[1]=8 / m2[0]="z"
后域字从 2 变成 9，dom[1] 误判 HEAP，销毁把整数 8 当堆指针 dec → SIGSEGV）。

### 2.2 硬阻塞：8/26 热基准含内联 1-bit 位图写入

对 26 个热基准的基线 asm  grep `btsq/btrq`：

```
b1_log_parse:2  b4_wordfreq:1  d2_stats:1  d4_primes:4
d6_rec_memo:2   fannkuch:6     matmul:1     nqueens:6
```

**8 个基准**的 kind2 列表下标写入走内联位图维护。要落 2-bit 域 tag（或把
kind2 写入改走运行时以维护裸 float），必须改这条生成代码路径 → 这 8 个
基准 asm 变化（且是增加指令的回归）→ **违反约束 ①「26 基准 asm 逐字节
不变」**。0.324 的口径是 22 相同 / 4 更少（推断收益），不允许增加开销。

### 2.3 可行但复杂的替代：独立 dom 数组 + 编译器路由

保持 heap_bits 1-bit 与内联 bts/btr **完全不动**，另开独立 dom 数组
（懒分配）只做类型判别；编译器对「可能含 float 的列表」把下标写入路由到
运行时 dom-aware 入口（避免内联路径绕过 dom 维护造成陈旧 tag），读取暴露
dom 给动态算子。26 基准的 kind2 列表都不含 float → 不路由 → 内联路径不变
→ asm 逐字节不变。

这条路**理论上 asm 安全**，但需要：
- 编译器「列表可能含 float」数据流分析 + 写入/读取路由（改 lower_subscript /
  lower_for_seq / gen_index_store 选择 + 新 IR 入口）；
- 运行时独立 dom 维护 + 裸 float 释放口径 + 跨容器拷贝传播；
- 动态算子 dom 描述符 + 取出装箱兜底 + 批量 repr 去装箱。

复杂度落在「中高」（设计文档 §4.3 预估），且**即使全部落地，u1 的标量
循环变量/累加器成本仍在**（§1）——u1 收益有限，u3 仅省元素侧探针（~25%
里的一部分）。收益不抵复杂度。

### 2.4 旁证：探针重排实验（asm 安全，运行时-only）

把 rc_index_has 改成「叶子位图优先」（str/CPYFLOAT 盒是叶子，旧口径先白探
一次主索引 miss 再查位图）。绑核交替中位实测：

| 用例 | 基线 | 叶子优先 | 变化 |
|---|---|---|---|
| u1 | 75.8ms | 74.5ms | −1.7%（噪声内） |
| u3 | 63.2ms | 62.5ms | −1.1%（噪声内） |
| u2 | （dict 探针多） | 显著下降 | 叶子优先对 dict 密集有效 |

u1/u3 几乎不动——再次印证成本不在「探针顺序」，而在**探针/装箱的次数**
（标量槽每轮 retain/release + 结果装箱）。降次数需要标量侧带 tag（P3），
不是容器 tag 或探针微优化能解决的。实验后已回退，保持基线干净。

## 3. 为什么 P3 封闭 tagged union 是正解

- u1/u3 的成本主体是标量 unknown 的「探针 + 装箱次数」。P3 给**可枚举异构
  域**（int/float/str/…）的 unknown 值用 {tag, payload} 内联表示（设计文档
  §4.2/§5 P3），tag 跟值走：标量槽 retain/release 按 tag 跳过探针、float
  结果内联不装箱、动态算子按 tag 直接分派。这才同时消掉 §1 的两大成本。
- P3 必然改 unknown 的生成代码（槽变宽或平行 tag），**接受 unknown 路径
  asm 变化**，但静态已知路径仍零开销（约束 ① 的本意是保护静态热路径，
  不是冻结 unknown 冷路径）。26 基准经 P0 推断后几乎无 unknown _float_
  路径（实测 26 个 asm 均无 pa_any_* 调用），P3 对它们 asm 影响可控。
- 容器域 tag 可作为 P3 的子集自然导出（容器元素就是 tagged 值），不必
  单独做一套与内联位图冲突的旁路 tag。

## 4. 本分支交付与验证

- **未改运行时/编译器**：2.1 的 2-bit 尝试与 2.4 的探针重排均已回退，
  worktree 相对 fdeea25 干净（仅新增本评估文档 + CHANGELOG 0.325 预留）。
- 基线正确性复核：tests/run/206_list_store_kind.py 对拍 expect 一致；
  普通套件 ran ok / EXIT=0（见 CHANGELOG）。0.324 的异构 print/算术正确性
  （247_unknown_float_tag）保持不变。
- 建议集成：本条作为**评估记录**合入（与 stdlib 并行，CHANGELOG 编号按
  实际落地顺序重排）；P3 立项时引用本文 §1 剖析与 §2 阻塞作为设计输入。

## 5. 给 P3 的具体输入

1. 目标负载：u1（标量循环变量 + 累加器）、u3（累加器每轮新 double）。
2. 必须消掉的成本：pa_rc_inc_unknown/dec_unknown 探针（u1 ~40%）、float
   结果盒 rc_alloc/link/index churn（u3 ~24%）。
3. 表示：封闭域 {i8 tag, 8B payload} 内联；开放 unknown 仍回退 CPYFLOAT 盒。
4. 静态路径纪律：具体类型容器/槽不变宽、不 tag、asm 不变（26 基准复核）。
5. 容器元素域 tag 作为 P3 子集导出，避免与现有内联 1-bit heap_bits 冲突。
