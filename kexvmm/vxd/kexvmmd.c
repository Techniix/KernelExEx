/*
 *  KernelExEx - KEXVMM.VXD: the ring-0 half of mouse integration.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  QEMU's and VMware's vmmouse is reached through the VMware "backdoor":
 *  an IN from port 5658h with a magic number in EAX and the command and
 *  arguments in EBX/ECX, the answer coming back in EAX-EDX. Windows 98
 *  traps that port for Win32 programs and performs the IN itself, without
 *  the other registers, so kexvmm.exe cannot do it. This dynamic VxD, which
 *  kexvmm.exe loads with CreateFile("\\.\KEXVMM.VXD"), does it for it.
 *
 *  It accepts only the backdoor port, the magic number and the commands
 *  mouse integration uses, so it cannot be used as general port access.
 */
#include <windows.h>
#include <vmm.h>
#include <vwin32.h>

#define BDOOR_MAGIC 0x564D5868UL
#define BDOOR_PORT 0x5658
#define KEXVMM_IOCTL_BACKDOOR 1

struct bd_regs
{
	ULONG a, b, c, d;
};

static int allowed(const struct bd_regs *r)
{
	ULONG cmd = r->c & 0xffff;
	return r->a == BDOOR_MAGIC && (r->d & 0xffff) == BDOOR_PORT &&
	       (cmd == 10 /* version */ || cmd == 39 /* vmmouse data */ || cmd == 40 /* status */ ||
	        cmd == 41 /* command */);
}

static ULONG ioctl(PDIOCPARAMETERS p)
{
	struct bd_regs r;
	if (p->dwIoControlCode == DIOC_OPEN || p->dwIoControlCode == DIOC_CLOSEHANDLE)
		return 0;
	if (p->dwIoControlCode != KEXVMM_IOCTL_BACKDOOR || p->cbInBuffer < sizeof(r) || p->cbOutBuffer < sizeof(r) ||
	    !p->lpvInBuffer || !p->lpvOutBuffer)
		return ERROR_INVALID_PARAMETER;
	r = *(const struct bd_regs *) p->lpvInBuffer;
	if (!allowed(&r))
		return ERROR_ACCESS_DENIED;
	__asm__ __volatile__("inl %%dx, %%eax" : "+a"(r.a), "+b"(r.b), "+c"(r.c), "+d"(r.d) : : "memory");
	*(struct bd_regs *) p->lpvOutBuffer = r;
	if (p->lpcbBytesReturned)
		*(DWORD *) p->lpcbBytesReturned = sizeof(r);
	return 0;
}

__attribute__((used)) BOOL __stdcall kexvmm_dispatch(DWORD msg, DWORD ebx, DWORD ecx, DWORD edx, DWORD esi, DWORD edi)
{
	(void) ebx;
	(void) ecx;
	(void) edx;
	(void) edi;
	switch (msg)
	{
	case W32_DEVICEIOCONTROL:
		return (BOOL) ioctl((PDIOCPARAMETERS) esi);
	default:
		return TRUE;   /* SYS_DYNAMIC_DEVICE_INIT/EXIT and the rest: nothing to do */
	}
}

/* the control procedure: VMM passes the message in EAX and its arguments in
 * registers; carry clear means success (as VKrnlEx's thunk) */
__attribute__((naked)) void KEXVMM_CTRL(void)
{
	__asm__("pushl %edi\n\t"
	        "pushl %esi\n\t"
	        "pushl %edx\n\t"
	        "pushl %ecx\n\t"
	        "pushl %ebx\n\t"
	        "pushl %eax\n\t"
	        "call _kexvmm_dispatch@24\n\t"
	        "cmpl $1, %eax\n\t"
	        "ret");
}

struct VxD_Desc_Block KEXVMM_DDB = {
	0, DDK_VERSION, UNDEFINED_DEVICE_ID, 1, 0, 0,
	{ 'K', 'E', 'X', 'V', 'M', 'M', ' ', ' ' }, UNDEFINED_INIT_ORDER, (ULONG) KEXVMM_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x76657250 /* 'Prev' */, sizeof(struct VxD_Desc_Block),
	0x31767352, 0x32767352, 0x33767352,
};
