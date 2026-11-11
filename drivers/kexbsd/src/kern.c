/*
 *  KernelExEx - kexbsd.sys: BSD kernel interfaces on Windows 98 SE / ME WDM.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Only kernel functions that Windows 98 SE provides are used (the build
 *  checks every import against net/tools/exports/win98se-wdm).
 */
#define KEXBSD_BUILD
#include "kexbsd/kern.h"

#define KEXBSD_TAG 'dsbK'

static void zero(void *p, size_t n)
{
	unsigned char *b = (unsigned char *) p;
	while (n--)
		*b++ = 0;
}

static void copy(void *d, const void *s, size_t n)
{
	unsigned char *a = (unsigned char *) d;
	const unsigned char *b = (const unsigned char *) s;
	while (n--)
		*a++ = *b++;
}

/* the compiler may emit these for structure copies and initialisers */
void *__cdecl memcpy(void *d, const void *s, size_t n)
{
	copy(d, s, n);
	return d;
}

void *__cdecl memset(void *d, int c, size_t n)
{
	unsigned char *b = (unsigned char *) d;
	while (n--)
		*b++ = (unsigned char) c;
	return d;
}

/* ---- malloc(9) ---------------------------------------------------------- */

MALLOC_DEFINE(M_DEVBUF, "devbuf", "device driver memory");
MALLOC_DEFINE(M_TEMP, "temp", "misc temporary data buffers");

typedef struct
{
	size_t size;
	struct malloc_type *type;
	size_t pad;   /* keep the payload 8-byte aligned */
	size_t pad2;
} MHDR;

void *kexbsd_malloc(size_t size, struct malloc_type *type, int flags)
{
	MHDR *h;
	/* M_WAITOK cannot fail in BSD; there is no reclaim to wait for here,
	 * so retry briefly at PASSIVE_LEVEL before giving up */
	int tries = (flags & M_WAITOK) && KeGetCurrentIrql() == PASSIVE_LEVEL ? 50 : 1;
	do
	{
		h = (MHDR *) ExAllocatePoolWithTag(NonPagedPool, sizeof(MHDR) + size, KEXBSD_TAG);
		if (!h && tries > 1)
		{
			LARGE_INTEGER d;
			d.QuadPart = -10 * 1000 * 10;   /* 10 ms */
			KeDelayExecutionThread(KernelMode, FALSE, &d);
		}
	} while (!h && --tries > 0);
	if (!h)
		return NULL;
	h->size = size;
	h->type = type;
	if (type)
		__sync_fetch_and_add(&type->ks_inuse, (LONG) size);
	if (flags & M_ZERO)
		zero(h + 1, size);
	return h + 1;
}

void kexbsd_free(void *p, struct malloc_type *type)
{
	MHDR *h;
	(void) type;
	if (!p)
		return;
	h = (MHDR *) p - 1;
	if (h->type)
		__sync_fetch_and_sub(&h->type->ks_inuse, (LONG) h->size);
	ExFreePool(h);
}

void *kexbsd_realloc(void *p, size_t size, struct malloc_type *type, int flags)
{
	void *n;
	if (!p)
		return kexbsd_malloc(size, type, flags);
	n = kexbsd_malloc(size, type, flags & ~M_ZERO);
	if (n)
	{
		size_t old = ((MHDR *) p - 1)->size;
		copy(n, p, old < size ? old : size);
		if ((flags & M_ZERO) && size > old)
			zero((char *) n + old, size - old);
		kexbsd_free(p, type);
	}
	return n;
}

/* ---- mutex(9) ---------------------------------------------------------- *
 * All BSD mutexes are spin locks here: DISPATCH_LEVEL while held, which is
 * what BSD allows a mutex holder to do anyway (no sleeping). */

void mtx_init(struct mtx *m, const char *name, const char *type, int opts)
{
	(void) type;
	KeInitializeSpinLock(&m->lock);
	m->owner = NULL;
	m->recurse = 0;
	m->opts = opts;
	m->name = name ? name : "mtx";
}

void mtx_destroy(struct mtx *m)
{
	m->name = NULL;
}

void kexbsd_mtx_lock(struct mtx *m)
{
	PKTHREAD me = KeGetCurrentThread();
	KIRQL irql;
	if (m->owner == me && (m->opts & MTX_RECURSE))
	{
		m->recurse++;
		return;
	}
	KeAcquireSpinLock(&m->lock, &irql);
	m->irql = irql;
	m->owner = me;
	m->recurse = 0;
}

