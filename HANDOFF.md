# 交接文档 —— 0.290（2026-11-18 晚）

> 时点快照：本轮结束时的可运行状态、怎么跑、下一步做什么、以及**本轮
> 攒下的教训清单**（§6，比数字更值钱）。不属于架构文档，下次交接直接覆盖。
> 写本文档已获用户明确授权（平时不要主动写/改 HANDOFF.md）。

## 0. 路径与会话形态

仓库在服务器 `/src/luo-proj/pyadamas`。**本会话直接以 celestea（uid 1003）
在服务器上运行，cwd 就是仓库根**——构建/测试/基准全部本地执行，不需要
ssh 包一层；只有 **git push 要走 root@localhost**（GitHub 密钥在 /root/.ssh）：

```bash
SSH="ssh -i /home/celestea/.ssh/id_ed25519_root_mc -o BatchMode=yes \
  -o UserKnownHostsFile=/home/celestea/.ssh/known_hosts -o ConnectTimeout=8 root@127.0.0.1"
$SSH 'cd /src/luo-proj/pyadamas && git push origin master'
```

git 命令带 `-c safe.directory=/src/luo-proj/pyadamas`（celestea 全局配置已加，
worktree 里偶尔还要）。harness 的 HOME 是 /srv/dsh-dev，celestea 真实 home
是 /home/celestea（密钥在那里的 .ssh）。/etc/ssh 是 700，ssh 必须带
UserKnownHostsFile。服务器多租户（Minecraft 等），负载波动大——测量纪律见 §2。

## 1. 环境与构建目录

| 项 | 值 |
|---|---|
| 版本 | **0.290**，HEAD `c5826d7`，与 origin/master 同步 |
| 服务器 | Debian 13，Xeon Gold 6252 @ 2.10GHz，容器 cpuset 24-47,72-95（48 核）|
| 工具链 | gcc 15.2、cmake 3.31.6、ninja、python 3.13.5、go（/usr/local/bin/go）、g++ |
| **build/lead** | **工作构建（0.290）**：compiler + rt.o + rt_asan.o。所有构建/测试/基准用它 |
| build/release | **0.271 oracle**——语料零差异对比的基线，**不要重建到新版本** |
| build/dev | 0.271（difftest 默认用它，传参可换）|
| build/asan、build/nocet | 历史实验目录，不用管 |

构建：`cmake --build build/lead -j32`（-j32 比 -j96 快，modules-ts 串行）。
新建构建目录：`cmake -S . -B build/X -G Ninja -DCMAKE_BUILD_TYPE=Release`。

