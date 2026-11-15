# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.173] - 2026-11-16

新增 **x86-64 Windows(win64) 目标**：`--target=win64` 产出可在 Windows 上原生运行的
PE 可执行文件（已在 Windows 宿主机实测运行，非 wine）。运行时 28 个编译单元全部
可交叉编译到 Windows。

### 新增

- `src/target_abi.h` / `target_abi.cpp`：TargetABI 描述一个目标的 ABI 事实（实参
  寄存器、callee-saved 池、浮点池、scratch、栈对齐、影子空间、setjmp 符号名、
  块内临时池等）。两个实例：`kX86_64SysV`（默认）与 `kX86_64Win64`。
- `--target=` 开关（x86_64-sysv / x86_64-win64，也接受 sysv/win64/linux/windows）。
- `emit_arg(pos, is_fp, cls_idx, v)`：同时支持 SysV 的「按类型分计数」与 Win64 的
  「按位置分配」，超出寄存器个数时按 ABI 落栈。
- 运行时平台层（`runtime/rt_internal.h`）：poll → WSAPoll（含非 socket 句柄桥接）、
  clock_gettime → QueryPerformanceCounter、nanosleep → Sleep、mkdir/realpath/
  sysconf/timegm 的 Win32 等价、iconv 恒等桩。
- `runtime/rt_subprocess.c` 的 Win32 分支：CreateProcess + 匿名管道，POSIX 路径零改动。
- `runtime/rt_exc.c` 的 `pa_win_setjmp`/`pa_win_longjmp`：只保存非易失寄存器、
  rsp、返回地址，不依赖展开信息（msvcrt 的 longjmp 走 SEH，而生成的汇编函数没有
  `.pdata`/`.xdata`，展开会失败）。
- 汇编指示符按目标开关：`.cfi_*`/`.type`/`.size`/`.note.GNU-stack` 在 COFF 下关闭，
  `.data.rel.ro` → `.rdata`。
- `verify_win.sh`：一条命令跑完「构建 / POSIX 全量 / SysV 汇编 md5 / 标准库基线 /
  Windows 覆盖」五项。

### 性能

- `try` 进入开销 **122 ns → 6 ns**：glibc 的 `setjmp` 每次都做 `rt_sigprocmask`
  保存信号掩码（实测 117.2 ns vs `_setjmp` 2.5 ns）。pyadamas 从不改信号掩码，
  改用 `_setjmp`/`_longjmp`；handler 记录再改空闲链表复用（13 ns → 6 ns）。

### 修复

- **SSE 不支持内存到内存的 `movsd`**：`move_fp` 在源和目的都在栈槽时直接发
  `movsd mem, mem`，汇编器报错、整个文件编译失败。此前零测试覆盖（跑 160 个
  用例的汇编逐字节对比，唯一差异是另一处）。现经 `%xmm0` 中转，新增
  `tests/run/102_float_slot_move.py` 锁死。
- **Win64 第 5+ 个栈实参的 mem-to-mem `movq`**：同理，改为经 `%r10` 中转。
- **`gc_collect` 晋升阶段读取已释放对象**：Windows 堆在 free 后会 decommit，
  读它直接 `0xC0000005`。POSIX 下靠运气不崩。
- **生成器 `GenSetSlot` 写死 `%rcx`**：Win64 下 `%rcx` 是第 1 个实参、此刻已失效，
  于是往生成器帧里写垃圾值——`for` 循环上界变成随机数，**不崩不报错、只是永不
  结束**。这是整批 Windows 问题里唯一致命形式不是崩溃的一类。
- 手写汇编跳板（`.Ldtor`/`.Lgcrel`/`.Lgengcrel`/`pa_call1`/`pa_call2`）的实参
  寄存器与影子空间按 ABI 取。

### 验证

| 项 | 结果 |
|---|---|
| Windows 一致用例 | **138 / 146**（派工前 69 / 139） |
| 挂起 | 0 |
| POSIX 全量 | 177 通过 / 0 失败 |
| 标准库前端基线 | 502 / 596 |
| SysV 汇编 | 160 用例逐字节不变（`88f2bb28…`）|

**未完成**：结构体按值传参的 Win64 分类规则（3 例）、可变参数（1 例）、
Windows 专有 API（2 例）、COFF 下缺 `.pdata` 导致 traceback（1 例）、
`86_popen` 用例里写死 `/bin/echo` 不可移植（1 例）。

## [0.171] - 2026-11-15

继续收口散簇：标准库前端基线 435 → 501 / 596（84.1%）。

### 变更

- **模块级 undefined name 保守放过**：只在函数体内确定性报错；f09 改成函数体用例保留诊断。
- **None 接收者全放行**：方法/属性、有序比较、成员测试、迭代、下标赋值。
- **类型对象**：`type(self).method()`、3 参 `type(name,bases,dict)`、`__qualname__`/`__mro__` 等、类实例无 `__call__` 当构造器。
- **super()**：类上下文丢失 / 协议 dunder 找不到 / `super(Cls,self)` 当前类。
- **多继承**：附加基类字段布局冲突降为 unsupported。
- **可重绑定**：推断形参、`*args`/`**kwargs`。
- **杂项**：`async with`、`print(file=)`、`open(**kwargs)`、独立 `slice()`、`str.maketrans`/`bytes.partition`、file 方法名当值、切片赋值 Class/None、Exception/Class/Dict 可解包。

### 验证

- 前端基线：435 → **501 / 596（84.1%）**，去掉 `test/` 449 / 532。
- `ctest` 3/3；run 套件 173 ok / 0 fail；`check_size.sh` 通过。

### 说明

剩余 95 个多数是「正确的检查结果」而非误杀（如 tempfile 裸 `os.` 而只 `import os as _os`，CPython 运行也会 NameError；enum/timedelta/Decimal 算术语义；跨模块成员可见性）。要全通需放宽真阳性或实现完整 import 图解析。


## [0.170] - 2026-11-14

一键扫荡剩余散簇：标准库前端基线 411 → 435 / 596（73.0%）。

### 变更

- **2 参 super**：`super(Cls, self)` 当 Cls 是当前类时按零参 super 处理。
- **no-`__init__` 带参实例化**：常量类的标记值实例化不再判死。
- **unpack 放宽**：generator/range/str/Optional 目标可解包（元素类型逐个赋）。
- **type 对象元数据属性**：`__qualname__`/`__mro__`/`__doc__`/`__bases__`/`__module__` 等放行。
- **异常值方法**：`with_traceback`/`add_note`；异常/type 对象属性赋值、方法名 monkey-patch（仅函数值）改为 unsupported。
- **推断形参可重绑定**：inferred 参数不锁定类型，`cannot assign` 簇 18→4。

### 验证

- 前端基线：411 → **435 / 596（73.0%）**，去掉 `test/` 392 / 532。
- `ctest` 3/3；run 套件 173 ok / 0 fail；`check_size.sh` 通过。


## [0.169] - 2026-11-14

getattr 非字面量 + 内置异常多参：标准库前端基线 374 → 411 / 596（68.9%）。

### 变更

- **getattr/setattr/hasattr 属性名非字面量**：`getattr(obj, name)` 对非类/unknown/模块/函数接收者从「报错」改为 check 一律放过（返回 unknown），类实例仍走运行期动态属性表。
- **内置异常构造多参 + 关键字**：`ValueError(a, b, c)` 任意位置参、`Error(code=1)` 关键字不再拒绝。lower/运行时建 args 元组、消息取元组 repr；`raise→except` 间经 `pending_exc_args` 保留 args，`e.args`/str 与 CPython 一致。

### 验证

- 前端基线：374 → **411 / 596（68.9%）**，去掉 `test/` 370 / 532。
- `ctest` 3/3；run 套件 173 ok / 0 fail；`check_size.sh` 通过。
- 新增 `tests/run/159_exc_multi.py`（多参异常与 CPython 逐字一致）。


## [0.168] - 2026-11-14

isinstance / bytes：标准库前端基线 348 → 374 / 596（62.8%）。

### 变更

- **isinstance 第二参数拓宽**：接受的内置类型扩展到 `bytes`/`bytearray`/`memoryview`/`type`/`slice`/`frozenset`/`property`/`classmethod`/`staticmethod`，还有 `(A, B, ...)` 元组/列表、unknown 与可迭代目标。
- **isinstance 运行期**：
  - 元组/列表目标短路 OR：逐个元素检查，任一命中为真，对象只求值一次。
  - known 类型静态判定（`isinstance(b'x', bytes)`→True 等）。
  - unknown 值带运行期标签的内置（str/list/tuple/dict/complex）按标签查；无标签的（int/float/bool 等）保守返回 false。
- **bytes**：`bytes()` 构造接受 2 参（对象+编码，编码忽略）与 list/tuple/生成器/range 的 int 元素；`bytes.decode` 放宽 0..2 参；`bytes * int` 重复、`bytes.translate`；内置类型名当值（`int_.from_bytes`）走类型级方法。

### 验证

- 前端基线：标准库本体 348 → **374 / 596（62.8%）**；去掉 `test/` 为 338 / 532。
- `ctest --preset dev` 3/3（运行 156+）；run 套件 172 ok / 0 fail；`tests/check_size.sh` 通过。
- 新增 `tests/run/158_isinstance_bytes.py`（输出与 CPython 逐字一致）。


## [0.167] - 2026-11-14\n\n自定义异常类当普通类：富异常收字段/方法/岛\u5efa模。\n\n### 变更\n\n- **富异常类当普通类**：自定义异常类有字段/方法/岚`__init__`/`__str__`时，实例是真正的 vtable 对象。`raise Rich(args)`通过类构造建实例并从岚`pa_raise_instance`抛出。`except Rich as e` 通过岚`pa_exc_accept_instance`取回省实例，`e.code`/`e.describe()` 走普通类字段/方法访问。\n- 实例是普通 vtable 对象：字段初始化、vtable 方法分派、`super()` 均正常；`多模块/内置接受` （请参check 阶段放过。\n- **内置异常属性保守**：`OSError.errno`/`UnicodeError.code` 这类内置异常的大多属性静态看不见，check 打 unsupported 放过。\n\n### 验证\n\n- 前端基线：标准库本体 335 → **348 / 596（58.4%）**。\n- `ctest --preset dev` 3/3（运行 156）；差分 840 个程序 0 失败；`tests/check_size.sh` 通过。\n- 新增 `tests/run/157_rich_exc.py`（富异常字段/方法、raise/except 取回实例『pass-only 仍 e.args）。\n\n## [0.166] - 2026-11-14

跨模块 super(): 基类来自别的模块时，check 不再拒绝。

### 修复

- **super() 上的跨模块基类**: 定义类的 MRO 里有导入基类时，它的方法在 check 阶段看不见，以前直接把“找不到方法”当成错误拍死了所有“class X(base.Base): super().__init__()”。现在只在纯本地继承链都找不到时才报错；有导入基类（或 open 类）时返回 unknown，build 沿运行期 MRO 分派。

### 验证

- 前端基线: 标准库本体 320 → **335 / 596（56.3%）**。
- `ctest --preset dev` 3/3（模块 13）; 差分 840 个程序 0 失败; `tests/check_size.sh` 通过。
- 新增 `tests/modules/cross_super/`（跨模块基类的 super()初始化与方法分派）。

## [0.165] - 2026-11-14

动态属性 + 反射放宽：标准库前端基线 294 → 320 / 596。

### 变更

- **实例动态属性**：类实例末尾可挂一个惰性分配的 `dict[str, unknown]`，偏移存进 vtable 元数据第 6 个字（0 = 没有，不用的类零开销）。`setattr`/`getattr`/`hasattr` 对静态没有的字段落到运行期查表；属性名非字面量也走查表。只有真的反射过的类才开槽，普通字段读取仍在编译期判死活（`f28` 拼错字段名保持报错）。
- **非类接收者反射，check 放过**：`hasattr(module, ...)` / `getattr(func, ...)` / unknown 接收者只打 `unsupported` 标记，`check` 不再判死，`build` 用模块当值才拒绝。

### 验证

- 前端基线（Debian CPython 3.13.5）：标准库本体 294 → **320 / 596（53.7%）**，去掉 `test/` 为 **288 / 532（54.1%）**。
- `ctest --preset dev` 3/3（运行 155、反例 53）；差分 840 个程序 0 失败；`tests/check_size.sh` 通过。
- 新增 `tests/run/156_dynamic_attr.py` 与 `tests/11_reflect.py`。

## [0.164] - 2026-11-14

前端基线：放开两处误杀，标准库基线 281 → 294 / 596。

### 修复

- **dunder 返回类型改到函数体推理之后再判**：未标注的 `__str__`/`__repr__`/
  `__len__`/`__hash__`/`__bool__`/`__eq__` 等协议 dunder，定义处还看不到
  return 的类型，以前按 unknown ≠ str 直接判死。现在统一在
  `check_dunder_returns()` 里按推理结果校验；仍是 unknown 就放过——证明
  不了错就不报错。
- **`getattr(obj, name, default)`**：支持第三个参数。静态布局里属性集合
  是固定的，属性缺失时结果就是缺省值；类实例按字段/属性查，其它已知类型
  按成员表判。

### 验证

