# 零开销异常 —— 设计草案

状态：**未实现**。本文是把 Itanium C++ ABI 的异常机制拆解到可实施粒度后的记录，
供后续动手时按图施工，不必重新调研。

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

- **`.eh_frame` 已生成**：`readelf -S` 能看到 `.eh_frame` / `.eh_frame_hdr`，
  CFI 指令（`.cfi_startproc`/`.cfi_def_cfa_offset`/`.cfi_offset` 等）每个函数都有。
  这是最关键的一条 —— libgcc 的展开器**已经能逐帧走过我们生成的代码**，
  `_Unwind_Backtrace` 就是这样实现 traceback 的。
- **libgcc 的 `_Unwind_*` 已经可用**：运行时里已有 5 处 `_Unwind_Backtrace`。

缺的只有三样：LSDA 表、personality routine、用 `_Unwind_RaiseException` 抛。

## LSDA 的确切格式（从 GCC 产物反推）

用下面这段 C++ 让 GCC 生成一份参考：

```cpp
extern void maybe_throw(int);
int probe(int n) {
    try { maybe_throw(n); return 1; }
    catch (int) { return 2; }
}
```

`g++ -O1 -S` 得到：

### 函数级指令

```asm
    .cfi_personality 0x9b, DW.ref.__gxx_personality_v0
    .cfi_lsda 0x1b, .LLSDA0
```

`0x9b` 是 personality 指针的编码（`DW_EH_PE_indirect|pcrel|udata4`），
`0x1b` 是 LSDA 指针的编码（`pcrel|udata4`）。

### 保护区标记

```asm
.LEHB0:                    # Exception Handler Begin
    call maybe_throw
.LEHE0:                    # Exception Handler End
```

这对标签界定「哪些 PC 落在保护区内」。我们的 lowering 里对应的就是
`try.body` 块的起止。

### LSDA 数据

```asm
    .section .gcc_except_table,"a",@progbits
    .align 4
.LLSDA0:
    .byte   0xff                     # LPStart 编码 = DW_EH_PE_omit（用函数起点）
    .byte   0x9b                     # TType 编码
    .uleb128 .LLSDATT0-.LLSDATTD0    # TType 表的偏移
.LLSDATTD0:
    .byte   0x1                      # call-site 表编码 = uleb128
    .uleb128 .LLSDACSE0-.LLSDACSB0   # call-site 表长度
.LLSDACSB0:                          # 每条目 4 项
    .uleb128 .LEHB0-.LFB0            #   保护区起点（相对函数起点）
    .uleb128 .LEHE0-.LEHB0           #   保护区长度
    .uleb128 .L5-.LFB0               #   landing pad 地址（0 表示无）
    .uleb128 0x1                     #   action 下标（0 = 只做清理，不捕获）
    .uleb128 .LEHB1-.LFB0
    .uleb128 .LEHE1-.LEHB1
    .uleb128 0                        #   无 landing pad
    .uleb128 0
.LLSDACSE0:
    .byte   0x1                      # action 表：1 条
    .byte   0
    .align 4
    .long   DW.ref._ZTIi-.            # TType 表：类型信息指针
.LLSDATT0:
```

**对我们最有用的简化**：如果只做「catch-all + 内联 except 体」而不需要按类型分派，
action 表可以只有一条 `0`（cleanup），landing pad 统一跳到我们的 except 分发块，
由现有代码按类型匹配。这样类型表可以退化成空，personality 实现只需处理
`_URC_HANDLER_FOUND` / `_URC_CONTINUE_UNWIND` 两个返回码。

## personality routine 契约

```c
_Unwind_Reason_Code personality(int version, _Unwind_Action actions,
                                uint64_t ex_class, struct _Unwind_Exception* exc,
                                struct _Unwind_Context* ctx);
```

phase 1（`_UA_SEARCH_PHASE`）：查 LSDA 的 call-site 表，当前 PC 落在哪个保护区、
有没有 landing pad。有就返回 `_URC_HANDLER_FOUND` 并把自己的上下文存到
`exc->private`（phase 2 要用）。

phase 2（`_UA_CLEANUP_PHASE`，带 `_UA_HANDLER_FRAME`）：对每个经过的帧做清理
（释放帧里登记的堆引用），回到自己的帧时安装上下文并返回 `_URC_INSTALL_CONTEXT`，
展开器就会跳到 landing pad。

「释放帧里登记的堆引用」这件事**现有代码已经做了**（`pa_frame_unwind`），
可以直接复用。

## 分阶段落地

| 阶段 | 内容 | 验收 |
|---|---|---|
| **Z1** | 写 personality routine 骨架 + 手写一个带 LSDA 的汇编函数做端到端验证（不接编译器） | `_Unwind_RaiseException` 能把控制转到 landing pad |
| **Z2** | 编译器为「catch-all 的单层 try」发射 `.cfi_personality`/`.cfi_lsda`/`.gcc_except_table`；`--zero-cost-exc` 开关选择新路径 | 该用例输出与 setjmp 路径一致 |
| **Z3** | 类型匹配下放到 personality 之前，即 landing pad 里先跑现有的 `pa_exc_matches` 分发（保持现有语义不变） | 全部 177 个 POSIX 用例通过 |
| **Z4** | 覆盖嵌套 try / finally / 生成器与协程挂起（这两者涉及 handler 栈重挂，是难点） | 异常相关的 30+ 用例全绿 |
| **Z5** | 移除 setjmp 路径，删掉 HandlerPush/Setjmp/HandlerLeave 三个 IR 操作 | 汇编里不再出现 `_setjmp` |

## 已知难点

1. **生成器/协程的挂起恢复**：现有实现用 `pa_handler_suspend` 把 handler 记录
   从全局栈上摘下来、恢复时重新 `setjmp`。表驱动方案里没有「handler 栈」，
   挂起时展开器的上下文会失效 —— 这块需要单独设计（可能要给每个 resume 帧
   保存一份 `_Unwind_Context` 快照）。
2. **Win64 目标**：COFF 没有 `.gcc_except_table`，Windows 用 SEH
   （`.pdata`/`.xdata` + `.seh_handler`）。两个目标的异常机制完全不同，
   TargetABI 需要再加一层「异常模型」的抽象。**不建议两条一起做。**
3. **`finally` 与 `with`**：它们不是 catch，是 cleanup，需要
   `_UA_CLEANUP_PHASE` 里逐帧执行代码（两步展开），比 catch 复杂。

## 建议

先做 **Z1**：它不碰编译器，风险为零，但能一次性验证 personality 契约、
LSDA 编码、landing pad 跳转这三件最容易出错的事。Z1 通了再谈 Z2。

不要和 Win64 的 SEH 一起做。
