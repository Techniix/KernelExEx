/*
 *  KernelExEx - ALPS touchpads and DualPoint sticks (protocols v1, v2, v3 Pinnacle,
 *  v3 Rushmore, v4, v5 Dolphin). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Ported from Linux drivers/input/mouse/alps.c (GPL-2.0-only):
 *  Copyright (c) 2003 Neil Brown, 2003-2005 Peter Osterlund, 2004 Dmitry Torokhov,
 *  2005 Vojtech Pavlik, 2009 Sebastian Kapfer; v3-v5 by Seth Forshee and others.
 *  Not ported yet: v6 (Dell XT2), v7, v8 (SS4), v9 (2013+ models): they fall back
 *  to IntelliMouse mode.
 */
#include "tp.h"
#include <string.h>

#define PROTO_V1 0x100
#define PROTO_V2 0x200
#define PROTO_V3 0x300
#define PROTO_V3_RUSHMORE 0x310
#define PROTO_V4 0x400
#define PROTO_V5 0x500
#define PROTO_V6 0x600

#define DUALPOINT 0x02
#define PASS 0x04
#define WHEEL 0x08
#define FW_BK_1 0x10
#define FW_BK_2 0x20
#define FOUR_BUTTONS 0x40
#define PS2_INTERLEAVED 0x80
#define QUIRK_TRACKSTICK_BUTTONS 1

#define REG_BASE_RUSHMORE 0xc2c0
#define REG_BASE_PINNACLE 0x0000
#define DOLPHIN_COUNT_PER_ELECTRODE 64
#define DOLPHIN_PROFILE_XOFFSET 8
#define DOLPHIN_PROFILE_YOFFSET 1

struct model
{
	u8 sig[3];
	int proto;
	u8 byte0, mask0;
	int flags;
};

static const struct model models[] = {
	{ { 0x20, 0x02, 0x0e }, PROTO_V2, 0xf8, 0xf8, PASS | DUALPOINT },
	{ { 0x22, 0x02, 0x0a }, PROTO_V2, 0xf8, 0xf8, PASS | DUALPOINT },
	{ { 0x22, 0x02, 0x14 }, PROTO_V2, 0xff, 0xff, PASS | DUALPOINT },   /* Dell Latitude D600 */
	{ { 0x32, 0x02, 0x14 }, PROTO_V2, 0xf8, 0xf8, PASS | DUALPOINT },   /* Toshiba Satellite Pro M10 */
	{ { 0x33, 0x02, 0x0a }, PROTO_V1, 0x88, 0xf8, 0 },                  /* UMAX-530T */
	{ { 0x52, 0x01, 0x14 }, PROTO_V2, 0xff, 0xff, PASS | DUALPOINT | PS2_INTERLEAVED },   /* Toshiba Tecra A11-11L */
	{ { 0x53, 0x02, 0x0a }, PROTO_V2, 0xf8, 0xf8, 0 },
	{ { 0x53, 0x02, 0x14 }, PROTO_V2, 0xf8, 0xf8, 0 },
	{ { 0x60, 0x03, 0xc8 }, PROTO_V2, 0xf8, 0xf8, 0 },                  /* HP ze1115 */
	{ { 0x62, 0x02, 0x14 }, PROTO_V2, 0xcf, 0xcf, PASS | DUALPOINT | PS2_INTERLEAVED },   /* Dell Latitude E5500, E6400, E6500 */
	{ { 0x63, 0x02, 0x0a }, PROTO_V2, 0xf8, 0xf8, 0 },
	{ { 0x63, 0x02, 0x14 }, PROTO_V2, 0xf8, 0xf8, 0 },
	{ { 0x63, 0x02, 0x28 }, PROTO_V2, 0xf8, 0xf8, FW_BK_2 },            /* Fujitsu Siemens S6010 */
	{ { 0x63, 0x02, 0x3c }, PROTO_V2, 0x8f, 0x8f, WHEEL },              /* Toshiba Satellite S2400-103 */
	{ { 0x63, 0x02, 0x50 }, PROTO_V2, 0xef, 0xef, FW_BK_1 },            /* NEC Versa L320 */
	{ { 0x63, 0x02, 0x64 }, PROTO_V2, 0xf8, 0xf8, 0 },
	{ { 0x63, 0x03, 0xc8 }, PROTO_V2, 0xf8, 0xf8, PASS | DUALPOINT },   /* Dell Latitude D800 */
	{ { 0x73, 0x00, 0x0a }, PROTO_V2, 0xf8, 0xf8, DUALPOINT },          /* ThinkPad R61 8918-5QG */
	{ { 0x73, 0x00, 0x14 }, PROTO_V6, 0xff, 0xff, DUALPOINT },          /* Dell XT2 (not supported yet) */
	{ { 0x73, 0x02, 0x0a }, PROTO_V2, 0xf8, 0xf8, 0 },
	{ { 0x73, 0x02, 0x14 }, PROTO_V2, 0xf8, 0xf8, FW_BK_2 },            /* Ahtec Laptop */
	{ { 0x73, 0x02, 0x50 }, PROTO_V2, 0xcf, 0xcf, FOUR_BUTTONS },       /* Dell Vostro 1400 */
};

