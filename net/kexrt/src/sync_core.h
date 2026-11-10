/*
 *  KernelEx kexrt: portable core of the Vista-era synchronisation APIs.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Windows 9x has no keyed events, which NT uses for SRW locks and
 *  condition variables. kexrt emulates the one primitive they need,
 *  "park on an address / unpark threads parked on it", with one wait list
 *  per process and an auto-reset event per thread. SRW locks, condition
 *  variables and one-time initialisation are built on that. No memory is
 *  allocated per lock or per condition variable, so the zero-initialised,
 *  never-destroyed objects of the Windows API work as documented.
 *
 *  The same code is compiled on the build host with a pthread platform
 *  layer and stress-tested there (net/tests/host).
 */
#ifndef KEXRT_SYNC_CORE_H
#define KEXRT_SYNC_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KX_INFINITE 0xFFFFFFFFu

/* ---- park / unpark ---- */
/* Under the wait-list lock: if still_blocked(ctx) returns nonzero, park the
 * calling thread on key until unparked or timeout. Returns 1 if unparked,
 * 0 on timeout, 2 if still_blocked() was false (did not wait). */
int kx_park(const void *key, int (*still_blocked)(void *ctx), void *ctx, uint32_t timeout_ms);
int kx_unpark(const void *key, int all);   /* returns number woken */
void kx_waitlist_lock(void);
void kx_waitlist_unlock(void);

/* ---- SRW locks: one pointer-sized word, zero = unlocked ---- */
void kx_srw_acquire_exclusive(volatile uintptr_t *w);
void kx_srw_acquire_shared(volatile uintptr_t *w);
int kx_srw_try_exclusive(volatile uintptr_t *w);
int kx_srw_try_shared(volatile uintptr_t *w);
void kx_srw_release_exclusive(volatile uintptr_t *w);
void kx_srw_release_shared(volatile uintptr_t *w);

/* ---- condition variables: one pointer-sized word, unused content ---- */
/* release/reacquire the caller's lock; returns 1 woken, 0 timeout */
int kx_cv_sleep(volatile uintptr_t *cv, void (*release)(void *), void (*reacquire)(void *), void *lock,
                uint32_t timeout_ms);
void kx_cv_wake(volatile uintptr_t *cv, int all);

/* ---- one-time initialisation (INIT_ONCE): 0 = new, 1 = running, done = ctx|2 ---- */
#define KX_ONCE_NEW 0
#define KX_ONCE_RUNNING 1
#define KX_ONCE_DONE_BIT 2
/* Returns 1 if the caller must run the initialiser (state now RUNNING),
 * 0 if already done (*ctx set). check_only: never claims; returns -1 if not done. */
int kx_once_begin(volatile uintptr_t *once, int check_only, uintptr_t *ctx);
/* Finish a claim: success stores ctx (low 2 bits must be 0), failure resets. */
int kx_once_complete(volatile uintptr_t *once, int success, uintptr_t ctx);

#ifdef __cplusplus
}
#endif

#endif
