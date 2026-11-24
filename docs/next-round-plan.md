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
| 2.1 | **RC 借用扩展到「入口全局根 + append-only 根」**（obj_dispatch） | ~8–15% | 所有权分析 fail-closed；ASan 全量 + `tests/run` 语料对拍 + CPython 对拍；abbench 15 轮中位数优于基线 >3.5%（A/A 噪声）。**红线**：全局槽是 GC 根，不允许留悬垂借用；先降级成帧槽再借用 |
| 2.2 | **浮点槽提升的作用域放宽**：把「整函数无调用」改成「热循环体内无调用」 | 中等：spectral_norm 内层循环的 `total`（float）仍在内存槽 | 实测证据：`ir` 的 `__pa_main` 内层块 `bb12` 每轮 `load.slot s5` / `store.slot s5` 搬浮点累加器；该函数有 `call.rt`（print/append）所以浮点槽整体不提升。改成按循环判「无调用」后，spectral_norm/nbody 的浮点内层循环应出现 xmm 常驻。验收：`objdump` 内层循环去掉槽访存 + abbench 两窗同向；含 handler 的函数仍排除 |
| 2.3 | **入口私有全局提升的红利回收（复测即可，不立项）** | 已基本兑现，剩余可能 ≈0 | **已测**：spectral_norm 的 `__pa_main` 内层循环 `bb12` 里没有任何 `load.global`——`u`/`v`/`N` 已被外提成 vreg（`%59`/`%60`/`%61`），剩下的 `load.global g4/g5` 只在 10 次的外层循环里。fannkuch/fasta 的 `load.global` 也从 0.300 口径的 13/19 降到 9/15。**结论：A 的完整形态（分配器候选集扩到标量 global）对这三个基准没有剩余空间，不需要再做；瓶颈已转移到浮点槽与循环体本身** |
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

## 6. 每轮闸门（缺一不可）

```bash
flock /tmp/pyadamas-checkall.lock bash tools/checkall.sh --full --jobs 8
PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=8 bash tests/run_tests.sh build/lead build/lead/pyadamas   # 仅当触及 runtime/ 或 lowering 所有权
```

- difftest 并行度 ≤8；短用例基准先 `cmp` 两侧 `mod.s` 再谈回归。
- 所有权/RC/GC 改动：ASan 全量 + 语料对拍 + CPython 对拍，三者缺一不可（ASan 抓不到自定义池的 UAF，0.470 已吃一次）。
- 负结论也写 CHANGELOG；撤回机制代码但保留回归测试与诊断。

## 7. 并行分工（0.471）

| 通道 | 归属 | 边界 |
|---|---|---|
| 文档漂移 | 子代理 hy4flash-free | 只改 `docs/*.md`、`README.md`；不碰 `HANDOFF.md`、不改代码、不提交，由 lead 复核后统一提交 |
| pya-lsp | 子代理（独立仓库） | 只改 `/src/luo-proj/pya-lsp`；不碰编译器仓库；本地提交不 push |
| 主线 | lead | P1 性能取证；`build/lead` 与 `build/release`（0.271 oracle）分开 |

合并顺序：文档 → lsp → 主线性能。三条线互不修改对方的工作区。
