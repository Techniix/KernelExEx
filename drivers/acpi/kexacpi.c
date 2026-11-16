/*
 *  KernelExEx - KEXACPI.VXD: laptop batteries, AC adapter and lid for
 *  Windows 98 SE / ME, read from the firmware's ACPI methods.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Windows 98 on a modern laptop runs without its own ACPI driver (it cannot
 *  cope with today's ACPI tables, and setup leaves it off), and the APM BIOS
 *  its battery meter relies on is gone. The battery is described only by
 *  ACPI control methods: _BIX (ACPI 4.0 and later) or _BIF for the fixed
 *  data, _BST for the charge. KEXACPI runs ACPICA (the reference AML
 *  interpreter, BSD-3/GPLv2) to evaluate them, with our own handler for the
 *  embedded controller (where laptops keep the battery data) and nothing
 *  else: the machine stays in legacy mode (no SCI, no events, no sleep
 *  states), and if Windows' own ACPI driver has the machine in ACPI mode,
 *  KEXACPI leaves it alone.
 *
 *  All work happens inside DeviceIoControl, in the calling program's thread,
 *  one call at a time: ACPICA starts on the first call.
 *
 *  References: ACPI Specification 6.5 (UEFI Forum): 10.2 control method
 *  batteries, 10.3 AC adapters, 9.4 lid, 12 embedded controller.
 */
#include "acpi.h"
#include "accommon.h"
#include "kexacpi.h"
#include "kexacpi_int.h"

#define _COMPONENT ACPI_OS_SERVICES
ACPI_MODULE_NAME("kexacpi")

/* ---- debug output ------------------------------------------------------------------- */
#ifdef KEX_DEBUGCON
#define dbg(...) AcpiOsPrintf(__VA_ARGS__)
#else
#define dbg(...) ((void) 0)
#endif

/* ---- state ---------------------------------------------------------------------------- */
static int started;               /* 0: not yet; else a KEXACPI_ST_* */
static ACPI_STATUS start_status;
static volatile LONG busy;

typedef struct
{
	ACPI_HANDLE h;
	int static_ok;                /* _BIX/_BIF read */
	KEXACPI_BATTERY b;
} BAT;

static BAT bats[KEXACPI_MAXBAT];
static int nbats;
static ACPI_HANDLE ac_dev, lid_dev;

static ACPI_STATUS eval_int(ACPI_HANDLE h, const char *name, UINT64 *v)
{
	ACPI_OBJECT o;
	ACPI_BUFFER buf = { sizeof(o), &o };
	ACPI_STATUS st = AcpiEvaluateObjectTyped(h, (char *) name, NULL, &buf, ACPI_TYPE_INTEGER);
	if (ACPI_SUCCESS(st))
		*v = o.Integer.Value;
	return st;
}

/* ---- the embedded controller ---------------------------------------------------------- */
/* ACPI 6.5, 12.2: status register bits; 12.3: commands */
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_RD_EC 0x80
#define EC_WR_EC 0x81

static struct
{
	ACPI_HANDLE h;
	USHORT data, cmd;
	int glk;                      /* _GLK: take the firmware's global lock around each access */
	int ok;
	ULONG errors;
	ULONG fails;                  /* time-outs in a row: after EC_GIVE_UP, the EC counts as dead */
} ec;

#define EC_GIVE_UP 8

static int ec_wait(UCHAR mask, UCHAR want)
{
	ULONG start = vmm_time_ms();
	for (;;)
	{
		UCHAR st = kex_inb(ec.cmd);
		if ((st & mask) == want)
			return 1;
		if (vmm_time_ms() - start > 200)
			return 0;
		AcpiOsStall(10);
	}
}

static int ec_glk_take(void)
{
	ULONG start;
	if (!ec.glk || !AcpiGbl_FACS)
		return 1;
	start = vmm_time_ms();
	while (!AcpiOsAcquireGlobalLock(AcpiGbl_FACS))
	{
		/* pending is set; the firmware gives it back when done. Without an
		 * SCI we cannot hear that, so poll (clearing our pending request
		 * first would race; AcpiOsAcquireGlobalLock retries the take) */
		if (vmm_time_ms() - start > 500)
			return 0;
		AcpiOsStall(100);
	}
	return 1;
}

static void ec_glk_give(void)
{
	if (ec.glk && AcpiGbl_FACS)
		AcpiOsReleaseGlobalLock(AcpiGbl_FACS);
}

