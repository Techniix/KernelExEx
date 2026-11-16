/*
 *  KernelExEx - KEXFATP.VXD: a probe for the exFAT file system driver's
 *  development (test builds only, never installed by setup).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Registers a mount routine with IFSMgr and, for every volume IFSMgr asks
 *  about, writes the raw request (ioreq) and what it points to (the IOS
 *  volume request parameters, VRP, and the device control block, DCB) to
 *  the debug port 0xE9 (QEMU -debugcon), then declines the volume. This is
 *  how the layouts the real driver relies on are checked on Windows 98
 *  itself, instead of taken on trust.
 */
#include <windows.h>
#include <vmm.h>

/* ---- the debug port ------------------------------------------------------------------- */
static void outc(char c)
{
	__asm__ __volatile__("outb %%al, $0xe9" : : "a"(c));
}

static void outs(const char *s)
{
	while (*s)
		outc(*s++);
}

static void outx(ULONG v, int digits)
{
	int i;
	for (i = digits - 1; i >= 0; i--)
		outc("0123456789abcdef"[(v >> (i * 4)) & 15]);
}

static void dump(const char *what, const void *p, ULONG n)
{
	const UCHAR *b = (const UCHAR *) p;
	ULONG i;
	outs(what);
	outs(" @");
	outx((ULONG) p, 8);
	for (i = 0; i < n; i++)
	{
		if (!(i & 15))
		{
			outs("\r\n  ");
			outx(i, 3);
			outs(":");
		}
		outc(' ');
		outx(b[i], 2);
	}
	outs("\r\n");
}

/* a pointer that is in the system arena (ring-0 heap and VxD memory) */
static int sys_ptr(ULONG p)
{
	return p >= 0xC0000000u && p < 0xFFFF0000u;
}

/* ---- IFSMgr ----------------------------------------------------------------------------- */
#define IFSMGR_REGISTERMOUNT 0x00400001
#define IFSMGRVERSION 0x22
#define NORMAL_FSD 0

static ULONG mounts;

/* ---- watching the I/O requests other drivers send ------------------------------------ */
__attribute__((used)) ULONG prev_send;   /* IOS_SendCommand before our hook */
__attribute__((used)) ULONG orig_fsd_entry;   /* the VRPs' fsd_entry (one IOS routine) */
static ULONG n_send, n_fsd;

struct pushal_regs;
static int armed;   /* set by the first mount request: boot-time traffic is not logged */
__attribute__((used)) ULONG cb_orig;   /* the callback the logged requests carry */
static ULONG n_cb;
void cb_hook(void);

/* one good read as VFAT sent it, replayed at the exFAT mount (does a known-good
 * request succeed when we send it, and when we send it to the exFAT volume?) */
static UCHAR saved[1024], replay_pkt[1024], replay_buf[4096];
static ULONG saved_len, saved_delta, saved_edi, saved_iop;
static volatile int replay_done;
static int zero_hdr;

__attribute__((used)) void __cdecl replay_cb_c(void)
{
	replay_done = 1;
}

__attribute__((naked)) void replay_cb(void)
{
	__asm__("pushal\n\t"
	        "call _replay_cb_c\n\t"
	        "popal\n\t"
	        "ret");
}

static void capture(ULONG ior, ULONG edi)
{
	ULONG delta;
	if (saved_len || !sys_ptr(ior) || *(USHORT *) (ior + 4) != 0 || !sys_ptr(*(ULONG *) (ior + 0x0C)) ||
	    *(ULONG *) (ior + 0x18) != 1)
		return;
	delta = *(ULONG *) (ior + 0x20);
	if (!delta || delta > 0x200)
		return;
	saved_delta = delta;
	saved_len = delta + 0x100;
	saved_edi = edi;
	saved_iop = ior - delta;
	memcpy(saved, (void *) saved_iop, saved_len);
	outs("kexfatp: captured a one-sector read, drive ");
	outx(*(UCHAR *) (ior + 0x4D), 2);
	outs(" handle ");
	outx(edi, 8);
	outs("\r\n");
}

static void ifs_yield(void)
{
	__asm__ __volatile__("int $0x20\n\t"
	                     ".long 0x00400014"
	                     :
	                     :
	                     : "eax", "ecx", "edx", "memory", "cc");
}

