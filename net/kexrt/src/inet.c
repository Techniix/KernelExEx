/*
 *  KernelEx kexrt: inet_pton / inet_ntop text conversion (IPv4 and IPv6).
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Pure string work: Windows 9x has no IPv6 stack, but programs parse and
 *  print IPv6 addresses anyway (configuration, URLs, logs), and they call
 *  these functions unconditionally. IPv6 output follows RFC 5952 (lower
 *  case, longest run of two or more zero groups compressed, leftmost on a
 *  tie, IPv4-mapped addresses as ::ffff:a.b.c.d).
 */
#include <stddef.h>
#include <string.h>
#include "inet.h"

int kx_pton4(const char *s, unsigned char out[4])
{
	int part = 0;
	unsigned v = 0, digits = 0;

	for (;; s++)
	{
		if (*s >= '0' && *s <= '9')
		{
			if (digits && v == 0)
				return 0; /* no leading zeros (would be octal elsewhere) */
			v = v * 10 + (unsigned)(*s - '0');
			if (v > 255 || ++digits > 3)
				return 0;
		}
		else if (*s == '.' || *s == '\0')
		{
			if (!digits || part > 3)
				return 0;
			out[part++] = (unsigned char)v;
			v = digits = 0;
			if (*s == '\0')
				break;
		}
		else
			return 0;
	}
	return part == 4;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int kx_pton6(const char *s, unsigned char out[16])
{
	unsigned char tmp[16];
	int n = 0, gap = -1;
	const char *p = s;

	memset(tmp, 0, sizeof(tmp));
	if (p[0] == ':')
	{
		if (p[1] != ':')
			return 0;
		p++;
	}
	while (*p)
	{
		if (*p == ':')
		{
			if (gap >= 0)
				return 0; /* only one "::" */
			gap = n;
			p++;
			if (!*p)
				break;
			continue;
		}
		{
			const char *start = p;
			unsigned v = 0;
			int d = 0, h;
			while ((h = hexval(*p)) >= 0)
			{
				if (++d > 4)
					return 0;
				v = (v << 4) | (unsigned)h;
				p++;
			}
			if (*p == '.')
			{
				/* embedded IPv4 in the last 32 bits */
				if (n > 12 || !kx_pton4(start, tmp + n))
					return 0;
				n += 4;
				break;
			}
			if (!d || n > 14)
				return 0;
			tmp[n++] = (unsigned char)(v >> 8);
			tmp[n++] = (unsigned char)v;
			if (*p == ':')
			{
				p++;
				if (!*p)
					return 0; /* trailing single ':' */
				if (*p == ':')
					continue; /* the next loop turn sees "::" */
			}
			else if (*p)
				return 0;
		}
	}
	if (gap >= 0)
	{
		int tail = n - gap;
		if (n == 16)
			return 0; /* "::" must stand for at least one group */
		memmove(tmp + 16 - tail, tmp + gap, (size_t)tail);
		memset(tmp + gap, 0, (size_t)(16 - n));
	}
	else if (n != 16)
		return 0;
	memcpy(out, tmp, 16);
	return 1;
}

static char *put_dec(char *o, unsigned v)
{
	if (v >= 100)
		*o++ = (char)('0' + v / 100);
	if (v >= 10)
		*o++ = (char)('0' + v / 10 % 10);
	*o++ = (char)('0' + v % 10);
	return o;
}

size_t kx_ntop4(const unsigned char a[4], char *buf, size_t size)
{
	char tmp[16], *o = tmp;
	int i;
	for (i = 0; i < 4; i++)
	{
		if (i)
			*o++ = '.';
		o = put_dec(o, a[i]);
	}
	*o = '\0';
	if ((size_t)(o - tmp) + 1 > size)
		return 0;
	memcpy(buf, tmp, (size_t)(o - tmp) + 1);
	return (size_t)(o - tmp);
}

size_t kx_ntop6(const unsigned char a[16], char *buf, size_t size)
{
	static const char hx[] = "0123456789abcdef";
	char tmp[48], *o = tmp;
	unsigned g[8];
	int i, best = -1, best_len = 0, cur = -1, cur_len = 0;

	for (i = 0; i < 8; i++)
		g[i] = ((unsigned)a[2 * i] << 8) | a[2 * i + 1];
	for (i = 0; i < 8; i++)
	{
		if (g[i] == 0)
		{
			if (cur < 0)
				cur = i, cur_len = 0;
			cur_len++;
			if (cur_len > best_len)
				best = cur, best_len = cur_len;
		}
		else
			cur = -1;
	}
	if (best_len < 2)
		best = -1;
	/* IPv4-mapped ::ffff:a.b.c.d */
	if (g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0xffff)
	{
		size_t n;
		memcpy(tmp, "::ffff:", 7);
		n = kx_ntop4(a + 12, tmp + 7, sizeof(tmp) - 7);
		o = tmp + 7 + n;
	}
	else
	{
		int skip_sep = 0;
		for (i = 0; i < 8; i++)
		{
			if (i == best)
			{
				*o++ = ':';
				*o++ = ':';
				skip_sep = 1;
				i += best_len - 1;
				continue;
			}
			if (i && !skip_sep)
				*o++ = ':';
			skip_sep = 0;
			{
				int shift, started = 0;
				for (shift = 12; shift >= 0; shift -= 4)
				{
					unsigned d = (g[i] >> shift) & 15;
					if (d || started || shift == 0)
					{
						*o++ = hx[d];
						started = 1;
					}
				}
			}
		}
	}
	*o = '\0';
	if ((size_t)(o - tmp) + 1 > size)
		return 0;
	memcpy(buf, tmp, (size_t)(o - tmp) + 1);
	return (size_t)(o - tmp);
}
