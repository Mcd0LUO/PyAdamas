# 下一阶段路线图（0.471 起）

> 基线：**0.470**（`8557f7a`），2026-11-24。本页取代
> [iteration-roadmap.md](iteration-roadmap.md)（那是 0.356 基线的一轮计划，保留作历史），
> 与 [roadmap.md](roadmap.md)（阶段记录 + 技术债）和 [HANDOFF.md](HANDOFF.md)（时点快照）
> 分工不同：**这里只排接下来做什么、按什么顺序、怎么算做完**。
>
> 数字口径：标 `[实测]` 的是本页写作时在仓库里跑出来的；标 `[文档]` 的是
> CHANGELOG/HANDOFF 记录的上一轮闸门结果；标 `[待复测]` 的还没在当前 HEAD 上重新取证。

## 0. 基线

| 项 | 值 |
|---|---|
| 版本 / HEAD | 0.470 / `8557f7a` |
| 闸门 | 普通 + ASan 套件 **469 ran ok / 0 失败**，difftest **440 / 0**，O0/O1/O2 全过 [文档] |
| 用例规模 | `tests/run` 441、`tests/fail` 91、正例 12、`strict` 7、`unsupported` 1、`tests/modules` 23 目录 [实测] |
| 规模检查 | 仍失败：`src/typecheck/typecheck.cpp` 1227、`src/cimport/cdecl_parse.cpp` 1271 [实测] |
| 基准 | 12 例 × -O0/-O1/-O2 输出 36/36 与 CPython 一致；obj_dispatch 292.5 ms（Codon 2.56×）[文档] |
| 已知负收益方向 | SSA 化、图着色/第三层 RA、堆槽提升、IVSR、dict 末次查找缓存、槽 SSA、`RA_SPLIT` |

**当前最贵的三个单点**（延续 HANDOFF §5 的判断）：
1. obj_dispatch 内层循环 ~11 周期/轮，约 18 条 RC inc/dec + 3 次 vtable 比较；
2. 8 个基准的向量化全部止步于前置门（体块规模 / 循环形状 / 调用屏障，见
   [vectorization-coverage.md](vectorization-coverage.md)）；
3. 前端能力面还有两类「编译期就拒绝、但用户天天写」的构造（`Fraction`/`Decimal`
   与 `int` 的混合运算、装饰后的 `__iter__`）。容器装精确数值已验证可用
   （`list[Fraction]`/`dict[str, Fraction]` 实测可读写与迭代）。

## 1. P0 —— 0.471：先把账平掉

| # | 任务 | 为什么现在 | 验收 |
|---|---|---|---|
| 1 | **文档漂移收口**（并行：hy4flash-free 子代理，只改 `.md`） | 0.470 的文档改动还躺在工作区；`index.md` 的「当前状态 0.446 / 420 ran ok」、`testing.md` 的闸门表（仍算 LSP 冒烟）、`roadmap.md` 的「当前状态 0.300」与 5 文件超限清单、README 的验证表与「异常用 setjmp」、`compatibility.md`/`development.md` 的用例计数、`codegen.md` 的异常展开一节，全部落后 | 每个数字都能用页内给出的命令复现；`docs/index.md` 文档地图补齐（`big-landing.md`、`testing.md`、本页）；不新增未验证数字 |
| 2 | **0.470 文档提交** | 与 #1 同批 `git add`，一次提交，避免版本号与文档分叉 | `CHANGELOG` 顶格版本 = `git log` 最新版本 = 0.470；提交后工作区干净 |
| 3 | **pya-lsp 0.3.x 落地**（并行：LSP 子代理，独立仓库） | 该仓库有 791 行未提交改动（0.3.0 作用域感知补全 + 0.3.1 prepareRename/documentChanges），冒烟声称 276 条 | 本地构建 + 冒烟 ≥276 全过；CHANGELOG/README 同步；本地提交，不 push |
| 4 | ~~**difftest 假绿治理**~~ **已完成（0.471）** | 默认 nproc 并行时 tarfile 组偶发超时假 DIFF；两侧都编不过时还会被判「一致」（假绿） | 编译/运行分开记退出码 → BUILDFAIL；首轮不一致自动单路复跑（20s→90s），抖动单独列出不计失败。48 路连续 2 次 440/0 |

