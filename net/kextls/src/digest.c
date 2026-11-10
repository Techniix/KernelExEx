/*
 *  KernelEx kextls: message digests and HMAC for bcrypt.dll.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS /* to copy the HMAC pads in kextls_digest_dup */
#include <stdlib.h>
#include <string.h>
#include "mbedtls/md.h"
#include "kextls.h"

struct kextls_digest
{
	mbedtls_md_context_t ctx;
	int alg;
	int hmac;
};

static mbedtls_md_type_t md_type(int alg)
{
	switch (alg)
	{
	case KEXTLS_MD_MD5: return MBEDTLS_MD_MD5;
	case KEXTLS_MD_SHA1: return MBEDTLS_MD_SHA1;
	case KEXTLS_MD_SHA256: return MBEDTLS_MD_SHA256;
	case KEXTLS_MD_SHA384: return MBEDTLS_MD_SHA384;
	case KEXTLS_MD_SHA512: return MBEDTLS_MD_SHA512;
	default: return MBEDTLS_MD_NONE;
	}
}

size_t kextls_digest_size(int alg)
{
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type(alg));
	return info ? mbedtls_md_get_size(info) : 0;
}

size_t kextls_digest_block(int alg)
{
	switch (alg)
	{
	case KEXTLS_MD_MD5:
	case KEXTLS_MD_SHA1:
	case KEXTLS_MD_SHA256: return 64;
	case KEXTLS_MD_SHA384:
	case KEXTLS_MD_SHA512: return 128;
	default: return 0;
	}
}

kextls_digest *kextls_digest_new(int alg, const void *hmac_key, size_t key_len)
{
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type(alg));
	kextls_digest *d;
	int r;

	if (!info)
		return NULL;
	d = (kextls_digest *)calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	mbedtls_md_init(&d->ctx);
	d->alg = alg;
	d->hmac = hmac_key != NULL;
	r = mbedtls_md_setup(&d->ctx, info, d->hmac);
	if (r == 0)
		r = d->hmac ? mbedtls_md_hmac_starts(&d->ctx, (const unsigned char *)hmac_key, key_len)
		            : mbedtls_md_starts(&d->ctx);
	if (r != 0)
	{
		kextls_digest_free(d);
		return NULL;
	}
	return d;
}

int kextls_digest_update(kextls_digest *d, const void *data, size_t len)
{
	int r;
	if (!d || (!data && len))
		return KEXTLS_E_PARAM;
	r = d->hmac ? mbedtls_md_hmac_update(&d->ctx, (const unsigned char *)data, len)
	            : mbedtls_md_update(&d->ctx, (const unsigned char *)data, len);
	return r == 0 ? KEXTLS_OK : KEXTLS_E_PARAM;
}

int kextls_digest_final(kextls_digest *d, void *out, size_t out_len)
{
	int r;
	if (!d || !out || out_len != kextls_digest_size(d->alg))
		return KEXTLS_E_PARAM;
	if (d->hmac)
	{
		r = mbedtls_md_hmac_finish(&d->ctx, (unsigned char *)out);
		if (r == 0)
			r = mbedtls_md_hmac_reset(&d->ctx);
	}
	else
	{
		r = mbedtls_md_finish(&d->ctx, (unsigned char *)out);
		if (r == 0)
			r = mbedtls_md_starts(&d->ctx);
	}
	return r == 0 ? KEXTLS_OK : KEXTLS_E_PARAM;
}

kextls_digest *kextls_digest_dup(const kextls_digest *src)
{
	kextls_digest *d;
	if (!src)
		return NULL;
	d = (kextls_digest *)calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	mbedtls_md_init(&d->ctx);
	d->alg = src->alg;
	d->hmac = src->hmac;
	if (mbedtls_md_setup(&d->ctx, mbedtls_md_info_from_ctx(&src->ctx), d->hmac) != 0 ||
	    mbedtls_md_clone(&d->ctx, &src->ctx) != 0)
	{
		kextls_digest_free(d);
		return NULL;
	}
	/* mbedtls_md_clone copies the hash state but not the HMAC ipad/opad
	 * block (2 x block size), so copy that too. */
	if (d->hmac)
		memcpy(d->ctx.hmac_ctx, src->ctx.hmac_ctx, 2 * kextls_digest_block(d->alg));
	return d;
}

void kextls_digest_free(kextls_digest *d)
{
	if (!d)
		return;
	mbedtls_md_free(&d->ctx);
	memset(d, 0, sizeof(*d));
	free(d);
}
