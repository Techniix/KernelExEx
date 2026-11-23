/*
 *  KernelExEx - mempatch.c: the "more than 1 GB of RAM" patch for Windows 98 SE's
 *  VMM (inside VMM32.VXD). Patches are found by pattern, so they fit any language
 *  edition built from the same source; each is checked to match exactly once.
 *
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Credit and thanks: Rudolph R. "Rudy" Loew (1952-2019), whose PATCHMEM
 *  (Windows 9x RAM Limitation Patch, 2007-2017) first made Windows 9x run with
 *  more than 1 GB. His family published its source for others to build on; the
 *  patches marked [Loew] below follow PATCHMEM 7.2's Windows 98 tables (same
 *  changes, our own pattern definitions). See docs/research/memory.md and
 *  CREDITS.md; if his family asks, we will remove this part of the project.
 *
 *  What each patch fixes (found with QEMU's debugger, docs/research/memory.md):
 *  1. [ours] VMM's table of physical memory ranges has 16 entries and Windows'
 *     loader hands it one range per 64 MB XMS block, each separated from the
 *     next by a page lost to rounding: 16 x 64 MB = the 1 GB limit. The range
 *     builder now merges ranges that are 1 or 2 pages apart (the lost page is
 *     Windows' own XMS memory). PATCHMEM instead patches the real-mode loader
 *     to take XMS 3.0 blocks; ours leaves the loader alone.
 *  2. [Loew] Page-table records are set up from pool memory that is not zeroed;
 *     a stale "in the hash" bit made VMM look up records that were never
 *     added (crash above ~1260 MB). The setup now clears that bit.
 *  3. [Loew] VMM takes at most 0x834 pages for its start-up tables; their page
 *     database entries (13 bytes each) sit in VMM's init object, right before
 *     the byte array of the page-table record pool. Raised to 0xE90 pages, with
 *     the pool's byte array moved into new space after the object (its virtual
 *     size grows by 0x91E4) and the pool enlarged from 0x52D0 to 0x91D0
 *     records, its committed pages from 6 to 10 and its buffer from 0x6000 to
 *     0xA000 bytes. Needed above about 2 GB.
 *  4. [Loew] VCACHE's own ceiling on the disk cache, 800 MB, is lowered to
 *     512 MB, which the system arena can hold even in Safe Mode.
 */
#include "mempatch.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t merge_find[] = { 0x8b, 0x41, 0x04, 0x2b, 0x03, 0x83, 0xf8, 0xff, 0x75, 0x08,
                                      0x8b, 0x43, 0x04, 0x89, 0x41, 0x04 };
static const uint8_t merge_repl[] = { 0x8b, 0x03, 0x2b, 0x41, 0x04, 0x48, 0xd1, 0xe8, 0x75, 0x08 };

static const uint8_t flag1_find[] = { 0x8a, 0x48, 0x02, 0x80, 0x48, 0x03, 0xc0, 0x80, 0xe1, 0x81,
                                      0x80, 0xc9, 0x01, 0x88, 0x48, 0x02 };
static const uint8_t flag1_repl[] = { 0x01 };
static const uint8_t flag2_find[] = { 0x8a, 0x48, 0x02, 0x80, 0xe1, 0x84, 0x80, 0xc9, 0x04, 0x88, 0x48, 0x02 };
static const uint8_t flag2_repl[] = { 0x04 };

static const uint8_t cap_find[] = { 0x3d, 0x34, 0x08, 0x00, 0x00, 0x72, 0x05, 0xb8, 0x34, 0x08, 0x00, 0x00 };
static const uint8_t cap_repl[] = { 0x3d, 0x90, 0x0e, 0x00, 0x00, 0x72, 0x05, 0xb8, 0x90, 0x0e, 0x00, 0x00 };
static const uint8_t commit_find[] = { 0x68, 0x08, 0x00, 0x02, 0x20, 0x6a, 0x00, 0x6a, 0x04, 0x6a, 0x06, 0x8b };
static const uint8_t commit_repl[] = { 0x0a };
static const uint8_t buf_find[] = { 0xc7, 0x44, 0x24, 0x30, 0x00, 0x00, 0xa0, 0x00,
                                    0xc7, 0x44, 0x24, 0x2c, 0x00, 0x60, 0x00, 0x00 };
static const uint8_t buf_repl[] = { 0xa0 };
static const uint8_t vc_find[] = { 0xa1, 0, 0, 0, 0, 0x3d, 0x00, 0x20, 0x03, 0x00, 0x72, 0x17,
                                   0xb8, 0x00, 0x20, 0x03, 0x00, 0xa3 };
