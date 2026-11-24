# RA_OPT 设计文档 —— callee-saved 提升增强（削减 spill/栈往返）

> 分支 ra1，基线 master 131e7bd（0.335）。门控 `PYADAMAS_RA_OPT=0/1`，**默认 0**。
> flag-off 生成 asm 逐字节不变；flag-on 只改分配决策，不改指令语义。
> 新代码在 src/backend/ra_opt.cpp（backend/regalloc.cpp 已 2511 行超限，不再往里加逻辑）。

## 1. 实测取证（本轮复核，build -O2，taskset -c 24，perf cycles:u）

单次运行：fib 26.5ms / matmul 18.5ms / nqueens 104ms。
热符号：fib → pyadamas_fn_1fib_1（91.2%），matmul → pyadamas_fn_2run_2（99.3%），
nqueens → pyadamas_fn_1place_1（99.6%）。

### 1.1 fib：递归块 8 条栈 mov，占函数周期 ~38%

fib(n>=2) 块内 IR：%11=n-1 → call → %12=fib(n-1) → %16=n-2 → call →
%17=fib(n-2) → %18=%12+%17 → return。**全部同块定义同块使用**
（cross_block_=false），不在 assign_global_vreg_regs 的候选池
（只收 cross_block_），块内有调用又拿不到 %r8-%r11 块临时池 → 全走栈槽：

```
mov %rax,0x28(%rsp)   2.48%   ; %11 存栈
mov 0x28(%rsp),%rdi   3.48%   ; 装参重载（纯往返）
mov %rax,0x38(%rsp)   1.20%   ; %12 跨调用 spill
mov %rax,0x40(%rsp)   1.40%   ; %16 存栈
mov 0x40(%rsp),%rdi   1.21%   ; 装参重载
mov %rax,0x30(%rsp)   0.60%   ; %17 存栈
mov 0x38(%rsp),%rax   6.55%   ; %12 跨调用 reload
add 0x30(%rsp),%rax   2.29%   ; %17 栈读
mov %rax,0x40(%rsp)  10.15%   ; %18 存栈
mov 0x40(%rsp),%rax   9.03%   ; return 重载
```

参数 n 已在 %rbx（参数槽线性扫描），**%r12-%r15/%rbp 五个 callee-saved 全空着**。
把 %11/%12/%16/%17/%18 提升进去，递归块栈流量归零。

### 1.2 nqueens：LICM 提升的只读参数副本各自溢出

place_1 的 licm.pre（bb11）把 row/n 的 load.slot 提升出 6 份独立 vreg：
%31/%39/%69 = load.slot s0（row），%18/%42/%72 = load.slot s1（n）。
s0/s1 是**只读参数**（函数内无 store.slot），已整函数驻留 %rbx/%r12。
但 6 个 callee-saved 被 5 个槽 + 1 个跨块 vreg（%33=row+c）占满，
这 6 份副本只能住栈，循环里反复重载：

```
mov 0x108(%rsp),%rax  9.06%   ; row 副本重载（row+c / row-c）
cmp 0x90(%rsp),%r14   2.77%   ; n 副本（循环上界）重载
```

栈触碰合计占函数周期 21.2%。这些副本的值**恒等于槽寄存器**——
不需要新寄存器，直接别名到槽寄存器即可（现有单使用别名机制
backend/ops.cpp gen_load_slot 只覆盖 use_count==1 且同块，多使用/跨块副本漏掉）。

### 1.3 matmul：6 个 callee-saved 被槽占满，跨块不变量住栈

run_2：16 个整型候选槽，9 个提升、6 regs 全占（i/k/j/j副本/aik/total 在
热区同时活跃，线性扫描的分配本身已是最优）。跨块不变量 %68=i*N、
%76=k*N、%72=aik副本（LICM 提升）拿不到寄存器，内循环每轮重载；
同块临时值（乘积/和/下标）在含慢路径调用的块里也全走栈：

```
mov %rax,0x210(%rsp)  10.80%  ; 乘积存栈
mov 0x1d0(%rsp),%rax   7.07%  ; aik 重载
mov %rax,0x200(%rsp)  11.14%  ; 和存栈
mov 0x1e8(%rsp),%rax  10.00%  ; i*N 重载
mov 0x208(%rsp),%rsi   5.16%  ; 下标装参重载
```

栈触碰合计占函数周期 72.2%。**容量问题**：槽与跨块 vreg 两趟分配，
槽先占满。统一分配/live range splitting 是历史多次失败区
（backend/regalloc.cpp 1856/2244 两处失败记录：slot_reg_ 是整函数布尔标记，
代码生成器不做区间判断；短区间槽拿寄存器 → 区间外读过期值 → 算错）。
本轮保守处理：P1/P2 先落地实测；matmul 需要的「槽让位给高权重跨块 vreg」
作为独立实验（§4），收益不抵风险就交付负面结论。

