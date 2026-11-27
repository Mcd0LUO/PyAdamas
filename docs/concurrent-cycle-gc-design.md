# B 路线设计：保留 RC 的并发循环 GC（借鉴 Go 并发标记技术）

> 状态：设计稿（lead 亲自产出，研究阶段，未动源码）。决策依据：用户选定 B 路线——
> **保留引用计数 RC + 确定性析构，只把循环 GC 并发化**，借鉴 Go 的并发标记/写屏障/pacer，
> 而不是 A 路线（去 RC 改纯 tracing，推翻 P3 与 Python 析构语义）。
> 源码引用：pyadamas `runtime/rt_core.c`（2880 行，RC+GC 全在此）；Go 1.26.10 `/usr/local/go/src/runtime/`。

## 0. 一个关键事实：Go 式机器已经造了大半，且曾试并发标记后退守

读 `rt_core.c` 发现，pyadamas **早已照抄 Go 的并发标记结构**（注释明写「照抄 Go」）：

| 机制 | 现状 | 位置 |
|---|---|---|
| 增量标记状态机 | `gc_incr_state` 0空闲/1标记/2清扫，`gc_incr_step` 按预算推进 | rt_core.c:106-111, 2334 |
| 标记预算/清扫预算/stride | `GC_MARK_BUDGET=64` / `GC_SWEEP_BUDGET=512` / `GC_INCR_STRIDE=16`（注释对标 Go gcDrain） | rt_core.c:84-104 |
| **Dijkstra 插入写屏障** | `gc_write_barrier` 写入即标灰新值，`pa_gc_write_barrier` 已导出 | rt_core.c:2324-2332 |
| allocate-black | `gc_alloc_black` 是**空操作占位**（新对象不在 gc_set 快照，天然不被清扫，靠 RC） | rt_core.c:2309-2322 |
| 标记期延迟释放 | `GC_PHASE_INCR_MARK` 期 RC dec 登记 `gc_pend`，收尾统一处理 | rt_core.c:65-68, 174-179, 1858-1890 |
| **Go pacer 阈值公式** | `Target = Live + (Live+roots)*GOGC/100`，GOGC=100，下限 2MB；major 零回收退避 | rt_core.c:2676-2728 |
| 精确根扫描 | 帧链 `gc_scan_frames` + 寄存器 `gc_scan_registers` + 挂起 vthread `pa_vt_gc_scan` | rt_core.c:2499-2509 |

**但是**：major 路径目前**标记同步做完**（`gc_drain` 一次标完，实测 ~1.5ms），**只把清扫分片**。
原因写在 rt_core.c:2511-2523——作者**试过把标记也分片（Go gcDrain 那样），撞了 RC 硬冲突后退守**：

> 「标记期间对象可能被 RC 释放，标记队列里还留着指向它的指针——即使登记 gc_pend 延后释放，
> 仍有对象在收尾之后才归零（实测 **174_gc_construct_roots 漏 21KB**）。根因是 RC 的释放时机不受 GC 控制，
> 而 Go 根本没有 RC。要真正实现并发标记，必须先让所有释放都经过 GC 的『灰对象』协议。」

UAF  likewise：13_gc 在标记期 RC 立即释放 → `gc_scan_object` 撞已释放内存（rt_core.c:2565-2570）。

**结论：B 路线不是从零造，而是补上作者退守的那两块——(1) 让 RC 释放与并发标记相容的删除屏障；(2) 把标记/清扫抬到后台线程。**
插入屏障/pacer/增量骨架/gc_pend 都已就位。这大幅降低工程量与风险。

## 1. 核心难题：RC 独立回收 vs 三色标记不变量

三色标记要求：标记期间 mutator 不能把一个白色对象「藏起来」躲过扫描。Go 用混合写屏障维持。
pyadamas 的 mutator 是 **RC**——它按自己的时机 inc/dec/free，**不受标记器控制**。两类破坏：

- **插入式隐藏**（已有屏障覆盖）：黑色对象 a 已扫描，之后 `a.next = b` 写入白色 b → b 漏标。
  `gc_write_barrier` 标灰新值 b 解决。
- **删除式隐藏 + 提前释放**（**缺屏障，正是退守根因**）：RC dec 把某对象唯一引用删掉并 free，
  而标记队列/快照还指向它 → UAF；或对象在标记收尾后才归零 → 漏释放（174 的 21KB）。
  插入屏障管不到「删除」，因为 RC 的 dec 不是「写指针槽」，是「减计数到零释放」。

