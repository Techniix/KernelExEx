/*
 *  KernelExEx - kexvmm.exe: mouse integration in QEMU and VMware. The
 *  pointer follows the host's mouse: it moves in and out of the virtual
 *  machine's window freely, with no click to capture it and no keys to
 *  release it.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Both QEMU (its "vmport" device, on by default in the PC machine) and
 *  VMware offer the vmmouse interface through the VMware backdoor I/O port
 *  (0x5658): once the guest asks for absolute positions, the host stops
 *  grabbing the mouse and hands out positions 0-65535 across the screen,
 *  button states and wheel steps. This program reads them a hundred times
 *  a second and gives them to Windows with mouse_event (absolute moves),
 *  through its small VxD (KEXVMM.VXD): Windows 98 traps that port for Win32
 *  programs.
 *
 *  The host also sends the PS/2 mouse a 1-pixel event for each change, so
 *  the interrupt fires; the next absolute position puts the pointer right.
 *
 *  It starts from the Run key, and quits at once outside a virtual machine
 *  (no hypervisor CPUID bit and no SeaBIOS/QEMU/VMware/Bochs name in the
 *  BIOS) or when the backdoor does not answer.
 */
#include <windows.h>
#include <string.h>

#define BDOOR_MAGIC 0x564D5868UL
#define BDOOR_PORT 0x5658
#define BDOOR_GETVERSION 10
#define VMMOUSE_DATA 39
#define VMMOUSE_STATUS 40
#define VMMOUSE_COMMAND 41
#define VMMOUSE_CMD_ENABLE 0x45414552UL
#define VMMOUSE_CMD_DISABLE 0x000000f5UL
#define VMMOUSE_CMD_ABSOLUTE 0x53424152UL
#define VMMOUSE_CMD_RELATIVE 0x4c455252UL
#define VMMOUSE_VERSION_ID 0x3442554aUL
#define VMMOUSE_ERROR 0xffff0000UL
#define VMMOUSE_LEFT 0x20
#define VMMOUSE_RIGHT 0x10
#define VMMOUSE_MIDDLE 0x08

#ifndef MOUSEEVENTF_WHEEL
#define MOUSEEVENTF_WHEEL 0x0800
#endif

struct regs
{
	unsigned long a, b, c, d;
};

/* the backdoor through KEXVMM.VXD: Windows 98 traps the port for Win32
 * programs and performs the IN without EBX/ECX (kexvmm/vxd/kexvmmd.c) */
static HANDLE vxd = INVALID_HANDLE_VALUE;

static int open_vxd(void)
{
	char path[MAX_PATH + 8], *slash;
	lstrcpyA(path, "\\\\.\\");
	GetModuleFileNameA(NULL, path + 4, MAX_PATH);
	slash = path + lstrlenA(path);
	while (slash > path + 4 && slash[-1] != '\\')
		slash--;
	lstrcpyA(slash, "KEXVMM.VXD");
	vxd = CreateFileA(path, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
	return vxd != INVALID_HANDLE_VALUE;
}

static void backdoor(unsigned long cmd, unsigned long arg, struct regs *r)
{
	struct regs in;
	DWORD got = 0;
	in.a = BDOOR_MAGIC;
	in.b = arg;
	in.c = cmd;
	in.d = BDOOR_PORT;
	if (!DeviceIoControl(vxd, 1, &in, sizeof(in), r, sizeof(*r), &got, NULL) || got != sizeof(*r))
	{
		r->a = 0xffffffffUL;   /* an error status for vmmouse, nothing for GETVERSION */
		r->b = r->c = r->d = 0;
	}
}

static int hypervisor_bit(void)
{
	unsigned long a, b, c, d, fl1, fl2;
	/* CPUID itself: the ID flag (bit 21) can be toggled */
	__asm__ __volatile__("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; pushl %1; popfl"
	                     : "=&r"(fl1), "=&r"(fl2));
	if (!((fl1 ^ fl2) & 0x200000))
		return 0;
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1UL), "c"(0UL));
	return (c >> 31) & 1;
}

/* Windows 9x maps the first megabyte at linear 0: look for a virtual
 * machine's BIOS name in the BIOS area (E0000-FFFFF) */
static int vm_bios(void)
{
	static const char *const names[] = { "SeaBIOS", "QEMU", "VMware", "Bochs", "BOCHS" };
	const unsigned char *p = (const unsigned char *) 0xE0000;
	unsigned long n = 0x20000, i;
	unsigned int k;
	if (IsBadReadPtr(p, n))
		return 0;
	for (i = 0; i + 8 < n; i++)
		for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
		{
			size_t l = strlen(names[k]);
			if (p[i] == (unsigned char) names[k][0] && !memcmp(p + i, names[k], l))
				return 1;
		}
	return 0;
}

static int vmmouse_enable(void)
{
	struct regs r;
	backdoor(VMMOUSE_COMMAND, VMMOUSE_CMD_ENABLE, &r);
	backdoor(VMMOUSE_STATUS, 0, &r);
	if ((r.a & VMMOUSE_ERROR) == VMMOUSE_ERROR || (r.a & 0xffff) == 0)
		return 0;
	backdoor(VMMOUSE_DATA, 1, &r);
	if (r.a != VMMOUSE_VERSION_ID)
		return 0;
	backdoor(VMMOUSE_COMMAND, VMMOUSE_CMD_ABSOLUTE, &r);
	return 1;
}

