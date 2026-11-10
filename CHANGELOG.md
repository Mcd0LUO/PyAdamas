# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.79.1] - 2026-11-10

内置函数的两个缺口。

### 新增

- `sorted(xs, reverse=True)`：`reverse` 接受字面量 `True`/`False`，排完整体反转。
- `format(value, spec)`：按值类型分派到 `FmtI64`/`FmtF64`/`FmtStr` 等，和
  f-string 的格式说明共用同一套实现。

### 测试

- `tests/run/64_builtins.py`：map / filter / sorted / any / all / divmod / pow /
  round / repr / format / min / max / sum / abs / zip / enumerate / hex / oct /
  bin / chr / ord / isinstance / callable / hash 一轮过。

## [0.79] - 2026-11-10

`bytes` 字面量与基本操作。

### 新增

- **`bytes` 字面量**：`b"..."` / `b'...'` 不再被解析器拒绝。运行时复用 `str`
  的表示（同样的 `Py23Str` 布局和 RC 头），类型层面用 `TyKind::Bytes` 区分。
- 索引 `b[i]` 返回该字节的整数值（0..255），切片返回 `bytes`；
  `len` / `+` / `in` / `==` / 大小比较都按字节工作。
- `str.encode()` 返回 `bytes`、`bytes.decode()` 返回 `str`——两者同表示，
  实现上只是改标类型，零成本。
- `bytes` 的 `repr`：`b'...'`，不可打印字节转 `\xNN`；内容含单引号且不含
  双引号时改用双引号（与 CPython 一致）。
- `bytes` 方法：`upper` / `lower` / `strip` / `lstrip` / `rstrip` / `replace`。

### 测试

- `tests/run/63_bytes.py`；删除过时的反例 `tests/fail/f23_bytes.py`。

### 已知限制

- `for c in b"..."` 还没接（迭代 bytes 要转成 `list[int]`）。
- `bytes` 还没有格式化、`%`、`join`、`find`、`startswith` 之外的大部分方法。
- `bytes` 跨模块接口降级成 `unknown`（`enc_type` 里归到 unknown 组）。

## [0.78] - 2026-11-10

`print` 对 unknown 值做运行时分派，`re.sub` 支持函数回调。

### 修复

- **unknown 值的 `print`**：`lower_print` 的兜底从 `PrintI64` 换成 `PrintAny`，
  运行时先查 GC 的地址索引（`rc_index_has`）判断是不是堆对象，命中且是 `str`
  就按字符串输出，否则按整数。查表只比对地址、不解引用，任意 `int64` 都安全。
  多态形参（`def f(v)` 同时吃 `str` 和 `int`）不再打印指针值。

### 新增

- `re.Pattern.sub` 的 `repl` 可以是函数：运行时每匹配一次回调一次闭包。
  复用 `rt_task.c` 里已有的 `py23_call1` 跳板（闭包指针走 `%r10`）；
  `typecheck` 在 `sub` 处把回调的第一个形参回填成 `re.Match`，回调里
  `m.group(0)` 才有静态类型。

### 测试

- `tests/run/62_re_subfn.py`。

### 已知限制

- `PrintAny` 只认 `str`；`unknown` 的 `float` 仍按整数打印（`int` 和 `float`
  在 `unknown` 里分不开）。
- 只支持 `Pattern.sub(func, s)`；模块级 `re.sub(pat, func, s)` 还没接。

## [0.77] - 2026-11-10

修 `print` 打印字符串参数的根因，`re` 的 Match 属性补齐。

### 修复

- **未标注形参的类型推断**：调用点现在用实参类型回填形参（`bind_param`），
  函数体在回填后按新签名重查（`FuncInfo::sig_dirty` + 固定点，最多 4 轮）。
  `def f(p): print(p)` 传字符串不再打印指针值。方法的 `call_sig` 是去掉
  `self` 的副本，回填会同步写回含 `self` 的 `sig`（`outer` / `outer_off`）。
- 同一形参被多种类型调用时退回 `unknown` 而不是报错（`Param::multi`），
  不把 Python 的多态用法误杀成类型错误。
- 嵌套函数（含 lambda 脱糖产物）不参与重查：它们的词法父作用域要重新挂接，
  收益不值这个复杂度。

### 新增

- `Match.groupdict()`。
- `Match.lastindex` / `Match.lastgroup` 属性。

### 已知限制

- 多态形参（同一函数用 `int` 和 `str` 各调一次）退化成 `unknown`，`print`
  仍按整数输出；根治需要运行时类型标签，代价见 roadmap 的待决策。
- `lastindex` 在没有参与匹配的组时返回 0、`lastgroup` 返回空串（CPython 是 None）。

### 测试

- `tests/run/60_param_types.py`：单态 / 多参数 / 关键字 / 跨函数 / 方法 / 多态。
- `tests/run/61_re_dict.py`：`groupdict` / `lastindex` / `lastgroup`。

## [0.76.1] - 2026-11-10

`re` 的 API 补齐（0.76 的语法层不动）。

### 新增

- `Pattern.groups` / `Pattern.pattern` / `Pattern.flags` 属性。
- `Match.expand(template)`。
- `Match.group(a, b, ...)` 多参数，返回元组。
- `re.finditer` / `Pattern.finditer`，返回 `list[Match]`：py23 没有惰性迭代器，
  按列表给，`for` 循环语义一致；副作用是 `len()` 能用（CPython 会 TypeError）。
- 替换模板支持 `\g<name>` / `\g<1>` / `\g<0>`，`sub` 和 `expand` 共用同一套展开。

### 修复

- `re.sub` 里的临时 Match 没带名字表，`\g<name>` 会静默输出原文。

### 测试

- `tests/run/59_re_api.py`，输出与 CPython 3.13 逐行一致。

## [0.76] - 2026-11-10

`re` v2：补上回溯引擎缺的那几块。

### 新增

- **后向引用**：`\1`..`\99` 与 `(?P=name)`；引用未参与匹配的组时整体失配，
  `IGNORECASE` 下按折叠比较。
- **四向断言**：`(?=...)`、`(?!...)`、`(?<=...)`、`(?<!...)`。变长 lookbehind
  也接受（CPython 只允许定长），用子模式最大宽度做上界枚举起点；断言失败
  时按快照回滚捕获组，成功时保留正向断言内部的捕获。
- **原子组与占有量词**：`(?>...)`、`*+`、`++`、`?+`、`{m,n}+`。原子组把子
  程序编译成一次不可回溯的调用，占有量词是它的语法糖。
- **内联标志**：`(?i)`、`(?im-sx)`、`(?i:...)`；全局形式与 CPython 一样限制
  在表达式开头。新增 `re.X` / `re.VERBOSE`，跳过空白与 `#` 注释。
- **锚点语义修正**：`\A`（串首）、`\Z` / `\z`（严格串尾）、`$`（无 `M` 时
  允许末尾换行之前，有 `M` 时按行尾）。v1 里 `\Z` 走的是 `$` 的路径，带
  `M` 标志时会错。
- **命名组**：`Match.group("name")`；重名组在编译期报错。
- **`findall` / `split` 的分组语义**：`findall` 在只有一个捕获组时返回组内容
  （多组仍返回整段匹配，CPython 是元组）；`split` 把捕获组按顺序插进结果。
- `(?#...)` 注释组；`\0` / `\0oo` 八进制转义；字符类内 `\1`..`\9` 按八进制。

### 修复

- 解析器里返回多节点子链的构造（`(?i:ab)` 这类作用域组）会被 `parse_seq`
  的尾指针覆盖掉后续节点，`(?i:ab)c` 实际只匹配 `ac`。

### 测试

- `tests/run/57_re2.py`：后向引用 / 命名组 / 断言 / 原子组 / 内联标志 /
  `\A\Z$` / VERBOSE / 替换模板，输出与 CPython 3.13 逐行一致。
- `tests/run/58_re_err.py`：7 条非法模式在运行时抛 `ValueError`。

### 已知限制

