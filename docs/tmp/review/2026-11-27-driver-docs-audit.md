# 驱动层代码审计 + 文档审计（0.637 基线）

> 临时审计产物，不进入 `docs/index.md`。
> 基线：`a28c819`（0.637）。审计期间工作区另有在途改动（`src/syntax/*.cpp`），与本审计无关，未触碰。
> 范围：`src/driver/` 全部 11 个 TU / 4678 行；`docs/` 非 archive 的 74 篇。
> 方法：全文阅读 + 与代码对表 + 最小复现（`build/lead/pyadamas`，实验目录 `/tmp/drvprobe`）。
> 未做：闸门（`tools/checkall.sh`）、ASan、difftest；不含任何性能结论。

## 摘要

| 类别 | 数量 | 条目 |
|---|---|---|
| 驱动层（高） | 2 | A1 A2 |
| 驱动层（中） | 3 | A3（含 3 处不对称）/ A4 / A5 |
| 驱动层（低） | 9 | A6 表 |
| 文档：与代码矛盾 | 7 | B1 |
| 文档：失效的文件引用 | 8 | B2 |
| 文档：失效的用例引用 | 5 | B3 |
| 文档：索引/可达性 | 1 | B4 |

**有可执行复现的问题 3 条**：A1、A3-1、A4；A7 是经实测的正面结论。其余为读代码 + 对表推导，证据强度逐条标注。

---

## A. 驱动层代码

### A1（高）`match` 体内的 import 不进依赖图 —— 误报 + check/build 不一致

位置：`src/driver/modules.cpp:359`（`scan_imports_stmt` 的 switch 缺 `StmtKind::Match`；枚举见 `src/common/ast.h:544`，`Match::cases[].body` 见 `src/common/ast.h:917`）。

现象：`import helper` 写在 `case` 分支里，driver 扫不到 → helper 不入 `Program::units`。build 拖到 codegen 才炸，并附带一条误导性诊断；check 走惰性 lookup，反而说 ok。

复现（`/tmp/drvprobe/m3.py`）：

```
$ pyadamas build m3.py -o m3
m3.py:4:13: error: cannot resolve module 'helper'
m3.py:5:32: error: method 'twice' is not supported by the code generator yet
2 errors in code generation
$ pyadamas check m3.py
ok: m3.py (2 statements, 1 functions, 9 expressions)
```

内置模块（`math` 等）看不出问题：`is_native_module()` 在 `src/driver/build.cpp:455` 就短路，不需要源文件。

同族缺口：`cimport` 的 link 参数只在顶层 body 收（`src/driver/build.cpp:379` 直接遍历 `u.ast->body`），函数体里的 `cimport` 声明的 `links` 会被丢掉。

修法：`scan_imports_stmt` 补 `Match`（并考虑把 `cimport` 扫描并入同一递归）；加一条 `tests/run/` 用例钉住。

### A2（高）入口模块产物缓存的键漏了「全程序」输入

位置：
- `src/driver/build.cpp:913` —— 入口 asm 用 `u.is_entry && !p.needs_unknown_index` 决定是否发 `pa_disable_unknown_index`（`src/backend/codegen.cpp:715`、`:829`）；
- `src/driver/build.cpp:314` —— manifest 只写 `u.needs_unknown_index`（本模块值）；
- `src/driver/build.cpp:601` —— `try_cache` 对依赖只比 `interface_hash`。

后果：某个依赖的 **body** 变了、使它的 `needs_unknown_index` 由 0 变 1，而**接口签名没变**时，入口仍命中旧产物，而那份汇编里带着进程级「关索引」。索引是运行期全局开关，关掉后依赖侧需要索引的探针（`IsStr` / `TruthAny` / dict 混合键那一批，清单见 `src/ir/ir.cpp:467` 起）会退化到 exact-RC 回退路径。

证据强度：读代码推导，**未复现**（构造「只翻 needs 不动接口」的依赖改动比较别扭）。但口径是硬的：`p.needs_unknown_index` 是程序级 codegen 输入，不在入口模块的缓存键/校验里。

修法（二选一）：入口 manifest 记程序级值；或把每个依赖的 `unknownindex` 也纳入 `try_cache` 比对。

### A3（中）check 与 build 的口径不对称（3 处）

