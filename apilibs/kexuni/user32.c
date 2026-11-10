/*
 *  KernelExEx - kexuni.dll: USER32 wide functions. GPLv2.
 */
#include "uni.h"
#include <dde.h>
#include <ddeml.h>
#include <dbt.h>

/* ---- resources -------------------------------------------------------- */

#define RES_LOAD(type, call)                                                                                           \
	char *a = kxu_res(name);                                                                                       \
	type r = call;                                                                                                 \
	kxu_res_free(a);                                                                                               \
	return r

HICON KXU(LoadIconW)(HINSTANCE inst, LPCWSTR name)
{
	RES_LOAD(HICON, LoadIconA(inst, a));
}

HCURSOR KXU(LoadCursorW)(HINSTANCE inst, LPCWSTR name)
{
	RES_LOAD(HCURSOR, LoadCursorA(inst, a));
}

HBITMAP KXU(LoadBitmapW)(HINSTANCE inst, LPCWSTR name)
{
	RES_LOAD(HBITMAP, LoadBitmapA(inst, a));
}

HACCEL KXU(LoadAcceleratorsW)(HINSTANCE inst, LPCWSTR name)
{
	RES_LOAD(HACCEL, LoadAcceleratorsA(inst, a));
}

HANDLE KXU(LoadImageW)(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags)
{
	RES_LOAD(HANDLE, LoadImageA(inst, a, type, cx, cy, flags));
}

HCURSOR KXU(LoadCursorFromFileW)(LPCWSTR name)
{
	char *a = kxu_w2a(name);
	HCURSOR r = LoadCursorFromFileA(a);
	kxu_free(a);
	return r;
}

/* ---- window properties ------------------------------------------------ */

HANDLE KXU(GetPropW)(HWND hwnd, LPCWSTR name)
{
	RES_LOAD(HANDLE, GetPropA(hwnd, a));
}

BOOL KXU(SetPropW)(HWND hwnd, LPCWSTR name, HANDLE data)
{
	RES_LOAD(BOOL, SetPropA(hwnd, a, data));
}

HANDLE KXU(RemovePropW)(HWND hwnd, LPCWSTR name)
{
	RES_LOAD(HANDLE, RemovePropA(hwnd, a));
}

typedef struct
{
	PROPENUMPROCEXW cb;
	LPARAM lp;
} PROPCTX;

static BOOL CALLBACK prop_ex_cb(HWND hwnd, LPSTR name, HANDLE data, ULONG_PTR lp)
{
	PROPCTX *c = (PROPCTX *) lp;
	LPWSTR w = IS_INTRES(name) ? (LPWSTR) name : kxu_a2w_new(CP_ACP, name);
	BOOL r = c->cb(hwnd, w, data, (ULONG_PTR) c->lp);
	if (!IS_INTRES(w))
		kxu_free(w);
	return r;
}

int KXU(EnumPropsExW)(HWND hwnd, PROPENUMPROCEXW cb, LPARAM lp)
{
	PROPCTX c;
	c.cb = cb;
	c.lp = lp;
	return EnumPropsExA(hwnd, (PROPENUMPROCEXA) prop_ex_cb, (LPARAM) &c);
}

static BOOL CALLBACK prop_cb(HWND hwnd, LPWSTR name, HANDLE data, ULONG_PTR lp)
{
	return ((PROPENUMPROCW) lp)(hwnd, name, data);
}

int KXU(EnumPropsW)(HWND hwnd, PROPENUMPROCW cb)
{
	return kxu_EnumPropsExW(hwnd, (PROPENUMPROCEXW) prop_cb, (LPARAM) cb);
}

/* ---- windows, classes, atoms ------------------------------------------ */

HWND KXU(FindWindowW)(LPCWSTR cls, LPCWSTR title)
{
	char *c = kxu_res(cls), *t = kxu_w2a(title);
	HWND r = FindWindowA(c, t);
	kxu_res_free(c);
	kxu_free(t);
	return r;
}

HWND KXU(FindWindowExW)(HWND parent, HWND after, LPCWSTR cls, LPCWSTR title)
{
	char *c = kxu_res(cls), *t = kxu_w2a(title);
	HWND r = FindWindowExA(parent, after, c, t);
	kxu_res_free(c);
	kxu_free(t);
	return r;
}

int KXU(GetClassNameW)(HWND hwnd, LPWSTR buf, int cch)
{
	char a[512];
	if (!GetClassNameA(hwnd, a, sizeof(a)))
		return 0;
	return kxu_a2w(CP_ACP, a, -1, buf, cch, NULL);
}

