/*
 *  KernelExEx - KEXPREP.EXE: prepares a disk for Windows 98 during the
 *  KernelExEx install (DOS, real mode, 386 or later).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  KEXPREP          list the disks, ask which one to use, erase it: one
 *                   FAT32 partition (up to 120 GB, the rest left free), our
 *                   MBR, formatted as Windows 98's FORMAT would, with a
 *                   marker file KEXTARGT.MRK in its root. Exit code 2: done,
 *                   restart needed (DOS sees new partitions only after one);
 *                   1: nothing done.
 *  KEXPREP REBOOT   restart the computer.
 *
 *  Windows 98's FDISK miscounts disks over 64 GB, and its tools fail over
 *  about 128 GB, which is why this exists. The disk the setup runs from (the
 *  RAM disk, MBR signature KXSU) and the install medium (KXIS) are never
 *  offered. Built with Open Watcom, without its C library (crt0.asm).
 */
#include "mbrcode.h"   /* mbr_code[440]: mbr.asm, assembled */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

extern u16 _psp;

/* ---- DOS ------------------------------------------------------------------------------------ */
static void putch(char c)
{
	if (c == '\n')
		putch('\r');
	_asm {
		mov dl, c
		mov ah, 2
		int 21h
	}
}

static void say(const char *s)
{
	while (*s)
		putch(*s++);
}

static void sayu(u32 v)
{
	char b[12];
	int i = 0;
	do
	{
		b[i++] = (char) ('0' + (u8) (v % 10));
		v /= 10;
	} while (v);
	while (i)
		putch(b[--i]);
}

static u8 inbuf[2 + 80];

/* a line from the keyboard, without the CR */
static void ask(char *out, int size)
{
	int i, n;
	u8 *ib = inbuf;   /* (an asm "offset" of a BSS variable would be segment-, not group-relative) */
	inbuf[0] = 80;
	inbuf[1] = 0;
	_asm {
		mov dx, ib
		mov ah, 0Ah
		int 21h
	}
	putch('\n');
	n = inbuf[1];
	for (i = 0; i < n && i + 1 < size; i++)
		out[i] = (char) inbuf[2 + i];
	out[i] = 0;
}

static int same(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		char x = *a, y = *b;
		if (x >= 'a' && x <= 'z')
			x -= 32;
		if (y >= 'a' && y <= 'z')
			y -= 32;
		if (x != y)
			return 0;
	}
	return *a == *b;
}

static void quit(u8 code)
{
	_asm {
		mov al, code
		mov ah, 4Ch
		int 21h
	}
}

static int mcmp4(const u8 *a, const char *b)
{
	return a[0] != (u8) b[0] || a[1] != (u8) b[1] || a[2] != (u8) b[2] || a[3] != (u8) b[3];
}

static void mset(void *d, u8 v, u16 n)
{
	u8 *p = (u8 *) d;
	while (n--)
		*p++ = v;
}

static void mcpy(void *d, const void *s, u16 n)
{
	u8 *p = (u8 *) d;
	const u8 *q = (const u8 *) s;
	while (n--)
		*p++ = *q++;
}

/* ---- BIOS disks ------------------------------------------------------------------------- */
#pragma pack(1)
struct dap
{
	u8 size, zero;
	u16 count;
	u16 off, seg;
	u32 lba_lo, lba_hi;
};

struct params
{
	u16 size, flags;
	u32 cyl, heads, spt;
	u32 sec_lo, sec_hi;
	u16 bps;
};
#pragma pack()

static struct dap dp;
static struct params pm;
static u8 sector[512];
static u8 big[32768];   /* 64 sectors */

static int has_ext(u8 drive)
{
	u16 bxv = 0, cxv = 0;
	u8 bad = 1;
	_asm {
		mov ah, 41h
		mov bx, 55AAh
		mov dl, drive
		int 13h
		setc bad
		mov bxv, bx
		mov cxv, cx
	}
	return !bad && bxv == 0xAA55 && (cxv & 1);
}

/* the size in sectors (0: no such disk or no extensions); over 2 TB is cut to 2 TB */
static u32 disk_size(u8 drive)
{
	u8 bad = 1;
	struct params *pp = &pm;
	if (!has_ext(drive))
		return 0;
	mset(&pm, 0, sizeof(pm));
	pm.size = sizeof(pm);
	_asm {
		mov ah, 48h
		mov dl, drive
		mov si, pp
		int 13h
		setc bad
	}
	if (bad || (pm.bps && pm.bps != 512))
		return 0;
	if (pm.sec_hi)
		return 0xFFFFFFFFul;
	return pm.sec_lo;
}

