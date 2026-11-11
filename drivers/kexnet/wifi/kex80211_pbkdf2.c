/*
 *  KernelExEx - PBKDF2-HMAC-SHA1 (RFC 8018), for the WPA passphrase -> PSK
 *  derivation of IEEE 802.11i (4096 rounds, SSID as salt). Needs only the
 *  crypto headers, so the host self-test builds the same file.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/hmac.h>

static void kex_wipe(void *p, size_t n)
{
	volatile unsigned char *q = (volatile unsigned char *) p;
	while (n--)
		*q++ = 0;
}

void kex80211_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, uint8_t *out, size_t olen)
{
	uint8_t asalt[32 + 4], d1[SHA1_DIGEST_LENGTH], d2[SHA1_DIGEST_LENGTH];
	uint32_t count;
	size_t i, j, r;
	HMAC_SHA1_CTX ctx;
	if (slen > 32)
		return;
	memcpy(asalt, salt, slen);
	for (count = 1; olen; count++)
	{
		asalt[slen + 0] = (uint8_t) (count >> 24);
		asalt[slen + 1] = (uint8_t) (count >> 16);
		asalt[slen + 2] = (uint8_t) (count >> 8);
		asalt[slen + 3] = (uint8_t) count;
		HMAC_SHA1_Init(&ctx, (const uint8_t *) pass, (unsigned int) plen);
		HMAC_SHA1_Update(&ctx, asalt, (unsigned int) (slen + 4));
		HMAC_SHA1_Final(d1, &ctx);
		memcpy(d2, d1, sizeof(d2));
		for (i = 1; i < 4096; i++)
		{
			HMAC_SHA1_Init(&ctx, (const uint8_t *) pass, (unsigned int) plen);
			HMAC_SHA1_Update(&ctx, d1, sizeof(d1));
			HMAC_SHA1_Final(d1, &ctx);
			for (j = 0; j < sizeof(d2); j++)
				d2[j] ^= d1[j];
		}
		r = olen < SHA1_DIGEST_LENGTH ? olen : SHA1_DIGEST_LENGTH;
		memcpy(out, d2, r);
		out += r;
		olen -= r;
	}
	kex_wipe(d1, sizeof(d1));
	kex_wipe(d2, sizeof(d2));
	kex_wipe(&ctx, sizeof(ctx));
}

