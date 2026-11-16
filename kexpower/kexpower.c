/*
 *  KernelExEx - kexpower.exe: the battery for Windows 98 SE / ME on laptops
 *  whose battery Windows cannot read (no APM BIOS, ACPI methods only). A
 *  taskbar icon with the charge, and a window with each battery's details,
 *  low-battery warnings and what closing the lid does. The readings come from
 *  KEXACPI.VXD (drivers/acpi/kexacpi.h).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  kexpower.exe /tray   taskbar icon only (from the Run key); exits at once on
 *                       a machine without a battery or lid
 *  kexpower.exe         open the window (or bring the running one forward)
 *
 *  Settings: HKEY_CURRENT_USER\Software\KernelExEx\Power
 *    TrayIcon (1), Warn (1), WarnPercent (10), Critical (0), CriticalPercent (5),
 *    Lid (0: nothing, 1: turn the screen off, 2: shut down)
 */
#define _WIN32_WINNT 0x0400
#define _WIN32_IE 0x0400
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include "resource.h"
#include "../drivers/acpi/kexacpi.h"

int snprintf(char *, size_t, const char *, ...);

#define WM_TRAY (WM_APP + 1)
#define WM_SHOWMAIN (WM_APP + 2)
#define TRAY_CLASS "KexPowerTray"
#define KEY "Software\\KernelExEx\\Power"
#define POLL_MS 4000
#define MENU_OPEN 1
#define MENU_EXIT 2

static HINSTANCE inst;
static HWND tray_wnd, main_dlg, crit_dlg;
static UINT msg_taskbar_created;
static int tray_shown;
static HANDLE dev = INVALID_HANDLE_VALUE;
static DWORD dev_error;
static KEXACPI_STATUS st;
static int have_status;
static int cur_bat;

static struct
{
	int tray, warn, warn_pc, crit, crit_pc, lid;
} opt = { 1, 1, 10, 0, 5, 0 };

static int warned, crit_started, last_lid = -1, screen_off;

/* ---- settings ----------------------------------------------------------------------------- */
static DWORD get_dword(HKEY h, const char *name, DWORD def)
{
	DWORD v, t, n = sizeof(v);
	if (RegQueryValueEx(h, name, NULL, &t, (BYTE *) &v, &n) == ERROR_SUCCESS && t == REG_DWORD)
		return v;
	return def;
}

static void load_options(void)
{
	HKEY h;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &h) != ERROR_SUCCESS)
		return;
	opt.tray = get_dword(h, "TrayIcon", 1) != 0;
	opt.warn = get_dword(h, "Warn", 1) != 0;
	opt.warn_pc = (int) get_dword(h, "WarnPercent", 10);
	opt.crit = get_dword(h, "Critical", 0) != 0;
	opt.crit_pc = (int) get_dword(h, "CriticalPercent", 5);
	opt.lid = (int) get_dword(h, "Lid", 0);
	RegCloseKey(h);
	if (opt.warn_pc < 1 || opt.warn_pc > 90)
		opt.warn_pc = 10;
	if (opt.crit_pc < 1 || opt.crit_pc > 50)
		opt.crit_pc = 5;
	if (opt.lid < 0 || opt.lid > 2)
		opt.lid = 0;
}

static void save_options(void)
{
	HKEY h;
	DWORD d;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_WRITE, NULL, &h, &d) != ERROR_SUCCESS)
		return;
#define PUT(n, v) (d = (DWORD) (v), RegSetValueEx(h, n, 0, REG_DWORD, (BYTE *) &d, sizeof(d)))
	PUT("TrayIcon", opt.tray);
	PUT("Warn", opt.warn);
	PUT("WarnPercent", opt.warn_pc);
	PUT("Critical", opt.crit);
	PUT("CriticalPercent", opt.crit_pc);
	PUT("Lid", opt.lid);
#undef PUT
	RegCloseKey(h);
}

/* ---- readings ------------------------------------------------------------------------------ */
static int poll(void)
{
	DWORD n;
	if (dev == INVALID_HANDLE_VALUE)
	{
		dev = CreateFile("\\\\.\\KEXACPI.VXD", 0, 0, NULL, 0, 0, NULL);
		if (dev == INVALID_HANDLE_VALUE)
		{
			dev_error = GetLastError();
			have_status = 0;
			return 0;
		}
	}
	if (!DeviceIoControl(dev, KEXACPI_IOCTL_STATUS, NULL, 0, &st, sizeof(st), &n, NULL) || st.size != sizeof(st))
	{
		/* busy (another program is reading it): keep the last reading */
		return have_status;
	}
	have_status = 1;
	return 1;
}

/* all present batteries together (they report in the same units on one machine) */
static void total(ULONG *pc, ULONG *mins, int *charging, int *discharging, int *critical)
{
	unsigned long long rem = 0, full = 0, rate = 0;
	int i, n = 0;
	*pc = *mins = KEXACPI_UNKNOWN;
	*charging = *discharging = *critical = 0;
	for (i = 0; i < (int) st.nbat; i++)
	{
		KEXACPI_BATTERY *b = &st.bat[i];
		ULONG f = b->full_cap != KEXACPI_UNKNOWN && b->full_cap ? b->full_cap : b->design_cap;
		if (!b->present)
			continue;
		if (b->state & KEXACPI_BAT_CHARGING)
			*charging = 1;
		if (b->state & KEXACPI_BAT_DISCHARGING)
			*discharging = 1;
		if (b->state & KEXACPI_BAT_CRITICAL)
			*critical = 1;
		if (b->remaining == KEXACPI_UNKNOWN || f == KEXACPI_UNKNOWN || !f)
			continue;
		rem += b->remaining;
		full += f;
		if (b->rate != KEXACPI_UNKNOWN && (b->state & KEXACPI_BAT_DISCHARGING))
			rate += b->rate;
		n++;
	}
	if (n && full)
	{
		*pc = (ULONG) ((rem * 100 + full / 2) / full);
		if (*pc > 100)
			*pc = 100;
		if (*discharging && rate)
			*mins = (ULONG) (rem * 60 / rate);
	}
}

static int any_battery(void)
{
	int i;
	for (i = 0; i < (int) st.nbat; i++)
		if (st.bat[i].present)
			return 1;
	return 0;
}

static void minutes_text(char *out, size_t size, ULONG m)
{
	if (m >= 60)
		snprintf(out, size, "%lu h %lu min", m / 60, m % 60);
	else
		snprintf(out, size, "%lu min", m);
}

static void summary(char *out, size_t size)
{
	ULONG pc, mins;
	int ch, dis, crit;
	char t[32];
	if (!have_status)
	{
		snprintf(out, size, "Battery: KEXACPI.VXD did not start (error %lu)", dev_error);
		return;
	}
	if (st.state != KEXACPI_ST_RUNNING)
	{
		snprintf(out, size, "Battery: not available");
		return;
	}
	if (!any_battery())
	{
		snprintf(out, size, st.ac == 1 ? "On AC power, no battery in" : "No battery in");
		return;
	}
	total(&pc, &mins, &ch, &dis, &crit);
	if (pc == KEXACPI_UNKNOWN)
	{
		snprintf(out, size, "Battery: charge not known%s", st.ac == 1 ? ", on AC power" : "");
		return;
	}
	if (ch)
		snprintf(out, size, "Battery: %lu%%, charging", pc);
	else if (dis && mins != KEXACPI_UNKNOWN)
	{
		minutes_text(t, sizeof(t), mins);
		snprintf(out, size, "Battery: %lu%% (%s left)", pc, t);
	}
	else if (st.ac == 1)
		snprintf(out, size, "Battery: %lu%%, on AC power", pc);
	else
		snprintf(out, size, "Battery: %lu%%", pc);
}

