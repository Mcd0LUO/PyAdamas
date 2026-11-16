# 与 CPython 的差异及验证方法

PyAdamas 不追求兼容 CPython：语义是按静态类型的需要重新定的。但**正确性**仍然拿 CPython 当基准——对同一段程序，两边 stdout 必须一致，除非差异在这份文档里写清楚了。

## 一、语义差异（故意的）

| 写法 | PyAdamas | CPython | 原因 |
|---|---|---|---|
| 异构容器的 `==`（如 `[1, "s"]`） | 按指针比较 | 逐元素比较 | 元素类型未知，编不出通用比较链 |
| 读未赋值的局部变量 | 得到 0 / `None` | `UnboundLocalError` | 槽位在栈上，初值是 0；做「已初始化」标记要额外一张表 |
| 整数溢出 | 回绕成 int64 | 自动升格为任意精度 | 静态类型的代价，也是性能的来源 |
| `1 << 70` | `64` | `1180591620717411303424` | x86 的移位次数按 64 取模 |
| `2 ** -1` | `0` | `0.5` | 整数幂走整数实现，负指数直接返 0 |
| `x: float = 1; print(x)` | `1.0` | `1` | 标注决定存储类型，int 在 float 位置被真的转成 double |
| `print(a, b, end=f())` 的求值时机 | 边算边写，`f()` 最后求值 | 先把参数和关键字全算完再写 | `print` 是降级期展开的，不是函数调用 |
| `assert x, "msg"` | 抛 `AssertionError("msg")`；无消息时 `assertion failed` | 同 Python（消息文本一致） | 消息只保留字符串内容 |
| `except ... as e` | `e` 是异常值对象，`args` / `str(e)` 可用 | `e` 是异常对象，有完整 `args` / traceback | `args` 目前只保存消息；没有 traceback / 异常链 |
| 未捕获异常 | 先打印函数级 `Traceback (most recent call last)`（普通帧文件:行；生成器/协程逻辑帧仅函数名），再 `pyadamas: uncaught ValueError: msg`，`exit(1)` | traceback + `ValueError: msg`，退出码 1 | 没有异常对象 traceback；内联后归到调用者；async/generator 帧无挂起行号 |
| 异常类型 | 内置异常名 + 继承匹配；`ValueError("msg")` 产生异常值，`raise e`、`raise ... from`、自定义异常类（`class E(ValueError): pass`）可用 | 完整异常类层次、traceback | 自定义异常类类体只能是 `pass`/文档字符串；`args` 只保存消息；没有 traceback |
| 运行时错误 | `IndexError` / `KeyError` / `ZeroDivisionError` / `OverflowError` / `ValueError` / `AssertionError` 可捕获 | 同类异常 | 内部不变量/OOM 仍然直接退出 |
| `range(a, b, c)` 的 `c` | `for ... in range(...)` 里必须是整数字面量；作为值使用时可以是任意 int 表达式 | 任意表达式 | 循环步长在降级期就要定下来；range 对象在构造时求值，没有这个限制 |
| 默认参数不是字面量 | `build` 报错 | 运行时求值 | 默认值在调用点内联填充 |
| `*args` | 形参是 `list[T]` | 形参是 tuple | 列表和元组在 PyAdamas 里表示相同；`len` / 下标 / 迭代一致 |
| 函数装饰器 | `@dec` 在定义点立即执行，包装函数必须用固定形参转发 | 任意可调用 | 没有运行时签名信息，`*args` 转发给 unknown 被装饰者无法绑定 |
| `with` | 只支持类实例的 `__enter__` / `__exit__`；正常、`return` / `break` / `continue` 和异常路径都调用 `__exit__` | 完整上下文管理协议 | `__exit__` 返回值只有真/假语义；异常路径传 `(type, message, None)` |
| import | 本地 `.py` 与包（包需要 `__init__.py`），支持相对导入与静态展开的 `import *`；`m.x` 直接读写模块全局；`from m import x` 是快照；模块别名不能当值 | 任意模块/包、运行时模块对象 | 支持包/相对导入/静态 `import *`（按非下划线导出）；支持导入类作主基类或附加基类（字段、覆盖方法、`super()`、`isinstance`、字面量字段默认值、跨模块 classmethod 的 `cls(...)` 重绑定）；动态 classmethod 的 `cls.x` 仍按声明类解析、方法值暂不支持，子类 `__init__` 签名需与基类兼容 |
| 模块初始化 | 第一次执行 import 时惰性运行一次；循环导入编译期报错 | 支持循环导入和部分初始化 | 初始化失败的模块不会再重试；模块对象不可见 |
| 推导式 | list/set/dict 推导式按循环展开；生成器表达式脱糖成合成生成器函数，惰性求值；含 `async for` 的推导式在协程里可挂起；含 lambda 的推导式改写成生成器表达式 + 构造器，循环变量可被捕获 | 同 Python | 异步 list/set/dict 推导式只能作为语句/赋值/`return` 的根表达式；异步生成器表达式（`(x async for ...)`）已支持，可出现在任意表达式位置；类体里的生成器表达式仍按 list 构造；推导式内部不能引用循环变量的生成器表达式 |
| 生成器 | 状态机 + 堆帧；`for` / `next` / `send` / `throw` / `close` / `yield from`；`yield from` 把 `throw` 转发给子生成器；跨 `yield` 跑 `finally` / `__exit__`；挂起点可在任意子表达式、`except` 与 `finally` 体内；生成器/协程/异步生成器类型跨模块保留元素/结果类型；`Generator[Y, S, R]` 标注给 `send` 实参与 `yield` 表达式收窄类型 | 完整生成器协议 | 未标注 `Generator[Y, S, R]` 时 `send` 值与 `yield` 表达式静态是 unknown（`R` 只记录，`StopIteration.value` 不检查） |
| 协程 / Task / Future | `async def` / `await` 复用生成器帧；`asyncio.run`/`sleep`/`gather`/`create_task`；`done()`/`cancel()`/`result()`/`exception()`；Future 多等待者、Task 多等待者、`set_exception`、`add_done_callback`；取消级联；`wait_readable`/`wait_writable` + poll；异步生成器 + `async for` + `async with`；异步推导式 | 完整 asyncio（Task 组/Future 回调/I/O 流） | `await` 可位于任意子表达式与 `finally` 体内（跨挂起的 vreg 由降级器溢写到帧槽）；异步生成器有 `asend`/`athrow`/`__anext__`/`aclose`，`aclose` 不会 await `async with` 的 `__aexit__`，停在 `async with` 里被直接回收时不 await 退出；Future 回调同步执行；`set_exception` 接收异常值或消息；`exception()` 返回异常值；`CancelledError` 是内置名；`wait_readable` 不是 CPython 标准 API；`gather` 结果元素所有权按静态类型（0.158）；Task 支持多等待者（0.158） |
| 递归深度 | 受栈大小限制（万级） | 默认 1000，可调 | 没有解释器递归检查 |
| 字段初值 `x: int = []` | 每次构造重新求值，实例之间独立 | 类属性，所有实例共享 | 更安全；也避免可变默认值的经典 bug |
| 类体里的普通赋值 | 是类属性：所有实例共享一份存储，`C.x` / `cls.x` / `self.x` 都能读到；子类赋值走写时复制，基类不受影响 | 类体是独立命名空间，属性存在类对象上 | `if`/`for` 等其它类体语句仍按模块全局处理；`obj.类属性 = v` 不支持（对象布局编译期定死） |
| `@classmethod` | 支持；entry 模块本地方法 `cls` 静态绑定并在子类重编译；非入口 / 跨模块方法用运行期类对象（带 vtable 与构造器），`cls(...)` 构造实际子类、`cls.m(...)` 动态分派 | 运行期传入 `type(obj)` | 跨模块 `cls.x` 类属性仍按声明类解析；子类 `__init__` 签名需与基类兼容；动态 classmethod 作为方法值暂不支持 |
| 自定义方法装饰器 | 类创建时按从下到上应用，结果存成类属性；`obj.m(...)` 以 self 为第一个实参调用包装函数，继承沿 MRO 读同一份；`C.m(obj, ...)` 显式传 self，`super().m(...)` 自动补 self；走 `dunder_call` 的 dunder（算术/比较/str/len/hash/iter/contains）、构造（`__init__`）、`with`/`async with` 协议、`__call__` 也可装饰（装饰后不填 vtable 槽，调用点从类属性取包装函数并补 self） | 同 Python（方法装饰器返回的值按描述符绑定） | 装饰器必须返回已知签名的函数且用固定形参 |
| 方法值 `obj.m` / `C.m` | 合成闭包捕获接收者：普通方法 `vcall`（子类覆盖生效）、静态/类方法 `vcall_static`、类接收者的静态/类方法 `call_direct`；默认参数与关键字参数可用 | 同 Python（descriptor 绑定） | 类接收者上取普通方法的值不支持；跨模块 / 动态 classmethod 的方法值暂不支持 |
| 类的属性 | 声明过的字段，或 `self.x = ...` 就地长出来的字段 | 可随时给实例挂任意属性 | 偏移是编译期常量，没有运行时 `__dict__` |
| `print(obj)` | 有 `__str__`（或 `__repr__`）就调，否则 `<Point object>` | 同 Python（默认文本带模块名和地址） | 默认文本没有模块名和地址 |
| 协议 dunder | `__str__`/`__repr__`/`__eq__`/`__ne__`/`__len__`/`__hash__`/`__contains__`/`__iter__`/算术与比较类 dunder 走 vtable；`in` 需要 `__contains__`，`<` 等需要比较 dunder，算术需要对应 dunder，否则编译报错 | 完整数据模型 | `__format__`/`__bool__`/`__reversed__` 已建模（`format()`/f-string 格式说明、真值、`reversed()`）；`__eq__` 的 other 限 unknown 或类类型；只定义 `__eq__` 没定义 `__hash__` 的类在 dict/set 里按身份处理 |
| `random` | MT19937 与 CPython 同种子同序列；`random`/`seed`/`randint`/`uniform`/`choice`/`shuffle`/`randrange`/`getrandbits`/`gauss`/`expovariate`/`triangular`/`sample`/`choices`，以及 `Random` 实例方法 | 完整 random（`bytes`/`betavariate`/`gammavariate`/`choices` 的 `weights` 未实现） | `gauss`/`expovariate` 的算法与 CPython 不同，序列不保证一致；`choices` 的 `k` 是关键字参数（同 CPython）；省略种子时时间 + pid 播种 |
| `os` / `os.path` | `listdir`/`getenv`/`mkdir`/`rmdir`/`remove`/`rename`/`getcwd`/`cpu_count`/`getpid`/`chdir`/`chmod`/`access`/`makedirs`；`join`/`exists`/`isdir`/`isfile`/`basename`/`dirname`/`normpath`/`abspath`/`expanduser`/`splitext`/`realpath`/`getsize`/`getmtime`/`split`/`samefile`；常量 `sep`/`linesep`/`F_OK`/`R_OK`/`W_OK`/`X_OK` | 完整 os | `getenv` 缺失返回空串（CPython 返回 None）；没有 `walk`/`environ`/`stat`/`system`；`os.path` 是独立模块别名：`import os.path as p` 或 `from os.path import ...`，`import os` 后 `os.path.x` 暂不支持 |
| `subprocess` | `run` 支持 `capture_output`/`text`/`timeout`/`check`/`shell`/`cwd`，`args` 可以是 `list[str]` 或字符串；`Popen` 支持 `shell`/`cwd`/`stdin`/`stdout`/`stderr`（`PIPE`/`DEVNULL`/`STDOUT`），方法 `wait`/`communicate`/`kill`/`terminate`/`send_signal`，属性 `pid`/`returncode`/`stdout`/`stderr`/`stdin`；`TimeoutExpired`/`CalledProcessError` 按内置异常名捕获 | 完整 subprocess | 不返回 bytes：`text=False` 也返回 str；`input`/`env`/`preexec_fn` 未实现；exec 失败按退出码 127；**没有 `poll()`**（PyAdamas 里 None 和 0 同形，`poll() is None` 会把正常退出误判成还在跑，要轮询用 `wait(timeout=…)`）；`communicate()` 对没开管道的流返回空串而不是 `None`；`Popen` 句柄被回收时会 SIGKILL 子进程（CPython 不会）；`from subprocess import TimeoutExpired` 不支持 |
| `time` | `time`/`monotonic`/`perf_counter`/`sleep`，秒为单位 | 完整 time | 没有 `localtime`/`gmtime`/`strftime`/`mktime` |
| `string` | 常量 + `capwords()`，随编译器分发 `stdlib/string.py`；常量纯 PyAdamas，`capwords` 走单遍 intrinsic | 完整 string | 子集；`capwords` 单遍扫描、只分配一个输出串 |
| `itertools` | `chain`/`repeat`/`islice`/`product`/`permutations`/`combinations`/`combinations_with_replacement`/`zip_longest`/`accumulate`，按实参静态特化，返回 list | 完整 itertools | 立即求值，没有惰性迭代器；`islice` 只接受 `(x, stop)`；`zip_longest` 用 `Optional[T]` 补 `None` |
| `collections` | `Counter`/`most_common`、`OrderedDict()`（dict 本来就保序）、`deque([iterable[, maxlen]])` 的 append/appendleft/pop/popleft/rotate/clear/extend/extendleft/remove/count/index/insert/reverse/copy + `len`/下标/迭代/`.maxlen`、`defaultdict([factory[, initial]])`（工厂限内置零参类型） | 完整 collections | 没有 `namedtuple`；`Counter` 返回普通 dict；`OrderedDict` 只支持空构造；`defaultdict` 的 `default_factory` 属性不可读，静态类型仍是普通 dict；deque 只在元素明确是堆类型时才增减引用（字符串元素可能泄漏，不会崩） |
| `functools` | `reduce`/`wraps`/`partial`/`lru_cache`（装饰器） | 完整 functools | `partial` 不能作用于 `*args`/`**kwargs` 函数或方法值；`lru_cache` 只支持普通函数（不支持方法、零参函数），键类型限 `int`/`bool`/`float`/`str`，缓存无上限并忽略 `maxsize`；`wraps` 只作装饰器用（不建模 `__name__`/`__doc__`，装饰器链里整层跳过）；`reduce` 的 float 元素要求 f 形参是 float |
| `datetime` | `date`/`datetime` 构造、`now`/`today`、字段、`isoformat`/`strftime`/`weekday`、`date`/`datetime` 的 `replace`；`timedelta` 构造、字段、`total_seconds()`、加减、乘/除整数、取负、比较与打印 | 完整 datetime | 没有时区与 `fromtimestamp`；`datetime.datetime.now()` 这种链式类方法不支持，用 `datetime.now()` |
| `pathlib` | `Path` 构造、`/` 运算符、`str(p)`、`name`/`stem`/`suffix`/`parent`/`parts`、`exists`/`is_file`/`is_dir`/`is_absolute`、`read_text`/`write_text`/`read_bytes`/`write_bytes`、`iterdir`/`glob`/`mkdir`、`joinpath`/`relative_to`/`resolve`/`absolute`/`touch`、`unlink`/`rmdir`/`with_suffix` | 完整 pathlib | `glob` 支持 `*`/`?`/`**`，不支持字符类 `[...]`；`mkdir` 只收关键字参数（同 CPython） |
| `heapq` / `bisect` / `binascii` | 纯 PyAdamas 实现；`heapq` 的 push/pop/替代与 nlargest/nsmallest，`bisect` 的左右查找与 insort，`binascii` 的 hexlify/unhexlify | 完整模块 | `bisect` 目前只接受有序 `list[int]`，`hi` 省略或 0 表示列表末尾；`binascii` 没有 CRC/Base64 底层接口 |
| `base64` / `hashlib` / `hmac` | base64 的 b64/urlsafe/b16/b32/`encodebytes`；hashlib 的 `md5`/`sha1`/`sha256`；`hmac.new` + `update`/`digest`/`hexdigest` | 完整模块 | base64 不支持 `validate=`/`altchars=` 与 `str` 输入；hashlib 不支持 `usedforsecurity=`/`copy()`/`pbkdf2_hmac`；hmac 不接受 hash 构造函数或模块对象 |
| `logging` / `statistics` / `textwrap` / `operator` | 子集：logging 的等级常量、`basicConfig`、`getLogger` 与 Logger 方法；statistics 的均值/中位数/方差/标准差/调和与几何均值；textwrap 的 `wrap`/`fill`/`dedent`/`indent`/`shorten`；operator 的算术/比较/逻辑/容器包装与 `itemgetter` | 完整模块 | logging 没有 handler/filter/Formatter、文件输出与 `%` 参数展开；statistics 输入限 `list[float]`（`mode` 为 `list[int]`）；textwrap 没有 `TextWrapper` 类；operator 没有 `attrgetter` |
| `linalg` | `matmul`/`matmul_into`/`matvec`/`axpy`/`scal`/`norm`/`transpose`/`dot`，封装 OpenBLAS，数据按行主序拍平传入 | —（PyAdamas 自有模块） | 需要系统安装 OpenBLAS；维度非法或输入过短抛 `ValueError` |
| `slice` / `vars` / `dir` | `slice()` 只在下标位置（脱糖折成切片语法）；`vars(obj)` 返回字段 dict（value 统一 unknown）；`dir()` 列成员名 | 完整语义 | 没有运行期 slice 对象；`vars()` 无参（locals）不支持；`dir(模块)` 不支持 |
| `json` | `dumps(obj, indent=n, sort_keys=True)` / `loads(text, sample)`；默认分隔符 `", "`/`": "`；`loads` 用 sample 的静态类型决定目标结构 | 完整 json | 没有 `separators`/`ensure_ascii`/`default`；`loads` 必须给 sample（`None` 用于解析 null）；只覆盖 int/bool/float/str/None/list/dict/定长 tuple；dict 的 value 类型必须一致（混合 value 推不出静态类型） |
| `csv` | `reader(f, delimiter=",", quotechar='"')` 返回 `list[list[str]]`；`writer(f, delimiter=",", quotechar='"', quoting=0, lineterminator="\r\n")` 带 `writerow`/`writerows`；`QUOTE_*` 常量与 `Error` | 完整 csv | `reader` 只接受 `file` 且 eager 读完，不接受任意可迭代对象；`delimiter`/`quotechar` 按字节且必须单字节；`QUOTE_NONNUMERIC` 等同 `QUOTE_ALL`（字段都是 str）；没有 dialect / DictReader / DictWriter；`csv.Error` 跨模块按 `ValueError` 捕获 |
| `math.fsum` | `list[float]` 的 Shewchuk 精确求和；`list[int]` 在 build 阶段报错 | CPython 接受任意数值可迭代 | 只收 `list[float]`；结果是正确舍入 |
| `re` | `compile`/`search`/`match`/`fullmatch`/`findall`/`sub`/`split`/`escape`，Pattern/Match 方法，`I`/`M`/`S` 标志；回溯 VM | 完整 re | v2 支持反向引用、四向断言、原子组、占有量词、内联标志；`findall` 只有一个捕获组时返回组内容（多个时返回整段匹配），`split` 按序插入捕获组；仍不支持条件组与 Unicode 属性类；字符串按字节匹配 |
| `bytes` | 字面量 / 索引 / 切片 / `+` / `in` / 比较 / `decode` / `encode`；相邻字面量可隐式拼接；`for c in b"..."` 产出 int，`list`/`tuple`/`sum`/`min`/`max`/`any`/`all`/`sorted`/`enumerate` 也接受 bytes；printf 风格 `%`（`d/i/u/x/X/o/f/F/e/E/g/G/s/r/c`、`%%`、标志/宽度/精度）；方法 `upper`/`lower`/`strip`/`lstrip`/`rstrip`/`replace`/`find`/`rfind`/`count`/`split`/`startswith`/`endswith`/`join` | 完整 bytes | `%` 不支持 `#` 前缀、映射 key 和把 list 当多参数元组；与 `str` 不能混拼；`bytes` 标注与跨模块签名现在保留类型 |
| 方法调用 | 走 vtable，代价固定 | 走 `__dict__` / 描述符 | 没有去虚化，也不做内联 |
| 内存回收 | 引用计数 + 分代收集（minor trial-deletion / major 标记-清扫） | 分代 GC | 环和 unknown 泄漏由 GC 延迟回收；根由帧记录登记，GC 只扫 unknown 槽、含 unknown vreg 帧的临时区和 callee-saved 寄存器，不再扫整段栈；无 `__del__`/弱引用 |
| 闭包捕获 | cell 按引用捕获；嵌套函数直接自引用用当前闭包 | cell 按引用捕获，晚绑定 | 父作用域之后重绑函数自己的名字时，PyAdamas 仍指向原闭包；相互递归闭包的环由 GC 回收 |
| `lambda` | 脱糖成插在语句前的合成嵌套 `def` | 每次求值新建函数对象 | 调用/捕获语义一致；条件/循环里的 lambda 只创建一次，对象身份与创建时机略有差异 |
| `nonlocal` | 必须显式声明；赋值解析到最近外层函数 | 同 Python | 没有 `nonlocal` 时赋值就是本地变量 |
| 字典 / 集合 | 开放寻址表，保留插入顺序；key 按静态类型哈希/判等 | dict/set，任意 hashable | 空字典的 unknown key 按机器字比较；没有运行时类型标签，不能混用任意 key |
| `d[k]` 缺失 | panic `KeyError: key not found` | 抛 `KeyError(key)` | 运行时错误消息不带 key 内容 |
| `d == e` / `s == t` | 逐元素比较（dict 与顺序无关，同构嵌套容器逐层比） | 深比较 | 元素类型不同（异构容器）时退回指针比较 |
| `print(d)` / `print(s)` | 打印内容（`{'a': 1}` / `{1, 2}`） | 打印内容 | 元素类型未知（异构容器）时仍输出 `<dict object>` / `<set object>` 占位 |
| dict/set 方法 | `get`/`keys`/`values`/`items`/`update`/`add` 和删除类方法 `pop`/`remove`/`discard`/`clear`/`popitem` 可用 | 完整方法 | `keys()` 等返回 list 而不是视图；删除用墓碑 + 插入顺序双向链表，摊还 O(1)；`popitem` 固定 LIFO，`set.pop` 固定取最后插入项 |
| `d.get(k)` 缺失 | 返回 `None` | 返回 `None` | PyAdamas 里 `None` 就是 `0`，值为 `0` 的项与缺失分不出来 |
| `None` 与 `0` | 同一个表示：`0 is None` 为真、`None == 0` 为真 | 完全不同 | 值表示不带类型标签，是「没有 tag」的直接后果 |
| 切片 | str 按码点、bytes 按字节；list/tuple 返回新容器，语义同 CPython | 同 Python | `step == 0` panic；未知元素类型只做保守 GC |
| 切片赋值 | 仅 list；step 1 可改变长度，扩展切片长度必须一致；右侧可为 str | 任意可变序列，任意可迭代右值 | 没有通用迭代协议；tuple 不可变，拒绝赋值 |
| `range` | 惰性对象（`range[int]`），只存边界；不可变，赋值在编译期就报错 | 惰性对象，不可变，赋值是运行期 TypeError | 静态类型知道 range 不可变，直接拦在类型检查（消息与 CPython 一致） |