static int xfer(u8 drive, u32 lba, u16 count, void *buf, int write)
{
	u8 bad = 1, fn = (u8) (write ? 0x43 : 0x42);
	u16 dseg = 0;
	struct dap *pd = &dp;
	_asm {
		mov dseg, ds
	}
	dp.size = 16;
	dp.zero = 0;
	dp.count = count;
	dp.off = (u16) buf;
	dp.seg = dseg;
	dp.lba_lo = lba;
	dp.lba_hi = 0;
	_asm {
		mov ah, fn
		mov al, 0
		mov dl, drive
		mov si, pd
		int 13h
		setc bad
	}
	return !bad;
}

static void wr_ok(u8 drive, u32 lba, u16 count, void *buf)
{
	if (!xfer(drive, lba, count, buf, 1))
	{
		say("\nWriting to the disk failed (sector ");
		sayu(lba);
		say("). Nothing more was done; the disk is not usable as it is.\n");
		quit(1);
	}
}

/* write `n` zero sectors from `lba` */
static void zero_run(u8 drive, u32 lba, u32 n)
{
	mset(big, 0, sizeof(big));
	while (n)
	{
		u16 k = (u16) (n > 64 ? 64 : n);
		wr_ok(drive, lba, k, big);
		lba += k;
		n -= k;
	}
}

/* ---- the list of disks ------------------------------------------------------------------- */
#define MAXD 8
static struct
{
	u8 drive;
	u32 size;
	char note[64];
} disks[MAXD];
static int ndisks;

static const char *ptype(u8 t)
{
	switch (t)
	{
	case 0x01: case 0x04: case 0x06: case 0x0E: return "FAT";
	case 0x0B: case 0x0C: return "FAT32";
	case 0x07: return "NTFS or exFAT";
	case 0x05: case 0x0F: return "extended";
	case 0x27: return "Windows recovery";
	case 0x82: case 0x83: return "Linux";
	case 0xEE: return "GPT";
	case 0xEF: return "EFI";
	}
	return "other";
}

static void addnote(char *n, const char *s)
{
	int i = 0;
	while (n[i])
		i++;
	while (*s && i < 62)
		n[i++] = *s++;
	n[i] = 0;
}

static void scan(void)
{
	u8 d;
	for (d = 0x80; d < 0x88 && ndisks < MAXD; d++)
	{
		u32 sz = disk_size(d);
		int i, any = 0;
		if (!sz || !xfer(d, 0, 1, sector, 0))
			continue;
		/* ours: the setup RAM disk and the install medium */
		if (!mcmp4(sector + 0x1B8, "KXSU") || !mcmp4(sector + 0x1B8, "KXIS"))
			continue;
		disks[ndisks].drive = d;
		disks[ndisks].size = sz;
		disks[ndisks].note[0] = 0;
		if (sector[510] == 0x55 && sector[511] == 0xAA)
			for (i = 0; i < 4; i++)
			{
				u8 t = sector[0x1BE + i * 16 + 4];
				if (!t)
					continue;
				addnote(disks[ndisks].note, any ? ", " : "holds: ");
				addnote(disks[ndisks].note, ptype(t));
				any = 1;
			}
		if (!any)
			addnote(disks[ndisks].note, "empty");
		ndisks++;
	}
}

static void say_gb(u32 sectors)
{
	u32 gb10 = sectors / 209715ul;   /* tenths of a GB (1 GB = 2097152 sectors) */
	sayu(gb10 / 10);
	putch('.');
	sayu(gb10 % 10);
	say(" GB");
}

/* ---- FAT32 --------------------------------------------------------------------------------- */
#define PSTART 2048ul
#define PMAX 251658240ul   /* 120 GB */

static void put16(u8 *p, u16 v)
{
	p[0] = (u8) v;
	p[1] = (u8) (v >> 8);
}

static void put32(u8 *p, u32 v)
{
	put16(p, (u16) v);
	put16(p + 2, (u16) (v >> 16));
}

static const char marker_text[] = "KernelExEx: this disk was prepared for Windows 98.\r\n";