/* ---- the taskbar icon ------------------------------------------------------------------- */
/* a battery lying down, filled to the charge: green, yellow under 40 %, red
 * under 15 %; a plug when on AC power, a bolt when charging */
static HICON make_icon(int size, ULONG pc, int ac, int charging)
{
	HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen), mdc = CreateCompatibleDC(screen);
	HBITMAP color = CreateCompatibleBitmap(screen, size, size), mask = CreateBitmap(size, size, 1, 1, NULL);
	HGDIOBJ oc = SelectObject(dc, color), om = SelectObject(mdc, mask);
	HBRUSH black = (HBRUSH) GetStockObject(BLACK_BRUSH), white = (HBRUSH) GetStockObject(WHITE_BRUSH);
	ICONINFO ii;
	HICON icon;
	int s = size / 16;
	RECT all = { 0, 0, size, size };
	RECT body = { 0, 4 * s, 13 * s, 12 * s }, tip = { 13 * s, 6 * s, 15 * s, 10 * s };
	RECT in = { s, 5 * s, 12 * s, 11 * s };
	if (s < 1)
		s = 1;
	FillRect(dc, &all, black);
	FillRect(mdc, &all, white);   /* white = transparent */
	/* outline */
	FillRect(dc, &body, black);
	FillRect(mdc, &body, black);
	FillRect(dc, &tip, black);
	FillRect(mdc, &tip, black);
	FillRect(dc, &in, white);
	if (pc != KEXACPI_UNKNOWN)
	{
		RECT fill = in;
		HBRUSH br = CreateSolidBrush(pc < 15 ? RGB(220, 0, 0) : pc < 40 ? RGB(230, 180, 0) : RGB(0, 160, 0));
		fill.right = in.left + (int) ((in.right - in.left) * (pc > 100 ? 100 : pc) / 100);
		if (pc && fill.right == in.left)
			fill.right++;
		FillRect(dc, &fill, br);
		DeleteObject(br);
	}
	if (charging || ac)
	{
		/* lower left: a bolt (charging) or a plug (on AC power), in dark blue */
		HBRUSH br = CreateSolidBrush(RGB(0, 0, 160));
		POINT bolt[6] = { { 5 * s, 12 * s }, { 9 * s, 12 * s }, { 7 * s, 14 * s }, { 9 * s, 14 * s },
		                  { 4 * s, 16 * s }, { 6 * s, 14 * s } };
		RECT plug = { 1 * s, 13 * s, 6 * s, 16 * s }, pin1 = { 2 * s, 12 * s, 3 * s, 13 * s },
		     pin2 = { 4 * s, 12 * s, 5 * s, 13 * s };
		if (charging)
		{
			HPEN np = (HPEN) GetStockObject(NULL_PEN);
			HGDIOBJ op = SelectObject(dc, np), ob = SelectObject(dc, br);
			HGDIOBJ omp = SelectObject(mdc, np), omb = SelectObject(mdc, black);
			Polygon(dc, bolt, 6);
			Polygon(mdc, bolt, 6);
			SelectObject(dc, op);
			SelectObject(dc, ob);
			SelectObject(mdc, omp);
			SelectObject(mdc, omb);
		}
		else
		{
			FillRect(dc, &plug, br);
			FillRect(dc, &pin1, br);
			FillRect(dc, &pin2, br);
			FillRect(mdc, &plug, black);
			FillRect(mdc, &pin1, black);
			FillRect(mdc, &pin2, black);
		}
		DeleteObject(br);
	}
	SelectObject(dc, oc);
	SelectObject(mdc, om);
	ii.fIcon = TRUE;
	ii.xHotspot = ii.yHotspot = 0;
	ii.hbmMask = mask;
	ii.hbmColor = color;
	icon = CreateIconIndirect(&ii);
	DeleteObject(color);
	DeleteObject(mask);
	DeleteDC(dc);
	DeleteDC(mdc);
	ReleaseDC(NULL, screen);
	return icon;
}

