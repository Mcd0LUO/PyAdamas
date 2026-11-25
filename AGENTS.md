# AGENTS.md —— 给在这个仓库里干活的人和代理

> 目标：**用最少的命令拿到可信结论**。这个仓库的构建/测试/基准都不便宜（全量 3–5 分钟），
> 所以「什么时候跑什么」比「跑得多」重要。以下规则对所有代理（含子代理）生效。

## 1. 先看这几条（最容易踩的）

1. **非必要不跑全量 ASan。** 默认构建就是 Release，跑功能用普通构建；见 §3 的「必须跑 ASan」清单。
2. **difftest 只在改了「会进生成代码」的部分时跑**；纯文档、纯运行期 C、纯类型检查放宽通常不需要，见 §4。
3. **优先用 `tools/` 里已有的脚本**，不要手搓 `find | xargs` 循环；见 §5 的清单。
4. **每次合并只跑一次闸门**（§2），不要每改一行跑一遍。
5. 测行为用**单个用例**：`PYADAMAS_TEST_FILTER='<名字>' bash tests/run_tests.sh build/lead build/lead/pyadamas`。

## 2. 构建与闸门

```bash
# 构建（增量，改 C++ 后约 30 秒）
cmake -S . -B build/lead -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/lead -j8

# 闸门：一次跑完 build / suite / difftest / check_size
bash tools/checkall.sh --quick                 # 日常合并用这个
bash tools/checkall.sh --full                  # 发版/大改：+ 12 基准 perf + 向量化自检 + git 状态
ctest --test-dir build/lead                    # 9 项（含 import 检查、traceback、opt 档位、progcache、pkg、gc_conservative）
```

- **改运行期 C**：`build` → 单用例 → 全套件 → 闸门；不要跳过套件。
- **只改文档**：不用跑任何测试，`git status` 干净即可。
- **只改运行期且没碰 codegen**：difftest 可以省，但要在提交信息里写明「未跑 difftest 的理由」。

## 3. ASan：什么时候必须跑

全量 ASan 套件 = 重新构建一份 `-fsanitize=address` 的编译器 + 全跑，时间成本高。**只在下列情况跑**：

| 场景 | 跑什么 |
|---|---|
| 改 `runtime/` 的内存布局、RC/GC、分配器、索引（`rt_core.c` `rt_*.c`） | 全量 ASan 套件：`PYADAMAS_TEST_ASAN=1 bash tests/run_tests.sh ...` |
| 新增/删除堆对象种类、改 RC kind、改 GC 根 | 同上 |
| 修 UAF/越界类 bug | 先用 ASan 复现（`build --asan`），修完再跑全量 |
| 只改类型检查/降级/后端（不碰 runtime） | **不跑全量 ASan**，最多对目标用例 `build --asan` 跑一次 |
| 只改文档/基准脚本/测试脚本 | 不跑 |

单用例 ASan：
```bash
build/lead/pyadamas build --asan -O1 -o /tmp/t tests/run/<case>.py
ASAN_OPTIONS=detect_leaks=0 /tmp/t
```

## 4. difftest：什么时候跑

`tools/difftest.sh` 比对同一份源码在 `-O0` 与 `-O2` 下的输出（抓优化遍的静默错误）。

- **要跑**：改 `src/backend/`、`src/opt/`、`src/lower/`、IR 结构、或任何可能影响生成代码的类型检查改动；
- **不必跑**：纯运行期 C（不动 codegen）、纯文档、纯 pya-lsp、纯测试脚本；
- **只跑子集**：`PYADAMAS_TEST_FILTER='3[0-4]*' bash tools/difftest.sh`；
- 注意 `-O0`/`-O2` 的**墙钟时间天然不同**，别用 difftest 判性能。

## 5. 优先用 `tools/` 里的脚本

| 脚本 | 用途 | 典型命令 |
|---|---|---|
| `tools/checkall.sh` | 闸门（`--quick` / `--full`） | `bash tools/checkall.sh --quick` |
| `tools/difftest.sh` | `-O0` vs `-O2` 差分 | `PYADAMAS_TEST_JOBS=8 bash tools/difftest.sh build/lead/pyadamas` |
| `tools/frontend_baseline.sh` | **前端基线四口径**（syntax/default/old/imports） | `bash tools/frontend_baseline.sh` |
| `tools/abbench.sh` | A/B 性能（独立 worktree + 交替测量 + 中位数） | `bash tools/abbench.sh HEAD realistic 11` |
| `tools/quickbench.sh` | 单版本回归计时（最好 N 次） | `bash tools/quickbench.sh build/lead` |
| `tools/opt_stats.sh` | 优化遍统计（哪些遍真正改了 IR） | `bash tools/opt_stats.sh` |
| `tools/flagbench.sh` | 编译选项矩阵 | `bash tools/flagbench.sh` |
| `tools/decexpect.py` / `tools/decfuzz.py` | 决策/边界小工具 | 见 `tools/README.md` |
| `tools/codon_cmp.py` | 与 Codon 对比 | 见 `tools/README.md` |

**别自己写**：`find /usr/lib/python3.14 -name '*.py' | xargs ... check` 这种循环——`frontend_baseline.sh` 已经做了，
而且口径（文件集、是否 `--imports`）统一，避免数字漂移。

## 6. 测试用例写法

- **新用例不编号**（不要 `570_xxx.py`），直接用有意义的文件名；
- **不要用 `f<数字>` 前缀**（`difftest.sh` 会把它当反例组跳过）；
- **不要打印墙钟时间**（`difftest` 会判不一致）；
- 输出要与 CPython 逐字节可比；不确定顺序时先排序再打印。

## 7. 提交与文档纪律

- **CHANGELOG 只由 lead 写**；代理不写、不改版本号；
- **代理不 push**；合并与推送由 lead 做；
- **不要动** `build/release/`（0.271 的对照构建）、`HANDOFF.md`（除非明确要求）、`pya-lsp/`（独立仓库）；
- git add 尽量**限定路径**（`git commit -- <files>`），避免把别人在途的改动扫进来；
- **文档里的数字必须实跑**，并写清口径（文件集、`--imports` 与否、机器）；判负也要记录理由与前置条件。

## 8. 目录速查

| 路径 | 内容 |
|---|---|
| `src/frontend/` | 词法/语法 |
| `src/typecheck/` | 类型检查（窄化、接口表、类布局） |
| `src/lower/` | IR 生成（含多继承 thunk、导入接线） |
| `src/backend/` | x86-64 汇编生成（含虚调用守卫） |
| `runtime/` | C11 运行期（RC/GC、容器、内建模块） |
| `stdlib/` | `.py` 标准库 + `sigs/*.pysig` 声明式签名 |
| `tests/run/` `tests/strict/` | 正例 / 反例 |
| `tools/` | 开发基础设施（见 §5） |
| `docs/` | 设计文档；**索引在 `docs/index.md`**，接手先读它 |