## 2. Go 的解法：混合写屏障 = Yuasa 删除 + Dijkstra 插入

Go `runtime/mbarrier.go:24-64` 的混合屏障伪代码：

```
writePointer(slot, ptr):
    shade(*slot)          // Yuasa 删除屏障：标灰「被覆盖的旧值」
    if stack is grey:
        shade(ptr)        // Dijkstra 插入屏障：标灰「写入的新值」
    *slot = ptr
```

- **删除屏障 shade(*slot)** 防止「把唯一指针从堆里unlink掉」藏对象——**这正对应 RC 的 dec/释放**。
- 内存序（mbarrier.go:68-93）：Go **无条件标灰 ptr**（不依赖槽颜色），避免 store/load 重排需要昂贵内存屏障——后台线程标记器必须照此办理。
- 栈写免屏障（当前帧），逃逸指针写要屏障；全局写要屏障（或终止时重扫全局）。
- 标记终止（mark termination）短 STW：排空各 P 屏障缓冲 + 重扫栈/全局，保证收敛。

**pyadamas 缺的就是 Yuasa 删除那一半**：在 RC 释放路径上，标记期对「即将归零的对象」标灰保活，而不是立即 free。

## 3. B 路线具体设计

### 3.1 删除屏障（shade-on-delete）——先补这块，单独可验证

钩子已在：`pa_rc_dec` 快路径见 `pa_gc_phase_fast!=0` 即转慢路径（rt_core.c:2875）→ `gc_phase_dec`。
改 `GC_PHASE_INCR_MARK` 分支（rt_core.c:174-179）：

- 现状：dec 到零的对象登记 `gc_pend` 延后释放。
- 改为：**标记期任何归零对象先标灰入标记队列（shade），并保活到本轮标记排空 + 终止检查之后**，再统一释放。
  即「所有释放经过 GC 灰对象协议」——作者点名需要的那套。对象在标记器扫到它之前不被 free（灭 UAF）；
  快照内可达但被 RC 删引用的对象靠删除屏障保活到被扫（灭 174 漏释放）。
- 配合已有插入屏障 → 完整混合屏障，三色不变量在 RC mutator 下成立。
- **关键：这一步不需要后台线程**。先在 M=1 上重新启用增量标记（`gc_incr_state=1` 分片标记），
  用删除屏障修好 174/13，验证正确性。这是最低风险的第一里程碑，直接回应作者的退守点。

### 3.2 后台标记线程（真并发，第二阶段）

把 `gc_incr_step` 的标记/清扫工作从「分配点驱动」抬到**一个专用后台 OS 线程**，mutator 继续跑 → 标记与运行真重叠，消除 ~1.5ms 同步标记停顿。

- **M=1 用户代码不变**：vthread 仍协作式单 OS 线程；只 **+1 个 GC 后台线程**，不是全 M:N。
  用户代码 RC 快路径在**非标记期仍非原子**（零原子红利保住）；只有标记期的屏障接口 + 标记器读需要原子/屏障。
- 「119 个 static 线程安全大山」** scoped 到 GC 子系统**：标记器与 mutator 共享的只是 gc_work/gc_pend/gc_set/池/freelist，不是全部运行时。

### 3.3 线程安全边界

- **标记器读指针用 atomic load**（mutator 并发写字段）；照 Go 无条件标灰避免重排（mbarrier.go:88-93）。
- **写屏障缓冲线程安全**：仿 Go `mwbbuf.go` 每线程缓冲 + 满则 flush 到加锁全局队列（pyadamas 单 mutator 线程，简化为 mutator 缓冲 + GC 线程消费，一把锁）。
- **gc_pend / 池 / freelist**：标记器清扫 free 与 mutator 分配并发 → 池 put/get 加锁或每线程缓存。这是 scoped 后的主要同步点。
- `gc_phase`/`gc_incr_state` 用 atomic（标记器与 mutator 都读）。

### 3.4 根快照 + 标记终止

- **标记启动短 STW**：`gc_scan_frames`/`gc_scan_registers`/`pa_vt_gc_scan` 一次性拍一致根快照（根必须一次扫完，rt_core.c:2497 注释）。vthread 挂起栈帧链此时冻结。
- **标记终止短 STW**：排空屏障缓冲 + 重扫根（Go mark termination），确认 gc_work 空 → 转清扫。
- 两次短 STW 之间标记并发跑。停顿从「整个标记 1.5ms」降到「两次根扫描亚毫秒」。