**工作树里有 docs agent 的并发改动**（README.md + docs/*.md 约 20 个文件，
用户派了另一个模型优化文档表述）。**永远不要 stage/覆盖/回退它们**；
`git add` 只加自己的文件；`git status` 里看到它们不要惊讶。

## 2. 常用命令与测量纪律

```bash
# 差分（O0 vs O2，193 用例）——注意 §6.5 的假绿陷阱
bash tools/difftest.sh build/lead/pyadamas

# 全量套件（211 ran ok × 2，退出码 0 才算过）
bash tests/run_tests.sh build/lead build/lead/pyadamas
PYADAMAS_TEST_ASAN=1 bash tests/run_tests.sh build/lead build/lead/pyadamas

# 语料零差异：当前 vs 0.271 oracle（脚本在 /tmp，可能被清，内容见下）
bash /tmp/pa-corpus-diff.sh        # 末行 CORPUS_DIFF_DONE 且无 DIFF 行

# 单用例对拍 CPython（新回归用例必须先过这关再钉 expect 段）
python3 tests/run/NNN.py > /tmp/ref.txt
build/lead/pyadamas run --rebuild -o /tmp/x tests/run/NNN.py > /tmp/got.txt
diff /tmp/ref.txt /tmp/got.txt

# A/B（新工具：打乱顺序 + 奇偶交换 + BURST 连跑平均，见 §6.1）
GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory \
GIT_CONFIG_VALUE_0=/src/luo-proj/pyadamas \
PYADAMAS_BIN=$PWD/build/lead/pyadamas bash tools/abbench.sh <旧ref> realistic 15
# 套件名：realistic（6 用例）/ classic（9 用例）/ all；BURST=N 可调连跑数

# 单版本快速计时（最好 N 次，不绑核，只看量级）
REP=7 bash tools/quickbench.sh queue_sim str_build

# 看 IR / 汇编
build/lead/pyadamas ir  bench/realistic/obj_dispatch.py
build/lead/pyadamas asm bench/realistic/obj_dispatch.py

# 四语言全量对比（9 案 × CPython/PyAdamas/Go/C++，约几分钟）
PYADAMAS_BIN=$PWD/build/lead/pyadamas python3 bench/compare/run_compare.py --repeat 5 --md /tmp/compare-results.md
```

**每轮验证清单（缺一不可）**：difftest 193/0 → 语料零差异 → 套件 211×2
（normal + ASan）→ ASan detect_leaks=1 抽查（13_gc / 88_defaultdict /
192_dict_cache / 66_genexpr / 119_gen_except_suspend）→ abbench 15 轮 →
`bash tests/check_size.sh` 无新增超限 → CHANGELOG + 选择性 git add + push。

**测量纪律（本轮血的教训，详见 §6）**：
- 只信交替 A/B + 打乱 + BURST 的 abbench；短用例（<100ms）历史数据需复核。
- 数字先对数量级；量级不对先怀疑测量失败。
- **perf 占比 ≠ 边际成本**：省掉 11.6% 的指令可能 cycles ±0（OoO 隐藏）。
  每个优化必须 abbench 验证，profile 只用来找候选。
- 性能回归先看 objdump（gcc 内联决策会被无关改动扰动）再看算法。
- 布局敏感：上游代码尺寸变化可通过 DSB（uop 缓存）产生 ±10% 远端效应；
  诊断用 `perf stat -e idq.dsb_uops,idq.mite_uops`。

**语料 oracle 脚本**（/tmp/pa-corpus-diff.sh 被清后照抄重建）：

```bash
#!/bin/bash
set -u
cd /src/luo-proj/pyadamas
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
run_one() {
  f=$1; name=$(basename "$f" .py)
  case "$name" in f*|*_perf) return;; esac
  d="$WORK/$name"; mkdir -p "$d"
  o1=$(timeout -s KILL 30 build/release/pyadamas build -O2 -o "$d/a" "$f" >/dev/null 2>&1 && timeout -s KILL 30 "$d/a" 2>&1); r1=$?
  o2=$(timeout -s KILL 30 build/lead/pyadamas build -O2 -o "$d/b" "$f" >/dev/null 2>&1 && timeout -s KILL 30 "$d/b" 2>&1); r2=$?
  if [ "$o1" != "$o2" ] || [ "$r1" != "$r2" ]; then printf 'DIFF %s (rc %s vs %s)\n' "$name" "$r1" "$r2"; fi
}
export -f run_one; export WORK
ls tests/run/*.py | xargs -P 32 -n 1 bash -c 'run_one "$0"'
echo CORPUS_DIFF_DONE
```

## 3. 当前状态与基准

| 项 | 值 |
|---|---|
| 版本 | 0.290（下一号 **0.291**；git log 与 CHANGELOG 顶格必须一致）|
| 差分 | 193 / 0 |
| 套件 | 211 ran ok × 2（normal + ASan），exit 0 |
| 规模检查 | 仍失败（5 个已知超限，见 §7.1；**无新增**）|

### realistic 六用例（abbench 15 轮中位数，0.290 实测）

| bench | 0.277 前后 | 0.290 | 累计 |
|---|---|---|---|
| queue_sim | 203 ms | **155.8 ms** | −23%（对 Go 40ms ≈ 3.9×）|
| str_build | 210 ms | **148.7 ms** | −29% |
| rec_memo | 439 ms | **264.3 ms** | −40% |
| obj_dispatch | ~550 ms | **439.2 ms** | −20% |
| dict_wordfreq | ~290 ms | **263.7 ms** | −9% |
| sort_struct | ~200 ms | **194.8 ms** | −3% |
| 合计 | ~1830 ms | **~1466 ms** | −20% |

### 四语言对比（0.290，repeat 5 中位数；完整报告 /tmp/compare-results.md）

| 用例 | CPython | PyAdamas | Go | C++ | PA/Go |
|---|---|---|---|---|---|
| binary_trees | 18.2 | **2.1** | 3.5 | 3.7 | **0.60×** |
| fannkuch | 56.6 | 10.9 | 6.5 | 3.9 | 1.68× |
| fasta | 45.5 | **2.6** | 5.4 | 4.2 | **0.48×** |
| fib | 402.5 | **19.1** | 28.0 | 15.2 | **0.68×** |
| mandelbrot | 467.5 | 29.2 | 17.2 | 27.9 | 1.70× |
| matmul | 206.2 | 14.1 | 4.6 | 6.3 | 3.07× |
| nbody | 583.8 | 46.8 | 19.0 | 15.7 | 2.46× |
| nqueens | 702.6 | 99.2 | 62.9 | 62.4 | 1.58× |
| spectral_norm | 1483.5 | 53.6 | 30.0 | 13.6 | 1.79× |

binary_trees/fasta/fib 快过 Go；binary_trees/fasta 快过 C++；mandelbrot ≈ C++。
短板集中在浮点密集（matmul/nbody/spectral）。

## 4. 本轮提交链（0.278 → 0.290）

| 提交 | 内容 | 效果 |
|---|---|---|
| efc0e66 | 0.278 修 0.277 嵌套同名非负提示阴影洞；余数旋转负收益撤回 | 正确性修复 |
| ceff6ed | 0.279 浮点临时寄存器与 GPR 门控解耦 + **abbench 抗干扰修复** | spectral −19.9% |
| c72c8cf | 0.280 kind-0 元组分配内联 + 修 4 个 bug（cmpq 宽度/payload 偏移/batch rc 归零/gc 标志滞留）| queue_sim −16.5% |
| 1deafe9 | 0.281 dict 缓冲池化 + 去双重 mix64 | rec_memo −32.4% |
| ff19c31 | 0.282 append 内联 + **循环头 32B 对齐（DSB）** | str_build −6.6% |
| df7e4dc | 0.283 dict 末次查找缓存**负收益撤回**（STLF 串行化）+ 回归 192 | 教训 |
| 670cc55 | 0.284 contains+get IR 层融合（DictTryGet + opt_dict_fuse）| rec_memo −11.8% |
| b64cb23 | 0.285 dict set 单探测 + 标量销毁快路径 + always_inline 修内联扰动 | wordfreq −1.7% |
| e8af460 | 0.286 临时栈 push/take 内联（永生形态 5 条指令）| str_build −5.3% |
| f197242 | 0.287 str 下标 ASCII 快路径内联（gen_str_index_inline）| str_build −6.2% |
| 12aa107 | 0.288 str 相等单字节内容快路径 | str_build −6.5% |
| 23e94a4 | 0.289 Bin+StoreGlobal 融合（can_fuse_global）| obj_dispatch −5.7% |
| c5826d7 | 0.290 store.global→load.global 转发（forward_global_loads）| obj_dispatch −13.7% |

关键机制位置：内联发射集中在 backend/call_rt.cpp（gen_rc_inline /
gen_tuple_inline / gen_append_inline / gen_str_index_inline）；
gen_call_rt（backend/call.cpp）有 MathSqrt / DictTryGet 特判；
opt/dict_fuse.cpp（DictTryGet 融合）、opt/forward.cpp（槽转发 + 全局转发）。

## 5. 下一步（按杠杆排序，均已完成取证）

### 5.1 obj_dispatch 剩余头部（84% 生成代码，439ms）

当前内层循环（bb15）每轮：index.get shapes[i] → **rc_inc(%97) +
store.global g8 + rc_dec(旧 s)**（~16 指令 + 2 次依赖加载）→ load.global
g6（total）→ vcall（mov (%r10),%r11; mov (%r11),%r11; call *%r11）→
add+store.global 已融合。0.290 已转发掉 load g8。

- **借用语义迭代**（首选）：shapes 列表拥有元素，循环变量 s 只是借用——
  若可证迭代期间 shapes 不被修改（callee 分析：area() 只读 self 字段），
  循环变量不持有引用，省掉每轮 inc/dec。需要：过程间「不修改该列表」
  分析 + 降级器为 for-in 生成借用形态 + 异常路径语义（迭代中列表被
  修改的 Python 语义对照）。预计 −8~12%。
- **投机去虚化 / PIC**：vcall 前插类守卫（cmp vtable, .Lvt_Rect; jne
  fallback; call Rect_area 直接），Shape/Rect/Square 三类按频率 1-2 路。
  省第二级 vtable 加载 + 间接→直接。守卫失败回退原 vcall。预计 −5~10%。
  注意 DSB 布局效应（§6.2），代码膨胀可能反噬。

### 5.2 queue_sim（156ms，对 Go 3.9×）

profile：simulate 62%、rc_alloc_core 6.9%、dec_slow 6.9%、batch 5.1%、
copy_range 3.3%、list_destroy 3%、rc_link 2.8%、rc_unlink 1.9%。
- **kind-1 小列表分配内联**（rest=[] 每步一次）：仿元组内联，但要内联
  rc_link（heap 链 + rc_index 哈希插入 + young_add）——复杂度高，
  rc_index 插入是哈希表操作，先取证值不值得。
- copy_range 的逐元素 pa_rc_inc 调用（kind1+heap1 路径）可内联快 inc。

### 5.3 浮点短板（matmul 3.07× Go、nbody 2.46× Go、spectral 3.94× C++）

0.279 已解耦浮点临时寄存器门控。剩余差距大概率在：Go/C++ 的 SIMD
向量化、更好的 xmm 分配（我们浮点槽提升只有 xmm8-15 八个、临时 xmm3-7
五个）。先 perf annotate matmul/nbody 热循环取证再定方向；**不要盲目
上 SIMD**（发射器大工程，先确认标量路径无浪费）。

### 5.4 不要重试的方向（已实测负收益/已决策）

- **运行时 dict 末次查找缓存**（0.283）：缓存字段形成循环携带 store→load
  依赖，STLF 串行化，rec_memo +8.8%。融合要走 IR/寄存器层（0.284 已做）。
- **增量余数旋转**（0.278）：i%常量 → rem=rem+1 旋转。循环携带依赖 vs
  魔数链各轮独立可流水；queue_sim +4.2%。mod 延迟不在关键路径。
- **batch dec 永生范围检查**（0.290 轮实验，未提交）：地址范围判永生
  免解引用——str_build cycles +1%（OoO 隐藏 L1 加载）、queue_sim +3.3%。
- **槽 SSA 化**：vreg 层已全 SSA；槽不是瓶颈（池 6→3 仅 ~3%）；堆槽必须
  可寻址（GC 根 + 异常展开）。见 docs/ir.md、regalloc-architecture.md §7/§8。
- **PYADAMAS_RA_SPLIT=1**（+28%）、图着色默认开（matmul 曾 −8.6% 但
  整体不稳）——实验开关，默认关。

## 6. 教训清单（本轮攒下的，比数字值钱）

1. **abbench 短用例相位锁定**：固定用例顺序 + A 恒先于 B，短用例锁进
   机器周期性干扰的固定相位，**同一份二进制量出 ±20% 假差**（对照实验：
   两侧同一编译器）。已修：每轮 sort -R 打乱 + 奇偶轮交换 A/B + BURST=5
   连跑平均。修复后对照 ±3.5% 内。**怀疑数字时先跑对照实验。**
2. **DSB（uop 缓存）布局敏感性**：上游代码尺寸变化把热循环 uop 挤出
   DSB 窗口 → MITE 传统解码 → cycles +9%（指令/分支/icache 全不变）。
   循环头对齐已从 16B 改 **32B**（.p2align 5，DSB 窗口粒度）；64B 无进一步
   收益（组冲突不由入口对齐单独解决）。诊断：idq.dsb_uops / idq.mite_uops。
3. **perf 占比 ≠ 边际成本**（四次验证）：dict_destroy 链游占 11.6%，
   跳过后指令 −5.3% 但 cycles ±0——L1 依赖加载全在其它延迟阴影里。
   0.277 指令 −5% 只换 −0.4%；0.278 余数旋转指令 −0.2% 但 cycles +6.1%。
   **profile 找候选，abbench 定生死。**
4. **gcc 内联扰动**：同一翻译单元加函数可能改变既有热函数的内联决策
   （dict_key_matches 被 outline，wordfreq 每次探测多一层 call，指令
   +6.3%）。修复：always_inline。**性能回归先 objdump 对比再怀疑算法。**
5. **difftest 假绿**：O0/O2 同样编译失败时 difftest 报「一致」（0.288
   movzbq 写错寄存器，193/0 全绿但套件 211→189）。**difftest 全绿还要
   看套件计数。**
6. **汇编坑**：movzbq 目的必须 64 位（%al 报 size mismatch，%eax 报 q
   后缀不允许）；**cmpq 读 8 字节，比较 4 字节 int 全局要用 cmpl**
   （否则把相邻 .bss 变量读进来永真，0.280 快路径从不命中）；内联分配
   返回 **payload（h+64）不是 RC 头**（0.280 池链被 interior 指针污染）。
7. **池块不变量**：同池 dtor/release/kind/size 恒定（0.276）；入池 rc
   必须归零（0.280 修 batch 漏归零）；内联缓冲是块内指针不能单独
   free/realloc（0.271）；dict 缓冲池化后指针寄存池节点（payload 会被
   memset，0.281）；**运行时直写的槽不能参与寄存器提升**（无 StoreSlot
   定义 → 块入口重载逻辑误判，0.284 值槽读陈旧寄存器）。
8. **调试工具链**：gdb 可用（无调试符号，用寄存器/地址 + cast）；
   watchpoint 抓内存破坏（0.280 池链污染靠它定位）；perf stat 计数器
   （cycles/instructions/branches/branch-misses/idq.dsb_uops/idq.mite_uops）
   区分算法收益 vs 布局效应；ASan 抓越界/UAF。**run_code 默认 120s
   超时，长任务用 run_in_background + job_output(wait)。**
9. **TS 模板literal 陷阱**（agent 侧）：run_code 里写 C++ 代码字符串，
   模板literal 遇 \` 或 \${} 会解析炸——用双引号 JS 字符串 + \n 拼接。

## 7. 已知问题（不要当新发现重复排查）

1. **check_size.sh 失败**（5 个已知超限，清单只能删不能加）：
   backend/regalloc.cpp(2475)、driver/build.cpp(1421)、ir/ops.h(1249)、
   lower/impl.h(1233)、typecheck/types.h(1296)。新代码放新文件或
   backend/call_rt.cpp（1010）/opt_dict_fuse.cpp 等有余量的文件。
2. **RC 统计盲区（部分）**：gen_rc_inline 的 inc/dec 快路径不读
   pa_rc_stats_on_fast（统计会漏）；0.286 的 temp push/take 内联**有**
   stats 守卫。要精确统计临时关 inc/dec 内联。
3. **docs/ 与 README 有 docs agent 的并发改动**（工作树 ~20 个 M 文件）：
   不要 stage/覆盖。docs/codegen.md 的历史漂移（32 字节头遗留偏移图等）
   由 docs agent 负责，别顺手修。
4. **旧 worktree**：/tmp/base_wt、/tmp/wt-268、/tmp/wt-head* 等是历史
   会话遗留（部分 prunable），不是本轮的，别动；本轮自建的已全部清理。
5. **/tmp 产物**：pa-corpus-diff.sh（oracle，§2 有备份）、compare-results.md
   （最新四语言对比）、各基准二进制（qs-*、sb-*、od-* 等，可重建）。
   root 跑文件类用例会在 /tmp 留 root 文件，celestea 再跑会权限错——
   套件放 bwrap 跑：`bwrap --bind / / --tmpfs /tmp --dev /dev --proc /proc
   --die-with-parent -- bash tests/run_tests.sh build/lead build/lead/pyadamas`。
