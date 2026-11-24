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
| 2.1 | **RC 借用扩展到「入口全局根 + append-only 根」**（obj_dispatch）——**落地（0.474，opt-in）** | ~8–15% → 实测 **-32.7%** | 所有权分析 fail-closed；ASan 全量 + `tests/run` 语料对拍 + CPython 对拍；abbench 15 轮中位数优于基线 >3.5%（A/A 噪声）。**红线**：全局槽是 GC 根，不允许留悬垂借用；先降级成帧槽再借用。**0.474 结果**：未降级成帧槽，改用「入口私有全局候选 + PrivateGlobal 根 + 出口零化」；obj_dispatch 300.59→202.24 ms（-32.7%）、dict_wordfreq -6.5%、sort_struct -2.6%、其余 15 例 asm 逐字节不变；普通/ASan 套件 470/0、difftest 441/0、CPython 18/18；新增 566。细节见 [borrow-loop-design.md](borrow-loop-design.md) §9。门控仍默认关 |
| 2.2 | ~~浮点槽提升的作用域放宽~~ **判负（0.472）** | 无（前提不成立） | **已判负**：浮点**槽**从 0.279 起就没有「整函数无调用」门禁——跨调用代价由 `build_fp_liveness` + 每指令 `fp_spill_mask` 只 spill「调用后还要读」的槽承担，即「活跃区间不跨调用」判据早已实现。`__pa_main` 的 `bb12` 在 **IR** 里有 `load.slot s5`，但 asm/objdump 里 `total` 在 `%xmm8`（`addsd %xmm7,%xmm8`，循环内零槽访存；-O1、VECT=0 同）。唯一的无调用门禁管的是**跨块浮点 vreg**（nbody 上已实测负收益）。nbody 的 dx/dy/dz/d2/mag 跨 `math.sqrt`，必须 spill；要消掉得内联 `sqrtsd`，另立一项 |
| 2.3 | **入口私有全局提升的红利回收（复测即可，不立项）**——**复测完成（0.472）** | 已基本兑现，剩余可能 ≈0 | **已测**：spectral_norm 的 `__pa_main` 内层循环 `bb12` 里没有任何 `load.global`——`u`/`v`/`N` 已被外提成 vreg（`%59`/`%60`/`%61`），剩下的 `load.global g4/g5` 只在 10 次的外层循环里。fannkuch/fasta 的 `load.global` 也从 0.300 口径的 13/19 降到 9/15。**结论：A 的完整形态（分配器候选集扩到标量 global）对这三个基准没有剩余空间，不需要再做；瓶颈已转移到浮点槽与循环体本身** |
| 2.4 | **`PYADAMAS_RA_OPT` 默认化决策**——**已决（0.475）：保持 opt-in** | 实测 12 基准上已无收益 | 15 轮 × 两窗实测：realistic -0.2%、classic +1.8%（反号），asm 真变化的 3 例里 nqueens +8.5% 是回退；判据不成立 → 默认关。理由与数据见 [ra-opt-design.md](ra-opt-design.md) §6 |
| 2.5 | **虚调用守卫（函数指针比较）**——**未做（本轮时间不足，条件成立）** | ~5–10% | 2.1（0.474）落地后 obj_dispatch 内层循环 25 条指令里守卫占 ~10 条（3 次 vtable 比较 + 3 条 `leaq`），把「vtable 地址比较」换成「函数指针比较」可把 Square/Rect 合并成 2 个候选、省 1 条 `leaq`+`cmp`+`jcc`（估 ~3 条/25 ≈ 10% 指令）。**未做**：安全论证要重写「同一实现被多类继承」的候选合并（Square 继承 Rect.area），且必须先用 `objdump` + `idq.dsb_uops` 证明收益不是布局假象——需要独立一版的时间预算。下一步：在 `vcall_inline.cpp` 的守卫候选键上按实现指针去重，跑 DSB 计数 + 15 轮 A/B |

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
| 3.6 | ~~**包管理 P0 切片**~~ **P0-a 已完成（0.473）** | `pyadamas.toml`（最小 TOML 子集）/ `pyadamas.lock`（SHA-256 树哈希）/ `resolve` / `--locked` / 锁派生 ModuleMap 全部落地，`tests/pkg` 56 项 + ctest | 六条验收全过（见 [package-management-design.md](package-management-design.md) 附录「落地记录（P0-a）」）。**P0-b 剩余**：版本进 mangling 与接口坐标（需一次 ABI bump，会改符号/汇编，单独一版）、`check --imports` 接包映射、本地模块与包同名报冲突、provider 侧 `modules` 声明 |
| 3.7 | ~~**修 `from pkg import sub` 后调用子模块函数的 SIGSEGV**~~ **已修（0.477）**：根因是子模块别名登记时漏发 `import.module`，init 未跑 → 函数全局为 0 → `call 0`；修法是登记前 `ensure_module_init(child)`。回归 `tests/modules/from_pkg_sub/` | 最小复现：`pkg/__init__.py` + `pkg/sub.py`（`def hi() -> int: return 7`），`from pkg import sub; print(sub.hi())` → **rc=139**；而 `import pkg.sub as s; s.hi()`、`from pkg.sub import hi; hi()`、`from pkg import sub`（不调用）都正常。0.473 之前的基线同样崩 | 先加 `tests/run` 回归（当前会崩，先写成 fail 用例或等修复后加），ASan 下定位 `src/lower/import.cpp` 的绑定/降级路径；修复后套件与 difftest 全绿 |
| 3.8 | **类体里的「方法别名」写法**：`class A: def f(self) ...; __str__ = f`（CPython stdlib 里非常常见，如 `_weakrefset.py` 的 `__sub__ = difference`、`doctest.py` 的 `__str__ = shortDescription`）。0.486 的 strict names 把 25 个 stdlib 文件暴露成 check 红——**这些文件 build 同样拒绝**（不是 check 独有的误杀，但与 CPython 不符） | 最小复现 3 行，`check`/`build` 都报 `undefined name 'f'`；`--lenient-names` 放行。根因待定位：typecheck 的类体符号登记时机（`def` 是否在语句结束时才进类作用域，导致后续 `__x__ = f` 看不到） | 修好后这 25 个文件的 check 通过率应回到 81.1% 一档；修不了就判负并写清「类体求值顺序 vs 静态登记」的取舍 |
| 3.9 | **多继承基类方法的字段偏移错误（静默算错）**：`class C(B, A)`（A 不是第一个基类）时，A 的方法里读 `self.x` 用的是 **A 自己的字段偏移**，读到的是 B 的字段。实测 `cb.get()` 返回 **20**（CPython 10），但 `cb.x` 属性访问**是对的**（10）——偏移只在「方法体内的 self 字段访问」这条路径上算错 | 最小复现 10 行：两个基类各带字段、`C(B, A)` 在 `__init__` 里给两字段赋不同值，再调 A 的方法读 A 的字段 | 修法二选一：①调用非首基类方法时给 `self` 加基址偏移（thunk）；②方法体内字段偏移走运行期表。修好前多继承只能当「首基类字段 + 无状态 mixin」用，必须写进 compatibility.md |
| 3.10 | **`PYADAMAS_GC_CONSERVATIVE=1` 下 `161_gc_conservative_equiv` 段错误**（既有，rc=139）：在 d6937be 的纯净构建上同样复现，与 0.497 的索引改造无关 | 复现：`PYADAMAS_GC_CONSERVATIVE=1 build/lead/pyadamas run tests/run/161_gc_conservative_equiv.py` | 先定位是保守模式漏标导致提前回收，还是索引/堆链在该模式下的不一致；保守模式是「回退路径」，但段错误意味着它现在不可用，要么修好要么明确标注不可用并下线开关 |
| 3.11 | **窄化的三类假阳性**（0.499 引入的 6 个文件）：① walrus `(n := x) is None` 后的窄化；② `isinstance(x, T)` 窄化；③ 模块全局「先定义后初始化」。前两类是同类 refinement 扩展（和 `T \| None` 同一层），第三类要在「模块全局零初始化」上区分「已赋值」 | 复现：628 基线里 base64.py / graphlib.py / modulefinder.py / pyclbr.py / multiprocessing/util.py / xml/dom/pulldom.py 六个文件；每例根因见 docs/type-narrowing.md | 修好前这 6 个文件是明确代价；修好后 628 默认口径应回到 500/623 以上 |
| 3.12 | **`if xs:` 对空 list 在 `-O0` 下崩溃**（既有，0.498 master 同样复现）：`if xs:` 的 `__len__`/`__bool__` 路径在 -O0 下对空容器解引用 | 复现：`xs: list[int] = []` + `if xs:` + `-O0` 编译运行 | 先定位是 -O0 的 `pa_list_len` 调用约定还是空容器指针，修好加回归（-O0 也要进闸门覆盖） |

