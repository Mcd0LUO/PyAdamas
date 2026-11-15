/* z1_step1.S —— 第一步：不依赖 LSDA 的 plumbing 验证
 *
 * 目标（全部用最笨的办法先证明，不解析任何表）：
 *   1. _Unwind_RaiseException 会走到本函数这一帧（.cfi_personality 生效）；
 *   2. 我们的 personality 在 phase 1 / phase 2 都被调用；
 *   3. personality 返回 _URC_INSTALL_CONTEXT 后，_Unwind_SetIP 生效，
 *      控制真的跳到本文件里的 landing pad；
 *   4. landing pad 里 GR0（x86-64 上是 %rax）确实是 _Unwind_SetGR 传进来的
 *      exception 指针。
 *
 * 这一步刻意没有 .cfi_lsda：personality 里硬编码 landing pad 地址，
 * 只用 IP 是否落在 [z1_step1_fn, z1_step1_end) 判断“是不是自己的帧”。
 * 这样把「.cfi_personality 能不能挂上 + 安装上下文能不能生效」和
 * 「LSDA 解码对不对」两件事解耦，出问题时能立刻分清是哪个环节。
 *
 * 关键实测结论（README 里有汇总）：
 *   展开器把 IP 设成 landing pad 时，rsp 被恢复成抛出点那条 call 之前的值
 *   （不是 CFA）。本函数只 subq $8，所以 landing pad 入口 rsp 恰好是调用点
 *   rsp（= 函数入口 rsp - 8），返回地址在 rsp+8；正常返回路径的
 *   addq $8,%rsp; ret 在 landing pad 里同样成立。
 */
	.text
	.globl	z1_step1_fn
	.type	z1_step1_fn, @function
z1_step1_fn:
	.cfi_startproc
	/* 0x1b = DW_EH_PE_pcrel | DW_EH_PE_sdata4：CIE 里直接放 4 字节
	   PC 相对偏移，省掉 GCC 常规的 DW.ref.* 间接层（那需要 0x9b）。
	   personality 和本函数链接进同一个可执行文件，所以直接 pcrel 够用。 */
	.cfi_personality 0x1b, z1_stub_personality

	/* 只有 8 字节的占位栈槽：CFA 从函数入口的 rsp+8 变成 rsp+16。
	   保护区内的 call 返回地址就在 CFA-8，跟标准帧完全一致。 */
	subq	$8, %rsp
	.cfi_def_cfa_offset 16
	movq	%rdi, (%rsp)			/* 保存 driver 传入的 exception 指针（保护区里第一个 call 可能 clobber %rdi） */

.LEHB0:
	movq	(%rsp), %rdi
	call	z1_raise_c
.LEHE0:

	/* 正常路径：本夹具一旦走进保护区就一定会抛出，这里只是让 CFI
	   有一块「正常返回」的范围，保持和编译器产物同构。 */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	xorl	%eax, %eax
	ret

	/* landing pad。进入时展开器已经做过两件事：
	     - rsp 恢复成抛出点 call 之前的值（本函数里 = 函数入口 rsp - 8）；
	     - 被调用者保存寄存器恢复成调用点的值。
	   所以这里可以照抄正常返回路径的 addq $8,%rsp; ret。
	   紧跟 LP 的标签补一条 .cfi_def_cfa_offset 16：展开器安装上下文时
	   rsp 处于“调用点”状态，CFA 仍是 rsp+16；这条 CFI 让 debugger /
	   二次展开（_Unwind_Resume）从 LP 走的时候也算得对。 */
	.globl	z1_step1_lp
z1_step1_lp:
	.cfi_def_cfa_offset 16
	movq	%rax, z1_step1_seen_exc(%rip)	/* GR0 = exception 指针 */
	movl	$1, z1_step1_seen_lp(%rip)
	movl	$42, %eax			/* 返回 42 表示“从 landing pad 回来” */
	addq	$8, %rsp
	.cfi_def_cfa_offset 8
	ret

	.globl	z1_step1_end
z1_step1_end:
	.cfi_endproc
	.size	z1_step1_fn, .-z1_step1_fn

	.section	.note.GNU-stack,"",@progbits
