/*
 *  KernelExEx - kexuni.dll: COMDLG32 wide functions. GPLv2.
 */
#include "uni.h"
#include <commdlg.h>

/* ---- DEVMODE / DEVNAMES in global memory ------------------------------ */

static HGLOBAL gdevmode_w2a(HGLOBAL hw)
{
	DEVMODEW *w;
	DEVMODEA *a;
	HGLOBAL ha = NULL;
	if (!hw || !(w = (DEVMODEW *) GlobalLock(hw)))
		return NULL;
	a = kxu_devmode_w2a(w);
	GlobalUnlock(hw);
	if (a)
	{
		ha = GlobalAlloc(GMEM_MOVEABLE, a->dmSize + a->dmDriverExtra);
		if (ha)
		{
			CopyMemory(GlobalLock(ha), a, a->dmSize + a->dmDriverExtra);
			GlobalUnlock(ha);
		}
		kxu_free(a);
	}
	return ha;
}

static HGLOBAL gdevmode_a2w(HGLOBAL ha)
{
	DEVMODEA *a;
	HGLOBAL hw = NULL;
	DWORD size;
	if (!ha || !(a = (DEVMODEA *) GlobalLock(ha)))
		return NULL;
	size = a->dmSize + 64 + a->dmDriverExtra;
	hw = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
	if (hw)
	{
		kxu_devmode_a2w(a, (DEVMODEW *) GlobalLock(hw), size);
		GlobalUnlock(hw);
	}
	GlobalUnlock(ha);
	return hw;
}

static HGLOBAL gdevnames_convert(HGLOBAL h, BOOL to_wide)
{
	HGLOBAL out = NULL;
	DEVNAMES *in;
	if (!h || !(in = (DEVNAMES *) GlobalLock(h)))
		return NULL;
	if (to_wide)
	{
		const char *base = (const char *) in;
		LPWSTR s[3];
		int i, len = 0;
		s[0] = kxu_a2w_new(CP_ACP, base + in->wDriverOffset);
		s[1] = kxu_a2w_new(CP_ACP, base + in->wDeviceOffset);
		s[2] = kxu_a2w_new(CP_ACP, base + in->wOutputOffset);
		for (i = 0; i < 3; i++)
			len += (s[i] ? lstrlenW(s[i]) : 0) + 1;
		out = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DEVNAMES) + len * sizeof(WCHAR));
		if (out)
		{
			DEVNAMES *d = (DEVNAMES *) GlobalLock(out);
			LPWSTR p = (LPWSTR) (d + 1);
			WORD *offs[3];
			offs[0] = &d->wDriverOffset;
			offs[1] = &d->wDeviceOffset;
			offs[2] = &d->wOutputOffset;
			for (i = 0; i < 3; i++)
			{
				*offs[i] = (WORD) (p - (LPWSTR) d);
				if (s[i])
					lstrcpyW(p, s[i]);
				p += (s[i] ? lstrlenW(s[i]) : 0) + 1;
			}
			d->wDefault = in->wDefault;
			GlobalUnlock(out);
		}
		for (i = 0; i < 3; i++)
			kxu_free(s[i]);
	}
	else
	{
		const WCHAR *base = (const WCHAR *) in;
		char *s[3];
		int i, len = 0;
		s[0] = kxu_w2a(base + in->wDriverOffset);
		s[1] = kxu_w2a(base + in->wDeviceOffset);
		s[2] = kxu_w2a(base + in->wOutputOffset);
		for (i = 0; i < 3; i++)
			len += (s[i] ? lstrlenA(s[i]) : 0) + 1;
		out = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DEVNAMES) + len);
		if (out)
		{
			DEVNAMES *d = (DEVNAMES *) GlobalLock(out);
			char *p = (char *) (d + 1);
			WORD *offs[3];
			offs[0] = &d->wDriverOffset;
			offs[1] = &d->wDeviceOffset;
			offs[2] = &d->wOutputOffset;
			for (i = 0; i < 3; i++)
			{
				*offs[i] = (WORD) (p - (char *) d);
				if (s[i])
					lstrcpyA(p, s[i]);
				p += (s[i] ? lstrlenA(s[i]) : 0) + 1;
			}
			d->wDefault = in->wDefault;
			GlobalUnlock(out);
		}
		for (i = 0; i < 3; i++)
			kxu_free(s[i]);
	}
	GlobalUnlock(h);
	return out;
}

/* replace the caller's W handles with converted results */
static void return_handles(HGLOBAL *wdm, HGLOBAL *wdn, HGLOBAL adm, HGLOBAL adn)
{
	if (adm)
	{
		HGLOBAL n = gdevmode_a2w(adm);
		if (n)
		{
			if (*wdm)
				GlobalFree(*wdm);
			*wdm = n;
		}
		GlobalFree(adm);
	}
	if (adn)
	{
		HGLOBAL n = gdevnames_convert(adn, TRUE);
		if (n)
		{
			if (*wdn)
				GlobalFree(*wdn);
			*wdn = n;
		}
		GlobalFree(adn);
	}
}

BOOL KXU(PrintDlgW)(LPPRINTDLGW w)
{
	PRINTDLGA a;
	char *t1 = NULL, *t2 = NULL;
	BOOL r;
	CopyMemory(&a, w, sizeof(a));
	a.lStructSize = sizeof(a);
	a.hDevMode = gdevmode_w2a(w->hDevMode);
	a.hDevNames = gdevnames_convert(w->hDevNames, FALSE);
	if (w->Flags & PD_ENABLEPRINTTEMPLATE)
		a.lpPrintTemplateName = t1 = kxu_res(w->lpPrintTemplateName);
	if (w->Flags & PD_ENABLESETUPTEMPLATE)
		a.lpSetupTemplateName = t2 = kxu_res(w->lpSetupTemplateName);
	r = PrintDlgA(&a);
	w->hDC = a.hDC;
	w->Flags = a.Flags;
	w->nFromPage = a.nFromPage;
	w->nToPage = a.nToPage;
	w->nCopies = a.nCopies;
	return_handles(&w->hDevMode, &w->hDevNames, a.hDevMode, a.hDevNames);
	if (t1)
		kxu_res_free(t1);
	if (t2)
		kxu_res_free(t2);
	return r;
}

BOOL KXU(PageSetupDlgW)(LPPAGESETUPDLGW w)
{
	PAGESETUPDLGA a;
	char *t = NULL;
	BOOL r;
	CopyMemory(&a, w, sizeof(a));
	a.lStructSize = sizeof(a);
	a.hDevMode = gdevmode_w2a(w->hDevMode);
	a.hDevNames = gdevnames_convert(w->hDevNames, FALSE);
	if (w->Flags & PSD_ENABLEPAGESETUPTEMPLATE)
		a.lpPageSetupTemplateName = t = kxu_res(w->lpPageSetupTemplateName);
	r = PageSetupDlgA(&a);
	w->Flags = a.Flags;
	w->ptPaperSize = a.ptPaperSize;
	w->rtMinMargin = a.rtMinMargin;
	w->rtMargin = a.rtMargin;
	return_handles(&w->hDevMode, &w->hDevNames, a.hDevMode, a.hDevNames);
	if (t)
		kxu_res_free(t);
	return r;
}

/* ---- colour and font ---------------------------------------------------- */

BOOL KXU(ChooseColorW)(LPCHOOSECOLORW w)
{
	CHOOSECOLORA a;
	char *t = NULL;
	BOOL r;
	CopyMemory(&a, w, sizeof(a));
	a.lStructSize = sizeof(a);
	if (w->Flags & CC_ENABLETEMPLATE)
		a.lpTemplateName = t = kxu_res(w->lpTemplateName);
	r = ChooseColorA(&a);
	w->rgbResult = a.rgbResult;
	w->Flags = a.Flags;
	if (t)
		kxu_res_free(t);
	return r;
}

BOOL KXU(ChooseFontW)(LPCHOOSEFONTW w)
{
	CHOOSEFONTA a;
	LOGFONTA lf;
	char style[LF_FACESIZE * 2], *t = NULL;
	BOOL r;
	CopyMemory(&a, w, sizeof(a));
	a.lStructSize = sizeof(a);
	if (w->lpLogFont)
		kxu_logfont_w2a(w->lpLogFont, &lf);
	else
		ZeroMemory(&lf, sizeof(lf));
	a.lpLogFont = &lf;
	style[0] = 0;
	if ((w->Flags & CF_USESTYLE) && w->lpszStyle)
		WideCharToMultiByte(CP_ACP, 0, w->lpszStyle, -1, style, sizeof(style), NULL, NULL);
	a.lpszStyle = w->lpszStyle ? style : NULL;
	if (w->Flags & CF_ENABLETEMPLATE)
		a.lpTemplateName = t = kxu_res(w->lpTemplateName);
	r = ChooseFontA(&a);
	if (r)
	{
		if (w->lpLogFont)
			kxu_logfont_a2w(&lf, w->lpLogFont);
		if (w->lpszStyle)
			kxu_a2w(CP_ACP, style, -1, w->lpszStyle, LF_FACESIZE, NULL);
	}
	w->iPointSize = a.iPointSize;
	w->Flags = a.Flags;
	w->rgbColors = a.rgbColors;
	w->nFontType = a.nFontType;
	if (t)
		kxu_res_free(t);
	return r;
}

short KXU(GetFileTitleW)(LPCWSTR file, LPWSTR buf, WORD cch)
{
	char *a = kxu_w2a(file), out[MAX_PATH * 2];
	short r = GetFileTitleA(a, out, sizeof(out));
	kxu_free(a);
	if (r == 0)
	{
		int need;
		kxu_a2w(CP_ACP, out, -1, buf, cch, &need);
		if (need >= cch)
			return (short) (need + 1);
	}
	else if (r > 0)
		r = (short) (r + 0);   /* bytes needed: an upper bound on characters */
	return r;
}

