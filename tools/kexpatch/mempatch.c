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
 *     Windows' own XMS memory).
 *  2. [Loew] Page-table records are set up from pool memory that is not zeroed;
 *     a stale "in the hash" bit made VMM look up records that were never
 *     added (crash above ~1260 MB). The setup now clears that bit.
 *  Not yet: VMM takes at most 0x834 pages (8.2 MB) for its start-up tables, and
 *     the page database alone needs more above ~2.5 GB. PATCHMEM raises that to
 *     0xE90, but with our merged ranges those pages would cross 16 MB (the first
 *     range now starts low and runs to the top) and Windows hung in testing. So
 *     this patch is for up to 2 GB (tested); 3-4 GB needs that solved first.
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

#if 0 /* see "Not yet" above */
static const uint8_t early_find[] = { 0x48, 0x3d, 0x34, 0x08, 0x00, 0x00, 0x72, 0x05, 0xb8, 0x34, 0x08, 0x00, 0x00 };
static const uint8_t early_repl[] = { 0x3d, 0x90, 0x0e, 0x00, 0x00, 0x72, 0x05, 0xb8, 0x90, 0x0e, 0x00, 0x00 };
#endif

const struct vf_patch mem_patches[] = {
	{ "VMM", 0, merge_find, 0, sizeof(merge_find), 0, merge_repl, sizeof(merge_repl), "merge memory ranges" },
	{ "VMM", 0, flag1_find, 0, sizeof(flag1_find), 9, flag1_repl, 1, "clear stale record flag (1)" },
	{ "VMM", 0, flag2_find, 0, sizeof(flag2_find), 5, flag2_repl, 1, "clear stale record flag (2)" },
};
const int mem_npatches = sizeof(mem_patches) / sizeof(mem_patches[0]);

/* patch a loaded container, all or nothing: 0 = every patch applied (or there already) */
int mem_apply(struct vxdfile *f, void (*log)(const char *what, int result))
{
	struct vxdfile copy = *f;
	int i, bad = 0;
	if (!(copy.data = malloc(f->size)))
		return -1;
	memcpy(copy.data, f->data, f->size);
	for (i = 0; i < mem_npatches; i++)
	{
		int r = vf_patch(&copy, &mem_patches[i], 0);
		if (log)
			log(mem_patches[i].what, r);
		if (r != VP_APPLIED && r != VP_ALREADY)
			bad = 1;
	}
	if (!bad)
		memcpy(f->data, copy.data, f->size);
	free(copy.data);
	return bad;
}

/* is the patch in place? 1 = all, 0 = none, -1 = partly or not this VMM */
int mem_state(struct vxdfile *f)
{
	int i, done = 0, todo = 0;
	struct vxdfile copy = *f;
	if (!(copy.data = malloc(f->size)))
		return -1;
	memcpy(copy.data, f->data, f->size);
	for (i = 0; i < mem_npatches; i++)
	{
		int r = vf_patch(&copy, &mem_patches[i], 0);
		if (r == VP_ALREADY)
			done++;
		else if (r == VP_APPLIED)
			todo++;
	}
	free(copy.data);
	return done == mem_npatches ? 1 : todo == mem_npatches ? 0 : -1;
}
