# 被跳过帧的 RC 槽释放 —— 表驱动展开下的设计（Z2）

状态：设计稿（不写代码）。范围：Z2-b 把 LVSA 接进 codegen 时，**异常穿过普通函数帧**（非可挂起函数）
时，被跳过的中间帧里那些 owned RC 槽怎么释放。
前置事实全部来自当前工作区（setjmp 路径 + Z1/Z1.5/Z2-a 已落地），证据统一用 `file:line`。
实测探针都放在 `/tmp`，未改仓库任何代码；复现方法见 §11。

## 0. 一页结论

1. **lead 提出的捷径（帧记录加 `cfa` → personality phase 1 记 `_Unwind_GetCFA` → landing pad 里
   调 `pa_frame_unwind_to_cfa`）被推翻。** 它的两个前提里，第一个成立、第二个不成立：
   - CFA 在 phase 1 与 phase 2 对同一帧**完全相同**，`_Unwind_GetCFA` 在 phase 1 就能用（§3.1 实测）；
     libgcc 自己的 `_UA_HANDLER_FRAME` 判定就依赖这件事。
   - 但 landing pad 执行时，被跳过帧的栈内存已经"退役"：LP 入口 rsp = 抛出点 call 前的 rsp，
     **LP 的第一条 `call` 就把最近被跳过帧的帧记录覆盖掉**（§3.2 实测：5/5 个字全被覆盖）。
     在这里回头走帧链、解引用槽地址 = 读已被 helper prologue 改写的栈。
2. **推荐 Z2-b 用方案 G（handler 帧 personality 里批量释放）**：在 phase 2 带 `_UA_HANDLER_FRAME`
   的那次 personality 调用里、安装 landing pad **之前**（此时物理栈还完整、且任何 LR 代码都还没跑），
   用 `_Unwind_GetCFA(ctx)` 作目标调 `pa_frame_unwind_to_cfa(target_cfa)`。语义与现有
   `pa_frame_unwind(handler_top->frame)` 逐字等价（§5.1 有等价性证明），代价只是帧记录多一个
   `cfa` 字 + 压帧时 2 条指令；**不需要**给每个有槽的函数发 cleanup range / landing pad。
3. 每帧 cleanup landing pad（`docs/exc-semantics.md` §5.1 推荐的 B）**正确但最贵**：每个有槽函数
   +~30 字节（pad 文本 + cleanup LSDA + FDE 的 LSDA 指针），抛出路径每跳一帧多一次
   `_Unwind_Resume` 重启。Z4 做 `finally` 时反正要有 cleanup pad，届时可以复用；但**不要**用它只做
   "纯释放"。
4. **"Z2 先不清理中间帧"不是可接受的降级**：探针实测每异常泄漏 **0.63–0.65 KB**（3 层帧、4 个 owned 槽），
   10 万次 ≈ 64 MB、无界；minor GC 完全兜不住（泄漏的 rc>0 被当外部根，实测 minor freed=0）；
   major GC 只能回收 GC 链上的对象且要等 8MB 阈值；exact-RC 模式下 str/range/struct/无字段实例
   **根本不进 GC 链**（`runtime/rt_core.c:341-353`、`:499-505`），连 GC 触发量都不计
   （`:559-563`）——连"延迟回收"都没有。
5. **Z2 验收必须新增"异常路径不涨 RSS"**，且要能在 GC 关闭 + exact-RC 模式下跑。
   **前置阻塞**：当前基线把 `tests/run/92_exc_inline_rc.py` 的 2000 次循环改成 100000 次会 SIGSEGV
   （`pa_exc_accept_value → pa_rc_inc`），这条验收用例现在跑不出来，得先定位（§10 备注）。

## 1. 现状：今天是怎么做的（file:line）

| 部件 | 证据 | 职责 |
|---|---|---|
| 帧记录结构 | `runtime/rt_internal.h:327-334` | `{prev, n, scan_base, scan_words, scan_mask, slots[]}`；槽地址低两位标记"可能非堆 / 只给 GC 扫" |
| handler 记录 | `runtime/rt_internal.h:336-343` | `{jmp_buf env; prev; mark; frame}`；`frame` = 压 handler 时的 `pa_frame_top` |
| 压帧 | `src/codegen_block.cpp:356-391` | 非生成器函数序言里内联写记录、链进 `pa_frame_top`；记录在局部区**顶部**（`src/codegen_impl.h` 的 `rec_off()` 与布局注释（写作时 :473-493）） |
| 弹帧 | `src/codegen_regalloc.cpp:36-39` | 正常返回时只做 `pa_frame_top = rec->prev`，槽由各语句/函数出口的 `rc_cleanup` 释放 |
| 逐帧释放 | `runtime/rt_exc.c:20-38` | `pa_frame_unwind(target)`：从栈顶往下逐帧"槽地址→非空→清零→`pa_rc_dec`/`pa_rc_dec_unknown`"，最后 `pa_frame_top = target` |
| raise | `runtime/rt_exc.c:204-214` | `exc_longjmp`：先 `pa_frame_unwind(handler_top->frame)`，再 `_longjmp` |
| 压 handler | `runtime/rt_exc.c:283-297` | `h->mark = pa_rc_mark()`、`h->frame = pa_frame_top` |
| 弹 handler | `runtime/rt_exc.c:300-306` | `pa_rc_cleanup(h->mark)` 清受保护区临时值栈 |
| 表驱动 personality | `runtime/rt_personality.c:488-659` | 只做 handler/cleanup 判定与上下文安装；不碰 `pa_frame_top` |
| Z2-a 发射器 | `src/eh_emit.h:1-95`、`src/eh_emit.cpp:165-270` | LSDA/CFI 文本发射器（未接 codegen；git dd6c8d8） |

