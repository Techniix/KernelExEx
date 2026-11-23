/*
 *  KernelExEx - mkimage.c: builds the Windows 98 install stick (see mkimage.h).
 *  Port of media/proto_image.py (KEXSETUP.IMG) plus the base ISO writer.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "mkimage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPT 63
#define HEADS 255
#define START 63                 /* the partition's first sector */
#define CYL (SPT * HEADS)
#define SLACK (48u * 1024 * 1024)
#define CHUNK (1024 * 1024)
#define FLOPPY_MAX 2949120

/* ------------------------------------------------------------------ helpers */

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t) rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static int fail(struct mk_job *j, const char *msg)
{
	snprintf(j->error, sizeof(j->error), "%s", msg);
	return -1;
}

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (upper((unsigned char) *a) != upper((unsigned char) *b))
			return 0;
	return *a == *b;
}

static int istarts(const char *s, const char *pre)
{
	for (; *pre; s++, pre++)
		if (upper((unsigned char) *s) != upper((unsigned char) *pre))
			return 0;
	return 1;
}

static uint32_t crc_table[256];

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
	if (!crc_table[1])
	{
		uint32_t i, k, c;
		for (i = 0; i < 256; i++)
		{
			for (c = i, k = 0; k < 8; k++)
				c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
	}
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* ------------------------------------------------------------------ payload */

int mk_payload(const uint8_t *buf, size_t len, struct mk_file *files, int max)
{
	const uint8_t *t, *p, *end;
	uint32_t count, plen, i;
	if (len < 16)
		return 0;
	t = buf + len - 16;
	if (memcmp(t, "KXPAY1\0\0", 8))
		return 0;
	count = rd32(t + 8);
	plen = rd32(t + 12);
	if (plen > len - 16 || (int) count > max)
		return 0;
	p = t - plen;
	end = t;
	for (i = 0; i < count; i++)
	{
		uint32_t nl, sz;
		if (end - p < 4)
			return 0;
		nl = rd32(p);
		p += 4;
		if (nl >= sizeof(files[i].name) || (uint32_t) (end - p) < nl + 4)
			return 0;
		memcpy(files[i].name, p, nl);
		files[i].name[nl] = 0;
		p += nl;
		sz = rd32(p);
		p += 4;
		if ((uint32_t) (end - p) < sz)
			return 0;
		files[i].data = p;
		files[i].size = sz;
		p += sz;
	}
	return (int) count;
}

static const struct mk_file *payload_file(const struct mk_file *f, int n, const char *name)
{
	int i;
	for (i = 0; i < n; i++)
		if (ieq(f[i].name, name))
			return &f[i];
	return NULL;
}

/* the base ISO ("BASE"): header, patches, then segments (data inline, or zeros) */
struct base
{
	uint32_t nseg, npatch;
	uint64_t total, ph_off, ph_cap;
	const uint8_t *patches, *segs, *end;
};

static int base_open(const struct mk_file *f, int n, struct base *b)
{
	const struct mk_file *bf = payload_file(f, n, "BASE");
	const uint8_t *p;
	uint32_t i;
	if (!bf || bf->size < 40 || memcmp(bf->data, "KXBASE1\0", 8))
		return -1;
	p = bf->data;
	b->nseg = rd32(p + 8);
	b->npatch = rd32(p + 12);
	b->total = rd64(p + 16);
	b->ph_off = rd64(p + 24);
	b->ph_cap = rd64(p + 32);
	b->end = p + bf->size;
	if ((uint64_t) b->npatch * 16 > bf->size - 40)
		return -1;
	b->patches = p + 40;
	b->segs = b->patches + b->npatch * 16;
	/* walk the segments once to check them */
	for (p = b->segs, i = 0; i < b->nseg; i++)
	{
		uint64_t len;
		if (b->end - p < 24)
			return -1;
		len = rd64(p + 8);
		if ((rd64(p) | len) & 511)
			return -1;
		p += 24;
		if (rd32(p - 8) == 0)
		{
			if ((uint64_t) (b->end - p) < len)
				return -1;
			p += len;
		}
	}
	if ((b->ph_off | b->ph_cap) & 511 || b->ph_off + b->ph_cap > b->total)
		return -1;
	return 0;
}

uint64_t mk_needed(const struct mk_file *files, int nfiles)
{
	struct base b;
	return base_open(files, nfiles, &b) ? 0 : b.total;
}

/* ------------------------------------------------------------------ output stream */

struct rec
{
	uint64_t off;
	uint32_t len, crc;
};

struct stream
{
	struct mk_job *j;
	uint8_t *buf;
	uint32_t fill;
	uint64_t off;          /* offset of buf[0] */
	struct rec *recs;
	int nrec, maxrec;
	uint64_t done, total;  /* for the progress bar */
	int err;
};

static void progress(struct mk_job *j, uint64_t done, uint64_t total, const char *what)
{
	if (j->progress)
		j->progress(j->pctx, total ? (int) (done * 100 / total) : 0, what);
}

static void flush(struct stream *s)
{
	uint32_t n = (s->fill + 511) & ~511u;
	if (!s->fill || s->err)
		return;
	memset(s->buf + s->fill, 0, n - s->fill);
	if (s->j->out.write(s->j->out.ctx, s->off, s->buf, n))
	{
		s->err = 1;
		fail(s->j, "Writing to the USB stick failed. Try another stick or USB port.");
		return;
	}
	if (s->nrec == s->maxrec)
	{
		struct rec *r = realloc(s->recs, sizeof(*r) * (s->maxrec = s->maxrec * 2 + 64));
		if (!r)
		{
			s->err = 1;
			fail(s->j, "Out of memory.");
			return;
		}
		s->recs = r;
	}
	s->recs[s->nrec].off = s->off;
	s->recs[s->nrec].len = n;
	s->recs[s->nrec].crc = crc32_buf(0, s->buf, n);
	s->nrec++;
	s->off += n;
	s->done += n;
	s->fill = 0;
	progress(s->j, s->done, s->total, "Writing");
}

static void seek_to(struct stream *s, uint64_t off)
{
	flush(s);
	s->off = off;
}

static void emit(struct stream *s, const void *data, size_t n)
{
	const uint8_t *p = data;
	while (n && !s->err)
	{
		uint32_t k = CHUNK - s->fill;
		if (k > n)
			k = (uint32_t) n;
		if (p)
		{
			memcpy(s->buf + s->fill, p, k);
			p += k;
		}
		else
			memset(s->buf + s->fill, 0, k);
		s->fill += k;
		n -= k;
		if (s->fill == CHUNK)
			flush(s);
	}
}

/* ------------------------------------------------------------------ the image's file tree */

struct node
{
	char name[13];          /* "NAME.EXT" */
	uint8_t attr;           /* FAT attributes; 0x10 = directory */
	int parent, child, last, next;
	uint32_t size;
	const uint8_t *mem;     /* data in memory, or */
	uint32_t lba;           /* on the CD */
	uint32_t clus, nclus;
};

struct tree
{
	struct node *n;
	int count, max;
	int *order;             /* nodes in cluster order */
	int norder;
	uint8_t *pool[16];      /* generated texts */
	int npool;
	uint8_t *fldata[64];    /* the boot floppy's files */
	int nfl;
};

static int add_node(struct tree *t, int parent, const char *name, uint8_t attr)
{
	struct node *n;
	if (t->count == t->max)
	{
		struct node *nn = realloc(t->n, sizeof(*nn) * (t->max = t->max * 2 + 64));
		if (!nn)
			return -1;
		t->n = nn;
	}
	n = &t->n[t->count];
	memset(n, 0, sizeof(*n));
	snprintf(n->name, sizeof(n->name), "%s", name);
	n->attr = attr;
	n->parent = parent;
	n->child = n->last = n->next = -1;
	if (parent >= 0)
	{
		if (t->n[parent].last >= 0)
			t->n[t->n[parent].last].next = t->count;
		else
			t->n[parent].child = t->count;
		t->n[parent].last = t->count;
	}
	return t->count++;
}

static int find_child(struct tree *t, int dir, const char *name)
{
	int c;
	for (c = t->n[dir].child; c >= 0; c = t->n[c].next)
		if (ieq(t->n[c].name, name))
			return c;
	return -1;
}

/* a valid 8.3 name? */
static int valid83(const char *s)
{
	const char *dot = strchr(s, '.');
	size_t base = dot ? (size_t) (dot - s) : strlen(s), ext = dot ? strlen(dot + 1) : 0;
	const char *p;
	if (!base || base > 8 || ext > 3 || (dot && strchr(dot + 1, '.')))
		return 0;
	for (p = s; *p; p++)
		if (*p != '.' && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		                   strchr("$%'-_@~`!(){}^#&", *p)))
			return 0;
	return 1;
}

