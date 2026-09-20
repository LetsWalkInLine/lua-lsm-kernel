/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/export.h>
#include <linux/linkage.h>

.section .noinstr.text, "ax"

/*
 * int setjmp(struct label_t *label);
 */
SYM_FUNC_START(setjmp)
	/*
	 * Only save registers that must be preserved across function
	 * calls according to the ABI (%rbx, %rsp, %rbp, %r12-%r15)
	 * and %rip.
	 */
	movq		%rdi, %rax
	movq		%rbx,   (%rax)
	movq		%rsp,  8(%rax)
	movq		%rbp, 16(%rax)
	movq		%r12, 24(%rax)
	movq		%r13, 32(%rax)
	movq		%r14, 40(%rax)
	movq		%r15, 48(%rax)
	movq		(%rsp), %rdx
	movq		%rdx, 56(%rax)
	xorq		%rax, %rax
	RET
SYM_FUNC_END(setjmp)
EXPORT_SYMBOL(setjmp)

/*
 * void longjmp(struct label_t *label, int val);
 */
SYM_FUNC_START(longjmp)
#ifdef CONFIG_KASAN_STACK
	/* The skipped frames will not run their KASAN stack cleanup. */
	pushq		%rdi
	pushq		%rsi
	movq		8(%rdi), %rdi
	call		kasan_unpoison_task_stack_below
	popq		%rsi
	popq		%rdi
#endif
	movq		%rdi, %rax
	movq		  (%rax), %rbx
	movq		 8(%rax), %rsp
	movq		16(%rax), %rbp
	movq		24(%rax), %r12
	movq		32(%rax), %r13
	movq		40(%rax), %r14
	movq		48(%rax), %r15
	movq		56(%rax), %rdx
	movq		%rdx, (%rsp)
	movq		%rsi, %rax
	RET
SYM_FUNC_END(longjmp)
EXPORT_SYMBOL(longjmp)
