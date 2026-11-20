# 浮点循环 SIMD 向量化设计（0.333 原型）

> 状态：**设计 + spectral_norm 最小原型**。门控 `PYADAMAS_SIMD=0/1`，**默认 0**。
> 本轮只证明一个循环模式（eval_A 的 `1/(i+j)` 除法 + 乘加累加 reduction）的收益，
> 不做通用自动向量化器。通用化分阶段（§9）。正确性优先：宁可保守跳过，不可误向量化。

## 0. 问题与取证

0.331 数值核与 Go/C++ 的差距（min-of-5，负载窗口比值）：

| bench | PA | Go | C++ | PA/Go |
|---|---|---|---|---|
| spectral_norm | 54.4ms | 15.9 | 15.1 | 3.4× |
| nbody | 33.2ms | 16.1 | 16.3 | 2.1× |
| matmul（整数） | 14.2ms | 6.0 | 5.9 | 2.4× |

lead 已 `perf annotate` spectral_norm（0.331 -O2）：98.19% 时间在
`pyadamas_fn_0__pa_main`（内循环全内联），热点指令**全是标量 SSE2**：

```
addsd %xmm7,%xmm9   9.48%      divsd %xmm7,%xmm6   9.36%
addsd               7.05%      mulsd 0x1d0(%rsp)   5.38%
movq %xmm6,%xmm7    4.35%      mulsd 0x1b8(%rsp)   3.35%
divsd               2.71%
```

并伴随**大量栈溢出/重载**（`movsd 0xa8(%rsp)` / `mulsd 0x1d0(%rsp)` /
`mov 0x1d8(%rsp)`）——浮点寄存器压力导致 spill；2D 寻址的整数 `imul/add` 可观。
生成 asm **0 条 packed SIMD**（spectral_norm 84 条标量 sd）。C++ g++ -O2 会自动
向量化这些循环。差距的主因不是算法，是**标量发射 + 每元素一条高延迟 divsd +
整数 floordiv 展开 + spill 流量**。

### 热点循环的 IR 形态（O2 内联 + LICM 后）

`mul_ata` 的内循环（`__pa_main` 里 bb12/bb13/bb14；独立函数时 f2 的 bb5/bb6/bb7）：

```
bb_cond:  %jc = load.slot sJ            ; j
          %c  = cmp.lt %jc, %n          ; j < n
          branch %c, bb_body, bb_end
bb_body:  %j1 = load.slot sJ
          store.slot sC, %j1            ; sC = j（块内副本槽）
          %tot = load.slot sT           ; total（float 累加器）
          %j2 = load.slot sC
          %s   = bin.add %i, %j2        ; s = i + j（mul_a 是 j + i）
          %s1  = bin.add %s, %one       ; s + 1
          %p   = bin.mul %s, %s1        ; s*(s+1)
          %p2  = bin.floordiv %p, %two  ; // 2
          %di  = bin.add %p2, %A        ; + A（A = eval_a 第一实参：mul_ata 是 i，mul_a 是 j）
          %d   = bin.add %di, %one      ; + 1
          %df  = convert %d             ; int → float
          %a   = bin.div %fone, %df     ; 1.0 / d
          %a2  = mov %a
          %j3  = load.slot sC
          %vj  = index.get %list[%j3]   ; v[j]（list[float]，kind0 裸 double）
          %t   = bin.mul %a2, %vj
          %tot2= bin.add %tot, %t       ; total += a*v
          store.slot sT, %tot2
          jump bb_incr
bb_incr:  %ji = load.slot sJ
          %jn = bin.add %ji, %step      ; j += 1
          store.slot sJ, %jn
          jump bb_cond
```

关键事实（决定原型可行性）：

* **list[float] 是 kind0 非装箱数组**：对象布局 +0 len、+16 data、+24 elem_kind，
  元素是裸 double（`codegen_index.cpp` 的 kind0 快路径直接 `movsd (data,j,8)`）。
  所以 `v[j]`、`v[j+1]` 可以一条 `movupd` 取两个。
* **floordiv //2 被展成 ~14 条整数指令**（sarq/andq/addq/sarq + 余数校正分支），
  因为通用常量 floordiv  lowering 不知道被除数非负且为偶。向量化用 `psrlq $1`
  一条搞定（守卫保证 p≥0 且偶）。
* **累加器 total 提升到 xmm8–15**（浮点槽提升），j 提升到 callee-saved GPR。
  原型直接在这两个寄存器上原地累加/递增，不新增 spill。
