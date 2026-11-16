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
