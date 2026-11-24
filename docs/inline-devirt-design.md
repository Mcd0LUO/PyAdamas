# 去虚拟化深化 + 函数内联扩展（0.337 预留，dv1 分支）

> 门控 `PYADAMAS_INLINE_OPT=0/1` **默认 0**。flag-off 生成 asm 逐字节不变。
> 测量先行：perf annotate 取证（基线 615fbe0 / 0.336，绑核 24，-O2）→ 选优化 →
> abbench 交替 A/B 定生死。本文档随实现更新实测节。

## 1. 热点取证（基线 0.336，perf record -F 4999，taskset -c 24）

基线二进制由 worktree 615fbe0 构建产出；三案 asm 与 build/lead（0.336）
`pyadamas asm` 输出 **cmp 逐字节一致**（fib/nqueens/obj_dispatch），取证等价。

### 1.1 fib（85.7% 周期在 pyadamas_fn_1fib_1）

perf annotate 逐指令占比（局部周期）：

| 类别 | 指令 | 占比合计 |
|---|---|---|
| **入口对齐 NOP 雪橇** | call .Lf1 落在 .p2align 5 填充上（0x3a0b/0x3a15 两条长 nop） | **8.4%** |
| 帧建立/拆卸 | push rbx 2.2 + sub rsp 7.1 + mov rdi,rbx 5.6；两处 add/pop/ret ≈ 6.6 | ~21.5% |
| call/ret | 2×call 5.7 + ret ~1.9 | ~7.6% |
| 栈 mov（实参/结果/中转） | 0x28/0x30/0x38/0x40 槽 store+reload 共 10 条 | **~61.7%** |
| 算术/比较 | cmp/jl/sub/add | ~8% |

**关键发现（新）**：函数入口标签 `.LfN` 发在 `.p2align 5` **之前**
（backend/block.cpp emit_function），而 call.direct / FuncAddr / vtable 全部
引用 `.LfN` —— 每次调用都先执行最多 31 字节对齐 NOP 再进函数体。fib 每次
递归调用白跑 21 字节 NOP，perf 实测 **8.4% 周期**落在 NOP 上（含 call _skid）。
nqueens 递归调用同样穿过 10 字节 NOP（0x3af6→0x3b00，0.48%）。

fib IR：f1 共 3 块 ~17 条指令、单 int 槽、无 handler、无 CallRt、仅 2 个
call.direct 自调用 —— 是「纯标量自递归」的最简形态。

### 1.2 nqueens（98.9% 周期在 pyadamas_fn_1place_1）

- 帧：6×push + sub rsp,0x148；递归调用点（call 3af6，穿 NOP）周边是
  5 实参装填（row+1/n/cols/d1/d2 从槽重载）。
- 热主体是 **list 下标读写**：cols[c]/d1[row+c]/d2[row-c+n] 每次
  「长度装载+cmp+分支+buf 装载+元素装载」~6 指令，写入侧还有 kind 分派
  （kind1/2/3 三路）+ 旧值 rc_dec_unknown。单是 3bc2 一条 mov 占 14.6%
  （d1 下标计算链的 skid 汇聚点）。
- **没有可内联的小 IR callee**：place 自递归（体 ~200 条指令），count 只被
  调一次且体超 48 条；其余全是运行时调用（pa_list_get/set/rc_dec）。
  小函数内联扩展对 nqueens **无作用面**；递归展开体太大（代码爆炸风险），
  保守跳过（§4 评估）。

### 1.3 obj_dispatch（99.5% 周期在 __pa_main 内层循环）

**0.294 守卫式叶子内联已生效**：`s.area()`（vcall[0]，静态类型 Shape）
在代码生成期展开为 3 路 vtable 守卫（Shape→const 0、Rect→w*h、Square 共享
Rect 函数体），命中路径**无 call**（imul 0x10(%rdi),%rax 等直接发射）。
annotate 证实守卫链 ~10%（lea/cmp/je ×3 + vtable 装载），其余头部是：

- 循环变量 s 的 **rc_inc/rc_dec + GC 相位检查**（mov %rax,-0x30(%rdi) 8.7%、
  reload 6.5%、test 5.2%、pa_gc_phase_fast cmp 5.4%…合计 ~25%+）；