void kexbsd_mtx_unlock(struct mtx *m)
{
	if (m->recurse > 0)
	{
		m->recurse--;
		return;
	}
	m->owner = NULL;
	KeReleaseSpinLock(&m->lock, m->irql);
}

int mtx_owned(struct mtx *m)
{
	return m->owner == KeGetCurrentThread();
}

/* ---- sleep(9) ----------------------------------------------------------- */

typedef struct SLEEPER
{
	struct SLEEPER *next;
	void *chan;
	KEVENT ev;
} SLEEPER;

static KSPIN_LOCK sleep_lock;
static SLEEPER *sleepers;

static void sleep_remove(SLEEPER *s)
{
	SLEEPER **pp;
	for (pp = &sleepers; *pp; pp = &(*pp)->next)
		if (*pp == s)
		{
			*pp = s->next;
			break;
		}
}

int msleep(void *chan, struct mtx *m, int pri, const char *wmesg, int timo)
{
	SLEEPER s;
	KIRQL irql;
	NTSTATUS st;
	LARGE_INTEGER due, *pdue = NULL;
	(void) pri;
	(void) wmesg;
	KeInitializeEvent(&s.ev, NotificationEvent, FALSE);
	s.chan = chan;
	KeAcquireSpinLock(&sleep_lock, &irql);
	s.next = sleepers;
	sleepers = &s;
	KeReleaseSpinLock(&sleep_lock, irql);
	if (m)
		kexbsd_mtx_unlock(m);
	if (timo > 0)
	{
		due.QuadPart = -(LONGLONG) timo * (10000000 / hz);
		pdue = &due;
	}
	st = KeWaitForSingleObject(&s.ev, Executive, KernelMode, FALSE, pdue);
	KeAcquireSpinLock(&sleep_lock, &irql);
	sleep_remove(&s);
	KeReleaseSpinLock(&sleep_lock, irql);
	if (m)
		kexbsd_mtx_lock(m);
	return st == STATUS_TIMEOUT ? EWOULDBLOCK : 0;
}

int pause(const char *wmesg, int timo)
{
	LARGE_INTEGER d;
	(void) wmesg;
	d.QuadPart = -(LONGLONG) (timo > 0 ? timo : 1) * (10000000 / hz);
	KeDelayExecutionThread(KernelMode, FALSE, &d);
	return 0;
}

static void wake(void *chan, int all)
{
	SLEEPER *s;
	KIRQL irql;
	KeAcquireSpinLock(&sleep_lock, &irql);
	for (s = sleepers; s; s = s->next)
		if (s->chan == chan)
		{
			KeSetEvent(&s->ev, IO_NO_INCREMENT, FALSE);
			if (!all)
				break;
		}
	KeReleaseSpinLock(&sleep_lock, irql);
}

void wakeup(void *chan)
{
	wake(chan, 1);
}

void wakeup_one(void *chan)
{
	wake(chan, 0);
}

/* ---- callout(9) --------------------------------------------------------- */

static VOID NTAPI callout_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
	struct callout *c = (struct callout *) ctx;
	(void) dpc;
	(void) a1;
	(void) a2;
	if (c->mtx)
		kexbsd_mtx_lock(c->mtx);
	/* stopped after the timer fired but before we got the lock */
	if (!InterlockedExchange(&c->pending, 0))
	{
		if (c->mtx)
			kexbsd_mtx_unlock(c->mtx);
		return;
	}
	InterlockedExchange(&c->running, 1);
	c->func(c->arg);
	InterlockedExchange(&c->running, 0);
	if (c->mtx)
		kexbsd_mtx_unlock(c->mtx);
}

void callout_init(struct callout *c, int mpsafe)
{
	(void) mpsafe;
	zero(c, sizeof(*c));
	KeInitializeTimer(&c->timer);
	KeInitializeDpc(&c->dpc, callout_dpc, c);
}

void callout_init_mtx(struct callout *c, struct mtx *m, int flags)
{
	(void) flags;
	callout_init(c, 1);
	c->mtx = m;
}

int callout_reset(struct callout *c, int t, void (*func)(void *), void *arg)
{
	LARGE_INTEGER due;
	int was = KeCancelTimer(&c->timer) ? 1 : 0;
	c->func = func;
	c->arg = arg;
	InterlockedExchange(&c->pending, 1);
	c->active = 1;
	due.QuadPart = -(LONGLONG) (t > 0 ? t : 1) * (10000000 / hz);
	KeSetTimer(&c->timer, due, &c->dpc);
	return was;
}

