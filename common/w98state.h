/*
 *  KernelExEx - is Windows far enough up for a driver to open files?
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  A WDM driver that opens a file by name while Windows 98 is still
 *  initializing devices can hang the machine for good: IFSMgr checks the
 *  name against the DOS device chain, whose head is still 0 then, and walks
 *  the interrupt vector table forever. This happens when TCP/IP waits for a
 *  DHCP answer at boot, which keeps Windows in device init for a while.
 *
 *  VMM_GetSystemInitState (VMM service 0111h) tells: measured on Windows 98
 *  SE, 10000000h during device init, 50000000h once Kernel32 is up. Files
 *  are opened only from then on.
 */
#ifndef KEX_W98STATE_H
#define KEX_W98STATE_H

#define KEX_SYSSTATE_KERNEL32_UP 0x50000000UL

static __inline unsigned long kex_init_state(void)
{
	unsigned long r;
	/* a VxD dynamic link: VMM patches it into a call on first use */
	__asm__ __volatile__("int $0x20\n\t.long 0x00010111" : "=a"(r) : : "ecx", "edx", "memory", "cc");
	return r;
}

static __inline int kex_files_ready(void)
{
	return kex_init_state() >= KEX_SYSSTATE_KERNEL32_UP;
}

#endif
