# 现行异常语义清单（Z2/Z3 改造对照基线）

状态：基于当前工作区（setjmp/longjmp 实现，`--zero-cost-exc` 开关尚不存在）。
用途：Z2/Z3/Z4 改造时逐条对照，改完用 `tests/exc/sem_baseline.sh` 回归。
证据约定：`file:line` 均为当前工作区行号；标「实测」的结论来自 `build/dev/pyadamas`
编译运行的真实输出（探针源码见各条说明，均可在任意临时目录复现）。

## 0. 一页结论

- 真正决定语义的不是展开机制，而是 **4 个全局变量 + 3 个 IR 操作 + lowering 的 landing 块结构**：
  `handler_top` / `pending_exc` / `current_exc` / `pa_frame_top`（runtime/rt_exc.c:9-15）；
  `HandlerPush` / `HandlerLeave` / `Setjmp`（src/ir_ops.h:229-231、1108）。
  表驱动只需要替换这一层；`try.body` / `try.land` / `except.body` / `except.run` /
  `finally.land` / `with.land` 这些块可以整体保留（src/lower_exc.cpp:204-349）。
- 每个 `try`（含只有 finally 的 try、每个 `with` 项、`async for` 每次取元素）都压一条
  handler 记录：`{jmp_buf env; prev; int64_t mark; PyAdamasFrame* frame}`
  （runtime/rt_internal.h:331-338），记录用 malloc + 空闲链复用（rt_exc.c:216-225、283-297）。
- `raise` 只做两件事：设 `pending_exc`，`pa_frame_unwind(handler_top->frame)` 释放被跳过帧的堆槽，
  然后 `_longjmp(handler_top->env, 1)`（rt_exc.c:204-214、450-469）。
- landing 块通用骨架 = `handler_leave`（弹记录 + `pa_rc_cleanup(mark)`，rt_exc.c:300-306）→
  类型匹配 / `__exit__` / finalbody → 命中则 `exc_accept`+`exc_done`，不命中 `exc_reraise`
  （src/lower_exc.cpp:231-337、src/lower_stmt_with.cpp:374-395）。
- 早退（return/break/continue）**不走 longjmp**：编译期 cleanup 栈直接内联发射
  `handler_discard` + `__exit__` + finalbody（src/lower_stmt_with.cpp:75-137，
  调用点 src/lower_stmt.cpp:169、187、196、208）。
- 可挂起函数把 handler 记录从全局栈摘下/重挂并重新 `setjmp`（rt_exc.c:320-332；
  src/lower_gen_async.cpp:430-544），异常上下文另用 `pa_exc_enter/leave` 节点栈换进换出
  （rt_exc.c:371-433）。
- 已知不一致 10 条 + 2 条改造陷阱见第 4 节。其中 3 条是改造后仍会存在 / 会被放大的：跨模块自定义异常
  不能出现在 `except`/`raise` 名里（4.8）、没有隐式 `__context__`（4.2）、生成器里逃出的
  `StopIteration` 不转 `RuntimeError`（4.9）。

## 1. 机制坐标（谁负责什么）

| 层 | 文件:行 | 职责 |
|---|---|---|
| lowering | src/lower_exc.cpp:187-350 | `try/except/else` 的块结构与 landing 分发；`:352-463` finally；`:70-184` raise |
| lowering | src/lower_stmt_with.cpp:6-19 | `push_handler` / `setjmp_handler` 两个原语 |
| lowering | src/lower_stmt_with.cpp:75-137 | `emit_cleanup_exits`：return/break/continue 的清理内联 |
| lowering | src/lower_stmt_with.cpp:234-396 | `with` 每项一层 handler + `__exit__` landing |
| lowering | src/lower_stmt_async.cpp:6-413 | `async with`（含内置锁/事件/信号量/超时/任务组特例） |
| lowering | src/lower_gen.cpp:72-112、320-368 | yield/await 挂起前摘 handler、换异常上下文 |
| lowering | src/lower_gen_async.cpp:389-544 | resume 入口按 state 重挂 handler + 重新 setjmp；throw 注入分发 |
| IR | src/ir_ops.h:229-257、281-290 | `HandlerPush/Leave/Discard`、`Exc*`、`Gen*` 运行时函数表 |
| IR | src/ir_ops.h:1108 | `IrOp::Setjmp`；打印在 src/ir.cpp:145-146 |
| codegen | src/codegen_call.cpp:906-916 | `setjmp` 指令发射（`call _setjmp`，记录首字段当 jmp_buf） |
| codegen | src/codegen_call_rt.cpp:132-... | 运行时函数实参编组；handler/exc 在 `:143`、`:148-157`、`:233-240`、`:279-281` |
| codegen | src/codegen_block.cpp:209-371 | 函数序言/尾声、CFI、压帧记录（`:293-323`）、`.cfi_endproc`（`:371`） |
| codegen | src/codegen_regalloc.cpp:12-55 | 帧布局 + 尾声（弹帧记录 `:36-39`）；`:267-315` handler 寄存器 hazard |
| codegen | src/codegen_ops.cpp:104、168-198 | Setjmp / GenYield / GenAwait / GenRethrow / GenDone 发射 |
| codegen | src/codegen.cpp:253-465 | 手写汇编：析构器、GC release、生成器槽释放、C 回调跳板、模块 init 桩、`main` |
| 运行时 | runtime/rt_exc.c | handler 栈、帧展开、异常对象、匹配、上下文栈、ExcValue；全文件 731 行 |
| 运行时 | runtime/rt_gen.c:63-107、211-267 | 生成器 `next/send/close/finalize` 的 C 级 setjmp 兜底 |
| 运行时 | runtime/rt_async.c:732-788 | 协程 `coro_step` 的 C 级 setjmp 兜底 |
| 运行时 | runtime/rt_agen.c:120-179 | 异步生成器驱动的 C 级 setjmp 兜底 |
| 运行时 | runtime/rt_debug.c:66-123 | 未捕获 traceback（`_Unwind_Backtrace` 逐帧查 PC 表） |
| 目标 ABI | src/target_abi.h:57-58、77-78 | `gnu_cfi` 开关、`setjmp_sym`/`longjmp_sym`（Win64 换自带实现） |
| 构建 | src/driver_build.cpp:558 | `cc -c` 汇编；`:1114-1127` `cc -Wl,--eh-frame-hdr` 链接 |

已经具备的表驱动前置条件（实测 `readelf`）：生成程序已有 `.eh_frame`/`.eh_frame_hdr`
（单个程序 789 段 FDE），但**没有** `.gcc_except_table`，也没有 `.cfi_personality`/`.cfi_lsda`；
当前反汇编里 `call _setjmp` 共 55 处。`libgcc_s.so.1` 已随 `cc` 链接，`_Unwind_*` 可直接用。

## 2. 语义对照表

表格列含义：

- 「现在怎么实现」= 代码证据，不是文档转述；
- 「表驱动必须做什么」= 改完后语义不变的最小要求；
- 「风险」= 做错时最可能的出错方式（多数是静默错误）。

### 2.1 try / except / else / finally 主干

| 语言构造 | 现在怎么实现（file:line） | 表驱动方案下必须做什么才能不变 | 风险 |
|---|---|---|---|
| `try:`（带 except） | `HandlerPush` 存槽 → `Setjmp` → `Branch` 到 `try.land`/`try.body`（src/lower_exc.cpp:202-213）；正常体结束 `HandlerLeave` 后跳 else/end（`:223-229`） | 保护区边界用 `.LEHB/.LEHE` 标出；正常路径不再需要 push/leave；landing pad 入口仍需“清理到 try 入口的临时栈水位”（现 `mark` 在 push 时记录，rt_exc.c:293） | 保护区把 `__enter__`/参数求值也包进去 → 不该捕获的异常被捕获；水位取函数入口 → except 体里释放掉外层还活着的临时值 |
| except 顺序匹配 | 每个 handler 一个 `except.next` 块，`pa_exc_matches` 失败继续下一个（src/lower_exc.cpp:239-259）；`ExcMatches` → runtime `exc_is_a` 走内置父类表（rt_exc.c:337-345、95-192） | landing 分发代码可以原样复用；但 **personality 的 phase 1 必须能做“这个 try 里有没有任何 except 可能命中”的判断**（见 4.1 的结论），不能一律 catch-all | personality 一律 `_URC_HANDLER_FOUND` 时，内层 try 的 except 全不匹配后 `_Unwind_Resume`，外层 except 永远不会再被 phase 1 搜索到 → 外层 except 被静默跳过 |
| 无匹配 → 继续向外抛 | 所有 handler 都不匹配（且没有 bare except）时 `ExcReraise`（src/lower_exc.cpp:261-264） | 必须**重新 `_Unwind_RaiseException` 同一个异常对象**（重启 phase 1 继续搜索外层 handler，见 4.1）；不能只用 `_Unwind_Resume`（它不再选外层 handler） | 用 `_Unwind_Resume` → 外层 except 被静默跳过；用新对象重抛 → 异常身份/RC/`__cause__` 归属错 |
| 裸 `except:` | 遇到无类型 handler 置 `bare=true`，直接 `Jump` 到 handler 体；其后的 handler 不再生成匹配（src/lower_exc.cpp:245-259） | 同「except 顺序匹配」，但该 try 区域在 phase 1 一定是命中 | 若把 bare except 也做成 cleanup action（不捕获），phase 1 不选它，异常继续外抛 |
| `except as e`（内置/扁平异常） | 绑定 landing 后 `ExceptHandler` 槽：`ExcAcceptValue` 把 pending 转 current 并产出 `PyAdamasExcValue`（src/lower_exc.cpp:270-285；rt_exc.c:706-719）；`rc_dec(ev)` 归还创建引用（`:283-285`） | `pending→current` 的状态机、`pa_exc_done` 恢复外层上下文必须保留；landing pad 里先 `exc_accept` 再进 handler 体 | 漏掉 accept 的 current 层叠 → except 体里裸 raise 重抛错对象 / `pa_exc_done` 弹错层 |
| `except as e`（自定义富异常类） | `rich_exc_handler` 判定字段/方法（src/lower_exc.cpp:7-16）；用 `ExcAcceptInstance` 取真实例（`:270-275`；rt_exc.c:676-682）；实例在 raise 时存入全局 `current_exc_inst`（rt_exc.c:659-674） | 富实例与扁平控制流异常的绑定关系不变；landing pad 仍按类名匹配再取实例 | 用类型表匹配时把「按名子类匹配」换成「精确类型」→ `except 父类` 抓不到子类富实例 |
| `except (A, B)` 多类型元组 | lowering 把元组展开成多个 `ExcMatches` 的按位或（src/lower_exc.cpp:34-48）；每个元素只接受异常名 | 与单类型同：phase 1 判定要按“或”的语义；元组元素不能再降低成运行期对象 | 每元素都做一次 phase 1 查询 → 复杂度上升；或只认第一个类型 |
| `else:` | 正常路径 `HandlerLeave` 后跳 `try.else`，异常 landing 直接进 handler 分发、不经过 else（src/lower_exc.cpp:207、223-229、339-348） | else 代码不属于保护区，也不属于任何 landing；它只在“try 体正常结束”后执行 | 把 else 并进保护区 → else 里抛的异常被自己的 except 抓住（CPython 不会） |
| 嵌套 try（多层） | 每层各自 push/setjmp/landing，内层 landing 不匹配时 `ExcReraise` 交给外层（src/lower_exc.cpp:195-350 递归；实测 `tests/run/22_exception.py` 的 nested/reraise 段） | LSDA 的 call-site 表天然嵌套；landing pad 位置/action 序号必须按词法内层到外层排 | 多层 landing pad 共用 action 表错位 → 跳错 handler 体 |
| except 体里再抛 | 进入 handler 体前再压一层 `hh` handler + setjmp，landing 先 `HandlerLeave` + `ExcDone` 再 `ExcReraise`（src/lower_exc.cpp:287-331） | 这层“handler 体外的兜底”在表驱动里可退化成 landing pad 内的失败路径；关键是异常逃出 handler 体时必须先 `exc_done` 恢复上下文再继续展开 | 不先 `exc_done` → `current_exc`/`exc_save_top` 泄漏、裸 raise 重抛错对象、后续 `pa_exc_accept` 拿错层 |
| `finally`（正常路径） | `HandlerLeave` → 内联 finalbody → 跳 end（非可挂起函数，src/lower_exc.cpp:381-388） | 正常路径直接 fall-through 到 finalbody，不经过 landing；finalbody 只编译一份（可挂起函数强制一份，`:407-462`） | 正常路径也走 landing → 误把 pending 异常取出来重抛 |
| `finally`（异常路径） | landing `HandlerLeave` → `ExcTakeValue` 把 pending 转成 `excval` 存 `pend_slot` → 内联 finalbody → 非空则 `ExcReraiseValueIf`（src/lower_exc.cpp:390-403；rt_exc.c:684-698） | 非可挂起函数可直接用 unwinder 的异常对象 + `_Unwind_Resume`；可挂起函数仍需要“把待重抛异常搬进帧槽”的等价物，因为 finalbody 里可能 setjmp/挂起 | 把 pending 留在全局而没有帧槽 → finalbody 里一个 `await`/`yield` 后异常上下文被调用方冲掉，原异常丢失 |
| `finally` 里再抛 | finalbody 里 `raise` 覆盖 `pending_exc`（src/lower_exc.cpp 的 finalbody 是普通 `stmts`；rt_exc.c:456 先 `exc_free(pending_exc)`） | 语义：新异常取代原异常继续展开（CPython 另设 `__context__`，见第 4 节）；表驱动里同样要用新异常替换 unwinder 正在处理的异常，或先清理旧对象 | 两个异常对象同时活着 → 双释放/泄漏；或者新异常被旧异常的 `_Unwind_Resume` 覆盖 |
| `finally` 里 `return` | finalbody 是普通语句，`return` 走 `emit_cleanup_exits(0)` 后 `emit_return`（src/lower_stmt.cpp:183-189）；此时本层 Finally 已从 cleanup 栈弹出（src/lower_exc.cpp:378-383、440-442、456-457），不再跑自己 | 早退路径与现状一样内联发射；异常路径 finalbody 终止控制流时 **不得再 resume**，需要“放弃当前展开”的等价动作（现 `terminated()` 判断跳过 `ExcReraiseValueIf`，`:400-403`） | 异常路径 finalbody 里 return 后仍 `_Unwind_Resume` → 已返回的帧继续展开，栈/对象状态错乱（实测 `tests/run/129_finally_jump.py` 的 swallow/after_swallow 依赖“吞掉异常”） |
| `finally` 里 `break`/`continue` | 同上，`emit_cleanup_exits(loop_cleanup_depth)` 只清到循环口的层数（src/lower_stmt.cpp:191-213）；finally 内 break/continue 目标块由当前 `st_->loops` 决定 | 早退内联逻辑原样保留；异常路径 finalbody 里 break/continue 同样要能“吞掉异常”并跳转 | 早退只 `handler_discard` 不跑 finalbody → 清理漏跑；异常路径跳转后 landing 状态残留 |
| try 体 `return` 穿过 finally | 编译期 `emit_cleanup_exits(0)` 依次发射：`HandlerDiscard`（不清理临时值）→ 内联 finalbody（src/lower_stmt_with.cpp:97-110；src/lower_stmt.cpp:187） | 不需要 landing pad；只有“异常穿过”才需要表驱动。当前 finally 内联两份（非可挂起）的语义等于 cleanup landing+fallthrough | 把早退也改成展开器驱动 → return 路径变慢，且返回值临时值的 RC 生命周期被 `rc_cleanup(mark)` 提前释放（`:308-315` 注释就是为这个） |
| try 体 `break`/`continue` 穿过 finally | 同上，`emit_cleanup_exits(st_->loop_cleanup_depth.back())`（src/lower_stmt.cpp:196-213）；Finally cleanup 条目自身也内联 finalbody（src/lower_stmt_with.cpp:97-110） | 同上 | 同上 |

