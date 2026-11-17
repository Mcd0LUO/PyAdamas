# 栈对齐违规：调查记录（根因已修复，-mstackrealign 已撤）

> 状态：**已修复**（2026-11-17，commit `6ae996a`；配套文档修正 `b1259f9`）。
> 两处独立的 ABI 违规 —— closure shim 的 tmp 对齐公式、C 回调蹦床缺 rsp 调整 ——
> 都已修掉，普通运行时的 `-mstackrealign`（`2860da5` 的止血措施）随之撤除。
> §9 是修复与验证记录；§1–§8 保留完整排查路径，其中 §7 的推断已推翻、§8 的计划已被取代。

## 1. 现象

换成服务器（Debian 13 + gcc 15）后 `tests/run/24_async.py` 在 -O0/-O1 SIGSEGV，
本地（gcc 14）正常。

## 2. 证据链

1. **不是编译器被误编译**：本地与服务器生成的汇编逐行 diff，只差源码路径
   字符串（长度/hash 跟着变），机器码完全相同。
2. 崩溃指令：
   ```
   pa_gen_new+20:  movaps %xmm1,(%rsp)
   rsp = 0x7fffffffe628   ; mod 16 = 8
   ```
   `movaps` 要求 16 字节对齐。gcc 15 用它溢出 xmm；gcc 14 恰好没用。
3. 条件断点抓到不对齐的调用：
   ```
   break pa_gen_new if ((unsigned long)$rsp % 16) != 8
   → entry rsp mod 16 = 0（应为 8），调用者 pyadamas_fn_33float_one_32
   ```
   `pa_gen_new` 序言是 `endbr64; push %rbx; ...; sub $0x20, %rsp`：
   入口 ≡8 时 body ≡0（movaps 安全），入口 ≡0 时 body ≡8 → #GP。

## 3. 已排除的假设

- **不是 prologue 数学错**：全文件静态核对 `(8 - 8·push - subq) ≡ 0 mod 16`，
  所有生成函数都满足。抽检：
  - `float_one_32`：gen=0, n_saved=0, `subq $88` → 8-88 ≡ 0 ✓
  - `float_gather_34$resume`：gen=1, n_saved=1, push 8 + `subq $144` → ≡ 0 ✓
  - `__pa_main`：n_saved=2, `subq $344` → 8-16-344 ≡ 0 ✓
- **不是函数 fall-through**：`float_one_32$resume` 以 `call pa_coro_rethrow; ud2`
  结尾，边界干净。
- **不是 coroutine dispatch 的 ABI**：`coro_step` 里是普通间接调用
  `((void(*)(void*,int64_t))(uintptr_t)p[4])(c, 0)`，不改 rsp。
- **不是 CET/IBT**：加 `-fcf-protection=none` 重编仍崩。

## 4. 现在的疑点

`pyadamas_fn_33float_one_32` 在整份汇编里**没有任何引用**（只有 `.type`/
`label`/`.size`），却会执行 —— 它是通过 `call *(%r11)` 这类**间接调用**
进入的（表地址由运行期算出）。因此：

- 进入它的那一跳发生在 rsp ≡ 0（而不是 8）的状态下；
- 之后它内部所有 `call`（包括 `call pa_gen_new`）都偏 8，gcc 15 的 `movaps`
  就炸。

gdb 的 `bt` 在这一段给出一个无法符号化的 `#2` 帧，说明帧链在该处已经不可信
（生成代码不用 rbp 链），所以不能只靠 bt 往上追。

## 5. 当时的下一步（已全部执行，见 §9）

> 这一节是修复前写的行动清单：第 1 条的自检开关（`PYADAMAS_ALIGN_CHECK`）正是
> 后来定位两处 ABI 违规的工具，第 3 条的撤除也已完成。

