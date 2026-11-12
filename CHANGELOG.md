# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.95] - 2026-11-12

`subprocess.Popen`、管道与 `shell=True`。

### 新增

- **`subprocess.Popen(args, shell=, cwd=, stdin=, stdout=, stderr=)`**：新值类型
  `TyKind::Popen`。流参数取 `subprocess.PIPE` / `DEVNULL` / `STDOUT`
  （`STDOUT` 只对 `stderr` 有意义）。方法 `wait(timeout=…)` /
  `communicate(input=…)` / `kill()` / `terminate()` / `send_signal(n)`，
  属性 `pid` / `returncode` / `stdout` / `stderr` / `stdin`（后三个是懒创建的
  `file` 对象，直接复用 `py23_file_open(NULL, fd, …)`）。
- **`subprocess.run` 支持 `shell=True` 与 `cwd=`**，`args` 也可以是字符串。
- **`subprocess.PIPE` / `STDOUT` / `DEVNULL`** 常量，取值与 CPython 一致
  （-1 / -2 / -3）。
- **`from subprocess import Popen`** 可用（`module_ctor` 白名单 + 别名）。

### 修复

- **`x is None` 在字符串/容器上段错误**。`map_cmp` 把 `Is`/`IsNot` 映射成
  `Eq`/`Ne`，而 `gen_cmp` 对 str 会调 `py23_str_eq` 解引用两个操作数，`None`
  就是 0，直接崩。改成：`is` / `is not` 先走 `Mov` 把堆对象转成整数位模式，
  再做整数比较——语义上就是比指针，和 CPython 的同一性比较一致。
- `subprocess.run` 的 `cwd` 之前是「未实现」，现在真正 `chdir`。

### 尚未支持

- **没有 `poll()`**：py23 里 `None` 和 `0` 同形，`poll() is None` 会把正常退出
  （rc=0）误判成还在跑。要非阻塞轮询用 `wait(timeout=…)` 捕获 `TimeoutExpired`。
- `communicate()` 对没开管道的流返回空串（CPython 是 `None`）；`env` /
  `preexec_fn` / `input=` 之于 `run` 未实现。
- `Popen` 句柄被引用计数回收时会 SIGKILL 子进程——CPython 不会这么做，但
  py23 的释放时机不可预期，留着僵尸进程更糟。

## [0.94] - 2026-11-12

`collections.deque` 与 pathlib 补完。

### 新增

- **`collections.deque([iterable[, maxlen]])`**：环形缓冲实现，`append` /
  `appendleft` / `pop` / `popleft` / `rotate` / `clear` / `extend` /
  `extendleft`，加 `len` / 下标 / 迭代 / `list(d)`。`maxlen` 满了从另一端
  丢最旧的，与 CPython 一致。新值类型 `TyKind::Deque`。
- **`pathlib` 的 `iterdir()` / `glob(pattern)` / `mkdir(mode, parents, exist_ok)`**。
  `glob` 支持 `*` / `?` / `**` 递归（不支持字符类）；`mkdir` 按 CPython 语义
  只收关键字参数。
- **`from <模块> import <构造器>`**：`from pathlib import Path`、
  `from collections import deque, OrderedDict, Counter`、
  `from datetime import date, datetime` 现在都能用。之前只支持
  `import pathlib; pathlib.Path(...)` 的写法。

### 修复

- deque 构造时沿用列表的 `elem_kind`（空列表是保守值 2），把 int 元素当堆
  指针执行 `rc_dec`，直接崩溃。改成只有元素类型明确是堆对象时才增减引用。

## [0.93] - 2026-11-12

`collections.OrderedDict`。

### 新增

- **`collections.OrderedDict()`**：py23 的 dict 本来就按插入序保存，这里直接当
  空 dict 用——`len` / 下标读写 / `keys` / `items` 等全套 dict 操作都能用。
  不支持带初始值的构造。

### 尚未支持

- `defaultdict`：要在 `Py23Dict` 上加 factory 字段，并在下标读取的缺省路径里
  调用它，改动面比表面大。
- `deque`：需要新的值类型和环形缓冲。
- `namedtuple`：要在运行期构造类，py23 的类是编译期展开的。

### 测试

- `tests/run/83_collections_extra.py`：OrderedDict 的读写与长度、Counter 计数。
- 用例数：运行 82→83。

## [0.92] - 2026-11-12

`json.dumps` 支持 `indent=` / `sort_keys=`。

### 新增

