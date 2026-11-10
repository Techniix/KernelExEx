/*
 *  KernelEx net runtime: 64-bit integer division helpers for i386
 *  (the libgcc names clang emits on the i686-w64-windows-gnu target).
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Portable C so the host tests can check them against native division.
 *  KEX_DIV_PREFIX renames them for that host build.
 */
#ifndef KEX_DIV_PREFIX
#define KEX_DIV(name) name
#else
#define KEX_DIV(name) kexdiv_##name
#endif

/* ---------- 64-bit division (no CPU instruction for it on i386) ----------
 * Plain shift-and-subtract on 32-bit halves; written so that the compiler
 * never needs to emit a call back into these functions. */

typedef unsigned long long u64;
typedef long long s64;

u64 KEX_DIV(__udivmoddi4)(u64 num, u64 den, u64 *rem)
{
	u64 q = 0, r = 0;
	int i;

	if (den == 0)
	{
		/* match hardware: divide by zero faults */
		volatile unsigned int zero = 0;
		return (u64)(1u / zero);
	}
	if ((den >> 32) == 0 && (num >> 32) == 0)
	{
		unsigned int n = (unsigned int)num, d = (unsigned int)den;
		if (rem)
			*rem = n % d;
		return n / d;
	}
	for (i = 63; i >= 0; i--)
	{
		r = (r << 1) | ((num >> i) & 1);
		if (r >= den)
		{
			r -= den;
			q |= (u64)1 << i;
		}
	}
	if (rem)
		*rem = r;
	return q;
}

u64 KEX_DIV(__udivdi3)(u64 a, u64 b) { return KEX_DIV(__udivmoddi4)(a, b, 0); }

u64 KEX_DIV(__umoddi3)(u64 a, u64 b)
{
	u64 r;
	KEX_DIV(__udivmoddi4)(a, b, &r);
	return r;
}

s64 KEX_DIV(__divdi3)(s64 a, s64 b)
{
	int neg = (a < 0) ^ (b < 0);
	u64 q = KEX_DIV(__udivmoddi4)(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0);
	return neg ? -(s64)q : (s64)q;
}

s64 KEX_DIV(__moddi3)(s64 a, s64 b)
{
	u64 r;
	KEX_DIV(__udivmoddi4)(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
	return a < 0 ? -(s64)r : (s64)r;
}

s64 KEX_DIV(__divmoddi4)(s64 a, s64 b, s64 *rem)
{
	s64 q = KEX_DIV(__divdi3)(a, b);
	if (rem)
		*rem = a - q * b;
	return q;
}