### 3.5 并发清扫 + assist

- 清扫已增量（预算 512），抬到后台线程持续做（Go 后台 sweep goroutine 同款）；池线程安全见 3.3。
- **mutator assist**：分配速率超过标记器时，分配方辅助标记（Go gcAssistAlloc）——pacer 已在，补 assist 配额即可。

## 4. 代价与张力（诚实）

- **写屏障热路径开销**：标记期每次指针存储多一次判断+可能入队；RC dec 多删除屏障。由 `gc_phase` 门控（非标记期零开销，与现状一致），但标记期 mutator 变慢。**净收益 = 停顿减少 > 屏障开销**，延迟敏感负载划算，纯吞吐负载未必。必须用 26 热基准量化。
- **零原子部分让步**：标记期屏障/标记器读要原子；非标记期用户 RC 快路径仍非原子。不是全 M:N 那种全局原子化。
- **析构语义微变**：标记期归零对象的 `__del__`/dtor 被**推迟到标记终止后**（删除屏障保活）。非标记期仍确定性析构。这是可观测变化——CPython 的循环 GC 期析构 likewise 延迟，需对拍确认口径可接受。
- **P3 交互**：P3 unknown 的 RC 探针/tag 依赖 RC 头；删除屏障改的是释放时机不改头布局，预期相容，但 262-266 + u1/u3 必须回归。
- **屏障正确性极微妙**：作者已漏过一次（174）。删除屏障+并发标记的组合证明要严谨，ASan+leak 闸门不可省。

## 5. 分阶段路径 + 闸门

| 阶段 | 内容 | 闸门 |
|---|---|---|
| 0 研究 | 本文档 + go-gc-study 源码研究 | 已完成 |
| 1 删除屏障 + M=1 增量标记 | 补 Yuasa 删除屏障，重启用分片标记（**不开后台线程**），修 174/13 | **13_gc / 174_gc_construct_roots**（作者退守点）+ difftest + ASan detect_leaks=1 + CPython 对拍 + 26 热基准（屏障开销） |
| 2 后台标记线程 | 标记抬到专用 OS 线程 + 线程安全边界 + 两次短 STW | 同上 + 并发压测（标记期 churn）+ 停顿测量（GC_TIMING）+ 数据竞争检测（TSan 若可用） |
| 3 并发清扫 + assist | 清扫后台化 + 池线程安全 + mutator assist | 同上 + 吞吐/尾延迟对比（同步标记基线） |

每阶段独立分支、独立 CHANGELOG 版本、26 热基准 asm/性能闸门、可回退（同步标记基线保留为 fallback）。

## 6. 风险与回退

- 若阶段 1 删除屏障仍漏/崩（RC 释放路径有未覆盖分支）→ 退回同步标记，重新审计释放路径全覆盖。
- 若阶段 2/3 屏障开销 > 停顿收益（热基准回退）→ 只保留阶段 1（M=1 增量标记已能摊薄停顿），不上后台线程。
- 同步标记 + 增量清扫（现状）始终是安全 fallback，B 是渐进增强不是推翻。

## 7. 开放问题（待用户拍板）

1. 标记期 `__del__` 延迟析构是否可接受？（CPython 循环 GC 同款延迟，但需确认对拍口径）
2. 目标停在阶段 1（M=1 增量标记，低成本低风险）还是要到阶段 2/3（后台线程真并发）？
3. 性能目标：优先降停顿（尾延迟）还是保吞吐？决定屏障开销的可接受阈值。
4. 是否立项为 sustained goal（多轮自动推进），还是逐阶段人工驱动？

## 8. 与 go-gc-study 研究对照（修订与补强）

代理研究文档 [go-concurrent-gc-study.md](go-concurrent-gc-study.md)（654 行，Go 1.26.10 源码逐行取证）
独立核实并补强了本设计。lead 已复核其关键取证（grep 全仓库确认）。一致结论：B 路线成立，
但以下几处须修订/补强（以本节为准）：

### 8.1 修订：屏障脚手架是「完全 dormant」，不只是被 major 绕过

