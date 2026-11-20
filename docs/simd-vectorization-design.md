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

## 7. 实测（0.333 原型，Xeon Gold 6252 / Cascade Lake，-O2）

### spectral_norm flag-on vs off（N=600，输出钉住 811972）

perf stat min-of-7 交替（cycles / instructions）：

| | cycles | instructions |
|---|---|---|
| flag-off | 148,806,626 | 376,673,721 |
| flag-on | 34,112,939 | 85,393,946 |
| **比值** | **4.36×** | **4.41×** |

墙钟（Python perf_counter，15 轮交替，含 ~8ms 固定进程启动）：
flag-off min 56.55ms / median 66.81ms；flag-on min 24.65ms / median 26.82ms
→ **墙钟 min 2.29× / median 2.49×**。墙钟比 cycles 比值小，因为固定启动开销
不随计算缩小；纯计算 cycles 缩 4.36×。基线 PA 54.4ms vs Go 15.9 / C++ 15.1
（3.4×）→ flag-on 24.65ms ≈ 1.55× Go，差距大幅收窄（剩余主要是启动开销 +
保序 addsd 链，见下）。

### perf annotate 热点前后（pyadamas_fn_0__pa_main）

* **flag-off**：0 条 packed SIMD；标量 sd 占 45.5%——addsd 9.36+8.43%、
  divsd 8.42+3.08%、mulsd 5.84+3.27%（与 lead 0.331 profile 一致）。
* **flag-on**：37 条 packed SIMD 占 45.4%——divpd 7.46+6.57%、psrlq 6.13%、
  pshufd 5.92+5.47%、paddq 4.85%、movhlps 4.19+2.81%、mulpd…。标量 sd 降到
  38.4%，且**集中在保序累加链** addsd 17.39+13.98%（§3.4 刻意保留的逐位一致
  累加）。divsd/floordiv 展开/spill 流量基本消失。

### divpd 收益实测（不假设）

收益不是单靠 divpd：divsd→divpd（2 lane）+ floordiv 14 条整数展开→psrlq/pshufd/
paddq 几条 + 每元素 ~6 次栈往返→每 2 元素 1 次 movupd，三者叠加使 instructions
缩 4.41×、cycles 缩 4.36×。Cascade Lake divpd 吞吐实测足以支撑该收益（以
perf stat 为准，未做微基准假设）。**当前瓶颈转移到保序 addsd 链**（31%）——
阶段 2 若改 2-lane 累加 + 水平求和可再压，但须先钉住输出（§3.4）。

### 正确性与回归闸门

* flag-on 输出 == flag-off == CPython（spectral_norm 811972；nbody  likewise）。
* flag-off asm 对 0.331 基线**逐字节不变**：spectral_norm/nbody/matmul + 全部
  38 个热基准（realistic 6 + compare 9 + daily 23）asm cmp 全同 → flag-off 零回归
  （字节相同 ⇒ 二进制相同 ⇒ 性能相同）。
* difftest（O0/O2，flag-on）270/0；全套件 normal flag-off/on EXIT=0（288 ran ok）。
* 回归用例 283（向量化路径，N=5 奇数尾）/284（标量 fallback/边界/大范围/IndexError）
  normal+ASan flag-on/off 均 ran ok。
* check_size 无新增超限（codegen_simd.cpp 313 行；codegen_impl.h 拆片段后 1198 行）。
* ASan 全套件 **JOBS=1 flag-off/on 均 EXIT=0（290 ran ok）**。JOBS=8 高并行下偶发
  环境性 flake（网络超时/GC 时序/C 编译资源竞争，失败用例每轮不同：
  277_urllib/174_gc/cfi_unwind/cdecl_sample），基线同样偶发（cfi_unwind 在 ASan 下
  standalone 对基线也 rc=1）；失败用例 standalone 输出 mine==base，flag-off 生成
  asm 与基线逐字节相同（二进制差异仅来自不同 build 目录的 rt_asan.o，与 codegen 无关）。


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

## 10. 0.334 扩展实测：nbody（P2）+ matmul（P3）

