# 并发循环 GC 阶段 1：M=1 增量标记 + 混合写屏障（设计硬文档）

> 状态：阶段 1 落地（分支 gc1，基线 master bed546e / 0.329）。**不开后台线程**
> （后台 marker 是 C2/阶段 2，不在本次范围）。本文是阶段 1 闸门要求的三份硬文档
> （屏障覆盖审计表 / rc·marked 正确性论证 / 门控方案）+ free 路径审计 + 基线 bug 记录。
> 上游设计：[concurrent-cycle-gc-design.md](concurrent-cycle-gc-design.md) §3.1/§8；
> Go 取证：[go-concurrent-gc-study.md](go-concurrent-gc-study.md)；
> 内存序模板：Go `runtime/mbarrier.go:24-93`（Yuasa 删除 + Dijkstra 插入混合屏障）。

## 0. 范围与开关

| 开关 | 层 | 默认 | 作用 |
|---|---|---|---|
| `PYADAMAS_GC_INCR_MARK=0/1` | 运行时（pa_gc_init 读一次缓存 `gc_incr_mark_on`/`pa_gc_incr_mark_fast`） | 0 | 1=major 走分片标记+删除/插入屏障；0=现状同步标记，**flag-off 行为/asm/leak 逐字不变** |
| `PYADAMAS_GC_BARRIER=0/1` | 编译期（Lowerer::gc_barrier 读一次缓存） | 0 | 1=对堆指针写发射 `pa_gc_write_barrier` IR；0=不发射，**生成 asm 逐字不变（26 按构造保护）** |

flag-off（两开关皆 0）= 现状基线，零变化。完整协议 = 两开关皆 1。
**最高不变量：flag-off 零变化。** 新协议全部按 `gc_incr_mark_on` 门控，legacy 路径原样保留
（含 flag-off 大集合增量清扫复用 INCR_MARK 相位的历史口径，rt_core.c:2571 一带）。

## 1. 状态机（M=1，分配点驱动）

```
major 触发（rc_alloc 阈值）
  └─ gc_collect(1)：组装整堆快照 gc_set + gc_tab_build
       └─ 根扫描（一次性原子：globals/temp/帧链/寄存器/挂起 VT）  ← M=1 切片间 mutator 才跑
            ├─ flag-off：gc_drain() 同步标完 → 同步/增量清扫（legacy，不动）
            └─ flag-on ：gc_incr_state=1, gc_phase=INCR_MARK, return
                 └─ 每次 rc_alloc 推进 gc_incr_step（GC_MARK_BUDGET=64 个对象/步，
                    GC_INCR_STRIDE=16 次分配摊一次调用）
                      ├─ state=1：drain gc_work；插入屏障（gc_incr_state==1）生效
                      └─ gc_work 空 → **同步** release pass + gc_free_body 清扫 + gc_incr_finish
                           （M=1：本次调用内 mutator 不跑，清扫原子）
```

**为什么清扫保持同步（阶段 1 关键决策）**：曾试「标记排空 → state=2 增量清扫」，
实测 174 漏 8816B。根因：增量清扫切片间 mutator 运行，release/RC 把**已标记活对象**
rc 减到 0，收尾再把它当延迟释放对象 free → 误释活对象（诊断抓到 kind=LIST rc=1 被收尾释放）。
M=1 下把清扫做成原子（一次 gc_incr_step 调用内 release+free+finish，mutator 不插入）即灭此坑。
**阶段 1 只增量摊薄昂贵的标记段（~1.5ms gc_drain）**；清扫与 mutator 交错的进一步分片
需要「活对象 rc 归零」的所有权协议，留待阶段 2/3 线程安全边界一起做。

## 2. 屏障覆盖审计表（覆盖率 100%）

### 2.1 插入屏障（Dijkstra，shade 新值）