## 2. 方案（保守子集）

### P1 同块调用块 vreg 提升（fib 类）

assign_global_vreg_regs 候选池扩展：定义与使用同块、但所在块**含调用类指令**
（Call/CallRt/CallExtern/CallPtr/CallDirect/VCall/VCallStatic/SuperVCall/Setjmp，
与 slot_range_has_call 同口径）的整型 vreg，按现有 weight（使用次数×10^循环深度）
与跨块候选一起排序，从 n_saved_regs_ 起分 callee-saved，整函数独占。

候选过滤（比现有跨块池更严）：
- def_count==1（多定义不碰）；
- 类型确定整型（排除 Float/Unknown/Optional/heap）；
- 定义指令不是 LoadSlot/ConstInt/ConstNone（常量走立即数延迟，LoadSlot 有
  自己的别名机制；与 assign_block_temp_regs 的过滤同口径）；
- 不是 RcMark 水位（vreg_is_rc_mark）；不是 global_imm_ 常量。

正确性论证：机制与现有跨块提升**完全相同**——整函数独占 callee-saved，
mem_mop 直取寄存器，无区间判断、无调用点 spill（GPR callee-saved 天然跨调用安全）。
- 序言/尾声：n_saved_regs_ 按最大寄存器序号+1（现有循环末尾逻辑），CFI 自动。
- 异常：has_handlers 函数整函数跳过（现有门槛，setjmp/longjmp 回滚风险）。
  零开销展开按 CFI 恢复 callee-saved，与现有跨块提升同暴露面。
- 生成器：gen_ 跳过（现有门槛；堆帧不适用）。
- vthread：pa_vt_swap 保存/恢复全部 6 个 callee-saved（rt_vthread.c:157-176），
  挂起/切换安全，与现有跨块提升同暴露面。
- 槽共享：被提升 vreg 的栈槽可能与其它 vreg 共享（线性扫描），提升后该 vreg
  永不读写栈槽，共享方不受影响；GC 保守扫描临时区按类型位图，int 帧不扫。
- 块临时池：%r8-%r11 只给无调用块，P1 候选全在含调用块，零交叠。

### P2 只读槽 load 别名（nqueens 类）

vreg 由 LoadSlot s 单次定义、s 已整函数驻留 GPR（slot_reg_[s] 非空）、
s 在函数内**从未被写**（无 StoreSlot/SlotAddr/任何以 idx 引用 s 的非 LoadSlot
指令——覆盖 DictTryGet 类运行时直写）→ vreg_reg_[v] = slot_reg_[s]。
值恒等，别名整函数有效；不占新寄存器，n_saved_regs_ 不变。
被别名的 vreg 从 P1/跨块候选池剔除（已有位置）。

- move_to 对 dst==src 直接跳过 → 提升点 load 指令整条消失。
- 现有单使用别名（alias_）仍生效且指向同一寄存器，两条路径一致。
- split/graph 模式下 slot_reg_ 非空即整函数驻留（参数槽），同样安全。
- 只读判定是函数内全局扫描，宁严勿松：任何 idx 写引用都取消别名资格。

### 门控与缓存

- PYADAMAS_RA_OPT=0/1（默认 0），main.cpp 读一次 → Program::ra_opt →
  generate_asm 尾参 → AsmGen::ra_opt_。
- manifest 加 raopt 行（-1 = 旧清单，缓存失效一次），try_cache 校验。
- ra_opt_=0 时两个新函数都不调用，候选池/分配循环与基线逐位相同。

## 3. 闸门（见 CHANGELOG 0.336 与完成报告）

difftest 0；全套件 normal+ASan（JOBS=1）EXIT=0；CPython 对拍零差异；
日常语料零差异（输出+rc）；ASan detect_leaks=1 钉子 13/174/88/192/66/119 全 0；
flag-off 38 热基准 asm cmp 逐字节不变；异常/生成器/vthread 钉子
（92/93/119/225 等）flag-on 通过；abbench 交替绑核 A/B。

## 3.1 命门 bug 实录：P2 首版静默算错（已修复，回归钉死）

首版 P2 假设「slot_reg_[s] 非空 ⇒ 整函数独占」——**错**。linear_scan_pool
让活跃区间不重叠的槽**复用同一寄存器**（这正是它相对按频次分配的优势）。
c2_inventory 的 gen_rows：s0（n）在入口 hoist load 后即死，%rbx 随即被
循环里的 LCG 状态槽接管；别名让循环上界 cmp 读到 LCG 值，rows 600→1。
同根因：204_list_buf_pool / 193_rc_inline / 205_dict_scalar_destroy /
206_list_store_kind / 233_json_perf / 236_fp_spill_call + 9 个日常语料。

