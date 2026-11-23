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