6. **移动仓库目录后必须重建构建目录**（CMakeCache 写死绝对路径）。
7. **Win64**：内联 RC/临时栈发射沿用 %rdi 硬编码先例（gen_rc_inline），
   与常规调用的 arg_reg_i 不一致——Win64 验证（verify_win.sh）本轮没跑，
   动内联发射时留意；scratch 用 rax/r10/r11（两 ABI 均 volatile）。

## 8. 版本号与提交约定

每次提交前：`git log --format='%s' -300 | grep -oE '0\.[0-9]{3}' | sort -u -V | tail -3`
与 `grep -oE '^## \[[0-9.]+\]' CHANGELOG.md | head -1` 必须一致。
现在都是 **0.290**，下一号 **0.291**。CHANGELOG 用 python 脚本插入
（marker=上一版标题，assert count==1）；提交信息写 /tmp 文件 -F 传入；
**选择性 git add**（排除 README/docs）；push 走 §0 的 root ssh。
负收益也写 CHANGELOG（0.278/0.283 先例）——撤回机制代码、保留回归测试、
记录诊断，这是仓库文化。

## 9. 工具与环境变量速查

| 工具 | 用途 |
|---|---|
| tools/difftest.sh | O0/O2 差分，并行；PYADAMAS_TEST_FILTER/JOBS；默认 build/dev，传参换 |
| tools/quickbench.sh | 单版本最好 N 次；REP=/PYADAMAS_BIN=/OUT= |
| tools/abbench.sh | A/B worktree + 交替 + **打乱 + BURST**；参数 <ref> <suite> <reps>；BURST= 可调 |
| bench/compare/run_compare.py | 四语言对比；PYADAMAS_BIN/GO_BIN/CXX；--repeat/--md |
| tests/check_size.sh | 文件规模（1200 行硬限）|
| tools/irdump.cpp | IrOp/RtFn 枚举编号对照 |