int callout_stop(struct callout *c)
{
	int was = KeCancelTimer(&c->timer) ? 1 : 0;
	if (InterlockedExchange(&c->pending, 0))
		was = 1;
	c->active = 0;
	return was;
}

int callout_drain(struct callout *c)
{
	int was = callout_stop(c);
	while (c->running)
	{
		LARGE_INTEGER d;
		d.QuadPart = -10 * 1000;   /* 1 ms */
		KeDelayExecutionThread(KernelMode, FALSE, &d);
	}
	return was;
}

/* ---- taskqueue(9) ------------------------------------------------------- */

struct taskqueue
{
	KSPIN_LOCK lock;
	struct task *head, *tail;
	struct task *running;
	KEVENT wake;
	KEVENT idle;
	volatile LONG exit;
	volatile LONG threads;
	const char *name;
};

struct taskqueue *taskqueue_thread;
struct taskqueue *taskqueue_swi;

struct taskqueue *taskqueue_create(const char *name, int mflags, taskqueue_enqueue_fn enqueue, void *context)
{
	struct taskqueue *tq = (struct taskqueue *) kexbsd_malloc(sizeof(*tq), M_DEVBUF, mflags | M_ZERO);
	(void) enqueue;
	(void) context;
	if (!tq)
		return NULL;
	KeInitializeSpinLock(&tq->lock);
	KeInitializeEvent(&tq->wake, SynchronizationEvent, FALSE);
	KeInitializeEvent(&tq->idle, NotificationEvent, TRUE);
	tq->name = name;
	return tq;
}

void taskqueue_thread_enqueue(void *context)
{
	(void) context;
}

static VOID NTAPI tq_thread(PVOID ctx)
{
	struct taskqueue *tq = (struct taskqueue *) ctx;
	for (;;)
	{
		struct task *t;
		KIRQL irql;
		int pending;
		KeWaitForSingleObject(&tq->wake, Executive, KernelMode, FALSE, NULL);
		for (;;)
		{
			KeAcquireSpinLock(&tq->lock, &irql);
			t = tq->head;
			if (!t)
			{
				tq->running = NULL;
				KeSetEvent(&tq->idle, IO_NO_INCREMENT, FALSE);
				KeReleaseSpinLock(&tq->lock, irql);
				break;
			}
			tq->head = t->ta_next;
			if (!tq->head)
				tq->tail = NULL;
			pending = t->ta_pending;
			t->ta_pending = 0;
			t->ta_next = NULL;
			tq->running = t;
			KeResetEvent(&tq->idle);
			KeReleaseSpinLock(&tq->lock, irql);
			t->ta_func(t->ta_context, pending);
		}
		if (tq->exit)
		{
			InterlockedDecrement(&tq->threads);
			PsTerminateSystemThread(STATUS_SUCCESS);
		}
	}
}

int taskqueue_start_threads(struct taskqueue **tqp, int count, int pri, const char *name, ...)
{
	struct taskqueue *tq = *tqp;
	int i;
	(void) pri;
	(void) name;
	for (i = 0; i < count; i++)
	{
		HANDLE h;
		if (!NT_SUCCESS(PsCreateSystemThread(&h, 0, NULL, NULL, NULL, tq_thread, tq)))
			return ENOMEM;
		ZwClose(h);
		InterlockedIncrement(&tq->threads);
	}
	return 0;
}

int taskqueue_enqueue(struct taskqueue *tq, struct task *t)
{
	KIRQL irql;
	KeAcquireSpinLock(&tq->lock, &irql);
	if (t->ta_pending)
	{
		if (t->ta_pending < 0xFFFF)
			t->ta_pending++;
		KeReleaseSpinLock(&tq->lock, irql);
		return 0;
	}
	t->ta_pending = 1;
	t->ta_next = NULL;
	if (tq->tail)
		tq->tail->ta_next = t;
	else
		tq->head = t;
	tq->tail = t;
	KeReleaseSpinLock(&tq->lock, irql);
	KeSetEvent(&tq->wake, IO_NO_INCREMENT, FALSE);
	return 0;
}

