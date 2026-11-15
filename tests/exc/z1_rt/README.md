# z1_rt —— 手写汇编 + personality 打通 _Unwind_RaiseException → landing pad

这是 [docs/zero-cost-exceptions.md](../../../docs/zero-cost-exceptions.md) 里 **Z1** 的落地验证：
完全不碰编译器，用「手写的汇编函数 + 我们自己写的 personality routine + 我们自己写的 LSDA」
证明 libgcc 的 `_Unwind_RaiseException` 能按表把控制跳到 landing pad，并且两阶段展开里
cleanup 帧和 handler 帧的行为符合 Itanium C++ ABI 的约定。

## 怎么跑

```bash
bash tests/exc/z1_rt/run.sh
```

- 依赖：Linux x86-64、gcc（实测 14.2.0）、binutils（readelf）、libgcc_s；脚本用 gcc 直接编译，
  **不碰仓库的 CMake/ninja 构建**，构建产物放 mktemp 临时目录，退出即删。
- 预期：`-no-pie` 和 PIE 两套链接各 43 项断言全部通过，脚本退出码 0：
  `==== 结果：全部 PASS（两套链接共 86 项断言）====`
- 脚本里同时检查了产物结构：z1_step2.o 必须有 `.gcc_except_table`、CIE augmentation 必须是
  `zPLR`（P/L/R 三个字段都在），z1_step1.o 必须是 `zPR`（只有 personality，没有 LSDA）。

## 产物

| 文件 | 作用 |
|---|---|
| `../../../runtime/rt_personality.c` | 通用 personality routine（**本次唯一的 runtime 改动**）：通用解码 LSDA，不做类型匹配 |
| `z1_step1.S` + `stub_personality.c` | 第一步：**不带 LSDA** 的 plumbing。stub personality 硬编码 landing pad 地址，只靠 IP 区间认领自己的帧 |
| `z1_step2.S` | 第二步：`.cfi_personality 0x1b` + `.cfi_lsda 0x1b` + 手写 `.gcc_except_table`，交给真 personality 解析 |
| `z1_step3.S` | 第三步：三层帧（内层无 landing pad / 中间 cleanup / 外层 catch-all），验证两阶段顺序与 `_Unwind_Resume` |
| `z1_broken_lsda.S` | 负向：LSDA 里只改坏 1 个 bit，验证 personality 真的在解析表并报对应错误 |
| `driver.c` | 构造 `struct _Unwind_Exception`、调 `_Unwind_RaiseException`、断言 landing pad 的结果；同时提供 `z1_raise_c` / `z1_note` / 探测入口等 C 侧 helper |
| `run.sh` | 一键编译 + 链接 + 运行 + 断言，PASS/FAIL 打印，退出码 0/1 |

## 三步各验证了什么

**第一步（无 LSDA）**：给汇编函数挂 `.cfi_personality 0x1b, z1_stub_personality`，
personality 不解析任何表，靠 `_Unwind_GetIP` 落在 `[函数起点, 函数末尾)` 判断"这是我的帧"。
断言：personality 在两阶段都被调用（SEARCH 一次、CLEANUP|HANDLER_FRAME 一次）、返回
`_URC_INSTALL_CONTEXT` 后 `_Unwind_SetIP` 真的把控制跳到硬编码的 landing pad、landing pad 里
`%rax`（GR0）等于 `_Unwind_SetGR` 传进来的 exception 指针。

**第二步（真 LSDA）**：`.cfi_lsda 0x1b, .LLSDA_step2` + 手写 `.gcc_except_table`。
保护区里先跑一次 `_Unwind_Backtrace`、一次 `_Unwind_ForcedUnwind`，最后真正抛出。
断言：personality 按 call-site 表找到 landing pad（地址/区间全部来自表格，硬编码不可能过）、
返回 7、GR0 是 exception 指针、解码错误码为 0；`_Unwind_Backtrace` 能穿过本帧（7 帧）；
`_Unwind_ForcedUnwind` 期间 personality 走 FORCE_UNWIND 分支 ≥1 次且 `_Unwind_SetIP` **0 次**。

