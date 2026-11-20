# Go 式并发 GC 研究（pyadamas 运行时设计输入）

> 状态：**研究阶段产物**，未提交、不改任何现有源码。lead review 后才进入设计阶段。
> 参考源码：`.ref_proj/go/src`（Go 1.26.10，从 GOROOT=/usr/local/go 本地拷贝，
> 157MB/11483 文件，gitignored；地图见 `.ref_proj/go/README.md`）。
> 下文 Go 引用一律 `runtime/<file>:<line>`（相对 `.ref_proj/go/src/`，行号为 1.26.10 实测）；
> pyadamas 引用相对仓库根。
>
> **结论先行**：
> 1. pyadamas 已经在单线程口径下「Go 化」了相当一部分 GC 机制（分配点 mark assist、
>    Dijkstra 写屏障脚手架、GOGC 式阈值公式、增量清扫），**但真正的并发标记被 RC
>    所有权模型硬性卡住**——这一点不是推测，是 rt_core.c:2511-2523 里写明的实测结论。
> 2. Go 式 tracing GC 与 pyadamas 的三块地基**根本冲突**：RC 确定性析构、基于 RC 探针
>    的 P3 unknown 表示（0.327/0.328 刚落地）、M=1 零原子全局态。全 Go 式（路线 A）
>    等于推翻这三块重来。
> 3. 推荐 **C（M=1 + 单后台并发 mark worker）作为 stepping stone**，B（保留 RC 的
>    混合并发循环 GC）作为中间形态，A 仅在「多核并行是硬目标」时立项。分阶段路径与
>    闸门见 §7。开放问题见 §8。

---

## 1. pyadamas 现状事实（本轮逐条核实）

### 1.1 引用计数（RC）

- 隐藏头 `PyAdamasRc` 64B，在 payload 前 64 字节（rc 字段在 payload-48）：
  `next/prev/rc/dtor/release/size/kind/marked`（runtime/rt_internal.h:371-387，
  `_Static_assert(sizeof==64)` 供 codegen 字面量头对齐）。
- **dtor/release 双回调**是 RC+GC 混合的关键设计：dtor 在 rc 归零时跑（dec 子对象、
  释放缓冲）；release 只在收集期跑（只交出「指向可达对象的引用」，不 free）
  （runtime/rt_core.c:10-14）。
- inc/dec 编译期内联快路径（抄 Swift ARC/CPython）：几条指令，归零/收集期/统计模式
  才进慢路径（runtime/rt_internal.h:700-767）。字面量 rc=INT64_MIN 永生，直接跳过。
- 叶子优化：str/bytes 是「索引叶子」（LEAF，0.321）——留在存活堆索引里供 unknown
  探针识别，但不进 GC 堆链/新生代/阈值记账（runtime/rt_core.c:35-49）；exact-RC
  模式（无 unknown 的程序）索引整体关闭，叶子连索引都不进（UNTRACKED）。
- 确定性析构：rc 归零即刻跑 dtor 级联。这是 Python 资源语义（文件/锁/生成器帧的
  及时释放）与 CPython 对拍的基础，也是 P3 的地基（见 §1.5）。

### 1.2 增量分代循环 GC（runtime/rt_core.c）

- 两代按「是否在本轮收集集合」划分：新生代 young_buf（上次收集后分配），其余老生代。
  minor 只对新生代做 **trial-deletion**（CPython 式：试探减计数→rc>0 的是根→标记→
  恢复→清扫），老对象的真实 rc 天然是外部根，**不需要写屏障**
  （runtime/rt_core.c:1822-1839）。major 集合=整堆，走经典 mark-sweep。
- 阈值：minor 2MB 起（PYADAMAS_GC_THRESHOLD，rt_internal.h:389），major 8MB 起；
  收集后**按 Go pacer 公式重算**：`target = live + (live + roots)`（GOGC=100 口径），
  下限 2MB；major 阈值=老生代×2，零回收连续退避到 ×8（runtime/rt_core.c:2676-2727）。
- **增量机制已存在但只增量了清扫**：
  - 分配点驱动（「Go 的 mark assist」注释原文）：rc_alloc_core 入口一个合并标志
    gc_need_check，增量有待办时每 GC_INCR_STRIDE=16 次分配调一次 gc_incr_step，
    标记预算 64 对象/步、清扫预算 512 对象/步（runtime/rt_core.c:84-104, 1146-1172,
    2334-2366）。
  - **major 的标记是同步一次做完的**（根扫描+gc_drain，实测 ~1.5ms），只有清扫分片。
    原因写在代码里：「RC + 增量标记有硬冲突：标记期间对象可能被 RC 释放，标记队列
    里还留着指向它的指针……根因是 RC 的释放时机不受 GC 控制，而 Go 根本没有 RC。
    要真正实现并发标记，必须先让所有释放都经过 GC 的『灰对象』协议——那是另一套
    所有权模型（类似 Go 的 span + mbitmap），不是这里能补的」
    （runtime/rt_core.c:2511-2523）。
  - 小集合（<20000 对象）直接同步清扫，不分片（分片的固定开销反而更贵，实测
    bench_rc_gen +5ms）（runtime/rt_core.c:2525-2556）。
- **写屏障脚手架已存在但当前 dormant**：gc_write_barrier 是 Dijkstra 插入屏障
  （「写入即标灰」，注释明言照抄 Go、本地化去掉每 P 缓冲），导出符号
  pa_gc_write_barrier（runtime/rt_core.c:2284-2332）。但它只在 gc_incr_state==1
  （增量标记中）生效，而**当前代码没有任何路径把 gc_incr_state 置 1**（本轮 grep
  核实：major 直接进 state=2 清扫）；全仓库也没有 pa_gc_write_barrier 的调用点
  （src/ 与 runtime/ grep 均 0）。gc_alloc_black 同样是语义占位空操作
  （runtime/rt_core.c:2309-2322：新对象不在本轮快照集合里，天然不被 GC 收，靠 RC
  正常释放）。GC_PHASE_INCR_MARK 期间 dec 统一登记 gc_pend 延后释放的机制是现成的
  （runtime/rt_core.c:60-70, 165-199, 2238-2261）。
- 根集合四处：全局根表（pa_gc_add_roots 登记槽地址）、临时值栈 temp_stack、
  帧记录登记区（精确槽 + scan_mask 位图收窄的保守临时区）、setjmp 兜住的
  callee-saved 寄存器（runtime/rt_core.c:2499-2509, 1950-2044）。

### 1.3 精确帧链根与 vthread（M=1）

- PyAdamasFrame：codegen 序言压栈/返回弹栈，记录槽地址数组（低 2 位标志：
  bit0=可能非堆走探针，bit1=只扫描不释放的借用 unknown 引用）+ 临时区
  scan_base/scan_words/scan_mask 位图（超集，宁多勿少）+ CFA
  （runtime/rt_internal.h:391-417）。GC 只扫登记过的地址，不扫整段真实栈。
