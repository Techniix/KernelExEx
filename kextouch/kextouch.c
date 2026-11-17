/*
 *  KernelExEx - kextouch.exe: the touchpad driver's helper.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Started from the Run key. Applies the settings (HKCU\Software\KernelExEx\Touch)
 *  to KEXTOUCH.VXD and turns the driver's scroll amounts into wheel events
 *  (mouse_event MOUSEEVENTF_WHEEL, whole notches as Windows 98 programs expect).
 *  "kextouch /settings" (Start menu: Touchpad settings) opens the settings window,
 *  which can also save a report (device found, counters, driver log) for bug reports.
 */
#include <windows.h>
#include <commctrl.h>
#include "../drivers/input/touch/tp.h"
#include "../drivers/input/touch/kextouch.h"
#include "resource.h"

#ifndef MOUSEEVENTF_WHEEL
#define MOUSEEVENTF_WHEEL 0x0800
#endif

static HANDLE vxd = INVALID_HANDLE_VALUE;
static struct tp_settings set;
static const char KEY[] = "Software\\KernelExEx\\Touch";

static const struct
{
	const char *name;
	int off;
	int def;
	int ctl;
} fields[] = {
	{ "Tap", 0, 1, IDC_TAP },
	{ "TapDrag", 1, 1, IDC_TAPDRAG },
	{ "TwoFingerScroll", 2, 1, IDC_SCROLL2 },
	{ "EdgeScroll", 3, 1, IDC_SCROLLEDGE },
	{ "ReverseScroll", 4, 0, IDC_REVERSE },
	{ "ScrollSpeed", 5, 10, IDC_SSPEED },
	{ "Speed", 6, 10, IDC_SPEED },
	{ "StickScroll", 7, 1, IDC_STICKSCROLL },
	{ "StickSpeed", 8, 10, IDC_STSPEED },
	{ "PalmCheck", 9, 1, 0 },
};
#define NFIELD (sizeof(fields) / sizeof(fields[0]))
#define FIELD(i) (((int *) &set)[fields[i].off])

static void load_settings(void)
{
	HKEY k;
	unsigned i;
	for (i = 0; i < NFIELD; i++)
		FIELD(i) = fields[i].def;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
		return;
	for (i = 0; i < NFIELD; i++)
	{
		DWORD v, sz = sizeof(v), type;
		if (RegQueryValueExA(k, fields[i].name, NULL, &type, (BYTE *) &v, &sz) == ERROR_SUCCESS && type == REG_DWORD)
			FIELD(i) = (int) v;
	}
	RegCloseKey(k);
}

