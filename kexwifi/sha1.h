/* KernelExEx - SHA-1 / PBKDF2 for kexwifi.exe. GPLv2. */
#ifndef KEXWIFI_SHA1_H
#define KEXWIFI_SHA1_H
#include <stddef.h>

struct sha1
{
	unsigned long h[5];
	unsigned long long len;
	unsigned char buf[64];
	size_t n;
};

void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, size_t len);
void sha1_final(struct sha1 *s, unsigned char out[20]);
void pbkdf2_sha1(const char *pass, size_t plen, const unsigned char *salt, size_t slen, unsigned long iter,
                 unsigned char *out, size_t olen);

#endif
