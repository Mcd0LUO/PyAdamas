# pyadamas 编译吞吐优化（编译器自身运行速度）

分支 speed1（worktree /tmp/wt-speed1，基线 master 9b4a6f9 / 0.341）。

**口径**：pyadamas 编译用户程序（`pyadamas asm -O2`）的墙钟吞吐，不是
生成码的运行速度，也不是编译器自身的构建时间（构建时间见
docs/compile-speed.md）。**硬不变量**：生成 asm 逐字节不变。

## 0. 测量环境与语料

- 机器 Intel Xeon Gold 6252 整机 96 逻辑核，本会话 affinity 24-47,72-95；
  多租户（其它代理并行构建/测试），测量窗口 load 6~15 起伏。
- 全部测量用 `taskset -c 24` 绑单核，避免迁移噪声。
- 语料 = tests/run/*.py + bench/*/*.py，共 **413 个文件**（排除需要实参的
  bench/compare/run_compare.py），每轮把语料完整编译一遍输出到 /dev/null；
  一次测量跑 2 遍，base / exp **交替轮转**取中位抗负载漂移。
- 单文件重负载样本：tests/run/234_re_opt.py（单函数 626 块 / 2150 vreg），
  基线单次编译 1.44s，是语料里唯一的大函数压力源（占基线总时间 ~17%）。

## 1. 热点定位（perf，语料 ×2 遍，-F 1999 -g）

基线（master 9b4a6f9，release -O3）：

| 符号 | 占比 |
|---|---|
| compute_dom（opt_loop 支配集） | **20.2%** |
| assign_vreg_slots（regalloc） | **14.2%** |
| unordered_map<unsigned,int> 插入（_M_insert_unique_node + emplace_uniq） | **14.0%** |
| _int_malloc / free 系 | 10.2% |
| licm_function | 2.9% |
| emit_function | 2.6% |

对 234_re_opt.py 单独 profile：compute_dom **43%**、assign_vreg_slots 18%、
哈希插入 12.5%。即：**编译时间被「大函数上的 O(n²) 数据结构」主导**，
不是字符串格式化/前端。

HEAD（52f8b96，同口径同语料）：

| 符号 | 占比 |
|---|---|
| emit_function | 5.7% |
| licm_function | 3.4% |
| _int_malloc / free 系 | ~7% |
| compute_dom | 1.6% |
| block_loop_depth | 1.2% |
| assign_vreg_slots | 1.1% |

## 2. 逐块改动（每块都独立构建 + 全语料 asm 对照 + 套件/difftest）

共同原则：**只换"同一组 0/1 或同一个索引用什么容器存"**，判定逻辑、
迭代顺序、不动点语义逐位保持；所以 asm 逐字节不变，只是变快。

| commit | 改动 | 基线热点 → 之后 |
|---|---|---|
| 7872bab | compute_dom 的 dom[b]：每块 nb 字节 char 向量 → 64 位字位集（同一不动点、同一交集顺序、同一全 1 初始化） | 20.2% → 1.6% |
| 57a9080 / 0ffb6b6 | assign_vreg_slots 的 blk_use/blk_def/live_in/live_out → 位集（新增 src/block_bitset.h）；assign_block_temp_regs 的三张 per-block unordered_map<VReg,…> → 定长数组 + 脏项复位 | 14.2% → 1.1%；哈希插入 15% → 0 |
| a30675d | hoist_loop / version_bounds_loop 的 def_block/multi_def/used_outside：每个循环头重建 unordered_map/set → 定长数组 | — |
| 118c333 | slots_needing_zero 的 must 数据流 → 位集；指令注释 Inst::text_into 直写复用缓冲（原来每指令 3 次分配）、vreg_str 编号缓存、PYADAMAS_IR_CHECK 的 getenv 只读一次 | — |
| 52f8b96 | vreg_is_rc_mark：每个 vreg 扫全函数（O(n²)）→ 入口一次扫描打标 | 3.2%（srcline）→ 0 |