### 2.2 with / async with

| 语言构造 | 现在怎么实现（file:line） | 表驱动方案下必须做什么才能不变 | 风险 |
|---|---|---|---|
| `with` 基本（`__enter__`/`__exit__`） | `__enter__` 在 handler **之前**调用并绑定 as（src/lower_stmt_with.cpp:296-336）；随后 `push_handler`+`setjmp`（`:338-349`）；正常路径 `HandlerLeave` → `__exit__(None,None,None)`（`:362-371`）；异常 landing 先 `HandlerLeave` → `__exit__(type,msg,None)`（`:374-377`） | 保护区必须从 `__enter__` 返回之后才开始（CPython：`__enter__` 抛出的异常不交给本 `with` 的 `__exit__`）；`__exit__` 的异常参数格式是 (str 类型名, str 消息, None)，不是 CPython 的 (类, 实例, traceback)（见 4.7） | 保护区包住 `__enter__` → `__enter__` 失败还调 `__exit__`；`__exit__` 返回真假的判定被做成类型匹配 → 抑制逻辑失效 |
| 多个 context manager（单条 `with a, b:`） | `lower_with_item` 递归：每项一层 handler + cleanup，退出顺序天然逆序（src/lower_stmt_with.cpp:234-237、362） | 每项一个保护区 + 一个 cleanup/handler action；逆序由词法嵌套保证 | 多个项共用一条 action → 退出顺序错/漏调某个 `__exit__` |
| `__exit__` 返回 True 抑制异常 | landing 里 `exc_exit` 返回值直接 `Branch`：真→`ExcClear`+跳 end，假→`ExcReraise`（src/lower_stmt_with.cpp:377-395） | 抑制分支要真正“吃掉”展开：要么在 phase 1 之后停止 unwinder（handler 语义），要么 `_Unwind_DeleteException` 并正常继续；不能 resume | 用 cleanup action 实现 with 时 `__exit__` 返回 True 无法阻止继续展开 → 外层 finally/except 多跑 |
| `__exit__` 自己抛异常 | landing 先 `HandlerLeave`，再调 `__exit__`；`__exit__` 里的 raise 覆盖 pending 并 longjmp 到外层（src/lower_stmt_with.cpp:374-394；rt_exc.c:456） | 新异常取代原异常继续展开；原异常对象按 CPython 应挂到新异常的 `__context__`（当前实现没有，见第 4 节） | 同一异常对象被两个展开流程引用 → 双释放；或在 `__exit__` 抛异常后还继续 `ExcReraise` 旧异常 |
| `with` + `return`/`break`/`continue` | `emit_cleanup_exits` 的 `With` 分支：`HandlerDiscard` → `emit_exit_call(false)`（src/lower_stmt_with.cpp:85-89） | 早退仍走内联发射，不需要 landing；但 handler 记录若在表驱动里不存在，这段要改成“只调 `__exit__`” | 表驱动下还在发 `HandlerDiscard` → 空指针/panic（`handler stack corrupted`，rt_exc.c:302、312） |
| `with open(...) as f` | 特例：识别 `TyKind::File` 后不发 `__exit__`，cleanup 类型是 `FileClose`（src/lower_stmt_with.cpp:245-291，正常 `:274-281`，异常 `:284-289`） | 现状本来就不走 `__exit__`；表驱动只需把 landing 里的 `FileClose` 动作原样保留 | 误以为它走类协议而改成 `__exit__` → open 的文件没有 `__exit__` 槽，编译/运行错 |
| `async with`（普通类协议） | `__aenter__` 先 await（src/lower_stmt_async.cpp:299-321），再 push/setjmp（`:323-335`），cleanup=AsyncWith（`:338-348`）；正常路径 await `__aexit__(None,None,None)`（`:353-361`）；异常 landing 先抄 type/msg、`ExcClear`、await `__aexit__`，真→跳 end，假→用 `Raise(type,msg)` 重抛（`:363-411`） | 可挂起函数里 `with` 的 handler 记录本来就是“挂起时摘、恢复重挂”，表驱动下往返可变简单；但“异常→清空 pending→await→重抛”这个流程必须保留，因为 await 期间全局 pending 会被调用方动到 | 直接跨 await 保留 pending → 调用方/事件循环的 raise 或 clear 冲掉待重抛异常（现状靠 `ExcClear`+重建规避，代价见 4.6） |
| `async with` 内置管理器特例（Lock/Event/Semaphore/Timeout/TaskGroup） | 五个特例各自手写 landing，异常路径都是 `ExcTypeName`+`ExcMessage` 抄成 str、`ExcClear`、释放资源、`Raise(type,msg)`（src/lower_stmt_async.cpp:61-74、127-140、179-192、234-247） | 语义同上；注意这些 landing 不经过 `pa_exc_accept`，只做“搬运后重抛” | 同样丢 args/cause/富实例（见 4.6） |

### 2.3 raise / 异常对象 / 异常链

