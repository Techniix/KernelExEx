/*
 *  KernelExEx - host self-test for the Wi-Fi crypto (the same sources the
 *  driver builds): IEEE 802.11i H.4 passphrase->PSK vectors, FIPS-197
 *  AES-128, RFC 2202 HMAC-SHA1.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *  cc -I shim -I .. crypto_test.c ../kex80211_pbkdf2.c ../crypto/{sha1,sha2,md5,hmac,aes}.c
 */
#define _DEFAULT_SOURCE 1
#include <sys/types.h>
#include <stdio.h>
#define _DEFAULT_SOURCE 1
#include <stdint.h>
#include <string.h>
#include <crypto/sha1.h>
#include <crypto/md5.h>
#include <crypto/sha2.h>
#include <crypto/hmac.h>
#include <crypto/aes.h>

void kex80211_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, uint8_t *out, size_t olen);

static int fails;

static void check(const char *what, const uint8_t *got, const char *hex, size_t n)
{
	char buf[200];
	size_t i;
	for (i = 0; i < n; i++)
		sprintf(buf + 2 * i, "%02x", got[i]);
	if (strcmp(buf, hex))
	{
		printf("FAIL %s\n  got  %s\n  want %s\n", what, buf, hex);
		fails++;
	}
	else
		printf("ok   %s\n", what);
}

int main(void)
{
	uint8_t psk[32], out[16], d[20];
	kex80211_pbkdf2_sha1("password", 8, (const uint8_t *) "IEEE", 4, psk, 32);
	check("PSK password/IEEE (802.11i H.4.1)", psk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32);
	kex80211_pbkdf2_sha1("ThisIsAPassword", 15, (const uint8_t *) "ThisIsASSID", 11, psk, 32);
	check("PSK ThisIsAPassword/ThisIsASSID (H.4.2)", psk,
	      "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32);
	kex80211_pbkdf2_sha1("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 63,
	                     (const uint8_t *) "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, psk, 32);
	/* longest passphrase, longest SSID: value from Python hashlib.pbkdf2_hmac (an independent implementation) */
	check("PSK 63 chars / 32-char SSID (vs hashlib)", psk, "2d43d0dabfdd635377172efa1fc4b4b87dbfc4219193909ded9a7cfb89a3097b", 32);
	{
		AES_CTX ctx;
		static const uint8_t key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
		static const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		                                0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
		AES_Setkey(&ctx, key, 16);
		AES_Encrypt(&ctx, pt, out);
		check("AES-128 (FIPS-197 C.1)", out, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);
	}
	{
		HMAC_SHA1_CTX h;
		uint8_t key[20];
		memset(key, 0x0b, sizeof(key));
		HMAC_SHA1_Init(&h, key, 20);
		HMAC_SHA1_Update(&h, (const uint8_t *) "Hi There", 8);
		HMAC_SHA1_Final(d, &h);
		check("HMAC-SHA1 (RFC 2202 case 1)", d, "b617318655057264e28bc0b6fb378c8ef146be00", 20);
	}
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}
