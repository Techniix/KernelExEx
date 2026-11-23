/*
 *  KernelExEx - touchpad core: detection, packet assembly, gestures.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *  The PS/2 detection sequences follow Linux psmouse-base.c (GPL-2.0-only).
 */
#include "tp.h"
#include <string.h>

#define CMD(t, p, c) tp_ps2_command(t, p, c)
#define ABS(x) ((x) < 0 ? -(x) : (x))

void tp_default_settings(struct tp_settings *s)
{
	s->tap = 1;
	s->tap_drag = 1;
	s->scroll_two = 1;
	s->scroll_edge = 1;
	s->scroll_reverse = 0;
	s->scroll_speed = 10;
	s->speed = 10;
	s->stick_scroll = 1;
	s->stick_speed = 10;
	s->palm = 1;
}

static int set_rates(struct tp *t, const u8 *r, int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		u8 p = r[i];
		if (CMD(t, &p, PS2_SETRATE))
			return -1;
	}
	return 0;
}

static int get_id(struct tp *t)
{
	u8 p[2] = { 0xff, 0xff };
	if (CMD(t, p, PS2_GETID))
		return -1;
	return p[0];
}

/* IntelliMouse (wheel, id 3) and IntelliMouse Explorer (5 buttons, id 4) knocks */
static enum tp_kind detect_wheel(struct tp *t)
{
	static const u8 im[] = { 200, 100, 80 }, ex[] = { 200, 200, 80 };
	enum tp_kind k = TP_PS2;
	if (set_rates(t, im, 3) == 0 && get_id(t) == 3)
	{
		k = TP_IMPS2;
		if (set_rates(t, ex, 3) == 0 && get_id(t) == 4)
			k = TP_EXPS2;
	}
	return k;
}

int tp_init(struct tp *t)
{
	u8 p[2];
	int id;
	struct tp_settings keep = t->set;
	void *ctx = t->ctx;
	memset(t, 0, sizeof(*t));
	t->ctx = ctx;
	t->set = keep;
	if (!t->set.speed)
		tp_default_settings(&t->set);

	/* reset: AA 00 */
	p[0] = p[1] = 0;
	if (CMD(t, p, PS2_RESET_BAT) || p[0] != 0xaa)
	{
		tp_log(t, "touch: no device on the PS/2 mouse port (reset: %02x %02x)\n", p[0], p[1]);
		return -1;
	}
	CMD(t, NULL, PS2_DISABLE);

	if (tp_syn_detect(t) == 0)
	{
		if (tp_syn_init(t) == 0)
		{
			t->kind = TP_SYNAPTICS;
			tp_log(t, "touch: %s, %dx%d\n", t->name, t->x_max, t->y_max);
			return 0;
		}
		tp_log(t, "touch: Synaptics set-up failed, using IntelliMouse mode\n");
		p[0] = p[1] = 0;
		CMD(t, p, PS2_RESET_BAT);
	}
	if (tp_alps_detect(t) == 0)
	{
		if (tp_alps_init(t) == 0)
		{
			t->kind = TP_ALPS;
			tp_log(t, "touch: %s, %dx%d\n", t->name, t->x_max, t->y_max);
			return 0;
		}
		tp_log(t, "touch: ALPS set-up failed, using IntelliMouse mode\n");
		p[0] = p[1] = 0;
		CMD(t, p, PS2_RESET_BAT);
	}

	p[0] = p[1] = 0;
	CMD(t, p, PS2_RESET_BAT);
	CMD(t, NULL, PS2_DISABLE);
	if (tp_elan_detect(t) == 0)
	{
		if (tp_elan_init(t) == 0)
		{
			t->kind = TP_ELANTECH;
			tp_log(t, "touch: %s, %dx%d\n", t->name, t->x_max, t->y_max);
			return 0;
		}
		tp_log(t, "touch: Elantech set-up failed, using IntelliMouse mode\n");
		p[0] = p[1] = 0;
		CMD(t, p, PS2_RESET_BAT);
	}

	/* not a touchpad we drive: best plain mouse mode */
	t->kind = detect_wheel(t);
	id = get_id(t);
	t->pktsize = t->kind == TP_PS2 ? 3 : 4;
	{
		const char *n = t->kind == TP_EXPS2 ? "IntelliMouse Explorer" : t->kind == TP_IMPS2 ? "IntelliMouse (wheel)" : "PS/2 mouse";
		strcpy(t->name, n);
	}
	p[0] = 100;
	CMD(t, p, PS2_SETRATE);
	p[0] = 3;   /* 8 counts/mm */
	CMD(t, p, PS2_SETRES);
	CMD(t, NULL, PS2_SETSCALE11);
	if (CMD(t, NULL, PS2_ENABLE))
		return -1;
	tp_log(t, "touch: %s (id %d)\n", t->name, id);
	return 0;
}

