/* z1_broken_table.S —— 负向用例 B：call-site 表越界
 *
 * LSDA 头本身可解析（TType 省略、call-site 编码 = 0x04 udata8、表长 8 字节），
 * 但 udata8 下每条 call-site 要 3×8 字节 + 1 个 action uleb = 25 字节 > 8 字节表长，
 * 解码器必须报 PA_LSDA_ERR_TRUNCATED = 「表越界」，而不是越读越界。
 * 表后面故意放了 32 字节 0，保证解码器即使读到表外也在节内（能复现「越界判定」而不是段错误）。
 */
	.text
	.globl	z1_broken_table_fn
	.type	z1_broken_table_fn, @function
z1_broken_table_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_broken_table

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)
.LEHB_broken_table:
	movq	(%rsp), %rdi
	call	z1_raise_c
.LEHE_broken_table:
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret
	.cfi_endproc
	.size	z1_broken_table_fn, .-z1_broken_table_fn

	.section	.gcc_except_table,"a",@progbits
	.align	4
.LLSDA_broken_table:
	.byte	0xff				/* LPStart enc = omit */
	.byte	0xff				/* TType enc = omit */
	.byte	0x04				/* call-site enc = udata8（每字段 8 字节） */
	.uleb128 8				/* 表长只有 8 字节 */
	.rept	32
	.byte	0				/* 让解码器读表外时仍在本节内 */
	.endr

	.section	.note.GNU-stack,"",@progbits