static int ec_read(UCHAR addr, UCHAR *val)
{
	int ok = 0;
	if (!ec_glk_take())
		return 0;
	if (ec_wait(EC_IBF, 0))
	{
		kex_outb(ec.cmd, EC_RD_EC);
		if (ec_wait(EC_IBF, 0))
		{
			kex_outb(ec.data, addr);
			if (ec_wait(EC_OBF, EC_OBF))
			{
				*val = kex_inb(ec.data);
				ok = 1;
			}
		}
	}
	ec_glk_give();
	if (!ok)
		ec.errors++;
	return ok;
}

static int ec_write(UCHAR addr, UCHAR val)
{
	int ok = 0;
	if (!ec_glk_take())
		return 0;
	if (ec_wait(EC_IBF, 0))
	{
		kex_outb(ec.cmd, EC_WR_EC);
		if (ec_wait(EC_IBF, 0))
		{
			kex_outb(ec.data, addr);
			if (ec_wait(EC_IBF, 0))
			{
				kex_outb(ec.data, val);
				ok = ec_wait(EC_IBF, 0);
			}
		}
	}
	ec_glk_give();
	if (!ok)
		ec.errors++;
	return ok;
}

static ACPI_STATUS ec_space(UINT32 Function, ACPI_PHYSICAL_ADDRESS Address, UINT32 BitWidth, UINT64 *Value,
                            void *HandlerContext, void *RegionContext)
{
	ULONG i, n = BitWidth / 8, a = (ULONG) Address;
	(void) HandlerContext;
	(void) RegionContext;
	if (!n || a + n > 256)
		return AE_BAD_PARAMETER;
	/* an EC that keeps timing out would make every reading take seconds */
	if (ec.fails >= EC_GIVE_UP)
		return AE_TIME;
	if ((Function & ACPI_IO_MASK) == ACPI_READ)
		*Value = 0;
	for (i = 0; i < n; i++)
	{
		if ((Function & ACPI_IO_MASK) == ACPI_READ)
		{
			UCHAR v;
			if (!ec_read((UCHAR) (a + i), &v))
			{
				if (++ec.fails == EC_GIVE_UP)
					dbg("kexacpi: the embedded controller does not answer; giving up on it\n");
				return AE_TIME;
			}
			*Value |= (UINT64) v << (i * 8);
		}
		else if (!ec_write((UCHAR) (a + i), (UCHAR) (*Value >> (i * 8))))
		{
			if (++ec.fails == EC_GIVE_UP)
				dbg("kexacpi: the embedded controller does not answer; giving up on it\n");
			return AE_TIME;
		}
		ec.fails = 0;
	}
	return AE_OK;
}

static ACPI_STATUS ec_crs(ACPI_RESOURCE *r, void *ctx)
{
	int *n = (int *) ctx;
	USHORT port = 0;
	if (r->Type == ACPI_RESOURCE_TYPE_IO)
		port = r->Data.Io.Minimum;
	else if (r->Type == ACPI_RESOURCE_TYPE_FIXED_IO)
		port = r->Data.FixedIo.Address;
	else
		return AE_OK;
	/* the first I/O resource is the data port, the second the command/status port */
	if (*n == 0)
		ec.data = port;
	else if (*n == 1)
		ec.cmd = port;
	(*n)++;
	return AE_OK;
}

static ACPI_STATUS ec_found(ACPI_HANDLE h, UINT32 level, void *ctx, void **ret)
{
	int n = 0;
	(void) level;
	(void) ctx;
	(void) ret;
	if (ec.h)
		return AE_OK;
	if (ACPI_FAILURE(AcpiWalkResources(h, METHOD_NAME__CRS, ec_crs, &n)) || n < 2)
		return AE_OK;
	ec.h = h;
	return AE_CTRL_TERMINATE;
}

