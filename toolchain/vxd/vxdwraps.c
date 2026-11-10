/*
 *  KernelExEx - C-callable VMM services for VxDs built with clang.
 *
 *  Each stub is a dynamic-link jump (VMMJmp): the VMM rewrites it into
 *  JMP [service], so the service sees the caller's return address and
 *  arguments exactly as if it had been called directly. This is how the
 *  DDK's wrapper library does it.
 */
#include "include/vmm.h"
#include <stdarg.h>
#include <stdio.h>

__declspec(naked) void * __cdecl _HeapAllocate(ULONG nbytes, ULONG flags)
{
	VMMJmp(_HeapAllocate)
}

__declspec(naked) ULONG __cdecl _HeapFree(void *hAddress, ULONG flags)
{
	VMMJmp(_HeapFree)
}

__declspec(naked) ULONG __cdecl _HeapGetSize(void *hAddress, ULONG flags)
{
	VMMJmp(_HeapGetSize)
}

__declspec(naked) ULONG __cdecl _LinPageLock(ULONG HLinPgNum, ULONG nPages, ULONG flags)
{
	VMMJmp(_LinPageLock)
}

__declspec(naked) void __cdecl _Debug_Printf_Service(const char *fmt, ...)
{
	VMMJmp(_Debug_Printf_Service)
}

/* debug builds only (vxd/patch.cpp); formats with kexcrt */
ULONG __cdecl _Vsprintf(char *out, char *fmt, va_list args)
{
	return (ULONG) vsprintf(out, fmt, args);
}