> 状态：**已落地**（simd2 分支）。门控沿用 `PYADAMAS_SIMD`，默认 0。
> 三个新匹配器 + 共享判据，全部精确形状匹配（不做通用向量化器）。
> 文件：codegen_simd_int.cpp（196 行）/ codegen_simd_nbody.cpp（206）/
> codegen_simd_nbody_pair.cpp（397）/ codegen_simd.cpp 共享 helpers（461）。

### 10.1 matmul 整数内循环（P3，codegen_simd_int.cpp）

实际基准形态不是教科书 `sum += a[i][k]*b[k][j]`（k-reduction），而是循环重排后的
**j 内循环逐元素 map**：`for j: c[i*N+j] += aik * b[k*N+j]`（aik 外提）。
b/c 沿 j 连续 → movupd 一对取两元素，是理想的 2-lane 目标。17 条核心体精确匹配。

**i64 语义关键事实**：pyadamas 整数是 **i64 回绕**（gen_bin_int 直发 imulq/addq，
无溢出检查/ bigint 提升）。所以：

* `paddq` 与 `addq` 模 2^64 逐位一致，**任意值**成立，c 无需守卫。
* 乘法要重构 imulq 低 64 位。SSE2 无 packed 64×64→64；**pmulld（SSE4.1）是
  32×32→32 低半，直接用会算错**（任务 caution 点名）。选用 **pmuludq 组合**：

```
设 aik ∈ [0, 2^31)（入口守卫），b = b_hi*2^32 + b_lo（b_lo = 低32无符号）：
  aik*b mod 2^64 = aik*b_lo + ((aik*b_hi) mod 2^32) << 32
向量体（每 2 元素）：
  movapd b, t ; psrlq $32, t        ; t_lo32 = b_hi mod 2^32
  pmuludq aik_vec, t                ; aik * (b_hi mod 2^32)，< 2^63 精确
  psllq $32, t                      ; 交叉项 << 32（保留低32到高半）
  pmuludq aik_vec, b                ; aik * b_lo，< 2^63 精确
  paddq t, b                        ; prod（回绕 = imulq）
```

  对**任意 i64 b**（负数、≥2^32、INT64_MIN/MAX）逐位一致——b_hi 取 mod 2^32
  后无符号乘的低 32 与有符号乘积 mod 2^32 相同。守卫只约束 aik（循环不变量，
  入口一次）；b/c 元素**无逐元素守卫**（零开销）。aik<0 或 >=2^31 → 整段标量。