static void prepare(u8 drive, u32 disk)
{
	u32 psize = disk - PSTART, fatsz, clusters, data, tmp1, tmp2, i;
	u8 spc;
	u16 k;
	if (psize > PMAX)
		psize = PMAX;
	/* cluster size as Windows 98's FORMAT picks it */
	if (psize <= 532480ul)
		spc = 1;
	else if (psize <= 16777216ul)
		spc = 8;
	else if (psize <= 33554432ul)
		spc = 16;
	else if (psize <= 67108864ul)
		spc = 32;
	else
		spc = 64;
	/* FAT size: Microsoft's FAT32 formula (reserved 32, two FATs) */
	tmp1 = psize - 32;
	tmp2 = (256ul * spc + 2) / 2;
	fatsz = (tmp1 + tmp2 - 1) / tmp2;
	data = 32 + 2 * fatsz;
	clusters = (psize - data) / spc;

	say("Erasing the old partition tables...\n");
	zero_run(drive, 0, PSTART);                 /* MBR, GPT header and entries, old boot code */
	if (disk > 64)
		zero_run(drive, disk - 33, 33);         /* the backup GPT at the end */

	say("Writing the new partition table...\n");
	mset(sector, 0, 512);
	mcpy(sector, mbr_code, 440);
	sector[0x1B8] = 'K';
	sector[0x1B9] = 'X';
	sector[0x1BA] = 'T';
	sector[0x1BB] = 'G';
	sector[0x1BE] = 0x80;                       /* active */
	sector[0x1BF] = 0xFE;                       /* CHS: beyond 8 GB, LBA only */
	sector[0x1C0] = 0xFF;
	sector[0x1C1] = 0xFF;
	sector[0x1C2] = 0x0C;                       /* FAT32, LBA */
	sector[0x1C3] = 0xFE;
	sector[0x1C4] = 0xFF;
	sector[0x1C5] = 0xFF;
	put32(sector + 0x1C6, PSTART);
	put32(sector + 0x1CA, psize);
	sector[510] = 0x55;
	sector[511] = 0xAA;
	wr_ok(drive, 0, 1, sector);

	say("Formatting (FAT32, ");
	sayu((u32) spc / 2);
	say(" KB clusters)...\n");
	/* the boot sector: a BPB as FORMAT writes it; SYS puts the boot code in later */
	mset(sector, 0, 512);
	sector[0] = 0xEB;
	sector[1] = 0x58;
	sector[2] = 0x90;
	mcpy(sector + 3, "MSWIN4.1", 8);
	put16(sector + 11, 512);
	sector[13] = spc;
	put16(sector + 14, 32);
	sector[16] = 2;
	sector[21] = 0xF8;
	put16(sector + 24, 63);
	put16(sector + 26, 255);
	put32(sector + 28, PSTART);
	put32(sector + 32, psize);
	put32(sector + 36, fatsz);
	put32(sector + 44, 2);                      /* root directory cluster */
	put16(sector + 48, 1);                      /* FSInfo */
	put16(sector + 50, 6);                      /* backup boot sector */
	sector[64] = 0x80;
	sector[66] = 0x29;
	{
		/* the volume serial: from the BIOS clock ticks */
		u16 lo = 0, hi = 0;
		_asm {
			mov ah, 0
			int 1Ah
			mov lo, dx
			mov hi, cx
		}
		put32(sector + 67, ((u32) hi << 16 | lo) ^ 0x4B580000ul);
	}
	mcpy(sector + 71, "KEXWIN98   ", 11);
	mcpy(sector + 82, "FAT32   ", 8);
	/* not bootable yet: say so if someone tries */
	sector[0x5A] = 0xCD;                        /* int 18h */
	sector[0x5B] = 0x18;
	sector[510] = 0x55;
	sector[511] = 0xAA;
	wr_ok(drive, PSTART, 1, sector);
	wr_ok(drive, PSTART + 6, 1, sector);
	/* FSInfo */
	mset(sector, 0, 512);
	put32(sector, 0x41615252ul);
	put32(sector + 484, 0x61417272ul);
	put32(sector + 488, clusters - 2);           /* clusters 2 and 3 are in use */
	put32(sector + 492, 4);
	put32(sector + 508, 0xAA550000ul);
	wr_ok(drive, PSTART + 1, 1, sector);
	wr_ok(drive, PSTART + 7, 1, sector);
	/* the rest of the reserved area */
	mset(sector, 0, 512);
	for (i = 2; i < 32; i++)
		if (i != 6 && i != 7)
			wr_ok(drive, PSTART + i, 1, sector);
	/* both FATs: zero, then the first sector of each */
	say("  clearing the FATs (");
	sayu(fatsz * 2 / 2048);
	say(" MB)...\n");
	zero_run(drive, PSTART + 32, fatsz * 2);
	mset(sector, 0, 512);
	put32(sector + 0, 0x0FFFFFF8ul);
	put32(sector + 4, 0x0FFFFFFFul);
	put32(sector + 8, 0x0FFFFFFFul);             /* root directory: one cluster */
	put32(sector + 12, 0x0FFFFFFFul);            /* the marker file: one cluster */
	wr_ok(drive, PSTART + 32, 1, sector);
	wr_ok(drive, PSTART + 32 + fatsz, 1, sector);
	/* the root directory (cluster 2) and the marker (cluster 3) */
	zero_run(drive, PSTART + data, (u32) spc * 2);
	mset(sector, 0, 512);
	mcpy(sector, "KEXWIN98   ", 11);
	sector[11] = 0x08;                          /* volume label */
	mcpy(sector + 32, "KEXTARGTMRK", 11);
	sector[32 + 11] = 0x20;
	put16(sector + 32 + 22, 0);                  /* 00:00 */
	put16(sector + 32 + 24, (u16) (((2026 - 1980) << 9) | (1 << 5) | 1));   /* 1 January 2026 */
	put16(sector + 32 + 26, 3);                  /* first cluster */
	for (k = 0; marker_text[k]; k++)
		;
	put32(sector + 32 + 28, k);
	wr_ok(drive, PSTART + data, 1, sector);
	mset(sector, 0, 512);
	mcpy(sector, marker_text, k);
	wr_ok(drive, PSTART + data + spc, 1, sector);
	say("Done.\n");
}