/* nibble -> PS/2 command, for command-mode register access (v3, v5 / v4 tables) */
static const u16 nib_v3_cmd[16] = { PS2_SETPOLL, PS2_RESET_DIS, PS2_SETSCALE21, PS2_SETRATE, PS2_SETRATE, PS2_SETRATE,
	                                PS2_SETRATE, PS2_SETRATE, PS2_SETRATE, PS2_SETRATE, PS2_GETID1, PS2_SETRES,
	                                PS2_SETRES, PS2_SETRES, PS2_SETRES, PS2_SETSCALE11 };
static const u16 nib_v4_cmd[16] = { PS2_ENABLE, PS2_RESET_DIS, PS2_SETSCALE21, PS2_SETRATE, PS2_SETRATE, PS2_SETRATE,
	                                PS2_SETRATE, PS2_SETRATE, PS2_SETRATE, PS2_SETRATE, PS2_GETID1, PS2_SETRES,
	                                PS2_SETRES, PS2_SETRES, PS2_SETRES, PS2_SETSCALE11 };
static const u8 nib_data[16] = { 0, 0, 0, 0x0a, 0x14, 0x28, 0x3c, 0x50, 0x64, 0xc8, 0, 0, 1, 2, 3, 0 };

#define CMD(t, p, c) tp_ps2_command(t, p, c)

static int send_nibble(struct tp *t, int nibble)
{
	struct tp_alps *a = &t->alps;
	u8 p[4];
	int cmd = a->nibble_cmd[nibble & 0xf];
	p[0] = nib_data[nibble & 0xf];
	return CMD(t, p, cmd) ? -1 : 0;
}

static int set_addr(struct tp *t, int addr)
{
	int i;
	if (CMD(t, NULL, t->alps.addr_command))
		return -1;
	for (i = 12; i >= 0; i -= 4)
		if (send_nibble(t, (addr >> i) & 0xf))
			return -1;
	return 0;
}

static int read_reg_cur(struct tp *t, int addr)
{
	u8 p[4];
	if (CMD(t, p, PS2_GETINFO))
		return -1;
	if (addr != ((p[0] << 8) | p[1]))
		return -1;
	return p[2];
}

static int read_reg(struct tp *t, int addr)
{
	if (set_addr(t, addr))
		return -1;
	return read_reg_cur(t, addr);
}

static int write_reg_cur(struct tp *t, u8 v)
{
	if (send_nibble(t, (v >> 4) & 0xf) || send_nibble(t, v & 0xf))
		return -1;
	return 0;
}

static int write_reg(struct tp *t, int addr, u8 v)
{
	if (set_addr(t, addr))
		return -1;
	return write_reg_cur(t, v);
}

/* init_command (if any), then repeated_command three times, then GETINFO */
static int rpt_cmd(struct tp *t, int init_command, int repeated_command, u8 *p)
{
	p[0] = 0;
	if (init_command && CMD(t, p, init_command))
		return -1;
	if (CMD(t, NULL, repeated_command) || CMD(t, NULL, repeated_command) || CMD(t, NULL, repeated_command))
		return -1;
	p[0] = p[1] = p[2] = 0xff;
	if (CMD(t, p, PS2_GETINFO))
		return -1;
	return 0;
}

static int valid_fw_id(const u8 *id)
{
	if (id[0] == 0x73)
		return 1;
	return id[0] == 0x88 && (id[1] == 0x07 || id[1] == 0x08 || (id[1] & 0xf0) == 0xb0 || (id[1] & 0xf0) == 0xc0);
}

static int enter_cmd(struct tp *t)
{
	u8 p[4];
	if (rpt_cmd(t, 0, PS2_RESET_WRAP, p) || !valid_fw_id(p))
	{
		tp_log(t, "alps: cannot enter command mode\n");
		return -1;
	}
	return 0;
}

static int exit_cmd(struct tp *t)
{
	return CMD(t, NULL, PS2_SETSTREAM) ? -1 : 0;
}

static int passthrough_v2(struct tp *t, int enable)
{
	int cmd = enable ? PS2_SETSCALE21 : PS2_SETSCALE11;
	if (CMD(t, NULL, cmd) || CMD(t, NULL, cmd) || CMD(t, NULL, cmd) || CMD(t, NULL, PS2_DISABLE))
		return -1;
	return 0;
}