UINT KXU(RegisterWindowMessageW)(LPCWSTR s)
{
	char *a = kxu_w2a(s);
	UINT r = RegisterWindowMessageA(a);
	kxu_free(a);
	return r;
}

UINT KXU(RegisterClipboardFormatW)(LPCWSTR s)
{
	char *a = kxu_w2a(s);
	UINT r = RegisterClipboardFormatA(a);
	kxu_free(a);
	return r;
}

int KXU(GetClipboardFormatNameW)(UINT fmt, LPWSTR buf, int cch)
{
	char a[512];
	if (!GetClipboardFormatNameA(fmt, a, sizeof(a)))
		return 0;
	return kxu_a2w(CP_ACP, a, -1, buf, cch, NULL);
}

long KXU(BroadcastSystemMessageW)(DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp)
{
	return BroadcastSystemMessageA(flags, recipients, msg, wp, lp);
}

HDEVNOTIFY KXU(RegisterDeviceNotificationW)(HANDLE recipient, LPVOID filter, DWORD flags)
{
	/* DEV_BROADCAST_DEVICEINTERFACE_W filters carry a name we can convert;
	 * the others are character-free */
	DEV_BROADCAST_HDR *h = (DEV_BROADCAST_HDR *) filter;
	if (h && h->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
	{
		DEV_BROADCAST_DEVICEINTERFACE_W *w = (DEV_BROADCAST_DEVICEINTERFACE_W *) filter;
		DEV_BROADCAST_DEVICEINTERFACE_A *a;
		int alen;
		char *name = kxu_w2a_n(CP_ACP, w->dbcc_name, -1, &alen);
		HDEVNOTIFY r;
		a = (DEV_BROADCAST_DEVICEINTERFACE_A *) kxu_alloc(sizeof(*a) + alen + 1);
		if (!a || !name)
		{
			kxu_free(a);
			kxu_free(name);
			return NULL;
		}
		a->dbcc_size = sizeof(*a) + alen;
		a->dbcc_devicetype = w->dbcc_devicetype;
		a->dbcc_reserved = 0;
		a->dbcc_classguid = w->dbcc_classguid;
		lstrcpyA(a->dbcc_name, name);
		r = RegisterDeviceNotificationA(recipient, a, flags);
		kxu_free(a);
		kxu_free(name);
		return r;
	}
	return RegisterDeviceNotificationA(recipient, filter, flags);
}

/* ---- keyboard ------------------------------------------------------- */

int KXU(GetKeyNameTextW)(LONG lp, LPWSTR buf, int cch)
{
	char a[256];
	if (!GetKeyNameTextA(lp, a, sizeof(a)))
		return 0;
	return kxu_a2w(CP_ACP, a, -1, buf, cch, NULL);
}

BOOL KXU(GetKeyboardLayoutNameW)(LPWSTR buf)
{
	char a[KL_NAMELENGTH];
	if (!GetKeyboardLayoutNameA(a))
		return FALSE;
	kxu_a2w(CP_ACP, a, -1, buf, KL_NAMELENGTH, NULL);
	return TRUE;
}

HKL KXU(LoadKeyboardLayoutW)(LPCWSTR id, UINT flags)
{
	char *a = kxu_w2a(id);
	HKL r = LoadKeyboardLayoutA(a, flags);
	kxu_free(a);
	return r;
}

static UINT vk_char_a2w(UINT r)
{
	/* MAPVK_VK_TO_CHAR: low word is the character, bit 31 marks dead keys */
	char c = (char) (r & 0xFF);
	WCHAR w = (BYTE) c;
	if (!(r & 0xFFFF))
		return r;
	MultiByteToWideChar(CP_ACP, 0, &c, 1, &w, 1);
	return (r & 0x80000000u) | w;
}

UINT KXU(MapVirtualKeyW)(UINT code, UINT type)
{
	UINT r = MapVirtualKeyA(code, type);
	return type == 2 ? vk_char_a2w(r) : r;
}

UINT KXU(MapVirtualKeyExW)(UINT code, UINT type, HKL hkl)
{
	UINT r = MapVirtualKeyExA(code, type, hkl);
	return type == 2 ? vk_char_a2w(r) : r;
}

static int w_to_byte(WCHAR c, char *out)
{
	char a[4];
	BOOL used = FALSE;
	int n = WideCharToMultiByte(CP_ACP, 0, &c, 1, a, sizeof(a), NULL, &used);
	if (n != 1 || used)
		return 0;
	*out = a[0];
	return 1;
}

SHORT KXU(VkKeyScanW)(WCHAR c)
{
	char a;
	return w_to_byte(c, &a) ? VkKeyScanA(a) : -1;
}

SHORT KXU(VkKeyScanExW)(WCHAR c, HKL hkl)
{
	char a;
	return w_to_byte(c, &a) ? VkKeyScanExA(a, hkl) : -1;
}

/* ---- text output ------------------------------------------------------ */

int KXU(DrawTextExW)(HDC hdc, LPWSTR s, int cch, LPRECT rc, UINT fmt, LPDRAWTEXTPARAMS dtp)
{
	int alen, r;
	char *a = kxu_w2a_n(CP_ACP, s, cch, &alen);
	char *big = NULL;
	if (!a)
		return 0;
	if (fmt & DT_MODIFYSTRING)
	{
		/* room for the "..." DrawText may add */
		big = (char *) kxu_alloc(alen + 5);
		if (!big)
		{
			kxu_free(a);
			return 0;
		}
		lstrcpyA(big, a);
		kxu_free(a);
		a = big;
	}
	r = DrawTextExA(hdc, a, alen, rc, fmt, dtp);
	if (dtp)
		dtp->uiLengthDrawn = (UINT) kxu_wchars_in(CP_ACP, a, (int) dtp->uiLengthDrawn);
	if ((fmt & DT_MODIFYSTRING) && s && cch < 0)
		kxu_a2w(CP_ACP, a, -1, s, lstrlenW(s) + 5, NULL);
	kxu_free(a);
	return r;
}

int KXU(DrawTextW)(HDC hdc, LPCWSTR s, int cch, LPRECT rc, UINT fmt)
{
	return kxu_DrawTextExW(hdc, (LPWSTR) s, cch, rc, fmt & ~DT_MODIFYSTRING, NULL);
}

DWORD KXU(GetTabbedTextExtentW)(HDC hdc, LPCWSTR s, int cch, int ntabs, const INT *tabs)
{
	int alen;
	char *a = kxu_w2a_n(CP_ACP, s, cch, &alen);
	DWORD r = a ? GetTabbedTextExtentA(hdc, a, alen, ntabs, tabs) : 0;
	kxu_free(a);
	return r;
}

LONG KXU(TabbedTextOutW)(HDC hdc, int x, int y, LPCWSTR s, int cch, int ntabs, const INT *tabs, int origin)
{
	int alen;
	char *a = kxu_w2a_n(CP_ACP, s, cch, &alen);
	LONG r = a ? TabbedTextOutA(hdc, x, y, a, alen, ntabs, tabs, origin) : 0;
	kxu_free(a);
	return r;
}

BOOL KXU(GrayStringW)(HDC hdc, HBRUSH br, GRAYSTRINGPROC fn, LPARAM data, int cch, int x, int y, int cx, int cy)
{
	if (!fn)
	{
		/* data is the string */
		int alen;
		char *a = kxu_w2a_n(CP_ACP, (LPCWSTR) data, cch ? cch : -1, &alen);
		BOOL r = a ? GrayStringA(hdc, br, NULL, (LPARAM) a, alen, x, y, cx, cy) : FALSE;
		kxu_free(a);
		return r;
	}
	return GrayStringA(hdc, br, fn, data, cch, x, y, cx, cy);
}

BOOL KXU(DrawStateW)(HDC hdc, HBRUSH br, DRAWSTATEPROC fn, LPARAM ld, WPARAM wd, int x, int y, int cx, int cy, UINT fl)
{
	UINT type = fl & 0x000F;
	if ((type == DST_TEXT || type == DST_PREFIXTEXT) && !fn)
	{
		int alen;
		char *a = kxu_w2a_n(CP_ACP, (LPCWSTR) ld, wd ? (int) wd : -1, &alen);
		BOOL r = a ? DrawStateA(hdc, br, NULL, (LPARAM) a, (WPARAM) alen, x, y, cx, cy, fl) : FALSE;
		kxu_free(a);
		return r;
	}
	return DrawStateA(hdc, br, fn, ld, wd, x, y, cx, cy, fl);
}

/* ---- menus ------------------------------------------------------------ */

int KXU(GetMenuStringW)(HMENU menu, UINT item, LPWSTR buf, int cch, UINT flags)
{
	int n = GetMenuStringA(menu, item, NULL, 0, flags), r = 0;
	char *a;
	a = (char *) kxu_alloc(n + 2);
	if (a && GetMenuStringA(menu, item, a, n + 1, flags) >= 0)
		r = (!buf || !cch) ? kxu_a2w_len(CP_ACP, a, -1) : kxu_a2w(CP_ACP, a, -1, buf, cch, NULL);
	kxu_free(a);
	return r;
}

static BOOL mii_has_string(const MENUITEMINFOW *w)
{
	if (w->fMask & 0x00000040 /* MIIM_STRING */)
		return TRUE;
	return (w->fMask & MIIM_TYPE) && !(w->fType & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW));
}

static char *mii_w2a(const MENUITEMINFOW *w, MENUITEMINFOA *a)
{
	char *s = NULL;
	CopyMemory(a, w, w->cbSize < sizeof(*a) ? w->cbSize : sizeof(*a));
	if (mii_has_string(w) && w->dwTypeData)
		a->dwTypeData = s = kxu_w2a(w->dwTypeData);
	return s;
}

BOOL KXU(SetMenuItemInfoW)(HMENU menu, UINT item, BOOL bypos, LPCMENUITEMINFOW mii)
{
	MENUITEMINFOA a;
	char *s = mii_w2a(mii, &a);
	BOOL r = SetMenuItemInfoA(menu, item, bypos, &a);
	kxu_free(s);
	return r;
}

BOOL KXU(InsertMenuItemW)(HMENU menu, UINT item, BOOL bypos, LPCMENUITEMINFOW mii)
{
	MENUITEMINFOA a;
	char *s = mii_w2a(mii, &a);
	BOOL r = InsertMenuItemA(menu, item, bypos, &a);
	kxu_free(s);
	return r;
}

BOOL KXU(GetMenuItemInfoW)(HMENU menu, UINT item, BOOL bypos, LPMENUITEMINFOW mii)
{
	MENUITEMINFOA a;
	BOOL want = mii_has_string(mii) && mii->dwTypeData && mii->cch;
	char *buf = NULL;
	BOOL r;
	CopyMemory(&a, mii, mii->cbSize < sizeof(a) ? mii->cbSize : sizeof(a));
	if (want)
	{
		a.cch = mii->cch * 2 + 2;
		a.dwTypeData = buf = (char *) kxu_alloc(a.cch);
	}
	else if (mii_has_string(mii))
		a.dwTypeData = NULL;
	r = GetMenuItemInfoA(menu, item, bypos, &a);
	if (r)
	{
		LPWSTR keep = mii->dwTypeData;
		UINT cch = mii->cch;
		CopyMemory(mii, &a, mii->cbSize < sizeof(a) ? mii->cbSize : sizeof(a));
		if (mii_has_string(mii))
		{
			mii->dwTypeData = keep;
			if (want && buf && (a.fType & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW)) == 0)
				mii->cch = (UINT) kxu_a2w(CP_ACP, buf, -1, keep, (int) cch, NULL);
			else if (!want)
				mii->cch = a.cch;   /* bytes: an upper bound on characters */
		}
	}
	kxu_free(buf);
	return r;
}

/* ---- dialogs and message boxes ---------------------------------------- */

int KXU(MessageBoxIndirectW)(const MSGBOXPARAMSW *p)
{
	MSGBOXPARAMSA a;
	char *t, *c, *i;
	int r;
	CopyMemory(&a, p, sizeof(a));
	a.cbSize = sizeof(a);
	a.lpszText = t = kxu_w2a(p->lpszText);
	a.lpszCaption = c = kxu_w2a(p->lpszCaption);
	a.lpszIcon = i = kxu_res(p->lpszIcon);
	r = MessageBoxIndirectA(&a);
	kxu_free(t);
	kxu_free(c);
	kxu_res_free(i);
	return r;
}

int KXU(DlgDirListW)(HWND dlg, LPWSTR path, int list, int stat, UINT type)
{
	char a[MAX_PATH * 2];
	int r;
	a[0] = 0;
	if (path)
		WideCharToMultiByte(CP_ACP, 0, path, -1, a, sizeof(a), NULL, NULL);
	r = DlgDirListA(dlg, path ? a : NULL, list, stat, type);
	if (r && path)
		kxu_a2w(CP_ACP, a, -1, path, MAX_PATH, NULL);
	return r;
}

int KXU(DlgDirListComboBoxW)(HWND dlg, LPWSTR path, int list, int stat, UINT type)
{
	char a[MAX_PATH * 2];
	int r;
	a[0] = 0;
	if (path)
		WideCharToMultiByte(CP_ACP, 0, path, -1, a, sizeof(a), NULL, NULL);
	r = DlgDirListComboBoxA(dlg, path ? a : NULL, list, stat, type);
	if (r && path)
		kxu_a2w(CP_ACP, a, -1, path, MAX_PATH, NULL);
	return r;
}