- vthread：**单 OS 线程协作式**，专用 libco 式调度器上下文（独立 mmap 栈），
  切换只换 rsp+callee-saved+rip（pa_vt_swap 汇编，runtime/rt_vthread.c:149-180）
  + 每 VT 一份「栈水位全局」save/load（frame_top/handler_top/temp_stack/异常上下文，
  runtime/rt_vthread.c:349-377）。让出点只在语句边界运行时调用（chan/sleep/join），
  纯标量死循环不让出（无抢占）。
- **GC 只在 vthread 切片间跑**（docs/vthread-design.md:121-149）：运行栈走既有精确
  根；挂起栈由 pa_vt_gc_scan 遍历 vt_all 链，逐 VT 复用 pa_gc_scan_frames_from
  精确扫帧链 + temp 栈；callee-saved 寄存器/fn 闭包/chan 寄存值在 VT payload 里，
  由 OBJ 保守扫描覆盖（可能误钉恰好等于堆地址的标量字，文档写明）
  （runtime/rt_vthread.c:811-824）。
- **零原子零锁**：rt_core.c 138 个顶层 static 声明（其中 79 个静态全局变量，
  其余静态函数；vthread-design.md:232 的「119 个 static」是当时口径），整个
  runtime/ 无 _Atomic/__sync/pthread_mutex（本轮 grep 核实；rt_re.c 的 "atomic"
  是正则原子组，无关）。M>1 被明确列为深坑：每 M 隔离堆 或 共享堆+原子化全部
  static（docs/vthread-design.md:230-233）。

### 1.4 池化分配

- rc_alloc_core 按 kind 路由 freelist：str_pool（8B 档位×N，小档容量 512/大档 64）、
  list_pool（32B 结构体）、list_inline_pool（112B 合并分配）、dict_pool（136B+缓冲）、
  float_box_pool（runtime/rt_core.c:406-594, 1176-1194）；未命中走 malloc。
  池命中不重写 dtor/release/kind/size（同池同值不变量，runtime/rt_core.c:1195-1203）。
- 单线程下这些池天然无锁——**等价于 Go 每 P mcache 的「P=1」退化情形**，但没有
  span/sizeclass 体系，也没有中央缓存层（池满直接 free 回 libc）。

### 1.5 P3 封闭 tagged union（0.327/0.328，刚落地）

- unknown 值仍是裸 i64；**平行 shadow tag 跟值走**（帧内平行 tag 区/全局平行符号/
  直调旁路双字 ABI：%r11 形参 tag 数组 + %rdx 返回 tag），tag 编码
  0=UNK/1=INT/2=FRAW/3=FBOX/4=STR/5=HEAP，堆⇔tag≥3
  （docs/p3-tagged-union-design.md:88-100；docs/p3-1b-landing.md:20-45）。
- **tag 是提示不是语义**：payload 单独仍是合法旧 unknown i64，任何边界丢 tag 只损失
  性能不损失正确性——回退路径就是 **RC 探针**（pa_unknown_is_heap：小整数短路 +
  rc_index_has 哈希索引 + exact-RC 堆链回退，runtime/rt_internal.h:674-684；
  pa_any_load_tag/pa_any_arith_t，runtime/rt_any.c:434/:466）。tag≥3 直接
  rc_inc/dec 免探针，tag 1/2 跳过 RC。
- 门控=模块 IR 含 AnyArithT/AnyCmpT/AnyNegT；26 个热基准全部不含 pa_any_* →
  门控全关 → **asm 逐字节不变按构造成立**（docs/p3-tagged-union-design.md:139-150）。
  落地纪律：每块增量后 26/26 asm cmp IDENTICAL + difftest + ASan leak 抽查
  （docs/p3-1b-landing.md:10-18）。
- **P3 的正确性模型整个压在 RC 上**：探针靠存活堆索引判「这个 i64 是不是堆指针」，
  RC 管生死。去掉 RC = 探针失去判据 = tag 从「提示」被迫变成「语义」= P3 一期
  硬约束（正确性不依赖传播完备）被推翻。

---

## 2. (a) Go 并发 GC 架构

### 2.1 总体性质

Go GC 是**并发、精确（type accurate）、非分代、非移动**的三色标记-清除，带写屏障，
允许多 GC 线程并行；分配用按 size 隔离的每 P 分配区，常见情况无锁
（runtime/mgc.go:5-10）。思想源头是 Dijkstra on-the-fly 算法（mgc.go:16-22）。
非分代是刻意选择：Go 的逃逸分析+栈分配把大量短命对象挡在堆外，堆上分代收益不抵
写屏障/ remembered set 复杂度（这一判断对 pyadamas 不完全适用——pyadamas 的
list/str churn 全在堆上，所以现有分代 trial-deletion 是有收益的，见 §4）。

### 2.2 GC 周期：两次短 STW + 并发标记/清扫

权威注释在 mgc.go:24-82，五步：

1. **Sweep termination（STW #1 的前半）**：stopTheWorld 让所有 P 到 GC 安全点；
   扫完上轮遗留的未清扫 span（正常情况 proportional sweep 已扫完，强制 GC 才有）。
2. **Mark setup（仍在 STW #1 内）**：gcphase 置 _GCmark，**全体 P 同时启用写屏障**
   （必须 STW 保证「没有任何对象在被扫描前屏障已生效」）、启用 mutator assist、
   根标记作业入队（mgc.go:33-37）。
3. **并发标记（startTheWorld 之后）**：mark worker（调度器调度的 G）+ 分配路径上的
   assist 共同排空灰色对象队列；写屏障对任何指针写**同时 shade 旧值和新值**；
   新分配对象立即标黑（allocate-black，mgc.go:39-44）。根作业包括扫所有 G 栈
   （扫单个栈时 suspend 该 G、shade 栈上指针、resume）、shade 全局、shade 运行时
   off-heap 结构里的堆指针（mgc.go:46-50）。
4. **Mark termination（STW #2）**：分布式终止检测（gcMarkDone，见 §2.5）确认无根
   作业无灰对象后，再次 STW，gcphase 置 _GCmarktermination，停 worker/assist，
   flush mcache 等家务（mgc.go:61-68）。
5. **并发/lazy sweep**：gcphase 置 _GCoff、关写屏障、startTheWorld；此后新分配对象
   是白色，**分配时按需先清扫 span**（lazy），后台 bgsweep goroutine 也持续扫
   （mgc.go:70-79, 84-110）。下轮 GC 在分配量达到 pacer 目标时重启（§2.4）。

代码对应：gcStart（mgc.go:733）里 STW #1 = stopTheWorldWithSema(stwGCSweepTerm)
（:835）→ finishsweep_m（:870）→ clearpools（:875）→ gcController.startCycle
（:881）→ setGCPhase(_GCmark)（:916）→ gcBgMarkPrepare/gcPrepareMarkRoots
（:918-919）→ gcMarkTinyAllocs（:926，tiny 块整块标黑）→ gcBlackenEnabled=1
（:933，assist 开闸）→ startTheWorldWithSema（:948）。
STW #2 在 gcMarkTermination（mgc.go:1344）：setGCPhase(_GCmarktermination)
（:1346）→ gcMark 收尾排空（:1366，实体 :1956）→ setGCPhase(_GCoff)（:1387，
**写屏障此刻关闭**）→ gcSweep（:1388，实体 :2049：sweepgen+=2、唤醒 bgsweep）→
gcControllerCommit 定下轮触发/目标（:1412）→ startTheWorldWithSema（:1515）。

