/*
 *  KernelExEx - KEXRAM.VXD: a RAM disk made of the RAM Windows 98 does not use
 *  (above KEXMEM's limit, and above 4 GB through PSE-36), for the swap file.
 *
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  - Memory: KEXMEM keeps the BIOS's own memory map (BDA 0040:00F0); ramcore
 *    picks the whole 4 MB chunks of RAM at or above KEXMEM's limit. All of it is
 *    reached through one 4 MB window: a page-directory entry with PS=1 (and
 *    physical address bits 32-35 from PSE-36) rewritten before each copy, with
 *    interrupts off (docs/research/ramdisk.md).
 *  - Disk: registers with IOS as a port driver and is its own type-specific
 *    driver: it creates a physical and a logical DCB itself, puts its request
 *    routine in their calldown chains and associates the logical one with a
 *    drive letter (Z: down to M:), so VFAT mounts the volume. The way through
 *    IOS (be your own TSD, which AEPs to acknowledge, DCB_DEV_WRITEABLE on both
 *    DCBs, the logical scatter-gather list's {sectors, pointer} order) follows
 *    zikolas's cfu1-vxd (MIT licence, https://github.com/zikolas/cfu1-vxd).
 *    Structure offsets and constants: Microsoft's Windows 98 DDK documentation,
 *    used as reference only; the definitions below are our own.
 *  - The volume (FAT32, or FAT16 when small) is written at every start; its
 *    contents do not survive a restart, which is what a swap file needs.
 */
#include "../../../toolchain/vxd/include/vmm.h"
#include "ramcore.h"

/* ---- VMM and IOS services ---------------------------------------------------------- */
#define ___MapPhysToLinear 0x0001006C
#define ___PageReserve     0x0001011D
#define ___IOS_Register    0x00100007
#define ____SHELL_CallAtAppyTime 0x0017000E
__declspec(naked) ULONG __cdecl _MapPhysToLinear(ULONG phys, ULONG nbytes, ULONG flags)
{
	VMMJmp(_MapPhysToLinear)
}
__declspec(naked) ULONG __cdecl _PageReserve(ULONG page, ULONG npages, ULONG flags)
{
	VMMJmp(_PageReserve)
}
__declspec(naked) void __cdecl _IOS_Register(void *drp)
{
	VMMJmp(_IOS_Register)
}
__declspec(naked) ULONG __cdecl _SHELL_CallAtAppyTime(void(__cdecl *cb)(ULONG), ULONG ref, ULONG flags, ULONG timeout)
{
	VMMJmp(__SHELL_CallAtAppyTime)
}
#define CAAFL_RING0 1
#define RC_MIN_CHUNKS 288   /* 1.125 GB: the 1 GB swap file, the FATs and room to spare */
#define PR_SYSTEM 0x80080000
#define PR_FIXED  0x00000008

/* ---- IOS structures: only the fields we use, at their offsets ----------------------
   Microsoft's IOS structures are byte-packed (from MASM includes): the DCB's ULONG
   fields after DCB_device_type (0x40) are unaligned. IOS itself reads
   DCB_max_sg_elements at 0x77 and DCB_apparent_blk_size at 0xDB. */
