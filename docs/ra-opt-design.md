# RA_OPT 设计文档 —— callee-saved 提升增强（削减 spill/栈往返）

> 分支 ra1，基线 master 131e7bd（0.335）。门控 `PYADAMAS_RA_OPT=0/1`，**默认 0**。
> flag-off 生成 asm 逐字节不变；flag-on 只改分配决策，不改指令语义。
> 新代码在 src/ra_opt.cpp（codegen_regalloc.cpp 已 2511 行超限，不再往里加逻辑）。

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
codegen_ops.cpp gen_load_slot 只覆盖 use_count==1 且同块，多使用/跨块副本漏掉）。

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
（codegen_regalloc.cpp 1856/2244 两处失败记录：slot_reg_ 是整函数布尔标记，
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

## 4. matmul 代价模型实验（独立门控位，可能负面结论）

（实施后补实测数字与结论。）

## 5. 实测结果（实施后补）

（abbench 数字、perf 复核、闸门结果。）
