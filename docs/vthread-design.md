# 虚拟线程（Go 式无色并发）—— 设计与先验原型

> 状态：**先验薄原型**（vthread 分支，基于 0.304 / 1e47057）。走通路径 + 标坑，
> 不追求完备。深坑（精确多栈 GC 的 CFI 化、可增长栈、M>1、抢占）记录绕行不做。
> 实现：runtime/rt_vthread.c（~900 行）+ src/lower_vthread.cpp + 前端薄切片。

## 1. 模型

**M=1 协作式有栈虚拟线程**：单 OS 线程上跑一个调度器 + 全部虚拟线程（vthread）。

- **无色**：`go f(args)` 可 spawn 任意普通函数，被调函数不需要任何标注
  （不是 async/await 那套传染颜色）。阻塞原语（channel send/recv、sleep、join）
  在运行时内部让出调度器，整条调用链无感——普通函数里调阻塞操作，它的调用者
  也不需要知道。这与现有 async/await/生成器（rt_gen.c 无栈状态机）**完全并存、
  互不改动**。
- **M=1 是绕过线程安全大山的关键**：rt_core.c 的 RC/GC/池/temp_stack 全是
  单线程全局态（119 个 static，零 atomic）。vthread 之间是协作切换而非并行，
  这些全局态**一行不用改**，零原子零锁。先验阶段不做 M>1。
- 参考 Go runtime / libco 的栈切换设计，**不参考** codon 的 OpenMP 数据并行。
  每个 vthread 一块 mmap 栈（默认 256KB + 底部 4KB PROT_NONE guard page，
  池化复用），切换 asm 保存/恢复 rsp + callee-saved + rip。生成代码本来就是
  无帧指针全 %rsp 相对帧 → 任意栈透明运行。

### 调度器形态（libco 式专用上下文）

主流程是第一个 vthread（root，用 OS 主栈）。另有一个**专用调度器上下文**
（sched，独立 mmap 栈，纯 C 循环）：

- 任何 vthread 阻塞（join/chan/sleep）→ 把自己挂进等待队列 → 切到 sched。
- sched 循环：run queue 取下一个 runnable → 切入；它阻塞/死亡时切回 sched。
- 所有 vthread 的「返回点」都是同一个 sched 上下文，**不存在 vthread 互相
  resume 的链**。第一版原型做过对称 inline 调度（每个阻塞者就地跑调度循环、
  被 resume 者记 ret 指针），实测形成循环 ret 链：ping-pong 双方互指对方的
  ctx，一方死亡后另一方的 ret 指向死栈，再切换就踩空（「dead vthread
  resumed」）。专用调度器上下文从结构上消灭这类环。
- run queue 是 PyAdamasList（借用指针；每个 vthread 有一份「调度器引用」
  从 spawn 持有到死亡，保活足够）。sleep 用 intrusive 定时器链，sched 空闲
  时 nanosleep 到最早 deadline 再批量唤醒。
- **协作式**：让出点只在语句边界的运行时调用。纯标量死循环饿死别人——
  文档写明，Go 的异步抢占是后续。

### 每 vthread 的栈水位全局

切换不只是寄存器。下列全局是「栈水位」语义，必须随 vthread save/load
（rt_vthread.c 的 vt_save_globals / vt_load_globals）：

| 全局 | 语义 |
|---|---|
| pa_frame_top | 帧记录链（GC 精确根 + 异常展开） |
| handler_top | setjmp handler 链 |
| pa_eh_depth | 零开销 try 保护区深度 |
| temp_stack / temp_len / temp_cap | 表达式临时值栈（**每 vthread 一份数组**） |
| exc_node_top / exc_node_depth | 异常上下文节点链（rt_exc.c 新增 save/load  accessor） |
| exc_save_top / current_exc / current_exc_inst / pending_exc_args / pending_exc | except 块水位 |

temp 栈必须每 vthread 独立：语句内阻塞时 temp_len 可以非零
（`print(ch.recv(), ch.recv())`），共享数组会互相覆盖。root 的 temp 数组
就是进程全局那一份，退出时所有权交接见 §5 坑 4。

## 2. 语法与语义（薄切片）

