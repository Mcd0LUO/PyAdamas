# 整数循环优化（0.335，PYADAMAS_LOOP_OPT 门控）—— 测量驱动设计

> 状态：进行中。分支 intloop，基线 master c7a5103（0.334）。
> 纪律：测量先行（perf annotate 定位真实热点），门控默认关，宁保守跳过不可误优化。
> 前车之鉴：docs/ivsr-negative-result.md（IVSR 负优化）、docs/bounds-elimination-attempt.md
> （ranged versioning 克隆阶段语义错误，fannkuch 死循环，已回退）。本轮不重蹈覆辙。

## 1. 热点取证（perf record -F 4999 -g，taskset -c 24，基线 -O2 生成码）

方法：每个基准循环跑 40–250 次（nqueens 40×~100ms、fannkuch 250×~11ms、
matmul 200×~14ms、fib 150×~19ms），perf annotate 按指令采样占比排序。
原始数据：/tmp/il-perf/{nq,fk,mm,fib}.data + *.annotate（会话临时目录）。

### 1.1 nqueens（98.75% 在 pyadamas_fn_1place_1）

热循环 = for c in range(n) 的早退路径（cols[c]!=0 → continue，占多数迭代）：

| 占比 | 指令 | 含义 |
|---|---|---|
| 15.32% | mov 0x108(%rsp),%rax | **row 从栈重载**（LICM 提升的 load.slot s0 被溢出） |
| 9.09% | jmp 3ba0 | continue 路径中转块 bb7（if.end: jump bb5） |
| 7.06% | jmp 3bc0 | bb4 尾跳到 bb7 |
| 6.59% | mov (%rdx,%rsi,8),%rax | cols[c] 元素装载 |
| 3.91% | mov 0x88(%rsp),%rax | row 第二份溢出重载（row-c） |
| 3.46% | jmp 3b60 | 回边无条件跳转 |
| 3.34%+2.81% | mov (%rdi),%rcx | len 装载（边界检查） |
| 3.33% | cmp 0x90(%rsp),%r14 | 循环条件（n 在栈上） |
| 3.01%+1.04% | mov 0x50(%rsp),%rdi | cols 基址每轮重载 |
| 1.08% | cmp $0,%r9 | cols[c]==0 |

**结论**：~19.6% 花在无条件跳转（continue 路径 bb4→bb7→bb5→bb3 三跳），
~20% 花在循环不变量 row 的栈重载（LICM 把 load.slot s0 提升成 **3 个独立
vreg** %31/%39/%69，各自溢出到 0x108/0x88/0xa8），~7% 在 len 装载。
边界检查的 cmp+jb 本身只占 ~1–2%（预测良好）——**BCE 不是 nqueens 的主菜**
（与 ivsr 文档的上界测量 -6% 一致），且 place 循环含递归调用，保守不做 BCE。

IR 根因：
- bb7（if.end）只有一条 jump bb5，是纯中转块 → **跳转线程化**可消掉。
- licm.pre 里 3× load.slot s0 未被合并：CSE 不处理 LoadSlot 指令本身
  （只把 LoadSlot 编码成 Bin 的 Slot 操作数）→ **同块 load 去重**。
  合并后 row 的权重（3 次使用×循环深度）压过 row+c（2 次），能抢到
  第 6 个 callee-saved 寄存器（place 槽分配 5/6，剩 1 个给跨块 vreg）。
- for 循环形状 cond→body→incr→cond 不满足 rotate_one_loop 的前提
  （branch 目标之一是回边块），旋转不生效；线程化后 continue 路径
  剩 2 跳（body→incr→cond），本轮不动旋转（保守）。

### 1.2 matmul（89.47% 在 pyadamas_fn_2run_2）

内层 j 循环 c[i*N+j] += aik * b[k*N+j] 每轮 ~40 条指令，其中 ~14 条
栈往返。最热指令全是索引算术 + 溢出：

| 占比 | 指令 | 含义 |
|---|---|---|
| 11.21% | add 0x210(%rsp),%rax | c_old + product（操作数在栈） |
| 10.29% | imul 0x200(%rsp),%rax | aik*b（操作数在栈） |
| 9.33%+8.17%+3.73% | mov %rax,0x200/0x210(%rsp) | 中间结果写栈 |
| 5.30% | mov 0x18(%rdi),%rax | index.set 读 elem_kind |
| 5.11%+3.52% | mov (%rdx,%rsi,8) / mov %rax,(...) | 元素读/写 |
| 4.68%+3.46% | mov 0x60(%rsp),%rdi | c/b 基址重载（堆槽，GC 要求住栈） |
| 3.65%+1.62% | mov (%rdi),%rcx | len 装载（边界检查） |

