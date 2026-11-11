/*
 *  KernelExEx - kernel random numbers for the wireless stack.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  WPA2 needs unpredictable nonces (SNonce/ANonce) and the Windows 9x
 *  kernel has no random number service. This is a hash DRBG: a SHA-256
 *  state seeded and continually re-stirred from RDRAND (when the CPU has
 *  it), RDSEED, TSC samples taken across timer-tick boundaries (their
 *  jitter is the entropy on CPUs without RDRAND), the system time and the
 *  performance counter. Output is SHA-256(state || counter); the state is
 *  replaced by SHA-256 of itself after every request (backtracking
 *  resistance).
 */
#include "obsd/kexobsd.h"
#include <crypto/sha2.h>

static uint8_t state[32];
static uint64_t counter;
static int seeded;
static KSPIN_LOCK lock;
static int lock_ready;

static uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t) hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

/* hardware random, if present: RDRAND (CPUID.1:ECX bit 30), RDSEED (CPUID.7:EBX bit 18) */
static int hw_random(uint32_t *out, int n)
{
	static int have = -1;
	uint32_t a, b, c, d;
	int i, got = 0;
	if (have < 0)
	{
		uint32_t max;
		/* CPUID itself exists on every CPU KernelExEx runs on (i586+) */
		cpuid(0, 0, &max, &b, &c, &d);
		cpuid(1, 0, &a, &b, &c, &d);
		have = (c >> 30) & 1;
		if (max >= 7)
		{
			cpuid(7, 0, &a, &b, &c, &d);
			have |= ((b >> 18) & 1) << 1;
		}
	}
	for (i = 0; i < n; i++)
	{
		uint32_t v = 0;
		unsigned char ok = 0;
		int tries;
		for (tries = 0; tries < 10 && !ok; tries++)
		{
			if (have & 2)
				__asm__ __volatile__(".byte 0x0f, 0xc7, 0xf8; setc %1" : "=a"(v), "=qm"(ok));   /* rdseed eax */
			else if (have & 1)
				__asm__ __volatile__(".byte 0x0f, 0xc7, 0xf0; setc %1" : "=a"(v), "=qm"(ok));   /* rdrand eax */
			else
				break;
		}
		if (ok)
		{
			out[got++] = v;
		}
	}
	return got;
}

static void stir(SHA2_CTX *ctx)
{
	LARGE_INTEGER t;
	uint64_t tsc = rdtsc();
	SHA256Update(ctx, (const uint8_t *) &tsc, sizeof(tsc));
	KeQuerySystemTime(&t);
	SHA256Update(ctx, (const uint8_t *) &t, sizeof(t));
	t = KeQueryPerformanceCounter(NULL);
	SHA256Update(ctx, (const uint8_t *) &t, sizeof(t));
}

static void seed(void)
{
	SHA2_CTX ctx;
	uint32_t hw[16];
	int i, n;
	SHA256Init(&ctx);
	SHA256Update(&ctx, state, sizeof(state));
	n = hw_random(hw, 16);
	SHA256Update(&ctx, (const uint8_t *) hw, n * sizeof(hw[0]));
	/* TSC across 64 timer ticks: the phase between the two clocks jitters */
	for (i = 0; i < 64; i++)
	{
		int t0 = kexbsd_ticks();
		uint64_t spins = 0;
		while (kexbsd_ticks() == t0 && spins < 50000000)
			spins++;
		stir(&ctx);
		SHA256Update(&ctx, (const uint8_t *) &spins, sizeof(spins));
	}
	SHA256Final(state, &ctx);
	explicit_bzero(hw, sizeof(hw));
	seeded = 1;
}

void obsd_random_buf(void *p, size_t len)
{
	uint8_t *out = (uint8_t *) p;
	KIRQL irql;
	if (!lock_ready)
	{
		KeInitializeSpinLock(&lock);
		lock_ready = 1;
	}
	if (!seeded)
		seed();   /* first use, at attach (PASSIVE_LEVEL, worker thread) */
	KeAcquireSpinLock(&lock, &irql);
	while (len)
	{
		SHA2_CTX ctx;
		uint8_t block[32];
		uint32_t hw[2];
		int n = hw_random(hw, 2);
		size_t c = len < sizeof(block) ? len : sizeof(block);
		SHA256Init(&ctx);
		SHA256Update(&ctx, state, sizeof(state));
		SHA256Update(&ctx, (const uint8_t *) &counter, sizeof(counter));
		SHA256Update(&ctx, (const uint8_t *) hw, n * sizeof(hw[0]));
		stir(&ctx);
		SHA256Final(block, &ctx);
		counter++;
		memcpy(out, block, c);
		out += c;
		len -= c;
		explicit_bzero(block, sizeof(block));
	}
	{
		SHA2_CTX ctx;   /* forward the state: earlier output cannot be recomputed */
		SHA256Init(&ctx);
		SHA256Update(&ctx, state, sizeof(state));
		SHA256Update(&ctx, (const uint8_t *) "next", 4);
		SHA256Final(state, &ctx);
	}
	KeReleaseSpinLock(&lock, irql);
}

uint32_t obsd_random(void)
{
	uint32_t v;
	obsd_random_buf(&v, sizeof(v));
	return v;
}
