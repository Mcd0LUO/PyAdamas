# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

<!-- 0.559–0.632 的条目在 2026-11-26 的一次文件回退中丢失，未恢复；逐条记录见 `git log --oneline 0.559..0.632` 与 `docs/refactor-plan.md`。以下从 0.633 起重续。 -->

## [0.683] - 2026-11-27

> fix(runtime)：**`list_buf_grow` 的增长前提加兜底**（运行期审计 A10-1）。
>
> 该函数三处 `memcpy` 都按**旧** `l->cap` 拷贝，正确性依赖「`ncap > l->cap`」——
>所有调用点（append / copy_range / inplace_repeat / insert）确实都满足，但这条前提
>**没有任何断言或兜底**。将来若出现「传更小的 ncap」的调用点，就是一次堆溢出。
>
> 加一行 `if (ncap < l->cap) ncap = l->cap;`：夹紧后三个分支都安全
>（等长时 memcpy / realloc 无副作用）。
>
> 套件 **602**、difftest **545-0**。
>
> ## 同轮的运行期审计结论（A10 / A11，**未改动任何代码**）
>
> 审计覆盖 `rt_core.c` 的 RC/临时值栈/GC 集合装配与清扫、`rt_list.c` 的池化增长与所有变异入口，
>以及 `rt_str.c` 的 SSO / 码点索引 / 构造契约，并跑了 **25 个 ASan 压力程序**（列表增长与收缩、
>dict 墓碑、字符串拼接与切片、循环引用、嵌套级联销毁、共享缓冲、迭代中修改、切片边界、
>200KB 大串、二进制串往返、GC 压力下的临时串……）：
>
> **没有发现内存错误，25/25 与 CPython 逐字节一致。**
>
> 另记录四条「非缺陷但值得固化」的观察（留在审计文档里）：
>
> 1. 内联 list 回收有两处形态不同的实现（`pa_rc_dec_slow` 与 `pa_rc_dec_heap_batch`，后者必须显式
>    `h->rc = 0`，注释记录过 0.274 的二次 free 事故）——同一语义两份代码，是最容易被后人改错的地方，
>    建议抽成单一 `list_recycle_inline(h)`；
> 2. `gc_collect` 必须把 IMMORTAL 对象显式标活（「只被 C 静态变量持有」的对象不在任何根里），
>    这是**隐式契约**：新增任何 C 侧长期持有的堆对象都必须走 IMMORTAL 或 `pa_gc_add_roots`，
>    否则 minor GC 会误回收（注释记录过 `sys.stdout` 被误 free）；建议写进 `rt_internal.h` 的不变量清单；
> 3. `pub = -1` 哨兵 + 「必须 seal」是纸面契约，只有 `PYADAMAS_STR_DEBUG=1` 才会拦；漏 seal 的串在
>    生产构建里会让 `len(s)` 返回 **-1**（静默错值）。建议把「带 DEBUG 旗跑一遍 tests/run」写进闸门；
> 4. `str_new` 每次 O(n) 扫码点数，ASCII 路径若误用它是纯性能税、无可观测差异，只能靠 profile 发现。
## [0.682] - 2026-11-27

> feat(exc)：**方案 A 第二步落地 —— 同帧 `raise` 直接跳到 landing pad，绕开 libgcc 展开**。
>
> excbench（10 万次真抛出，绑核 7 轮交错中位）：
>
> | 口径 | 中位 |
> |---|---|
> | 门控关（默认，表驱动） | **224 ms** |
> | `PYADAMAS_EXC_DIRECT=1` | **38 ms（5.9×）** |
> | `--no-zero-cost-exc`（setjmp） | 36 ms |
>
> 直接跳已基本追平 setjmp。我独立复测（本机带负载）：off 239–251 ms / on 53–64 ms（**4.5×**），
>`ir -O2` 里 `pa_eh_raise_prep` 出现 1 次（excbench 就一处 raise）、两个二进制 md5 不同。
>
> ## 实现（约 214 行）
>
> - **runtime**：`pa_eh_raise` 拆成 `pa_eh_raise_prep(type,msg,slot)`（构造异常、设 pending_exc、
>   捕获 traceback、写约定槽位，**不抛**）+ 原展开路径复用；新增 `RtFn::EhRaisePrep`
>   （`rtfn.def`/`rtfn.h`/`rtfn_names.h`/`call_rt.cpp` 编组，`gen_rtfn.py --check` 一致）。
> - **lower**：`EhRegion` 增 `direct_block`/`direct_slot`/`direct_ok`；`lower_try_zero_cost`
>   对「单 handler、无 finally」的保护区建 `try.dispatch` + `try.land.direct`（跳过
>   `EhLandingPad`/`pa_eh_land`，取约定槽位后汇入**同一条** handler 分发链）。
> - **新 pass** `src/opt/exc_direct.cpp`（紧跟内联、LICM 之前）：判据**只用块布局区间**
>   `[begin_idx, end_idx)`（codegen 的 `.LEHB/.LEHE` 即按布局发；内联器原地 splice）。
>   嵌套 setjmp 用 `HandlerPush/HandlerLeave` 区间排除；嵌套零开销取最内层；
>   不匹配 handler 完全复用既有 `pa_exc_matches` / `EhRethrow`（重抛重启 phase 1）。
> - **门控** `PYADAMAS_EXC_DIRECT`，**默认关**（合并零风险；默认开只需去掉两处 env 判断）。
>
> ## 过程中抓到并修掉的 bug
>
> `land_direct` 初版把 `EhLeave + RcCleanup` 写在直接入口里，而它跳进的 `dispatch` 块开头
>**也**做这两件事 → 直接跳路径 `EhLeave` 两次：单层 try 被 `pa_eh_leave` 的 `if (depth>0)` 掩盖，
>但**嵌套 try 时把外层 depth 记账吃掉**，之后 C 运行时 `IndexError` 误走 setjmp 路径 → 未捕获退出。
>已改为直接入口只做 slot load + jump。
>
> ## 验证
>
> 17 形态等价性（`tests/run/exc_direct_shapes.py`：裸 except / except Exception / 具体类匹配 /
>不匹配重抛（同函数外层、调用方、无外层→进程终止）/ 子类匹配 / 元组 handler /
>`KeyboardInterrupt` 不被 `except Exception` 接住 / finally / 嵌套 try / handler 里再 raise /
>`raise ... from` / 生成器 / 递归），门控开/关/CPython 三者逐字节一致。
>
> 闸门：套件 **602**、difftest **545-0**、opt_levels **40/40**（默认与门控开各一遍）、
>`VERIFY_IR=1` 全套件无违规、单例 ASan 0 报错、`checkall --full` **全部 PASS**
>（含 perf-baseline 12 项 warn=0 fail=0）。非 eligible 函数门控开/关 asm 逐字节一致。
>
> ## 一个测量陷阱（值得记）
>
> 我第一次复测时「门控开/关运行时没差别」，原因是**两次 `build` 都没加 `--rebuild`**——
>门控是编译期的，而 modcache 的 key 不含它，于是第二次直接复用了门控关的 IR，两个二进制逐字节相同。
>**结论：凡是用 env 门控做 A/B，两边都必须 `--rebuild`**（或清 modcache）。
>
> ## 范围外（保持老路径）
>
> 值 raise（`raise ValueError("x")` 走 `pa_raise_value_from` / `pa_raise_instance`）、多 handler、
>带 finally 的保护区、生成器/协程里的 try、`raise ... from`。
## [0.681] - 2026-11-27

> feat(lower)：**方案 A 第一步**——「当前 try 区域」栈 + landing pad 第二条入口骨架（76 行）。
>
> 目标（见 `docs/exc-path-performance.md`）：同一函数内 `raise` 直接跳到该 try 的 landing pad，
>绕开 libgcc 展开。表驱动抛出 100k 次 **235 ms** vs setjmp **45 ms**，79.5% 在 `_Unwind_RaiseException`。
>
> 本步只做准备，**零行为变化**：
>
> 1. `ZcRegion` + `zc_try_stack_`：try 体降级期间 push（`exc.cpp:952`），
>    LP 块建好后回填 `lp_block`/`end_block`/`land_direct_block` 再 pop（`:1039`）。
>    pop 在 handler 体之前，所以 handler/finally 里的 raise 不会被误判成「在 try 体内」。
>    对外只读查询 `const ZcRegion* current_try() const`。
> 2. LP 第二条入口 `try.land.direct`：跳过 `EhLandingPad` + `pa_eh_land`，从约定槽位取异常对象，
>    再汇入与正常落地**共用**的 handler 分发链。没有任何跳转过来（`lower_raise` 不动）→ 死块。
>
> **门控**：只在 `PYADAMAS_EXC_DIRECT_PREP=1` 时发射。默认路径 `chain = land`，不建任何新块，
>所以默认生成代码不变。
>
> 验收（两种门控状态都跑）：套件 **601 + 2 strict rejected**、全量 difftest **544-0**、
>`tests/opt_levels.sh` **40/40**；同一二进制重复编译的 asm 逐字节一致。
>
> 已知限制（写给 step 2）：`.LEHE` 必须落在 try 体指令之后 ⇒ `land`/`dispatch`/`land_direct`
>只能在 body 降级**之后**创建，所以 body 降级期间 `current_try()` 的块号还是 0/0/-1。
>step 2 有两条路：把直接入口块提前到 body 之前建，或 raise 降级时登记「待回填跳转」补丁表
>（后者布局最干净，推荐）。
## [0.680] - 2026-11-27

> fix(runtime)：**挂起的生成器在 `except` 里遇到 GC 会丢失当前异常**（会退化成
>`RuntimeError: No active exception to reraise`）。
>
> ```python
> def g():
>     try:
>         raise ValueError('boom')
>     except ValueError:
>         yield 1              # 挂起，异常写进帧槽（own==3，裸 malloc 的 PyAdamasException）
>         raise                # 恢复后重抛
> # 两次 yield 之间分配 12 万个 list 触发 minor GC：
> #   修前：RuntimeError: No active exception to reraise
> #   修后：1 / re-raised: boom / 2（与 CPython 一致）
> ```
>
> 根因：`pa_gen_slot_release` 顶部已按不变量跳过 trial/restore 阶段的清槽
>（「那时只是试算计数，帧的内容之后还要用」），但 `own==3`（异常对象）/ `own==4`（save 链）
>两个分支**自己无条件 free 并清槽**，把这条不变量绕过去了。gen 帧是被跟踪对象，
>trial pass 必然对它调 release → 真 free 掉异常对象并清零 → restore 阶段看到 0 回滚不了。
>
> 修法（3 行）：trial/restore 阶段对 3/4 分支直接 return，不动非 RC 对象。
>
> ## 同轮的联合类型收尾（来自子代理）
>
> 0.678 只改了**类型检查**层的联合选型；降级层 `lower/method/method.cpp` 还有一份自己的
>`hits == 1 → 选它`，所以 `r13`（打印 7）/ `r15`（越界读 0）仍然错。判据改成
>`classes == 1 && hits == 1`——只有「非 None 成员恰好一个」（Optional 形态，运行期有
>`guard_not_none` 兜底）才静态选型，多类联合一律显式报错。
>
> 试过与类型检查同口径的 `hits == classes`：`r11/r12` 立刻退化成静默错值（打印垃圾值
>`96993287288576`、越界打印 0），因为降级层静态选第一个成员本身就错——多类联合只能靠
>运行期 tag 分派。这条写进了注释。
>
> 前置缺口一并补上：`isinstance(o, int)` 原来**完全不窄化**（内置类型名没有 ClassInfo），
>`tests/run/isinstance_neg_branch.py` 一直靠降级层 `hits==1` 启发式兜着；收紧后必然误杀，
>所以新增 `narrow_isinstance_builtin`（按 TyKind）。
>
> 顺手修了 0.679 的测试缺陷：`tests/run/assert_narrow_scope.py` **缺 `# --- expect ---` 块**
>→ 期望输出为空、无论如何都 OUTPUT DIFF、**套件恒 rc=1**（0.679 的「600 全绿」实际不可能）。
>已补期望并核对 pya/CPython 一致。
>
> 另外修掉审计 #3 的误杀：`return x.m() if x is not None else 0` 现在编译并输出 `4 / 0`。
>
> 闸门：套件 **601 + 2 条 strict rejected**、difftest **544-0**、check_size。
>
> **未修（记录）**：GC 清扫路径对 untracked 字符串是裸 `free`，绕过 `str_pool_put`/码点索引释放，
>ASan 实测漏 618 字节（探针 `/tmp/rcprobe/str_cp_leak2.py`）。试过改成走池，泄漏只从 626 变 618，
>说明根因不在这里，已回退注释留痕，避免用未证实的改动掩盖问题。
## [0.679] - 2026-11-27

> fix(typecheck)：**`assert x is not None` 的窄化不再是「泄漏出块」的** —— 修掉一个段错误。
>
> ```python
> def f(x: Optional[C], n: int) -> int:
>     while i < n:              # n = 0，循环体一次都不执行
>         assert x is not None
>         i = i + 1
>     return x.m()              # 修前：段错误（把 None 当类指针解引用）
>                               # 修后：显式类型错误（x 仍可能是 None）
> ```
>
> 根因：`stmt_assert` 施加窄化时**不登记恢复点**，注释假设「外层 if 分支的 restore_types
>快照会把它限制在分支内」——但**只有 `if` 有快照**，`while`/`for`/`with`/`try` 的体没有。
>于是循环体（或 try 体里 assert 之前抛异常跳过 assert）未执行时，窄化仍然生效到块之后。
>
> 修法：在 `TypeChecker` 里加 `narrow_sink_`——`stmts_impl` 把当前序列的恢复点集合挂上去，
>`stmt_assert` 把窄化登记进去，出序列时统一 `restore_narrowing`。嵌套体各自建 sink，
>所以作用域就是**余下的语句**，与 CPython 的语义一致。
>
> 直行代码里的 assert 窄化不受影响（`assert x is not None; return x.m()` 仍通过，输出与 CPython 一致）。
>
> 三个探针从「段错误 / 静默错值」变成**显式类型错误**：
>`p8_assert_loop_crash`（段错误）、`p2_assert_for`（静默错值 8）、`r16_try_realistic`（段错误）。
>
> 新增回归用例 `tests/run/assert_narrow_scope.py`（循环零次执行 + 直行 assert 两种形态）。
>
> 闸门：套件 **600**、difftest **543-0**、check_size。
## [0.678] - 2026-11-27

> fix(typecheck)：**联合接收者的方法调用只在「其余成员都是 None」时才按命中者查**。
>
> ```python
> class C:
>     def m(self) -> int: return 7
> class D: pass
> def f(x: C | D) -> int: return x.m()
> f(D())   # 修前：打印 7（按 C 发码，无 tag 守卫）
>          # CPython：AttributeError: 'D' object has no attribute 'm'
> ```
>
> 原判据 `hits >= 1`——只要**有一个**成员有该方法就选它。对 `Optional[C]` 是对的
>（成员是 `[C, None]`，运行期有 `guard_not_none` 兜底），但真·多类联合 `C | D` 里 D 没有该方法时
>也会被选成 C，降级层直接发 `call C.m(x)`（无运行期 tag 检查）→ D 的实例按 C 的布局读字段，
>**越界读/静默错值**。
>
> 修法：统计「非 None 成员数」，要求 `hits == classes` 才选；否则走 unknown，
>codegen 明确报「不支持」而不是静默错值。
>
> 效果（`/tmp/tcaudit/` 的三个探针）：
>
> | 探针 | 修前 | 修后 |
> |---|---|---|
> | `r10_union_attr` | 静默错值 | **显式 codegen 错误** |
> | `r11_union_method_mixed` | 静默错值 | **显式 codegen 错误** |
> | `r12_union_layout` | 静默错值 | **显式 codegen 错误** |
>
> 闸门：套件 **600**、difftest **543-0**、check_size。
>
> **未修（已记录）**：
> 1. `r13_union_missing` / `r15_union_oob` 仍走错路径（这两条的方法解析在别的分支）；
> 2. **`assert` 窄化不是块作用域**——循环体/`try` 体里的 `assert x is not None` 在块未执行时
>    仍把窄化泄漏到块之后，可导致段错误或静默错值（探针 `p2`/`p8`/`r16`）。这是更严重的一条，
>    修它要改窄化的恢复点登记机制（`stmt_forms.cpp` 的 assert + 各块体的 `stmts_impl`），
>    不在本轮的上下文余量内。
## [0.677] - 2026-11-27

> refactor(opt)：**把「哪条指令可能抛异常」收敛成一个共享谓词** —— `opt_may_raise()`。
>
> 起因是 0.676 修的那两个 bug：`float→int` 转换会抛（cvttsd2si 的 `INT64_MIN` 哨兵检查），
而 `fold` / `loop`(LICM) / `global_promote` / `ifconv` **各自维护一份判断**，两处漏了、一处仍在漏。
>
> ```cpp
> // src/opt/detail.h 声明，src/opt/fold.cpp 实现
> bool opt_may_raise(const Inst& in, const IRFunction* fn);   // fn == nullptr → 保守口径
> ```
>
> - **保守口径**（拿不到操作数类型）：`Convert` 目标为任何整数种类都算可能抛；
> - **精确口径**（有 fn）：目标 `Int` 且源是 float/unknown/取不到 → 抛；
>   目标定宽整数（I8..U64）是裸 cvttsd2si + 截断，不抛；目标 Bool/Float 不抛；
>   调用族、`IndexLoad`/`IndexStore`、`Setjmp`、`Bin` 的 Div/Mod/FloorDiv → 可能抛。
>
> 四处接入后的保守度：`fold` / `loop` / `global_promote` **逐字不变**；`ifconv` **收紧**
>（原来「目标 Int/Bool 一律收」是最后一处潜在漏判，现在只拒真正的 float→int）。
>
> 验收：套件 **600**；全量 difftest **543 例 0 不一致**；`tests/opt_levels.sh` **40/40**
>（13 用例 × O0/O1/O2 + fast-math，覆盖 backend 的 6 处 `opt_ >= 1`）；
>12 基准**无一项退化**（最差 binary_trees −0.12%，方向为变好）。
>
> 约定（写进 `detail.h` 注释）：今后出现新的「可能抛异常」形态，**只改这一处**。
## [0.676] - 2026-11-27

> fix(opt)：**两处 -O2 语义错误**（同一根因：`float → int` 转换被当成纯指令）。
>
> 来自优化遍审计（`src/opt/` 25 个 pass 的变换安全性）。
>
> ### #1 LICM 外提了会抛异常的 `int(x)`
>
> ```python
> def f(n: int, x: float) -> int:
>     while i < n:
>         y = int(x)        # ← 被外提到循环之前
>         i = i + 1
> # loop 0 次 + x = inf：
> #   -O0 → 15    -O2 → 99（凭空抛 OverflowError 被外层 catch）
> ```
>
> `hoistable` 的 Convert 分支只要求「数值类型对得上」，但 codegen 对 `cvttsd2si` 的 `INT64_MIN`
>哨兵做检查，NaN / ±inf / 越界会抛 ValueError / OverflowError。`fold.cpp` 的 `is_pure` 早为 DCE
>修过这一点，**没有传播到 LICM**。修法：目标 Int 且源 Float 的 Convert 一律不外提。
>
> ### #2 `promote_globals_in_loops` 漏掉 Convert → 展开后读到陈旧全局值
>
> ```python
> g = 0
> def f(n: int, x: float) -> int:
>     global g
>     while i < n:
>         g = g + 1
>         y = int(x + float(i))   # ← 抛异常，展开跳过循环出口的写回
> # -O0 → r=1 g=1     -O2 → r=1 g=0（静默错值）
> ```
>
> `is_raising_op` 列了除零等，但漏了 Convert。修法：`case IrOp::Convert: return in.ty->kind == Int`
>（保守：这里拿不到操作数类型，多拒绝几个 int→int 只是少一档优化）。
>
> 两条都有回归用例：`tests/run/licm_float_convert.py`、`tests/run/global_promote_convert.py`
>（`-O0` 与 `-O2` 输出现在一致，`difftest` 从此能钉住这两个形态）。
>
> 审计同时**证伪**了一条怀疑（`rc_redundant_pairs` 的 1b 形态夹调用）：200 万次抛异常穿过该形态，
>`--asan -O2` + `detect_leaks=1` **0 leak**、RSS 与 -O0 相同，说明另有帧展开清理兜住。
>
> 套件 **600**（新增 2 个用例）。
## [0.675] - 2026-11-27

> docs：确认并记录一个对「怎么验证优化遍」很关键的事实——
>**所有 IR 优化遍只在 `-O2` 跑**。
>
> ```c
> // src/opt/opt.cpp:12
> void optimize_ir(IRModule& m, int opt_level, int loop_opt, bool inline_opt) {
>     if (opt_level < 2) return;
> ```
>
> 推论（三条，都影响测试策略）：
>
> 1. **`-O0` 与 `-O1` 的产物相同**（除了 `src/lower/` 里 2 处 `opt_level >= 2` 的门控，
>    例如 SIMD min/max 也只在 -O2 开）；
> 2. 因此 `tools/difftest.sh`（比对 `-O0` vs `-O2`）**覆盖了 `src/opt/` 的全部 25 个 pass**——
>    任何 pass 改错语义都会在 difftest 里暴露成输出不一致；
> 3. 反过来：**只在 `-O1` 与 `-O2` 之间才会出现的差异，现有测试抓不到**（因为 -O1 不跑 pass）。
>    以后若要加「中间档」优化，必须同时加 `-O1` vs `-O2` 的对拍。
>
> 这条也解释了为什么今晚两轮 codegen 融合（0.666 / 0.667）都必须跑全量 difftest 而不是抽样。
## [0.674] - 2026-11-27

> perf(memory)：**小 list 改用 4 槽内联块** —— gcbench RSS **47.8 MB → 41.4 MB（−12.8%）**，
>性能零退化。
>
> `PYADAMAS_LIST_INLINE_CAP` 是 **8**，所以每个「请求容量 ≤8」的 list 都按 176B 分配
>（头 64 + 结构 48 + 内联 8×8），哪怕它只有 2 个元素——**多预留 48B**。而我之前文档里按 4 算的
>（写成了 `data(32)`），把这块开销漏掉了一半。
>
> 改法：按**请求容量**选块——`1..4 → 4 槽块（144B）`，`0 或 5..8 → 8 槽块（原路径）`，`>8 → 外部 data`。
>`req==0`（`temp=[]` 再 append 构建）必须留在 8 槽，否则每轮多一次增长+拷贝。
>元组/定长填充路径（`pa_list_new_filled` / codegen `gen_tuple_inline` 的硬编码偏移）**完全没动**。
>
> | 指标 | 修前 | 修后 |
> |---|---|---|
> | RSS（3 次） | 47,660 / 47,892 / 47,896 KB | **41,100 / 41,756 / 41,696 KB** |
> | churn 1M（绑核 7 轮交错） | 56–58 ms | 56–58 ms |
> | 12 基准 instructions | — | 最差 fannkuch **+0.20%**，其余持平或变好 |
>
> 回归：套件 **598**；**全量 difftest 541-0**；**ASan 5 例 0 报错**（含池化路径，
>`detect_leaks=1`）；perf-baseline **12 项 0 fail**（fib −16.19%、nqueens −0.57% 仍在）。
>
> 改动 121 行，只动分配策略/参数，**不碰对象布局与 RC/GC 簿记**。
## [0.673] - 2026-11-27

> docs：索引条目与文档状态对齐，并给「下一步该做什么」留一份总览。
>
> `docs/index.md` 里 `mem-rc-header.md` 的描述已按最终结论更新（**不要单做 48 字节**，
>合并到第二步 dtor/release 表化 → 32 字节）；`exc-path-performance.md` 的描述补上
>「方案 B 与分配侧均已证否」。
>
> 夜间累计（0.644 → 0.673，57 个提交）的结论汇总：
>
> | 项 | 结论 | 位置 |
> |---|---|---|
> | GC 循环引用 | ✅ 8601 ms → 82 ms | 0.650 |
> | 协程调度 | ✅ 1610 ms → 180 ms | 0.652 |
> | fib 调用开销 | ✅ 指令 −16.2% | 0.666 / 0.667 |
> | nqueens 回归 | ✅ 448M → 297M 指令 | 0.669 |
> | `or`/`and` 类型错值 | ✅ 已修 | 0.672 |
> | 异常方案 B（LSDA 缓存） | ❌ 时间 0 收益，已回退 | `docs/exc-path-performance.md` |
> | 异常分配侧池化 | ❌ 反而慢 1.3%，已回退 | 同上 |
> | RC 头 48 字节 | ❌ 两个硬冲突（`marked` 存 young 下标；RSS 只 −6.7%） | `docs/mem-rc-header.md` |
> | 异常方案 A（同帧直达 LP） | ⬜ 施工图已备（80–150 行） | `docs/exc-path-performance.md` |
## [0.672] - 2026-11-27

> fix(typecheck)：**`or`/`and` 的 int/float 混合结果被静态推成 float**（静默错值）。
>
> ```python
> print(1 or 2.5)      # 修前：1.0    CPython：1
> print(0 and 2.5)    # 修前：0.0    CPython：0
> print((0.0 or 9)*2) # 修前：18.0   CPython：18
> ```
>
> 数值相等但类型不同——`repr`、`print`、`isinstance(x, int)` 全都不同。
>
> 根因在 0.620：为修 `x = "" or 0.0` 打印成 `0`，把 join 类型为 float 的值统一 `coerce` 成 float，
>于是「选中的操作数本来是 int」也被转成了 float。
>
> 修法：`TypeChecker::type_of(BoolOp)` 在**操作数混有 int 与 float** 时返回 `unknown`——
>结果类型本来就是动态的（`1 or 2.5` 是 int、`0 or 2.5` 是 float），交给 unknown 域：
>int 原样、float 装箱，运行期按实际值分派。纯 int / 纯 float 的 boolop 不受影响。
>
> 用例 `421_boolop_mixed_numeric.py` 的最后一行原本钉住的是**旧的非 CPython 行为**（`18.0`），
>已改成 `18` 并在文件头写明：**标注为 float 的返回槽**仍按静态槽转 double（`9.0`，这是既有口径），
>但没有标注、没有静态槽的表达式是动态类型。
>
> 套件 **598** 全绿；**全量 difftest 541 例 0 不一致**。
## [0.671] - 2026-11-27

> **记录一个既有错值**（不是本轮引入）：`or` / `and` 的 int/float 混合会让结果变成 float。
>
> ```python
> print(1 or 2.5)    # pya: 1.0     CPython: 1
> print(0 and 2.5)   # pya: 0.0     CPython: 0
> print(3 or 4.5)    # pya: 3.0     CPython: 3
> ```
>
> 用 0.665 的二进制（早于 0.669 的 boolop 单槽快路径）复测输出完全相同，**确认是 0.620 引入的**：
>那时为了修 `x = "" or 0.0` 打印成 `0`，把 join 类型为 float 的值统一 `coerce(v, vt, ty)` 成 float，
>于是「选中的那个操作数本来是 int」也被转成 float。数值相等，但 `repr`、`print`、
>`isinstance(x, int)` 都不同——属**静默错值**，不是纯外观差异。
>
> 0.669 的单槽快路径只覆盖 `ty` 为 int/bool 的情形，**没有改变这个行为**（已用旧二进制对拍确认）。
>
> 正确的修法方向（未做，需要动类型层）：`TypeChecker::type_of(BoolOp)` 在操作数混有 int 与 float 时
>返回 `unknown`，让值走动态域（int 保持 int、float 装箱）——这正是 0.620 之前 unknown 域的做法；
>代价是这类表达式的求值要走一次动态分派，收益是语义与 CPython 一致。
>
> 之所以不在本轮改：动 `type_of` 的影响面覆盖所有用到 BoolOp 类型的地方（赋值、返回、参数、
>容器元素），凌晨上下文不足时改它，风险远大于收益。**已记为待办。**
## [0.670] - 2026-11-27

> docs：**内存效率的测量与评估**（`docs/mem-rc-header.md`，未实现）。
>
> `PyAdamasRc` 是 64 字节/对象（`next/prev/rc/dtor/release/size/kind/marked` 各 8 字节）。
>对 gcbench 的 20 万个 2 元素 list：头部合计 **12.8 MB / 47.3 MB RSS ≈ 27%**；
>而单个对象体只有约 40 字节，所以头部占了单个对象的 62%。
>
> 可压缩项与代价（都写进文档了）：
>
> | 字段 | 处理 | 代价 |
> |---|---|---|
> | `size` / `kind` / `marked` | 打包进一个 8 字节字（size 32 位足够） | 读写的掩码，改动面小 |
> | `dtor` / `release` | 按 kind 查表/switch | 释放路径多一次 load，构造点删两行赋值 |
> | `next` / `prev` | **不能动** | 单链会让 `rc_unlink` 从 O(1) 退化成 O(n)，把 0.650 修好的悬崖种回去 |
>
> 建议先做 64 → 48 字节那一步，验收口径：RSS 降 ≥8% 且 churn 不退化；头布局改动必须全量套件 + ASan。
## [0.669] - 2026-11-27

> perf(backend)：**修掉 0.620 引入的 boolop 50% 指令膨胀** —— nqueens `448.1M → 297.0M` 指令。
>
> 二分定位（`git bisect run`，从 0.482 的 `44ca045` 到 0.668）：
>
> ```
> f644ac5 fix(expr): or/and 的 float 分支装箱（0.620）
>         两个槽：tmp 原样存值供判真，tmp_out 存动态域表示（float 装箱）
> ```
>
> 0.620 为了修 `x = "" or 0.0` 打印成 `0`，给**每个**操作数都写两个槽（判真槽 + 动态域槽），
>落到 float 还要 `pa_any_from_f64` 装箱。可 join 类型是 int/bool 时（nqueens 的
>`if not board[i] and col != j` 这种），两个槽存的是同一个值——纯多一次 store，还多一个栈槽。
>
> 修法：`need_two_slots` 只在 join 类型**不是** int/bool 时才成立；否则两个槽合并成一个。
>语义不变（float/动态域仍走原路径）。
>
> | | 指令数（`perf stat`） |
> |---|---|
> | 0.482 基线 | 298,740,130 |
> | 0.620 之后 | 448,066,824 |
> | **0.669** | **297,042,997** |
>
> 比基线还低 0.57%（其余改动累积）。
>
> 语义验证（与 CPython 逐字节一致）：
>
> ```python
> print("" or 0.0)     # 0.0
> print(repr(0.0 or "")) # ''
> print(False or 0.0) # 0.0
> print([] or 0.0)    # 0.0
> print(0.0 and 5)    # 0.0
> ```
>
> 套件 **598** 全绿。
## [0.668] - 2026-11-27

> chore(gate)：跑 `tools/checkall.sh --full` 全量闸门，**其余全过、perf-baseline 失败一处**，
>定位为**陈旧基线**而非今晚引入。
>
> ```
> 闸门              状态    备注
> build            PASS   4:15（cmake -j4）
> suite            PASS   598 ran ok / 0 失败
> difftest         PASS   541 例 0 不一致
> check_size       PASS
> bench-12x3       PASS   36/36 逐字节一致
> vect-variants    PASS
> perf-baseline    FAIL   nqueens +49.99%（298.7M → 448.1M）
> ```
>
> **nqueens 那 50% 不是今晚的改动**：用 0.665 的二进制（`perf6` worktree，早于 0.666/0.667 的融合）
>编同一个 `bench/compare/nqueens.py`，指令数是 **450,606,992**，与当前 448,066,824 同量级。
>而 `tools/perf_baseline.txt` 的写入时间是 **2026-11-24T21:23（HEAD `44ca045`，约 0.482）**——
>也就是说 nqueens 在 0.482 之后的某个版本里涨了 50%，与今晚无关。
>
> 其余 11 个基准都在阈值内，其中 **fib −16.19%** 正是 0.666/0.667 的收益，闸门如实记录。
>
> 待办（下一轮或之后）：
> 1. 在 `44ca045..0.665` 之间对 nqueens 二分，找出 50% 的来源；
> 2. 找到后要么修，要么在 `--write-baseline` 人工确认下刷新基线；
> 3. `tools/perf_baseline.txt` 已陈旧 6 天，刷新前 perf-baseline 闸门会一直红。
## [0.667] - 2026-11-27

> perf(codegen)：**直接调用的整数返回值直接参与紧邻的 Bin** —— 指令数再降 **6.0%**，
>累积（0.665 → 0.667）**50.04M → 41.96M（−16.2%）**。
>
> 上一轮 0.666 之后，递归路径上还剩下这一对：
>
> ```
>  8.48 : mov %rax,0x40(%rsp)     ← 第二次调用返回值落临时槽
> 13.82 : mov 0x40(%rsp),%rax     ← 立刻读回
> ```
>
> 现在 `CallDirect` 的返回值只要被紧随其后的整数 Bin 使用一次，就直接留在 `%rax` 参与运算。
>
> | 指标 | 0.665 | 0.666 | 0.667 |
> |---|---|---|---|
> | 指令数 | 50,042,554 | 44,657,490 | **41,964,972** |
> | cycles:u | 19.80M | 16.99M | **16.70M** |
> | 对 Go（39.86M 指令） | 1.26× | 1.12× | **1.053×** |
>
> 实现（5 个文件，全在 `backend/`）：`fuse.cpp` 加 `can_fuse_call_result_bin()`（允许中间夹 ≤2 条
>`ConstInt`）、`gen_bin_int` 在右操作数已在 `%rax` 且**运算符满足交换律**时换序原地算、
>`gen_call_direct` 在目标就是 `%rax` 时跳过 `movq %rax,%rax`。
>
> 边界形态**仍走老路径**（都有反汇编证据）：`x - f(x)`（非交换律右操作数）、`f(x) > 5` / `if f(x):`
>（Cmp/条件）、float 返回（`%xmm0`）、结构返回（sret）、中间夹 `LoadSlot`、返回值被两次使用或跨块。
>
> **诚实记录**：指令数 −6.0%，但 cycles 只 −1.8%，墙钟差在本机噪声内。删掉的两条指令不在关键路径上
>（fib 受递归依赖链限制，额外 uops 本来就被乱序执行藏掉了）。不绑核时测到 14–16ms、绑核 5–6ms，
>频率/调度噪声 2.5×，所以时间只认绑核交错中位。
>
> 我独立复测：**41,964,972 条指令 / cycles 16,696,999**（与子代理的 41,964,943 / 16,686,368 一致）。
>
> 回归：套件 **598**；**全量 difftest 541 例 0 不一致**。
## [0.666] - 2026-11-27

> perf(codegen)：**整数临时值不再「先写栈槽、立刻读回」** —— fib(30) 指令数 **−10.8%**、
>用户态中位 **−12.8%**。
>
> perf 指令级定位（`perf annotate -s pyadamas_fn_1fib_1`，91.6% 采样在这个函数里）：
>
> ```
>  1.44 :  mov %rax,0x28(%rsp)     ← 实参 1 存栈
>  2.22 :  mov 0x28(%rsp),%rdi     ← 立刻读回
>  8.48 :  mov %rax,0x40(%rsp)     ← 结果存回槽
> 13.82 :  mov 0x40(%rsp),%rax     ← 立刻读回返回寄存器（单行最高）
> ```
>
> 每次递归多 4 条访存，每对还有 5 周期 store-forwarding 延迟。
>
> 改动：`backend/fuse.cpp` 新增 `can_fuse_bin_value_use()`——把既有的「整数 Bin 结果单次使用、
>紧邻消费时留在 `%rax`」判据（原来只认「下一条也是 Bin」）扩到两种消费者：**CallDirect 的实参**
>和**本块 Return**。
>
> 安全性（不碰 ABI）：值只在相邻两条指令间活（`use_count==1 && !cross_block`）；实参装载前没有
>指令碰 `%rax`（栈实参走 `%r10` 中转，ptag 编组会中转 `%rax` → 该路径被 `call_ptag_words>0` 关掉）；
>只放行 int/bool（无 RC/GC 所有权、无多字返回）；被调方仍按原 ABI 收参。
>
> | 指标 | 修前 | 修后 |
> |---|---|---|
> | 指令数（`perf stat`，稳定到个位） | 50,042,554 | **44,657,490** |
> | cycles:u | 19.80M | 16.98M |
> | 用户态中位（7 轮交错） | 17.96 ms | **15.66 ms** |
>
> 指令数我在合并后**独立复测**过：44,657,490，与子代理的 44,657,497 一致。
>
> 回归：套件 **598** 全绿；**全量 difftest 541 例 0 不一致**（backend 改动必跑）。
>
> 两点记录：
> - README 里「比 Go 慢 3×」在参考机上没复现（Go 在这里约 13ms 而非 5.4ms）；指令数是跨机器可比的：
>   Go 39.86M vs pya 修前 50.04M（1.26×）→ 修后 44.66M（**1.12×**）。
> - 还有一块可测余量（**未做**）：`CallDirect` 结果直接喂给下一条 Bin，手改汇编量到上限 **−16% 指令**；
>   需要改 `gen_bin` 的操作数装载，更热更险，留作下一轮。
## [0.665] - 2026-11-27

