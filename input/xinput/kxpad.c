/*
 *  KernelExEx - gamepad mapping engine. See kxpad.h.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 */
#include "kxpad.h"
#include <string.h>
#include <stdio.h>

#ifdef KXPAD_W9XRT        /* Windows 9x build: msvcrt.dll there has no snprintf */
#include "../../net/rt/w9xrt.h"
#define KX_SNPRINTF w9x_snprintf
#else
#define KX_SNPRINTF snprintf
#endif

const char *const kxpad_target_names[KXT_COUNT] = {
	"a", "b", "x", "y", "back", "guide", "start",
	"leftstick", "rightstick", "leftshoulder", "rightshoulder",
	"dpup", "dpdown", "dpleft", "dpright",
	"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

/* XINPUT_GAMEPAD_* bit for each button target */
static const unsigned short target_bit[KXT_COUNT] = {
	0x1000, 0x2000, 0x4000, 0x8000, 0x0020, KXPAD_GUIDE_BIT, 0x0010,
	0x0040, 0x0080, 0x0100, 0x0200,
	0x0001, 0x0002, 0x0004, 0x0008,
	0, 0, 0, 0, 0, 0,
};

static int is_axis_target(int t) { return t >= KXT_LEFTX; }

static int parse_bind(const char *s, size_t n, kxpad_bind *b)
{
	unsigned v = 0, digits = 0;
	size_t i = 0;
	memset(b, 0, sizeof(*b));
	if (i < n && (s[i] == '+' || s[i] == '-'))
	{
		b->half = s[i] == '+' ? 1 : -1;
		i++;
	}
	if (i >= n)
		return -1;
	switch (s[i++])
	{
	case 'b': b->type = KXS_BUTTON; break;
	case 'a': b->type = KXS_AXIS; break;
	case 'h': b->type = KXS_HAT; break;
	default: return -1;
	}
	while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 3)
	{
		v = v * 10 + (unsigned) (s[i++] - '0');
		digits++;
	}
	if (!digits)
		return -1;
	b->index = (unsigned char) v;
	if (b->type == KXS_HAT)
	{
		unsigned mask = 0;
		if (i >= n || s[i++] != '.')
			return -1;
		digits = 0;
		while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 2)
		{
			mask = mask * 10 + (unsigned) (s[i++] - '0');
			digits++;
		}
		if (!digits || (mask != 1 && mask != 2 && mask != 4 && mask != 8))
			return -1;
		b->hatmask = (unsigned char) mask;
	}
	if (i < n && s[i] == '~')
	{
		b->invert = 1;
		i++;
	}
	if (i != n)
		return -1;
	if ((b->type == KXS_BUTTON && v > 31) || (b->type == KXS_AXIS && v > 5) || (b->type == KXS_HAT && v > 0))
		return -1;
	return 0;
}

int kxpad_parse(const char *line, kxpad_map *m)
{
	const char *p = line, *e;
	int field = 0;
	memset(m, 0, sizeof(*m));
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '#' || *p == 0 || *p == '\r' || *p == '\n')
		return -1;
	for (;;)
	{
		size_t n;
		e = p;
		while (*e && *e != ',' && *e != '\r' && *e != '\n')
			e++;
		n = (size_t) (e - p);
		if (field == 0)
		{
			if (n != 32)
				return -1;
			memcpy(m->guid, p, 32);
			m->guid[32] = 0;
		}
		else if (field == 1)
		{
			if (n >= sizeof(m->name))
				n = sizeof(m->name) - 1;
			memcpy(m->name, p, n);
			m->name[n] = 0;
		}
		else if (n)
		{
			const char *colon = memchr(p, ':', n);
			int t;
			if (colon)
			{
				size_t kl = (size_t) (colon - p);
				for (t = 0; t < KXT_COUNT; t++)
					if (strlen(kxpad_target_names[t]) == kl && !memcmp(p, kxpad_target_names[t], kl))
						break;
				/* unknown keys (platform:, crc:, paddles, touchpad...) are ignored */
				if (t < KXT_COUNT)
				{
					kxpad_bind b;
					if (parse_bind(colon + 1, n - kl - 1, &b) == 0)
						m->bind[t] = b;
				}
			}
		}
		if (*e != ',')
			break;
		p = e + 1;
		field++;
	}
	return field >= 1 ? 0 : -1;
}