关键点：**现在的释放发生在 longjmp 之前**，此时所有被跳过帧的栈都还完整；`pa_frame_unwind` 可以在
真实栈上走链、解引用槽。表驱动把它换成了"逐帧逻辑展开"：unwinder 只调 personality / landing pad，
不会有人替我们碰 `pa_frame_top`——释放时机没了。

## 2. 问题定义：三个硬约束

表驱动下要让"被跳过帧的槽"安全释放，必须同时满足：

- **C1（内存有效）**：读某个帧的 `PyAdamasFrame`/槽时，该帧的栈内存还没被复用。逻辑展开之后
  这些帧的栈是"退役"的；C 栈指针已经回到外层帧，任何函数调用都会向下占用它们的内存。
- **C2（不自我覆盖）**：释放过程中 `pa_rc_dec` 会跑析构器（生成代码里的字段 dec / GC release
  跳板，`src/codegen.cpp:253-353`），析构器继续用栈；如果此时正在遍历"在死栈里的槽地址数组"，
  遍历会被自己的析构器踩坏。
- **C3（GC 根链一致）**：`pa_frame_top` 同时也是 GC 影子根链表（`runtime/rt_core.c:980-1004`）。
  任何**会分配**的代码（用户函数、`pa_rc_dec` 里的 free 不分配，但正常代码会）运行之前，
  链上不能还挂着已死帧，否则 GC 会把死栈内容当根扫（误标/崩溃）。

由此只有两类位置可行：

- **逐帧经过时释放**：personality 在 phase 2 走过某帧时，该帧仍是"当前帧"，物理栈完整（C1、C2 满足）；
  每释放一帧就把它从链上摘掉（C3 立刻满足）。→ 方案 B / C。
- **在 phase 2 的 handler 帧人格调用里一次性释放**：这是整条展开链上**最后一个**"物理栈仍完整、
  且尚未执行任何 landing pad 代码"的时刻；在此把"比 handler 帧更深的记录"一次性清掉，
  之后 LP 拿到的 `pa_frame_top` 就和老世界 longjmp 后一模一样。→ 方案 G。

**LP 里回头释放一定违反 C1/C2**（见 §3.2），这是捷径失败的根因。

## 3. 捷径验证：CFA 能拿到，但位置错了

lead 的提案分三步：(a) `PyAdamasFrame` 加 `cfa`；(b) personality phase 1 找到 handler 时把
`_Unwind_GetCFA(ctx)` 记下来；(c) landing pad 调 `pa_eh_landing()` → `pa_frame_unwind_to_cfa(cfa)`。
逐步验证：

### 3.1 前提：phase 1 / phase 2 的 CFA 是否一致？`_Unwind_GetCFA` phase 1 能用吗？

**能，且一致（实测）。** 用现有 Z1 夹具 + `PA_PERSONALITY_TRACE` + `Z1_TRACE=1`
（personality 在 trace 里已经打印 `_Unwind_GetCFA(ctx)`，`runtime/rt_personality.c:589-596`），
同一帧（同一 `ip`）在 phase 1（`actions=0x1`）与 phase 2（`0x2`/带 handler 的 `0x6`）的 CFA 完全相同：

```
# z1_step2（单帧 catch-all）
actions=0x1 ip=0x402a7a pc=0x402a79 cfa=0x7fff0c7b9480 lp=0x402a81 action=1 kind=handler filter=1
actions=0x6 ip=0x402a7a pc=0x402a79 cfa=0x7fff0c7b9480 lp=0x402a81 action=1 kind=handler filter=1
# z1_step3（内层 cleanup，外层 handler）
actions=0x1 ip=0x402ae9 ... cfa=0x7fff0c7b9470 ... kind=cleanup    ← phase 1
actions=0x2 ip=0x402ae9 ... cfa=0x7fff0c7b9470 ... kind=cleanup    ← phase 2
actions=0x1 ip=0x402aba ... cfa=0x7fff0c7b9480 ... kind=handler    ← phase 1
actions=0x6 ip=0x402aba ... cfa=0x7fff0c7b9480 ... kind=handler    ← phase 2
```

（`bash tests/exc/z1_rt/run.sh` 的 58 项断言在此次探针里同样全 PASS。）CFA 还满足单调性：
内层 cleanup 帧 `0x…9470` < 外层 handler 帧 `0x…9480`，即**越深 CFA 越小**（x86-64 栈向下长）。

更强的旁证：libgcc 自己的 `_Unwind_RaiseException` 在 phase 1 把 handler 帧的 CFA 写进
`exc->private_2`，phase 2 每帧比较 `_Unwind_GetCFA(ctx) == exc->private_2` 才打 `_UA_HANDLER_FRAME`
（`tests/exc/z1_rt/README.md:167-170` 的反汇编结论）。如果两阶段 CFA 不一致，Z1 夹具的 handler
永远不会被安装——事实是它被安装了。

