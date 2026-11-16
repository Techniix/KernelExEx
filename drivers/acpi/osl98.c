/*
 *  KernelExEx - KEXACPI.VXD: ACPICA's operating-system layer for Windows 98
 *  ring 0.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  ACPICA runs only inside the VxD's DeviceIoControl handler, one call at a
 *  time (kexacpi.c serialises them), in the calling program's thread: no
 *  interrupts, no events, no threads of its own. So locks are no-ops
 *  (ACPI_SINGLE_THREADED), waiting is busy waiting, and AcpiOsExecute runs
 *  the work at once.
 *
 *  Physical memory: the VMM's _MapPhysToLinear never gives linear space
 *  back, so every mapping is kept and reused (tables and operation regions
 *  touch a fixed, small set of pages).
 */
#include "acpi.h"
#include "accommon.h"
#include "kexacpi_int.h"

#define _COMPONENT ACPI_OS_SERVICES
ACPI_MODULE_NAME("osl98")

/* ---- VMM services --------------------------------------------------------------------- */
void *vmm_heap_alloc(ULONG n)
{
	void *p;
	__asm__ __volatile__("pushl $1\n\t"   /* HEAPZEROINIT */
	                     "pushl %1\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x0001004F\n\t"   /* _HeapAllocate */
	                     "addl $8, %%esp"
	                     : "=a"(p)
	                     : "r"(n)
	                     : "ecx", "edx", "memory", "cc");
	return p;
}

void vmm_heap_free(void *p)
{
	__asm__ __volatile__("pushl $0\n\t"
	                     "pushl %0\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00010051\n\t"   /* _HeapFree */
	                     "addl $8, %%esp"
	                     :
	                     : "r"(p)
	                     : "eax", "ecx", "edx", "memory", "cc");
}

static void *vmm_map_phys(ULONG pa, ULONG len)
{
	void *p;
	__asm__ __volatile__("pushl $0\n\t"
	                     "pushl %2\n\t"
	                     "pushl %1\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x0001006C\n\t"   /* _MapPhysToLinear */
	                     "addl $12, %%esp"
	                     : "=a"(p)
	                     : "r"(pa), "r"(len)
	                     : "ecx", "edx", "memory", "cc");
	return p;
}

/* milliseconds since Windows started */
ULONG vmm_time_ms(void)
{
	ULONG t;
	__asm__ __volatile__("int $0x20\n\t"
	                     ".long 0x0001003F"   /* Get_System_Time */
	                     : "=a"(t)
	                     :
	                     : "memory", "cc");
	return t;
}

/* ---- 64-bit division (clang calls these for 32-bit x86) --------------------------- */
static unsigned long long udivmod64(unsigned long long n, unsigned long long d, unsigned long long *rem)
{
	unsigned long long q = 0, r = 0;
	int i;
	if (!d)
	{
		if (rem)
			*rem = 0;
		return 0;
	}
	if (!(d >> 32) && !(n >> 32))
	{
		if (rem)
			*rem = (ULONG) n % (ULONG) d;
		return (ULONG) n / (ULONG) d;
	}
	for (i = 63; i >= 0; i--)
	{
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d)
		{
			r -= d;
			q |= 1ULL << i;
		}
	}
	if (rem)
		*rem = r;
	return q;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d)
{
	return udivmod64(n, d, 0);
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d)
{
	unsigned long long r;
	udivmod64(n, d, &r);
	return r;
}

long long __divdi3(long long n, long long d)
{
	int neg = (n < 0) ^ (d < 0);
	unsigned long long q = udivmod64(n < 0 ? -(unsigned long long) n : (unsigned long long) n,
	                                 d < 0 ? -(unsigned long long) d : (unsigned long long) d, 0);
	return neg ? -(long long) q : (long long) q;
}

long long __moddi3(long long n, long long d)
{
	unsigned long long r;
	udivmod64(n < 0 ? -(unsigned long long) n : (unsigned long long) n,
	          d < 0 ? -(unsigned long long) d : (unsigned long long) d, &r);
	return n < 0 ? -(long long) r : (long long) r;
}

/* ---- debug output (QEMU -debugcon) ---------------------------------------------------- */
void kex_dbg_char(char c)
{
#ifdef KEX_DEBUGCON
	__asm__ __volatile__("outb %%al, $0xe9" : : "a"(c));
#else
	(void) c;
#endif
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *Fmt, ...)
{
	va_list ap;
	va_start(ap, Fmt);
	AcpiOsVprintf(Fmt, ap);
	va_end(ap);
}

void AcpiOsVprintf(const char *Fmt, va_list Args)
{
#ifdef KEX_DEBUGCON
	char buf[256];
	char *p;
	vsnprintf(buf, sizeof(buf), Fmt, Args);
	for (p = buf; *p; p++)
		kex_dbg_char(*p);
#else
	(void) Fmt;
	(void) Args;
#endif
}

