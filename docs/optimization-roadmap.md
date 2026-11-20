# pyadamas 性能优化自主迭代 Roadmap

> 用户授权（睡眠期间）：**自主迭代，单代理槽位**——simd 完成后自动派下一轮，
> 等代理→严审闸门→集成→评估收益→派下一轮，持续不停。本文件是决策依据。
> 每轮独立 worktree/分支、门控默认关、非优化路径 asm 逐字节不变、全套闸门、
> CHANGELOG 版本递增、lead 严审后集成推送。负面结果（收益不抵复杂度）记录跳过。

## 基线（0.332，四语言 min-of-5，负载窗口比值为准）

| 用例 | PyAdamas | Go | C++ | PA/Go | 瓶颈类型 |
|---|---|---|---|---|---|
| spectral_norm | 54.4 | 15.9 | 15.1 | **3.4×** | 浮点循环（0 SIMD） |
| matmul | 14.2 | 6.0 | 5.9 | **2.4×** | 整数循环 |
| nbody | 33.2 | 16.1 | 16.3 | **2.1×** | 浮点循环+sqrt |
| nqueens | 99.5 | 63.2 | 61.9 | 1.6× | 回溯/整数+递归 |
| fib | 19.4 | 16.9 | 7.3 | 1.15× | 递归 |
| mandelbrot | 18.4 | 16.8 | 14.2 | 1.1× | 浮点迭代 |
| fannkuch | 10.9 | 10.9 | 4.5 | 1.0× | 整数排列 |
| binary_trees | 5.0 | 4.9 | 3.4 | 1.0× | 分配（RC+池） |
| fasta | 4.7 | 5.0 | 3.5 | 0.94× | 分配/整数（胜 Go） |

结论：数值核（浮点/整数循环）落后 2~3.6× 是主战场；分配/整数案已持平或胜 Go。

## 轮次（按 ROI 排序，每轮单代理，版本递增）

### R1 — SIMD 向量化（0.333，simd 代理进行中）
- 靶：spectral_norm/nbody 浮点乘加循环 → SSE2 packed double（AVX2 评估）。
- 取证：spectral_norm 98% 在 main，全标量 addsd/mulsd/divsd + 栈 spill，0 SIMD。
- 目标：抹平 2~3.6× 浮点差距。门控 PYADAMAS_SIMD 默认关。
- 风险：别名/依赖判错算错（宁保守跳过）；reduction 累加顺序浮点微差（基准输出钉住）。

### R2 — 循环 RC 消除（0.334）
- 靶：RC 密集容器循环（binary_trees/fasta/dict_wordfreq/queue_sim + 日常语料）。
- 内容：循环级 inc/dec 对消除（迭代内抵消）+ 循环不变 RC 外提（LICM 式）；borrow 分析已是地基。
- 参照：Swift ARC retain/release 优化。RC 语言补「赋值零成本」的关键。
- 闸门：RC 正确性命门（ASan leak/UAF + 13/174 + 语料零差异），不可误删 retain。

### R3 — 整数循环优化（0.335）
- 靶：matmul（2.4×）/nqueens（1.6×）整数循环。
- 内容：边界检查消除（BCE）、循环展开、强度削减（索引 imul→add）、regalloc 收紧。
- 取证：matmul 热点 imul/add 索引算术 + 循环开销。

### R4 — 去虚拟化深化（0.336）
- 靶：OO/动态分派（obj_dispatch/nqueens 方法 + 日常语料类代码）。
- 内容：0.294 守卫式 devirt 基础上加 speculative devirt + 类型守卫 + 内联缓存 → 直调+内联。
- 已有：codegen_vcall_inline.cpp gen_guarded_vcall。

### R5 — 浮点 regalloc / spill 削减（0.337）
- 靶：spectral_norm/nbody 浮点寄存器压力（热点大量 movsd (%rsp) spill）。
- 内容：xmm0-15 分配改进、活跃区间分裂、spill 代价模型；与 R1 SIMD 协同。

### R6 — 编译速度剩余热点（0.338）
- 靶：编译吞吐（非生成码运行）。cgspeed 后剩余：comment(in.text()) IR 文本 vformat、
  block_stores/first_use 每指令哈希（~2-3%）、assign_vreg_slots（regalloc 3.56%）。
- 内容：IR 注释惰性/缓存、哈希合并、regalloc 提速。

