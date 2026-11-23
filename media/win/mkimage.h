/*
 *  KernelExEx - mkimage: builds the Windows 98 install stick from the user's own
 *  Windows 98 CD, in portable C (kexmedia.exe on Windows, tests on the host).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  The stick is a hybrid ISO (media/make_iso.sh, packed by media/pack_media.py
 *  into the "payload"): isolinux + MEMDISK for BIOS, ReBIOS/CSMWrap for UEFI, and
 *  KEXSETUP.IMG, a FAT16 hard-disk image with the CD's boot files, its WIN98
 *  folder and our DOS stage. The base ISO keeps an empty extent for KEXSETUP.IMG;
 *  mkimage builds the image straight into it and patches the file's size and the
 *  MEMDISK geometry. Nothing from Microsoft is shipped: it all comes from the CD.
 */
#ifndef MKIMAGE_H
#define MKIMAGE_H

#include <stddef.h>
#include <stdint.h>

/* the CD (or an .iso file): 2048-byte sectors */
struct mk_src
{
	int (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf); /* 0 = ok */
	void *ctx;
};

/* where the stick goes: a disk or a file; writes are whole 512-byte sectors */
struct mk_out
{
	int (*write)(void *ctx, uint64_t off, const void *buf, uint32_t len); /* 0 = ok */
	int (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);        /* for checking; may be NULL */
	void *ctx;
};

/* a file of the payload (appended to kexmedia.exe) */
struct mk_file
{
	char name[64];          /* "BASE", "STAGE/KEXINST/T_CONFIG.SYS", ... */
	const uint8_t *data;
	uint32_t size;
};

struct mk_job
{
	struct mk_src src;
	struct mk_out out;
	const struct mk_file *files;  /* the payload */
	int nfiles;
	uint16_t dos_date, dos_time;  /* time stamp of the files we create */
	void (*progress)(void *ctx, int percent, const char *what);
	void *pctx;
	uint64_t out_size;            /* filled in: bytes the stick needs */
	char error[256];              /* filled in when mk_build fails */
};

/* parse a payload (the bytes appended to kexmedia.exe, without the trailer check:
   pass the whole file; returns the number of files, 0 if there is none) */
int mk_payload(const uint8_t *buf, size_t len, struct mk_file *files, int max);

/* how many bytes the stick needs (the base ISO); 0 if the payload is bad */
uint64_t mk_needed(const struct mk_file *files, int nfiles);

/* check that the source is a Windows 98 CD: 0 = yes, else error[] says why */
int mk_check_source(struct mk_job *j);

/* build and write the whole stick (and read it back if out.read is set): 0 = ok */
int mk_build(struct mk_job *j);

#endif
