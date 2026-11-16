/*
 * Host test for fs/exfat: works on an image made by mkfs.exfat, then the
 * image must pass fsck.exfat, and a second mount must read back every byte.
 *
 *   test_exfat IMAGE [seed]
 */
#include "../../fs/exfat/exfat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *img;
static unsigned sec_size = 512;
static unsigned long long n_reads, n_writes;

static int dread(void *ctx, uint64_t s, uint32_t n, void *buf)
{
	(void) ctx;
	n_reads++;
	if (fseeko(img, (off_t) (s * sec_size), SEEK_SET))
		return -1;
	return fread(buf, sec_size, n, img) == n ? 0 : -1;
}

static int dwrite(void *ctx, uint64_t s, uint32_t n, const void *buf)
{
	(void) ctx;
	n_writes++;
	if (fseeko(img, (off_t) (s * sec_size), SEEK_SET))
		return -1;
	return fwrite(buf, sec_size, n, img) == n ? 0 : -1;
}

static void *dalloc(void *ctx, size_t n) { (void) ctx; return calloc(1, n); }
static void dfree(void *ctx, void *p) { (void) ctx; free(p); }

static void clock_now(void *ctx, struct exfat_time *t)
{
	(void) ctx;
	/* 2026-11-15 21:30:20 */
	t->dos = ((uint32_t) ((2026 - 1980) << 9 | 11 << 5 | 15) << 16) | (21 << 11 | 30 << 5 | 10);
	t->ms10 = 50;
	t->utc_off = 0x80 | (uint8_t) (-20 & 0x7f);   /* UTC-5 */
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define OK(x) do { int _r = (x); if (_r != EXFAT_OK) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #x, _r); fails++; } } while (0)

static size_t u16(uint16_t *out, const char *s)
{
	size_t n = 0;
	while (*s)
		out[n++] = (uint8_t) *s++;
	out[n] = 0;
	return n;
}

static uint32_t rng = 1;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

/* file contents are a function of (file id, offset): checkable after remount */
static uint8_t pattern(int id, uint64_t off)
{
	return (uint8_t) ((id * 131 + off * 7 + (off >> 9) * 13) & 0xff);
}

struct tfile
{
	char path[200];
	int id;
	uint64_t size;
	int alive;
};

static struct tfile files[600];
static int nfiles;

static int lookup(exfat_vol *v, const char *path, exfat_node *n)
{
	uint16_t w[260];
	size_t l = u16(w, path);
	return exfat_lookup(v, w, l, n);
}

static int mkfile(exfat_vol *v, const char *dir, const char *name, uint16_t attr, exfat_node *out)
{
	exfat_node d;
	uint16_t w[260];
	size_t l;
	int r = lookup(v, dir, &d);
	if (r != EXFAT_OK)
		return r;
	l = u16(w, name);
	return exfat_create(v, &d, w, l, attr, out);
}

static void write_pattern(exfat_vol *v, exfat_node *n, int id, uint64_t from, uint64_t to, uint32_t chunk)
{
	static uint8_t buf[1 << 20];
	while (from < to)
	{
		uint32_t len = (uint32_t) (to - from < chunk ? to - from : chunk), put, i;
		for (i = 0; i < len; i++)
			buf[i] = pattern(id, from + i);
		OK(exfat_write(v, n, from, buf, len, &put));
		CHECK(put == len);
		from += len;
	}
}

static void verify(exfat_vol *v, struct tfile *f)
{
	static uint8_t buf[1 << 20];
	exfat_node n;
	uint64_t off = 0;
	int r = lookup(v, f->path, &n);
	if (r != EXFAT_OK)
	{
		printf("FAIL: %s not found (%d)\n", f->path, r);
		fails++;
		return;
	}
	if (n.size != f->size)
	{
		printf("FAIL: %s size %llu, expected %llu\n", f->path, (unsigned long long) n.size,
		       (unsigned long long) f->size);
		fails++;
		return;
	}
	while (off < f->size)
	{
		uint32_t want = (uint32_t) ((f->size - off) < 300000 ? (f->size - off) : 300000), got, i;
		OK(exfat_read(v, &n, off, buf, want, &got));
		CHECK(got == want);
		for (i = 0; i < got; i++)
			if (buf[i] != pattern(f->id, off + i))
			{
				printf("FAIL: %s byte %llu is %02x, expected %02x\n", f->path, (unsigned long long) (off + i), buf[i],
				       pattern(f->id, off + i));
				fails++;
				return;
			}
		off += got;
	}
}

int main(int argc, char **argv)
{
	struct exfat_dev dev = { NULL, 0, dread, dwrite, dalloc, dfree };
	exfat_vol *v;
	struct exfat_info info;
	exfat_node n, d, a, b;
	uint16_t w[300];
	int i, r;
	uint32_t free0;

	if (argc < 2)
		return 2;
	if (argc > 2)
		rng = (uint32_t) atoi(argv[2]);
	if (argc > 3)
		sec_size = (unsigned) atoi(argv[3]);
	dev.sector_size = sec_size;
	img = fopen(argv[1], "r+b");
	if (!img)
		return 2;

	OK(exfat_mount(&v, &dev, 0));
	if (fails)
		return 1;
	exfat_set_clock(v, clock_now, NULL);
	exfat_info(v, &info);
	free0 = info.free_clusters;
	printf("volume: %u-byte sectors, %u-byte clusters, %u clusters, %u free\n", info.sector_size, info.cluster_size,
	       info.clusters, info.free_clusters);

	/* directories, nested */
	OK(mkfile(v, "", "Games", EXFAT_ATTR_DIR, &n));
	OK(mkfile(v, "Games", "Quake II", EXFAT_ATTR_DIR, &n));
	OK(mkfile(v, "Games/Quake II", "baseq2", EXFAT_ATTR_DIR, &n));
	OK(mkfile(v, "", "Music", EXFAT_ATTR_DIR, &n));
	CHECK(mkfile(v, "", "games", EXFAT_ATTR_DIR, &n) == EXFAT_EEXIST);   /* case-insensitive */
	CHECK(mkfile(v, "", "bad:name", 0, &n) == EXFAT_EINVAL);

	/* files of awkward sizes */
	{
		uint64_t sizes[] = { 0, 1, 511, 512, 513, info.cluster_size - 1, info.cluster_size, info.cluster_size + 1,
		                     3 * (uint64_t) info.cluster_size + 77, 1000000, 2500000 };
		for (i = 0; i < (int) (sizeof(sizes) / sizeof(sizes[0])); i++)
		{
			struct tfile *f = &files[nfiles];
			snprintf(f->path, sizeof(f->path), "Games/Quake II/baseq2/pak%d with a rather long name.pak", i);
			f->id = nfiles + 1;
			f->size = sizes[i];
			f->alive = 1;
			OK(mkfile(v, "Games/Quake II/baseq2", strrchr(f->path, '/') + 1, EXFAT_ATTR_ARCHIVE, &n));
			write_pattern(v, &n, f->id, 0, f->size, 4096 + (rnd() % 70000));
			nfiles++;
		}
	}

	/* two files written in turns: their clusters interleave (FAT chains, not contiguous) */
	{
		exfat_node x, y;
		struct tfile *fx = &files[nfiles++], *fy = &files[nfiles++];
		uint64_t pos = 0;
		strcpy(fx->path, "Music/track01.wav");
		strcpy(fy->path, "Music/track02.wav");
		fx->id = 101;
		fy->id = 102;
		fx->alive = fy->alive = 1;
		OK(mkfile(v, "Music", "track01.wav", 0, &x));
		OK(mkfile(v, "Music", "track02.wav", 0, &y));
		for (i = 0; i < 12; i++)
		{
			uint32_t len = info.cluster_size + (rnd() % info.cluster_size);
			write_pattern(v, &x, fx->id, pos, pos + len, len);
			write_pattern(v, &y, fy->id, pos, pos + len, len);
			pos += len;
		}
		fx->size = fy->size = pos;
		CHECK(!x.nofat && !y.nofat);
	}

	/* many files in one directory: it has to grow past its first cluster */
	OK(mkfile(v, "", "Many", EXFAT_ATTR_DIR, &n));
	for (i = 0; i < 300; i++)
	{
		struct tfile *f = &files[nfiles++];
		snprintf(f->path, sizeof(f->path), "Many/file number %03d - %s.txt", i,
		         (i % 3) ? "short" : "with a name long enough to need several name entries in its set");
		f->id = 1000 + i;
		f->size = rnd() % 3000;
		f->alive = 1;
		OK(mkfile(v, "Many", strrchr(f->path, '/') + 1, 0, &n));
		write_pattern(v, &n, f->id, 0, f->size, 1000);
	}

	/* delete every fourth, truncate every fifth (some longer, some shorter) */
	for (i = 0; i < nfiles; i++)
	{
		struct tfile *f = &files[i];
		if (strncmp(f->path, "Many/", 5))
			continue;
		if (i % 4 == 0)
		{
			OK(lookup(v, f->path, &n));
			OK(exfat_remove(v, &n));
			f->alive = 0;
		}
		else if (i % 5 == 0)
		{
			uint64_t ns = rnd() % 5000;
			OK(lookup(v, f->path, &n));
			if (ns > f->size)
			{
				/* grow by writing (truncate up would leave zeros, not the pattern) */
				write_pattern(v, &n, f->id, f->size, ns, 700);
			}
			else
				OK(exfat_truncate(v, &n, ns));
			f->size = ns;
		}
	}

	/* rename in place (case change), and move to another directory */
	OK(lookup(v, "Music/track01.wav", &n));
	OK(lookup(v, "Music", &d));
	u16(w, "TRACK01.WAV");
	OK(exfat_rename(v, &n, &d, w, 11));
	strcpy(files[11].path, "Music/TRACK01.WAV");
	OK(lookup(v, "Music/track02.wav", &n));
	OK(lookup(v, "Games", &d));
	u16(w, "moved track.wav");
	OK(exfat_rename(v, &n, &d, w, 15));
	strcpy(files[12].path, "Games/moved track.wav");
	/* a directory with contents moves, and its contents with it */
	OK(lookup(v, "Games/Quake II", &n));
	OK(lookup(v, "Music", &d));
	u16(w, "Quake 2");
	OK(exfat_rename(v, &n, &d, w, 7));
	for (i = 0; i < nfiles; i++)
		if (!strncmp(files[i].path, "Games/Quake II/", 15))
		{
			char tmp[200];
			snprintf(tmp, sizeof(tmp), "Music/Quake 2/%s", files[i].path + 15);
			strcpy(files[i].path, tmp);
		}
	/* removing a directory that is not empty fails; an empty one goes */
	OK(lookup(v, "Music", &n));
	CHECK(exfat_remove(v, &n) == EXFAT_ENOTEMPTY);
	OK(mkfile(v, "", "Empty", EXFAT_ATTR_DIR, &n));
	OK(exfat_remove(v, &n));
	CHECK(lookup(v, "Empty", &n) == EXFAT_ENOENT);

	/* listing the root */
	{
		uint64_t cookie = 0;
		int count = 0;
		exfat_node root;
		exfat_root(v, &root);
		while ((r = exfat_readdir(v, &root, &cookie, &n)) == EXFAT_OK)
			count++;
		CHECK(r == EXFAT_END);
		CHECK(count == 3);   /* Games, Music, Many */
	}
	(void) a;
	(void) b;
	for (i = 0; i < nfiles; i++)
		if (files[i].alive)
			verify(v, &files[i]);
	exfat_info(v, &info);
	printf("written; %u clusters free (were %u)\n", info.free_clusters, free0);
	exfat_unmount(v);

	/* a second mount reads everything back */
	OK(exfat_mount(&v, &dev, 1));
	if (!fails)
	{
		for (i = 0; i < nfiles; i++)
			if (files[i].alive)
				verify(v, &files[i]);
			else
				CHECK(lookup(v, files[i].path, &n) == EXFAT_ENOENT);
		exfat_info(v, &info);
		exfat_unmount(v);
	}
	fclose(img);
	printf("%d files, %llu sector reads, %llu writes: %s\n", nfiles, n_reads, n_writes, fails ? "FAILED" : "all passed");
	return fails != 0;
}
