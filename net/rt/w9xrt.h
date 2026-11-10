/*
 *  KernelEx net runtime: declarations shared by the modules and the mbed TLS
 *  configuration. Also compiled on the build host for tests, where these map
 *  to the host C library (see w9xrt_host.c).
 *  Licensed under the GNU General Public License version 2.
 */
#ifndef W9XRT_H
#define W9XRT_H

#include <stdarg.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

long long w9x_time(long long *out);
long long w9x_ms_time(void);
struct tm *w9x_gmtime_r(const long long *tt, struct tm *out);
int w9x_vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int w9x_snprintf(char *s, size_t n, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