**结论：前提 1 成立；phase 1 记下的 CFA 在 LP 里就是目标帧（handler 帧）的 CFA。** 但这不是瓶颈，
而且还有更省的做法：LP 在 handler 帧内，自己的 CFA 静态可算（`rsp + frame_bytes_ + 8`，
`src/codegen_regalloc.cpp:24`），根本不必 phase 1 记。

### 3.2 致命问题：LP 执行时被跳过帧的栈已经不可安全访问（实测推翻）

先用代码把布局算清楚（全部是当前 codegen 的确定行为）：

- rsp 在函数体内恒定（`src/codegen_impl.h` 布局注释（写作时 :473-481）；`src/codegen_regalloc.cpp:12-26`），
  CFA = rsp + frame_bytes_ + 8（`src/codegen_regalloc.cpp:24`）。
- 帧记录在局部区**顶端**：`rec_off(i) = out_bytes_ + locals_ - 8*(words - i)`
  （`src/codegen_impl.h` 的 `rec_off()`（写作时 :489-492））。
- LP 入口 rsp = 抛出点那条 `call` **之前**的 rsp（= handler 体 rsp，记作 A），不是 CFA
  （`tests/exc/z1_rt/README.md:244-255` 的实测；`docs/zero-cost-exceptions.md:158-166` 的硬约束 1）。

设 handler 帧在 call 点的 rsp = A；最近被跳过帧 I 的压栈情况是 n_saved 个 callee-saved 寄存器 +
frame_pad_I。则 I 的 rsp = A - 8 - 8*n_saved_I - frame_pad_I，记录最后一个字的地址是
`rsp + locals_I - 8 + out_bytes_I = A - 16 - 8*n_saved_I`（用 frame_pad = out_bytes + locals 化简）。
也就是说：**记录整体落在 [A - 16 - 8*n_saved_I - 8*(words-1), A - 8 - 8*n_saved_I)**，
紧贴 A 的下方。而 LP 的任何 `call` 都从 A-8 开始向下写栈；一个普通 C helper 的 prologue
（`push %rbp`、保存 callee-saved）就落在 A-16、A-24……——**正好是记录区**。

实测（`/tmp` 探针，照 codegen 布局摆一条 5 字记录、n_saved=0、记录在 [A-48, A-8)；
handler 的 catch-all LSDA + LP 与 z1_step2 同形；LP 第一条指令先把 5 个字快照到全局，
再 `call` 一个只"保存寄存器"的 helper，然后回报）：

```
[lp] LP entry rsp = 0x7ffdd00506b0 (A)
[lp] record       = 0x7ffdd0050680 = A - 48, 占 [0x7ffdd0050680, 0x7ffdd00506a8)
  word0  快照 0x1111111111111111  现在 0x7c91d49eb000  <-- 被 helper 覆盖
  word1  快照 0x2222222222222222  现在 0x7ffdd00507e8  <-- 被 helper 覆盖   (word1 = n)
  word2  快照 0x3333333333333333  现在 0x1             <-- 被 helper 覆盖
  word3  快照 0x4444444444444444  现在 0             <-- 被 helper 覆盖
  word4  快照 0x5555555555555555  现在 0x7ffdd00507f8  <-- 被 helper 覆盖   (word4 = 槽地址)
[lp] changed = 5 / 5
```

5/5 个字被覆盖，包括链指针 `prev`、槽数 `n` 和槽地址数组。若照捷径去走链：`prev` 被改成堆地址、
`n` 被改成天文数字——要么崩溃、要么对野地址 dec（比崩溃更糟的静默破坏）。
helper 的栈用量只要 ≥ `8*n_saved + 16` 字节即可覆盖记录顶端；真实 `pa_eh_landing` /
`pa_frame_unwind_to_cfa` 不可能这么小，而且它内部 `pa_rc_dec` 跑的析构器还会继续深挖，
这是 C1 和 C2 的双重违反。

**补充两点，说明"绕一下"也救不回来：**

- 即便 LP 第一条指令前先快照（用 callee-saved 寄存器或全局预分配缓冲）再做释放，
  **快照动作本身**必须先读死帧内存；LP 入口没有"零栈使用"的余地（`call` 一进来就写 A-8）。
  而"零栈使用地读完整条链"要先把 n、槽地址、槽值全部读到寄存器/全局，再开始 dec；
  链深度无上限、槽数无上限，需要预分配全局缓冲——这就是方案 E，复杂度高、收益为零（见 §4）。
- GC：LP 运行期间 `pa_frame_top` 还指向死帧（捷径把释放放在 LP 里更晚），任何分配触发的 GC
  都会沿链扫死栈（C3）。探针 §6 里"只修链不释放"的降级特意先把链修好，就是为了不引入这一类崩溃。

### 3.3 订正后的用法

捷径错的是**释放位置**，不是 `cfa` 字段。把"批量释放"从 LP 挪到 **phase 2 的 handler 帧 personality
调用里**（安装上下文之前）就成立——见方案 G。另外 phase 1 记账那一步可以整个去掉：
phase 2 拿到的 `_Unwind_GetCFA(ctx)` 就是目标 CFA（§3.1 已证一致），不需要全局中转。