int taskqueue_cancel(struct taskqueue *tq, struct task *t, unsigned int *pendp)
{
	KIRQL irql;
	struct task **pp;
	int busy;
	KeAcquireSpinLock(&tq->lock, &irql);
	if (pendp)
		*pendp = (unsigned int) t->ta_pending;
	for (pp = &tq->head; *pp; pp = &(*pp)->ta_next)
		if (*pp == t)
		{
			*pp = t->ta_next;
			if (tq->tail == t)
			{
				struct task *x = tq->head;
				tq->tail = NULL;
				while (x)
				{
					tq->tail = x;
					x = x->ta_next;
				}
			}
			break;
		}
	t->ta_pending = 0;
	busy = tq->running == t;
	KeReleaseSpinLock(&tq->lock, irql);
	return busy ? EBUSY : 0;
}

void taskqueue_drain(struct taskqueue *tq, struct task *t)
{
	for (;;)
	{
		KIRQL irql;
		int busy;
		KeAcquireSpinLock(&tq->lock, &irql);
		busy = t->ta_pending || tq->running == t;
		KeReleaseSpinLock(&tq->lock, irql);
		if (!busy)
			return;
		pause("tqdrain", 1);
	}
}

void taskqueue_free(struct taskqueue *tq)
{
	tq->exit = 1;
	while (tq->threads)
	{
		KeSetEvent(&tq->wake, IO_NO_INCREMENT, FALSE);
		pause("tqfree", 1);
	}
	kexbsd_free(tq, M_DEVBUF);
}

/* ---- time --------------------------------------------------------------- */

uint64_t kexbsd_uptime_us(void)
{
	return (uint64_t) KeQueryInterruptTime() / 10;
}

int kexbsd_ticks(void)
{
	return (int) (KeQueryInterruptTime() / (10000000 / hz));
}

void getmicrouptime(struct timeval *tv)
{
	uint64_t us = kexbsd_uptime_us();
	tv->tv_sec = (long) (us / 1000000);
	tv->tv_usec = (long) (us % 1000000);
}

void getnanouptime(struct timespec *ts)
{
	uint64_t us = kexbsd_uptime_us();
	ts->tv_sec = (long) (us / 1000000);
	ts->tv_nsec = (long) (us % 1000000) * 1000;
}

void microtime(struct timeval *tv)
{
	LARGE_INTEGER t;
	uint64_t us;
	KeQuerySystemTime(&t);
	us = (uint64_t) (t.QuadPart - 116444736000000000LL) / 10;
	tv->tv_sec = (long) (us / 1000000);
	tv->tv_usec = (long) (us % 1000000);
}

void DELAY(int usec)
{
	while (usec > 0)
	{
		int n = usec > 50 ? 50 : usec;
		KeStallExecutionProcessor((ULONG) n);
		usec -= n;
	}
}

/* ---- printf and the log ring -------------------------------------------- */

static int fmt_num(char *out, size_t size, size_t pos, unsigned long long v, unsigned base, int upper, int neg,
                   int width, int zeropad, int left)
{
	char tmp[24];
	int n = 0, i, len;
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	do
	{
		tmp[n++] = dig[v % base];
		v /= base;
	} while (v);
	len = n + (neg ? 1 : 0);
	if (!left && !zeropad)
		for (i = len; i < width; i++, pos++)
			if (pos + 1 < size)
				out[pos] = ' ';
	if (neg)
	{
		if (pos + 1 < size)
			out[pos] = '-';
		pos++;
	}
	if (!left && zeropad)
		for (i = len; i < width; i++, pos++)
			if (pos + 1 < size)
				out[pos] = '0';
	while (n)
	{
		if (pos + 1 < size)
			out[pos] = tmp[--n];
		else
			--n;
		pos++;
	}
	for (i = len; left && i < width; i++, pos++)
		if (pos + 1 < size)
			out[pos] = ' ';
	return (int) pos;
}

