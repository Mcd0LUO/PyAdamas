# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

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

0.40 及更早的记录已归档到 [docs/changelog-archive.md](docs/changelog-archive.md)。
