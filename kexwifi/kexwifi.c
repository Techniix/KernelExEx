/*
 *  KernelExEx - kexwifi.exe: Wi-Fi for Windows 98 SE / ME. A taskbar icon
 *  with the connection's state and signal, and a window that lists the
 *  networks in range, connects (with a password for WPA2), remembers
 *  networks and forgets them. It works with the KernelExEx Wi-Fi drivers
 *  (kexrtwn.sys, kexurtwn.sys) through the registry; see
 *  drivers/kexnet/wifi/kexwifi_reg.h for the exact layout.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  kexwifi.exe /tray   start with the taskbar icon only (from the Run key)
 *  kexwifi.exe         open the window (or bring the running one forward)
 *
 *  Security: only WPA2 (and, if the user allows it per network, WPA/TKIP)
 *  and open networks are joined; WEP is shown but refused. The password is
 *  turned into the WPA2 key here (PBKDF2, as the standard says) and only
 *  the key is stored, so the password itself is never written anywhere.
 */
#define _WIN32_WINNT 0x0400
#define _WIN32_IE 0x0400
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>
#include "resource.h"
#include "sha1.h"
#include "../drivers/kexnet/wifi/kexwifi_reg.h"

int snprintf(char *, size_t, const char *, ...);

/* wiping secrets: a volatile loop the compiler keeps (Windows 98 has no
 * RtlSecureZeroMemory) */
static void wipe(void *p, size_t n)
{
	volatile unsigned char *v = (volatile unsigned char *) p;
	while (n--)
		*v++ = 0;
}
#undef SecureZeroMemory
#define SecureZeroMemory(p, n) wipe((p), (n))

#define WM_TRAY (WM_APP + 1)
#define WM_SHOWMAIN (WM_APP + 2)
#define TRAY_CLASS "KexWifiTray"
#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define MENU_NET 100   /* tray menu: network n = MENU_NET + n */
#define MENU_OPEN 1
#define MENU_OFF 2
#define MENU_ON 3
#define MENU_EXIT 4
#define MAX_ADAPTERS 4
#define MAX_ROWS (KEXWIFI_MAX_AP + 32)

static HINSTANCE inst;
static HWND tray_wnd, main_dlg;
static UINT msg_taskbar_created;
static int tray_shown, want_tray = 1;
static HICON icons[8];   /* 0 off, 1 searching, 2 connecting, 3..7 connected with 0-4 bars */
static HIMAGELIST bars_list;

struct adapter
{
	char key[64];      /* the name under Adapters */
	char name[96];
	DWORD present, state, channel, signal, fixed, scan_seq;
	unsigned char ssid[32];
	int ssid_len;
	unsigned char mac[6];
	int has_mac;
};
static struct adapter adapters[MAX_ADAPTERS];
/* per adapter (by key): the state last seen, to notice "now connected" */
static struct
{
	char key[64];
	DWORD state;
	int renew_in;   /* ticks until the DHCP renew, 0 = none */
} seen[MAX_ADAPTERS];
static int nadapters, cur_adapter;

struct row
{
	unsigned char ssid[32];
	int ssid_len;
	int signal;      /* -1: not in range (a saved network) */
	int security;
	int saved;
	int channel;
};
static struct row rows[MAX_ROWS];
static int nrows;
static DWORD shown_scan_seq = 0xffffffff;
static int ticks;
static char pending_ssid[33];   /* "Connecting to ..." until the driver says so */
static int pending_len, pending_ticks;

/* ---- registry ---------------------------------------------------------------------- */
static HKEY open_key(const char *sub, int create)
{
	char path[200];
	HKEY h;
	snprintf(path, sizeof(path), KEXWIFI_KEY_A "%s%s", sub && *sub ? "\\" : "", sub ? sub : "");
	if (create)
	{
		DWORD disp;
		if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, &disp) != ERROR_SUCCESS)
			return NULL;
	}
	else if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &h) != ERROR_SUCCESS)
		return NULL;
	return h;
}

static DWORD get_dword(HKEY h, const char *name, DWORD dflt)
{
	DWORD v, t, n = sizeof(v);
	if (RegQueryValueEx(h, name, NULL, &t, (BYTE *) &v, &n) != ERROR_SUCCESS || n != sizeof(v))
		return dflt;
	return v;
}

static int get_bin(HKEY h, const char *name, void *out, DWORD cap)
{
	DWORD t, n = cap;
	if (RegQueryValueEx(h, name, NULL, &t, (BYTE *) out, &n) != ERROR_SUCCESS)
		return -1;
	return (int) n;
}

static void set_dword(HKEY h, const char *name, DWORD v)
{
	RegSetValueEx(h, name, 0, REG_DWORD, (const BYTE *) &v, sizeof(v));
}

static void hex_name(const unsigned char *ssid, int len, char *out)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	for (i = 0; i < len; i++)
	{
		out[2 * i] = hx[ssid[i] >> 4];
		out[2 * i + 1] = hx[ssid[i] & 15];
	}
	out[2 * len] = 0;
}

static int is_saved(const unsigned char *ssid, int len)
{
	char sub[80], hx[65];
	HKEY h;
	hex_name(ssid, len, hx);
	snprintf(sub, sizeof(sub), "Networks\\%s", hx);
	h = open_key(sub, 0);
	if (!h)
		return 0;
	RegCloseKey(h);
	return 1;
}

/* the network name for display: UTF-8 if it is valid UTF-8, else as is */
static void ssid_text(const unsigned char *ssid, int len, char *out, int cap)
{
	WCHAR w[40];
	int n;
	if (len == 0)
	{
		lstrcpyn(out, "(hidden network)", cap);
		return;
	}
	n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *) ssid, len, w, 40);
	if (n > 0)
	{
		n = WideCharToMultiByte(CP_ACP, 0, w, n, out, cap - 1, "?", NULL);
		if (n > 0)
		{
			out[n] = 0;
			return;
		}
	}
	if (len > cap - 1)
		len = cap - 1;
	memcpy(out, ssid, (size_t) len);
	out[len] = 0;
}