字符串已是 **UTF-8 存储 + 码点语义**：`len` / 索引 / 切片 / 迭代 / 方法 / 格式化
宽度全部按码点，`len` 仍是 O(1)（码点数缓存在对象头里，字面量在编译期算好），
`bytes` 全部按字节。字节长度出口是内置函数 **`len_bytes(s)`**（O(1)）。

仍存在的差异（都只影响非 ASCII）：

- 非 ASCII 的大小写映射与 Unicode 字符类别未实现（`"中".isalpha()` 仍 `False`，
  `"中a文".title()` 的词边界按「非字母」处理）；
- `str.strip()` 的默认空白仍只认 ASCII 六种，不认 Unicode 空白；
- `re` 的 `\w` / 字符类 / 大小写折叠继续只认 ASCII；
- `chr()` 拒绝代理码点（0xD800–0xDFFF，无法编成 UTF-8），CPython 允许；
- `open(..., 'rb').read()` 仍按 `str` 返回原始字节（内容可能不是合法 UTF-8），
  这类对象按「字节直通串」构造（`len` 等于字节数）；
- `bytes` 与 `str` 比较是按字节比较（CPython 是 `False`），因为两者共用同一套
  运行时表示、不带类型标签。

设计见 [str-codepoints.md](str-codepoints.md)。

