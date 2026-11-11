/*
 *  KernelExEx - kexbsd OpenBSD personality: task(9), bsearch, Ethernet CRC
 *  and addresses. Linked into each driver (not part of kexbsd.sys).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "obsd/kexobsd_net.h"
#include "kexbsd/kexusb.h"

const uint8_t etherbroadcastaddr[ETHER_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
const uint8_t etheranyaddr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

/* ---- task(9) -------------------------------------------------------------------- *
 * A taskq is a kexbsd worker. systq is the driver binary's shared worker:
 * the network glue makes it the same thread USB drivers run on, so tasks
 * are serialized with everything else the driver does. */
struct taskq
{
	kexbsd_worker *w;
};
static struct taskq sys_tq;

void obsd_set_systq_worker(kexbsd_worker *w)
{
	sys_tq.w = w;
}

struct taskq *obsd_systq(void)
{
	if (!sys_tq.w)
		sys_tq.w = kexbsd_worker_create("systq");
	return &sys_tq;
}

struct taskq *obsd_taskq_create(const char *name)
{
	(void) name;
	return obsd_systq();   /* one thread per driver keeps the serialization simple */
}

static void task_thunk(void *arg)
{
	struct task *t = (struct task *) arg;
	t->t_func(t->t_arg);
}

int obsd_task_add(struct taskq *tq, struct task *t)
{
	int was = t->t_work.queued;
	t->t_work.fn = task_thunk;
	t->t_work.arg = t;
	t->t_tq = tq;
	kexbsd_worker_post(tq->w, &t->t_work);
	return !was;
}

int obsd_task_del(struct taskq *tq, struct task *t)
{
	(void) tq;
	(void) t;
	/* a queued task still runs once; net80211 and the USB drivers check
	 * their state when it does */
	return 0;
}

void obsd_panic(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	kexbsd_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	kexbsd_printf("kexbsd: panic: %s\n", buf);
	KeBugCheckEx(0x4B455842 /* 'KEXB' */, 0, 0, 0, 0);
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char) *a - (unsigned char) *b;
}

int abs(int v)
{
	return v < 0 ? -v : v;
}

/* ---- bsearch, as net80211's regdomain tables need -------------------------------- */
const void *obsd_bsearch(const void *key, const void *base, size_t n, size_t size,
                         int (*cmp)(const void *, const void *))
{
	const char *b = (const char *) base;
	while (n)
	{
		const char *p = b + (n >> 1) * size;
		int c = cmp(key, p);
		if (!c)
			return p;
		if (c > 0)
		{
			b = p + size;
			n--;
		}
		n >>= 1;
	}
	return NULL;
}

/* ---- Ethernet CRC-32 (IEEE 802.3) ------------------------------------------------- */
uint32_t ether_crc32_le_update(uint32_t crc, const uint8_t *buf, size_t len)
{
	size_t i;
	int b;
	for (i = 0; i < len; i++)
	{
		crc ^= buf[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
	}
	return crc;
}

uint32_t ether_crc32_le(const uint8_t *buf, size_t len)
{
	return ether_crc32_le_update(0xffffffffu, buf, len);
}

uint32_t ether_crc32_be(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xffffffffu;
	size_t i;
	int b;
	for (i = 0; i < len; i++)
		for (b = 0; b < 8; b++)
		{
			uint32_t bit = ((buf[i] >> b) & 1) ^ (crc >> 31);
			crc = (crc << 1) ^ (bit ? 0x04c11db7u : 0);
		}
	return crc;
}