static HICON tray_icon;

static void tray_update(void)
{
	NOTIFYICONDATA nid;
	ULONG pc = KEXACPI_UNKNOWN, mins;
	int ch = 0, dis, crit;
	int want = opt.tray && have_status && st.state == KEXACPI_ST_RUNNING && any_battery();
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = NOTIFYICONDATA_V1_SIZE;
	nid.hWnd = tray_wnd;
	nid.uID = 1;
	if (!want)
	{
		if (tray_shown)
			Shell_NotifyIcon(NIM_DELETE, &nid);
		tray_shown = 0;
		return;
	}
	total(&pc, &mins, &ch, &dis, &crit);
	if (tray_icon)
		DestroyIcon(tray_icon);
	tray_icon = make_icon(16, pc, st.ac == 1, ch);
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = WM_TRAY;
	nid.hIcon = tray_icon;
	summary(nid.szTip, sizeof(nid.szTip));
	if (!Shell_NotifyIcon(tray_shown ? NIM_MODIFY : NIM_ADD, &nid))
		tray_shown = Shell_NotifyIcon(NIM_ADD, &nid) != 0;
	else
		tray_shown = 1;
}

/* ---- warnings, the lid -------------------------------------------------------------------- */
static int crit_left;

static INT_PTR CALLBACK crit_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
	char t[160];
	(void) lp;
	switch (msg)
	{
	case WM_INITDIALOG:
		crit_left = 60;
		SetTimer(d, 1, 1000, NULL);
		/* fall through */
	case WM_TIMER:
		if (msg == WM_TIMER && --crit_left <= 0)
		{
			KillTimer(d, 1);
			EndDialog(d, IDOK);
			return TRUE;
		}
		snprintf(t, sizeof(t),
		         "The battery is nearly empty. Windows shuts down in %d seconds. Connect the charger and choose "
		         "Cancel to keep working.",
		         crit_left);
		SetDlgItemText(d, IDC_COUNT, t);
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
		{
			KillTimer(d, 1);
			EndDialog(d, LOWORD(wp));
		}
		return TRUE;
	}
	return FALSE;
}

static void shut_down(void)
{
	ExitWindowsEx(EWX_SHUTDOWN, 0);
}

