/*
 *  KernelEx kexrt: Vista/7-era KERNEL32 functions for Windows 9x.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  SRW locks, condition variables and INIT_ONCE are thin wrappers over
 *  sync_core.c. FLS is built on TLS plus callbacks run from
 *  DllMain(DLL_THREAD_DETACH). Behaviour follows the Windows documentation
 *  and Wine (dlls/kernelbase, dlls/ntdll; LGPL, consulted not copied).
 */
#include <windows.h>
#include "sync_core.h"
#include "kexrt.h"

/* ---------------- SRW locks ---------------- */

static void WINAPI k_InitializeSRWLock(PVOID *lock) { *lock = NULL; }
static void WINAPI k_AcquireSRWLockExclusive(PVOID *lock) { kx_srw_acquire_exclusive((volatile uintptr_t *)lock); }
static void WINAPI k_AcquireSRWLockShared(PVOID *lock) { kx_srw_acquire_shared((volatile uintptr_t *)lock); }
static void WINAPI k_ReleaseSRWLockExclusive(PVOID *lock) { kx_srw_release_exclusive((volatile uintptr_t *)lock); }
static void WINAPI k_ReleaseSRWLockShared(PVOID *lock) { kx_srw_release_shared((volatile uintptr_t *)lock); }
static BOOLEAN WINAPI k_TryAcquireSRWLockExclusive(PVOID *lock) { return (BOOLEAN)kx_srw_try_exclusive((volatile uintptr_t *)lock); }
static BOOLEAN WINAPI k_TryAcquireSRWLockShared(PVOID *lock) { return (BOOLEAN)kx_srw_try_shared((volatile uintptr_t *)lock); }

/* ---------------- condition variables ---------------- */

static void cs_release(void *p) { LeaveCriticalSection((CRITICAL_SECTION *)p); }
static void cs_acquire(void *p) { EnterCriticalSection((CRITICAL_SECTION *)p); }
static void srw_rel_x(void *p) { kx_srw_release_exclusive((volatile uintptr_t *)p); }
static void srw_acq_x(void *p) { kx_srw_acquire_exclusive((volatile uintptr_t *)p); }
static void srw_rel_s(void *p) { kx_srw_release_shared((volatile uintptr_t *)p); }
static void srw_acq_s(void *p) { kx_srw_acquire_shared((volatile uintptr_t *)p); }

static void WINAPI k_InitializeConditionVariable(PVOID *cv) { *cv = NULL; }

static BOOL WINAPI k_SleepConditionVariableCS(PVOID *cv, CRITICAL_SECTION *cs, DWORD ms)
{
	if (kx_cv_sleep((volatile uintptr_t *)cv, cs_release, cs_acquire, cs, ms))
		return TRUE;
	SetLastError(ERROR_TIMEOUT);
	return FALSE;
}

#define CONDITION_VARIABLE_LOCKMODE_SHARED_ 0x1

static BOOL WINAPI k_SleepConditionVariableSRW(PVOID *cv, PVOID *srw, DWORD ms, ULONG flags)
{
	int shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED_) != 0;
	if (kx_cv_sleep((volatile uintptr_t *)cv, shared ? srw_rel_s : srw_rel_x, shared ? srw_acq_s : srw_acq_x, srw, ms))
		return TRUE;
	SetLastError(ERROR_TIMEOUT);
	return FALSE;
}

static void WINAPI k_WakeConditionVariable(PVOID *cv) { kx_cv_wake((volatile uintptr_t *)cv, 0); }
static void WINAPI k_WakeAllConditionVariable(PVOID *cv) { kx_cv_wake((volatile uintptr_t *)cv, 1); }

/* ---------------- one-time initialisation ---------------- */

#define INIT_ONCE_CHECK_ONLY_ 0x1
#define INIT_ONCE_ASYNC_ 0x2
#define INIT_ONCE_INIT_FAILED_ 0x4

typedef BOOL (WINAPI *init_once_fn)(PVOID *once, PVOID param, PVOID *ctx);

static void WINAPI k_InitOnceInitialize(PVOID *once) { *once = NULL; }