## 4. 候选方案对比

| # | 方案 | 释放位置 | 非抛出路径成本 | 代码/数据面积 | 抛出路径额外工作 | Z4 兼容性 | 结论 |
|---|---|---|---|---|---|---|---|
| A | lead 捷径（cfa + phase1 记账 + LP 里 `pa_frame_unwind_to_cfa`） | handler LP 内 | 帧记录 +8B，压帧 +2 指令（cfa） | 0 | 0（但正确性为 0） | 差 | **否决**（§3.2） |
| B | 每帧 cleanup landing pad（`docs/exc-semantics.md` §5.1） | 被跳过的每一帧自己的 pad | 0 | 每有槽函数：~18B pad 文本 + 8–12B LSDA + 4B FDE 指针 | 每跳一帧：1 次 phase2 personality + 1 次 pad 安装 + 1 次 `_Unwind_Resume`（重启 FDE 查找） | **最好**（finally 本来就要 pad） | 正确但最贵；留给 Z4 做 finally |
| C | personality 逐帧释放（LSDA ⇔ 记录一一对应） | phase 2 每帧 personality 内 | **0** | 每有槽函数 4B 最小 LSDA + 4B FDE 指针；需要"有 LSDA 必有记录"不变量 | 每帧 phase1 + phase2 各 1 次 personality；额外解析空 LSDA 头 | 一般（cleanup pad 仍需 release_self） | 备选；非抛出零成本，但 codegen 改动面比 G 大 |
| D | 降级：只修链、不释放 | handler LP 或 handler personality | 取决于修链手段 | 低 | 0 | 好 | **不可接受**（§6 量化） |
| E | LP 里先快照到堆/全局、再释放 | handler LP 内 | 0 | 需全局缓冲/堆分配 | 深递归、O(链深+槽数) 快照 | 差 | 否决（复杂且照样要面对 C2） |
| F | 帧记录搬出栈（运行时维护堆帧栈，prologue/epilogue 调用） | 任意 | **每函数每次调用都多一次 runtime 调用** | 低 | 低 | 好 | 否决（违背"非抛出路径零开销"） |
| **G** | **handler 帧 personality 里批量释放（cfa 字段）** | phase 2 的 `_UA_HANDLER_FRAME` 调用内、安装 LP 前 | 帧记录 +8B，压帧 +2 指令 | 0（LSDA 形态不变） | 每次捕获 1 次 O(中间记录数) 释放；无 pad、无 resume | **好**（Z4 的 cleanup pad 自己 release_self 即可，§5.2） | **推荐 Z2-b** |

### 4.1 方案 B 的问题（为什么不是 Z2-b 首选）

`docs/exc-semantics.md:334-375` 推荐 B 作为"复用现有代码约 90%"的最小方案。从**正确性**看它是好的；
但代价被低估：它要求**每个有 owned 槽的函数**都有 personality + LSDA + 一段 pad 代码，
而且每跳一帧都要一次 `_Unwind_Resume` 重启。用当前生成代码规模估：`22_exception` 有 18 个函数、
8 个压帧记录；整个程序量级是 789 个 FDE（`docs/zero-cost-exceptions.md:35`），按 ~44% 有记录算，
≈ 350 个函数要加 pad：约 6.3KB 文本 + 3.5KB LSDA + 1.4KB FDE 指针 ≈ **11KB**（§9 表），
抛出路经还要多 O(N) 次 resume。Z2-b 只需要"先跑通"，这笔面积/复杂度不划算。

### 4.2 为什么 C 不是首选

C 的非抛出成本确实为 0、面积也最小（≈3KB），但它需要一条不变量：**"有 personality/LSDA 的帧
一定在 `pa_frame_top` 链上有一条记录"**——否则 personality 无法判断"这个被跳过的帧要不要
`release_self`"。现状里"有 try 但一个 owned 槽都没有"的函数不发记录（`rec_words == 0`，
`src/codegen_block.cpp:250-254`），要给它补一条空记录（+40B 栈 + 6 条序言指令），这本身又是
非抛出成本；同时"无 try 但有槽"的函数也要发 personality+空 LSDA，codegen 改动面比 G 大。
留作 Z4 之后的面积优化（届时 Z4 已有 per-frame pad 的发射基础设施，可顺便切换）。

## 5. 推荐方案 G：handler 帧 personality 批量释放

### 5.1 运行时语义：`pa_frame_unwind_to_cfa` 与 `pa_frame_unwind` 的等价性

老世界的停止条件：`target = handler_top->frame = HandlerPush 时的 pa_frame_top`（`runtime/rt_exc.c:294`），
即"handler 帧的帧记录"，若 handler 帧没有记录则是它外面最近一条记录。

新世界用 CFA 表达同一个条件：