1. **依赖语法错误被吞**。`src/driver/check.cpp:212` 的 `catch (...)` 把 LexError/ParseError 与 `bad_alloc` 一视同仁，记 `missing` 后按 unknown 处理。
   复现（`/tmp/drvprobe`，`usesbad.py` 里 `import badep`，`badep.py` 有语法错）：
   ```
   $ pyadamas check usesbad.py     # exit=0
   ok: usesbad.py (2 statements, 0 functions, 2 expressions)
   note: 1 construct(s) parsed but not yet compiled: import
   $ pyadamas build usesbad.py -o ub   # exit=1
   badep.py:1:7: error: expected parameter name, found ':'
   ```
   提示语 `parsed but not yet compiled` 对有语法错的文件是误导。
2. **同名不同文件**。`src/driver/check.cpp:221` 按 name 记忆化，不看 importer；build 端有 `module '{}' resolves to two different files`（`src/driver/build.cpp:488`），check 端没有。
3. **槽冲突检测只在 build**。`src/driver/build.cpp:749` 的 `add_slot` 冲突检查在 `src/driver/check.cpp:89` 没有对应实现 → check 放行、build 才报 cross-module slot conflict。

### A4（中）重复的子模块 import → 该模块永久缓存 miss

位置：`src/driver/build.cpp:517`（child 推入队列只判 `p.units.count`，不判队列是否已有）、`src/driver/build.cpp:403`（合并同名依赖时 `from_names` 无条件 append，不去重）。

复现（`/tmp/drvprobe/dup2`，`from pkg import a` 写两遍，`pkg/a.py`）：

```
第一次：building pkg / pkg.a / __entry__；manifest 里 dep pkg.a 出现两次
第二次：cached pkg / pkg.a / __entry__ → building __entry__
```

原因：`u.deps` 有 3 项、`read_manifest` 的 `std::map` 只有 2 项，`m->deps.size() != u.deps.size()` 恒不等。属纯性能问题，但「怎么写都不命中」很难查。

### A5（中）缓存的原子性与并发

- 单文件原子写用 pid+seq 唯一临时名（`src/driver/build_cache.cpp:57`），正确；但 `src/driver/progcache.cpp:86` 用固定名 `to + '.progcache.tmp'`、`src/driver/lock.cpp:897` 用 `file + '.tmp'` —— 同一目录并行 build / resolve 会互相踩。
- 模块缓存是三个文件（`mod.o` / `mod.pya` / `manifest`），单文件原子、跨文件不原子，校验只认 manifest。缓存目录键（`src/driver/build_cache.cpp:127`）只有 name+version+source，**不含编译开关**，所以 `--rebuild -O0` 与 `-O2` 共用同一目录：若进程在写完 `mod.o`、未写 manifest 时被杀，旧 manifest 仍「合法」，下一次 -O2 构建会拿到 -O0 的对象。
- 修法：manifest 记 `mod.o` 内容摘要；或重建前先删 manifest。

### A6（低）

| 位置 | 问题 |
|---|---|
| `src/driver/link.h:11` vs `src/driver/link.cpp:53` | 头文件说 `--gc-sections`「默认关」，实现默认**开**；`src/driver/build.cpp:1050` 注释同样过期 |
| `src/driver/check.cpp:248` | `entry->source` 是相对路径（如 `argparse.py`），却与绝对 `stdlib_dir()` 做前缀比较 → `in_compiler_stdlib` 恒 false，死分支（后按解析后的绝对路径判，行为仍对） |
| `src/driver/modules.cpp:315` | `#ifdef NDEBUG return true` —— 模块表自检（源文件存在 / deps 已登记）在正式二进制里从不运行 |
| `src/driver/build.cpp:128` | 匿名命名空间的 `struct Manifest` 与 `src/driver/manifest.h:40` 的 `pyadamas::Manifest` 同名；今天不冲突（未 include），将来一 include 就 ambiguous |
| `src/driver/check.cpp:59` | `const_cast` + no-op deleter 的非拥有 `shared_ptr<ClassInfo>`，靠 `ctx.keep_alive` 兜生命期 |
| `src/driver/lock.cpp:987` | `diff_locks` 的 dep 以 (from,to) 为键，同一 (from,to) 多条 path 边只比最后一条 |
| `src/driver/build.cpp:775` 起 vs `src/driver/check.cpp:49` 起 | 扁平类导出逻辑两份平行实现（约 120 行重复），已出现偏离（槽冲突只在 build） |
| `src/driver/check.cpp:328` | `load_check_lookup` 为收集 missing 把入口再解析一遍；异常靠 `src/main.cpp:785` 的兜底 catch 接住（实测不崩，但依赖外层） |
| `src/driver/modules.cpp:266` / `:465` | `find_module_entry` 每次线性扫约 70 项；`merge_extension_module` 每次 lookup 深拷贝一份 `ModuleInfo`（`program_module` 是热路径） |