BOOL KXU(DlgDirSelectExW)(HWND dlg, LPWSTR buf, int cch, int id)
{
	char a[MAX_PATH * 2];
	BOOL r = DlgDirSelectExA(dlg, a, sizeof(a), id);
	kxu_a2w(CP_ACP, a, -1, buf, cch, NULL);
	return r;
}

BOOL KXU(DlgDirSelectComboBoxExW)(HWND dlg, LPWSTR buf, int cch, int id)
{
	char a[MAX_PATH * 2];
	BOOL r = DlgDirSelectComboBoxExA(dlg, a, sizeof(a), id);
	kxu_a2w(CP_ACP, a, -1, buf, cch, NULL);
	return r;
}

BOOL KXU(WinHelpW)(HWND hwnd, LPCWSTR file, UINT cmd, ULONG_PTR data)
{
	char *f = kxu_w2a(file), *d = NULL;
	BOOL r;
	switch (cmd)
	{
	case HELP_KEY:
	case HELP_PARTIALKEY:
	case HELP_COMMAND:
		d = kxu_w2a((LPCWSTR) data);
		data = (ULONG_PTR) d;
		break;
	}
	r = WinHelpA(hwnd, f, cmd, data);
	kxu_free(f);
	kxu_free(d);
	return r;
}

/* ---- display settings ------------------------------------------------- */

LONG KXU(ChangeDisplaySettingsExW)(LPCWSTR dev, LPDEVMODEW dm, HWND hwnd, DWORD flags, LPVOID param)
{
	char *d = kxu_w2a(dev);
	DEVMODEA *a = kxu_devmode_w2a(dm);
	LONG r = ChangeDisplaySettingsExA(d, a, hwnd, flags, param);
	kxu_free(d);
	kxu_free(a);
	return r;
}

LONG KXU(ChangeDisplaySettingsW)(LPDEVMODEW dm, DWORD flags)
{
	DEVMODEA *a = kxu_devmode_w2a(dm);
	LONG r = ChangeDisplaySettingsA(a, flags);
	kxu_free(a);
	return r;
}

BOOL KXU(EnumDisplaySettingsExW)(LPCWSTR dev, DWORD mode, LPDEVMODEW dm, DWORD flags)
{
	char *d = kxu_w2a(dev);
	struct
	{
		DEVMODEA dm;
		char extra[1024];
	} a;
	BOOL r;
	ZeroMemory(&a, sizeof(DEVMODEA));
	a.dm.dmSize = sizeof(DEVMODEA);
	a.dm.dmDriverExtra = 0;
	r = EnumDisplaySettingsExA(d, mode, &a.dm, flags);
	kxu_free(d);
	if (r)
	{
		WORD extra = dm->dmDriverExtra;
		DWORD room = (DWORD) dm->dmSize + extra;
		a.dm.dmDriverExtra = 0;
		kxu_devmode_a2w(&a.dm, dm, room < sizeof(DEVMODEW) ? sizeof(DEVMODEW) : room);
	}
	return r;
}

BOOL KXU(EnumDisplaySettingsW)(LPCWSTR dev, DWORD mode, LPDEVMODEW dm)
{
	return kxu_EnumDisplaySettingsExW(dev, mode, dm, 0);
}

BOOL KXU(EnumDisplayDevicesW)(LPCWSTR dev, DWORD n, PDISPLAY_DEVICEW dd, DWORD flags)
{
	char *d = kxu_w2a(dev);
	DISPLAY_DEVICEA a;
	BOOL r;
	ZeroMemory(&a, sizeof(a));
	a.cb = sizeof(a);
	r = EnumDisplayDevicesA(d, n, &a, flags);
	kxu_free(d);
	if (r)
	{
		kxu_a2w(CP_ACP, a.DeviceName, -1, dd->DeviceName, 32, NULL);
		kxu_a2w(CP_ACP, a.DeviceString, -1, dd->DeviceString, 128, NULL);
		dd->StateFlags = a.StateFlags;
		if (dd->cb >= sizeof(DISPLAY_DEVICEW))
		{
			kxu_a2w(CP_ACP, a.DeviceID, -1, dd->DeviceID, 128, NULL);
			kxu_a2w(CP_ACP, a.DeviceKey, -1, dd->DeviceKey, 128, NULL);
		}
	}
	return r;
}