/* the EC from the ECDT (usable before the namespace runs), else the PNP0C09 device */
static void ec_start(void)
{
	ACPI_TABLE_ECDT *ecdt = NULL;
	ACPI_STATUS st;
	UINT64 glk = 0;
	if (ACPI_SUCCESS(AcpiGetTable(ACPI_SIG_ECDT, 1, (ACPI_TABLE_HEADER **) &ecdt)) && ecdt->Control.Address &&
	    ecdt->Data.Address)
	{
		ec.cmd = (USHORT) ecdt->Control.Address;
		ec.data = (USHORT) ecdt->Data.Address;
		if (ACPI_FAILURE(AcpiGetHandle(NULL, (char *) ecdt->Id, &ec.h)))
			ec.h = NULL;
	}
	if (!ec.h)
		AcpiGetDevices("PNP0C09", ec_found, NULL, NULL);
	if (!ec.h || !ec.cmd || !ec.data)
	{
		dbg("kexacpi: no embedded controller\n");
		ec.h = NULL;
		return;
	}
	if (ACPI_SUCCESS(eval_int(ec.h, "_GLK", &glk)))
		ec.glk = glk != 0;
	/* also runs the EC's _REG: the firmware learns the EC may be used */
	st = AcpiInstallAddressSpaceHandler(ec.h, ACPI_ADR_SPACE_EC, ec_space, NULL, NULL);
	ec.ok = ACPI_SUCCESS(st);
	dbg("kexacpi: embedded controller ports %x/%x, global lock %d: %s\n", ec.data, ec.cmd, ec.glk,
	    AcpiFormatException(st));
}

/* ---- batteries, AC, lid ------------------------------------------------------------------ */
static ULONG pkg_int(ACPI_OBJECT *p, ULONG i)
{
	if (i >= p->Package.Count || p->Package.Elements[i].Type != ACPI_TYPE_INTEGER)
		return KEXACPI_UNKNOWN;
	return (ULONG) p->Package.Elements[i].Integer.Value;
}

static void pkg_str(ACPI_OBJECT *p, ULONG i, char *out, ULONG size)
{
	ACPI_OBJECT *e;
	ULONG n = 0;
	out[0] = 0;
	if (i >= p->Package.Count)
		return;
	e = &p->Package.Elements[i];
	if (e->Type == ACPI_TYPE_STRING)
	{
		for (; n + 1 < size && n < e->String.Length && e->String.Pointer[n]; n++)
			out[n] = e->String.Pointer[n];
	}
	else if (e->Type == ACPI_TYPE_BUFFER)
	{
		for (; n + 1 < size && n < e->Buffer.Length && e->Buffer.Pointer[n]; n++)
			out[n] = (char) e->Buffer.Pointer[n];
	}
	else if (e->Type == ACPI_TYPE_INTEGER)
	{
		/* some firmware gives the serial number as a number */
		ULONG v = (ULONG) e->Integer.Value;
		char t[12];
		int k = 0;
		do
			t[k++] = (char) ('0' + v % 10);
		while ((v /= 10) && k < 11);
		while (k && n + 1 < size)
			out[n++] = t[--k];
	}
	out[n] = 0;
	while (n && out[n - 1] == ' ')
		out[--n] = 0;
}

/* the fixed data: _BIX when the firmware has it, else _BIF */
static void bat_static(BAT *bt)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT *p;
	KEXACPI_BATTERY *b = &bt->b;
	if (ACPI_SUCCESS(AcpiEvaluateObjectTyped(bt->h, "_BIX", NULL, &buf, ACPI_TYPE_PACKAGE)))
	{
		/* ACPI 6.5 10.2.2.2: revision, power unit, design capacity, last full,
		 * technology, design voltage, warning, low, cycle count, accuracy,
		 * max/min sampling time, max/min averaging interval, granularity 1 and 2,
		 * model, serial, type, OEM (revision 1 adds swapping capability) */
		p = (ACPI_OBJECT *) buf.Pointer;
		b->has_bix = 1;
		b->units = pkg_int(p, 1);
		b->design_cap = pkg_int(p, 2);
		b->full_cap = pkg_int(p, 3);
		b->rechargeable = pkg_int(p, 4);
		b->design_volt = pkg_int(p, 5);
		b->warn_cap = pkg_int(p, 6);
		b->low_cap = pkg_int(p, 7);
		b->cycles = pkg_int(p, 8);
		pkg_str(p, 16, b->model, sizeof(b->model));
		pkg_str(p, 17, b->serial, sizeof(b->serial));
		pkg_str(p, 18, b->type, sizeof(b->type));
		pkg_str(p, 19, b->oem, sizeof(b->oem));
		bt->static_ok = 1;
	}
	else if (ACPI_SUCCESS(AcpiEvaluateObjectTyped(bt->h, "_BIF", NULL, &buf, ACPI_TYPE_PACKAGE)))
	{
		/* 10.2.2.1: power unit, design capacity, last full, technology, design
		 * voltage, warning, low, granularity 1 and 2, model, serial, type, OEM */
		p = (ACPI_OBJECT *) buf.Pointer;
		b->has_bix = 0;
		b->units = pkg_int(p, 0);
		b->design_cap = pkg_int(p, 1);
		b->full_cap = pkg_int(p, 2);
		b->rechargeable = pkg_int(p, 3);
		b->design_volt = pkg_int(p, 4);
		b->warn_cap = pkg_int(p, 5);
		b->low_cap = pkg_int(p, 6);
		b->cycles = KEXACPI_UNKNOWN;
		pkg_str(p, 9, b->model, sizeof(b->model));
		pkg_str(p, 10, b->serial, sizeof(b->serial));
		pkg_str(p, 11, b->type, sizeof(b->type));
		pkg_str(p, 12, b->oem, sizeof(b->oem));
		bt->static_ok = 1;
	}
	if (buf.Pointer)
		ACPI_FREE(buf.Pointer);
}

