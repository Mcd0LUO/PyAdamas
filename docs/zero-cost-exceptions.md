# 零开销异常 —— 设计草案

状态：**Z1 已打通**（手写汇编 + 自己的 personality，`_Unwind_RaiseException` 能跳到 landing pad，
两阶段顺序与 FORCE_UNWIND 行为都已实测）。LSDA 的字节布局已用本机 gcc 14.2 / binutils 2.44
**实测标定**，权威数据在 [`tools/lsda_ref/FINDINGS.md`](../tools/lsda_ref/FINDINGS.md)，
复核命令：`bash tools/lsda_ref/check.sh`。现有异常语义的逐条对照与改造边界在
[`docs/exc-semantics.md`](exc-semantics.md)。本文只保留结论与路线；被实测推翻的早期猜测标了「订正」。

## 现状与目标

当前实现是 setjmp/longjmp：

```asm
call pa_handler_push      # 压一条 handler 记录
call _setjmp              # 非局部跳转点
...
call pa_handler_leave     # 弹出
```

每个 `try` 进入都要付出这三条指令的代价。实测（0.173）：

| 场景 | 每轮开销 |
|---|---|
| 原始 `setjmp`（glibc 存信号掩码） | 122 ns |
| 改 `_setjmp` | 13 ns |
| 再加 handler 记录空闲链表 | **6 ns** |

**目标**：非抛出路径上 `try` 的运行时开销降到 0。只有抛出时才走表驱动展开。

## 已有的前置条件（已具备）

- **`.eh_frame` 已生成**：CFI 指令（`.cfi_startproc`/`.cfi_def_cfa_offset`/`.cfi_offset` 等）每个函数都有。
  这是最关键的一条 —— libgcc 的展开器**已经能逐帧走过我们生成的代码**。
- **libgcc 的 `_Unwind_*` 已经可用**：运行时里已有 5 处 `_Unwind_Backtrace`。
- 实测：生成程序的 `.eh_frame` 有 789 个 FDE，`libgcc_s.so.1` 已链接，反汇编里有 55 处 `call _setjmp`，
  且**没有任何 `.gcc_except_table`** —— 也就是说「表驱动展开所需的展开信息全都在，只是没有表」。

缺的只有三样：LSDA 表、personality routine、用 `_Unwind_RaiseException` 抛。

## LSDA 的确切格式（gcc 14.2 实测）

### 函数级指令

```asm
    .cfi_personality 0x9b, DW.ref.pa_personality
    .cfi_lsda 0x1b, .LLSDA0
```

- `0x9b` = `DW_EH_PE_indirect | pcrel | sdata4`（0x80|0x10|0x0b）
- `0x1b` = `pcrel | sdata4`（0x10|0x0b）
- **订正**：早期本文写成 `udata4` 是错的。`sdata4` 是 0x0b，`udata4` 是 0x03；0x0f 不是合法组合。
- `0x9b` 后面必须是**一个 8 字节指针槽**（`DW.ref.*`），不是函数本体：

```asm
    .section .data.rel.local,"aw",@progbits
    .align 8
DW.ref.pa_personality:
    .quad pa_personality
```

- **坑（已实测）**：`.cfi_personality 0x9b, pa_personality`（直接写函数符号、省掉 DW.ref 槽）
  **汇编和链接都不报错，但运行期 SIGSEGV**。
- 若 personality 指向**同一链接单元**内的符号，可以用 `0x1b` 直连、省掉 DW.ref（PIE / no-pie 都能跑）；
  但指向共享库符号（如 `__gxx_personality_v0`）时 PIE 下会被 ld 拒绝：
  `relocation R_X86_64_PC32 against symbol ... can not be used when making a PIE object`。
  我们的 `pa_personality` 在同一个可执行文件里，Z1 用 0x1b；将来若接共享符号再改 0x9b + DW.ref。

### 保护区标记

```asm
.LEHB0:                    # Exception Handler Begin
    call maybe_throw
.LEHE0:                    # Exception Handler End
```

这对标签界定「哪些 PC 落在保护区内」。我们的 lowering 里对应的就是 `try.body` 块的起止。
call-site 表里的 `start` / `length` 实测就是 `.LEHB0 - 函数起点` / `.LEHE0 - .LEHB0`。

