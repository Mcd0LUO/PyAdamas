# pyadamas 编译器自身构建速度优化（测量先行）

分支 `speed`（worktree /tmp/wt-speed，基线 commit 3e94c26 = 0.306）。
主口径：**构建编译器项目本身的墙钟时间**（cmake --build 全量/增量）；
副口径：pyadamas 编译用户程序的吞吐（time pyadamas build bench/realistic/*.py）。

## 0. 测量环境（多租户，所有数字都带负载注记）

- CPU：Intel Xeon Gold 6252 @ 2.10GHz，整机 96 逻辑核；本会话 affinity 被限制在
  24-47,72-95（48 逻辑核 ≈ 24 物理核，超线程）。统一 `-j32`。
- **多租户**：主树另有代理在集成 vthread（持续构建/跑测试），整机 load 在测量窗口内
  3～49 波动。同一配置的重复测量可差 1.8×（dev 全量 46.7s vs 85.6s）。
  因此所有 A/B 采用**交替轮转**（base,exp,base,exp,…）抵消负载漂移，取中位数。
- 工具链：GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1)，CMake 4.2.3，Ninja 1.13.2，
  GNU ld 2.46（**仅 bfd**；lld/mold/gold 均不存在，uid=1003 无安装权限）。
  ccache 4.12.3 可用。
- 构建结构：89 个 C++ TU（C++23，-Wall -Wextra -Werror …）+ 34 个运行时 C 部件
  （固定 -O2）+ 34 个 ASan 运行时 C 部件（固定 -O1+asan，`ALL` 默认构建）
  + 2 次 ld -r 合并 + 1 次可执行链接。
- 预处理规模（compile_commands 逐 TU `-E` 计行）：**总计 10,535,456 行**，
  中位 125,800 行/TU，最大 driver/build.cpp 145,572 行；~95% 是标准库头文本。
  （docs/development.md 的「10 万行」是单 TU 口径。）
- 相位分析（driver/build.cpp，dev flags，`-ftime-report -fsyntax-only`，10.25s）：
  parsing 66%（其中 **template instantiation 53%**、overload resolution 29%），
  preprocessing 仅 3%，deferred templates 34%。
  → 瓶颈是**每个 TU 重复实例化标准库模板**，不是预处理文本量。
- 头文件包含分析（传递闭包，89 TU）：
  - 项目头：common/token.h/ast.h 80 TU，syntax/parser.h/lexer.h 79，typecheck/typecheck.h 74，
    **typecheck/types.h 74 TU**（全部经 typecheck/typecheck.h 间接包含，无直接包含），
    ir/ops.h/ir_types.h 62，ir/module.h 60，lower/lower.h 41。
  - 标准库头（按覆盖 TU 数）：vector 88、string 86、**format 85**、algorithm 82、
    memory 82、optional 82、unordered_map 81、variant 80、**ranges 74**、
    functional 74、map 63、**expected 61**。
  - 重构方向（未做，太深）：typecheck/types.h 被 74/89 TU 拖着走；
    <format>/<ranges>/<expected> 是 libstdc++ 最重的模板头，
    项目头里「顺手 include」是主要扩散源。

## 1. 基线（原始 CMake，-j32，clean 全量 = ninja -t clean 后构建）

configure 耗时：dev 1.49s，release 1.34s（一次性，可忽略）。

注意：仓库 dev preset 实际是 `-g -Og`、release preset 实际是 `-O3 -DNDEBUG`
（CMake 默认）；按任务口径基线用裸 `-DCMAKE_BUILD_TYPE=Debug`（= -g/-O0）
与 `Release`（= -O3 -DNDEBUG）。

| 配置 | 3 次 clean 全量 (s) | 中位 | 负载窗口 (1min load) |
|---|---|---|---|
| dev (-O0 -g) 第一轮 | 84.95 / 48.57 / 61.80 | **61.80** | 16→36 |
| dev (-O0 -g) 第二轮 | 46.67 / 85.56 / 73.62 | **73.62** | 8→42 |
| dev 交替对照轮（负载平稳） | 48.49 / 51.95 / 48.33 | **48.49** | ~29 |
| release (-O3) 第一轮 | 66.80 / 70.45 / 91.25 | **70.45** | 36→49 |
| release (-O3) 第二轮 | 60.70 / 59.62 / 58.88 | **59.62** | 42→30 |

docs/development.md 标称 idle 机器 dev 全量 ~4.7s；本测量窗口机器从未完全空闲，
绝对值偏大是负载所致，**A/B 结论以交替轮转的相对差为准**。

### Top10 重 TU（.ninja_log，第二轮最后一次构建，负载下绝对值）

| dev (-O0) | s | release (-O3) | s |
|---|---|---|---|
| driver/build.cpp | 45.4 | driver/build.cpp | 35.7 |
| lower/class.cpp | 43.7 | typecheck/expr.cpp | 31.6 |
| lower/lower.cpp | 43.7 | typecheck/builtin_sigs.cpp | 30.8 |
| typecheck/typecheck.cpp | 39.5 | typecheck/typecheck.cpp | 27.0 |
| typecheck/func.cpp | 39.3 | typecheck/func.cpp | 27.0 |
| lower/module/import_native.cpp | 38.8 | lower/call.cpp | 26.9 |
| lower/exc.cpp | 37.4 | typecheck/stmt.cpp | 26.8 |
| main.cpp | 37.2 | lower/exc.cpp | 26.6 |
| lower/module/import_stdlib.cpp | 35.9 | typecheck/stmt_forms.cpp | 26.3 |
| typecheck/expr.cpp | 35.8 | main.cpp | 25.5 |

- 链接：dev 2.95s（-g 调试信息），release 0.29s；rt.o/rt_asan.o 合并 <0.1s。
  **链接不是瓶颈**（即使有 lld/mold，收益上限 dev ~3s / release ~0.3s）。
- ASan 运行时（34 个 C 文件）在构建早期并行完成（dev 8s 内），不在关键路径，
  但消耗 ~170 CPU·s（负载下），加剧多租户争抢。
- 89 个 C++ TU 合计 ~1874 CPU·s（dev，负载下），平均 21s/TU。

### 增量重建（dev，touch 单文件后 cmake --build -j32，×3 中位）

| 改动 | 中位 | 说明 |
|---|---|---|
| src/main.cpp | 10.21s | 单 TU + 链接 |
| src/backend/codegen.cpp | 9.24s | 单 TU + 链接 |
| src/syntax/parser.cpp | 6.27s | 单 TU + 链接 |
| src/typecheck/typecheck.h | 44.02s | 触发 74 个 TU 重编 |

## 2. 逐项 A/B（每次只改一项；交替轮转抗噪）

### 2.1 链接器 lld/mold —— 不可用，记录放弃
系统只有 GNU bfd 2.46；lld/mold/gold 不存在，llvm-21 目录里也只有 lldb。
非 root 无法安装。链接占比小（见上），收益上限有限。**未实施。**

### 2.2 意外发现：CMake 4.2 的 C++23 模块扫描（noscan）
原始构建里每个 TU 被 CMake 自动注入 `-fmodules-ts -fmodule-mapper=…`，
并多出一遍 `-E` 模块依赖扫描（89 个 CXX_SCAN 边，406→226 边）。
项目源码**没有任何 C++ module 声明/import**。
- 墙钟：交替 ×3，scan-ON 48.49/51.95/48.33（中位 48.49）vs
  scan-OFF 48.80/46.84/49.29（中位 48.80）→ **墙钟中性**（扫描不在关键路径）。
- 但：少 89 遍预处理（省 CPU、减轻多租户争抢），且 **ccache 见 -fmodules-ts
  一律拒缓存**——关掉扫描是 ccache 生效的前提。
- 实现：`option(PYADAMAS_MODULE_SCAN … ON)` +
  `set_target_properties(pyadamas PROPERTIES CXX_SCAN_FOR_MODULES OFF)`。
- 结论：**保留（作为 ccache 的使能项），单独看墙钟无收益。**

### 2.3 ccache
- scan-ON 时：89/89 调用 **100% uncacheable**（-fmodules-ts），warm 重建
  49.47s vs 基线 47.75s，纯开销 → 该组合放弃。
- scan-OFF 时（交替 ×3，负载 ~27）：
  - cold 48.51s ≈ 基线（无惩罚）；
  - **warm clean 重建 6.40 / 5.44 / 5.14 → 中位 5.44s**，
    对照 noscan 47.05 / 48.23 / 46.54 → 中位 47.05s。**8.6× 提速**。
  - 命中率：warm 轮 89/89 direct hit（100%）。
  - 剩余 5.4s 主要是 68 个运行时 C 编译（add_custom_command 不继承
    CMAKE_<LANG>_COMPILER_LAUNCHER，绕过了 ccache）+ 链接。
- 适用场景：clean 重建/切分支/CI/worktree 初始化；**真实源码改动的增量无收益**
  （改动 TU 必然 miss）。
- 结论：**正收益，保留**（launcher + 运行时 custom command 接线，见 2.4）。

### 2.4 ccache 接线运行时 custom command（ccache+rt）
add_custom_command 不继承 CMAKE_<LANG>_COMPILER_LAUNCHER，把 \${PYADAMAS_RT_LAUNCHER}
（= CMAKE_C_COMPILER_LAUNCHER，未设置时为空）接到 68 个运行时 C 编译前。
交替 ×3（负载 ~25）：noscan 45.36 / 45.31 / 44.82（中位 45.31）vs
**ccache+rt warm 3.50 / 3.48 / 3.48（中位 3.48s）→ 13×**。
剩余 3.5s ≈ dev 链接（~3s，-g）+ ninja 开销，已到 dev 全量的链接下限。
未设置 launcher 时展开为空，行为零变化。**正收益，保留。**

### 2.5 CMake UNITY_BUILD —— ODR 冲突，记录放弃
`-DCMAKE_UNITY_BUILD=ON`（默认 batch=8，noscan）：编译失败。
典型错误（unity_8_cxx.cxx）：
\`opt/dict_bump.cpp\` 与 \`opt/dict_fuse.cpp\` 在同一个
\`pyadamas::optdetail\` 的**匿名命名空间**里定义了同名 helper
（is_rt / same_slot），合并进一个 TU 后匿名命名空间合一 →
redefinition + -Werror=unused-function。全项目 ~30 个 cpp 有匿名命名空间，
同名 helper 跨文件复用是普遍模式；cimport/cdecl.cpp 等还出现跨文件符号遮蔽。
修复需要给几十个 helper 改名/加命名空间隔离，超出「便宜手段」范围。
**放弃（冲突如实记录）。**

### 2.6 PCH 按配置匹配 -O 重试（PYADAMAS_PCH）
上次失败归因是跨 -O 复用被 GCC 静默拒绝。这次用 CMake
target_precompile_headers：每个 build 目录/配置生成独立 PCH，flags 自动匹配。
PCH 内容 = 46 个高频标准库头（src/common/pch_std.h，不含项目头，避免增量频繁失效）。
验证确实生效：编译命令注入 \`-include …/cmake_pch.hxx\`，-Winvalid-pch 无告警，
dev .gch 137.6MB / release .gch 126.5MB，各自独立。

交替 ×3（负载 ~26-30）：

| 配置 | noscan | noscan+PCH | 结论 |
|---|---|---|---|
| dev 全量 | 44.83/45.06/44.96 → 44.96s | 45.36/45.03/45.38 → 45.36s | 中性 |
| release 全量 | 56.84/54.45/56.20 → 56.20s | 57.09/56.16/55.59 → 56.16s | 中性 |

增量（dev，×3 中位）：

| 改动 | 无 PCH | 有 PCH | 收益 |
|---|---|---|---|
| src/main.cpp | 10.21s | 8.83s | -14% |
| src/typecheck/typecheck.h（74 TU） | 44.02s | 36.09s | -18% |

解释：-ftime-report 显示 53% 时间是模板实例化，PCH 只省声明解析，
实例化仍逐 TU 发生，再扣掉每 TU 加载 130MB+ PCH 的成本 → 全量净收益≈0。
与旧文档「只省 0.34s」一致——**-O 匹配修好了，但 PCH 对本项目全量构建依然不划算**。
**保留为 option（默认 OFF）：增量有 14-18% 收益，代价是每配置 ~130MB 磁盘。**

### 2.7 release 优化级别 -O3 → -O2 → -O1
仓库 release preset 实际是 CMake 默认 **-O3 -DNDEBUG**（任务描述里的 -O2 与仓库
现状不符，此处以实测 -O3 为基线）。三配置轮转 ×3（noscan，负载 ~27-29）：

| flags | 3 次 clean 全量 (s) | 中位 | vs -O3 |
|---|---|---|---|
| -O3 -DNDEBUG | 55.92 / 56.76 / 56.71 | 56.71s | — |
| -O2 -DNDEBUG | 51.25 / 51.01 / 50.44 | 51.01s | **-10.1%** |
| -O1 -DNDEBUG | 38.02 / 38.30 / 39.04 | 38.30s | **-32.5%** |

副口径（编译器自身吞吐）见 §3：**O1/O2/O3 无可测差异**，故取构建收益最大的 -O1。
**正收益，release preset 改为 -O1 -DNDEBUG（已落 CMakePresets.json）。**

## 3. 副口径：pyadamas 编译用户程序的吞吐（编译器二进制自身速度）

担心点：release 降 -O 会让编译器二进制变慢，拖慢用户程序编译。三层测量：

**(a) time pyadamas build（含 gcc 子过程），6 工作负载 ×3 交替**
（bench/realistic 4 个 + tests/run/123_finally_yield.py 281 行 +
自构 big.py：hashlib/statistics/csv --rebuild）：

| 工作负载 | O3 | O2 | O1 | dev -O0 |
|---|---|---|---|---|
| queue_sim/str_build/dict_wordfreq/sort_struct/big123 | ~0.07s | ~0.07s | ~0.07s | ~0.07-0.08s |
| bigmods（3 模块 --rebuild） | 0.247s | 0.246s | 0.248s | 0.514s |

**(b) 重负载 ×10 交替**（bigmods + big2.py：6 模块 --rebuild）：

| 工作负载 | O3 中位 | O2 中位 | O1 中位 |
|---|---|---|---|
| bigmods | 0.253s | 0.244s | 0.252s |
| big2（6 模块） | 0.329s | 0.328s | 0.329s |

**(c) 纯前端隔离测量**：pyadamas check 遍历 tests/run 全部 216 个正例
（无 gcc 子过程，纯编译器二进制速度），×3 交替，负载 ~6：

| 二进制 | 3 轮 (s) | 中位 | vs O3 |
|---|---|---|---|
| release -O3 | 2.77 / 2.74 / 2.68 | 2.74s | — |
| release -O2 | 2.82 / 2.82 / 2.79 | 2.82s | +2.9%（噪声级） |
| release -O1 | 2.82 / 2.70 / 2.73 | 2.73s | **-0.4%（中性）** |
| dev -O0 | 3.47 / 3.48 / 3.48 | 3.48s | +27% |

结论：GCC 对本项目代码 -O1 与 -O3 的产物速度几乎相同（指针追逐型代码，
-O3 的向量化/激进内联收益≈0）；-O0 才明显慢。**降 -O 的吞吐代价实测为零。**
另外：生成程序的性能与编译器自身 -O 无关（rt.o 恒 -O2，codegen 直接发射机器码）；
6 个工作负载在 O3/O2/O1/devO0 四种编译器下产出程序**输出全部一致**
（OUTPUT_IDENTICAL），行为零变化。

## 4. 行为零变化验证（bash tests/run_tests.sh + tools/difftest.sh）

| 构建 | 套件 | difftest |
|---|---|---|
| **release 最终配置（-O1，noscan+ccache）** | **234 ran ok，EXIT=0** | **216 用例 0 不一致，EXIT=0** |
| dev 最终配置（-O0，noscan+ccache） | 232 ran ok，EXIT=1（见下） | 216 用例 0 不一致，EXIT=0 |
| dev 原始配置（scan ON、无 ccache、同 -O0 -g） | 232 ran ok，EXIT=1（同样 2 个失败） | — |

**dev 的 2 个失败是基线固有的，与本次优化无关**：118_suspend_expr.py 与
154_nested_await.py 在**原始配置**的 dev 编译器上以完全相同的方式崩溃
（libstdc++ 下标断言 \`stl_vector.h:1263 … __n < this->size() failed\`，
vector<uint8_t>，疑似 codegen/IR 位图越界的潜在 UB，-O0 布局下被
Ubuntu GCC 15.2 的 libstdc++ 硬化断言抓出；release -O1 全过）。
优化前后 dev 失败集合完全一致 = 行为零变化。另：首轮 difftest 的
27_runtime 不一致为负载性偶发（两配置单独重试均 0 不一致）。

测量事故记录：/tmp 是 12G tmpfs，实验目录堆到 86% 时一次套件运行被
磁盘写满污染（echo: write error）；清理实验目录后重跑，结果以重跑为准。

补充：**仓库真实 dev preset 是 -g -Og**（不是裸 -O0）。用最终选项
（noscan+ccache）+ -Og 构建：**套件 234 ran ok EXIT=0，difftest 216/0 EXIT=0**，
两个 -O0 崩溃用例在 -Og 下正常。即优化后的仓库默认 dev/release 两条预设
都满足「234 + 216/0」；裸 -O0 的 2 个失败为基线固有（原始配置同样失败）。

## 5. 头牌对比：原始配置 vs 最终配置（交替轮转，同负载窗口）

最终配置 = noscan（默认）+ ccache 自动探测（含运行时接线）+ release -O1。
cold 轮每次构建前 ccache -C 清空，保证公平。

负载 ~27-30 平稳窗口，orig 与 final 逐轮交替；dev warm 取同负载带的前一轮 ×3。

| 口径 | 原始配置（scan ON、无 ccache、release -O3） | 最终配置（noscan+ccache、release -O1） | 变化 |
|---|---|---|---|
| dev 全量 **cold** | 49.61 / 46.62 / 48.31 → **48.31s** | 49.01 / 48.62 / 48.22 → **48.62s** | 中性（首次构建无惩罚） |
| dev 全量 **warm** | 无缓存机制，每次 ~48s | 3.44 / 3.47 / 3.47 → **3.47s** | **13.9×** |
| release 全量 **cold** | 58.22 / 58.45 / 58.50 → **58.45s** | 41.10 / 41.29 / 40.94 → **41.10s** | **-29.7%** |
| release 全量 **warm** | 无缓存机制，每次 ~58s | 0.45 / 0.44 / 0.45 → **0.45s** | **130×** |
| dev 增量（真实内容改动） | main.cpp 10.21s / typecheck/typecheck.h 44.02s | 同量级（miss 必然重编） | 不变 |
| configure | 1.3-1.5s | 1.3-1.5s | 不变 |

warm 的下限由链接决定：dev -g 链接 ~3s，release 链接 ~0.3s。

增量（dev 最终配置，touch 单文件）：
- src/main.cpp：首次 10.39s（真实重编+链接）；随后 3.06/3.07s ——
  **touch 不改内容时 ccache 直接命中**，只剩链接。真实内容改动仍会 miss（~10s）。
- src/typecheck/typecheck.h：首次 48.92s（74 TU 重编）；随后 3.70/3.52s（同上，ccache 命中）。
- 对照基线增量：main.cpp 10.21s、typecheck/typecheck.h 44.02s（无 ccache）。

## 6. 推荐配置（已落 speed 分支）

| 手段 | 默认 | 收益（实测） | 代价/风险 |
|---|---|---|---|
| 关 C++23 模块扫描（PYADAMAS_MODULE_SCAN=OFF） | 开（=关扫描） | 墙钟中性；省 89 遍 -E 扫描与 -fmodules-ts 注入；**ccache 的前提** | 项目无 C++ modules，零行为变化；将来要用 import std 时打开 |
| ccache 自动探测（PYADAMAS_USE_CCACHE=ON） | 开 | **warm clean 重建 dev 13×（45→3.5s）**；cold 无惩罚 | 需要 ccache（没有则自动跳过）；真实改动的增量无收益；CI 建议持久化 CCACHE_DIR |
| 运行时 custom command 接 launcher | 随 ccache | warm 重建 5.4→3.5s | 未设 launcher 时展开为空，零变化 |
| release preset -O3→**-O1 -DNDEBUG** | -O1 | **release 全量 -32.5%（56.7→38.3s）** | 三层吞吐测量均中性（§3）；行为验证 234+216/0 通过；保守可选 -O2（-10.1%） |
| PCH（PYADAMAS_PCH） | **关** | 全量中性；增量 -14%（单 TU）/-18%（typecheck/typecheck.h） | 每配置 ~130MB .gch；按需开 |

日常开发建议：dev 用仓库 preset（-Og，已验证 234+216/0）；clean 重建/切分支
自动享受 ccache；跑分用 release（-O1，编译器吞吐与 -O3 无实测差异）。

## 7. 未做/放弃的方向（记录）

1. **lld/mold**：系统仅 GNU bfd 2.46，无 lld/mold/gold，非 root 无法安装。
   链接占比小（dev ~3s / release ~0.3s），收益上限有限。未实施。
2. **UNITY_BUILD**：匿名命名空间同名 helper 跨文件冲突（§2.5），放弃。
3. **C++23 std modules（只记录，不做）**：工具链可行性已实测——
   GCC 15 `g++ -std=c++23 -fmodules-ts -c /usr/include/c++/15/bits/std.cc`
   构建 std BMI 15.7s / 30MB；`import std;` 测试程序编译+链接 4.1s，运行正常。
   这是唯一能根治「53% 时间逐 TU 重复实例化标准库模板」的方向，潜在收益最大，
   但集成很深：CMake 4.2 的 CXX_MODULE 实验 API + Ninja dyndep、每配置独立 BMI、
   与 -Werror/现有 46 个 std include 的混用迁移、clangd/IDE 支持、Windows 移植
   （MSVC import std 路径不同）。本轮不做。
4. **重模板头重构（只分析，不做）**：typecheck/types.h 经 typecheck/typecheck.h 被 74/89 TU
   包含；<format>(85 TU)/<ranges>(74)/<expected>(61) 是最重的模板头，
   项目头顺手 include 是扩散源。拆分/前向声明/pimpl 收益可观但改动面大。
5. **rt_asan 可选化**：34 个 ASan 运行时 C 文件默认 ALL 构建，耗 ~170 CPU·s
   （负载下），但 8s 内并行完成、不在关键路径；可选化会改变 --asan 行为，未做。
6. **-j 扫描**：按任务口径固定 -j32，未扫其他并行度。
7. **dev -O0 的 2 个固有崩溃**（118_suspend_expr/154_nested_await）：
   基线即有（§4），属编译器潜在 UB，建议另开 bug 跟踪，不在本轮修。

## 8. 集成建议（给 lead）

- speed 分支改动是**加性 option + preset flags**，与 vthread 集成冲突面小
  （CMakeLists 的 add_executable 附近与 CMakePresets release 段）；cherry-pick 后
  重跑 run_tests.sh + difftest 即可。
- release -O1 是策略性变更：若团队保守，改回 -O2 是一行 preset 的事（-10.1%）。
- CI：装 ccache 并跨 job 持久化 CCACHE_DIR，clean 构建可到 ~3.5s（dev）。
- 测量环境注意：本机 /tmp 是 12G tmpfs 且有空闲目录回收（本次测量中
  build-release 等闲置目录被清掉过一次）；构建目录勿长时间闲置，或放磁盘分区。
- 多租户负载使绝对墙钟波动 ~1.8×；本报告所有结论基于交替轮转的相对差。

