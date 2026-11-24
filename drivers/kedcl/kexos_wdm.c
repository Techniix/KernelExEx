/*
 *  KEDCL backend for WDM drivers on Windows 98/ME (NTKERN) and 2000/XP.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.  See docs/KEDCL.md.
 *
 *  What kexbsd/kern.c and kexhda learnt, in one place:
 *  - NTKERN has no KeSetTimerEx (periodic timers): one-shot KeSetTimer, re-armed.
 *  - Interrupts come from the translated resources of IRP_MN_START_DEVICE, so a
 *    WDM driver attaches with kexos_irq_attach_wdm(); kexos_irq_attach() (an IRQ
 *    number, as VxDs use) is not available here and returns 0.
 *  Build with kexos_common.c.
 */
#include <ntddk.h>   /* wdm.h lacks the contiguous-memory calls */
#include "kexos.h"

/* ---- time ---- */
kx_u32 kexos_ms(void)
{
	/* interrupt time (100 ns units; as kexehci, works on NTKERN) / 10000, without the C
	   library's 64-bit division: divl divides edx:eax by 10000, the high half reduced
	   first so the quotient fits (only its low 32 bits matter: ms wrap like the VxD's) */
	ULONGLONG n = KeQueryInterruptTime();
	kx_u32 hi = (kx_u32) (n >> 32) % 10000, lo = (kx_u32) n, q;
	__asm__("divl %2" : "=a"(q), "+d"(hi) : "r"((kx_u32) 10000), "a"(lo) : "cc");
	return q;
}

void kexos_delay_ms(kx_u32 ms)
{
	while (ms--)
	{
		int i;
		for (i = 0; i < 20; i++)
			KeStallExecutionProcessor(50);   /* short stalls: NTKERN's is a busy loop */
	}
}

/* ---- one-shot timers (fixed slots, as the VxD backend) ---- */
#define NTIMERS 4
static struct
{
	KTIMER timer;
	KDPC dpc;
	void (*fn)(void *ref);
	void *ref;
	int init;
} timers[NTIMERS];

static VOID NTAPI timer_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
	int i = (int) (ULONG_PTR) ctx;
	void (*fn)(void *) = timers[i].fn;
	(void) dpc;
	(void) a1;
	(void) a2;
	timers[i].fn = NULL;   /* free before the call: fn may re-arm */
	if (fn)
		fn(timers[i].ref);
}

int kexos_timer(kx_u32 ms, void (*fn)(void *ref), void *ref)
{
	kx_u32 f = kexos_irq_save();
	int i;
	LARGE_INTEGER due;
	for (i = 0; i < NTIMERS && timers[i].fn; i++)
		;
	if (i == NTIMERS)
	{
		kexos_irq_restore(f);
		return 0;
	}
	if (!timers[i].init)
	{
		KeInitializeTimer(&timers[i].timer);
		KeInitializeDpc(&timers[i].dpc, timer_dpc, (PVOID) (ULONG_PTR) i);
		timers[i].init = 1;
	}
	timers[i].fn = fn;
	timers[i].ref = ref;
	kexos_irq_restore(f);
	due.QuadPart = -(LONGLONG) ms * 10000;
	KeSetTimer(&timers[i].timer, due, &timers[i].dpc);
	return 1;
}

/* ---- interrupts ---- */
#define NIRQS 2
static struct
{
	PKINTERRUPT obj;
	int (*isr)(void *ref);
	void *ref;
} irqs[NIRQS];

static BOOLEAN NTAPI irq_thunk(PKINTERRUPT obj, PVOID ctx)
{
	int i = (int) (ULONG_PTR) ctx;
	(void) obj;
	return irqs[i].isr && irqs[i].isr(irqs[i].ref) ? TRUE : FALSE;
}

kx_u32 kexos_irq_attach(int irq, int shared, int (*isr)(void *ref), void *ref)
{
	(void) irq;
	(void) shared;
	(void) isr;
	(void) ref;
	return 0;
}