| 语言构造 | 现在怎么实现（file:line） | 表驱动方案下必须做什么才能不变 | 风险 |
|---|---|---|---|
| `raise ValueError` | `lower_raise` 的 Name 分支取类型名，`call_void(Raise, {type, ""})`（src/lower_exc.cpp:118-124、183）；runtime `pa_raise` 建 `PyAdamasException` 设 `pending_exc` 后 longjmp（rt_exc.c:450-469） | 新实现里 raise 入口只把“展开”从 longjmp 换成 `_Unwind_RaiseException`，`pending_exc` 仍要设（landing 分发依赖它） | 两套异常表示（unwinder 对象 / pending_exc）不同步 → landing 分发看到旧 pending 或 NULL |
| `raise ValueError(msg)` | 位置实参转成 str 后走 `Raise`（src/lower_exc.cpp:157-168、183）；消息就是 `PyAdamasException.message`（rt_exc.c:459-464） | 同上；消息字符串化行为不要顺手改（见 4.3，CPython 保留原值，现状已不一致） | 改表驱动时把 msg 改成 Python 对象 → 与现有 `e.args` / `str(e)` 语义分叉 |
| `raise ValueError(a,b,c)` 多参 | >1 个位置参：逐个字符串化、append 成 list，再 `RaiseArgs`（src/lower_exc.cpp:140-156）；runtime 存 `pending_exc_args`（rt_exc.c:572-593），消息是 `(a, b, c)` join（`:546-569`） | 多参 args 的存活期与 `pending_exc_args` 全局一致：`exc_accept_value` 负责消费/释放（rt_exc.c:715-718） | landing 分发换了入口却没接管 `pending_exc_args` → 泄漏/串到下一个异常 |
| `raise e`（e 是异常值对象） | 静态类型 `TyKind::Exception` → `RaiseValueFrom(v, cause)`（src/lower_exc.cpp:110-113）；runtime `pa_exc_from_value` 把 ExcValue 还原成扁平异常（含多参 args，rt_exc.c:633-645） | ExcValue 与扁平异常互转逻辑不变；`raise e` 是复制语义而不是搬同一个对象（CPython 也不是同一对象） | 试图在新方案里“直接抛 ExcValue 指针” → `pa_exc_matches` 只认 type 字符串，匹配失效 |
| 裸 `raise` | `Reraise`（src/lower_exc.cpp:70-78）；runtime `pa_reraise`：`current_exc` 搬到 `pending_exc` 后 longjmp（rt_exc.c:471-481）；没有 current 则 RuntimeError（`:478-480`） | `current_exc` / `exc_save_top` 的层叠语义必须保留（`ExcAccept`/`ExcDone` 维护，rt_exc.c:347-369） | 表驱动 landing 直接重抛 unwinder 对象而绕过 `current_exc` → 裸 raise 在 except 体内抛的是外层异常或空 |
| `raise X from Y` | cause 先求值（src/lower_exc.cpp:79-90）；X 不是异常值对象时先 `ExcValueNew` 造值再 `RaiseValueFrom(v,cause)`（`:174-182`）；runtime 把 cause 递归复制到 `e->cause`（rt_exc.c:647-655、629） | `__cause__` 只保存扁平异常副本的链条；`from` 与隐式 `__context__` 是两回事（现状只有前者） | 把 cause 链与 unwinder 的上一帧异常混在一起 → 链错/环 |
| `raise X from None` | cause 求值为 int 0（src/lower_exc.cpp:81-83）；`pa_raise_value_from` 只在 cause 非 NULL 时设（rt_exc.c:650-653） | 语义是没有显式 cause，同时应压制隐式上下文；现状只做到前者（见 4.2） | 把 None 当成显式 cause=None 对象 → `__cause__` 判定出错 |
| 隐式链 `__context__` | **未实现**：`pa_raise` 不读 `current_exc`，新异常 `cause=NULL`（rt_exc.c:450-469）；except 体里再抛不会挂上下文 | 表驱动没有必要顺手补；若补，应在 raise 入口设 context 指针，并保证跨 resume 的异常上下文节点栈同步（rt_exc.c:371-433） | 误以为 unwinder 会替你串链 → CPython 语义缺失被当成改造引入的回归 |
| `except ... as e; e.__cause__` | ExcValue 的 `cause` 字段（runtime/rt_internal.h:361-370）；属性在 lowering 里映射到 `ExcValueCause`（src/lower_expr_assign.cpp:501-507；src/typecheck_expr.cpp:329-330） | 属性到运行时函数的映射是一张白名单，新方案不要动 | 类型表若把 `__cause__` 当普通字段 → 读出垃圾 |
| `e.args` | `pa_exc_value_args` 返回 `v->args`（runtime/rt_exc.c:721-725），静态类型 `tuple[str]`（src/typecheck_expr.cpp:328）；元素是字符串化后的值（rt_exc.c:614-628） | 现状 args 元素永远是 str；表驱动不改变异常对象构造路径即可 | 改异常对象存储后 args 的所有权（`exc_accept_value` 里 `pa_rc_dec(pending_exc_args)`，rt_exc.c:715-718）容易双释放 |
| `e.__traceback__` | **没有该属性**：lowering 对“内置异常其它属性”一律 `retype(ev, unknown)`（src/lower_expr_assign.cpp:508-509）；实测 `print(e.__traceback__)` 打印一个整数指针（99498760223040），既不是 None 也不是对象 | 表驱动需要自己的 traceback 表示；Z2/Z3 不补的话，必须保证 landing pad 分发不依赖这个属性 | 静默错误：任何 `e.__traceback__ is None` 判断都得到 False（见 4.4） |
| `sys.exc_info()` | **不存在**：`sys` 模块没有该属性（实测 check 报 `module sys has no attribute exc_info`）；src/runtime 全文 grep 无 `exc_info` | 表驱动不引入；若要补，入口是 `current_exc`/`pending_exc`（rt_exc.c:11-12），并考虑挂起时置空 | 生成器/协程挂起后 exc_info 应为空（CPython 语义），补的时候要用 `pa_exc_enter/leave` 的上下文栈 |
| 自定义异常类（pass-only） | 类定义时 `ExcRegister(name, parent)`（src/lower_class.cpp:191-201）；父子识别在 typecheck（src/typecheck_func.cpp:356-369、424）；runtime 注册表 + 内置父类表（rt_exc.c:72-93、95-184） | 注册时机（模块初始化时）与顺序不变；跨模块按名匹配依赖它 | 表驱动若改用类型表指针，注册表与 TType 表不一致 → 跨模块子类匹配退化 |
| 自定义异常类（富异常，有字段/方法） | `is_rich_exception`（src/typecheck_func.cpp:950-955）；raise 时 `RaiseInstance(inst,type)` 存 `current_exc_inst`（src/lower_exc.cpp:100-108；rt_exc.c:659-674），控制流仍是扁平异常；`except Rich as e` 用 `ExcAcceptInstance` 取回（rt_exc.c:676-682） | 富实例生命周期挂在 `pa_exc_done`（rt_exc.c:359-369）与 `current_exc_inst` 引用上；landing 分发不改变顺序 | 表驱动只搬 pending 不搬 `current_exc_inst` → 富实例泄漏/串到下一个异常 |
| 继承匹配（内置 + 自定义多级） | `exc_is_a` 沿 parent 链走（rt_exc.c:186-192）；内置表 98-179；实测 `SubError→MyError→ValueError` 与 `KeyError→LookupError→Exception` 都能命中（tests/run/34_exception_chain.py、22_exception.py） | 匹配逻辑仍是按名字的字符串表；类型表若引入 TType，只能作 phase 1 的快速过滤，最终语义要能落回 `exc_is_a` | 类型表只放精确类 → 父类 except 静默不命中 |
| `assert cond, msg` | lowering 直接 `Raise(AssertionError, msg)`（src/lower_exc.cpp:465-493） | 无需变化 | 无 |
| 运行时错误入口（IndexError/KeyError/ZeroDivisionError/OSError…） | 运行时 `pa_raise_c(type,msg)`（rt_exc.c:489-493），消息固定 C 字符串；能否捕获取决于外层 handler | 表驱动 raise 入口要和 `pa_raise` 共用一条路径（否则运行时错误绕过 pending_exc 直接 abort） | 只改编译器发起的 raise，忘了运行时 `pa_raise_c` → 运行时错误在无 setjmp 世界里变成 terminate |
| 异常值当普通值用（`e = ValueError("x")`） | 构造器识别为异常类型时走 `ExcValueNew`/`NewArgs`（src/lower_call.cpp:419-425）；对象是 RC 堆对象（rt_exc.c:498-543） | ExcValue 的 RC/GC 生命周期与 landing 无关，保持即可 | 把控制流异常对象换成 ExcValue 当 pending → landing 分发与异常值语义混淆 |

### 2.4 生成器 / 协程 / 异步生成器

| 语言构造 | 现在怎么实现（file:line） | 表驱动方案下必须做什么才能不变 | 风险 |
|---|---|---|---|
| 生成器里有 `try`（yield 在 try 里） | 挂起前把 `st_->cleanups` 里的 handler 记录从内到外 `HandlerSuspend`（src/lower_gen.cpp:83-94）；状态机 `GenYield` 写 out/state 后返回（`:102-106`；发射 src/codegen_ops.cpp:168-175） | 表驱动下 handler 栈不存在，摘/挂本身可以删；但 resume 入口必须保留 `pa_exc_enter/leave` 的异常上下文换进换出（src/lower_gen.cpp:77、328） | 只删 handler 摘挂、忘了异常上下文切换 → except 体里挂起后裸 raise 重抛错对象 |
| resume 时重挂 handler + 重新 setjmp | 正常恢复：按 state 找 `gen_state.cleanups`，逐个 `HandlerRearm` + `Setjmp`，非 0 跳原 landing（src/lower_gen_async.cpp:430-478）；throw 注入：同样重挂后 `GenRethrow`（`:484-544`）；runtime `pa_handler_rearm` 重新记录 `mark/frame`（rt_exc.c:326-332） | 表驱动没有 jmp_buf 可重捕；若 LSDA 是词法的，进入 resume 时 PC 落在保护区里，展开器自然能命中，**不需要 rearm**；但 throw 注入分发仍要保留（GenRethrow 是在恢复帧里抛） | 半改状态：删了 rearm 但 throw 分发还在重挂 → `handler stack corrupted` panic（rt_exc.c:322）；或恢复后异常落点跳到旧 resume 帧的 landing pad |
| 协程里 `try` + `await` | `suspend_on_awaitable`：`exc_leave` → `cleanup_gen_mark` → 挂起 handler → `GenAwait`/`GenSent`（src/lower_gen.cpp:320-368；发射 src/codegen_ops.cpp:176-184） | 同生成器；await 注入异常走同一 throw 分发（`:484-544`） | 同生成器 |
| except 体里挂起（异常上下文跨 resume） | `pa_exc_enter` 把调用方的 `current_exc`/`exc_save_top` 压节点栈，装本帧保存的；`pa_exc_leave` 写回并换回（rt_exc.c:385-433）；生成代码在 resume 入口/出口调用（src/lower_gen_async.cpp:396-397；src/lower_gen.cpp:37-47、77、328） | 这套上下文交换与展开机制正交，必须原样保留；表驱动只换 handler 记录这一层 | 忘掉 `pa_exc_unwind`（异常逃出 resume 时退节点栈，rt_exc.c:424-433；rt_gen.c:91、rt_async.c:759、rt_agen.c:170）→ except 状态串到调用方 |
| finally 体内挂起 | 可挂起函数的 finally 只编译一份：正常/异常都跳 `fin`；异常路径先 `ExcTakeValue` 存帧槽，finalbody 后再 `ExcReraiseValueIf`（src/lower_exc.cpp:407-462） | 表驱动里“异常对象由 unwinder 持有”与“跨挂起要存进帧槽”冲突：可挂起函数的 landing 仍必须把待重抛异常复制成帧槽可持有的对象（等价 `ExcTakeValue`），再在结束时重抛 | 直接持有 unwinder 的 `_Unwind_Exception*` 跨 yield → 展开器内部状态跨 C 栈帧失效，恢复后崩溃 |
| close() 重放 finally / `__exit__` | 每个挂起点存 `gen_state.cleanups` 快照；close 分发按快照逆序跑 `__exit__`/finalbody（src/lower_gen_async.cpp:315-387），最后 `HandlerFree` 释放摘下的记录（`:366-369`）；runtime `pa_gen_close`/`finalize` 有 C 级 setjmp 兜底（rt_gen.c:211-240、244-267） | 表驱动下 close 不再需要释放 handler 记录，但**重放清理**本身要原样保留；`pa_gen_close/finalize` 的 C 级 setjmp 必须换成 resume 函数级 catch-all landing pad（见 5.2） | 只删 `HandlerFree` 忘了重放逻辑 → close 不跑 finally；或保留 C 级 setjmp 但 raise 已改成 _Unwind → C setjmp 抓不到，进程 terminate |
| close 时又挂起 | `reject_suspend_in_close` 生成 `Raise(RuntimeError, "generator ignored GeneratorExit")`（src/lower_gen.cpp:63-70）；close 重放深度 `close_replay_depth_`（src/lower_gen_async.cpp:357-359） | 语义是 close 期间禁止真正挂起，与展开机制无关，保留 | 表驱动改造后 close 重放走 landing pad 而不是直接跳块 → 挂起点判断失效 |
| `gen.throw(exc)` | runtime 把 ExcValue 转成扁平异常塞帧槽 p[8]（rt_gen.c:165-178），驱动 resume；生成代码 throw 分发重挂 handler 后 `GenRethrow`（src/lower_gen_async.cpp:484-544）→ `pa_coro_rethrow` → longjmp（src/codegen_ops.cpp:185-189；rt_async.c:791-799） | 表驱动下 `GenRethrow` 要改成在当前 resume 帧内发起 `_Unwind_RaiseException`；已结束/未启动生成器的分支语义（rt_gen.c:166-168、540-543）不变 | GenRethrow 还调 `exc_longjmp` 而代码已无 handler → 跳到 NULL/panic |
| `yield from` 的 throw 转发 | 挂起点位于委托中时注入异常不重抛，写 `delegate_exc_slot` 交回委托循环；`GenThrowOpt` 驱动子生成器（src/lower_gen.cpp:204-227；rt_gen.c:183-207） | 转发语义与展开机制正交，保留 | 表驱动改 GenRethrow 时误删 delegate 分支判断（src/lower_gen_async.cpp:521-531）→ 异常不再转发给子生成器 |
| 异步生成器 `athrow` / `aclose` / `__anext__` | 驱动对象 `pa_asyncgen_anext/asend/aclose/athrow`（runtime/rt_agen.c:6-47）→ `agen_drive`（`:75-...`），C 级 setjmp 兜底（`:152-179`） | 与同步生成器同一套结论：C 级 setjmp 要么保留在“只由 C 驱动”的层面（raise 已是 _Unwind 时不成立），要么改成 resume 级 landing pad | 同 close/gen_run 的风险 |
| 生成器/协程异常逃出：释放帧槽 + 挂逻辑帧 | `gen_run` 的 C 级 setjmp 捕获后 `pa_gen_release_slots` + `pa_rc_cleanup` + `pa_exc_unwind` + `pa_trace_attach`，再重抛（rt_gen.c:74-94）；协程 `coro_step` 同构（rt_async.c:737-761）；异步生成器 `agen_drive` 同构（rt_agen.c:152-179） | 这三处是“异常逃出 resume 帧”的唯一落点，Z4 必须逐个换成表驱动版本（否则 _Unwind 会穿过它们，进程 abort） | 漏改一处：该对象里的异常逃出时直接 terminate（`_Unwind_RaiseException` 无 handler 的默认行为），不是 traceback |
| `async for` 每轮 try 保护区 | 每轮 `HandlerPush`+`Setjmp` 保护 `__anext__`+await，landing 只捕获 `StopAsyncIteration`、其余重抛（src/lower_gen_async.cpp:72-196，尤其 `:88-107`、`:148-170`） | 这个词法保护区在表驱动里可用 LSDA；但循环体每轮进入保护区**不应再有运行时动作**（这正是零开销收益） | 把 StopAsyncIteration 判定做成 phase 1 类型表 → 类型表要能表达 `StopAsyncIteration`；否则每轮都进 landing pad 再 resume，性能退化 |
| 异步推导式里的 async for | 同上，手写 push/setjmp/landing，landing 只接 StopAsyncIteration（src/lower_comp.cpp:145-253，`:166-181`、`:228-251`） | 同上 | 同上 |
| 取消（cancel）跨 await | 取消注入协程帧槽 p[8] 后走 resume throw 分发（runtime/rt_async.c:681-730 coro_inject；`:930-951` cancel_suspended；生成侧 src/lower_gen_async.cpp:484-544） | 注入分发保留；嵌入的 try 保护区靠 LSDA，取消异常会在 resume 帧内展开到最近 handler | 取消变成 abort 而不是被 finally 处理 |