static const uint8_t vc_mask[] = { 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t vc_repl[] = { 0x00, 0x00, 0x02, 0x00, 0x72, 0x17, 0xb8, 0x00, 0x00, 0x02, 0x00 };

const struct vf_patch mem_patches[] = {
	{ "VMM", 0, merge_find, 0, sizeof(merge_find), 0, merge_repl, sizeof(merge_repl), "merge memory ranges" },
	{ "VMM", 0, flag1_find, 0, sizeof(flag1_find), 9, flag1_repl, 1, "clear stale record flag (1)" },
	{ "VMM", 0, flag2_find, 0, sizeof(flag2_find), 5, flag2_repl, 1, "clear stale record flag (2)" },
	{ "VMM", 0, cap_find, 0, sizeof(cap_find), 0, cap_repl, sizeof(cap_repl), "start-up pages 0x834 -> 0xE90" },
	{ "VMM", 0, commit_find, 0, sizeof(commit_find), 10, commit_repl, 1, "record pool: commit 10 pages" },
	{ "VMM", 0, buf_find, 0, sizeof(buf_find), 13, buf_repl, 1, "record pool: buffer 0xA000" },
	{ "VCACHE", 0, vc_find, vc_mask, sizeof(vc_find), 6, vc_repl, sizeof(vc_repl), "disk cache ceiling 512 MB" },
};
const int mem_npatches = sizeof(mem_patches) / sizeof(mem_patches[0]);

/* 3, the pool: "mov [esp+18h],52D0h; mov [esp+14h],52D0h; ...; push array; call init"
   becomes "...91D0h...; mov eax,array; jmp hook" and the hook, in the init object's
   last page right after its old end, is
       add eax, new-array - array ; push eax ; call init ; jmp back
   (mov eax keeps the push's fixup, so the array address stays relocatable). The
   new array (0x91D0 bytes, one per record) starts 20 bytes after the hook. */
#define POOL_OLD 0x52D0
#define POOL_NEW 0x91D0
#define POOL_GROW 0x91E4
#define HOOK_LEN 16
static int pool_match(const uint8_t *p, uint16_t n, uint8_t push, uint8_t call)
{
	static const uint8_t a[] = { 0xc7, 0x44, 0x24, 0x18 }, b[] = { 0xc7, 0x44, 0x24, 0x14 };
	return !memcmp(p, a, 4) && p[4] == (n & 0xff) && p[5] == n >> 8 && !p[6] && !p[7] && !memcmp(p + 8, b, 4) &&
	       p[12] == (n & 0xff) && p[13] == n >> 8 && !p[14] && !p[15] && p[52] == push && p[57] == call;
}
static int32_t rd32s(const uint8_t *p)
{
	return (int32_t) ((uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t x)
{
	p[0] = (uint8_t) x, p[1] = (uint8_t) (x >> 8), p[2] = (uint8_t) (x >> 16), p[3] = (uint8_t) (x >> 24);
}
/* apply = 0: only report. VP_APPLIED (or would apply), VP_ALREADY, or an error */
static int pool_patch(struct vxdfile *f, int apply)
{
	struct vxd v;
	int o, hits = 0, done = 0, obj = 0;
	uint32_t at = 0, i;
	if (vf_find(f, "VMM", &v) != VF_OK)
		return VP_NOVXD;
	for (o = 1; o <= v.nobj; o++)
	{
		uint32_t base = vf_obj_offset(f, &v, o, 0), len = vf_obj_stored(f, &v, o);
		if (!base || len < 62)
			continue;
		for (i = 0; i + 62 <= len; i++)
		{
			const uint8_t *p = f->data + base + i;
			if (pool_match(p, POOL_OLD, 0x68, 0xe8))
				hits++, obj = o, at = i;
			else if (pool_match(p, POOL_NEW, 0xb8, 0xe9))
				done++;
		}
	}
	if (done == 1 && !hits)
		return VP_ALREADY;
	if (hits != 1 || done)
		return hits > 1 ? VP_AMBIGUOUS : VP_NOMATCH;
	{
		uint32_t z = v.obj[obj - 1].vsize, y = at + 62, array, target;
		uint8_t *p = f->data + vf_obj_offset(f, &v, obj, 0) + at, *h;
		if (vf_obj_slack(f, &v, obj) < HOOK_LEN)
			return VP_NOMATCH;
		if (!apply)
			return VP_APPLIED;
		array = (uint32_t) rd32s(p + 53);
		target = y + (uint32_t) rd32s(p + 58);
		p[4] = p[12] = POOL_NEW & 0xff;
		p[5] = p[13] = POOL_NEW >> 8;
		p[52] = 0xb8;
		p[57] = 0xe9;
		wr32(p + 58, z - y);
		h = f->data + vf_obj_offset(f, &v, obj, 0) + z;   /* in the slack, so stored */
		h[0] = 0x05;
		wr32(h + 1, z + 20 - array);
		h[5] = 0x50;
		h[6] = 0xe8;
		wr32(h + 7, target - (z + 11));
		h[11] = 0xe9;
		wr32(h + 12, y - (z + 16));
		wr32(f->data + v.objtab + 24 * (obj - 1), z + POOL_GROW);
		return VP_APPLIED;
	}
}

/* patch a loaded container, all or nothing: 0 = every patch applied (or there already) */
int mem_apply(struct vxdfile *f, void (*log)(const char *what, int result))
{
	struct vxdfile copy = *f;
	int i, bad = 0;
	if (!(copy.data = malloc(f->size)))
		return -1;
	memcpy(copy.data, f->data, f->size);
	for (i = 0; i <= mem_npatches; i++)
	{
		int r = i < mem_npatches ? vf_patch(&copy, &mem_patches[i], 0) : pool_patch(&copy, 1);
		if (log)
			log(i < mem_npatches ? mem_patches[i].what : "record pool 0x52D0 -> 0x91D0 records", r);
		if (r != VP_APPLIED && r != VP_ALREADY)
			bad = 1;
	}
	if (!bad)
		memcpy(f->data, copy.data, f->size);
	free(copy.data);
	return bad;
}

/* is the patch in place? 1 = all of it, 0 = can be applied (none or some of it is
   there, e.g. an earlier, smaller version of this patch), -1 = not this VMM */
int mem_state(struct vxdfile *f)
{
	int i, done = 0, todo = 0;
	struct vxdfile copy = *f;
	if (!(copy.data = malloc(f->size)))
		return -1;
	memcpy(copy.data, f->data, f->size);
	for (i = 0; i <= mem_npatches; i++)
	{
		int r = i < mem_npatches ? vf_patch(&copy, &mem_patches[i], 0) : pool_patch(&copy, 0);
		if (r == VP_ALREADY)
			done++;
		else if (r == VP_APPLIED)
			todo++;
	}
	free(copy.data);
	return done == mem_npatches + 1 ? 1 : done + todo == mem_npatches + 1 ? 0 : -1;
}
