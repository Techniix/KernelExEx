/*
 *  KernelExEx - KEXFAT.VXD: exFAT for Windows 98 SE / ME, a file system
 *  driver (FSD) for IFSMgr around the portable core (fs/exfat/exfat.c).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 *
 *  Stage 1 (in development, test builds only): mounts exFAT volumes,
 *  reading and writing sectors through IOS the way VFAT does, and logs
 *  every other request to the debug port so each one can be implemented
 *  against what IFSMgr really sends. Layouts: fs/exfat/DESIGN.md.
 */
#include <windows.h>
#include <vmm.h>
#include "../exfat.h"

/* ---- debug port ------------------------------------------------------------------------ */
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

/* ---- the ioreq (fields as seen on 98 SE; DESIGN.md) ------------------------------------ */
#define IR_LENGTH(p) (*(ULONG *) ((UCHAR *) (p) + 0x00))
#define IR_FLAGS(p) (*(UCHAR *) ((UCHAR *) (p) + 0x04))
#define IR_PPATH(p) (*(ULONG *) ((UCHAR *) (p) + 0x0C))
#define IR_AUX1(p) (*(ULONG *) ((UCHAR *) (p) + 0x10))
#define IR_DATA(p) (*(ULONG *) ((UCHAR *) (p) + 0x14))
#define IR_OPTIONS(p) (*(USHORT *) ((UCHAR *) (p) + 0x18))
#define IR_ERROR(p) (*(short *) ((UCHAR *) (p) + 0x1A))
#define IR_RH(p) (*(ULONG *) ((UCHAR *) (p) + 0x1C))
#define IR_FH(p) (*(ULONG *) ((UCHAR *) (p) + 0x20))
#define IR_POS(p) (*(ULONG *) ((UCHAR *) (p) + 0x24))
#define IR_AUX2(p) (*(ULONG *) ((UCHAR *) (p) + 0x28))
#define IR_AUX3(p) (*(ULONG *) ((UCHAR *) (p) + 0x2C))

#define ERROR_ACCESS_DENIED_ 5
#define ERROR_NOT_SUPPORTED_ 50
#define ERROR_UNRECOGNIZED_VOLUME_ 1005

/* ---- sectors through IOS --------------------------------------------------------------- */
#define IOR_READ 0
#define IOR_WRITE 1
#define IORF_VERSION_002 0x00000400
#define IORF_LOGICAL_START_SECTOR 0x00000008

struct kvol
{
	ULONG vrp;
	ULONG dev_handle;   /* the VRP's 0x1C value: what IOS_SendCommand takes in EDI */
	ULONG phys_handle;  /* the disk's: physical DCB + 0x83 (as IOS's own disk requests) */
	ULONG part_start;   /* the partition's first sector (VRP 0x10) */
	int physical;       /* read through the disk, not the volume */
	UCHAR drive;
	USHORT max_req;
	ULONG delta;
	UCHAR *iop;          /* one request packet, reused (requests are serialised) */
	exfat_vol *ex;
	volatile int done;
};

static struct kvol *cur_io;   /* the volume whose request is in flight */

__attribute__((used)) void __cdecl io_done_c(ULONG ior)
{
	(void) ior;
	if (cur_io)
		cur_io->done = 1;
}

/* IOS calls the callback with ESI = the IOR */
__attribute__((naked)) void io_done(void)
{
	__asm__("pushal\n\t"
	        "pushl %esi\n\t"
	        "call _io_done_c\n\t"
	        "addl $4, %esp\n\t"
	        "popal\n\t"
	        "ret");
}

static void ifs_yield(void)
{
	__asm__ __volatile__("int $0x20\n\t"
	                     ".long 0x00400014"   /* IFSMgr_Yield */
	                     :
	                     :
	                     : "eax", "ecx", "edx", "memory", "cc");
}

#define IOR_MEDIA_CHECK_RESET 0x0D
#define IORS_UNCERTAIN_MEDIA 0x1C   /* removable media not checked since it changed */

static int ios_rw1(struct kvol *v, ULONG func, ULONG sector, ULONG count, void *buf, USHORT *st);

static int ios_rw(struct kvol *v, ULONG func, ULONG sector, ULONG count, void *buf)
{
	USHORT st;
	int r = ios_rw1(v, func, sector, count, buf, &st);
	if (r && st == IORS_UNCERTAIN_MEDIA)
	{
		ios_rw1(v, IOR_MEDIA_CHECK_RESET, 0, 0, NULL, &st);
		r = ios_rw1(v, func, sector, count, buf, &st);
	}
	return r;
}