**关键不变量**：写屏障只在 _GCmark/_GCmarktermination 期间开；「白→黑」指针写
必须被屏障拦截（no-white-to-black invariant，mgc.go:928-932 注释）。两次 STW 都
只做 O(根/家务) 的工作，重活（扫堆、扫栈、清扫）全在并发段。

### 2.3 混合写屏障（Go 1.8+，hybrid barrier）

权威注释 mbarrier.go:24-64。伪码：

```
writePointer(slot, ptr):
    shade(*slot)                  // Yuasa 删除屏障：shade 被覆盖的旧值
    if current stack is grey:
        shade(ptr)                // Dijkstra 插入屏障：shade 新值（栈灰时）
    *slot = ptr
```

- 删除部分防「把堆里唯一指针搬到栈上藏起来」；插入部分防「把栈上唯一指针写进
  已黑的堆对象藏起来」；**栈一旦扫黑，插入部分可省**——这是 Go 1.8 消除
  mark termination 重扫栈（rescan）的关键（证明见 mbarrier.go:63-64 引用的
  proposal 17503）。
- **不做「slot 所在对象颜色」条件化**：mutator 写 slot 与 GC 写 mark bit 之间
  存在 store-load 重排（x86 也有），条件化需要昂贵内存屏障；Go 选择无条件
  shade，用「总是灰化 ptr」换掉 membar（mbarrier.go:68-93）。
- **栈写免屏障**（编译器对当前帧的写不插屏障，mbarrier.go:96-101）；**全局写要
  屏障**（避免 termination 重扫全局，mbarrier.go:104-111）；屏障是
  **pre-publication**（先屏障后写入，mbarrier.go:114-118）。
- 实现分两层：
  - 单指针写：编译器插 gcWriteBarrier 汇编快路径（asm_amd64.s:1885-1906，
    1..8 个寄存器变体）——查 writeBarrier.enabled，开则把 old/new 塞进
    **每 P wbBuf**（512 槽，mwbbuf.go:42-75），满则 wbBufFlush（:166/:195）
    批量 shade 成标记工作。缓冲把「每次写一次原子/队列操作」摊薄成 512 次一flush。
  - 批量写（typedmemmove/typedslicecopy/typedmemclr，mbarrier.go:150/:276/:361）：
    bulkBarrierPreWrite（mbitmap.go:388）按类型指针位图逐槽取 old/new 入 wbBuf；
    dst 是全局走 data/bss 位图（:397-410），dst 是栈直接跳过（:412-419）。
- shade（mgcmark.go:1632）→ findObject → greyobject（:1648）：置 mark bit、
  noscan 对象直接算黑（:1687-1690）、否则入本 P gcw 队列（:1698）。
- **对 pyadamas 的启示**：现有 gc_write_barrier 只有 Dijkstra 插入半边
  （标灰新值），没有 Yuasa 删除半边。单线程增量标记下「旧值被覆盖后仍可达性
  丢失」的场景被 RC 兜住（旧值 rc 归零前标记队列可能还引用它——这正是
  gc_pend 延后释放要解决的）；真要并发化，删除半边 + 栈黑条件都要重新论证
  （§5 路线 B/C 详述）。

### 2.4 GC pacer（mgcpacer.go）

gcControllerState（:92）是 pacer 全部状态，字段注释即设计文档：

- **目标堆**：GOGC（默认 100）→ `gcPercentHeapGoal = live + live*GOGC/100`；
  GOMEMLIMIT 软上限取更小者（heapGoalInternal :1002-1045）；heapMinimum=4MB×GOGC/100
  防小堆高频 GC（:106-118）；sweep 距离下限、trigger+64KB 最小 runway 等修正。
  **pyadamas 已抄了主公式**（rt_core.c:2676-2695），缺的是 assist 汇率与 worker 配额。
- **consMark 比率**（:126-143）：应用分配速率(B/cpu-ns) ÷ GC 扫描速率(B/cpu-ns)，
  每轮 endCycle（:600）用实测更新、4 轮滑动窗口。它决定「要多少 GC CPU 才能
  在目标堆前扫完」——是 assist 汇率和 worker 配额的共同输入。
- **worker 配额**：gcBackgroundUtilization=0.25（:39）——**GC 目标占 25% CPU**。
  startCycle（:386）算 dedicated worker 数 = round(GOMAXPROCS×0.25)（:403-405），
  余数由 fractional worker 补（fractionalUtilizationGoal :339-349）；idle worker
  只在有空闲 P 时跑（addIdleMarkWorker :1439）。
- **assist 汇率**：assistWorkPerByte/assistBytesPerWork（revise :492 随 heapLive
  逼近 goal 动态收紧）——分配越多欠债越多，逼 mutator 自己扫（§2.6）。
- **trigger**（:1190）：本轮实际触发点，commit（:1276）在 mark termination 时
  把 goal/trigger 定死供下轮用。heapLive 只在从 mcentral 拿 span 时批量更新
  （高估优于低估，:177-196 注释；mcache.go:221-235）。

### 2.5 mark worker 与分布式终止

- **worker 是普通 G**：gcBgMarkStartWorkers（mgc.go:1672）给每个 P 起一个
  gcBgMarkWorker G（:1750），平时 park 在**无锁栈 gcBgMarkWorkerPool**里，
  由 pacer 的 findRunnableGCWorker（mgcpacer.go:861）在 schedule/findRunnable
  里按配额唤醒（proc.go:3433）。worker 不退出，GOMAXPROCS 变化复用。
- 三种模式（mgc.go:1858-1882 分派；drain 变体 mgcmark.go:1176-1246）：
  - **dedicated**：占满一个 P 的 25% 配额份额，gcDrainMarkWorkerDedicated
    先可抢占地 drain（被抢占就把本地 runq 倒进全局队列让别的 P 跑），再不可抢占
    地 drain 到没活；
  - **fractional**：补 dedicated 取整余数，pollFractionalWorkerExit 到点就退；
  - **idle**：只在 P 空闲时跑，pollWork 一有用户活就让位。
- **gcWork 每 P 双缓冲**（mgcwork.go:83-137）：wbuf1/wbuf2 两个 workbuf 拼成
  逻辑栈，一个缓冲的滞回摊薄全局队列操作；全局只有 full/empty 两条 workbuf 链
  （putfull/trygetfull/handoff :490/:499/:509）。取活顺序：本地 obj→本地 span→
  全局 obj→全局 span→flush wbBuf→偷 span（gcDrain :1319-1338，顺序理由见
  mgcwork.go:64-68：先本地后全局、先对象后 span）。