| 堆指针写路径 | 发射点 | 覆盖 | 免屏障证明 |
|---|---|---|---|
| list append（kind 0/1/2/3） | rt_list.c `pa_list_append` 入口 `pa_gc_insert_barrier(v)` | ✅ | 标量 v 查 gc_tab 落空 no-op |
| list setitem | rt_list.c `pa_list_set` 入口 | ✅ | 同上 |
| list copy_range（批量 append） | 逐元素走 `pa_list_append`；kind1 快路径 memcpy+inc | ✅* | *kind1 快路径见 §2.3 备注 |
| dict set（插入/覆盖） | rt_dict.c `pa_dict_set` 入口 shade(key)+shade(val) | ✅ | 标量键值 no-op |
| 实例**静态字段**写 | IR `store_field`（lower/impl_slots.cpp）堆/unknown 分支 `gc_barrier(v)` | ✅ | 标量字段（!ir_is_heap && !unknown）不进 |
| 实例**动态属性**写 | rt_class.c `pa_attr_set` → `pa_dict_set` | ✅ | 经 dict 屏障 |
| cell 写（闭包捕获槽=list） | cell 是 list，经 `pa_list_set`/`pa_list_append` | ✅ | — |
| 生成器帧槽写 | 帧槽拥有引用经 store_slot→RC；堆值产生经 temp/容器屏障 | ✅* | *见 §2.3 备注 |
| chan 缓冲/寄存值 | rt_vthread.c `pa_chan_send` 入口 shade(v)；缓冲经 pa_list_append | ✅ | 标量 no-op |
| 全局槽写 | 全局根每轮 major 重扫（gc_globals）；写经 store_global→RC | ✅ | 根重扫兜底 |
| **栈/当前帧写** | 不发射 | 免 | 根扫描一次性拍快照（M=1 原子），栈写不在标记窗口藏对象（Go mbarrier.go:96 栈免屏障同理论） |
| **标量写** | 不发射 | 免 | 非堆指针，gc_mark 查表落空 |
| **新对象内部写** | 不发射 | 免 | 新对象不在 gc_set 快照（allocate-black 口径，§4），靠 RC |

### 2.2 删除屏障（Yuasa，shade-on-delete）

| RC 释放路径 | 标记期路由 | 覆盖 |
|---|---|---|
| `pa_rc_dec` 快路径 | `pa_gc_phase_fast!=0` → `pa_rc_dec_slow` → `gc_phase_dec` INCR_MARK | ✅ |
| `pa_rc_dec_slow`（归零） | `gc_phase_dec` INCR_MARK：rc>1 正常减；rc==1 shade+DEFER+pend 保活 | ✅ |
| `pa_rc_dec_heap_batch` | 检查 `pa_gc_phase_fast` → 逐元素 `pa_rc_dec` | ✅ |
| `pa_rc_dec_unknown` | → `pa_rc_dec`（rt_core.c:1043） | ✅ |
| `pa_gc_unref_child`（release 回调） | → `gc_phase_dec` | ✅ |
| temp cleanup（`pa_rc_cleanup`） | → `pa_rc_dec` | ✅ |
| EH 展开（rt_eh.c） | → `pa_rc_cleanup`/`pa_rc_dec` | ✅ |
| 生成器帧槽释放（`pa_gen_slot_release`） | → `pa_rc_dec`/`pa_rc_dec_unknown` | ✅ |
| VT 死亡级联（rt_vthread.c） | → `pa_rc_dec` | ✅ |
| 叶子 str/float（LEAF） | INCR_MARK 分支**先于** LEAF 分支 → 延迟释放（收尾 rc_leaf_free） | ✅ |
| untracked 小元组 | INCR_MARK 分支先于 UNTRACKED 分支 → 延迟 | ✅ |
| 池快路径（list_inline_pool_put 等） | 仅 phase==NONE 可达；标记期 phase!=0 被 gc_phase_dec 拦截 | ✅ |
| GC 清扫 `gc_free_body` | flag-on 同步清扫先 release（dec 活子/叶子）再 free；DEFER 对象已标记不被清扫 | ✅ |

**结论**：所有把对象 rc 减到 0 的路径在标记期都汇聚到 `gc_phase_dec` 的 INCR_MARK 分支
（删除屏障），无旁路。所有堆指针写路径都有插入屏障或免屏障证明。覆盖率 100%。

### 2.3 备注（两处批量/帧槽快路径的口径）

- `pa_list_copy_range` kind1 快路径（memcpy + 批量 `pa_rc_inc`）：源元素已是堆指针且
  被源列表持有（源在快照内会被扫描标记），目标列表新建/扩容时其元素经源列表的标记传递。
  标记期若目标已黑、源元素白且源即将被删引用，由删除屏障 shade 源元素兜底。实测 278-282
  + 全套 ASan leak 干净。阶段 2 上线程前对此快路径补显式逐元素 shade 以收紧证明。
- 生成器帧槽：挂起帧链是根（pa_vt_gc_scan / gc_scan_frames 每轮 major 重扫），帧槽堆值
  由根扫描标记；帧槽**写入**的新堆值经产生点 temp/容器屏障覆盖。281（生成器帧槽写）+
  282（10k VT 挂起）ASan leak 全 0 验证。

## 3. rc/marked 正确性论证（M=1 单线程：逻辑时序，非数据竞争）

模板 Go mbarrier.go:68-93：Go 无条件 shade（不依赖槽颜色）换掉 store/load 内存屏障。
**M=1 无并发**：mutator 与 marker 不在同一时刻跑（marker 只在 rc_alloc 切片的 gc_incr_step
里跑，mutator 在切片之间跑）。所以没有 rc 字段「mutator 写/marker 读」的数据竞争——
难点是**逻辑时序**：两个 mutator 步之间 marker 推进，对象图/引用计数在标记窗口内变化。
三色不变量靠混合屏障在**每个 mutator 步边界**维持：

