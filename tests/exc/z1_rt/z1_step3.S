/* z1_step3.S —— 第三步：两阶段展开的行为验证（Z3 / finally 的前置）
 *
 * 栈形状（由内到外）：
 *   z1_step3_nopad_fn    personality + LSDA，但保护区条目 landing pad = 0
 *                        → 展开时必须被静默穿过
 *   z1_step3_cleanup_fn  cleanup 帧：call-site action 下标 = 0（纯 cleanup），
 *                        LP 记录顺序、改状态后 _Unwind_Resume
 *   z1_step3_handler_fn  catch-all 帧：GCC 同款 LSDA（filter>0 + null TType 项），
 *                        LP 记录顺序后正常返回
 *
 * LSDA 全部按 GCC 形态发：
 *   handler 帧 = 20 字节（ttype=0x9b + null 项 + filter=1 + 2 条 call-site）
 *   cleanup 帧 = 12 字节（ttype 省略 + 2 条 call-site，action 下标 0）
 *   nopad   帧 = 8 字节 （ttype 省略 + 1 条 call-site，lp=0）
 * cleanup/handler 的 landing pad 里调用了 C helper（可能抛出的位置），
 * 各补一条 lp=0 的 call-site 记录——GCC 对这类帧也这么发，缺了标准 personality
 * 在 _Unwind_Resume 阶段会 found_terminate。
 */
	.text

/* ── 外层：catch-all handler ─────────────────────────────────────── */
	.globl	z1_step3_handler_fn
	.type	z1_step3_handler_fn, @function
z1_step3_handler_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_handler

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)
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
	movl	$2, %edi			/* tag = 2：handler 层 */
	movq	%rax, %rsi
.LEHB_handler_lp:
	call	z1_note
.LEHE_handler_lp:
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
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_cleanup

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)
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
.LEHB_cleanup_lp:
	call	z1_note				/* 记录“B 先跑”并改全局状态 */
	movq	0(%rsp), %rdi
	addq	$16, %rsp
	.cfi_def_cfa_offset 16
	/* 继续展开：外层 handler 帧随后被安装，A 才会跑。
	   这条 call 之后不会返回（_Unwind_Resume 是 noreturn）。 */
	call	_Unwind_Resume
.LEHE_cleanup_lp:
	ud2
	.cfi_endproc
	.size	z1_step3_cleanup_fn, .-z1_step3_cleanup_fn

/* ── 内层：有 personality + LSDA，但保护区没有 landing pad ──────── */
	.globl	z1_step3_nopad_fn
	.type	z1_step3_nopad_fn, @function
z1_step3_nopad_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_nopad

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)
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

/* 外层 handler：20 字节，ttype=0x9b + null 项 + filter=1（catch-all） */
.LLSDA_handler:
	.byte	0xff
	.byte	0x9b
	.uleb128 .LLSDATT_handler-.LLSDATTD_handler
.LLSDATTD_handler:
	.byte	0x01
	.uleb128 .LLSDACSE_handler-.LLSDACSB_handler
.LLSDACSB_handler:
	.uleb128 .LEHB_handler-z1_step3_handler_fn
	.uleb128 .LEHE_handler-.LEHB_handler
	.uleb128 z1_step3_handler_lp-z1_step3_handler_fn
	.uleb128 0x1
	.uleb128 .LEHB_handler_lp-z1_step3_handler_fn
	.uleb128 .LEHE_handler_lp-.LEHB_handler_lp
	.uleb128 0
	.uleb128 0
.LLSDACSE_handler:
	.byte	0x01
	.byte	0x00
	.align	4
	.long	0
.LLSDATT_handler:

/* 中间 cleanup：12 字节，ttype 省略，action 下标 0（纯 cleanup） */
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
	.uleb128 .LEHB_cleanup_lp-z1_step3_cleanup_fn
	.uleb128 .LEHE_cleanup_lp-.LEHB_cleanup_lp
	.uleb128 0
	.uleb128 0
.LLSDACSE_cleanup:

/* 内层 nopad：8 字节，lp=0 → 该区间没有 landing pad */
.LLSDA_nopad:
	.byte	0xff
	.byte	0xff
	.byte	0x01
	.uleb128 .LLSDACSE_nopad-.LLSDACSB_nopad
.LLSDACSB_nopad:
	.uleb128 .LEHB_nopad-z1_step3_nopad_fn
	.uleb128 .LEHE_nopad-.LEHB_nopad
	.uleb128 0
	.uleb128 0
.LLSDACSE_nopad:

	.align	4
	.section	.note.GNU-stack,"",@progbits
