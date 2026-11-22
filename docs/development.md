# 开发

## 环境

| 依赖 | 版本 | 用途 |
|---|---|---|
| g++ | 13+ | `<format>`、`<expected>`、`std::unreachable` |
| CMake + Ninja | CMake 3.28+ | 构建与测试 |
| cc | 任意（gcc/clang） | 把生成的汇编汇编成可执行文件，链接运行时 |
| python3 | 3.10+ | 只用于差分测试，编译本身不需要 |

`std::print` 要 GCC 14 才有，所以输出统一走 `std::format` + `printf`。

## 构建

```bash
cmake --preset dev                 # Debug + Ninja → build/dev/pyadamas
cmake --build --preset dev
cmake --preset release             # Release → build/release/PyAdamas
cmake --build --preset release
ctest --preset dev                 # 正例/反例/运行/未建模/模块
cmake --build build/dev --target clean-modcache
rm -rf build/dev                   # 连 CMake 中间产物一起清
```

`build/dev/compile_commands.json` 由预设生成，clangd/IDE 直接可用。

### 为什么默认 Debug

这个项目自己的运行时间只有十几毫秒，`-O0` 完全够用；`-O2` 下标准库模板的优化阶段会吃掉八成编译时间。只有跑分时才用 release preset。

### 增量原理

源码按翻译单元拆成多个 `.cpp`，Ninja 根据编译器的 depfile 跟踪头文件，改哪个 `.cpp` 只重编哪个 `.o`，改 `.h` 只重编真正依赖它的单元。dev/release 是两个独立 build 目录，`-O` 级别不会混。

| 场景 | 耗时 |
|---|---|
| 无改动 | 0.00s |
| 改 `codegen*.cpp` | 约 2.7s |
| 改 `main.cpp` | 约 2.4s |
| 改 `parser.cpp` | 约 3.4s |
| 改 `typecheck.h`（触发多个单元） | 约 3.7s |
| 全量 `cmake --build --preset dev` | 约 4.7s |

拆 TU 只对「只改一个文件」有效：预处理后一共 10 万行，项目自身只有 1 万行，95% 是标准库模板展开，全量构建只能靠并行。PCH 试过（84MB，只省 0.34s，且 `-O` 级别不匹配会静默失效），不划算。

## 测试

### 四类用例

多模块用例放在 `tests/modules/<名字>/main.py`（旁边放被导入的 `.py`）：
首轮 `--rebuild` 编译并比对 `# --- expect ---`，第二轮不带 `--rebuild`
检查 stderr 出现 `cached module`，同时验证缓存命中。`ctest --preset dev` 会跑
`tests/modules/*/`。

| 目录 | 要求 | 命名 |
|---|---|---|
| `tests/*.py` | `check` 必须通过 | 编号 + 主题，如 `09_types.py` |
| `tests/fail/*.py` | `check` 必须报错，且第一行写 `# expect: <错误片段>` | `fNN_主题.py` |
| `tests/run/*.py` | 必须编译运行成功，输出与末尾 `# --- expect ---` 逐行一致 | `NN_主题.py` |
| `tests/unsupported/*.py` | `check -v` 必须打标记，`build` 必须拒绝 | `主题.py` |

当前规模：正例 12 / 反例 91 / 运行 377 / 未建模 1 / 严格 7 / 模块 18。`ctest` 另含 `pyadamas_cache_race`：两进程并发 `--rebuild` 编译 hashlib/hmac，守模块缓存原子写；`cfi:unwind`（`tests/cfi/`）用 libgcc 的 `_Unwind_Backtrace` 数帧，守生成代码的 CFI。

跑单个文件：

```bash
./build/dev/pyadamas check tests/09_types.py
./build/dev/pyadamas check tests/fail/f17_len.py     # 期望报错
./build/dev/pyadamas run   tests/run/06_operators.py
```

生成代码优化等级用 `-O0` / `-O1` / `-O2`（默认 `-O2`），和编译器自身的
dev/release preset 无关。`ctest` 里的 `pa_opt_levels` 会用三档各跑一遍
容器/推导式/免检查/序言回归。