- **`json.dumps(obj, indent=n, sort_keys=True)`**：`indent` 控制缩进（缺省或 0 是
  紧凑输出），`sort_keys` 按 key 字符串排序。嵌套容器逐层缩进，输出与 CPython
  的 `json.dumps` 逐行一致。
- 序列化仍然按静态类型描述符机械遍历，所以 dict 的 value 类型必须一致——
  `{"a": [1], "b": {"x": 1}}` 这种混合 value 推不出静态类型，会报
  "json needs a statically known type"。

### 测试

- `tests/run/82_json_dump_opts.py`：紧凑 / indent / sort_keys / 嵌套。
- 用例数：运行 81→82。

## [0.91] - 2026-11-12

`os` / `os.path` 补齐常用项。

### 新增

- **`os.makedirs(path[, exist_ok])`**：逐级创建；中间层已存在直接跳过，
  最后一级已存在时只有 `exist_ok` 为真才不报错。
- **`os.getpid()` / `os.chdir(path)` / `os.chmod(path, mode)` / `os.access(path, mode)`**。
- **`os.path.realpath` / `getsize` / `getmtime` / `split` / `samefile`**：
  `split` 返回 `(head, tail)`，`samefile` 比较 `st_dev` + `st_ino`。
- **常量 `os.sep` / `os.linesep` / `os.F_OK` / `R_OK` / `W_OK` / `X_OK`**。

### 测试

- `tests/run/81_os_extra.py`：目录创建（含 exist_ok）、大小 / 修改时间、
  拆分 / 真实路径 / 同一文件判定、chmod + access、chdir。
- 用例数：运行 80→81。

## [0.90] - 2026-11-12

`random` 补齐常用函数。

### 新增

- **`random.randrange([start, ]stop[, step])`**：1~3 个位置参数，负步长按方向取。
- **`random.getrandbits(k)`**：`0..2^k-1` 的整数（k > 62 截到 62，避免溢出）。
- **`random.gauss(mu, sigma)`**：Box-Muller。
- **`random.expovariate(lambd)`**：反变换采样。
- **`random.triangular(low, high[, mode])`**：mode 缺省取中点。
- **`random.sample(seq, k)`**：无放回（复制后部分洗牌），k 超过长度报 ValueError。
- **`random.choices(seq, k=n)`**：有放回，`k` 是关键字参数（和 CPython 一致），缺省 1。

### 说明

- 序列不保证与 CPython 一致：MT19937 的 `random()` 序列相同，但 `gauss` /
  `expovariate` 用的是不同算法（CPython 的 gauss 会缓存成对的第二个值）。
- 这几个函数的运行时入口都带一个 rng 首参，模块级调用传 0 走全局 rng。

### 测试

- `tests/run/80_random_extra.py`：范围 / 均值 / 去重性质，与 CPython 输出逐行一致。
- 用例数：运行 79→80。

## [0.89] - 2026-11-12

`datetime` 与 `pathlib`。

### 新增

- **`datetime`**：`date(y, m, d)` / `datetime(y, m, d[, h, mi, s])` 构造，
  `now()` / `today()` 取当前时间；`.year` / `.month` / `.day` / `.hour` /
  `.minute` / `.second` / `.weekday()` 读字段，`.isoformat()` / `.strftime(fmt)`
  格式化。`print()` 按 ISO 格式输出，构造时校验字段范围。
- **`pathlib.Path`**：`Path(s)` 构造，`.name` / `.stem` / `.suffix` / `.parent`
  取路径成分，`.exists()` / `.is_file()` / `.is_dir()` 查文件系统，
  `.read_text()` / `.write_text(s)` 读写文本。`print(p)` 直接输出路径。

### 已知限制

- 只支持模块级函数写法（`datetime.now()`），不支持 `datetime.datetime.now()`
  这种链式类方法。
- 没有 `timedelta`、时区、`fromtimestamp`。
- `Path` 没有 `/` 运算符、`iterdir` / `glob` / `mkdir`。
- `str(Path)` 还没接（`print(p)` 可以）。

### 测试

- `tests/run/79_datetime_pathlib.py`：日期构造 / 字段 / ISO / strftime /
  weekday / now，路径成分 / 存在性 / 读写。
- 用例数：运行 78→79。

## [0.88] - 2026-11-12

`for` 里的 `enumerate` / `zip` 脱糖成计数循环。

### 修复

- **`for i, v in enumerate(xs)` 不再建 `[下标, 值]` 的元组表**：降级时直接在
  `lower_for_seq` 里识别这个调用形状，走「计数循环 + 按下标取元素」，两个循环
  变量分别赋值。每个元素省掉一次堆分配（`py23_enumerate_list` 里的
  `py23_list_new(2, ...)`）。
