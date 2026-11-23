/*
 *  KEDCL: the parts every backend shares (C basics, the log rings).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "kexos.h"

/* ---- C basics ---- */
void *memset(void *d, int c, unsigned n)
{
	unsigned char *p = (unsigned char *) d;
	while (n--)
		*p++ = (unsigned char) c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
	unsigned char *p = (unsigned char *) d;
	const unsigned char *q = (const unsigned char *) s;
	while (n--)
		*p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, unsigned n)
{
	unsigned char *p = (unsigned char *) d;
	const unsigned char *q = (const unsigned char *) s;
	if (p == q || !n)
		return d;
	if (p < q)
		while (n--)
			*p++ = *q++;
	else
		while (n--)
			p[n] = q[n];
	return d;
}

int memcmp(const void *a, const void *b, unsigned n)
{
	const unsigned char *p = (const unsigned char *) a, *q = (const unsigned char *) b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

unsigned strlen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

/* ---- logging ---- */
static void put(struct kexos_log *l, char c)
{
	l->buf[l->pos % sizeof(l->buf)] = c;
	l->pos++;
#ifdef KEX_DEBUGCON
	kexos_outb(0xe9, (kx_u8) c);
#endif
}

static void put_num(struct kexos_log *l, unsigned long v, int base, int width, int zero, int neg)
{
	char tmp[16];
	int n = 0;
	do
	{
		tmp[n++] = "0123456789abcdef"[v % (unsigned) base];
		v /= (unsigned) base;
	} while (v && n < 16);
	if (neg)
		width--;
	if (neg && zero)
		put(l, '-');
	while (width-- > n)
		put(l, zero ? '0' : ' ');
	if (neg && !zero)
		put(l, '-');
	while (n)
		put(l, tmp[--n]);
}

void kexos_vlog(struct kexos_log *l, const char *fmt, va_list ap)
{
	kx_u32 f = kexos_irq_save();
	for (; *fmt; fmt++)
	{
		int width = 0, zero = 0;
		if (*fmt != '%')
		{
			put(l, *fmt);
			continue;
		}
		fmt++;
		if (*fmt == '0')
			zero = 1, fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == 'l')
			fmt++;
		switch (*fmt)
		{
		case 'd':
		{
			long v = va_arg(ap, long);
			put_num(l, (unsigned long) (v < 0 ? -v : v), 10, width, zero, v < 0);
			break;
		}
		case 'u': put_num(l, va_arg(ap, unsigned long), 10, width, zero, 0); break;
		case 'x': put_num(l, va_arg(ap, unsigned long), 16, width, zero, 0); break;
		case 'c': put(l, (char) va_arg(ap, int)); break;
		case 's':
		{
			const char *s = va_arg(ap, const char *);
			while (s && *s)
				put(l, *s++);
			break;
		}
		case '%': put(l, '%'); break;
		default: put(l, '?');
		}
	}
	kexos_irq_restore(f);
}

void kexos_log(struct kexos_log *l, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	kexos_vlog(l, fmt, ap);
	va_end(ap);
}

kx_u32 kexos_log_read(const struct kexos_log *l, char *out, kx_u32 size)
{
	kx_u32 n = l->pos < sizeof(l->buf) ? l->pos : sizeof(l->buf);
	kx_u32 start = l->pos - n, i;
	if (n > size)
	{
		start += n - size;
		n = size;
	}
	for (i = 0; i < n; i++)
		out[i] = l->buf[(start + i) % sizeof(l->buf)];
	return n;
}