BOOL KXU(GetMonitorInfoW)(HMONITOR mon, LPMONITORINFO mi)
{
	MONITORINFOEXA a;
	BOOL r;
	a.cbSize = mi->cbSize >= sizeof(MONITORINFOEXW) ? sizeof(MONITORINFOEXA) : sizeof(MONITORINFO);
	r = GetMonitorInfoA(mon, (LPMONITORINFO) &a);
	if (r)
	{
		DWORD cb = mi->cbSize;
		CopyMemory(mi, &a, sizeof(MONITORINFO));
		mi->cbSize = cb;
		if (cb >= sizeof(MONITORINFOEXW))
			kxu_a2w(CP_ACP, a.szDevice, -1, ((LPMONITORINFOEXW) mi)->szDevice, CCHDEVICENAME, NULL);
	}
	return r;
}

BOOL KXU(GetAltTabInfoW)(HWND hwnd, int item, PALTTABINFO ati, LPWSTR buf, UINT cch)
{
	/* not on Windows 98 SE: look it up */
	BOOL(WINAPI * fn)(HWND, int, PALTTABINFO, LPSTR, UINT) =
		(BOOL(WINAPI *)(HWND, int, PALTTABINFO, LPSTR, UINT)) GetProcAddress(GetModuleHandleA("user32.dll"),
		                                                                     "GetAltTabInfoA");
	char *a;
	BOOL r;
	if (!fn)
	{
		SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
		return FALSE;
	}
	a = buf ? (char *) kxu_alloc(cch * 2 + 2) : NULL;
	r = fn(hwnd, item, ati, a, buf ? cch * 2 + 2 : 0);
	if (r && buf)
		kxu_a2w(CP_ACP, a, -1, buf, (int) cch, NULL);
	kxu_free(a);
	return r;
}

/* ---- SystemParametersInfo --------------------------------------------- */

static void ncm_a2w(const NONCLIENTMETRICSA *a, NONCLIENTMETRICSW *w)
{
	w->iBorderWidth = a->iBorderWidth;
	w->iScrollWidth = a->iScrollWidth;
	w->iScrollHeight = a->iScrollHeight;
	w->iCaptionWidth = a->iCaptionWidth;
	w->iCaptionHeight = a->iCaptionHeight;
	kxu_logfont_a2w(&a->lfCaptionFont, &w->lfCaptionFont);
	w->iSmCaptionWidth = a->iSmCaptionWidth;
	w->iSmCaptionHeight = a->iSmCaptionHeight;
	kxu_logfont_a2w(&a->lfSmCaptionFont, &w->lfSmCaptionFont);
	w->iMenuWidth = a->iMenuWidth;
	w->iMenuHeight = a->iMenuHeight;
	kxu_logfont_a2w(&a->lfMenuFont, &w->lfMenuFont);
	kxu_logfont_a2w(&a->lfStatusFont, &w->lfStatusFont);
	kxu_logfont_a2w(&a->lfMessageFont, &w->lfMessageFont);
}

static void ncm_w2a(const NONCLIENTMETRICSW *w, NONCLIENTMETRICSA *a)
{
	a->cbSize = sizeof(*a);
	a->iBorderWidth = w->iBorderWidth;
	a->iScrollWidth = w->iScrollWidth;
	a->iScrollHeight = w->iScrollHeight;
	a->iCaptionWidth = w->iCaptionWidth;
	a->iCaptionHeight = w->iCaptionHeight;
	kxu_logfont_w2a(&w->lfCaptionFont, &a->lfCaptionFont);
	a->iSmCaptionWidth = w->iSmCaptionWidth;
	a->iSmCaptionHeight = w->iSmCaptionHeight;
	kxu_logfont_w2a(&w->lfSmCaptionFont, &a->lfSmCaptionFont);
	a->iMenuWidth = w->iMenuWidth;
	a->iMenuHeight = w->iMenuHeight;
	kxu_logfont_w2a(&w->lfMenuFont, &a->lfMenuFont);
	kxu_logfont_w2a(&w->lfStatusFont, &a->lfStatusFont);
	kxu_logfont_w2a(&w->lfMessageFont, &a->lfMessageFont);
}

