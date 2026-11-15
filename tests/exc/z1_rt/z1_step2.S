/* z1_step2.S —— 第二步：手写 .cfi_lsda + .gcc_except_table，交给
 * runtime/rt_personality.c 里的 pa_personality 真正解析。
 *
 * 和第一步的区别：
 *   - 不再有 stub_personality：CIE 里的 personality 直接是 pa_personality；
 *   - FDE 里挂了 .cfi_lsda，LSDA 里只描述「保护区区间 + landing pad 地址 +
 *     action 下标」，personality 按表把 IP 设到 LSDA 给出的地址。
 *   - landing pad 地址、保护区范围全部来自表格：如果 pa_personality 不解析
 *     LSDA 而是硬编码，这个夹具不可能跑通（第三步还有两层帧的夹具）。
 *
 * LSDA 布局（Itanium ABI，GCC 14 实测对照见 README）：
 *   LPStart encoding = 0xff（省略 → 用 FDE 的 region start = 函数起点）
 *   TType encoding   = 0xff（省略 → 没有类型表；我们不做类型匹配）
 *   call-site enc    = 0x01（uleb128，偏移都相对 LPStart）
 *   call-site 表长   = uleb128
 *   条目：start / len / lp / action 四个 uleb128
 *   action 表：catch-all 条目 (filter=0, next=0)
 *
 * 保护区里先 call z1_trace_probe（内部走 _Unwind_Backtrace）和
 * z1_forced_probe（内部走 _Unwind_ForcedUnwind），最后 call 抛出入口：
 * 一条 LSDA 条目覆盖连续三条 call，顺手验证 _UA_FORCE_UNWIND 时 personality
 * 只穿过、不安装 landing pad（_Unwind_Backtrace 本身不调用 personality，
 * 真正会带 FORCE_UNWIND 调 personality 的是 _Unwind_ForcedUnwind）。
 */
	.text
	.globl	z1_step2_fn
	.type	z1_step2_fn, @function
z1_step2_fn:
	.cfi_startproc
	.cfi_personality 0x1b, pa_personality
	.cfi_lsda 0x1b, .LLSDA_step2

	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)			/* 保存 driver 传入的 exception 指针（保护区里第一个 call 可能 clobber %rdi） */

.LEHB_step2:
	call	z1_trace_probe		/* 只走 CFI，验证 traceback 能穿过本帧 */
	movq	(%rsp), %rdi
	call	z1_forced_probe		/* _Unwind_ForcedUnwind：personality 必须 CONTINUE */
	movq	(%rsp), %rdi
	call	z1_raise_c
.LEHE_step2:

	/* 正常路径（本夹具不会走到）。 */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	.globl	z1_step2_lp
z1_step2_lp:
	.cfi_def_cfa_offset 16
	/* 入口：rsp = 调用点 rsp（= 函数入口 rsp - 8，返回地址在 rsp+8），
	   %rax = GR0 = exception 指针。 */
	movq	%rax, z1_step2_seen_exc(%rip)
	movl	$1, z1_step2_seen_lp(%rip)
	movl	$7, %eax
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
	.size	z1_step2_fn, .-z1_step2_fn

/* ── LSDA ────────────────────────────────────────────────────────── */
	.section	.gcc_except_table,"a",@progbits
	.align	4
.LLSDA_step2:
	.byte	0xff				/* LPStart encoding = omit */
	.byte	0xff				/* TType encoding = omit */
	.byte	0x01				/* call-site 表编码 = uleb128 */
	.uleb128 .LLSDACSE_step2-.LLSDACSB_step2	/* call-site 表字节长度 */
.LLSDACSB_step2:
	.uleb128 .LEHB_step2-z1_step2_fn		/* 保护区起点（相对 LPStart） */
	.uleb128 .LEHE_step2-.LEHB_step2		/* 保护区长度 */
	.uleb128 z1_step2_lp-z1_step2_fn		/* landing pad 偏移 */
	.uleb128 1				/* action 下标：1 = 指向 action 表第 0 项 */
.LLSDACSE_step2:
	/* action 表：type filter = 0（catch-all），next action offset = 0（链结束） */
	.byte	0x00
	.byte	0x00
	.align	4

	.section	.note.GNU-stack,"",@progbits
