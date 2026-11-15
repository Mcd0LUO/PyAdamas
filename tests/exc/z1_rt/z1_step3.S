/* z1_step3.S —— 第三步：两阶段展开的行为验证（Z3 / finally 的前置）
 *
 * 栈形状（由内到外）：
 *
 *   z1_step3_nopad_fn   personality + LSDA，但保护区条目的 landing pad = 0
 *                       → 展开时必须被静默穿过
 *   z1_step3_cleanup_fn cleanup 帧：LSDA 条目 action = 0（纯清理），
 *                       landing pad B 记录顺序、改全局状态后 _Unwind_Resume
 *   z1_step3_handler_fn catch-all 帧：LSDA 条目 action = 1（handler），
 *                       landing pad A 记录顺序后正常返回
 *
 * 断言：phase 1 只有外层认领 handler；phase 2 先给中间 cleanup 帧安装
 * 上下文 → B 先跑；B 里 _Unwind_Resume 后外层 handler 帧被安装 → A 后跑；
 * 两层各跑一次；没有 landing pad 的中间帧对结果没有影响。
 */
	.text

/* ── 外层：catch-all handler ─────────────────────────────────────── */
	.globl	z1_step3_handler_fn
	.type	z1_step3_handler_fn, @function
z1_step3_handler_fn:
	.cfi_startproc
	.cfi_personality 0x1b, pa_personality
	.cfi_lsda 0x1b, .LLSDA_handler

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)			/* 保存 driver 传入的 exception 指针（保护区里第一个 call 可能 clobber %rdi） */
.LEHB_handler:
	movq	(%rsp), %rdi
	call	z1_step3_cleanup_fn
.LEHE_handler:
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	.globl	z1_step3_handler_lp
z1_step3_handler_lp:
	.cfi_def_cfa_offset 16
	/* 入口：rsp = 调用点 rsp（= 函数入口 rsp - 8），%rax = exception 指针。 */
	movl	$2, %edi			/* tag = 2：handler 层 */
	movq	%rax, %rsi
	call	z1_note
	movl	$100, %eax			/* 从 handler 正常返回给 driver */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z1_step3_handler_fn, .-z1_step3_handler_fn

/* ── 中间层：cleanup（finally / 析构 的模型） ────────────────────── */
	.globl	z1_step3_cleanup_fn
	.type	z1_step3_cleanup_fn, @function
z1_step3_cleanup_fn:
	.cfi_startproc
	.cfi_personality 0x1b, pa_personality
	.cfi_lsda 0x1b, .LLSDA_cleanup

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)			/* 保存 driver 传入的 exception 指针（保护区里第一个 call 可能 clobber %rdi） */
.LEHB_cleanup:
	movq	(%rsp), %rdi
	call	z1_step3_nopad_fn
.LEHE_cleanup:
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	.globl	z1_step3_cleanup_lp
z1_step3_cleanup_lp:
	.cfi_def_cfa_offset 16
	/* 入口：rsp = 调用点 rsp，%rax = exception 指针。
	   给 helper 调用留 16 字节，把 exc 存起来（helper 会 clobber rax）；
	   16 字节之后 rsp 仍是 16 字节对齐，call 的 ABI 对齐没问题。 */
	subq	$16, %rsp
	.cfi_def_cfa_offset 32
	movq	%rax, 0(%rsp)
	movl	$1, %edi			/* tag = 1：cleanup 层 */
	movq	%rax, %rsi
	call	z1_note				/* 记录“B 先跑”并改全局状态 */
	movq	0(%rsp), %rdi
	addq	$16, %rsp
	.cfi_def_cfa_offset 16
	/* 继续展开：外层 handler 帧随后被安装，A 才会跑。
	   注意这条 call 之后不会返回（_Unwind_Resume 是 noreturn）。 */
	call	_Unwind_Resume
	ud2
	.cfi_endproc
	.size	z1_step3_cleanup_fn, .-z1_step3_cleanup_fn

/* ── 内层：有 personality + LSDA，但保护区没有 landing pad ──────── */
	.globl	z1_step3_nopad_fn
	.type	z1_step3_nopad_fn, @function
z1_step3_nopad_fn:
	.cfi_startproc
	.cfi_personality 0x1b, pa_personality
	.cfi_lsda 0x1b, .LLSDA_nopad

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)			/* 保存 driver 传入的 exception 指针（保护区里第一个 call 可能 clobber %rdi） */
.LEHB_nopad:
	movq	(%rsp), %rdi
	call	z1_raise_c
.LEHE_nopad:
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret
	.cfi_endproc
	.size	z1_step3_nopad_fn, .-z1_step3_nopad_fn

/* ── LSDA ────────────────────────────────────────────────────────── */
	.section	.gcc_except_table,"a",@progbits
	.align	4

/* 外层：action = 1 → action 表第 0 项（catch-all） */
.LLSDA_handler:
	.byte	0xff
	.byte	0xff
	.byte	0x01
	.uleb128 .LLSDACSE_handler-.LLSDACSB_handler
.LLSDACSB_handler:
	.uleb128 .LEHB_handler-z1_step3_handler_fn
	.uleb128 .LEHE_handler-.LEHB_handler
	.uleb128 z1_step3_handler_lp-z1_step3_handler_fn
	.uleb128 1
.LLSDACSE_handler:
	.byte	0x00				/* type filter = 0：catch-all */
	.byte	0x00				/* next action offset = 0：链结束 */
	.align	4

/* 中间层：action = 0 → 纯清理，phase 1 不认领 handler */
.LLSDA_cleanup:
	.byte	0xff
	.byte	0xff
	.byte	0x01
	.uleb128 .LLSDACSE_cleanup-.LLSDACSB_cleanup
.LLSDACSB_cleanup:
	.uleb128 .LEHB_cleanup-z1_step3_cleanup_fn
	.uleb128 .LEHE_cleanup-.LEHB_cleanup
	.uleb128 z1_step3_cleanup_lp-z1_step3_cleanup_fn
	.uleb128 0
.LLSDACSE_cleanup:
	.align	4

/* 内层：保护区覆盖到了，但 landing pad 偏移 = 0 → 没有 landing pad */
.LLSDA_nopad:
	.byte	0xff
	.byte	0xff
	.byte	0x01
	.uleb128 .LLSDACSE_nopad-.LLSDACSB_nopad
.LLSDACSB_nopad:
	.uleb128 .LEHB_nopad-z1_step3_nopad_fn
	.uleb128 .LEHE_nopad-.LEHB_nopad
	.uleb128 0				/* lp = 0：这一段没有 landing pad */
	.uleb128 0
.LLSDACSE_nopad:
	.align	4

	.section	.note.GNU-stack,"",@progbits
