/*
 *  KernelExEx - exFAT file system core. See exfat.h.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 */
#include "exfat.h"

#include <string.h>

#define FAT_EOC 0xFFFFFFFFu
#define FAT_BAD 0xFFFFFFF7u
#define ENT_SIZE 32

#define T_EOD 0x00
#define T_BITMAP 0x81
#define T_UPCASE 0x82
#define T_LABEL 0x83
#define T_FILE 0x85
#define T_STREAM 0xC0
#define T_NAME 0xC1
#define IN_USE 0x80

struct exfat_vol
{
	struct exfat_dev dev;
	int ro;
	uint32_t sec_shift, spc_shift;
	uint32_t sec_size, cl_size;
	uint32_t fat_off, fat_len, heap_off;
	uint32_t clusters, root_cluster, serial;
	uint8_t nfats, active_fat;
	uint16_t vol_flags;

	uint8_t *bitmap;            /* one bit per cluster, in memory */
	uint32_t bitmap_cluster;    /* where it lives on disk */
	uint64_t bitmap_len;
	uint8_t *bitmap_dirty;      /* one byte per bitmap sector */
	uint32_t free_clusters;
	uint32_t alloc_hint;

	uint16_t *upcase;           /* 65536 entries */
	uint16_t label[12];
	uint8_t label_len;

	uint8_t *fat_buf;           /* one FAT sector */
	uint64_t fat_sec;           /* which; ~0 = none */
	int fat_dirty;

	uint8_t *sec_buf;           /* directory sector */
	uint64_t sec_no;
	int dirty_flag_set;         /* VolumeDirty written on the disk */

	void (*now)(void *ctx, struct exfat_time *t);
	void *now_ctx;
};

/* ---- little-endian access ---------------------------------------------------------- */
static uint16_t g16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint64_t g64(const uint8_t *p) { return g32(p) | (uint64_t) g32(p + 4) << 32; }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t) v); p32(p + 4, (uint32_t) (v >> 32)); }

/* ---- sector I/O ------------------------------------------------------------------------ */
static int rd(exfat_vol *v, uint64_t sec, uint32_t n, void *buf)
{
	return v->dev.read(v->dev.ctx, sec, n, buf) ? EXFAT_EIO : EXFAT_OK;
}

static int wr(exfat_vol *v, uint64_t sec, uint32_t n, const void *buf)
{
	if (v->ro)
		return EXFAT_EROFS;
	return v->dev.write(v->dev.ctx, sec, n, buf) ? EXFAT_EIO : EXFAT_OK;
}

static uint64_t cl_sector(exfat_vol *v, uint32_t cl)
{
	return v->heap_off + ((uint64_t) (cl - 2) << v->spc_shift);
}

static int valid_cl(exfat_vol *v, uint32_t cl)
{
	return cl >= 2 && cl < v->clusters + 2;
}

/* the volume is being changed: VolumeDirty on, once (it is outside the boot checksum) */
static int mark_dirty(exfat_vol *v)
{
	uint8_t *b;
	int r;
	if (v->ro)
		return EXFAT_EROFS;
	if (v->dirty_flag_set)
		return EXFAT_OK;
	b = v->sec_buf;
	v->sec_no = ~0ull;
	if ((r = rd(v, 0, 1, b)) != EXFAT_OK)
		return r;
	p16(b + 106, (uint16_t) (g16(b + 106) | 2));
	if ((r = wr(v, 0, 1, b)) != EXFAT_OK)
		return r;
	v->dirty_flag_set = 1;
	return EXFAT_OK;
}

/* ---- FAT -------------------------------------------------------------------------------- */
static int fat_flush(exfat_vol *v)
{
	int r = EXFAT_OK;
	if (v->fat_dirty)
	{
		r = wr(v, v->fat_sec, 1, v->fat_buf);
		if (r == EXFAT_OK && v->nfats == 2)   /* TexFAT media: keep the other FAT the same */
		{
			uint64_t other = v->fat_sec + (v->active_fat ? -(int64_t) v->fat_len : (int64_t) v->fat_len);
			r = wr(v, other, 1, v->fat_buf);
		}
		v->fat_dirty = 0;
	}
	return r;
}

static int fat_load(exfat_vol *v, uint32_t cl, uint32_t *idx)
{
	uint64_t byte = (uint64_t) cl * 4;
	uint64_t sec = v->fat_off + (uint64_t) v->active_fat * v->fat_len + (byte >> v->sec_shift);
	int r;
	if (sec != v->fat_sec)
	{
		if ((r = fat_flush(v)) != EXFAT_OK)
			return r;
		if ((r = rd(v, sec, 1, v->fat_buf)) != EXFAT_OK)
		{
			v->fat_sec = ~0ull;
			return r;
		}
		v->fat_sec = sec;
	}
	*idx = (uint32_t) (byte & (v->sec_size - 1));
	return EXFAT_OK;
}

static int fat_get(exfat_vol *v, uint32_t cl, uint32_t *next)
{
	uint32_t i;
	int r = fat_load(v, cl, &i);
	if (r == EXFAT_OK)
		*next = g32(v->fat_buf + i);
	return r;
}

static int fat_set(exfat_vol *v, uint32_t cl, uint32_t val)
{
	uint32_t i;
	int r = fat_load(v, cl, &i);
	if (r == EXFAT_OK)
	{
		p32(v->fat_buf + i, val);
		v->fat_dirty = 1;
	}
	return r;
}

/* ---- allocation bitmap ---------------------------------------------------------------- */
static int __attribute__((unused)) bm_get(exfat_vol *v, uint32_t cl)
{
	uint32_t i = cl - 2;
	return (v->bitmap[i >> 3] >> (i & 7)) & 1;
}

static void bm_set(exfat_vol *v, uint32_t cl, int on)
{
	uint32_t i = cl - 2;
	uint8_t m = (uint8_t) (1u << (i & 7));
	if (on)
	{
		if (!(v->bitmap[i >> 3] & m))
			v->free_clusters--;
		v->bitmap[i >> 3] |= m;
	}
	else
	{
		if (v->bitmap[i >> 3] & m)
			v->free_clusters++;
		v->bitmap[i >> 3] &= (uint8_t) ~m;
	}
	v->bitmap_dirty[(i >> 3) >> v->sec_shift] = 1;
}

