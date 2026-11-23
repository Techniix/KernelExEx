/*
 *  KernelExEx - KEXRAMT.VXD: test only, never installed. Checks, at Device_Init,
 *  that a VxD can reach RAM Windows 98 does not use (above KEXMEM's limit, and
 *  above 4 GB) through one 4 MB window: a page-directory entry with PS=1 whose
 *  physical address bits 32-35 come from PSE-36. Results go to port 0xE9
 *  (QEMU -debugcon). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "../../../toolchain/vxd/include/vmm.h"

#define ___MapPhysToLinear 0x0001006C
__declspec(naked) ULONG __cdecl _MapPhysToLinear(ULONG phys, ULONG nbytes, ULONG flags)
{
	VMMJmp(_MapPhysToLinear)
}

static void dc(char c) { __asm__ __volatile__("outb %0, $0xe9" : : "a"(c)); }
static void ds(const char *s) { while (*s) dc(*s++); }
static void dx(ULONG v) { int i; for (i = 28; i >= 0; i -= 4) dc("0123456789abcdef"[(v >> i) & 15]); }
static void kv(const char *k, ULONG v) { ds(k); ds("="); dx(v); ds(" "); }

static ULONG cr3(void) { ULONG v; __asm__ __volatile__("movl %%cr3, %0" : "=r"(v)); return v; }
static ULONG cr4(void) { ULONG v; __asm__ __volatile__("movl %%cr4, %0" : "=r"(v)); return v; }
static void set_cr4(ULONG v) { __asm__ __volatile__("movl %0, %%cr4" : : "r"(v)); }
static void invlpg(void *p) { __asm__ __volatile__("invlpg (%0)" : : "r"(p) : "memory"); }
static ULONG cpuid_edx(void) { ULONG a = 1, b, c, d; __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d)); return d; }

static volatile ULONG *pde;     /* the window's page-directory entry */
static ULONG *win;              /* its linear address (4 MB aligned) */

/* map the 4 MB at physical (hi:lo), lo 4 MB aligned */
static void map(ULONG hi, ULONG lo)
{
	*pde = (lo & 0xFFC00000) | (hi & 0xF) << 13 | 0x83;   /* P | RW | PS */
	invlpg(win);
}

static void test(void)
{
	ULONG d = cpuid_edx(), lin, pdl, old, i, ok = 1;
	static const ULONG hi_lo[][2] = { { 0, 0x90000000 }, { 1, 0x00000000 }, { 1, 0x00400000 }, { 1, 0x40000000 } };
	{
		/* KEXMEM's saved BIOS memory map (BDA 0040:00F0 = 'KX', offset, segment) */
		const unsigned short *ica = (const unsigned short *) 0x4F0;
		ds("\nKEXRAMT map: ");
		if (ica[0] == 0x584B)
		{
			const unsigned char *t = (const unsigned char *) ((ULONG) ica[2] * 16 + ica[1]);
			unsigned n = *(const unsigned short *) (t + 8), k;
			kv("limit", *(const ULONG *) (t + 4));
			kv("n", n);
			for (k = 0; k < n && k < 32; k++)
			{
				const ULONG *e = (const ULONG *) (t + 10 + 20 * k);
				ds("\n  ");
				dx(e[1]); dx(e[0]); ds(" +"); dx(e[3]); dx(e[2]); ds(" type "); dx(e[4]);
			}
		}
		else
			ds("none");
	}
	ds("\nKEXRAMT: ");
	kv("cpuid.edx", d);
	kv("cr3", cr3());
	kv("cr4", cr4());
	if (!(d & 8) || !(d & 0x20000))
	{
		ds("no PSE/PSE-36\n");
		return;
	}
	/* any 8 MB of system linear space whose page tables we may override: map some RAM */
	lin = _MapPhysToLinear(0x90000000, 0x800000, 0);
	pdl = _MapPhysToLinear(cr3() & ~0xFFF, 0x1000, 0);
	kv("lin", lin);
	kv("pd", pdl);
	if (lin == 0xFFFFFFFF || pdl == 0xFFFFFFFF)
		return;
	win = (ULONG *) ((lin + 0x3FFFFF) & ~0x3FFFFF);
	pde = (volatile ULONG *) pdl + ((ULONG) win >> 22);
	old = *pde;
	kv("win", (ULONG) win);
	kv("oldpde", old);
	set_cr4(cr4() | 0x10);   /* PSE */
	for (i = 0; i < 4; i++)  /* a different tag in each window */
	{
		map(hi_lo[i][0], hi_lo[i][1]);
		win[0] = 0x4B580000 + i;
		win[0xFFFFF] = 0x4B590000 + i;
	}
	for (i = 0; i < 4; i++)
	{
		map(hi_lo[i][0], hi_lo[i][1]);
		ds("\n  ");
		kv("hi", hi_lo[i][0]);
		kv("lo", hi_lo[i][1]);
		kv("first", win[0]);
		kv("last", win[0xFFFFF]);
		if (win[0] != 0x4B580000 + i || win[0xFFFFF] != 0x4B590000 + i)
			ok = 0;
	}
	*pde = old;
	invlpg(win);
	ds(ok ? "\nKEXRAMT: OK\n" : "\nKEXRAMT: MISMATCH\n");
}

__attribute__((used)) BOOL __stdcall kexramt_dispatch(DWORD msg)
{
	if (msg == 3)   /* Device_Init */
		test();
	return TRUE;
}

__attribute__((naked)) void KEXRAMT_CTRL(void)
{
	__asm__("pushl %eax\n\t"
	        "call _kexramt_dispatch@4\n\t"
	        "cmpl $1, %eax\n\t"
	        "ret");
}

struct VxD_Desc_Block KEXRAMT_DDB = {
	0, DDK_VERSION, UNDEFINED_DEVICE_ID, 1, 0, 0,
	{ 'K', 'E', 'X', 'R', 'A', 'M', 'T', ' ' }, UNDEFINED_INIT_ORDER, (ULONG) KEXRAMT_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x50726576, sizeof(struct VxD_Desc_Block),
	0x52737631, 0x52737632, 0x52737633,
};