- **分布式终止**（gcMarkDone，mgc.go:1015）：work.nproc/nwait 原子计数
  （gcBeginWork/gcEndWork :1933/:1944），「所有 worker 都在等 + 全局无活」
  只是**疑似**完成——本地缓冲可能还藏着活。gcMarkDone 用 **ragged barrier**
  （forEachP 逐 P flush 本地 gcw/wbBuf，两遍确认 flushedWork 标志）把疑似变成
  确定，才进 STW #2。这是「无全局锁的分布式快照」经典手法。
- **大对象 oblet**（mgc.go:120-127）：超过 maxObletBytes 的对象拆成 oblet 分批扫，
  防单对象长停顿、提高并行度。pyadamas 的大 list data 缓冲扫描有同款问题
  （gc_scan_object 一次扫整个 l->data，rt_core.c:2051-2066）。

### 2.6 mutator assist（分配方为 GC 付账）

- mallocgc 入口：gcBlackenEnabled 时 deductAssistCredit(size)（malloc.go:1112-1114）
  扣本 G 的 gcAssistBytes 信用；欠债超阈值 → gcAssistAlloc（mgcmark.go:499）：
  按 assistWorkPerByte 把欠债字节换算成扫描工作量（:563-570），**先偷后台
  worker 攒的 bgScanCredit**（:578-588，偷不够再自己扫），gcAssistAlloc1
  （:711）实际 drain；扫不够就进 assistQueue 排队等 worker 喂信用
  （gcFlushBgCredit :836 把后台扫描量分给排队者，gcWakeAllAssists :784）。
- 效果：分配速率超过 GC 扫描能力时，mutator 被**按比例减速**而不是让堆爆掉；
  GC 目标 CPU 占比（25%）由 pacer+assist 共同闭环。
- **pyadamas 已有单线程退化版**：rc_alloc_core 的 gc_incr_remaining 倒计数 +
  gc_incr_step 有界预算（rt_core.c:1146-1172, 2334-2366），注释明言「这正是 Go 用
  gcBgMarkWorker + gcAssistAlloc 做的事，只是我们单线程，靠分配点天然分片」。
  缺的是「按欠债比例」的闭环（现在是固定预算）和真正的后台执行体。

### 2.7 span 堆与每 P 无锁分配（四级漏斗）

1. **mcache（每 P，无锁）**：alloc[spanClass] 各缓存一个 span + tiny 块
   （mcache.go:20-66）。小对象分配 = 从当前 span 的 freeindex 拿一个 slot，
   几条指令，无原子无锁。span 用完才 refill（:160）。
2. **mcentral（每 sizeclass，带锁）**：partial/full × swept/unswept 四个 spanSet
   （mcentral.go:22-77）；cacheSpan（:82）优先拿已清扫的 partial，没有就扫一个
   （**sweep 与分配在这里耦合**），再没有向 mheap 要新 span（grow :251）。
3. **mheap（全局，带锁）**：页堆 arena + pageAlloc；alloc（mheap.go:1006）
   **先 reclaim（:832，扫未清扫 span 回收页）再 allocSpan（:1224）**，不够才
   grow（:1553，sysAlloc 向 OS 要）。sweepgen 代际号区分「本轮已扫/待扫」。
4. **fixalloc（运行时元对象）**：mspan/mcache 本身用定长分配器（mfixalloc.go:31），
   freelist+chunk，**调用方负责加锁**（:25 注释）——Go 把「GC 自己的内存」与
   「用户堆」彻底分开，元对象不在 GC 堆里（NotInHeap），避免自指。
- sizeclass：68 档（8B..32KB，internal/runtime/gc/sizeclasses.go:90-100），
  roundupsize（msize.go:16）；>32KB 大对象直接走 mheap 整 span。
- **GC 与分配的耦合点**：mark termination flush 所有 mcache（releaseAll
  mcache.go:290 / prepareForSweep :350）——缓存的 span 交回 central 才能被 sweep；
  下轮拿 span 时按需先扫（lazy sweep）。**「分配即清扫」是 Go 不需要独立清扫
  停顿的原因**。
- 对照 pyadamas：池=「P=1 的 mcache」，但池满直接 free（没有 central 层缓冲），
  清扫是独立阶段（gc_incr_step 的 sweep 段）而不是分配耦合。M=1 下这不是问题；
  M>1 下全局池就是第一个要原子化/每 M 化的东西。

### 2.8 栈扫描：精确栈图 + 栈对象 + 保守兜底

- 根作业 markroot（mgcmark.go:221）：全局 data/bss 按编译器位图扫（:226-236）、
  finalizer 块（:238）、span specials（:257）、**每个 G 的栈**（:261-313）：
  suspendG 停住目标 G（运行中的 G 靠抢占/安全点停）→ scanstack → resumeG。
  **扫栈是逐 G 并发的，不需要全局 STW**——这是 Go 栈扫描与 pyadamas「切片间
  一次性全扫」的最大结构差异。
- scanstack（:904）逐帧 scanframeworker（:1069）：正常帧用编译器栈图
  （getStackMap :1127，精确到每个 PC 点的活指针槽）；**asyncPreempt/debugCall
  帧及其父帧保守扫**（:1076-1124，scanConservative :1524）——Go 也承认
  「异步停下来的帧没有精确图」，用保守兜底。
- **栈对象追踪**（mgcstack.go:9-93）：取地址的栈变量构成「栈内堆」，用迷你
  tracing 判定哪些栈对象还活着（死的栈对象里的堆指针不算根）——比「整帧保守」
  精确，比「全精确数据流」便宜。
- 对照 pyadamas：帧链 scan_mask 位图是**每帧一份静态超集**（不是每 PC 一份活
  指针图），精度介于 Go 栈图与整帧保守之间；VT payload 保守扫描 ≈ Go 的
  asyncPreempt 帧保守兜底。**pyadamas 的根机制并不落后，落后的是「并发扫栈的
  停 G 协议」**（M=1 下不需要，M>1 下必须有）。

---

## 3. (b) GMP 调度如何与 GC 协作

- **G/M/P**：G=goroutine（栈+上下文），M=OS 线程，P=逻辑处理器（持有 mcache/
  gcw/wbBuf/本地 runq，runtime2.go:780/:877/:882）。M 必须绑 P 才能跑 G；
  P 的数量=GOMAXPROCS。**GC 的并行度、无锁分配、本地标记队列全部挂在 P 上**。
- **stopTheWorld 安全点机制**（stopTheWorldWithSema，proc.go:1628）：
  持 sched.lock，置 sched.gcwaiting=true、stopwait=GOMAXPROCS，preemptall
  （:6843，逐 P 设抢占标志/发信号）；当前 P 直接 _Pgcstop；syscall 中的 P
  被 retake（:1668-1673）；空闲 P 直接停（:1677-1685）；其余 P 在**安全点**
  自愿停（函数序言栈检查/异步抢占信号，isAsyncSafePoint preempt.go:390 判定
  「此处栈图有效、无写屏障在途」），每 100us 重发抢占防丢（:1690-1699）。
  startTheWorldWithSema（:1760）恢复所有 P 并顺手 netpoll(0) 收就绪 G（:1765）。