运行用例的期望输出这么写：

```python
print("hi")

# --- expect ---
# hi
```

`# --- expect ---` 之后每一行去掉 `# ` 就是期望输出，必须逐字节一致（包括空行）。

### 用例合并政策

同主题的小用例合并成一个文件（0.381 起）：把各文件正文按顺序拼起来，
`# --- expect ---` 块也按同样顺序拼接，每个原文件的位置留一行
`# ── 原 xxx.py ──` 注释便于追溯。合并前**必须验证输出逐字节等于各原文件
输出之和**，否则宁可不合并。

三条禁令（都是踩过的）：

1. **不要用函数包裹各段**（`def _case_N(): ...` 再调用）。静态类型是模块级
   中心的，包进函数会改变推断：
   - 嵌套 `def` 的未标注参数无法从调用点推断（`ch.send` 直接报不支持）；
   - `cimport` 只允许在模块层（包进函数报 "only allowed at module level"）；
   - 部分对象的方法解析依赖静态类型（`Decimal.as_tuple`、`tarfile.X` 会变成 unknown）。
2. **注意同名变量的类型冲突**：`x = 1` 一段、`x = "s"` 一段拼在一起会撞上
   「未标注变量不能换类型」（0.375 的限制）。同名类不同布局也一样
   （`cannot assign 'Meter' to 'Meter' variable`）。
3. **依赖静态路径的调用**（`open(d + "/x")`）在拼接后可能失去常量性，
   报 "must be a statically known path"。

能合的都合了；剩下的大文件（vthread、decimal、tarfile、codepoint、statistics）
就是被上面三条卡住的，别再试包裹式合并。

### 差分测试

```bash
# 自举：runner 自己用 PyAdamas 执行
./build/dev/pyadamas build -o /tmp/run_diff.prog tests/diff/run_diff.py
/tmp/run_diff.prog --pyadamas build/dev/pyadamas --cpython python3 --seeds 3

# 同一份脚本用 CPython 执行也行（随机序列一致，生成的用例相同）
python3 tests/diff/run_diff.py --seeds 20   # 更多随机性
python3 tests/diff/run_diff.py --only repr  # 只测浮点转字符串
```

`pyadamas run prog.py -- a b` 会把 `--` 之后的参数原样传给被运行的程序。

生成器在 `tests/diff/generators.py`，加新生成器就三步：写 `gen(rng, n)` 返回源码、在 `GENERATOR_NAMES` / `default_cases` 与 `run_diff.py` 的分派里登记、确认默认种子下零差异。生成器本身也必须限制在 PyAdamas 子集内（不 import 第三方模块，`random.Random` 参数在函数体里用 `r: random.Random = rng` 收窄类型）。

生成的程序必须两端都能跑通、输出一致；已知的语义分歧不要测（列在 [compatibility.md](compatibility.md)），否则只是制造噪音。

`tests/run` 的 -O0/-O2 输出自差分由 `tools/difftest.sh` 负责：默认按 CPU 核数并行（`PYADAMAS_TEST_JOBS` 调并行度），`PYADAMAS_TEST_FILTER` 只跑匹配的用例，自动跳过 `*_perf`（打印计时的用例）；当前 204 个用例 / 0 不一致。

### 性能 bench

`bench/` 下是逐个内置函数的性能对比：同一份源码分别喂给 PyAdamas（编译成原生
二进制）和 CPython 3.13，比墙钟时间。标准库与 RC/GC 层面的对比在 `tests/bench/`
（`python3 tests/bench/run_bench.py`），两套基准口径不同、互不替代。RC 统计用
`PYADAMAS_RC_STATS=1 ./prog`，退出时打印 inc/inc_unknown/dec/dec_unknown/
temp/cleanup/free、GC 次数（`gc_runs` 以及 `gc_minor`/`gc_major`）与
`gc_freed`/`gc_live`。