```python
h = go f(a, b)        # spawn 任意普通函数，返回 handle（vthread 类型）
join(h)               # 阻塞让出；vthread 内逃出的异常在此重抛
ch = make_chan(int, cap)   # cap 0 = 无缓冲握手；元素类型 int/float/str/unknown
ch.send(v)            # 满/无对端时阻塞让出
ch.recv()             # 空/无对端时阻塞让出
sleep(ms)             # 毫秒，阻塞让出（无 vthread 时退化为 nanosleep）
```

- **go 是软关键字**：只有 `go NAME…` 才解析为 spawn 表达式；`go = 1` /
  `go()` / `go[0]` 里的 go 仍是普通名字。go 可作表达式（`h = go f()`）
  也可作裸语句（丢弃 handle）。
- **被 spawn 函数的限制**（编译期明确报错）：用户定义、签名已知、非
  generator/coroutine/asyncgen、无 *args/**kwargs、无关键字实参、实参逐个
  显式给出（默认值省略暂不支持）。返回值丢弃（Go 语义）。
- **channel 语义**：无缓冲 = 直接手递手（sender 把值交给已在等待的 receiver，
  引用转移）；有缓冲 = FIFO 队列（缓冲用 PyAdamasList，GC/所有权语义现成）。
  等待队列 **FIFO 唤醒**（Go 语义；第一版头插是 LIFO，229 用例抓出）。
  满时 sender 阻塞、值寄存在 sender 的 VT 上；receiver 取缓冲值时把第一个
  等待 sender 的寄存值搬进缓冲并唤醒它。
- **元素类型**：int/bool/float 按裸机器字（float 走位模式，ChanSendF/RecvF
  用 i1f1 寄存器组）；str 按已知堆引用；unknown 按注册表探针（整数安全）。
  send 接管一份引用，recv 交出一份引用——与容器所有权纪律同构。
- **异常**：vthread 内逃出 → 存 handle，join 重抛（克隆后重抛，先克隆再释放
  等待引用，避免 dec 级联释放 exc 本体后读悬空）。vthread 自己 try/except
  接住的不传播。**未 join 的死亡 vthread 带异常 → 打印**
  （`pyadamas: uncaught <Type> in unjoined vthread <id>: <msg>`，stderr）。
- **退出语义（与 Go 不同，先验取确定性）**：main 返回时不杀剩余 vthread，
  而是 **drain**——把 runnable/sleeping 的 vthread 全部跑完，再放弃永久阻塞者
  （释放其持有状态、还栈），打印未 join 异常。方便 expect 钉输出。
  Go 的「main 返回即退出」语义列后续可选开关。
- **死锁**：所有 vthread 都阻塞且无定时器 → `panic: deadlock: all vthreads
  are blocked`（exit 1，Go 同款口径）。

## 3. 切换 asm 与栈

```
pa_vt_swap(from, to):           # SysV x86-64
  存 rsp+8 / rbx / rbp / r12-r15 / 返回地址 → from
  从 to 恢复同组寄存器 + rdi（新生 vthread 的第 0 实参 = VT*）
  jmp *to.rip
  .cfi_undefined %rip           # 禁止 traceback/展开跨栈乱走
```

- **sp 存 rsp+8（返回地址已弹出口径）**：恢复用 jmp 不是 ret，不弹栈。
  存原始 rsp 会让每次恢复后 rsp 低 8 字节，踩坏调用者帧（第一版实测
  stack smashing，坑 1）。
- 新生栈：mmap(256KB+4KB)，mprotect 底部 4KB PROT_NONE；栈顶 16 对齐，
  top-8 放假返回地址（underflow panic 桩），ctx.rip = 跳板。
  **栈深固定**：溢出直接 SIGSEGV 撞 guard page（无信号恢复，文档写明）。
- 栈池：freelist 复用，上限 256 块（≈64MB 虚拟留存），超出 munmap。
  **还栈必须在调度器栈上做**：池满时 vt_stack_retire 会 munmap，若在
  将死 vthread 自己的栈上还栈，swap 还没执行就踩空（10k 规模实测，坑 5）。
- 跳板：pa_vt_body_invoke（asm 外壳，带 CFI + catch-all LSDA）→
  pa_vt_invoke_inner（C，setjmp 兜底 + pa_call1 调包装闭包）→ 死亡收尾
  vt_finish（标 dead、唤醒 joiner、清帧链、切回 sched）。
- Windows：先验不支持（mmap/LSDA 都是 SysV 口径），Win64 构建给明确
  运行期 panic 桩。

## 4. GC 跨栈策略

**GC 只在 vthread 切片间跑**（单线程调度器保证；分配触发的同步 GC 落在
当前 vthread 的运行时调用里，切换点全局水位干净）。

- **运行栈**：走既有精确根扫描（全局根表 + temp 栈 + 帧链 scan_mask +
  setjmp 寄存器），一行不改。
- **挂起栈**：pa_vt_gc_scan（gc_collect 根扫描阶段调用，major 必扫、
  minor 与既有保守根同门禁）遍历 vt_all 链，对每个非当前、非死亡 VT：
  1. **帧链精确扫描**：vt->frame_top 是切换时保存的链头，帧记录物理上
     还在它自己的 mmap 栈上（栈未退役）。pa_gc_scan_frames_from
     （rt_core.c 导出，与 gc_scan_frames 同一份逻辑）按 scan_mask 扫
     临时区 + 逐槽标记。**这比任务原方案的「stack base..saved sp 逐字
     保守探针」更精确**——帧记录机制现成，挂起栈直接复用，不需要保守
     回退哲学。精确化（CFI unwind 找帧）因此也不再是后续必需。
  2. **temp 栈扫描**：每 VT 独立数组，逐字 pa_gc_mark_word。
  3. **callee-saved 寄存器 / fn 闭包 / 寄存的 chan 值**：存在 VT payload
     里，VT 是 RC OBJ，既有 OBJ 保守扫描逐字覆盖（不精确性：payload 里
     恰好等于存活堆地址的标量字会被误钉——与既有 gc_conservative 同款
     哲学，文档写明）。
- **VT 互钉**：vt_all 链经 payload 保守扫描传递标记，全部 VT 对 GC 可见；
  生命周期仍归 RC（调度器引用 + handle），不会泄漏。
- **channel 内容**：缓冲是 PyAdamasList（LIST 精确扫描）；等待 sender 的
  寄存值在 VT payload（保守扫描）；chan 本身是 RC OBJ。无新增扫描逻辑。
- **验收**：225 用例——只被挂起 vthread 持有的 list[str] + 自环 list，
  main 40000 轮混合表 churn 强制多次 major GC 后存活且内容正确；
  ASan detect_leaks=1 干净。C 夹具另有「temp 栈唯一引用过双 major GC」用例。
- exact-RC 模式（pa_disable_unknown_index）：minor 与既有口径一致跳过
  保守根（所有引用归槽/临时值/容器，rc>0 试探即活）；major 照常全扫。

## 5. 先验踩过的坑（全部已修，留给集成者）

1. **swap 存 rsp 而非 rsp+8**：jmp 恢复不弹返回地址，每次恢复 rsp 低 8
   字节 → 调用者 canary 踩坏。存「已弹出」口径。
2. **对称 inline 调度的循环 ret 链**：见 §1。专用 sched 上下文消灭环。
3. **唤醒后的幽灵 run queue 条目**：wake 方 rq_push 被唤醒者，被唤醒者
   自己的 vt_block 循环经 woken 检查直接返回时没把自己摘出队列 → 之后
   rq_pop 出「正在运行/阻塞的自己」→ 自切换。woken 返回路径统一
   rq_discard(self)；定时器到期唤醒自己也走同一纪律。
4. **root temp 数组双重释放**：root 的 temp_stack 就是进程全局那一份，
   vt_destroy(root) free 它，pa_rc_shutdown 再 free → double free。
   退出时以当前全局为准收进 root、清全局指针，RcShutdown free(NULL)。
5. **将死栈上 munmap 自己**：池满还栈即 munmap，vt_finish 还在自己栈上 →
   10k 并发死亡潮踩空。还栈推迟到 sched 循环（与调度器引用释放同处）。
6. **C 夹具 pa_gc_add_roots 口径**：根表登记的是「槽地址的数组」，
   不是槽地址本身（loop_ensure 同款）。夹具曾把 &g_chan 当数组传，
   存进表的是 g_chan 的值（NULL）→ GC 解引用 NULL。运行时本身无此 bug。
7. **ASan 纤维切换**：vthread 栈上的 longjmp/_Unwind 触发
   __asan_handle_no_return 警告污染 expect。切换包装加
   __sanitizer_start/finish_switch_fiber 注解（仅 __SANITIZE_ADDRESS__），
   新生栈入口 finish(NULL)。警告消失，毒化记账正确。

## 6. 前端薄切片

- ast：ExprKind::Go + GoExpr（call 子表达式）。parser 软关键字 lookahead。
- typecheck：TyKind::VThread / TyKind::Chan（sub[0]=元素类型），工厂
  mk_vthread/mk_chan，type_name/join/assignable 同族补齐；go 表达式校验
  被调函数形态；builtin join/sleep/make_chan（make_chan 第 0 参是类型名，
  编译期特殊形式，不按值求类型）；chan 方法表 send/recv。
- lowering（src/lower_vthread.cpp）：`go f(a,b)` 合成包装闭包
  `__pa_go_N`（build_lru_wrapper / build_partial_wrapper 同款形态）：
  env cell = [f, a, b]，体 = 读 cell 调 f、丢弃返回值（heap 结果 dec，
  unknown 结果 dec_unknown）。ClosureNew + cell  retain + VtSpawn（接管
  闭包引用，rc_temp_take）。**包装闭包是真实 pyadamas 函数**——ABI/帧记录/
  异常区域全由既有 codegen 处理，运行时跳板只需 pa_call1 单参调用。
- chan send：retain_elem 后 ChanSend（转移一份引用）；float 走 ChanSendF。
  recv：heap 结果 def_heap（temp 接管），unknown 结果 RcTempUnknown
  （探针入栈，整数安全），标量裸字。
- 入口退出：emit_return 的 is_entry 分支在 release_globals 前插 VtShutdown。
- 跨模块签名：VThread/Chan 暂按 unknown 序列化（与 Popen/datetime 同款口径）。

## 7. 实测（Debug 编译器 -O2 产物，Xeon Gold 6252 @ 2.10GHz，容器 cpuset）

| 指标 | 数值 | 口径 |
|---|---|---|
| 无缓冲握手往返 | **~150–300 ns/round**（负载敏感） | 2 vthread 乒乓 100k 轮，每轮 2 次切换 + 2 次 chan 操作；空闲测得 152/155，负载下 291/399 |
| spawn+join | **~1.0 µs/对** | 空函数 20k 次，栈池复用路径（含闭包/cell 分配 + 2 次切换 + RC 收尾） |
| 缓冲 channel 吞吐 | **~69 ns/item** | cap 64，300k int 生产/消费 |
| 10k 并发阻塞 vthread | **RSS ≈ 5.0 KB/个**（合计 ~49MB） | 浅 waiter（recv 阻塞），含 VT 结构 + 闭包/cell + 触达栈页 |
| 10k 并发虚拟内存 | **VSZ ≈ 2.5 GB** | 10k × (256KB 栈 + 4KB guard)，mmap 惰性触达 |
| 10k 唤醒收尾后 RSS | ~14 MB | 栈还池/munmap，池留存 256 块虚拟 |

量级对照：Go goroutine spawn ~数百 ns、channel 操作 ~百 ns 级——同量级。
切换 asm 本身 ~20-50ns（7 存 + 8 载 + jmp），其余是调度器 C 路径 + 全局
水位 save/load + chan 所有权操作。先验未做 perf 剖析优化。

**内存账（10k 并发）**：每 vthread = VT 结构 ~330B + 包装闭包/cell ~200B
+ 触达栈页（浅帧 1-2 页 4-8KB）+ 栈池节点。虚拟 260KB/个是 mmap 预留，
RSS 只算触达页。深递归 vthread 按实际触达页增长，上限 256KB（guard page
硬撞）。PYADAMAS_VT_STACK_KB 可调（16..65536）。

## 8. 限制（先验，均有明确口径）

- **协作饥饿**：纯标量死循环不让出，饿死全部其他 vthread。无异步抢占。
- **M=1 无多核**：单 OS 线程，并发不并行。CPU 密集负载无加速。
- **固定栈深**：256KB 默认，溢出 SIGSEGV（guard page），无信号恢复/增长。
- **无 select / 无 close / 无缓冲队列 O(1)**：缓冲 FIFO 用 list take_at(0)
  是 O(n)（先验规模够用）；chan 丢弃时阻塞者永久等待（退出按放弃处理）。
- **go 形态限制**：见 §2（无默认值省略/kwargs/varargs/generator/方法）。
- **异常打印时机**：未 join 异常在 handle 释放或退出 drain 时打印（不是
  死亡即打印）；join 后的异常克隆重抛，原异常随 VT 释放。
- **RC 析构级联里 spawn/join**：调度器栈上的 dec 级联若跑用户代码且用户
  代码再阻塞，会重入调度器——先验不支持，文档写明（与 asyncio 同款边界）。
- **Windows 不支持**；**跨模块 handle/chan 签名按 unknown**。
- **不精确保守钉住**：VT payload 保守扫描可能误钉恰好等于堆地址的标量字
  （与既有 OBJ 保守扫描同款哲学）；挂起栈帧链扫描本身是精确的。
- **GC 统计口径**：PYADAMAS_GC_STATS 的分裂根统计不含挂起栈部分（诊断
  计数偏小，不影响正确性）。

## 9. 后续（记录绕行，不在先验做）

1. **M>1 隔离堆**：每 M 独立 RC/GC/池（或共享堆 + 原子化 119 个 static），
   work-stealing 调度。线程安全大山，先验刻意绕过。
2. **prange 数据并行**：与 vthread 正交（OpenMP 式），另行立项。
3. **select / chan close / 超时 recv**：等待队列已 intrusive 化，加多路
   等待需要「一个 VT 挂多个队列」的摘除纪律。
4. **异步抢占**：信号 + 安全点（语句边界已有水位纪律，但寄存器里的堆指针
   需要 spill 协议），Go 的 async preemption 同款深坑。
5. **可增长栈**：分段栈 / 拷贝栈（morestack），需要精确栈图或 CFI 全量化。
6. **async 改基**：现有 asyncio 事件循环可改基到 vthread 调度器（阻塞原语
   统一让出），消除 async 颜色——先验证明无色路径可行后再议。
7. **精确多栈 GC 的 CFI 化**：本先验用帧链已精确，CFI unwind 挂起栈找帧
   记录不再是必需；若未来去掉帧记录（性能），再走 CFI 路线。
8. **退出语义开关**：Go 式 main 返回即杀 vs 当前确定性 drain。

## 10. 集成建议与风险

- **合入路径**：runtime 新增 rt_vthread.c 已进 PYADAMAS_RT_PARTS（rt.o /
  rt_asan.o 自动合成）；前端改动全是加法（新 ExprKind/TyKind/RtFn 追加在
  枚举尾，存量值不位移），既有语料零差异预期成立（difftest 验证）。
- **零开销异常共存**：跳板 catch-all LSDA 与既有 personality/方案 G 帧清理
  兼容（224 用例 O0/O2/ASan 验证）；personality 把 typed catch 一律按
  catch-all 处理的既有口径对跳板无影响。
- **风险点**：① 调度器栈上的 dec 级联重入（§8）；② 深栈 vthread 的 GC
  扫描成本随帧数线性（10k 深栈 major GC 未测）；③ temp 栈 save/load 每
  切换 3 次全局写，热路径切换密集时有优化空间（先验未剖析）；④ 栈池
  256 块上限外的 munmap/mmap churn（大规模死亡潮可测）；⑤ Debug 编译器
  既有 118/154 崩溃（0.304 CHANGELOG 已记录，与本轮无关）。
- **性能中性预期**：未用 go/chan 的模块不发射任何新运行时调用；rt_vthread.o
  并入 rt.o 的尺寸效应需 abbench 复核（先验未跑，集成时按 0.304 纪律补）。

## 11. 验证清单（本轮）

- 220–229 十个端到端用例：O0/O2 输出逐字一致（expect 段钉输出）。
- ASan + detect_leaks=1：220–229 全干净（O2）。
- C 夹具（/tmp/vt_harness.c，不经过前端）：spawn/join、无缓冲、ping-pong、
  sleep 序、join 异常、挂起栈 temp 唯一引用过双 major GC、1000 churn、
  嵌套、drain、死锁 panic——全过 + ASan 干净。
- 全量套件 normal + ASan、difftest O0/O2、规模检查：见 CHANGELOG 0.305 条。
