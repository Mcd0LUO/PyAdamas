# z2_rt —— Z2-b1：零开销异常的运行时胶水 + 方案 G 帧清理

这是 [docs/exc-frame-cleanup.md](../../../docs/exc-frame-cleanup.md) 里 **方案 G** 的落地验证，
也是 [docs/zero-cost-exceptions.md](../../../docs/zero-cost-exceptions.md) Z2 的运行时部分（不接 codegen）：

- 新增 `runtime/rt_eh.c`：`pa_eh_throw` / `pa_eh_landing` / `pa_eh_done` 把「抛 + 落地」
  从 setjmp/longjmp 换成 `_Unwind_RaiseException` + landing pad，**不与 `rt_exc.c` 的
  setjmp 路径互相改写**（异常对象、类型匹配、handler 记录栈仍归 `rt_exc.c`）；
- 新增 `pa_frame_unwind_to_cfa`：在 personality 的 phase 2、`_UA_HANDLER_FRAME` 那一帧里、
  **安装 landing pad 之前**，把「比 handler 帧更深、被逻辑展开跳过」的帧记录批量释放
  （语义等价于老世界 `pa_frame_unwind(handler_top->frame)`）；
- `PyAdamasFrame` 加 `cfa` 字段（帧记录头 5 字 → 6 字），codegen 压帧时写入；
- 夹具用**手写汇编**摆帧记录 + GCC 同款 catch-all LSDA，不碰编译器。

## 怎么跑

    bash tests/exc/z2_rt/run.sh

- 依赖：Linux x86-64、gcc、binutils（readelf）。脚本用 gcc 直接编译运行时各部分
  （不碰仓库的 CMake/ninja 构建），构建产物放 mktemp 临时目录，退出即删。
- 预期：`-no-pie` 与 PIE 两套链接各 61 项断言全过，外加 GC 关闭的 10 万次循环 6 项、
  两组反证，脚本退出码 0，约 15–20 秒：

      ==== 结果：全部 PASS（两套链接共 134 项断言；含 GC 开/关与两组反证）====

## 产物

| 文件 | 作用 |
|---|---|
| `../../../runtime/rt_eh.c` | **新增**：展开包装（`_Unwind_Exception` + 异常载荷）、`pa_eh_throw` / `pa_eh_landing` / `pa_eh_done`、`pa_frame_unwind_to_cfa`（方案 G 的批量释放 + 重入 guard）、personality 帧清理钩子的装配 |
| `../../../runtime/rt_personality.c` | phase 2 的 `_UA_HANDLER_FRAME` 分支里、`_Unwind_SetIP` **之前**调用帧清理钩子（钩子变量默认 NULL，单独链接 personality 的 Z1 夹具不受影响） |
| `../../../runtime/rt_internal.h` | `PyAdamasFrame.cfa` 字段、`PA_EH_EXC_CLASS`、胶水声明、`pending_exc_args` 的 extern |
| `../../../src/codegen_impl.h`、`../../../src/codegen_block.cpp` | 帧记录头 5 → 6 字（`rec_off` / `rec_words` / 槽地址从 `ri = 6` 起）；序言里在进链之前 `movq %rsp, rec_mem(5)` 写 CFA |
| `z2_threelayer.S` | 三层帧夹具：inner 抛 → mid（owned / owned|bit0 / 只扫描 bit1 三个槽）→ outer catch-all（有记录）+ norec 变体（handler 帧没有记录）+ 未捕获路径 |
| `driver.c` | 构造异常 / RC 对象 / 释放计数，断言帧链、释放次数、全局状态、RSS；反证开关 |
| `run.sh` | 一键编译 + 断言 + 反证，PASS/FAIL 打印，退出码 0/1 |

## 验证内容（每个 link 模式 61 项断言）

1. **L1 三层帧核心**：inner 抛，mid 有 3 个槽，outer catch-all 接住。
   - LP 入口 `pa_frame_top` 恰好是 outer 自己的记录（inner/mid 的记录已释放），
     `prev` 是更外层的驱动帧记录；
   - `rec->cfa == rec + 64`（与汇编手摆的 `rsp+112` 一致）；
   - inner 的 owned 槽、mid 的 owned 堆槽、mid 的 owned unknown（bit0）槽**各释放恰好一次**；
   - mid 的 bit1（只给 GC 扫的借来引用）**不释放**；outer 自己的 owned 槽也不被展开释放，
     由 handler 正常出口释放一次；
   - 观测计数器：personality 在 phase 2 handler 帧里释放调用 1 次、共释放 2 条记录；
     phase 1 不释放（有断言）。
2. **L2 landing 状态**：`pa_eh_landing` 取回的就是抛出的 `PyAdamasException*`；
   `pending_exc` 装好；富异常实例 / 多参 args 搬回 `current_exc_inst` / `pending_exc_args`；
   `current_exc` / `exc_save_top` 保持抛出时的值（与 longjmp 落地后逐项一致）。
   `pa_eh_done` 后展开包装存活数 0（不泄漏）。