去掉无读者的 IR 索引（ir.cpp rebuild_use_def 不再重建 inst_by_id /
def_of / uses_of）：包在 57a9080 里。这三张表自 0.3xx 的 DCE 改线性计数后
已无读者（全仓库 grep 只有写入），但重建是逐指令哈希插入 + 每结点 malloc。
**集成注意**：若别的分支新增了对 def_of/uses_of/inst_by_id 的读取，需要
改成惰性显式重建入口（成员已移除，编译期即报错，不会静默拿到空表）。

## 3. 编译吞吐 A/B（交替轮转）

语料 ×2 遍，base/exp 交替 5 轮（load 6~15）：

| 轮 | base (s) | exp (s) |
|---|---|---|
| 1 | 11.36 | 5.98 |
| 2 | 12.57 | 7.81 |
| 3 | 13.34 | 7.69 |
| 4 | 13.50 | 6.87 |
| 5 | 11.46 | 5.99 |
| **中位** | **12.57** | **6.87** |

**提速 -45.3%**（单遍 5.68s → 2.99s，约 1.83×）。

单文件重负载 234_re_opt.py：**1.44s → 0.14s（10.3×）**，5 次重复稳定。

## 4. 闸门

- **生成 asm 逐字节不变**：413 文件全语料 `pyadamas asm -O2` 输出 sha256
  逐文件对照 **全同**；另对 40 个最重文件做 `cmp` 直接对比 **40/40 相同**；
  ir dump（`pyadamas ir -O2`）60 文件逐字节相同。
- **套件**：normal 327 ran ok，EXIT=0；ASan 套件 327 ran ok，EXIT=0。
- **difftest**：309 用例，0 不一致，EXIT=0。
- **check_size**：无新增超限文件（现存 5 个超限文件是基线固有）。
- 构建：`-DCMAKE_BUILD_TYPE=Release`（-O3 -DNDEBUG）+ Ninja -j18，无警告
  （项目 -Wall -Wextra -Werror）。

## 5. 负面结论 / 未做

1. **"注释惰性化"不可做**：IR 文本注释是**生成 asm 的一部分**（每条指令
   一条 `	# ...`），任何"只在 dump 时才算"的方案都会改 asm 字节。能做的
   只有把"算同样文本"变便宜（format_to 复用缓冲 + vreg_str 缓存），
   实测 vformat 热点 1.0% → 0.9%，收益有限。
2. **malloc/free 仍占 ~7%**：主要是每条指令一个注释 MItem（std::string）
   与 MItem/MInstr 池增长。彻底解决要给 MItem::text 换 arena/偏移表示，
   会动 mach_instr.h + mach_emit.cpp 的公共结构，收益 ~2-3%，本轮不做。
3. **emit_function 自身 5.7%**：热点分散在几十条标量存储（per-slot 循环、
   tag 字分配、缓存构建），没有单一 O(n²)；进一步要重排分配阶段，收益/
   风险比不划算。
4. **compute_dom 还能再快**：改成 Cooper-Harvey-Kennedy 的 idom + 树
   前缀 OR 是 O(nb·words) 级别，比现在的位集不动点更省；但要重写算法并
   靠 asm 对照兜底，剩余占比只有 1.6%，本轮不做。
5. **block_loop_depth（1.2%）**：每块做一次 BFS（O(nb·edges)），可换 SCC/
   支配树，但占比小。
6. 前端（词法/语法/类型检查）不是热点：perf 里前端符号合计 <3%。

## 6. 集成建议（给 lead）

- 改动集中在 opt_loop.cpp / ir.cpp / ir_types.h / codegen_regalloc.cpp /
  codegen_block.cpp / codegen_impl.h / codegen_ops.cpp + 新增
  src/block_bitset.h。域隔离上不碰 codegen_simd*（vect1）、rt_big/Big
  （big1）、rt_file/rt_eh/rt_class（lang1）。
- ir_types.h 删了三个死索引成员：与别的分支冲突时，保留删除（或在对方
  读取处补访问器）。
- 所有改动都不改生成码，理论上不需要重跑生成码基准；但 regalloc/位集
  改动触及分配路径，**建议集成后再跑一次 difftest + 40 文件 cmp**。