**根因**：检查型 index.get/set 的冷路径是 call pa_list_get/pa_list_set，
regalloc 把块内临时值一律视为跨调用活跃 → 全部溢出到栈。LICM 已把
i*N/k*N/aik/N 提升出循环，但它们是跨块 vreg，6 个 callee-saved 全被
槽占满（run: int 9/16 promoted, 6 regs）→ 不变量也住栈（0x1e0/0x1f0/
0x1f8/0x200/0x210）。另外 bb39（licm.pre）里 **3 份 i*N**（%49/%68/%85）
没被 CSE 合并：CSE 的 encode() 只归一化 LoadSlot/Const，**LoadGlobal
操作数按原始 vreg 编码**，g0 的三份 load.global 是三个不同 Reg 操作数 →
bin.mul 键不同 → 不合并。

**对策**：仿射 IV 边界检查消除（BCE）。j 循环无调用（IR 层面），
idx = IV + 不变偏移（i*N、k*N），IV 初值 0、步长 +1、条件 j<N。
入口守卫 off>=0 && (N-1)+off < len(base)（含溢出检测 max>=off），
通过则走 unchecked 克隆循环。**消掉冷路径 call 后块内临时值不再溢出**，
每轮 ~40 → ~16 条指令。这是 IVSR 文档「方案 B」之外被验证的路：
不建累加器（避免 12 累加器悲剧），只去检查。
同块 load 去重 + CSE 再合并 3 份 i*N → 1 份，减溢出槽。

### 1.3 fannkuch（65% 在 fannkuch 函数，RC ~8%）

热点分散：temp 列表元素读/写（swap 循环）、栈往返（0x98/0x1d0/0x1e0）、
jmp 链（2.12%+1.54%+1.27%）、rc_alloc_core/rc_link/pa_rc_dec_slow
（每轮排列 temp=[] 分配，合计 ~8%）。flip 内循环 while i<m 的
bound m 每轮递减（非不变量），且含双 IV（i+=1, m-=1）——本轮仿射
BCE 的保守形状（单 IV + 不变 bound）匹配不上，**跳过**（宁保守）。
跳转线程化可收掉 jmp 链的一部分。RC 开销是结构性的（R3 教训：不耗）。

### 1.4 fib（85.8% 在 fib 函数 + 8.3% 调用 thunk）

~48% 是栈 mov：第一次递归结果 %12 跨第二次调用活跃 → 溢出
（56(%rsp)），实参 n-1/n-2 经栈槽往返（40/64(%rsp)），n 虽在 %rbx
但结果加法走栈。**根因**：assign_global_vreg_regs 只提升跨块 vreg，
%12 定义/use 同块（bb1）但跨调用 → 不在候选池 → 溢出。fib 只用 1 个
callee-saved（%rbx），%r12-%r15 空闲。候选对策：把「同块但跨调用活跃」
的整型 vreg 纳入 callee-saved 提升。风险：整函数独占寄存器语义与
块内临时池的交互——**本轮先评估，若复杂度高则跳过**（fib 差距主要
是调用开销，Go 也 16.9ms，C++ 7.3ms 靠内联/尾优化，非单纯 regalloc）。

## 2. 实现计划（按 ROI/风险排序，全部 PYADAMAS_LOOP_OPT 门控，默认 0）

| 块 | 优化 | 目标 | 风险 |
|---|---|---|---|
| P1 | 跳转线程化（单 jump 块）+ prune_unreachable | nqueens ~9%、fannkuch jmp 链 | 极低（纯 CFG 等价变换） |
| P3 | 同块 LoadSlot/LoadGlobal 去重，再跑 CSE | nqueens row 合并抢寄存器；matmul i*N 3→1 | 低（局部值编号，无跨块活跃性变化） |
| P2 | 仿射 IV BCE（单 IV、初值 0、步长 +1、不变 bound、idx=IV+off、无调用循环、允许 IndexStore） | matmul 内循环去冷路径 call → 去溢出 | 中（克隆+守卫；用 IR 校验+difftest+ASan+边界回归兜底） |
| P4 | fannkuch 双 IV BCE / fib 同块跨调用提升 | stretch | 高 → 默认跳过，负面结论也记录 |