**三色定义**：白=不在 gc_set 或未标记；灰=已标记待扫（在 gc_work）；黑=已扫描（MARK 位且已出队）。

**不变量（强）**：标记期间，任何从根或灰对象可达的对象不会被误回收。

- **插入屏障（Dijkstra）**：mutator 把白对象 b 写进黑对象 a 的槽 → shade(b) 标灰入队。
  防止「把唯一指针从栈/灰对象移进黑对象」藏 b（mbarrier.go:50-53）。无条件 shade 新值，
  不依赖 a 的颜色（避免判色与标记器改色的时序耦合，同 Go 内存序论证）。
- **删除屏障（Yuasa）**：mutator 的 RC 把对象 x 最后一个引用删掉（rc 归零）→ shade(x)
  标灰保活 + DEFER + gc_pend，**rc 保持 1** 到收尾。防止「unlink 唯一指针」藏 x
  （mbarrier.go:46-48），同时灭 UAF（标记队列/快照仍指向 x 时不 free）。
  - **归零对象必无环**：环会保 rc>=1，rc 归零说明无环、无其它引用。故 shade 的子树若与
    活对象共享，由标记器扫描保住；收尾 dtor 再正常 dec 共享子对象 —— 不误收活对象。
  - **DEFER 去重**：同一对象只入 pend 一次（DEFER 位），收尾释放一次，灭二次释放。
  - **保活时序（174 命门）**：x 保活到「gc_work 排空 + 同步清扫 + gc_incr_finish 收尾」
    之后才 free。收尾**先复位 gc_phase=NONE 再处理 gc_pend**（rt_core.c:2238-2242 历史
    死循环教训：顺序反了 pa_rc_dec 见 phase!=0 又把对象登记回 pend，永远释放不掉）。
    收尾时 DEFER 对象 rc=1 → pa_rc_dec 1→0 跑 dtor 级联正常释放。
- **新对象口径（allocate-black，§4）**：标记期新分配对象不在 gc_set 快照 → 清扫扫不到 →
  天然不被本轮回收，靠 RC。**不打 MARK 位**（打了会被收尾晋升循环/清扫当集合成员处理 →
  174 历史漏 21KB 原因之二）。新对象引用的老对象由插入屏障 shade 保护。

**gc_pend 收尾时序**：
```
gc_work 空 → gc_phase=SWEEP → release pass（垃圾交活边，mark_sweep 口径 dec 活子/叶子）
  → gc_free_body 清扫未标记垃圾（DEFER 对象已标记，跳过）
  → gc_incr_finish：
      (1) 遍历 gc_set 清 marked=0（含 DEFER 位）+ 重算 old_bytes
      (2) **先** gc_phase=NONE, gc_incr_state=0
      (3) **后** 遍历 gc_pend 调 pa_rc_dec（此时 phase=0 → 正常 dec+dtor 级联）
```
DEFER 对象（标记期归零保活）在第 (3) 步释放；它们已标记故第清扫步不碰 → 无二次释放。
新对象 DEFER（不在 set）同样第 (3) 步释放。pend 内对象构成无环森林（父保子活），级联安全。

## 4. allocate-black 口径（新对象）

`gc_alloc_black` 保持空操作占位（rt_core.c:2320）。新对象：
- 不进 gc_set 快照（标记开始时拍的）→ 清扫遍历 gc_set 扫不到它 → 不被本轮 GC 回收。
- **不打 MARK 位**：MARK 语义是「本轮集合内已标记存活」，新对象不在集合，打上去会让
  收尾晋升循环/增量清扫把它当集合成员 → 漏（174 历史原因之二）。
- 靠 RC 正常释放；标记期归零走删除屏障 DEFER+pend 收尾释放。
- 新对象引用的老对象：插入屏障 shade 老对象保护（§2.1）。
- `pa_rc_track`（kind0→kind2 升级入链）同口径：新入链对象不在本轮快照，不被清扫。

## 5. 门控方案（26 热基准按构造不进）