BOOL KXU(SystemParametersInfoW)(UINT action, UINT uparam, PVOID pv, UINT winini)
{
	switch (action)
	{
	case SPI_GETICONTITLELOGFONT:
	{
		LOGFONTA a;
		if (!SystemParametersInfoA(action, sizeof(a), &a, winini))
			return FALSE;
		kxu_logfont_a2w(&a, (LOGFONTW *) pv);
		return TRUE;
	}
	case SPI_SETICONTITLELOGFONT:
	{
		LOGFONTA a;
		kxu_logfont_w2a((const LOGFONTW *) pv, &a);
		return SystemParametersInfoA(action, sizeof(a), &a, winini);
	}
	case SPI_GETNONCLIENTMETRICS:
	{
		NONCLIENTMETRICSA a;
		NONCLIENTMETRICSW *w = (NONCLIENTMETRICSW *) pv;
		a.cbSize = sizeof(a);
		if (!SystemParametersInfoA(action, sizeof(a), &a, winini))
			return FALSE;
		ncm_a2w(&a, w);
		return TRUE;
	}
	case SPI_SETNONCLIENTMETRICS:
	{
		NONCLIENTMETRICSA a;
		ncm_w2a((const NONCLIENTMETRICSW *) pv, &a);
		return SystemParametersInfoA(action, sizeof(a), &a, winini);
	}
	case SPI_GETICONMETRICS:
	{
		ICONMETRICSA a;
		ICONMETRICSW *w = (ICONMETRICSW *) pv;
		a.cbSize = sizeof(a);
		if (!SystemParametersInfoA(action, sizeof(a), &a, winini))
			return FALSE;
		w->iHorzSpacing = a.iHorzSpacing;
		w->iVertSpacing = a.iVertSpacing;
		w->iTitleWrap = a.iTitleWrap;
		kxu_logfont_a2w(&a.lfFont, &w->lfFont);
		return TRUE;
	}
	case SPI_SETICONMETRICS:
	{
		ICONMETRICSA a;
		const ICONMETRICSW *w = (const ICONMETRICSW *) pv;
		a.cbSize = sizeof(a);
		a.iHorzSpacing = w->iHorzSpacing;
		a.iVertSpacing = w->iVertSpacing;
		a.iTitleWrap = w->iTitleWrap;
		kxu_logfont_w2a(&w->lfFont, &a.lfFont);
		return SystemParametersInfoA(action, sizeof(a), &a, winini);
	}
	case SPI_SETDESKWALLPAPER:
	{
		char *a = pv ? kxu_w2a((LPCWSTR) pv) : NULL;
		BOOL r = SystemParametersInfoA(action, uparam, a, winini);
		kxu_free(a);
		return r;
	}
	case 0x0073: /* SPI_GETDESKWALLPAPER */
	{
		char a[MAX_PATH * 2];
		if (!SystemParametersInfoA(action, sizeof(a), a, winini))
			return FALSE;
		kxu_a2w(CP_ACP, a, -1, (LPWSTR) pv, (int) uparam, NULL);
		return TRUE;
	}
	case SPI_GETHIGHCONTRAST:
	{
		HIGHCONTRASTA a;
		HIGHCONTRASTW *w = (HIGHCONTRASTW *) pv;
		a.cbSize = sizeof(a);
		if (!SystemParametersInfoA(action, sizeof(a), &a, winini))
			return FALSE;
		w->dwFlags = a.dwFlags;
		/* the scheme name stays the system's; callers rarely read it */
		w->lpszDefaultScheme = NULL;
		return TRUE;
	}
	default:
		return SystemParametersInfoA(action, uparam, pv, winini);
	}
}

/* ---- DDE -------------------------------------------------------------- */

HSZ KXU(DdeCreateStringHandleW)(DWORD inst, LPCWSTR s, int cp)
{
	char *a = kxu_w2a(s);
	HSZ r = DdeCreateStringHandleA(inst, a, CP_WINANSI);
	(void) cp;
	kxu_free(a);
	return r;
}

DWORD KXU(DdeQueryStringW)(DWORD inst, HSZ hsz, LPWSTR buf, DWORD cch, int cp)
{
	DWORD n = DdeQueryStringA(inst, hsz, NULL, 0, CP_WINANSI);
	char *a;
	DWORD r = 0;
	(void) cp;
	if (!n)
		return 0;
	a = (char *) kxu_alloc(n + 1);
	if (a && DdeQueryStringA(inst, hsz, a, n + 1, CP_WINANSI))
	{
		if (!buf)
			r = (DWORD) kxu_a2w_len(CP_ACP, a, -1);
		else
			r = (DWORD) kxu_a2w(CP_ACP, a, -1, buf, (int) cch, NULL);
	}
	kxu_free(a);
	return r;
}

UINT KXU(DdeInitializeW)(LPDWORD inst, PFNCALLBACK cb, DWORD cmd, DWORD res)
{
	/* string handles are code-page neutral; callbacks get HSZs */
	return DdeInitializeA(inst, cb, cmd, res);
}

/* ---- wsprintf --------------------------------------------------------- */

static void put(LPWSTR out, int *pos, WCHAR c)
{
	if (*pos < 1024 - 1)
		out[*pos] = c;
	(*pos)++;
}

