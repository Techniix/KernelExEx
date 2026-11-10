/*
 * Structured exception handling for clang builds of KernelEx.
 *
 * clang only implements __try/__except for the MSVC ABI; with the GNU ABI
 * used here it parses the block and then silently drops the handler. The
 * macros in msvc_compat.h turn  __try { } __except (EXCEPTION_EXECUTE_HANDLER) { }
 * into a setjmp-style pair built on these two routines.
 *
 * struct kex_seh_frame {
 *      void *prev;      0   EXCEPTION_REGISTRATION: must be first
 *      void *handler;   4
 *      ebx, esi, edi, ebp, esp, eip   8..28
 *      int once;        32  used by the macro only
 * };
 */
	.text
	.globl	_kex_seh_enter
	.def	_kex_seh_enter;	.scl 2;	.type 32;	.endef
/* int __cdecl kex_seh_enter(struct kex_seh_frame *f)
 * Returns 0 after installing the frame, 1 when an exception unwinds to it. */
_kex_seh_enter:
	movl	4(%esp), %eax
	movl	%ebx, 8(%eax)
	movl	%esi, 12(%eax)
	movl	%edi, 16(%eax)
	movl	%ebp, 20(%eax)
	leal	4(%esp), %ecx		/* esp as the caller sees it after we return */
	movl	%ecx, 24(%eax)
	movl	(%esp), %ecx
	movl	%ecx, 28(%eax)
	movl	$_kex_seh_handler, 4(%eax)
	movl	%fs:0, %ecx
	movl	%ecx, 0(%eax)
	movl	%eax, %fs:0
	xorl	%eax, %eax
	ret

/* EXCEPTION_DISPOSITION __cdecl handler(EXCEPTION_RECORD *rec, void *frame,
 *                                       CONTEXT *ctx, void *dispatch) */
_kex_seh_handler:
	movl	4(%esp), %eax
	testl	$6, 4(%eax)		/* EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND */
	jnz	1f
	movl	8(%esp), %edx
	pushl	$0			/* ReturnValue */
	pushl	%eax			/* ExceptionRecord */
	pushl	$2f			/* TargetIp */
	pushl	%edx			/* TargetFrame */
	call	*__imp__RtlUnwind@16	/* does not preserve ebx/esi/edi */
2:
	movl	8(%esp), %edx		/* our arguments are still on the stack */
	movl	8(%edx), %ebx
	movl	12(%edx), %esi
	movl	16(%edx), %edi
	movl	20(%edx), %ebp
	movl	28(%edx), %ecx
	movl	24(%edx), %esp		/* fs:0 is this frame again; the macro's cleanup unlinks it */
	cld
	movl	$1, %eax
	jmp	*%ecx
1:
	movl	$1, %eax		/* ExceptionContinueSearch */
	ret
