# P3 BigInt：静态 big 与部分 unknown 接入（big2 候选）

分支从 0.356（8e8873e）分出，接续块 A–C 的 b6005d8、32cc212、0e70907。此文记录候选及未完成项，不代表已经合入或发布。

## 已实现

- `big` 标注、超 u64 的十进制字面量、永生静态字面量；有明确 big 上下文的 u64 大字面量保留无符号幅值（包括跨模块默认参数）。旧的裸 `int` 仍是 i64；`2 ** 100` 不会凭结果自动升级，先给底数 `big` 类型。
- 静态算术、增量赋值、无限补码位运算、移位、真值、big/整数比较、精确 big/float 比较和有静态 big 的链式比较。整数真除有限值正确舍入（包括亚正规、半程到偶数、带符号零），过大抛 OverflowError；`//` 向下取整、`%` 与除数同号、除零可捕获。
- `cast_big(x)` 支持整数（含完整 u64）、布尔、float、ASCII 十进制 str 和数值 unknown；`cast_i64(big)` 等显式窄化取低位。静态 `int(big)` 恒等，`float(big)` 正确舍入。静态文本入口包括 print、str、repr、hex、bin、oct、整数 format/f-string、abs 和两参数 pow。
- 现有动态分派可识别 BIG 标签和探针命中的 BigInt：`+ - * // %`、负号、单步比较、真值、print/str/repr、标记模式的 `isinstance(v, int)`，以及显式还原为 big。动态值打印可以递归显示列表/字典中的 BigInt。并非所有 unknown 表达式都走该分派（下节）。
- 对象是 RC 跟踪的不可变叶子，limb 内联、无堆出边。动态提升临时对象将初始引用交给临时栈；转回 big 会返回一个自己的引用。异号除法在重新分配商前释放未入根的余数。解析、格式组合以及真除溢出避免让可捕获的异常跨越仍持有裸缓冲的 C 帧。

## 有意差异与尚未完成

- 混合 `big + float` 等提升到 big，float 向零截断；静态含 big 的 `/` 结果为 float，但先将同表达式中的 float 操作数截成 big。负指数按 big 静态返回类型约束抛 ValueError（零底数仍为 ZeroDivisionError）；正指数及可分配 limb 有运行时上限。定宽左值配静态 big 移位计数会诊断，先显式改左域。
- 格式化首版支持整数说明符（d/b/o/x/X/n）、符号/宽度/分组，未覆盖完整 CPython mini-language（如浮点显示型和带分组的零填充组合）。`cast_big(str)` 是 ASCII 十进制入口，未接 Unicode 数字及 Python 的十进制转换位数限制。
- 静态可知的 big 键、big 容器比较/排序和 big 的 sum/min/max/divmod、三参数 pow、双参数 round 明确拒绝相关未接路径。Big 可作为容器值储存、传递、索引；这不是一般容器值语义已落地。CPython bridge/JSON/原生模块的整型 API、f32 未列入本批。
- 纯动态域（把 big 擦成 `object` 之后）已按「全图模式」补齐：见下节。仍未接的只有 `int(v)`（unknown 返回 i64 截断，不等于静态 `int(big)`）、混合「静态 float + unknown」的旧浮点快路径（不做 big 提升）、以及纯动态真除（仍走 float 路径而非 exact big 除法）。

## 动态域隔离（needs_dynamic_big）

问题：`big` 一旦擦成 `object`，运算落到旧的纯 `unknown` 发射路径，那里把机器字当整数——BigInt 指针会被当成数值参与 `& | ^ ~ ** << >>`，动态 dict/set 键也按指针比较，等值异对象查不到。

做法：**按编译图启用的模式，而不是改旧路径**。

