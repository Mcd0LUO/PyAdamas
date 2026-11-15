# m1 手写 LSDA 用例：catch-all + gcc 同款间接 personality（0x9b + DW.ref 槽）。
# 只要求汇编/链接通过并 dump 出 .gcc_except_table / .eh_frame；不要求运行。
# 函数体特意和 gcc -O1 的 probe_catch_all 布局对齐：
#   LEHB=0x04 LEHE=0x09 landing-pad=0x13 第二保护区=0x1b..0x20 总长=0x27
# 因此生成的 LSDA 应当与 probe_catch_all.o 的 20 字节逐字节相同。

    .text
    .globl  mini_catch_all
    .type   mini_catch_all, @function
mini_catch_all:
.LFB_mini:
    .cfi_startproc
    .cfi_personality 0x9b, DW.ref.mini_personality
    .cfi_lsda 0x1b, LSDA_mini
    subq    $8, %rsp
    .cfi_def_cfa_offset 16
.LEHB_mini:
    call    mini_maybe_throw
.LEHE_mini:
    movl    $1, %eax
.Lmini_ret:
    addq    $8, %rsp
    .cfi_remember_state
    .cfi_def_cfa_offset 8
    ret
.Lmini_lp:
    .cfi_restore_state
    movq    %rax, %rdi
    call    mini_begin
.LEHB2_mini:
    call    mini_end
.LEHE2_mini:
    movl    $2, %eax
    jmp     .Lmini_ret
    .cfi_endproc
.LFE_mini:
    .size   mini_catch_all, .-mini_catch_all

# 被调用/引用的本地桩函数（只保证链接，不实现语义）
    .type   mini_maybe_throw, @function
mini_maybe_throw:
    ret
    .size   mini_maybe_throw, .-mini_maybe_throw
    .type   mini_begin, @function
mini_begin:
    ret
    .size   mini_begin, .-mini_begin
    .type   mini_end, @function
mini_end:
    ret
    .size   mini_end, .-mini_end

# personality：_URC_CONTINUE_UNWIND = 5
    .type   mini_personality, @function
mini_personality:
    movl    $5, %eax
    ret
    .size   mini_personality, .-mini_personality

# 0x9b = indirect|pcrel|sdata4：CIE 里存的是指向“指针槽”的 pcrel 偏移，槽里才是 personality 地址。
    .section    .data.rel.local,"aw",@progbits
    .align 8
    .type   DW.ref.mini_personality, @object
    .size   DW.ref.mini_personality, 8
DW.ref.mini_personality:
    .quad   mini_personality

# LSDA：与 gcc -O1 的 catch-all 形态一致（20 字节）
    .section    .gcc_except_table,"a",@progbits
    .align 4
LSDA_mini:
    .byte   0xff                    # LPStart 编码 = DW_EH_PE_omit（用 FDE 的 region start）
    .byte   0x9b                    # TType 编码 = indirect|pcrel|sdata4
    .uleb128 LSDATT_mini-LSDATTD_mini   # TType 表偏移：指向表“末尾”，反向索引
LSDATTD_mini:
    .byte   0x01                    # call-site 表编码 = uleb128
    .uleb128 LSDACSE_mini-LSDACSB_mini  # call-site 表长度
LSDACSB_mini:
    .uleb128 .LEHB_mini-.LFB_mini   #   保护区起点
    .uleb128 .LEHE_mini-.LEHB_mini  #   保护区长度
    .uleb128 .Lmini_lp-.LFB_mini    #   landing pad
    .uleb128 0x1                    #   action 下标（1 = action 表第 0 项）
    .uleb128 .LEHB2_mini-.LFB_mini
    .uleb128 .LEHE2_mini-.LEHB2_mini
    .uleb128 0                      #   无 landing pad
    .uleb128 0
LSDACSE_mini:
    .byte   0x1                     # action 记录：filter = 1（指向 TType[1]）
    .byte   0x0                     #               next  = 0（链尾）
    .align 4
    .long   0                       # TType[1] = 空指针 => catch(...)
LSDATT_mini:
