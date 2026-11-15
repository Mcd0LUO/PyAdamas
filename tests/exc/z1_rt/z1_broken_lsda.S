/* z1_broken_lsda.S —— 负向用例：故意改坏 LSDA 里一个字节，验证
 * pa_personality 真的在解析表、并且能报出对应的解码失败。
 *
 * 和 z1_step2.S 的差别只有 LSDA 头里 TType encoding 那个字节：
 *   z1_step2.S:  .byte 0xff   （DW_EH_PE_omit，合法）
 *   z1_broken:   .byte 0xfe   （低 4 位 0x0e 不是合法数据类型 → 解码失败）
 * 只差 1 个 bit。
 *
 * 期望：_Unwind_RaiseException 返回 _URC_FATAL_PHASE1_ERROR，
 *       pa_personality_last_error == PA_LSDA_ERR_ENCODING，
 *       并且 landing pad 永远不会被执行。
 */
	.text
	.globl	z1_broken_fn
	.type	z1_broken_fn, @function
z1_broken_fn:
	.cfi_startproc
	.cfi_personality 0x1b, pa_personality
	.cfi_lsda 0x1b, .LLSDA_broken

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)			/* 保存 driver 传入的 exception 指针（保护区里第一个 call 可能 clobber %rdi） */
.LEHB_broken:
	movq	(%rsp), %rdi
	call	z1_raise_c
.LEHE_broken:
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	.globl	z1_broken_lp
z1_broken_lp:
	.cfi_def_cfa_offset 16
	movl	$1, z1_broken_lp_hit(%rip)	/* 坏表下这行不该执行 */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z1_broken_fn, .-z1_broken_fn

	.section	.gcc_except_table,"a",@progbits
	.align	4
.LLSDA_broken:
	.byte	0xff				/* LPStart encoding = omit，合法 */
	.byte	0xfe				/* ← 故意改坏：TType encoding 非法 */
	.byte	0x01
	.uleb128 .LLSDACSE_broken-.LLSDACSB_broken
.LLSDACSB_broken:
	.uleb128 .LEHB_broken-z1_broken_fn
	.uleb128 .LEHE_broken-.LEHB_broken
	.uleb128 z1_broken_lp-z1_broken_fn
	.uleb128 1
.LLSDACSE_broken:
	.byte	0x00
	.byte	0x00
	.align	4

	.section	.note.GNU-stack,"",@progbits
