# z2_codegen —— Z2-b2：把零开销异常接进 codegen（第一刀，最小闭环）

这是 [docs/zero-cost-exceptions.md](../../../docs/zero-cost-exceptions.md) Z2 的 codegen 接线，
也是 [docs/exc-semantics.md](../../../docs/exc-semantics.md) §6.4「最小改动」的落地验证。
运行时胶水（throw / landing / 方案 G 帧清理）见 [../z2_rt/](../z2_rt/)（Z2-b1，已完成）；
本目录只验证**编译器发射**这一半：同一个 `.py` 用老路径（setjmp）和
`--zero-cost-exc` 新路径各编一次，行为必须逐字节一致，而新路径的汇编里
**不能再出现 `call _setjmp`**。

## 怎么跑

    bash tests/exc/z2_codegen/run.sh build/dev build/dev/pyadamas
    # 或者省略参数，默认就是 build/dev

预期输出（每个用例 11 项断言，9 个用例共 99 项）：

    ==== 结果：99/99 PASS ====
    PASS

## 每个用例检查什么

1. **老路径确实覆盖目标形状**：默认（开关关）汇编里有 `call _setjmp`，
   且**没有** `.cfi_personality`（证明开关默认关、老路径一个字没变）。
2. **新路径不再用 setjmp**：`asm --zero-cost-exc` 里
   `call _setjmp` 与 `call pa_handler_push` 都是 0 处。
3. **LSDA 真的发了**：汇编里出现 `.cfi_personality` / `.cfi_lsda` /
   `.section .gcc_except_table`，头部两字节是 `ff 9b`（GCC 同款
   catch-all 20 字节形态，与 [tools/lsda_ref/FINDINGS.md](../../../tools/lsda_ref/FINDINGS.md) 一致）。
4. **行为一致**：两条路径各 `run` 一次，stdout+stderr 逐字节 `cmp` 相同，
   并且与用例里的 `# --- expect ---` 逐字节相同。
5. **产物级证据**：新路径可执行文件里有 `.gcc_except_table` 段；
   `.eh_frame` 的 CIE augmentation 是 `zPLR`（personality + LSDA 都在）。

## 用例清单

| 文件 | 覆盖点 |
|---|---|
| `cases/01_direct_raise.py` | 函数内直接 raise（两个类型），`except Exception as e` |
| `cases/02_try_normal.py` | try 体正常完成（非异常路径不能有行为变化） |
| `cases/03_bare_except.py` | 裸 `except:`（无条件 catch-all 分发） |
| `cases/04_assert_in_try.py` | try 体里 `assert` 失败 → raise 路径 |
| `cases/05_try_in_loop.py` | try 在循环里反复进入/退出（RcMark/RcCleanup 水位） |
| `cases/06_rethrow_to_outer.py` | 新→新跨函数展开：内层不匹配 → `pa_eh_rethrow` → 外层 catch-all |
| `cases/07_bare_reraise.py` | 裸 `raise`（异常上下文来自调用方 except 体）→ `pa_eh_reraise` |
| `cases/08_method_nested.py` | 方法体 / 嵌套函数体里的 try（函数种类不影响形状判定） |
| `cases/09_empty_and_last.py` | try 体为空（pass）、try 是函数最后一条语句（begin/end 可能相邻） |

运行时入口与用例的对应关系（便于定位）：`pa_eh_raise` 由 `assert` /
`raise X`（无实例构造）覆盖（04），`pa_eh_raise_value_from` 由
`raise X("msg")` 覆盖（01/02/03/05），`pa_eh_reraise` 由 07 覆盖，
`pa_eh_rethrow` 由 06 覆盖，`pa_eh_land` 每个用例都走。

## 编译器侧改了什么（最小闭环）

**开关**：`--zero-cost-exc`（默认关），写进模块缓存清单（`zerocostexc` 行）
参与缓存失效。关闭时所有输出与原路径逐字节一致。

**形状门禁**（`src/lower_exc.cpp` 的 `find_zero_cost_try`）——只有同时满足
下面全部条件的函数才走新路径，其余函数保持 setjmp 路径：

1. 非可挂起（函数体里没有 yield/await）；
2. 函数体里恰好一条 `try`、没有任何 `with`；
3. 那条 `try` 恰好一个 handler，且是 catch-all（裸 `except` 或
   `except Exception`），没有 `else` / `finally`；
4. try 体与 handler 体里没有 `return` / `break` / `continue`
   （第一刀要求「try 之后才 return」）；
5. try 体里的 `raise` 只用「无参 / 单位置参」形态，且不是富异常实例
   （多参 raise / 富异常实例对应的 `pair` 还没有 `_Unwind` 版本）。