- 前端基线（Debian CPython 3.13.5）：语法通过 596，类型通过 281 → **294 /
  596（49.3%）**；去掉 `test/` 是 253 → **266 / 532（50.0%）**。
- `ctest --preset dev` 3/3（运行用例 154、反例 53）；差分 840 个程序 0 失败；
  `tests/check_size.sh` 通过。
- 新增 `tests/run/155_dunder_ret.py`（未标注 dunder + getattr 缺省值）与
  `tests/fail/f55_dunder_ret.py`（`__len__` 返回 str 必须报错）。
- 剩余失败已不是误判，而是四项没建模的能力：动态属性（hasattr/getattr
  48+20）、跨模块基类（super() 41）、isinstance（32）、自定义异常类（25+11）。
  明细记在 compatibility.md 的基线一节。

## [0.163] - 2026-11-14

栈回溯换成标准展开器，生成代码去掉帧指针；顺手修掉跨多个挂起点的溢出 bug。

### 修复

- **跨多个挂起点的值没有落槽**：`spill_across_suspends` 算活跃性时把
  `GenYield`/`GenAwait` 当成没有后继，只在续跑块里用到的 vreg 既不存槽、
  也不装回。`await add(await add(1, 2), await add(3, 4))` 这类链式 await 会
  读到上一轮留下的垃圾（RC 报 `refcount increment on a freed object`，或者
  直接段错误）。现在把续跑块算作挂起块的后继；新增
  `tests/run/154_nested_await.py`（await 链 + 生成器连续 `send`）。

### 变更

- **traceback 走 `_Unwind_Backtrace`**：`pa_print_traceback` 不再手走 rbp 链，
  改用 libgcc 的标准展开器逐帧取返回地址，仍只打印调试表里登记过的帧。
  运行时因此可以去掉 `-fno-omit-frame-pointer`（`rt_core.c` 早就不带）。
- **生成代码去掉帧指针**：槽位、帧记录、栈上实参改相对 `rsp` 寻址；出参区
  按整函数最大用量一次留够（调用点不再 `subq`/`addq`，`rsp` 全程不动），
  `%rbp` 并入变量寄存器池（最多 6 个）。槽号小的排低地址，偏移落在 disp8。
- **对齐**：函数入口 `.p2align 4`，循环头（回边目标）同样 16 字节对齐。
  去掉帧指针后 nqueens 一度慢 10%，对齐后回到原水平。
- CFI 跟着改：不再 `push %rbp`，CFA 全程由 rsp 定义，尾声逐条
  `cfi_restore` 并更新 `def_cfa_offset`；`link_program` 显式加
  `-Wl,--eh-frame-hdr`。

### 验证

- `ctest --preset dev` 3/3（运行用例 153）；差分 840 个程序 0 失败；
  `tests/check_size.sh` 通过。
- 未捕获异常回溯、gdb `bt`（含 `set backtrace past-main on`）都还能穿过生成帧；
  帧里已经没有 rbp，能展开只可能是 CFI 起效。
- 同机 A/B（8 轮取最小，旧 = 0.162，各 8 次交错跑）：fib 10.67 → 9.22 ms
  （-13.6%，少 push/pop rbp 与一次 mov）、mandelbrot -2.5%、binary_trees -0.9%；
  nqueens +3.6%、matmul +2.8%、spectral_norm +2.5%，在逐轮 ±4% 的噪声内。
  多出来的 `%rbp` 变量寄存器让 fib 这类调用密集的代码受益最明显。

## [0.162] - 2026-11-14

调试信息收尾：生成代码补上 CFI（栈展开规则）。

### 变更

- **CFI**：每个生成函数发 `.cfi_startproc` / `.cfi_endproc`，prologue 用
  `.cfi_def_cfa_offset` / `.cfi_offset` / `.cfi_def_cfa_register` 描述 CFA 与
  保存下来的 callee-saved（生成器是 `%r15`）；尾声用 `.cfi_remember_state` +
  `.cfi_def_cfa %rsp, 8` + `.cfi_restore_state`，一个函数里的多条返回各描述
  各的。GNU as 据此生成 `.eh_frame`。
- 函数符号补 `.type ..., @function` 与 `.size`；手写汇编（类析构器、GC
  release、生成器帧释放、C 回调跳板、模块 init 桩、`main`）同样带 CFI。
- 新增 `tests/cfi/`（cdecl + `_Unwind_Backtrace` 探针），在
  `tests/run_tests.sh` 里以 `cfi:unwind` 输出：CFI 断掉时深度从 7 掉回 2。

### 验证

- `gdb` 断在 `pushq %rbp` 之后（rbp 链尚未建立）仍能回溯，栈帧里列出
  rbp 的保存位置；`set backtrace past-main on` 可一路走到 `_start`。
- libgcc `_Unwind_Backtrace` 数出的帧依次是 `probe_depth` → `__pa_main` →
  `main` → `__libc_start_call_main` → `__libc_start_main` → `_start`，与 gdb 一致；
  生成器 resume 帧与协程帧（`%r15` 那一套）同样能穿过：探针在生成器体内
  和 `asyncio.run` 的协程体内都数出 10 层。
- `ctest --preset dev` 3/3；运行用例 152 个；差分 840 个程序 0 失败；
  `tests/check_size.sh` 通过。
- 生成代码只多了汇编指示（`.cfi_*` / `.type` / `.size`），指令序列不变；同机 A/B
  （同一二进制用 objcopy 去掉 `.eh_frame`）20 次取最小：mandelbrot
  6.91 / 6.81 ms、spectral_norm 18.19 / 18.35 ms，差值在测量噪声内。

## [0.161] - 2026-11-14

可靠性 + async/generator traceback 补全。

### 修复

- **模块缓存写入原子化**：`assemble_object` 不再直接写 `mod.s`/`mod.o`；
  `mod.pya` 与 `manifest` 也改为临时文件 + `rename`。多个进程同时编译
  同一模块（并行测试、ctest 与 diff 并发）不会再读到截断的汇编/对象文件。
- 新增 `pyadamas_cache_race` ctest：两进程并发 `--rebuild` 编译
  hashlib/hmac 5 轮，专门守这个回归。

### 变更

- **异常携带生成器/协程逻辑帧**：`PyAdamasException` 新增 trace 链表；
  协程 `coro_step`、生成器 `gen_run`、异步生成器驱动在异常逃出时把
  resume 函数帧挂到异常上。未捕获异常先打印普通 rbp 回溯，再追加
  `main` → `outer` → `inner` 这样的逻辑帧（当前只有函数名，没有挂起行号）。
- trace 节点随 `exc_clone` 深拷贝、随 `exc_free` 释放；153 测试扩展到
  async/generator 子进程。

### 验证

- `ctest --preset dev` **3/3**（新增 cache_race）；运行用例 152 个；
  差分 840 个程序 0 失败；16MB 内存验收、`tests/check_size.sh` 通过。
- `pyadamas_cache_race` 连续 5 轮并发 hashlib/hmac 编译通过；
  并行 stress（10 轮）无截断、无残留 `.tmp.*`。

## [0.160] - 2026-11-14
运行时 traceback：未捕获异常打印函数级回溯。

### 变更

- **PC → (函数, 行号) 表**：codegen 在行变化处发 `.LdbgN` 标签并收集表项，
  模块入口调用 `pa_debug_register` 注册；函数结束有哨兵项。
- **rbp 链回溯**：运行时 `rt_debug.c` 沿 rbp 链取返回地址，命中调试表后
  打印 `Traceback (most recent call last)` + `File "...", line N, in func`；
  运行时除 RC 热路径 `rt_core.c` 外统一 `-fno-omit-frame-pointer` 编译。
  core 不抛用户异常，排除它避免 RC 热路径回退（对比 0.158，容器类基准
  恢复到同一水平）。
- **显示名**：函数名去掉生成后缀（`inner_1` → `inner`）。
- **测试**：新增 `tests/run/153_traceback.py`，父进程以子进程重跑并检查
  stderr 含 traceback、函数名、异常消息与退出码。

### 验证

- `ctest --preset dev` 2/2（152 个运行用例）；差分 840 个程序 0 失败；
  16MB 内存验收和 `tests/check_size.sh` 通过。
- release 基准复跑（同机）：mandelbrot 26.5×、spectral_norm 30.3×、fib 15.3×、
  nqueens 8.7× vs CPython；容器类与 0.158 持平。
- 示例：
  ```
  Traceback (most recent call last):
    File "x.py", line 7, in __pa_main
    File "x.py", line 4, in rec
    ...
  pyadamas: uncaught ValueError: tb
  ```

### 限制

- 生成器/协程的堆帧不在 rbp 链上，回溯可能只显示调用它们的普通函数；
  优化内联后内联体归到调用者。
- 没有 CFI；`panic_c` 内部不变量错误仍直接退出；异常值对象仍不携带
  traceback（只有未捕获时打印）。

## [0.159] - 2026-11-14

调试信息：生成代码带 DWARF 行号与可读函数符号。

### 变更

- **IR 指令带源码行**：`Inst` 新增 `line`；降级期按当前语句/表达式行打标，
  优化遍复制/内联指令时保留行号。
- **DWARF 行号表**：codegen 输出 `.file 1 "<源路径>"` 与按行变化的
  `.loc 1 <line>`，GNU as 生成 `.debug_line`；每个函数发出可读符号
  `pyadamas_fn_<N>_<name>`（内部跳转仍是 `.LfN`）。
- **可定位**：`addr2line -e prog -f <addr>` 返回函数名与 `源文件:行`；
  `readelf --debug-dump=decodedline prog`、`nm prog | grep pyadamas_fn`
  可直接查看。

### 说明与验证

- 没有 `.debug_info` / CFI：`gdb` 能定位源码行，但栈回溯不可靠；未捕获异常
  仍只有 `pyadamas: uncaught <type>: <message>`，运行时 traceback 留待后续。
- `ctest --preset dev` 2/2；差分 840 个程序 0 失败；16MB 内存验收和
  `tests/check_size.sh` 通过；生成代码行为与 0.158 一致。

## [0.158] - 2026-11-14
正确性修复、asyncio 收尾与前端基线复测。

### 修复

- **NaN 的 `!=` 在比较+分支融合路径判错**：`if a != b` 生成
  `ucomisd + jne`，NaN 时 ZF=1 落到假分支；改为先把无序（PF=1）跳到真
  分支，再按 ZF 分派。`tests/run/48_branch_mnemonics.py` 补六种比较的
  NaN 回归。
- **生成器/协程 resume 复用上一个函数的浮点提升状态**：`assign_slot_regs`
  对生成器提前返回时没有清 `fp_promoted_slots_` / `slot_fpidx_`，包含
  `list[float]` 打印或 float gather 的协程会生成 `movsd , off(%rbp)` 这种
  空操作数汇编。生成器分支现在显式清空这两张表。

### 变更

- **Task 支持多个并发等待者**：完成时把结果/异常分发给 `waiters` 列表中的
  所有协程；取消等待对象型 Task 时同样通知全部等待者；等待者被取消时从
  列表摘除。`pa_asyncio_create_task` 新增 `result_kind` 参数，Task 结果的
  所有权按静态 `task[T]` 元素类型带入运行时。
- **gather 结果元素所有权按静态类型带入运行时**：`pa_asyncio_gather` 多一个
  `result_kind` 参数（0 裸值 / 1 堆引用 / 2 unknown），避免 float 位模式被
  当成堆指针 dec，也让字符串/对象结果按精确 RC 释放。
- **`from asyncio import ...` 保留特化类型**：`gather` / `run` / `wait_for` /
  `create_task` 等别名仍走 asyncio 的调用点规则，不再退回泛型
  `list[unknown]` / unknown。

### 验证

- `ctest --preset dev` 2/2；151 个运行用例（含新增 NaN、float gather、
  Task 多等待者/多等待者取消覆盖）。
- 差分 840 个程序，0 失败。
- 16MB `RLIMIT_AS` 下 13 个 RC/GC/异步用例全部通过；`tests/check_size.sh` 通过。
- 标准库前端基线（Debian CPython 3.13.5）：598 个文件，语法通过 596，类型通过
  281 / 596（47.1%）；去掉 `test/` 为 533 个文件、语法 532、类型 253 / 532
  （47.6%）。0.133 同口径为 277 / 596（46.5%），本版 +4。
- release 基准（本机 WSL，g++ 14.2，5 次取最小）：mandelbrot 25.8×、
  spectral_norm 29.5×、fib 14.9×、nqueens 8.5× vs CPython，9 个用例输出与
  CPython/C++ 一致；快照见 `bench/compare/RESULTS-0p158-wsl.md`。

## [0.157] - 2026-11-14

优化：浮点比较分支去掉 NaN 专用跳转。

### 变更

- `ucomisd` 对无序（NaN）把 CF/ZF/PF 全置 1。四种有序比较（`< <= > >=`）
  通过交换操作数顺序、改用 `ja`/`jae` 判断，让 NaN 自然落到「不满足」的
  一侧，于是不再需要先发一条 `jp`。内层循环里的浮点条件从
  `ucomisd + jp + jbe` 缩成 `ucomisd + jae`。