**第三步（两阶段行为）**：栈由内到外是
`z1_step3_nopad_fn`（挂 personality+LSDA，但保护区条目的 landing pad=0）
→ `z1_step3_cleanup_fn`（LSDA 条目 action 下标=0，纯清理，LP 叫 B）
→ `z1_step3_handler_fn`（action 下标=1，catch-all，LP 叫 A）。
断言：phase 1 只由外层认领 handler；phase 2 先给中间 cleanup 帧安装上下文 → **B 先跑**，
B 里改全局状态后调 `_Unwind_Resume` 继续展开 → **A 后跑**；两层各跑一次、顺序正确；
没有 landing pad 的中间帧被静默穿过；两个 LP 都拿到同一个 exception 指针。

**负向**：`z1_broken_lsda.S` 和第二步的差别只有 LSDA 头的 TType encoding 字节：
`0xff` → `0xfe`（1 个 bit）。断言：`_Unwind_RaiseException` 返回 `_URC_FATAL_PHASE1_ERROR`、
`pa_personality_last_error == PA_LSDA_ERR_ENCODING`、landing pad 完全没执行。
另外 driver 里有一个反例断言：栈上没有任何认领者时，`_Unwind_RaiseException` 返回
`_URC_END_OF_STACK`（不是崩、不是 0），并且 phase 2 根本不会开始。

## 实测出来的 LSDA 字节（给 Z2 抄）

三份 LSDA 都是「LPStart 省略、TType 省略、call-site 用 uleb128」，全部无重定位，
PIE / -no-pie 通用。字段：LPStart enc | TType enc | call-site enc | call-site 表长 |
若干条目(start, len, lp, action) | action 表 | 对齐填充。

```
z1_step2.S（tests/exc/z1_rt/z1_step2.S 的 .LLSDA_step2）：
  ff ff 01 04  08 17 26 01  00 00 00 00
  LPStart omit | TType omit | call-site enc=01(uleb128) | 表长=4
  条目: start=8 len=0x17 lp=0x26 action=1 | action 表 = 00 00 | 对齐填充

z1_step3.S 三个 LSDA：
  handler: ff ff 01 04  08 09 18 01  00 00 00 00
  cleanup: ff ff 01 04  08 09 18 00  00 00 00      ← action 下标 0 = 纯清理
  nopad  : ff ff 01 04  08 09 00 00                ← lp = 0 = 这一段没有 landing pad

z1_broken_lsda.S（负向，只改 1 bit）：
  ff fe 01 04  08 09 18 01  00 00 00 00            ← 第 2 字节 0xff→0xfe，非法编码
```

- 偏移基准：LPStart 省略时 = FDE 的 region start = 函数起点；`start/len/lp` 都是相对 LPStart 的 uleb128；
  `action` 恒为 uleb128。
- **cleanup 与 handler 的判据是 call-site 第 4 个字段（action 下标）是否 0**：
  0 = 只做清理（phase 1 不认领，phase 2 安装上下文）；非 0 = 指向 action 表的记录，是 handler。
  action 表那条记录才是两个 SLEB128：`(type filter, next offset)`。
  GCC 14 对 `catch (...)` 实测是 `filter=1 + TType 表里一个 null type_info 项`（见下），
  而 ABI 也允许 `filter=0` 表示 catch-all；**我们的 personality 故意不读 action 表/filter**，
  所以两种写法都兼容（Z3 做类型过滤时再读，扩展点见 rt_personality.c 里 `Z3 扩展点` 注释）。
- **TType 表的 offset 语义**（GCC 14 实测，容易踩）：它是 uleb128，含义是「从该 uleb 字段之后的位置
  到 TType 表**末尾**的偏移」；`filter=i` 的条目在 `TType - i * size_of_encoded_value`（反向下标，
  GCC 的 `get_ttype_entry` 就这么取）。GCC 非 PIE 用 TType enc `0x03`(udata4)，PIE 用 `0x9b`
  (indirect|pcrel|sdata4)；类型表里 null type_info 表示 catch-all。
- CIE 实测（本夹具）：
  `Augmentation: "zPLR"`，augmentation data `1b ed ff ff ff 1b 1b`
  = personality enc `0x1b` + 4 字节 pcrel 偏移 + LSDA enc `0x1b` + FDE enc `0x1b`。
  第一步没有 LSDA 时是 `"zPR"`。