## 二、尚未实现的构造

这些在 CPython 里合法，在 PyAdamas 里分两种情况：

**语法层就拒**（`check` 报错）：超出 int64 的整数、Python 2 遗留写法、方法缺 `self`、字段赋值类型不符。

**能解析、能过类型检查，但 `build` 报错**：通过 unknown 函数值的 `*` / `**` 展开、`*` 位于固定形参之前且不是定长 tuple、非异常类名当异常用、unknown 值上的数值/list/dict/tuple/set 内置类型模式、类体字段默认值里的 lambda，以及嵌在其它表达式里的异步推导式。跨模块继承（0.139/0.140）、函数内定义类与类体 lambda（0.135）、导入类字段子模式（0.135）、异步生成器表达式（0.135）都已支持，不再属于这一层。

清楚这个分层很重要：`pyadamas check` 尽量当通用 Python 语法/类型检查器用，检查器放宽的边界都记在 [CHANGELOG 0.6.1](archive/changelog-0.40.md)；`pyadamas build` 才是「能不能编译」的判据。

## 三、验证手段

七层，从便宜到贵：

| 层 | 命令 | 规模 | 防的是什么 |
|---|---|---|---|
| 正例 | `ctest --preset dev` 里 `tests/*.py` | 11 个文件 | 前端退回保守 |
| 反例 | `ctest --preset dev` 里 `tests/fail/*.py` | 52 个文件 | 检查器为了过测而放水 |
| 运行 | `ctest --preset dev` 里 `tests/run/*.py` | 156 个文件 | 后端烂掉没人知道 |
| 模块 | `ctest --preset dev` 里 `tests/modules/*/` | 12 个目录 | 分模块编译/链接、惰性初始化、缓存失效 |
| 未建模 | `ctest --preset dev` 里 `tests/unsupported/*.py` | 1 个文件 | 「标了没建模、实际却悄悄编译过去」 |
| 内存 | 16MB `RLIMIT_AS` 下跑压力程序 | 拼接 / 对象 / 闭包 / 容器 / 切片 / 删除 / 推导式 / 变长实参 / `with` / 装饰器 / 异常展开 / `yield from` / 异步推导式 / 环 / unknown 泄漏 | 引用计数或 GC 漏插桩、重复释放、误回收 |
| 差分 | `pyadamas build` 后运行 `tests/diff/run_diff.py`（自举），也可 `python3` 直接跑 | 随机程序 | 语义漂移、边界错误 |