static void name83(const char *s, uint8_t out[11])
{
	int i = 0;
	memset(out, ' ', 11);
	for (; *s && *s != '.' && i < 8; s++)
		out[i++] = (uint8_t) upper((unsigned char) *s);
	if (*s == '.')
		for (s++, i = 8; *s && i < 11; s++)
			out[i++] = (uint8_t) upper((unsigned char) *s);
}

/* ------------------------------------------------------------------ the CD */

static int cd_read(struct mk_job *j, uint32_t lba, uint32_t count, void *buf)
{
	if (j->src.read(j->src.ctx, lba, count, buf))
		return fail(j, "Reading the Windows 98 CD failed. Clean the disc, or use an .iso file.");
	return 0;
}

/* the directory record of NAME in the CD directory (lba, len), or -1 */
static int cd_find(struct mk_job *j, uint32_t lba, uint32_t len, const char *name, uint8_t rec[34])
{
	uint8_t sec[2048];
	uint32_t s;
	for (s = 0; s < (len + 2047) / 2048; s++)
	{
		uint32_t o = 0;
		if (cd_read(j, lba + s, 1, sec))
			return -1;
		while (o < 2048 && sec[o])
		{
			uint8_t rl = sec[o], nl = sec[o + 32];
			char nm[64];
			int k;
			if (rl < 34 || o + rl > 2048 || 33 + nl > rl)
				break;
			memcpy(nm, sec + o + 33, nl < 63 ? nl : 63);
			nm[nl < 63 ? nl : 63] = 0;
			for (k = 0; nm[k]; k++)
				if (nm[k] == ';')
					nm[k] = 0;
			k = (int) strlen(nm);
			if (k && nm[k - 1] == '.')
				nm[k - 1] = 0;
			if (nl > 1 || sec[o + 33] > 1)
				if (ieq(nm, name))
				{
					memcpy(rec, sec + o, 34);
					return 0;
				}
			o += rl;
		}
	}
	return -1;
}

static int cd_root(struct mk_job *j, uint8_t rec[34])
{
	uint8_t pvd[2048];
	if (cd_read(j, 16, 1, pvd))
		return -1;
	if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5))
		return fail(j, "This is not a CD image (no ISO 9660 file system).");
	memcpy(rec, pvd + 156, 34);
	return 0;
}

