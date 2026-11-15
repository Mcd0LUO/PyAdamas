/* z2_threelayer.S —— Z2-b1 方案 G 的夹具：被跳过的中间帧的 RC 槽在
 * personality 的 phase 2、安装 landing pad 之前被批量释放。
 *
 * 设计文档：docs/exc-frame-cleanup.md §5（方案 G）。
 *
 * 栈形状（由内到外，括号里是帧记录里的槽）：
 *   z2_inner           普通帧：1 个 owned 堆槽            → 展开时释放
 *   z2_mid             普通帧：3 个槽 = owned 堆槽 + owned unknown(bit0) 槽
 *                      + 只给 GC 扫的借来槽(bit1)        → 前两个释放、bit1 不释放
 *   z2_outer           有 personality + catch-all LSDA 的 handler 帧：
 *                      1 个 owned 堆槽                    → 保留，由正常出口释放
 *   z2_norec_fn        同形 handler 帧，但**不压帧记录**（证明等价性证明的另一半：
 *                      链上第一条 cfa > target 的记录才是停止点，此时 top = NULL）
 *
 * 帧记录布局完全按 codegen 的口径手摆（src/codegen_impl.h 的 rec_off、
 * src/codegen_block.cpp 的压帧序列，6 个字头 + 槽地址×n），并由 driver.c 的
 * _Static_assert/offsetof 交叉校验，防止两边漂移：
 *   word0 prev / word1 n / word2 scan_base / word3 scan_words /
 *   word4 scan_mask / word5 cfa / word6.. 槽地址（低两位标志）
 * cfa = 正常路径 rsp + FRAME + 8（与 .cfi_def_cfa 同一口径）。
 *
 * landing pad 入口：%rax = 展开包装指针，%rsp = 抛出点那条 call 之前的 rsp
 * （= 函数体 rsp；见 tests/exc/z1_rt/README.md 坑 6），所以 LP 可以按 rsp 相对
 * 偏移取本帧局部变量、直接 call（此时 rsp 16 字节对齐）、最后照抄正常 epilogue。
 */
	.text

/* ── 外层 handler 帧（有记录，cfa == 释放目标 → 记录与槽都要保留） ────
 * FRAME = 104（≡ 8 mod 16，保证 call 时 rsp 16 对齐）
 *   rsp+0   exc 形参暂存        rsp+8   outer_obj（owned 槽）
 *   rsp+16  mid_a               rsp+24  mid_b
 *   rsp+32  scan_c              rsp+40  inner_obj
 *   rsp+48  帧记录（7 个字）    CFA = rsp+112                      */
	.globl	z2_outer
	.type	z2_outer, @function
z2_outer:
	.cfi_startproc
	.cfi_personality 0x9b, z2_personality_slot
	.cfi_lsda 0x1b, .LLSDA_outer
	subq	$104, %rsp
	.cfi_def_cfa_offset 112
	movq	%rdi, 0(%rsp)
	movq	%rsi, 8(%rsp)
	movq	%rdx, 16(%rsp)
	movq	%rcx, 24(%rsp)
	movq	%r8, 32(%rsp)
	movq	%r9, 40(%rsp)
	movq	%rsp, z2_dbg_rsp_outer(%rip)
	/* 压帧记录 */
	movq	pa_frame_top(%rip), %rax
	movq	%rax, 48(%rsp)			/* prev */
	movq	$1, 56(%rsp)			/* n = 1 */
	movq	$0, 64(%rsp)			/* scan_base */
	movq	$0, 72(%rsp)			/* scan_words */
	movq	$0, 80(%rsp)			/* scan_mask */
	leaq	112(%rsp), %rax			/* CFA */
	movq	%rax, 88(%rsp)
	leaq	8(%rsp), %rax			/* slots[0] = &outer_obj */
	movq	%rax, 96(%rsp)
	leaq	48(%rsp), %rax
	movq	%rax, pa_frame_top(%rip)
.LEHB_outer:
	movq	0(%rsp), %rdi
	movq	16(%rsp), %rsi
	movq	24(%rsp), %rdx
	movq	32(%rsp), %rcx
	movq	40(%rsp), %r8
	call	z2_mid
.LEHE_outer:
	/* 走到这里 = 没人接（未捕获路径）。正常出口：先弹记录再释放本帧槽。 */
	movq	48(%rsp), %rcx
	movq	%rcx, pa_frame_top(%rip)
	movl	%eax, 0(%rsp)
	movq	8(%rsp), %rdi
	call	z2_release_one_owned
	movl	0(%rsp), %eax
	addq	$104, %rsp
	.cfi_def_cfa_offset 8
	ret

	.globl	z2_outer_lp
	.type	z2_outer_lp, @function