§0 原说「插入屏障已有、major 绕过增量标记」——**不够准确**。实测取证：
`gc_write_barrier`/`pa_gc_write_barrier` 全仓库**零调用点**（仅 rt_core.c:2324 定义 + 2332 导出包装）；
`gc_incr_state = 1` **无任何赋值路径**（增量标记态从未进入）；`gc_alloc_black` 是空操作占位。
即增量标记 + 写屏障是**完全没接线的死脚手架**，codegen 根本不发射屏障。
**所以阶段 1 不是「重新启用」，是要接线**：(a) codegen 对堆指针写发射屏障（门控）；
(b) 激活 `gc_incr_state=1` 分片标记路径；(c) 补 Yuasa 删除半边。骨架（gc_incr_step state-1 分支、
gc_write_barrier 函数、gc_pend）现成可复用，但接线工作量比「re-enable」大。

### 8.2 补强：C2/B1 分档（比 §5 更锐利）

研究文档把后台线程方案分两档，本设计 §5 阶段 2/3 采纳此分档：
- **C2（推荐起点）**：起独立 marker OS 线程，但**并发严格限制在标记扫描段**；释放/清扫/收尾全在
  mutator 切片边界（现有纪律天然提供安全点）。正确性论证面最小。env 开关 `PYADAMAS_GC_CONCURRENT=0/1`
  灰度，关掉即回退现状 C1。
- **B1（C2 的自然放宽）**：允许标记窗口内任意释放路径并发，审计面更大。C2 做稳再长成 B1。
- 本设计 §5 阶段 1（M=1 增量标记 + 删除屏障，不开线程）保留为**正确性预验证里程碑**：先在单线程
  证明删除屏障修好 174/13，再上 C2 线程。两者不冲突，是递进。

### 8.3 补强：B2 隔离堆出局

每 M 隔离堆（vthread-design.md:232 选项一）与 pyadamas 语义**根本不兼容**：chan 是**转移 RC 引用跨 VT
共享对象**（rt_vthread.c send/recv 接管引用），隔离堆下跨堆指针非法，要么深拷贝（语义变+性能崩）
要么全局 arena（回到共享堆）。Python 对象自由共享语义不变则 B2 不可行。本研究只走共享堆（B1/C2）。

### 8.4 补强：阶段 1 设计须产出的三份硬文档（闸门）

1. **屏障覆盖审计表**：每条堆指针写路径（list set/append、dict set、实例字段、cell 写、生成器帧槽、
   chan 缓冲）逐条给「插屏障 / 免屏障证明（栈/标量/新对象）」。方法论复用 P3-2 边界装箱审计表
   （p3-1b-landing.md:128-129）。**覆盖率 100% 才算设计过关**。
2. **rc/marked 竞争论证**：rc 字段 mutator 写/marker 读、marked 位 marker 写/mutator 读。最小方案
   marked 用原子位操作、rc 在窗口内靠「快照+屏障」论证免原子。**这是 B1/C2 的核心正确性负担**，
   模板 = Go mbarrier.go:68-93 内存序论证（无条件 shade 换掉 store/load 屏障）。
3. **门控方案**：哪些模块发射屏障——**26 热基准按构造不进**（静态类型热路径无 unknown 堆指针写，
   根本不需屏障；容器 kind2 路径才付屏障，量级≈今天的 RC 探针）。P3 式模块门控保 26 asm 逐字节不变。

### 8.5 补强：收益上限与立项理由（诚实）

- M=1 下 GC 与 mutator 并行**只省标记墙钟，不省总 CPU**；对 GC 占比低的负载（26 基准多数）**收益≈0**，
  对 GC 重负载（13_gc 类、大堆 churn、10k VT）收益可观。**这不是多核加速**。
- **立项理由必须是 GC 重负载/大堆场景，不能用 26 热基准讲故事**。测量选型：13_gc/174_gc_construct_roots/
  大堆 churn/10k VT；abbench 15 轮，目标标记停顿下降可测、吞吐回归 <2% 噪声级。
- **多租户抖动风险**：marker 线程与 mutator 抢核（本机负载波动大，HANDOFF.md:22），可能反增抖动——
  集成前 abbench 复核（§5 阶段 3 闸门）。
- **标记窗口须设硬上限**（pacer 闭环），限制确定性析构偏离；窗口外析构语义不变。

### 8.6 验证设施（研究文档核实，沿用 P3 纪律）

