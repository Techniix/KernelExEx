/* Forced include for sources written for Visual C++ 6, when built with clang. */
#ifndef KEX_MSVC_COMPAT_H
#define KEX_MSVC_COMPAT_H

/* The Platform SDK defined min/max as macros in C++ too; mingw-w64 does not. */
#ifdef __cplusplus
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#endif

/*
 * __try / __except. See toolchain/rt/seh.S for why this is needed.
 * Supported form:  __try { ... } __except (EXCEPTION_EXECUTE_HANDLER) { ... }
 * Any other filter fails at link time (kex_seh_unsupported_filter).
 * As with setjmp, a local changed inside the __try block and read in the
 * handler should be volatile. break/continue must not leave the block.
 */
struct kex_seh_frame
{
	void *prev;
	void *handler;
	unsigned long ebx, esi, edi, ebp, esp, eip;
	int once;
};

#ifdef __cplusplus
extern "C" {
#endif
int __cdecl kex_seh_enter(struct kex_seh_frame *f) __attribute__((returns_twice));
void kex_seh_unsupported_filter(void);
#ifdef __cplusplus
}
#endif

static __inline__ __attribute__((always_inline)) void kex_seh_leave(struct kex_seh_frame *f)
{
	__asm__ __volatile__("movl %0, %%fs:0" : : "r" (f->prev));
}

#define __try \
	for (struct kex_seh_frame __kex_seh __attribute__((cleanup(kex_seh_leave))) = { 0, 0, 0, 0, 0, 0, 0, 0, 1 }; \
	     __kex_seh.once; __kex_seh.once = 0) \
		if (kex_seh_enter(&__kex_seh) == 0)

#define __except(filter) \
		else if ((filter) != 1) kex_seh_unsupported_filter(); else

#endif
