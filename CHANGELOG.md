# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.371] - 2026-11-21

> 第二轮三代理扫描（基线 0.370）后的第一批修复。**这一轮暴露的问题比上一轮多**，
> 本文只记已修的四条；未修的列在末尾，按严重度排序。
>
> ### 已修
>
> 1. **常量折叠只在「结果超 i64」时替换**（0.367 的设计缺陷）：子表达式可能早已在
>    int 域回绕 —— `x: big = (2**70) % 3` 的两个子式都超 i64，结果却是 1，以前得 0。
>    改成只要折叠成功就**无条件**采用精确值。
> 2. **`//` 与 `%` 用了 C++ 的向零截断**：`(2**70) // -3` 差一。改成 Python 的向下
>    取整、余数与除数同号。
> 3. **`(2**70) // (2**70)` 抛 ZeroDivisionError**：折叠中途失败 → 整式退回 int 域 →
>    0//0。无条件折叠后正常得 1。
> 4. **u64 范围字面量（2^63..2^64−1）在折叠里被当有符号读**：
>    `x: big = -9223372036854775808` 得 +2^63。按 `u64_lit` 转无符号即可。
> 5. **min/max 用 `has_big_value` 判 big，而它会递归进容器**：`list[tuple[big,str]]`
>    被误判成 big，把堆指针当整数比并打印。改成只认「元素本身是 big」；tuple 元素的
>    min/max 现在明确报错（CPython 支持，属已知限制 —— 但报错好过输出随机地址）。
>
> 回归：`tests/run/442_big_const_fold_semantics.py`、`tests/run/443_big_const_u64_lit.py`。
>
> 闸门：difftest **393/0**；全套件 **414 ran ok**、exit 0。
>
> ### 未修（第二轮扫描发现，按严重度）
>
> | 严重度 | 问题 | 位置 |
> |---|---|---|
> | **崩溃** | 未标注名字被赋成另一种类型后槽沿用旧类型：先 `for pair in zip(dict,dict)` |
> | | 再 `for pair in zip(set,set)` 时按旧 tuple 类型建元素 → `rc_dec` 把 int 当指针 |
> | | 解引用。同源纯错值形态：`v = "x"; v = 1.5` 打印 1.5 的位模式。 |
> | | `src/lower_impl_slots.cpp:6`（已知→已知不重定型）+ `lower_expr_assign.cpp:52` |
> | **崩溃** | `list[big] = <list[int]>` 元素不转换（类型检查放行 int→big 加宽，降级只给 |
> | | Tuple 做逐元素重建）→ 段错误 | `typecheck_join.h:251` + `lower_expr_dunder.cpp:276` |
> | **崩溃** | `async with` 中 task 被 cancel → 引用计数二次释放（ASan UAF） | `rt_gen.c:100` / `rt_async.c:780` |
> | **崩溃** | 推导式里迭代中改 dict → 不可捕获 panic（`for` 有守卫，推导式没有） | `lower_comp.cpp` |
> | 错值 | `dict[big]` 里写 int 键查不到（kind 9 的 raw 与 big 哈希不同） | `rt_dict.c:70-72,100-104` |
> | 错值 | `"%d" % big` 抛 TypeError | `rt_format.c:649` |
> | 错值 | 超 `__int128` 的常量折叠（`2 ** 127`、`2 ** 200`）仍静默回绕 | `desugar.cpp` |
> | 漏报 | dict「改键不改大小」（del+add）不报错，CPython 3.14 报 keys changed | `lower_stmt_for.cpp:381` |
> | 漏报 | `enumerate(dict)` / `zip(dict,set)` 用**单名**目标时不检测迭代中改容器 | `lower_stmt_for.cpp:713` |
> | 误报 | `zip(d1,d2)` 在最后一次取元素后修改时误报（CPython 先问 d1 就 StopIteration） | `lower_stmt_for.cpp` |

## [0.370] - 2026-11-21

> **big 容器专项收尾**：`in` / `index` / `count` / `remove` / `sorted` / `list.sort` /
> 列表相等比较 / `dict[big]` 与 `set[big]` 键，全部打通。
>
> 三块共用一个事实：**这些比较都发生在运行时容器代码里**，编译器只传一个
> 「怎么比」的种类。
>
> ### 1. 查找类（in / index / count / remove）
>
> `key_kind(big)` 新增 **4**，`list_value_eq` 按 `pa_big_cmp == 0` 判等。
> 不能复用 3「其它/未知」—— 那是按机器字比指针的语义，两个数值相等的 big 会
> 被判成不等。
>
> ### 2. 排序（sorted / list.sort）
>
> 两条链路都要改：`sort_kind(big)` 必须返回 4（走描述符路径），否则落到
> `sort_i64` 按机器字比。实测漏改时 `sorted([2^70, 5, 2^70+1])` 出乱序、
> `[3,1,2].sort()` 原地不动。描述符字符新增 `'b'`，`sort_desc_cmp` 用
> `pa_big_cmp`。
>
> ### 3. dict / set 的 big 键
>
> `hash_key_kind(big)` 返回 **9**（raw-or-big）—— 运行时早就为动态 big 键实现了
> 这一档（按值哈希 + `pa_big_eq`），静态 big 键直接复用。注意不能传 4：
> `pa_dict_new` 只认 0..3 与 9。
>
> ### 4. 元素相等链
>
> 列表相等走字符链（`i/s/f/x/l/d/t`），新增 `'b'` → `pa_big_cmp`。
>
> 放开四处类型检查/降级拒绝，删掉两个已过时的负面用例（`tests/fail/big_dict_key.py`、
> `tests/fail/big_list_compare.py`）。
>
> **仍然不支持（有意保留）**：两个 `dict`/`set` 之间的相等比较、容器的有序比较
>（`<`/`>`）、`list.sort(key=...)`。
>
> 回归 `tests/run/441_big_containers.py`：查找、删除、排序、列表相等、dict 键与
> set 成员共 17 项，与 CPython 逐字节一致。
>
> 闸门：difftest **391/0**；普通与 ASan 套件各 **412 ran ok**、0 ASan 报告；
> check_size 通过。
>
> 至此 [`docs/iteration-roadmap.md`](iteration-roadmap.md) 里挂着的「big 容器专项」
> 全部关闭。

## [0.369] - 2026-11-21

> big 容器专项第二块：**`min()`/`max()` 支持 big**。
>
> 沿用 0.368 的经验 —— **big 的比较是运行时函数**（`pa_big_cmp` 返回 -1/0/1），
> 裸 `Cmp` 的 codegen 没有 big 路径，会把 big 指针当整数比大小。
>
> 修法：`lower_minmax` 里把「候选 vs 当前最优」的比较收口成一个 `take_new()`：
> big 先 `RtFn::BigCmp` 再与 0 比，其它类型原样发 `Cmp`。序列循环与多参数两条
> 路径都改用它。类型检查侧去掉 `min()/max() with big are not supported`，并让
> 结果类型在混入 big 时取 `big`（big 吸收其它整数/浮点域）。
>
> 回归 `tests/run/440_minmax_big.py`：`list[big]` 的 max/min、多参数混类型、
> 负数 big，外加 int/float/str 三条不受影响的对照，与 CPython 逐字节一致。
>
> 闸门：difftest **390/0**；全套件 **411 ran ok**、exit 0。
>
> ### 专项剩余（同一层，需要运行时分派）
>
> `sorted()` / `list.sort()` / `in` / `index` / `count` / `remove` 以及
> `dict[big]`、`set[big]` 键仍然拒绝。它们与上面两块不同：比较发生在**运行时**
> 容器代码里（`pa_list_index` / 排序器 / 键哈希），不是编译器发一条 `Cmp` 就完
> 事。需要给 `key_kind()` 增加 big 档（沿用动态容器键已有的 raw-or-big 思路），
> 并在 `rt_list.c` / 排序器 / 键哈希三处按值比较。

## [0.368] - 2026-11-21

> big 容器专项第一块：**`sum()` 支持 big 元素与 big 起始值**。
>
> `sum(list[big])` 之前被类型检查直接拒绝。放开之后立刻段错误，原因有两层，
> 缺一不可：
>
> 1. 累加初值写死成 `const_int(0)`，往 big 槽里塞 int 0 —— 后续把整数当指针
>    解引用；
> 2. 累加循环发的是裸 `IrOp::Bin` + `BinKind::Add`，而 **big 加法是运行时函数**
>    （`pa_big_add`），codegen 对「`Bin` 且操作数类型是 big」没有路径。
>
> 修法：big 累加器走 `RtFn::BigAdd`，零初值用 `const_big("0")`；生成器与序列
> 两条累加路径都改；int/float 路径一行不动。
>
> 删掉 `tests/fail/big_sum.py`（它钉的是「sum 的 big 被拒绝」，限制已不存在），
> 改由 `tests/run/439_sum_big.py` 正向覆盖：非空、空、带起始值、big 起始值，
> 外加 int/float 不受影响的对照。
>
> **这条经验对整个专项都成立**：big 的算术/比较全是运行时函数，任何「按静态
> 类型发 `Bin`/`Cmp`」的通用路径都不会自己变成 big 感知。`min`/`max`/`sorted`/
> `in`/`index` 以及 dict/set 键都卡在同一层，要按这个模式逐个改。
>
> 注意：`sum([2 ** 70, 1])` 里的 `2 ** 70` 仍在 int 域回绕成 0 —— 0.367 的折叠
> 只覆盖「注解目标」，容器字面量元素没有目标类型可依，与 sum 无关。
>
> 闸门：difftest **389/0**；全套件 **410 ran ok**、exit 0。

## [0.367] - 2026-11-21

> `x: big = <常量整数表达式>` 不再在 int 域里回绕。
>
> 默认 `int` 是 i64、溢出回绕（`docs/compatibility.md` 的既定规则，性能来源）。
> 但目标是 `big` 时把回绕值塞进去毫无意义：
>
>     a: big = 2 ** 70        # 以前静默得 0
>     print(a + 1)            # → 1
>
> 字面量写法（直接写十进制大数）一直是对的，只有**全常量表达式**会被 int 域先
> 算掉。修法：脱糖阶段在 `AnnAssign` 里认「注解是 `big` + 初始值是全常量整数
> 表达式」，用 `__int128` 精确求值，超出 i64 就折成 `CK::Big` 字面量 —— 与
> 「写超 u64 的十进制字面量」走同一条路径。支持 `+ - * ** // % << >> & | ^`
> 与一元正负。
>
> 两个实现细节（都是实测踩出来的）：
>
> - **别自己设「差不多够大」的上限**：第一版把上限写死成 2**126，于是
>   `2 ** 126 - 1` 在算 `2 ** 126` 那步就被判超范围、退回回绕，`d > 0` 变假。
>   改用 `__builtin_{add,sub,mul}_overflow` 判定，左移用「移回去是否相等」验；
> - 超出 `__int128` 的表达式**不改写**，保持原来的回绕行为，不引入新的编译
>   错误 —— 保守处理，避免影响存量代码。
>
> `int` 目标完全不受影响：`x: int = 2 ** 70` 仍然是回绕的 0（刻意如此）。
>
> 回归 `tests/run/438_big_const_expr.py`：`2**70`、`1<<100`、负指数、
> `2**126 - 1`、乘积形式，以及 int 目标的对照，与 CPython 逐字节一致。
>
> 闸门：difftest **388/0**；普通与 ASan 套件各 **409 ran ok**、0 ASan 报告；
> check_size 通过。
>
> 已知边界（未做）：`return <常量表达式>` 在 `-> big` 函数里、以及把常量表达式
> 直接传给 `big` 形参，仍走 int 域回绕。

## [0.366] - 2026-11-21

> `zip` 迭代中改 dict/set 也抛可捕获的 `RuntimeError` —— 0.363/0.365 之后
> 「迭代中改容器」这条线上的最后一格。
>
>     try:
>         for k, v in zip(d, e):
>             del d[k]        # 以前 panic（进程退出、except 接不住）
>     except RuntimeError as err:
>         ...                 # CPython 能接住，现在 PyAdamas 也能
>
> 这里有个容易踩的点：zip 的循环上界是入口算出的 **min(len a, len b)**，删短
> 其中一个之后「min」可能**不变**（另一个本来就短），只盯 min 发现不了。所以
> 两个操作数各存一份**自己的**入口长度，条件块里逐轮比对。
>
> 口径与 0.363 的 `for k in d` / `enumerate(d)` 一致：不等就抛 RuntimeError，
> 文案与 CPython 相同（dict 是 `dictionary changed size during iteration`，
> set 是 `Set changed size during iteration`）。
>
> 回归 `tests/run/437_zip_iter_mutation.py`：删第一个、删第二个、set 增元素、
> 未修改的正常 zip（dict 与 list 各一），与 CPython 逐字节一致。
>
> 闸门：difftest **387/0**；普通与 ASan 套件各 **408 ran ok**、0 ASan 报告；
> check_size 通过。
>
> 至此 0.362 列的「已确认未修」清空。

## [0.365] - 2026-11-21

> `except` 体里 `raise` 跳过同一 try 的 `finally` —— 0.364 之后剩下的那条。
>
> 进入 except 体时已经 `pa_eh_leave` 出了本 try 的保护区，**handler 体里再
> raise 不会经过本 try 的 landing pad**，于是 finally 被整段跳过：
>
>     try:
>         raise ValueError("v")
>     except ValueError:
>         raise KeyError("k")
>     finally:
>         print("A-fin")      # 以前不打印；CPython 会打印
>
> handler 体降级时会给 `Cleanup::FinallyBody` 登记一份内联的 finally 体，
> `return`/`break`/`continue` 早就靠它跑（0.362 的修法）。`raise` 这条退出路径
> 之前完全没接上。
>
> 修法与两个坑：
>
> - **显式 `raise X`**：找到**最内层**那一份 FinallyBody，用 `emit_cleanup_exits`
>   跑到它（含）为止。第一版跑到底（`down_to = 0`），结果嵌套形态里外层 finally
>   被内联一次、又被结构化展开跑一次，`C-outer-fin` 打印两遍 —— 只跑到最内层才对；
> - **裸 `raise`（重抛）**：不能走 `emit_cleanup_exits`，因为清理项里的 `ExcDone`
>   会把 `current_exc` free 掉（运行时 "No active exception to reraise"）。只内联
>   那一份 FinallyBody 体（`stmts(*fbody)`，并把 `cleanups` 临时截断到它之前），
>   其余清理项一律不碰。
>
> 回归 `tests/run/436_raise_in_except_finally.py`：显式 raise、裸重抛、嵌套两层
> finally、`raise ... from`、以及 except 里 return 的对照，与 CPython 逐字节一致。
>
> 闸门：difftest **386/0**；普通与 ASan 套件各 **407 ran ok**、0 ASan 报告；
> check_size 通过。
>
> 仍未修：`zip` 中迭代 dict 时改大小不报错（`for k in d` 与 `enumerate(d)` 已修）。

## [0.364] - 2026-11-21

> `async with` 的 `__aexit__` 异常路径泄漏修掉（0.363 里定位到但没动的那条）。
>
> 症状：协程里 `async with` 报错并逃离时，每次泄漏 **224 字节**（120 + 104，
> 正好是 `pa_exc_type_name` 与 `pa_exc_message` 造的两个串）。
>
>     async def worker() -> None:
>         async with CM():
>             raise ValueError("boom")
>
> 根因有两层，缺一不可：
>
> 1. 异常分支要把 `(type, message)` 穿过 `await` 交给 `__aexit__`，所以先
>    `ExcTypeName`/`ExcMessage` 造两个串、`push_temp` 再塞进临时槽 —— 但**挂起时
>    临时栈被清空**（挂起前后不属于同一次 resume），push 的那两份就没了；
> 2. 重抛分支调 `Raise` 是 noreturn，**longjmp 之后槽里的副本永远走不到释放点**。
>
> 修法：
>
> - 新增 `pa_raise_own(str*, str*)`：与 `pa_raise` 同构，但在把 type/message 拷进
>   异常对象之后、longjmp 之前 **dec 掉这两个串**（真正的所有权转移）；对应
>   `RtFn::RaiseOwn`，重抛分支改用它；
> - `__aexit__` 返回真值（抑制异常）的分支显式释放两个槽并清零。
>
> 为什么不做成「`pa_raise` 统一接管所有权」：其余十几个 `Raise` 调用点依赖的是
> 临时栈在作用域结束时释放，改 `pa_raise` 会双重释放。
>
> 回归 `tests/run/435_async_with_exception.py`：抑制路径、传播路径、嵌套
> `async with`（内层抑制后外层继续），与 CPython 逐字节一致；`--asan` +
> `detect_leaks=1` 下 rc=0、0 报告。
>
> 闸门：difftest **385/0**；普通与 ASan 套件各 **406 ran ok**、exit 0；check_size 通过。
>
> 仍未修：`except` 体内 `raise` 跳过同一 try 的 `finally`（return/break/continue
> 已修，raise 走 setjmp 长跳不经清理项，需要给 handler 体单独保护区）；
> `zip` 中迭代 dict 时改大小不报错。

## [0.363] - 2026-11-21

> 0.362 之后把「已确认未修」里的两条自己修掉。
>
> ### 1. dict/set 迭代中改大小：改为可捕获的 RuntimeError
>
> 旧行为有两种错法：
>
>     for k in d: del d[k]     # 删空之后被下标上界悄悄结束，不报错
>     for k in d: d[99] = 9    # 走到越界读时 panic_c，进程直接退出、except 接不住
>
> CPython 在**下一次取元素之前**就检查容器大小并抛 RuntimeError。修法：在
> dict/set 的循环**条件块**里逐轮比较 `len(seq)` 与进入循环时记下的长度
>（这个槽本来就有，原先只用来比下标上界），不等则抛 RuntimeError，文案与
> CPython 一致（dict 是 `dictionary changed size during iteration`，set 是
> `Set changed size during iteration`）。
>
> 放在条件而不是循环体是关键：放在体里的话，删空之后下标上界先失效、循环
> 直接结束，永远走不到检查。顺带覆盖「删一个再加一个」这种大小不变的改写。
> 覆盖 `for k in d` 与 `for i, k in enumerate(d)`；`zip` 里的 dict 迭代未覆盖。
>
> ### 2. `-> complex` 注解不再静默退化
>
> `parse_annotation` 的标量分支唯独缺 complex，`def f() -> complex` 被解析成
> unknown，调用方按整数打印堆地址（`94718388971600`）。补一行即可。
>
> 顺带暴露的真实边界（不是本次引入）：complex 上还不支持 `.real`/`.imag`，
> 以前类型是 unknown 所以没被检查，现在会明确报错。
>
> ### 仍然未修（下一轮）
>
> - **`async with` 的 `__aexit__` 异常路径泄漏 224B**：已复现并定位到
>   `src/lower_stmt_with.cpp:44-51` —— 异常路径用 `ExcTypeName`/`ExcMessage` 造了
>   两个字符串并 `push_temp`，同步 with 由本帧临时栈回收，但异步路径要把
>   `__aexit__` 包成 awaitable 再挂起，挂起时这两个临时串就丢了。要动 async
>   挂起/恢复的临时栈快照，需单独一轮。
> - **`except` 体内 `raise` 仍跳过同一 try 的 `finally`**：return/break/continue
>   已修，raise 走 setjmp 长跳不经清理项，要给 handler 体单独保护区。
> - `zip` 中迭代 dict 时改大小仍不报错。
> - 0.362 里记的「模块级 except 捕获运行时异常泄漏 152B」**已无法复现**：
>   `try: int("x") except ValueError` 在模块级与函数内 ASan+LSan 都干净，
>   应是被 0.362 的内联槽释放修复顺带解决。
>
> 闸门：difftest **384/0**；普通与 ASan 套件各 **405 ran ok**、exit 0；check_size 通过。

## [0.362] - 2026-11-21

> **三个独立代理全项目扫描 + 各自修复**（前端 / 运行时 / 代码生成）。共修 18 个缺陷，
> 其中 3 个是崩溃（含 1 个越界写、1 个 use-after-free）。
>
> ### 前端（10 条）
>
> - `"a" == 1` **段错误**：typecheck 对 `==`/`!=` 放行任意类型对，而 gen_cmp 按**左**操作数
>   选指令，左边是 str 时把右值当字符串解引用（读 +8）。修法：一边确定 str/bytes、另
>   一边不可能为 str 时直接给常量 False/True；`Optional[str]` 走 NULL 安全的
>   `pa_str_eq_unknown`（顺带修好可选字符串的内容比较）。
> - `and`/`or` 两侧 int 与 float 混合：结果槽是 float 却把 int 位模式直接落进去
>   （`0.0 or 9` → O0 打 `4.4e-323`、O2 汇编失败）。先去 coerce 再落槽。
> - **增量赋值把 int 加宽成 float 时不插转换**：`x = 1; x += 0.5` 得 `9.2e18`。根因是
>   `stmt_aug_assign` 开头缓存了加宽前的目标类型；非 dunder 路径赋值后同步 `expr_types_`。
> - `except` 里 `return`/`break`/`continue` 跳过同一 try 的 `finally`：新增
>   `Cleanup::FinallyBody`，在 handler/else 体降级期间入栈。
> - `match` 的 `case None` 永不匹配（Optional 被先剥成 int）；`case True/False` 按数值
>   相等匹配（`match 1: case True` 命中）。两条都按 CPython 的 `is` 语义修正。
> - `and`/`or` 的 unknown 结果把空串/空容器当真（`"" or 5` 打印空行）：重载时用操作数
>   自己的类型判真，不再拿 join 后的 unknown 走 `cmpq $0`。
> - `int | bool` 合并被 promote 成 float（`(1 if c else True)*2` → `2.0`；`-> int` 里直接
>   编译失败）：Bool 也进整数提升分支。
> - `int | complex`、`list | tuple` 合不上时退 unknown，打印出地址；int 分支未转复数还会
>   段错误。join 增加数值+complex 规则，coerce 只对**目标** complex 发 `pa_complex_new`。
> - gate 关时 `@dataclass` 只是恒等装饰器，但调用点仍放行实参 → `P(1)` 的字段静默为 0。
>   改为 gate 关时报 `'P' takes no arguments (no __init__)`。
>
> ### 运行时 / 降级（5 条 + 1 条自引回归）
>
> - **内联体局部槽在异常/早退路径无人释放**（str 叶子 104B 泄漏）：IR 内联把 callee 槽搬
>   进调用方帧，而调用方的 `release_slots` 是**内联之前**生成的。修法：内联后给调用方每个
>   Return 补释放。**这个修复第一版插在 Return 前，而 entry 的 Return 前有
>   `pa_gc_shutdown`（不管 rc 直接 free 堆链对象）→ -O2 段错误 / ASan heap-use-after-free**；
>   第 5 个提交把它改插到「收尾序列第一个 shutdown 调用之前」，并补 class 实例、
>   list+dict+bytes、生成器内 raise 三类回归。
> - **`for x in list` 现在每轮查长度**（与 CPython 一致）：旧实现入口缓存 len，体内删元素
>   会多报 `IndexError`、append 的新元素永不遍历。这是**有意的语义变更**，连带把
>   `tests/run/44_bounds_elim.py` 里编码旧语义的 `grow()` 改成有界形态，新增
>   `tests/run/238_for_iter_semantics.py` 锁住八种形态。
> - `re.fullmatch` 顶层交替不回溯（`fullmatch(r"a|ab","ab")` 假阴性）：把「整串必须消费完」
>   下沉到 VM 的 `RE_MATCH_TOP`，`match`/`search` 不受影响。
> - `%` 的整数精度被忽略（`"%.3d" % 5` 应为 `005`）：新增只给 `%` 用的 `pct_fmt_int`。
>
> ### 代码生成（3 条）
>
> - **`LOOP_OPT=1` 的仿射 BCE 会越界写**：候选下标是「循环外预赋值的 IV 副本槽」时，
>   `dom_check` 只查块级支配（同块恒真），守卫按 `i` 证明在界内、实际下标是 `j` 的旧值。
>   新增 `slot_store_loop_pos[]`：跨块仍用支配，同块要求「写的位置 < 读的位置」。
>   修前有 `SIGSEGV`/越界写，修后与 CPython 一样抛 `IndexError`。
> - **删除图着色分配器 `RA_GRAPH`**：它在 -O2 下静默算错浮点（`236`/`204`/`384` 均错值），
>   文档已判放弃。删除自足 300 行，默认路径 asm 逐字节不变；`PYADAMAS_RA_GRAPH=1` 现在
>   **报错退出**而不是静默生成错代码。
> - **`RA_SPLIT` 入口拒绝**：split 分配器会死循环/算错值，同样已声明放弃。它与
>   `codegen_block` 的槽位置发射代码耦合太深，删除会波及默认路径，故改为设了环境变量
>   就报错（`=0` 视为关闭，旧实现连 `=0` 都会打开）。
>
> ### 工程
>
> - `src/lower_stmt.cpp` 涨到 1244 行超出硬限制，把 for 家族拆到 `src/lower_stmt_for.cpp`
>   （351 + 903 行）。
>
> ### 已确认但仍未修（本轮只报不改）
>
> - dict/set **迭代中删除**会 `panic_c` 直接退出（不可捕获），CPython 是可捕获的
>   `RuntimeError: dictionary changed size during iteration`。
> - `async with` 的 `__aexit__` 走异常路径泄漏 `pa_exc_type_name`+`pa_exc_message`（224B）。
> - 模块级 `except` 捕获「运行时 C 代码抛出」的异常泄漏消息串（152B，同代码在函数里不漏）。
> - `except` 体内 `raise` 仍跳过同一 try 的 `finally`（return/break/continue 已修；raise 走
>   setjmp 长跳，不经清理项，需单独设计）。
> - `-> complex` 标注不被识别（返回类型退化成 unknown）；`list | tuple` 仍退 unknown、
>   打印地址。
>
> 闸门（合并后主线 build/lead 复验）：difftest **382/0**；普通与 ASan 套件各 **403 ran ok**、
> exit 0；`check_size` 通过（拆分后）。

## [0.361] - 2026-11-21

> **块内标注声明也遵守「一个名字一种类型」**。这是语言规则收紧，不碰代码生成。
>
> Python 的名字是函数作用域：`if flag: c: str = "x"` 与后面函数级的
> `c: big = 7` 指的是同一个槽。但类型检查只按**当前**作用域查 vars，而块
> 作用域是临时的、块结束就没了，于是这种形态完全绕过检查：
>
>     def f(flag: bool) -> int:
>         if flag:
>             c: str = "x"
>         c: big = 7
>         return 1 if c == 0 else 0   # c 按 str 解释，恒假，静默错型
>
> 修法：在函数作用域上累积「标注过的名字 → (类型, 首次声明行)」，任何块内
> 的标注声明都先按它检查；同类型（assignable）照旧放行，冲突报与重复赋值
> 同一条错误并指出首次声明位置：
>
>     error: cannot assign 'big' to 'str' variable 'c' (annotated at line 4)
>
> **为什么不是「禁止换类型」**：本轮先实测了现行规则——有标注变量本来就
> 一个名字一种类型，同族加宽（i8→i32、int→float、int→big、unknown→big）
> 自动插转换，同族收窄（i32→i8、float→int、big→int）要求显式 cast_*；
> 无标注变量按 Python 习惯允许换类型、退化成 unknown。也就是说「只支持同类型
> 的缩减和提升」其实已经实现了，缺的只是块内声明这个洞。收紧成「禁止换类型」
> 会连加宽一起禁掉，并大面积误杀 `x = None` 占位、`x = []` 后定型这类模式。
>
> 不受影响（都有回归）：同名同类型的块内声明、嵌套函数（各自函数作用域）、
> 类体（独立命名空间）、模块级变量被函数内同名标注遮蔽、for 循环变量、
> try/except 绑定。
>
> **asm：全语料 484 份 `asm -O2` 与 0.360 逐字节全同**——纯类型检查改动。
> 收紧紧前后全量套件均无新增失败（387→388 是新增了 419）。
>
> 回归：tests/fail/block_redecl_type.py、tests/fail/block_redecl_branches.py
>（两种冲突形态），tests/run/419_block_annotation_scope.py（五种不受影响的
> 形态，与 CPython 逐字节一致）。
>
> 闸门（合并后主线 build/lead 复验）：difftest **367/0**；普通与 ASan 套件各
> **388 ran ok**、exit 0；ASan+LSan 钉子 9/9 stderr 空；check_size 通过。

## [0.360] - 2026-11-21

> 两条线：**f-string 不再对 __format__ 的结果二次套用格式说明符**、
> **Decimal 补上上下文（localcontext/setcontext/create_decimal）**。
>
> ### 1. f-string 二次格式化（预存 bug）
>
> f-string 降级的类实例路径走完 __format__ 之后把静态类型改成 str，却仍用
> 同一个 format spec 再调一次 FmtStr，等于拿数值型说明符去格式化一个字符串：
>
>     f"{d:.0f}"   → ValueError: Invalid format specifier for str
>     f"{d:<10}"   → 重复补白（width 幂等，平时看不出来）
>
> 修法：__format__ 成功时记 already_formatted，直接把它的结果当片段拼进
> 输出，不再经过 FmtStr；没有 __format__ 的类仍走 __str__ + FmtStr 的老路。
>
> asm（相对 0.359，480 份语料）：只有 2 份变化——132_dunder_edges（类实例带
> 格式说明的 f-string，pa_fmt_str 2→0）与 416_decimal_fstring（这条修复的
> 回归本身）。其余 478 份逐字节全同。
>
> 回归 tests/run/418_fstring_format_once.py：空说明符、精度、符号、分组、
> 宽度/居中、Decimal 的 .1f 与对齐，以及普通 int/str 变量的 f-string 不变。
>
> ### 2. Decimal 上下文
>
> - setcontext(ctx) 替换当前上下文；getcontext() 返回当前对象（改字段即生效）。
> - localcontext(ctx=None)：无参时**复制**当前上下文（必须在 with 里改字段而
>   不漏回外层）；显式传入 Context 时沿用该对象——CPython 会复制，这里记录了
>   差异：with 内改字段会改到调用方的对象。异常路径同样还原。
> - Context.copy()、Context.create_decimal(value)：只有 create_decimal 会按
>   本上下文的 prec/rounding 舍入。
> - Decimal(value, context) 维持「接受但不舍入」：CPython 3.14 实测对
>   str/int/big/Decimal 都不舍入，连 Decimal(float, ctx) 也不按 prec 舍。
>
> 实现约束（记在注释里）：形参静态类型是 unknown，而 unknown 上既不能调用户
> 方法也不能读属性，所以显式 Context 无法复制、只能沿用。
>
> 回归 tests/run/417_decimal_context.py，与 CPython 3.14 逐字节一致。
>
> ### 仍未修
>
> **块内同名变量换类型不诊断**：同一函数里先 `c: str = ...`（在 if/for 体内）
> 再 `c: big = ...` 不报错，后续按旧类型解释（`c == 0` 恒假）。顶层重声明会拦，
> 块内不拦。修它会动类型检查的作用域规则，需要单独评估对 stdlib 既有模式的
> 影响，本轮不做。
>
> 闸门（合并后主线 build/lead 复验）：difftest **366/0**；普通与 ASan 套件各
> **387 ran ok**、exit 0；check_size 通过。

## [0.359] - 2026-11-21

> 两条线：**元组元素按目标类型逐元素转换**（修 0.358 记账的预存 bug）、
> **Decimal 补上五种构造来源与 __format__ 全谱**。
>
> ### 1. 元组元素类型转换（预存 bug，0.357/0.358 都复现）
>
> 元组字面量按**推断**类型构造（(1, 2) 是 tuple[int, int]），边界上只在
> Assign/Return 处做一次 type_eq 判断，tuple[int,int] → tuple[float,float]
> 被当成机器字直通。后果：
>
>     def f() -> tuple[float, float]: return 1, 2
>     print(f())        # (5e-324, 1e-323)，int 位模式被当 double
>
>     def g() -> tuple[int, big, int]: return 0, 7, 0
>     a, b, c = g(); print(b + 1)   # 段错误（0 被当 big 指针）
>
> 修法：convert_to 增加元组重建分支——元数相同、元素类型有差异时，逐元素
> seq_elem_at 取值、coerce 到目标元素类型、再 build_seq_vregs 重新构造。新
> 元组自己持有元素引用，源值不动（仍由临时栈/槽释放），没有多减。coerce 与
> bad_elem 相应放行「同元数定长元组」走重建；变长元组（tuple[T, ...]）与元数
> 不同的情形保持原状——第一版放得太宽，曾让 230_tuple_sort 与 322_calendar
> 在排序描述符上段错误，收窄后才回到 0 差异。
>
> 回归 tests/run/406_tuple_element_convert.py，覆盖返回、实参、嵌套元组、标注
> 字面量、变量赋值五种位置，输出与 CPython 逐字节一致（浮点只比数值，避开
> 文档化的 1.0 vs 1 打印差异）。
>
> **asm：全语料 476 份逐字节全同**——现有语料里没有任何程序触发过这条转换
> （正因为它是坏的，没人写出依赖它的代码）。
>
> ### 2. Decimal 构造与格式化
>
> 构造来源补到五种：Decimal() / str / int / **big**（任意精度，无 i64 截断）/
> float / Decimal，外加两参 Decimal(value, context)（context 接受后忽略，
> 尚不做上下文舍入）。float 走 **repr 的最短十进制**再解析，而不是 CPython
> 的二进制精确展开——这是本模块的口径，已写进模块头；NaN/Infinity 三路
> 输入统一抛 ValueError，不静默。
>
> __format__ 按 CPython 3.14 的 _pydecimal 逐条移植：空说明（= G）、
> e E f F g G %、.N、符号 +/空格、宽度/对齐 </>/^/=/任意 fill/零填充、
> 千分位 , _ 与小数位分组、#、z，舍入跟随 getcontext().rounding。明确拒绝
> d s x X o b c 与 n（locale），非法说明一律 ValueError。
>
> CPython 对拍：8 个 seed × 1466 行 = **11728 行输出逐字节相同、0 差异**，
> 另做极端指数量级与说明符合法性抽查。回归 412–416。
>
> **asm**：除 403–405 三个 import decimal 的用例外全部逐字节相同；那三个的
> 差异是「两参构造必然多一个形参」带来的 frame 728→744 与 movq $0, %rsi，
> src/ 零改动。
>
> ### 本轮发现的编译器缺陷（未修，已绕开）
>
> - **f-string 对类实例二次套用格式**（lower_expr_dunder 调用 __format__ 之后
>   又用同一 spec 走 FmtStr）：f"{d:.0f}" 报 ValueError、f"{d:<10}" 重复补白；
>   纯宽度/填充/对齐因为结果已够宽而幂等，所以看起来正常。带精度的请先用
>   format(d, spec)。
> - **块内同名变量换类型**：先 c: str = ... 后 c: big = ...（顶层重声明会报错，
>   块内不报），后续 c == 0 恒假而 str(c) 仍对。stdlib 已避免多类型同名。
>
> 闸门（合并后主线 build/lead 复验）：difftest **364/0**；普通与 ASan 套件各
> **385 ran ok**、exit 0；ASan+LSan 钉子 9/9 stderr 空；check_size 通过。

## [0.358] - 2026-11-21

> 三条并行线：**修编译器「早返回漏 dec」**（0.356 留着负面的那座山）、
> **Fraction 升级到 big + 新增 decimal**、**一次性短脚本构建固定成本取证**
> （结论：无可取微优化，记账为负面）。
>
> ### 1. 早返回出口按最终槽属性补齐释放（修 unknown 槽引用泄漏）
>
> 降级是单遍源码序：一条 return 在某个槽的首次堆/owned 写入**之前**被
> 降级时，release_slots() 看到的 slot_owned 还是 false，整类跳过 unknown
> 槽；而循环次轮起槽里已是上一轮 store_slot retain 过的堆值，于是每次早
> 返回都漏一份引用。0.356 只在 stdlib 里用 return→break 绕行，本体没修
> （docs/strfix-rc-early-return.md 的负面结论）。
>
> 修法：emit_return 调 release_slots 时记下出口块与该刻的 slot_owned
> 快照；函数体（含模块体）降级结束后 fixup_early_releases 用**最终**槽
> 属性，给这些出口补上「最终 owned、但发射那一刻还没 owned」的 unknown 槽
> 的 load.slot + pa_rc_dec_unknown。borrow 槽与始终借用的 unknown 槽一律
> 不动，因此不会多减。
>
> **asm 影响（相对 0.357，全语料 471 份 asm -O2 逐字节比对）**：462 份全同，
> 9 份变化，全部在 tests/run，bench/realistic 与 bench/compare 无一变化。
> 6 份是纯新增 load+dec（如 204 的 pa_rc_dec_unknown 10→12 次）；3 份
> （206/258/340）因为函数体多了两条指令、栈帧与静态内联预算随之变化，出现
> 偏移/标签重编号与内联边界移动，逐份核对过**没有任何与 dec 无关的语义改动**。
> 这条「允许的 asm 差异」是本轮唯一的例外，其余轮次仍是逐字节不变。
>
> 回归 tests/run/402_early_return_rc.py：同一用例在 0.357 上 ASan+LSan 报
> 104 bytes（exit 1），修复后 exit 0、stderr 空、输出与 CPython 一致；覆盖
> 零次迭代、正常结束、早返回、异常四种出口。
>
> ### 2. Fraction 改用 big；新增 decimal
>
> stdlib/fractions.py 的 numerator/denominator 从 int 改成 big，自带 big 上的
> Euclid 约分（math.gcd 只吃 int），四则与比较全走 big 交叉乘积 —— **修掉
> 「中间乘积超 int64 静默回绕」**。公开 API 与字符串形式不变；跨 i64 用例
> 例如 3037000500/1 + 1/3037000500 的分子 9223372037000250001 > 2^63-1。
>
> 新增 stdlib/decimal.py（纯 pa，big 精确整数内核）：定点表示
> (-1)^sign * coefficient * 10^exponent（尾随零保留）、默认上下文 prec=28 +
> ROUND_HALF_EVEN，实现 + - * /、quantize、四个舍入模式
> （HALF_EVEN/HALF_UP/DOWN/UP）、as_tuple/adjusted/compare 与比较、
> str/repr 的 CPython 规则。**明确不做**：无 localcontext/traps/signals、
> 无 NaN/Infinity、构造只接受 str、不与 int/float 混合运算、无 ** // %、
> 无 __format__；边界写在模块 docstring 里。
>
> 对拍 CPython 3.14.4：回归 403–405 共 87 行 + Fraction 随机 fuzz 360 行 +
> Decimal fuzz 380/550 行，合计 **1377 行逐字节全同**。fuzz 还抓到一个真 bug：
> 29 位整数 /1 时精确商剥零后仍按旧位数定位舍入，结果掉一个数量级（已修，
> 用例进 403）。
>
> ### 3. 一次性短脚本的构建固定成本（负面结论，未提交代码）
>
> 绑核实测：hello-world（313 行汇编）冷构建 52ms、热构建 38ms，而它自身编译
> 只要 2ms —— **固定成本占 70~80%**。拆开：as 8ms、ld --gc-sections 9ms、
> 编译器自身 10~15ms，其余散在两次 sh -c（实测各约 1ms）、缓存清单与临时
> 文件收尾。
>
> 因此本轮**不提交微优化**：把 std::system 换 fork/exec、把汇编喂给 as 的
> 标准输入，单项 1~2ms（约 2~5%），低于 A/A 噪声，还要动链接回退路径，风险
> 不成比例。量级改动只有内嵌汇编器/链接器或整程序产物缓存——后者对首次运行
> 无效，与「一次性脚本」场景不符。结论记在 docs/iteration-roadmap.md。
>
> ### 已知问题（本轮发现，未修）
>
> **元组字面量元素不按标注类型转换**（**预存 bug，0.357 同样复现**）：
> def f() -> tuple[float, float]: return 1, 2 打印 (5e-324, 1e-323)（int 位
> 模式被当 double）；def f() -> tuple[int, big, int]: return 0, 0, 0 取第二个
> 元素即段错误。根因是元组字面量按**推断**类型构造，tuple[int,int] →
> tuple[float,float] 的边界转换没有逐元素重建。stdlib 侧已用「先赋给标注
> 变量再返回」绕开。
>
> 闸门（合并后主线 build/lead 复验）：difftest **358/0**；普通与 ASan 套件各
> **379 ran ok**、exit 0；ASan+LSan 钉子 17/17 stderr 空；check_size 通过。

## [0.357] - 2026-11-21

> **P3 任意精度整数（big BigInt）落地**：显式 `big` 类型、超 u64 十进制字面量、
> 静态算术/比较/位运算/移位/真除、常用内建与格式化、跨模块默认值，以及
> **纯动态域按编译图隔离的安全分派**。默认 `int` 仍是 i64 回绕，既有程序的
> 生成 asm 逐字节不变。分支 big2（c560614 / 418d65a / b714dd8），merge --no-ff。
>
> 运行时 `runtime/rt_big.c`（基 2^32 小端 limb、nd 带符号、不可变、RC 叶子、
> 柔性数组与 RC 头同一 malloc 块）：加减乘、`//` 向下取整、`%` 与除数同号、
> 无限补码位运算、算术右移、平方乘幂、精确 `big/float` 混比（含 NaN/无穷）、
> 正确舍入的 `to_f64`/`truediv`（含亚正规、半程到偶数、带符号零）、十进制/
> 2/8/16 进制输出。运行时 fuzz 98569 normal + 36959 ASan/LSan（`FUZZ_GC=1`）
> 与 CPython 零差异。
>
> 前端：`TyKind::Big` + `CK::Big` 字面量 + `ConstBig` 永生静态对象（limb 由十进制
> 串在编译期算好，与运行时表示逐位一致）；提升格顶层、`int_promote`/`join`/
> `assignable`、跨模块类型编码 24 与默认值 tag 9；`cast_big`（整数/布尔/float/
> 十进制 str/数值 unknown）与 `cast_i64(big)` 显式窄化。
>
> **动态域隔离（本轮的核心设计）**：把 big 擦成 `object` 后，旧有纯 `unknown`
> 路径会把 BigInt 指针当机器整数参与 `& | ^ ~ ** << >>`，动态 dict/set 键也按
> 指针比较，等值异对象查不到。修法是**按编译图启用模式**而不是改旧路径：
> `Parser` 记录语法特征（`big` 注解、`cast_big` 调用、超 u64 字面量，f-string
> 子解析器回传），`load_program` 在全部依赖解析完（含缓存命中）后对全图取或得到
> `Program::needs_dynamic_big`；该标志写进模块缓存清单 `dynamicbig`，模式不同
> 即失效重建（实测非 big 缓存过的模块在 big 程序里会重新编译）。模式开时
> `unknown` 域新增 `AnyBigBinary`/`AnyInvert` 入口，命中 BIG 标签走 `pa_big_*`，
> 否则**逐条复刻旧机器字语义**（`1<<70==64`、`2**-1==0` 不变）；动态容器键用
> `key_kind 9`（raw-or-big，探针认出 BigInt 才按值哈希/等值，其余与旧 kind 3
> 相同）；并强制保留堆注册表（探针依赖它）。静态可判定的 `big` 键仍编译期拒绝。
>
> 已修的实测缺陷：混合 `and/or` 把标量 0/1 当堆指针（SIGSEGV）、`float` 在左侧的
> 顺序比较写反、`unknown` 注解赋值绕过转换、跨模块 u64 上界默认值读成 −1、
> 真除亚正规舍入（`1/(1<<1074)` 曾返回 0）、`x: big = -123` 把 123 当指针（139）、
> `big << object` 静默算地址（改为要求 `cast_big`）、解析/格式化/除法异常的
> longjmp 泄漏与临时余数跨分配未根化。
>
> 有意差异（文档化，非缺陷）：`big + float` 截断 float 侧；`big ** 负指数` 抛
> ValueError（CPython 返 float）；显式窄化是 C 式取低位；整数真除过大抛
> OverflowError。容器值比较/排序、`sum/min/max/divmod`、三参数 `pow`、双参数
> `round` 对 big 明确拒绝。未接：纯动态 `int(v)`（仍 i64 截断）、混合「静态
> float + unknown」不做 big 提升、纯动态真除仍走 float 路径。详见
> docs/big-landing.md。
>
> 闸门（合并后主线 build/lead 复验）：difftest 353/0；普通与 ASan 套件各 374
> `ran ok`、exit 0；ASan+LSan 钉子 13/174/88/92/93/94/119/153/192 与 395–401
> 共 16/16 stderr 空；`check_size` 通过；0.356 主树 464 个 `.py` 的 `asm -O2`
> 与 0.356 编译器**逐字节全同**；新增 395–401、四个拒绝用例、跨模块
> 65_big_default / 66_big_dynamic；独立第三方语料（另一模型自写，非本仓测试）
> 6 个入口 36/36、动态样本 18/18 与 CPython 逐字节一致；23 个日常语料不变。
>
> **0.271 oracle 本轮未跑**（其固定模块缓存指向受保护主树）；上面的旧行为
> 对照用的是 0.356，不能替代 0.271 语料零差异。
## [0.356] - 2026-11-21

> **修复 configparser 插值路径字符串 refcount 泄漏**（0.355 取证留下的独立
> 预存 bug，非异常）。**纯 stdlib 改动**（stdlib/configparser.py 一行 return
> → break + 注释），src/runtime/codegen 零改动，门控关程序生成 asm 逐字节
> 不变（38 热基准天然全同）。
>
> 机理（ASan + gdb 取证，详见 docs/strfix-rc-early-return.md）：最小复现
> ConfigParser 读 `"[a]\nslash = %(third)s/%%plit\nthird = end"` 后
> items("a")，detect_leaks=1 报 208B/2 对象（各 104B = 64B RC 头 + 40B SSO
> 串）。gdb 遍历索引叶子位图确认退出时仅存两个非永生串：`third`（len5，
> optionxform 的 lower 结果，_interp_some:696 `var = self.optionxform(var)`）
> 与 `end`（len3，_read:530 提交的字典值）。LSan 栈指向 _read 是字符串池
> 复用假象。根因是**编译器 lowering 通用 bug**：`_interp_some` 的
> `if p < 0: accum.append(rest); return` 早返回在 var/v 首次堆写入之前
> 降级，单遍 `release_slots` 当时看不到这两个 unknown 槽的 slot_owned
> 标记而漏 dec；循环自然结束出口在循环体之后降级则全释放。循环次轮起早返回
> 时槽里已是上一轮 retain 过的堆值 → var 直接泄漏、v（字典借用）多持一份
> 退出时泄漏。
>
> 修复：把该早返回 `return` 改成 `break`，经循环自然结束出口（全槽释放）
> 退出。循环后无语句，break 与 return 语义完全相同，插值输出/字符串内容
> 不变。
>
> **编译器通用 bug 留负面**：已实现并验证 lowering 后置遍
> fixup_early_return_releases（用最终槽属性补齐早返回漏 dec），复现 leak→0，
> 但 38 热基准 asm 对拍 37 同 1 异——bench/daily/a2_config_parse.py 的
> coerce() 自身两个早返回同样漏 dec，通用修给它补 dec 改了 asm，违反
> 「flag-off 38 基准 asm 逐字节不变」硬闸门。正确修法在 lowering（出口释放
> 需用最终槽属性），须独立任务连同基准 asm 基线处理；本轮按硬闸门走纯
> stdlib 修。
>
> 闸门：最小复现与 253/254/390-394 detect_leaks=1 由漏转 0；全套件 normal
> 与 ASan（JOBS=1）EXIT=0；38 热基准 asm 逐字节全同；CPython 3.14 对拍
> 253/254/390-394 零差异；语料差分零差异；check_size 无新增超限。
## [0.355] - 2026-11-21

> **修复异常跨函数 raise/catch 的捕获异常泄漏**（预存运行期 bug，tarfile/
> configparser 批 detect_leaks=1 暴露；**纯 runtime 改动，src/codegen 零改动**，
> 门控关程序生成 asm 逐字节不变）。
>
> 机理（取证修正 batch4 §六 / batch7 的「RC 环」判断）：except 体里 raise 新
> 异常会 longjmp/_Unwind 跳过 ExcDone，被 pa_exc_accept_value 接管成
> current_exc 的那个**捕获异常**（含 phys/phys_extra 帧链）无人释放。setjmp
> 漏斗 pa_raise / pa_raise_value_from / pa_raise_args 与零开销 pa_eh_raise 都
> 漏了这步收尾（零开销 pa_eh_raise_value_from 早已正确实现 drop_save +
> free current_exc）。单站点时泄漏对象仍被 current_exc 全局根可达、LSan 不报；
> 多站点时早先的异常被埋进 exc_save 链 prev，退出 pa_exc_shutdown 释放 save
> 链后不可达 → 报 Direct leak。**并非 RC 环、也不依赖周期 GC**（实测 GC
> runs=0、PYADAMAS_GC_DISABLE 无影响；泄漏对象是 malloc 的 PyAdamasException）。
>
> 修复：上述漏斗对齐 pa_eh_raise_value_from —— pa_exc_drop_save +
> exc_free(current_exc) + current_exc=NULL。捕获异常的 traceback 已克隆进
> except as e 的 ExcValue，释放不丢内容；异常匹配/展开/finally 复制/生成器
> 挂起/async 投递语义不变。
>
> 闸门：最小复现（gzip 两站点 except OSError→raise MyErr）与 363/390/391
> detect_leaks=1 由漏转 0；异常/生成器钉子 92/93/94/119/153 门控关/开 × ASan
> leak 全 0；sem_baseline 42/42；全套件 normal 与 ASan（JOBS=1）EXIT=0（365
> ran ok，0 FAILED/DIFF）；O0/O2 语料差分 346/0；CPython 对拍 840/0；flag-off
> asm 38 热基准逐字节全同；check_size 无新增超限。
>
> **负面结论（独立预存 bug，非本次异常修复范围）**：392/253 在 detect_leaks=1
> 下仍各留字符串泄漏（392 624B/6 对象、253 208B/2 对象）。最小复现：
> ConfigParser 读 `"[a]\nslash = %(third)s/%%plit\nthird = end"` 后
> items("a")——**无异常参与、门控关也复现、GC 不参与**，是插值路径的字符串
> refcount 泄漏，与异常 raise/catch 无关；batch7 将其归因「异常 RC 环」不准确。
> 本次异常修复对其无影响（基线同值），留待单独修复。

## [0.354] - 2026-11-21

> **标准库自举第七批**（stdlib7 分支，基线 master 1589e34 / 0.353）。纯
> PyAdamas 源码按 CPython 3.14 语义重写 stdlib/configparser.py；**不碰
> src / runtime / codegen，无 C 胶水、无 BigInt**，模块表也未改
> （configparser 在 0.326 已登记）。轻量标尺：同参数同功能同结果，错误
> 类型正确、消息合理（253/254 钉住的路径仍逐字对齐）。
>
> 覆盖：read（单路径或路径列表）/ read_string / read_file / read_dict；
> sections / options / defaults / has_section / has_option / items；get
> （DEFAULT 继承 + vars 覆盖 + raw + fallback，含 fallback=None 返回 None）；
> getint / getfloat / getboolean（8 种布尔字面量、ValueError）；set /
> add_section / remove_section / remove_option（空节名与 "DEFAULT" 操作默认
> 节）；write（space_around_delimiters、多行缩进、InvalidWriteError 键校验）；
> 映射协议与 SectionProxy（下标读写/in/len/iter/get*/items/keys/values/
> update/setdefault/pop/clear）；BasicInterpolation %(key)s（%% 转义、10 层
> 深度、缺失/语法/深度三类错误）；RawConfigParser 与
> ConfigParser(interpolation=None)。
>
> 语义要点：解析按 CPython _read 状态机（节头尾随文本、最左分隔符、缩进
> 续行、空行计入多行值、注释行不计入、解析错误累积到文件末尾并先提交已解析
> 部分、[DEFAULT] 可重复声明、strict=False 后者覆盖）；错误类型
> Error/NoSectionError/DuplicateSectionError/DuplicateOptionError/
> NoOptionError/Interpolation*Error/ParsingError/MissingSectionHeaderError
> 同名同继承，另加 InvalidWriteError。
>
> pa 表示层边界（记录在模块头与 docs/stdlib-batch7.md）：默认参数只收字面
> 常量，fallback 的「省略」用私有串哨兵 + _is_unset 双判；object 静态类型
> 的 None/False 经 print 显示为 0（值比较正确），故 get 返回 object、
> getboolean 标 bool；defaults/read_dict/vars 的值要求 str（CPython 会
> str() 化）；c["s"] 静态类型 unknown，需
> `from configparser import SectionProxy` 后标注变量；实例下标删除与
> list(parser) 不支持，用 remove_section/pop 与 for 循环代替。
>
> 回归 390-394（CPython 3.14.4 跑同一源码逐行对拍，5/5 逐字节一致）；既有
> 253/254 保持全同。全套件 normal 与 ASan（JOBS=1）EXIT=0；check_size 无
> 新增超限；编译器零改动，既有程序 asm 与基线一致（详见 docs/stdlib-batch7.md
> 第七节）。

## [0.353] - 2026-11-21

> **修复编译器自身求值顺序 UB 堆越界**（预存 bug，stdlib6 轮 ASan 编译器构建 118_suspend_expr 暴露）：
> `lower_impl.h def()` 的 `vreg_nonneg[r] = nonneg_of(in)` 左值 vector 引用与右值求值顺序
> 隐患——改为先算右值临时量 `nn`、显式守 `vreg_nonneg.size()` 再赋值，杜绝悬空引用/越界。
> 生成 asm 逐字节不变（16 基准 cmp）、全套件 360 ok、ASan 编译器构建运行 118 exit 0 无 UB。
> 纯正确性修复。继 0.345 vreg_str UAF 后第二处 ASan 编译器构建暴露的预存内存安全 bug。

## [0.352] - 2026-11-21

> **标准库自举第六批**（stdlib6 分支，基线 master f70d7e8 / 0.351）。纯
> PyAdamas 源码补齐 stdlib/statistics.py 的 CPython 3.14 核心面；**不碰
> src / runtime / codegen，无 C 胶水、无 BigInt，模块表也未改**（statistics
> 原本已登记）。轻量标尺：同参数同功能同结果，错误类型正确、消息合理。
>
> 覆盖：mean / fmean / median / median_low / median_high / median_grouped /
> mode / multimode / pvariance / variance / pstdev / stdev / geometric_mean /
> harmonic_mean / quantiles / covariance / correlation / linear_regression；
> StatisticsError 保持 ValueError 子类；linear_regression 返回
> LinearRegression(slope, intercept) 轻量类（字段 + 同 CPython repr，非
> namedtuple）。
>
> 语义要点：mode 并列返回**最先出现**的值（CPython 3.14 语义，3.8–3.10 的
> “no unique mode” 抛错口径废弃）、multimode 按首现序；quantiles 对齐
> CPython 3.14（单点数据返回 n-1 个该值、exclusive 的 j 钳制到 1..ld-1、
> 未知 method 抛 ValueError）；variance/pvariance 支持 mu/xbar（object=None
> 哨兵，显式 0.0 不被吞）；correlation 支持 linear/ranked；linear_regression
> 支持 proportional。
>
> 精度边界（无 BigInt/Fraction，float+math.fsum 近似，tests 逐行钉住）：
> mean([0.1,0.2,0.3]) 得 0.19999999999999998（CPython 0.2，double rounding）；
> pvariance([1e50,1,-1e50]) 差 1 ulp；harmonic_mean([40,60]) 得
> 47.99999999999999（CPython 48.0）。其余普通量级、[1e50,1,-1e50] 的
> variance、covariance/correlation、quantiles、geometric_mean、
> median_grouped 等与 CPython 逐位一致。int 保留性/0.0 vs 0/不能解包等
> 类型差异记录在模块头与 docs/stdlib-batch6.md。
>
> 回归 380–385（CPython 3.14.4 跑同一源码逐行对拍；仅上述三条边界行不同，
> 其余零差异）；171 更新为 CPython 3.14 quantiles 口径。既有程序 asm 对
> 基线 f70d7e8 的同配置构建：342 个 tests/run 中 336 逐字节全同；差异只有
> 105（pvariance 新增可选 mu 形参，调用点多传 None，属被内联模块签名本身）
> 与 380/381/382/384/385（新用例在基线缺符号，按预期编译失败）。
>
> 闸门：全套件 normal 360 ran ok / ASan（--asan 运行期插桩，JOBS=1）
> 360 ran ok / EXIT=0；O0/O1/O2 三档统计用例输出一致；check_size.sh 无新增超限。
> （注：把编译器自身也做 ASan 插桩的 build-asan 会暴露 118_suspend_expr
> 在 lower_impl.h:399 的**既有**求值顺序 UB 堆越界，与本批 stdlib 改动无关；
> 本批 ASan 闸门用 --asan 的生成代码+运行时插桩口径。）

## [0.351] - 2026-11-21

> **SIMD 通用化深化**（vect2 分支，基线 master faeb80d / 0.350）。扩展 0.344
> 落地的通用循环向量化器（codegen_vect.cpp），补上 docs/simd-vectorization-design.md
> §11.7 P4+ 的三项 tractable 路径：**int 乘 map/reduction**（pmuludq 奇偶 lane
> 全 i64 重建，模 2^64 与 imulq 逐位一致，任意 i64 含负数/INT64_MIN **无需值域
> 守卫**）、**Convert plain i64→float**（cvtdq2pd 折算 + pshufd 聚 dword，中间量
> 全精确、末条 addpd 单次舍入 = cvtsi2sdq，>2^53/INT64_MIN/MAX 逐位一致）、
> **多 reduction 并行累加器**（双 float/双 int/混合/三累加器，共享 expr 用
> scratch 副本保序提取，出口各自写回）。设计 §11.8 记录完整推导。
>
> 门控 `PYADAMAS_VECT` **默认关**不变。同时修复五个潜伏 bug：通用 reduction
> 路径自 0.344 fbc77a8 起实际是死代码（read_before_store 判据反用）；int acc
> 出口覆盖写丢循环前累加器值（298 rw 形状静默算错）；ConstInt 不变量广播漏收
> 致悬空 tail 标签（49_inline 链接失败）；LICM 外提值误判逐 lane 类；跨块装载
> list 基址解析到被 RC move-from 清 0 的槽现值致 NULL 段错误（376 内联形态）。
>
> 闸门（基线 2c58a99 独立 worktree 对拍）：flag-off asm **38 热基准逐字节全同**；
> difftest vect-on **335/0**；38 基准语料 flag-on/off 输出零差异；全套件 normal
> vect-on/off 与 **ASan vect-on JOBS=1 均 EXIT=0（354 ran ok）**；check_size 无新增
> 超限（codegen_vect.cpp 998 行）；spectral_norm/nbody/matmul 输出钉住
> （811972 / -928502048 / 43198080）且 abbench 不回归。
>
> 回归 375-378（非基准，CPython 参考对拍零差异）：int 乘大值回绕/原地/临时槽/
> dot/前值非 0；Convert 全值域/float(j)/float(j+c)/不变量/乘后折算/保序 reduction；
> 多 reduction 10 形状；别名依赖负例（跨迭代/写后读/写偏移非 0/循环变量外读/
> 运行时别名守卫/IndexError 时机）asm 零 VECT 保守跳过取证。
>
> 性能（非基准探针 N=200000×300 轮，min-of-9 交替绑核，整程序墙钟）：int 乘 map
> **2.36×**、Convert map **2.34×**、多 reduction **1.38×**（checksum 逐位一致）。

## [0.350] - 2026-11-21

> **标准库自举第五批**（stdlib5 分支，基线 master b0e2f8b / 0.349）。纯
> PyAdamas 源码补齐三个缺口：stdlib/textwrap.py 按 CPython 3.14 口径重写、
> stdlib/string.py 补 Template 与 capwords(sep)、stdlib/pprint.py 补
> **sort_dicts=True 默认**与 indent/width/depth/compact 参数面。**不碰
> src/runtime/codegen、无 C 胶水、无模块表改动**（三个模块本来就在表里），
> 因此编译器二进制与基线同源。
>
> 现有程序 asm（对基线 b0e2f8b 的独立 worktree 构建逐字节 cmp）：可比的
> **459/459 全同、0 差异**，13 个文件两边都以同样方式不可编译（check-only
> 夹具/环境门控脚本，非 asm 口径）；改动过的三个模块的既有使用者
> （54/106/140/170/330）差异只来自被内联的 stdlib 模块代码本身。
>
> 终验（JOBS=1）：全套件 normal **350 ran ok EXIT=0**、ASan **350 ran ok
> EXIT=0**（含新增 370-372）；check_size 无新增超限。

### 新增

- **stdlib/textwrap.py 重写**（CPython 3.14 口径）。旧实现只做「按空白切词
  的贪心装行」，忽略 break_long_words/break_on_hyphens，没有 max_lines/
  placeholder/expand_tabs/replace_whitespace/drop_whitespace/fix_sentence_endings。
  新版：自写 ASCII 扫描器复刻 3.14 的 `wordsep_re`（空白段 / 词间 em-dash /
  连字符断点，含 `(?<=lt{2}-)`|`(?<=lt-lt-)` 两种 lookbehind 与 `lt -? lt`
  lookahead），`_wrap_chunks`/`_handle_long_word`/`_split_chunks`/
  `_fix_sentence_endings` 逐条对齐（allowance=1 尾逗号预留、drop_whitespace、
  max_lines 截断与 placeholder 回填）；dedent 换成 3.14 的 min/max 公共前缀
  算法；indent 补 splitlines(keepends) 内实现（纯空白行不加前缀、行尾保留）；
  shorten 走 `TextWrapper(max_lines=1)` 口径。ValueError 逐字一致
  （`invalid width ... (must be > 0)` / `placeholder too large for max width`）。
- **stdlib/string.py 补 Template 与 capwords(sep)**：`$$` 转义、`$identifier`、
  `${identifier}`（ASCII 标识符 + IGNORECASE）、substitute（缺键 KeyError(name)）/
  safe_substitute（缺键与非法占位符原样保留）/ is_valid / get_identifiers；
  非法占位符 ValueError 的行列口径按 3.14 的 `_invalid` 公式（占位符后的位置
  + splitlines(keepends)）。capwords 支持 sep（`sep=""` 与 CPython 一样抛
  ValueError("empty separator")）。常量保持逐字节相同。
- **stdlib/pprint.py 补 sort_dicts=True 与参数面**：dict 键排序后格式化
  （**单行 _safe_repr 与断行两条路径都排序**）；indent（每层缩进 + 开头
  (indent-1) 空格）、width（allowance 语义：非末项留 1 列给尾逗号）、depth
  （level>=depth 截断 {...}/[...]/(...)，3.14 口径）、compact（_format_items
  的宽度预算 + ", " 续行）；set 元素排序；长字符串按 _pprint_str 折行（含
  多行字符串与 level==1 的括号包裹）。排序不依赖 sorted()：键值对走自写稳定
  归并排序，比较器先试自然序 `<`，混类型不可比时按 (str(type), repr) 兜底
  （CPython `_safe_key` 用 id 打破同类型名平局，地址序不可复现）。错误口径
  indent<0 / depth<=0 / width==0 逐字一致，PrettyPrinter 也在构造期校验。
- **回归**：tests/run/370_textwrap.py（CPython 逐行对拍，106 行输出）、
  371_string.py（111 行）、372_pprint.py（106 行）；330 的边界注释同步更新。

### CPython 3.14.4 对拍

- textwrap：22 文本 × 13 宽度 × 14 参数组 + shorten/dedent/indent/谓词 的
  **8490 行**输出逐行相同；只剩 2 行 `repr` 的 `\x0b/\x0c/\x1c` 未转义差异，
  属运行期 repr 面（字符串本身相同）。
- string：**1481 行**（38 模板 × 9 mapping + safe/is_valid/get_identifiers +
  capwords 10 文本 × 5 sep + 常量）：27 行差异 = 1 行常量 repr 控制字符未转义
  + 26 行 mapping 取值的 `str()` 退化（float/None/bool，见边界）。
- pprint：**1926 行**（26 个值 × 17 组参数：width 1/5/10/20/40/80、indent
  0/2/4、depth 1/2/3、compact、sort_dicts=False、混类型键、长/多行字符串、
  PrettyPrinter、三条 ValueError）**逐行完全相同（0 diff）**。

### 语义边界 / 负面（均有模块 docstring 记录）

- textwrap：词字符按 ASCII 判（CPython 的 `\w` 对 str 是 Unicode 语义），
  非 ASCII 连字符断点可能不同；indent 的换行集为 ASCII 子集；`max_lines`
  用 **-1 哨兵**表示不限（CPython 的 None）——pa 的可空 int 槽里 None 与 0
  同形，用 None 默认值会把 `max_lines=0` 误当不限（现在 0 的截断语义与
  CPython 一致）；CPython 3.14 在「indent 比 width 宽 + 文本以空白开头」时
  的死循环（CPython 自身 bug）在 pa 侧检测无进展即收手，不死循环。
- string：Template 没有 `delimiter/idpattern/pattern` 类属性（pa 类体不支持
  类作用域常量），分隔符固定 "$"；substitute 只收一个 mapping 实参（不支持
  CPython 的 `**kws`），mapping 需是 dict；取值经 8B 元素（无逐元素 tag），
  float/None/bool/容器值的 `str()` 会退化（要保持输出一致请传 str 或同构 int）。
- pprint：**没有递归检测**（运行期没有 id()），自引用容器会无限递归
  （CPython 打 `<Recursion on ...>`）；容器元素无逐元素 tag（嵌套 bool/None/
  tuple/set/bytes 与同构 float 容器退化）；`saferepr/isreadable/isrecursive/pp`
  未提供；bytes/bytearray 不折行；长字符串折行按 ASCII 空白；`depth` 用
  **-1 哨兵**表示 CPython 的 None（原因同上；其余 depth<=0 照常报错）。

## [0.349] - 2026-11-21

> **P1 定宽整数收尾打磨**（fixint2 分支，基线 master e570f9d / 0.348）。
> 补 docs/numeric-types-landing.md §6 负面清单里 tractable 的三项：u64 大
> 整数字面量、定宽容器元素字面量 coerce、u64 列表无符号排序，另加可选的
> cimport 窄/无符号 C 类型映射。**现有程序（无定宽标注）的生成 asm 逐字节
> 不变**（bench 全量 120 文件 cmp 全同，逐块复验）；提升规则/回绕语义不变
> （332-335 回归全过）。
>
> 终验（JOBS=1）：全套件 normal **347 ran ok EXIT=0**、ASan **347 ran ok
> EXIT=0**；difftest 328/0；语料零差异（tests/run 既有全量，基线 e570f9d
> 原始构建 vs 当前，按文件头 type-tags/lang-gaps 环境，0 DIFF）；新增
> 365-368 与 CPython/C 语义模型对拍一致、O0/O1/O2 逐字节相同；check_size
> 通过。机制与负面清单详见 docs/numeric-types-landing.md §9。

### 新增

- **u64 范围十进制大字面量**（>INT64_MAX..2^64-1）：词法按无符号解析成位
  模式并打 `u64_lit` 标记（token/AST 各加一个 bool），只有 u64 上下文消费：
  标注声明/赋值/实参/默认值/return 五类 coerce 位置 + 容器元素位置 +
  提升到 U64 的 binop/compare/augassign 操作数。未被消费的照旧报
  "integer literal too large for int64"（**默认 int 字面量行为不变**，报错
  从词法期移到类型检查收尾，消息逐字一致）；超 u64 范围仍在词法期报错。
  进制前缀路径（0x/0o/0b）保持既有回绕行为逐字不动。u64 默认值跨模块
  序列化用新 tag 8（旧缓存 tag 0..7 解码逐字不变；旧解码器读 tag 8 按
  「有默认但不可表示」降级）。新增 tests/run/365（CPython 非回绕段对拍 +
  pa 回绕钉子）与反例 f70/f71/f72。
- **定宽容器元素的字面量 coerce**（landing §6.3 开口）：元素类型是定宽时，
  字面量在容器位置按元素类型范围检查——范围内隐式（类型检查改写静态类型，
  降级按元素类型发常量），**超范围编译报错，不静默窄化**。覆盖：
  list.append/insert/extend、set.add、下标写入 l[0]=5 / d[k]=v、dict 键的
  字面量下标（读/写）、list/set/dict 字面量（标注声明/赋值/实参/返回值/
  +=）。非字面量仍走 merge_elem（窄化要 cast，锁住报错）。**insert 补齐了
  此前完全缺失的元素检查**（与 append 同口径；bench 无 insert 用例，asm
  不变）。空字面量保留既有洞回填路径；tuple 字面量 coerce 未开（留后续）。
  新增 tests/run/366（与 CPython 逐字节对拍）与反例 f73/f74/f75/f76。
- **u64 列表无符号排序**（landing §6.4 修复）：sort()/sorted() 的比较器按
  元素类型分派——sort_kind 新增 **5=u64**（运行时 LESS_U64 timsort），容器
  排序描述符新增 **'u'** 字符（嵌套 list/tuple 里的 u64 元素同样无符号）；
  sorted(key=) 的键比较同步支持 kind 5。窄定宽（i8/u8/…/u32）槽内规范值下
  有符号机器字比较逐例等价，仍落 kind 0，**其它类型排序路径逐字不变**。
  sorted(key=恒等函数)（lambda x: x / def k(x): return x）且元素是 u64 时，
  键域跟随元素类型（无标注函数返回类型推不出的既有洞，只对 u64 开口）；
  标注 -> u64 的键函数本来就走 kind 5。min/max 走 IR Cmp 早已无符号，不动。
  新增 tests/run/367（与 CPython 逐字节对拍，含稳定性/嵌套/200 元素归并路径）。
- **cimport 窄/无符号 C 类型 → 定宽 TyKind**（可选任务，landing §6 补充）：
  .cpya 新增 `char`/`int8`→i8、`short`/`int16`→i16、`uchar`/`uint8`→u8、
  `ushort`/`uint16`→u16、`uint64`/`ulong`→u64（CKind 追加在枚举末尾，既有
  值不变）。覆盖常量（u64 常量按无符号解析）、参数/返回、全局变量读写、
  ptr[T] 解引用、按值结构体字段（1/2/8 字节宽，符号/零扩展）、回调跳板
  （C 侧窄参数按宽度提升到 64 位）。字面量实参/赋值同口径 coerce，非字面量
  窄化要 cast。**既有 int/int32/uint32 仍映射默认 int**（改它们会动既有程序
  的提升语义，不做）；cdecl 生成器、varargs 定宽实参、list[i8]→char* 缓冲区
  编组未开（留后续）。新增 tests/run/368（cfixedint.c 辅助库，C 语义模型
  对拍）与反例 f77/f78。

### 变更

- **src/codegen_call.cpp**（1238 行 → 735 行）：C 互操作边界编组
  （gen_call_extern / gen_load_extern / gen_store_extern / gen_ptr_load /
  gen_ptr_store）拆入 **src/codegen_cffi.cpp**（516 行）。纯代码移动，函数体
  逐字不变（本轮新增窄宽度编组后超 1200 行硬限，拆回限内，check_size 清零
  状态保持）。

## [0.348] - 2026-11-21

> **代码整洁重构：拆分超 1200 行文件**（refactor1 分支，基线 master 2ce0820 /
> 0.347）。**纯代码移动/拆分，生成 asm 逐字节不变**：5 个基线超限文件全部拆回
> 硬限内，check_size.sh 超限清零。不改任何语义、不改函数体/签名/声明顺序。

### 变更

- **src/codegen_regalloc.cpp**（2581 行 → 904 行）按职责拆成三个 TU：
  - codegen_regalloc.cpp：帧布局 / vreg 栈槽 / fp+slot 活跃性 / 通用线性扫描池；
  - **codegen_regalloc_scan.cpp**（1019 行）：变量槽寄存器分配
    （assign_slot_regs_split / graph / linear / 入口分派）；
  - **codegen_regalloc_temp.cpp**（575 行）：块内临时寄存器 / 跨块 vreg 提升 /
    操作数缓存 / classify；
  - **regalloc_loop_depth.h**（123 行）：block_loop_depth 原为 static 工具函数，
    拆分后两个 TU 共用，改 inline（函数体逐字移动）。
- **src/driver_build.cpp**（1604 行 → 974 行）按职责拆成三个 TU：
  - driver_build.cpp：构建 / 缓存 / manifest / 链接驱动；
  - **driver_modules.cpp**（378 行）+ **driver_modules.h**（68 行）：模块表查询 /
    import 源解析 / AST import 扫描 / 原生+扩展接口合并（原匿名名字空间函数
    改外部链接，签名与函数体不变；模块表数据仍留在 cpp 匿名名字空间）；
  - **driver_check.cpp**（269 行）：check 模式 import 图（只建接口不生成代码）。
- **src/typecheck_types.h**（1593 行 → 899 行）按域拆成三个组成部分头
  （由 typecheck_types.h 末尾按原顺序包含，声明顺序逐字不变）：
  - **typecheck_promote.h**（148 行）：定宽整数分类与提升规则；
  - **typecheck_query.h**（212 行）：类型判定与显示（is_num/type_eq/type_name）；
  - **typecheck_join.h**（376 行）：类型合并 / 赋值兼容 / 容器元素合并。
- **src/ir_ops.h**（1398 行 → 227 行）按域拆分：RtFn 枚举 + rt_name 名字表
  移入 **ir_rtfn.h**（1177 行，ir_ops.h 名字空间内原位展开的组成部分头）；
  IR 算子枚举（BinKind/UnKind/CmpKind/StrOp/IrOp）留在 ir_ops.h。
- **src/lower_impl.h**（1356 行 → 1087 行）：class Lowerer 的引用计数成员声明
  （271 行）移入 **lower_impl_rc.h**（276 行），在类体内原位置 #include 展开，
  声明顺序与内容逐字不变。
- CMakeLists.txt：新增 codegen_regalloc_scan.cpp / codegen_regalloc_temp.cpp /
  driver_modules.cpp / driver_check.cpp 四个源文件。

### 验证（每拆一个文件独立过一遍）

- **38 热基准 asm 逐字节不变**（bench/realistic + compare + daily，cmp 基线
  2ce0820 同 commit 构建）：每个拆分步骤 38/38 IDENTICAL。
- 逐行核对：每个原文件的每一行都原样保留在新文件集合中（纯移动证明）。
- difftest 324/0；全套件 normal + ASan EXIT=0（343 ran ok ×2，JOBS=1）。
- tests/check_size.sh：超限文件 5 → 0（函数长度警告不变，仅警告）。

## [0.347] - 2026-11-21

> **标准库自举第四批：tarfile**（tar1 分支，基线 master 7924474 / 0.346）。
> 纯 PyAdamas 源码：`stdlib/tarfile.py` + 模块表 +1 行，不碰 lower /
> runtime / codegen，**不新增 C 胶水**。stdlib-b3 曾以「纯 py 侧没有
> open()/通用文件对象」跳过 tarfile；P4 块2 的 file 协议泛化 +
> io.BytesIO（0.343/0.346）落地后该理由不再成立。设计与边界详见
> docs/stdlib-batch4.md。

### 新增

- **stdlib/tarfile.py**（ustar 核心，1158 行纯源码）：
  - **TarInfo**：字段（name/mode/uid/gid/size/mtime/chksum/type/linkname/
    uname/gname/devmajor/devminor/offset/offset_data）+ 类型谓词
    （isfile/isreg/isdir/issym/islnk/isdev/ischr/isblk/isfifo/iscont/
    issparse）+ get_info + frombuf/tobuf。头编解码按 ustar 512 字节布局：
    name(100)/mode(8)/uid(8)/gid(8)/size(12)/mtime(12)/chksum(8)/
    typeflag(1)/linkname(100)/magic(6+2)/uname(32)/gname(32)/devmajor(8)/
    devminor(8)/prefix(155)/padding(12)；八进制数值字段纯整数算法
    （`_oct`，不用 % 动态宽度）、chksum 双算（无符号/有符号）、
    base-256 读侧（63 位内）、长名 prefix+name 拆分、目录尾斜杠归一。
    type 常量按 CPython 3.14 是 **bytes**（b"0"/b"5"/b"2" …）。
  - **TarFile**：mode r/w/a/x（含 :gz、r|/w| 按整读整写）、fileobj
    （BytesIO/真 file）或路径、getmembers/getnames/getmember（末次同名
    优先）、extractfile（普通文件返回 BytesIO、软/硬链接跟到目标）、
    addfile/addfile_bytes、add（递归目录 + arcname）、gettarinfo（lstat
    面）、extract/extractall（安全名净化 + chmod/utime + 目录属性延迟）、
    close（两块全零尾 + RECORDSIZE 补齐，尺寸与 CPython 一致）、with、
    __iter__。错误按 CPython 3.14 口径：关闭/模式不符抛 OSError，
    KeyError 文本一致，r/r:* 首头坏掉包成 ReadError。
  - **扩展头读面**：GNU 长名/长链接 L/K（**连续叠加**，K→L→真头用累积
    循环而非递归）、pax x/g（path/linkpath/size/mtime/uid/gid/uname/
    gname；x 覆盖下一个成员，g 对后续全体生效）。写侧只写 ustar。
  - **gzip 透明**：r:gz/w:gz/x:gz 走 stdlib/gzip；r:*/r 魔数嗅探；
    bz2/xz/zst 抛 CompressionError；a:gz 不支持。
- **src/driver_build.cpp**：模块表加 tarfile（deps gzip,_gzipio,os,os.path，
  1 行）。
- **回归**：tests/run/360-364（TarInfo+头字节 / 内存面 / 磁盘面 /
  gzip+错误面 / 扩展头+format），全部**同一份 .py 在 CPython 3.14.4 与 pa
  下输出逐字节一致**，expect 由 CPython 输出钉死。

### 语义边界（轻量标尺，详见 docs/stdlib-batch4.md §3）

- 整读整写（内存与归档大小成正比）；只写 ustar（默认 format 与 CPython
  同值 PAX_FORMAT，但产出 ustar 头：短 ASCII 名成员与 CPython 默认写出
  **逐字节一致**）。
- 数值字段 int64：写侧八进制溢出抛 ValueError，读侧 base-256 超 63 位抛
  InvalidHeaderError（CPython 任意精度）。
- uid/gid 恒 0、无 uname/gname（_shutilc.stat 没有属主；无 chown
  落盘）；add 不做硬链接合并（没有 inode）；LNKTYPE 解压回退成复制。
- 不做 CPython 3.12+ 的 data_filter（filter=/numeric_owner= 不存在），
  名字按 zipfile 口径净化（绝对路径/上跳落到解压根下）。
- extractfile 静态返回 BytesIO（目录成员返回空 BytesIO，CPython 是 None）；
  imported 类的类方法经模块限定名调用需改成 from-import（pa 语言面）。
- 稀疏文件（S）、Solaris X 头、pax/GNU **写**、bz2/xz/zst 是明确负面结论。

### 验证

- CPython 3.14 对拍：**360-364 共 5 个用例逐字节一致**（头字节 base64、
  归档字节 base64、错误类型、字段/mtime/mode）。
- 跨实现互操作（一次性手工验证）：pa 写的 .tar.gz 被 CPython 读出；CPython
  写的 .tar.gz（默认 PAX）被 pa 读出；CPython 写 GNU/PAX 的 186 字节长名 +
  长链接被 pa 读出；`tar -tzf` 能列 pa 的归档。
- 全套件 normal（JOBS=1）**EXIT=0，343 个 ran ok**；ASan
  （PYADAMAS_TEST_ASAN=1，JOBS=1）**EXIT=0，343 个 ran ok**（0 FAILED/DIFF）。
- 现有程序 asm 对基线逐字节不变：**489/489**（tests/run + tests/*.py +
  tests/modules + bench 全量与基线 7924474 的独立构建 cmp）。
- check_size 无新增超限（基线 5 个；stdlib/tarfile.py 1158 行 < 1200）。
- ASan detect_leaks=1：360/361/362/364 全 0；363 命中既有「except 里 raise
  另一异常」两站点的 RC 环泄漏（单站点不复现，最小复现见
  docs/stdlib-batch4.md §6，修需动运行期异常/GC 链，不在本批）。

## [0.346] - 2026-11-21

> **P4 语言缺口 块3+4**（lang2 分支，基线 master 2ef0a9d / 0.345）：
> **traceback 对象 / sys.exc_info + stdlib traceback** 与 **@dataclass
> 编译期展开**。门控 `PYADAMAS_LANG_GAPS=1` **默认 0**（开启强制
> type_tags=true）；门控关时既有程序生成 asm **逐字节不变**（448 个
> 基准/语料 cmp 全同）。设计与边界详见 docs/lang-gaps-landing.md §8。

### 新增

- **块3 traceback / sys.exc_info**：抛出点 _Unwind_Backtrace 捕获生成码帧
  （ip-1 行号归因，内部包装帧跳过），phys/phys_extra 双链 + except 落地按
  handler 帧 CFA 裁剪提交（零开销走 personality 钩子，setjmp 走帧记录地址
  锚点）——CPython 口径：tb 从接住帧开始、裸 raise 不加当前帧、raise e
  整链前插、with/生成器/finally 重抛不重复捕获。运行期门控 LangGapsEnable
  入口模块体首发；O2 内联门控开时关（帧归因）；IRFunction.debug_name
  （<module>/裸方法名）。sys.exc_info() 三元组（类值单例复用块1 + 异常值 +
  tb RC OBJ 包帧链克隆）；__pa_tb_frames intrinsic；stdlib/traceback.py
  纯 pa format_exc/print_exc/extract_tb（源行运行期读+缓存）。ExcValue
  携带帧链副本（finally 复制/raise e/生成器挂起不丢 tb）；富异常
  accept_instance 门控开时补上下文接管；unknown 域异常值 str/repr/print
  门控开分支；sys.executable（argv[0] 借用口径）。
- **块4 @dataclass 编译期展开**：typecheck_dataclass.cpp 类体字段收集后
  合成 __init__/__repr__/__eq__ AST 注入类体，复用既有方法登记/槽分配/
  降级/跨模块接口编码（非运行期反射）。用户方法逐方法不覆盖；__eq__
  isinstance 守卫 + 类型化局部绑定；__eq__ 无 __hash__ → unhashable 既有
  规则。stdlib/dataclasses.py 恒等装饰器；门控开降级跳过装饰器调用。
  v1：单类 + 字面量默认 + int/float/bool/str/bytes/list/dict/tuple/set；
  frozen/order/kw_only/field()/继承/可变默认/默认序违规等 7 类编译期拒绝。
  静态容器/bytes repr 门控开支持（set/bytes 专用入口，探针分不出表示）。
- 回归：tests/run/350（traceback/exc_info，CPython 3.14
  PYTHONNODEBUGRANGES=1 口径逐字节）、351（dataclass 逐字节）、
  tests/fail/352-358（门控负面）、tests/modules/dc_dataclass（跨模块
  合成方法导出 + 缓存）。run_tests.sh fail/strict/unsupported/modules
  循环认 type-tags/lang-gaps 文件头指令（纯加法）。

### 修复

- **重查轮不传门控**（块1 遗留）：typecheck_infer replay TypeChecker 不
  复制 lang_gaps_，递归触发 result-hint 重查时异常类名退回 unknown，
  isinstance(e, ValueError) 降级报错。next.set_lang_gaps(lang_gaps_)。
- **单值 BoolOp 悬空块**：lower_boolop 单值时不发射跳转，当前块落进下一块
  （合成单字段 dataclass __eq__ 恒 False）。单值直接求值；语法分析器不
  产生单值 BoolOp，基线程序不受影响。

### 验证

- flag-off asm 对基线逐字节不变：**448/448** 既有基准/语料 cmp 全同。
- 异常/生成器钉子 **92/93/94/119/153**：门控关/开 × ASan detect_leaks=1
  **10/10 全 0**；350/351 ASan detect_leaks=1 为 0。
- 350/351 O0/O1/O2 与 CPython 3.14 逐字节一致。
- 全套件 normal（JOBS=1）**338 ran ok EXIT=0**；ASan（PYADAMAS_TEST_ASAN=1，
  JOBS=1）**338 ran ok EXIT=0**（336 基线含 350 + 351 + dc_dataclass 模块）。
- difftest O0 vs O2：**319 用例 0 不一致 EXIT=0**（JOBS=4）。
- 语料 gate ON vs OFF（tests/run + bench 全量 440 文件）：**431 逐字节同 +
  4 门控用例（340/341/350/351）对 expect 一致 + 5 计时基准仅计时列差异**
  （cpy_ipc/bench_re/chan_throughput/spawn_latency/switch_latency，浮点
  归一化后逐字节同——本征非确定，计数/标签列全同）。
- check_size 无新增超限（超限仍是基线 5 个文件）。

### 负面 / 边界（文档化，docs/lang-gaps-landing.md §8）

- 块3：~~~^^^ 锚点不复刻（对拍用 no_debug_ranges 口径）；KeyError str
  引号/多参 str 沿 pa 口径；raise-from 链格式化不在本轮；outside except
  直接印三元组按 pa None 口径；__exit__ 体里 exc_info 为 None 三元组；
  生成器跨挂起 finally 重抛的陈旧 CFA 宁多勿删。
- 块4：None 默认进容器槽按 pa 一般类型规则拒绝；函数内类跨调用 eq
  （pa 静态编译一次 vs CPython 每次执行 class 语句）；嵌套 set 元素 repr
  探针口径；类类型字段/Optional/定宽整数/深层嵌套 qualname 不在 v1；
  unhashable 是编译期报错（CPython 运行期 TypeError）。

## [0.345] - 2026-11-21

> **修复编译器自身 heap-use-after-free**（0.342 编译速度轮引入，simd-general 轮 ASan 编译器构建暴露）：
> `ir.cpp vreg_str` 的 vreg 文本缓存用 `std::vector<std::string>`，`resize` 重分配搬移所有
> string 使先前返回的 `string_view` 悬空——`text_into` 单条 `format_to` 多个 `vreg_str`
> 实参先求值，大 r 触发 resize 使小 r 的 view 失效 → UAF。改 `std::deque`（尾部 resize
> 不失效已有元素引用）。生成 asm 逐字节不变（21 基准 cmp）、全套件 335 ok、ASan 编译器
> 构建运行 exit 0 无 UAF。纯运行时正确性修复，非优化。

## [0.344] - 2026-11-21

> vect1 分支：**SIMD 通用化（P4 tractable 核心）——通用循环自动向量化器**
> （docs/simd-vectorization-design.md §11）。新 codegen_vect.cpp，门控
> `PYADAMAS_VECT=1` **默认 0**。与特化 matcher（0.333/0.334 认
> spectral_norm/nbody/matmul 精确形状）互补：基于 **IR 结构 + 数据引用/别名
> 分析**识别任意可数 for 循环的逐元素 map / reduction，**不依赖精确指令形状**。
> 别名/依赖证不出保守跳过（标量 fallback）；浮点 reduction 保序累加逐位一致。
> flag-off 生成 asm 逐字节不变（38 热基准 cmp 全同）。

### 新增

- **通用循环向量化器**（codegen_vect.cpp/.h）：分析先行发射在后（循环结构 →
  槽分类 → 表达式 DAG → 别名/依赖 → 寄存器分配 dry-run 全过才发射，否则零字节）。
  覆盖：可数 for（步长 1 结构判据）+ 浮点数组逐元素 map（out[j+c]=f(a[j+c'],
  b[j+c''],不变量)，float add/sub/mul/div → SSE2 2-lane packed）+ 浮点 reduction
  （s+=f(...)，保序提取累加 addsd 低/高 lane，与标量逐位一致）+ 整数数组 map
  （add/sub → paddq/psubq 模 2^64 精确）+ 整数 reduction（paddq 2-lane + 出口
  水平求和）+ 原地 map（读在写前同偏移）+ 偏移下标（j+c）+ 体局部临时槽 SSA 替换。
- **别名/依赖安全判据**：不同 list 对象入口运行时两两判等守卫（循环版本化）；
  同对象读+写只允许同偏移 0 且读在写前的逐元素原地 map；len>=n+maxoff（溢出
  检查）&& kind==0 && j>=0 && j+1<n 守卫；循环变量副本槽外部有读 → 跳过。
  向量循环零调用，IndexError/负索引/kind 升级语义与时机不变。
- **规范化数组基址**：非内联函数体内 load.slot/load.global 解析到稳定来源，
  同槽/全局不同 vreg 归并为同一逻辑数组；不变量广播 float/int 统一鲁棒化。
- **尾循环**：向量循环退出 jmp cond（计数器精确等于标量 j==n），余 1 经 cond
  回体块入口守卫失败走原标量体。门控写进模块缓存清单（manifest vect 行）。
- 回归 344-349：非基准 map/reduction/原地/偏移/int map + 别名依赖负例 + 边界
  （n=0/1/2/3、len<n IndexError 时机、kind 升级），CPython 对拍零差异。

### 验证

- flag-off asm 对 master 逐字节不变（38 热基准）；difftest vect-on 317/0；
  全套件 normal vect-on/flag-off EXIT=0（335/334 ran ok）；**ASan vect-on
  JOBS=1 EXIT=0（335 ran ok）**；check_size 无新增超限文件。
- 非基准探针（saxpy+dot，N=200000×30）vect-on **1.89×** 加速，checksum 逐位
  一致；spectral_norm/nbody/matmul 输出钉住不变、性能不回归（0.98/1.00/1.01×）。

### 修复

- 槽 read_before_store 顺序误判致 classify 自环栈溢出（编译器崩溃）：min_load_pos
  完整扫描后统一判序 + classify visiting 环检测（编译器绝不崩溃）。

### 负面 / 剩余路径

- 本轮不做 AVX2（需 cpuid+xgetbv 检测 + ymm/xmm 提升池冲突重设计）；不做改序
  float 水平求和（输出风险，保序已接近 2-lane 上限）；int 乘（pmuludq 组合+
  守卫）/Convert int→float/多 reduction/写后读 forwarding/循环变量外读维护
  留待后续（§11.7）。通用自动向量化是 gcc/clang 数年大功能，本轮交付结构判据
  驱动、别名/依赖保守、门控默认关的安全通用核心。

## [0.343] - 2026-11-21

> lang1 分支：**P4 语言缺口 块1+2——异常类当值 + contextlib.suppress/closing；file 协议泛化 + io.StringIO/BytesIO**
> （docs/numeric-types-design.md §3/§4 P4）。门控 `PYADAMAS_LANG_GAPS=1`
> **默认 0**，开启时强制带上 P2 type_tags（CLASS tag=11 是表示基础）。
> flag-off 生成 asm 逐字节不变（410 个基准/语料文件 cmp 全同）。接编译速度
> 0.342 之后集成，定版 0.343。设计/边界/负面
> 结论见 docs/lang-gaps-landing.md。

### 新增

- **内建异常类当值**：ValueError/IndexError 等名字在值位置得到 ClassObj
  类型（内层 Exception[name]），降级成运行期类对象单例（rt_lang.c 按名
  缓存，pa_class_new cid=-1/无 vtable，注册表持永久引用 + GC 根登记）。
  过 unknown 带 CLASS tag=11 不退化成 0：同一性（is）、str/repr/print
  （<class 'ValueError'>）、__name__、容器存储/迭代全链路。
- **自定义异常类当值**：门控开时类名值位置统一 ClassObj（raise/except
  名字机制不受影响）；pass-only 异常类补建单例（旧早退路径门控开且
  value_used 时继续）。异常值构造取限定名末段（与 pa_exc_register 裸名
  登记口径一致），except 匹配正确。repr 按 CPython 显示完整限定名
  （<class '__main__.MyErr'>，判据=裸名在异常注册表；普通用户类 repr
  保持既有末段口径）。
- **isinstance/issubclass**：isinstance(e, ValueError)（e 是捕获的异常
  值）按 exc_is_a 父子链；富异常实例按 MRO 类名（pa_is_exc INSTANCE
  分支）。issubclass 异常类×异常类（含自定义×内建）运行期按名字判。
- **raise 经类值**：E = ValueError; raise E / raise E("x") / raise M
  （自定义）全链路；运行期读类名构造，不用静态内层名（形参回填可能
  留着首个调用点的类型）。
- **contextlib.suppress/closing**（stdlib/contextlib.py）：suppress 把
  类值存进 unknown 元组，__exit__ 用内部 intrinsic __pa_exc_is_a(et, E)
  按 with 协议的 et 名串判父子；closing 用 __pa_close(thing) 零参动态
  .close()。
- **方法名→槽运行期登记表**（rt_lang.c）：门控开时类定义点登记
  「限定名.方法名→槽」（pa_method_register），pa_method_call0 沿实例
  MRO 查表分派（尊重覆盖，与 pa_super_lookup 同元数据布局）；file
  对象直调 pa_file_close。closing 的 .close() 与后续 file 协议泛化共用。
- **探针路径识别类对象**：pa_str_any/pa_print_any/repr_any_into 按魔数
  认出类对象印 <class '...'>（门控关时类对象不存在，分支永不命中）。
- **类对象 GC release 回调**（pa_class_new）：字符串是索引叶子只靠 RC
  释放；活到 GcShutdown 的类对象（异常类单例）无 release 则 name 串
  泄漏。与 exc_value_gc_release 同模式，普通用户类纯加法。
- **回归** tests/run/340_exc_class_value.py（53 行 expect 由 CPython
  3.14 生成，O0/O1/O2 逐字节一致）；run_tests.sh/difftest.sh 支持
  `# pyadamas: lang-gaps` 用例级指令。
- **文件拆分**：typecheck_expr.cpp 的 Attribute case 逐字拆出
  typecheck_expr_attr.cpp（1214→976 行，check_size 无新增超限）。
- **file 协议泛化（块2）**：门控开时 file 标注/形参收 FileLike——带
  read/write 方法面的类实例（assignable 放宽，全局门控开关）。运行期
  pa_file_* 入口对非 file 值委托实例侧分派（rt_lang.c pa_fileinst_*，
  方法名→槽登记表沿 MRO 查表，尊重覆盖）——**零 codegen 改动**，门控关
  asm 天然不变。read/readline/readlines/write/writelines/seek/tell/
  truncate/close/flush/closed/fileno/isatty/readable/writable/seekable
  全方法面；name/mode/encoding/errors 实例侧 AttributeError（CPython
  StringIO 也没有）。print(file=...) 同口径收 FileLike。
- **stdlib/io.py**（新模块，注册进 kModuleTable）：纯 pa StringIO/
  BytesIO。CPython 口径：write 越界 NUL 填充、seek whence 1/2 只收
  offset 0（非零报 OSError，消息逐字）、错误消息逐字（Negative seek
  position N / Invalid whence (N, should be 0, 1 or 2) / I/O operation
  on closed file）、with 协议、closed 属性、getvalue。
- **csv 互操作**：csv.writer/reader 的 f: file 形参经同一分派直接收
  StringIO（stdlib/csv.py 源码零改动）；属性 getter 也进方法登记表
  （closed 面经 file 分派可读）。
- **回归** tests/run/341_io_stringio.py（65 行 expect 由 CPython 3.14
  生成，O0/O1/O2 逐字节一致，ASan detect_leaks=1 零泄漏）：StringIO/
  BytesIO 全方法面 + 错误面 + csv  round-trip + print(file=) + file
  形参分派（StringIO 与用户自定义 FileLike）+ for line in f + closing。

### 闸门（实测）

- flag-off asm 逐字节不变：**410/410 identical**（bench 全量 + tests/run，
  块2 后复验全同）。
- difftest O0/O2：**311 用例 0 不一致**（JOBS=4；JOBS=8 时 277 网络时序
  抖动一次，串行/低并行复验全过——固定端口 http.server 子进程本征负载
  敏感，非本版引入）。
- 全套件 normal（JOBS=1）与 ASan（JOBS=1）：见落地文档 §6（块2 修复后
  复验）。ASan detect_leaks=1 钉子：13/174/92/93/94/119 flag-off/on
  **12/12 全 0**；340/341 lang-gaps **0 泄漏**。
- 语料对拍（LANG_GAPS on vs off，358 文件）：**353 逐字节同 + 2 门控
  用例（340/341）对 expect 一致**；bench_re 仅计时列（本征非确定）；
  180_codepoint_perf 计时跳过；277 负载抖动一次（单独跑过）。
- CPython 对拍：340/341 逐字节一致（O0/O1/O2）。check_size 无新增超限。
- **门控关回归修复**：pa_is_file 探针原用 rc_index_has + size==，无
  unknown 值的程序跳过索引维护时真 file 查不到 → pa_file_write 误委托
  实例分派（167_stdlib_glob/81_os_extra 门控关崩溃）。改
  pa_unknown_is_heap（exact-RC 堆链回退）+ size>=sizeof（h->size 是池
  对齐 alloc_size；File 是最大 OBJ 种类，下限唯一）。lang_as_class/
  lang_method_fn 同步换探针。修后 167/81 对 expect 逐字节一致。

### 已知边界（详见 docs/lang-gaps-landing.md §5）

- 多参异常 str() 不引号（pa 既有口径）；异常实例 e.__name__ 不支持；
  closing.__enter__ 返回 unknown（as 绑定后不能调方法）；门控关时
  suppress 不可用（运行期 TypeError）。
- truncate(显式负参) 按默认处理（pa int 默认参只能是字面量，None 默认
  与显式 0 在 int 槽不可分）；str repr 不转义控制字符（既有 pa 口径，
  NUL 填充用 len/ord 验证）；BytesIO.write(str) 不抛 TypeError（bytes/str
  同运行期表示）；file 形参的 name/mode/encoding/errors 属性实例侧
  AttributeError。tarfile 模块本身未实现（FileLike+BytesIO 基础已就绪）。
- 块3/4（traceback/sys.exc_info、dataclass 编译期展开）：设计就绪，
  见 docs/lang-gaps-landing.md §8 剩余清单。

## [0.342] - 2026-11-21

> speed1 分支：**编译吞吐优化**（编译器自身运行速度，生成 asm 逐字节不变）。
> 每块独立构建 + asm 全语料 sha256 对照 + 全套件/difftest 闸门。测量方法见
> docs/compile-speed-throughput.md。

### 变更（六块，逐块独立构建 + 闸门）

1. **支配集位集化**（opt_loop.cpp compute_dom）：LICM/下标版本化的 dom[b]
   从每块 nb 字节的 char 向量改成 64 位字位集。不动点迭代、交集顺序、
   全 1 初始化都逐位保持，结果与原实现逐位相同。perf 热点 20.2% → 1.6%。
2. **regalloc 活跃性位集化**（codegen_regalloc.cpp assign_vreg_slots）：
   blk_use/blk_def/live_in/live_out 由每块 nv 字节 char 向量改位集
   （新增 src/block_bitset.h）。热点 14.2% → 1.1%。
3. **块内索引去哈希**：codegen 的 block_first_use_（VReg→块内首次读位置）
   由 unordered_map 改按 vreg 直接下标 + 脏项复位；assign_block_temp_regs
   的三张 per-block unordered_map<VReg,…> 同样改定长数组。原哈希插入 +
   配套 malloc/free 合计 ~15% 归零。
4. **去掉无读者的 IR 指令级索引**（ir.cpp rebuild_use_def）：inst_by_id /
   def_of / uses_of 自 0.3xx DCE 改线性计数后已无读者（全仓库 grep 只有
   写入），重建仍是逐指令哈希插入。只保留仍被 block_index() 读的
   block_by_id；指令 id 照旧分配（唯一性语义不变）。将来要恢复指令级索引
   应改成惰性显式入口。
5. **LICM/下标版本化定义块索引定长数组**（opt_loop.cpp hoist_loop /
   version_bounds_loop）：原来每个循环头调用一次就重建一遍
   unordered_map<VReg,int> def_block + 两个集合，改定长数组 + 按 vreg 下标。
6. **RcMark 水位打标 + 槽清零数据流位集 + 指令注释直写**：
   - vreg_is_rc_mark 从「每个 vreg 扫全函数」改成入口一次扫描打标（O(n^2) → O(n)）；
   - slots_needing_zero 的 must 数据流由每块 std::vector<bool> 改位集；
   - Inst::text_into / comment_inst：指令注释 format_to 直写复用缓冲，
     每条指令省掉 text() 临时串 + 前缀拼接 + 拷贝三次分配；vreg_str 按
     编号缓存；PYADAMAS_IR_CHECK 的每指令 getenv 改成只读一次。

### 实测

- 编译吞吐（413 文件语料 ×2 遍，taskset 绑核，base/exp 交替 5 轮取中位，
  load 6~15）：base **12.57s** → exp **6.87s**，**-45.3%**（单遍 5.68→2.99s）。
- 单文件重负载 tests/run/234_re_opt.py（626 块/2150 vreg）：
  **1.44s → 0.14s（10×）**。
- 热点前后（同口径 perf，语料两遍）：compute_dom 20.2%→1.6%、
  assign_vreg_slots 14.2%→1.1%、VReg 哈希插入（_M_insert_unique_node +
  emplace_uniq）合计 14%→0。

### 闸门

- 生成 asm 逐字节不变：413 文件全语料 sha256 逐文件对照全同 + 40 个最重
  文件 cmp 逐一相同；ir dump 60 文件逐字节相同。
- 套件 normal 327 ran ok EXIT=0；ASan 套件 327 ran ok EXIT=0；
  difftest 309 用例 0 不一致 EXIT=0；check_size 无新增超限文件。
- 详细方法论/剩余方向见 docs/compile-speed-throughput.md。

## [0.341] - 2026-11-20

> fixint1 分支：**P1 定宽整数——i8/i16/i32/i64 + u8/u16/u32/u64**
> （docs/numeric-types-design.md §1）。**无门控**：定宽是 opt-in 标注，
> 默认 int 的代码路径与生成 asm 逐字节不变（bench 120 个文件 cmp 全同）。
> 接 P2 0.340 之后集成，定版 0.341。设计/边界/负面结论见
> docs/numeric-types-landing.md。

### 新增

- **新 TyKind**（typecheck_types.h）：I8/I16/I32/U8/U16/U32/U64，
  Int 仍是 i64（i64/long/int 解析到 Int）。标注别名：char=i8、
  short=i16、uchar/u8、ushort/u16、uint/u32、ulong/u64。存储统一 8 字节
  i64 槽：有符号符号扩展、无符号零扩展（**不改**帧布局/RC 头/ABI）。
- **提升规则一处实现**（typecheck_types.h int_promote）：浮点 > 较宽整数；
  同宽 signed+unsigned → unsigned（C 口径，i64+u64→u64）；bool 提升为
  对方；位运算同规则；移位跟随左操作数；比较先提升再比（结果 bool）。
  类型检查、降级期两侧转换、常量折叠共用同一份判断。
- **codegen 宽度/符号**（codegen_int_fixed.cpp，从 codegen_ops.cpp 拆出）：
  结果按宽度回绕正常化（movsbq/movzbq/movslq/movl/移位扩展），
  u64 用 divq/shrq 与 setb/seta（分支形 jb/ja），除零检查对 u64 跳过
  INT64_MIN/−1 溢出误报；宽度 < 64 的无符号槽内值恒非负，signed 指令
  逐例等价（不变量见文档 §3）。
- **字面量 coerce + 显式 cast**：定宽目标 + 字面量范围内隐式（赋值/
  声明/实参/默认参数/return/类字段），**超范围编译报错**；宽→窄必须
  cast_i8…cast_u64（C 式截断 + pa 回绕）；复合赋值按 C 口径隐式转回。
  abs/sum 对定宽按 C 整数提升返回 int。
- **u64 无符号 I/O**（runtime）：pa_str_from_u64/pa_print_u64/pa_fmt_u64/
  pa_uint_to_base（str/repr/f-string/format/printf-%d/hex/bin/oct），
  u64→f64 走无符号折算；printf 描述符新增 'u'。
- **跨模块类型编码** 17..23（0..16 逐字沿用，旧 modcache 兼容）。
- **回归** tests/run/332-335（提升/回绕/符号/I-O/unknown 边界/控制流）+
  反例 f67（字面量超范围）、f68（缺 cast）、f69（实参缺 cast）。

### 闸门（实测）

- 默认 int 程序 asm 逐字节不变：bench **120/120 identical**。
- difftest O0/O2 309 用例 **0 不一致**；全套件 normal **327 ran ok EXIT=0**；
  ASan（JOBS=1）**327 ran ok EXIT=0**；新用例 O0/O1/O2 输出一致。
- CPython 对拍：332 逐字节一致；334 仅 §6.1 的 u64→unknown→−1 一行
  差异（预期边界）；333 钉 pa 回绕语义（不与 CPython 对拍）。

## [0.340] - 2026-11-20

> types1 分支：**P2 unknown 运行期类型标签——解锁 isinstance/repr/pprint**
> （stdlib-b3 取证的根本缺口）。门控 `PYADAMAS_TYPE_TAGS=1` **默认 0**；
> flag-off 生成 asm 逐字节不变（117 个基准/语料文件 cmp 全同，含 P3 的 5 个
> unknown 基准）。tag 值分配**不改动 P3 既有取值**（0..5 逐字不变），只追加：
> NONE=6 / BOOL=7（非堆标量，RC 跳过）与 TUPLE=8 / SET=9 / BYTES=10 /
> CLASS=11 / BIG=12（堆子类，预留 BigInt）；门控开时堆 ⇔ 3..5 或 >=8，
> 门控关时逐字沿用 P3「堆 ⇔ tag>=3」。设计/边界/负面结论见
> docs/numeric-types-design.md §2 与 docs/type-tags-landing.md。

### 新增

- **tag 扩展**（codegen_impl.h/tag_defs.h、runtime/rt_internal.h）：P3 的
  PaTag 保持 0..5 不变，追加非堆标量 NONE/BOOL 与精确堆子类 TUPLE/SET/
  BYTES/CLASS/BIG。None/Bool 产生点发专 tag（ConstNone、bool 常量、静态
  类型导出、直调形参/返回传播）；Tuple/Set/Bytes/类对象按静态类型导 tag
  ——这些子类在对象头 kind 里分不开（tuple 与 list 同为 LIST、set 与 dict
  同为 DICT、bytes 与 str 同为 STR、类对象藏在 OBJ），静态类型是唯一来源。
- **运行期类型码**（runtime/rt_any.c）：`pa_any_type(v,tag)` →
  NONE/BOOL/INT/FLOAT/STR/BYTES/LIST/DICT/TUPLE/SET/OBJECT/CLASS/OTHER；
  有精确 tag 用 tag，否则 HEAP/UNK 读头 kind（UNK 先探针）。`pa_any_type_is
  (v,tag,code)` 是 isinstance 谓词（object 匹配一切、int 接受 bool，CPython
  口径）。
- **tag 版 repr/print**（runtime/rt_str.c）：`pa_repr_any_t` /
  `pa_print_any_repr_t` 按类型码分派：None→None、bool→True/False、
  tuple→(...)、set→{...}/set()、bytes→b'...'（CPython 的定界符选择与
  \xNN 转义）、类对象→<class '...'>；HEAP/UNK 回退旧探针路径。
- **isinstance 降级**（lower_builtin_reflect.cpp）：unknown 值的
  int/bool/float/str/bytes/list/dict/tuple/set/object 判定改发
  `pa_any_type_is`（P2 之前这些一律保守 false）。
- **跨调用 tag 传递**（codegen_tag.cpp/codegen_call.cpp/codegen_block.cpp）：
  直调 tag 数组布局改成「第 i 个形参 = 第 i 个字、长度 = 形参个数」（被调
  按自己的形参个数校验，不匹配整体忽略——错位 tag 会让 RC 按错误堆性
  inc/dec）。跨模块/间接调用（导入函数、函数值、闭包）用 `pa_ptag_in/
  pa_ptag_len` 侧信道传 tag：`%r11` 仍归目标地址，旧入口 gadget/vtable/
  闭包布局逐字不变，运行时回调（vthread/生成器 resume）读到 0 即回退 UNK。
  unknown 返回值的 tag 从 %rdx 落 dst（静态签名不含浮点时；含浮点走 shim
  重排不取）。
- **stdlib/pprint.py**（新模块，注册进 kModuleTable）：pformat/pprint/
  PrettyPrinter；单行 repr 放得下就用它，放不下按 CPython 的断行缩进/
  dict 键值对齐/单元素 tuple 尾逗号口径。内部 intrinsic
  `__pa_any_items`（unknown dict 的 (key,value) 对物化，未知值不支持方法
  调用）与 `pa_any_iter_list_t`（带 tag 迭代，未进 RC 索引的标量元组也
  能迭代）。
- **回归用例 328-331**（tests/run，文件头 `# pyadamas: type-tags` 由
  runners 自动以 PYADAMAS_TYPE_TAGS=1 编译；tests/run_tests.sh 与
  tools/difftest.sh 支持该指令）：328 isinstance 全类型矩阵、329 repr
  全类型（None/True/tuple/set/bytes/嵌套）、330 pprint（CPython 逐字节
  对拍，含断行）、331 RC 压力（tag 导出堆性维护位图、unknown 槽反复改写、
  直调 tag 参数/返回值；ASan detect_leaks=1 为 0）。

### 已知边界 / 负面结论

- **容器元素没有逐元素 tag**（容器只存 8B 值 + 1-bit 堆位图）：从容器/
  字段取出的值只能按头 kind 判型 —— 嵌套的 bool/None 退化成 int、
  tuple/set/bytes 退化成 list/dict/str。顶层值与静态类型已知的边界不受
  影响。
- **虚调用（vtable）不传 tag**（调用点没有可信的静态方法签名）：类方法
  形参一律 UNK 回退；因此 pprint.PrettyPrinter 的方法版 pformat/pprint
  对 tuple/set/bytes 参数退化成探针口径（模块函数版正常）。
- **异常类当值仍不可用**：内建异常类在 pa 运行时只有名字（无类对象），
  unknown 里退化成 0，contextlib.suppress 仍不通（P4/traceback 一起做）。
- **pprint 不排序 dict 键**（CPython 默认 sort_dicts=True）：按插入顺序
  输出；需要 CPython 顺序时用键已排好序的字面量。
- **既有 leak（非本版引入，flag-off 同样复现）**：list 在 len>0 时从
  「全堆(kind1)」转「混合(kind2)」不新建位图，销毁回退探针，未进 RC 索引
  的标量元组漏 dec。328-331 已规避该形状；修复属 list 运行时核心，另案。

## [0.339] - 2026-11-20

> borrow1 分支：**borrow 细化——只读借用循环变量槽免每轮 inc/dec**（R3 取证
> 定位的真正通用 RC 杠杆，压 RC 密集容器循环，真实日常语料受益）。门控
> `PYADAMAS_BORROW_OPT=0/1` **默认 0**；flag-off 生成 asm 逐字节不变
> （38 热基准 cmp 全同）；manifest 缓存键加 `borrowopt`。RC 正确性是命门
> （历史多起 borrow UAF/leak）：fail-closed 白名单判定，可证安全才做，
> 任何一条不确定整槽/整函数跳过。设计/安全论证/负面清单见
> docs/borrow-loop-design.md。接 stdlib3 0.338 之后集成，定版 0.339。

### 新增

- **lower_borrow.cpp `borrow_readonly_slots`**（门控，降级后 opt 前，与
  finalize_param_borrows 同位置）：候选槽的每条 StoreSlot 精确匹配 retain
  形状 [RcInc(v); old=LoadSlot; StoreSlot; RcDec(old)]，v 溯源到
  IndexLoad（list/tuple 借用读），容器溯源到稳定根——PARAM（形参从不覆盖）/
  GLOBAL（本函数从不写）/ FRESHLOCAL（唯一实写为新分配 MakeList/MakeTuple/
  ListNew，分配值与 LoadSlot 只作纯读不逃逸）。全函数扫描：生成器/async/
  vthread/chan/Class/unknown 槽/extern/ptr 整类跳过；用户调用仅 FRESHLOCAL
  根允许（容器不逃逸够不着）；list 修改类接收者必须可证不与根别名
  （dict/set/deque/file 修改运行时类型上不别名 list/tuple 根，故
  `for w in words: d[w]=d.get(w,0)+1` 的 fresh dict 写入不影响 words 借用）；
  CallRt 白名单 Pure/Dispatch（后者仅 FRESHLOCAL），**未知编号默认 Bad**。
  借用值 use 分析（Mov 传播）：纯运算/len/容器读/分支/Return（先 inc）/
  存入别处（先 retain）/白名单调用；RcDec/RcTemp* 显式禁止。出口 release
  形状逐条匹配（清零常量可夹在 dec/store 之间），del 的 load/store0/dec
  顺序不同自然失配。改写：删 inc/old-load/dec 与出口 load/dec，**保留出口
  清零**，置 slot_owned=false。
- **计数守恒**：容器持有元素那份引用贯穿函数体——基线循环期间 C+1、出口
  dec 回 C；借用恒 C。任何对象计数任何时刻不降到 0，析构时机逐点不变。
  **不需要跨迭代值 identity**（每轮借不同元素同样守恒）——R3 的 identity
  分析是外提 retain 的前置，不是 borrow 的前置。
- **协同**：slot_owned=false → codegen frame_owned_ 不含借用槽 → setjmp
  longjmp 与零开销 _Unwind 展开都不 dec 借用槽（容器才是 owner）；GC 借元素
  经容器根可达（形参根调用方帧持有/本地根帧记录/全局根表）；内联
  slot_owned 随槽搬进调用方、入口对借用槽只清零不释放；临时栈不触碰
  （IndexLoad 借用不入栈）。
- **门控管线**：LowerOptions/Program.borrow_opt；main.cpp 读
  PYADAMAS_BORROW_OPT；manifest 写/读/比对 borrowopt。
  PYADAMAS_BORROW_OPT_STATS=1 打印命中槽数/删除指令数（诊断）。
- **回归 304-310**（expect 钉死，CPython 对拍）：304 命中形态（for-range/
  for-each/while/嵌套/tuple/fresh-local/str 方法/早退）；305 对抗（容器
  append/set/del/sort/重赋值/别名修改/del s/自赋值/覆盖写/逃逸/unknown/
  object 标注——须保守跳过或安全变换）；306 异常（IndexError/raise/finally
  早退/嵌套/循环后读/双循环/**setjmp 多参 raise 展开**）；307 生成器跨挂起
  借用+调用方挂起期改容器/genexpr/send/async；308 GC 压力（churn/环垃圾/
  dict churn，借元素经根存活）；309 内联复用/交替实参/递归；310 vthread+chan。

### 正确性口径

- 历史教训（lower_impl_slots.cpp 注释）：0.323 unknown 槽每写漏一份、
  gc02_uaf_junk 野指针。本轮防线：形状精确匹配（del/自赋值/None 覆盖/
  temp_take 自然失配）、根不逃逸证明、修改接收者别名分析、use 白名单
  fail-closed、未知 RtFn 默认 Bad、出口释放点全匹配（漏删即多 dec → 拒绝）。
- 命中 7/38 热基准：dict_wordfreq（count_words）、sort_struct（aggregate）、
  b1_log_parse（gen_logs 3 槽）、b2_normalize、b3_template、b5_re_extract
  （2 槽）、d6_rec_memo。保守跳过负面清单见设计文档 §5（根修改/Class/
  非堆元素/非新分配来源/tuple 变量下标 unknown/入口全局根/可挂起整类）。

### 性能（abbench 式交替 A/B，flag-on vs off，taskset 绑核，如实报）

三轮交替 A/B（B5×R9 → B20×R11 → B40×R13，打乱顺序 + 奇偶交换，中位数/p25）：

- **dict_wordfreq（RC 密集主目标）：-4.1/-3.7 → -4.4/-4.5 → -4.6/-4.4，
  稳健 ~-4.5%**（239ms→228ms）。count_words 借 w（形参 words 根 + fresh
  dict 写入不别名 list 根）；静态 RC inc/dec 130→124（count_words 体
  14→8）；动态 12000×400=480 万借用迭代 ×2 ≈ 960 万条 RC 调用消除。
- **sort_struct：-1.6/-2.0 → -1.6/-1.6 → -1.8/-1.8，稳健 ~-1.7%**。
- b1_log_parse（gen_logs 3 槽）~-1.2%、b3_template ~-1.4%（高精度两轮
  方向一致；初轮 +1.5/+2.9 复测证伪）。b2_normalize/b5_re_extract 中性
  （初轮 b2 -5.8% 证伪）。d6_rec_memo +1~2% 轻微回归方向一致（6ms 短案，
  噪声带内，疑布局效应，如实报）。
- **噪声标定**：未命中案 flag-on/off 产物 cmp 逐字节相同，初轮却量出
  binary_trees +4.8%/d5_finance +11.0% → 纯机器噪声；短案噪声带 ±3%。
- queue_sim/binary_trees/fasta/c1_ledger/d5_finance 未命中（根修改/Class/
  str 下标新串/非新分配来源/非堆元素），二进制逐字节同，收益如实报零——
  与 R3 一致：borrow 只压「稳定容器只读借用」一类，不是所有 RC 密集开销。
- 净结论：通用杠杆成立（日常语料 b1/b3 受益，非基准特化），主目标 -4.5%
  稳健可测；门控默认关，集成按语料净收益定默认值。详见设计文档 §8。

### 闸门

- ASan **detect_leaks=1** + UAF 钉子 13_gc/174_gc_construct_roots/
  88_defaultdict/192_dict_cache/66_genexpr/119_gen_except_suspend/
  92_exc_inline_rc/93_gen_exc_rc/94_suspend_positions/225_vthread_gc_suspend
  + 新回归 304-310，flag-on/off 各一遍全 0（含 setjmp 展开借用案）。
- 全套件 normal flag-on EXIT=0（312 ran ok，0 失败）；全套件 ASan flag-on
  （JOBS=1）EXIT=0（312 ran ok，0 失败）。
- difftest flag-on（O0 vs O2）294 用例 0 不一致。
- 语料 353 案（tests/run + daily 23 + _bugs 13 + realistic 6 + compare 9 +
  unknown 5 + json/re）flag-on vs off 输出+rc 零差异：351 逐字节同；
  bench_re 仅计时列差异（语义列逐字节同，计时本征非确定）；
  277_urllib_errors 并行端口竞争 flaky（产物 cmp 逐字节同，独跑 on/off
  各 3 遍全 0，套件 ran ok）。
- CPython 对拍：daily 23 案 flag-on 全同（a1/a3/a5 用 build+argv 对拍）。
- flag-off 38 热基准 asm 对 lead 0.337 基线逐字节 cmp 38/38 IDENTICAL。
- check_size 无新增超限（lower_borrow.cpp 738 行；既有 5 个超限文件与
  基线同一组，driver_build 1540→1551 本就超限）。

### 剩余/风险（负面结论，如实报）

- 非堆元素循环（d5_finance list[float]）本就没有 RC churn，无 borrow 可做；
  queue_sim/binary_trees/fasta 因根修改/Class/str 下标新串保守跳过——
  RC 密集案里只有 dict_wordfreq 形态命中，其余「密集」开销不是可省记账。
- 扩展方向（本轮不做）：新分配来源扩到 ListSlice/StrToList 等（c1_ledger
  main 仅 800 对，收益微小徒增信任面）；unknown 槽（历史洞多不碰）；
  dict/set 根（需键不被替换的修改分析）；append-only 根细化（queue_sim
  实际伴随重赋值，细化后仍不命中）；入口全局根 CFG 分析（收益有限）。
- 风险：改的是所有权记账，误判即 UAF/leak。门控默认关可回退；每块增量
  过 ASan leak+UAF 与语料对拍；未知 RtFn/op fail-closed。

## [0.338] - 2026-11-20

> stdlib3 分支：**纯 PyAdamas 标准库自举批**——把「纯字符串/纯算术就能做、
> 不需要 C 胶水、不需要原生类方法、不需要 BigInt」的一批高频模块用 pa
> 自己的语法写出来，随编译器分发（stdlib/<mod>.py），只做模块表登记，
> 不动 lower/runtime/codegen。轻量标尺：同参数/同功能/同结果；错误类型对、
> 消息合理但不逐字复刻 CPython；help/格式合法但不复刻算法。接 0.337 之后
> 集成，定版 0.338（borrow 顺延 0.339）。

### 新增

- **urllib.parse**（stdlib/urllib/parse.py）：urlparse / urlsplit /
  urlunparse / urlunsplit / urljoin / urldefrag、quote / quote_plus /
  quote_from_bytes / unquote / unquote_plus / unquote_to_bytes、urlencode /
  parse_qs / parse_qsl + uses_relative/uses_netloc/uses_params 与
  ParseResult/SplitResult/DefragResult。只收 str 输入；结果对象是普通类
  （字段/下标/len/repr/geturl 可用，元组解包不做）。回归 320。
- **secrets**（stdlib/secrets.py）：token_bytes / token_hex / token_urlsafe /
  randbelow / randbits / choice / compare_digest。**随机源是内建 random**
  （pa 没有 os.urandom/getrandom 原语），令牌强度弱于 CPython，接口与
  计算口径一致；randbits 走 int64（k<=63 为正）。回归 321。
- **calendar**（stdlib/calendar.py）：isleap / leapdays / weekday /
  monthrange / monthcalendar / timegm、day_name/day_abbr/month_name/
  month_abbr、weekheader、firstweekday/setfirstweekday、month/prmonth、
  calendar/prcal、mdays、error/IllegalMonthError/IllegalWeekdayError。
  不依赖 datetime（自算 days_from_civil），文本日历与 CPython 逐字节一致
  （含 str.center 的奇数补白规则）；day_name 等是普通 list，无 locale。
  回归 322。
- **struct**（stdlib/struct.py）：pack / unpack / unpack_from / calcsize /
  iter_unpack + error；格式前缀 < > ! = @ 与 x ? b B h H i I l L q Q f d
  c s p / 计数前缀。浮点位级编解码（double/float32）纯算术实现，带符号零、
  次正规、inf/nan 都对；'s'/'p' 收 bytes 或 str（ASCII 一致）。没有
  pack_into（bytes 不可变，无写缓冲原语）；Q/L 高位按 int64 回绕。回归 324。
- **queue**（stdlib/queue.py）：Queue / LifoQueue / PriorityQueue /
  SimpleQueue + Empty / Full；put/get/put_nowait/get_nowait/qsize/empty/
  full/task_done/join，maxsize 与 timeout 阻塞口径可用（time.sleep 轮询，
  M=1 协作式无锁）. 回归 325。
- **contextlib**（stdlib/contextlib.py）：nullcontext / @contextmanager /
  _GeneratorContextManager。**异常注回做不到**（异常实例/类过 unknown 形参
  退化成裸 8 字节），异常路径改用生成器 close()：清理照跑、异常不吞。
  suppress()/closing() 跳过（异常类同一性 / unknown 值方法调用都是 pa 不支持
  的能力）。回归 326。
- **difflib**（stdlib/difflib.py）：SequenceMatcher（find_longest_match /
  get_matching_blocks / get_opcodes / get_grouped_opcodes / ratio /
  quick_ratio / real_quick_ratio / set_seqs / set_seq1 / set_seq2，含 junk
  与 autojunk popular 剔除）、Match、get_close_matches、unified_diff。
  unified_diff / get_grouped_opcodes 返回 list（CPython 是生成器）；内容行
  只加前缀不补 lineterm（同 CPython）。回归 327。

### 未做（pa 子集不支持，见 docs/stdlib-batch3.md）

- **pprint**：unknown 值没有运行期类型标签（bool/None/int 同为裸 8 字节、
  tuple 无标签、set 被识别成 dict、bytes 与 str 混同），无法对任意对象正确
  递归 repr；实测证据见文档。
- **tarfile**：纯 py 侧没有 open()/文件对象（流式/seek 原语）。
- **traceback**：异常值没有 traceback 对象、没有 sys.exc_info。
- **dataclasses**：类布局编译期固定、无注解反射/动态类生成。
- contextlib 的 **suppress/closing**（异常类过 unknown 参数退化成 0；
  unknown 值不支持方法调用）。

## [0.337] - 2026-11-20

> dv1 分支：**去虚拟化深化 + 函数内联扩展——削减调用开销**（fib/nqueens/
> obj_dispatch），测量先行（perf annotate 三案取证，基线 0.336 绑核 24）。
> 门控 `PYADAMAS_INLINE_OPT=0/1` **默认 0**；flag-off 生成 asm 逐字节不变
> （92 基准 cmp 全过：compare+realistic+builtins+daily）。P0 入口别名/
> P2 守卫直调在 codegen（entry_ref + codegen_vcall_inline.cpp），P3 自递归
> 展开在 opt_inline.cpp。设计/取证/命门/负面结论见 docs/inline-devirt-design.md。
> 接 0.336 之后集成，定版 0.337。

### 新增

- **P0 调用入口对齐修复（布局保持式）**：取证发现 call.direct/FuncAddr/
  vtable/静态闭包/shim 都引用 .LfN，而 .LfN 发在 .p2align 5 之前——每次调用
  先穿最多 31 字节 NOP 雪橇（perf 实测 fib 8.4% 周期落在 NOP 上，nqueens
  递归调用同穿 10 字节）。在对齐后加 .LfNe 别名，引用经 entry_ref() 改指
  .LfNe；**填充字节原样保留**（.text 与基线逐字节同位，只改分支目标位移），
  避免 DSB 布局远端效应。EH/LSDA 基准 fname（.cfi_startproc）不变，ptag
  双入口相对布局不变。v1 挪对齐方案撤回（见负面结论）。
- **P2 devirt 深化（守卫直调）**：gen_guarded_vcall 整数叶子候选为空时，
  对单态/双态（≤2 候选）小方法（≤32 条/≤4 块、无 handler/EH/生成器/闭包）
  发射守卫命中 → 直调（gen_vcall 第三参 direct_fn，函数移入
  codegen_vcall_inline.cpp），失败回退原 vcall。守卫比较**实际虚表地址**
  （与 0.294 同构），继承共享函数共享命中标签、每个虚表单独比较。
  obj_dispatch 热路径 area() 已被 0.294 叶子内联消除，P2 只命中冷 __init__
  （实测中性）——机制交付，收益如实报零。
- **P3 纯标量自递归单层展开（fib 形态，严格界）**：self_unroll_function——
  ≤24 条/≤3 块、全槽/形参/返回 Int/Bool、无 handler/EH/生成器/闭包、体内
  除自调用外无任何其它调用（白名单指令集，抛不出 Python 异常 → traceback
  语义透明）时，≤2 个自调用点用展开前快照各内联一层（倒序内联保持位点
  下标；先 DCE 清 rewrite_direct_calls 遗留死 LoadGlobal；克隆体内自调用
  保留为 call，只此一遍）。调用计数 ~1/3，代码增长 ≤3×/≤64 条。
- **PYADAMAS_INLINE_DBG** 诊断：[inline]/[unroll] 决策追踪。
- **回归 300-303**（CPython 对拍钉 expect）：devirt 守卫语义（单态/双态/
  继承共享/带实参非叶子/异常穿过 devirt 调用点/后定义子类回退/全局累加
  融合形态）；内联与异常帧（helper raise + 递归 raise 不得跳过）；生成器
  帧不可内联（yield/try-finally/close）；自递归展开（fib/fact/双参/深递归
  + ackermann/list_sum/safe_rec 负例）。

### 实测（交替 A/B 绑核 24，打乱+奇偶交换，中位/p25）

- **fib 23.74 → 17.10 ms（−27.9%，p25 −27.5%）**（13 轮 BURST=10）；
  高 BURST=20 复核 22.74 → 16.13 ms（−29.0%，p25 −28.5%）。对 C++ 比值
  同相位交替 ~2.4× → ~1.7×。text +184 B（+0.3%）。
- **nqueens +0.2%（中性）**：无可内联小 callee（place 自递归 ~200 条超界，
  保守跳过）；text +0。
- **obj_dispatch −0.0%（中性）**：vcall 已被 0.294 叶子内联消除，剩余头部
  是 RC/全局流量（不在本轮范围）；text +64 B（+0.09%，P2 冷 __init__）。
- 其余 12 案噪声带内（rec_memo −1.1% 小正面；短用例 BURST=10 的 +3~4%
  经对照实验（±2.5% 噪声底）+ BURST=20 复测证伪为相位噪声）。
- 合计 1811.97 → 1805.15 ms（−0.4%）。

### 负面结论（撤回/不采纳）

- **P1 放宽小函数判据（48→96 条/12→20 块/预算 40/640）**：语料无可测收益，
  且 nqueens count(80 条)内联进 __pa_main 使热函数 place 布局位移，隔离
  实验 +1.8%（去掉放宽 +0.1%）。判据回退 48/12/24/256，保留 DBG 诊断。
- **P0-v1 挪对齐到 .LfN 前**：binary_trees DSB uops −32%、cycles +4.2%
  （布局远端效应，HANDOFF §6.2 再验证）——撤回，改布局保持式 v2。
- **nqueens 递归展开**：place 体 ~200 条含 list RC/运行时调用，超 P3 界，
  收益不抵代码爆炸/异常语义风险，保守跳过。
- **obj_dispatch devirt 深化收益 ≈ 0**：叶子内联比直调更彻底；剩余头部
  rc_inc/dec + GC 相位检查（~25%+）与全局 s/total 流量（~10%），借用语义
  迭代方向（HANDOFF §5.1）不在本轮范围。

### 验证（闸门）

- flag-off asm 逐字节不变：**92/92 cmp 全过**。
- difftest（flag-on O0/O2）：**287/0**。
- 套件 normal flag-on/off：**305 ran ok × 2，EXIT=0**；ASan flag-on
  **JOBS=1：305 ran ok，EXIT=0**。
- 语料零差异（flag-on vs off，输出+rc）：tests/run + 全部基准 **0 DIFF**。
- ASan detect_leaks=1 钉子：13_gc / 88_defaultdict / 192_dict_cache /
  66_genexpr / 119_gen_except_suspend / 92/93/94 / 300-303 **全 0**。
- 异常/生成器/vthread 钉子：92/93/94/119/153_traceback flag-on ran ok；
  220-229 vthread 全过。
- CPython 对拍：300-303 expect 钉自真实 CPython；fib/nqueens/obj_dispatch
  输出一致（2178309 / 14200 / 139011840000）。
- check_size：无新增超限（codegen_block 1200 / codegen_impl.h 1200 /
  codegen_call 1162；已知 5 超限不变）。
- manifest 缓存键加 inlineopt（切开关必然失效，实测验证）；逐块独立构建
  （c59c343 快照独立 worktree 构建 EXIT=0）。

## [0.336] - 2026-11-20

> **SIMD 默认开**（用户授权）：`PYADAMAS_SIMD` 默认值翻为 on（=0 显式关），数值核
> （spectral_norm/nbody/matmul）反超 Go 的收益自动生效。仅 opt≥2 非生成器非 ra_graph
> 非 tags 模块生效（codegen_simd 守卫），不满足整段标量 fallback，输出逐位一致；
> 全套件 normal+ASan 301 ok、difftest 283/0。LOOP_OPT/RA_OPT 仍 opt-in。

> ra1 分支：**寄存器分配改进——削减 spill/栈往返**（callee-saved 提升增强），
> 测量先行（perf annotate 复核 fib/matmul/nqueens 栈流量热点再选优化）。
> 门控 `PYADAMAS_RA_OPT=0/1` **默认 0**；flag-off 生成 asm 逐字节不变
> （92 基准 cmp 全过：compare+realistic+builtins+daily）。新逻辑在
> src/ra_opt.cpp（codegen_regalloc.cpp 已超限不再加逻辑）。设计/取证/
> 命门 bug 实录/负面结论见 docs/ra-opt-design.md。接 0.335 之后集成，定版 0.336。

### 新增

- **ra_opt.cpp**（新文件，两个门控分配改进，机制与现有跨块提升同构：
  整函数独占 callee-saved，无区间判断、无调用点 spill）：
  - **P2 只读槽 load 别名**：整函数驻留 GPR、函数内从未被写、且**寄存器
    全函数单射**（没有第二个槽认领）的槽，其单次 LoadSlot vreg 直接别名
    到槽寄存器（值恒等），不占新寄存器，load 指令整条消失。nqueens LICM
    提升的 row/n 共 6 份独立副本全部别名（perf 实测 row 重载占函数周期
    9.06%、n 上界 cmp 2.77%），place 栈 mov 78→68 条。mandelbrot +4.3%、
    fasta +3.3%、nqueens +1~2%（11 轮交替绑核 A/B 中位）。
  - **P1 同块跨调用 vreg 提升**：定义使用同块、活跃区间真跨过调用的
    整型值（单定义、Int/Bool 白名单、RcMark 排除），**只捡跨块候选挑剩
    的寄存器**（不按权重插队，结构性保证不劣化已有分配）。**自递归函数
    整函数跳过**：fib 实测提升 1/5 个寄存器分别净回归 3.8%/6.2%——递归
    每次调用（含叶子）都付 push/pop 税，perf 取证 push 27%/pop 15%。
    递归 callee-saved 提升需 shrink-wrapping 才可能正收益，本轮保守不做。
- **manifest 缓存键 raopt**（-1 = 旧清单失效一次），try_cache 校验，
  切开关不再命中旧产物。
- **回归 297-299**：P1 跨调用形状+递归 guard、P2 只读别名+重赋值负例、
  异常/生成器/vthread 命门（flag-on/off 双模式过）。

### 修复（开发期命门 bug，已钉死）

- **P2 首版静默算错**：误信「slot_reg_[s] 非空 ⇒ 整函数独占」——
  linear_scan_pool 让不重叠区间复用寄存器，c2_inventory 的 s0(n) 入口
  hoist 后即死、%rbx 被 LCG 状态槽接管，别名让循环上界读错值（rows
  600→1）；204/193/205/206/233/236 + 9 语料同根因。修复：寄存器单射
  校验（reg_users==1）。教训：注释描述的语义意图 ≠ 分配器实现保证；
  语料对拍比随机 run_diff 更能压出复用形状 bug（首版 run_diff 840/0
  但语料 9 错）。
- **P1 按 weight 插队回归**：nbody −1.5%（weight 误判把已有跨块 vreg
  挤出寄存器落栈）。改 leftover-only 后 nbody asm 恢复逐字节不变。

### 负面结论

- **fib**：callee-saved 提升在自递归函数上净负（叶子税），guard 跳过；
  GCC 对照是递归展开成迭代，非纯 regalloc 差距。
- **matmul**：6 个 callee-saved 被热区同时活跃的 6 个槽占满（槽分配
  本身最优），跨块不变量（i*N/k*N/aik副本）拿不到寄存器；P1/P2 无
  候选，asm 不变。统一分配/live range splitting 是历史多次失败区
  （codegen 不做区间判断），需独立一轮，本轮保守跳过。

### 闸门

- difftest（O0/O2）283 用例 0 差异 EXIT=0（277_urllib_errors 固定端口
  并行竞争偶发 rc 抖动，单跑 5/5 干净，JOBS=6 全量复跑 0 差异）。
- 全套件 normal flag-on EXIT=0；**ASan flag-on JOBS=1 EXIT=0**。
- 随机 CPython 对拍 run_diff 840 程序 0 失败；日常语料 23/23（输出+rc）。
- ASan detect_leaks=1 钉子 13/174/88/192/66/119 全 0。
- flag-off 92 基准 asm 逐字节不变；异常/生成器/vthread 钉子
  （92/93/119/225 等）flag-on 全套件内通过。
- check_size 无新增超限（ra_opt.cpp < 1200 行）。

## [0.335] - 2026-11-20

> intloop 分支：**整数循环优化**（跳转线程化 + 仿射 IV 边界检查消除），
> 测量先行（perf annotate 定位真实热点再选优化）。门控 `PYADAMAS_LOOP_OPT=0/1`
> **默认 0**；flag-off 生成 asm 逐字节不变（104 基准 cmp 全过）。=1 开推荐子集
> （掩码 3 = thread+bce）；bit2 同块 load 去重实测负收益，仅显式掩码开发用。
> 设计/取证/负面结论见 docs/intloop-design.md。接 0.334 之后集成，定版 0.335。

### 新增

- **opt_intloop.cpp**（新文件，三个独立门控遍）：
  - **P1 跳转线程化**（bit0）：多指令块的出边绕过单 jump 中转块直达链尾。
    中转块**原样冻结**（不裁块、不重编号、不改向）——两条实测教训：prune
    重编号扰动 regalloc 线性扫描区间坐标（matmul +9.7% 假回归）；清空/改向
    中转块让 codegen 的「回边 jump → 循环头 .p2align 5」代理标记丢失
    （matmul +8.2% / fannkuch +10.7%）。冻结后 nqueens continue 路径
    bb4→bb7→bb5→bb3 三跳变一跳（perf 实测中转 jmp 占 9.09% 周期）。
  - **P2 仿射 IV BCE**（bit1）：for-range 自然循环（单 IV、初值 0、步长 +1、
    bound 不变），下标 idx=IV+off（off 不变，含副本槽链追踪）或 idx=off。
    入口守卫 off>=0 && max>=off（回绕检测）&& max<len(base)，通过走
    unchecked 克隆循环，失败回退原检查循环（IndexError 语义/时机不变）。
    与旧 bounds-elimination-attempt 的关键区别：**克隆纯复制 + 候选翻
    unchecked，无外提/重定向**（旧 cand_dsts/redirect 混用是 fannkuch 死循环
    根因）；**允许 IndexStore**（不外提就没有读旧值问题）；只版本化最内层
    循环（防嵌套克隆爆炸）；体内定义被体外使用/多定义 vreg/含调用 → 拒绝。
    **kind0 证明存储**：循环内全 hk0 标量存储 + 入口 elem_kind==0 守卫 ⇒
    克隆存储 imm=1 走纯 mov 快路径（无 kind 检查/旧值读/call）。**摊销门槛**：
    常量 bound>=8 或运行时 bound 候选数>=3（fannkuch perm 旋转循环平均
    ~3.5 轮摊不平守卫，实测 +1.7%，门槛后跳过）。
  - **P3 同块 load 去重**（bit2，**不进默认**）：LoadSlot/LoadGlobal 块内
    局部值编号 + 再跑 CSE。正确但实测负收益（见下）。
- **codegen_index.cpp**：gen_index_store_unchecked 的 imm==1 && hk==0 快路径
  （纯 mov，5 条指令，无 call）。降级器发的 unchecked 存储 imm 恒 0，不走这里。
- **codegen_regalloc.cpp**：temp_reg_op_ok 接纳 imm==1 hk0 的
  IndexStoreUnchecked（序列只用 r0/r1/scratch_ptr/nv，都在块内临时池
  {r8,r9} 外）——**消掉冷路径 call 后块内临时值不再溢出，这是 matmul 收益
  的关键（不是去 cmp，是去 call）**。
- **门控管线**：Program.loop_opt → LowerOptions → optimize_ir；manifest 新增
  `loopopt <mask>` 行参与缓存失效（同 simd/rcsplit/ragraph 纪律，防 A/B
  命中旧产物）。PYADAMAS_LOOP_OPT：unset/0→0，1→3，其他整数→显式掩码。
- **回归用例** tests/run/293_intloop_bce.py（仿射形状 N=0/1/奇数 + 副本槽 +
  运行时偏移）/ 294_intloop_bce_bounds.py（命门：中途越界副作用已发生、
  首轮越界、零次循环、负偏移回绕、INT64_MAX 偏移回绕检测、运行时 bound、
  存储越界时机、三候选守卫失败、常量 bound 门槛上下）/ 295_intloop_thread.py
  （nqueens 早退链 + continue/break + or 链）/ 296_intloop_dedup.py（跨写不得
  合并）。expect 段全部 CPython 3.14 对拍生成，flag on/off 双模式通过。

### 正确性口径

- IV 范围证明：体内 S∈[0,BOUND-1]（初值 0、步长 +1、有符号 cmp.lt）；
  S 不回绕（S<=BOUND-1<=INT64_MAX-1）。idx=IV+off∈[off,BOUND-1+off]，
  守卫三条件 ⇒ idx∈[0,len)。max>=off 检测 BOUND-1+off 回绕。
- len 不变：循环内无用户调用，CallRt 仅纯标量白名单 + RC 记账；
  IndexStore 只改元素不改 len；base 槽体内无写、全函数无取地址。
- kind0 不变：入口 elem_kind==0 + 循环内全 hk0 标量存储 ⇒ kind 恒 0；
  旧值必是标量（无 RC），免读免 dec。
- 异常时机：守卫只含不抛指令；unchecked 路径已证不越界；守卫失败走原
  循环，IndexError 时机/负索引回绕语义不变。零次循环不多抛。
- 克隆共享槽（两版互斥执行），RC 记账原样复制配平；块号平移用
  rotate_one_loop 同款 bump（克隆紧跟循环体插入，保住槽区间紧凑——
  追加函数末尾会拉爆线性扫描区间，实测 +10%）。

### 测量（Xeon Gold 6252 / Cascade Lake，-O2 生成码，taskset 绑核交替 A/B 中位）

- **perf annotate 取证**（基线）：nqueens 98.75% 在 place，~19.6% 周期在
  continue 跳转链、~20% 在 row 栈重载（LICM 提升出 3 份独立 vreg 各自溢出）；
  matmul 89.5% 在 run，内循环 ~40 条/轮含 ~14 条栈往返（检查型下标冷路径
  call 迫使块内临时值全溢出）；fannkuch 65% 函数体 + RC ~8%；fib ~48% 栈 mov。
- **flag-on（掩码 3）vs off，11 轮交替 BURST=5 中位**：
  matmul 19.73→14.80ms（**-25.0%**）、nqueens 105.96→96.50ms（**-8.9%**）、
  fasta -4.0%、fib -1.8%、nbody -1.1%、fannkuch +0.9%（噪声级，门槛后中性）、
  binary_trees/mandelbrot/spectral_norm ±1.2% 内。
- matmul flag-on perf：热点变为 imul 9.5% + 元素写 8.7% + 不变量重载
  （i*N/k*N/aik 跨块 vreg 仍住栈——6 个 callee-saved 被槽占满），栈往返消失。
- 闸门：difftest 280/0（off+on）；flag-off asm 逐字节 cmp 104/104；
  套件 298 ran ok ×2（normal off+on）EXIT=0；ASan（JOBS=1）off+on EXIT=0；
  语料 302 文件 flag-on==off 零差异；check_size 无新增超限（5 个既有超限不变）。

### 负面结论（保留代码/结论，不默认开启）

- **P3 同块 load 去重**：正确但 matmul BCE 收益被吃一半（-24.4%→-11.7%），
  fannkuch -7.8%→-1.7%。机制：删 load 改变槽活跃区间 → 线性扫描重排 →
  克隆热槽丢寄存器。nqueens 期望的「row 合并抢第 6 个 callee-saved」未发生
  （-0.2%）。仅显式掩码 4-7 开发用。
- **fannkuch flip 内循环**（while i<m，双 IV i+=1/m-=1，bound 每轮递减）：
  本轮保守形状（单 IV + 不变 bound）匹配不上，跳过。对 C++ 2.4× 差距是
  结构性的（RC 分配 ~8% + 双 IV swap + 栈往返），需双 IV 仿射分析或
  regalloc 改进，独立一轮。
- **fib 调用开销**（~48% 栈 mov）：assign_global_vreg_regs 只提升跨块 vreg，
  同块跨调用活跃值（第一次递归结果）不在候选池 → 溢出。扩展候选池涉及
  整函数独占寄存器与块内临时池交互，风险高，本轮跳过（Go 也 16.9ms，
  C++ 7.3ms 靠内联/尾优化，非单纯 regalloc）。
- **IVSR 历史教训验证**：本轮不建累加器（12 累加器悲剧），BCE 去 call
  而非去 imul——matmul 的 imul 是业务乘法（aik*b），削不得；真正可削的
  索引 imul（i*N）已被 LICM 外提，剩余成本在溢出而非乘法本身。

## [0.334] - 2026-11-20

> simd2 分支：SIMD 向量化扩展到 **nbody（浮点逐元素 map + pairwise 力）与 matmul
> （整数内循环）**，建立在 0.333 codegen_simd.cpp 原型上。门控 `PYADAMAS_SIMD=0/1`
> **默认 0**；flag-off 生成 asm 逐字节不变。正确性优先：全部逐位一致（整数 i64
> 回绕语义精确重构、浮点保序），守卫不满足整段标量 fallback。设计见
> docs/simd-vectorization-design.md §10。接 simd 0.333 之后集成，定版 0.334。

### 新增

- **codegen_simd_int.cpp**（P3）：matmul 整数内循环 `c[base+j] += aik*b[base2+j]`
  （17 条核心体精确匹配）。SSE2 2-lane i64：movupd 取相邻两元素 → **pmuludq 组合
  精确重构 imulq 低 64 位**（aik∈[0,2^31) 守卫下 prod = aik*b_lo + ((aik*b_hi)<<32)，
  对任意 i64 b 含负数/≥2^32 逐位一致）→ paddq（=addq 回绕）→ movupd 写回。
  守卫：j>=0、base>=0、base+n<=len（溢出检查）、**b!=c 对象别名守卫**、c 读/写
  基址运行时判等、kind==0；aik 超范围整段标量。不做 pmulld 32 位截断（会算错）。
- **codegen_simd_nbody.cpp**（P2a）：位置更新 stride-7 map `b[i7+k] += dt*b[i7+k+3]`
  （34 条核心体）。沿 i 轴两体拼对（movsd×2+unpcklpd，SSE2 无 gather），dt*vel+pos
  逐 lane 逐位一致；7n<=len/kind0 守卫；奇数标量尾。只用 xmm0-4，任何 fp 提升形态安全。
- **codegen_simd_nbody_pair.cpp**（P2b）：pairwise 力循环（113 条核心体精确匹配）。
  **分量轴 [x,y]+z 标量**（不是 j 对轴——j 对轴需 16 xmm，advance 有 4 个 fp 提升槽
  占 xmm8-15 放不下；分量轴 11 个）。步长 1 无尾循环；d2 **保序水平求和**
  （movhlps+addsd 先 dx²+dy² 再 +dz²）；sqrtsd/divsd 与标量 fast path 逐位一致；
  i 侧 acc_x/acc_y 是独立累加器 → subpd 双 lane 不改序，acc 提升 xmm 跨轮持有，
  **退出/sqrt 负数守卫路径先写回**。别名结构判据：j7>=i7+7 守卫 → i/j 行不相交、
  行内读偏移{0,1,2,6}与写偏移{3,4,5}不相交 → b[i7+0..2,6] 循环不变 setup 一次装载。
  sqrt 负数双保险守卫（d2<0 → 写回 acc → 标量尾 ValueError 时机不变；NaN 走
  sqrtsd=NaN 与 pa_math_sqrt 一致）。**空闲 xmm 发现**：扫描 slot_reg_/vreg_reg_
  排除提升占用，11 寄存器动态分配，不够保守跳过。
- **共享判据**（codegen_simd.cpp）：simd_func_ok/simd_core_body/simd_incr_cond_ok/
  simd_loop_ops_ok/simd_slot_writers/simd_outside/simd_slot_stable/simd_scratch_safe；
  gen_block 挂钩改 try_simd_vectorize 分发（reduction → int map → nbody pos → pair）。
- **机器层**：MOp 登记 sqrtsd（仅枚举+名字表，现有发射点不引用）。
- **回归用例** tests/run/285_simd_int_map.py（向量路径 N=7/4/1，对拍 CPython）/
  286_simd_int_map_scalar.py（负 aik/aik>=2^31/别名/kind 混合/IndexError 时机/n=0/
  大 b pmuludq 重构/溢出回绕，on==off==O0==O1）/287_simd_nbody_map.py（5 体 25 步 +
  3/2/1 体小 trip-count，对拍 CPython 逐位一致）/288_simd_nbody_scalar.py
  （len<7n IndexError 时机/kind 混合/d2=0 inf-nan 语义/负 dt）。

### 正确性口径

- 整数：pyadamas i64 回绕语义（标量 imulq/addq 无溢出检查）→ paddq 逐位一致；
  pmuludq 组合在 aik∈[0,2^31) 守卫下对**任意** b 精确（推导见设计文档 §10.2）。
  溢出不是语义分歧点（回绕双方一致），守卫只保证乘法重构精确。
- 浮点：逐 lane IEEE 同标量 + 保序（d2 水平求和按标量结合序、i 侧跨 j 串行顺序
  逐轮保持、x/y 独立累加器无跨 lane 依赖）→ nbody 输出逐位一致
  （-928502048 == flag-off == CPython；N=1 全 35 元素逐位比对通过）。
- flag-on 只改匹配到的循环：38 热基准中仅 matmul/nbody/spectral_norm 出现 packed
  SIMD（10/35/36 条），其余逐字节不变。

### 测量（Xeon Gold 6252 / Cascade Lake，-O2）

- **matmul**（N=120，输出 43198080 == CPython）perf stat min-of-7 交替：
  cycles 34.46M→5.80M（**5.94×**）、instructions 89.2M→16.6M（5.36×）。
  墙钟 15 轮交替 min 12.76→5.95ms（2.15×，含固定启动）/ median 20.71→6.42ms（3.22×）。
  perf annotate：movupd 29.15% + pmuludq 14.18% 主导热点（flag-off 0 packed，
  每元素 ~49 条标量含边界检查/栈往返 → 向量每 2 元素 ~15 条）。
- **nbody**（N=50000，输出 -928502048 == CPython）perf stat min-of-7 交替：
  cycles 89.87M→20.81M（**4.32×**）、instructions 200.7M→55.7M（3.60×）。
  墙钟 15 轮交替 min 34.66→7.91ms（**4.38×**）/ median 48.84→19.36ms（2.52×）。
  perf annotate：99.07% 在 advance_1，packed 14.0%（movupd/mulpd/subpd/addpd/
  movlhps）+ 标量 sd 19.3%（保序 d2 链 + sqrt/div + z 分量）。
- **同窗口 Go 对比**（min-of-9 交替，go build 参照）：nbody Go 12.17ms /
  PA-off 32.80ms（2.69× Go）/ PA-on 8.49ms（**0.70× Go，反超 30%**）；
  matmul Go 6.45ms / PA-off 25.04ms（3.88× Go）/ PA-on 3.10ms（**0.48× Go，快 2×**）。
  0.331 差距（nbody 2.1× Go、matmul 2.4× Go）已消除并反超。

### 闸门

- flag-off asm 对 0.333 基线（build/lead）**逐字节不变**：38 热基准
  （realistic 6 + compare 9 + daily 23）asm cmp 全同 → flag-off 零回归。
- difftest（O0/O2，flag-on）**276/0**；全套件 normal flag-off/on **EXIT=0（294 ran ok）**；
  **ASan JOBS=1 flag-off/on EXIT=0（294 ran ok）**（flag-off 首轮 277_urllib_errors
  环境性 flake——与并行 CPython 对拍 job 资源竞争，standalone 双 flag ran ok，
  无并行重跑全绿；基线同样偶发，见 0.333 闸门取证）；285-288 normal+ASan flag-on/off 均 ran ok；
  CPython 对拍 38 热基准 flag-on/off 零差异；check_size 无新增超限
  （codegen_simd.cpp 461 / int 196 / nbody 206 / pair 397 行，codegen_block.cpp 1199 不变）。

### 剩余 / 通用化路径

- nbody pairwise 的 sqrt/div 仍是标量延迟链（分量轴未摊销）；j 对轴（sqrtpd 2 体）
  需 16 xmm，得先解决 fp 提升槽冲突（不变量 spill 到帧或提升池让位）——收益不确定，
  实测决定（§10.4）。spectral_norm 保序 addsd 链同理（0.333 §3.4 阶段 2）。
- 匹配器仍是精确形状匹配（17/34/113 条核心体），覆盖三个基准的热点形态；
  通用化（向量 IR op + 依赖分析 + 循环版本化）见 §9 P4，不在本轮。
- matmul aik<0 或 >=2^31 整段标量（pmuludq 组合守卫）；可扩展负 aik 修正项或
  全 64 位 emulation（9 条/2 元素，无守卫）——实测收益不抵复杂度时保持保守。

## [0.333] - 2026-11-20

> simd 分支：浮点循环 SIMD 向量化**原型**（设计 + spectral_norm 最小可行原型 +
> 实测）。门控 `PYADAMAS_SIMD=0/1`，**默认 0**；flag-off 生成 asm 逐字节不变。
> 本轮只证明一个循环模式（eval_A 的 `1/(i+j)` 除法 + 乘加累加 reduction）的收益，
> 不做通用自动向量化器。正确性优先：保序累加 → 与标量**逐位一致**，基准输出钉住。
> 设计见 docs/simd-vectorization-design.md。接 slim 0.332 之后集成，定版 0.333。

### 新增

- **设计文档** docs/simd-vectorization-design.md：循环模式识别判据、SSE2 packed
  double 起步、对齐/尾循环/reduction、别名依赖安全判据、浮点 regalloc 减 spill、
  与 cgspeed 结构化发射机器集成、门控、分阶段路径（P1 spectral_norm → P2 nbody
  逐元素 → P3 matmul 整数 → P4 通用化 → P5 AVX2）。
- **codegen_simd.cpp**（+类体片段 codegen_simd.h）：`gen_block` 挂钩识别 reduction
  热点循环体（可数 for + 浮点数组逐元素 map + 乘加累加），发 SSE2 2-lane 向量体
  （movupd/mulpd/divpd/paddq/pmuludq/psrlq/pshufd/cvtdq2pd/movlhps/movhlps）+ 标量
  尾循环。覆盖 mul_ata（a_is_j=0）与 mul_a（a_is_j=1）两形态；j 可提升 GPR 或在栈
  （%rsi 持有每轮写回）；匹配器容忍内联/外提常量。
- **机器层**：MOp 登记 packed SSE2 助记符（仅枚举，现有发射点不引用）。
- **门控**：PYADAMAS_SIMD 经 Program.simd → generate_asm → AsmGen::set_simd，
  写进模块缓存清单参与失效（与 rc_split/ra_graph 同口径）。
- **回归用例** tests/run/283_simd_reduction.py（向量化路径，N=5 奇数尾，钉住
  794176）/284_simd_reduction_scalar.py（标量 fallback/边界 n/大范围守卫/len<n
  IndexError 语义）。

### 正确性口径

- 保序提取累加（addsd 低 lane → movhlps → addsd 高 lane），total 与标量逐位一致，
  spectral_norm 输出钉住 811972（flag-on == flag-off == CPython）。
- 范围守卫 i>=0 && i+n<=46341（pmuludq 低32乘/psrlq //2/cvtdq2pd int32 精确）+
  len>=n && kind==0 && j+1<n（movupd 不越界、读裸 double）；任一不满足整段标量
  fallback，IndexError 等异常语义与时机不变。向量循环零调用 → 无 GC safepoint、
  不变量不可被改写。

### 测量（Xeon Gold 6252 / Cascade Lake，-O2）

- spectral_norm（N=600）perf stat min-of-7 交替：cycles 148.8M→34.1M（**4.36×**）、
  instructions 376.7M→85.4M（**4.41×**）。墙钟 15 轮交替 min 56.55→24.65ms
  （**2.29×**，含 ~8ms 固定启动）/ median 66.81→26.82ms（2.49×）。基线 3.4× Go
  → flag-on ≈1.55× Go。
- perf annotate：flag-off 0 条 packed、标量 sd 45.5%（addsd/divsd/mulsd）；
  flag-on 37 条 packed 45.4%（divpd/psrlq/pshufd/paddq/movhlps），标量 sd 降到
  38.4% 且集中在保序 addsd 链（17.39+13.98%，阶段 2 目标）。divpd 收益实测成立
  （叠加 floordiv 展开消除 + spill 削减），未做吞吐假设。

### 闸门

- flag-off asm 对 0.331 基线**逐字节不变**：spectral_norm/nbody/matmul + 全部 38
  热基准（realistic 6 + compare 9 + daily 23）asm cmp 全同 → flag-off 零回归。
- difftest（O0/O2，flag-on）270/0；全套件 normal flag-off/on EXIT=0（290 ran ok）；
  **ASan JOBS=1 flag-off/on EXIT=0（290 ran ok）**；283/284 normal+ASan flag-on/off
  均 ran ok；check_size 无新增超限（codegen_simd.cpp 325 行、codegen_impl.h 拆片段后
  1198 行）。
- ASan JOBS=8 高并行偶发环境性 flake（网络/GC/C 编译资源竞争，失败用例每轮不同，
  基线同样偶发，cfi_unwind ASan standalone 对基线也 rc=1）；JOBS=1 干净 EXIT=0，
  失败用例 standalone 输出 mine==base。flag-off asm 与基线逐字节相同（二进制差异
  仅来自不同 build 目录的 rt_asan.o，与 codegen 无关）。

## [0.332] - 2026-11-20

> slim 分支合入：**单二进制瘦身** —— 运行时 -ffunction-sections/-fdata-sections +
> 链接期 --gc-sections 剥掉用户程序未用到的运行时函数/数据。**零功能回归为最高
> 不变量**；env 门控 `PYADAMAS_GC_SECTIONS=0/1`（**默认开**，闸门全过；=0 显式关）。
> **生成 asm 逐字节不变**（codegen 未触碰，1430 语料闸门钉死）。
> 接 cgspeed 0.331 之后集成，定版 0.332（simd 顺延 0.333）。

### 变更

- **CMakeLists**：运行时编译（普通 -O2 与 ASan -O1 两版）加
  `-ffunction-sections -fdata-sections`。ld -r 合并后段粒度保留（rt.o 1893 个
  .text.*、rt_asan.o 2063 个，已验证）。不带 --gc-sections 时链接器照旧并入
  .text/.data，功能零变化（布局因 hot/unlikely 分组与段对齐有位移，非逐字节同
  0.331；direct-ld==cc 不变量保持）。
- **driver_link / driver_build**：`PYADAMAS_GC_SECTIONS=1` 时两条链接路径等价
  附加 --gc-sections（direct-ld 前缀 / cc 回退 -Wl,），重建「direct-ld==cc 逐字节」
  不变量（12 代表程序 cmp 全同）。ASan 走 cc 回退同样带 gc。只认精确 "1"。
- **from c import 安全回退**：dlsym(RTLD_DEFAULT) 的符号名是运行期字符串，静态
  可达性覆盖不到（可能查找任何 pa_* 运行时符号）。新增
  `ir_module_uses_c_sym_default`（扫 IRExtern.lazy==1）→ ModuleUnit/Program 聚合
  标志 → 写进模块缓存清单（csymdefault，旧清单保守按用到处理）→ 这类程序整程序
  关 gc（212_c_lazy 尺寸不变、行为不变）。dylib 句柄路径（lazy==2）不受影响。
- **保留审计**（--gc-sections 误剥防线，逐项取证）：
  - .init_array/.fini_array 构造器：ld 默认脚本 KEEP，pa_eh_install_personality_hook
    实测保留（hello-gc nm 可见），钩子安装的 pa_eh_phase2_frame_release 随之可达；
  - personality：生成 asm 的 DW.ref.pa_personality 槽在整体 .data 里随模块保留，
    .quad 重定位拉住 pa_personality（exc_loop-gc nm 可见 T pa_personality）；
    .eh_frame_hdr/.eh_frame/.gcc_except_table 齐全，未捕获异常 traceback 与
    gc-off 逐字一致；
  - atexit 回调（diag_report/gc_stats_dump/rc_stats_dump/leaf_index_shutdown 等）
    经注册点直接引用保留；PYADAMAS_RC_TRACE/GC_STATS/RC_STATS 在剥离后的 hello
    上实测正常输出；
  - GC 根表 gc_globals/pa_gc_add_roots/gc_mark/gc_phase_dec 经调用链可达保留；
    13_gc/174 flag-on（GC_BARRIER=1 + GC_INCR_MARK=1）normal+ASan 全过；
  - 运行时内部函数指针表（AVX2 分派等）都是直接调用/重定位，可达性传递保留；
  - 运行时无 weak 符号、无自定义段、无 TLS；diag 大 BSS 表经无条件调用点保留
    （BSS 是虚拟空间，不计文件成本，本轮不动）。

### 尺寸（同一编译器 A/B，-O2，gc off→on；文件字节）

| 程序 | off | on | 缩减 |
|---|---|---|---|
| hello（print） | 481912 | 88072 | **−81.7%** |
| fib | 482128 | 88328 | −81.7% |
| nbody | 503184 | 114256 | −77.3% |
| queue_sim | 486408 | 97576 | −79.9% |
| dict_wordfreq | 491080 | 119792 | −75.6% |
| json_bench | 499152 | 140512 | −71.9% |
| bench_re | 570248 | 229280 | −59.8% |
| dict_cache（192） | 499048 | 119144 | −76.1% |
| 13_gc | 499696 | 111400 | −77.7% |
| exc_loop（173） | 491000 | 115872 | −76.4% |
| c_lazy（212，from c import） | 486520 | 486520 | 0（安全回退；off 口径含块1对齐 +4096） |
| cpy_json（208） | 490256 | 128144 | −73.9% |

hello 的 pa_* 文本符号 668 → 58（−91%）；cj_parse/cpy_decode/pa_subprocess_run/
g_rng/pa_str_method/pa_percent_format 等未用运行时全部剥离。text 段
409541 → 58906 B。BSS（GC arena 虚拟空间）基本不变。

### 闸门（PYADAMAS_GC_SECTIONS=1）

- difftest **270/0**；全量套件 normal **288 ran ok / EXIT=0**、ASan **288 ran ok /
  EXIT=0**（PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 钉子 6/6 全 0（13_gc/174/88/192/66/119）；13/174 flag-on
  （PYADAMAS_GC_BARRIER=1 + PYADAMAS_GC_INCR_MARK=1）normal+ASan 4/4 过。
- 语料零差异：tests/run + bench/daily（292 程序）三路对拍（0.271 oracle vs gc-off
  vs gc-on）：**GCDIFF=0**（gc 引入零差异）；ORACLEDIFF 66 全为既有差异（49 个
  oracle 编不了 0.271 之后的新用例 rc 1vs0；14 个输出差异经 gc-off 复核同样存在，
  系 0.271 之后有意的语义变更：tuple-sort/float-dict/pep479/fnmatch/Neumaier 等）。
- asm 语料 **1430/1430 逐字节 IDENTICAL**（O0/O1/O2/win64/no-zce × bench+tests/run，
  vs 改前基线）——codegen 零变化。
- direct-ld==cc（带 gc）12 代表程序 cmp 全同；gc-off 与块1产物逐字节一致。
- check_size 无新增超限（仍为基线既有 5 处：codegen_regalloc/driver_build/ir_ops/
  lower_impl/typecheck_types，本轮 driver_build +22 行不新增超限文件）。
- opt_levels（O0/O1/O2 + fast-math）EXIT=0；cache_race ok（gc-on）。
- **abbench 无性能回归**（05680b7 基线 vs gc-on，38 案＝compare 9 + realistic 6 +
  daily 23，交替 9 轮打乱 BURST=5 绑核 24）：总计 **−2.6%**（2162→2106ms）。
  长案（>100ms，最可信）：sort_struct −7.8% / rec_memo −3.6% / dict_wordfreq −3.2% /
  str_build −1.8% / obj_dispatch −0.2% / nqueens +0.2% / queue_sim +1.1%。
  数值案：nbody +1.9% / mandelbrot +2.1% / fib +1.5% / fasta +2.1% / matmul +0.5% /
  spectral −0.4% / fannkuch −2.8% / binary_trees −1.5%（p25 口径 nbody +2.1%、
  mandelbrot +0.1%、fib −1.4%、fasta +0.7%）；短 daily 案 ±4.6% 内噪声主导。
  边界案 15 轮复测（交替打乱 BURST=5 绑核）全部落回噪声级或转负：nbody +0.6%
  （p25 +0.7%）/ queue_sim +1.0% / mandelbrot −1.3% / fib −1.0% / fasta −1.6% /
  c2_inventory −2.1%（首轮 +4.4% 系短案噪声）。**结论：无性能回归**。
  difftest gc-off 终版编译器复跑 270/0。

### 分支与集成建议

- 分支 **slim**，基于 05680b7（0.331）；不推 master，主树与主树 build 未触碰。
- 默认开与否由 lead 集成定：门控 PYADAMAS_GC_SECTIONS=1 已全闸门通过；默认开只需
  把 gc_sections_enabled() 的默认值翻成 on（或集成时改 CMake 探测注入）。
- 生成码按函数分段（未用用户函数也可剥）是可选第二步，本轮未做（运行时是大头，
  且会改 asm 文本、动 1430 闸门，收益/风险比低）。
- 已知边界：from c import 程序不瘦身（安全回退）；flag-off 二进制相对 0.331 有
  布局位移（段分组/对齐），功能与输出零变化。

## [0.331] - 2026-11-20

> cgspeed 分支合入：codegen 汇编发射路径提速（纯内部优化，**生成 asm 逐字节
> 不变**为最高不变量）。参照 LLVM/GCC：操作数直建结构化、静态寄存器名表、
> 直写缓冲、内联存储，消除「拼字符串→回解析」往返 + std::format + 分配
> 抖动。接 gc1 0.330 之后集成，定版 0.331。

### 性能

- **机器层**（mach_instr.h/mach_emit.cpp）：MReg 编号 + 编译期静态寄存器
  名表（packed-key 二分查找）；MOperand 152B→56B（base/index 编号化、
  canonical text、has_sym 标志）；MOpVec 两操作数内联 + 溢出 vector，
  两操作数指令零堆分配；MachFunc 指令侧池，MItem 瘦成 40B 句柄；
  mop_name O(1)；render 走结构化字段 + to_chars 直写。
- **静态字面量行缓存**：line(const char*) 重载按内容缓存解析结果，
  RC 内联序列等约 700 处固定指令模板进程内只解析一次。
- **操作数缓存结构化**：vreg/槽/别名/常量缓存全部 MOperand 单元，
  定义点解析一次、使用点直发；src_mop/dst_mop/slot_mop 等结构化访问器；
  emit2/emit1/emit0/emit_imm/emit_imm_hex(MOp,…) 直发重载 + 预建寄存器
  操作数表；op_mem 编号版工厂（含变址/比例/位移）。
- **热路径迁移**：bin/cmp/convert/un/truth、index load/store、field
  load/store、vcall/call_direct/call_ptr、tuple/append/str-index 内联、
  RC inc/dec/temp/take、实参装填 emit_arg、序言/尾声 CFI、split
  reload/spill、全局访问（模块级 rip 相对操作数缓存）。
- **std::format 迁移**：标签工厂（fresh_label/func_label/shim/ptag/
  dbg/num_label）、CFI 数字行、数据段（字面量/全局/vtable/闭包/调试名）
  全部 to_chars 直拼 + 复用缓冲。
- 历史发射怪癖逐字节保留：jo/jc/jz 与含 '$' mangled 符号操作数的文本
  兜底是列 0 Raw（无 tab），结构化路径显式复刻（1444 语料闸门钉住）。

### 测量（多租户，taskset 24-27 绑核，交替轮转取中位）

- asm 语料 **1544/1544 逐字节 IDENTICAL**（bench/realistic+compare+
  tests/run 全量 × O0/O1/O2/win64/no-zce/RA_SPLIT/RA_GRAPH + fast-math/
  NO_RA/NO_FPLIVE/POOLCAP 变体，vs 0.329 oracle）。
- perf self%：parse_operand 1.23→0.59、parse_instr 0.73→跌出前 22、
  std::vformat 1.82→1.31（剩余主要是 IR 注释文本 in.text()）。
- codegen 相位（PYADAMAS_TIMING，14 bench 合计）：各窗口 +8~19%，
  最近 10 轮窗口 58.4→47.5ms（**+18.8%**）；逐文件 13/15 提升 3~25%
  （dict_wordfreq +25%、nqueens +21%、fib +20%、nbody +18%）。
- 整程序编译墙钟：最近 10 轮窗口 0.156→0.146s（**+6.3%**，9/10 轮胜）；
  空闲轮次 0.077→0.064s（+17%）；高负载窗口中位持平（进程启动与
  非 codegen 相位主导微基准语料，负载噪声 ±8%）。
- 闸门：difftest 265 用例 0 不一致；全量套件 normal+ASan EXIT=0
  （283 项 ran ok）；CPython 对拍 840 程序 0 失败；check_size 无新增
  超限（codegen_emit.h 拆分承接发射器）。

## [0.330] - 2026-11-20

> **并发循环 GC 阶段 1（gc1 分支合入，定版 0.330；cgspeed 顺延 0.331）**：
> 保留 RC + 确定性析构，补上作者退守的两块——**Yuasa 删除屏障 + M=1 增量标记**，
> 修好历史撞墙用例 **13_gc（UAF）/ 174_gc_construct_roots（漏 21KB）**。**不开后台线程**
> （后台 marker 是 C2/阶段 2）。设计硬文档（屏障覆盖审计表 / rc·marked 正确性论证 /
> 门控方案 / free 路径审计 / 基线 bug 记录）见 **docs/gc-concurrent-mark-design.md**。
>
> 双门控，**flag-off 零变化（最高不变量）**：
> - 运行时 `PYADAMAS_GC_INCR_MARK=0/1`（默认 0=现状同步标记）；
> - 编译期 `PYADAMAS_GC_BARRIER=0/1`（默认 0=不发射屏障 IR，生成 asm 逐字不变）。

### 运行时（runtime/rt_core.c 等）

- **Yuasa 删除屏障（shade-on-delete）**：`gc_phase_dec` 的 INCR_MARK 分支按
  `gc_incr_mark_on` 门控——标记期 RC 归零对象先 `gc_mark` 标灰入队（shade）保活、
  置 `PYADAMAS_RC_DEFER` 位去重、登记 `gc_pend`，**rc 保持 1** 到本轮标记排空 +
  收尾后再释放（灭 UAF + 灭漏释放）。rc>1 的 dec 正常减计数。legacy（flag-off）原样保留。
- **增量标记接线**：major 根扫描（一次性原子）后 `gc_incr_state=1` / `gc_phase=INCR_MARK`，
  `gc_incr_step` 按 `GC_MARK_BUDGET=64` 分片标记（分配点驱动）；**标记排空后同步
  release+清扫+finish**（M=1 清扫原子，避免切片间 mutator 误释活对象——实测增量清扫会漏
  8816B）。清扫与 mutator 交错的进一步分片留待阶段 2/3。
- **Dijkstra 插入屏障**：`pa_list_append/set`、`pa_dict_set`、`pa_chan_send` 入口
  `pa_gc_insert_barrier(v)`（`pa_gc_incr_mark_fast` 门控，flag-off 一次可预测分支跳过；
  标量查集合表落空 no-op）。`pa_gc_incr_mark_fast` 公开别名。
- **allocate-black 口径不变**：新对象不进 gc_set 快照、不打 MARK 位、靠 RC；插入屏障保护
  新对象引用的老对象。

### 编译器（src/）

- `RtFn::GcWriteBarrier` → `pa_gc_write_barrier`；`Lowerer::gc_barrier` 在
  `store_field` 堆/unknown 字段写后发射，编译期 `PYADAMAS_GC_BARRIER=1` 门控（默认关）。

### 测试

- 新增 **tests/run/278-282**：cycle churn / a.next=b 中途写+新引用老 / 深析构级联 /
  生成器帧槽写 / 10k VT 挂起栈扫描压力。先 CPython 对拍钉 expect，O0/O1/O2 flag-on/off
  全一致，ASan detect_leaks=1 全 0。

### 闸门

- 13_gc / 174 flag-ON normal + ASan detect_leaks=1 全过；全套 normal/ASan flag-off/on
  EXIT=0；difftest 270/0；leak 钉子 11/11 全 0；**26 热基准 asm 26/26 IDENTICAL**（默认编译
  vs lead 0.329）；check_size 无新增超限。
- abbench（flag-on vs off）：bench_rc_gen −0.1% / 13_gc +0.6% / queue_sim −0.4%（噪声级）；
  极端合成 cycle-churn 278 约 +3.6%（标记期屏障开销，文档化代价）。**标记停顿下降可测**：
  174 同步 gc_drain 标记停顿 1.89/8.47/**32.09ms** → flag-on 根扫描 0.00ms + 64 对象分片
  （32ms 同步标记停顿消除）。

### 记录在案（独立基线 bug，flag-off 不改，移交）

- flag-off 大集合增量清扫 `gc_incr_step` state-2 只 `gc_free_body` 不调 release →
  垃圾容器→活叶子边漏 dec。取证：old-gen 环垃圾引用新串用例 flag-off ASan 漏 7.2MB/59998
  allocs；flag-on（同步清扫补 release）全 0。13/174 flag-off 不触发此路径。

## [0.329] - 2026-11-20

> stdlib2 分支合入：标准库拓宽第二批（需求驱动，**轻量标尺**）——**zipfile /
> shutil / tempfile / urllib.request（系统 libcurl 原生）**。接 p3next
> 0.328 之后集成，定版 0.329。
>
> 轻量标尺（用户明确纠正一批的过度对齐）：同参数同功能同结果即可——
> 错误类型对、消息合理不逐字；格式合法（zip 能读能解压、HTTP 响应正确）
> 不字节级；差分只钉功能输出。选型理由/API 覆盖/差异/桥接兜底/已知问题见
> **docs/stdlib-batch2.md**。
>
> 未做（记录在案）：pathlib 补全（原生类方法要动 builtin_sigs/lower，
> NativePython 扩展加不了类方法）/ io.StringIO（csv 等收 f: file 静态类型，
> 互通要 file 协议泛化）→ 后续批次；decimal/fractions 深化（BigInt）/
> sqlite3 维持一批评估结论。

### 模块交付（stdlib/，模块表登记）

- **zipfile**：ZipFile r/w/a/x（namelist/infolist/getinfo/read/write/writestr/
  extract/extractall 路径净化/testzip/open(r/w)/comment/上下文）、ZipInfo
  常用字段面 + from_file、is_zipfile、BadZipFile/LargeZipFile。stored/deflated
  复用一批 zlib 胶水；读 CPython 流式 data-descriptor 产物、写 CPython 可读
  产物（互操作 fixture 内嵌测试）。整读整写；Zip64/bzip2/lzma/加密文档化拒绝。
- **shutil**：copyfile/copy/copy2/copymode/copystat（权限位+atime/mtime 纳秒）/
  move（跨设备回退）/rmtree（符号链接安全）/copytree（symlinks/ignore/
  dirs_exist_ok）/ignore_patterns/which + Error/SameFileError/SpecialFileError。
  stat/utime/symlink 走小 C 胶水（错误归一化负码）。
- **tempfile**：mkstemp（O_EXCL 0600 胶水 + 随机名重试）/mkdtemp/
  NamedTemporaryFile（file 全方法面代理，delete 语义）/TemporaryDirectory
  （cleanup 幂等）/gettempdir（候选探测+缓存）/gettempprefix/tempdir。
- **urllib.request**（lead 追加，libcurl 8.18 原生）：urlopen(url|Request,
  data, timeout, headers, method) → Response（status/read/readline/headers/
  geturl/getcode/info/上下文）；Request/HTTPMessage（大小写不敏感）；
  URLError(OSError)/HTTPError(URLError 带响应面)；GET/POST/自定义头/超时/
  重定向/HTTPS（系统 CA 默认验证）。C 胶水封装 easy API 全流程，缓冲所有权
  交 Python；超时按连接阶段区分 URLError/TimeoutError（同 CPython）。
  链接顺序 glue 在前 -lcurl 在后（Ubuntu --as-needed）。

### 前端/运行时使能（最小修复，均基线问题）

- **E1** open() errno → FileNotFoundError/FileExistsError/PermissionError/
  NotADirectoryError/IsADirectoryError（rt_file.c，此前一律 OSError）。
- **E2** os.close(fd) + os.unlink 别名（RtFn **OsClose 枚举尾部**）。
- **E3** os.makedirs exist_ok 关键字此前被降级静默丢弃（lower_import_native.cpp）。
- **E4** os.path.islink（RtFn **OsPathIslink 枚举尾部**）。
- **F1** cmp+branch 融合排除 Bytes（同 Str）：bytes 切片 eq/ne 在循环条件里
  被融合成指针比较恒不等（基线 0.327 复现，顶层"碰巧对"是常量折叠成同一
  驻留字面量）——can_fuse_branch 一行修复。

### 已知问题（基线已验证，绕行记录在 docs/stdlib-batch2.md §六）

嵌套 try/finally mark 夹层抛异常 → 临时栈 mark 校验失败（copyfile 单层化绕行）；
复杂帧 cimport ptr 装填错（urllib._perform 独立小帧绕行）；cast 到容器类型
IndexError（直接标注赋值绕行）；类实例 __call__ unknown 间接调用 SEGV
（ignore_patterns 闭包化绕行）。均 **pristine 1501040 重建复现**（主树
release 二进制是旧物位且主树已进 0.328，不能当 0.327 基线），非本批引入，移交。

leaf×unknown RC 泄漏（一批移交）：模块内 object→容器/字符串改直接标注赋值
（j14 口径，实测 cast 漏标注不漏），测试侧二进制助手改文本口径；剩余小量
（118B–9.4KB/用例）属一批 _gzipio cast + 基线容器临时洞，记录不硬修，
p3next 0.328 集成后预期收敛。

### 验证

- tests/run/270–277（8 个）：先对拍 CPython 3.14 再钉 expect，O0/O1/O2
  逐字节一致（271 含 2 行文档化 NI 边界手工钉入）。网络用例自起本地
  http.server 子进程（18326/18327），不打外网。
- 普通套件 **278 ran ok EXIT=0**（PYADAMAS_TEST_JOBS=8）。
- ASan 套件 **278 ran ok EXIT=0**（PYADAMAS_TEST_JOBS=8，detect_leaks=0 口径）。
- difftest（O0/O2 自差分）：**260 用例 0 不一致**。
- ASan detect_leaks=1 抽查 270–277（--asan -O2）：**274/275 CLEAN**；
  270 9379B/29obj、271 3939B/22obj、272 256B/2obj、273 118B/3obj、
  276 712B/6obj、277 120B/1obj——均基线 leaf×unknown/容器临时 RC 机制
  （pristine 1501040 重建同复现），绕行与分类见 docs/stdlib-batch2.md §六.5；
  测试侧 cast 中转已清（272 由 600987B 降至 256B），模块内 j14 标注赋值已落。
- check_size：**无新增超限**（既有 5 处不变：codegen_regalloc/driver_build/
  ir_ops.h/lower_impl.h/typecheck_types.h；driver_build +6 行模块登记、
  ir_ops.h +6 行 RtFn 尾部追加，均既有超限文件最小增量）。

## [0.328] - 2026-11-20

> p3next 分支合入：unknown 表示层 **P3-1b**（接续 0.327 P3 一期）。先于
> stdlib2 集成，定版 0.328（stdlib2 顺延 0.329）。落地记录
> docs/p3-1b-landing.md；设计总纲 docs/p3-tagged-union-design.md §7 已更新。
>
> 判据延续一期：静态已知路径 asm 逐字节不变（26 基准 **26/26 全同**，每块
> 增量后 cmp）；门控关模块零影响；UNK 回退探针保正确性（不依赖传播完备）；
> 异构对拍 CPython；ASan leak 干净；check_size 无新增超限。

### P3-1b 实现（四块增量，逐块构建验证）

- **tag 过直调形参/返回（旁路双字 ABI + 双入口 shim）**：payload 的 classify
  落位逐字不变（值 ABI 零变化）；unknown 形参 tag 经平行数组 + %r11 指针通道
  （数组在出参区栈实参之后，outgoing_bytes 扩容），call 被调 .LfN$pt 入口；
  旧入口（闭包/vtable/间接/跨模块全部既有引用）先清零 %r11 再进同一函数体，
  形参 tag 全 UNK 回退。unknown 返回经 %rdx 带 tag（旧调用方忽略 rdx，无兼容
  问题）。资格按形参**槽类型**（槽升级会让 object 形参槽定型，argparse
  add_argument 实测）。被调内 RC/AnyArithT 按形参 tag 免探针，调用方按返回
  tag 免探针。
- **dict 值/类字段一次探针导 tag**：DictGet/GetOr/GetOpt/Pop/PopLast、
  FieldLoad、DictTryGet 融合直写槽——装载发一次 pa_any_load_tag，后续 RC/
  算术免探针（容器/对象布局零变化）。
- **生成器 resume vreg tag**：一期整函数禁 tag 的两根因修复（tag 清零挪到
  g.entry 块头，每次 resume 都走；GenSent 写 UNK）。unknown vreg 启用 tag；
  堆帧命名槽仍无 tag（装载 UNK 回退，列剩余）。
- **send/throw 值装箱**：float 裸位模式进 gen.sent 被当整数（基线已存在）→
  coerce 到 unknown。

### 基线 leak/UAF 根治（stdlib-batch1 §四半移交 + 本轮新发现）

- **unknown 调用结果所有权洞**（j12 str 104B / m5 盒 72B / print(object 调用)
  144B/2obj）：def_heap 只给静态堆类型结果登记临时栈，unknown 调用结果那份
  调用方所有的引用直接消费时无人释放 → 对 Call/CallDirect/VCall* 的 unknown
  结果补 push_temp_unknown（只限用户调用 op，借用结果不登记）。
- **leaf×unknown RC 探针对称**：inc_unknown/temp_unknown 改用与 dec_unknown
  逐位一致的 pa_unknown_is_heap（含 exact-RC 回退）；常规模式行为逐字不变。
- **object 槽升级 RC 洞**（新发现，UAF 链）：显式 object/Any 标注槽禁止
  unknown→typed 升级（按标注 AST 精确判定，不用 sym.annotated+unknown——
  typecheck 对循环内 j: int 也给该组合，误标会改 3 个基准 asm）；兜底
  de-upgrade（unknown/堆值流入已升级非堆槽 → 回退 unknown 走 retain/释放）。
- **yield unknown 消费份额**（配套）：next/__next__/send/throw/for-gen/
  list(gen)/genfor 七个消费点补 push_temp_unknown / RcDecUnknown（基线靠
  「槽升级裸存→盒提前释放→池复用」UAF 链掩盖，槽修复后暴露为干净泄漏）。

### 性能（绑核 24，交替 7 轮中位；同一运行时，仅生成代码不同）

| 用例 | 0.327 | 本分支 | 变化 |
|---|---|---|---|
| u4_call_tag（新，40 万轮 object 调用累加） | 73.3 ms | 48.3 ms | **−34.2%** |
| u5_dict_multiuse（新，40 万轮小 dict 多使用） | 67.0 ms | 58.4 ms | **−12.9%** |
| u1_hetero_list | 48.5 ms | 48.9 ms | +0.8%（噪声级，11 轮） |
| u3_unknown_arith | 35.5 ms | 35.6 ms | +0.0%（噪声级，11 轮） |
| u2_hetero_dict | 135.8 ms | 136.2 ms | +0.3%（噪声级） |

u1/u3 无用户调用、u2 被字符串拼接/大表哈希主导——新收益面在调用边界与
dict 多使用，符合设计预期；一期成果（u1 −38% / u3 −44.7% vs 0.325）保持。

### P3-2/P3-3 评估（本轮不做，记录路径）

- **P3-2（FBOX→FLOAT 内联）**：新事实——GC 保守扫描用 gc_tab_find 查表标记，
  不对 unknown 字解引用，裸 double 位模式天然安全；EH 释放走探针/tag 跳过也
  安全。真正代价：FRAW payload 不再是合法旧 unknown i64，约 20 处无 tag 消费
  边界必须先装箱（漏一处=静默值损坏，正确性前提从「探针回退」变成「边界装箱
  完备」）；收益只剩 72B/盒内存+一次解盒间接（churn 已被一期复用消除）。
  风险/收益不划算 → 下一轮先落边界装箱审计表再灰度（landing §6）。
- **P3-3（跨模块 ABI 版本化 + None/bool 盒）**：跨模块 tag ABI 需接口版本
  bump + 导出 $pt 符号 + 门控协调，收益面小；None/bool 盒是语义工程。长期。

### 验证

- tests/run/262–266（新增 5 个）：调用结果所有权形态、直调 tag（8 形参栈实参
  共存/混合类型/递归/嵌套/函数值间接回退）、dict/字段 tag（含 miss 默认值/
  pop/覆写旧值 tag 保持/DictTryGet）、object 槽升级（累加器 return/形参重赋值/
  生成器/类字段/闭包）、门控生成器 vreg tag + send 装箱。全部 CPython 对拍 +
  O0/O1/O2 + ASan detect_leaks=1 干净。
- 26 热基准 asm 逐字节 cmp：每块增量后 26/26 IDENTICAL。
- difftest 257 用例 0 不一致；普通套件 + ASan 套件 ran ok EXIT=0
  （PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 抽查：247/248/13_gc/231/262/263/264/265/266 全 0；
  j12/m5/pf3/pf5 leak 复现族全 0。
- 生成器回归：118/119/122/123/124/128/129/131/133/136 全过（一期 SIGSEGV
  修复保持）。
- check_size：无新增超限（发射集中 codegen_tag.cpp 382 行；codegen_call.cpp
  1199 / codegen_block.cpp 1200 / lower_stmt.cpp 1200；既有 5 处超限文件不变）。

### 已知边界（列剩余，landing §8）

1. 生成器堆帧命名槽无平行 tag（rt_gen.c 帧布局 + yield/resume 存还原）。
2. P3-2 边界装箱审计表 + FRAW 灰度。
3. 跨模块 tag ABI 版本化。
4. 独立 bug 移交：dict 值里 list[unknown] 的 print repr 把 boxed float 按整数
   打（基线已存在）；253 fallback 32B 异常对象泄漏（stdlib-batch1 移交）。
5. 复用匹配仍限同块直接形态（一期 §9.4 维持）。

## [0.327] - 2026-11-20

> p3union 分支：unknown 表示层 **P3 封闭 tagged union 第一阶段**（p3union 分支合入）。
> 设计文档 docs/p3-tagged-union-design.md：选型 B「平行 shadow tag 跟值走」
> +「float 盒原位复用」混合，按模块门控（封闭活跃域）。不推 master、不动主树。
>
> 判据：静态已知路径 asm 逐字节不变（26 基准 **26/26 全同**，门控按构造
> 保证）；不撞 kind2 内联 1-bit heap_bits（位图格式/bts/btr 逐字不动）；
> 0.321 字符串 RC 成果保持；tag 丢失一律回退探针，正确性不依赖传播完备。

### 设计选型（docs/p3-tagged-union-design.md）

- **A 加宽槽 {i8 tag,8B payload}→16B**：float 内联免盒最干净，但值宽 ABI
  全量重构（形参/返回/槽/容器/EH/GC/生成器帧），一轮做不完且半做态不可
  验证；其收益主体（免探针+免 churn）B 已覆盖，增量只剩 72B/盒内存与一次
  解盒间接 → 列第二阶段（P3-2，在 B 的 tag 传播地基上把 FBOX 换内联 FLOAT）。
- **B 平行 shadow tag 跟值走（本阶段采用）**：值仍是 8B i64（float 仍装箱），
  平行 tag 字（UNK0/INT1/FRAW2/FBOX3/STR4/HEAP5，堆⇔>=3）随值传播到标量
  槽/全局/vreg/容器边界；RC 与动态算子按 tag 免探针，UNK 回退旧路径。
  解决 P2 指出的「取出落标量槽 tag 丢失」：tag 跟值走，不跟容器。
- **C 指针低位 tag**：解决不了 int/float 判别（信息论边界），指针掩码面与
  约束①冲突 → 否决（0.324 §4.5 同结论）。
- **float 盒原位复用**：acc = acc + x 形态，codegen 同块匹配「load S →
  arith → store S」+ 单使用守卫，运行时验 tag==FBOX && rc==1（唯一所有权）
  才原位改写并 rc++（引用计数证明见设计 §4）；常量缓存盒/共享盒 rc>=2
  一律不复用。消 u3 每轮新盒分配/回收 churn。
- **门控（封闭/开放分治）**：模块含 unknown 动态算术/比较才启用 tag；
  26 热基准全部不含 pa_any_* → 门控关 → 不进任何 tag 分支（asm 按构造
  逐字节不变）。开放 unknown（无动态算术的模块/容器无位图/字段/形参/
  生成器）= UNK 探针+盒兜底，与 codon「封闭 union + PyObject 桥」同构。

### 实现

- runtime/rt_any.c：pa_any_arith_t/neg_t（{rax=payload,rdx=tag} 双字返回 +
  原位复用）、pa_any_cmp_t/is_float_t/to_f64_t/to_i64_t、pa_any_load_tag
  （i64→tag 一次探针）；旧入口全保留。rt_internal.h 声明 + PaAnyPair。
- src/codegen_tag.cpp（新文件，从 codegen_call_rt.cpp 拆出守 1200 行限制）：
  tag 版算子装填/双字落盘、复用匹配器、unknown RC 三入口 tag 内联
  （>=3 直接 inc/dec/temp，1/2 跳过，0 回退）、heap 标志从 tag 导出
  （append/make_list/index_store 传 0/1 保住 kind2 位图）、下标装载按
  elem_kind/位图导 tag（无位图一次探针）。
- codegen：模块门控扫描、帧内平行 tag 区（unknown vreg/槽专属字，序言
  清零）、unknown 全局平行 tag 符号（健全性规则：外部全局与非入口导出
  全局一律 UNK——防跨模块写坏 tag；入口 __entry__ 不可被 import 豁免）、
  Mov/Load/Store/Const/Global tag 搬运、unknown 禁别名/常量折叠/融合、
  DictTryGet 直写 unknown 槽清 tag、生成器 resume 整函数无 tag（序言清零
  不走常规路径，垃圾 tag 会误 dec——118/124/128/133 实测修复）。
- 不动：容器布局/位图格式/内联 bts/btr、临时栈、EH 帧记录与展开、GC 扫描、
  生成器堆帧、类字段、闭包统一约定、cimport/C ABI、降级器（仍发旧 Any
  入口，codegen 门控内拦截改发 tag 版）、推断/单态化。

### 性能（绑核 24，交替 7 轮中位；同一运行时，仅生成代码不同）

| 用例 | 基线 0.325 | 本分支 | 变化 |
|---|---|---|---|
| u1_hetero_list | 71 ms | 44 ms | **−38.0%** |
| u3_unknown_arith | 57 ms | 31.5 ms | **−44.7%** |

- u1：标量槽探针（inc/dec/temp）+ 算术解码探针消除，累加器盒复用，
  append 堆标志从 tag 导出保住位图 → 元素装载免探针。
- u3：探针 ~25% + 盒 churn ~24% 两大成本结构性消除（P2 §1 剖析的目标）。
- 26 热基准 asm 逐字节 cmp：**26/26 IDENTICAL**（含 nbody/spectral/matmul
  数值循环）。门控关模块逐字旧路径。

### 验证

- tests/run/248_p3_shadow_tag.py（新增）：复用命中/安全边界（旧值别名、
  共享盒、acc+acc、常量缓存盒）、异构遍历+isinstance、tag 回退（dict/
  形参/字段/生成器）、一元负/混合算术/比较/str 拼接、下标写入+冷路径；
  O0/O1/O2 与 CPython 逐字一致。
- difftest 240 用例 0 不一致；普通套件 258 ran ok EXIT=0；ASan 套件
  258 ran ok EXIT=0（PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 抽查 247/248/13_gc/231：无泄漏。
- check_size：无新增超限（P3 发射拆 codegen_tag.cpp 276 行，
  codegen_call_rt.cpp 回落 1148 行；既有 5 处不变）。

### 已知边界（本阶段明确不做，列后续）

1. tag 不过调用边界（形参/返回 UNK 回退）——P3-1b。
2. 生成器堆帧/类字段/dict 值无 tag（UNK 回退，性能持平）——P3-1b。
3. float 仍装箱（72B/盒），复用只消 churn 不消内存——P3-2 内联。
4. 复用只匹配同块「load S → arith → store S」直接形态 + 单使用守卫；
   Mov 链/跨块/多使用不匹配（正确，只是不复用）。
5. 统计模式复用路径少计一次 alloc/free；GC-sweep 期禁用复用。
6. 多态生成器 yield（float+int+str 混合）是 0.325 基线已存在的边界
   （基线/本分支输出逐字相同，248 注释标注），不在本阶段范围。
7. None/bool 仍裸 0/1（0.324 边界不变）。
8. 直接 print(object 函数调用) 形态命中 0.325 基线已存在的 unknown 调用
   结果所有权洞（inline/indirect 两形态基线/本分支均泄漏 144B/2obj，IR
   逐字相同）；结果先落 object 槽再打印则无泄漏（247/248 采用形态）。
   非本阶段引入，列后续修复。

### 后续阶段

- P3-1b：tag 过直调形参/返回（双字 ABI + 闭包 shim 适配）；DictGet/字段/
  生成器帧 tag 化。
- P3-2：A 内联——封闭域 FBOX→FLOAT 内联（16B 仅 unknown 槽/vreg，容器仍
  8B+边界装箱）；容器元素域 tag 由本阶段位图+tag 自然导出。
- P3-3：跨模块 ABI 版本化；None/bool 盒；开放 unknown 的 PyObject 式通用盒。

## [0.326] - 2026-11-20

> stdlib1 分支：标准库拓宽第一批（需求驱动，日常脚本刚需）——**argparse /
> configparser / gzip（+zlib 支撑层）**。与 P2（容器表示层）/P3（rt_any.c）
> 并行开发，**不改主树、不推 master**；集成时编号按落地顺序重排（stdlib1 分支合入）。方法：差分基建先行，每个 API 对拍 CPython 3.14（正常/边界/
> 错误路径），错误路径（argparse 退出码 2 + usage）用自子进程对拍。
> 选型理由/API 覆盖/差异/桥接兜底清单见 **docs/stdlib-batch1.md**。
>
> 未做（评估记录在案）：decimal/fractions 深化需任意精度 BigInt（深，另票）；
> sqlite3 需外部库 + 完整游标/事务语义（cimport 路径已验证可行，性价比低）；
> zipfile/pathlib 补全后续批次。

### 模块交付（stdlib/，模块表登记）

- **argparse.py**：ArgumentParser/Namespace/ArgumentTypeError/ArgumentError。
  actions（store/store_true/store_false/store_const/append/append_const/
  count/help）、nargs（None/?/*/+/N）、type/choices/default/required/metavar/
  dest、位置参数、--long=value、短选项 bundling（-abc/-fval/-f=val）、-- 分隔、
  唯一前缀缩写、parse_args/parse_known_args、usage/help 排版（默认
  HelpFormatter 口径：帮助列 min(最长调用+4,24)、宽度 78、usage 超长折行同
  CPython get_lines）、error/exit。错误消息逐字对齐（expected one argument /
  invalid int value / invalid choice / ambiguous option / unrecognized
  arguments / required / conflicting option string 等）。
- **configparser.py**：ConfigParser + 11 个异常类。read/read_string/read_file、
  sections/options/has_*/get/getint/getfloat/getboolean（raw/fallback/vars）、
  items/set/add_section/remove_*/defaults/write、optionxform 可覆盖、DEFAULT、
  续行/空行/整行注释、BasicInterpolation %(key)s（%% 转义/10 层深度/
  before_set 校验）。异常消息逐字对齐（含 [line %2d] 与 repr(line)）。
  多行值合并用「带标注局部 dict 整体写入字段」口径（避开 DictGet 借用嵌套
  dict 的下标存值路径）。
- **gzip.py + zlib.py + _gzipio.py + zlib.cpya + gzip_glue.c**：系统 zlib
  原生（cimport，link z + C 胶水，z_stream 留 C 侧）。gzip.compress **字节级
  同 CPython**（XFL/OS/mtime=0 默认）；decompress 多 member/零填充/
  FEXTRA/FNAME/FCOMMENT/FHCRC/CRC/ISIZE 校验；GzipFile rb/wb/rt/wt 整读整写 +
  上下文管理 + 通用换行；zlib compress/decompress/crc32/adler32。错误消息对齐
  （BadGzipFile/EOFError/Error -3/-5）。_gzipio 单独成模块是因为 gzip.py 的
  open 遮蔽内置 open。

### 前端使能（argparse 前提，通用语言修复，各带回归用例）

- **F1 跨模块动态属性**：模块接口导出 needs_dict（格式 v11→v12，旧 modcache
  自动失效）；getattr 字面量名/属性直写尊重 needs_dict。导入类实例 args.x
  直读直写/getattr/setattr/hasattr/vars 全通（257）。
- **F2 setattr 非字面量名**：值 coerce 进 unknown（补 float 装箱漏路）（257）。
- **F3 显式 object/Any 形参保持 unknown 域**：不回填单态化，float 实参调用点
  装箱；接口序列化 ann_unknown；负数字面量默认值（-1/-2.5，literal_of_expr/
  as_const 收一元负/正）（258）。
- **F4 类型名当值**：int/float/str 等是 TypeObj（机器表示＝类型名字符串），
  同一性/相等/print/str/容器/isinstance/issubclass/type(x) 全通；str(类型对象)
  = "<class 'name'>"；类型对象值当被调者明确拒绝（259）。
- **B1 空容器字面量类型洞按目标填**：字段赋值/重赋值（declare）、dict/list
  下标存与 append（merge_elem）、return（stmt_return）、实参（bind_param）。
  修复 str 键退化成指针哈希、内容相同的键查不回（260）。**异构列表改用
  unknown 单例（不可填）**：[1, "x"] 的 fresh_hole 会被目标单态化成 list[str]，
  字面量按 kind1 建表、int 元素当堆指针 dec → 段错误（206 回归 + 260 新增
  异构用例；该单例化同时修掉基线已有的同族崩溃）。
- **B2 is/is not 对 unknown/Optional 操作数补 Mov 到机器字**：修复 cmp.eq 按
  底层 str 走内容比较、None 当 NULL 解引用段错误（261）。
- **B3 unknown 迭代/物化**：for/推导式/list()/tuple() 经 pa_any_iter_list 按
  运行期种类展开（list 零拷贝+1、str 拆码点、dict 取键）。**放 rt_class.c，
  刻意不动 rt_any.c（stdlib1 分支合入）**（257/261）。
- **B4 vars() 合并动态属性 dict**（AttrDict 借用 + DictUpdate）（257）。
- **B5 repr(unknown)**（ReprAny → pa_repr_any）（261）。
- **B6 int()/float() 错误消息 CPython 口径**（with base %d: %r / could not
  convert string to float: %r，exc_new 复制后释放临时缓冲，longjmp 前无泄漏）
  （261）。
- **B7 cimport link "file.c"**：cwd 找不到时回退 .cpya 所在目录（转绝对路径），
  stdlib 分发不依赖用户 cwd（255/256）。
- RtFn 新增（AttrDict/AnyIterList/ReprAny）**全部追加枚举末尾**，不改既有编号。

### 回归用例（tests/run/250–261，12 个，先对拍 CPython 再钉 expect）

250 argparse 基本 / 251 argparse 错误（自子进程）/ 252 argparse help 排版 /
253 configparser 基本 / 254 configparser 错误 / 255 gzip 字节级往返 /
256 GzipFile / 257 跨模块动态属性 / 258 object 形参 / 259 类型值 /
260 类型洞填充 / 261 is-None 与消息。O0/O1/O2 逐字节一致。

### 已知问题（差分 harness 新发现，**基线 fdeea25 已验证复现**，非本批引入）

leaf 值（str/bytes/float 盒）× unknown 域 RC 不对称：0.321 leaf-str 不进索引，
探针 inc/dec 对 leaf 失效，与直接 inc/dec 混用时引用计数不对称 → 小泄漏。
最小复现 j12（def f(s: str) -> object: return s.lower()，104B）/ m5
（print(f() * 2)，f -> object 返回 float，72B 盒）；PYADAMAS_GC_CONSERVATIVE=1
全消、GC_DISABLE 仍漏（与收集无关）。绕行：object 调用结果先赋带标注局部变量
（j14 口径）。根治属 P2/P3 unknown 表示层工作流，本批不做表示层改动，记录移交。
详见 docs/stdlib-batch1.md §四半。

### 验证

- difftest **251 / 0**（239 + 12）。
- 普通套件 **269 ran ok / EXIT=0**；ASan 套件 **269 ran ok / EXIT=0**
  （PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 抽查 12 新用例（pty 口径，LSan die 路径会丢缓冲
  stdout）：**12/12 程序输出逐字节正确**；251/252/254/255/260 零泄漏；
  250/253/256/257/258/259/261 有小泄漏（72–3040B/用例），出自上述基线已
  验证的 leaf×unknown 机制。保守口径（PYADAMAS_GC_CONSERVATIVE=1）下
  leaf×unknown 泄漏全消（256/258 等归零）；253 余 1 处 32B 异常对象泄漏
  （fallback 路径 raise→catch→丢弃，rt_exc.c 本批未改，最小复现 j17/j18
  不漏、需更多上下文，属异常路径 RC 既有问题，另录移交）。
- check_size：仍只有原有 5 处超限，**无新增**（lower_stmt.cpp 精确压到 1200；
  新增 stdlib/*.py 与 runtime 函数不在 src/ 1200 行规则内）。

### 分支与集成建议

- 分支 **stdlib1**，基于 fdeea25（0.324）；不推 master，主树与主树 build 未触碰。
- 与 P2/P3 并行：未动 rt_any.c（pa_any_iter_list 在 rt_class.c）；RtFn 追加
  末尾，集成按落地顺序重排编号（rt_name 同步）；模块接口 v12 与 P2 的接口
  改动合并时注意 enc/dec 字段顺序。
- 合入前重跑：difftest → 250–261 → 普通+ASan 套件 → leak 抽查 → check_size。
  若主树期间改过 lower_expr_ops/lower_builtin_reflect/typecheck_call/
  typecheck_types.h（merge_elem）/module.cpp 相邻代码，重点复核 is-None 的
  Mov 路径、bind_param 的 ann_unknown 早退、merge_elem 的洞填充分支。

## [0.325] - 2026-11-19

> p2tag 分支：unknown 表示层 **P2 容器级域 tag 评估**（0.324 后续，与 stdlib
> 轮并行；集成时 CHANGELOG 编号按实际落地顺序重排）。**结论：P2 容器域 tag
> 单独落地收益不抵复杂度，建议直接 P3 封闭 tagged union。** 未改运行时/
> 编译器（尝试均已回退），仅新增评估文档 docs/p2-container-tag-eval.md。
> 不推 master、不动主树。

### 评估结论（docs/p2-container-tag-eval.md）

- **成本主体在标量 unknown 侧，不在容器侧**：perf 剖析 u1 的 rc_index_has
  占 ~40%（pa_rc_inc_unknown / dec_unknown / temp_unknown / any_decode 的
  探针），u3 的探针 ~25% + float 结果盒分配/回收 churn ~24%（rc_alloc_core /
  rc_link / rc_index_put / del / pa_rc_dec_slow）。这些来自 for 循环变量 v、
  累加器 total/acc 等**标量 unknown 槽**每轮 retain/release 与结果装箱。
  容器域 tag 管不到取出后的标量（设计文档 §4.3「tag 属于容器，取出即丢」），
  故对 u1/u3 主体成本无效。
- **硬阻塞（asm 约束）**：降装箱频率需「裸 float 内联 + 2-bit 域 tag」，
  必须改 kind2 下标写入的生成代码（src/codegen_index.cpp 内联 `btsq/btrq`
  1-bit 位图维护）。26 个热基准里 **8 个**（b1/b4/d2/d4/d6/fannkuch/matmul/
  nqueens）含这条内联路径，改它破坏「26 基准 asm 逐字节不变」（约束 ①）。
  2-bit 升级原型实测令 206_list_store_kind SIGSEGV（内联 1-bit 置位写花
  2-bit 域字，销毁把整数当堆指针 dec），已回退。
- **可行但复杂的替代**（独立 dom 数组 + 编译器「可能含 float」路由）理论上
  asm 安全，但需数据流分析 + 写入/读取路由 + 裸 float 释放/传播，复杂度中高，
  且 u1 标量成本仍在 → 收益不抵复杂度。
- **旁证**：探针「叶子位图优先」重排（运行时-only，asm 安全）实测 u1 −1.7% /
  u3 −1.1%（噪声内），u2（dict 探针密集）显著下降——印证 u1/u3 成本在探针/
  装箱**次数**（标量侧），非探针顺序。实验已回退。

### 给 P3 的输入

- P3 封闭 tagged union（{i8 tag, 8B payload} 内联，tag 跟值走）才能同时消掉
  标量槽探针与 float 结果装箱 churn；容器元素域 tag 作为 P3 子集自然导出，
  避免与现有内联 1-bit heap_bits 冲突。静态已知路径仍零开销（26 基准 asm
  均无 pa_any_* 调用，P3 对其影响可控）。

### 验证（干净基线 fdeea25 复核）

- 2.1 的 2-bit 尝试与探针重排均回退，worktree 相对 fdeea25 仅新增本文档与
  本 CHANGELOG 条目。
- tests/run/206_list_store_kind.py 对拍 expect 一致；普通套件 ran ok /
  EXIT=0、difftest 全 0（PYADAMAS_TEST_JOBS=8）。0.324 异构 print/算术正确性
  （247_unknown_float_tag）保持不变。

## [0.324] - 2026-11-19

> typetag 分支：unknown/动态值表示层对齐 codon 的"带标签方向"——先做设计
> 评估（docs/unknown-repr-design.md，7 方案矩阵），再落第一阶段原型：
> 推断消 unknown + 真 unknown 的 float 最小判别（复用 CPYFLOAT 盒）+ 若干
> 被原型暴露出来的既有洞。**不推 master、不改主树、不动主树 build**；
> 全量 NaN-boxing 评估为不可行（信息论边界，见设计文档 §2/§4.1），未做。
>
> 判据：静态已知路径零开销（26 个基准 asm 逐字节对比，22 个完全相同、
> 4 个更少指令）；标签成本只落在真 unknown 冷路径；8 字节容器槽不变宽。

### 设计与结论（docs/unknown-repr-design.md，新增 429 行）

- 对比 NaN-boxing / codon 式 tagged union / 旁路 tag / 装箱 / 指针低位 tag /
  推断优先 / 混合，给出正确性、内存、热/冷路径、ABI、复杂度、开放封闭矩阵。
- **核心边界**：int64 与 double 都占满 2^64，64 位内不可能同时保留全 int64、
  全 double 再塞类型 tag。任何 in-band 方案都要牺牲一个值域（或依赖指针
  规范化假设），与 pyadamas 文档化的 int64 回绕/完整 NaN 语义冲突；全量
  NaN-boxing ABI 重构因此不做，记录理由。
- **推荐分阶段**：P0 推断消 unknown（最大杠杆，零运行期成本）→ P1 真 unknown
  最小 float 判别（装箱，本阶段）→ P2 容器级 2-bit 域 tag / dict entry tag →
  P3 显式 union/封闭 tagged union（16B 仅用于 union 容器）→ P4 None/bool/
  整型的通用装箱。开放 unknown 与封闭可推断部分分治；与 codon 的
  UnionType（codon/cir/type.h:572、llvisitor.cpp:2776）和 PyObject
  （llvisitor.cpp:851）逐项对照。

### 推断 / 降级（P0，零运行期成本）

- **dict.get 两参数形式把缺省值类型 join 进值类型洞**：空 dict 的计数器
  counts[w] = counts.get(w,0)+1 从 dict[str, unknown] 推断成 dict[str, int]，
  回到纯 addq 路径并重新触发 dict_bump 融合；dict_wordfreq A/B **−8.78%**。
- **list.append 按元素类型 coerce**：修掉空列表先 append(int)、后 append(float)
  时首元素按 int 位模式存进 list[float]（打印 5e-324）的老 bug。
- **for 元组解包的目标类型优先取源 tuple 的 sub[i]**（不再依赖可能停在
  unknown 的符号表类型）：fannkuch 整函数摆脱 unknown 帧扫描/索引，帧
  144B→96B，A/B **−2.62%**。
- **dict_bump 融合接受 6/7 指令两种形态**（值类型是 int 时没有 RcIncUnknown
  那条 retain）；b1_log_parse / b5_re_extract 的计数器重新命中融合（2/1 处），
  A/B −1.14% / +0.14%。

### unknown 的 float 最小判别（P1，runtime/rt_any.c 新增）

- **float 进入 unknown 时装箱成 PYADAMAS_RC_CPYFLOAT**（64B RC 头 + 8B
  double payload）；unknown 仍是 i64，ABI 不变。int/None/bool/普通堆指针
  保持裸机器字。只装 float：int/str/容器本来就能靠机器字 + RC/kind 探针
  区分，全量装箱会反噬 0.321 的字符串成果。
- 新增运行时入口：pa_any_from_f64 / pa_any_to_f64 / pa_any_to_i64 /
  pa_any_arith（int/float/str 分派，保持 int64 回绕与 Python 的 //、% 语义）/
  pa_any_neg / pa_any_cmp（数值提升 double、str 字节序、异类型 Eq/Ne）/
  pa_any_is_float。实参静态域用 'i'/'f'/'s'/'o'/'x' 描述符传给运行时，
  静态已知的实参跳过堆探针。
- 降级层：float<->unknown 走装箱/解盒（只在这两个域）；unknown->int/bool
  保持裸机器字直通（避免把 fannkuch 这类下标/计数变成调用）；unknown 上的
  算术/一元负/比较走动态分派；list.append/insert 的元素 coerce；
  显式 int(unknown)/float(unknown) 解盒；isinstance(unknown, float) 精确判定。
- print/repr/str/f-string/csv 全部打通：pa_str_any 支持 CPYFLOAT 与
  容器 repr；异构 dict/list 的 float 打印/算术、混合 csv 行不再打位模式。
- **冷路径成本控制**：CPYFLOAT 按索引叶子处理（0.321 的 4KB 页位图，
  不进 GC 链、不驱动阈值）；1024 槽 freelist 复用动态结果盒；256 槽常量
  float 盒缓存（重复写同一字面量零分配）。合成负载从 +1471% 降到 +316%
  （u1），u2 +7.3%，最坏形态 u3 +373%（累加器每轮产生新 double）。
- 内存：盒 72B/个，缓存上界 256 盒 ≈18KB、池 1024 盒 ≈74KB，退出归还；
  容器槽不变宽（仍 8B），没有 16B tagged 槽。

### 被原型暴露并修复的既有洞

- **cimport/extern 返回 float 的 vreg 被记成 unknown**：
  def_heap(in.ty, std::move(in)) 在同一实参列表里读 in.ty，求值顺序未指定；
  GCC 先构造 by-value 参数移走 in.ty。改成先取 TypePtr 再 move
  （lower_method.cpp / lower_c_lazy.cpp）。unknown->float 解盒第一次把
  "机器值在 xmm、静态类型 unknown"的长期不一致暴露成数值错误。
- **dict.keys/values/items 在 exact-RC 下 SIGSEGV**：运行时按 key_own/val_own
  混合种类以 heap=2 追加元素，值为 int/float 标量时会走 unknown 探针；
  索引关闭后 exact-RC 回退把对齐的 double 位模式当指针解引用
  （231_float_dict 实测）。把 DictKeys/DictValues/DictItems 以及 0.324 的
  AnyFromF64/AnyToF64/AnyArith/AnyCmp/RcTempUnknown 等加入"必须保留索引"
  集合（src/ir.cpp）。
- **pa_dict_bump 插入新键不 retain**：融合删掉了 IR 里的 RcInc(key)，但运行时
  miss 分支没有补引用；计数器推断成 int 后 b1/b5 第一次触发该融合，动态
  构造的键插入即被释放，留下悬空键（"refcount increment on a freed object"）。
  修：miss 分支 pa_own_inc(key_own, key)。

### 回归用例

- 新增 tests/run/247_unknown_float_tag.py（167 行，先对拍 CPython 3.14 再钉
  expect，O0/O1/O2 全一致）：异构 list/dict 的 float 打印/str/f-string；
  unknown 上的 + - * / // % 与六种比较、一元负、int()/float()/bool()；
  unknown 形参/返回/object 字段/循环变量；空容器 append 推断；
  嵌套 list[object]/dict[str, object]；csv 混合行（str+int+float，含
  list[object] 行）。已知边界（None/bool 仍裸 0/1、嵌套具体容器无域 tag、
  extend/+= 不逐元素装箱、f-string 格式说明）在设计文档 §10 列明。

### 验证

- difftest **239 / 0**（238 + 247）。
- 普通套件 **257 ran ok / EXIT=0**；ASan 套件 **257 ran ok / EXIT=0**
  （PYADAMAS_TEST_JOBS=8）。
- ASan detect_leaks=1 抽查 247 / 231_float_dict / t9 装箱 churn 负载：
  无 leak/ERROR，输出与 expect 一致。
- 热路径 asm：26 个基准（9 个 compare 数值 + realistic 6 + daily 11）逐字节
  对比，22 个完全相同；4 个更少指令（dict_wordfreq、b1、b5、fannkuch，均为
  推断/类型修正的收益）。
- A/B（绑核 24，7 轮交替 + 奇偶交换 + BURST=3）：dict_wordfreq −8.78%、
  fannkuch −2.62%、b1 −1.14%，其余 ±0.4% 内（d1 +1.07% 是 3.3ms 短用例
  噪声，asm 相同）；nbody/spectral/matmul ≤0.2%。字符串类 b2/b3/b4/b5
  −0.29%~+0.34%，0.321 成果保住。
- unknown 冷路径（bench/unknown/，合成）：u1 +316%、u2 +7.3%、u3 +373%，
  设计文档 §7.3 给出解读与后续优化目标。
- tests/check_size.sh 仍只有原有 5 处超限，无新增超限文件；新增
  runtime/rt_any.c 363 行（运行时文件不在 src/ 1200 行规则内）。

### 分支与集成建议

- 分支 **typetag**，基于 8248c21；不推 master。主树 /src/luo-proj/pyadamas
  与主树 build 未触碰；本 worktree 用独立 build。
- 集成顺序建议：先合 0.323 之后的性能轮，再 rebase 本条；CHANGELOG 编号按
  实际落地顺序重排。合入前重跑：difftest → 247 → 普通+ASan 套件 → ASan
  leak 抽查 → check_size → 26 基准 asm → abbench。若主树期间改过
  lower_expr_ops/lower_expr_dunder/opt_dict_bump/ir.cpp 的相邻代码，重点
  复核 RtFn 分组（新增 enum 必须在末尾）和 dict_bump 6/7 形态。
- 风险：装箱冷路径成本（u3 最坏），以及新增的 256/1024 进程级缓存；
  所有缓存退出时归还，ASan 已验证。设计文档 §10 的已知边界需要在语言
  文档/用户可见说明里同步（尤其 None/bool 与嵌套具体容器）。

## [0.323] - 2026-11-19

> 遗留 bug 修复轮：0.321 记录的「unknown 元素双 inc 泄漏」与 re 空匹配推进
> 旧口径两项 P0；P1 的 unknown→具体元素重解释洞评估后记录边界；P2 两项
> （unknown 值类型标签 / statistics.mean 精确求和）给出设计与影响评估，延期。
> 不触碰 README/docs；build/release（0.271 oracle）未重建。

### 正确性修复（P0）：unknown 槽所有权泄漏 + re.findall/finditer 空匹配推进

- **unknown 元素写入 unknown 全局/字段连发两次 `pa_rc_inc_unknown`（每个元素漏一个引用）**：
  `store_global` / `store_field` 在统一的所有权入口 `retain_for_store`（unknown 源走
  `rc_inc_unknown`、堆源走 `rc_inc`、fresh 值走 `move_fresh` 转移临时栈那份）之后，
  又对 unknown 源显式补了一次 `rc_inc_unknown`。全局出口 `release_globals` 和字段
  析构只释放一次，于是 `for v in list[object]` 的循环变量（模块级是全局）和
  `v: object = xs[i]` 每写一次就多留一个引用。字符串还是 tracked 叶子时被退出时
  沿链 free 掩盖，0.321 改成索引叶子后被 LeakSanitizer 直接抓出（240 用例当时
  只能绕开槽赋值）。**修**：删除这两处重复 inc，所有权只由 `retain_for_store` /
  `move_fresh` 给出一次。gdb 断点复核：修复前每个 unknown 全局写入两次
  `pa_rc_inc_unknown`，修复后一次。
- **unknown 局部槽 / for 变量「只 retain 不释放」（同族泄漏，0.321 只记了全局/字段口径）**：
  `release_slots` 历史上整类跳过 unknown 槽（注释记录了试过无条件释放导致 12 个
  用例 over-release：装饰器回填、borrow_slots、帧槽别名等路径的 unknown 槽可能
  持有借来的值）。但 `retain_for_store` 自 0.215 起确实会给 unknown 槽补引用，
  两边口径不一致 → 每个 unknown 局部变量/循环变量泄漏一份。**修（槽级所有权记账）**：
  - `store_slot` 的 unknown 分支：覆盖前**无条件**按注册表释放旧值（槽在函数序言 /
    生成器帧创建时清零，第一次 `dec_unknown(0)` 是空操作；同一条 store 在循环里
    执行多次，不能按 `slot_owned` 的静态值跳过，否则每轮漏上一轮那份）；写入时
    retain 过（moved / 堆源 / unknown 源）就置 `slot_owned`，只置不清。
  - `release_slots`：owned 的 unknown 槽出口 `rc_dec_unknown` + 清零；borrow 槽和
    未经 `store_slot` 的裸写槽（如 `DictTryGet` 的值出口）保持跳过。
  - `inc_params`：unknown 形参入口 `rc_inc_unknown` 并置 owned，出口对称释放 ——
    否则「形参槽在某些分支未被覆盖」时静态 owned 标记会导致 over-release。
  - codegen 帧记录：owned unknown 槽进 `frame_owned`（异常展开按注册表 dec），
    借来的才进 `frame_scan`；生成器/协程包装对 unknown 形参 `rc_inc_unknown`，
    `slot_heap` 的 2 类覆盖 owned unknown 槽（正常终止由 `release_slots` 释放并
    清零，异常/GC 销毁由 `pa_gen_slot_release` 幂等释放）。
  - 内联：entry 释放旧值的条件从「堆槽」扩到「堆槽或 unknown 槽」且 owned，
    dec 选 `RcDecUnknown`；形参复制同样处理。
- **`re.findall(r'\b|a', 'ab')` 比 CPython 3.7+ 少一项（空匹配后推进旧口径）**：
  旧实现空匹配后 `pos = end + 1`，直接跳过同起点的非空分支。CPython 3.7+ 的
  finditer 是「下一次仍从同一位置搜，但同位置的空匹配必须失败」，让引擎回溯去
  试 `a` 分支：`(0,0)`、`(0,1)`、`(2,2)`。**修**：VM 增加 must-advance 口径。
  - 新增 `RE_MATCH_TOP` opcode 用于整段程序的最终 MATCH（断言/原子组子程序内部
    的 `RE_MATCH` 保持原样 `return 1`，不受影响）；判定用文件级
    `re_no_empty`/`re_no_empty_pos`，不往 ReCtx 加字段。
  - `find_from` 增加 `must_advance`：空匹配重试时先在同一位置做一次带标记的
    尝试，失败才从下一位置继续普通扫描；普通扫描循环逐条保持原实现（热路径
    不背 must-advance 判断）。`findall`/`finditer`/`Pattern.findall` 循环改为
    `adv = (caps[1] == caps[0])`、`pos = caps[1]`。
  - 附带修：空 subject 上 `^`/`\A`（`first_kind == 2` 在 `start == len == 0` 被
    误判死）。`re.findall(r'^', '')` 从 `[]` 改为 `['']`，与 CPython 一致；
    0.271 oracle 同款旧行为。
  - `re.split` / `re.sub` 仍传 `must_advance=0`，维持旧口径 —— 它们的零宽语义
    不在本轮范围（单独记录）。

### 回归用例

- 新增 `tests/run/246_unknown_rc_re_empty.py`（先对拍 CPython 3.14 再钉 expect，
  O0/O1/O2 全一致）：局部/全局 `list[object]` 的槽赋值与 `for` 循环变量、unknown
  字段（类级标注 `v: object`，`__init__` 里的 `self.v` 会被首赋值收窄成 int，测不到
  unknown 字段路径）、unknown 形参、re 空匹配（`\b|a` / `|a` / `a*|b` / `\b|ab` /
  前瞻与后顾 / `(\b)+` / `(a*)+` / finditer spans / 空 subject 的 `^` `\A` `$`）。

### 验证

- difftest **238 / 0**；普通套件 **256 ran ok / EXIT=0**；ASan 套件 **256 ran ok /
  EXIT=0**（`PYADAMAS_TEST_JOBS=8`）。
- ASan `detect_leaks=1` 抽查 246 / 13_gc / 174_gc_construct_roots 全干净（无
  leak/ERROR 输出，输出与 expect 一致）；修复前 246 的最小复现（局部 for 变量 +
  全局 + 字段）各报 4–5 对象 Direct leak。
- 0.271 oracle 语料：36 条差异，**全部为时代差**（201–229 共 23 例 oracle 编译
  不了、206/237 oracle 侧 SIGSEGV 为既有现象、230/231/232/236/238/239/245 新用例、
  47/66/162/185 是 0.313–0.317 自身修复的 expect 变更、246 为 0.323 新用例且
  oracle 侧 SIGSEGV）；无意外差异。
- `tests/check_size.sh` 仍只有原有 5 处超限（codegen_regalloc.cpp / driver_build.cpp /
  ir_ops.h / lower_impl.h / typecheck_types.h），无新增超限文件；lower_impl.h 从
  1233 → 1278 行仍属既有超限项。
- re 基准（`tools/rebench.py`，9 轮交替 + 奇偶交换，基线 54ab489 独立 worktree）：
  **TOTAL 279.140ms → 279.199ms（+0.0%）**；perf stat（-r 5）instructions
  2284.0M → 2286.2M（+0.1%）、cycles 783.6M → 785.1M（+0.2%），判中性；个别
  短用例 ±4% 属布局/噪声（同轮 sub_group −3.4%、findall_alt −3.4%）。
  **负收益记录**：第一版把 must-advance 字段加进 ReCtx 并在 `run()` 的 RE_MATCH
  里判定，re 基准合计 **+8.4%**（关闭判定仍 +8.1%，根因是 ReCtx 布局/热函数
  代码位移）；改成「文件级标记 + RE_MATCH_TOP 冷判定 + 热扫描循环逐条不变」后
  回到 0.0%。
- 浮点/现实热路径：对 nbody / spectral_norm / matmul / fannkuch + realistic 四案
  与基线 54ab489 逐案 `pyadamas asm -O2` 比较，生成汇编 **cmp 逐字节相同**；
  unknown 形参的入口 inc 只落在未标注签名的函数上，不触及浮点寄存器分配。

### P1 评估（本轮不做，记录边界）

- **unknown→具体元素隐式重解释**（`list[object]` 实参传给 `list[str]` 形参）：
  0.322 曾尝试在编译期拒绝，误杀 `bisect(list[unknown])` / `copy.copy_list` 等
  合法泛型用法后回退。本轮复核：光看「实参元素 unknown、形参元素具体」无法
  区分两种情形 —— 泛型函数是**单态化**的，`T` 可能在更早的调用点已经被回填成
  `str`，之后同一个 `list[unknown]` 实参与「真正的重解释」在签名比较处完全同形；
  而 bisect/copy_list 内部只做比较/搬运，不按 `str` 指针解引用。要精确守卫需要
  「按调用点重新单态化」或「元素级运行期类型标签」（见下 P2），二者都超出局部
  修复范围。**延期**，已知边界与 0.322 相同：把 `list[object]` 传给具体元素标注的
  形参仍可能被按具体类型读（混合容器优先用 `list[object]` 全程承载）。

- **同批发现、未修（既有，基线同款）**：非内联的 `-> object` / 未标注返回函数，
  调用结果的那份引用没有进临时栈。`def f(x: object, flag: bool) -> object` 在循环里
  调用并赋给 unknown 全局时，0.271 / 54ab489 基线与 0.323 同样每轮漏一份（ASan
  4 对象 / 416B）。它与本轮 P0 的「槽/全局重复 inc」不是同一层（调用结果所有权
  本身没入临时栈，store 再 inc 就多一份），修它要改 `def_heap` 对 unknown 返回值
  的临时栈口径，影响面大于 P0，另票。本轮 246 的 unknown 形参函数在内联后
  不产生这条路径，LSan 干净。

### P2 专项设计（评估后延期，不半做）

- **unknown 值无类型标签（异构 dict / 混合 csv 行里 float 打位模式）**：
  现状 unknown 是裸 64 位字，堆性靠 rc 索引/叶子索引探针，int 与 double 位模式
  不可区分；`pa_repr_any`、csv 混合行、异构 dict 打印都只能把 float 当整数。
  三条路线：① **NaN-boxing**：把 int/指针/float 全部打包进 64 位（指针/整数加标签），
  改动表示层与所有寄存器/栈槽/容器/GC 出边识别，影响全部 codegen 与运行时 ABI，
  属根本性重构；② **每元素旁路 tag**：kind2 列表已有 `heap_bits`，可再加每元素
  1 字节 tag（或扩展位图）区分 int/float/str；dict 条目借助现有 flags 扩展；
  unknown 帧槽需要并行 tag 数组并进 GC/异常展开口径。内存 +1B/元素（仅混合
  容器）、热路径在 kind2 上多一次 tag 读写，需 abbench 验证；③ **装箱 float**
  （推荐分阶段）：unknown 边界上的 float 统一转成现有 `PYADAMAS_RC_CPYFLOAT`
  盒（8 字节 payload 存 double），读回具体 float 上下文时拆箱。表示局部、不动
  ABI；代价是 unknown 浮点流的分配/RC（只在未标注/混合路径），且能直接让
  print/csv/异构 dict 拿到值语义。**方案③为首个专项轮候选**，②作为性能回退
  备选；①不建议。预估影响面：rt_list/rt_dict/rt_cpy/rt_format + lower 的
  unknown 装箱/拆箱点 + 回归；1–2 个专项轮。
- **statistics.mean 精确求和**：CPython 3.12+ 用 `Fraction` 精确有理数累加再除以 n
  （`mean([0.1,0.2,0.3]) == 0.2`）。double 的分母可达 2^1074，n 个值求和的中间
  分子远超 int64；int 输入的精确和也无上界。方案：专用定点大整数累加器
  （~2176 位 = 34×64 位，覆盖全部 double 指数 + log2(n) 进位）按最小指数对齐
  求和，再正确舍入到 double；int 路径仍需真 BigInt（无上界）。`math.fsum(xs)/len(xs)`
  不够（该例仍给 0.19999999999999998）。代价：约 300–500 行运行时 + 舍入/溢出
  边界 + 对拍，超出本轮，**延期**；绕行仍是整数/定点数据（金额用 int 分）或
  接受 1 ulp 差异。

## [0.322] - 2026-11-19

> bugfix2 分支（基于 cedca8c / 0.320），与主树性能轮并行开发。性能轮 0.321
> （86d9c6b）改的是 runtime/rt_core.c（str/bytes 叶子 GC 记账）+ rt_file.c +
> 新增 tests/run/240；本批改 rt_re.c / rt_dict.c / rt_str.c / rt_exc.c 与
> src/codegen_* / lower_* / typecheck_* / stdlib/csv.py。**源码无共同文件**
> （rt_str.c 本批只在 pa_repr_tuple 前追加新函数，0.321 未动它），rebase 的
> 唯一冲突点是 CHANGELOG 顶部的插入位置（0.321 与 0.322 相邻），按落地顺序
> 排列即可。

### 正确性修复（P0 崩溃/误编译）：N19b 浮点副本槽、生成器表达式元素类型、csv 混合行、re 零宽量化

- **N19b 余半（O2 下 amortize 循环把 pay 读成 0，静默误编译；基线 2aae2ac 同复现）**：N19a（0.314）只补了「别名 load 的 use 点」，没动掩码本身的宽度。**根因**：调用点浮点 spill 掩码 `fp_live_in_/out_`、每指令 live-after 掩码都是 `uint8_t` 位图（每提升槽一位），隐含「提升槽 ≤ 8」的假设 —— 那是「整函数提升 + 寄存器不复用」时代的口径。R4 的线性扫描按活跃区间**复用** xmm8-15 后，提升槽数可以超过 8（amortize 13 个浮点槽跑在 8 个 xmm 上）；位图第 8 位之后静默丢失（`1u << fi` 截断/移位为 0），调用点不 spill 被调方要踩的槽。被内联的 `monthly_payment` 里 `r` 因此被 `pa_math_pow` 破坏，外层 `pay` 变成 0。**修**：三处掩码全部改成「每提升槽一字节」的 `vector`（`fp_live_in_/out_` 为 [块][提升槽]，live-after 为 [指令][提升槽]，alias-use 扫描同步），`fi` 不再限制 < 8；溢写/重读仍只处理 mask 置位的槽。≤ 8 个提升槽的函数生成的汇编逐字节不变（见验证）。
- **生成器表达式元素类型丢浮点**：`for v in (x for x in list[float])` 打印 double 位模式、`sum(x for x in list[float])` 按 int 累加。**根因**：合成生成器函数靠单态化回填形参类型，回填后要等 worklist 末尾才刷新调用点类型；但 `for` 的循环变量在第一次检查时已经按 `Generator[unknown]` 定型，之后再没人更新它（`v` 的符号停在 unknown）。**修**：调用点识别 `__pyadamas$gen_*` 且 `sig_dirty` 时**同步重查**合成函数（`TypeChecker::recheck_synth_gen`，与 worklist 同样的作用域/owner 保存恢复），拿到最终 `Generator[T]` 后再返回，`for` 变量因此按 T 定型。
- **csv.writer 收 [str, int] 混合行段错误**：`writerow(list[str])` 收到 `list[unknown]`（混合字面量）时按 str 指针读 int 元素，`delimiter in field` 直接 SIGSEGV。**修**：stdlib/csv.py 的 `writerow` 改收 `list[object]`，每字段 `str(row[i])`（unknown→str 走运行时 `pa_str_any`：堆字符串原样、int 十进制，与 CPython 的 `str()` 口径一致）；`writerows` 同步 `list[list[object]]`。现在 `w.writerow(["x", 1])` 与 CPython 一样写 `x,1`。**已知边界**：unknown 里分不开 float（0.317 记录的类型标签缺失），混合行里的 float 会按位模式字符串化 —— 原来是段错误，现在是「浮点已知限制」，纯 int/str 混合行完全正确。
- **re 量化零宽 SIGSEGV（`re.findall(r'\b+', 'ab')`，base/current 同崩）**：两条根因。① 解析器接受直接量化零宽锚点（`\b+`/`\b*`/`\b?`/`\b{2}`/`^+`…），CPython 在 _parser 里对 `AT` 一律报 `nothing to repeat`；这些模式在 VM 的 plus 循环里体不推进位置，形成无限递归 → 栈溢出。② 组形态 `(\b)+` / `(?:\b)+` / `(a*)+` / `(?=a)+` 是**合法**的（CPython 允许），但循环体同样可以匹配空串，plain SPLIT 没有「无推进」保护。**修**：解析器按 CPython 口径拒绝直接量化锚点（N_BOL/N_EOL/N_WORDB/N_BOS/N_EOS/N_EMPTY，含 `{m,n}` 形式），非捕获组/内联标志组保留一层 `N_GROUP(group=-1)` 包装（不产生 SAVE，让 `(?:\b)+` 合法，与 CPython 的量词 item 语义一致）；编译期为 N_STAR/N_PLUS 的 SPLIT 打标记（`ch=1` 贪婪 / `ch=2` 惰性），VM 在「同一循环、同一位置再次进入」时判定本轮体匹配空串，停止重复走出口（等价 CPython repeat 的 no-progress break）；贪婪/惰性分支顺序与原 SPLIT 逐条一致。RE_LOOP 紧扫快路径不读 ch，语义不变。
- **附带修复（同批踩到的既有汇编失败）**：`2.5 if c else 3` 这类「分支类型不同、整体类型是 float」的 IfExp，else 分支的 int 直接落进 float 借槽，生成 `movq $3, %xmm8`（汇编器 operand type mismatch，整个程序编译失败）。**修**：`lower_ifexp` 两个分支按表达式类型 `coerce` 后落槽。

### 语义修复（P1）

- **re 多捕获 findall 返回 tuple**：`re.findall(r'(a)(b)', s)` 原来返回整段匹配字符串；CPython 在 >= 2 个捕获组时每个元素是各组匹配串的元组（未参与的组为空串）。**修**：模式是**字面量**且扫描出 >= 2 组时，typecheck 返回 `list[tuple[str, ...]]`，lowering 走新的 `pa_re_findall_groups_str`（每组一个 str，`pa_list_new + pa_list_append` 组元组，GC pin 住中间元组）。**已知边界**：动态模式（变量/f-string）与 `Pattern.findall` 仍维持旧的 `list[str]` 口径（返回整段匹配）—— 静态类型与运行时行为保持一致，不会像「运行时一律 tuple」那样把 tuple 指针当 str 读；要修需要 Regex 类型携带组数，另立一项。
- **type(e).__name__ 恒为 Exception**：`except Exception as e` 里 `type(e)` 的类名是编译期常量（静态类型是基类 Exception），拿不到实际子类。**修**：静态类型是 Exception 时，`type(e)` 求值 e 后调用新的 `pa_exc_value_type_name(e)`（`PyAdamasExcValue->type` 就是运行时真实类名），仍按 TypeObj 参与后续 `__name__`/`<class '...'>` 打印；非异常值路径不变。参数在 codegen 的 RtFn 分派里属于 1 参数组（i1），不能放错到 0 参数组（会读到上一个调用的残留寄存器 —— 本批初版就踩过）。
- **异构 dict repr 仍打 `<dict object>`**：`{"a": 1, "b": "x"}` 的静态类型是 `dict[str, unknown]`，print_dict 对 unknown 值直接打占位符。**修**：新增运行时 `pa_repr_dict_any` / `pa_repr_any`（rt_str.c，堆探针递归：str 走 `pa_repr_str` 带引号、list/dict 递归、CPYFLOAT 按 double、非堆按 int），lower_print 对未知元素的 dict 调用它，stdout 与 `print(file=...)` 两条路径都走；空 dict 仍打 `{}`。**已知边界**：unknown 里 float/int 无标签（0.317 记录的固有边界），异构 dict 里的 float 值仍按位模式打印。
- **非原地集合运算 `s | t` / `s & t` / `s - t` / `s ^ t` 报「后端不支持」**：0.315 只做了原地 `|=`/`&=`/`-=`/\^=`。**修**：新增 `pa_set_union` / `pa_set_intersection` / `pa_set_difference` / `pa_set_symdiff`（rt_dict.c；set 复用 dict，返回新 set，左右操作数不动，键按 key_own 补引用）+ 四个 RtFn + `lower_expr_ops` 的 set×set 分派（VBAR/AMPER/MINUS/CIRCUMFLEX）。

### P2（评估后不做，记录绕行）

- **statistics.mean 精确求和**：CPython 3.12+ 的 `statistics.mean` 用 `Fraction` 做**精确有理数**累加再除以 n（`mean([0.1,0.2,0.3])` = 0.2）。对齐需要大整数/有理数（double 的分母可达 2^1074，中间分子远超 int64），PyAdamas 只有 int64；`math.fsum(xs)/len(xs)` 也不够（该例仍给 0.19999999999999998）。代价是引入任意精度整数（或专门的精确有理数累加器），超出本批范围，**未改代码**。绕行：对逐位一致性有要求时用整数/定点数据（如 daily 语料的「金额用 int 分」），或接受 1 ulp 差异。

### 回归用例

- 新增 `tests/run/245_regressions.py`（先对拍 CPython 3.14 再钉 expect，O0/O1/O2 全一致；64 行输出）：csv 混合行（字面量 `["x",1]`、`list[object]` 变量、writerows、默认 CRLF/自定义行终止符）、re 零宽（`\b+`/`\b*`/`\b{2}` 被拒；`(\b)+`/`(?:\b)+`/`(?:\b)*`/`(a*)+`/`(?=a)+`/`(?<=a)+`/`(\b|b)`/惰性空循环）、findall 多捕获（2/3 组、可选组未参与、for 解包、`list[tuple[str,str]]` 注解、0/1 组对照）、`type(e).__name__`（ZeroDivisionError/KeyError/IndexError/ValueError/具体类捕获）、异构 dict repr（嵌套 list/dict、空 dict、`print(..., file=)`）、集合四种非原地运算（int/str/空集/操作数不变）、生成器表达式 float（for/sum/推导式）、N19b amortize 全量行 + monthly_payment、IfExp int→float 分支。

### 验证

- difftest **236 / 0**。
- 普通套件 **254 ran ok / EXIT=0**；ASan 套件 **254 ran ok / EXIT=0**（`PYADAMAS_TEST_JOBS=8`）。180_codepoint_perf 是 wall-clock 用例，并行跑套件时偶发 `gen_slope_ok False`；单独跑 2/2 通过，非本批回归。
- ASan `detect_leaks=1` 抽查 245_regressions / 13_gc / 236_fp_spill_call / 237_rc_container_ops 全干净（无 leak/ERROR 输出）。
- 0.271 oracle 语料：35 条差异**全为时代差**（201-229 共 22 例 oracle 编译不了、206 oracle 侧 SIGSEGV 为既有现象、230/231/232/236/237/238/239/245 新用例、47/66/162/185 是 0.313-0.317 自身修复的 expect 变更），无意外差异。
- `tests/check_size.sh` 仍只有原有 5 处超限（codegen_regalloc.cpp / driver_build.cpp / ir_ops.h / lower_impl.h / typecheck_types.h），无新增超限文件；新增 `src/re_groups.h` 59 行。
- 浮点热路径**中性**：以 cedca8c 独立 worktree 构建基线，对 nbody / spectral_norm / matmul + realistic 六案（queue_sim / str_build / rec_memo / obj_dispatch / dict_wordfreq / sort_struct）逐案 `pyadamas asm -O2` 比较，生成汇编 **cmp 逐字节相同**（≤ 8 提升槽的函数掩码宽度改造不产生任何指令差异），因此未再跑 abbench。

### 与主树性能轮的并行说明

- 本批改 `runtime/rt_re.c`（+134 行）、`rt_dict.c`（+44）、`rt_str.c`（+102，纯追加）、`rt_exc.c`（+6）、`rt_internal.h`、`src/codegen_block/regalloc/impl`、`src/lower_*`、`src/typecheck_*`、`stdlib/csv.py`；0.321 性能轮改 `rt_core.c` / `rt_file.c`。**源码无共同文件**（rt_str.c 未被 0.321 触碰），rebase 冲突面只有 CHANGELOG 顶部条目顺序；master 新增的 tests/run/240 与本批 245 不冲突。
- 集成建议：性能轮落地后 `git rebase master`；CHANGELOG 条目按实际落地顺序重编号（本条预留 0.322）；若性能轮先到 0.322，本条顺延。回归按「difftest → 245 → 普通+ASan 套件 → oracle 语料 → check_size → asm 中性对比」重跑一遍。

## [0.321] - 2026-11-19

### 性能：str/bytes 索引叶子 —— 字符串脚本 RC/GC 记账优化（日常字符串脚本合计 −11.8%）

> 任务给的取证是「B 类字符串脚本 GC 族 ≈28%、根因是 str tracked 叶子」。本轮先
> 按测量纪律复核（HANDOFF §2：perf 占比 ≠ 边际成本、数字先对数量级），结果与取证
> 不一致：**GC 族只有 ~1.4%，真正的天花板是存活堆索引**（字符串占主索引绝大多数
> 条目）。下面按复核后的两个方向落地，两者都保留「字符串留在 rc_index」这条硬约束。

- **测量先行（基线 cedca8c，绑核 24，perf record -F 4000 × 15 轮 / perf stat -r 5）**：b1_log_parse 的 flat 剖面 = 生成代码 run 29.0%；RC 族 rc_alloc_core 8.0% + rc_index_put 6.6% + rc_index_del 5.1% + pa_rc_dec_slow 5.1% + rc_link 2.7% + rc_unlink 1.6% + str_alloc_raw 2.3% + pa_rc_cleanup 1.8% ≈ **31%**；GC 族 gc_collect 0.5% + gc_scan_object 0.4% + gc_mark 0.2% + rc_index_rehash 0.3% ≈ **1.4%**（gcstats 独立佐证：b1 8 次收集共 0.216ms/30ms、b2 4 次 0.184ms/20ms、b4 1 次 0.549ms/20ms）。**与取证不符的原因**：语料脚本存活串少（gc_live 769KB / 512KB），收集触发由分配字节数决定、集合也不大 —— 字符串确实在驱动阈值与集合规模，但绝对开销远小于 28%。
- **str_build 是 exact-RC 模式**（objdump：有 `call pa_disable_unknown_index`、无索引维护调用；gc_live 352B、9 次收集共 0.012ms）：字符串本来就是 untracked 叶子，**不参与 GC 记账** —— 本轮对它不可能有收益，实测中性（见下，两侧生成汇编逐字节相同，差异纯属运行时布局）。
- **优化 1（`PYADAMAS_RC_LEAF`：叶子不进 GC 记账，但保留索引成员资格）**：str/bytes 是不可变叶子（零出边、不可能成环），生死完全由 RC 决定。它们不再进 GC 堆链 / 新生代 / 本轮集合，也不累加 alloc_since_gc / alloc_since_major（同 0.276「untracked 元组不入 GC 记账」的思路，区别是**必须留索引**：unknown 值探针 pa_rc_inc_unknown / pa_rc_dec_unknown 靠 rc_index_has 识别堆指针，整体 untrack 会让 unknown 槽里的字符串漏释放）。实现：rc_link 走叶子分支（marked=LEAF，prev/next 置空，rc_index_put），rc_unlink 只撤索引，gc_phase_dec 的 trial/restore 跳过、sweep 时就地归零释放（rc_leaf_free：索引 del → dtor → str freelist），阈值记账按 marked 位排除；pa_gc_shutdown 增加「先按 major sweep 口径交出边、再 free 链」的一遍（否则环垃圾持有的字符串没人减引用，ASan 会报 Direct leak）；`PYADAMAS_GC_CONSERVATIVE=1` 时整个叶子模式关闭，回到旧的「字符串全跟踪」口径（排障 / 行为对照）。
- **优化 2（索引叶子改「4KB 页 × 16 字节槽位图」，攻击真正的天花板）**：叶子不进主索引，改按页登记：页基址哈希到 2048 桶，每页 256 个 16 字节槽 → 32 字节位图 + 活对象计数；put/del 退化成「页哈希 + 置/清一位」，has 不解引用对象。**精确性与主索引逐元素相同**（不是概率结构）：位图标记「这个 16B 槽位上恰好有一个活着的字符串头」，堆块来自 malloc（ABI 16 字节对齐）且不会两个块落在同一槽；释放清位，之后落在这个地址的整数（v=addr+64）与主索引一样不命中；8 字节对齐的静态驻留串（`PyAdamasStaticStr` 表项 104 字节排布，一半落在 8 mod 16）无法用槽位表示，仍走主索引 —— 路由判据只依赖「地址对齐 + kind」，put/del/has 三处一致。atexit 还掉页节点（必须晚于 pa_rc_shutdown，它还会释放临时栈上的字符串）。实测 b1 剖面 rc_index_put 6.6%→4.4%、rc_index_del 5.1%→4.0%（位图内联进这两个函数），主索引条目数骤降（只剩容器/闭包/异常/8 字节对齐静态串）。
- **优化 3（顺带修：pa_file_readlines 每行多一次 pa_rc_inc 的既有引用泄漏）**：readlines 对刚产生的字符串（rc=1）多加一次引用又不释放，每行泄漏一个引用；tracked 叶子时代它被 pa_gc_shutdown 的「沿链逐个 free」掩盖，字符串变成索引叶子后 ASan detect_leaks 立刻抓出来（174_gc_construct_roots 漏 10MB / 10 万行）。改为 append 直接拿走产生者那一份引用（与 store_list_elem 的 fresh 分支同口径），文件末尾那个空串显式 dec。
- **实测（abbench，15 轮交替中位，绑核 24，基线 cedca8c）**：
  - **日常字符串脚本**：b1_log_parse 26.66→23.82ms（**−10.6%**，p25 −11.4%）、b2_normalize 20.53→17.52ms（**−14.7%**）、b3_template 10.88→9.78ms（**−10.2%**）、b4_wordfreq 24.67→21.01ms（**−14.8%**）、b5_re_extract 26.91→24.55ms（**−8.7%**）；**合计 109.65→96.68ms（−11.8%）**。
  - **perf stat（-r 5）**：b1 instructions 115.1M→101.2M（−12.1%）、cycles 55.8M→48.7M（−12.7%）；b2 81.5M→69.6M / 40.8M→31.0M（−14.6% / −23.9%）；b4 86.7M→79.1M / 48.4M→40.1M（−8.8% / −17.1%）；b5 137.6M→127.5M / 56.5M→51.0M（−7.3% / −9.8%）。
  - **realistic 6 案**：dict_wordfreq **−5.7%**（词频 dict 的键全是堆串，直接受益）、obj_dispatch −0.0%、queue_sim +0.3%、rec_memo +0.4%、sort_struct −0.4%、str_build **+2.0%**（p25 +1.7%）；**合计 1638.37→1624.05ms（−0.9%）**。str_build 的 +2% 是纯运行时布局效应：**两侧生成汇编逐字节相同**（`pyadamas asm` + cmp），instructions 1132.8M→1131.2M（−0.1%），只有 cycles 变化（+1.5%）—— 与 HANDOFF §6.4「gcc 内联/布局扰动」同款；exact-RC 下字符串本来就不参与任何 GC 记账，本轮没有可受益路径。
  - **GC 侧证据**：b1/b2/b4 的 gcstats 从 8/4/1 次收集（0.216/0.184/0.549ms）降到 **0 次**（字符串不再驱动阈值，容器分配量又低于 2MB 门槛）；str_build（exact 模式）保持 9 次 / 0.012ms 不变，证明改动只落在「索引维护开启」的字符串脚本上。
- **验证（终态）**：difftest **236/0**（235 + 新增 240）；普通与 ASan 套件各 **254 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 13_gc / 66_genexpr / 119_gen_except_suspend / 88_defaultdict / 192_dict_cache / 174_gc_construct_roots / 237_rc_container_ops / 240_str_leaf_gc 全干净且输出与 expect 一致；0.271 oracle 语料对比零意外差异（只剩 4 个**既有**时代差，用 cedca8c 基线逐条 `--rebuild` 复核，基线/oracle 的差异与当前/oracle 完全一致：47_float_sum_order、66_genexpr 是 0.317 的 CPython 口径修正；206_list_store_kind 与 237_rc_container_ops 是 oracle 侧 SIGSEGV —— 前者 0.312 起就记录在案、后者是 0.315 新用例）；规模检查仍是原有 5 处超限（rt_core.c 2825 行是运行时文件，不在 src/ 的 1200 行规则内；新用例 240_str_leaf_gc.py 127 行）。
- **新增**：tests/run/240_str_leaf_gc.py（先对拍 CPython 3.14 再钉 expect，O0/O1/O2 + `PYADAMAS_GC_CONSERVATIVE=1` 四种口径输出一致）：字符串密集 churn、unknown 容器元素/unknown 槽持字符串跨多次 GC（含 4 次 major）、类实例持字符串成环的真收集（gc_freed>0）与存活环、字符串键 dict、bytes 叶子、unknown 返回值的两条分支。
- **负收益 / 撤回记录**：① 把 `str_pool_put` 标成 `always_inline`（rc_leaf_free 让它多了一个调用点、被 gcc outline 出 dec_slow，怀疑多了 call/ret）：instructions 略降但 cycles 反升（b1 48.7M→49.7M、b5 51.2M→53.4M，交替 7 轮中位），**撤回**；② 把整块分页登记表移到 `rt_core.c` 末尾冷区（想减少热函数位移、修 str_build 的布局效应）：b1/b2/b4/str_build 全部 ±0.5% 内（与 HANDOFF §6.4 的「布局位移不可预测」一致），**已还原**到索引节旁边（可读性优先）。
- **同批发现、未修（记录备查）**：降级器在「unknown 元素赋给槽 / for 循环变量」时会连发两次 `pa_rc_inc_unknown`（`for v in list[object]: ...` 与 `v: object = xs[i]` 都是），每元素多留一个引用。这是**既有 bug**（cedca8c 基线的 IR 与 rc 计数完全相同，不是本轮引入），tracked 叶子时代被「退出时沿 GC 链 free」掩盖；字符串变成索引叶子后 LeakSanitizer 会把它报出来。240 用例因此改用下标直取 + print（无槽赋值）覆盖 unknown 探针路径，并在注释里写明原因。修它需要动降级器的 unknown 所有权口径，另票。
## [0.320] - 2026-11-19

### 日常脚本语料库（bench/daily/，23 脚本）+ 对拍报告

- **dailycorpus 分支合入**：23 个自包含日常脚本（快速往返 5 / 字符串 5 / csv 5 / 数值 6 / 已知 bug 覆盖 2），四类均衡、带类型标注、确定性数据（int64 安全 LCG）。配套 docs/daily-corpus.md 报告、_tools 计时 harness（run_daily.py + results.json + perf_hotspots.txt）、_bugs 13 个最小复现。
- **价值**：纯运行全类胜 CPython 3.14（数值 7.68×、CLI 6.25×、csv 3.51×、字符串 2.72×，matmul 11.46×）；compile+run 一次性场景全负（编译 42–83ms ≫ CPython 启动 ~18ms，收益在产物复用/长运行）。字符串/分配密集类 RC/GC 占 30–50%，实证 str tracked 叶子是性能天花板（后续优化方向）。本语料挖出 13 个正确性 bug，已由 0.314–0.317 修复（N19b 余半 O2 amortize 循环为既有洞，另票）。
- **口径**：语料在 bench/daily/（非 tests/run，不进套件/difftest）；_bugs 含已修 bug 的历史复现快照。集成时 0.318 json / 0.319 re 已先落地，本条按序编号。

## [0.319] - 2026-11-19

### re 性能：模式编译缓存 + 起点剪枝 + 单指令循环紧扫（基准合计 −46%）

> 与 0.314（json）并行；本条目只动 runtime/rt_re.c（+ 基准/工具/回归用例），
> 不触碰前端与其它运行时文件，合并面独立。

- **测量先行（绑核 24、交替 A/B、奇偶轮交换、多轮中位；基线 = 2aae2ac 独立 worktree 构建）**：新基准 bench/re/bench_re.py 覆盖 20 个用例（字面量/字符类/量词/分组/交替/锚定/病态回溯 × match/search/fullmatch/findall/finditer/split/sub/compile，另有「同模式反复调用」的短行负载与编译后 Pattern 对照），A/B 驱动 tools/rebench.py（两侧各自 --rebuild，交替 + 奇偶轮交换）。最终 15 轮：基线合计 **532.1ms → 283.6ms（−46.7%）**；更早一轮负载较低时同套用例 483.0→230.3ms（−52.3%），方向一致。绝对值随多租户负载漂移，看比值。
- **热点 breakdown（perf + 用例内拆分）**：① 大文本 `re.findall(r"[0-9]+", 20KB)` 61.4ms 中，`run` 占 48%、`find_from` 10%，其余是每次匹配的 str_new/list_append 分配；② 短行反复调用里**编译占 25~49%**（编译后 Pattern 对照：lines_match 1.48ms → 0.52ms、lines_split 1.24→0.57ms），`prog_build` 每个 AST 节点一次 malloc 是主因；③ `re.findall("literal", text)` 39.8ms 全是 VM 逐位尝试——纯字面量完全没有快路径；④ `re.search("zzz", text)` 20ms 是「每个起点都进一次递归 VM」；⑤ `\w+`/`\d+` 这类「单指令体量词」是递归最深也最常走的路径（每字符一次 `run` 递归），占 `run` 的 70%。
- **优化 1（模式编译缓存，最便宜最大收益）**：模块级 API（findall/split/sub/search/finditer 的 `_str` 入口）按 (pattern 字节, flags) 缓存编译产物，8 槽 LRU + 借用期 pin（用户回调里再调 re 不会挤掉外层程序；8 槽全 pin 时退回现编现用）。键用 (len, bytes) 不用 strlen——模式串可以含 NUL。命中只做字节比较，免去整棵 AST 的 malloc/free。`re.compile` 的对象仍各自独占编译产物（语义不变），分析结果按需惰性生成。
- **优化 2（编译期分析 + 起点剪枝，语义零风险）**：编译后算三样只用于**排除必然失败的起点**的信息——纯字面量、首字节集（首个消费指令是字符/类/`.`，含 IGNORECASE 折叠；`|` 两支都有首字节集时取并集）、起点约束（`\A` / 非 MULTILINE `^` → 只有 pos 0；MULTILINE `^` → 只有行首）。分析保守：零宽断言、后向引用、宽子程序一律放弃退回逐位尝试。纯字面量模式 search 走 memchr+memcmp 的朴素子串查找（不用 GNU memmem，保留 Windows 可移植性）、match 走 `memcmp`；其余按首字节集整段跳过不可能命中的起点。
- **优化 3（单指令体循环紧扫 + 一阶失败剪枝）**：`X*` / `X+`（X 是字符/类/`.`）编译期识别成固定布局，把模型里「每字符一次 `run` 递归」换成一次按指令种类特化的紧循环（类→位图、字符→直比、`.`→换行判断），再在出口处按原顺序逐个回溯（贪婪从右往左、惰性从左往右），**语义与递归展开逐位等价**；紧扫的字节数按原口径一次性记入步数上限（否则超长 subject 上本该 RuntimeError 的场景会静默成功）。贪婪侧再加一阶剪枝：若循环后紧跟「一个消费指令 + MATCH」（`X*Y` 这类相邻匹配，Y 必然吃掉一个字节），先把「Y 不可能匹配」的回溯点整段跳过。仅对「循环后紧跟收尾」的形状启用——嵌套量词（`(a+)+b`）没有收尾可剪，开启纯多付扫描开销（实测 +17%），保持原递归展开。
- **实测（15 轮中位，基线 2aae2ac）**：findall_literal 40.16→2.59ms（−93.6%）、search_miss 19.42→0.115ms（−99.4%）、search_literal_hit 0.393→0.035ms（−91.0%）、findall_alt 68.81→12.73ms（−81.5%）、sub_group 43.35→7.96ms（−81.6%）、sub_digits 38.02→16.65ms（−56.2%，原 38.02ms 里含 120 次编译）、findall_class 75.58→50.85ms（−32.7%）、split_ws 82.57→60.98ms（−26.1%）、finditer 45.62→32.85ms（−28.0%）、findall_word 89.17→73.87ms（−17.2%）；短行负载 lines_match −58.9%、lines_search −63.7%、lines_findall −48.7%、lines_split −41.3%、lines_sub −46.7%、lines_fullmatch −60.5%；**编译后 Pattern 对照** lines_findall_pat −29.0%、lines_split_pat −24.4%、lines_sub_pat −29.1%、lines_match_pat +1.5%（已到 VM/分配极限）。病态回溯两例（`(a+)+b`、`(a|aa)+$`）**中性**（+1.6% / −0.4%，perf stat 指令数 +0.1%），未做记忆化，见下。
- **负收益/撤回记录（都是 abbench 判死的，profile 阶段看不出来）**：① 「所有简单循环」都上紧扫在 `(a+)+b` 上 **+17%**（多一次 O(n) 扫描、没有收尾可剪），改为仅「循环后紧跟 MATCH」启用，信号来自编译期 opcode 替换而不是运行期判断；② 贪婪回退时「跳过字节不满足 X 的回溯点」的证明剪枝实测无收益（±1%，噪声级）且证明链条依赖出口程序只读该位置字节，为省风险撤回；③ **内联扰动**：把紧扫快路径内联进 `run()` 让递归 VM 栈帧从 0x6d5 涨到 0xaa4，嵌套量词路径指令 +4.3%/cycles +8.6%（`run` 是递归 VM 的核心，栈帧变大直接变慢）；抽出 `run_loop` 并 `noinline` 后回到 +0.1% 指令/−0.7% 时间——与 HANDOFF §6.4「gcc 内联扰动」同款。
- **未做（记录方向）**：DFA/JIT 本轮不做；**灾难性回溯记忆化**（位置×状态去重）未做——要保证与 CPython 的捕获组/贪婪口径一致，需要把 caps 快照纳入去重键（否则会漏掉「同一状态不同捕获」的分支），内存与语义风险都超出本轮，保留 2 亿步上限兜底；回溯 VM 仍是递归实现，深模式/量化零宽断言（如 `\b+`）依旧会吃栈——fuzz 里 `re.findall("\\b+", "ab")` 在 **base 与当前版本同款 SIGSEGV**（既有问题，非本轮引入，另票）。
- **验证**：行为零变化——**随机差分 fuzz**（/tmp 生成器，6 个 seed × 60 个随机模式 × {search/match/fullmatch/findall/split/sub}，覆盖字符类/量词/分组/交替/锚定/内联标志/贪婪惰性）：base 与当前输出**逐字节相同**（每 seed 250+ 行断言，两边 rc 均 0）；新增 tests/run/234_re_opt.py（82 行断言，先对拍 CPython 再钉 expect，覆盖字面量子串快路径、IGNORECASE 折叠、首字节剪枝、`\A`/`^`/MULTILINE、`.*`/DOTALL、`X*Y` 一阶剪枝、`$`/`\b` 收尾、空匹配尾巴轮、Pattern 惰性分析、后向引用/断言保守回退、缓存多模式轮换）；difftest **229/0**；普通与 ASan 套件各 **247 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 234_re_opt / 13_gc 全干净且输出与 expect 一致；0.271 oracle 语料对比（229 文件）：23 个 rc 差全为时代差（201-203 推断、207-215 cpy/cffi、220-229 vthread，oracle 编译不了；206 oracle SIGSEGV 为 0.312 已记录现象），输出级差异只有 230_tuple_sort / 231_float_dict（0.312/0.313 自身的修复），其余逐字节一致；规模检查仍只有原有 5 处超限，rt_re.c 2217 行（运行时文件不在 src/ 的 1200 行规则内），新文件 bench/re/bench_re.py 264 行 / tools/rebench.py 108 行 / tests/run/234_re_opt.py 238 行。

## [0.318] - 2026-11-19

> jsonopt 分支（基于 0.311 e855662），与 0.312/0.313 并行开发；集成时 rebase 到
> master，冲突面预期仅 rt_json.c（独立文件，0.312 fixdict 未触及）+ CHANGELOG 同位追加。

### 健壮化：json 对拍 CPython 语义（审计矩阵 114 案，修 9 类真实差异）

- **审计方法**：逐案对拍 CPython 3.14.4（键强制转换 / ensure_ascii / indent / separators / allow_nan / float 最短往返 repr / loads 严格性 / 错误消息与位置 / 嵌套深度 / int64 边界 / 重复键 / 控制字符）。float repr 另对 339 个值（含 300 个随机 double 位模式）逐字节对拍，零差异（format_double 本就符合 CPython repr 规则）。重复键 last-wins、控制字符严格拒绝、NaN/Infinity 默认输出原已正确。
- **修复 1（已知洞）dumps dict 键强制转换**：旧实现编译期拒绝非 str 键（"json dict keys must be str"），CPython 自动转 str。描述符文法 (d<v>) 扩为 (d<k><v>)（k=s/i/f/b/n，与桥的 cpy_desc 文法同形但解析器各自独立，rt_cpy.c 未动）：int→十进制、float→floatstr（repr，NaN/Infinity/-Infinity）、True/False→"true"/"false"、None→"null"。sort_keys 按 CPython 口径在转 str **之前**按原键排序（int/float/bool 数值序，{10,2}→{"2","10"}）。loads 样例键仍必须 str——CPython loads 永远返回 str 键，不做逆向转换（编译期明确报错）。
- **修复 2 ensure_ascii kwarg**（默认 True 不变）：False 时 >=0x80 字节原样输出（控制字符/引号/反斜杠仍转义）。同时修 True 口径的 DEL(0x7f) 漏转义洞（CPython ESCAPE_ASCII 是 [^\ -~]，0x7f 在范围外，旧实现原样输出）。
- **修复 3 separators kwarg**：tuple[str, str]（字面量或变量均可，静态类型检查），默认紧凑 (", ", ": ") / 缩进 (",", ": ") 与 CPython 一致；元素数不对时运行期消息对齐 CPython 的 unpack ValueError。自定义分隔符可含非 ASCII（CPython 原样插入，不走 ensure_ascii）。
- **修复 4 allow_nan kwarg**（默认 True 不变）：False 遇 NaN/±Inf（值或 float 键）抛 ValueError "Out of range float values are not JSON compliant: nan/inf/-inf"，消息逐字对齐 CPython。
- **修复 5 indent=0/负数语义**：CPython indent=0 输出换行但不缩进、负数同（' '*负数==''），只有 None 紧凑；旧实现把 <=0 一律当紧凑。用 INT64_MIN 哨兵表示 None（lower_json 对 indent=None 特判），0/负数走换行路径；indent>65536 拒绝（防巨量缩进挂死）。
- **修复 6 loads 严格数字文法**：旧 jp_number_token 把 [+\-0-9.eE] 一锅端给 strtoll/strtod，接受 CPython 拒绝的 +1/01/00/0123/.5/1./0./十六进制浮点/小写 inf/nan。改为严格 JSON 文法 -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)? 的最长前缀扫描：非法尾缀不消费，由外层报 Extra data / Expecting ',' delimiter——与 CPython 扫描器的接受集/错误位置逐案对齐（0e0 合法、1e 只扫出 1）。int 样例遇浮点 token 仍按类型口径报错（CPython 返回 float，静态样例设计差异）。
- **修复 7 语法错误消息对齐 CPython JSONDecodeError**："what: line L column C (char N)"（N 为 0 基字节偏移），覆盖 Expecting value / Expecting ',' delimiter / Expecting ':' delimiter / Expecting property name enclosed in double quotes / Unterminated string starting at（指向开引号）/ Invalid control character at / Invalid \escape（指向反斜杠）/ Invalid \uXXXX escape（指向 u，低代理坏 hex 指向低 u，与 CPython 一致）/ Extra data / Illegal trailing comma before end of array|object（3.14 新消息，指向逗号）。pyadamas 无 JSONDecodeError 内置名，按桥的映射口径抛 ValueError。类型导向失配（样例类型 vs 文本类型，CPython 无对应错误）保留 "json: ... at offset N" 格式。
- **修复 8 int64 边界口径**：loads 溢出抛 ValueError "json: integer out of int64 range"（不回绕；CPython 大整数无限精度，政策差异钉进 232）。±2^63 边界值正常。
- **修复 9 前端接线**：typecheck json_call 与 lower_json 接受 indent/sort_keys/ensure_ascii/separators/allow_nan；运行期签名 pa_json_dumps(v, desc, indent, flags, seps)，flags 打包 bit0 sort_keys / bit1 ensure_ascii / bit2 allow_nan（IR Bin 位运算合成，常量折叠后默认 0b110），JsonDumps 分组 i4→i5。
- **政策差异（记录不修）**：① 嵌套深度受 parser kMaxExprDepth=200 限制（loads 样例与 dumps 值都须静态类型字面量，编译期即拒；CPython 3.14 C 扫描器实测接受 20000+ 层）——静态样例设计的固有边界，运行期描述符深度因此有界，无需运行期深度守卫；② 孤立 \ud800 代理拒绝（pyadamas str 是 UTF-8 字节串无法表示；CPython 接受含代理码点的 str）；③ indent=str（如 '\t'）/skipkeys/default/cls/check_circular 不支持（编译期报错）；④ float NaN 键的 sort_keys 顺序 CPython 本身未定义，视为相等；⑤ {1:'a',True:'b'} 的键折叠是 CPython dict 语义（True==1），pyadamas 静态键类型不混用。
- **验证（健壮轮）**：difftest **228/0**；53/82/232/208 对拍 expect 全过（232 O0/O1/O2 一致）；桥 207–211 全过（rt_cpy.c 未动，描述符解析器独立）；普通与 ASan 套件各 **246 ran ok / EXIT=0**（244+232+233；233 是优化轮的正确性哨兵，提前引入，在未优化运行时上同样通过——优化轮以它为基线哨兵）；ASan detect_leaks=1 抽查 232/53/82/208 全干净；审计矩阵 99 MATCH，剩余 DIFF 全为上述政策项。新增 tests/run/232_json_hard.py（expect 钉自 CPython 3.14.4；int64 溢出/孤立代理两段为 pyadamas 政策口径，注释标明）。tools/difftest.sh 的 *_perf 跳过改为只跳 180_codepoint_perf（233 只钉正确性不打印时间，参与差分）。


### 优化：json 吞吐 −57%（测量先行，输出逐字节不变）

- **取证（perf record dwarf，heavy bench = 200 用户 str-dict + 50×20 嵌套 int dict + 2000 纯 int 列表，各 dumps+loads 2000 轮）**：jb_raw 14.9%（逐字符容量检查+memcpy+NUL store）、snprintf 族 ~17%（int 的 %lld：__printf_buffer 9.9 + _itoa_word 2.1 + 驱动）、jp_string 9.9%（逐字符解析+每串 JBuf malloc/free）、jb_str 9.4%（逐字符转义判断）、memmove 7.7%、strtoll 4.3%、malloc/free churn ~5%。与「str 是 tracked 叶子、json 大量临时 str」的既有发现同族：rc_alloc_core 仍占优化后 4.8%（每个键/值一个 str 是 loads 的固有分配，json 侧能做的是减少**额外**分配与扫描）。
- **优化（全在 rt_json.c，行为零变化）**：① 缓冲追加热路径——jb_grow 冷路径 outline（noinline），jb_raw/jb_ch 内联、小段（<=8B）循环拷贝免 PLT、去掉每次追加的 NUL store（str_alloc_raw 分配自带终止符，中间态无人把缓冲当 C 串读）；② jb_str 与 jp_string 的干净段成段搬运（ensure_ascii=False 时 >=0x80 字节也入段）；③ int64 手写 itoa（jb_i64，INT64_MIN 用 -(v+1)+1 取绝对值）与 \uXXXX 手写 4 位小写 hex（jb_u4），替换 snprintf；④ loads 字符串两遍结构——第一遍只校验+统计解码长度（**不分配**），第二遍 jb_grow 一次到精确容量再解码；无转义串直接从输入切片建 str，全 ASCII 用 str_new_ascii 免 str_seal 码点扫描（dumps 结果同口径：ensure_ascii+默认分隔符保证全 ASCII）；⑤ jp_int 手写 uint64 累加替换 strtoll+token 拷贝（19 位数字 < 2^64 不回绕，limit 判边界，-(int64_t)(u-1)-1 安全表示 INT64_MIN）；⑥ loads list/tuple 的 pa_list_append 传描述符静态 kind（0/1）作 heap 提示，免逐元素 pa_unknown_is_heap 探针（0.303 内部 append 同款口径）；⑦ jb_nl 缩进 memset 批量。
- **泄漏修复（ASan detect_leaks=1 抓出，优化轮自己的洞自己修）**：单遍快路径版 jp_string 边解码边建 JBuf，非法转义抛异常时 malloc 缓冲随 longjmp 丢失——**cleanup 属性不在 longjmp 时执行**（rt_internal.h 明言），232 实测漏 3×128B。两遍「先校验后分配」从结构上消灭错误路径分配（解码遍已校验、不再可能抛异常）；dumps 侧同族洞（allow_nan=False 写了一半抛异常丢缓冲）用 jb_check_nan 预检值树修复——保证抛异常时还没分配。只在罕见路径多走一遍，默认路径零开销。健壮轮状态与终态抽查均干净。
- **实测（绑核 24，交替 9 轮中位，A=0.311 基线快照编译器，B=本轮；两侧 rt.o 均 -O2、生成程序均 -O2）**：heavy bench **2091→891 ms（−57.4%）**，输出逐字节相同（cmp）；B 侧 9 轮 887–897 ms 极差 1.1%。分负载（交替 5 轮中位）：字符串密集 users **1241→570 ms（−54.1%）**、嵌套 int dict **332→142 ms（−57.2%）**、纯 int 列表 **550→204 ms（−62.9%）**。两遍解码在转义密集负载上比单遍 +3.4%（891 vs 862 ms），换错误路径零泄漏，接受。优化后热点回归固有成本：jp_value 15.8% / jb_str 10.4%（转义解码）/ jp_string 8.0% / jb_i64 6.4% / rc_alloc_core 4.8%。
- **中性（realistic 6 案，交替 7 轮打乱 BURST=5，绑核）**：queue_sim +0.3% / str_build +1.0% / rec_memo −0.3% / dict_wordfreq +0.6% / sort_struct −0.5%；obj_dispatch 中位数受多租户负载尖峰污染（两侧同现 350→650 ms 尖峰），**asm 逐字节相同 + perf stat instructions 3192.3M→3192.2M、cycles 923.1M→923.2M（±0.01%）**判中性（ quiet 轮两侧均 350–356 ms）。json 未参与 realistic 任何负载，rt_json.c 增大只动 rt.o 布局。
- **验证（终态，Release build-rel）**：difftest **228/0**（226+232+233）；普通与 ASan 套件各 **246 ran ok / EXIT=0**（244+2，含桥 207–211，PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 232/233/208_cpy_json/53/82 全干净；0.271 oracle 语料**零意外差异**（24 条已知「oracle 早于用例」rc 差：201–229 编译失败 + 206 已知 SIGSEGV + 232 新增；53_json/82_json_dump_opts 与 oracle 逐字节相同）；规模检查仍只有原有 5 处超限（rt_json.c 659→1111 行 < 1200，无新增超限文件）。
- **新增**：tests/run/233_json_perf.py（优化正确性哨兵：三类负载 dumps+loads 循环 + ensure_ascii=False/indent/separators/键转换路径抽查，只钉正确性不钉时间，~105 ms@O2，O0≈O2 因运行时主导；健壮轮已在未优化运行时上通过，优化前后输出逐字节不变）；bench/json/json_bench.py（取证/A-B 基准，2000 轮 heavy 版在 /tmp 会话记录）。

## [0.317] - 2026-11-19

### 正确性修复：P3 三件 + list[list] 自然排序（N6/N7/N9/同族）

- **N6（`dict(x=1, y=2)` 静态类型丢成 dict[unknown, unknown]）**：关键字构造只推 `dict[unknown, unknown]`，于是打印是 `<dict object>`、下标读回只能走 unknown 路径。修：typecheck 对「无位置实参 + 有关键字」的 `dict()` 返回 `dict[str, join(各关键字值类型)]`（键恒为 str）。同类型关键字值现在与字面量一致（`dict(a=1,b=2)` → `{'a': 1, 'b': 2}`）。
- **N7（`filter(None, 异构列表)` 保留空串 + 元素无 repr 引号）**：两层。
  ① 真值：`truth_bool` 对 unknown 直接返回原值，branch 只测「非零」，空串/空容器恒真。改为对 unknown 调 `pa_truth_unknown`（0/0.0/空串/空容器/None 为假，CPython 语义；CPyFloat 盒按 double 判零），`emit_truth` 的 unknown 分支同样走它。
  ② 打印：`print_elem` 对 unknown 元素改调 `pa_print_any_repr`（字符串带引号）。同时把 `PrintAny`/`PrintAnyRepr`/`TruthAny` 加进「索引必需」与「索引有人查」两张分析表 —— 原来程序只做 unknown 打印时索引会被判定「没人查」而只读化，字面量 str 探不到（打印成地址、真值恒真）。
- **N9（`sum(list[float])` 朴素累加）**：CPython 3.12+ 用 Neumaier（改进 Kahan-Babuska）补偿求和。运行时 `pa_list_sum_f64` 改成 Neumaier（AVX2 分批累加删除 —— SIMD 归约与 CPython 的串行补偿顺序不同，病态数据必然分叉）；降级层把「list/tuple 元素与累加器都是 float」的 `sum()` 一律路由到该入口（原来只有 `--ffast-math` 才走运行时，默认落到标量朴素循环，且 O0/O2 分叉）。实测 `sum([0.1]*10)=1.0`、`sum([1e16,1,-1e16])=1.0`、`sum([1e300,1e300,-1e300])=1e300` 全部与 CPython 逐位一致。
- **list[list] 自然排序（0.312 遗留的 kind-3 机器字）**：`sort_kind` 把 List 也映射到 kind 4（描述符 `l`+子描述符，运行时 `sort_desc_cmp` 早已支持），于是 `list[list[int]]`/`list[list[str]]`/`list[list[list[int]]]` 的 `sort()`/`sorted()` 从「比堆地址≈分配顺序」变成真字典序（前缀相等短者小）。
- **回归用例**：新增 tests/run/239_p3_minor.py（先对拍 CPython 再钉 expect，O0/O1/O2 全一致）：dict 关键字构造（int/str 值、下标读回、`dict()` 空、`.keys()`）、`filter(None, 异构)` + `bool(unknown)` 全形态、Neumaier 六个病态/常态求和、list[list] 四种形状的自然排序。
- **同时把三处「按旧行为钉的 expect」改成 CPython 口径**（本轮修复让它们与 CPython 一致，属预期变化）：`185_fnmatch` 的 `['a.py', 'c.py']`、`66_genexpr` 的 `('a', 'b')`、`162_pep479` 的 `[1, 'caught', 3]` —— 原来钉的是「unknown 元素打印无引号」的旧限制（docs/language.md §7 记录过）；`47_float_sum_order` 的期望值从朴素求和结果 `...343` 改成 CPython 的 `...345`。
- **验证**：difftest **232/0**；普通与 ASan 套件各 **250 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 239 / 238 / 13_gc 全干净；0.271 oracle 语料零意外差异；规模检查仍只有原有 5 处超限。
- **abbench（对 ec35fb6 realistic 绑核交替 15 轮）**：dict_wordfreq −1.0%、obj_dispatch +0.0%、queue_sim −1.2%、rec_memo +0.3%、sort_struct −0.1%、str_build −0.4%，合计 **1640.49→1635.32ms（−0.3%）**。**踩坑如实记录**：第一版把 `pa_truth_unknown` / `pa_print_any_repr` 写在 rt_core.c（rc_alloc/inc/dec 的家），abbench 立刻报 str_build **+3.7%**（p25 +3.5%）；把这两个函数搬到 rt_format.c、rt_core.c 逐字节复原后，同一负载回到 −0.4% —— 又一次「gcc 内联/布局扰动」（HANDOFF §6.4）：新增运行时函数不要往热文件里塞。
- **同批发现、未修（记录备查）**：① `statistics.mean` 仍是朴素求和（CPython 用精确有理数；`mean([0.1,0.2,0.3])` 仍差 1 ulp）—— 要对齐需要精确有理数累加（PyAdamas 只有 int64），记录为已知边界；② **生成器表达式的元素类型推断丢浮点**：`for v in (x for x in list[float])` 打印出的是 double 位模式（`4609434218613702656`），`sum(x for x in list[float])` 把元素转成 int 再累加（1.5+2.5 → 3）—— 这是独立于本轮的既有洞，单独立项；③ 异构 dict（值类型 unknown）仍打印 `<dict object>`（需要带类型标签的通用 repr，未做）。
## [0.316] - 2026-11-19

### 正确性修复：日常语料的四个静默语义差（N2/N3/N4/N5）+ try 里 return 的 RC 顺序

四条「不崩但答案不同」的语义差，加一个同族崩溃。五条最小复现现在都与 CPython 逐字节一致。

- **N2（文本写文件忽略 `newline=`，csv 默认写出 LF）**：`open(p,"w",newline="")` 写 `a\r\nb` 得到 `a\nb`，`newline="\r\n"` 不转换。**根因不在写侧**：`pa_file_open` 的实参被放在 `gen_rt_i4` 组（按 4 个寄存器发射），而运行时签名是 6 参 `(path, fd, mode, buffering, newline, closefd)` —— newline/closefd 从没进寄存器，运行时读到上一次调用残留（newline 恒 0 = 通用换行，写侧翻译因此从不生效；closefd 随机）。修：`FileOpen` 移到 `gen_rt_i6`。四种 newline 形态（None / "" / "\r" / "\r\n"）与读侧通用换行、`csv.writer` 默认 CRLF 全部对齐 CPython（csv 默认行终止符现在真的写 `\r\n`）。
- **N3（`getrandbits(k>=31)` 及 randrange/sample 序列分叉）**：`getrandbits` 原来是 `random()*2^k`（还钳到 62 位），CPython 是「k<=32 取一个 uint32 右移；否则按 32 位字逐字组装、最后一字右移」。randrange 用 `random()*n`、sample 用 `random()*(n-i)`，CPython 用 `_randbelow`（getrandbits + 拒绝采样）。逐条改成 CPython 算法：`mt_getrandbits` 按字组装（顺带修掉旧实现 k=64 时右移 32 位的未定义行为）、`randrange` → `_randbelow(width)`、`sample` → 3.11 的两条分支（小种群 `result[i]=pool[j]; pool[j]=pool[n-i-1]`；大种群已选下标集合重抽，门槛 `21+4**ceil(log(k*3,4))` 同 CPython）。**int64 边界**：结果保留低 63 位（PyAdamas 没有无符号/大整数），抽取字数与 CPython 完全一致所以后续序列不漂；`uuid4` 的 128 位因此是 63 位熵（合法 32 位 hex，已在 stdlib 文档声明）。
- **N4（浮点 `','` 分组与 `'#'` 交替形式被静默忽略）**：`fmt_parse` 解析了 `#` 却直接丢弃，浮点路径从不看 `grouping`。修：`PyAdamasFmt` 增加 `alternate` / `group_char`；浮点走 printf 的 `#` 标志（`#.0f` → `1235.`、`#.3g` → `1.00e+20`，repr 形态补小数点 `1e+20` → `1.e+20`）；整数 `#` 加前缀 `0x/0o/0b`（零填充插在前缀之后：`#08x` → `0x0000ff`，靠新的 `fmt_pad_parts` 前缀槽实现）；分组统一成 `fmt_group_n`（十进制 3 位、十六/二进制 4 位、`','`+非十进制按 CPython 报 ValueError）；浮点只给整数部分分组（`12,345,678.90`、`1_234.57`），指数形式与 inf/nan 不动。
- **N5（strftime 的星期/年积日与 naive 时区）**：`tm_wday`/`tm_yday` 从未计算（一直是 0）→ `%a/%A/%w/%u/%j/%U/%W` 全错；naive datetime 的 `%z/%Z` 被 glibc 填成本地时区。修：用 Howard Hinnant 的 `days_from_civil` 算出 `tm_wday`（1970-01-01 周四）与 `tm_yday`；`%z/%Z` 在格式串预处理里换成哨兵字符（正确处理 `%%` 与 `%E/%O` 修饰），strftime 输出后剥掉 —— naive 值现在与 CPython 一样是空串。
- **附带（同族崩溃，语料未覆盖）**：`try: return s + "!"` 这样的形态退出时 panic「refcount decrement on a freed object」。`Return` 原来先 `emit_cleanup_exits(0)`（清理会释放临时栈上方的值）再 `rc_temp_take` 返回值 —— 返回值本身就是其中之一，取到的是已释放对象。修：新增 `own_return_value`（新值 take / 借用值 inc，struct 走拷贝），在清理**之前**接管返回值，`emit_return` 加 `already_owned` 参数跳过重复接管。
- **回归用例**：新增 tests/run/238_daily_semantics.py（先对拍 CPython 再钉 expect，O0/O1/O2 全一致）：四种 newline 写形态 + 通用换行读 + csv 默认/自定义行终止符、getrandbits(8/16/31/32/63) + randrange/randint/sample 三形态/choices/shuffle/choice/uniform/random、浮点与整数的 `,`/`_`/`#` 组合（含 `#08x`、`#.3g`、`#_x`、`#_b`、`#` repr 形态）、strftime 全部字母指令与四个边界日期（含闰年、年积日 366）、try 里 return 新值/借用值/列表下标。
- **验证**：difftest **231/0**；普通与 ASan 套件各 **249 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 238 / 237 / 13_gc 全干净；0.271 oracle 语料零意外差异（201-229 共 23 例 oracle 编译 rc 差、230/231/236/237/238 新用例时代差、206 oracle 侧 SIGSEGV 既有现象，其余逐字节一致）；规模检查仍只有原有 5 处超限。
- **abbench（对 99c30e6 realistic 绑核交替 15 轮）**：dict_wordfreq +0.1%、obj_dispatch +0.1%、queue_sim −0.2%、rec_memo +0.2%、sort_struct −0.5%、str_build −0.5%，合计 **1639.28→1636.73ms（−0.2%）**——五处修复都不在六个热路径用例的代码路径上（FileOpen 参数表、random 采样算法、格式说明符解析、strftime、Return 的清理顺序）。
- **同批发现、未修（记录备查）**：`type(e).__name__` 对运行时内置异常一律返回 `"Exception"`（`except ValueError as e` 里拿不到真实类名）；`csv.writer.writerow` 传 `[str, int]` 混合列表会段错误（`list[str]` 形参收 `list[int]` 值的隐式转换洞，与 N17 同族但更深，需要元素级转换或编译期拒绝）。
## [0.315] - 2026-11-19

### 正确性修复：容器 RC 崩溃批（N10 / N16 / N17 / N18）+ set 原地运算

日常语料挖出的四个崩溃级 bug，同族但根因各异（都在降级器的容器所有权 / 原地语义上）。四条最小复现现在都与 CPython 逐字节一致，退出码 0。

- **N10（list 原地 `+=` / `*=` 段错误）**：`l: list[int]=[1,2]; l += [3,4]` → SIGSEGV（`*=` 同）。`AugAssign` 对所有类型一律降成 `IrOp::Bin`，而 list 的 `bin.add` 是「后端不支持」分支（显式 `list+list` 有编译期拦截，`+=` 从旁边漏过去），codegen 按整数加法把两个列表指针相加 → 野指针。修复：`+=` → `pa_list_extend`（typecheck 早已按 `list.__iadd__` 接受任意 iterable 的 list/Tuple 形态）、`*=` → 新增 `pa_list_inplace_repeat`（真·原地：CPython `__imul__` 改同一个对象，`a=l; l*=2` 之后 a 也变长；元素引用按元素种类补；n<=0 清空；kind2 位图保守丢弃）。
- **N16（`dict.setdefault(k, [])` 双重释放）**：退出时「refcount decrement on a freed object」。原降级是 `DictContains` → 分支 → `DictSet` → 再 `DictGet`，且返回的既有值**没有补引用**就 `push_temp` —— 临时栈按「持有」记账，所以 dict 里的那个对象被多 dec 一次；同时 key 的 `__hash__/__eq__` 被调用两次（CPython 只一次）。修复：改走 0.284 已实现的 `pa_dict_try_get`（一次查找、命中即把值写进帧槽），三处语义一起归位：命中路径把借用值补成新引用（`rc_inc` + `push_temp` + `fresh_vregs`）、未命中路径返回默认值本身（同一对象，dict 与新引用各持一份）、键只哈希一次。`out_slot` 是借用槽（不入 `slot_owned`），读完立刻写回 0，避免帧出口二次 dec。
- **N17（空 `set[str]=set()` 不认标注）**：动态串不去重（按指针比较）+ 退出 RC 崩。`set()`/`dict()` 的空构造调用只能推出 `set[unknown]` / `dict[unknown,unknown]`，AnnAssign 的标注（`set[str]`）没有传下去，运行时拿到 key_kind=3（指针比较）、元素 own=2，于是两个等值不同对象的串都留下、销毁时按注册表 dec 两次。修复：AnnAssign 对「裸 `set`/`frozenset`/`dict`/`list` 空构造 + 同种类标注」挂 `pending_ctor_ty_`，`lower_builtin_container` 取用该标注建容器。标注现在真正决定 key_kind / own_kind（`dict()` 空构造同病同修）。
- **N18（`sorted(key=返回堆 str)` 规模化双重释放）**：输出正确但退出 rc 1。key 物化循环 `f(e)` 的结果是 fresh 堆值（已在临时栈里），却只做了裸 `pa_list_append(keys, kv, heap_arg)` —— 既没 `move_fresh` 也没 `retain_elem`，而键表销毁时按元素种类 dec，于是每轮少一次引用 ⇒ 双重释放。修复：与所有容器写入同一条纪律——`store_list_elem(keys, list(kret), kv, kowned)` 先定型/补引用，再 append。**同族一并解决**：0.312 遗留的「key 返回元组崩溃」随之消失（`sorted(key=返回 tuple)` 现在走 0.312 已实现的 kind-4 字典序比较路径，结果与 CPython 一致）。
- **顺带（同族静默错，语料未覆盖）**：`s |= t` / `s &= t` / `s -= t` / `s ^= t` / `d |= t` 原来也落到 bin op——后端没有 set 的 bin 运算，`s |= {2}` 把哈希表指针当整数算，打出 `set()` 并 rc 崩。现按 CPython `__ior__` 语义分派：`|=`/`+=` → `pa_dict_update`（同 `set.update`），`&=`/`-=`/`^=` → 新增 `pa_set_intersect_update` / `pa_set_difference_update` / `pa_set_symdiff_update`（`^=` 对 t 单趟：交集删、差集补，只删不补会漏）；dict 只有 `|=`。非原地集合运算（`s | t`）仍按既有口径报「后端不支持」。
- **回归用例**：新增 tests/run/237_rc_container_ops.py（先对拍 CPython 再钉 expect，O0/O1/O2 全一致）：list `+=`/`*=`（别名可见性、tuple 右操作数、0/负倍数、堆元素重复、循环内反复追加）、setdefault 四条路径（链式 append、命中绑定、未命中返回同一对象、非字面量默认可变对象共享、defaultdict 对照）、空 `set[str]` 动态串去重 + `dict()` 空构造、`sorted(key=堆 str)` 2000 元素规模化 + 稳定性 + key=len + key 返回元组、set/dict 四种原地集合运算（含对称差双向、空集、堆元素）。
- **验证**：difftest **230/0**；普通与 ASan 套件各 **248 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 237 / 236 / 13_gc 全干净；0.271 oracle 语料零意外差异（201-229 共 23 例 oracle 编译 rc 差、230/231/236/237 新用例时代差、206 oracle 侧 SIGSEGV 既有现象，其余逐字节一致）；规模检查仍只有原有 5 处超限——**lower_stmt.cpp 因本批涨到 1242 行（新超限），已把 AugAssign 降级整体拆到新文件 src/lower_stmt_assign.cpp（约 90 行）**，lower_stmt.cpp 回到 1182 行，超限清单恢复为原 5 个。
- **abbench（对 f89112b realistic 绑核交替 15 轮）**：dict_wordfreq −0.7%（p25 −1.3%）、obj_dispatch −0.1%、queue_sim −0.3%、rec_memo −0.5%、sort_struct −0.0%、str_build +0.3%，合计 **1644.07→1640.33ms（−0.2%）**——四条修复都不在热路径上（setdefault/sorted(key=)/list += 语料里本就绕开），新增的 `pa_list_inplace_repeat` / set 原地运算只在对应语法出现时链接调用。
- **一条测量噪声的如实记录**：首次并行跑套件（同时跑 difftest）时 174_gc_construct_roots 出现一次 OUTPUT DIFF（`100005` vs `16390`），随后同二进制连跑 3 次 + 8 路并发都无法复现；该用例是「构造期对象不在根上」的 GC 压力用例（输出是分配量相关的整数和），本轮四个修复都不碰 GC 根扫描路径，按既有纪律记为负载/时序敏感噪声，未做代码改动。
## [0.314] - 2026-11-19

### 正确性修复：浮点累加器跨调用被踩（N19a 静默误编译，调用点 spill 掩码漏别名 load）

- **现象（O1/O2 错，O0 对）**：日常语料挖出的 N19a——函数内「浮点累加器 + 嵌套调用」循环静默算错，`det4(单位阵)` 返 0.0（应 1.0）、`det4(diag(1,2,3,4))` 返 0.0（应 24.0）、`det4(m4)` 返 5.0（应 0.0），不崩不报错。Heisenbug：同一份代码在模块级、整数版、以及被调函数能被内联时都正确，随寄存器压力变化；`PYADAMAS_NO_FPLIVE=1`（调用点全量 spill）后立刻正确——**指针直指调用点浮点 spill 掩码**。
- **根因（`gen_block` 的 fp_after 预扫描与 `gen_load_slot` 的别名优化交互）**：浮点局部变量提升到 xmm8-15 后，每个调用点按「块内 live-after」只 spill 还要再读的槽（0.279 起，spectral_norm 靠它）。`gen_load_slot` 对「单次使用 + 块内 + 读后无赋值」的 load **不发指令**，直接把 vreg 别名到槽寄存器；于是 `s = s + f(x)` 的真实形态是「load 在调用前（无指令）、readsd 在调用后」，而槽级活跃性把 **load 那一点**当成最后一次读：`store.slot s` 在块尾倒推时清位，调用点在 load 之后、store 之前，掩码里就没有累加器 → 被调用者（自己也在用 xmm8-15 提升）踩掉 → 静默算错。O0 不做浮点槽提升，所以 difftest（O0 vs O2）也是绿的——**这是 0.312/0.313 一路带过来的洞**。
- **修复（一处，+41 行，src/codegen_block.cpp）**：别名 load 的 **use 点**补成槽寄存器的一次读——预扫描里收集「单次使用 + 块内 + use 在 load 之后 + 两者之间无对该槽的 store（与 `gen_load_slot` 的 clobber 判定同口径）」的 load，把它的 bit 置在 use 点上，倒推扫描时 `cur |= 别名 use 位`，于是位在「use 之前、最近一次 store 之后」整段成立，区间内的调用点一律 spill/reload。原有的 load 置位、store 清位、块级 live_in/out（跨块读）**逐条不动**，未别名的 load（被融合、跨块、多次使用）不置位。没有调用、或调用点不在「load→use」区间的块，生成的汇编逐字节不变（abbench 实测中性）。
- **回归用例**：新增 tests/run/236_fp_spill_call.py，先对拍 CPython 再钉 expect，O0/O1/O2 全一致。覆盖：最小形态（递归被调防内联的累加循环，acc(0/1/5/9)）、原报 4×4 余子式（unit/diag/m4 三个行列式 + minor/det3 中间量）、累加器在 if/else 两支里读而调用在支外（跨块掩码）、同块两个调用夹住累加器且只在最后使用（最紧形态）、store 之后继续累加、整数累加器对照。**用改为修复前的 `codegen_block.cpp` 构建验证过：该用例 7 行输出与 CPython 分叉（1.0→2.0、24.0→0.0 等），修复后逐字节一致**。
- **验证**：difftest **229/0**；普通与 ASan 套件各 **247 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8）；ASan detect_leaks=1 抽查 236_fp_spill_call / 13_gc / 88_defaultdict 全干净；0.271 oracle 语料零意外差异（201-229 共 22 例 oracle 编译 rc 差、230 真排序/231 float dict/236 新用例的时代差、206_list_store_kind oracle 侧 SIGSEGV 为既有现象，其余逐字节一致）；规模检查仍只有原有 5 处超限（codegen_block.cpp 1112→1153，无新增超限文件）；**abbench 对 2aae2ac realistic 绑核交替 15 轮**：dict_wordfreq +0.7%（p25 +1.3%）、obj_dispatch −0.0%、queue_sim +0.2%、rec_memo −0.2%、sort_struct −0.0%、str_build −0.2%，合计 **1643.68→1645.06ms（+0.1%）**——掩码只多了「别名 load 的 use 点在区间内」这一种置位，六个热路径用例的生成代码不变。
- **同批未改（既有设计，均在此之前就存在）**：`xmm3-7` 块内浮点临时池与 GPR 池的门控（`fp_temp_op_ok`）本轮未动；`PYADAMAS_NO_FPLIVE=1` 仍保留为排障开关（全量 spill，天然正确）。
## [0.313] - 2026-11-19

### 正确性修复：float 值 dict 的 get/下标读回损坏（fixdict 分支合入，0.312 之后）


- **现象（O0/O1/O2 同病）**：`d: dict[str,float]={}; d["x"]=1.5` 后 `d.get("x",0.0)` 返 0.0（存在键返默认值）；非字面量 float 经下标「存储后读回损坏」（`lat_sum[path]=lat_sum[path]+lat` 累加失效，读回 0/-nan/垃圾小浮点）；`pop`/`set.pop` 读回垃圾。字面量初始化 dict 的直接读+算术「看似正常」只是寄存器残留恰好等于该值。int 值 dict 完全正常。
- **根因（单一，读路径）**：dict 取值运行时签名统一 `int64_t pa_dict_*(...)`——float 值按 IEEE-754 位模式存在 `entry.val`、原样从 **%rax** 返回；而 `gen_call_rt`（src/codegen_call.cpp）对静态类型 float 的结果一律按 double 读 **%xmm0**。`raw_ret` 白名单此前只有 DictKeyAt/JsonLoads（同款 bug 的两处前科，见该处注释），dict 取值全家漏网。于是：① get 的 const.float 默认值经 %xmm0 物化，调用后 %xmm0 恰好还是它 → 「存在键也返默认值」；② 下标读回的是 %xmm0 残留（上次浮点物化/运行时调用的 leftovers）→ 读回 0/-nan/垃圾。**存储路径逐位正确**（DictSet 实参走 movq 位模式；O2 融合 DictTryGet 由运行时 movq 直写帧槽、load.slot 按位读回，实测本来就正确）——「存储损坏」实为读损坏喂给累加再存储的二次污染（O0/O1 无融合，累加循环因此把垃圾写回表；O2 融合路径的累加本身正确，坏的是融合外的 DictGet 读）。typecheck 对 dict[str,float] 的 get 返回类型推断正确（float），无需改。
- **修复（一处，+16/−1 行）**：`raw_ret` 白名单补齐 dict 取值全家：DictGet（下标读/setdefault）、DictGetOr（get 带默认）、DictGetOpt（get 单参，ty=Optional 实为防御性纳入）、DictPop（pop）、DictPopLast（set.pop）。float 结果走既有 `movq %rax,%xmm0` + move_fp 落位；非 float 结果 movq %rax 与原行为逐位相同（int dict 零影响）。opt_dict_bump 只匹配 int/unknown 加法，float 不融合，无交集。
- **验证**：新增 tests/run/231_float_dict.py（get 存在/缺失/默认、非字面量默认、单参 get 返 None、非字面量 float 下标存储读回（含后续算术改 xmm0 残留）、contains+get 与 get 默认值两种累加循环、float 覆盖、推导/拷贝、pop/setdefault、values/items 迭代、跨函数借用读、defaultdict(float)、float 键、set[float].pop 成员资格、int dict 对照、40 键 reindex 大表、f-string 格式化），先对拍 CPython 再钉 expect，O0/O1/O2 全一致。difftest **227/0**（Debug 与 Release 各一遍）；Release 普通与 ASan 套件各 **245 ran ok / EXIT=0**；ASan detect_leaks=1 抽查 231/13_gc/88_defaultdict 全干净（--asan 插桩确认、输出与 expect 一致）；0.271 oracle 语料零意外差异（244 文件：201-229 共 23 例 oracle 编译 rc 差、206 oracle SIGSEGV 为 0.303 新增用例的时代差、231 oracle 带本 bug 输出差——均已知预期，其余逐字节一致）。Debug（-O0 dev）套件 118/154 两失败为既有 Debug 断言（0.308 已记录、另票），已在**未改动的基线 Debug 构建**上复现确认与本轮无关；180_codepoint_perf 负载敏感，Release 套件（含 ASan）内通过，Debug 低并发单跑亦过。
- **已知边界（不在本轮）**：`d.get(k)` 单参对「存储的 0.0/0」与「缺失返 None」不可区分（None 就是 0 的既有表示设计，int dict 同款）；`d.get(k, 3)` 的 int 默认值按静态值类型转 float（CPython 原样返回 int）——两者均为先于本轮的语义设计，非损坏。

## [0.312] - 2026-11-19

### fix: 元组列表排序完全是 no-op（元素字典序比较）

- **取证**：/tmp/iso3.py——list[tuple[int,int]] / list[tuple[str,int]] / list[tuple[int,str]] 的 .sort() 与 sorted() 全部原样不动（O0/O2 同），int/float/str 标量列表正常。根因是两层分派缺口：前端 sort_kind()（lower_impl.h）只认 Int/Bool→0、Float→1、Str→2，**元组落 3（机器字比较）**；运行时 pa_list_sort（rt_list.c）只有 LESS_I64/F64/STR 三个 timsort 宏实例化，kind 3 走 sort_i64 **比堆地址**——池分配地址≈分配顺序≈原顺序，排序看起来是 no-op（实为指针序 timsort，对顺序分配的数据退化成单自然 run）。**长期被顺序无关测试掩盖**：sort_struct 排 list[tuple[int,int,int]] 3900×500 却只 aggregate 求和；204_list_buf_pool 的元组排序校验和同样顺序无关（原注释甚至写明「元组排序按机器字（指针）序……既有语义」）——排序坏了输出仍对，本轮已把该过时注释改写。
- **修复（kind 4 + 编译期描述符 + 运行时字典序）**：① sort_kind 对 Tuple 返回 4；② 新增 Lowerer::append_sort_desc 按静态类型生成自定界前缀树描述符（intern 串常量，永生）：i/f/s/x = int/float/str/机器字，l+子描述符 = list/变长元组（逐元素字典序、前缀相等短者小），t+arity 字节+逐位置描述符 = 定长异构元组（逐位置比）；病态深度>12/长度>400/arity>255 整体退 x（机器字，即修复前行为）；③ 运行时新增 sort_desc_cmp 三路比较（游标纪律：返回 0 保证消费完当前描述符，非 0 逐层回退；depth≥32 防御退化；NaN 同 LESS_F64 口径与一切相等；str 复用 pa_str_cmp）+ 第四个 timsort 实例化 PYADAMAS_TIMSORT(cmp, LESS_CMP)；④ pa_list_sort/pa_sorted_list/pa_list_sorted_by 三签名尾追加 desc 参数（标量种类传空串、不读），codegen ListSort/SortedList 从 i2 组移 i3 组，ListSortedBy 留 i4 组（4 参恰满）；⑤ kind-4 key 路径（sorted(key=) 返回元组）在 cmp_sort_by_idx 相等 key 退回原下标——qsort 不保证稳定，钉住稳定性（CPython 口径）。排序重排不改所有权，kind2 位图保守丢弃沿用 0.303；比较纯 C 无分配，排序期间不执行用户代码，缓冲池纪律不变；标量 kind 0/1/2 热路径逐字节未动。
- **范围**：仅元组元素走 kind 4；list[list]/unknown 元素仍机器字（kind 3，修复前行为，语料无依赖）。.sort(key=/reverse=) 仍报不支持（既有口径）；sorted(reverse=True) 走既有「排完整体反转」路径，天然生效。
- **实测**：iso3 三种元组形状 + 无标注推断（list[tuple[int,str]]）O0/O1/O2 全部真排序且与 CPython 一致；新增 tests/run/230_tuple_sort.py（30 行输出断言：同质 int/str 元组、混合 int+str/str+int、float 元组、变长不同 arity、reverse=True/False、int/str key 函数、稳定性（key 相等保持原相对顺序）、三层嵌套元组、空表/单元素、降序/近乎有序输入、三元组（sort_struct 同款）、tuple 输入物化、300 元素大表触发 timsort 归并+临时区入池）O0/O1/O2 对拍 CPython 全一致。**abbench 对 e855662 realistic 15 轮（交替、打乱、绑核 24，负载 ~5.6）**：sort_struct **115.85→473.82ms（+309%）**、dict_wordfreq **272.55→293.59ms（+7.7%）**——**这是正确性的代价，如实记录**：基线的「排序」是 no-op（指针序单自然 run，O(n)），现在真排 3900×500 / 12000×16 个元组（O(n log n) 字典序比较），增量 ~358ms 与 17.5M 次比较×~20ns 的数量级吻合；其余四项 obj_dispatch +0.3% / queue_sim −0.4% / rec_memo +0.0% / str_build −0.7% 均在噪声带内（无布局回归）。合计 1270.05→1648.57ms（+29.8%，全由前两项贡献）。sort_struct/dict_wordfreq 输出与 0.271 oracle 逐字节相同（1948572072 / 36000，和顺序无关）——**和不变，排序真生效**。
- **验证**：difftest **227/0**；普通与 ASan 套件各 **245 ran ok / EXIT=0**（PYADAMAS_TEST_JOBS=8 避负载敏感用例）；ASan detect_leaks=1 抽查 230_tuple_sort / 13_gc 全干净；0.271 oracle 语料对比除已知预期项外零差异——201-203/207-215/220-229 共 22 例新功能 rc 差（oracle 编译不了，rc 1 vs 0）、230 新用例（oracle no-op vs 本轮真排序，输出差为预期）、**206_list_store_kind oracle 侧 SIGSEGV（rc 139 vs 0）为既有现象**：0.271 二进制（11-18 冻结）+ 0.303 后未改的源，单跑 3/3 确定性复现，lead rc=0 输出正确，与本修复无关（本轮未触碰 kind 3 路径），如实记录；规模检查仍只有原有 5 处超限（ir_ops.h 1276→1277、lower_impl.h 1242→1257 均在既有名单内；lower_expr_ops.cpp 720、rt_list.c 1186，无新增超限文件）。
- **遗留（既有 bug，本轮发现不修）**：**sorted(key=) 的 key 函数返回元组时崩溃**——最小复现：`k=[(1,9),(2,1)]; def ident(t: tuple[int,int]) -> tuple[int,int]: return t; print(sorted(k, key=ident))` → 「runtime error: refcount decrement on a freed object」（rc 1）；**0.271 oracle 与 e855662 基线同款复现**（三处一致），int/str key 正常、普通元组返回函数正常——key 物化循环对堆元组调用结果的 RC 处理是另一个既有 bug，230 用例以 int/str key 绕行；kind-4 key 比较路径已实现（含稳定性兜底），待该 RC bug 修复后启用。另：oracle 侧 206 SIGSEGV 同上，建议另立票。
## [0.311] - 2026-11-19

### 编译吞吐：直调 as/ld（build 全程 −9.4%，累计对 0.309 −11.6%）

- **取证**：build 的墙钟按模式阶梯拆开（无参启动 / check / ir / asm / build，绑核 11 次中位）：nbody 启动 2.4 + 类型检查 1.5 + 降级+优化 3.0 + 代码生成 3.3 = 前端 10.2 ms，build 54.9 ms ⇒ **cc 占 44.7 ms（81%）**；dict_wordfreq 41.4/48.3 ms（86%）；含 6 个 stdlib 模块的 140_stdlib_batch 124.1/134.5 ms（92%）。strace -f -e execve 显示每次 build 的 cc 链路是 `pyadamas → sh → cc → as`（汇编）与 `pyadamas → sh → cc → collect2 → ld`（链接）——**双重 fork/exec + sh**；`cc --version` 单独就 3.0 ms（/bin/true 1.4 ms），链接期 gcc 驱动（specs 解析/库搜索/两次 exec）约 11 ms，ld.bfd 25 ms。
- **优化（配置期复刻 gcc 命令行，运行期直调）**：CMake 在 configure 时用 `-print-file-name` / `-print-prog-name` / `-print-search-dirs` 探测出 gcc 实际传给 as 与 ld 的命令行（crt*.o、-L 目录、-pie -z now -z relro --hash-style=gnu --build-id --eh-frame-hdr、装载器路径、-lgcc/-lgcc_s/-lc 次序），编成 PYADAMAS_AS_EXECUTABLE / PYADAMAS_LD_FLAGS / PYADAMAS_LD_TAIL；driver 运行时直接 exec **as --64 -c** 与 **ld**（省掉 cc 驱动与 collect2 两层）。新文件 src/driver_link.cpp（71 行）+ driver_link.h，driver_build.cpp 只加 ~10 行接线。**五重回退保行为不变**：探测失败 → cc；PYADAMAS_CC_FLAGS 非空（--asan 等）→ cc；目标非 x86_64-sysv（Win64 交叉汇编）→ cc；link 参数含 .c/.其它非产物（cimport 的 structabi/callbacks/c_float32/cdecl/cfi 会现编 C 源）→ cc；直调 ld 失败 → 再走 cc 一次。环境变量 PYADAMAS_NO_DIRECT_LD=1 可整体关闭（A/B 与排障用）。
- **踩坑（都被验证抓住）**：① .interp 必须逐字节等于 gcc 写的 /lib64/ld-linux-x86-64.so.2——用 -print-file-name 得到的 /usr/lib/gcc/.../../../../x86_64-linux-gnu/... 会长 0x34 字节，段偏移全变（cmp 立刻发现）；② 链接参数里的 .c 源文件 ld 当链接脚本读，套件 108_structabi/112_callbacks/215_c_float32/cdecl/cfi 五项 OUTPUT DIFF，加 direct_ld_args_ok() 过滤后全绿；③ 直调 as 在带 -fsanitize 时会被喂无法识别的选项，故 PYADAMAS_CC_FLAGS 非空一律回退。
- **实测（交替 A/B、打乱顺序、绑核 24，7 轮中位；A=0.310 二进制，B=本轮）**：build -O2 --rebuild 11 个负载合计 **676.49→613.14 ms（−9.4%）**，单项 −7.3%~−10.9%（nbody −7.3%、spectral −10.5%、rec_memo −10.6%、queue_sim −9.5%），含 6 模块的 stdlib_batch −8.0%、stdlib54 −10.9%；**对 0.309 原点累计 711.79→629.56 ms（−11.6%）**。前端 asm 未动（10 负载合计 69.63→69.20 ms，−0.6%，噪声内）——本轮省的全是编译器之外的进程链。
- **验证**：行为零变化——21 个 bench/语料可执行文件「直调 as/ld」与「PYADAMAS_NO_DIRECT_LD=1 走 cc」链接结果 **cmp 逐字节相同**（含 .note.gnu.build-id）；243 个语料的 asm 输出与 0.309 逐字节一致；difftest **226/0**；普通与 ASan 套件各 **244 ran ok / EXIT=0**（--asan 走 cc 回退，插桩链接不受影响）；0.271 oracle 语料零差异（仅 201-229 新增用例 rc 差为已知预期）；规模检查仍只有原有 5 处超限（driver_build.cpp 1438→1461 行仍在既有名单内，新文件 driver_link.cpp 71 行）。
- **同一轮撤回的尝试**：gen_block 预扫描的两张 unordered_map（block_stores_/block_first_use_）改扁平数组 + 脏列表，calibrate 后 asm 合计 69.28→69.07 ms（−0.3%，噪声级），确认分配点不是当前瓶颈后退回（diff 保留在会话记录，不进仓库）。
## [0.310] - 2026-11-19

### 编译吞吐：静态 libstdc++ + 两处 O(n²) 编译期扫描（前端 −12.9%，语料 check 逐文件 −23.1%）

- **阶段占比（先量化，Xeon Gold 6252 绑核 24，多租户负载 3-5；单次 build 总耗时占比）**：cc 汇编（cc -c）≈ 17%（dict_wordfreq 8.2/51 ms），**链接（cc -o）≈ 70%**（36/51 ms：ld.bfd 25 ms 不可压缩 + gcc 驱动 11 ms），编译器前端（词法/解析/类型检查/降级/IR 优化/codegen/渲染）≈ 19%；逐阶段 PYADAMAS_TIMING：check 0.2-0.5 / lower 0.6-7.1 / asm 0.7-6.3 / cc 5.3-11.2 ms。语料批处理（tests/run 227 例逐文件 check）则是**进程启动占主导**：单进程 3.87 ms 里 1.24 ms 是动态装载器解析 libstdc++.so.6 的 ~800 个符号（LD_DEBUG=statistics：1972 次重定位 / 43 万拍，其中 1720 次来自符号查找缓存），语料本身平均只占 ~0.6 ms。perf（--children，整条 build 链）证实：链接期 bfd_link_hash_traverse 12.7% / bfd_elf_link_add_symbols 5.1% / bfd_hash_lookup 4.9%（全部在 ld 内；-z now、PIE、reflink、--threads（bfd 不支持）、strip 局部符号、输出到 tmpfs、-O1 逐项 A/B 均为 0 或不可用，本机无 lld/mold）；前端则 _int_malloc + _int_free ≈ 13%、assign_vreg_slots 6.3%、汇编行重解析（parse_instr + parse_operand）5.5%、rebuild_use_def 4.5%、std::vformat 2.1%。
- **优化 1（链接/启动，一行）**：-static-libstdc++（新 CMake 选项 PYADAMAS_STATIC_LIBSTDCXX，默认开）。编译器每次被调用都要装载 6.9 MB 的 libstdc++.so.6 并解析其符号；静态链接后进程启动 **5.01M→3.19M 条指令（−36%）、43.1 万→31.4 万拍（−27%）**，tests/run 逐文件 check 227 例 473.9→365.9 ms。不改生成代码/运行时（用户程序仍动态链接），二进制 5.2→7.0 MB（页缓存只读）。
- **优化 2（codegen）**：AsmGen::assign_vreg_slots 的跨块判定原本是「每个 vreg 扫全函数每条指令的 a/b/c/args（含内层 lambda 分配）」= O(vreg × 指令)，nbody 的 simulate/advance 各 3000+ 指令 × 4000+ vreg；改成一趟顺扫：对每条指令的每个操作数，若「所在块 ≠ 定义块」则置 cross_block_[r]。语义与原实现**逐位相同**（原来是「命中第一个非定义块的使用点即置位，之后保持」），243 个语料 asm 逐字节比对确认零差异。
- **优化 3（opt）**：dce_function 每轮 rebuild_use_def() 重建三张 unordered_map（inst_by_id/def_of/uses_of，逐指令哈希插入 = profile 里 _int_malloc 与 _Map_base::operator[] 的大头），而 DCE 只需要「某 vreg 被读了几次」。改为一次性线性 vector<uint32_t> 计数 + 删除时精确递减（删纯指令只会让计数变小，等价于下一轮重建），并保留外层不动点循环，保证「删除后**前面**已扫过的指令变死」的连锁与旧实现一致（首版漏了这一点，32_exception_value / 39_match_advanced 的 asm 多留一条 const.int -1，被逐字节比对抓出后修好）。
- **实测（同负载交替 A/B、打乱顺序、绑核，7-9 轮中位）**：前端 asm（10 个负载合计，排除 cc）**79.21→68.99 ms（−12.9%）**，单项 −11.1%~−14.6%，其中 −6.2% 来自静态 libstdc++（启动）、−4.8% 来自两处 O(n²) 扫描；build -O2 --rebuild（11 个负载合计，含 cc 汇编+链接）**654.3→633.9 ms（−3.1%）**，单项 −1.6%~−4.4%（前端只占 ~19%，故绝对值有限），stdlib_batch −3.8%、stdlib54 −3.8%；语料逐文件 check（bash 循环 227 文件）**474.8→365.1 ms（−23.1%，2.09→1.61 ms/次）**，--imports 525.2→412.8 ms（−21.4%）。
- **验证**：行为零变化——243 个语料（bench realistic/compare 11 + tests/run 227 + 其它）的 pyadamas asm 输出与 0.309 逐字节一致；difftest **226/0**；普通与 ASan 套件各 **244 ran ok / EXIT=0**；ASan detect_leaks=1 抽查 13_gc/88_defaultdict/192_dict_cache/66_genexpr/119_gen_except_suspend 全干净；0.271 oracle 语料零差异（build/release 未触碰；201-229 等新增用例 rc 差为已知预期）；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475→2477 行、driver_build.cpp 1438、ir_ops.h 1276、lower_impl.h 1242、typecheck_types.h 1329，无新增超限文件）。
- **未做（如实记录）**：链接占 build 总耗时 ~70%，其中 ld.bfd 的 25 ms 由「gcc 链接 rt.o（2064 个符号、~350 段、17 MB .bss）+ -z now 全符号绑定」决定；能砍掉它的只有换链接器（本机无 lld/mold，实测 clang -fuse-ld=lld 36→23 ms，但 /usr/bin/ld 只有 bfd 与 ld.so，lld 未安装）或缩 rt.o（运行时职责，超出前端范围），故本轮不硬优化前端之外的部分。
## [0.309] - 2026-11-19

### release preset -O1→-O2（保守退路）

- 0.308 的 release -O1 经三层吞吐测量中性，用户选择保守 -O2：构建收益仍有约 −10%（compile-speed.md 实测 −10.1%），编译器二进制运行余量更足。一行 CMAKE_CXX_FLAGS_RELEASE 改动。
- 生成代码与运行时不受影响（用户代码 -O2、rt.o -O2 本就独立于 preset）；build/lead 口径不用 release preset，行为无关，验证沿用 0.308 终态（226/0、244×2）。build/release（0.271 oracle）仍未触碰——release preset 的 binaryDir 指向它，重建会覆盖 oracle，需新 oracle 时另开目录。

## [0.308] - 2026-11-19

### 编译器构建速度优化（ccache + 模块扫描关闭 + release -O1）

- **集成**：speed 分支（8563c03）合入 0.307。测量报告随分支合入：docs/compile-speed.md（基线/top TU/-ftime-report：53% 时间逐 TU 模板实例化、29% 重载决议、预处理仅 3%；逐项 A/B 与放弃记录）。冲突仅 CMakeLists RT_PARTS 一处：保留 master 的 vthread + speed 的 launcher 接线。
- **生效手段**：① 关 CMake4 的 C++23 模块扫描（项目无 modules 却被注入 -fmodules-ts + 89 遍 -E 扫描，ccache 见之 100% 拒缓存）——墙钟中性，是 ccache 的前提，-DPYADAMAS_MODULE_SCAN=ON 可恢复；② ccache 自动探测（-DPYADAMAS_USE_CCACHE=OFF 可关）+ 接线 68 个运行时 custom command（不继承 launcher）；③ release preset -O3→-O1 -DNDEBUG——吞吐三层测量（pyadamas build 6 负载、重负载×10、216 用例纯前端 check）O1/O2/O3 全部中性（dev -O0 才慢 27%），取最大构建收益；保守退路 -O2（−10.1%）一行可改。
- **实测**（-j32 clean 全量，交替轮转抗噪 ×3 中位，负载 ~27-30 平稳窗口）：dev warm 全量 ~48s→**3.47s（13.9×**）；release cold 58.45s→**41.10s（−29.7%**）；release warm ~58s→**0.45s（130×**）；cold 无惩罚；真实源码改动的增量不变（改动 TU 必然 miss）。configure ~1.4s、链接 dev 3s/release 0.3s 均非瓶颈。Top1 重 TU driver_build.cpp（预处理 145k 行；全项目 89 TU 共 1050 万行，95% 标准库）。
- **放弃/记录**：lld/mold 仅 bfd 无安装权限且链接占比小；UNITY_BUILD 匿名命名空间同名 helper 跨文件 ODR 冲突（opt_dict_bump/fuse 的 is_rt/same_slot）；PCH 按配置匹配 -O 机制修好（dev 137MB/release 126MB 独立 .gch）但全量仍≈0（实例化不省）——保留默认 OFF 的 option（增量 −14/−18%）；std modules（GCC15 import std 实测可行，BMI 15.7s/30MB）是唯一根治实例化重复的方向，集成太深记录不做。typecheck_types.h 被 74/89 TU 间接包含，<format>/<ranges>/<expected> 最重扩散源。
- **验证**：行为零变化——分支口径（0.306 基）release 最终配置与 dev -Og 各 234 ran ok EXIT=0 + difftest 216/0；合入 0.307 后复核 difftest **226/0**、普通与 ASan 套件各 **244 ran ok EXIT=0**。裸 -O0 dev 的 118/154 两失败与原配置基线完全一致（既有 Debug 断言，另票）。build/release（0.271 oracle）未触碰；release preset 新旗标在分支已验证，合并未改 preset 内容。CI 装 ccache 并持久化 CCACHE_DIR 可拿满 warm 收益。

## [0.307] - 2026-11-19

### Go 式无色虚拟线程（go/channel/join，M=1 协作调度，合入 vthread）

- **集成**：把 vthread 分支（dbbb01e，基于 1e47057/0.304）合入 0.306（master 已含 0.305 cffi + 0.306 dylib 改名）。分支自编号 0.305 与 cffi 撞号，改编号 0.307。三件：① `go f(args)` spawn 任意普通函数（无色，被调函数零标注，阻塞原语在运行时内部让出，调用链无感）；② channel（make_chan/send/recv，无缓冲手递手 + 缓冲 FIFO，FIFO 唤醒）；③ join/sleep(ms) + 退出确定性 drain。与现有 async/await/生成器（rt_gen.c 无栈状态机）、CPython 桥、C FFI 完全并存互不改动。设计文档随分支合入保留：docs/vthread-design.md。
- **冲突解法（两边语义都保留）**：四处文本冲突，均为同位追加——① CHANGELOG.md：分支自编号 0.305 条目与 master 的 0.305 cffi/0.306 撞位，保留 master 历史、本条改写为 0.307；② CMakeLists.txt 的 PYADAMAS_RT_PARTS：cffi 加 cffi、vthread 加 vthread，合成 `… diag cpy cffi vthread`（同一列表喂 rt.o 与 rt_asan.o）；③ src/module.cpp enc_type：DYLIB 与 VThread/Chan 三 case 并存，都按 unknown 序列化；④ src/typecheck_types.h：TyKind 尾追加 DYLIB、VThread、Chan（master 存量值不位移），type_name 补 dylib/vthread/chan[T]。其余自动合并干净、两侧叠加（逐一核对语义）：ir_ops.h（DYLIBLoad/Close 保持 master 原位，VtSpawn/VtJoin/VtSleep/VtShutdown/ChanNew/ChanSend/ChanRecv/ChanSendF/ChanRecvF 追加枚举尾，rt_name 是 switch 无索引冲突）、codegen_call_rt.cpp（VtShutdown→none 组、VtSpawn/VtJoin/VtSleep/ChanRecv/ChanRecvF→i1 组、ChanNew/ChanSend→i2 组、ChanSendF→i1f1 组，DYLIB 留原指针组）、rt_internal.h/ir_types.h/lower_call.cpp/lower_impl.h/lower_method.cpp/typecheck_call.cpp 各分支两侧叠加。builtin_sigs.cpp 与 driver_build.cpp kModuleTable 仅 master 侧改动（vthread 不新增伪模块），无冲突。
- **模型（设计文档 §1）**：M=1 协作式有栈虚拟线程——单 OS 线程跑专用调度器（独立 mmap 栈，libco 式纯 C 循环）+ 全部 vthread。RC/GC/池/temp_stack 等 119 个单线程 static 全局态零改动、零原子零锁——M=1 是先验绕过线程安全大山的关键。每 vthread 一块 mmap 栈（默认 256KB + 底部 4KB PROT_NONE guard，池化复用上限 256 块，PYADAMAS_VT_STACK_KB 可调 16..65536）；切换 asm 存恢复 rsp+8/callee-saved/rip（.cfi_undefined 禁跨栈展开），生成代码全 %rsp 相对帧，任意栈透明运行。主流程是 root vthread（OS 主栈）。
- **调度器形态与切换（§1/§3，踩坑 §5 全部已修）**：所有 vthread 阻塞/死亡切回同一个 sched 上下文，不存在互相 resume 的链——第一版对称 inline 调度形成循环 ret 链（ping-pong 一方死亡后另一方 ret 指向死栈，「dead vthread resumed」实测），专用上下文从结构上消灭环。run queue 是 PyAdamasList 借用指针（每 VT 一份 spawn→死亡的调度器引用保活），sleep 走 intrusive 定时器链（sched 空闲 nanosleep 到最早 deadline 批量唤醒）。切换另修三个实测坑：sp 存 rsp+8（jmp 恢复不弹返回地址，存原始 rsp 每次恢复低 8 字节踩坏调用者 canary）；唤醒返回路径统一 rq_discard(self)（幽灵 run queue 条目 → 自切换）；还栈推迟到 sched 循环（池满 munmap 若在将死栈上执行，10k 死亡潮踩空）。root temp 数组退出时所有权收进 root 再清全局指针（否则 vt_destroy + pa_rc_shutdown 双重释放）。
- **栈水位全局随切换 save/load（§1）**：pa_frame_top / handler_top / pa_eh_depth / temp 三件套（**每 vthread 独立 temp 数组**，语句内阻塞 temp_len 可非零，共享数组会互相覆盖）/ 异常上下文节点链（rt_exc.c 新增 pa_exc_ctx_save/load accessor）/ exc_save_top / current_exc / current_exc_inst / pending_exc_args / pending_exc。
- **GC 跨栈策略（§4）**：GC 只在 vthread 切片间跑（切换点全局水位干净）。gc_collect 根扫描阶段调 pa_vt_gc_scan（major 必扫，minor 与既有保守根同门禁）：挂起栈按保存的帧链头走 pa_gc_scan_frames_from（rt_core.c 导出，与 gc_scan_frames 同一份 scan_mask 精确逻辑——**比逐字保守探针更精确**，帧记录物理上还在未退役的 mmap 栈上）+ 每 VT temp 数组逐字 pa_gc_mark_word；callee-saved 寄存器/fn 闭包/chan 寄存值在 VT payload 里由既有 OBJ 保守扫描覆盖（误钉不精确性文档写明）；VT 经 all_next 链保守互钉，生命周期仍归 RC。channel 缓冲是 PyAdamasList（LIST 精确扫描），无新增扫描逻辑。225 用例：只被挂起 vthread 持有的 list[str] + 自环 list 经 40000 轮 churn 强制多次 major GC 后存活且内容正确。
- **语法薄切片（§2/§6）**：go 软关键字（只有 go NAME… 才解析为 spawn，go=1/go()/go[0] 仍是普通名字），可作表达式或裸语句；ExprKind::Go + TyKind::VThread/Chan（枚举尾追加）。被 spawn 函数编译期校验：用户定义、签名已知、非 generator/coroutine/asyncgen、无 *args/**kwargs、无关键字实参、实参逐个显式给出（默认值省略暂不支持），返回值丢弃（Go 语义）。builtin join/sleep(ms)/make_chan(T,cap)（T=int/float/str/unknown，第 0 参编译期类型名特殊形式）；chan.send/recv 方法表。元素类型：int/bool/float 按裸机器字（float 走位模式 ChanSendF/RecvF i1f1 组）、str 按已知堆引用、unknown 按注册表探针（整数安全）；send 接管一份引用、recv 交出一份引用（容器所有权纪律同构）。降级 src/lower_vthread.cpp：go 合成包装闭包 __pa_go_N（build_lru_wrapper 同款形态，cell=[f,args…]，体调 f 丢弃返回值），VtSpawn 接管闭包引用；包装闭包是真实 pyadamas 函数，ABI/帧记录/异常区域全由既有 codegen 处理。入口退出 release_globals 前插 VtShutdown：**确定性 drain**（跑完 runnable/sleeping 的 vthread、放弃永久阻塞者、打印未 join 异常），与 Go「main 返回即杀」不同，方便 expect 钉输出。死锁全阻塞且无定时器 → panic: deadlock exit 1（Go 口径）。跨模块 handle/chan 签名按 unknown 序列化（与 Popen/datetime/dylib 同款口径）。
- **异常双路径（§2/§3）**：跳板 pa_vt_body_invoke（asm 外壳 + catch-all LSDA，与既有 personality/方案 G 帧清理兼容）接零开销 _Unwind 逃出；pa_vt_invoke_inner 的 setjmp 接 longjmp 路径。vthread 内逃出的异常存 handle，join 克隆重抛（先克隆再 dec 等待引用，避免级联释放 exc 本体后读悬空）；自己 try/except 接住的不传播；未 join 死亡异常在 handle 释放/退出 drain 时打印 stderr。ASan 纤维注解：切换包装 __sanitizer_start/finish_switch_fiber（仅 __SANITIZE_ADDRESS__），新生栈入口 finish(NULL)，root 主栈区间 pthread_getattr_np 捕获——警告消失，毒化记账正确。
- **实测（设计文档 §7，分支侧 Debug 编译器 -O2 产物，Xeon Gold 6252 @ 2.10GHz，容器 cpuset）**：无缓冲握手往返 **~150–300 ns/round**（100k 轮乒乓，每轮 2 切换 + 2 chan 操作；负载敏感，空闲 152/155、负载下 291/399）；spawn+join **~1.0 µs/对**（20k 空函数，栈池复用路径）；缓冲 channel **~69 ns/item**（cap 64，300k int）；**10k 并发阻塞 vthread：RSS ≈ 5.0 KB/个（合计 ~49MB），VSZ ≈ 2.5GB**（10k×260KB mmap 惰性触达），唤醒收尾后 RSS ~14MB（栈还池/munmap）。量级与 Go goroutine/channel 同级；切换 asm 本身 ~20–50ns，其余是调度器 C 路径 + 水位 save/load + chan 所有权操作。
- **限制（先验，设计文档 §8）**：**调度器重入不支持**——调度器栈上的 RC 析构级联若跑用户代码且再阻塞会重入调度器（与 asyncio 同款边界）；**M=1 无多核**（并发不并行，CPU 密集无加速）；**协作饥饿**（纯标量死循环不让出，饿死全部其他 vthread，无异步抢占）；固定栈深（溢出 SIGSEGV 撞 guard page，无信号恢复/增长）；无 select/close/超时 recv；缓冲 FIFO take_at(0) O(n)；chan 丢弃时阻塞者永久等待（退出按放弃处理）；go 形态限制见语法条；Windows 运行期 panic 桩（mmap/LSDA 都是 SysV 口径）；VT payload 保守扫描可能误钉恰好等于堆地址的标量字（既有 OBJ 同款哲学，挂起栈帧链扫描本身精确）；PYADAMAS_GC_STATS 分裂根统计不含挂起栈部分（诊断计数偏小，不影响正确性）。
- **后续（设计文档 §9，记录绕行不做）**：M>1 隔离堆、prange 数据并行、select/close/超时、异步抢占、可增长栈、async 改基到 vthread 调度器、退出语义开关。
- **验证（主树合后复跑，Release build/lead）**：cmake --build build/lead -j32 全 235 目标通过；220–229（vthread）/212–215（cffi）/207–211（桥）单跑 O0/O2 共 **38/38 ran ok**（rc=0，expect 段一致，O0==O2）；差分 **226/0**；普通与 ASan 套件各 **244 ran ok / EXIT=0**（234+10：227 run + 15 modules + cdecl:sample + cfi:unwind；180_codepoint_perf 是墙钟斜率验收用例，多租户机器负载 ~33 下默认 48 并发两次触发负载敏感失败，单跑与 PYADAMAS_TEST_JOBS=8 低并发复跑均过——与 vthread 分支同款纪律，非本轮回归）；ASan detect_leaks=1 抽查 224（join 异常）/225（挂起栈 GC）/226（churn）+ 13_gc 全干净；abbench 对 3e94c26 realistic 绑核交替 15 轮（BURST=5）go/chan 未使用，合计 **1262.14→1269.67ms（+0.6%）**——obj_dispatch +0.3% / queue_sim +0.5% / rec_memo +0.3% / sort_struct +0.1% / str_build +0.2% 均在 ±0.5% 内，dict_wordfreq 266.65→271.46ms（+1.8%，p25 +1.3%）超噪声带，按纪律 perf stat idq.dsb_uops/mite 诊断：**该负载指令数强 ASLR 布局敏感**（ASLR 开时基线 5 轮指令 1446–1592M 极差 10.1%、当前 1532–1607M 极差 4.9%，PYADAMAS_GC_DISABLE=1 仍在——既有负载的堆布局敏感路径，非本轮引入），setarch -R 确定性布局下指令 1591.0M→1547.9M（**−2.7%**）、cycles −2.0%、idq.dsb_uops −4.2% / idq.mite_uops +0.2%，绑核交替 7 轮（BURST=5）计时 280.23→274.91ms（**−1.9%**，p25 −2.0%）；objdump 逐函数对比热函数（dict_lookup/count_words/主循环/pa_dict_bump）逐字节相同，新增执行路径仅入口退出 pa_vt_shutdown 一次调用与每 GC 一次 pa_vt_gc_scan 调用（无 vthread 时空链早退），其余为 rt_core.c 增大后 GCC 对 GC 扫描助手的重内联（gc_scan_masked outline）/寄存器重分配与对齐 nop——判 +1.8% 为 ASLR 布局抽样噪声，**中性、无 DSB 布局回归亦无算法回归**；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475 / driver_build.cpp 1438 / ir_ops.h 1257→1276 / lower_impl.h 1238→1242 / typecheck_types.h 1310→1329，无新增超限文件；新文件 rt_vthread.c 937 行 / lower_vthread.cpp 200 行，codegen_call_rt.cpp 1099<1200）。
- **既有问题（不在本轮修）**：Debug 构建 118_suspend_expr / 154_nested_await 编译器侧 STL 越界断言崩溃（SIGABRT exit 134），0.304/0.305 已记录、base 同款复现，与 vthread/cffi/桥均无关，Release 口径不触发（建议单独立项）。

## [0.306] - 2026-11-19

### cdll 改名 dylib（平台中性命名）

- **理据**：DLL 是 Windows 术语（Linux .so / macOS .dylib），本功能是 dlopen/LoadLibrary 的跨平台 facade，cdll 让人误以为只管 Windows。ctypes 的 cdll 本意是调用约定命名空间（cdecl vs windll/stdcall），本项目无此区分，借来的名字连原始理据都不成立。用户拍板 dylib。
- **范围**：纯机械重命名，零语义变化——TyKind::CDLL→DYLIB、RtFn::CDLLLoad/Close→DYLIBLoad/Close、pa_cdll_*→pa_dylib_*、t_cdll→t_dylib、kModuleTable 条目、用户可见字符串（dylib.load: cannot open / in dylib handle / <dylib handle> / Windows NotImplementedError 文案）、tests/cdll/→tests/dylib/、213_cdll.py→213_dylib.py、214 错误路径 expect 同步、docs/cffi-design.md 示例与标识符同步（顶部加改名注记）。不留 cdll 别名（0.305 刚落地，无外部用户）。0.305 条目作为历史记录保留（其中 cdll 字样是当时命名）。
- **验证**：212–215 O2 单跑 OK；差分 216/0；普通与 ASan 套件各 234 ran ok / EXIT=0；abbench 对 f5d57b8 realistic 绑核交替 15 轮中性（合计 1262.59→1261.37ms，−0.1%，六项 ±0.3% 内）；规模检查无新增超限。

## [0.305] - 2026-11-19

### C FFI 免写直调与动态库（from c import / cdll.load，合入 cffi）

- **集成**：把 cffi 分支（9036fce，基于 f0a4af4/0.303）合入 0.304。三件：① 免写声明直调 C 函数（`from c import`，零 cdecl/.cpya）；② dlopen/dlsym 动态库（`cdll.load` 句柄 + 属性调用）；③ 既有 cimport/cdecl 健壮化（C float 单精度编组 + 变参 %al 钳制）。c/cdll 是独立 Native 伪模块，与 CPython 桥（python 伪模块）语义互不相交，测试编号 212+ 已避让 207–211。设计文档随分支合入保留：docs/cffi-design.md。
- **审计（设计文档 §1）**：既有管线 cimport→手写 .cpya 文本声明→FuncInfo(is_extern)→lower_cimport_call 逐实参 coerce→CallExtern→gen_call_extern SysV 分类（6 int/8 xmm/栈溢出/结构体 chunk/隐藏 sret/回调跳板/变参 %al）→链接期绑定；cdecl 把 C 头文件子集转 .cpya 走同一管线。审计确认已支持：CKind 标量/结构体按值/回调跳板/变参/extern var/ptr[T] 解引用/str NUL 终止不变量。本轮挑两个便宜洞修（C float 参数/返回整体跳过、变参 %al 未钳制）+ 两件新能力（免写直调、dlopen）；深坑（宏展开/回调重入/float32 字段等）记录绕行。
- **冲突解法（两边语义都保留）**：仅两处文本冲突，均为同位追加——CMakeLists.txt 的 PYADAMAS_RT_PARTS 桥加 cpy、cffi 加 cffi，合成 `… diag cpy cffi`（同一列表喂 rt.o 与 rt_asan.o）；driver_build.cpp kModuleTable 桥登记 python、cffi 登记 c/cdll，三条目并存。其余自动合并干净、两侧叠加：builtin_sigs.cpp（python/python.* 的 cpy_proxy 开集分支 + c 空 globals/cdll load/close 签名）、driver_build.cpp link_program（-lm → -lm -ldl，glibc 2.34+ 已并入 libc、-ldl 是空壳旧 glibc 仍需要）与 program_module/check_load 的 python.<module> 开集子代理并存、ir_ops.h（RtFn::CpyCall 与 CDLLLoad/CDLLClose 不同位，rt_name 是 switch 无索引冲突）、codegen_call_rt.cpp（CpyCall 归 i4 组、CDLL 归指针组）、rt_internal.h/lower_import.cpp/typecheck_stmt_forms.cpp/typecheck_call_forms.cpp/lower_method.cpp 各分支两侧叠加。
- **语法语义（设计文档 §3）**：`from c import sqrt, strlen`（可 as 别名，C 侧符号名不变）绑定默认库符号，符号名集合编译期不可知，每名建 FuncInfo{c_lazy,c_symbol}；调用点（check_c_lazy）拒绝关键字/`*` 展开、求实参类型、推断返回 ABI；降级（lower_c_lazy.cpp）按 (符号,绑定方式,返回种类) 去重登记 IRExtern{lazy=1} 发 CallExtern；首调 dlsym(RTLD_DEFAULT,name) 缓存到静态槽 .Lcsym<N>，此后只多 load+test+jne 与间接 call。`cdll.load(path)` → dlopen(RTLD_NOW|RTLD_LOCAL) 句柄（TyKind::CDLL，标量整数地址，非堆无 RC，可存容器/传参，print `<cdll handle>`，跨模块 enc_type→unknown）；句柄属性调用 lib.sym(...) 与免写路径共享 lower_c_lazy_call/CallExtern/gen_call_extern，lazy=2 句柄值在 Inst::a，逐次 dlsym(handle,name)（原型不做缓存）。错误路径统一 OSError 点名符号/路径（库不存在/默认符号不存在/句柄符号不存在；dlsym 的 NULL 用 dlerror 区分符号值合法为 0 的边界）。`from c import *` 拒绝；`import c; c.sqrt(...)` 不支持（报错指引 from-import）；c_lazy 函数不能当值。
- **ABI 契约（设计文档 §4）**：实参推断 float→double(xmm)/int,bool→i64/str→char*(data+24，NUL 终止不变量)/bytes→uint8_t*/list[float|int]→double*|i64*(data+16)/Optional[str]→可空 char*/Optional[int]→void*；其它/unknown build 期报错（cannot infer C ABI）。返回默认：有 float 实参→double 否则 i64（ctypes restype=c_int 同构契约）；**无标注时指针返回按 i64 读出垃圾值**（ctypes 同契约，213 用标注演示正确姿势）。标量标注覆盖返回 ABI：AnnAssign 与 return 标注（float/int/str/bool/None/Optional[str|int]），求值后覆写 expr_types_[call]。免写路径不做：C float 单精度/结构体按值/变参/回调（需 cimport .cpya）；float 实参永远按 double 传（调 C float 函数用 float32 声明，否则 ABI 错）。SysV 6 int/8 xmm 溢出落栈沿用；免写新增 fn_tmp 暂存（out_base+stack_bytes+48），outgoing_bytes 上界 48→56。
- **修复（设计文档 §5）**：① CKind::Float32（ckind_width=4，.cpya 类型名 float32，`float` 仍是 double 兼容不变，PyAdamas 侧统一 double）：参数 cvtsd2ss（寄存器/栈两路）、返回 cvtss2sd、extern var movss 加载/存储、ptr[float32] 按 fwidth=4 解引用（写回 cvtsd2ss+movss）；cdecl 的 C float 参数/返回从整体跳过函数改为生成 float32（sample.h scalef/global_ratiof 端到端）。刻意边界：float32 结构体字段（cfield_layout 显式报错）、含 float32 回调签名（callback_kind_ok 拒绝，cdecl 跳过并警告）——字段访问宽度/跳板单精度提升深度超出本轮。变参里的 float 仍按 double（C 默认实参提升，语义正确）。② 变参 %al 钳制：fi>8（浮点实参溢出到栈）时 %al=8（ABI 上限），原样塞 fi 会让 va_arg 寄存器保存区越界读；111_varargs 回归通过。
- **实测（设计文档 §6，分支侧 Debug 编译器，生成程序 O0/O2 均验证）**：212_c_lazy sqrt(2.0)=1.4142135623730951（O0==O2，别名/标注/函数返回标注/循环内缓存槽均正确）；213_cdll 现场 cc -shared -fPIC -O2 建 probe.so，probe_add(2,3)=5/probe_scale(4.0)=10.0/int+str 混合 probe_mix(10,"hello")=15/str 标注 probe_greet="hi"/close=0；214 三类错误消息 O0==O2；215 f32_mul(1.5,2.25)=3.375/f32_of_int(7)=3.5/roundtrip(π)=3.1415927410125732（单精度舍入可见）/float32 全局变量/ptr[float32] 读写。分支口径全套件 227 ran ok（基线 223+新 4，较主树同基线口径少 2，系 worktree 环境口径差；主树合后实际 234，构成见验证），与 base 逐条 diff 仅新增用例、零回归，fail/strict/cdecl/cfi 全过；ASan 212–215 与既有 FFI 用例（97/98/101/103/104/107–113）全过。性能注记（原型未优化）：lazy=1 首调多一次 dlsym（含 dlerror 清/查），此后 load+test+jne+间接 call（vs 链接期直 call）；lazy=2 逐次 dlsym（热路径 (handle,symbol) 缓存后续）；未做 PLT 式惰性绑定。
- **验证（主树合后复跑，Release build/lead）**：cmake --build build/lead -j32 全 223 目标通过；212–215（cffi）与 207–211（桥）单跑 O0/O2 共 18/18 ran ok（rc=0，expect 段一致）；差分 **216/0**；普通与 ASan 套件各 **234** ran ok / EXIT=0（230+4：217 run + 15 modules + cdecl:sample + cfi:unwind，OpenBLAS 在场无跳过；f27 关键字实参/s04 unknown 实参正确拒绝）；ASan detect_leaks=1 抽查 212–215 / 207–211 / 13_gc 全干净；abbench 对 1e47057 realistic 绑核交替 15 轮（BURST=5）桥/cffi 未使用呈中性——dict_wordfreq 265.80→268.37ms（+1.0%，p25 −0.3%）、obj_dispatch −0.1%、queue_sim 158.15→157.23ms（−0.6%）、rec_memo +0.1%、sort_struct +0.2%、str_build −0.4%，合计 **1259.49→1260.51ms（+0.1%）**，六项均在 ±1.0% 噪声带内（p25 均在 ±0.3% 内）、无回归（rt_cffi.c 并入 rt.o 未引发可测 DSB 布局回归）；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475 / driver_build.cpp 1438 / ir_ops.h 1257 / lower_impl.h 1238 / typecheck_types.h 1310，无新增超限文件；codegen_call.cpp 1132<1200）。
- **限制（先验，设计文档 §7）**：免写直调=任意符号调用，ABI 不符会**静默算错**（ctypes 同契约，生产代码建议 cimport 显式声明）；回调槽不可重入（既有，免写路径不接回调绕开）；cdecl 不展开函数式宏/条件编译（既有）；float32 结构体字段/含 float 回调显式拒绝；c_lazy 函数不能当值（需捕获句柄的闭包+统一约定 shim，后续）；lazy=2 逐次 dlsym、句柄无自动 dlclose（泄漏到进程退出，ctypes 同语义）、RTLD_GLOBAL/flags 未暴露；Windows rt_cffi.c 给 NotImplementedError 桩（LoadLibrary/GetProcAddress+Win64 调用约定后续）；CDLL 跨模块按 unknown 序列化会丢类型；标注覆盖只及 AnnAssign/return（float(sqrt(2)) 转换包装、容器元素标注不传播，推断默认兜底）。
- **既有问题（不在本轮修）**：Debug 构建 118_suspend_expr / 154_nested_await 编译器侧 vreg_nonneg STL 越界断言崩溃（SIGABRT exit 134），base f0a4af4 独立 worktree 复现完全相同，与 FFI/桥均无关，Release 口径不触发（与 0.304 同条，建议单独立项）。

## [0.304] - 2026-11-19

### CPython 互操作先验（from python import 子进程 IPC，合入 cpybridge）

- **集成**：把 cpybridge 分支（9c19e77，基于 0.302）合入 0.303。`from python import <module> [as alias]` 把属性调用降级成一次 CPython 子进程 IPC（首次调用懒拉起 python3 worker，带标签 JSON 帧，全进程一个 worker 复用），结果回到 pyadamas 原生值域（unknown）。不嵌入 libpython：无 GIL 互嵌、无 Py_INCREF/DECREF 跨边界所有权、无解释器初始化与主程序 RC/GC 耦合。lexer/parser 零改动（from python import 本就是合法 ImportFrom），语义全在 typecheck/lower 的模块分支。点子模块链调用（cpyos.path.basename）编译期拼成完整模块名仍是一条 IPC。设计文档随分支合入保留：docs/cpybridge-design.md。
- **冲突解法（两边语义都保留）**：文本自动合并干净——ir.cpp 两级索引分析（needs_unknown_index 与 can_skip_index_maint/index_readonly）拉黑 CpyCall/RcTempUnknown；rt_core.c pa_print_any 改用共享探针 pa_unknown_is_heap + CPYFLOAT/LIST/DICT 分派，与 0.303 的 pa_rc_track、rc_index_off 非 static 并存；rt_internal.h（PYADAMAS_RC_CPYFLOAT=8 + pa_cpy_call/pa_print_cpy_value 声明）、ir_ops.h（RtFn::CpyCall + rt_name）、codegen_call_rt.cpp（CpyCall 归 i4 组）、CMakeLists.txt（rt_cpy.c + codegen_index.cpp + lower_cpy.cpp）、typecheck hoist 对 from python import 跳过 bind_alias、lower scan_globals 不占全局槽，均两侧叠加。**唯一语义冲突在运行时交互**：0.303 给 pa_list_append 加了种类维护（kind0 表 append 堆值会升级 kind2），而桥的实参打包是 kind0 借用表（元素引用仍归原主，销毁不 dec）。itertools pack_seqs 传 heap=2 之所以无恙，是那些模块走 index_readonly（探针恒 0、不升级）；桥模块被两级分析强制保留 unknown 索引（CpyCall 拉黑），探针真命中 → 借用表升级 kind2 → 销毁按注册表 dec 借来的实参引用 → 过度释放（208 实测「refcount decrement on a freed object」，C harness 复现 kind0→2 升级后源表 rc 1→0）。修：lower_cpy.cpp 实参 append 的 heap 恒传 0（kind==heap 跳过升级，保持 kind0 借用语义）；marshalling 由描述符驱动（pa_cpy_call 读 args->data[i]+descs），与 elem_kind 无关，故 heap=0 不影响序列化。0.303 运行时种类维护语义不动。
- **延迟（设计文档 §6 实测，Debug 编译器 -O2 产物，python3 3.14.4，N=3000 三轮）**：轻调用 IPC 地板 ~25µs——os.getpid 26.2/25.5/25.5µs（原生 0.59µs ~44×，CPython 进程内 0.36µs ~70×）；math.sqrt(2.0) 26.8/30.0/28.8µs（原生内联 sqrtsd ~10^4×）；json.dumps(100 元素 list) 103/130/122µs（原生 8.1–13.1µs ~10–13×，CPython 进程内 8.3µs）；首次调用含 spawn 36.5/39.7/38.0ms。量化口径 ~25µs/次 ⇒ 单 worker 上限 ~4 万次调用/秒，payload 越大传输占比越高（bytes 走 base64 +33%）。适用边界：粗粒度库调用（一次干 ≥ms 级的活）或批量数据一次过桥（convert a batch once, work natively）；不适合热循环逐元素、把桥当数学库（原生 math 快 10^4×）；短命进程（<100 次调用）摊不平 spawn。
- **限制（先验，设计文档 §8，均有编译期/运行期明确报错）**：kwargs 不支持（协议只带位置参数）；handle 代理（文件句柄/迭代器/生成器/自定义实例/模块/函数）拒绝 TypeError；模块级 from python import name 与裸属性读（math.pi/sys.version）不支持（只支持调用）；结果类型永远 unknown（float 盒已解决打印，unknown 算术/下标/dict 键查找仍受限——下标当 list、bin.add 当整数加）；无超时（worker 卡死挂住调用方）；单 worker 串行（无池/并发，asyncio 需后续）；生成器/async 不能过桥；Windows 报 OSError（CreateProcess 路径后续补）；int64 边界（CPython int 超 int64 → OverflowError，float NaN/Inf 全链路支持）；dict 非 str 键按指针哈希（float 盒/容器键等值查不到）；用了桥的模块强制保留 unknown 索引（放弃 exact-RC 优化）；worker 崩溃当次报 RuntimeError、下次重拉干净 worker（模块状态丢失）；每调用 malloc DOM+帧缓冲不池化。unknown 值域固有歧义：None/bool 进 unknown 与 int 不可分（回程 None→0、bool→0/1，print 出 0/1 而非 None/True，静态类型已知位置不受影响）；tuple 回程打印 [..] 而非 (..)（运行期无 tuple 标记）。异常按 type(e).__mro__ 名表映射到 pyadamas 内置名（JSONDecodeError→ValueError），全不命中→RuntimeError 带 CPython 原名。
- **验证**：207–211（cpy）与 206（store kind）O0/O2 对拍一致（cpy 用例按 expect 段，该段钉自真实 CPython 输出）；差分 **212/0**；普通与 ASan 套件各 **230** ran ok / EXIT=0；ASan detect_leaks=1 抽查 207–211 / 13_gc / 206 全干净；abbench 对 f0a4af4 realistic 绑核交替 15 轮（BURST=5）桥未使用呈中性——dict_wordfreq 268.53→267.09ms（−0.5%）、obj_dispatch −0.1%、queue_sim 159.73→158.12ms（−1.0%）、rec_memo −0.1%、sort_struct −0.4%、str_build −0.1%，合计 **1265.08→1260.79ms（−0.3%）**，六项均在 ±1.0% 噪声带内、无回归（rt_cpy.c 并入 rt.o 未引发可测 DSB 布局回归，无需 perf stat 诊断）；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475 / driver_build.cpp 1432 / ir_ops.h 1253 / lower_impl.h 1236 / typecheck_types.h 1297，无新增超限文件）。
- **既有问题（不在本轮修）**：Debug 构建 118_suspend_expr / 154_nested_await 编译器侧 std::vector<unsigned char> 越界断言崩溃（SIGABRT exit 134），base c467230 独立快照 Debug 构建复现完全相同崩溃，与桥无关，Release 口径不触发（设计文档 §9，建议单独立项）。

## [0.303] - 2026-11-19

### 下标写入维护列表种类元数据（修 unknown 静态类型写入的 SIGSEGV）

- **根因**：静态容器类型是 unknown 时，降级器 store_list_elem 只 retain 不调 pa_list_prep（lk==2 分支），运行期 elem_kind 可能与新值冲突。gen_index_store（O1+ 内联）与 pa_list_set（O0/冷路）写入时都不维护种类/位图：kind1 列表存进标量后种类不升级，销毁 pa_rc_dec_heap_batch 把标量当堆指针 dec → SIGSEGV（/tmp/kind1_repro.py，CPython 正常）；kind2 带位图的混合表（如 [1,"x",3]）覆盖后位图陈旧，销毁按旧位 dec 同样崩（0.271 oracle 即崩，属既有洞）。kind0 存堆值则漏释放/漏 GC 出边。
- **运行时（rt_list.c）**：pa_list_set / pa_list_insert 增加第 4 参 heap（新值静态种类 0/1/2，与 append 同口径），写入时经共享的 pa_list_store_kind 维护：kind 冲突升级 2；kind2 带位图按新值堆性更新第 i 位；kind2 无位图无需动作（销毁走注册表）。**heap==2 才探针**（pa_unknown_is_heap，与 dec_unknown 逐位一致含 exact-RC 回退）——静态已知标量绝不探针：大整数/浮点位模式可能恰好等于存活堆地址，探针会误判（206 用 2^46+12345 在 ASan 下覆盖）。旧值仍按**写入前**的 kind 释放，新值 retain 仍由降级器负责，所有权语义不变。pa_list_prep 的旧 list_probe（只查 rc_index，exact-RC 下恒 0 会漏升级）换成共享探针；探针包装加常规模式内联快路（rc_index_off 改 extern），slow 只服务 exact-RC 回退，prep 热路径与 0.302 同形态。pa_list_append 同族补种类维护（kind3 定型/冲突升级），热路径（kind0+0/kind1+1/kind2）只多一次 kind 装载+两次比较。
- **发射（codegen_index.cpp，新文件）**：gen_index_store / gen_index_store_unchecked 从 codegen_call.cpp 拆出（原文件 1188 行贴近 1200 硬限；codegen_call_rt.cpp 余量也不足，按 §7.1 放新文件）。O0 传 heap 第 4 参；O1+ 内联按新值静态堆性分流：kind0+标量（含 0.298 的 list[float] 快路径，逐字节保留）几条指令跳过；冲突种类/unknown 探针交运行时 pa_list_set；kind2 位图用 bts/btr 内存位操作按静态堆性更新（unknown 新值+kind2 无位图仍纯内联，不必探针）。gen_append_inline 增加「运行期 kind==静态 heap」守卫（heap 是编译期常量，按立即数比较，热路径多一条 cmp+jne；heap==2 恒落回）。RtFn::ListSet/ListInsert 改 4 参（i4 组），datetime.replace 与 list.insert 降级点补 heap 实参。
- **同族审计（切片/extend/乘法/set_len 及重排路径）**：pa_list_slice_assign 已有 kind 合并，但移动/覆盖元素后位图陈旧 → kind2 一律 pa_list_drop_bits 回退注册表（源空表收缩也成立），0→2 升级补 GC 链；insert/del_index/reverse/sort/random.shuffle 同族丢位图（del 另补 kind3→2 防御映射）；copy_range 快路径增加「源运行期种类==静态 heap」守卫（unknown 别名可让 static list[str] 装 kind2 混合表，memcpy+批量 inc 会把标量当堆指针）；extend/reversed/copy 经 prep 已覆盖；set_len 只截断、按 kind 释放，无洞；repeat/slice 新建表种类随源，无洞。运行时内部 append 调用点（dict keys/values/items、itertools、most_common、accumulate、str/bytes/range_to_list、list slice/repeat）把 heap=2 改成准确的运行期种类提示，避免种类维护引入逐元素探针（items 的混合元组因此获得精确位图）。
- **GC link 语义（kind0→2 升级）**：kind0 小元组是 untracked 分配（不进 GC 堆链/索引/新生代）。升级成 kind2 后有了堆出边、可能成环，新增 pa_rc_track（rt_core.c）：清 UNTRACKED 位再 rc_link，与 tracked 分配同形态；young_add 不改 rc、gc_alloc_black 是占位空操作，存活对象计数不受影响；新入链对象不在本轮 gc_set 快照里，增量标记期入链也安全（与新生对象同一论证）。prep/store_kind/append/slice_assign 的 0→2 升级点统一调用。30000 轮「元组经 unknown 升级后自环」ASan detect_leaks=1 干净（环真收集）。
- **实测（对 c467230 绑核交替 15 轮，BURST=5）**：realistic 六项 dict_wordfreq 262.18→266.10ms（+1.5%，p25 +1.0%）、obj_dispatch +0.1%、queue_sim 156.51→159.51ms（+1.9%）、rec_memo 232.65→236.89ms（+1.8%）、sort_struct +0.4%、str_build 141.20→144.00ms（+2.0%，p25 +1.6%），合计 1243.32→1257.97ms（**+1.2%**）；**nbody 39.48→39.42ms（−0.1%）、spectral_norm 60.56→60.58ms（+0.0%）、matmul 19.66→19.26ms（−2.0%）**，三项合计 −0.4%，浮点热路径无回归（kind0+标量守卫与 0.298 快路径逐字节保留）。
- **回归诊断与修复（本轮三次迭代）**：初版 str_build +5.9% / dict_wordfreq +4.7%，逐一定位修掉：① append 内联守卫写了「kind!=2 + kind==heap」两条比较，而 str_build 的 parts.append 推成 list[str]（heap=1），每 append 多 2 指令（8M 次 +1.4% 指令）——融合成单条 cmp $heap（kind2/3 与冲突种类一并排除），与 0.282 守卫同指令数，asm 对比只剩立即数差异；② items/most_common 的混合元组按 own 传 0/1 提示，kind2 元组首元素触发位图分配，每对一次 alloc/free churn——tk==2 改回 heap2（不建位图，注册表销毁），tk 0/1 传准确种类免探针；③ prep 的无条件「升级+track」写法让 gcc 给整函数加 push/spill/setne 序列——track 移进冲突冷分支，kind2 直接跳过探针（种类恒 2，探针本就冗余）；④ 共享探针经 slow 函数多一层 call——头文件包装加常规模式快路（rc_index_has 命中即返回，rc_index_off 改 extern 供判断），exact-RC 才进 slow 回退，判定逐位一致。修后 str_build 指令 +0.1%（逐位级持平）。剩余 +1.2% 是 **DSB 布局**：rec_memo 不执行任何改动代码，perf stat 指令 +0.15%（1803.1M→1805.8M）、cycles +1.9%、idq.dsb_uops −8.9%（1230M→1121M）、idq.mite_uops +19.4%（565M→675M），与 0.301 rec_memo +1.4% / 0.302 sort_struct +2.5% 同族（权衡接受，未放宽守卫）；queue_sim 指令 +0.9%、DSB −2.3%/MITE +7.0% 同族。
- **验证**：新增 206 覆盖 kind1 存标量（原始复现同族）/kind0 存堆值/unknown 探针双向/kind2 位图堆位↔标量位互换（含 63/64/65/128/129 跨字边界）/浮点 kind1 升级与 kind0 float 保持/大整数静态标量不探针/insert/del/pop/reverse/clear/切片增长收缩/extend/append/kind3 定型/sort/shuffle/churn 升级销毁循环/kind0 与 kind1 自环 GC。读回走 unknown 通道（静态元素类型特化是既有语义，不在本轮范围）。O0/O1/O2 对拍 CPython 一致，0.271 oracle 在 206 上 SIGSEGV（rc 139 vs 0，与 201/202/203 同属「oracle 早于用例」的预期 rc 差异）；ASan detect_leaks=1 干净。差分 207/0；0.271 原有语料零差异；普通与 ASan 套件各 **225** ran ok / EXIT=0；泄漏抽查 13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend / 204 / 205 / 206 全干净；Win64 交叉汇编 nbody/spectral_norm/matmul 通过（206 与 13_gc/205 同样受既有扫描位图 .pushsection 段指令限制，未验证 Windows 运行）；规模检查仍只有原有 5 处超限（codegen_call.cpp 1188→1066，新文件 codegen_index.cpp 256 行）。
- **遗留（审计发现，未在本轮修）**：① 静态元素类型特化的读回（list[str] 经 unknown 存标量后按 str 读）仍按旧类型解释，属既有语义；② 混合字面量经切片赋值推断成 list[str] 时 MakeList 按 kind1 建表装标量（0.271 即崩，类型推断洞）；③ deque 的 set/extend 不维护种类（同族，独立容器）。

## [0.302] - 2026-11-19

### unknown dict 的标量-only 销毁跳过（rec_memo −12.8%，realistic 合计 −2.5%）

- **取证**：rec_memo 的模块级 `memo={}` IR 类型是 dict[unknown,unknown]（pa_dict_new 传 own=2,2），虽然实参形参都是 int，运行时所有权标志仍是 unknown —— 0.285 的标量销毁跳过（key_own==0 && val_own==0）从不命中。perf：dict_destroy 13.1%（逐 entry 调 pa_rc_dec_unknown）+ pa_rc_dec_unknown 族 4.7%，每轮 25 个 entry 白走链游 + 探测。
- **发射（运行时-only）**：PyAdamasDict 尾部追加 sticky 标志 saw_heap（既有字段偏移不动；grep 核实 src/ 无 PyAdamasDict 硬编码尺寸/尾部偏移，codegen 只按偏移 0 读 len；sizeof 136→144 由 sizeof 表达式自动跟随，dict 池路由不受影响）。own==2 的 entry 写入路径探测键/值是否堆指针，是则置 1（只置不清；clear/new 归零）。dict_destroy / dict_gc_release / pa_dict_clear 在 (key_own==2 && val_own==2 && !saw_heap) 时跳过链游 —— 从未存过堆值，全部 entry 是标量，无 RC 可做。0.285 的 (0,0) 跳过保留。
- **共享探针（安全核心）**：rt_core.c 暴露 pa_unknown_is_heap_slow + 头文件内联包装 pa_unknown_is_heap，与 pa_rc_dec_unknown 的「该不该 dec」判定**逐位一致**（常规模式 rc_index；exact-RC 模式对齐 + 堆链回退；阈值 <65536 与 inc/dec_unknown 同款 —— 取证描述里的 4096 是 list_probe 的阈值，未采用）。探针说 0 的值销毁时必然空操作，跳过不会漏释放。dec_unknown 改为共用该探针，不复制两份逻辑。内联包装先排除小整数，标量不进函数调用。
- **写入路径全枚举（grep entries 写入点逐一核对，rt_dict.c 外无 entry 写入）**：pa_dict_set 插入/覆盖（函数入口统一探针）；pa_dict_bump 命中直写 val（单独探针 nv）、未命中走 set；update/copy/from_pairs/set_from_list/defaultdict 缺省创建/class 属性表/stdlib/json/re 全部经 pa_dict_set。**update 的 1→2 升级洞**：dst 既有 entry 按旧 own==1 retain（不探针），升级后必须保守置 saw_heap=1（len>0 时），否则跳过链游漏释放；0→2 升级表里只有标量无需置位。copy 保守传播 src->saw_heap。覆盖/删除不减位（sticky 保守）。GC 标记（gc_scan_object）不读 saw_heap，照常逐 entry 扫描。
- **pa_dict_set 的标量快路**：saw_heap 已置位直接返回；标量只做 own 比较 + <65536 比较，**不写内存**（0.283 STLF 教训：不要每 set 必写标志形成循环携带 store→load 依赖）。
- **内联扰动修复（0.285 同款）**：共享探针让 unknown_is_heap 多了第二个调用点，gcc 把它从 dec_unknown 里 outline（objdump 证实多一层 call/ret）——加 always_inline 恢复紧形态（dec_unknown 内 0 处 call unknown_is_heap）。
- **实测（对 1ce2d21 绑核交替 15 轮，BURST=5）**：rec_memo **268.01 → 233.72 ms（−12.8%，p25 −12.8%）**；str_build −1.8%（p25 −2.1%）；dict_wordfreq +0.1%、obj_dispatch −0.0% 中性；queue_sim +0.9%（p25 +0.8%）；sort_struct **+2.5%**（p25 +2.4%）。六项合计 **1282.45 → 1249.84 ms（−2.5%）**。perf 证实机制命中：rec_memo 里 dict_destroy 11.1% + dec_unknown 族 4.7% 整体跌出 profile。
- **回归诊断（DSB 布局，非算法）**：sort_struct 不执行任何 dict 代码，perf stat 指令 455.208M → 455.198M（−0.002%，逐位级持平）、cycles +2.2%、idq.dsb_uops −12.7%（280.5M → 245.0M）、idq.mite_uops +19.3%（171.0M → 204.0M）—— rt_dict.c/rt_core.c 尺寸变化把热区 uop 挤出 DSB 窗口，与 0.282/0.289/0.301 同族（权衡接受，未放宽守卫）。queue_sim 无 dict，指令 +0.5%、cycles +0.24%（perf stat 单次）、DSB +4.8%/MITE −7.0%，噪声级布局代价。
- **验证**：新增 205 覆盖 {} 推 unknown 的标量 dict 销毁（含 fib(25)=75025 ≥65536 的大标量探针路径）、存堆值（str/list/dict 嵌套）后销毁真释放、堆→标量与标量→堆覆盖、update 的 (2,2)/(1,1)→(2,2)/(0,0)→(2,2) 三种升级、copy 传播、clear 归零后再探针、pop/del/setdefault/bump/from_pairs/set/defaultdict(list/int/str)、unknown dict 自环与标量 dict 经列表进 GC 环。O0/O1/O2 对拍 CPython 一致；ASan detect_leaks=1 干净（漏探针即泄漏，LSan 抓）。差分 206/0；0.271 原有语料零差异（201/202/203 为 0.299/0.300 新增，oracle 早于其语法）；普通与 ASan 套件各 **224** ran ok / EXIT=0；泄漏抽查 13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend / 204 / 205 全干净；规模检查仍只有原有 5 处超限。

## [0.301] - 2026-11-19

### 列表缓冲池扩容 + 内联头池扩容（sort_struct −42.2%，realistic 合计 −6.1%）

- **取证**：sort_struct（3900 轮 × 500 元素 list[tuple[int,int,int]] 建表→sort→聚合→丢弃）perf 里 glibc malloc/free 合计约 27%。初判三个来源：list_buf_pool 只覆盖 <=512B（64 元素），500 元素缓冲 4000B 超档，增长链与销毁全走裸 malloc/realloc/free；kind-2 位图每次增长 realloc；排序临时区每轮 malloc((n/2+1)*8)。
- **发射（运行时-only，rt_list.c）**：① list_buf_pool 覆盖扩到 8192B（1024 元素），小档（<=512B）每档仍 64 块、大档每档 8 块防囤内存；请求档为空时按 128 位非空位图 O(1) 向上取第一个非空档（容量只会变大，cap_io 回写真实容量），找不到才 malloc。② list_buf_grow 池内尺寸先试池：get 新块 + memcpy 旧内容 + 旧外部块 put 归还（内联块不动）；>8192B 保留原 malloc/realloc fallback。③ heap_bits 位图入同一池（字数即 8 字节单位 cap）：增长复制旧字 [0,owords)、新字 [owords,bcap) 全清零（比旧实现清零范围只大不小），bits_cap 记池块真实字数；销毁/unknown 降级 put 代替 free。增长后位图扩容改按 l->cap（池可能把 cap 抬高到 > ncap，按 ncap 会漏扩）。④ timsort 临时区入池（n/2+1 元素）。**排序期间不执行用户代码**：plain sort 比较是运行时宏（LESS_STR 只调 pa_str_cmp）；sorted(key=) 的 key 在生成循环预算好（lower_builtin_seq.cpp），ListSortedBy 只收 keys 列表；list.sort(key=) 不支持（lower_method_containers.cpp 报错）。get/put 纯缓冲操作、无 RC/GC 副作用，即使重入也安全。临时区 OOM 改走运行时统一 panic（与 list_buf_grow 一致），不再退化为插入排序。
- **取证修正（关键）**：只做缓冲池化时 abbench sort_struct 仅 −2.0%。perf 复查发现 malloc 大头不在缓冲链而在**元组 RC 头块**：每轮 500 个元组同时存活，list_inline_pool 上限 64，每轮 ~436 个头块走裸 malloc（_int_malloc 9% 全在 rc_alloc_core）/free（_int_free 族 15% 全在 pa_rc_dec_heap_batch）。**内联头池上限 64 -> 512**（rt_core.c，512×176B≈90KB 缓存上限）后第二轮起头块零 malloc，perf 里 malloc/free 跌出前十（_int_malloc 0.15%）。缓冲池化本身确已生效（增长链 malloc 消失），只是边际占比小。
- **不变量保持**：GC 清扫（gc_free_body）不经池、直接 free data/位图（池块是普通 malloc 块，直接 free 合法）；shutdown drain 释放所有档（含非空位图清零）；池块复用不携带旧数据语义（data 只读 [0,len) 先写后读，位图按上文清零/复制）；bits_cap 独立记账语义保持；同池 dtor/release/kind/size 恒定、入池 rc 归零（0.276/0.280 不变量）不受影响。
- **实测（对 85cb1e5 绑核交替 15 轮，BURST=5）**：sort_struct **194.90 → 112.56 ms（−42.2%，p25 −42.4%）**；str_build **−2.7%**（p25 −2.6%）；dict_wordfreq −0.5%；obj_dispatch −0.1%；queue_sim +0.5%（p25 +0.4%，噪声级）；rec_memo **+1.4%**（p25 +1.4%）。六项合计 **1365.60 → 1282.32 ms（−6.1%）**。
- **rec_memo +1.4% 诊断（DSB 布局，非算法）**：perf stat 指令 −0.07%（1943.8M → 1942.5M，逐位级持平）、cycles +1.9%、idq.dsb_uops −2.1%（1253M → 1226M）、idq.mite_uops +4.6%（646M → 676M）——rt_list.c 尺寸变化把热区 uop 挤出 DSB 窗口、传统解码增多，与 0.289 的 rec_memo +1.8% 同族（权衡接受，未放宽任何守卫）。
- **验证**：新增 204 覆盖跨档增长/cap 抬高/位图复用后 kind-2 正确性（list[object] 混合表 + 清空再增长）/排序结果（int/float/str，0/1/2/64/65/500 边界）/sorted(key=) 稳定性/超大表（3000 元素）池外 fallback/extend 与切片赋值增长/GC 环列表（自环混合表 30000 轮）。O0/O1/O2 对拍 CPython 一致（元组排序按机器字序是既有语义，校验和取顺序无关量），ASan/LSan 通过。差分 205/0；0.271 原有语料零差异（201/202/203 是 0.299/0.300 新增用例，oracle 早于其语法，rc 1 vs 0 属预期）；普通与 ASan 套件各 **223** ran ok / EXIT=0；ASan detect_leaks=1 抽查 13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend / 204 全部干净；规模检查仍只有原有 5 处超限。

## [0.300] - 2026-11-18

### 按实际绑定的实参推断类型（不新增语法）

- 调用确实省略形参时，局部可重查的函数或方法才用其字面量默认值推断；显式传参不会被未使用的默认值约束。保持 `None` 哨兵，不把它钉成形参类型；对无法安全共用浮点调用约定的默认值、实参与已知覆盖给出诊断。
- `*` 展开固定长度元组时按元素逐槽绑定，使用每个元素自己的类型，同时核对缺参、重复和多余实参。未知长度的展开不猜固定槽位；同名关键字可由 `**kwargs` 接收，不会占用仅位置形参。
- 验证：新增一项运行回归和六项诊断回归。新运行用例在 O0/O1/O2 与 CPython 输出一致，并通过 ASan/LSan；普通及 ASan 各 222 例通过，差分 204/0，CTest 3/3。0.271 原有语料无输出差异，无新增超限文件。

## [0.299] - 2026-11-18

### 将推断出的返回类型传播到调用方

- 模块内普通函数和方法的返回类型在形参回填后发生变化时，使用最终签名作为提示，在新作用域中按源码顺序重查。更新的不只是调用表达式，还包括外层运算、赋值和下游形参检查。只在可脱离旧作用域的标量或容器返回类型变化时重放；生成器专用路径保持不变。
- 修复方法的仅关键字形参写错位置，以及浮点优先调用绕过混合类型诊断的问题。无标注形参混传浮点和非浮点时两种调用顺序都报错；显式 `float` 标注仍允许整数转换。本轮不新增多态实例化、类型参数语法或跨模块泛型。
- 裸容器返回标注只填类型洞：保留构造器、显式 `Any` 与固定位置上的类型；裸可变容器不因推断出元素类型而被锁定。发现已存储的容器元素与固定浮点标注不一致时给出诊断，不把整数位模式当成浮点数读。
- 验证：新增两项运行回归和五项诊断回归。新运行用例在 O0/O1/O2 对拍 CPython，并通过 ASan/LSan；普通和 ASan 套件各 221 例通过，差分 203/0，CTest 3/3。0.271 原有语料输出无差异，与 0.298 相比 15 项基准的 O2 汇编完全一致，无新增超限文件。

## [0.298] - 2026-11-18

### 纯标量列表的浮点下标写入跳过旧值读取（nbody −1.4% 至 −2.7%）

- **发射**：O1+ 的 checked 浮点 `IndexStore` 保留边界检查和负下标归一化；通过后仅在运行时 `elem_kind == 0` 时直接写入，不读旧值也不进行释放。非零种类走原路径，不用静态 `list[float]` 代替所有权证明。不改变列表长度、位图或 unchecked 发射。
- **取证**：在 `advance` 做独立汇编消融。把长度加载折进比较约少 5% 指令但耗时近乎不变；运行时 kind 0 守卫降约 2.5%。本输入专用的「强行去掉所有权检查和边界检查」仍需 28.76 ms，不能作为生产优化，说明这两项解释不了与 Codon 的全部差距。
- **实测**：对 `572c03b` 的专项绑核 21 轮（每侧每轮 5 次），`nbody` **33.40 → 32.51 ms（−2.7%）**，Codon 0.20.7 为 14.22 ms，输出相同。另测 10 项的 15 轮中，`nbody` **39.91 → 39.36 ms（−1.4%，p25 −1.1%）**，`spectral_norm` +0.3%，合计 +0.2%；其余八项没有生成新快路径。
- **验证**：新增 200 覆盖浮点正负下标覆盖、两个方向的越界及堆列表的覆盖与别名。O0/O1/O2、setjmp、分裂及图着色分配路径对拍 CPython，Win64 的 `nbody` 通过交叉汇编（未执行 Windows 程序）。差分 201/0、0.271 语料无差异、普通与 ASan 各 219 例通过、CTest 3/3、泄漏抽查 13 例通过；规模检查仍只有原有 5 处超限。

## [0.297] - 2026-11-18

### 守卫虚调用直接把整数结果交给累加（obj_dispatch −1.5%）

- **发射**：O2 后端只融合相邻的守卫零参数整数 `VCall → Add → StoreGlobal`，调用和加法结果均须块内单用。命中体和间接回退都将返回值留在 `%rax`，合流后直接加上调用前已求值的左操作数再写全局。回退可能改写全局或抛错，不重新读取左值，也不提前执行外层赋值。
- **实测**：对 `d7e1257` 绑核交替 15 轮，`obj_dispatch` 343.22 → **338.12 ms（−1.5%，p25 −1.6%）**；realistic 六项合计 1368.94 → 1364.61 ms（−0.3%），其余在 ±0.6% 内。另用适配后同一源码配对复测 11 轮：PyAdamas 344.04 → 330.31 ms（−4.0%），Codon 0.20.7 为 110.78 ms，三者输出相同。
- **未采用的实验**：以手改汇编让累加器跨迭代留在 callee-saved 寄存器，在真实慢路前写回；析构和虚调用改写/抛出样例对拍 CPython。独立消融在原负载和适配负载分别慢 2.8% / 7.0%，尽管执行指令少约 7.7%；硬件计数显示传统解码增加。与返回值融合同时采用也更慢，未改循环全局写回策略。
- **验证**：199 覆盖同一调用点的守卫命中、析构重入、回退正常改写和改写后抛错。O0/O1/O2、setjmp、分裂及图着色分配路径对拍 CPython，ASan/LSan 通过；Win64 基准已交叉汇编。差分 200/0、0.271 语料无差异、普通与 ASan 各 218 例通过、CTest 3/3、泄漏抽查 12 例通过；规模检查仍只有原有 5 处超限。

## [0.296] - 2026-11-18

### RC 快路条件转发全局接收者（obj_dispatch −13.6%）

- **发射**：O2 后端匹配同块的「读旧全局、发布新值、`RcDec`、再读同一全局」。不改 IR/活跃性，也不撤掉 0.295 的重入屏障。只有没有运行时调用的减计数快路才复用已发布值；一旦走慢调用，返回后重新读全局。零额外实参的 `VCall` 可直接从 ABI arg0 消费单用结果，中间最多跨过一个不同全局的整数读；相邻 `GenNext` 用原结果栈位置。既有所有权、异常边和慢路浮点保存不变。
- **实测**：对 `040d1d5` 绑核交替 15 轮，`obj_dispatch` **398.20 → 344.07 ms（−13.6%，p25 −13.7%）**；realistic 六项合计 **1429.27 → 1373.42 ms（−3.9%）**，其余在 ±0.8% 内。适配后的同源源码配对复测 11 轮：PyAdamas 389.69 → 343.71 ms，Codon 0.20.7 为 110.54 ms，三者输出一致。
- **验证**：新增 198 用例在同一调用点执行带写入的析构慢路和非归零快路；慢路同时改写全局接收者与累加器。O0/O1/O2、setjmp、分裂和图着色分配路径均对拍 CPython；检查汇编确认慢调用后重新读取全局。差分 199/0、0.271 语料无差异、普通与 ASan 各 217 例通过、CTest 3/3、LeakSanitizer 抽查 11 例通过。Win64 基准守卫交叉汇编通过，完整用例与基线同样受扫描位图段指令限制，未验证 Windows 运行；规模检查仍只有原有 5 处超限。

## [0.295] - 2026-11-18

### 禁止全局转发跨过可重入的 RC 释放，撤回显式虚调用守卫实验

- **正确性**：释放旧生成器可能在 `finally` 中重入并覆写刚发布的全局对象。0.290 的全局转发错误地跨过了 `RcDec`，接下来把已释放的值传给 `next`；新增 197 用例在旧 O2 上触发 ASan heap-use-after-free。现在 `RcDec` / `RcDecUnknown` / `RcCleanup` 清空全局转发状态；边界版本化同样不把会释放引用的调用当作无修改操作。保持 retain → 发布 → release 的顺序。
- **代价**：对 `2726794` 的 15 轮绑核 A/B 中，`obj_dispatch` 因恢复释放后的全局装载，从 339.89 → **399.38 ms（+17.5%，p25 同为 +17.5%）**；六项合计 +4.8%，其余在 −0.3% 到 +1.3% 间。未放宽屏障来掩盖退步。
- **撤回的实验**：另用未修改的 0.294 对照显式 `FieldLoad / ClassAddr / Cmp / Branch`、整数合流槽和原虚调用慢路。15 轮绑核随机交替，`obj_dispatch` **333.05 → 436.75 ms（+31.1%）**，指令约 +16.6%；跨块虚表/类地址往返和布局变化抵销了原快路径。实验未合入，0.294 的后端守卫继续保留。下一步先减少守卫地址搬运，再以可重入慢路为边界证明全局与容器状态，不能靠放行所有 RC 调用来提升循环。
- **验证**：197 在 O0/O1/O2 和 setjmp 路径与 CPython 一致，ASan/LSan 通过。差分 198/0、0.271 语料无差异、普通与 ASan 各 216 例通过、CTest 3/3、泄漏抽查 10 例通过。初次并发 CTest 的码点计时斜率失败；该用例基线和修复版汇编相同，单测和低并发全套复跑通过。规模检查仍只有原有 5 处超限。

## [0.294] - 2026-11-18

### 小型整数虚方法的守卫内联（obj_dispatch −11.7%，realistic 合计 −3.3%）

- **发射**：O2 的无额外实参、返回整数的普通 `VCall`，最多为四个本模块实际虚表生成守卫。只接受整个叶子方法 IR 为整数常量返回、读 `self` 的整数实例字段、或两个此类字段的加减乘。继承同一方法的类分别比较实际虚表地址，共用内联体。命不中、候选过多、方法带副作用或不满足形态时保留原间接调用。静态类型只筛候选，不证明实际对象类型；不改变 IR、RC 或现有调用 ABI/EH。
- **实测**：对 `1ac7acd` 绑核交替 15 轮，`obj_dispatch` 385.65 → **340.44 ms（−11.7%，p25 同为 −11.7%）**；realistic 六项中位数合计 1410.17 → **1363.54 ms（−3.3%）**，其余用例在 ±0.5% 内。另用相同的 Codon 兼容源码配对复测 11 轮：PyAdamas 380.95 → 339.83 ms，Codon 0.20.7 为 110.53 ms，三者输出相同。
- **验证**：新增 196 用例覆盖继承、覆盖、经 `Any` 到达的同槽非子类、带写入和抛错的回退。O0/O1/O2 及 setjmp 路径对拍 CPython，O2 汇编确认守卫和原间接回退并存。差分 197/0、0.271 语料无差异、普通与 ASan 各 215 例通过、CTest 3/3、LeakSanitizer 抽查 9 例通过。Win64 目标基准的守卫汇编通过交叉汇编，未验证 Windows 运行；规模检查仍只有原有 5 处超限。

## [0.293] - 2026-11-18

### obj_dispatch 栈转发实验撤回，保留全局释放顺序回归

- **旧全局值直接传给 `RcDec`**：在相邻的 `LoadGlobal → StoreGlobal → RcDec` 中，把仅供释放的旧值直接读入 ABI 首个整数实参寄存器。反汇编确认少了一次栈写和读回；对 0.292 的 15 轮绑核打乱顺序对照（每侧每轮 5 次），`obj_dispatch` 从 378.42 变为 **403.88 ms（+6.7%，p25 +6.8%）**。单独的硬件计数显示指令 −2.9%、周期 +6.5%，DSB 微操作减少、MITE 微操作增加，与布局改变导致更多传统解码相符。
- **整数虚调用结果留在 `%rax`**：独立试验 `VCall → Add → StoreGlobal`，保留调用前的累加器快照，用 `addq` 直接更新返回寄存器。同一轮对照 **379.40 ms（+0.3%，p25 +0.3%）**，无可辨收益。另试把列表长度加载折进边界比较，短测也变慢。三项均撤回，不改变编译器发射。
- **回归**：新增 195 用例。旧生成器归零执行 `finally` 时必须读到已发布的新全局；如果它重入写入第三个值，外层赋值不能覆盖该结果。还覆盖空旧值、自赋值和两个全局共享同一生成器。O0/O1/O2 和 setjmp 路径对拍 CPython；差分 196/0，0.271 语料无差异，普通与 ASan 各 214 例通过，CTest 3/3，LeakSanitizer 抽查 8 例通过。规模检查仍只有原有 5 处超限。

## [0.292] - 2026-11-18

### 精简普通虚调用发射（obj_dispatch −3.5%，realistic 合计 −0.7%）

- **发射**：普通 `VCall` 先写栈实参，把 `self` 装进 ABI 的首个整数实参寄存器（SysV `%rdi` / Win64 `%rcx`），用它读取 vtable，再直接发射 `call *slot(%r11)`。每次少一次重复的 `self` 装载和一条显式槽加载；仍按实际对象的虚表分派，不改 IR、引用所有权或调用约定，静态和 `super` 调用未改。
- **实测**：对 `1421c8f` 绑核交替 15 轮、每侧每轮 5 次，`obj_dispatch` 399.02 → **385.20 ms（−3.5%，p25 同为 −3.5%）**；六项中位数合计 1418.28 → **1408.31 ms（−0.7%）**。`dict_wordfreq` +1.3%，其余变化在 ±0.3% 内，六项输出与基线一致。
- **验证**：新增 194 用例覆盖非零槽、继承和重写、混合实参（SysV 整型和浮点各两个落栈）、浮点返回和异常捕获，O0/O1/O2 与 setjmp 回退路径均对拍 CPython。差分 195/0、0.271 语料对照无差异、普通与 ASan 各 213 例通过、CTest 3/3、LeakSanitizer 抽查 7 例通过。Win64 混合实参抽查交叉汇编通过；带捕获绑定的完整用例仍触发基线也有的扫描位图段指令问题，未验证 Windows 运行。文件规模检查仍只有原有 5 处超限。

## [0.291] - 2026-11-18

### 缩短引用计数热路径（obj_dispatch −8.9%，realistic 合计 −2.8%）

- **取证**：`obj_dispatch` 的 6400 万轮内层循环每次都 retain 新元素、发布全局循环变量、release 旧元素。采样约 82% 落在生成的主函数；反汇编显示每次增减还要先装入并比较 64 位永生哨兵。优化只缩短发射序列，不改变全局绑定、引用所有权或虚调用。
- **发射**：普通堆值把 `INT64_MIN` 哨兵判断移到冷分支；`RcInc` 在非正计数上用 `negq/jo` 识别哨兵，`RcDec` 用 `subq $1` 同时做有符号比较与减法，只有原计数大于 1 才写回，冷分支借溢出标志识别哨兵。空指针、GC 收集期、计数归零及非法计数仍按原条件走慢路径；两者均使用目标 ABI 的首个整数实参寄存器。
- **测量取舍**：对所有类型统一推迟哨兵判断的实验让 `str_build` 慢 4.7%；最终版对 `str` / `bytes` 保留优先判断永生值。对 `4108e72` 绑核交替 15 轮（每侧每轮 5 次）：`obj_dispatch` 437.94 → **399.09 ms（−8.9%，p25 同为 −8.9%）**，`str_build` +0.4%，`dict_wordfreq` +1.4%，六项中位数合计 1465.20 → **1424.47 ms（−2.8%）**。
- **验证**：新增 193 用例覆盖空的旧全局值、清空列表后仍存活的循环变量、永生字符串与动态字符串交替赋值，O0/O2 对拍 CPython。差分 194/0、0.271 语料对照无差异、普通与 ASan 各 212 例通过，LeakSanitizer 抽查 6 例通过，新用例 Win64 交叉汇编通过。高并发 ASan 跑 `27_runtime.py` 曾有一次取消清理输出缺行；基线/候选各单独复跑 5 次及低并发 ASan 全套均通过。文件规模检查仍只报原有 5 处超限。

## [0.290] - 2026-11-18

### store.global→load.global 转发（obj_dispatch −13.7%，套件 −4.8%）

- **取证**：obj_dispatch 的模块级 `for s in shapes: total += s.area()` —— s 是全局 g8，每轮 store g8 之后又 load g8 当 vcall 的 self（STLF 往返 + 一次全局读，内层 64M 轮）。0.289 的 annotate 里全局读/写合计 ~25%。
- **forward_global_loads**（opt_forward.cpp）：块内 load.global 读到刚被 store.global 写入的全局，且中间只隔纯 IR 指令或 RC 族 CallRt → 直接转发到存入的 vreg，删除 load。要求装载结果块内只用一次（不延长跨块活跃区间）+ 类型一致；GlobalAddr / 用户调用（Call/VCall/…）/ CallExtern / GenYield 使映射失效。
- **安全论证**：用户全局只由生成代码的 StoreGlobal 写入；pyadamas 不支持 `__del__`（全仓库 grep 核实），RC 析构全是运行时内部代码（list_destroy/dict_destroy 等），不碰用户全局；sort 的 key 在生成循环里预算好（ListSortedBy 收 keys 列表），运行时会调用户代码的入口（FuncReduce 族等）不在 RC 白名单里，一律失效映射。
- **实测（15 轮）**：obj_dispatch 508.73 → **439.15 ms（−13.7%**，p25 −13.9%）；rec_memo −0.8%、str_build −0.6%（模块级赋值即读形态同样受益）；其余中性；套件合计 **−4.8%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 192 / 66_genexpr / 119_gen_except_suspend 抽查）。

## [0.289] - 2026-11-18

### Bin+StoreGlobal 融合（obj_dispatch −5.7%，套件 −1.5%）

- **取证**：obj_dispatch 的 `total += s.area()` 内层 64M 轮，每轮把 add 结果写进 vreg 栈槽再读回来写全局（`mov %rax,0xf8(%rsp)` + `mov 0xf8(%rsp),%rax` 的 STLF 往返，annotate 里合计 ~10%）。
- **can_fuse_global**：整数 Bin（Add/Sub/Mul/位运算/移位）+ StoreGlobal，结果块内只用一次且不跨块 → bin 结果留在 %rax，紧跟一条 `movq %rax, global`，省掉栈槽往返。StoreGlobal 是纯 store（RC 由降低器单独发 inc/dec），全局根本身就是那个符号，无 GC 帧扫描问题（与 StoreSlot 融合的坑不同），任意类型安全。
- **实测（15 轮）**：obj_dispatch 540.29 → **509.55 ms（−5.7%**，p25 −5.6%）；rec_memo +1.8%（perf stat：指令 −0.5% 但 cycles +1.6%，DSB 近平 —— 函数尺寸变化的布局效应，权衡接受）；wordfreq +0.9% 同类；str_build −0.2%；套件合计 **−1.5%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 192 / 66_genexpr / 119_gen_except_suspend 抽查）。

## [0.288] - 2026-11-18

### str 相等单字节内容快路径（str_build −6.5%，套件 −0.8%）

- **取证**：pa_str_eq 仍占 str_build 8.4% —— 指针相等快路径早已存在，但 `c == "0"` 有 9/10 是**不等的数字字符**（驻留指针不同）→ 每次为一个字节比较走完整 call。
- **gen_cmp 扩展**：指针不等后双方 len==1 直接比 data[0]（str/bytes 同布局：len+8、data+24）；不等直接出结果（Eq→0 / Ne→1），len!=1 才落回 pa_str_eq。顺带删掉 Eq/Ne 分支前重复的实参装填。scratch 用 r10/r11 避开 Win64 实参寄存器。
- **两个汇编坑（记录警示）**：movzbq 目的必须 64 位寄存器 —— %al 报 operand size mismatch，%eax 报 q 后缀不允许 32 位，正确是 %rax 再 cmpb %al 比低字节。首版让所有含 str 相等的用例编译失败，**difftest 因 O0/O2 同样失败而假绿（193/0）**，套件 211→189 才暴露。教训：difftest 全绿还要看套件计数，两侧同样失败不算正确。
- **实测（15 轮）**：str_build 160.47 → **150.07 ms（−6.5%**，p25 −6.2%）；dict_wordfreq −0.9%；其余中性；套件合计 **−0.8%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净。

## [0.287] - 2026-11-18

### str 下标 ASCII 快路径内联（str_build −6.2%，套件 −0.6%）

- **取证**：str_build 逐字符下标走 pa_str_char_at（5.3%）+ pa_char_str（7%）两层 call。ASCII 字符命中 256 项驻留表（零分配、永生），调用开销纯浪费。
- **gen_index_load 的 Str 分支内联快路径**（提取为 gen_str_index_inline，避免 codegen_call.cpp 超限）：`0<=i<pub` 且 `pub==len`（str 不变量：等号当且仅当全 ASCII）且字节 <0x80 → 直接查 pa_char_tab（main 序言已初始化）。负索引/越界/非 ASCII 落回 pa_str_char_at（归一化、IndexError、UTF-8 码点路径语义不变）；高字节守卫是防御性的。
- **ABI**：s/i 走 arg_reg_i，scratch 用 rax/r10（两 ABI 均 volatile 且避开实参寄存器）；快路径不碰 xmm，慢路径 spill/reload。
- **实测（15 轮）**：str_build 171.10 → **160.49 ms（−6.2%**，p25 −6.0%）；queue_sim −0.5%；其余中性；套件合计 **−0.6%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净；unicode（héllo世界）/负索引/越界 IndexError/空串边界对拍 CPython 逐行一致。

## [0.286] - 2026-11-18

### 临时栈 push/take 内联快路径（str_build −5.3%，套件 −0.6%）

- **取证**：str_build 逐字符循环里 pa_rc_temp 占 6.8%。字符串是永生值（256 项驻留表），push 读 rc 比较 INT64_MIN 就返回；take 更要把整条栈白扫一遍（永生值从不入栈，永远找不到）。call/ret + 扫描全是浪费。
- **gen_rc_inline 扩展 RcTemp / RcTempTake**（仿 inc/dec 先例）：永生 push/take 5 条指令（null 检查 + rc 加载 + INT64_MIN 比较）；非永生 push 12 条（stats/diag/扩容守卫落回原函数）；take 内联栈顶命中 + 从后往前扫描 + swap-remove，与运行时逐位一致。运行时 pa_rc_temp_take 同步加永生早退（语义等价：pa_rc_temp 跳过永生值，栈上不可能有）。
- **寄存器安全**：CallRt 所在块本就没有 GPR/FP 临时值（既有门控）；快路径不碰 xmm，慢路径 spill/reload_fp_slots（析构可能跑，遵循 inc/dec 先例）。
- **实测（15 轮）**：str_build 180.36 → **170.79 ms（−5.3%**，p25 −5.4%）；其余中性；套件合计 **−0.6%**。RC stats 模式输出一致（temp=132000 / cleanup=132000 平衡；take=0 因 opt_rc 后 str_build IR 已无 take 指令，push/cleanup 配对）。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 66_genexpr / 119_gen_except_suspend / 192_dict_cache 抽查）。

## [0.285] - 2026-11-18

### dict set 单探测 + 标量销毁快路径（wordfreq −1.7%；两条方法论教训）

- **set 单探测**：pa_dict_set 原来先 dict_lookup 找已有 key，未命中再 dict_place_slot 从同一 hash 重探一遍找空槽/墓碑 —— 新键插入走两遍探测链。新增 dict_lookup_place：探测路上顺带记第一个墓碑，命中返回 entry，未命中把可写槽写进 out 参（与 place_slot 逐位一致）。reindex 会重建索引，触发后重算 place。bump 未命中路径也从「lookup+lookup+place」三探降到两探。
- **标量销毁快路径**：dict_destroy / dict_gc_release / pa_dict_clear 在 key_own==0 && val_own==0 时跳过整条 entry 链游（无 RC 可做）。rec_memo 的 dict[int,int] 每轮 25 项链游是循环携带指针追逐。
- **教训一 —— gcc 内联扰动**：新增 dict_lookup_place 后 gcc 的内联预算把 dict_key_matches 从 dict_lookup 里 outline 成公共函数（两处调用共享），wordfreq 每次探测多一层 call/ret，**指令 +6.3%、cycles +4.5%**（perf stat + objdump 证实：old lookup 94 行内联形态 → new 63 行带 call）。修复：dict_key_matches 加 always_inline，恢复紧探测循环。**同一翻译单元里加函数可能改变既有热函数的内联决策，性能回归要先看 objdump 再怀疑算法。**
- **教训二 —— perf 占比 ≠ 边际成本**：销毁链游在 perf 里占 rec_memo 11.6%，跳过后指令 −5.3% 但 **cycles ±0**（abbench 15 轮 p25 +0.0%）—— 链游的 L1 依赖加载全落在其它延迟的阴影里，OoO 把它完全隐藏。perf 的采样占比（含 skid）不能直接当「省掉就赚」的依据，必须 A/B 验证。
- **实测（15 轮）**：dict_wordfreq **−1.7%**（p25 −2.5%；perf stat cycles −6%、指令 −5%）；rec_memo 中性；其余中性；套件合计 −0.2%。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 88_defaultdict / 192_dict_cache 抽查）。

## [0.284] - 2026-11-18

### contains+get 双查找 IR 层融合（rec_memo −11.8%，套件 −2.0%）

- **0.283 的运行时末次查找缓存因 STLF 串行化撤回后，这版在 IR 层做融合**：新 pass `opt_dict_fuse` 识别 `%c = DictContains(d,k); branch %c, T, F` + T 的纯前缀后的 `DictGet(d2,k2)`（d/k 同槽），改写成 `DictTryGet(d,k)`（idx=新帧槽 S，found 落 %c 直接进分支寄存器）+ T 里 `load.slot S`。一次哈希+探测替代两次；值经帧槽中转但**同一迭代内一写一读，无跨迭代依赖链**（与失败缓存的本质区别）。
- **守卫**：T 的唯一前驱是 B 且 F≠T（别的路径进 T 会读到未写的槽）；contains→分支、T 入口→get 之间都是纯指令（调用/存储可能改 dict）；get 的值只在 T 内用（SSA 保证）；不匹配 `not in`（条件经 Un）等变形，保守。
- **新运行时入口 pa_dict_try_get**：命中写借用值到 *out 返回 1，缺失返回 0 不写 out、不创建 default（与 contains 守卫下的 get 逐位等价；defaultdict 的缺省创建只发生在真 get，这里缺失走 F 分支，语义不变）。
- **开发中修掉的坑**：值槽没有 StoreSlot 定义 → 寄存器提升的活跃分析视其为入口 live-in，块入口重载逻辑见「所有前驱都持有寄存器」就不重载 —— 运行时写栈后提升寄存器不更新，读陈旧值（rec_memo 输出 390000、difftest 两例红）。修复：DictTryGet 的值槽与 SlotAddr 一样排除提升，一律留在内存（堆类型槽本就不提升，整型槽才受影响）。
- **实测（15 轮）**：rec_memo 297.69 → **262.57 ms（−11.8%**，p25 −12.1%）；profile 里 pa_dict_contains / pa_dict_get 消失，pa_dict_try_get 8.3%，dict_lookup 27% → 22.7%；其余用例中性；套件合计 **−2.0%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 88_defaultdict / 192_dict_cache / 90_functools_final / 39_match_advanced 重点抽查）。

## [0.283] - 2026-11-18

### dict 末次查找缓存：尝试 → 实测负收益 → 撤回（STLF 串行化诊断）

- **尝试**：rec_memo 的 `if n in memo: return memo[n]` 是 contains+get 双查找（dict_lookup 占运行时 27%）。给 PyAdamasDict 加末次查找缓存（cache_key/hash/entry/valid，+32 字节）：同键连续查找命中直接返回 entry 下标，省 hash+probe；插入/擦除/reindex 失效。正确性通过 20000 步确定性伪随机操作对拍 CPython + 边界用例（del+空闲 entry 复用、扩容、墓碑清理、相等异指针 str 键、±0.0、pop/clear/update）。
- **实测负收益**：rec_memo **+8.8%**（abbench 15 轮，p25 +8.7%），dict_wordfreq 中性，其余噪声。撤回。
- **判别实验诊断**：保留缓存写入、禁用命中检查的变体 cycles=945M，完整缓存 886M，基线约 800M —— 命中确实省了 ~59M cycles，但**每次查找的缓存写入花掉 ~145M（≈7.6 cycles/次）**。根因：缓存字段在 dict payload 第三条 cache line，**相邻两次查找经缓存字段形成循环携带的 store→load 依赖（STLF ~5-7 cycles 串行）**——本来独立的查找被串成链。与 0.278 余数旋转同一族教训（OoO 下内存依赖链比指令数更致命）。
- **结论**：运行时内存驻留的查找缓存是死路；可行的融合路径在 codegen 层（寄存器驻留的 found/value 对，如 DictGetOpt IR  op），留作后续候选。
- **保留**：`tests/run/192_dict_cache.py` 作为字典语义回归（含 expect 段）。首版测试曾因 LCG 乘法溢出 int64 与 CPython 大整数语义分叉而假失败——改 32 位 LCG（乘积 <2^61）后两边序列一致；这不是缓存 bug，是测试 bug，记录以免再犯。
- **验证（撤回后）**：差分 193/0；release 与 ASan 套件各 **211** ran ok / EXIT=0；abbench 对 0.282 全噪声级（运行时与 0.282 逐位一致）。

## [0.282] - 2026-11-18

### list.append 内联快路径 + 循环头 32 字节对齐（str_build −6.6%，套件 −1.2%）

- **append 内联**：`pa_list_append` 在无扩容（len<cap）、无位图（heap_bits==NULL）、非 kind2（kind2 空表运行时会分配位图）时就是 `data[len++]=v` 一条 store，call/ret + 装填 + 运行时检查比活本身还贵（str_build profile 里 pa_list_append 14.6%）。三个守卫任一不满足落回原调用。RC retain 不在这条指令里（生成代码在 append 前单独发 pa_rc_inc），语义不变。
- **重要发现 —— DSB（uop 缓存）布局敏感性**：append 内联初版让 obj_dispatch **+7.2%**。perf stat：指令 −0.5%、分支/branch-misses/L1-icache-misses 全不变、**cycles +9.4%**；`idq.dsb_uops` −228M / `idq.mite_uops` +176M —— 上游代码尺寸变化把热循环的 uop 挤出 DSB 窗口，落回 MITE 传统解码。循环头原来已 16 字节对齐（.p2align 4）但不够：**改 32 字节（.p2align 5，DSB 窗口粒度）后 obj_dispatch 大部分恢复**，且 queue_sim / dict_wordfreq 额外受益。64 字节无进一步收益（DSB 组冲突不由入口对齐单独解决）。**教训：codegen 小改动也可能通过 uop 缓存布局产生 ±10% 的远端效应；评估时看 idq.dsb_uops / idq.mite_uops，循环头对齐用 32B。**
- **实测（15 轮）**：str_build 193.32 → **180.61 ms（−6.6%**，p25 −6.6%）；dict_wordfreq **−5.3%**；queue_sim **−2.7%**；sort_struct −1.2%；rec_memo 中性；obj_dispatch **+2.9%**（残余 DSB 组冲突，perf stat 证实非分支/非 icache，权衡接受）；套件合计 **−1.2%**。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；ASan detect_leaks=1 干净。

## [0.281] - 2026-11-18

### dict 缓冲池化 + 去掉双重 mix64（rec_memo −32.4%，套件合计 −8.4%）

- **取证**：rec_memo（每轮新建 memo dict 跑 fib(25)，195000 轮）profile 里 dict_lookup 19.6% + dict_hash_value 17.6% + pa_dict_set 9.1% + dict_destroy 7.3% + dict_reindex 6.7% + malloc 4.7% + free 2.8% ≈ 75% 在字典路径。每个新 dict 三次 malloc（entries/order/index）+ 25 个键触发 3 次 reindex（icap 8→16→32→64，每次 calloc+free+重哈希）+ 销毁三次 free。
- **缓冲池化**：dict_pool 原来只池化 RC 头块，dict_destroy 把三块缓冲 free 掉。改成**小 dict（cap≤64、icap≤128）缓冲随头一起保留**：缓冲指针寄存在池节点（数组栈，256 槽）里 —— payload 会被 rc_alloc_core 的 memset 清零，不能留在 payload；分配命中后 memset 之后写回。pa_dict_new 见 entries 非空即走复用路径：状态重置 + index 清零，**零 malloc、零增长**（缓冲已是上一世高水位，25 键的 memo 不再 reindex）。大 dict 缓冲直接 free 不保留，防池囤内存；GC 清扫路径（gc_free_body）自己 free 缓冲，不经池；shutdown drain 连缓冲一起 free（ASan 泄漏干净）。
- **去双重 mix64**：int/float 键哈希原来 mix64 两次（单值路径一次 + 公共尾部又一次）—— 单次 mix64 已是全雪崩双射，第二次纯浪费。字符串路径提前返回不受影响，与编译期字面量哈希（FNV+单次 mix64）一致；hash() 内置与字典共用同一函数，同值同哈希不变。
- **实测（15 轮）**：rec_memo 439.14 → **296.98 ms（−32.4%**，p25 −32.4%）；dict_wordfreq −2.9%；sort_struct −1.2%；queue_sim +1.2%（rc_alloc_core 多一个 DICT 池命中判断的分支/布局代价，权衡接受）；其余噪声。套件合计 **−8.4%**。profile 里 malloc/free/reindex 全部跌出前十，dict_hash_value 17.6% → 10.6%。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 88_defaultdict / 66_genexpr / 119_gen_except_suspend 重点抽查）。

## [0.280] - 2026-11-18

### kind-0 元组分配内联快路径（queue_sim −16.5%）+ 四个潜在 bug 修复

- **内联快路径**：`gen_make_list` 的 kind-0 元组（全非堆元素、n≤4）在池命中且 `gc_need_check==0`、diag 关闭时，直接在生成代码里弹池、初始化（0.276 不变量：池块 dtor/release/kind/size 恒定，只重置 rc/marked/prev/next + payload），省 call/ret + 参数装填 + 运行时检查；任一守卫不满足落回 `pa_tuple_new_filled` 原路径（GC 触发/assist 点语义不变）。perf 里 pa_tuple_new_filled 跌出前八。
- **bug 1（发射，致命）**：内联路径返回 RC 头指针 h 而非 payload h+64 —— 槽里存了头指针，dec 的 p-1 落到前一个对象内部，池链被 interior 指针污染，级联破坏。调试实录：ASan heap-buffer-overflow（读 argv 块尾后 16 字节）→ gdb 池头/数据字段 watchpoint → 抓到 put 参数是 `__file__` 字符串内部 → 反推返回值差 64。修复：返回前 `leaq 64(%rax), %rax`。
- **bug 2（发射，致命）**：守卫用 `cmpq`（8 字节）比较 4 字节 int 全局（gc_need_check / pa_diag_rc_on），把相邻 .bss 变量读进高 4 字节 → 永真 → 快路径从不命中（174 万个元组全走慢路径，计数器 fast=0 暴露）。gdb 单步 + eflags 证实 ZF=0 而 flag=0。修复：`cmpl`。
- **bug 3（运行时，0.274 起潜在）**：`pa_rc_dec_heap_batch` 的元组快释放入池前没把 rc 归零（池不变量 rc==0，dec_slow 的「dec on freed」panic 靠它挡二次释放）——stray dec 会静默二次 free → 池重复块。修复：入池前 `h->rc = 0`。
- **bug 4（运行时，潜在）**：`gc_need_check` 唯一清除点是 gc_incr_finish，小堆同步收集从不走增量 → 标志首次置 1 后永久滞留，每次分配白跑检查块（也挡住内联快路径）。修复：检查块无动作时清标志；记账在每次被跟踪分配接近 7/8 阈值时重新置 1、增量启动也置 1，触发时机不变（gc_runs=85 前后一致）。
- **实测（15 轮，0.279 修复后的 abbench）**：queue_sim 188.59 → **157.55 ms（−16.5%**，p25 −16.6%）；str_build −1.8%；sort_struct **+2.2%**（perf stat：指令 −0.4% 但 cycles +2.6% —— 内联序列在小 key 函数里的分支/布局代价，权衡接受）；其余噪声级。queue_sim 对 Go 40ms：5.1× → **3.9×**。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；ASan detect_leaks=1 干净；最小复现（纯元组 churn、元组+列表循环）对拍 CPython 一致。

## [0.279] - 2026-11-18

### 浮点临时寄存器分配与 GPR 门控解耦（spectral_norm −19.9%）+ abbench 测量可靠性修复

- **根因**：`assign_block_temp_regs` 用同一个安全门控管 GPR 池（r8-r11）和浮点池（xmm3-7）：spectral_norm 的 eval_a 内联进内层循环后含整数 `//2`（魔数序列用 GPR scratch）→ **整块被踢出临时分配**，所有浮点中间值被迫走栈往返（divsd/mulsd 读写内存，perf annotate 里 `mulsd 0x1d0(%rsp)` 一片）。
- **修复**：新增 `fp_temp_op_ok` 独立门控（定义在 codegen_ops.cpp）：整数 div/mod/floordiv 只用 GPR scratch（panic 调用不返回）→ 浮点安全；浮点 FloorDiv/Mod/Pow 调 floor/fmod/pow 会正常返回 → 不安全；列表 IndexLoad 越界慢路径不返回 → 安全，字符串 IndexLoad 调 pa_str_char_at 会返回 → 不安全；CallRt（含 RC 慢路径）→ 不安全。xmm3-7 只被会正常返回的调用破坏；浮点算术 scratch 是 xmm0/1/2，与临时池不相交。两池独立门控后，含整数除法的浮点热块也能拿到 xmm 临时寄存器。
- **实测（修复后的 abbench，15 轮）**：spectral_norm 74.79 → **59.93 ms（−19.9%**，p25 −20.0%）；其余 8 个 classic 用例生成二进制**逐字节相同**（cmp 证实），差值在噪声带内。
- **abbench 可靠性修复（重要）**：对照实验（两侧同一编译器）发现短用例（<100ms）锁进机器周期性干扰的固定相位 —— 固定用例顺序 + A 恒先于 B，**同一份二进制量出 ±20% 假差**（fannkuch +19.7%、fasta +21.8%、fib −14.6%）。修复三条：每轮 `sort -R` 打乱用例顺序；奇偶轮交换 A/B 先后；`BURST=5` 连跑取平均（覆盖干扰周期）。修复后对照：全用例 ±3.5% 内、多数 ±1%。**历史短用例数据需用新工具复核**；长用例（queue_sim 等 >200ms）跨多个干扰周期，历史数据仍可信。
- **验证**：差分 192/0；release 与 ASan 套件各 210 ran ok / EXIT=0。

## [0.278] - 2026-11-18

### 修 0.277 嵌套同名提示阴影洞；增量余数强度削减实测负收益，撤回

- **正确性修复（0.277 的洞）**：非负提示原来用 save/restore 包围循环体 —— 嵌套**同名**循环会重写同一个槽，内层退出后恢复的外层标记把内层终值当外层计数器值。反例：`for i in range(3): for i in range(-4, 0): pass; s += i % 3` —— 内层结束后 i=-1，恢复的外层标记让它走无符号路径，结果错。改成单层集合 + **入口擦目标槽旧键、退出擦本层键**：外层提示从内层入口起永久作废（运行期值已是内层的），不同槽嵌套互不影响。回归并入 `tests/run/191_rem_strength.py`。
- **增量余数强度削减（尝试 → 撤回）**：曾把循环体里 `i % 正常量` 替换成旋转余数槽（循环前 rem=start%c，incr 里 rem=rem+1、遇 c 归零，cmp+branch 替代魔数乘法链），含 AST 预扫描、load 识别、写槽作废。**实测负收益**：queue_sim **+4.2%**（p25 +4.6%）、str_build +4.5%、合计 +1.3%。perf stat：指令 −0.2%、分支 +3.9%、branch-misses −67%、**cycles +6.1%**。根因三条：① 旋转是**循环携带的串行依赖**（rem_k → rem_k+1），魔数链各轮独立、乱序窗口里流水重叠 —— 省指令不等于省周期（0.277 教训的延伸）；② 两个长活余数槽加剧 6 寄存器池竞争，mod 序列的 scratch 与余数槽寄存器冲突引发 spill/STLF 重载，rem 链拉长到 ~7-8 周期成为循环瓶颈；③ t%7 只在 1/3 迭代用到却每轮旋转 —— 急切旋转比按需魔数更贵。**结论：mod 延迟不在关键路径上（分支预测 + OoO 已隐藏），此方向关闭。**机制代码全部撤回，测试保留为取模语义回归。
- **重构**：nn / nonneg_of / 提示窗口助手移到 `src/lower_idiom.cpp`（新头 `lower_idiom.h`），`lower_stmt.cpp` 回到 1200 行内（1194）；`lower_impl.h` 只留内联胶水（def/alloc/store 钩子 + 声明）。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；190/191 对拍 CPython 逐行一致（负 start、重赋值作废、嵌套负 range 阴影、break/continue、生成器、INT64_MAX 边界、10 万轮压力）；ASan detect_leaks=1 干净。终态 abbench 对 0.277 全噪声级（sort_struct 生成二进制逐字节相同，cmp 证实）。

## [0.277] - 2026-11-18

### 非负被除数的无符号魔数除法/取模（指令数 −5%，queue_sim −0.4% / str_build −1.9% —— OoO 重叠教训）

- **非负打标**：降级期给可证非负的 vreg 打标（`IRFunction::vreg_nonneg`）：非负常量、len、非负槽的 load、非负操作数的 and/shr、除数非负的 mod（结果与除数同号）。**add/mul/sub 不打标**：int64 加法静默回绕，两非负数相加可能溢出成负，无界分析不能声称非负。
- **range 循环结构提示**：start 可证非负且 step==1 时，私有计数器只取 [start, stop] 的值（每次自增前有 ctr<stop 守卫，从 <= INT64_MAX-1 加 1 不回绕），循环变量由它赋值 —— 两个槽进 `nonneg_slots`，load 打标；体内写非非负值即作废（store_slot_raw，与 BoundsHint 同款机制）；体结束恢复外层提示集（嵌套/同名变量安全）。step!=1 不标记（大 stop 下 ctr+step 可能回绕）。提示可组合：内层 start 来自外层打标循环变量时同样生效。
- **无符号魔数路径**：被除数非负且除数为正时，|x|==x、商非负、余数天然在 [0,d) —— 省 abs/符号还原 4 条 + 余数符号修正 3 条；2 的幂商是逻辑右移、mod 退化成一条 AND。魔数本就按无符号域算（udiv_magic），直接复用。优化遍新建 vreg 无标（越界按 false），保守走有符号路径。
- **实测（abbench 交替 15 轮中位数，绑核）**：queue_sim 203.78 → 203.04 ms（−0.4%，p25 −0.4%）；str_build −1.9%（p25 −1.9%）；dict_wordfreq −1.0%、obj_dispatch −0.6%；sort_struct +1.8% 是噪声（生成二进制与基线**逐字节相同**，cmp 证实）；合计 −0.4%。
- **为什么指令 −5% 只换来 −0.4%**：perf stat 实测 queue_sim 指令 1536.5M → 1460.1M（−5.0%），cycles 508M → 506M。被省的符号修正指令本来就在乱序窗口里与魔数乘法链（movabs→mulq→add→shr→imul→sub，~9 周期关键路径）**重叠执行**，不在关键路径上 —— 指令数不等于周期。下一步若要真正缩短 mod 关键路径，候选是循环内增量余数强度削减（维护 r=(r+1)%c 的旋转寄存器，cmp+cmov 2-3 周期替代乘法链）。
- **验证**：新增 `tests/run/190_nonneg_divmod.py`（对拍 CPython 逐行一致：正常循环、负 start、体内写负作废、step2/while/循环外不标记、**INT64_MAX 边界逐项**、负除数/变量除数、嵌套同名、组合提示、and/shr/mod 传播、生成器；ASan detect_leaks=1 干净）。差分 191/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 209 ran ok / EXIT=0。
- **过程注记**：本轮期间并行文档代理截断了 `codegen_regalloc.cpp`（−1420 行，构建破坏），经用户授权回退该代理对 4 个源文件的改动（文档改动保留），0.277 在干净树上融合验证。

## [0.276] - 2026-11-18

### rc_alloc 本体瘦身：池命中不重写不变头字段，untracked 不入 GC 记账（queue_sim −4.6%，sort_struct −4.2%）

- **池命中跳过 dtor/release/kind/size**：这四个头字段按池恒定 —— 池路由按 kind（dec_slow），str 池恒 STR/NULL/NULL（str 分配点全在 rt_core），两个列表池恒 LIST/list_destroy/list_gc_release，dict 池恒 DICT/dict_destroy/dict_gc_release；运行期没有任何代码改写活对象的这四个字段（grep 核实），GC 清扫直接 free 不回池。四个 store 移进 malloc 分支，池命中只重置 rc/marked（prev/next 由 link/untracked 路径写）。**新增池化分配点必须维持「同池同 dtor/release/kind」不变量。**
- **untracked 对象不入 GC 记账**：调用方保证无出边、不成环，生死全由 RC 管理，GC 永远收不到它们 —— 让它们驱动 GC 频率纯属浪费。queue_sim 改前 gc_runs=231 / gc_freed=0（全是元组 churn 触发的空收集），改后 **gc_runs=85（−63%）**，RC inc/dec/free 计数逐位一致。入口的 gc_need_check 检查块不变：untracked 分配点仍是 mark assist / 收集的触发点，只是不再累加阈值计数；GC 频率只由被跟踪对象的分配量驱动（环只可能出现在被跟踪对象之间）。
- **实测（abbench 交替 15 轮中位数，绑核）**：queue_sim 211.84 → **202.20 ms（−4.6%**，p25 −4.7%）；sort_struct 220.53 → **211.32 ms（−4.2%**，p25 −5.5%）；dict_wordfreq / obj_dispatch / rec_memo 持平；str_build +0.8%（p25 +0.2%，中性）；合计 −0.9%。rc_alloc_core profile 12.2% → 9.0%。queue_sim 累计 420 ms（0.269）→ 202 ms，对 Go 约 5.1×。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；9 个 GC/异常重用例（13/151/160/161/174/188/189/92/93）单独 ASan detect_leaks=1 干净。

## [0.275] - 2026-11-18

### 净 push 计数消除 mark 脚手架 + 函数入口 32 字节对齐（queue_sim −7.6%，六用例全中性偏好，合计 −2.2%）

- **净 push 计数**：`move_fresh`（槽/容器 take 新值的所有权交接）原来只发 `RcTempTake` 不减 `rc_pushes` —— push 后立刻被 take 的值仍让语句/循环的 mark/cleanup 被判为「有临时值」而保留。现在 take 减一，既有消除机制（stmt / 循环 / 推导式 / with 的 `rc_pushes == pushes_before` → `erase_mark`/`erase_cleanup`）自然生效：queue_sim 的 mark/cleanup **19 → 0**，temp/take 对被 opt_rc 相邻对消除全部删掉，热路径上 RC 水位脚手架整体消失。`fresh_vregs` 的每个插入点都配对 `push_temp`，计数不会下溢；异常路径不受影响 —— 被消的 cleanup 本来就不在展开路径上跑，push 与 take 之间抛异常滞留的临时值由外层水位兜底（try 的 mark_slot landing pad、caller 语句水位、入口 fn_mark），与既有循环/推导式消除同款不变量。
- **布局回归与修复**：单独上消除时 obj_dispatch **+5.0%**（p25 +4.3%）。perf stat 证明是纯布局效应：指令数 / 分支数 / branch-misses 全部不变（被消脚手架在 obj_dispatch 的冷路径），cycles +5%，`frontend_retired.dsb_miss` 1.37M → 10.3M —— 代码缩短后热区跨 uop-cache 窗口，DSB→MITE 切换增多。试循环头 `.p2align 5` 无效；**函数入口 `.p2align 4` → 5**（32 字节，Cascade Lake DSB 窗口宽度）把 obj_dispatch 从 +5% 拉到 −2%，rec_memo（零消除、纯对齐效应）−1.1%。
- **实测（abbench 交替 15 轮中位数，绑核）**：queue_sim 232.14 → **214.50 ms（−7.6%**，p25 −7.8%）；obj_dispatch −2.0%；sort_struct −2.4%；dict_wordfreq −1.6%；rec_memo −1.1%；str_build +0.6%（p25 +0.1%，中性）；**六用例全中性偏好，合计 1981.14 → 1938.53 ms（−2.2%）**。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；GC/生成器/async/异常重用例（13/151/174/188/189/24/73/92/93/173/129/123/159）单独 ASan detect_leaks=1 干净。
- **教训**：消除冷路径代码要警惕前端布局效应 —— 指令数不变 cycles 也能摆 ±5%；用 perf stat 的 `idq.dsb_uops` / `frontend_retired.dsb_miss` 诊断，别急着判语义回归。函数入口 32 字节对齐是便宜的稳定器。

## [0.274] - 2026-11-18

### kind-1 列表元素批量 dec：销毁循环内联快路径与元组释放（sort_struct −4.8%，str_build −2.7%，合计 −1.9%）

- **`pa_rc_dec_heap_batch`**：`list_destroy` 的 kind-1（全堆）元素释放原来逐元素 `pa_own_dec` → `pa_rc_dec_fast` 内联判断 + rc==1 时 `pa_rc_dec_slow` 调用。新批量入口把 dec 快路径与 0.273 的 untracked 小元组释放快路径内联进循环，模式开关（统计 / 收集期 / 诊断）提到循环外 —— 任一开启就逐元素走 `pa_rc_dec` 原路径，语义逐位一致（含 rc<=0 panic、永生跳过、NULL 跳过）。queue_sim 的 pending 每步销毁一批 rc==1 的元组，常见情形零调用。
- `list_destroy` 的 kind 1 分派到批量；kind 2（位图/注册表）与 kind 0 不变；`list_gc_release`（GC 试探 unref）不变。顺手：`rc_alloc_core` untracked 分支不再先写 marked=0 再写 UNTRACKED（省一次冗余 store）。
- **实测（abbench 交替 9 轮中位数，绑核）**：sort_struct 236.26 → **225.01 ms（−4.8%**，p25 −4.2%）；str_build 223.43 → **217.41 ms（−2.7%**，p25 −2.4%）；queue_sim 233.20 → 231.20 ms（−0.9%，p25 −1.6%）；dict_wordfreq 中位 −4.8% / p25 −0.7%（噪声偏大）；rec_memo −1.0%；obj_dispatch 持平；六用例合计 −1.9%。
- **profile 变化**：list_destroy 6.8% → 1.0%，工作移进 pa_rc_dec_heap_batch 2.6% + list_inline_pool_put 0.9%；pa_rc_dec_slow 5.8% → 4.4%。queue_sim 剩余大头：生成代码 59%、rc_alloc_core 12%、copy_range 4%、dec_unknown 2.8%。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；8 个 GC/列表重用例单独 ASan detect_leaks=1 干净；PYADAMAS_RC_STATS=1 回退路径计数正常（dec=481 万 / free=276 万）。

## [0.273] - 2026-11-18

### 分配/释放快路径：untracked 元组省 link/unlink 往返，列表省冗余清零（queue_sim −16.1%，sort_struct −12.6%）

- **untracked 直接分配**：`rc_alloc_untracked_ex` 旧实现是「按普通路径分配（`rc_link`：堆链插入 + RC 索引 put + 新生代 add）再立刻 `rc_unlink`（链摘除 + 索引 del + 新生代 del）」—— 对象存活期间的观察态本来就是不在链/不在索引，往返纯属浪费，每个元组出生还在索引里留一个墓碑。`rc_alloc` / `rc_alloc_ex` 拆出 `rc_alloc_core(..., untracked)`，untracked 路径直接置 prev/next NULL + marked=UNTRACKED，跳过 link/unlink 往返。GC 分配记账口径不变（元组原本就计入）。queue_sim 的 rc_link+rc_unlink 从 7.2% 降到 2.7%（只剩被跟踪的列表）。
- **LIST 跳过 memset**：三个列表构造点（pa_list_new / pa_list_new_filled / pa_tuple_new_filled）都显式写满全部 6 个字段；data 缓冲只读 [0, len)，append/filled 先写后读，池块里的陈旧内容永远不会被读到。`rc_alloc_core` 对 LIST kind 跳过 memset（其余 kind 不变）。profile 里 memset 2.6% 消失。diag 钩子只读 RC 头，不受影响。
- **untracked 小元组释放快路径**：`pa_rc_dec_slow` 的 rc 归零分支里，kind LIST + UNTRACKED + size 112 + elem_kind 0 + 无位图 + 内联缓冲时 `list_destroy` 是空操作（无元素可 dec、无位图可 free、无外部缓冲可还），整块直接挂回内联池，省一次函数调用 + 三个分支；条件不满足（增长过 / 混合种类）照走 dtor 原路径。
- **实测（abbench 交替 9 轮中位数，绑核）**：queue_sim 279.45 → **234.34 ms（−16.1%**，p25 −16.6%）；sort_struct 270.19 → **236.20 ms（−12.6%**，p25 −12.3%）；dict_wordfreq −2.2%；str_build / obj_dispatch / rec_memo 噪声级；六用例合计 −3.9%。分段测量：跳 link/unlink + 清零单独 queue_sim −14.6%、sort_struct −11.9%；释放快路径再补约 2%。queue_sim 累计 420 ms（0.269）→ 234 ms，对 Go 约 5.9×。
- **新 profile**：生成代码 58%、rc_alloc_core 10.6%、list_destroy 6.8%（kind-1 列表逐元素 dec，必要工作）、pa_rc_dec_slow 5.8%、pa_tuple_new_filled 3.8%、copy_range 3.1%、append 2.9%。下一步候选：分配本体（池查找 + 头初始化 + 记账）与列表元素 dec 循环。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；7 个 GC 重用例（13/151/160/161/174/188/189）单独 ASan detect_leaks=1 干净。

## [0.272] - 2026-11-18

### 切片拷贝惯用法识别：range-append 循环批量化的（queue_sim −2.5%，收益低于预期，原因已定位）

- **惯用法**：`for k in range(lo, hi): dst.append(src[k])`（单语句体、步长常量 1、无 else、dst/src 静态类型都是 list、k 不同于 dst/src）在降级器里识别，替换成一次 `pa_list_copy_range(dst, src, lo, hi, heap)` 调用，省掉逐元素 append 调用、容量检查和生成代码循环开销。新增 `src/lower_idiom.cpp`（纯 AST + 静态类型匹配，失败不发射任何 IR）；调用点在 `lower_for_range` 参数求值之前。
- **为什么不直接改写成切片语法**：Python 切片对负索引归一化、越界静默截断；range 循环逐个取值、越界抛 IndexError，语义不同。运行时入口分两条路：快路径（0<=lo<=hi<=src->len）一次扩容 + memcpy —— kind0/heap0 纯值拷贝，kind1/heap1 拷贝后批量 retain，其余种类（未定 3 / 混合 2 / 静态冲突）逐元素 prep+append（定型、位图、保守 retain 全走原路径）；慢路径逐元素 pa_list_get+prep+append，负索引与 IndexError（部分结果保留）和原循环逐位一致。dst==src 别名安全：hi<=len 保证源区间整体在写入起点之下，扩容先于取指针，逐元素分支每轮重读 src->data。
- **语义护栏**：批量版只在正常结束补 k=hi-1（条件分支：lo<hi 才写，零迭代保持原值），循环中途抛异常时 k 的中间进度无法还原 —— 所以只在分叉不可观测时匹配：当前位置不在任何 try/with/finally 里（cleanups 空，异常必然离开本函数），且 k 是普通局部槽（非全局 / 闭包 cell/env / 推导式槽）。
- **注册**：RtFn::ListCopyRange 追加在枚举末尾（不移动既有编号）+ rt_name + gen_rt_i5（5 整型参数）+ ir.cpp container_use。ir_ops.h / lower_impl.h 只加注册行（+2/+1，两文件本已超限，逻辑全在新文件与 rt_list.c）。
- **实测（abbench 交替 9 轮中位数，绑核）**：queue_sim 286.69 → **279.53 ms（−2.5%**，p25 −3.0%）；dict_wordfreq −1.4%、obj_dispatch +0.2%、rec_memo +0.5%、sort_struct −0.2%（噪声）；str_build +1.5% 是噪声 —— 该用例**零匹配**，生成二进制与基线完全相同。六用例合计 −0.2%。quickbench 最好 7 次口径：queue_sim 279 ms、str_build 211 ms。
- **为什么低于交接预期的 20%+**：profile 定位 —— 旧拷贝循环只占 queue_sim 约 6%（生成代码 54.4% 里的一小块），批量版 pa_list_copy_range 现占 4.12%（其中 memcpy + 逐元素 retain 是必要工作），生成代码占比降到 50.8%。真正的大头仍是分配与 RC 生命周期：rc_alloc_ex 10.4%、pa_rc_dec_slow 7%、rc_link+rc_unlink 7.2%、list_destroy 5.7%、pa_tuple_new_filled 4%。下一步候选应瞄准这些（如元组/列表的分配合并、RC 慢路径），而不是继续在拷贝循环上挖。
- **验证**：新增 `tests/run/189_copy_range_idiom.py`（对拍 CPython 逐行一致：跨内联边界、循环变量终值、负索引、越界异常部分结果、别名、非空 dst、裸列表定型、混合种类、求值顺序副作用、不匹配形态退回、生成器、GC churn；ASan detect_leaks=1 干净）。**全语料对照**：tests/run 全部用例分别用 0.271 基线编译器与本编译器编译运行，输出与退出码零差异 —— 惯用法在降级器（-O0/-O2 都生效），difftest 的 O0/O2 对照抓不到这类改动，必须对拍旧编译器或 CPython。差分 190/0，release 与 ASan 套件各 208 ran ok / EXIT=0。

## [0.271] - 2026-11-18

### 小列表 header+buffer 合并分配（sort_struct −20.3%，queue_sim −3.8%，六用例合计 −4.0%）

- **合并分配**：容量 <=8 元素的列表（含 `pa_list_new_filled` / `pa_tuple_new_filled` 定长快路径）把 `data` 缓冲内联在「RC 头 + PyAdamasList」同一个 malloc 块尾部（64+48+64=176 字节），新增 `list_inline_pool`（最多缓存 64 块）。建/销各从两次池操作（`list_pool_get` + `list_buf_get` / `list_buf_put` + `list_pool_put`）合成一次。`rc_alloc` 拆出 `rc_alloc_ex(size, ..., extra)`，`h->size = size + extra`，释放时按 size 路由回对应池；untracked 变体同步（`rc_alloc_untracked_ex`，kind 0 元组用）。GC 记账把内联缓冲计入（原先缓冲字节不入账）。
- **内联缓冲是块内指针，不能 realloc / free**：新增 `list_buf_grow`，append / insert / 切片赋值三处增长统一走它 —— 内联时换 malloc + memcpy（内联区从此闲置，头块销毁时仍整块回内联池，下次复用重新启用），外部缓冲照旧 realloc。`list_destroy` 与 GC 清扫（`gc_free_body`）按 `data == list_inline_at(l)` 跳过块内指针的单独释放；shutdown 清空内联池，ASan 泄漏口径不变。
- **实测（abbench 交替 9 轮中位数，绑核）**：`sort_struct` 335.91 → **267.74 ms（−20.3%**，p25 −21.1%）；`queue_sim` 295.25 → **284.01 ms（−3.8%**，p25 −2.8%）；`str_build` −0.9%、`dict_wordfreq` −1.2%、`obj_dispatch` −0.8%、`rec_memo` +0.6%（噪声）；六用例合计 2171.72 → 2084.95 ms（−4.0%）。quickbench 最好 7 次口径：queue_sim 283 ms、str_build 214 ms。queue_sim 新 profile：`list_buf_get` 从 8% 降到榜外，生成代码占比 47% → 54.4%，GC 仍 0.06%；剩余大头是 `rc_link`/`rc_unlink`（合计 7.1%）与生成代码里的逐元素 append/retain —— 交接文档 §5 的切片惯用法识别是下一步。
- **验证**：新增 `tests/run/188_list_inline_buf.py`（append/insert/切片赋值三条增长路径越过内联边界、堆元素高频建/销复用、列表自环 + GC churn、元组快路径 kind 0/1、混合种类、大列表外部路径；输出与 CPython 逐行一致，单用例 ASan `detect_leaks=1` 干净）。差分测试 189 用例 / 0 不一致，release 套件与 ASan 套件各 207 ran ok / EXIT=0。
- **工具修复**：`abbench.sh` 写死 `taskset -c 0`，在 cpuset 为 24-47,72-95 的容器里 taskset 直接失败，六个用例全量出 ~12.5 ms 的「失败开销」假数字。改成从 `/proc/self/status` 的 `Cpus_allowed_list` 取第一个可用 CPU，测量前自检 taskset 可用性，不可用就报错退出。
- 文档：`docs/runtime.md` 列表节补合并分配布局与增长/销毁/GC 清扫的不变量。

## [0.270] - 2026-11-17

### 字符串/除法/RC 三条热路径内联，单字节 str 接运行期驻留表（queue_sim −32.9%，str_build −55.4%）

- **短串拷贝/比较内联，join 跳过空分隔符，个位数驻留**：新增 `sm_copy_bytes` / `sm_bytes_eq` / `sm_bytes_cmp`，`n <= 8` 时内联成几条 mov、`n > 8` 回落 libc（长串上 libc 的向量化仍最优）；`sm_join_fill` 遇到空分隔符整个跳过 —— 原来 `"".join` 对前 n−1 个元素各做一次 `memcpy(dst, sep, 0)`，空拷贝照样走 PLT 跳板，str_build 累计 **792 万次纯跳板往返**（perf 里 `__memmove` 8.8% 采样的来源）；`pa_str_from_i64` 对 0..9 直接返回 `pa_char_str('0' + v)`，不再每次 malloc + 释放（每轮 60 次 `str(i % 10)`，共 396 万次分配）。
- **常数除数除法用魔数乘法替换 `idivq`**：新增 `AsmGen::gen_div_const` / `gen_div_const_emit`，`/`、`//`、`%` 三个入口都先试它，能化简就完全不生成 `cqto + idivq + div_guard`。perf annotate 指到指令级：`simulate` 里 `t % 3` 与 `t % 7 + 1` 两个 `idiv` 合计占该函数 **47.6%** 采样，`build_line` 里 `i % 10` 占 **71.9%**。算法用 Granlund-Montgomery 的 round-up 魔数按 64 位重推（65 位魔数只存低 64 位的 `m'`），有符号走 `trunc(x/d) = sign(x)·floor(|x|/d)` 在无符号域算；2 的幂单独走移位修正 —— 魔数法在 2 的幂上会退化，第一版没分开就在这里翻车；除数 0 / −1 / INT64_MIN / 超出 int32 范围仍走原通用路径。正确性：**3058 万组 `(x, d)` 对拍 C 的 `/`，零失败**。
- **RC 记账内联**：`RcInc` / `RcDec` / `RcMark` / `RcCleanup` 四个 RtFn 在 `gen_call_rt` 里开专用发射，生成代码不再 `call pa_rc_inc/dec/mark/cleanup`，只有慢路径才调 `pa_rc_*_slow`。根因：四个函数体分别只有 1~6 条指令（`pa_rc_mark` 就是一条 `return temp_len;`），call/ret + 被调用者保存 + PLT 跳板比活本身贵一个数量级，perf 里四个函数合计占 18~20%。慢路径可能触发析构，**必须**补 `spill_fp_slots` / `reload_fp_slots`（xmm 全 caller-saved），快速路径不 clobber 寄存器所以不用落盘。0.240 内联的是运行时内部调用点，这一轮内联的是**生成代码**里的调用。
- **单字节 str 字面量接运行期驻留表**：导出 `PyAdamasStr* pa_char_tab[256]`（`char_tab` 的载荷指针），`pa_static_strs_init()` 在 main 序言调用（依赖 `pa_disable_unknown_index` 的结果，必须排在后面）；`AsmGen::char_tab_literal` 把单字节 ASCII 的 str 字面量生成成 `movq pa_char_tab+N(%rip), %rax`，与 `s[i]` 拿到的是同一个指针；bytes 字面量不参与（`pub` 语义不同）。`gen_cmp` 的 str `==` / `!=` 加指针相等快路径：相等直接出结果，否则才 `call pa_str_eq`。
- **两个 bug（差分测试都没抓到）**：① `str(单个数字)` 返回控制字符 —— `pa_char_str` 的参数是**字符编码**不是数值，0..9 快速路径错写成 `pa_char_str(v)`，`str(7)` 拿到 BEL（0x07）而不是 `'7'`（0x37），`len()` 仍是 1，只按长度看完全正常；② str `!=` 的慢路径忘了取反 —— 第一版只在指针相等快路径处理 `!=`，于是所有「内容不同」的 `!=` 恒为假。两个都是**新加的 `tests/run/187_str_smallint_divconst.py` 抓到的**（`line[i] != "3"` 的累加值从 91 变成 1），说明差分测试有真实盲区：它只比最终输出，187 个用例里没有「`str(单个数字)` 之后再打印/比较」的。
- **实测**：`queue_sim` 420 ms → **282 ms（−32.9%）**，对 Go 10.5× → 7.05×；`str_build` 496 ms → **221 ms（−55.4%）**，4.4× → 1.97×。验证：差分测试 188 个用例 / 0 不一致，`tests/run/*.py` 189 个文件，release 套件与 ASan 套件 EXIT=0。

<!-- 下面 0.244–0.269 是补写：这些版本提交时没有同步更新 CHANGELOG，2026-11-18 从
     commit message 原文回填。数字都来自当时的提交说明，但篇幅比逐条手写的条目短。 -->

## [0.269] - 2026-11-17

### 循环内全局变量寄存器化（微基准 −84%，真实基准无收益）

- 把循环里反复读写的模块级全局变量在循环体内寄存器化，窄版本 `promote_globals_in_loops` 已接入优化管线。
- 微基准 131.1ms → 21.1ms（−84%），但 6 个真实基准合计 −0.5%、优化遍零触发 —— **真实代码上无收益**，这条路线到此为止。

## [0.268] - 2026-11-17

### 字典计数器模式融合（dict_wordfreq −37.7%）

- 识别 `d[k] = d.get(k, 0) + 1` 这类计数器更新，融合成一次哈希查找 + 原地写回，省掉重复查表。
- `dict_wordfreq` −37.7%。

## [0.267] - 2026-11-17

### 短字符串哈希缓存（dict_wordfreq −12.5%，短键场景 −27%）

- 字符串对象新增独立的 `chash` 字段（偏移 32），第一次被字典当键时算好写回；SSO 短串和长串都能缓存。
- 原先短串的哈希每次现算，是字典热路径上的固定开销。`dict_wordfreq` −12.5%，短键场景 −27%。

## [0.266] - 2026-11-17

### 元组/列表构造快路径（sort_struct −42%，queue_sim −16%）

- 新增 `pa_list_new_filled` / `pa_tuple_new_filled`：定点构造 n≤4 的元素时一次调用写完，省掉 N+1 次 `pa_list_append`（每次还要走位图维护和容量检查）。
- `sort_struct` −42%、`queue_sim` −16%。

## [0.265] - 2026-11-17

### 索引访问热路径从 19 条指令降到 9 条（nqueens −8.3%）

- 照 Go 的边界检查结构重排 `index.get` 快路径：无符号比较一次挡掉越界，负索引归一化和浮点元素分派都挪进慢路径。
- `nqueens` −8.3%。

## [0.264] - 2026-11-17

### math.sqrt 内联（照 Go 的 SQRTSD + 冷路径，nbody −2.3%）

- `go tool objdump` 确认 Go 的 `nbody.advance` 里 0 次函数调用，而我们的 `pa_math_sqrt` 每次都 call 并额外溢出 3 个浮点寄存器。
- 改成 `comisd` 判负 → 非负直接 `sqrtsd`、负数才落回调用序列，语义一致。`nbody` −2.3%。

## [0.263] - 2026-11-17

### 图着色分配器（精确活跃集，matmul −8.6% / nbody −3.5%）

- 新增实验性图着色分配器（精确活跃集 + 溢出代价模型），环境变量 `PYADAMAS_RA_GRAPH` 开启，默认关闭。
- `matmul` −8.6%、`nbody` −3.5%。没有照抄 Chaitin-Briggs —— 在 SSA 化的 vreg 上直接建冲突图更简单。

## [0.262] - 2026-11-17

### split 分配器改为增量叠加 + 修两个真 bug（无性能收益）

- 把 split 从「重算整函数位置」改成「在默认分配之上增量叠加」，并修掉两个正确性 bug。
- **性能无收益**：TOTAL +27%。随后 0.269 的池容量实验证明容量本来就不是瓶颈，这条路线放弃。

## [0.261] - 2026-11-17

### 模块缓存键漏掉 rc_split（A/B 测试拿到假数据）

- 模块缓存的键里没有 `rc_split` 标志，于是开关 split 后复用了旧缓存的目标文件 —— A/B 对比测的其实是同一份代码。
- 这类 bug 最危险的地方不是慢，是**让实验结论整个作废**。修完后把 cache key 补上全部影响代码生成的开关。

## [0.260] - 2026-11-17

### live range splitting 基础设施（按块分配槽寄存器）

- WIP：按基本块为槽变量分配寄存器，把「整函数一个位置」放宽成「每块一个位置」。
- 只有基础设施，未做跨块的一致性编排，性能未评估。

## [0.259] - 2026-11-17

### 统一浮点寄存器分配尝试（失败，记录根因）

- 尝试让浮点变量和整型变量共用同一套寄存器分配框架。
- **失败**：浮点槽的提升位置同时被 `spill_fp_slots`/`reload_fp_slots` 的调用点编排依赖，两套机制混在一起会互相覆盖。根因已记录，代码回退。

## [0.258] - 2026-11-17

### 跨块浮点 vreg 提升（修两个真 bug）

- 把浮点栈槽提升到 xmm8–15，按 Load/Store 热度决定提升哪些。
- 修两个真 bug：跨块的浮点 vreg 在块边界没有正确的 spill/reload；提升过的槽和 `spill_fp_slots` 的写出顺序冲突。

## [0.257] - 2026-11-17

### vreg 栈槽改用活跃性分析（修死 vreg 撞槽）

- vreg 的栈槽分配原来只看「变量还活着」，改用活跃区间后不再让两个不重叠的 vreg 抢同一个槽。
- 修掉「死 vreg 撞槽」：一个已经不再使用的临时值覆盖了另一个仍然活跃的值。

## [0.256] - 2026-11-17

### join 不再销毁类型洞

- 控制流汇合点原来会把两侧的类型标注「求交」成洞（unknown），实际上洞一旦产生就再也补不回来。
- 改成保留精确的一侧；只有真正冲突时才落洞。

## [0.255] - 2026-11-17

### 类型洞统一用 fresh_hole，裸容器标注不再比不标注更差

- 所有产生类型洞的地方统一走 `fresh_hole`，避免同一个洞被两处标注复用后互相污染。
- 修掉「显式标注裸容器反而比不标注更慢」这个反直觉现象。

## [0.254] - 2026-11-17

### 裸 list 标注不再退化成 unknown（matmul 3.67x → 2.33x）

- 根因链三个环节都错：`list`（不写元素类型）被当成未知元素类型、元素读写走进保守的 unknown 路径、每次访问都要查 RC 索引确认「这到底是整数还是指针」。
- 修完后 `matmul` 从 3.67x 降到 2.33x。

## [0.253] - 2026-11-17

### 移除异常路径上遗留的调试 getenv

- 异常路径上还留着几处 `getenv` 调试开关，正常执行不会经过，但异常频繁的循环里会被反复调用。
- 改成启动时读一次的静态标志。

## [0.252] - 2026-11-17

### 移除分配热路径上的 getenv 调用（bench_rc_gen 0.87x → 1.13x）

- **这是那一轮性能调查的最终根因**：`rc_alloc` 里每分配一次就调一次 `getenv` 判断诊断模式。
- `getenv` 要遍历环境数组并做字符串比较，65 万次分配就是 65 万次全表扫描 —— 比分配本身贵得多。改成启动时读一次的 `pa_diag_rc_on`。`bench_rc_gen` 0.87x → 1.13x。

## [0.251] - 2026-11-17

### temp_take 栈顶快速路径（扫描步数 −40%）

- `pa_rc_temp_take` 要从栈顶往下找目标值。实测 30 万次 take 共 75 万步、平均 2.5 步。
- 加栈顶直接命中判断：容器构造时「刚 push 就 take」是主流形态，一次比较就结束。扫描步数 −40%。

## [0.250] - 2026-11-17

### str/list 重复运算漏登记 fresh_vregs（inc 减半）

- 同一表达式被重复求值时，IR 里会复用同一个 vreg；但 `fresh_vregs` 没有把复用的那一份登记进去，导致 RC 引用计数被多算一遍。
- 修完后 inc 调用次数减半。

## [0.249] - 2026-11-17

### GC 检查移出分配热路径 + 定位那 10ms 的真实构成

- `rc_alloc` 每次都读 7 个 GC 相关全局变量做判断，合并成一个 `gc_need_check` 标志：为 0 时整块跳过。
- 同时用 rdtsc 逐函数计时定位到那 10ms 的真实构成 —— 不是 GC 本身，是这些判断和诊断钩子。

## [0.248] - 2026-11-17

### 修正 GC 计时口径（增量工作此前完全未统计）

- GC 的计时只在完整收集时累加，增量标记的工作量完全没被统计，于是「GC 只花 0.007ms」这个数字是假的。
- 改成增量步进也计时，得到的才是可以拿来判断优化方向的数。

## [0.247] - 2026-11-17

### RC 冗余消除遍（temp/temp_take 抵消对）

- 降级时产生的 `pa_rc_temp` / `pa_rc_temp_take` 配对里有一部分是纯冗余的（push 完立刻 take，中间没有任何可能抛异常的调用）。
- 新增一个优化遍把它们互相抵消掉，省掉一整对函数调用。

## [0.246] - 2026-11-17

### unknown RC 加小整数短路（bench_heapq 1.71x → 2.00x）

- unknown 值的 dec 要先判断「这到底是整数还是指针」。裸 int64 可能是任意下标/长度，减掉 64 字节头后落在极低地址。
- 加 64KB 下限 + 16 字节对齐双约束先廉价排除小整数，再才查堆链确认。`bench_heapq` 1.71x → 2.00x。

## [0.245] - 2026-11-17

### dict 对象池（bench_rc_literals 1.51x → 1.58x）

- 字符串和列表都已经有按档位划分的 freelist，字典没有 —— 每次 `{}` 都要走一次 malloc。
- 补上字典对象池。`bench_rc_literals` 1.51x → 1.58x。

## [0.244] - 2026-11-17

### 字符串池按档位分层（textwrap 133ms → 113ms）

- 字符串池原来所有长度共用一档，短串被切成大块浪费、长串又命中不到合适档位。
- 改成按长度分档，小档（≤40 字节）放宽到 512 块、其余 64 块。`textwrap` 133ms → 113ms。

## [0.242] - 2026-11-17

### RAII 推广到容器与字符串方法

- 按「多出口 + 需要清理」筛选，转换 6 个函数：`jp_value`（list/dict/tuple 三分支）、`pa_itertools_product`、`pa_dict_items`、`pa_dict_copy`、`pa_str_method`、`pa_bytes_method`。合计 `PA_PIN_SCOPE` 13 处、`PA_FREE_SCOPE` 2 处，手工 `pa_gc_pin` 从 71 降到 62。
- 新增 `PA_FREE_SCOPE`：同样的 RAII 思路但清理动作是 `free`，管理 calloc/malloc 的裸数组。`pa_itertools_product` 原来在末尾手工 `free(idx); free(vals);`，中途加提前返回就会漏。
- 按标准跳过：`buf_append` 的 pin 是静态分析误报（实际是 `realloc`）、`pa_asyncio_create_task` pin 后只有一个 return、其余 62 处都是单出口或极短函数。

## [0.241] - 2026-11-17

### RAII 作用域守卫（GCC cleanup 属性）

- **问题**：运行时「保命对象」要手工配对 `pa_gc_pin`/`pa_gc_unpin`。`jp_value` 有 11 个 return、4 处 pin，每条出口都要人工保证配对 —— 0.233 就漏过一处（tuple 分支），造成 96 字节泄漏。
- **做法**：C 没有析构函数，但 GCC/Clang 有 `__attribute__((cleanup(fn)))` —— 变量离开作用域时自动调用。`PA_PIN_SCOPE(var)` 自动 pin，任意出口自动 unpin；`PA_PIN_RELEASE(var)` 交出所有权时解除守卫。
- **两层保障**：RAII 管正常路径，临时栈管异常路径（longjmp 不触发 cleanup 属性，这是 C 语言限制）。
- **效果**：`jp_value` 手工 `pa_gc_pin` 4 → 0。实验验证：故意漏写 `PA_PIN_RELEASE` 时 cleanup 仍自动兜住，不泄漏。
- **代价实测**：性能零损失（`bench_json` 8ms vs 8ms）；代码 +4%（151 → 157 行）。

## [0.240] - 2026-11-17

### inc/dec 内联（抄 Swift ARC 的编译期内联思路）

- `pa_rc_inc` / `pa_rc_dec` 从跨编译单元函数调用改成头文件 `static inline`：快路径（计数 > 1、不在收集期、非统计模式）内联展开，慢路径（归零释放 / 收集期 / 统计）调 `pa_rc_*_slow`。
- 运行时内部上千处调用点通过宏 `pa_rc_inc(p) → pa_rc_inc_fast(p)` 重定向；`rt_core.c` 用 `PA_RC_NO_INLINE` 提供生成代码调用的外部符号。
- **机制验证**：`rt.o` 里 `pa_rc_inc`/`pa_rc_dec` 未定义符号引用 **239/236 → 0**。
- **性能**：`bench_rc_gen` 76ms → 75ms（约 1.3%）。幅度小，因为 RC 调用几乎全在运行时库内部（IR 里只有 5 个 inc / 12 个 dec，实际执行 30 万 / 95 万次）。
- **试过无效**：方案 D 扩大对象池（64 → 512 → 2048）。class 3/6 溢出清零，但 class 5（size 41-48）仍 98% 溢出。单独跑 `s = 'x' * 16` 时溢出是 0 且快于 CPython（17ms vs 19ms）—— 问题是**释放时机**不是池容量。已回退。
- **评估后放弃**：方案 C 静态消除冗余 inc/dec —— IR 里只有 5 个 `rc_inc` + 12 个 `rc_dec`，编译期能消除的上限是这 17 个操作，优化空间 < 0.001%。

## [0.239] - 2026-11-17

### 增量清扫（照抄 Go 并发 GC 结构）

- 先读 Go 官方 [GC guide](https://go.dev/doc/gc-guide) 与 `src/runtime/mgcmark.go`、`mwbbuf.go` 源码，再动手。照抄三点：① 增量驱动（清扫拆成有界步数，挂在 `rc_alloc` 上，即 Go 的 mark assist 思想）；② allocate-black（标记期间新分配的对象不参与本轮）；③ 写屏障 `pa_gc_write_barrier`。
- **本地化优化**（Go 没有对应物）：① **标记同步、只分片清扫** —— 试过标记也分片，但 RC + 增量标记有硬冲突：标记期间对象可能被 RC 释放、队列还指着它，即使登记 `gc_pend` 延后仍有对象在收尾后才归零（`174_gc_construct_roots` 漏 21KB）。Go 没这问题是因为它根本没有 RC。② 小集合（< 2 万）同步清扫，避免分片打断分配路径缓存局部性。③ 倒计数器，热路径只有三条指令。
- **效果**：最大停顿 5.09ms → **1.68ms（-67%）**；GC 总时间 12.99ms → 0.62ms；吞吐 73ms → 75ms。
- **过程中修的三个真 bug**：① `gc_incr_finish` 里 `old_bytes +=` 写成累加（gc_set 是本轮全部存活对象，必须覆盖）；② 同步清扫分支漏设 `gc_incr_major`，导致 `alloc_since_major` 永远停在阈值之上、**每次分配都触发 major**（`180_codepoint_perf` 触发 28 万次 GC、开销 72ms 占程序 46%）；③ `gc_pend` 处理必须在 `gc_phase` 复位**之后**，否则 `pa_rc_dec` 又登记回去形成自循环。

## [0.238] - 2026-11-17

### minor 阈值改用 Go pacer 公式

- Go 的目标堆公式：`Target heap = Live heap + (Live heap + GC roots) * GOGC / 100`，即阈值应按存活堆比例算而非固定值。原来 `young_threshold` 写死 2MB。
- 每轮收集收尾时重算：`target = live + (live + roots)`（GOGC=100），下限 2MB（对应 Go 的 4MiB 最小堆）。
- **效果**：GC 次数 34 → 7（minor 31 → 4）。总耗时不变 —— 剩下的是 3 次 major，minor 本来只有 0.4ms/次。

## [0.237] - 2026-11-17

### 按需关闭索引维护（`bench_rc_gen` 0.75x → 0.90x）

- 三步定位：① 关掉 GC 跑 73ms（GC 占 11ms）；② 关掉**索引维护**跑 80ms（原本 102ms，索引维护占 22ms）；③ 但直接关掉会让 45 个用例失败。
- **做法**：新增 `ir_module_can_skip_index_maint()`，与 `needs_unknown_index` **正交** —— 后者回答「索引要不要建」，前者回答「建了有没有人查」。运行时加 `pa_set_index_readonly()`，只关维护不动查询语义。
- **判据是试出来的**：只排除生成器/协程 → 45 个失败；+ 排除 stdlib 导入 → 4 个；+ 只放行单函数模块 → 1 个（`86_popen` 字符串被多包一层引号，`IsStr` 失效）；+ 全局变量只允许内置类型 → 0。最后一条关键：`86_popen` 的全局是 `subprocess.Popen`，`m.imports` 记不到内置模块。

## [0.236] - 2026-11-17

### 按位置放宽 exact-RC 判定（exact-RC 用例 41 → 50）

- `ir_module_needs_unknown_index` 原来只要模块里任何位置出现 unknown 就整体退化。实测这个判定过宽：走 exact-RC 能快 33%（83ms → 56ms，追平 CPython）。
- **按位置区分**：裸 unknown 在槽/返回值/参数上时必须保留索引；但 `list[unknown]` 这种「unknown 只在容器元素位置」的情况，0.235 的 `heap_bits` 已在写入点按静态类型记录了每个元素是不是堆指针，销毁时不需反查。
- **试过但不安全（已回退）**：「`maybe_unknown` 的全局变量也可以放宽」—— `unknown_is_heap` 在索引关闭时靠 `marked` 位和遍历堆链判断，对象已被 GC 释放后读的是悬空内存，ASan 实测 `heap-use-after-free`。
- **同时修的真 bug**：`pa_repr_tuple` 每轮拼接用的临时字符串 `s` 用完没 dec。保守模式下有 GC 兜底看不出来，exact-RC 下字符串是 untracked leaf 直接泄漏（`75_tuple_repr` 96 字节）。

## [0.235] - 2026-11-17

### 容器逐元素堆位图（`dec_unknown` 降 96%）

- 给 `PyAdamasList` 加 `heap_bits`：写入时按**写入点的静态类型**记录「这个元素是不是堆指针」，销毁时按位图直接 `pa_rc_dec`/跳过，不必走全局 rc_index 反查。
- 新增 `pa_list_append(l, v, heap)` 第三参数，由 codegen 按静态类型给出：0 非堆 / 1 堆 / 2 unknown。出现 2 时整个列表放弃位图退回旧路径。
- **效果**：`bench_rc_gen` 的 `dec_unknown` **467613 → 17418（-96%）**。**但总耗时没有改善** —— 这否定了「哈希索引查询是瓶颈」的假设。之前测出的「跳过判定省 21ms」来自一个段错误的构建，计时无效，已作废。
- **保留理由**：方案 B 需要这个能力（容器元素在没有全局索引时也要能正确 dec）。
- **过程中修的两个真 bug**：① `gen_make_list` 直接发射 `pa_list_append` 但只设 2 个参数，第 3 个寄存器是垃圾值 → tuple 构造后按错误类型 dec，段错误；② `gc_free_body` 只 `free(l->data)` 漏了 `free(l->heap_bits)`，GC 清扫路径不走 `list_destroy`，15 个用例泄漏。

## [0.234] - 2026-11-17

### GC 零回收退避（容器段 major 从 8211 次降到 22 次）

- `bench_rc_gen` 比 CPython 慢，但单独测三个组成段都不慢 —— 是 GC 的组合效应。开 `PYADAMAS_GC_STATS` 看到容器段 `runs=8211 minor=3 major=8208`，每次 `set=5`、`freed=0`。
- **根因**：阈值算法 `mt = old_bytes * 2` 本该有 8MB 绝对下限，但容器段 `old_bytes` 只有 520 字节，每轮 major 都零回收，`alloc_since_major` 很快又越过阈值 —— 死循环式空扫。
- **修法**：上一轮 major 零回收时阈值提到 `old_bytes * 8`，**同时保住 8MB 绝对下限**（没有下限时 `old_bytes*8` 算出来才 4KB，比一次分配还小，等于没退避）。
- **效果**：容器段 `gc_runs 8211 → 22`，`total_ms 2.484 → 0.024`。

## [0.233] - 2026-11-16

### 混合容器元素在 exact-RC 模式下永远不释放（泄漏 1 → 0）

- **根因**：`json.loads("[1, \"x\"]", (0, ""))` 的描述符 `(tis)` 推断出 `elem_kind=2`（混合），析构走 `pa_rc_dec_unknown`；而编译器判定程序「没有 unknown 值」时会调 `pa_disable_unknown_index()`，`rc_index_has` 恒返回 0 —— dec 被静默跳过，元素永久泄漏。
- **修法**：`rc_index_off` 时用三条硬约束做回退判断，绝不误判整数——① 16 字节对齐（`rc_alloc` 走 malloc）；② `marked == PYADAMAS_RC_UNTRACKED`（exact-RC 下字符串 / range / struct 是 untracked leaf，`rc_link` 置 2 且不挂链）；③ 其余遍历 `heap_head` 确认链表成员。0.221 试过的「对象头自洽性」判据会段错误（整数很容易凑出看起来合法的头），已否决。
- **tuple 异常安全**：`jp_value` 的 tuple 分支只有 `pa_gc_pin` 没有 `pa_rc_temp`（list/dict 分支都有），`jp_expect(']')` / 长度不匹配抛异常时容器泄漏；补齐并在正常返回点 take。
- **定位方法**：分配序号 + 按时间顺序消费 RELEASE 的严格时序配对，不受地址复用干扰。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；O0/O2 差分 188 用例 0 不一致；**泄漏扫描 0 处 / 188 用例**（从 0.217 的 23 处一路清零）。

## [0.232] - 2026-11-16

### json 解析器异常路径上的容器泄漏

- **根因**：`jp_value` 创建 list/dict 后只做 `pa_gc_pin`（防 GC 清扫），**没有 `pa_rc_temp`**。`pa_gc_pin` 只影响 GC，异常展开时临时栈的 RC 清理看不到它 —— 抛异常跳过正常返回路径后容器永不释放。gdb 分配回溯给出确凿证据：`pa_list_new ← jp_value ← pa_json_loads ← __pa_main (53_json.py:24)`。
- **修法**：容器创建后立刻 `pa_rc_temp(out)`，正常返回前 `pa_rc_temp_take(out)` 把所有权转给调用者。正常路径行为不变，异常路径由展开的 cleanup 释放。覆盖 list/dict 两个分支的 4 个返回点。
- `34_exception_chain` 的 45 字节同类泄漏一并消失。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；剩余泄漏 1 处（`53_json` 96 字节）。

## [0.231] - 2026-11-16

### 诊断工具改用线性扫描（修开放寻址导致的归属错乱）

- **两类错乱**：① 插入时复用「地址不同」的 DEAD 槽，新记录落在 `hash(旧地址)+k`，而 `diag_find(新地址)` 从 `hash(新地址)` 起探测 —— 找不回来，表现为「刚 alloc 完，紧接着的 temp-push 报 UNTRACKED」；② 同地址遇到 LIVE 时原地复用，把还在世的对象挤出表，退出汇总漏报。
- **修法**：活跃对象只有几十个，O(n) 线性扫描完全够，且没有开放寻址的不变量问题（只在诊断开启时走）。`diag_insert` 收紧为：同地址 + DEAD 原地复用；同地址 + LIVE **不复用**、分配新槽；地址不同 + DEAD 链尾兜底复用并计 overflow。
- 顺带修：`temp-push` / `temp-pop` 原用 `what[0] == 'p'` 判断（两者都以 p 开头），改看第二个字符；`UNTRACKED` 现在打印 RC 头（kind/rc/size），用来区分「被 MIN/KIND 过滤掉」与「根本没走 rc_alloc」。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；工具关闭时零 debug 输出。

## [0.230] - 2026-11-16

### 诊断工具补 temp 栈事件 + 两个陷阱的记录

- **临时栈事件**（`temp-push` / `temp-pop`）：对象进栈后由 `pa_rc_cleanup` 统一释放，「进了栈却没人 pop」光看 alloc/free 看不出来，现在能直接看到配对是否完整。
- **未追踪指针报警**：`temp-push` 收到不在表里的指针时明确报 `UNTRACKED`，不再静默跳过。
- **`pa_diag_snapshot(when)`**：任意时刻快照存活对象数，区分「真的没人释放」与「由 shutdown 统一释放」。实测 `53_json`：`gc_shutdown` 前存活 5 个、之后 3 个 —— 2 个由 GC 释放，剩 3 个是 untracked leaf（不在 GC 堆链上），只能靠 RC。
- **两个陷阱写进 docs/development.md**：① 地址复用会让事件归属错乱，怀疑某对象时要把**地址**拿去 grep 全部事件，看是否与其他对象生命周期重叠；② `cmake --build | grep error` 在 ninja 并行下会吃掉编译失败（`-Werror=unused-variable` 让 `rt_json.o` 一直是旧的），应该看 `tail` 或退出码。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；泄漏 1 处（`53_json`）。

## [0.229] - 2026-11-16

### 运行时诊断工具独立成模块（rt_diag.c）

- 0.227 的追踪代码塞在 `rt_exc.c` 里只管异常对象，这一版独立成 `runtime/rt_diag.c`，能力扩到所有 RC 对象。开关：`PYADAMAS_RC_TRACE=1` / `PYADAMAS_RC_TRACE_MIN=N` / `PYADAMAS_RC_TRACE_KIND=K`（str/list/dict/obj/struct）/ `PYADAMAS_EXC_TRACE=1` / `PYADAMAS_DIAG_DEPTH=N`（默认 3）。不设变量时零输出、零可测开销（一次 getenv + 一次自增）。
- 输出直接给出归因：`[rc#20] alloc str 32B rc=1 <- str_alloc_raw <- str_new <- pa_json_loads <- __pa_main (53_json.py:20)`，退出时汇总未释放对象。
- **四个设计决定**：① 外部哈希表，不改结构体布局（加字段会让对象 126 → 142 字节，ASan 数字跟着变、前后对照失效）；② 调用点归因走 `_Unwind_Backtrace`（`__builtin_return_address(n>0)` 要编译期常量且被 `-Werror=frame-address` 拦），按 `.eh_frame` 逐帧还原并用生成代码的 PC 表拿到文件名行号，链接加 `-rdynamic`；③ 表满不静默丢弃，溢出置标志并在退出时提示「结果不完整」；④ 地址复用不算新对象，同地址 DEAD 原地复用（否则误报 14 个），但绝不覆盖 LIVE 槽（否则真实泄漏消失）。
- **踩到的坑：热路径上不能加钩子**。把悬挂指针检测放在 `pa_rc_inc` / `pa_rc_dec` 入口，`88_defaultdict` 的 `f["v"] += 1.5` 输出从 `1.5` 变成 `-1.6882786079646144e+260`。二分锁定到 `pa_diag_check_use`：它只是一个普通函数调用，但在这两个会被内联的极热函数里多一次调用就改变了寄存器分配，而生成代码对 `%xmm0` 的存活假设太脆弱。钩子只能放在非热路径（`rc_alloc` 入口、真正释放的那一支）。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；工具关闭时零 debug 输出；泄漏 1 处（已精确定位到 `53_json.py:20`）。

## [0.228] - 2026-11-16

### 生成器帧槽里的异常对象没人释放（泄漏 2 → 1）

- **诊断先定位再修**：`PYADAMAS_EXC_TRACE` 打出 `119_gen_except_suspend` 的轨迹 `alloc → leave(写进帧槽 s1) → leave(同槽再写) → LEAK`；帧槽快照显示生成器已正常终止（status=1）但 `s1`（exc_cur_slot）里仍存着异常对象。
- **两处机制都漏了它**：① `gen_finish` 的顺序是 `release_slots()` 然后 `exc_leave()`，写回槽的那一刻释放已跑完，而 codegen 的帧销毁开头 `cmpq $1, 8(%rbx); je .Lgengcreldone` 对 status==1 短路跳过整块槽释放（对 RC 槽是对的，对异常槽不是）；② `exc_cur_slot` / `exc_top_slot` 类型被推断成 int（存裸指针），`lower_gen_async.cpp` 的 `if (!ir_is_heap(ty)) continue;` 跳过它们，`slot_heap` 为 0，释放循环也不生成代码。
- **修法**：`exc_cur_slot` 标 `slot_heap=3`、`exc_top_slot` 标 `4`；codegen 把 `slot_heap >= 3` 的释放移到 status 检查**之前**无条件执行；`pa_gen_slot_release` 对 own==3 走 `exc_free`、own==4 走 `pa_exc_free_save_chain`（都不能 `pa_rc_dec`，那会按 RC 头读到 malloc 块前面）；`pa_rc_shutdown` 退出前把临时栈残留值 dec 干净。
- **为什么前几轮失败**：0.226 之后在这个 126 字节上盲试三次（slot_heap 标记、生成器 owner 登记表、调整 gen_finish 顺序），每次都引入新的 heap-buffer-overflow / use-after-free 并回退。差别在于当时不知道对象在哪一步丢掉，只能猜所有权。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；泄漏扫描 **1 处 / 188 用例**（`119_gen_except_suspend` 126 字节 → 0）。

## [0.227] - 2026-11-16

### 异常对象生命周期追踪工具（PYADAMAS_EXC_TRACE）

- **动机**：0.226 后剩两处泄漏，盲试多轮（改 slot_heap 所有权标记、加生成器 owner 登记表、调整 gen_finish 顺序）每次都引入新内存错误并回退。结论：异常对象的所有权在 `pending_exc` / `current_exc` / 生成器帧槽 / 临时栈之间反复转移，光靠读代码和 ASan 的字节数推不出来。
- **工具**：`PYADAMAS_EXC_TRACE=1` 把每个 `PyAdamasException` 的 `alloc` / `free` / `leave`（写回帧槽 = 所有权转移）/ `enter`（取回）逐条打到 stderr，退出汇总 `LEAK`。追踪表是外部表（指针 → 分配序号），不在结构体里加字段；不设变量时只有一次 getenv 和一次自增，实测全量测试与泄漏字节数与加入前完全一致。
- **直接给出的结论**：`119_gen_except_suspend` 的两个泄漏对象轨迹是 `alloc → leave → leave → 再无任何事件`，即写进帧槽后再没被取回也没被释放 —— 问题确定在「生成器销毁时帧槽里的异常对象没人还」，排除了 RC 配平方向。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；泄漏 2 处（与加入工具前一致）；docs/development.md 新增追踪一节。

## [0.226] - 2026-11-16

### 零开销 raise 路径的收尾责任（泄漏 3 → 2）

- **根因**：0.225 让 `terminated()` 认出不返回的运行时调用后，`lower_try` 不再在 except 体后补 `ExcDone`（补了也是死代码）。这本身是对的，但**收尾责任落到那些 `_Noreturn` 函数身上**，而它们没做 `pa_exc_drop_save()`（还 except 入口压的 `PyAdamasExcSave`）和 `exc_free(current_exc)`（`pa_exc_accept_value` 接管成 `current_exc`，原由 `ExcDone` 负责 free）。运行时计数 `accept=7 done=5` 差 2 个。
- **修法**：三个零开销入口都补收尾——`pa_eh_raise`、`pa_eh_raise_value_from`、`pa_eh_reraise`。（setjmp 路径的 `pa_reraise` 在 0.225 已修。）
- `34_exception_chain` 45 B → 0、`39_match_advanced` 47 B → 0。
- **回退的方案**：`pa_raise_value_from` 里保存/恢复 `pending_exc_args` 对 119 无效（126 字节不变），且 34/39 的修复不依赖它，保持最小改动。
验证：全量 205 ran ok；差分 188 用例 0 不一致；泄漏扫描 188 用例 **2 处**（`119_gen_except_suspend` 126 B、`53_json` 96 B，均需改生成器销毁路径 / GC 交互，量级更大）。

## [0.225] - 2026-11-16

### terminated() 漏认不返回的运行时调用（死代码吞掉清理）

- **根因**：`Block::terminated()` 只认控制流指令，而 `pa_reraise` / `pa_raise*` / `pa_eh_*` 都是 `_Noreturn`。于是 `lower_try` 认为「块还没终止」，在 `pa_reraise()` **后面**又追加 `pa_exc_done()` + `jump` —— 永远执行不到却被当活代码保留，其中的清理动作静默丢失。运行时计数 `accept=6 done=5 reraise=0`，尽管 IR 里明明有 5 处 reraise。
- **修法**：① `terminated()`（ir_types.h）补上 `ExcReraise` / `Raise` / `RaiseValue` / `RaiseValueFrom` / `RaiseInstance` / `Reraise` / `EhRaise` / `EhRaiseValueFrom` / `EhReraise` / `EhRethrow`；② 新增 `pa_exc_drop_save()`，供 setjmp 的 `pa_reraise` 与零开销的 `pa_eh_rethrow` 在跳转前还掉 save 节点。
- **踩过的坑**：`pa_exc_drop_save` 最初实现成「还 save 节点 + `exc_free(current_exc)` + 恢复外层值」，引入 2 个回归（`No active exception to reraise`、抛错对象）。裸 `raise` 重抛的**正是** `current_exc`，不能在它之前 free；改成只还 save 节点。
- `32_exception_value` 16 B → 0。剩余 3 处经 `git stash` 验证在 HEAD 上同样存在，非本次引入。
验证：普通全量 205 ran ok；差分 188 用例 0 不一致。

## [0.224] - 2026-11-16

### os.path.split / raise 收尾 / 异常上下文链（泄漏 5 → 2）

- **① `pa_os_path_split` 多 inc 一次**（rt_os.c）：与 0.220 pathlib 同类，`str_new` 返回的串本来就带 rc=1（产生者持有），`pa_list_append` 只存指针不加引用，多 inc 的那份没人释放（`81_os_extra` 泄漏 120 字节，head/tail 各一条）。
- **② `pa_exc_reraise` 不清理 save 节点**（rt_exc.c）：裸 `raise` 直接 `exc_longjmp`，跳过 except 块的 `pa_exc_done`，而 `pa_exc_accept_value` 进 except 体时压的 `PyAdamasExcSave` 必须在这里还掉，否则每执行一次 `except ...: raise` 泄漏 16 字节。
- **③ 异常上下文链被覆盖丢失**：`pa_exc_enter` 把 `exc_save_top` 摘下存进 `n->top`，恢复时直接覆盖，当前那条非空链永久丢失；加 `free_save_chain` 在覆盖前还掉。
- **走错又退回的一步**：给 `lower_raise` 加 `emit_cleanup_exits` 引入 11 个用例回归——有值 raise 的清理把要抛的异常值释放了（`refcount increment on a freed object`），裸 raise 的 ExcDone 把 `current_exc` free 了（`No active exception to reraise`）。结论：**raise 的收尾不能靠通用清理机制**，要抛的对象必须在清理之后仍然有效，正确做法是运行时在跳转前自己收拾。
验证：普通全量 205 ran ok；ASan 全量 204 ok / 1 偶发（`180_codepoint_perf` 性能阈值在 ASan 下的时间抖动，连跑 5 次全过）；差分 188 用例 0 不一致；`81_os_extra` 216 B → 0、`119_gen_except_suspend` 207 B → 0。

## [0.223] - 2026-11-16

### 异常子系统 shutdown 审计（挂起 handler 记录丢失）

- **审计方法**：盘点运行时所有 `static` 堆指针逐项检查清理点。`young_buf` / `gc_work_buf` / `gc_set_buf` / `gc_tab_buf` / `gc_pend` / `list_pool` 已有；`exc_regs` / `handler_freelist` / `suspended_head/pool` / `exc_node_top` 四块**无清理点**。
- **修法 1**：新增 `pa_exc_shutdown`，统一还掉挂起表 → 空闲节点池 → handler freelist → 异常继承登记表 → 上下文嵌套节点，在 `pa_gc_shutdown` 末尾调用。
- **修法 2（关键）**：`pa_handler_suspend` 原来只在 `gen_owner` 非空时登记，但很多 suspend 发生在 owner 为 NULL 的上下文（生成器被 `pa_gen_next_opt` 驱动时 owner 已在上一轮 `gen_run` 结束时清掉）。那些记录摘链后既不在 `handler_top` 也不在挂起表 —— 永久丢失（快照 `handler_top=0 suspended=0 freelist=3` 就是证据）。改成**无条件登记**，owner 为空时挂到「孤儿」owner 上由 shutdown 兜底，生成器销毁按自己的 owner 找，不会误删孤儿。
- `162_pep479` 240 B → 0，总泄漏 5 处 → 4 处。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致。

## [0.222] - 2026-11-16

### 生成器 handler 清理统一到 unwind_handlers_to

- 0.219 只在 `gen_run` 的异常路径加了清理循环，但生成器有**三个**退出点会留下 resume 内部压过的记录：`gen_run` 的 `PA_SETJMP != 0` 分支、`pa_gen_close` 的异常分支（finally 里再抛）、`pa_gen_close` / `pa_gen_finalize` 的正常路径。后两个只做 `handler_top = h.prev`，沿途记录既不在全局链上也没人归还。
- 抽成 `unwind_handlers_to(stop)` 三处共用；`pa_handler_discard_record` 内部会摘挂起表 + 检查 `heap_alloc` 再归还 freelist，栈记录与已复用记录都安全。
- **诚实说明**：这个重构让三条路径一致（此前只有一条对），但**没有消除剩下的生成器泄漏**——`162_pep479` / `119_gen_except_suspend` / `32_exception_value` 字节数不变。它们的泄漏栈都指向 `gen_run → pa_gen_next_opt`，说明生成器还在迭代中（`list(g_inner())` 消费到一半），记录挂在挂起表里、对象还没销毁程序就退出了，需要在 shutdown 里清空挂起表（0.223 做）。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致。

## [0.221] - 2026-11-16

### json 类型描述符漏认 tuple（混合类型容器元素泄漏）

- **根因**：`desc_heap` 实现是 `return *d == 's' || *d == '(';`，而描述符首字符编码里 **`t`（tuple）漏了** —— tuple 也是堆对象。于是 `(tis)` 的 `all_heap` 变 0、`any_heap` 变 1，`pa_list_new` 拿到 `elem_kind=2`，析构走依赖 `rc_index` 的 `pa_rc_dec_unknown`；这类程序被判定「无 unknown 值」→ `pa_disable_unknown_index` → dec 静默跳过 → 元素永久泄漏。补 `'t'` 后 `elem_kind=1`，析构走 `pa_rc_dec` 正常释放。
- **为什么不去改 `pa_rc_dec_unknown`**：让它在 `rc_index_off` 时靠对象头自洽性（kind/rc 范围 + marked 保留位）判断指针，试过**会段错误**（`53_json` / `89_bytes_final` 整个程序无输出）—— 整数很容易凑出看起来合法的对象头。要正确做需让混合容器在 exact-RC 下也维护元素类型信息（位图或标签），是容器布局级改动。补 `desc_heap` 是在**产生 elem_kind=2 的源头**上修。（0.233 最终用三条硬约束解决了这个分支。）
- `79_datetime_pathlib` 120 B → 0，`141_stdlib_pathlib_extra` 同类问题一并消失，总泄漏 8 处 → 5 处。
验证：全量 205 ran ok；差分 188 用例 0 不一致。

## [0.220] - 2026-11-16

### pathlib 返回值引用泄漏（str_alloc_raw 的产生者引用没人放）

- **根因**：`pa_path_new` 的语义是「接管一个**已有**的字符串引用，内部 `pa_rc_inc` 补一份」，但 6 处调用传的是 `str_alloc_raw` / `str_seal` 的**刚产生**结果 —— 那个串本来就带 rc=1，再 inc 变成 2，产生者那份没人释放（`147_pathlib_extra` / `141_stdlib_pathlib_extra` 各泄漏 128 字节）。
- **修法**：新增 `pa_path_adopt()` 接管刚产生的串、不额外 inc；6 处 fresh 串改用它，2 处传已有串的保持 `pa_path_new`。pin/unpin 两边一致，防止 `rc_alloc` 触发主收集时参数串被清扫。
验证：两处 pathlib 泄漏 → 0；全量 205 ran ok。

## [0.219] - 2026-11-16

### 生成器 handler 记录的生命周期（泄漏 67200B → 0）

- **根因**：handler 记录的完整生命周期是 `push → suspend（yield 时摘链）→ rearm（resume 时压回）→ leave/discard`，但生成器可能**再也不会 resume**（被 close 后丢弃、finally 里抛异常跳出），suspend 过的记录既不在全局链上也没人归还。更糟的是 `pa_handler_free` 是裸 `free(p)`，不进 freelist 也不摘登记表，留下悬空指针。
- **四件事**：① `PyAdamasHandler` 加 `suspend_owner`，suspend 时记下所属生成器，销毁时按 owner 找回（记录在挂起期间保持有效，因为 rearm 用的就是同一地址）；② 全局挂起表，suspend 登记、rearm/leave/discard/free 都 `unsuspend()`；③ `pa_handler_free` 改为「摘挂起表 + 归还 freelist」，这是 123/119 的 use-after-free 根源；④ `gen_run` 异常路径先把 `h.prev` 到 `handler_top` 之间的记录逐个归还。
- **容易踩的坑：栈记录 vs 堆记录**。`gen_run` / `pa_gen_close` / `pa_gen_finalize` / `rt_async` / `rt_agen` 里的 `PyAdamasHandler h` 是栈变量，进 freelist 会造成 stack-use-after-return。加 `heap_alloc` 标记（push 置 1、栈记录置 0，归还前检查），**且必须在所有栈记录处显式初始化** —— 漏一个就读到垃圾值，普通构建直接段错误，而 ASan 构建因内存清零反而看不出来（这个差异一度让 5 个用例只在普通模式失败）。
- `93_gen_exc_rc` 67200 B → 0（300 个生成器 × 224 B）、`123_finally_yield` 224 → 0 并修掉一个 UAF、`162_pep479` 448 → 240，总泄漏 10 处 → 8 处。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致。

## [0.218] - 2026-11-16

### ASan 覆盖运行时内部 + 修 json.loads 浮点返回值错位

- **运行时插桩**：之前 `--asan` 只给生成代码插桩，`rt.o` 是预编译零插桩，RC/GC/列表/字典这些**运行时内部**的指针错误完全看不到（0.216 能抓到纯属运气）。新增 CMake 目标 `rt_asan.o`：同样源文件用 `-O1 -g -fsanitize=address` 重编（79 个 ASan 符号），`--asan` 时链接它。代价：纯分配负载 16ms → 27ms（1.68×），测试套件不受影响。
- **立刻抓到一个真 bug**：`PYADAMAS_TEST_ASAN=1` 全量扫描发现 `53_json.py` 输出 `-1.6882786079646144e+260` 而不是 `2.5`。根因在 `gen_call_rt` 的 `raw_ret` 只列了 `DictKeyAt`，而 `pa_json_loads` 的契约是**返回 i64 位模式**（浮点值也塞在 `%rax`），没在列表里于是 codegen 按 double 去读 `%xmm0` 的残留值。普通构建下被寄存器残留掩盖（恰好凑出正确结果），ASan 改变内存布局后稳定暴露。
- **修法**：`raw_ret` 加上 `JsonLoads`；float 类型时先 `movq %rax, %xmm0` 搬位模式（`movsd` 不能直接从 GPR 取源）再按浮点落位。
验证：普通全量 205 ran ok；**ASan 全量 205 ran ok**；差分 188 用例 0 不一致；7 个 GC 复现（gc01/gc02/x01/z01/g04/g05/d04）全部 ok。

## [0.217] - 2026-11-16

### ASan 成为一等公民（CLI + 测试框架 + 修掉 23 处假泄漏）

- **CLI**：`--asan` / `--ubsan` 让生成可执行文件带检查器，编译和链接同时生效，底层仍是 `PYADAMAS_CC_FLAGS`。`ASAN_OPTIONS=detect_leaks=0` 是有意的默认——GC 程序里「故意留到进程退出」的对象很多，泄漏报告会淹没真正的 use-after-free。
- **测试框架**：`PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=4 bash tests/run_tests.sh`，188 个用例全跑 ASan，慢 3-5 倍，改动触及 RC/GC/lowering 时用。首次全量扫描 **0 处内存错误**。
- **修掉 23 处假泄漏**：开 `detect_leaks=1` 扫 108 个用例报 23 处 Direct leak（各 120-464 字节），全是进程退出时还挂在分配器缓存里的块。两个来源：`str_pool` / `list_pool` / `list_buf_pool` 回收时不立刻 free；`sys.argv` 挂在全局根上永不 dec，而 exact-RC 下字符串**不进 GC 堆链**（`rc_link` 对叶子直接 return），`pa_gc_shutdown` 的 `while (heap_head)` 扫不到。修法：shutdown 清空三个池 + 新增 `pa_sys_argv_shutdown` 在释放堆之前交还 argv 引用。**23 处 → 5 处**。
- **文档重写**：docs/development.md 的「内存 / UB 检查」整节删掉手工编 rt.o / cc 包装脚本 / 改 PATH 的老流程，改成三条命令，并把 0.216 的定位过程写进去当例子。
验证：全量 205 ran ok；差分 188 用例 0 不一致。

## [0.216] - 2026-11-16

### unknown 槽被提升导致 GC 根缺失（use-after-free 完整修复）

- **问题**：`def f(): u = ident([...]); return u[0]` 在 major GC 后堆损坏 / SEGV，`PYADAMAS_GC_DISABLE=1` 完全正常，`-O0` 也崩。
- **根因（ASan 抓到第一现场）**：ASan 报 `SEGV on address 0xffffffffffffffe6`（= -26）在 `pa_rc_temp`。`u` 是 unknown 槽，被寄存器分配提升到 `%r12`，而帧记录登记的是栈槽 `48(%rsp)` —— 栈槽自序言清零后再没被写过，GC 读到 0 不标记，`u` 被回收，之后 `index.get` 从已释放列表取元素得到 -26。`PYADAMAS_GCSLOT_DEBUG` 实证：`s1 ty=unknown scan=1 reg=%r12`（该留内存却拿了寄存器）且与 `s3 ty=int` 撞同一个寄存器。
- **三处修复**：① `build_slot_liveness` 候选表排除 unknown 槽（原来只排除堆槽 / 取过地址 / handler hazard）—— unknown 可能是堆指针，GC 要靠栈槽地址找根；② `gc_scan_slot_` 改在寄存器分配**之前**按静态类型算好，消除「先分配再登记」的循环依赖（槽一旦被提升就既不在栈槽也不在扫描清单里）；③ `can_fuse` 禁止融合被 GC 扫描的槽（融合会让栈槽从此不再更新）。
- **顺带**：`driver_build.cpp` 的 `cc` 调用支持 `PYADAMAS_CC_FLAGS`，编译和链接共用。这个 bug 靠它才定位到 —— IR 层面完全看不出问题。
验证：全量 205 ran ok；差分 188 用例 0 不一致；`/home/lenovo/wb_repro/` 的 7 个 GC 复现全部通过。

## [0.215] - 2026-11-16

### unknown 返回值的引用所有权缺失（use-after-free 根因）

- **问题**：`def f(): return a[0]` 里 `a[0]` 静态类型是 unknown 时返回**不加引用**，调用方拿到借来的指针；被调函数返回后那份引用被临时栈清理放掉，对象被回收。`PYADAMAS_GC_DISABLE=1` 下完全正常（对象不在池里被复用，问题被掩盖）。
- **两处同一类不对称**：① `emit_return`（lower_impl_slots.cpp）的 `ir_is_heap(unknown)` 是 false，unknown 返回值什么都不做 → 改走 `RcIncUnknown`（先查堆索引确认是堆指针再 inc，整数安全）；② `retain_for_store`（lower_impl_util.cpp）在 dst 与 src **都是 unknown** 时什么都不做 → 补 `rc_inc_unknown`。`u = ident([...])` 正是「无标注函数返回未知类型、存进未知槽」。
- **效果**：6 个 GC 复现里 4 个修复（gc01_min_uaf、z01_global_root、g04/g05_unknown_root_hold），2 个从「堆元数据损坏 / 静默算错」变成可检测的 panic（gc02_uaf_junk、x01_only_u_root）。
- **诚实说明剩余缺口**：`release_slots` 只释放 `ir_is_heap(ty)` 的槽、unknown 槽整个跳过（`release_globals` 是对称的）。直接补齐会让 12 个用例 over-release —— unknown 槽里的值未必被 retain 过（装饰器回填、borrow_slots、帧槽别名都会让它持有借来的值）。正确做法是槽级「是否 owned」标记，不在这一版做。
- **顺带修测试框架撑爆磁盘**：`tests/run_tests.sh` 输出无大小限制（某次 `78_enumerate_zip.py` 死循环写了 **19G** 日志）+ `testlogs.$$` 从不清理，`build/dev` 涨到 **32G**。改为 `head -c 1048576` 截断（截断后 diff 必然不等，照样报 OUTPUT DIFF）+ `trap cleanup_logs EXIT INT TERM`。
验证：全量 205 ran ok。

## [0.214] - 2026-11-16

### join(T, None) 返回 Optional[T] 而不是 unknown（4.7×）

- **根因**：`join_into` 在 Optional 分支之后、兜底之前有一条 `if (a->kind == None || b->kind == None) return t_unknown();`，于是三元表达式、if-else 合并、列表字面量里**只要掺了 None 就全退化成 unknown**（`v = 1 if flag else None` 应为 `int | None`，`[1, None]` 应为 `list[int | None]`）。
- **为什么是缺口不是权衡**：`Optional[T]` 本身完整支持——标注路径生成的 IR 正确、`is not None` 收窄也正确，只是推断的 join 没走它。改为 `mk_optional(b)` / `mk_optional(a)`。
- **实测**：300 万次热循环（`v = i if i % 2 == 0 else None` + `is not None` 收窄）34-36ms → **6-7ms，约 4.7×**。asm 层面 `pa_rc_inc_unknown` / `pa_rc_dec_unknown` 从 16 处降到 0 —— unknown 走保守 RC 路径，每个操作都要额外调用查注册表。
- **来源与后续**：team 的 type-auditor 审查 351 处 `t_unknown()` 时定位（B2）。同批的 B1（调用点缓存重查前的返回类型，3.7×）两种修法都失败回退：重跑模块体会让类定义处理两次生成两个 Type 对象（`cannot assign Point to Point`，47 个用例失败）；就地更新符号类型会破坏 12 个合法重绑定用例。B1 需要完整的「赋值点 → 类型版本」跟踪，是类型系统级改动；B3（hoist 的 unknown 占位污染分支合并）同理。
验证：全量 205 ran ok；差分 188 用例 0 不一致。

## [0.213] - 2026-11-16

### 切片赋值缺少元素类型检查（导致垃圾指针读出）

- **Bug**：`l: list[int] = [1, 2]; l[0:1] = ['hello']` 编译期不报错，`print(l[0])` 输出 `96042627193968` 这类垃圾且每次运行不同（CPython 输出 `hello`）。字符串指针被写进声明为 `list[int]` 的列表，后续按 int 读出就是未初始化内存。
- **根因**：`typecheck_stmt.cpp` 的切片赋值分支只检查「对象是否可变」，注释写着「切片赋值在运行时才展开，类型层面只确认可变」——这个假设是错的；而紧随其后的单元素赋值分支有完整的 `merge_elem` 检查，切片赋值漏了。
- **修法**：`list[T]` 的切片赋值在右值也是 `list[U]` 时比较 T 与 U，复用单元素赋值的 `merge_elem`，报 `cannot assign 'list[str]' to slice of 'list[int]'`。
- **来源**：team 的 runtime-auditor 扫运行时内存管理时发现，独立复现后定位到类型检查器。
验证：错误用例编译期报错；正确用例 `l[0:1] = [9, 8]` 输出 `[9, 8, 2, 3]` 与 CPython 一致；全量 205 ran ok；差分 188 用例 0 不一致。

## [0.212] - 2026-11-16

### 清理 R3 死代码 + 修正误导性的寄存器分配调试输出

- **死代码（净删 35 行）**：`spill_int_caller_slots()` / `reload_int_caller_slots()` 依赖的 `int_caller_slots_` **只被 clear、从未 push_back**，两个函数永远空操作却挂在每个调用点的 spill/reload 路径上；`build_int_caller_mask()` 只有声明没有实现。一并删掉函数、调用点、成员变量、死声明。
- **误导性调试输出（这才是真正浪费时间的）**：`assign_slot_regs_linear()` 末尾统计 `slot_reg_` 里全部非空项，但**浮点槽是在调用方随后才分配的**，此刻浮点项还是空的。于是长期输出 `advance_1: 7/14 slots promoted`，看起来「只提升了一半、还有大量空间」，实际是整型 7/8、浮点 6/6，**已接近饱和**。这个错误数字直接导致「寄存器分配还有很大优化空间」的误判，进而在 vreg 分配上连续多轮投入失败。修正后输出 `[ra] advance_1: int 7/8 promoted, 6 regs`。
验证：全量 205 ran ok；差分 188 用例 0 不一致；删除的是死代码，行为完全不变。

## [0.211] - 2026-11-16

### 两处真实 bug：堆槽 LICM 外提 + RcMark 寄存器化

在尝试重写 RC 临时栈 / 扩展 vreg 寄存器分配的过程中暴露出两个**既有**正确性 bug，平时不发作是因为依赖的栈槽恰好没被覆盖，分配策略一变就变成段错误或静默算错。

- **Bug 1：LICM 把堆槽的 load.slot 提到循环外**。`hoist_loop` 的 `hoistable` 对 `LoadSlot` 只检查「槽本身没被 StoreSlot 写过」，但堆槽内容可以被循环里的 `IndexStore` 间接改写。实测 nbody 的 `energy()` 把 `load.slot s0`（列表 b）提到 `licm.pre`，栈槽被别的 vreg 复用后读到垃圾指针，段错误在 `b[i*7]`。修法：`LoadSlot` 对堆槽一律不外提。
- **Bug 2：RcMark 的返回值被放进寄存器**。`pa_rc_mark()` 返回 RC 临时栈水位，入口 mark、出口 cleanup，中间隔着任意多次调用；它是普通 int vreg，会被按活跃区间复用栈槽或分到 callee 寄存器，一旦离开栈槽某次 call 就冲掉它，`pa_rc_cleanup(mark)` 拿到错误水位并 panic `invalid temporary stack mark`。修法：`vreg_is_rc_mark()` 识别这类 vreg，在 `assign_vreg_slots` 里强制独占栈槽（区间拉满整个函数）。
- **本轮寄存器分配改动全部回退（有数据）**：块内 vreg 进 callee 池让 nbody 差 306、binary_trees 输出 25（应 8191）。根因是 `vreg_off()` 仍指向那个栈槽，任何读栈槽的路径（spill/reload、RC 调用）都拿到过期值 —— 要安全地做必须先重写 RC 临时栈与 vreg 栈槽的耦合，是另一个量级的改动。
验证：全量 205 ran ok；差分 188 用例 0 不一致；z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.210] - 2026-11-16

### 参数 >6 且含浮点的间接调用丢实参（+ 差分测试工具）

- **Bug**：10 个 float 形参的函数经间接调用时第 9、10 个实参丢失。`fargs(1.1..10.1)` 在 -O0/-O1 输出 8.8（只加到第 8 个），-O2 输出 59.6（内联掩盖了 bug），CPython 59.6。
- **两层根因**：① `closure_needs_shim` 对 >6 参数直接 return false，注释写「暂不 shim（退回原行为）」，但原行为并不成立——统一约定是第 i 个实参进第 i 个**整型**寄存器（只有 6 个），真实 SysV ABI 是整型/浮点各自计数、浮点有 8 个寄存器，两者在「>6 参数且含浮点」时不一致，没有 shim 就没人做重排；② shim 读溢出区少算 8 字节——`call` 压入返回地址后调用者的溢出区整体后移 8，漏掉这个偏移就把返回地址当第 1 个溢出实参，最后一个实参永远读不到（正好是 59.6-49.5=10.1）。
- **修法**：去掉 `params.size() > 6` 限制（所有非生成器函数都生成 shim，统一约定才自洽）；`emit_closure_shim` 支持栈上实参（临时区扩成 [0,8n) 存全部实参 + [8n,8n+8*sp) 放真实 ABI 栈传参区，重排后调整 rsp 让栈传参区紧贴返回地址）；溢出区读取补 `+8`。
- **顺带：`tools/difftest.sh`** —— 同一份源码在 -O0 与 -O2 下输出必须一致。这个 bug 就是它抓到的（188 个用例里唯一一个不一致）。优化遍引入静默错误时常规测试未必发现，O0/O2 差分能。
验证：差分 **188 用例 0 不一致**（修复前 1 个）；全量 205 ran ok；五套夹具 z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.209] - 2026-11-16

### IR 重构决策记录（稳定 ID / tag 操作数 / 暂不上 SSA）

纯文档，无代码改动。把 0.205–0.208 四步重构的**理由**写进 docs/ir.md，特别是「为什么不上 SSA」这个容易被反复提起的问题。

- 新增「那为什么不干脆上 SSA」章节：SSA 的三个主要好处（优化遍不用重建数据流、值单一定义、跨块引用不失效）分别由 `uses_of`/`def_of` 索引（0.205）、`Operand` 带 tag（0.206）、`Inst::id`/`Block::id`（0.205/0.208）拿到大部分，不用动前端降级和 phi 构造。真正的分界线是「要不要做需要精确数据流的优化」——现有的遍（折叠、DCE、CSE、LICM、内联、边界版本化）都是局部或结构性的，索引表够用。
- 补充稳定标识章节（含 0.203 边界版本化栽在块号过期上的教训）、索引操作数带 tag 章节（`PYADAMAS_IR_CHECK=1` 校验、隐式转换是刻意的迁移期手段），数据结构表补 `id` 字段。
- 同期另有一次 docs 提交修正 codegen.md 的寄存器分配与 SSA 过时描述，对齐 0.203–0.209。
验证：前四步已各自提交并验证（全量 205 ran ok，五套夹具全过）。

## [0.208] - 2026-11-16

### 块稳定 ID，EhRegion 不再存块号（重构第 4 步）

- **为什么必须做**：`IRFunction::EhRegion` 原存 `int begin_block / end_block / lp_block`（vector 下标），而字段自己的注释就写着「内联等 pass 会重建块表，这里的块号会过期，仅作记录/诊断用」——「明知会过期还存下来」是技术债。0.203 的边界版本化被同一类问题打垮：`version_bounds_loop` 每次插入 3+ 个新块，块号整体偏移，`h=9` 那层循环（含全部列表读写）被永久跳过，99 处边界检查一个没消。
- **改动**：`Block::id`（uint32_t，0 = 未分配，分配器 `next_block_id` 只增不减）；`block_by_id`（id → 当前下标），`block_index(id)` 查不到返回 -1 表示块已删；`EhRegion` 三字段改存块 id，赋值处用 `ensure_block_id()`；`rebuild_use_def()` 顺带重建 `block_by_id`；`new_block()` 从聚合初始化改显式赋值（加字段后聚合初始化会错位）。
验证：全量 205 ran ok；z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.207] - 2026-11-16

### DCE 改用 use-def 索引（重构第 3 步）

- `fold_all()` 的死代码消除原来每轮手工扫全函数统计每个 vreg 的使用次数，现在直接用 `IRFunction::uses_of`（`uses_of[v].size()` 就是 v 被读的次数），语义等价。
- **第 3 步的意义**：前两步（稳定 ID、tag 操作数）是铺路本身不产生收益，这一步才开始消费那套基础设施。后续 `opt_forward` 的 use_count、`opt_inline` 的 uses 计数都能同样接过来 —— 它们现在各自扫一遍，同一个函数在一条 pass 链上被扫十几次。
验证：全量 205 ran ok；编译耗时 matmul 4ms / nbody 6ms / spectral_norm 4ms / nqueens 3ms；**生成代码 md5 与改造前一致**（索引只换统计方式，不应改变任何优化决策）。

## [0.206] - 2026-11-16

### IR 索引操作数带 tag（重构第 2 步）

- `Inst::idx` 从裸 `int32_t` 换成带 tag 的 `Operand`（Kind: None/Slot/Global/Str/Func/Class/Extern/Block/Field）。这是那套 IR 最容易踩的坑：idx 的含义随 op 变（槽号/全局号/字符串号/函数号/类号/外部符号号/字段下标），填错编译器不管，只能靠测试和 dump 发现。
- **隐式转换是刻意的过渡手段**：195 处使用点一次性改完风险太高，保留 `operator int32_t()` 与 `operator=(int32_t)` 让存量代码继续编译，同时 kind 能被记录和校验；等所有消费点改成显式取 kind 后删掉隐式转换，即可让「漏写 kind」变成编译错误。
- **校验**：`idx_kind_ok(op, kind)` 在 `Inst::text()` 里做一致性检查，由 `PYADAMAS_IR_CHECK=1` 开启，迁移期 `kind == None` 放行。本次迁移 19 处降级侧赋值点改显式构造，覆盖 CallDirect / StoreExtern / LoadGlobal / GlobalAddr / SlotAddr / LoadSlot / StoreSlot / CallExtern / GenSetSlot；全量扫描 **0 处不匹配**。
验证：全量 205 ran ok；z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.205] - 2026-11-16

### IR 稳定指令 ID + use-def 索引（重构第 1 步）

- 给 IR 加「稳定标识 + 反向索引」基础设施。优化遍要在指令之间建立引用（可用表达式、死代码判定、循环版本化候选表），而**块内下标和块号都会被 `vector::insert/erase` 与新增块改变** —— 0.203 的边界版本化就是栽在这上面。
- **新增**：`Inst::id`（uint32_t，0 = 未分配，函数内唯一、不随重排而变，分配器 `next_inst_id` 只增不减、删指令不回收，因此任何时刻拿到的 id 都能查到「是否已删」）；`IRFunction` 上三张表 `inst_by_id`（id → (block, index)）、`def_of`（vreg → 定义指令 id）、`uses_of`（vreg → 使用指令 id 列表）；`rebuild_use_def()` 一次 O(指令数) 扫描重建，在结构改动的 pass 之后调用。
- **刻意取舍**：不做 SSA 判定（vreg 本来就可多次定义，`def_of` 只留最后一次定义，索引服务「快速找到使用点」不承担正确性判定）；`inst_by_id` 的位置值会过期，是快速定位而非权威来源，真正的稳定引用是 id 本身；id 只在 emit/def 分配，pass 新建指令走 `ensure_inst_id()`。
验证：全量 205 ran ok（ID 分配不影响语义）；编译耗时 matmul 5ms / nbody 8ms，无可见开销。纯增量，不改变任何生成的代码。

## [0.204] - 2026-11-16

### R4 浮点线性扫描 + matmul 基准改函数形式

- **R4**：抽出通用 `linear_scan_pool()`（整型/浮点共用），浮点不再「按频次挑前 8 个整函数常驻」；`slot_reg_cand_` 同时收两类候选，`slot_cand_is_fp_` 标记，GPR / xmm 两套池分别扫描。修掉一个自引入的严重 bug：区间计算用全局候选下标而传入的是子集，导致 nbody 输出错；改为按 `cands` 建「全局→局部」映射。
- **matmul 基准**：热循环从模块顶层（循环变量是全局变量，分配器管不到）包进 `run()` 变成局部槽，可常驻寄存器。
验证：全量 205 ran ok；四套异常夹具无回退。

## [0.203] - 2026-11-16

### 寄存器分配 R1–R3（活跃性 + 线性扫描 + caller 池）

基于活跃区间给槽位做分配，替代按访问频次挑前 N 个常驻。**正确性收益明确，性能收益为零**：`__pa_main` 有 106 个 call / 42 个块，活跃区间几乎都跨调用点，caller 池条件命中不了，matmul / nbody / spectral_norm / fib / nqueens 的栈访问数一个没变。根因是模块顶层没有函数边界——要提速得先解决这个，而不是继续投入寄存器分配。
- R1 `build_slot_liveness`：块级 live-in/out 位图（`PYADAMAS_RA_DEBUG=1` 可 dump）；R2 `assign_slot_regs_linear`：线性扫描 + 溢出「终点最远」区间；R3 caller 池：块内临时池缩到 r8/r9，r10/r11 只给不跨调用的槽，调用点 spill/reload。
- 途中修两个真实 bug：区间用「首次/末次出现」在 CFG 顺序 + 循环回边下错误（3 挂起 + 4 段错误），改为扩展到 live-in/out 覆盖的整块；SysV 下 r8/r9 是参数寄存器、r10/r11 是闭包/环境指针，全当 caller 池导致 matmul 算错（43198080 → 420883）。
验证：全量 205 ran ok；`PYADAMAS_NO_RA=1` 可回退旧分配器。设计见 [regalloc.md](docs/regalloc.md)。

## [0.202] - 2026-11-16

### Z5：表驱动展开成为默认路径

`--zero-cost-exc` 从默认关闭变为默认路径，另留 `--no-zero-cost-exc` 回退。可挂起函数（生成器/协程）永久保留 setjmp（5 处 C 级兜底抓不到 `_Unwind`，设计上的降级）。收益：全量用例 `_setjmp` 670 → 361（-46%）。沿途修 6 个「两套机制混用」的真实 bug：handler 体里的 setjmp、try 体以 raise/return 结尾、EhEnd 块被当死块裁掉、LP 缺 `pa_eh_leave`、可挂起函数进出未屏蔽 `pa_eh_depth`、早退时 finally 不执行。验证：全量 205 ran ok；z1_rt 116 / z2_rt 134 / z2_codegen 209 / tests/eh 33 / sem_baseline 42/42 全过。

## [0.201] - 2026-11-16

### 基准用例全部补上类型标注

9 个基准用例全部标注为 `list[float]` 等正确形式，输出与 CPython 逐字节一致（裸 `list` 会让元素退化成 unknown，是 nbody `math domain error`、spectral_norm 输出 0 的真正原因）。最终 release 基准见 [bench/compare/RESULTS-release.md](../bench/compare/RESULTS-release.md)：binary_trees 9.20×、fasta 13.14×、mandelbrot 24.16×、spectral_norm 26.51× 对 CPython。

## [0.200] - 2026-11-16

### 恢复四语言经典基准（CPython / PyAdamas / Go / C++）并重跑

从旧提交恢复 `bench/compare/` 的 9 个用例与 `run_compare.py`，装 Go 1.27.4 并加自动探测。定位：与 Go 大致同档，binary_trees / fasta 略快于 Go；matmul / nbody / spectral_norm 落后 3–6×（当时根因归为无寄存器分配，0.203 证实迁移后收益为零，真正瓶颈是模块顶层函数边界）。

## [0.199] - 2026-11-16

### 标准库新增 fnmatch / copy

- **fnmatch**：shell 通配翻译成正则，`*` / `?` / `[seq]` / `[!seq]` 全支持，四入口，8 项输出与 CPython 一致。
- **copy**：静态类型下 `isinstance` 不能收窄成具体容器，提供按类型分开的显式入口 `copy_list` / `copy_dict` / `copy_set` / `deepcopy_list` / `deepcopy_dict`；deepcopy 用「内层长度 + `is` 身份」验证。未实现 `__copy__` / `__deepcopy__` 钩子、类实例字段递归、环检测。全量 205 ran ok；模块表 37 个条目。

## [0.198] - 2026-11-16

### Z5 第一步：模块顶层的 try 也走零开销路径

`__pa_main` / `__pa_init` 是合成函数，模块级语句绕过 `collect_zero_cost_tries`。修复后在模块入口对顶层语句也跑形状判定。186 用例 `--zero-cost-exc` 下 `_setjmp` 582 → 429、仍有 setjmp 的用例 42 → 24；剩余里 22 个含 yield/async，非生成器只剩 with-in-try 与多参 raise 两个已知排除。全量 203 ran ok。

## [0.197] - 2026-11-16

### Z4 完成：try-else 接入；with 在 try 内明确排除

try-else 新增保护区外 else 块，四路与老路径、CPython 一致。with 在 try 内明确排除（证明做不了）：LP 里 `__exit__` 虽生成了正确汇编但运行期没被调用——with 用 setjmp 保护 `__exit__`，与外层 `_Unwind` 混用状态不一致；实测 203 用例中该结构 0 次。z2_codegen 209 项全过。Z4 至此完成：finally、嵌套 try、早退、try-else 全部接入。

## [0.196] - 2026-11-16

### Z4：嵌套 try 接入零开销异常路径

`zero_cost_try`（单指针）改成 `zero_cost_tries`（集合），逐个 try 独立判定；codegen `EhPlanRec` 从一条保护区改成一组 range，同函数多条保护区合成一份 LSDA。with 从「函数体里一个都不能有」改成「只拒绝含 with 的那个 try」。z2_codegen 176 → 198 项全过，全量 203 ran ok。

## [0.195] - 2026-11-16

### Z4：早退（return/break/continue）穿过零开销保护区

`FuncCtx::Cleanup` 加 `ZeroCostEh`（带临时值水位槽），`emit_cleanup_exits` 遇到它发 `pa_eh_leave` + `pa_rc_cleanup`，否则 `pa_eh_depth` 不归零会导致后续异常误走 `_Unwind`。新增 `16_early_exit.py`，z2_codegen 165 → 176 项全过。

## [0.194] - 2026-11-16

### Z4：finally 接入零开销异常路径

实测 112 处 try 里 finally 占 42（最大头）。两个关键 bug：带 finally 的 try 在 `lower_try` 里直接走 `lower_finally` 到不了新路径；finally 不能内联两份到同一落点。改成独立 fin 块，异常路径用 `pa_eh_rethrow` 重启 phase 1，外层仍接得住。z2_codegen 143 → 165 项全过。

## [0.193] - 2026-11-16

### itertools 补 6 个算子（stdlib 自举）

`takewhile` / `dropwhile` / `filterfalse` / `compress` / `pairwise` / `groupby`；`kModuleTable` 里 itertools 从 `Native` 改成 `NativePython`。统一接收 `list`、返回 `list`（非惰性迭代器）。未提供 `starmap` / `count` / `cycle` / `tee`。顺带确认 `collections.namedtuple` 做不了（运行期生成类与编译期布局冲突）。全量 203 ran ok。

## [0.192] - 2026-11-16

### Z3：多 handler 分派链 + typed catch 接入零开销路径

此前零开销只接「单层 try + 一个 catch-all」，185 用例里 0 个能走新路径。去掉 handler 数量/类型限制，except 改成链式分派，全不匹配走 `pa_eh_rethrow`（重启 phase 1，内层不匹配时外层仍接得住）。新增 3 用例，z2_codegen 110 → 143 项全过，全量 202 ran ok。

## [0.191] - 2026-11-16

### Z2-b3：C 运行时抛出的异常也能被零开销 try 抓到

所有抛出点收口到 `rt_exc.c` 的 `exc_longjmp`，新增 `pa_eh_depth` + `pa_eh_enter` / `pa_eh_leave`，按深度二选一走 `pa_eh_throw` 或 `longjmp`。三条 C 运行时异常三方输出逐字一致，z2_codegen 110/110 PASS，全量 202 ran ok。

## [0.190] - 2026-11-16

### 签名无浮点的 shim 退化成单条 jmp

签名无浮点时 shim 只发一条 `jmp .LfN`。间接调用 2000 万次 53ms → 46ms，`22_exception` 的 .text 从 +384 字节 → +64 字节。全量 202 ran ok。

## [0.189] - 2026-11-16

### 形参类型混用（float/非 float）改成编译期报错

`bind_param` 回填分支在浮点/非浮点混用时报错要求标注，消除静默算错。全量 202 ran ok。

## [0.188] - 2026-11-16

### 间接调用统一 ABI 约定

闭包 +0 指向 shim（非函数本体），所有间接调用走统一约定（实参进整型寄存器、返回值 `%rax`），修浮点经 unknown 函数值传递的静默算错。全量 202 ran ok；四套异常夹具无回退。

## [0.186] - 2026-11-16

### 字符串码点语义 S1+S2

[str-codepoints.md](docs/str-codepoints.md) 的 S1+S2：`PyAdamasStr` 24 → 32 字节，`pub`（str 码点数 / bytes 字节数）放偏移 0，`len` 与真值判断指令一行未改；字面量编译期算好 pub；下标/切片/迭代/方法按码点，bytes 保持字节；顺序迭代游标 + 全 ASCII 快路径；新增 `len_bytes(s)`；cimport 取 data 偏移 16 → 24。性能对比 0.184 在 0.87–1.05×，迭代线性。全量 201 ran ok。S3 未做。

## [0.187] - 2026-11-16

### 修 `functools.reduce` 传浮点回调时的静默算错

现象：`reduce(fadd, [1.5, 2.5])` 输出 `4612811918334230528`（= `1.5` 的 IEEE-754 位模式）。

**根因不是跳板缺失**（`pa_call2_f` 早就存在），而是**编译器选错了跳板**：
`lower_functools` 只看结果类型来决定用整型版还是浮点版，而 `reduce` 的结果类型来自
`functools_call` 的 `return elem`；当 `xs` 是裸 `list` 形参或没有标注返回时 `elem` 就是
`unknown`，于是选了整型版 `pa_functools_reduce`，浮点元素的位模式被当整数塞进 `%rdi`。

修复：
- `typecheck_call_modules.cpp`：`reduce` 的结果类型改为按**回调形参类型**推导 ——
  两个形参都是 `float` 且元素类型为 `float` / `unknown` / `int` / `bool` 时，结果类型为 `float`；
- `lower_import_misc.cpp`：`use_fp = 结果类型为 float || 回调两个形参都是 float`；
  元素类型为非数值时给出明确错误，而不是静默算错。

验证：原始例子从位模式变成 `4.0`（带初值 `4.5`），与 CPython 一致；
新增 `tests/run/182_dynamic_call_float.py`；全量 **202 ran ok / 0 FAILED / 0 OUTPUT DIFF**；
四套异常夹具无回退。

影响面已逐个核对（`pa_call1` / `pa_call2` 全部 4 个调用点）：只有 `reduce` 的整型版会传浮点，
其余（Future 回调、`re.sub` 替换回调）形参类型固定，类型检查会拒绝 `float` 标注，无需改。

**仍未修的架构性限制**（不在本任务范围，已在文档登记）：
- `unknown` 形参的函数（`def f(a, b): return a+b`）本身按整数 ABI 编译，被浮点实参调用时仍算错；
- 元素类型为 `unknown` 的裸 list 被按 double 读时，int 元素会读错（`fold([1,2])` 得到 `1.5e-323`）。
  这两个都需要「元素域上的运行期信息」才能修，与 0.187 的选择问题不同源。
## [0.185] - 2026-11-16

### 修用户类一元运算符的 SIGSEGV（`__neg__` 等）

现象：用户类定义 `__neg__` 后取负直接 **SIGSEGV(139)**。

**根因不是 dunder 查表缺失，而是「一元运算整条链都没做类实例分派」**：

1. `typecheck_expr.cpp` 的 UnaryOp 对 MINUS/PLUS/TILDE 一律返回操作数自身类型（Class），
   TILDE 还直接判死 `expected int`；
2. `lower_expr_ops.cpp` 的 `lower_unary` 没有任何类分支，直接落 `IrOp::Un`（`negq`/`notq`
   机器整数指令），**把对象指针取负**；
3. 结果 vreg 的静态类型仍是 Class（`ir_is_heap` 为真），降级层对野指针 `pa_rc_inc` -> 崩溃。

二元运算有 `lower_dunder_binop` / `dunder_binop_ret`，一元完全没有对应物。

修复（9 个文件，+137/-12）：
- 新增 `TypeChecker::dunder_unary_ret`：按 MRO 查单参 dunder 的声明返回类型；
- MINUS/PLUS/TILDE 对 Class 走 `__neg__` / `__pos__` / `__invert__`，无实现且非开放类则报错
  （不再静默）；`lower_unary` 加类分支走 `dunder_call`；
- `abs()` 走 `__abs__`（原来把对象指针当 i64 取绝对值）；`operator.neg/pos/abs/invert` 同样走 dunder；
- `AnnAssign` 到 `self.attr` 时补登记接收者类型（顺带修掉一个 `unsupported assignment target`）；
- `dunder_len` 容忍 bool 返回、`dunder_bool` 容忍 int 返回（否则静默退回指针判空，结果相反）。

`__pos__` / `__invert__` / `__abs__` 与 `__neg__` **完全同根因**，一并修；
`__bool__` / `__len__` 根因不同（本来就有 dunder_call，但只认精确返回类型），也已修。

验证：字面复现从 rc=139 变成 rc=0 且输出 `-3` 与 CPython 一致；新增
`tests/run/176_unary_dunder.py`；全量 **195 ran ok / 0 FAILED / 0 OUTPUT DIFF**；
z1_rt 116、z2_rt 134、z2_codegen 99、tests/eh 33、sem_baseline 42 全部无回退。
## [0.184] - 2026-11-16

### 零开销异常 Z2-b2：codegen 接线打通（`--zero-cost-exc`，默认关闭）

最小闭环完成。同一份 `.py` 用两条路径各编一次，核心证据：

| | 老路径 | 新路径 |
|---|---|---|
| `call _setjmp` | 有 | **0 处** |
| `call pa_handler_push` | 有 | **0 处** |
| `.cfi_personality` / `.cfi_lsda` / `.gcc_except_table` | 无 | 齐全（GCC 同款 catch-all 形态） |
| stdout | 基准 | **逐字节一致** |

- 开关 `--zero-cost-exc` 默认关；关闭时 **187/187 个文件的 asm md5 与 HEAD 完全一致**
  （隔离树 HEAD + 仅本补丁验证）。
- 形状门禁（`lower_exc.cpp: find_zero_cost_try`）：非可挂起 + 恰好一条 try + 零 with +
  单 catch-all handler + 无 else/finally + try/handler 体内无 return/break/continue。
- IR 新增 `EhBegin/EhEnd/EhLandingPad` 标记与 5 个 `RtFn`；LP = `pa_eh_land` -> `RcCleanup` ->
  **现有 except 分发块一行不改**；不匹配走 `pa_eh_rethrow` 重启 phase 1。
- 新增 `tests/exc/z2_codegen/run.sh`：9 用例 × 11 断言 = 99 项全过。
- 压力：10 万次跨两个被跳过帧的 throw/catch，新路径 0 setjmp / 2 LSDA，maxrss 11324KB
  （老路径 11348KB）——方案 G 的帧清理接线正确、无泄漏。
- 注意：**现有 177 个用例里没有任何函数满足第一刀门禁**，所以开关打开对现有套件零影响；
  新路径目前只由专用夹具覆盖。

### 模块注册表：从 3 处硬编码名单变成单一数据源

- `src/driver_build.cpp` 新增 `kModuleTable`（36 行，`ModuleKind` = Native / Python / NativePython），
  删除原来散在三处的重复名单（`program_module`、`check_builtin_name`，以及 `visit_unit` 里的
  import 跳过名单）。
- `NativePython` 语义：导入时把 `stdlib/<source>` 编成模块，对外接口 = 原生接口 ∪ 扩展源新增名字，
  **原生名字优先**（扩展不允许覆盖原生，会破坏现有语义）。
- **登记门**：`stdlib/` 里的 `.py` 未登记则直接报错，保证表是唯一来源；
  Debug 构建每次 load 跑 `validate_module_table()` 自检源文件与依赖。
- 证明：改动前的二进制跑新用例报 6 个错（`module 'functools' has no attribute 'cache'`…），
  改动后输出与 CPython 一致。
- 现在加一个新模块 = 表里 **1 行** + 一个 `.py`；给原生模块补 API = **0 行 C++**。

验证：全量 194 ran ok / 0 fail；z1_rt 116、z2_rt 134、tests/eh 33 项全过。
## [0.183] - 2026-11-16

### GC：修复 54 个函数的「构造期对象不在 GC 根上」窗口（一整类 UAF）

0.181 修掉异常路径的 GC 根缺失后，沿着同一类问题做了系统性审计。

**方法**：脚本递归统计「能传递触发 `rc_alloc`」的函数共 243 个 → 取「≥2 次分配调用」的候选 80 个
→ 逐个人工核对「第二次分配发生时，第一次的结果是否只存在于 C 局部变量里」。

**结果**：修复 **54 个函数、70 个 pin 点**，另修 **2 处全局根的登记顺序**（改成「先登记根、再分配」）。
新增统一助手 `pa_gc_pin` / `pa_gc_unpin`：不转移所有权（临时 +1 引用并挂临时值栈，unpin 撤销），
所以既能 pin 自己刚分配的对象，也能 pin 借来的参数；**异常安全**——pin 的引用落在临时值栈上，
`longjmp` 时由 handler 的清理水位统一释放。

**修复前必崩的实际路径**（都实测到 139）：`split` / `split(sep)` / `splitlines`、`dict.items`、
`re.findall`、`Path.parts`、`itertools.permutations(9)`、`file.readlines`、`json.loads` 深嵌套、
`asyncio` 的 `wait_for`/`gather`/`create_task`、`subprocess`、`sha256_hex`、`re.sub` 的用户回调等。
这些都是「平时跑得好、GC 压力一大就炸」的形态。

**根因证据**：新增 `tests/run/174_gc_construct_roots.py` 在**修复前**的运行时上 3/3 稳定 SIGSEGV；
修复后连跑 5 次 rc=0，`MALLOC_CHECK_=3 MALLOC_PERTURB_=170` 下同样稳定，
`PYADAMAS_GC_CONSERVATIVE=1` 下输出一致。

**没有采用「保守扫整段 C 栈」**——那会把 0.175 的根扫描收窄对冲掉；本任务坚持定点 pin 加固。

验证：全量 194 个运行用例 ran ok、59 个反例按预期 rejected，0 失败。
## [0.182] - 2026-11-16

### 零开销异常 Z2-b1：运行时胶水 + 方案 G 的帧清理（personality 侧，尚未接 codegen）

- 新增 `runtime/rt_eh.c`：`pa_eh_throw`（把异常包进 `_Unwind_Exception`，16 字节对齐、
  `private` 清零；`_URC_END_OF_STACK` 时返回 `PA_EH_UNCAUGHT` 交上层打印并退出）、
  `pa_eh_landing`（取回 `PyAdamasException*`，装好 `pending_exc` / `current_exc_inst` /
  `pending_exc_args`）、`pa_eh_done`。
- `PyAdamasFrame` 加 `cfa` 字段（帧记录 5 字 -> 6 字），codegen 序言在压帧**之前**把 CFA 写进去；
  `pa_frame_unwind_to_cfa` 逐行对齐 `pa_frame_unwind` 的 bit0/bit1 处理，并加重入 guard 与
  不变量 panic；personality 在 phase 2 的 `_UA_HANDLER_FRAME` 分支、`_Unwind_SetIP` **之前**
  经钩子调用它（不链 `rt_eh.c` 的 Z1 夹具里钩子为 NULL，行为不变）。
- 新增 `tests/exc/z2_rt/`：手写三层帧夹具（owned / bit0 / 只扫 bit1 三种槽）+ driver +
  `run.sh`（PIE 与 -no-pie、GC 开/关、两组反证）。
- 本轮**生成代码仍走 setjmp**，raise / landing pad 的接线是 Z2-b2。
- 非抛出路径的新成本：有帧记录的函数 +8 字节栈与 2 条指令。

### 两处实测订正（已写回 `docs/exc-frame-cleanup.md`）

1. **`_Unwind_GetCFA` 的语义和设计稿假设不同**：在 personality 里它返回的不是当前帧自己的
   DWARF CFA，而是「当前帧直接调用出去的那一帧」的 CFA（约等于该帧 call site 的 rsp）。
   实测 handler 帧真 CFA = rec+112，拿到的是 rec+0；Z1 那种 8 字节帧两者只差 16，所以此前
   看起来「就是自己的 CFA」。因此停止条件必须是 `cfa <= target`，用 `<` 会漏掉最深一帧。
2. **「异常路径每次泄漏 0.63 KB」被证伪**：0.181 查明那次规模相关的崩溃是 GC 根缺失，
   修好后 1 万 / 10 万 / 100 万次异常的峰值 RSS 都是 1.9–2.3 MB，没有泄漏。
   「用 RSS 不随规模增长做 Z2 验收」这个指标仍然有效。

验证：`tests/exc/z2_rt/run.sh` 134 项断言全过（含两组反证：不压记录时释放计数 FAIL、
不释放时 RSS 涨 29.8 MB 被抓）；`tests/exc/z1_rt/run.sh` 116 项仍全过；
全量 `tests/run_tests.sh` 204 用例 0 失败。
## [0.181] - 2026-11-16

### 修复异常路径的 GC 根缺失（10 万次异常循环必崩的 UAF）

现象：`tests/run/92_exc_inline_rc.py` 把循环从 2000 改成 100000 后**必然 SIGSEGV(139)**，
20000 以内正常。gdb 断在 `pa_exc_accept_value -> pa_rc_inc`，即对已释放对象做 `rc_inc`。

**根因不是泄漏，是 GC 根缺失**（`runtime/rt_exc.c`）：

1. `pending_exc_args` / `current_exc_inst` 是 RC owned 的普通 C 全局，**从未登记为 GC 根**。
   raise 与 accept 之间只要有一次分配触发主收集，它们就会被清扫掉；
   实测 gdb 抓到 args 表内存已被 `PyAdamasExcValue` 复用（`->len` 读到 `EXCVAL_MAGIC`）。
2. 异常值构造期间（`pa_list_new` / `str_new` / cause 递归）新对象只存在于 C 局部变量里，
   主收集同样看不见 —— 只修第 1 条后，20 万次仍会崩在 `exc_new(type=NULL)`。

修法：
- 首次 raise 前把两个全局槽地址 `pa_gc_add_roots` 登记为根；
- `exc_value_alloc` / `pa_exc_value_from_exc` 构造期间用临时值栈（`pa_rc_temp` /
  `pa_rc_temp_take`，本来就是 GC 根）挂住半成品，交还调用方时再取回所有权。

归因证据：修前 `PYADAMAS_GC_DISABLE=1` 下 rc=0（确证是 GC 误回收）；
`PYADAMAS_GC_CONSERVATIVE=1` 下仍 139（排除 0.175 的位图收窄）。

验证（`tests/run/173_exc_loop_100k.py` 新增）：10 万次连跑 5 次 rc=0，输出与 CPython 一致；
派生用例 10 万 / 50 万 / **100 万** 次均 rc=0；`MALLOC_CHECK_=3 MALLOC_PERTURB_=170` 下同样稳定；
`-O0/-O1/-O2` 与 runtime `-O0` 全过。
**峰值 RSS 不随规模增长**（1 万 / 10 万 / 100 万次都是约 1.9–2.3 MB）——
之前「每次异常泄漏 0.63KB」的判断是错的，修好根之后没有任何泄漏。

遗留：其它 runtime 模块存在同类「构造期只在 C 局部、不在根上」的窗口（如 `loop_ensure`
连发 4 个 list、`pa_sys_set_args`），本次只覆盖异常路径，建议后续抽统一 pin 助手再全量审计。
## [0.180] - 2026-11-16

### 标准库：新增 7 个模块 + 3 处补全，并给出覆盖缺口矩阵

用 51 个常用 CPython 模块做了实测（`import` + 2~3 个常用 API），结论是
**27 个原有可用 / 24 个完全缺失 / 0 个「能 import 但运行失败」**。矩阵与补齐成本见
[`docs/stdlib-gaps.md`](docs/stdlib-gaps.md)。

新增（全部是 `stdlib/*.py` 自举实现，不动 `src/` 与 `runtime/`）：
`typing` / `enum` / `abc` / `collections.abc` / `glob` / `uuid` / `fractions`；
补全 `textwrap`（`TextWrapper`）、`statistics`（`quantiles`）、`csv`（`DictReader`/`DictWriter`）。

其中 `typing` 桩模块的收益被低估了：注解解析器（`src/typecheck.cpp`）本来就按名字特判 typing
名字，所以一个纯桩模块就能解锁大量真实脚本的注解。

新增 10 个用例（`tests/run/163`~`172`）。验证：191 项全过（基线 181 + 新增 10）。

### 已知阻塞（需决策，未做）

- **内建模块名单是硬编码的**（`src/driver_build.cpp:609-618` 与 `:890-897`），所以
  `stdlib/functools.py` 这类会被挡住 —— 补 functools / itertools / collections 必须先把名单
  改成数据表。
- `dataclasses`：codegen 不合成 `__init__`，shim 会让 `P(1,2)` 静默变成 `0,0`，不能做。
- `contextlib`：unknown 函数值的 `*args` 转发被编译器拒绝。

### 顺带发现的既有编译器缺陷（非本次引入，已记入 stdlib-gaps.md）

- 用户类定义 `__neg__` 后取负 **SIGSEGV（exit 139）**；
- `list(obj)` 不能消费只有 `__iter__` 的用户类。
## [0.179] - 2026-11-16

### 汇编发射换成结构化 Machine 层（行为逐字节不变）

codegen 过去把 AT&T 文本直接拼进 `std::string`（约 250 处 `std::format` + 250 处 `out_ +=`）。
现在 `AsmGen::out_` 是 `MachFunc`：标签 / 指示符 / 指令 / 注释 / 原始文本的序列，
指令再拆成操作码 `MOp` 与操作数 `MOperand`，由 `render()` 拼回文本。

- 新增 `src/mach_instr.h`（数据模型）与 `src/mach_emit.cpp`（一行文本 -> MItem 的解析 + 渲染）。
- 迁移策略是**先换容器、再逐点结构化**：历史发射点通过 `operator+=`（等价 `append_raw`）与
  `line()`（解析成结构化 item，认不出的文法退化 Raw）零改动接进来，所以这一轮生成的汇编
  **与改动前逐字节一致**。
- 验收门槛：176 个文件（`tests/run/*.py` + `tests/*.py`）逐个 `pyadamas asm` 的 md5 全部与
  改动前相同 —— 这是「行为不变」的硬证据，比「测试通过」强。
- 后续工作：219 个 `out_ +=` 直写点逐步换成 `instr()` / `directive()` / `label()` 直接构造；
  之后寄存器分配才能在指令层改写操作数。

**已知坑**：`MachFunc::push_line` 的契约是「已含前导 tab 的一整行」，而旧的 `line()` 自己会补 tab。
两者契约不一致时，所有走 `line()` 的指令都会丢掉前导 tab —— 编译通过、汇编也能过、
`readelf` 一切正常，只有字节变了。用复用缓冲区补 tab 修掉（`line_buf_`，避免每行一次分配）。
## [0.177] - 2026-11-16

### 零开销异常 Z2-a：LSDA 发射器（已就绪，尚未接入 codegen）

新增 `src/eh_emit.h` / `src/eh_emit.cpp`：纯函数式的 LSDA / CFI **文本发射器**，
输入一个函数的保护区计划，输出 `.cfi_personality` / `.cfi_lsda` / `.gcc_except_table`。

- 两种形态：handler（`ttype=0x9b` + TType 表一个 null 项 + `filter=1`）、
  全 cleanup（`ttype=0xff`、12 字节，TType 偏移字段整段消失）。
- 输入支持数值偏移与标签差（`.uleb128 .LEHE0-.LFB0`）两种模式，产物字节相同。
- 接口拆成三段（CFI 指令必须夹在 `.cfi_startproc`/`.cfi_endproc` 之间、LSDA 在
  `.gcc_except_table`、DW.ref 每个 TU 一次），这是位置约束决定的，不是设计选择。

**订正（实测）**：call-site 的 action 字段是 **action 表内的 1 起字节偏移**，不是记录序号
（多 handler 应为 1, 3, ...）；按序号实现会被独立解码器判为结构非法。另外单 handler
且没有 LP 覆盖 range 时是 16 字节 —— 20 字节基准必须**含第二条 `lp=0` 的 range**。

验证：`bash tests/eh/run.sh` 33 项断言全过；`catch_gcc20` 与 `cleanup_gcc12` 两组产物与
gcc 14.2 实测基准**逐字节一致**；另用 `tools/lsda_ref/decode_eh_lsda.py` 独立解码 6 份 `.o`，
结构校验 0 处「结构非法」。

尚未接入 codegen（Z2-b），也未处理 LP 覆盖 range 与 typed catch。
## [0.176] - 2026-11-16

### 零开销异常 Z1.5：handler 判据对齐 ABI + 真 gcc personality 交叉验证

交叉核对（`tools/lsda_ref/CROSSCHECK_M2.md`）指出 Z1 的 personality 用「call-site 的 action
下标 != 0 即 handler」是**私有语义**：ABI 里 `filter == 0` 就是 cleanup，catch-all 必须
`filter > 0` **且** TType 表里有一个 null 项。这一版把它对齐。

- `pa_personality` 解析 action 记录链（两个 SLEB128 的 filter / next）：action 下标 == 0 或
  `filter == 0` -> cleanup；`filter > 0` -> 读 TType 表（反向索引 `ttype_end - filter*size`，
  并实现 libgcc 的 `result == 0` 特判 —— 这正是 null 项为「4 字节 0 + 无重定位」的原因）；
  `filter < 0`（exception spec）-> `_URC_CONTINUE_UNWIND` + 诊断计数。
- 夹具升级为 GCC 同款形态：handler 20 字节（`ttype=0x9b`、偏移指向表末尾、null 项、`filter=1`）、
  cleanup 12 字节、无 pad 8 字节；personality 指针用 `0x9b` 指向 driver 里的运行期槽，
  于是**同一份 .S 的同一套字节可以被两种 personality 解释**。
- cleanup / handler 的 landing pad 区段补了覆盖 LP 的 call-site 记录 —— 缺它时标准
  personality 在 `_Unwind_Resume` 阶段会 `found_terminate` -> terminate。
- 负向用例重做：`0x9b` -> `0xfe` 断言「编码非法」，另加 udata8 编码的夹具覆盖「表越界」分支。
- 新增**真 gcc personality 交叉验证**（`tests/exc/z1_rt/crosscheck_gxx.cpp`）：同一份 `.S`
  换填 `__gxx_personality_v0`，真 C++ `throw 7` + 外层 `catch(int)`。实测
  `caught=0 / lp_calls=1 / fn_ret=7 / z1_lp_seen=1` —— 夹具帧被标准 personality 正确当成
  catch-all（异常被吃掉、landing pad 只跑一次）。PIE 与 -no-pie 两套都过。

`filter < 0` 的处理选择：继续展开 + 诊断计数，**不 terminate**。理由是我们没有 spec 表与
`__cxa_call_unexpected` 协议，做不了 ABI 的正确行为；贸然 abort 会直接杀用户进程，
保守继续 + 可观测计数更可控，且不会错误吞异常。Z4 做 spec 时在该分支补实现即可。

验证：`bash tests/exc/z1_rt/run.sh` 退出码 0，两套链接共 **116 项断言**（43 -> 58 / 套，
含 2 次交叉验证）；`tests/run_tests.sh` 179 项全过。
## [0.175] - 2026-11-16

### GC 根扫描：整段保守扫描 -> 位图收窄（技术债 #1 / #2）

`docs/roadmap.md` 把技术债「按什么时候会疼」排过序，第 1 条是「GC 的根只扫登记区、
临时区仍整段保守扫描、没有栈映射」，第 2 条是「unknown 引用计数不精确、靠保守扫栈兜底」。
两者共同的代价是**误报保留**：临时区里一个恰好长得像指针的整数，会把一整片垃圾对象钉在堆上。

- 临时区保守扫描按 vreg 静态类型收窄：编译期为每个帧发射一张位图（`scan_mask`，放 `.rodata`），
  GC 只扫置位的字；**纯标量帧直接 `scan_words = 0`，整段跳过**。
- 兜底方向只有「宁多勿少」：`unknown` / 无类型 / `Optional[unknown]` / `BoundMethod` /
  非 C `StructPtr` 一律置 1；vreg 槽复用按字取并集；vreg 槽号与表对不上、或收窄不到一半
  就不发位图、退回整段扫；生成器/协程帧不参与；`unknown` 名字槽仍按精确地址登记，不受位图影响。
- 新增 `PYADAMAS_GC_STATS=1`：每轮打集合/释放/耗时/存活对象与字节，根扫描按
  「帧数 / 登记槽 / 临时区实扫÷整段 / 寄存器字数 / 位图是否生效」分解，并给出
  `cons_kept`（先精确根扫排空、再扫保守根时新标活的对象数与字节，按临时区 / unknown 槽 /
  寄存器分项）——这是「误报保留」的直接度量。默认关闭时**零额外开销**（启动读一次环境变量）。
- 新增 `PYADAMAS_GC_CONSERVATIVE=1`：忽略位图，完全退回整段保守扫描。

实测（全量 163 个 `tests/run`、143 轮收集）：

| 指标 | 收窄前 | 收窄后 |
|---|---|---|
| 临时区实扫字数 | 2901 | **403（13.9%）** |
| 保守保留对象 / 字节 | 92 个 / 8144 B | **0 / 0** |

单例：`13_gc` 35→3 字、`151_gc_generations` 33→1 字、`92_exc_inline_rc` 42→1 字。

验证：179 个用例在收窄与回退两种模式下各 **179 pass / 0 fail**；`MALLOC_PERTURB_=170` +
`MALLOC_CHECK_=3` 下再跑一遍同样全过；16MB `RLIMIT_AS` 下拼接/列表/对象环/dict 自环/闭包/
字段/切片压力程序 `rc=0`，峰值 RSS **7808KB（收窄）/ 8020KB（回退）**。
新增 `tests/run/160_gc_scan_narrow.py`（用 `id()` 造「恰好等于堆地址的整数」，收窄前
保守保留 86 个 / 7568 字节，收窄后归零）与 `161_gc_conservative_equiv.py`（回退开关语义等价）。

范围与前提：位图按**字**收，不按**时刻**收（不活跃的 unknown 字仍会扫）；仍没有增量/并发 GC
和完整栈映射；收窄依赖「类型化堆引用要么 owned、要么持有者从根可达」这一既有前提。
## [0.174] - 2026-11-16

### 零开销异常：Z1 地基打通（不改默认代码路径）

目标是把 setjmp/longjmp 换成 Itanium C++ ABI 的表驱动展开。这一步**不碰编译器**，
只验证 personality 契约、LSDA 编码、landing pad 跳转这三件最容易出错的事。

- 新增 `runtime/rt_personality.c`：通用解码 uleb128/sleb128/encoded pointer 与
  LPStart/TType/call-site 表头，实现两阶段语义（search 只认 action != 0 的 handler 帧；
  cleanup 阶段对纯清理帧同样安装上下文，由 landing pad 自己 `_Unwind_Resume`），
  `_UA_FORCE_UNWIND` 下只穿过、不安装。已编进 `rt.o`，默认代码路径不引用。
- 新增 `tests/exc/z1_rt/`：四份手写 `.S` 夹具（无 LSDA 的 plumbing / 真 LSDA /
  两阶段 + 无 pad 中间帧 / 只改 1 bit 的坏表负向）+ driver，`-no-pie` 与 PIE
  两套链接各 43 项断言共 86 项全过；`bash tests/exc/z1_rt/run.sh` 可复跑。
- 新增 `tools/lsda_ref/`：用 gcc 14.2 / binutils 2.44 实测标定 LSDA 字节布局
  （100+ 条硬编码字节断言、11 条链接矩阵断言，`check.sh` 可复跑）。手写汇编产出的
  `.gcc_except_table`（20 B）与 `.eh_frame`（64 B）和 `g++ -O1` 的 catch-all 产物
  **逐字节完全一致**。
- 新增 `docs/exc-semantics.md`（现行异常语义 60+ 行 file:line 对照表、10 条已知不一致、
  三条难点的具体结论、Z2 发射清单）与 `tests/exc/sem_baseline.sh`（42 个异常用例基线，
  `PASS 42 / 42`）；重写 `docs/zero-cost-exceptions.md`。

### 订正与范围收紧（实测推翻了若干早期结论）

- `0x9b` / `0x1b` 的字节对没错，但语义是 `sdata4` 不是 `udata4`（`udata4` 是 0x03）。
- 与 gcc 同构的形态下 **catch-all 的类型表不能省**：gcc 用 `ttype=0x9b` + 一个 null 项
  表示 `catch(...)`；省掉会让 catch 退化成 cleanup，永远抓不到异常。
- action 表不是「条数 + 计数」，而是每条记录两个 SLEB128 的 `(filter, next)`。
- 判断 catch / cleanup 的正确层次是 **call-site 的第 4 个字段（action 下标）是否为 0**。
- Z2 范围收紧为「非可挂起函数 + catch-all 单层 try」：生成器/协程/异步生成器里的 5 处
  C 级 setjmp 兜底抓不到 `_Unwind` 抛出的异常，**永久保留 setjmp**；`with` 可能抑制异常，
  不是纯 cleanup，不一起迁。
- Z3 的类型过滤必须放进 personality 的 phase 1：展开器在 phase 1 只搜索一次，
  内层 except 不匹配后 `_Unwind_Resume` **不会**重新选外层 handler。

### 已知坑（都复现过，详见 `tests/exc/z1_rt/README.md`）

- landing pad 入口的 `%rsp` = 抛出点那条 `call` 之前的 rsp（不是 CFA、不是入口 rsp）。
- landing pad 必须落在 `.cfi_endproc` 的 FDE 范围内，否则 LP 里 `_Unwind_Resume` 时
  libgcc 找不到 FDE -> `SIGABRT(134)`，且**没有任何错误输出**。
- `.cfi_personality 0x9b, <函数名>`（省掉 `DW.ref` 指针槽）汇编和链接都不报错，
  运行期 SIGSEGV。
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
