/*
 *  KernelExEx - kexbsd: BSD kernel interfaces on Windows 98 SE / ME WDM.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  The subset of FreeBSD's kernel API (malloc(9), mutex(9), sleep(9),
 *  callout(9), taskqueue(9), time, DELAY, printf) that drivers use, on top
 *  of the Windows 98 SE kernel services. See docs/research/bsdcompat.md.
 *
 *  Rules, as in FreeBSD:
 *   - mtx_lock() may be used from DPCs and taskqueue threads, not from ISRs;
 *   - msleep(), pause(), callout_drain(), taskqueue_drain() and
 *     malloc(M_WAITOK) need a thread context (PASSIVE_LEVEL), no lock held
 *     other than the one passed to msleep().
 */
#ifndef KEXBSD_KERN_H
#define KEXBSD_KERN_H

#include <ntddk.h>   /* wdm.h plus HalGetBusData, MmGetPhysicalAddress ... */

#ifdef KEXBSD_BUILD
#define KEXBSD_API __declspec(dllexport)
#else
#define KEXBSD_API __declspec(dllimport)
#endif

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#define hz 100

/* ---- errors (FreeBSD values) ------------------------------------------ */
/* mingw-w64 errno.h may already define some of these with other values */
#undef EPERM
#undef ENOENT
#undef EIO
#undef ENXIO
#undef E2BIG
#undef ENOMEM
#undef EBUSY
#undef EEXIST
#undef ENODEV
#undef EINVAL
#undef ENOSPC
#undef EFBIG
#undef EAGAIN
#undef EWOULDBLOCK
#undef EINPROGRESS
#undef EALREADY
#undef ENOTSUP
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EINTR
#undef ERESTART
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOMEM 12
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define EFBIG 27
#define ENOSPC 28
#define EAGAIN 35
#define EWOULDBLOCK EAGAIN
#define EINPROGRESS 36
#define EALREADY 37
#define ENOTSUP 45
#define EOPNOTSUPP ENOTSUP
#define ETIMEDOUT 60
#define EINTR 4
#define ERESTART (-1)

/* ---- malloc(9) -------------------------------------------------------- */
struct malloc_type
{
	const char *ks_shortdesc;
	volatile LONG ks_inuse;   /* bytes, for the driver log */
};
#define MALLOC_DEFINE(type, shortdesc, longdesc) struct malloc_type type[1] = { { shortdesc, 0 } }
#define MALLOC_DECLARE(type) extern struct malloc_type type[1]
#define M_NOWAIT 0x0001
#define M_WAITOK 0x0002
#define M_ZERO 0x0100
#define M_NODUMP 0x0800

KEXBSD_API extern struct malloc_type M_DEVBUF[1];
KEXBSD_API extern struct malloc_type M_TEMP[1];

KEXBSD_API void *kexbsd_malloc(size_t size, struct malloc_type *type, int flags);
KEXBSD_API void *kexbsd_realloc(void *p, size_t size, struct malloc_type *type, int flags);
KEXBSD_API void kexbsd_free(void *p, struct malloc_type *type);
#define malloc(s, t, f) kexbsd_malloc((s), (t), (f))
#define realloc(p, s, t, f) kexbsd_realloc((p), (s), (t), (f))
#define free(p, t) kexbsd_free((p), (t))

/* ---- mutex(9) ---------------------------------------------------------- */
#define MTX_DEF 0x0000
#define MTX_SPIN 0x0001
#define MTX_RECURSE 0x0004
#define MTX_NOWITNESS 0x0008
#define MA_OWNED 0x01
#define MA_NOTOWNED 0x02

struct mtx
{
	KSPIN_LOCK lock;
	KIRQL irql;
	PKTHREAD owner;
	int recurse;
	int opts;
	const char *name;
};

KEXBSD_API void mtx_init(struct mtx *m, const char *name, const char *type, int opts);
KEXBSD_API void mtx_destroy(struct mtx *m);
KEXBSD_API void kexbsd_mtx_lock(struct mtx *m);
KEXBSD_API void kexbsd_mtx_unlock(struct mtx *m);
KEXBSD_API int mtx_owned(struct mtx *m);
#define mtx_lock(m) kexbsd_mtx_lock(m)
#define mtx_unlock(m) kexbsd_mtx_unlock(m)
#define mtx_lock_spin(m) kexbsd_mtx_lock(m)
#define mtx_unlock_spin(m) kexbsd_mtx_unlock(m)
#define mtx_assert(m, what) do { (void) (m); (void) (what); } while (0)
#define mtx_initialized(m) ((m)->name != NULL)