/* ---- relative mice ---- */
static int mouse_packet(struct tp *t, struct tp_out *o)
{
	u8 *p = t->packet;
	memset(o, 0, sizeof(*o));
	o->buttons = p[0] & 7;
	o->dx = (p[0] & 0x10) ? (int) p[1] - 256 : p[1];
	o->dy = -((p[0] & 0x20) ? (int) p[2] - 256 : p[2]);
	if (t->kind == TP_IMPS2)
		o->wheel = -(s8) p[3] * 120;
	else if (t->kind == TP_EXPS2)
	{
		int w = p[3] & 0x0f;
		if (w & 8)
			w -= 16;
		o->wheel = -w * 120;
		if (p[3] & 0x10)
			o->buttons |= 8;
		if (p[3] & 0x20)
			o->buttons |= 16;
	}
	t->g.sent_buttons = o->buttons;
	return 1;
}

static int valid_byte(struct tp *t, int idx, u8 b)
{
	if (t->kind == TP_ALPS)
		return tp_alps_valid_byte(t, idx, b);
	if (t->kind == TP_SYNAPTICS)
		return tp_syn_valid_byte(t, idx, b);
	if (t->kind == TP_ELANTECH)
		return tp_elan_valid_byte(t, idx, b);
	if (idx == 0)
		return (b & 0x08) != 0;   /* bit 3 is always 1 in a PS/2 first byte */
	return 1;
}

static int finish(struct tp *t, u32 now, struct tp_out *out)
{
	struct tp_frame f;
	struct tp_out st;
	int hs, hf, r = 0;
	if (t->kind == TP_ALPS)
		tp_alps_packet(t, &f, &st, &hs, &hf);
	else if (t->kind == TP_SYNAPTICS)
		tp_syn_packet(t, &f, &st, &hs, &hf);
	else
		return mouse_packet(t, out);
	memset(out, 0, sizeof(*out));
	if (hs)
	{
		if (hs == 2)
			t->g.stick_buttons = st.buttons;   /* stick buttons reported in the touchpad packet */
		else
			tp_stick(t, st.dx, st.dy, st.buttons, out);
		if (st.wheel)
			out->wheel += st.wheel;
		r = 1;
	}
	if (hf)
	{
		struct tp_out o2;
		tp_gesture(t, &f, now, &o2);
		out->dx += o2.dx;
		out->dy += o2.dy;
		out->wheel += o2.wheel;
		out->hwheel += o2.hwheel;
		r = 1;
	}
	out->buttons = t->g.sent_buttons;
	return r;
}

int tp_byte(struct tp *t, u8 b, u32 now, struct tp_out *out)
{
	/* a gap of more than 50 ms means a new packet starts (as Linux psmouse resync) */
	if (t->pktcnt && now - t->last_byte_ms > 50)
		t->pktcnt = 0;
	t->last_byte_ms = now;
	if (!valid_byte(t, t->pktcnt, b))
	{
		t->out_of_sync++;
		t->pktcnt = 0;
		if (!valid_byte(t, 0, b))
			return 0;
	}
	t->packet[t->pktcnt++] = b;
	/* ALPS bare PS/2 packets are 3 bytes */
	if (t->kind == TP_ALPS && t->pktcnt == 3 && (t->packet[0] & 0xc8) == 0x08 && t->packet[0] != t->alps.byte0)
	{
		t->pktcnt = 0;
		return finish(t, now, out);
	}
	if (t->pktcnt < t->pktsize)
		return 0;
	if (t->kind == TP_ELANTECH)
	{
		struct tp_frame f;
		int r = tp_elan_packet(t, &f);
		if (r < 0)
		{
			/* not a packet: try the next byte as its start */
			memmove(t->packet, t->packet + 1, (size_t) (t->pktsize - 1));
			t->pktcnt = t->pktsize - 1;
			t->out_of_sync++;
			return 0;
		}
		t->pktcnt = 0;
		t->out_of_sync = 0;
		if (!r)
			return 0;
		tp_gesture(t, &f, now, out);
		out->buttons = t->g.sent_buttons;
		return 1;
	}
	t->pktcnt = 0;
	t->out_of_sync = 0;
	return finish(t, now, out);
}