/* add the CD directory (lba, len) under dir, recursively */
static int cd_tree(struct mk_job *j, struct tree *t, int dir, uint32_t lba, uint32_t len, int depth)
{
	uint8_t sec[2048];
	uint32_t s;
	if (depth > 16)
		return fail(j, "The CD's folders are nested too deeply.");
	for (s = 0; s < (len + 2047) / 2048; s++)
	{
		uint32_t o = 0;
		if (cd_read(j, lba + s, 1, sec))
			return -1;
		while (o < 2048 && sec[o])
		{
			uint8_t rl = sec[o], nl = sec[o + 32], fl = sec[o + 25];
			char nm[64];
			int k, n;
			if (rl < 34 || o + rl > 2048 || 33 + nl > rl)
				return fail(j, "The CD's directory is damaged.");
			if (nl == 1 && sec[o + 33] <= 1)
			{
				o += rl;
				continue;
			}
			memcpy(nm, sec + o + 33, nl < 63 ? nl : 63);
			nm[nl < 63 ? nl : 63] = 0;
			for (k = 0; nm[k]; k++)
				if (nm[k] == ';')
					nm[k] = 0;
			k = (int) strlen(nm);
			if (k && nm[k - 1] == '.')
				nm[k - 1] = 0;
			if (!valid83(nm))
			{
				char m[128];
				snprintf(m, sizeof(m), "Unexpected file name on the CD: %s", nm);
				return fail(j, m);
			}
			if (fl & 0x80)
				return fail(j, "The CD has a file in several pieces (not a Windows 98 CD?).");
			n = add_node(t, dir, nm, (fl & 2) ? 0x10 : 0x20);
			if (n < 0)
				return fail(j, "Out of memory.");
			if (fl & 2)
			{
				if (cd_tree(j, t, n, rd32(sec + o + 2), rd32(sec + o + 10), depth + 1))
					return -1;
			}
			else
			{
				t->n[n].lba = rd32(sec + o + 2);
				t->n[n].size = rd32(sec + o + 10);
			}
			o += rl;
		}
	}
	return 0;
}

/* the CD's boot floppy (El Torito); returns its size */
static uint32_t cd_floppy(struct mk_job *j, uint8_t *fl)
{
	uint8_t sec[2048];
	uint32_t s, cat = 0, size;
	for (s = 17; s < 32; s++)
	{
		if (cd_read(j, s, 1, sec))
			return 0;
		if (sec[0] == 0xFF)
			break;
		if (sec[0] == 0 && !memcmp(sec + 1, "CD001", 5) && !memcmp(sec + 7, "EL TORITO SPECIFICATION", 23))
		{
			cat = rd32(sec + 71);
			break;
		}
	}
	if (!cat)
		return fail(j, "This CD cannot start a computer (no boot record): use a Windows 98 CD that can."), 0;
	if (cd_read(j, cat, 1, sec))
		return 0;
	if (sec[0] != 1 || sec[30] != 0x55 || sec[31] != 0xAA || sec[32] != 0x88)
		return fail(j, "The CD's boot catalog is not usable."), 0;
	switch (sec[33] & 0x0F)
	{
	case 1: size = 1228800; break;
	case 2: size = 1474560; break;
	case 3: size = 2949120; break;
	default: return fail(j, "The CD does not start from a floppy image (not a Windows 98 CD?)."), 0;
	}
	if (cd_read(j, rd32(sec + 40), size / 2048, fl))
		return 0;
	return size;
}

/* ------------------------------------------------------------------ the boot floppy (FAT12) */

struct flfile
{
	char name[13];
	uint8_t *data;
	uint32_t size;
};

