/*
 *  KernelEx kextls: internal declarations.
 *  Licensed under the GNU General Public License version 2.
 */
#ifndef KEXTLS_INTERNAL_H
#define KEXTLS_INTERNAL_H

#include <stddef.h>
#include "mbedtls/x509_crt.h"

typedef struct
{
	int cryptoapi;        /* CryptGenRandom produced bytes */
	int jitter_distinct;  /* distinct low-byte timing deltas of 256 */
} kextls_entropy_report;

int kextls_gather_entropy(unsigned char out[64], kextls_entropy_report *report);

/* f_rng-compatible wrapper around the DRBG */
int kextls_rng(void *unused, unsigned char *out, size_t len);

/* process-wide one-time setup (threading hooks); safe to call repeatedly */
void kextls_platform_init(void);
void kextls_platform_shutdown(void);

/* a simple lazily initialised lock usable before any other setup */
typedef struct
{
	volatile long state;  /* 0 = new, 1 = initialising, 2 = ready */
	void *impl;
} kextls_lock;

void kextls_lock_enter(kextls_lock *l);
void kextls_lock_leave(kextls_lock *l);

/* trust store (truststore.c) */
mbedtls_x509_crt *kextls_trust_roots(int *count_or_error);
long long kextls_trust_build_time(void);

/* directory containing kextls.dll, with trailing separator */
const char *kextls_module_dir(void);

#endif
