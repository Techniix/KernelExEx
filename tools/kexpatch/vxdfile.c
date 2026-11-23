/*
 *  KernelExEx - vxdfile.c: see vxdfile.h.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  The DS ("DriveSpace") decompression of W4 chunks is written from patcher9x's
 *  pe/ds_decompress.c, Copyright (c) 2022 Jaroslav Hensl, MIT licence:
 *    Permission is hereby granted, free of charge, to any person obtaining a copy
 *    of this software and associated documentation files (the "Software"), to deal
 *    in the Software without restriction, including without limitation the rights
 *    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *    copies of the Software, and to permit persons to whom the Software is
 *    furnished to do so, subject to the following conditions: The above copyright
 *    notice and this permission notice shall be included in all copies or
 *    substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
 *    WITHOUT WARRANTY OF ANY KIND.
 *  Differences: works on memory buffers, checks every bound of its input.
 */
#include "vxdfile.h"
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }

/* ------------------------------------------------------------------ DS decompression */

struct bits
{
	const uint8_t *p, *end;
	uint32_t buf;
	int len;
};

/* refill to 32 bits, least significant first; past the end reads zeros */
static void refill(struct bits *b)
{
	while (b->len <= 24)
	{
		uint32_t byte = b->p < b->end ? *b->p++ : 0;
		b->buf |= byte << b->len;
		b->len += 8;
	}
}

static uint32_t take(struct bits *b, int n)
{
	uint32_t v = n == 32 ? b->buf : b->buf & ((1u << n) - 1);
	b->buf = n == 32 ? 0 : b->buf >> n;
	b->len -= n;
	return v;
}

/* a copy length: n zeros, a one, then n bits; 2..4414 (0 = bad) */
static uint32_t ds_count(struct bits *b)
{
	int zeros = 0;
	refill(b);
	while (zeros < 9 && !(b->buf >> zeros & 1))
		zeros++;
	if (zeros == 9)
		return 0;
	take(b, zeros + 1);
	refill(b);
	return (1u << zeros) + take(b, zeros) + 1;
}

/* decompress one chunk; returns the bytes written (0 = damaged) */
static size_t ds_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	struct bits b = { in, in + in_len, 0, 0 };
	size_t pos = 0;
	while (pos < out_len)
	{
		uint32_t dist, n;
		refill(&b);
		switch (b.buf & 3)
		{
		case 1: /* 01: a byte with its top bit set */
			take(&b, 2);
			out[pos++] = (uint8_t) (0x80 | take(&b, 7));
			continue;
		case 2: /* 10: a byte below 0x80 */
			take(&b, 2);
			out[pos++] = (uint8_t) take(&b, 7);
			continue;
		case 0: /* 00: distance 1..63; 0 = end of chunk */
			take(&b, 2);
			dist = take(&b, 6);
			if (!dist)
				return pos;
			break;
		default:
			if ((b.buf & 7) == 3) /* 011: distance 64..319 */
			{
				take(&b, 3);
				dist = take(&b, 8) + 64;
			}
			else /* 111: distance 320..4414; 4415 = sector break */
			{
				take(&b, 3);
				dist = take(&b, 12) + 320;
				if (dist == 4415)
					continue;
			}
			break;
		}
		n = ds_count(&b);
		if (!n || dist > pos || pos + n > out_len)
			return 0;
		while (n--)
		{
			out[pos] = out[pos - dist];
			pos++;
		}
	}
	return pos;
}

/* ------------------------------------------------------------------ containers */