- 相等/不等比较仍保留原形式：`==` 在无序时 ZF=1，无法和相等区分。

### 验证

- dev 测试 164 个通过；`ctest` 2/2；差分 840 程序 0 失败；内存与规模检查通过。
- release：mandelbrot 12.1→12.0 ms，其余持平。

## [0.156] - 2026-11-14

优化：拷贝合并与跨块公共子表达式消除。

### 变更

- 拷贝合并：线性扫描分配临时寄存器时带 `v = a op b` 提示——`a` 的最后
  一个使用就是这条指令时，把 `a` 的寄存器直接分给 `v`，指令生成器看到
  目标就是左操作数便原地算（`subsd b, a`），省掉一条 `movsd`/`movq`。
  提示只接受本块临时池里的寄存器，跨块值和变量槽的寄存器不会被覆盖。
- 寄存器之间的浮点搬运用 `movapd` 做整 128 位拷贝。`movsd` 只写低 64 位，
  硬件要把它和目的寄存器的旧值合并，凭空多出一条依赖链；紧挨着复用同
  一个临时寄存器时这一条依赖能吃掉三成性能（实测 mandelbrot）。
- 新增 `opt_cse.cpp`：跨块公共子表达式消除。用前向「可用表达式」数据流
  合并条件块与循环体里重复的纯表达式，键把局部槽的当前值编码成
  `Slot(s)`、常量按值编码、交换律算子规范化；`store.slot` 杀掉依赖该槽
  的表达式，块入口取所有前驱出口的交集。`while i < n and zr*zr + zi*zi
  <= 4.0:` 的两次乘法因此从体块里消失。只删「所有使用都在本块内」的
  重复指令，取过槽地址的函数整函数跳过。比较指令不参与：它常和紧跟的
  分支融合成一条 `cmp+jcc`，融合后不会真的写出布尔值，跨块复用那个
  vreg 会读到没写过的寄存器。
- 上一版用来中转浮点中间结果的 `%xmm1` 融合在发生拷贝合并时让位，避免
  多一条 `movsd`。

### 工程

- 同机 A/B（release，三次取中）：mandelbrot 26.4ms → 12.0ms（2.2×，
  相对 CPython 23.3×，已接近 g++ -O2 的 9.4ms）；spectral_norm
  34.1ms → 30.0ms（1.14×）；nbody 17.2ms、matmul 12.3ms、fib 16.7ms、
  nqueens 77.5ms 持平；标准库 17 项基准无可见回退（statistics 5.4→4.6ms）。
- 164 运行用例、差分 840 程序 0 失败、ctest、memcheck、文件规模检查通过。

## [0.155] - 2026-11-14

优化：浮点数值循环的代码生成。

### 变更

- 块内浮点临时寄存器：确定是 float 的块内纯计算值分配到 `xmm3–xmm7`
  （`xmm0/1/2` 留给指令生成器），mandelbrot 内层循环不再为每个中间
  结果写/读栈槽。
- 浮点比较 + 分支融合：`ucomisd` 直接吃标志位，有序比较用 `jp` 排除
  NaN，省掉 `setcc`/`movzbq`/`cmpq $0`；整数比较路径不变。
- 浮点除法所在块不再被排除在块内临时寄存器之外（浮点除法只用 xmm0，
  不碰借出的 GPR）。
- 新增 `opt_forward.cpp`：把 `and`/`or` 短路布尔值经临时槽往返的形态
  改成块内转发 + 分支线程化 + 不可达块清理 + 死槽写入删除，循环条件
  直接走 cmp+branch。只转发布尔槽——转发浮点/堆槽会延长活跃区间，在
  nbody 这类含调用的热循环里反而变慢。
- 跨块浮点 vreg 可以使用 `xmm8–15` 中剩余的空闲寄存器（原上限 2 个）。
- 跨块 vreg 提升按「使用次数 × 循环深度」排序（支配集 + 回边求自然
  循环嵌套深度），内层循环每轮都读的值优先拿到寄存器，不再被循环外
  只执行一次的常量按 vreg 编号抢走；同时去掉调试用的 `PYADAMAS_NO_FPX`
  开关。

### 工程

- 同机 A/B：mandelbrot 47.2ms → 25.2ms（1.87×），相对 CPython 从 6.1×
  提到 11.6×；nbody 17.6ms 持平；其余语言基准无可见回退。
- 151 运行用例、52 反例、840 差分、ctest、memcheck、标准库 bench 全部通过。
- 槽位转发只做「块内、类型一致、所有使用同块」的值，并对转发链做传递
  解析，避免替换后引用到已删除的 vreg。

## [0.154] - 2026-11-14

工程治理：拆完全部 allowlist 大文件，所有源文件回到 1200 行以内。

### 变更

- `typecheck_call.cpp`（2106 行）按调用类别拆成入口/模块规则/方法表三块，
  新增 `typecheck_call_modules.cpp`、`typecheck_call_method.cpp`。
- `lower_expr.cpp`（2276 行）拆成容器提升、赋值与 `expr` 主分派、运算符与
  比较、容器构造、dunder 协议与转换五个文件。
- `lower_import.cpp`（2130 行）拆成模块别名/import 语句、原生调用、
  operator/collections、functools/datetime/pathlib/json 四个文件。
- `lower_stmt.cpp`（1968 行）拆成控制流/for、with/异常清理、async with。
- `lower_gen.cpp`（1389 行）拆成 yield 与生成器 for、async for 与可挂起
  函数编译、跨挂起溢出三块。
- `lower.cpp`（1267 行）把类相关成员搬到 `lower_class.cpp`，驱动留在原文件。
- `cdecl.cpp`（1247 行）拆成词法/驱动与解析器两部分，新增内部头
  `cdecl_internal.h`；`cdecl_parse.cpp` 保留类型解析与 `Cdecl`。
- `tests/check_size.sh` 的 `ALLOW_FILES` 清空：文件行数检查对所有文件生效。

### 工程

- 纯搬移，接口、报错文本与生成代码不变；151 运行用例、52 反例、840 差分、
  ctest、memcheck、release bench 全部通过。
- 函数级警告仍按约定只提示不阻塞（如 `gen_rt_args` 543 行、
  `builtin_module` 736 行）。

## [0.153] - 2026-11-14

工程治理：拆分剩余的上帝函数与大文件，并加入规模检查。

### 变更

- `gen_call_rt`：505 个运行期调用 case 按实参形态（全整数/全浮点/混合）
  抽成 17 个 `gen_rt_*` 装填函数，移到 `codegen_call_rt.cpp`；
  `codegen_call.cpp` 从 1701 行降到 954 行。
- `builtin_module`（736 行模块签名数据表）整段搬到 `builtin_sigs.cpp`。
- `lower_builtin_call`（1655 行）按家族拆成 `lower_builtin_reflect/seq/
  container/text/misc.cpp` 五个文件，入口只保留 print 特判、关键字拦截
  和按序分派。
- `TypeChecker::call` 按被调表达式形态拆成 `call_name` / `call_attr` /
  `call_value`；`TypeChecker::stmt` 的 15 类语句体搬到
  `typecheck_stmt_forms.cpp`，`typecheck_stmt.cpp` 从 1364 行降到 864 行。
- 头文件：`ir.h` 拆出 `ir_ops.h`（算子与运行期函数编号）和
  `ir_types.h`（数据结构与机器类映射）；`typecheck.h` 的类型模型搬到
  `typecheck_types.h`；`lower_impl.h` 里 25 个较大的内联成员体移到
  `lower_impl_util.cpp` / `lower_impl_slots.cpp`。
- 新增 `tests/check_size.sh`：源文件超过 1200 行直接失败（剩余待拆文件
  登记在允许清单），单个函数超过 300 行只警告；规则写进
  `docs/development.md`。

### 工程

- 纯机械搬移，报错文本、检查顺序和生成代码不变；151 运行用例、52 反例、
  840 差分、全量 bench 全部与拆分前一致。
- 拆分后仍超过 1200 行的文件：`lower_expr.cpp`（2276）、
  `lower_import.cpp`（2130）、`typecheck_call.cpp`（2106）、
  `lower_stmt.cpp`（1968）、`lower_gen.cpp`（1389）、`lower.cpp`（1267）、
  `cdecl.cpp`（1247）。

## [0.152] - 2026-11-14

工程治理：拆分 `lower_method.cpp` 的上帝函数。

### 变更

- `lower_method_call` 从 2131 行降到约 600 行，只保留模块/类接收者分派
  和用户类方法调用；其余按接收者类型抽成 15 个成员函数：
  `lower_method_native` / `generator` / `async_generator` / `task` /
  `future` / `dict` / `set` / `seq` / `str` / `bytes` / `random` /
  `regex` / `match` / `file` / `int`。
- 新增 `lower_method_native.cpp`（异步/原生类型）、
  `lower_method_containers.cpp`（dict/set/list/tuple）、
  `lower_method_str.cpp`（str/bytes）、`lower_method_misc.cpp`
  （random/re/file/int）；`lower_method.cpp` 从 2612 行降到 865 行。
- 纯机械拆分，不改变降级逻辑与生成代码；151 运行用例、52 反例、840
  差分、16MB memcheck、全量 bench 全部与拆分前一致。

### 工程

- 拆分前扫描：`lower_method_call` 2131 行（203 处 `at.attr` 分派）、
  `lower_builtin_call` 1655 行、`gen_call_rt` 761 行（505 个 case）、
  `builtin_module` 736 行（模块签名数据表）、`typecheck_stmt::stmt` 587 行。
  这批先处理最大的 `lower_method`，其余按风险从低到高后续处理。

## [0.151] - 2026-11-13

修复 `str(unknown)` 不做运行期类型分派。

### 变更

- 新增 `pa_str_any`：unknown 值先查 RC 索引，是堆字符串就原样返回（补
  一份引用），否则按整数格式化。`str()` 的 unknown 分支改走它，不再把
  字符串指针当整数格式化成地址数字。
- 新增运行用例 152（unknown 字符串/整数/负数的 `str()` 与 `len`）。

`repr(unknown)` 仍在降级期报「不支持」，不在这次修复范围内。

## [0.150] - 2026-11-13

GC 保守根收窄：不再扫描整段真实栈。

### 变更

- 帧记录 `PyAdamasFrame` 扩展成「影子根表」：新增临时区起点/字数；槽
  地址低两位区分「可能非堆」（bit0，异常展开按注册表 release）和「只给
  GC 扫描」（bit1，借来的 unknown 引用，异常展开跳过）。
- 含 unknown 局部/形参的槽登记到帧记录，GC 只扫这些地址；含 unknown
  vreg 的帧登记整段 vreg 槽区，纯类型化帧一个字节都不扫。
- `pa_frame_unwind` 跳过 scan-only 条目，异常路径不会释放借来的引用。
- exact-RC 模式保持完全不扫栈；含 unknown 的程序也不再扫整段栈。

### 性能

`bench_rc_gen` 的 GC 总耗时在噪声内（约 18.5ms），`bench_heapq` 20.7ms
不变；收益是根集合从「整段栈」缩到「登记过的槽/临时区」，栈上整数被
误认为根的面大幅减小。

## [0.149] - 2026-11-13

分代循环收集：minor 用 trial-deletion，major 保留标记-清扫。

### 变更

- 堆对象按「是否在当前收集集合」分代：上次收集后分配、还没晋升的对象
  进新生代 `young_buf`；minor 只对新生代做 trial-deletion，老对象的引用
  按真实 rc 计入外部引用、天然是根，不需要写屏障。
- minor 的根 = 试探减计数后 rc > 0 的对象 + 保守扫栈（unknown 局部槽这
  类借来的引用不计入 rc）；exact-RC 模式（整程序无 unknown）下所有引用
  都归槽/临时值/容器所有，直接跳过扫栈。
- major 仍走经典标记-清扫（集合 = 整堆），频率降到「老生代翻倍或 8MB」，
  比原来每 `live` 字节全扫一次少一半。
- 收集期间的 `pa_rc_dec` 按阶段分流：trial/restore 只调集合内计数；清扫
  时集合外的边先攒进 pending，回合结束再按正常 RC 释放，避免清扫中重入
  释放。
- 生成器帧的槽释放改走 `pa_gen_slot_release`：只有清扫阶段才清零槽，
  trial/restore 不会破坏生成器状态。
- 修复 `rc_index_has` 把墓碑哨兵 `(PyAdamasRc*)-1` 误判为命中的老 bug：
  unknown 值 `63` 减去 64 字节头正好等于哨兵，会解引用非法地址。
- 新增运行用例 151（新生代环、晋升后的老生代环、生成器帧与 unknown 局部
  槽里的环）与 `bench_rc_gen` 基准。

### 性能

`bench_rc_gen`（10MB 存活堆 + 大量短命容器）GC 总耗时 25.5ms → 17.5ms
（-31%），墙钟中位数 77.8ms → 69.1ms（-11%）；其余基准在噪声范围内
（`bench_heapq` 20.8ms 不变）。

## [0.148] - 2026-11-13

引用计数：裸 `list` / 空字面量的元素种类延迟到第一次写入时定型。

### 变更