- **`for a, b in zip(xs, ys)` 同理**：双游标，长度取两者较小值（运行期比较），
  不再建元组表。
- 覆盖面：List / Tuple / Str / Dict / Set 作为源。Generator / Range / 用户类
  仍然走原来的物化路径；`for i, (a, b) in ...` 这种嵌套解构也退回物化。

### 语义

脱糖后 `enumerate` / `zip` 在 `for` 里是**活视图**：循环体里改列表会看到新元素
（以前是先建整张表，属于快照语义）。这和 CPython 的惰性迭代器一致，也和
`for x in xs` 的现有行为一致。

### 性能

`bench/builtins/`：`34_enumerate` **0.7× → 30.9×**（403.9ms → 9.4ms）、
`35_zip` **0.9× → 43.2×**（407.5ms → 9.2ms），N=100000，对手 CPython 275.7 / 372.2ms。
两个用例的「线性」检查都通过，不是被优化器消掉了。

### 已知限制

- `e = enumerate(xs)` / `list(zip(a, b))` 当普通值用仍然物化成列表——要 `len`
  和下标就得是真列表，这部分没变。
- 同一个循环变量在不同位置用不同类型时（`for i, v in enumerate(ints)` 之后
  `for i, v in enumerate("abc")`），第二次的 `print(v)` 会按第一次的类型打印。
  这是 lower 按名字取最终类型的老问题，和这次改动无关；换变量名即可。

## [0.87] - 2026-11-12

`sorted` / `list.sort()` 从 qsort 换成 Timsort。

### 修复

- **排序换成 Timsort**：原来用 `qsort` + 比较函数指针，两个问题——每次比较
  一次间接调用；完全没有自适应（近乎有序的数据照样跑 n log n）。现在按
  `kind` 生成三份内联比较的实例（int/float/str），入口先找自然 run（降序的
  就地反转），短 run 用二分插入排序补到 minrun，再按栈不变式归并相邻 run。
  近乎有序的数据基本不触发归并，退化成 O(n)。
- **排序稳定了**：Timsort 保证相等元素保持原相对顺序，和 Python 的
  `sorted` / `list.sort` 一致；原来的 `qsort` 不稳定。

### 性能

`bench/builtins/32_sorted.py`：**0.8× → 1.1×**（365.7ms → 285.8ms，N=674877）。
对手 CPython 的 `sorted` 本身就是 C 实现的 Timsort，所以这个用例的上限就是
「打平」，不会像 `sum` / `len` 那样 20×+。

### 已知限制

- `sorted(xs, key=f)` 走的是下标数组 + qsort（`py23_list_sorted_by`），没跟着
  换成 Timsort，稳定性同样没有保证。

## [0.86] - 2026-11-12

小内置（`slice` / `vars` / `dir`）与 `functools.reduce` / `wraps`。

### 新增

- **`slice(a, b, c)` 作下标**：脱糖阶段把 `xs[slice(a, b, c)]` 折成切片语法
  `xs[a:b:c]`，读取、切片赋值、`del` 三条路径都通；`slice(3)`（只有 stop）
  和 `slice(None, 2)`（`None` 表示不设限）也对齐 CPython。当独立值用会明确
  报错——py23 没有运行期的 slice 对象。
- **`vars(obj)`**：返回实例字段的 dict（含继承来的字段，key 是字段名）。
  value 统一是 `unknown`：字段类型各不相同，dict 装不下异构的具体类型。
- **`dir(obj)`**：字段名 + 方法名 + 类属性名 + property 名，排序去重；
  `dir()` 无参返回当前作用域的局部名（模块级是全局名）。
- **`functools` 模块**：`reduce(f, xs[, init])` 折叠——int/str 走整数跳板、
  float 走 xmm 跳板（新增 `py23_call2` / `py23_call2_f` 两个汇编跳板）；
  `wraps(f)` 只作装饰器用，装饰器链里整层跳过（py23 不建模
  `__name__`/`__doc__`）。
- **嵌套 lambda 也参与形参回填**：`__py23$lambda_*` 和生成器合成函数一样
  登记重查，重查时回到定义时的词法父作用域。`reduce(lambda a, b: a + b,
  [1.5, 2.5])` 靠这条把形参回填成 float，否则 float 元素会按整数寄存器传参。

### 修复

- 切片里的 `None` 表示「这一端不设限」：`xs[None:3]` / `xs[-2:None]` 之前
  会把 None 当成 0。