- **schedule 与 GC 的交织**（proc.go:4135）：每轮 findRunnable（:3389）里——
  gcwaiting 时进 gcstopm（:3296）停 M；**优先检查本 P 是否该跑 mark worker**
  （findRunnableGCWorker :3433，pacer 按 dedicated/fractional/idle 配额决定），
  选中 worker 就执行它（worker 也是 G，跑完 park 回池）；没用户活时
  netpoll(0)/netpoll(delay) 收网络就绪 G（:3496/:3754）；handoffp（:3131）
  交接 P 时也检查 GC worker（:3146）。schedule 选中别的 G 时释放预分配的
  worker（releaseNextGCMarkWorker :4181）。
- **gcStart 由分配触发**：mallocgc 各分路末尾 gcTrigger{gcTriggerHeap}.test()
  （malloc.go:1335/:1447/:1587/:1680/:1754）→ gcStart（mgc.go:733）。
  gcStart 先并发 sweepone 把遗留 span 扫完（:767），拿 startSema/gcsema/worldsema
  三把语义锁防重入（:772/:791-792），然后 §2.2 的 STW #1 流程。**触发在 mutator
  路径上，执行主体是调度器调度的 worker G**——这是「GC 是普通用户态工作」的体现。
- **worker 作为 G 的深意**：GC 不占专用线程、不吃调度器特殊路径（除了配额优先级），
  park/wake 走 gopark/goready（proc.go:445/:481），与用户 G 完全同构。
  pyadamas 的 vthread 调度器（vt_sched_loop，rt_vthread.c:460-509）结构上已经
  具备「把 GC worker 当一个 VT 调度」的骨架——但 M=1 下它只能分时，不能并行。

---

## 4. (c) netpoller 简述（留待网络阶段）

- 平台无关层 netpoll.go：每个 fd 一个 pollDesc（**NotInHeap，无堆指针**，:74-76），
  rg/wg 两个二值信号量（pdNil/pdReady/pdWait/G 指针四态，:51-68）。G 等 IO 时
  netpollblock（:548）把 G 指针 CAS 进信号量然后 **gopark——G 挂起不占 M**；
  IO 就绪 netpollunblock（:591）取出 G 指针 → netpollready（:494）进就绪列表。
- Linux 实现 netpoll_epoll.go：epoll_create1（:36）、fd 注册
  EPOLLIN|EPOLLOUT|EPOLLRDHUP|**EPOLLET 边沿触发**（:64）、eventfd 做
  netpollBreak 唤醒（:81）；netpoll（:113）EpollWait 最多 128 事件，按
  pd 解包就绪模式。kqueue（macOS/BSD）/IOCP（Windows）同构替换。
- 调度集成：findRunnable 空闲时 netpoll(0) 非阻塞收（proc.go:3496/:3813），
  全空时 netpoll(delay) 阻塞到最近 timer（:3754）；startTheWorld 也收一把（:1765）。
  **十万并发连接 = 十万 park 的 G + 一个 epoll fd，线程数不随连接数涨**。
- 对 pyadamas：现有 rt_async.c 事件循环 + rt_vthread 的 sleep 是 nanosleep 忙等
  （rt_vthread.c:235-240, 467-485），chan 阻塞是纯用户态队列。网络阶段要把
  fd 就绪接入 vt_timers/runq（epoll + eventfd 唤醒调度器），pollDesc 状态机
  可直接抄。**与 GC 的交集**：pollDesc 这类 off-heap 根要登记进根扫描
  （Go 用 NotInHeap+显式根作业）；pyadamas 对应 pa_gc_add_roots 登记槽地址。
  本阶段不展开。

---

## 5. (d) 逐机制映射到 pyadamas

| 机制 | Go 1.26 | pyadamas 现状 | 差距/冲突 |
|---|---|---|---|
| 所有权模型 | 纯 tracing，无 RC；对象生死只由可达性决定 | RC 为主（确定性析构）+ 循环 GC 兜底 | **根本差异**。Go 没有「rc 归零即刻 free」，sweep 前对象内存恒在；pyadamas 的 RC 随时 free，标记队列可能悬空（rt_core.c:2511-2523 实测结论） |
| 分代 | 非分代（逃逸分析挡掉多数短命对象） | 分代 trial-deletion（minor）+ 整堆 mark-sweep（major） | pyadamas 堆上 churn 多，分代有实测收益；Go 的非分代判断不可直接搬 |
| 标记并发度 | 多 worker G 并行 + assist，25% CPU 配额闭环 | major 标记同步 ~1.5ms；只有清扫增量（分配点驱动） | 缺后台执行体 + 缺 RC 与标记的释放协议 |
| 写屏障 | 混合（Yuasa+Dijkstra+栈黑条件），编译器全量插入，每 P wbBuf 摊薄 | Dijkstra 半边脚手架（rt_core.c:2324），**dormant**（无调用点、state=1 不可达）；minor 靠「老对象 rc 是根」免屏障 | 并发化必须补删除半边+覆盖审计；热路径开销要门控（§6） |
| allocate-black | 新对象立即标黑（mallocgc 路径） | 新对象不在本轮快照集合，天然不被收，靠 RC（gc_alloc_black 空操作占位，rt_core.c:2309-2322） | 语义等价但论证不同：Go 靠「黑」，pyadamas 靠「集合快照+RC」 |
| pacer | consMark 比率 + assist 汇率 + worker 配额 + GOMEMLIMIT | GOGC 主公式已抄（rt_core.c:2676-2695）+ 零回收退避 | 缺闭环（固定预算而非按欠债比例）；单线程下够用 |
| 根扫描 | 逐 G suspendG→精确栈图→resumeG，并发；保守兜底异步抢占帧 | 切片间一次性：全局根表+temp 栈+帧链 scan_mask+setjmp 寄存器+VT 挂起栈精确扫+VT payload 保守 | 精度相当；**缺「运行中栈的停 G 协议」**（M=1 不需要，M>1 必需） |
| 分配 | mcache(每P无锁)→mcentral(带锁)→mheap(带锁)→OS；分配耦合 lazy sweep | 全局 kind 池→malloc；清扫独立增量阶段 | M=1 下池=退化 mcache，无锁已成立；M>1 下池是第一批要每 M 化/原子化的 static |
| 调度协作 | worker 是 G，pacer 配额调度；STW 靠安全点+抢占 | GC 只在 VT 切片间跑；调度器是专用 sched 上下文 | 骨架同构（可把 worker 当 VT），但 M=1 无并行；无抢占=无异步安全点 |
| 析构语义 | 无析构，finalizer 异步不保证时机 | dtor 级联确定性（Python 资源语义） | **去 RC 即失去**；CPython 对拍/用户代码依赖及时释放 |
| unknown 表示 | 静态类型+接口 itab，运行期类型精确 | P3 shadow tag（提示）+ RC 探针（兜底） | **探针判据=存活堆索引=RC 体系**；去 RC 则 P3 正确性模型崩塌 |
| 原子/锁 | 全 runtime 原子+锁分层（lockRank） | 零原子零锁（138 顶层 static/79 静态变量，rt_core.c） | M>1 的第一座山（vthread-design.md:232 已列） |

