/*
 *  KernelEx kexrt: portable core of the Vista-era synchronisation APIs.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  See sync_core.h for the design. Correctness argument for SRW locks:
 *  a thread that must wait sets the WAITERS bit and re-checks the lock
 *  *while holding the wait-list lock*, and only then parks. A releaser
 *  drops the lock with an atomic exchange first and, if it saw WAITERS,
 *  takes the wait-list lock to clear the bit and unpark everyone. Either
 *  the waiter's re-check sees the release, or the releaser sees WAITERS and
 *  wakes the (already listed) waiter, so a wake-up is never lost. Woken
 *  threads retry; losers set WAITERS again. Windows 9x schedules one CPU,
 *  so this "wake all, retry" policy costs little.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* recursive mutex initialiser */
#endif
#include <stddef.h>
#include "sync_core.h"

/* ---------------------------------------------------------------- */
/* platform layer                                                    */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>

static CRITICAL_SECTION wl_cs;
static volatile LONG wl_state; /* 0 new, 1 initialising, 2 ready */
static DWORD tls_event = 0xFFFFFFFF;

static void wl_init(void)
{
	if (wl_state == 2)
		return;
	if (InterlockedCompareExchange((LONG *)&wl_state, 1, 0) == 0)
	{
		InitializeCriticalSection(&wl_cs);
		tls_event = TlsAlloc();
		InterlockedExchange((LONG *)&wl_state, 2);
	}
	while (wl_state != 2)
		Sleep(0);
}

void kx_waitlist_lock(void)
{
	wl_init();
	EnterCriticalSection(&wl_cs);
}

void kx_waitlist_unlock(void)
{
	LeaveCriticalSection(&wl_cs);
}

typedef HANDLE kx_event;

static kx_event thread_event(void)
{
	HANDLE e;
	wl_init();
	e = (HANDLE)TlsGetValue(tls_event);
	if (!e)
	{
		e = CreateEventA(NULL, FALSE, FALSE, NULL); /* auto-reset */
		TlsSetValue(tls_event, e);
	}
	return e;
}

/* called from DllMain(DLL_THREAD_DETACH) */
void kx_sync_thread_detach(void)
{
	if (wl_state == 2 && tls_event != 0xFFFFFFFF)
	{
		HANDLE e = (HANDLE)TlsGetValue(tls_event);
		if (e)
		{
			CloseHandle(e);
			TlsSetValue(tls_event, NULL);
		}
	}
}

static int event_wait(kx_event e, uint32_t ms)
{
	return WaitForSingleObject(e, ms) == WAIT_OBJECT_0;
}

static void event_set(kx_event e)
{
	SetEvent(e);
}

static uintptr_t cas(volatile uintptr_t *p, uintptr_t expect, uintptr_t value)
{
	return (uintptr_t)InterlockedCompareExchange((LONG *)p, (LONG)value, (LONG)expect);
}

static void yield(void)
{
	Sleep(0);
}

#else /* host: pthreads */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t wl_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

void kx_waitlist_lock(void)
{
	pthread_mutex_lock(&wl_mutex);
}

void kx_waitlist_unlock(void)
{
	pthread_mutex_unlock(&wl_mutex);
}

typedef struct
{
	pthread_mutex_t m;
	pthread_cond_t c;
	int signaled;
} host_event;
typedef host_event *kx_event;

static __thread host_event *tev;

static kx_event thread_event(void)
{
	if (!tev)
	{
		tev = (host_event *)calloc(1, sizeof(*tev));
		pthread_mutex_init(&tev->m, NULL);
		pthread_cond_init(&tev->c, NULL);
	}
	return tev;
}

void kx_sync_thread_detach(void) {}