static void check_alarms(void)
{
	ULONG pc, mins;
	int ch, dis, crit;
	if (!have_status || st.state != KEXACPI_ST_RUNNING)
		return;
	/* the lid: act when it closes, undo "screen off" when it opens */
	if (st.lid != -1)
	{
		if (last_lid == 1 && st.lid == 0)
		{
			if (opt.lid == 1)
			{
				DefWindowProc(tray_wnd, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
				screen_off = 1;
			}
			else if (opt.lid == 2)
				shut_down();
		}
		else if (last_lid == 0 && st.lid == 1 && screen_off)
		{
			DefWindowProc(tray_wnd, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM) -1);
			mouse_event(MOUSEEVENTF_MOVE, 0, 1, 0, 0);
			screen_off = 0;
		}
		last_lid = st.lid;
	}
	if (!any_battery())
		return;
	total(&pc, &mins, &ch, &dis, &crit);
	if (pc == KEXACPI_UNKNOWN || !dis || st.ac == 1)
	{
		/* charging or on mains: warn again next time it runs down */
		warned = crit_started = 0;
		return;
	}
	if (opt.crit && !crit_started && (pc <= (ULONG) opt.crit_pc || crit))
	{
		crit_started = warned = 1;
		if (DialogBox(inst, MAKEINTRESOURCE(IDD_CRIT), NULL, crit_proc) == IDOK)
			shut_down();
		return;
	}
	if (opt.warn && !warned && pc <= (ULONG) opt.warn_pc)
	{
		char t[200], m[32];
		warned = 1;
		if (mins != KEXACPI_UNKNOWN)
		{
			minutes_text(m, sizeof(m), mins);
			snprintf(t, sizeof(t), "The battery is down to %lu%% (about %s left). Connect the charger soon.", pc, m);
		}
		else
			snprintf(t, sizeof(t), "The battery is down to %lu%%. Connect the charger soon.", pc);
		MessageBox(NULL, t, "Battery low", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
	}
}

/* ---- the window -------------------------------------------------------------------------- */
static void num(char *out, size_t size, ULONG v)
{
	/* thousands separated: 45,000 */
	char t[16];
	int n, i, j = 0;
	snprintf(t, sizeof(t), "%lu", v);
	n = (int) strlen(t);
	for (i = 0; i < n && j + 2 < (int) size; i++)
	{
		out[j++] = t[i];
		if ((n - i - 1) % 3 == 0 && i != n - 1)
			out[j++] = ',';
	}
	out[j] = 0;
}

static void details(char *out, size_t size, KEXACPI_BATTERY *b)
{
	const char *wh = b->units ? "mAh" : "mWh", *w = b->units ? "mA" : "mW";
	char a[24], c[24], d[24], t[32], line[160];
	size_t len = 0;
	ULONG full = b->full_cap != KEXACPI_UNKNOWN && b->full_cap ? b->full_cap : b->design_cap;
	out[0] = 0;
#define ADD(...) (snprintf(line, sizeof(line), __VA_ARGS__), len += (size_t) snprintf(out + len, size - len, "%s\n", line))
	if (!b->present)
	{
		ADD("No battery in this bay.");
		return;
	}
	if (b->percent == KEXACPI_UNKNOWN)
		ADD("Charge: not known");
	else if (b->state & KEXACPI_BAT_CHARGING)
	{
		if (b->minutes_full != KEXACPI_UNKNOWN)
		{
			minutes_text(t, sizeof(t), b->minutes_full);
			ADD("Charge: %lu%%, charging (full in %s)", b->percent, t);
		}
		else
			ADD("Charge: %lu%%, charging", b->percent);
	}
	else if (b->state & KEXACPI_BAT_DISCHARGING)
	{
		if (b->minutes_left != KEXACPI_UNKNOWN)
		{
			minutes_text(t, sizeof(t), b->minutes_left);
			ADD("Charge: %lu%%, in use (%s left)", b->percent, t);
		}
		else
			ADD("Charge: %lu%%, in use", b->percent);
	}
	else
		ADD("Charge: %lu%%%s", b->percent, st.ac == 1 ? ", not charging (on AC power)" : "");
	if (b->state & KEXACPI_BAT_CRITICAL)
		ADD("The battery reports itself critically low.");
	if (b->remaining != KEXACPI_UNKNOWN && full != KEXACPI_UNKNOWN)
	{
		num(a, sizeof(a), b->remaining);
		num(c, sizeof(c), full);
		if (b->design_cap != KEXACPI_UNKNOWN && b->design_cap && b->full_cap != KEXACPI_UNKNOWN)
		{
			num(d, sizeof(d), b->design_cap);
			ADD("Capacity: %s of %s %s (new: %s %s, so %lu%% of new)", a, c, wh, d, wh,
			    (ULONG) (((unsigned long long) b->full_cap * 100 + b->design_cap / 2) / b->design_cap));
		}
		else
			ADD("Capacity: %s of %s %s", a, c, wh);
	}
	if (b->rate != KEXACPI_UNKNOWN || b->voltage != KEXACPI_UNKNOWN)
	{
		char r[48] = "", v[32] = "";
		if (b->rate != KEXACPI_UNKNOWN)
		{
			num(a, sizeof(a), b->rate);
			snprintf(r, sizeof(r), "Rate: %s %s", a, w);
		}
		if (b->voltage != KEXACPI_UNKNOWN)
			snprintf(v, sizeof(v), "%sVoltage: %lu.%02lu V", r[0] ? "    " : "", b->voltage / 1000,
			         (b->voltage % 1000) / 10);
		ADD("%s%s", r, v);
	}
	if (b->cycles != KEXACPI_UNKNOWN && b->cycles)
		ADD("Charge cycles: %lu", b->cycles);
	if (b->model[0] || b->type[0] || b->oem[0] || b->serial[0])
		ADD("%s%s%s%s%s%s%s", b->model[0] ? b->model : "Battery", b->type[0] ? " (" : "", b->type,
		    b->type[0] ? ")" : "", b->oem[0] ? ", made by " : "", b->oem, "");
	if (b->serial[0])
		ADD("Serial number: %s", b->serial);
#undef ADD
}

static void main_refresh(void)
{
	char t[600];
	HWND combo;
	int i;
	if (!main_dlg)
		return;
	combo = GetDlgItem(main_dlg, IDC_BATTERY);
	if (!have_status)
	{
		snprintf(t, sizeof(t),
		         "The battery driver KEXACPI.VXD could not be loaded (error %lu). Reinstall KernelExEx with the "
		         "\"Laptop battery\" component.",
		         dev_error);
		SetDlgItemText(main_dlg, IDC_POWER, "");
		SetDlgItemText(main_dlg, IDC_DETAILS, t);
		return;
	}
	if (st.state != KEXACPI_ST_RUNNING)
	{
		const char *why = st.state == KEXACPI_ST_WINACPI
		                      ? "Windows' own power management (ACPI) runs this computer, so Windows shows the "
		                        "battery itself (Control Panel, Power Management)."
		                  : st.state == KEXACPI_ST_NOACPI ? "This computer has no ACPI tables, so there is no battery "
		                                                     "to read here."
		                                                   : "The ACPI tables of this computer could not be read.";
		SetDlgItemText(main_dlg, IDC_POWER, "");
		SetDlgItemText(main_dlg, IDC_DETAILS, why);
		return;
	}
	snprintf(t, sizeof(t), "%s%s", st.ac == 1 ? "Power: AC adapter (mains)" : st.ac == 0 ? "Power: battery" : "",
	         st.lid == -1 ? "" : st.lid ? "    Lid: open" : "    Lid: closed");
	SetDlgItemText(main_dlg, IDC_POWER, t);
	if ((int) SendMessage(combo, CB_GETCOUNT, 0, 0) != (int) st.nbat)
	{
		SendMessage(combo, CB_RESETCONTENT, 0, 0);
		for (i = 0; i < (int) st.nbat; i++)
		{
			snprintf(t, sizeof(t), "Battery %d%s%s", i + 1, st.bat[i].model[0] ? ": " : "", st.bat[i].model);
			SendMessage(combo, CB_ADDSTRING, 0, (LPARAM) t);
		}
		if (cur_bat >= (int) st.nbat)
			cur_bat = 0;
		SendMessage(combo, CB_SETCURSEL, cur_bat, 0);
	}
	EnableWindow(combo, st.nbat > 1);
	if (!st.nbat)
	{
		SetDlgItemText(main_dlg, IDC_DETAILS, "This computer has no battery (none in its ACPI tables).");
		SendDlgItemMessage(main_dlg, IDC_CHARGE, PBM_SETPOS, 0, 0);
		return;
	}
	details(t, sizeof(t), &st.bat[cur_bat]);
	SetDlgItemText(main_dlg, IDC_DETAILS, t);
	SendDlgItemMessage(main_dlg, IDC_CHARGE, PBM_SETPOS,
	                   st.bat[cur_bat].percent == KEXACPI_UNKNOWN ? 0 : st.bat[cur_bat].percent, 0);
}

static void read_controls(void)
{
	BOOL ok;
	int v;
	opt.tray = IsDlgButtonChecked(main_dlg, IDC_TRAYICON) == BST_CHECKED;
	opt.warn = IsDlgButtonChecked(main_dlg, IDC_WARN) == BST_CHECKED;
	opt.crit = IsDlgButtonChecked(main_dlg, IDC_CRITACT) == BST_CHECKED;
	v = (int) GetDlgItemInt(main_dlg, IDC_WARNPC, &ok, FALSE);
	if (ok && v >= 1 && v <= 90)
		opt.warn_pc = v;
	v = (int) GetDlgItemInt(main_dlg, IDC_CRITPC, &ok, FALSE);
	if (ok && v >= 1 && v <= 50)
		opt.crit_pc = v;
	opt.lid = (int) SendDlgItemMessage(main_dlg, IDC_LID, CB_GETCURSEL, 0, 0);
	if (opt.lid < 0)
		opt.lid = 0;
	save_options();
	tray_update();
}

static INT_PTR CALLBACK main_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
	(void) lp;
	switch (msg)
	{
	case WM_INITDIALOG:
		main_dlg = d;
		SendMessage(d, WM_SETICON, ICON_BIG, (LPARAM) LoadIcon(inst, MAKEINTRESOURCE(IDI_POWER)));
		CheckDlgButton(d, IDC_TRAYICON, opt.tray ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_WARN, opt.warn ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_CRITACT, opt.crit ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemInt(d, IDC_WARNPC, (UINT) opt.warn_pc, FALSE);
		SetDlgItemInt(d, IDC_CRITPC, (UINT) opt.crit_pc, FALSE);
		SendDlgItemMessage(d, IDC_WARNPC, EM_LIMITTEXT, 2, 0);
		SendDlgItemMessage(d, IDC_CRITPC, EM_LIMITTEXT, 2, 0);
		SendDlgItemMessage(d, IDC_LID, CB_ADDSTRING, 0, (LPARAM) "Do nothing");
		SendDlgItemMessage(d, IDC_LID, CB_ADDSTRING, 0, (LPARAM) "Turn the screen off");
		SendDlgItemMessage(d, IDC_LID, CB_ADDSTRING, 0, (LPARAM) "Shut down");
		SendDlgItemMessage(d, IDC_LID, CB_SETCURSEL, (WPARAM) opt.lid, 0);
		SendDlgItemMessage(d, IDC_CHARGE, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
		SetDlgItemText(d, IDC_NOTE, "KernelExEx reads the battery from the computer's ACPI firmware.");
		main_refresh();
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDCANCEL:
			read_controls();
			DestroyWindow(d);
			main_dlg = NULL;
			if (!tray_shown)
				DestroyWindow(tray_wnd);
			return TRUE;
		case IDC_BATTERY:
			if (HIWORD(wp) == CBN_SELCHANGE)
			{
				cur_bat = (int) SendDlgItemMessage(d, IDC_BATTERY, CB_GETCURSEL, 0, 0);
				main_refresh();
			}
			return TRUE;
		case IDC_TRAYICON:
		case IDC_WARN:
		case IDC_CRITACT:
			read_controls();
			return TRUE;
		case IDC_LID:
			if (HIWORD(wp) == CBN_SELCHANGE)
				read_controls();
			return TRUE;
		case IDC_WARNPC:
		case IDC_CRITPC:
			if (HIWORD(wp) == EN_KILLFOCUS)
				read_controls();
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void show_main(void)
{
	if (main_dlg)
	{
		ShowWindow(main_dlg, SW_RESTORE);
		SetForegroundWindow(main_dlg);
		return;
	}
	CreateDialog(inst, MAKEINTRESOURCE(IDD_MAIN), NULL, main_proc);
	ShowWindow(main_dlg, SW_SHOW);
}

/* ---- the hidden tray window --------------------------------------------------------------- */
static void tick(void)
{
	poll();
	tray_update();
	main_refresh();
	check_alarms();
}

static LRESULT CALLBACK tray_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == msg_taskbar_created && msg)
	{
		tray_shown = 0;
		tray_update();
		return 0;
	}
	switch (msg)
	{
	case WM_TIMER:
		tick();
		return 0;
	case WM_SHOWMAIN:
		show_main();
		return 0;
	case WM_TRAY:
		if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP)
			show_main();
		else if (lp == WM_RBUTTONUP)
		{
			HMENU m = CreatePopupMenu();
			POINT pt;
			int cmd;
			AppendMenu(m, MF_STRING, MENU_OPEN, "&Battery details...");
			AppendMenu(m, MF_SEPARATOR, 0, NULL);
			AppendMenu(m, MF_STRING, MENU_EXIT, "&Hide the battery icon");
			SetMenuDefaultItem(m, MENU_OPEN, FALSE);
			GetCursorPos(&pt);
			SetForegroundWindow(w);
			cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, w, NULL);
			PostMessage(w, WM_NULL, 0, 0);
			DestroyMenu(m);
			if (cmd == MENU_OPEN)
				show_main();
			else if (cmd == MENU_EXIT)
			{
				opt.tray = 0;
				save_options();
				tray_update();
				if (main_dlg)
					CheckDlgButton(main_dlg, IDC_TRAYICON, BST_UNCHECKED);
				else
					DestroyWindow(w);
			}
		}
		return 0;
	case WM_DESTROY:
	{
		NOTIFYICONDATA nid;
		memset(&nid, 0, sizeof(nid));
		nid.cbSize = NOTIFYICONDATA_V1_SIZE;
		nid.hWnd = w;
		nid.uID = 1;
		if (tray_shown)
			Shell_NotifyIcon(NIM_DELETE, &nid);
		tray_shown = 0;
		PostQuitMessage(0);
		return 0;
	}
	}
	return DefWindowProc(w, msg, wp, lp);
}