difftest（-O0/-O2 自差分）/ run_tests.sh（normal+ASan）/ tests/diff/run_diff.py（CPython 对拍 --seeds 20）/
abbench.sh（交替 A/B+打乱+BURST）/ bench/compare/run_compare.py（**四语言对比已含 Go**，可复用为多核基准）/
PYADAMAS_GC_STATS/RC_STATS / ASan detect_leaks 钉子（13_gc/88_defaultdict/192_dict_cache/66_genexpr/119_gen_except_suspend）。
每块增量独立 worktree/分支、26 asm cmp、不推 master（除非授权）。

### 8.7 合并后的开放问题（待用户拍板，研究文档 §8 + 本设计 §7）

1. **目标**：隐藏 GC 停顿（C2/B1 足够）还是多核并行加速（必须 A/M:N，基准体系要先扩）？
2. **确定性析构**是不是硬语义承诺？不能降级 → A 出局，C2/B1 窗口设硬上限并文档化偏离。
3. **P3 投资**保不保？必须 → A 出局。
4. **26 asm 闸门**继续硬守（门控豁免）还是重新谈判？
5. **M>1 调度**本期范围？先 C2 可完全绕开。
6. 网络并发（libcurl/netpoll）排序：已暂缓；netpoll pollDesc 根登记与 GC 根快照协议有交集，设计预留。

## 附：与 A/C 路线的关系

- **A 全 Go 式**（去 RC + tracing + 每 P 分配 + M:N）：推翻 P3/RC/Python 析构语义，撞全 119 static 大山。B 不碰这些。
- **C 单后台 mark worker（M=1）**：其实就是 B 的阶段 2 的简化叙述；B 把它纳入完整混合屏障框架，先阶段 1 验证正确性再上线程。
- B 的本质：**RC 管非标记期的确定性回收（快、零原子），GC 混合屏障管标记期的并发循环回收**——两套所有权在时间上分域，而非 A 的全局替换。
---

## 8. 现状实测：增量 ≠ 并发（2026-11-27，0.700 之后）

> 用户观察：「我们的 GC 好像没有并发」。**确认属实**。
>
> ### 8.1 实测暂停分布（gcbench，默认配置，53 次收集）
>
> ```
> 2.259 ms     ← 主 GC（同步 drain）
> 1.228 ms     ← 次大
> 0.007 ms     ← 其余 51 次全部 ≤ 7 微秒
> ```
>
> 也就是：**绝大多数收集是亚毫秒，但最大暂停 2.3ms 来自同步标记**（`gc_drain()` 一次标完）。
>
> ### 8.2 三层状态的准确说法
>
> | 机制 | 现状 | 是否并发 |
> |---|---|---|
> | 清扫分片（`GC_SWEEP_BUDGET`） | 已实现，分配点推进 | ❌ 与 mutator 同线程，只是**分片**不是**并发** |
> | 标记分片（`PYADAMAS_GC_INCR_MARK=1`） | 已实现并验收（0.700） | ❌ 同线程，**增量**不是**并发**；且吞吐 −32%~−52% |
> | 后台标记线程（本文件 §3.2 阶段 2） | **未实现** | ✅ 这才是并发 |
>
> 关键区分：**分片（incremental）= 把一个长停顿切成一串小停顿，仍然占用 mutator 的 CPU；
>并发（concurrent）= 标记跑在另一个核上，mutator 不付这笔 CPU。** 我们只有前者。
>
> ### 8.3 阶段 1 的成果正好是阶段 2 的前提
>
> 0.700 验证了「标记期不释放任何对象」这条不变量（Yuasa 删除屏障 + DEFER 保活）。
>**这条不变量是后台线程可行的全部理由**：标记线程读对象时不会撞到已释放内存。
>没有它，后台线程就是 13_gc 那个 UAF。
>
> ### 8.4 最小可行实现（一次可验证的切片）
>
> 1. `runtime/CMakeLists.txt` + 顶层：`find_package(Threads REQUIRED)` → `Threads::Threads`；
> 2. `gc_work` 栈的 push/pop 加一把互斥锁（mutator 的屏障 push 与标记线程的 pop 并发）；
>    **扫描时不持锁**（`gc_scan_object` 内的 push 自己加锁），避免自锁；
> 3. 标记线程：`while (pop != NULL) gc_scan_object(h);` 栈空 → 置 `gc_mark_done=1` 退出；
> 4. `gc_incr_step` 在分配点改成：`gc_mark_done && work 空` → 进清扫；`gc_mark_done && work 非空` → 重启线程；
> 5. `gc_phase` / `gc_incr_state` / `gc_mark_done` 用 `_Atomic int`；标记器读指针用 `atomic_load`（对齐 8 字节，
>    x86 上编译成普通 load）；
> 6. **标记终止 STW** 保留在 mutator：最后一次 drain 残余工作 + 清扫 + `gc_incr_finish`（现有代码不动）；
> 7. 开关：`PYADAMAS_GC_THREAD=1`，默认关；关时零指令差异（沿用 `gc_incr_mark_fast` 同款门控）。
>
> ### 8.5 风险与验收
>
> - 风险 1：`gc_scan_object` 里有非线程安全的全局（`gc_cons_phase` / `gc_cons_note`）→ 标记线程单独跑时需确认只在 STW 段用；
> - 风险 2：池/freelist 在清扫期与 mutator 分配并发 → 本切片**不动清扫**（仍在 mutator），规避；
> - 风险 3：TSan 不可用时的数据竞争静默性 → 用 ASan + 全量套件 + 26 热基准 + 反复 100 轮压力跑；
> - 验收：`PYADAMAS_GC_BARRIER=1 PYADAMAS_GC_THREAD=1` 下套件 602 + ASan detect_leaks=1 全绿，
>   gcbench 最大暂停从 2.259ms 降到 < 0.3ms（根扫描 + 终止 STW），吞吐回退 < 5%。

