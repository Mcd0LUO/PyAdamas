# 标准库自举第三批（stdlib-batch3）

> 分支 **stdlib3**，基线 master 1f3daba（0.337）。这一批只做**纯 PyAdamas
> 源码**：stdlib/*.py + 模块表登记，不碰 lower / runtime / codegen，也不加
> C 胶水。轻量标尺（用户明确）：同参数、同功能、同结果；错误类型对、消息
> 合理但不逐字复刻 CPython；输出格式合法但不字节级对齐；差分只钉功能输出。
> CHANGELOG 预留 **0.339**（borrow 分支占 0.338，集成时按落地顺序重编号）。

## 一、选型与结论

| 模块 | 日常频率 | 纯 pa 可行性 | 结论 |
|---|---|---|---|
| urllib.parse | 高（配 urllib.request） | 纯字符串，可行 | ✅ |
| secrets | 中 | 有 random 源，可行（见边界） | ✅ |
| calendar | 中 | 纯整数日期算法，可行 | ✅ |
| struct | 高（二进制协议/文件） | 位级浮点纯算术可算，可行 | ✅ |
| queue | 中 | 纯 list/heapq，M=1 协作式无锁 | ✅ |
| contextlib | 中 | 部分可行（@contextmanager 异常注回做不到） | ⚠️ 子集 |
| difflib | 中 | 纯序列算法，可行 | ✅ |
| pprint | 中 | **不可行**：unknown 值没有运行期类型标签 | ⏭ 跳过 |
| tarfile | 低 | 不可行：纯 py 侧没有 open()/通用文件对象 | ⏭ 跳过 |
| traceback | 低 | 不可行：异常值没有 traceback 对象 | ⏭ 跳过 |
| dataclasses | 低 | 不可行：类布局编译期固定、无注解反射 | ⏭ 跳过 |

## 二、交付清单

| 文件 | 内容 |
|---|---|
| stdlib/urllib/parse.py | urlparse/urlsplit/urlunparse/urlunsplit/urljoin/urldefrag、quote/quote_plus/quote_from_bytes/unquote/unquote_plus/unquote_to_bytes、urlencode/parse_qs/parse_qsl、uses_* 表、ParseResult/SplitResult/DefragResult |
| stdlib/secrets.py | token_bytes/token_hex/token_urlsafe/randbelow/randbits/choice/compare_digest |
| stdlib/calendar.py | isleap/leapdays/weekday/monthrange/monthcalendar/timegm、day_name/day_abbr/month_name/month_abbr、weekheader、firstweekday/setfirstweekday、month/prmonth/calendar/prcal、mdays、error/IllegalMonthError/IllegalWeekdayError |
| stdlib/struct.py | pack/unpack/unpack_from/calcsize/iter_unpack、error；格式前缀 < > ! = @，类型码 x ? b B h H i I l L q Q f d c s p |
| stdlib/queue.py | Queue/LifoQueue/PriorityQueue/SimpleQueue、Empty/Full、put/put_nowait/get/get_nowait/qsize/empty/full/task_done/join |
| stdlib/contextlib.py | nullcontext、@contextmanager、_GeneratorContextManager/ContextManager |
| stdlib/difflib.py | SequenceMatcher（含 junk/autojunk）、Match、get_close_matches、unified_diff |

模块表（src/driver/build.cpp 的 kModuleTable）逐个登记：urllib.parse、
secrets、calendar、struct、queue、contextlib、difflib。

## 三、各模块语义边界（轻量标尺）

### urllib.parse
- 只收 str：CPython 的 *Bytes 变体与 bytes/bytearray 输入不做。
- 结果对象是普通类（不是 tuple 子类）：字段读、下标、len、repr、geturl
  可用；元组解包/对象 == 不支持（拿字段自己比）。
- urlunparse/urlunsplit 按文档口径收 5/6 元组；结果对象用 .geturl()。
- query/fragment 的「空但存在」与「缺失」不区分（拼接输出相同）。
- netloc 的 IDNA/NFKC 安全校验、Windows 盘符特例不做；控制字符仍按
  WHATWG 规则剔除（前导 C0/空格、\t\r\n）。urldefrag/urljoin 走 RFC 3986
  合并；CPython 对非法 URL 的部分 ValueError 不做。

### secrets
- **随机源是内建 random**：pa 运行时没有 os.urandom/getrandom 原语，纯源码
  拿不到内核熵——接口与计算口径（字节数/编码/区间/拒绝采样）一致，但令牌
  强度弱于 CPython，不能用于真实安全用途。
- nbytes 省略/负数走 CPython 默认 32（CPython 用 None 哨兵，pa 的 int 形参
  默认值只能是字面量）。
- randbits(k) 走 int64：k <= 63 非负；k > 63 按 int64 回绕。
- compare_digest 恒定时间比较；pa 的 bytes 与 str 共用运行期表示、unknown
  实参分不出类型，CPython 的「str/bytes 混比抛 TypeError」不复刻（同类型
  输入结果一致，任意字节值的 bytes 也比得对）。

### calendar
- 不依赖 datetime：星期/timegm 用 days_from_civil 整数算法，1..9999 年与
  CPython 一致（超范围年份按 CPython 的 `2000 + year % 400` 归一）。
- day_name/month_name 等是普通 list（len/下标/切片），无 locale 切换。
- 没有 Calendar/TextCalendar/HTMLCalendar 类；month/calendar/weekheader 按
  模块级函数提供（CPython 里它们也只是 TextCalendar 方法别名）。
- 文本日历与 CPython **逐字节一致**：包括 str.center 的奇数补白规则
  （pa 的 str.center 与 CPython 不同，这里自带 _center）。
- IllegalMonthError 只继承 ValueError：pa 的自定义异常类不支持多继承，
  CPython 的 ValueError+IndexError 双基类做不到。

### struct
- 没有 pack_into：pa 的 bytes 不可变，没有可写缓冲原语（用 pack + 拼接；
  unpack_from 用切片实现）。
- 整数是 int64：Q/L 解出高位为 1 的数按 int64 回绕成负数（CPython 任意精度）；
  pack 侧接受负数按补码写入。
- < > ! = 用标准尺寸（l=4）；只有 @ 用本机尺寸（x86_64 上 l=8）并对成员按
  自身尺寸对齐。
- 浮点位级编解码（double/float32）是纯算术实现：含符号零、次正规、inf/nan、
  就近舍入；对拍 CPython 的 pack 字节完全一致。
- s/p/c 收 bytes 或 str：bytes 实际按「码点 -> UTF-8」打包（ASCII 完全一致；
  非 UTF-8 字节串请先自己 encode）。反解出的 s/p 值按 bytes 标记，但 print
  会按 str 渲染（pa 的容器元素过 unknown 后丢 bytes 标签）。
- unpack* 返回元组；iter_unpack 返回 list（CPython 是迭代器）。

### queue
- **线程安全**：pa 运行时是 M=1 协作式（vthread 单 OS 线程、全局态无锁），
  所以不加锁、不模拟 Condition；阻塞 put/get 用 time.sleep 轮询（sleep 是
  vthread 让出点），带 timeout 的路径与 CPython 同语义。单线程里阻塞 put/get
  会一直等（同 CPython 死锁用法）。
- PriorityQueue 用 heapq，元素按 < 比较。

### contextlib
- **@contextmanager 无法把 with 体的异常注回生成器**：异常实例/类跨 unknown
  形参会退化成裸 8 字节（生成器 throw() 只认静态异常名/实例），所以被装饰的
  生成器里 `except E:` 捕获不到 with 体抛出的异常。实现口径：异常路径对生成器
  close()，finally/with 清理照常执行，异常原样外抛（从不吞异常）。
- 包装函数只转发 0~4 个位置参数（f(*args) 对 unknown 被装饰者只能定长），
  关键字参数抛 TypeError。
- suppress()/closing() 跳过：suppress 要把异常类当普通值存起来再和 with 协议
  的异常类比较，但异常类过 unknown 参数只剩 0（实测 `def f(*excs)` 里
  `str(ValueError)` 是 "0"）；closing 要在任意对象上调用 .close()，pa 对
  unknown 值不支持方法调用（codegen 直接拒绝）。

### difflib
- SequenceMatcher 的 a/b 先转成统一序列（str 按码点拆、list 原样）；元素按值
  比较/哈希；junk 函数与 autojunk 的 popular 剔除同 CPython。
- unified_diff / get_grouped_opcodes 返回 list（CPython 是生成器）；
  unified_diff 内容行只加前缀、不补 lineterm（同 CPython），lineterm 只用于
  文件头与 @@ 行。
- 不建模 IS_LINE_JUNK / IS_CHARACTER_JUNK、ndiff、context_diff。

## 四、跳过清单（含实测证据）

### pprint —— unknown 值没有运行期类型标签

pprint 的 API 是「对任意对象递归 repr」，而 pa 的 `unknown` 是个裸 8 字节
槽：只有堆对象（str/list/dict）带类型标签。实测（`def show(x: unknown)`，
把各种值传进去后打印 isinstance 矩阵与 repr）：

| 实参 | isinstance int | bool | tuple | set | dict | str | repr/str |
|---|---|---|---|---|---|---|---|
| 1 | False | False | False | False | False | False | 1 |
| True | False | False | False | False | False | False | 1（与 1 不可分） |
| None | False | False | False | False | False | False | 0（与 0 不可分） |
| (1, 2) | False | False | False | False | False | False | 指针数字 |
| {1, 2} | False | False | False | False | **True** | False | {1: 0, 2: 0} |
| b"xy" | False | False | False | False | False | **True** | 'xy' |

bool/None/int 同为裸整数、tuple 无标签、set 被识别成 dict、bytes 与 str 混同，
于是 pformat(True)=='1'、pformat(None)=='0'、元组打印成指针、集合打印成
内部 dict。这不是「消息/格式没对齐」，而是功能上不可能正确——按约定跳过。

（过程中写过一版，能正确处理 str/int/float/list/dict，但 bool/None/tuple/set
全错，属于「硬写」，已删除，未进提交历史。）

### tarfile
纯 PyAdamas 侧没有 open()/文件对象原语（CSV/gzip/zipfile 都走原生类型或
C 胶水）。tarfile 的 API 是 fileobj/tarfile 读写协议，纯源码接不上；
Byte IO 只有 bytes/str，没有流式读写与 seek。跳过。

### traceback
pa 的异常值没有 traceback 对象（文档：args 只保存消息），也没有
sys.exc_info()，format_exc/print_exc 无从取材。跳过。

### dataclasses
@dataclass 需要在运行期读类的字段注解、生成 __init__/__repr__/__eq__；pa 的
类布局与 vtable 在编译期固定，没有注解反射，也没有动态类/方法生成。跳过。

## 五、测试

tests/run/320-327（323 是 pprint 的空号，模块跳过所以未落地）：

| 用例 | 覆盖 |
|---|---|
| 320_urllib_parse.py | 拆分/拼接/相对合并/百分号编解码/表单编码/parse_qs |
| 321_secrets.py | 令牌长度与字符集、randbelow/randbits 区间、choice、compare_digest、错误分支 |
| 322_calendar.py | 闰年/星期/月历矩阵/timegm/文本月历与年历/异常 |
| 324_struct.py | calcsize/pack/unpack/unpack_from/iter_unpack、大小端、native 对齐、float32/double、pascal 串、错误分支 |
| 325_queue.py | 三种队列顺序、maxsize/full、Empty/Full、timeout、task_done/join |
| 326_contextlib.py | nullcontext、@contextmanager（含嵌套/多参/finally 清理/异常路径） |
| 327_difflib.py | SequenceMatcher 各方法、junk/autojunk、get_close_matches、unified_diff |

每个用例先跑 CPython 3.14.4 对拍再钉 `# --- expect ---`；320/321/322/324/325/
327 的输出与 CPython **逐字节一致**（322 的月/年历文本也一致）。

## 六、已知运行时问题（不属于本批改动）

@contextmanager 生成器 yield 一个**元组**、且后面紧跟另一个带 try/finally 的
@contextmanager 时，ASan detect_leaks=1 会在进程退出时报 176 字节泄漏
（一个由 pa_tuple_new_filled 分配、被 `yield 1, 2` 的 resume 帧持有的元组）。
最小复现：两个 `GeneratorContextManager` 连续 with，第一个 yield 元组、第二个
生成器带 try/finally；把顺序换成「cleanup 在前」或把 yield 值换成 list 就不
泄漏。判断是生成器 resume 帧/临时槽在 with 结构里的保留问题（codegen/
runtime），stdlib 侧改不掉；本批测试把 yield 元组的用例放在文件末尾规避，
7 个新用例 detect_leaks=1 全干净。

## 七、闸门（stdlib3 分支最终状态）

| 闸门 | 结果 |
|---|---|
| 全套件 normal（PYADAMAS_TEST_JOBS=8） | **EXIT=0，312 个 ran ok**（295 基线 + 7 新增 + 其它基线用例），0 FAILED/DIFF |
| 全套件 ASan（PYADAMAS_TEST_ASAN=1，JOBS=1） | **EXIT=0，312 个 ran ok**，0 FAILED/DIFF |
| 新用例 O0/O1/O2（320/321/322/324/325/326/327） | 三档输出与钉住的 expect **逐字节一致** |
| 新用例 ASan detect_leaks=1 | 7/7 干净（无 LeakSanitizer 报告；见第六节的已知交互） |
| difftest（tests/diff/run_diff.py，seeds=3） | **840 个程序，0 个失败** |
| check_size | 无**新增**超限文件：5 个超限文件是基线就超的（codegen_regalloc 2539、driver_build 1540→1547、ir/ops.h 1350、lower/impl.h 1342、typecheck/types.h 1337）；本批 7 个新模块最大 564 行 |

构建：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` +
`cmake --build build -j18`（只在本 worktree 的 build/，不碰主树）。