## personality 契约要点（实测）

- phase 1 `_UA_SEARCH_PHASE`（actions=0x1）：每个挂了 personality 的帧都会被问一次。
  有 landing pad 且 action 下标非 0（handler）→ `_URC_HANDLER_FOUND`；否则 `_URC_CONTINUE_UNWIND`。
- phase 2 `_UA_CLEANUP_PHASE`（actions=0x2）：带 `_UA_HANDLER_FRAME` 时 actions=0x6。
  handler 帧：`_Unwind_SetGR(GR0=exc)` + `_Unwind_SetIP(lp)` → `_URC_INSTALL_CONTEXT`；
  纯清理帧（action=0，且没有 HANDLER_FRAME）：同样 `INSTALL_CONTEXT`，LP 里必须自己
  `_Unwind_Resume` 继续；外层 catch 帧（无 HANDLER_FRAME）：`CONTINUE_UNWIND`。
- phase 2 怎么判断"哪一帧是 handler 帧"（从 libgcc_s 反汇编实测）：phase 1 找到 handler 后，
  libgcc 把该帧的 `_Unwind_GetCFA(context)`（按 CFI 里 cfa 是否带标志位减 0/1）存进
  `exc->private_2`（并把 `private_1` 清零）；phase 2 每帧比较 `_Unwind_GetCFA(ctx) == exc->private_2`
  来决定要不要加 `_UA_HANDLER_FRAME`。**这意味着传给 `_Unwind_RaiseException` 的异常对象必须是
  真正那个对象且 private_1/private_2 抛前清零、中途不能被踩**（我们踩过一次，见坑 7）。
- GR0（x86-64 的 `%rax`）= exception 指针。GR1（`%rdx`）我们简化为 0；GCC 产物会在 landing pad 里
  用 `cmpq $1,%rdx` 之类做分派（Z2 由我们自己生成 LP，约定可自定，但别假设 GR1 有值）。
- `_UA_FORCE_UNWIND`（actions=0x8）时必须只 `CONTINUE_UNWIND`、**不得** SetIP/SetGR 安装上下文，
  否则运行时 traceback/forced unwind 会崩。实测 `_Unwind_Backtrace` 本身**不调用** personality
  （只走 CFI），真正带 FORCE_UNWIND 调 personality 的是 `_Unwind_ForcedUnwind`。

## 坑清单（每条都有实测现象 / 原因 / 规避）

### 坑 1：`.cfi_personality` 的编码语义与"直连 vs DW.ref"
- `0x1b` = `DW_EH_PE_pcrel | DW_EH_PE_sdata4`（**sdata4**，不是 udata4=0x03）。
- 同链接单元（我们和 `pa_personality` 编进同一个可执行文件）用 `0x1b` 直连完全可以：
  `-no-pie` 和 `-pie` 都实测链接 + 运行通过（本仓库 run.sh 两套都跑）。
- 但 `0x1b` 直连共享库符号（例如 `__gxx_personality_v0`）在 PIE 下会被 ld 拒绝，原文：
  `relocation R_X86_64_PC32 against symbol '__gxx_personality_v0@@CXXABI_1.3' can not be used when making a PIE object; recompile with -fPIE`。
  真接 DSO 时要按 GCC 的写法用 `0x9b`（indirect）+ `DW.ref.<符号>` 间接层。
  实测 `0x9b + DW.ref` 与 `0x1b` 直连在本夹具上等价（28 项断言全过）。
- `.cfi_lsda 0x1b, <本地 .gcc_except_table 标签>` 在 PIE/-no-pie 下都正常（pcrel 由链接器解析），
  我们的 LSDA 内容本身不含绝对地址（全 uleb 相对偏移），所以两种模式字节一样、都能跑。

### 坑 2：`0x9b` 少了 `DW.ref.` —— 汇编器/链接器沉默，运行期 SIGSEGV
把坑 1 的 `0x9b` 直接写成普通函数符号（`.cfi_personality 0x9b, pa_personality`）：
GAS 不报错、链接（符号存在时）也不报错，但 CIE 里 `0x9b` 的语义是"字段值是间接层的地址"，
展开器会把函数代码的头 8 个字节当地址读出来跳过去 → **运行期 SIGSEGV（139），没有任何输出**。
这是典型的"汇编器沉默、运行期才炸"，想省 `DW.ref` 请用 `0x1b`，不要用 `0x9b` 直连函数符号。