* i、n、list、1.0 都是 LICM 外提的跨块值（寄存器或栈槽），循环内不变。

## 1. 目标循环模式识别

原型只认**一种**模式：「可数 for + 浮点数组逐元素 map + 乘加累加 reduction」，
即上面 bb_body 的 19 条指令序列（op + 操作数结构精确匹配，vreg 用变量绑定）。
判据（全部满足才向量化，任一不满足整段跳过走标量）：

1. **循环结构**：cond 块恰为 `load.slot sJ; cmp.lt sJ, n; branch body, end`；
   incr 块恰为 `load.slot sJ; bin.add sJ, step; store.slot sJ; jump cond`，
   step 是 `const.int 1`；body 末尾 `jump incr`；三块互为唯一前后继（回边）。
2. **步长 1**：保证标量访问 j, j+1, j+2…，向量对 (j, j+1) 恰好覆盖相邻两元素。
   步长 ≠1 一律跳过（否则 j+1 是标量循环不会访问的元素，会算错）。
3. **j 单调非负**：运行时守卫 `j ≥ 0`（不靠证明初值，省扫描）；sJ 在循环三块内
   只被 incr 写（+1），不被取地址（`SlotAddr`）。
4. **副本槽 sC**：body 第 1 条 `store.slot sC, j`，sC≠sJ，循环内只被这一条写；
   两处 `load.slot sC` 读的就是 j。
5. **累加器 sT**：float 槽，循环三块内只被 body 末条 `store.slot sT` 写，不被取地址。
   不要求初值是 0.0——向量循环读当前 total 原地累加，初值任意都正确。
6. **不变量输入** i / n / list / fone：定义在循环三块之外；若 i 是 `load.slot sI`，
   sI 在循环内无写；n/list 是 `load.global` 或跨块 vreg，循环三块内无 `store.global`、
   无调用（body 唯一可能的调用是 index.get 的冷路径 pa_list_get，只在标量尾循环走）。
   向量循环本身**零调用**，故这些值在向量循环期间不可能被改写。
7. **算术形状**：s=A+B（A/B 之一是 j=load.slot sC，另一个是不变量 i，加法可交换）；
   s1=s+1；p=s*s1；p2=p//2（除数 const.int 2）；di=p2+A（A 是 s 加法的第一操作数，
   即 eval_a 第一实参）；d=di+1；convert int→float；a=fone/df（fone 是 const.float）；
   mov；vj=list[j]（IndexLoad/IndexLoadUnchecked，结果 float，list 类型 list[float]）；
   t=a*vj；tot2=tot+t；store sT。所有 +1 是 const.int 1。
8. **函数级前提**：非生成器（`!gen_`）、`opt_≥2`、默认寄存器分配
   （`slot_loc_.empty()` 且 `!ra_graph_`）、`!tags_enabled_`、SysV 目标、
   函数无 `setjmp`（handler 会关闭提升，形态不稳）。

### 为什么这些判据足够安全（别名/依赖）

* 循环内**唯一的内存写**是槽写（sC/sT/j，都在栈帧或寄存器），**没有 index.set /
  store.global / 调用**。list 数据数组只读。因此不存在「写 v[k] 影响 v[j]」的
  循环携带依赖，也不存在 out 与 v 别名问题——`out[i]=total` 在 bb_end（内循环外）
  才发生，即便 out 与 v 是同一对象也不影响内循环向量化。
* 向量循环零调用 → 无 GC safepoint、无信号检查点变化（标量热路径本来也无调用），
  list 对象/data 指针在循环期间不可能被 append/realloc 改写。
* 越界与异常语义由**守卫 + 标量 fallback** 完整保留（§3）：len<n 或 kind≠0 时
  整段走标量，IndexError 仍在第一个越界 j 处由 pa_list_get 抛出，时机不变。

## 2. IR 层表达：codegen 直接 lowering（不上向量 IR op）

两条路线：

| 路线 | 做法 | 代价 |
|---|---|---|
| 向量 IR op | 新增 `vec.bin`/`vec.load`/`vec.reduce` 等 op + 向量类型，opt 遍改写循环 | 要动 ir_ops/ir_types、类型系统、regalloc（128 位值的活跃区间/spill）、所有消费点；一轮做不完 |
| **codegen 直接 lowering（选用）** | 在 `gen_block` 发射体块时识别 IR 指令序列，直接发 packed SSE2，跳过对应标量指令 | 不碰 IR/类型/regalloc；模式识别局限在发射层，通用化时再上 IR |

