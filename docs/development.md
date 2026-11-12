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
cmake --preset dev                 # Debug + Ninja → build/dev/py23
cmake --build --preset dev
cmake --preset release             # Release → build/release/py23
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

当前规模：正例 11 / 反例 37 / 运行 88 / 未建模 1 / 模块 9。

跑单个文件：

```bash
./build/dev/py23 check tests/09_types.py
./build/dev/py23 check tests/fail/f17_len.py     # 期望报错
./build/dev/py23 run   tests/run/06_operators.py
```

生成代码优化等级用 `-O0` / `-O1` / `-O2`（默认 `-O2`），和编译器自身的
dev/release preset 无关。`ctest` 里的 `py23_opt_levels` 会用三档各跑一遍
容器/推导式/免检查/序言回归。

运行用例的期望输出这么写：

```python
print("hi")

# --- expect ---
# hi
```

`# --- expect ---` 之后每一行去掉 `# ` 就是期望输出，必须逐字节一致（包括空行）。

### 差分测试

```bash
# 自举：runner 自己用 py23 执行
./build/dev/py23 build -o /tmp/run_diff.prog tests/diff/run_diff.py
/tmp/run_diff.prog --py23 build/dev/py23 --cpython python3 --seeds 3

# 同一份脚本用 CPython 执行也行（随机序列一致，生成的用例相同）
python3 tests/diff/run_diff.py --seeds 20   # 更多随机性
python3 tests/diff/run_diff.py --only repr  # 只测浮点转字符串
```

`py23 run prog.py -- a b` 会把 `--` 之后的参数原样传给被运行的程序。

生成器在 `tests/diff/generators.py`，加新生成器就三步：写 `gen(rng, n)` 返回源码、在 `GENERATOR_NAMES` / `default_cases` 与 `run_diff.py` 的分派里登记、确认默认种子下零差异。生成器本身也必须限制在 py23 子集内（不 import 第三方模块，`random.Random` 参数在函数体里用 `r: random.Random = rng` 收窄类型）。

生成的程序必须两端都能跑通、输出一致；已知的语义分歧不要测（列在 [compatibility.md](compatibility.md)），否则只是制造噪音。

### 性能 bench

`bench/` 下是逐个内置函数的性能对比：同一份源码分别喂给 py23（编译成原生
二进制）和 CPython 3.13，比墙钟时间。

```bash
cd /w && python3 bench/run_bench.py                   # 全量
cd /w && python3 bench/run_bench.py 13_sum 32_sorted  # 只跑指定用例
cd /w && python3 bench/run_bench.py --md /w/bench/RESULTS.md --json /w/bench/report.json
```

用例在 `bench/builtins/<序号>_<内置名>.py`，第一行是 `N = <整数>`，驱动脚本
按 CPython 的实测时间把它校准到 0.35 秒左右。三条规则让倍数有意义：

- **参数必须随循环变化**（`S[i % 64]` 或直接用循环变量）。写成 `len(S)` 的
  话，py23 的 LICM 会把整个调用提到循环外，测到的是优化器不是内置函数。
- **容器类内置先改写再消费**（`T[i % 64] = i` 之后再 `sum(T)`），否则整条
  调用同样会被提出循环。
- **结果要打印出来**（末尾 `print(total)`），避免整段被当成死代码删掉。

驱动还会用 `2N` 再跑一次 py23：耗时没有跟着翻倍，就说明循环体被优化掉了，
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
~ `tests/run/56_re.py` 覆盖生成器、协程、Task、异步语言面、
Future/取消/fd/回调、`yield from`、异步推导式、异步生成器协议、
异常值、内置函数、f-string、del/print(*args)/复数、match 与多继承；
`53_json.py` / `54_stdlib.py` / `55_slot_types.py` / `56_re.py` 覆盖 json、
string/time/os/itertools/collections、call.rt 槽位类型、循环条件更新、
fsum 抵消和 re。

敏捷开发时只跑相关用例，不要每次全量：

```bash
PY23_TEST_FILTER='5[0-6]*' PY23_TEST_JOBS=16 bash tests/run_tests.sh build/dev build/dev/py23
python3 tests/diff/run_diff.py --only async --seeds 1 --jobs 16
```

