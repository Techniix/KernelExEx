/*
 *  KEDCL backend for ring-0 VxDs (Windows 98 / ME).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  VMM/VPICD services are called with INT 20h + service id (Windows 3.1/95 DDK
 *  numbering; the same ids osl98.c uses for Get_System_Time and the heap).
 */
#include "kexos.h"

/* ---- time ---- */
kx_u32 kexos_ms(void)
{
	kx_u32 t;
	__asm__ __volatile__("int $0x20\n\t.long 0x0001003F" : "=a"(t) : : "memory", "cc");   /* Get_System_Time */
	return t;
}

void kexos_delay_ms(kx_u32 ms)
{
	kx_u32 start = kexos_ms();
	while (kexos_ms() - start < ms)
		__asm__ __volatile__("pause");
}

/* timers: a few slots, each with its own entry thunk (VMM passes EDX = ref; we keep ours) */
#define NTIMER 4
static struct
{
	void (*fn)(void *);
	void *ref;
	/* 12 bytes, not 8: an index scaled by 8 off ECX encodes as SIB byte CD, and
	   an address byte 20 after it reads as INT 20h to check_vxd and to anyone
	   disassembling the image */
	kx_u32 pad;
} timers[NTIMER];

__attribute__((used)) static void timer_fire(int slot)
{
	void (*fn)(void *) = timers[slot].fn;
	timers[slot].fn = 0;
	if (fn)
		fn(timers[slot].ref);
}

