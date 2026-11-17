# 交接文档 —— 0.271（2026-11-18）

> 这是一份**时点快照**，记录本轮结束时服务器上的可运行状态、怎么跑、
> 以及下一步该做什么。它不属于架构文档，下次交接时直接覆盖。

## 0. 路径约定

仓库在服务器的 `/src/luo-proj/pyadamas`，但你**不需要记这个绝对路径**。
下面所有命令都从仓库根出发，用相对路径。进入仓库：

```bash
R=/src/luo-proj/pyadamas          # 只在这里出现一次
ssh porj "cd $R && ..."            # 其余命令都用相对路径
```

本地（Windows）侧的脚本也都不写死绝对路径：`tools/difftest.sh`、
`tools/quickbench.sh`、`tools/abbench.sh` 都从脚本自身位置推导仓库根，
换目录、换机器、换 checkout 都不用改。`tools/sync-server.sh` 的远端路径由
`PYADAMAS_SYNC_TARGET` 覆盖（默认就是上面那个）。

## 1. 环境

| 项 | 值 |
|---|---|
| 仓库 | `pyadamas`（分支 `master`，HEAD 见本轮提交，与 origin 同步）|
| 服务器 | `root@110.42.51.202`，Debian 13 (trixie)，Xeon Gold 6252 @ 2.10GHz / 96 线程 / 123GB；**容器 cpuset 24-47,72-95，实际可用 48 核** |
| 服务器路径 | `/src/luo-proj/pyadamas` |
| 工具链 | gcc 15.2、cmake 3.31.6、ninja 1.12.1、git 2.47.3、python 3.13.5 |
| 客户端 | OpenSSH，密钥 `~/.ssh/id_ed25519_root_mc`，ssh 别名 `porj` |
| 构建目录 | `build/release`（优化版）、`build/dev`（调试）、`build/asan`（插桩运行时）、`build/lead`（celestea 自建，同为 0.271）|

**会话用户不一定是 root。** 本轮 agent 以 celestea（uid 1003，celesdev 组）运行。
仓库文件多为 uid 1000 / root 所建，目录组可写；git 已设
`core.sharedRepository=group`（谁建的对象都组可写），celestea 的全局 git
配置已加 `safe.directory`。遇到权限墙时：

- harness 的 HOME 是 `/srv/dsh-dev`，**celestea 的真实 home 是 `/home/celestea`**，
  密钥在后者的 `.ssh` 里（`ls ~/.ssh` 会扑空，别据此断定没有密钥）。
- root@本机：
  `ssh -i /home/celestea/.ssh/id_ed25519_root_mc -o BatchMode=yes -o UserKnownHostsFile=/home/celestea/.ssh/known_hosts root@127.0.0.1`
  （`/etc/ssh` 是 700，不带 `UserKnownHostsFile` 会 host key 检查失败）。
- GitHub push 密钥在 `/root/.ssh`（`id_ed25519_pyadamas_github` 等），
  celestea 没有密钥，push 要走上面的 root ssh。
- root 干活建议 `umask 002`，否则新建文件 644/755 组无写权限，会给下一个
  非 root 会话留坑（git 对象除外，sharedRepository 已兜底）。

**不要用本地 WSL**。所有构建、测试、基准都在服务器上跑。

## 2. 常用命令

下面 `$R` 指仓库根。全部用相对路径，在 `cd "$R"` 之后执行。