static BOOL WINAPI k_InitOnceBeginInitialize(PVOID *once, DWORD flags, BOOL *pending, PVOID *ctx)
{
	uintptr_t c = 0;
	int r;

	if ((flags & ~INIT_ONCE_CHECK_ONLY_) || !pending)
	{
		/* INIT_ONCE_ASYNC is not supported in this implementation */
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	r = kx_once_begin((volatile uintptr_t *)once, flags & INIT_ONCE_CHECK_ONLY_, &c);
	if (r < 0)
	{
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	*pending = r == 1;
	if (r == 0 && ctx)
		*ctx = (PVOID)c;
	return TRUE;
}

static BOOL WINAPI k_InitOnceComplete(PVOID *once, DWORD flags, PVOID ctx)
{
	int ok = !(flags & INIT_ONCE_INIT_FAILED_);
	if ((flags & ~(INIT_ONCE_INIT_FAILED_ | INIT_ONCE_ASYNC_)) || (flags & INIT_ONCE_ASYNC_) ||
	    (!ok && ctx) || !kx_once_complete((volatile uintptr_t *)once, ok, (uintptr_t)ctx))
	{
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return TRUE;
}

static BOOL WINAPI k_InitOnceExecuteOnce(PVOID *once, init_once_fn fn, PVOID param, PVOID *ctx)
{
	uintptr_t c = 0;
	int r = kx_once_begin((volatile uintptr_t *)once, 0, &c);
	if (r == 0)
	{
		if (ctx)
			*ctx = (PVOID)c;
		return TRUE;
	}
	{
		PVOID out = NULL;
		BOOL ok = fn(once, param, &out);
		/* the low two bits are reserved (INIT_ONCE_CTX_RESERVED_BITS) */
		kx_once_complete((volatile uintptr_t *)once, ok, ok ? ((uintptr_t)out & ~(uintptr_t)3) : 0);
		if (ok && ctx)
			*ctx = (PVOID)((uintptr_t)out & ~(uintptr_t)3);
		return ok;
	}
}

/* ---------------- critical sections and kernel objects ---------------- */

static BOOL WINAPI k_InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD spin, DWORD flags)
{
	(void)spin;
	(void)flags; /* CRITICAL_SECTION_NO_DEBUG_INFO etc.: nothing to do on 9x */
	InitializeCriticalSection(cs);
	return TRUE;
}

/* Object names: Windows 9x is ANSI-only; names outside the ANSI code page
 * would not round-trip, so they are refused rather than silently changed. */
static int name_to_ansi(LPCWSTR w, char *out, int size)
{
	BOOL lossy = FALSE;
	if (!w)
		return 1;
	if (!WideCharToMultiByte(CP_ACP, 0, w, -1, out, size, NULL, &lossy) || lossy)
	{
		SetLastError(ERROR_INVALID_NAME);
		return 0;
	}
	return 1;
}

#define CREATE_EVENT_MANUAL_RESET_ 0x1
#define CREATE_EVENT_INITIAL_SET_ 0x2
#define CREATE_MUTEX_INITIAL_OWNER_ 0x1

static HANDLE WINAPI k_CreateEventExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access)
{
	(void)access;
	return CreateEventA(sa, (flags & CREATE_EVENT_MANUAL_RESET_) != 0, (flags & CREATE_EVENT_INITIAL_SET_) != 0, name);
}

static HANDLE WINAPI k_CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{
	char a[MAX_PATH];
	if (!name_to_ansi(name, a, sizeof(a)))
		return NULL;
	return k_CreateEventExA(sa, name ? a : NULL, flags, access);
}

static HANDLE WINAPI k_CreateMutexExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access)
{
	(void)access;
	return CreateMutexA(sa, (flags & CREATE_MUTEX_INITIAL_OWNER_) != 0, name);
}

static HANDLE WINAPI k_CreateMutexExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{
	char a[MAX_PATH];
	if (!name_to_ansi(name, a, sizeof(a)))
		return NULL;
	return k_CreateMutexExA(sa, name ? a : NULL, flags, access);
}

static HANDLE WINAPI k_CreateSemaphoreExA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCSTR name, DWORD flags,
                                          DWORD access)
{
	(void)flags;
	(void)access;
	return CreateSemaphoreA(sa, initial, max, name);
}

static HANDLE WINAPI k_CreateSemaphoreExW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCWSTR name, DWORD flags,
                                          DWORD access)
{
	char a[MAX_PATH];
	if (!name_to_ansi(name, a, sizeof(a)))
		return NULL;
	return k_CreateSemaphoreExA(sa, initial, max, name ? a : NULL, flags, access);
}