3. **L3 handler 帧没有记录**：`z2_norec_fn` 不压帧记录，释放停止点是「cfa 到 handler 帧」，
   链上更外层的驱动帧记录保留 —— 这是方案 G 等价性证明的另一半。
4. **L4 未捕获路径**：`pa_eh_throw` 返回 `PA_EH_UNCAUGHT`（对应 `_URC_END_OF_STACK`），
   phase 2 不跑、不释放任何帧，`pending_exc` 仍归上层打印，`pa_eh_done` 收尾后不泄漏。
5. **V1 RSS**：100000 次 throw/catch 后 RSS 增量 <= 1024 KB（实测 +72 KB）；
   同一用例在 `PYADAMAS_GC_DISABLE=1` 下再跑一遍（实测 +16～24 KB）——判据不依赖 GC。
6. **反证（证明断言不空转）**：
   - 用 `-DZ2_NEGATIVE=1` 编出「中间帧不压帧记录」的夹具：帧释放计数断言立刻失败（退出码 1）；
   - 再用 `Z2_NEG_KEEP_GOING=1` 把 10 万次跑完：RSS 判据报出 **+29 MB** 的增量并失败
     （`> 1024 KB`），证明 RSS 判据确实抓得住漏释放；
   - 两种链接模式各跑一组。

## 实测出来的关键口径：`_Unwind_GetCFA` 在 personality 里是什么？

**结论：personality 在 phase 2 收到的 `_Unwind_GetCFA(ctx)` 不是 handler 帧自己的 DWARF CFA，
而是 handler 帧的「call site rsp」，也就是 handler 帧直接调用的那个内层帧的 CFA。**
所以 `pa_frame_unwind_to_cfa` 的停止条件写的是 `cfa <= target`（释放内层帧记录、保留 handler
帧自己的记录），而不是设计稿伪代码里的 `cfa < target`。

怎么量出来的（本夹具 `Z2_DEBUG=1` 会打印）：

- outer 帧真 CFA = `rsp_outer + 112`（与 `.cfi_def_cfa_offset 112` 同一口径），夹具记录里写的也是它；
  landing pad 入口 rsp 就是 outer 帧体 rsp（与 `tests/exc/z1_rt/README.md` 坑 6 一致）；
- personality 两阶段打印的 `cfa` 都等于 `rsp_outer`（= outer 帧体 rsp = outer 直接调用 mid 时的
  call site rsp）——比真 CFA 小 112（正好是本帧帧长），而不是 0；
- 数值上它恰好等于 **mid 帧的真 CFA**（mid CFA = mid rsp + 112 = outer rsp），这就是
  「等于内层帧 CFA」的直接证据。

为什么 z1 夹具没暴露：z1 的帧只有 8 字节（`subq $8`），真 CFA 与 `_Unwind_GetCFA` 只差 16，
逐帧比较时看起来「像」自己的 CFA（而且方案 G 当时只用它做单调排序，不卡等号）。
Z2 的帧几十到几百字节，差一个帧长就会「少释放一条记录」，本夹具直接把它抓成了 FAIL
（第一次跑：inner 释放了、mid 没释放、`pa_frame_top` 停在 mid 的记录上）。

### 边界与限制（与设计文档一致的已知项）

- **未捕获异常**：`_Unwind_RaiseException` 返回 `_URC_END_OF_STACK`，phase 2 不跑，被跳过帧
  的槽泄漏；本脚本只断言「不释放」，实际程序里上层会打印 traceback + `exit(1)`（Z2 接线后
  由入口 catch-all pad 兜住，见设计文档 §5.3）。
- **foreign（C++）异常穿过我们的帧**：`rt_eh.c` 的钩子先按 `exception_class` 过滤，不是我们的
  异常不碰 `pa_frame_top` 链；personality 的「不认领」还没做（属 Z3/Z4，见设计文档 §5.2-1）。
- **重入**：释放过程中若再发生展开（析构里 raise），`in_frame_release` guard 直接 `panic_c`
  报错，不静默。当前语义下不会发生（析构不跑用户代码）。
- **测试专用探针 / 开关**（都不影响生产路径）：
  - `Z2_DEBUG=1`：打印 personality 每帧 cfa、帧链快照、各帧 rsp 探针；
  - `Z2_TRACE=1`：打开 personality 逐帧日志；
  - `Z2_ONLY_LOOP=1`：只跑 10 万次循环用例（run.sh 的 GC 关闭/反证用）；
  - `Z2_NEG_KEEP_GOING=1`：断言失败也不中断循环，让泄漏反映到 RSS（反证用）；
  - `-DZ2_NEGATIVE=1` 是夹具的「不压记录」编译开关，只在 run.sh 的反证里用。