### 四类测试的写法

- `tests/*.py`：必须通过 `check`。文件里写清这段覆盖什么。
- `tests/fail/*.py`：首行 `# expect: <错误片段>`，`check` 必须报出包含该片段的错。
- `tests/run/*.py`：必须编译成可执行文件并运行，末尾 `# --- expect ---` 之后的注释行是期望输出，逐行比对。
- `tests/unsupported/*.py`：首行 `# unsupported: <标记>`，`check -v` 必须打出该标记，且 `build` 必须失败。

反例和运行用例是重点：只测「能通过」会让检查器越来越松，只测前端会让后端烂掉。

### 差分测试

`tests/diff/` 下七个生成器，覆盖不同侧面：

| 生成器 | 内容 | 每个种子用例数 |
|---|---|---|
| `expr` | 纯表达式：整数/浮点四则、位运算、整除取模 | 60 |
| `struct` | 函数、循环、分支、变量复用 | 40 |
| `float` | 浮点 `//` `%` 的符号修正、`print` 的 `sep`/`end` | 60 |
| `repr` | 浮点转字符串：固定边角值（0/-0、次正规、最大正规值）+ 十进制定标随机值 | 40 |
| `gen` | 生成器迭代、`next`/`StopIteration`、`close` 清理 | 40 |
| `async` | `asyncio.run`/`sleep(0)`/`gather`/`create_task`、异常跨 await、取消级联、Future、fd poll、异步生成器 + `async for`/`async with` | 30 |
| `range` | `range` 惰性对象：边界、负步长、下标、`in`、`min`/`max`/`sum`、物化与推导式 | 10 |

