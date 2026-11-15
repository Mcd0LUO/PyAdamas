/* z1_spec_lsda.S —— filter < 0（exception spec）的处理选择：CONTINUE_UNWIND
 *
 * ABI 里 action 记录的 filter<0 表示 exception specification（throw() 之类）。
 * 我们目前不维护异常规格表，也没有 __cxa_call_unexpected 协议；选择的语义是
 * 「当作与本帧无关，继续展开」，并记一次 pa_dbg_spec 诊断计数，**不 terminate**。
 * 理由写在 README「filter<0 的处理」：Z2 不会发射负 filter；即使将来遇到，
 * 静默继续比在用户进程里 abort 更可控，真正的 spec 语义等 Z4 再做。
 *
 * 注意：这个夹具只用于验证我们自己的选择，**不是** ABI 合法的 spec 表
 * （ABI 的 spec 列表放在 TType 表里，这里为了最小化没有建表）。
 * 期望：personality 在 phase 1 返回 CONTINUE → 没人接管 → _URC_END_OF_STACK，
 *       spec 计数 +1，landing pad 不执行。
 */
	.text
	.globl	z1_spec_fn
	.type	z1_spec_fn, @function
z1_spec_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_spec

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)
.LEHB_spec:
	movq	(%rsp), %rdi
	call	z1_raise_c
.LEHE_spec:
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	.globl	z1_spec_lp
z1_spec_lp:
	.cfi_def_cfa_offset 16
	movl	$1, z1_spec_lp_hit(%rip)	/* spec 帧不该被当 handler，不该执行 */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z1_spec_fn, .-z1_spec_fn

	.section	.gcc_except_table,"a",@progbits
	.align	4
.LLSDA_spec:
	.byte	0xff				/* LPStart enc = omit */
	.byte	0xff				/* TType enc = omit */
	.byte	0x01
	.uleb128 .LLSDACSE_spec-.LLSDACSB_spec
.LLSDACSB_spec:
	.uleb128 .LEHB_spec-z1_spec_fn
	.uleb128 .LEHE_spec-.LEHB_spec
	.uleb128 z1_spec_lp-z1_spec_fn
	.uleb128 0x1				/* action 下标 1 → 读 action 记录 */
.LLSDACSE_spec:
	.byte	0x7f				/* filter = -1（SLEB128） */
	.byte	0x00				/* next = 0 */
	.align	4

	.section	.note.GNU-stack,"",@progbits