static int tap_mode(struct tp *t, int enable)
{
	int cmd = enable ? PS2_SETRATE : PS2_SETRES;
	u8 arg = enable ? 0x0a : 0x00, p[4];
	if (CMD(t, p, PS2_GETINFO) || CMD(t, NULL, PS2_DISABLE) || CMD(t, NULL, PS2_DISABLE) || CMD(t, &arg, cmd))
		return -1;
	return rpt_cmd(t, 0, PS2_DISABLE, p);
}

static int hw_init_v1_v2(struct tp *t)
{
	struct tp_alps *a = &t->alps;
	if ((a->flags & PASS) && passthrough_v2(t, 1))
		return -1;
	/* hardware tapping stays on (as Linux does): our gesture code also accepts it */
	if (tap_mode(t, 1))
		tp_log(t, "alps: could not enable hardware tapping\n");
	if (CMD(t, NULL, PS2_DISABLE) || CMD(t, NULL, PS2_DISABLE) || CMD(t, NULL, PS2_DISABLE) ||
	    CMD(t, NULL, PS2_DISABLE) || CMD(t, NULL, PS2_ENABLE) || CMD(t, NULL, PS2_SETPOLL))
	{
		tp_log(t, "alps: could not enable absolute mode\n");
		return -1;
	}
	if ((a->flags & PASS) && passthrough_v2(t, 0))
		return -1;
	return CMD(t, NULL, PS2_SETSTREAM) ? -1 : 0;
}

static int stick_extended_mode(struct tp *t)
{
	u8 p[2] = { 0xc8, 0x14 };
	if (CMD(t, NULL, PS2_SETSCALE11) || CMD(t, NULL, PS2_SETSCALE11) || CMD(t, NULL, PS2_SETSCALE11) ||
	    CMD(t, &p[0], PS2_SETRATE) || CMD(t, &p[1], PS2_SETRATE))
		return -1;
	return 0;
}

static int passthrough_v3(struct tp *t, int base, int enable)
{
	int v, r = -1;
	if (enter_cmd(t))
		return -1;
	v = read_reg(t, base + 0x0008);
	if (v != -1)
		r = write_reg_cur(t, (u8) (enable ? v | 1 : v & ~1));
	if (exit_cmd(t))
		r = -1;
	return r;
}

static int probe_stick_v3(struct tp *t, int base)
{
	int v, r = -1;
	if (enter_cmd(t) == 0)
	{
		v = read_reg(t, base + 0x08);
		if (v != -1)
			r = (v & 0x80) ? 0 : -2;
	}
	exit_cmd(t);
	return r;
}

/* -1: I/O error, -2: no stick */
static int setup_stick_v3(struct tp *t, int base)
{
	u8 p[4];
	int r = 0, v;
	if (passthrough_v3(t, base, 1))
		return -1;
	if (rpt_cmd(t, 0, PS2_SETSCALE21, p))
		r = -2;
	else if (stick_extended_mode(t))
		r = -1;
	if (passthrough_v3(t, base, 0))
		return -1;
	if (r)
		return r;
	if (enter_cmd(t))
		return -1;
	v = read_reg(t, base + 0x08);
	if (v == -1 || write_reg_cur(t, (u8) (v | 2)))
		r = -1;
	if (exit_cmd(t))
		return -1;
	return r;
}

static int hw_init_v3(struct tp *t)
{
	u8 p[4];
	int v;
	if ((t->alps.flags & DUALPOINT) && setup_stick_v3(t, REG_BASE_PINNACLE) == -1)
		goto error;
	if (enter_cmd(t))
		goto error;
	v = read_reg(t, 0x0004);   /* absolute mode */
	if (v == -1 || write_reg_cur(t, (u8) (v | 0x06)))
		goto error;
	v = read_reg(t, 0x0006);
	if (v == -1 || write_reg_cur(t, (u8) (v | 0x01)))
		goto error;
	v = read_reg(t, 0x0007);
	if (v == -1 || write_reg_cur(t, (u8) (v | 0x01)))
		goto error;
	if (read_reg(t, 0x0144) == -1 || write_reg_cur(t, 0x04))
		goto error;
	if (read_reg(t, 0x0159) == -1 || write_reg_cur(t, 0x03))
		goto error;
	if (read_reg(t, 0x0163) == -1 || write_reg(t, 0x0163, 0x03))
		goto error;
	if (read_reg(t, 0x0162) == -1 || write_reg(t, 0x0162, 0x04))
		goto error;
	exit_cmd(t);
	p[0] = 0x64;
	if (CMD(t, p, PS2_SETRATE) || CMD(t, NULL, PS2_ENABLE))
		return -1;
	return 0;
error:
	exit_cmd(t);
	return -1;
}