原型选**codegen 直接 lowering**：vreg 层已是 SSA、槽位提升已把 total/j 放进寄存器，
发射层拿得到一切物理位置信息（`slot_mop`/`src_mop`），无需新 IR 语义。新增
`src/codegen_simd.cpp`（codegen 文件已贴近 1200 行闸门，向量化 lowering 独立成文件），
在 `AsmGen::gen_block` 开头挂钩：匹配成功 → 发「守卫 + 向量循环 + 跳标量尾」，
随后**让原有指令发射机器照常发射全部标量指令作为尾循环/fallback**（§4）。
匹配失败 → 一个字节都不发，走原路径。

## 3. SSE2 packed double 起步

x86-64 基线必有 SSE2，**无需 CPU 检测**即可用 128 位 packed double（2 double/指令）：

```
movupd  (data,j,8), %xmm0   ; 取 v[j], v[j+1]（unaligned，§3.1）
mulpd   %xmm0, %xmm1        ; a0*v0, a1*v1
addpd   / divpd             ; 逐 lane 加 / 除
paddq   / pmuludq / psrlq   ; 64 位整数加 / 低32×低32→64 乘 / 逻辑右移
pshufd  $8, %xmm0, %xmm0    ; 把两个 qword lane 的低 dword 挤到一起
cvtdq2pd %xmm0, %xmm0       ; 两个 int32 → 两个 double
movlhps / movhlps           ; lane 广播 / 高 lane 提取
```

### 3.1 对齐

起步一律 **`movupd`（unaligned）**。list data 是 `malloc` 出来的 double 数组，
基址通常 16 对齐但 j 任意 → `data+8j` 多半不对齐；`movupd` 在 Nehalem 之后
与 `movapd` 同速（跨 cache line 才有罚），不值得为对齐做 peeling。原型不做
对齐 peel，文档记为后续可选优化。

### 3.2 整数部分向量化的范围守卫（pmuludq / cvtdq2pd 的精确性）

SSE2 没有 64×64→64 packed 乘，`pmuludq` 只乘每个 qword lane 的**低 32 位**
（无符号）得 64 位积；`cvtdq2pd` 只转**低 dword 的 int32**。要让它们与标量
`imulq` + `cvtsi2sdq` 逐位一致，必须保证 s=i+j 足够小。原型用一条保守守卫：

```
i ≥ 0  且  i + n ≤ 46341
```

推导：s_max = i+n-1 ≤ 46340 → p = s(s+1) ≤ 46340·46341 = 2147441940 < 2^31
（pmuludq 低 32 位精确、积非负）；p 恒偶（s(s+1) 必偶）→ `psrlq $1` 精确等于 //2；
d = p/2 + A + 1 ≤ 1073720970 + 46340 + 1 = 1073767311 < 2^31（cvtdq2pd int32 精确、
d≥1 无除零）。spectral_norm 的 i+n ≤ 1199，远小于 46341，守卫恒过。
守卫不满足 → 整段标量 fallback（任意大整数都正确）。46341 是保守常数，
后续可放宽到「运行时算 d_max < 2^31」或走 64 位转换路径（§9）。

### 3.3 尾循环（标量余数）

向量循环每轮吃 2 个元素，`j += 2`，条件 `j+1 < n`。退出时 j==n-1（奇数余 1）
或 j==n（偶数吃尽）：

```
cmpq n, j
jl  .Lscalar_tail   ; j==n-1：剩 1 个，走标量体一次
jmp bb_incr         ; j==n：吃尽，直接进 incr（j=n+1，cond 退出，无副作用）
.Lscalar_tail:
<原 19 条标量指令照常发射>   ; 末尾 jump bb_incr
```

n≤1 时 `j+1<n` 守卫直接失败 → 全标量。j 在下一轮外层迭代被重置为 0，
偶数情形 j 短暂 overshoot 到 n+1 无害（sJ 只被本内循环读）。

### 3.4 reduction：原型保持累加顺序（逐位一致），水平求和留待阶段 2

**这是正确性命门。** 标量是 `total = ((t0 + t1) + t2) + …` 串行链。若用
2-lane 累加器 `acc=[偶数和, 奇数和]` 最后 `acc0+acc1`（水平求和），加法顺序
改变 → 末位 ulp 微差。spectral_norm 打印 `int(sqrt(v_bv/v_vv)*1e6)`，微差**可能**
翻转整数输出。

