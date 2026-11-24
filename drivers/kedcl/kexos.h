/*
 *  KernelExEx Driver Compatibility Layer (KEDCL): the OS interface drivers use.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.  See docs/KEDCL.md.
 *
 *  Backends: kexos_vxd.c (ring-0 VxDs on Windows 98/ME), kexos_wdm.c (WDM drivers),
 *  each with kexos_common.c. Drivers include only this header.
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
void *memmove(void *d, const void *s, unsigned n);
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
/* WDM backend: attach to the CmResourceTypeInterrupt descriptor of the translated
   resources (IRP_MN_START_DEVICE); detach at stop/remove */
kx_u32 kexos_irq_attach_wdm(void *desc, int (*isr)(void *ref), void *ref);
void kexos_irq_detach_wdm(kx_u32 h);

/* ---- memory-mapped registers: plain volatile access (Windows 98's NTKERN has no
 *      READ_REGISTER_*; a VxD has none either). Map once at start, unmap at stop. ---- */
void *kexos_mmio_map(kx_u32 phys_lo, kx_u32 phys_hi, kx_u32 len);
void kexos_mmio_unmap(void *va, kx_u32 len);
static __inline kx_u8 kexos_rd8(volatile void *b, kx_u32 o) { return *(volatile kx_u8 *) ((volatile char *) b + o); }
static __inline kx_u16 kexos_rd16(volatile void *b, kx_u32 o) { return *(volatile kx_u16 *) ((volatile char *) b + o); }
static __inline kx_u32 kexos_rd32(volatile void *b, kx_u32 o) { return *(volatile kx_u32 *) ((volatile char *) b + o); }
static __inline void kexos_wr8(volatile void *b, kx_u32 o, kx_u8 v) { *(volatile kx_u8 *) ((volatile char *) b + o) = v; }
static __inline void kexos_wr16(volatile void *b, kx_u32 o, kx_u16 v) { *(volatile kx_u16 *) ((volatile char *) b + o) = v; }
static __inline void kexos_wr32(volatile void *b, kx_u32 o, kx_u32 v) { *(volatile kx_u32 *) ((volatile char *) b + o) = v; }

/* ---- DMA memory: physically contiguous, below 4 GB, zeroed; *phys = its bus address ---- */
void *kexos_dma_alloc(kx_u32 size, kx_u32 *phys);
/* the same, below max_phys (last usable byte; 0 = 4 GB) and aligned to align (a power of
   two; page alignment is always given) */
void *kexos_dma_alloc_ex(kx_u32 size, kx_u32 max_phys, kx_u32 align, kx_u32 *phys);
void kexos_dma_free(void *va, kx_u32 size);

/* ---- short waits (busy; for register handshakes) ---- */
void kexos_stall_us(kx_u32 us);

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
/* save the ring to the registry, HKLM\Software\KernelEx\Diag, value l->name (CR LF
   line ends), where kexinfo's hardware report picks it up; call after start-up */
void kexos_log_save(const struct kexos_log *l);

#endif