### 2.5 异常穿过边界

| 语言构造 | 现在怎么实现（file:line） | 表驱动方案下必须做什么才能不变 | 风险 |
|---|---|---|---|
| 穿过普通函数帧 | 生成函数序言压帧记录（src/codegen_block.cpp:293-323，槽地址低两位标记，runtime/rt_internal.h:315-329）；`exc_longjmp` 先 `pa_frame_unwind(handler_top->frame)` 逐帧 dec 槽（rt_exc.c:204-214、20-38）；正常返回由尾声弹帧（src/codegen_regalloc.cpp:34-39） | 表驱动下被跳过的帧由 personality phase 2 逐帧经过；每个需要释放槽的函数要有 cleanup landing pad（或在 personality 里逐帧释放），最终调用现有 `pa_frame_unwind` 的等价物（见 5.1） | 只跳 landing pad 不释放中间帧 → 每次异常泄漏这些帧的堆引用；`pa_frame_top` 不恢复到正确位置 → 后续帧链错乱 |
| 穿过模块边界（import 的模块函数） | 所有模块编进同一可执行文件、链同一份运行时（src/driver_build.cpp:1118-1121）；模块 init 桩是手写汇编 `jmp __pa_main`（src/codegen.cpp:403-425），普通函数都有帧记录 | 模块边界不是特殊边界：与普通调用一致；模块 init 的尾跳桩没有自己的帧，展开器按 CFI 直接走上一层 | 在模块 init 里抛的异常若被 init 桩截住（没有 LSDA）不受影响；但若给 init 桩误加 personality 会破坏尾跳 CFI |
| 穿过 cimport / C 原生调用 | `CallExtern` 直接 call C 函数（src/ir_ops.h:1105）；C 回调经跳板 `jmp *(%r10)`（src/codegen.cpp:372-401，仅 CFI）。当前若回调里 raise，longjmp 会跨过 C 栈帧 | 表驱动反而更安全：C 帧没有 LSDA，personality 不处理，展开器直接跳过；回调跳板 CFI 已就绪 | 现状下跨 C 帧 longjmp 是 UB（C 函数若有锁/临时缓冲会泄漏）；现有测试完全没有覆盖（带 raise/try 的 cimport、回调用例为 0） |
| 未捕获异常 | `exc_longjmp` 发现 `handler_top==NULL` → `exc_print_uncaught`（先 `_Unwind_Backtrace` 打印函数级 traceback，再打印 `pyadamas: uncaught X: msg`）+ `exit(1)`（rt_exc.c:194-214；traceback runtime/rt_debug.c:92-104） | `_Unwind_RaiseException` 返回 `_URC_END_OF_STACK` 时必须复刻同一行为；不能落到 C++ 的 `std::terminate` | 未捕获异常从“打印 traceback + exit 1”变成 terminate/abort，tests/run/153_traceback.py 直接失败 |
| 逻辑帧（生成器/协程）traceback | 异常逃出 resume 时 `pa_trace_attach` 把 resume 代码地址/函数名挂到异常链表（rt_gen.c:92；rt_async.c:750；rt_agen.c:171；打印 runtime/rt_debug.c:108-123） | 表驱动下逃出点变为 landing pad，`pa_trace_attach` 调用时机要跟着挪 | 少 attach 一层 → traceback 少生成器帧（tests/run/153_traceback.py 的 `in gen` 断言失败） |

### 2.6 优化 / 寄存器 / RC / 其它耦合

| 语言构造 | 现在怎么实现（file:line） | 表驱动方案下必须做什么才能不变 | 风险 |
|---|---|---|---|
| callee-saved 寄存器回退 | 含 handler 的函数对“保护区内写过、之后还要读”的槽禁止提升到寄存器（src/codegen_regalloc.cpp:267-315；判定扫描 HandlerPush..HandlerLeave，`:276-297`） | `_Unwind` 跳 landing pad 时同样会把 callee-saved 恢复成函数入口值：**hazard 判定必须保留**，不能因为“没有 setjmp 了”就放开寄存器提升 | 静默算错：循环里 try 内写的变量回到旧值（tests/run/100_handler_loop_carried.py 就是为此写的） |
| 含 handler 的函数关闭变量提升/相关优化 | `has_handlers` 置位（src/lower_stmt_with.cpp:7）；LICM 等跳过（src/opt_loop.cpp:639、684、909）；inline 拒绝（src/opt_inline.cpp:581；src/lower.cpp:480）；序言不清零槽（src/codegen_block.cpp:258） | 表驱动后可以逐渐放开，但 Z2/Z3 期间建议保持 `has_handlers` 语义不变，避免把优化差异混进语义回归 | 放开优化与异常语义改造同时做 → 出问题无法区分是哪边引入 |
| 帧记录同时是 GC 影子根 | 序言登记 owned 槽与 scan 槽（src/codegen_block.cpp:162-193、293-323）；slot 低两位标记（unknown / 仅 GC）（runtime/rt_internal.h:315-329） | 帧记录结构不能动；新 landing pad 释放槽时要走同一个函数（否则 GC 扫描到已释放引用） | 自己写释放逻辑漏掉 unknown 注册表判断 → 整数被当指针 dec，崩溃 |
| 临时值栈水位 | `HandlerPush` 时记录 `mark=pa_rc_mark()`（rt_exc.c:293）；`HandlerLeave` 里 `pa_rc_cleanup(mark)`（`:305`）；早退用 `HandlerDiscard` 只弹记录不清临时值（`:309-315`，原因见 src/lower_stmt_with.cpp:308-315 注释） | try 保护区需要自己的水位：landing pad 入口把全局临时栈清到“进入 try 时”的水位（不能清到函数入口，否则 except 体内还要用的临时值被释放） | 水位算错 → 双释放/提前释放；这正是 `ExcAcceptValue` 之后 `rc_dec(ev)` 不能留在临时栈上的原因（src/lower_exc.cpp:283-285） |
| handler 记录分配 | malloc + freelist（rt_exc.c:216-225、283-306、335）；嵌套深度决定链表长度 | 表驱动不需要记录，可整体删除；但删的时候要连带删 `HandlerPush/Leave/Discard/Suspend/Rearm/Free` 六个 RtFn 与相关 IR（Z5 才删） | 删早/删漏：仍有 lowering 路径发 `HandlerPush` 打到 dangling 符号 |
| 异常值对象 RC/GC | `PyAdamasExcValue` 是 RC 对象，`exc_value_destroy` 释放 type/message/args/cause（rt_exc.c:498-510）；args 元素是 str（`:524-527`） | 与控制流异常（malloc、不参与 RC）是两套生命周期；表驱动只碰后者 | 把 ExcValue 塞进 `_Unwind_Exception.private` 并让 unwinder 持有 → 引用计数与 unwinder 生命周期打架 |
| traceback 数据 | 未捕获时 `_Unwind_Backtrace` 查 codegen 登记的 PC→(file,func,line) 表（runtime/rt_debug.c:66-104；表由 codegen 发出，src/codegen.cpp:357-370） | 表驱动不改 traceback 数据面；但 landing pad 自身 PC 也会被回溯扫到，要确认 debug 表里没有 landing pad 条目（否则 traceback 多一行假帧） | traceback 输出与基线不一致（tests/run/153_traceback.py 依赖字符串） |
| 内联/优化遍对异常位置的处理 | 内联体归到调用者（docs/codegen.md:578-584）；优化遍复制指令保留行号 | 表驱动需要保护区标签在优化后仍然正确；优化遍目前按块/指令搬运，没有 EH 标签概念 | 优化复制/删除带保护区标签的指令 → LSDA 覆盖错 PC 范围，异常落错或漏清理 |

## 3. 改造边界：哪些保留、哪些替换

| 部分 | Z2/Z3 处理方式 |
|---|---|
| lowering 的块结构（try.body/try.land/except.*、finally.*、with.*） | **保留**。landing 块只是入口从 `Branch(setjmp)` 变成 landing pad 标签；块内分发代码（`pa_exc_matches`、`ExcAcceptValue`、`ExcDone`、`ExcClear`、`ExcReraise`）尽量不动 |
| `HandlerPush/HandlerLeave/HandlerDiscard` | 由“保护区 begin/end + landing pad”取代；Z2 只做单层 catch-all try，其它构造继续走 setjmp |
| `Setjmp` IR 操作与 `gen_setjmp` | Z5 才删；Z2 新路径不再发射 |
| 帧记录 / GC 影子根 / 槽标记 | **保留**，供新的逐帧释放 helper 复用 |
| 异常对象、`pending_exc`/`current_exc`、`ExcValue`、匹配表、`exc_enter/leave` | **保留**，是 landing 分发的语义基础 |
| 生成器/协程的挂起/恢复、close 重放、C 级 setjmp 兜底 | Z4 之前**保持 setjmp 路径**；Z2/Z3 必须用 `has_handlers && !is_generator` 之类的门把可挂起函数排除在新路径外（见 5.2） |
| Win64 | 维持 SEH/自带 setjmp；新路径用 `target_abi().gnu_cfi`/新增 `eh_model` 字段 gate 住（src/target_abi.h:57-58） |