static int fl_files(struct mk_job *j, const uint8_t *fl, uint32_t flsize, struct flfile *out, int max)
{
	uint32_t bps = rd16(fl + 11), spc = fl[13], rsv = rd16(fl + 14), nf = fl[16], ents = rd16(fl + 17),
	         fsz = rd16(fl + 22), root, data, i;
	int n = 0;
	if (bps != 512 || !spc || !nf || !fsz || fl[510] != 0x55 || fl[511] != 0xAA)
		return fail(j, "The CD's boot floppy image is not readable.");
	root = (rsv + nf * fsz) * bps;
	data = root + ents * 32;
	if (data > flsize)
		return fail(j, "The CD's boot floppy image is not readable.");
	for (i = 0; i < ents; i++)
	{
		const uint8_t *e = fl + root + i * 32;
		uint32_t cl, left, k;
		char nm[13];
		int p = 0;
		if (!e[0])
			break;
		if (e[0] == 0xE5 || (e[11] & 0x18) || e[11] == 0x0F)
			continue;
		for (k = 0; k < 8 && e[k] != ' '; k++)
			nm[p++] = (char) e[k];
		if (e[8] == ' ')
			continue;     /* names without an extension are not copied */
		nm[p++] = '.';
		for (k = 8; k < 11 && e[k] != ' '; k++)
			nm[p++] = (char) e[k];
		nm[p] = 0;
		if (n == max)
			return fail(j, "Too many files on the CD's boot floppy.");
		memcpy(out[n].name, nm, sizeof(nm));
		out[n].size = rd32(e + 28);
		out[n].data = malloc(out[n].size ? out[n].size : 1);
		if (!out[n].data)
			return fail(j, "Out of memory.");
		/* follow the FAT12 chain */
		for (cl = rd16(e + 26), left = out[n].size, p = 0; left; )
		{
			uint32_t off = data + (cl - 2) * spc * bps, k2 = spc * bps < left ? spc * bps : left, w;
			if (cl < 2 || cl >= 0xFF0 || off + k2 > flsize)
				return fail(j, "The CD's boot floppy image is damaged.");
			memcpy(out[n].data + p, fl + off, k2);
			p += (int) k2;
			left -= k2;
			w = rd16(fl + rsv * bps + cl + cl / 2);
			cl = cl & 1 ? w >> 4 : w & 0xFFF;
		}
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ text files */

struct lines
{
	char *l[128];
	int n;
};

static void split(const uint8_t *d, uint32_t n, struct lines *L, uint8_t *store)
{
	uint32_t i = 0, o = 0;
	L->n = 0;
	while (i < n && d[i] != 0x1A && L->n < 128)
	{
		char *line = (char *) store + o;
		while (i < n && d[i] != '\r' && d[i] != '\n' && d[i] != 0x1A)
			store[o++] = d[i++];
		store[o++] = 0;
		L->l[L->n++] = line;
		if (i < n && d[i] == '\r')
			i++;
		if (i < n && d[i] == '\n')
			i++;
	}
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = 0;
	return s;
}

static int lower_eq(const char *a, const char *b) { return ieq(a, b); }

/* text of lines joined with CR LF; kept in the tree's pool */
static int make_text(struct tree *t, struct lines *parts[], int nparts, const uint8_t **data, uint32_t *size)
{
	uint32_t n = 0, i;
	int p;
	uint8_t *buf;
	for (p = 0; p < nparts; p++)
		for (i = 0; i < (uint32_t) parts[p]->n; i++)
			n += (uint32_t) strlen(parts[p]->l[i]) + 2;
	if (t->npool == 16 || !(buf = malloc(n + 1)))
		return -1;
	t->pool[t->npool++] = buf;
	*data = buf;
	*size = n;
	for (p = 0; p < nparts; p++)
		for (i = 0; i < (uint32_t) parts[p]->n; i++)
		{
			size_t k = strlen(parts[p]->l[i]);
			memcpy(buf, parts[p]->l[i], k);
			buf += k;
			*buf++ = '\r';
			*buf++ = '\n';
		}
	return 0;
}

/* ------------------------------------------------------------------ building */

static const char *SKIP[] = { "JO.SYS", "AUTOEXEC.BAT", "CONFIG.SYS", "MSCDEX.EXE", "FINDCD.EXE", "OAKCDROM.SYS",
	"BTCDROM.SYS", "BTDOSM.SYS", "FLASHPT.SYS", "ASPI2DOS.SYS", "ASPI4DOS.SYS", "ASPI8DOS.SYS", "ASPI8U2.SYS",
	"ASPICD.SYS", "EMM386.EXE", "SETUP.EXE", "OEMSETUP.EXE", NULL };
static const char *CORE[] = { "IO.SYS", "MSDOS.SYS", "COMMAND.COM", "HIMEM.SYS", "EXTRACT.EXE", "FDISK.EXE",
	"DRVSPACE.BIN", NULL };

static int in_list(const char *n, const char **l)
{
	for (; *l; l++)
		if (ieq(n, *l))
			return 1;
	return 0;
}

static int fl_rank(const char *n) { return ieq(n, "IO.SYS") ? 0 : ieq(n, "MSDOS.SYS") ? 1 : 2; }

static int fl_cmp(const void *a, const void *b)
{
	const struct flfile *x = a, *y = b;
	int r = fl_rank(x->name) - fl_rank(y->name);
	return r ? r : strcmp(x->name, y->name);
}

static const struct mk_file *stage(struct mk_job *j, const char *rel)
{
	char nm[64];
	snprintf(nm, sizeof(nm), "STAGE/%s", rel);
	return payload_file(j->files, j->nfiles, nm);
}

/* put a STAGE/ file of the payload into the tree, making its folders */
static int add_stage(struct mk_job *j, struct tree *t, const struct mk_file *f)
{
	char path[64], *p, *slash;
	int dir = 0, n;
	snprintf(path, sizeof(path), "%s", f->name + 6);
	for (p = path; (slash = strchr(p, '/')); p = slash + 1)
	{
		*slash = 0;
		n = find_child(t, dir, p);
		if (n < 0 && (!valid83(p) || (n = add_node(t, dir, p, 0x10)) < 0))
			return fail(j, "Bad folder name in kexmedia's own files.");
		dir = n;
	}
	if (!valid83(p))
		return fail(j, "Bad file name in kexmedia's own files.");
	n = find_child(t, dir, p);
	if (n < 0 && (n = add_node(t, dir, p, 0x20)) < 0)
		return fail(j, "Out of memory.");
	t->n[n].mem = f->data;
	t->n[n].size = f->size;
	return 0;
}

static int set_text(struct mk_job *j, struct tree *t, const char *path, struct lines *parts[], int nparts)
{
	/* path is "NAME" or "DIR/NAME"; the file already exists (from the stage) or is made in the root */
	char d[16];
	const char *slash = strchr(path, '/');
	int dir = 0, n;
	if (slash)
	{
		snprintf(d, sizeof(d), "%.*s", (int) (slash - path), path);
		if ((dir = find_child(t, 0, d)) < 0)
			return fail(j, "kexmedia's own files are incomplete.");
		path = slash + 1;
	}
	n = find_child(t, dir, path);
	if (n < 0 && (n = add_node(t, dir, path, 0x20)) < 0)
		return fail(j, "Out of memory.");
	if (make_text(t, parts, nparts, &t->n[n].mem, &t->n[n].size))
		return fail(j, "Out of memory.");
	return 0;
}

static void lay_out(struct tree *t, int dir, uint32_t *next, uint32_t clbytes)
{
	int c;
	for (c = t->n[dir].child; c >= 0; c = t->n[c].next)
	{
		struct node *n = &t->n[c];
		if (n->attr & 0x10)
		{
			int k = 2, x;
			for (x = n->child; x >= 0; x = t->n[x].next)
				k++;
			n->size = (uint32_t) k * 32;
		}
		n->nclus = (n->size + clbytes - 1) / clbytes;
		if (n->attr & 0x10 && !n->nclus)
			n->nclus = 1;
		n->clus = n->nclus ? *next : 0;
		*next += n->nclus;
		if (n->nclus)
			t->order[t->norder++] = c;
		if (n->attr & 0x10)
			lay_out(t, c, next, clbytes);
	}
}

static void dirent(uint8_t *e, const char *name, uint8_t attr, uint32_t clus, uint32_t size, uint16_t date,
                   uint16_t time)
{
	memset(e, 0, 32);
	if (!strcmp(name, ".") || !strcmp(name, ".."))
	{
		memset(e, ' ', 11);
		memcpy(e, name, strlen(name));
	}
	else
		name83(name, e);
	e[11] = attr;
	wr16(e + 14, time);
	wr16(e + 16, date);
	wr16(e + 18, date);
	wr16(e + 22, time);
	wr16(e + 24, date);
	wr16(e + 26, clus);
	wr32(e + 28, (attr & 0x10) ? 0 : size);
}

/* a directory's entries (root: n = 0, with the volume label) */
static void dir_bytes(struct mk_job *j, struct tree *t, int d, uint8_t *buf)
{
	uint8_t *e = buf;
	int c;
	if (d == 0)
	{
		memset(e, 0, 32);
		memcpy(e, "KEXSETUP   ", 11);
		e[11] = 0x08;
		wr16(e + 22, j->dos_time);
		wr16(e + 24, j->dos_date);
		e += 32;
	}
	else
	{
		int p = t->n[d].parent;
		dirent(e, ".", 0x10, t->n[d].clus, 0, j->dos_date, j->dos_time);
		dirent(e + 32, "..", 0x10, p > 0 ? t->n[p].clus : 0, 0, j->dos_date, j->dos_time);
		e += 64;
	}
	for (c = t->n[d].child; c >= 0; c = t->n[c].next, e += 32)
		dirent(e, t->n[c].name, t->n[c].attr, t->n[c].clus, t->n[c].size, j->dos_date, j->dos_time);
}

static uint64_t tree_bytes(struct tree *t)
{
	uint64_t s = 0;
	int i;
	for (i = 0; i < t->count; i++)
		s += t->n[i].size;
	return s;
}

int mk_check_source(struct mk_job *j)
{
	uint8_t root[34], w98[34], cab[34];
	if (cd_root(j, root))
		return -1;
	if (cd_find(j, rd32(root + 2), rd32(root + 10), "WIN98", w98) || !(w98[25] & 2) ||
	    cd_find(j, rd32(w98 + 2), rd32(w98 + 10), "BASE4.CAB", cab))
		return fail(j, "This is not a Windows 98 CD (it has no WIN98 folder with BASE4.CAB).");
	return 0;
}

static int build_tree(struct mk_job *j, struct tree *t, uint8_t *floppy, uint8_t **vbr_src)
{
	static const char *const cfg_skip[] = { "FILES", "BUFFERS", "LASTDRIVE", "DOS=", "STACKS", NULL };
	static const char *const aut_skip[] = { "@ECHO", "ECHO", "PATH", "SET TEMP", "SET TMP", "CLS", NULL };
	struct flfile fl[64];
	struct lines common, prefix, ours[4], fcfg, faut;
	struct lines *parts[3];
	uint8_t *store[6] = { 0 };
	uint8_t root[34], w98[34];
	uint32_t flsize;
	int nfl = 0, i, k, w, kexinst, lang, sect_common = 0, rc = -1;
	static const char *const OURS[4] = { "CONFIG.SYS", "AUTOEXEC.BAT", "KEXINST/T_CONFIG.SYS", "KEXINST/T_AUTOEX.BAT" };

	if (mk_check_source(j))
		return -1;
	cd_root(j, root);
	cd_find(j, rd32(root + 2), rd32(root + 10), "WIN98", w98);
	progress(j, 0, 1, "Reading the CD's boot floppy");
	if (!(flsize = cd_floppy(j, floppy)))
		return -1;
	*vbr_src = floppy;
	if ((nfl = fl_files(j, floppy, flsize, fl, 64)) < 0)
		return -1;
	for (i = 0; i < nfl; i++)
		if (ieq(fl[i].name, "IO.SYS"))
			break;
	if (i == nfl)
	{
		fail(j, "The CD's boot floppy has no IO.SYS (not a Windows 98 CD?).");
		goto out;
	}

	/* the edition's language set-up (proto_image.py): CONFIG.SYS [COMMON], AUTOEXEC.BAT before the CD-ROM part */
	memset(&common, 0, sizeof(common));
	memset(&prefix, 0, sizeof(prefix));
	memset(&fcfg, 0, sizeof(fcfg));
	memset(&faut, 0, sizeof(faut));
	for (i = 0; i < nfl; i++)
	{
		struct lines *L = ieq(fl[i].name, "CONFIG.SYS") ? &fcfg : ieq(fl[i].name, "AUTOEXEC.BAT") ? &faut : NULL;
		if (!L)
			continue;
		if (!(store[L == &fcfg ? 0 : 1] = malloc(fl[i].size * 2 + 256)))
			goto oom;
		split(fl[i].data, fl[i].size, L, store[L == &fcfg ? 0 : 1]);
	}
	for (i = 0; i < fcfg.n; i++)
	{
		char *s = trim(fcfg.l[i]);
		const char *const *x;
		if (*s == '[')
		{
			sect_common = istarts(s, "[COMMON]") && strlen(s) == 8;
			continue;
		}
		if (!sect_common || !*s)
			continue;
		for (x = cfg_skip; *x && !istarts(s, *x); x++)
			;
		if (*x)
			continue;
		for (k = 0; k < common.n && !lower_eq(common.l[k], s); k++)
			;
		if (k == common.n && common.n < 128)
			common.l[common.n++] = s;
	}
	for (i = 0; i < faut.n; i++)
	{
		char *s = trim(faut.l[i]);
		const char *const *x;
		const char *m;
		int cdrom = istarts(s, "IF ");
		for (m = s; *m && !cdrom; m++)
			cdrom = istarts(m, "MSCDEX");
		if (cdrom)
			break;
		if (!*s)
			continue;
		for (x = aut_skip; *x && !istarts(s, *x); x++)
			;
		if (!*x && prefix.n < 128)
			prefix.l[prefix.n++] = s;
	}

	/* the root: label (in dir_bytes), the floppy's files (IO.SYS first), our stage, WIN98 */
	if (add_node(t, -1, "", 0x10) != 0)
		goto oom;
	for (i = 1; i < nfl; i++)          /* insertion sort (few files; no qsort in kexcrt) */
		for (k = i; k > 0 && fl_cmp(&fl[k - 1], &fl[k]) > 0; k--)
		{
			struct flfile x = fl[k];
			fl[k] = fl[k - 1];
			fl[k - 1] = x;
		}
	for (i = 0; i < nfl; i++)
	{
		int n;
		if (in_list(fl[i].name, SKIP))
			continue;
		if ((n = add_node(t, 0, fl[i].name, ieq(fl[i].name, "IO.SYS") || ieq(fl[i].name, "MSDOS.SYS") ? 0x27 : 0x20)) < 0)
			goto oom;
		t->n[n].mem = fl[i].data;
		t->n[n].size = fl[i].size;
	}
	for (i = 0; i < j->nfiles; i++)
		if (istarts(j->files[i].name, "STAGE/") && !ieq(j->files[i].name, "STAGE/CONFIG.SYS") &&
		    !ieq(j->files[i].name, "STAGE/AUTOEXEC.BAT"))
			if (add_stage(j, t, &j->files[i]))
				goto out;
	for (i = 0; i < 4; i++)
	{
		const struct mk_file *f = stage(j, OURS[i]);
		if (!f)
		{
			fail(j, "kexmedia's own files are incomplete (download it again).");
			goto out;
		}
		if (!(store[2 + i] = malloc(f->size * 2 + 256)))
			goto oom;
		split(f->data, f->size, &ours[i], store[2 + i]);
	}
	{
		struct lines first, rest;
		for (w = 0; w < 4; w++)
		{
			int front = (w & 1);   /* AUTOEXEC.BAT and T_AUTOEX.BAT: ours[0], then the floppy's part, then the rest */
			struct lines *add = w == 0 || w == 2 ? &common : w == 1 ? &prefix : NULL;
			if (front)
			{
				first.n = ours[w].n ? 1 : 0;
				first.l[0] = ours[w].l[0];
				rest.n = ours[w].n ? ours[w].n - 1 : 0;
				memcpy(rest.l, ours[w].l + 1, sizeof(char *) * (size_t) rest.n);
				parts[0] = &first;
				parts[1] = add ? add : &rest;
				parts[2] = &rest;
				k = add ? 3 : 2;
			}
			else
			{
				parts[0] = &ours[w];
				parts[1] = add;
				k = 2;
			}
			if (set_text(j, t, OURS[w], parts, k))
				goto out;
		}
	}
	/* the root's CONFIG.SYS and AUTOEXEC.BAT come after the other stage files (as proto_image.py) */
	if ((kexinst = find_child(t, 0, "KEXINST")) < 0)
	{
		fail(j, "kexmedia's own files are incomplete (download it again).");
		goto out;
	}
	if ((lang = add_node(t, kexinst, "LANG", 0x10)) < 0)
		goto oom;
	for (i = 0; i < nfl; i++)
	{
		int n;
		if (in_list(fl[i].name, SKIP) || in_list(fl[i].name, CORE))
			continue;
		if ((n = add_node(t, lang, fl[i].name, 0x20)) < 0)
			goto oom;
		t->n[n].mem = fl[i].data;
		t->n[n].size = fl[i].size;
	}
	if ((w = add_node(t, 0, "WIN98", 0x10)) < 0)
		goto oom;
	progress(j, 0, 1, "Reading the CD's WIN98 folder");
	if (cd_tree(j, t, w, rd32(w98 + 2), rd32(w98 + 10), 0))
		goto out;
	rc = 0;
	goto out;
oom:
	fail(j, "Out of memory.");
out:
	for (i = 0; i < 6; i++)
		free(store[i]);
	/* the floppy files' data belong to the tree from here on */
	for (i = 0; i < nfl; i++)
		t->fldata[t->nfl++] = fl[i].data;
	return rc;
}

struct plan
{
	uint32_t nsec, spc, fatsz, clusters, rsv, rootsec, clbytes, used;
};

static int plan_image(struct mk_job *j, struct tree *t, uint64_t ph_cap, struct plan *pl)
{
	uint64_t bytes = tree_bytes(t);
	uint32_t nsec, spc, fatsz, clusters, rsv = 8, rootsec = 32, next = 2;
	int nroot = 1, c;

	nsec = (uint32_t) ((((bytes + SLACK) / 512 + START + CYL - 1) / CYL) * CYL - START);
	if ((uint64_t) (nsec + START) * 512 > ph_cap)
		return fail(j, "This CD has more files than the stick image has room for.");
	/* cluster size as Windows chooses it for FAT16 */
	spc = nsec <= 262144 ? 4 : nsec <= 524288 ? 8 : nsec <= 1048576 ? 16 : nsec <= 2097152 ? 32 : 64;
	for (fatsz = 1;;)
	{
		uint32_t f;
		clusters = (nsec - rsv - 2 * fatsz - rootsec) / spc;
		f = ((clusters + 2) * 2 + 511) / 512;
		if (f <= fatsz)
			break;
		fatsz = f;
	}
	if (clusters < 4085 || clusters > 65524)
		return fail(j, "Internal error: bad FAT16 size.");
	t->order = malloc(sizeof(int) * (size_t) t->count);
	if (!t->order)
		return fail(j, "Out of memory.");
	lay_out(t, 0, &next, spc * 512);
	if (next - 2 > clusters)
		return fail(j, "This CD has more files than the stick image has room for.");
	for (c = t->n[0].child; c >= 0; c = t->n[c].next)
		nroot++;
	if (nroot > 512)
		return fail(j, "Too many files in the image's root folder.");
	pl->nsec = nsec;
	pl->spc = spc;
	pl->fatsz = fatsz;
	pl->clusters = clusters;
	pl->rsv = rsv;
	pl->rootsec = rootsec;
	pl->clbytes = spc * 512;
	pl->used = next - 2;
	return 0;
}

/* bytes write_image writes */
static uint64_t image_written(const struct plan *pl)
{
	return (uint64_t) (START + pl->rsv + 2 * pl->fatsz + pl->rootsec) * 512 + (uint64_t) pl->used * pl->clbytes;
}

static int write_image(struct mk_job *j, struct stream *s, struct tree *t, const uint8_t *floppy, uint64_t ph_off,
                       const struct plan *pl)
{
	uint32_t nsec = pl->nsec, spc = pl->spc, fatsz = pl->fatsz, rsv = pl->rsv, rootsec = pl->rootsec,
	         clbytes = pl->clbytes, i;
	uint8_t sec[512], *fat, *buf;
	int o;

	seek_to(s, ph_off);
	/* MBR: our boot code, the disk signature KXSU, one active FAT16 partition */
	{
		const struct mk_file *mbr = payload_file(j->files, j->nfiles, "MBR.BIN");
		uint32_t total = START + nsec, chs_s[2], k;
		if (!mbr || mbr->size < 440)
			return fail(j, "kexmedia's own files are incomplete (download it again).");
		memset(sec, 0, 512);
		memcpy(sec, mbr->data, 440);
		memcpy(sec + 0x1B8, "KXSU", 4);
		sec[0x1BE] = 0x80;
		chs_s[0] = START;
		chs_s[1] = total - 1;
		for (k = 0; k < 2; k++)
		{
			uint8_t *e = sec + 0x1BE + (k ? 5 : 1);
			uint32_t lba = chs_s[k], c = lba / CYL, h = (lba / SPT) % HEADS, sct = lba % SPT + 1;
			if (c > 1023)
			{
				e[0] = 254;
				e[1] = 0xFF;
				e[2] = 0xFF;
			}
			else
			{
				e[0] = (uint8_t) h;
				e[1] = (uint8_t) (sct | ((c >> 2) & 0xC0));
				e[2] = (uint8_t) c;
			}
		}
		sec[0x1BE + 4] = 0x06;
		wr32(sec + 0x1BE + 8, START);
		wr32(sec + 0x1BE + 12, nsec);
		sec[510] = 0x55;
		sec[511] = 0xAA;
		emit(s, sec, 512);
		emit(s, NULL, 512 * (START - 1));
	}
	/* the boot sector: the floppy's jump and boot code around our BPB */
	memset(sec, 0, 512);
	memcpy(sec, floppy, 3);
	memcpy(sec + 3, "MSWIN4.1", 8);
	wr16(sec + 11, 512);
	sec[13] = (uint8_t) spc;
	wr16(sec + 14, rsv);
	sec[16] = 2;
	wr16(sec + 17, rootsec * 16);
	wr16(sec + 19, nsec < 65536 ? nsec : 0);
	sec[21] = 0xF8;
	wr16(sec + 22, fatsz);
	wr16(sec + 24, SPT);
	wr16(sec + 26, HEADS);
	wr32(sec + 28, START);
	wr32(sec + 32, nsec < 65536 ? 0 : nsec);
	sec[36] = 0x80;
	sec[38] = 0x29;
	memcpy(sec + 39, "1XEK", 4);    /* volume ID 4B455831 */
	memcpy(sec + 43, "KEXSETUP   FAT16   ", 19);
	memcpy(sec + 0x3E, floppy + 0x3E, 0x1FE - 0x3E);
	sec[510] = 0x55;
	sec[511] = 0xAA;
	emit(s, sec, 512);
	emit(s, NULL, 512 * (rsv - 1));
	/* the FATs */
	if (!(fat = calloc(fatsz, 512)))
		return fail(j, "Out of memory.");
	wr16(fat, 0xFFF8);
	wr16(fat + 2, 0xFFFF);
	for (o = 0; o < t->norder; o++)
	{
		struct node *n = &t->n[t->order[o]];
		for (i = 0; i < n->nclus; i++)
			wr16(fat + 2 * (n->clus + i), i + 1 < n->nclus ? n->clus + i + 1 : 0xFFFF);
	}
	emit(s, fat, (size_t) fatsz * 512);
	emit(s, fat, (size_t) fatsz * 512);
	free(fat);
	/* the root folder, then every cluster in order */
	if (!(buf = calloc(1, clbytes > 64 * 1024 ? clbytes : 64 * 1024)))
		return fail(j, "Out of memory.");
	{
		uint8_t *rootbuf = calloc(rootsec, 512);
		if (!rootbuf)
		{
			free(buf);
			return fail(j, "Out of memory.");
		}
		dir_bytes(j, t, 0, rootbuf);
		emit(s, rootbuf, rootsec * 512);
		free(rootbuf);
	}
	for (o = 0; o < t->norder && !s->err; o++)
	{
		struct node *n = &t->n[t->order[o]];
		uint64_t pad = (uint64_t) n->nclus * clbytes - n->size;
		if (n->attr & 0x10)
		{
			uint8_t *d = calloc(n->nclus, clbytes);
			if (!d)
			{
				free(buf);
				return fail(j, "Out of memory.");
			}
			dir_bytes(j, t, t->order[o], d);
			emit(s, d, (size_t) n->nclus * clbytes);
			free(d);
			continue;
		}
		if (n->mem)
			emit(s, n->mem, n->size);
		else
		{
			uint32_t left = n->size, lba = n->lba;
			while (left && !s->err)
			{
				uint32_t secs = (left + 2047) / 2048, k;
				if (secs > 32)
					secs = 32;
				if (cd_read(j, lba, secs, buf))
				{
					free(buf);
					return -1;
				}
				k = secs * 2048 < left ? secs * 2048 : left;
				emit(s, buf, k);
				left -= k;
				lba += secs;
			}
		}
		emit(s, NULL, (size_t) pad);
	}
	free(buf);
	flush(s);
	return s->err ? -1 : 0;
}

/* the base ISO: every segment, with the image's size and geometry patched in */
static int emit_base(struct mk_job *j, struct stream *s, struct base *b, int after, uint64_t img_bytes)
{
	const uint8_t *p = b->segs;
	uint32_t i, k;
	for (i = 0; i < b->nseg && !s->err; i++)
	{
		uint64_t off = rd64(p), len = rd64(p + 8);
		uint32_t kind = rd32(p + 16);
		const uint8_t *data = p + 24;
		p += 24 + (kind == 0 ? len : 0);
		if ((off >= b->ph_off) != after)
			continue;
		seek_to(s, off);
		if (kind)
		{
			emit(s, NULL, (size_t) len);
			continue;
		}
		{
			uint8_t *copy = malloc((size_t) len);
			if (!copy)
				return fail(j, "Out of memory.");
			memcpy(copy, data, (size_t) len);
			for (k = 0; k < b->npatch; k++)
			{
				const uint8_t *pt = b->patches + k * 16;
				uint64_t po = rd64(pt);
				uint32_t type = rd32(pt + 8);
				uint8_t *q;
				if (po < off || po + 8 > off + len)
					continue;
				q = copy + (po - off);
				if (type == 1)          /* ISO directory record: data length, both byte orders */
				{
					uint32_t v = (uint32_t) img_bytes;
					wr32(q, v);
					q[4] = (uint8_t) (v >> 24);
					q[5] = (uint8_t) (v >> 16);
					q[6] = (uint8_t) (v >> 8);
					q[7] = (uint8_t) v;
				}
				else if (type == 2)     /* isolinux.cfg: "c=" and the cylinders, 8 characters */
				{
					char tok[16];
					snprintf(tok, sizeof(tok), "c=%-6u", (unsigned) (img_bytes / 512 / CYL));
					memcpy(q, tok, 8);
				}
			}
			emit(s, copy, (size_t) len);
			free(copy);
		}
	}
	flush(s);
	return s->err ? -1 : 0;
}

static int check(struct mk_job *j, struct stream *s)
{
	uint8_t *buf = s->buf;
	int i;
	uint64_t done = 0;
	for (i = 0; i < s->nrec; i++)
	{
		if (j->out.read(j->out.ctx, s->recs[i].off, buf, s->recs[i].len) ||
		    crc32_buf(0, buf, s->recs[i].len) != s->recs[i].crc)
		{
			char m[160];
			snprintf(m, sizeof(m), "The stick does not read back what was written (at %u MB): it may be faulty. "
			         "Try another one.", (unsigned) (s->recs[i].off >> 20));
			return fail(j, m);
		}
		done += s->recs[i].len;
		progress(j, done, s->done, "Checking");
	}
	return 0;
}

int mk_build(struct mk_job *j)
{
	struct base b;
	struct tree t;
	struct stream s;
	struct plan pl;
	uint8_t *floppy = NULL, *vbr;
	uint64_t img;
	const uint8_t *p;
	uint32_t i;
	int rc = -1, k;

	j->error[0] = 0;
	memset(&t, 0, sizeof(t));
	memset(&s, 0, sizeof(s));
	if (base_open(j->files, j->nfiles, &b))
		return fail(j, "kexmedia's own files are missing or damaged (download it again).");
	j->out_size = b.total;
	if (!(floppy = malloc(FLOPPY_MAX)) || !(s.buf = malloc(CHUNK)))
	{
		fail(j, "Out of memory.");
		goto out;
	}
	if (build_tree(j, &t, floppy, &vbr) || plan_image(j, &t, b.ph_cap, &pl))
		goto out;
	img = (uint64_t) (START + pl.nsec) * 512;
	/* the progress bar's 100%: the base's segments and the image */
	s.j = j;
	s.total = image_written(&pl);
	for (p = b.segs, i = 0; i < b.nseg; i++)
	{
		s.total += rd64(p + 8);
		p += 24 + (rd32(p + 16) == 0 ? rd64(p + 8) : 0);
	}
	/* the base up to the image, the image, the rest of the base */
	if (emit_base(j, &s, &b, 0, img) || write_image(j, &s, &t, floppy, b.ph_off, &pl) || emit_base(j, &s, &b, 1, img))
		goto out;
	if (j->out.read && check(j, &s))
		goto out;
	progress(j, 100, 100, "Done");
	rc = 0;
out:
	free(floppy);
	free(s.buf);
	free(s.recs);
	free(t.n);
	free(t.order);
	for (k = 0; k < t.npool; k++)
		free(t.pool[k]);
	for (k = 0; k < t.nfl; k++)
		free(t.fldata[k]);
	return rc;
}