```c
/* 释放所有 cfa < target_cfa 的帧记录；target_cfa 只可能是 handler 帧自己的记录
   （cfa 严格随栈深度递减，同帧只有一条记录），或它外层第一条记录（handler 帧无记录）。 */
void pa_frame_unwind_to_cfa(uintptr_t target_cfa) {
    while (pa_frame_top && pa_frame_top->cfa < target_cfa) {
        PyAdamasFrame* f = pa_frame_top;
        /* 与 runtime/rt_exc.c:22-35 的单帧循环逐字相同：
           逐槽 读地址→判 bit1→非空清零→pa_rc_dec[_unknown] */
        release_one_frame(f);
        pa_frame_top = f->prev;
    }
    if (pa_frame_top && pa_frame_top->cfa == target_cfa) {
        /* handler 帧自己的记录：保留（handler 还要继续执行） */
    }
    /* 否则 pa_frame_top 为 NULL 或 cfa > target：handler 帧无记录，正确 */
}
```

等价性证明：CFA 随深度严格递减（§3.1 实测 + x86-64 栈布局）；handler 帧 H 的记录存在时其
`cfa == CFA_H`；不存在时链上第一条 `cfa > CFA_H` 的记录正好是"HandlerPush 时 pa_frame_top"
所指向的那条（H 及其内部所有帧都没有记录）。两种情况下释放集合与老世界完全一致。

实现要点（都从现有代码抄）：

1. `release_one_frame` 就是 `pa_frame_unwind` 内层循环（`runtime/rt_exc.c:22-35`）抽出来，
   含 bit0/bit1 标记判定，不要新写；
2. **释放期间记录留在链上**，逐槽"先清零再 dec"（与原实现一致）：析构器若触发 GC，
   未处理的槽仍是 GC 可见的根，不会被提前回收；这一帧全部槽处理完再 `pa_frame_top = f->prev`；
3. 结束时断言 `!pa_frame_top || pa_frame_top->cfa > target_cfa`，违反就 `panic_c`（不静默）；
4. 旧 `pa_frame_unwind(target)` **保留**：可挂起函数的 setjmp 降级路径还在用（`rt_exc.c:210`）。

### 5.2 personality 改动（Z2-b 的全部 personality 工作）

1. **`ex_class` 过滤（前置）**：只处理自己的异常魔数，其它类一律 `_URC_CONTINUE_UNWIND`
   （`docs/exc-semantics.md:508` 已有此要求）。不做的话，C++ 异常会被 catch-all 误认领，
   这里还会错误触发释放。
2. **phase 2 安装前释放**：在 `actions & _UA_HANDLER_FRAME`、`kind == PA_ACTION_HANDLER` 的分支里
   （`runtime/rt_personality.c:623-644`），在 `_Unwind_SetGR/SetIP`（`:655-658`）**之前**：

   ```c
   if (exc->exception_class == PA_EH_EXC_CLASS)
       pa_frame_unwind_to_cfa((uintptr_t)_Unwind_GetCFA(ctx));
   /* 然后才 SetGR/SetIP → _URC_INSTALL_CONTEXT */
   ```

   为什么这里安全：phase 2 的展开是**逻辑**展开，物理栈指针仍在抛出点；被跳过帧全部还在当前
   栈指针**上方**（未复用），unwinder 也只是普通 C 调用。释放期间析构器用的栈在更下方，不会覆盖
   被跳过帧（C1/C2 满足）；释放完 LP 才被安装，LP 拿到的 `pa_frame_top` 与老世界一致（C3 满足）。
3. **phase 1 不释放**、**FORCE_UNWIND 不释放**（现有早退分支 `rt_personality.c:515-521` 不动）。
   FORCE_UNWIND 若将来真用于取消/栈切换，需要另做逐帧 pad 版本；Z2 的 `_Unwind_Backtrace`
   根本不调 personality（Z1 README 坑 11），不影响。
4. 可观测计数器（`PA_PERSONALITY_TRACE` 下）：`pa_dbg_frame_release_calls`、
   `pa_dbg_frames_released`，给验收断言用（§10）。
5. **重抛/嵌套的正确性**：每次 `_Unwind_RaiseException` 都重新走 phase1/phase2；到新 handler 帧时
   批量释放的集合是"链上还未释放、且 cfa < 新 CFA"的记录——前面 cleanup pad 已经 release_self 过
   的帧不在链上，不会二次释放。这正是 G 与 Z4 的 finally/with 组合方式：**cleanup pad 自己
   `release_self` 后 `_Unwind_Resume`；G 负责 pad 之外那些"没人管的中间帧"**。

### 5.3 边界与限制

- **未捕获异常**：phase 1 找不到 handler 时 `_Unwind_RaiseException` 返回 `_URC_END_OF_STACK`，
  phase 2 根本不会跑 → 不释放；随后 `pa_eh_throw` 打 traceback + `exit(1)`（`docs/exc-semantics.md:490-497`），
  进程退出，泄漏不可观察。**但 Z2 仍必须加入口函数级 catch-all pad**（`docs/exc-semantics.md:599-602`）：
  既为 finally 语义，也让这条路径进入 phase 2。
- **C++/foreign 异常穿过我们的帧**：personality 不认领也不释放（§5.2 第 1 条）→ 我们的槽泄漏，
  但进程不崩。现有 setjmp 路径对这种情况是 UB（longjmp 跨 C++ 帧），所以这是净改善；登记为限制。
- **重入/析构里 raise**：析构路径是生成 trampoline + runtime free，不跑用户代码（`src/codegen.cpp:253-353`；
  `docs/runtime.md:756` 明确没有 `__del__`），所以不会在释放过程中嵌套 `_Unwind`。为防将来破坏这个前提，
  建议加 `static int in_frame_release` 重入 guard，重入即 `panic_c`。