## 2. P1 —— 0.472–0.474：性能，一版一件事

按杠杆排序，**每项独立成版，闸门全绿才进下一项**。

| 顺序 | 任务 | 杠杆 | 验收 / 风险 |
|---|---|---|---|
| 2.1 | **RC 借用扩展到「入口全局根 + append-only 根」**（obj_dispatch）——**落地（0.473，opt-in）** | ~8–15% → 实测 **-32.7%** | 所有权分析 fail-closed；ASan 全量 + `tests/run` 语料对拍 + CPython 对拍；abbench 15 轮中位数优于基线 >3.5%（A/A 噪声）。**红线**：全局槽是 GC 根，不允许留悬垂借用；先降级成帧槽再借用。**0.473 结果**：未降级成帧槽，改用「入口私有全局候选 + PrivateGlobal 根 + 出口零化」；obj_dispatch 300.59→202.24 ms（-32.7%）、dict_wordfreq -6.5%、sort_struct -2.6%、其余 15 例 asm 逐字节不变；普通/ASan 套件 470/0、difftest 441/0、CPython 18/18；新增 566。细节见 [borrow-loop-design.md](borrow-loop-design.md) §9。门控仍默认关 |
| 2.2 | ~~浮点槽提升的作用域放宽~~ **判负（0.472）** | 无（前提不成立） | **已判负**：浮点**槽**从 0.279 起就没有「整函数无调用」门禁——跨调用代价由 `build_fp_liveness` + 每指令 `fp_spill_mask` 只 spill「调用后还要读」的槽承担，即「活跃区间不跨调用」判据早已实现。`__pa_main` 的 `bb12` 在 **IR** 里有 `load.slot s5`，但 asm/objdump 里 `total` 在 `%xmm8`（`addsd %xmm7,%xmm8`，循环内零槽访存；-O1、VECT=0 同）。唯一的无调用门禁管的是**跨块浮点 vreg**（nbody 上已实测负收益）。nbody 的 dx/dy/dz/d2/mag 跨 `math.sqrt`，必须 spill；要消掉得内联 `sqrtsd`，另立一项 |
| 2.3 | **入口私有全局提升的红利回收（复测即可，不立项）**——**复测完成（0.472）** | 已基本兑现，剩余可能 ≈0 | **已测**：spectral_norm 的 `__pa_main` 内层循环 `bb12` 里没有任何 `load.global`——`u`/`v`/`N` 已被外提成 vreg（`%59`/`%60`/`%61`），剩下的 `load.global g4/g5` 只在 10 次的外层循环里。fannkuch/fasta 的 `load.global` 也从 0.300 口径的 13/19 降到 9/15。**结论：A 的完整形态（分配器候选集扩到标量 global）对这三个基准没有剩余空间，不需要再做；瓶颈已转移到浮点槽与循环体本身** |
| 2.4 | **`PYADAMAS_RA_OPT` 默认化决策** | obj_dispatch ~2%，历史 mandelbrot/fasta +3~4% | 全量 abbench 15 轮 × 3 次；两窗同号才默认开，否则保持 opt-in 并写清理由 |
| 2.5 | **虚调用守卫（函数指针比较）** | ~5–10%，但仅当 2.1 后仍有空间 | 只在 2.1 落地后做；必须用 `objdump` 确认内层循环指令数下降且 DSB 未被挤出（`idq.dsb_uops`） |

> 纪律：profile 只用来找候选，abbench 定生死；任何「改操作数形态」的 pass 都要回头
> 检查消费该形态的后端融合（0.470 的 7% 回退就是这么来的）。

## 3. P2 —— 0.475+：能力面

