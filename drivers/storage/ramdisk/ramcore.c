/*
 *  KernelExEx - ramcore: see ramcore.h. FAT layout per Microsoft's FAT
 *  specification (fatgen103, "FAT: General Overview of On-Disk Format").
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "ramcore.h"

rc_u32 rc_chunks(const struct rc_e820 *map, int n, rc_u64 limit, int above4g, rc_u32 *chunks, rc_u32 max)
{
	rc_u32 cnt = 0;
	rc_u64 top = above4g ? (rc_u64) 64 << 30 : (rc_u64) 4 << 30;
	int i;
	for (i = 0; i < n; i++)
	{
		rc_u64 a = map[i].base, b = map[i].base + map[i].len, c;
		if (map[i].type != 1 || !map[i].len || b < a)
			continue;
		if (a < limit)
			a = limit;
		if (b > top)
			b = top;
		a = (a + RC_CHUNK - 1) & ~(rc_u64) (RC_CHUNK - 1);
		b &= ~(rc_u64) (RC_CHUNK - 1);
		for (c = a; c + RC_CHUNK <= b && cnt < max; c += RC_CHUNK)
		{
			/* a chunk listed twice (overlapping BIOS entries) is taken once */
			rc_u32 k, id = (rc_u32) (c >> RC_CHUNK_SHIFT), dup = 0;
			for (k = 0; k < cnt; k++)
				if (chunks[k] == id)
					dup = 1;
			if (!dup)
				chunks[cnt++] = id;
		}
	}
	/* other entries (reserved, ACPI) must not overlap a chunk: drop any that does */
	for (i = 0; i < n; i++)
	{
		rc_u32 k;
		if (map[i].type == 1 || !map[i].len)
			continue;
		for (k = 0; k < cnt;)
		{
			rc_u64 c = (rc_u64) chunks[k] << RC_CHUNK_SHIFT;
			if (map[i].base < c + RC_CHUNK && map[i].base + map[i].len > c)
				chunks[k] = chunks[--cnt];
			else
				k++;
		}
	}
	return cnt;
}

#define RSVD32 32
int rc_fat_layout(rc_u32 secs, struct rc_fat *f)
{
	rc_u32 t1, t2;
	f->secs = secs;
	f->spc = 8;                               /* 4 KB clusters: the pager's page size */
	f->fat32 = secs >= 532480;                /* 260 MB: enough clusters for FAT32 */
	if (secs < 16384)                         /* 8 MB */
		return -1;
	if (f->fat32)
	{
		f->rsvd = RSVD32;
		f->rootsecs = 0;
		t1 = secs - f->rsvd;
		t2 = (256 * f->spc + 2) / 2;
	}
	else
	{
		f->rsvd = 1;
		f->rootsecs = 32;                     /* 512 entries */
		t1 = secs - f->rsvd - f->rootsecs;
		t2 = 256 * f->spc + 2;
	}
	f->fatsz = (t1 + t2 - 1) / t2;
	f->zero_end = f->rsvd + 2 * f->fatsz + (f->fat32 ? f->spc : f->rootsecs);
	return 0;
}

static void w16(rc_u8 *p, rc_u32 v) { p[0] = (rc_u8) v; p[1] = (rc_u8) (v >> 8); }
static void w32(rc_u8 *p, rc_u32 v) { w16(p, v); w16(p + 2, v >> 16); }
static void cpy(rc_u8 *d, const char *s, int n) { while (n--) *d++ = (rc_u8) *s++; }

static void boot(const struct rc_fat *f, rc_u32 serial, rc_u8 *b)
{
	rc_u8 *x;
	b[0] = 0xEB; b[1] = f->fat32 ? 0x58 : 0x3C; b[2] = 0x90;
	cpy(b + 3, "MSWIN4.1", 8);                /* what Windows 98 itself writes */
	w16(b + 11, 512);
	b[13] = (rc_u8) f->spc;
	w16(b + 14, f->rsvd);
	b[16] = 2;
	w16(b + 17, f->fat32 ? 0 : f->rootsecs * 16);
	w16(b + 19, f->secs < 65536 ? f->secs : 0);
	b[21] = 0xF8;
	w16(b + 22, f->fat32 ? 0 : f->fatsz);
	w16(b + 24, 63);
	w16(b + 26, 255);
	w32(b + 28, 0);
	w32(b + 32, f->secs < 65536 ? 0 : f->secs);
	if (f->fat32)
	{
		w32(b + 36, f->fatsz);
		w32(b + 44, 2);                       /* root directory cluster */
		w16(b + 48, 1);                       /* FSInfo */
		w16(b + 50, 6);                       /* backup boot sector */
		x = b + 64;
	}
	else
		x = b + 36;
	x[0] = 0x80;                              /* drive number */
	x[2] = 0x29;
	w32(x + 3, serial);
	cpy(x + 7, "KEXRAM SWAP", 11);
	cpy(x + 18, f->fat32 ? "FAT32   " : "FAT16   ", 8);
	/* not bootable: a tiny message loop instead of code (int 18h) */
	x[26] = 0xCD; x[27] = 0x18;
	b[510] = 0x55; b[511] = 0xAA;
}

void rc_fat_sector(const struct rc_fat *f, rc_u32 serial, rc_u32 s, rc_u8 *out)
{
	rc_u32 i, fat0 = f->rsvd, fat1 = f->rsvd + f->fatsz;
	for (i = 0; i < 512; i++)
		out[i] = 0;
	if (s == 0 || (f->fat32 && s == 6))
		boot(f, serial, out);
	else if (f->fat32 && (s == 1 || s == 7))
	{
		/* FSInfo: every cluster but the root directory's is free; next free 3 */
		rc_u32 clusters = (f->secs - f->rsvd - 2 * f->fatsz) / f->spc;
		w32(out, 0x41615252);
		w32(out + 484, 0x61417272);
		w32(out + 488, clusters - 1);
		w32(out + 492, 3);
		w32(out + 508, 0xAA550000);
	}
	else if (f->fat32 && s == 2)
		w32(out + 508, 0xAA550000);           /* the third boot sector's signature */
	else if (s == fat0 || s == fat1)
	{
		if (f->fat32)
		{
			w32(out, 0x0FFFFFF8);
			w32(out + 4, 0x0FFFFFFF);
			w32(out + 8, 0x0FFFFFFF);         /* root directory: one cluster, end */
		}
		else
		{
			w16(out, 0xFFF8);
			w16(out + 2, 0xFFFF);
		}
	}
	else if (s == fat1 + f->fatsz)
	{
		/* first sector of the root directory: the volume label */
		cpy(out, "KEXRAM SWAP", 11);
		out[11] = 0x08;
	}
}