原型选**保序提取**：向量算出 t=[t0,t1] 后，`addsd t_low, total` →
`movhlps` 取高 lane → `addsd t_high, total`，**两次标量加按原顺序**累加。
IEEE 加/乘/除每 lane 正确舍入，与标量逐位相同；累加顺序不变 →
**total 与标量逐位一致，输出钉住零风险**。代价：每 2 元素 2 条 addsd + 1 条
movhlps（而非 1 条 addpd）。实测（§7）表明 addsd 链不是瓶颈（标量 ~16 cycles/元素，
addsd 链 4 cycles/元素，被 OoO 掩盖），divpd/整数/spill 的削减才是收益大头。

阶段 2 若要进一步压 addsd 链，可切 2-lane 累加 + 水平求和，但**必须**先验证
spectral_norm/nbody 打印输出在全部基准规模下与标量一致（钉住），否则不启用。
设计层面水平求和用 `movhlps` + `addsd`（2 lane）或 `haddpd`（SSE3，慎用）。

## 4. 与 cgspeed 结构化发射机器（MOperand/MInstr）集成

发射全走结构化层（`codegen_emit.h` 的 `emit2/emit1/emit_imm` + `op_reg/op_mem`
工厂），不拼字符串：

* 新 packed 助记符进 `MOp` 枚举 + `mach_emit.cpp` 的 `kOpNames` 表
  （`kOpNameByEnum` 编译期自动生成，数组容量 128 足够）。**只加枚举项不改任何
  现有发射点 → flag-off 逐字节不变**。
* 向量体用 `emit2(MOp::kMulpd, op_reg(kXmm0), op_reg(kXmm1))` 等直建；
  内存操作数 `op_mem(kRdx, jreg, 8)`（data+j*8）。
* 输入物理位置经 `src_mop(vreg)`/`slot_mop(slot)` 解析（寄存器或栈槽皆可），
  不硬编码寄存器名。

### 4.1 寄存器约定与减 spill

* **scratch 只用永不提升的寄存器**：GPR `%rax/%rcx/%rdx/%rsi/%rdi`
  （提升池是 callee-saved rbx/r12–r15/rbp + 块临时 r8–r11，绝不碰前者）；
  xmm `%xmm0–%xmm7`（浮点提升池是 xmm8–15）。块内浮点临时 xmm3–7 只在
  标量尾循环里被定义，向量体先于它们运行，clobber 无害。
* **total 原地累加**：sT 提升到 xmm → 直接 `addsd …, %xmmT`（T∈8..15）；
  sT 在栈 → 用 xmm7 当工作累加器，每轮 `movsd` 写回栈（标量尾 load.slot 读栈，最新）。
* **j 原地递增**：要求 sJ 提升到 GPR（`slot_mop(sJ)` 是 Reg），`addq $2, jreg`；
  未提升则跳过向量化（保守）。spectral_norm 恒提升。
* 向量循环**不产生新 spill**：所有中间值在 xmm0–7 scratch，循环不变量
  （I01=[i,i+1]、IP1=[i+1,i+1]、ONEI=[1,1]、ONEF=[1.0,1.0]、data 指针）
  在 bb_body 入口 setup 一次常驻 scratch，循环内不再访存取它们。
  对比标量每元素 ~6 次栈往返（0x1c8/0x1b8/0x1d0），向量每 2 元素仅
  1 次 movupd（取 v）+ 可选 1 次 total 写回。

### 4.2 与标量发射机器的状态衔接

向量体在 `gen_block` 指令循环**之前**发射，随后原机器照常发射全部 19 条标量
指令作为尾循环。衔接不变量：

* 块入口 `alias_`/`const_val_` 已清空；向量体不建别名，标量机器从干净状态开始。
* 向量体维护 sJ/sT 在 `slot_mop` 指定位置（与 StoreSlot 同口径，默认模式无双写），
  标量尾的 `load.slot sJ/sT` 别名到同一寄存器/栈，读到最新值。
* 向量体只 clobber scratch（§4.1），不碰任何提升寄存器（除按语义更新 j/total）。
* 标量尾 index.get 冷路径调用 pa_list_get 时，`spill_fp_slots`/`reload_fp_slots`
  按既有浮点活跃性掩码写回/读出 total——向量体已把 total 放在提升寄存器，一致。
* 块内临时 vreg（%s/%p/%df…）只在标量尾被定义；向量路径跳过它们，但它们块内即死，
  块出口无人读，安全。

## 5. 门控方案

* **`PYADAMAS_SIMD`**：环境变量，`0/1`，**默认 0**。`main.cpp` 读一次写进
  `Program.simd`，经 `generate_asm(..., simd)` → `AsmGen::set_simd`。