## 6. 泄漏量化：不清理会漏多少、GC 兜不兜得住（Q4）

### 6.1 方法（探针，不改仓库代码）

- 复制 `runtime/` 到 `/tmp/zv_e3/rt`，只把 `pa_frame_unwind` 改成 `pa_frame_top = target;`
  （**等价于"Z2 只修链、不释放"的降级**），其余运行时逐字不变；
- 用 `build/dev/pyadamas asm` 取探针程序汇编，手工链接正常 `build/dev/rt.o` 与 patched `rt.o`，
  得到两份只差"中间帧是否释放"的二进制；
- 探针程序：`top`（try/except）→ `mid`（2 个 owned 堆槽）→ `leaf`（2 个 owned 堆槽）→ raise；
  每异常穿过 2 个有槽帧、**4 个 owned 槽**（`PYADAMAS_RC_STATS=1` 的 dec 差 80000 = 4×20000 验证）；
- RSS 用 fork+wait4 的 `ru_maxrss` 采样；同时用 `PYADAMAS_RC_STATS/GC_STATS` 看 GC 行为。

### 6.2 数据

| 场景 | 5000 次 | 10000 次 | 20000 次 | 斜率 |
|---|---|---|---|---|
| 基线（现有 setjmp 释放） | 1.97–2.28 MB | 2.00–2.28 MB | 1.72–2.00 MB | 约 0（噪声内） |
| 只修链不释放，**GC 关**（`PYADAMAS_GC_DISABLE=1`） | 4.50–5.09 MB | 7.89–8.17 MB | 14.53–14.80 MB | **约 0.63–0.65 KB/异常** |
| 只修链不释放，GC 开（`[rc]` 统计：minor=10/major=3，major freed=71088，minor freed=0） | 4.75–5.36 MB | 7.60–7.90 MB | 6.56–6.87 MB | 平台 ~6.5–7.9 MB |
| 同上但 exact-RC 模式（程序无 unknown） | 4.76–5.02 MB | 7.60–7.91 MB | 20k 处基线崩溃 | GC 开与 GC 关斜率相同 |

推导：**每异常泄漏 = 所有被跳过帧的 owned 槽对象**。本探针 4 槽约 640 B（含 RC 头/分配器开销）。
3 层帧、10 万次 = 64 MB；100 万次 = 640 MB，**无界**。

### 6.3 GC 到底能兜住多少

GC 是「RC 为主 + 循环收集器为辅」（`docs/runtime.md:770-805`），分两种收集：

- **minor（trial-deletion，`runtime/rt_core.c:1260-1302`）完全兜不住**：它把"集合外引用"当根
  （`:1272-1278`），而泄漏留下的 rc>0 **看起来就是外部根**，探测时被标活、晋升。实测 minor freed=0。
- **major（mark-sweep，`:1242-1259`、`:1305-1324`）能回收 *在 GC 链上的* 不可达对象**（实测
  gc_freed=71088，RSS 平台 ~7 MB），但有三个前提：对象在链上、到达 8 MB 级阈值才触发
  （`:1352-1360` 的 `major_threshold`），且回收前峰值 RSS 已经抬高数 MB。
- **exact-RC 模式完全兜不住**：整程序没有 unknown 时入口会调 `pa_disable_unknown_index`
  （`src/codegen.cpp:447-448`；触发条件 `src/ir.cpp:199-254`），此后 str/range/struct/
  无堆字段实例被 `rc_leaf_untracked` 判定为叶子（`runtime/rt_core.c:341-353`），
  `rc_link` 直接**不挂进 `heap_head`**（`:499-505`），且不计入 `alloc_since_gc/major`
  （`:559-563`）——GC 既看不见它们，也不会被它们触发。字符串是最常见的泄漏对象，这条路径上
  泄漏是**纯无界**的。实测：GC 开与 GC 关的 RSS 曲线几乎重合（上表第 4 行）。

### 6.4 结论：不可接受的降级

- 每异常 0.6 KB、无界增长；RLIMIT/长驻服务会被打爆；
- "靠 GC 兜"只对 major 可达的 tracked 对象成立，且代价是 RSS 峰值 + 数 MB 与延迟回收；
- **"不释放又不修链"更糟**：`pa_frame_top` 指向死栈，GC 根扫描（`runtime/rt_core.c:980-1004`）
  会读垃圾——随机误标甚至崩溃。要临时降级，**最低限度必须修链**（把 `pa_frame_top` 设回 handler 帧
  记录），并同时：(a) 加 debug 开关/计数器量化泄漏量，(b) 在 CI 里跑"允许上限"用例
  （如 20k 次 ≤ 2 MB），(c) 文档明确标注"Z2 临时缺陷、Z3 前必须修"。
  即便如此，也只能是"一两个里程碑"的临时状态，不能作为 Z2 的交付形态。

## 7. 对 codegen 的发射要求（方案 G，Z2-b）

