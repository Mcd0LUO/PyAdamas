# 接续 Handoff —— 0.462（HEAD 668449f）

> 2026-11-23 23:10 快照。仓库 `/src/luo-proj/pyadamas`，master = origin/master = `668449f`。
> 工作区另有**未提交**的 0.463–0.466（见 §4）：sorted 类实例键修复、Path 迁移、
> LSP formatting/workspace symbol、fannkuch kind0 写路径。
> 本文档覆盖状态、闸门、已交付、剩余任务与纪律；下次交接直接覆盖。

## 0. 会话形态与推送

- 直接在服务器仓库根运行；日常用户是 **celestea**（uid 1003），构建/测试/基准都在本地跑。
- **不要用 root 跑构建/测试**：root 会在 `build/lead/modcache`、`/tmp` 留下 root 属主文件，
  celestea 之后会撞 `fs.protected_regular`（现象：`Permission denied` / `cannot write mod.s`），
  看起来像测试失败。确需 root 操作后，把 `build/lead`、`build/gfix` chown 回 celestea，
  并清掉测试临时文件。
- push 走 root（GitHub 密钥在 /root/.ssh）：

  ```bash
  ssh -i /home/celestea/.ssh/id_ed25519_root_mc -o BatchMode=yes \
    -o UserKnownHostsFile=/home/celestea/.ssh/known_hosts -o ConnectTimeout=8 root@127.0.0.1 \
    'cd /src/luo-proj/pyadamas && git push origin master'
  ```

- git 需 `-c safe.directory=/src/luo-proj/pyadamas`（celestea 全局已配）。

## 1. 环境与构建目录

| 项 | 值 |
|---|---|
| 版本 | 0.462（HEAD `668449f`）；工作区未提交 0.463 |
| 服务器 | Debian 13，Xeon Gold 6252，容器 cpuset 48 核 |
| 工具链 | gcc 15.2、cmake 3.31、ninja、python 3.13、go |
| build/lead | 工作构建（编译器 + rt.o/rt_asan.o/rt_debug.o） |
| build/release | 0.271 语料 oracle，**绝不重建** |
| build/gfix | 套件/difftest 构建目录（checkall 默认） |
| build/asan、build/gXXX | 历史实验目录，忽略 |

构建：`cmake --build build/lead -j32`。

## 2. 闸门与基线

```bash
bash tools/checkall.sh --full     # 约 3 分钟；必须整套跑
```

| 门 | 668449f 基线 | 加 0.463 用例后 |
|---|---|---|
| suite | 461 ran ok / 0 失败 | 462 ran ok / 0 失败 |
| difftest | 433 / 0 不一致 | 434 / 0 |
| bench 12×3 | 36/36 逐字节 | 36/36 |
| 12 基准合计 | −8.54%（见 tools/perf_baseline.txt） | 同 |
| LSP 冒烟 | 201 checks | 228 checks |

- 性能口径：`instructions:u` 确定性；perf_baseline 阈值 +2% 警告 / +5% 失败。
- ASan **不在默认闸门**；仅动 runtime/RC/GC/lowering 所有权时用过滤器定点跑，见
  `docs/semantics-audit-playbook.md §13`。
- difftest 必须独占（`tools/checkall.sh` 内置 `flock /tmp/pyadamas-difftest.lock`）；
  lock 文件属主是 celestea，别 chown 成 root。

## 3. 已交付（0.447 → 0.462）

- **声明式签名**（.pysig）：time、os.path、math（57 函数）、sys、os、random 模块级、
  random.Random、re.Pattern/Match（15 方法）、datetime 三类、collections.deque（10 方法）。
  `src/native_sigs.h` + `stdlib/sigs/*.pysig`；新增原生 API 不再改 C++。
- **声明语法统一**：`.pysig`/`.cpya` 都改成 Python 语法，共用 `src/pydecl.h`；
  `.cpya` 支持 union/定长数组/`@packed`。
- **语言**：enum/abc 编译期语义（0.456）；`.pysig` 类声明、`@ret_specialized`。
- **标准库**：第 3 批纯 py（itertools/functools/logging/codecs）；22 个模块 CPython 对拍
  验收（修 copy.copy/deepcopy、calendar 常量、decimal.to_integral_value 等）。