- `Parser` 在构造 AST 时记录语法特征 `mentions_big`（注解位置出现 `big`、`cast_big` 调用、超 u64 十进制字面量；f-string 子解析器会回传）。不认普通标识符，避免用户拿 `big` 当变量名就误触发。
- `load_program` 在所有依赖解析完（含命中缓存的模块）之后，对**全图**取或，得到 `Program::needs_dynamic_big`。单模块判断不够：big 常常只在辅助模块里出现。
- 该标志写进模块缓存清单 `dynamicbig`，模式不同即失效重建（实测：先以非 big 程序缓存 `plain`，再以 big 程序构建会重新编译而不是复用）。
- 模式开时：`unknown` 域新增 `AnyBigBinary`（`& | ^ ** << >>`）与 `AnyInvert`（`~`）两个运行时入口，任一侧命中 BIG 标签就走 `pa_big_*`；否则**逐条复刻旧机器字语义**（含 `1 << 70 == 64` 的 64 取模与 `2 ** -1 == 0`）。
- 模式开时：动态 dict/set 键用新的 `key_kind = 9`（`DICT_KEY_RAW_OR_BIG`）：探针认出 BigInt 才按值哈希/等值，其它值与旧 `kind 3` 完全一致。静态可判定的 big 键仍然编译期拒绝。
- 模式开时强制保留堆注册表（`needs_unknown_index`）：探针靠它确认「这个机器字确实是堆对象」，索引一关等值 BigInt 会被当成两个不同指针。
- 模式关（程序里没有 big）时以上分支全不触发，生成 asm 与 0.356 逐字节相同。

回归：[400](../tests/run/400_big_dynamic_ops.py)、[401](../tests/run/401_big_dynamic_keys.py)、[66_big_dynamic](../tests/modules/66_big_dynamic/main.py)（跨模块 + 缓存）。

## 实测闸门

稳定候选：本树 `build/pyadamas`、`build/rt.o`、`build/rt_asan.o`。以下命令在 `/tmp/wt-big2` 运行；全套顺序执行，避免固定端口的网络用例相互干扰。

| 检查 | 实测 |
|---|---|
| `cmake --build build -j 8` | 通过 |
| `PYADAMAS_TEST_JOBS=8 bash tests/run_tests.sh build/gate-big-final-normal build/pyadamas` | exit 0；370 个 `ran ok`，含跨模块缓存再运行 |
| `PYADAMAS_TEST_JOBS=1 bash tools/difftest.sh build/pyadamas` | exit 0；350/0 |
| `PYADAMAS_TEST_JOBS=1 PYADAMAS_TEST_ASAN=1 bash tests/run_tests.sh build/gate-big-final-asan build/pyadamas` | exit 0；370 个 `ran ok`（套件默认关闭 LSan，下行独立覆盖） |
| `bash tests/check_size.sh`；`git diff --check` | exit 0；0 文件超限，函数长度仍有非阻塞警告 |
| ASan+LSan：逐个 `build --asan --rebuild -O2`，`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` | 13/174/88/192/92/93/94/119/153 和 395–398，13/13 退出 0、stderr 空 |
| [395](../tests/run/395_big_arithmetic.py)、[396](../tests/run/396_big_builtins.py)、[397](../tests/run/397_big_unknown.py)、[跨模块](../tests/modules/65_big_default/main.py) | CPython 3.14.4，注入 `big=int`；O0/O2 × TYPE_TAGS=0/1，共 14 个 stdout 逐字节相同（397 仅 tags=1；探针版另有 smoke）。[398](../tests/run/398_big_pa_boundaries.py) 单独验证有意差异 |
| 生成代码 GC 压力（本树 build/gate-big-gc-force.py） | 65,000 轮跨 big/unknown 槽和列表留活，带捕获异常；通过 cimport 静态调用 pa_gc_collect，`TYPE_TAGS` × `GC_INCR_MARK` 四组 ASan+LSan 与 CPython stdout 相同。各组 RC 统计 4 minor / 9 major，增量开组有 major incr-mark 启动日志；这不证明所有 GC 写屏障组合 |
| [运行时 fuzz](../tools/bigfuzz/big_fuzz.py) | 98569 normal + 36959 ASan/LSan、`FUZZ_GC=1`，CPython 零差异；含新增亚正规与无穷比较 |
| 0.356 旧 asm | 同一源码路径下旧 `build/lead/pyadamas` 和候选 `asm -O2`，stdout/stderr 均 38/38 逐字节相同：`bench/realistic/*.py`（6）、`bench/compare/*.py` 排除 `run_compare.py`（9）、`bench/daily/[abcdk]*.py`（23） |
| 日常语料 CPython | 同上的 23 个 daily 文件全部 stdout 逐字节同且退出 0；参数沿用 [run_daily.py](../bench/daily/_tools/run_daily.py) |
| 0.356 旧运行语料（主代理独立复验） | 主树 `tests/run/*.py` 排除 basename `f*` 和 `180_codepoint_perf.py`；按文件头启用 type-tags/lang-gaps，`-O2 --rebuild`。双方各 346/346 编译和运行成功，stdout/stderr/exit 一致；新四案未纳入旧对照 |
| 旧运行时性能抽样 | 38 份同址 asm 中取 fib、queue_sim、str_build，分别链接只读 0.356 rt.o 与候选；CPU 24，交替 A/B，9 个有效样本、每样本 3 次。中位变化 −0.19%、+0.02%、约 0.00%，无可辨变化；非全基准 |
| 0.271 oracle | **本轮未与 0.271 二进制对比**；0.356 不是 0.271。0.271 的固定模块缓存指向受保护主树，未运行可能写入该缓存的编译，也未重建 oracle |