### R7 — 数学函数 SSE 内联（0.339）
- 靶：nbody 6 次 sqrt call → sqrtsd 指令；其它 libm 热点内联/SSE。

### 可选 — GC 阶段 2（后台 marker，C2）
- 若 GC 停顿仍是瓶颈（174 类大堆），上后台 marker 线程真并发（设计已就绪 docs/gc-concurrent-mark-design.md）。
- 非性能主线，按需插入。

## 自主决策框架

1. **单代理槽位**：当前代理完成前不派下一个。simd 运行中→等。
2. **每轮完成**：lead 独立复核闸门（difftest/套件 normal+ASan/CPython 对拍/语料/leak/26 asm/abbench）→
   通过则集成（merge --no-ff + CHANGELOG 定版 + 重建 master 复验 + 推送 + 清理 worktree）。
3. **评估收益**：abbench/四语言基准前后对照，量化本轮收益；更新 roadmap 优先级。
4. **派下一轮**：按 ROI 选下一个（默认 R2→R3→...，据实测收益动态调整）；代理早夭→send_message 恢复。
5. **负面结果**：收益不抵复杂度→记录跳过（如 P2），进下一轮。
6. **不许停**：持续等代理→评估→派下一轮，直到 roadmap 走完或用户唤醒暂停。

## 硬纪律（每轮）

- 独立 worktree /tmp/wt-<name>，分支从最新 master，基线对照用主树 build/lead。
- 门控默认关（env/编译期），非优化路径 asm 逐字节不变（26+ 语料 cmp）。
- 闸门：difftest 0 + 套件 normal/ASan EXIT=0（JOBS=8）+ CPython 对拍 + 语料零差异 + ASan leak 钉子 + abbench 无回归 + check_size 无新增超限。
- RC/GC 改动额外：13/174 flag-on + leak 干净。
- CHANGELOG 版本递增（0.333 起），集成时定版；选择性 git add；推送走 SSH root@127.0.0.1。
- build/release 是 0.271 oracle，**绝不重建**；主树 build/lead 是当前 release 对照。

## 迭代日志（自主执行记录）

| 轮 | 内容 | 版本 | 结果 |
|---|---|---|---|
| R1 | SIMD 原型 spectral_norm reduction | 0.333 | ✅ cycles 4.36× / 墙钟 2.29×，逐位一致，集成 |
| R2 | SIMD 扩展 nbody+matmul | 0.334 | ✅ matmul 5.94× / nbody 4.32×，**反超 Go**（0.48×/0.70×），集成 |
| R3 | 循环 RC 消除（块内对消除） | — | ❌ **负面**：正确但≈0 加速（RC 密集循环开销主体是必要所有权转移，非冗余对）。循环外提/borrow 细化收益不抵风险。**跳过不集成**。真正杠杆=borrow 细化（只读借用循环变量槽），需独立专项轮（slot_owned/帧释放/异常展开协同 + 跨迭代值 identity 分析）。rc1 分支留档未合。 |
| R4 | 整数循环 BCE+跳转线程化 | 0.335 | ✅ matmul −25% / nqueens −8.9%（关键：BCE 去 call 非去 cmp，消块内 spill）；fannkuch 中性保守跳过；P3 dedup 负面。集成 | |
| R5 | regalloc 只读槽别名+跨调用提升 | 0.336 | ✅ modest：mandelbrot +4.3% / fasta +3.3% / nqueens 栈触碰 21→6.6%（墙钟 +1.4%）；**fib/matmul 负面**（fib 自递归 callee-saved 净负需 shrink-wrap；matmul 容量bound）。命门 bug：P2 别名需寄存器全函数单射（语料压出 run_diff 漏的 9 错）。集成 |

### 优先级调整（据实测）
- 数值核（spectral_norm/nbody/matmul）已反超 Go → SIMD 主线告一段落（通用化 P4 / AVX2 P5 留后续）。
- RC 对消除是死胡同（≈0）；borrow 细化是真正 RC 杠杆但高风险，列专项（R-borrow）。
- **下一轮 R4 = 整数循环优化**（nqueens 1.6× Go / fannkuch 2.4× C++ / matmul 索引）：BCE + 展开 + 强度削减 + regalloc。
- 待议：SIMD 默认开（当前 opt-in，门控全过；narrow matcher 仅匹配特定循环，fallback 安全）。