### A7 正面结论（建议保留）

- 缓存键 = **内容**，不是 mtime/路径（`src/driver/build_cache.cpp:115` 的注释把踩过的坑写清了）；manifest 缺字段一律失效，fail-closed。
- `progcache` 键从**真实链接输入**推导（每个 .o 的内容摘要 + target/toolchain/开关/链接参数），不从命令行选项名单猜。
- 锁：行式 / 排序 / 无时间戳与绝对路径 / `sha256:` + 64 位小写校验 / `..` 只在 pkg 路径放行；`--locked` 与宽松模式的分流、以及 `diff_locks` 的方向经 `src/main.cpp:213` 核对一致。
- `src/driver/sha256.cpp` 实测正确：空串 / `abc` / 1e6 个 `a` 三条标准向量全对，7 字节分块的流式结果与非流式一致。

---

## B. 文档（非 archive 74 篇）

### B1 与代码矛盾

| 位置 | 问题 |
|---|---|
| `docs/experiment-switches.md:20` | `PYADAMAS_TYPE_TAGS` 标「默认 关 / 保持 opt-in」；实际 `src/main.cpp:464` 注释「0.528 起默认开」，且未设时按需自动开（源码出现 `isinstance` 即开，`src/driver/program_opts.cpp:13`）。表的「默认＝不设环境变量」口径在这条失效 |
| `docs/experiment-switches.md:1` | 标题「（0.481）」，表内已有 0.522 / 0.530 / 0.531 的条目 |
| `docs/codegen.md:642`、`docs/development.md:241` | 教用户用 `PYADAMAS_KEEP_ASM=1`；该变量在 `src/ runtime/ tools/ tests/ stdlib/ CMakeLists.txt` 全无（已 grep 确认），敲了是静默无效 |
| `docs/vectorization-coverage.md:1`、`:14` 说「7 个」vs `:34`、`:54`、`:56`、`:265` 仍写「8 个」 | 同一页两套结论（0.479 收尾 vs 0.423 口径）未统一 |
| `docs/index.md:27` / `:35` / `:149` | 同一篇文档三处数字互相打架：「剩余 7 个」「8 个 0 触发基准」「其余 8 个基准」 |
| `docs/index.md:27` 与 `:35` | `vectorization-coverage.md` 在主表占两行，且描述不同 |
| `docs/big-landing.md:60`、`:65-67`、`:94` | 用 `build/pyadamas`、`build/rt.o`、`build/rt_asan.o`；现产物在 `build/<preset>/`（`docs/HANDOFF.md:31` 与 AGENTS.md 都写 `build/lead`），照抄跑不通 |

### B2 指向已搬迁 / 已删除的源文件

| 位置 | 现在在哪 / 状态 |
|---|---|
| `docs/index.md:32`、`docs/development.md:537` 的 `src/lower/builtin.cpp` | `src/lower/builtin/builtin.cpp` |
| `docs/compatibility.md:321` 的 `lower/builtin_container.cpp` | `src/lower/builtin/` |
| `docs/unknown-semantics-audit.md:122` 的 `src/lower/builtin_reflect.cpp` | `src/lower/builtin/` |
| `docs/str-codepoints.md:590` 的 `src/lower/method_str.cpp` | `src/lower/method/` |
| `docs/native-stdlib-cookbook.md:12,28,271,277,357,364,528` 的 `src/lower/import.cpp`；`:30,365,456` 的 `import_native.cpp`；`:221,462` 的 `lower/method_misc.cpp` | `src/lower/module/`、`src/lower/method/` |
| `docs/type-tags-landing.md:66` 的 `src/backend/tag_defs.h` | `src/common/tag_defs.h` |
| `docs/next-round-plan.md:49` 的 `vcall_inline.cpp` | `src/backend/vcall_inline.cpp` |
| `docs/modularization.md:131` 的 `backend/simd.h`、`docs/simd-vectorization-design.md:143` 的 `src/backend/simd.cpp` | 0.480 已删（`docs/index.md:22` 自己标了「已删」） |

