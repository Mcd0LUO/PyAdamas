# 路线图

按「收益 / 风险」排序，不是按「看起来酷」排序。每项都写了验收标准——没有验收标准的需求不算需求。

## 已完成

| 阶段 | 内容 |
|---|---|
| 词法 / 语法 | 缩进敏感的词法器、递归下降解析器、PEP 701 f-string、全语法 dump |
| 类型检查 | 标注 + 推断 + `unknown` 放行、Optional 收窄、分支合并、内置函数/方法签名 |
| 增量构建 | 拆 TU + `-MMD`，改一个文件不再全量重编 |
| IR | 基本块 + 三地址码 + vreg，类型挂在指令上 |
| 后端 | IR → x86-64 汇编，System V 调用约定，栈帧与槽位复用 |
| 寄存器分配 | 热变量提升：整型/指针用 callee-saved，浮点用 `xmm8`–`xmm15`（仅无调用函数）；块内别名、结果融合、常量延迟、比较连分支 |
| 对象模型 | `class` / 方法 / 字段 / 多继承（C3 MRO）/ vtable 虚调用；属性访问是常量偏移 |
| 内存管理 | 引用计数：隐藏头、临时值栈、字段/列表析构、无 RC 代码的脚手架消除 |
| 闭包 | cell 捕获、`nonlocal`、多层嵌套、自递归、方法捕获 self；函数值是一等闭包对象 |
| 匿名函数 | `lambda` 脱糖成合成嵌套 `def`，复用闭包/cell；函数值默认参数可用 |
| 循环收集 | 保守标记-清扫：堆注册表、全局/临时栈/真实栈/寄存器根、存活堆自适应阈值（下限 2MB）、退出清空 |
| 字典 / 集合 | 开放寻址哈希表 + 插入顺序；字面量、下标读写、`len`、`in`、`for`；RC/GC |
| 容器方法与切片 | dict get/keys/values/items/update、set add/update；str/list/tuple 读切片（缺省/负数/步长） |
| 构造补齐 | 删除类方法、切片赋值、`*args`/`**kwargs`、推导式、`with`、函数装饰器 |
| 异常 | setjmp/longjmp handler 栈；try/except/else/finally、raise、with 异常路径；运行时错误可捕获 |
| 运行时 | 字符串、列表、字典、集合、对象分配、输出、panic；浮点 repr 与 CPython 一致 |
| 验证 | 四类用例（正例 / 反例 / 运行 / 未建模） + 差分测试 + 标准库基线 |
| 0.6–0.38 | 对象模型、异常值、match、C3 MRO、优化遍/O2、块内寄存器；逐版本记录见 [archive/roadmap-0.6-0.38.md](archive/roadmap-0.6-0.38.md) |
| 0.39–0.80 | -O0/O1/O2、dunder 第二批、random/os/subprocess、差分 runner 自举、stdlib 批次（string/time/os/itertools/collections/json）、re 模块 v1/v2；详见 [CHANGELOG](../CHANGELOG.md) |
| 0.96–0.97 | `collections.defaultdict`；`bytes` 收尾：`for c in b"..."`、printf 风格 `%`、`bytes` 标注与跨模块接口 |
| 0.98 | `functools.partial` / `lru_cache`：编译期合成转发闭包与缓存包装 |
| 0.99 | `csv`：纯 PyAdamas stdlib（reader/writer、QUOTE_*）；`file` 标注与模块接口编码修复 |
| 0.100 | 短字符串 freelist、`split` 去泄漏与预分配（`string.capwords` 反超 CPython） |
| 0.101 | 列表结构体/data freelist、`split(sep)` 单遍 + `memchr`、append 快路径 |
| 0.102 | 单遍 `string.capwords` intrinsic、<=7 字节短串 SSO |
| 0.103 | 单字符/True/False/None 静态驻留、`memchr` 搜索、长键哈希缓存、不可变串共享 |
| 0.104 | 字典/集合删除改墓碑 + 插入顺序双向链表，单次删除摊还 O(1) |
| 0.105 | 异常 longjmp 路径回收被跳过帧的局部引用（函数帧记录 + 展开释放） |
| 0.106 | 生成器/协程异常逃出、close 清理抛异常时立即释放帧槽 |
| 0.107 | 自定义方法装饰器；yield/await 挂起点位置放宽 |
| 0.108 | 项目更名 PyAdamas（pyadamas / pa_ / PYADAMAS_ / .pya） |
| 0.110–0.112 | C 互操作 v1–v3：`cimport`/`.cpya` 声明文件、`ptr[T]` 缓冲区传参与 BLAS 封装（`linalg`）、不透明结构体指针 |
| 0.113 | 语言级 `struct` 值类型：C 兼容布局、字段读写、值语义复制、`sizeof`/`addr` |
| 0.114 | C 互操作 v4：`.cpya` 带字段结构体 + `ptr[Struct]` 字段读写；四语言基准；`logging`/`statistics`/`textwrap` |
| 0.115 | C 互操作 v5：结构体按值传参/返回（SysV INTEGER/SSE/MEMORY 分类） |
| 0.116 | C 互操作 v6：`extern` 变量读写、`ptr[T]` 解引用、可变参数 |
| 0.117 | C 互操作 v7：回调（`fn(...)` 跳板 + 闭包槽）、`ptr[T]` 标注 |
| 0.118 | 基准扩充到九个用例（fannkuch / binary_trees / fasta） |
| 0.119 | `base64`/`hashlib` 标准库；跨模块类引用与 bytes 默认值 |
| 0.120 | `hmac` 标准库（MD5 / SHA-1 / SHA-256） |
| 0.121 | release 基准复跑；`pyadamas cdecl` 头文件子集 → `.cpya` |
| 0.122 | 链式调用回归（跨模块类引用修复后已可用） |
| 0.123 | 挂起点泛化：yield/await 可位于任意子表达式与 `except` 体；`yield from` 返回子生成器的 `return` 值 |
| 0.124 | 方法装饰器：`C.m(obj, ...)`、`super().m(...)` |
| 0.125 | 推导式里的 lambda（生成器表达式 + 构造器）；`dict(iterable)` |
| 0.126 | 装饰 dunder 方法（dunder_call 那一批）；装饰器形参类型回填与重查 |
| 0.127 | 生成器 `.throw()`；`yield from` 的 throw 转发 |
| 0.128 | `finally` 体内的挂起（yield / yield from / await）；finalbody 单份编译，异常路径待重抛异常存帧槽 |
| 0.129 | 异步生成器内部 `await` 的结果/异常不再被下轮 `driver_sent` 覆盖 |
| 0.130 | 跨模块 `Generator` / `Coroutine` / `AsyncGenerator` 保留元素与结果类型 |
| 0.131 | 构造、`with` / `async with`、`__call__` 的装饰 dunder 支持 |
| 0.132 | 方法值绑定 self：`obj.m` / `C.m` 得到闭包，支持默认/关键字参数与子类覆盖 |
| 0.133 | `Generator[Y, S, R]` 标注：`send` 实参按 S 检查、`yield` 表达式为 S；模块格式 7 带 S/R |
| 0.134 | `finally` 体内的 `return`/`break`/`continue`；`throw()`/`athrow()` 接受异常类；自定义 `__aiter__` |
| 0.135 | 语言边角收尾：函数内定义类、类体 lambda、`__format__`/`__bool__`/`__reversed__`、`iter(__iter__)`、`hasattr`/`setattr` 扩展、`C.m(obj, ...)`、异步生成器表达式、导入类字段子模式、list/dict 元素 int → float 提升；模块格式 8 |
| 0.136 | 异构元组 `repr()`/`str()`、`set` 元素 int → float 提升、导出类 `__init__` 的 `*args`/`**kwargs`、`asyncio.Queue` 的 float 元素；修正 `pa_dict_key_at` 的浮点返回 ABI |
| 0.137 | 方法装饰器返回类型未知时的 unknown 间接调用；修复恒等装饰器在方法上的误拒 |
| 0.138 | 运行期类对象：类名当值、类装饰器、类值构造与 `isinstance`；新类型 `ClassObj` |
| 0.139 | 跨模块继承：导入类作主基类、字段布局、vtable 外部符号、`super()` 按限定名定位、`isinstance` mirror MRO；模块格式 9 |
| 0.140 | 跨模块继承补完：类对象带 vtable/ctor、动态 classmethod 重绑定、导入类附加基类与槽别名、导入基类字段默认值；模块格式 10 |
| 0.141 | 标准库基础静态包（第一批）：heapq、bisect、binascii、base64 b16/b32/encodebytes、statistics.geometric_mean、textwrap.shorten/缩进、pathlib unlink/rmdir/with_suffix/parts |
| 0.142 | 标准库基础静态包（第二批）：itertools 组合函数、datetime.timedelta（算术/比较/打印）、operator 子集 |
| 0.143 | operator 改为内置模块：算术/比较/逻辑/容器包装函数按调用点特化，itemgetter 支持多键 |
| 0.144 | collections/datetime/pathlib 收尾（deque 方法与元素类型、timedelta 除法、date.replace、Path 字节与路径方法）；新增 tests/bench 标准库基准 |
| 0.145 | 序列重复（`str`/`list`/`tuple` × `int`）与 `base64`/`hashlib` C 内建，标准库基准反超 CPython |
| 0.146 | 引用计数所有权转移（新值写入目标时移动临时栈引用）与 RC/GC 统计，新增 RC 基准 |
| 0.147 | 容器字面量与推导式的 RC 所有权转移，扩展 RC 基准与 149 用例 |
| 0.148 | 裸 list / 空字面量的元素种类延迟定型（kind 3），修复裸 list `pop` 的 use-after-free |
| 0.149 | 分代循环收集：minor 用 trial-deletion、major 用标记-清扫；修复 RC 索引墓碑哨兵误命中 |
| 0.150 | GC 保守根收窄：帧记录扩展成影子根表，不再扫描整段真实栈 |
| 0.151 | 修复 `str(unknown)` 的运行期分派（堆字符串不再被格式化成地址数字） |
| 0.152 | 工程治理：拆分 `lower_method.cpp`（2131 行上帝函数 → 15 个按接收者类型划分的成员函数） |
| 0.153 | 工程治理：拆完 `gen_call_rt` / `builtin_module` / `lower_builtin_call` / `call` / `stmt`，拆分 `ir.h`、`typecheck.h`、`lower_impl.h`，新增 `tests/check_size.sh` |
| 许可证 | Apache-2.0（见 [LICENSE](../LICENSE) / [NOTICE](../NOTICE)） |