int vf_load(struct vxdfile *f, const uint8_t *buf, size_t len)
{
	uint32_t hdr;
	memset(f, 0, sizeof(*f));
	if (len < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
		return VF_NOTMZ;
	hdr = rd32(buf + 0x3C);
	if ((size_t) hdr + 16 > len)
		return VF_FORMAT;
	if (buf[hdr] == 'W' && buf[hdr + 1] == '4')
	{
		uint32_t chunk = rd16(buf + hdr + 4), n = rd16(buf + hdr + 6), i;
		const uint8_t *tab = buf + hdr + 16;
		size_t out = hdr;
		if (buf[hdr + 8] != 'D' || buf[hdr + 9] != 'S' || !chunk || (size_t) (tab - buf) + 4 * n > len)
			return VF_FORMAT;
		f->data = malloc(hdr + (size_t) chunk * n);
		if (!f->data)
			return VF_NOMEM;
		memcpy(f->data, buf, hdr);
		for (i = 0; i < n; i++)
		{
			uint32_t a = rd32(tab + 4 * i), e = i + 1 < n ? rd32(tab + 4 * (i + 1)) : (uint32_t) len;
			size_t got;
			if (a > e || e > len)
				goto damaged;
			if (e - a == chunk)          /* stored uncompressed */
			{
				memcpy(f->data + out, buf + a, chunk);
				got = chunk;
			}
			else if (!(got = ds_decompress(buf + a, e - a, f->data + out, chunk)) && i + 1 < n)
				goto damaged;
			out += got;
		}
		f->size = out;
		f->was_w4 = 1;
	}
	else if (buf[hdr] == 'W' && buf[hdr + 1] == '3')
	{
		if (!(f->data = malloc(len)))
			return VF_NOMEM;
		memcpy(f->data, buf, len);
		f->size = len;
	}
	else
		return VF_FORMAT;
	f->w3 = hdr;
	if (f->data[hdr] != 'W' || f->data[hdr + 1] != '3')
		goto damaged;
	f->count = rd16(f->data + hdr + 4);
	if (hdr + 16 + 16 * (size_t) f->count > f->size)
		goto damaged;
	return VF_OK;
damaged:
	vf_free(f);
	return VF_DAMAGED;
}

void vf_free(struct vxdfile *f)
{
	free(f->data);
	memset(f, 0, sizeof(*f));
}

int vf_find(const struct vxdfile *f, const char *name, struct vxd *v)
{
	int i, k;
	char want[8];
	memset(want, ' ', 8);
	for (k = 0; name[k] && k < 8; k++)
		want[k] = (char) (name[k] >= 'a' && name[k] <= 'z' ? name[k] - 32 : name[k]);
	for (i = 0; i < f->count; i++)
	{
		const uint8_t *e = f->data + f->w3 + 16 + 16 * i, *le;
		uint32_t off = rd32(e + 8), end = f->size, o, ot, pm;
		int j;
		if (memcmp(e, want, 8))
			continue;
		for (j = 0; j < f->count; j++)   /* the next VxD's start ends this one */
		{
			uint32_t x = rd32(f->data + f->w3 + 16 + 16 * j + 8);
			if (x > off && x < end)
				end = x;
		}
		if ((size_t) off + 0xC4 > f->size)
			return VF_DAMAGED;
		le = f->data + off;
		if (le[0] != 'L' || le[1] != 'E')
			return VF_DAMAGED;
		memset(v, 0, sizeof(*v));
		memcpy(v->name, e, 8);
		for (k = 7; k >= 0 && v->name[k] == ' '; k--)
			v->name[k] = 0;
		v->le = off;
		v->size = end - off;
		v->npages = rd32(le + 0x14);
		v->page_size = rd32(le + 0x28);
		v->last_page = rd32(le + 0x2C);
		ot = rd32(le + 0x40);
		v->nobj = (int) rd32(le + 0x44);
		pm = rd32(le + 0x48);
		/* in W3, "data pages from the top of the file" counts from the container's start */
		v->data_pages = rd32(le + 0x80);
		if (v->nobj < 1 || v->nobj > 64 || !v->page_size || v->page_size > 0x1000 || off + ot + 24u * v->nobj > f->size ||
		    v->data_pages + (v->npages ? (v->npages - 1) * v->page_size + v->last_page : 0) > f->size)
			return VF_DAMAGED;
		v->objtab = off + ot;
		if (off + pm + 4u * v->npages > f->size)
			return VF_DAMAGED;
		for (k = 0; k < v->nobj; k++)
		{
			uint32_t p, q;
			o = off + ot + 24 * k;
			v->obj[k].vsize = rd32(f->data + o);
			v->obj[k].flags = rd32(f->data + o + 8);
			v->obj[k].first_page = rd32(f->data + o + 12);
			v->obj[k].npages = rd32(f->data + o + 16);
			/* LE page map entry: 24-bit page number (high byte first), then flags */
			v->obj[k].phys_page = 0;
			if (!v->obj[k].npages || v->obj[k].first_page < 1 ||
			    v->obj[k].first_page + v->obj[k].npages - 1 > v->npages)
				continue;
			for (q = 0; q < v->obj[k].npages; q++)
			{
				const uint8_t *e2 = f->data + off + pm + 4 * (v->obj[k].first_page - 1 + q);
				p = (uint32_t) e2[0] << 16 | (uint32_t) e2[1] << 8 | e2[2];
				if (!q)
					v->obj[k].phys_page = p;
				else if (p != v->obj[k].phys_page + q)
					v->obj[k].phys_page = 0;
				if (!v->obj[k].phys_page)
					break;
			}
			if (v->obj[k].phys_page && v->obj[k].phys_page + v->obj[k].npages - 1 > v->npages)
				v->obj[k].phys_page = 0;
		}
		return VF_OK;
	}
	return VF_NOTFOUND;
}

uint32_t vf_obj_stored(const struct vxdfile *f, const struct vxd *v, int obj)
{
	const struct le_obj *o;
	uint32_t n;
	(void) f;
	if (obj < 1 || obj > v->nobj)
		return 0;
	o = &v->obj[obj - 1];
	if (!o->npages || !o->phys_page)
		return 0;
	n = (o->npages - 1) * v->page_size;
	n += o->phys_page + o->npages - 1 == v->npages ? v->last_page : v->page_size;
	return n < o->vsize ? n : o->vsize;
}

uint32_t vf_obj_slack(const struct vxdfile *f, const struct vxd *v, int obj)
{
	const struct le_obj *o;
	uint32_t n;
	(void) f;
	if (obj < 1 || obj > v->nobj)
		return 0;
	o = &v->obj[obj - 1];
	if (!o->npages || !o->phys_page)
		return 0;
	n = (o->npages - 1) * v->page_size;
	n += o->phys_page + o->npages - 1 == v->npages ? v->last_page : v->page_size;
	return n > o->vsize ? n - o->vsize : 0;
}

uint32_t vf_obj_offset(const struct vxdfile *f, const struct vxd *v, int obj, uint32_t off)
{
	if (off >= vf_obj_stored(f, v, obj))
		return 0;
	return v->data_pages + (v->obj[obj - 1].phys_page - 1) * v->page_size + off;
}

static int match(const uint8_t *p, const uint8_t *find, const uint8_t *mask, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++)
		if ((p[i] ^ find[i]) & (mask ? mask[i] : 0xFF))
			return 0;
	return 1;
}