* **别名守卫**：b 与 c 必须不同对象（入口指针判等）。同对象时写 c[iN+j] 可能
  覆盖后续迭代读的 b[kN+j']（如 iN=kN+1），标量顺序语义被破坏 → 保守跳过。
  不同 list 对象 data 数组必不相交（各自 malloc，无 view）。c 读/写基址
  （IR 里是不同 vreg）运行时判等。
* 范围守卫 j>=0、base>=0、base+n<=len（addq 后 js 查溢出）、kind==0 →
  movupd 不越界、读裸 i64；不满足整段标量，IndexError/负索引时机不变。
  kind0 写无 RC（旧值是裸标量，标量快路径同样不 dec）→ movupd  store 等价。

**实测**（N=120，输出 43198080 == CPython）：cycles 34.46M→5.80M（**5.94×**），
instructions 89.2M→16.6M。标量每元素 ~49 条（3 组边界检查 + kind 检查 + 栈往返）
→ 向量每 2 元素 ~15 条。perf annotate：movupd 29.15% + pmuludq 14.18%。
墙钟 min 12.76→5.95ms（含启动）；同窗口 Go 6.45ms → **PA-on 3.10ms = 0.48× Go**。

### 10.2 nbody 位置更新 map（P2a，codegen_simd_nbody.cpp）

`for i in range(5): b[i7+k] += dt*b[i7+k+3]`（k=0,1,2），34 条核心体。
stride-7 逐元素 map：沿 i 轴取 (i,i+1) 两体，同分量相距 56 字节，SSE2 无 gather →
movsd×2 + unpcklpd 拼对。dt*vel + pos 逐 lane IEEE 同标量，元素独立无 reduction。
别名**结构判据**（无运行时守卫）：行距 7 > 最大偏移 5 → 两体行不相交；行内读
{0..5} 写 {0,1,2} 分量先读后写；跨迭代行不相交。守卫 7n<=len（imulq $7 + js）、
kind0、j>=0。只用 xmm0-4 → 不碰提升池，任何 fp 提升形态都安全。奇数 n 标量尾。

### 10.3 nbody pairwise 力循环（P2b，codegen_simd_nbody_pair.cpp）

113 条核心体精确匹配（dx/dy/dz → d2 → sqrt → mag → i 侧 3 减 + j 侧 3 加）。

**向量化轴选择：分量轴 [x,y]+z 标量，不是 j 对轴。** 任务原设想 j 对轴
（sqrtpd 一次算两体）需要 16 个 xmm（9 不变量 + 7 scratch）；advance 有 4 个
fp 提升槽 + dt 提升 vreg 占 xmm8-12，只剩 12 个，**放不下**。分量轴只需 11 个
（5 不变量 INV_XY/ZI/MIXY/ACCXY/ACCZ + 6 scratch），且步长 1 **无尾循环**。
代价：sqrt/div 延迟未摊销（仍标量 sqrtsd/divsd 每对一次）——实测收益仍 4.32×，
因为标量热点的大头是边界检查/槽 spill 流量而非 sqrt 吞吐（§10.5）。

**逐位一致构造**：

* dx/dy 成对 subpd；dz 标量 subsd。d2 **保序水平求和**：mulpd [dx²,dy²] →
  movhlps+addsd 得 dx²+dy²（= 标量第一次加）→ +dz²（= 标量第二次加）。
* sqrt：sqrtsd（与标量 fast path 同指令，IEEE 正确舍入）；**负数双保险守卫**
  comisd d2,0 → ja 冷路径（写回 acc → 标量尾，pa_math_sqrt 抛 ValueError 时机
  不变）。d2 是平方和理论上 >=0 或 NaN；NaN 时 comisd 无序不跳，sqrtsd(NaN)=NaN
  与 pa_math_sqrt(NaN) 一致。
* mag = dt/(d2*sqrt)：mulsd+divsd 标量，逐位同。
* **i 侧 acc_x/acc_y 是独立累加器**（b[i7+3]/b[i7+4] 两个不同内存位置）→
  subpd 双 lane 各自减，**无跨 lane 改序**；跨 j 的串行顺序逐轮保持（每向量
  迭代 = 1 标量 j 迭代）。acc_z 标量 subsd。acc 提升 xmm 跨轮持有，**正常退出
  与 sqrt 负数冷路径都先写回** b[i7+3..5] 再进 cond/标量尾。
* j 侧逐元素：读旧值对 + addpd + movupd 写回（xy），z 标量。

**别名结构判据**：守卫 j7>=i7+7（即 j>i，行距 7）→ i 行偏移 0..6 与 j 行偏移
0..6 不相交；i 侧写 {3,4,5} 与 i 侧读 {0,1,2,6} 不相交；j 侧读 {0,1,2,6} 与写
{3,4,5} 不相交；跨迭代 j 行不相交。→ b[i7+0..2,6] 循环不变，setup 一次装载
（标量每迭代重载是因为 LICM 无法证明不别名，结构判据补上）。i7>=0、7n<=len、
i7+6<len、kind0 守卫。向量循环零调用（sqrt 内联，冷路径跳标量尾才调用）。

**空闲 xmm 发现**：扫描 slot_reg_/vreg_reg_ 收集被提升占用的 xmm8-15，其余 +
xmm0-7 组成池；不变量优先占空闲高位，scratch 补 xmm0-7；池 <11 保守跳过。
绝不写提升寄存器（标量尾的 LoadSlot 别名/spill-reload 依赖它们）；标量体所有
fp 槽读都在体内写之后（dx/dy/dz/d2/mag 先 store 后 load），向量体不维护它们
也安全——尾循环第一条起就重新计算。

**实测**（N=50000，输出 -928502048 == flag-off == CPython；N=1 全 35 元素逐位
比对通过）：cycles 89.87M→20.81M（**4.32×**），instructions 200.7M→55.7M。
墙钟 min 34.66→7.91ms（4.38×）；同窗口 Go 12.17ms → **PA-on 8.49ms = 0.70× Go**。
perf annotate：99.07% 在 advance_1，packed 14.0% + 标量 sd 19.3%（保序 d2 链 +
sqrt/div + z 分量），其余是整数/控制/访存。

**开发期 bug 取证**：dz² 误发 `mulsd DZ, T2`（T2 低 lane 还是 dy² → 算成
dy²*dz）。零速度小测试（dy=dz=0）掩盖了它，全 3D 初值 N=1 即暴露 → 改
movapd+mulsd 自乘。教训：**浮点向量化回归必须用非退化初值**（287 用基准全初值）。

### 10.4 闸门汇总（0.334）

* flag-off asm 对 0.333 基线逐字节不变：38 热基准 cmp 全同。
* flag-on 只改 3 个基准（matmul 10 / nbody 35 / spectral_norm 36 条 packed），
  其余 35 个热基准 asm 不变。
* difftest flag-on 276/0；全套件 normal flag-off/on EXIT=0（294 ran ok）；
  ASan JOBS=1 flag-off/on EXIT=0；285-288 normal+ASan 双 flag ran ok；
  CPython 对拍 38 热基准双 flag 零差异；check_size 无新增超限。
* 回归 285（int 向量路径）/286（int fallback：负 aik、aik>=2^31、别名、kind、
  IndexError 时机、大 b 重构、溢出回绕）/287（nbody 向量路径 + 3/2/1 体）/
  288（nbody fallback：len<7n、kind、d2=0、负 dt）。O0/O1/O2 一致。

### 10.5 剩余路径（更新 §9）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1 | spectral_norm reduction | 0.333 已落地 |
| P2 | nbody 位置 map + pairwise 力（分量轴） | **0.334 已落地** |
| P3 | matmul 整数内循环（pmuludq 组合 + 守卫） | **0.334 已落地** |
| P2+ | nbody j 对轴 sqrtpd（摊销 sqrt/div 延迟）：需 16 xmm，先解决 fp 提升冲突（不变量 spill 到帧 / 提升池让位 / 帧扩展）。收益不确定（sqrt 链占比已小），**实测决定** | 待评估 |
| P3+ | matmul 负 aik 修正项 / 全 64 emulation（无守卫，9 条/2 元素）；iN/kN 索引 imul 强度削减已在 LICM 外提 | 待评估 |
| P4 | 通用化：向量 IR op + 类型 + regalloc + data-ref 分析 + 循环版本化 | 待 |
| P5 | AVX2/AVX-512 dispatch（CPU 检测）、对齐 peel | 待 |

spectral_norm 的保序 addsd 链（0.333 §3.4）与 nbody 的标量 sqrt/div 链是同类
剩余瓶颈：改序/加宽都必须先钉住输出（逐位或基准规模全验证），否则不启用。


## 11. 0.344 通用化（P4 tractable 核心）：codegen_vect.cpp

> 状态：**已落地**（vect1 分支）。门控 `PYADAMAS_VECT`，**默认 0**。与特化
> matcher（§1-10，认 spectral_norm/nbody/matmul 精确形状）**互补**：gen_block
> 先调特化 `try_simd_vectorize`，未命中再调通用 `try_vect_loop`。通用路径
> **不依赖精确指令形状**，基于 IR 结构 + 数据引用/别名分析识别任意可数 for
> 循环的逐元素 map / reduction（参照 gcc tree-vect / LLVM LoopVectorize 的
> 依赖分析 + 循环版本化思路）。本轮做 tractable 通用核心，非一轮做完所有。

### 11.1 架构：分析先行，发射在后

正确性命门要求「证不出保守跳过，一个字节不发」。因此 `try_vect_loop` 分两阶段：
**纯分析**（循环结构 → 槽分类 → 表达式 DAG 分类 → 别名/依赖 → 寄存器分配可行性
dry-run）全部通过后才**发射**。任何一步失败立即 return false（标量 fallback）。
另设 classify `visiting` 环检测：任何表达式环保守 Fail，**编译器绝不崩溃**
（开发期曾踩 read_before_store 顺序误判致自环栈溢出，见 §11.6）。

### 11.2 识别（结构判据，非基准形状）

* **可数 for**：cond 块 `load.slot sJ; cmp.lt sJ,n; branch body,end`、incr 块
  `load.slot sJ; bin.add sJ,1; store.slot sJ; jump cond`（步长 1）。所有
  `for j in range(n)` 都降成这个形状——是结构判据，不是认某个基准。
* **归纳变量 j 与副本槽**：sJ 及其「体块单写、写值来自 j-alias 装载、读全在
  写后、无外部读、不取地址」的副本槽（j-alias 不动点）。读 j-alias ≡ j。
* **表达式 DAG 分类**（memoized 递归）：InvF/InvI（常量/稳定槽/全局/跨块值，
  setup 广播）、InvPtr（list 基址，透明）、J、VecF/VecI（逐 lane 2-lane 值）、
  Acc（reduction 旧值）。float Bin add/sub/mul/div → packed SSE2（每 lane IEEE
  与标量同）；int Bin add/sub → paddq/psubq（模 2^64 与 addq/subq 逐位一致）。
* **仿射下标** j+c（c>=0 常量）：折叠进寻址位移 disp=c*8，不占向量整数运算。
  下标计算 bin.add(j,c) 标记 Skip（不是向量值）。
* **map**：`out[j+c]=f(a[j+c'],b[j+c''],不变量)`，list[float]/list[int] kind0。
* **reduction**：槽 sT 体块恰一写一读，写值=读值+expr（或 expr+读值），expr
  不含 acc。float **保序提取累加**（addsd 低 lane→movhlps→addsd 高 lane），
  与标量逐位一致（不做改序水平求和，输出零风险）；int **paddq 2-lane** 累加
  （模 2^64 结合律精确）+ 出口水平求和写回槽。
* **体局部临时槽**：单写、读全在写后、无外部读、非堆、不取地址 → SSA 替换
  （load≡所存值），跳过存储。用户变量 `t=a[i]*2; out[i]=t*t` 由此通用支持。
* **规范化数组基址**：非内联函数在体内 `load.slot s0` 装载 list 指针；把
  load.slot/load.global/Mov 解析到稳定来源（slot_mop/global_mop/src_mop），
  同槽/全局的不同 vreg 归并为同一逻辑数组（list_key）。

### 11.3 别名/依赖安全判据（宁保守跳过）

* 循环内唯一内存写 = 下标写（map）+ 槽写（栈帧，不与堆别名）；**无调用、
  无 store.global**（op 白名单）→ 向量循环零调用，list/data/不变量期间不可能
  被 append/realloc/GC 改写。
* **不同 list 对象**：data 数组必不相交（各自 malloc，无 view）→ 入口运行时
  两两判等守卫（**循环版本化**）；相等 → 保守标量。
* **同对象读+写**：只允许「全偏移 0 且读在写前」的逐元素原地 map
  （`a[i]=f(a[i])`，跨迭代/跨 lane 无依赖证明）。写偏移非 0、读在写后
  （`out[i]=...; s+=out[i]`）、跨迭代依赖（`a[i]=a[i+1]`）→ 跳过。
* **范围守卫**：每 list `len>=n+maxoff`（addq 后 js 查溢出）&& `kind==0`
  && `j>=0` && `j+1<n`。不满足整段标量，IndexError/负索引/kind 升级语义
  与时机不变（冷路径仍由 pa_list_get/pa_list_set 处理）。
* **循环变量外读**：副本槽外部有读（`for i: ...; print(i)`）→ 跳过（向量
  路径不维护副本槽终值）。计数器 sJ 用 **jmp cond 出口**精确等于标量（j==n），
  故 sJ 外部可读。

### 11.4 lowering 与尾循环

SSE2 2-lane（movupd/addpd/subpd/mulpd/divpd/paddq/psubq）。scratch 只用永不
提升的 GPR（rax/rcx/rdx/rsi/rdi）与 xmm0-7；j 原地 addq $2 在提升 GPR 或 rsi
工作+每轮写回；float acc 原地累加在提升 xmm8-15 或 scratch+每轮写回；data
指针每 list 常驻一个 scratch GPR；不变量广播 setup 一次常驻。寄存器分配
（持久不变量+acc 先占，临时值按末次使用线性扫描回收）在发射前 dry-run，
放不下保守跳过。**尾循环**：向量循环退出 `jmp cond`——j==n 直接退出（计数器
精确）；j==n-1 经 cond 回体块，入口 j+1<n 守卫失败 → gen_block 照常发射的
原标量体处理余 1。int reduction 出口先水平求和写回槽再 jmp cond（标量尾/
end 读最新）。

### 11.5 门控与闸门

`PYADAMAS_VECT=1` 开启（默认 0），写进模块缓存清单（manifest vect 行，
read/write/try_cache 比对）。flag-off `vect_func_ok` 第一判断返回，一个字节
不发。验收（vect1，基线 master d6aea9a）：

* flag-off asm 对 master **逐字节不变**：38 热基准（realistic 6 + compare 9
  + daily 23）cmp 全同。
* difftest（O0/O2，vect-on）**317/0**。
* 全套件 normal vect-on **EXIT=0（334 ran ok）**；normal flag-off EXIT=0（334）；
  **ASan vect-on JOBS=1 EXIT=0（334 ran ok）**。
* check_size 无新增超限文件（codegen_vect.cpp <1200；codegen_impl.h 保持 1200；
  其余超限为 master 既有）。
* 回归 344-349（非基准 map/reduction/原地/偏移/int map + 别名依赖负例 + 边界）
  flag-off/vect-on 均 ran ok，CPython 对拍零差异。
* spectral_norm/nbody/matmul flag-on 输出==flag-off（811972 / -928502048 /
  43198080 钉住）；性能不回归（min-of-9 比值 0.98/1.00/1.01×）。

### 11.6 性能实证（非基准）

非基准探针 `saxpy`（out[i]=a[i]*1.5+b[i]）+ `dot`（s+=a[i]*b[i]），
N=200000 × 30 轮，Xeon Gold 6252，min-of-9 交替绑核：

| | flag-off | vect-on | 比值 |
|---|---|---|---|
| 墙钟 min | 49.2ms | 26.1ms | **1.89×** |
| 墙钟 median | 49.5ms | 26.3ms | 1.88× |

checksum 逐位一致（5318446）。2-lane 理论上限 2×，保序 addsd 链使 reduction
部分略低于 2×，实测 1.89× 接近上限。**证明通用性**：任意浮点数组 map/reduction
循环（非 spectral_norm/nbody/matmul）向量化成功并加速。

开发期 bug 取证：read_before_store 按指令顺序扫描时，load 早于 store 而
store_pos 尚未置位 → 漏判 → 把「读在写前」的槽误判临时槽 → classify 自环 →
编译器栈溢出（50_licm/49_inline/298）。修复：min_load_pos 完整扫描后统一判
序 + classify visiting 环检测。教训：**槽读写序判定必须在全量扫描后做**。

### 11.7 剩余 / 通用化路径（更新 §9）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P4-core | 通用可数 for + float/int 逐元素 map + float/int reduction + 原地/偏移/临时槽 + 别名版本化 | **0.344 已落地** |
| P4+ | Convert int→float（float(i)，需 int32 范围守卫 + [j,j+1] 向量）；int 乘（pmuludq 组合 + 不变量因子守卫，§10.1 已证）；多 reduction；float Un Neg（xorpd 符号掩码） | 待 |
| P4++ | 写后读同数组（store-to-load forwarding）；循环变量外读（维护副本槽终值）；跨迭代依赖距离 >=VF 的安全判定；inv+j 行基址（matmul 形态通用化） | 待 |
| P5 | AVX2/AVX-512 dispatch（cpuid+xgetbv 检测，ymm 与提升池冲突需重设计）；对齐 peel；向量 IR op + 类型 + regalloc（彻底通用化） | 待评估 |

**负面结论**：本轮不做 AVX2（需运行时 CPU 检测 + ymm/xmm 提升池冲突重设计，
收益不确定，§8 已评估）；不做改序 float 水平求和（输出风险，保序已 1.89×
接近 2-lane 上限）；int 乘/Convert/多 reduction 复杂度高于本轮 tractable
核心，保守跳过并留路径。通用自动向量化是 gcc/clang 数年的大功能，本轮交付
的是**结构判据驱动、别名/依赖保守、门控默认关**的安全通用核心。
