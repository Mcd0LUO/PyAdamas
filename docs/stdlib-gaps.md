# 标准库覆盖缺口评估（任务 L）

> 结论：**51 个常用 CPython 模块中，34 个实测可用（27 个原有 + 阶段二补齐 7 个），17 个仍然缺失；0 个「能 import 但运行失败」。** 缺失模块里绝大多数卡在编译器/运行时能力上，纯 PyAdamas 源码补不了；能自举的已按阻塞概率补齐（见第五节）。
>
> 本文件是评估 + 阶段二交付记录。所有「现状」结论都来自实测，不是读源码猜的。
>
> **0.350 更新（stdlib5 分支）**：矩阵里 **pprint「缺失」的结论作废**——
> 已用纯 pa 源码自举（sort_dicts=True 默认 + indent/width/depth/compact +
> 长字符串折行，与 CPython 3.14 对拍 0 diff）。同批还补齐 textwrap 的
> TextWrapper 全参数面（break_long_words/break_on_hyphens/max_lines/
> placeholder/expand_tabs 真实生效，断词按 3.14 的 wordsep 语义）与 string 的
> Template（substitute/safe_substitute/is_valid/get_identifiers）+
> capwords(sep)。选型/边界/对拍见 CHANGELOG 0.350 与三个模块的 docstring。
>
> **0.326 更新（stdlib1 分支）**：argparse/configparser/gzip/zlib 已交付，
> 选型理由/API 覆盖/CPython 差异/桥接兜底见 [stdlib-batch1.md](stdlib-batch1.md)。
> argparse 的「Namespace 动态属性无解」结论已被推翻：0.165 的动态属性 +
> 0.326 的跨模块使能（接口导出 needs_dict）足够自举。decimal/fractions
> 深化需任意精度 BigInt、sqlite3 需外部库 + 完整游标语义，评估后仍不做。

## 一、方法与证据

探针脚本（放 /tmp，不提交）：

- `/tmp/pa_probe.sh` —— 逐模块写一个探针 .py（import + 2~3 个最常用 API），用 `build/std/pyadamas run --rebuild` 编译并运行；按三档归类：
  - **编译失败**：`pyadamas ... error:`（找不到模块 / 模块没有该属性 / 类型检查拒绝）；
  - **运行失败**：输出里出现 `pyadamas: uncaught` 或段错误；
  - **通过**：编译并运行，退出码 0。
- `/tmp/pa_extra.sh` —— 对「部分实现」的模块逐个补测常见缺 API。
- `/tmp/pa_feat*.sh` —— 语言特性可行性探针（枚举 / typing 标注 / 装饰器转发 / 上下文管理器等）。

编译环境：`cmake -S . -B build/std -G Ninja -DCMAKE_BUILD_TYPE=Debug`，`-j4`；未碰 build/dev。
探针时期源码含其它并行任务的在途改动（mach_emit 等），build/std 能正常构建。

阶段一实测汇总：**PASS 27 / 编译失败 24 / 运行失败 0**。24 个编译失败全部是找不到模块 —— 模块根本不存在，不存在「能 import 但一跑就崩」的模块。

## 二、覆盖矩阵（51 个模块）

