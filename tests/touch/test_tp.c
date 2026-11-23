/*
 *  Host tests for the touchpad core: a simulated ALPS v3 (Pinnacle) DualPoint
 *  and an IntelliMouse answer the PS/2 commands; packets go through the gesture
 *  engine.  cc -I../../drivers/input/touch test_tp.c ../../drivers/input/touch/tp_*.c
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tp.h"

static int fails, verbose;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- simulated devices ---------------- */
enum { DEV_ALPS3, DEV_IMPS2, DEV_NONE, DEV_SYN, DEV_ELAN };
static struct
{
	int type;
	int hist[3], nhist;      /* last commands (for the 3x reports) */
	int cmdmode;
	int addr, nib[4], nnib, have_addr, wnib[2], nwnib;
	u8 reg[0x10000];
	u8 rates[3];
	int writes;
	/* Synaptics */
	int slices, sliced, mode, client;
	unsigned identity, model, caps;
	/* Elantech */
	int e6run, f8, f8q, regwrites;
	unsigned fw;
} sim;

static const u16 v3_cmd[16] = { PS2_SETPOLL, PS2_RESET_DIS, PS2_SETSCALE21, PS2_SETRATE, PS2_SETRATE, PS2_SETRATE,
	                            PS2_SETRATE, PS2_SETRATE, PS2_SETRATE, PS2_SETRATE, PS2_GETID1, PS2_SETRES,
	                            PS2_SETRES, PS2_SETRES, PS2_SETRES, PS2_SETSCALE11 };
static const u8 v3_data[16] = { 0, 0, 0, 0x0a, 0x14, 0x28, 0x3c, 0x50, 0x64, 0xc8, 0, 0, 1, 2, 3, 0 };

static int nibble_of(int cmd, u8 p)
{
	int i;
	for (i = 0; i < 16; i++)
		if (v3_cmd[i] == cmd && ((cmd >> 12) == 0 || v3_data[i] == p))
			return i;
	return -1;
}

static void push(int c)
{
	sim.hist[0] = sim.hist[1];
	sim.hist[1] = sim.hist[2];
	sim.hist[2] = c;
}

