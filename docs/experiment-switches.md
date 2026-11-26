# 实验开关结账表（0.481 起；表内最新条目 0.531）

> 2026-11-24 收尾（[next-round-plan.md](next-round-plan.md) §8.2-3）。每个开关只允许三种结论：
> **默认开** / **保持 opt-in**（写清理由与触发条件）/ **删代码**。本表是唯一清单：
> 新开关必须在这里登记，结账后不许留「挂着当待办」的条目。
>
> 口径：「默认」列是**不设任何环境变量**时的行为；「证据」列的数字都必须能用本页 §2 的命令复现。

## 1. 结账表

| 开关 | 默认 | 读取位置 | 结论 | 证据 / 理由 |
| `PYADAMAS_VECT_FASTFLOAT=1` | 浮点归约用 packed 累加（`addpd`），**不保序**：与标量逐位结果可能差最后几位。默认关（0.522）。 | 热循环归约 **28ms → 23ms（−18%）**；内存受限场景 ~3% | 需要与 `-O0` 逐位一致时**不要开**（difftest 会红） |
|---|---|---|---|---|
| `PYADAMAS_RA_OPT` | **关** | `main.cpp`（`prog.ra_opt`；manifest 行 `raopt`） | **保持 opt-in** | 0.475 两窗各 15 轮：realistic 合计 −0.2%（逐例 \|Δ\|<0.7%，obj_dispatch +0.2%）、classic 合计 +1.8%（**反号**）。asm 真正变化的只有 3 例：nqueens **+8.5%**（p25 +7.0%，回退方向）、fasta +4.8%（p25 +20.9%）、dict_wordfreq −0.7%；其余 15 例 asm 逐字节相同。0.470 的入口私有全局提升之后 obj_dispatch 上收益已归零。**触发条件**：出现「同块跨调用、活跃区间长」的新热循环，或 call graph 分析落地后重测；`docs/ra-opt-design.md` §6 |
| ~~`PYADAMAS_RA_SPLIT`~~ | **已删除** | `main.cpp`：设了非 0 值直接 `error` 退出（rc=2） | **删代码（已完成）** | `docs/regalloc.md` §已否决：三轮实现均 +27~28%。已按清单删除 `assign_slot_regs_split`、`slot_loc_/slot_reload_in_/slot_spill_out_` 及其在 `block.cpp` 的 5 处使用、`spill_placement.{cpp,h}`、`Program.rc_split` 与 manifest 行 `rcsplit`。**验收**：默认路径 12 基准 asm 逐字节不变（删前/删后 `cmp` 全同）；只剩一条 rc=2 报错文案，防止 A/B 脚本误以为在跑另一条分配路径 |
| `PYADAMAS_RA_GRAPH` | **已删除** | `main.cpp`：残留一条报错（rc=2） | **删代码（已完成）** | 图着色分配器整段删除（0.3xx）。只剩报错文案，防止 A/B 脚本误以为在跑另一条分配路径；无死代码。`docs/regalloc.md` |
| `PYADAMAS_LOOP_OPT` | **3**（默认开：bit0 跳转线程化 + bit1 仿射 BCE） | `main.cpp`；`1`→3、`0`→0、数字掩码 4–7 开发期 A/B | **默认开（现状）** | bit0 纯 CFG 等价变换（nqueens −2.9%，其余中性；含单块直线循环体的函数由 pass 内部跳过，避免破坏向量化）；bit1 仿射 BCE。**bit2（同块 load 去重）实测负收益**，只能显式掩码开。`docs/intloop-design.md` §2 |
| `PYADAMAS_BORROW_OPT` | **关** | `main.cpp`（`prog.borrow_opt`；manifest 行 `borrowopt`） | **保持 opt-in** | 命中即纯赢、不命中 asm 逐字节不变：12 基准里 obj_dispatch 300.59→202.24 ms（**−32.7%**，指令数 −43.2%）、dict_wordfreq −6.5%、sort_struct −2.6%，其余 15 例产物不变。**但日常语料没有可测收益**：`bench/daily` 23 例（其中 5 例命中借用）BURST=20 × 15 轮聚合 **−0.1%**（全在 ±1% 噪声带内；`bench/daily` 的命中用例都 <30ms）。命中 5 例：a5_todo_cli / b1_log_parse / b2_normalize / b3_template / b5_re_extract。**缺的证据**：①一个 ≥100ms、借用能命中的真实语料用例，测得 ≥2% 且 p25 同向；②或 3 倍长的日常语料集上聚合越过 ±1% 噪声带。在此之前默认化 = 用所有权正确性风险换一个量不出来的收益。`docs/borrow-loop-design.md` §8/§9 |
| `PYADAMAS_VECT` | **开** | `main.cpp`；`PYADAMAS_VECT=0` 显式关；manifest 行 `vect` | **默认开（现状）** | 5/12 基准命中：matmul −86.5%、spectral_norm −59.6%、sieve −51.9%、nbody −45.2%、mandelbrot −36.3%（masked）；其余 7 个 0 标记，已判负归档（`docs/vectorization-coverage.md` §10） |
| `PYADAMAS_TYPE_TAGS` | **按需自动开**（不设环境变量时，源码出现 `isinstance` 就开；`=1` 强制开、`=0` 强制关） | `main.cpp`（`prog.type_tags`；manifest 行 `typetags`）+ `driver/program_opts.cpp` 的 `program_wants_type_tags()`；`LANG_GAPS=1` 会强制打开 | **默认开（按需）** | 0.528 起默认开。这是**语义**开关不是性能开关：unknown 值追加 NONE/BOOL/精确堆子类，解锁 `isinstance`/`repr`/`pprint`（`docs/numeric-types-design.md` §2）。关着时 `isinstance(x, int/bool/bytes)` 对动态值是**静默错值**（只有 str/list/dict/float 有探针）；全量强制开又给每个 unknown 值多一个 tag 字（`src/main.cpp` 注释记 12 基准 matmul +660%、spectral_norm +148%），所以折中成「按需开 + 显式逃生口」 |
| `PYADAMAS_GC_INCR_MARK` | **关** | 运行时 `runtime/rt_core.c`（`pa_gc_init` 读一次缓存） | **保持 opt-in** | GC 并发化阶段1 原型：major 走分片标记 + Yuasa 删除屏障 + 门控插入屏障。默认关时行为/asm/leak **逐字不变**；0.444 修过它开启后的 shutdown UAF。没有性能数字（原型不追性能）。**触发**：GC 阶段2（并发标记）立项时整体评估 |
| ~~`PYADAMAS_SIMD`~~ | **已删除（0.480）** | `main.cpp`：残留报错（rc=2） | **删代码（已完成）** | 特化模板对当前 IR 全部失配（12 基准 asm 零 `SIMD` 标记、指令数与关掉向量化逐位相同）→ 1304 → 149 行，默认产物逐字节不变。`docs/simd-vectorization-design.md` 顶部 |