static void bat_read(BAT *bt)
{
	KEXACPI_BATTERY *b = &bt->b;
	UINT64 sta = 0x1F;
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	/* _STA bit 4: a battery is in the bay (no _STA: always there) */
	if (ACPI_SUCCESS(eval_int(bt->h, "_STA", &sta)) && !(sta & 0x10))
	{
		b->present = 0;
		bt->static_ok = 0;   /* another battery may go in */
		b->percent = b->minutes_left = b->minutes_full = KEXACPI_UNKNOWN;
		return;
	}
	b->present = 1;
	if (!bt->static_ok)
		bat_static(bt);
	if (ACPI_SUCCESS(AcpiEvaluateObjectTyped(bt->h, "_BST", NULL, &buf, ACPI_TYPE_PACKAGE)))
	{
		/* 10.2.2.11: state, present rate, remaining capacity, present voltage */
		ACPI_OBJECT *p = (ACPI_OBJECT *) buf.Pointer;
		b->state = pkg_int(p, 0);
		b->rate = pkg_int(p, 1);
		b->remaining = pkg_int(p, 2);
		b->voltage = pkg_int(p, 3);
		ACPI_FREE(buf.Pointer);
	}
	else
		b->state = b->rate = b->remaining = b->voltage = KEXACPI_UNKNOWN;
	if (b->state == KEXACPI_UNKNOWN)
		b->state = 0;
	/* the rate's sign is not given: some firmware reports a negative value
	 * (a large unsigned number) or the top bit set while discharging */
	if (b->rate != KEXACPI_UNKNOWN && (b->rate & 0x80000000u))
		b->rate = (ULONG) -(LONG) b->rate;
	b->percent = KEXACPI_UNKNOWN;
	{
		ULONG full = b->full_cap;
		if (full == KEXACPI_UNKNOWN || !full)
			full = b->design_cap;
		if (full && full != KEXACPI_UNKNOWN && b->remaining != KEXACPI_UNKNOWN)
		{
			ULONG pc = (ULONG) (((unsigned long long) b->remaining * 100 + full / 2) / full);
			b->percent = pc > 100 ? 100 : pc;
		}
		b->minutes_left = b->minutes_full = KEXACPI_UNKNOWN;
		if (b->rate && b->rate != KEXACPI_UNKNOWN && b->remaining != KEXACPI_UNKNOWN)
		{
			if (b->state & KEXACPI_BAT_DISCHARGING)
				b->minutes_left = (ULONG) ((unsigned long long) b->remaining * 60 / b->rate);
			else if ((b->state & KEXACPI_BAT_CHARGING) && full != KEXACPI_UNKNOWN && full > b->remaining)
				b->minutes_full = (ULONG) ((unsigned long long) (full - b->remaining) * 60 / b->rate);
		}
	}
}

static ACPI_STATUS bat_found(ACPI_HANDLE h, UINT32 level, void *ctx, void **ret)
{
	ACPI_BUFFER name = { sizeof(bats[0].b.path), NULL };
	(void) level;
	(void) ctx;
	(void) ret;
	if (nbats >= KEXACPI_MAXBAT)
		return AE_CTRL_TERMINATE;
	bats[nbats].h = h;
	name.Pointer = bats[nbats].b.path;
	AcpiGetName(h, ACPI_FULL_PATHNAME, &name);
	dbg("kexacpi: battery %s\n", bats[nbats].b.path);
	nbats++;
	return AE_OK;
}

static ACPI_STATUS one_found(ACPI_HANDLE h, UINT32 level, void *ctx, void **ret)
{
	(void) level;
	(void) ret;
	if (!*(ACPI_HANDLE *) ctx)
		*(ACPI_HANDLE *) ctx = h;
	return AE_CTRL_TERMINATE;
}