> fix(driver)：**依赖有语法/词法错误时，`check` 不再报 ok**（审计 A3-1，重做）。
>
> ```
> # badep.py: def f(          ← 语法错
> # usesbad.py: import badep
>
> 修前: $ pyadamas check usesbad.py
>       ok: usesbad.py (2 statements, ...)        exit=0
> 修后: $ pyadamas check usesbad.py
>       /tmp/a3probe/badep.py:2:1: error: expected parameter name, found 'newline'
>       exit=1
> ```
>
> 根因：`check_load_file` 的 `catch (...)` 把 `ParseError`/`LexError` 一起吞成「模块缺失」，
>于是 check 说 ok、build 才在编译到那个文件时炸。
>
> 这次的做法与 0.663 撤掉的那版**刻意不同**——只做这一件事，不碰同名不同文件、不碰槽冲突：
>- `check.cpp` 单独 `catch (const ParseError&/LexError&)` → 打到原文件原行列 + 计数；
>- `build.h` 暴露 `check_hard_errors()`，`main.cpp` 在加载 import 图后据此返回 1。
>
> 之所以重做而不是留着：这正是「静默放过」的典型——依赖都解析不了，主文件当然不可能编过。
>
> 四口径复测：syntax 626/626、default 507、old 509、**imports 173**（与改动前逐项相同），
>说明这条修复没有把任何 stdlib 文件误伤。
## [0.664] - 2026-11-27

> fix(typecheck)：**定义了 `__getattr__` 的类，未知属性不再误报**。
>
> `xmlrpc.client.ServerProxy.currentTime`、`pyexpat._ClosedParser.ErrorColumnNumber` 这类读取，
>静态成员表里当然没有——它们由 `__getattr__` 在运行期合成。原来直接报
>`'ServerProxy' object has no attribute 'currentTime'`，是误报。
>
> 修法：实例属性解析在报错前检查类和基类是否定义了 `__getattr__` / `__getattribute__`
>（`ot->cls->method()` 会沿基类链查），命中就返回 `unknown`。
>
> 顺带把四口径基线的 delta 定位清楚了（用 0.646 的二进制对同一批 626 个文件逐文件 diff）：
>
> ```
> old(0.646): 512 OK / 114 FAIL
> new(0.664): 507 OK / 119 FAIL
> 由 OK 变 FAIL: 5 个文件，全部是同一条新诊断
>   'X.y' is an instance method; take the value on an instance   ← A8-5（0.657）
> 由 FAIL 变 OK: 0 个
> ```
>
> 这 5 个（`importlib/abc.py`、`multiprocessing/queues.py`、`multiprocessing/resource_sharer.py`、
>`test/typinganndata/ann_module695.py`、`urllib/request.py`）**不是 bug**：它们在 CPython 里合法
>（`SomeClass.method` 返回普通函数），但 pya 的降级层本来就不支持「把实例方法当值取出来」，
>以前只是 `check` 没发现、`build` 才报。A8-5 把这个不一致收掉了，于是
>**`default` / `old` 两个口径下降是「检查更诚实」，不是能力退化**。
>
> 遗留：`_ClosedParser.ErrorColumnNumber` 仍报错（该类的 `__getattr__` 可能来自 C 扩展，
>静态查不到），属同一族但未修。
## [0.663] - 2026-11-27

> **revert**：撤掉 A3 在 `check.cpp` 的三处改动——它在 stdlib 上制造了大规模误报。
>
> 起因：合并 A3 后我跑了 `tools/frontend_baseline.sh`（四口径），`imports` 从 **174/626**
>掉到 **117/626**。逐文件诊断后确认是两个新检测的误报：
>
> | 误报 | 数量 | 根因 |
> |---|---|---|
> | `module 'io'/'enum'/'stat'/'codecs' resolves to two different files` | 83 | 内建模块首次加载走 `builtin_module`，记下的 path 是空的；第二次解析到 `.py` 就必然「不同」 |
> | `method slot N of '_PdbClient' is used by both '__init__' and 'cmdloop'` | 45 | 槽号是**每次编译自己的 intern 表**编号，跨来源不可比；`_PdbClient.__init__` 与继承来的 `Cmd.cmdloop` 都可能是 slot 1 |
>
> 第二个尤其值得记：`pdb.py` 用 `build` 编译是**通的**，只有 `check` 报冲突——说明不是真冲突，
>是 check 侧把「本模块编译的类 / 导入接口的类 / 继承来的方法」混在一张槽表里比较。
>
> 处理：`src/driver/check.cpp`、`src/driver/build.h`、`src/main.cpp`、`tests/check_imports.sh`
>整体退回 `654b9e5`（A3 之前）。A3-1（依赖语法错误传播）本身是对的，但它和另两处在同一批文件里，
>等重新实现时会连它一起回来。
>
> 复测（`tools/frontend_baseline.sh`）：
>
> | 口径 | A3 后 | 回退后 | 基线 |
> |---|---|---|---|
> | syntax | 626/626 | 626/626 | 626 |
> | default | 507 | 507 | 516 |
> | old | 509 | 509 | 518 |
> | **imports** | **117** | **173** | 174 |
>
> `default`/`old` 仍各差 9 个，**尚未定位**——需要拿 0.646 的二进制对同一批文件做逐文件 diff。
>已记为待办，不在本轮猜测。
>
> 教训：这批改动只跑了过滤子集（子代理不能跑全量），**四口径基线才是唯一能兜住这种全局误报的尺子**。
>今后任何动 `check` 侧的改动都必须附一次 `frontend_baseline.sh` 的前后对比。
## [0.662] - 2026-11-27

> docs：**审计 B2 补（同类残留）+ B4（索引可达性）**。
>
> B2 补（13 文件）：除了点名的 8 处，把同一类「源码搬迁后文档没跟上」的残留一次清掉
>（`lower/method.cpp` → `lower/method/method.cpp`、`lower/builtin/*`、`backend/target_abi.h` →
>`common/target_abi.h` 等）。
>
> 其中 `exc-semantics.md §4.8` 不只路径旧，**结论也早变了**——原文说「跨模块自定义异常类
>不能出现在 except/raise 名字位置」，实测（`lib.py` 的 `LibError` 带 `self.msg`）从 `lib` 导入后
>`raise` / `except as e` 全正常，是 0.178 修掉的。改成【已修（0.178）】，保留仍不一致的点号形态
>（`import lib; except lib.LibError as e` 能按名匹配接住，但 `e` 只按 Exception 基类视图绑定）。
>文档开头的「已知不一致 10 条」同步成 9 条。
>
> B4（`docs/index.md` +4/−2）：不照抄审计的「8 篇未收录」，而是从 index 做 BFS 实测——
>真正不可达只有 3 篇（`refactor-plan.md`、`stdlib-batch2.md`、`stdlib-batch6.md`）。根因是
>区间写法「stdlib-batch1 ～ stdlib-batch7」只有首尾可点，已展开成六个链接并注明「批次 5 不存在」。
>改后 BFS **0 篇不可达**。
>
> 两处判定为「不修」并写明理由：`cffi-design.md` / `cpybridge-design.md` 里的
>`typecheck/import.cpp` 是 **Codon 源码路径**不是本仓库；`simd*.cpp` 是 0.480 已删除的特化模板
>（该文首行自带「历史存档」）。
## [0.661] - 2026-11-27

> docs：**审计 B3（5 处指向不存在的用例）**逐条核对后修正。
>
> | 文档 | 审计说 | 实测 | 处理 |
> |---|---|---|---|
> | `mi-layout.md:100` | 引用 `tests/strict/mi_non_primary_base_fields.py` | 该文件在 `268201a` 被删（提交说明：它钉的是 0.503 的拒绝行为，现在允许），且**结论也反了** | 改成 0.510 删除 + 指向 `mi_two_state_bases.py` / `mi_primary_base_fields.py` |
> | `str-codepoints.md`（4 处） | 引用 `163–166_codepoint_*.py` | 这四个名字**从来没存在过**（`git log --all` 无记录） | 前三个 → `176_codepoint.py`（0.382 合并自这四个）；`166_codepoint_re_file.py` 无对应用例 → 删引用并注明 |
> | `compatibility.md:727/742` | 引用 `159_exc_multi.py` / `158_isinstance_bytes.py` | 0.382 合并进 `155_exc_value.py` / `135_hetero_tuple.py`（两文件第 2 行有合并记录） | 改成「0.382 合并进 …」 |
> | `frontend-fixes.md:145` | 引用 `tests/unsupported/…` | 实际在 `tests/strict/` | 改目录 |
> | `testing.md:68` | 引用 `big_round.py` | 实际是 `big_round_powmod.py` | 改名 |
>
> 另外把「文件在、内容早已改写」单独扫了一遍：改完后 `docs/*.md`（不含 archive）里所有
>`tests/…py` 引用，只剩 `HANDOFF.md` 的 `tests/run/NNN.py` 占位符（按 AGENTS.md §7 不动）。
>替换目标的内容也抽验过（`155_exc_value` 有「原 159_exc_multi」段、`135_hetero_tuple` 有
>「原 158_isinstance_bytes」段、`176` 覆盖 len/index/methods）。
## [0.660] - 2026-11-27

> feat(tools)：**文档链接检查**进闸门——`tools/check_docs_links.py`。
>
> 审计 B2 的 8 处坏引用全是「源码搬迁后文档没跟上」，这类漂移没有机制防就必然复发。
>新工具扫 `docs/**/*.md` 的 `[x](path)` 相对链接（266 个），任何指向不存在文件即失败，
>已接进 `tests/check_size.sh`。
>
> 两个必要的豁免（都写在代码注释里）：
> - **跳过代码片段**：`[0](a)`、`list[int](pyobj)` 这类 C++/伪代码会被正则误抓，
>   先按围栏块与行内 code 等长抹掉再扫；**裸词**（无扩展名、无路径分隔符）也跳过。
> - **跳过 `docs/archive/`**：冻结的历史归档，里面的失效链接是历史事实。
>
> 顺手修掉唯一一处真问题：`docs/str-codepoints.md:4` 把行号写进了链接目标
>（`[roadmap.md](roadmap.md:141)` → `[roadmap.md](roadmap.md)（第 141 行）`）。
>
> （B2 本身在 0.659，这条只是补的工具与收尾。）
## [0.659] - 2026-11-27

> docs：**审计 B2（8 处指向已搬迁/已删除的源文件）**逐条核对后修正。
>
> 多数是模块拆分后留下的旧路径：`src/lower/builtin.cpp` → `src/lower/builtin/builtin.cpp`、
>`method_str.cpp` → `method/method_str.cpp`、`import.cpp` → `module/import.cpp`、
>`backend/tag_defs.h` → `common/tag_defs.h` 等。有两处**同一个坏路径在全文出现多次**
>（`str-codepoints.md` 7 处、`native-stdlib-cookbook.md` 12 处），一并改掉——只改审计点到的那一处
>会留一堆坏引用。
>
> 0.480 已随特化模板删除的两个文件（`backend/simd.h`、`src/backend/simd.cpp`）按文档性质分别处理：
>- `modularization.md` 那句是**现在时**描述 → 从列表删掉；
>- `simd-vectorization-design.md` 是**历史设计存档**（文首自带「0.480 代码已删除」）→ 保留原句，
>  就地补一句「0.480 已随特化模板一起删除」。
>
> 同类残留在审计清单外还有 8 处（`cffi-design.md`、`compile-speed.md`、`cpybridge-design.md`、
>`exc-semantics.md`、`lang-gaps-landing.md`、`next-round-plan.md`、`stdlib-batch1.md`），已记录待下一批收。
## [0.658] - 2026-11-27

> docs：**驱动层审计 B1（7 处「与代码矛盾」）**逐条核对后修正 6 处，1 处判定不成立。
>
> | # | 审计说 | 实测结论 | 处理 |
> |---|---|---|---|
> | 1 | `PYADAMAS_TYPE_TAGS` 文档标「默认关 / opt-in」，与代码矛盾 | 成立：`main.cpp` 是 `=1`/`=0` 两档，`program_opts.cpp` 在未设环境变量时**扫源码**，出现 `isinstance` 就自动开 | 改成「默认按需自动开」，补上第三个读取点 |
> | 2 | `experiment-switches.md` 标题写「0.481」，表里却有 0.522–0.531 | 成立 | 改成「0.481 起；表内最新条目 0.531」 |
> | 3 | 文档里的 `PYADAMAS_KEEP_ASM=1` 并不存在 | 成立（`src/`/`runtime/`/`tools/` 全无），敲了静默无效 | 改成真实机制：`build/<preset>/modcache/*/mod.s` |
> | 4 | `index.md` 同篇文档两行、数字不同 | 成立（重复行 + 与 §0 的 7 个矛盾） | 删重复行，并入 27 行 |
> | 5 | `index.md` 「当前口径」写的是 0.470 的 4/9 | **部分成立**：数字是历史口径不算错，措辞错 | 改成「0.470 口径」并注明 0.479 收尾的 5/12 |
> | 6 | `big-landing.md` 的 `build/pyadamas` 路径过时 | 成立但属 0.357 历史记录 | 数字原样保留，只给「稳定候选」那句加限定 |
> | 7 | `vectorization-coverage.md` 7 个 vs 8 个 | **不成立**：文档第 3 行已显式声明 §1–§9 是 0.418/0.423 存档、当前以本节为准 | 未改 |
>
> 性能百分比（36/36、−45.31% 等）一个没动——拿不到实测就不凭印象改。
## [0.657] - 2026-11-27

> fix(driver+typecheck)：**check 与 build 的口径收敛**（审计 A3 三处 + A8-5）。
>
> ### A3-1 依赖语法错误被 check 吞掉
>
> ```
> 修前 check: ok: usesbad.py (2 statements, ...)   exit=0   ← 依赖 badep.py 第 1 行语法错
> 修前 build: badep.py:1:7: error: expected parameter name, found ':'
> 修后 check: badep.py:1:7: error: expected parameter name, found ':'   exit=1
> ```
>
> `check_load_file` 把 `LexError`/`ParseError` 一起吞成了「缺失模块」；现在单独 catch、
>报到原文件原位置，并计入 `errors`。
>
> ### A3-2 同名不同文件
>
> 同一个模块名解析到两个不同文件时，build 拒绝而 check 放行。现在 check 在
>「解析出路径」与「缓存命中」两处比对模块名 → 规范路径（缓存命中只在 importer 变化时重解析）。
>
> ### A3-3 槽冲突检测只在 build
>
> 按结构对齐把同一份 `add_slot` 检测补进 `check.cpp`。**这一条没有活例**：槽号是进程内按方法名
>intern 的，同名不同槽只能来自「接口来自另一个进程的缓存」，而 `reserve_imported_method_slots`
>在方法 intern 之前就会先报冲突——`add_slot` 够不到。补它是为了**防止两份平行实现继续漂移**；
>真要根治得把两份扁平导出合并（审计 A6）。
>
> ### A8-5 同一件事三种文案 → 一条
>
> | 形态 | 修前 | 修后 |
> |---|---|---|
> | `g = C.get` | codegen 层报错，check 放行 | `'C.get' is an instance method; take the value on an instance` |
> | `C2 = C; g = C2.get` | 另一条文案 | 同上（一字不差） |
> | `g = kk.K.get`（跨模块） | 报在**调用点**：`takes 0 positional arguments but 1 were given` | 同上 |
>
> 修法：`typecheck/expr_attr.cpp` 的类接收者分支取到 Normal 且非装饰的实例方法时直接给诊断，
>不再 `mk_method_value` 后各走各的。静态/类方法/装饰方法不受影响（类属性先命中），
>`obj.m` 的绑定方法值也不受影响。
>
> ### 影响面与验证
>
> - `load_check_lookup` 加 `errors` 出参（driver 内部 API，唯一调用点 main）；未动 `Program` 结构、
>   未动缓存键；
> - `tests/check_imports.sh` 4 项 → **6 项**（新增两条负例），6/6 通过；
> - 套件 **598**；未跑 `frontend_baseline` 四口径——check 现在会在上述两类拒绝，数字可能变化，
>   下次跑基线时需留意。
>
> 未做（需扩功能）：跨模块**直接未绑定调用** `mod.K.get(obj)` 仍报「takes 0 positional arguments」，
>根因是接口只导出 `call_sig`（不含 self）。
## [0.656] - 2026-11-27

> fix(lower)：**类对象上解析不到的属性从「静默给 0」改成编译期报错**（审计 A8-5 同类）。
>
> ```python
> import kenum
> print(kenum.Color.__members__)   # 修前：0   修后：error: class 'Color' has no attribute '__members__'
> print(kenum.Color.__qualname__)  # 修前：0   修后：同上
> ```
>
> 单文件里 `print(Color.__members__)` 一直是编译期报错，跨模块却落到运行期 `AttrGet` → `0`
>（类对象没有属性字典，`AttrGet` 只能给 0）。修法：`lower/expr_assign.cpp` 的 Attribute 读
>记下 `from_class_obj`（接收者静态类型是 `ClassObj`），静态解析不到属性时直接报错，不再落 `AttrGet`。
>
> 扫过 22 个 dunder（枚举类对象，本地/导入两形态）：除 `__name__` 外全部从 `0` 变成与单文件一致的报错；
>**没有**误伤 `__str__`/`__repr__`（它们在类对象上本来就报错，实例上的照旧）。
>
> 顺带挡掉一个**既有崩溃**：类对象上读 `@property`（`mod.K.p`）会把类对象当 self 传 getter，
>改前 `exit 139`；现在同样报 `class 'K' has no attribute 'p'`。
>
> 新增 `tests/strict/class_obj_unknown_attr.py`（`build` 必须拒绝）。套件 **598**。
>
> 遗留（既有，未修）：CPython 里 `mod.K.<property>` 返回 property 对象本身，pya 不建模 property 对象，
>现在给的是明确报错而不是静默值。
## [0.655] - 2026-11-27

> fix(typecheck)：**`Color('RED')`（int 值枚举传 str）从运行时段错误变成编译期报错**。
>
> ```python
> from enum import Enum
> class Color(Enum):
>     RED = 1
> print(Color("RED"))
> # 修前：Segmentation fault (core dumped)，exit 139
> # 修后：error: enum 'Color' expects an int value, got 'str'
> ```
>
> ASan 单例确认**不是内存管理错误，是读错地址**：`SEGV in pa_str_eq`（`rdi=0x9`）——
>`coerce(str -> int)` 静默放过，比较指令按 vreg 的 str 类型生成 `pa_str_eq`，
>把成员值 `1` 当串指针读串头。
>
> 修法：新增 `TypeChecker::check_enum_call()` 集中做「恰好一个位置实参」+「int 值枚举
>只收 int-like（unknown 放行）」，本模块 `Color(...)` 与跨模块 `mod.C(...)` 两个分支都走它，
>`from kenum import Color` 的调用点一并覆盖。降级层没动——typecheck 报错后不再进降级。
>
> `Color(2).name` → `GREEN`、`Color(99)` → 仍运行期 `ValueError`、`Color(True)` → `RED`（同 CPython）。
>
> 新增 `tests/fail/enum_ctor_arg.py`。套件 / check_size 见下。
## [0.654] - 2026-11-27

> fix(iface)+feat：**跨模块 enum 可按值构造、可迭代、可 `len`**（审计 A8-4）。
>
> ```python
> import kenum
> print(kenum.Color(1))        # 修前：error: 'Color' takes no arguments → 修后：RED
> print(len(kenum.Color))      # 修前：object of type 'type[Color]' has no len() → 3
> for m in kenum.Color: ...    # 修前：'type[Color]' object is not iterable
> ```
>
> 改动：
> - 类记录里加 `is_enum` + `enum_members`（名 → 字面量值，复用 `enc/dec_default_value`），
>   `kModuleFormatVersion 19 → 20`；`build.cpp` / `check.cpp` 两份扁平导出同步；
> - `lower_construct` 里的 enum 反查抽成 `lower_enum_by_value`（本模块/导入类通用），
>   `construct_imported` 开头认 `is_enum` 直接走它（不能走 ctor wrapper——那会新建实例，
>   而 `Color(1)` 必须返回已存在的成员）；
> - `len()` 对 enum **类对象**返回成员数（本模块与导入类同一判断）。
>
> 口径说明：`len(Color)` 在单文件里原本也不支持（对 CPython 是缺口），这次一并补上，
>保持单文件与跨模块一致；非 enum 类对象仍按原样报错。
>
> `isinstance(mod.Color.RED, mod.Color)` 本来就对（靠导入类的 qualname 元数据），本次没为它加代码。
>
> 新夹具 `tests/modules/enum_xmod` + `tests/run/enum_len_iter.py`。套件 **598**。
>
> 顺带记录的既有口子（**不是本次引入**，单文件同样复现）：
> - 类对象上的未知 dunder（`mod.Color.__members__`）在读侧静默给 `0`，而单文件是编译期报错；
> - `Color('RED')`（给 int 值枚举传 str）能编过，运行时段错误——待修。
## [0.653] - 2026-11-27

> fix(iface)：**类属性进接口**，跨模块 `mod.C.X` / 实例 `c.X` 可读（审计 A8-3）。
>
> ```python
> # kattr.py
> class C:
>     LIMIT: int = 42      # 无标注 = 类属性（带标注的是字段，见 0.647）
>     NAME: str = "hi"
>
> # main.py
> import kattr
> print(kattr.C.LIMIT)    # 修前：error: class 'C' has no attribute 'LIMIT'
> print(kattr.C.NAME)     # 修后：42 / hi
> ```
>
> 改动链条（5 个文件，运行期没动）：
>
> 1. `ClassAttrInfo` 增加 `external_symbol`（定义模块里那个隐藏全局的符号名）与 `writable`；
> 2. `build.cpp` / `check.cpp` 的扁平类导出按 C3 MRO 收集 `class_attrs`（同名方法不导出成类属性），
>    符号用 `module_global_symbol(声明模块, "__pa_cattr_<类>_<属性>")`；
> 3. 接口在 `match_args` 之后加段（name/type/writable/symbol），`kModuleFormatVersion 18 → 19`；
> 4. `lower/class.cpp` 新增 `class_attr_decl_slot()`：带 `external_symbol` 的走 `extern_global()`，
>    引用定义模块的全局——原来无条件按本模块分配，导入方读到自己那份空槽（`0` / 空串）；
> 5. 类对象上的类属性**写**（`mod.C.X = v`）给出明确诊断：
>    `class attribute 'Base.X' of an imported class is read-only across modules`。
>
> A8-1 没有被放回去：带标注的 `LIMIT: int = 42` 仍是字段，`mod.C.LIMIT` 仍报错。
>
> 新夹具 `tests/modules/class_attr_xmod`：类对象读、实例读、from-import、继承来的类属性、
>定义模块 bump 后导入方读到新值。套件 **596**。
>
> 已知残余（edgefix 实测）：本地子类覆盖**导入基类**的类属性时，基类方法里 `self.X` 仍读基类那份
>（`ncow.D().gx()` 给 11，CPython 给 5）。机制是基类方法在定义模块编译，看不到跨模块子类；
>要修需要运行期按类号查属性，属新机制，未做。
## [0.652] - 2026-11-27

> perf(async)：**就绪队列 pop 从 O(n) 改 O(1)** —— 协程基准 `1.61 s → 0.18 s`（8.9×）。
>
> 复现（`/src/luo-proj/coroutine/pya/main.py`）：20000 个任务 × 20 次 `await sleep(0)`。
>
> perf 定位：**85.4% 的时间在 `__memmove_evex_unaligned_erms`**。
>
> ```c
> void* ready_pop(void) {
>     void* c = (void*)loop_ready->data[0];
>     memmove(&loop_ready->data[0], &loop_ready->data[1],   // ← 每次 pop 前移整个数组
>             sizeof(int64_t) * (size_t)(loop_ready->len - 1));
>     loop_ready->len--;
> }
> ```
>
> 20000 个就绪任务时每次 pop 要搬 ~160KB，40 万次切换 = 数十 GB 的 memmove，
>这正是「1 万任务 439 ms、2 万任务 1542 ms」超线性的来源。
>
> 改成**头指针 + 批量压缩**：O(1) 取头，只在「头指针过半且 ≥1024」时压缩一次（均摊 O(1)）。
>已消费槽位显式清零（`loop_ready` 是 GC 根，留着旧指针会让调度过的任务被错误保活）；
>清零不 `dec`，因为引用已转移给调用方（与原实现一致）；`list_clear_owned` 跳过 0 槽位。
>
> | 基准 | 修前 | 修后 | 对照 |
> |---|---|---|---|
> | coroutine 20K×20 | 1610–1620 ms | **180–190 ms** | Go 315–423 ms、CPython 691–834 ms |
>
> 套件 **595**；ASan（4 个 async 用例 + 协程基准本身）全部 `errors=0`。
## [0.651] - 2026-11-27

> merge：edgefix 分支的三个**静默错值**修复（来自 `edgeprobe/REPORT.md`）。
>
> - **MI 显式基类调用**：`C.m(obj)` 直调不做多继承字段块平移，`A.__init__(self); B.__init__(self)`
>  导致 B 的字段覆盖 A 的（`2 0 3` → `1 2 3`）；新增 `explicit_self_type()` + `mi_block_offset`。
> - **`del` 作用域**：`del x` 后访问原来打印 `0`，现在编译期拒绝 `name 'x' is not defined (deleted above)`，
>  删除可见性继承进同作用域嵌套块。
> - **异构实例 repr**：无公共基类的实例列表 `print` 出裸指针；dunder 槽表扩成 `[eq,lt,hash,del,str,repr]`，
>  新增 `pa_instance_str` / `pa_instance_default_str`，四个 unknown 出口全部接上。
>
> 套件 594；ASan（三个新用例 + 两个 dunder 用例）全 0 报错。

## [0.650] - 2026-11-27

> perf(gc)：**动态元素容器的 unknown 探测从 O(n) 降到 O(1)**——循环引用基准 `8601 ms → 82 ms`（105×）。
>
> perf 指令级定位：93.7% 周期在 `unknown_is_heap()` 的 `mov (%rax),%rax`（遍历 GC 堆链）。
>`list[unknown]`/`set[unknown]`/`dict[unknown,*]` 的元素走动态探针，而编译器在 exact-RC 模式下
>关掉了索引 → 每次 append/释放 O(堆大小) → O(n²)。修法：`ir_module_needs_unknown_index` 里，
>只要出现元素域为 unknown 的容器就保留索引。A/B 对照（未标注 list append 2M）24–27ms vs 25–26ms，无可测回归。

## [0.649] - 2026-11-27

> fix(driver)：重复的子模块 import 让入口**永久缓存 miss**（审计 A4）。`from pkg import a` 写两遍 →
>`u.deps` 出现重复项，而 manifest 读回是 map 语义，`deps.size()` 恒不等。两处去重后 manifest 恢复单条。

## [0.648] - 2026-11-27

> fix(typecheck+lower)：跨模块 `mod.K.cm()` / `mod.K.sm()` 全部失败（审计 A8-2）。两处都只认裸 Name 接收者，
>补上「接收者静态类型是 ClassObj 且写法是属性链」。实测 5/6/7/5 与 CPython 一致。

## [0.647] - 2026-11-27

> fix(typecheck)：跨模块 `mod.C.<字段名>` 静默读到垃圾值（审计 A8-1，`-1`/空串）。
>`is_class_receiver()` 只认裸 Name，`a2.C` 走实例属性分支按实例偏移在类对象上读槽。
>补 `from_class_obj` 后与单模块一致：`class 'C' has no attribute 'LIMIT'`。

## [0.646] - 2026-11-27

> fix(driver)：`case` 分支体里的 `import` 不进依赖图（审计 A1）。`scan_imports_stmt` 的 switch 漏了 `Match`；
>修前 build 报「cannot resolve module」+ 误导性第二错，check 却报 ok。

## [0.645] - 2026-11-27

> fix(match)：映射模式键判重。`{0.0, -0.0}` 与 `{10**20, 10**20}` 原来漏判，
>`{2**53+1, 2**53}` 反而误报（double 精度丢失）。整数改精确十进制、`-0.0` 归一化（须在取负之后）、Big 参与判重。

## [0.644] - 2026-11-27

> fix(ir)：跨模块类引用（tag 29）的接口生命周期显式化。`Dec::loaded` 是局部对象，`Type::cls` 是裸指针；
>当前靠 Program 持有 ModuleInfo 才没炸。加 `ModuleInfo::imported_refs` 把隐式约定变成显式所有权。
>「`unordered_map` 致 interface_hash 非确定」**不成立**：编码遍历的三张表都是 `std::map`。

## [0.643] - 2026-11-27

> 订正 0.642 的**假阳性记录**：用例里的 `a, *b = [1, 2, 3]` 一直是红的（星号解包赋值 codegen 未实现），
>我用 `grep -c 'ran ok'` 数通过数把它掩盖了。教训：通过数必须与失败列表交叉核对。

## [0.642] - 2026-11-27

> fix(syntax)：`with (C(), C()):` 无 `as` 的多上下文管理器（原来被当元组，报「需要 __enter__」）；
>星号赋值目标 `*a = [1,2]` 现在明确拒绝（PEP 8/语法）；裸 `except:` 不在末尾时报错。

## [0.641] - 2026-11-27

> fix(match)：字面量模式边界。裸 `*` 不再静默当 `*_`；补 `bytes` 字面量；负 Big（`case -10**20:`）可匹配；
>`case 1 + 2j:` 支持（保留成 BinOp 交给降级求值，因为 Constant 的 Complex 实部固定 0）。

## [0.640] - 2026-11-27

> fix(syntax)：lambda 的 keyword-only 默认值原来塞进 `defaults`（`lambda a, *, b=2` 报「缺 b」）→ 改 `kw_defaults`；
>lambda/classdef 重置 `async_depth_`/`loop_depth_`；`/` 的位置与次数校验；`lambda **kw,:` 尾逗号；
>`annotation_mentions_big` 补 ListExpr。

## [0.639] - 2026-11-27

> fix(syntax)：`f"{x=:.1f}"` 自文档化表达式强行设 `!r` → 运行期 `Invalid format specifier for str`；
>只有无格式说明符时才默认 `!r`。f-string 表达式里的 `#` 注释（PEP 701）跳过（限定在表达式部分，
>格式说明符里的 `#` 是标志位）。`genexp_depth_` 置位时机提前到解析元素之前（PEP 530 的 `(await f(x) for ...)`）。

## [0.638] - 2026-11-27

> fix(desugar)：`[sum(x + y for y in ys) for x in xs]` 原来报 `undefined name 'x'`——内层生成器要捕获外层
>推导式的循环变量，而推导式是就地展开的。`has_lambda` 现在把 GeneratorExp 本身当信号，外层提升成生成器。
>混合装饰器顺序（`@m.outer` + `@deco` 得 11 而非 20）：只有整栈都是未知简单名字装饰器时才走 aliases 路径。

## [0.637] - 2026-11-27

> fix(desugar)：作用域扫描补三类绑定——`match` 的 case 模式捕获、海象运算符 `:=`、`del`。
>修前 `if (range := my_range) …` 之后的 `range(3)` 仍被当内建改写成计数循环。

## [0.636] - 2026-11-27

> fix(verify)：`FuncAddr`/`FuncCode`/`ClassAddr` 的目标在 `imm` 却按 `idx` 校验（被 `idx<0` 跳过，等于没查）；
>补 `ConstBig`；槽位指令的负下标不再无条件放行；`Branch` 缺 false 目标不再重复报错；补 `vreg_ty` 边界检查。

## [0.635] - 2026-11-27

> fix(ir)：索引分析三处口径——裸 unknown 全局在 `can_skip_index_maint` 被放行（与 `needs_unknown_index` 冲突，UAF 风险）；
>`ListAppend` 的元素实参被误标为「纯容器用途」；`ClassAddr`/`FuncAddr`/`FuncCode` 的 `idx_kind_ok` 映射错位。

## [0.634] - 2026-11-27

> fix(ir)：5 个 asyncio 类型（Task/Future/AsyncEvent/AsyncLock/AsyncQueue）跨模块编码**穿透成 ClassObj**（tag 26），
>导入方会把 `asyncio.Lock` 当构造器。并入「按 unknown 编码」组。

## [0.633] - 2026-11-27

> fix(minmax)：生成器路径手写 `IrOp::Cmp` 而没走 `take_new`，big 元素按堆指针比较 → 全 big 时错误结果、
>混合 int/big 时段错误。统一改走 `lower_cmp_single`；unknown/big 元素类型明确拒绝；空序列 `Raise` 后补终结指令。
## [0.558] - 2026-11-26

> fix(typecheck)：**两条 C 互操作的坑改成编译期明确报错**（原来是段错误 / 误导信息）。
>
> ### 1. lambda 作 C 回调 → 段错误（A 类）
>
> ```python
> c.qsort(xs, 3, 8, lambda a, b: a[0] - b[0])
> # 修前：Segmentation fault（lambda 形参无类型，体内 a[0] 降级成通用索引，把裸指针当 list 读）
> # 修后：error: lambda cannot be used as a C callback: its parameters have no types, so
> #        pointer indexing inside it would be miscompiled; define a named function with
> #        ptr[T] parameters
> ```
>
> lambda 在脱糖后是 `__pyadamas$lambda_N`，形参**无从标注**；命名函数（含捕获闭包）是完整可用的，
>所以按「拒绝 lambda、保留命名函数」处理。回调的 C 侧跳板机制本身没问题。
>
> ### 2. `buffer_i32` / `buffer_f32` 传 `ptr[int]` / `ptr[float]` 的报错误导
>
> 修前：`argument 'p' of 'memset' expects list[int] for 'ptr[int]', got 'buffer[int]'` ——
>用户看到「expects list」完全猜不到真实原因是**元素宽度**（4 字节 buffer 冒充 8 字节指针会越界读）。
>修后：
>
> ```
> error: argument 'p' of 'memset' cannot take this buffer: it has 4-byte elements,
>        but list[int] for 'ptr[int]' is 8-byte; pass the bare 'ptr' (with a byte count)
>        or use buffer(n) (8-byte elements)
> ```
>
> 两条都加进反例集：`tests/fail/callback_lambda.py`、`tests/fail/buffer_ptr_width.py`
>（配合 `tests/fail/bufwidth.cpya` 的最小声明）。
>
> 基线：syntax 621、default 500、old 505、imports **168**（不降）。
> 闸门：套件 562 / difftest 509/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.557] - 2026-11-26

> fix(typecheck)：**`Optional[T]` 上的方法调用不再误报**（`T` 只要真有这个方法）。
>
> ```python
> d: dict[str, int] | None = None
> print(d.get("k"))     # 修前：error: 'dict[str, int] | None' object has no method 'get'
> ```
>
> 原来只有 `Optional[Class]` 与 `Optional[Match]` 两种情况会「剥壳按内层查」，其余内置类型
>（dict/list/str/bytes…）一律按联合类型去找方法 → 找不到 → 误报。标准库里 5 个文件是这一簇
>（`_module_completer.py` 的 `dict|None.get`、`taskgroups.py` 的 `list|None.append`、`smtplib.py` 的
>`bytes|None.decode` 等）。
>
> 修法：把剥壳条件放宽成「**内层类型真的有这个方法**」，并新增「联合接收者挑能接住这个方法的成员」
>（多命中取第一个，返回类型可能不精确但不会误报）。运行期本来就有 `guard_not_none` 兜底：
>`Optional` 接收者的方法调用会先检查 None，为空则给出明确 panic，不是段错误。
>
> 基线：syntax 621、default 500、old 505、**imports 167 → 168**（↑1）。
> 闸门：套件 562 / difftest 509/0 / check_size PASS。
## [0.556] - 2026-11-26

> fix(lower)：**函数值写属性给出可执行的错误信息**（外部清单的 J1）。
>
> ```python
> def f() -> int:
>     return 1
> f.calls = 0
> print(f.calls)
> ```
>
> 修前：`error: unsupported assignment target`（不知道发生了什么）。
> 修后：
>
> ```
> error: function values have no runtime attribute dict;
>        use a closure cell (nonlocal counter) or a module-level dict instead
> ```
>
> 函数值在我们的实现里是**编译期实体**（静态闭包：code + 捕获清单，没有运行期字典），
>`f.attr = v` 无法编译成动态属性写。这是刻意的设计取向（J1 同时记在「设计限制」一栏），
>所以本轮只把错误信息改成**说明原因 + 给出替代方案**，不引入运行期字典。
>
> 替代写法（文档里也这么建议）：闭包 cell 计数（`nonlocal`）或模块级 dict。
>
> 基线：syntax 621、default 500、old 505、imports 167（不降）。
> 闸门：套件 562 / difftest 509/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.555] - 2026-11-26