int tp_ps2_command(struct tp *t, u8 *p, int cmd)
{
	(void) t;
	if (sim.type == DEV_NONE)
		return -1;
	if (cmd == PS2_RESET_BAT)
	{
		p[0] = 0xaa;
		p[1] = 0;
		sim.cmdmode = 0;
		return 0;
	}
	if (sim.type == DEV_ELAN)
	{
		int q = -1;
		if (cmd == PS2_SETSCALE11)
		{
			sim.e6run++;
			sim.slices = 0, sim.sliced = 0;
			if (sim.f8 == 1)
				sim.f8 = 0;
		}
		else if (cmd == PS2_SETRES)
		{
			sim.sliced = ((sim.sliced << 2) | (p[0] & 3)) & 0xff;
			sim.slices++;
			sim.e6run = 0;
		}
		else if (cmd == 0xf8)
			sim.f8 = 1, sim.e6run = 0;
		else if (cmd == PS2_GETINFO)
		{
			if (sim.e6run >= 3)
				p[0] = 0x3c, p[1] = 0x03, p[2] = 0xc8;   /* the magic knock */
			else
			{
				q = sim.f8 == 2 ? sim.f8q : sim.slices >= 4 ? sim.sliced : 0xff;
				if (q == 0x01)
					p[0] = (u8) (sim.fw >> 16), p[1] = (u8) (sim.fw >> 8), p[2] = (u8) sim.fw;
				else if (q == 0x02)
					p[0] = 0x00, p[1] = 0x15, p[2] = 0x0c;   /* capabilities: traces */
				else if (q == 0x00)
					p[0] = 0x35, p[1] = 0xac, p[2] = 0xc0;   /* range 0x5ac x 0x3c0 */
				else
					p[0] = 0, p[1] = 0, p[2] = 0x64;
			}
			sim.e6run = 0, sim.slices = 0, sim.sliced = 0, sim.f8 = 0;
		}
		else if ((cmd >> 8) == 0)   /* a raw byte after F8 */
		{
			if (sim.f8 == 1)
				sim.f8 = 2, sim.f8q = cmd & 0xff;
			else
				sim.f8 = 0;
			sim.e6run = 0;
			sim.regwrites++;
		}
		else if (cmd == PS2_GETID)
			p[0] = 0;
		else
			sim.e6run = 0;
		return 0;
	}
	if (sim.type == DEV_SYN)
	{
		if (cmd == PS2_SETSCALE11)
			sim.slices = 0, sim.sliced = 0;
		else if (cmd == PS2_SETRES)
		{
			sim.sliced = ((sim.sliced << 2) | (p[0] & 3)) & 0xff;
			sim.slices++;
		}
		else if (cmd == PS2_GETINFO)
		{
			unsigned v = sim.sliced == 0 ? sim.identity : sim.sliced == 2 ? sim.caps : sim.sliced == 3 ? sim.model : 0;
			p[0] = (u8) (v >> 16), p[1] = (u8) (v >> 8), p[2] = (u8) v;
			sim.slices = 0, sim.sliced = 0;
		}
		else if (cmd == PS2_SETRATE && sim.slices >= 4 && p[0] == 0x14)
			sim.mode = sim.sliced, sim.slices = 0, sim.sliced = 0;
		else if (cmd == PS2_SETRATE && sim.slices >= 4 && p[0] == 0x28)
			sim.client = sim.sliced, sim.slices = 0, sim.sliced = 0;
		else if (cmd == PS2_GETID)
			p[0] = 0;
		return 0;
	}
	if (sim.type == DEV_IMPS2)
	{
		if (cmd == PS2_SETRATE)
		{
			sim.rates[0] = sim.rates[1];
			sim.rates[1] = sim.rates[2];
			sim.rates[2] = p[0];
		}
		if (cmd == PS2_GETID)
			p[0] = (sim.rates[0] == 200 && sim.rates[1] == 100 && sim.rates[2] == 80) ? 3 : 0;
		if (cmd == PS2_GETINFO)
			p[0] = p[1] = 0, p[2] = 100;
		return 0;
	}
	/* ALPS v3 */
	if (sim.cmdmode)
	{
		int n;
		if (cmd == PS2_SETSTREAM)
		{
			sim.cmdmode = 0;
			return 0;
		}
		if (cmd == PS2_GETINFO)
		{
			if (sim.hist[0] == PS2_RESET_WRAP && sim.hist[1] == PS2_RESET_WRAP && sim.hist[2] == PS2_RESET_WRAP)
			{
				p[0] = 0x88, p[1] = 0x07, p[2] = 0x9d;   /* command mode again: EC report */
				push(cmd);
				return 0;
			}
			p[0] = (u8) (sim.addr >> 8);
			p[1] = (u8) sim.addr;
			p[2] = sim.reg[sim.addr];
			push(cmd);
			return 0;
		}
		if (cmd == PS2_RESET_WRAP)
		{
			/* address command: four nibbles follow */
			push(cmd);
			sim.nnib = 0;
			sim.have_addr = 0;
			sim.nwnib = 0;
			return 0;
		}
		n = nibble_of(cmd, p ? p[0] : 0);
		push(cmd);
		if (n < 0)
			return 0;
		if (sim.nnib < 4)
		{
			sim.nib[sim.nnib++] = n;
			if (sim.nnib == 4)
			{
				sim.addr = (sim.nib[0] << 12) | (sim.nib[1] << 8) | (sim.nib[2] << 4) | sim.nib[3];
				sim.have_addr = 1;
				sim.nwnib = 0;
			}
			return 0;
		}
		sim.wnib[sim.nwnib++] = n;
		if (sim.nwnib == 2)
		{
			sim.reg[sim.addr] = (u8) ((sim.wnib[0] << 4) | sim.wnib[1]);
			sim.writes++;
			sim.nwnib = 0;
		}
		return 0;
	}
	if (cmd == PS2_GETINFO)
	{
		if (sim.hist[0] == sim.hist[1] && sim.hist[1] == sim.hist[2])
		{
			switch (sim.hist[2])
			{
			case PS2_SETSCALE11: p[0] = 0, p[1] = 0, p[2] = 0x64; break;   /* E6 */
			case PS2_SETSCALE21: p[0] = 0x73, p[1] = 0x02, p[2] = 0x64; break;   /* E7 */
			case PS2_RESET_WRAP: p[0] = 0x88, p[1] = 0x07, p[2] = 0x9d; sim.cmdmode = 1; sim.nnib = 4; break;   /* EC */
			default: p[0] = 0, p[1] = 0, p[2] = 0x64;
			}
		}
		else
			p[0] = 0, p[1] = 0, p[2] = 0x64;
		push(cmd);
		return 0;
	}
	push(cmd);
	return 0;
}