1. **帧记录布局**（`runtime/rt_internal.h:327-334` + `src/codegen_impl.h` 的 `rec_off()` 与布局注释（写作时 :473-493） 同步改）：
   `PyAdamasFrame` 在 `scan_mask` 之后、`slots[]` 之前加 `uintptr_t cfa;`。
   生成代码侧：头字 5 → 6（`words = 6 + entries`，`src/codegen_block.cpp:250-254` 的 `rec_words`、
   `src/codegen_impl.h` 的 `rec_off()`（写作时 :489-492） 的 `rec_off` 公式同步），槽地址项从 `ri = 6` 开始。
2. **序言写 CFA**：在压链（`movq %rax, pa_frame_top`，`src/codegen_block.cpp:389-390`）之前：

   ```asm
   leaq  (frame_bytes_+8)(%rsp), %rax   # 与 .cfi_def_cfa_offset 同一口径（codegen_regalloc.cpp:24）
   movq  %rax, rec_cfa(%rsp)            # rec_mem(5)
   ```

   只有 `rec_words > 0` 的函数发（与现有压帧条件一致）。
3. **可选省一条指令**：若在意非抛出路径的 2 条指令，可存常数 `K = frame_bytes_+8-rec_off(0)`
   （`movq $K, rec_cfa(%rsp)`），personality 比较 `(uintptr_t)rec + K == CFA`；语义等价，
   但可读性差，建议 Z2-b 先用 `cfa`，性能敏感时再换。
4. **LSDA/CFI 形态不变**：方案 G **不新增** cleanup range；Z2-b 仍按 `docs/exc-semantics.md` §6.3
   只给 try 函数发 `.cfi_personality`/`.cfi_lsda`（`src/eh_emit.cpp:165-175`）与 catch-all 形态 A
   的 LSDA（`:177-262`）、以及 landing pad（LP 内分发块照旧）。
5. **landing pad 不承担任何帧释放**：LP 入口 `%rsp` 的约束见 `tests/exc/z1_rt/README.md:244-255`；
   LP 里"多打一次 `pa_frame_top` 的快照/恢复"是**错误**的（§3.2）。
6. **入口 catch-all**：`__pa_main` 外再包一层 catch-all 保护区，LP 复刻 `exc_print_uncaught + exit(1)`
   （`runtime/rt_exc.c:194-214`；`docs/exc-semantics.md:599-602`）。
7. **不要给手写汇编帧加 personality**：`main`、模块 init 桩、C 回调跳板、析构器、生成器帧释放跳板
   （`src/codegen.cpp:243-353、403-425、427-465`）不在 `pa_frame_top` 链上，也不需要释放；
   给它们加 personality 会破坏尾跳/跳板 CFI 约定（`docs/exc-semantics.md:359-360`）。
8. **门禁**：`zero_cost_exc && !is_generator && target_abi().gnu_cfi`；可挂起函数继续旧路径
   （`docs/exc-semantics.md` §5.2）。Win64 不发（`src/target_abi.h:57-58`）。
9. **优化遍**：新标签/LP 不能跨块搬运、不能被 DCE 删（对齐 `docs/exc-semantics.md` §6.5）。

## 8. 对 personality 的改动清单

| 改动 | 位置 | 内容 |
|---|---|---|
| ex_class 过滤 | `runtime/rt_personality.c:503` 附近 | `exc->exception_class != PA_EH_EXC_CLASS` → CONTINUE（§5.2-1） |
| phase 2 安装前释放 | `runtime/rt_personality.c:623-644`，SetIP 之前 | `pa_frame_unwind_to_cfa(_Unwind_GetCFA(ctx))` |
| FORCE_UNWIND | `:515-521` | 不动（不释放） |
| trace 计数器 | `:588-596` 的观测块 | 新增 release 次数/释放记录数，供验收 |
| 重入 guard | 新 helper 内 | `in_frame_release` 重入 panic（§5.3） |
| 断言 | 新 helper 内 | 结束后 `!pa_frame_top || pa_frame_top->cfa > target_cfa` |

## 9. 代价估算

口径：当前生成程序的规模按 `22_exception`（18 函数 / 8 个压帧）与"典型程序 789 FDE"
（`docs/zero-cost-exceptions.md:35`）外推；有记录函数占比按探针/用例的 44–50% 取约 350。

| 项 | G（推荐） | B（每帧 pad） | C（personality 逐帧） |
|---|---|---|---|
| 非抛出路径 | 帧记录 +8B；压帧 +2 条指令（lea+mov），只加在有记录的函数 | 0 | 0（"有 try 无槽"的函数要补空记录，约 +6 指令/个，数量少） |
| 代码面积 | +13B/有记录函数，约 **+4.5KB/程序**（LSDA 不变） | +18B pad 文本 +8–12B LSDA +4B FDE，约 **+10–12KB/程序** | 4B LSDA +4B FDE，约 **+3KB/程序** |
| 抛出路径 | 每次捕获 1 次批量释放，O(被跳过记录数)；无 pad/resume | 每跳一帧：1 次 personality + 1 次 pad + 1 次 `_Unwind_Resume` 重启（O(N) 次 FDE 查找） | 每帧 phase1+phase2 各 1 次 personality（额外解码空 LSDA 头） |
| 实现改动面 | 最小：结构体 1 字段 + 压帧 2 指令 + personality 1 处 + helper | 中：所有有记录函数都要发 LSDA+pad（codegen 大改） | 中：所有有记录函数发 LSDA；需不变量与两套释放语义 |
| 风险 | personality 里跑 RC 释放（析构无用户代码，已核）；foreign 异常不释放 | 机制最常规，但 resume 循环和 pad CFI 易错（Z1 坑 4/6/7） | "LSDA 与记录一一对应"不变量破坏时静默漏/多释放 |

