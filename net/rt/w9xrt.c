/*
 *  KernelEx net runtime: the few runtime pieces a C DLL built by clang for
 *  i686-w64-windows-gnu needs, without linking mingw-w64's runtime.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  - 64-bit integer division helpers (libgcc names)
 *  - time and gmtime_r replacements that only use Windows 95 APIs
 *
 *  Everything else (memcpy, malloc, _vsnprintf...) comes from msvcrt.dll,
 *  which every Windows 9x system has.
 */
#include <windows.h>
#include "w9xrt.h"

typedef unsigned long long u64;

/* ---------- time ---------- */

/* 100 ns intervals between 1601-01-01 and 1970-01-01 */
#define EPOCH_DIFF 116444736000000000ULL

long long w9x_time(long long *out)
{
	FILETIME ft;
	u64 t;
	long long s;

	GetSystemTimeAsFileTime(&ft);
	t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	s = (long long)((t - EPOCH_DIFF) / 10000000ULL);
	if (out)
		*out = s;
	return s;
}

/* Milliseconds from an arbitrary but monotonic origin. GetTickCount wraps
 * every 49.7 days; extend it to 64 bits under a lock. */
static CRITICAL_SECTION ms_lock;
static volatile LONG ms_lock_state;
static DWORD ms_last;
static u64 ms_high;

long long w9x_ms_time(void)
{
	DWORD now;
	u64 result;

	if (InterlockedCompareExchange((LONG *)&ms_lock_state, 1, 0) == 0)
	{
		InitializeCriticalSection(&ms_lock);
		InterlockedExchange((LONG *)&ms_lock_state, 2);
	}
	while (ms_lock_state != 2)
		Sleep(0);

	EnterCriticalSection(&ms_lock);
	now = GetTickCount();
	if (now < ms_last)
		ms_high += (u64)1 << 32;
	ms_last = now;
	result = ms_high | now;
	LeaveCriticalSection(&ms_lock);
	return (long long)result;
}

#include "w9xrt_gmtime.inc"

/* snprintf that always terminates and returns the C99 meaning closely
 * enough for mbed TLS (which only checks for truncation via ret >= n or
 * ret < 0). msvcrt's _vsnprintf returns -1 on truncation. */
int __cdecl _vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

int w9x_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
	int r;

	if (n == 0)
		return -1;
	r = _vsnprintf(s, n, fmt, ap);
	if (r < 0 || (size_t)r >= n)
	{
		s[n - 1] = '\0';
		return (int)n; /* reported as truncated */
	}
	return r;
}

int w9x_snprintf(char *s, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = w9x_vsnprintf(s, n, fmt, ap);
	va_end(ap);
	return r;
}