static void replay(ULONG edi, ULONG start, UCHAR drive, const char *what)
{
	UCHAR *ior = replay_pkt + saved_delta;
	ULONG i, n = 0;
	memcpy(replay_pkt, saved, saved_len);
	/* pointers into the old packet move with it */
	for (i = 0; i + 4 <= saved_len; i += 4)
	{
		ULONG v = *(ULONG *) (replay_pkt + i);
		if (v >= saved_iop && v < saved_iop + saved_len)
		{
			*(ULONG *) (replay_pkt + i) = v - saved_iop + (ULONG) replay_pkt;
			n++;
		}
	}
	if (zero_hdr)
		memset(replay_pkt, 0, saved_delta);
	*(USHORT *) (ior + 6) = 0;
	*(ULONG *) (ior + 0x0C) = (ULONG) replay_cb;
	*(ULONG *) (ior + 0x10) = start;
	*(ULONG *) (ior + 0x1C) = (ULONG) replay_buf;
	if (drive != 0xFF)
		*(UCHAR *) (ior + 0x4D) = drive;
	memset(replay_buf, 0, sizeof replay_buf);
	replay_done = 0;
	__asm__ __volatile__("pushal\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00100004\n\t"
	                     "popal"
	                     :
	                     : "S"(ior), "D"(edi)
	                     : "memory", "cc");
	for (i = 0; !replay_done && i < 2000; i++)
		ifs_yield();
	outs("kexfatp: replay ");
	outs(what);
	outs(" (");
	outx(n, 2);
	outs(" pointers moved): done ");
	outx(replay_done, 1);
	outs(" status ");
	outx(*(USHORT *) (ior + 6), 4);
	outs(", bytes 3..10 ");
	for (i = 3; i < 11; i++)
		outc(replay_buf[i] >= 32 && replay_buf[i] < 127 ? (char) replay_buf[i] : '.');
	outs(" sig ");
	outx(*(USHORT *) (replay_buf + 510), 4);
	outs(" part0 ");
	for (i = 0x1BE; i < 0x1CE; i++)
		outx(replay_buf[i], 2);
	outs(" flags ");
	outx(*(ULONG *) (ior + 8), 8);
	outs("\r\n");
}

__attribute__((used)) void __cdecl log_send(ULONG ior, ULONG dcb)
{
	capture(ior, dcb);
	/* everything except C: (drive 2), from boot on: how VFAT mounts removable media */
	if (!sys_ptr(ior) || *(UCHAR *) (ior + 0x4D) == 2 || ++n_send > 40)
		return;
	/* follow the completion: the callback is wrapped once we know its target */
	if (sys_ptr(ior) && sys_ptr(*(ULONG *) (ior + 0x0C)))
	{
		ULONG cb = *(ULONG *) (ior + 0x0C);
		if (!cb_orig)
			cb_orig = cb;
		if (cb == cb_orig)
			*(ULONG *) (ior + 0x0C) = (ULONG) cb_hook;
	}
	outs("IOS_SendCommand #");
	outx(n_send, 2);
	outs(" dcb ");
	outx(dcb, 8);
	if (sys_ptr(dcb))
	{
		outs(" drive ");
		outx(*(UCHAR *) (dcb + 0x14), 2);
	}
	outs("\r\n");
	if (sys_ptr(ior))
	{
		ULONG delta = *(ULONG *) (ior + 0x20);
		if (delta && delta < 0x200)
			dump("  iop header", (void *) (ior - delta), delta);
		dump("  ior", (void *) ior, 0x58);
	}
}

/* the registers as pushal left them, then the caller's return address and arguments */
struct pushal
{
	ULONG edi, esi, ebp, esp, ebx, edx, ecx, eax;
	ULONG ret, arg1, arg2, arg3;
};

__attribute__((used)) void __cdecl log_fsd(struct pushal *r)
{
	if (++n_fsd > 24)
		return;
	outs("fsd_entry #");
	outx(n_fsd, 2);
	outs(" ret ");
	outx(r->ret, 8);
	outs(" args ");
	outx(r->arg1, 8);
	outc(' ');
	outx(r->arg2, 8);
	outs(" eax ");
	outx(r->eax, 8);
	outs(" esi ");
	outx(r->esi, 8);
	outs(" edi ");
	outx(r->edi, 8);
	outs("\r\n");
	if (sys_ptr(r->arg1))
		dump("  arg1", (void *) r->arg1, 0x18C);
}

