/*
 *  KernelExEx Driver Compatibility Layer (KEDCL): the OS interface drivers use.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.  See docs/KEDCL.md.
 *
 *  Backends: kexos_vxd.c (ring-0 VxDs on Windows 98/ME). A WDM backend follows,
 *  extracted from kexbsd/kern.c and kexhda. Drivers include only this header.
 */
#ifndef KEXOS_H
#define KEXOS_H

#include <stdarg.h>

typedef unsigned char kx_u8;
typedef unsigned short kx_u16;
typedef unsigned int kx_u32;

/* ---- C basics (ring 0 has no C runtime) ---- */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
char *strcpy(char *d, const char *s);
unsigned strlen(const char *s);

/* ---- port I/O ---- */
static __inline kx_u8 kexos_inb(kx_u16 port)
{
	kx_u8 v;
	__asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static __inline void kexos_outb(kx_u16 port, kx_u8 v)
{
	__asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* ---- interrupt-safe sections (VxD: CLI/STI with the previous state kept) ---- */
static __inline kx_u32 kexos_irq_save(void)
{
	kx_u32 f;
	__asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(f) : : "memory");
	return f;
}
static __inline void kexos_irq_restore(kx_u32 f)
{
	__asm__ __volatile__("pushl %0\n\tpopfl" : : "r"(f) : "memory", "cc");
}

/* ---- time ---- */
kx_u32 kexos_ms(void);                 /* milliseconds since Windows started */
void kexos_delay_ms(kx_u32 ms);        /* busy wait (init paths only) */
/* one-shot timer: fn(ref) runs in about ms milliseconds, at event time; re-arm inside fn */
int kexos_timer(kx_u32 ms, void (*fn)(void *ref), void *ref);

/* ---- interrupts: isr(ref) at hardware-interrupt time; return 1 if it was ours.
 *      The backend sends the EOI. Returns a handle, 0 on failure. ---- */
kx_u32 kexos_irq_attach(int irq, int shared, int (*isr)(void *ref), void *ref);
void kexos_irq_mask(kx_u32 h, int masked);

/* ---- logging (KEDCL: a ring per driver, read by kexinfo and the driver's helper) ---- */
struct kexos_log
{
	char buf[4096];
	kx_u32 pos;      /* total bytes written; buf holds the last 4096 */
	const char *name;
};
void kexos_log(struct kexos_log *l, const char *fmt, ...);
void kexos_vlog(struct kexos_log *l, const char *fmt, va_list ap);
/* copy the ring out, oldest first; returns bytes copied */
kx_u32 kexos_log_read(const struct kexos_log *l, char *out, kx_u32 size);

#endif