## 4. 已知不一致与静默出错点（按危险程度排序）

每条都给出代码证据；标「实测」的可用文档里的探针复现（探针都是几行小程序，直接
`build/dev/pyadamas run --rebuild -o /tmp/x /tmp/x.py` 即可）。

### 4.1 【改造陷阱，最危险】catch-all + landing pad 分发会漏掉外层 except

现状不存在这个问题：`try` 的 setjmp 点只属于一个 `try`，不匹配就 `ExcReraise` 重新 longjmp，
外层 handler 的记录还在栈上（src/lower_exc.cpp:261-264；rt_exc.c:483-486）。
但 Itanium ABI 的 phase 1 只搜索一次：如果 personality 对所有 try 区域都回答
`_URC_HANDLER_FOUND`（文档 Z3 的“landing pad 里再跑 `pa_exc_matches`”方案），
phase 1 会停在**最内层** try；该 try 的 except 全不匹配时调用 `_Unwind_Resume`
只会继续 phase 2，**不会重新搜索外层 except**，外层 handler 永远不执行。

结论（必须写进 Z3 设计）：

1. personality 的 phase 1 必须能判断“本帧有没有可能接住”，即把
   `pa_exc_matches` 的按名继承匹配（rt_exc.c:337-345、186-192）搬进 phase 1；或者
2. personality 一律 catch-all，但 landing pad 不匹配时**重新 `_Unwind_RaiseException`
   同一个异常对象**（等价 C++ `__cxa_rethrow`，重启 phase 1 向外搜索），并且该
   landing pad 的 PC 必须落在自己保护区之外，否则会再次选中自己。

方案 2 的代价：每次“不匹配”都要多一轮 phase 1 扫描，嵌套 try 最坏 O(depth²)；
方案 1 的代价：类型表（TType）不能为空，类型条目要能表达“异常名 + 继承关系”。
两者都比“只做 cleanup action + landing 分发”复杂，但后者在多层 try 上是错的。
建议 Z3 采用 1 + 2 的混合：phase 1 用名字表做快速过滤，landing pad 分发仍是权威，
分发失败用方案 2 兜底（正确性不受 phase 1 过滤错误影响）。

### 4.2 【不一致】没有隐式异常链 `__context__`

CPython：except 体里抛出的新异常自动把当前处理中的异常放进 `__context__`。
现状：`pa_raise` 只 `exc_free(pending_exc); pending_exc = e;`，`e->cause=NULL`，
不读 `current_exc`（rt_exc.c:450-469）；`pa_reraise` 也只处理 current→pending（`:471-481`）。
实测：`try: try: raise ValueError("inner") except ValueError: raise TypeError("outer")`
得到 `e.__cause__ == None`（CPython 该处 `__cause__` 也是 None，但 `__context__` 是 inner）；
而 `e.__context__` 在 PyAdamas 里走“未知属性返回异常值本身”的路径，打印出一个整数指针
（实测 96968646883888），不是异常对象。
改造影响：表驱动不会自动补上 `__context__`；如果外层 finally 在展开时又抛，
两个异常的关系仍无法表达。验收时不要把它算成 Z2/Z3 回归，但要在兼容性文档里登记。

### 4.3 【不一致】异常 args 全部字符串化；与 int 比较会 segfault

CPython：`raise ValueError(42)` 之后 `e.args == (42,)`，元素保留 int。
现状：单参走 `convert_to(raw, mt, t_str())`（src/lower_exc.cpp:157-168），
多参每个都转 str（`:140-156`），runtime 只存 message 与 str 版 args（rt_exc.c:546-569、
614-631，args 元素是 `str_new`）。静态类型固定为 `tuple[str]`（src/typecheck_expr.cpp:328）。
实测：`raise ValueError(42)` 后 `e.args[0] == 42` 段错误（SIGSEGV，status 139）；
`e.args[0] == "42"` 直接比较为 True，但先赋给变量再比较得到 False（原因未查清，疑似临时值
静态类型退化成 unknown 走了机器字比较）。这是现状的真实 bug，表驱动改造不负责修，但基线子集没有这种
写法，所以改造前后都不会被 `sem_baseline.sh` 发现——需要单独立项。

### 4.4 【不一致】没有 `__traceback__`；未知属性静默返回异常对象指针

CPython：异常对象有 `__traceback__`（可为 None）。
现状：runtime 没有该字段（PyAdamasException 只有 type/message/cause/trace，
runtime/rt_internal.h:348-354）；lowering 对内置异常的未知属性一律
`retype(ev, t_unknown())` 放行（src/lower_expr_assign.cpp:508-509）。
实测：`print(e.__traceback__)` 打印 `99498760223040`（异常对象指针按整数打印）；
`e.nosuchattr` 同样打印 `107846690941248`。**静默错误**：任何属性探测都得到“真值”。
改造影响：landing pad 分发或调试输出别写 `e.__traceback__`；若未来把 traceback 存进
`_Unwind_Exception` 私有区，必须同时把这里的 fallback 关掉。

### 4.5 【不一致】`sys.exc_info()` 不存在

实测：`sys.exc_info()` 在 check 阶段报 `module sys has no attribute exc_info`。
runtime 全文无 `exc_info`；内部等价物是全局 `current_exc`（rt_exc.c:12、347-369）。
改造影响：无（Z2/Z3 不引入），但用 `sys.exc_info()` 的 CPython 代码无法编译。

### 4.6 【不一致】async with / async for 的异常重抛丢失 args / cause / 富实例

`async with` 内置管理器（Lock/Event/Semaphore/Timeout/TaskGroup）与普通类协议的异常 landing
都是“`ExcTypeName` + `ExcMessage` 抄成 str → `ExcClear` → 等 `__aexit__`/释放资源 →
`Raise(type, msg)` 重建”（src/lower_stmt_async.cpp:61-74、127-140、179-192、234-247、
363-411；`async for` 类似地按名字处理 StopAsyncIteration，src/lower_gen_async.cpp:148-170）。
`Raise` 只带类型名和消息（rt_exc.c:450-469），所以：多参 args 丢失、`__cause__` 丢失、
富异常实例 `current_exc_inst` 丢失。CPython 中 `__aexit__` 拿到的是原异常对象。
改造影响：表驱动不会改善它；若 Z4 把 async with landing 换成表驱动，反而更容易保留原对象
（不再需要 ExcClear+Raise），但要小心跨 await 的 pending 生命周期。

### 4.7 【不一致】`__exit__` / `__aexit__` 的异常参数是字符串，不是异常类/实例

现状参数：`(ExcTypeName() → str, ExcMessage() → str, None)`（src/lower_stmt_with.cpp:42-55；
src/lower_stmt_async.cpp:363-382 同样）。实测 `__exit__(t,m,tb)` 打印 `exit-kind ValueError boom 0`。
CPython 是 `(ValueError, ValueError("boom"), traceback)`。文档也登记了这一点
（docs/compatibility.md:19-27）。改造影响：`__exit__` 里写 `t is ValueError`、`m.args`、
`tb is not None` 的代码语义不同；表驱动不要顺手把这里改成实例，否则基线里
tests/run/22_exception.py、20_with.py 的期望输出会变（它们按字符串写的）。

### 4.8 【不一致】跨模块自定义异常类不能出现在 `except` / `raise` 的名字位置

`lower_exc_match` 只认内置异常名或**本模块** `classes_` 里的异常类（src/lower_exc.cpp:22-32）；
`lower_raise` 的 `is_exc_name` 同样（`:91-95`）。导入类存在 `imported_classes_` 里
（src/lower_import.cpp:466、487），但 `lower_exc.cpp` 从不查它。
实测（lib.py 定义 `class LibError(ValueError)`，main.py `from lib import LibError`）：
`except LibError as e:` 与 `except lib.LibError as e:` 都在 codegen 阶段报
`except requires an exception name`；main.py 里 `raise LibError(...)` 报同类错误。
但**跨模块按名匹配是好的**：lib.py 里 `raise ValueError(...)`、`raise LibError(...)`
能在 main.py 的 `except ValueError` 里被接住（实测 caught / rich-as-valueerror）。
改造影响：表驱动若改成类型表，必须先解决“跨模块异常类名解析”，否则会出现
“旧路径能按父类 catch、新路径连编译都不过”的分裂；Z2/Z3 不要动这条路径。

### 4.9 【不一致】生成器里逃出的 `StopIteration` 不转 `RuntimeError`（PEP 479）

CPython 3.7+：`raise StopIteration` 从生成器逃出会转成 `RuntimeError: generator raised StopIteration`。
现状：`gen_run` 的 C 级 setjmp 把异常原样交给调用方（rt_gen.c:74-94），没有 PEP 479 转换；
`pa_gen_next` 只在 `done` 时抛 StopIteration（rt_gen.c:109-114）。
实测：`def g(): yield 1; raise StopIteration("x")`，for 循环外的 `except StopIteration:`
能接住 → 打印 `leaked-stop`（CPython 会以 RuntimeError 退出）。
改造影响：表驱动下 `gen_run` 的 setjmp 兜底要换成 landing pad（见 5.2），
替换时很容易把 escape 当作正常结束，需要在 landing pad 里显式做 PEP 479 转换，
否则 for 循环会静默提前结束。

### 4.10 【不一致】except 变量是词法作用域，不是运行时删除

CPython：`except E as e:` 结束后 `del e`，之后再读是 NameError（运行期）。
现状：`e` 是 except 体的临时槽，`comp_slots_` 在体末弹出（src/lower_exc.cpp:277-285、315）；
实测 except 之后 `print(e)` 直接编译错误 `undefined name e`。行为方向一致（代码不可用）
但错误时机不同：条件分支里使用 `e` 的合法 CPython 代码在 PyAdamas 上无法编译。

### 4.11 【不一致】`isinstance(e, ValueError)` 不支持

实测：`except ValueError as e: isinstance(e, ValueError)` 报
`isinstance() second argument must be a class or builtin type`。
异常类名不是一等类型对象；但 `except` 按名匹配是支持的（rt_exc.c:95-192）。
改造影响：任何把 except 匹配下沉到运行期类型对象的方案都要处理“异常类不是 value”这个事实。

### 4.12 【现状风险】C 回调里 raise 会 longjmp 跨过 C 栈帧

`CallExtern` 直接 call C 函数（src/ir_ops.h:1105），C 回调跳板只有 `jmp *(%r10)`
（src/codegen.cpp:372-401）。如果 C 回调（qsort 比较器、第三方库回调）里 raise，
`exc_longjmp` 会跨过库的 C 栈帧，C 侧的锁/临时缓冲不会清理。现状测试完全没有覆盖
（grep tests/run 的 cimport/回调用例里 raise/try 数量为 0）。表驱动在这里是净收益：
C 帧没有 LSDA，unwinder 直接跳过，不会执行错误的清理。

### 危险程度速查