> feat(driver)：**`@dataclass` 按需自动开**——必填字段不再丢参（外部清单的 N9）。
>
> ```python
> from dataclasses import dataclass
> @dataclass
> class P:
>     x: int      # 无默认值
>     y: int
> p = P(1, 2)     # 修前：error: 'P' takes no arguments (no __init__)
> print(p.x, p.y, p)   # 1 2 / P(x=1, y=2)
> ```
>
> `@dataclass` 的编译期展开（合成 `__init__`/`__repr__`/`__eq__`）一直挂在 `PYADAMAS_LANG_GAPS` 门控后面，
>而 lang_gaps 会改语义（file 协议泛化、异常类当值等），默认全开风险大。
>
> 修法与 0.528 的 type_tags 同一套「按需自动开」口径：**源码里出现 `dataclass` 才开**，三态控制——
>`PYADAMAS_LANG_GAPS=1` 强制开、`=0` 强制关、未设按需。
>
> 关键约束（实测出来的）：**只对项目文件自动开**。标准库/CPython Lib 里提到 dataclass 很常见，
>给它们开门控会改变诊断口径（626 基线 default 500 → 495、old 505 → 499、imports 167 → 165），
>所以 build 与 check 两条路径都加了「项目文件」判定。
>
> 配套：
>- `tests/run/dataclass_auto.py`（不设环境变量，覆盖必填字段 + 默认值 + `repr` + `__eq__`）；
>- 测试框架新增 `# pyadamas: no-lang-gaps` 指令（显式关掉门控），`tests/fail/f79_dataclass_requires_gate.py`
>  改用它——它测的是「门控关掉时仍然拒绝」，语义不变；
>- `program_wants_type_tags` / `program_wants_lang_gaps` 从 `build.cpp` 拆到 `driver/program_opts.cpp`
>  （`build.cpp` 已到 1219 行，硬限制 1200）。
>
> 基线：syntax 621、default 500、old 505、imports 167（不降）。
> 闸门：套件 **562** / difftest 509/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.554] - 2026-11-25

> fix(ir)：**`ptr[float]` 作类字段跨模块**——`celem`（C 元素种类）现在进接口。
>
> 复现：
>
> ```python
> # ndmod.py
> class Arr:
>     data: ptr[float]
>     def __init__(self, n: int) -> None:
>         self.data = c.malloc_f(n * 8)
> # main.py
> def fill(a: ndmod.Arr) -> None:
>     a.data[0] = 1.5      # 修前：IndexError: list assignment index out of range
> ```
>
> `ptr[T]` 在类型系统里是 **`Optional[T]` + `celem`**（C 元素种类，决定下标寻址宽度）。接口编码只写了
>`Optional[int]` 的内层类型，**`celem` 丢了** → 导入方拿到的字段类型是「普通 `Optional[int]`」，
>`a.data[i] = v` 走**列表下标写**路径 → `IndexError`。同模块内 `celem` 还在，所以一直正常。
>
> 修法：`enc_type` 对 `Optional` 且 `celem != None` 的写 **tag 46 + celem 字节 + 内层类型**；
>`dec_type` 的 `case 46` 复原 `celem`。`kModuleFormatVersion` **17 → 18**。
>
> 回归 `tests/modules/ptr_field_xmod/`：模块内构造 `Arr`、**在导入方函数里**做 `a.data[i] = v`、
>回到定义方读回（`1.5 4.25`）。单用例 ASan `detect_leaks=1` 干净。
>
> 基线：syntax 621、default 500、old 505、imports 167（不降）。
> 闸门：套件 **561** / difftest 508/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.553] - 2026-11-25

> fix(ir)：**Deque 跨模块 tag（30）恢复可用**——根因是 fall-through 落错 case，不是编解码不对称。
>
> ```python
> # dmod.py: from collections import deque; d: deque[int] = deque(); d.append(1)
> #          def total(x: deque[int]) -> int: ...
> # main.py: import dmod; print(len(dmod.d), dmod.total(dmod.d))
> ```
>
> 0.548 把 tag 30 加进了 `enc_type` 的一串 **fall-through** case 末尾（`TypeObj` / `AsyncTimeout` /
>`AsyncTaskGroup` / `AsyncSemaphore` / `Exception` / `Complex` 共用同一个 `e.u8(0); break;`）。
>插在 Deque 之前的那些 kind 于是**全部写成了 tag 30 + 一个元素类型**，而解码侧把 30 当 Deque 消费一个元素
>—— 接口整体错位：`types.NoneType`、`types.CoroutineType`、`_collections_abc.*` 这些 `TypeObj` 全被解成
>`Deque`，于是 `asyncio/coroutines.py:46` 的 `isinstance(obj, _COROUTINE_TYPES)` 报「第二个参数不是类」。
>
> 修法：给 Deque **单独**一个 case（`e.u8(30)` + 元素类型 + `break`），把 `TypeObj` 那组恢复成 `e.u8(0); break;`。
>
> 这也解释了之前所有「矛盾」的实验结论：
>- 单个 `d: deque[int]` 用例能过（它确实编码正确）；
>- dec 侧改成 `t_unknown()`（不消费元素）就能过（错位消失）；
>- 只影响 `asyncio/coroutines.py`（它的 `import types` 会把 `types` 模块的 `TypeObj` 全局全部解坏）。
>
> 回归 `tests/modules/deque_xmod/`：跨模块读 `d[0]`、遍历、在对方模块追加后重算、本方构造再传入。
>
> 基线：syntax 621、default 500、old 505、imports **167**（不降）。
> 闸门：套件 **560** / difftest 508/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.552] - 2026-11-25

> fix(driver/check)：**项目代码导入编译器 stdlib 时按完整接口解析**——`check -I .` 与 `run` 的口径终于一致。
>
> ```python
> # cmmod.py: from contextlib import contextmanager; @contextmanager def managed(...)
> # main.py:   with cmmod.managed('x') as v: ...
> check -I . main.py   # 修前：error: module attribute is not callable（run 正常）
>                     # 修后：ok
> ```
>
> 根因：`check_load` 对标准库模块**一律返回 nullptr**（0.523 的设计，避免用不完整的 CPython 接口大面积误杀——
>那条红线是「626 文件 default 499 → 223」换来的），于是项目代码 `from contextlib import contextmanager` 只拿到
>unknown，`@contextmanager` 装饰后的符号在 check 侧是 unknown、build 侧是 Func，两边口径不一致。
>
> 修法：把**请求方**纳入口径判断——
>- **项目代码**（不在 stdlib / 不在 `/python3.*` 下）导入编译器自带的 `stdlib/*.py` 时，按完整接口解析；
>- **标准库扫描**（626 文件那套，请求方也在标准库里）与 **CPython Lib** 保持 unknown，红线不变。
>
> 验证：13 个常见 stdlib 模块的 `import` 与 5 个 `from ... import ...` 在项目代码里 `check` 全部 ok（无误报）；
>`tests/modules/contextlib_xmod` 的 `check -I . main.py` 从报错变为 ok。
> 回归用例：`tests/run/ann_io_from.py`（项目模块 `from io import StringIO` + write/getvalue）。
>
> ## 顺带：Deque(30) 的决定性实验（不是撞号，也不是编解码问题）
>
> enc 开 tag 30、只改 dec：`case 30 = mk_deque(dec_type(d))` → asyncio/coroutines.py 报 isinstance 错；
>`case 30 = t_unknown()`（消费元素类型）→ ok；`case 30 = t_unknown()`（不消费）→ ok。
> 两个 unknown 变体都过 ⇒ **字节流没错位、编解码对称**；问题出在「解成真正的 `Deque` 类型之后」——
>该文件的 `_COROUTINE_TYPES` tuple 元素在类型合并/推断里被 Deque 参与后不再是「类」，即**类型检查侧**
>（`join` / tuple 元素推断）的问题。累计排除：① 编解码不对称 ② isinstance 的 tuple 元素本身
>③ 直接依赖带坏 ④ tag 撞号。tag 30 保持停用；下次收尾的起点已写进注释。

>
> 尝试过但**没有采用**的修法（已记录，避免重蹈）：在 check 模式下合成 `_GeneratorContextManager` 类——
>目标用例能过，但 default **500 → 496**、old **505 → 501**（4 个 stdlib 文件因为「装饰 dunder 方法」等被拒）。
>
> 基线：syntax 621、default 500、old 505、imports 167（与 0.551 持平）。
> 闸门：套件 558 / difftest 507/0 / check_size PASS / ctest 8/8。
## [0.551] - 2026-11-25

> feat(typecheck)：**内置模块里用 pya 源码实现的类，注解现在能解析**（`io.StringIO` / `io.BytesIO` 等）。
>
> ```python
> import io
> s: io.StringIO = io.StringIO()   # 修前：类型 unknown
> s.write("ab")                   # error: method 'write' is not supported by the code generator yet
> print(s.getvalue())              # 修后：ab
> ```
>
> 排查结论比预期简单得多：**不需要新 `TyKind`、不需要 tag、不需要动 runtime** ——
>`stdlib/io.py` 本来就用 pya 源码实现了 `StringIO` / `BytesIO` 两个类（file 协议面齐全：
>`read/readline/readlines/write/seek/tell/getvalue/close`）。缺的只是 `parse_annotation` 的 Attribute 分支：
>对内置模块只认几个硬编码名字（`random.Random`、`subprocess.*`、`re.*`、`asyncio.*`），其余 dotted 一律 unknown。
>
> 修法（约 4 行）：内置模块分支在硬编码名字之后**再查一次 `mod->classes`**，命中就 `mk_class(...)`。
>这样「内置模块里用 pya 源码实现的类」（`io.*`、`collections.*`、`contextlib.*` 同理）都能被注解解析。
>
> 跨模块**也直接可用**（StringIO 是普通 pya 类，走 0.540 的 class 引用机制），所以不需要 tag 46，
>也不需要格式版本 18。
>
> 回归：`tests/run/ann_stringio.py`（write/getvalue/tell/seek/read/close、`StringIO("hi").readline()`）、
>`tests/modules/textio_xmod/`（跨模块返回 `io.StringIO` 后 `getvalue()` + `read(1)`）。
>
> 基线：syntax 621、default 500、old 505、imports 167（不降）。
> 闸门：套件 **558** / difftest 507/0 / check_size PASS / ctest 8/8。
## [0.550] - 2026-11-25

> test(ir)：记录 **Deque(30) 跨模块 tag 的排除结论**（只加注释，无行为变化）。
>
> 0.548 停用 tag 30 是因为 `check --imports /usr/lib/python3.14/asyncio/coroutines.py` 报
>`isinstance() second argument must be a class or builtin type`。这轮把两个最可能的假设都验掉了：
>
> | 假设 | 实验 | 结论 |
> |---|---|---|
> | 编解码不对称 | 被 import 模块导出 `d: deque[int]` + `def f(x: deque[int]) -> int`，开 tag 30 后主模块 `len(dmod.d)` 与 `dmod.f(dmod.d)` 都正常 | **排除** |
> | `isinstance` 的 tuple 元素退化成非类 | `T = (types.CoroutineType, collections.abc.Coroutine); isinstance(x, T)` 用 `check -I .` 不触发 | **排除** |
> | 整文件 `check --imports` 才报 | 仍复现 | **未解决** |
>
> 结论：Deque 的 tag 编码本身没问题，问题出在 **asyncio 的 import 图**——某个被间接导入的模块接口在 tag 30 打开后
>解出的类表被破坏。下一步方向（已写进代码注释）：对 asyncio 的 import 链做子集二分，或在 dec 侧打印每个接口
>解出的类数对比。**tag 30 保持停用**，同模块内 `deque`/`deque[T]` 不受影响。
>
> 顺带记录两个本轮尝试过但**没有采用**的修法（都会让基线退化，已回退）：
>- 在 check 模式下合成一个 `_GeneratorContextManager` 类，让 `@contextmanager` 的类型在两个模式口径一致——
>  **已实测：default 500 → 496、old 505 → 501**（4 个 stdlib 文件因为「装饰 dunder 方法」等错误被拒），
>  所以不采用；
>- 按名字文本匹配 `contextlib` 的 Attribute 形式（保留在原代码里，是 0.540 已有的行为）。
>
> `check -I . main.py` 对 `tests/modules/contextlib_xmod` 仍报 `module attribute is not callable`（build/run 正常）——
>这是**已知的 check/build 口径差**，根因是 check 模式下 contextlib 的类表没被加载，待单独修。
>
> 基线：syntax 621、default 500、old 505、imports 167（与 0.549 持平）。
> 闸门：套件 556 / difftest 506/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.549] - 2026-11-25

> test(modules)：**补 7 个类型 tag 的跨模块用例**（0.548 的 tag 有实现、缺验收）。
>
> | 用例 | 打印的稳定字段 |
> |---|---|
> | `popen_xmod` | `Popen(["true"]).wait()` → `0` |
> | `struct_time_xmod` | `time.gmtime(0)` → `1970 1 1` |
> | `uname_xmod` | `os.uname()` 的 `sysname` 非空、`machine == machine` |
> | `float_info_xmod` | `sys.float_info.max > 0`、`dig > 0` |
> | `sys_flags_xmod` | `sys.flags.optimize >= 0` |
> | `stat_xmod` | `os.stat(__file__).st_size >= 0` |
> | `completed_process_xmod` | `subprocess.run(["true"]).returncode` → `0` |
>
> 全部**只打印稳定字段**（不打印随机/环境相关的值），所以可重复对拍。
>
> 两个踩到的坑（写用例时注意）：`len(uname_result)` 不支持（用字段判空代替）；
>`os.stat("dmod.py")` 在套件里会失败——模块用例是在**临时目录**里跑的，必须用 `__file__`。
>
> 基线：syntax 621、default 500、old 505、imports **167**（不降）。
> 闸门：套件 **556** / difftest 506/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.548] - 2026-11-25

> feat(ir)：**交叉模块类型 tag 31–45**——15 个内置类型跨模块可带类型身份（不再是 unknown）。
>
> | tag | 类型 | | tag | 类型 |
> |---|---|---|---|---|
> | 31 | `DateTime` | | 39 | `UnameInfo` |
> | 32 | `TimeDelta` | | 40 | `FloatInfo` |
> | 33 | `Path` | | 41 | `SysFlags` |
> | 34 | `Regex` | | 42 | `StatResult` |
> | 35 | `Match` | | 43 | `CompletedProcess` |
> | 36 | `Popen` | | 44 | `Random` |
> | 37 | `Buffer`（元素类型 + 宽度） | | 45 | `Range` |
> | 38 | `StructTime` | | | |
>
> 跨模块用例（`tests/modules/*_xmod/`）：datetime、timedelta、path、range、buffer、regex、match、random
>——**句柄类都真的在导入侧调了方法**，不是只赋值。
>
> **tag 30（Deque）被停用**：打开它会让 `check --imports /usr/lib/python3.14/asyncio/coroutines.py` 从 ok 变成
>`isinstance() second argument must be a class or builtin type`（`_COROUTINE_TYPES` 那个 tuple 的元素类型在导入方
>不再是类）。二分定位到 tag 30 单点；按「不为一个文件放宽 isinstance 检查」的原则，Deque 跨模块退回 unknown
>——**同模块内的 `deque` / `deque[T]` 注解仍然可用**（0.544 的成果保留）。
>
> 仍保持 unknown 的（注释清单已写进 `enc_type`）：`File`、`DYLIB`、`VThread`、`Chan`（跨模块传裸句柄不安全），
>`Struct`/`StructPtr`（走 0.543 的 27/28），`.cpya` 的 `c_struct`（布局由 C 头决定）。
>
> 接口格式版本 **17**（0.543 的 struct 段漏 bump 已在 0.547 补成 16，本次再进到 17）。
>
> 基线：syntax 621、default **500**、old **505**、imports **167**（与 0.544 持平，不降）。
> 闸门：套件 **549** / difftest 506/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.547] - 2026-11-25

> feat(typecheck)：**默认参数支持常量表达式**（编译期折叠）。
>
> 修前：
>
> ```python
> def scale(x: int = 2 ** 10) -> int:
>     return x
> # error: default value of 'x' is not a literal constant;
> #        the code generator cannot fill it in yet
> ```
>
> 默认参数一直是「只收字面量」（接口序列化需要不依赖 AST 的表示），所以 `2 ** 10`、`1 << 4`、
>`"a" + "b"` 这些在 CPython 里完全正常的写法都被拒。
>
> 修法：`literal_of_expr` 对**两侧都是字面量**的二元运算做折叠，只保留确定不溢出、不依赖运行期的组合：
>- 整数：`+ - * % // << >> & | ^`（`+ - *` 用 `__builtin_*_overflow` 查溢出，越界即放弃折叠）与受限 `**`
>  （指数 0..62，逐步乘并查溢出）；除数/模数为 0 一律不折叠；
>- 浮点：`+ - * /`；字符串/字节串：`+`；
>- 其它情况（变量、调用、混合 u64 大字面量）仍返回「不是字面量」，错误消息不变。
>
> 回归 `tests/run/default_const_fold.py`：`2**10` → 1024、`1<<4 + 1.5*2.0` → 19.0、`"a"+"b"` → `ab`、
>`7%3 + 100//7` → 15；`x: int = n`（变量）仍被拒。
>
> 顺带：`kModuleFormatVersion` 从 15 补到 **16** —— 0.543 加了 struct 段却漏了 bump（我的疏漏），
>这会让旧 modcache 有被新解码器误读的风险。
>
> 基线：default 500/626、old 505、`--imports` 167（不降）。
> 闸门：套件 **541** / difftest 506/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.546] - 2026-11-25

> fix(typecheck)：**`Chan[T]` / `Chan` 注解**——跨模块 channel 从此可用。
>
> 外部验证的「跨模块对象协议第 8 例」：
>
> ```python
> # vtmod.py
> def producer(ch):
>     ch.send(42)        # error: method 'send' is not supported by the code generator yet
> # main.py
> ch = make_chan(int, 1)
> go vtmod.producer(ch)
> ```
>
> 同模块内正常，跨模块失败——根因还是**注解不认**：`ch` 没有标注时是 `unknown`，`send`/`recv` 在降级期
>找不到通道协议；而同模块内编译器能从调用点推出类型。
>
> 修法：`annotation.cpp` 的 Name 分支加 `Chan`（元素 unknown）、Subscript 分支加 `Chan[T]`（元素类型参与
>`send` 的值类型检查）。加上标注后跨模块 `go` + `send`/`recv` 正常：
>
> ```python
> def producer(ch: Chan[int]) -> None:
>     ch.send(42)        # 42
> ```
>
> 回归 `tests/modules/chan_xmod/`（跨模块 channel + 跨模块 `go`）。
>
> 顺带确认：**Decimal/Fraction 跨模块返回值的「打印机器地址」已经不复现**（0.544 的注解映射修好了）——
>`import nb; print(nb.dec_value())` → `0.3`、`Fraction(1,3)` → `1/3`。
>
> 基线：default 500/626、old 505、`--imports` 167（不降）。
> 闸门：套件 **540** / difftest 505/0 / check_size PASS / ctest 8/8。
## [0.545] - 2026-11-25

> fix(lower)：**值类型 struct 的「只声明」局部变量**（A 类段错误）。
>
> ```python
> struct Point:
>     x: float
>     y: float
> def f() -> float:
>     p: Point        # 只声明、不构造
>     p.x = 1.0       # 修前：Segmentation fault（写到地址 0）
>     return p.x
> ```
>
> 根因：`AnnAssign` 无初值时降级器直接 `break`（「槽本来就是 0（None）」）——对标量成立，
>但**值类型 struct 在槽里是指针**，0 就是空指针，字段写等于写地址 0。
>
> 修法：无初值但标注是值类型 struct（非 `.cpya` 的 `c_struct`）时，在声明处 `struct_new` 分配一块
>零填充 payload 并走正常赋值路径。语义与标量一致（**未初始化 = 零**），`p.x` 读回 `0.0`、写正常。
>
> 回归 `tests/run/struct_declare.py`：声明后写字段（3.0）、声明后读字段（0.0）。
>
> 这是 0.543 跨模块 struct 收尾时顺带发现的**既有 bug**（在 0.542 的基线上同样复现），不属于那次改动。
>
> 基线：default 500/626、old 505、`--imports` 167（不降）。
> 闸门：套件 **539** / difftest 505/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8；
>**全量 ASan 套件 539 ran ok、零报告**。
## [0.544] - 2026-11-25

> feat(typecheck)：**补全内置类型的注解映射**（10 个名字，`default`/`old` 基线各 +1）。
>
> 之前 `buf: buffer` 退化成 unknown（0.542 的 S2 段错误就是这么来的）。同类问题成片存在——
>这些类型**都有运行期实现**，坏的只是「注解不认识」：
>
> | 注解 | 映射到 |
> |---|---|
> | `deque` / `deque[T]` / `Deque[T]` | `mk_deque(unknown/T)` |
> | `datetime` / `date` | `TyKind::DateTime` |
> | `timedelta` | `TyKind::TimeDelta` |
> | `Path` | `TyKind::Path` |
> | `re.Pattern` / `re.Match`（dotted） | `t_regex()` / `t_match()` |
> | `subprocess.Popen`（dotted） | `t_popen()` |
> | `range` | `mk_range(int)` |
> | `buffer` | `mk_buffer(float)`（0.542） |
>
> 另外补了 `assignable` 的 Deque 规则（与 list 同口径），否则 `d: deque[int] = deque()` 会因为
>「元素 unknown vs int」被拒。
>
> 仍然保持 unknown 的（**不猜**）：`io.StringIO`/`BytesIO`/`IOBase`（没有对应 TyKind，运行期是 File 那套但
>不完全等价）；裸 `Generator`/`AsyncGenerator`/`Coroutine`/`Future`/`Task`（只有下标形式能表达产出类型，
>文档应写 `Generator[int]`）；`Match`/`Regex`/`Random`/`CompletedProcess` 的**裸名**（正常用法是从对应
>模块构造/调用返回，dotted 形式已覆盖）。
>
> 基线：syntax 621、**default 500**（↑1）、**old 505**（↑1）、imports 167 —— 有 stdlib 文件因为注解能认了而通过。
> 闸门：套件 538 / difftest 504/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。

## [0.543] - 2026-11-25

> feat(ir,typecheck,driver)：**跨模块 struct 导出**。
>
> ```python
> # smod.py
> struct Point:
>     x: float
>     y: float
> def make(a: float, b: float) -> Point:
>     p: Point = Point(0.0, 0.0)
>     p.x = a; p.y = b
>     return p
> def norm2(p: Point) -> float:
>     return p.x * p.x + p.y * p.y
> # main.py
> import smod
> p = smod.make(3.0, 4.0)      # 3.0 4.0
> print(smod.norm2(p))          # 25.0（按值传参）
> p.x = 6.0                     # 跨模块字段写
> print(smod.norm2(p))          # 52.0
> ```
>
> 实现：接口新增**用户 struct 段**（name / c_union / 每字段 name+type+offset+size / size / align）；
>`enc_type` 打开 **tag 27（值）/ 28（指针）**，按名字引用；解码用 `pending_structs` 回填 `Type::st`
>（struct 段在 globals **之后**解码，globals 里的 27/28 必须先登记）；`kModuleFormatVersion` **15 → 16**。
>
> 关键教训（上一轮一次性做完时踩的坑）：**`StructInfo` 必须由 `ModuleInfo` 拷贝持有**，不能借用定义模块
>`TypeChecker` 的对象（no-op deleter 的 shared_ptr → 运行时段错误）。`.cpya` 的 `c_struct` **不导出**
>（布局由 C 头决定，这是上一轮 4 个 stdlib OUTPUT DIFF 的根因）。
>
> 回归 `tests/modules/struct_export/`：字段读写、按值传参、跨模块修改后重新计算。
> **全量 ASan 套件 538 ran ok、零报告。**
>
> 顺带发现的**既有 bug**（不属于本次改动，单独修）：`p: Point` 只声明不构造、随后写字段 → 段错误
>（`Point(0.0, 0.0)` 构造后再写就正常）。已在 CHANGELOG 记录，另派。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 538 / difftest 504/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.542] - 2026-11-25

> fix(typecheck)：**`buffer` 作类字段的类型标注**（外部验证的 S2，A 类段错误）。
>
> ```python
> class W:
>     buf: buffer
>     def __init__(self, n: int) -> None:
>         self.buf = buffer(n)
> w = W(10)
> w.buf[2] = 1.5      # 修前：Segmentation fault
> print(len(w.buf), w.buf[2])
> ```
>
> 根因不是 RC（`ir -O0` 显示 `pa_rc_inc` 已经发了），而是**注解没映射**：`buf: buffer` 在
>`annotation.cpp` 里没有对应项，字段类型退化成 `unknown`；于是 `len(w.buf)` 按 list/str 头部解释，
>`w.buf[2] = 1.5` 走**列表下标写**路径，把 buffer 对象当 `PyAdamasList` 改写 → 段错误。
>
> 修法：`parse_annotation` 的 Name 分支加 `buffer` → `mk_buffer(t_float())` + `celem = CKind::Float`
>（与内置 `buffer(n)` 同类型）。纯类型推导，不动 runtime/RC。
>
> 回归 `tests/run/buffer_class_field.py`：跨语句（`len` + 下标读）、跨函数（`w.set/get`）、跨对象（互不影响）、
>传参后再读；ASan `detect_leaks=1` 零报告。
>
> ## 关于外部报的 S1（`ptr[float]` 变量下标写入 → 堆损坏）：**不是编译器 bug**
>
> `tests/run/clib.cpya` 的 `malloc_f(n)` 是 `@extern("malloc")`，**参数是字节数**。报告里的 `malloc_f(5)`
> 只分配 5 字节却写了 5 个 double（40 字节）：`p[0]` 落在 malloc 最小块（可用 24 字节）里不崩，
>`p[4]` 写到 +32 越过 chunk → `malloc(): corrupted top size`。**与下标是常量还是变量无关**
>（常量下标同样损坏）。按正确大小 `malloc_f(40)` 分配后，常量下标、while/for 变量下标、
>`malloc_i` 的 int 变量写入全部正确且 ASan 干净。
>
> 回归 `tests/run/ptr_float_store.py` 覆盖上述四种写法，并在用例里写明「参数是字节数」这个坑。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **531** / difftest 498/0 / check_size PASS / ctest 9/9。
## [0.541] - 2026-11-25

> fix(driver/check)：**`check --imports` 接上跨模块类引用的 loader**（与 build 同口径）。
>
> 0.540 给接口加了 **tag 29（跨模块类引用 = 归属模块 + 类名）**，但只有 build 侧注入了 loader；
>`check.cpp` 仍只走 `builtin_module` 兜底，导致 `check --imports` 遇到「别的模块的类」时落到 unknown，
>与 build 口径不一致。
>
> 改法：`check_load_file` 里 encode→decode 归一那一步给 `decode_module_interface` 传 loader 闭包，
>直接复用 check 自己的 `check_load(ctx, path, dep)`（**没有新写一套解析**）。
>
> 已知遗留（与本改动无关，单独排期）：`check -I . main.py`（`tests/modules/contextlib_xmod`）仍报
>`module attribute is not callable` —— 用 0.540 的旧二进制复现同一错误，所以是 **check 侧 info 构建**
>（`check_build_info`）没有导出「装饰后符号的类型」，不在 loader 这条线上。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 529 / difftest 496/0 / check_size PASS / ctest 8/8。
## [0.540] - 2026-11-25

> fix(ir,typecheck)：**跨模块类引用带归属模块**（接口格式 v15）+ **`@contextmanager` 跨模块可用**。
>
> 修前：
>
> ```python
> # cmmod.py
> from contextlib import contextmanager
> @contextmanager
> def managed(tag: str):
>     yield 1
> # main.py
> import cmmod
> with cmmod.managed("x") as v:   # error: with requires a class instance with __enter__/__exit__
>     ...
> ```
>
> 两层根因：
>1. **接口的类引用只写裸类名**（tag 13），解码时只查导入方自己的类表——别的模块的类
>   （`contextlib._GeneratorContextManager`）解析失败，静默退化成 `unknown`；
>2. `@contextmanager` 包装后的符号在**定义模块自己的 `types` 里就是 unknown**（装饰器的返回类型不可表达），
>   同模块内能过是因为降级器手里有函数定义，跨模块就只剩 unknown。
>
> 修法：
>- `enc_type` 新增 **tag 29 = 类引用(归属模块, 类名)**（同模块仍用 tag 13，`0..28` 逐字沿用）；
>- `Dec` 加 `loader` 回调 + 已解析缓存 + loading 集合（A↔B 互相引用可重入）；解析顺序：本模块类表 →
>  `loader(module)` → 仍无则保持 unknown（`PYADAMAS_DIAG_TYPEREF=1` 打一行诊断，不 panic）；
>- `@contextmanager` 按**名字文本**识别（不依赖符号类型——导入的名字本身就是 unknown），给被装饰符号
>  精确签名 `(*args: unknown) -> _GeneratorContextManager`，varargs 用现有 `enc_sig` 编码；
>- **`kModuleFormatVersion` 14 → 15**（旧 modcache 自动失效，首次全量构建慢一次）。
>
> 由此修好的是一整类场景（不再只是 contextmanager）：任何「返回值/参数是**别的模块的类**」的跨模块签名。
>
> 回归 `tests/modules/contextlib_xmod/`（跨模块 `@contextmanager` + 跨模块 `nullcontext` 返回值）。
>
> 顺带：`typecheck/func.cpp` 因这次改动涨到 1232 行、超过 1200 硬限制；把 `@contextmanager` 的类型推导
>抽成 `TypeChecker::contextmanager_func_type`（实现放 `call_modules.cpp`）并压缩注释，回到 1199 行。
>
> 已知未做：`check --imports` 侧的 loader 目前只走 `builtin_module` 兜底，跨模块引用别的模块的类时
>仍可能落到 unknown（有诊断打印），下一轮接上 `check_load`。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **529** / difftest 496/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8。
## [0.539] - 2026-11-25

> feat(runtime,lower)：**支持用户类 `__del__`**（RC 归零、释放前调用一次）。
>
> ```python
> class V:
>     def __init__(self, x: int) -> None:
>         self.x = x
>     def __del__(self) -> None:
>         print("destroyed", self.x)
> v: V = V(1)
> print("before")     # before → destroyed 1（与 CPython 一致）
> ```
>
> 实现：类元数据 dunder 表从 `[eq, lt, hash, -1]` 扩成 `[eq, lt, hash, del_slot]`（第 5 个 `.quad`，
>无实现填 -1），`pa_instance_dunder_fn` 的 `which` 上限 2 → 3；释放路径（`rc_leaf_free` 与
>`pa_rc_dec_slow`）在 dtor 之前调用 `__del__`。
>
> 异常兜底走 **C 侧 handler**：`pa_handler_push()` + `setjmp(h->env)`，捕获后
>`pa_exc_type_name/message` 取内容、`pa_exc_clear()` 清 pending、`pa_handler_leave(h)` 收尾；
>往 stderr 打一行 `__del__ raised <类型>: <消息>` 后**继续释放、不终止程序**。
>
> 实现时踩到并修掉的两个问题（值得记）：
>1. **必须在 `rc_unlink` 之前调用**：`inst_vtable` 依赖 `rc_index_has`，对象一旦从 RC 索引摘掉，
>   查 vtable 会得 NULL，`__del__` 静默不执行（第一版就是这个症状）；
>2. **不能无条件查**：每个实例释放都做一次 dunder 查表，`binary_trees` 实测 **+8.3%**。
>   改成程序级开关：codegen 发现「有类实现 `__del__`」时发射强符号 `pa_has_del_classes = 1`，
>   覆盖 runtime 的弱定义；没有 `__del__` 的程序只多一次全局读 → 12 基准回到 0.00%。
>
> 保证与不保证（写进 `docs/language.md`）：
>- 保证：RC 归零会调用；`__del__` 里读字段、`del self.x` 合法；异常打一行吞掉；同一对象至多一次；
>- **不保证**：GC 环回收**不调用**（与 CPython 的 PEP 442 不同）；`__del__` 里触发另一个对象析构时，
>  那一次的 `__del__` 会被跳过（`in_del_call` 防重入的代价）。
>
> 回归 `tests/run/del_method.py`（正常释放 / `__del__` 读字段 / 抛异常不终止 / 互相引用不崩），
>与 CPython 的可比部分逐字节一致。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **528** / difftest 496/0 / check_size PASS / bench 36/36 / perf warn=0 fail=0 / ctest 8/8；
>**全量 ASan 套件 528 ran ok、零 sanitizer 报告**。
## [0.538] - 2026-11-25

> feat(typecheck)：**`del <name>` 之后在同一块内再读会报错**（保守版）。
>
> 修前：`del y` 之后 `print(y)` 静默打印 **0**（CPython 是 `NameError`）。
>
> 修法（保守，不引入任何新增误杀）：`deleted_names_` 只在**当前 statements 序列**内有效——
>进嵌套块保存并清空、出块恢复；`declare()` 里 erase（重新赋值即复活）；只跟踪 `del <Name>`。
>
> ```
> y: int = 5
> del y
> print(y)      # error: name 'y' is not defined (deleted above)
> ```
>
> **仍然不报错的情形**（刻意，写进 `docs/language.md`）：
>- `del` 写在 `if`/`for`/`while`/`with`/`try` 体内 → 不跟踪；
>- `del` 之后在**另一个块**里读 → 不报错；
>- `del` 的是闭包变量（外层作用域）→ 不跟踪；
>- `del` 之后重新赋值再读 → 正常（复活）；
>- `del self.x` 这类属性目标 → 与原来一致（不跟踪）。
>
> 为什么不做完整版：静态「已删除集合」在跨分支/跨路径时天然会误杀——实测 `concurrent/futures/thread.py`
>与 `pickletools.py` 是「del 与使用处于不同的可达路径」，完整版需要 definite-assignment 格（与窄化同构），
>半天工作量且风险高，暂缓。
>
> 顺带：`typecheck/stmt.cpp` 触到 1200 行硬限制，DelGuard 抽进 `typecheck.h`、注释压缩后回到 1200。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 527 / difftest 495/0 / check_size PASS / ctest 8/8。
## [0.537] - 2026-11-25

> feat(runtime,lower)：**`buffer` —— RC 管理的连续内存**，可直接传给 C（BLAS/memcpy/memset）。
>
> 目标：解决「`cimport` 里 `calloc` 自建缓冲区必然泄漏」（没有 `__del__`）导致 ndarray 这类库只能用
>`list[float]` 的问题。
>
> ```python
> b = buffer(4)            # 4 个 float64，零初始化（calloc）
> b[0] = 1.5
> ib = buffer_i32(8)       # int32
> fb = buffer_f32(4)       # float32（4 字节/元素，见下面的限制）
> print(len(b), b[0])      # 4 1.5
> cimport clib
> clib.modf(3.75, b)       # 作为 ptr[float] 实参：连续 data 直接交给 C
> clib.memset(ib, 0, 32)   # 作为 ptr[int]
> ```
>
> 实现：新堆种类 `PYADAMAS_RC_BUFFER`（`{magic, data*, len, elem_kind}`，data `calloc`、dtor `free`，
>GC 叶子）；`TyKind::Buffer`；内置 `buffer/buffer_i32/buffer_f32`；`len`；下标读写（越界 panic、
>负索引）；`CKind::BufBuffer` 让 buffer 作为 `ptr`/`ptr[T]` 实参（data 在对象 +8）。
>
> 修掉的两个真问题（外部验证报告 `temp-proj/BUFFER_REPORT.md` 发现）：
>1. **泄漏**：`ir_is_heap()` 没把 `Buffer` 算成持有引用、`gc_free_body()` 没有 BUFFER 分支 →
>   每个 buffer 的 data 必然泄漏（单 8KB → 8KB leaked）。修后 `ASAN_OPTIONS=detect_leaks=1` **零泄漏**；
>2. **静默错值**：`BufferSetF64` 的第三实参是 double 却进了三整型寄存器的编组 → 裸变量/表达式写入垃圾，
>   只有字面量恰好蒙对。新增 `gen_rt_i2f1`(int,int,double) 修掉。
>
> 限制（刻意，写进 `docs/language.md`）：
>- **32 位 buffer（`buffer_i32`/`buffer_f32`）只能传裸 `ptr`**，不能传 `ptr[T]`（避免 8 字节步长越界）；
>- 不支持切片、迭代、`in`；跨模块接口把 Buffer 记成 unknown（拿不到静态类型）。
>
> 性能（外部实测，1e6 元素）：`arange` buffer **0.0050s** vs list 0.0092s（1.84× 快）；
>`sum` buffer 0.00475s vs list 0.00176s（2.7× 慢，每次下标都是带边界检查的 runtime 调用）；合计约快 11%。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **526** / difftest 494/0 / check_size PASS / bench 36/36 / vect-variants PASS / ctest 8/8；
>buffer 用例 ASan 零泄漏。
## [0.536] - 2026-11-25