static int bm_flush(exfat_vol *v)
{
	uint64_t nsec = (v->bitmap_len + v->sec_size - 1) >> v->sec_shift, s;
	uint32_t spc = 1u << v->spc_shift;
	int r;
	for (s = 0; s < nsec; s++)
	{
		uint32_t cl, k;
		uint8_t *tmp;
		uint64_t off, left;
		if (!v->bitmap_dirty[s])
			continue;
		/* the bitmap's own clusters: a FAT chain from bitmap_cluster */
		cl = v->bitmap_cluster;
		for (k = 0; k < (uint32_t) (s >> v->spc_shift); k++)
			if ((r = fat_get(v, cl, &cl)) != EXFAT_OK)
				return r;
		if (!valid_cl(v, cl))
			return EXFAT_ECORRUPT;
		off = s << v->sec_shift;
		left = v->bitmap_len - off;
		if (left >= v->sec_size)
			r = wr(v, cl_sector(v, cl) + (s & (spc - 1)), 1, v->bitmap + off);
		else
		{
			tmp = v->sec_buf;
			v->sec_no = ~0ull;
			memset(tmp, 0, v->sec_size);
			memcpy(tmp, v->bitmap + off, (size_t) left);
			r = wr(v, cl_sector(v, cl) + (s & (spc - 1)), 1, tmp);
		}
		if (r != EXFAT_OK)
			return r;
		v->bitmap_dirty[s] = 0;
	}
	return EXFAT_OK;
}

/* a free cluster, at or after hint first */
static int alloc_one(exfat_vol *v, uint32_t hint, uint32_t *out)
{
	uint32_t i, n = v->clusters, start;
	if (!v->free_clusters)
		return EXFAT_ENOSPC;
	start = (hint >= 2 && hint < n + 2) ? hint - 2 : 0;
	for (i = 0; i < n; i++)
	{
		uint32_t c = (start + i) % n;
		if (v->bitmap[c >> 3] == 0xFF && (c & 7) == 0 && i + 8 <= n)
		{
			i += 7;
			continue;
		}
		if (!((v->bitmap[c >> 3] >> (c & 7)) & 1))
		{
			*out = c + 2;
			bm_set(v, c + 2, 1);
			v->alloc_hint = c + 3;
			return EXFAT_OK;
		}
	}
	return EXFAT_ENOSPC;
}

static int zero_cluster(exfat_vol *v, uint32_t cl)
{
	uint32_t s, spc = 1u << v->spc_shift;
	int r;
	memset(v->sec_buf, 0, v->sec_size);
	v->sec_no = ~0ull;
	for (s = 0; s < spc; s++)
		if ((r = wr(v, cl_sector(v, cl) + s, 1, v->sec_buf)) != EXFAT_OK)
			return r;
	return EXFAT_OK;
}

/* ---- up-case table ------------------------------------------------------------------- */
uint16_t exfat_upcase(exfat_vol *v, uint16_t c)
{
	return v->upcase ? v->upcase[c] : (c >= 'a' && c <= 'z' ? (uint16_t) (c - 32) : c);
}

static int name_eq(exfat_vol *v, const uint16_t *a, size_t al, const uint16_t *b, size_t bl)
{
	size_t i;
	if (al != bl)
		return 0;
	for (i = 0; i < al; i++)
		if (exfat_upcase(v, a[i]) != exfat_upcase(v, b[i]))
			return 0;
	return 1;
}