- **LSP**（独立仓库 pya-lsp，纯 .pya）：completion / definition / semanticTokens /
  references / rename / signatureHelp / documentHighlight / foldingRange / 诊断 debounce；
  冒烟 119 → 201。
- **性能三条线**：rc_dec_unknown 特化（fannkuch −4.56%）、整数 Bin 留 %rax 融合
  （collatz −20.5%、matmul −1.1%）、循环版本化+仿射 BCE（nqueens −11.35%）；
  12 基准合计 −8.54%。
- **P2（未提交）**：Path 声明化（0.464）、LSP formatting/workspace/symbol（0.465）、
  fannkuch kind0 写路径（0.466，再 −5.61%）。

## 4. 未提交改动（0.463–0.466）

- **0.463 sorted 类实例键**：`runtime/rt_list.c` 的 `pa_list_sorted_by` 复用 kind-3
  族分类/`__lt__` 分派（抽 `uk_sort_family`）；unknown float/big 键按数值比；相等键按
  原下标稳定。`tests/run/559_sorted_key_class.py`。
- **0.464 pathlib.Path 迁移**：`stdlib/sigs/pathlib.pysig`（构造器 + 18 方法）、
  `declared_class_type`、method_call 声明查表；`/` 运算符、构造器降级、`mkdir` 关键字
  白名单、参数装配保留 C++；`unlink`/`rmdir` 无 pa_path_* 入口保留 method_call。
  C++ −22 行 / pysig +49 行；`tests/run/560_pathlib_pysig.py`。
- **0.465 LSP formatting + workspace/symbol**：formatting 只归一缩进（4 空格）与行尾空白，
  括号续行/三引号内部原样，幂等；workspace/symbol 扫 rootUri 下 `.py`/`.pyi`（≤2000），
  复用 scan_symbols。冒烟 **201 → 228**。
- **0.466 kind0 列表写路径**：`src/codegen_index.cpp` 的 hk==0 热路径 elem_kind 检查
  合并为一次比较；fannkuch **33,292,318 → 31,424,592（−5.61%，累计约 −10%）**；
  nqueens +0.28%（布局噪声）；`tools/perf_baseline.txt` 已更新 fannkuch。
  **LoadSlot 块内 CSE 负收益已回退**（fannkuch +6.4%）。
- 全闸门已过：**463 ran ok / difftest 435-0 / bench 36/36 / LSP 228 / perf warn=0 fail=0**。
- `CHANGELOG.md` 0.463–0.466 + `docs/language.md`/`compatibility.md`/`native-stdlib-cookbook.md`
  已同步。

## 5. 分支与 worktree

- 已清理：base-check、lsp-a/lsp-b、perf/a-d（7 个，均已合并）。
- 保留：`/src/luo-proj/.wt/scan2-{cg,fe,rt}`（同一 commit `cdcbf3c`，scan2 后续用）。
- 其余旧分支：backup_before_w5w9/w6/w7、fix/win_*（未合并，历史留档）、rc1、regalloc。

## 6. 下一步（来自 /tmp/pyadamas-todo.md）

- P2 已完成（0.464 Path / 0.465 LSP / 0.466 fannkuch），当前工作区未提交。
- P3-T8 `check` 校验模块属性；T9 enum 剩余语义；T10 os.stat/datetime.strptime；
  T11 内存调试覆盖二级缓冲。P3 先写方案再动手。
- **账目缺口**：`35089ac`（re.Pattern/Match）、`56ab9f8`（datetime 三类）、`af2acf4`
  （全局提升 + dict contains/get 融合）还没有 CHANGELOG 条目；若要补，另起 0.464+。

## 7. 纪律（沿用）

- 每轮：增量构建 → `checkall --full` → CHANGELOG + 选择性 `git add` → push。
- 性能改动只许泛化（IR 结构判据），禁止基准/文件名特化；abbench 交替绑核，
  profile 只找候选、abbench 定生死；布局敏感先 objdump。
- 改语言语义/运行时行为必须同步 `language.md` / `compatibility.md` / `runtime.md`。
- `docs/index.md` 的文档维护规则仍适用；CHANGELOG 数字必须是实测。