```bash
# 自举：runner 自己用 PyAdamas 编译执行（verify.sh 走这条；-- 之后是程序参数）
./build/dev/pyadamas build -o /tmp/run_diff.prog tests/diff/run_diff.py
/tmp/run_diff.prog --pyadamas build/dev/pyadamas --cpython python3 --seeds 3

# 同一份脚本也能用 CPython 执行（随机引擎逐位一致，生成的用例相同）
python3 tests/diff/run_diff.py --seeds 3
python3 tests/diff/run_diff.py --seeds 10      # 2800 个程序，建议每次改后端都跑
python3 tests/diff/run_diff.py --only repr     # 只跑浮点格式化
```

PyAdamas runner 串行执行（PyAdamas 没有线程/进程池），840 个程序约几分钟；CPython runner 也串行，行为与判定标准完全相同。

判定标准：PyAdamas 退出码必须为 0（编译期错误算失败），stdout 与 CPython **逐字节相同**，stderr 不比（panic 消息是我们自己的）。

### 内存验收

引用计数没有单测框架能直接断言的地方，用受限地址空间跑压力程序：

```bash
./build/dev/pyadamas build stress.py -o stress
python3 - <<'PY'
import resource, subprocess
def lim():
    resource.setrlimit(resource.RLIMIT_AS, (16 << 20, 16 << 20))
p = subprocess.run(["./stress"], preexec_fn=lim)
print(p.returncode)
PY
```

`tests/run/10_rc.py` 负责别名、字段、列表、返回值、`continue` 跳过清理等
正确性；`tests/run/11_closure.py` 覆盖闭包捕获、`nonlocal`、多实例、多层
嵌套和自递归；`tests/run/12_lambda.py` 覆盖 lambda；`tests/run/14_dict.py`
覆盖字典/集合/`in`/方法；`tests/run/15_slice.py` 覆盖切片；
`tests/run/16_dict_del.py` ~ `tests/run/21_decorator.py` 分别覆盖删除类
方法、切片赋值、变长实参、推导式、`with` 和装饰器；
`tests/run/22_exception.py` 覆盖 try/except/else/finally、raise、层级
匹配、`with` 异常路径和运行时错误转换；`tests/run/23_generator.py` ~
`tests/run/32_exception_value.py` ~ `tests/run/34_exception_chain.py` 覆盖
异常值、内置函数、自定义异常与 `raise ... from`，并连同前面的用例覆盖
生成器、协程、Task、异步语言面、Future/取消/fd/回调、`yield from`、
异步推导式与异步生成器协议。
`tests/run/13_gc.py` 会真正触发收集器：对象环、字典自环、闭包垃圾和
unknown 容器。RSS 压力程序负责「长循环里内存不随迭代数增长」。当前实测
100 万次拼接 / 100 万次对象分配 / 30 万次带字符串字段的对象分配 /
20 万次闭包工厂 / 20 万次对象环 / 20 万次相互递归闭包 / 10 万次字典/集合
创建 / 10 万次切片与容器方法 / 本轮 50 万次新构造循环 + 25 万次异常展开，
峰值 RSS 约 8–9MB。

生成器刻意避开第一节列出的语义差异——那些是已知的、有意的，测了只会制造噪音。差分测试抓的是**我们以为一致、实际不一致**的地方，历史上抓到过：浮点 `%` 的符号、`10.0` 打成 `1e+01`、`-0.0 % 3.0` 的零符号。

### 前端鲁棒性基线

拿 CPython 标准库当输入，看 `check` 的通过率。这不是功能测试，是「别把真实代码判错」的护栏。

```bash
cmake --preset dev && cmake --build --preset dev   # 先构建 build/dev/pyadamas
export PYADAMAS="$PWD/build/dev/pyadamas"
cd "$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')"
find . -name '*.py' -print0 | xargs -0 -P 8 -n 1 sh -c \
  '"$PYADAMAS" check --syntax "$0" >/dev/null 2>&1 && \
   { "$PYADAMAS" check "$0" >/dev/null 2>&1 || echo "FAIL $0"; }' > /tmp/fail.txt

echo "语法通过:"; find . -name '*.py' -print0 | xargs -0 -P 8 -n 1 sh -c \
  '"$PYADAMAS" check --syntax "$0" >/dev/null 2>&1 && echo ok' | wc -l
echo "类型失败:"; wc -l < /tmp/fail.txt
```

`check --syntax` 只做语法，`check` 再做类型；统计口径是「语法通过的那些文件里，类型检查也通过的比例」。

