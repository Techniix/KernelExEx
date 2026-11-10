/*
 *  KernelExEx - ring-0 support for VxDs built with clang.
 *
 *  RtlUnwind: the Visual C++ build of VKrnlEx forwards RtlUnwind to a
 *  Windows 98 VMM service whose number is not publicly documented. This is
 *  a plain implementation instead: walk the fs:0 registration chain from
 *  the top down to the target frame, calling each handler with the
 *  unwinding flag, then make the target frame the top of the chain. That is
 *  what RtlUnwind does on x86 (minus nested-unwind collision handling,
 *  which VKrnlEx does not need: its handlers do not themselves unwind).
 *
 *  __imp__RtlUnwind@16 is a pointer so that toolchain/rt/seh.S, which
 *  calls through the import slot in user mode, works unchanged in a VxD.
 */
#include <windows.h>
#include <string.h>

#ifndef STATUS_UNWIND
#define STATUS_UNWIND ((DWORD) 0xC0000027L)
#endif

typedef struct kex_reg
{
	struct kex_reg *prev;
	EXCEPTION_DISPOSITION (__cdecl *handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *);
} kex_reg;

#define KEX_CHAIN_END ((kex_reg *) -1)

static __inline kex_reg *chain_top(void)
{
	kex_reg *r;
	__asm__ __volatile__("movl %%fs:0, %0" : "=r" (r));
	return r;
}

static __inline void set_chain_top(kex_reg *r)
{
	__asm__ __volatile__("movl %0, %%fs:0" : : "r" (r) : "memory");
}

void __stdcall kex_vxd_RtlUnwind(void *target, void *ip, EXCEPTION_RECORD *rec, void *retval)
{
	EXCEPTION_RECORD local;
	kex_reg *r;
	(void) ip;
	(void) retval;

	if (!rec)
	{
		memset(&local, 0, sizeof(local));
		local.ExceptionCode = STATUS_UNWIND;
		rec = &local;
	}
	rec->ExceptionFlags |= EXCEPTION_UNWINDING;
	if (!target)
		rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

	for (r = chain_top(); r != KEX_CHAIN_END && r != (kex_reg *) target; r = chain_top())
	{
		CONTEXT ctx;
		void *dispatcher = NULL;
		memset(&ctx, 0, sizeof(ctx));
		r->handler(rec, r, &ctx, &dispatcher);
		set_chain_top(r->prev);
	}
}

void *kex_vxd_imp_RtlUnwind __asm__("__imp__RtlUnwind@16") = (void *) kex_vxd_RtlUnwind;