### 已知限制

- `functools.partial` / `lru_cache` 需要运行期构造闭包（合成转发函数 +
  填 env），当前架构没有这个能力，明确报 `not supported yet`。
- `reduce` 的 float 元素要求 f 的形参是 float（或由调用点回填）；同一个
  函数被不同类型的 reduce 用过会退回 unknown，float 场景会算错——这是
  py23 单态化的固有限制。
- `vars()` 无参（locals）和 `dir(模块)` 不支持。
- `slice()` 没有运行期对象，只能作下标。

### 测试

- `tests/run/76_small_builtins.py`：slice 的读 / 写 / 删、`None` 边界、
  vars 的字段、dir 的三种形态。
- `tests/run/77_functools.py`：reduce 的 int / float / str / tuple / lambda /
  init / 空序列 TypeError，wraps 的两种导入方式。
- 用例数：运行 75→77。

## [0.85] - 2026-11-11

元组的 repr 与单元素元组的打印补齐；容器转标量不再静默给垃圾值。

### 修复

- **单元素元组带尾逗号**：`(1,)` 之前打成 `(1)`，CPython 是 `(1,)`。打印和
  `repr` 都对齐了。
- **`repr(元组)` 支持了**：`(1, 2)` / `(1,)` / `()` / 字符串元组都和 CPython
  一致；`str(元组)` 与 `repr(元组)` 相同。
- **容器转标量不再静默给垃圾值**：`int([1, 2])` / `str({})` 以前把容器指针
  当数字用，打出来是一串乱码（CPython 抛 TypeError）。现在明确报错。

### 已知限制

- **异构元组的 `repr()` 还不支持**：元素类型不同的元组（比如 int 和 str 混着）
  报错，不给错值。

### 测试

- `tests/run/75_tuple_repr.py`：元组的打印 / `repr` / `str`，以及容器真值判断，
  输出与 CPython 逐字节一致。
- `tests/fail/f40_int_list.py`：容器转标量明确报错。
- 用例数：反例 36→37，运行 74→75。

### 文档

- `docs/language.md`：`repr` 的支持范围补上元组；「会被拒绝的写法」补容器转标量
  和异构元组的 `repr()`。

## [0.84] - 2026-11-11

`range` 根治：不再冒充 list，变成真正的惰性对象。

### 修复

- **`range(...)` 是惰性对象了**。以前 `range` 作为值使用会实体化成整数
  列表：`r = range(10**9)` 就是当场造 10 亿个元素、8GB 内存，`len(range(n))` /
  `range(n)[i]` / `sum(range(n))` 这些也都是列表语义。现在 `range` 有自己的
  运行时表示（只存起点、终点、步长）和静态类型 `range[int]`：
  - `len(r)` / `r[i]`（含负下标）/ `x in r` / `for x in r` / 推导式都不物化，
    `range(10**9)` 只占几十字节；
  - `sum` / `min` / `max` / `any` / `all` 按边界流式算，不建中间列表；
  - `list(r)` / `tuple(r)` / `set(r)` / `sorted(r)` / `reversed(r)` /
    `enumerate(r)` / `zip(r, ...)` 需要真列表时才物化；
  - `repr(r)` 是 `"range(0, 10)"`，和 CPython 一样。
- **生成器表达式的 range 源不再物化**（上一版只改了脱糖这一条路）：
  `(x for x in range(10**9))` 既不建列表也不建中间结果，第一次 `next()` 就出 0。
- **`range()` 的动态步长现在支持了**：`for x in range(0, 10, n)` 和
  `(x for x in range(0, 10, n))` 都能跑（以前报「step must be an integer
  literal」）。方向在运行期定，步长为 0 抛 ValueError。
- **错误措辞按 CPython 对齐**：`min()/max()` 空序列改成
  `min() iterable argument is empty`（list 与 range 两条路径都改）；
  不可变对象的赋值/切片赋值改成 `'tuple' object does not support item
  assignment` 这种 CPython 原话，类型名不带参数。

### 已知限制

- **range 不可变**：`r[0] = 1` 在 py23 是编译期报错，CPython 是运行期 TypeError
  （消息一致）。
- **动态步长为 0 时 ValueError 抛得比 CPython 晚**：CPython 在
  `range(0, 10, 0)` 求值时抛，py23 在第一次 `next()` 时抛（字面量步长 0
  仍然在创建时抛）。

### 测试

- `tests/run/74_range_object.py`：len / 下标（含负下标）/ 成员 / 归约 / 物化 /
  repr / 相等 / 迭代 / 推导式 / 枚举 / zip，以及 10**9 的 range 不物化，
  输出与 CPython 逐字节一致。
