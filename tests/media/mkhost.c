/* tests/media/mkhost.c: mkimage on the host. usage: mkhost CD.ISO PAYLOAD OUT.IMG
   Builds the install stick as a file, as kexmedia.exe does on a USB stick. GPLv2. */
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700
#include "../../media/win/mkimage.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int src_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	ssize_t want = (ssize_t) count * 2048;
	return pread(*(int *) ctx, buf, (size_t) want, (off_t) lba * 2048) == want ? 0 : -1;
}

static int out_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
	if ((off | len) & 511)
		return fprintf(stderr, "unaligned write %llu %u\n", (unsigned long long) off, len), -1;
	return pwrite(*(int *) ctx, buf, len, (off_t) off) == (ssize_t) len ? 0 : -1;
}

static int out_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	return pread(*(int *) ctx, buf, len, (off_t) off) == (ssize_t) len ? 0 : -1;
}

static int last = -1;
static void prog(void *ctx, int pct, const char *what)
{
	(void) ctx;
	if (pct / 10 != last / 10 || pct == 100)
		fprintf(stderr, "%s %d%%\n", what, pct);
	last = pct;
}

int main(int argc, char **argv)
{
	struct mk_file files[64];
	struct mk_job j = { 0 };
	struct stat st;
	int in, out, pf, n;
	unsigned char *pay;
	if (argc != 4)
		return fprintf(stderr, "usage: mkhost CD.ISO PAYLOAD OUT.IMG\n"), 2;
	in = open(argv[1], O_RDONLY);
	pf = open(argv[2], O_RDONLY);
	if (in < 0 || pf < 0 || fstat(pf, &st))
		return perror("open"), 1;
	pay = malloc((size_t) st.st_size);
	if (read(pf, pay, (size_t) st.st_size) != st.st_size)
		return perror("read"), 1;
	n = mk_payload(pay, (size_t) st.st_size, files, 64);
	if (!n)
		return fprintf(stderr, "no payload\n"), 1;
	out = open(argv[3], O_RDWR | O_CREAT | O_TRUNC, 0644);
	j.src.read = src_read;
	j.src.ctx = &in;
	j.out.write = out_write;
	j.out.read = out_read;
	j.out.ctx = &out;
	j.files = files;
	j.nfiles = n;
	j.dos_date = (46 << 9) | (11 << 5) | 22;   /* 2026-11-22 */
	j.dos_time = 12 << 11;
	j.progress = prog;
	if (mk_check_source(&j))
		return fprintf(stderr, "source: %s\n", j.error), 1;
	if (mk_build(&j))
		return fprintf(stderr, "error: %s\n", j.error), 1;
	if (ftruncate(out, (off_t) j.out_size))
		return 1;
	fprintf(stderr, "%s: %llu MB\n", argv[3], (unsigned long long) (j.out_size >> 20));
	free(pay);
	return 0;
}