#define OFS(p, o, t) (*(t *) ((unsigned char *) (p) + (o)))
/* DCB */
#define DCB_physical_dcb        0x00
#define DCB_next_logical_dcb    0x10
#define DCB_dmd_flags           0x1C
#define DCB_device_flags        0x20
#define DCB_Partition_Start     0x28
#define DCB_apparent_blk_shift  0x3C
#define DCB_sig                 0x3E
#define DCB_device_type         0x40
#define DCB_max_xfer_len        0x50
#define DCB_actual_sector_cnt   0x54
#define DCB_actual_blk_size     0x5C
#define DCB_actual_head_cnt     0x60
#define DCB_actual_cyl_cnt      0x64
#define DCB_actual_spt          0x68
#define DCB_max_sg_elements     0x77   /* set by the port driver: 0 makes IOS split I/O into nothing */
#define DCB_unit_on_ctl         0xBA
#define DCB_apparent_sector_cnt 0xD3
#define DCB_apparent_blk_size   0xDB
#define DCB_apparent_head_cnt   0xDF
#define DCB_apparent_cyl_cnt    0xE3
#define DCB_apparent_spt        0xE7
#define DCB_SIZE                0x12B
#define DCB_DEV_PHYSICAL  0x8000
#define DCB_DEV_LOGICAL   0x4000
#define DCB_DEV_WRITEABLE 0x0002
#define DMD_NONE          0x00   /* we copy with the CPU: any linear buffer will do */
/* IOP / IOR */
#define IOP_callback_ptr 0x14
#define IOP_ior          0x64
#define IOR_func         (IOP_ior + 0x04)
#define IOR_status       (IOP_ior + 0x06)
#define IOR_flags        (IOP_ior + 0x08)
#define IOR_start_addr   (IOP_ior + 0x10)
#define IOR_xfer_count   (IOP_ior + 0x18)
#define IOR_buffer_ptr   (IOP_ior + 0x1C)
#define IORF_SCATTER_GATHER 0x0002
#define IORF_CHAR_COMMAND   0x0200
#define IORS_SUCCESS         0x00
#define IORS_DEVICE_ERROR    0x15
#define IORS_INVALID_COMMAND 0x16
#define IORS_INVALID_SECTOR  0x10
/* AEP */
#define AEP_func 0
#define AEP_result 2
#define AEP_lgn 8
#define AEP_INITIALIZE 0
#define AEP_BOOT_COMPLETE 2
#define AEP_SUCCESS 0
#define AEP_FAILURE 0xFFFF
#define AEP_NO_MORE_DEVICES 2
/* ISP */
#define ISP_CREATE_DDB 0
#define ISP_CREATE_DCB 1
#define ISP_INSERT_CALLDOWN 5
#define ISP_ASSOCIATE_DCB 6

struct drp
{
	char eyecatch[8];
	ULONG lgn;
	void *aer;
	void *ilb;
	char name[16];
	unsigned char revision;
	ULONG feature_code;
	unsigned short if_requirements;
	unsigned char bus_type;
	unsigned short reg_result;
	ULONG reference_data, reserved1, reserved2;
} __attribute__((packed));
struct ilb
{
	void(__cdecl *service)(void *isp);
	ULONG rest[12];
};

/* ---- log: QEMU's debug console, in --debugcon builds only ----------------------------- */
#ifdef KEX_DEBUGCON
static void dc(char c) { __asm__ __volatile__("outb %0, $0xe9" : : "a"(c)); }
#else
static void dc(char c) { (void) c; }
#endif
static void ds(const char *s) { while (*s) dc(*s++); }
static void dx(ULONG v) { int i; for (i = 28; i >= 0; i -= 4) dc("0123456789abcdef"[(v >> i) & 15]); }
static void kv(const char *k, ULONG v) { ds(k); dc('='); dx(v); dc(' '); }

/* ---- the RAM: chunks and the window --------------------------------------------------- */
static rc_u32 *chunks, nchunks;
static ULONG total_secs;
static volatile ULONG *pde;     /* the window's page-directory entry */
static ULONG pde_idle;          /* its value outside our copies (as VMM left it) */
static unsigned char *win;      /* its linear address (4 MB aligned) */

static ULONG cr3(void) { ULONG v; __asm__ __volatile__("movl %%cr3, %0" : "=r"(v)); return v; }
static ULONG cr4(void) { ULONG v; __asm__ __volatile__("movl %%cr4, %0" : "=r"(v)); return v; }
static void set_cr4(ULONG v) { __asm__ __volatile__("movl %0, %%cr4" : : "r"(v)); }
static ULONG cpuid_edx(void) { ULONG a = 1, b, c, d; __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d)); return d; }
static ULONG save_cli(void) { ULONG f; __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) : : "memory"); return f; }
static void restore(ULONG f) { __asm__ __volatile__("pushl %0; popfl" : : "r"(f) : "memory", "cc"); }