**修复**：P2 只别名「全函数没有第二个槽认领同一寄存器」的槽
（reg_users==1，backend/ra_opt.cpp）。参数槽/整函数活跃槽天然满足；nqueens
row/n 收益保留。跨块 vreg 提升（含 P1）不受此问题影响：那些寄存器从
n_saved_regs_ 起独占分配，本就不与槽复用。

教训：**「整函数布尔标记」的注释描述的是语义意图，不是分配器实现的
保证**；线性扫描/图着色都复用寄存器。任何「值恒等于槽寄存器」的推理
必须先证明寄存器单射。difftest（O0/O2）+ 日常语料对拍抓住了它；
随机 run_diff 840 程序首版也 0 失败 —— 复用形状需要足够长的函数 +
特定的死亡顺序，语料比随机生成更能压出这类 bug。

## 4. matmul：负面结论（容量问题，保守跳过分裂分配）

run_2 热区同时活跃 6 个整型槽（i/k/j/j副本/aik/total），线性扫描已把
6 个 callee-saved 分满且复用最优 —— 槽分配本身没有浪费。跨块不变量
（i*N、k*N、aik副本，LICM 提升）weight 再高也拿不到寄存器：两趟分配
槽先占满。P1/P2 都帮不上：P1 候选需空位（没有），P2 需只读槽
（aik 每轮 k 都写）。flag-on asm 与 off 逐字节一致，A/B 1.002（噪声）。

要真正解决必须「槽让位给高权重跨块 vreg」或 live range splitting：
- 统一分配是历史多次失败区（本文件 1856/2244 两处记录：slot_reg_ 是
  整函数布尔标记，codegen 不做区间判断，短区间槽拿寄存器 → 区间外
  读过期值 → mandelbrot 输出 2990360→1；限制整函数活跃槽又让
  spectral_norm 28ms→94ms）。
- 本轮还实测了「P1 按 weight 插队」的温和版：nbody −1.5%（weight
  误判把已有跨块 vreg 挤出寄存器）。已改成 leftover-only（§2 P1）。
- 分裂分配需要 codegen 支持区间边界插 move，是独立一轮工作。

**结论：matmul 本轮不动，交付负面结论。** 收益不抵复杂度/风险。

## 5. 实测结果

### 5.1 A/B（taskset -c 24，交替乱序，BURST=3，11 轮中位数；flag-on vs off）

| 用例 | off(us) | on(us) | 加速 | p25 加速 | asm |
|---|---|---|---|---|---|
| fib | 27947 | 27920 | 1.001 | 1.003 | 不变（递归 guard） |
| matmul | 22828 | 22786 | 1.002 | 1.035 | 不变（§4） |
| nqueens | 108108 | 107220 | 1.008 | 1.015 | P2 别名 6 副本 |
| fannkuch | 16655 | 16439 | 1.013 | 1.051 | P2 |
| mandelbrot | 27813 | 26658 | **1.043** | 1.051 | P2 |
| nbody | 41928 | 42341 | 0.990 | 0.999 | 不变（leftover-only） |
| spectral_norm | 62705 | 63095 | 0.994 | 1.009 | P2 |
| fasta | 9901 | 9586 | **1.033** | 1.080 | P2 |

初轮（7 轮）fannkuch 0.929 / nbody 0.985 异常值：11 轮复测证伪
（fannkuch 1.013；nbody asm 恢复不变后 0.990 是纯噪声）。nqueens 多轮
中位 +1~2%（前几轮 1.020/1.021/1.013，本轮 1.008，多租户波动）。
mandelbrot/fasta 收益稳定（p25 同向）。

最终二进制三目标复核（9 轮 BURST=5 交替绑核）：

| 用例 | off(us) | on(us) | 加速 | p25 |
|---|---|---|---|---|
| fib | 24318 | 24312 | 1.000 | 0.989 |
| matmul | 18642 | 18243 | 1.022* | 1.036* |
| nqueens | 104250 | 102812 | **1.014** | 1.022 |

*matmul 两侧二进制逐字节相同（cmp 验证），1.022 是多租户噪声，真值 1.000。

**nqueens perf 复核（flag-on）**：place_1 栈触碰周期占比 **21.2% → 6.6%**。
9.06% 的 row 栈重载（mov 0x108(%rsp),%rax）变 reg-reg mov（mov %rbx,%rax，
move elimination 零延迟）；2.77% 的 n 上界栈 cmp 变 cmp %r12,%r14。
剩余 6.6% 是堆列表指针（cols/d1/d2，帧记录/GC 根，不可提升）与 row+1
副本。墙钟 +1~2% 小于栈周期降幅：被消的 load 都是 L1 命中、与分支
误判主导的执行重叠，循环是前端/分支 bound —— 如实报。

### 5.2 fib 负面结论（P1 的由来与限制）