首轮并行尝试曾遇固定端口的 277 差分失败和旧 f71 诊断文本不符；修复诊断、套件与差分隔离后，最终以上述三项 exit 0 为准。

### 定向跟进：带符号字面量与未知移位计数

标注/形参/返回值上下文可只给一元表达式而非其操作数定型为 big；现在先加宽，再调用 BigInt 一元实现。big 左操作数与静态 unknown 移位计数明确拒绝（含复合赋值），可先显式转换计数。这不改变纯动态运算路径。

[399](../tests/run/399_big_signed_literals.py) 与 [移位拒绝](../tests/fail/big_unknown_shift_count.py) 定向通过；399 的 O0/O2 × TYPE_TAGS=0/1 ASan+LSan 四组退出 0、stderr 空。普通 int/i8 正负号用例与 [332](../tests/run/332_fixed_int_basic.py) 的 O0/O2 asm 对 c560614 逐字节不变。此跟进未重跑上表全套。

### 动态域隔离的实测

在动态模式落地后重跑了上表全部闸门（下表为最终数字，替换前面的中间值）：

| 检查 | 实测 |
|---|---|
| `bash tests/run_tests.sh`，normal `JOBS=8`、ASan `JOBS=1` | 各 **371** 个 `ran ok`，均 exit 0（新增 400/401 与 66_big_dynamic） |
| `PYADAMAS_TEST_JOBS=8 bash tools/difftest.sh build/pyadamas` | **351/0**，exit 0 |
| `bash tests/check_size.sh`、`git diff --check` | exit 0，0 文件超限 |
| ASan+LSan 钉子 13/174/88/92/93/94/119/153/192 与 395–401 | 14/14 退出 0、stderr 空 |
| 全语料 asm（0.356 主树 464 个 `.py`，按文件头开关，`asm -O2`） | 0.356 `build/lead/pyadamas` 与候选**逐字节全同** |
| 动态样本 `dyn_ops`/`dyn_keys`/跨模块 `xmod`/`idxtest` | O0/O2 × TYPE_TAGS=0/1 × ASan，**24/24** stdout 逐字节同 CPython |
| 模块缓存模式失效 | 非 big 构建缓存 `plain` 后，big 程序重新编译 `plain`（`note: building`）而非复用；同模式仍 `note: cached` |
| 独立语料（DeepSeek 另一模型自写，非本仓测试） | 6 个正向入口 36/36 全绿；4 个负面探针中 `a: big = -123` 与 `big << object` 两个真实缺陷已修，其余静态拒绝与格式差异记录在案 |