| 顺序 | 任务 | 证据 | 验收 |
|---|---|---|---|
| 3.1 | **精确算术的用户可用性**：`Fraction`/`Decimal` 与 `int`/`float` 混合运算 | 实测 `Fraction(1,3) * 3`、`f + 1`、`Decimal("1.1") * 3` 均编译期报 `unsupported operand type(s)`——`__mul__(self, o: Fraction)` 的静态签名不接受 int | 构造/四则/比较/格式化与 CPython 对拍（含中间乘积超 i64）；小整数旧用例不退；先给设计取舍（改 stdlib 签名走 unknown 动态分派，还是在类型检查里做数值提升） |
| 3.2 | **big 剩余缺口按当前版本重测** | `big-landing.md` 写于 0.357 候选期，已过时：实测 `dict[big,int]` 键、`list[big].sort()`、`%`/`//`/比较都可用 | 重跑 big-landing 的「尚未完成」清单，逐项标 已支持/仍拒绝；只补真实缺口（sum/min/max、divmod、三参 pow、两参 round、容器键/排序的静态可判定 case） |
| 3.3 | **pya-lsp 反馈的编译器缺口** | pya-lsp（独立仓库）的 `LANGUAGE_GAPS.md`：二进制 `file.read(n)` 返回 str、`check` 不校验模块属性、无 `asyncio.TimeoutError` | 每条缺口给最小复现 + 修复 + 回归用例；优先 `file.read` 的语义含糊（返回类型与 `open("rb")` 矛盾） |
| 3.4 | **前端基线重测** | 最后一次完整口径是 0.171（501/596），之后过了 300 个版本，标准库通过与失败分布都变了 | 当前 HEAD 重跑 Debian CPython 3.13.5 口径，刷新 [compatibility.md](compatibility.md) 的数字；按失败簇修 top-3（不要只刷数字） |
| 3.5 | **三项已知语义缺口**，按用户可见度排 | 装饰后的 `__iter__`（内建迭代路径不查类属性）、未初始化变量读 0（需哨兵/数据流）、异常对象 traceback/列号 | 每项要么修，要么在 compatibility.md 写成明确限制并附最小复现；不允许「文档承诺但不可用」 |
| 3.6 | **包管理 P0 切片**：锁 + 路径依赖 + ModuleMap（**留在本仓库，不单开项目**） | 设计先验已完成（[package-management-design.md](package-management-design.md)，1218 行）；地基已落：`deps --json`、`build --module-map`、版本化 mangling（`$v$0`）、接口 fmt 14 / manifest 3；但无清单、无锁、无 store、无解析器 | 验收按该文档 §6.1 六条：无清单项目 0 回归、path 依赖跨目录可编、手改清单后 `--locked` 失败并打印 diff、连续两次 `resolve` 字节相同、移动目录 lock 不变、锁内无绝对路径。前置：先拍 Q1/Q3/Q5；P0 阶段不改生成代码（汇编与指令数须逐字节/逐项不变） |

## 4. P3 —— 工程债（穿插做，不单独占版本）

1. **拆两个超限文件**：`typecheck/typecheck.cpp`（1227）、`cimport/cdecl_parse.cpp`（1271）。规矩是清单只能删不能加。
2. **CHANGELOG 轮转**：0.100–0.199 归档到 `docs/archive/`，同步 index 与交叉链接。
3. **文档地图扩容**：`docs/` 下已有 60+ 篇，地图只挂了约 30 篇；`numeric-types-landing.md`、`type-tags-landing.md`、`borrow-loop-design.md`、`compile-speed.md` 等仍未挂（`big-landing`/`testing`/`package-management-design` 已补）。
4. **`tools/checkall.sh` 的 git 闸门**：多代理会话默认只提示（`CHECKALL_STRICT_GIT=0`），提交前应显式跑一次 `CHECKALL_STRICT_GIT=1`。

## 5. 明确不做

沿用已判负结论，不要再立项（理由见对应文档，不要当成待办）：