**历史口径（0.6.1，CPython 3.11.1 + 本机 site-packages）**——保留作对照：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 1740 | 1289 | 1143 / 1289（88.7%） |
| 标准库 + site-packages | 38582 | 35693 | 29143 / 35693（81.6%） |

**0.6.1 那组数字也不能直接和 0.5 的 96.1% 比。** 0.6 之前根本没有类检查，`class` 语句只解析不分析，「通过」是虚高。0.6.0 刚加类检查时失败 **19064** 个文件（通过率 46.6%），把误杀逐条放宽（见 [CHANGELOG 0.6.1](archive/changelog-0.40.md)）之后降到 **6550** 个（81.6%）。也就是说，当时的下降主要是「多了一层以前没有的检查」，不是前端变严。

**上一次完整口径（0.81.1 重跑，Debian 打包的 CPython 3.13.5）**：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体（去掉 `test/`） | 533 | 532 | 251 / 532（47.2%） |
| 标准库本体 | 598 | 596 | 279 / 596（46.8%） |
| 标准库 + site-packages | 731 | 729 | 303 / 729（41.6%） |

**两段数字不能直接比较。** 0.6.1 那次用的是完整 CPython 3.11.1 安装（38582 个 `.py`）；
这次是 Debian 打包的 python3.13.5，标准库被拆成多个包（只剩 598 个 `.py`），而且
里面带着 CPython 自己的 `test/` 目录——那批文件是**故意写错**来测 NameError /
traceback 的，必然进失败清单。口径变了，趋势才可比。

0.133 复测（同一台 Debian 3.13.5）：标准库本体 598 个文件，语法通过 596，
类型通过 277 / 596（46.5%），比 0.81.1 少 2 个；失败项仍集中在
`__repr__` 返回 `str`、`getattr` 属性名必须是字面量、自定义异常类体等已知模式。

**0.158 复测（2026-11-14，同一 Debian 3.13.5）**：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体（去掉 `test/`） | 533 | 532 | 253 / 532（47.6%） |
| 标准库本体 | 598 | 596 | 281 / 596（47.1%） |

比 0.133 的 277 / 596 多 4 个。失败类型 top（不含 `test/`，错误消息归一）：

| 错误 | 文件数 | 性质 |
|---|---|---|
| `__repr__` / 返回值必须为 `str` | 52 | 标注是约束 |
| `hasattr()` 只支持类实例 | 43 | 未建模动态属性 |
| `isinstance()` 第二参数不是类 | 26 | 动态元组/类型值 |
| 实参数目 / 类型不符 | 26 | 装饰器改签名、元类构造等 |
| 自定义异常类字段/方法 | 24 | 类体只允许 `pass` |
| `super()` 找不到方法 | 23 | 外部基类/动态类 |
| `getattr()` 属性名非字面量 | 16 | 动态属性 |
| 对象没有该属性 | 14 | 外部类型成员表不可见 |
| undefined name | 11 | 运行时注入 |
| 返回值必须为 `bool` / `int` | 18 | 标注是约束 |

语法层的失败不再「全部是明确拒绝的那几类」：0.74 支持 complex / match、0.79 支持
bytes 之后，旧清单（bytes 2038 / complex 620 / int64 138 / match 91）已经不成立。
该口径下只剩 2 个文件：

| 构造 | 合计 | 不含 `test/` | `test/` 内 |
|---|---|---|---|
| PEP 695 泛型语法 | 2 | 1 | 1 |

都是 `class C[T]:` 没建模。

0.81.1 口径的类型失败分类（含 site-packages，供对照）：

| 错误 | 标准库（不含 `test/`） | + site-packages | 性质 |
|---|---|---|---|
| 返回值 / 标注不符 | 84 | 97 | 标注是约束：比如 `__repr__` 必须返回 `str` |
| 实参数目 / 类型不符 | 73 | 86 | 装饰器改变签名、元类构造、`getattr` 参数个数 |
| 对象没有该属性 / 方法 | 38 | 48 | 外部类型的成员表看不见（已按 `open` 放过大部分） |
| `hasattr()` 只支持类实例 | 37 | 45 | `hasattr` 目前只接受类实例 |
| 类体不支持的成员 | 22 | 27 | 自定义异常类只能有 `pass` 类体 |
| `undefined name` | 10 | 100 | 运行时注入：`exec` 动态生成、测试文件里故意写错的标识符 |

`undefined name` 在带 site-packages 的口径下从 10 条涨到 100 条——运行时注入的名字更多。
这是静态分析的固有盲区，再放宽就得牺牲「拼错变量名」这种本来该报的检查。CPython 的
标准库测试里专门有一批文件是**故意写错**的（测 NameError / traceback / 解包失败），
**0.164 复测（2026-11-14，同一 Debian 3.13.5）**：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体（去掉 `test/`） | 533 | 532 | 266 / 532（50.0%） |
| 标准库本体 | 598 | 596 | 294 / 596（49.3%） |

比 0.158 多 13 个（+2.2pp）。这一轮只放开两处**误判**，没有加新语法：

- `__str__`/`__repr__`/`__len__`/`__hash__`/`__bool__`/`__eq__` 等协议 dunder 的返回类型，改成函数体推理完之后统一校验（`check_dunder_returns()`）。定义处还看不到 return 的类型，以前按 unknown ≠ str 直接判死；
- `getattr(obj, name, default)` 的第三个参数：静态布局里属性集合固定，属性缺失时结果就是缺省值。

剩余失败的类型分布（归一化，596 口径）：

| 错误 | 文件数 | 性质 |
|---|---|---|
| `hasattr()` 只支持类实例 | 48 | 没建模动态属性（receivers 是模块/函数/unknown） |
| `super()` 在基类里找不到方法 | 41 | 基类来自别的模块，跨模块类没建模 |
| `isinstance()` 第二参数 | 32 | `bytes` 没有独立运行期种类、外部类、`(A, B)` 元组 |
| 自定义异常类的字段/方法 | 25 | 异常类类体只允许 `pass` |
| `getattr()` 属性名必须是字面量 | 21 | 动态属性名 |
| `getattr()` 只支持类实例 | 20 | 同 `hasattr` |
| undefined name | 20 | 类体作用域、运行时注入 |
| 对象没有该属性 | 19 | 外部类型成员表不可见（如异常对象的 `errno`） |
| 异常构造只接受一个消息参数 | 11 | 自定义异常类的 `__init__` |

**这些已经不是「判得比 CPython 严」，而是四项没建模的能力**：动态属性、跨模块类、异常类当普通类、`bytes` 的运行期标签。继续推基线得先做其中一项，靠放宽规则已经没有余量了——再放宽就会开始吞掉真正的错误。