- 列表新增元素类别 3「未定」：`list` / `[]` / `list()` 这类看不出元素
  种类的**空**容器先记 3，第一次 `append` / `insert` / 下标赋值 /
  `extend` 时由新增的 `pa_list_prep` 按写入值定型（堆→1，非堆→0，
  冲突降级为 2）。
- 写入路径统一走 `store_list_elem`：容器未定型、静态种类冲突或值是
  unknown 时调用 `pa_list_prep`，其余情况仍按静态种类直接 retain，
  省掉一次调用。
- `extend` / 切片赋值在目标未定型时采用源的种类，冲突时降级为 2；
  `set_from_list` / `Counter` 把源的 3 按 2 处理。
- 修复裸 `list` 上 `pop()` 的 use-after-free：删除元素前按列表种类补
  一份引用，返回值不再指向已释放对象。
- `PYADAMAS_RC_STATS` 增加 `inc_unknown` 字段。
- 新增运行用例 150（裸 list 定型、冲突降级、切片赋值、推导式、pop）。

### 性能

`bench_heapq` 的 `dec_unknown` 从 143 万降到约 10 万，墙钟 30.5ms →
20.9ms（CPython 40.8ms，1.95x）；其余标准库/RC 基准在噪声范围内无变化。

## [0.147] - 2026-11-13

引用计数：容器字面量与推导式的所有权转移。

### 变更

- `list` / `tuple` 字面量：新元素不再「先 retain、后 cleanup dec」，而是在
  容器构造成功后用 `pa_rc_temp_take` 把临时栈上的引用直接交给容器；中途
  元素求值抛异常时引用仍在临时栈上，由 cleanup 正常释放。
- `dict` / `set` 字面量、list / dict / set 推导式、下标赋值对 fresh 键、
  值、元素同样跳过 retain。
- 新增 `bench_rc_literals` 基准；运行用例 149 扩展到 dict / set / tuple
  字面量与推导式。

### 性能

`bench_rc_literals` 的 `rc_inc` 从 10.0 万降到 1.0 万，墙钟约 5.7ms（不转移
引用时约 5.9ms）；主要成本仍在字符串构造与格式化，所有权转移本身只省掉
一对 inc/dec。

## [0.146] - 2026-11-13

引用计数所有权转移与 RC/GC 统计。

### 变更

- 新增 `pa_rc_temp_take`：新产生的堆值写入局部槽 / 全局 / 字段 / 列表元素 /
  字典键值、`append` / `insert` 或作为返回值时，直接把临时栈上的引用转给
  目标，省掉「retain + cleanup dec」这一对；被别名后不再移动。
- `PYADAMAS_RC_STATS=1`：退出时打印 `rc_inc` / `rc_dec` / `rc_dec_unknown` /
  `temp` / `cleanup` / `free` 与 GC 次数、回收数、存活字节。
- 新增 5 个 RC/GC 基准：短命字符串、短命列表、参数/返回传递、对象环、
  大存活堆 + 短命临时值。
- 新增运行用例 149（所有权转移下的别名与旧值释放）。

### 性能

新值移动后 RC 调用数明显下降：`bench_rc_calls` 的 `rc_inc` 从 6.0 万降到 4，
`bench_rc_strings` 从 4.0 万降到 2，`bench_base64_hashlib` 从 16.0 万降到
8.0 万，`bench_string_textwrap` 从 89.8 万降到 53.4 万；对应墙钟提升约
3–12%（小用例受启动和噪声影响更大）。

## [0.145] - 2026-11-13

序列重复与 `base64` / `hashlib` 的 C 内建，修掉标准库基准里唯一慢于 CPython 的一项。

### 变更

- `str` / `list` / `tuple` 与 `int` 的 `*` 改为序列重复（两侧顺序均可），
  走运行时 `pa_str_repeat` / `pa_list_repeat`：一次分配 + `memcpy`，负次数得到
  空序列；`operator.mul` 对序列同样按此特化。此前这类表达式直接报
  「not supported by the code generator yet」。
- `hashlib.sha256` 的压缩函数改为 C 实现（`runtime/rt_hash.c`）：`SHA256`
  只累积 `bytes`，`digest()` / `hexdigest()` 调 `__pa_sha256` /
  `__pa_sha256_hex`，不再逐字节跑 PyAdamas 循环；md5 / sha1 保持纯 PyAdamas。
- `base64` 的 b64 / urlsafe 编解码改为 C 内建 `__pa_b64encode` /
  `__pa_b64decode`；b16 / b32 仍为纯 PyAdamas。
- 新增运行用例 148（序列重复）。

### 性能

`tests/bench` 的 `bench_base64_hashlib`（dev 构建，repeat 3）从 0.27x 提升到
3.05x（14.3ms vs CPython 43.7ms）；拆分测量中 sha256 从 125.8ms 降到 9.6ms，
b64 从 38.9ms 降到 5.3ms。release 下该项为 2.99x（14.7ms vs 43.9ms）。

## [0.144] - 2026-11-13

collections / datetime / pathlib 收尾，并固化标准库基准。

### 变更

- `collections.deque` 补齐 `remove`/`count`/`index`/`insert`/`reverse`/
  `copy` 与 `.maxlen`；`deque(...)` 保留元素类型（`pop`/下标/迭代不再退化成
  `int`），`append`/`insert` 按元素所有权补 retain。
- `datetime.timedelta` 增加 `//`（int 与 timedelta）和 `/`（int 与 timedelta）；
  `date`/`datetime` 增加 `replace()`；日期校验按实际月份天数（含闰年）。
- `pathlib.Path` 增加 `read_bytes`/`write_bytes`、`is_absolute`、`joinpath`、
  `relative_to`、`resolve`、`absolute`、`touch`。
- 新增 `tests/bench/`：同一份用例分别用 pyadamas 与 CPython 运行并取最优
  耗时（`python3 tests/bench/run_bench.py`），覆盖 itertools、collections、
  heapq、bisect、statistics、math、datetime、string/textwrap、json、
  base64/hashlib。
- 新增运行用例 145（deque）、146（timedelta 除法与 replace）、
  147（pathlib 扩展）。

## [0.143] - 2026-11-13

`operator` 改为内置模块并补齐包装函数。

### 变更

- `operator` 从纯 PyAdamas 子集改为内置模块：`add`/`sub`/`mul`/`truediv`/
  `floordiv`/`mod`/`pow`/`and_`/`or_`/`xor`/`lshift`/`rshift`/`concat`、
  比较 `eq`/`ne`/`lt`/`le`/`gt`/`ge`、`is_`/`is_not`、
  `neg`/`pos`/`abs`/`invert`、`truth`/`not_`、`contains`、`getitem`。
  结果类型与运算在调用点特化：数值提升、字符串拼接、容器相等、
  `datetime`/`timedelta` 比较都复用与运算符相同的 lowering。
- `itemgetter` 改为编译期合成闭包：支持单键与多键（多键返回元组），
  键目前限 `int`；`attrgetter` 需要运行期按名字取属性，调用点给出
  明确的不支持错误。
- 运行用例 144 扩展为完整的 operator 覆盖。

## [0.142] - 2026-11-13

标准库基础静态包（第二批）：itertools 组合函数、datetime.timedelta、operator 子集。

### 变更

- `itertools` 增加 `product`、`permutations`、`combinations`、
  `combinations_with_replacement`、`zip_longest`、`accumulate`。结果仍是
  立即求值的列表；`zip_longest` 的每一列按 `Optional[T]` 补齐 `None`。
- 新增 `datetime.timedelta`：新类型 `TimeDelta`。构造支持
  `days`/`seconds`/`microseconds`/`milliseconds`/`minutes`/`hours`/`weeks`
  （位置或关键字）；提供 `.days`/`.seconds`/`.microseconds`、
  `total_seconds()`、加减、乘整数、取负、比较，打印文本与 CPython 的
  `str(timedelta)` 对齐。
- `datetime`/`date` 与 `timedelta` 的加减、`datetime - datetime` 得到
  `timedelta`；`datetime`/`date` 之间的比较改为按值（原来按指针）。
- 新增纯 PyAdamas `operator` 子集：`itemgetter`（列表/元组下标）、
  `truth`、`not_`。`attrgetter` 需要运行期按名字取属性、算术与比较包装
  需要按调用点特化，留待后续。
- 新增运行用例 142（itertools 组合函数）、143（timedelta）、
  144（operator）。

## [0.141] - 2026-11-13

标准库基础静态包（第一批）。

### 变更

- 新增纯 PyAdamas `heapq`（push/pop/heapify/heapreplace/nlargest/nsmallest）、
  `bisect`（左右查找与 insort，当前限有序 `list[int]`）、`binascii`
  （hexlify/unhexlify/b2a_hex/a2b_hex）。
- `base64` 增加 `standard_b64encode/decode`、`b16encode/decode`、
  `b32encode/decode`、`encodebytes/decodebytes`。
- `statistics` 增加 `geometric_mean`；`textwrap` 增加 `shorten` 与
  `initial_indent`/`subsequent_indent` 参数。
- `pathlib.Path` 增加 `unlink()`、`rmdir()`、`with_suffix()` 和 `.parts`。
- 新增运行用例 140（heapq/bisect/binascii/base64/statistics/textwrap）与
  141（pathlib 扩展）。
- `itertools` 组合函数、`datetime.timedelta`、`operator` 留到下一批：
  它们分别需要新的 lowering、类型/运行期与 getter 建模。

## [0.140] - 2026-11-13

跨模块继承补完：运行期类表、导入类附加基类、导入字段默认值。

### 变更

- `PyAdamasClass` 增加 vtable 指针；`pa_class_new` 接收 vtable，
  新增 `pa_class_ctor` / `pa_class_method`。非入口模块编译的 classmethod
  把 `cls` 当运行期类对象：`cls(...)` 用实际子类的构造器，`cls.m(...)`
  通过类对象 vtable 动态分派；`cls.__name__` 返回实际类名。
- 新增 IR `class.addr` / `global.addr` / `call.ptr` 与 `pa_class_ctor` /
  `pa_class_method` 运行时入口；entry 模块内本地 classmethod 仍走静态
  cid 直调，避免回归。
- vtable 外部槽改为引用导出的裸代码符号
  `pyadamas$<模块>$ms$<类>$<槽>`，修复「引用 `vt + 8*slot` 数据槽地址」
  导致的跨模块继承调用崩溃。
- 模块接口 v10：字段带字面量默认值，方法带 `aliases` 槽表；导入类作为
  多继承附加基类时按 `(name, kind)` 把实现写到全部别名槽，不同名抢同一
  槽仍报错。新构建的模块也走一次接口编解码，保证导出签名里的类类型指向
  扁平 `ClassInfo`。
- `isinstance` 对本地类目标也改按限定名匹配，修复跨模块对象 MRO 里同号
  类编号的误匹配；本地类对象存限定名，`__name__` / `repr` 只显示最后一段。
- 边界：动态 classmethod 的 `cls.x` 类属性仍按声明类解析；子类 `__init__`
  签名需与基类兼容；动态 classmethod 的方法值暂不支持；导入字段的非字面量
  默认值在子类构造时报错。
- 扩展模块用例 `tests/modules/inherit_import`：字段默认值、`Sub2.make`
  构造子类、`cls.__name__`、导入类附加基类的别名槽与 `isinstance`。

## [0.139] - 2026-11-13

跨模块继承：导入类可以作为单继承的主基类。

### 变更

- 模块接口升到 v9：导出类的运行期限定名、完整祖先链、方法 `kind`
  （普通/静态/类方法/属性访问器）；导入方据此重建属性描述符并再次导出。
- 非入口模块为每个类导出 `pyadamas$<模块>$vt$<类>` 与
  `pyadamas$<模块>$os$<类>` 符号；导入方用外部 vtable 项填充子类未覆盖
  的槽，并为导入祖先发 cid=-1 的 mirror 元数据（限定名参与 `isinstance`，
  vtable/own_slots 参与 `super()`）。
- 方法槽跨模块对齐：类定义时预置导入基类的槽；同名同槽复用，同名不同槽
  或不同名同槽在类定义处报错，保证 vtable 同时满足两边的调用点。
- `super()` 的 after 类改为按运行期限定名定位，基类模块里的方法在子类
  实例上执行 `super()` 也能找到正确位置。
- 非入口模块不再做「类没有子类」的终结假设（关闭直调优化），避免导入方
  覆盖被基类模块绕过。
- 边界：只支持单个导入基类，导入类不能作为多继承的附加基类；导入基类的
  字段默认值、`@classmethod` 重绑定、装饰器方法不跨模块重建。
- 新增模块用例 `tests/modules/inherit_import`（多级继承、覆盖、`super()`、
  `isinstance`、导入类标注）。

## [0.138] - 2026-11-13

运行期类对象：类名可以当值用，类装饰器落地。

### 变更

- 新增 `PyAdamasClass` 运行时对象（类 id、构造指针、限定名）与
  `pa_class_new` / `pa_class_name` / `pa_class_repr` / `pa_isinstance_class`。
- 新类型 `ClassObj`：类名在值位置是 `type[类]`，可赋值、传参、打印、
  `__name__`、`isinstance`；本地类在类定义点生成单例，导入类按需生成
  （跨模块签名里仍降级为 unknown）。