#define TIMER_THUNK(n)                                                                                                 \
	__attribute__((naked)) static void timer_thunk##n(void)                                                            \
	{                                                                                                                  \
		__asm__("pushal\n\tpushl $" #n "\n\tcall _timer_fire\n\taddl $4, %esp\n\tpopal\n\tret");                       \
	}
TIMER_THUNK(0)
TIMER_THUNK(1)
TIMER_THUNK(2)
TIMER_THUNK(3)
static void (*const timer_thunks[NTIMER])(void) = { timer_thunk0, timer_thunk1, timer_thunk2, timer_thunk3 };

int kexos_timer(kx_u32 ms, void (*fn)(void *ref), void *ref)
{
	int i;
	kx_u32 h, f = kexos_irq_save();
	for (i = 0; i < NTIMER && timers[i].fn; i++)
		;
	if (i == NTIMER)
	{
		kexos_irq_restore(f);
		return -1;
	}
	timers[i].fn = fn;
	timers[i].ref = ref;
	kexos_irq_restore(f);
	/* Set_Global_Time_Out: EAX = ms, EDX = reference, ESI = callback -> ESI = handle */
	__asm__ __volatile__("int $0x20\n\t.long 0x0001003C"
	                     : "=S"(h)
	                     : "a"(ms), "d"(0), "S"(timer_thunks[i])
	                     : "ebx", "ecx", "edi", "memory", "cc");
	if (!h)
	{
		timers[i].fn = 0;
		return -1;
	}
	return 0;
}

/* ---- interrupts (VPICD) ---- */
#define NIRQ 2
struct vid
{
	kx_u16 irq, options;
	kx_u32 hw_int, virt_int, eoi, mask_change, iret, iret_timeout, hw_int_ref;
};
static struct
{
	struct vid d;
	kx_u32 handle;
	int (*isr)(void *);
	void *ref;
} irqs[NIRQ];

/* returns 0 (carry clear) if ours: then the EOI is sent here */
__attribute__((used)) static int irq_fire(int slot)
{
	if (irqs[slot].isr && irqs[slot].isr(irqs[slot].ref))
	{
		kx_u32 h = irqs[slot].handle;
		__asm__ __volatile__("int $0x20\n\t.long 0x00030004" : : "a"(h) : "memory", "cc");   /* VPICD_Phys_EOI */
		return 0;
	}
	return 1;
}

/* VPICD calls with EAX = IRQ handle, EBX = VM; carry clear = handled */
#define IRQ_THUNK(n)                                                                                                   \
	__attribute__((naked)) static void irq_thunk##n(void)                                                              \
	{                                                                                                                  \
		__asm__("pushal\n\tpushl $" #n "\n\tcall _irq_fire\n\taddl $4, %esp\n\t"                                       \
		        "testl %eax, %eax\n\tpopal\n\tjnz 1f\n\tclc\n\tret\n1:\n\tstc\n\tret");                                \
	}
IRQ_THUNK(0)
IRQ_THUNK(1)
static void (*const irq_thunks[NIRQ])(void) = { irq_thunk0, irq_thunk1 };

kx_u32 kexos_irq_attach(int irq, int shared, int (*isr)(void *ref), void *ref)
{
	int i;
	kx_u32 h, cf;
	for (i = 0; i < NIRQ && irqs[i].isr; i++)
		;
	if (i == NIRQ)
		return 0;
	memset(&irqs[i].d, 0, sizeof(irqs[i].d));
	irqs[i].d.irq = (kx_u16) irq;
	irqs[i].d.options = shared ? 0x0002 : 0;   /* VPICD_OPT_CAN_SHARE */
	irqs[i].d.hw_int = (kx_u32) irq_thunks[i];
	irqs[i].d.iret_timeout = 500;
	irqs[i].isr = isr;
	irqs[i].ref = ref;
	/* VPICD_Virtualize_IRQ: EDI -> descriptor; EAX = handle, carry on error */
	__asm__ __volatile__("int $0x20\n\t.long 0x00030001\n\tsbbl %1, %1"
	                     : "=a"(h), "=r"(cf)
	                     : "D"(&irqs[i].d)
	                     : "ebx", "ecx", "edx", "esi", "memory", "cc");
	if (cf)
	{
		irqs[i].isr = 0;
		return 0;
	}
	irqs[i].handle = h;
	return h;
}

void kexos_irq_mask(kx_u32 h, int masked)
{
	if (masked)
		__asm__ __volatile__("int $0x20\n\t.long 0x00030008" : : "a"(h) : "memory", "cc");   /* VPICD_Physically_Mask */
	else
		__asm__ __volatile__("int $0x20\n\t.long 0x00030009" : : "a"(h) : "memory", "cc");   /* VPICD_Physically_Unmask */
}

/* ---- VMM memory services (C calling convention: a dynamic-link jump keeps the caller's
 *      frame, as toolchain/vxd/vxdwraps.c) ---- */
__attribute__((naked)) static kx_u32 vmm_MapPhysToLinear(kx_u32 phys, kx_u32 len, kx_u32 flags)
{
	__asm__("int $0x20\n\t.long 0x0001806C");   /* _MapPhysToLinear */
}
__attribute__((naked)) static unsigned long long vmm_PageAllocate(kx_u32 n, kx_u32 type, kx_u32 vm, kx_u32 align,
                                                                  kx_u32 minp, kx_u32 maxp, kx_u32 *phys, kx_u32 flags)
{
	__asm__("int $0x20\n\t.long 0x00018053");   /* _PageAllocate */
}
__attribute__((naked)) static kx_u32 vmm_PageFree(kx_u32 handle, kx_u32 flags)
{
	__asm__("int $0x20\n\t.long 0x00018055");   /* _PageFree */
}

/* ---- memory-mapped registers: VMM keeps the mapping for good (there is no unmap) ---- */
void *kexos_mmio_map(kx_u32 phys_lo, kx_u32 phys_hi, kx_u32 len)
{
	kx_u32 lin;
	if (phys_hi)
		return 0;
	lin = vmm_MapPhysToLinear(phys_lo, len, 0);
	return lin == 0xFFFFFFFF ? 0 : (void *) lin;
}

void kexos_mmio_unmap(void *va, kx_u32 len)
{
	(void) va;
	(void) len;
}

/* ---- DMA memory: locked, contiguous, page aligned, below 4 GB ---- */
#define PG_SYS 1
#define PAGEZEROINIT 0x00000001
#define PAGEUSEALIGN 0x00000002
#define PAGECONTIG 0x00000004
#define PAGEFIXED 0x00000008
static struct
{
	void *va;
	kx_u32 handle;
} dma_blocks[8];

void *kexos_dma_alloc_ex(kx_u32 size, kx_u32 max_phys, kx_u32 align, kx_u32 *phys)
{
	kx_u32 p = 0, i, mask = align > 4096 ? (align >> 12) - 1 : 0;
	unsigned long long r;
	for (i = 0; i < 8 && dma_blocks[i].va; i++)
		;
	if (i == 8)
		return 0;
	r = vmm_PageAllocate((size + 4095) >> 12, PG_SYS, 0, mask, 0, max_phys ? max_phys >> 12 : 0xFFFFF, &p,
	                     PAGEZEROINIT | PAGEUSEALIGN | PAGECONTIG | PAGEFIXED);
	if (!(kx_u32) r)
		return 0;
	dma_blocks[i].handle = (kx_u32) r;
	dma_blocks[i].va = (void *) (kx_u32) (r >> 32);
	if (phys)
		*phys = p;
	return dma_blocks[i].va;
}

void *kexos_dma_alloc(kx_u32 size, kx_u32 *phys)
{
	return kexos_dma_alloc_ex(size, 0, 0, phys);
}

void kexos_dma_free(void *va, kx_u32 size)
{
	kx_u32 i;
	(void) size;
	for (i = 0; i < 8; i++)
		if (va && dma_blocks[i].va == va)
		{
			vmm_PageFree(dma_blocks[i].handle, 0);
			dma_blocks[i].va = 0;
		}
}

/* ---- short waits: a write to port 0x80 (POST codes) takes about 1 us on every PC ---- */
void kexos_stall_us(kx_u32 us)
{
	while (us--)
		kexos_outb(0x80, 0);
}

/* ---- the log ring: VxDs hand it to their helper program (kextouch /report, ...);
 *      registry saving here is not needed yet ---- */
void kexos_log_save(const struct kexos_log *l)
{
	(void) l;
}