- `tests/fail/f39_range_assign.py`：range 不可变，编译期就报错。
- 差分测试新增 `range` 生成器（边界、下标、成员、归约、物化、推导式），
  默认种子下零差异。
- 用例数：反例 35→36，运行 73→74。

### 文档

- `docs/language.md`：类型表加 `range[int]`；内置函数那节补 range 对象的
  语义（惰性、不可变、repr）。
- `docs/compatibility.md`：「range 作为值仍然会实体化」改成「range 是惰性对象」，
  说清还剩的两处差异；语义差异表加一行 range。

## [0.83] - 2026-11-11

生成器表达式的 range 源改成真惰性。

### 修复

- **生成器表达式的 range 源不再实体化**。以前 `(x for x in range(10**9))`
  会先造出 10 亿个元素的整数列表（8GB），生成器形同虚设；现在脱糖时把
  边界值传进合成生成器函数，用计数循环逐个产出——第一次 `next()` 就出 0，
  内存是常数。`range` 作为**普通值**使用仍然会实体化，见下面的已知限制。
- **`range()` 的动态步长现在支持了**：`for x in range(0, 10, n)` 和
  `(x for x in range(0, 10, n))` 都能跑（以前报「step must be an integer
  literal」）。方向在运行期定，步长为 0 抛 ValueError。

### 已知限制

- **range 作为值仍然会实体化**：`r = range(10**9)` 还是当场造 10 亿个元素
  （CPython 的 range 是惰性对象）。把 range 做成惰性对象要动值的表示
  （现在 range 和 list 同一套静态类型），先记在
  [compatibility.md](docs/compatibility.md) 里。
- **动态步长为 0 时 ValueError 抛得比 CPython 晚**：CPython 在
  `range(0, 10, 0)` 求值时抛，py23 在第一次 `next()` 时抛（字面量步长 0
  仍然在创建时抛）。

### 测试

- `tests/run/73_lazy_gen_range.py`：10^9 的 range 源只取前三个值、字面量步长
  （正负）、动态步长（含负步长）、过滤、多层生成器、sum/max 消费、步长 0 的
  ValueError、动态步长的 for 循环，输出与 CPython 逐字节一致。
- 差分测试的 `gen` 生成器新增生成器表达式分支（range 源 + 动态步长 +
  过滤），默认种子下零差异。
- 用例数：运行 72→73。

### 文档

- `docs/language.md`：生成器表达式那节补「range 源也是惰性的」和
  「range 作为普通值仍会实体化」。
- `docs/compatibility.md`：「生成器表达式的 range 源会被实体化」改成
  「range 作为值仍然会实体化」，说清剩下的坑是哪一个。

## [0.82.5] - 2026-11-11

给内置函数加了一套逐个对比的性能 bench，顺手挖出三个比 CPython 慢的内置，
还有一个 `range` 被实体化的坑。

### 新增