/* d: the CmResourceTypeInterrupt descriptor of the translated resources */
kx_u32 kexos_irq_attach_wdm(void *desc, int (*isr)(void *ref), void *ref)
{
	PCM_PARTIAL_RESOURCE_DESCRIPTOR d = (PCM_PARTIAL_RESOURCE_DESCRIPTOR) desc;
	int i;
	for (i = 0; i < NIRQS && irqs[i].obj; i++)
		;
	if (i == NIRQS || !d || d->Type != CmResourceTypeInterrupt)
		return 0;
	irqs[i].isr = isr;
	irqs[i].ref = ref;
	if (!NT_SUCCESS(IoConnectInterrupt(&irqs[i].obj, irq_thunk, (PVOID) (ULONG_PTR) i, NULL, d->u.Interrupt.Vector,
	                                   (KIRQL) d->u.Interrupt.Level, (KIRQL) d->u.Interrupt.Level,
	                                   (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive,
	                                   d->ShareDisposition == CmResourceShareShared, d->u.Interrupt.Affinity, FALSE)))
	{
		irqs[i].obj = NULL;
		irqs[i].isr = NULL;
		return 0;
	}
	return (kx_u32) i + 1;
}

void kexos_irq_detach_wdm(kx_u32 h)
{
	if (h < 1 || h > NIRQS || !irqs[h - 1].obj)
		return;
	IoDisconnectInterrupt(irqs[h - 1].obj);
	irqs[h - 1].obj = NULL;
	irqs[h - 1].isr = NULL;
}

/* WDM devices mask their interrupt at the device, not at the PIC: a no-op here */
void kexos_irq_mask(kx_u32 h, int masked)
{
	(void) h;
	(void) masked;
}

/* ---- memory-mapped registers ---- */
void *kexos_mmio_map(kx_u32 phys_lo, kx_u32 phys_hi, kx_u32 len)
{
	PHYSICAL_ADDRESS pa;
	pa.LowPart = phys_lo;
	pa.HighPart = (LONG) phys_hi;
	return MmMapIoSpace(pa, len, MmNonCached);
}

void kexos_mmio_unmap(void *va, kx_u32 len)
{
	if (va)
		MmUnmapIoSpace(va, len);
}

/* ---- DMA memory ---- */
void *kexos_dma_alloc(kx_u32 size, kx_u32 *phys)
{
	PHYSICAL_ADDRESS hi;
	void *va;
	hi.QuadPart = 0xffffffff;
	va = MmAllocateContiguousMemory(size, hi);
	if (!va)
		return NULL;
	memset(va, 0, size);
	if (phys)
		*phys = MmGetPhysicalAddress(va).LowPart;
	return va;
}

void kexos_dma_free(void *va, kx_u32 size)
{
	(void) size;
	if (va)
		MmFreeContiguousMemory(va);
}

void kexos_stall_us(kx_u32 us)
{
	while (us > 50)
	{
		KeStallExecutionProcessor(50);
		us -= 50;
	}
	if (us)
		KeStallExecutionProcessor(us);
}

/* ---- the log ring into the registry (what kexhda did for itself) ---- */
void kexos_log_save(const struct kexos_log *l)
{
	static WCHAR w[sizeof(l->buf) * 2 + 1];
	static char c[sizeof(l->buf)];
	static const WCHAR *const keys[] = { L"\\Registry\\Machine\\Software\\KernelEx",
	                                     L"\\Registry\\Machine\\Software\\KernelEx\\Diag" };
	UNICODE_STRING us, name;
	OBJECT_ATTRIBUTES oa;
	HANDLE h;
	ULONG disp, n, i, k = 0;
	int j;
	WCHAR wname[32];
	if (!l || !l->name)
		return;
	n = kexos_log_read(l, c, sizeof(c));
	for (i = 0; i < n; i++)
	{
		if (c[i] == '\n' && (i == 0 || c[i - 1] != '\r'))
			w[k++] = '\r';
		w[k++] = (WCHAR) (UCHAR) c[i];
	}
	w[k] = 0;
	for (j = 0; j < 31 && l->name[j]; j++)
		wname[j] = (WCHAR) (UCHAR) l->name[j];
	wname[j] = 0;
	for (j = 0; j < 2; j++)
	{
		RtlInitUnicodeString(&us, keys[j]);
		InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
		if (!NT_SUCCESS(ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp)))
			return;
		if (j == 1)
		{
			RtlInitUnicodeString(&name, wname);
			ZwSetValueKey(h, &name, 0, REG_SZ, w, (k + 1) * sizeof(WCHAR));
		}
		ZwClose(h);
	}
}