> fix(ir,typecheck)：**跨模块调用被装饰方法不再段错误**（`temp-proj/DECORATORS.md` 的 J2）。
>
> 复现（修前）：
>
> ```python
> # decorators.py
> def doubled(fn):
>     def wrapper(self, x: int) -> int:
>         return fn(self, x) * 2
>     return wrapper
> class Calc:
>     @doubled
>     def add(self, x: int) -> int: ...
> # main.py
> import decorators as d
> c = d.Calc(10)
> print(c.add(5))     # Segmentation fault
> ```
>
> 根因链：装饰方法的实现是**类属性里的闭包值**，不占 vtable 槽（`vtable: f3 f-1`）；
>但接口导出时丢了两处信息，导入方按普通方法 `vcall %14[1]` → 跳空槽（-1）→ 段错误：
>1. 接口 flatten（`driver/build.cpp` 的 `by_key` 重建）没复制 `MethodInfo::decorated`；
>2. 接口编码/解码没有这个字段。
>
> 修法：`MethodInfo::decorated` 进接口（编码加 1 字节），flatten 两处（build 与 check）都复制；
>导入类上的装饰方法调用在 **check 阶段**就报清楚：
>
> ```
> error: decorated method 'add' of imported class 'Calc' is not available across modules;
>        call it inside its own module or drop the decorator
> ```
>
> 同模块内装饰方法不受影响（仍正常，`30`）。
>
> 顺带记录：函数值不能挂属性（`wrapper.calls = ...`）check 过、build 报 `unsupported assignment target`，
>函数值是纯编译期实体、没有运行期字典——用闭包 cell 或模块级 dict 替代（已记入 `docs/frontend-gaps.md`）。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 524 / difftest 492/0 / check_size PASS / ctest 8/8。
## [0.535] - 2026-11-25

> feat(lower)：**局部变量绑定的方法可以直接调用**（B1 最小范围），并带健全性护栏。
>
> ```python
> xs: list[float] = [1.0]
> push = xs.append        # 以前：build 报 attribute access is only supported as a method call
> push(2.0)               # 现在：正常，[1.0, 2.0]
> ```
>
> 表示是**纯编译期**的：降级器每个函数记 `名字 → 那条 <recv>.<method> 的 AST`，不产生值、不占槽、
>不新增堆对象种类；调用点命中登记就走一次普通方法调用。
>
> **健全性护栏（版本号）**：`assign_to` 每写一次名字或 `self.field` 就 +1；绑定点记下接收者与当时版本，
>调用点版本不一致就**报错**而不是重放：
>
> ```python
> push = xs.append
> xs = ys
> push(2.0)   # error: bound method's receiver 'xs' was reassigned after binding; call the method directly instead
> ```
>
> 没有这条护栏时重放会在调用点重新求值接收者，静默改成对新对象的调用（CPython 调的是原对象）——
>属于我们这轮一直在清的静默错值。
>
> 边界（继续报错，不猜）：存进容器 / 返回 / 作为参数传递 / 接收者 unknown 或联合 / 方法名不唯一。
>回归：`tests/run/bound_method.py`（list.append、str.upper、用户类方法、重新赋值切对象，4 行与 CPython 一致）、
>`tests/strict/bound_method_reassigned.py`（健全性反例，期望 build 拒绝）。
>
> 628 口径：default 499/626、old 504、`--imports` 167（不降；那 15 个把方法引用存变量的 stdlib 文件
>都超出「局部变量 + 直接调用」的范围，因此没有新增通过）。
> 闸门：套件 **523** / difftest 491/0 / check_size PASS / bench 36/36 / vect-variants PASS / ctest 9/9。
## [0.534] - 2026-11-25

> feat(runtime,lower)：**内置 `complex` 补 `.real` / `.imag` / `.conjugate()`**（`temp-proj/PITFALLS.md` 的 C3）。
>
> 修前：`z.real` / `z.imag` / `z.conjugate()` 全部报 `'complex' object has no attribute`——内置 complex 只有
>`+ - * /` 和 `abs()`，用户只能自己包一层 `get_re()`/`get_im()`/`conj()`。
>
> 修法：
>- runtime `rt_complex.c`：`pa_complex_real` / `pa_complex_imag`（f64 返回）、`pa_complex_conj`（新 complex，re 不变、im 取负）；
>- RtFn 三个入口 + `rtfn_names` + `call_rt` 编组，**复用 `ComplexAbs` 那一组**（单指针入参、f64 返回），
>  `float_info` 那次的 f64 编组问题没有复现；
>- typecheck：`expr_attribute` 补 `.real`/`.imag`（float）与 `.conjugate`（BoundMethod），`method_call` 补 `conjugate()`；
>- 降级：属性读走 `ComplexReal`/`ComplexImag`，方法调用走 `ComplexConj`。
>
> 实测（`tests/run/complex_accessors.py`，与 CPython 逐字节一致）：
>```
>pya:     1.0 -2.0 (1+2j) 5.0
>CPython: 1.0 -2.0 (1+2j) 5.0
>```
>
> 只加了只读字段访问，complex 的堆对象种类与 RC kind 都没动 → **单用例 ASan**（`build --asan -O1`）无报告。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **522** / difftest 490/0 / check_size PASS / bench 36/36 / vect-variants PASS / ctest 9/9。
## [0.533] - 2026-11-25

> fix(typecheck,lower)：**类实例下标返回 float 的静默错值** + **标注 asyncio 原语反而更差**。
>
> 两个都是「另一个 agent 在真实项目里踩到、我们复核确认」的坑（`temp-proj/PITFALLS.md` 的 A1 与 B3/B3b/C1）。
>
> ### A1：`__getitem__` 返回 float 时直接使用会打成位模式
>
> ```python
> class Box:
>     def __getitem__(self, i: int) -> float:
>         return self.data[i]
> b: Box = Box([1.5, 2.5, 3.5])
> print(b[1])          # 修前 4612811918334230528（应为 2.5）
> v: float = b[1]      # 修前就是对的（标注强制转换）
> ```
>
> 根因两处：
>1. `typecheck/expr.cpp` 的 subscript 对 `TyKind::Class` 一律返回 unknown —— 改成取 `__getitem__`
>   的**返回标注**。这一条同时修好 `print`/`str`/`repr`/容器字面量/算术五条路径；
>2. `lower/print.cpp`：静态类型 unknown 时按 vreg 类型兜底，**只认 float**。为什么只认 float：
>   unknown 域里整型和堆指针同形，把 Int 也当兜底会让 `122_gen_throw.py` 把字符串指针打成整数
>   （实测踩到，收窄到 float 后恢复）。
>
> 修后：`2.5 2.5 [2.5, 3.5] 3.5`，与 CPython 逐字节一致（`tests/run/getitem_float_unbound.py`）。
>
> ### B3b + C1：标注成 asyncio 原语反而比不标注更差
>
> ```python
> lock: asyncio.Lock = asyncio.Lock()
> async with lock:      # 修前：error: async with requires a class instance with async __aenter__/__aexit__
> ```
>
> 不标注时类型取自初始化表达式的 `AsyncLock`，反而正常。根因：`annotation.cpp` 的 dotted 注解分支
>解析不到 `asyncio.Lock/Semaphore/Event/Queue` → 符号退化成 unknown。补上映射后，`async with`
>与 `acquire()`/`release()` 的既有降级路径（`stmt_async.cpp`）就能接上，标注与不标注行为一致。
>
> 回归 `tests/run/async_with_annotated.py`：标注局部 Lock、标注 Semaphore、`await acquire()`、
>`release()`、标注参数传入 —— 5 行与 CPython 一致。
>
> ### 判负：B1 方法引用（未做）
>
> 在 assign 里拦截 BoundMethod 会让 628 口径从 **499 掉到 484**（15 个文件真的把方法引用存进变量/容器），
>属于不可接受的新增拒绝，已回退。正确做法是在降级支持 bound method 值（`(recv, slot)` 闭包 + 调用点拆包），
>单独立项。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **521** / difftest 489/0 / check_size PASS / bench 36/36 / vect-variants PASS / ctest 9/9。
## [0.532] - 2026-11-25

> perf(opt)：**RC 临时值配对消除**——0.518 评估的 P2-C（选了低风险的 B 方案）。
>
> 现状（1e7 次实例创建循环，-O2）：每次迭代 IR 是
>`obj.new` + `pa_rc_temp` + `field.set` + `pa_rc_temp_take` + 循环尾 `rc_dec`，62 ns/次。
>原有 `rc_redundant_pairs` 只能吃掉**相邻**的 `RcTemp`/`RcTempTake`，被 `field.set` 隔开的那对漏掉了。
>
> 修法：新增形态 **1b** —— 同一基本块内 `RcTemp(v)` 与它之后**不碰临时值栈**
>（不出现 `RcTemp`/`RcTempTake`/`RcCleanup`）的 `RcTempTake(v)` 成对删除；
>中间只要有一条动栈指令就放过。依据：`pa_rc_temp` 只压栈不改 rc、`pa_rc_temp_take` 只出栈，
>两条一起删时 rc 与栈水位都不变，异常路径也一样（owner 自己的引用还在）。
>
> 实测：
>
> | 场景 | 修前 | 修后 |
> |---|---|---|
> | 实例创建循环（1e7 次，-O2） | 0.62–0.63 s（62 ns/次） | **0.57–0.58 s（57 ns/次）**，约 8% |
> | CPython 3.14 同机 | 3.71–3.73 s（372 ns/次） | 不变 |
>
> 我在合并后复核（2e6 次含方法调用）：pya **130 ms** vs CPython **767 ms**（≈5.9×）。
>
> 收益 <10%，按既定口径**不做 A 方案**（栈分配非逃逸实例，需动分配器 + GC/异常交互）。
>
> 闸门：**全量 ASan 套件 519 ran ok / 0 失败 / 无 sanitizer 报告**；常规套件 519；difftest 487/0；
>check_size PASS；bench 36/36（`obj_dispatch` 的 asm 因 RC 对消变了，perf 在噪声内）；ctest 9/9。
> 基线：default 499/626、old 504、`--imports` 167（不降）。
## [0.531] - 2026-11-25

> perf(runtime,lower)：**`isinstance` 走整数 class id**，替掉 MRO 上的字符串比较。
>
> 修前：`pa_is_named_class(obj, "mod.Cls")` 沿 MRO 逐个 `strlen`+`memcmp`；深继承时每次 isinstance
>都在做字符串比较。
>
> 修法：
>- 类元数据 **7 字 → 8 字**，第 8 字 = **限定名的 FNV-1a 64**（口径 `<模块运行时名>.<类名>`，
>  与导入方 `origin_qualname` 逐字节一致；不用模块内序号——模块分开编译/缓存，跨模块会不一致）；
>- 新增 `pa_is_instance_of(obj, cid)`：沿 MRO 比较整数；
>- 降级侧：**本地类**的 isinstance 发 `IsInstanceOf(obj, const cid)`；**导入类保留名字比较**
>  （跨模块实例的 vtable 元数据由定义模块发射，导入方的镜像元数据不一定是实例实际用的那份，走 cid 会误判 False）。
>
> 实测（-O2）：
>
> | 场景 | 修前 | 修后 |
> |---|---|---|
> | 3 层继承 isinstance，1e7 次 | 0.19 s（19 ns/次） | **0.13 s（13 ns/次）**，1.46× |
> | union 形参脚本，1e6 次 | 0.05 s | 0.05 s（瓶颈已不在 isinstance） |
> | CPython 3.14 对照 | — | 2.34 s / 0.28 s |
>
> **布局坑（记下来）**：8 字后 `dict_off` 从 `vt[-2]` 移到 `vt[-3]`，漏改会让
>`155_exc_value`、`156_dynamic_attr`、`250/251_argparse` 四个用例 OUTPUT DIFF。
>
> 新增 `tests/modules/cross_isinstance/`（`from value import Value; isinstance(x, Value)` → `True` / `2.0`），
>正好锁住「导入类不能走 cid」的边界。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 **518** / difftest 486/0 / check_size PASS / bench 36/36 / ctest 9/9；
>单用例 ASan（`build --asan -O1` 跑 isinstance + 跨模块用例）无报告。
## [0.530] - 2026-11-25

> perf(runtime,lower)：**`x in list` 按元素类型特化**——0.518 评估的 P2-A。
>
> 修前：`x in xs` 一律降成 `call.rt pa_list_contains(list, value, cmp_kind)`，运行期每元素过一次
> `cmp_kind` 分派（switch + 可能的 unknown 探针）。
>
> 修法：新增三个运行期入口 `pa_list_contains_i64` / `_f64` / `_str`（按机器字 / IEEE 双精度 /
> `pa_str_eq` 比较），降级侧在**元素类型静态已知**且容器是 list 时选特化入口：int/bool → i64、
> float → f64、str → str；unknown/联合/容器元素/tuple 保持通用路径。
>
> 微基准（1000 元素 list、1e6 次 `in`，-O2）：
>
> | 场景 | 修前 | 修后 | 加速 |
> |---|---|---|---|
> | 命中 | 2547 ms | 410 ms | **6.2×** |
> | 未命中 | 2537 ms | 394 ms | **6.4×** |
>
> 我在合并后复核了一次（1000 元素、1e6 次命中，含 `time.time()` 计时）：pya **218 ms** vs
>CPython 3.14 **4262 ms**（≈19.5×）。
>
> 语义：int/float/str/bool 列表、带 `__eq__` 的类实例列表、`not in` 共 7 行与 CPython 逐字节一致
>（类实例仍走 `__eq__`，不是指针比较）。
>
> 12 基准：`bench/realistic` 六个 + `bench/*.py` 三个的 asm md5 **全部不变**（它们没有 list 上的 `in`），
>`--full` 的 perf 比对 warn=0 fail=0。
>
> 基线：default 499/626、old 504、`--imports` 167（不降）。
> 闸门：套件 517 / difftest 486/0 / check_size PASS / bench 36/36 / ctest 9/9。
## [0.529] - 2026-11-25

> fix(typecheck,lowe)：**`isinstance` 否定分支的窄化真正落地**——0.518 评估的 P0-D。
>
> ```python
> def f(o: int | str) -> str:
>     if isinstance(o, int):
>         return "int"
>     return o.upper()        # 修前：method 'upper' is not supported by the code generator yet
> print(f(1), f("ab"))      # 修后：int AB（与 CPython 一致）
> ```
>
> 三处类型层修复（上一轮 0.528 修好守卫后剩下的正是这三层丢信息）：
>
> 1. `mk_union` 把**联合当成裸 unknown 短路**：`mk_union(int | str, float)` 直接塌成 unknown，
>    于是三元以上联合永远建不起来（`pyadamas types` 里三成员标注显示 `func(unknown)`）；
> 2. `join` 不认识联合：`join(int | str, int)` → unknown，多分支窄化窄不回来；
> 3. `stmt_if` 的分支合并以空表起步：`join(int, str)` → unknown。现在**只播种联合类型**
>    （Optional 不能播种——0.498 的「先合并再恢复窄化」依赖 merge 看到分支末端类型，
>    播种 Optional 会把 None 并回来，实测 `narrow_optional` 立刻误报）。
>
> 降级层配合：方法调用的接收者是联合时，选「唯一能接住这个方法」的成员派发
>（`TypeChecker::type_has_method`），**槽的表示不变**（unknown 里指针类就是裸指针），
>因此没有改槽宽带来的表示风险。
>
> 回归 `tests/run/isinstance_neg_branch.py`：两成员 / 三成员 / `not isinstance` / `raise` /
>属性路径五组，输出与 CPython 逐字节一致（`int AB` / `int float ab` / `no ab` / `skip AB` / `b CD`）。
>
> 基线：default 499/626、old 504/626、`--imports` 167（均不变）。
> 闸门：套件 **517** / difftest 486/0 / check_size PASS / bench 36/36 / vect-variants PASS / ctest 8/8。
## [0.528] - 2026-11-25

> fix(lower)：**`isinstance(动态值, int/bool/bytes)` 不再恒为 False**——一个 P0 级静默错值。
>
> 现象（修前，类型标签默认关）：
>
> ```python
> def h(o: object) -> int:
>     return 1 if isinstance(o, int) else 0
> print(h(1), h("x"))          # 0 0   ← 应为 1 0
> ```
>
> 对列表元素同样：`for x in xs: if isinstance(x, int)` 统计 `[1, "a", 2.0, 3]` 得 **0**（CPython 2）。
>
> 根因：静态类型是 unknown 时，只有 `str/list/dict/float` 有运行期探针，`int/bool/bytes` 没有 tag
> 就**保守返回 false**；精确判定要 `AnyTypeIs(v, code)`，而它依赖 `PYADAMAS_TYPE_TAGS`，默认关。
>
> 直接默认全量开标签实测不可接受（`--full` 的 12 基准，相对 0.527）：
>
> | 基准 | 变化 |
> |---|---|
> | matmul | **+660%** |
> | spectral_norm | **+148%** |
> | sieve | **+110%** |
> | nbody | **+83%** |
> | mandelbrot | **+57%** |
> | fib | **+38%** |
> | binary_trees | **+11%** |
>
> 所以改成**程序级按需开启**：`prepare_program` 里扫一遍源码，出现 `isinstance` 才给这个程序打开标签
>（`PYADAMAS_TYPE_TAGS=1` 强制开、`=0` 强制关，作逃生口）；标签本身仍写在模块缓存清单里参与失效。
>
> 结果：
>- 正确性：`tests/run/isinstance_dynamic.py` 与 CPython 逐字节一致（`1 0 0 1` / `2` / `int str float int list other`）；
>- 性能：12 基准 **36/36 逐字节一致、warn=0 fail=0**（它们不含 `isinstance`，一个字 tag 都不多）；
>- 基线：default 499/626 不变。
>
> 闸门：套件 **516** / difftest 485/0 / check_size PASS / bench 36/36 / vect-variants PASS / ctest 8/8。
## [0.527] - 2026-11-25

> fix(ir)：**模块接口保留联合类型与类对象**——0.518 评估的 P0-B。
>
> 修前：跨模块使用 union 形参的库会退化。`value.py` 里 `def add(self, o: "Value | float")`，
>`main.py` 里 `a.add(2.0)` 之后 `a.add(Value(1.0))` 直接报
>`argument 'o' of '<imported>' is used with both float and non-float values; annotate the parameter type`；
>更糟的是退回 unknown 后 ABI 错位（静默错值）。类对象（`P = Value`）跨模块也拿不到构造器。
>
> 根因两处：
>1. `enc_type` 把 `TyKind::Unknown`（联合的表示就是 Unknown + 成员表）和 `ClassObj` 都写成了 tag 0；
>2. 解码侧 `mk_union/union_flatten` 把「类引用占位（解码时是 Unknown）」当成 unknown，把整个联合折成裸 unknown。
>
> 修法：
>- 新增 tag **25 = 联合**（成员依次编码）、tag **26 = ClassObj**（内部 Class 引用仍是 tag 13）；0..24 逐字沿用，
>  旧 modcache 解码不受影响；新编码需要 ≥0.527 的编译器读。
>- 解码联合时**直接构造**（不经过 `mk_union`），保留类引用占位，等 `pending_classes` 原地填成 Class。
>
> 实测（`tests/modules/union_sig/`）：
>```
>pya:     5.0 4.0 / 7.0 / 9.0
>CPython: 5.0 4.0 / 7.0 / 9.0
>```
> 同一模块内的 union 调用本来就是对的（0.525 的 dunder coerce），本轮只修跨模块。
>
> 基线：**default 499/626 不变**、old 504、`--imports` 167。
> 闸门：套件 515（新增 module 用例）/ difftest 484/0 / check_size PASS / ctest 8/8。
## [0.526] - 2026-11-25

> fix(lower)：**`float()` / `int()` 不再接受类实例的指针位模式**——0.518 评估的 P0-C。
>
> 修前：`float(Value(3.0))` 编译通过，输出 `107287860196448.0`（把对象指针按 double 读）；CPython 抛 `TypeError`。
> IR 上它是一条 `convert`（`%18 = convert %15 ; float`），既不走 `lower_builtin_text`，也不走 `convert_to` 的显式分支。
>
> 修法：`convert_to` 末尾的 `IrOp::Convert` 兜底前拦一道——目标 `float`/`int` 且源是
>`Class`/`List`/`Dict`/`Set`/`Tuple`/`None`/`Bytes` 时，先试 `__float__`/`__int__`/`__index__`（走 0.525 修好的
>dunder coerce 路径），没有协议就编译期报错，文本对齐 CPython：
>
> ```
> float() argument must be a string or a real number, not 'Value'
> int() argument must be a string, a bytes-like object or a real number, not 'Value'
> ```
>
> 对拍：反例 `tests/strict/float_class_rejected.py`、`int_class_rejected.py` 被拒；
> 正例 `tests/run/convert_numeric.py`（`float(1)`/`float(True)`/`float("1.5")`/`int(3.9)`/`int("42")`）5 行与 CPython 逐字节一致。
> 已知既有差异（非本轮引入）：`x: float = 1` 我们按标注转成 `1.0`，CPython 保持 `int 1`。
>
> 闸门：套件 / difftest / check_size / ctest（合并时跑一次）。
## [0.525] - 2026-11-25

> fix(lower)：**dunder 调用补参数 coerce**——修掉 `q + 2.0` 这类运算的静默错值（0.518 评估的 P0-A）。
>
> 现象（修前）：自定义类 `Q.__add__(self, o: "Q | float")`，`q + 2.0` 得到 **6.0**（CPython 8.0）、`q == 6.0` 得到 **False**。
>
> 根因：`dunder_call` 把实参直接塞进 `IrOp::VCall`，而 `gen_vcall` 按**实参 vreg 的静态类型**分类 ABI——
>float 实参进 `xmm`，但被调形参是 union（unknown，整型字），于是 callee 把 double 的位模式当整数/指针读。
>正常方法调用走 `bind_args`，那里有 `coerce(args[i], vreg_ty, want)`，dunder 这条路径漏了。
>
> 修法：`dunder_call` 在构造 VCall/Call 之前按 `call_sig->pos[i].ty` 逐参 `coerce`（from 取 `vreg_ty(args[i])`），
>装饰后的 dunder 分支共用同一段逻辑。
>
> 对拍（`tests/run/dunder_arg_coerce.py`）：`+ - * / **`、`== != <`、`in`（`__contains__`）、str 的 `__add__` ——
>**8 行与 CPython 逐字节一致**；原复现从 `6.0 8.0 False` 变成 `8.0 8.0 True`。
>
> 闸门：套件 / difftest / check_size / ctest（合并时跑一次）。
## [0.524] - 2026-11-25

> feat(driver)：**发布包可迁移 + `dist/` 里带编译好的二进制**。
>
> 之前二进制里烧死的是编译期 `PYADAMAS_STDLIB_DIR`（开发机的 `/src/.../stdlib`），拷到别的机器上就找不到模块表。
> 现在 `stdlib_dir()`（`src/driver/util.cpp`）按顺序解析：
>
> 1. `PYADAMAS_STDLIB` 环境变量；
> 2. 编译期路径（**存在时**才用）；
> 3. 可执行文件旁边：`<exe>/stdlib`、`<exe>/../stdlib`、`<exe>/../share/pyadamas/stdlib`；
> 4. 都没有就退回编译期值（读模块时会明确报错，不会静默用空表）。
>
> 全仓 5 处 `PYADAMAS_STDLIB_DIR` 的运行时使用（`check`/`build`/`modules` 的 sigs 与模块表路径）都改成 `stdlib_dir()`。
> 实测：把 `bin/pyadamas` 与 `stdlib/` 拷到临时目录、`cwd=/tmp`、不设任何环境变量，
>`run`/`check` 正常（`9 行 smoke` 输出与仓库内一致）。
>
> 新增 `tools/make_release.sh`：打包 `bin/pyadamas + stdlib/ + README/LICENSE + RELEASE.txt`，
>生成 `dist/pyadamas-<ver>-linux-x86_64.tar.gz` 与 `.sha256`，并**解包自检**（换 cwd、无环境变量跑一个用到 stdlib 的程序）。
> 本轮产物 `dist/pyadamas-0.524-linux-x86_64.tar.gz`（3.7M）已随仓库提交。
>
> 依赖：x86-64 Linux + glibc（本包在 Debian 13 / glibc 2.41 构建）+ binutils 的 `as`/`ld`。
## [0.523] - 2026-11-25

> fix(driver/check)：**默认解析项目内的跨模块导入**——`check` / `types` 不再把导入的符号压成 unknown。
>
> 之前 `check`/`types` 默认**完全不加载 import 图**（0.486 的保守回退），于是：
>
> ```
> import mod;  s: str = mod.add(1, 2)      # 不报错（mod.add 是 unknown）
> from mod import add;  t: str = add(3, 4) # 同样漏掉
> ```
>
> 现在默认加载 import 图，**但按模块来源分层**：
>
> | 模块来源 | 默认行为 | `--imports` |
> |---|---|---|
> | **本项目自己的模块**（入口同层/`include_dirs` 下的 .py） | **严格解析**（接口完整，跨模块调用拿到真实类型） | 严格 |
> | 原生模块（`os`/`sys`/`re`…） | unknown（签名表不全，避免 0.486 的误杀） | 严格 |
> | 我们自己的 stdlib 扩展模块（`functools` 等） | unknown（`partial`/`itertools.count` 等未建模） | 严格 |
> | CPython 标准库（`/usr/lib/python3.*`）与我们 `stdlib/` 的纯 Python 模块 | unknown | 严格 |
> | `--no-imports` | 整张图关掉（等价旧行为） | — |
>
> 实测（项目内三文件：`mod.py` 定义 `add`/`CONST`/`Box`，`main.py` 用 `mod.add`、`from mod import add`、`Box()`）：
> `types` 现在输出 `add func(int, int) -> int` / `CONST int` / `Box Box`；写成 `s: str = mod.add(1, 2)` 会正常报 **4 处**
> `annotated as 'str' but assigned 'int'`。原生模块仍宽松（`os.path.join` 等不误杀）。
>
> 基线：**default 499/626（不变）**、old 504/626、`--imports` 167/626。
> **过程中量到的坑**（已写进代码注释）：先试「全部严格解析」会掉到 **223/626**；只跳过 CPython Lib 掉到 **302**；
> 再跳过我们自己的 stdlib 扩展模块才回到 **499**。掉下去的那 21 个文件暴露的是我们接口的真实缺口
>（`itertools.count`、`chain.from_iterable`、`functools.partial`），留作下一批接口表工作。
>
> 闸门：套件 / difftest / check_size / ctest（合并时跑一次）。
## [0.522] - 2026-11-25

> perf(backend)：**浮点归约接通 packed 累加**（`addpd`），新增开关 `PYADAMAS_VECT_FASTFLOAT=1`（默认关）。
>
> 问题（用户实测发现）：`PYADAMAS_VECT=1` 下浮点归约的 asm 是 **`mulpd` 算出两个 lane、再用两条 `addsd` 写进同一个累加器**——
>依赖链没断，所谓 2-lane 是「假并行」，所以 `VECT=1` 与 `VECT=0` 计时几乎一样（浮点 ~10 GB/s vs 整数 40 GB/s 的一个来源）。
>
> 根因不是「没有 AVX2」，而是**浮点归约这条路径为了保序**（与标量逐位一致）主动做了 lane 提取 + 两次标量加。
> 本版把 packed 累加接通：
>
> - 累加器在循环里保持 **packed 两 lane**，每轮一条 `addpd expr, acc`（两条 lane 各自累加，依赖链断成两条）；
> - 入口：栈上 acc 用 `movsd` 装载（高 lane 天然清零）；寄存器提升的 acc 用 `pxor + movsd + movapd` 清高 lane；
> - 出口：`movhlps` + `addsd` 做**水平求和**写回（`acc.lo += acc.hi`）；
> - 开关参与 modcache 键（`vectff` 行），切换开关不会命中旧产物。
>
> **为什么默认关**：packed 累加改变了浮点加法顺序（偶数位一组、奇数位一组，最后相加），结果可能与标量**差最后几位**，
>而 `difftest` 要求 `-O0`/`-O2` 输出逐位一致。所以默认仍走保序路径；需要速度时显式开。
>
> 实测（`fsum` 热循环，8 元素 list 跑 200 万轮，L1 常驻）：
>
> | 模式 | 耗时 |
> |---|---|
> | `VECT=0`（标量） | 42ms |
> | `VECT=1`（保序 2-lane） | 28ms |
> | `VECT=1 + FASTFLOAT=1`（packed 累加） | **23ms（−18% vs 保序，−45% vs 标量）** |
>
> 内存受限场景（2000 万元素 `list[float]`，只测了「建表 + 求和」的墙钟）只快 ~3%——那一段是建表主导、不是纯归约，不能当带宽证据。
>
> **更正**：`list[float]` 的元素是**裸 f64 字**（向量段 `movupd 0(%rdi,%rbx,8)` 直接读 data 数组；`append`/`get` 走 `pa_list_append`/`pa_list_get`），**不装箱**。
> 浮点比整数慢的真因是两条：① 浮点只走 **SSE2 2-lane**、整数走 **AVX2 4-lane**；② 归约原先为保序做**两条标量 `addsd`**，依赖链没断。
> 默认路径（不开开关）的生成代码**逐字节不变**。
>
> 文档：`docs/experiment-switches.md` 增加该开关一行（含「不要与逐位对拍同时用」的警告）。
>
> 闸门：套件 / difftest / check_size / ctest（合并时跑一次）。
## [0.521] - 2026-11-25

> feat(runtime)：**系统对象的最后两件**——`os.uname()` 与 `sys.float_info` / `sys.flags`。
>
> - **`os.uname()`**：新增 `TyKind::UnameInfo`（类型名 `posix.uname_result`，5 个字符串属性 `sysname/nodename/release/version/machine`）。
>   与 `StatResult` 的一个有意差异：**属性按需取值**（`pa_os_uname_field(which)` 每次调 `uname(2)`），`os.uname()` 本身只返回占位值——
>   list 承载在「赋值给变量」的路径上会被 temp 栈与槽各释放一次（实测 `refcount decrement on a freed object`），而 uname 的字段本来就是只读快照。
> - **`sys.float_info`**：新增 `TyKind::FloatInfo`（11 个属性）。`epsilon/max/min` 是 `<float.h>` 的 `DBL_*`，与 CPython 同 libc，
>   降级层**直接发十进制常量**（最短往返表示即逐位相同）；其余 8 个整数属性走 `pa_sys_float_info_i(which)`。
> - **`sys.flags`**：新增 `TyKind::SysFlags`（15 个 int 属性），全 0（pyadamas 没有这些开关，与 pysig 的 `dont_write_bytecode = False` 一致）。
>
> 对拍：`tests/run/os_uname.py` 5 行、`tests/run/sys_float_info.py` 7 行与 CPython 3.14.4 逐字节相同；
>`epsilon/max/min` **逐位相同**。常规 + ASan 都 ran ok。
>
> 基线：syntax 621/626、**default 499/626（不降）**、old 504/626、imports **165/626**（163 → +2）。
> 闸门：套件 **512** / difftest / check_size / ctest（合并时跑）。
## [0.520] - 2026-11-25

> feat(runtime)：**`time.struct_time`**——`localtime`/`gmtime`/`strptime` 现在返回**带 `tm_*` 属性的对象**，同时**完整保留旧的 tuple 语义**。
>
> 上一轮卡在这里的原因：`523/524/522` 三个用例在用 `len(t)`、`gmtime(0)[:6]` 切片、`strftime(fmt, t)` 的 tuple 形态，
>直接换成对象会把它们打坏。本版两头都占：
>
> - 新增 `TyKind::StructTime`（照 `StatResult` 的路子，**没有新 RC kind**）；运行期就是 9 个 int 的 list（`tm_year..tm_isdst`），
>   `t.tm_year` 在降级层映射成 `ListGet(t, 常量下标)`；
> - **len**：`len(t)` → 9；**下标**：`t[i]`/负下标 → int（`ListGet`，越界走 list 语义）；
> - **切片**：`t[:6]` → `tuple[int, ...]`（`ListSlice` + `mk_tuple_rest(int)`），`gmtime(0)[:6] == (1970,1,1,0,0,0)` 与改动前一致；
> - **`strftime(fmt, t)`** 接受 `StructTime`；`assignable` 增加「`StructTime` 可赋给 `tuple`」，让 `time.mktime(time.localtime(...))` 继续成立；
> - 新增 **`time.strptime(s, fmt)`**（C 的 `strptime`，失败抛 `ValueError`，文本与 CPython 一致）。
>
> **不可变**：`t.tm_year = 1` / `t[0] = 1` 在**编译期**拒绝，消息固定：
>`'time.struct_time' object is immutable (assign to a variable first)`（属性写与下标写两条路径都覆盖，回归 `tests/strict/time_struct_time_immutable.py`）。
>
> 对拍（`tests/run/time_struct_time.py`）：`localtime(0)`/`gmtime(0)` 的 6 个 `tm_*`、`len`、`t[0]`/`t[-1]`、`t[:6]`、
>`strftime('%Y-%m-%d', t)`、`strptime(...)` 字段——10 行与 CPython 3.14.4 逐字节相同；**522/523/524 原样通过**（一个都没改）。
>
> 闸门：套件 **510** / difftest **480/0** / check_size **PASS** / ctest 8/8。
## [0.519] - 2026-11-25

> feat(typecheck)：**成员级编译期拒绝原语 `@reject("原因")`**——成员**存在但不可用**时，在调用/使用处报明确原因。
>
> 动机：有些 API 的语义在编译期模型里**无法成立**（或运行期只能给出静默错的假数据）。之前的做法是「不声明」→ 用户看到
> `module 'sys' has no attribute 'modules'`，既不知道原因也不知道怎么办。现在可以声明出来、但使用即报错。
>
> - **pysig 语法**：`@reject("why")` 加在 `def` 上（`stdlib/sigs/*.pysig`）；解析器拒绝无参/重复的 `@reject`，
>   且被拒绝的成员**不参与运行期符号反查**（`resolve_rt_symbols` 跳过，不会要求一个不存在的 `pa_*` 符号）；
> - **C++ 侧**：`ModuleInfo::rejects`（名字 → 原因），`builtin_sigs` 里直接 `info->rejects[...] = ...`；
> - **使用点统一拦截**：`reject_message(module, name)`（`driver/build.h`）在五个「没有这个属性」的报错点先查拒绝表——
>   表达式取属性、调用、赋值、`from ... import`、以及 `--imports` 的成员解析；命中就报原因、不再落到通用错误。
>
> 首批用上两个真实例子：
> - `sys.modules` → `pyadamas resolves modules at compile time (no runtime module registry); import the module explicitly`；
> - `sys.getrefcount` → `pyadamas does not expose reference counts`。
>
> 实测三种形态都报对原因：`print(sys.modules)`、`print(sys.getrefcount(1))`、`from sys import modules`；
> 正常成员（`sys.maxsize`/`sys.prefix`）不受影响。
>
> 闸门：套件 / difftest / check_size / ctest（合并时跑一次）。
## [0.518] - 2026-11-25

> 一批「系统面 + 多继承性能」的落地：`os.walk` 崩溃、fd 面、`os.stat`、`sys` 身份常量、虚调用守卫看穿 thunk。
>
> **① fix(runtime)：`os.walk` 的 heap-use-after-free（崩溃）**
> 根因**不是 rc**，是 **GC 视界**：walk 是 C 递归，每层把 `dirs/files/tup` 只放在 C 局部变量里；递归期间每个目录项都 `str_new`，
> 一旦触发主收集，这些对象在根集合的四处（全局根表 / 临时值栈 / 帧记录 / 寄存器）**都看不见**，整棵 `out→tup→dirs→str` 子图被当垃圾清扫。
> （ASan：`READ of size 8 in walk_recurse` 的 `dirs->data[i]`，freed by `gc_free_body`；先加 `pa_rc_inc` 钉住**没用**——杀它的是「看不见 → 判不可达」。）
> 修法：改成**迭代式 + 显式栈**，`out` 与栈都用 `pa_gc_pin` 挂成根，每层构造 `dirs/files/tup` 时各自 pin、构造完 unpin。
> 实测本仓库根目录 `n = 31511`，CPython `len(list(os.walk('.'))) = 31511`，rc=0。
>
> **② feat(runtime)：文件描述符面**
> `os.open/read/write/lseek/dup/fdopen` + 8 个 `O_*` 常量；错误文本按 CPython 的 `[Errno N] strerror: 'path'` 口径（新加 `os_raise_path`），
>常量在 `rt_os.c` 用 `_Static_assert` 对齐宿主 C 宏（换平台数值不符会**编译失败**，不会静默错）。
> 对拍 `tests/run/os_fd.py`：open→write→lseek→read→dup→fdopen 读回→close，9 行与 CPython 3.14.4 逐字节相同（含 OSError 文本）。
>
> **③ feat(runtime)：`os.stat` / `os.lstat`**
> 新增薄的 `TyKind::StatResult`（照 `subprocess.CompletedProcess` 的「一个 TyKind 一个固定属性表」通道，**没有**新增 RC kind）；
>运行期用 10 个整数的 list 承载（顺序同 CPython 的 stat_result），属性读由降级层按名字映射成 `ListGet(obj, 常量下标)`。
> 对拍 `tests/run/os_stat_struct.py`：`st_size`、`S_ISREG`/`S_ISDIR`、`nlink`、`uid`、OSError 文本，6 行与 CPython 逐字节相同。
>
> **④ feat(lowering)：`sys.prefix` / `exec_prefix` / `path`**
> 前两个是编译期字符串快照（`/usr`，与 `sys.pysig` 的 `version`/`platform` 同口径）；`path` 是 `list[str]` 快照（3 项，**不参与导入解析**）。
> `sys.maxsize`/`version`/`byteorder`/`version_info` 本来就有；`float_info`/`flags` 需要「内建记录对象」通道，留待后续。
>
> **⑤ perf(backend)：虚调用守卫看穿多继承 thunk**
> `IRFunction` 记 `mi_thunk_target`/`mi_thunk_delta`；`gen_guarded_vcall` 收集候选时若槽里是 thunk，则**守卫比较 thunk 入口**（与运行期槽一致）、
>**内联体取 thunk 目标的叶子**、命中后 `addq delta` 把 self 挪到目标基类的字段块。
> 实测两状态多继承的虚调用 5M 循环 **40ms → 23ms**（单继承基准 25ms）；10 个基准 asm 与改动前逐字节相同。
>
> **⑥ test：`os_walk_smoke` 改成可重复跑**
> 原用例用固定路径 `/tmp/walk_smoke` + `os.mkdir`，第二次跑就撞「目录已存在」→ 输出 DIFF；现在目录名带 pid、跑完自清理、expect 块补了结尾换行。
>
> 闸门：套件 **509** / difftest **479/0** / check_size **PASS** / ctest 8/8（suite 由 checkall 跑）；ASan 套件 **508 ok**（唯一失败就是上面那条用例的输出换行，已修，非内存问题）。
> 628 默认口径 **499/626**、本仓库 907 个 `.py` **789 通过**，零新拒绝。
>
> **未做（已定位，下一步）**：`time.struct_time` 与现有 tuple 语义冲突（`523/524/522` 三个用例在用 `len()`/切片/`strftime` 的 tuple 形态），
>要落地需给 `StructTime` 加下标/切片/len/strftime 四处分支（约 2–3 小时）；`os.uname`、`sys.float_info`/`flags`、`sys.modules` 同批后续。
## [0.517] - 2026-11-25