static LONGLONG WINAPI k_InterlockedCompareExchange64(volatile LONGLONG *dst, LONGLONG value, LONGLONG cmp)
{
	return __sync_val_compare_and_swap(dst, cmp, value); /* cmpxchg8b: Pentium and later */
}

static DWORD WINAPI k_GetCurrentProcessorNumber(void) { return 0; } /* 9x schedules one CPU */

static void WINAPI k_FlushProcessWriteBuffers(void)
{
	volatile LONG x = 0;
	InterlockedExchange((LONG *)&x, 1); /* a locked instruction is a full barrier */
}

/* ---------------- time ---------------- */

static CRITICAL_SECTION tick_cs;
static volatile LONG tick_state;
static DWORD tick_last;
static ULONGLONG tick_high;

/* GetTickCount wraps every 49.7 days. Extending it works as long as some
 * thread in the process calls GetTickCount64 at least once per wrap. */
static ULONGLONG WINAPI k_GetTickCount64(void)
{
	DWORD now;
	ULONGLONG r;
	if (tick_state != 2)
	{
		if (InterlockedCompareExchange((LONG *)&tick_state, 1, 0) == 0)
		{
			InitializeCriticalSection(&tick_cs);
			tick_last = GetTickCount();
			InterlockedExchange((LONG *)&tick_state, 2);
		}
		while (tick_state != 2)
			Sleep(0);
	}
	EnterCriticalSection(&tick_cs);
	now = GetTickCount();
	if (now < tick_last)
		tick_high += (ULONGLONG)1 << 32;
	tick_last = now;
	r = tick_high | now;
	LeaveCriticalSection(&tick_cs);
	return r;
}

static void WINAPI k_GetSystemTimePreciseAsFileTime(LPFILETIME ft)
{
	GetSystemTimeAsFileTime(ft); /* the best clock 9x has */
}

static BOOL WINAPI k_QueryUnbiasedInterruptTime(PULONGLONG t)
{
	if (!t)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*t = k_GetTickCount64() * 10000; /* 100 ns units */
	return TRUE;
}

/* ---------------- fiber-local storage ---------------- */

#define FLS_SLOTS 128
#define FLS_OUT_OF_INDEXES_ 0xFFFFFFFF
typedef void (WINAPI *fls_callback)(PVOID);

static struct
{
	DWORD tls;
	fls_callback cb;
	int used;
} fls[FLS_SLOTS];
static CRITICAL_SECTION fls_cs;
static volatile LONG fls_state;

static void fls_init(void)
{
	if (fls_state == 2)
		return;
	if (InterlockedCompareExchange((LONG *)&fls_state, 1, 0) == 0)
	{
		InitializeCriticalSection(&fls_cs);
		InterlockedExchange((LONG *)&fls_state, 2);
	}
	while (fls_state != 2)
		Sleep(0);
}

static DWORD WINAPI k_FlsAlloc(fls_callback cb)
{
	DWORD i, tls;
	fls_init();
	tls = TlsAlloc();
	if (tls == 0xFFFFFFFF)
	{
		SetLastError(ERROR_NO_MORE_ITEMS);
		return FLS_OUT_OF_INDEXES_;
	}
	EnterCriticalSection(&fls_cs);
	for (i = 0; i < FLS_SLOTS; i++)
		if (!fls[i].used)
		{
			fls[i].used = 1;
			fls[i].tls = tls;
			fls[i].cb = cb;
			LeaveCriticalSection(&fls_cs);
			return i;
		}
	LeaveCriticalSection(&fls_cs);
	TlsFree(tls);
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FLS_OUT_OF_INDEXES_;
}

static int fls_valid(DWORD i)
{
	if (i >= FLS_SLOTS || fls_state != 2 || !fls[i].used)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return 1;
}

static PVOID WINAPI k_FlsGetValue(DWORD i)
{
	PVOID v;
	if (!fls_valid(i))
		return NULL;
	v = TlsGetValue(fls[i].tls);
	SetLastError(ERROR_SUCCESS);
	return v;
}

static BOOL WINAPI k_FlsSetValue(DWORD i, PVOID v)
{
	return fls_valid(i) && TlsSetValue(fls[i].tls, v);
}

/* NT runs the callback for every thread that has a value; 9x gives no way
 * to reach other threads' TLS, so only the calling thread's value is
 * passed to the callback (documented limitation). */
