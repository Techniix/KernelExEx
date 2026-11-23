/*
 *  KernelExEx - Elantech touchpads, hardware versions 2, 3 and 4.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Ported from Linux drivers/input/mouse/elantech.c (GPL-2.0-only):
 *  Copyright (C) 2007-2009 Arjan Opmeer; trademarks are the property of their
 *  respective owners. Not ported: hardware version 1 (2005 models), the
 *  TrackPoint packets of v3/v4 (Lenovo), SMBus/host-notify mode.
 */
#include "tp.h"
#include <string.h>

#define CMD(t, p, c) tp_ps2_command(t, p, c)

#define FW_ID_QUERY 0x00
#define FW_VERSION_QUERY 0x01
#define CAPABILITIES_QUERY 0x02
#define REG_READ 0x10
#define REG_WRITE 0x11
#define REG_READWRITE 0x00
#define CUSTOM 0xf8

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

/* a raw byte to the pad, expecting its ACK (a "command" with no parameters) */
static int raw(struct tp *t, u8 b)
{
	int i;
	for (i = 0; i < 3; i++)   /* as Linux elantech_ps2_command: retried */
		if (CMD(t, NULL, b) == 0)
			return 0;
	return -1;
}

static int send_cmd(struct tp *t, u8 c, u8 *p)
{
	if (t->elan.hw >= 3)
	{
		if (raw(t, CUSTOM) || raw(t, c))
			return -1;
	}
	else if (sliced(t, c))
		return -1;
	return CMD(t, p, PS2_GETINFO) ? -1 : 0;
}

static int read_reg(struct tp *t, u8 reg, u8 *val)
{
	u8 p[3] = { 0, 0, 0 };
	int hw = t->elan.hw;
	if (raw(t, CUSTOM) || raw(t, hw == 2 ? REG_READ : REG_READWRITE) || raw(t, CUSTOM) || raw(t, reg) ||
	    CMD(t, p, PS2_GETINFO))
		return -1;
	*val = hw == 4 ? p[1] : p[0];
	return 0;
}

static int write_reg(struct tp *t, u8 reg, u8 val)
{
	int hw = t->elan.hw;
	if (raw(t, CUSTOM) || raw(t, hw == 2 ? REG_WRITE : REG_READWRITE) || raw(t, CUSTOM) || raw(t, reg))
		return -1;
	if (hw == 4 && (raw(t, CUSTOM) || raw(t, REG_READWRITE)))
		return -1;
	if (raw(t, CUSTOM) || raw(t, val) || CMD(t, NULL, PS2_SETSCALE11))
		return -1;
	return 0;
}

static int signature_valid(const u8 *p)
{
	static const u8 rates[] = { 200, 100, 80, 60, 40, 20, 10 };
	unsigned i;
	if (p[0] == 0)
		return 0;
	if (p[1] == 0)
		return 1;
	if ((p[0] & 0x0f) >= 0x06 && (p[1] & 0xaf) == 0x0f && p[2] < 40)
		return 1;
	for (i = 0; i < sizeof(rates); i++)
		if (p[2] == rates[i])
			return 0;
	return 1;
}

int tp_elan_detect(struct tp *t)
{
	struct tp_elan *e = &t->elan;
	u8 p[3] = { 0, 0, 0 };
	int ic;
	CMD(t, NULL, PS2_RESET_DIS);
	if (CMD(t, NULL, PS2_DISABLE) || CMD(t, NULL, PS2_SETSCALE11) || CMD(t, NULL, PS2_SETSCALE11) ||
	    CMD(t, NULL, PS2_SETSCALE11) || CMD(t, p, PS2_GETINFO))
		return -1;
	if (p[0] != 0x3c || p[1] != 0x03 || (p[2] != 0xc8 && p[2] != 0x00))
		return -1;
	memset(e, 0, sizeof(*e));
	e->hw = 2;   /* the version query is a sliced command on every version */
	if (send_cmd(t, FW_VERSION_QUERY, p) || !signature_valid(p))
		return -1;
	e->fw = ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2];
	ic = (int) ((e->fw >> 16) & 0x0f);
	if (e->fw < 0x020030 || e->fw == 0x020600)
		e->hw = 1;
	else if (ic == 2 || ic == 4)
		e->hw = 2;
	else if (ic == 5)
		e->hw = 3;
	else if (ic >= 6)
		e->hw = 4;
	else
		e->hw = 0;
	tp_log(t, "elantech: firmware %06x, hardware version %d\n", e->fw, e->hw);
	if (e->hw < 2)
		return -1;   /* version 1: not ported, IntelliMouse mode */
	e->crc = (e->fw & 0x4000) != 0;
	e->pressure = e->fw >= 0x020800;
	e->buttonpad = (e->fw & 0x001000) != 0;
	if (send_cmd(t, CAPABILITIES_QUERY, e->caps))
		return -1;
	if (e->hw == 2)
	{
		if (e->fw == 0x020800 || e->fw == 0x020b00 || e->fw == 0x020030)
			t->x_max = 1152, t->y_max = 768;
		else if (e->fw == 0x040216)
			t->x_max = 819, t->y_max = 405;
		else if (e->fw == 0x040219 || e->fw == 0x040215)
			t->x_max = 900, t->y_max = 500;
		else
		{
			int i = (e->fw > 0x020800 && e->fw < 0x020900) ? 1 : 2;
			t->x_max = (e->caps[1] - i) * 64;
			t->y_max = (e->caps[2] - i) * 64;
		}
	}
	else
	{
		if (send_cmd(t, FW_ID_QUERY, p))
			return -1;
		t->x_max = ((p[0] & 0x0f) << 8) | p[1];
		t->y_max = ((p[0] & 0xf0) << 4) | p[2];
	}
	if (t->x_max <= 0 || t->y_max <= 0)
		return -1;
	e->y_max = t->y_max;
	t->pktsize = 6;
	{
		static const char hex[] = "0123456789abcdef";
		char *n = t->name;
		const char *c = "Elantech v";
		int i;
		while (*c)
			*n++ = *c++;
		*n++ = (char) ('0' + e->hw);
		for (c = " fw="; *c;)
			*n++ = *c++;
		for (i = 20; i >= 0; i -= 4)
			*n++ = hex[(e->fw >> i) & 15];
		if (e->buttonpad)
			for (c = " clickpad"; *c;)
				*n++ = *c++;
		*n = 0;
	}
	return 0;
}