### 两种 LSDA 形态（都实测过）

**A. 与 GCC 逐字节同构的 catch-all（20 字节）**

```text
ff 9b 11 01 08 04 05 13 01 1b 05 00 00 01 00 00 00 00 00 00
```

| 偏移 | 字节 | 字段 |
|---|---|---|
| 0x00 | `ff` | lpstart_encoding = omit（LPStart 取 FDE 的 region start，即函数起点） |
| 0x01 | `9b` | ttype_encoding = indirect / pcrel / sdata4 |
| 0x02 | `11` | ttype_table_offset（uleb128），**指向 TType 表末尾** |
| 0x03 | `01` | call_site_encoding = uleb128 |
| 0x04 | `08` | call_site_table_length = 8 字节 |
| 0x05 | `04 05 13 01` | call-site[1]：start=0x4, len=0x5, landing_pad=0x13, action=1 |
| 0x09 | `1b 05 00 00` | call-site[2]：landing pad 0 = 该区间无 landing pad |
| 0x0d | `01 00` | action[1] = (filter=1, next=0)，**两个 SLEB128** |
| 0x0f | `00` | `.align 4` 的 padding（不属于任何表，解码时别当记录） |
| 0x10 | `00 00 00 00` | TType[1] = null `type_info*` ⇒ **catch(...)**；**不需要重定位** |

**B. 我们自己的形态（Z1 实际用的，12 字节起）**

```text
ff ff 01 04 08 0e 1d 01 00 00 00 00
```

`ttype_encoding = 0xff`（omit，**TType 表偏移字段整段消失**）→ header 变成「2 字节 + 1 个 uleb128」。
call-site 的 action 下标 `01` 表示「这一帧是 handler」，`00` 表示「只做 cleanup」。
这个形态不需要 TType 表、不需要重定位，是我们的 personality 自己解析的。

**但它不是 ABI 语义上的 catch-all。** 用真的 `__gxx_personality_v0` 实测：`ttype_enc=0xff` +
action 记录 `(filter=0, next=0)` 会被当成 **cleanup**（landing pad 只跑一次、异常继续往外走）；
改成 `filter=1` 又因为取不到 TType 项而在 phase 1 直接 SIGABRT。
所以形态 B 的正确定位是「**字节容器 ABI 合法 + handler 语义是本项目私有扩展**」，
不能用于和 C++ 异常互通。**Z2 起一律用形态 A**；Z1 的夹具已按形态 A 升级。
详见 [`tools/lsda_ref/CROSSCHECK_M2.md`](../tools/lsda_ref/CROSSCHECK_M2.md)。

**判断 catch / cleanup 的正确层次**：ABI 的判据是 **action 记录里的 `filter`** ——
`filter > 0` 是 handler（指向 TType 表第 `filter` 项），`filter == 0` 是 cleanup，`filter < 0` 是
exception spec。call-site 的第 4 个字段只是「有没有 action 记录」的开关：为 0 表示该区间
完全没有 action（等价于 cleanup），非 0 才是 action 表下标。两层要分开看，早期本文把两层混了。

### 四条最容易踩错的（都踩过了）

1. **catch-all 的类型表不能省** —— 这条只对「与 gcc 同构的形态 A」成立。
   形态 A 里 catch-all = action 的 `filter > 0` + TType 表一个 **null** 项；`filter == 0` 才是 cleanup。
   早期本文说「类型表可以退化成空」是错的：省掉类型表**并用 libstdc++ 的 personality** 时，
   catch 会退化成 cleanup，永远抓不到异常。形态 B 我们是自己的 personality，才允许彻底没有类型表。
2. **action 表不是「条数 + 计数」**，而是每条记录两个 SLEB128：`(filter, next)`。
3. **TType 表是反向索引**：偏移指向表的**末尾**，第 i 项在 `ttype_end - i*4`。
4. **dump 里 null 项和真实项都显示 0**，`readelf -x` / `objdump -s` 分不出来；
   要看 `.rela.gcc_except_table` 有没有对应重定位才能区分。

## personality routine 契约

```c
_Unwind_Reason_Code personality(int version, _Unwind_Action actions,
                                uint64_t ex_class, struct _Unwind_Exception* exc,
                                struct _Unwind_Context* ctx);
```