### 坑 3：只挂 `.cfi_lsda`、忘了 `.cfi_personality` —— GAS 静默接受，handler 永远没人认领
实测 GAS 不报错、不警告，产物 CIE augmentation 变成 `"zLR"`（没有 P）。运行时展开器只会读 LSDA
但没有人认领 handler，`_Unwind_RaiseException` 一路走到栈顶返回 `_URC_END_OF_STACK`，
landing pad 一行都不会执行。排查口诀：`readelf --debug-dump=frames` 看 CIE 是 `zPR`（无 LSDA）、
`zPLR`（有 personality + LSDA）还是 `zLR`（少了 personality，必错）。

### 坑 4：帧没有 `.eh_frame`（漏 `-funwind-tables`）—— 展开在抛点就断
汇编夹具和 C helper 都要有 CFI。实测把 driver.c 用
`-fno-asynchronous-unwind-tables -fno-unwind-tables` 编译（抛点 `z1_raise_c` 就在 driver.c 里）：
所有"抛出并被 LP 接住"的用例全部失败（返回 0、LP 没执行），因为展开器在抛点这一帧就找不到 FDE，
phase 1 直接 `_URC_END_OF_STACK`，汇编帧上的 personality 根本没被调用。
规避：driver/helper 一律 `-funwind-tables`（本项目 x86-64 上默认已开异步 unwind table，但显式写死）。

### 坑 5：landing pad 入口的 `%rsp` 是"抛出点 call 之前的值"，不是 CFA
- 现象：如果按"展开器已经把 CFA 恢复到 rsp"的直觉写 LP 返回路径，进程**死循环**——
  LP 的 `ret` 又跳回 LP 自身（本仓库最初版本就是这样；gdb 里 LP 第一个断点反复命中，
  `timeout` 杀进程返回 137）。
- 正确规则：**LP 入口 rsp = 抛出点那条 `call` 执行前的 rsp（也就是函数体正常路径的 rsp）**。
  所以 LP 的返回路径可以照抄正常 epilogue（例如 prologue `subq $8` → LP 里 `addq $8,%rsp; ret`）；
  被调用者保存寄存器也恢复成调用点的值；返回地址仍在 `CFA-8`。
- 怎么量出来的（不依赖夹具布局）：
  1. gdb 断在 LP：`subq $8` 的帧里 LP 入口 `rsp = 函数入口 rsp - 8`，`[rsp+8]` 是返回地址；
  2. 更有说服力的是 GCC 自己的产物：`-O1` 编一个 `subq $72` 的大帧函数（reference 探针），
     它的 landing pad 直接 `jmp` 正常 epilogue 的 `addq $72,%rsp; ret`——只有 LP 入口 rsp 等于
     调用点 rsp（`E-72`，而不是 `CFA-16 = E-8`）才可能算对；
  3. 我们把自己的第二步夹具 prologue 从 `subq $8` 临时改成 `subq $24`（LP 相应 `addq $24`），
     43 项断言同样全过——规则与帧大小无关（变体只用于验证，未入库）。
- 推论：Z2 生成 LP 时，LP 里"恢复栈"这件事 = 生成和正常返回路径完全一样的 epilogue；
  不需要额外的 CFA 修正。注意 LP 区段的 `.cfi_def_cfa*` 要按"调用点状态"发射（见坑 6）。

### 坑 6：landing pad 必须在 FDE 内，且 LP 区段 CFI 要描述 LP 入口状态 —— 否则从 LP 里 `_Unwind_Resume` 直接 SIGABRT 且无输出
- 现象：把 cleanup 帧的 landing pad 放到 `.cfi_endproc` 之后（LP 没有 FDE 覆盖），
  LP 里调 `_Unwind_Resume` 时进程 `SIGABRT`（退出码 **134**），**stderr 一个字都没有**；
  gdb 回溯：`abort ← libgcc_s.so.1 ← <landing pad 所在函数>`。
- 原因：`_Unwind_Resume` 从 LP 的返回地址重新 `uw_frame_state_for` 找 FDE，找不到就 abort。
- 规避：LP 留在 `.cfi_startproc/.cfi_endproc` 范围内；LP 区段按"进入 LP 时的真实状态"补 CFI。
  我们的每一步都这样写：LP 标签后补 `.cfi_def_cfa_offset <call 点的 CFA 规则>`；LP 里如果临时
  `subq` 出栈槽（例如给 helper 调用保存 exc），要成对补 `.cfi_def_cfa_offset` 调整。
- 复现方式：把 `z1_step3.S` 里 cleanup 的 LP 整段挪到 `.cfi_endproc` 之后（并去掉 LP 内的
  `.cfi_*` 指令，否则 GAS 报 "CFI instruction used without previous .cfi_startproc"），重跑即可。

### 坑 7：异常对象本身会被 libgcc 写 —— `%rdi` 被中间调用踩掉会表现为"phase 2 找不到 handler 帧"
我们踩过的真实 bug：第二步夹具为了加 traceback 探针，在保护区里先 `call z1_trace_probe`，
再 `call z1_raise_c`；但 `z1_raise_c` 的 exc 参数是入口时的 `%rdi`，被探针调用 clobber 了，
于是 `_Unwind_RaiseException` 收到的是栈上垃圾指针。现象非常迷惑：
- phase 1 正常找到 handler（`_URC_HANDLER_FOUND`），phase 2 却给同一帧 actions=0x2（没有
  `_UA_HANDLER_FRAME`），最后 `_Unwind_RaiseException` 返回 `_URC_FATAL_PHASE2_ERROR`(2)；
- 因为 libgcc 把 handler 帧 CFA 写进的是那个"垃圾 exception 对象"的 `private_2`，
  而 phase 2 的栈活动又把垃圾对象覆盖了，比较自然失败。
规避：汇编里把 `%rdi`（或任何跨 call 存活的值）存进自己的栈槽，每次调用前重装；
异常对象必须 16 字节对齐、`private_1/private_2` 抛前置 0、全程只用一个真实对象。
定位技巧：`PA_PERSONALITY_TRACE` + `Z1_TRACE=1` 打印每帧 `actions/ip/pc/cfa`，
并在日志里看 `exc` 指针是否等于自己传进去的地址（我们就是靠这个一眼看出来的）。

### 坑 8：personality 必须非 static 且 `__attribute__((used))`
`.cfi_personality` 只在 `.eh_frame` 里留符号引用，C 代码里没人调用它。如果写成 static，
`-O2` 会把它当死代码删掉，链接期报：
`/usr/bin/ld: persref.o:(.eh_frame+0x12): undefined reference to 'hidden_personality'`。
`pa_personality` 是非 static + `used`，stub 也一样。

### 坑 9：LSDA 的对齐与节属性
`.gcc_except_table` 里每个 LSDA 前 `.align 4`；节声明 `.section .gcc_except_table,"a",@progbits`。
TType 表（如果发射）本身也要求编码宽度对齐（GCC 用 `.align 4` + `.long`）。
我们的 LSDA 全用 uleb 相对偏移，所以没有任何重定位，PIE 下也不需要额外处理。

### 坑 10：personality 里 PC 语义 —— 拿到的是 call 之后的返回地址，要 -1
展开器给的 IP 是"抛出点 call 之后的返回地址"。GCC `__gxx_personality` 用
`_Unwind_GetIPInfo(ctx,&before)`，`before==0` 时把 IP 减 1 再查 call-site 表；不减的话，
当抛出的 call 是保护区最后一条指令（返回地址正好落在区间末尾、半开区间取不到）时会查不到。
我们的 `pa_personality` 同样处理。

### 坑 11：汇编约定
- `.cfi_startproc/.cfi_endproc` 必须包住整个函数（含 LP）；`.cfi_endproc` 后再写 `.cfi_*` 会报
  `CFI instruction used without previous .cfi_startproc`。
- 帧里用 `%rsp` 相对 CFA（`.cfi_def_cfa_offset`）就够，不必强上 `%rbp`；没动过被调用者保存寄存器
  就不需要 `.cfi_offset`。