**3.x 结账（0.484–0.490）**：

- **3.1 判负（0.489）**：`Fraction`/`Decimal` 与 `int`/`big` 的混合运算**不再支持**。0.487 试过「stdlib 里 union 形参 + `isinstance` 分派」，但 `isinstance` 无法区分未装箱 double 与 int/big（同为裸字），运行期必落到构造函数、把位模式当 BigInt 指针读（静默算错、每次结果不同）→ 回退。正解二选一：①类型检查层数值提升（每个运算符点插 `Fraction(int)`）；②联合类型加运行期标签。两者都是独立一层，不是 stdlib 能修的。
- **3.2 部分落地（0.488）**：重测确认 `dict[big]`/`set`/`sort`/`sum`/`min`/`max`/`%`/`//`/比较**早已可用**；`divmod` 落地（与 CPython 逐字节一致，含负数与除零；顺带修掉 big 的 `Bin{FloorDiv,Mod}` 静默发 `idivq` 的段错误，改走 `pa_big_floordiv/pa_big_mod`）；`round(x, 2)` 与 `pow(x, y, m)` 判负（要新增 `pa_big_round` / `pa_big_pow_mod`）。
- **3.3 三条落地 + 一条判负（0.484/0.486）**：模块属性检查（`--imports` opt-in）、`asyncio.TimeoutError`、`check` 报模块级未定义名（默认开，`--lenient-names` 逃生口）；二进制 `file.read(n)` 仍返回 str，判负并给三步改法。
- **3.4 完成（0.485）**：CPython 3.14.4 标准库 628 文件，语法 623/628（99.2%）、旧口径类型 505/623（81.1%）、默认口径 480/623（77.0%）、`--imports` 155/623；失败簇 top-10 与 top-3 判负已归档。
- **3.5 三项全负（0.490）**：`for` 对装饰/未装饰 `__iter__` 都能跑（**路线图原前提有误**），真正缺口是 `list(obj)`/`sorted(obj)` 等内置构造函数只认内建容器；未初始化变量读 0（槽位零初始化，要 init 位或确定性赋值分析）；异常 traceback 只有单帧、无列号、无 `__traceback__`（行号可修，其余判负）。