1. **在生成的函数序言里加对齐自检**（最直接）：
   编译期加 `PYADAMAS_ALIGN_CHECK` 开关，在每个生成函数入口发：
   ```asm
   movq %rsp, %rax
   andl $15, %eax
   cmpl $8, %eax
   je .Lok
   call pa_align_violation     # 打印 return address 并 abort
   .Lok:
   ```
   跑 24_async，**第一个**报错的函数就是被错误进入的那个，再用
   `__builtin_return_address(0)` 打印它的调用者地址 → 精确定位那一跳。
2. 找到后判断是哪一类：表驱动的间接调用 / 尾跳 / 异常展开落地。
3. 修完后把运行时的 `-mstackrealign` 撤掉，确认不再崩，验证 188/188 + 205/205。

## 6. 为什么当时先缓解而不是硬修

> 这段是修复前的止血决策记录：`-mstackrealign` 已于 `6ae996a` 撤掉（见 §9），
> 不要当成现状读。

`-mstackrealign` 让运行时函数自己重对齐 rsp，当时保证了：
- 24_async -O0/-O1/-O2 在 gcc 15 上全部正常；
- 服务器与本地全量验证都是 188/0 + 205/0。

残留风险：生成代码直接调 **libc** 里用对齐 SSE 的函数时仍可能炸（概率低，
但存在）。所以根因不能靠它遮住，仍需按 §5 修掉 —— 后来确实这么做了，见 §9。

## 7. 早期假设（**已推翻**，见 §9）

> 本节是把「无名 stub + 调用目标早 4 字节」误判成 EH 落地块的记录。
> 最终根因是 closure shim 的栈对齐公式和 C 回调跳板（见 §9），
> 两者都与 EH 无关。保留本段是为了记录排查路径。

### 当时的（错误）推断

加了编译期开关 `PYADAMAS_ALIGN_CHECK` 后（见 §5 第 1 条，实现在
`codegen_block.cpp` 的函数序言里），跑 24_async 立刻抓到：

    Program received signal SIGILL
    0x000055555556274b in pyadamas_fn_33float_one_32 () at 24_async.py:126

也就是说 **`float_one_32` 被进入时 rsp ≡ 0 (mod 16)**（应为 8）。

读 `(%rsp)` 拿到返回地址 → 文件偏移 `0xed3c`。用 `nm` 反查：

    0xec50  size=119(0x77)  pyadamas_fn_35float_gather_34   → 结束于 0xecc7
    0xed4a  size=100        main

**0xed3c 不属于任何函数** —— 它落在一段无名代码里：

    ed37:  e8 00 fa ff ff     call   e73c        ; 目标是 one_32(0xe740) 前 4 字节
    ed3c:  48 83 ec 08        sub    $0x8,%rsp   ; ← 返回地址落在这里
    ed40:  48 83 c4 18        add    $0x18,%rsp
    ed44:  c3                 ret
    ed45:  e9 02 ff ff ff     jmp    ec4c

结论：**这是一段异常落地/冷路径 stub，它自己就是在「rsp ≡ 8」的状态下执行
的**（否则被调方看不到 ≡0），所以它发 `call` 时违反 ABI。

两个连带问题：
1. 这段 stub **被发在函数 `.size` 之外**（`nm` 里没有符号归属），本身就
   不合规 —— 调试器、perf、栈展开都不认识它；
2. 它 `call` 的目标是 `0xe73c`，比 `float_one_32` 函数起点早 4 字节，
   靠落在 `.p2align` 填充上「滑」进函数体。这两处都指向同一件事：
   **落地块/冷路径的栈状态与调用约定没有按 ABI 维护**。

## 8. 当时的下一步（已被 §9 取代）

> 下面这些步骤指向 EH 落地块，方向是错的；真实修复见 §9。


1. 在 `codegen_block.cpp` 里查落地块（EH landing pad）的生成路径：它进入时
   的 rsp 偏移是否被显式建模（当前 prologue 的 `cfa_off_` 只覆盖线性代码）；
