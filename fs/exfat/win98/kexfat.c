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
/* IOR flags (published DDK values) */
#define IORF_HIGH_PRIORITY 0x00000001
#define IORF_BYPASS_VOLTRK 0x00000008
#define IORF_SYNC_CMD_DONE 0x00000080
#define IORF_SYNC_COMMAND 0x00000100
#define IORF_VERSION_002 0x00000400
#define IORF_LOGICAL_START_SECTOR 0x00080000   /* start_addr counts from the volume, not the disk */

struct kvol
{
	ULONG vrp;
	ULONG dev_handle;   /* the VRP's 0x1C value: what IOS_SendCommand takes in EDI */
	ULONG phys_handle;  /* the disk's physical DCB */
	ULONG part_start;   /* the partition's first sector (VRP 0x10) */
	int physical;       /* read through the disk, not the volume */
	UCHAR unit;         /* the disk's BIOS-style number (0x80 + n): IOS routes by IOR_vol_designtr */
	UCHAR drive;
	USHORT max_req;
	ULONG delta;
	UCHAR *iop;          /* one request packet, reused (requests are serialised): one fixed page */
	ULONG iop_phys;
	exfat_vol *ex;
	volatile int done;
	volatile int busy;   /* a volume function is using the core and the packet */
};

static struct kvol *cur_io;   /* the volume whose request is in flight */

static void ifs_wakeup(ULONG key)
{
	__asm__ __volatile__("pushl %0\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00400013\n\t"   /* IFSMgr_Wakeup */
	                     "addl $4, %%esp"
	                     :
	                     : "r"(key)
	                     : "eax", "ecx", "edx", "memory", "cc");
}

static void ifs_block(ULONG key)
{
	__asm__ __volatile__("pushl %0\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00400012\n\t"   /* IFSMgr_Block */
	                     "addl $4, %%esp"
	                     :
	                     : "r"(key)
	                     : "eax", "ecx", "edx", "memory", "cc");
}