> 前端缺口与 unknown 来源的总账见 [frontend-gaps.md](frontend-gaps.md)（基线数字、失败簇、修法清单与状态）。

## 4. P3 —— 工程债（0.483 结账）

1. ~~拆两个超限文件~~ **已完成（0.483）**：`typecheck/typecheck.cpp` 1227 → 1017（注解解析拆到 `typecheck/annotation.cpp`）、`cimport/cdecl_parse.cpp` 1271 → 1116（常量表达式求值拆到 `cimport/cdecl_eval.cpp`）。**`tests/check_size.sh` 首次全绿**（全仓库 0 超限，`ALLOW_FILES` 仍为空）。12 基准 `instructions:u` 与 0.482 逐项 ±0.00%，证明拆 TU 不改代码生成。
2. ~~CHANGELOG 轮转~~ **已完成（0.483）**：0.100–0.199（97 版本 / 2183 行）→ `docs/archive/changelog-0.100-0.199.md`；根 CHANGELOG 7753 行 / 282 版本，末尾留指针，`archive/README.md` 与 `docs/index.md` 同步。
3. ~~文档地图扩容~~ **已完成（0.483）**：主表之外新增「其余设计与落地记录（按主题）」，把此前未挂的 30+ 篇按六个主题分组挂上；新增开关登记表 `experiment-switches.md` 也在地图里。
4. **提交前跑 `CHECKALL_STRICT_GIT=1`**：作为流程约定保留（多代理会话默认只提示）。0.483 提交前已按此验证工作区干净；闸门本身不改代码。

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

**交付（0.478 已完成 1–3）**：
1. ~~整程序产物缓存~~ **已落地**：键 = 每个 `.o`（含运行时对象）的内容摘要 + target ABI + `toolchain_id` + gc/asan/cc flags + cimport 链接参数；命中直接写输出，`--rebuild` 与 `PYADAMAS_NO_PROGCACHE=1` 绕过。
2. ~~`run` 的重复执行~~ **已随 1 生效**：临时产物按内容键复用（实测 95 → 26 ms）。
3. ~~缓存键确定性~~ **已修**：CHANGELOG 进 `CMAKE_CONFIGURE_DEPENDS`；顺带把 `toolchain_id` 的内容摘要从逐字节 FNV 改成按 64 位字混合（原来每次进程要读 151MB 的 `/proc/self/exe`，实测吃 25–30ms）。
4. **不做**内嵌汇编器/链接器：那是另一个量级的工程，先让产物缓存把收益上限量出来。

**实测（本机，5 次最小；`/bin/true` 同窗 11–12ms，是进程启动地板）**：hello 热构建 91 → **23 ms**（扣掉启动 79 → 11 ms），`run` 95 → **26 ms**；关掉缓存的构建也从 90 → 72 ms（digest 修复的独立收益）。