2. 落地块发出的 `call` 前是否少了 `subq $8`/`addq $8` 平衡；
3. 落地块应发在所属函数的 `.size` 之内（或显式给符号），否则 `nm`/
   `perf`/栈展开都看不见；
4. 修完后撤掉运行时的 `-mstackrealign`，跑 188/188 + 205/205 + 24_async。

开关：`PYADAMAS_ALIGN_CHECK=1 ./build/release/pyadamas build -O0 file.py`
会往每个生成函数入口插入 rsp 对齐自检（失败即 `ud2`）。

## 9. 修复完成（2026-11-17）

用 `PYADAMAS_ALIGN_CHECK` 自检把地图扫了两遍，找到**两处独立的 ABI 违规**：

### Bug 1：closure shim 的栈对齐公式错

`emit_closure_shim`（`codegen_block.cpp`）慢路径的序列是
`subq tmp → addq spill → call`，其中 `spill = 8*n`（n=实参个数）。

原来的 `tmp`：

```cpp
const int tmp = ((spill + stack_bytes + 15) & ~15) + 8;
```

只保证了「subq 之后、addq spill 之前」rsp 是 16 对齐。一旦 `spill ≡ 8`
（n 为奇数），`addq spill` 又把 rsp 推成 ≡ 8，于是 `call` 时被调方看到
≡ 0 —— 违反 ABI。

n 为偶数时 spill 是 16 的倍数，恰好不出问题，所以这个 bug 藏了很久；
`24_async` 里的 `float_one_32`（1 个 float 参数 → spill=8）正好踩中。

修法：让 tmp 满足 **call 那一刻** 的对齐条件
`(8 - tmp + spill) ≡ 0 (mod 16)`：

```cpp
int tmp = ((spill + stack_bytes + 15) & ~15);
while (((8 - tmp + spill) & 15) != 0) tmp += 8;
```

### Bug 2：C 回调跳板没有补 8 字节

`.LcbtrampN`（`codegen.cpp`）由 C 当普通函数调用（入口 rsp ≡ 8），它自己
又要发一次 `call *(%r10)`，但**没有任何 rsp 调整**：

```asm
xorl %eax, %eax
call *(%r10)     ; ← 这里 rsp 还是 ≡ 8，被调方看到 ≡ 0
ret
```

修法：call 前 `subq $8, %rsp`、之后 `addq $8, %rsp`，CFI 相应
`.cfi_def_cfa_offset` 8 → 16 → 8。

这个 bug 影响所有「把 Python 函数传给 C 当回调」的代码 ——
自检在 `112_callbacks.py`、`113_qsort.py` 上抓到 Illegal instruction。

### 验证

| 项目 | 结果 |
|---|---|
| `PYADAMAS_ALIGN_CHECK=1` difftest | **187 用例 / 0 不一致** |
| `PYADAMAS_ALIGN_CHECK=1` 全量用例 | **205 / 0**（修复前 203，两个 Illegal instruction）|
| 正常模式全量用例 | **205 / 0** |
| 9 个经典基准 | 输出全部正确 |

同时**撤掉了普通运行时的 `-mstackrealign`**（commit 2860da5 的缓解措施；
`--asan` 用的插桩版 `rt_asan.o` 编译命令里仍保留这个 flag，只影响测试构建），
证明根因确实修掉而不是被遮住。

### 遗留

- 生成函数的入口标签 `.LfN` 发在 `.p2align` **之前**，导致标签指向对齐
  填充而不是函数首字节（两者差 4 字节时，靠「滑过 NOP」仍能工作，但不干净）
  —— 待整理；
- shim / trampoline 仍发在所有函数的 `.size` 之外，`nm`/`perf`/栈展开
  看不见它们（本次排查就因此多花了几轮）—— 建议后续给它们补符号。

自检开关保留：
`PYADAMAS_ALIGN_CHECK=1 ./build/release/pyadamas build ...` 会在每个生成函数
入口插入 rsp 对齐自检，失败即 `ud2`。