```bash
R=/src/luo-proj/pyadamas

# 构建（-j32 比 -j96 快：-fmodules-ts 的模块编译是串行的，线程太多反而抢 CPU）
ssh porj "cd $R && cmake --build build/release -j32"

# 差分测试：同一份源码在 -O0 / -O2 下跑，比输出（189 个用例，约 6 秒）
ssh porj "cd $R && bash tools/difftest.sh"
ssh porj "cd $R && PYADAMAS_TEST_FILTER=nbody bash tools/difftest.sh"   # 只跑匹配的

# 全量套件（207 个 ran ok；退出码 0 才算过）
ssh porj "cd $R && bash tests/run_tests.sh build/release build/release/pyadamas"

# 单版本回归计时（最好 N 次；REP / PYADAMAS_BIN / OUT 可覆盖）
ssh porj "cd $R && REP=7 bash tools/quickbench.sh"
ssh porj "cd $R && bash tools/quickbench.sh nbody matmul spectral_norm"

# A/B 两版本对比（独立 worktree + 交替测量 + 中位数）
ssh porj "cd $R && bash tools/abbench.sh <旧ref> <新ref>"

# 编译并手动跑单个程序（产物放 /tmp，不要污染仓库）
ssh porj "cd $R && build/release/pyadamas build -o /tmp/x bench/realistic/queue_sim.py && /tmp/x"
ssh porj "cd $R && build/release/pyadamas asm bench/realistic/queue_sim.py"   # 看生成的汇编
ssh porj "cd $R && build/release/pyadamas ir  bench/realistic/queue_sim.py"   # 看 IR
```

**测量纪律**：服务器上还有别的租户（Minecraft 服等），负载在 3–22 之间波动。
**只能用交替 A/B 测量，不能前后顺序测** —— 顺序测会把负载波动当成性能差异。
`tools/abbench.sh` 和 `tools/quickbench.sh`（取最好 N 次）都是为此设计的。

**abbench 现在自动从 `Cpus_allowed_list` 取第一个可用 CPU 并自检 taskset。**
它曾写死 `taskset -c 0`，在容器 cpuset（24-47,72-95）里直接失败，六个用例
全量出 ~12.5ms 的「失败开销」假数字。**A/B 数字先和 quickbench 对数量级，
量级不对先怀疑测量本身失败**，再怀疑性能。

## 3. 当前状态

| 项 | 值 |
|---|---|
| 版本 | 0.271 |
| 差分测试 | 189 用例 / 0 不一致 |
| 全量套件 | 207 ran ok / exit 0（release 与 ASan 套件都过）|
| 文件规模检查 | **会失败**（5 个文件超 1200 行，见 §6）|

### 基准（abbench 交替 9 轮中位数，绑核；0.270 → 0.271）

| bench | 0.270 | 0.271 | 变化 |
|---|---|---|---|
| queue_sim | 295.25 ms | **284.01 ms** | −3.8% |
| sort_struct | 335.91 ms | **267.74 ms** | −20.3% |
| str_build | 221.17 ms | 219.11 ms | −0.9% |
| dict_wordfreq | 310.72 ms | 306.94 ms | −1.2% |
| obj_dispatch | 553.76 ms | 549.39 ms | −0.8% |
| rec_memo | 454.91 ms | 457.77 ms | +0.6%（噪声）|

quickbench 最好 7 次（不绑核）：queue_sim **283 ms**、str_build **214 ms**。
对 Go 大约 queue_sim ~7×、str_build ~1.9×（Go 侧沿用上轮 40 / 112 ms）。

## 4. 本轮做了什么（0.271）

| 提交 | 内容 | 效果 |
|---|---|---|
| `27956ec` | 小列表 header+buffer 合并分配 + 回归用例 188 + abbench 绑核修复 + runtime.md | sort_struct −20.3%、queue_sim −3.8%、六用例合计 −4.0% |
| `6a23bdc` | 覆盖本文档 | — |

实现要点：

- 容量 <=8 元素的列表（含 `pa_list_new_filled` / `pa_tuple_new_filled` 定长
  快路径）把 data 缓冲内联在「RC 头 + PyAdamasList」同一 malloc 块尾部
  （64+48+64=176 字节，`list_inline_pool`，最多 64 块）。`rc_alloc` 拆出
  `rc_alloc_ex(..., extra)`，释放按 `h->size` 路由（48 → list_pool，
  112 → inline pool）；untracked 变体同步（kind 0 元组）。
- **内联缓冲是块内指针，不能 realloc / free**：append / insert / 切片赋值
  三处增长统一走 `list_buf_grow`（内联时换 malloc + memcpy，内联区闲置，
  头块销毁时整块回池复用）；`list_destroy` 与 GC 清扫 `gc_free_body` 按
  `data == list_inline_at(l)` 跳过块内指针的单独释放。**以后新增碰
  `l->data` 的代码，先想这条不变量。**
- GC 记账把内联缓冲计入（原先缓冲字节不入账）；实测 GC 占比仍 0.06%，无副作用。

