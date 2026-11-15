# z1_rt —— 手写汇编 + personality 打通 _Unwind_RaiseException → landing pad（Z1.5）

这是 [docs/zero-cost-exceptions.md](../../../docs/zero-cost-exceptions.md) 里 **Z1** 的落地验证，
Z1.5 起升级为「ABI 语义 + GCC 同款 LSDA」：

- 完全不碰编译器，用「手写的汇编函数 + 我们自己写的 personality routine + 我们自己写的 LSDA」
  证明 libgcc 的 `_Unwind_RaiseException` 能按表把控制跳到 landing pad；
- handler / cleanup 的判据按 **Itanium ABI**（action 记录的 filter），不是私有约定；
- LSDA 与 gcc 14 的 catch-all 同构（`ttype_encoding=0x9b` + TType 表一个 **4 字节 0、无重定位**
  的 null 项 + action 记录 `(filter=1, next=0)`）；
- 用**真的 libstdc++ `__gxx_personality_v0`** 交叉验证同一个 handler 夹具确实被当作 catch-all。

## 怎么跑

```bash
bash tests/exc/z1_rt/run.sh
```

- 依赖：Linux x86-64、gcc/g++（实测 14.2.0）、binutils（readelf）、libstdc++/libgcc_s；
  脚本用 gcc/g++ 直接编译，**不碰仓库的 CMake/ninja 构建**，构建产物放 mktemp 临时目录，退出即删。
- 预期：`-no-pie` 和 PIE 两套链接各 **58 项断言**全部通过（共 116 项），
  外加两套链接各一次「真 `__gxx_personality_v0`」交叉验证，脚本退出码 0：
  `==== 结果：全部 PASS（两套链接共 116 项断言）====`
- 脚本里同时检查产物结构：
  - z1_step2.o / z1_step3.o 的 LSDA 头是 `ff 9b ...`（GCC 同款）；
  - `.gcc_except_table` 内部**没有重定位**（catch-all 的 null TType 项必须是 4 字节 0 + 无重定位）；
  - z1_step2.o 的 CIE personality 编码是 `0x9b`（间接），z1_step1.o 是 `"zPR"` + `0x1b`（直连、无 LSDA）；
  - 交叉验证程序输出 `[crosscheck] caught=0 lp_calls=1 fn_ret=7 z1_lp_seen=1`。

## 产物

| 文件 | 作用 |
|---|---|
| `../../../runtime/rt_personality.c` | 通用 personality routine（**本次唯一的 runtime 改动**）：通用解码 LSDA + action 记录，按 ABI 语义判 handler/cleanup/spec |
| `z1_step1.S` + `stub_personality.c` | 第一步：**不带 LSDA** 的 plumbing。stub personality 硬编码 landing pad 地址，`.cfi_personality 0x1b` 直连（保留 0x1b 路径的活证据） |
| `z1_step2.S` | 第二步：`.cfi_personality 0x9b` + `.cfi_lsda 0x1b` + **GCC 同款 20 字节 LSDA**；同一份 .S 可编出交叉验证变体 |
| `z1_step3.S` | 第三步：三层帧（内层无 landing pad / 中间 cleanup / 外层 catch-all），验证两阶段顺序与 `_Unwind_Resume` |
| `z1_broken_lsda.S` | 负向 A：LSDA 编码非法（TType enc 0x9b → 0xfe） |
| `z1_broken_table.S` | 负向 B：call-site 表越界（udata8 编码 + 8 字节表长） |
| `z1_spec_lsda.S` | filter<0（exception spec）的处理选择：CONTINUE_UNWIND + 诊断计数 |
| `driver.c` | 构造 `struct _Unwind_Exception`、调 `_Unwind_RaiseException`、断言结果；提供 personality 槽、`z1_raise_c`、`z1_note`、traceback/forced-unwind 探针 |
| `crosscheck_gxx.cpp` | 交叉验证驱动：真 C++ `throw 7` + 外层 `catch(int)`，personality 槽换真 `__gxx_personality_v0` |
| `run.sh` | 一键编译 + 链接 + 运行 + 断言（PIE / -no-pie / 交叉验证），PASS/FAIL 打印，退出码 0/1 |

## 验证内容