static int hw_init_rushmore(struct tp *t)
{
	int v;
	if ((t->alps.flags & DUALPOINT) && setup_stick_v3(t, REG_BASE_RUSHMORE) == -1)
		goto error;
	if (enter_cmd(t) || read_reg(t, 0xc2d9) == -1 || write_reg(t, 0xc2cb, 0x00))
		goto error;
	v = read_reg(t, 0xc2c6);
	if (v == -1 || write_reg_cur(t, (u8) (v & 0xfd)))
		goto error;
	if (write_reg(t, 0xc2c9, 0x64))
		goto error;
	v = read_reg(t, 0xc2c4);
	if (v == -1 || write_reg_cur(t, (u8) (v | 0x02)))
		goto error;
	exit_cmd(t);
	return CMD(t, NULL, PS2_ENABLE) ? -1 : 0;
error:
	exit_cmd(t);
	return -1;
}

static int hw_init_v4(struct tp *t)
{
	u8 p[4];
	int v;
	if (enter_cmd(t))
		goto error;
	v = read_reg(t, 0x0004);
	if (v == -1 || write_reg_cur(t, (u8) (v | 0x02)))
		goto error;
	if (write_reg(t, 0x0007, 0x8c) || write_reg(t, 0x0149, 0x03) || write_reg(t, 0x0160, 0x03) ||
	    write_reg(t, 0x017f, 0x15) || write_reg(t, 0x0151, 0x01) || write_reg(t, 0x0168, 0x03) ||
	    write_reg(t, 0x014a, 0x03) || write_reg(t, 0x0161, 0x03))
		goto error;
	exit_cmd(t);
	p[0] = 0xc8;
	p[1] = 0x64;
	p[2] = 0x50;
	if (CMD(t, &p[0], PS2_SETRATE) || CMD(t, &p[1], PS2_SETRATE) || CMD(t, &p[2], PS2_SETRATE) || CMD(t, p, PS2_GETID))
		return -1;
	p[0] = 0x64;
	if (CMD(t, p, PS2_SETRATE) || CMD(t, NULL, PS2_ENABLE))
		return -1;
	return 0;
error:
	exit_cmd(t);
	return -1;
}

static int dolphin_area(struct tp *t)
{
	struct tp_alps *a = &t->alps;
	u8 p[4] = { 0 };
	int nx, ny;
	if (enter_cmd(t))
		return -1;
	p[0] = 0x0a;
	if (CMD(t, NULL, PS2_RESET_WRAP) || CMD(t, NULL, PS2_SETPOLL) || CMD(t, NULL, PS2_SETPOLL) ||
	    CMD(t, &p[0], PS2_SETRATE) || CMD(t, &p[0], PS2_SETRATE))
		return -1;
	if (CMD(t, p, PS2_GETINFO))
		return -1;
	nx = DOLPHIN_PROFILE_XOFFSET + (p[2] & 0x0f);
	ny = DOLPHIN_PROFILE_YOFFSET + ((p[2] >> 4) & 0x0f);
	a->x_bits = nx;
	a->y_bits = ny;
	a->x_max = (nx - 1) * DOLPHIN_COUNT_PER_ELECTRODE;
	a->y_max = (ny - 1) * DOLPHIN_COUNT_PER_ELECTRODE;
	return exit_cmd(t);
}

static int hw_init_dolphin(struct tp *t)
{
	u8 p[2] = { 0x64, 0x28 };
	if (CMD(t, NULL, PS2_SETSTREAM) || CMD(t, &p[0], PS2_SETRATE) || CMD(t, &p[1], PS2_SETRATE))
		return -1;
	return 0;
}

static const char *proto_name(int p)
{
	switch (p)
	{
	case PROTO_V1: return "v1";
	case PROTO_V2: return "v2";
	case PROTO_V3: return "v3 (Pinnacle)";
	case PROTO_V3_RUSHMORE: return "v3 (Rushmore)";
	case PROTO_V4: return "v4";
	case PROTO_V5: return "v5 (Dolphin)";
	default: return "unsupported";
	}
}