static int fmt_bind(const kxpad_bind *b, char *o, size_t n)
{
	const char *pre = b->half > 0 ? "+" : b->half < 0 ? "-" : "";
	const char *inv = b->invert ? "~" : "";
	switch (b->type)
	{
	case KXS_BUTTON: return KX_SNPRINTF(o, n, "%sb%u%s", pre, b->index, inv);
	case KXS_AXIS: return KX_SNPRINTF(o, n, "%sa%u%s", pre, b->index, inv);
	case KXS_HAT: return KX_SNPRINTF(o, n, "h%u.%u", b->index, b->hatmask);
	}
	return 0;
}

int kxpad_format(const kxpad_map *m, char *out, size_t outlen)
{
	size_t len;
	int t, r;
	r = KX_SNPRINTF(out, outlen, "%s,%s,", m->guid, m->name);
	if (r < 0 || (size_t) r >= outlen)
		return -1;
	len = (size_t) r;
	for (t = 0; t < KXT_COUNT; t++)
	{
		char b[16];
		if (m->bind[t].type == KXS_NONE)
			continue;
		fmt_bind(&m->bind[t], b, sizeof(b));
		r = KX_SNPRINTF(out + len, outlen - len, "%s:%s,", kxpad_target_names[t], b);
		if (r < 0 || (size_t) r >= outlen - len)
			return -1;
		len += (size_t) r;
	}
	r = KX_SNPRINTF(out + len, outlen - len, "platform:Windows 9x,");
	if (r < 0 || (size_t) r >= outlen - len)
		return -1;
	return (int) (len + (size_t) r);
}

void kxpad_usb_guid(unsigned vid, unsigned pid, char out[33])
{
	/* SDL 2 GUID layout: bus(le16) crc(le16) vendor(le16) 0000 product(le16) 0000 version(le16) 0000 */
	KX_SNPRINTF(out, 33, "03000000%02x%02x0000%02x%02x000000000000",
	         vid & 0xFF, (vid >> 8) & 0xFF, pid & 0xFF, (pid >> 8) & 0xFF);
}

void kxpad_default(kxpad_map *m, const char *guid, const char *name, int naxes)
{
	static const char *const generic =
		"a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
		"leftstick:b8,rightstick:b9,guide:b10,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
		"leftx:a0,lefty:a1,rightx:a2,righty:a3,";
	char line[512];
	KX_SNPRINTF(line, sizeof(line), "%.32s,%.40s,%s%s", guid, name, generic,
	         naxes >= 6 ? "lefttrigger:a4,righttrigger:a5," : "lefttrigger:b6,righttrigger:b7,");
	kxpad_parse(line, m);
	if (naxes < 6)
	{
		/* small pads: back/start on 8/9, triggers on 6/7 */
		kxpad_parse(line, m);
		m->bind[KXT_BACK].index = 8;
		m->bind[KXT_START].index = 9;
		m->bind[KXT_LEFTSTICK].index = 10;
		m->bind[KXT_RIGHTSTICK].index = 11;
		m->bind[KXT_GUIDE].index = 12;
	}
}

unsigned kxpad_pov_to_hat(unsigned long pov)
{
	if (pov > 36000)
		return 0;
	/* 8 sectors of 45 degrees centred on the directions */
	switch (((pov + 2250) % 36000) / 4500)
	{
	case 0: return 1;
	case 1: return 1 | 2;
	case 2: return 2;
	case 3: return 2 | 4;
	case 4: return 4;
	case 5: return 4 | 8;
	case 6: return 8;
	default: return 8 | 1;
	}
}

