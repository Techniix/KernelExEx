/* tests/ramdisk/ramtest_host.c: ramcore on the build host. usage: ramtest_host OUT.IMG SECTORS
   - checks the chunk selection on sample BIOS maps, then writes a formatted volume */
#include "../../drivers/storage/ramdisk/ramcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define G (1ull << 30)
#define M (1ull << 20)
static int fails;
static void check(const char *what, unsigned got, unsigned want)
{
	printf("%-50s %u%s\n", what, got, got == want ? "" : " FAIL");
	if (got != want) fails++;
}
int main(int argc, char **argv)
{
	static rc_u32 ch[RC_MAX_CHUNKS];
	/* QEMU 6 GB: RAM 1 MB..3 GB, 4..7 GB */
	struct rc_e820 q[] = { { 0, 0x9fc00, 1 }, { 0x9fc00, 0x400, 2 }, { 0xf0000, 0x10000, 2 }, { 0x100000, 0xbfedf000, 1 },
	                       { 0xbffdf000, 0x21000, 2 }, { 0xfffc0000, 0x40000, 2 }, { 4 * G, 3 * G, 1 } };
	/* a laptop with 8 GB: an ACPI hole inside high RAM, odd alignment */
	struct rc_e820 l[] = { { 0x100000, 0xC7E00000, 1 }, { 0xC7F00000, 0x100000, 4 }, { 4 * G + 2 * M, 2 * G, 1 },
	                       { 4 * G + 512 * M, 4 * M, 2 } };
	struct rc_fat f;
	unsigned n;
	FILE *o;
	rc_u8 sec[512];
	rc_u32 s, secs;
	n = rc_chunks(q, 7, 2 * G, 1, ch, RC_MAX_CHUNKS);
	check("QEMU 6 GB, limit 2 GB: chunks", n, 255 + 768);   /* RAM ends at 0xBFFDF000 */
	n = rc_chunks(q, 7, 2 * G, 0, ch, RC_MAX_CHUNKS);
	check("same, no PSE-36", n, 255);
	n = rc_chunks(q, 7, 4095 * M, 1, ch, RC_MAX_CHUNKS);
	check("same, limit 4095 MB", n, 768);
	n = rc_chunks(l, 4, 3 * G, 1, ch, RC_MAX_CHUNKS);
	/* low: 3 GB..0xC7F00000 -> (0xC7F00000-0xC0000000)/4M = 31 (whole ones);
	   high: 4G+4M .. 6G (511 chunks) minus the one overlapping the reserved entry */
	check("laptop 8 GB, limit 3 GB", n, 31 + 511 - 1);
	if (argc < 3) return fails != 0;
	secs = (rc_u32) strtoul(argv[2], 0, 0);
	if (rc_fat_layout(secs, &f)) { puts("too small"); return 1; }
	printf("%s, %u sectors, fat %u, zero_end %u\n", f.fat32 ? "FAT32" : "FAT16", f.secs, f.fatsz, f.zero_end);
	o = fopen(argv[1], "wb");
	for (s = 0; s < f.zero_end; s++) { rc_fat_sector(&f, 0x4B455852, s, sec); fwrite(sec, 1, 512, o); }
	memset(sec, 0, 512);
	for (; s < f.secs; s++) fwrite(sec, 1, 512, o);   /* the data area: anything */
	fclose(o);
	return fails != 0;
}