- `findall` 有多个捕获组时仍返回整段匹配而不是元组，`split` 里未参与匹配的组
  用空串代替 None，没有 `groupdict` / `lastindex` / `lastgroup` / `expand` /
  `finditer`，`sub` 的替换只支持 `\1`..`\9` 和 `\\`（不接受函数）；`Match.group(n)`
  对未参与匹配的组返回空串而不是 None。
- 模式错误抛 `ValueError`，不是 CPython 3.13 的 `re.PatternError`。
- 没有 Unicode 属性类（`\p{...}`），全程按字节匹配。

## [0.75.1] - 2026-11-10

### 工程

- 许可证定为 Apache License 2.0，新增 `LICENSE` 与 `NOTICE`。

## [0.75] - 2026-11-10

`re` 模块 v1：回溯 VM + Pattern / Match。

### 新增

- 内置 `re`（`runtime/rt_re.c`）：`compile` / `search` / `match` /
  `fullmatch` / `findall` / `sub` / `split` / `escape`；`Pattern` 上的同名
  方法；`Match` 的 `group` / `groups` / `start` / `end` / `span`。
- 语法：字面量、`.`、字符类（含范围/取反）、`\d\w\s\D\W\S`、`\b`、
  `^ $`、分组（捕获 / `(?:...)` / `(?P<name>...)`）、交替、`* + ? {m,n}`
  （含惰性）；标志 `I`/`IGNORECASE`、`M`/`MULTILINE`、`S`/`DOTALL`。
- 字符串按字节匹配，与 py23 的 `str` 语义一致。
- `tests/run/56_re.py` 覆盖 search/groups/compile/flags/lazy/alternation/
  findall/sub/split/escape 和 Pattern 方法。

### 已知限制

- v1 不支持 lookaround、反向引用、条件组；`findall` 返回整段匹配而不是
  分组元组，`split` 不含捕获组，没有 `groupdict`；未匹配分组返回空串
  而不是 None；`sub` 的替换只支持 `\1`..`\9` 和 `\\`。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 差分 0 失败。
- re 探针与 CPython 3.13 逐行一致（除 CPython 对位置 `count`/`maxsplit`
  的 DeprecationWarning）。

## [0.74.1] - 2026-11-10

修复四元数 probe 暴露的类型传播与 fsum 缺陷。

### 修复

- **容器元素跨数值域实参**：`list` / `tuple` / `dict` / `set` 的 `int` → `float`
  实参现在在 build 阶段报错，不再静默按 double 读 int64 槽位（`bind_args`
  补 `bad_elem` 检查，与赋值路径一致）；`math.fsum([1,2,3])` 同样被拒。
- **槽位类型回填**：`store.slot` 遇到 unknown 槽位时从源 vreg 补类型，
  `load.slot` 在调用方给 unknown 时回退到槽位表。修复 `call.rt` 结果经
  临时槽后退化成整数比较（非法 `cmpq %xmm8`）、`-O2` 浮点结果与
  `-O0/-O1` 分叉、循环内条件更新不生效（`max_drift` 恒 0）三条现象。
- **`math.fsum` 精确求和**：从 Kahan 补偿求和换成 CPython 的 Shewchuk
  部分和算法；`fsum([1e16, 1.0, -1e16])` 现在返回 1.0。
- `tests/run/55_slot_types.py` 覆盖 call.rt → 槽位 → 浮点比较、循环条件
  更新和 fsum 抵消。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 差分 0 失败。
- quaternion-probe：attitude 在 `-O0/-O1/-O2` 下与 CPython 逐位一致；
  selftest 14/14；bench checksum 与末态一致。

## [0.74] - 2026-11-10

stdlib 批次：`string` / `time` / `os` / `itertools` / `collections` / `json`。

### 新增

- `stdlib/string.py`：随编译器分发的纯 py23 子集，提供
  `ascii_lowercase` / `ascii_uppercase` / `ascii_letters` / `digits` /
  `hexdigits` / `octdigits` / `punctuation` / `whitespace` / `printable`
  常量与 `capwords()`。
- 内置 `time`（`runtime/rt_time.c`）：`time` / `monotonic` /
  `perf_counter` / `sleep`，秒为单位。
- `os` 扩展（`runtime/rt_os.c`）：`listdir` / `getenv` / `mkdir` /
  `rmdir` / `remove` / `rename` / `getcwd` / `cpu_count`；`getenv` 缺失
  返回空串（CPython 返回 None）。`os.path` 增加 `join` / `exists` /
  `isdir` / `isfile` / `basename` / `dirname` / `normpath` / `abspath` /
  `expanduser` / `splitext`。
- `itertools`（`runtime/rt_stdlib.c`）：`chain` / `repeat` / `islice`，
  按实参静态特化，结果是 list。
- `collections`：`Counter` / `most_common`。
- `json`（`runtime/rt_json.c`）：`dumps` / `loads`。`dumps` 按实参静态
  类型生成描述符；`loads(text, sample)` 用 sample 的静态类型决定目标
  结构，覆盖 int / bool / float / str / None / list / dict / 定长 tuple。
- `tests/run/53_json.py`、`tests/run/54_stdlib.py`。

### 修复

- `json.dumps` 默认分隔符对齐 CPython：`", "` / `": "`（原来是无空格
  的 `,` / `:`）。
- `json.loads` 允许 `None` 作为 sample，解析 JSON `null` 返回 None，
  与 `dumps(None)` 对称；类型检查与降级两处原先显式拒绝 None。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；O0/O1 52 个运行用例通过。
- 自举差分 runner `--seeds 3`：810 个程序 0 失败。
- `json.dumps` 与 CPython 输出逐字节一致；`loads` 覆盖 null、嵌套、
  类型错误与语法错误；`54_stdlib.py` 覆盖 string / time / os /
  itertools / collections。

## [0.73] - 2026-11-10

差分测试自举：`tests/diff/run_diff.py` 现在可以用 py23 自己执行。

### 新增

- `tests/diff/run_diff.py` / `generators.py` 改写为 py23 子集：去掉
  `pathlib`/`dataclasses`/`argparse`/`tempfile`/`concurrent.futures`，
  用 `os`/`os.path`/`random`/`subprocess`/`sys.argv` 重写；810 个差分
  用例由 py23 编译执行 runner 后生成并比对。
- 随机引擎逐位一致，同一份脚本在 py23 和 CPython 下生成的用例完全相同
  （用 `--seeds/--only/--cases` 控制）；runner 串行执行。
- `repr` 生成器不再依赖 `struct`/`getrandbits`（py23 没有 bytes 重解释），
  改成固定边角值（0/-0、次正规、最大正规值）+ 十进制定标随机值。
- `py23 run prog.py -- a b`：`--` 之后的参数原样传给被运行的程序。
- 注解支持模块限定的原生类型：函数体里可以写
  `r: random.Random = rng` / `p: subprocess.CompletedProcess = ...`
  （函数签名注解在 import 处理前解析，这两个类型仍要在函数体内收窄）。

### 修复

- **unknown 值写入带标注的堆槽时引用计数失衡**：`store_slot` /
  `store_global` / `store_field` 原来按源值类型决定 retain，源是 unknown
  时不加引用，但函数出口按目标槽类型 release，导致对象被提前释放
  （递归 + `r: random.Random = rng` 可稳定触发堆损坏）。现在按目标类型
  判断，unknown 源走运行时的保守 inc（整数安全 no-op）。

### 验证

- 自举 runner 跑满 `--seeds 3`：810 个程序 0 失败；CPython 执行同一脚本
  的输出逐字节相同。
- `ctest --preset dev` 连跑 3 次、release 全绿；O0/O1 全量 52 个运行
  用例通过；`verify.sh` 的差分段已切到 py23 runner。

## [0.72] - 2026-11-10

运行时扩展：`random`、`os`/`os.path`、`subprocess` 三个内置模块。

### 新增

- `random`：`Random(seed)` 对象与模块级函数（`random` / `seed` /
  `randint` / `uniform` / `choice` / `shuffle`）。引擎按 CPython 的
  MT19937 实现（`seed(int, version=2)`、`genrand_res53`、
  `_randbelow`、Fisher-Yates 全部对齐），同一个整数种子产生与 CPython
  逐位相同的序列；省略种子时用时间 + pid 播种。`choice` 返回序列的
  元素类型（支持 list/tuple/str，str 在降级期转 list），`shuffle` 只
  接受 list。
