# PyAdamas 测试与全闸门指南

**每轮改动后请跑 `bash tools/checkall.sh --full`，不要手写命令。**

## 一键全闸门：tools/checkall.sh

checkall.sh 把日常要手动跑的 5-6 条命令合成一条，输出汇总表（含每项耗时与总耗时），任一项失败非零退出。

```bash
bash tools/checkall.sh                 # --full（默认）：全部闸门
bash tools/checkall.sh --quick         # 日常：套件 + difftest + check_size
bash tools/checkall.sh --perf          # 只采集/比对 -O2 指令数基线
bash tools/checkall.sh --jobs 8        # 覆盖并行度（默认 16，上限 16）
bash tools/checkall.sh --write-baseline  # !! 人工确认后才用：覆盖基线文件
```

### 三种模式

| 模式 | 闸门 | 用途 |
|---|---|---|
| --full（默认） | 1 增量构建 + 2 套件 + 3 difftest + 4 check_size + 5 12基准x3档输出 + 6 向量化自检（默认 vs `VECT=0`，原来的 SIMD 变体闸门随 0.480 删特化改成这个）+ 7 git 状态 + 8 性能基线比对（LSP 冒烟 0.469 起已迁出） | 提交前 / CI |
| --quick | 2 套件 + 3 difftest + 4 check_size（可用 CHECKALL_SKIP_BUILD=1 跳过构建） | 日常快速回归 |
| --perf | 只跑性能基线的采集与比对 | 排查性能变化 / 验证告警 |

### 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| PYADAMAS_BIN | build/lead/pyadamas | 被测编译器 |
| SUITE_BUILD_DIR | build/gfix | 套件/difftest 的构建目录（多人共用时各设各的） |
| CHECKALL_SKIP_BUILD | 0 | =1 跳过增量构建（已构建过时省时间） |
| CHECKALL_WARN_PCT | 2 | 黄色警告阈值（%） |
| CHECKALL_FAIL_PCT | 5 | 红色失败阈值（%） |
| CHECKALL_PERF_REPS | 20 | 性能采集轮数（取最小） |
| BUILD_JOBS | 4 | 构建并行度（cmake -j） |
| CHECKALL_STRICT_GIT | 0 | =1 时 git status 非空即失败（多代理会话默认只提示） |
| PYADAMAS_TEST_FILTER | 空 | 套件用例过滤器（ASan 缩小范围用） |

## 并行度

**三个入口的默认值并不相同**：

| 入口 | 默认并行度 | 上限 |
|---|---|---|
| `tools/checkall.sh` | **16**（`--jobs N` 或 `PYADAMAS_TEST_JOBS` 覆盖） | 16（超出会被压回 16 并警告） |
| 它内部的 cmake 构建 | `BUILD_JOBS=4` | 无 |
| `tests/run_tests.sh`（直接跑） | **nproc** | 无 |
| `tools/difftest.sh`（直接跑） | **nproc** | 无 |

checkall 会把 `PYADAMAS_TEST_JOBS=$JOBS` 注入两个子脚本，所以走 checkall 时三者统一是 16。
想在所有入口都用一个值，就导出 `PYADAMAS_TEST_JOBS`——三个脚本都认它。

实测（2026-11-23；当时套件 420 例、difftest 392 例，当前为 469 / 440，耗时同量级、未在当前 HEAD 复测）：

| 项目 | JOBS=4 | JOBS=16 | JOBS=1 |
|---|---|---|---|
| run_tests（现 469 例） | 46.4s（2026-11-23，420 例） | 31.9s（同上） | — |
| difftest（现 440 例） | — | 17.6–19s（同上，392 例） | 172s（同上） |

JOBS=8 套件另测 35.1s（2026-11-23，介于 4 和 16 之间）。

difftest 是提速主力（172s → 19s，9 倍）；套件只有 1.4 倍（单例冷编译 120–190ms 已经很短）。
今后要再快，方向是共享 stdlib 模块缓存而不是加并行度。

## 用例命名：不再分配序号（2026-11-25 起）

`tests/run/` 与 `tests/fail/` 里**已有**的编号保留不动，**新增用例不再编号**，直接用描述性名字，
例如 `re_anchors.py`、`fnmatch_anchor.py`、`big_round.py`。

理由：编号是并行开发时的协调负担——两个代理会话里撞过两次（标准库第二轮占了 570–574，而主线已有
570/571；re 锚点修复又占了 573），每次都要重命名并改文档引用，收益为零。
`PYADAMAS_TEST_FILTER` 按 basename glob 匹配，名字本身就能当过滤器，不需要序号。

## 并发安全（多人同时跑时必读）

1. difftest 必须独占：它用共享的 /tmp/pyadamas-diff-cases 和 modcache。两个 difftest 并发会互相污染，出假 timeout。checkall.sh 已内置 flock /tmp/pyadamas-difftest.lock，不需要手动加。
   0.471 起脚本自身也兜了一层：首轮 DIFF/BUILDFAIL 的用例会单路复跑一次（20s → 90s 超时），复跑一致的记为「抖动」单独列出、不算失败；任一侧编不过则直接报 BUILDFAIL（旧版两侧都编不过会被判成一致，即「假绿」）。
2. 套件用独立构建目录：checkall.sh 默认用 build/gfix；多人同时跑时给每个人传不同的 SUITE_BUILD_DIR（例如 SUITE_BUILD_DIR=build/gfix-gates）。
3. 整个闸门串行化：如果多个人要跑完整闸门，用 flock 包住整条命令：
   ```bash
   flock /tmp/pyadamas-checkall.lock bash tools/checkall.sh --full
   ```
