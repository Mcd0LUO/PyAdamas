# 语义审计方法论（playbook）

> 2026-11-23（0.422–0.428）。本文固化最近四轮审计（unknown 类型语义 / 算术语义 / 切片语义 / float→int 转换）
> 反复用到的一套方法：**怎么找出「静默错值」类 bug、怎么排除「设计取舍」、怎么避免修一半**。
> 相关文档：[compatibility.md](compatibility.md)（已声明的偏差清单）、
> [unknown-semantics-audit.md](unknown-semantics-audit.md)（unknown 分派审计的完整案例）、
> [optimization-roadmap.md](optimization-roadmap.md)（pass 顺序依赖的同类「隐式假设」问题）。

## 1. 穷举对拍 > 手写用例

手写用例覆盖的是「你想到的形状」；静默错值往往藏在**你没想的组合**里。

**做法**：把参数放进变量，用循环一次跑完整个小空间，两边（CPython / PyAdamas）逐行 diff。

```python
# 切片示例：start/stop 各 12 个取值、step 6 个取值 → 864 组，一次跑完
starts = [-100, -8, -7, -3, -1, 0, 1, 3, 6, 7, 8, 100]
stops  = [-100, -8, -7, -3, -1, 0, 1, 3, 6, 7, 8, 100]
steps  = [-3, -2, -1, 1, 2, 3]
for a in starts:
    for b in stops:
        for c in steps:
            print(a, b, c, xs[a:b:c])
```

**收益（实测）**：切片赋值那轮 484 组组合里 **99 行与 CPython 不同**，直接定位到「`start > stop`
的负区间/空区间形状」——手写 10 个用例大概率只会覆盖 `[1:3]` 这种正向形状，漏掉这个 bug。

**适用场景**：切片/下标、clamp/负数/步长、格式化、比较链、索引越界这类「规则组合多」的语义。
对拍脚本要**同时**跑三档（`-O0/-O1/-O2`）和必要的门控组合（`PYADAMAS_SIMD=1` 等）。

## 2. 先查文档，再判 bug

**看起来像 bug 的行为，先 grep 文档**：`docs/compatibility.md`（「语义差异（故意的）」表 +
各条「待定」小节）、`docs/language.md`（类型/运算语义表）。

算术那轮至少有 3 个「候选 bug」查完文档后被排除：

| 现象 | 结论 | 文档依据 |
|---|---|---|
| `MAX + 1` 得 MIN | 设计取舍（性能来源） | compatibility.md「整数溢出」 |
| `2 ** -1` 得 0 | 设计取舍 | compatibility.md「`2 ** -1`」 |
| `1 << 63` / `0.0 / 0.0` | 设计取舍（移位按 64 取模 / IEEE 除零） | compatibility.md + language.md |

反例是「确实没写进文档」的偏差：`MIN // -1` 抛 OverflowError、`int(float(2**63))` 抛
OverflowError —— 这类要**补进 compatibility.md**，否则下一轮又会被当 bug 查一遍。
（0.428 轮已补。）

## 3. 静态设计差异不是 bug

PyAdamas 是静态类型编译器，很多 CPython 的运行期错误在它这里是**编译期拒绝**：

| 写法 | CPython | PyAdamas | 判定 |
|---|---|---|---|
| `a[1.5]` / `a["x"]` | 运行期 `TypeError` | 编译期报错（`list index must be int`） | 设计，不修 |
| `s[1:2] = "z"` / `t[0] = 5` | 运行期 `TypeError` | 编译期报错 | 设计，不修 |
| `s = slice(1,3)`（作为值） | 允许 | 编译期拒绝（只支持内联 `xs[slice(...)]`） | 设计，不修 |
| 未标注变量重绑定不同类型 | 允许 | 编译错误（槽表示不兼容） | 设计，不修（compatibility.md 已记） |

**判据**：差异发生在「编译期拒绝」而非「运行期错值/错异常」时，先假设是设计；
只有**运行期静默错值**才是优先修的 bug。

## 4. 测试可能钉住 UB（修语义会让测试反而失败）

`tests/run/288_simd_nbody_scalar.py` 的 expect 曾把 `int(NaN)` 的 UB 垃圾值
（`-9223372036854775808` 等）钉死；0.427 修好 `int(inf)/int(nan)` 后该测试反而 OUTPUT DIFF。

**检查清单（修任何「以前是 UB / 未定义行为」的语义时）**：

1. 全仓库 grep 相关调用形状，看 expect/注释里有没有旧 UB 结果；
2. 有的话：把测试改成**不依赖 UB 的表示**（例如「非有限 → 记 -1」）而不是删断言；
3. 同一用例在 `-O0/-O1/-O2` + 相关门控下重跑，expect 只写稳定输出。