| 风险 | 现在会不会错 | 改造后会不会静默错 | 基线能否发现 |
|---|---|---|---|
| 4.1 phase 1 选错帧 | 不会 | **会**（外层 except 被跳过） | 能（22/34 等嵌套用例） |
| 4.2 无 __context__ | 与 CPython 不一致 | 继续不一致 | 不能（基线没有 __context__ 用例） |
| 4.3 args 字符串化 / 段错误 | 与 CPython 不一致 + 崩溃 | 与改造无关 | 不能（基线没有该写法） |
| 4.4 __traceback__ 垃圾指针 | 静默错误 | 若 landing 用到会放大 | 不能 |
| 4.6 async with 重抛丢 args | 静默丢信息 | 可能被 Z4 放大/改善 | 部分（32/34 覆盖 __cause__，不覆盖 async with 的 args） |
| 4.8 跨模块异常类名 | 编译错误 | 新路径可能变成两套规则 | 不能（基线没有跨模块异常用例） |
| 4.9 PEP 479 | 静默错误 | landing pad 替换时可能继续错 | 不能 |
| 寄存器 hazard（2.6） | 已用分析规避 | **会**（若放开寄存器提升） | 能（100_handler_loop_carried） |
| 中间帧槽不释放（5.1） | 不会 | **会**（RC 泄漏） | 部分（92_exc_inline_rc 放大泄漏，但不检查内存用量） |

## 5. 三个难点的具体结论

### 5.1 (a) 帧局部资源释放：谁在什么时候做？能不能直接复用？

现状机制：每个生成函数在序言把“槽地址数组 + 帧链指针”压进栈帧，注册到全局
`pa_frame_top`（src/codegen_block.cpp:293-323；帧结构 runtime/rt_internal.h:315-329）；
异常 longjmp 前 `pa_frame_unwind(handler_top->frame)` 从帧栈顶往下逐帧
“解引用槽地址 → 非空则清零 → `pa_rc_dec` / `pa_rc_dec_unknown`”，最后把
`pa_frame_top` 设回目标帧（rt_exc.c:20-38）。正常返回时尾声只做 `pa_frame_top = rec->prev`，
槽释放走函数自己的出口（src/codegen_regalloc.cpp:34-39）。

表驱动下这件事**没有任何机制替你做**：unwinder phase 2 只会调用 personality 和 landing pad，
不会碰 `pa_frame_top`。如果只实现 landing pad 而不释放中间帧，异常每穿过一个函数就漏掉
该函数全部堆槽引用（循环里跑异常 = 无界泄漏；tests/run/92_exc_inline_rc.py 是压力用例，
但它只比输出，不测内存，所以基线抓不到泄漏）。

**推荐方案（可复用现有代码约 90%）**：

1. 在 runtime 增加单帧版 helper（从现有 `pa_frame_unwind` 抽出循环体一次迭代）：
   `void pa_frame_release_self(void)`：取 `pa_frame_top`，逐槽 dec/清零，`pa_frame_top = prev`；
   若 `pa_frame_top` 不是自己（嵌套 pad 打乱）就 `panic_c` 报错，别静默。
2. 每个“有 owned 槽的函数”（codegen 里 `rec_words > 0`，src/codegen_block.cpp:190-193）
   发一段 **cleanup-only LSDA**，保护区覆盖函数体（不含序言压帧之前、不含尾声弹帧之后），
   call-site 的 action 下标写 0（纯 cleanup，见 6.3）；landing pad 只做三件事：
   `leaq rec(%rsp), %rdi; call pa_frame_release_self; call _Unwind_Resume`。
   pad 只用 caller-saved 寄存器（不要 push，省 CFI）；pad 地址不落在自己保护区内。
3. 没有 owned 槽的函数（`rec_words == 0`）不发 LSDA；展开器跳过，非抛出路径零成本。
4. 手写汇编帧（`main`、模块 init 桩、回调跳板）没有槽，不需要 pad；但**不要**给它们加
   personality（会破坏尾跳/跳板的 CFI 约定，src/codegen.cpp:403-425、372-401）。

**能不能直接复用 `pa_frame_unwind`？** 释放逻辑可以复用，接口不能：
`pa_frame_unwind(target)` 的语义是“释放 target 之上的**全部更内层**帧”，依赖
“一次 longjmp 跳过一串帧”；表驱动是**一帧一次 pad**，每次只释放自己。所以抽成
`pa_frame_release_self`；原函数保留给 setjmp 路径（可挂起函数降级路径仍要用，见 5.2）。

**不要忘的两件事**：

- 临时栈：中间帧的临时值不在帧记录里，而在全局临时栈。现状靠“最内层 handler 的 mark”
  在 `pa_handler_leave` 里一次 `pa_rc_cleanup(mark)` 清掉（rt_exc.c:300-306、293）。
  表驱动下 handler 帧的 landing pad 必须做同样的清理；中间帧没有 mark，它们的临时值
  本来就在 handler 帧 mark 之上，会被一次清掉——前提是 landing pad 用“进入 try 时
  记录的水位”，不是“函数入口水位”。
- GC 影子根：pad 释放槽后要把槽清零（`pa_frame_release_self` 内做），否则 GC 扫描时
  会看到已 dec 的悬空引用（帧记录同时是影子根，runtime/rt_internal.h:315-322）。

### 5.2 (b) 生成器/协程挂起：会怎么坏？最小可行方案与降级

现状机制（三件套）：

1. 挂起前，`emit_yield` / `suspend_on_awaitable` 把 `st_->cleanups` 里所有 handler 记录
   从内到外 `HandlerSuspend`（src/lower_gen.cpp:83-94、331-343），runtime 只从全局栈摘下
   （rt_exc.c:320-324），记录本身留在帧槽里；
2. 恢复时按 `gen_state.cleanups` 快照逆序 `HandlerRearm` + 重新 `Setjmp`，非 0 跳回对应
   landing（src/lower_gen_async.cpp:430-478、484-544；rt_exc.c:326-332）——因为 jmp_buf
   属于已经返回的旧 resume C 帧，必须在本帧重新捕获；
3. 异常上下文另用 `pa_exc_enter/leave` 节点栈换进换出（rt_exc.c:371-433）；
   close()/finalize 用每个挂起点的 cleanups 快照重放 finally/`__exit__`，最后 `HandlerFree`
   释放摘下的记录（src/lower_gen_async.cpp:315-387、366-369；runtime rt_gen.c:211-267）。

**表驱动下这块到底怎么坏**：

- 好的一面：LSDA 是**词法的**（PC 落在哪个保护区由代码地址决定），所以“挂起后在 try 里”
  本身不是问题——resume 在任意 PC 重建 C 栈帧，展开器照样能找到保护区并跳到 landing pad。
  `HandlerSuspend` / `HandlerRearm` / `HandlerFree` 存在的唯一理由是 jmp_buf 的生命周期；
  表驱动下这部分**反而变简单**，可以整体删除。
- 坏的一面 1：**异常逃出 resume 帧的落点**。现在靠 C 级 setjmp 捕获：`gen_run`
  （rt_gen.c:74-94，异常→status=2→release slots→trace attach→重抛）、`coro_step`
  （rt_async.c:737-761）、`agen_drive`（rt_agen.c:152-179）、`pa_gen_close`
  （rt_gen.c:211-240）、`pa_gen_finalize`（rt_gen.c:244-267）。raise 换成
  `_Unwind_RaiseException` 之后，这些 C 级 setjmp **完全捕获不到**；如果 resume 函数里
  没有任何 landing pad 接住，异常会一路穿过 C 帧到 `_URC_END_OF_STACK`，默认行为是 abort。
- 坏的一面 2：**finally 体内挂起**。纯 cleanup 的标准做法是 pad 里跑完 finalbody 再
  `_Unwind_Resume`；但 finalbody 里一旦 yield/await 返回，unwinder 的展开过程就断了——
  `_Unwind_Resume` 无法跨一个已经返回的 C 栈帧继续。所以**可挂起函数的 finally 必须保留
  “把异常复制进帧槽、结束时重新抛”的方案**（现状 `ExcTakeValue`/`ExcReraiseValueIf`，
  src/lower_exc.cpp:407-462；rt_exc.c:684-698）。
- 坏的一面 3：`GenRethrow` 现在编译成 `call pa_coro_rethrow`（src/codegen_ops.cpp:185-189），
  后者 `exc_longjmp`（rt_async.c:791-799）。没有 handler 栈后必须改成
  `_Unwind_RaiseException`，且 throw 注入分发（src/lower_gen_async.cpp:484-544）不需要
  再 rearm/setjmp。

**最小可行方案（推荐，Z2/Z3 就这么做）**：

新路径只用于**非可挂起函数**（lowering 侧判据：`LowerOptions::zero_cost_exc` 且
`!st_->is_generator`；`st_->is_coroutine`/`is_async_generator` 都走生成器帧，同样排除）。
生成器/协程/异步生成器整体保留现有 setjmp 路径（包括 rt_gen.c / rt_async.c / rt_agen.c 的
C 级兜底），零改动。这样 Z2/Z3 的验收范围就是一个函数内不许混用两种机制，
风险可控，且普通函数热路径已经拿到零开销。

**最坏情况允许的降级（明确写出来）**：给生成器/协程**永久保留 setjmp 特例**。
代价是生成器里的 try 仍有 push/setjmp 的开销（文档实测 6 ns/次），收益只来自普通函数；
但避免了 Z4 最复杂的部分。这不是临时 workaround，而是一个可以接受的最终形态——
前提是文档/编译器内部明确标注“异常模型按函数种类分裂”，别让人误以为漏改。

**如果 Z4 要做统一（更完整但更贵），必须同时完成**：

1. 每个 resume 函数（生成器/协程/异步生成器共用 state machine，src/lower_gen_async.cpp:202-628）
   发一个**函数级 catch-all 保护区**覆盖 resume body，landing pad 做：status=2、
   `pa_gen_release_slots`、`pa_exc_unwind`、`pa_trace_attach`，然后以“异常逃出”返回值
   回到驱动器（不再用 C 级 setjmp）；
2. `pa_gen_close/finalize`、`coro_step`、`agen_drive` 改成读上面的返回状态；
3. close 重放路径删掉 `HandlerFree`，清理块直接跳（现快照机制可保留）；
4. `GenRethrow` → `_Unwind_RaiseException`；删 `HandlerSuspend/Rearm/Free` 三个 IR/运行时入口；
5. 可挂起函数的 finally 仍保留帧槽异常复制 + 结束时重新抛（`ExcTakeValue` 语义不变）。

**硬约束（写进 checklist）**：同一个函数内不能混用 longjmp 与 _Unwind 两种展开。
只要该函数里还有一条 `raise → exc_longjmp` 路径，它的 setjmp 兜底就必须还在；
反过来，一旦该函数的 raise 全换成 _Unwind，C 级 setjmp 就成了永远抓不到东西的死代码，
必须同批删除，否则异常逃出时直接 abort。

### 5.3 (c) finally 是 cleanup 不是 catch：现有 lowering 到 landing pad 的映射

现状 finally 有两条发射路径 + 一条早退路径（src/lower_exc.cpp:352-463；
src/lower_stmt_with.cpp:97-110）：

- 正常路径：`HandlerLeave` → 内联 finalbody → 跳 end（`:381-388`）；
- 异常路径：landing → `HandlerLeave` → `ExcTakeValue` 存帧槽 → finalbody →
  `ExcReraiseValueIf`（`:390-403`；可挂起函数一份 finalbody，`:407-462`）；
- 早退路径：`emit_cleanup_exits` 里 Finally 条目先 `HandlerDiscard` 再内联 finalbody（`:97-110`）。

**映射关系（直接可施工）**：