/* identify (E6, E7, EC reports); 0 if an ALPS device we can drive */
int tp_alps_detect(struct tp *t)
{
	struct tp_alps *a = &t->alps;
	u8 e6[4], e7[4], ec[4];
	unsigned i;
	if (rpt_cmd(t, PS2_SETRES, PS2_SETSCALE11, e6))
		return -1;
	if ((e6[0] & 0xf8) != 0 || e6[1] != 0 || (e6[2] != 10 && e6[2] != 100))
		return -1;
	if (rpt_cmd(t, PS2_SETRES, PS2_SETSCALE21, e7) || rpt_cmd(t, PS2_SETRES, PS2_RESET_WRAP, ec) || exit_cmd(t))
		return -1;
	memset(a, 0, sizeof(*a));
	memcpy(a->e7, e7, 3);
	memcpy(a->ec, ec, 3);
	for (i = 0; i < sizeof(models) / sizeof(models[0]); i++)
		if (!memcmp(e7, models[i].sig, 3))
		{
			a->proto = models[i].proto;
			a->byte0 = models[i].byte0;
			a->mask0 = models[i].mask0;
			a->flags = models[i].flags;
			break;
		}
	if (!a->proto)
	{
		if (e7[0] == 0x73 && e7[1] == 0x02 && e7[2] == 0x64 && ec[2] == 0x8a)
			a->proto = PROTO_V4, a->byte0 = 0x8f, a->mask0 = 0x8f;
		else if (e7[0] == 0x73 && e7[1] == 0x03 && e7[2] == 0x50 && ec[0] == 0x73 && (ec[1] == 0x01 || ec[1] == 0x02))
			a->proto = PROTO_V5, a->byte0 = 0xc8, a->mask0 = 0xd8;
		else if (ec[0] == 0x88 && ec[1] == 0x08)
			a->proto = PROTO_V3_RUSHMORE, a->byte0 = 0x8f, a->mask0 = 0x8f, a->flags = DUALPOINT;
		else if (ec[0] == 0x88 && ec[1] == 0x07 && ec[2] >= 0x90 && ec[2] <= 0x9d)
			a->proto = PROTO_V3, a->byte0 = 0x8f, a->mask0 = 0x8f, a->flags = DUALPOINT;
		else
			a->proto = -1;
	}
	tp_log(t, "alps: E7=%02x %02x %02x EC=%02x %02x %02x: %s\n", e7[0], e7[1], e7[2], ec[0], ec[1], ec[2],
	       a->proto > 0 ? proto_name(a->proto) : "not a supported ALPS model");
	if (a->proto <= 0 || a->proto == PROTO_V6)
		return -1;
	a->x_max = 2000;
	a->y_max = 1400;
	a->x_bits = 15;
	a->y_bits = 11;
	a->second_touch = -1;
	switch (a->proto)
	{
	case PROTO_V1:
	case PROTO_V2:
		a->x_max = 1023;
		a->y_max = 767;
		break;
	case PROTO_V3:
		a->nibble_cmd = nib_v3_cmd;
		a->addr_command = PS2_RESET_WRAP;
		if (probe_stick_v3(t, REG_BASE_PINNACLE) < 0)
			a->flags &= ~DUALPOINT;
		break;
	case PROTO_V3_RUSHMORE:
		a->nibble_cmd = nib_v3_cmd;
		a->addr_command = PS2_RESET_WRAP;
		a->x_bits = 16;
		a->y_bits = 12;
		if (probe_stick_v3(t, REG_BASE_RUSHMORE) < 0)
			a->flags &= ~DUALPOINT;
		break;
	case PROTO_V4:
		a->nibble_cmd = nib_v4_cmd;
		a->addr_command = PS2_DISABLE;
		break;
	case PROTO_V5:
		a->nibble_cmd = nib_v3_cmd;
		a->addr_command = PS2_RESET_WRAP;
		a->x_bits = 23;
		a->y_bits = 12;
		if (dolphin_area(t))
			return -1;
		break;
	}
	t->x_max = a->x_max;
	t->y_max = a->y_max;
	t->has_stick = (a->flags & DUALPOINT) != 0;
	t->pktsize = a->proto == PROTO_V4 ? 8 : 6;
	{
		static const char hex[] = "0123456789abcdef";
		char *n = t->name;
		const char *s = "ALPS ";
		while (*s)
			*n++ = *s++;
		s = proto_name(a->proto);
		while (*s)
			*n++ = *s++;
		s = " E7=";
		while (*s)
			*n++ = *s++;
		for (i = 0; i < 3; i++)
			*n++ = hex[e7[i] >> 4], *n++ = hex[e7[i] & 15];
		s = " EC=";
		while (*s)
			*n++ = *s++;
		for (i = 0; i < 3; i++)
			*n++ = hex[ec[i] >> 4], *n++ = hex[ec[i] & 15];
		if (t->has_stick)
			for (s = " +stick"; *s;)
				*n++ = *s++;
		*n = 0;
	}
	return 0;
}

int tp_alps_init(struct tp *t)
{
	switch (t->alps.proto)
	{
	case PROTO_V1:
	case PROTO_V2: return hw_init_v1_v2(t);
	case PROTO_V3: return hw_init_v3(t);
	case PROTO_V3_RUSHMORE: return hw_init_rushmore(t);
	case PROTO_V4: return hw_init_v4(t);
	case PROTO_V5: return hw_init_dolphin(t);
	}
	return -1;
}

/* byte idx (0-based) of the packet being assembled: still plausible? */
int tp_alps_valid_byte(struct tp *t, int idx, u8 b)
{
	struct tp_alps *a = &t->alps;
	if (idx == 0)
		return (b & a->mask0) == a->byte0 || (b & 0xc8) == 0x08;   /* or a bare PS/2 packet */
	if (a->proto < PROTO_V5 && (t->packet[0] & 0xc8) != 0x08 && (b & 0x80))
		return 0;
	return 1;
}

