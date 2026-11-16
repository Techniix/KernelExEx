/*
 *  KernelExEx - exFAT file system core, portable C (no OS calls).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 *
 *  Written from Microsoft's published exFAT specification. Used by the
 *  Windows 98 file system driver (fs/exfat/win98) and by the host tests
 *  (tests/exfat). The caller supplies sector I/O and memory; this file
 *  does the on-disk format: boot region and checksum, FAT and NoFatChain
 *  files, allocation bitmap, up-case table, directory entry sets with
 *  checksums and name hashes.
 *
 *  Names are UTF-16 (uint16_t), compared through the volume's up-case
 *  table. Times are DOS date/time (as exFAT stores them) plus 10 ms units.
 */
#ifndef KEX_EXFAT_H
#define KEX_EXFAT_H

#include <stddef.h>
#include <stdint.h>

#define EXFAT_OK 0
#define EXFAT_EIO (-1)
#define EXFAT_ENOENT (-2)
#define EXFAT_EEXIST (-3)
#define EXFAT_ENOSPC (-4)
#define EXFAT_ENOTDIR (-5)
#define EXFAT_EISDIR (-6)
#define EXFAT_ENOTEMPTY (-7)
#define EXFAT_EINVAL (-8)
#define EXFAT_EROFS (-9)
#define EXFAT_ECORRUPT (-10)
#define EXFAT_ENOMEM (-11)
#define EXFAT_END 1   /* readdir: no more entries */

#define EXFAT_ATTR_RO 0x01
#define EXFAT_ATTR_HIDDEN 0x02
#define EXFAT_ATTR_SYSTEM 0x04
#define EXFAT_ATTR_DIR 0x10
#define EXFAT_ATTR_ARCHIVE 0x20

#define EXFAT_NAME_MAX 255

struct exfat_dev
{
	void *ctx;
	uint32_t sector_size;   /* the device's; the volume must use the same (0: not checked) */
	/* sectors of the volume (sector 0 = its boot sector); 0 on success */
	int (*read)(void *ctx, uint64_t sector, uint32_t count, void *buf);
	int (*write)(void *ctx, uint64_t sector, uint32_t count, const void *buf);
	void *(*alloc)(void *ctx, size_t size);   /* zeroed memory */
	void (*free)(void *ctx, void *p);
};

/* when the file system needs the time (creating, writing) */
struct exfat_time
{
	uint32_t dos;     /* date << 16 | time, as FAT */
	uint8_t ms10;     /* 0..199: 10 ms units past the 2-second DOS time */
	uint8_t utc_off;  /* exFAT's UtcOffset byte (0 = not given) */
};

typedef struct exfat_vol exfat_vol;

/* a file or directory: where its entry set is and what it says */
typedef struct exfat_node
{
	/* the parent directory's data, and the entry set's place in it */
	uint32_t pdir_cluster;
	uint64_t pdir_size;
	uint8_t pdir_nofat;
	uint64_t entry;        /* byte offset of the File entry in the parent */
	uint8_t secondaries;   /* entries after the File entry */
	uint8_t is_root;

	uint16_t attr;
	uint8_t nofat;         /* clusters contiguous, no FAT chain */
	uint32_t first_cluster;
	uint64_t size;         /* DataLength */
	uint64_t valid;        /* ValidDataLength */
	uint32_t ctime, mtime, atime;
	uint8_t c10ms, m10ms;
	uint8_t ctz, mtz, atz;
	uint8_t name_len;
	uint16_t name[EXFAT_NAME_MAX + 1];

	/* a cursor into the cluster chain, for sequential access */
	uint32_t cur_index, cur_cluster;
} exfat_node;

struct exfat_info
{
	uint32_t sector_size;
	uint32_t cluster_size;
	uint32_t clusters;
	uint32_t free_clusters;
	uint32_t serial;
	uint16_t label[12];
	uint8_t label_len;
};

int exfat_mount(exfat_vol **pv, const struct exfat_dev *dev, int readonly);
int exfat_flush(exfat_vol *v);
void exfat_unmount(exfat_vol *v);
void exfat_info(exfat_vol *v, struct exfat_info *out);
void exfat_set_clock(exfat_vol *v, void (*now)(void *ctx, struct exfat_time *t), void *ctx);

void exfat_root(exfat_vol *v, exfat_node *out);
/* path: names separated by '\\' or '/', relative to the root; "" is the root */
int exfat_lookup(exfat_vol *v, const uint16_t *path, size_t len, exfat_node *out);
int exfat_find(exfat_vol *v, const exfat_node *dir, const uint16_t *name, size_t len, exfat_node *out);
/* *cookie starts at 0; EXFAT_END after the last entry */
int exfat_readdir(exfat_vol *v, const exfat_node *dir, uint64_t *cookie, exfat_node *out);

int exfat_read(exfat_vol *v, exfat_node *f, uint64_t off, void *buf, uint32_t len, uint32_t *got);
int exfat_write(exfat_vol *v, exfat_node *f, uint64_t off, const void *buf, uint32_t len, uint32_t *put);
int exfat_truncate(exfat_vol *v, exfat_node *f, uint64_t size);
int exfat_create(exfat_vol *v, const exfat_node *dir, const uint16_t *name, size_t len, uint16_t attr, exfat_node *out);
int exfat_remove(exfat_vol *v, exfat_node *n);
int exfat_rename(exfat_vol *v, exfat_node *n, const exfat_node *newdir, const uint16_t *name, size_t len);
/* writes attr and times of n back to its entry set */
int exfat_update(exfat_vol *v, exfat_node *n);
int exfat_dir_empty(exfat_vol *v, const exfat_node *dir);

/* the up-case table: for callers that compare names themselves */
uint16_t exfat_upcase(exfat_vol *v, uint16_t c);

#endif