| 现状 lowering | 表驱动对应物 | 注意事项 |
|---|---|---|
| try body 的 `.LEHB/.LEHE` 保护区 | call-site 表的受保护区间 | 与 try 体块边界一致（src/lower_exc.cpp:215-221）；`try.land` 自身的地址必须在区间之外 |
| `finally.land` 块 | cleanup landing pad（call-site action 下标 0） | phase 1 对 cleanup 只返回 `_URC_CONTINUE_UNWIND`，phase 2 才进 pad；pad 入口先 `rc_cleanup(mark)`（复刻 `HandlerLeave` 的水位清理，rt_exc.c:300-306） |
| 异常路径 `ExcTakeValue` + 帧槽 | **可挂起函数保留**；非可挂起函数可不要 | 非可挂起函数直接用 unwinder 手里的异常对象，finalbody 结束 `_Unwind_Resume`；但 finalbody 里若 `return/break/continue`（由 `terminated()` 判断）则不 resume，改 `_Unwind_DeleteException` 吞掉异常 |
| `ExcReraiseValueIf` | 非可挂起：`_Unwind_Resume`；可挂起：复制帧槽 → 重新 `_Unwind_RaiseException` | 重新抛会换一个包装对象（现状 `pa_exc_value_from_exc` 本来也是复制），若要保留对象身份需要别的手段 |
| 正常路径 finalbody | fall-through（同一份 finalbody 代码） | 现状非可挂起函数内联两份；Z2 之后建议统一成一份，与可挂起函数一致（`:407-415` 的 `fin` 块） |
| 早退路径 | **原样保留** `emit_cleanup_exits` 内联（不需要 landing pad） | 早退不是展开（没有异常对象），走表驱动反而要伪造异常；保留现逻辑最省事也最安全 |
| `with` 的 `__exit__` | 与 finally 不同：`__exit__` 可能**抑制**异常 | 纯 cleanup action 无法抑制（phase 1 已经决定继续展开）。若要不改语义，with 区要么在 phase 1 作为“候选 handler”被选中、landing 里调 `__exit__`，返回假时用**重新 `_Unwind_RaiseException` 同一对象**继续向外（不是 `_Unwind_Resume`，见 4.1）；要么 Z2/Z3 直接不把 with 迁到新路径 |

**结论**：finally 能干净地映射到 cleanup landing pad；`with` 不能——它是“可能捕获的 cleanup”，
需要 handler 语义。Z2 只做单层 catch-all try 的验收里不要顺手把 finally/with 一起迁；
Z4 时把 finally 放 cleanup action、with 放 handler action，并用“重新 raise 同一对象”解决
with 返回假之后的继续展开。

另外两个没有明说但必须处理的细节：

1. landing pad 的 CFI：pad 位于函数体内的 PC，展开器要求 pad 自身也有正确的 CFI（`.cfi_startproc`
   之内、与函数体同一 CFA 状态；如果 pad 里 push 了寄存器必须补 `.cfi_*`）。
2. `.cfi_lsda` 必须在 `.cfi_startproc` 之后、且 LSDA 标签可以前向引用；
   每个函数一份 LSDA，标签名按函数唯一（建议 `.LLSDA<func_index>`）。

## 6. Z2 发射清单（按“编译器要发射什么”的粒度）

前置事实：生成程序已有 `.eh_frame`/`.eh_frame_hdr`（789 个 FDE，`tests/cfi/` 在回归），
`libgcc_s.so.1` 随 `cc` 自动链接（`_Unwind_Backtrace` 已在用），缺的只有 LSDA + personality +
`_Unwind_RaiseException` 这条抛路径。以下按实现顺序排；带 ☐ 的条目可以直接当 issue 用。

### 6.1 运行时（新文件 runtime/rt_eh.c，或并入 rt_exc.c）

- ☐ 定义异常包装：
  `struct PaEhExc { struct _Unwind_Exception unwind; PyAdamasException* pending; };`
  要求 `alignas(16)`，`unwind` 必须是首成员；`exception_class` 用唯一 64 位魔数（如
  `0x50414558_00000001`，“PAEX”），与 `PYADAMAS_EXCVAL_MAGIC` 同级但不同值；
  `exception_cleanup` 只 `free(wrapper)`，**不要**释放 `pending`（pending 归 `pending_exc`/
  `ExcValue` 的生命周期管）。
- ☐ `pa_eh_throw(PyAdamasException* e)`：先 `exc_free(pending_exc)` 再 `pending_exc = e`（与 `pa_raise`
  一致，rt_exc.c:456-468）→ `_Unwind_RaiseException(&w->unwind)`；返回 `_URC_END_OF_STACK` 时
  走与 `exc_longjmp` 的“无 handler”分支相同的收尾：`exc_print_uncaught(pending_exc)` +
  `exit(1)`（rt_exc.c:194-208）。
- ☐ `pa_eh_rethrow(void)`：取当前包装（全局或异常对象私有区）再 `_Unwind_RaiseException`
  **同一个对象**（重启 phase 1，见 4.1）；不新建 pending。
- ☐ `pa_eh_done(void)`：landing pad 处理完（命中 except / 抑制）后
  `_Unwind_DeleteException` + 清全局包装指针；保持幂等。
- ☐ `pa_eh_frame_release_self(void)`：从 `pa_frame_unwind`（rt_exc.c:20-38）抽出单帧版；
  语义 `panic` on mismatch（`pa_frame_top != 自己`）。
- ☐ `pa_personality(...)`（导出名 `pa_personality`，不要用 `__gxx_personality_v0`，避免拉
  libstdc++）实现 phase 1/phase 2 的下述逻辑。

### 6.2 personality routine 契约

签名：`_Unwind_Reason_Code pa_personality(int version, _Unwind_Action actions,
uint64_t ex_class, struct _Unwind_Exception* exc, struct _Unwind_Context* ctx)`。

- ☐ 只处理 `ex_class == 自己的魔数`；其它类返回 `_URC_CONTINUE_UNWIND`。
- ☐ `lsda = _Unwind_GetLanguageSpecificData(ctx)`；空则 CONTINUE。
- ☐ `ip = _Unwind_GetIPInfo(ctx, &before_insn)`；call-site 查表用 `ip - (before_insn ? 0 : 1)`。
- ☐ 解 LSDA（格式见 6.3）：LPStart → region_start；TType 编码/偏移；call-site 表；action 表。
- ☐ phase 1（`_UA_SEARCH_PHASE`）：查当前 PC 所在 call-site 条目；
  - action 下标 0 = 纯 cleanup（finally / 帧释放 pad）→ `_URC_CONTINUE_UNWIND`；
  - action 下标 >0：沿 action 表的链取 filter；filter 指向 null typeinfo = catch-all（Z2 全命中），
    指向类型表条目则用 `pending_exc->type` 与该类型做
    `pa_exc_matches` 等价匹配（`exc_is_a`，rt_exc.c:186-192）；命中才
    `_URC_HANDLER_FOUND`，否则 CONTINUE（或按 4.1 的方案 2 一律 HANDLER_FOUND + pad 里重抛）。
- ☐ phase 2（`_UA_CLEANUP_PHASE`）：
  - 若 `actions & _UA_HANDLER_FRAME`：`_Unwind_SetGR(ctx, 0, exc)`、`_Unwind_SetGR(ctx, 1, 0)`、
    `_Unwind_SetIP(ctx, landing_pad)`，返回 `_URC_INSTALL_CONTEXT`；
  - 否则若该条目有 landing pad（cleanup）：只 `_Unwind_SetIP(ctx, landing_pad)`（cleanup 不需要
    SetGR），返回 INSTALL_CONTEXT；
  - 否则 CONTINUE。
- x86-64 SysV 的 EH 返回数据寄存器：`__builtin_eh_return_data_regno(0)` 是 %rax、
  `(1)` 是 %rdx；landing pad 入口按“%rax=异常包装指针，%rdx=selector”读。
- ☐ 拒绝：Win64（`target_abi().gnu_cfi == false`）一律不走新路径。

### 6.3 编译期要发射的汇编（codegen 侧）

每个带 EH 区域的函数：

1. ☐ 在 `.cfi_startproc`（src/codegen_block.cpp:227）之后立刻发：
   - `.cfi_personality 0x9b, .Lpers<fn>`（0x9b = indirect|pcrel|sdata4；`.Lpers<fn>` 是
     `.data.rel.ro` 里的一个字 `.quad pa_personality`；也可用 0x13 直接 pcrel|udata4，
     但按 Z1 实测口径先用 0x9b）。GCC 用 `DW.ref.*` 只为 personality 可能来自 DSO；
     我们把 `pa_personality` 编进 rt.o（同一可执行文件），直接一个 `.quad` 即可，不需要
     DW.ref 的 weak/comdat 那一套；
   - `.cfi_lsda 0x1b, .LLSDA<fn>`（0x1b = pcrel|sdata4）。
   这两条必须紧跟 `.cfi_startproc`，且 `.LLSDA<fn>` 允许前向引用（定义在文件尾的
   `.gcc_except_table`）。
2. ☐ 保护区标签：try body 起点发 `.LEHB<fn>_<i>:`，结束（正常路径跳出 body 的地方）发
   `.LEHE<fn>_<i>:`。位置对齐 lowering 的 `try.body` 块边界（src/lower_exc.cpp:215-221）；
   早退路径不进保护区（早退是普通跳转）。
3. ☐ landing pad：在 `.cfi_endproc` 之前（函数体内）发 `.LehLP<fn>_<i>:`；
   - pad 地址不能被自己的 call-site 条目覆盖（`.LEHE` 在 pad 之前；多个 pad 依次排，
     每个 pad 的条目区间不包含 pad 自身）；
   - pad 内只用 caller-saved；若必须动栈/callee-saved，补对应 `.cfi_*`；
   - pad 第一条指令把 `%rax`（异常包装）存到栈槽（若后续还要 rethrow）；
   - 命中 except 之后继续走现有 `try.land` 分发块（见 6.4）。
4. ☐ `.gcc_except_table`（文件尾，与 `.text` 切换）。**下面的编码/字节序是 g++ 14.2.0 实测口径**
   （任意 `g++ -O0 -S -fno-asynchronous-unwind-tables` 小程序都能反推出同样的 .LLSDA 段）：
   - 纯 cleanup 区（finally / 帧释放 pad）：call-site 的 action 下标写 **0**，TType 编码写 `0xff`，
     整个 action 表/类型表都不写；
   - catch-all 区（Z2 的 try 与 with）：TType 编码 `0x9b` + 类型表偏移，call-site action 下标写 **1**，
     action 表一条 `.byte 1, .byte 0`，类型表一条 `.long 0`（null typeinfo = catch-all）。
   ```asm
   # (a) 纯 cleanup 的 LSDA（GCC 实测）
       .section .gcc_except_table,"a",@progbits
       .align 4
   .LLSDA<fn>:
       .byte 0xff                 # LPStart 编码：omit（= 函数起点）
       .byte 0xff                 # TType 编码：omit
       .byte 0x1                  # call-site 表编码：uleb128
       .uleb128 .LLSDACSE<fn>-.LLSDACSB<fn>
   .LLSDACSB<fn>:
       .uleb128 .LEHB<fn>_0-.Lf<fn>     # 保护区起点（相对函数起点）
       .uleb128 .LEHE<fn>_0-.LEHB<fn>_0 # 长度
       .uleb128 .LehLP<fn>_0-.Lf<fn>    # landing pad（0 = 无 pad）
       .uleb128 0                       # action 0 = 纯 cleanup
   .LLSDACSE<fn>:
   # (b) catch-all 的 LSDA（GCC 实测；Z2 的 try/with 用这个）
   #     下面是同一 .section 里另一个函数的 LSDA，把 <fn> 换成 <fn2> 即可
       .byte 0xff
       .byte 0x9b                 # TType：indirect|pcrel|sdata4
       .uleb128 .LLSDATT<fn>-.LLSDATTD<fn>   # 类型表偏移（TTBase - TType 起点）
   .LLSDATTD<fn>:
       .byte 0x1                  # call-site 表编码：uleb128
       .uleb128 .LLSDACSE<fn>-.LLSDACSB<fn>
   .LLSDACSB<fn>:
       .uleb128 .LEHB<fn>_0-.Lf<fn>
       .uleb128 .LEHE<fn>_0-.LEHB<fn>_0
       .uleb128 .LehLP<fn>_0-.Lf<fn>
       .uleb128 1                 # action 下标 1
   .LLSDACSE<fn>:
       .byte 1                    # action 表：filter index 1
       .byte 0                    # next action = 0（链结束）
       .align 4
       .long 0                    # 类型表[1] = null typeinfo = catch-all
   .LLSDATT<fn>:
       .text
   ```
   注意：`.Lf<fn>` 是 codegen 已有的函数起点标签（src/codegen_block.cpp:210 的
   `label(func_label(fi))`；func_label 返回 `.LfN`，src/codegen_impl.h:350）；类型表偏移是
   「TTBase - TType 起点」，TTBase 在类型表**末尾**（`.LLSDATT`）。Z2 只需要上面 (a)(b) 两种；
   类型过滤（Z3）把 `.long 0` 换成一串异常名字符串/typeinfo 指针，由 personality 按
   `pa_exc_matches` 语义比较。
