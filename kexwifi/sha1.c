/*
 *  KernelExEx - SHA-1, HMAC-SHA1 and PBKDF2-HMAC-SHA1 for kexwifi.exe: the
 *  WPA2 key (PSK) is derived from the password here, so only the key is
 *  stored (IEEE 802.11i, H.4: PSK = PBKDF2(password, SSID, 4096, 256 bits)).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include <string.h>
#include "sha1.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(unsigned long h[5], const unsigned char *p)
{
	unsigned long w[80], a, b, c, d, e, t;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (unsigned long) p[4 * i] << 24 | (unsigned long) p[4 * i + 1] << 16 | (unsigned long) p[4 * i + 2] << 8 |
		       p[4 * i + 3];
	for (; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1) & 0xffffffffUL;
	a = h[0];
	b = h[1];
	c = h[2];
	d = h[3];
	e = h[4];
	for (i = 0; i < 80; i++)
	{
		unsigned long f, k;
		if (i < 20)
			f = (b & c) | (~b & d), k = 0x5a827999UL;
		else if (i < 40)
			f = b ^ c ^ d, k = 0x6ed9eba1UL;
		else if (i < 60)
			f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdcUL;
		else
			f = b ^ c ^ d, k = 0xca62c1d6UL;
		t = (ROL(a, 5) + (f & 0xffffffffUL) + e + k + w[i]) & 0xffffffffUL;
		e = d;
		d = c;
		c = ROL(b, 30) & 0xffffffffUL;
		b = a;
		a = t;
	}
	h[0] = (h[0] + a) & 0xffffffffUL;
	h[1] = (h[1] + b) & 0xffffffffUL;
	h[2] = (h[2] + c) & 0xffffffffUL;
	h[3] = (h[3] + d) & 0xffffffffUL;
	h[4] = (h[4] + e) & 0xffffffffUL;
}

void sha1_init(struct sha1 *s)
{
	s->h[0] = 0x67452301UL;
	s->h[1] = 0xefcdab89UL;
	s->h[2] = 0x98badcfeUL;
	s->h[3] = 0x10325476UL;
	s->h[4] = 0xc3d2e1f0UL;
	s->len = 0;
	s->n = 0;
}

void sha1_update(struct sha1 *s, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *) data;
	s->len += len;
	while (len)
	{
		size_t k = 64 - s->n;
		if (k > len)
			k = len;
		memcpy(s->buf + s->n, p, k);
		s->n += k;
		p += k;
		len -= k;
		if (s->n == 64)
		{
			sha1_block(s->h, s->buf);
			s->n = 0;
		}
	}
}

void sha1_final(struct sha1 *s, unsigned char out[20])
{
	unsigned long long bits = (unsigned long long) s->len * 8;
	unsigned char pad = 0x80, z = 0, lb[8];
	int i;
	sha1_update(s, &pad, 1);
	while (s->n != 56)
		sha1_update(s, &z, 1);
	for (i = 0; i < 8; i++)
		lb[i] = (unsigned char) (bits >> (56 - 8 * i));
	sha1_update(s, lb, 8);
	for (i = 0; i < 20; i++)
		out[i] = (unsigned char) (s->h[i / 4] >> (24 - 8 * (i % 4)));
	memset(s, 0, sizeof(*s));
}

static void hmac_sha1(const unsigned char *key, size_t klen, const unsigned char *msg, size_t mlen,
                      unsigned char out[20])
{
	unsigned char k[64], pad[64];
	struct sha1 s;
	int i;
	memset(k, 0, sizeof(k));
	if (klen > 64)
	{
		sha1_init(&s);
		sha1_update(&s, key, klen);
		sha1_final(&s, k);
	}
	else
		memcpy(k, key, klen);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha1_init(&s);
	sha1_update(&s, pad, 64);
	sha1_update(&s, msg, mlen);
	sha1_final(&s, out);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	sha1_init(&s);
	sha1_update(&s, pad, 64);
	sha1_update(&s, out, 20);
	sha1_final(&s, out);
	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
}

void pbkdf2_sha1(const char *pass, size_t plen, const unsigned char *salt, size_t slen, unsigned long iter,
                 unsigned char *out, size_t olen)
{
	unsigned char msg[64 + 4], u[20], t[20];
	unsigned long block = 1, i;
	size_t j;
	if (slen > 64)
		slen = 64;
	while (olen)
	{
		size_t k = olen < 20 ? olen : 20;
		memcpy(msg, salt, slen);
		msg[slen] = (unsigned char) (block >> 24);
		msg[slen + 1] = (unsigned char) (block >> 16);
		msg[slen + 2] = (unsigned char) (block >> 8);
		msg[slen + 3] = (unsigned char) block;
		hmac_sha1((const unsigned char *) pass, plen, msg, slen + 4, u);
		memcpy(t, u, 20);
		for (i = 1; i < iter; i++)
		{
			hmac_sha1((const unsigned char *) pass, plen, u, 20, u);
			for (j = 0; j < 20; j++)
				t[j] ^= u[j];
		}
		memcpy(out, t, k);
		out += k;
		olen -= k;
		block++;
	}
	memset(u, 0, sizeof(u));
	memset(t, 0, sizeof(t));
}