> feat(runtime)：**`json.dump(obj, fp)` / `json.load(fp)`**（文件流版）。
>
> - **`dump`**：原生层的文件写入口**本来就可达**——`runtime/rt_internal.h` 里已有 `pa_file_write(PyAdamasFile*, PyAdamasStr*)`（`rt_file.c`），
>   不需要补新 API。流程：`pa_json_dumps(value, desc, indent, flags, NULL)` 拿结果串 → `pa_file_write(fp, s)` → `rc_dec` 中间串 → 返回 None；
>   `desc` 由调用点的静态类型生成（与 dumps 同一条 `json_desc`），`indent` 用 `INT64_MIN` 哨兵表示 None；
> - **`load`**：用 `pa_file_read(fp, -1)` 读全文（支持 FileLike 的实例侧分派），再走**动态**解析 `pa_json_parse`——不经 sample 描述符。
>   **返回类型故意是 unknown**：CPython 的 `load` 返回类型本来就是动态的，没有 sample 参数无法静态定；与 `loads(text, sample)` 的规则不同，已写进 pysig 注释。
>
> 接线：`RtFn::JsonDump`（5 参，与 JsonDumps 同编组）/ `JsonLoad`（1 参）+ `rtfn_names` + `call_rt` + `rt_internal` +
>`builtin_sigs.cpp` 的 json 分支（`dump` 的 obj/fp 标 `ann_unknown`、`load` 返回 unknown）+ `lower/import.cpp` 原生名表 + `lower/import_misc.cpp` 的 dump/load 分支（支持 `indent=`）。
>
> 对拍（`tests/run/json_dump_load.py`，11 行全同）：dump 内容 == `json.dumps(obj)`、`indent=2` 排版逐字节相同；
>load 读回 `repr` 与原对象一致；str 往返（中文/转义）；非法 JSON 的
>`ValueError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)` 逐字一致。
>
> 限制（写进文档）：dump 只开放 `indent=` 关键字（`separators`/`sort_keys`/`ensure_ascii` 未开放）；
>dump 的目标必须是真 file/FileLike（其它值走 `file_check` 的 TypeError，CPython 是 AttributeError，未对齐）；
>`load` 返回 unknown 意味着后续要静态类型的操作得经动态路径。
> 另外沿用 json 既有模型：**混合类型 dict 的 dumps/dump 需要静态可定的类型**（`json needs a statically known type`），这是既有行为、不是本版引入。
>
> ASan 套件 **506 ran ok / 0 失败**；628 默认口径 **501/623 不降**；本仓库 **907 个 `.py`：789 通过**；
>12 基准 asm 与 master 逐字节相同。闸门：套件 **506** / difftest **476** / check_size **PASS** / ctest 9/9。
## [0.516] - 2026-11-25

> feat(runtime)：**`operator.setitem(obj, key, value)` / `operator.delitem(obj, key)`**——照 `pa_any_index` 的结构做运行期分派。
>
> - `pa_any_setindex` / `pa_any_delindex`（`rt_class.c`）：同一个 `pa_unknown_is_heap` 探针 + 头上 kind 分派；
> - list：负下标折算、越界 `IndexError: list assignment index out of range`（**`del` 的越界消息也是这句**，实测确认）；
> - dict：`pa_dict_set` / `pa_dict_remove`；缺键先用 `pa_dict_contains` 判、再按 CPython 抛 `KeyError: '<键 repr>'`
>  （运行期内部的 `pa_dict_remove` 只说 `key not found`，不能直接用）；
> - str/None/bool 与其它堆对象 → 对应 TypeError；探针说不是堆时按 NoneType / int / 标量元组抛。
>
> **一个必须说明的设计选择**：**tuple 与 list 在运行期同形**（都是 `RC_LIST`，没有不可变标记），运行期分不开。
> 所以 tuple/str/标量的 TypeError 由**调用点按静态类型**发射（`lower/import_stdlib.cpp`），只有 unknown 才走运行期兜底。
> 顺带发现 CPython 自己的删除消息不统一：tuple/str 是 `doesn't support item deletion`，标量是 `does not support item deletion`，按实测分开写。
>
> 对拍 16 行全同：list 正/负下标写删（含返回 None 与内容）、dict 插入/覆盖/删除、越界 `IndexError`、缺键 `KeyError: 'zz'`、
> tuple/str/标量的写删 TypeError（消息逐字）。
>
> 判负：自定义类的 `__setitem__`/`__delitem__`（要按对象 vtable 找 dunder，运行期没有这条路）。
>
> ASan 套件 **505 ran ok / 0 失败**；628 默认口径 **501/623 不降**；本仓库 **906 个 `.py`：788 通过**；
>12 基准 asm 与 master 逐字节相同。闸门：套件 **505** / difftest **475** / check_size **PASS** / ctest 9/9。
## [0.515] - 2026-11-25

> feat(runtime)：**`random` 的六个分布函数，与 CPython 3.14.4 逐字节一致**（120/120）。
>
> 之前判负的理由是「要复刻算法链」，但前提条件其实已经具备：**`random()` 序列本身逐位相同**（`seed(12345)` 前 5 个值与 CPython 全同，
>`randbytes` 早前已验证）。所以只要公式逐行照抄，结果就逐位相同——不需要任何近似。
>
> | 函数 | 要点 |
> |---|---|
> | `normalvariate(mu, sigma)` | Kinderman-Monahan；注意新版是 `zz = z*z/4.0`（旧版没有 `/4.0`） |
> | `gammavariate(alpha, beta)` | 三分支逐行照抄：`alpha>1` 的 Cheng（含 `1e-7<u1<0.9999999` 拒绝采样、`r+SG-4.5z>=0 \|\| r>=log z` 接受条件）、`alpha==1`、`alpha<1` 的 Kennedy-Gentle |
> | `betavariate` / `lognormvariate` / `paretovariate` / `weibullvariate` | 照抄（`if y:` 才除、`exp(normalvariate(...))`、单抽/幂运算） |
>
> 常数用 CPython import 期算出的 double 字面值：`NV_MAGICCONST = 1.7155277699214135`、`SG_MAGICCONST = 2.504077396776274`、
>`LOG4 = 1.3862943611198906`、`e = 2.718281828459045`。
>
> 对拍（`tests/run/random_distributions.py`）：6 组 × 20 个值、每组各自 `seed(12345)`，**120/120 逐字节相同**；
>边界 `gammavariate(0, 1)`/`gammavariate(1, 0)` 的 `ValueError: gammavariate: alpha and beta must be > 0.0` 消息逐字一致。
>
> 接线：`RtFn::RandG*` 六个 + `rtfn_names` + `call_rt` 编组（两个 f64 实参组、单 f64 实参组）+ `stdlib/sigs/random.pysig`（`@native`）。
>
> ASan 套件 **504 ran ok / 0 失败**；628 默认口径 **501/623 不降**；本仓库 **905 个 `.py`：787 通过**、零新拒绝；
>12 基准 asm 与 master 逐字节相同。闸门：套件 **504** / difftest **474** / check_size **PASS** / ctest 9/9。
## [0.514] - 2026-11-25

> fix(typecheck)：**全 None 列表字面量按 `list[unknown]` 推断**——消掉窄化剩下的两个假阳性。
>
> 0.499 的「可能为 None」诊断在 628 语料上留下 4 个文件，本版逐个判定后修掉 2 个假阳性：
>
> | 文件 | 判定 | 处理 |
> |---|---|---|
> | `base64.py:482`（`_b85dec = [None]*256`，import 期填满） | **假阳性** | 已修，转绿 |
> | `xml/dom/pulldom.py:110`（`self.firstEvent = [None, None]` 占位） | **假阳性** | 已修，转绿 |
> | `modulefinder.py:442`（`determine_parent()` 未标注 + `return None` 分支） | **正确诊断** | 不放宽 |
> | `multiprocessing/util.py:182`（`_SUN_PATH_MAX` 条件定义） | **正确诊断** | 不放宽 |
>
> 修法（窄规则，只动字面量推断）：`typecheck/expr.cpp` 的 `case ExprKind::List` —— 元素合起来是 `None` 时按 `list[unknown]` 推断，
>而不是 `list[None]`；`[None] * 256` 先经过这个字面量分支，所以乘出来也是 `list[unknown]`。**标注成 `list[int]` 的完全不受影响**（标注路径不走字面量推断）。
>
> **影响面先量过**：628 stdlib 里 `[None] * N` / `[None, None]` 共 **13 处**，本仓库 **0 处**；
>代价只是这类列表的 `xs[0] + 1` 不再报错（本来也是对 None 的误报）。
> **没削弱检查的证据**：`def f(x: int | None) -> int: return x + 1` 仍然报 `'x' may be None here`，已固化成 `tests/strict/narrow_optional_still_errors.py`；
>占位惯用法固化成 `tests/run/narrow_none_placeholder.py`。
>
> 628 默认口径：**499 → 501/623**；本仓库 **902 个 `.py`：785 通过**、零新拒绝；12 基准 asm 与 master 逐字节相同。
> 闸门：套件 **503** / difftest **473** / check_size **PASS** / ctest 9/9。
## [0.513] - 2026-11-25

> feat(lower)：**`operator.index(x)` / `operator.length_hint(x, default=0)`**——静态分派，**零运行期改动**。
>
> - **`index(x)`**：整数域（`Int`/`Bool`/定宽/`Big` 与 `unknown`）**原样返回**（`index(True)` = 1，bool 与 int 同槽天然一致）；
>  其余类型在**调用点**发射 `raise TypeError("'<type>' object cannot be interpreted as an integer")`，类型名按静态类型映射；
> - **`length_hint(x, default=0)`**：静态类型是 list/str/dict/tuple/set/bytes/range → 发射 `IrOp::Len`（零运行期开销）；其余返回 `default`。
>
> **CPython 语义纠正（我给的清单里那条是错的）**：`operator.length_hint(1)` **返回 0**，不是抛 TypeError ——
>`default` 默认就是 0，抛 TypeError 的是内置 `len()`。按实测对齐，注释与测试都写明。
>
> 踩到的坑：第一版把两个参数当普通 unknown 声明，结果 `operator.index(1.5)` 被类型检查判死
>（`argument 'a' of 'operator.index' is used with both float and non-float values`）—— 那是**新增拒绝**；
>修法：签名里把参数标 `ann_unknown`（同一函数被不同静态类型调用是合法的，运行期抛）。
>
> 对拍（CPython 3.14.4 逐字节一致）：`index(5/True/-3)` → `5 1 -3`；`length_hint([1,2,3])`/`{}`/`'abc'`/`(1,2)` → `3 0 3 2`；
>`length_hint(1,7)` → 7、`length_hint(1)` → 0；`index(1.5)` 的 TypeError 消息逐字一致。
> 自定义类的 `__index__`/`__length_hint__` **判负**（要在 lowering 里按静态类型找 dunder、或运行期按 vtable 分派）；`unknown` 参数目前不做校验。
>
> 语料：628 默认口径 **499/623 不降**；本仓库 **902 个 `.py`：785 通过**、零新拒绝；12 基准 asm 与 master 逐字节相同。
> 闸门：套件 **502** / difftest **472** / check_size **PASS**。（本轮没改运行期，只对目标用例跑了 ASan。）
## [0.512] - 2026-11-25

> feat(runtime)：**`re.subn(pattern, repl, string, count=0)` → `tuple[str, int]`**（接口表小件，一次一件推进）。
>
> 实现原则是「不写第二套匹配逻辑」：`pa_re_subn_str`/`pa_re_subn` 与 `sub` **共用同一条替换循环**
>（同样的空匹配推进 `pos = caps[1] + 1`、`count` 截断、无匹配时 `done = 0`），循环里本来就有 `done` 计数，
>直接拿它当返回值 —— 语义不会与 `sub` 分叉。
>
> 返回值：`pa_tuple_new_filled(2, 2, out, done, 0, 0)`（`elem_kind=2`：str 是堆引用、次数是标量），
>新建的结果串**所有权直接转给 tuple**（不额外 retain；销毁走注册表 dec，不会多 dec 标量）。
>
> 接线：`RtFn::RegexSubnStr`/`RegexSubn`（与 `sub` 同调用组）+ `rtfn_names` + `call_rt` 编组；
>`builtin_sigs` 的 `re_fn` 表加 `subn`（`ret = tuple[str,int]`）；`stdlib/sigs/re.pysig` 加 `Pattern.subn`（`@native`）；
>`lower/import.cpp` 原生名表加 `subn → RegexSubnStr`；`lower/method_misc.cpp` 加 `Pattern.subn` 发射（与 `sub` 同形）。
>
> **对拍**（`tests/run/re_subn.py`，与 CPython 3.14.4 逐字节一致）：
>
> ```
> re.subn('a','X','banana')            → ('bXnXnX', 3)
> re.subn('a','X','banana',1)          → ('bXnana', 1)
> re.subn('zz','X','banana')           → ('banana', 0)
> re.subn('','-','abc')                → ('-a-b-c-', 4)      # 空匹配
> re.subn('(?P<x>a)', r'[\g<x>]', …)   → ('b[a]n[a]n[a]', 3)  # 命名组
> type(r).__name__, len(r), r[0], r[1] → tuple 2 bXnXnX 3
> ```
>
> ASan 套件 **501 ran ok / 0 失败**；628 默认口径 **499/623 不降**；本仓库 **901 个 `.py`：784 通过**、零新拒绝；
> 12 基准 asm 与 master 逐字节相同。
>
> 闸门：套件 **501** / difftest **471** / check_size **PASS** / ctest 9/9。
## [0.511] - 2026-11-25

> feat(runtime)：`sys.getrecursionlimit()` / `sys.setrecursionlimit(n)`（接口表小件）；其余六项**判负**。
>
> - 声明式签名进 `stdlib/sigs/sys.pysig`；运行期两个小函数（`rt_math.c` + `RtFn` 零参/单参两组）；
>   pyadamas 没有运行期递归限制，只保存/查询一个值（默认 1000，`set` 校验 `n >= 1`）——
>   这样依赖它的代码行为与 CPython 一致；对拍 **1000 / 5000** 逐字节相同。
>
> **判负（逐项理由，避免下轮重试）**：
> - `sys.getsizeof`：标量域分不清 int/float（CPython 28/24），容器尺寸是 CPython 实现细节（`str` 41+len、`list` 56+8n…），
>   而我们的布局与 CPython 不同——**返回「CPython 尺寸」等于撒谎**，给假数字比不提供更糟；
> - `re.subn`：现有 `pa_re_sub` 只返回结果串、没有匹配计数，要改引擎并新建 `tuple[str,int]` 的 RC 所有权；
> - `operator.index/length_hint/setitem/delitem/methodcaller`：需要按静态类型分派的入口 / 新的通用下标写面 / 闭包工厂；
> - `random.normalvariate`/`betavariate`：要逐位复刻 CPython 的算法链（Kinderman-Monahan 比值法、Marsaglia-Tsang 伽马），
>   停了「分布对但值不同」的版本——那会让同种子对拍永远红；**逐字节复刻要单独立项**；
> - `json.dump`/`json.load`：`dump` 要文件对象 write 协议；`load` 没有 sample 参数、返回类型在语义上无法静态定；
> - `functools.total_ordering`：要编译期合成 `__le__/__gt__/__ge__`（FunctionDef 合成、槽位、与 dunder 校验交互），弄错就是静默错比较。
>
> 语料：628 默认口径 **499/623 不降**；本仓库 **900 个 `.py`：783 通过**、零新拒绝；12 基准 asm 与 master 逐字节相同。
>
> 闸门：套件 **500** / difftest **470** / check_size **PASS** / ctest 9/9。
## [0.510] - 2026-11-25

> feat(lower)：**`super()` 进入非 layout 基类时的 self 调整**——多继承的四条路径（属性 / 直调 / 虚调用 / `super()`）现在全部正确。
>
> **方案 2（lowering 侧 delta + CallDirect，不动运行期 ABI）**，理由三条（都是实测）：
> 1. `lower_super_call` 本来就把目标实现类（`declaring` = MRO 里 owner 之后第一个定义该方法的类）静态解出来了；
> 2. 方案 1（给 `pa_super_lookup` 加 adjust 参数）要改运行期签名 + `RtFn` + IR 发射 + 后端四处，回归面大；
> 3. vtable thunk 在 super 调用点拿不到——**方法体在类 vtable 装配之前编译**，此刻子类槽里还是基类的真实实现
>    （debug 实测 `slot val=1 mi_thunk=0`），所以不能「走槽跳 thunk」。
>
> 落点：`lower_super_call` 在已有 `SuperVCall` 之前加分支——`declaring != owner && mi_block_offset(owner, declaring) > 0` 时
> 给 `self` 加 delta、`CallDirect` 到 `declaring` 的真实实现；`declaring` 是导入类/解析不到时**退回 `SuperVCall`**（语义不变）。
> `mi_block_offset` 沿 layout_base 链找块，所以子类（`D(C)`）也能拿到 A 的块偏移。
> 顺带：`install_mi_thunks` 改为沿 layout_base 链**复用祖先已装的 thunk**（子类 vtable 的继承槽不再依赖复制顺序）。
>
> 实测（默认配置，五条与 CPython 逐字节一致）：
>
> ```
> cb.x, cb.get()  → 1 1
> f(cb)           → 1
> cb.y            → 2
> d.get2()        → 1      （CPython 1；改前 102）✓
> d.get()         → 1
> ```
>
> 语料：628 默认口径 **499/623 不降**；本仓库 899 个 `.py` **782 通过**、零新拒绝；
> 12 基准 asm 与 master 逐字节相同。
> 遗留（非阻塞）：`super()` 目标在导入类/动态 MRO 时仍走运行期 `SuperVCall`（不做 delta，保持原语义）；守卫按 (class,base) 分组是纯性能项。
>
> 闸门：套件 **499** / difftest **469** / check_size **PASS** / ctest 9/9。
## [0.509] - 2026-11-25

> feat(typecheck)：**默认放宽多继承护栏**——两个状态基类的形态现在**默认就能编译且正确**（不再需要 `PYADAMAS_MI_UNSAFE`）。
>
> 护栏判据：非 layout 基类的方法碰字段时，只要它的字段块**确实进了子类布局**（`base_field_offsets` 里有该基类且偏移 > 0）就放行；
> 没有 block 的（导入类、静态判不准、字段没进布局）继续拒绝；菱形（字段已在前缀里、`moved == false`）照旧放行。
> `PYADAMAS_MI_UNSAFE` 保留为调试开关（注释写清）。删掉钉旧拒绝行为的 `tests/strict/mi_non_primary_base_fields.py`，
> 新增 `tests/run/mi_two_state_bases.py`（属性 / 直调 / 虚调用三条路径，CPython 对拍）。
>
> **验收（默认配置、不带任何开关）**：
>
> ```
> cb.x, cb.get()  →  1 1      （CPython 1 1）✓
> f(cb)           →  1        （CPython 1）✓
> ```
>
> **三个长期被拒的 stdlib 文件转绿**（628 默认口径 **496 → 499/623**）：`xmlrpc/server.py`、`urllib/request.py`、`xml/dom/expatbuilder.py`，
>本仓库 **900 个 `.py`：783 通过**、零新拒绝；12 基准 asm 与 master 逐字节相同。
>
> **唯一遗留缺口（已定位、已回退）**：`super()` 进入「非 layout 基类的方法」时 self 没有调整（`class D(C): super().get()` 得 102，CPython 101）。
> 根因：`SuperVCall` 的运行期分派 `pa_super_lookup` **用 self 指针查对象所属类**（读 vtable），先挪 self 再调用就查不到类
>（实测直接抛 `'super' object has no attribute`）。正确修法二选一：①给运行期分派加 adjust 参数（lookup 用原指针、调用用调整后的）；
> ②在 lowering 里算出目标槽、按 vtable 跳 thunk。两条都要动运行期 ABI 或后端，留作下一轮；代码里已留注释。
>
> 闸门：套件 **499** / difftest **469** / check_size **PASS** / ctest 9/9。
## [0.508] - 2026-11-25

> feat(lower)：**vtable thunk 落地**——多继承的虚调用也正确了（`f(a: A)` 传 `C(B, A)` 实例现在返回 1）。
>
> 0.507 只修了直调路径；虚调用在运行期不知道真实类，必须在 vtable 槽里放 thunk。本版：
>
> - 新增 `src/lower/mi_thunk.cpp`：`build_mi_thunk(target_fidx, delta)` 用**正常 IRFunction 合成流程**
>  （`mod_.add_func` → `push_frame` → entry → `rc_mark_at` → 逐参数 `new_slot`+`load_slot` → `inc_params` →
>  `args[0] = Bin(Add, args[0], const_int(delta))` → `CallDirect Operand::func(target)` → `emit_return` → `pop_frame`）
>  拿自己的 fidx，走正常的链接与函数槽分配；
> - 安装点：`class.cpp` 的「继承来的 winner 填充」之后、`compile_ctor_wrapper` 之前；
>  只替换**没被更靠前类覆盖**的槽（`vtable[slot] == bvt[slot]`），跳过 static/classmethod/装饰方法/闭包（`env_slot >= 0`）；
> - **返回值用 `def` 不用 `def_heap`**：callee 返回的是 owned 引用，thunk 只转发；用 `def_heap` 会把结果也挂进临时栈、多 dec 一次
>   —— 实测报 `refcount increment on a freed object`，改成 `def` 后正常；
> - **`IRFunction::mi_thunk` 标记**：直调路径遇到 thunk 不再加 delta，否则会**双重偏移**（`cb.get()` 第一次就 RC 报错）。
>
> 验收（`PYADAMAS_MI_UNSAFE=1`，护栏未动、默认仍拒绝）：
>
> ```
> cb.x, cb.get()  →  1 1     （CPython 1 1）
> f(cb)           →  1       （CPython 1；改前 2）✓ 走 vtable thunk
> ```
>
> 语料：628 默认口径 **496/623 不降**；本仓库 **899 个 `.py`：781 通过**、零新拒绝；
> 12 基准 asm 与 master 逐字节相同（没有多继承的代码不生成 thunk）。
>
> 未做（下一轮）：`super()` 的 delta、守卫按 (class, base) 分组（**性能**，不影响正确性——thunk 是新指针，
> 按实现指针分组的候选自然不匹配、退回通用路径）、护栏默认放宽。
>
> 闸门：套件 **498** / difftest **468** / check_size **PASS** / ctest 9/9。
## [0.507] - 2026-11-25

> feat(lower)：**给 `MethodInfo` 补「声明类」，并把直调路径的 self 偏移修掉**——adjustor thunk 的原子第一步。
>
> 0.506 之后卡在一个缺失的字段上：`lower_method_call` 从 `recv_ty->cls->method(attr)` 拿到的 `MethodInfo` **没有记录它声明在哪个类**，
> 而 `delta = 8 * base_field_offsets[声明类]` 必须要它。本版补上：
>
> **① `MethodInfo::owner`**（`const ClassInfo*`，生命周期由 `TypeChecker::all_classes_` 保证，与 `Type::cls` 同一契约；
> 不用 `ClassInfoPtr` 是为了避免 `ClassInfo::methods` 再持 shared_ptr 成环）：
> - 填充点：`func.cpp` 的 `cls->methods.push_back(...)`（与 push 合一行，保住 1200 行上限）；
> - **别名填声明者**：`class_alias.cpp` 的 `MethodInfo am = src` 复制，owner 跟着 `src` 走，不是别名所在的类；
> - 继承槽没有「owner 被复制成子类」的路径（vtable 只填槽、不复制 `MethodInfo`）。
> **零行为改动**：三个基准 asm 与 master 逐字节相同。
>
> **② 直调路径的 self delta**：`replace` 在 sealed `CallDirect` 分支（`method.cpp:478` 的 `cd.args.push_back(recv)` 处），
> 条件是 `m->owner && m->owner != recv_ty->cls` 且 `base_field_offsets[owner] > 0`；手段是现有 `IrOp::Bin(BinKind::Add)` + 常量，
> **没有新增 IR op**；`VCall` 路径（487–495）完全没动。
>
> **顺带修一个布局 bug**：`materialize_base_fields` 现在只在**真的追加了字段**时记 `base_field_offsets`——
> 菱形继承（共享祖先）时字段已在前缀里、偏移差为 0，之前记了偏移会把 `self` 挪错位（六形态实测段错误）。
>
> **实测**（临时开关 `PYADAMAS_MI_UNSAFE=1` 绕过护栏，默认关闭、注释写明「thunk 落地前不要用」）：
>
> ```
> cb.x, cb.get()  →  1 1      （CPython 1 1；改前是 1 2）
> f(cb)           →  2        （f(a: A) 里的 a.get()：基类转型的虚调用，静态类型不知道真实类，下一轮 vtable thunk 修）
> ```
>
> 护栏**默认行为不变**（两状态基类仍拒绝，三个 stdlib 文件仍被拒——要等 thunk 覆盖虚调用）。
> 语料：628 默认口径 **496/623 不降**；本仓库 **899 个 `.py`：781 通过**；12 基准 asm 不变。
>
> 闸门：套件 **498** / difftest **468** / check_size **PASS** / ctest 9/9。
## [0.506] - 2026-11-25

> feat(typecheck)：**布局实体化**——非 layout 基类的字段块真正进入子类布局（adjustor thunk 的第一步）。
>
> 0.505 的中期结论暴露了前提问题：`class C(B, A)`（B 有 `y`、A 有 `x`）里，**C 的布局根本没有 A 的字段块**
>（`in_C = -1`），`self.x = 1` 反而走 `add_implicit_field` 新建了一个**遮蔽**的 C 字段——所以「调用点 `self += delta`」无从谈起。
>
> 本版把布局补上：
> - `ClassInfo::base_field_offsets`（基类 → 该基类字段块在子类布局里的**起始下标**）；
> - 布局顺序统一为 `[layout_base 前缀] + [各非 layout base 的字段块（MRO 顺序追加）] + [本类声明]`；
> - `field()/field_index()` own-first：本类声明先查，再 base 链、再 layout_base 链；**同名遮蔽保留**（子类声明遮蔽基类，Python 语义）；
> - `add_implicit_field` 能在 `field()` 里找到基类块里的字段 → 不再造遮蔽的隐式槽。
>
> **探针**（`PYADAMAS_DUMP_LAYOUT=1`）：`block A -> start 1`、`field x -> 1 (block/implicit)`——
> `A.x` 在 C 里的下标从 **-1 变成 1**（B 的字段数 + 0）。
> 护栏**保持生效**：两个状态基类的类定义继续明确拒绝（本步不修 thunk，方法体读基类字段仍会错，不能放行）。
>
> 阶段证据（临时短路护栏时）：`cb.x` = **1**（属性访问正确，写进 A 的块槽）、`cb.get()` = **2**（CPython 1 1）——
> 后半正是下一步 thunk 要修的。
>
> 语料：628 标准库默认口径 **496/623 不降**；本仓库 **899 个 `.py`：781 通过**（780 + 新增的 `mi_layout_base.py`），零新拒绝；
> 六形态与 CPython 逐字节一致；12 基准 asm 与 master 逐字节相同。
>
> 闸门：套件 **498** / difftest **468** / check_size **PASS**（`func.cpp` 正好 1200 行，后续改动要放 `mi_guard.cpp` 或 backend） / ctest 9/9。
## [0.505] - 2026-11-25

> feat(typecheck)：**多继承的布局前缀选择**——「mixin 在前、状态基类在后」的写法现在正确且被允许。
>
> 0.503 的护栏把「非主基类的方法访问自有字段」变成明确报错，但把一类**常见且本该正确**的写法也挡住了：
> `class C(Mixin, State)`（mixin 无字段、状态基类在后）。本版让 **layout base = MRO 里第一个带字段的基类** 提供布局前缀：
>
> - `ClassInfo::layout_base` 按 MRO 顺序取第一个有字段的基类（没有就退化成无前缀，与 0.503 相同）；
> - 字段偏移一律相对 layout base 计算；**`field()`/`field_index()` 也走 layout_base 链**——否则子类 `self.x = ...` 会被当成新字段补出第二个槽，
>   方法体读到没赋值的那个（实测就是 `9 0`）；
> - 接口导出顺序改成**运行期布局顺序**（layout_base 链 → 本类）。
>
> **接口结论：不需要 bump 格式版本**。字段**偏移不进接口**（`encode_module_interface` 只写名字/类型/默认值），
> 导入方的 `field_index` 就是编码列表里的位置、解码端 `base_fields` 留 0 按收到顺序压平；只要「导出顺序 = 运行期布局顺序」两边就一致（已在 `module.cpp` 注释里写明）。module format 14 不动。
>
> **语义修复实证**：`ThreadingUDPServer(ThreadingMixIn, UDPServer)` 之前 `s.sock`/`s.bind()` 得到 `9 0`（读到错误槽），现在 `9 9` 与 CPython 一致；
> `Combo(Mixin, State)` 输出 `11 m`（改前要么被拒要么读错）。
>
> **语料**：标准库默认口径 **496 → 496**（没有净增——被拒的 5 个文件里 2 个转为「既有的其他错误」、3 个是**两个状态基类**的字段交错，前缀选择原理上解决不了，要 adjustor thunk）；
> 本仓库 **898 个 `.py`：780 → 780，零新拒绝**。六形态回归 `tests/run/mi_layout_base.py` 与 CPython 逐字节一致；`04_containers`/`09_class`/`19_comprehension` 的 asm 与 master 逐字节相同。
>
> **判负**：「两个状态基类、字段交错」的形态（`xmlrpc/server.py`、`urllib/request.py`、`xml/dom/expatbuilder.py`）没有硬上，
> 需要 adjustor thunk（[mi-layout.md](mi-layout.md) §3 B 的另一半），留作后续。
>
> 闸门：套件 **498** / difftest **468** / check_size **PASS** / ctest 9/9。
## [0.504] - 2026-11-25

> fix(runtime)：**修掉保守 GC 模式的 heap-use-after-free**（3.10）。根因不是「收集触发点」，也不是「没扫寄存器」，
> 而是**运行期有一处静态缓存持有堆对象却没登记为 GC 根**。
>
> `runtime/rt_any.c` 的常量 float 盒缓存 `any_fcache[256]` 是静态全局数组，直接持有 256 个堆对象：
> - **默认模式按 RC 清扫**：盒子由缓存持有一份引用（`rc>=1`），永远不会被回收；
> - **`PYADAMAS_GC_CONSERVATIVE=1` 做全量可达性**（不看 RC）：盒子只被这个静态数组指着，从 `gc_globals`/临时值栈/帧记录/寄存器都不可达
>   → 被 `gc_collect` 当垃圾清扫 → 下一次命中同一槽时 `pa_rc_inc` 打在已释放块上（ASan 栈：`pa_rc_inc_fast ← pa_any_from_f64`，
>   free 侧 `gc_free_body ← gc_collect ← rc_alloc_core`）。
>
> **修法**：照 `rt_exc.c` 的 `exc_root_slots` 同一套既有做法，把 256 个槽的**地址**用 `pa_gc_add_roots` 登记成根；
> 登记放在 `any_float_box()` 里**首次装箱时执行一次**——命中路径（一次哈希 + `rc_inc`）零代价，且必然先有一次装箱，根一定已登记。
>
> **验证**：`161_gc_conservative_equiv` 保守模式 rc=0、输出 `900060` 与默认模式**逐字节相同**（加/不加 `--asan` 都对）；
> 新增 ctest **`pyadamas_gc_conservative`**（`tests/gc_conservative.sh`：默认与保守两跑都必须等于 expect 且互相一致），ctest 8 → **9** 项。
> 默认模式零影响：生成代码 asm（`04_containers`/`09_class`/`19_comprehension`）与 master 逐字节相同，改动只在运行期 `.c`；
> 套件 497、difftest 467、check_size PASS、ASan 编译器的全套件 497 全绿无报告。
>
> **教训**（写进 [memory-debug.md](memory-debug.md) §9.1）：运行期里任何「静态数组/全局缓存持有堆对象」的地方都**必须** `pa_gc_add_roots`；
> 已登记的有 `exc_root_slots` 与 `any_fcache_roots`，新增缓存时照抄。
>
> 闸门：套件 **497** / difftest **467** / check_size **PASS** / ctest **9/9**。
## [0.503] - 2026-11-25

> fix(typecheck)：**多继承「非主基类方法读错字段」从静默错值变成明确报错**（3.9）。
>
> 之前 `class C(B, A)`（A 不是主基类）时，A 的方法里 `self.x` 按 **A 自己的布局**取偏移，实际读到的却是 B 的字段（实测 `cb.get()` 返回 20，CPython 10）；
> 而 `cb.x` 属性访问是对的——错只发生在方法体内。
>
> **护栏的精确条件**（三条同时满足才拒绝，写在 `typecheck/mi_guard.cpp`，class 定义处）：
> 1. 非主基类 A 的 `methods_access_own_fields` 为真（方法体里有 `self.<字段>` 的读或写，静态记录）；
> 2. A 链上的字段在 C 的布局里**真的挪了位置**（同名字段落在同一偏移也算安全；菱形共享祖先偏移不变 → 放行）；
> 3. 不是导入类、不是 open 类（静态判不准就保守放过）。
>
> 报错：`base 'A' of 'C' is not the primary base and its methods access instance fields; put A first or make it a stateless mixin`。
> 单继承、主基类（第一个）、纯 mixin、菱形、`super()` 链**全部放行**（五形态输出与 CPython 逐字节一致）。
>
> **语料**：标准库默认口径 **499 → 496/623**（新拒绝 5 例，全部是「状态基类放在第二位、其方法按自己布局读字段」的真会算错组合）：
> `socketserver.py:722`（`ThreadingUDPServer(ThreadingMixIn, UDPServer)`）、`_pyio.py:1393`、`xmlrpc/server.py:938`、
> `urllib/request.py:1199`、`xml/dom/expatbuilder.py:803`。本仓库 **898 个 `.py`：0 新拒绝**。
>
> 下一步（已写进 [mi-layout.md](mi-layout.md) §6）：**主基类没有字段时，让第一个带字段的基类提供布局前缀**——
> 这样「mixin 在前、状态在后」的常见写法会变正确且被允许，上面 5 例里至少 3 例会自己消失；
> 剩下真正需要 adjustor thunk 的是「非主基类字段与主基类字段交错」。
>
> codegen 不动：`04_containers`/`09_class`/`19_comprehension` 的 asm 与 master 逐字节相同。
> 闸门：套件 **497** / difftest **467** / check_size **PASS** / ctest 8/8。
## [0.502] - 2026-11-25

