/* z1_broken_lsda.S —— 负向用例 A：LSDA 编码非法
 *
 * 和 z1_step2.S 的 LSDA 只差 1 个字节：TType encoding 0x9b → 0xfe。
 * 0xfe 的低 4 位 0x0e 不是合法的 DW_EH_PE 数据格式；而且按 ABI 的宽松解析
 * （0xfe != 0xff）TType 偏移字段会「重新出现」，把后面的 call-site 编码字节吃掉，
 * 整个 header 解析错位（call-site 编码会被读成 0x04=udata8，表越界）。
 * 我们的解码器在 TType encoding 校验处就干净地报 PA_LSDA_ERR_ENCODING = 「编码非法」；
 * 断言把这一类统称「编码非法 / 表越界」。
 *
 * 期望：_Unwind_RaiseException 返回 _URC_FATAL_PHASE1_ERROR，
 *       pa_personality_last_error 是「编码非法 / 表越界」类错误，landing pad 永不执行。
 */
	.text
	.globl	z1_broken_fn
	.type	z1_broken_fn, @function
z1_broken_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_broken

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)
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
	.byte	0xff				/* LPStart enc = omit，合法 */
	.byte	0xfe				/* ← 故意改坏：TType encoding 非法 */
	.uleb128 .LLSDATT_broken-.LLSDATTD_broken
.LLSDATTD_broken:
	.byte	0x01
	.uleb128 .LLSDACSE_broken-.LLSDACSB_broken
.LLSDACSB_broken:
	.uleb128 .LEHB_broken-z1_broken_fn
	.uleb128 .LEHE_broken-.LEHB_broken
	.uleb128 z1_broken_lp-z1_broken_fn
	.uleb128 0x1
.LLSDACSE_broken:
	.byte	0x01
	.byte	0x00
	.align	4
	.long	0
.LLSDATT_broken:

	.section	.note.GNU-stack,"",@progbits