static void copy(void *d, const void *s, ULONG n)
{
	__asm__ __volatile__("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

/* point the window at chunk c, or (c = ~0) put back VMM's own entry. The 4 MB
   entry exists only while we copy, with interrupts off: VMM never sees it (it would
   take the chunk's first page for a page table when it walks its tables) */
static void map_chunk(ULONG c)
{
	if (c == 0xFFFFFFFF)
		*pde = pde_idle;
	else
	{
		ULONG id = chunks[c];   /* physical address / 4 MB */
		*pde = (id << RC_CHUNK_SHIFT) | (id >> 10 & 0xF) << 13 | 0x83;   /* P | RW | PS */
	}
	__asm__ __volatile__("invlpg (%0)" : : "r"(win) : "memory");
}

/* copy n sectors from (write: to) the disk at sector s; buf is linear */
static void xfer(ULONG s, ULONG n, unsigned char *buf, int write)
{
	while (n)
	{
		ULONG c = s / RC_SECTORS_PER_CHUNK, off = s % RC_SECTORS_PER_CHUNK, k = RC_SECTORS_PER_CHUNK - off, f;
		if (k > n)
			k = n;
		if (k > 128)   /* at most 64 KB with interrupts off */
			k = 128;
		f = save_cli();
		map_chunk(c);
		if (write)
			copy(win + off * 512, buf, k * 128);
		else
			copy(buf, win + off * 512, k * 128);
		map_chunk(0xFFFFFFFF);
		restore(f);
		s += k;
		n -= k;
		buf += k * 512;
	}
}

/* ---- IOS ---------------------------------------------------------------------------- */
static void __cdecl aer(unsigned char *aep);
static void __cdecl appy(ULONG ref);
static void __cdecl req(unsigned char *iop);
static struct ilb ilb;
static struct drp drp = { { 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X' }, 1u << 0x13 /* DRP_MISC_PD */, (void *) aer, &ilb,
                          { 'K', 'E', 'X', 'R', 'A', 'M', ' ', 'R', 'A', 'M', ' ', 'D', 'I', 'S', 'K', ' ' } };
static void *ddb;
static unsigned char *pdcb, *ldcb, lgn;
static int volume_up, ldcb_assoc;

static unsigned short isp(void *p)
{
	ilb.service(p);
	return ((unsigned short *) p)[1];
}

static unsigned char *new_dcb(void)
{
	struct { unsigned short func, result, size; void *dcb; unsigned char flags, pad; } __attribute__((packed)) p =
		{ ISP_CREATE_DCB, 0, DCB_SIZE, 0, 0, 0 };
	isp(&p);
	return p.result ? 0 : (unsigned char *) p.dcb;
}

static int calldown(unsigned char *dcb)
{
	struct { unsigned short func, result; void *dcb, *req, *ddb; unsigned short expan; ULONG flags; unsigned char lgn, pad; }
		__attribute__((packed)) p = { ISP_INSERT_CALLDOWN, 0, dcb, (void *) req, ddb, 0, DMD_NONE, lgn, 0 };
	return isp(&p) == 0;
}

static void geometry(unsigned char *d, ULONG secs)
{
	ULONG cyl = secs / (255 * 63);
	OFS(d, DCB_dmd_flags, ULONG) = DMD_NONE;
	OFS(d, DCB_apparent_blk_shift, unsigned char) = 9;
	OFS(d, DCB_sig, unsigned short) = 0x4342;
	OFS(d, DCB_device_type, unsigned char) = 0;   /* disk */
	OFS(d, DCB_unit_on_ctl, unsigned char) = 0;
	OFS(d, DCB_max_xfer_len, ULONG) = 0x10000;
	OFS(d, DCB_max_sg_elements, unsigned char) = 17;   /* becomes VRP_max_sgd: 0 = zero-length I/O */
	OFS(d, DCB_actual_sector_cnt, ULONG) = secs;
	OFS(d, DCB_actual_sector_cnt + 4, ULONG) = 0;
	OFS(d, DCB_actual_blk_size, ULONG) = 512;
	OFS(d, DCB_actual_head_cnt, ULONG) = 255;
	OFS(d, DCB_actual_spt, ULONG) = 63;
	OFS(d, DCB_actual_cyl_cnt, ULONG) = cyl;
	OFS(d, DCB_apparent_sector_cnt, ULONG) = secs;
	OFS(d, DCB_apparent_sector_cnt + 4, ULONG) = 0;
	OFS(d, DCB_apparent_blk_size, ULONG) = 512;
	OFS(d, DCB_apparent_head_cnt, ULONG) = 255;
	OFS(d, DCB_apparent_spt, ULONG) = 63;
	OFS(d, DCB_apparent_cyl_cnt, ULONG) = cyl;
}

/* create both DCBs and give the volume a drive letter: Z:, else the next one down */
static void volume(void)
{
	int drive;
	if (volume_up || !ddb || !total_secs)
		return;
	volume_up = 1;
	if (!pdcb)
		return ds("KEXRAM: IOS gave no physical DCB\n");
	if (!ldcb)
	{
		if (!(ldcb = new_dcb()))
			return ds("KEXRAM: no logical DCB\n");
		OFS(ldcb, DCB_physical_dcb, void *) = pdcb;
		OFS(pdcb, DCB_next_logical_dcb, void *) = ldcb;
		if (!calldown(ldcb))
			return ds("KEXRAM: no calldown (logical)\n");
	}
	geometry(ldcb, total_secs);
	OFS(ldcb, DCB_device_flags, ULONG) = DCB_DEV_LOGICAL | DCB_DEV_WRITEABLE;
	OFS(ldcb, DCB_Partition_Start, ULONG) = 0;   /* the volume starts at sector 0 */
	for (drive = 25; drive >= 12; drive--)
	{
		struct { unsigned short func, result; void *dcb; unsigned char drive, flags, pad[2]; } __attribute__((packed)) p =
			{ ISP_ASSOCIATE_DCB, 0, ldcb, (unsigned char) drive, 0, { 0, 0 } };
		if (isp(&p) == 0)
		{
			ldcb_assoc = 1;
			ds("KEXRAM: drive ");
			dc((char) ('A' + drive));
			ds(":\n");
			return;
		}
	}
	ds("KEXRAM: no free drive letter\n");
}

/* the volume services (ISP_ASSOCIATE_DCB: IFSMGR mounts the volume) work at appy
   time, once the system is up (cfu1-vxd's finding) */
static void __cdecl appy(ULONG ref)
{
	(void) ref;
	volume();
}

static void __cdecl aer(unsigned char *aep)
{
	unsigned short f = OFS(aep, AEP_func, unsigned short), r = AEP_FAILURE;
	if (f == AEP_INITIALIZE)
	{
		struct { unsigned short func, result, size; void *ddb; unsigned char flags, pad; } __attribute__((packed)) p =
			{ ISP_CREATE_DDB, 0, 32, 0, 0, 0 };
		lgn = OFS(aep, AEP_lgn, unsigned char);
		isp(&p);
		ddb = p.ddb;
		r = ddb ? AEP_SUCCESS : AEP_FAILURE;
	}
	else if (f == 6)                      /* AEP_DEVICE_INQUIRY: we have one disk, unit 0 */
	{
		unsigned char *d = OFS(aep, 12, unsigned char *);
		static int claimed;
		if (!claimed && total_secs && d && !OFS(d, DCB_unit_on_ctl, unsigned char))
		{
			claimed = 1;
			r = AEP_SUCCESS;
		}
		else
			r = AEP_NO_MORE_DEVICES;
	}
	else if (f == 3)                      /* AEP_CONFIG_DCB: IOS's physical DCB for our disk */
	{
		unsigned char *d = OFS(aep, 12, unsigned char *);
		r = AEP_SUCCESS;
		if (d && !pdcb && (OFS(d, DCB_device_flags, ULONG) & DCB_DEV_PHYSICAL))
		{
			pdcb = d;
			lgn = OFS(aep, AEP_lgn, unsigned char);
			geometry(pdcb, total_secs);
			OFS(pdcb, DCB_device_flags, ULONG) |= DCB_DEV_WRITEABLE;
			OFS(pdcb, DCB_device_flags, ULONG) &= ~0x400u;   /* DCB_DEV_MUST_CONFIGURE: we are its TSD */
			if (!calldown(pdcb))
				ds("KEXRAM: no calldown (physical)\n");
			kv("KEXRAM: physical DCB", (ULONG) d);
			kv("flags", OFS(d, DCB_device_flags, ULONG));
			ds("\n");
		}
	}
	else if (f == AEP_BOOT_COMPLETE)
		r = AEP_SUCCESS;
	else if (f >= 16 && f <= 19)          /* lock, mount notify, VRP create/destroy */
		r = AEP_SUCCESS;
	OFS(aep, AEP_result, unsigned short) = r;
}

static void __cdecl req(unsigned char *iop)
{
	unsigned short f = OFS(iop, IOR_func, unsigned short), st = IORS_SUCCESS;
	ULONG flags = OFS(iop, IOR_flags, ULONG), s = OFS(iop, IOR_start_addr, ULONG), n = OFS(iop, IOR_xfer_count, ULONG);
	ULONG cb;
#ifdef KEX_DEBUGCON
	{
		/* each new (function, flags) pair once, with its first buffer address */
		static ULONG seen[32];
		ULONG key = (ULONG) f << 24 ^ flags, k;
		for (k = 0; k < 32 && seen[k] && seen[k] != key; k++)
			;
		if (k < 32 && !seen[k])
		{
			seen[k] = key;
			ds("REQ ");
			dx(f); dc(' '); dx(flags); dc(' '); dx(s); dc(' '); dx(n); dc(' ');
			dx(OFS(iop, IOR_buffer_ptr, ULONG)); dc('\n');
		}
	}
#endif
	if (f == 0 || f == 1)                 /* IOR_READ, IOR_WRITE */
	{
		if (flags & IORF_CHAR_COMMAND)
			n /= 512;
		if (OFS(iop, IOR_start_addr + 4, ULONG) || s >= total_secs)
			st = IORS_INVALID_SECTOR;
		else if (flags & IORF_SCATTER_GATHER)
		{
			/* the logical list: {sectors, linear pointer} pairs, 0-terminated */
			ULONG *sg = (ULONG *) OFS(iop, IOR_buffer_ptr, ULONG);
			for (; n && sg[0] && sg[1]; sg += 2)
			{
				ULONG k = sg[0] < n ? sg[0] : n;
				if (s + k > total_secs)
				{
					st = IORS_INVALID_SECTOR;
					break;
				}
				xfer(s, k, (unsigned char *) sg[1], f == 1);
				s += k;
				n -= k;
			}
		}
		else if (s + n > total_secs)
			st = IORS_INVALID_SECTOR;
		else
			xfer(s, n, (unsigned char *) OFS(iop, IOR_buffer_ptr, ULONG), f == 1);
	}
	else if (f == 2 || f == 5 || f == 6 || f == 7 || f == 9 || f == 10 || f == 0x10 || f == 0x11 || f == 0x22 ||
	         f == 0x27 || f == 0x28)
		st = IORS_SUCCESS;                /* verify, media check, lock, flush...: nothing to do */
	else
		st = IORS_INVALID_COMMAND;
	OFS(iop, IOR_status, unsigned short) = st;
	cb = OFS(iop, IOP_callback_ptr, ULONG) - 8;
	OFS(iop, IOP_callback_ptr, ULONG) = cb;
	((void(__cdecl *)(void *)) * (ULONG *) cb)(iop);
}

/* ---- start-up ------------------------------------------------------------------------- */
static int init(void)
{
	const unsigned short *ica = (const unsigned short *) 0x4F0;
	const unsigned char *t;
	static struct rc_e820 map[32];
	struct rc_fat fat;
	ULONG d = cpuid_edx(), lin, pdl, i, n;
	unsigned char sec[512];
	ds("\nKEXRAM: ");
	if (ica[0] != 0x584B)
		return ds("no KEXMEM memory map\n"), 0;
	t = (const unsigned char *) ((ULONG) ica[2] * 16 + ica[1]);
	if (t[0] != 'K' || t[1] != 'X' || t[2] != 'M' || t[3] != 'P')
		return ds("bad KEXMEM table\n"), 0;
	n = *(const unsigned short *) (t + 8);
	for (i = 0; i < n && i < 32; i++)
	{
		const ULONG *e = (const ULONG *) (t + 10 + 20 * i);
		map[i].base = (rc_u64) e[1] << 32 | e[0];
		map[i].len = (rc_u64) e[3] << 32 | e[2];
		map[i].type = e[4];
	}
	if (!(d & 8))
		return ds("no PSE\n"), 0;
	if (!(chunks = (rc_u32 *) _HeapAllocate(RC_MAX_CHUNKS * 4, 0)))
		return ds("no memory\n"), 0;
	nchunks = rc_chunks(map, (int) i, *(const ULONG *) (t + 4), (d & 0x20000) != 0, chunks, RC_MAX_CHUNKS);
	kv("limit", *(const ULONG *) (t + 4));
	kv("chunks", nchunks);
	/* Windows sizes its swap file by its own RAM and, if the drive cannot hold that,
	   waits for ever at start-up; setup caps the swap file at 1 GB
	   (MaxPagingFileSize), so the RAM disk must hold that much and some: below it
	   there is no drive Z: and Windows keeps its swap file where it was */
	if (nchunks < RC_MIN_CHUNKS)
		return ds("not enough unused RAM\n"), 0;
	if (nchunks > 0x1FFFFFFF / RC_SECTORS_PER_CHUNK)
		nchunks = 0x1FFFFFFF / RC_SECTORS_PER_CHUNK;
	total_secs = nchunks * RC_SECTORS_PER_CHUNK;
	/* 8 MB of system linear space, never committed: the 4 MB-aligned half is the window */
	lin = _PageReserve(PR_SYSTEM, 2048, PR_FIXED);
	pdl = _MapPhysToLinear(cr3() & ~0xFFFu, 0x1000, 0);
	if (lin == 0xFFFFFFFF || !lin || pdl == 0xFFFFFFFF)
		return ds("no window\n"), (total_secs = 0);
	win = (unsigned char *) ((lin + RC_CHUNK - 1) & ~(RC_CHUNK - 1));
	pde = (volatile ULONG *) pdl + ((ULONG) win >> 22);
	pde_idle = *pde;
	set_cr4(cr4() | 0x10);   /* PSE */
	kv("win", (ULONG) win);
	/* the volume */
	if (rc_fat_layout(total_secs, &fat))
		return (total_secs = 0);
	for (i = 0; i < fat.zero_end; i++)
	{
		rc_fat_sector(&fat, 0x4B580000 ^ nchunks, i, sec);
		xfer(i, 1, sec, 1);
	}
	kv("MB", total_secs / 2048);
	ds(fat.fat32 ? "FAT32\n" : "FAT16\n");
	_IOS_Register(&drp);
	kv("KEXRAM: IOS", drp.reg_result);
	ds("\n");
	/* the drive letter: now, so the swap file can be on it (Windows opens the swap
	   file before "appy time"); appy time is the fallback if that is refused */
	volume();
	if (pdcb && !ldcb_assoc)
	{
		volume_up = 0;
		_SHELL_CallAtAppyTime(appy, 0, CAAFL_RING0, 0);
	}
	return 1;
}

__attribute__((used)) BOOL __stdcall kexram_dispatch(DWORD msg)
{
	if (msg == 1)        /* Device_Init: before the swap file is opened */
		init();
	return TRUE;
}

__attribute__((naked)) void KEXRAM_CTRL(void)
{
	__asm__("pushl %eax\n\t"
	        "call _kexram_dispatch@4\n\t"
	        "cmpl $1, %eax\n\t"
	        "ret");
}

struct VxD_Desc_Block KEXRAM_DDB = {
	0, DDK_VERSION, UNDEFINED_DEVICE_ID, 1, 0, 0,
	{ 'K', 'E', 'X', 'R', 'A', 'M', ' ', ' ' }, UNDEFINED_INIT_ORDER, (ULONG) KEXRAM_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x50726576, sizeof(struct VxD_Desc_Block),
	0x52737631, 0x52737632, 0x52737633,
};