4. 构建：checkall.sh 用 cmake --build build/lead -j$(nproc)（ninja 自己会加锁）。不要在别人跑测试时 rebuild 到一半又跑测试。

## ASan 政策：默认完全不跑

**`--full` 和 `--quick` 都不含 ASan**。ASan 只给生成程序插桩，编译器本身不插桩；
整套闸门的耗时主要来自编译，因此开 ASan 的墙钟与普通模式接近（469 个用例、8 作业
上一轮实测），不要因为「会很慢」而跳过必要的 ASan 验证。

### 什么时候真的需要 ASan

- **需要**：`runtime/` 下的 RC/GC/EH/容器/字符串/文件内存路径改动；lowering 的所有权/cleanup 改动。
- **不需要**：类型检查、代码生成/向量化判据、文档/脚本/测试改动。

### 需要时的正确姿势：只跑相关用例，不要全量

```bash
PYADAMAS_TEST_FILTER='51[67]*' PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=16 \
    bash tests/run_tests.sh build/gfixa build/lead/pyadamas
```

`PYADAMAS_TEST_FILTER` 是现有过滤器（按 basename 的 shell glob），先用它缩小范围。
那 14/20 分钟就是「全量 + JOBS=1」跑出来的——别这么干；如果非要全量 ASan，用 `PYADAMAS_TEST_JOBS=16`。

### 性能告警与 ASan

`--perf` 发现某项回退 >5% 时只报红，**不会自动触发 ASan**。

## 性能基线

### 文件

tools/perf_baseline.txt：12 个基准在 -O2 下的 perf stat -e instructions:u 值，每行 name value，# 开头是注释（日期 / HEAD / 说明）。

### 采集口径

- 必须 instructions:u：perf stat 默认会算内核态指令，内核路径随负载变化，引入假差异。
- **`perf stat -x,` 的字段顺序是 `count,unit,event,run_time,percent`**：取指令数要 `$1`，
  `$4` 是 **run_time（纳秒）**。0.4x 版本曾误取 `$4`，于是把「墙钟时间」当成了指令数，
  表现为「同一二进制指令数差 40%」「多代理并发时误报 +39%」——那不是机器噪声，是取错字段。
- 取 3 轮最小即可：`instructions:u` 是**确定性**的，同一二进制重复跑只差几条（实测 ±0.001%）。
  可用 `CHECKALL_PERF_REPS` 调整。
- 绑核（`taskset`）与 `MALLOC_ARENA_MAX=1` 只是让个别分配相关路径更稳定，对指令数判定不是必需的。
- 每轮 `build --rebuild -O2` 强制真编译（codegen 改动不进 modcache 键，不 rebuild 会命中旧产物——这个坑踩过两次）。

### 告警阈值

| 变化 | 行为 |
|---|---|
| ≤ +2% | OK（噪声内） |
| +2% ~ +5% | 黄色 WARN（打印表格，不失败） |
| > +5% | 红色 FAIL（非零退出） |
| 负值（变好） | 只打印，不失败 |

### 更新基线

只在人工确认这次变更是有意的性能变化后才更新，不要因为一次红就改：

```bash
bash tools/checkall.sh --write-baseline
```

脚本会打印醒目警告，并重新采集全部 12 个基准覆盖 tools/perf_baseline.txt。

### 噪声有多大

- **`instructions:u` 是确定性的**：同一二进制跑两次只差几条（实测 3 轮分别 44,080,443 / 44,080,444 / 44,080,444）。
- 历史文档里「同一二进制差 20%~40%」「多代理并发误报 +39%」的记载，**是 `checkall.sh` 早期误取 `$4`(run_time) 造成的假象**，已修；
  现在机器满载跑 `--perf` 也不受影响（同一字节码就是同一指令数）。
- 仍建议 `--perf` / `--full` 用 `flock /tmp/pyadamas-checkall.lock` 串行化：不是为指令数准，而是避免**构建/模块缓存**互相踩。
- 阈值保持 +2% 警告 / +5% 失败：相对于 ±0.001% 的实际噪声这里留了很宽的余量，
  好处是能容忍「不同机器/不同 libc 下几条指令的差异」，坏处是 <2% 的真实回退看不出来（需要对比 asm 或用 `tools/opt_stats.sh`）。

## 当前基线值（2026-11-23，HEAD 18cd374）

基线与 HEAD 无关，按 `tools/perf_baseline.txt`（下表只是当时快照，判定以文件为准）。

| 基准 | instructions:u (-O2) |
|---|---:|
| binary_trees | 4,748,393 |
| collatz | 81,504,584 |
| fannkuch | 34,927,066 |
| fasta | 5,956,391 |
| fib | 130,640,135 |
| mandelbrot | 44,080,444 |
| matmul | 10,127,395 |
| nbody | 109,040,726 |
| nqueens | 339,789,107 |
| sieve | 19,631,913 |
| spectral_norm | 94,638,050 |
| wordfreq | 251,337,123 |

> 口径提示：这几个数字是**用户态指令数**，与「墙钟」无关。mandelbrot 的 44.08M 是 masked 向量化之后的值（原 69.31M），
> sieve 的 19.63M 是类型推断修复 + 跨步向量化之后的值（最初 104.39M）。

## 实测总耗时

| 模式 | 总耗时 | 备注 |
|---|---:|---|
| --full | 2026-11-23 实测 76.8s（当时 9 道闸门含 LSP 冒烟；现 8 道，未在当前 HEAD 复测） | 8 道闸门，全部 PASS；469 ran ok / 440 difftest 0 不一致 / 36/36 基准逐字节一致 |
| --quick | 2026-11-23 实测 53.1s（未在当前 HEAD 复测） | 构建+套件+difftest+check_size，全部 PASS |