**第一步（无 LSDA，0x1b 直连）**：给汇编函数挂 `.cfi_personality 0x1b, z1_stub_personality`，
personality 不解析任何表，靠 `_Unwind_GetIP` 落在 `[函数起点, 函数末尾)` 认领自己的帧。
断言：两阶段都被调用（SEARCH 一次、CLEANUP|HANDLER_FRAME 一次）、`_Unwind_SetIP` 生效、
landing pad 里 `%rax`（GR0）= exception 指针。此用例保留，作为「0x1b 直连同链接单元符号可用」的活证据。

**第二步（GCC 同款 LSDA，0x9b 间接 personality）**：`.cfi_lsda 0x1b, .LLSDA_step2` + 20 字节 LSDA。
保护区内先跑一次 `_Unwind_Backtrace`（只走 CFI，实测**不调用** personality）、
一次 `_Unwind_ForcedUnwind`（带 `_UA_FORCE_UNWIND` 调 personality），最后真正抛出。
断言：personality 用 action filter 判定 catch-all + 安装 landing pad（地址/区间全来自表）、
返回 7、GR0 是 exception 指针、解码错误码 0；FORCE_UNWIND 路径被走到且 `_Unwind_SetIP` 0 次；
action filter>0 + null TType 项被判为 catch-all（计数 ≥2：phase1 + phase2 各解析一次）；
typed catch 计数 0（本夹具没有 typed catch）；cleanup 计数 0（action 下标 1 没被误判 cleanup）。

**负向 A（编码非法）**：LSDA 的 TType encoding 从 `0x9b` 改成 `0xfe`（1 个字节）。
`0xfe` 的低 4 位不是合法 DW_EH_PE 格式，且按宽松解析会让 TType 偏移字段「重新出现」、
把后面 call-site 编码字节吃掉造成错位/越界。断言：`_URC_FATAL_PHASE1_ERROR`、
错误类别是「编码非法 / 表越界」（本解码器报 `PA_LSDA_ERR_ENCODING`）、landing pad 不执行。

**负向 B（表越界）**：LSDA 头合法（call-site 编码 `0x04`=udata8、表长 8 字节），
但 udata8 下每条 call-site 至少 25 字节 > 8 字节。断言：`_URC_FATAL_PHASE1_ERROR`、
错误码 `PA_LSDA_ERR_TRUNCATED`（「表越界」），不越读崩溃。

**filter<0（exception spec）**：见下文「filter<0 的处理」。断言：不被当 handler、
继续展开 → `_URC_END_OF_STACK`、landing pad 不执行、不是解码错误、`filter<0` 诊断计数 +1。

**第三步（两阶段行为）**：栈由内到外 `nopad`（LSDA 有表但 lp=0）→ `cleanup`（action 下标 0）
→ `handler`（filter=1 + null TType 项）。断言：phase 1 只由外层认领 handler；phase 2 先给
cleanup 帧安装上下文 → **B 先跑**，B 改状态后 `_Unwind_Resume` → **A 后跑**；各一次、顺序正确；
无 landing pad 的中间帧被静默穿过；两个 LP 都拿到同一个 exception 指针；
cleanup 条目判定计数 ≥2、catch-all 计数 ≥2、filter<0 计数 0。

**交叉验证（真 `__gxx_personality_v0`）**：`z1_step2.S` 用 `-Wa,--defsym,Z1_CROSSCHECK=1`
编出「保护区内只有一条 C++ `throw 7`」的变体（LSDA 结构与主夹具同形，20 字节、`ff 9b` 开头），
personality 槽在运行期填 `__gxx_personality_v0`，外层是真正的 C++ `try { ... } catch (int)`。
断言输出 `[crosscheck] caught=0 lp_calls=1 fn_ret=7 z1_lp_seen=1`：
**夹具帧被 gcc personality 当作 catch-all，异常被吃掉，landing pad 恰好跑一次，程序正常退出。**
（旧形态 filter=0 时，同一测试会得到 `caught=7, lp_calls=1`：被当 cleanup，异常继续往外走。）

## 实测出来的 LSDA 字节（Z1.5，GCC 同款）

主夹具 `z1_step2.o`（20 字节，与 gcc -O1 catch-all 用例同形；偏移值随代码布局不同）：

