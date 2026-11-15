/* mini_m2_cleanup_semantics.S
 * 目的：拿 **gcc/libstdc++ 的 __gxx_personality_v0** 去解析 M2 形态的 LSDA
 *       （ttype_encoding=0xff 无 TType 表 + call-site action!=0 + action 记录 (0x00,0)），
 *       观察 ABI 规定的真实语义（本文件不改 tests/exc/ 里的夹具，只独立复刻其字节形态）。
 *
 * 注意：着陆垫里 call _Unwind_Resume 的那条 call 也必须被一条 call-site 记录覆盖
 *       （GCC 会发第二段“无 landing pad”的保护区）。否则 __gxx_personality_v0 在
 *       _Unwind_Resume 阶段会拿到“不在表内”的 PC → found_terminate → std::terminate。
 *       本夹具第二段保护区即 .LEHB2_m2..LEHE2_m2（lp=0, action=0）。
 */
    .text
    .globl  m2_like_fn
    .type   m2_like_fn, @function
m2_like_fn:
.LFB_m2:
    .cfi_startproc
    .cfi_personality 0x9b, DW.ref.__gxx_personality_v0
    .cfi_lsda 0x1b, LSDA_m2
    subq    $8, %rsp
    .cfi_def_cfa_offset 16
.LEHB_m2:
    call    throw_int
.LEHE_m2:
    addq    $8, %rsp
    .cfi_def_cfa_offset 8
    xorl    %eax, %eax
    ret
.Llp_m2:
    .cfi_def_cfa_offset 16
    addq    $1, m2_lp_runs(%rip)
    movq    %rax, %rdi
.LEHB2_m2:
    call    _Unwind_Resume@PLT
.LEHE2_m2:
    ud2
    .cfi_endproc
.LFE_m2:
    .size   m2_like_fn, .-m2_like_fn

    .section .data.rel.local,"aw",@progbits
    .align 8
DW.ref.__gxx_personality_v0:
    .quad   __gxx_personality_v0

    .data
    .globl  m2_lp_runs
    .align 8
m2_lp_runs:
    .quad   0

    .section .gcc_except_table,"a",@progbits
    .align 4
LSDA_m2:
    .byte   0xff
    .byte   0xff
    .byte   0x01
    .uleb128 .LLSDACSE_m2-.LLSDACSB_m2
.LLSDACSB_m2:
    /* 条目 1：被保护的 call throw_int → landing pad，action=1 */
    .uleb128 .LEHB_m2-.LFB_m2
    .uleb128 .LEHE_m2-.LEHB_m2
    .uleb128 .Llp_m2-.LFB_m2
    .uleb128 0x1
    /* 条目 2：landing pad 里 call _Unwind_Resume → 无 landing pad（覆盖住） */
    .uleb128 .LEHB2_m2-.LFB_m2
    .uleb128 .LEHE2_m2-.LEHB2_m2
    .uleb128 0
    .uleb128 0
.LLSDACSE_m2:
    .byte   0x00
    .byte   0x00
    .align 4

    .section .note.GNU-stack,"",@progbits