int tp_elan_init(struct tp *t)
{
	struct tp_elan *e = &t->elan;
	u8 v;
	int tries, r = 0;
	switch (e->hw)
	{
	case 2:   /* the Windows driver's values (Linux) */
		r = write_reg(t, 0x10, 0x54) || write_reg(t, 0x11, 0x88) || write_reg(t, 0x21, 0x60);
		break;
	case 3:
		r = write_reg(t, 0x10, 0x0b);
		break;
	case 4:
		r = write_reg(t, 0x07, 0x01);
		break;
	}
	if (r)
		return -1;
	if (e->hw != 4)
	{
		/* the pad is ready when it reads back register 0x10 */
		for (tries = 0; tries < 5; tries++)
			if (read_reg(t, 0x10, &v) == 0)
				break;
		if (tries == 5)
			return -1;
	}
	return CMD(t, NULL, PS2_ENABLE) ? -1 : 0;
}

/* whole packets only: Elantech's checks need all six bytes */
int tp_elan_valid_byte(struct tp *t, int idx, u8 b)
{
	(void) t;
	(void) idx;
	(void) b;
	return 1;
}

enum { P_BAD, P_SKIP, P_V3_HEAD, P_V3_TAIL, P_V4_STATUS, P_V4_HEAD, P_V4_MOTION, P_OK };

static int packet_type(struct tp *t)
{
	struct tp_elan *e = &t->elan;
	const u8 *p = t->packet;
	static const u8 deb2[6] = { 0x84, 0xff, 0xff, 0x02, 0xff, 0xff }, deb3[6] = { 0xc4, 0xff, 0xff, 0x02, 0xff, 0xff };
	if (e->hw == 2)
	{
		if (!memcmp(p, deb2, 6))
			return P_SKIP;
		if (e->pressure)
			return (p[0] & 0x0c) == 0x04 && (p[3] & 0x0f) == 0x02 ? P_OK : P_BAD;
		if ((p[0] & 0xc0) == 0x80)
			return (p[0] & 0x0c) == 0x0c && (p[3] & 0x0e) == 0x08 ? P_OK : P_BAD;
		return (p[0] & 0x3c) == 0x3c && (p[1] & 0xf0) == 0 && (p[3] & 0x3e) == 0x38 && (p[4] & 0xf0) == 0 ? P_OK : P_BAD;
	}
	if (e->hw == 3)
	{
		if (!memcmp(p, deb3, 6))
			return P_SKIP;
		if (e->crc)
		{
			if ((p[3] & 0x09) == 0x08)
				return P_V3_HEAD;
			if ((p[3] & 0x09) == 0x09)
				return P_V3_TAIL;
		}
		else
		{
			if ((p[0] & 0x0c) == 0x04 && (p[3] & 0xcf) == 0x02)
				return P_V3_HEAD;
			if ((p[0] & 0x0c) == 0x0c && (p[3] & 0xce) == 0x0c)
				return P_V3_TAIL;
			if ((p[3] & 0x0f) == 0x06)
				return P_SKIP;   /* a TrackPoint packet: not ported */
		}
		return P_BAD;
	}
	/* v4 */
	if ((p[3] & 0x0f) == 0x06 && !e->crc)
		return P_SKIP;
	if (e->crc ? (p[3] & 0x08) != 0 : ((p[0] & 0x08) != 0 || (p[3] & 0x1c) != 0x10))
		return P_BAD;
	switch (p[3] & 3)
	{
	case 0: return P_V4_STATUS;
	case 1: return P_V4_HEAD;
	case 2: return P_V4_MOTION;
	}
	return P_BAD;
}