```
ff 9b 11 01 08 08 17 26 01 3f 05 00 00 01 00 00 00 00 00 00
  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  +--+--+--+-- TType[1] = 4 字节 0（无重定位）→ null type_info* = catch(...)
  |  |  |  |  |  |  |  |  |  |  |  |  |  +--+-- action[0] = (filter=1, next=0) 两个 SLEB128
  |  |  |  |  |  |  |  |  |  |  |  +--+--+--+ 条目 2：LP 内 call 的保护区，lp=0、action=0
  |  |  |  |  |  |  |  |  |  +--+--+--+ 条目 1：被保护 call → LP，action 下标 1
  |  |  |  |  |  |  |  +--+ call-site 表字节长度 = 8（2 条）
  |  |  |  |  |  |  +-- call-site 编码 = 0x01 (uleb128)
  |  |  |  |  +--+-- TType 表偏移 = 0x11（从该 uleb 之后到表**末尾**）
  |  |  |  +-- TType 编码 = 0x9b = indirect|pcrel|sdata4
  |  |  +-- LPStart 编码 = 0xff（省略 → 用 FDE region start）
  |  +--（同上）
  +-- ...
```

其他夹具（实际 readelf -x 结果）：

```
z1_step2 交叉验证变体（同一 .S + --defsym，20 字节）：
  ff 9b 11 01 08 08 05 14 01 2d 05 00 00 01 00 00 00 00 00 00
z1_step3 handler（20 字节，GCC catch-all 同形）：
  ff 9b 11 01 08 08 09 18 01 20 05 00 00 01 00 00 00 00 00 00
z1_step3 cleanup（12 字节，GCC cleanup 同形，ttype 省略、2 条 call-site）：
  ff ff 01 08 08 09 18 00 28 12 00 00
z1_step3 nopad（8 字节，lp=0 的保护区）：
  ff ff 01 04 08 09 00 00
z1_broken_lsda（负向 A，TType 编码 0xfe；16 字节）：
  ff fe 0d 01 04 08 09 18 01 01 00 00 00 00 00 00
z1_broken_table（负向 B）：ff ff 04 08 + 32 字节 0（表长 8 但 udata8 条目 ≥25 字节）
z1_spec_lsda（filter=-1；12 字节）：
  ff ff 01 04 08 09 18 01 7f 00 00 00
```

- 偏移基准：LPStart 省略时 = FDE 的 region start（函数起点）；`start/len/lp` 相对 LPStart 的 uleb128；
  `action` 恒为 uleb128。
- **action 表的记录是两个 SLEB128**：`filter` 和 `next`；`next!=0` 表示链下一条（相对记录末尾的字节偏移）。
  action 表紧跟在 call-site 表之后。
- **cleanup / handler 判据（ABI）**：call-site 的 `action 下标 == 0` → 纯 cleanup；
  `action 下标 != 0` → 读 action 记录：`filter==0` cleanup，`filter>0` catch handler，
  `filter<0` exception spec。**`filter>0` 的 catch 还要读 TType 表**：
  null type_info* = catch-all；非 null = typed catch（类型匹配由 personality 做）。
- **TType 表偏移语义**：uleb128，从该字段之后到 TType 表**末尾**的偏移；第 i 项在 `表尾 - i*size`
  （反向索引，GCC 的 `get_ttype_entry` 就这么取）。GCC 非 PIE 的 TType enc 是 `0x03`(udata4)，
  PIE 是 `0x9b`；本夹具统一用 `0x9b`（两种链接方式都验证过）。
- **null TType 项为什么是 4 字节 0 而不是「指针槽」**：libgcc `read_encoded_value_with_base` 对
  `result == 0` 有特判——不施加 pcrel/funcrel 相对基址、也不做 indirect 解引用，直接返回 0。
  所以 `0x9b` 编码下 4 字节 0 + 无重定位 = null type_info*（GCC 自身写法）。我们的解码器照此实现。
- CIE 实测：`Augmentation: "zPLR"`，augmentation data `9b <4 字节 pcrel> 1b 1b`。
  第一步（无 LSDA、0x1b 直连）是 `"zPR"`，aug data `1b <4 字节 pcrel> 1b`。

## personality 指针：0x9b + 运行期槽（与 gcc 的差异）

夹具的 CIE 用 `.cfi_personality 0x9b, z1_personality_slot`（0x9b = indirect|pcrel|sdata4，
**gcc 同款编码**），但 `z1_personality_slot` 是普通 8 字节数据符号，由 driver 在运行期填：

- 主测试：`z1_personality_slot = (void *)&pa_personality`；
- 交叉验证：`z1_personality_slot = (void *)&__gxx_personality_v0`。

