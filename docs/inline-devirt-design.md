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
（codegen_block.cpp emit_function），而 call.direct / FuncAddr / vtable 全部
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

- **P0 调用入口对齐修复**：.p2align 5 移到 .LfN 标签**之前**——调用不再
  穿 NOP 雪橇，函数体对齐不变（DSB 32B 粒度保留）。EH/LSDA 基准是 fname
  （.cfi_startproc 处），不受影响；ptag 双入口相对布局不变。
- **P1 小函数内联扩展**：放宽 inline_cost（48→96 条、12→20 块、预算
  24 位点/256 增长→40/640），加 PYADAMAS_INLINE_DBG 诊断。判据/RC/异常
  护栏与现有内联完全一致（can_inline、非生成器、无 handler、非递归）。
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

GCC 对 fib.cpp 的收益来自递归内联+部分迭代化；纯内联一层只能省调用计数
（~2/3），栈 mov 大头（61.7%）仍在——P0+P3 合计预期 fib −15~25%，
距 C++ 仍有差距（剩余是帧/栈流量，需 shrink-wrapping 或递归→迭代变换，
本轮不做）。nqueens place 体 ~200 条指令，展开一层代码 ~3× 且含 list
RC/handler 形态，收益不抵风险，**保守跳过**。

## 5. 实测（待填）

（实现后补：abbench 交替绑核 A/B、代码增长、各闸门结果。）
