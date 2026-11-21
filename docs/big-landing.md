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
- **发布待决：unknown 擦除类型后仍有静默错误。** 纯动态的幂、位运算/移位、按旧路径降级的链式比较，以及绕过上述静态检查的动态 dict/set 键没有完整 big 分派；等值异对象键可 miss，未接的运算可能拿地址当整数。双标签均复现，不能作为支持路径（可先 `cast_big(v)` 再运算）。unknown 的 `int(v)` 当前返回 i64 截断，不等于静态 `int(big)`；混合静态 float + unknown 的旧浮点快路径也没有 big 提升，纯动态真除未升级到 exact big 的统一路径。不要把这些写成仅缺 API。

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
