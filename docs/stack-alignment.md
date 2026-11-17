# 栈对齐违规：调查记录（缓解已落地，根因待查）

> 状态：**已缓解**（运行时 `-mstackrealign`，commit 2860da5）。
> 根因未定位，本文记录全部证据与下一步。

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

## 5. 下一步（按性价比排序）

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

## 6. 为什么先缓解而不是硬修

`-mstackrealign` 让运行时函数自己重对齐 rsp，已经保证：
- 24_async -O0/-O1/-O2 在 gcc 15 上全部正常；
- 服务器与本地全量验证都是 188/0 + 205/0。

残留风险：生成代码直接调 **libc** 里用对齐 SSE 的函数时仍可能炸（概率低，
但存在）。所以根因仍需按 §5 修掉。