/* ---- semi-MT bitmaps (v3, v4, v5) ---- */
struct bpt
{
	int start, num;
};

static void bitmap_points(u32 map, struct bpt *lo, struct bpt *hi, int *fingers)
{
	struct bpt *p = lo;
	int i, bit, prev = 0;
	for (i = 0; map != 0; i++, map >>= 1)
	{
		bit = map & 1;
		if (bit)
		{
			if (!prev)
			{
				p->start = i;
				p->num = 0;
				(*fingers)++;
			}
			p->num++;
		}
		else if (prev)
			p = hi;
		prev = bit;
	}
}

static int process_bitmap(struct tp_alps *a, u32 xmap, u32 ymap, int stx, int sty, int *x2, int *y2)
{
	int i, fx = 0, fy = 0, fingers, closest;
	struct bpt xl = { 0, 0 }, xh = { 0, 0 }, yl = { 0, 0 }, yh = { 0, 0 };
	int cx[4], cy[4];
	if (!xmap || !ymap)
		return 0;
	bitmap_points(xmap, &xl, &xh, &fx);
	bitmap_points(ymap, &yl, &yh, &fy);
	fingers = fx > fy ? fx : fy;
	if (fx == 1)
	{
		i = (xl.num - 1) / 2;
		xl.num -= i;
		xh.start = xl.start + i;
		xh.num = i > 1 ? i : 1;
	}
	if (fy == 1)
	{
		i = (yl.num - 1) / 2;
		yl.num -= i;
		yh.start = yl.start + i;
		yh.num = i > 1 ? i : 1;
	}
#define CX(p) ((a->x_max * (2 * (p).start + (p).num - 1)) / (2 * (a->x_bits - 1)))
#define CY(p) ((a->y_max * (2 * (p).start + (p).num - 1)) / (2 * (a->y_bits - 1)))
	cx[0] = CX(xl), cy[0] = CY(yl);
	cx[1] = CX(xh), cy[1] = CY(yl);
	cx[2] = CX(xh), cy[2] = CY(yh);
	cx[3] = CX(xl), cy[3] = CY(yh);
	for (i = 0; i < 4; i++)
	{
		if (a->proto == PROTO_V5)
			cx[i] = a->x_max - cx[i];
		if (a->proto == PROTO_V3 || a->proto == PROTO_V4)
			cy[i] = a->y_max - cy[i];
	}
	if (a->second_touch == -1)
	{
		closest = 0x7fffffff;
		for (i = 0; i < 4; i++)
		{
			int dx = stx - cx[i], dy = sty - cy[i], d = dx * dx + dy * dy;
			if (d < closest)
			{
				a->second_touch = i;
				closest = d;
			}
		}
		a->second_touch = (a->second_touch + 2) % 4;
	}
	*x2 = cx[a->second_touch];
	*y2 = cy[a->second_touch];
	return fingers;
}

struct fields
{
	int first_mp, is_mp, fingers;
	u32 x_map, y_map;
	int x, y, z;
	int left, right, middle, ts_left, ts_right, ts_middle;
};

static void buttons_v3(struct fields *f, const u8 *p)
{
	f->left = !!(p[3] & 0x01);
	f->right = !!(p[3] & 0x02);
	f->middle = !!(p[3] & 0x04);
	f->ts_left = !!(p[3] & 0x10);
	f->ts_right = !!(p[3] & 0x20);
	f->ts_middle = !!(p[3] & 0x40);
}

