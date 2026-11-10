/*
 *  KernelEx kextls: KernelEx API library interface.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Listed last in CORE.INI "contents=", so these entries override earlier
 *  API libraries. In particular ADVAPI32.SystemFunction036 (RtlGenRandom)
 *  replaces kexbases' old implementation, which was the non-cryptographic
 *  KISS generator seeded from the tick count and kept in shared memory.
 *
 *  kextls.dll is a *non-shared* API library (image base below 2 GB), so the
 *  KernelEx resolver loads it into each process that binds to one of these
 *  functions, and its generator state is private to that process.
 *
 *  Table rules (core/apiconf.cpp): names sorted by strcmp, one table per
 *  target DLL, terminated by an entry with a NULL target.
 */
#include <windows.h>
#include <errno.h>
#include "kextls.h"

/* The KernelEx SDK structures (common/kexcoresdk.h), restated here because
 * that header is C++ and MSVC-specific. Layout must stay identical. */
typedef struct
{
	const char *name;
	unsigned long addr;
} apilib_named_api;

typedef struct
{
	unsigned short ord;
	unsigned long addr;
} apilib_unnamed_api;

typedef struct
{
	const char *target_library;
	const apilib_named_api *named_apis;
	int named_apis_count;
	const apilib_unnamed_api *ordinal_apis;
	int ordinal_apis_count;
} apilib_api_table;

/* ADVAPI32!SystemFunction036 a.k.a. RtlGenRandom */
static BOOLEAN WINAPI SystemFunction036_kex(PVOID buffer, ULONG length)
{
	if (!buffer && length)
		return FALSE;
	return kextls_random(buffer, length) == KEXTLS_OK;
}

/* MSVCRT!rand_s (added by Visual C++ 2005; Windows 98's msvcrt lacks it) */
static int __cdecl rand_s_kex(unsigned int *value)
{
	if (!value)
		return EINVAL;
	if (kextls_random(value, sizeof(*value)) != KEXTLS_OK)
	{
		*value = 0;
		return EINVAL;
	}
	return 0;
}

static const apilib_named_api advapi32_apis[] = {
	{ "SystemFunction036", (unsigned long)SystemFunction036_kex },
};

static const apilib_named_api msvcrt_apis[] = {
	{ "rand_s", (unsigned long)rand_s_kex },
};

static const apilib_api_table api_tables[] = {
	{ "ADVAPI32.DLL", advapi32_apis, sizeof(advapi32_apis) / sizeof(advapi32_apis[0]), NULL, 0 },
	{ "MSVCRT.DLL", msvcrt_apis, sizeof(msvcrt_apis) / sizeof(msvcrt_apis[0]), NULL, 0 },
	{ NULL, NULL, 0, NULL, 0 }
};

__declspec(dllexport) const apilib_api_table *get_api_table(void)
{
	return api_tables;
}