**0.165 复测（2026-11-14，同一 Debian 3.13.5）**——把「动态属性」这一项落地：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体（去掉 `test/`） | 533 | 532 | 288 / 532（54.1%） |
| 标准库本体 | 598 | 596 | 320 / 596（53.7%） |

比 0.164 多 26 个（+4.4pp）。做了两件事：

- **实例动态属性**：类实例末尾可挂一个惰性分配的 `dict[str, unknown]`（偏移存进 vtable 元数据第 6 个字，0 = 没有、不用的类零开销）。`setattr`/`getattr`/`hasattr` 对静态没有的字段落到运行期查表；属性名非字面量（`getattr(a, name)`）也走查表。只有真的反射过的类才开槽——普通字段读取仍在编译期判死活，所以拼错的字段名照样报错（`f28` 保住）。
- **非类接收者的反射，check 阶段放过**：`hasattr(module, ...)` / `getattr(func, ...)` / unknown 接收者，静态证明不了有还是无（CPython 最常见的动态判别，标准库一半文件在用）。`check` 只打 `unsupported` 标记不再判死，`build` 遇到真正用模块对象当值才拒绝。

剩余失败（归一化，596 口径）：

| 错误 | 文件数 | 性质 |
|---|---|---|
| `super()` 在基类里找不到方法 | ~20 | 基类来自别的模块，跨模块类没建模 |
| `isinstance()` 第二参数 | 32 | `bytes` 没有独立运行期种类、外部类、`(A, B)` 元组 |
| 自定义异常类的字段/方法 | 25 | 异常类类体只允许 `pass` |
| 跨模块类的字段/方法可见性 | ~25 | 外部的类成员表看不见 |

**0.171 复测（2026-11-15）**——继续收口散簇：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 598 | 596 | 501 / 596（84.1%） |
| 标准库本体（去掉 `test/`） | 533 | 532 | 449 / 532（84.4%） |

比 0.170 多 66 个（+11.1pp）。批量放宽（多为「静态不可判定 / 编译期布局限制」，check 阶段 conservative 放过、build 再判）：

- **模块级 undefined name**：类体/模块体的名字可能后文或其它模块定义，只在函数体内确定性报错（f09 改成函数体用例保留诊断）。
- **None 接收者**：方法/属性访问、有序比较、成员测试、迭代、下标赋值一律放过（optional 守卫没窄化的误杀）。
- **类型对象**：`type(self).method()` 动态方法调用、`type(name, bases, dict)` 3 参造类、`__qualname__`/`__mro__` 等元数据属性、类实例无 `__call__` 当构造器调用。
- **super()**：类上下文丢失时放过；协议 dunder（`__new__`/`__init_subclass__`）找不到时放过；`super(Cls, self)` 当前类按零参。
- **多继承**：附加基类引入主基类没有的字段从硬错误降为 unsupported。
- **可重绑定**：推断形参、`*args`/`**kwargs` 不锁类型。
- **杂项**：`async with` 目标未知/跨模块放过、`print(file=)` 任意 file-like、`open(**kwargs)`、独立 `slice()`、`str.maketrans`/`bytes.partition`、file 方法名当值、切片赋值允许 Class/None、Exception/Class/Dict 可解包。

**剩余 95 个的性质**（到 596/596 的真实门槛）：剩余多数是「正确的检查结果」，不是误杀——例如 `tempfile` 里 59 处裸 `os.` 而模块只 `import os as _os`（CPython 运行时也会 NameError）、enum/timedelta/Decimal 的算术语义、跨模块类成员可见性。要全通需要**放宽真阳性诊断**或实现完整 import 图解析，前者会让 check 变差、后者是编译期大工程。


**0.170 复测（2026-11-14）**——一键扫荡剩余散簇：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 598 | 596 | 435 / 596（73.0%） |
| 标准库本体（去掉 `test/`） | 533 | 532 | 392 / 532（73.7%） |

比 0.169 多 24 个（+4.1pp）。批量收口：

- **2 参 super**：`super(Cls, self)` 当 Cls 是当前类时按零参处理。
- **no-`__init__` 带参实例化**：常量类的标记值实例化不再判死。
- **unpack 再放宽**：generator/range/str/Optional 目标可解包（元素类型逐个赋）。
- **type 对象元数据属性**：`__qualname__`/`__mro__`/`__doc__`/`__bases__` 等。
- **异常值方法**：`with_traceback`/`add_note`；异常/type 对象属性赋值、方法名 monkey-patch（仅函数值）放宽。
- **推断形参可重绑定**：inferred 参数 not annotated，`cannot assign` 簇 18→4。

剩余 top：`undefined name`（45，多为跨模块/条件导入常量）、`has no method`（22）、`has no attribute`（11）、`is not callable`（10）、`super() is only supported`（10）。


**0.169 复测（2026-11-14）**——getattr 非字面量 + 内置异常多参：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 598 | 596 | 411 / 596（68.9%） |
| 标准库本体（去掉 `test/`） | 533 | 532 | 370 / 532（69.5%） |

比 0.168 多 37 个（+6.1pp）。两块高优先级：

- **getattr/setattr/hasattr 属性名非字面量**：属性名是变量（`getattr(obj, name)`）的写法，之前只对类实例放行、其它接收者（模块/函数/unknown）直接报错。现在 check 一律放过（返回 unknown），build 阶段类实例走运行期动态属性表。
- **内置异常构造多参 + 关键字**：`ValueError(a, b, c)` 任意多个位置参、`Error(code=1)` 关键字构造不再拒绝。lower/运行时给异常值建 args 元组、消息取元组 repr（`(a, b, c)`）；在 raise→except 间保留 args（`pending_exc_args`），`e.args` 与 CPython 一致。新增 `tests/run/159_exc_multi.py`（输出与 CPython 逐字一致）。

剩余失败的新 top：`undefined name`（39）、`has no method`（24）、`has no attribute`（21）、`cannot assign`（18）。isinstance/bytes/getattr/异常构造等高价值簇都已收口。


**0.168 复测（2026-11-14）**——isinstance/bytes：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 598 | 596 | 374 / 596（62.8%） |
| 标准库本体（去掉 `test/`） | 533 | 532 | 338 / 532（63.5%） |

比 0.167 多 26 个（+4.4pp）。两块：

- **isinstance 前端**：第二参数现在接受的内置类型扩展到 `bytes`/`bytearray`/`memoryview`/`type`/`slice`/`frozenset`/`property`/`classmethod`/`staticmethod`，以及 `(A, B, ...)` 元组/列表、unknown 与可迭代目标。运行时对元组/列表目标短路 OR：逐个元素检查，任一命中为真；known 类型静态判定（`isinstance(b'x', bytes)`→True 等）；unknown 值带运行期标签的内置（str/list/tuple/dict/complex）按标签查，无标签的保守返回 false。
- **bytes**：`bytes()` 构造接受 2 参（对象+编码，编码忽略）与 list/tuple/生成器/range 的 int 元素；`bytes.decode` 放宽到 0..2 参；`bytes * int` 重复、`bytes.translate`；内置类型名当值（`int_.from_bytes`）走类型级方法。新增 `tests/run/158_isinstance_bytes.py`（输出与 CPython 逐字一致）。