> fix(typecheck)：**修掉 `check --imports` 的 heap-use-after-free**（3.13）。根因不是栈溢出——是 `TypeChecker::check()` 的**重查轮**。
>
> **根因（ASan 实证）**：重查轮里 `*this = std::move(next)` 会析构旧的 `classes_`/`all_classes_`，释放**上一代 `ClassInfo`**；
> 而 `Type::cls` 是裸指针，跨轮缓存 / 接口快照 / result-hint 重放仍可能引用它。随后编码接口时读已释放对象的 `std::string` 长度 →
> `memcpy` 越界 → SIGSEGV。循环 import 只是把这条路径的触发概率放到最大（深 import 图 × 多轮重查）。
> 实测 import 栈最深只有 19 层、`ctx.loading` 早有环检测——所以**「加 visited 集合」治不了这个崩溃**。
>
> **修法**（循环依赖保持合法，不靠拒绝）：
> 1. `TypeChecker` 加 `retired_classes_`：重查轮结束前把上一代 `classes_`/`all_classes_` 转移进去，随 checker 析构——悬空裸指针变成有效对象；
>    编码只读 `cls->name`/`qualname`，语义不变（asm 不变可证）。
> 2. `enc_sig`/`enc_type` 加防御性递归保护：签名展开栈上重复 `FuncInfo` → 按 unknown 签名编码；类型编码深度上限 64，超出按 unknown 收口。
>
> **证据**：`sched.py`/`random.py`/`threading.py` 等 16 个原崩溃文件 rc 全部 ≤1（**0 个 139**）；
> **628 文件 `--imports` 全量 623/623 全部正常完成**（通过或报类型错，无一崩溃）；`--imports` 通过数 **155 → 164/623（26.3%）**，
> 之前那个「下界」注记已作废；`deps --json` 对同样文件明确报错（`cannot find module '_heapq'`）而不是崩。
> 同一个 UAF 位于共享的 `TypeChecker::check()`，`build`/`deps` 走同一条路——这次修复对它们同样生效（只是 build 对这批文件先因类型错返回，没走到崩点）。
> 12 基准 asm 抽查（`04_containers`/`09_class`/`19_comprehension`）与 master 逐字节相同。
>
> 闸门：套件 **496** / difftest **466** / check_size **PASS** / ctest 8/8（`pyadamas_check_imports` 新增「循环 import 必须终止（rc ≤ 1）」）。
## [0.501] - 2026-11-25

> feat(typecheck)：**窄化的三类假阳性修掉**（walrus / `isinstance` / 模块全局条件 None）；`if xs:` 空容器的 -O0 崩溃**三档都不可复现**（判负）。
>
> **一、三类假阳性**（0.499 引入的 6 个文件里修掉 5 个）：
>
> | 类 | 根因 | 修法 | 语料 |
> |---|---|---|---|
> | walrus | `narrow_on` 只认 `Name`，`if (n := d.get(k)) is not None:` 的对象是 `NamedExpr` | 新增 `narrow_name_of()`：Compare 两侧/裸真值/`isinstance` 第一参都取 walrus 的 target；提前返回模式随之窄化 | graphlib.py 转绿 |
> | `isinstance` | 没有 refinement，`if isinstance(x, C)` 之后仍是联合 | 新增 `narrow_isinstance()`：真值分支收窄成被检查类（联合里必须至少一个兼容成员），假值分支去掉「该类或其子类」的成员；只认用户类 + Name/walrus 目标，内置类型与属性目标保守不动 | pyclbr.py 转绿 |
> | 模块全局「先定义后初始化」 | `_P = None if cond else 92` 把未标注全局并成 `T \| None` | `declare()`：模块级**未标注**名字已有确定类型时，后续条件赋值的 `T \| None` 不再并回 None（与既有「None 占位 → unknown」同口径）；只对纯 Python Optional 生效，cimport 的指针/struct Optional 保留 | 2 个文件间接受益 |
>
> **628 文件基线默认口径：495/623 → 499/623**；本仓库 **896 个 `.py`：778 → 779，零新拒绝**。
> 剩下 4 个文件根因不同（不是这三类）：`base64.py` 的 `[None]*256` 占位靠 `except TypeError` 兜底、`xml/dom/pulldom.py` 的字段版同类占位、
> `modulefinder.py` 的 `determine_parent()` 静态上确实是 Optional、`multiprocessing/util.py` 的条件定义——要流分析或外部不变量，已记录。
>
> **试过并回退的两条**（避免下轮重试）：① 在 `stmt_if` 分支合并处也去掉 None → 分支合并结果同时决定降级表示，`print(d.get(k))` 把 `None` 打成 `0`（`70_dict_get.py` 抓到）；
> ② 放开排序比较的 Optional 诊断 → 对那 6 个文件一个都没修好。
>
> **二、3.12 判负**：`if xs:` 对空 list 在 **-O0/-O1/-O2 三档都不复现**（0.498 构建同样不崩），扩展 20+ 形状（list/dict/set/str/tuple、字段、形参、全局、walrus、`and` 链、`while`、生成器、`try`、`not`/`bool()`/`len()`、空字面量、`list \| None`）全绿。
> 补了全族回归 `tests/run/empty_container_truth.py` 并接进 `tests/opt_levels.sh`（ctest 的 `pyadamas_opt_levels` 会跑 -O0/-O1/-O2 三档）。
>
> **三、顺带发现一个真实崩溃（既有，0.498/0.500 同样复现）**：`check --imports` 在**循环 import** 上栈溢出（`encode_module_interface → enc_sig → enc_sig …` 无深度保护），
> `sched.py`/`random.py`/`threading.py` 等 20+ 文件 rc=139；`build` 不受影响（同一文件 rc=1 正常报错）。
> 影响：**`--imports` 的 155/623 基线不可信**（被崩溃压低），已记为路线图 **3.13**（visited 集合 / 深度上限），修好后要重测。
>
> 闸门：套件 **496** / difftest **466** / check_size **PASS** / ctest 8/8（含 opt_levels 三档）。
## [0.500] - 2026-11-25

> feat(frontend) + 判负：**补原生/标准库接口表**（33 个成员 + 12 个运行期函数）、**`except (A, B) as e` 类型解析**；
> **「装饰器保留原签名」判负**。
>
> **一、接口表（0.498 那条「json.dumps 返回 unknown」的真正原因不是缺签名）**：
> 审计前先发现一个**口径问题**——默认 `check` 单文件根本不解析模块成员（`module_lookup_` 为空时 `import` 只声明 unknown），
> 所以在 `check --imports` 下实测：`json.dumps(...)` 本来就是 **str**、`sys.argv[0]` 本来就是 **str**。
> 用正确口径重新审计 16 个原生模块 358 个成员：**missing 130 → 94、unknown 27 → 28**（那 1 个是 `os.altsep` 为 `None`）。
> 补的 36 项只挑「类型确定**且运行期有实现**」的（只补类型 = 静默 0/None）：
> - 22 个常量进 `stdlib/sigs/*.pysig`（`sys.platform`/`version`/`byteorder`/`maxunicode`/`hexversion`/`api_version`、
>   `os.name`/`curdir`/`pardir`/`altsep`/`pathsep`/`extsep`/`devnull`、`re.A`/`ASCII`/`U`/`UNICODE`/`NOFLAG`、`datetime.MINYEAR`/`MAXYEAR`），值用 CPython 3.14.4 在目标平台核对；
>   `sys.version_info` 走 `builtin_value` 的 `MakeTuple`（`(3,14,4,"final",0)`）。
> - 12 个新运行期函数 + 1 个别名：`os.getuid/geteuid/getppid/urandom/strerror/replace`(=rename)、
>   `time.time_ns/monotonic_ns/perf_counter_ns/process_time`、`random.randbytes`（**与 CPython 同种子逐字节一致**）、`sys.intern`、`re.purge`（无缓存，no-op）；`itertools.batched`（eager list）。
> - 剩余 94 项**分类判负**（缺 fd/stat/uname/解释器身份/惰性对象/类工厂/子进程面），清单在 [frontend-gaps.md](frontend-gaps.md) §8.5。
>
> **二、「装饰器保留原签名」判负**：宽/窄两版都试了，**628 语料默认口径 500 → 496**，四个文件：
> `asyncio/tasks.py`（`@types.coroutine` → awaitable）、`importlib/resources/_common.py` 与 `pkgutil.py`（`@singledispatch` → 带 `.register` 的包装）、
> `test/support/import_helper.py`（`@contextmanager` → 上下文管理器）。「装饰器不改变签名」在本项目语料里**不成立**，
> 保留签名等于把「全放行」换成「按不成立的前提检查」，必然新增拒绝 → 触红线。代码保持 master 语义，理由写进 `func.cpp` 注释防下一轮重试。
> 安全落地需要给每个已知装饰器建模返回类型（`contextmanager`→`_GeneratorContextManager` 等），记在 [frontend-gaps.md](frontend-gaps.md) §8.3。
>
> **三、`except (A, B) as e`**：逐分支解析，全部分支同类型时用该类型（含富异常类），否则退 `Exception`（多类型方法面是交集，不硬造 union）。
> 另复核：「`except E as e` 是 unknown」**也是误判**——它本来就是 `Exception[E]`（`need_int(e)` 会报 `got 'ValueError'`），
> 之前看到的「放行」来自 `call_method.cpp` 对 `TyKind::Exception` 一律允许成员访问。**不收紧**内置异常成员访问（CPython 允许 `e.user_id = 1`）。
>
> 语料：628 文件 **500/623 → 500/623（零回归、零新增通过）**；本仓库 889 个 `.py` **775 → 775**。
> 闸门：套件 **493** / difftest **463** / check_size **PASS** / ctest 8/8。
## [0.499] - 2026-11-25

> feat(typecheck)：**`T | None` 真窄化** + 未判空使用的编译期诊断。
>
> 之前 `def f(x: int | None) -> int: return x + 1` 能过 check、运行时崩——`None` 在联合里被降解。
> 现在支持：
> 1. `if x is not None:` / `if x is None: ... else:` 两个分支；
> 2. **提前返回**（`if x is None: return` 之后余下部分 x 非 None）；
> 3. 真值判断 `if xs:`（可选容器）；
> 4. `assert x is not None`；
> 5. 短路 `x is not None and x.foo()`（`narrow_all` 收全部合取项）；
> 6. 多层联合 `int | str | None`（只去掉 None）；
> 7. 窄化后重新赋值能重新合并；附加：`while cur is not None` 循环体、`if self.x is not None` 属性窄化。
>
> **未判空即使用**（算术/排序比较/属性/下标）报：
> `error: 'x' may be None here; check it first (if x is not None:) or use assert`；3 条 fail 用例钉住。
>
> **628 文件基线**（本机 CPython 3.14.4，本次实测 626 个 `.py`）：
>
> | 口径 | 通过 |
> |---|---|
> | 语法 | 621/626（99.2%） |
> | 默认（本版） | **495/626（79.1%）** |
> | 旧范式（`--no-imports --lenient-names`） | 500/626 |
>
> 相对 0.498 的默认口径（500/623 = 80.3%）**少 6 个文件**，全部是「未判空」诊断，根因分三类、逐例记录在
> [docs/type-narrowing.md](type-narrowing.md)：① walrus `(n := x) is None` 后的窄化未做；② `isinstance` 窄化未做；
> ③ 模块全局「先定义后初始化」的假阳性。**这是本版明确接受的代价**：诊断本身要抓的就是「可能为 None」，「宁可漏报」在这一条上让位于「别把 None 崩带进生产」。
> 仓库内 552 个 `.py` 相对同版本基线**零新拒绝**（3 例是新增 fail 用例本身）；为了通过新诊断改了 1 个既有用例
> （`tests/run/70_dict_get.py` 里 `d.get()` 之后补 `assert v is not None`）。
>
> **已知边界**：跨模块联合仍退化 unknown（接口不序列化成员表）；`isinstance`/walrus 窄化未做；
> `unknown` 上的下标/方法不动；C 结构体指针的 `T | None` 不报（裸指针本来就可为 NULL，否则三例误杀）。
>
> 闸门：套件 **492** / difftest **462** / check_size **PASS** / ctest 8/8 / `--perf` warn=0。
## [0.498] - 2026-11-25

> fix(frontend) + fix(runtime)：**类体里的方法别名**、**`unknown` 下标按运行期种类分派**；「局部变量丢类方法面」复核后**判负**。
>
> **一、类体方法别名**（`__str__ = shortDescription` 这类，CPython stdlib 很常见）：根因两层——类体遍历只处理带装饰器的 `def`
> （普通 `def` 直接 break，名字没进类体命名空间），且方法不是一等值。修法：把已执行的 `def` 记进类体命名空间，
> `NAME = 前面的方法名` 建成「共享实现的两个方法槽」（含 `property`/`classmethod`/`staticmethod` 别名），降级层跳过该赋值。
> **628 文件基线默认口径：480/623 → 500/623（80.3%）**；`undefined name` 触发文件 45 → 18。
> 上限说明：本机 lenient 实测 504/623（默认口径可达上限），差 4 个来自模块级条件导入 / `globals().update()`；
> 另有 6 个文件是「方法默认参数引用类体名字」（`__marker`/`LOG_USER` 等），要动「def 点求值默认值」这一新机制 → 判负并记录。
> 回归：`tests/run/class_body_alias.py`、`tests/fail/class_body_alias_forward.py`（顺序语义：别名只看得到前面的 `def`）。
>
> **二、「局部变量丢类方法面」判负**：12 组形状在两个构建（0.495 / 9ca49e5）上都不复现；唯一产生那句错误消息的是**静态类型确实为 unknown 的接收者**，
> 链式与局部变量**一样**报错——所以「只对链式走类方法面」是误诊，真问题属于 `unknown` 语义（要动降级层运行期方法表或 P2 标签）。
> 留 `tests/run/local_var_method.py`（三条路一致）与 `tests/strict/unknown_receiver_method.py`（unknown 接收者仍明确拒绝）钉住。
>
> **三、`unknown` 上的下标不再静默按机器语义解码**：全仓 **195 处（193 读 / 2 写，36 个文件）**，其中 5 个是核心 stdlib 模块（contextlib/itertools/pprint/string/tarfile），
> 所以「编译期禁掉」判负（它们会编不过）。改为运行期分派 `pa_any_index`：先用 `pa_unknown_is_heap` 探针判堆，再按头上 kind 分派 list/tuple/dict/str；
> 其它堆对象与 JSON null/bool 抛 `TypeError`；探针看不见的**标量元组**沿用机器语义（小整数和 `None` 抛 `TypeError`）。
> 实测修复：unknown dict 的「假象 IndexError」→ `7`、unknown str **段错误** → `b`、小整数**段错误** → `TypeError: 'int' object is not subscriptable`，与 CPython 一致。
> 诊断开关 `PYADAMAS_DIAG_UNKNOWN_INDEX=1` 留在降级层（静态缓存，零热路径成本；注意**统计必须带 `--rebuild`**，否则产物缓存会让数字假小）。
> **剩余缺口**（已记录）：大整数仍段错误（`1<<40`）、bytes 无运行期标签（按 str 口径）、unknown 实例的 `__getitem__`、下标写 2 处——都要 P2 标签默认开或让标量元组进 RC 索引。
>
> `typecheck/func.cpp` 触到 1200 行硬限，拆出 `typecheck/class_alias.cpp`（76 行）。
>
> 闸门：套件 **491** / difftest **461** / check_size **PASS** / ctest 8/8 / ASan 491 无报告。
## [0.497] - 2026-11-25

> perf(rc)：**存活堆索引从哈希表换成统一页位图**——干掉 unknown 密集代码里「探测税」的访存。
>
> **取证**：`pprint` 里 `rc_index_has` 6.76M 次，其中 5.34M 是字符串/浮点盒（走叶子位图），但**每次都先做一次失败的主表线性探测**：
> 主表 cap=256、装载 67.6%、1.70M put/del 制造墓碑 → **54.0M 槽 / 5.34M 次 = 10.1 槽/次**。瓶颈是访存次数，不是哈希计算。
> 堆页跨度 120,838 页（470MB）但**同时只存活 33 页** → 扁平表不可行。
>
> **结构**：16 字节对齐 → 统一页位图（4KB 页 = 256 槽 = 4×u64，表项 `{base, live, bits[4]}` = 48B = 一条 cache line，
> 开放寻址装载 ≤50%，无链表、无 per-page malloc）；页空则位图清零并摘除表项；命中 **1 次访存**。
> 非 16B 对齐走旧哈希（现在只剩静态字面量），且**非对齐的真对象 put 当场 panic**（`unaligned_heap_put` 计数：38 个基准 + 全套件 + ASan + canary 全 0）。
> `PYADAMAS_RC_INDEX=hash` 可整条回退。索引只用于探测/插入/删除，**无遍历依赖**（GC 标记走 heap_head）。
>
> | 证据（同二进制切换 hash↔bitmap） | hash | bitmap | Δ |
> |---|---|---|---|
> | pprint 墙钟（25 轮奇偶交换，中位/p25） | 0.4730/0.4680 | 0.3811/0.3782 | **−19.4%/−19.2%** |
> | pprint 对 master（d6937be） | 0.4414/0.4370 | 0.3859/0.3805 | **−12.6%/−12.9%** |
> | difflib | 0.2104/0.2021 | 0.1836/0.1782 | −12.7%/−11.8% |
> | `instructions:u`（我复测 3 轮） | 2.62–2.70G | 2.37–2.38G | **−6.4% vs master** |
> | 大存活堆（30,287 活页 / 500k 对象）RSS | 154.1MB | **146.6MB** | −5%（页表峰值 65,536×48B≈3.1MB） |
>
> **自检**：`PYADAMAS_RC_INDEX_SELFTEST=1` 在 `pa_gc_init` 里跑（2048 个合成地址 + 512 个真实对象：登记 → 全命中 → 全撤销 → **全不命中**；
> `live_pages/live_objs/entries` 归零），两模式均 PASS，失败即 panic。地址复用路径由 del 清位 + `live==0` 摘除表项兜住。
>
> **中性/局限**：textwrap、zipfile 本来 index 调用就少（4.8 万/6.4 万），结构对它们中性；12 基准 `instructions:u` 零回退（最大 binary_trees −0.50%）。
> 另发现一个**既有**问题：`PYADAMAS_GC_CONSERVATIVE=1` 时 `161_gc_conservative_equiv` **段错误**，在 d6937be 的纯净构建上同样复现（rc=139），已记为路线图 3.10，非本版引入。
>
> 闸门：套件 **488** / ASan 488 / canary 452 / hash 回退 488 / difftest **458** / check_size **PASS** / ctest 8/8 / `--perf` warn=0。
## [0.496] - 2026-11-25

> refactor(regalloc) + perf(vcall)：**删掉已废弃的 live range splitting**，并把**虚调用守卫改成比较实现指针**。
>
> **一、删 RA_SPLIT 死代码**（净删 **925 行**，22 文件）：`assign_slot_regs_split`（含 SpillPlacement 求解与块频次模型）、
> `slot_loc_`/`slot_reload_in_`/`slot_spill_out_` 及 `block.cpp` 的 5 处使用、`spill_placement.{cpp,h}`、
> `Program.rc_split` 与 manifest 的 `rcsplit` 行、`generate_asm` 参数。默认路径的槽位置回到「整函数固定寄存器或栈槽」。
> `PYADAMAS_RA_SPLIT` / `PYADAMAS_SPLIT_MAXREG` 保留明确报错（rc=2），不让 A/B 脚本以为在跑另一条分配路径。
> **验收**：删前/删后 12 个基准 asm **逐字节相同**（12/12，`cmp`）。
>
> **二、虚调用守卫（2.5）**：守卫从「比较 vtable 地址」改成「取 vtable 槽里的实现指针再比较」，
> 并按实现指针合并候选（Square 继承 Rect.area → 同一个 `.Lf5`），比较顺序按「该实现覆盖几个类」降序。
> obj_dispatch 的守卫从 **3 次比较降到 2 次**（内层每条守卫 10 → 8 条指令）。
>
> | 证据 | 改前 | 改后 | Δ |
> |---|---|---|---|
> | 指令/内层迭代 | 48.62 | 47.67 | **−1.95%**（与按类频率 34/33/33 算出的 −0.95 条完全吻合，不是布局巧合） |
> | `idq.dsb_uops`/迭代 | 39.22 | 36.98 | −5.7%（前端总 uop 44.31→43.81） |
> | `cycles:u`（单事件） | 799.1M | 786.8M | −1.6%，IPC 持平 |
> | 墙钟（15 轮×BURST=5，A/A ±0.1%） | 298.55ms | 292.75ms | 中位 **−1.9%**、p25 −1.8% |
>
> **已丢弃的反例**（值得记）：只换实现指针但**保持类顺序** → 指令净变化 +0.01 条/迭代、墙钟 **+1.1%**
> （多出的 vtable 槽 load 有 load-to-use 依赖，抵掉省下的 `leaq`/`cmp`）。所以「合并组优先」是收益成立的前提。
> 已知取舍：合并组先比会让**单类占绝对多数**的站点每次调用多花 4 条；现有 12 基准没有守卫点，闸门测不到这类回退。
> 12 基准 asm 逐字节不变、`instructions:u` ±0.03%。
>
> 闸门：套件 **488** / difftest **458** / check_size **PASS** / ctest 8/8 / ASan 488 / `--perf` warn=0。
## [0.495] - 2026-11-25

> feat(big)：**`round(big, ndigits)` 与三参 `pow(b, e, m)` 落地**，并修掉 int 三参 pow 的溢出。
>
> - `pa_big_round`：`ndigits >= 0` 原样返回；`< 0` 按 10^k 做**半程到偶数**取整（与 CPython 的
>   10 进制口径一致：round(15,-1)=20、round(25,-1)=20、round(35,-1)=40、round(-25,-1)=-20）。
>   ndigits 上限 1e6（防病态分配）。
> - `pa_big_pow_mod`：平方乘 + 逐步取模。`m == 0` 抛 `ValueError`（CPython 同文案）；`m` 为负
>   时结果取负一侧（pow(2,3,-5) = -2）；`m == 1` 给 0。**负指数仍拒绝**（CPython 3.8+ 走模逆，
>   要 big 的扩展欧几里得；消息里写明差异）。
> - **顺带修 int 三参 pow 的溢出**：旧实现先 i64 算幂再取模，`pow(2, 100, 1000000007)` 回绕成 0
>   （CPython 976371285）。现在 int 也走 big 模幂再回程 `BigToI64`（模数在 i64 内 ⇒ 结果无损）。
>
> 对拍：`tests/run/big_round_powmod.py`（16 行，含超 i64 的 big 底数/指数/模数、负数、m=1、
> `pow(2,100,m)` 溢出回归）与 CPython 3.14.4 **逐字节相同**。
>
> 闸门：suite / difftest / check_size / perf / **ASan**（动了 runtime）。
## [0.494] - 2026-11-24

> fix(runtime)：**未捕获异常的 traceback 行号**（以前一律 `line 0`）。
>
> 根因不是「合成指令没行号」（0.490 的判断），而是**返回地址归因**：`.LdbgEnd` 哨兵紧跟在
> `raise` 的调用指令之后，回溯用 `ip` 查表时正好落在哨兵上（`func == NULL` → 查不到），
> 于是回落到函数起点项（行 0）。
>
> 修法（只动 `runtime/rt_debug.c` 的查表，不改 codegen、不增指令）：先按 `ip` 查
> （内联点的 `.Ldbg`，函数名与行号都准），再退回 `ip-1`（call 恰好是某行最后一条指令的口径）。
>
> 效果：`raise` 在 `inner` 第 8 行 → 回溯显示 `line 8`（内联后函数名是 `__pa_main`）；
> 以前是 `line 0`。回归 `tests/traceback_lines.sh`（ctest `pyadamas_traceback_lines`，断言
> `line 8` 且**不**出现 `line 0`；回溯打 stderr，`tests/run` 的 expect 只比 stdout，所以单独成脚本）。
>
> **边界（不修，写进 [docs/compatibility.md](docs/compatibility.md)）**：回溯帧数 = 物理调用栈。
> `-O2` 把小函数内联（实测带循环的 `inner`/`outer` 也被内联），物理栈只剩一帧 → 默认只显示一帧；
> `-O0` 下是完整链（实测 3 帧）。要 CPython 的逻辑帧，得「不内联会抛的函数」或「抛点记录 IR 调用链」，
> 都是独立决策。列号与 `__traceback__` 维持现状（后者仍是显式「未实现」错误）。
## [0.493] - 2026-11-24

> fix(runtime)：**`re.match` 不锚定**（语义 bug：含元字符的模式下 `match` 等价于 `search`）。
>
> 根因：`rt_re.c::find_from` 的「首字节剪枝」在 `mode != 0`（match/fullmatch）时**也推进起点** ——
> `a.c`.match("xabc") 的首字节 `a` 在第 1 位，剪枝把 s0 推到 1 后就匹配了。纯字面量模式走
> `memcmp` 快路径，所以 `re.match("abc", "xabc")` 碰巧是对的，掩盖了这个 bug。
> 修法：`mode != 0` 时只判断起点首字节是否命中集合，**不推进**（不命中直接返回 0）。
>
> 复核对拍（`tests/run/re_anchors.py`，16 个模式 × 14 个主语 × match/search/fullmatch
> = 224 行）：与 CPython **3.14.4 逐字节相同**，覆盖 `.`/`[]`/`*`/`+`/`{m,n}`/`\A`/`\Z`/`\z`/
> `^`/`$`/`|`/分组。`\A`/`\Z`/`\z` 本来就是锚点（`parse_escape` 已实现，本轮复核确认，没有代码改动）；
> `search` 语义不变。
>
> 边界：`Pattern.match(s, pos[, endpos])` 的 `pos`/`endpos` **仍不支持**（`Pattern.*` 只收 1 个参数，
> 编译期报错），写进 [docs/compatibility.md](docs/compatibility.md)。`fnmatch` 编译期前置 `^` + `end()`
> 的绕法仍然正确，未改动（`tests/run/fnmatch_anchor.py`、`539_fnmatch_accept.py` 全过）。
## [0.492] - 2026-11-24

> perf(stdlib)：**标准库第二轮全量扫描**——54 个模块逐个过（`docs/stdlib-round2.md`），12 个文件改动，5 个可测收益。
>
> | 模块 | before → after | 手法 |
> |---|---|---|
> | fnmatch | 89.5 → 67.7 ms（−24%） | `translate` 缓冲 + `re.escape` 查表 + 锚定修正 |
> | shlex | 20.9 → 17.6 ms（−16%） | `quote`/`join` 逐字符拼接改缓冲 |
> | csv | 79.0 → 67.0 ms（−15%） | `writerow`/`_format` 提升循环不变量与 `len` |
> | pprint | 493.5 → 439.8 ms（−11%） | 6 处 `out = out + …` 改 `list[str]` + `join` |
> | urllib.parse | 152.2 → 136.5 ms（−10%） | scheme/safe/hex 三张 128 项查表、`_clean` 短路 |
>
> **修掉一个真 bug**：`fnmatch` 此前**不是整串匹配** —— `fnmatchcase(".hidden", "[a-z]*")` 返回 True
> （CPython False）。根因在 `re` 引擎：**含元字符**模式的 `Pattern.match` 与 `search` 同义、`\Z` 不当锚点
> （纯字面量模式走快速路径，反而是对的）。fnmatch 侧改为编译时前置 `^` + `search` + `end()` 补齐终点；
> `translate()` 输出保持原样（539 个用例钉着它）。另外做了两轮差分：7082 个模式的 `translate` 等价性、
> 14400 组 `fnmatchcase` 匹配矩阵，与 CPython 0 差异。
>
> **新增资产**：`tests/bench/cases` 17 → **38**（tarfile/zipfile/configparser/argparse/difflib/pprint/csv/
> urllib.parse/glob/shlex/calendar/shutil/io/queue/struct/zlib/gzip/traceback/secrets/copy/fnmatch，全部 <1s、
> 确定性、`--filter` 可单跑）；5 个 CPython 对拍用例（`fnmatch_anchor` / `urllib_quote_edges` / `shlex_quote_roundtrip` / `csv_quoting` / `glob_match_escape`）。
>
> **判负/跳过**（根因见文档 §2/§6）：`io.StringIO.write` 每次 O(n)（str 不可变，要可变缓冲）、
> `functools.cmp_to_key` O(N³)（`sorted` 不支持 key 对象比较）、zlib 包装层已到顶（0.85–1.2×）、
> struct 1.3×（缺静态 fmt 特化）、glob 由 syscall 主导；另 19 个模块是常量/薄封装/编译期占位。
>
> **编译器侧观察**（只记录未改 `src/`，`docs/stdlib-round2.md` §7）：`re` 缺 `\A`/`\Z` 锚；「先赋给局部变量」
> 会丢类方法面（链式才行）；`unknown` 上的下标按机器语义解码；分支内重新赋值会丢 `list[T]` 注解。
> 其中「queue 取元组下标 SIGSEGV」一条**未能稳定复现**，已在文档里标注为待独立复现，不作为结论引用。
>
> 闸门：套件 **485** / difftest **455** / check_size **PASS** / ctest 7/7。
## [0.491] - 2026-11-24

> docs(compat)：把 3.5 的三项语义缺口写进「尚未实现的构造」相邻段落，每条给 3 行以内的最小复现。
>
> - **`for` 与内置构造函数拆开写**：`for x in obj` 对装饰/未装饰 `__iter__` 都正确（不是缺口）；
>   真正的缺口是 `list(obj)`/`tuple(obj)`/`set(obj)`/`sorted(obj)`/`sum(obj)` 对内建容器以外的对象报
>   `cannot iterate over 'Box'`，附 `RtFn::AnyIterList` 只吃内建容器、喂生成器报 `object is not iterable`
>   的实测证据与正确修法（复用 `stmt_for` 的 `dunder_call` 做「任意迭代器物化」新降级形状）。
> - **未初始化变量读 0**：`def f(c): if c: x = 1; return x` 调 `f(False)` 打印 0（CPython `UnboundLocalError`），
>   槽位零初始化、没有「已初始化」位；两条实现路径的代价都写清了。
> - **异常 traceback**：`e.__traceback__` 维持显式「未实现」错误；未捕获回溯只有单帧且行号 0（`.loc` 只在
>   `in.line > 0` 登记，异常边的合成指令没有行号）；列号缺失。行号可修（补异常边 `in.line` + 两帧回归），
>   其余判负。
>
> 另记录一条边界：**联合类型不跨模块序列化**（`enc_type` 对带成员表的 Unknown 仍走 unknown 码），
> 所以 `from lib import take; take(1.5)` 仍会放行——方向是「check 比 build 宽」，符合「宁可漏报」。
## [0.490] - 2026-11-24

> 取证（§3.5）：**三项已知语义缺口的结论都是负的**，逐条给最小复现、根因、要动的层。
>
> 1. **装饰后的 `__iter__`** —— 前提要修正：`for x in obj` **两种写法都能跑**
>    （`stmt_for.cpp` 走 `dunder_call(recv, ty, "__iter__")`，装饰与否都能拿到）。真正缺的是
>    **内置构造函数**：`list(obj)` / `tuple` / `set` / `sorted` / `sum` 对用户类直接报
>    `list() cannot iterate over 'Box'`（`lower/builtin_container.cpp` 只认内建容器种类；
>    实测**未装饰**也一样，不止装饰后）。修法要「把任意迭代器物化成列表」= 一段 for 循环形状的
>    降级（`__iter__` 的结果接 append 循环）；`RtFn::AnyIterList` 只能吃内建容器，喂生成器报
>    `object is not iterable`（本轮试过、已回退，证据记在这里）。要动的层：`lower/builtin_container.cpp`
>    + 复用 `stmt_for` 的迭代器构造。**判负**（工程量 = 一个新降级形状，不是补丁）。
> 2. **未初始化变量读 0** —— 复现：`def f(c): if c: x = 1; return x` 调 `f(False)` 打印 **0**
>    （CPython `UnboundLocalError`）。根因是槽位一律零初始化（`slot` 是栈帧里的裸字，没有
>    「已初始化」位）。两条路：① 每个槽加 init 位 → 每次 load 都要判，热路径摊成本；
>    ② 降级期做**确定性赋值分析**（CFG 已有，但要保守处理 try/循环/提前 return，且 `unknown`
>    与容器元素的「已赋值」没有静态定义）。**判负**（收益是错误信息，代价是每次读取或一遍数据流）。
> 3. **异常对象的 traceback / 列号** —— 三条实测：`e.__traceback__` 降级期就报
>    `exception attribute '__traceback__' is not implemented yet`；未捕获异常的 traceback 打印
>    `line 0, in inner`（只有抛出的那一帧，**没有调用者链**，且行号是 0）；DWARF 的 `.loc` 是
>    `.loc 1 <line>`，**只有行号没有列号**。**部分判负**：行号可修（`.loc` 已有，缺的是函数入口/
>    raise 点的 `.loc` 覆盖），列号与调用者链要改运行时 traceback 结构与 codegen 的每指令定位，
>    本轮不做；`__traceback__` 维持显式「未实现」错误（比返回 None 好）。
>
> 结论已写进 [docs/compatibility.md](docs/compatibility.md)「尚未实现的构造」相邻段落。
## [0.489] - 2026-11-24

> fix(typecheck)：**联合类型不再接受 float**（Bug A）；**回退 0.487 的 dunder 放宽**
> （Bug B：静默算错）。3.1 的 int/big 混合运算**判负**，根因写清。
>
> **Bug A（既有缺陷，被 0.487 的 union 签名暴露）**：`x: int | str = 1.5` 与
> `def g(o: int | str); g(1.5)` 都放行。根因：`parse_annotation` 里 `A | B` 走 `join()`，
> join 合不了就塌成 **`t_unknown()` 单例**，于是目标侧 `is_unknown` 早退、任何值都能进。
> 修法：用 `TyKind::Unknown + 非空 sub` 表示联合（**运行期表示与降级零改动**，仍是 8 字节字），
> 只在五处认它 —— `assignable`（目标侧按成员逐个判，且在 `is_unknown` 捷径**之前**）、
> `check_args` 的「未知形参→回填并放行」捷径、`ann_unknown`（只有显式 unknown/object 才早退）、
> `join_into` 的洞判定（联合不是待填的洞，否则 `Decimal | int | big` 会被 big 顶掉）、
> 比较 dunder 的第二参数校验。数值塔内部仍走 join（`int | float` → float，`int | big` → big）。
> 附带修好 `annotated as ...` 把联合拼成 `cast_int | str(x)` 的四不像消息。
> 回归：`tests/fail/f82_union_float.py`（赋值）、`f83_union_arg_float.py`（调用点）、
> `tests/run/571_union_types.py`（正向：int 成员收 int、`int | float` 收 int、本地类联合）。
>
> **Bug B（0.487 引入，静默算错）**：`Fraction(1,3) + 0.5` 输出每次不同的垃圾
> （`285998712124225/3` 之类），`Decimal + 0.5` 同样。
>
> 根因**比「漏了一个 raise」深一层**：0.487 把 dunder 形参放宽成 `Fraction | int | big`，
> 而 pyadamas 的**联合在运行期没有标签** —— 实测 `isinstance(unknown_int, int)` 为 **False**
> （只有装箱的 float 能被 `isinstance(o, float)` 认出来，未装箱的 double 与 int 都是裸字）。
> 于是 `_coerce` 里 int / big / float 三者不可区分，落到 `Fraction(o)` 时那个字被当成
> BigInt **指针**读 → 越界读 → 每次不同的数。加 `isinstance(o, float)` 也拦不住未装箱的路径。
>
> **处理：回退 0.487 的 dunder 放宽**（`stdlib/fractions.py` / `stdlib/decimal.py` 与 0.485
> 逐字节相同，`git diff 8c915c8 -- stdlib/` 为空），`Fraction(1,3) * 3` 回到编译期
> `unsupported operand type(s)`。**3.1 判负**：正确的做法是二选一 ——
> ① 类型检查层的数值提升（在 BinOp/比较/反射每个点插入 `Fraction(int)` 构造，影响所有
> 定义了算术 dunder 的类）；② 给联合加运行期标签（改 unknown 表示与 codegen）。
> 两者都是独立一层，**不能**在 stdlib 里用 `isinstance` 糊出来 —— 那正是这次静默算错的来源。
>
> **0.487 的结论更正**：当时写的「float 仍然被拒绝」不成立 —— `Fraction | int` 里的导入类
> 标注在没有 import 图时会塌成 unknown，`f + 0.5` 照样通过 check。现在 union 修好了，
> 但 `Fraction` 本身仍要在 `--imports` 下才解析得到，所以 `f + 0.5` 目前是**编译期放行、
> 运行期按未知域处理**；由于 dunder 已回退成 `o: Fraction`，float 走不到那些算式里
> （会先报 `unsupported operand type(s)`），不再有静默算错。
>
> **审计**：stdlib 里除这两处没有别的「union 形参 + 构造函数 fallback」；`Decimal.__init__`
> 的 `value: unknown` 是 CPython 兼容入口（`Decimal(0.5)` 本来就合法），保留。
>
> 验收：`tests/fail/f82`/`f83`、`tests/run/571_union_types.py`；Fraction 热路径与 0.485
> 逐字节相同（回退后无任何改动，无需重测性能）。
## [0.488] - 2026-11-24