## 当前状态（0.153）

流水线 `源码 → 词法 → AST → lambda 脱糖 → 类型检查 → IR → x86-64 汇编 → 可执行文件` 已打通。

- **语言面**：标量、字符串、`bytes`（含迭代与 `%` 格式化）、列表/元组/字典/集合、切片读写、闭包/lambda/`*args`/`**kwargs`/装饰器、推导式、`with`、异常值（`args`/`raise ... from`/自定义异常）、类（字段/方法/C3 多继承/零参 `super()`/`@staticmethod`/`@classmethod`/`@property`/类属性/类装饰器/类名当值）、`match/case`、`del`、复数字面量、f-string 格式说明、dunder 协议（含装饰 dunder、`__call__` 与方法值）、`Generator[Y, S, R]` 标注、`open`/`input`/`sys.stdin`、`struct` 值类型、`cimport`/`cdecl` C 互操作与跨模块继承。
- **标准库**：`math`/`sys`/`asyncio`/`random`/`os`/`os.path`/`subprocess`/`time`/`itertools`/`collections`/`functools`/`json`/`string`/`re`/`datetime`/`pathlib`/`csv`/`base64`/`binascii`/`heapq`/`bisect`/`hashlib`/`hmac`/`logging`/`statistics`/`textwrap`/`operator`，以及封装 OpenBLAS 的 `linalg`。
- **验证**：151 个运行用例、52 个反例、12 个模块目录；差分 runner 已自举，默认 840 例；`tests/bench/` 提供 pyadamas vs CPython 的标准库与 RC/GC 基准（`base64`/`hashlib` 已反超）。
- **主要缺口**：asyncio 边角；调试信息与 traceback；依赖动态属性的 stdlib（如 `argparse`，见「技术债」）。