- 静态类型已知的类值可直接构造；类装饰器在类定义点执行并接收类对象，
  返回值继续作为该类名的类对象。
- 边界：类装饰器必须返回类；返回替换类时 `C(...)` 仍按原类构造；
  异常类不支持类装饰器；unknown 类值不能当构造器。
- 新增运行用例 138 / 139，扩展模块 classes。

## [0.137] - 2026-11-13

方法装饰器的返回类型未知时不再被后端拒绝。

### 变更

- 装饰器返回类型在第一遍检查时可能还没定下来：类型检查收尾阶段为
  装饰方法补齐类属性条目，并按最终类型刷新 `decorated_ty` / `call_sig`
  （修复 `return f` 这类恒等装饰器在方法上的误拒）。
- 装饰器链结果类型未知时，调用改为按类属性里的函数值做 unknown 间接
  调用：实例调用与 `C.m(obj, ...)` 都支持纯位置实参；没有签名可重排，
  关键字实参仍报错。`super()` 上的未知装饰方法暂不支持。
- 新增运行用例 137。

## [0.136] - 2026-11-13

四个剩余边角：异构元组 repr、set 元素提升、导出类 varargs 构造、Queue 的 float 元素。

### 变更

- `repr()` / `str()` 支持异构元组：逐元素按各自静态类型渲染后用字符串拼接，
  单元素补尾逗号，嵌套元组递归；类元素走 `__repr__`，复数/range 也按各自
  文本形式。纯标量同构元组仍走 `pa_repr_tuple` 快路径。
- `set[int]` / `set[bool]` → `set[float]` 支持赋值与实参提升：按 set 的 dict
  载体逐元素转换后重建（float key 用数值哈希）。同时修正
  `pa_dict_key_at` 返回值的 ABI：float 元素也从 `rax` 取位模式，不再读
  `xmm0`。
- 模块导出类的 `__init__` 支持 `*args` / `**kwargs`：构造包装按方法 ABI
  追加 list/dict 形参并原样转发，模块接口沿用已有 varargs 编码，无需升
  格式版本。
- `asyncio.Queue` 支持 float 元素：`asyncio.Queue[float]` 标注把元素类型
  带进构造，运行时队列记录元素类别（裸值/引用），Future 增加
  `result_kind`，`put` / `get` / `put_nowait` / `get_nowait` 在裸值路径上
  跳过 RC；未标注队列保持原有引用语义。
- 新增运行用例 135 / 136，扩展 134 与模块 classes。

## [0.135] - 2026-11-13

语言边角收尾：嵌套类、类体 lambda、dunder 补齐、异步生成器表达式、导入类字段子模式、
容器元素隐式数值提升。

### 变更

- 函数内定义的类：类表收集会进入函数体，构造、方法调用、类内继承都可用；
  `Symbol` 新增 `is_class_name`，嵌套类名不再被当成「类实例变量」。
- 类体 `f = lambda self, ...: ...` 在脱糖阶段改写成方法定义，复用方法注册与编译。
- dunder 补齐：`__format__`（`format()` 与 f-string 普通格式说明）、
  `__bool__`（`bool()`、`if`/`while`、`and`/`or`/`not`/`assert` 的真值）、
  `__reversed__`（`reversed()` 物化成 list）。`and`/`or` 的真值按各操作数
  自己的静态类型判断，修掉了类实例被 join 成 int 后漏查 `__bool__` 的误判。
- `iter()` 支持类实例的 `__iter__` 和内置可迭代类型；`hasattr()` 对内建/容器
  类型静态判定、`Optional` 堆类型补裸指针非空检查；`setattr()` 支持带 setter
  的 property（走 setter 虚调用）。
- `C.m(obj, ...)` 直接调用类接收者的普通方法（显式传 self）。
- 异步生成器表达式 `(x async for x in agen())` 脱糖成异步生成器函数，可出现在
  任意表达式位置；`build_sig` 用语法预扫描区分「协程」和「异步生成器」，
  前置引用的异步生成器函数不再被当成 `Coroutine`。
- 导入类的类模式支持字段子模式：模块接口格式 8 导出按布局拍平的字段和
  `__match_args__`，`case mod.Point(x=1)` 与本地类语义一致。
- 容器元素隐式数值提升：`list[int]` / `dict[str,int]` 赋给或传给
  `list[float]` / `dict[str,float]` 时逐元素转成 double；`reduce()` 在
  回调形参为 float/unknown 时同样支持（回调按 int 检查过的组合仍明确报错）。
- 新增 `tests/run/132_dunder_edges.py`、`133_asyncgen_expr.py`、
  `134_container_promote.py`；`tests/modules/matchclasses` 增加导入类字段子模式。

## [0.134] - 2026-11-13

`finally` 体内跳出、`throw()`/`athrow()` 接受异常类、自定义 `__aiter__`。

### 变更

- `finally` 体内的 `return` / `break` / `continue`：跳出路径内联 finalbody，
  最终返回值/跳转以 finally 为准，异常被吞掉。非可挂起函数的 landing 路径
  先用 `ExcTakeValue` 把待重抛异常转成槽里的异常值，跳出时不会留下悬空的
  `pending_exc`。
- 生成器 `close()` 重放 finally 体时恢复 finally 词法上所属的循环，
  `break` / `continue` 能正常找到目标块（`Cleanup::Finally` 记录
  break/continue 目标与清理深度）。
- `throw()` / `athrow()` 接受异常类：`throw(ValueError)` 等价于
  `throw(ValueError())`；内置异常名与自定义异常类都支持，类型检查接受
  `Class` 形态的异常类实参。
- 自定义 `__aiter__`：`async for` 支持类实例。`__aiter__` 返回
  `AsyncGenerator` 时复用 `AsyncGenANext` 驱动；返回自身（或另一个带
  `async __anext__` 的对象）时每轮 `VCall __anext__` 并
  `suspend_on_awaitable`。装饰的 `__aiter__`/`__anext__` 走 `dunder_call`。
- 新增 `tests/run/129_finally_jump.py`（普通函数/生成器/协程的 finally
  跳出、异常吞掉、嵌套 finally）、`tests/run/130_throw_class.py`
  （内置/自定义异常类、未启动/已结束生成器、异步生成器）、
  `tests/run/131_async_aiter.py`（`__aiter__` 返回异步生成器、
  `__aiter__` 返回 self + `async __anext__`、break/continue/else），
  `build/memcheck.sh` 覆盖这三个用例。

## [0.133] - 2026-11-13

`Generator[Y, S, R]` 标注给 `send` 与 `yield` 表达式静态类型。

### 变更

- 注解解析：`Generator` 接受 1–3 个参数，`sub[0]` = yield 类型、`sub[1]` =
  `send` 值类型、`sub[2]` = `return` 值类型（只做记录，`StopIteration.value`
  仍是 unknown）；`AsyncGenerator` 仍只接受一个参数。
- 类型检查：函数返回 `Generator[Y, S, R]` 时，`x = yield v` 里 `x` 的类型是
  `S`，`.send(v)` 按 `S` 检查实参并返回 `Y`（`throw` 仍返回 unknown）。
  显式标注优先于生成器返回类型的推导结果，避免元素类型退化成 unknown 后
  把标注的 S/R 一起覆盖掉。
- 未标注时行为不变：`yield` 表达式与 `send` 值仍是 unknown，不做检查。
- 跨模块接口：`Generator` 额外编码 send / return 类型的 presence 字节与类型，
  模块格式版本升到 7。
- 新增 `tests/run/128_send_type.py`（`Generator[int, int, None]`、
  `Generator[str, str, None]`、只写 `Generator[int]` 三种）与反例
  `tests/fail/f20_send_type.py`（`send("oops")` 报
  `send() expects int, got str`），`build/memcheck.sh` 覆盖该用例。

## [0.132] - 2026-11-13

方法值（bound method）：`obj.m` / `C.m` 不加括号得到绑定接收者的函数值。

### 变更

- 类型检查：用户类的方法属性不再返回 `BoundMethod`，而是按
  `mk_method_value` 合成函数签名——实例上的普通方法去掉 `self`，静态/类
  方法保持原签名，装饰方法按包装函数签名去掉 `self`；类接收者
  （`C.m`）上的静态/类方法保持绑定语义。
- 降级：`lower_method_value` 为每次取方法值合成一个零参/一参闭包，
  捕获接收者（单元素 cell），调用时按方法种类分派：实例普通方法
  `vcall`（子类覆盖生效）、静态/类方法 `vcall_static`、类接收者
  `call_direct` 到该类的 vtable 最终实现、装饰方法调用类属性里的包装
  函数并补 `self`。默认参数与关键字参数沿用合成签名。
- 实例属性读取顺序调整：先查方法再查类属性，避免装饰方法的包装函数
  被当成普通类属性读出来。
- 新增 `tests/run/127_bound_method.py`（变量/容器/实参传递、默认与关键字
  参数、静态/类方法、子类覆盖、装饰方法、optional 接收者），
  `build/memcheck.sh` 覆盖该用例。

## [0.131] - 2026-11-13

vtable 槽 dunder 支持装饰器：构造、`with` / `async with`、`__call__`。

### 变更

- `__init__`、`__enter__`、`__exit__`、`__aenter__`、`__aexit__`、
  `__call__` 与算术/比较那批 dunder 一样，可以套自定义装饰器。装饰后的
  实现不填 vtable 槽，调用点统一用 `dunder_call` 从类属性取包装函数并
  显式补上实例，子类覆盖仍然生效。
- 构造路径：`lower_construct` 与跨模块/前向引用的构造包装函数
  （`compile_ctor_wrapper`）都按 `decorated` 分派，装饰器可以改
  `__init__` 的副作用和返回值。
- `with` / `async with`：`FuncCtx::Cleanup` 新增 `exit_cls`，退出路径
  （正常、`return`/`break`/`continue`、异常展开）都从类属性分派；绑定值
  的类型按包装函数的返回值计算。
- 实例调用 `obj(...)`：类型检查新增 `name_is_class`，用
  `class_obj_names_` 区分「类名引用（构造）」和「装着实例的变量/调用
  结果（`__call__`）」；没有 `__call__` 时报「not callable」。降级侧
  `lower_call` 用同样的判别走 `dunder_call`。
- 新增 `tests/run/126_decorated_slots.py`（装饰 `__init__`、`with`
  正常/异常/抑制、普通与装饰 `__call__`、链式 `Adder(10)(1, 1)`、
  装饰 `async with`），`build/memcheck.sh` 覆盖该用例。

## [0.130] - 2026-11-13

跨模块生成器 / 协程 / 异步生成器保留元素类型。

### 变更

- `.pya` 接口编码新增 tag 14/15/16：`Generator[T]`、`Coroutine[T]`、
  `AsyncGenerator[T]` 不再降级为 unknown；导入方可以直接用 `for` /
  `next` / `send` / `await` / `async for`，元素类型参与运算。
- 其余 asyncio 对象（`Task` / `Future` / `AsyncEvent` 等）仍按 unknown。
- 新增 `tests/modules/gen_interface`：跨模块生成器、协程、异步生成器。

## [0.129] - 2026-11-13

异步生成器内部的 `await` 结果不再丢失。

### 变更

- 异步生成器驱动对象新增 `driver_await_result`：内部 awaitable 完成时
  已把结果/异常写进帧槽（`p[2]` / `p[8]`），下一轮 `agen_drive` 不再用
  `driver_sent`（普通 `anext` 为 0）把它覆盖掉。
- 修复 `async for` / 异步推导式里「`__anext__` 返回堆值」时的引用计数
  错误：`handler_leave` 会按旧水位清理临时栈，之前刚送达的值已经入栈，
  被当场释放。现在先离开 handler，再把结果压入临时栈。
- `suspend_on_awaitable` 新增 `push_result_temp` 参数，`async for` 与
  异步推导式在 `handler_leave` 之后自行入栈。
- 新增 `124_asyncgen_await.py`（标量/字符串/列表结果、内部 await 抛异常
  被 `try/except` 接住、`asend`、`async for`、`aclose`），`build/memcheck.sh`
  覆盖该用例。

## [0.128] - 2026-11-13

生成器/协程的 `finally` 体内支持挂起。

### 变更

- `finally` 体内可以写 `yield` / `yield from` / `await`，恢复状态只有一套：
  可挂起函数里 finalbody 只编译一份，正常路径与异常 landing 汇聚到同一个
  `finally.fin` 块。异常路径不再把待重抛异常留在全局 `pending_exc`，而是
  用 `pa_exc_take_value` 复制成异常值存进帧槽（`Exception` 堆槽，随
  release/close 自动释放），finalbody 结束后 `pa_exc_reraise_value_if`
  非空就重抛——体内再 `raise` / 嵌套 `try/except` 不会覆盖它。
- `close()` 按快照重放 finalbody 时若再次遇到挂起点，按 CPython 报
  `RuntimeError: generator ignored GeneratorExit`（协程/异步生成器用对应
  措辞），不再真的挂起。
- `break` / `continue` / `return` 穿越含挂起 finally 的内联副本时，发射
  finalbody 期间把编译期 cleanup 栈截断到该 finally 之外，避免恢复时去
  suspend 已经释放的 handler 记录。