### 三个结论（实测得出）

1. **合并分配的收益取决于小列表构造占比**：sort_struct −20.3%，queue_sim
   −3.8%（低于交接预期的 10–15%）——`list_buf_get` 从 profile 榜上消失，
   但 `rc_link`/`rc_unlink` 和逐元素 retain 还在，它们才是 queue_sim 的大头。
2. **测量工具会静默产出假数字**：abbench 写死 taskset -c 0，容器里失败，
   量出六个 ~12.5ms。已修（自动选 CPU + 自检），但「数字先对数量级」要成为习惯。
3. **差分测试盲区仍在**（0.270 的教训）：本轮新用例
   `tests/run/188_list_inline_buf.py` 先对拍 CPython 逐行一致再钉输出，
   覆盖三条增长路径越界、堆元素高频建/销、自环 GC churn、元组快路径、混合种类。

## 5. 下一步该做什么

### 首选：切片惯用法识别（queue_sim）

`rest = []; for k in range(a, b): rest.append(pending[k])` 识别成一次
memcpy + 一次批量 retain，把 2–4 次 append + 每元素 RC 变成一次拷贝。
queue_sim 当前 profile：生成代码 **54.4%**、`rc_alloc_ex` 8%、
`list_destroy` 7.2%、`pa_rc_dec_slow` 6.8%、`rc_alloc_untracked_ex` 5%、
`rc_link` 4% + `rc_unlink` 3.2%、`pa_tuple_new_filled` 3.7%、
memset 1.5%、`pa_rc_dec_unknown` 1.7%、GC 0.06%。预计 20%+，但要动降级器，
风险比合并分配大。

### 次选：其它基准

spectral_norm（浮点代码生成，0.270 快照 5.11×）、obj_dispatch、nbody、
matmul —— 本轮只测了 realistic 六用例，compare 套件数字要重测。
`pa_rc_dec_slow` 的 6.8% 是真的在释放对象，属于必要工作，不要试图消掉。

## 6. 已知问题（不要当成新发现重复排查）

1. **`tests/check_size.sh` 会失败**：`codegen_regalloc.cpp`(2461)、
   `driver_build.cpp`(1421)、`ir_ops.h`(1245)、`lower_impl.h`(1207)、
   `typecheck_types.h`(1296) 五个文件超 1200 行。清单只能删不能加，
   新增代码不要再往这五个文件里堆。
2. **生成代码的 RC 内联不读统计开关**：`PYADAMAS_RC_STATS=1` 时生成代码的
   inc/dec 走内联快路径，不读 `pa_rc_stats_on_fast`，统计会漏。要精确统计
   得临时关掉内联（`src/codegen_call_rt.cpp` 的 `gen_rc_inline`）。
3. **`docs/codegen.md` 两处已知漂移没修**：§对象与虚调用 偏移图写
   `-16 rc / -8 dtor`（32 字节头遗留，现在 64 字节头，`h->rc` 在 `p-48`）；
   §调用约定 表里「栈对齐」行与同文帧布局段矛盾（实际出参区在帧里一次性
   预留、调用点不动 rsp）。
4. **`docs/runtime.md` 的历史实测段**（163 个用例、179 pass 那些）是 M4
   那轮的记录，不是现状数字。列表节本轮已更新为合并分配。
5. **`src/codegen_block.cpp:343` 的注释过时**：说「运行时 traceback 仍走
   rbp 链」，实际早改成 CFI 展开器（`_Unwind_Backtrace`）。
6. **移动仓库目录后必须重建构建目录**：`build/*/CMakeCache.txt` 写死源码
   绝对路径。本轮已重建 `build/dev`（旧 cache 指向迁移前的 `/src/luo-porj`，
   stdlib 路径都编错了）；`build/release` 增量重建到 0.271。两个目录都已
   `chmod -R g+w`，celestea 可直接维护。
7. **`/etc/ssh` 是 700**：非 root 的 ssh / git-ssh 必须带
   `-o UserKnownHostsFile=<可写路径>`，否则 host key 检查失败（见 §1）。