int kxpad_scale_axis(unsigned long v, unsigned long lo, unsigned long hi)
{
	long long span, x;
	if (hi <= lo)
		return 0;
	if (v < lo) v = lo;
	if (v > hi) v = hi;
	span = (long long) hi - (long long) lo;
	x = ((long long) (v - lo) * 65535 + span / 2) / span - 32768;
	return (int) x;
}

/* value of a source, as -32768..32767 for axes and 0/32767 for buttons and hat directions */
static int source_value(const kxpad_bind *b, const kxpad_raw *raw)
{
	int v;
	switch (b->type)
	{
	case KXS_BUTTON:
		v = (raw->buttons >> b->index) & 1 ? 32767 : 0;
		break;
	case KXS_HAT:
		v = (raw->hat & b->hatmask) ? 32767 : 0;
		break;
	case KXS_AXIS:
		v = raw->axis[b->index];
		if (b->half > 0)
			v = v > 0 ? v : 0;
		else if (b->half < 0)
			v = v < 0 ? -v : 0;
		if (v > 32767)
			v = 32767;
		break;
	default:
		return 0;
	}
	if (b->invert)
		v = b->type == KXS_AXIS && !b->half ? (v == -32768 ? 32767 : -v) : 32767 - v;
	return v;
}

static short clamp16(int v)
{
	return (short) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void kxpad_apply(const kxpad_map *m, const kxpad_raw *raw, kxpad_out *out)
{
	int t;
	memset(out, 0, sizeof(*out));
	for (t = 0; t < KXT_COUNT; t++)
	{
		const kxpad_bind *b = &m->bind[t];
		int v;
		if (b->type == KXS_NONE)
			continue;
		v = source_value(b, raw);
		if (!is_axis_target(t))
		{
			/* an axis bound to a button counts as pressed past half way */
			int pressed = b->type == KXS_AXIS ? (b->half ? v > 16384 : v > 16384 || v < -16384) : v != 0;
			if (pressed)
				out->buttons |= target_bit[t];
			continue;
		}
		switch (t)
		{
		case KXT_LEFTX: out->lx = clamp16(v); break;
		case KXT_RIGHTX: out->rx = clamp16(v); break;
		/* joystick Y grows downwards; XInput Y grows upwards */
		case KXT_LEFTY: out->ly = clamp16(-1 - v); break;
		case KXT_RIGHTY: out->ry = clamp16(-1 - v); break;
		case KXT_LEFTTRIGGER:
		case KXT_RIGHTTRIGGER:
		{
			/* full axis -32768..32767 -> 0..255; half axis or button 0..32767 -> 0..255 */
			int z = (b->type == KXS_AXIS && !b->half) ? (v + 32768) / 257 : v / 128;
			unsigned char u = (unsigned char) (z < 0 ? 0 : z > 255 ? 255 : z);
			if (t == KXT_LEFTTRIGGER) out->lt = u; else out->rt = u;
			break;
		}
		}
	}
}

int kxpad_db_find(const char *db, size_t len, const char *guid, kxpad_map *m)
{
	const char *p = db, *end = db + len;
	int found = 0;
	while (p < end)
	{
		const char *nl = memchr(p, '\n', (size_t) (end - p));
		size_t n = nl ? (size_t) (nl - p) : (size_t) (end - p);
		if (n >= 33 && !memcmp(p, guid, 32) && p[32] == ',')
		{
			char line[1024];
			kxpad_map tmp;
			if (n >= sizeof(line))
				n = sizeof(line) - 1;
			memcpy(line, p, n);
			line[n] = 0;
			/* only mappings for this platform or with no platform field */
			if ((strstr(line, "platform:Windows 9x") || !strstr(line, "platform:")) && kxpad_parse(line, &tmp) == 0)
			{
				*m = tmp;
				found = 1;   /* keep going: later lines win */
			}
		}
		p += n + 1;
	}
	return found;
}
