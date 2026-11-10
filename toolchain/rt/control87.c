/*
 * _control87 for modules that used the static Visual C++ runtime. Same
 * abstract bits as <float.h> of msvcrt (they differ from the x87 layout).
 */
#define K_MCW_EM 0x0008001f
#define K_MCW_IC 0x00040000
#define K_MCW_RC 0x00000300
#define K_MCW_PC 0x00030000

static unsigned int from_x87(unsigned short cw)
{
	unsigned int v = 0;
	if (cw & 0x01) v |= 0x00000010; /* invalid */
	if (cw & 0x02) v |= 0x00080000; /* denormal */
	if (cw & 0x04) v |= 0x00000008; /* zero divide */
	if (cw & 0x08) v |= 0x00000004; /* overflow */
	if (cw & 0x10) v |= 0x00000002; /* underflow */
	if (cw & 0x20) v |= 0x00000001; /* inexact */
	v |= (unsigned int) (cw & 0x0C00) >> 2;          /* rounding: 0x400->0x100, 0x800->0x200 */
	switch (cw & 0x0300)                             /* precision */
	{
	case 0x0000: v |= 0x00020000; break;             /* 24 bits */
	case 0x0200: v |= 0x00010000; break;             /* 53 bits */
	default: break;                                  /* 64 bits = 0 */
	}
	if (cw & 0x1000) v |= 0x00040000;                /* infinity control */
	return v;
}

static unsigned short to_x87(unsigned int v, unsigned short old)
{
	unsigned short cw = old & (unsigned short) ~0x1F3F;
	if (v & 0x00000010) cw |= 0x01;
	if (v & 0x00080000) cw |= 0x02;
	if (v & 0x00000008) cw |= 0x04;
	if (v & 0x00000004) cw |= 0x08;
	if (v & 0x00000002) cw |= 0x10;
	if (v & 0x00000001) cw |= 0x20;
	cw |= (unsigned short) ((v & K_MCW_RC) << 2);
	switch (v & K_MCW_PC)
	{
	case 0x00020000: break;
	case 0x00010000: cw |= 0x0200; break;
	default: cw |= 0x0300; break;
	}
	if (v & K_MCW_IC) cw |= 0x1000;
	return cw;
}

unsigned int __cdecl _control87(unsigned int newval, unsigned int mask)
{
	volatile unsigned short cw;
	unsigned int cur;
	__asm__ __volatile__("fnstcw %0" : "=m" (cw));
	cur = from_x87(cw);
	mask &= K_MCW_EM | K_MCW_IC | K_MCW_RC | K_MCW_PC;
	if (mask)
	{
		cur = (cur & ~mask) | (newval & mask);
		cw = to_x87(cur, cw);
		__asm__ __volatile__("fldcw %0" : : "m" (cw));
	}
	return cur;
}

unsigned int __cdecl _controlfp(unsigned int newval, unsigned int mask)
{
	return _control87(newval, mask & ~0x00080000u);
}