static void decode(struct tp_alps *a, struct fields *f, const u8 *p)
{
	memset(f, 0, sizeof(*f));
	if (a->proto == PROTO_V5)
	{
		f->first_mp = !!(p[0] & 0x02);
		f->is_mp = !!(p[0] & 0x20);
		if (!f->is_mp)
		{
			f->x = (p[1] & 0x7f) | ((p[4] & 0x0f) << 7);
			f->y = (p[2] & 0x7f) | ((p[4] & 0xf0) << 3);
			f->z = (p[0] & 4) ? 0 : p[5] & 0x7f;
			buttons_v3(f, p);
		}
		else
		{
			/* 35 bits of profile: low part in 32 bits, the top 3 bits separately */
			u32 lo = (u32) (p[1] & 0x7f) | ((u32) (p[2] & 0x7f) << 7) | ((u32) (p[4] & 0x7f) << 14) |
			         ((u32) (p[5] & 0x7f) << 21) | ((u32) (p[3] & 0x07) << 28);
			u32 hi = (u32) ((p[3] & 0x70) >> 4) | ((u32) (p[0] & 0x01) << 3);   /* bits 31..34 */
			int yb = a->y_bits, xb = a->x_bits;
			f->fingers = ((p[0] & 0x6) >> 1) | ((p[0] & 0x10) >> 2);
			lo |= (hi & 1) << 31;
			hi >>= 1;   /* bits 32..34 */
			f->y_map = lo & ((1u << yb) - 1);
			/* x map = bits yb .. yb+xb-1 of the 35-bit value */
			f->x_map = ((lo >> yb) | (yb ? (hi << (32 - yb)) : 0)) & (xb >= 32 ? 0xffffffffu : ((1u << xb) - 1));
		}
		return;
	}
	if (a->proto == PROTO_V3_RUSHMORE)
	{
		f->first_mp = !!(p[4] & 0x40);
		f->is_mp = !!(p[5] & 0x40);
		if (f->is_mp)
		{
			int f1 = p[5] & 3, f2 = (p[5] >> 2) & 3;
			f->fingers = (f1 > f2 ? f1 : f2) + 1;
			f->x_map = ((u32) (p[5] & 0x10) << 11) | ((u32) (p[4] & 0x7e) << 8) | ((p[1] & 0x7f) << 2) |
			           ((p[0] & 0x30) >> 4);
			f->y_map = ((p[5] & 0x20) << 6) | ((p[3] & 0x70) << 4) | ((p[2] & 0x7f) << 1) | (p[4] & 0x01);
			return;
		}
	}
	else
	{
		f->first_mp = !!(p[4] & 0x40);
		f->is_mp = !!(p[0] & 0x40);
		if (f->is_mp)
		{
			f->fingers = (p[5] & 0x3) + 1;
			f->x_map = ((u32) (p[4] & 0x7e) << 8) | ((p[1] & 0x7f) << 2) | ((p[0] & 0x30) >> 4);
			f->y_map = ((p[3] & 0x70) << 4) | ((p[2] & 0x7f) << 1) | (p[4] & 0x01);
			return;
		}
	}
	f->x = ((p[1] & 0x7f) << 4) | ((p[4] & 0x30) >> 2) | ((p[0] & 0x30) >> 4);
	f->y = ((p[2] & 0x7f) << 4) | (p[4] & 0x0f);
	f->z = p[5] & 0x7f;
	buttons_v3(f, p);
}

