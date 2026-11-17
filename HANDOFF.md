# 交接文档 —— 0.270（2026-11-18）

> 这是一份**时点快照**，记录本轮结束时服务器上的可运行状态、怎么跑、
> 以及下一步该做什么。它不属于架构文档，下次交接时直接覆盖。

## 1. 环境

| 项 | 值 |
|---|---|
| 仓库 | `/src/luo-porj`（分支 `master`，HEAD `f079356`，与 origin 同步）|
| 服务器 | `root@110.42.51.202`，Debian 13 (trixie)，Xeon Gold 6252 @ 2.10GHz / 96 线程 / 123GB |
| 工具链 | gcc 15.2、cmake 3.31.6、ninja 1.12.1、git 2.47.3、python 3.13.5 |
| 客户端 | `C:\Windows\System32\OpenSSH\ssh.exe`，密钥 `C:\Users\lenovo\.ssh\id_ed25519_root_mc`，已配置别名 `porj` |
| 构建目录 | `build/release`（优化版）、`build/dev`（调试）、`build/asan`（插桩运行时）|

**不要用本地 WSL**。所有构建、测试、基准都在服务器上跑。

## 2. 常用命令

```bash
# 构建（-j32 比 -j96 快：-fmodules-ts 的模块编译是串行的，线程太多反而抢 CPU）
ssh porj 'cd /src/luo-porj && cmake --build build/release -j32'

# 差分测试：同一份源码在 -O0 / -O2 下跑，比输出（188 个用例，约 6 秒）
ssh porj 'cd /src/luo-porj && bash tools/difftest.sh'
ssh porj 'cd /src/luo-porj && PYADAMAS_TEST_FILTER=nbody bash tools/difftest.sh'   # 只跑匹配的

# 全量套件（206 个 ran ok；退出码 0 才算过）
ssh porj 'cd /src/luo-porj && bash tests/run_tests.sh build/release build/release/pyadamas'

# 单版本回归计时（最好 N 次；REP / PYADAMAS_BIN / OUT 可覆盖）
ssh porj 'cd /src/luo-porj && REP=7 bash tools/quickbench.sh'
ssh porj 'cd /src/luo-porj && bash tools/quickbench.sh nbody matmul spectral_norm'

# A/B 两版本对比（独立 worktree + 交替测量 + 中位数）
ssh porj 'cd /src/luo-porj && bash tools/abbench.sh <旧ref> <新ref>'

# 编译并手动跑单个程序
ssh porj 'cd /src/luo-porj && build/release/pyadamas build -o /tmp/x bench/realistic/queue_sim.py && /tmp/x'
ssh porj 'cd /src/luo-porj && build/release/pyadamas asm bench/realistic/queue_sim.py'   # 看生成的汇编
ssh porj 'cd /src/luo-porj && build/release/pyadamas ir  bench/realistic/queue_sim.py'   # 看 IR
```

**测量纪律**：服务器上还有别的租户（Minecraft 服等），负载在 3–22 之间波动。
**只能用交替 A/B 测量，不能前后顺序测** —— 顺序测会把负载波动当成性能差异。
`tools/abbench.sh` 和 `tools/quickbench.sh`（取最好 N 次）都是为此设计的。

## 3. 当前状态

| 项 | 值 |
|---|---|
| 版本 | 0.270 |
| 差分测试 | 188 用例 / 0 不一致 |
| 全量套件 | 206 ran ok / exit 0 |
| 文件规模检查 | **会失败**（5 个文件超 1200 行，见 §6）|

### 基准（服务器实测，与 Go 交替取最优）

| bench | 本轮开始 | 现在 | 变化 | Go | 倍数 |
|---|---|---|---|---|---|
| queue_sim | 420 ms | **286 ms** | −31.9% | 40 ms | 10.5× → 7.05× |
| str_build | 496 ms | **220 ms** | −55.6% | 112 ms | 4.4× → 1.97× |

str_build 已经接近和 Go 打平。queue_sim 还差 7 倍，瓶颈见 §5。

## 4. 本轮做了什么（0.270）

全部已合并并推送。按时间序：