/* ---- starting ---------------------------------------------------------------------------- */
static int start(void)
{
	ACPI_STATUS st;
	UINT32 sci = 0;
	if ((st = AcpiInitializeSubsystem()) != AE_OK || (st = AcpiInitializeTables(NULL, 16, FALSE)) != AE_OK)
	{
		start_status = st;
		dbg("kexacpi: no ACPI tables: %s\n", AcpiFormatException(st));
		return st == AE_NOT_FOUND ? KEXACPI_ST_NOACPI : KEXACPI_ST_FAILED;
	}
	/* in ACPI mode with an SMI command port means someone switched it there:
	 * Windows' own ACPI driver. Stay out of its way */
	if (ACPI_SUCCESS(AcpiReadBitRegister(ACPI_BITREG_SCI_ENABLE, &sci)) && sci && AcpiGbl_FADT.SmiCommand)
	{
		dbg("kexacpi: the machine is in ACPI mode (Windows' ACPI driver): leaving it alone\n");
		return KEXACPI_ST_WINACPI;
	}
	if ((st = AcpiLoadTables()) != AE_OK ||
	    (st = AcpiEnableSubsystem(ACPI_NO_ACPI_ENABLE | ACPI_NO_EVENT_INIT | ACPI_NO_HANDLER_INIT)) != AE_OK)
	{
		start_status = st;
		dbg("kexacpi: loading the ACPI tables failed: %s\n", AcpiFormatException(st));
		return KEXACPI_ST_FAILED;
	}
	ec_start();
	if ((st = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION)) != AE_OK)
	{
		/* _INI methods failing is not fatal for reading batteries */
		start_status = st;
		dbg("kexacpi: initialising the namespace: %s\n", AcpiFormatException(st));
	}
	AcpiGetDevices("PNP0C0A", bat_found, NULL, NULL);
	AcpiGetDevices("ACPI0003", one_found, &ac_dev, NULL);
	AcpiGetDevices("PNP0C0D", one_found, &lid_dev, NULL);
	dbg("kexacpi: %d batteries, AC adapter %s, lid %s\n", nbats, ac_dev ? "yes" : "no", lid_dev ? "yes" : "no");
	return KEXACPI_ST_RUNNING;
}

static void fill(KEXACPI_STATUS *s)
{
	int i;
	UINT64 v;
	memset(s, 0, sizeof(*s));
	s->size = sizeof(*s);
	s->version = KEXACPI_VERSION;
	s->state = (ULONG) started;
	s->acpi_status = start_status;
	s->ac = s->lid = -1;
	if (started != KEXACPI_ST_RUNNING)
		return;
	s->have_ec = ec.ok;
	if (ac_dev && ACPI_SUCCESS(eval_int(ac_dev, "_PSR", &v)))
		s->ac = v ? 1 : 0;
	if (lid_dev && ACPI_SUCCESS(eval_int(lid_dev, "_LID", &v)))
		s->lid = v ? 1 : 0;
	for (i = 0; i < nbats; i++)
	{
		bat_read(&bats[i]);
		s->bat[i] = bats[i].b;
	}
	s->nbat = (ULONG) nbats;
}

/* ---- the VxD ----------------------------------------------------------------------------- */
#define SYS_DYNAMIC_DEVICE_INIT 0x1B
#define SYS_DYNAMIC_DEVICE_EXIT 0x1C
#define W32_DEVICEIOCONTROL 0x23
#define ERROR_BUSY 170
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_NOT_SUPPORTED 50
#define ERROR_NOT_ENOUGH_MEMORY 8

typedef struct
{
	ULONG Internal1, VMHandle, Internal2, dwIoControlCode, lpvInBuffer, cbInBuffer, lpvOutBuffer, cbOutBuffer,
	    lpcbBytesReturned, lpoOverlapped, hDevice, tagProcess;
} DIOCPARAMS;

/* ACPICA nests deeply while running AML; a ring-0 thread stack is a few KB
 * (Windows 98 does not grow it), so all ACPICA work runs on our own stack */
#define BIG_STACK 0x10000
static UCHAR *big_stack;

static ULONG __attribute__((noinline)) on_big_stack(ULONG (*fn)(void *), void *arg)
{
	ULONG r;
	__asm__ __volatile__("movl %%esp, %%ebx\n\t"
	                     "movl %3, %%esp\n\t"
	                     "pushl %%ebx\n\t"
	                     "pushl %2\n\t"
	                     "call *%1\n\t"
	                     "addl $4, %%esp\n\t"
	                     "popl %%esp"
	                     : "=a"(r)
	                     : "r"(fn), "r"(arg), "r"(big_stack + BIG_STACK - 16)
	                     : "ebx", "ecx", "edx", "memory", "cc");
	return r;
}