/* ---- Find and Replace (modeless) -------------------------------------- *
 * The dialog sends the owner FINDMSGSTRING with a pointer to the A
 * structure. The owner window is subclassed so it receives the caller's W
 * structure instead, with the strings copied back first. */

typedef struct FRCTX
{
	FINDREPLACEA a;
	LPFINDREPLACEW w;
	HWND dlg;
	struct FRCTX *next;
} FRCTX;

static FRCTX *fr_list;
static UINT fr_msg;
static const char fr_prop[] = "kexuni.fr";

static void fr_copy_back(FRCTX *c)
{
	c->w->Flags = c->a.Flags;
	if (c->w->lpstrFindWhat && c->w->wFindWhatLen)
		kxu_a2w(CP_ACP, c->a.lpstrFindWhat, -1, c->w->lpstrFindWhat, c->w->wFindWhatLen, NULL);
	if (c->w->lpstrReplaceWith && c->w->wReplaceWithLen && c->a.lpstrReplaceWith)
		kxu_a2w(CP_ACP, c->a.lpstrReplaceWith, -1, c->w->lpstrReplaceWith, c->w->wReplaceWithLen, NULL);
}

static LRESULT CALLBACK fr_owner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WNDPROC old = (WNDPROC) GetPropA(hwnd, fr_prop);
	if (msg == fr_msg && fr_msg)
	{
		FRCTX *c;
		for (c = fr_list; c; c = c->next)
			if ((LPARAM) &c->a == lp)
			{
				LRESULT r;
				fr_copy_back(c);
				r = CallWindowProcA(old, hwnd, msg, wp, (LPARAM) c->w);
				if (c->a.Flags & FR_DIALOGTERM)
				{
					FRCTX **pp;
					for (pp = &fr_list; *pp; pp = &(*pp)->next)
						if (*pp == c)
						{
							*pp = c->next;
							break;
						}
					kxu_free(c->a.lpstrFindWhat);
					kxu_free(c->a.lpstrReplaceWith);
					kxu_free(c);
				}
				return r;
			}
	}
	if (msg == WM_NCDESTROY && old)
	{
		SetWindowLongA(hwnd, GWL_WNDPROC, (LONG) old);
		RemovePropA(hwnd, fr_prop);
	}
	return CallWindowProcA(old ? old : DefWindowProcA, hwnd, msg, wp, lp);
}

static HWND fr_start(LPFINDREPLACEW w, BOOL replace)
{
	FRCTX *c = (FRCTX *) kxu_alloc(sizeof(*c));
	HWND dlg;
	if (!c)
		return NULL;
	if (!fr_msg)
		fr_msg = RegisterWindowMessageA(FINDMSGSTRINGA);
	CopyMemory(&c->a, w, sizeof(c->a));
	c->a.lStructSize = sizeof(c->a);
	c->w = w;
	c->a.wFindWhatLen = (WORD) (w->wFindWhatLen * 2 > 0xFFFF ? 0xFFFF : w->wFindWhatLen * 2);
	c->a.lpstrFindWhat = (LPSTR) kxu_alloc(c->a.wFindWhatLen + 1);
	if (c->a.lpstrFindWhat)
		WideCharToMultiByte(CP_ACP, 0, w->lpstrFindWhat, -1, c->a.lpstrFindWhat, c->a.wFindWhatLen, NULL, NULL);
	c->a.lpstrReplaceWith = NULL;
	if (replace)
	{
		c->a.wReplaceWithLen = (WORD) (w->wReplaceWithLen * 2 > 0xFFFF ? 0xFFFF : w->wReplaceWithLen * 2);
		c->a.lpstrReplaceWith = (LPSTR) kxu_alloc(c->a.wReplaceWithLen + 1);
		if (c->a.lpstrReplaceWith)
			WideCharToMultiByte(CP_ACP, 0, w->lpstrReplaceWith, -1, c->a.lpstrReplaceWith, c->a.wReplaceWithLen,
			                    NULL, NULL);
	}
	if (w->Flags & FR_ENABLETEMPLATE)
		c->a.lpTemplateName = NULL;   /* named templates: not converted (rare) */
	if (!GetPropA(w->hwndOwner, fr_prop))
		SetPropA(w->hwndOwner, fr_prop, (HANDLE) SetWindowLongA(w->hwndOwner, GWL_WNDPROC, (LONG) fr_owner_proc));
	c->next = fr_list;
	fr_list = c;
	dlg = replace ? ReplaceTextA(&c->a) : FindTextA(&c->a);
	if (!dlg)
	{
		fr_list = c->next;
		kxu_free(c->a.lpstrFindWhat);
		kxu_free(c->a.lpstrReplaceWith);
		kxu_free(c);
	}
	return dlg;
}

HWND KXU(FindTextW)(LPFINDREPLACEW w)
{
	return fr_start(w, FALSE);
}

HWND KXU(ReplaceTextW)(LPFINDREPLACEW w)
{
	return fr_start(w, TRUE);
}