### P2 正确性论证（命门）

- **IV 范围**：header cmp.lt (load.slot S), BOUND（有符号），latch 恰一处
  store.slot S, add(load.slot S, 1)，循环外恰一处 store.slot S, 0 且支配
  header。⇒ 体内 S∈[0, BOUND-1]（BOUND<=0 时零次迭代）。S 不会回绕：
  体内 S<=BOUND-1<=INT64_MAX-1，+1 后 <=INT64_MAX。
- **idx = S + off**（off 循环不变、单定义、支配 header；含副本槽链
  load.slot T ← load.slot S 的追踪，T 循环内唯一写且支配所有读）。
  idx∈[off, BOUND-1+off]。
- **守卫**（循环入口，每次进入执行一次）：off>=0 && max>=off（检测
  BOUND-1+off 的回绕）&& max<len(base)。三者全过 ⇒ 整个循环
  idx∈[0,len)，unchecked 安全。守卫不过 → 原检查循环（负索引/越界
  语义、IndexError 时机完全不变）。零次循环：守卫可能不过，走原循环
  同样零次，不多抛异常。
- **len 不变**：循环内无 Call/CallDirect/VCall/…，CallRt 只放行纯标量
  白名单（复用 version_bounds 的 rtfn_is_pure_scalar + RC 记账）；
  IndexStore 只改元素不改 len；base 槽循环内无 StoreSlot、全函数无
  SlotAddr ⇒ base 对象与 len 恒定。**允许 IndexStore**（与旧
  version_bounds 不同：那里拒绝是因为外提 load 会读旧值；本轮不外提，
  只去检查，无别名问题）。
- **克隆**：整块复制，所有 dst 换新 vreg，块内目标重映射，块外目标
  （出口）不变；候选 IndexLoad/IndexStore 改 unchecked 变体；槽操作
  原样共享（两版互斥执行）。**不做外提/重定向**（旧 attempt 的
  cand_dsts/redirect 混用正是死循环根因）——克隆是纯复制，语义平凡。
- **异常时机**：守卫只含 Len/Cmp/Bin/Const（不抛）；unchecked 路径已证
  不越界；原路径异常时机不变。RC 记账指令原样复制，两版互斥 ⇒ 配平。
- 函数级前提：!has_handlers && !is_generator（与现有循环遍一致）。
- 变换后跑 IR 校验（vreg 定义存在、块目标合法），再 difftest 全量。

### 门控与缓存

- PYADAMAS_LOOP_OPT：unset/0 → 0（关）；1 → 7（全开）；其他整数 →
  显式位掩码（bit0=thread, bit1=bce, bit2=dedup，开发期 A/B 用）。
- Program.loop_opt → LowerOptions.loop_opt → optimize_ir(m, opt, loop_opt)。
- manifest 新增 loopopt <mask> 行（-1 = 旧清单，缓存失效一次），
  try_cache 比对——与 simd/rcsplit/ragraph 同纪律，防 A/B 命中旧产物。
- flag-off：optimize_ir 不进入任何新代码路径，asm 逐字节不变（38 热基准 cmp）。

## 3. 实测结果

### 3.1 实现与迭代（测量驱动的三次修正）

1. **P1 初版（prune 重编号）**：nqueens -6.4% 但 matmul +9.7%。objdump 对比
   发现 prune 重编号扰动线性扫描区间坐标（块号×stride），克隆/中转块删除
   让 s2/s6 丢寄存器。→ **不裁块不重编号**。
2. **P1 二版（清空死块）**：matmul 仍 +8.2%、fannkuch +10.7%。根因：codegen
   用「回边 jump」当循环头对齐代理（.p2align 5），清空/改向中转块让热 incr
   块丢对齐。→ **单 jump 块原样冻结**，只改多指令块出边（直达链尾）。
   修正后 nqueens -7~9%，matmul/fannkuch 中性。