void entrypoint(void)
{
	WNDCLASS wc;
	MSG m;
	HWND other;
	int tray_only;
	const char *cmd = GetCommandLine();
	INITCOMMONCONTROLSEX icc;
	inst = GetModuleHandle(NULL);
	tray_only = strstr(cmd, "/tray") != NULL || strstr(cmd, "/TRAY") != NULL;

	other = FindWindow(TRAY_CLASS, NULL);
	if (other)
	{
		if (!tray_only)
		{
			int i;
			PostMessage(other, WM_SHOWMAIN, 0, 0);
			/* Windows 98 lets only the foreground program hand over the focus */
			for (i = 0; i < 20; i++)
			{
				HWND dlg = FindWindow("#32770", "Battery");
				if (dlg && IsWindowVisible(dlg))
				{
					SetForegroundWindow(dlg);
					break;
				}
				Sleep(50);
			}
		}
		ExitProcess(0);
	}
	load_options();
	poll();
	/* from the Run key: nothing to show on a desktop, or with the icon turned off */
	if (tray_only && (!opt.tray || !have_status || st.state != KEXACPI_ST_RUNNING || (!st.nbat && st.lid == -1)))
		ExitProcess(0);

	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_PROGRESS_CLASS;
	InitCommonControlsEx(&icc);
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = tray_proc;
	wc.hInstance = inst;
	wc.lpszClassName = TRAY_CLASS;
	RegisterClass(&wc);
	tray_wnd = CreateWindow(TRAY_CLASS, "KernelExEx Battery", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
	msg_taskbar_created = RegisterWindowMessage("TaskbarCreated");
	SetTimer(tray_wnd, 1, POLL_MS, NULL);
	if (have_status)
		last_lid = st.lid;
	tray_update();
	if (!tray_only)
		show_main();
	while (GetMessage(&m, NULL, 0, 0) > 0)
	{
		if (main_dlg && IsDialogMessage(main_dlg, &m))
			continue;
		TranslateMessage(&m);
		DispatchMessage(&m);
	}
	if (dev != INVALID_HANDLE_VALUE)
		CloseHandle(dev);
	ExitProcess(0);
}