**返回值有三个必需**（早期本文漏了第三个）：

| 返回码 | 何时 |
|---|---|
| `_URC_CONTINUE_UNWIND` | PC 不在保护区内 / 该区间无 landing pad / phase 2 还没轮到自己 / **FORCE_UNWIND** |
| `_URC_HANDLER_FOUND` | **phase 1**，PC 落在带 action 记录的区间，且 action 的 `filter > 0`、类型匹配（Z3 起） |
| `_URC_INSTALL_CONTEXT` | **phase 2 且 `actions & _UA_HANDLER_FRAME`**，装好上下文后展开器跳 landing pad |

phase 2 里轮到自己的 handler 帧时：
`_Unwind_SetGR(ctx, 0, (uintptr_t)exc)` + `_Unwind_SetIP(ctx, landing_pad)`，返回 `_URC_INSTALL_CONTEXT`。

**`_UA_FORCE_UNWIND` 必须只 CONTINUE、不得调用 `_Unwind_SetIP`。** 运行时 traceback 走
`_Unwind_Backtrace`（内部会 FORCE_UNWIND 扫一遍），personality 一旦在 FORCE_UNWIND 下装上下文，
traceback 会直接崩或者跳到 landing pad 执行用户代码。Z1 已把这条写成断言。

「释放帧里登记的堆引用」这件事**现有代码已经做了**（`pa_frame_unwind`），可以直接复用。

## Z1 实测出来的硬约束（Z2 的 codegen 必须遵守）

1. **landing pad 入口的 `%rsp` = 抛出点那条 `call` 之前的值**（也就是函数体内正常执行的 rsp），
   **不是 CFA，也不是函数入口 rsp**。所以 LP 的返回路径照抄正常 epilogue 即可。
   写错的表现是死循环（LP 的 `ret` 跳回自己）。
2. **LP 必须落在 `.cfi_endproc` 的 FDE 范围内**，且 LP 区段的 CFI 要描述 LP 入口状态。
   否则 LP 里 `_Unwind_Resume` 时 libgcc 找不到 FDE → **SIGABRT(134)，而且没有任何错误输出**。
   这条对编译器最危险：我们每个函数都在 `.cfi_startproc`/`.cfi_endproc` 之间发 LP，天然满足；
   但**把 LP 挪到另一个段或者补 `.cfi_endproc` 之后再发就会炸**。
3. 编译器现在的 CFI 状态在「保护区内的 call 点」上是一致的，LP 入口状态可以复用同一套；
   这一点要在 Z2 里用 md5 基线守住。
4. **cleanup landing pad 里若调用了可能抛出的函数**（析构、用户代码），必须补一条覆盖
   LP 区段的 call-site 记录；缺它时标准 personality 在 `_Unwind_Resume` 阶段会
   `found_terminate` -> terminate（Z1 的 cleanup 夹具实测复现过）。

## 分阶段落地（Z2/Z3 的范围已按实测收窄）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **Z1** ✅ | personality routine + 手写带 LSDA 的汇编函数端到端验证（不接编译器） | 已打通：raise → LP，两阶段顺序、负向用例、FORCE_UNWIND 全覆盖 |
| **Z2** | 编译器为**非可挂起函数**里的「catch-all 单层 try」发射 `.cfi_personality`/`.cfi_lsda`/`.gcc_except_table`；`--zero-cost-exc` 开关选择新路径 | 该用例输出与 setjmp 路径一致 |
| **Z3** | **类型过滤放进 personality 的 phase 1**（见下），landing pad 只做分派 | 全部 177 个 POSIX 用例通过 |
| **Z4** | 覆盖嵌套 try / finally / 异常对象 traceback | 异常相关的 30+ 用例全绿 |
| **Z5** | 移除非可挂起函数的 setjmp 路径，删掉对应的 HandlerPush/Setjmp/HandlerLeave | 那些函数的汇编里不再出现 `_setjmp` |

### Z2 的三条范围收紧（都是实测结论，不是保守）

