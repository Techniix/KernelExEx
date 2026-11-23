/*
 *  KernelExEx - Synaptics touchpads (absolute mode with W, multi-finger count,
 *  ClickPads, pass-through TrackPoint). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Ported from Linux drivers/input/mouse/synaptics.c (GPL-2.0-only):
 *  Copyright (c) 2003 Peter Osterlund, 2003 Neil Brown; based on the Synaptics
 *  TouchPad Interfacing Guide.
 */
#include "tp.h"
#include <string.h>

#define CMD(t, p, c) tp_ps2_command(t, p, c)

#define QUE_IDENTIFY 0x00
#define QUE_CAPABILITIES 0x02
#define QUE_MODEL 0x03
#define QUE_EXT_CAPAB_0C 0x0c
#define BIT_ABSOLUTE 0x80
#define BIT_HIGH_RATE 0x40
#define BIT_W_MODE 0x01
#define PS_SET_MODE2 0x14
#define PS_CLIENT_CMD 0x28

#define XMIN 1472
#define XMAX 5472
#define YMIN 1408
#define YMAX 4448
#define X_MAX_POSITIVE 8176
#define Y_MAX_POSITIVE 8176
#define ABS_POS_BITS 13

/* a byte to the touchpad as four 2-bit SETRES arguments (Linux ps2_sliced_command) */
static int sliced(struct tp *t, u8 c)
{
	int i;
	if (CMD(t, NULL, PS2_SETSCALE11))
		return -1;
	for (i = 6; i >= 0; i -= 2)
	{
		u8 d = (u8) ((c >> i) & 3);
		if (CMD(t, &d, PS2_SETRES))
			return -1;
	}
	return 0;
}

static int query(struct tp *t, u8 q, u32 *v)
{
	u8 p[3] = { 0, 0, 0 };
	if (sliced(t, q) || CMD(t, p, PS2_GETINFO))
		return -1;
	*v = ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2];
	return 0;
}

static int mode_cmd(struct tp *t, u8 mode)
{
	u8 p = PS_SET_MODE2;
	if (sliced(t, mode))
		return -1;
	return CMD(t, &p, PS2_SETRATE) ? -1 : 0;
}

int tp_syn_detect(struct tp *t)
{
	u8 p[4] = { 0, 0, 0, 0 };
	struct tp_syn *s = &t->syn;
	CMD(t, p, PS2_SETRES);
	CMD(t, p, PS2_SETRES);
	CMD(t, p, PS2_SETRES);
	CMD(t, p, PS2_SETRES);
	p[0] = p[1] = p[2] = 0;
	if (CMD(t, p, PS2_GETINFO) || p[1] != 0x47)
		return -1;
	memset(s, 0, sizeof(*s));
	if (query(t, QUE_IDENTIFY, &s->identity) || ((s->identity >> 8) & 0xff) != 0x47)
		return -1;
	if (query(t, QUE_MODEL, &s->model) || query(t, QUE_CAPABILITIES, &s->caps))
		return -1;
	if (!(s->caps & (1u << 23)))   /* no extended capabilities: old pad */
		s->caps = 0;
	if (((s->caps >> 20) & 7) >= 4)
		query(t, QUE_EXT_CAPAB_0C, &s->ext0c);
	s->newabs = (s->model & 0x80) != 0;
	t->x_max = XMAX - XMIN;
	t->y_max = YMAX - YMIN;
	t->has_stick = (s->caps & (1u << 7)) != 0;   /* pass-through port */
	t->pktsize = 6;
	{
		static const char hex[] = "0123456789abcdef";
		char *n = t->name;
		const char *c = "Synaptics v";
		int major = (int) (s->identity & 0x0f), minor = (int) ((s->identity >> 16) & 0xff);
		while (*c)
			*n++ = *c++;
		*n++ = (char) ('0' + major % 10);
		*n++ = '.';
		if (minor >= 10)
			*n++ = (char) ('0' + minor / 10 % 10);
		*n++ = (char) ('0' + minor % 10);
		for (c = " caps="; *c;)
			*n++ = *c++;
		{
			int i;
			for (i = 20; i >= 0; i -= 4)
				*n++ = hex[(s->caps >> i) & 15];
		}
		if (s->ext0c & (1u << 20))
			for (c = " clickpad"; *c;)
				*n++ = *c++;
		if (t->has_stick)
			for (c = " +stick"; *c;)
				*n++ = *c++;
		*n = 0;
	}
	tp_log(t, "synaptics: identity %06x model %06x caps %06x ext0c %06x\n", s->identity, s->model, s->caps, s->ext0c);
	return 0;
}