struct pushal_regs
{
	ULONG edi, esi, ebp, esp, ebx, edx, ecx, eax;
	ULONG ret, arg1, arg2;
};

__attribute__((used)) void __cdecl log_cb(struct pushal_regs *r)
{
	if (++n_cb > 30)
		return;
	outs("callback #");
	outx(n_cb, 2);
	outs(" eax ");
	outx(r->eax, 8);
	outs(" ebx ");
	outx(r->ebx, 8);
	outs(" ecx ");
	outx(r->ecx, 8);
	outs(" edx ");
	outx(r->edx, 8);
	outs(" esi ");
	outx(r->esi, 8);
	outs(" edi ");
	outx(r->edi, 8);
	outs(" ebp ");
	outx(r->ebp, 8);
	outs(" ret ");
	outx(r->ret, 8);
	outs(" arg1 ");
	outx(r->arg1, 8);
	outs("\r\n");
	if (sys_ptr(r->esi))
		dump("  esi", (void *) r->esi, 0x20);
}

__attribute__((naked)) void cb_hook(void)
{
	__asm__("pushal\n\t"
	        "pushl %esp\n\t"
	        "call _log_cb\n\t"
	        "addl $4, %esp\n\t"
	        "popal\n\t"
	        "jmp *_cb_orig");
}

__attribute__((naked)) void send_hook(void)
{
	__asm__("pushal\n\t"
	        "pushl %edi\n\t"
	        "pushl %esi\n\t"
	        "call _log_send\n\t"
	        "addl $8, %esp\n\t"
	        "popal\n\t"
	        "jmp *_prev_send");
}

__attribute__((naked)) void fsd_entry_hook(void)
{
	__asm__("pushal\n\t"
	        "pushl %esp\n\t"
	        "call _log_fsd\n\t"
	        "addl $4, %esp\n\t"
	        "popal\n\t"
	        "jmp *_orig_fsd_entry");
}

static void maybe_dump(const char *what, ULONG p, ULONG n)
{
	if (sys_ptr(p))
		dump(what, (void *) p, n);
}

static int __cdecl probe_mount(UCHAR *pir)
{
	/* seen on 98 SE (mount of the CD drive D:): ir_aux1 (0x10) points to the
	 * volume's IOS parameters (VRP), ir_aux2 (0x28) is the drive number
	 * (3 = D:), ir_rh (0x1C) and ir_fh (0x20) are IFSMgr structures */
	ULONG vrp = *(ULONG *) (pir + 0x10);
	armed = 1;
	outs("kexfatp: mount #");
	outx(++mounts, 4);
	outs(" flags ");
	outx(pir[4], 2);
	outs(" drive ");
	outx(*(ULONG *) (pir + 0x28), 2);
	outs("\r\n");
	dump("ioreq", pir, 0x74);
	if (sys_ptr(vrp))
	{
		dump("vrp", (void *) vrp, 0x30);
		/* 0x28 is aligned and 0x1C = it + 0xC3 (seen twice): 0x28 is the DCB */
		maybe_dump("dcb(vrp+28)", *(ULONG *) (vrp + 0x28), 0x100);
	}
	maybe_dump("rh", *(ULONG *) (pir + 0x1C), 0x40);
	/* watch what the FSD that takes this volume sends through its VRP */
	if (sys_ptr(vrp))
	{
		ULONG *fe = (ULONG *) (vrp + 0x18);
		if (*fe != (ULONG) fsd_entry_hook)
		{
			orig_fsd_entry = *fe;
			*fe = (ULONG) fsd_entry_hook;
			outs("kexfatp: fsd_entry wrapped\r\n");
		}
	}
	if (saved_len && sys_ptr(vrp) && *(ULONG *) (pir + 0x28) == 3)
	{
		ULONG h = *(ULONG *) (vrp + 0x1C);
		replay(saved_edi, *(ULONG *) (saved + saved_delta + 0x10), 0xFF, "as captured");
		replay(saved_edi, 0, 0xFF, "captured target, sector 0");
		replay(h, 0x800, 3, "to D:, sector 0x800");
		replay(h, 0, 3, "to D:, sector 0");
		{
			ULONG ldcb = *(ULONG *) (vrp + 0x28), pdcb = *(ULONG *) ldcb;
			outs("kexfatp: D: logical dcb ");
			outx(ldcb, 8);
			outs(" handle ");
			outx(h, 8);
			outs(" physical dcb ");
			outx(pdcb, 8);
			outs("\r\n");
			{
				ULONG cl = saved_edi & ~0xFFu;
				outs("kexfatp: C: logical dcb ");
				outx(cl, 8);
				outs(" -> ");
				outx(*(ULONG *) cl, 8);
				outs("\r\n");
				dump("D: ldcb", (void *) ldcb, 0x100);
				dump("D: pdcb", (void *) pdcb, 0x100);
				dump("C: ldcb", (void *) cl, 0x40);
			}
			if (sys_ptr(pdcb) && pdcb != ldcb)
			{
				replay(pdcb, 0x800, 0x81, "EDI = physical dcb, drive 81, 0x800");
				replay(pdcb, 0, 0x81, "EDI = physical dcb, drive 81, 0");
				replay(pdcb, 0x800, 0x80, "EDI = physical dcb, drive 80, 0x800");
			}
		}
	}
	/* not ours: ERROR_UNRECOGNIZED_VOLUME, so IFSMgr asks the next FSD */
	*(short *) (pir + 0x1A) = 1005;
	return 1005;
}