- `os`：`mkdir` / `rmdir` / `remove` / `rename` / `getcwd` /
  `cpu_count`，失败按 errno 抛 OSError 子类（FileNotFoundError /
  FileExistsError / PermissionError 等）。
- `os.path`：`join`（变长参数，posixpath 语义）、`exists` / `isdir` /
  `isfile`。作为独立内建模块支持 `import os.path as p` 与
  `from os.path import join, exists`。
- `subprocess.run(args, capture_output=False, text=False,
  timeout=-1.0, check=False)`：fork/execvp + 管道捕获，返回
  `CompletedProcess` 原生对象（`.returncode` / `.stdout` / `.stderr`）。
  超时 SIGKILL 后抛 `TimeoutExpired`；`check=True` 且退出码非零抛
  `CalledProcessError`；被信号终止时返回码为负信号号。
- 类型系统新增 `Random` / `CompletedProcess` 两个原生类型：属性读取、
  方法表、堆类型与 RC 管理、打印文本（`<random.Random object>` /
  `<subprocess.CompletedProcess object>`）、`str()` 转换全部接通。
- `TimeoutExpired` / `CalledProcessError` / `SubprocessError` 进入内置
  异常名表和父类匹配表，可直接 `except TimeoutExpired`。

### 实现说明

- `random` 的模块级函数共用一把静态全局 RNG（C 静态对象，不进 GC
  链）；实例用 `rc_alloc` 分配，无子对象，GC 保守扫描状态数组。
- `subprocess.run` 用 `poll` + `waitpid(WNOHANG)` 循环实现超时；捕获时
  同时读 stdout/stderr 两个管道，关闭后回收子进程。没有捕获时按 10ms
  粒度轮询退出状态。
- 已知偏差：没有 bytes 类型，`text=False` 也返回 `str`；没有捕获时
  `.stdout`/`.stderr` 是空串（CPython 是 None）；`input`/`cwd`/`env`/
  `shell` 未实现；exec 失败按退出码 127 返回；`from subprocess import
  TimeoutExpired` 暂不支持（按内置异常名捕获）。

### 验证

- 探针与 CPython 逐行一致：`Random(42)`/`Random(99)` 的
  randint/random/choice/uniform/shuffle 序列、模块级 `seed()` 后序列、
  from-import 用法；os 的目录创建/改名/删除、join/exists/isdir/isfile、
  FileExistsError 捕获；subprocess 的正常输出、非零退出码、
  stdout/stderr 分离、超时与 check。
- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过。

## [0.71] - 2026-11-10

协议 dunder 第二批：哈希/包含/迭代，以及算术与比较运算符重载。

### 新增

- `__hash__` / `__eq__` 接入 dict/set 键：`Py23Dict` 增加 hash/eq 槽号，
  哈希与查相等经 vtable 调类的实现（不同实例内容相等可命中同一个键）。
  字典/集合的所有构造点（字面量、推导式、`dict()`/`set()`、kwargs、
  `set(list)`）都传槽号。两个都缺时退回指针哈希 + 指针比较（没定义
  `__eq__` 的类的身份语义）。
- `hash()`：类走 `__hash__`；没有该方法和 `__eq__` 时退回身份哈希
  （指针混合，和字典对类键的默认一致）；只定义 `__eq__` 没有 `__hash__`
  按 CPython 规则编译期报不可哈希。
- `__contains__`：`in` / `not in` 分发；类没有可用实现时编译报错。
- `__iter__`：`for x in obj` 先调 `__iter__`，再按返回值
  （generator/list/tuple/str）走现有迭代逻辑。
- 算术 dunder：`__add__` / `__sub__` / `__mul__` / `__truediv__` /
  `__floordiv__` / `__mod__` / `__pow__`，以及反射版 `__r*__` 和原地版
  `__i*__`。分派顺序：普通二元先左后右反射；`+=` 依次
  `__iadd__` → `__add__` → 右操作数 `__radd__`。
- 比较 dunder：`__lt__` / `__le__` / `__gt__` / `__ge__` 及右操作数
  镜像版（`3 < obj` 可调 `obj.__gt__(3)`）。
- 类型检查在定义处校验新 dunder 的签名：算术 dunder 允许标量参数；
  返回 bool 的（eq/ne/contains/cmp）第二个参数限 unknown 或类类型。

### 修复

- `kNoReg` 是 `0xFFFFFFFF`，`if (VReg v = dunder_call(...))` 恒为真：
  正向分派失败后直接返回哨兵，反射分支永远不执行（`3 * obj` 退回整数
  乘法后崩溃）。所有同类判断改为显式 `!= kNoReg`。
- 增广赋值把 `+=` 记号直接传给 dunder 表（表里只有 `+`），查表失败退化
  成整数运算；先归一化记号再查表。增量赋值同时扩展到「右侧是类」的
  反射场景（`x += obj` 调 `obj.__radd__`）。
- `dict.setdefault` 只 retain value 没 retain key：类键（以及字符串键）
  在临时栈释放后留下悬空引用，程序退出时报 refcount 错误。补上 key 的
  引用计数。

### 变更

- 类实例参与算术、两侧都没有匹配 dunder 时编译报错
  `unsupported operand type(s)`（以前会按机器整数算指针，静默出错）。
- `<` / `<=` / `>` / `>=` 在类实例缺少比较 dunder 时编译报错
  `not orderable`（`==` / `!=` 仍按指针身份比较，和 CPython 一致）。

### 验证

- 探针与 CPython 逐行一致（默认对象文本 `<X object>` 保留既有简化）：
  类键 dict 字面量/get/setdefault/pop/del/update/clear/len、set 的
  in/add/discard/pop/update/clear、哈希与相等、反射算术、增量回退、
  生成器 `__iter__`；dict/set 变更后无引用计数告警
- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过

## [0.70] - 2026-11-10

协议 dunder：`__str__` / `__repr__` / `__eq__` / `__ne__` / `__len__`。

### 新增

- 类实例的协议方法通过 vtable 分发（子类覆盖生效）；静态类型的 MRO 里
  没有实现时退回默认行为：
  - `__str__` / `__repr__`：`print`、`str()`、`repr()`、f-string（含
    `!r` 和格式说明）、list/tuple 容器内部元素。str 语义先 `__str__` 再
    `__repr__`；repr 语义只用 `__repr__`，缺省打印 `<类名 object>`。
  - `__eq__` / `__ne__`：`==` / `!=`。`other` 可省略标注（unknown）或
    标成类类型；标成具体类时调用点检查实参可赋值性，不匹配回退指针
    比较（不会把错误类型的值送进方法体）。没定义 `__ne__` 时用
    `__eq__` 取反。两边都没有实现时维持原来的指针比较。
  - `__len__`：`len()` 以及所有真值场景（`if` / `while` / `not` /
    `and` / `or` / 三元表达式 / `bool()` / `assert` / 推导式过滤 /
    match guard）。没有 `__len__` 的类调 `len()` 在编译期报错。
- 类型检查在定义处校验 dunder 签名（必须是普通方法、参数个数、返回类型；
  `__eq__`/`__ne__` 的 other 必须是 unknown 或类类型），错误直接指到
  定义行。

### 验证

- 协议探针与 CPython 输出逐行一致（默认对象文本 `<X object>` 保留
  py23 的既有简化）：子类覆盖、`super().__str__()`、继承的
  `__repr__`/`__len__`、容器内 repr、文件输出、`while obj:` 真值循环、
  f-string `!r`/格式说明
- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过

### 已知限制

- 只接这五个 dunder；`__hash__` / `__contains__` / `__iter__` / 算术类
  dunder 未实现
- `__eq__` 的 other 为 unknown 时方法体里不能读字段（属性访问需要具体
  类）；内容比较请把 other 标成本类（类体内用字符串前向引用）
- f-string 格式说明对类实例有意放宽：先 `__str__` 再套用字符串格式说明
- 默认对象文本没有模块名和地址

## [0.69] - 2026-11-10