int tp_tick(struct tp *t, u32 now, struct tp_out *out)
{
	if (t->kind != TP_ALPS && t->kind != TP_SYNAPTICS && t->kind != TP_ELANTECH)
		return 0;
	return tp_gesture_tick(t, now, out);
}

/* ---- gestures ---- */
#define TAP_MS 180          /* longest touch that is a tap; also the double-tap/drag window */
#define EDGE_X 92           /* right edge: last 8% of the width */
#define EDGE_Y 88           /* bottom edge: last 12% */

/* device units -> mickeys (256ths kept in *acc) */
static int scale(int d, int range, int speed, int *acc)
{
	/* the full pad width is about 900 mickeys at speed 10 */
	long v = (long) d * 900 * 256 / (range ? range : 1) * speed / 10 + *acc;
	int r = (int) (v / 256);
	*acc = (int) (v - (long) r * 256);
	return r;
}

static int scroll_units(struct tp *t, int d, int range, int *acc)
{
	/* one notch (120) for about 1/12 of the pad */
	long v = (long) d * 120 * 12 * t->set.scroll_speed / 10 + *acc;
	int r = (int) (v / (range ? range : 1));
	*acc = (int) (v - (long) r * (range ? range : 1));
	return r;
}

void tp_stick(struct tp *t, int dx, int dy, int buttons, struct tp_out *out)
{
	int mid;
	if (buttons < 0)
		buttons = t->g.stick_buttons;
	mid = (buttons & 4) && t->set.stick_scroll;
	if (mid)
	{
		if (!t->g.stick_mid)
		{
			t->g.stick_mid = 1;
			t->g.stick_scrolled = 0;
		}
		if (dx || dy)
		{
			t->g.stick_scrolled = 1;
			out->wheel += -dy * 30 * t->set.scroll_speed / 10 * (t->set.scroll_reverse ? -1 : 1);
			out->hwheel += dx * 30 * t->set.scroll_speed / 10 * (t->set.scroll_reverse ? -1 : 1);
		}
		buttons &= ~4;
	}
	else
	{
		if (t->g.stick_mid && !t->g.stick_scrolled && t->set.stick_scroll)
		{
			/* middle pressed and released without moving: a middle click */
			t->g.sent_buttons |= 4;
			out->buttons = t->g.sent_buttons;
			t->g.tap_pending = 4;   /* released by the next tick */
			t->g.tap_up_ms = t->last_byte_ms;
		}
		t->g.stick_mid = 0;
		out->dx += dx * t->set.stick_speed / 10;
		out->dy += dy * t->set.stick_speed / 10;
	}
	t->g.stick_buttons = buttons;
	t->g.sent_buttons = (t->g.sent_buttons & ~7) | t->g.hw_buttons | buttons | t->g.tap_buttons |
	                    (t->g.tap_pending == 4 ? 4 : 0);
}

