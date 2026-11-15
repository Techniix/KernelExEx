/*
 *  KernelExEx - kexbsd: short critical sections that are safe against
 *  Windows 98's interrupt-time callbacks.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  On Windows 98, NDIS timer callbacks (and other VMM event callbacks) run
 *  at interrupt time and do not respect NTKERN's emulated IRQL: such a
 *  callback can run while a thread holds a KSPIN_LOCK, and if it takes the
 *  same lock, everything stops (seen with the worker queue: a timer firing
 *  while the worker took the next item stopped every timer in the system).
 *  Windows 98 runs on one processor, so turning interrupts off is the lock.
 *  Only for sections that are short and never wait.
 */
#ifndef KEXBSD_IRQLOCK_H
#define KEXBSD_IRQLOCK_H

static __inline KIRQL irq_lock(void)
{
	unsigned long f;
	__asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) : : "memory");
	return (KIRQL) ((f >> 9) & 1);   /* IF before */
}

static __inline void irq_unlock(KIRQL was)
{
	if (was)
		__asm__ __volatile__("sti" : : : "memory");
}

#endif
