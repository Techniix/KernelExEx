/*
 *  KernelEx kextls: entropy collection for the DRBG seed.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Windows 9x has no kernel CSPRNG. We combine three independent sources
 *  and hash everything with SHA-512, so the seed is at least as strong as
 *  the strongest source that worked:
 *
 *    1. CryptoAPI CryptGenRandom (advapi32 with Internet Explorer's base
 *       provider, present on 98/ME and on 95 with IE 3+). Loaded at run
 *       time so a machine without it still gets sources 2 and 3.
 *       CRYPT_SILENT is tried only as a fallback: Windows 9x rejects it.
 *    2. CPU timing jitter: the time-stamp counter (RDTSC, when CPUID says
 *       it exists) sampled across scheduler and timer boundaries. Without a
 *       TSC (486, or a Cyrix part with the TSC disabled) the 1.19 MHz
 *       performance counter is used instead, with more rounds.
 *    3. Volatile system state: process/thread ids, tick count, time, memory
 *       status, stack and heap addresses.
 *
 *  The seed is refused (KEXTLS_E_NO_ENTROPY) only if CryptoAPI failed and
 *  the jitter source showed too little variation to be trusted.
 */
#include <string.h>
#include "mbedtls/sha512.h"
#include "kextls.h"
#include "kextls_internal.h"

#ifdef _WIN32
#include <windows.h>

typedef BOOL (WINAPI *acquire_fn)(ULONG_PTR *, LPCSTR, LPCSTR, DWORD, DWORD);
typedef BOOL (WINAPI *genrandom_fn)(ULONG_PTR, DWORD, BYTE *);
typedef BOOL (WINAPI *release_fn)(ULONG_PTR, DWORD);

#ifndef CRYPT_VERIFYCONTEXT
#define CRYPT_VERIFYCONTEXT 0xF0000000
#endif
#ifndef CRYPT_SILENT
#define CRYPT_SILENT 0x40
#endif