__attribute__((used)) void __cdecl io_done_c(ULONG ior)
{
	(void) ior;
	if (cur_io)
	{
		cur_io->done = 1;
		ifs_wakeup((ULONG) cur_io);
	}
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

#define IOR_MEDIA_CHECK_RESET 0x06
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
	memset(v->iop, 0, 4096);
	*(ULONG *) (v->iop + 0x00) = v->iop_phys;   /* IOP_physical */
	*(USHORT *) (ior + 0x04) = (USHORT) func;
	/* reads/writes: volume-relative sectors; the media check as VFAT sends it */
	*(ULONG *) (ior + 0x08) = func == IOR_MEDIA_CHECK_RESET
	                              ? 0x00080519
	                              : IORF_VERSION_002 | IORF_BYPASS_VOLTRK | IORF_HIGH_PRIORITY;
	/* the media check is synchronous (VFAT sends it without a callback) */
	*(ULONG *) (ior + 0x0C) = func == IOR_MEDIA_CHECK_RESET ? 0 : (ULONG) io_done;
	*(ULONG *) (ior + 0x10) = v->part_start + sector;   /* disk sectors, as VFAT's (0x800 for a partition at 2048) */
	*(ULONG *) (ior + 0x18) = count;
	*(ULONG *) (ior + 0x1C) = (ULONG) buf;
	*(ULONG *) (ior + 0x20) = v->delta;
	*(ULONG *) (ior + 0x24) = (ULONG) v->iop;   /* IOR_private_IOS: the packet, as in IOS's own requests */
	*(ULONG *) (ior + 0x44) = 0;   /* VFAT puts a per-request pointer here, not the VRP */
	*(ULONG *) (ior + 0x48) = (ULONG) (ior + 0x58);   /* room for IOS's SGDs, inside the packet as VFAT's */
	*(UCHAR *) (ior + 0x4D) = v->physical ? v->unit : v->drive;
	v->done = 0;
	cur_io = v;
	__asm__ __volatile__("pushal\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00100004\n\t"   /* IOS_SendCommand */
	                     "popal"
	                     :
	                     : "S"(esi), "D"(edi)
	                     : "memory", "cc");
	/* the thread sleeps until the callback wakes it. IFSMgr_Yield in a loop
	 * here nested the timer interrupt over and over until the ring-0 stack
	 * ran out (seen as a triple fault once the mount read the whole FAT) */
	if (func != IOR_MEDIA_CHECK_RESET)
	{
		__asm__ __volatile__("pushfl\n\tcli" ::: "memory");
		while (!v->done)
			ifs_block((ULONG) v);
		__asm__ __volatile__("popfl" ::: "memory", "cc");
	}
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

/* exFAT volumes already mounted, so a second mount finds the next one */
#define MAX_VOLS 8
static struct { UCHAR unit; ULONG start; } mounted[MAX_VOLS];
static int n_mounted;

static int is_mounted(UCHAR unit, ULONG start)
{
	int i;
	for (i = 0; i < n_mounted; i++)
		if (mounted[i].unit == unit && mounted[i].start == start)
			return 1;
	return 0;
}

static int try_at(struct kvol *v, UCHAR unit, ULONG start, UCHAR *sec)
{
	v->unit = unit;
	v->part_start = start;
	if (is_mounted(unit, start) || ios_rw(v, IOR_READ, 0, 1, sec))
		return 0;
	return !memcmp(sec + 3, "EXFAT   ", 8);
}

/* TODO: match the volume to its partition exactly (IOS's logical DCB);
 * for now the first exFAT volume not yet mounted, disk by disk */
static int find_on_disks(struct kvol *v, UCHAR *sec)
{
	UCHAR mbr[64 + 2];
	int u, i;
	v->physical = 1;
	for (u = 0x80; u < 0x88; u++)
	{
		v->unit = (UCHAR) u;
		v->part_start = 0;
		if (ios_rw(v, IOR_READ, 0, 1, sec))
			continue;
		if (!memcmp(sec + 3, "EXFAT   ", 8))
		{
			if (!is_mounted((UCHAR) u, 0))
				goto found;   /* no partition table: the whole disk */
			continue;
		}
		if (*(USHORT *) (sec + 510) != 0xAA55)
			continue;
		memcpy(mbr, sec + 0x1BE, sizeof mbr);
		for (i = 0; i < 4; i++)
		{
			UCHAR type = mbr[i * 16 + 4];
			ULONG start = *(ULONG *) (mbr + i * 16 + 8);
			/* 07 is what exFAT is given; 0C/0B/0E/06 when a tool kept the old type */
			if (type != 0x07 && type != 0x0C && type != 0x0B && type != 0x0E && type != 0x06)
				continue;
			if (try_at(v, (UCHAR) u, start, sec))
				goto found;
		}
	}
	v->physical = 0;
	return -1;
found:
	outs("kexfat: exFAT on disk ");
	outx(v->unit, 2);
	outs(" at sector ");
	outx(v->part_start, 8);
	outs("\r\n");
	return 0;
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

/* ---- volume functions ---------------------------------------------------------------- */
/* The interface is the one the Windows 95/98 DDK's ifs.h describes (field
 * meanings learned from it and from how IFSMgr calls us; nothing of it is
 * copied here). Stage 2: everything that reads works, everything that would
 * change the volume answers ERROR_WRITE_PROTECT. */
#define NUM_VOLFUNC 15
#define NUM_HNDLMISC 8
#define IFS_VERSION 0x030A
#define IFS_REVISION 0x10

#define ERROR_INVALID_FUNCTION_ 1
#define ERROR_FILE_NOT_FOUND_ 2
#define ERROR_PATH_NOT_FOUND_ 3
#define ERROR_NOT_ENOUGH_MEMORY_ 8
#define ERROR_NO_MORE_FILES_ 18
#define ERROR_WRITE_PROTECT_ 19
#define ERROR_READ_FAULT_ 30
#define ERROR_NEGATIVE_SEEK_ 131

/* ioreq fields that share storage (ifs.h's overlays) */
#define IR_ATTR(p) IR_LENGTH(p)             /* attributes, also search flags in the high bits */
#define IR_SIZE(p) IR_POS(p)
#define IR_SECTORS(p) IR_OPTIONS(p)
#define IR_NUMFREE(p) IR_AUX2(p)
#define IR_DOSTIME(p) IR_AUX2(p)            /* date << 16 | time */
#define IR_HFUNC(p) IR_AUX1(p)
#define IR_PPATH2(p) IR_AUX1(p)

#define FILE_FLAG_WILDCARDS 0x80000000u
#define FATTR_LABEL 0x08
#define FATTR_DIR 0x10

typedef int(__cdecl *ifsfn)(UCHAR *pir);

struct hndlmisc
{
	short version;
	char revision;
	char size;
	ifsfn func[NUM_HNDLMISC];
};

struct hndlfunc
{
	ifsfn read, write;
	struct hndlmisc *misc;
};

/* one request at a time per volume: the packet is shared, and the core is
 * not re-entrant. Threads that find it busy sleep on IFSMgr_Block. */
static void vol_lock(struct kvol *v)
{
	__asm__ __volatile__("pushfl\n\tcli" ::: "memory");
	while (v->busy)
		ifs_block((ULONG) &v->busy);
	v->busy = 1;
	__asm__ __volatile__("popfl" ::: "memory", "cc");
}

static void vol_unlock(struct kvol *v)
{
	v->busy = 0;
	ifs_wakeup((ULONG) &v->busy);
}

static int set_err(UCHAR *pir, int e)
{
	IR_ERROR(pir) = (short) e;
	return e;
}

static int win_err(int r, int last)
{
	switch (r)
	{
	case EXFAT_OK: return 0;
	case EXFAT_ENOENT: return last ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_;
	case EXFAT_ENOTDIR: return ERROR_PATH_NOT_FOUND_;
	case EXFAT_ENOMEM: return ERROR_NOT_ENOUGH_MEMORY_;
	case EXFAT_EROFS: return ERROR_WRITE_PROTECT_;
	case EXFAT_EISDIR: return ERROR_ACCESS_DENIED_;
	default: return ERROR_READ_FAULT_;
	}
}

/* IFSMgr's parsed path: total length (bytes, with this 4-byte header),
 * offset of the last element, then elements of a byte length (counting
 * itself) and UTF-16 characters */
#define PP_TOTAL(pp) (*(USHORT *) (pp))
#define PP_PREFIX(pp) (*(USHORT *) ((UCHAR *) (pp) + 2))

static int path_ok(ULONG pp)
{
	return pp >= 0xC0000000u && pp < 0xF0000000u;
}

/* walks from the root through the path's elements; with skip_last, stops at
 * the last element's directory. *last_name gets the last element. */
static int walk(struct kvol *v, ULONG pp, int skip_last, exfat_node *out, const USHORT **last_name, int *last_len)
{
	exfat_node *tmp;
	ULONG off = 4, total = PP_TOTAL(pp);
	int r = 0;
	exfat_root(v->ex, out);
	if (last_name)
	{
		*last_name = NULL;
		*last_len = 0;
	}
	tmp = (exfat_node *) _HeapAllocate(sizeof(*tmp), 0);
	if (!tmp)
		return EXFAT_ENOMEM;
	while (off + 2 <= total)
	{
		USHORT el = *(USHORT *) (pp + off);
		const USHORT *name = (const USHORT *) (pp + off + 2);
		int len = el >= 2 ? (el - 2) / 2 : 0;
		int is_last = off + el >= total || el < 2;
		if (is_last && last_name)
		{
			*last_name = name;
			*last_len = len;
		}
		if (is_last && skip_last)
			break;
		if (el < 2)
			break;
		r = exfat_find(v->ex, out, name, (size_t) len, tmp);
		if (r)
		{
			r = r == EXFAT_ENOENT && !is_last ? EXFAT_ENOTDIR : r;
			break;
		}
		memcpy(out, tmp, sizeof(*tmp));
		off += el;
	}
	_HeapFree(tmp, 0);
	return r;
}

/* DOS time (date << 16 | time, local) to FILETIME (UTC, 100 ns since 1601):
 * IFSMgr_DosToWin32Time applies the system's time zone, as VFAT does */
static void dos_to_filetime(ULONG dt, UCHAR ms10, ULONG *ft)
{
	ULONG lo, hi;
	unsigned long long t;
	if (!dt)
	{
		ft[0] = ft[1] = 0;
		return;
	}
	__asm__ __volatile__("pushl %2\n\t"
	                     "int $0x20\n\t"
	                     ".long 0x00400039\n\t"   /* IFSMgr_DosToWin32Time */
	                     "addl $4, %%esp"
	                     : "=a"(lo), "=d"(hi)
	                     : "r"(dt)
	                     : "ecx", "memory", "cc");
	t = ((unsigned long long) hi << 32 | lo) + (unsigned long long) ms10 * 100000u;
	ft[0] = (ULONG) t;
	ft[1] = (ULONG) (t >> 32);
}

/* case-insensitive (the volume's up-case table) match with * and ? */
static int wild(struct kvol *v, const USHORT *p, int pl, const USHORT *n, int nl)
{
	int star_p = -1, star_n = 0, i = 0, j = 0;
	while (j < nl)
	{
		if (i < pl && (p[i] == '?' || (p[i] != '*' && exfat_upcase(v->ex, p[i]) == exfat_upcase(v->ex, n[j]))))
		{
			i++;
			j++;
		}
		else if (i < pl && p[i] == '*')
		{
			star_p = i++;
			star_n = j;
		}
		else if (star_p >= 0)
		{
			i = star_p + 1;
			j = ++star_n;
		}
		else
			return 0;
	}
	while (i < pl && (p[i] == '*' || p[i] == '?'))
		i++;
	return i == pl;
}

static int name_matches(struct kvol *v, const USHORT *p, int pl, const USHORT *n, int nl)
{
	USHORT dotted[EXFAT_NAME_MAX + 2];
	int k, has_dot = 0;
	/* "*.*" is everything, as on FAT */
	if (pl == 3 && p[0] == '*' && p[1] == '.' && p[2] == '*')
		return 1;
	if (wild(v, p, pl, n, nl))
		return 1;
	/* DOS: a name without an extension also matches as "NAME." */
	for (k = 0; k < nl; k++)
		has_dot |= n[k] == '.';
	if (has_dot || nl > EXFAT_NAME_MAX)
		return 0;
	memcpy(dotted, n, (size_t) nl * 2);
	dotted[nl] = '.';
	return wild(v, p, pl, dotted, nl + 1);
}

/* ---- disk information, attributes, directories --------------------------------------- */
static int __cdecl vfn_getdiskinfo(UCHAR *pir)
{
	struct kvol *v = (struct kvol *) IR_RH(pir);
	struct exfat_info info;
	ULONG spc, clusters, free_cl;
	vol_lock(v);
	exfat_info(v->ex, &info);
	vol_unlock(v);
	spc = info.cluster_size / info.sector_size;
	clusters = info.clusters;
	free_cl = info.free_clusters;
	if (!(IR_OPTIONS(pir) & 1))
	{
		/* the old calls keep 16 bits: bigger "clusters" as VFAT reports big drives */
		while (clusters > 0xFFFF && spc < 0x8000)
		{
			spc <<= 1;
			clusters >>= 1;
			free_cl >>= 1;
		}
		if (clusters > 0xFFFF)
			clusters = 0xFFFF;
		if (free_cl > 0xFFFF)
			free_cl = 0xFFFF;
	}
	IR_LENGTH(pir) = info.sector_size;
	IR_SECTORS(pir) = (USHORT) spc;
	IR_SIZE(pir) = clusters;
	IR_NUMFREE(pir) = free_cl;
	IR_DATA(pir) = 0;
	return set_err(pir, 0);
}

#define GET_ATTRIBUTES 0
#define GET_ATTRIB_COMP_FILESIZE 2
#define GET_ATTRIB_MODIFY_DATETIME 4
#define GET_ATTRIB_LAST_ACCESS_DATETIME 6
#define GET_ATTRIB_CREATION_DATETIME 8
#define GET_ATTRIB_FIRST_CLUST 9

static int __cdecl vfn_fileattrib(UCHAR *pir)
{
	struct kvol *v = (struct kvol *) IR_RH(pir);
	exfat_node *n;
	int r;
	UCHAR f = IR_FLAGS(pir);
	if (f & 1)
		return set_err(pir, ERROR_WRITE_PROTECT_);   /* odd: changes the volume */
	if (!path_ok(IR_PPATH(pir)))
		return set_err(pir, ERROR_FILE_NOT_FOUND_);
	n = (exfat_node *) _HeapAllocate(sizeof(*n), 0);
	if (!n)
		return set_err(pir, ERROR_NOT_ENOUGH_MEMORY_);
	vol_lock(v);
	r = walk(v, IR_PPATH(pir), 0, n, NULL, NULL);
	vol_unlock(v);
	if (r)
	{
		_HeapFree(n, 0);
		return set_err(pir, win_err(r, 1));
	}
	switch (f)
	{
	case GET_ATTRIBUTES:
		IR_ATTR(pir) = n->is_root ? FATTR_DIR : n->attr & 0x37;
		break;
	case GET_ATTRIB_COMP_FILESIZE:
		IR_SIZE(pir) = (ULONG) n->size;
		break;
	case GET_ATTRIB_MODIFY_DATETIME:
		IR_DOSTIME(pir) = n->mtime;
		break;
	case GET_ATTRIB_LAST_ACCESS_DATETIME:
		IR_DOSTIME(pir) = n->atime;
		break;
	case GET_ATTRIB_CREATION_DATETIME:
		IR_DOSTIME(pir) = n->ctime;
		IR_OPTIONS(pir) = n->c10ms;
		break;
	case GET_ATTRIB_FIRST_CLUST:
		IR_AUX2(pir) = n->first_cluster;
		break;
	default:
		_HeapFree(n, 0);
		return set_err(pir, ERROR_INVALID_FUNCTION_);
	}
	_HeapFree(n, 0);
	return set_err(pir, 0);
}

#define CHECK_DIR 2
#define QUERY83_DIR 3
#define QUERYLONG_DIR 4

static int __cdecl vfn_dir(UCHAR *pir)
{
	struct kvol *v = (struct kvol *) IR_RH(pir);
	exfat_node *n;
	int r;
	UCHAR f = IR_FLAGS(pir);
	if (f != CHECK_DIR && f != QUERY83_DIR && f != QUERYLONG_DIR)
		return set_err(pir, ERROR_WRITE_PROTECT_);   /* create / delete */
	if (!path_ok(IR_PPATH(pir)))
		return set_err(pir, ERROR_PATH_NOT_FOUND_);
	n = (exfat_node *) _HeapAllocate(sizeof(*n), 0);
	if (!n)
		return set_err(pir, ERROR_NOT_ENOUGH_MEMORY_);
	vol_lock(v);
	r = walk(v, IR_PPATH(pir), 0, n, NULL, NULL);
	vol_unlock(v);
	if (!r && !n->is_root && !(n->attr & EXFAT_ATTR_DIR))
		r = EXFAT_ENOTDIR;
	_HeapFree(n, 0);
	if (r)
		return set_err(pir, ERROR_PATH_NOT_FOUND_);
	if (f != CHECK_DIR)
	{
		/* TODO: short names for QUERY83_DIR; both get the path as given */
		ULONG pp = IR_PPATH(pir), pp2 = IR_PPATH2(pir);
		if (path_ok(pp2))
			memcpy((void *) pp2, (void *) pp, PP_TOTAL(pp) + 2u);
	}
	return set_err(pir, 0);
}

static int __cdecl vfn_flush(UCHAR *pir)
{
	return set_err(pir, 0);
}

static int __cdecl vfn_wp(UCHAR *pir)   /* delete, rename: the volume is read-only for now */
{
	return set_err(pir, ERROR_WRITE_PROTECT_);
}

static int __cdecl vfn_bad(UCHAR *pir)
{
	return set_err(pir, ERROR_INVALID_FUNCTION_);
}

static int __cdecl vfn_search(UCHAR *pir)
{
	/* TODO: DOS searches (INT 21h 4Eh/4Fh) need 8.3 names */
	return set_err(pir, ERROR_NO_MORE_FILES_);
}

#define FS_CASE_IS_PRESERVED 0x00000002
#define FS_UNICODE_STORED_ON_DISK 0x00000004
#define FS_VOL_SUPPORTS_LONG_NAMES 0x00004000

static int __cdecl vfn_query(UCHAR *pir)
{
	/* ir_options 2: GetVolumeInformation's file system name and flags */
	if (IR_OPTIONS(pir) == 2)
	{
		static const char fsname[] = "EXFAT";
		ULONG buf = IR_DATA(pir);
		if (buf && IR_LENGTH(pir) >= sizeof(fsname))
			memcpy((void *) buf, fsname, sizeof(fsname));
		IR_LENGTH(pir) = (260u << 16) | 255;   /* longest path, longest name */
		IR_OPTIONS(pir) = FS_CASE_IS_PRESERVED | FS_UNICODE_STORED_ON_DISK | FS_VOL_SUPPORTS_LONG_NAMES;
		return set_err(pir, 0);
	}
	return set_err(pir, ERROR_INVALID_FUNCTION_);
}

/* ---- searches (FindFirstFile / FindNextFile) ------------------------------------------ */
struct kfind
{
	struct kvol *v;
	exfat_node dir;
	exfat_node ent;
	uint64_t cookie;
	int dots;         /* "." and ".." still to give (subdirectories only) */
	ULONG attr;       /* what the caller asked for: search bits, must-match bits */
	int plen;
	USHORT pat[EXFAT_NAME_MAX + 1];
};

struct find_data   /* WIN32_FIND_DATA, wide */
{
	ULONG attr;
	ULONG ctime[2], atime[2], mtime[2];
	ULONG size_high, size_low;
	ULONG reserved0, reserved1;
	USHORT name[260];
	USHORT alt[14];
};

static int attr_wanted(ULONG want, ULONG a)
{
	ULONG search = want & 0xFF, must = (want >> 8) & 0x3F;
	if (a & (EXFAT_ATTR_HIDDEN | EXFAT_ATTR_SYSTEM | EXFAT_ATTR_DIR) & ~search)
		return 0;
	return (a & must) == must;
}

static void fill_find(struct find_data *fd, const exfat_node *n, const USHORT *name, int len)
{
	memset(fd, 0, sizeof(*fd));
	fd->attr = n->attr & 0x37;
	dos_to_filetime(n->ctime, n->c10ms, fd->ctime);
	dos_to_filetime(n->atime, 0, fd->atime);
	dos_to_filetime(n->mtime, n->m10ms, fd->mtime);
	fd->size_high = (ULONG) (n->size >> 32);
	fd->size_low = (ULONG) n->size;
	if (len > 259)
		len = 259;
	memcpy(fd->name, name, (size_t) len * 2);
}

/* the next entry that matches, into the caller's find data */
static int find_step(struct kfind *k, struct find_data *fd)
{
	struct kvol *v = k->v;
	static const USHORT dot[2] = { '.', '.' };
	int r;
	while (k->dots)
	{
		int len = k->dots == 2 ? 1 : 2;
		k->dots--;
		if (attr_wanted(k->attr, EXFAT_ATTR_DIR) && name_matches(v, k->pat, k->plen, dot, len))
		{
			fill_find(fd, &k->dir, dot, len);
			fd->attr = FATTR_DIR;
			return 0;
		}
	}
	vol_lock(v);
	for (;;)
	{
		r = exfat_readdir(v->ex, &k->dir, &k->cookie, &k->ent);
		if (r)
			break;
		if (attr_wanted(k->attr, k->ent.attr) && name_matches(v, k->pat, k->plen, k->ent.name, k->ent.name_len))
			break;
	}
	vol_unlock(v);
	if (r == EXFAT_END)
		return ERROR_NO_MORE_FILES_;
	if (r)
		return win_err(r, 1);
	fill_find(fd, &k->ent, k->ent.name, k->ent.name_len);
	return 0;
}

static int __cdecl find_next(UCHAR *pir)
{
	struct kfind *k = (struct kfind *) IR_FH(pir);
	IR_POS(pir) = 0;
	return set_err(pir, find_step(k, (struct find_data *) IR_DATA(pir)));
}

#define CLOSE_FINAL 2

static int __cdecl find_close(UCHAR *pir)
{
	_HeapFree((void *) IR_FH(pir), 0);
	return set_err(pir, 0);
}

static struct hndlmisc find_misc = { IFS_VERSION, IFS_REVISION, NUM_HNDLMISC,
	                                 { vfn_bad, find_close, vfn_bad, vfn_bad, vfn_bad, vfn_bad, vfn_bad, vfn_bad } };

static int __cdecl vfn_findopen(UCHAR *pir)
{
	struct kvol *v = (struct kvol *) IR_RH(pir);
	struct kfind *k;
	const USHORT *pat;
	int plen, r;
	ULONG want = IR_ATTR(pir);
	if (!path_ok(IR_PPATH(pir)) || PP_TOTAL(IR_PPATH(pir)) == 4 || (want & 0xFF) == FATTR_LABEL)
		return set_err(pir, ERROR_FILE_NOT_FOUND_);
	k = (struct kfind *) _HeapAllocate(sizeof(*k), HEAPZEROINIT);
	if (!k)
		return set_err(pir, ERROR_NOT_ENOUGH_MEMORY_);
	k->v = v;
	k->attr = want & 0xFFFF;
	vol_lock(v);
	r = walk(v, IR_PPATH(pir), 1, &k->dir, &pat, &plen);
	vol_unlock(v);
	if (!r && !k->dir.is_root && !(k->dir.attr & EXFAT_ATTR_DIR))
		r = EXFAT_ENOTDIR;
	if (r || !pat || plen <= 0 || plen > EXFAT_NAME_MAX)
	{
		_HeapFree(k, 0);
		return set_err(pir, r ? ERROR_PATH_NOT_FOUND_ : ERROR_FILE_NOT_FOUND_);
	}
	memcpy(k->pat, pat, (size_t) plen * 2);
	k->plen = plen;
	k->dots = k->dir.is_root ? 0 : 2;
	r = find_step(k, (struct find_data *) IR_DATA(pir));
	if (r)
	{
		_HeapFree(k, 0);
		return set_err(pir, r == ERROR_NO_MORE_FILES_ ? ERROR_FILE_NOT_FOUND_ : r);
	}
	{
		struct hndlfunc *hf = (struct hndlfunc *) IR_HFUNC(pir);
		hf->read = find_next;
		hf->write = vfn_bad;
		hf->misc = &find_misc;
	}
	IR_FH(pir) = (ULONG) k;
	IR_POS(pir) = 0;
	return set_err(pir, 0);
}

/* ---- open files ---------------------------------------------------------------------- */
struct kfile
{
	struct kvol *v;
	exfat_node n;
};

#define ACCESS_MODE_MASK 0x0007
#define ACCESS_READONLY 0x0000
#define ACCESS_EXECUTE 0x0003
#define ACTION_TRUNCATE 0x02
#define ACTION_NEXISTS_CREATE 0x10
#define ACTION_OPENED 1
#define FILE_END 2
#define GET_MODIFY_DATETIME 0
#define GET_LAST_ACCESS_DATETIME 4
#define GET_CREATION_DATETIME 6
#define ENUMH_GETFILEINFO 0

static int __cdecl file_read(UCHAR *pir)
{
	struct kfile *f = (struct kfile *) IR_FH(pir);
	uint32_t got = 0;
	int r;
	vol_lock(f->v);
	r = exfat_read(f->v->ex, &f->n, IR_POS(pir), (void *) IR_DATA(pir), IR_LENGTH(pir), &got);
	vol_unlock(f->v);
	IR_LENGTH(pir) = got;
	IR_POS(pir) += got;
	return set_err(pir, r ? win_err(r, 1) : 0);
}

static int __cdecl file_write(UCHAR *pir)
{
	IR_LENGTH(pir) = 0;
	return set_err(pir, ERROR_WRITE_PROTECT_);
}

static int __cdecl file_seek(UCHAR *pir)
{
	struct kfile *f = (struct kfile *) IR_FH(pir);
	if (IR_FLAGS(pir) == FILE_END)
	{
		long pos = (long) (ULONG) f->n.size + (long) IR_POS(pir);
		if (pos < 0)
			return set_err(pir, ERROR_NEGATIVE_SEEK_);
		IR_POS(pir) = (ULONG) pos;
	}
	return set_err(pir, 0);
}

static int __cdecl file_close(UCHAR *pir)
{
	if (IR_FLAGS(pir) == CLOSE_FINAL)
		_HeapFree((void *) IR_FH(pir), 0);
	IR_POS(pir) = 0;
	return set_err(pir, 0);
}

static int __cdecl file_ok(UCHAR *pir)   /* commit, locks: nothing to do on a read-only volume */
{
	return set_err(pir, 0);
}

static int __cdecl file_times(UCHAR *pir)
{
	struct kfile *f = (struct kfile *) IR_FH(pir);
	switch (IR_FLAGS(pir))
	{
	case GET_MODIFY_DATETIME: IR_DOSTIME(pir) = f->n.mtime; break;
	case GET_LAST_ACCESS_DATETIME: IR_DOSTIME(pir) = f->n.atime; break;
	case GET_CREATION_DATETIME:
		IR_DOSTIME(pir) = f->n.ctime;
		IR_OPTIONS(pir) = f->n.c10ms;
		break;
	default: return set_err(pir, (IR_FLAGS(pir) & 1) ? ERROR_WRITE_PROTECT_ : ERROR_INVALID_FUNCTION_);
	}
	return set_err(pir, 0);
}

struct bhfi   /* BY_HANDLE_FILE_INFORMATION */
{
	ULONG attr;
	ULONG ctime[2], atime[2], mtime[2];
	ULONG serial, size_high, size_low, links, index_high, index_low;
};

static int __cdecl file_enum(UCHAR *pir)
{
	struct kfile *f = (struct kfile *) IR_FH(pir);
	if (IR_FLAGS(pir) == ENUMH_GETFILEINFO)
	{
		struct bhfi *b = (struct bhfi *) IR_DATA(pir);
		struct exfat_info info;
		exfat_info(f->v->ex, &info);
		memset(b, 0, sizeof(*b));
		b->attr = f->n.attr & 0x37;
		dos_to_filetime(f->n.ctime, f->n.c10ms, b->ctime);
		dos_to_filetime(f->n.atime, 0, b->atime);
		dos_to_filetime(f->n.mtime, f->n.m10ms, b->mtime);
		b->serial = info.serial;
		b->size_high = (ULONG) (f->n.size >> 32);
		b->size_low = (ULONG) f->n.size;
		b->links = 1;
		b->index_high = (ULONG) (f->n.entry >> 32);
		b->index_low = (ULONG) f->n.entry ^ f->n.pdir_cluster;
		return set_err(pir, 0);
	}
	return set_err(pir, ERROR_INVALID_FUNCTION_);
}

static struct hndlmisc file_misc = { IFS_VERSION, IFS_REVISION, NUM_HNDLMISC,
	                                 { file_seek, file_close, file_ok, file_ok, file_times, vfn_bad, vfn_bad, file_enum } };

static int __cdecl vfn_open(UCHAR *pir)
{
	struct kvol *v = (struct kvol *) IR_RH(pir);
	struct kfile *f;
	ULONG access = IR_FLAGS(pir) & ACCESS_MODE_MASK;
	ULONG action = IR_OPTIONS(pir) & 0xFF;
	int r;
	if (!path_ok(IR_PPATH(pir)) || (IR_ATTR(pir) & FATTR_LABEL))
		return set_err(pir, ERROR_ACCESS_DENIED_);
	f = (struct kfile *) _HeapAllocate(sizeof(*f), HEAPZEROINIT);
	if (!f)
		return set_err(pir, ERROR_NOT_ENOUGH_MEMORY_);
	f->v = v;
	vol_lock(v);
	r = walk(v, IR_PPATH(pir), 0, &f->n, NULL, NULL);
	vol_unlock(v);
	if (r)
	{
		_HeapFree(f, 0);
		if (r == EXFAT_ENOENT && (action & ACTION_NEXISTS_CREATE))
			return set_err(pir, ERROR_WRITE_PROTECT_);
		return set_err(pir, win_err(r, 1));
	}
	if (f->n.is_root || (f->n.attr & EXFAT_ATTR_DIR))
	{
		_HeapFree(f, 0);
		return set_err(pir, ERROR_ACCESS_DENIED_);
	}
	if ((access != ACCESS_READONLY && access != ACCESS_EXECUTE) || (action & ACTION_TRUNCATE))
	{
		_HeapFree(f, 0);
		return set_err(pir, ERROR_WRITE_PROTECT_);
	}
	{
		struct hndlfunc *hf = (struct hndlfunc *) IR_HFUNC(pir);
		hf->read = file_read;
		hf->write = file_write;
		hf->misc = &file_misc;
	}
	IR_FH(pir) = (ULONG) f;
	IR_OPTIONS(pir) = ACTION_OPENED;
	IR_SIZE(pir) = (ULONG) f->n.size;
	IR_ATTR(pir) = f->n.attr & 0x37;
	IR_DOSTIME(pir) = f->n.mtime;
	return set_err(pir, 0);
}

/* ---- the table ----------------------------------------------------------------------- */
static int __cdecl vfn_getdiskparms(UCHAR *pir)
{
	/* the DOS drive parameter block: not for a volume DOS cannot read */
	return set_err(pir, ERROR_INVALID_FUNCTION_);
}

struct volfunc
{
	short version;
	char revision;
	char size;
	ifsfn func[NUM_VOLFUNC];
};

static struct volfunc vfuncs = {
	IFS_VERSION, IFS_REVISION, NUM_VOLFUNC,
	{ vfn_wp /* delete */, vfn_dir, vfn_fileattrib, vfn_flush, vfn_getdiskinfo, vfn_open, vfn_wp /* rename */,
	  vfn_search, vfn_query, vfn_bad /* disconnect */, vfn_bad /* uncpipereq */, vfn_bad /* ioctl16drive */,
	  vfn_getdiskparms, vfn_findopen, vfn_bad /* dasdio */ }
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
	outs("kexfat: called from ");
	outx((ULONG) __builtin_return_address(0), 8);
	outs(", pir ");
	outx((ULONG) pir, 8);
	outs("\r\n");
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
	/* the packet from fixed, contiguous memory whose physical address goes in
	 * IOP_physical (0x00): IOS and the port drivers find the packet's parts
	 * in physical memory (bus-master DMA lists) from it */
	{
		ULONG phys = 0;
		unsigned long long r2 = _PageAllocate(1, PG_SYS, 0, 0, 0, 0xFFFFF, &phys,
		                                      PAGEFIXED | PAGECONTIG | PAGEUSEALIGN | PAGEZEROINIT);
		v->iop = (UCHAR *) (ULONG) (r2 >> 32);
		v->iop_phys = phys;
		outs("kexfat: packet at ");
		outx((ULONG) v->iop, 8);
		outs(", physical ");
		outx(phys, 8);
		outs("\r\n");
		if (!(ULONG) r2)
			v->iop = NULL;
	}
	sec = (UCHAR *) _HeapAllocate(4096, HEAPZEROINIT);
	if (!v->iop || !sec)
		goto free_no;
	v->part_start = *(ULONG *) (vrp + 0x10);
	{
		ULONG ldcb = *(ULONG *) (vrp + 0x28);
		v->phys_handle = (ldcb >= 0xC0000000u && *(ULONG *) ldcb >= 0xC0000000u) ? *(ULONG *) ldcb : 0;   /* the physical DCB (IOS routes by vol_designtr anyway) */
	}
	/* IOS refuses every request to the volume itself (IORS_UNFORMATTED_MEDIA):
	 * its logical layer found no FAT BPB. The disk underneath takes them, so
	 * the volume is found on the disks and read through them. */
	r = find_on_disks(v, sec);
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
		if (n_mounted < MAX_VOLS)
		{
			mounted[n_mounted].unit = v->unit;
			mounted[n_mounted].start = v->part_start;
			n_mounted++;
		}
		outs("kexfat: mounted, ");
		outx(info.clusters, 8);
		outs(" clusters of ");
		outx(info.cluster_size, 8);
		outs(" bytes, ");
		outx(info.free_clusters, 8);
		outs(" free\r\n");
	}
#ifdef KEXFAT_DECLINE_TEST
	goto free_no;
#endif
	IR_RH(pir) = (ULONG) v;
	/* ir_vfunc shares ir_aux1 with ir_volh (the VRP coming in): the DDK's ifs.h
	 * defines both on ir_aux1. Put in ir_data, IFSMgr took the VRP for the
	 * table and called into it (a fault in IOS's memory, then a triple fault) */
	IR_AUX1(pir) = (ULONG) &vfuncs;
	IR_ERROR(pir) = 0;
	outs("kexfat: mount done\r\n");
	return 0;
free_no:
	/* TODO: _PageFree the packet */
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
