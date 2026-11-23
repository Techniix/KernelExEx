/*
 *  KernelExEx - vxdfile: Windows 9x VxD containers (VMM32.VXD) in memory.
 *  W4 (DriveSpace-compressed) -> W3, the W3 directory, and LE objects inside,
 *  so patches can be applied to the user's own Windows files.
 *
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *  The W4 format and its DS decompression follow patcher9x by Jaroslav Hensl
 *  (MIT licence, https://github.com/JHRobotics/patcher9x); see vxdfile.c.
 *
 *  Portable C on buffers: host tests, the Win32 installer helper and (later)
 *  a DOS recovery tool use the same code.
 */
#ifndef VXDFILE_H
#define VXDFILE_H

#include <stddef.h>
#include <stdint.h>

enum { VF_OK = 0, VF_NOTMZ, VF_FORMAT, VF_DAMAGED, VF_NOMEM, VF_NOTFOUND };

/* A container as W3 in memory (a W4 input is decompressed into this). */
struct vxdfile
{
	uint8_t *data;      /* malloc'ed W3 image */
	size_t size;
	uint32_t w3;        /* offset of the "W3" header */
	int count;          /* VxDs in the directory */
	int was_w4;
};

/* One VxD's LE image inside the container, and one of its objects. */
struct le_obj
{
	uint32_t vsize;     /* virtual size */
	uint32_t flags;
	uint32_t first_page, npages;   /* 1-based page index into the page map */
};

struct vxd
{
	char name[9];
	uint32_t le;        /* offset of the LE header in vxdfile.data */
	uint32_t size;      /* bytes up to the next VxD (or the end) */
	uint32_t page_size, last_page, npages;
	uint32_t data_pages; /* file offset (in vxdfile.data) of page 1 */
	int nobj;
	struct le_obj obj[64];
};

/* load: W3 or W4 (decompressed); takes ownership of nothing (copies) */
int vf_load(struct vxdfile *f, const uint8_t *buf, size_t len);
void vf_free(struct vxdfile *f);
/* find a VxD by name ("VMM", "VCACHE"); 0 = ok */
int vf_find(const struct vxdfile *f, const char *name, struct vxd *v);
/* the file offset of byte `off` of object `obj` (1-based); 0 if outside the
   object's stored data (an object's tail may be zero-filled, not stored) */
uint32_t vf_obj_offset(const struct vxdfile *f, const struct vxd *v, int obj, uint32_t off);
/* bytes of object `obj` stored in the file (pages are contiguous in W3/LE files
   written by Microsoft's tools; checked) */
uint32_t vf_obj_stored(const struct vxdfile *f, const struct vxd *v, int obj);

/* A patch: find `find` (with `mask`: 0xFF bytes must match, 0x00 are "any")
   once in object `obj` of VxD `vxd` (0: in all its objects), then write `repl` at `at` bytes from the
   match. Exactly one match is required, so a patch never lands in the wrong
   build; if `repl` is found instead, the patch counts as already applied. */
struct vf_patch
{
	const char *vxd;
	int obj;
	const uint8_t *find, *mask;
	uint32_t find_len;
	int32_t at;
	const uint8_t *repl;
	uint32_t repl_len;
	const char *what;   /* for the log */
};

enum { VP_APPLIED = 0, VP_ALREADY, VP_NOMATCH, VP_AMBIGUOUS, VP_NOVXD };
/* *where = object << 24 | offset in the object */
int vf_patch(struct vxdfile *f, const struct vf_patch *p, uint32_t *where);
/* patch undo: the same patch with find/repl swapped is the undo */

#endif