循环旋转：省掉每轮的无条件跳转。

### 新增

- O2 新增循环旋转遍：`while` 头块末尾是 `cmp` + `branch`、循环体内只有
  一条回边时，把头的条件计算克隆成 latch 块插在体块后面，回边改走
  latch。条件求值次数不变（k 次体执行对应 k+1 次判断），但体块 →
  latch 是相邻块，O2 的下落消除省掉每轮的 `jmp`，latch 末尾直接
  `cmp` + 条件跳回体块。实现上要求头块有循环外前驱（避免把上一轮的
  latch 当新头反复旋转）、头块内部定义的 vreg 不被其它块引用；
  `has_handlers`/生成器/带 `continue` 的多回边循环跳过。

### 验证

- 纯计数循环 1e9 次：167–174ms → **110–126ms（约 -26%）**
- 依赖链较重的基准变化在噪声内（`loop` 95ms、`calls` 53ms 不变）
- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过；continue/break/nested 循环单独验证

## [0.68] - 2026-11-10

`str(int)` 改手写十进制转换。

### 新增

- `py23_str_from_i64` 不再走 `snprintf`：从缓冲区尾部倒写十进制，负数补
  符号，`INT64_MIN` 用 `-(v+1)+1` 取绝对值。`str(i)` 的格式解析在
  `strlist`/`dict_str` 这类热路径上占比很大。

### 验证

- 运行时微基准（10M 次 `str(i)` + 释放）：420ms → **173ms**
- 同源 A/B：`strlist 300k` 26–34ms → **19–23ms**；`dict_str 300k`
  56–60ms → **45–50ms**（C++ 53.8ms，已反超）；`dict_int` 不变
- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过；正负数、19 位边界、INT64_MIN 输出与
  CPython 一致

## [0.67] - 2026-11-10

修复异构定长元组打印崩溃。

### 修复

- `print((str, int))`、`print(list(d.items()))` 这类「元素类型不同的定长
  元组」原来只按 `sub[0]` 一个静态类型打印，int 会被当成 `str` 交给
  `py23_repr_str`，读非法指针直接段错误（旧版本同样存在）。
  `print_seq` 现在检测定长异构元组，按常量下标展开、每个元素用自己的
  静态类型打印；同构元组仍走循环。`print(..., file=...)` 同步修好。

### 验证

- 直接、嵌套、列表内、文件输出等元组打印形状与 CPython 一致
  （类实例仍按 py23 的简化 `<P object>` 格式）
- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过

## [0.66] - 2026-11-10

字典改成「密集 entries + int32 索引表」。

### 新增

- `Py23Dict` 重写为 CPython 式紧凑布局：entries 是密集数组、按插入顺序
  存放 `{hash, key, val}`；索引表是 int32 开放寻址，槽里是「entry 下标
  + 1」，0 表示空。查找先走 4 字节索引（probe 链常驻缓存），命中才读
  24 字节 entry。扩容/删除只重建索引表，entries 不搬家；顺序遍历直接
  扫 entries，不再需要 order 数组。
- 装载因子 2/3；插入路径去掉 order 数组写入；删除时 entries 保持顺序
  前移一格再重建索引（删除仍不在热路径）。
- GC 的 dict 扫描/清理、运行时销毁路径同步到新布局。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过；字典顺序/删除/迭代/items/pop/update
  单独验证
- 运行时微基准（1M int 插入 + 1M 查找）：插入 94–110ms → **38–40ms**，
  查找 21ms → 17.5ms
- 基准（release `-O2`，3 轮中位数）：`dict_int 1M` **141ms → 61.8ms**
  （CPython 137.8ms，C++ 43.8ms）；`dict_str 300k` 74.7 → **59.3ms**
  （CPython 78.4ms）；其余项持平。至此 8 项基准全部快过 CPython

### 已知限制

- 删除仍是 O(n)（entries 顺序前移 + 索引重建）
- 索引槽是 int32，单字典条目上限 2^31-1

## [0.65] - 2026-11-10

内联器接通直接调用。

### 新增

- O2 内联器的调用图与内联点识别同时接受 `call.direct`：去虚化后的方法
  调用、静态函数直接调用都能内联；递归检测补上 `call.direct` 边，自递归
  不会膨胀。直接化 pass 之后再跑一轮内联，静态可解析的闭包调用也有机会。
- `field.get` 计入纯指令：内联后没人读的字段装载会被 DCE 删掉。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过
- 基准（release `-O2`，`taskset -c 0`，3 轮中位数）：`calls 1e8`
  **123ms → 58ms**（CPython 4910ms、C++ 20ms）；`objects` 60.6ms、
  `dict_str` 74.7ms、`strlist` 28.2ms，这三项都已快过 CPython；
  其余项持平
- 内联预算沿用每函数 24 个内联点 / 256 条指令增量，未出现代码膨胀失控

### 已知限制

- 递归函数（含 `call.direct` 自递归边）不内联；动态调用只做直接化尝试
- 内联按槽替换：只读形参完全消掉槽，被写形参仍复制一份槽

## [0.64] - 2026-11-10

调用开销：终态类去虚化、只读形参免 RC、死结果存储。

### 新增

- 终态类去虚化：跨模块继承不支持，所以一个类的所有子类都在本模块
  可见；没有被任何本模块类继承的类就是终态。终态类接收者上的方法
  调用在编译期把 vtable 槽解析成函数下标，发 `call.direct`；构造点的
  `__init__` 调用同样直接调用（对象类型确定）。存在子类的静态类型
  仍走 vtable。
- 只读形参免 RC：函数降级完成后扫描——形参槽在整个函数里从未被覆盖、
  也没被取址时，入口的 retain 和所有出口的 release 全部撤掉。形参
  本来就是按值借用的，调用期间实参的引用在调用方手里；方法调用的
  `self` 绝大多数属于这一类。
- 调用结果没人读取时（表达式语句里的调用），返回值不再搬回栈槽。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过；子类覆盖、继承方法、staticmethod、
  闭包捕获形参、形参重赋值、返回形参等路径单独验证
- 基准（release `-O2`，`taskset -c 0`，3 轮中位数）：`calls 1e8`
  **280ms → 129ms**（CPython 5085ms、C++ 22ms）；`objects` 72.8ms、
  `dict_str` 87ms、`strlist` 31ms 与上一版持平；`fib` 32.6ms
  （历史波动 32–41ms）

### 已知限制

- 去虚化要求被调方法已经编译：前向引用、类内后定义的方法会退化成
  vtable 调用（语义不变）
- 生成器/协程的形参保持原有 retain/release：帧槽跨挂起，需要持有

## [0.63] - 2026-11-10

GC 只收集可能成环的对象，容器堆加无表快速路径。

### 新增

- exact-RC 模式（索引关闭）下，字符串和没有堆字段的类实例不进 GC 堆链：
  它们没有任何出边、不可能成环，RC 归零即释放。GC 只遍历容器和带字段
  的实例；垃圾容器持有的这类叶子在清扫时按正常引用释放。叶子也不计入
  GC 触发阈值，字符串再多也不会白触发全堆扫描。
- 无表快速路径：链上没有任何需要保守扫描的 OBJ/实例时，不再给整堆建
  地址表。把全局/临时栈/真实栈/寄存器里的机器字收敛成一张常驻缓存的
  小候选表，扫一遍堆命中标记；列表/字典/闭包的子节点按静态类型直接
  标记，纯数值元素的容器完全不标记。
- 链上对象数在分配/释放时增量维护，收集时不再计数；GC 地址表和工作栈
  跨收集复用，不再每轮 calloc/free 十几 MB。
- `PY23_GC_TIMING=1` 打印每轮收集的 n/live 与 table/mark/release/sweep
  分段耗时。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过；循环、异常、复数、带堆字段类、super
  等路径单独验证
- 8 项基准（release `-O2`，`taskset -c 0`，3 轮中位数）：
  `objects 1M` **72ms**（CPython 364ms，快 5×；最初 1558ms）；
  `dict_str 300k` **85ms**（CPython 89ms，已反超）；
  `strlist 300k` **35ms**（CPython 41ms）；其余项持平