| 提交 | 内容 | 效果 |
|---|---|---|
| `fdea7b8` | 短串拷贝/比较内联（`sm_copy_bytes`/`sm_bytes_eq`/`sm_bytes_cmp`，n≤8 内联、n>8 回落 libc）；`join` 跳过空分隔符；`pa_str_from_i64` 个位数驻留 | str_build −37% |
| `8b68271` | 常数除数除法改魔数乘法（Granlund-Montgomery），2 的幂单独走移位 | queue_sim −13%、str_build −10% |
| `89b41ee` | RC 记账（inc/dec/mark/cleanup）生成代码内联快路径 | 两者各 −20% |
| `455df2c` | 修 `str(单个数字)` 返回控制字符（`pa_char_str` 参数是字符编码不是数值）+ 新增回归用例 | — |
| `d48b842` | 单字节 str 字面量接运行期驻留表 `pa_char_tab` + str `==`/`!=` 指针快路径 | 各 −3% |
| `16aefbf` | RC 记账内联从 `codegen_call.cpp` 搬到 `codegen_call_rt.cpp`（回到 1200 行以内）| 纯搬家 |
| `e3f7315` | 文档漂移清理（codegen/runtime/str-codepoints/regalloc×2/stack-alignment/index/tools/bench/dev/roadmap/README）| — |
| `f079356` | 补 CHANGELOG 0.244–0.269（26 个版本）+ ir.md 的 SSA 最终结论 | — |

### 三个关键结论（都是实测得出，不要当假设）

1. **寄存器容量不是瓶颈。** `PYADAMAS_RA_POOLCAP` 把槽寄存器池 6→3，性能只掉 0–8%、均值约 3%。
   所以「区间级所有权 / SplitKit + InlineSpiller」第三层正式放弃（理论上限 1–2%）。
2. **常数除数的 `idivq` 曾是最大单点开销。** `perf annotate` 指到指令级：queue_sim 的 `simulate`
   里 `t % 3` + `t % 7` 合计占该函数 47.6% 采样，str_build 的 `build_line` 里 `i % 10` 占 71.9%。
3. **差分测试有盲区**：它只比最终输出。本轮两个 bug（`str(单数字)` 返回 BEL、`!=` 慢路径忘了取反）
   **187 个用例全过**，是新增的 `tests/run/187_str_smallint_divconst.py` 抓到的。
   以后改字符串/数值转换路径，先想想输出是否恰好抵消了错误。

## 5. 下一步该做什么

### 首选：queue_sim 的列表生命周期

queue_sim 每次循环迭代创建一个列表（`rest = []`）并销毁一个（旧的 `pending`），
5.2M 步 = 1040 万次列表创建/销毁。当前 profile：生成代码 47%、列表+RC 生命周期 36%
（`rc_alloc` 11%、`list_buf_get` 8%、`list_destroy` 8%、`pa_rc_dec_slow` 6%、`rc_link` 5%、`rc_unlink` 4%）。

两个候选（按风险从低到高）：

- **header + buffer 合并分配**：现在建一个列表要两次池操作（`list_pool_get` 拿 48 字节头，
  `list_buf_get` 拿 4 元素缓冲）。合成一次能省掉一次池查找 + 一次分支。预计 10–15%。
  `list_destroy` / `list_buf_put` 要同步改（缓冲内联时不能 free）。
- **切片惯用法识别**：`rest = []; for k in range(a, b): rest.append(x[k])` 识别成一次
  `memcpy` + 一次 retain 批量处理，把 2–4 次 append + 每元素 RC 全变成一次拷贝。
  收益更大（可能 20%+），但要动降级器，风险也更大。

### 次选：其它基准

- `dict_wordfreq` 1.71×、`matmul` 2.11×、`nbody` 2.16×、`obj_dispatch` 3.06×、
  `spectral_norm` 5.11×（浮点代码生成，值得单独看）
- `queue_sim` 里 `pa_rc_dec_slow` 占 6% —— 那是真的在释放对象，属于必要工作，不要试图消掉

## 6. 已知问题（不要当成新发现重复排查）

1. **`tests/check_size.sh` 会失败**：`codegen_regalloc.cpp`(2461)、`driver_build.cpp`(1421)、
   `ir_ops.h`(1245)、`lower_impl.h`(1207)、`typecheck_types.h`(1296) 五个文件超 1200 行。
   规则是「清单只能删不能加」，所以新增代码不要再往这五个文件里堆。
   本轮 `codegen_call.cpp` 曾因 RC 内联涨到 1219 行，已通过搬到 `codegen_call_rt.cpp` 修回 1152。
2. **生成代码的 RC 内联不读统计开关**：`PYADAMAS_RC_STATS=1` 时，生成代码里的 inc/dec
   走的是内联快路径，**不读 `pa_rc_stats_on_fast`**，所以统计会漏。运行时内部的
   `pa_rc_inc_fast` 是读的。要精确统计得临时关掉内联（`src/codegen_call_rt.cpp` 的 `gen_rc_inline`）。
