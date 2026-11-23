/* tests/kexpatch/memtest.c: the memory patch on a real VMM32.VXD.
   usage: memtest IN.VXD OUT.W3 - prints the state before/after, writes the patched W3 */
#include "../../tools/kexpatch/mempatch.h"
#include <stdio.h>
#include <stdlib.h>
static void lg(const char *what, int r) { printf("  %-40s %s\n", what, r == VP_APPLIED ? "applied" : r == VP_ALREADY ? "already" : "FAILED"); }
int main(int argc, char **argv)
{
	FILE *fp; long n; uint8_t *buf; struct vxdfile f; int s0, s1, rc;
	if (argc != 3 || !(fp = fopen(argv[1], "rb"))) return 2;
	fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
	buf = malloc((size_t) n); if (fread(buf, 1, (size_t) n, fp) != (size_t) n) return 2; fclose(fp);
	if (vf_load(&f, buf, (size_t) n)) return 1;
	s0 = mem_state(&f);
	rc = mem_apply(&f, lg);
	s1 = mem_state(&f);
	printf("state %d -> apply %d -> state %d\n", s0, rc, s1);
	fp = fopen(argv[2], "wb"); fwrite(f.data, 1, f.size, fp); fclose(fp);
	vf_free(&f); free(buf);
	return s0 == 0 && rc == 0 && s1 == 1 ? 0 : 1;
}