- `objects` 规模扫描 8/22/83ms（100k/300k/1M），线性

### 已知限制

- 带堆字段的类实例、异常、文件仍是保守扫描；这类对象在场时走地址表
- 程序退出时，未进链的叶子交给进程回收，不逐个释放（泄漏检测工具会
  看到这部分）
- 叶子排除与快速路径都依赖 exact-RC 判定；unknown/asyncio/生成器程序
  行为不变

## [0.62] - 2026-11-10

Typed 程序关闭存活堆索引。

### 新增

- 存活堆索引按程序裁剪：编译器扫描全部模块的 IR（类型里的 `unknown`、
  `RcIncUnknown`、`super()`、`isinstance` / 异常匹配 / unknown 窗口比较，
  以及 asyncio 导入和生成器），全都不需要时入口调用
  `py23_disable_unknown_index`，此后每次堆分配/释放不再维护索引哈希表。
  多模块取并集；缓存清单新增 `unknownindex` 行，命中缓存也参与判断。
  实测同源 A/B：`objects 1M` -29%（241→171ms）、`strlist 300k` -19%
  （57→46ms）、`dict_str 300k` -13%（152→132ms）。
- `PY23_FORCE_UNKNOWN_INDEX=1` 强制保留索引（A/B 诊断用）。
- 模块缓存清单加 `toolchain` 指纹（编译器可执行文件 + `rt.o` 的大小与
  mtime）：换了编译器/运行时后源码没变也不会命中旧对象。之前
  `py23 run` 曾静默复用旧代码生成结果。

### 修复

- `super()` 的运行期查找用 RC 注册表确认接收者是实例；`isinstance`、
  异常匹配、unknown 值的容器/字符串模式检查同理，所以这些调用点一律
  保留索引，避免「typed 程序关索引后 super 报 AttributeError」。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过
- 基准（release `-O2`，`taskset -c 0`，3 轮）：相对 CPython 的差距
  `strlist` 3.6×→1.3×、`dict_str` 6.1×→1.7×、`objects` 4.7× 慢→1.4× 快；
  纯标量项不变

## [0.61] - 2026-11-10

GC 阈值自适应与空字典标注修复。

### 修复

- 循环收集阈值从固定 2MB 改成「上次收集后的存活堆字节数」，下限 2MB。
  固定阈值在存活堆大时反复全扫：1M 个存活对象约 88MB，按 2MB 触发要
  扫四十多遍，总成本接近 O(n²)。自适应后存活堆翻倍才扫一次，总扫描量
  是 O(live)；垃圾多、存活少时回落到 2MB 下限，扫描本身也便宜。
- `d: dict[str, int] = {}`：标注赋值里的 `dict` / `set` 字面量现在和
  `list` / `tuple` 一样按标注类型构造。空字典字面量自己推不出 key 类型，
  以前 key_kind 落成「按机器字比较」，动态 `str(i)` key 写进去再查会
  `KeyError`。
- 新增 `PY23_GC_DISABLE=1`：关闭循环收集，只保留引用计数（嵌入/基准用，
  会漏检环）。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过；RC/循环压力测试 O0/O1/O2 全过
- 8 项对比（release `-O2`，`taskset -c 0`，3 轮中位数）：
  `objects 1M` 1558ms → 238ms（CPython 330ms，从慢 4.7× 变成快 1.4×）；
  `dict_str 300k` 572ms → 140ms（CPython 77ms，差距从 6.1× 收到 1.8×）；
  `strlist 300k` 138ms → 56ms（CPython 33ms）；`dict_int 1M` 148ms → 130ms，
  已与 CPython 持平；其余项不变
- `objects` 规模扫描恢复线性：100k/300k/1M = 17/46/239ms
  （CPython 27/87/314ms）

### 已知限制

- 循环收集仍是 stop-the-world 全堆扫描；大存活堆下每次收集仍是 O(live)
- 没有标注、或先声明后 `d = {}` 的空字典仍按 `unknown` key 类型处理
- 模块缓存键只看源码/选项/ABI，编译器换了以后 `py23 run` 可能命中旧
  对象，基准与回归要用 `--rebuild`

## [0.60] - 2026-11-10

跨块常量立即数化。

### 新增

- O2 下整函数范围的 `const.int`（32 位有符号内）直接当立即数用：不占
  callee-saved 寄存器、不落栈槽，定义指令整条省略。循环里的 `i += 1`
  因此是 `addq $1, %rbx`，也不再挤占跨块值可用的一个寄存器。
- `emit_truth` 遇到常量条件时先落 `%rax`，与常量分支折叠路径一致。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过
- 基准 `fib` / `inv` / 浮点循环与上一版持平（这些循环受依赖链限制），
  生成代码少一条指令、函数序言少 push 一个寄存器

## [0.59] - 2026-11-10

浮点跨块寄存器化，外加一处静默错误修复。

### 新增

- 浮点跨块 vreg 提升（O2）：无调用函数里 xmm8–15 扣掉浮点槽后仍有空闲
  时，再按使用次数把至多两个跨块浮点值常驻寄存器。会发调用的浮点指令
  （`//`、`%`、`**` 走 libm）不算无调用；`has_handlers` / 生成器跳过。
- 浮点槽调用点按活跃性写回：块级 live-in/live-out 只把调用后仍会被读的
  提升槽 spill/reload，调用后不再读的槽不再往返内存。

### 修复

- 未标注参数与 `float` 混合的算术（`n * 1.1`、`n / 2`）以前落进整数
  路径，整数乘/除的是一串位模式，结果静默错误。现在结果类型按 `float`
  计算，`unknown` 一侧补 `int`→`double` 转换；`/` 只要有一侧 `unknown`
  也按 `float` 产出。
- 类型被 `unknown` 吞掉、实际却流着浮点值的变量不再进整数寄存器：
  结果融合会生成非法的 `movsd %xmmN, %rGPR`，汇编器直接报错。

### 验证

- `ctest --preset dev` 连跑 3 次、release 全绿；810 程序差分零失败；
  O0/O1 全量 52 个运行用例通过
- 基准：100M 次依赖加法浮点循环开/关浮点跨块提升都是 ~74ms（省掉两条
  内存操作数，但乱序执行已把访存藏在依赖链后面）；差分确认语义不变

### 已知限制

- 浮点跨块提升只覆盖无调用函数；有调用的函数仍靠槽写回
- 未标注参数按整数表示传递，实际的浮点实参传给未标注参数仍不支持

## [0.58] - 2026-11-10

跨块寄存器化与编译提速。

### 新增

- 跨块 vreg 提升（O2）：局部变量没用到的 callee-saved 寄存器按使用次数
  分给最热的跨块整型临时值（循环不变量、合并结果、循环边界等）。寄存器
  整函数独占、不做活跃性复用，循环回边天然安全；`has_handlers` 函数跳过。
- 无调用函数（整函数没有会破坏 %r8–%r11 的指令）再拿至多两个 %r8–%r11
  承载跨块值；块内临时寄存器池同步缩小，二者不冲突。
- 编译期提速：
  - 块标签、vreg/槽内存操作数、全局符号标签全部预生成缓存，指令发射
    不再反复 `std::format`；
  - 常用 `movq/cmpq/jmp` 等改为直接字符串拼接 + `std::to_chars` 立即数；
  - `move_to` 走无分配拼接路径；
  - dev 预设改为 `-g -Og`：开发版编译器编译大文件从 ~350ms 降到 ~61ms
    （约 5.7 倍），release 约 33ms（较基线再快 2–5%）。
- `PY23_TIMING=1` 可打印 check/lower/codegen 分阶段耗时（env 开关）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 基准：`fib` 7ms、`inv` 4ms、`matmul(160)` 5ms 与优化前持平（这些循环
  受依赖链/边界检查限制，跨块值的访存已被乱序执行隐藏）；差分与全套
  测试确认生成的代码语义不变

### 已知限制

- 跨块提升只覆盖整型/指针类 vreg；浮点跨块值仍走内存
- 无调用函数的 %r8–%r11 全局占用上限为 2 个，避免挤掉块内临时值
- 活跃性/移动端优化未做；跨块提升按「整函数独占」保守分配