5. ☐ 每个函数“有 EH 区域”的判据：含 try/with/finally 的**普通函数**（`!is_generator` 且
   `LowerOptions::zero_cost_exc`）；可挂起函数仍走旧路径（5.2）。
6. ☐ 入口函数的 catch-all：zero-cost 模式下给 entry 模块的 `__pa_main` 包一层 catch-all 保护区，
   landing pad 复刻 `exc_print_uncaught` + `exit(1)`（rt_exc.c:194-208）。**必须有**：
   否则“一个 handler 都没有”时 `_Unwind_RaiseException` 返回 `_URC_END_OF_STACK` 且
   **phase 2 不跑**，未捕获异常路径上的 finally 不会执行（CPython 会执行）。

### 6.4 IR / lowering 侧最小改动

| 改动 | 位置 | 内容 |
|---|---|---|
| 开关 | src/lower_impl.h 的 LowerOptions / CLI | `--zero-cost-exc`（默认关）；Z2 期间只对 `!is_generator` 生效 |
| 保护区起点 | 新 IrOp `EhBegin`（imm=区域号） | 替代 `HandlerPush` 的“记录水位”部分：保留 `RcMark` 取临时栈 mark 存槽（现有 `RtFn::RcMark`，src/lower_gen_async.cpp:394 已在用） |
| 保护区终点 | 新 IrOp `EhEnd`（imm=区域号） | 对应 `.LEHE`；正常路径跳走前发 |
| landing 标签 | 新 IrOp `EhLandingPad`（imm=区域号） | codegen 只发标签 + 记录区域信息；块内分发代码由现有 lowering 生成 |
| 弹记录替代 | `HandlerLeave` → `RcCleanup(mark)` | `pa_handler_leave` 的两件事拆开：记录弹栈在表驱动里不存在，只保留 `pa_rc_cleanup(mark)`（rt_exc.c:305）；用现有 `RtFn::RcCleanup` |
| 不匹配重抛 | `ExcReraise` → `EhRethrow`（新 RtFn `pa_eh_rethrow`） | 语义：重新 `_Unwind_RaiseException` 同一对象；不要用 `_Unwind_Resume`（4.1） |
| 捕获后收尾 | landing 里取走异常后加 `pa_eh_done()` | 在 `ExcAcceptValue`/`ExcAcceptInstance` 之后立即释放包装（最迟在 `ExcDone` 之后）；不要碰 pending（`ExcDone` 已管，rt_exc.c:359-369） |
| 抑制（with） | `ExcClear` 之后加 `pa_eh_done()` | 同上 |
| 终止控制流的 landing | `terminated()` 分支 | finalbody 里 return/break/continue 时 `pa_eh_done()` 吞掉异常，不再 resume |

**接缝原则**：landing pad 的**分发代码一行不改**——还是 `ExcMatches`/`ExcAcceptValue`/
`ExcDone`/`ExcClear`/`Raise*`（src/lower_exc.cpp:233-337）。只替换“到达 landing 的方式”和
“离开 landing 的方式”（leave→cleanup，reraise→eh_rethrow，done→eh_done）。这样 Z3 的
验收“输出与 setjmp 路径一致”才可控。

### 6.5 门禁与回落

- ☐ `zero_cost_exc && !st_->is_generator && target_abi().gnu_cfi` 才走新路径；其余全走旧路径。
- ☐ 旧路径保留到 Z5；Z2 期间两套都编译通过（用开关切）。
- ☐ 生成器/协程/异步生成器在 Z2/Z3 明确不迁；CLI 或内部断言拒绝混用（5.2 硬约束）。
- ☐ IR/optimizer 新 op：`EhBegin`/`EhEnd`/`EhLandingPad` 必须登记进终止指令判断与
  “不能跨块搬动”的集合（对照 src/ir_types.h:60-67 的 `terminated()` 和
  src/lower_gen_spill.cpp:58-66 的 Gen* 处理）。

### 6.6 验收命令（Z2/Z3 每步都跑）

```bash
P=/tmp/z2/prog
build/dev/pyadamas run --rebuild -o $P tests/run/22_exception.py
readelf -S $P | grep -E "gcc_except_table|eh_frame"     # 出现 .gcc_except_table
readelf -wf $P | grep -c personality                     # 对应函数有 personality
objdump -d $P | grep -c "call _setjmp"                   # 新路径函数里应为 0
bash tests/exc/sem_baseline.sh                           # 42/42 PASS
```

Z2 的最小验收建议：只迁 `tests/run/22_exception.py` 里的单层 try（或新写一个 catch-all 用例），
逐字对比输出；Z3 再加 `pa_exc_matches` 类型分发与嵌套 try。

## 7. 基线脚本 tests/exc/sem_baseline.sh

### 7.1 选例方法（可复跑）

```bash
# tests/run 里异常相关关键词（多关键词取并集后人工确认）
grep -lE "try:|except|finally:|raise |with |\.throw\(|athrow\(|StopIteration" tests/run/*.py
# tests/fail、tests/unsupported、tests/strict：grep 结果为 0 —— 没有异常语义用例，
# 它们的期望是编译期拒绝，不属于“语义基线”
grep -rnE "except|finally|raise |try:|with " tests/fail/ tests/unsupported/ tests/strict/
```

人工确认后选 42 个（控制在 2 分钟内；实测串行总耗时约 8 秒）：

| 分组 | 用例 | 覆盖的语义 |
|---|---|---|
| try/except/else/finally 主干 | 22_exception、20_with、159_exc_multi、32_exception_value、34_exception_chain、157_rich_exc、153_traceback | 多 except 顺序、bare except、else、嵌套、reraise、多参 args、富异常、with 抑制/嵌套/早退、`__cause__`、未捕获 traceback 文本 |
| 帧释放 / 寄存器 hazard | 92_exc_inline_rc、100_handler_loop_carried | 异常穿过真实帧的槽释放（2000 轮）、保护区写 + 区外读的寄存器回退 |
| 生成器/协程 | 23_generator、28_yield_from、118_suspend_expr、119_gen_except_suspend、122_gen_throw、123_finally_yield、129_finally_jump、130_throw_class、94_suspend_positions、93_gen_exc_rc | yield 在 try/except/finally、挂起后重挂、throw 注入、close 重放、finally 里 return/break/continue、异常逃出时帧槽释放 |
| asyncio/异步 | 24_async、25_task、26_async_lang、29_async_comp、31_asyncgen_proto、124_asyncgen_await、131_async_aiter、154_nested_await | 异常跨 await、Task 等待者异常、取消级联、async for/with、异步生成器 athrow/aclose、异步推导式 |
| 运行时错误/内置/优化交互 | 27_runtime、33_builtins、39_match_advanced、43_indexing、46_simd_reduce、50_licm、53_json、58_re_err、66_genexpr、73_lazy_gen_range、74_range_object、77_functools、91_csv、126_decorated_slots、145_collections_deque | 运行时错误入口（IndexError/KeyError/ZeroDivisionError…）、优化遍与 handler 的交互、native/stdlib 抛出的异常 |

### 7.2 用法与判定

```bash
bash tests/exc/sem_baseline.sh          # 逐个 PASS/FAIL + 汇总行；全 PASS 退出 0
bash tests/exc/sem_baseline.sh -v       # 额外打印失败用例的 diff 前几行
PYADAMAS_BIN=/path/to/pyadamas bash tests/exc/sem_baseline.sh   # 指到别的构建
```

- 每个用例：`<pyadamas> run --rebuild -o <tmp>/<name>.prog <test>`（stdout+stderr 合并，
  与 `tests/run_tests.sh` 的 `run_test` 口径一致，src 见 tests/run_tests.sh:78-93），
  期望输出从测试文件自身的 `# --- expect ---` 段提取，`diff` 完全一致才算 PASS；
- 输出形如 `PASS 22_exception` / `FAIL 22_exception (output diff)`，最后一行
  `SUM PASS 42 / 42 FAIL 0`；退出码 0/1；二进制缺失退出 2；
- 输出不含时间戳/随机数/临时路径，重复跑逐字节一致，便于改造前后 `diff` 基线；
  对 `output diff` 自动重试一次（容忍高并发负载下 asyncio 毫秒 timer 的偶发抖动），
  重试不写进输出，真回归两次都会失败；`run failed`（编译/运行错）不重试；
- 不依赖 `build/` 里的既有产物：编译输出与中间文件都在 `mktemp -d` 目录，结束时删除；
  不跑全量 161 个用例，不并行，不跑 ASan；
- 可以从任意当前目录跑：脚本用 `BASH_SOURCE` 定位仓库根；每个用例在 `mktemp -d` 子目录里执行，
  测试文件传绝对路径（因此不依赖调用者的 cwd，也不会往 `build/` 写缓存）。

### 7.3 当前基线（改造前）

```text
$ bash tests/exc/sem_baseline.sh
PASS 22_exception
PASS 20_with
...（共 42 行）
SUM PASS 42 / 42 FAIL 0
$ echo $?
0
```

本轮（2026-11-16）在 `build/dev/pyadamas` 上实测 42/42 PASS；Z2/Z3 改完后
先重跑本脚本得到 before/after 两份输出再 diff。失败用例按 docs 第 2 节的语义行定位。

### 7.4 基线覆盖不到的（别误以为全绿 = 语义没丢）

- 跨模块自定义异常类名（4.8）：没有用例；
- `__context__` / `__traceback__` / `sys.exc_info`（4.2/4.4/4.5）：没有用例；
- `StopIteration` 逃出生成器（4.9）：没有用例；
- 异常穿过 cimport / C 回调（4.12）：没有用例；
- 内存泄漏：只用“输出一致”判定；`92_exc_inline_rc` 的循环会把泄漏放大到可观察，
  但不检查 RSS；要测泄漏需另配 `PYADAMAS_RC_STATS` 或手写压力程序（docs/runtime.md）；
- 编译期错误/拒绝：`tests/fail`、`tests/unsupported`、`tests/strict` 不在本基线范围；
- 性能：本基线只验证语义，不测 setjmp 开销（性能口径见 docs/zero-cost-exceptions.md 的「现状与目标」节）。