提交前再跑全量；`run_tests.sh` 和 `run_diff.py` 都默认并行（`PY23_TEST_JOBS` /
`--jobs` 可调）。`tests/modules/`
覆盖 import 图、`from` 快照、惰性初始化、外部全局读写、GC 根和缓存
失效。RSS 压力用受限地址空间跑长循环（命令见
[compatibility.md](compatibility.md#内存验收)）。
改 `lower*.cpp` 的 RC/闭包/容器/异常插桩、模块 ABI 或 `runtime/rt_*.c` 的 GC/哈希表后至少
跑一遍：

```bash
./build/dev/py23 build stress.py -o stress
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
| token 流 | `py23 tokens x.py` |
| 语法树 | `py23 ast x.py` |
| 推断出的顶层类型 | `py23 types x.py` |
| 降级后的 IR | `py23 ir x.py` |
| 生成的汇编 | `py23 asm x.py` |
| 保留中间 `.s` | `PY23_KEEP_ASM=1 py23 run x.py` |
| 单步调试生成程序 | `py23 build x.py -o x && gdb ./x` |

汇编里每条指令前都有对应的 IR 注释，对照 `py23 ir` 的输出能定位到是哪条 IR 生成错了。

### 内存 / UB 检查

```bash
g++ -std=c++23 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
    -DPY23_RT_OBJ="\"$PWD/build/dev/rt.o\"" src/*.cpp -o /tmp/py23-asan

/tmp/py23-asan check tests/09_types.py
/tmp/py23-asan run   tests/run/04_containers.py
/tmp/py23-asan run   tests/run/09_class.py   # 类：字段偏移 / vtable / 对象分配
```

注意 ASan 下每层递归帧约 4KB，8MB 栈撑不住几千层。所以语法层对表达式嵌套设了硬上限：`parser.h` 的 `kMaxExprDepth` / `kMaxUnaryDepth`（目前 200），超了报 `expression nested too deeply`。这类保护必须在解析器里，因为类型检查和降级都是照 AST 递归的，树有多深它们就有多深。

上面的命令只给**编译器**插桩，生成代码和 `rt.o` 不受检查。要验证引用计数
路径，运行时的各个部分也要用同样的 sanitizer 编译再合成 `rt.o`，并在
`PATH` 前面放一个给链接加 `-fsanitize=address,undefined` 的 `cc` 包装脚本：

```bash
mkdir -p /tmp/rt_asan
for f in runtime/rt_*.c; do
    cc -O1 -g -fsanitize=address,undefined -Iruntime -c "$f" \
       -o "/tmp/rt_asan/$(basename "$f" .c).o"
done
ld -r /tmp/rt_asan/*.o -o /tmp/rt_asan.o
g++ -std=c++23 -O1 -g -fsanitize=address,undefined -Isrc \
    -DPY23_RT_OBJ='"/tmp/rt_asan.o"' src/*.cpp -o /tmp/py23-asan
mkdir -p /tmp/asancc
printf '#!/bin/sh\nexec /usr/bin/cc -fsanitize=address,undefined "$@"\n' > /tmp/asancc/cc
chmod +x /tmp/asancc/cc
PATH=/tmp/asancc:$PATH ASAN_OPTIONS=detect_leaks=1 \
    bash -c 'for t in 10_rc 11_closure 12_lambda 13_gc 14_dict 15_slice \
        16_dict_del 17_slice_assign 18_starargs 19_comprehension 20_with 21_decorator \
        22_exception; do \
        /tmp/py23-asan run tests/run/$t.py || exit 1; done'
```

### 严格警告

```bash
g++ -std=c++23 -O0 -g -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual \
    -Isrc -DPY23_RT_OBJ="\"$PWD/build/dev/rt.o\"" src/*.cpp -o /tmp/py23-strict
```

直接用上面的 g++ 命令；不要用 `cmake -DCMAKE_CXX_FLAGS=...` 覆盖，`PY23_RT_OBJ` 由 CMake 生成，手改 flags 会丢。

当前零警告。新代码请保持。

## 代码风格

没有单独的格式文件，跟着周围的代码写。几条硬规矩：

- **注释说「为什么」，不说「是什么」**。`// 取模结果与除数同号，补一个 |b|` 有价值；`// 调用 fmod` 没有。
- **错误消息格式统一**：`py23 xxx.py:12:5: error: ...`，由 `report()` 负责排版，不要自己 `printf`。
- **拒绝的东西要给出下一步**：`'*args' is not supported by the code generator yet` 比 `invalid syntax` 有用。
- **枚举 switch 不留 `default`**：让 `-Wswitch` 在加新枚举项时报错，比运行时漏一个 `case` 强。
- **死代码立刻删**：`IrOp::Print` 曾经是一个「谁发出来就会被静默丢掉」的空 `case`，这种比没实现更危险。

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
