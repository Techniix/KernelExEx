/*
 *  KernelExEx - ACPITEST.EXE: test client for KEXACPI.VXD (not shipped).
 *  Reads the status N times (argument, default 3), 2 seconds apart, and
 *  writes every field to C:\ACPITEST.TXT. GPLv2.
 */
#include <windows.h>
#include <stdio.h>
#include "kexacpi.h"

static HANDLE out;

static void say(const char *fmt, ...)
{
	char buf[512];
	DWORD n;
	va_list ap;
	va_start(ap, fmt);
	n = (DWORD) vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	WriteFile(out, buf, n, &n, NULL);
}

void entrypoint(void)
{
	HANDLE h;
	KEXACPI_STATUS st;
	DWORD n, t0;
	int rounds = 3, r, i;
	const char *cmd = GetCommandLineA();
	const char *sp = strrchr(cmd, ' ');
	if (sp && sp[1] >= '1' && sp[1] <= '9')
		rounds = atoi(sp + 1);
	out = CreateFileA("C:\\ACPITEST.TXT", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	t0 = GetTickCount();
	h = CreateFileA("\\\\.\\KEXACPI.VXD", 0, 0, NULL, 0, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		say("open: error %lu\r\n", GetLastError());
		CloseHandle(out);
		ExitProcess(1);
	}
	say("open: ok (%lu ms)\r\n", GetTickCount() - t0);
	for (r = 0; r < rounds; r++)
	{
		memset(&st, 0, sizeof(st));
		t0 = GetTickCount();
		if (!DeviceIoControl(h, KEXACPI_IOCTL_STATUS, NULL, 0, &st, sizeof(st), &n, NULL))
		{
			say("round %d: error %lu\r\n", r, GetLastError());
			continue;
		}
		say("round %d (%lu ms): size %lu version %lx state %lu acpi_status %lx ac %ld lid %ld nbat %lu ec %lu\r\n", r,
		    GetTickCount() - t0, st.size, st.version, st.state, st.acpi_status, st.ac, st.lid, st.nbat, st.have_ec);
		for (i = 0; i < (int) st.nbat && i < KEXACPI_MAXBAT; i++)
		{
			KEXACPI_BATTERY *b = &st.bat[i];
			say("  bat %d %s: present %lu bix %lu units %lu design %lu full %lu volt %lu warn %lu low %lu cycles %ld "
			    "rech %lu\r\n", i, b->path, b->present, b->has_bix, b->units, b->design_cap, b->full_cap,
			    b->design_volt, b->warn_cap, b->low_cap, (long) b->cycles, b->rechargeable);
			say("    state %lu rate %lu remaining %lu voltage %lu percent %ld left %ld full-in %ld\r\n", b->state,
			    b->rate, b->remaining, b->voltage, (long) b->percent, (long) b->minutes_left, (long) b->minutes_full);
			say("    model '%s' serial '%s' type '%s' oem '%s'\r\n", b->model, b->serial, b->type, b->oem);
		}
		Sleep(2000);
	}
	CloseHandle(h);
	CloseHandle(out);
	ExitProcess(0);
}