- 建议加 `.section .note.GNU-stack,"",@progbits`（GNU 工具链惯例，防止可执行栈标记）。
  实测本环境 binutils 2.44 漏掉它**不报警告**，别指望链接器提醒你。
- LP 里调用 helper 前注意 16 字节栈对齐：LP 入口 rsp 是调用点 rsp（调用点上 `rsp % 16 == 0`），
  直接 `call` 就行；如果 LP 里又 `subq` 了非 16 倍数，记得补到 16 对齐。

### 坑 12：`_Unwind_Backtrace` 不调用 personality，别拿它测 FORCE_UNWIND
实测：`_Unwind_Backtrace` 只按 CFI 走帧，personality 一次都不会被调（我们最初加了个
"backtrace 期间 personality 走 FORCE_UNWIND"的断言，计数是 0，才发现这一点）。
真正会带 `_UA_FORCE_UNWIND` 调 personality 的是 `_Unwind_ForcedUnwind`；本仓库第二步
用它在保护区里触发强制展开，断言 personality 走了 FORCE_UNWIND 分支、且 `_Unwind_SetIP` 0 次。

### 坑 13：PIE / -no-pie 要整套一致
driver/夹具用 `-fno-pie` 就配 `-no-pie` 链接、用 `-fPIE` 就配 `-pie`；混用会出现
"relocation ... can not be used when making a PIE object" 之类的链接错误。
本仓库 run.sh 两套都跑（两套各 43 项断言全过），说明我们的 LSDA/CFI 写法与 PIE 无关。

## 已知局限（Z1 刻意不做，Z2/Z3 再补）

1. **不做类型匹配**：`action != 0` 一律当 handler（catch-all），TType 表整个省略。
   Z3 要做 phase 1 类型过滤时，在 `pa_find_landing_pad` 里从 action 表读 filter/next 即可
   （扩展点已在 `rt_personality.c` 注释里标出；不要假设 filter 一定是 0 或 1）。
2. action 表不解析、不校验；call-site 表也只做了"最长 4096 项 / LEB128 ≤10 字节"的合理性上限，
   因为 `_Unwind_GetLanguageSpecificData` 不给我们 LSDA 长度，做不了真正的边界检查。
3. 编码只支持 absptr/uleb128/udata2/udata4/udata8/sleb128/sdata2/sdata4/sdata8 的
   absptr/pcrel/funcrel/aligned/indirect 组合；textrel/datarel 需要段基址，明确返回
   `PA_LSDA_ERR_ENCODING`（当前 `.gcc_except_table` 产物用不到）。
4. 异常类/foreign exception、exception spec、`_Unwind_Resume_or_Rethrow`、多线程/异步异常、
   生成器/协程挂起（见设计文档"已知难点"）都不在 Z1 范围。
5. LP 里 GR1（`%rdx`）简化为 0；GCC 的 landing pad 用 GR1 做分派，Z2 生成 LP 时自定义约定即可。

## 给 Z2 的 codegen 约束（从本夹具直接抄）

1. 函数级：`.cfi_personality 0x1b, pa_personality`（同一可执行文件；将来进 DSO 换 `0x9b + DW.ref`）
   + `.cfi_lsda 0x1b, .LLSDA_xxx`。
2. 保护区用一对标签界定在 `call` 前后；call-site 条目写 `start/len/lp/action` 四个 uleb128，
   action 下标 0 = 纯清理、非 0 = handler；LPStart/TType 编码用 0xff 省略（catch-all 基线）。
3. LP 代码的栈恢复 = 正常 epilogue（LP 入口 rsp 就是抛出点 call 前的 rsp），并在 LP 区段发射
   与"调用点状态"一致的 CFI（`.cfi_def_cfa_offset` 等）。
4. LP 里先按现有语义做类型匹配/清理：handler 帧直接跑 except 体；cleanup 帧必须 `_Unwind_Resume`。
5. 抛点函数本身也要有完整 `.eh_frame`（`-funwind-tables` 已具备）。
6. exception 对象：16 字节对齐、`_Unwind_Exception` 32 字节 + payload 跟在后面、
   `private_1/private_2` 抛前置 0、只用一个真实对象。
