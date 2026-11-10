/*
 *  KernelEx net: host stress tests for kexrt's portable sync core and the
 *  address text functions. Licensed under the GNU General Public License v2.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sync_core.h"
#include "inet.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---- SRW exclusive counter ---- */
static volatile uintptr_t srw;
static long counter;
#define THREADS 8
#define ITERS 200000

static void *excl_worker(void *p)
{
	int i;
	(void)p;
	for (i = 0; i < ITERS; i++)
	{
		kx_srw_acquire_exclusive(&srw);
		counter++;
		kx_srw_release_exclusive(&srw);
	}
	return NULL;
}

/* ---- SRW readers see a consistent pair ---- */
static volatile long pair_a, pair_b;
static volatile int stop_readers;
static long torn;

static void *reader(void *p)
{
	(void)p;
	while (!stop_readers)
	{
		kx_srw_acquire_shared(&srw);
		if (pair_a != pair_b)
			__sync_fetch_and_add(&torn, 1);
		kx_srw_release_shared(&srw);
	}
	return NULL;
}

/* WRITER_ITERS updates; every 50th one yields inside the lock, so readers
 * really do run while it is held, without turning the test into a measure
 * of scheduler latency (with a yield on every iteration glibc's rwlock
 * needs minutes on one CPU too). */
#define WRITER_ITERS 50000
static void *writer(void *p)
{
	int i;
	(void)p;
	for (i = 0; i < WRITER_ITERS; i++)
	{
		kx_srw_acquire_exclusive(&srw);
		pair_a++;
		if (i % 50 == 0)
			sched_yield();
		pair_b++;
		kx_srw_release_exclusive(&srw);
	}
	return NULL;
}

/* ---- condition variable bounded queue ---- */
#define QCAP 16
#define ITEMS 100000
static volatile uintptr_t qlock, not_empty, not_full;
static int queue[QCAP], qhead, qcount;
static long consumed_sum;
static int consumed;

static void rel_x(void *l) { kx_srw_release_exclusive((volatile uintptr_t *)l); }
static void acq_x(void *l) { kx_srw_acquire_exclusive((volatile uintptr_t *)l); }

static void *producer(void *p)
{
	int base = (int)(intptr_t)p, i;
	for (i = 0; i < ITEMS / 4; i++)
	{
		kx_srw_acquire_exclusive(&qlock);
		while (qcount == QCAP)
			kx_cv_sleep(&not_full, rel_x, acq_x, (void *)&qlock, KX_INFINITE);
		queue[(qhead + qcount) % QCAP] = base + i;
		qcount++;
		kx_srw_release_exclusive(&qlock);
		kx_cv_wake(&not_empty, 0);
	}
	return NULL;
}

static void *consumer(void *p)
{
	(void)p;
	for (;;)
	{
		int v;
		kx_srw_acquire_exclusive(&qlock);
		while (qcount == 0 && consumed < ITEMS)
			kx_cv_sleep(&not_empty, rel_x, acq_x, (void *)&qlock, 50); /* timeouts exercised too */
		if (qcount == 0 && consumed >= ITEMS)
		{
			kx_srw_release_exclusive(&qlock);
			kx_cv_wake(&not_empty, 1);
			return NULL;
		}
		v = queue[qhead];
		qhead = (qhead + 1) % QCAP;
		qcount--;
		consumed++;
		consumed_sum += v;
		kx_srw_release_exclusive(&qlock);
		kx_cv_wake(&not_full, 0);
	}
}

/* ---- INIT_ONCE race ---- */
static volatile uintptr_t once;
static int init_calls;
static uintptr_t seen_ctx[16];

static void *once_worker(void *p)
{
	int id = (int)(intptr_t)p;
	uintptr_t ctx = 0;
	int r = kx_once_begin(&once, 0, &ctx);
	if (r == 1)
	{
		__sync_fetch_and_add(&init_calls, 1);
		usleep(20000);
		ctx = 0x1000;
		kx_once_complete(&once, 1, ctx);
	}
	seen_ctx[id] = ctx;
	return NULL;
}

