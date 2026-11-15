/* z1_step2.S —— 第二步：手写 .cfi_lsda + **GCC 同款 LSDA**，交给
 * runtime/rt_personality.c 里的 pa_personality 解析；同一份 .S 还能用
 * -Wa,--defsym,Z1_CROSSCHECK=1 编出交叉验证变体，交给真 __gxx_personality_v0。
 *
 * 与 gcc 对齐的 LSDA 形态（见 tools/lsda_ref/FINDINGS.md §0.1）：
 *   LPStart enc   = 0xff            （省略 → 用 FDE region start）
 *   TType enc     = 0x9b            （indirect|pcrel|sdata4）
 *   TType 表偏移  = uleb，指向 TType 表**末尾**（第 i 项在表尾 - i*4）
 *   call-site enc = 0x01 (uleb128)
 *   条目 1        = 被保护 call 区间 → landing pad，action 下标 1
 *   条目 2        = landing pad 里那条可能抛出的 call → lp=0，action 0
 *   action 记录   = (filter=1, next=0)：filter>0 才是 handler（catch-all 靠
 *                   TType 表里那个 **4 字节 0、无重定位** 的 null type_info 项）
 *   末尾 4 字节   = TType[1] = 0（null type_info* → catch(...)）
 * 本夹具 LSDA 恰好 20 字节，与 gcc -O1 的 catch-all 用例同形（偏移值随代码布局不同）。
 *
 * personality 指针用 0x9b + 运行期槽 z1_personality_slot（driver 里定义）：
 *   - 主测试把槽填成 pa_personality；
 *   - crosscheck 驱动填成 __gxx_personality_v0。
 * 这样**同一个 .S 的同一套 LSDA 字节**可以分别被两种 personality 解释。
 *
 * 交叉验证变体（Z1_CROSSCHECK）只保留一条抛出 call（C++ 的 throw），
 * 主测试变体额外在保护区里跑 _Unwind_Backtrace / _Unwind_ForcedUnwind 探针。
 */
	.text
	.globl	z1_step2_fn
	.type	z1_step2_fn, @function
z1_step2_fn:
	.cfi_startproc
	.cfi_personality 0x9b, z1_personality_slot
	.cfi_lsda 0x1b, .LLSDA_step2

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)		/* 保存 driver 传入的异常对象指针 */

.LEHB_step2:
.ifdef	Z1_CROSSCHECK
	call	z1_cxx_throw		/* C++ driver 提供：内部 throw 7 */
.else
	call	z1_trace_probe		/* 只走 CFI，验证 traceback 能穿过本帧 */
	movq	(%rsp), %rdi
	call	z1_forced_probe		/* _Unwind_ForcedUnwind：personality 必须 CONTINUE */
	movq	(%rsp), %rdi
	call	z1_raise_c		/* 真正抛出 */
.endif
.LEHE_step2:

	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	.globl	z1_step2_lp
z1_step2_lp:
	.cfi_def_cfa_offset 16
	/* LP 入口：rsp = 抛出点 call 之前的 rsp（不是 CFA），%rax = GR0 = exc 指针。 */
	movq	%rax, z1_step2_seen_exc(%rip)
	movl	$1, z1_step2_seen_lp(%rip)
	movl	$2, %edi		/* z1_note(tag=2, exc) */
	movq	%rax, %rsi
.LEHB_lp_step2:
	call	z1_note			/* landing pad 里可能抛出的 call：必须被 call-site 覆盖 */
.LEHE_lp_step2:
	movl	$7, %eax		/* catch-all handler 正常返回 */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z1_step2_fn, .-z1_step2_fn

/* ── LSDA（GCC catch-all 同款 20 字节形态） ───────────────────────── */
	.section	.gcc_except_table,"a",@progbits
	.align	4
.LLSDA_step2:
	.byte	0xff				/* LPStart enc = omit */
	.byte	0x9b				/* TType enc = indirect|pcrel|sdata4 */
	.uleb128 .LLSDATT_step2-.LLSDATTD_step2	/* TType 表偏移（到表末尾） */
.LLSDATTD_step2:
	.byte	0x01				/* call-site enc = uleb128 */
	.uleb128 .LLSDACSE_step2-.LLSDACSB_step2
.LLSDACSB_step2:
	.uleb128 .LEHB_step2-z1_step2_fn		/* 被保护 call 区间 → LP */
	.uleb128 .LEHE_step2-.LEHB_step2
	.uleb128 z1_step2_lp-z1_step2_fn
	.uleb128 0x1				/* action 下标 1 → 读 action 记录 */
	.uleb128 .LEHB_lp_step2-z1_step2_fn	/* LP 内 call：lp=0（覆盖住，避免 terminate） */
	.uleb128 .LEHE_lp_step2-.LEHB_lp_step2
	.uleb128 0
	.uleb128 0
.LLSDACSE_step2:
	.byte	0x01				/* action[0].filter = 1 (SLEB) → TType[1] */
	.byte	0x00				/* action[0].next   = 0 (SLEB) */
	.align	4
	.long	0				/* TType[1] = 4 字节 0，无重定位 → null type_info* → catch(...) */
.LLSDATT_step2:

	.section	.note.GNU-stack,"",@progbits