* **缓存失效**：simd 写进模块缓存清单（Manifest 加 `simd` 行，read/write/try_cache
  比对），与 rc_split/ra_graph 同口径——否则切开关命中旧产物，A/B 拿假数据。
* **flag-off 逐字节不变**：门控关时 `gen_block` 挂钩直接 return false，不发一个字节；
  新 MOp 枚举项不被任何现有发射点引用。验收：spectral_norm/nbody/matmul 及 26 热基准
  flag-off asm 与基线 `cmp` 逐字节相同。
* **flag-on 也只改匹配到的循环**：未匹配函数/循环一个字节不变。

## 6. 正确性口径

1. **逐位一致（原型）**：保序提取累加（§3.4）→ total 与标量逐位相同 →
   spectral_norm 输出钉住（N=600 → 811972），nbody 输出 likewise。
2. **fallback 语义不变**：守卫不满足/模式不匹配 → 原标量路径，越界 IndexError、
   负索引、kind 升级等全部由运行时按原时机处理。
3. **验收闸门**：difftest（O0/O2）0 差异；全套件 normal+ASan EXIT=0
   （PYADAMAS_TEST_JOBS=8）；CPython 对拍零差异；flag-off asm 逐字节不变 +
   全套件零回归；check_size 无新增超限。
4. **flag-on 额外验收**：spectral_norm/nbody flag-on 输出 == flag-off 输出 ==
   CPython 输出；新增回归用例钉住向量化 reduction 输出（含奇偶 n、n≤1、
   大 i+n 触发 fallback、len<n 触发 IndexError 等边界）。

## 7. 实测（见 CHANGELOG 0.333 与完成报告）

原型完成后填：spectral_norm flag-on/off min-of-5 加速比、perf annotate 前后
（packed SIMD 出现、标量 sd 占比下降）、divpd 收益实测（**不假设**：Cascade Lake
divpd 吞吐可能与 divsd 不同，收益以 abbench 为准）、26 热基准 flag-off 无回归。
若收益不抵复杂度，如实给负面结论（参照 P2）。

## 8. AVX2 评估（阶段 2+，本轮不做）

AVX2 `vmulpd/vaddpd/vdivpd ymm` 4 double/指令，理论再翻倍。但：

* **CPU 检测**：需 `cpuid` 查 AVX2（leaf 7 EBX bit 5）+ OS 支持（XCR0 的 YMM 位，
  `xgetbv`）。运行时检测或编译期 `-mavx2`（会破坏「基线必可运行」）。原型阶段
  用 SSE2 无需检测；AVX2 要么运行时 dispatch（多版本代码），要么门控 + 检测。
* **收益不确定**：divpd ymm 在 Cascade Lake 吞吐是 xmm 的一半（256 位除法器半速），
  除法密集循环未必翻倍；且 ymm 上尾循环/水平求和更复杂。
* **寄存器压力**：ymm0–15 与 xmm 别名，提升池冲突要重新设计。
* 结论：先证明 SSE2 收益，AVX2 作为阶段 3 评估项，**实测决定是否值得**。

## 9. 分阶段路径

| 阶段 | 内容 | 状态 |
|---|---|---|
| **P1（本轮）** | spectral_norm eval_A reduction：SSE2 2-lane，保序累加，门控默认关，逐位一致 | 原型 |
| P2 | nbody 逐元素 map（无 reduction，纯 `a[i]=f(b[i],c[i])`）：模式更简，可上 2-lane 累加器 + 水平求和（先钉输出） | 待 |
| P3 | matmul 整数循环：SSE2 `pmulld`(SSE4.1)/`paddq` 或 AVX2；整数无舍入问题，但需处理 i64 乘积溢出守卫 | 待 |
| P4 | 通用化：上向量 IR op + 类型 + regalloc 支持，依赖/别名分析（类似 GCC vectorizer 的 data-ref 分析），循环版本化 + 运行时别名检查，多模式（map/reduction/scan） | 待 |
| P5 | AVX2/AVX-512 dispatch（CPU 检测），对齐 peel，更宽范围守卫 | 待 |

**纪律**：每阶段独立门控、独立验收（逐位/钉住输出 + 全套件 + asm 字节不变）。
别名/依赖判据宁保守跳过不可误向量化；浮点 reduction 改序必须先钉住基准输出。
通用自动向量化是 gcc/clang 数年的大功能，不在一轮内尝试。