1. **只迁非可挂起函数**（函数体里没有 `yield`/`await`）。生成器 / 协程 / 异步生成器里有 **5 处 C 级 setjmp 兜底**
   （`rt_gen.c` / `rt_async.c` / `rt_agen.c`），它们**抓不到 `_Unwind` 抛出的异常**。
   让它们永久保留 setjmp 是**允许且推荐的降级**，不是过渡状态。想统一得先给 resume 帧保存
   `_Unwind_Context` 快照，那是另一个量级的工程。
2. **`with` 不要一起迁**：`with` 可能**抑制**异常（`__exit__` 返回 True），不是纯 cleanup，
   语义上必须留在 setjmp 路径里，等 Z4 单独处理。
3. **catch-all 单层 try 先行**：类型分派还没进 personality 之前，只有 catch-all 能保证「内层抓到就是对的」。

### Z3 必须改设计：类型过滤在 phase 1，不在 landing pad

早期方案是「landing pad 统一跳到现有的 except 分发块，由 `pa_exc_matches` 分派」。
**这个方案在多层 try 上不成立**：展开器在 phase 1 只搜索一次 handler；一旦把上下文装进内层 landing pad，
若发现类型不匹配再 `_Unwind_Resume`，展开器只会继续 cleanup 阶段，**不会重新选外层 handler**，
一路展开到底 → terminate。

所以类型判定必须在 phase 1、在 personality 里完成。做法（personality 是我们自己的，不受 gcc 编码约束）：

- **保持形态 A 的结构**：`ttype_encoding = 0x1b`（pcrel|sdata4，不需要 indirect 槽），
  TType 表项写成 `.long <我们的 handler 描述符> - .`，不再是真正的 `type_info` 指针；
- action 记录按 ABI 用法写 `(filter = i, next = 0)`，`i` 是 1 起的 TType 表下标；
- 描述符形如 `{ uint32_t kind; uint32_t n; const char* names[] }`，`kind = 0` 表示 catch-all；
- personality phase 1：`filter > 0` → 取第 `filter` 项 → 调运行时 `pa_eh_handler_matches(exc, desc)`
  （内部复用 `pa_exc_matches` 的异常类层级判定）→ 匹配才 `_URC_HANDLER_FOUND`，否则 `_URC_CONTINUE_UNWIND`；
  `filter == 0` 是 cleanup 帧，phase 1 一律 CONTINUE；
- 这样**结构上仍是合法 LSDA**，只是 TType 表的内容是我们的扩展（标准 personality 会把它当
  `type_info*` 解，所以不能互通 —— 这是已知且可接受的代价）。

personality 里读 filter 必须是**通用的**，不要假设它一定是 0 或 1。

**回退方案 B**（若上面的复杂度超预期）：landing pad 里分发，不匹配就**重新 `_Unwind_RaiseException` 同一个对象**。
代价是每层一次完整搜索，且同一函数内多级 try 的边界要逐个验证。不到万不得已不走这条。

## 已知难点

1. **生成器/协程的挂起恢复**：现有实现用 `pa_handler_suspend` 把 handler 记录从全局栈上摘下来、
   恢复时重新 `setjmp`。表驱动方案里没有「handler 栈」，挂起时展开器上下文会失效。
   **决定：生成器/协程永久保留 setjmp 路径**（5 处 C 级兜底抓不到 `_Unwind`），不在本项目范围内统一。
2. **Win64 目标**：COFF 没有 `.gcc_except_table`，Windows 用 SEH（`.pdata`/`.xdata` + `.seh_handler`）。
   两个目标的异常机制完全不同，TargetABI 需要再加一层「异常模型」的抽象。**不建议两条一起做。**
3. **`finally` 与 `with`**：它们不是 catch，是 cleanup，需要 `_UA_CLEANUP_PHASE` 里逐帧执行代码
   （两步展开）；`with` 还可能抑制异常，必须留在 setjmp 路径单独处理。cleanup 帧在 call-site 表里是 action=0。

## 建议

Z1 已完成（`runtime/rt_personality.c` + `tests/exc/z1_rt/`）。下一步 Z2 只做最小闭环：
**非可挂起函数 + catch-all 单层 try + `--zero-cost-exc` 开关**，与 setjmp 路径并存，靠开关做 A/B。

发射任何 LSDA 之前先用 `tools/lsda_ref/` 对一遍字节 —— 那是唯一有实测背书的口径。

不要和 Win64 的 SEH 一起做。