```bash
cd /w && python3 bench/run_bench.py                   # 全量
cd /w && python3 bench/run_bench.py 13_sum 32_sorted  # 只跑指定用例
cd /w && python3 bench/run_bench.py --md /w/bench/RESULTS.md --json /w/bench/report.json
```

单版本回归计时用 `tools/quickbench.sh`：对 `bench/realistic/` 下的用例重复 `REP=` 次取最好一次（默认 5 次，默认用例 `queue_sim` / `str_build`），`PYADAMAS_BIN=` 换构建目录（默认 `build/release/pyadamas`），`OUT=` 指定产物目录；参数可以换成 `bench/realistic` 里的其他用例名。两个版本对比用 `tools/abbench.sh`（独立 worktree + 交替测量 + 中位数）。

用例在 `bench/builtins/<序号>_<内置名>.py`，第一行是 `N = <整数>`，驱动脚本
按 CPython 的实测时间把它校准到 0.35 秒左右。三条规则让倍数有意义：

- **参数必须随循环变化**（`S[i % 64]` 或直接用循环变量）。写成 `len(S)` 的
  话，PyAdamas 的 LICM 会把整个调用提到循环外，测到的是优化器不是内置函数。
- **容器类内置先改写再消费**（`T[i % 64] = i` 之后再 `sum(T)`），否则整条
  调用同样会被提出循环。
- **结果要打印出来**（末尾 `print(total)`），避免整段被当成死代码删掉。

驱动还会用 `2N` 再跑一次 PyAdamas：耗时没有跟着翻倍，就说明循环体被优化掉了，
表格里的「线性」列就是这个检查；此时倍数只能当「优化器收益」看，不能当
「内置函数成本」看。跑完顺带比对两边的标准输出（行数 + 最后一行），确认
跑的是同一件事。