static ULONG do_start(void *unused)
{
	(void) unused;
	return (ULONG) start();
}

static ULONG do_fill(void *s)
{
	fill((KEXACPI_STATUS *) s);
	return 0;
}

static ULONG ioctl(DIOCPARAMS *p)
{
	ULONG r = 0;
	if (p->dwIoControlCode == 0 || p->dwIoControlCode == (ULONG) -1)
		return 0;   /* open and close */
	if (__sync_lock_test_and_set(&busy, 1))
		return ERROR_BUSY;
	if (!big_stack && !(big_stack = (UCHAR *) vmm_heap_alloc(BIG_STACK)))
	{
		__sync_lock_release(&busy);
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	if (!started)
	{
		started = (int) on_big_stack(do_start, NULL);
		dbg("kexacpi: started (%d)\n", started);
	}
	switch (p->dwIoControlCode)
	{
	case KEXACPI_IOCTL_STATUS:
		if (p->cbOutBuffer < sizeof(KEXACPI_STATUS) || !p->lpvOutBuffer)
		{
			r = ERROR_INSUFFICIENT_BUFFER;
			break;
		}
		on_big_stack(do_fill, (void *) p->lpvOutBuffer);
		if (p->lpcbBytesReturned)
			*(ULONG *) p->lpcbBytesReturned = sizeof(KEXACPI_STATUS);
		break;
	case KEXACPI_IOCTL_REREAD:
	{
		int i;
		for (i = 0; i < nbats; i++)
			bats[i].static_ok = 0;
		ec.fails = 0;
		break;
	}
	default:
		r = ERROR_NOT_SUPPORTED;
	}
	__sync_lock_release(&busy);
	return r;
}

__attribute__((used)) ULONG __stdcall kexacpi_dispatch(ULONG msg, ULONG ebx, ULONG ecx, ULONG edx, ULONG esi,
                                                       ULONG edi)
{
	(void) ebx;
	(void) ecx;
	(void) edx;
	(void) edi;
	switch (msg)
	{
	case W32_DEVICEIOCONTROL:
		return ioctl((DIOCPARAMS *) esi);
	case SYS_DYNAMIC_DEVICE_EXIT:
		return 0;   /* stay loaded: ACPICA's state and mappings cannot be given back */
	default:
		return 1;
	}
}

/* the control procedure: message in EAX; for W32_DEVICEIOCONTROL the result
 * goes back in EAX (0 = success), for the rest carry clear = success */
__attribute__((naked)) void KEXACPI_CTRL(void)
{
	__asm__("pushl %eax\n\t"
	        "pushl %edi\n\t"
	        "pushl %esi\n\t"
	        "pushl %edx\n\t"
	        "pushl %ecx\n\t"
	        "pushl %ebx\n\t"
	        "pushl %eax\n\t"
	        "call _kexacpi_dispatch@24\n\t"
	        "popl %ecx\n\t"
	        "cmpl $0x23, %ecx\n\t"
	        "je 1f\n\t"
	        "cmpl $1, %eax\n\t"
	        "ret\n"
	        "1:\n\t"
	        "clc\n\t"
	        "ret");
}

struct VxD_Desc_Block
{
	ULONG DDB_Next;
	USHORT DDB_SDK_Version, DDB_Req_Device_Number;
	UCHAR DDB_Dev_Major_Version, DDB_Dev_Minor_Version;
	USHORT DDB_Flags;
	UCHAR DDB_Name[8];
	ULONG DDB_Init_Order, DDB_Control_Proc, DDB_V86_API_Proc, DDB_PM_API_Proc, DDB_V86_API_CSIP,
	    DDB_PM_API_CSIP, DDB_Reference_Data, DDB_Service_Table_Ptr, DDB_Service_Table_Size, DDB_Win32_Service_Table,
	    DDB_Prev, DDB_Size, DDB_Reserved1, DDB_Reserved2, DDB_Reserved3;
};

struct VxD_Desc_Block KEXACPI_DDB = {
	0, 0x030A, 0x0000, 1, 0, 0,
	{ 'K', 'E', 'X', 'A', 'C', 'P', 'I', ' ' }, 0x80000000u, (ULONG) KEXACPI_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x50726576, sizeof(struct VxD_Desc_Block),
	0x52737631, 0x52737632, 0x52737633,
};