> feat(big)：**divmod(big, …) 落地**，并把 big 缺口清单按 0.488 实测重写（§3.2）。
>
> 复测 `docs/big-landing.md`「有意差异与尚未完成」里的拒绝项（CPython 3.14.4 对拍）：
> **旧的「明确拒绝」清单五项里四项早已可用** —— `dict[big, int]` / `set[big]` 键、
> `list[big].sort()`、`sum`/`min`/`max(list[big])`、big 的 `%`/`//`/比较全部通过；
> 真正缺的只有 `divmod(big, …)`、`round(big, ndigits)`、三参 `pow`。
>
> **本轮补 `divmod`**：`typecheck` 不再拦 big；降级不能发 `Bin{FloorDiv,Mod}`（那条路径的
> codegen 对 big 会**静默出 `idivq`**，实测段错误），改为 `CallRt(pa_big_floordiv/pa_big_mod)`，
> 结果按「产生者引用」交给 `tuple.new` 偷走（与字面量元组同口径）。
> 新用例 `tests/run/570_big_divmod.py` 与 CPython **逐字节相同**：正/负被除数与除数、
> `divmod(7, big)`、`divmod(big, big)`、`q*5+r == a`、除零抛 `ZeroDivisionError`。
>
> **仍拒绝（负结论，写进 big-landing.md）**：`round(big, ndigits)` 需要运行时
> `pa_big_round`（按 10^k 取整 + 半程到偶数；单参 `round(big)` 本来就返回自身）；
> 三参 `pow(big, big, mod)` 需要 `pa_big_pow_mod`（右到左二进制幂 + limb 级取模，现有
> `pa_big_pow` 只做非模幂且指数 >2^31 直接 ValueError）。两者都是新增运行时函数 + RtFn +
> 降级 + 对拍用例的完整一轮，本轮不做；`big + float` 提升（结果类型变化）与格式化
> mini-language 维持原状。
## [0.487] - 2026-11-24

> **后续（0.489）**：本版把 dunder 形参放宽成 `Fraction | int | big` 的做法**已回退并判负** ——
> `isinstance` 无法区分裸 double 与 int/big（未装箱的 double 与 int 同为裸字），联合分派在
> 运行期必落到构造函数、把位模式当 BigInt 指针读（静默算错、每次结果不同）。见 0.489 的 Bug B。

> feat(stdlib)：**Fraction / Decimal 与 int / big 的混合运算**（§3.1）。
>
> 之前 `Fraction(1,3) * 3`、`f + 1`、`Decimal("1.1") * 3` 都是编译期
> `unsupported operand type(s)`：`__mul__(self, o: Fraction)` 的静态签名不接受 int。
>
> **设计取舍**（先定口径再动手）：选**改 stdlib 签名**，不做类型检查里的「数值类提升」。
> 理由：① 提升规则必须知道「哪些类是数值类」——任何定义了 `__mul__(self, self)` 的类
> （比如矩阵）都满足，静态一刀切会静默放宽所有类；② 提升要在每个运算符点合成转换调用
> （BinOp/比较/反射/容器元素），影响面从「两个模块」扩到整个类型系统，而 Fraction/Decimal
> 自己最清楚自己的数值域（CPython 也是用 `_operator_fallbacks` 在类内做这件事）；
> ③ 代价可控：dunder 形参变成 `Fraction | int | big`（Decimal 同理），**float 仍然被拒绝**
> （CPython 的 `Fraction + 0.5` 返回 float，是结果类型变化，本模块不覆盖），
> 非数值类型（str/bytes/list）照样编译期报错 —— 只放弃「用 Fraction 当参数」这一条静态
> 精确性，换来 int/big 的双向混合（含反射 `3 * f`）。
>
> 实现：`stdlib/fractions.py` 加 `_coerce(o: Fraction | int | big) -> Fraction`
> （`isinstance` 分派，int/big → `Fraction(o)`），四则/六个比较的形参放宽为联合类型，
> 并补 `__radd__/__rsub__/__rmul__/__rtruediv__`（`3 * f`、`1 - f` 之前直接报错）；
> `stdlib/decimal.py` 同构（`_to_decimal`，int/big → `Decimal(o)` 走精确 str 解析）。
>
> 验收（新用例 `tests/run/569_fraction_decimal_mix.py`，输出与 CPython 3.14.4 **逐字节相同**）：
> - 四则 + 反射两个方向 + 六个比较：`f*3 / 3*f / f+1 / 1+f / f-1 / 1-f / f/3 / 3/f`；
> - 超 i64：big 分子字面量 `Fraction(1180591620717411303424, 3) * 3`，以及
>   「操作数都在 i64 内、交叉乘积越界」`Fraction(3037000500,1) * 3037000500 = 9223372037000250000`；
> - Decimal：`1/d` 按 28 位上下文正确舍入（1.818181818181818181818181818）、`0.1+0.2 = 0.3`；
> - 容器场景不变：`list[Fraction]` / `dict[str, Fraction]` 照旧；
> - 旧用例不退：169 / 402_fraction_big / 403_decimal / 534 / 535 全过。
>
> 已记录的边界：float 混合仍编译期拒绝；`"%s" % (Fraction(...),)` 这类 %-格式化收对象
> 会报 `unsupported format argument type`（与本条无关的既有缺口，用 `str()`/f-string）。
## [0.486] - 2026-11-24

> fix(frontend)：**`--imports` 回退为 opt-in**（0.484 的默认值把 `check` 在真实代码上的
> 通过率打掉了，lead 复核发现）。`strict_module_names` 保持默认开。
>
> **证据**（lead 在 20 个 CPython 3.14 stdlib 文件上量的四种口径，本机复现一致）：
>
> | 口径 | 通过 |
> |---|---|
> | `--no-imports --lenient-names`（0.171 旧口径） | 12/20 |
> | `--lenient-names`（只开 import 图） | **5/20** |
> | `--no-imports`（只开 strict names） | 12/20（本仓库 628 文件全量：480/623，见下） |
> | 默认（两个都开，0.484 的行为） | **5/20** |
>
> 掉分全部来自「默认加载 import 图」。根因不是用户代码错，而是**我们自己的 stdlib 签名表
> 不全**：`module 'subprocess' has no attribute 'check_output'`、`module 'sys' has no
> attribute 'version_info'/'base_prefix'/'implementation'/'platform'`、
> `module 're' has no attribute 'Pattern'`、`cannot assign to module attribute 'sys.stdout'`
> 全是误杀。默认打开等于让一切用到未建模成员的真实代码被判错，直接违背「前端宽、后端严；
> 宁可漏报不可误杀」。
>
> **改了什么**：
> 1. `check`/`types` 默认**不**加载 import 图（等价 0.483 行为），`--imports` 显式开启；
>    `--no-imports` 保留（显式关，优先级高于 `--imports`）。
> 2. `strict_module_names` 仍默认开（`print(VALU)` 这类真拼写错误必须报；628 文件全量上
>    它多报 25 个文件，**全部是 `build` 同样拒绝的**类体同层引用，如
>    `class A: def f(self): ...; __sub__ = f` —— check 与 build 一致，不算误杀）。
> 3. `tests/fail/f80_module_attr.py` 删除（fail harness 只支持环境变量，不支持 CLI 参数），
>    改为 `tests/check_imports.sh` 并接进 ctest（`pyadamas_check_imports`，3 条断言）：
>    默认 check 放行 `json.nonexistent_attr`（不误杀）、`--imports` 下报出
>    `module 'json' has no attribute 'nonexistent_attr'`、`--imports` 下 `os.path.join`
>    仍能解析（0.484 的子模块链能力不回退）。
>
> **0.484 那两条结论的更正**（原文保留，就地标注）：
> - 「check 默认加载 import 图」→ 现为 opt-in；
> - 「550 文件语料对照旧口径零回归」→ 只覆盖了**本仓库自己的** `.py`（tests/bench/stdlib，
>   接口成员表齐全），没覆盖「stdlib 签名表不全」这条路径。
>
> **628 文件全量（本机 CPython 3.14.4）**：语法通过 623/628（99.2%）；`check` 默认
> **480/623 = 77.0%**（0.171 旧口径 505/623 = 81.1%，差的 25 个见上条）；`check --imports`
> 155/623 = 24.9%（更严口径，只给需要模块属性检查的工具用）。
>
> 附带说明：**pya-lsp 要用模块属性检查就自己传 `--imports`**（那是另一个仓库，本轮不改）。
>

## [0.485] - 2026-11-24

> 取证（§3.4）：**前端鲁棒性基线在 0.484 上重测**（CPython **3.14.4**，机器上的版本；
> 0.171 那次是 3.13.5，两段不可直接比较）。
>
> 口径照 [docs/compatibility.md](docs/compatibility.md)「前端鲁棒性基线」（命令已写进该页）：
> `check --syntax` 看语法；类型通过率 = 语法通过者里 `check` 也通过的比例。
>
> | 范围 | 文件数 | 语法通过 | 类型通过（旧口径） | 类型通过（0.484 默认口径） |
> |---|---|---|---|---|
> | 标准库本体 | 628 | **623（99.2%）** | **505/623 = 81.1%** | 153/623 = 24.6% |
> | 去掉 `test/` | 560 | 556（99.3%） | 452/556 = 81.3% | 128/556 = 23.0% |
>
> 「旧口径」= `check --no-imports --lenient-names`（与 0.171 可比）；默认口径是 0.484 起
> 加载 import 图 + 报模块级未定义名的严格版 —— 24.6% 不是变差，是把 stdlib 里大量动态写法
> 如实报了出来。同口径对比 0.171 的 84.1%：**趋势持平**，差额来自 3.14 新增文件与新语法
> （`annotationlib`/`_colorize`/`compression.zstd` 等）。
>
> 失败簇 top-10（123 个失败文件，含 5 个语法失败）与代表文件：生成器/标注不兼容 14
>（`fnmatch.py`）、`X | None` 缺窄化 11（`concurrent/futures/process.py`）、`takes no arguments` 9
>（`_colorize.py`、`uuid.SafeUUID`）、条件导入的 `undefined name` 9（`http/client.py` 的 `CONTINUE`）、
> 动态属性 7、装饰后的协议 dunder 4（`numbers.__complex__`）、`tuple|None` 不可切片 4、
> `property` 与字段同名 4（`urllib/error.py`）、unknown 算术 4、协作式多继承 `super()` 3
>（`socketserver.ForkingMixIn`）。完整表在 compatibility.md。
>
> **top-3 判负**（要动的层与代价写进文档）：① 类型窄化（`is None` 分支 refinement，
> 需在 typecheck 新增一层环境并在汇合点 join，≈15 文件）；② 生成器表达式类型（从 yield 推
> `generator[E]` 并让标注兼容，14 文件，影响所有推导式）；③ 协作式多继承的 `super()`
> （需整模块子类图 + 运行期 MRO，只值 3 文件）。三者都是「新增一层分析」，不是补丁。
> 另两簇（装饰后 dunder、注解与 `@property` 同名）是**故意的表示限制**，已在 language.md 记录。
## [0.484] - 2026-11-24

> feat(frontend)：**pya-lsp 反馈的四条编译器缺口**（3.3）。三条落地、一条判负。

> 1. **`check` 不校验模块属性**（缺口 2，**落地**）：`check --imports` 会加载依赖模块接口
>    （`load_check_lookup`），`json.nonexistent_attr` 在 check 阶段就报
>    `module 'json' has no attribute ...`。**（0.486 更正）本条原先写的是「check 默认加载
>    import 图」，那是错的：默认打开把真实代码通过率从 12/20 打到 5/20（stdlib 签名表不全 →
>    `subprocess.check_output`/`sys.version_info` 被误杀），0.486 已改回 opt-in。**
>    连带三处修正：
>    - **子模块属性链端到端可用**：`os.path.join(...)` 之前 check/build 都报
>      `module 'os' has no attribute 'path'`（只能 `from os.path import ...`）。现在
>      typecheck 与降级都把链展开成完整模块名（`os.path`）按声明式签名解析，
>      与 from-import 同口径。新增 `tests/run/567_os_path_chain.py`。
>    - **接口必须带 `needs_dict`**：`check_build_info` 漏拷这个标记，导致
>      `argparse.Namespace` 这类「成员运行期才存在」的类在 check 下误报
>      （3 个 tests/run 用例回归）。
>    - **模块名当值不再报错**：`hasattr(os, "fork")` 是标准库最常见的动态探测，
>      check 比 build 严会误杀（`tests/11_reflect.py`）→ 降级成 unknown 并记未建模。
> 2. **缺 `asyncio.TimeoutError`**（缺口 3，**落地**）：asyncio 模块表补
>    `TimeoutError` / `CancelledError` 两个**内置异常别名**；`except` 的类型名解析
>    认「模块表里的 Exception 类型」（之前只认模块里的本地异常类）。`wait_for`
>    超时抛的内置 `TimeoutError` 与 `asyncio.TimeoutError` 现在落到同一类型上，
>    与 CPython 3.11+ 一致。新增 `tests/run/568_asyncio_timeout_error.py`。
> 3. **`check` 不报顶层未定义名字**（缺口 4，**落地**）：check 默认 `strict_module_names`，
>    `print(VALU)` 在 check 阶段报 `undefined name 'VALU'`；`--lenient-names` 是逃生口。
>    全语料 550 个 `.py` 对照旧口径**零回归**（新增 4 个「旧失败新通过」，均为接口变准）。
>    **（0.486 更正）**这个「零回归」只覆盖了**本仓库自己的** `.py`（tests/bench/stdlib，
>    这些模块的接口是我们自己写的、成员表齐全），没有覆盖「stdlib 签名表不全」这条路径——
>    在 CPython 标准库这类真实代码上，默认加载 import 图会把 12/20 打到 5/20。现在
>    `--imports` 是 opt-in，并由 `tests/check_imports.sh`（ctest：`pyadamas_check_imports`）钉住三条口径。
>    新增 `tests/fail/f81_undefined_top.py`。
> 4. **二进制 `file.read(n)` 返回 str**（缺口 1，**判负**）：`open(..., "rb")` 的值类型
>    是 str（byte-transparent，长度按字节）。改成 bytes 是正确方向，但要三件事同时做：
>    运行时 `pa_file_read/readline/readlines/write` 全族改 bytes、`Type` 给 File 加
>    `binary` 标记（否则「类型 bytes、值是 str」会让 str 操作按 bytes 布局读内存）、
>    以及 **pya-lsp 自己那行 `body: str = inp.read(length)`** 改成 bytes+decode（不改
>    它 LSP 就编不过）。本轮按「不改 pya-lsp」的硬约束判负，口径写进
>    [docs/compatibility.md](docs/compatibility.md)「二进制模式 file 的 read/write 语义」，
>    并给了最小复现与三步改法。
>
> 验收：普通套件（见下）/ difftest / check_size；`tests/fail/f80_module_attr.py`、
> `f81_undefined_top.py`、`tests/run/567/568` 为新增回归。
## [0.483] - 2026-11-24

> chore(P3)：工程债结账——拆掉最后两个超限文件、CHANGELOG 轮转、文档地图扩容。
>
> **规模检查首次全绿**：`bash tests/check_size.sh` 从「两个文件超限」变成**通过**（`ALLOW_FILES` 为空，全仓库 0 超限）。
>
> | 文件 | 拆前 | 拆后 | 拆出 |
> |---|---|---|---|
> | `src/typecheck/typecheck.cpp` | 1227 | **1017** | `src/typecheck/annotation.cpp`（注解解析：`parse_annotation` / `_impl` / `_text`，210 行） |
> | `src/cimport/cdecl_parse.cpp` | 1271 | **1116** | `src/cimport/cdecl_eval.cpp`（C 常量表达式求值，155 行；声明进 `cdecl_internal.h`） |
>
> 证明「拆 TU 不改代码生成」：12 个基准 `instructions:u` 与 0.482 基线**逐项 ±0.00%**（perf 闸门 warn=0 fail=0），套件 477 / difftest 447/0 / ctest 6/6。
>
> **CHANGELOG 轮转**：0.100–0.199 共 97 个版本、2183 行移到 `docs/archive/changelog-0.100-0.199.md`；
> 根 CHANGELOG 现 7753 行 / 282 个版本（0.482 → 0.200），末尾留指向归档的指针。`archive/README.md` 索引与
> `docs/index.md` 的引用同步。
>
> **文档地图扩容**：主表之外新增「其余设计与落地记录（按主题）」——把此前未挂的 30+ 篇（数值与表示、
> 后端与优化、并发与运行时、构建速度、互操作与生态、语料与流程）按主题分组挂上，避免主表无限变长。
>
> **顺带修掉一个被 0.480 打破的闸门**：`checkall --full` 的第 6 项原来跑 `PYADAMAS_SIMD=1`，而该开关
> 已随特化模板删除（设了 rc=2），`--full` 会红在一个「设计上已不存在」的变体上。换成**向量化自检**：
> 同一程序默认 vs `PYADAMAS_VECT=0`，默认指令数必须显著更少（matmul / spectral_norm 两例）。
> `docs/testing.md` 的闸门表同步。
>
> P3-4（提交前显式跑 `CHECKALL_STRICT_GIT=1`）作为流程约定保留；本次提交前按此跑过 `--full`：
> 除 git-status（提交前的预期脏工作区）外 8 项全绿。
## [0.482] - 2026-11-24

> 收尾（§8.2-4）：**基线刷新**（当前 HEAD 全量重测，全部可用页内命令复现）。
>
> 1. **12 基准 `instructions:u`**：`tools/perf_baseline.txt` 重写（`checkall.sh --perf` 确认
>    `warn=0 fail=0` 后人工跑 `--write-baseline`，不是为过闸门改数）。相对旧基线的变化：
>    **wordfreq −7.38%**（旧基线写于 HEAD `668449f`，0.47x 的优化之后没再刷新——这就是上次
>    那个 −7.38% 的来源）、nqueens +0.28%、fannkuch +0.59%、binary_trees +1.16%（后三者都在
>    ±2% 警告带内，且 0.480 的产物与 0.479 逐字节相同，与本版无关）。其余 8 例变化 ≤0.03%。
> 2. **四语言对比**（`PYADAMAS_BIN=build/lead/pyadamas run_compare.py --repeat 5`）：
>    `bench/compare/RESULTS-0.480.md`。12 例全部快于 CPython（4.63× ~ 57.11×）；
>    **spectral_norm 26.0 ms**（Go 16.1 / C++ 14.0 → 1.61× Go、1.86× C++，对 CPython 57.11×）、
>    matmul 4.3 ms（比 C++ 还快 0.88×）、mandelbrot 15.5 ms（比 Go/C++ 快）；
>    obj_dispatch 291.9 ms 是唯一显著落后项（Codon 113.6 ms = 2.57×）。
> 3. **Codon 九例**：**7 例可比**（脚本收编为 `tools/codon_cmp.py`，/tmp 清掉后仍可复现；JSON 落到
>    `.ref_proj/benchmarks/`，属本地实验产物、不入库），绑核交替 5 轮取最小：几何平均 **PA/Codon = 0.960**；
>    `binary_trees` / `fannkuch` 用 Codon 0.20.7 编不过（本仓源码的泛型标注不兼容），
>    文档里已注明不可比。逐例：mandelbrot 0.570 / matmul 0.509 / nqueens 0.861 /
>    spectral_norm 0.840 / nbody 1.030 / fib 1.352 / obj_dispatch **2.570**。
> 4. **文档表格同步**：`README` 验证表（`tests/run` 441→448、difftest 440→447，均按 `ls`/闸门实测）、
>    `docs/compatibility.md` §四「当前性能」加 0.480 复核段（保留 0.7 时代 `-O0` 历史口径）、
>    `docs/nightly-summary.md` §五 两行（向量化 8→7 结账、P4 删特化改为已删）、
>    `bench/compare/run_compare.py` 的输出前言（不再声称「特化留着作对照」）、
>    `docs/next-round-plan.md` §8.2 四条交付全部标结账。
>
> 口径：`-O2` 生成码、每例 `--rebuild`（0.478 起整程序产物缓存会命中，A/B 必须 `--rebuild` 或
> `PYADAMAS_NO_PROGCACHE=1`）、多租户机器按轮取最小/中位。
>
> 顺带：`tools/flagbench.sh` 的 p25 列在 `REPS=1` 时除零（只在试跑时出现），已修。
## [0.481] - 2026-11-24

> 收尾（§8.2-3）：**实验开关结账**。8 个开关逐个定论，登记在新增的
> [docs/experiment-switches.md](docs/experiment-switches.md)（唯一清单：默认值 / 结论 / 证据 /
> 触发条件 / 回退方式 / 删除清单）。本版无编译器代码改动。
>
> | 开关 | 结论 | 依据 |
> |---|---|---|
> | `RA_OPT` | 保持 opt-in | 0.475 两窗 15 轮：realistic −0.2%、classic +1.8%（反号）；asm 真变化 3 例，nqueens +8.5% 是回退。开关自包含（`ra_opt_` 只在一处门控），无其它开关引用 |
> | `RA_SPLIT` | 保持「已废弃 + 拒绝入口」（`rc_split` 恒 false） | 与默认路径的块内槽发射共用 `gen_block`，删除要独立一版 + asm 对照；**删除清单已写进文档**（~200 行 + 5 处发射 + spill_placement） |
> | `RA_GRAPH` | 删代码（已完成） | 图着色整段删除，只剩防误用的报错 |
> | `LOOP_OPT` | 默认开（3 = 跳转线程化 + 仿射 BCE） | bit0 纯 CFG 等价（nqueens −2.9%）；bit2 同块 load 去重实测负收益，只能显式掩码 |
> | `BORROW_OPT` | 保持 opt-in | 命中即纯赢（obj_dispatch −32.7%、dict_wordfreq −6.5%、sort_struct −2.6%，其余 15 例 asm 逐字节不变），但 `bench/daily` 23 例（5 例命中）BURST=20×15 轮聚合 **−0.1%**（噪声带内）。缺的证据与触发条件写在文档里 |
> | `VECT` | 默认开（现状） | 5/12 命中，最高 −86.5%；剩余 7 个已判负（0.479） |
> | `TYPE_TAGS` | 保持 opt-in | 语义开关（unknown tag 化），不是性能开关；`LANG_GAPS` 会强制打开 |
> | `GC_INCR_MARK` | 保持 opt-in | GC 并发化阶段1 原型，默认关时行为/asm/leak 逐字不变；阶段2 立项时整体评估 |
> | ~~`SIMD`~~ | 删代码（0.480 已完成） | 模板对当前 IR 全部失配 |
>
> 配套：新增 `tools/flagbench.sh`（把 0.474/0.475 用过的临时 A/B 脚本收编：只切一个环境变量、
> 两侧独立 modcache + `--rebuild`、打乱顺序 + 奇偶轮交换、BURST 连跑、15 轮中位/p25、先比对输出一致），
> 文档里的每条结论都给了可复现命令。`docs/index.md` 文档地图补两行（本表与向量化覆盖率）。
## [0.480] - 2026-11-24

> 收尾（§8.2-2）：**删特化 —— 特化 SIMD 模板（`PYADAMAS_SIMD`）整段删除**。
> 1304 行 → 149 行（只留 VectPlanner 复用的纯 IR 判据）。默认路径 asm 逐字节不变。
>
> **取证（删之前先证明它是死的）**：
> - 12 个基准 `asm -O2` 里 **0 条** `SIMD` 标记（模板一次都没匹配上）；
> - `PYADAMAS_SIMD=1` 与「关掉全部向量化」的用户态指令数**逐位相同**：
>   spectral_norm 234,860,961 vs 234,860,233、nbody 199,420,356 vs 199,420,323、
>   matmul 76,380,915 vs 76,381,175、sieve 33,044,313 vs 33,044,332；
> - 也就是说 0.417 记录的「spectral_norm 仍约 1.13× 依赖特化路径」**已经不成立**：
>   0.384 之后通用向量化（含 0.4xx 的 masked）把它全面盖过（今天 VECT=0→1：
>   spectral_norm −59.6%、matmul −86.5%、sieve −51.9%、nbody −45.2%、mandelbrot −36.3%）。
>
> **删除内容**：
> - `src/backend/simd_int.cpp` / `simd_nbody.cpp` / `simd_nbody_pair.cpp` 整文件删除；
> - `src/backend/simd.cpp` 里的 `try_simd_reduction/vectorize/int_map/nbody_pos/nbody_pair`
>   与只服务它们的 `simd_func_ok/simd_core_body/simd_slot_writers` 删除；
> - 剩下的纯 IR 形状判据（for/while 块形状、loop op 白名单、槽写者/取址/逃逸/稳定、
>   scratch 安全、`simd_def`）**保留并改名为 `backend/vect_gates.{cpp,h}`** ——
>   VectPlanner（通用向量化）一直在用它们，不是死代码；
> - `block.cpp` 的三路分流（`block_int_only` 顺序分流）收敛成一行 `try_vect_loop(b)`；
>   `block_int_only` 随之删除（只服务旧分流）；
> - `PYADAMAS_SIMD` 从 manifest 键与 `generate_asm` 签名里去掉；env 仍被识别，
>   但**显式报错**（`error: PYADAMAS_SIMD 已删除`），免得 A/B 脚本以为在跑另一条路径。
>
> **验收**：默认路径（`PYADAMAS_VECT=1`、无 SIMD）12 基准 `asm` 与删除前**逐字节相同**
> （HEAD worktree 编译器对照）；`PYADAMAS_SIMD=1` 现在返回码 2；套件/difftest/check_size 见下。
## [0.479] - 2026-11-24

> 收尾（§8.2-1）：**向量化缺口定论 —— 剩余 7 个 0 触发基准不再做，负结论归档**。
> 本版无代码改动；`docs/vectorization-coverage.md` 增加「当前状态」块与 §10 定论。
>
> **先修正事实**：路线图写的「8 个基准 0 触发」是 0.423 的口径。当前实测（VECT=0 → VECT=1
> 的 `instructions:u`，`build/lead/pyadamas build -O2 --rebuild`）：
>
> | 基准 | 指令数 off → on | 变化 | 路径 |
> |---|---|---|---|
> | matmul | 76,381,175 → 10,291,764 | -86.5% | 通用（AVX2 4-lane） |
> | spectral_norm | 234,860,891 → 94,916,852 | -59.6% | 通用 2-lane |
> | sieve | 33,044,332 → 15,910,760 | -51.9% | 通用 2-lane |
> | nbody | 199,420,323 → 109,319,750 | -45.2% | 通用 2-lane |
> | mandelbrot | 69,589,634 → 44,359,636 | **-36.3%** | **masked**（asm 里 10 条 masked 标记） |
> | collatz/fannkuch/fasta/nqueens/wordfreq/binary_trees/fib | 0.0% | 0 标记 | — |
>
> 即 **5/12 已向量化、7/12 仍 0**：mandelbrot 已被 `backend/masked.cpp`（masked divergence：
> 外层独立迭代 + 内层数据依赖早退）覆盖，题目三条候选里的「masked」**早已兑现**。
>
> 定论（逐条对账，详见 vectorization-coverage.md §10）：
> 1. **masked**：已落地并拿到 mandelbrot -36.3%；剩下唯一的 masked 候选 collatz 需要的是
>    「先把 `steps` 内联进外层循环 + 新的 masked **int** 发射器」，不是放宽判据。发散实测
>    （N=100k）：`steps` 均值 107.5 / 最大 350、相邻配对 max/mean = 0.61 → 掩码后迭代数 0.30×，
>    天花板约 2×，但代价是 mask 框架的第二个特化，与「删特化」方向相反。
> 2. **嵌套循环**：唯一名副其实的嵌套（mandelbrot 外层 `for x` + 内层 `while`）已由 masked
>    整块向量化；matmul/spectral_norm/nbody 的收益本来就在内层 → 无增量。
> 3. **调用屏障**：fasta（bb8 一条块 10 处调用/存储）、wordfreq（bb22 11 处）、nqueens（递归）、
>    sieve 初始化（append）的调用**就是工作量本身** → 零收益。fannkuch trip≤8、binary_trees/fib
>    0 个循环块，早已判死。
>
> 所以「实现收益最大的一个」= 实现 masked int + 内联，估计只覆盖 collatz 一个基准（≤2×，
> 占 12 基准的 1/12），而风险是静默算错（0.314/0.383 的教训）。**判负，不立项**；
> 未来若要重启，前置清单写在 §10.3（先做泛化内联，再谈 masked int，且必须有发散守卫）。
## [0.478] - 2026-11-24

> perf(build)：**整程序产物缓存**——同源码 + 同开关 + 同工具链的重复 `build`/`run` 不再跑 `as`/`ld`。
>
> 键由**真实链接输入**推导：每个 `.o`（含运行时对象）的内容摘要（按链接顺序）+ target ABI +
> `toolchain_id` + gc/asan/cc 额外 flags + cimport 链接参数；不从命令行选项名单猜。命中时把
> 缓存产物写到输出路径（已存在且字节相同则跳过拷贝），`--rebuild` 与 `PYADAMAS_NO_PROGCACHE=1`
> 绕过；任何 IO 失败静默退化成照常链接（缓存是纯优化）。
>
> 顺带修掉一块隐藏固定成本：`toolchain_id` 要对 `/proc/self/exe`（本机构建 151MB）算内容摘要，
> 原来逐字节 FNV 每次进程吃掉 25–30ms；改成按 64 位字混合（本地失效指纹，不是信任根）。
> 副作用：已有 modcache/progcache 失效一次。
>
> 实测（同窗、5 次最小；`/bin/true` 同机 11–12ms 为进程启动地板）：
>
> | 场景 | 改前 | 改后 |
> |---|---|---|
> | 热构建（hello，`build -O2 -o` 同路径） | 91 ms | **23 ms**（扣掉启动 79 → 11 ms） |
> | `run` 同源重复执行 | 95 ms | **26 ms** |
> | 关掉缓存的构建（digest 修复的独立收益） | 90 ms | 72 ms |
>
> 回归：`tests/progcache.sh`（16 项，已接进 ctest）覆盖 store/hit/换输出路径/源码变更/-O 档/
> `--asan`/工具链指纹/`--rebuild`/关缓存开关/`run` 命中/清缓存重建。另做了一次**「故意漏项」
> 反向实验**：把 toolchain 从键里摘掉后，换 `PYADAMAS_TOOLCHAIN_ID` 仍命中（错误行为可复现），
> 恢复后 A/B 都 miss——证明这条防线有效，而不是靠审阅。
>
> 另把 CHANGELOG 登记进 `CMAKE_CONFIGURE_DEPENDS`，修掉「改了版本号但 ninja 不重配、
> `deps --json` 仍报旧版本」。
>
> 闸门：套件 477 / difftest 447 / ctest（pkg + progcache）/ check_size 无新增。
## [0.477] - 2026-11-24

> fix(lower)：`from pkg import sub` 之后调用子模块函数会 **SIGSEGV（call 0）**。
>
> 根因：`lower_import_from` 在 `pkg` 的 globals 里找不到 `sub` 时会把它当子模块别名登记，
> 但只对父模块 `pkg` 发了 `import.module`，**没对 `pkg.sub` 发**。父包 `__init__.py` 不 import
> 子模块时，`pkg.sub` 的 init 从未运行，它的函数全局还是 0，`sub.f()` 就成了 `call 0`。
> 修法：登记子模块别名前先 `ensure_module_init(child)`（与 `import pkg.sub as x` 同口径）。
>
> 该 bug 在 0.473 之前的基线就存在（用 0.476 的二进制实测 rc=139）。`from pkg import sub`
> 本身是 README 承诺支持的写法，只是「只 import 不调用」不会触发，所以一直没被测到。
>
> 回归：新增 `tests/modules/from_pkg_sub/`（空 `lib/__init__.py` + `lib/sub.py`，覆盖
> `from lib import sub`、`from lib import sub as s`、`sub.VALUE`）。验证：CPython 与 PyAdamas
> 输出逐字节一致、ASan 干净；套件 **477 ran ok / 0 失败**、difftest **447 / 0**、check_size 无新增。
## [0.476] - 2026-11-24

> perf(stdlib)：标准库（纯 PyAdamas 实现）一轮「扣性能 + 抠细节」，5 个模块 + 6 个对拍用例。
> 只动 `stdlib/*.py` 与新增 `tests/run/`，不动 `src/`/`runtime/`，不改生成代码。
>
> | 模块 | 改动 | 同窗实测 |
> |---|---|---|
> | `textwrap` | `_expand_tabs`/`_replace_ws` 的逐字符 `out = out + c` 改 list+join；内联 ASCII 空白判定 | bench_string_textwrap **969 → 540 ms**（0.85× → 1.5× vs CPython）；perf 里 memmove+str_alloc+concat ≈20%、`_is_space` 12% |
> | `heapq` | 核心堆操作形参 `list` → `list[int]`，元素比较从 `pa_any_cmp_t` 通用标签路径变内联整数比较 | bench_heapq **63 → 18 ms**（1.0× → 3.4×） |
> | `statistics` | `_rank` 从 O(n²) 逐对比改 O(n log n) 二分计数 | ranked correlation n=4000 ×30：**3.12 s → 0.05 s**（60×） |
> | `string` | `Template._substitute` 缓冲化 + `__pa_any_items(mapping)` 提到函数入口 | 2000 占位符模板 **0.03 → 0.01 s** |
> | `base64` | `b16decode` 错误口径对齐 CPython（奇数长度也报 `Non-base16 digit found`，实测 CPython 3.13 行为） | — |
>
> **语义收紧**：`heapq` 的堆元素现在只接受 `list[int]`。此前 `list[float]` 能编译，但
> `heappop` 返回的是 8B 位模式整数（type-tags 缺口下的静默算错）；现在编译期拒绝并给明确
> 消息。`docs/compatibility.md` 同步。
>
> 判负：`bisect`（42% self 是已最优的整数折半，23% 是 builtin `list.insert` 的 memmove）、
> `difflib._sort_matches`（插入排序 O(n²)，但 PA 的 `list.sort` 不支持 `key=`，改并行 key 列表
> 收益不抵复杂度）；`base64` 的 `b64decode`/`b32decode` 对非法/缺填充输入过于宽松，试修后
> CPython 对拍失败已回退，记为负结论。
>
> 闸门：套件 **475 ran ok / 0 失败**、difftest **446 / 0**、check_size 无新增（仍只有既有两个
> src 文件）、每个改过的 stdlib 文件 `check` 通过；用例 470–475 的 expect 均来自 CPython 实跑。
>
> 两条编译器侧线索（只记录，见 [next-round-plan.md](docs/next-round-plan.md) §8.3）：
> ①裸 `list` 形参在推断出 `list[int]` 后元素比较仍走 `pa_any_cmp_t`（触发条件是「跨模块 import
> 的裸 list 形参 + fresh_hole」，本地复刻 `_siftup` 不触发）；②循环内 `s = s + c` 每次
> `pa_str_concat` 都分配新对象，n=1k/10k/50k/100k → 0.00/0.00/0.03/0.19 s 的二次曲线。
## [0.475] - 2026-11-24

> 决策：`PYADAMAS_RA_OPT` **保持 opt-in（默认关）**，不默认打开。
>
> 口径：abbench 式交替 A/B —— 同一编译器、两侧独立 `PYADAMAS_CACHE_DIR` + `--rebuild`、
> 每轮打乱用例顺序、奇偶轮交换 A/B 先后、BURST=3 连跑取平均、15 轮中位数/p25、绑核。
> 两窗：realistic（6 例）+ classic（13 例）。
>
> | 窗口 | off | on | 合计 |
> |---|---|---|---|
> | realistic | 1321.78 ms | 1319.08 ms | -0.2% |
> | classic | 315.99 ms | 321.83 ms | +1.8% |
>
> 逐例（只列 asm 真正变化的；其余 asm 逐字节相同 → 差值全是噪声）：
>
> | 用例 | 中位 | p25 | asm |
> |---|---|---|---|
> | nqueens | +8.5% | +7.0% | 变（34 行） |
> | fasta | +4.8% | +20.9% | 变（16 行） |
> | dict_wordfreq | -0.7% | -0.3% | 变（33 行） |
> | obj_dispatch | +0.2% | +0.1% | 逐字节相同 |
> | binary_trees / fannkuch / spectral_norm / sieve 等 10 例 | +5.7 / -5.4 / -4.3 / -2.7% | 反号 | 逐字节相同 |
>
> 判据「两窗同向且 ≥2%」**不成立**：一窗 -0.2%、一窗 +1.8%（反号），且唯一稳定的大幅
> 差异是 nqueens 的真实产物变化，方向是回退（+8.5%，p25 同向）。**结论：保持默认关**，
> 理由与数据写进 [docs/ra-opt-design.md](docs/ra-opt-design.md) §6。
>
> 附带结论：0.470 的入口私有全局提升之后，obj_dispatch 上 RA_OPT 的收益已**归零**
> （+0.2%，与 HANDOFF §5.3 的预判一致）；历史 mandelbrot/fasta 的 +3~4% 也不复现
> （fasta 这次是 +4.8% 回退、p25 +20.9% 属噪声）。本版不改代码。
## [0.474] - 2026-11-24