- 全提升 5 个同块值：0.938×（perf：push 27% / pop 15%，叶子白付税）。
- 只提升跨调用值（1 寄存器）：0.962×（仍负）。
- 自递归 guard 后：asm 不变，1.001×。递归函数的 callee-saved 提升
  需要 shrink-wrapping（叶子路径免保存）才可能正收益 —— 帧/CFI 改动，
  本轮保守不做。GCC 对照：-O2 把 fib 递归展开成迭代+显式栈，不是纯
  regalloc 差距。

### 5.3 闸门（最终代码）

- difftest（O0/O2，flag-on）：283 用例 0 差异 EXIT=0（JOBS=6）。
  277_urllib_errors 在 JOBS=12+ 并行负载下偶发 rc 抖动（固定端口
  18327 的本地 http.server 启动竞争），单跑 5/5 干净，低并行全量 0 差异。
- 全套件 normal flag-on：EXIT=0（301 ran ok，含 check/fail/strict/
  unsupported/modules/cdecl/cfi 全组）。
- 全套件 **ASan flag-on JOBS=1：EXIT=0**。
- 随机 CPython 对拍 run_diff（seeds 3）：840 程序 0 失败。
- 日常语料对拍 CPython（输出+rc）：23/23。
- ASan detect_leaks=1 钉子 13/174/88/192/66/119：6/6 全 0。
- flag-off asm 逐字节：92 基准（compare 9 + realistic 6 + builtins 54
  + daily 23）对主树 build/lead 0.335 基线 cmp 全过，多轮复验。
- 异常/生成器/vthread 钉子 92/93/94/119/225 + 回归 297-299：flag-on
  全套件内通过；297-299 flag-on/off 双模式独立验证。
- check_size：无新增超限（backend/ra_opt.cpp 239 行；backend/impl.h 压回 1199）。
- 模式组合抽查：RA_GRAPH+RA_OPT、RA_SPLIT+RA_GRAPH+RA_OPT 全过；
  **RA_SPLIT+RA_OPT 在 119/299（生成器+except）SIGSEGV —— 归因：
  RA_SPLIT 单独（无 RA_OPT）同样 rc=139，主树 0.335 基线编译器亦
  rc=139，是 split 实验门控的既有 bug，与 RA_OPT 无关**（加不加
  RA_OPT 崩溃一致）。split 默认关，不在本轮修复范围。

### 5.4 残留风险

- P1 互递归（A↔B）无 call graph 分析，不在自递归 guard 内；leftover-only
  保证不劣化已有分配，但新增寄存器仍可能有叶子税。门控默认关。
- P2 单射校验是保守充分条件：寄存器复用但时序安全的形状不别名
  （少赚不亏）。
- fannkuch 初轮 −7% 复测证伪（1.013），但布局敏感性真实存在（本仓库
  有 ±10-17% 布局位移先例）；flag-on 的逐基准收益以多轮中位为准。

## 6. 默认化决策（0.475）：保持 opt-in

口径：abbench 式交替 A/B —— 同一编译器、两侧独立 `PYADAMAS_CACHE_DIR` + `--rebuild`、
每轮打乱用例顺序、奇偶轮交换 A/B 先后、BURST=3 连跑取平均、15 轮中位数/p25、绑核。
两窗：realistic（6 例）/ classic（13 例）。

| 窗口 | off | on | 合计 |
|---|---|---|---|
| realistic | 1321.78 ms | 1319.08 ms | -0.2% |
| classic | 315.99 ms | 321.83 ms | +1.8% |

逐例（只列 asm 真正变化的——其余 15 例 flag-on/off `asm` 逐字节相同，差值全是噪声）：

| 用例 | 中位 | p25 | asm |
|---|---|---|---|
| nqueens | +8.5% | +7.0% | 变（34 行） |
| fasta | +4.8% | +20.9% | 变（16 行） |
| dict_wordfreq | -0.7% | -0.3% | 变（33 行） |
| obj_dispatch | +0.2% | +0.1% | 逐字节相同 |

**判据（两窗同向且 ≥2% → 默认打开）不成立**：一窗 -0.2%、一窗 +1.8%（反号）；
唯一稳定的大幅差异是 nqueens 的真实产物变化，方向是回退。因此**保持默认关**。

附带两条结论：

1. 0.470 的入口私有全局提升之后，obj_dispatch 上 RA_OPT 的收益**归零**（+0.2%，
   与 HANDOFF §5.3 的预判一致）。
2. 历史记录的 mandelbrot/fasta +3~4% 不复现：fasta 这次是 +4.8% 回退且 p25 +20.9%
   （短用例噪声带内），mandelbrot 的 asm 逐字节相同（0.0% 量级）。

未做：默认化需要的是「全量日常语料（`bench/daily` 23 例 + `bench/realistic`）A/B」，
不是 12 个基准；本轮只回答「12 基准上是否达标」，答案是不达标，故不打开。