/* the full packet in t->packet. Fills a touchpad frame and/or stick motion. */
int tp_alps_packet(struct tp *t, struct tp_frame *fr, struct tp_out *st, int *have_stick, int *have_frame)
{
	struct tp_alps *a = &t->alps;
	u8 *p = t->packet;
	*have_stick = *have_frame = 0;
	memset(fr, 0, sizeof(*fr));
	memset(st, 0, sizeof(*st));

	/* a bare 3-byte PS/2 packet (external mouse, or the stick on v2) */
	if ((p[0] & 0xc8) == 0x08)
	{
		st->buttons = p[0] & 7;
		st->dx = (p[0] & 0x10) ? (int) p[1] - 256 : p[1];
		st->dy = -((p[0] & 0x20) ? (int) p[2] - 256 : p[2]);
		*have_stick = 1;
		return 3;
	}

	if (a->proto == PROTO_V1 || a->proto == PROTO_V2)
	{
		int x, y, z, left, right, middle, ges, fin;
		if (a->proto == PROTO_V1)
		{
			left = p[2] & 0x10;
			right = p[2] & 0x08;
			middle = 0;
			x = p[1] | ((p[0] & 0x07) << 7);
			y = p[4] | ((p[3] & 0x07) << 7);
		}
		else
		{
			left = p[3] & 1;
			right = p[3] & 2;
			middle = p[3] & 4;
			x = p[1] | ((p[2] & 0x78) << 4);
			y = p[4] | ((p[3] & 0x70) << 3);
		}
		z = p[5];
		if (a->flags & FW_BK_2)
		{
			int back = p[3] & 4, fwd = p[2] & 4;
			middle = back && fwd;
		}
		ges = p[2] & 1;
		fin = p[2] & 2;
		if ((a->flags & DUALPOINT) && z == 127)
		{
			st->dx = x > 383 ? x - 768 : x;
			st->dy = y > 255 ? y - 512 : y;   /* Linux reports -y with y up; we use y down */
			st->buttons = (left ? 1 : 0) | (right ? 2 : 0) | (middle ? 4 : 0);
			*have_stick = 1;
			return 6;
		}
		if (ges && !fin)   /* hardware tap: a short contact */
			z = 40;
		a->prev_fin = fin;
		fr->fingers = z > 0;
		fr->x = x;
		fr->y = y;
		fr->z = z;
		fr->left = !!left;
		fr->right = !!right;
		fr->middle = !!middle;
		if (a->flags & WHEEL)
		{
			/* hardware wheel: pass as a scroll through the stick path */
			int w = ((p[2] << 1) & 0x08) - ((p[0] >> 4) & 0x07);
			st->wheel = w * 120;
			*have_stick = w != 0;
		}
		*have_frame = 1;
		return 6;
	}

	if (a->proto == PROTO_V3 || a->proto == PROTO_V3_RUSHMORE)
	{
		if (p[5] == 0x3f)
		{
			/* DualPoint stick packet */
			int x, y;
			if (!(a->flags & DUALPOINT) || !(p[0] & 0x40))
				return 6;
			if (p[1] == 0x7f && p[2] == 0x7f && p[4] == 0x7f)
				return 6;
			x = (s8) (((p[0] & 0x20) << 2) | (p[1] & 0x7f));
			y = (s8) (((p[0] & 0x10) << 3) | (p[2] & 0x7f));
			st->dx = x / 8;
			st->dy = -(y / 8);
			if (p[3] & 7)
				a->quirks |= QUIRK_TRACKSTICK_BUTTONS;
			st->buttons = (a->quirks & QUIRK_TRACKSTICK_BUTTONS) ? (p[3] & 7) : -1;
			*have_stick = 1;
			return 6;
		}
	}

	if (a->proto == PROTO_V3 || a->proto == PROTO_V3_RUSHMORE || a->proto == PROTO_V5)
	{
		struct fields f, pos;
		int fingers = 0;
		decode(a, &f, p);
		if (a->multi_packet)
		{
			if (f.is_mp)
			{
				fingers = f.fingers;
				decode(a, &pos, a->multi_data);
				fingers = process_bitmap(a, f.x_map, f.y_map, pos.x, pos.y, &fr->x2, &fr->y2) ? fingers : 0;
				f = pos;   /* position data + buttons from the position packet */
				f.fingers = fingers;
			}
			else
				a->multi_packet = 0;
		}
		if (f.is_mp)
			return 6;
		if (!a->multi_packet && f.first_mp)
		{
			a->multi_packet = 1;
			memcpy(a->multi_data, p, 6);
			return 6;
		}
		a->multi_packet = 0;
		if (f.x && f.y && !f.z)
			return 6;   /* a fluke z=0 packet in the middle of a stream */
		fingers = f.fingers;
		if (fingers < 2)
		{
			fingers = f.z > 0 ? 1 : 0;
			a->second_touch = -1;
		}
		fr->fingers = fingers;
		fr->x = f.x;
		fr->y = f.y;
		fr->z = f.z;
		fr->left = f.left;
		fr->right = f.right;
		fr->middle = f.middle;
		*have_frame = 1;
		if ((a->flags & DUALPOINT) && !(a->quirks & QUIRK_TRACKSTICK_BUTTONS))
		{
			/* stick buttons come in the touchpad packet */
			st->buttons = f.ts_left | (f.ts_right << 1) | (f.ts_middle << 2);
			*have_stick = 2;   /* buttons only */
		}
		return 6;
	}

	if (a->proto == PROTO_V4)
	{
		int off;
		u32 xm, ym;
		if (p[6] & 0x40)
			a->multi_packet = 0;
		if (a->multi_packet > 2)
			a->multi_packet = 0;
		off = 2 * a->multi_packet;
		a->multi_data[off] = p[6];
		a->multi_data[off + 1] = p[7];
		fr->left = !!(p[4] & 1);
		fr->right = !!(p[4] & 2);
		fr->x = ((p[1] & 0x7f) << 4) | ((p[3] & 0x30) >> 2) | ((p[0] & 0x30) >> 4);
		fr->y = ((p[2] & 0x7f) << 4) | (p[3] & 0x0f);
		fr->z = p[5] & 0x7f;
		if (++a->multi_packet > 2)
		{
			u8 *m = a->multi_data;
			int n;
			a->multi_packet = 0;
			xm = ((u32) (m[2] & 0x1f) << 10) | ((m[3] & 0x60) << 3) | ((m[0] & 0x3f) << 2) | ((m[1] & 0x60) >> 5);
			ym = ((m[5] & 0x01) << 10) | ((m[3] & 0x1f) << 5) | (m[1] & 0x1f);
			n = process_bitmap(a, xm, ym, fr->x, fr->y, &fr->x2, &fr->y2);
			a->v4_fingers = n;
			a->v4_x2 = fr->x2;
			a->v4_y2 = fr->y2;
		}
		/* the bitmap comes every third packet: keep its finger count in between */
		fr->fingers = fr->z <= 0 ? 0 : a->v4_fingers >= 2 ? a->v4_fingers : 1;
		fr->x2 = a->v4_x2;
		fr->y2 = a->v4_y2;
		if (fr->fingers < 2)
			a->second_touch = -1;
		*have_frame = 1;
		return 8;
	}
	return t->pktsize;
}