**三个必须点名的张力**（不回避）：

1. **RC 释放时机 vs 标记队列悬空**。这是已经实测撞过的墙：13_gc 的
   heap-use-after-free（ASan）、174_gc_construct_roots 漏 21KB，根因都是
   「标记期间 RC 把队列里还指着的对象 free 了」。现有解法是 gc_phase 非 0 时
   dec 全部登记 gc_pend 延后释放（rt_core.c:165-199）——**但这只是把问题推迟到
   收尾，且收尾后仍有对象归零的时序洞**（rt_core.c:2513-2516 注释原文）。
   Go 没有这个问题是因为 Go 对象在 sweep 前内存恒在。要在保留 RC 的前提下
   并发标记，必须让「标记期所有 free 走 GC 协议」——等于在标记窗口内临时
   废除确定性析构，窗口外恢复。这个混合语义要逐路径审计（EH 展开、生成器帧、
   temp 栈 cleanup、VT 死亡级联都在窗口内可能跑）。
2. **P3 与去 RC 互斥**。P3 一期硬约束是「tag 丢失=回退探针=旧行为，正确性不
   依赖传播完备」（p3-tagged-union-design.md:99-100）。探针的判据是
   rc_index_has/堆链回退——**整个「这个 i64 是不是堆指针」的判定建立在
   RC 存活堆索引上**。tracing GC 下对象不再按 rc 生死，索引语义消失；
   unknown 槽里的裸 i64 要么强制 tag 完备传播（P3-2 评估已论证：正确性从
   「不依赖完备」变「依赖边界装箱完备」，约 20 处边界审计，
   p3-1b-landing.md:110-129），要么全量保守扫描（性能回退到 0.302 之前）。
   **刚落的 0.327/0.328 四块增量（双字 ABI、dict 导 tag、生成器 vreg tag、
   三个 leak/UAF 根治）全部以 RC 为前提**，去 RC 不是「调整」是推翻。
3. **写屏障对热路径的开销 vs 26 基准 asm 闸门**。Go 的屏障成本被三件事压住：
   栈写免屏障、标量写免屏障、wbBuf 512 槽摊薄。pyadamas 若全量插屏障：
   静态类型热路径（26 基准的主体）**根本不需要屏障**（没有 unknown 堆指针写），
   但容器 kind2 路径（list append/set、dict set 的 unknown 新值）每次写入多
   「读全局标志+分支+可能入队」——与今天每次写入的 RC 探针同量级，不是免费。
   更硬的约束是纪律：**26 热基准 asm 逐字节不变是 P3 立下的闸门**
   （p3-1b-landing.md:18），任何屏障发射进这些路径都违反闸门，除非像 P3 一样
   按模块门控（无 unknown 动态写入的模块不发射）。Go 没有这个闸门——Go 接受
   全量屏障的个位数百分比开销换并发；pyadamas 要不要接受，是用户决策（§8）。

---

## 6. (e) 可选路线与诚实权衡

### 路线 A：全 Go 式（去 RC，并发 tracing mark-sweep + 混合写屏障 + 每 P 分配 + M:N）

**形态**：堆对象去掉 rc 字段（或保留为调试计数），生死由并发标记决定；
dtor 语义改为 sweep 期回调（时机不确定）；编译器对所有堆指针写插混合屏障
（栈写/标量写豁免）；池改每 P mcache + 中央 span 缓存；vthread 升 M:N，
138 个 static 按「每 P 化 / 原子化 / 锁分层」三类处理；帧链根配 suspendG
式停 VT 协议；P3 tag 从提示升为语义（或 unknown 全保守）。

**收益**：真并行 GC（多核扫描）、无 RC 级联停顿、无 rc 字段缓存行争用、
与 Go 生态机制完全对齐（netpoll/抢占/可增长栈都有现成蓝图）。

**代价（诚实清单）**：
- **推翻 P3**：0.327/0.328 全部重做或废弃；unknown 表示回到「要么完备 tag
  要么全保守」的二选一，两条都比现状贵（§5 张力 2）。
- **失去确定性析构**：Python 资源语义（with/文件/锁的及时释放、CPython 风格
  __del__ 时机）变成「最终一致」。现有语料里依赖及时释放的用例（13_gc、
  88_defaultdict、192_dict_cache 等 ASan 抽查钉子）行为会变，CPython 对拍
  可能出现时序性差异。这是**语义降级**，不是实现细节。
- **全量审计面**：EH 展开（rt_eh.c/rt_personality.c 按帧槽 dec）、temp 栈
  cleanup、生成器帧（rt_gen.c）、VT 死亡级联（rt_vthread.c）、池路由
  （同池同 dtor 不变量）——全部从「RC 归零即释放」改写成「tracing 协议」。
  这是 rt_core.c 2880 行 + rt_internal.h 1679 行 + 周边 ~26000 行 runtime 的
  所有权模型重写。
- **M:N 调度独立成山**：work-stealing、抢占（信号+安全点+寄存器 spill 协议，
  vthread-design.md:237-238 已列深坑）、可增长栈、跨栈 CFI——每一项都是
  Go 花了数年打磨的 subsystem。
- **性能不确定性**：tracing 对「大量短命小对象」的 PyAdamas 负载未必比
  RC+池快（RC 的释放是 O(1) 且缓存友好；tracing 要扫全堆+屏障）。Go 靠逃逸
  分析把 churn 挡在堆外，pyadamas 没有等价物（list/str 全堆分配）。
  **26 基准大概率回归**，闸门本身要重新谈判。

**判断**：只有在「多核并行是产品硬目标、且愿意接受析构语义降级+P3 重做」时
才值得。研究阶段不建议直接立项 A。

### 路线 B：保留 RC，仅把循环 GC 并发化（混合）

**形态**：RC 仍是日常所有权（确定性析构保留）；循环 GC（trial-deletion/
mark-sweep）的**扫描阶段**放到后台线程并发跑，mutator 侧加屏障+释放协议。
两个子形态：

- **B1 共享堆 + 单后台 marker 线程**（M=1 mutator + 1 marker OS 线程）：
  marker 只读对象图+置 marked 位；mutator 的 RC 照旧但标记窗口内 free 走
  gc_pend 延后（机制现成）；容器指针写插 Dijkstra+Yuasa 屏障（脚手架现成，
  要补删除半边+接线）；rc/marked 字段用原子或「marker 只碰快照集合」纪律
  避免数据竞争。帧链根在 VT 切片边界快照（现有纪律天然提供安全点）。
  **这实际上是路线 C 的完整体**（见下），区别只在 B1 允许标记窗口内
  mutator 继续跑（真并发），C 的保守版只在切片间推进。