void AcpiOsRedirectOutput(void *Destination)
{
	(void) Destination;
}

/* ---- start, tables ------------------------------------------------------------------------ */
ACPI_STATUS AcpiOsInitialize(void)
{
	return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
	return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
	ACPI_PHYSICAL_ADDRESS rsdp = 0;
	AcpiFindRootPointer(&rsdp);
	return rsdp;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal, ACPI_STRING *NewVal)
{
	(void) InitVal;
	*NewVal = NULL;
	return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_TABLE_HEADER **NewTable)
{
	(void) ExistingTable;
	*NewTable = NULL;
	return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_PHYSICAL_ADDRESS *NewAddress,
                                        UINT32 *NewTableLength)
{
	(void) ExistingTable;
	*NewAddress = 0;
	*NewTableLength = 0;
	return AE_OK;
}

/* ---- locks (single-threaded: nothing to do) ------------------------------------------- */
ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
	*OutHandle = (ACPI_SPINLOCK) 1;
	return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
	(void) Handle;
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
	(void) Handle;
	return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
	(void) Handle;
	(void) Flags;
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE *OutHandle)
{
	(void) MaxUnits;
	(void) InitialUnits;
	*OutHandle = (ACPI_SEMAPHORE) 1;
	return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
{
	(void) Handle;
	return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout)
{
	(void) Handle;
	(void) Units;
	(void) Timeout;
	return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units)
{
	(void) Handle;
	(void) Units;
	return AE_OK;
}

/* ---- memory ------------------------------------------------------------------------------------ */
void *AcpiOsAllocate(ACPI_SIZE Size)
{
	return vmm_heap_alloc((ULONG) Size ? (ULONG) Size : 1);
}

void AcpiOsFree(void *Memory)
{
	if (Memory)
		vmm_heap_free(Memory);
}

/* mappings kept for good: physical page range -> linear address */
#define NMAP 64
static struct
{
	ULONG pa, len;
	UCHAR *lin;
} maps[NMAP];
static int nmaps;

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
	ULONG pa = (ULONG) Where, len = (ULONG) Length, base, end;
	int i;
	if (!len)
		len = 1;
	for (i = 0; i < nmaps; i++)
		if (pa >= maps[i].pa && pa + len <= maps[i].pa + maps[i].len)
			return maps[i].lin + (pa - maps[i].pa);
	/* whole pages, and at least 64 KB so neighbouring requests share it */
	base = pa & ~0xFFFu;
	end = (pa + len + 0xFFFu) & ~0xFFFu;
	if (end - base < 0x10000)
		end = base + 0x10000;
	{
		UCHAR *lin = (UCHAR *) vmm_map_phys(base, end - base);
		if (!lin || lin == (UCHAR *) -1)
			return NULL;
		if (nmaps < NMAP)
		{
			maps[nmaps].pa = base;
			maps[nmaps].len = end - base;
			maps[nmaps].lin = lin;
			nmaps++;
		}
		return lin + (pa - base);
	}
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Size)
{
	(void) LogicalAddress;
	(void) Size;
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress, ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
	int i;
	for (i = 0; i < nmaps; i++)
		if ((UCHAR *) LogicalAddress >= maps[i].lin && (UCHAR *) LogicalAddress < maps[i].lin + maps[i].len)
		{
			*PhysicalAddress = maps[i].pa + ((UCHAR *) LogicalAddress - maps[i].lin);
			return AE_OK;
		}
	return AE_ERROR;
}

BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length)
{
	(void) Pointer;
	(void) Length;
	return TRUE;
}

BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length)
{
	(void) Pointer;
	(void) Length;
	return TRUE;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width)
{
	void *p = AcpiOsMapMemory(Address, Width / 8);
	if (!p)
		return AE_ERROR;
	switch (Width)
	{
	case 8:
		*Value = *(volatile UINT8 *) p;
		break;
	case 16:
		*Value = *(volatile UINT16 *) p;
		break;
	case 32:
		*Value = *(volatile UINT32 *) p;
		break;
	case 64:
		*Value = *(volatile UINT64 *) p;
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width)
{
	void *p = AcpiOsMapMemory(Address, Width / 8);
	if (!p)
		return AE_ERROR;
	switch (Width)
	{
	case 8:
		*(volatile UINT8 *) p = (UINT8) Value;
		break;
	case 16:
		*(volatile UINT16 *) p = (UINT16) Value;
		break;
	case 32:
		*(volatile UINT32 *) p = (UINT32) Value;
		break;
	case 64:
		*(volatile UINT64 *) p = Value;
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

/* ---- ports, PCI ------------------------------------------------------------------------- */
static UCHAR inb(USHORT p)
{
	UCHAR v;
	__asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(p));
	return v;
}

static USHORT inw(USHORT p)
{
	USHORT v;
	__asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(p));
	return v;
}

static ULONG inl(USHORT p)
{
	ULONG v;
	__asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(p));
	return v;
}