好处是**同一个 .S 的同一套 LSDA 字节**可以分别交给两种 personality 解释，交叉验证不是在
「另一份复制夹具」上做的。与 gcc 的差异：gcc 在 `.data.rel.local` 里发静态 `DW.ref.<符号>` 槽
（`.quad 目标函数`，运行期不再改）。两种写法 CIE 字节层面的编码一模一样，只是槽的初始化方式不同；
PIE / -no-pie 都验证通过。注意 0x9b 的操作数必须是「存指针的 8 字节槽」，直接写函数符号会
汇编/链接通过、运行期 SIGSEGV（见坑 2）。

## ABI 语义要点（从 libsupc++ 源码 + 实测核对）

phase 1 `_UA_SEARCH_PHASE`（actions=0x1）/ phase 2 `_UA_CLEANUP_PHASE`（0x2，
带 handler 帧时 0x6）里，personality 对每个挂了 personality 的帧都调用。判定：

| call-site action 下标 | action 记录 | 结论 |
|---|---|---|
| 0 | （没有记录） | 纯 cleanup：phase1 CONTINUE，phase2 INSTALL_CONTEXT 跑 LP |
| !=0 | filter == 0 | cleanup（同上） |
| !=0 | filter > 0 + TType 项为 null | **catch-all handler**：phase1 HANDLER_FOUND，phase2 INSTALL_CONTEXT |
| !=0 | filter > 0 + TType 项非 null | typed catch（本实现**不做类型匹配**，一律当 handler，Z3 再补） |
| !=0 | filter < 0 | exception spec（本实现选择 CONTINUE_UNWIND + 诊断计数，见下） |

phase 2 里 `_UA_HANDLER_FRAME` 的判定（libgcc 反汇编实测）：phase 1 找到 handler 后把该帧的
`_Unwind_GetCFA(context)` 存进 `exc->private_2`（private_1 清零）；phase 2 每帧比较
`_Unwind_GetCFA(ctx) == exc->private_2` 决定是否加 `_UA_HANDLER_FRAME`。所以传给
`_Unwind_RaiseException` 的异常对象必须是真实那个、private 字段抛前置零、中途不能被踩。

GR0（x86-64 的 `%rax`）= exception 指针；GR1（`%rdx`）我们清 0（简化路径；gcc 的 landing pad
会用 GR1 做分派，Z2 生成 LP 时自定义约定即可）。landing pad 入口的 rsp = 抛出点那条 call
**之前**的 rsp（不是 CFA），所以 LP 的返回路径照抄正常 epilogue 即可。

## filter<0（exception spec）的处理选择

**选择：CONTINUE_UNWIND + `pa_dbg_spec` 诊断计数，不 terminate，也不安装 landing pad。**

理由：
1. Z2 的 codegen 不会发射负 filter（我们的 except 语义由 landing pad 里的分发块负责），
   所以正常路径上这条分支不可达；
2. 一旦真的遇到（比如误读或将来混入 C++ 产物），ABI 的正确行为是执行 spec 检查并可能
   调 `__cxa_call_unexpected`/terminate——我们既没有 spec 表也没有该协议，贸然 terminate
   会把用户进程直接 abort；选择「当作本帧无关、继续展开」是保守且可观测的（有诊断计数），
   同时不会把异常错误地吞掉；
3. 等 Z4 真做 exception spec 时，在 `pa_classify_action` 的 `filter < 0` 分支里实现
   「读 TType 表里的 spec 列表 + 匹配失败 → 交给 LP 调 __cxa_call_unexpected」即可；
   `z1_spec_lsda.S` 用例已经把「我们选择 CONTINUE」钉成回归断言。

## 交叉验证输出（run.sh 里真实输出）

```
[PASS] 交叉验证（-no-pie）：真 __gxx_personality_v0 把夹具帧当 catch-all（LP 1 次、异常被吃掉）
    [crosscheck] caught=0 lp_calls=1 fn_ret=7 z1_lp_seen=1
[PASS] 交叉验证（PIE）：真 __gxx_personality_v0 把夹具帧当 catch-all（LP 1 次、异常被吃掉）
    [crosscheck] caught=0 lp_calls=1 fn_ret=7 z1_lp_seen=1
```

`caught=0` = 外层 `catch (int)` 没拿到异常（被夹具帧的 catch-all 吃掉了）；
`lp_calls=1` = 夹具 landing pad 恰好跑一次；`fn_ret=7` = 夹具 handler 正常返回；
如果是旧形态（filter=0，ABI 上是 cleanup），同一程序会输出 `caught=7 lp_calls=1` 并 exit 1。

