/*
 *  KernelEx net runtime: host (Linux) versions of the w9xrt functions, used
 *  only to run the portable code under test on the build machine.
 *  Licensed under the GNU General Public License version 2.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "w9xrt.h"

long long w9x_time(long long *out)
{
	/* test hook: KEXTLS_TEST_TIME pins the clock (host builds only) */
	const char *fake = getenv("KEXTLS_TEST_TIME");
	long long t = fake ? atoll(fake) : (long long)time(NULL);
	if (out)
		*out = t;
	return t;
}

long long w9x_ms_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Use the Windows implementation's arithmetic here too, so tests cover it. */
#define W9X_GMTIME_ONLY
#include "w9xrt_gmtime.inc"

int w9x_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
	return vsnprintf(s, n, fmt, ap);
}

int w9x_snprintf(char *s, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vsnprintf(s, n, fmt, ap);
	va_end(ap);
	return r;
}