- **`bench/`**：逐个内置函数的 py23 / CPython 对比设施。驱动是
  `bench/run_bench.py`，用例在 `bench/builtins/<序号>_<内置名>.py`（50 个）。
  驱动会按 CPython 的实测时间校准循环次数、取多次运行的最小值、扣掉进程
  启动开销，再用 2N 复跑一次检查「循环体有没有被优化掉」，顺带比对两边的
  标准输出。跑法见 [docs/development.md](docs/development.md#性能-bench)。
- **`bench/RESULTS.md`**：一次实测快照（机器相关，别当基准线）。

### 已知限制

- **`enumerate` / `sorted` / `zip` 比 CPython 慢**（约 0.7× / 0.8× / 1.0×）：
  py23 把结果物化成列表（`enumerate` 一次造出整张 `[下标, 值]` 元组表），
  排序用 `qsort` + 比较函数指针，CPython 用惰性迭代器 + Timsort。
- **生成器表达式的 range 源会被实体化**：`(x for x in range(10**9))` 在
  py23 里当场造出 10 亿个元素的列表（CPython 的 range 是惰性的）。脱糖时
  range 作为值传进合成生成器函数，而 range 作为值使用会实体化成整数列表
  （`runtime/rt_slice.c` 的 `py23_range_new`）。实测 `range(50000000)`：
  py23 建生成器要 191ms / 400MB 内存，CPython 7ms；把地址空间限制在
  200MB 时 py23 直接 OOM。`for x in range(N)` 不受影响（for 循环走专门的
  计数循环），源是列表的生成器表达式也不受影响。

### 文档

- `docs/compatibility.md`：性能章节新增「内置函数逐个对比」和「生成器表达式
  的 range 源会被实体化」两小节。
- `docs/development.md`：测试章节新增「性能 bench」小节。

## [0.82.4] - 2026-11-11

`sorted(xs, key=f)` 之前被**静默忽略**——按 key 排和不按 key 排结果不同。

### 修复

- **`sorted(xs, key=f)` 真的按 key 排了**。做法：编译器先用 `f` 算出每个元素的
  键（一个平行列表），再让运行时按键排序。运行时按下标数组 + `qsort` 实现
  （比较函数用静态指针读键表，py23 单线程不会并发踩）。
- **其它关键字不再被静默吞掉**：`sorted(xs, foo=1)` 现在明确报错。

### 已知限制

- **`key` 必须是用户函数**。内建函数没有闭包对象，走闭包调用会跳飞——`len`
  特判成取长度，其它内建函数明确报错。
- **键的返回类型一般推不出来**（py23 不把推断出的返回类型写回函数名的类型），
  这时退到机器字比较：int 键对，两边看起来都是字符串对象时按字符串比也对，
  但**负的 float 键会排错**（IEEE 位模式在负数段是反的）。给 key 函数标注
  返回类型可以避开。

### 测试

- `tests/run/72_sorted_key.py`：key / reverse / 两者组合 / `key=len` /
  原列表不被修改，输出与 CPython 逐字节一致。
- 用例数：运行 71→72。

### 文档

- `docs/language.md`：新增「sorted 的 key」小节；内置函数那节删掉
  「`key=` / `reverse=` 尚未支持」——reverse 早在 0.79.1 就有了。

## [0.82.3] - 2026-11-11

`bytes` 补齐剩余方法。

### 新增

- **`bytes` 的方法补齐**：`split` / `find` / `rfind` / `count` / `startswith` /
  `endswith` / `join`。这六个类型检查早就放行了，只是降级侧没实现，
  `build` 会报「bytes method '...' is not supported」。`join` 是新加的
  （类型检查侧也补上了）。字节层面和 `str` 完全一致，只是结果标成 `bytes`——
  `split` 返回 `list[bytes]`，`join` 要一个 `list[bytes]`。

### 测试

- `tests/run/71_bytes_methods.py`：七个方法的输出与 CPython 逐字节一致。

### 文档

- `docs/language.md`：bytes 小节补上方法清单。
- `docs/compatibility.md`：bytes 那行的方法清单与限制。

## [0.82.2] - 2026-11-11

`d.get(k)` 缺失时返回 `None`，不再抛 `KeyError`。

### 修复

- **`d.get(k)` 缺失时返回 `None`**：之前像 `d[k]` 一样 panic `KeyError`，
  而 CPython 返回 `None`——这是最常用的写法之一，缺失键直接崩。
  静态返回类型同步改成 `Optional[值类型]`（`get(k, default)` 仍是值类型），
  这样 `if d.get(k) is not None:` 的收窄才成立。

### 已知限制

- **py23 里 `None` 就是 `0`**：所以 `d.get(k)` 拿到的值恰好是 `0` 时，
  和「键缺失」分不出来（`0 is None` 为真、`None == 0` 也是真）。这是值表示
  不带类型标签的直接后果，不是这次改动引入的——`x: int = 0` 之后
  `x is None` 同样为真。要彻底解决得动「值表示」那项待决策。

### 测试

- `tests/run/70_dict_get.py`。
- 用例数：运行 69→71。

### 文档

- `docs/compatibility.md`：改掉 `d.get` 缺失那行，补一条 `None` 与 `0`
  同表示的差异说明。

## [0.82.1] - 2026-11-11

嵌套容器也能逐层比了。

### 新增

- **同构的嵌套容器逐层比较**：0.82 只比一层，`[[1]] == [[1]]` 仍是 False
  （CPython 是 True）。现在编译器把「元素比较链」编成常量串——每个字符说明
  该层元素怎么比（`i` int / `s` str / `f` float / `x` 机器字 / `l` list /
  `d` dict / `t` set）——运行时按链递归，所以 `[[1]] == [[1]]`、
  `[[[1]]] == [[[1]]]`、`{"k": [1]} == {"k": [1]}` 都和 CPython 一致。

### 已知限制

- **元素类型不同（异构容器）时退回指针比较**：`[1, [2]] == [1, [2]]`、
  `(1, (2, 3)) == (1, (2, 3))` 仍是 False（CPython 是 True）。这两种容器的
  元素类型逐个不同，一条比较链表达不了；定长异构元组要按位置分别比，
  得再设计一套编码。

### 测试

- `tests/run/69_container_eq.py` 补上嵌套容器的用例。

## [0.82] - 2026-11-11

容器的相等与打印：不再比指针，不再只给占位符。

### 新增

- **容器逐元素相等**：`list` / `tuple` / `dict` / `set` 的 `==` / `!=` 现在
  按 CPython 语义逐元素比较，取代原来的指针比较。元素类型在编译期已知，
  比较方式也随之确定（int 按整数、str 按字节、float 按 IEEE）；`dict` 的比较
  与顺序无关（`{"a": 1, "b": 2} == {"b": 2, "a": 1}` 为真），`set` 按「每个
  元素都在对方里」比。
- **`print(d)` / `print(s)` 打印真实内容**：`{'a': 1}` / `{1, 2}`，键和值
  按 repr 语义（容器里的字符串带引号）。空集合打印成 `set()`。

### 已知限制

- 元素本身还是容器时只比指针——再往里一层要编译器展开递归（`[[1]] == [[1]]` 仍为
  False，CPython 为 True）。
- `list` 和 `tuple` 是不同类型，彼此不相等（`[1, 2] == (1, 2)` 为 False），即使
  运行时表示相同。
- 元素类型未知时（异构容器，如 `{"a": 1, "b": "x"}` 的值类型是 unknown），
  `print` 仍输出 `<dict object>` / `<set object>` 占位：运行时没有类型标签，
  分不清 float 和 int，与其打错值不如不给。
- 定长异构元组（`tuple[int, str]`）的元素类型不同，同样退化成机器字比较。

### 测试

- `tests/run/69_container_eq.py`：容器相等、dict/set 打印（期望输出与 CPython 逐字节对齐）。
- 用例数：运行 66→69。

### 文档

- `docs/language.md`：比较、print、字典与集合三处补上容器语义。
- `docs/compatibility.md`：两条语义差异行跟着改。

## [0.81.1] - 2026-11-11

`bytes` 的两个洞：相邻字面量拼接、方法的操作码。

### 修复

- **相邻 `bytes` 字面量可以隐式拼接**：`b"a" b"b"` 之前被解析器拒绝（0.79 加
  `bytes` 时只接了字面量本身，漏了隐式拼接）。现在按 CPython 语义拼接，
  `rb"r" b"w"`、多段连拼、列表元素里拼接、空串拼接都正常。
- **`bytes` 与 `str` 不能混拼**：写 `b"a" "b"` 现在报
  `cannot mix bytes and nonbytes literals`（CPython 同款消息），之前是个含糊的
  `expected ')', found 'b'`。
- **bytes 方法的操作码按方法名选**：之前 `lower_method.cpp` 的 bytes 分支把
  StrMethod 的操作码写死成 0（= `Upper`），于是 `lower` / `strip` / `lstrip` /
  `rstrip` / `replace` 全都执行成 `upper`——`b"  x  ".strip()` 返回 `b'  X  '`。
  现在照 str 分派同样的选法，结果类型标成 `bytes`。
- **`bytes.replace()` 不再崩**：旧实现把实参打包进一个 `list[str]`，bytes 实参
  塞进去触发运行时 `invalid list element kind`。现在实参按字符串语义直接传，
  第三个参数（替换次数）也支持了。

### 测试

- `tests/run/67_bytes_concat.py`：相邻 bytes 字面量拼接、`len`、`==`、
  `decode()`、列表元素里拼接、空串拼接、转义拼接。
- `tests/run/68_bytes_methods.py`：`upper` / `lower` / `strip` / `lstrip` /
  `rstrip` / `replace`（含替换次数）、`decode` / `encode`。
- `tests/fail/f38_bytes_mix.py`：`bytes` 与 `str` 混拼必须报错。

### 文档

- `docs/language.md`：新增「bytes」小节，写清字面量、拼接、方法清单和剩下的
  缺口；去掉「bytes 不能拼接」的限制。
- `docs/compatibility.md`：补 `bytes` 一行语义差异；前端基线重跑——语法失败从
  8 个降到 2 个（只剩 PEP 695 泛型语法），类型失败分类表跟着更新。
- `docs/roadmap.md`：`bytes` 收尾清单去掉「相邻字面量拼接」。
- 用例数：反例 34→35，运行 66→68。

## [0.81] - 2026-11-10

生成器表达式惰性求值。

### 新增

- **生成器表达式真正惰性**：`(elt for x in xs)` 在脱糖阶段展开成「合成
  生成器函数 + 一次调用」，最外层迭代源在创建时求值（Python 语义），
  其余部分等 `next()` 才跑。`sum(x*x for x in xs)` 不再先建整个列表。
- **内置函数的流式消费**：`sum` / `min` / `max` / `any` / `all` 逐元素
  拉取（`any` / `all` 命中即停，空序列的 `min` / `max` 照旧报
  `ValueError`）；`list` / `tuple` / `set` / `frozenset` / `sorted` /
  `reversed` / `enumerate` / `zip` / `str.join` 接受生成器实参，顺序
  抽干一次。
- **推导式的迭代源可以是生成器**：`[x for x in (y*2 for y in ys) if x > 2]`
  走流式路径，不建中间列表。
- **嵌套函数形参的单态化回填**：合成生成器函数（`__py23$gen_*`）在函数
  体内定义时也登记重查，重查时回到原来的词法父作用域——否则形参类型
  永远停在 unknown，降级层没法选迭代路径。

### 修复

- **调用点的生成器元素类型**：合成函数的返回类型在形参回填重查后才从
  `Generator[unknown]` 变成 `Generator[T]`，调用点缓存的表达式类型跟着
  刷新。否则 `min(x for x in ["bb", "a"])` 会退化成指针比较，`all` 也会
  把空串当成真值。
- `list(gen)` / `tuple(gen)` 不再多复制一份：生成器物化出来的列表已经
  是新的。
- 生成器迭代的每个元素转进槽里再由下一轮覆盖释放，长循环不再在临时栈
  上累积，短路跳出时也不会漏掉当前元素。

### 测试

- `tests/run/66_genexpr.py`：惰性时序、一次性消费、短路、闭包捕获、
  多层 `for`、嵌套生成器、内置函数消费、推导式消费、跨 `yield` 的
  `finally` 清理。
- `tests/run/19_comprehension.py` 里的 `(y + 1 for y in range(3))` 改成
  `list(...)`：它现在是生成器，没有下标。

### 已知限制

- 类体里的生成器表达式仍按 list 立即构造（类体只支持简单值）。
- 推导式内部不能写引用推导式循环变量的生成器表达式（合成函数被提到
  语句之前，那时循环变量还不存在），会报 `undefined name`。

## [0.80] - 2026-11-10

`@classmethod` 与类属性。

### 新增

- **`@classmethod`**：首参 `cls` 静态绑定到所属类，`cls(...)` 走构造、`cls.x`
  读类属性、`cls.m()` 调类方法/静态方法。继承来的类方法在子类里**重新特化
  一份**，所以 `Sub.make()` 里的 `cls(...)` 构造的是 `Sub` 而不是基类。
- **类属性**：类体里的普通赋值（`class C: x = 1`）不再是模块全局，而是存进
  模块里的隐藏全局，所有实例共享。`C.x` / `cls.x` / `self.x` 都能读；实例
  字段同名时字段优先。类体里的前向引用（`y = x + 1`）按类命名空间解析。
- **写时复制**：通过子类给继承来的类属性赋值（`Sub.x = v` 或 `cls.x = v`），
  子类从此有自己的一份，基类不受影响；赋值之前读到的仍是基类的值。用一个
  每 (类, 属性) 的「已拥有」标志位实现，读路径按标志回退到父类。
- **运行期类号派发**：`self.x` 读类属性时按实例的真实类号选择存储槽，静态
  类型是基类、值是子类时读到子类覆盖的那份。没有任何子类覆盖时退化成一次
  直接读，不产生额外开销（新增运行时 `py23_obj_class_id`）。
- **实例字段遮蔽类属性**：`self.x = ...` 且 `x` 是类属性时补出隐式字段，
  构造时先把类属性的当前值拷进字段，所以「还没赋值就读」看到的是类属性的
  默认值而不是 0 / None。

### 修复

- 类接收者的属性读取（`C.x` / `cls.x`）在类型检查阶段优先查类属性，不再被
  同名实例字段挡住类型（否则隐式字段是 unknown 时 `cls.name + "..."` 会退
  化成指针加法）。
- `obj.类属性 = v` 现在报明确错误：需要新建实例属性，而对象布局是编译期
  定死的；以前会落到「没有这个属性」的泛泛提示。

### 测试

- `tests/run/65_classattrs.py`：类属性读写、继承、写时复制、类方法特化、
  可变类属性共享、实例字段遮蔽、类体前向引用、`__match_args__`。

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
