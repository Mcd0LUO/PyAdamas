# 边界检查消除（ranged loop versioning）：尝试记录与阻塞点

> 状态：**已实现分析部分，克隆阶段有语义错误（fannkuch 死循环），代码已回退。**
> 日期：2026-11。
> 相关提交：0165148（IndexStoreUnchecked op + 上界测量）。

## 0. 上界（这条路的收益天花板）

用 PYADAMAS_NO_BOUNDS=1 强制所有索引访问无检查，实测：

    bench            有检查    无检查    变化
    matmul              8ms       7ms    -12%
    nbody              18ms      16ms    -11%
    nqueens            77ms      72ms     -6%
    spectral_norm      30ms      29ms     -3%
    mandelbrot         13ms      13ms      0%
    fib                12ms      12ms      0%

binary_trees / fasta / fannkuch 在该模式下 SIGSEGV —— 它们合法使用负下标
（Python 回绕）。所以真实实现必须**保留「下标可能为负」时的检查**。

结论：这条路上限约 **nbody -11% / matmul -12%**，加权总计约 -2%。
nbody 距 Go 仍是 3.30x，边界检查只能解释一小部分。

## 1. 为什么已有 version_bounds 覆盖不到

`version_bounds_loop` 只处理**循环不变下标**的 IndexLoad（本质是 LICM：
把 a[k] 提到循环外 + 一次检查）。函数开头还有一条一刀切的
`case IrOp::IndexStore: return false;` —— 因为外提 load 可能被循环内的写
别名污染。而真实热循环（nbody 有 44 个 index.set）必然含写，于是整个
循环的版本化被放弃，83 个访问全部保留检查。

## 2. 实现思路（已写通的部分）

在 `version_bounds_loop` 上增量扩展，而不是另写一套（复用已有的循环
克隆 / 前驱重定向机制）：

1. **含 IndexStore 时不再放弃**：改为 `has_index_store = true`，
   清空「外提候选」（放弃外提，别名不安全），但仍可做区间版本化。
2. **规范归纳变量识别**：头块 `cmp.lt (load.slot S), bound` + branch；
   循环内恰一处 `store.slot S, (load.slot S) + 1`；进循环前 S = 0。
   → 循环体里 S 属于 [0, bound-1]。
3. **仿射下标分析**：idx = a*S + off（a 常量 >= 0，off 常量或一个不变 vreg）。
4. **循环入口守卫**：每组 (base, a) 一次 `a*(bound-1)+off < len(base)`
   （bound 常量时上界是编译期常量），负偏移额外守卫 `off >= 0`。
5. **克隆循环体**：把可证的 IndexLoad/IndexStore 换成 unchecked 变体。

## 3. 踩到的三个坑（按发现顺序）

### 坑 1：增量常量被 LICM 提到前置块

在「当前块」里找 `const.int 1` 找不到，导致 IV 识别失败（iv_slot = -2）。
修法：建**全函数定义表** all_def，而不是只看本块。

### 坑 2：系数常量同理

`bin.mul %32, %33` 里的 %33（=7）被提到循环外，affine 看不到。
修法：affine 在 loop_def 里找不到时，回查 all_def，是 ConstInt 就按常量算。

### 坑 3（关键）：**下标读的不是 IV 槽，是一个「复制槽」**

nbody 的实际形态：

    bb10:
      %24 = load.slot s13      ; j（IV）
      store.slot s7, %24       ; ← j 被复制到 s7
      ...
      %32 = load.slot s7       ; ← 下标链读 s7，不是 s13
      %34 = bin.mul %32, %33   ; j*7
      %35 = index.get %31[%34]

不追踪这条复制链，nbody 的 83 个候选里 **0 个**能通过仿射分析（实测
aff_fail 等于总数）。

修法：`copy_slots` 不动点传播 —— 槽 T 满足「循环内唯一一次 store，值来自
load.slot(IV 或已知复制槽)，且该 store 所在块支配所有读 T 的块」。
修复后 i 循环（h=5）拿到 12 个候选并通过守卫。

## 4. 阻塞点：克隆阶段的语义错误

守卫正确生成了（[rv] h=5 ranged=12 bound=11），但生成的程序错误：

- IR 里 checked 索引从 83 降到 35（不该降这么多）；
- **fannkuch 死循环**（编译产物运行不终止）；
- unchecked 计数为 0，说明转换没落到预期的指令上。

因此判断问题在**克隆 + 重映射**这一段，可能的原因（下一步排查顺序）：

1. `cand_dsts` 跳过与外提候选混用：含写时 cands 被清空，但
   `cand_dsts` 是否也清干净（已 clear，需确认 hoisted 语义没有残留）。
2. `proved_ranged` 的 (block, pos) 键用的是**克隆前**的下标，而克隆里
   我改成按下标迭代 —— 需要确认两者一致。
3. 复制槽的 affine 结论在克隆里仍成立吗：克隆保留了原 store/load，
   但 base 重定向表 `ranged_base_redirect` 只处理 base，不处理复制槽。
4. 守卫里 `load.slot` 外提的 base 是 t_unknown() 类型，可能影响后续
   类型相关的 codegen 分支。

## 5. 回退与后续

`src/opt_loop.cpp` 的改动已 `git checkout` 回退，工作区干净，
9/9 基准正确。`IrOp::IndexStoreUnchecked` 与其代码生成保留（0165148），
它是这个优化必需的地基，单独看也是完整的。

要继续做的话，建议先在 clone 之后加一个 **IR 校验**（`PYADAMAS_IR_CHECK`
已有类似机制：检查每个 vreg 有定义、块目标合法），再做 diff 对比：
对同一个 benchmark 分别用 `-O1` 和 `-O2` 跑 difftest（已有工具，
5.6s 一轮），把错误缩小到单个函数再单步克隆。

## 6. 复现

```bash
PYADAMAS_NO_BOUNDS=1 ./build/release/pyadamas run bench/compare/nbody.py  # 上界
```