static int ios_rw1(struct kvol *v, ULONG func, ULONG sector, ULONG count, void *buf, USHORT *st)
{
	UCHAR *ior = v->iop + v->delta;
	ULONG esi = (ULONG) ior, edi = v->physical ? v->phys_handle : v->dev_handle;
	USHORT status;
	memset(v->iop, 0, v->max_req);
	*(USHORT *) (ior + 0x04) = (USHORT) func;
	/* as VFAT sends them (logged on 98 SE): reads/writes 0x409, the media
	 * check 0x80519 */
	*(ULONG *) (ior + 0x08) = func == IOR_MEDIA_CHECK_RESET ? 0x00080519
	                                                        : v->physical ? IORF_VERSION_002 | 0x1
	                                                                      : IORF_VERSION_002 | IORF_LOGICAL_START_SECTOR | 0x1;
	/* the media check is synchronous (VFAT sends it without a callback) */
	*(ULONG *) (ior + 0x0C) = func == IOR_MEDIA_CHECK_RESET ? 0 : (ULONG) io_done;
	*(ULONG *) (ior + 0x10) = v->physical ? v->part_start + sector : sector;
	*(ULONG *) (ior + 0x18) = count;
	*(ULONG *) (ior + 0x1C) = (ULONG) buf;
	*(ULONG *) (ior + 0x20) = v->delta;
	*(ULONG *) (ior + 0x44) = 0;   /* VFAT puts a per-request pointer here, not the VRP */
	*(ULONG *) (ior + 0x48) = (ULONG) (ior + 0x58);   /* room for IOS's SGDs, inside the packet as VFAT's */
	*(UCHAR *) (ior + 0x4D) = v->drive;
	v->done = 0;
	cur_io = v;
	__asm__ __volatile__("pushal\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00100004\n\t"   /* IOS_SendCommand */
	                     "popal"
	                     :
	                     : "S"(esi), "D"(edi)
	                     : "memory", "cc");
	if (func != IOR_MEDIA_CHECK_RESET)
		while (!v->done)
			ifs_yield();
	cur_io = NULL;
	status = *(USHORT *) (ior + 0x06);
	*st = status;
	if (status >= 0x10)
	{
		outs("kexfat: I/O error, status ");
		outx(status, 4);
		outs("\r\n");
		dump("  ior", ior, 0x58);
	}
	return status < 0x10 ? 0 : -1;   /* IORS_SUCCESS and the "succeeded with ..." codes are below 0x10 */
}

/* the core's device: whole sectors, buffers from the locked heap */
static int dev_read(void *ctx, uint64_t sector, uint32_t count, void *buf)
{
	return ios_rw((struct kvol *) ctx, IOR_READ, (ULONG) sector, count, buf);
}

static int dev_write(void *ctx, uint64_t sector, uint32_t count, const void *buf)
{
	return ios_rw((struct kvol *) ctx, IOR_WRITE, (ULONG) sector, count, (void *) buf);
}

static void *dev_alloc(void *ctx, size_t n)
{
	(void) ctx;
	return _HeapAllocate(n, HEAPZEROINIT);
}

static void dev_free(void *ctx, void *p)
{
	(void) ctx;
	_HeapFree(p, 0);
}

/* ---- volume functions: stage 1 logs them ----------------------------------------------- */
#define NUM_VOLFUNC 15
static const char *const vfn_names[NUM_VOLFUNC] = { "delete", "dir", "fileattrib", "flush", "getdiskinfo",
	                                                "open", "rename", "search", "query", "disconnect",
	                                                "uncpipereq", "ioctl16drive", "getdiskparms", "findopen",
	                                                "dasdio" };

static int log_vfn(int i, UCHAR *pir)
{
	outs("kexfat: ");
	outs(vfn_names[i]);
	outs(" flags ");
	outx(IR_FLAGS(pir), 2);
	outs("\r\n");
	dump("  ioreq", pir, 0x34);
	if (IR_PPATH(pir) >= 0xC0000000u)
	{
		USHORT len = *(USHORT *) IR_PPATH(pir);
		dump("  ppath", (void *) IR_PPATH(pir), len < 0x80 ? len + 4 : 0x80);
	}
	IR_ERROR(pir) = ERROR_NOT_SUPPORTED_;
	return ERROR_NOT_SUPPORTED_;
}

#define VFN(i)                                                                                                       \
	static int __cdecl vfn_##i(UCHAR *pir)                                                                           \
	{                                                                                                                \
		return log_vfn(i, pir);                                                                                      \
	}
VFN(0) VFN(1) VFN(2) VFN(3) VFN(4) VFN(5) VFN(6) VFN(7) VFN(8) VFN(9) VFN(10) VFN(11) VFN(12) VFN(13) VFN(14)

struct volfunc
{
	ULONG version;
	UCHAR revision;
	UCHAR size;
	USHORT reserved;
	void *func[NUM_VOLFUNC];
};

static struct volfunc vfuncs = {
	0x0000010A /* IFS_VERSION */, 0x10 /* IFS_REVISION */, NUM_VOLFUNC, 0,
	{ vfn_0, vfn_1, vfn_2, vfn_3, vfn_4, vfn_5, vfn_6, vfn_7, vfn_8, vfn_9, vfn_10, vfn_11, vfn_12, vfn_13, vfn_14 }
};

/* ---- mount ------------------------------------------------------------------------------ */
#define IR_FSD_MOUNT 0