/* count matches of a pattern in [base, base+len); first one in *at */
static int search(const uint8_t *base, uint32_t len, const uint8_t *find, const uint8_t *mask, uint32_t n,
                  uint32_t *at)
{
	uint32_t i;
	int hits = 0;
	if (n > len)
		return 0;
	for (i = 0; i + n <= len; i++)
		if (match(base + i, find, mask, n))
		{
			if (!hits)
				*at = i;
			hits++;
		}
	return hits;
}

/* one object; returns hits, the first one's object-relative offset in *at */
static int patch_obj(struct vxdfile *f, const struct vxd *v, int obj, const uint8_t *find, const uint8_t *mask,
                     uint32_t n, uint32_t *at)
{
	uint32_t start = vf_obj_offset(f, v, obj, 0), len = vf_obj_stored(f, v, obj);
	if (!start || !len)
		return 0;
	return search(f->data + start, len, find, mask, n, at);
}

/* search object p->obj, or every object when it is 0; exactly one hit in all */
static int locate(struct vxdfile *f, const struct vxd *v, int want, const uint8_t *find, const uint8_t *mask,
                  uint32_t n, int *obj, uint32_t *at)
{
	int o, hits = 0;
	for (o = want ? want : 1; o <= (want ? want : v->nobj); o++)
	{
		uint32_t a;
		int h = patch_obj(f, v, o, find, mask, n, &a);
		if (h && !hits)
		{
			*obj = o;
			*at = a;
		}
		hits += h;
	}
	return hits;
}

int vf_patch(struct vxdfile *f, const struct vf_patch *p, uint32_t *where)
{
	struct vxd v;
	uint32_t at = 0, i;
	uint8_t *patched, *m;
	int hits, obj = 0, rc;
	if (vf_find(f, p->vxd, &v) != VF_OK)
		return VP_NOVXD;
	hits = locate(f, &v, p->obj, p->find, p->mask, p->find_len, &obj, &at);
	if (hits == 1)
	{
		if ((int64_t) at + p->at < 0 || at + p->at + p->repl_len > vf_obj_stored(f, &v, obj))
			return VP_NOMATCH;
		memcpy(f->data + vf_obj_offset(f, &v, obj, 0) + at + p->at, p->repl, p->repl_len);
		if (where)
			*where = (uint32_t) obj << 24 | (at + p->at);
		return VP_APPLIED;
	}
	if (hits > 1)
		return VP_AMBIGUOUS;
	/* already applied? look for the pattern with the replacement laid over it */
	patched = malloc(p->find_len);
	m = malloc(p->find_len);
	rc = VP_NOMATCH;
	if (patched && m)
	{
		memcpy(patched, p->find, p->find_len);
		for (i = 0; i < p->find_len; i++)
		{
			int32_t k = (int32_t) i - p->at;
			int over = k >= 0 && (uint32_t) k < p->repl_len;
			if (over)
				patched[i] = p->repl[k];
			m[i] = over ? 0xFF : (p->mask ? p->mask[i] : 0xFF);
		}
		if (locate(f, &v, p->obj, patched, m, p->find_len, &obj, &at) == 1)
		{
			rc = VP_ALREADY;
			if (where)
				*where = (uint32_t) obj << 24 | (at + p->at);
		}
	}
	free(patched);
	free(m);
	return rc;
}