static int event_wait(kx_event e, uint32_t ms)
{
	int ok;
	pthread_mutex_lock(&e->m);
	if (ms == KX_INFINITE)
	{
		while (!e->signaled)
			pthread_cond_wait(&e->c, &e->m);
	}
	else
	{
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += ms / 1000;
		ts.tv_nsec += (long)(ms % 1000) * 1000000L;
		if (ts.tv_nsec >= 1000000000L)
		{
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		while (!e->signaled)
			if (pthread_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT)
				break;
	}
	ok = e->signaled;
	e->signaled = 0; /* auto-reset */
	pthread_mutex_unlock(&e->m);
	return ok;
}

static void event_set(kx_event e)
{
	pthread_mutex_lock(&e->m);
	e->signaled = 1;
	pthread_cond_signal(&e->c);
	pthread_mutex_unlock(&e->m);
}

static uintptr_t cas(volatile uintptr_t *p, uintptr_t expect, uintptr_t value)
{
	return __sync_val_compare_and_swap(p, expect, value);
}

static void yield(void)
{
	sched_yield();
}
#endif

/* ---------------------------------------------------------------- */
/* park / unpark                                                     */
/* ---------------------------------------------------------------- */

typedef struct waiter
{
	const void *key;
	kx_event ev;
	volatile int signaled;
	struct waiter *next, *prev;
} waiter;

static waiter *wl_head, *wl_tail;

static void wl_remove(waiter *w)
{
	if (w->prev)
		w->prev->next = w->next;
	else
		wl_head = w->next;
	if (w->next)
		w->next->prev = w->prev;
	else
		wl_tail = w->prev;
	w->next = w->prev = NULL;
}

int kx_park(const void *key, int (*still_blocked)(void *ctx), void *ctx, uint32_t timeout_ms)
{
	waiter w;
	int woken;

	kx_waitlist_lock();
	if (still_blocked && !still_blocked(ctx))
	{
		kx_waitlist_unlock();
		return 2;
	}
	w.key = key;
	w.ev = thread_event();
	w.signaled = 0;
	w.next = NULL;
	w.prev = wl_tail;
	if (wl_tail)
		wl_tail->next = &w;
	else
		wl_head = &w;
	wl_tail = &w;
	kx_waitlist_unlock();

	woken = event_wait(w.ev, timeout_ms);

	kx_waitlist_lock();
	if (!w.signaled)
		wl_remove(&w); /* timed out and nobody woke us */
	else if (!woken)
	{
		/* Woken between the timeout and re-taking the lock: the event is
		 * set and would make this thread's next wait return early. */
		event_wait(w.ev, KX_INFINITE);
		woken = 1;
	}
	kx_waitlist_unlock();
	return woken ? 1 : 0;
}

int kx_unpark(const void *key, int all)
{
	waiter *w, *next;
	int n = 0;

	kx_waitlist_lock();
	for (w = wl_head; w; w = next)
	{
		next = w->next;
		if (w->key == key)
		{
			wl_remove(w);
			w->signaled = 1;
			event_set(w->ev);
			n++;
			if (!all)
				break;
		}
	}
	kx_waitlist_unlock();
	return n;
}

/* ---------------------------------------------------------------- */
/* SRW locks                                                         */
/* word layout: bit 0 = writer, bit 1 = waiters, bits 2.. = readers  */
/* ---------------------------------------------------------------- */

#define SRW_W 1u
#define SRW_WAIT 2u
#define SRW_R 4u

int kx_srw_try_exclusive(volatile uintptr_t *w)
{
	uintptr_t v = *w;
	return (v & ~(uintptr_t)SRW_WAIT) == 0 && cas(w, v, v | SRW_W) == v;
}

int kx_srw_try_shared(volatile uintptr_t *w)
{
	for (;;)
	{
		uintptr_t v = *w;
		if (v & (SRW_W | SRW_WAIT)) /* queue behind waiters so writers are not starved */
			return 0;
		if (cas(w, v, v + SRW_R) == v)
			return 1;
	}
}

typedef struct
{
	volatile uintptr_t *w;
	int shared;
	int woken;   /* this thread was unparked: it may pass the WAITERS bit */
} srw_ctx;

/* Under the wait-list lock: is anyone still parked on this key? */
static int key_parked(const void *key)
{
	waiter *w;
	for (w = wl_head; w; w = w->next)
		if (w->key == key)
			return 1;
	return 0;
}

static void clear_wait_bit(volatile uintptr_t *w)
{
	for (;;)
	{
		uintptr_t v = *w;
		if (!(v & SRW_WAIT) || cas(w, v, v & ~(uintptr_t)SRW_WAIT) == v)
			return;
	}
}

/*
 * Called under the wait-list lock. Returns 0 if the lock was acquired, 1 if
 * the caller must park (WAITERS is then set).
 *
 * The WAITERS bit stays set from the moment a thread parks until a woken
 * thread acquires the lock and finds nobody else parked. While it is set,
 * arriving readers queue instead of joining current readers; without this
 * a releasing reader on a single CPU re-enters before the woken writer is
 * ever scheduled, and the writer starves. Woken readers may pass the bit
 * (otherwise they would park again forever). Writers take any idle lock.
 */
static int srw_blocked(void *p)
{
	srw_ctx *c = (srw_ctx *)p;
	for (;;)
	{
		uintptr_t v = *c->w;
		int idle = (v & ~(uintptr_t)SRW_WAIT) == 0;
		int can = c->shared ? (!(v & SRW_W) && (!(v & SRW_WAIT) || c->woken)) : idle;
		if (can)
		{
			uintptr_t nv = c->shared ? v + SRW_R : v | SRW_W;
			if (cas(c->w, v, nv) != v)
				continue;
			if ((nv & SRW_WAIT) && !key_parked((const void *)c->w))
				clear_wait_bit(c->w);
			return 0;
		}
		if ((v & SRW_WAIT) || cas(c->w, v, v | SRW_WAIT) == v)
			return 1;
	}
}

static void srw_slow(volatile uintptr_t *w, int shared)
{
	srw_ctx c;
	int spins;

	c.w = w;
	c.shared = shared;
	c.woken = 0;
	for (spins = 0; spins < 4; spins++) /* brief yield first: cheap on 1 CPU */
	{
		if (shared ? kx_srw_try_shared(w) : kx_srw_try_exclusive(w))
			return;
		yield();
	}
	while (kx_park((const void *)w, srw_blocked, &c, KX_INFINITE) != 2)
		c.woken = 1; /* re-checked under the wait-list lock on the next turn */
}

void kx_srw_acquire_exclusive(volatile uintptr_t *w)
{
	if (!kx_srw_try_exclusive(w))
		srw_slow(w, 0);
}

void kx_srw_acquire_shared(volatile uintptr_t *w)
{
	if (!kx_srw_try_shared(w))
		srw_slow(w, 1);
}

/* WAITERS stays set (see srw_blocked); woken threads clear it once no one
 * is left parked. */
static void srw_wake_if_waiters(volatile uintptr_t *w, uintptr_t old)
{
	if (old & SRW_WAIT)
		kx_unpark((const void *)w, 1);
}

void kx_srw_release_exclusive(volatile uintptr_t *w)
{
	uintptr_t v;
	for (;;)
	{
		v = *w;
		if (cas(w, v, v & ~(uintptr_t)SRW_W) == v)
			break;
	}
	srw_wake_if_waiters(w, v);
}

void kx_srw_release_shared(volatile uintptr_t *w)
{
	uintptr_t v, nv;
	for (;;)
	{
		v = *w;
		nv = v - SRW_R;
		if (cas(w, v, nv) == v)
			break;
	}
	if ((nv & ~(uintptr_t)SRW_WAIT) == 0) /* last reader out */
		srw_wake_if_waiters(w, nv);
}

/* ---------------------------------------------------------------- */
/* condition variables                                               */
/* ---------------------------------------------------------------- */

typedef struct
{
	void (*release)(void *);
	void *lock;
} cv_ctx;

/* Runs under the wait-list lock just before this thread joins the wait
 * list: releasing the caller's lock here makes "release + start waiting"
 * atomic with respect to kx_cv_wake, which needs the same lock, so a wake
 * cannot fall between them. The wait-list lock is recursive, so releasing
 * an SRW lock (which may take it to wake its own waiters) is safe. */
static int cv_release_then_block(void *p)
{
	cv_ctx *c = (cv_ctx *)p;
	c->release(c->lock);
	return 1;
}

int kx_cv_sleep(volatile uintptr_t *cv, void (*release)(void *), void (*reacquire)(void *), void *lock,
                uint32_t timeout_ms)
{
	cv_ctx c;
	int r;
	c.release = release;
	c.lock = lock;
	r = kx_park((const void *)cv, cv_release_then_block, &c, timeout_ms);
	reacquire(lock);
	return r == 1;
}

void kx_cv_wake(volatile uintptr_t *cv, int all)
{
	kx_unpark((const void *)cv, all);
}

/* ---------------------------------------------------------------- */
/* one-time initialisation                                           */
/* ---------------------------------------------------------------- */

static int once_running(void *p)
{
	return *(volatile uintptr_t *)p == KX_ONCE_RUNNING;
}

int kx_once_begin(volatile uintptr_t *once, int check_only, uintptr_t *ctx)
{
	for (;;)
	{
		uintptr_t v = *once;
		if (v & KX_ONCE_DONE_BIT)
		{
			if (ctx)
				*ctx = v & ~(uintptr_t)3;
			return 0;
		}
		if (check_only)
			return -1;
		if (v == KX_ONCE_NEW && cas(once, KX_ONCE_NEW, KX_ONCE_RUNNING) == KX_ONCE_NEW)
			return 1;
		if (v == KX_ONCE_RUNNING)
			kx_park((const void *)once, once_running, (void *)once, KX_INFINITE);
	}
}

int kx_once_complete(volatile uintptr_t *once, int success, uintptr_t ctx)
{
	uintptr_t nv = success ? ((ctx & ~(uintptr_t)3) | KX_ONCE_DONE_BIT) : KX_ONCE_NEW;
	if (*once != KX_ONCE_RUNNING || (success && (ctx & 3)))
		return 0;
	kx_waitlist_lock();
	*once = nv;
	kx_unpark((const void *)once, 1);
	kx_waitlist_unlock();
	return 1;
}