## [0.57] - 2026-11-10

asyncio 再补 `Semaphore`，并接通 `frozenset`。

### 新增

- `asyncio.Semaphore([value=1])`：`await acquire()` / `release()` /
  `locked()`，等待者 FIFO；release 优先把名额转给等待者，无等待者时
  计数累加（与 CPython 一致）。
- `async with sem:`：进入 acquire、退出（含异常路径）自动 release。
- `from asyncio import Semaphore` 支持零参形式。
- `frozenset(seq)`：复用 set 的运行时与去重语义（不强制不可变）；
  `id()` 的类型检查补成 int（此前只放行）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：Semaphore(2) 三任务并发顺序、async with 与
  locked() 状态、from-import、手动 acquire/release 均与 CPython 一致
  （O0/O1/O2）

### 已知限制

- 与 Lock 相同：不支持 `Semaphore` 的 `_value` 直接读写；取消等待者时
  名额回收由 release 路径承担（未实现取消时自动补发）

## [0.56] - 2026-11-10

asyncio 收尾：Queue nowait、Event 上下文、`asyncio.timeout` 与 `TaskGroup`。

### 新增

- Queue：`put_nowait` / `get_nowait`（满/空抛内置异常 `QueueFull` /
  `QueueEmpty`）；与前一轮的 `full`/`join`/`task_done` 拼齐。
- `async with event:`：进入时 await wait()，退出无动作。
- `asyncio.timeout(delay)`：新增 await kind 9 定时器；到期向当前协程
  注入 `TimeoutError`（`try/except` 可捕获），退出时移除定时器；
  `t.expired()` 可查询。新增 `loop_active_coro` 记录正在执行的协程。
- `asyncio.TaskGroup()`：`async with` 内 `create_task` 记录子任务；退出
  时用 gather 等待全部完成，任一失败取消其余任务并抛第一个异常
  （普通异常而非 `ExceptionGroup`）。
- 内置 async with（Lock/Event/Timeout/TaskGroup）的清理记录接入挂起
  摘除与恢复重挂；push handler 前清理管理器表达式临时值，修复
  “invalid temporary stack mark”。
- 类型/降级/代码生成新增 AsyncTimeout、AsyncTaskGroup 及对应运行时；
  `from asyncio import timeout, TaskGroup` 可用。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：timeout 成功/超时/expired、TaskGroup 正常与子任务
  失败、Queue nowait 满/空、Event async with、from-import
  timeout/TaskGroup/Lock 均与 CPython 输出一致（O0/O1/O2）

### 已知限制

- TaskGroup 子任务失败抛原始异常（CPython 抛 ExceptionGroup），且在
  失败后仍会等完其余任务，不立即取消兄弟任务
- `asyncio.timeout` 依赖毫秒定时器；仅注入当前协程，不支持嵌套超时
  的精确语义
- Queue 元素静态类型仍是 unknown；`put`/`put_nowait` 不接受 float

## [0.55] - 2026-11-10

补 `type` / `id`，asyncio 深入：受限队列、join 与 `async with lock`。

### 新增

- `type(x)`：新增 `TypeObj` 类型（sub[0] 是被表示类型）。打印输出
  `<class 'int'>`；`type(x) == int` / `is` / `!=`、`type(x) == type(y)`
  编译期折叠；`type(x).__name__` 返回类型名。运行时令牌是驻留字符串。
- `id(x)`：堆值返回指针、整数返回自身、浮点返回位模式；同进程内稳定。
- `asyncio.Queue([maxsize])`：满队列时 put 返回未完成 Future 并按 FIFO
  等待，get 腾位后放行；新增 `full()`、`await join()`、`task_done()`
  （unfinished 计数与 CPython 一致），put 改为始终返回 Future。
- `async with lock:`：内置 Lock 的异步上下文管理器，退出（含异常路径）
  自动 release；类型检查同步放宽。
- `from asyncio import Queue` 支持零参形式（maxsize 默认 0）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：type 打印/比较/`__name__`、id 自反性、受限 Queue
  阻塞与 join/task_done、`async with lock` 正常/异常路径、from-import
  Queue 均与 CPython 一致（O0/O1/O2）

### 已知限制

- `type(x)` 是静态类型令牌：拿不到更精确的运行期子类；`type(None)` 的
  `__name__` 为 `NoneType`，但令牌不适合当字典键以外的动态用途
- `id` 数值与 CPython 不同；`type`/`id` 不能用于动态类构造
- Queue 的 `put_nowait` / `get_nowait` / `QueueFull` / `QueueEmpty` 未实现
- `async with` 仅内置 Lock 支持，Event/Timeout/TaskGroup 尚未接入

## [0.54] - 2026-11-10

asyncio 同步原语：`Event` / `Lock` / `Queue` 与 `wait_for`。

### 新增

- `asyncio.Event()`：`await wait()` 返回 `Future[bool]`，`set()` 唤醒全部
  等待者，`is_set()` 查询状态；已 set 的 `wait()` 立即完成。
- `asyncio.Lock()`：`await acquire()` 返回 `Future[bool]`，`release()`
  按 FIFO 唤醒下一个等待者，`locked()` 查询；release 未持有时抛
  `RuntimeError`。
- `asyncio.Queue()`：无界队列；`await put(v)`（返回已完成的
  `Future[None]`，与 CPython 的协程签名兼容）、`await get()`、`qsize()`、
  `empty()`。空队列阻塞 get，put 直接唤醒等待中的 getter。
- `asyncio.wait_for(aw, timeout)`：事件循环里的「子任务 + 定时器」竞速
  （新增 await kind 8）；正常完成按原结果/异常交付，超时取消子任务并抛
  `TimeoutError`；被外部取消时级联取消子任务。支持协程、Task、Future、
  sleep/fd 等可等待对象。
- 类型系统新增 `asyncio.Event` / `asyncio.Lock` / `asyncio.Queue` 三种
  类型（跨模块接口按 unknown 退化）；`from asyncio import Event, Lock,
  Queue, wait_for` 也可用。
- `RtFn` 超过 255 项，底层类型从 `uint8_t` 扩到 `uint16_t`。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：Event 阻塞/唤醒/重复 wait、Lock FIFO 互斥、
  Queue put/get 阻塞与 qsize、wait_for 成功/超时/对 Future 竞速与
  `TimeoutError` 捕获均与 CPython 一致（O0/O1/O2）

### 已知限制

- Queue 元素静态类型是 unknown：`await q.get()` 的结果拿不到具体类型；
  `put` 暂不接受 float（会明确报错）
- `asyncio.Queue(maxsize=...)`、`TaskGroup`、`asyncio.timeout` 尚未支持
- `wait_for` 的超时精度受毫秒定时器限制

## [0.53] - 2026-11-10

补 `hash()`，并把 math 模块补齐到 CPython 成员集合。

### 新增

- `hash(x)`：支持 int/bool/float/str，直接复用字典键的哈希
  （`dict_hash_value`），同值同哈希，与 py23 字典/集合行为自洽；不可哈希
  类型（list/dict/set/tuple/类）明确报错。
- math 新增：`acos`/`asin`/`atan`/`acosh`/`asinh`/`atanh`/`cbrt`/`cosh`/
  `sinh`/`tanh`/`exp2`/`expm1`/`log1p`/`erf`/`erfc`/`gamma`/`lgamma`/
  `ulp`/`nextafter`/`remainder`/`ldexp`/`fma`/`isclose`/`frexp`/`modf`/
  `isqrt`/`comb`/`perm`/`prod`/`sumprod`/`dist`。`frexp`/`modf` 返回二元组；
  定义域错误（`acos`/`atanh`/`gamma` 极点等）抛 `ValueError`。
- 运行期新增 frexp/modf 二元组构造、整数 sqrt/comb/perm、浮点距离与
  补偿求和等实现；`call_native` 对堆返回值补临时值登记。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：31 个新 math 函数/常量与 CPython 数值一致
  （`lgamma` 末位受浮点打印格式影响除外）；`hash` 的自反/区分性、
  frexp/modf 解包、isqrt/comb/perm/prod/sumprod/dist 均通过（O0/O1/O2）