static BOOL WINAPI k_FlsFree(DWORD i)
{
	PVOID v;
	fls_callback cb;
	DWORD tls;
	if (!fls_valid(i))
		return FALSE;
	EnterCriticalSection(&fls_cs);
	cb = fls[i].cb;
	tls = fls[i].tls;
	fls[i].used = 0;
	fls[i].cb = NULL;
	LeaveCriticalSection(&fls_cs);
	v = TlsGetValue(tls);
	if (cb && v)
		cb(v);
	TlsFree(tls);
	return TRUE;
}

/* Called from DllMain on DLL_THREAD_DETACH and normal process detach. */
void kexrt_fls_thread_exit(void)
{
	int i;
	if (fls_state != 2)
		return;
	for (i = FLS_SLOTS - 1; i >= 0; i--)
	{
		fls_callback cb;
		DWORD tls;
		PVOID v;
		EnterCriticalSection(&fls_cs);
		cb = fls[i].used ? fls[i].cb : NULL;
		tls = fls[i].tls;
		LeaveCriticalSection(&fls_cs);
		if (cb && (v = TlsGetValue(tls)) != NULL)
		{
			TlsSetValue(tls, NULL);
			cb(v);
		}
	}
}

/* ---------------- table (sorted by strcmp) ---------------- */

const kexrt_api kexrt_kernel32_apis[] = {
	{ "AcquireSRWLockExclusive", (unsigned long)k_AcquireSRWLockExclusive },
	{ "AcquireSRWLockShared", (unsigned long)k_AcquireSRWLockShared },
	{ "CreateEventExA", (unsigned long)k_CreateEventExA },
	{ "CreateEventExW", (unsigned long)k_CreateEventExW },
	{ "CreateMutexExA", (unsigned long)k_CreateMutexExA },
	{ "CreateMutexExW", (unsigned long)k_CreateMutexExW },
	{ "CreateSemaphoreExA", (unsigned long)k_CreateSemaphoreExA },
	{ "CreateSemaphoreExW", (unsigned long)k_CreateSemaphoreExW },
	{ "FlsAlloc", (unsigned long)k_FlsAlloc },
	{ "FlsFree", (unsigned long)k_FlsFree },
	{ "FlsGetValue", (unsigned long)k_FlsGetValue },
	{ "FlsSetValue", (unsigned long)k_FlsSetValue },
	{ "FlushProcessWriteBuffers", (unsigned long)k_FlushProcessWriteBuffers },
	{ "GetCurrentProcessorNumber", (unsigned long)k_GetCurrentProcessorNumber },
	{ "GetSystemTimePreciseAsFileTime", (unsigned long)k_GetSystemTimePreciseAsFileTime },
	{ "GetTickCount64", (unsigned long)k_GetTickCount64 },
	{ "InitOnceBeginInitialize", (unsigned long)k_InitOnceBeginInitialize },
	{ "InitOnceComplete", (unsigned long)k_InitOnceComplete },
	{ "InitOnceExecuteOnce", (unsigned long)k_InitOnceExecuteOnce },
	{ "InitOnceInitialize", (unsigned long)k_InitOnceInitialize },
	{ "InitializeConditionVariable", (unsigned long)k_InitializeConditionVariable },
	{ "InitializeCriticalSectionEx", (unsigned long)k_InitializeCriticalSectionEx },
	{ "InitializeSRWLock", (unsigned long)k_InitializeSRWLock },
	{ "InterlockedCompareExchange64", (unsigned long)k_InterlockedCompareExchange64 },
	{ "QueryUnbiasedInterruptTime", (unsigned long)k_QueryUnbiasedInterruptTime },
	{ "ReleaseSRWLockExclusive", (unsigned long)k_ReleaseSRWLockExclusive },
	{ "ReleaseSRWLockShared", (unsigned long)k_ReleaseSRWLockShared },
	{ "SleepConditionVariableCS", (unsigned long)k_SleepConditionVariableCS },
	{ "SleepConditionVariableSRW", (unsigned long)k_SleepConditionVariableSRW },
	{ "TryAcquireSRWLockExclusive", (unsigned long)k_TryAcquireSRWLockExclusive },
	{ "TryAcquireSRWLockShared", (unsigned long)k_TryAcquireSRWLockShared },
	{ "WakeAllConditionVariable", (unsigned long)k_WakeAllConditionVariable },
	{ "WakeConditionVariable", (unsigned long)k_WakeConditionVariable },
};
const int kexrt_kernel32_count = sizeof(kexrt_kernel32_apis) / sizeof(kexrt_kernel32_apis[0]);