## 坑清单（每条都有实测现象 / 原因 / 规避）

### 坑 1：catch-all 必须有 TType 表 —— `filter==0` 是 cleanup，不是 catch-all
这是 Z1.5 的核心修正。「call-site action 下标 != 0 就当 handler」是私有约定：
ABI 里 handler 由 action 记录的 **filter>0** 决定；`filter==0` 是 cleanup；
catch-all 必须 `filter>0` **且** TType 表里对应项是 null type_info*（4 字节 0 + 无重定位）。
用真 `__gxx_personality_v0` 实测旧形态：handler 夹具被当 cleanup，LP 跑 1 次后异常继续
传给外层 `catch(int)`（`caught=7`）；而 `ttype=0xff` + `filter=1` 会在 phase 1 直接
SIGABRT（get_ttype_entry 走 `__gxx_abort`）。结论：**类型表不能省**；
`ttype_encoding=0xff` 只适合真正的 cleanup-only LSDA。

### 坑 2：`.cfi_personality 0x9b` 的操作数必须是「指针槽」，不是函数本体
`0x9b` = indirect|pcrel|sdata4：展开器先按 pcrel 找到槽地址，再读 8 字节当作 personality 指针。
写成 `.cfi_personality 0x9b, <函数符号>`（缺 DW.ref/槽）：GAS 和 ld 都不报错，但运行期会把
函数代码的头 8 字节当地址跳过去 → **SIGSEGV（139），没有任何输出**。
本仓库夹具的做法：槽 = driver 里的 `z1_personality_slot` 数据符号，运行期填函数指针
（见上文「personality 指针」）；gcc 的做法是 `.data.rel.local` 里静态 `.quad 目标函数`。
同链接单元的符号也可以用 `0x1b` 直连（第一步夹具就这么做，PIE/-no-pie 都验证过）；
但指向共享库符号（如 `__gxx_personality_v0`）时 PIE/-shared 会被 ld 拒绝：
`relocation R_X86_64_PC32 against symbol ... can not be used when making a PIE object`。

### 坑 3：只挂 `.cfi_lsda`、忘了 `.cfi_personality`
GAS 静默接受（CIE 变成 `"zLR"`），运行时没有任何人认领 handler，
`_Unwind_RaiseException` 一路到栈顶返回 `_URC_END_OF_STACK`，landing pad 不执行。
排查口诀：`readelf --debug-dump=frames` 看 CIE 是 `zPR`（无 LSDA）、`zPLR`（都在）还是 `zLR`（缺 personality）。

### 坑 4：landing pad 里可能抛出的 call 必须被 call-site 表覆盖，否则标准 personality terminate
GCC 对 cleanup/catch 的 LP 里每条可能抛出的 call 都会再发一条 `lp=0, action=0` 的保护区。
缺这条时，`_Unwind_Resume` 阶段标准 personality 拿到「不在表内的 PC」→ found_terminate →
`std::terminate`。本仓库 `z1_step3.S` 的 cleanup LP 里既有 `call z1_note` 又有
`call _Unwind_Resume`，专门补了一条覆盖这两个 call 的 `lp=0` 记录；handler LP 里的
`call z1_note` 同样补了。第二步夹具的 LP 只有内存写和 `call z1_note`，也补了。

### 坑 5：`0xfe` 不是「合法但含义不同」，是结构非法
TType encoding `0xff` 时偏移字段整段消失；改成 `0xfe` 后（`0xfe != 0xff`）偏移字段
「重新出现」，紧随其后的 call-site 编码字节被吃成 TType offset，整个 header 错位
（call-site 编码被读成 `0x04`=udata8，表越界）。我们的解码器在 TType encoding 校验处
就报 `PA_LSDA_ERR_ENCODING`；另有独立用例（`z1_broken_table.S`）验证「表越界」判定
（`PA_LSDA_ERR_TRUNCATED`）。断言消息统一写成「编码非法 / 表越界」。