### 已知限制

- `hash` 数值与 CPython 不同（按 py23 字典的 mix64/FNV），只保证同值同
  哈希；tuple/None 暂不支持
- `hypot`、`isclose` 固定两参；`prod`/`sumprod` 只接受 `list[int]`；
  `dist` 只接受等长 `list[float]`
- math 整数函数按 int64 回绕/截断，不做大整数

## [0.52] - 2026-11-10

内置函数补齐（三）：`map` / `filter`、`callable` / `issubclass`、
list/tuple 打印、`UnboundLocalError` 与常用 math 函数。

### 新增

- `map(f, xs)` / `filter(f, xs)` 在脱糖阶段改写成列表推导式，复用现有
  推导式的类型检查与降级，结果立即求值；`filter(None, xs)` 保留真值元素。
  `f` 限定为名字（普通函数、内置转换或脱糖后的 lambda），模块里绑定过
  `map`/`filter` 时不做改写。
- `callable(x)` / `issubclass(A, B)`：按静态类型编译期折叠，支持本地类
  与内置类型名（含 `bool` 是 `int` 子类、一切类型是 `object` 子类）。
- list/tuple 的 `print`：按静态元素类型逐元素递归输出，字符串元素按 repr
  带引号；`print(..., file=f)` 同样适用。此前 list 会被当成整数打印指针。
- 异常名补 `UnboundLocalError`（父类 `NameError`，运行期层级同步）。
- math 新增：`atan2` / `hypot` / `degrees` / `radians` / `trunc` /
  `copysign` / `isnan` / `isinf` / `isfinite` / `factorial` / `gcd` /
  `lcm` / `fsum`，以及常量 `inf` / `nan`（`hypot` 固定两参）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：map/filter（lambda、命名函数、内置转换、嵌套）、
  callable/issubclass、list/tuple 打印（含 file=）、math 各函数、
  UnboundLocalError 捕获均与 CPython 一致（O0/O1/O2）

### 已知限制

- `map`/`filter` 的 `f` 不能是任意表达式（如属性访问），否则报 codegen
  不支持；同一模块里先绑定 `map` 再调用会按用户函数处理
- `math.hypot` 只支持两个参数；`math.fsum` 只接受 list[float]
- dict/set 的 print 仍是占位符

## [0.51] - 2026-11-10

重构：拆分上帝文件，无行为变化。

### 结构

- `lower_expr.cpp` 拆出 `lower_comp.cpp`（推导式）
- `lower_call.cpp` 拆出 `lower_builtin.cpp`（内置分派）、`lower_aggregate.cpp`
  （min/max/sum/any/all）、`lower_method.cpp`（方法/super）、
  `lower_print.cpp`（print 与 `print_value`，后者来自 lower_expr）
- `lower_exc.cpp` 拆出 `lower_import.cpp`、`lower_match.cpp`
- `typecheck_call.cpp` 拆出 `typecheck_builtin.cpp`
- `opt.cpp` 拆为 `opt_fold.cpp` / `opt_inline.cpp` / `opt_loop.cpp` +
  `opt_detail.h`，驱动只留调度
- `rt_obj.c` 按职责拆成 `rt_str.c` / `rt_list.c` / `rt_dict.c` /
  `rt_slice.c` / `rt_class.c` / `rt_math.c` / `rt_complex.c` /
  `rt_format.c` / `rt_file.c`，CMake 运行时 parts 同步
- `main.cpp` 拆出 `driver_util.*`（文件/诊断/临时文件）与
  `driver_build.*`（多模块构建、缓存、汇编、链接）

### 验证

- 每步单独构建 + `ctest --preset dev`
- 全部完成后 dev×3 / release 全绿，810 程序差分零失败，O0/O1 全量
  52 个运行用例通过
- 最大源文件从 3059 行降到 1143 行（`lower_stmt.cpp`）

## [0.50] - 2026-11-10

补内置函数：`isinstance` / `getattr` / `setattr` / `hasattr`。

### 新增

- `isinstance(x, T)`：目标支持内置类型名（int/float/str/bool/list/tuple/
  dict/set/complex/object）和本地类。静态类型确定时编译期折叠；unknown
  值上 str/list/dict/complex 走运行期类型标签检查，类走 MRO 检查
  （`py23_isinstance`）；Optional 值与父类/子类关系走运行时，避免把
  None 误判成实例；导入类目标明确报不支持。
- `getattr` / `setattr` / `hasattr` 支持类实例，属性名必须是字符串
  字面量：字段读写与 property 读取复用普通属性访问的降级路径；property
  setter 暂不支持。`hasattr` 对字段/方法/property 在编译期给出
  True/False（保留对象求值副作用）。
- 类型检查同步校验参数个数、字面量属性名、目标类型和字段存在性。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：字段/方法/property 的 hasattr/getattr、setattr、
  基类/子类/无关类、内置类型、unknown 值与 Optional 的 isinstance 均与
  CPython 一致（O0/O1/O2）

## [0.49] - 2026-11-10

文件 I/O 编码层：任意 `encoding=`（iconv）、`errors=` 策略、精确通用换行与
文本逻辑定位。

### 新增

- 文本模式的读写统一走 glibc iconv：源编码 ↔ UTF-8。编码名做候选解析
  （原样、去 `-`/`_`/空格转大写、常见别名），glibc 不认的 `latin-1`
  也能映射到 `ISO-8859-1`；未知编码抛 `LookupError`。
- `errors=` 支持 `strict` / `ignore` / `replace`：解码失败抛
  `UnicodeDecodeError`（replace 输出 U+FFFD），编码失败抛
  `UnicodeEncodeError`（replace 输出 `?`）。
- 通用换行改为在解码后的缓冲上翻译，`\r\n` 跨读块（含逐字节
  `read(1)`）也能正确折成一个 `\n`；EOF 时挂起的 `\r` 补发 `\n`。
- 解码缓冲会预读整块，`tell()` 在文本模式下改为记录解码后的逻辑位置，
  `seek()` 接受同样的值（从文件头重解码定位）；写入前若缓冲里有预读
  字节，先把底层句柄退回逻辑位置，`r+` 下 `seek` 后 `write` 与 CPython
  一致。
- 文件属性新增 `encoding` / `errors`。

### 修复

- `truncate()` 无参时和 CPython 一样使用底层缓冲位置（文本预读后即原始
  偏移）；显式 `size` 会先把逻辑位置对齐到底层句柄再截断。
- `seekable()` 改为探测 `fseek` 是否成功：管道上的 stdin 返回 `False`。
- `runtime/rt_internal.h` 的 `exc_longjmp` 补上 `_Noreturn`，消除
  `py23_raise_c` 的 “noreturn function does return” 警告。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：latin-1 写读字节数/文本、utf-16 写读、未知编码、
  strict/replace、跨块 `\r\n`、`r+` 读写混用均与 CPython 一致
  （O0/O1/O2）

### 已知限制

- 文本 `tell()` 的位置按解码后的 UTF-8 字节计；非 UTF-8 源编码（如
  latin-1）与 CPython 的原始字节 cookie 数值不同，但 `tell`/`seek`
  自身自洽
- 文本 `seek` 是 O(n)（重解码到目标位置）
- 仍无 `sys.stdout.buffer`、`reconfigure()`、`opener=`

## [0.48] - 2026-11-10

文件 I/O 修正：独占创建模式与 `input()` 的 EOF 语义。

### 修复

- `open(path, "x")` 生成的 C 模式拼成了 `"xx"`（glibc 视为非法，必然
  OSError）；改为 `"wx"`（读写加 `+`）。独占创建、已存在报错的行为与
  CPython 一致。
- `input()` 在 stdin 立即 EOF 时抛 `EOFError`（以前返回空串）；读到部分
  行但没有换行仍按 CPython 返回该行。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：`x` 模式、`seek(whence=2)`、`truncate`、
  `input()` EOF 与 CPython 一致（O0/O1/O2）

## [0.47] - 2026-11-10

文件 I/O 续：标准流对象与 `iter(f)` / `next(f)`。