int KXU(wvsprintfW)(LPWSTR out, LPCWSTR fmt, va_list ap)
{
	int pos = 0;
	while (*fmt)
	{
		WCHAR digits[32];
		int left = 0, zero = 0, width = 0, prec = -1, wide = 1, nd = 0, neg = 0, i, len;
		LPCWSTR f;
		if (*fmt != L'%')
		{
			put(out, &pos, *fmt++);
			continue;
		}
		f = ++fmt;
		if (*fmt == L'-')
		{
			left = 1;
			fmt++;
		}
		if (*fmt == L'#')
			fmt++;
		if (*fmt == L'0')
		{
			zero = 1;
			fmt++;
		}
		while (*fmt >= L'0' && *fmt <= L'9')
			width = width * 10 + (*fmt++ - L'0');
		if (*fmt == L'.')
		{
			prec = 0;
			fmt++;
			while (*fmt >= L'0' && *fmt <= L'9')
				prec = prec * 10 + (*fmt++ - L'0');
		}
		if (*fmt == L'l' || *fmt == L'w')
		{
			wide = 1;
			fmt++;
		}
		else if (*fmt == L'h')
		{
			wide = 0;
			fmt++;
		}
		switch (*fmt)
		{
		case L'c':
		case L'C':
		{
			WCHAR c;
			if ((*fmt == L'c') == (wide != 0))
				c = (WCHAR) va_arg(ap, int);
			else
			{
				char b = (char) va_arg(ap, int);
				c = (BYTE) b;
				MultiByteToWideChar(CP_ACP, 0, &b, 1, &c, 1);
			}
			for (i = 1; !left && i < width; i++)
				put(out, &pos, L' ');
			put(out, &pos, c);
			for (i = 1; left && i < width; i++)
				put(out, &pos, L' ');
			fmt++;
			continue;
		}
		case L's':
		case L'S':
		{
			int asWide = (*fmt == L's') == (wide != 0);
			LPWSTR tmp = NULL;
			LPCWSTR s;
			if (asWide)
				s = va_arg(ap, LPCWSTR);
			else
			{
				LPCSTR a = va_arg(ap, LPCSTR);
				s = tmp = kxu_a2w_new(CP_ACP, a ? a : "(null)");
			}
			if (!s)
				s = L"(null)";
			len = lstrlenW(s);
			if (prec >= 0 && len > prec)
				len = prec;
			for (i = len; !left && i < width; i++)
				put(out, &pos, L' ');
			for (i = 0; i < len; i++)
				put(out, &pos, s[i]);
			for (i = len; left && i < width; i++)
				put(out, &pos, L' ');
			kxu_free(tmp);
			fmt++;
			continue;
		}
		case L'd':
		case L'i':
		case L'u':
		case L'x':
		case L'X':
		case L'p':
		{
			unsigned long v;
			unsigned base = (*fmt == L'x' || *fmt == L'X' || *fmt == L'p') ? 16 : 10;
			const WCHAR *dig = *fmt == L'X' || *fmt == L'p' ? L"0123456789ABCDEF" : L"0123456789abcdef";
			if (*fmt == L'd' || *fmt == L'i')
			{
				long sv = va_arg(ap, long);
				neg = sv < 0;
				v = neg ? (unsigned long) -sv : (unsigned long) sv;
			}
			else
				v = va_arg(ap, unsigned long);
			if (*fmt == L'p')
			{
				width = 8;
				zero = 1;
			}
			do
			{
				digits[nd++] = dig[v % base];
				v /= base;
			} while (v);
			while (prec > nd)
				digits[nd++] = L'0';
			len = nd + neg;
			if (!left && !zero)
				for (i = len; i < width; i++)
					put(out, &pos, L' ');
			if (neg)
				put(out, &pos, L'-');
			if (!left && zero)
				for (i = len; i < width; i++)
					put(out, &pos, L'0');
			while (nd)
				put(out, &pos, digits[--nd]);
			for (i = len; left && i < width; i++)
				put(out, &pos, L' ');
			fmt++;
			continue;
		}
		case 0:
			break;
		default:
			/* unknown: print as written, like wsprintf */
			put(out, &pos, L'%');
			fmt = f;
			continue;
		}
		break;
	}
	if (pos > 1024 - 1)
		pos = 1024 - 1;
	out[pos] = 0;
	return pos;
}

int WINAPIV kxu_wsprintfW(LPWSTR out, LPCWSTR fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = kxu_wvsprintfW(out, fmt, ap);
	va_end(ap);
	return r;
}
