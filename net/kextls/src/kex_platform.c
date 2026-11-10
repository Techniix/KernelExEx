/*
 *  KernelEx kextls: platform hooks for Mbed TLS on Windows 9x
 *  (threading, time) and the module's lazily initialised locks.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 */
#include <string.h>
#include <stdlib.h>
#include "mbedtls/threading.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "kextls_internal.h"
#include "w9xrt.h"

#ifdef _WIN32
#include <windows.h>

typedef char check_cs_size[sizeof(kex_critsec_storage) >= sizeof(CRITICAL_SECTION) ? 1 : -1];

static void cs_init(mbedtls_threading_mutex_t *m)
{
	InitializeCriticalSection((CRITICAL_SECTION *)&m->cs);
	m->is_valid = 1;
}

static void cs_free(mbedtls_threading_mutex_t *m)
{
	if (m->is_valid)
		DeleteCriticalSection((CRITICAL_SECTION *)&m->cs);
	m->is_valid = 0;
}

static int cs_lock(mbedtls_threading_mutex_t *m)
{
	if (!m->is_valid)
		return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
	EnterCriticalSection((CRITICAL_SECTION *)&m->cs);
	return 0;
}

static int cs_unlock(mbedtls_threading_mutex_t *m)
{
	if (!m->is_valid)
		return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
	LeaveCriticalSection((CRITICAL_SECTION *)&m->cs);
	return 0;
}

void kextls_lock_enter(kextls_lock *l)
{
	if (InterlockedCompareExchange((LONG *)&l->state, 1, 0) == 0)
	{
		CRITICAL_SECTION *cs = (CRITICAL_SECTION *)HeapAlloc(GetProcessHeap(), 0, sizeof(CRITICAL_SECTION));
		InitializeCriticalSection(cs);
		l->impl = cs;
		InterlockedExchange((LONG *)&l->state, 2);
	}
	while (l->state != 2)
		Sleep(0);
	EnterCriticalSection((CRITICAL_SECTION *)l->impl);
}

void kextls_lock_leave(kextls_lock *l)
{
	LeaveCriticalSection((CRITICAL_SECTION *)l->impl);
}

#else /* host */
#include <pthread.h>
#include <sched.h>

static void cs_init(mbedtls_threading_mutex_t *m)
{
	pthread_mutex_init(&m->mutex, NULL);
	m->is_valid = 1;
}

static void cs_free(mbedtls_threading_mutex_t *m)
{
	if (m->is_valid)
		pthread_mutex_destroy(&m->mutex);
	m->is_valid = 0;
}

static int cs_lock(mbedtls_threading_mutex_t *m)
{
	return (m->is_valid && pthread_mutex_lock(&m->mutex) == 0) ? 0 : MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
}

static int cs_unlock(mbedtls_threading_mutex_t *m)
{
	return (m->is_valid && pthread_mutex_unlock(&m->mutex) == 0) ? 0 : MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
}

void kextls_lock_enter(kextls_lock *l)
{
	if (__sync_bool_compare_and_swap(&l->state, 0, 1))
	{
		pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
		pthread_mutex_init(m, NULL);
		l->impl = m;
		__sync_synchronize();
		l->state = 2;
	}
	while (l->state != 2)
		sched_yield();
	pthread_mutex_lock((pthread_mutex_t *)l->impl);
}

void kextls_lock_leave(kextls_lock *l)
{
	pthread_mutex_unlock((pthread_mutex_t *)l->impl);
}
#endif

static int platform_ready;

void kextls_platform_init(void)
{
	if (platform_ready)
		return;
	/* Called from DllMain(PROCESS_ATTACH), so it must stay cheap: this only
	 * installs function pointers and initialises Mbed TLS's global mutexes. */
	mbedtls_threading_set_alt(cs_init, cs_free, cs_lock, cs_unlock);
	platform_ready = 1;
}

void kextls_platform_shutdown(void)
{
	if (!platform_ready)
		return;
	mbedtls_threading_free_alt();
	platform_ready = 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)w9x_ms_time();
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
	long long t = (long long)*tt;
	return w9x_gmtime_r(&t, tm_buf);
}