## 5. 修复要在语义层，不是现象层

切片负区间 bug 的第一反应是「给 `start > stop` 加特殊处理」；实际 CPython 规则统一为
**「步长 1 的空切片，插入点 = start」**——1 行 `if (st > 0 && lo > hi) hi = lo;` 修掉全部 99 例。

**做法**：先把差异归纳成一条规则（对着 CPython 语义/文档），再改代码；现象层特判会让
「99 例差异」变成「补 99 个 if」。

同类例子（pass 顺序那轮）：`if_convert_accumulators` 不命中不是「判据不够宽」，而是**调度顺序**
错位——补第二遍调用（纯调度）优于放宽判据（见 optimization-roadmap.md「pass 顺序依赖」）。

## 6. 找全同类点位（修一处必须扫全）

同一语义往往有多份实现/多个分派点，**修一处不扫全 = 只修半个 bug**：

| 语义 | 已知点位（实测） | 漏掉的后果 |
|---|---|---|
| float→int 转换 | codegen `cvttsd2siq`（int 目标 / 定宽目标两处）+ `rt_math.c` 的 floor/ceil/trunc（+ round 已有检查） | 只修 codegen，`math.floor(inf)` 仍静默得 MIN（0.427 实况） |
| unknown 种类分派 | `rt_dict.c` key_kind / `rt_list.c` list_value_eq+chain_elem_eq / `rt_any.c` pa_any_cmp(+`_t`) / `rt_list.c` sort kind | 只修 dict，混合容器的 in/index/==/sorted 四处仍错（0.422–0.425 实况） |
| `del` 切片 / 切片赋值 | 运行时入口（assign / del）+ lowering（del 曾复用 assign 的「空序列」路径） | 扩展步长 `del a[::2]` 直接抛 ValueError（0.428 实况） |
| 动态比较 | `pa_any_cmp` 与 `pa_any_cmp_t`（type_tags 默认开时实际跑的是 `_t`，IR 文本仍印非 `_t`） | 只修一个符号：完全无效（0.424 实况） |

**做法**：`grep` 全仓库相关指令/函数/类型码（如 `cvtt`、`kind ==`、`switch (kind)`、
`dup` 实现）；新增运行时入口时同步检查「枚举 / 名字表 / codegen 实参形状 / borrow 分类 /
lowering 调用点」五处（0.428 的 `ListSliceDel` 就改了 5 个文件，borrow 分类漏了不报编译错）。

## 7. 标准流程（每轮照做）

1. **列审计项**：按语言语义分区（容器 / 算术 / 转换 / 类型未知 / 异常），每项先写最小对拍脚本；
2. **跑三档 + 门控**：`-O0/-O1/-O2`、必要时 `PYADAMAS_SIMD=1`，与 CPython 逐行 diff；
3. **分类**：一致 / 设计取舍（查文档）/ 编译期拒绝（设计）/ **静默错值（优先修）** / 异常文案差异（低优先）；
4. **穷举补刀**：对「规则组合多」的语义写变量循环对拍（§1）；
5. **实现前先找全同类点位（§6）**，实现时优先语义层规则（§5）；
6. **检查既有测试是否钉住 UB（§4）**；
7. **新用例**（`tests/run/NNN_*.py`，带 expect，三档一致）+ 全量闸门（run_tests / difftest / ASan / check_size /
   12 基准输出与指令数 / SIMD 量级）；
8. **补文档**：新的设计取舍写进 compatibility.md（§2）；方法本身有新教训写回本文。

## 8. 效果（各轮实测）

| 轮次 | 主题 | 找到的真 bug | 关键手段 |
|---|---|---|---|
| 0.422–0.425 | unknown 类型分派 | 4 个（dict 键 / list in-eq / any_cmp / sort） | 混合容器构造 unknown 形状（推断会过度精确掩盖 bug） |
| 0.426–0.427 | 算术 / 转换 | 2 个（bool 一元类型 / int(inf,nan) 静默 MIN） | 先查文档排除设计取舍；grep 找全转换点；`jp` 先于 `je` |
| 0.428 | 切片 | 2 个（负区间赋值静默错值 / del 扩展切片） | **穷举对拍 864 组**、484 组赋值对比出 99 行差异 |
| 0.429 | 字符串 | 1 个（replace count=0 替换一次，str/bytes 两份实现） | 穷举 count ∈ {0,1,2,-1,-5}；`grep "case 11"` 找全同点位（§6） |
| 0.430 | 优化交互 | 1 个（-O2 DCE 吞掉会抛异常的 int(float)，0.427 的回归） | 改语义 → 审计优化器纯指令假设（§10）；语句形态最小复现 |
| 0.431 | EH 控制流 | 1 个（finally 里 raise 在早退路径逃出调用方 except） | IR/汇编对照正常路径 vs 早退路径落点；LSDA 覆盖是代码位置（§5） |