8. **root 跑文件类用例会在 /tmp 留 root 所有的文件**，celestea 再跑
   79/85/91/172/174 会 OSError/PermissionError（本轮已清理）。再遇到时
   root 清掉，或把套件放进 bwrap 跑（私有 /tmp）：
   `bwrap --bind / / --tmpfs /tmp --dev /dev --proc /proc --die-with-parent -- bash tests/run_tests.sh build/release build/release/pyadamas`

## 7. 版本号约定（踩过一次坑）

`0.x` 阶段每个「能跑通新一层」算一个次版本。**每次提交前先**：

```bash
git log --format='%s' -300 | grep -oE '0\.[0-9]{3}' | sort -u -V | tail -3
grep -oE '^## \[[0-9.]+\]' CHANGELOG.md | head -1
```

两者应该一致。现在都是 **0.271**，下一个号是 **0.272**。
（0.270 那轮曾误按 CHANGELOG 顶格编号，git 历史才是权威，见 CHANGELOG 回填说明。）

## 8. 工具清单

| 工具 | 用途 |
|---|---|
| `tools/difftest.sh` | 差分测试，并行。`PYADAMAS_TEST_JOBS` 并行度、`PYADAMAS_TEST_FILTER` 过滤、自动跳过 `*_perf`。默认用 build/dev/pyadamas，可传参换 |
| `tools/quickbench.sh` | 单版本回归计时，最好 N 次。`REP=` / `PYADAMAS_BIN=` / `OUT=` |
| `tools/abbench.sh` | A/B 两版本对比（独立 worktree + 交替测量 + 中位数）。自动选 cpuset 第一个可用 CPU 并自检 taskset |
| `tools/sync-server.sh` | rsync 同步到服务器，带排除表。`--dry-run` / `--pull` / `--build` |
| `tools/irdump.cpp` | IrOp / RtFn 枚举编号 → 名字对照 |

所有工具脚本都从**自身位置**推导仓库根（`ROOT=$(cd "$(dirname "$0")/.." && pwd)`），
不写死绝对路径。新增工具请照这个约定。

## 9. 环境变量速查

| 变量 | 作用 |
|---|---|
| `PYADAMAS_RA_POOLCAP` | 槽寄存器池上限（实验用，默认不设 = 满池 6）|
| `PYADAMAS_RA_SPLIT=1` | 块级 live-range 分裂（默认关，实测 +28%）|
| `PYADAMAS_RA_GRAPH=1` | 实验性图着色分配器（默认关，matmul −8.6%）|
| `PYADAMAS_LI_STATS` | 精确 LiveInterval 峰值寄存器压力诊断（只读，不影响生成结果）|
| `PYADAMAS_ALIGN_CHECK=1` | 函数入口自检 `rsp % 16 == 8`，不满足就 `ud2` |
| `PYADAMAS_RC_STATS=1` | RC 统计（注意 §6.2 的盲区）|
| `PYADAMAS_TEST_JOBS` / `PYADAMAS_TEST_FILTER` | difftest / 套件的并行度 / 用例过滤 |
| `PYADAMAS_TEST_ASAN=1` | 用 ASan 插桩的运行时跑套件 |
| `PYADAMAS_SYNC_TARGET` / `PYADAMAS_SYNC_KEY` | sync-server.sh 的目标与密钥 |

## 10. 仓库结构速查

```
src/           C++23 前端：词法 → AST → 脱糖 → 类型检查 → IR → codegen
runtime/       C11 运行时（38 个 .c，按职责拆分后用 ld -r 合成 rt.o）
stdlib/        纯 Python 写的标准库模块
bench/         builtins/（逐个内置函数）、realistic/（整程序）、compare/（四语言对比）
tests/         run/（189 个差分用例）、fail/、cdecl/、cfi/、modules/
docs/          架构与专题文档（index.md 是地图）
tools/         差分测试 / 基准 / 同步 / IR dump
```

## 11. 结论

现在状态可以直接继续开发：release/dev 已重建到 0.271 且组可写，交接里的
规范命令 celestea 可直接跑，测试全绿，基准可复现。接手后第一件事建议跑一遍
`quickbench.sh` 和 `difftest.sh` 确认基线，然后按 §5 从切片惯用法识别开始。