void tp_log(struct tp *t, const char *fmt, ...)
{
	va_list ap;
	(void) t;
	if (!verbose)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---------------- packet builders (inverse of the v3 decoder) ---------------- */
static void v3_pos(u8 *p, int x, int y, int z, int buttons, int first_mp)
{
	p[0] = (u8) (0x8f | ((x & 3) << 4));
	p[1] = (u8) ((x >> 4) & 0x7f);
	p[2] = (u8) ((y >> 4) & 0x7f);
	p[3] = (u8) (buttons & 0x77);
	p[4] = (u8) (((x >> 2) & 3) << 4 | (y & 0x0f) | (first_mp ? 0x40 : 0));
	p[5] = (u8) (z & 0x7f);
	if (p[5] == 0x3f)
		p[5] = 0x3e;
}

static void v3_bitmap(u8 *p, unsigned xmap, unsigned ymap, int fingers)
{
	p[0] = (u8) (0x8f | 0x40 | ((xmap & 3) << 4));
	p[1] = (u8) ((xmap >> 2) & 0x7f);
	p[2] = (u8) ((ymap >> 1) & 0x7f);
	p[3] = (u8) (((ymap >> 8) & 7) << 4);
	p[4] = (u8) ((((xmap >> 9) & 0x3f) << 1) | (ymap & 1));
	p[5] = (u8) ((fingers - 1) & 3);
}

static void v3_stick(u8 *p, int dx, int dy, int buttons)
{
	int x = dx * 8, y = -dy * 8;
	p[0] = (u8) (0x8f | 0x40 | ((x & 0x80) ? 0x20 : 0) | ((y & 0x80) ? 0x10 : 0));
	p[1] = (u8) (x & 0x7f);
	p[2] = (u8) (y & 0x7f);
	p[3] = (u8) (buttons & 7);
	p[4] = 0x10;
	p[5] = 0x3f;
}

/* Synaptics NEWABS packet (inverse of tp_syn_packet), our coordinates (0-based, y down) */
static void syn_pkt(u8 *b, int x, int y, int z, int w, int buttons)
{
	int rx = x + 1472, ry = 4448 - y;
	b[0] = (u8) (0x80 | ((w & 0x0c) << 2) | ((w & 0x02) << 1) | (buttons & 3));
	b[1] = (u8) ((((ry >> 8) & 0x0f) << 4) | ((rx >> 8) & 0x0f));
	b[2] = (u8) z;
	b[3] = (u8) (0xc0 | (((rx >> 12) & 1) << 4) | (((ry >> 12) & 1) << 5) | ((w & 1) << 2) | (buttons & 3));
	b[4] = (u8) rx;
	b[5] = (u8) ry;
}

static void syn_stick(u8 *b, int dx, int dy, int buttons)
{
	int y = -dy;
	b[0] = 0x84 | 0x00;
	b[1] = (u8) (0x08 | (buttons & 7) | (dx < 0 ? 0x10 : 0) | (y < 0 ? 0x20 : 0));
	b[2] = 0;
	b[3] = 0xc4;
	b[4] = (u8) dx;
	b[5] = (u8) y;
}

/* Elantech v4 packets (no CRC), our coordinates (y down); y_max 0x3c0 */
static void elan_head(u8 *b, int id, int x, int y, int buttons)
{
	int ry = 0x3c0 - y, pres = 0x40;
	b[0] = (u8) (buttons & 3);
	b[1] = (u8) ((pres & 0xf0) | ((x >> 8) & 0x0f));
	b[2] = (u8) x;
	b[3] = (u8) (0x10 | 1 | ((id + 1) << 5));
	b[4] = (u8) (((pres & 0x0f) << 4) | ((ry >> 8) & 0x0f));
	b[5] = (u8) ry;
}

static void elan_status(u8 *b, int bitmap, int buttons)
{
	b[0] = (u8) (buttons & 3);
	b[1] = (u8) bitmap;
	b[2] = 0;
	b[3] = 0x10;
	b[4] = 0;
	b[5] = 0;
}

static void elan_motion(u8 *b, int id, int dx, int dy, int sid, int dx2, int dy2)
{
	b[0] = (u8) ((id + 1) << 5);
	b[1] = (u8) dx;
	b[2] = (u8) -dy;   /* the pad's y grows upward */
	b[3] = (u8) (0x10 | 2 | (sid >= 0 ? (sid + 1) << 5 : 0));
	b[4] = (u8) dx2;
	b[5] = (u8) -dy2;
}

static struct tp T;
static u32 now;
static struct tp_out acc;   /* sums since the last reset */
static int presses[8], releases[8], last_buttons;

static void take(struct tp_out *o)
{
	int b;
	acc.dx += o->dx;
	acc.dy += o->dy;
	acc.wheel += o->wheel;
	acc.hwheel += o->hwheel;
	for (b = 0; b < 3; b++)
	{
		if ((o->buttons & (1 << b)) && !(last_buttons & (1 << b)))
			presses[b]++;
		if (!(o->buttons & (1 << b)) && (last_buttons & (1 << b)))
			releases[b]++;
	}
	last_buttons = o->buttons;
}

static void reset_acc(void)
{
	memset(&acc, 0, sizeof(acc));
	memset(presses, 0, sizeof(presses));
	memset(releases, 0, sizeof(releases));
}

static void feed(const u8 *p, int n)
{
	int i;
	struct tp_out o;
	for (i = 0; i < n; i++)
		if (tp_byte(&T, p[i], now, &o))
			take(&o);
}

static void wait_ms(int ms)
{
	struct tp_out o;
	int i;
	for (i = 0; i < ms; i += 10)
	{
		now += 10;
		if (tp_tick(&T, now, &o))
			take(&o);
	}
}

static void touch(int x, int y, int z)
{
	u8 p[6];
	v3_pos(p, x, y, z, 0, 0);
	feed(p, 6);
	now += 10;
}

static void two(int x, int y, unsigned xmap, unsigned ymap)
{
	u8 p[6];
	v3_pos(p, x, y, 40, 0, 1);
	feed(p, 6);
	v3_bitmap(p, xmap, ymap, 2);
	feed(p, 6);
	now += 10;
}

static void lift(void)
{
	touch(0, 0, 0);
}

int main(int argc, char **argv_)
{
	int i;
	(void) argv_;
	verbose = argc > 1;

	/* ---- ALPS v3 detection and set-up ---- */
	memset(&sim, 0, sizeof(sim));
	sim.type = DEV_ALPS3;
	sim.reg[0x0008] = 0x80;   /* trackstick present */
	sim.reg[0x0004] = 0x00;
	CHECK(tp_init(&T) == 0, "ALPS init");
	CHECK(T.kind == TP_ALPS, "kind %d", T.kind);
	CHECK(T.alps.proto == 0x300, "proto %x", T.alps.proto);
	CHECK(T.has_stick, "stick found");
	CHECK((sim.reg[0x0004] & 0x06) == 0x06, "absolute mode bits: reg4=%02x", sim.reg[0x0004]);
	CHECK(sim.reg[0x0163] == 0x03 && sim.reg[0x0162] == 0x04, "v3 regs 163/162: %02x %02x", sim.reg[0x163], sim.reg[0x162]);
	CHECK((sim.reg[0x0008] & 0x02) == 0x02, "stick extended mode reg8=%02x", sim.reg[0x0008]);
	CHECK(!strncmp(T.name, "ALPS v3 (Pinnacle) E7=730264 EC=88079d", 38), "name '%s'", T.name);

	/* ---- one finger moving right and down: pointer moves, no click ---- */
	reset_acc();
	now = 1000;
	for (i = 0; i < 20; i++)
		touch(500 + i * 20, 400 + i * 10, 40);
	lift();
	wait_ms(300);
	CHECK(acc.dx > 100 && acc.dy > 50, "move: dx %d dy %d", acc.dx, acc.dy);
	CHECK(acc.dx > acc.dy, "dx > dy");
	CHECK(presses[0] == 0, "no click after a move");

	/* ---- tap: left click (press, then release after the tap window) ---- */
	reset_acc();
	touch(900, 700, 40);
	touch(901, 700, 40);
	lift();
	CHECK(presses[0] == 1 && releases[0] == 0, "tap press: %d/%d", presses[0], releases[0]);
	wait_ms(300);
	CHECK(releases[0] == 1, "tap release");
	CHECK(abs(acc.dx) < 3 && abs(acc.dy) < 3, "tap did not move the pointer: %d", acc.dx);

	/* ---- tap disabled ---- */
	T.set.tap = 0;
	reset_acc();
	touch(900, 700, 40);
	lift();
	wait_ms(300);
	CHECK(presses[0] == 0, "no tap click when disabled");
	T.set.tap = 1;

	/* ---- double tap: two clicks ---- */
	reset_acc();
	touch(900, 700, 40);
	lift();
	now += 50;
	touch(900, 700, 40);
	lift();
	wait_ms(400);
	CHECK(presses[0] == 2 && releases[0] == 2, "double tap: %d presses %d releases", presses[0], releases[0]);

	/* ---- tap and drag: button held while moving ---- */
	reset_acc();
	touch(900, 700, 40);
	lift();
	now += 50;
	for (i = 0; i < 20; i++)
		touch(900 + i * 20, 700, 40);
	CHECK(last_buttons & 1, "dragging holds the left button");
	lift();
	wait_ms(300);
	CHECK(!(last_buttons & 1) && presses[0] == 1, "drag released once: %d", presses[0]);
	CHECK(acc.dx > 100, "drag moved %d", acc.dx);

	/* ---- two-finger tap: right click ---- */
	reset_acc();
	two(900, 700, (3u << 2) | (3u << 10), 7u << 4);
	lift();
	wait_ms(300);
	CHECK(presses[1] == 1 && releases[1] == 1 && presses[0] == 0, "two-finger tap = right click: %d %d", presses[1], presses[0]);

	/* ---- two-finger scroll: fingers move up -> wheel up (positive) ---- */
	reset_acc();
	for (i = 0; i < 20; i++)
		two(900, 1000 - i * 30, (3u << 2) | (3u << 10), 7u << 4);
	lift();
	wait_ms(300);
	CHECK(acc.wheel >= 360, "two-finger scroll up: wheel %d", acc.wheel);
	CHECK(acc.dx == 0 && acc.dy == 0, "no pointer motion while scrolling: %d %d", acc.dx, acc.dy);
	CHECK(presses[0] == 0 && presses[1] == 0, "no click after scrolling");

	/* ---- reversed ("natural") scrolling ---- */
	T.set.scroll_reverse = 1;
	reset_acc();
	for (i = 0; i < 20; i++)
		two(900, 1000 - i * 30, (3u << 2) | (3u << 10), 7u << 4);
	lift();
	wait_ms(300);
	CHECK(acc.wheel <= -360, "reversed scroll: wheel %d", acc.wheel);
	T.set.scroll_reverse = 0;

	/* ---- right-edge scroll: finger moves down along the edge -> wheel down ---- */
	reset_acc();
	for (i = 0; i < 20; i++)
		touch(1950, 300 + i * 30, 40);
	lift();
	wait_ms(300);
	CHECK(acc.wheel <= -360 && acc.dx == 0, "edge scroll: wheel %d dx %d", acc.wheel, acc.dx);

	/* ---- hardware left button ---- */
	reset_acc();
	{
		u8 p[6];
		v3_pos(p, 0, 0, 0, 0x01, 0);
		feed(p, 6);
		v3_pos(p, 0, 0, 0, 0x00, 0);
		feed(p, 6);
	}
	CHECK(presses[0] == 1 && releases[0] == 1, "hardware button: %d %d", presses[0], releases[0]);

	/* ---- stick: moves the pointer ---- */
	reset_acc();
	{
		u8 p[6];
		for (i = 0; i < 10; i++)
		{
			v3_stick(p, 5, -3, 0);
			feed(p, 6);
			now += 10;
		}
	}
	CHECK(acc.dx == 50 && acc.dy == -30, "stick motion: %d %d", acc.dx, acc.dy);

	/* ---- stick: middle button held + moving = scroll, not a middle click ---- */
	reset_acc();
	{
		u8 p[6];
		v3_stick(p, 0, 0, 4);
		feed(p, 6);
		for (i = 0; i < 10; i++)
		{
			v3_stick(p, 0, -4, 4);
			feed(p, 6);
			now += 10;
		}
		v3_stick(p, 0, 0, 0);
		feed(p, 6);
		wait_ms(300);
	}
	CHECK(acc.wheel > 0 && acc.dy == 0, "stick scroll: wheel %d dy %d", acc.wheel, acc.dy);
	CHECK(presses[2] == 0, "no middle click after stick scrolling");

	/* ---- stick: middle pressed and released without moving = middle click ---- */
	reset_acc();
	{
		u8 p[6];
		v3_stick(p, 0, 0, 4);
		feed(p, 6);
		now += 50;
		v3_stick(p, 0, 0, 0);
		feed(p, 6);
		wait_ms(300);
	}
	CHECK(presses[2] == 1 && releases[2] == 1, "stick middle click: %d %d", presses[2], releases[2]);

	/* ---- resync after a garbage byte ---- */
	reset_acc();
	{
		u8 g = 0x00;
		feed(&g, 1);
		touch(500, 500, 40);
		touch(520, 500, 40);
		lift();
		wait_ms(300);
	}
	CHECK(acc.dx > 0, "recovers after garbage: dx %d", acc.dx);

	/* ---- Synaptics ---- */
	memset(&sim, 0, sizeof(sim));
	sim.type = DEV_SYN;
	sim.identity = 0x024717;   /* v7.2 */
	sim.model = 0x0000b1;      /* NEWABS */
	sim.caps = 0x800083;       /* extended, pass-through, multi-finger, palm */
	CHECK(tp_init(&T) == 0 && T.kind == TP_SYNAPTICS, "Synaptics detected: kind %d", T.kind);
	CHECK(sim.mode == 0xc1, "mode absolute|high rate|W: %02x", sim.mode);
	CHECK(sim.client == 0xf4, "pass-through stick enabled: %02x", sim.client);
	CHECK(T.has_stick && !strncmp(T.name, "Synaptics v7.2", 14), "name '%s'", T.name);
	{
		u8 b[6];
		reset_acc();
		last_buttons = 0;
		now = 50000;
		for (i = 0; i < 20; i++)
		{
			syn_pkt(b, 1000 + i * 40, 1000 + i * 20, 60, 4, 0);
			feed(b, 6);
			now += 12;
		}
		syn_pkt(b, 0, 0, 0, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(acc.dx > 100 && acc.dy > 40 && presses[0] == 0, "syn move: %d %d", acc.dx, acc.dy);
		/* tap */
		reset_acc();
		syn_pkt(b, 2000, 1500, 60, 4, 0);
		feed(b, 6);
		now += 30;
		syn_pkt(b, 0, 0, 0, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(presses[0] == 1 && releases[0] == 1, "syn tap: %d %d", presses[0], releases[0]);
		/* two fingers (W = 0) moving up: scroll up */
		reset_acc();
		for (i = 0; i < 20; i++)
		{
			syn_pkt(b, 2000, 2500 - i * 60, 60, 0, 0);
			feed(b, 6);
			now += 12;
		}
		syn_pkt(b, 0, 0, 0, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(acc.wheel >= 360 && acc.dx == 0 && acc.dy == 0, "syn two-finger scroll: %d (%d %d)", acc.wheel, acc.dx, acc.dy);
		/* a palm (W = 12) moves nothing and clicks nothing */
		reset_acc();
		for (i = 0; i < 10; i++)
		{
			syn_pkt(b, 1500 + i * 50, 1500, 120, 12, 0);
			feed(b, 6);
			now += 12;
		}
		syn_pkt(b, 0, 0, 0, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(acc.dx == 0 && presses[0] == 0, "syn palm ignored: %d", acc.dx);
		/* hardware buttons */
		reset_acc();
		syn_pkt(b, 0, 0, 0, 0, 2);
		feed(b, 6);
		syn_pkt(b, 0, 0, 0, 0, 0);
		feed(b, 6);
		CHECK(presses[1] == 1 && releases[1] == 1, "syn right button: %d %d", presses[1], releases[1]);
		/* the stick through the pass-through port */
		reset_acc();
		for (i = 0; i < 5; i++)
		{
			syn_stick(b, 4, -2, 0);
			feed(b, 6);
			now += 10;
		}
		CHECK(acc.dx == 20 && acc.dy == -10, "syn stick: %d %d", acc.dx, acc.dy);
	}

	/* ---- Elantech v4 ---- */
	memset(&sim, 0, sizeof(sim));
	sim.type = DEV_ELAN;
	sim.fw = 0x470f01;   /* ic 7: hardware version 4 */
	CHECK(tp_init(&T) == 0 && T.kind == TP_ELANTECH, "Elantech detected: kind %d (%s)", T.kind, T.name);
	CHECK(T.elan.hw == 4 && T.x_max == 0x5ac && T.y_max == 0x3c0, "v4 %d, range %d x %d", T.elan.hw, T.x_max, T.y_max);
	CHECK(sim.regwrites > 0, "absolute mode register written");
	{
		u8 b[6];
		reset_acc();
		last_buttons = 0;
		now = 90000;
		elan_status(b, 1, 0);
		feed(b, 6);
		elan_head(b, 0, 400, 300, 0);
		feed(b, 6);
		now += 12;
		for (i = 0; i < 20; i++)
		{
			elan_motion(b, 0, 12, 6, -1, 0, 0);
			feed(b, 6);
			now += 12;
		}
		elan_status(b, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(acc.dx > 100 && acc.dy > 40 && presses[0] == 0, "elan move: %d %d", acc.dx, acc.dy);
		/* tap */
		reset_acc();
		elan_status(b, 1, 0);
		feed(b, 6);
		elan_head(b, 0, 700, 500, 0);
		feed(b, 6);
		now += 40;
		elan_status(b, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(presses[0] == 1 && releases[0] == 1, "elan tap: %d %d", presses[0], releases[0]);
		/* two fingers moving up: scroll up */
		reset_acc();
		elan_status(b, 3, 0);
		feed(b, 6);
		elan_head(b, 0, 600, 700, 0);
		feed(b, 6);
		elan_head(b, 1, 800, 700, 0);
		feed(b, 6);
		now += 12;
		for (i = 0; i < 20; i++)
		{
			elan_motion(b, 0, 0, -20, 1, 0, -20);
			feed(b, 6);
			now += 12;
		}
		elan_status(b, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(acc.wheel >= 360 && acc.dx == 0 && acc.dy == 0, "elan two-finger scroll: %d (%d %d)", acc.wheel, acc.dx, acc.dy);
		/* garbage, then a valid stream again */
		reset_acc();
		{
			u8 g[3] = { 0x08, 0x55, 0xaa };
			feed(g, 3);
		}
		elan_status(b, 1, 0);
		feed(b, 6);
		elan_head(b, 0, 400, 300, 0);
		feed(b, 6);
		now += 12;
		for (i = 0; i < 10; i++)
		{
			elan_motion(b, 0, 10, 0, -1, 0, 0);
			feed(b, 6);
			now += 12;
		}
		elan_status(b, 0, 0);
		feed(b, 6);
		wait_ms(300);
		CHECK(acc.dx > 30, "elan recovers after garbage: %d", acc.dx);
	}

	/* ---- IntelliMouse fallback ---- */
	memset(&sim, 0, sizeof(sim));
	sim.type = DEV_IMPS2;
	CHECK(tp_init(&T) == 0 && T.kind == TP_IMPS2, "IntelliMouse detected: kind %d", T.kind);
	reset_acc();
	last_buttons = 0;
	{
		u8 p[4] = { 0x29, 10, 0xfb, 0xff };   /* left, dx 10, dy -5 (Y sign bit set) (up in PS/2 = down for us: +5), wheel -1 */
		feed(p, 4);
	}
	CHECK(acc.dx == 10 && acc.dy == 5 && acc.wheel == 120 && presses[0] == 1, "imps2 packet: %d %d %d %d", acc.dx, acc.dy,
	      acc.wheel, presses[0]);

	/* ---- nothing connected ---- */
	memset(&sim, 0, sizeof(sim));
	sim.type = DEV_NONE;
	CHECK(tp_init(&T) != 0, "no device");

	printf("%s (%d failures)\n", fails ? "FAILED" : "all touchpad tests passed", fails);
	return fails != 0;
}