static void vmmouse_disable(void)
{
	struct regs r;
	backdoor(VMMOUSE_COMMAND, VMMOUSE_CMD_RELATIVE, &r);
	backdoor(VMMOUSE_COMMAND, VMMOUSE_CMD_DISABLE, &r);
}

int wsprintfA(char *, const char *, ...);

/* KEXVMM.LOG in the Windows folder: why it stopped, or that it runs */
static void note(const char *fmt, unsigned long a, unsigned long b)
{
	char path[MAX_PATH], line[160];
	HANDLE h;
	DWORD n;
	UINT l = GetWindowsDirectoryA(path, MAX_PATH - 16);
	lstrcpyA(path + l, path[l - 1] == '\\' ? "KEXVMM.LOG" : "\\KEXVMM.LOG");
	n = (DWORD) wsprintfA(line, fmt, a, b);
	h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	SetFilePointer(h, 0, NULL, FILE_END);
	WriteFile(h, line, n, &n, NULL);
	CloseHandle(h);
}

static volatile int quit;

static BOOL WINAPI on_ctrl(DWORD t)
{
	(void) t;
	quit = 1;
	return TRUE;
}

void entrypoint(void)
{
	struct regs r;
	HANDLE mutex;
	unsigned long buttons = 0, errors = 0;

	mutex = CreateMutex(NULL, TRUE, "KernelExEx.VmMouse");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
		ExitProcess(0);
	{
		int hv = hypervisor_bit(), bios = vm_bios();
		if (!hv && !bios)
		{
			note("not a virtual machine: exiting\r\n", 0, 0);
			ExitProcess(0);   /* a real PC: the backdoor port is not ours to touch */
		}
		note("virtual machine (hypervisor bit %lu, BIOS name %lu)\r\n", (unsigned long) hv, (unsigned long) bios);
	}
	if (!open_vxd())
	{
		note("cannot load KEXVMM.VXD (error %lu): exiting\r\n", GetLastError(), 0);
		ExitProcess(0);
	}
	backdoor(BDOOR_GETVERSION, ~BDOOR_MAGIC, &r);
	if (r.b != BDOOR_MAGIC)
	{
		note("no VMware backdoor (ebx %08lx): exiting\r\n", r.b, 0);
		ExitProcess(0);
	}
	if (!vmmouse_enable())
	{
		backdoor(VMMOUSE_STATUS, 0, &r);
		note("backdoor version %lu, but no vmmouse (status %08lx): exiting\r\n", r.a, r.a);
		ExitProcess(0);
	}
	note("vmmouse on: absolute pointer\r\n", 0, 0);
	SetConsoleCtrlHandler(on_ctrl, TRUE);
	/* above normal, so the pointer keeps up while programs are busy */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	while (!quit)
	{
		unsigned long queued;
		backdoor(VMMOUSE_STATUS, 0, &r);
		if ((r.a & VMMOUSE_ERROR) == VMMOUSE_ERROR)
		{
			/* the host reset the device (a VM reset, a resume): again */
			if (++errors > 50)
				break;
			Sleep(200);
			vmmouse_enable();
			continue;
		}
		errors = 0;
		queued = r.a & 0xffff;
		if (queued < 4)
		{
			Sleep(10);
			continue;
		}
		while (queued >= 4)
		{
			unsigned long st, b;
			long x, y;
			int z;
			DWORD flags = 0;
			backdoor(VMMOUSE_DATA, 4, &r);
			queued -= 4;
			st = r.a;
			x = (long) (r.b & 0xffff);
			y = (long) (r.c & 0xffff);
			z = (int) (signed char) (r.d & 0xff);
			b = st & (VMMOUSE_LEFT | VMMOUSE_RIGHT | VMMOUSE_MIDDLE);
			if (st & 0x00010000UL)
				continue;   /* a relative packet: not asked for */
			/* every packet repositions the pointer, even when the host's
			 * position did not change: QEMU nudges the PS/2 mouse by one
			 * pixel per event to raise its interrupt, and without this the
			 * pointer drifts (four clicks were too far apart for Windows to
			 * see a double-click) */
			if ((b ^ buttons) & VMMOUSE_LEFT)
				flags |= (b & VMMOUSE_LEFT) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
			if ((b ^ buttons) & VMMOUSE_RIGHT)
				flags |= (b & VMMOUSE_RIGHT) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
			if ((b ^ buttons) & VMMOUSE_MIDDLE)
				flags |= (b & VMMOUSE_MIDDLE) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
			mouse_event(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | flags, (DWORD) x, (DWORD) y, 0, 0);
			buttons = b;
			if (z)   /* the host counts down for "away from the user" */
				mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD) (-z * WHEEL_DELTA), 0);
		}
	}
	vmmouse_disable();
	if (mutex)
		CloseHandle(mutex);
	ExitProcess(0);
}