static int cryptoapi_bytes(unsigned char *out, size_t len)
{
	HMODULE adv = LoadLibraryA("ADVAPI32.DLL");
	acquire_fn acquire;
	genrandom_fn gen;
	release_fn release;
	ULONG_PTR prov = 0;
	int ok = 0;

	if (!adv)
		return 0;
	acquire = (acquire_fn)(void *)GetProcAddress(adv, "CryptAcquireContextA");
	gen = (genrandom_fn)(void *)GetProcAddress(adv, "CryptGenRandom");
	release = (release_fn)(void *)GetProcAddress(adv, "CryptReleaseContext");
	if (acquire && gen && release)
	{
		if (acquire(&prov, NULL, NULL, 1 /* PROV_RSA_FULL */, CRYPT_VERIFYCONTEXT) ||
		    acquire(&prov, NULL, NULL, 1, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
		{
			ok = gen(prov, (DWORD)len, out) ? 1 : 0;
			release(prov, 0);
		}
	}
	FreeLibrary(adv);
	return ok;
}

static int cpu_has_tsc(void)
{
	unsigned int before, after, edx = 0;

	/* CPUID exists if EFLAGS.ID (bit 21) can be toggled. */
	__asm__ volatile(
		"pushfl\n\t"
		"popl %0\n\t"
		"movl %0, %1\n\t"
		"xorl $0x200000, %0\n\t"
		"pushl %0\n\t"
		"popfl\n\t"
		"pushfl\n\t"
		"popl %0\n\t"
		"pushl %1\n\t"
		"popfl\n\t"
		: "=&r"(after), "=&r"(before));
	if (((after ^ before) & 0x200000) == 0)
		return 0;
	{
		unsigned int a = 1, b, c;
		__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(edx));
	}
	return (edx >> 4) & 1;
}

static unsigned long long read_tsc(void)
{
	unsigned int lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

static unsigned long long fine_clock(int tsc)
{
	if (tsc)
		return read_tsc();
	{
		LARGE_INTEGER q;
		QueryPerformanceCounter(&q);
		return (unsigned long long)q.QuadPart;
	}
}

/* Time a short memory-touching workload with the fine clock. The low bits
 * of each duration vary with cache and bus state, interrupts and the
 * scheduler (a Sleep(0) every 16 rounds invites a reschedule). Returns the
 * number of distinct low-byte durations seen: a crude health measure, since
 * a frozen or perfectly emulated clock gives very few. Bounded work, so a
 * coarse GetTickCount (55 ms on some 9x setups) never stalls the caller.
 * About 256 x 2,000 simple operations: a few milliseconds on a Pentium. */
static int jitter_bytes(mbedtls_sha512_context *h, int rounds)
{
	int tsc = cpu_has_tsc();
	unsigned char seen[256];
	volatile unsigned int pool[64];
	unsigned long long start, end;
	int i, j, distinct = 0, work = tsc ? 2000 : 8000;

	memset(seen, 0, sizeof(seen));
	for (j = 0; j < 64; j++)
		pool[j] = (unsigned int)j * 2654435761u;
	if (!tsc)
		rounds *= 2;
	for (i = 0; i < rounds; i++)
	{
		unsigned int acc = (unsigned int)i;
		unsigned char d;

		if ((i & 15) == 15)
			Sleep(0);
		start = fine_clock(tsc);
		for (j = 0; j < work; j++)
		{
			acc = (acc << 5) ^ (acc >> 27) ^ pool[(acc + (unsigned int)j) & 63];
			pool[j & 63] = acc;
		}
		end = fine_clock(tsc);
		d = (unsigned char)(end - start);
		if (!seen[d])
		{
			seen[d] = 1;
			distinct++;
		}
		mbedtls_sha512_update(h, (const unsigned char *)&start, sizeof(start));
		mbedtls_sha512_update(h, (const unsigned char *)&end, sizeof(end));
	}
	return distinct;
}

static void system_state(mbedtls_sha512_context *h)
{
	struct
	{
		DWORD pid, tid, tick;
		FILETIME ft;
		LARGE_INTEGER qpc;
		MEMORYSTATUS mem;
		void *stack, *heap;
		HANDLE process;
	} s;

	memset(&s, 0, sizeof(s));
	s.pid = GetCurrentProcessId();
	s.tid = GetCurrentThreadId();
	s.tick = GetTickCount();
	GetSystemTimeAsFileTime(&s.ft);
	QueryPerformanceCounter(&s.qpc);
	s.mem.dwLength = sizeof(s.mem);
	GlobalMemoryStatus(&s.mem);
	s.stack = &s;
	s.heap = GetProcessHeap();
	s.process = GetCurrentProcess();
	mbedtls_sha512_update(h, (const unsigned char *)&s, sizeof(s));
}

#else /* test host */
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static int cryptoapi_bytes(unsigned char *out, size_t len)
{
	int fd = open("/dev/urandom", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, out, len);
	close(fd);
	return n == (ssize_t)len;
}

static int jitter_bytes(mbedtls_sha512_context *h, int rounds)
{
	int i;
	struct timespec ts;

	for (i = 0; i < rounds; i++)
	{
		clock_gettime(CLOCK_MONOTONIC, &ts);
		mbedtls_sha512_update(h, (const unsigned char *)&ts, sizeof(ts));
	}
	return 256;
}

static void system_state(mbedtls_sha512_context *h)
{
	pid_t pid = getpid();
	void *stack = &pid;
	mbedtls_sha512_update(h, (const unsigned char *)&pid, sizeof(pid));
	mbedtls_sha512_update(h, (const unsigned char *)&stack, sizeof(stack));
}
#endif

/* Minimum distinct jitter deltas (of 256 possible) to accept jitter alone. */
#define JITTER_MIN_DISTINCT 48

int kextls_gather_entropy(unsigned char out[64], kextls_entropy_report *report)
{
	mbedtls_sha512_context h;
	unsigned char capi[64];
	int capi_ok, distinct;

	memset(report, 0, sizeof(*report));
	mbedtls_sha512_init(&h);
	if (mbedtls_sha512_starts(&h, 0) != 0)
		return KEXTLS_E_NO_ENTROPY;

	capi_ok = cryptoapi_bytes(capi, sizeof(capi));
	if (capi_ok)
		mbedtls_sha512_update(&h, capi, sizeof(capi));
	report->cryptoapi = capi_ok;

	distinct = jitter_bytes(&h, 256);
	report->jitter_distinct = distinct;

	system_state(&h);
	mbedtls_sha512_finish(&h, out);
	mbedtls_sha512_free(&h);
	memset(capi, 0, sizeof(capi));

	if (!capi_ok && distinct < JITTER_MIN_DISTINCT)
	{
		memset(out, 0, 64);
		return KEXTLS_E_NO_ENTROPY;
	}
	return KEXTLS_OK;
}