| 方向 | 文档 |
|---|---|
| SSA 化 / 槽位 SSA | [ir.md](ir.md#为什么不是-ssa) |
| 图着色分配 / LiveInterval 第三层 / 块级分裂 | [regalloc.md](regalloc.md)、[regalloc-architecture.md](regalloc-architecture.md) |
| 堆槽提升、IVSR、批量 dec 永生范围检查、dict 末次查找缓存 | [heap-slot-promotion.md](heap-slot-promotion.md)、[ivsr-negative-result.md](ivsr-negative-result.md) |
| 范围版本化边界消除（克隆阶段回退） | [bounds-elimination-attempt.md](bounds-elimination-attempt.md) |
| cpybridge 产品化（协议完备 / IPC 性能 / 关键字参数） | 只作**逃生口**：`from python import …` 仅用于迫不得已的场景，不投产品化；限制写进 [cpybridge-design.md](cpybridge-design.md)（2026-11-24 定） |

## 6. 每轮闸门（缺一不可）

```bash
flock /tmp/pyadamas-checkall.lock bash tools/checkall.sh --full --jobs 8
PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=8 bash tests/run_tests.sh build/lead build/lead/pyadamas   # 仅当触及 runtime/ 或 lowering 所有权
```

- difftest 并行度 ≤8；短用例基准先 `cmp` 两侧 `mod.s` 再谈回归。
- 所有权/RC/GC 改动：ASan 全量 + 语料对拍 + CPython 对拍，三者缺一不可（ASan 抓不到自定义池的 UAF，0.470 已吃一次）。
- 负结论也写 CHANGELOG；撤回机制代码但保留回归测试与诊断。

## 7. 并行分工（0.471 进行中）

| 通道 | 归属 | 边界 |
|---|---|---|
| P1 性能（2.3/2.2/2.1/2.4） | 子代理 `p1-perf`（主仓库） | 只动 `src/` 的性能相关部分；`build/lead` 由它独占；CHANGELOG 版本号由 lead 合并时分配 |
| 包管理 P0-a | 子代理 `pkg-p0`（worktree `.wt/pkg-p0`，分支 `pkg-p0`） | 只动驱动层 + `tests/pkg/`；**不改生成代码**（asm 逐字节不变）；不动 CHANGELOG/HANDOFF |
| 标准库性能与细节 | 子代理 `stdlib-opt`（worktree `.wt/stdlib-opt`，分支 `stdlib-opt`） | 只动 `stdlib/**/*.py` + 新增 `tests/run/` 用例；不动 `src/`、`runtime/`、CHANGELOG |
| 已完成 | 文档漂移（`c7a8dbe`）、difftest 假绿/假红（0.471）、pya-lsp 0.3.x/0.4.0（`f2a5f68`/`bdc9741`）、包管理归属（`ff59eb8`） | — |
| 主线 | lead | 文档、闸门、复核 diff、独立重跑闸门、按合并顺序分配版本号 |

合并顺序：**包管理 P0-a → 标准库 → P1 性能**（前两条不改生成代码，先合风险低；性能线最后，避免把 asm 口径搅在一起）。
三条线互不修改对方的工作区；每个 worktree 自带构建目录（`.wt/*/build/...`）。

## 8. 收尾之后（0.48x）：增量重编 → 性能收尾

> **2026-11-24 定调**：cpybridge（`from python import …`）**只作逃生口**——迫不得已才用，
> 不做产品化（协议完备性、IPC 性能都不投）。因此「借 CPython/pip 生态」不构成「不必自己做包管理」
> 的理由，registry 仍是包管理主线（P2）。下面两件事排在当前 P0–P3 收尾之后，顺序即优先级。

### 8.1 增量重编（先做）

**目标**：把「改一行 → 重新出二进制」的固定成本压下来，且不引入任何正确性风险。

**现状**（口径见 [iteration-roadmap.md](iteration-roadmap.md#一次性短脚本的固定成本实测构建路径)）：hello-world 冷构建 52ms、热构建 38ms，其中 `as`≈8ms、`ld`≈9ms、编译器自身 10–15ms、两次 `sh -c` 各约 1ms。模块级缓存已经存在（manifest v3 逐字段校验，入口模块也走缓存），**真正的固定成本在汇编、链接与进程启动**，不在前端。

**交付**：
1. **整程序产物缓存**：源码 + 全部开关 + 工具链 + 依赖接口哈希都没变、且输出文件仍是上次产物 → 直接复用，不再跑 `as`/`ld`。键必须含 target、module_version、lang_gaps、dynamic_big、simd/vect/avx2——**漏一项就是静默错误**。
2. **`run` 的重复执行**：同源同选项第二次 `run` 不重新编译链接（临时产物按内容键复用）。
3. **缓存键确定性**：把 CHANGELOG 纳入 CMake 的 `CMAKE_CONFIGURE_DEPENDS`，消灭「版本号改了但二进制仍报旧版本」（现在 `deps --json` 报 0.469，而 CHANGELOG 是 0.471）。
4. **不做**内嵌汇编器/链接器：那是另一个量级的工程，先让产物缓存把收益上限量出来。

**验收**：
- hello-world 第二次 `build` 从 38ms 量级降到 <10ms（同窗 A/B、5 次取最小、绑核）；
- 改动任一输入（源码 / 任一开关 / 编译器 / target / 依赖接口）必须 miss，并且做一次**「故意漏项」反向实验**：从键里删掉一个开关，应观察到「切换开关仍命中」的错误行为，然后修复并留下回归；
- 复用产物后程序输出逐字节一致；两进程并发 0 损坏（复用 `tests/cache_race.sh` 的思路）；
- 全量闸门绿（套件 469 / difftest 440 / check_size 不新增超限）。

**风险**：缓存键漏项 = 静默错误（比慢更糟）；产物文件被外部改写要能检出。

### 8.2 性能收尾

**目标**：把性能这条线**结账**——该拿的拿完、该判负的判负、死路删掉、文档数字统一到可复现口径。

**交付**：
1. **向量化缺口定论**：8 个基准的 0 触发（masked 向量化 / 嵌套循环 / 调用屏障，逐条 IR 证据见 [vectorization-coverage.md](vectorization-coverage.md)）——要么实现其中收益最大的一个，要么写负结论并归档，不再挂着当待办。
2. **P4 删特化**：spectral_norm 仍约 1.13× 依赖特化路径。决定是删特化收敛到通用路径（减维护面），还是保留并写清理由。
3. **实验开关结账**：`PYADAMAS_RA_OPT` / `RA_SPLIT` / `RA_GRAPH` / `LOOP_OPT` / `BORROW_OPT` / `VECT` / `TYPE_TAGS` / `GC_INCR_MARK` 逐个给结论：默认开、保持 opt-in（写明理由）、或删代码。
4. **基线刷新**：12 基准 `instructions:u`、四语言对比、Codon 九例，全部在当前 HEAD 重测，更新 `tools/perf_baseline.txt` 与文档表格。

**验收**：
- `tools/checkall.sh --full` 全绿；perf 基线是「人工确认后写入」，不是为过闸门改数；
- 文档里每个性能数字都能用页内命令复现；负结论进 CHANGELOG；
- 没有死代码/空壳 `switch`（CONTRIBUTING 第 6 条）。

### 8.3 编译器侧候选（由标准库轮提出，先取证再立项）

标准库轮（`stdlib-opt` 分支）用两个真实模块把「编译器缺什么」量出来了。这两条**不是** P0–P3 的待办，
而是最后一轮编译器优化的候选，立项门槛写在最后一列。

| # | 候选 | 证据（同窗 A/B） | 立项门槛 |
|---|---|---|---|
| a | **泛型函数体按推断的元素类型自动特化**：推出 `list[int]` 后，函数体里的元素比较/算术走静态特化，而不是 `pa_any_cmp_t` 通用标签比较（`runtime/rt_any.c`） | `heapq` 把形参从裸 `list` 改成 `list[int]` 后，`pa_any_cmp_t` 占比 29% → 0，bench_heapq **63.6 → 18.5 ms**（对 CPython 1.20× → 4.06×） | 在 ≥2 个真实 stdlib 模块上各拿到 ≥20%；O0/O1 产物不变；unknown 语义不动 |
| b | **循环内字符串构造（str builder）**：`out = out + c` 每次 `pa_str_concat` 都分配新对象，O(n²) 字节拷贝 | `textwrap` 的逐字符拼接是 memmove/str_alloc 主因（perf ~20%）；改 list+join 后 **968.7 → 547.2 ms**（0.85× → 1.50×） | 只对能证明「累加器无别名、循环内不读中间值」的循环生效，fail-closed；与 RC 所有权不冲突；输出与 CPython 逐字节一致 |