3. **`docs/codegen.md` 还有两处已知漂移没修**（本轮 agent 发现但没敢改）：
   - §对象与虚调用 的偏移图写 `-16 rc / -8 dtor`，那是 32 字节头的遗留；现在是 64 字节头，`h->rc` 在 `p-48`。
   - §调用约定 表里「栈对齐」行写「压栈字节数向上取整到 16 的倍数再 subq」，与同文帧布局段矛盾
     （实际是出参区在帧里一次性预留、调用点不动 rsp）。
4. **`docs/runtime.md` 的历史实测段**（163 个用例、179 pass 那些）是 M4 那轮的记录，
   不是现状数字，不要按它做判断。
5. **`src/codegen_block.cpp:343` 的注释过时**：说「运行时 traceback 仍走 rbp 链」，
   实际早改成 CFI 展开器（`_Unwind_Backtrace`）。

## 7. 版本号约定（踩过一次坑）

`0.x` 阶段每个「能跑通新一层」算一个次版本。**每次提交前先**：

```bash
git log --format="%s" -300 | grep -oE "0\\.(2[0-9]{2})" | sort -u -V | tail -3
grep -oE "^## \\[0\\.[0-9]+\\]" CHANGELOG.md | head -1
```

两者应该一致。本轮一度按「CHANGELOG 顶格 0.242，下一个就是 0.243」编号，
但 git 历史实际已经到 **0.269**（那 26 个版本的提交都没更新 CHANGELOG）。
正确编号是 0.270，0.243 这个号从未被使用过。已回填修复。

## 8. 工具清单

| 工具 | 用途 |
|---|---|
| `tools/difftest.sh` | 差分测试，并行。`PYADAMAS_TEST_JOBS` 并行度、`PYADAMAS_TEST_FILTER` 过滤、自动跳过 `*_perf` |
| `tools/quickbench.sh` | 单版本回归计时，最好 N 次。`REP=` / `PYADAMAS_BIN=` / `OUT=` |
| `tools/abbench.sh` | A/B 两版本对比（独立 worktree + 交替测量 + 中位数）|
| `tools/sync-server.sh` | rsync 同步到服务器，带排除表。`--dry-run` / `--pull` / `--build` |
| `tools/irdump.cpp` | IrOp / RtFn 枚举编号 → 名字对照 |

## 9. 环境变量速查

| 变量 | 作用 |
|---|---|
| `PYADAMAS_RA_POOLCAP` | 槽寄存器池上限（实验用，默认不设 = 满池 6）|
| `PYADAMAS_RA_SPLIT=1` | 块级 live-range 分裂（默认关，实测 +28%）|
| `PYADAMAS_RA_GRAPH=1` | 实验性图着色分配器（默认关，matmul −8.6%）|
| `PYADAMAS_LI_STATS` | 精确 LiveInterval 峰值寄存器压力诊断（只读，不影响生成结果）|
| `PYADAMAS_ALIGN_CHECK=1` | 函数入口自检 `rsp % 16 == 8`，不满足就 `ud2` |
| `PYADAMAS_RC_STATS=1` | RC 统计（注意 §6.2 的盲区）|
| `PYADAMAS_TEST_JOBS` / `PYADAMAS_TEST_FILTER` | difftest 并行度 / 用例过滤 |
| `PYADAMAS_TEST_ASAN=1` | 用 ASan 插桩的运行时跑套件 |

## 10. 仓库结构速查

```
src/           C++23 前端：词法 → AST → 脱糖 → 类型检查 → IR → codegen
runtime/       C11 运行时（38 个 .c，按职责拆分后用 ld -r 合成 rt.o）
stdlib/        纯 Python 写的标准库模块
bench/         builtins/（逐个内置函数）、realistic/（整程序）、compare/（四语言对比）
tests/         run/（188+1 个用例）、fail/、cdecl/、cfi/、modules/
docs/          架构与专题文档（index.md 是地图）
tools/         差分测试 / 基准 / 同步 / IR dump
```

## 11. 结论

现在状态可以直接继续开发：构建干净、测试全绿、基准可复现。
接手后第一件事建议跑一遍 `quickbench.sh` 和 `difftest.sh` 确认基线，
然后按 §5 从 queue_sim 的列表生命周期开始。