**IR**（`src/ir_ops.h`、`src/ir_types.h`）：新增三条标记指令
`EhBegin` / `EhEnd` / `EhLandingPad`，以及五个运行时入口
`EhRaise` / `EhRaiseValueFrom` / `EhReraise` / `EhRethrow` / `EhLand`。
`IRFunction::eh_regions` 记录保护区/landing pad 的块号（给不可达块裁剪当根用）。

**降级**（`lower_exc.cpp`）：

- try 进入：`RcMark` 取临时栈水位（替代 `pa_handler_push` 的那一半；
  handler 记录栈在表驱动里不存在）；
- try 体：`EhBegin` … `EhEnd` 保护区；正常退出 `RcCleanup(mark)`；
- landing pad：`EhLandingPad` → `EhLand`（`pa_eh_land`：取回异常 +
  收掉展开包装）→ `RcCleanup(mark)` → 现有 except 分发块
  （`ExcMatches` / `ExcAcceptValue` / `ExcDone` 一行不改）。
  分发不匹配时用 `EhRethrow`（`pa_eh_rethrow` 重启 phase 1），不用
  `ExcReraise`（longjmp）；
- try 体里的 raise 换 `pa_eh_raise*`（`raise_rt()` 按 `zc_raise_depth` 映射），
  同一个函数里不再有 `raise → exc_longjmp` 路径（docs/exc-semantics.md §5.2
  的硬约束：同一函数内不许混用两种展开）。

**codegen**（`codegen_block.cpp` / `codegen.cpp` / `codegen_ops.cpp`）：

- 函数入口预扫描三条标记，分配 `.LehB*` / `.LehE*` / `.LehLP*` 标签；
- `.cfi_personality 0x9b, DW.ref.pa_personality` + `.cfi_lsda 0x1b, .LLSDA<N>`
  紧跟 `.cfi_startproc`；偏移基准是 **FDE 起点**（`fname` 标签），不是
  `.LfN`（两者之间隔着 `.p2align` 的填充）；
- 所有函数发完后统一发 `.gcc_except_table`（每个函数一份 LSDA，catch-all
  形态 A）+ 每个 TU 一份 `DW.ref.pa_personality`。

**运行时**（`runtime/rt_eh.c` 扩展）：新增长跳版抛出的 `_Unwind` 对应物
`pa_eh_raise` / `pa_eh_raise_value_from` / `pa_eh_reraise` /
`pa_eh_rethrow` / `pa_eh_land`，异常对象构造与全局状态逐项复刻
`rt_exc.c`，只把最后一步 `exc_longjmp` 换成 `pa_eh_throw`。

## 已知限制 / 还没接的形状（第一刀）

1. **C 运行时内部抛的异常**（`pa_raise_c`：IndexError、ZeroDivisionError、
   文件/JSON 之类的错误）仍走 `exc_longjmp`。新路径 try 抓不到它们 ——
   `try: xs[9]`、`try: 1 // 0` 这类用例在新路径下会按未捕获退出。
   要接得改 `rt_exc.c` 的抛出收口（本任务禁止触碰），属于下一刀。
2. **老路径函数（没有 try，或用的是 setjmp 路径）里 raise、被新路径 try
   调用时**：它 longjmp 去找 handler 栈，看不到新路径 try 的 landing pad。
   跨函数的「新→新」传播是通的（用例 06），「旧→新」传播要等全部函数
   迁完（Z5）或加动态模式标记。
3. **except 体里再 raise**：handler 体没有自己的保护区（第一刀不做
   cleanup landing pad），所以 handler 体内发生的 raise 不在新路径语义内。
   形状门禁目前已把「handler 体里显式 raise」排除，但被调函数里的 raise
   仍可能发生。
4. **finally / with / 嵌套 try / 多 handler / 带 else 的 try**：保持 setjmp
   路径（`docs/zero-cost-exceptions.md` 的 Z2 范围收紧）。
5. **return / break / continue 穿过 try 体**：保持 setjmp 路径
   （`emit_cleanup_exits` 那套早退清理第一刀不迁移）。
6. **多参 raise / 富异常实例 raise**：`pa_raise_args` / `pa_raise_instance`
   还没有 `_Unwind` 版，形状门禁排除这类 try 体。
7. **Win64（`gnu_cfi == false`）**：不发射新路径（表驱动展开只在
   ELF/gnu_cfi 目标上有意义）。

## 复跑命令

    bash tests/exc/z2_codegen/run.sh build/z2b2 build/z2b2/pyadamas
    bash tests/run_tests.sh build/z2b2 build/z2b2/pyadamas          # 开关关：全量回归
    PYADAMAS_TEST_JOBS=2 bash tests/run_tests.sh build/z2b2 build/z2b2/pyadamas
