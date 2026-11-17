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
| 循环收集 | 分代收集：minor trial-deletion + major 标记-清扫；堆注册表、全局/临时栈/帧记录登记区/寄存器根、存活堆自适应阈值（下限 2MB）、退出清空 |
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
| 0.154 | 工程治理：拆完剩余 7 个大文件（`typecheck_call` / `lower_expr` / `lower_import` / `lower_stmt` / `lower_gen` / `lower` / `cdecl`），全部源文件 ≤1200 行 |
| 0.155 | 浮点数值循环优化：块内 xmm 临时寄存器、浮点比较分支融合、短路布尔槽转发；mandelbrot 同机 1.87× |
| 0.156 | 拷贝合并与跨块公共子表达式消除：`opt_cse.cpp` 前向可用表达式；mandelbrot release 26.4→12.0ms（约 CPython 23.3×） |
| 0.157 | 浮点比较分支去掉 NaN 专用跳转：有序比较交换操作数、改用 `ja`/`jae`，内层循环少一条分支 |
| 0.158 | 正确性 + asyncio 收尾：NaN `!=` 分支修复、生成器浮点提升状态修复、Task 多等待者与结果类型、gather 结果类型、from-import asyncio 特化；基线 281/596 |
| 0.159 | 调试信息：IR 指令带源码行，生成代码发 DWARF `.file`/`.loc` 与可读函数符号；`addr2line`/`readelf` 可映射回源码 |
| 0.160 | 运行时 traceback：codegen 发 PC→(函数, 行号) 表并在模块入口注册；未捕获异常沿 rbp 链打印 `Traceback (most recent call last)` |
| 0.161 | 模块缓存原子写 + async/generator 逻辑帧：并发编译不再读到截断文件；未捕获异常追加协程/生成器 resume 帧 |
| 0.162 | 调试信息收尾：每个生成函数带 CFI（`.eh_frame`）与 `.type`/`.size`，手写汇编（析构器 / GC release / 回调跳板 / init 桩 / main）同样覆盖；新增 `cfi:unwind` 回归 |
| 0.163 | 回溯走标准展开器 + 去掉帧指针：`pa_print_traceback` 改用 `_Unwind_Backtrace`，运行时不再强制 `-fno-omit-frame-pointer`；生成代码槽位改相对 rsp、出参区在帧里预留、`%rbp` 并入变量寄存器池，函数入口与循环头 16 字节对齐；修复跨多个挂起点的值不落槽 |
| 0.164 | 前端基线：dunder 返回类型改到函数体推理之后判、`getattr` 支持缺省值参数；标准库前端基线 281 → 294 / 596 |
| 0.165 | 动态属性 + 反射放宽：类实例可挂惰性 `dict[str, unknown]`，`setattr`/`getattr`/`hasattr` 落到运行期查表；非类接收者的反射在 check 阶段放过；基线 294 → 320 / 596 |
| 0.166 | 跨模块 super()：定义类的 MRO 里有导入基类时，`super()` 的方法在 check 阶段看不见，只在纯本地继承链都找不到时报错；有导入基类/`open` 类返回 unknown 交给 build；基线 320 → 335 / 596 |
| 0.167 | 自定义异常类当普通类：有字段/方法的富异常是真实 vtable 实例，`raise`/`except as e` 走运行期实例；内置异常属性保守放过；基线 335 → 348 / 596 |
| 0.171 | 继续收口散簇：模块级 undefined name、None 接收者全放行、type 对象动态调用、super 上下文丢失、多继承字段布局降级、推断形参/*args 可重绑定；基线 435 → 501 / 596 |
| 0.170 | 一键扫荡：2参 super、no-__init__ 带参实例化、unpack(generator/range/str/Optional)、type 元数据属性、异常值方法、推断形参可重绑定；基线 411 → 435 / 596 |
| 0.169 | getattr 属性名非字面量放宽 + 内置异常多参构造/关键字：args 元组跨 raise 还原（pending_exc_args）；基线 374 → 411 / 596 |
| 0.168 | isinstance/bytes：第二参数接受 `bytes`/`bytearray`/`type`/`slice`/`frozenset` 等与 `(A,B,...)` 元组/列表；运行期元组短路 OR、known 类型静态判定、unknown 带运行期标签内置判定；`bytes()` 2 参/可迭代-of-int、`bytes*int`、`decode` 0..2、`translate`、内置类型名当值；基线 348 → 374 / 596 |
| 0.185 | **字符串码点语义 S1+S2**：`PyAdamasStr` 扩到 32 字节、`pub`（str 码点数 / bytes 字节数）放偏移 0，`len` 与真值判断的指令一行未改；字面量在编译期算好 `pub` 写进 `.data.rel.ro`；`intern_string` 按 `(kind, 内容)` 去重；下标/切片/迭代/方法/格式化宽度改码点语义，`bytes` 全部保持字节；新增内置 `len_bytes(s)`；顺序迭代加游标（`for`/`enumerate`/`zip`/推导式/生成器），全 ASCII 走快路径；`cimport` 取 `data` 偏移 16→24；str 的 `\xNN` 改成 U+00NN 编 UTF-8、bytes 的八进制/\xNN 保持原始字节 |
| 0.186 | 字符串码点语义 S1+S2（详见 [str-codepoints.md](str-codepoints.md)） |
| 0.188 | 间接调用统一 ABI 约定 + 每函数 shim，修浮点经 unknown 函数值传递的静默算错 |
| 0.189 | 形参 float/非 float 混用改编译期报错 |
| 0.190 | 无浮点签名 shim 退化成单条 `jmp` |
| 0.191 | Z2-b3：C 运行时抛出的异常也能被零开销 try 抓到 |
| 0.192 | Z3：多 handler 分派链 + typed catch 接入零开销路径 |
| 0.193 | itertools 补 6 个算子（stdlib 自举） |
| 0.194 | Z4：finally 接入 |
| 0.195 | Z4：早退穿过零开销保护区 |
| 0.196 | Z4：嵌套 try 接入；一份 LSDA 多条 call-site 记录 |
| 0.197 | Z4：try-else 接入；with 在 try 内明确排除 |
| 0.198 | Z5 第一步：模块顶层的 try 也走零开销路径（`_setjmp` 582 → 429） |
| 0.199 | 标准库新增 `fnmatch` / `copy` |
| 0.200 | 恢复四语言经典基准并重跑 |
| 0.201 | 9 个基准用例全部补上类型标注 |
| 0.202 | Z5 完成：表驱动成为默认路径，`_setjmp` 670 → 361 |
| 0.203 | 寄存器分配 R1–R3；正确性收益明确、性能收益为零 |
| 0.204 | R4 浮点线性扫描 + matmul 基准改函数形式 |
| 0.205–0.209 | IR 重构四步：稳定指令 ID / 块 ID、`uses_of`/`def_of` 反向索引、`Operand` 带 tag、DCE 改用索引；决策记录「暂不上 SSA」（见 [ir.md](ir.md)） |
| 0.210–0.213 | 正确性修复：>6 参数含浮点的间接调用丢实参、堆槽被 LICM 外提、RcMark 被寄存器化、切片赋值缺元素类型检查；新增 `tools/difftest.sh`（O0/O2 输出必须一致） |
| 0.214 | `join(T, None)` 返回 `Optional[T]` 而非 unknown，热循环 34–36ms → 6–7ms（约 4.7×） |
| 0.215–0.216 | unknown 值的两类 use-after-free 根治：返回值/存储的引用所有权缺失、unknown 槽被提升导致 GC 根缺失 |
| 0.217–0.218 | ASan 成为一等公民：`--asan`/`--ubsan`、`PYADAMAS_TEST_ASAN=1` 整套件、运行时内部插桩 `rt_asan.o`；顺带修 `json.loads` 浮点返回值错位 |
| 0.219–0.233 | **泄漏清零**：handler 记录生命周期、异常对象所有权、json/pathlib 引用、异常子系统 shutdown 审计；新增 `runtime/rt_diag.c` 诊断模块。188 用例泄漏 **23 处 → 0** |
| 0.234 | GC 零回收退避：容器段 major 8211 → 22 次（阈值必须有绝对下限，否则退化成每次分配都触发） |
| 0.235 | 容器逐元素堆位图：`dec_unknown` 467613 → 17418（-96%），但总耗时未变——否定了「索引查询是瓶颈」的假设 |
| 0.236 | 按位置放宽 exact-RC：exact-RC 用例 41 → 50（unknown 只在容器元素位置时不必保留全局索引） |
| 0.237 | 按需关闭索引维护：`ir_module_can_skip_index_maint` 与 `needs_unknown_index` 正交，`bench_rc_gen` 0.75x → 0.90x |
| 0.238 | minor 阈值改用 Go pacer 公式（按存活堆比例算），GC 次数 34 → 7 |
| 0.239 | 增量清扫（照抄 Go 并发 GC 结构）：最大停顿 5.09ms → **1.68ms（-67%）**，代价是吞吐 +2.7% |
| 0.240 | inc/dec 内联（抄 Swift ARC）：`rt.o` 符号引用 239/236 → 0，性能 +1.3% |
| 0.241–0.242 | RAII 作用域守卫（GCC `cleanup` 属性）：13 处 pin + 2 处 free 守卫，手工 `pa_gc_pin` 71 → 62 |
| 许可证 | Apache-2.0（见 [LICENSE](../LICENSE) / [NOTICE](../NOTICE)） |

## 当前状态（0.242）

流水线 `源码 → 词法 → AST → lambda 脱糖 → 类型检查 → IR → x86-64 汇编 → 可执行文件` 已打通。

- **语言面**：标量、字符串、`bytes`（含迭代与 `%` 格式化）、列表/元组/字典/集合、切片读写、闭包/lambda/`*args`/`**kwargs`/装饰器、推导式、`with`、异常值（`args`/`raise ... from`/自定义异常）、类（字段/方法/C3 多继承/零参 `super()`/`@staticmethod`/`@classmethod`/`@property`/类属性/类装饰器/类名当值）、`match/case`、`del`、复数字面量、f-string 格式说明、dunder 协议（含装饰 dunder、`__call__` 与方法值）、`Generator[Y, S, R]` 标注、`open`/`input`/`sys.stdin`、`struct` 值类型、`cimport`/`cdecl` C 互操作与跨模块继承。
- **标准库**：`math`/`sys`/`asyncio`/`random`/`os`/`os.path`/`subprocess`/`time`/`itertools`/`collections`/`functools`/`json`/`string`/`re`/`datetime`/`pathlib`/`csv`/`base64`/`binascii`/`heapq`/`bisect`/`hashlib`/`hmac`/`logging`/`statistics`/`textwrap`/`operator`/`fnmatch`/`copy`，以及封装 OpenBLAS 的 `linalg`。
- **验证**：188 个运行用例、53 个反例、12 个正例、6 个严格用例、1 个未建模、15 个模块目录；`tests/run_tests.sh` 全量 **205 ran ok**（= 188 运行 + 15 模块 + 1 cdecl + 1 cfi），exit 0。差分 runner 已自举，默认 840 例；另有 `tools/difftest.sh` 做 O0/O2 自差分（188 用例 0 不一致）。`tests/bench/` 提供 pyadamas vs CPython 的标准库与 RC/GC 基准（`base64`/`hashlib` 已反超）。
- **内存正确性（0.215–0.233）**：ASan 全量 205 ran ok；**泄漏扫描 0 处 / 188 用例**（0.217 首次开 `detect_leaks=1` 时为 23 处）。两类 unknown 值的 use-after-free 已根治（返回值/存储所有权、unknown 槽被提升导致 GC 根缺失）。剩余已知不精确点见「技术债」第 2 条。
- **诊断工具（0.227–0.231）**：`runtime/rt_diag.c` 独立模块，`PYADAMAS_RC_TRACE` / `PYADAMAS_RC_TRACE_MIN` / `PYADAMAS_RC_TRACE_KIND` / `PYADAMAS_EXC_TRACE` / `PYADAMAS_DIAG_DEPTH` 追踪对象分配、释放、临时栈进出与异常所有权转移，退出汇总未释放对象并给出源码行归因。关闭时零输出、零可测开销。用法与两个陷阱见 [development.md](development.md)。
- **IR 基础设施（0.205–0.209）**：`Inst::id` / `Block::id` 稳定标识（不随 `vector::insert/erase` 与新增块失效）、`uses_of` / `def_of` / `inst_by_id` / `block_by_id` 反向索引、`Operand` 带 tag（`PYADAMAS_IR_CHECK=1` 校验 op 与 kind 一致性）。`EhRegion` 不再存会过期的块号。**明确不上 SSA**，理由见 [ir.md](ir.md)。
- **优化**：O2 有常量折叠/DCE/小函数内联/静态调用直接化/LICM/检查型下标外提/循环旋转/跨块 CSE；后端有拷贝合并、浮点临时寄存器与浮点比较分支融合（有序比较不再补 NaN 跳转）。
- **asyncio**：Task 支持多等待者；`gather` 结果元素所有权按静态类型带入运行时；`from asyncio import ...` 保留调用点特化；`run`/`wait_for` 跨模块结果仍可能退化为 unknown。
- **调试信息**：生成代码带 DWARF 行号、CFI（`.eh_frame`）与可读函数符号（`.type`/`.size`）；帧里没有 rbp，gdb、libgcc 的展开器和运行时自己的 traceback 都靠 CFI 逐帧还原。未捕获异常打印函数级 traceback，异步/生成器 resume 帧也会追加（仅函数名）。异常值对象仍不带 traceback。
- **Windows 目标**：`--target=win64` 产出可原生运行的 PE 可执行文件（已在 Windows 宿主机实测）。
  ABI 差异集中在 `TargetABI`（实参寄存器、影子空间、callee-saved 集合、setjmp 符号名）；
  运行时通过 `rt_internal.h` 的平台层把 poll/clock_gettime/nanosleep/mkdir/realpath/
  sysconf/timegm 映射到 Win32，subprocess 用 CreateProcess + 匿名管道；Setjmp 换成自带的
  `pa_win_setjmp`（不依赖展开信息，因为生成的汇编函数没有 `.pdata`）。覆盖面 138 / 146。
- **主要缺口**：asyncio 边角；异常对象 traceback / 列号 / `.debug_info`；依赖动态属性的 stdlib（如 `argparse`，见「技术债」）；字符串码点语义 S3（格式化宽度、`re`、文件 I/O，见 [str-codepoints.md](str-codepoints.md)）。性能瓶颈：**顶层变量是全局、不参与寄存器分配**（见「下一步」，归因已按 0.233 后的实测修正）。
- **零开销异常（Z1–Z5 已完成）**：Itanium ABI 表驱动展开，**默认路径**（0.202，`--no-zero-cost-exc` 可回退）。
  非可挂起函数里的 try **全程无 `_setjmp`**：personality routine + LSDA 发射器 +
  运行时胶水（`pa_eh_throw`/`pa_eh_landing`/`pa_eh_rethrow`）+ codegen 接线。
  支持单层与嵌套 try、多 typed handler、裸 except、finally、try-else、return/break/continue 穿过保护区、
  Python `raise` 与 C 运行时异常（IndexError / ZeroDivisionError / KeyError）。
  LSDA 字节布局用 gcc 14.2 实测标定（`tools/lsda_ref/`，100+ 条可复跑断言），
  handler 判据与真 `__gxx_personality_v0` 交叉验证过。
  四套夹具：`z1_rt` 116 项、`z2_rt` 134 项、`z2_codegen` 209 项、`tests/eh` 33 项。
  Z5 后全量用例 `_setjmp` 从 670 降到 361（-46%）。
  **永久排除**：with 在 try 内（with 用 setjmp 保护 `__exit__`，与 `_Unwind` 混用不可靠，实测 0 用例用到）、
  生成器/协程/异步生成器（5 处 C 级 setjmp 兜底抓不到 `_Unwind`，设计上的永久降级）、Win64（SEH 是另一套机制）。
  设计见 [zero-cost-exceptions.md](zero-cost-exceptions.md)。
- **寄存器分配（R1–R4，0.203–0.204）**：活跃性 + 线性扫描 + caller 池 + 浮点线性扫描，正确性收益明确但性能收益为零；根因见「下一步」。0.211 修掉两个被分配策略变化顶出来的既有 bug（堆槽被 LICM 外提、RcMark 被寄存器化），并回退了「块内 vreg 进 callee 池」——`vreg_off()` 仍指向栈槽，任何读栈槽的路径都拿到过期值，要安全地做必须先重写 RC 临时栈与 vreg 栈槽的耦合。0.212 修正了一处**误导性调试输出**：浮点槽在调用方随后才分配，原统计把它们算成「未提升」，长期显示 `7/14 slots promoted`，实际是整型 7/8、浮点 6/6 **已接近饱和**——这个错误数字曾导致「寄存器分配还有很大空间」的误判。设计见 [regalloc.md](regalloc.md)。
- **GC 根扫描收窄（0.175）**：临时区保守扫描改为按 vreg 静态类型发射位图，
  纯标量帧整段不扫；全量 163 个用例合计临时区实扫字数 2901 -> 403、
  保守保留对象 92 个 / 8144 B -> 0。诊断 `PYADAMAS_GC_STATS=1`，回退 `PYADAMAS_GC_CONSERVATIVE=1`。

## 下一步

性能优化（先解决模块顶层函数边界）与语言/标准库完备性并行；C 互操作与 BLAS 已在 0.110–0.120 落地（`cimport`、`ptr[T]` 缓冲区、`linalg` 封装 OpenBLAS），不再是下一步。

### 0. 性能瓶颈的根因（归因已按实测修正，路线待决策）

寄存器分配 R1–R4 已落地（0.203–0.204，见 [regalloc.md](regalloc.md)）：基于活跃区间的线性扫描、caller 池（r10/r11）、浮点也走线性扫描。正确性收益明确，但**性能收益为零**。

原先的归因是「`__pa_main` 有 106 个 call / 42 个块，调用密度极高，活跃区间几乎都跨调用点，caller 池条件命中不了」。**0.233 后重新实测，这个归因不准确**，两点需要修正：

**① 那个 106 call / 42 块的数字描述的是 0.204 之前的 matmul。** 0.204 已把热循环包进 `run()`，现在 matmul 的 `__pa_main` 只有 3 个块 / 17 个 `call.rt`。仍有顶层热循环的是 spectral_norm / fannkuch / fasta（nbody 的顶层只是初始化，热循环在 `advance()` 里）。

**② 顶层的调用几乎全是 `call.rt`，用户调用是 0–2 个。** 实测各基准 `__pa_main` 内部：

| 基准 | blocks | call.rt | call.direct | load.global | store.global | load.slot | store.slot |
|---|---|---|---|---|---|---|---|
| spectral_norm | 40 | 43 | 0 | 39 | 16 | 34 | 24 |
| fasta | 10 | 34 | 1 | 19 | 9 | 2 | 4 |
| fannkuch | 5 | 30 | 1 | 13 | 6 | 2 | 4 |
| nbody | 8 | 26 | 2 | 49 | 7 | 4 | 4 |
| matmul | 3 | 17 | 1 | 8 | 5 | 1 | 2 |

复现命令（逐个换文件名即可）：

```bash
./build/dev/pyadamas ir bench/compare/spectral_norm.py | awk '
  /^func / { m = ($0 ~ /__pa_main/) }
  m && /^bb/ { b++ } m && /= call\.rt/ { rt++ } m && /= call\.direct/ { d++ }
  m && /load\.global/ { lg++ } m && /store\.global/ { sg++ }
  m && /load\.slot/ { ls++ } m && /store\.slot/ { ss++ }
  END { printf "blocks=%d call.rt=%d call.direct=%d load.global=%d store.global=%d load.slot=%d store.slot=%d\n", b, rt, d, lg, sg, ls, ss }'
```

`pa_*` 是 C 运行时，碰不到用户全局。所以「调用密度高导致 caller 池命中不了」不是真障碍。**真障碍是这些值压根不在分配器的候选集里**：R1–R4 只处理**槽位（slot）**，而顶层变量是**全局（global）**。spectral_norm 顶层热循环（`bench/compare/spectral_norm.py:38`）的 `v_bv` / `v_vv` / `i` / `N` 全是标量全局，一个寄存器都拿不到，每轮迭代都走内存。

0.204 手工把 matmul 包进 `run()` 就见效，侧面印证症结是「变量是不是局部槽」，而不是「调用有多少」。

#### 两条候选路线（尚未定案）

| 路线 | 做法 | 代价与风险 |
|---|---|---|
| **A. 标量全局提升** | 把分配器候选集从 slot 扩到**标量** global，复用现有活跃区间 + 线性扫描 | 不改程序结构，语义 / traceback / 异常路径零风险。标量不是 GC 根，提升到 callee-saved 后不需要在 `call.rt` 前写回，绕开 R4 列的 GC / 展开耦合。**主要工作量是审计一份「哪些 `call.rt` 会回调用户代码」的白名单**（sort 比较器、生成器 resume、`__exit__`、C 回调跳板），这些必须保守写回。堆全局先不碰。收益上限仍受 6 个 callee 寄存器约束，且只覆盖顶层 |
| **B. 顶层热循环 outlining** | 把顶层循环包成合成函数，诱导变量变局部槽（等价于自动化 0.204 对 matmul 的手工改造） | 更通用，覆盖未来任意顶层代码。但要处理 try 保护区切割、traceback 的 PC→函数表、GC 帧记录链与控制流边界，风险与工作量高一档 |

**倾向 A 先做**：风险确定低、直接打实测到的主要成本（spectral_norm 的 39 次 `load.global`），且可带 `PYADAMAS_NO_GLOBAL_RA=1` 之类回退开关。B 留到 A 实测证明 callee 池仍是瓶颈之后再评估。

**验收（两条路线通用）**：spectral_norm / fannkuch / fasta 的 `load.global` + `store.global` 数明显下降，四语言基准实测缩小与 Go 的差距（同机 A/B、`--rebuild`、交替多轮取中位数）；全量 205 ran ok + ASan 全量 + O0/O2 差分 188 用例 + 四套异常夹具 + 泄漏扫描 0 处，全部无回退。

**注意 0.212 的教训**：判断「还有多少提升空间」必须看修正后的调试输出（整型与浮点分开统计）。此前的 `7/14 slots promoted` 是错的，实际已接近饱和——不要基于饱和的池子再承诺收益。
### 1. 工程收尾

- **`tests/check_size.sh` 当前会失败**（0.242 实测）：硬限制 1200 行、`ALLOW_FILES` 为空，5 个文件超限 —— `src/driver_build.cpp` 1395、`src/codegen_regalloc.cpp` 1325、`src/typecheck_types.h` 1253、`src/ir_ops.h` 1241、`src/lower_impl.h` 1207。按 [development.md](development.md) 的规矩清单只能删不能加，这五个必须拆。0.152–0.154 那轮拆分后的回弹主要来自寄存器分配（0.203–0.204）、IR 基础设施（0.205–0.208）和 0.234–0.242 的性能优化（`lower_impl.h` +7 是 RAII 宏，`driver_build.cpp` +9 是方案 C 的判定接入）。
- 标准库前端基线 0.171 复测（Debian 3.13.5：本体 501 / 596，去掉 `test/` 449 / 532）；剩余失败已高度分散（undefined name、各类运算符、getattr 非字面量、跨模块类成员等）；原口径（CPython 3.11.1 + site-packages，38582 个文件）仍未重跑；
- 保持 `docs/` 与 README 的验证表同步（运行用例、模块目录等数字按仓库实际统计）；
- **CHANGELOG 轮转逾期**：现含 0.100–0.242 共 140 个版本 / 2582 行。既有约定是把旧区间整块移到 `docs/archive/`（已有 `changelog-0.40.md`、`changelog-0.41-0.99.md`），下一次轮转建议归档 0.100–0.199、保留 0.200–0.242，并同步 `docs/index.md` 与本文里指向被移动条目的链接。
- `tests/bench/` 标准库基准已固化（0.144）。

## 待决策

这些不是「做不做」，是「选哪条路」。**其中三条已定案，不再重复讨论：**

| 问题 | 定案 | 影响 |
|---|---|---|
| 字符串语义 | **已完成（0.185，S1+S2）**：UTF-8 字节存储 + 码点语义，`len("中文") == 2`，索引/切片/迭代按码点；`bytes` 类型不变；字节长度出口 `len_bytes(s)`。S3（find/split/replace 下标换算、格式化宽度、re、文件 I/O）待做。设计见 [str-codepoints.md](str-codepoints.md) |
| 整数溢出 | **维持回绕**（不加检查） | 保留一类静默算错；已在 compatibility.md 声明为有意差异 |
| 值表示 | **维持裸 i64**（不加 tag） | 不做 `unknown` 的运行时分派；`unknown` 继续走保守 RC/GC 路径。要做也只在寄存器分配之后考虑 shadow tag |

仍然待定：

| 问题 | 选项 | 影响 |
|---|---|---|
| `print` 求值顺序 | 边算边写（现状）/ 对齐 CPython | 只有副作用可见时才看得出来 |

## 技术债

按「什么时候会疼」排序：

1. **GC 的根只扫登记区，且 stop-the-world**：分代已落地（minor trial-deletion + major 标记-清扫）；GC 不再扫整段真实栈，只扫帧记录登记的 unknown 槽和 callee-saved 寄存器；0.175 起临时区按 vreg 静态类型发射位图、只扫「可能含指针」的字（纯标量帧整段不扫），保守保留实测从 92 个 / 8144 B 降到 0；0.181/0.183 修掉一整类「构造期对象不在 GC 根上」的 UAF（54 个函数 / 70 个 pin 点）；0.216 修掉「unknown 槽被提升到寄存器 → 帧记录登记的栈槽恒为 0 → GC 漏标记」这一类（unknown 槽禁止提升、`gc_scan_slot_` 改在分配之前算、被扫描的槽禁止融合）；仍没有增量/并发 GC，也没有完整栈映射（位图按字收，不按时刻收）
2. **unknown 引用的计数不精确**：容器 kind 2 元素、unknown 字段与全局已用 RC 索引精确释放（`pa_rc_dec_unknown`）；裸 `list` 的空容器已在第一次写入时定型；0.215 补上 unknown 返回值与「unknown → unknown」存储的引用所有权；0.233 让 `rc_index_off`（exact-RC）时的混合容器元素也能精确释放（16 字节对齐 + `marked == UNTRACKED` + 堆链成员三条硬约束）。**仍剩 `release_slots` 跳过 unknown 槽**：直接补齐会让 12 个用例 over-release，因为 unknown 槽里的值未必被 retain 过（装饰器回填、borrow_slots、帧槽别名都会让它持有借来的值）。根治需要槽级「是否 owned」标记
3. **Optional 的收窄能力不足**：`x: Optional[str]` 在 `if x is None: ...` 之后、跨语句块或跨函数调用时仍按 Optional 处理；修它需要正经的数据流分析
4. **优化遍仍不完整**：有常量折叠、DCE、小函数内联、静态调用直接化、LICM、下标版本化/免检查、循环旋转、跨块 CSE 与后端拷贝合并；没有强度削减、通用向量化和全局寄存器分配
5. **`tuple` 与 `list` 表示相同**：tuple 也是可变的动态数组，语义上不严谨
6. **诊断不带列区间**：只指向起点，长表达式不好定位
7. **没有增量重编 Python 源码**：每次 `build` 都是全量编译，只有 import 模块有缓存
8. **浮点变量只在无调用函数里提升**：`xmm8–15` 用于无调用函数的跨块浮点值；含调用/handler 的函数仍走内存槽，跨调用要 save/restore
9. **异常对象仍缺 traceback**：有异常值、`args`、自定义异常类和 `raise ... from`，但 `args` 只保存消息；0.160 的 traceback 只在未捕获异常时打印，异常值本身仍没有 traceback 对象/链
10. **含 handler 的函数没有寄存器提升**：longjmp 会回滚 callee-saved 寄存器，只能全走内存槽
11. **生成器/协程的剩余边角**：`finally` 体内的挂起与 `return`/`break`/`continue` 跳出都已支持（finalbody 在可挂起函数里只编译一份，正常与异常路径汇聚；跳出路径内联 finalbody；close 重放到挂起点时报 `RuntimeError`，重放跳出会恢复 finally 词法上所属的循环）。未标注 `Generator[Y, S, R]` 时 `send` 与 `yield` 表达式静态是 unknown（`await` 已按 `Coroutine[T]` / `Awaitable[T]` 定类型）
12. **asyncio 仍是最小集合**：`async for` 已支持自定义 `__aiter__`，但 fd 读写、asyncio 子进程、线程安全循环未支持；`Queue` 的 float 元素需要 `asyncio.Queue[float]` 这类标注把元素类型带进构造，未标注队列仍按引用语义；`wait_readable` 不是 CPython 标准 API；Future 回调同步执行。Task 已支持多等待者，`gather` 结果元素所有权按静态类型带入运行时（0.158）
13. **stdlib 覆盖不足**：没有 `argparse`（返回对象要有动态属性，和静态布局冲突）；`logging`/`statistics`/`textwrap`/`base64`/`hashlib`/`hmac`/`csv` 已接上子集，`functools` 已覆盖 `reduce`/`wraps`/`partial`/`lru_cache`（`lru_cache` 无淘汰、键类型受限），`json`/`collections`/`itertools` 是子集；`re` 有后向引用/四向断言/原子组/内联标志，但缺 Unicode 属性类和 `findall` 的分组语义
14. **C FFI 与调试信息仍不全**：`cdecl` 只解析头文件子集（不展开宏函数/条件编译，C `float`、union 按值、定长数组不生成）；回调槽不可重入；已有 DWARF 行号（0.159）、函数级 traceback（0.160）、async/generator 逻辑帧（0.161）和 CFI（0.162，0.163 起运行时也走展开器），仍缺 `.debug_info`、列号、异常对象 traceback 和包管理
15. **`re` 是纯回溯 VM**：递归实现，深模式或灾难性模式会吃栈，只靠 2 亿步上限兜底；没有 DFA/JIT，`findall`/`split` 每次重新编译模式字符串
16. **形参类型推断只做单态**：未标注形参由调用点回填，同一形参吃多种类型时退回 unknown（`print` 会按整数输出）；嵌套函数不参与重查。根治要运行时类型标签，代价是全量 tag 或 NaN-boxing（见「待决策」）