> perf(lower)：**RC 借用扩展到「入口私有全局根 + append-only 根」**。
> obj_dispatch 内层循环 300.59 → 202.24 ms（**-32.7%**，15 轮中位/p25 同向），
> 指令数 3.110e9 → 1.766e9（-43.2%）；`PYADAMAS_BORROW_OPT=1` 门控（保持 opt-in）。
>
> 形态（bench/realistic/obj_dispatch.py）：模块顶层 `for s in shapes: total += s.area()`，
> 循环变量与容器都是入口私有堆全局。0.472 之前每轮 `rc_inc(元素)` + `load.global s`
> （旧值）+ `store.global s` + `rc_dec(旧值)` 全是可省的记账（640000×100 轮）。
>
> 六道门与其解锁方式（细节见 [docs/borrow-loop-design.md](docs/borrow-loop-design.md) §9）：
>
> 1. 候选只能是帧槽 → 新增**全局候选**（候选形状对 LoadGlobal/StoreGlobal 同口径匹配）；
> 2. GLOBAL 根要求「本函数内从不写」 → 新增根类别 **PrivateGlobal**：入口私有 +
>    唯一实写来自新分配 + 只增不删（append/extend）；
> 3. `class_free` 整函数否决 Class 值 → 只在「容器不可达」的根（FreshLocal/PrivateGlobal）
>    上放行：容器逃不出去，被调用户代码无法在借用窗口内让元素计数归零，`__del__` 时机不变；
> 4. `has_hard_bad` 否决含 `ObjNew`/`ClassAddr` 的函数、`ClassNew` 落 Bad → 前两者移出
>    硬拒清单（新对象不可能与既有容器别名 / 不写内存），`ClassNew`（rt_class.c 纯分配）归 Pure；
>    `CallExtern/PtrStore/Gen*` 仍在清单里；
> 5. 根值被迭代源持有（`rc_inc`+`rc_temp`）与 append 接收者消费 → 根的消费白名单放行这两类；
> 6. 全局没有清零 store → 出口 `[load.global g; rc_dec]` **原地**换成
>    `[const.int 0; store.global g, 0]`，`GcShutdown` 的根表里不留借来的指针。
>
> 红线保持：含 handler 的函数不借；生成器/协程整类跳过；跨调用仍必须持有引用；
> 容器被删/被替换/被别的函数写过/循环体内 pop 一律拒绝（新增用例 566 逐条钉死）。
> O0/O1 产物不变（本遍只受 `PYADAMAS_BORROW_OPT` 门控，flag-off 不跑）。
>
> 实测（同一编译器、两侧独立 modcache + `--rebuild`、打乱顺序 + 奇偶轮交换、BURST=3、
> 15 轮中位/p25、绑核）：
>
> | 用例 | off | on | 中位 | p25 |
> |---|---|---|---|---|
> | obj_dispatch | 300.59 ms | 202.24 ms | **-32.7%** | -32.7% |
> | dict_wordfreq | 232.56 ms | 217.55 ms | -6.5% | -6.8% |
> | sort_struct | 298.70 ms | 290.79 ms | -2.6% | -3.0% |
> | queue_sim / rec_memo / str_build | — | — | -0.2 / +0.1 / -0.2% | 噪声 |
> | realistic 合计 | 1331.99 ms | 1210.39 ms | **-9.1%** | — |
> | classic 13 例合计 | 311.07 ms | 309.87 ms | -0.4% | — |
>
> classic 窗无真实回退：fasta +2.9%（p25 +15.8%）、fannkuch −5.9%、matmul +1.6%、
> binary_trees +1.3% 四例的 flag-on/off **asm 逐字节相同**，是短用例噪声（±3% 口径，
> 见 docs/testing.md）。全 18 个基准里只有 3 个（obj_dispatch/dict_wordfreq/sort_struct）
> 产物发生变化，其余 15 个 asm 逐字节不变。
>
> 闸门：普通套件 **470 ran ok / 0 失败**、ASan 套件 **470 ran ok / 0 失败**（均 flag-on，
> 8 作业）；difftest（O0 vs O2）flag-on **441 / 0**；18 个基准输出与 CPython 逐字节一致；
> check_size 无新增超限（borrow.cpp 745 → 1024 行，仍 < 1200；既有两处 1227/1271 不变）。
> 新增回归 `tests/run/566_entry_global_borrow.py`（expect 来自 CPython 实跑）。
>
> 未做/遗留：门控仍默认关（默认化需要全量日常语料 A/B）；`del s`、先赋值再被 for 覆盖、
> 运行时新容器来源（ListSlice/StrToList）、dict/set 根、生成器/协程仍未覆盖。
## [0.473] - 2026-11-24

> feat(pkg)：包管理 **P0-a** 落地——清单 + 锁 + `resolve` + `--locked` + 锁派生 ModuleMap。
> 设计见 [docs/package-management-design.md](docs/package-management-design.md) §6.1；**实现留在本仓库**，不单开项目。
>
> 交付：
> - `pyadamas.toml` 最小 TOML 子集：`[package]` / `[dependencies]`（**只支持 path 依赖**）/
>   `[modules]`（逻辑名 →（包, 模块））；registry / 版本范围 / git 依赖与 `[aliases]` 明确报「P2 才支持」；
> - `pyadamas.lock`：行式、排序、可 diff；坐标 + 相对路径 + **SHA-256 源码树哈希** + 逻辑名→包内模块 + 依赖边，
>   无时间戳/主机名/绝对路径；SHA-256 自实现，无新增链接依赖；
> - `pyadamas resolve` 写锁；`build/run --locked` 在锁与清单不一致（含锁缺失）时失败并打印**逐行 diff**；
>   非 `--locked` 是宽松模式（note + diff + 按清单重解析，绝不让漂移静默发生）；
> - 锁派生与 `deps --json` **同 schema** 的 ModuleMap，交给现有 `--module-map` 加载器，不新增第二套映射格式。
>
> 验收（`tests/pkg/run.sh` 56 项，已接进 ctest）：无清单项目 0 回归；path 依赖跨目录编译运行；
> 手改清单/改依赖源码后 `--locked` 失败并给 diff；两次 `resolve` 字节相同；整目录搬家锁不变；
> 锁内无绝对路径（注入绝对路径被拒）；循环依赖/缺文件/未知键报错指到行列。
>
> 闸门：套件 469/0、difftest 440/0（合并后复跑仍 0）、check_size 无新增、ctest 2/2；
> 5 个基准的生成汇编归一化后与合并前逐字节相同（P0 只动驱动层，不改代码生成）。
>
> 已知缺口（P0-b / P1 / P2）：版本尚未进 mangling 与接口坐标（需一次 ABI bump，会改符号与汇编，
> 与「P0 不改生成代码」冲突，单独立项）；`check --imports` 未接包映射；`[modules]` 只有
> 消费方绑定一种语义；本地模块与包同名不报冲突（映射优先）；树哈希不覆盖权限/空目录，符号链接明确报错。

## [0.472] - 2026-11-24

> 取证版（**无代码改动**）：路线图 2.2「浮点槽提升作用域放宽」前提不成立，**判负**；
> 2.3「入口私有全局红利」复测确认已兑现，不再立项。

### 2.2 判负：浮点槽提升从 0.279 起就没有「整函数无调用」门禁

路线图的依据是 `ir` 里 `__pa_main` 的内层块 `bb12` 有 `load.slot s5` / `store.slot s5`。
那是**IR 层**现象，汇编层早已没有：

| 取证 | 结果 |
|---|---|
| `asm -O2 bench/compare/spectral_norm.py`（bb12） | `addsd %xmm7, %xmm8`，s5 在 `%xmm8`；循环内对 s5 零访存 |
| `asm -O1` 同函数 | `addsd %xmm1, %xmm6`，同样常驻 |
| `PYADAMAS_VECT=0 asm -O2` | 同上（不是向量化带来的巧合） |
| `objdump -d` 实际二进制 | `50eb: addsd %xmm7,%xmm8`、`5147: movsd %xmm8,%xmm0` |
| `perf stat -e instructions:u` 3 轮 | 94916055 / 94916063 / 94916052（稳定，改动前=改动后） |

机制：`assign_slot_regs` 对浮点槽走 `linear_scan_pool(fp_cands, kFpSlotRegs(), …)`，
**没有调用门禁**；跨调用的代价由 `build_fp_liveness` + 每指令 `fp_spill_mask` 承担 ——
只 spill「调用之后还要读」的槽（0.279 起，spectral_norm 正是靠它）。也就是说路线图设想的
判据「**该值的活跃区间不跨调用**」在 0.279 就已经实现了，不存在可放宽的整函数门禁。

唯一的整函数「无调用」门禁是 `assign_global_vreg_regs` 的 `call_free_fp`，它管的是
**跨块浮点 vreg**（与槽不是同一机制）；该放宽在 nbody 上已实测负收益（7 个浮点变量 +
4 个跨块临时值抢 8 个 xmm），源码注释有记录，不再重试。

「热循环体内无调用」这条替代判据对唯一还有代价的用例（nbody）无效：`dx/dy/dz/d2/mag`
的活跃区间**跨 `math.sqrt` 调用**，按 ABI 必须 spill/reload。要消掉它得把 `math.sqrt`
内联成 `sqrtsd`（还要处理负数参数抛 ValueError 的语义），是另一项，不在 2.2 范围。

**结论**：不改代码。spectral_norm 内层循环剩下的唯一内存往返是 `index.get` 结果经临时槽
（`movsd %xmm0,0x1c0(%rsp)` / `mulsd 0x1c0(%rsp),%xmm7`），属「索引装载直出寄存器」
另一类问题。

### 2.3 复测（不立项，与路线图一致）

`build/lead/pyadamas ir <case>` + `docs/roadmap.md` §0 的 awk 口径，当前 HEAD 的
`__pa_main` 六项计数：

| 用例 | blocks | call.rt | call.direct | load.global | store.global | load.slot | store.slot |
|---|---|---|---|---|---|---|---|
| spectral_norm | 28 | 30 | 0 | 38 | 16 | 43 | 34 |
| fannkuch | 5 | 23 | 1 | **9** | 6 | 2 | 4 |
| fasta | 9 | 31 | 1 | **15** | 10 | 12 | 9 |
| nbody | 7 | 21 | 2 | 50 | 7 | 6 | 6 |
| matmul | 3 | 18 | 1 | 8 | 5 | 1 | 2 |
| obj_dispatch | 27 | 44 | 2 | 22 | 15 | 12 | 11 |

- `bb12`（内层循环）里**没有任何 `load.global`**：`u`/`v`/`N` 已外提成 vreg
  （`%59`/`%60`/`%61`），剩下的 `load.global g4/g5` 只在 10 次的外层循环里。
- fannkuch / fasta 的 `load.global` 从 0.300 口径的 13 / 19 降到 **9 / 15**，与路线图一致。
- 结论：A 的完整形态（分配器候选集扩到标量 global）对这三个基准没有剩余空间，不立项。

## [0.471] - 2026-11-24

> tooling：`tools/difftest.sh` 的两类假判（假绿 / 假红）一次收口。
>
> 1. **假绿**：旧脚本用 `build && run` 串联，两侧**都编不过**时输出同为空、退出码同为 1，
>    被判「一致」——0.288 的 movzbq 写错寄存器就是这么混过去的（difftest 193/0 全绿，
>    套件却从 211 掉到 189）。现在编译与运行分开记退出码，任一侧编不过直接报
>    `BUILDFAIL`（附单侧首行错误）并计入失败。
> 2. **假红**：默认 nproc 并行时 tarfile 组（361–364）偶发 20s 超时/资源竞争造成的假 DIFF。
>    首轮不一致的用例改为**单路复跑一次**（`DT_TIMEOUT` 20s → 90s），复跑一致的记「抖动」
>    并单独列出、不计失败；真差异是确定性的，复跑仍报错。
>
> 验证：48 路默认并行连续 2 次 **440/0**（30.48s / 30.03s）；两个合成用例覆盖
> 「两侧都编不过 → BUILDFAIL + exit 1」与「首轮编译抖动 → 复跑排除 + exit 0」。
>
> 影响：difftest 不再需要「并行度必须 ≤8」的口头纪律（HANDOFF §6.2/§6.5 的教训已由脚本兜住）。

## [0.470] - 2026-11-24

> perf/fix：**obj_dispatch 对 Codon 3.42× → 2.56×**（389.9 → 292.5 ms），三项改动。
>
> 1. `perf(lower)`：`for x in list/tuple` 的元素装载紧跟循环条件分支，`i < len(seq)`
>    已成立；有副作用的循环体只要装载前只有索引槽读 / 空操作 rc_cleanup，也标
>    `index.get.unchecked`（下一轮条件仍重读 len，pop/append 语义与 CPython 迭代器
>    一致）。单项 obj_dispatch 389.9 → 302.4 ms；全量内置基准 53 项几何 0.997×。
> 2. `fix(lower)`：`for x in a: a = ...` 的序列只是借用，体内重绑会 dec 掉最后一份
>    引用并释放旧列表，后续迭代读已释放对象（a=[1,2,3] 实际只迭代出 2 个；ASan 因
>    分配器池化不报）。循环前补一份引用进临时栈、语句边界释放；新分配值不重复持有。
>    新增 564。
> 3. `perf(opt)`：入口模块私有标量全局跨调用提升——除 `__pa_main` 外无人引用、全模块
>    未取址的标量全局，循环体允许用户调用（入口符号前缀 `__entry__` 不可被 import；
>    `globals/eval/exec` 直接报错；入口无 handler，未捕获异常即终止）。配套保住两处
>    融合：`VCall+add+store.slot` 目标、跨 RC 转发夹 `load.slot`，否则回退 7%。新增 565。
>
> 验证：普通/ASan 闸门 **469/469**（EXIT=0）；difftest **440/0**（8 路并行；默认
> nproc 路并行时 4 个 tarfile 用例偶发超时假 DIFF，单跑 0 差异）；O0/O1/O2 全过；
> check_size 仍只报 HEAD 既有两个超限；全量内置基准 53 项几何 0.979×（3 个 <1.2ms
> 标记用例与 0.469 产物 asm 逐字节相同，纯噪声）；Codon 九例几何 0.869
> （obj_dispatch 292.8 / 114.2 ms）。
>
> 文档：0.470 同步 + 全仓漂移收口（`index`/`testing`/`roadmap`/`README`/`compatibility`/
> `development`/`codegen` 的版本号、闸门项、用例计数、check_size 超限清单、异常默认路径均
> 按当前 HEAD 校正，无法复现的旧数字标注口径）；新增 [docs/next-round-plan.md](docs/next-round-plan.md)
> （0.471 起路线图：P0 收口 / P1 浮点槽与 RC 借用 / P2 能力面 / P3 工程债）。

## [0.469] - 2026-11-24

> refactor：**LSP 拆成独立仓库 pya-lsp**，编译器回归「只是编译器」。
>
> `tools/lsp/` + `tests/lsp/` 迁出本仓库；新仓库用 `git subtree split` 保留原有提交历史，
> 布局扁平化（源码在根目录，冒烟在 `tests/`），编译器改为外部依赖：只认 `$PYADAMAS`
> 或 PATH 里的 `pyadamas`，不再推断 `build/*/pyadamas`。
>
> 本仓库相应移除 `lsp-smoke` 闸门（checkall 少一项，其余闸门编号顺延）；LSP 功能与行为
> 不变，迁出时点冒烟 **228 checks** 全过。后续 LSP 回归在 pya-lsp 仓库里跑。

## [0.468] - 2026-11-24

> refactor(lsp)：**删除 minijson.py 薄封装，LSP 直接用 runtime 动态 JSON**。
>
> 出包用 `json.stringify`（字符串转义/紧凑 JSON/非 ASCII → \uXXXX），Content-Length 用
> `len_bytes`；入包不再经过 204 行的同名转发层。runtime `json.*` 是严格口径（`as_*`
> 类型不符抛 TypeError、`get` 缺失抛 KeyError），而 LSP 入参常省略字段或给 null，
> 所以 `lsp_protocol.py` 保留 9 个宽容取值 helper
> （`j_has/j_get/j_str/j_int/j_bool/j_obj/j_arr/j_at/j_size`），语义与旧 minijson
> 访问器一致：缺失/类型不符 → null、""、0、False、0 长度。
>
> 顺带修正 LANGUAGE_GAPS 第 2 条：跨模块 class 只在「入口模块的局部变量/返回值」上
> 可用；用作**函数形参类型**时，函数体里读字段仍退化成 unknown 并报
> `attribute access is only supported as a method call`（build/lead 与 3 个近期构建
> 实测）。因此 `Symbol` 与所有字段读写留在 lsp_doc.py。
>
> 验证：build.sh 成功；LSP 冒烟 228 checks；全闸门 **463 ran ok / difftest 435-0 /
> bench 36/36 / LSP 228 / perf warn=0 fail=0**。

## [0.467] - 2026-11-24

> refactor(lsp)：**把 2438 行的 `tools/lsp/main.py` 拆成 7 个模块**（纯搬移，行为不变）。
>
> `lsp_text`（文本/UTF-16 位置/定义查找）、`lsp_protocol`（帧/响应/环境/日志）、
> `lsp_state`（文档表/增量编辑/workspace）、`lsp_diagnostics`（临时文件 + check +
> publish）、`lsp_intel`（definition/hover/semanticTokens/signatureHelp/
> documentHighlight）、`lsp_doc`（Symbol 扫描/补全候选/documentSymbol/foldingRange/
> formatting/workspace symbol）、`lsp_refs`（references/rename）；`main.py` 只留
> stdio 读循环、生命周期、didChange debounce 与 dispatch（2438 → 264 行）。
>
> `Symbol` 与它的字段读写留在同一模块（lsp_doc）：当前编译器不支持跨模块 class 的
> 字段读/方法调用（`attribute access is only supported as a method call`），
> 拆到别的模块会编译失败。跨模块读文档表/workspace 根用 `lsp_state.xxx` 模块属性
> （`from ... import` 是快照绑定，模块级变量重新赋值后不会更新）。
>
> 验证：`build.sh` 成功；LSP 冒烟 **228 checks 全过**（与拆分前一致）；编译器/运行时
> 未改。

## [0.466] - 2026-11-24

> perf(codegen)：**kind0 列表写入的 elem_kind 检查合并为一次比较**（fannkuch −5.61%，
> 相对 rc_dec 之前的累计约 −10%）。（未提交）
>
> `gen_index_store` 的 hk==0 热路径原先每次写都要 `mov elem_kind + cmp1/je + cmp2/jae`
> （5 条），现在用 `cmp $0, 24(list)` + `jne cold`（2 条）；kind1/2/3 全走冷分发
> （冷路径重读 kind），语义与 `pa_list_set` 的升级/位图/旧值释放一致。判据只看
> 「新值静态非堆」，不匹配基准/文件名。
>
> 负面结果（已回退并记录）：**LoadSlot 块内 CSE**（合并重复 `load.slot`）实测
> fannkuch **+6.4%**（33.29M → 35.41M）——省掉的 L1 装载被更长的活跃区间与额外
> spill 抵消；按纪律回退，不保留代码。
>
> 验证：fannkuch **33,292,318 → 31,424,592（−5.61%）**；12 基准无回退（nqueens
> +0.28% 为布局噪声，<1%）；输出逐字节相同；闸门 **463 ran ok / difftest 435-0 /
> bench 36/36 / LSP 228 checks / perf warn=0 fail=0**。

## [0.465] - 2026-11-24

> feat(lsp)：**`textDocument/formatting`（保守版）+ `workspace/symbol`**。（未提交）
>
> formatting 只做两件事：行首缩进按块层级归一到 4 空格（tab 按 4 计）+ 行尾空白清理；
> 括号续行与三引号字符串内部原样保留，不重排表达式、不合并/拆分代码行；
> `else/elif/except/finally` 先退一层再对齐。逐行整行替换的 TextEdit，幂等。
> workspace/symbol 用 initialize 的 `rootUri`（退 `workspaceFolders[0].uri`/`rootPath`）
> 递归扫 `.py`/`.pyi`，上限 2000 文件，跳过隐藏目录/`__pycache__`/`build`/`node_modules`，
> 复用 `scan_symbols` 输出 `SymbolInformation`；query 按子串过滤。
>
> 验证：LSP 冒烟 **201 → 228 checks**（formatting 归一/幂等、workspace 跨文件/子目录/
> 查询过滤/file URI/range）；编译器与运行时未改，既有闸门 **463 ran ok / difftest 435-0**
> 不受影响。README 方法表与限制同步。

## [0.464] - 2026-11-24

> refactor(native)：**`pathlib.Path` 方法表迁到声明式签名**——最后一个原生类的方法表。（未提交）
>
> 新增 `stdlib/sigs/pathlib.pysig`（`class Path` + 构造器 + 18 个方法）；
> `declared_class_type()` 登记 `pathlib.Path → TyKind::Path`；`typecheck_call_method.cpp`
> 的 76 行硬编码方法表换成声明查表（参数个数/类型检查、未知方法报错统一）。
> `/` 运算符、构造器降级（`lower_import_misc.cpp`）、`mkdir` 关键字白名单与全部参数装配
> （`lower_method_native.cpp`）保留 C++；`unlink`/`rmdir` 没有 `pa_path_*` 运行期入口
> （降级是 PathStr + OsRemove/OsRmdir），仍留在 method_call。
>
> 记账（如实）：C++ **−22 行**（builtin_sigs −3、native_sigs +1、typecheck −20），
> 新增 pysig **49 行**，源码合计净 **+27 行**；价值在方法清单集中与未知方法自动报错，
> 不是行数减少。`write_text`/`write_bytes` 返回 0 而非 CPython 的写入长度是**预存差异**
> （`pa_path_write_text` 原本就 `return 0`），本轮未改。
>
> 验证：12 基准 × O0/O1/O2 **36/36 汇编逐字节相同**；Path 全方法示例与 CPython
> **19 行 0 diff**；闸门 **463 ran ok / difftest 435-0 / check_size 通过**；
> 新增 `tests/run/560_pathlib_pysig.py`。

## [0.463] - 2026-11-23

> fix(runtime)：**`sorted(key=返回用户类实例)` 不再静默不排序**——`pa_list_sorted_by`
> 过去按机器字（堆地址）比键，对象顺序≈分配顺序，看起来像 no-op；现在 kind-3 键
> 复用 `pa_list_sort` 的族分类与 `__lt__` 路径。
>
> 做法：把 kind-3 的族分类/可比较性校验抽成 `uk_sort_family`（`pa_list_sort` 与
> `pa_list_sorted_by` 共用），OTHER 族在比较器里按**左操作数实例**的 `__lt__` 槽
> 逐对分派（CPython 口径，不再假定全表同一类）；NUM 族改走 `pa_uk_num_cmp`
> （unknown 的 float/big 键不再按位模式/指针比），STR 族走字节序；qsort 相等键
> 用原下标钉稳定性。校验前置到拷贝/分配之前，用户 `__lt__` 执行期间把结果列表
> pin 成 GC 根。
>
> 边界：unknown 返回类型的**元组/列表键**仍明确 TypeError（与 `list.sort()` 同口径，
> 标注返回类型即可走描述符路径）；没有 `__lt__` 的类仍抛 `object`/`object` TypeError。
>
> 用例：`tests/run/559_sorted_key_class.py`（类实例键 / 未标注返回类型 / 每键新建实例 /
> 稳定性 / reverse / unknown float 与 str 键 / TypeError / 大表 / 原地 sort）。
>
> 验证：全闸门 **462 ran ok / difftest 434-0 / 12 基准 × 3 档 36/36 / check_size 通过 /
> LSP 冒烟 / SIMD 变体 / perf warn=0 fail=0**。

## [0.462] - 2026-11-23

> feat(lsp)：**`signatureHelp` + `documentHighlight` + `foldingRange`**，与 references/rename 合并（冒烟 **201**）。（commits 6b55e9d、668449f）
>
> signatureHelp：括号栈定位最内层调用 `(`，复用 hover 的签名提取；`activeParameter` 按深度 0
> 逗号数（跳过字符串/注释/嵌套括号），支持跨行调用；无参函数返回 `parameters=[]`。
> documentHighlight：整词扫描，写入/赋值目标/形参标 Write(3)，其余 Text(1)。
> foldingRange：def/class 块、≥2 行连续注释、跨行三引号字符串，kind 为 region/comment。
> initialize 能力表补三个 provider；合并只冲突 dispatch/capabilities/smoke URI，两套 elif 全保留。
>
> 验证：冒烟 **PASS 201**（119 原有全绿；lsp-b 单分支 171）；闸门 **461 ran ok / difftest 433-0 /
> bench 36/36 / perf warn=0 fail=0**。

## [0.461] - 2026-11-23

> feat(lsp)：**`textDocument/references` + `rename`**（冒烟 119 → 149）。（commit ffcb29d）
>
> references：光标符号做整词扫描（跳过注释与普通/三引号字符串），返回文档内全部 Location；
> rename：同一扫描生成 WorkspaceEdit（单文件 changes），新名字按整词替换。
>
> 边界：只搜当前文档（无 workspace 索引）；不支持 `prepareRename` 与 `documentChanges`。
> 验证：`tests/lsp/smoke.py` **PASS 149**（原 119 全绿），往返 <1s 护栏保留。

## [0.460] - 2026-11-23

> perf(opt)：**循环版本化 + 仿射边界检查消除**（nqueens −11.35%）。（commits 9a2cd85、8b8f52e、b1ed64f）
>
> 对仿射下标的循环做版本化：入口守卫成立时走免边界检查副本，否则回退原循环；
> 顺带把 `opt_intloop.cpp` 拆出 `opt_intloop_bce.cpp`（1204 → 248 行，过 check_size 硬限）。
> 拆分是纯搬移（nqueens 差 22 条指令 = TU 顺序布局噪声）。
>
> 验证：nqueens **−11.35%**；12 基准合计 **−8.54%**（现值见 `tools/perf_baseline.txt`）；
> 闸门 **461 ran ok / difftest 433-0 / bench 36/36 / perf warn=0 fail=0**。

## [0.459] - 2026-11-23

> perf(codegen)：**整数 Bin 结果留 `%rax` 的相邻 Cmp/Bin 融合**（collatz −20.5%、matmul −1.1%）。（commits 4f37d9c、739824e）
>
> 整数 Bin 的结果不再立即落临时槽：下一条 Cmp 或 Bin 直接消费 `%rax`，省掉一对 store/load。
> 融合在发射层按指令相邻性与寄存器类判定，不改 IR 语义。
>
> 验证：collatz **−20.5%**、matmul **−1.1%**（基线 8c77367）；12 基准无回退；
> 闸门 **461 ran ok / difftest 433-0 / bench 36/36 / perf warn=0 fail=0**。

## [0.458] - 2026-11-23

> perf(opt)：**`rc_dec_unknown` 按静态堆类型特化 + 非堆槽写入不读旧值**（fannkuch −4.56%）。（commits f023c1b、898f5c8）
>
> unknown 释放按编译期已知的堆种类分派到特化 dec，省掉逐值探针；静态非堆的槽写入不再先读旧值。
> 判据只看 IR 静态类型，不匹配基准/文件名。
>
> 验证：fannkuch **−4.56%**（基线 b3812d7），12 基准无回退；
> 闸门 **461 ran ok / difftest 433-0 / bench 36/36 / perf warn=0 fail=0**。

## [0.457] - 2026-11-23

> refactor(native)：**`collections.deque` 方法表迁到声明式签名**（10 个实例方法）。（commit 0b76046）
>
> `stdlib/sigs/collections.pysig` 新建 `class deque:`，10 个方法全部 `@native("pa_deque_*")`；
> `declared_class_type()` 登记 `collections.deque → TyKind::Deque`，`typecheck_call_method.cpp`
> 的 Deque 方法表换成声明查表。构造器、`pop/popleft/copy` 的「接收者元素类型」返回、参数装配、
> 序列协议与 `maxlen` 属性保留 C++（pysig 表达不了）。
>
> 验证：12 基准 × O2 与迁移前**逐字节相同**；deque 10 方法示例指令数 **237,934 完全相同**；
> CPython 对拍一致；闸门 **461 ran ok / difftest 433-0 / bench 36/36 / perf warn=0 fail=0**；
> 新增 `tests/run/558_deque_pysig.py`。
>
> 已知差异（预存）：`repr(deque)` 是 `<deque object>`；未指定 `maxlen` 时返回 `-1`（CPython 是 `None`）。

## [0.456] - 2026-11-23

> lang：**`enum` / `abc` 编译期类语义**——`enum` 成员变成真正的成员对象（有 `.name`/`.value`），`abc` 阻止抽象类实例化。（commit d8d6c47）
>
> 机制：两个「语义特判 + 共享实现」，**没有**做通用 class-creation hook——pyadamas 的类是编译期静态布局，运行期 hook 改不了已生成的槽位，通用化需要元类语义，代价远大于收益。`abc` 只要一个编译期事实位，`enum` 只要类体赋值拦截，两者都落在 typecheck 层（`dataclasses` 已验证这条路）。
>
> 新增 `src/typecheck_enum.cpp`（95 行）；`ClassInfo::is_enum`/`MethodInfo::is_abstract`；`lower_class.cpp` 为每个成员构造实例（ObjNew + value/name 槽）；`Color(1)` 值反查；三个迭代入口（`for`/`list()`/推导式）。
>
> 对拍 CPython：`Color.RED.name/value`、`Color(2).name`、`Color.RED == Color(1)` True / `== 1` False、`len(list(Color))`、`[c.name for c in Color]`、抽象类子类实例化全部一致；抽象类实例化编译期报错带行号。
>
> 边界：enum 别名（重复值）选择**明确报错**而非静默合并；`auto()`/`IntFlag` 位运算/`__members__`/functional API 未支持；`ABCMeta` 当 metaclass、`abc.register()` 未支持；**P3 通用化未做**，`dataclasses` 仍是按名字特判。
>
> 验证：12 基准 × O0/O1/O2 **36/36 逐字节相同**；类/OOP 用例 × O0/O2 **44/44 相同**（普通类语义未变）；闸门 **458 ran ok / difftest 430-0 / bench 36/36 / perf warn=0 fail=0**。

## [0.455] - 2026-11-23

> test/stdlib：**未测模块与浅覆盖模块的 CPython 对拍验收**（22 个模块）。（commits 7cfd563、523752a）
>
> 背景：48 个纯 `.py` 模块里 7 个零测试、20 个只有 1 个测试；`decimal` 有 956 行却零测试。
>
> 修到的真 bug：`decimal.to_integral_value` 缺失（4 行薄包装）；**`copy.copy()` 直接 `return x`（浅拷贝根本不复制，别名共享）**、`copy.deepcopy()` 缺失；`calendar` 缺 `MONDAY..SUNDAY` 常量。
>
> `dataclasses` 经复核**能用**（此前判「坏」是没开 lang-gaps 门控的误判）。
>
> 编译器缺口结论（成为 0.456 的派单依据）：`enum` 需要类体赋值拦截 + 元类语义（判据 `lower_class.cpp:491`、`typecheck_func.cpp:488`）；`abc` 需要抽象方法标记 + 实例化检查（`typecheck_call.cpp:442`）。
>
> 限制如实记录：`codecs` 的 `errors=` 被忽略（运行时 ABI）、`decimal` NaN/Infinity 不支持、`contextlib.suppress` 需 lang-gaps 门控。
>
> 验证：22 个新用例（534~555）各自 ran ok；闸门 **458 ran ok**。

## [0.454] - 2026-11-23

> feat(sigs)：**`.pysig` 支持原生类声明**，`random.Random` 全量迁移。（commit 0f2771c）
>
> ```python
> class Random:
>     @native("pa_random_new")
>     def __init__(self, x: int = 0) -> None: pass
>     @native("pa_random_gnext")
>     def random(self) -> float: pass
> ```
>
> `TyKind::Random` 的 ~58 行硬编码方法表 → 声明查表；删 `builtin_sigs.cpp` −18、`lower_import.cpp` −5。方法降级（参数装配）保持 C++ 不动——这是 asm 能逐字节相同的原因。
>
> **CPython 同种子逐位对拍**（MT19937）：`Random(42).random()` = `0.6394267984578837`、`randint`/`uniform`/`choice`/`seed(5)` 全部一致。
>
> 唯一留在 C++ 的一层：`declared_class_type()` 把 `(module, class)` 映射到内建类型种类——声明格式表达不了「对象类型种类」，与 `t_int()`/`t_list()` 同属内建事实。
>
> 踩坑（两条都是静默算错型）：`resolve_rt_symbols()` 只扫模块函数，**类方法的 `@native` 符号没被反查** → 构造器被降级成 `pa_list_new`；`FuncInfo` 缺 `ret_specialized` 承载位。
>
> 验证：12 基准 × 3 档 **36/36 相同**、类示例 **14/14 相同**；闸门 **435 ran ok / difftest 407-0**。

## [0.453] - 2026-11-23

> feat(sigs)：**`@ret_specialized` + `INT64_MIN`/`NaN` 字面量**；`random.choice/sample/choices/seed` 与 `math.nan` 全部声明化。（commit d3aebfe）
>
> **真正的坑不是 `ret_known`**：硬编码里这三个函数用**模块限定名** `"random.choice"`，而 `check_args()` 靠 `fn->name` 前缀分派到 `random_call()`；pysig 写裸名 → 分派不触发 → 调用点退化 unknown → GC 扫描 mask 出现 → **栈帧 144→192**。修法：声明函数的 `FuncInfo::name` 统一写成 `module + "." + name`。
>
> 通用教训：**签名表不只含类型，还含「名字」所携带的分派语义。**
>
> `INT64_MIN`：`9223372036854775808` 被词法解析成 **u64 位模式**，旧代码直接拒绝；改为按 `uint64_t` 重新解释幅度再折叠负号。
>
> `NaN`：`literal_of` 识别合法 Python 调用形式 `float("nan")`/`float("inf")`；`math.pysig` 改 `nan: float = float("nan")`，**删掉 `builtin_value` 那 8 行 C++ 特判**——此前 `nan = 1e400`（实为 +inf）是「声明说谎」。
>
> 验证：12 基准 × 3 档 **36/36 相同**；`choice` 调用点 `-O0` 栈帧 **144 == 144**；闸门 **434 ran ok / difftest 406-0**。

## [0.452] - 2026-11-23

> feat(lsp)：**LSP 服务器补齐主线能力**——completion / definition / semanticTokens / 诊断 debounce。`tools/lsp/main.py` 649 → 1219 行。（commit 07e1826）
>
> 冒烟断言 **58 → 119**（+61）。semanticTokens 按 **UTF-16 code unit** 编码（deltaStart 与 length 都是），用「中文 + 😀（BMP 外）」逐 token 校验；三引号跨行 docstring 用状态机。
>
> minijson 换 `json.parse`：**543 → 204 行**（手写 Parser/stringify 整段删除），保留旧宽容语义所以 `main.py` 调用点一行没改；切换前后各跑一次 smoke（PASS 58 → PASS 58）。
>
> stdin 保留**二进制无缓冲** `open(0, "rb", 0)`：LSP 帧是字节语义，且 debounce 依赖「fd 可读 == 还有待处理帧」，有 FILE 缓冲时 poll 会误判。
>
> 已知天花板：`pyadamas check` 不校验模块属性（`json.nonexistent_attr` check 放行、build 才报）。

## [0.451] - 2026-11-23

> refactor(native)：**`os` / `random` 模块级函数迁到声明式签名**。（commits 02b5b07、14aa9b0）
>
> `os`：16 函数 + 6 常量（`getcwd/cpu_count/getpid/mkdir/rmdir/remove/unlink/chdir/listdir/getenv/rename/chmod/access/makedirs/system/close`），净删 ~88 行 C++；`unlink` 无独立 RtFn，用 `@native("pa_os_remove")` 指到 `OsRemove`。
>
> `random`：9 个模块级函数（`random/randint/uniform/shuffle` 用 `@native` 覆盖到 `pa_random_g*`），净删 ~53 行；保留 `seed`（当时 INT64_MIN 字面量表达不了）、`Random` 类、`choice/sample/choices`（当时 `ret_known` 无法声明）。
>
> `operator` **跳过**：31 个函数全走 `operator_call()` 类型推导 + `lower_operator()` 专门降级，**无 `pa_operator_*` RtFn 入口**，pysig 的符号反查硬失败。判据写进 cookbook：**只有存在 `pa_<模块>_<名字>` RtFn 的函数才能声明**。
>
> 验证：12 基准 × 3 档 **36/36 相同**；闸门 **432→433 ran ok / difftest 404-405-0**。

## [0.450] - 2026-11-23

> refactor(native)：**`sys` 部分签名声明化 + `.pysig` 默认按名字合并**。（commits e4022e4、5386ba8）
>
> `sys` 有 6 个成员无法用声明表达（`argv: list[str]`、`executable: str`、`stdin/stdout/stderr: file`、`exc_info()` 返回含 ClassObj），
> 而 `module_info()` 原是**整模块替换** → 这些成员变成 `module has no attribute`，大量用例会崩。
>
> 修法：pysig hook 改为在 `builtin_module()` 末尾**逐名合并**（声明优先覆盖，未声明的保留硬编码）。
>
> **这是高杠杆修复**：否则 `operator`/`os`/`random` 每个「半能声明」的模块都要加一个 C++ 专用桥接分支，「零 C++ 迁移」就退化成「每模块 30 行 C++」。
>
> merge 直接证据：临时只声明 `sys.maxsize` → `sys.argv`/`stdout`/`exc_info`/`exit` 全部继续可用。
>
> 验证：12 基准 × 3 档 **36/36 相同**；闸门 **431 ran ok / difftest 403-0**。

0.100–0.199 的逐版本记录已轮转到 [docs/archive/changelog-0.100-0.199.md](docs/archive/changelog-0.100-0.199.md)（2026-11-24）。
0.200–0.449 的逐版本记录已轮转到 [docs/archive/changelog-0.200-0.449.md](docs/archive/changelog-0.200-0.449.md)（2026-11-25）。