注：以上是静态估算（按发射字节数与函数计数），不是实测；抛出路径的绝对纳秒数需 Z2-b 落地后用
`92_exc_inline_rc` 类用例压测。**Z2 期间不要同时放开寄存器提升/has_handlers 语义**
（`docs/exc-semantics.md:163-164`），否则性能差异无法归因。

## 10. 对 Z2 验收标准的增补（Q5）

建议在 `docs/exc-semantics.md` §6.6 的验收命令之外加：

- **V1（核心）异常路径 RSS 不涨**：一个"3 层帧、每层至少 1 个 owned 堆槽、10 万次 raise/catch"的用例，
  `RSS(100k) - RSS(1k) <= 1MB`。判据必须**不依赖 GC**：同一用例在
  `PYADAMAS_GC_DISABLE=1` 下再跑一遍（关闭 GC 时线性增长 = 帧释放没做）。
  用 §6 的 patched-runtime 手法可先验证"该用例确实能抓住不释放"（反证），避免验收空转。
- **V2 计数一致性**：`PYADAMAS_RC_STATS=1` 对比新旧路径：`dec/free` 计数差 =
  每异常被跳过帧的 owned 槽总数（本探针为 80000@20000 次）；漏释放时差值精确等于 80000。
- **V3 personality 观测**：`PA_PERSONALITY_TRACE` 计数器断言：每次捕获 release 调用 1 次、
  释放记录数 = 预期被跳过记录数；phase 1 释放 0 次；FORCE_UNWIND 释放 0 次。
- **V4 链一致性**：LP 入口（debug 模式）断言 `pa_frame_top == NULL || pa_frame_top->cfa > CFA_H`；
  或者老/新路径在 LP 入口打印等价的帧链快照，逐项对齐。
- **V5 嵌套/重抛**（Z3/Z4 逐步加）：except 体里再 raise、finally 里 raise、cleanup 之后继续展开，
  RSS 同样不涨。
- **V6 既有基线**：`bash tests/exc/sem_baseline.sh` 42/42，加 `readelf -S | grep gcc_except_table`、
  `objdump -d | grep -c "call _setjmp"`（新路径函数应为 0）。
- **备注（当前阻塞）**：把 `tests/run/92_exc_inline_rc.py` 的 `while i < 2000:` 改成 100000 后，
  当前 `build/dev/pyadamas` 跑出 SIGSEGV（gdb：`pa_exc_accept_value → pa_rc_inc`；
  探针同样复现）。这与本设计无关，但 V1 的"10 万次"规模依赖它，需要先定位/修复，
  或 V1 先按"2 万次 x 5 轮"实现。

## 11. 证据复现

> 说明：本文的 `file:line` 以写作时（2026-11-16 凌晨）当前工作区为准。写作期间 `src/codegen_impl.h`、
> `src/codegen.cpp` 正被并发修改，行号可能漂移；引用 codegen 内部结构时以符号名（`rec_off`/`rec_mem`）为准。

- **E1（CFA）**：按 `tests/exc/z1_rt/run.sh` 的方式用 `-DPA_PERSONALITY_TRACE` 编 `rt_personality.c` +
  夹具，`Z1_TRACE=1 ./z1_rt`；stderr 的 `[pa_personality] ... cfa=...` 逐帧对比 phase 1/2。
  本次实测 58 项断言全 PASS，CFA 两阶段逐帧相同（输出见 §3.1）。
- **E2（LP 覆盖记录）**：`/tmp/zv_e2/`：`probe.S`（handler catch-all LSDA + 被跳过帧按 codegen 公式摆
  5 字记录 + LP 先快照后 call 保存寄存器的 helper）、`probe.c`、`run.sh`；输出见 §3.2。
- **E3（泄漏）**：`/tmp/zv_e3/`：`patch_rt.py` 把 `pa_frame_unwind` 改成只修链；`run_e3*.sh` 编译
  patched `rt.o`、用 `pyadamas asm` 生成探针 `.s` 后手工链接；`rssrun.c` 用
  `fork+wait4(ru_maxrss)` 采样；数据见 §6.2。
- 以上探针全部在 `/tmp`，**未改仓库代码**；仓库内唯一产物是本文档。

## 12. 与其他文档的关系

- **修订 `docs/exc-semantics.md` §5.1**：其中的"每个有 owned 槽的函数发 cleanup-only LSDA + pad"
  仍是**正确**方案（B），但不应作为 Z2-b 首选；建议 Z2-b 用 G，B 留给 Z4 的 finally/cleanup pad。
- **补充 `docs/zero-cost-exceptions.md`**：Z2 的验收表加"异常路径无泄漏"；"已知难点"加本文 §5.3 的
  foreign 异常/未捕获限制。
- **与 lead 捷径的关系**：结论是"思路对了一半"——`cfa` 字段有用、CFA 两阶段一致，但**释放必须在
  personality 里做，不能在 landing pad 里做**。§3.2 的探针是这条结论的直接证据。
