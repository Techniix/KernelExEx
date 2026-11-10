/*
 *  KernelEx kextls: mutex type for MBEDTLS_THREADING_ALT.
 *  On Windows a CRITICAL_SECTION (Windows 95 and later); on the test host a
 *  pthread mutex. Kept free of <windows.h> so that it can be included from
 *  every Mbed TLS translation unit.
 *  Licensed under the GNU General Public License version 2.
 */
#ifndef KEX_THREADING_ALT_TYPES_H
#define KEX_THREADING_ALT_TYPES_H

#ifdef _WIN32
/* Same size and alignment as CRITICAL_SECTION on 32-bit Windows (24 bytes). */
typedef struct
{
	void *debug_info;
	long lock_count;
	long recursion_count;
	void *owning_thread;
	void *lock_semaphore;
	unsigned long spin_count;
} kex_critsec_storage;

typedef struct mbedtls_threading_mutex_t
{
	kex_critsec_storage cs;
	char is_valid;
} mbedtls_threading_mutex_t;
#else
#include <pthread.h>
typedef struct mbedtls_threading_mutex_t
{
	pthread_mutex_t mutex;
	char is_valid;
} mbedtls_threading_mutex_t;
#endif

#endif