static void init(void)
{
	int id;
	outs("kexfatp: Device_Init, registering\r\n");
	__asm__ __volatile__("pushl %3\n\t"
	                     "pushl %2\n\t"
	                     "pushl %1\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00400001\n\t"
	                     "addl $12, %%esp"
	                     : "=a"(id)
	                     : "r"(probe_mount), "r"(IFSMGRVERSION), "r"(NORMAL_FSD)
	                     : "ecx", "edx", "memory", "cc");
	{
		/* Hook_Device_Service(IOS_SendCommand): the old handler comes back
		 * in ESI; interrupts off so no request runs through the hook first */
		ULONG eax = 0x00100004, esi = (ULONG) send_hook;
		__asm__ __volatile__("pushfl\n\t"
		                     "cli\n\t"
		                     "int $0x20\n\t"
		                     ".long 0x00010090\n\t"
		                     "movl %%esi, _prev_send\n\t"
		                     "popfl"
		                     : "+a"(eax), "+S"(esi)
		                     :
		                     : "ecx", "edx", "memory", "cc");
		outs("kexfatp: IOS_SendCommand hooked, previous ");
		outx(prev_send, 8);
		outs("\r\n");
	}
	outs("kexfatp: provider id ");
	outx((ULONG) id, 8);
	outs("\r\n");
}

__attribute__((used)) BOOL __stdcall kexfatp_dispatch(DWORD msg, DWORD ebx, DWORD ecx, DWORD edx, DWORD esi, DWORD edi)
{
	(void) ebx;
	(void) ecx;
	(void) edx;
	(void) esi;
	(void) edi;
	if (msg == DEVICE_INIT)
		init();
	return TRUE;
}

__attribute__((naked)) void KEXFATP_CTRL(void)
{
	__asm__("pushl %edi\n\t"
	        "pushl %esi\n\t"
	        "pushl %edx\n\t"
	        "pushl %ecx\n\t"
	        "pushl %ebx\n\t"
	        "pushl %eax\n\t"
	        "call _kexfatp_dispatch@24\n\t"
	        "cmpl $1, %eax\n\t"
	        "ret");
}

struct VxD_Desc_Block KEXFATP_DDB = {
	0, DDK_VERSION, UNDEFINED_DEVICE_ID, 1, 0, 0,
	{ 'K', 'E', 'X', 'F', 'A', 'T', 'P', ' ' }, UNDEFINED_INIT_ORDER, (ULONG) KEXFATP_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x50726576 /* 'Prev', as VKrnlEx */, sizeof(struct VxD_Desc_Block),
	0x52737631, 0x52737632, 0x52737633,
};