- **B2 每 M 隔离堆**（vthread-design.md:232 选项一）：每 M 独立 RC/GC/池，
  零共享零原子。**但 pyadamas 的 chan 是转移 RC 引用跨 VT 共享对象的**
  （rt_vthread.c chan send/recv 接管引用）——隔离堆下跨堆指针非法，要么
  深拷贝（语义变、性能崩），要么全局共享 arena（又回到共享堆问题）。
  Python 对象自由共享的语义与隔离堆根本不兼容，**B2 基本不可行**，
  除非把语言语义改成「跨线程只传值/不可变」（那是另一个语言设计决策）。

**收益**：保留确定性析构（标记窗口外）、保留 P3（RC 探针判据不变）、
保留 M=1 调度（B1 只加一个不参与调度的 marker 线程）、major 标记 ~1.5ms
停顿被后台吸收。改动面集中在 rt_core.c GC 段 + 屏障接线 + 少量原子。

**代价**：
- **标记窗口内确定性析构暂停**（gc_pend 延后）——窗口多长、用户代码在窗口内
  跑多少，决定语义偏离多大。需要给窗口长度设硬上限（pacer 式闭环）。
- **RC 与 marker 的数据竞争**：rc 字段 mutator 写/marker 读，marked 位
  marker 写/mutator 读（gc_phase_dec 路径）。最小方案是 marked 用原子位操作、
  rc 在窗口内用「快照+屏障」论证免原子——**这个论证是 B1 的核心正确性负担**，
  必须写成设计文档并逐路径审计（Go 的 mbarrier.go:68-93 内存序论证是模板）。
- **屏障覆盖审计**：所有「堆对象里的指针写」路径（list set/append、dict set、
  实例字段、cell 写、生成器帧槽、chan 缓冲）都要么插屏障要么证明免屏障
  （栈/标量/新对象）。P3-2 的「边界装箱审计表」方法论（p3-1b-landing.md:128-129）
  可直接复用为「屏障覆盖审计表」。
- **池/temp 栈/根表的线程安全**：marker 线程不分配（只扫），可以避开池；
  但根表（gc_globals/temp_stack/帧链）在 mutator 侧持续变化，快照协议要
  论证「扫描期间根变化不丢标记」（屏障+切片边界快照）。
- 收益上限：M=1 下 GC 与 mutator 并行只省「标记墙钟」，不省总 CPU；
  对 GC 占比低的负载（26 基准多数）收益≈0，对 GC 重负载（13_gc 类、
  大堆 churn）收益可观。**不是多核加速**。

### 路线 C：保留 M=1，加单后台并发 mark worker（stepping stone）

**形态（推荐起点）**：分两档——
- **C1（现状，已完成）**：分配点驱动的增量清扫 + 同步标记。这就是今天。
- **C2（本路线实体）**：起一个**独立 OS 线程**做 marker（不参与 vthread 调度，
  不跑用户代码），mutator 仍是单线程。GC 周期：切片边界 STW-lite（快照根+
  拍集合，现有 gc_collect 前半）→ 唤醒 marker 并发扫 → mutator 继续跑，
  容器写走屏障（接线 pa_gc_write_barrier，补 Yuasa 半边）→ 标记窗口内
  RC free 走 gc_pend（现成）→ marker 扫完通知 → 下个切片边界收尾
  （gc_incr_finish 现成）+ 增量清扫（现成）。
  **与 B1 的区别**：C2 把「并发」严格限制在标记扫描段，释放/清扫/收尾全在
  mutator 切片边界，正确性论证面最小；B1 允许窗口内任意释放路径，审计面更大。
  C2 做稳了自然长成 B1。

**收益**：
- 真并行吸收 major 标记停顿（~1.5ms → 接近 0 的 mutator 感知），GC 重负载
  可测收益；不动 P3、不动析构语义（窗口外）、不动 M=1 调度、不动 26 基准
  静态路径（屏障按 P3 式门控：无 unknown 动态写入的模块不发射）。
- 把「后台执行体 + 屏障接线 + 释放协议 + 根快照」四件套在最小风险面上
  先验证，为 B1/A 积累实测数据与审计表。
- 失败可回退：marker 线程关掉就是 C1，env 开关即可灰度。

**代价**：
- 仍然要付 B1 的核心正确性负担（rc/marked 竞争论证、屏障覆盖审计、
  根快照协议）——只是范围收窄到标记段。**没有免费的并发**。
- 一个 OS 线程的常驻成本（栈/调度噪声）；多租户服务器（HANDOFF.md:22 负载
  波动大）上 marker 与 mutator 抢核可能反而增加抖动——需要 abbench 实测。
- 标记窗口内析构延后的语义偏离同 B1（但窗口更短、更可控）。
- 对 26 基准（GC 占比低）收益≈0，**立项理由必须是 GC 重负载/大堆场景**，
  不能用热基准讲故事。

### 推荐

**C2 → B1 → （仅当多核硬目标）A**。理由：C2 用最小改动面验证四件套，
保留全部既有投资（P3/确定性析构/M=1/26 闸门）；B1 是 C2 的自然放宽；
A 是语义重构，单独立项。B2（隔离堆）与 Python 共享语义不兼容，不建议。

---

## 7. (f) 分阶段路径与闸门

沿用 P3 的落地纪律（独立 worktree/分支、每块增量独立验证、26 asm cmp、
不推 master）。验证设施本轮核实：tools/difftest.sh（-O0/-O2 自差分，
204 用例，docs/development.md:106）、tests/run_tests.sh（全量套件 normal+ASan，
HANDOFF.md:78-81）、tests/diff/run_diff.py（CPython 随机对拍，
docs/development.md:88-104）、tools/abbench.sh（交替 A/B+打乱+BURST，
HANDOFF.md:61-65）、bench/compare/run_compare.py（**四语言对比已含 Go**，
10 经典案×CPython/PyAdamas/Go/C++）、PYADAMAS_GC_STATS/RC_STATS 诊断
（rt_core.c:249-260）、ASan detect_leaks 抽查钉子（13_gc/88_defaultdict/
192_dict_cache/66_genexpr/119_gen_except_suspend，HANDOFF.md:79-80）。

### 阶段 0：研究（本文档，已完成）

产出：Go 源码落地 + 机制地图 + 张力清单 + 路线权衡。
**闸门**：lead review 本文档；用户回答 §8 开放问题（尤其「多核是不是硬目标」
「析构语义能不能降级」「P3 投资保不保」）。

### 阶段 1：设计（C2 优先）

产出 docs/gc-concurrent-mark-design.md：
- 所有权不变量清单（标记窗口内/外的释放协议，逐路径：RC dec/EH 展开/
  temp cleanup/生成器/VT 级联）；
- **屏障覆盖审计表**（每条堆指针写路径：插屏障/免屏障证明，方法论抄
  P3-2 边界审计，p3-1b-landing.md:128-129）；
- rc/marked 竞争论证（模板：mbarrier.go:68-93 内存序论证）；
- 根快照协议（切片边界=安全点；窗口内根变化由屏障兜底）；
- 门控方案（哪些模块发射屏障，26 基准按构造不进）；
- 测量计划（GC 重负载用例选型：13_gc/174_gc_construct_roots/大堆 churn/
  10k VT；abbench 口径）。