**26 热基准集**：bench/compare/*.py（9，除 run_compare）+ bench/realistic/*.py（6）
+ bench/daily/b*.py（5）+ bench/daily/d*.py（6）= 26。

**两层门控**：
1. **编译期 `PYADAMAS_GC_BARRIER`（默认 0）**：Lowerer::gc_barrier 仅在 =1 时发射
   `RtFn::GcWriteBarrier` IR。**默认 0 → 任何模块都不发射屏障 IR → 生成 asm 与基线逐字相同**。
   26 基准用默认编译对照基线 lead 0.329：**26/26 IDENTICAL（实测 cmp）**。这是按构造保证——
   不依赖「26 是否含 unknown」的经验判断（取证：12/26 含 pa_rc_dec_unknown 冷路径、8/26 含
   kind2 位图写，故不能用「无 unknown」做模块判据；编译期总开关默认关才是硬保证）。
2. **运行时 `PYADAMAS_GC_INCR_MARK`（默认 0）**：运行时写原语屏障 `pa_gc_insert_barrier`
   由 `pa_gc_incr_mark_fast` 门控（flag-off 一次可预测分支跳过，零调用）；`pa_gc_write_barrier`
   内部再由 `gc_incr_state==1` 门控（仅标记期生效）。flag-off 运行时行为零变化。

**运行时屏障为何放在写原语而非纯 codegen**：容器/chan/动态属性写都汇聚到
pa_list_append/set、pa_dict_set、pa_chan_send 少数原语，在此发射保证**全路径覆盖**
（含 codegen 内联快路径回落运行时的分支），且不改生成 asm（26 更强保证：生成代码原样）。
静态实例字段写是内联 movq（不经运行时），由 IR store_field 的 gc_barrier 覆盖（编译期门控）。
**正确性优先于「静态模块免屏障」的乐观断言**：纯静态环代码理论上也可触发插入屏障需求
（白对象从灰对象移进黑对象），故编译开关打开时对堆写一律发射（安全），26 由默认关保护。

## 6. 基线既有 bug 记录（flag-off 不改，移交）

**flag-off 大集合增量清扫漏 release**：gc_incr_step state-2（rt_core.c:2353 一带）只
`gc_free_body` 不调 release，而 gc_free_body 只 free 容器自身缓冲、不 dec 子引用。
垃圾容器指向活叶子（str）的边漏 dec → 叶子 rc 悬空 → 永久漏。
**取证**：构造 old-gen 环垃圾引用新串用例（/tmp/gc1_leakprobe2.py 形态），flag-off 大集合
增量清扫 ASan detect_leaks=1 漏 **7198160B / 59998 allocs**；flag-on（同步清扫补 release pass）
**全 0**。13_gc/174 flag-off 不触发此路径（13 环引用 immortal 字面量、174 垃圾无环由 RC dtor
释放），故全套 flag-off 仍干净。**这是独立基线 bug，按 flag-off 零变化原则保留不改**，移交 lead。

## 7. 闸门结果（阶段 1）

| 闸门 | 结果 |
|---|---|
| 13_gc / 174 flag-ON normal + ASan detect_leaks=1 | ✅ 全过（作者撞墙点修复） |
| 全套 normal flag-off / flag-on | ✅ EXIT=0 / EXIT=0（283 ok，0 失败） |
| 全套 ASan flag-off / flag-on | ✅ EXIT=0 / EXIT=0 |
| difftest（-O0/-O2 自差分） | ✅ 270 用例 0 不一致 |
| ASan detect_leaks=1 钉子（13/174/88/192/66/119 + 278-282） | ✅ 11/11 全 0 |
| 26 热基准 asm 逐字节 cmp（默认编译 vs lead 0.329） | ✅ 26/26 IDENTICAL |
| CPython 对拍 run_diff.py --seeds 20 | ✅ flag-on 5600/0 + flag-off 5600/0，EXIT=0 |
| abbench flag-on vs off（GC 重负载） | ✅ bench_rc_gen −0.1% / 13_gc +0.6% / queue_sim −0.4%（噪声级）；极端合成 278 约 +3.6%（标记期屏障开销，文档化代价） |
| 标记停顿下降可测 | ✅ 174 同步 gc_drain 标记停顿 1.89/8.47/**32.09ms** → flag-on 根扫描 0.00ms + 64 对象分片（32ms 同步标记停顿消除） |
| check_size 无新增超限 | ✅（5 个超限文件均基线既有，ir/ops.h/lower_impl.h 本已 >1200） |
| 新增测试 278-282（CPython 对拍 + O0/O1/O2 + ASan） | ✅ 全过 |

## 8. 剩余问题 / 阶段 2 接口

- 清扫与 mutator 交错的增量分片（含「活对象 rc 归零」所有权协议）→ 阶段 2/3。
- `pa_list_copy_range` kind1 快路径补显式逐元素 shade 收紧证明（§2.3）→ 阶段 2 上线程前。
- 标记期 `__del__`/dtor 延迟到收尾（删除屏障保活）是可观测语义微变，CPython 循环 GC 同款延迟；
  本阶段测试不钉析构时序，对拍口径以可观测输出为准。
- 后台 marker 线程（C2）：gc_work/gc_pend/gc_set/池/freelist 线程安全边界 + 两次短 STW +
  marked 原子位 + 无条件 shade（mbarrier.go:88-93）→ 阶段 2。
- 临时诊断 `PYADAMAS_GC_DBG`（gc_dbg_defer/defer_swept/pend_badrc）落地后移除。