/* ---- sleep(9) ---------------------------------------------------------- */
#define PZERO 0
#define PCATCH 0x100
KEXBSD_API int msleep(void *chan, struct mtx *m, int pri, const char *wmesg, int timo);
KEXBSD_API int pause(const char *wmesg, int timo);
KEXBSD_API void wakeup(void *chan);
KEXBSD_API void wakeup_one(void *chan);
#define tsleep(chan, pri, wmesg, timo) msleep((chan), NULL, (pri), (wmesg), (timo))
#define mtx_sleep(chan, m, pri, wmesg, timo) msleep((chan), (m), (pri), (wmesg), (timo))

/* ---- callout(9) -------------------------------------------------------- */
struct callout
{
	KTIMER timer;
	KDPC dpc;
	void (*func)(void *);
	void *arg;
	struct mtx *mtx;
	volatile LONG pending;   /* scheduled and not yet run */
	volatile LONG running;   /* function executing now */
	volatile LONG active;    /* set by reset, cleared by stop/deactivate */
};
#define CALLOUT_RETURNUNLOCKED 0x0010

KEXBSD_API void callout_init(struct callout *c, int mpsafe);
KEXBSD_API void callout_init_mtx(struct callout *c, struct mtx *m, int flags);
KEXBSD_API int callout_reset(struct callout *c, int ticks_, void (*func)(void *), void *arg);
KEXBSD_API int callout_stop(struct callout *c);
KEXBSD_API int callout_drain(struct callout *c);
#define callout_schedule(c, t) callout_reset((c), (t), (c)->func, (c)->arg)
#define callout_pending(c) ((c)->pending != 0)
#define callout_active(c) ((c)->active != 0)
#define callout_deactivate(c) ((c)->active = 0)

/* ---- taskqueue(9) ------------------------------------------------------ */
typedef void task_fn_t(void *context, int pending);

struct task
{
	struct task *ta_next;
	int ta_pending;
	int ta_priority;
	task_fn_t *ta_func;
	void *ta_context;
};
#define TASK_INIT(t, prio, fn, ctx)                                                                                    \
	do                                                                                                             \
	{                                                                                                              \
		(t)->ta_next = NULL;                                                                                   \
		(t)->ta_pending = 0;                                                                                   \
		(t)->ta_priority = (prio);                                                                             \
		(t)->ta_func = (fn);                                                                                   \
		(t)->ta_context = (ctx);                                                                               \
	} while (0)

struct taskqueue;
typedef void (*taskqueue_enqueue_fn)(void *context);

KEXBSD_API struct taskqueue *taskqueue_create(const char *name, int mflags, taskqueue_enqueue_fn enqueue,
                                              void *context);
KEXBSD_API int taskqueue_start_threads(struct taskqueue **tqp, int count, int pri, const char *name, ...);
KEXBSD_API int taskqueue_enqueue(struct taskqueue *tq, struct task *task);
KEXBSD_API int taskqueue_cancel(struct taskqueue *tq, struct task *task, unsigned int *pendp);
KEXBSD_API void taskqueue_drain(struct taskqueue *tq, struct task *task);
KEXBSD_API void taskqueue_free(struct taskqueue *tq);
KEXBSD_API void taskqueue_thread_enqueue(void *context);
KEXBSD_API extern struct taskqueue *taskqueue_thread;
KEXBSD_API extern struct taskqueue *taskqueue_swi;
#define taskqueue_create_fast taskqueue_create

/* ---- time --------------------------------------------------------------- */
struct timeval
{
	long tv_sec;
	long tv_usec;
};
struct timespec
{
	long tv_sec;
	long tv_nsec;
};
KEXBSD_API int kexbsd_ticks(void);
KEXBSD_API uint64_t kexbsd_uptime_us(void);
KEXBSD_API void getmicrouptime(struct timeval *tv);
KEXBSD_API void getnanouptime(struct timespec *ts);
KEXBSD_API void microtime(struct timeval *tv);
KEXBSD_API void DELAY(int usec);
#define ticks kexbsd_ticks()
#define time_uptime ((long) (kexbsd_uptime_us() / 1000000))
#define microuptime getmicrouptime
#define nanouptime getnanouptime

/* ---- printf and logging ------------------------------------------------- */
KEXBSD_API int kexbsd_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
KEXBSD_API int kexbsd_snprintf(char *buf, size_t size, const char *fmt, ...);
KEXBSD_API int kexbsd_printf(const char *fmt, ...);
KEXBSD_API int kexbsd_log_tail(char *buf, size_t size);   /* last lines, for the property page */
#define printf kexbsd_printf
#define snprintf kexbsd_snprintf
#define vsnprintf kexbsd_vsnprintf
#define bootverbose 0

/* ---- framework ---------------------------------------------------------- */
/* front ends call this once (PASSIVE_LEVEL) before using anything else */
KEXBSD_API NTSTATUS kexbsd_framework_init(void);

#endif