- 全局 s/total 的 store/load（~10%）；循环控制 ~8%。

**结论**：obj_dispatch 的虚调用开销已被 0.294 消除，剩余头部是 RC/全局
流量（借用语义迭代方向，HANDOFF §5.1，不在本轮范围）。devirt 深化
（守卫直调）对本案**预期收益 ≈ 0**——叶子内联比直调更彻底。本轮仍实现
「非叶子小方法守卫直调」作为通用 OO 能力（其它用例可能受益），如实测量。

## 2. 本轮实现（全部 PYADAMAS_INLINE_OPT 门控，默认关）

- **P0 调用入口对齐修复（v2，布局保持式）**：.p2align 填充字节**原样保留**，
  只在对齐后加 .LfNe 别名；call/FuncAddr/vtable/静态闭包/shim 经 entry_ref()
  改引 .LfNe —— 调用跳过 NOP 雪橇，但 .text 布局与基线逐字节同位（只改分支
  目标位移）。EH/LSDA 基准 fname（.cfi_startproc 处）不变；ptag 双入口相对
  布局不变。**v1（挪对齐到 .LfN 前）已撤回**：binary_trees 实测 DSB uops
  −32%、cycles +4.2%（布局远端效应，HANDOFF §6.2 再验证）；v2 高 BURST
  对照回到噪声带内。
- **P1 小函数内联扩展（负面结论，判据不放宽）**：曾试 48→96 条/12→20 块/
  预算 40 位点/640 增长——realistic+compare 语料无可测收益（各案 ±1% 噪声），
  且 nqueens 的 count(80 条)内联进 __pa_main 使热函数 place 布局位移，隔离
  实验 +1.8%（去掉放宽即 +0.1%）。**回退原判据 48/12/24/256**，保留
  PYADAMAS_INLINE_DBG 诊断（[inline]/[unroll] 决策追踪）。
- **P2 devirt 深化**：gen_guarded_vcall 在整数叶子匹配失败时，对「小方法」
  （≤32 条指令、≤4 块、无 handler/eh_regions、非生成器、无闭包环境）
  发射守卫命中 → **直接调用**（call .LfN），守卫失败回退原 vcall。
  语义不变：慢路径原样保留，EH 区域覆盖整个调用点。
- **P3 纯标量自递归展开（fib，严格界）**：仅当函数 ≤24 条指令、≤3 块、
  全槽 Int/Bool、无 handler/eh_regions、非生成器、无闭包、体内除自调用外
  **无任何其它调用**（不可能抛 Python 异常 → traceback 语义透明）时，
  把自调用点内联一层（克隆体内自调用保留为 call）。代码增长 ≤3×、
  每函数只展开一次。fib 调用次数 ≈ 降至 1/3（每次调用做 3 个节点的工作）。

## 3. 正确性护栏

- 生成器/协程：callee.is_generator 与 fn.is_generator 一律不内联（既有护栏）。
- 异常：callee.has_handlers / eh_regions 非空不内联；P3 额外要求体内无
  任何可抛异常的运行时调用。traceback 钉子 92/93/94/119 复跑。
- devirt 守卫比较**实际 vtable 地址**（不按类名/静态类型断言），失败回退
  原 vcall —— 与 0.294 同构，运行期类替换/继承全覆盖。
- manifest 缓存键加 inlineopt（切开关必然失效，A/B 不命中旧产物）。

## 4. 递归内联评估（如实）

GCC 对 fib.cpp 的收益来自递归内联+部分迭代化；纯内联一层省调用计数
（~2/3）+ 内联边界的实参/结果栈往返。实测 P0+P3 合计 fib **−29%**
（超出 §1 预估的 −15~25%：内联边界消掉的栈 mov 比预估多），对 C++
比值从 ~2.4× 收窄到 ~1.7×（同相位交替；剩余差距是帧/栈流量，需
shrink-wrapping 或递归→迭代变换，本轮不做）。