/* 1: frame filled; 0: nothing to report; -1: not a valid packet (resync) */
int tp_elan_packet(struct tp *t, struct tp_frame *f)
{
	struct tp_elan *e = &t->elan;
	const u8 *p = t->packet;
	int type = packet_type(t), i;
	memset(f, 0, sizeof(*f));
	if (type == P_BAD)
		return -1;
	if (type == P_SKIP)
		return 0;
	if (e->buttonpad)
		f->left = (p[0] & 3) != 0;
	else
	{
		f->left = (p[0] & 1) != 0;
		f->right = (p[0] & 2) != 0;
		f->middle = (p[0] & 4) != 0;
	}
	if (e->hw == 2)
	{
		int n = (p[0] & 0xc0) >> 6;
		if (n == 3 && (p[3] & 0x80))
			n = 4;
		if (n == 2)
		{
			f->x = ((((p[0] & 0x10) << 4) | p[1]) << 2);
			f->y = e->y_max - ((((p[0] & 0x20) << 3) | p[2]) << 2);
			f->x2 = ((((p[3] & 0x10) << 4) | p[4]) << 2);
			f->y2 = e->y_max - ((((p[3] & 0x20) << 3) | p[5]) << 2);
		}
		else if (n)
		{
			f->x = ((p[1] & 0x0f) << 8) | p[2];
			f->y = e->y_max - (((p[4] & 0x0f) << 8) | p[5]);
			f->width = ((p[0] & 0x30) >> 2) | ((p[3] & 0x30) >> 4);
		}
		f->fingers = n;
		f->z = n ? 40 : 0;
		f->width = 0;   /* v2 widths are not comparable to the palm threshold */
		return 1;
	}
	if (e->hw == 3)
	{
		int n = (p[0] & 0xc0) >> 6;
		if (n == 2)
		{
			if (type == P_V3_HEAD)
			{
				e->mx[0] = ((p[1] & 0x0f) << 8) | p[2];
				e->my[0] = e->y_max - (((p[4] & 0x0f) << 8) | p[5]);
				return 0;   /* the tail packet completes it */
			}
			f->x = e->mx[0];
			f->y = e->my[0];
			f->x2 = ((p[1] & 0x0f) << 8) | p[2];
			f->y2 = e->y_max - (((p[4] & 0x0f) << 8) | p[5]);
		}
		else if (n)
		{
			f->x = ((p[1] & 0x0f) << 8) | p[2];
			f->y = e->y_max - (((p[4] & 0x0f) << 8) | p[5]);
		}
		f->fingers = n;
		f->z = n ? 40 : 0;
		return 1;
	}
	/* v4: up to five fingers by id */
	if (type == P_V4_STATUS)
	{
		e->active = p[1] & 0x1f;
		e->known &= e->active;   /* a lifted finger's position is forgotten */
	}
	else if (type == P_V4_HEAD)
	{
		int id = ((p[3] & 0xe0) >> 5) - 1;
		if (id < 0 || id >= 5)
			return 0;
		e->mx[id] = ((p[1] & 0x0f) << 8) | p[2];
		e->my[id] = e->y_max - (((p[4] & 0x0f) << 8) | p[5]);
		e->active |= (u8) (1 << id);
		e->known |= (u8) (1 << id);
	}
	else
	{
		int id = ((p[0] & 0xe0) >> 5) - 1, sid = ((p[3] & 0xe0) >> 5) - 1, w = (p[0] & 0x10) ? 5 : 1;
		if (id < 0 || id >= 5)
			return 0;
		e->mx[id] += (s8) p[1] * w;
		e->my[id] -= (s8) p[2] * w;
		if (sid >= 0 && sid < 5)
		{
			e->mx[sid] += (s8) p[4] * w;
			e->my[sid] -= (s8) p[5] * w;
		}
	}
	/* a finger counts once its head packet gave its position (status packets
	 * announce it earlier; its old position would look like a jump) */
	if (e->active & ~e->known)
		return 0;
	{
		int first = -1, second = -1;
		for (i = 0; i < 5; i++)
			if (e->active & (1 << i))
			{
				f->fingers++;
				if (first < 0)
					first = i;
				else if (second < 0)
					second = i;
			}
		if (first >= 0)
		{
			f->x = e->mx[first];
			f->y = e->my[first];
		}
		if (second >= 0)
		{
			f->x2 = e->mx[second];
			f->y2 = e->my[second];
		}
		f->z = f->fingers ? 40 : 0;
	}
	return 1;
}