### 坑 6：landing pad 入口的 `%rsp` 是「抛出点 call 之前的值」，不是 CFA
现象：按「展开器已经恢复 CFA 到 rsp」写 LP 返回路径 → 进程死循环（LP 的 `ret` 跳回自己，
gdb 里同一断点反复命中，timeout 杀进程 137）。
正确规则：**LP 入口 rsp = 抛出点那条 call 执行前的 rsp**（= 函数体正常路径的 rsp），
所以 LP 的返回路径照抄正常 epilogue；被调用者保存寄存器也恢复成调用点的值。
怎么量出来的（不依赖夹具布局）：
1. gdb 断在 LP：`subq $8` 的帧里 LP 入口 `rsp = 函数入口 rsp - 8`，`[rsp+8]` 是返回地址；
2. GCC 自己的产物：`-O1` 编 `subq $72` 的大帧函数，LP 直接 `jmp` 正常 epilogue 的
   `addq $72,%rsp; ret`——只有 LP 入口 rsp 等于调用点 rsp（E-72，而不是 CFA-16=E-8）才可能对；
3. 本仓库把第二步夹具 prologue 从 `subq $8` 临时改成 `subq $24`（LP 相应 `addq $24`），
   全部断言同样通过——规则与帧大小无关（变体只用于验证，未入库）。
推论：Z2 生成 LP 时不需要额外的 CFA 修正，照抄正常 epilogue 即可。

### 坑 7：landing pad 必须在 FDE 内，且 LP 区段 CFI 要描述 LP 入口状态
否则 LP 里 `_Unwind_Resume` 时 libgcc 找不到 FDE → `SIGABRT`（退出码 **134**），
**stderr 一个字都没有**；gdb 回溯是 `abort ← libgcc_s.so.1 ← <LP 所在函数>`。
复现：把 `z1_step3.S` 的 cleanup LP 整段挪到 `.cfi_endproc` 之后（并去掉 LP 内 `.cfi_*`
指令，否则 GAS 报 `CFI instruction used without previous .cfi_startproc`），重跑即 SIGABRT。
规避：LP 留在 `cfi_startproc/endproc` 内；LP 区段按「进入 LP 时的真实状态」补
`.cfi_def_cfa_offset`（本仓库每个 LP 都这么写，包括 LP 里临时 `subq` 后成对调整）。

### 坑 8：异常对象会被 libgcc 写 —— `%rdi` 被中间调用踩掉会表现成「phase 2 找不到 handler 帧」
真实踩坑：保护区里先 `call z1_trace_probe` 再 `call z1_raise_c`，而 `z1_raise_c` 的 exc
参数是入口时的 `%rdi`，被探针调用 clobber → `_Unwind_RaiseException` 收到栈上垃圾指针。
现象：phase 1 正常 HANDLER_FOUND，phase 2 给同一帧 actions=0x2（没有 HANDLER_FRAME），
最后 `_URC_FATAL_PHASE2_ERROR`（因为 libgcc 把 handler 帧 CFA 写进了垃圾对象的 private_2，
phase 2 再读时已被栈活动覆盖）。
规避：汇编里把 `%rdi` 存进自己的栈槽、每次 call 前重装；异常对象 16 字节对齐、
private 字段抛前置 0、全程只用一个真实对象。定位技巧：`PA_PERSONALITY_TRACE` + `Z1_TRACE=1`
逐帧日志（含 cfa/action/kind），并看日志里的 exc 指针是不是自己传进去的那个。

### 坑 9：personality 必须非 static 且 `__attribute__((used))`
`.cfi_personality` 只留符号引用，C 代码里没人调用；写成 static 会被 `-O2` 当死代码删掉，
链接期报 `(.eh_frame+0x12): undefined reference to 'hidden_personality'`。

### 坑 10：personality 里 PC 语义 —— 拿到的是 call 之后的返回地址，要 -1
展开器给的 IP 是抛出点 call 之后的返回地址；用 `_Unwind_GetIPInfo(ctx,&before)`，
`before==0` 时减 1 再查 call-site 表（GCC `__gxx_personality` 同样处理）。不减的话，
当 call 是保护区最后一条指令（返回地址正好等于区间末尾）会查不到。

### 坑 11：`_Unwind_Backtrace` 不调用 personality，别拿它测 FORCE_UNWIND
实测 traceback 只按 CFI 走帧、personality 一次都不会被调。真正会带 `_UA_FORCE_UNWIND`
调 personality 的是 `_Unwind_ForcedUnwind`（本仓库第二步在保护区里触发它，并断言
personality 只 CONTINUE、`_Unwind_SetIP` 0 次）。