static int __cdecl fsd_mount(UCHAR *pir)
{
	ULONG vrp = IR_AUX1(pir);
	struct kvol *v;
	struct exfat_dev dev;
	UCHAR *sec;
	int r;
	outs("kexfat: mount request, flags ");
	outx(IR_FLAGS(pir), 2);
	outs(" drive ");
	outx(IR_AUX2(pir), 2);
	outs("\r\n");
	if (IR_FLAGS(pir) != IR_FSD_MOUNT || vrp < 0xC0000000u)
	{
		IR_ERROR(pir) = ERROR_UNRECOGNIZED_VOLUME_;
		return ERROR_UNRECOGNIZED_VOLUME_;
	}
	v = (struct kvol *) _HeapAllocate(sizeof(*v), HEAPZEROINIT);
	if (!v)
		goto no;
	v->vrp = vrp;
	v->dev_handle = *(ULONG *) (vrp + 0x1C);
	v->max_req = *(USHORT *) (vrp + 0x0A);
	v->delta = *(ULONG *) (vrp + 0x0C);
	v->drive = (UCHAR) IR_AUX2(pir);
	v->iop = (UCHAR *) _HeapAllocate(v->max_req, HEAPZEROINIT);
	sec = (UCHAR *) _HeapAllocate(4096, HEAPZEROINIT);
	if (!v->iop || !sec)
		goto free_no;
	v->part_start = *(ULONG *) (vrp + 0x10);
	{
		ULONG ldcb = *(ULONG *) (vrp + 0x28);
		v->phys_handle = (ldcb >= 0xC0000000u && *(ULONG *) ldcb >= 0xC0000000u) ? *(ULONG *) ldcb + 0x83 : 0;
	}
	r = ios_rw(v, IOR_READ, 0, 1, sec);
	if (r && v->phys_handle)
	{
		outs("kexfat: volume refused the read; through the disk, partition at ");
		outx(v->part_start, 8);
		outs("\r\n");
		v->physical = 1;
		r = ios_rw(v, IOR_READ, 0, 1, sec);
	}
	outs("kexfat: boot sector read ");
	outx((ULONG) r, 8);
	outs(": ");
	{
		int i;
		for (i = 3; i < 11; i++)
			outc(sec[i] >= 32 && sec[i] < 127 ? (char) sec[i] : '.');
	}
	outs("\r\n");
	if (r || memcmp(sec + 3, "EXFAT   ", 8))
	{
		_HeapFree(sec, 0);
		goto free_no;
	}
	_HeapFree(sec, 0);
	dev.ctx = v;
	dev.sector_size = 0;
	dev.read = dev_read;
	dev.write = dev_write;
	dev.alloc = dev_alloc;
	dev.free = dev_free;
	r = exfat_mount(&v->ex, &dev, 1);   /* read-only in stage 1 */
	outs("kexfat: exfat_mount ");
	outx((ULONG) r, 8);
	outs("\r\n");
	if (r)
		goto free_no;
	{
		struct exfat_info info;
		exfat_info(v->ex, &info);
		outs("kexfat: mounted, ");
		outx(info.clusters, 8);
		outs(" clusters of ");
		outx(info.cluster_size, 8);
		outs(" bytes, ");
		outx(info.free_clusters, 8);
		outs(" free\r\n");
	}
	IR_RH(pir) = (ULONG) v;
	IR_DATA(pir) = (ULONG) &vfuncs;   /* ir_vfunc */
	IR_ERROR(pir) = 0;
	return 0;
free_no:
	if (v->iop)
		_HeapFree(v->iop, 0);
	_HeapFree(v, 0);
no:
	IR_ERROR(pir) = ERROR_UNRECOGNIZED_VOLUME_;
	return ERROR_UNRECOGNIZED_VOLUME_;
}

static void init(void)
{
	int id;
	__asm__ __volatile__("pushl %3\n\t"
	                     "pushl %2\n\t"
	                     "pushl %1\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00400001\n\t"   /* IFSMgr_RegisterMount */
	                     "addl $12, %%esp"
	                     : "=a"(id)
	                     : "r"(fsd_mount), "r"(0x22), "r"(0)
	                     : "ecx", "edx", "memory", "cc");
	outs("kexfat: registered, provider ");
	outx((ULONG) id, 8);
	outs("\r\n");
}

__attribute__((used)) BOOL __stdcall kexfat_dispatch(DWORD msg, DWORD ebx, DWORD ecx, DWORD edx, DWORD esi, DWORD edi)
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

__attribute__((naked)) void KEXFAT_CTRL(void)
{
	__asm__("pushl %edi\n\t"
	        "pushl %esi\n\t"
	        "pushl %edx\n\t"
	        "pushl %ecx\n\t"
	        "pushl %ebx\n\t"
	        "pushl %eax\n\t"
	        "call _kexfat_dispatch@24\n\t"
	        "cmpl $1, %eax\n\t"
	        "ret");
}

struct VxD_Desc_Block KEXFAT_DDB = {
	0, DDK_VERSION, UNDEFINED_DEVICE_ID, 1, 0, 0,
	{ 'K', 'E', 'X', 'F', 'A', 'T', ' ', ' ' }, UNDEFINED_INIT_ORDER, (ULONG) KEXFAT_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x50726576, sizeof(struct VxD_Desc_Block),
	0x52737631, 0x52737632, 0x52737633,
};
