/*
 *  KernelExEx - ramcore: the portable part of the RAM disk (KEXRAM.VXD).
 *  Which RAM it may use (from KEXMEM's copy of the BIOS memory map), how disk
 *  sectors map onto 4 MB chunks of that RAM, and the FAT volume written into it
 *  at every start. No OS calls: unit-tested on the build host.
 *
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#ifndef RAMCORE_H
#define RAMCORE_H

typedef unsigned char rc_u8;
typedef unsigned short rc_u16;
typedef unsigned int rc_u32;
typedef unsigned long long rc_u64;

#define RC_CHUNK_SHIFT 22                     /* 4 MB: one page-directory entry */
#define RC_CHUNK (1u << RC_CHUNK_SHIFT)
#define RC_SECTORS_PER_CHUNK (RC_CHUNK / 512)
#define RC_MAX_CHUNKS 16384                   /* 64 GB: PSE-36's reach */

/* one BIOS (E820) memory map entry, as KEXMEM keeps it */
struct rc_e820
{
	rc_u64 base, len;
	rc_u32 type;
};

/* chunks[i] = physical address of chunk i / 4 MB (so it fits 32 bits); returns
   the number of chunks. RAM (type 1) at or above `limit` (KEXMEM's limit: what
   Windows may use) is taken; above 4 GB only if `above4g` (the CPU has PSE-36).
   Every chunk is a whole 4 MB, 4 MB aligned, inside one RAM entry. */
rc_u32 rc_chunks(const struct rc_e820 *map, int n, rc_u64 limit, int above4g, rc_u32 *chunks, rc_u32 max);

/* The volume: FAT32 (4 KB clusters) from 260 MB, FAT16 below; the layout of the
   first sectors and what must be zero. `secs` = disk size in 512-byte sectors. */
struct rc_fat
{
	rc_u32 secs;          /* total sectors */
	int fat32;
	rc_u32 spc;           /* sectors per cluster */
	rc_u32 rsvd;          /* reserved sectors (boot sector, FSInfo...) */
	rc_u32 fatsz;         /* sectors per FAT (2 FATs) */
	rc_u32 rootsecs;      /* FAT16: root directory sectors */
	rc_u32 zero_end;      /* sectors [0, zero_end) must be written (rest: data area) */
};
/* 0 = ok; -1 = too small for a volume */
int rc_fat_layout(rc_u32 secs, struct rc_fat *f);
/* the content of sector `s` < f->zero_end (512 bytes into `out`) */
void rc_fat_sector(const struct rc_fat *f, rc_u32 serial, rc_u32 s, rc_u8 *out);

#endif