int tp_syn_init(struct tp *t)
{
	struct tp_syn *s = &t->syn;
	u8 mode = BIT_ABSOLUTE | BIT_HIGH_RATE;
	if (s->caps & (1u << 23))
		mode |= BIT_W_MODE;
	if (mode_cmd(t, mode))
		return -1;
	if (t->has_stick)
	{
		/* enable the stick behind the pass-through port (a "client command") */
		u8 p = PS_CLIENT_CMD;
		if (sliced(t, 0xf4) || CMD(t, &p, PS2_SETRATE))
			t->has_stick = 0;
	}
	return CMD(t, NULL, PS2_ENABLE) ? -1 : 0;
}

/* plausible byte idx of a packet (Linux relaxed NEWABS / OLDABS validation) */
int tp_syn_valid_byte(struct tp *t, int idx, u8 b)
{
	static const u8 newabs_mask[] = { 0xc0, 0x00, 0x00, 0xc0, 0x00, 0x00 };
	static const u8 newabs_rslt[] = { 0x80, 0x00, 0x00, 0xc0, 0x00, 0x00 };
	static const u8 oldabs_mask[] = { 0xc0, 0x60, 0x00, 0xc0, 0x60, 0x00 };
	static const u8 oldabs_rslt[] = { 0xc0, 0x00, 0x00, 0x80, 0x00, 0x00 };
	if (idx < 0 || idx > 5)
		return 0;
	if (t->syn.newabs)
		return (b & newabs_mask[idx]) == newabs_rslt[idx];
	return (b & oldabs_mask[idx]) == oldabs_rslt[idx];
}

int tp_syn_packet(struct tp *t, struct tp_frame *f, struct tp_out *st, int *have_stick, int *have_frame)
{
	struct tp_syn *s = &t->syn;
	const u8 *b = t->packet;
	int x, y, z, w;
	*have_stick = *have_frame = 0;
	memset(f, 0, sizeof(*f));
	memset(st, 0, sizeof(*st));

	/* the pass-through stick: a PS/2 packet in bytes 1, 4, 5 */
	if ((s->caps & (1u << 7)) && (b[0] & 0xfc) == 0x84 && (b[3] & 0xcc) == 0xc4)
	{
		u8 p0 = b[1];
		st->buttons = p0 & 7;
		st->dx = (p0 & 0x10) ? (int) b[4] - 256 : b[4];
		st->dy = -((p0 & 0x20) ? (int) b[5] - 256 : b[5]);
		*have_stick = 1;
		return 6;
	}
	if (s->newabs)
	{
		w = ((b[0] & 0x30) >> 2) | ((b[0] & 0x04) >> 1) | ((b[3] & 0x04) >> 2);
		x = ((b[3] & 0x10) << 8) | ((b[1] & 0x0f) << 8) | b[4];
		y = ((b[3] & 0x20) << 7) | ((b[1] & 0xf0) << 4) | b[5];
		z = b[2];
		f->left = (b[0] & 1) != 0;
		f->right = (b[0] & 2) != 0;
		if (s->ext0c & (1u << 20))
			f->left |= ((b[0] ^ b[3]) & 1) != 0;   /* ClickPad: the pad is the button */
		else if (s->caps & (1u << 18))
			f->middle = ((b[0] ^ b[3]) & 1) != 0;
	}
	else
	{
		x = ((b[1] & 0x1f) << 8) | b[2];
		y = ((b[4] & 0x1f) << 8) | b[5];
		z = ((b[0] & 0x30) << 2) | (b[3] & 0x3f);
		w = ((b[1] & 0x80) >> 4) | ((b[0] & 0x04) >> 1);
		f->left = (b[0] & 1) != 0;
		f->right = (b[0] & 2) != 0;
	}
	if (x > X_MAX_POSITIVE)
		x -= 1 << ABS_POS_BITS;
	else if (x == X_MAX_POSITIVE)
		x = XMAX;
	if (y > Y_MAX_POSITIVE)
		y -= 1 << ABS_POS_BITS;
	else if (y == Y_MAX_POSITIVE)
		y = YMAX;

	if (z > 0 && x > 1)
	{
		f->fingers = 1;
		if (s->caps & (1u << 23))
		{
			if (w <= 1 && (s->caps & 2))   /* multi-finger: W 0 = two, 1 = three fingers */
				f->fingers = w + 2;
			else if (w >= 4 && (s->caps & 1))
				f->width = w;              /* palm detection: 4..15 is the contact width */
		}
	}
	/* to our convention: 0..max, y grows downward */
	x -= XMIN;
	y = YMAX - y;
	f->x = x < 0 ? 0 : x > t->x_max ? t->x_max : x;
	f->y = y < 0 ? 0 : y > t->y_max ? t->y_max : y;
	f->z = z;
	*have_frame = 1;
	return 6;
}