static void outb(USHORT p, UCHAR v)
{
	__asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(p));
}

static void outw(USHORT p, USHORT v)
{
	__asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(p));
}

static void outl(USHORT p, ULONG v)
{
	__asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(p));
}

UCHAR kex_inb(USHORT p)
{
	return inb(p);
}

void kex_outb(USHORT p, UCHAR v)
{
	outb(p, v);
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
	USHORT p = (USHORT) Address;
	switch (Width)
	{
	case 8:
		*Value = inb(p);
		break;
	case 16:
		*Value = inw(p);
		break;
	case 32:
		*Value = inl(p);
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
{
	USHORT p = (USHORT) Address;
	switch (Width)
	{
	case 8:
		outb(p, (UCHAR) Value);
		break;
	case 16:
		outw(p, (USHORT) Value);
		break;
	case 32:
		outl(p, Value);
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

/* configuration mechanism #1, with interrupts off around the address/data pair */
static ULONG pci_addr(ACPI_PCI_ID *id, UINT32 reg)
{
	return 0x80000000u | ((ULONG) id->Bus << 16) | ((ULONG) id->Device << 11) | ((ULONG) id->Function << 8) |
	       (reg & 0xFC);
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 *Value, UINT32 Width)
{
	ULONG v;
	if (Reg > 0xFF || PciId->Segment)
		return AE_BAD_PARAMETER;
	__asm__ __volatile__("pushfl\n\tcli" ::: "memory");
	outl(0xCF8, pci_addr(PciId, Reg));
	switch (Width)
	{
	case 8:
		v = inb((USHORT) (0xCFC + (Reg & 3)));
		break;
	case 16:
		v = inw((USHORT) (0xCFC + (Reg & 2)));
		break;
	default:
		v = inl(0xCFC);
		break;
	}
	__asm__ __volatile__("popfl" ::: "memory", "cc");
	*Value = v;
	return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 Value, UINT32 Width)
{
	if (Reg > 0xFF || PciId->Segment)
		return AE_BAD_PARAMETER;
	__asm__ __volatile__("pushfl\n\tcli" ::: "memory");
	outl(0xCF8, pci_addr(PciId, Reg));
	switch (Width)
	{
	case 8:
		outb((USHORT) (0xCFC + (Reg & 3)), (UCHAR) Value);
		break;
	case 16:
		outw((USHORT) (0xCFC + (Reg & 2)), (USHORT) Value);
		break;
	default:
		outl(0xCFC, (ULONG) Value);
		break;
	}
	__asm__ __volatile__("popfl" ::: "memory", "cc");
	return AE_OK;
}

/* ---- time ---------------------------------------------------------------------------------- */
void AcpiOsStall(UINT32 Microseconds)
{
	/* an ISA bus write takes about a microsecond */
	while (Microseconds--)
		outb(0x80, 0);
}

void AcpiOsSleep(UINT64 Milliseconds)
{
	ULONG start = vmm_time_ms(), ms = (ULONG) Milliseconds;
	while (vmm_time_ms() - start < ms)
		AcpiOsStall(50);
}

UINT64 AcpiOsGetTimer(void)
{
	return (UINT64) vmm_time_ms() * 10000;   /* 100 ns units */
}

/* ---- threads, interrupts, events (none here) --------------------------------------- */
ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
	return 1;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context)
{
	(void) Type;
	Function(Context);
	return AE_OK;
}

void AcpiOsWaitEventsComplete(void)
{
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context)
{
	(void) InterruptNumber;
	(void) ServiceRoutine;
	(void) Context;
	return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine)
{
	(void) InterruptNumber;
	(void) ServiceRoutine;
	return AE_OK;
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info)
{
	(void) Function;
	(void) Info;
	return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue)
{
	(void) SleepState;
	(void) RegaValue;
	(void) RegbValue;
	return AE_OK;
}

/* ---- the FACS global lock (shared with the firmware's SMM code) ------------------------ */
int AcpiOsAcquireGlobalLock(void *Facs)
{
	volatile ULONG *gl = &((ACPI_TABLE_FACS *) Facs)->GlobalLock;
	ULONG old, nw;
	do
	{
		old = *gl;
		/* owned: set pending; free: take it */
		nw = ((old & ~3u) | 2u) | ((old >> 1) & 1u);
	} while (!__sync_bool_compare_and_swap(gl, old, nw));
	return (nw & 3u) != 3u;   /* acquired unless it was owned (then pending is set) */
}

int AcpiOsReleaseGlobalLock(void *Facs)
{
	volatile ULONG *gl = &((ACPI_TABLE_FACS *) Facs)->GlobalLock;
	ULONG old, nw;
	do
	{
		old = *gl;
		nw = old & ~3u;
	} while (!__sync_bool_compare_and_swap(gl, old, nw));
	return (old & 1u) != 0;   /* the firmware waits for it */
}