## 下一步

语言与标准库的完备性优先；C 互操作与 BLAS 已在 0.110–0.120 落地（`cimport`、`ptr[T]` 缓冲区、`linalg` 封装 OpenBLAS），不再是下一步。

### 1. 工程收尾

- 重跑标准库前端基线：最近一次完整口径是 0.81.1（Debian python3.13.5，见 [compatibility.md](compatibility.md#前端鲁棒性基线)），原口径（CPython 3.11.1 + site-packages，38582 个文件）还没重跑；0.133 之后尚未复测；
- 保持 `docs/` 与 README 的验证表同步（运行用例、模块目录等数字按仓库实际统计）；
- `tests/bench/` 标准库基准已固化（0.144）。

## 待决策

这些不是「做不做」，是「选哪条路」，需要先定下来再动手：

| 问题 | 选项 | 影响 |
|---|---|---|
| 字符串语义 | 字节串（现状）/ UTF-8 码点 | 改码点则 `len` 变成 O(n)，索引要解码；不改则非 ASCII 的 `len` 与 CPython 不一致 |
| 整数溢出 | 回绕（现状）/ 溢出检查 / 升格 bignum | 检查要每条算术加一条 `jo`；bignum 要一整套运行时 |
| `print` 求值顺序 | 边算边写（现状）/ 对齐 CPython | 只有副作用可见时才看得出来 |
| 值表示 | 裸 i64（现状）/ 全量 tag / NaN-boxing / 只在 unknown 加 shadow tag | 有 tag 才能对 `unknown` 做运行时分派；全量 tag 值翻倍、寄存器压力减半，NaN-boxing 会废掉 AVX2 归约 |

## 技术债

按「什么时候会疼」排序：

1. **GC 的根只扫登记区，且 stop-the-world**：分代已落地（minor trial-deletion + major 标记-清扫）；GC 不再扫整段真实栈，只扫帧记录登记的 unknown 槽、含 unknown vreg 帧的临时区和 callee-saved 寄存器，临时区仍整段保守扫描；没有增量/并发和栈映射
2. **unknown 引用的计数不精确**：容器 kind 2 元素、unknown 字段与全局已用 RC 索引精确释放（`pa_rc_dec_unknown`）；裸 `list` 的空容器已在第一次写入时定型；仍剩 unknown 类型的局部槽是借来的引用（不计入 rc），靠保守扫栈兜底
3. **Optional 的收窄能力不足**：`x: Optional[str]` 在 `if x is None: ...` 之后、跨语句块或跨函数调用时仍按 Optional 处理；修它需要正经的数据流分析
4. **优化遍仍不完整**：有常量折叠、DCE、小函数内联、LICM、下标版本化/免检查；没有 CSE、强度削减、循环旋转和通用向量化
5. **`tuple` 与 `list` 表示相同**：tuple 也是可变的动态数组，语义上不严谨
6. **诊断不带列区间**：只指向起点，长表达式不好定位
7. **没有增量重编 Python 源码**：每次 `build` 都是全量编译，只有 import 模块有缓存
8. **浮点变量只在无调用函数里提升**：`xmm8–15` 用于无调用函数的跨块浮点值；含调用/handler 的函数仍走内存槽，跨调用要 save/restore
9. **异常对象仍缺 traceback**：有异常值、`args`、自定义异常类和 `raise ... from`，但 `args` 只保存消息
10. **含 handler 的函数没有寄存器提升**：longjmp 会回滚 callee-saved 寄存器，只能全走内存槽
11. **生成器/协程的剩余边角**：`finally` 体内的挂起与 `return`/`break`/`continue` 跳出都已支持（finalbody 在可挂起函数里只编译一份，正常与异常路径汇聚；跳出路径内联 finalbody；close 重放到挂起点时报 `RuntimeError`，重放跳出会恢复 finally 词法上所属的循环）。未标注 `Generator[Y, S, R]` 时 `send` 与 `yield` 表达式静态是 unknown（`await` 已按 `Coroutine[T]` / `Awaitable[T]` 定类型）
12. **asyncio 仍是最小集合**：`async for` 已支持自定义 `__aiter__`，但 fd 读写、asyncio 子进程、线程安全循环未支持；`Queue` 的 float 元素需要 `asyncio.Queue[float]` 这类标注把元素类型带进构造，未标注队列仍按引用语义；`wait_readable` 不是 CPython 标准 API；Future 回调同步执行；`gather` 结果按 unknown 容器处理；Task 只允许一个并发等待者
13. **stdlib 覆盖不足**：没有 `argparse`（返回对象要有动态属性，和静态布局冲突）；`logging`/`statistics`/`textwrap`/`base64`/`hashlib`/`hmac`/`csv` 已接上子集，`functools` 已覆盖 `reduce`/`wraps`/`partial`/`lru_cache`（`lru_cache` 无淘汰、键类型受限），`json`/`collections`/`itertools` 是子集；`re` 有后向引用/四向断言/原子组/内联标志，但缺 Unicode 属性类和 `findall` 的分组语义
14. **C FFI 与调试信息仍不全**：`cdecl` 只解析头文件子集（不展开宏函数/条件编译，C `float`、union 按值、定长数组不生成）；回调槽不可重入；没有包管理和 traceback
15. **`re` 是纯回溯 VM**：递归实现，深模式或灾难性模式会吃栈，只靠 2 亿步上限兜底；没有 DFA/JIT，`findall`/`split` 每次重新编译模式字符串
16. **形参类型推断只做单态**：未标注形参由调用点回填，同一形参吃多种类型时退回 unknown（`print` 会按整数输出）；嵌套函数不参与重查。根治要运行时类型标签，代价是全量 tag 或 NaN-boxing（见「待决策」）