- 运行时新增 `pa_exc_take_value` / `pa_exc_reraise_value_if`。
- 新增 `123_finally_yield.py`（含异常重抛、close、`break`、`yield from`、
  `with`、`send` 与协程 `await`），`build/memcheck.sh` 覆盖该用例。
- 限制：`finally` 体内的 `return` / `break` / `continue` 仍不支持。

## [0.127] - 2026-11-13

生成器 `.throw()` 与 `yield from` 的 throw 转发。

### 变更

- `Generator.throw(exc)`：把异常注入当前挂起点，生成器自己的 `try` /
  `except` 能接住；没人接住时异常随生成器结束抛给调用方。已结束的生成器
  直接抛给调用方，未启动的生成器从函数入口抛出（函数体不执行）。只接受
  异常实例，不接受异常类。
- `yield from` 转发 `throw`：外层停在委托挂起点时异常交给子生成器的
  `throw`，按「子生成器产出 / 结束 / 抛出」分流——产出转 yield、结束取它的
  `return` 值、没接住则在外层挂起点继续传播（外层 `try` 能抓）。委托序列
  （list/str/...）不转发。
- 生成器的恢复入口现在和协程共用 throw 分发：先重挂 handler 并在本帧重新
  `setjmp` 再 rethrow，避免跨调用点 longjmp 到上次 resume 的栈帧。委托中的
  挂起点用新指令 `gen.take.exc` 取走异常（清零帧槽）后跳回委托循环。
- 运行时新增 `pa_gen_throw` / `pa_gen_throw_opt`；`yield from` 循环多了
  `exc_slot` / `ev_slot` 两个帧槽。
- 新增 `122_gen_throw.py`，`build/memcheck.sh` 覆盖该用例。

## [0.126] - 2026-11-13

装饰 dunder 方法。

### 变更

- 走 `dunder_call` 的那批 dunder 现在可以加自定义装饰器：`__str__` /
  `__repr__`、`__eq__` / `__ne__` / 比较、算术与反射/增量、`__contains__`、
  `__len__`、`__hash__`、`__iter__`。调用点从类属性取包装函数并显式补
  self；dict/set 键槽在 `__hash__` 或 `__eq__` 被装饰时退回机器字身份。
- 类型检查在类定义点把被装饰方法的类型回填给装饰器的第一个形参，
  单态化重查后刷新 `decorated_ty` / `call_sig` / 类属性类型。包装体里的
  `f(self, ...)` 不再退化成 unknown，字符串结果也不会被当整数拼接。
- 构造（`__init__`）、`with` 协议（`__enter__` / `__exit__`）、`__call__`
  等仍由 vtable 槽或专门路径分派，装饰时继续明确报错。
- 新增 `121_decorated_dunder.py`。

## [0.125] - 2026-11-13

推导式里的 lambda；`dict(iterable)`。

### 变更

- 含 lambda 的 list/set/dict 推导式在脱糖阶段改写成「生成器表达式 +
  `list()` / `set()` / `dict()`」：循环变量成为合成生成器函数的局部
  变量，lambda 的闭包捕获按常规路径工作，语义与 CPython 一致（所有
  lambda 共享循环结束时的值）。`[lambda: i * 10 for i in range(3)]`
  得到 20/20/20，`{i: (lambda: i) for i in range(2)}` 也能用。
  不含 lambda 的推导式仍走原来的就地展开路径，性能不变。
- `dict(iterable)`：元素是 `(key, value)` 二元组时构造字典，新增运行时
  `pa_dict_from_pairs`；`dict((x, x*x) for x in ...)` 与
  `dict([(1, 2), (3, 4)])` 都可用，`dict()` 的结果类型从元素类型推成
  `dict[K, V]`。`d[key]` 的 value 随之有精确类型。
- 新增 `120_comp_lambda.py` 回归用例。

### 仍未支持

- lambda 的默认参数引用循环变量（默认值全局只支持字面量常量，在调用点
  内联填充）。

## [0.124] - 2026-11-13

方法装饰器的剩余边界（类接收者与 super）。

### 变更

- `C.m(obj, ...)`：类接收者调用装饰方法时，按类属性里包装函数的未绑定
  签名检查实参（显式传 self），降级走 `class_attr_load` + 普通 `Call`。
  子类类接收者读的是继承链上的同一份类属性。
- `super().m(...)`：目标是装饰方法时，在声明类上取包装函数并自动补
  `self`；不再报「decorators on super targets are not supported yet」。
- `95_method_decorator.py` 增补类接收者、子类类接收者和 `super()` 用例。

### 仍未支持

- 装饰 dunder 方法（构造/运算符走 vtable 静态分派）。
- 装饰后的方法值绑定 self（需要 bound method 对象）。

## [0.123] - 2026-11-13

生成器/协程挂起点泛化。

### 变更

- `yield` / `await` / `yield from` 不再要求是语句里第一个求值的东西。
  降级完成后对 resume 函数做 vreg 活跃性分析：续跑块的 live-in 就是
  挂起期间还活着的值，挂起前 retain 一份写进生成器堆帧槽，恢复后装回
  并把所有权交给临时栈。`f(g(), (yield x))`、`10 + (yield x)`、比较链、
  f-string、容器字面量等位置都可用了；寄存器分配对「同一 vreg 多次
  定义」按跨块处理，独占一个栈槽。
- `except` 体内的挂起：resume 入口用 `pa_exc_enter` 换入本帧的异常
  上下文（当前异常 + 保存节点栈），挂起时 `pa_exc_leave` 换回调用方
  那份；异常逃出 resume 时驱动层按进入深度 `pa_exc_unwind` 退栈。
  生成器在 except 体里挂起期间，调用方的 except 状态不受影响，恢复后
  裸 `raise` 仍然重抛生成器当前处理的异常。修正了 except 体重挂
  handler 缺少 landing 分支的问题（之前禁止在 except 内挂起，不会
  触发）。
- `yield from` 委托生成器时，表达式的值是子生成器的 `return` 值
  （`pa_gen_send_opt` 在结束步转移 out 槽）；委托序列仍是 None。
  生成器的 return 值不再直接丢弃，没人取走时由帧销毁释放。
- 新增 `118_suspend_expr.py`、`119_gen_except_suspend.py`，扩写
  `28_yield_from.py`；`build/memcheck.sh` 纳入这三个生成器用例。

### 仍未支持

- `finally` 体内的挂起（close 路径要复制 finally 体，和体内挂起冲突）。
- `yield from` 的 `throw` 转发（生成器还没有 `.throw()`）。

## [0.122] - 2026-11-13

链式调用回归与未支持特性盘点。

### 变更

- 跨模块类引用修复后，`hashlib.md5(b"x").hexdigest()`、
  `logging.getLogger(...).info(...)` 这类链式调用已可用；新增
  `117_chain_call.py` 回归用例，更新 logging/hashlib 文档中的旧限制。

## [0.121] - 2026-11-13

release 基准复跑与 C 头文件导入（cdecl）。

### 新增

- `pyadamas cdecl <header.h> -o <out.cpya> [--link ARG]`：把 C 头文件的
  声明子集转换成 `.cpya`。支持 `#define` 数值/字符串常量、`typedef`、
  `struct`/`enum`、函数原型、`extern` 变量、函数指针参数与可变参数；
  `const char*` → `str`，`T*` → `ptr[T]`，`void*` → `ptr`。
  宏函数、条件编译、C `float`、union 按值、定长数组会跳过或在 stderr
  给出 warning；union/复杂 struct 降级为不透明 `struct`。
- `tests/cdecl/` 端到端用例：生成 `sample.cpya`、链接 `sample.c` shim、
  运行 `main.py`，并接入 `tests/run_tests.sh`。
- release preset 复跑九个基准（[RESULTS-release.md](bench/compare/RESULTS-release.md)、
  `results-release.json`）。生成代码默认 `-O2`，所以 release 与 dev 的
  运行时间基本一致，差异主要来自测量噪声。
- `bench/compare/run_compare.py` 的 Markdown 会按实际二进制标注
  `dev（-O0）` / `release（-O2）`。

## [0.120] - 2026-11-13

标准库补充：`hmac`。

### 新增

- 标准库 `hmac`：`new(key, msg=b"", digestmod="sha256")` 返回带
  `update` / `digest` / `hexdigest` 的对象，支持 `"md5"` / `"sha1"` /
  `"sha256"` 字符串名；>64 字节 key 先摘要，增量 update 与 CPython 一致。
- 测试新增 `116_hmac.py`（三种摘要、长 key、增量更新）。

## [0.119] - 2026-11-13

标准库补充：`base64` 与 `hashlib`，以及跨模块接口增强。

### 新增

- `bytes(list[int])` / `bytes(n)` / `bytes()` 内置转换；越界元素抛
  `ValueError`。字符串与字节串仍共用运行时表示，零成本。
- 标准库 `base64`：`b64encode` / `b64decode` / `urlsafe_b64encode` /
  `urlsafe_b64decode`，解码兼容两种字母表。
- 标准库 `hashlib`：`md5` / `sha1` / `sha256` 的 `update` / `digest` /
  `hexdigest`，纯 pyadamas 实现，标准向量与多块输入均与 CPython 一致。
- 跨模块接口现在按名字序列化类引用（tag 13），导入模块的函数返回类实例
  时类型不再退化成 `unknown`（如 `hashlib.md5(...)` 的返回值）。
- 默认参数字面量新增 `bytes`（tag 7），跨模块调用 `md5()` 这类带
  `b""` 默认值的函数不再把默认值当成 `None`。
- 模块接口格式版本升到 6。
- 测试新增 `114_base64.py`、`115_hashlib.py`（MD5 / SHA-1 / SHA-256
  标准向量、增量 update、200 字节多块输入）。

## [0.118] - 2026-11-13

基准扩充到九个用例。

### 新增

- `bench/compare/` 新增 fannkuch（全排列 + 前缀翻转）、binary_trees
  （完美二叉树分配与递归统计）、fasta（伪随机 DNA 序列生成）三个
  Benchmarks Game 经典用例，每个都有 PyAdamas / CPython / Go / C++
  四份实现，输出逐字节一致。
- `binary_trees.py` 用字符串标注 `"Optional[Node]"` 同时兼容 CPython
  与 PyAdamas（py23 没有 `typing` 模块）。
- 重新生成 [RESULTS.md](bench/compare/RESULTS.md)（每例 5 次取最小）：
  PyAdamas 比 CPython 快 6.6×–27.8×，与 gccgo 同数量级；新用例中
  binary_trees 10.1×、fannkuch 11.5×、fasta 12.8×。

## [0.117] - 2026-11-13

C 互操作：回调（C 函数指针）。

### 新增

- `.cpya` 参数类型 `fn(T1, T2, ...) -> R` 声明 C 回调，调用时传 py23
  函数或闭包；签名只允许标量/指针，参数上限 6 整数 + 8 浮点。
- codegen 为每个回调参数生成跳板与闭包槽：调用前登记闭包，跳板把闭包
  放回 `%r10` 后进入 py23 函数，因此捕获环境的闭包可用；`int32`/
  `uint32`/`bool` 参数按声明宽度提升。
- 语言标注支持 `ptr[int]`/`ptr[float]`/`ptr[bool]`，保留元素种类，
  回调里可直接 `a[0]` 解引用。
- 测试新增 `112_callbacks.py`（int/float/int32/bool、闭包捕获）、
  `113_qsort.py`（`qsort` 排序）与反例 `f54_callback_not_func.py`。

## [0.116] - 2026-11-13

C 互操作：指针解引用与可变参数。

### 新增

- `ptr[T]` 保留元素类型，支持 `p[i]` 读写（`int`/`float`/`int32`/`uint32`/
  `bool`），按元素宽度比例寻址；`int32` 符号扩展、`uint32` 零扩展，
  `float` 使用 `movsd`。指针为 `None` 时抛 `AttributeError`。
- 新增 IR 指令 `ptr.load` / `ptr.store`；元素类型随变量传播
  （手写 `Optional[int]` 标注会丢失元素类型）。
- `.cpya` 的 `extern def` 支持末尾 `...` 声明 C 可变参数函数；
  额外实参按 py23 类型编组（int/bool→整数寄存器、float→xmm、
  str→data 指针、Optional[str]/ptr），调用前按 SysV 设置 `%al`。
- 测试新增 `110_ptr_deref.py`、`111_varargs.py`，并在
  `108_structabi.py` 覆盖 4 字节指针的符号/零扩展。

## [0.115] - 2026-11-13

C 互操作：结构体按值传参与返回。

### 新增

- `.cpya` 的 `extern def` 可以直接使用带字段的 `struct`（不带 `ptr[]`），
  按 SysV x86-64 聚合 ABI 编组：≤16 字节按 8 字节 chunk 分为
  INTEGER/SSE，分别走整数寄存器或 `xmm0–7`；寄存器不足时整块走栈；
  >16 字节的参数全部在栈上，返回值通过隐藏指针（`rdi`）传回。
  INTEGER 返回值按 chunk 依次放在 `rax`/`rdx`，SSE 在 `xmm0`/`xmm1`。