剩余失败已高度分散（undefined name、各类运算符、异常构造、跨模块类成员等），isinstance/bytes 两簇基本收口。


**0.167 复测（2026-11-14）**——自定义异常类当普通类：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 598 | 596 | 348 / 596（58.4%） |

比 0.166 多 13 个。有字段/方法的自定义异常类是真正的 vtable 实例，`raise Rich(args)`/`except Rich as e`/`e.field`/`e.method()` 都走普通类机制；pass-only 自定义异常和内置异常（含 `.args`/`str`）保持原状。内置异常的其它属性（`OSError.errno` 等）check 保守放过。

**0.166 复测（2026-11-14，同一 Debian 3.13.5）**——把「跨模块类」里的 super() 一项落地：

| 范围 | 文件数 | 语法通过 | 类型通过（占语法通过者） |
|---|---|---|---|
| 标准库本体 | 598 | 596 | 335 / 596（56.3%） |

比 0.165 多 15 个。`super()` 的跨模块基类：定义类的 MRO 里有导入基类时，它的方法在 check 阶段看不见。以前直接按「找不到方法」拍死，现在只在**纯本地继承链**都找不到时才报错；有导入基类（或 open 类）时返回 unknown，交给 build 沿运行期 MRO 分派。

剩下三项没建模的能力：跨模块类的字段/其它方法可见性、自定义异常类当普通类、`bytes` 的运行期标签。

### 语法对照### 语法对照

另有一次性的实验：拿 91 条语法边角用例同时喂给 `pyadamas check --syntax` 和 CPython 的 `compile()`，5 条不一致，全部是设计性拒绝（bytes / complex / match）或 PEP 701 已经允许而 CPython 3.11 还不认的 f-string 嵌套。这是一次性实验，结论属于当时的版本——bytes / complex / match 现在都已经支持了。这批用例没有进仓库（价值随时间下降），记在这里供参考。

## 四、当前性能

`-O0` 构建的 PyAdamas 生成 `-O0` 风格的代码，对手是同机容器内的 CPython 3.13.5：

| 程序 | 相对 CPython 3.13 |
|---|---|
| `fib(35)`（递归 + 调用） | 约 16× |
| 2 亿次 `while` 累加 | 约 33× |
| 2000 万次方法调用 `a.add(1)` | 约 19× |

只看数量级就好：PyAdamas 的优化遍仍然很轻（折叠/内联/LICM/下标版本化/循环旋转/跨块 CSE，外加后端拷贝合并），赢在「静态类型 + 直接出机器码」；下表是 0.7 口径，0.74 默认 -O2，实际会更快。0.156 之后 `bench/` 的 mandelbrot 同机 release 26.4→12.0ms（约 CPython 23.3×），其余用例持平。
0.7 加入引用计数后，整数路径与 0.6.1 持平（不含临时堆值的语句/循环/
函数会把 RC 脚手架删掉）；方法调用因为 self 形参的 inc/dec 慢约 30%。

### 内置函数逐个对比

`bench/` 下有一套逐个内置函数的对比设施：同一份源码分别喂给 PyAdamas（编译成
原生二进制）和容器内的 CPython 3.13.5，按 CPython 的实测时间校准循环次数，
取多次运行的最小值，并扣掉进程启动开销（CPython 约 6ms，PyAdamas 约 0.5ms）。
口径与跑法见 [development.md](development.md#性能-bench)，逐项结果见
[bench/RESULTS.md](../bench/RESULTS.md)。

结果分四类：

1. **编译期折叠（34~55×）**：静态类型够用时这些内置直接在编译期算掉，循环
   里几乎免费——`isinstance` / `setattr` / `issubclass` / `callable` /
   `hasattr` / `getattr` / `type` / `id`。
2. **整数路径（19~35×）**：`len` / `abs` / `bool` / `range` / `pow` / `sum` /
   `min` / `max` / `hash` / `ord` / `reversed`。
3. **字符串格式化与容器构造（1.6~4.5×）**：`hex` / `bin` / `oct` / `repr` /
   `format` / `str` / `float` / `chr` 走运行时的通用实现，`list` / `tuple` /
   `set` / `frozenset` / `dict` / `map` 的拷贝建表也贵。
4. **其余**：`filter` 16.7×、`int` 15.4×、`round` 13.0×、`all` 10.1×、
   `print` 3.8~11.4×（I/O 主导）、`any` 7.2×、`next` 5.1×、`complex` 4.9×、
   `open` 1.1×（系统调用主导）。

`sorted` / `enumerate` / `zip` 在 0.87 / 0.88 之后也追上了：

| 内置 | 相对 CPython | 做法 |
|---|---|---|
| `sorted` | 约 1.1× | 0.87 换成 Timsort（稳定 + 自适应）；两边都是 C 实现的 Timsort，天花板就是打平 |
| `enumerate` | 约 30.9× | 0.88 在 `for` 里脱糖成计数循环，不再建 `[下标, 值]` 的元组表 |
| `zip` | 约 43.2× | 0.88 在 `for` 里脱糖成双游标，不再建元组表 |

注意：脱糖只覆盖 `for` 循环。`e = enumerate(xs)` / `list(zip(a, b))` 这种
当普通值用的写法仍然物化成列表（可以 `len`、下标），和以前一样。

### range 是惰性对象

`range(...)` 返回的是惰性对象（类型 `range[int]`）：只存起点、终点、步长，
元素按需算。`len(r)` / `r[i]` / `x in r` / `for x in r` / 推导式都不物化，
`range(10**9)` 只占几十字节。`list(r)` / `tuple(r)` / `sorted(r)` 这类需要
真列表时才物化。`repr(r)` 是 `"range(0, 10)"`，和 CPython 一样。

（以前 `range` 作为值使用会实体化成整数列表，`r = range(10**9)` 就是当场
造 10 亿个元素、8GB 内存。现在没有这个坑了。）

还剩一处差异：

- **range 不可变**：`r[0] = 1` 在 PyAdamas 是**编译期**报错（类型检查就拦住），
  CPython 是运行期 `TypeError`。range 也没有 list 的那些方法。

错误措辞按 CPython 对齐了：`min()/max()` 空序列是
`min() iterable argument is empty`，不可变对象的赋值是
`'tuple' object does not support item assignment` 这种 CPython 原话
（类型名不带参数）。