---

## 9. 阶段 2（后台标记线程）实测结论（2026-11-27，0.705–0.711）

状态：线程可运行、正确、稳定（20/20 次收集无崩溃），但比同步标记慢约 24%。默认关闭。

### 9.1 已实现（0.705）

- 工作栈自旋锁 + 私有线程栈（__thread LIFO，0.706）；
- 线程 drain -> 置 done -> mutator 做 mark-termination（flush 屏障缓冲 + 清扫 + 收尾）；
- 关键修复（0.705）：标记期二级缓冲延后释放。线程遍历 list->data / dict->entries 时，
  mutator 会 list_buf_put / realloc 掉这些数组，导致线程读已释放内存。
  修法是标记期把释放/还池/复用挂进待释放链（pa_gc_defer_free），termination 时统一 free。
  修复前 8 次跑 5 次崩，修复后 20/20 稳定。

### 9.2 实测数据（gcbench）

模式 | 墙钟中位 | instructions | cycles | cache-misses | IPC
---|---|---|---|---|---
同步标记（默认）|0.170s|937.8M|371.9M|433K|2.47
增量标记（无线程）|0.190s|991.0M|431.5M|833K|2.32
增量+后台线程|0.210s|1074.4M|524.4M|1.92M|2.17

### 9.3 四次归因：两次被数据推翻、一次被回归测试抓回

1. 线程创建太贵 -> 加计数器（0.707）：spawn 20 次共 0.76ms、termination 0.00ms，推翻。
2. 工作栈 60 万次原子 RMW -> 私有栈（0.706）：墙钟 0 改善，推翻。
3. 1.1M 额外 cache miss 约等于 false sharing -> 按符号归因（0.710）：
   miss 分散在 libc free(24%)、gc_mark(18%)、finish(16%)、marker(10%)、scan(7%)，
   不是单一热点，推翻。
4. gc_mark 哈希查找可用 pad:u16 下标省掉 -> 实现 + 套件（0.711）：
   19 用例失败、3 段错误。保守扫描收到任意机器字，读头部即读野指针，抓回。

### 9.4 结论

gc_mark 的查表与保守扫描是 unknown 域类型精度的代价，不是可优化掉的浪费。
Go 每个槽都有精确类型、从不探查疑似指针；我们是保守扫描，任何可能是指针的值
必须走不 dereference 的路径。这是设计差异，不是技巧问题。

要降这部分开销只能：
1. 提升 unknown 域类型精度（编译器侧工作，减少保守扫描次数）；
2. 或接受并发标记的固有吞吐成本（Go 也付，靠 pacer 换停顿收益）。

### 9.5 重启时的顺序

1. 先量保守扫描次数 / 标记对象总数（9.4 的量化前提）；
2. pacer 改进（控制标记推进速率，不只控制触发时机）；
3. 线程常驻 + 条件变量（实测只值 0.76ms，但结构更干净）。

不要再尝试：标记位搬出对象头（0.710 证明 miss 不集中在那里）、
给保守路径加头部快路径（0.711 证明会崩）。
