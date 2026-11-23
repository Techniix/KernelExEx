/* tests/kexpatch/vftest.c: VMM32.VXD W4 -> W3, VxD lookup, object data. usage: vftest VMM32.VXD [OUT.W3 [PATCHED.W3]] */
#include "../../tools/kexpatch/vxdfile.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	FILE *fp = fopen(argv[1], "rb");
	long n; uint8_t *buf; struct vxdfile f; struct vxd v; int rc, i;
	if (!fp) return 2;
	fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
	buf = malloc((size_t) n); if (fread(buf, 1, (size_t) n, fp) != (size_t) n) return 2; fclose(fp);
	rc = vf_load(&f, buf, (size_t) n);
	printf("load %d: %s, %zu bytes, %d VxDs\n", rc, f.was_w4 ? "W4" : "W3", f.size, f.count);
	if (rc) { free(buf); return 1; }
	if (argc > 2) { FILE *o = fopen(argv[2], "wb"); fwrite(f.data, 1, f.size, o); fclose(o); }
	for (i = 0; i < f.count; i++) {
		char nm[9]; int k; for (k = 0; k < 8; k++) nm[k] = (char) f.data[f.w3 + 16 + 16 * i + k]; nm[8] = 0;
		if (vf_find(&f, nm, &v) != VF_OK) { printf("bad %s\n", nm); vf_free(&f); free(buf); return 1; }
	}
	if (vf_find(&f, "VMM", &v) == VF_OK)
		printf("VMM: le %x, %d objects, obj1 %x bytes stored at %x\n", v.le, v.nobj, vf_obj_stored(&f, &v, 1), vf_obj_offset(&f, &v, 1, 0));
	{
		/* the range-merge test patch (docs/research/memory.md): apply, re-apply, check the file */
		static const uint8_t find[] = { 0x8b, 0x41, 0x04, 0x2b, 0x03, 0x83, 0xf8, 0xff, 0x75, 0x08, 0x8b, 0x43, 0x04, 0x89, 0x41, 0x04 };
		static const uint8_t repl[] = { 0x8b, 0x03, 0x2b, 0x41, 0x04, 0x48, 0xd1, 0xe8, 0x75, 0x08 };
		struct vf_patch pt = { "VMM", 0, find, NULL, sizeof(find), 0, repl, sizeof(repl), "merge" };
		uint32_t w = 0;
		int r1 = vf_patch(&f, &pt, &w), r2;
		printf("merge patch: %d at obj %u +%x\n", r1, w >> 24, w & 0xffffff);
		r2 = vf_patch(&f, &pt, &w);
		printf("again: %d (1 = already)\n", r2);
		if (r1 != VP_APPLIED || r2 != VP_ALREADY) { vf_free(&f); free(buf); return 1; }
		if (argc > 3) { FILE *o = fopen(argv[3], "wb"); fwrite(f.data, 1, f.size, o); fclose(o); }
	}
	vf_free(&f); free(buf);
	return 0;
}