| 变量 | 作用 |
|---|---|
| PYADAMAS_RA_POOLCAP / RA_SPLIT / RA_GRAPH | 分配器实验开关（默认满池 6 / 关 / 关）|
| PYADAMAS_LI_STATS | LiveInterval 峰值压力诊断 |
| PYADAMAS_ALIGN_CHECK=1 | 入口 rsp 对齐自检 |
| PYADAMAS_RC_STATS=1 / GC_STATS=1 / GC_TIMING | RC/GC 统计（注意 §7.2 盲区）|
| PYADAMAS_RC_DIAG / RC_TRACE / EXC_TRACE | 诊断追踪（rt_diag.c）|
| PYADAMAS_TEST_ASAN=1 / TEST_JOBS / TEST_FILTER | 套件开关 |
| BURST | abbench 连跑平均数（默认 5）|

## 10. 仓库结构速查

```
src/           C++23 前端：词法 → AST → 脱糖 → 类型检查 → IR → codegen
               优化 pass：opt/opt.cpp 调度（fold/inline/licm/forward/dict_bump/
               dict_fuse/global_promote/bounds_version/loop_rotate/cse/rc）
runtime/       C11 运行时（38 个 .c，ld -r 合成 rt.o）；池：str/list/
               list_inline/dict（含缓冲）；temp 栈；增量 GC
stdlib/        纯 Python 标准库
bench/         builtins/ realistic/（6 用例）compare/（9 案 × 4 语言）
tests/         run/（193 差分用例，含 190 非负除法/191 取模语义/192 字典缓存）
docs/          架构文档（docs agent 维护中，勿动）
tools/         差分/基准/同步/IR dump
```

## 11. 接手第一步

```bash
cmake --build build/lead -j32
bash tools/difftest.sh build/lead/pyadamas          # 期望 193/0
REP=5 bash tools/quickbench.sh queue_sim str_build  # 期望 ~156 / ~149 ms
```

确认基线后按 §5 开工：obj_dispatch 借用语义迭代（先取证 RC 流量占比 +
callee 修改分析可行性），或浮点短板取证。**每个优化走 §2 验证清单，
记住 §6：profile 找候选，abbench 定生死，回归先 objdump。**