- 单符号覆盖 4/8/16 字节 INTEGER、16 字节双 SSE、24 字节 MEMORY 的
  传参与返回；混合 `标量 + 大结构体 + 标量` 时隐藏指针与整数寄存器
  正确移位。
- C 结构体可以在 py23 里按字段构造（`DivT(7, 3)`），作为值类型参与
  赋值/传参/返回；payload 布局与 C 完全一致，复制沿用 `pa_struct_copy`。
- 不透明 `struct NAME` 仍只能以 `ptr[NAME]` 使用，按值出现会报
  `struct 'NAME' is opaque and cannot be passed by value`。
- 测试新增 `107_cstruct_byval.py`（`div`/`ldiv`/`inet_ntoa`）、
  `108_structabi.py` + `structabi.c`（SSE/MEMORY shim）与反例
  `f52_cstruct_opaque_byval.py`。
- `.cpya` 支持 `extern var NAME: type` 读取 C 全局变量，赋值按声明宽度
  写回；支持 int/int32/uint32/float/bool/ptr/ptr[Struct]/str，
  `str` 变量只读。`stdout`、`optind` 等可直接使用。
- 测试新增 `109_extern_var.py` 与反例 `f53_cimport_assign_func.py`。

## [0.114] - 2026-11-13

C 互操作 v4、四语言经典基准与常用标准库补充。

### 新增

- `.cpya` 结构体可以带字段：`struct NAME:` 后接缩进的
  `field: type`，按 C 规则计算布局（`int` 8、`int32`/`uint32` 4、
  `float` 8、`bool` 1、`ptr` 8）。`ptr[NAME]` 的类型是
  `Optional[ptr[NAME]]`，可直接读写字段，读取时做空指针检查。
- 字段读写复用编译期固定偏移；`int32` 符号扩展、`uint32` 零扩展，
  IR 的 `field.get`/`field.set` 新增 4 字节宽度与无符号标志。
- C 结构体指针只是 C 地址，没有 RC 头，不参与引用计数。
- `bench/compare/`：nbody、spectral_norm、matmul、nqueens、mandelbrot、
  fib 六个经典基准，每个都有 PyAdamas / CPython / Go / C++ 四份实现，
  `run_compare.py` 统一构建、计时（多次取最小）、校验输出并生成
  [RESULTS.md](bench/compare/RESULTS.md)。dev preset 下 PyAdamas 比
  CPython 快 6×–25×（spectral_norm 25×、fib 21×、matmul 19×、
  nbody 17×、nqueens 7×、mandelbrot 6×），与 gccgo 同一数量级，
  C++ 仍最快。
- 标准库 `logging`：等级常量、`basicConfig`、`getLogger`、
  `getLevelName`/`isEnabledFor` 与 `Logger` 的常用方法，默认写 stderr。
- 标准库 `statistics`：mean/median/mode/方差/标准差/调和平均等子集。
- 标准库 `textwrap`：`wrap`/`fill`/`dedent`/`indent` 子集。
- `argparse` 仍未提供：返回对象需要动态属性，与静态布局冲突，已在
  roadmap 技术债中记录。

### 验证

- 新增 `104_cstruct`（gmtime 的 `struct tm` 字段读写、负数 int32、
  `tm_zone` 判空）、`105_stdlib_logging_stats`、`106_textwrap`；
  反例 `f51_cstruct_field`（`.cpya` 字段类型不支持时报错）。
- `ctest --preset dev`、完整 `tests/run_tests.sh`、`build/memcheck.sh`
  全绿。

## [0.113] - 2026-11-13

语言级 `struct` 值类型，语义与 C 结构体一致。

### 新增

- 模块顶层 `struct NAME:` 定义，字段限 `int` / `float` / `bool`；`struct`
  是软关键字，`struct = 1`、`import struct` 不受影响。
- 布局自实现，与 C 完全一致：字段按声明顺序，`int`→`int64_t`、
  `float`→`double`、`bool`→`_Bool`，逐字段对齐，整体按最大对齐补齐；
  不依赖构建期 C 探针或外部编译器源码。
- `Point(...)` 按字段顺序构造；字段读写编译期固定偏移，`bool` 字段按
  1 字节读写；`sizeof(T)` / `sizeof(v)` 为编译期常量；`addr(p)` 得到
  `ptr[Point]`，就是 payload 地址，可直接传给 C。
- 值语义：赋值、传参、返回都复制整块 payload；容器仍存引用（与类实例
  一致，连续数组以后用 buffer）。
- `print(struct)` 明确报错，替代打印指针数值。

### 修复

- `re.Match.group(n)`（n > 1）构造列表时漏传元素 kind，暴露为
  `invalid list element kind`。

### 验证

- 新增 `102_struct`：布局/构造/字段/值语义/传参返回/`addr`/`bool` 字段/
  局部 struct/复合赋值；`103_struct_c`：`cimport clib` +
  `c.memset(addr(p), 0, 16)`，证明与 C 布局一致。
- 新增反例 `f46`–`f50`：字段类型、重复字段、构造参数个数、定义位置、
  直接 `print`。
- `ctest --preset dev`、完整 `tests/run_tests.sh` 全绿。

## [0.112] - 2026-11-13

C 互操作 v3 第一步：不透明结构体指针，可接 `FILE*`、`sqlite3*` 等句柄。

### 新增

- `.cpya` 支持 `struct NAME` 声明：只登记类型名，不描述布局；解析时先
  扫描全部 `struct` 行，允许先用后声明。
- `ptr[Struct]` 作为参数/返回类型，C 侧为 `Struct*`，py23 类型系统里仍是
  `Optional[int]`，可判 `is None`。
- `ptr[Struct]` 不接受 `list`/`bytes` 实参，避免把缓冲区误当句柄；普通
  `ptr`/`ptr[T]` 的缓冲区规则不变。
- 文档补充不透明结构体指针与 `fopen`/`fputs`/`fclose` 示例。

### 验证

- 新增 `101_cimport_struct`：`tmpfile` 取句柄、`fputs` 写入、`fclose`
  关闭，并验证 `fopen` 失败返回 `None`。
- 新增反例 `f45_cimport_opaque_buffer`：`list[float]` 传给 `ptr[FILE]`
  被拒绝。
- `ctest --preset dev`、完整 `tests/run_tests.sh` 全绿。

## [0.111] - 2026-11-13

C 互操作 v2：`ptr[T]` 可直接接收 py23 缓冲区，支持把数组交给 BLAS 等 C API。

### 新增

- **缓冲区传参**：`ptr[float]` 接 `list[float]`（`double*`）、`ptr[int]` 接
  `list[int]`（`long*`）、无类型 `ptr` 接 `list[float]`/`list[int]`/`bytes`；
  编译器取 `data` 字段（list/bytes 均偏移 16）直接传给 C。元素宽度不匹配的
  组合（如 `list[int]` → `ptr[float]`、`bytes` → `ptr[float]`）报错。
- **按调用点编组**：IR 的 `CallExtern` 记录每个实参的 `CKind`，同一符号既可
  接地址也可接 list/bytes；`BufF64`/`BufI64`/`BufBytes` 在汇编中先判空再取
  `data`，空 list 的 NULL 原样传 0。
- 文档补充缓冲区规则、生命周期约定与 `cblas_dgemm` 声明示例。

### 验证

- 新增 `98_cimport_buffer`：`modf` 写 `list[float]`、`time` 写 `list[int]`、
  `memcmp` 比较 `list[float]` 与 `bytes`。
- 新增 `99_cblas`：在真实 OpenBLAS 上跑 `cblas_dgemm`（2×2 方阵、2×3·3×2
  非方阵、带转置）与 `cblas_ddot`，验证 14 参数混合编组与数组指针；开发容器
  已安装 `libopenblas-dev`。
- 新增 stdlib `linalg`：`matmul`（支持 `trans_a`/`trans_b`）、`matmul_into`
  复用输出缓冲、`matvec`、`axpy`、`scal`、`norm`、`transpose`、`dot`
  封装 BLAS，用户只传行主序数据与维度；配套 `stdlib/cblas.cpya`。
- 新增 `100_linalg` 用例；`tests/run_tests.sh` 在缺少 OpenBLAS 时跳过
  `99_cblas`/`100_linalg` 而不是判失败。
- 新增反例 `f44_cimport_buffer_mismatch`。
- 本机基准（行主序方阵，BLAS 多线程 vs py23 三重循环）：64² 约 20–27×，
  128² 约 60–73×，256² 约 39–71×，512² 约 65–83×（512² 单次约 6 ms
  对 420 ms）。
- `ctest --preset dev`、完整 `tests/run_tests.sh`、`build/memcheck.sh` 全绿。

## [0.110] - 2026-11-13

C 互操作 v1：新增 `cimport` 语句与 `.cpya` 声明文件，可直接调用系统 C 库。

### 新增

- **`cimport`**：模块顶层语句，支持 `cimport a.b`、`as` 别名和一行多个名字；
  不产生运行时对象，当值使用会报错。
- **`.cpya` 声明文件**：`cmodule` / `link` / `const` / `extern def` 四种声明，
  支持符号改名、多个 `link` 去重、编译期常量内联。文件按 `a.b` → `a/b.cpya`
  相对源文件目录或 `-I` 路径查找，不进入模块缓存，**不影响既有 `.pya` 二进制接口**。
- **调用编组**：`int`(long)、`int32`/`uint32`、`float`(double)、`bool`、
  `str`(char*)、`ptr`/`ptr[T]`(void*/T*)、`None`、`Optional[str]`；返回值
  按类型做符号/零扩展、bool 归一、char* 复制成 `str`（NULL 对 `Optional[str]`
  返回 `None`）。
- 链接参数自动拼到最终 `cc` 命令。

### 验证

- 新增 `97_cimport`：调用 `libm`/libc 的 `sqrt`/`pow`/`strlen`/`labs`/`abs`/
  `isalpha`/`malloc`/`free`/`memset`/`getenv`/`syscall`，覆盖常量内联、别名、
  多绑定、关键字参数、`ptr[T]`、`Optional[str]` 传 NULL，以及 8 个整型参数
  走栈的编组。
- 新增模块用例 `cimport_shared`：两个模块各自 `cimport` 同一 `.cpya`，验证
  链接参数去重与模块缓存二次运行。
- 新增反例 `f41_cimport_value`、`f42_cimport_nested`、
  `f43_cimport_optional_str`（拒绝把可能为 NULL 的 `Optional[str]` 传给 `str`）。
- `str` 入参汇编先做空指针检查；类型检查拒绝 `Optional[str]` → `str` 的隐式收窄。
- `ctest --preset dev` 2/2，完整 `tests/run_tests.sh` 全绿。

## [0.109] - 2026-11-12

unknown 引用计数精确化：容器 kind 2 元素、unknown 字段与全局改走 RC 索引释放。

### 修复

- **容器 kind 2**：`list`/`dict`/`set` 的 unknown/混合元素不再「只 retain
  不 release」。写入统一 retain（堆→`rc_inc`，unknown→`rc_inc_unknown`），
  覆盖、截断、`del`、销毁统一 `pa_rc_dec_unknown`（先查 RC 索引，整数是
  空操作）。运行时辅助构造路径（extend/reversed/slice/enumerate/zip/
  keys/values/items/update/copy/popitem/set_from_list/deque/random/
  itertools）同步补齐 retain；目标容器原本是 kind 1 而源是 kind 2 时
  运行期把目标提升为 2。
- **unknown 字段**：`IRClass.field_heap` 增加 2（unknown 字段）；类析构器
  与 GC release 改调 `pa_rc_dec_unknown`，`store_field` 覆盖旧值同样按
  注册表释放，unknown 源写入字段时补一份引用。
- **unknown 全局**：`IRGlobal.maybe_unknown` 记录「写过的值可能非堆」；
  `store_global` 的旧值释放与出口 `release_globals` 改走注册表，
  unknown 源写入补 `rc_inc_unknown`。
- **槽/帧记录**：unknown 流过静态堆槽时打 `slot_unknown` 标记，出口、
  异常展开的帧记录和内联入口释放都改走 `pa_rc_dec_unknown`；生成器帧的
  `slot_heap` 增加 2，GC 扫描 kind 2 列表时保守查表标记。
- **队列所有权**：`pa_queue_get` / `get_nowait` 摘元素改用 `list_take_at`，
  把引用直接转交给 Future/调用方，避免新增的 `del` 释放提前回收。

### 验证

- 专项：`dict[str, unknown]` 2000 轮峰值 158MB → 2.1MB；unknown 字段
  310MB → 2.0MB；unknown 值流进堆槽的段错误用例输出正常。
- 新增 `96_unknown_rc`：unknown dict/list 的切片、反转、enumerate/zip，
  unknown 字段覆盖，unknown 全局覆盖，输出与 CPython 一致。
- `ctest --preset dev` 2/2；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败；typed 基准 `03_len`/`08_range`/`11_min`/`13_sum`
  每轮耗时与快照持平。

## [0.108] - 2026-11-12

项目更名为 **PyAdamas**。

### 重构

- **命名**：可执行文件/CLI 从 `py23` 改为 `pyadamas`；C 符号前缀
  `py23_` → `pa_`；宏与环境变量 `PY23_*` → `PYADAMAS_*`；模块接口
  扩展名 `.py23i` → `.pya`；CMake 目标、测试名、Windows 包装脚本
  （`tools/win/pyadamas.*`）与文档统一改名。