**验收**：
- ~~hello-world 第二次 `build` 从 38ms 量级降到 <10ms~~ **0.478 实测 23ms（扣启动 11ms）**：剩下的固定成本是进程启动本身，再往下要内嵌 as/ld；
- 改动任一输入（源码 / 任一开关 / 编译器 / target / 依赖接口）必须 miss，并且做一次**「故意漏项」反向实验**：从键里删掉一个开关，应观察到「切换开关仍命中」的错误行为，然后修复并留下回归；
- 复用产物后程序输出逐字节一致；两进程并发 0 损坏（复用 `tests/cache_race.sh` 的思路）；
- 全量闸门绿（套件 469 / difftest 440 / check_size 不新增超限）。

**风险**：缓存键漏项 = 静默错误（比慢更糟）；产物文件被外部改写要能检出。

### 8.2 性能收尾

**目标**：把性能这条线**结账**——该拿的拿完、该判负的判负、死路删掉、文档数字统一到可复现口径。

**交付（2026-11-24 全部结账）**：
1. ~~向量化缺口定论~~ **已结账（0.479，判负）**：先修正事实——当前 **5/12 命中**（mandelbrot 已被 masked 覆盖，−36.3%），仍 0 的 **7 个**逐条判负并归档：[vectorization-coverage.md](vectorization-coverage.md) §10。三条候选里 masked 已落地、嵌套无增量、调用屏障对剩余 4 个是零收益。
2. ~~P4 删特化~~ **已删（0.480）**：模板对当前 IR 全部失配（12 基准 asm 零 `SIMD` 标记、指令数与关掉向量化逐位相同），1304 → 149 行；默认产物逐字节不变，`PYADAMAS_SIMD=1` 改为显式报错。
3. ~~实验开关结账~~ **已完成（0.481）**：8 个开关逐个给结论，登记在 [experiment-switches.md](experiment-switches.md)（含证据、触发条件、回退方式与删除清单）。
4. ~~基线刷新~~ **已完成（0.482）**：`tools/perf_baseline.txt` 在 HEAD 重测并人工确认后重写；四语言表 [bench/compare/RESULTS-0.480.md](../bench/compare/RESULTS-0.480.md)；Codon 九例（7 例可比）几何 0.960。

**验收**：
- `tools/checkall.sh --full` 全绿；perf 基线是「人工确认后写入」，不是为过闸门改数；
- 文档里每个性能数字都能用页内命令复现；负结论进 CHANGELOG；
- 没有死代码/空壳 `switch`（CONTRIBUTING 第 6 条）。

### 8.3 编译器侧候选（由标准库轮提出，先取证再立项）

标准库轮（`stdlib-opt` 分支）用两个真实模块把「编译器缺什么」量出来了。这两条**不是** P0–P3 的待办，
而是最后一轮编译器优化的候选，立项门槛写在最后一列。

| # | 候选 | 证据（同窗 A/B） | 立项门槛 |
|---|---|---|---|
| a | **泛型函数体按推断的元素类型自动特化**：推出 `list[int]` 后，函数体里的元素比较/算术走静态特化，而不是 `pa_any_cmp_t` 通用标签比较（`runtime/rt_any.c`） | `heapq` 把形参从裸 `list` 改成 `list[int]` 后，`pa_any_cmp_t` 占比 **34.04% → 0**，bench_heapq **62.7 → 18.3 ms**（对 CPython 1.0× → 3.4×）。**触发边界**（很关键）：单独复刻 `_siftup` 函数体**不触发**该路径，必须是「跨模块 import 的裸 `list` 形参 + `fresh_hole` 推断出的 `list[int]`」 | 在 ≥2 个真实 stdlib 模块上各拿到 ≥20%；O0/O1 产物不变；unknown 语义不动 |
| b | **循环内字符串构造（str builder）**：`out = out + c` 每次 `pa_str_concat` 都分配新对象，O(n²) 字节拷贝 | `textwrap` 的逐字符拼接是 memmove（8.98%）+ `pa_str_concat`（2.26%）+ 分配类（~10%）的主因；改 list+join 后 **968.7 → 547.2 ms**（0.85× → 1.50×）。独立 repro：`s = s + "x"` 在 n=1k/10k/50k/100k 上是 **0.00/0.00/0.03/0.19 s**，同循环改 `parts.append` + `join` 四档全部 ≤0.00 s | 只对能证明「累加器无别名、循环内不读中间值」的循环生效，fail-closed；与 RC 所有权不冲突；输出与 CPython 逐字节一致 |