**边界如实记录**：P3 只接「抛不出 Python 异常」的纯标量自递归（白名单
指令集）。nqueens place 体 ~200 条指令、含 list 下标 RC 与运行时调用，
超界不展开——收益不抵代码爆炸/异常语义风险，**保守跳过**。含 handler
的递归（153 的 rec）、heap 实参递归（list_sum）、自调用点 >2（ackermann）
一律不展开（303 负例钉住）。栈深度减半：极端深度下栈溢出崩溃点后移
（pyadamas 无 RecursionError 限制，CPython 对拍用例深度 ≤500 不受影响），
opt-in 门控默认关。

## 5. 实测

### 5.1 性能（交替 A/B 绑核 24，打乱+奇偶交换，中位数/p25）

最终全量（13 轮 BURST=10，flag-on vs off，P1 已回退）：

| 用例 | off ms | on ms | 变化 | p25 |
|---|---|---|---|---|
| **fib** | 23.74 | 17.10 | **−27.9%** | −27.5% |
| nqueens | 103.65 | 103.87 | +0.2% | +0.5% |
| obj_dispatch | 335.78 | 335.70 | −0.0% | +0.1% |
| rec_memo | 218.09 | 215.65 | −1.1% | −0.8% |
| binary_trees | 7.73 | 7.80 | +0.9% | −1.9% |
| fannkuch | 10.46 | 10.85 | +3.8%* | +4.4%* |
| fasta | 7.55 | 7.79 | +3.1%* | +3.6%* |
| 其余 8 案 | — | — | ±0.7% 内 | — |
| 合计 | 1811.97 | 1805.15 | −0.4% | — |

fib 高 BURST 复核（13 轮 BURST=20）：22.74 → 16.13 ms（**−29.0%**，p25 −28.5%）。
对 C++ 比值：同相位交替 ~2.4× → ~1.7×（g++ 15.2 -O2 本机 fib.cpp；绝对值随
多租户负载相位漂移，只信交替比值）。

*短用例（<15ms）BURST=10 相位噪声：对照实验（两侧同一二进制）噪声底 ±2.5%；
fannkuch/fasta 高 BURST=20 复测 +0.4%/−1.1%（噪声带内）。这两案 flag-on 只有
P0-v2 入口别名（布局逐字节同位、只改分支目标），无 P1/P2/P3 作用面，
理论上不可能负收益——复测确认是测量噪声。

### 5.2 代码增长（text 段）

| 用例 | off | on | 增量 |
|---|---|---|---|
| fib | 59801 | 59985 | +184 B（+0.3%，P3 展开） |
| nqueens | 67878 | 67878 | **0**（P1 回退后无内联） |
| obj_dispatch | 72766 | 72830 | +64 B（+0.09%，P2 冷 __init__） |

### 5.3 闸门（全过）

- flag-off asm 逐字节不变：**92/92 cmp**（compare 9 + realistic 6 + builtins 54 + daily 23）。
- difftest flag-on（O0/O2）：**287/0**。
- 套件 normal flag-on/off：**305 ran ok × 2，EXIT=0**；ASan flag-on **JOBS=1：305 ran ok，EXIT=0**。
- 语料零差异（flag-on vs off，输出+rc）：tests/run + 全部基准 **0 DIFF**。
- ASan detect_leaks=1：13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr /
  119_gen_except_suspend / 92 / 93 / 94 / 300-303 **全 0**。
- 异常/生成器/vthread 钉子：92/93/94/119/153_traceback flag-on ran ok；220-229 全过。
- CPython 对拍：300-303 expect 钉自真实 CPython；fib/nqueens/obj_dispatch 输出一致。
- check_size：无新增超限（codegen_block 1200 / backend/impl.h 1200 / codegen_call 1162；
  已知 5 超限不变）。
- manifest inlineopt 缓存键：切开关必然失效（实测验证）。
- 逐块独立构建：c59c343 快照独立 worktree 构建 EXIT=0。

### 5.4 隔离实验（P1 负面结论证据）

nqueens 三向交替（13 轮 BURST=20）：off vs 全包（含 P1 放宽）= **+1.8%**（p25 +2.0%）；
off vs 去 P1（其余同）= **+0.1%**（p25 −0.1%）。count(80 条)内联进 __pa_main 使
热函数 place 布局位移是回归源 → P1 判据回退 48/12/24/256。