### 新增

- `sys.stdin` / `sys.stdout` / `sys.stderr` 现在是文件对象：直接包住 C
  标准流（不 dup，和 `print` 共用一个 `FILE*`，缓冲顺序一致），对象标记
  为永生，进程退出时不析构；`close()` 会像 CPython 一样关掉底层流。
  支持 `sys.stdout.write(...)`、`print(..., file=sys.stderr)`、
  `for line in sys.stdin:`、`sys.stdin.readline()` 等写法。
- `iter(f)` 返回文件本身；`next(f)` 读下一行，EOF 抛 `StopIteration`。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：stdout/stderr 写入与刷新顺序、stdin 逐行读取 +
  迭代、`next(f)`/`StopIteration` 与 CPython 一致（O0/O1/O2）

### 已知限制

- 标准流包装的是 C 标准流：没有 `buffer`/`reconfigure`；关闭
  `sys.stdout` 后普通 `print` 也会失败（CPython 同样）
- `input()` 与 `sys.stdin` 共用同一个 C 输入缓冲，混用时分界以底层
  stdio 缓冲为准

## [0.46] - 2026-11-10

文件 I/O 细化：`open()` 参数全覆盖、文件方法/属性、按行迭代、`print(file=)`。

### 新增

- `open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None,
  closefd=True)`：位置/关键字按 CPython 的名字绑定；`file` 支持路径或 fd
  （`closefd=False` 时 dup）；`mode` 校验 `r/w/a/x` 与 `+`/`b`/`t` 组合；
  `buffering` 0/1/>1 映射 `setvbuf`（文本模式不允许 0）；`encoding` 只接受
  utf-8/ascii 字面量；`newline` 支持 None/''/`\n`/`\r`/`\r\n`（None 读时
  通用换行，`\r`/`\r\n` 写时翻译）；`opener=` 明确拒绝。
- 文件方法：`read([n])` / `readline([size])` / `readlines([hint])` /
  `write` / `writelines` / `seek` / `tell` / `truncate` / `fileno` /
  `isatty` / `readable` / `writable` / `seekable` / `flush` / `close`；
  属性 `closed` / `name` / `mode`。
- `for line in f:` 按行迭代；`list(f)` / `sorted(f)` / `enumerate(f)` /
  `zip(f)` 走 readlines 语义。
- `print(..., file=f[, flush=True])`：值按 print 的类型规则转成字符串后
  写入文件，sep/end 同样生效。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 52 个运行用例通过；
  810 程序差分零失败
- 临时探针（不入库）：mode/buffering/newline/fd/closefd、文件方法、属性、
  按行迭代、`print(file=)` 与 CPython 逐字节一致（O0/O1/O2）

### 已知限制

- `encoding` 仅 utf-8/ascii（字节串模型）；`opener=` 不支持
- `seek`/`tell` 是原始字节位置；`read(n)` 的通用换行转换按单次读取缓冲区
  进行，跨块 `\r\n` 边界可能不折叠
- `sys.stdin`/`sys.stdout` 对象仍未建模（`input()` 直接读 stdin）

## [0.45] - 2026-11-10

`open()` 的 `encoding` / `errors` 兼容参数。

### 新增

- `open(path, mode, encoding=..., errors=...)` 现在被接受：py23 字符串是
  字节串，文件读写本身就是原始字节，所以 `encoding="utf-8"` / `"utf8"` /
  `"ascii"` 是恒等变换；其它编码（如 `utf-16`）在类型检查阶段明确报错，
  不会静默按字节读出一堆乱码。
- `errors=` 接受但忽略（字节语义下没有解码错误）。
- 开发期验证：encoding 用法与不支持的编码报错均已覆盖。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 57 个运行用例通过；
  810 程序差分零失败

## [0.44] - 2026-11-10

标准输入 `input()`。

### 新增

- `input([prompt])`：先打印提示（不带换行），从 stdin 读一行并去掉行尾
  换行/回车，返回 str；EOF 返回空串（CPython 抛 `EOFError`）。
- 与文件 I/O 一起补齐了「读输入 / 写文件」的基本闭环。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 57 个运行用例通过；
  810 程序差分零失败
- stdin 管道手工验证：提示语和无提示两种用法与 CPython 输出一致

### 已知限制

- EOF 返回空串而不是 `EOFError`；没有 `sys.stdin` 对象

## [0.43] - 2026-11-10

文件输入输出。

### 新增

- `open(path[, mode])` 返回文件对象（新增 `file` 类型）：
  `read([n])` / `readline()` / `readlines()` / `write(str)` / `close()` /
  `flush()`；`with open(...) as f:` 在正常退出、异常展开、`return`、
  `break`/`continue` 路径都会关闭文件。
- 运行时 `Py23File` 由 RC/GC 管理，析构时兜底 `fclose`；打开失败抛
  `OSError`，关闭后读写抛 `ValueError`。
- 跨模块签名里的 `file` 类型暂时降级为 unknown。
- 开发期验证：读写、追加、缺失文件异常以及 with 的 return/break/异常
  清理路径（O0/O1/O2 与 CPython 一致）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 57 个运行用例通过；
  810 程序差分零失败

### 已知限制

- `print(..., file=f)`、`seek`/`tell`/`closed` 属性、`for line in f`、
  `encoding=`/文本模式参数未支持（字节串模型）
- 文件对象只能通过 `with` 使用其上下文协议；直接调 `__enter__`/`__exit__`
  会明确报错

## [0.42] - 2026-11-10

内置函数补齐（四）：字符串方法。

### 新增

- 字符串方法（按字节串语义，ASCII 大小写）：
  `upper` / `lower` / `casefold` / `capitalize` / `title`；
  `strip` / `lstrip` / `rstrip`（可选字符集）；`split(sep[, maxsplit])`、
  无参 `split()` 按空白、`splitlines`；`join`；`replace(old, new[, count])`；
  `find` / `rfind` / `index`（带 start/end）；`startswith` / `endswith`；
  `count`；`zfill`；`ljust` / `rjust` / `center`（可填充字符）；
  `removeprefix` / `removesuffix`；
  `isdigit` / `isalpha` / `isspace` / `isupper` / `islower` / `isalnum`。
- 运行时不加几十个 RtFn，而是用一个派发入口
  `py23_str_method(self, op, a0, a1, a2)`，操作码与 `StrOp` 枚举同序。
- 开发期验证：18 行输出与 CPython 逐字节一致。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 56 个运行用例通过；
  810 程序差分零失败

### 已知限制

- 非 ASCII 的大小写/字符类别判断按字节处理（语言整体是字节串）
- `str.format`、`encode`、`partition/rpartition`、`expandtabs`、
  `translate`、`isnumeric/isdecimal/isidentifier/isprintable/istitle`
  未实现（降级阶段明确报错）

## [0.41] - 2026-11-10

内置函数补齐（三）：list / dict / tuple 方法。

### 新增

- list：`extend` / `insert` / `remove` / `pop(i)` / `clear` / `index` /
  `count` / `copy` / `reverse` / `sort`（int/float/str 三种比较器）；
  原有 `append` / `pop()` 保留。
- dict：`setdefault` / `copy`。
- set：`copy`。
- tuple：`count` / `index`（tuple 与 list 表示相同，共用运行时）。
- int / bool：`bit_length`。
- 运行时新增 list_reverse / int_bit_length；ListIndex / ListCount /
  ListInsert / ListSort 接入降级。
- 开发期验证：容器方法输出与 CPython 一致；另做 2000 轮字符串容器 RC
  压测（O0/O1/O2）。

### 验证

- `ctest --preset dev/release` 全绿；O0/O1 全量 55 个运行用例通过；
  810 程序差分零失败

### 已知限制

- `list.sort(key=..., reverse=...)`、set 的 union/intersection/difference/
  issubset 等集合运算尚未实现（明确报错）
- `dict.popitem` 固定 LIFO、`set.pop` 固定取最后插入项（既有语义）

## 更早版本

0.40 及更早的记录已归档到 [docs/archive/changelog-0.40.md](docs/archive/changelog-0.40.md)。