### 坑 12：LSDA 对齐 / 节属性 / TType 表位置
`.gcc_except_table` 里每个 LSDA 前 `.align 4`；节声明 `.section .gcc_except_table,"a",@progbits`。
action 表与 TType 表之间是 `.align 4` 的 padding（不属于任何表，不能按「action 表长度」硬解析）。
TType 表项按 ttype 编码定宽（0x9b → 4 字节），且 null 项必须「4 字节 0 + 无重定位」——
用 `readelf -SW` 确认没有 `.rela.gcc_except_table`。

### 坑 13：PIE / -no-pie 要整套一致
driver/夹具用 `-fno-pie` 就配 `-no-pie`，用 `-fPIE` 就配 `-pie`；混用会出现
`relocation ... can not be used when making a PIE object`。本仓库 run.sh 两套都跑、各 58 项断言全过。

### 坑 14：汇编约定
``.cfi_startproc/`.cfi_endproc`` 必须包住整个函数（含 LP）；LP 里调用 helper 前注意 16 字节
栈对齐（LP 入口 rsp 是调用点 rsp，直接 call 即可）；建议加
``.section .note.GNU-stack,"",@progbits``（GNU 惯例；本环境 binutils 2.44 漏掉不报警）。

## 已知局限（Z1.5 刻意不做，Z2/Z3/Z4 再补）

1. **不做类型匹配**：`filter>0` 时会把 TType 表项解出来（顺便校验表），但非 null 的 typed catch
   也一律按 handler 处理（catch-all 语义）。Z3 要做 phase 1 类型过滤时，在
   `pa_classify_action` 的 filter>0 分支里用 `pa_get_ttype_entry` 拿 type_info* 做匹配、
   不匹配时沿 `next` 继续读 action 链即可（结构已就位）。
2. **filter<0（exception spec）**：选择 CONTINUE_UNWIND + 诊断计数（理由见上）。
3. **TType 偏移/action 链只做了合理性校验**（反向索引下溢、action 指针/长度上限 64 条、
   call-site 条目上限 4096），因为 `_Unwind_GetLanguageSpecificData` 不提供 LSDA 长度，
   做不了真正边界检查；超界时以「表越界/编码非法」报错而不是静默错算。
4. 编码只支持 absptr/uleb128/udata2/udata4/udata8/sleb128/sdata2/sdata4/sdata8 的
   absptr/pcrel/funcrel/aligned/indirect 组合；textrel/datarel 需要段基址，明确报
   `PA_LSDA_ERR_ENCODING`。
5. crosscheck 只验证「结构/语义被 gcc personality 认作 catch-all」：夹具 LP 是正常返回，
   没有走 `__cxa_begin_catch/end_catch`（C++ 异常对象会泄漏，程序马上退出，不影响裁决）。
6. 异常类/foreign exception、多线程/异步异常、生成器/协程挂起（见设计文档「已知难点」）不在 Z1.5 范围。

## 给 Z2 的 codegen 约束（从本夹具直接抄）

1. 函数级：`.cfi_personality 0x9b, DW.ref.<personality>`（或运行期槽；gcc 同款编码）
   + `.cfi_lsda 0x1b, .LLSDA_xxx`。
2. catch-all LSDA 用 20 字节形态：`ff 9b <offset> 01 <len> <条目...> 01 00 <align4> 00000000`；
   TType 偏移指向表末尾、null 项 4 字节 0 且**无重定位**；action 记录 `(filter=1, next=0)`。
   cleanup-only 用 12 字节形态（`ff ff 01 <len> ...`，action 下标 0，无 action/TType 表）。
3. 保护区用一对标签界定在 `call` 前后；call-site 条目 `start/len/lp/action` 四个字段
   （前三个按 call-site 编码，action 恒 uleb128）。**LP 里每条可能抛出的 call 都要补一条
   `lp=0, action=0` 的保护区**（否则标准 personality terminate）。
4. LP 代码的栈恢复 = 正常 epilogue（LP 入口 rsp 就是抛出点 call 前的 rsp），LP 区段发射与
   「调用点状态」一致的 CFI。
5. LP 里先按现有语义做类型匹配/清理：handler 帧直接跑 except 体；cleanup 帧必须 `_Unwind_Resume`。
6. 抛点函数本身也要有完整 `.eh_frame`（`-funwind-tables` 已具备）。
7. exception 对象：16 字节对齐、`_Unwind_Exception` 32 字节 + payload 跟在后面、
   `private_1/private_2` 抛前置 0、只用一个真实对象。
