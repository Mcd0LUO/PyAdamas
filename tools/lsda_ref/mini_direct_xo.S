# m1 变体：personality 用 0x1b 直接 pcrel 指向**另一个 TU**里的普通函数（定义在最终可执行文件内）。
# 用来回答：省掉 DW.ref 间接层时，什么情况下能链接、什么情况下会被 ld 拒绝。
    .text
    .globl  mini_catch_all
    .type   mini_catch_all, @function
mini_catch_all:
.LFB_mini:
    .cfi_startproc
    .cfi_personality 0x1b, ext_personality
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

    .section    .gcc_except_table,"a",@progbits
    .align 4
LSDA_mini:
    .byte   0xff
    .byte   0x9b
    .uleb128 LSDATT_mini-LSDATTD_mini
LSDATTD_mini:
    .byte   0x01
    .uleb128 LSDACSE_mini-LSDACSB_mini
LSDACSB_mini:
    .uleb128 .LEHB_mini-.LFB_mini
    .uleb128 .LEHE_mini-.LEHB_mini
    .uleb128 .Lmini_lp-.LFB_mini
    .uleb128 0x1
    .uleb128 .LEHB2_mini-.LFB_mini
    .uleb128 .LEHE2_mini-.LEHB2_mini
    .uleb128 0
    .uleb128 0
LSDACSE_mini:
    .byte   0x1
    .byte   0x0
    .align 4
    .long   0
LSDATT_mini:
