# 堆槽进寄存器：GC 障碍不存在，但池子已经饱和

> 状态：实验完成，结论为**负收益**，改动已回退。
> 日期：2026-11。
> 相关提交：c56e068（峰值压力诊断）。

## 0. 一句话结论

「堆槽（list/str/dict）进不了寄存器是因为 GC 靠栈槽地址找根，需要栈映射」
这个假设**是错的** —— GC 早就用 `setjmp` 扫描 callee-saved 寄存器了。
真正拦住堆槽提升的是编译期一行无条件排除，而把它放行之后**性能反而降 5.5%**，
因为 6 个 GPR 的池子已经饱和，给堆槽寄存器等于从标量槽手里抢。

## 1. 运行时早就支持了

`runtime/rt_core.c`：

```c
// setjmp 会把 callee-saved 寄存器存进 jmp_buf；生成代码里常驻寄存器的
// 堆指针（槽位提升、全局 vreg）就靠这一段兜住。
static void gc_scan_registers(void) {
    jmp_buf regs;
    if (_setjmp(regs) == 0) gc_scan_range(&regs, (char*)&regs + sizeof regs);
}
```

所以「寄存器里的堆指针 GC 看不见」这个前提不成立。注释里写的
「槽位提升（heap pointer）」正是这个用途。

## 2. 真正的障碍：拥有型 vs 借用型

`lower/impl_slots.cpp:260` 把形参槽设为 `slot_owned[p.slot] = false`，
即**形参是借用引用**，不需要 dec。

- **拥有型**堆槽：异常展开要按栈槽地址读值来 dec。值在寄存器里 -> 展开读
  到旧值 -> 泄漏。必须整函数留栈。
- **借用型**堆槽：不需要 dec；GC 标记由 `gc_scan_registers()` 兜住。
  **进寄存器是安全的。**

而 `backend/regalloc.cpp` 原来无条件排除所有堆槽，并在第一遍
`gc_scan_slot_` 里把所有堆槽标记为「必须留内存」，于是借用型也被殃及。

## 3. 实验：放行借用型堆槽

两处改动：

1. 第一遍 `gc_scan_slot_`：只标记「拥有型」堆槽 + unknown 槽；
2. 候选筛选：堆槽若 `slot_owned == false` 则允许提升。

提升确实发生了：

```
nbody.advance_1:  15 cand slots（原 14），int 7/9 promoted
nqueens.place_1:   9 cand slots（原  8），int 6/9 promoted
```

正确性：9/9 基准通过（含 GC 压力测试）。

## 4. 性能：净负 -5.5%

| bench | 改动前 | 改动后 |
|---|---|---|
| fib | 12ms | 12ms |
| binary_trees | 2ms | 2ms |
| fasta | 2ms | 2ms |
| mandelbrot | 13ms | 13ms |
| **nqueens** | **75ms** | **85ms (+13%)** |
| fannkuch | 6ms | 5ms |
| matmul | 7ms | 7ms |
| spectral_norm | 29ms | 29ms |
| nbody | 18ms | 18ms |
| **TOTAL** | **164ms** | **173ms (+5.5%)** |

只有 nqueens 变化明显，而且是变慢。

## 5. 为什么会变慢：还是池子饱和

`PYADAMAS_LI_STATS` 测到的精确峰值压力：

```
函数                    槽数  精确峰值  包络峰值   GPR
nbody.advance_1          14      8        11        6
nbody.energy_2           10      7         8        6
spectral_norm.__pa_main  14      6        12        6
matmul.run_2             20      8        17        6
nqueens.place_1           8      7         8        6
mandelbrot.run_1         13      7        11        6
```

**所有热函数的精确峰值就是 6-8，池只有 6 个 GPR。** 无论把寄存器给谁，
总有槽要溢出。给堆槽腾一个寄存器，就是让一个标量槽降级；堆槽每次索引
省 1 条 load，被降级的标量可能每次访问多付 1-2 条。nqueens 就是净亏。

## 6. 对「栈映射」路线的影响

这个实验说明：**为了「解锁堆槽进寄存器」而做栈映射，是拿不到收益的。**
栈映射能解掉的是「GC 看不见寄存器里的堆指针」，而这个障碍本来就不存在
（`gc_scan_registers` 已经兜住）；剩下的障碍是异常展开的 dec，那只需要
「拥有型堆槽留栈」，代价小得多，也不需要栈映射。

栈映射仍有价值，但价值在别处：

1. **精确根集合**，让 GC 少扫无关内存（当前是保守扫 callee-saved 寄存器
   + 帧记录里登记的地址）。这是 GC 暂停时间的问题，不是吐吞量的问题。
2. 未来若要把 **frame 记录本身去掉**（当前每帧压帧有固定开销：写 6+ 个
   字），栈映射是前提。

而 nbody（3.30×）、spectral_norm（2.58×）慢的**真正原因不是寄存器不够**，
是每次 `a[i]` 都要重新做「基址取、长度取、边界比较、可能 call」这套
序列，以及浮点值在栈和 xmm 之间反复搬运。这两件事都不需要更多寄存器：

- **边界检查消除**：`for i in range(len(a))` 之后 `a[i]` 的检查可证伪，
  nbody.advance 里有 42 组 `cmp`+`jb`；
- **循环不变量提升**：把 `len(a)`、`a.data` 提到循环外（当前 LICM 对
  slot 覆盖不全）。

## 7. 可复现

```bash
PYADAMAS_LI_STATS=1 ./build/release/pyadamas build -O2 bench/compare/nbody.py -o /tmp/x
# 看 "peak: precise=8 env=11"
```