/* ---- adapters ----------------------------------------------------------------------- */
static void read_adapters(void)
{
	HKEY base = open_key("Adapters", 0);
	DWORD i;
	char keep[64];
	lstrcpyn(keep, nadapters ? adapters[cur_adapter].key : "", sizeof(keep));
	nadapters = 0;
	if (!base)
		return;
	for (i = 0; nadapters < MAX_ADAPTERS; i++)
	{
		char name[64];
		DWORD n = sizeof(name);
		HKEY h;
		struct adapter *a = &adapters[nadapters];
		if (RegEnumKeyEx(base, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
			break;
		if (RegOpenKeyEx(base, name, 0, KEY_READ, &h) != ERROR_SUCCESS)
			continue;
		memset(a, 0, sizeof(*a));
		lstrcpyn(a->key, name, sizeof(a->key));
		a->present = get_dword(h, "Present", 0);
		if (a->present)
		{
			DWORD t, nn = sizeof(a->name);
			if (RegQueryValueEx(h, "Name", NULL, &t, (BYTE *) a->name, &nn) != ERROR_SUCCESS)
				lstrcpyn(a->name, name, sizeof(a->name));
			a->state = get_dword(h, "State", 0);
			a->channel = get_dword(h, "Channel", 0);
			a->signal = get_dword(h, "Signal", 0);
			a->fixed = get_dword(h, "Fixed", 0);
			a->scan_seq = get_dword(h, "ScanSeq", 0);
			a->ssid_len = get_bin(h, "SSID", a->ssid, sizeof(a->ssid));
			if (a->ssid_len < 0)
				a->ssid_len = 0;
			a->has_mac = get_bin(h, "MAC", a->mac, sizeof(a->mac)) == 6;
			nadapters++;
		}
		RegCloseKey(h);
	}
	RegCloseKey(base);
	cur_adapter = 0;
	for (i = 0; i < (DWORD) nadapters; i++)
		if (!lstrcmp(adapters[i].key, keep))
			cur_adapter = (int) i;
}

static HKEY adapter_key(void)
{
	char sub[96];
	if (!nadapters)
		return NULL;
	snprintf(sub, sizeof(sub), "Adapters\\%s", adapters[cur_adapter].key);
	return open_key(sub, 0);
}

/* a request to the driver; it acts within 2 seconds and clears it */
static int request(DWORD req, const unsigned char *target, int tlen)
{
	HKEY h = adapter_key();
	if (!h)
		return 0;
	if (target)
		RegSetValueEx(h, "Target", 0, REG_BINARY, target, (DWORD) tlen);
	set_dword(h, "Request", req);
	RegCloseKey(h);
	return 1;
}

static DWORD pending_request(void)
{
	HKEY h = adapter_key();
	DWORD r;
	if (!h)
		return 0;
	r = get_dword(h, "Request", 0);
	RegCloseKey(h);
	return r;
}

/* ---- icons: drawn, so they match the state exactly ------------------------------------ */
static HICON make_icon(int kind, int size)
{
	/* kind: 0 off, 1 searching, 2 connecting, 3..7 connected 0-4 bars */
	HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen), mdc = CreateCompatibleDC(screen);
	HBITMAP color = CreateCompatibleBitmap(screen, size, size), mask = CreateBitmap(size, size, 1, 1, NULL);
	HGDIOBJ oc = SelectObject(dc, color), om = SelectObject(mdc, mask);
	ICONINFO ii;
	HICON icon;
	int i, bars = kind >= 3 ? kind - 3 : 0;
	int w = size / 5, gap = (size - 4 * w) / 5;
	RECT r = { 0, 0, size, size };
	FillRect(dc, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));
	FillRect(mdc, &r, (HBRUSH) GetStockObject(WHITE_BRUSH));   /* white = transparent */
	for (i = 0; i < 4; i++)
	{
		int h = (size - 2) * (i + 1) / 4, x = gap + i * (w + gap);
		RECT b = { x, size - 1 - h, x + w, size - 1 };
		COLORREF c;
		HBRUSH br;
		if (kind == 0)
			c = RGB(128, 128, 128);
		else if (kind == 1)
			c = i == ((GetTickCount() / 600) & 3) ? RGB(0, 0, 160) : RGB(160, 160, 160);
		else if (kind == 2)
			c = RGB(200, 140, 0);
		else
			c = i < bars ? RGB(0, 96, 192) : RGB(176, 176, 176);
		br = CreateSolidBrush(c);
		FillRect(dc, &b, br);
		DeleteObject(br);
		FillRect(mdc, &b, (HBRUSH) GetStockObject(BLACK_BRUSH));
	}
	if (kind == 0)
	{
		/* a red cross: off */
		HPEN pen = CreatePen(PS_SOLID, size >= 32 ? 3 : 2, RGB(200, 0, 0)), op = SelectObject(dc, pen);
		HPEN mpen = CreatePen(PS_SOLID, size >= 32 ? 3 : 2, RGB(0, 0, 0)), omp = SelectObject(mdc, mpen);
		MoveToEx(dc, 1, 1, NULL);
		LineTo(dc, size / 2, size / 2);
		MoveToEx(dc, size / 2, 1, NULL);
		LineTo(dc, 1, size / 2);
		MoveToEx(mdc, 1, 1, NULL);
		LineTo(mdc, size / 2, size / 2);
		MoveToEx(mdc, size / 2, 1, NULL);
		LineTo(mdc, 1, size / 2);
		SelectObject(dc, op);
		SelectObject(mdc, omp);
		DeleteObject(pen);
		DeleteObject(mpen);
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

static int icon_kind(const struct adapter *a)
{
	if (!a || a->state == KEXWIFI_OFF)
		return 0;
	if (a->state == KEXWIFI_SEARCHING)
		return 1;
	if (a->state == KEXWIFI_CONNECTING)
		return 2;
	return 3 + (a->signal >= 80 ? 4 : a->signal >= 55 ? 3 : a->signal >= 30 ? 2 : a->signal > 0 ? 1 : 0);
}

static int bars_of(int signal)
{
	return signal < 0 ? 0 : signal >= 80 ? 4 : signal >= 55 ? 3 : signal >= 30 ? 2 : 1;
}

/* ---- taskbar icon ---------------------------------------------------------------------- */
static void status_text(char *out, int cap)
{
	const struct adapter *a = nadapters ? &adapters[cur_adapter] : NULL;
	char ss[64];
	if (!a)
	{
		lstrcpyn(out, "No Wi-Fi adapter. Plug in a supported USB Wi-Fi adapter.", cap);
		return;
	}
	ssid_text(a->ssid, a->ssid_len, ss, sizeof(ss));
	switch (a->state)
	{
	case KEXWIFI_OFF:
		lstrcpyn(out, "Wi-Fi is off (disconnected).", cap);
		break;
	case KEXWIFI_SEARCHING:
		if (pending_len)
			snprintf(out, (size_t) cap, "Looking for %s...", pending_ssid);
		else if (a->ssid_len)
			snprintf(out, (size_t) cap, "Looking for %s...", ss);
		else
			lstrcpyn(out, "Not connected. Choose a network to connect to.", cap);
		break;
	case KEXWIFI_CONNECTING:
		snprintf(out, (size_t) cap, "Connecting to %s...", ss);
		break;
	default:
		snprintf(out, (size_t) cap, "Connected to %s (signal %lu%%, channel %lu).", ss, a->signal, a->channel);
	}
}

static void tray_update(void)
{
	NOTIFYICONDATA nid;
	int want = want_tray && nadapters > 0;
	char tip[128];
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
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = WM_TRAY;
	{
		int k = icon_kind(&adapters[cur_adapter]);
		if (k == 1)
		{
			/* searching: the bar that is lit moves along */
			if (icons[1])
				DestroyIcon(icons[1]);
			icons[1] = make_icon(1, 16);
		}
		nid.hIcon = icons[k];
	}
	status_text(tip, sizeof(tip));
	lstrcpyn(nid.szTip, tip, sizeof(nid.szTip));
	if (!Shell_NotifyIcon(tray_shown ? NIM_MODIFY : NIM_ADD, &nid))
		tray_shown = Shell_NotifyIcon(NIM_ADD, &nid) != 0;
	else
		tray_shown = 1;
}

/* ---- the list of networks -------------------------------------------------------------- */
static const char *security_name(int s)
{
	switch (s)
	{
	case KEXWIFI_SEC_OPEN:
		return "Open (not encrypted)";
	case KEXWIFI_SEC_WEP:
		return "WEP (not supported)";
	case KEXWIFI_SEC_WPA2:
		return "WPA2";
	case KEXWIFI_SEC_WPA:
		return "WPA (older)";
	case KEXWIFI_SEC_WPA3:
		return "WPA3 (not supported yet)";
	case KEXWIFI_SEC_ENT:
		return "WPA2-Enterprise (not supported yet)";
	}
	return "?";
}

static int row_cmp(const struct row *a, const struct row *b)
{
	if (a->signal != b->signal)
		return b->signal - a->signal;
	return memcmp(a->ssid, b->ssid, (size_t) (a->ssid_len < b->ssid_len ? a->ssid_len : b->ssid_len));
}

/* Scan from the driver, plus saved networks that are not in range */
static void read_rows(void)
{
	static struct kexwifi_ap aps[KEXWIFI_MAX_AP];
	HKEY h = adapter_key(), nets;
	int n = 0, i, j;
	nrows = 0;
	if (h)
	{
		n = get_bin(h, "Scan", aps, sizeof(aps));
		n = n > 0 ? n / (int) sizeof(aps[0]) : 0;
		RegCloseKey(h);
	}
	for (i = 0; i < n && nrows < MAX_ROWS; i++)
	{
		struct row *r = &rows[nrows++];
		r->ssid_len = aps[i].ssid_len > 32 ? 32 : aps[i].ssid_len;
		memcpy(r->ssid, aps[i].ssid, (size_t) r->ssid_len);
		r->signal = aps[i].signal;
		r->security = aps[i].security;
		r->channel = aps[i].channel;
		r->saved = is_saved(r->ssid, r->ssid_len);
	}
	nets = open_key("Networks", 0);
	if (nets)
	{
		DWORD k;
		for (k = 0; nrows < MAX_ROWS; k++)
		{
			char name[80];
			DWORD nn = sizeof(name);
			HKEY nh;
			unsigned char ssid[32];
			int len, found = 0;
			if (RegEnumKeyEx(nets, k, name, &nn, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
				break;
			if (RegOpenKeyEx(nets, name, 0, KEY_READ, &nh) != ERROR_SUCCESS)
				continue;
			len = get_bin(nh, "SSID", ssid, sizeof(ssid));
			if (len > 0)
			{
				char key[80];
				DWORD t, kn = sizeof(key);
				for (j = 0; j < nrows; j++)
					if (rows[j].ssid_len == len && !memcmp(rows[j].ssid, ssid, (size_t) len))
						found = 1;
				if (!found)
				{
					struct row *r = &rows[nrows++];
					memset(r, 0, sizeof(*r));
					memcpy(r->ssid, ssid, (size_t) len);
					r->ssid_len = len;
					r->signal = -1;
					key[0] = 0;
					RegQueryValueEx(nh, "Key", NULL, &t, (BYTE *) key, &kn);
					r->security = key[0] ? KEXWIFI_SEC_WPA2 : KEXWIFI_SEC_OPEN;
					r->saved = 1;
				}
			}
			RegCloseKey(nh);
		}
		RegCloseKey(nets);
	}
	/* strongest first; insertion sort, the list is short */
	for (i = 1; i < nrows; i++)
		for (j = i; j > 0 && row_cmp(&rows[j - 1], &rows[j]) > 0; j--)
		{
			struct row t = rows[j];
			rows[j] = rows[j - 1];
			rows[j - 1] = t;
		}
}

static int selected_row(void)
{
	HWND lv = GetDlgItem(main_dlg, IDC_LIST);
	int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
	LVITEM it;
	if (i < 0)
		return -1;
	memset(&it, 0, sizeof(it));
	it.mask = LVIF_PARAM;
	it.iItem = i;
	ListView_GetItem(lv, &it);
	return (int) it.lParam;
}

static int is_current(const struct row *r)
{
	const struct adapter *a = nadapters ? &adapters[cur_adapter] : NULL;
	return a && a->state >= KEXWIFI_CONNECTING && a->ssid_len == r->ssid_len &&
	       !memcmp(a->ssid, r->ssid, (size_t) r->ssid_len);
}

static void update_buttons(void)
{
	int i = selected_row();
	const struct adapter *a = nadapters ? &adapters[cur_adapter] : NULL;
	const struct row *r = i >= 0 ? &rows[i] : NULL;
	EnableWindow(GetDlgItem(main_dlg, IDC_CONNECT), r && a && !a->fixed && r->signal >= 0 && !is_current(r));
	EnableWindow(GetDlgItem(main_dlg, IDC_FORGET), r && r->saved);
	EnableWindow(GetDlgItem(main_dlg, IDC_DISCONNECT), a != NULL);
	EnableWindow(GetDlgItem(main_dlg, IDC_REFRESH), a != NULL);
	EnableWindow(GetDlgItem(main_dlg, IDC_ADDNET), a != NULL && !a->fixed);
	SetDlgItemText(main_dlg, IDC_DISCONNECT, a && a->state == KEXWIFI_OFF ? "Turn &on" : "&Disconnect");
}

static void fill_list(void)
{
	HWND lv = GetDlgItem(main_dlg, IDC_LIST);
	unsigned char keep[32];
	int keep_len = -1, i, sel = -1;
	i = selected_row();
	if (i >= 0)
	{
		keep_len = rows[i].ssid_len;
		memcpy(keep, rows[i].ssid, (size_t) keep_len);
	}
	read_rows();
	SendMessage(lv, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(lv);
	for (i = 0; i < nrows; i++)
	{
		LVITEM it;
		char name[64], sig[32];
		int idx;
		const struct row *r = &rows[i];
		ssid_text(r->ssid, r->ssid_len, name, sizeof(name));
		if (is_current(r))
			lstrcat(name, "  (connected)");
		memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
		it.iItem = i;
		it.pszText = name;
		it.lParam = i;
		it.iImage = r->signal < 0 ? 0 : bars_of(r->signal);
		idx = ListView_InsertItem(lv, &it);
		if (r->signal < 0)
			lstrcpy(sig, "not in range");
		else
			snprintf(sig, sizeof(sig), "%d%%", r->signal);
		ListView_SetItemText(lv, idx, 1, sig);
		ListView_SetItemText(lv, idx, 2, (char *) security_name(r->security));
		ListView_SetItemText(lv, idx, 3, r->saved ? "Yes" : "");
		if (keep_len == r->ssid_len && !memcmp(keep, r->ssid, (size_t) keep_len))
			sel = idx;
	}
	if (sel >= 0)
		ListView_SetItemState(lv, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	SendMessage(lv, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(lv, NULL, TRUE);
	update_buttons();
}

static void update_main(void)
{
	char s[200];
	const struct adapter *a = nadapters ? &adapters[cur_adapter] : NULL;
	HWND cb = GetDlgItem(main_dlg, IDC_ADAPTER);
	int i;
	if ((int) SendMessage(cb, CB_GETCOUNT, 0, 0) != nadapters)
	{
		SendMessage(cb, CB_RESETCONTENT, 0, 0);
		for (i = 0; i < nadapters; i++)
			SendMessage(cb, CB_ADDSTRING, 0, (LPARAM) adapters[i].name);
		SendMessage(cb, CB_SETCURSEL, (WPARAM) cur_adapter, 0);
		shown_scan_seq = 0xffffffff;
	}
	status_text(s, sizeof(s));
	SetDlgItemText(main_dlg, IDC_STATUS, s);
	if (a && a->fixed)
		SetDlgItemText(main_dlg, IDC_NOTE, "This adapter joins the network named in its Advanced properties "
		                                   "(Network control panel). Clear the name there to choose networks here.");
	else if (!a)
		SetDlgItemText(main_dlg, IDC_NOTE, "Supported: Realtek USB Wi-Fi adapters such as the TP-Link Archer T2U "
		                                   "family, TL-WN722N v2/v3 and TL-WN725N.");
	else
		SetDlgItemText(main_dlg, IDC_NOTE, "");
	if (a && a->scan_seq != shown_scan_seq)
	{
		shown_scan_seq = a->scan_seq;
		fill_list();
	}
	else
		update_buttons();
}

/* ---- connecting -------------------------------------------------------------------- */
static void psk_hex(const char *pass, const unsigned char *ssid, int len, char out[65])
{
	unsigned char psk[32];
	static const char hx[] = "0123456789abcdef";
	int i;
	if (strlen(pass) == 64)
	{
		lstrcpyn(out, pass, 65);   /* already a key */
		return;
	}
	pbkdf2_sha1(pass, strlen(pass), ssid, (size_t) len, 4096, psk, 32);
	for (i = 0; i < 32; i++)
	{
		out[2 * i] = hx[psk[i] >> 4];
		out[2 * i + 1] = hx[psk[i] & 15];
	}
	out[64] = 0;
	SecureZeroMemory(psk, sizeof(psk));
}

static int save_network(const unsigned char *ssid, int len, const char *pass, int tkip, int autoj)
{
	char sub[80], hx[65], key[65];
	HKEY h;
	hex_name(ssid, len, hx);
	snprintf(sub, sizeof(sub), "Networks\\%s", hx);
	h = open_key(sub, 1);
	if (!h)
		return 0;
	key[0] = 0;
	if (pass && *pass)
		psk_hex(pass, ssid, len, key);
	RegSetValueEx(h, "SSID", 0, REG_BINARY, ssid, (DWORD) len);
	RegSetValueEx(h, "Key", 0, REG_SZ, (const BYTE *) key, (DWORD) strlen(key) + 1);
	set_dword(h, "TKIP", (DWORD) (tkip != 0));
	set_dword(h, "Auto", (DWORD) (autoj != 0));
	RegCloseKey(h);
	SecureZeroMemory(key, sizeof(key));
	return 1;
}

static int valid_pass(const char *p)
{
	size_t n = strlen(p), i;
	if (n == 64)
	{
		for (i = 0; i < 64; i++)
			if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') || (p[i] >= 'A' && p[i] <= 'F')))
				return 0;
		return 1;
	}
	return n >= 8 && n <= 63;
}

struct pass_ctx
{
	const struct row *r;
	char pass[80];
	int autoj, tkip;
	/* "Other network" */
	unsigned char ssid[32];
	int ssid_len, open;
};

static void show_chars(HWND dlg)
{
	HWND e = GetDlgItem(dlg, IDC_PASS);
	SendMessage(e, EM_SETPASSWORDCHAR, IsDlgButtonChecked(dlg, IDC_SHOW) ? 0 : '*', 0);
	InvalidateRect(e, NULL, TRUE);
}

static BOOL CALLBACK pass_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	struct pass_ctx *c = (struct pass_ctx *) GetWindowLong(dlg, DWL_USER);
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		char name[64], s[160];
		c = (struct pass_ctx *) lp;
		SetWindowLong(dlg, DWL_USER, (LONG) c);
		ssid_text(c->r->ssid, c->r->ssid_len, name, sizeof(name));
		snprintf(s, sizeof(s), "Enter the password (network key) for \"%s\".", name);
		SetDlgItemText(dlg, IDC_PROMPT, s);
		SendDlgItemMessage(dlg, IDC_PASS, EM_LIMITTEXT, 64, 0);
		CheckDlgButton(dlg, IDC_AUTO, BST_CHECKED);
		/* TKIP only matters for a network that offers nothing better */
		EnableWindow(GetDlgItem(dlg, IDC_TKIP), c->r->security == KEXWIFI_SEC_WPA);
		CheckDlgButton(dlg, IDC_TKIP, c->r->security == KEXWIFI_SEC_WPA ? BST_UNCHECKED : BST_UNCHECKED);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDC_SHOW:
			show_chars(dlg);
			return TRUE;
		case IDOK:
			GetDlgItemText(dlg, IDC_PASS, c->pass, sizeof(c->pass));
			if (!valid_pass(c->pass))
			{
				MessageBox(dlg, "A WPA2 password has 8 to 63 characters (or is a key of 64 hex digits).", "Wi-Fi",
				           MB_OK | MB_ICONEXCLAMATION);
				return TRUE;
			}
			c->autoj = IsDlgButtonChecked(dlg, IDC_AUTO) == BST_CHECKED;
			c->tkip = IsDlgButtonChecked(dlg, IDC_TKIP) == BST_CHECKED;
			if (c->r->security == KEXWIFI_SEC_WPA && !c->tkip)
			{
				MessageBox(dlg, "This network only offers older WPA (TKIP). To join it anyway, tick \"Allow older "
				                "WPA\". Better: switch the router to WPA2 (AES).",
				           "Wi-Fi", MB_OK | MB_ICONEXCLAMATION);
				return TRUE;
			}
			EndDialog(dlg, IDOK);
			return TRUE;
		case IDCANCEL:
			EndDialog(dlg, IDCANCEL);
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL CALLBACK add_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	struct pass_ctx *c = (struct pass_ctx *) GetWindowLong(dlg, DWL_USER);
	switch (msg)
	{
	case WM_INITDIALOG:
		c = (struct pass_ctx *) lp;
		SetWindowLong(dlg, DWL_USER, (LONG) c);
		SendDlgItemMessage(dlg, IDC_SSID, EM_LIMITTEXT, 32, 0);
		SendDlgItemMessage(dlg, IDC_PASS, EM_LIMITTEXT, 64, 0);
		SendDlgItemMessage(dlg, IDC_SECURITY, CB_ADDSTRING, 0, (LPARAM) "WPA2 (password)");
		SendDlgItemMessage(dlg, IDC_SECURITY, CB_ADDSTRING, 0, (LPARAM) "Open (no password, not encrypted)");
		SendDlgItemMessage(dlg, IDC_SECURITY, CB_SETCURSEL, 0, 0);
		CheckDlgButton(dlg, IDC_AUTO, BST_CHECKED);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDC_SHOW:
			show_chars(dlg);
			return TRUE;
		case IDC_SECURITY:
			if (HIWORD(wp) == CBN_SELCHANGE)
				EnableWindow(GetDlgItem(dlg, IDC_PASS), SendDlgItemMessage(dlg, IDC_SECURITY, CB_GETCURSEL, 0, 0) == 0);
			return TRUE;
		case IDOK:
		{
			char name[40];
			GetDlgItemText(dlg, IDC_SSID, name, sizeof(name));
			c->open = SendDlgItemMessage(dlg, IDC_SECURITY, CB_GETCURSEL, 0, 0) == 1;
			GetDlgItemText(dlg, IDC_PASS, c->pass, sizeof(c->pass));
			if (!name[0])
			{
				MessageBox(dlg, "Type the network's name.", "Wi-Fi", MB_OK | MB_ICONEXCLAMATION);
				return TRUE;
			}
			if (!c->open && !valid_pass(c->pass))
			{
				MessageBox(dlg, "A WPA2 password has 8 to 63 characters (or is a key of 64 hex digits).", "Wi-Fi",
				           MB_OK | MB_ICONEXCLAMATION);
				return TRUE;
			}
			if (c->open)
				c->pass[0] = 0;
			/* the name as the router sends it: UTF-8 */
			{
				WCHAR w[40];
				int n = MultiByteToWideChar(CP_ACP, 0, name, -1, w, 40);
				c->ssid_len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n - 1, (char *) c->ssid, 32, NULL, NULL) : 0;
				if (c->ssid_len <= 0)
				{
					c->ssid_len = (int) strlen(name);
					memcpy(c->ssid, name, (size_t) c->ssid_len);
				}
			}
			c->autoj = IsDlgButtonChecked(dlg, IDC_AUTO) == BST_CHECKED;
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		case IDCANCEL:
			EndDialog(dlg, IDCANCEL);
			return TRUE;
		}
	}
	return FALSE;
}

static void start_connect(const unsigned char *ssid, int len)
{
	request(KEXWIFI_REQ_CONNECT, ssid, len);
	ssid_text(ssid, len, pending_ssid, sizeof(pending_ssid));
	pending_len = len;
	pending_ticks = 0;
}

static void connect_row(HWND owner, const struct row *r)
{
	char name[64], s[400];
	if (!nadapters)
		return;
	ssid_text(r->ssid, r->ssid_len, name, sizeof(name));
	switch (r->security)
	{
	case KEXWIFI_SEC_WEP:
		snprintf(s, sizeof(s), "\"%s\" uses WEP, an old encryption that can be broken in minutes. KernelExEx does "
		                       "not join WEP networks.\n\nSwitch the router to WPA2 (AES), then connect.", name);
		MessageBox(owner, s, "Wi-Fi", MB_OK | MB_ICONINFORMATION);
		return;
	case KEXWIFI_SEC_WPA3:
		snprintf(s, sizeof(s), "\"%s\" only allows WPA3, which is not supported yet.\n\nIf the router has a "
		                       "\"WPA2/WPA3\" (transition) mode, turning it on lets this computer connect with WPA2.",
		         name);
		MessageBox(owner, s, "Wi-Fi", MB_OK | MB_ICONINFORMATION);
		return;
	case KEXWIFI_SEC_ENT:
		snprintf(s, sizeof(s), "\"%s\" uses WPA2-Enterprise (a user name and a server certificate), which is not "
		                       "supported yet.", name);
		MessageBox(owner, s, "Wi-Fi", MB_OK | MB_ICONINFORMATION);
		return;
	case KEXWIFI_SEC_OPEN:
		if (!r->saved)
		{
			snprintf(s, sizeof(s), "\"%s\" is an open network: what you send over it is not encrypted, and others "
			                       "nearby can see it (secure web pages, HTTPS, stay encrypted).\n\nConnect, and "
			                       "connect automatically from now on?", name);
			if (MessageBox(owner, s, "Wi-Fi", MB_YESNO | MB_ICONQUESTION) != IDYES)
				return;
			save_network(r->ssid, r->ssid_len, NULL, 0, 1);
		}
		break;
	default:
		if (!r->saved)
		{
			struct pass_ctx c;
			memset(&c, 0, sizeof(c));
			c.r = r;
			if (DialogBoxParam(inst, MAKEINTRESOURCE(IDD_PASS), owner, pass_proc, (LPARAM) &c) != IDOK)
				return;
			save_network(r->ssid, r->ssid_len, c.pass, c.tkip, c.autoj);
			SecureZeroMemory(c.pass, sizeof(c.pass));
		}
	}
	start_connect(r->ssid, r->ssid_len);
}

static void forget_row(HWND owner, const struct row *r)
{
	char name[64], s[200], sub[80], hx[65];
	HKEY nets;
	ssid_text(r->ssid, r->ssid_len, name, sizeof(name));
	snprintf(s, sizeof(s), "Forget \"%s\"? Its saved settings (and its password, if it has one) are removed, and this computer no longer joins it by itself.",
	         name);
	if (MessageBox(owner, s, "Wi-Fi", MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;
	hex_name(r->ssid, r->ssid_len, hx);
	nets = open_key("Networks", 0);
	if (nets)
	{
		RegDeleteKey(nets, hx);
		RegCloseKey(nets);
	}
	(void) sub;
	request(KEXWIFI_REQ_RELOAD, NULL, 0);
	shown_scan_seq = 0xffffffff;
}

/* ---- settings: the taskbar icon ------------------------------------------------------- */
static void set_run_key(int on)
{
	HKEY h;
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, RUN_KEY, 0, KEY_ALL_ACCESS, &h) != ERROR_SUCCESS)
		return;
	if (on)
	{
		char path[MAX_PATH + 16], cmd[MAX_PATH + 32];
		GetModuleFileName(NULL, path, MAX_PATH);
		snprintf(cmd, sizeof(cmd), "\"%s\" /tray", path);
		RegSetValueEx(h, "KexWifi", 0, REG_SZ, (const BYTE *) cmd, (DWORD) strlen(cmd) + 1);
	}
	else
		RegDeleteValue(h, "KexWifi");
	RegCloseKey(h);
}

/* ---- the window -------------------------------------------------------------------- */
static void refresh_now(void)
{
	if (nadapters && pending_request() == 0)
		request(KEXWIFI_REQ_SCAN, NULL, 0);
}

static BOOL CALLBACK main_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		HWND lv = GetDlgItem(dlg, IDC_LIST);
		LVCOLUMN col;
		int i;
		static const char *const names[] = { "Network", "Signal", "Security", "Saved" };
		static const int widths[] = { 140, 55, 150, 40 };
		main_dlg = dlg;
		SendMessage(dlg, WM_SETICON, ICON_BIG, (LPARAM) LoadIcon(inst, MAKEINTRESOURCE(IDI_WIFI)));
		SendMessage(dlg, WM_SETICON, ICON_SMALL, (LPARAM) LoadImage(inst, MAKEINTRESOURCE(IDI_WIFI), IMAGE_ICON, 16, 16, 0));
		ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
		ListView_SetImageList(lv, bars_list, LVSIL_SMALL);
		memset(&col, 0, sizeof(col));
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		for (i = 0; i < 4; i++)
		{
			col.pszText = (char *) names[i];
			col.cx = widths[i];
			col.iSubItem = i;
			ListView_InsertColumn(lv, i, &col);
		}
		CheckDlgButton(dlg, IDC_TRAYICON, want_tray ? BST_CHECKED : BST_UNCHECKED);
		read_adapters();
		shown_scan_seq = 0xffffffff;
		update_main();
		refresh_now();
		return TRUE;
	}
	case WM_NOTIFY:
	{
		NMHDR *nh = (NMHDR *) lp;
		if (nh->idFrom == IDC_LIST)
		{
			if (nh->code == LVN_ITEMCHANGED)
				update_buttons();
			else if (nh->code == NM_DBLCLK && selected_row() >= 0 && IsWindowEnabled(GetDlgItem(dlg, IDC_CONNECT)))
				connect_row(dlg, &rows[selected_row()]);
		}
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDC_ADAPTER:
			if (HIWORD(wp) == CBN_SELCHANGE)
			{
				cur_adapter = (int) SendMessage((HWND) lp, CB_GETCURSEL, 0, 0);
				if (cur_adapter < 0 || cur_adapter >= nadapters)
					cur_adapter = 0;
				shown_scan_seq = 0xffffffff;
				update_main();
				refresh_now();
			}
			return TRUE;
		case IDC_CONNECT:
			if (selected_row() >= 0)
				connect_row(dlg, &rows[selected_row()]);
			update_main();
			return TRUE;
		case IDC_DISCONNECT:
			if (nadapters)
			{
				request(adapters[cur_adapter].state == KEXWIFI_OFF ? KEXWIFI_REQ_ON : KEXWIFI_REQ_OFF, NULL, 0);
				pending_len = 0;
			}
			return TRUE;
		case IDC_FORGET:
			if (selected_row() >= 0)
				forget_row(dlg, &rows[selected_row()]);
			fill_list();
			return TRUE;
		case IDC_REFRESH:
			refresh_now();
			return TRUE;
		case IDC_ADDNET:
		{
			struct pass_ctx c;
			memset(&c, 0, sizeof(c));
			if (DialogBoxParam(inst, MAKEINTRESOURCE(IDD_ADD), dlg, add_proc, (LPARAM) &c) == IDOK)
			{
				save_network(c.ssid, c.ssid_len, c.pass, 0, c.autoj);
				SecureZeroMemory(c.pass, sizeof(c.pass));
				start_connect(c.ssid, c.ssid_len);
				fill_list();
			}
			return TRUE;
		}
		case IDC_TRAYICON:
		{
			HKEY h = open_key(NULL, 1);
			want_tray = IsDlgButtonChecked(dlg, IDC_TRAYICON) == BST_CHECKED;
			if (h)
			{
				set_dword(h, "TrayIcon", (DWORD) want_tray);
				RegCloseKey(h);
			}
			set_run_key(want_tray);
			tray_update();
			return TRUE;
		}
		case IDCANCEL:
			DestroyWindow(dlg);
			main_dlg = NULL;
			if (!want_tray)
				PostQuitMessage(0);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void show_main(void)
{
	if (!main_dlg)
		CreateDialog(inst, MAKEINTRESOURCE(IDD_MAIN), NULL, main_proc);
	if (main_dlg)
	{
		ShowWindow(main_dlg, SW_SHOWNORMAL);
		SetForegroundWindow(main_dlg);
	}
}

/* ---- the tray menu ----------------------------------------------------------------- */
static void tray_menu(void)
{
	HMENU m = CreatePopupMenu();
	POINT pt;
	int i, n = 0, cmd;
	const struct adapter *a = nadapters ? &adapters[cur_adapter] : NULL;
	AppendMenu(m, MF_STRING, MENU_OPEN, "&Open Wi-Fi...");
	SetMenuDefaultItem(m, MENU_OPEN, FALSE);
	read_rows();
	if (a && !a->fixed && a->state != KEXWIFI_OFF)
	{
		AppendMenu(m, MF_SEPARATOR, 0, NULL);
		for (i = 0; i < nrows && n < 8; i++)
		{
			char name[80];
			if (rows[i].signal < 0)
				continue;
			ssid_text(rows[i].ssid, rows[i].ssid_len, name, 64);
			if (rows[i].security != KEXWIFI_SEC_OPEN && rows[i].security != KEXWIFI_SEC_WPA2 &&
			    rows[i].security != KEXWIFI_SEC_WPA)
				lstrcat(name, " (not supported)");
			AppendMenu(m, MF_STRING | (is_current(&rows[i]) ? MF_CHECKED : 0), MENU_NET + i, name);
			n++;
		}
		if (!n)
			AppendMenu(m, MF_STRING | MF_GRAYED, 0, "(no networks found yet)");
	}
	AppendMenu(m, MF_SEPARATOR, 0, NULL);
	if (a)
		AppendMenu(m, MF_STRING, a->state == KEXWIFI_OFF ? MENU_ON : MENU_OFF,
		           a->state == KEXWIFI_OFF ? "Turn Wi-Fi &on" : "&Disconnect");
	AppendMenu(m, MF_STRING, MENU_EXIT, "E&xit (hide this icon)");
	GetCursorPos(&pt);
	SetForegroundWindow(tray_wnd);
	cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, tray_wnd, NULL);
	PostMessage(tray_wnd, WM_NULL, 0, 0);
	DestroyMenu(m);
	if (cmd == MENU_OPEN)
		show_main();
	else if (cmd == MENU_OFF)
		request(KEXWIFI_REQ_OFF, NULL, 0);
	else if (cmd == MENU_ON)
		request(KEXWIFI_REQ_ON, NULL, 0);
	else if (cmd == MENU_EXIT)
	{
		if (main_dlg)
			DestroyWindow(main_dlg);
		PostQuitMessage(0);
	}
	else if (cmd >= MENU_NET && cmd < MENU_NET + nrows)
	{
		/* the connect path asks for a password itself when needed */
		struct row r = rows[cmd - MENU_NET];
		connect_row(NULL, &r);
	}
}

/* ---- an address when the connection comes up --------------------------------------- *
 * Windows 98 asks for a DHCP address when it starts, usually before the
 * Wi-Fi has connected, and then not again for minutes. So when an adapter
 * connects (or moves to another network), its DHCP lease is released and
 * renewed through IP Helper, the adapter found by its MAC address. */
typedef DWORD(WINAPI *get_adapters_fn)(PIP_ADAPTER_INFO, PULONG);
typedef DWORD(WINAPI *get_ifinfo_fn)(PIP_INTERFACE_INFO, PULONG);
typedef DWORD(WINAPI *renew_fn)(PIP_ADAPTER_INDEX_MAP);
typedef DWORD(WINAPI *release_fn)(PIP_ADAPTER_INDEX_MAP);

/* the last renewal's outcome, as HKLM\Software\KernelEx\WiFi "LastRenew" */
static void renew_note(const char *what)
{
	HKEY h = open_key(NULL, 1);
	char line[200];
	if (!h)
		return;
	snprintf(line, sizeof(line), "%s (tick %lu)", what, GetTickCount() / 1000);
	RegSetValueEx(h, "LastRenew", 0, REG_SZ, (const BYTE *) line, (DWORD) strlen(line) + 1);
	RegCloseKey(h);
}

static void renew_dhcp(const unsigned char mac[6])
{
	static HMODULE iph;
	static get_adapters_fn get_adapters;
	static get_ifinfo_fn get_ifinfo;
	static renew_fn renew;
	static release_fn release;
	static unsigned char abuf[8192], ibuf[2048];
	ULONG alen = sizeof(abuf), ilen = sizeof(ibuf);
	PIP_ADAPTER_INFO ai;
	PIP_INTERFACE_INFO ii = (PIP_INTERFACE_INFO) ibuf;
	LONG i;
	if (!iph)
	{
		iph = LoadLibraryA("iphlpapi.dll");
		if (!iph)
			return;
		get_adapters = (get_adapters_fn) GetProcAddress(iph, "GetAdaptersInfo");
		get_ifinfo = (get_ifinfo_fn) GetProcAddress(iph, "GetInterfaceInfo");
		renew = (renew_fn) GetProcAddress(iph, "IpRenewAddress");
		release = (release_fn) GetProcAddress(iph, "IpReleaseAddress");
	}
	char note[160];
	DWORD e1, e2;
	note[0] = 0;
	if (!get_adapters || !get_ifinfo || !renew)
	{
		renew_note("IP Helper functions missing");
		return;
	}
	e1 = get_adapters((PIP_ADAPTER_INFO) abuf, &alen);
	if (e1 != NO_ERROR)
	{
		snprintf(note, sizeof(note), "GetAdaptersInfo %lu", e1);
		renew_note(note);
		return;
	}
	/* Windows 98 wants the size asked for first (122 otherwise) */
	ilen = 0;
	get_ifinfo(NULL, &ilen);
	e2 = ilen && ilen <= sizeof(ibuf) ? get_ifinfo(ii, &ilen) : ERROR_INSUFFICIENT_BUFFER;
	for (ai = (PIP_ADAPTER_INFO) abuf; ai; ai = ai->Next)
	{
		IP_ADAPTER_INDEX_MAP own, *map = NULL;
		DWORD r1, r2;
		if (ai->AddressLength != 6 || memcmp(ai->Address, mac, 6) || !ai->DhcpEnabled)
			continue;
		if (e2 == NO_ERROR)
			for (i = 0; i < ii->NumAdapters; i++)
				if (ii->Adapter[i].Index == ai->Index)
					map = &ii->Adapter[i];
		if (!map)
		{
			/* no interface table: the index and name from GetAdaptersInfo */
			memset(&own, 0, sizeof(own));
			own.Index = ai->Index;
			MultiByteToWideChar(CP_ACP, 0, ai->AdapterName, -1, own.Name, MAX_ADAPTER_NAME);
			map = &own;
		}
		/* release first: a renewal alone asks the previous network's DHCP
		 * server, which is not reachable after moving to another network;
		 * after a release the request is a broadcast any server answers */
		r1 = release ? release(map) : 0;
		r2 = renew(map);
		snprintf(note, sizeof(note), "adapter index %lu: release %lu, renew %lu (interface table %lu)", ai->Index, r1,
		         r2, e2);
	}
	renew_note(note[0] ? note : "no DHCP adapter with this MAC address");
}

static void watch_connections(void)
{
	int i, k;
	for (i = 0; i < nadapters; i++)
	{
		const struct adapter *a = &adapters[i];
		for (k = 0; k < MAX_ADAPTERS && seen[k].key[0] && lstrcmp(seen[k].key, a->key); k++)
			;
		if (k == MAX_ADAPTERS)
			continue;
		if (!seen[k].key[0])
		{
			lstrcpyn(seen[k].key, a->key, sizeof(seen[k].key));
			seen[k].state = KEXWIFI_OFF;
		}
		/* two seconds after connecting: the link is settled by then */
		if (a->state == KEXWIFI_CONNECTED && seen[k].state != KEXWIFI_CONNECTED && a->has_mac)
		{
			seen[k].renew_in = 2;
			renew_note("connected: renewing in 2 s");
		}
		seen[k].state = a->state;
		if (seen[k].renew_in && --seen[k].renew_in == 0 && a->state == KEXWIFI_CONNECTED)
			renew_dhcp(a->mac);
	}
}

static void tick(void)
{
	read_adapters();
	ticks++;
	watch_connections();
	if (pending_len)
	{
		const struct adapter *a = nadapters ? &adapters[cur_adapter] : NULL;
		if (!a || a->state == KEXWIFI_CONNECTED || ++pending_ticks > 40)
		{
			if (a && a->state != KEXWIFI_CONNECTED && main_dlg)
				SetDlgItemText(main_dlg, IDC_NOTE, "Could not connect. If the network has a password, check it: "
				                                   "select the network, Forget, then Connect and type it again.");
			pending_len = 0;
		}
	}
	tray_update();
	if (main_dlg && IsWindowVisible(main_dlg))
	{
		update_main();
		if (ticks % 4 == 0)
			refresh_now();
	}
}

static LRESULT CALLBACK tray_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == msg_taskbar_created)
	{
		tray_shown = 0;   /* Explorer restarted */
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
		if (lp == WM_LBUTTONDBLCLK)
			show_main();
		else if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
			tray_menu();
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
	HKEY h;
	int i, tray_only;
	const char *cmd = GetCommandLine();
	INITCOMMONCONTROLSEX icc;
	inst = GetModuleHandle(NULL);
	tray_only = strstr(cmd, "/tray") != NULL || strstr(cmd, "/TRAY") != NULL;

	other = FindWindow(TRAY_CLASS, NULL);
	if (other)
	{
		if (!tray_only)
		{
			HWND dlg;
			int i;
			PostMessage(other, WM_SHOWMAIN, 0, 0);
			/* Windows 98 lets only the foreground program hand over the
			 * keyboard focus: this one is (it was just started) */
			for (i = 0; i < 20; i++)
			{
				dlg = FindWindow("#32770", "Wi-Fi");
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
	h = open_key(NULL, 1);
	if (h)
	{
		want_tray = get_dword(h, "TrayIcon", 1) != 0;
		RegCloseKey(h);
	}
	if (tray_only && !want_tray)
		ExitProcess(0);

	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icc);
	for (i = 0; i < 8; i++)
		icons[i] = make_icon(i, 16);
	bars_list = ImageList_Create(16, 16, ILC_COLOR8 | ILC_MASK, 5, 0);
	ImageList_AddIcon(bars_list, icons[0]);
	for (i = 1; i <= 4; i++)
		ImageList_AddIcon(bars_list, icons[3 + i]);

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = tray_proc;
	wc.hInstance = inst;
	wc.lpszClassName = TRAY_CLASS;
	RegisterClass(&wc);
	tray_wnd = CreateWindow(TRAY_CLASS, "KernelExEx Wi-Fi", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
	msg_taskbar_created = RegisterWindowMessage("TaskbarCreated");
	SetTimer(tray_wnd, 1, 1000, NULL);
	read_adapters();
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
	if (tray_wnd && IsWindow(tray_wnd))
	{
		NOTIFYICONDATA nid;
		memset(&nid, 0, sizeof(nid));
		nid.cbSize = NOTIFYICONDATA_V1_SIZE;
		nid.hWnd = tray_wnd;
		nid.uID = 1;
		if (tray_shown)
			Shell_NotifyIcon(NIM_DELETE, &nid);
	}
	ExitProcess(0);
}