3. **P2 初版（追加克隆到函数末尾 + 仅去检查）**：matmul +10%。两个根因：
   (a) 克隆共享槽但区间被拉成跨全函数 → 热槽丢寄存器；(b) **unchecked 存储
   仍有 kind 冲突冷路径 call** → 块内临时值照旧溢出（temp_reg_op_ok 拒绝
   IndexStoreUnchecked）。→ 克隆紧跟循环体插入（bump 块号）+ **kind0 证明
   存储**（入口 elem_kind==0 守卫 + 循环内全 hk0 标量 ⇒ imm=1 纯 mov 快路径）
   + temp_reg_op_ok 接纳该形态。修正后 **matmul -24.4%（BCE-only）**。
4. **摊销门槛**：fannkuch perm 旋转循环（平均 ~3.5 轮）摊不平守卫（BCE-only
   +1.7%）。常量 bound>=8 或运行时 bound 候选数>=3 才版本化。门槛后
   fannkuch 中性（+0.9%，噪声级）。

### 3.2 最终 A/B（掩码 3 = thread+bce，11 轮交替 BURST=5 绑核中位）

| 用例 | off | on | 变化 |
|---|---|---|---|
| matmul | 19.73ms | 14.80ms | **-25.0%** |
| nqueens | 105.96ms | 96.50ms | **-8.9%** |
| fasta | 8.84ms | 8.48ms | -4.0% |
| fib | 25.75ms | 25.29ms | -1.8% |
| nbody | 39.91ms | 39.48ms | -1.1% |
| fannkuch | 13.36ms | 13.48ms | +0.9%（噪声级） |
| binary_trees | 8.65ms | 8.71ms | +0.8%（噪声级） |
| mandelbrot | 24.01ms | 24.30ms | +1.2%（噪声级） |
| spectral_norm | 59.76ms | 60.15ms | +0.6%（噪声级） |

多租户负载窗口间 fannkuch 曾在 -3.9%~+3.5% 摆动（小效应+窗口敏感），
门槛后稳定中性。matmul flag-on perf：imul 9.5% + 元素写 8.7% 主导，
栈往返消失；剩余成本是跨块不变量（i*N/k*N/aik）重载——6 个 callee-saved
被槽占满，跨块 vreg 无寄存器可用（后续方向）。

### 3.3 闸门结果

- flag-off asm 逐字节 cmp：**104/104** 基准与基线 c7a5103 一致。
- difftest（O0 vs O2）：**280/0**，flag-off 与 flag-on 各一遍。
- 全套件：**298 ran ok ×2**（normal off+on）EXIT=0；**ASan（JOBS=1）off+on
  EXIT=0**。
- 语料零差异：tests/run + bench/daily **302 文件 flag-on==off**（输出+rc）。
- CPython 对拍：回归 293-296 expect 段由 CPython 3.14 生成，on/off 双模式
  通过；四基准输出与 CPython 一致（14200 / -104936,22 / 43198080 / 2178309）。
- check_size：无新增超限（5 个既有超限文件不变；opt_intloop.cpp 993 行 < 1200）。

### 3.4 负面结论与剩余

- **P3 同块 load 去重（bit2）**：正确但负收益——matmul BCE 收益被吃一半
  （-24.4%→-11.7%），fannkuch -7.8%→-1.7%；nqueens 期望的 row 合并抢寄存器
  未发生（-0.2%）。机制：删 load 改变槽活跃区间 → 线性扫描重排 → 克隆热槽
  丢寄存器。**不进默认掩码**，代码保留供后续（与 regalloc 改进协同可能翻正）。
- **fannkuch flip 循环**：双 IV（i+=1, m-=1）+ 非常量 bound，本轮形状不匹配。
  对 C++ 2.4× 差距结构性（RC ~8% + swap 栈往返）。需双 IV 仿射分析，独立一轮。
- **fib**：同块跨调用活跃值溢出（~48% 栈 mov），callee-saved 提升候选池扩展
  风险高，跳过。Go 16.9ms 同样有调用开销；C++ 7.3ms 靠内联/尾优化。
- **matmul 剩余**：跨块不变量住栈（槽占满 callee-saved）。方向：槽/vreg 统一
  分配（需 live range splitting，见 regalloc 失败记录）或 SIMD 整数路径
  （0.334 PYADAMAS_SIMD 已另行覆盖 matmul，门控独立）。
- **nqueens 剩余**：row+c/row-c+n 每轮重算（LICM 提了 row 但加法仍在循环内，
  因为 c 变）；place 递归调用阻止 BCE。对 Go 1.5× 差距主要在调用约定/帧
  开销（0x148 字节帧 + 6 push），需帧布局/调用优化，独立一轮。
