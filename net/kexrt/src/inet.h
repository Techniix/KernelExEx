/* KernelEx kexrt: address text conversion. GPL version 2. */
#ifndef KEXRT_INET_H
#define KEXRT_INET_H
#include <stddef.h>
int kx_pton4(const char *s, unsigned char out[4]);
int kx_pton6(const char *s, unsigned char out[16]);
size_t kx_ntop4(const unsigned char a[4], char *buf, size_t size);  /* 0 if too small */
size_t kx_ntop6(const unsigned char a[16], char *buf, size_t size);
#endif