同一注册表里还有（不在本次清单，登记备查）：`PYADAMAS_INLINE_OPT`（默认关，内联/devirt 深化）、`PYADAMAS_LANG_GAPS`（默认关，P4 语言缺口）、`PYADAMAS_AVX2`（默认开，通用向量化的 4-lane 路径）、`PYADAMAS_SPLIT_MAXREG`（已随 RA_SPLIT 删除、设了即报错）。

## 2. 复现

```bash
# 开关 A/B（同一编译器、两侧独立 modcache + --rebuild、打乱顺序 + 奇偶轮交换、BURST 连跑、15 轮中位/p25、绑核）
bash tools/flagbench.sh <ENV_NAME> realistic 15     # realistic / classic / all / 显式文件列表
BURST=20 bash tools/flagbench.sh PYADAMAS_BORROW_OPT bench/daily/*.py 15

# 12 基准指令数（默认管线）
bash tools/checkall.sh --perf --jobs 8

# 向量化覆盖率（VECT=0 vs 默认）
for c in matmul nbody sieve spectral_norm mandelbrot; do
  PYADAMAS_VECT=0 build/lead/pyadamas build -O2 --rebuild -o /tmp/v0 bench/compare/$c.py
  build/lead/pyadamas build -O2 --rebuild -o /tmp/v1 bench/compare/$c.py
  perf stat -e instructions:u /tmp/v0 2>&1 | grep instructions
  perf stat -e instructions:u /tmp/v1 2>&1 | grep instructions
done
```

> `tools/flagbench.sh` 是 0.481 新加的一次性 A/B 工具（从 0.474/0.475 的临时脚本收编）：
> 只切一个环境变量、两侧独立 `PYADAMAS_CACHE_DIR` + `--rebuild`、每轮打乱顺序 + 奇偶轮交换 A/B 先后、
> BURST 连跑取平均、报中位数与 p25。它不碰 `build/release`（0.271 oracle）。

## P1-A 虚调用去虚拟化（0.530 候选，**未合并**）

分支 `.wt/p1`（`44a5bea`），实测结论：**收益为零，且有健全性问题，已弃用**。

- 现状核实：`src/opt/devirt.cpp` 处理的是「经函数变量的间接调用」，不是 vtable 的 VCall；
  真正的 VCall 只在接收者静态类是**终态类**（`sealed_classes_`）时才降成直调。
- 做法：新增 `devirt_vcall.cpp`，按**槽号**扫所有类的 vtable，条目唯一就改直调。
- 实测：-O0 下确实变直调；1e7 次单态调用微基准 **0.07s → 0.07s**（-O1/-O2 本来就被内联）；
  12 基准 IR 里 **0 个 VCall**，perf 全 0 变化。
- **健全性问题（弃用原因）**：槽号是**每个类自己的布局**，不是全局方法 id。按槽号跨类匹配，
  会在「两个不相干类在同一槽号放不同方法」时改错目标——合并后 `370_textwrap`、
  `390_configparser_core` 立刻出现 OUTPUT DIFF（`'MiXeD'` 被 lower 成 `'mixed'`、
  textwrap 结果变成 `'[one two]\n[three]'`），`git reset` 回 0.529 后恢复。

若还要做，正确判据必须是「接收者静态类型已知 + 该类 vtable 该槽的目标 == 全类唯一实现 +
本模块无子类/无 import」，而不是只看槽号。

## P2-B union 形参单态化（延后，已测差距）

- 现状：union 形参每次调用有 `pa_is_named_class`（现已改 cid）+ `AnyFromF64` 装箱 +
  `any_to_f64` 解盒 + `rc_inc/dec_unknown`；
- 实测（-O2，1e6 次）：union 形参 **50 ns/次**（0.05 s）vs 形参是具体 float **~1 ns/次**（0.00 s）；
  CPython 3.14 同机 0.28 s；
- `int | float` 这类纯标量联合在类型层就归并成 `float`，IR 里形参直接是 `float`，**没有**这个问题；
- 真做单态化需要「克隆函数体 + 用替换后的形参类型**重跑一遍类型检查**」（降级取类型只认
  `tc_.expr_types_`），跨轮工作量；先做了 0.531 的 cid（1.46×），剩余收益待评估。