z2_outer_lp:
	.cfi_def_cfa_offset 112
	movq	%rsp, z2_dbg_rsp_lp(%rip)
	/* 入口 rsp = 函数体 rsp；%rax = 展开包装。rsp+0 是 exc 形参暂存位，
	   函数马上要结束，可以当临时槽用。 */
	movq	%rax, 0(%rsp)
	leaq	48(%rsp), %rsi			/* 本帧记录地址，给 driver 比对 */
	movq	8(%rsp), %rdx			/* 本帧 owned 槽的对象 */
	movq	0(%rsp), %rdi
	call	z2_outer_handler		/* landing + 断言 + 释放本帧槽 + 收尾 */
	movq	48(%rsp), %rcx
	movq	%rcx, pa_frame_top(%rip)	/* 正常 epilogue 的弹记录 */
	movl	$100, %eax
	addq	$104, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z2_outer, .-z2_outer

/* ── 中间帧（有记录，3 个槽） ─────────────────────────────────────
 * FRAME = 104，帧记录在 rsp+32（9 个字），CFA = rsp+112
 *   rsp+0  mid_a（owned）   rsp+8  mid_b（owned|bit0）   rsp+16  scan_c（bit1） */
	.globl	z2_mid
	.type	z2_mid, @function
z2_mid:
	.cfi_startproc
	subq	$104, %rsp
	.cfi_def_cfa_offset 112
	movq	%rsi, 0(%rsp)
	movq	%rdx, 8(%rsp)
	movq	%rcx, 16(%rsp)
	movq	%rsp, z2_dbg_rsp_mid(%rip)
#ifndef Z2_NEGATIVE
	movq	pa_frame_top(%rip), %rax
	movq	%rax, 32(%rsp)			/* prev */
	movq	$3, 40(%rsp)			/* n = 3 */
	movq	$0, 48(%rsp)
	movq	$0, 56(%rsp)
	movq	$0, 64(%rsp)
	leaq	112(%rsp), %rax			/* CFA */
	movq	%rax, 72(%rsp)
	leaq	0(%rsp), %rax			/* slots[0] = &mid_a（bit0=0） */
	movq	%rax, 80(%rsp)
	leaq	8(%rsp), %rax			/* slots[1] = &mid_b | bit0 */
	orq	$1, %rax
	movq	%rax, 88(%rsp)
	leaq	16(%rsp), %rax			/* slots[2] = &scan_c | bit1 */
	orq	$2, %rax
	movq	%rax, 96(%rsp)
	leaq	32(%rsp), %rax
	movq	%rax, pa_frame_top(%rip)
#endif
	movq	%r8, %rsi			/* inner_obj */
	call	z2_inner			/* rdi = exc */
	/* 只有“没人接”才会回到这里：弹记录 + 按正常出口释放自己的槽。 */
	movq	32(%rsp), %rcx
	movq	%rcx, pa_frame_top(%rip)
	movl	%eax, 24(%rsp)			/* padding 槽当临时 */
	movq	0(%rsp), %rdi
	call	z2_release_one_owned
	movq	8(%rsp), %rdi
	call	z2_release_one_owned
	movq	16(%rsp), %rdi
	call	z2_release_one_owned
	movl	24(%rsp), %eax
	addq	$104, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z2_mid, .-z2_mid

/* ── 内层帧（有记录，1 个 owned 槽；抛出点） ──────────────────────
 * FRAME = 72，帧记录在 rsp+16（7 个字），CFA = rsp+80 */
	.globl	z2_inner
	.type	z2_inner, @function
z2_inner:
	.cfi_startproc
	subq	$72, %rsp
	.cfi_def_cfa_offset 80
	movq	%rsi, 0(%rsp)			/* inner_obj */
	movq	%rsp, z2_dbg_rsp_inner(%rip)
#ifndef Z2_NEGATIVE
	movq	pa_frame_top(%rip), %rax
	movq	%rax, 16(%rsp)
	movq	$1, 24(%rsp)
	movq	$0, 32(%rsp)
	movq	$0, 40(%rsp)
	movq	$0, 48(%rsp)
	leaq	80(%rsp), %rax
	movq	%rax, 56(%rsp)
	leaq	0(%rsp), %rax
	movq	%rax, 64(%rsp)
	leaq	16(%rsp), %rax
	movq	%rax, pa_frame_top(%rip)
