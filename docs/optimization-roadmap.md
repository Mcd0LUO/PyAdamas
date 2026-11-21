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

## 夜间自主迭代续（0.337-0.349，用户睡前授权单槽位顺序 + 完成后自由发挥）

| 轮 | 内容 | 版本 | 结果 |
|---|---|---|---|
| R6 | devirt 去虚化+递归展开 | 0.337 | ✅ fib −29%（递归展开，通用）；集成 |
| R7 | borrow 只读借用循环槽 RC 消除 | 0.339 | ✅ dict_wordfreq −4.5%（~960 万 RC 调用消除，通用 RC 杠杆，非基准特化）；sort_struct −1.7%；ASan leak/UAF 34/34、语料 353 零差异；d6_rec_memo +1~2% 噪声带负面记录。集成 |
| stdlib3 | urllib.parse/secrets/calendar/struct/queue/contextlib/difflib 自举 | 0.338 | ✅ 7 模块纯 pa；pprint 因 unknown 类型标签缺口跳过（→P2）；集成 |
| P2 | unknown 运行期类型标签（NONE/BOOL/TUPLE/SET/BYTES/CLASS） | 0.340 | ✅ isinstance/repr/pprint 解锁（stdlib-b3 根本缺口）；P3 既有 tag 0-5 不变只追加；pprint 10 例字节同 CPython；flag-off asm 117/117。集成 |
| P1 | 定宽整数 i8-i64/u8-u64/char/short | 0.341 | ✅ int_promote 提升规则 C 式定死；64 位域计算+宽度正常化；字面量 coerce 范围检查；抓常量折叠回绕 bug；现有 asm 120/120。集成 |
| 编译速度 | 位集系列（compute_dom/regalloc 活跃性/哈希→定长数组/注释直写复用/RcMark 单扫） | 0.342 | ✅ **编译吞吐 −46.8%（≈1.88×）**，重案 234_re_opt 1.48s→0.15s（≈10×）；compute_dom 20.2%→1.6%、assign_vreg_slots 14.2%→1.1%、VReg 哈希 14%→0；asm 413 文件 sha256 全同。集成 |
| P4块1+2 | 异常类当值（CLASS tag）+suppress/closing；file 协议泛化+io.StringIO/BytesIO+csv 互操作 | 0.343 | ✅ 修 custom 异常类 qualname except 匹配命门+repr 全限定名+GC name leak；file 零 codegen 改动源码分派；异常钉子 12/12 leak 0；CPython 340/341 字节同。集成 |
| SIMD通用化 | 结构驱动通用循环向量化器 codegen_vect.cpp（map/reduction/原地/偏移/SSA） | 0.344 | ✅ **非基准探针 1.93×**（saxpy+dot，近 2-lane 上限）；别名/依赖保守跳过+循环版本化；基准不回归（spectral_norm 1.00×/nbody 0.98×/matmul 0.99×）；回应基准特化质疑。集成 |
| UAF修 | ir.cpp vreg_str 缓存 vector→deque | 0.345 | ✅ 修 0.342 引入的 resize 重分配致 string_view 悬空 heap-UAF（ASan 编译器构建暴露）；asm 不变。集成 |
| P4块3+4 | traceback 帧捕获+sys.exc_info+tb 对象+traceback.py；dataclass 编译期 AST 展开 | 0.346 | ✅ 修 3 命门 bug（重查轮门控传递/单值 BoolOp 悬空块/CFA 提交口径）；异常钉子 10/10 leak 0；CPython 350/351 字节同；dataclass 7 类保守拒绝钉住。集成 |
| tarfile | stdlib/tarfile.py 纯 pa 自举（ustar 读写/gz/GNU L-K/pax x-g 读/流式 BytesIO） | 0.347 | ✅ ustar 头字节同 CPython 3.14；5 用例字节同+跨实现互操作；src 仅 +1 行（复用既有 _shutilc）；pax/GNU 写保守跳过。集成 |
| 重构 | 拆 5 个超 1200 行文件（regalloc/driver/typecheck_types/ir_ops/lower_impl） | 0.348 | ✅ **check_size 超限 5→0**；纯代码移动（逐行多重集核对 0 缺失）；asm 38/38 不变；cache_race 仍安全。集成 |
| P1收尾 | u64 大字面量（词法）/定宽容器元素 coerce/u64 无符号排序/C FFI 窄无符号映射 | 0.349 | ✅ 默认 int 行为不变（hex/oct/bin 逐字不动）；超范围编译报错不静默窄化；int_promote 一字未改（332-335 全过）；asm 120/120。集成 |
| stdlib5 | textwrap（CPython 3.14 口径全参数面）/string（Template/capwords/常量）/pprint sort_dicts+参数面 | 0.350 | ✅ 纯 stdlib 无 src 改动；pprint 1926 行 0 diff、textwrap 8490 行、string 1481 行对拍；pprint 无递归检测（无 id()）/容器元素退化（P2 边界）负面记录。集成 |
| SIMD深化 | int 乘 map（pmuludq 全 i64 重建无守卫）/Convert i64→float（cvtdq2pd 逐位）/多 reduction 并行累加器 | 0.351 | ✅ 非基准 int 乘 2.36×/Convert 2.34×/多 reduction 1.38×；**复活 0.344 起死代码 reduction 路径并修 5 潜伏 bug**（int acc 出口覆盖写静默算错/跨块 list 基址 RC move-from NULL 段错误等）；基准不回归噪声带。集成 |
| stdlib6 | statistics（mean/median 家族/mode/离散度/geometric/harmonic/quantiles/covariance/correlation/linear_regression） | 0.352 | ✅ 纯 pa 无 src 改动；380-385 CPython 对拍（3 条浮点精度边界钉住，pa 无 big 精确分数中间量）；NormalDist/weights 保守跳过。集成 |
| UB修2 | lower_impl.h def 非负提示先算右值再取左值引用+守 size | 0.353 | ✅ 修预存求值顺序 UB 堆越界（ASan 编译器 118_suspend_expr 暴露）；asm 不变；继 0.345 后第二处编译器内存安全 bug。集成 |

### 夜间小结
- **17 版集成推送（0.333-0.349）**，单槽位顺序（收回 4 代理并行减少早夭混乱）；harness 间歇早夭频繁，send_message 恢复套路管用。
- **通用性兑现**：borrow（dict_wordfreq −4.5% 日常语料）+ SIMD 通用化（非基准 1.93×）回应基准特化质疑；编译速度 −46.8% 普惠所有编译。
- **语言面大幅完善**：定宽整数全（P1+收尾）+ unknown 类型标签（P2，pprint 解锁）+ P4 全 4 块（异常类当值/file-io/traceback/dataclass）+ tarfile stdlib。
- **代码整洁**：check_size 超限清零（重构）；ir.cpp UAF 修。
- **暂缓**：P3 big BigInt（用户停，big1 有 rt_big.c partial 留档）；SIMD AVX2/改序 reduction（输出风险）；pax/GNU 写。
- 测试规模 262→347+（含定宽/类型标签/语言缺口/tarfile/向量化回归）；difftest 328/0；语料零差异。