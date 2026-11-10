/*
 *  KernelEx net runtime: stack probe for the i686-w64-windows-gnu target.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  The compiler calls ___chkstk_ms before allocating a frame larger than a
 *  page, with the frame size in EAX. Windows 9x, like NT, grows the stack
 *  one guard page at a time, so every page between the current stack
 *  pointer and the new frame bottom must be touched in order. All registers
 *  are preserved; the caller subtracts EAX from ESP itself.
 */
	.text
	.globl	___chkstk_ms
	.def	___chkstk_ms; .scl 2; .type 32; .endef
___chkstk_ms:
	pushl	%ecx
	pushl	%eax
	leal	12(%esp), %ecx		/* ESP of the caller before its call */
1:
	cmpl	$0x1000, %eax
	jb	2f
	subl	$0x1000, %ecx
	orl	$0, (%ecx)		/* touch the next page down */
	subl	$0x1000, %eax
	jmp	1b
2:
	subl	%eax, %ecx
	orl	$0, (%ecx)
	popl	%eax
	popl	%ecx
	ret

/*
 *  _alloca / ___chkstk: what clang and GCC call on 32-bit mingw targets for
 *  frames larger than a page. Unlike ___chkstk_ms these allocate as well:
 *  on entry EAX = bytes; on return ESP has been lowered by EAX (pages probed
 *  in order) and every register except EAX is preserved.
 */
	.globl	__alloca
	.def	__alloca; .scl 2; .type 32; .endef
	.globl	___chkstk
	.def	___chkstk; .scl 2; .type 32; .endef
__alloca:
___chkstk:
	pushl	%ecx
	leal	8(%esp), %ecx		/* caller's ESP once our return address is popped */
3:
	cmpl	$0x1000, %eax
	jb	4f
	subl	$0x1000, %ecx
	orl	$0, (%ecx)
	subl	$0x1000, %eax
	jmp	3b
4:
	subl	%eax, %ecx
	orl	$0, (%ecx)
	movl	%esp, %eax		/* EAX -> saved ECX, return address above it */
	movl	%ecx, %esp		/* the new stack pointer */
	movl	(%eax), %ecx		/* restore ECX */
	pushl	4(%eax)			/* return address onto the new stack */
	ret
