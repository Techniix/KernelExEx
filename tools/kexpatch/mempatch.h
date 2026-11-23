/* KernelExEx - mempatch.h: the more-than-1-GB patch for VMM (see mempatch.c). GPLv2. */
#ifndef MEMPATCH_H
#define MEMPATCH_H
#include "vxdfile.h"
extern const struct vf_patch mem_patches[];
extern const int mem_npatches;
int mem_apply(struct vxdfile *f, void (*log)(const char *what, int result));
int mem_state(struct vxdfile *f);
#endif