- 仓库目录保持 `cpy23`；语言测试程序里的字符串数据（如
  `"hello, py23"`）保持不变，避免改变期望输出。

### 验证

- 全量重新配置/构建通过；`ctest --preset dev` 2/2（`pyadamas_suite` /
  `pyadamas_opt_levels`）；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败；`94`/`95` 在 `-O0/-O1/-O2` 下输出一致。

## [0.107] - 2026-11-12

语言面收尾：自定义方法装饰器；yield/await 挂起点位置放宽。

### 新增

- **自定义方法装饰器**：`@deco def m(self, ...)` 在类创建点按从下到上
  的顺序应用装饰器，结果存进该类的类属性槽（与普通类属性共用存储/
  继承机制）；实例调用 `obj.m(...)` 加载这个函数值并把 `obj` 作为
  第一个实参传入，继承时沿 MRO 读到同一份包装函数。方法体仍按原签名
  检查，调用点按装饰器返回的函数类型查参。限制：装饰器必须返回已知
  签名的函数；不支持装饰 dunder 方法（vtable 静态分派）、`super()`
  调用和类接收者调用（`C.m(...)`），方法值本身不绑定 self。

### 修复

- **挂起点位置放宽**：`yield`/`await` 不再限于语句/赋值右值，只要它是
  当前语句里第一个求值的表达式即可，例如 `return (yield x)`、
  `if (await f()) > 0:`、`v = (await f()) + x`。生成器 resume 会重建
  C 栈帧，vreg 不跨挂起，所以挂起点之前的其它求值仍会被拒绝并给出
  明确诊断；`finally`/`except` 体内的挂起仍不支持。

### 验证

- 新增 `94_suspend_positions`（return 值/条件/二元表达式）与
  `95_method_decorator`（单层、链式、继承、访问 self 字段），输出与
  CPython 逐行一致。
- `ctest --preset dev` 2/2；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败。

## [0.106] - 2026-11-12

生成器/协程异常逃出、close 清理抛异常时立即释放帧槽引用。

### 修复

- **gcrel 改为真实释放**：从「GC 清扫时只减子对象计数」改为逐槽
  `rc_dec` + 清零，并用「status==1 跳过 + 槽清零」保证幂等。GC 清扫
  期间 `pa_rc_dec` 自带「只调可达对象计数」的保护，语义与原来的
  `pa_gc_unref_child` 一致。
- **异常逃出释放帧槽**：`gen_run` / `coro_step` / `agen_drive` 的异常
  路径、`pa_gen_close` 的 finally 抛异常路径、`pa_gen_finalize`
  的清理失败路径都调用 `pa_gen_release_slots`。此前只把 `status`
  标成 2，只要生成器/协程对象还被引用着，帧里的容器/字符串就永远
  不回收：2000 个被保留的生成器（各带 20000 元素 list）峰值
  157MB → 2.3MB。
- `pa_gen_close` 补上 handler 与临时值水位：close 期间产生的临时值
  在边界统一清理，异常信息原样向外传。

### 验证

- 新增 `93_gen_exc_rc`：生成器 `next` 抛异常、`close` 的 finally 抛
  异常、协程 `await` 抛异常、异步生成器 `__anext__` 抛异常，各保留
  150–300 个对象，输出与 CPython 一致。
- 专项：四类保留对象场景峰值约 2–3MB；生成器帧参与引用环、
  20000 轮 GC 清扫峰值 63MB（有界，无崩溃）。
- `ctest --preset dev` 2/2；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败。

## [0.105] - 2026-11-12

异常 longjmp 路径的引用计数清理：被跳过函数帧的局部堆值当场回收。

### 修复

- **普通函数帧记录**：非生成器函数在序言压一条
  `Py23Frame {prev; n; 槽地址×n}`（只登记持有引用的堆槽），正常返回前
  弹栈；`exc_longjmp` 在跳转前调 `pa_frame_unwind`，沿链把被异常跳过
  的帧里槽值先清空再 `rc_dec`。此前这些槽要等 GC，而 str 等叶子对象
  untracked、GC 不会回收：2000 轮「str + 20000 元素 list 后 raise」的
  非内联用例峰值 315MB → 2.9MB。
- **内联槽不再漏引用**：内联入口在复用堆槽前先释放旧值（形参槽在
  被实参覆盖前、`need_zero` 槽在清零前，覆盖了「异常在调用方自己的
  handler 里被捕获、不经过帧展开」的路径）；`release_slots` 释放后把
  槽清零（内联把槽留在调用方帧里跨多次执行复用，悬空指针会让下一次
  赋值二次释放）。堆槽一律在序言清零，且不再提升到 callee-saved
  寄存器（帧记录按栈槽地址读值）。
- **只读形参借用恢复**：出口清零曾让「函数体是否写过形参」误判，借用
  优化整体失效；判定时忽略这段清零序列，并在借用槽上把出口 `dec` 与
  随后的清零一起删掉，避免内联重映射后出现无落点的 store。
- handler 记录新增 `frame` 字段（压入/重压时的帧栈顶）；生成器/协程的
  槽在堆帧里，由帧销毁负责，不压记录。

### 验证

- `ctest --preset dev` 2/2 通过；`92_exc_inline_rc` 扩展为内联、递归
  非内联、只读形参三段，输出与 CPython 逐行一致（`-O0/-O1/-O2` 均验证）。
- 内联/非内联/str/list/参数变体在 2000 轮异常下峰值均 < 4MB；其中
  「内联体形参槽 + 调用方自己捕获」用例 2.5MB。
- `05_strings` / `10_rc` / `13_gc` / `14_dict` / `15_slice` / `54_stdlib`
  在 16MB `RLIMIT_AS` 下通过。

## [0.104] - 2026-11-12

字典/集合删除从 O(n) 改为摊还 O(1)：墓碑 + 插入顺序双向链表。

### 性能

- **删除不再 memmove / 整表重建索引**：存活项用 `prev`/`next` 串成插入
  顺序双向链表，删除只摘链，entry 原地变墓碑（`hash = 0`）挂进
  `free_head` 空闲链；索引槽改存 -1 墓碑，探测越过继续，插入时优先复用
  墓碑与墓碑槽。删除 20 万项的 dict 与 set、再做 10 万次「删一个插一个」
  的交替增删，整段程序 0.036s（旧实现单次删除 O(n)，同规模不可能跑完）。
- **遍历顺序不变**：按序号遍历走 `order` 快照（链表下标），删除后标记
  失效、下一次按序访问时重建；顺序遍历自身是 O(n)，因此不增加摊还代价。
  `popitem` 取链表尾，仍是最后一个插入项。

### 验证

- `ctest --preset dev` 2/2 通过；`16_dict_del`（含 popitem 顺序）恢复通过。
- 专项对比 CPython：50 万次增删、中间删除后重插、`popitem` LIFO、
  `set.pop`、墓碑复用、`copy`/`update`/`==`、`json.dumps(sort_keys=True)`、
  `defaultdict`、删除后的迭代顺序，输出逐行一致（`set.pop` 与 PyAdamas
  既有的确定性取尾语义除外）。
- `05_strings` / `10_rc` / `13_gc` / `14_dict` / `15_slice` / `54_stdlib`
  在 16MB `RLIMIT_AS` 下通过。

## [0.103] - 2026-11-12

字符串运行时深度优化：字符不再分配、搜索走 `memchr`、长键哈希缓存。

### 性能

- **单字符驻留**：`s[i]`、`for c in s`、`chr()` 返回 256 个静态永生单字节串
  （首次使用时初始化并登记 RC 索引），不再每取一个字符就新建 RC 对象；
  `"True"/"False"/"None"` 同样驻留。200k 次遍历 57 字节串：0.273s → 0.067s；
  下标取值：0.239s → 0.062s（同机 CPython 约 0.76s / 1.47s）。
- **`memchr` 搜索**：`find`/`count`/`replace`/`split(sep)`/`in` 统一改为
  「`memchr` 找首字节 + `memcmp` 校验」，替代逐位置整段比较。
  `find` 0.052s → 0.006s，`replace` 0.0027s → 0.0006s。
- **长键哈希缓存**：>7 字节字符串把 FNV-1a + mix64 结果缓存在 `hash` 字段；
  长串字面量的哈希在编译期算好、直接写进静态对象（`.data.rel.ro` 在
  RELRO 下只读，不能在运行期懒写）。100 万次长键字典查找 0.0045s，
  CPython 0.051s。
- **共享不可变串**：`s[:]`、无变化的 `strip()`、`''.join([x])` 直接返回原串
  （按 CPython 语义 inc 一次），省掉一次分配加拷贝；连续切片改为整段
  `memcpy`。

### 验证

- 字符串边界探针（strip/slice/join/replace/find/长键字典/字符迭代/格式化）
  与 CPython 输出逐行一致。
- `ctest --preset dev` 2/2 通过；`05_strings` / `10_rc` / `13_gc` / `14_dict`
  / `15_slice` / `54_stdlib` 在 16MB `RLIMIT_AS` 下通过。

## [0.102] - 2026-11-12

字符串单遍化与短串 SSO。

### 性能

- **单遍 `string.capwords`**：`stdlib/string.py` 的 `capwords` 改调内部
  intrinsic `__pa_capwords`，降级为 `pa_str_capwords`；按 ASCII 空白
  单遍扫描，词首大写、其余小写，直接写进一个预分配的输出串，不再建中间
  词表。实测 20 万次（44 字节 / 9 词）：0.108s → 约 0.011s，同机
  CPython 3.13.5 约 0.16–0.19s。
- **短串 SSO**：<=7 字节的字符串把字符内联在 `Py23Str::hash` 的 8 字节里
  （留 1 字节 NUL），`data` 指向这段内联区，省掉结构体后的字符区；长串
  布局不变。值仍是 8 字节裸指针、对象仍走 RC/GC，属于 ABI 兼容的内联，
  不是 tagged-value 级 SSO。

### 验证

- `ctest --preset dev` 2/2 通过。
- `10_rc` / `13_gc` / `14_dict` / `15_slice` / `05_strings` / `54_stdlib`
  在 16MB `RLIMIT_AS` 下全部通过。
- `capwords` 语义探针输出与 CPython 一致。

## [0.101] - 2026-11-12

字符串 / 列表分配继续优化。

### 性能

- **列表 freelist**：`Py23List` 结构体单独一档（最多 64 块）；`data` 缓冲
  <=512 字节按 64 字节分 8 档，每档最多 64 块。复用时容量按档对齐。
- **`split(sep)` 单遍填充**：单字节分隔符走 `memchr`，容量从 8 起，常见
  2–6 段不再 realloc。
- **`sm_push_str` 快路径**：容量足够时直接写槽，避开跨 TU 的
  `pa_list_append` 调用。
- 实测 20 万次（44 字节 / 9 词）：`capwords` 0.116s → 0.108s，同机
  CPython 3.13.5 约 0.15–0.19s；`split(sep)` 约 0.022s（CPython 约 0.024s）；
  `split + join` 约 0.048s（CPython 约 0.064s）。

### 验证

- `ctest --preset dev` 2/2 通过。
- `10_rc` / `13_gc` / `14_dict` / `15_slice` / `05_strings` 在 16MB
  `RLIMIT_AS` 下全部通过；50 万次 split 在 16MB 下通过。
- dev / release 预设生成的基准程序运行时间无实质差异：生成代码走
  `PyAdamas -O2`，`rt.o` 固定由 `cc -O2` 编译，与编译器自身的 dev/release
  预设无关。

## [0.100] - 2026-11-12

字符串分配优化与 `split` 泄漏修复。

### 修复

- **`str.split()` / `splitlines()` 分词泄漏**：每个新词被额外 `rc_inc` 一次，
  列表销毁后仍剩一个引用，分词结果永不释放（50 万次 `split` 在 32MB 地址
  空间下 OOM）。改为把 `str_new` 的初始引用直接转交给列表。

### 性能

- **短字符串 freelist**：<=128 字节的字符串按 8 字节分 16 档，每档最多缓存
  64 块；RC 归零时回收进池，下一次同档分配直接复用，不再每次走 malloc/free。
- **`split()` 预分配**：按空白切分前先数词数，用精确容量建表，省掉 append
  过程中的 realloc。
- 实测 20 万次 `string.capwords`（44 字节 / 9 个词）：0.219s → 0.116s
  （约 1.9×），同机 CPython 3.13.5 为 0.156s；`s.split()` 0.124s → 0.041s，
  同机 CPython 为 0.044s。

### 验证

- `ctest --preset dev` 2/2 通过。
- `10_rc` / `13_gc` / `14_dict` / `15_slice` / `05_strings` 在 16MB
  `RLIMIT_AS` 下全部通过；50 万次 split 在 32MB 下通过。

## 更早版本

0.41–0.99 的记录已归档到 [docs/archive/changelog-0.41-0.99.md](docs/archive/changelog-0.41-0.99.md)；
0.40 及更早见 [docs/archive/changelog-0.40.md](docs/archive/changelog-0.40.md)。
