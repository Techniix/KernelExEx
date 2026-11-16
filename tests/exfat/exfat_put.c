/*
 *  KernelExEx - exfat_put: puts files and directories on an exFAT image
 *  through the portable core (fs/exfat), for preparing test disks.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  exfat_put IMAGE START_SECTOR mkdir PATH
 *  exfat_put IMAGE START_SECTOR put HOSTFILE PATH
 *  exfat_put IMAGE START_SECTOR ls PATH
 *
 *  PATH uses '\' or '/'; names are UTF-8 (BMP only). START_SECTOR is where
 *  the volume begins (2048 for a partition at 1 MiB, 0 for a bare volume).
 *
 *  cc -O1 -o exfat_put exfat_put.c ../../fs/exfat/exfat.c
 */
#include "../../fs/exfat/exfat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *img;
static unsigned long long base;

static int dread(void *ctx, uint64_t s, uint32_t n, void *buf)
{
	(void) ctx;
	if (fseeko(img, (off_t) ((base + s) * 512), SEEK_SET))
		return -1;
	return fread(buf, 512, n, img) == n ? 0 : -1;
}

static int dwrite(void *ctx, uint64_t s, uint32_t n, const void *buf)
{
	(void) ctx;
	if (fseeko(img, (off_t) ((base + s) * 512), SEEK_SET))
		return -1;
	return fwrite(buf, 512, n, img) == n ? 0 : -1;
}

static void *dalloc(void *ctx, size_t n) { (void) ctx; return calloc(1, n); }
static void dfree(void *ctx, void *p) { (void) ctx; free(p); }

static void clock_now(void *ctx, struct exfat_time *t)
{
	(void) ctx;
	/* a fixed time: 2026-11-16 08:00:00 */
	t->dos = ((uint32_t) ((2026 - 1980) << 9 | 11 << 5 | 16) << 16) | (8 << 11);
	t->ms10 = 0;
	t->utc_off = 0;
}

/* UTF-8 to UTF-16 (BMP) */
static size_t u16(uint16_t *out, const char *s)
{
	const unsigned char *p = (const unsigned char *) s;
	size_t n = 0;
	while (*p && n < 259)
	{
		unsigned c = *p++;
		if (c >= 0xE0 && p[0] && p[1])
		{
			c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u);
			p += 2;
		}
		else if (c >= 0xC0 && p[0])
			c = (c & 31) << 6 | (*p++ & 63u);
		out[n++] = (uint16_t) (c == '/' ? '\\' : c);
	}
	out[n] = 0;
	return n;
}

/* splits "a\b\c" into the directory "a\b" and the name "c" */
static int parent_and_name(exfat_vol *v, const uint16_t *w, size_t l, exfat_node *dir, const uint16_t **name, size_t *nl)
{
	size_t cut = l;
	while (cut && w[cut - 1] != '\\')
		cut--;
	*name = w + cut;
	*nl = l - cut;
	return exfat_lookup(v, w, cut ? cut - 1 : 0, dir);
}

int main(int argc, char **argv)
{
	struct exfat_dev dev = { NULL, 512, dread, dwrite, dalloc, dfree };
	exfat_vol *v;
	exfat_node dir, n;
	uint16_t w[260];
	const uint16_t *name;
	size_t l, nl;
	int r;
	if (argc < 5)
	{
		fprintf(stderr, "usage: exfat_put IMAGE START mkdir PATH | put HOSTFILE PATH | ls PATH\n");
		return 2;
	}
	img = fopen(argv[1], "r+b");
	base = strtoull(argv[2], NULL, 0);
	if (!img || exfat_mount(&v, &dev, strcmp(argv[3], "ls") == 0))
	{
		fprintf(stderr, "exfat_put: cannot mount %s at %s\n", argv[1], argv[2]);
		return 1;
	}
	exfat_set_clock(v, clock_now, NULL);
	if (!strcmp(argv[3], "ls"))
	{
		uint64_t cookie = 0;
		l = u16(w, argv[4]);
		r = exfat_lookup(v, w, l, &dir);
		while (!r && (r = exfat_readdir(v, &dir, &cookie, &n)) == EXFAT_OK)
		{
			int i;
			printf("%c %10llu  ", n.attr & EXFAT_ATTR_DIR ? 'd' : '-', (unsigned long long) n.size);
			for (i = 0; i < n.name_len; i++)
				putchar(n.name[i] < 128 ? n.name[i] : '?');
			putchar('\n');
		}
		r = r == EXFAT_END ? 0 : r;
	}
	else if (!strcmp(argv[3], "mkdir"))
	{
		l = u16(w, argv[4]);
		r = parent_and_name(v, w, l, &dir, &name, &nl);
		if (!r)
			r = exfat_create(v, &dir, name, nl, EXFAT_ATTR_DIR, &n);
	}
	else if (!strcmp(argv[3], "put") && argc >= 6)
	{
		FILE *src = fopen(argv[4], "rb");
		static unsigned char buf[65536];
		uint64_t off = 0;
		size_t got;
		l = u16(w, argv[5]);
		r = src ? parent_and_name(v, w, l, &dir, &name, &nl) : EXFAT_EIO;
		if (!r)
			r = exfat_create(v, &dir, name, nl, EXFAT_ATTR_ARCHIVE, &n);
		while (!r && src && (got = fread(buf, 1, sizeof buf, src)) > 0)
		{
			uint32_t put;
			r = exfat_write(v, &n, off, buf, (uint32_t) got, &put);
			off += put;
		}
		if (src)
			fclose(src);
	}
	else
		r = EXFAT_EINVAL;
	if (!r)
		r = exfat_flush(v);
	exfat_unmount(v);
	fclose(img);
	if (r)
		fprintf(stderr, "exfat_put: error %d\n", r);
	return r ? 1 : 0;
}