**闸门**：设计评审通过（lead）；审计表覆盖率 100%（每条路径有结论）；
门控方案证明 26 基准 asm 按构造不变。

### 阶段 2：隔离原型（独立 worktree，不碰主树）

- C 运行时原型（marker 线程 + 屏障接线 + gc_pend 协议），env 开关灰度
  （PYADAMAS_GC_CONCURRENT=0/1，默认 0=现状）。
- 每块增量独立构建验证（P3-1b 纪律，p3-1b-landing.md:8-18）。
**正确性闸门（每块都过）**：
- difftest 204/0（-O0/-O2 自差分）；
- 全量套件 normal+ASan 双跑退出码 0；ASan detect_leaks=1 抽查钉子全 0；
- **13_gc / 174_gc_construct_roots 钉住**（历史上 RC+增量标记撞墙的两个用例，
  rt_core.c:2513-2516/2315 注释原文）——并发标记原型必须过这两关；
- CPython 对拍（tests/diff/run_diff.py --seeds 20）零差异；
- 语料零差异（vs build/release oracle，HANDOFF.md:92-107 脚本）；
- 并发专项：marker 开/关输出逐字节一致；TSan（若工具链支持）或
  ASan+压力 churn（cache_race.sh 同款思路）；10k VT 挂起栈扫描压力
  （vthread-design.md:145-147 的 225 用例扩展）。
**性能闸门**：
- **26 热基准 asm 逐字节 cmp 26/26 IDENTICAL**（门控关时按构造；门控开时
  静态路径仍不变）；
- abbench 15 轮（GC 重负载案）：并发版不劣于同步版（目标：标记停顿下降
  可测，吞吐回归 <2% 噪声级）；
- 26 基准墙钟无回归（噪声级 ±2% 内）；
- PYADAMAS_GC_STATS：gc_incr_ms/标记墙钟/assist 步数前后对照，
  确认收益来源是「标记被后台吸收」而不是测量假象（HANDOFF.md:86-87
  「perf 占比≠边际成本」教训）。

### 阶段 3：集成（评审后）

- 默认仍关（env 灰度），跑全量语料+日常 corpus（docs/daily-corpus.md 口径）
  一段时间收集抖动数据；
- 多租户负载波动下 abbench 复核（marker 抢核风险，§6 C2 代价）；
- 达标后默认开 + CHANGELOG + 选择性 git add（lead review，不 push 除非授权）；
- 若 C2 稳定且收益实证，再立 B1 设计（放宽窗口内释放协议）；
  A 仅在用户确认多核硬目标+语义降级可接受后另行立项。
**闸门**：阶段 2 全部闸门在集成构建上复跑；默认开关切换前后语料零差异；
HANDOFF 更新。

---

## 8. 留给用户决策的开放问题

1. **目标到底是什么**：隐藏 GC 停顿（C2/B1 足够），还是多核并行加速
   （必须 A 或 M:N+隔离堆）？这决定整条路线。现有 26 基准是单核墙钟口径，
   若目标是多核，基准体系本身要先扩（bench/compare 已有 Go 对照可复用）。
2. **确定性析构是不是硬语义承诺**：CPython 风格及时释放（with/文件/__del__
   时机）能不能降级为「最终一致」？不能 → A 出局，B1/C2 的标记窗口也要
   设硬上限并文档化偏离。
3. **P3 投资保不保**：0.327/0.328 的 tag=提示+RC 探针模型是否必须存活？
   必须 → A 出局（或 A 里 unknown 全保守，性能回退）。
4. **热路径成本预算**：接不接受「unknown 容器写路径 +屏障开销」换并发？
   26 asm 闸门是继续硬守（门控豁免），还是重新谈判（允许门控模块 asm 变）？
5. **M>1 调度是否本期范围**：vthread M:N（work-stealing/抢占/可增长栈）
   是独立大山（vthread-design.md §9），与并发 GC 耦合但可解耦立项。
   先 C2（M=1+marker 线程）可以完全绕开它。
6. **网络并发（libcurl/netpoll）的排序**：用户已决定暂缓。若 C2 落地后
   转向网络阶段，netpoll 的 pollDesc 根登记与 GC 根快照协议有交集，
   设计时预留（§4）。
7. **B2 隔离堆的语言语义**：若未来考虑每 M 隔离堆，跨 VT 共享对象语义
   （现 chan 转移引用）要不要改成「传值/不可变」？这是语言设计决策，
   不是运行时决策。

---

## 附录 A：本轮核实的关键证据索引

| 论断 | 证据 |
|---|---|
| pyadamas 已有 Go 式 assist/pacer/增量清扫 | rt_core.c:84-104, 1146-1172, 2334-2366, 2676-2695；CHANGELOG.md:1551（「先读 Go GC guide 与 mgcmark.go/mwbbuf.go 再动手」） |
| 并发标记被 RC 硬卡（实测结论） | rt_core.c:2511-2523（13_gc UAF / 174 漏 21KB 注释原文） |
| 写屏障脚手架 dormant | rt_core.c:2324-2332；grep 全仓库 pa_gc_write_barrier 调用点=0；gc_incr_state=1 赋值点=0 |
| 零原子零锁 | grep runtime/*.c：_Atomic/__sync/pthread_mutex=0（rt_re.c 的 atomic 是正则原子组）；rt_core.c 138 顶层 static（79 静态变量） |
| P3 正确性压在 RC 探针上 | rt_internal.h:674-684；rt_any.c:434/:466；p3-tagged-union-design.md:99-100 |
| 26 asm 闸门纪律 | p3-1b-landing.md:10-18；CHANGELOG.md:162/:242 |
| Go 混合屏障/两次 STW/worker/pacer/分配漏斗 | §2-§3 全部行号引用（.ref_proj/go/src/runtime/） |
| Go 1.26 结构变化 | gcBgMarkWorker 在 mgc.go:1750（非旧版 mgcmark.go）；bulkBarrierPreWrite 在 mbitmap.go:388（非 mbarrier.go）；sizeclass 表在 internal/runtime/gc/sizeclasses.go:90；栈扫描含保守兜底 mgcmark.go:1076-1124 |
| 验证设施 | docs/development.md:88-129；HANDOFF.md:43-90；bench/compare/run_compare.py:1-47 |

## 附录 B：Go 源码落地清单

- 位置：`.ref_proj/go/src`（157MB，11483 文件，全量）+ `.ref_proj/go/VERSION`
  （go1.26.10, time 2026-10-19T22:20:36Z）+ `.ref_proj/go/README.md`（文件地图）。
- 来源：本地 `cp -a /usr/local/go/src`（GOROOT=/usr/local/go，go version
  go1.26.10 linux/amd64），未依赖网络；git 历史未 clone（可选后补，研究阶段不需要）。
- git 状态：`.ref_proj/` 整体被 `.git/info/exclude:9` ignore（check-ignore 核实），
  不污染版本库。本轮新增文件仅 `.ref_proj/go/*` 与本文档；现有 src/runtime/
  stdlib/tests 零改动（git status 核实）；未 commit、未 push、未动 master。