现状分类：**原生**＝src/builtin_sigs.cpp + 降级里手写；**Python 源**＝stdlib/*.py 自举；**部分**＝能 import，但常用 API 有缺口；**缺失**＝import 直接编译失败；**受限**＝受语言/运行时能力限制，纯源码做不了。

| 模块 | 现状 | 缺口 / 说明 | 实测 |
|---|---|---|---|
| math | 原生 | 常用面基本齐（sqrt/floor/gcd/pi/isclose/fsum/comb…） | 通过 |
| sys | 原生 | 缺 `version`、`path`、`platform` | 通过 |
| os | 原生 | 缺 `walk`、`environ`、`stat`、`system`；**`os.path.x` 不可用**（要单独 `import os.path`） | 通过 |
| os.path | 原生 | join/basename/splitext/isdir/exists 可用 | 通过 |
| re | 原生 | findall/sub/search 可用；条件组/Unicode 属性类不支持 | 通过 |
| json | 原生 | **`loads` 必须给 sample**；缺 `ensure_ascii`/`default` | 通过（带 sample） |
| collections | 原生 | **缺 `namedtuple`**；Counter/OrderedDict/defaultdict/deque 可用 | 通过 |
| itertools | 原生 | 有 chain/islice/product/permutations/combinations/zip_longest/accumulate/repeat；**缺 `takewhile`/`dropwhile`/`groupby`/`starmap`/`count`/`cycle`/`tee`/`filterfalse`** | 通过 |
| functools | 原生 | reduce/wraps/partial/lru_cache 可用；**缺 `cached_property`/`total_ordering`/`singledispatch`/`cmp_to_key`/`cache`** | 通过 |
| datetime | 原生 | 构造/isoformat/字段可用；**缺 `strptime`；`datetime.datetime.now()` 链式类方法不可用** | 通过 |
| pathlib | 原生 | Path、/、name/suffix/parent、exists/read_text/write_text/iterdir/glob 可用 | 通过 |
| random | 原生 | random/seed/randint/uniform/choice/shuffle/randrange/getrandbits/gauss 可用 | 通过 |
| time | 原生 | time/monotonic/perf_counter/sleep 可用；**缺 `strftime`/`localtime`/`gmtime`/`mktime`** | 通过 |
| subprocess | 原生 | run/Popen 可用；**`from subprocess import TimeoutExpired` 失败** | 通过 |
| asyncio | 原生 | run/sleep/gather/create_task/Future/异步生成器 | 通过 |
| operator | 原生 | 算术/比较/itemgetter；attrgetter 有明确不支持报错 | 通过 |
| string | Python 源 | 常量 + capwords | 通过 |
| base64 | Python 源 | b64/urlsafe/b16/b32/encodebytes；缺 `validate=`/`altchars=` | 通过 |
| binascii | Python 源 | hexlify/unhexlify | 通过 |
| heapq | Python 源 | push/pop/heapify/nlargest/nsmallest | 通过 |
| bisect | Python 源 | 只支持 `list[int]` | 通过 |
| hashlib | Python 源 | md5/sha1/sha256 | 通过 |
| hmac | Python 源 | new/update/digest/hexdigest | 通过 |
| logging | Python 源 | 等级/Logger/basicConfig；缺 FileHandler/Formatter/handler | 通过 |
| statistics | Python 源 | 0.352：mean/fmean/median 家族/median_grouped/mode/multimode/pvariance/variance/pstdev/stdev/geometric_mean/harmonic_mean/quantiles/covariance/correlation/linear_regression；mode 多峰返回首现、multimode 首现序；float 近似（无 BigInt，精度边界见模块头） | 通过 |
| textwrap | Python 源 | **阶段二补 `TextWrapper` 类**；仍不做 exceed/break 语义 | 通过 |
| csv | Python 源 | **阶段二补 `DictReader`（函数式）/`DictWriter`**；仍无 dialect | 通过 |
| **typing** | **Python 源（桩）** | 阶段二补齐：注解 + cast/TYPE_CHECKING/TypeVar/overload 等；Generic[T]/NamedTuple/TypedDict/Protocol 不参与检查 | 阶段二通过 |
| **enum** | **Python 源（部分）** | 阶段二补齐：Enum/IntEnum/Flag/IntFlag/StrEnum + unique/verify；无 `.name`/`.value`、`Enum(1)`、迭代、auto | 阶段二通过 |
| **abc** | **Python 源（桩）** | 阶段二补齐：ABC/ABCMeta/abstractmethod 等；不拦截抽象类实例化 | 阶段二通过 |
| **collections.abc** | **Python 源（桩）** | 阶段二补齐 `stdlib/collections/abc.py`；`isinstance` 恒 False；`from collections import abc` 不支持 | 阶段二通过 |
| **uuid** | **Python 源（部分）** | 阶段二补齐：uuid4 / UUID / NAMESPACE_*；无 uuid1/3/5/version | 阶段二通过 |
| **glob** | **Python 源** | 阶段二补齐：glob/iglob/escape，`*`/`?`/`[]`/`**`（recursive） | 阶段二通过 |
| **fractions** | **Python 源（部分）** | 阶段二补齐：Fraction 四则/比较/str；无一元负号（见第六节 bug）、无 float/str 构造 | 阶段二通过 |
| **dataclasses** | **缺失** | `@dataclass` 名字被识别但 codegen 未实现（单个 `@dataclass` 报 `too many positional arguments`，`@dataclass(...)` 报 `builtin dataclass is not supported by the code generator yet`）。纯 shim 会让 P(1,2) 静默丢参（实测得到 0,0） | 编译失败 |
| **contextlib** | **缺失** | `contextmanager` 做不了：包装器要把 `*args/**kwargs` 转发给 unknown 函数值，实测 build 报 `keyword arguments require a known function signature` | 编译失败 |
| **io** | **缺失** | StringIO 可写纯类，但 `open` 写只收 str、二进制 IO 不支持，与 csv 等不互通 | 编译失败 |
| **queue** | **缺失** | 单线程版可写；没线程，语义价值低 | 编译失败 |
| **pprint** | **缺失** | unknown 容器无法内省元素，做不出有意义的 pformat | 编译失败 |
| **argparse** | **Python 源（0.326 交付）** | 跨模块动态属性使能后自举；CLI 面覆盖见 docs/stdlib-batch1.md | 通过（250–252） |
| **configparser** | **Python 源（0.326 交付）** | INI 解析/插值/错误消息逐字对齐；SectionProxy 下标等长尾不做 | 通过（253–254） |
| **gzip** | **Python 源 + C 胶水（0.326 交付）** | 系统 zlib（cimport）；compress 字节级同 CPython；GzipFile 整读整写 | 通过（255–256） |
| **zlib** | **Python 源 + C 胶水（0.326 交付）** | compress/decompress/crc32/adler32；wbits 固定 15 | 通过（255） |
| **struct** | **缺失** | 需要原生 pack/unpack；且 File 写只收 str，bytes 二进制 IO 不可用 | 编译失败 |
| **shutil** | **缺失** | 二进制 IO 不支持、rmtree 缺 walk，性价比低 | 编译失败 |
| **tempfile** | **缺失** | 需要 mkstemp/mkdtemp 原生原语（os 没有） | 编译失败 |
| **decimal** | **受限** | 整数没有任意精度（溢出回绕 int64），Decimal 无法正确实现 | 编译失败 |
| **unicodedata** | **受限** | 字符串是字节串，没有 Unicode 码点/字符表 | 编译失败 |
| **threading** | **受限** | 运行时无线程/锁原语 | 编译失败 |
| **weakref** | **受限** | 无弱引用（README：无 `__del__`/弱引用） | 编译失败 |
| **inspect** | **受限** | 无运行期反射（无 `__code__`/`__dict__`） | 编译失败 |
| **traceback** | **受限** | 无 traceback 对象（异常值只有消息） | 编译失败 |
| **pickle** | **受限** | 无反序列化/类型内省能力 | 编译失败 |
| **copy** | **受限** | copy.copy 需要运行期类型分派，unknown 值上做不了 | 编译失败 |

> 另：`linalg` 是 PyAdamas 自有模块（OpenBLAS 封装），不在 CPython 矩阵内。

## 三、实测得到的三个关键架构事实

1. **内建模块名单是硬编码的**：`src/driver_build.cpp:609-618` 的 `program_module()` 与 `:890-897` 的 `check_builtin_name()` 只认 math/sys/asyncio/random/os/os.path/subprocess/time/itertools/collections/json/re/functools/datetime/pathlib/operator。
   → **给 functools/itertools/collections 这些模块补 API 必须改 `src/`，加 `stdlib/functools.py` 会被内建名单挡住**（同名 stdlib 文件不生效）。这是任务 L「优先自举」边界的关键。
2. **注解解析器已内置 typing 名字**：`src/typecheck.cpp:240-385` 直接按名字把容器/联合/可选类型映射成内部类型，**不要求真的 import typing**。所以 `typing.py` 只需要「能 import + 提供值级名字（cast/TYPE_CHECKING/TypeVar）」就能解锁大量真实脚本。
3. **stdlib/*.py 里的模块是真正被编译的**（`set_strict_module_names`），不是解释执行：写模块时同样受类型检查约束；默认参数只能是标量字面量（list 默认值编译不过），`list()` 不能消费只有 `__iter__` 的用户类，等等。这些约束直接决定了各模块能做到什么程度。

## 四、优先级排序（按「真实脚本被它阻塞的概率」）

成本口径：**纯 Python 源**＝只加 `stdlib/*.py` + 测试；**需原生原语**＝要新 runtime 函数；**需语言特性**＝要改 `src/` 的检查/降级/代码生成。

| 序 | 模块 | 阻塞概率 | 成本 | 补齐方式 | 状态 |
|---|---|---|---|---|---|
| 1 | typing | **极高** | 纯 Python 源 | 桩模块；注解由 parse_annotation 特判 | 已交付 |
| 2 | enum | 高 | 纯 Python 源（部分） | 空基类 + 类属性常量；元类语义做不到 | 已交付 |
| 3 | abc | 中 | 纯 Python 源 | ABC 空基类 + 装饰器透传 | 已交付 |
| 4 | collections.abc | 中 | 纯 Python 源（包） | 空基类桩；isinstance 恒 False | 已交付 |
| 5 | glob | 中 | 纯 Python 源 | os.listdir + 自写 fnmatch | 已交付 |
| 6 | uuid | 中 | 纯 Python 源 | getrandbits(128) + %032x | 已交付 |
| 7 | fractions | 低-中 | 纯 Python 源 | gcd 归一化 + dunder 算术 | 已交付 |
| 8 | textwrap/statistics/csv 补全 | 中 | 纯 Python 源 | TextWrapper / quantiles / DictReader+DictWriter | 已交付 |
| 9 | io / queue | 低-中 | 纯 Python 源 | StringIO / 单线程队列 | 可做，价值有限 |
| — | dataclasses | 高 | **需语言特性**（codegen 合成 `__init__`） | 纯 shim 静默丢实参，宁可先不做 | 需 lead 决策 |
| — | contextlib | 中 | **需语言特性**（unknown 函数值 `*args` 转发） | 只能做 nullcontext 之类无用子集 | 需 lead 决策 |
| — | functools/itertools/collections 补全 | 中-高 | **需改 src**（内建名单硬编码） | 同时动 builtin_sigs + typecheck + lower | 需 lead 决策 |
| 10 | argparse | 中 | 纯 Python 源 + 前端使能（跨模块动态属性/object 形参/类型值） | 0.326 交付，见 docs/stdlib-batch1.md | 已交付 |
| 11 | configparser | 中 | 纯 Python 源 | 0.326 交付 | 已交付 |
| 12 | gzip/zlib | 中 | Python 源 + cimport zlib + C 胶水 | 0.326 交付 | 已交付 |
| — | struct/tempfile/shutil | 中-低 | **需原生原语** | rt_os/rt_file 加二进制 IO、mkstemp、stat | 暂缓 |
| — | decimal/unicodedata/threading/weakref/inspect/traceback/pickle/copy/pprint | 低 | **需运行时能力** | 见矩阵 | 暂缓 |

## 五、阶段二交付（已实现，全部走 stdlib/*.py）

新增 7 个模块：

| 文件 | 内容 | 语义边界 |
|---|---|---|
| `typing.py` | List/Dict/Optional/Union/Any/Iterable/Sequence/Callable/IO 等名字 + cast/TYPE_CHECKING/TypeVar/overload/NewType 等 | 只服务注解与 import；泛型/Protocol/NamedTuple/TypedDict 不做结构检查；值级 `List[int]` 下标不可用（编译器限制） |
| `enum.py` | Enum/IntEnum/Flag/IntFlag/StrEnum + unique/verify | 成员是类属性 int；无 name/value/Enum(1)/迭代；故意不导出 auto（避免静默错值） |
| `abc.py` | ABC/ABCMeta/abstractmethod/abstractclassmethod/abstractstaticmethod | 不拦截抽象类实例化；不能用 metaclass=ABCMeta |
| `collections/__init__.py` + `collections/abc.py` | Iterable/Iterator/Sequence/Mapping/Set/Awaitable 等空基类 | 只保 import/注解；isinstance 恒 False；`from collections import abc` 不支持（内建 collections 挡住） |
| `glob.py` | glob/iglob/escape；`*`/`?`/`[a-z]`/`[!x]`，recursive=True 支持 `**` | 结果为排序 list（iglob 也 eager）；隐藏文件规则同 CPython；不做大小写折叠 |
| `uuid.py` | UUID / uuid4 / NAMESPACE_* / getnode | 无 uuid1/uuid3/uuid5（缺时钟序列与 bytes 原语）；不做版本位校验 |
| `fractions.py` | Fraction：gcd 归一化、四则、比较、hash、str/repr、is_integer | 构造只收 int；无一元负号（见第六节）、无 float/str 构造、abs()/float() 不支持 |

补全 3 个原有 Python 源模块：

| 文件 | 新增 | 语义边界 |
|---|---|---|
| `textwrap.py` | `TextWrapper` 类（属性 + wrap/fill） | break_long_words/break_on_hyphens 只保存不生效 |
| `statistics.py` | `quantiles(data, n=4, method=exclusive 或 inclusive)` | 插值公式与 CPython 一致 |
| `csv.py` | `DictWriter` 类、`DictReader` **函数**（返回 list[dict[str,str]]） | DictReader 拿不到 .fieldnames、不支持显式 fieldnames/restkey（list 默认参数编译不过）；list(DictReader(f)) 与 for 循环都可用 |

配套测试：`tests/run/163_stdlib_typing.py` … `172_stdlib_csv_dict.py`，共 10 个运行用例，每个文件末尾按仓库约定写 `# --- expect ---`；覆盖 import/注解/算术/比较/异常分支/文件系统夹具。

全量回归（原有 181 个用例 + 新增 10 个）：

```
$ cmake -S . -B build/std -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build/std -j4
$ PYADAMAS_TEST_JOBS=4 bash tests/run_tests.sh build/std build/std/pyadamas
... 191 ran ok，0 FAILED/DIFF，EXIT=0
```

## 六、需要 lead 决策 / 需单独修的问题

1. **@dataclass 语义**：typecheck 已经认 `is_dataclass`，但降级/代码生成没有合成 `__init__`。要真支持，得在 lower_class 里按字段标注生成赋值序。在 src 修好之前，`stdlib/dataclasses.py` 只放透传装饰器 = 构造实参被静默丢弃（实测 P(1,2) → 0,0），**建议不做**。
2. **内建模块名单开放**：把 `program_module()/check_builtin_name()` 的硬编码名单换成「内建表 + stdlib 文件兜底」，即可用 `stdlib/functools.py` 等继续自举，给 itertools/collections 加 API 不再必须动 C++。改动小、收益大。
3. **contextmanager**：需要支持 unknown 函数值的 `*args/**kwargs` 转发（compatibility.md 已列为 build 报错项）。这是通用语言能力，不只是 contextlib 需要。
4. **二进制 IO**：`open` 的写模式（含 binary）只收 str，struct/shutil 类模块没意义；要先补 bytes 读写原语。

**阶段二额外发现的编译器 bug（不是本任务引入，建议单列修复）**：

- **用户类的一元负号段错误**：在用户类上调用 `__neg__` 即 `Segmentation fault`（exit 139）。
  最小复现：
  ```python
  class N:
      v: int
      def __init__(self, v: int) -> None:
          self.v = v
      def __neg__(self) -> N:
          return N(-self.v)
  print((-N(3)).v)
  ```
  影响：fractions 因此不定义 `__neg__`；任何自定义数值类的取反都会炸。
- `list(obj)` 不能消费只有 `__iter__` 的用户类（`csv.DictReader` 因此做成返回 list 的函数）；`for x in obj` 可以。