#endif
	call	pa_eh_throw			/* rdi = exc；成功时不返回 */
	/* 返回 = 没人接：弹记录、释放本帧槽、把返回值带回上层。 */
	movl	%eax, 8(%rsp)
	movq	16(%rsp), %rcx
	movq	%rcx, pa_frame_top(%rip)
	movq	0(%rsp), %rdi
	call	z2_release_one_owned
	movl	8(%rsp), %eax
	addq	$72, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z2_inner, .-z2_inner

/* ── 不压帧记录的 handler 帧（等价性另一半：top 应该变成 NULL） ────
 * FRAME = 56；没有记录，所以 phase 2 释放后链上只剩“更外层”的记录（本夹具里
 * 没有 → NULL）。这验证 pa_frame_unwind_to_cfa 的停止条件不是“handler 帧还有
 * 记录”，而是“cfa 已经到 handler 帧”。 */
	.globl	z2_norec_fn
	.type	z2_norec_fn, @function
z2_norec_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z2_personality_slot
	.cfi_lsda 0x1b, .LLSDA_norec
	subq	$56, %rsp
	.cfi_def_cfa_offset 64
	movq	%rdi, 0(%rsp)
	movq	%rsi, 8(%rsp)
	movq	%rdx, 16(%rsp)
	movq	%rcx, 24(%rsp)
	movq	%r8, 32(%rsp)
.LEHB_norec:
	movq	0(%rsp), %rdi
	movq	8(%rsp), %rsi
	movq	16(%rsp), %rdx
	movq	24(%rsp), %rcx
	movq	32(%rsp), %r8
	call	z2_mid
.LEHE_norec:
	addq	$56, %rsp
	.cfi_def_cfa_offset 8
	ret

	.globl	z2_norec_lp
	.type	z2_norec_lp, @function
z2_norec_lp:
	.cfi_def_cfa_offset 64
	movq	%rax, %rdi
	call	z2_norec_handler		/* landing + 断言 + 收尾 */
	movl	$200, %eax
	addq	$56, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z2_norec_fn, .-z2_norec_fn

/* ── 未捕获路径：抛出去没人接，_Unwind_RaiseException 返回 END_OF_STACK ── */
	.globl	z2_throw_fn
	.type	z2_throw_fn, @function
z2_throw_fn:
	.cfi_startproc
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	call	pa_eh_throw
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z2_throw_fn, .-z2_throw_fn

/* ── 探针：各帧实际 rsp（driver 在 Z2_DEBUG=1 下打印，用来核对 CFA 口径） ── */
	.data
	.globl	z2_dbg_rsp_outer, z2_dbg_rsp_mid, z2_dbg_rsp_inner, z2_dbg_rsp_lp
	.align	8
z2_dbg_rsp_outer:	.quad	0
z2_dbg_rsp_mid:		.quad	0
z2_dbg_rsp_inner:	.quad	0
z2_dbg_rsp_lp:		.quad	0
	.text

/* ── LSDA：GCC 同款 20 字节 catch-all（与 tests/exc/z1_rt/z1_step2.S 同形） ── */
	.section	.gcc_except_table,"a",@progbits
	.align	4

.LLSDA_outer:
	.byte	0xff				/* LPStart 编码：省略（= 函数起点） */
	.byte	0x9b				/* TType：indirect|pcrel|sdata4 */
	.uleb128 .LLSDATT_outer-.LLSDATTD_outer
.LLSDATTD_outer:
	.byte	0x01				/* call-site 编码：uleb128 */
	.uleb128 .LLSDACSE_outer-.LLSDACSB_outer
.LLSDACSB_outer:
	.uleb128 .LEHB_outer-z2_outer
	.uleb128 .LEHE_outer-.LEHB_outer
	.uleb128 z2_outer_lp-z2_outer
	.uleb128 0x1				/* action 下标 1 */
.LLSDACSE_outer:
	.byte	0x01				/* action[1]：filter = 1（>0 = handler） */
	.byte	0x00				/* next = 0 */
	.align	4
	.long	0				/* TType[1] = null type_info* ⇒ catch-all */
.LLSDATT_outer:

.LLSDA_norec:
	.byte	0xff
	.byte	0x9b
	.uleb128 .LLSDATT_norec-.LLSDATTD_norec
.LLSDATTD_norec:
	.byte	0x01
	.uleb128 .LLSDACSE_norec-.LLSDACSB_norec
.LLSDACSB_norec:
	.uleb128 .LEHB_norec-z2_norec_fn
	.uleb128 .LEHE_norec-.LEHB_norec
	.uleb128 z2_norec_lp-z2_norec_fn
	.uleb128 0x1
.LLSDACSE_norec:
	.byte	0x01
	.byte	0x00
	.align	4
	.long	0
.LLSDATT_norec:

	.section	.note.GNU-stack,"",@progbits