static void reboot(void)
{
	/* tell the BIOS: warm boot, then reset through the keyboard controller */
	_asm {
		mov ax, 40h
		mov es, ax
		mov word ptr es:[72h], 1234h
		mov al, 0FEh
		out 64h, al
		db 0EAh
		dw 0, 0FFFFh
	}
}

/* ---- main ----------------------------------------------------------------------------------- */
static char tail[128];

void main(void)
{
	u16 psp = _psp;
	u8 n, i;
	char *tp = tail;
	char line[16];
	int choice = 0;
	/* the command tail from the PSP */
	_asm {
		push ds
		mov ax, ds
		mov es, ax
		mov ds, psp
		mov si, 80h
		mov di, tp
		xor ch, ch
		mov cl, [si]
		inc si
		cld
		rep movsb
		mov al, 0
		stosb
		pop ds
	}
	for (i = 0; tail[i] == ' '; i++)
		;
	if (same(tail + i, "REBOOT"))
		reboot();

	say("\nKernelExEx: preparing a disk for Windows 98\n\n");
	scan();
	if (!ndisks)
	{
		say("No hard disk found. Check that the disk is connected and that the\n"
		    "BIOS shows it (SATA mode: AHCI or IDE both work here).\n");
		quit(1);
	}
	say("Disks:\n");
	for (n = 0; n < ndisks; n++)
	{
		say("  ");
		sayu(n + 1);
		say(")  ");
		say_gb(disks[n].size);
		say(" (BIOS disk ");
		sayu(disks[n].drive - 0x80u + 1);
		say("), ");
		say(disks[n].note);
		say("\n");
	}
	if (ndisks == 1)
		choice = 1;
	else
	{
		say("\nWhich disk should Windows 98 go on? Type its number, or Enter to stop: ");
		ask(line, sizeof(line));
		if (line[0] >= '1' && line[0] <= '0' + ndisks && !line[1])
			choice = line[0] - '0';
		if (!choice)
		{
			say("Stopped; nothing was changed.\n");
			quit(1);
		}
	}
	n = (u8) (choice - 1);
	say("\nEVERYTHING on disk ");
	sayu(choice);
	say(" (");
	say_gb(disks[n].size);
	say(") will be erased:\nevery partition and every file, including any other operating system.\n"
	    "Windows 98 gets ");
	say_gb(disks[n].size - PSTART > PMAX ? PMAX : disks[n].size - PSTART);
	say(disks[n].size - PSTART > PMAX ? " of it; the rest stays free.\n" : ".\n");
	say("\nType ERASE (in capitals) to go ahead, or anything else to stop: ");
	ask(line, sizeof(line));
	if (line[0] != 'E' || line[1] != 'R' || line[2] != 'A' || line[3] != 'S' || line[4] != 'E' || line[5])
	{
		say("Stopped; nothing was changed.\n");
		quit(1);
	}
	prepare(disks[n].drive, disks[n].size);
	quit(2);
}