static void test_sync(void)
{
	pthread_t t[16];
	int i;
	double t0, waited;
	long expect = 0;

	printf("SRW exclusive (%d threads x %d)\n", THREADS, ITERS);
	for (i = 0; i < THREADS; i++)
		pthread_create(&t[i], NULL, excl_worker, NULL);
	for (i = 0; i < THREADS; i++)
		pthread_join(t[i], NULL);
	CHECK(counter == (long)THREADS * ITERS, "counter %ld", counter);
	CHECK(srw == 0, "lock word left as %lx", (unsigned long)srw);

	printf("SRW shared vs exclusive\n");
	for (i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, reader, NULL);
	for (i = 4; i < 6; i++)
		pthread_create(&t[i], NULL, writer, NULL);
	for (i = 4; i < 6; i++)
		pthread_join(t[i], NULL);
	stop_readers = 1;
	for (i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
	CHECK(torn == 0 && pair_a == 100000 && pair_b == 100000, "torn=%ld a=%ld b=%ld", torn, pair_a, pair_b);
	CHECK(srw == 0, "lock word left as %lx", (unsigned long)srw);
	CHECK(kx_srw_try_exclusive(&srw) && !kx_srw_try_shared(&srw) && !kx_srw_try_exclusive(&srw), "try semantics");
	kx_srw_release_exclusive(&srw);

	printf("condition variable queue (4 producers, 4 consumers, %d items)\n", ITEMS);
	for (i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, producer, (void *)(intptr_t)(i * 1000000));
	for (i = 4; i < 8; i++)
		pthread_create(&t[i], NULL, consumer, NULL);
	for (i = 0; i < 8; i++)
		pthread_join(t[i], NULL);
	for (i = 0; i < 4; i++)
		expect += (long)(i * 1000000) * (ITEMS / 4) + (long)(ITEMS / 4) * (ITEMS / 4 - 1) / 2;
	CHECK(consumed == ITEMS && consumed_sum == expect, "consumed %d sum %ld want %ld", consumed, consumed_sum, expect);

	printf("condition variable timeout\n");
	kx_srw_acquire_exclusive(&qlock);
	t0 = now_ms();
	i = kx_cv_sleep(&not_empty, rel_x, acq_x, (void *)&qlock, 100);
	waited = now_ms() - t0;
	kx_srw_release_exclusive(&qlock);
	CHECK(i == 0 && waited >= 90 && waited < 1000, "returned %d after %.0f ms", i, waited);

	printf("INIT_ONCE race (16 threads)\n");
	for (i = 0; i < 16; i++)
		pthread_create(&t[i], NULL, once_worker, (void *)(intptr_t)i);
	for (i = 0; i < 16; i++)
		pthread_join(t[i], NULL);
	CHECK(init_calls == 1, "initialiser ran %d times", init_calls);
	for (i = 0; i < 16; i++)
		CHECK(seen_ctx[i] == 0x1000, "thread %d saw ctx %lx", i, (unsigned long)seen_ctx[i]);
	{
		uintptr_t c;
		CHECK(kx_once_begin(&once, 1, &c) == 0 && c == 0x1000, "check-only after done");
	}
}

static unsigned long long rng(void)
{
	static unsigned long long x = 0x9E3779B97F4A7C15ULL;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return x;
}

static void test_inet(void)
{
	static const char *good6[] = { "::", "::1", "1::", "2001:db8::1", "fe80::1:2:3:4", "1:2:3:4:5:6:7:8",
	                               "::ffff:192.0.2.1", "2001:db8:0:0:1:0:0:1", "1:0:0:2::3", "0:0:1::", NULL };
	static const char *bad6[] = { ":", ":::", "1:::2", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7::8:9",
	                              "::g", "1:", ":1", "1.2.3.4", "::1.2.3", NULL };
	static const char *bad4[] = { "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1..2.3", " 1.2.3.4", "1.2.3.4 ", NULL };
	unsigned char a[16], b[16];
	char s1[64], s2[64];
	int i;

	printf("inet_pton / inet_ntop vs glibc\n");
	for (i = 0; good6[i]; i++)
	{
		int ok = kx_pton6(good6[i], a) == 1 && inet_pton(AF_INET6, good6[i], b) == 1 && !memcmp(a, b, 16);
		CHECK(ok, "parse %s", good6[i]);
	}
	for (i = 0; bad6[i]; i++)
		CHECK(kx_pton6(bad6[i], a) == 0, "should reject %s", bad6[i]);
	for (i = 0; bad4[i]; i++)
		CHECK(kx_pton4(bad4[i], a) == 0, "should reject \"%s\"", bad4[i]);
	CHECK(kx_pton4("0.0.0.0", a) == 1 && kx_pton4("255.255.255.255", a) == 1, "IPv4 edges");
	for (i = 0; i < 200000; i++)
	{
		int j;
		for (j = 0; j < 16; j++)
		{
			unsigned long long r = rng();
			/* bias towards zero groups so compression paths are hit */
			a[j] = (r & 3) ? 0 : (unsigned char)(r >> 8);
		}
		if (!memcmp(a, "\0\0\0\0\0\0\0\0\0\0\0\0", 12) && (a[12] | a[13]))
			continue; /* ::a.b.c.d (deprecated IPv4-compatible): glibc prints dotted, RFC 5952 does not */
		kx_ntop6(a, s1, sizeof(s1));
		inet_ntop(AF_INET6, a, s2, sizeof(s2));
		CHECK(!strcmp(s1, s2), "format: ours %s glibc %s", s1, s2);
		CHECK(kx_pton6(s1, b) == 1 && !memcmp(a, b, 16), "round trip %s", s1);
		if (failures > 10)
			return;
	}
	for (i = 0; i < 100000; i++)
	{
		unsigned long long r = rng();
		memcpy(a, &r, 4);
		kx_ntop4(a, s1, sizeof(s1));
		inet_ntop(AF_INET, a, s2, sizeof(s2));
		CHECK(!strcmp(s1, s2) && kx_pton4(s1, b) == 1 && !memcmp(a, b, 4), "v4 %s vs %s", s1, s2);
		if (failures > 10)
			return;
	}
	CHECK(kx_ntop6(a, s1, 5) == 0, "small buffer refused");
}

int main(void)
{
	test_inet();
	test_sync();
	printf(failures ? "KEXRT FAILED (%d)\n" : "KEXRT OK\n", failures);
	return failures ? 1 : 0;
}