### B3 指向不存在的用例

| 位置 | 实际 |
|---|---|
| `docs/mi-layout.md:100` 的 `tests/strict/mi_non_primary_base_fields.py` | 目录与名字都变了：`tests/run/mi_primary_base_fields.py` |
| `docs/str-codepoints.md:572,582,592,657,658,660,662` 的 `163/164/165/166_codepoint_*.py` | 现为 `tests/run/176_codepoint.py` |
| `docs/compatibility.md:727,742` 的 `tests/run/159_exc_multi.py`、`158_isinstance_bytes.py` | 均不存在 |
| `docs/frontend-fixes.md:145` 的 `tests/unsupported/unknown_receiver_method.py` | 目录改名：`tests/strict/` |
| `docs/testing.md:68` | 拿 `big_round.py` 当「描述性命名」范例，该文件不存在（同类的 `re_anchors.py` / `fnmatch_anchor.py` 存在） |

### B4 索引与可达性

`docs/index.md` 无坏链，但**未收录 8 篇**：`frontend-fixes.md`、`refactor-plan.md`、`stdlib-round2.md`、`stdlib-batch2.md`、`stdlib-batch3.md`、`stdlib-batch4.md`、`stdlib-batch6.md`、`archive/roadmap-0.6-0.38.md`。

stdlib 那组源于 `stdlib-batch1.md ～ stdlib-batch7.md` 的区间写法：只有 1 和 7 可点，2/3/4/6 悬空（batch5 本就不存在，也未说明）。

### B5 只记录、不建议改

- `docs/HANDOFF.md:108` 引 `vcall_inline.cpp` / `fuse.cpp` 未带 `src/backend/` 前缀 —— AGENTS.md §7 规定代理不动 `HANDOFF.md`。
- `docs/regalloc-architecture.md:314-328` 引 LLVM 源码路径 —— 对照对象，不是本仓库文件。
- `docs/package-management-design.md` 里的 `PYADAMAS_HOME`、`Cargo.lock`、`deno.lock` 等是对照分析，不是可用开关。
- `docs/archive/*` 里的旧文件名是历史快照，不修。

---

## C. 复现清单

```bash
# A1：match 体内的 import
cd /tmp/drvprobe
build/lead/pyadamas build m3.py -o m3     # exit=1（cannot resolve module 'helper'）
build/lead/pyadamas check m3.py           # exit=0（ok）

# A3-1：check 吞掉依赖的语法错误
build/lead/pyadamas check usesbad.py      # exit=0（ok）
build/lead/pyadamas build usesbad.py -o ub # exit=1（badep.py:1:7）

# A4：重复子模块 import 产生重复 dep 行、入口永远 miss
cd /tmp/drvprobe/dup2
PYADAMAS_CACHE_DIR=$PWD/cache build/lead/pyadamas build -v main.py -o m
grep -r 'dep pkg.a' cache/*/manifest      # 同一行出现两次
PYADAMAS_CACHE_DIR=$PWD/cache build/lead/pyadamas build -v main.py -o m  # 仍 building __entry__

# A7：sha256 向量 + 流式一致性
g++ -std=c++23 -I src /tmp/drvprobe/shatest.cpp src/driver/sha256.cpp -o /tmp/shatest && /tmp/shatest
```

## D. 未覆盖

- 闸门 / ASan / difftest 一次未跑；无性能数字。
- `pya-lsp/`（独立仓库）、`src/cimport/`、`ir/module_map` 的加载器、`src/main.cpp` 的参数解析未审。
- 包管理只审到 driver 侧实现（`manifest.cpp` / `lock.cpp`），未审其上游设计取舍（`docs/package-management-design.md`）。

## E. 建议顺序（按 AGENTS.md §0）

1. A1（误报，必修）+ 一条回归用例；
2. A2（潜在陈旧命中，先补复现再修）；
3. A3 的三处 check/build 口径统一；
4. A4 / A5（缓存健壮性，可与上并行）；
5. B 组文档等对应代码改完后一次性对齐，避免二次返工。
