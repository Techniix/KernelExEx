/*
 *  KernelEx kextls: the per-process cryptographic random number generator.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  NIST SP 800-90A HMAC_DRBG with SHA-256 (Mbed TLS implementation),
 *  seeded lazily on first use from kextls_gather_entropy() and reseeded
 *  every RESEED_INTERVAL requests. The state lives in kextls.dll's data
 *  section, which Windows 9x gives each process privately because the DLL
 *  is based below 2 GB (never in the shared arena). That property is what
 *  the old kexbases RtlGenRandom lacked.
 *
 *  If the process id changes under us (a copied state, or a DLL somehow
 *  shared), the generator reseeds before producing output.
 */
#include <string.h>
#include "mbedtls/hmac_drbg.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"
#include "kextls.h"
#include "kextls_internal.h"

#ifdef _WIN32
#include <windows.h>
#define current_pid() ((unsigned long)GetCurrentProcessId())
#else
#include <unistd.h>
#define current_pid() ((unsigned long)getpid())
#endif

#define RESEED_INTERVAL 4096
#define PERSONALIZATION "KernelEx kextls DRBG v1"

static kextls_lock drbg_lock;
static mbedtls_hmac_drbg_context drbg;
static int drbg_seeded;
static unsigned long drbg_pid;
static kextls_entropy_report last_report;

/* Mbed TLS calls this for (re)seed material. */
static int drbg_entropy(void *unused, unsigned char *out, size_t len)
{
	unsigned char seed[64];
	size_t done = 0;

	(void)unused;
	while (done < len)
	{
		size_t take = len - done < sizeof(seed) ? len - done : sizeof(seed);
		if (kextls_gather_entropy(seed, &last_report) != KEXTLS_OK)
			return MBEDTLS_ERR_HMAC_DRBG_ENTROPY_SOURCE_FAILED;
		memcpy(out + done, seed, take);
		done += take;
	}
	memset(seed, 0, sizeof(seed));
	return 0;
}

static int drbg_seed_locked(void)
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	int ret;

	if (drbg_seeded)
		mbedtls_hmac_drbg_free(&drbg);
	mbedtls_hmac_drbg_init(&drbg);
	ret = mbedtls_hmac_drbg_seed(&drbg, md, drbg_entropy, NULL,
	                             (const unsigned char *)PERSONALIZATION, sizeof(PERSONALIZATION) - 1);
	if (ret != 0)
	{
		mbedtls_hmac_drbg_free(&drbg);
		drbg_seeded = 0;
		return KEXTLS_E_NO_ENTROPY;
	}
	mbedtls_hmac_drbg_set_reseed_interval(&drbg, RESEED_INTERVAL);
	drbg_seeded = 1;
	drbg_pid = current_pid();
	return KEXTLS_OK;
}

int kextls_random(void *buf, size_t len)
{
	unsigned char *out = (unsigned char *)buf;
	int ret = KEXTLS_OK;

	if (!buf && len)
		return KEXTLS_E_PARAM;
	kextls_lock_enter(&drbg_lock);
	if (!drbg_seeded || drbg_pid != current_pid())
		ret = drbg_seed_locked();
	while (ret == KEXTLS_OK && len)
	{
		size_t take = len < MBEDTLS_HMAC_DRBG_MAX_REQUEST ? len : MBEDTLS_HMAC_DRBG_MAX_REQUEST;
		if (mbedtls_hmac_drbg_random(&drbg, out, take) != 0)
			ret = KEXTLS_E_NO_ENTROPY;
		out += take;
		len -= take;
	}
	kextls_lock_leave(&drbg_lock);
	if (ret != KEXTLS_OK && buf)
		memset(buf, 0, (size_t)(out - (unsigned char *)buf));
	return ret;
}

int kextls_rng(void *unused, unsigned char *out, size_t len)
{
	(void)unused;
	return kextls_random(out, len) == KEXTLS_OK ? 0 : MBEDTLS_ERR_HMAC_DRBG_ENTROPY_SOURCE_FAILED;
}

/* MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG: PSA (and so TLS 1.3) draws from here. */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context,
                                             uint8_t *output, size_t output_size, size_t *output_length)
{
	(void)context;
	if (kextls_random(output, output_size) != KEXTLS_OK)
	{
		*output_length = 0;
		return PSA_ERROR_INSUFFICIENT_ENTROPY;
	}
	*output_length = output_size;
	return PSA_SUCCESS;
}

/* for tests and diagnostics */
void kextls_last_entropy_report(int *cryptoapi, int *jitter_distinct)
{
	if (cryptoapi)
		*cryptoapi = last_report.cryptoapi;
	if (jitter_distinct)
		*jitter_distinct = last_report.jitter_distinct;
}