static uint16_t name_hash(exfat_vol *v, const uint16_t *n, size_t len)
{
	uint16_t h = 0;
	size_t i;
	for (i = 0; i < len; i++)
	{
		uint16_t c = exfat_upcase(v, n[i]);
		h = (uint16_t) (((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
		h = (uint16_t) (((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
	}
	return h;
}

/* ---- reading a cluster chain by byte offset ------------------------------------------ */
struct chain
{
	uint32_t first;
	uint8_t nofat;
	uint64_t size;   /* bytes the chain holds; ~0 = follow the FAT to its end */
	uint32_t cur_index, cur_cluster;
};

/* the cluster holding byte off of the chain */
static int chain_cluster(exfat_vol *v, struct chain *c, uint64_t off, uint32_t *cl)
{
	uint32_t idx = (uint32_t) (off >> (v->sec_shift + v->spc_shift)), k, x;
	int r;
	if (!valid_cl(v, c->first))
		return EXFAT_ECORRUPT;
	if (c->nofat)
	{
		*cl = c->first + idx;
		return valid_cl(v, *cl) ? EXFAT_OK : EXFAT_ECORRUPT;
	}
	if (c->cur_cluster && c->cur_index <= idx)
	{
		k = c->cur_index;
		x = c->cur_cluster;
	}
	else
	{
		k = 0;
		x = c->first;
	}
	for (; k < idx; k++)
	{
		if ((r = fat_get(v, x, &x)) != EXFAT_OK)
			return r;
		if (!valid_cl(v, x))
			return EXFAT_ECORRUPT;
	}
	c->cur_index = idx;
	c->cur_cluster = x;
	*cl = x;
	return EXFAT_OK;
}

static int chain_sector(exfat_vol *v, struct chain *c, uint64_t off, uint64_t *sec)
{
	uint32_t cl;
	int r = chain_cluster(v, c, off, &cl);
	if (r == EXFAT_OK)
		*sec = cl_sector(v, cl) + ((off >> v->sec_shift) & ((1u << v->spc_shift) - 1));
	return r;
}

/* ---- directory entries -------------------------------------------------------------- */
static void dir_chain(const exfat_node *d, struct chain *c, exfat_vol *v)
{
	(void) v;
	c->first = d->first_cluster;
	c->nofat = d->nofat;
	c->size = d->is_root ? ~0ull : d->size;
	c->cur_index = 0;
	c->cur_cluster = 0;
}

static int dir_size(exfat_vol *v, const exfat_node *d, uint64_t *size)
{
	uint32_t cl, n = 1;
	int r;
	if (!d->is_root)
	{
		*size = d->size;
		return EXFAT_OK;
	}
	cl = d->first_cluster;
	for (;;)
	{
		uint32_t nx;
		if ((r = fat_get(v, cl, &nx)) != EXFAT_OK)
			return r;
		if (nx == FAT_EOC)
			break;
		if (!valid_cl(v, nx) || n > v->clusters)
			return EXFAT_ECORRUPT;
		cl = nx;
		n++;
	}
	*size = (uint64_t) n << (v->sec_shift + v->spc_shift);
	return EXFAT_OK;
}

/* entry at byte offset off of directory chain c: copied to out */
static int ent_read(exfat_vol *v, struct chain *c, uint64_t off, uint8_t *out)
{
	uint64_t sec;
	int r = chain_sector(v, c, off, &sec);
	if (r != EXFAT_OK)
		return r;
	if (sec != v->sec_no)
	{
		if ((r = rd(v, sec, 1, v->sec_buf)) != EXFAT_OK)
		{
			v->sec_no = ~0ull;
			return r;
		}
		v->sec_no = sec;
	}
	memcpy(out, v->sec_buf + (off & (v->sec_size - 1)), ENT_SIZE);
	return EXFAT_OK;
}

static int ent_write(exfat_vol *v, struct chain *c, uint64_t off, const uint8_t *in)
{
	uint64_t sec;
	int r = chain_sector(v, c, off, &sec);
	if (r != EXFAT_OK)
		return r;
	if (sec != v->sec_no)
	{
		if ((r = rd(v, sec, 1, v->sec_buf)) != EXFAT_OK)
		{
			v->sec_no = ~0ull;
			return r;
		}
		v->sec_no = sec;
	}
	memcpy(v->sec_buf + (off & (v->sec_size - 1)), in, ENT_SIZE);
	return wr(v, sec, 1, v->sec_buf);
}

static uint16_t set_checksum(const uint8_t *ents, int n)
{
	uint16_t cs = 0;
	int i;
	for (i = 0; i < n * ENT_SIZE; i++)
	{
		if (i == 2 || i == 3)
			continue;
		cs = (uint16_t) (((cs & 1) ? 0x8000 : 0) + (cs >> 1) + ents[i]);
	}
	return cs;
}

/* a whole entry set (File + secondaries) into ents (up to 19 entries) */
#define MAX_SET 19

static void parse_set(exfat_vol *v, const uint8_t *e, int n, exfat_node *out)
{
	int i, k = 0;
	(void) v;
	out->secondaries = (uint8_t) (n - 1);
	out->attr = g16(e + 4);
	out->ctime = g32(e + 8);
	out->mtime = g32(e + 12);
	out->atime = g32(e + 16);
	out->c10ms = e[20];
	out->m10ms = e[21];
	out->ctz = e[22];
	out->mtz = e[23];
	out->atz = e[24];
	out->nofat = (e[32 + 1] & 2) != 0;
	out->name_len = e[32 + 3];
	out->valid = g64(e + 32 + 8);
	out->first_cluster = g32(e + 32 + 20);
	out->size = g64(e + 32 + 24);
	for (i = 2; i < n && k < out->name_len; i++)
	{
		int j;
		for (j = 0; j < 15 && k < out->name_len; j++)
			out->name[k++] = g16(e + i * 32 + 2 + j * 2);
	}
	out->name[k] = 0;
	out->cur_index = out->cur_cluster = 0;
	out->is_root = 0;
}

/* read the entry set whose File entry is at off */
static int read_set(exfat_vol *v, struct chain *c, uint64_t dsize, uint64_t off, uint8_t *ents, int *nents)
{
	int r, n, i;
	if ((r = ent_read(v, c, off, ents)) != EXFAT_OK)
		return r;
	if (ents[0] != T_FILE)
		return EXFAT_ECORRUPT;
	n = 1 + ents[1];
	if (n < 3 || n > MAX_SET || off + (uint64_t) n * ENT_SIZE > dsize)
		return EXFAT_ECORRUPT;
	for (i = 1; i < n; i++)
		if ((r = ent_read(v, c, off + (uint64_t) i * ENT_SIZE, ents + i * ENT_SIZE)) != EXFAT_OK)
			return r;
	if (ents[32] != T_STREAM || set_checksum(ents, n) != g16(ents + 2))
		return EXFAT_ECORRUPT;
	*nents = n;
	return EXFAT_OK;
}

int exfat_readdir(exfat_vol *v, const exfat_node *dir, uint64_t *cookie, exfat_node *out)
{
	struct chain c;
	uint64_t dsize, off;
	uint8_t e[ENT_SIZE], set[MAX_SET * ENT_SIZE];
	int r, n;
	if (!(dir->attr & EXFAT_ATTR_DIR))
		return EXFAT_ENOTDIR;
	if ((r = dir_size(v, dir, &dsize)) != EXFAT_OK)
		return r;
	dir_chain(dir, &c, v);
	for (off = *cookie; off + ENT_SIZE <= dsize; off += ENT_SIZE)
	{
		if ((r = ent_read(v, &c, off, e)) != EXFAT_OK)
			return r;
		if (e[0] == T_EOD)
			break;
		if (e[0] != T_FILE)
			continue;
		if (read_set(v, &c, dsize, off, set, &n) != EXFAT_OK)
			continue;   /* a damaged set is skipped, as Windows does */
		parse_set(v, set, n, out);
		out->pdir_cluster = dir->first_cluster;
		out->pdir_size = dsize;
		out->pdir_nofat = dir->nofat;
		out->entry = off;
		*cookie = off + (uint64_t) n * ENT_SIZE;
		return EXFAT_OK;
	}
	*cookie = dsize;
	return EXFAT_END;
}

int exfat_find(exfat_vol *v, const exfat_node *dir, const uint16_t *name, size_t len, exfat_node *out)
{
	uint64_t cookie = 0;
	uint16_t h = name_hash(v, name, len);
	int r;
	(void) h;
	if (len == 0 || len > EXFAT_NAME_MAX)
		return EXFAT_ENOENT;
	while ((r = exfat_readdir(v, dir, &cookie, out)) == EXFAT_OK)
		if (name_eq(v, out->name, out->name_len, name, len))
			return EXFAT_OK;
	return r == EXFAT_END ? EXFAT_ENOENT : r;
}

void exfat_root(exfat_vol *v, exfat_node *out)
{
	memset(out, 0, sizeof(*out));
	out->is_root = 1;
	out->attr = EXFAT_ATTR_DIR;
	out->first_cluster = v->root_cluster;
}

int exfat_lookup(exfat_vol *v, const uint16_t *path, size_t len, exfat_node *out)
{
	exfat_node cur;
	size_t i = 0;
	exfat_root(v, &cur);
	while (i < len)
	{
		size_t s;
		int r;
		while (i < len && (path[i] == '\\' || path[i] == '/'))
			i++;
		if (i >= len)
			break;
		s = i;
		while (i < len && path[i] != '\\' && path[i] != '/')
			i++;
		if (!(cur.attr & EXFAT_ATTR_DIR))
			return EXFAT_ENOTDIR;
		if ((r = exfat_find(v, &cur, path + s, i - s, out)) != EXFAT_OK)
			return r;
		cur = *out;
	}
	*out = cur;
	return EXFAT_OK;
}

/* ---- reading and writing file data ---------------------------------------------------- */
static void node_chain(exfat_node *f, struct chain *c)
{
	c->first = f->first_cluster;
	c->nofat = f->nofat;
	c->size = f->size;
	c->cur_index = f->cur_index;
	c->cur_cluster = f->cur_cluster;
}

static void node_save_cursor(exfat_node *f, struct chain *c)
{
	f->cur_index = c->cur_index;
	f->cur_cluster = c->cur_cluster;
}

int exfat_read(exfat_vol *v, exfat_node *f, uint64_t off, void *buf, uint32_t len, uint32_t *got)
{
	struct chain c;
	uint8_t *out = (uint8_t *) buf;
	uint32_t done = 0;
	int r = EXFAT_OK;
	*got = 0;
	if (f->attr & EXFAT_ATTR_DIR)
		return EXFAT_EISDIR;
	if (off >= f->size)
		return EXFAT_OK;
	if (len > f->size - off)
		len = (uint32_t) (f->size - off);
	node_chain(f, &c);
	while (done < len)
	{
		uint64_t pos = off + done, sec;
		uint32_t in_sec = (uint32_t) (pos & (v->sec_size - 1));
		uint32_t n = v->sec_size - in_sec;
		if (n > len - done)
			n = len - done;
		if (pos >= f->valid)   /* past ValidDataLength: reads as zeros */
		{
			uint64_t z = f->size - pos;
			if (z > len - done)
				z = len - done;
			memset(out + done, 0, (size_t) z);
			done += (uint32_t) z;
			continue;
		}
		if (pos + n > f->valid)
			n = (uint32_t) (f->valid - pos);
		if ((r = chain_sector(v, &c, pos, &sec)) != EXFAT_OK)
			break;
		if (in_sec == 0 && n == v->sec_size)
		{
			/* whole sectors straight into the caller's buffer, as many as are contiguous */
			uint32_t secs = n >> v->sec_shift, spc = 1u << v->spc_shift;
			uint32_t left_in_cl = spc - (uint32_t) ((pos >> v->sec_shift) & (spc - 1));
			uint32_t want = (len - done) >> v->sec_shift;
			uint64_t vleft = (f->valid - pos) >> v->sec_shift;
			secs = want < left_in_cl ? want : left_in_cl;
			if (secs > vleft)
				secs = (uint32_t) vleft;
			if (!secs)
				secs = 1;
			if ((r = rd(v, sec, secs, out + done)) != EXFAT_OK)
				break;
			done += secs << v->sec_shift;
			continue;
		}
		if ((r = rd(v, sec, 1, v->sec_buf)) != EXFAT_OK)
			break;
		v->sec_no = sec;
		memcpy(out + done, v->sec_buf + in_sec, n);
		done += n;
	}
	node_save_cursor(f, &c);
	*got = done;
	return r;
}

/* the entry set of n, read again, for an update */
static int load_set(exfat_vol *v, const exfat_node *n, struct chain *pc, uint8_t *set, int *nents)
{
	pc->first = n->pdir_cluster;
	pc->nofat = n->pdir_nofat;
	pc->size = n->pdir_size;
	pc->cur_index = pc->cur_cluster = 0;
	return read_set(v, pc, n->pdir_size, n->entry, set, nents);
}

static int store_set(exfat_vol *v, struct chain *pc, uint64_t off, uint8_t *set, int nents)
{
	int i, r;
	p16(set + 2, set_checksum(set, nents));
	for (i = 0; i < nents; i++)
		if ((r = ent_write(v, pc, off + (uint64_t) i * ENT_SIZE, set + i * ENT_SIZE)) != EXFAT_OK)
			return r;
	return EXFAT_OK;
}

/* n's size, clusters, attributes and times into its entry set */
static int write_node(exfat_vol *v, exfat_node *n)
{
	struct chain pc;
	uint8_t set[MAX_SET * ENT_SIZE];
	int ne, r;
	if (n->is_root)
		return EXFAT_OK;
	if ((r = load_set(v, n, &pc, set, &ne)) != EXFAT_OK)
		return r;
	p16(set + 4, n->attr);
	p32(set + 8, n->ctime);
	p32(set + 12, n->mtime);
	p32(set + 16, n->atime);
	set[20] = n->c10ms;
	set[21] = n->m10ms;
	set[22] = n->ctz;
	set[23] = n->mtz;
	set[24] = n->atz;
	set[32 + 1] = (uint8_t) (0x01 | (n->nofat ? 0x02 : 0));
	p64(set + 32 + 8, n->valid);
	p32(set + 32 + 20, n->first_cluster);
	p64(set + 32 + 24, n->size);
	return store_set(v, &pc, n->entry, set, ne);
}

int exfat_update(exfat_vol *v, exfat_node *n)
{
	int r = mark_dirty(v);
	if (r != EXFAT_OK)
		return r;
	return write_node(v, n);
}

static void stamp(exfat_vol *v, uint32_t *t, uint8_t *ms, uint8_t *tz)
{
	struct exfat_time now;
	if (!v->now)
		return;
	v->now(v->now_ctx, &now);
	*t = now.dos;
	if (ms)
		*ms = now.ms10;
	*tz = now.utc_off;
}

/* clusters the file holds now */
static uint32_t clusters_of(exfat_vol *v, uint64_t size)
{
	return (uint32_t) ((size + v->cl_size - 1) >> (v->sec_shift + v->spc_shift));
}

/* write the FAT chain for a contiguous run, so the file can stop being NoFatChain */
static int unfold(exfat_vol *v, exfat_node *f, uint32_t count)
{
	uint32_t i;
	int r;
	for (i = 0; i + 1 < count; i++)
		if ((r = fat_set(v, f->first_cluster + i, f->first_cluster + i + 1)) != EXFAT_OK)
			return r;
	if (count && (r = fat_set(v, f->first_cluster + count - 1, FAT_EOC)) != EXFAT_OK)
		return r;
	f->nofat = 0;
	return EXFAT_OK;
}

/* make f hold want clusters (it holds have); zero = directories only */
static int grow(exfat_vol *v, exfat_node *f, uint32_t have, uint32_t want, int zero)
{
	uint32_t last = 0, cl;
	int r;
	struct chain c;
	if (have)
	{
		node_chain(f, &c);
		if ((r = chain_cluster(v, &c, (uint64_t) (have - 1) * v->cl_size, &last)) != EXFAT_OK)
			return r;
	}
	if (want - have > v->free_clusters)
		return EXFAT_ENOSPC;
	while (have < want)
	{
		uint32_t hint = have ? last + 1 : v->alloc_hint;
		if ((r = alloc_one(v, hint, &cl)) != EXFAT_OK)
			return r;
		if (zero && (r = zero_cluster(v, cl)) != EXFAT_OK)
			return r;
		if (!have)
		{
			f->first_cluster = cl;
			f->nofat = !f->is_root;   /* a single cluster is contiguous */
			if (f->is_root && (r = fat_set(v, cl, FAT_EOC)) != EXFAT_OK)
				return r;
		}
		else if (f->nofat && cl == last + 1)
			;   /* still contiguous */
		else
		{
			if (f->nofat && (r = unfold(v, f, have)) != EXFAT_OK)
				return r;
			if ((r = fat_set(v, last, cl)) != EXFAT_OK || (r = fat_set(v, cl, FAT_EOC)) != EXFAT_OK)
				return r;
		}
		last = cl;
		have++;
	}
	f->cur_index = f->cur_cluster = 0;
	return EXFAT_OK;
}

/* free clusters from index keep on */
static int shrink(exfat_vol *v, exfat_node *f, uint32_t have, uint32_t keep)
{
	uint32_t i, cl, nx;
	struct chain c;
	int r;
	if (keep >= have)
		return EXFAT_OK;
	if (f->nofat)
	{
		for (i = keep; i < have; i++)
			bm_set(v, f->first_cluster + i, 0);
	}
	else
	{
		node_chain(f, &c);
		c.cur_index = c.cur_cluster = 0;
		if (keep)
		{
			if ((r = chain_cluster(v, &c, (uint64_t) (keep - 1) * v->cl_size, &cl)) != EXFAT_OK)
				return r;
			if ((r = fat_get(v, cl, &nx)) != EXFAT_OK || (r = fat_set(v, cl, FAT_EOC)) != EXFAT_OK)
				return r;
			cl = nx;
		}
		else
			cl = f->first_cluster;
		for (i = keep; i < have && valid_cl(v, cl); i++)
		{
			if ((r = fat_get(v, cl, &nx)) != EXFAT_OK || (r = fat_set(v, cl, 0)) != EXFAT_OK)
				return r;
			bm_set(v, cl, 0);
			cl = nx;
		}
	}
	if (!keep)
	{
		f->first_cluster = 0;
		f->nofat = 0;
	}
	f->cur_index = f->cur_cluster = 0;
	return EXFAT_OK;
}

static int write_zeros(exfat_vol *v, exfat_node *f, uint64_t from, uint64_t to)
{
	struct chain c;
	int r;
	node_chain(f, &c);
	while (from < to)
	{
		uint64_t sec;
		uint32_t in = (uint32_t) (from & (v->sec_size - 1));
		uint32_t n = v->sec_size - in;
		if (n > to - from)
			n = (uint32_t) (to - from);
		if ((r = chain_sector(v, &c, from, &sec)) != EXFAT_OK)
			return r;
		if (in || n < v->sec_size)
		{
			if ((r = rd(v, sec, 1, v->sec_buf)) != EXFAT_OK)
				return r;
		}
		v->sec_no = ~0ull;
		memset(v->sec_buf + in, 0, n);
		if ((r = wr(v, sec, 1, v->sec_buf)) != EXFAT_OK)
			return r;
		from += n;
	}
	node_save_cursor(f, &c);
	return EXFAT_OK;
}

int exfat_write(exfat_vol *v, exfat_node *f, uint64_t off, const void *buf, uint32_t len, uint32_t *put)
{
	const uint8_t *in = (const uint8_t *) buf;
	uint32_t done = 0, have, want;
	struct chain c;
	int r;
	*put = 0;
	if (f->attr & EXFAT_ATTR_DIR)
		return EXFAT_EISDIR;
	if ((r = mark_dirty(v)) != EXFAT_OK)
		return r;
	if (!len)
		return EXFAT_OK;
	if (off + len < off)
		return EXFAT_EINVAL;
	have = clusters_of(v, f->size);
	want = clusters_of(v, off + len);
	if (want > have)
	{
		if ((r = grow(v, f, have, want, 0)) != EXFAT_OK)
			return r;
	}
	if (off + len > f->size)
		f->size = off + len;
	/* bytes between the old valid length and off become zeros on the disk */
	if (off > f->valid)
	{
		if ((r = write_zeros(v, f, f->valid, off)) != EXFAT_OK)
			return r;
		f->valid = off;
	}
	node_chain(f, &c);
	while (done < len)
	{
		uint64_t pos = off + done, sec;
		uint32_t in_sec = (uint32_t) (pos & (v->sec_size - 1));
		uint32_t n = v->sec_size - in_sec;
		if (n > len - done)
			n = len - done;
		if ((r = chain_sector(v, &c, pos, &sec)) != EXFAT_OK)
			break;
		if (in_sec == 0 && n == v->sec_size)
		{
			uint32_t spc = 1u << v->spc_shift;
			uint32_t left_in_cl = spc - (uint32_t) ((pos >> v->sec_shift) & (spc - 1));
			uint32_t want_s = (len - done) >> v->sec_shift;
			uint32_t secs = want_s < left_in_cl ? want_s : left_in_cl;
			if ((r = wr(v, sec, secs, in + done)) != EXFAT_OK)
				break;
			done += secs << v->sec_shift;
			continue;
		}
		/* part of a sector: read it unless it lies past the valid data */
		if (pos - in_sec < f->valid)
		{
			if ((r = rd(v, sec, 1, v->sec_buf)) != EXFAT_OK)
				break;
		}
		else
			memset(v->sec_buf, 0, v->sec_size);
		v->sec_no = ~0ull;
		memcpy(v->sec_buf + in_sec, in + done, n);
		if ((r = wr(v, sec, 1, v->sec_buf)) != EXFAT_OK)
			break;
		done += n;
	}
	node_save_cursor(f, &c);
	if (off + done > f->valid)
		f->valid = off + done;
	stamp(v, &f->mtime, &f->m10ms, &f->mtz);
	f->attr |= EXFAT_ATTR_ARCHIVE;
	*put = done;
	if (r == EXFAT_OK)
		r = write_node(v, f);
	return r;
}

int exfat_truncate(exfat_vol *v, exfat_node *f, uint64_t size)
{
	uint32_t have, want;
	int r;
	if (f->attr & EXFAT_ATTR_DIR)
		return EXFAT_EISDIR;
	if ((r = mark_dirty(v)) != EXFAT_OK)
		return r;
	have = clusters_of(v, f->size);
	want = clusters_of(v, size);
	if (want > have)
	{
		if ((r = grow(v, f, have, want, 0)) != EXFAT_OK)
			return r;
	}
	else if ((r = shrink(v, f, have, want)) != EXFAT_OK)
		return r;
	f->size = size;
	if (f->valid > size)
		f->valid = size;
	stamp(v, &f->mtime, &f->m10ms, &f->mtz);
	return write_node(v, f);
}

/* ---- creating, removing, renaming entries --------------------------------------------- */
static int entries_for(size_t len)
{
	return 2 + (int) ((len + 14) / 15);
}

static int name_ok(const uint16_t *n, size_t len)
{
	size_t i;
	if (!len || len > EXFAT_NAME_MAX)
		return 0;
	for (i = 0; i < len; i++)
	{
		uint16_t c = n[i];
		if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
		    c == '\\' || c == '|')
			return 0;
	}
	if (len == 1 && n[0] == '.')
		return 0;
	if (len == 2 && n[0] == '.' && n[1] == '.')
		return 0;
	return 1;
}

/* a run of need free entries in dir: byte offset in *off; grows the directory if needed */
static int find_free(exfat_vol *v, exfat_node *dir, int need, uint64_t *off)
{
	struct chain c;
	uint64_t dsize, o, run_start = 0;
	uint8_t e[ENT_SIZE];
	int run = 0, r;
	if ((r = dir_size(v, dir, &dsize)) != EXFAT_OK)
		return r;
	dir_chain(dir, &c, v);
	for (o = 0; o + ENT_SIZE <= dsize; o += ENT_SIZE)
	{
		if ((r = ent_read(v, &c, o, e)) != EXFAT_OK)
			return r;
		if (e[0] & IN_USE)
		{
			run = 0;
			continue;
		}
		if (!run)
			run_start = o;
		if (++run == need)
		{
			*off = run_start;
			return EXFAT_OK;
		}
		if (e[0] == T_EOD)
		{
			/* everything after end-of-directory is free too */
			if (dsize - run_start >= (uint64_t) need * ENT_SIZE)
			{
				*off = run_start;
				return EXFAT_OK;
			}
			break;
		}
	}
	/* one more cluster (zeroed) */
	{
		uint32_t have = (uint32_t) (dsize >> (v->sec_shift + v->spc_shift));
		if ((r = grow(v, dir, have, have + 1, 1)) != EXFAT_OK)
			return r;
		if (!run)
			run_start = dsize;
		*off = run_start;
		if (!dir->is_root)
		{
			dir->size += v->cl_size;
			dir->valid = dir->size;
			if ((r = write_node(v, dir)) != EXFAT_OK)
				return r;
		}
	}
	return EXFAT_OK;
}

static void build_set(exfat_vol *v, uint8_t *set, int ne, const exfat_node *n)
{
	int i, k = 0;
	memset(set, 0, (size_t) ne * ENT_SIZE);
	set[0] = T_FILE;
	set[1] = (uint8_t) (ne - 1);
	p16(set + 4, n->attr);
	p32(set + 8, n->ctime);
	p32(set + 12, n->mtime);
	p32(set + 16, n->atime);
	set[20] = n->c10ms;
	set[21] = n->m10ms;
	set[22] = n->ctz;
	set[23] = n->mtz;
	set[24] = n->atz;
	set[32] = T_STREAM;
	set[32 + 1] = (uint8_t) (0x01 | (n->nofat ? 0x02 : 0));
	set[32 + 3] = n->name_len;
	p16(set + 32 + 4, name_hash(v, n->name, n->name_len));
	p64(set + 32 + 8, n->valid);
	p32(set + 32 + 20, n->first_cluster);
	p64(set + 32 + 24, n->size);
	for (i = 2; i < ne; i++)
	{
		int j;
		set[i * 32] = T_NAME;
		for (j = 0; j < 15; j++)
			p16(set + i * 32 + 2 + j * 2, (uint16_t) (k < n->name_len ? n->name[k++] : 0));
	}
}

static int place_set(exfat_vol *v, exfat_node *dir, exfat_node *n)
{
	struct chain c;
	uint8_t set[MAX_SET * ENT_SIZE];
	uint64_t off, dsize;
	int ne = entries_for(n->name_len), r;
	if ((r = find_free(v, dir, ne, &off)) != EXFAT_OK)
		return r;
	if ((r = dir_size(v, dir, &dsize)) != EXFAT_OK)
		return r;
	build_set(v, set, ne, n);
	dir_chain(dir, &c, v);
	if ((r = store_set(v, &c, off, set, ne)) != EXFAT_OK)
		return r;
	n->pdir_cluster = dir->first_cluster;
	n->pdir_size = dsize;
	n->pdir_nofat = dir->nofat;
	n->entry = off;
	n->secondaries = (uint8_t) (ne - 1);
	return EXFAT_OK;
}

static int erase_set(exfat_vol *v, const exfat_node *n)
{
	struct chain pc;
	uint8_t e[ENT_SIZE];
	int i, r;
	pc.first = n->pdir_cluster;
	pc.nofat = n->pdir_nofat;
	pc.size = n->pdir_size;
	pc.cur_index = pc.cur_cluster = 0;
	for (i = 0; i <= n->secondaries; i++)
	{
		uint64_t o = n->entry + (uint64_t) i * ENT_SIZE;
		if ((r = ent_read(v, &pc, o, e)) != EXFAT_OK)
			return r;
		e[0] &= (uint8_t) ~IN_USE;
		if ((r = ent_write(v, &pc, o, e)) != EXFAT_OK)
			return r;
	}
	return EXFAT_OK;
}

int exfat_create(exfat_vol *v, const exfat_node *dir, const uint16_t *name, size_t len, uint16_t attr, exfat_node *out)
{
	exfat_node d = *dir, tmp;
	int r;
	if (!(dir->attr & EXFAT_ATTR_DIR))
		return EXFAT_ENOTDIR;
	if (!name_ok(name, len))
		return EXFAT_EINVAL;
	if ((r = exfat_find(v, &d, name, len, &tmp)) == EXFAT_OK)
		return EXFAT_EEXIST;
	if (r != EXFAT_ENOENT)
		return r;
	if ((r = mark_dirty(v)) != EXFAT_OK)
		return r;
	memset(out, 0, sizeof(*out));
	out->attr = attr & (EXFAT_ATTR_RO | EXFAT_ATTR_HIDDEN | EXFAT_ATTR_SYSTEM | EXFAT_ATTR_DIR | EXFAT_ATTR_ARCHIVE);
	out->name_len = (uint8_t) len;
	memcpy(out->name, name, len * 2);
	stamp(v, &out->ctime, &out->c10ms, &out->ctz);
	out->mtime = out->atime = out->ctime;
	out->m10ms = out->c10ms;
	out->mtz = out->atz = out->ctz;
	if (attr & EXFAT_ATTR_DIR)
	{
		/* a directory always has a cluster (zeroed: no entries) */
		if ((r = grow(v, out, 0, 1, 1)) != EXFAT_OK)
			return r;
		out->size = out->valid = v->cl_size;
	}
	if ((r = place_set(v, &d, out)) != EXFAT_OK)
	{
		if (attr & EXFAT_ATTR_DIR)
			shrink(v, out, 1, 0);
		return r;
	}
	return EXFAT_OK;
}

int exfat_dir_empty(exfat_vol *v, const exfat_node *dir)
{
	uint64_t cookie = 0;
	exfat_node n;
	int r = exfat_readdir(v, dir, &cookie, &n);
	return r == EXFAT_END ? 1 : r == EXFAT_OK ? 0 : r;
}

int exfat_remove(exfat_vol *v, exfat_node *n)
{
	int r;
	if (n->is_root)
		return EXFAT_EINVAL;
	if (n->attr & EXFAT_ATTR_DIR)
	{
		r = exfat_dir_empty(v, n);
		if (r < 0)
			return r;
		if (!r)
			return EXFAT_ENOTEMPTY;
	}
	if ((r = mark_dirty(v)) != EXFAT_OK)
		return r;
	if ((r = erase_set(v, n)) != EXFAT_OK)
		return r;
	return shrink(v, n, clusters_of(v, n->size), 0);
}

int exfat_rename(exfat_vol *v, exfat_node *n, const exfat_node *newdir, const uint16_t *name, size_t len)
{
	exfat_node d = *newdir, tmp, moved;
	int r;
	if (n->is_root)
		return EXFAT_EINVAL;
	if (!(newdir->attr & EXFAT_ATTR_DIR))
		return EXFAT_ENOTDIR;
	if (!name_ok(name, len))
		return EXFAT_EINVAL;
	r = exfat_find(v, &d, name, len, &tmp);
	if (r == EXFAT_OK && !(tmp.entry == n->entry && tmp.pdir_cluster == n->pdir_cluster))
		return EXFAT_EEXIST;
	if (r != EXFAT_OK && r != EXFAT_ENOENT)
		return r;
	if ((r = mark_dirty(v)) != EXFAT_OK)
		return r;
	/* a directory may not move into itself or below itself */
	if (n->attr & EXFAT_ATTR_DIR)
	{
		/* checked by the caller's path handling; here only the direct case */
		if (newdir->first_cluster == n->first_cluster)
			return EXFAT_EINVAL;
	}
	moved = *n;
	moved.name_len = (uint8_t) len;
	memcpy(moved.name, name, len * 2);
	/* the old entries go first, so a rename in place can reuse them */
	if ((r = erase_set(v, n)) != EXFAT_OK)
		return r;
	if ((r = place_set(v, &d, &moved)) != EXFAT_OK)
		return r;
	*n = moved;
	return EXFAT_OK;
}

/* ---- mount -------------------------------------------------------------------------------- */
static uint32_t boot_checksum(const uint8_t *sectors, uint32_t sec_size)
{
	uint32_t cs = 0, i, n = 11 * sec_size;
	for (i = 0; i < n; i++)
	{
		if (i == 106 || i == 107 || i == 112)
			continue;
		cs = ((cs & 1) ? 0x80000000u : 0) + (cs >> 1) + sectors[i];
	}
	return cs;
}

static int load_upcase(exfat_vol *v, uint32_t first, uint64_t len)
{
	struct chain c;
	uint64_t off;
	uint32_t i = 0;
	int r, skip = 0;
	v->upcase = (uint16_t *) v->dev.alloc(v->dev.ctx, 65536 * 2);
	if (!v->upcase)
		return EXFAT_ENOMEM;
	for (i = 0; i < 65536; i++)
		v->upcase[i] = (uint16_t) i;
	c.first = first;
	c.nofat = 0;
	c.size = len;
	c.cur_index = c.cur_cluster = 0;
	i = 0;
	for (off = 0; off + 2 <= len && i < 65536; off += 2)
	{
		uint64_t sec;
		uint16_t val;
		if ((off & (v->sec_size - 1)) == 0)
		{
			if ((r = chain_sector(v, &c, off, &sec)) != EXFAT_OK)
				return r;
			if ((r = rd(v, sec, 1, v->sec_buf)) != EXFAT_OK)
				return r;
			v->sec_no = sec;
		}
		val = g16(v->sec_buf + (off & (v->sec_size - 1)));
		if (skip)
		{
			i += val;   /* identity mappings */
			skip = 0;
			continue;
		}
		if (val == 0xFFFF)
		{
			skip = 1;
			continue;
		}
		v->upcase[i++] = val;
	}
	return EXFAT_OK;
}

static int load_bitmap(exfat_vol *v, uint32_t first, uint64_t len)
{
	struct chain c;
	uint64_t need = (v->clusters + 7) / 8, off;
	uint32_t i;
	int r;
	if (len < need)
		return EXFAT_ECORRUPT;
	v->bitmap_len = need;
	v->bitmap_cluster = first;
	v->bitmap = (uint8_t *) v->dev.alloc(v->dev.ctx, (size_t) ((need + v->sec_size) & ~(uint64_t) (v->sec_size - 1)));
	v->bitmap_dirty = (uint8_t *) v->dev.alloc(v->dev.ctx, (size_t) ((need >> v->sec_shift) + 1));
	if (!v->bitmap || !v->bitmap_dirty)
		return EXFAT_ENOMEM;
	c.first = first;
	c.nofat = 0;
	c.size = len;
	c.cur_index = c.cur_cluster = 0;
	for (off = 0; off < need; off += v->sec_size)
	{
		uint64_t sec;
		if ((r = chain_sector(v, &c, off, &sec)) != EXFAT_OK)
			return r;
		if ((r = rd(v, sec, 1, v->bitmap + off)) != EXFAT_OK)
			return r;
	}
	/* bits past the last cluster do not count */
	if (v->clusters & 7)
		v->bitmap[need - 1] &= (uint8_t) ((1u << (v->clusters & 7)) - 1);
	v->free_clusters = 0;
	for (i = 0; i < v->clusters; i++)
		if (!((v->bitmap[i >> 3] >> (i & 7)) & 1))
			v->free_clusters++;
	return EXFAT_OK;
}

static void vol_free(exfat_vol *v)
{
	void (*fr)(void *, void *) = v->dev.free;
	void *ctx = v->dev.ctx;
	if (v->bitmap)
		fr(ctx, v->bitmap);
	if (v->bitmap_dirty)
		fr(ctx, v->bitmap_dirty);
	if (v->upcase)
		fr(ctx, v->upcase);
	if (v->fat_buf)
		fr(ctx, v->fat_buf);
	if (v->sec_buf)
		fr(ctx, v->sec_buf);
	fr(ctx, v);
}

int exfat_mount(exfat_vol **pv, const struct exfat_dev *dev, int readonly)
{
	exfat_vol *v;
	uint8_t *boot;
	uint32_t cs, i;
	int r;
	struct chain c;
	uint64_t off, dsize;
	uint8_t e[ENT_SIZE];
	uint32_t bm_first = 0, up_first = 0;
	uint64_t bm_len = 0, up_len = 0;
	exfat_node root;

	*pv = NULL;
	v = (exfat_vol *) dev->alloc(dev->ctx, sizeof(*v));
	if (!v)
		return EXFAT_ENOMEM;
	v->dev = *dev;
	v->ro = readonly;
	v->fat_sec = v->sec_no = ~0ull;
	/* the boot region at the largest sector size: 12 sectors of 4096 bytes */
	boot = (uint8_t *) dev->alloc(dev->ctx, 12 * 4096);
	if (!boot)
	{
		dev->free(dev->ctx, v);
		return EXFAT_ENOMEM;
	}
	r = EXFAT_ECORRUPT;
	if (dev->read(dev->ctx, 0, 1, boot))
	{
		r = EXFAT_EIO;
		goto fail;
	}
	if (memcmp(boot + 3, "EXFAT   ", 8) || boot[510] != 0x55 || boot[511] != 0xAA)
		goto fail;
	v->sec_shift = boot[108];
	v->spc_shift = boot[109];
	if (v->sec_shift < 9 || v->sec_shift > 12 || v->sec_shift + v->spc_shift > 25)
		goto fail;
	v->sec_size = 1u << v->sec_shift;
	if (dev->sector_size && dev->sector_size != v->sec_size)
	{
		r = EXFAT_EINVAL;
		goto fail;
	}
	v->cl_size = 1u << (v->sec_shift + v->spc_shift);
	/* sector 0 was read with the caller's sector size; read the region again */
	if (dev->read(dev->ctx, 0, 12, boot))
	{
		r = EXFAT_EIO;
		goto fail;
	}
	cs = boot_checksum(boot, v->sec_size);
	for (i = 0; i < v->sec_size / 4; i++)
		if (g32(boot + 11 * v->sec_size + i * 4) != cs)
			goto fail;
	v->fat_off = g32(boot + 80);
	v->fat_len = g32(boot + 84);
	v->heap_off = g32(boot + 88);
	v->clusters = g32(boot + 92);
	v->root_cluster = g32(boot + 96);
	v->serial = g32(boot + 100);
	v->vol_flags = g16(boot + 106);
	v->nfats = boot[110];
	v->active_fat = (v->nfats == 2 && (v->vol_flags & 1)) ? 1 : 0;
	if (g16(boot + 104) >> 8 != 1 || !v->clusters || v->nfats < 1 || v->nfats > 2 || !valid_cl(v, v->root_cluster))
		goto fail;
	dev->free(dev->ctx, boot);
	boot = NULL;

	v->fat_buf = (uint8_t *) dev->alloc(dev->ctx, v->sec_size);
	v->sec_buf = (uint8_t *) dev->alloc(dev->ctx, v->sec_size);
	if (!v->fat_buf || !v->sec_buf)
	{
		r = EXFAT_ENOMEM;
		goto fail;
	}
	/* the root directory: bitmap, up-case table, label */
	exfat_root(v, &root);
	if ((r = dir_size(v, &root, &dsize)) != EXFAT_OK)
		goto fail;
	dir_chain(&root, &c, v);
	for (off = 0; off < dsize; off += ENT_SIZE)
	{
		if ((r = ent_read(v, &c, off, e)) != EXFAT_OK)
			goto fail;
		if (e[0] == T_EOD)
			break;
		if (e[0] == T_BITMAP && !(e[1] & 1) == !(v->active_fat))
		{
			bm_first = g32(e + 20);
			bm_len = g64(e + 24);
		}
		else if (e[0] == T_UPCASE)
		{
			up_first = g32(e + 20);
			up_len = g64(e + 24);
		}
		else if (e[0] == T_LABEL)
		{
			v->label_len = e[1] > 11 ? 11 : e[1];
			for (i = 0; i < v->label_len; i++)
				v->label[i] = g16(e + 2 + i * 2);
		}
	}
	r = EXFAT_ECORRUPT;
	if (!valid_cl(v, bm_first) || !valid_cl(v, up_first))
		goto fail;
	if ((r = load_bitmap(v, bm_first, bm_len)) != EXFAT_OK)
		goto fail;
	if ((r = load_upcase(v, up_first, up_len)) != EXFAT_OK)
		goto fail;
	v->alloc_hint = 2;
	/* a volume left dirty by someone else stays dirty: we never clear their flag */
	v->dirty_flag_set = (v->vol_flags & 2) != 0;
	*pv = v;
	return EXFAT_OK;
fail:
	if (boot)
		dev->free(dev->ctx, boot);
	vol_free(v);
	return r;
}

int exfat_flush(exfat_vol *v)
{
	int r;
	if (v->ro)
		return EXFAT_OK;
	if ((r = fat_flush(v)) != EXFAT_OK)
		return r;
	return bm_flush(v);
}

void exfat_unmount(exfat_vol *v)
{
	if (!v->ro && exfat_flush(v) == EXFAT_OK && v->dirty_flag_set && !(v->vol_flags & 2))
	{
		/* clean again: VolumeDirty off, PercentInUse brought up to date */
		uint8_t *b = v->sec_buf;
		if (rd(v, 0, 1, b) == EXFAT_OK)
		{
			p16(b + 106, (uint16_t) (g16(b + 106) & ~2));
			b[112] = (uint8_t) ((uint64_t) (v->clusters - v->free_clusters) * 100 / v->clusters);
			wr(v, 0, 1, b);
		}
	}
	vol_free(v);
}

void exfat_info(exfat_vol *v, struct exfat_info *out)
{
	out->sector_size = v->sec_size;
	out->cluster_size = v->cl_size;
	out->clusters = v->clusters;
	out->free_clusters = v->free_clusters;
	out->serial = v->serial;
	memcpy(out->label, v->label, sizeof(out->label));
	out->label_len = v->label_len;
}

void exfat_set_clock(exfat_vol *v, void (*now)(void *ctx, struct exfat_time *t), void *ctx)
{
	v->now = now;
	v->now_ctx = ctx;
}