static void save_settings(void)
{
	HKEY k;
	unsigned i;
	if (RegCreateKeyExA(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
		return;
	for (i = 0; i < NFIELD; i++)
	{
		DWORD v = (DWORD) FIELD(i);
		RegSetValueExA(k, fields[i].name, 0, REG_DWORD, (const BYTE *) &v, sizeof(v));
	}
	RegCloseKey(k);
}

static int open_vxd(void)
{
	/* the driver is loaded by VMOUSE; open it by its device name */
	vxd = CreateFileA("\\\\.\\KEXTOUCH", 0, 0, NULL, 0, 0, NULL);
	return vxd != INVALID_HANDLE_VALUE;
}

static void apply(void)
{
	DWORD got;
	if (vxd != INVALID_HANDLE_VALUE)
		DeviceIoControl(vxd, KEXTOUCH_IOCTL_SET, &set, sizeof(set), NULL, 0, &got, NULL);
}

static int get_status(struct kextouch_status *s)
{
	DWORD got = 0;
	ZeroMemory(s, sizeof(*s));
	return vxd != INVALID_HANDLE_VALUE &&
	       DeviceIoControl(vxd, KEXTOUCH_IOCTL_STATUS, NULL, 0, s, sizeof(*s), &got, NULL) && got >= sizeof(*s);
}

/* ---- the report ---- */
static void save_report(HWND owner)
{
	static char log[8192];
	char path[MAX_PATH], line[512];
	struct kextouch_status s;
	OSVERSIONINFOA ov;
	HANDLE f;
	DWORD got = 0, w;
	SYSTEMTIME t;
	if (!GetWindowsDirectoryA(path, sizeof(path)))
		return;
	lstrcpyA(path + 3, "KEXTOUCH.TXT");   /* C:\KEXTOUCH.TXT */
	f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
	{
		MessageBoxA(owner, "Could not write the report.", "Touchpad", MB_ICONERROR);
		return;
	}
	GetLocalTime(&t);
	ov.dwOSVersionInfoSize = sizeof(ov);
	GetVersionExA(&ov);
	wsprintfA(line, "KernelExEx touchpad report, %04u-%02u-%02u %02u:%02u\r\nWindows %lu.%lu.%lu %s\r\n\r\n", t.wYear,
	          t.wMonth, t.wDay, t.wHour, t.wMinute, ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber & 0xffff,
	          ov.szCSDVersion);
	WriteFile(f, line, (DWORD) lstrlenA(line), &w, NULL);
	if (get_status(&s))
	{
		wsprintfA(line,
		          "Driver: running %u\r\nDevice: %s (kind %u)\r\nRange: %u x %u, stick %u\r\n"
		          "Bytes %u, packets %u, posts %u, command errors %u, resyncs %u\r\n"
		          "Settings: tap %d drag %d two-finger %d edge %d reverse %d scroll speed %d speed %d stick scroll %d "
		          "stick speed %d\r\n\r\nDriver log:\r\n",
		          s.running, s.name, s.kind, s.x_max, s.y_max, s.has_stick, s.bytes, s.packets, s.posts, s.errors,
		          s.resyncs, s.set.tap, s.set.tap_drag, s.set.scroll_two, s.set.scroll_edge, s.set.scroll_reverse,
		          s.set.scroll_speed, s.set.speed, s.set.stick_scroll, s.set.stick_speed);
		WriteFile(f, line, (DWORD) lstrlenA(line), &w, NULL);
		if (DeviceIoControl(vxd, KEXTOUCH_IOCTL_LOG, NULL, 0, log, sizeof(log), &got, NULL))
		{
			DWORD i, j = 0;
			static char crlf[sizeof(log) * 2];
			for (i = 0; i < got; i++)
			{
				if (log[i] == '\n')
					crlf[j++] = '\r';
				crlf[j++] = log[i];
			}
			WriteFile(f, crlf, j, &w, NULL);
		}
	}
	else
	{
		lstrcpyA(line, "KEXTOUCH.VXD is not loaded: the mouse is not using the KernelExEx driver.\r\n"
		               "(Device Manager > Mouse > Properties > Driver should show kextouch.vxd.)\r\n");
		WriteFile(f, line, (DWORD) lstrlenA(line), &w, NULL);
	}
	CloseHandle(f);
	wsprintfA(line, "Saved %s", path);
	MessageBoxA(owner, line, "Touchpad", MB_ICONINFORMATION);
}

/* ---- settings window ---- */
static void show_status(HWND d)
{
	struct kextouch_status s;
	char buf[160];
	if (get_status(&s))
		wsprintfA(buf, "%s%s", s.name[0] ? s.name : "(none)", s.running ? "" : " - not running");
	else
		lstrcpyA(buf, "The KernelExEx touchpad driver is not active for this mouse.");
	SetDlgItemTextA(d, IDC_DEVICE, buf);
}

static INT_PTR CALLBACK dlg(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
	unsigned i;
	(void) lp;
	switch (msg)
	{
	case WM_INITDIALOG:
		for (i = 0; i < NFIELD; i++)
		{
			if (!fields[i].ctl)
				continue;
			if (fields[i].ctl == IDC_SSPEED || fields[i].ctl == IDC_SPEED || fields[i].ctl == IDC_STSPEED)
			{
				SendDlgItemMessageA(d, fields[i].ctl, TBM_SETRANGE, TRUE, MAKELONG(1, 20));
				SendDlgItemMessageA(d, fields[i].ctl, TBM_SETPOS, TRUE, FIELD(i));
			}
			else
				CheckDlgButton(d, fields[i].ctl, FIELD(i) ? BST_CHECKED : BST_UNCHECKED);
		}
		show_status(d);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDOK:
		case IDC_APPLY:
			for (i = 0; i < NFIELD; i++)
			{
				if (!fields[i].ctl)
					continue;
				if (fields[i].ctl == IDC_SSPEED || fields[i].ctl == IDC_SPEED || fields[i].ctl == IDC_STSPEED)
					FIELD(i) = (int) SendDlgItemMessageA(d, fields[i].ctl, TBM_GETPOS, 0, 0);
				else
					FIELD(i) = IsDlgButtonChecked(d, fields[i].ctl) == BST_CHECKED;
			}
			save_settings();
			apply();
			if (LOWORD(wp) == IDOK)
				EndDialog(d, 1);
			return TRUE;
		case IDCANCEL:
			EndDialog(d, 0);
			return TRUE;
		case IDC_REPORT:
			save_report(d);
			return TRUE;
		case IDC_REDETECT:
		{
			DWORD got;
			if (vxd != INVALID_HANDLE_VALUE)
				DeviceIoControl(vxd, KEXTOUCH_IOCTL_RESET, NULL, 0, NULL, 0, &got, NULL);
			apply();
			show_status(d);
			return TRUE;
		}
		}
		break;
	}
	return FALSE;
}

void entrypoint(void)
{
	const char *cmd = GetCommandLineA();
	int settings = 0, report = 0;
	HANDLE mutex;
	int acc = 0;
	{
		const char *p;
		for (p = cmd; *p; p++)
			if (*p == '/' || *p == '-')
			{
				if ((p[1] | 0x20) == 's')
					settings = 1;
				if ((p[1] | 0x20) == 'r')
					report = 1;
			}
	}
	load_settings();
	open_vxd();
	if (settings || report)
	{
		INITCOMMONCONTROLSEX ic;
		ic.dwSize = sizeof(ic);
		ic.dwICC = ICC_BAR_CLASSES;
		InitCommonControlsEx(&ic);
		if (report)
			save_report(NULL);
		else
			DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_SETTINGS), NULL, dlg, 0);
		ExitProcess(0);
	}

	mutex = CreateMutexA(NULL, TRUE, "KernelExEx.Touch");
	if (GetLastError() == ERROR_ALREADY_EXISTS || vxd == INVALID_HANDLE_VALUE)
		ExitProcess(0);
	apply();
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	for (;;)
	{
		struct kextouch_wheel w;
		DWORD got = 0;
		if (!DeviceIoControl(vxd, KEXTOUCH_IOCTL_WHEEL, NULL, 0, &w, sizeof(w), &got, NULL) || got != sizeof(w))
		{
			Sleep(500);
			continue;
		}
		acc += w.wheel;
		/* whole notches only: many Windows 98 programs ignore anything else */
		while (acc >= WHEEL_DELTA || acc <= -WHEEL_DELTA)
		{
			int n = acc >= WHEEL_DELTA ? WHEEL_DELTA : -WHEEL_DELTA;
			mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD) n, 0);
			acc -= n;
		}
		/* horizontal scrolling has no Windows 98 message: dropped for now */
		Sleep(w.wheel ? 5 : 15);
	}
}