void tp_gesture(struct tp *t, const struct tp_frame *f, u32 now, struct tp_out *o)
{
	int fingers = f->fingers, was = t->g.fingers;
	memset(o, 0, sizeof(*o));
	if (t->set.palm && f->width >= 10)
		fingers = 0;   /* a palm */

	if (fingers && !was)
	{
		/* touch begins */
		t->g.touch_ms = (int) now;
		t->g.moved = 0;
		t->g.max_fingers = fingers;
		t->g.acc_x = t->g.acc_y = t->g.acc_w = t->g.acc_h = 0;
		t->g.scroll = 0;
		if (t->set.scroll_edge && f->x * 100 > t->x_max * EDGE_X)
			t->g.scroll = 2;
		else if (t->set.scroll_edge && f->y * 100 > t->y_max * EDGE_Y)
			t->g.scroll = 3;
		/* touching again soon after a tap: drag with the tapped button held */
		if (t->g.tap_pending && t->set.tap_drag && now - t->g.tap_up_ms < TAP_MS)
		{
			t->g.drag = t->g.tap_pending;
			t->g.tap_pending = 0;
		}
	}
	else if (fingers && was)
	{
		int dx = f->x - t->g.x, dy = f->y - t->g.y;
		if (fingers > t->g.max_fingers)
			t->g.max_fingers = fingers;
		if (fingers != was)
			dx = dy = 0;   /* finger count changed: positions jump */
		if (ABS(f->x - t->g.x) + ABS(f->y - t->g.y) > t->x_max / 60)
			t->g.moved = 1;
		if (fingers >= 2 && t->set.scroll_two && t->g.scroll != 2 && t->g.scroll != 3)
			t->g.scroll = 1;
		if (t->g.scroll)
		{
			int sgn = t->set.scroll_reverse ? -1 : 1;
			if (t->g.scroll == 1 || t->g.scroll == 2)
				o->wheel = -scroll_units(t, dy, t->y_max, &t->g.acc_w) * sgn;
			if (t->g.scroll == 1 || t->g.scroll == 3)
				o->hwheel = scroll_units(t, dx, t->x_max, &t->g.acc_h) * sgn;
			if (o->wheel || o->hwheel)
				t->g.moved = 1;
		}
		else if (fingers == 1)
		{
			o->dx = scale(dx, t->x_max, t->set.speed, &t->g.acc_x);
			o->dy = scale(dy, t->x_max, t->set.speed, &t->g.acc_y);   /* same scale both ways */
		}
	}
	else if (!fingers && was)
	{
		/* touch ends */
		if (t->g.drag)
		{
			/* a quick second tap (no drag happened): release now, press again on the
			 * next tick -> a double click */
			if (!t->g.moved && now - (u32) t->g.touch_ms < TAP_MS)
				t->g.repress = t->g.drag;
			t->g.drag = 0;
		}
		else if (t->set.tap && !t->g.moved && now - (u32) t->g.touch_ms < TAP_MS && !f->left && !f->right)
		{
			int b = t->g.max_fingers >= 3 ? 4 : t->g.max_fingers == 2 ? 2 : 1;
			t->g.tap_pending = b;   /* pressed now, released after the tap window */
			t->g.tap_up_ms = now;
		}
		t->g.scroll = 0;
	}
	t->g.fingers = fingers;
	t->g.x = f->x;
	t->g.y = f->y;
	t->g.hw_buttons = (f->left ? 1 : 0) | (f->right ? 2 : 0) | (f->middle ? 4 : 0);
	t->g.tap_buttons = t->g.drag | t->g.tap_pending;
	t->g.sent_buttons = t->g.hw_buttons | t->g.tap_buttons | (t->g.stick_buttons & (t->set.stick_scroll ? 3 : 7));
	o->buttons = t->g.sent_buttons;
}

int tp_gesture_tick(struct tp *t, u32 now, struct tp_out *o)
{
	if (t->g.repress)
	{
		t->g.tap_pending = t->g.repress;
		t->g.tap_up_ms = now;
		t->g.repress = 0;
		t->g.tap_buttons = t->g.tap_pending;
		t->g.sent_buttons |= t->g.tap_pending;
		memset(o, 0, sizeof(*o));
		o->buttons = t->g.sent_buttons;
		return 1;
	}
	if (t->g.tap_pending && !t->g.fingers && now - t->g.tap_up_ms >= TAP_MS)
	{
		t->g.tap_pending = 0;
		t->g.tap_buttons = t->g.drag;
		t->g.sent_buttons = t->g.hw_buttons | t->g.tap_buttons | (t->g.stick_buttons & (t->set.stick_scroll ? 3 : 7));
		memset(o, 0, sizeof(*o));
		o->buttons = t->g.sent_buttons;
		return 1;
	}
	return 0;
}