结果是机器相关的快照，别当基准线看：最新一次见 [bench/RESULTS.md](../bench/RESULTS.md)，
结论写在 [compatibility.md](compatibility.md#内置函数逐个对比)。

### 内存验收

`tests/run/10_rc.py` 覆盖别名、字段、列表、返回值和 `continue` 跳过清理；
`tests/run/11_closure.py` 覆盖闭包捕获、`nonlocal`、多层嵌套、自递归；
`tests/run/12_lambda.py` 覆盖 lambda；`tests/run/13_gc.py` 会真正触发
阈值后的标记-清扫（对象环、字典自环、闭包垃圾、unknown 容器）；
`tests/run/14_dict.py` 覆盖字典/集合/`in`/方法；`tests/run/15_slice.py`
覆盖切片（缺省、负边界、正负步长）；`tests/run/16_dict_del.py` 覆盖删除类
方法；`tests/run/17_slice_assign.py` 覆盖切片赋值；`tests/run/18_starargs.py`
覆盖 `*args` / `**kwargs` / 方法 / 闭包 / 转发；`tests/run/19_comprehension.py`
覆盖推导式；`tests/run/20_with.py` 覆盖 `with` 的提前退出；`tests/run/21_decorator.py`
覆盖装饰器；`tests/run/22_exception.py` 覆盖异常层级、bare raise、
finally 的提前退出、with 异常路径和运行时错误转换；`tests/run/23_generator.py`
~ `tests/run/149_rc_move.py` 覆盖生成器、协程、Task、异步语言面、
Future/取消/fd/回调、`yield from`、异步推导式、异步生成器协议、
异常值、内置函数、f-string、del/print(*args)/复数、match 与多继承；
`53_json.py` / `54_stdlib.py` / `55_slot_types.py` / `56_re.py` 覆盖 json、
string/time/os/itertools/collections、call.rt 槽位类型、循环条件更新、
fsum 抵消和 re；`98`–`101` 覆盖 C 互操作/缓冲区/BLAS/不透明结构体，
`130`–`155` 覆盖挂起点边角、类对象与类装饰器、标准库批次（heapq/bisect/
binascii/base64/statistics/textwrap、itertools 组合函数、operator、deque、
timedelta、pathlib、序列重复、RC 所有权转移、裸 list 元素定型、分代 GC、
unknown 值的 `str()` 分派）。

敏捷开发时只跑相关用例，不要每次全量：

```bash
PYADAMAS_TEST_FILTER='5[0-6]*' PYADAMAS_TEST_JOBS=16 bash tests/run_tests.sh build/dev build/dev/pyadamas
python3 tests/diff/run_diff.py --only async --seeds 1 --jobs 16
```

提交前再跑全量；`run_tests.sh` 和 `run_diff.py` 都默认并行（`PYADAMAS_TEST_JOBS` /
`--jobs` 可调）。`tests/modules/`
覆盖 import 图、`from` 快照、惰性初始化、外部全局读写、GC 根和缓存
失效。RSS 压力用受限地址空间跑长循环（命令见
[compatibility.md](compatibility.md#内存验收)）。
改 `lower*.cpp` 的 RC/闭包/容器/异常插桩、模块 ABI 或 `runtime/rt_*.c` 的 GC/哈希表后至少
跑一遍：

```bash
./build/dev/pyadamas build stress.py -o stress
python3 -c 'import resource,subprocess
def lim(): resource.setrlimit(resource.RLIMIT_AS,(16<<20,16<<20))
p=subprocess.run(["./stress"],preexec_fn=lim)
print(p.returncode, resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)'   # KB
```

峰值 RSS 应稳定在 8MB 左右，不随循环次数增长。

### 前端基线

改了类型检查之后，除了 `ctest --preset dev`，还应该重跑一次标准库基线，确认没有把真实代码判错。命令和数字见 [compatibility.md](compatibility.md#前端鲁棒性基线)。

## 调试

| 想看什么 | 命令 |
|---|---|
| token 流 | `pyadamas tokens x.py` |
| 语法树 | `pyadamas ast x.py` |
| 推断出的顶层类型 | `pyadamas types x.py` |
| 降级后的 IR | `pyadamas ir x.py` |
| 生成的汇编 | `pyadamas asm x.py` |
| 保留中间 `.s` | `PYADAMAS_KEEP_ASM=1 pyadamas run x.py` |
| 单步调试生成程序 | `pyadamas build x.py -o x && gdb ./x` |
| 地址 → 源码行 | `addr2line -e x -f <addr>`（`build` 默认带 DWARF 行号） |
| 查看 DWARF 行号表 | `readelf --debug-dump=decodedline x` |
| 查看栈展开规则（CFI） | `readelf --debug-dump=frames x` |
| 回溯穿过生成帧和 libc | `gdb -batch -ex 'set backtrace past-main on' -ex run -ex bt ./x` |
| 未捕获异常回溯 | `pyadamas run bad.py`（stderr 打印 `Traceback`） |
| IR 不变量校验 | `PYADAMAS_VERIFY_IR=1 pyadamas run x.py`（见 [ir-verify.md](ir-verify.md)） |
| 优化遍回归（零违规基线） | `PYADAMAS_TEST_VERIFY=1 bash tests/run_tests.sh build/dev build/dev/pyadamas` |
汇编里每条指令前都有对应的 IR 注释，对照 `pyadamas ir` 的输出能定位到是哪条 IR 生成错了。

### 内存 / UB 检查

RC/GC 这一类「堆被写坏」的 bug，**在 IR 层面完全看不出问题** —— 生成代码的
inc/dec 是配平的、`-O0` 也照样崩、关掉 GC 就好了。只有内存检查器能抓到
第一现场：谁第一个读了已释放的内存。

0.216 的 unknown 槽提升 bug 就是这样定位的：ASan 报 `SEGV on address
0xffffffffffffffe6`（= -26）在 `pa_rc_temp`，顺着调用栈才发现 `f()` 返回
了一个从已释放列表里取出的元素。此前在帧记录、RC 配平上盲试了十几轮都
没结果。

**遇到 RC/GC/异常展开相关的「诡异」问题时，第一步就该跑这个。**

### 运行时诊断工具（rt_diag.c）

RC/GC 的 bug 有一个共同点：**ASan 能告诉你漏了多少字节，但说不出那个对象
是怎么被漏掉的**。异常对象、临时值、帧槽之间的所有权转移看不见，于是只能
猜 —— 0.226 之前我在同一个 126 字节泄漏上盲试三轮，每次都引入新的内存错误。

这个工具把「对象在哪一步丢掉」变成可以读出来的事实。

#### 开关

| 环境变量 | 作用 |
|---|---|
| `PYADAMAS_RC_TRACE=1` | 追踪所有 RC 对象的分配/释放 |
| `PYADAMAS_RC_TRACE_MIN=N` | 只看分配字节数 >= N 的对象 |
| `PYADAMAS_RC_TRACE_KIND=K` | 只看某类：`str` / `list` / `dict` / `obj` / `struct` |
| `PYADAMAS_EXC_TRACE=1` | 只追踪异常对象（含所有权转移事件） |
| `PYADAMAS_DIAG_DEPTH=N` | 调用链深度（默认 3，最大 8） |

不设任何变量时：一次 `getenv` 判断 + 一次自增，**零输出、零可测开销**。

#### 用法

```bash
./build/dev/pyadamas build -O2 -o /tmp/prog tests/run/53_json.py
PYADAMAS_RC_TRACE=1 PYADAMAS_RC_TRACE_KIND=str \
PYADAMAS_RC_TRACE_MIN=25 PYADAMAS_DIAG_DEPTH=4 /tmp/prog
```

输出（每个对象一行分配、一行释放，退出时汇总未释放的）：

```
[rc#20] alloc  str  32B rc=1  <- str_alloc_raw <- str_new <- pa_json_loads <- __pa_main (53_json.py:20)
[rc#20] free   str  32B rc=0  <- pa_rc_cleanup <- __pa_main (53_json.py:20)
[diag] 未释放 RC 对象 1 个，共 32 字节：
  [rc#20] str 32B at 0x...
        分配于 str_alloc_raw <- str_new <- pa_json_loads <- __pa_main (53_json.py:20)
```

最后那行直接给出了答案：**是 `53_json.py:20` 的 `json.loads` 产生的字符串
没人释放** —— 不用猜。

异常对象另有一套事件：`alloc` / `free` / `leave`（写回生成器帧槽，
所有权转移，不是释放）/ `enter`（从帧槽取回）：

```
[exc#5] alloc  KeyError "inner"  <- pa_raise (rt_exc.c:660)
[exc#5] leave  KeyError at 0x...e2a8      <- 写进帧槽 s1
[exc#5] enter  KeyError at 0x...e2a8      <- 取回
[exc#5] leave  KeyError at 0x...e2a8      <- 又写回去
[exc#5] LEAK   KeyError                   <- 再没人还
```

0.228 的修复就是靠这条轨迹定位的（写回槽后 status 短路跳过了释放）。

#### 设计要点

**外部哈希表，不改结构体布局。** 给 `PyAdamasException` 加一个追踪字段
试过：对象从 126 字节变成 142，ASan 报的数字跟着变，前后对照就失效了。
追踪信息全部放在 `rt_diag.c` 自己的表里。

**调用点归因走 `_Unwind_Backtrace`。** `__builtin_return_address(n>0)`
需要编译期常量、还会被 `-Werror=frame-address` 拦下。改用
`_Unwind_Backtrace`（和 `rt_debug.c` 的 traceback 同一套机制），
按 `.eh_frame` 逐帧还原，不依赖帧指针，并且能用生成代码的 PC 表拿到
**文件名和行号**。链接时加 `-rdynamic`，运行时符号也能解析。

**表满不静默丢弃。** 溢出时置标志，退出汇总明确打印「结果不完整」。
诊断工具撒谎比不工作更糟。

**地址复用不算新对象。** 哈希表插入时，同地址的 DEAD 记录原地复用（否则
同一地址在表里出现多份，汇总把历史记录当泄漏 —— 实测误报 14 个对象）；
但绝不覆盖 LIVE 槽（那会让真实泄漏从报告里消失 —— 实测漏报）。

#### 一个必须记住的教训：不要在 `pa_rc_inc` / `pa_rc_dec` 里加钩子

最初把「悬挂指针检测」放在这两个函数的入口：

```c
void pa_rc_inc(void* p) {
    pa_diag_check_use(p, "inc");   // ← 这一行让 88_defaultdict 算错浮点
    ...
}
```

结果 `88_defaultdict` 的 `f["v"] += 1.5` 从 `1.5` 变成
`-1.6882786079646144e+260` —— 一个纯浮点值被污染了。

排查过程：先怀疑 `-rdynamic`（排除），再怀疑 `_Unwind_Backtrace`
破坏浮点寄存器（排除），最后逐个钩子二分，锁定到 `pa_diag_check_use`。
它只是**一个普通函数调用**，但在 `pa_rc_inc`/`pa_rc_dec` 这两个极热、
会被内联的函数里，多一次调用就改变了寄存器分配，而生成的代码对
`%xmm0` 的存活假设太脆弱，于是值被覆盖。

**结论**：诊断钩子只能放在非热路径（`rc_alloc` 入口、真正释放的那一支）。
想覆盖 `inc`/`dec`，只能用「先查一个全局标志、只在开启时才调函数」的
形式，而且这个标志判断本身必须能被内联成一条 load + test。

这个 bug 也顺带说明：**运行时对浮点寄存器生命期的依赖比想象中脆弱**，
任何在热路径上新增的调用都可能踩到。
#### 两个必须知道的陷阱

**① 地址复用会让事件归属错乱。**

追踪表按**地址**索引。malloc 会把刚释放的地址立刻复用给下一个对象，于是
旧对象的 `temp-push` / `free` 事件会被算到新对象头上。

实测 `53_json`：`rc#35` 报告「分配后没有任何事件」，但把地址拿去对照
`[T] push` 日志，发现那个地址**在更早的对象上 push 过三次** —— 全部
被错误归属。看起来像「没进临时栈」，实际是「地址被复用了」。

**判断方法**：怀疑某个对象时，不要只看 `[rc#N]` 行，要把它的**地址**拿去
grep 全部事件，看是否与其他对象的生命周期重叠。

**② `cmake --build` 的编译失败会被 `grep error` 吃掉。**

排查 `53_json` 时我在 `rt_json.c` 里加了日志，跑了几轮都「没有输出」，
一度怀疑是逻辑问题。实际是那行代码有 `-Werror=unused-variable` 编译错误，
**`rt_json.o` 一直是旧的**，日志根本没编进去。

`cmake --build build/dev -j4 2>&1 | grep -E 'error:'` 这种写法在 ninja
并行构建下会漏报。要么看 `tail`，要么看退出码：

```bash
cmake --build build/dev -j4 2>&1 | tail -5   # 而不是 grep error
```
#### 已知边界

* 只在 `rc_alloc` / `pa_rc_dec` / `gc_free_body` 三个点埋钩子，绕过它们的
  分配（裸 `malloc` 的异常对象、handler 记录等）不在 RC 追踪范围内 ——
  那些用 `PYADAMAS_EXC_TRACE` 单独追。
* 追踪表固定 65536 项，长跑程序会溢出（会提示）。
* 有编译开销：每个分配点多一次函数调用 + `getenv` 缓存判断。只在诊断时开。
#### 生成程序插桩（最常用）

`--asan` / `--ubsan` 让生成的可执行文件带检查器，编译和链接同时生效：

```bash
./build/dev/pyadamas build --asan -O2 -o /tmp/prog tests/run/10_rc.py
ASAN_OPTIONS=detect_leaks=0 /tmp/prog
```

`detect_leaks=0` 是有意的默认：GC 程序里「故意留到进程退出」的对象很多，
泄漏报告会淹没真正的 use-after-free。要查泄漏时显式打开：

```bash
ASAN_OPTIONS=detect_leaks=1 /tmp/prog
```

底层是 `PYADAMAS_CC_FLAGS` 环境变量 —— 想传别的编译选项也用它：

```bash
PYADAMAS_CC_FLAGS='-fsanitize=undefined -fno-omit-frame-pointer' \
    ./build/dev/pyadamas build -O2 -o /tmp/prog x.py
```

#### 整个测试套件跑 ASan

```bash
PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=4 \
    bash tests/run_tests.sh build/dev build/dev/pyadamas
```

比平时慢 3-5 倍，日常不用。**改动触及 RC / GC / lowering 时跑一遍** ——
全量 222 ran ok 说明内存层面没有回归。

#### 编译器自身插桩

上面两步只给**生成代码**插桩。怀疑编译器自己（C++ 侧）有内存问题时用
单独的构建目录：

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g' \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address'
cmake --build build/asan -j4
build/asan/pyadamas check tests/09_types.py
```

注意 ASan 下每层递归帧约 4KB，8MB 栈撑不住几千层。所以语法层对表达式嵌套
设了硬上限：`parser.h` 的 `kMaxExprDepth` / `kMaxUnaryDepth`（目前 200），
超了报 `expression nested too deeply`。这类保护必须在解析器里，因为类型检查
和降级都是照 AST 递归的，树有多深它们就有多深。

#### 历史上踩过的坑

早期给生成程序插桩要手工编 `rt.o`（逐个 `cc -c` 加 sanitizer 再 `ld -r` 合并）、
写一个给链接加 `-fsanitize` 的 `cc` 包装脚本、再把它放进 `PATH` 前面。现在
`PYADAMAS_CC_FLAGS` 一处搞定 —— 编译和链接共用同一套 flag，`rt.o` 也一起
重新编。
### 严格警告

```bash
g++ -std=c++23 -O0 -g -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual \
    -Isrc -DPYADAMAS_RT_OBJ="\"$PWD/build/dev/rt.o\"" src/*.cpp -o /tmp/PyAdamas-strict
```

直接用上面的 g++ 命令；不要用 `cmake -DCMAKE_CXX_FLAGS=...` 覆盖，`PYADAMAS_RT_OBJ` 由 CMake 生成，手改 flags 会丢。

当前零警告。新代码请保持。

## 代码风格

没有单独的格式文件，跟着周围的代码写。几条硬规矩：

- **注释说「为什么」，不说「是什么」**。`// 取模结果与除数同号，补一个 |b|` 有价值；`// 调用 fmod` 没有。
- **错误消息格式统一**：`PyAdamas xxx.py:12:5: error: ...`，由 `report()` 负责排版，不要自己 `printf`。
- **拒绝的东西要给出下一步**：`'*args' is not supported by the code generator yet` 比 `invalid syntax` 有用。
- **枚举 switch 不留 `default`**：让 `-Wswitch` 在加新枚举项时报错，比运行时漏一个 `case` 强。
- **死代码立刻删**：`IrOp::Print` 曾经是一个「谁发出来就会被静默丢掉」的空 `case`，这种比没实现更危险。

### 规模检查

```bash
bash tests/check_size.sh
```

规则：

- **源文件超过 1200 行 → 失败**。当前 `ALLOW_FILES` 为空，但仓库里仍有文件超限（`bash tests/check_size.sh` 目前会失败，超限清单见 [roadmap.md](roadmap.md) 的「工程收尾」）；清单只能删不能加，新增文件超限时必须拆分。
- **单个函数超过 300 行 → 只警告**。函数长度是拆分提示，不阻塞提交；
  改到某个函数时顺手拆一下即可。

新增 `.cpp` 时记得同时加进 `CMakeLists.txt` 的源文件列表（工程没有用
`file(GLOB)`）。

## 扩展 checklist

### 加一条语法

1. `token.h`：需要新记号就加枚举 + `tok_name`
2. `lexer.h`：产出它
3. `ast.h`：加节点，实现 `dump`
4. `parser.cpp`：解析它（注意 `parse_atom` / `parse_stmt` 的分派和优先级）
5. `typecheck*.cpp`：加类型规则；不确定就返回 `unknown` 放行
6. `lower*.cpp`：降级；做不到就 `err("... is not supported by the code generator yet")`
7. `tests/`：正例 + 反例各一个

### 加一条类型规则

`typecheck*.cpp` 里按语句/表达式分派。注意两条底线：

- 遇到 `unknown` 一律放行（`is_unknown(t)` 提前 return）
- 不确定的时候宁可漏报——把真实代码误判为错误，比漏掉一个错更糟

改完重跑基线。

### 加一个类特性

类的真相在 `ClassInfo`（`typecheck.h`）：字段顺序决定 `field.get` 的偏移，`MethodInfo::slot` 决定 vtable 下标。加特性时按这条链走：

1. `typecheck.h`：`ClassInfo` / `FieldInfo` / `MethodInfo` 加字段
2. `typecheck_func.cpp` 的 `classdef()`：填进去（注意先绑类名、再解析字段、最后检查方法体）
3. `lower.cpp` 的 `collect_classes()`（预扫描建 `IRClass`）与 `lower_classdef()`（填 vtable）
4. `codegen*.cpp`：只有对象布局变了才需要动（`field.get` 的偏移由 IR 携带）
5. `tests/run/09_class.py`：钉住新行为

两条不变量别破坏：**子类的字段前缀必须等于父类的全部字段**、**覆盖方法的槽号必须等于父类的槽号**。任何一条破了，「父类类型装子类实例」就会读错偏移或调到错的实现。

放宽的边界也要守住：`ClassInfo::open`（基类在外部、类体里有语句）和骨架（`complete == false`）只影响**成员查找**，不能让它影响布局。`n_fields()` / `obj_size()` 必须只数真实字段——把 open 当成「字段数不定」就会让所有对象分配小一圈，运行时踩内存。

新增的隐式字段（`self.x = ...`）在方法体检查过程中追加到 `fields`，顺序就是首次赋值顺序。这依赖一个前提：**类型检查阶段没有任何地方缓存 `n_fields()`**。目前只有 `lower.cpp` 的 `collect_classes()` 读它，那是类型检查全部结束之后。

### 加一条 IR 指令

见 [ir.md](ir.md#加一条新指令要动哪里)。要点：`IrOp` 枚举 → `irop_name` → `text()` → `lower` 产出 → `codegen` 消费（`-Wswitch` 会逼你补）。

### 加一个内置函数

1. `typecheck_builtin.cpp` 的 `builtin_call()`：加签名规则
2. `typecheck_call.cpp` 的 `is_builtin_callable()`：登记名字
3. `lower_builtin.cpp` 的 `lower_builtin_call()`：降级（可能展开成多条指令，也可能调运行时）
4. `tests/`：正例 + 反例

### 加一个运行时函数

见 [runtime.md](runtime.md#加一个运行时函数)。

## 踩过的坑（都写进了代码注释）

| 坑 | 教训 |
|---|---|
| vreg 槽位按线性区间复用 | 跨块的值必须独占槽位，否则循环第二轮读到垃圾 |
| 复合赋值 `*=` 被降成 `+=` | `Tok` 有两套记号，`STAREQUAL` 不等于 `STAR`，映射要写全 |
| f-string 里的表达式从没被检查 | `return t_str()` 之前得先把 `values` 走一遍 |
| `Constant::boolean/none` 没有行列号 | 静态工厂不设位置，诊断会指到 1:1 |
| `%g` 直接当浮点格式化 | 最短数字串的精度太小，`10.0` 会变成 `1e+01` |
| 字符串 `<` 什么指令都不生成 | 「留个注释明天再写」等于静默错误 |
| 预扫描占位的符号吃掉标注 | `x: int = 1` 之后的 `x = "s"` 没人管，等于标注失效 |
| 标注是 float 但存了整数位模式 | 赋值/传参/返回/字面量四处都要补 `convert`，否则打印出 `5e-324` |