/* %d %i %u %x %X %o %c %s %p %%, with -, 0, width, l, ll, z, j, h */
int kexbsd_vsnprintf(char *out, size_t size, const char *fmt, va_list ap)
{
	size_t pos = 0;
	for (; *fmt; fmt++)
	{
		int left = 0, zeropad = 0, width = 0, lng = 0;
		if (*fmt != '%')
		{
			if (pos + 1 < size)
				out[pos] = *fmt;
			pos++;
			continue;
		}
		fmt++;
		for (;; fmt++)
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zeropad = 1;
			else
				break;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		for (;; fmt++)
			if (*fmt == 'l')
				lng++;
			else if (*fmt == 'z' || *fmt == 'h')
				;
			else if (*fmt == 'j')
				lng = 2;
			else
				break;
		switch (*fmt)
		{
		case 'd':
		case 'i':
		{
			long long v = lng >= 2 ? va_arg(ap, long long) : (long long) va_arg(ap, long);
			pos = (size_t) fmt_num(out, size, pos, (unsigned long long) (v < 0 ? -v : v), 10, 0, v < 0, width,
			                       zeropad, left);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		{
			unsigned long long v =
				lng >= 2 ? va_arg(ap, unsigned long long) : (unsigned long long) va_arg(ap, unsigned long);
			unsigned base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
			pos = (size_t) fmt_num(out, size, pos, v, base, *fmt == 'X', 0, width, zeropad, left);
			break;
		}
		case 'p':
			pos = (size_t) fmt_num(out, size, pos, (unsigned long long) (ULONG_PTR) va_arg(ap, void *), 16, 0, 0,
			                       8, 1, 0);
			break;
		case 'c':
			if (pos + 1 < size)
				out[pos] = (char) va_arg(ap, int);
			pos++;
			break;
		case 's':
		{
			const char *s = va_arg(ap, const char *);
			int len = 0, i;
			if (!s)
				s = "(null)";
			while (s[len])
				len++;
			for (i = len; !left && i < width; i++, pos++)
				if (pos + 1 < size)
					out[pos] = ' ';
			for (i = 0; i < len; i++, pos++)
				if (pos + 1 < size)
					out[pos] = s[i];
			for (i = len; left && i < width; i++, pos++)
				if (pos + 1 < size)
					out[pos] = ' ';
			break;
		}
		case '%':
			if (pos + 1 < size)
				out[pos] = '%';
			pos++;
			break;
		case 0:
			fmt--;
			break;
		default:
			if (pos + 1 < size)
				out[pos] = *fmt;
			pos++;
			break;
		}
	}
	if (size)
		out[pos < size ? pos : size - 1] = 0;
	return (int) pos;
}

int kexbsd_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = kexbsd_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/* 8 KB ring of recent messages; the log files are written by the front
 * ends at PASSIVE_LEVEL from this (drivers print at any IRQL) */
#define RING 8192
static char ring[RING];
static ULONG ring_pos;
static KSPIN_LOCK ring_lock;

int kexbsd_printf(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n, i;
	KIRQL irql;
	va_start(ap, fmt);
	n = kexbsd_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n > (int) sizeof(line) - 1)
		n = sizeof(line) - 1;
	KeAcquireSpinLock(&ring_lock, &irql);
	for (i = 0; i < n; i++)
		ring[ring_pos++ % RING] = line[i];
	KeReleaseSpinLock(&ring_lock, irql);
#ifdef KEX_DEBUGCON
	for (i = 0; i < n; i++)
		__asm__ volatile("outb %0, $0xe9" : : "a"(line[i]));
#endif
	return n;
}

int kexbsd_log_tail(char *buf, size_t size)
{
	KIRQL irql;
	ULONG start, n, i;
	if (!size)
		return 0;
	KeAcquireSpinLock(&ring_lock, &irql);
	n = ring_pos < RING ? ring_pos : RING;
	if (n > size - 1)
		n = (ULONG) size - 1;
	start = ring_pos - n;
	for (i = 0; i < n; i++)
		buf[i] = ring[(start + i) % RING];
	KeReleaseSpinLock(&ring_lock, irql);
	buf[n] = 0;
	return (int) n;
}

/* ---- framework ---------------------------------------------------------- */

static volatile LONG inited;

NTSTATUS kexbsd_framework_init(void)
{
	if (InterlockedExchange(&inited, 1))
		return STATUS_SUCCESS;
	KeInitializeSpinLock(&sleep_lock);
	KeInitializeSpinLock(&ring_lock);
	taskqueue_thread = taskqueue_create("thread", M_WAITOK, taskqueue_thread_enqueue, NULL);
	taskqueue_swi = taskqueue_create("swi", M_WAITOK, taskqueue_thread_enqueue, NULL);
	if (!taskqueue_thread || !taskqueue_swi)
		return STATUS_INSUFFICIENT_RESOURCES;
	taskqueue_start_threads(&taskqueue_thread, 1, 0, "taskq");
	taskqueue_start_threads(&taskqueue_swi, 1, 0, "swi");
	kexbsd_printf("kexbsd: framework ready\n");
	return STATUS_SUCCESS;
}

/* an export driver still needs an entry point; the loader calls it once */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	(void) drv;
	(void) reg;
	return STATUS_SUCCESS;
}