## 9. 探针要按「方法组」拆分

把**不支持的方法**和常用方法放进同一个探针文件，会让整份文件**编译失败**（`not supported by the
code generator yet` / 参数个数不符），于是拿不到其它方法的结果 —— 字符串轮就踩过：
`rsplit` / `partition` / `swapcase` / `format` / `startswith(tuple)` 任何一个在文件里，整份探针
都跑不起来。

**做法**：

1. 一个方法组一个文件（split 组、find/replace 组、strip/case 组、格式化组、运算组…）；
2. 先跑「支持面」，再单独列「不支持项」（编译期拒绝本身就是结论，不需要运行）；
3. 不支持项记进审计表的「未实现/设计取舍」列，不要混进对拍输出。

## 10. 改语义 → 必须审计优化器假设（连锁失效）

**给指令增加「可能抛异常/有副作用」的语义，必须同步检查优化器对它的纯指令/可删除假设。**

实例（0.427 → 0.430）：0.427 让 float→int 转换在 NaN/±inf/越界时抛异常；但
`inst_is_pure(IrOp::Convert)` 仍把目标为 `int` 的转换当纯指令，`dce_function` 把「结果没人用」的
转换整条删掉 → `int(float("inf"))` 语句形态在 -O2 下**异常静默消失**（-O0/-O1 正常）。

**检查清单（改一条指令的语义时）**：

1. `grep` 该 `IrOp` 在所有优化 pass 里的假设：`inst_is_pure` / DCE / CSE / 常量折叠 /
   循环不变量外提 / 向量化分类（`classify`）/ 内联判据；
2. 特别看「删除/合并/移动」类优化：纯指令假设一旦失效，错的是**可观察行为**（异常被吞、
   副作用丢失），而且往往只在 -O2 出现；
3. 修完补一条**语句形态**的回归（结果不使用）：这是 DCE 类 bug 的最小复现形状；
4. 同类历史：0.423 的 Cmp/bool 加进向量化分类要同步 `has_cmp` 强制 SSE2；0.431 的 EH
   语义要同步检查「LSDA 覆盖是代码位置」这类隐式假设（见 exc-semantics.md §2.1.1）。

## 11. pass 有效性验证（tools/opt_stats.sh，0.433）

优化 pass「写了但从未生效」是隐蔽故障：0.420 的 `promote_globals_in_loops` 判据只数
`body→exit` 边，对标准循环恒不成立（pass 自写入以来从未跑过）；0.419 的 `fuse_dict_bump`
判据过窄也靠同一手法发现。这个能力已固化成工具：

```bash
# 12 个基准 + tests/run/ 全量；两次编译（默认配置 / 全部门控打开）
bash tools/opt_stats.sh
# 只跑 12 个基准（快）
bash tools/opt_stats.sh --quick
```

开关：`PYADAMAS_OPT_STATS=1`（默认关，关闭时驱动里只多一次静态 bool 分支，零成本）；
输出（stderr，退出时汇总）：

```
pass=<name> changed=<n> funcs=<n>
zero_hit=<name1>,<name2>
```

`changed` 是「pass 前后函数 IR 指纹变化」的函数次数；`funcs` 是 pass 被调用的函数次数。
指纹覆盖块序列 + 每条指令的操作码/操作数/立即数/跳转目标/实参/类型，所以改 CFG、改操作数、
改立即数都会算命中；**pass 只在驱动里包一层**（`src/opt.cpp` 的 `opt_stats_run`），不污染 pass 本身。

**同名 pass 调用两次要分别计数**（`forward_slot_loads#1` / `#2`、`if_convert_accumulators#1` / `#2`、
`inline#1` / `#2`、`cse#1` / `#2`）—— 历史上「第二次调用 0 命中」是插桩命名伪影，工具通过调用点后缀
避免重名合并。

**门控白名单**：`self_unroll` / `dedup_block_loads` / `cse#2` / `thread_jump_blocks` /
`version_bounds_affine` 默认关是既定策略。脚本跑两阶段（默认 / `PYADAMAS_LOOP_OPT=7`
`+ PYADAMAS_INLINE_OPT=1`），据此区分「门控关 → 未调用」与「判据坏 → 0 命中」；
**0 命中且不在白名单里的 pass 高亮为 SUSPICIOUS，脚本退出码 1**。

判读经验：

1. 新加 pass 后先 `--quick` 看它是否在基准上有命中，再跑全量核对 tests 覆盖；
2. 0 命中先查**调用点是否真的被走到**（门控/env 传参），再怀疑判据；
3. 指纹命中 ≠ 语义正确：工具只回答「有没有变」，正确性仍靠 run_tests / difftest / ASan。

