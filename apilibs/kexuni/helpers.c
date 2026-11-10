/*
 *  KernelExEx - kexuni.dll helpers. GPLv2.
 */
#include "uni.h"

void *kxu_alloc(size_t n)
{
	return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}

void kxu_free(void *p)
{
	if (p)
		HeapFree(GetProcessHeap(), 0, p);
}

char *kxu_w2a_n(UINT cp, LPCWSTR w, int cch, int *alen)
{
	int n;
	char *a;
	if (!w)
	{
		if (alen)
			*alen = 0;
		return NULL;
	}
	if (cch < 0)
		cch = lstrlenW(w);
	n = cch ? WideCharToMultiByte(cp, 0, w, cch, NULL, 0, NULL, NULL) : 0;
	a = (char *) kxu_alloc(n + 1);
	if (!a)
		return NULL;
	if (n)
		WideCharToMultiByte(cp, 0, w, cch, a, n, NULL, NULL);
	a[n] = 0;
	if (alen)
		*alen = n;
	return a;
}

char *kxu_w2a_cp(UINT cp, LPCWSTR w)
{
	return kxu_w2a_n(cp, w, -1, NULL);
}

char *kxu_w2a(LPCWSTR w)
{
	return kxu_w2a_n(CP_ACP, w, -1, NULL);
}

char *kxu_res(LPCWSTR w)
{
	if (IS_INTRES(w))
		return (char *) w;
	return kxu_w2a(w);
}

void kxu_res_free(char *a)
{
	if (!IS_INTRES(a))
		kxu_free(a);
}

size_t kxu_multi_len_w(LPCWSTR w)
{
	LPCWSTR p = w;
	if (!w)
		return 0;
	while (*p)
		p += lstrlenW(p) + 1;
	return (size_t) (p - w) + 1;
}

char *kxu_w2a_multi(LPCWSTR w)
{
	if (!w)
		return NULL;
	return kxu_w2a_n(CP_ACP, w, (int) kxu_multi_len_w(w), NULL);
}

int kxu_a2w_len(UINT cp, LPCSTR a, int alen)
{
	if (!a)
		return 0;
	if (alen < 0)
		alen = lstrlenA(a);
	return alen ? MultiByteToWideChar(cp, 0, a, alen, NULL, 0) : 0;
}

int kxu_a2w(UINT cp, LPCSTR a, int alen, LPWSTR w, int cchw, int *need)
{
	int n, got;
	if (!a)
		a = "";
	if (alen < 0)
		alen = lstrlenA(a);
	n = kxu_a2w_len(cp, a, alen);
	if (need)
		*need = n;
	if (cchw <= 0 || !w)
		return 0;
	if (n < cchw)
	{
		got = n ? MultiByteToWideChar(cp, 0, a, alen, w, n) : 0;
		w[got] = 0;
		return got;
	}
	/* truncate on a character boundary */
	{
		LPWSTR tmp = (LPWSTR) kxu_alloc((n + 1) * sizeof(WCHAR));
		if (!tmp)
		{
			w[0] = 0;
			return 0;
		}
		MultiByteToWideChar(cp, 0, a, alen, tmp, n);
		got = cchw - 1;
		CopyMemory(w, tmp, got * sizeof(WCHAR));
		w[got] = 0;
		kxu_free(tmp);
		return got;
	}
}

LPWSTR kxu_a2w_new(UINT cp, LPCSTR a)
{
	int n;
	LPWSTR w;
	if (!a)
		return NULL;
	n = kxu_a2w_len(cp, a, -1);
	w = (LPWSTR) kxu_alloc((n + 1) * sizeof(WCHAR));
	if (w)
		kxu_a2w(cp, a, -1, w, n + 1, NULL);
	return w;
}

int kxu_wchars_in(UINT cp, LPCSTR a, int nbytes)
{
	return nbytes > 0 ? MultiByteToWideChar(cp, 0, a, nbytes, NULL, 0) : 0;
}

int kxu_bytes_for(UINT cp, LPCSTR a, int alen, int nchars)
{
	int b = 0, c = 0;
	if (alen < 0)
		alen = lstrlenA(a);
	while (b < alen && c < nchars)
	{
		b += IsDBCSLeadByteEx(cp, (BYTE) a[b]) && b + 1 < alen ? 2 : 1;
		c++;
	}
	return b;
}

DWORD kxu_ret_buf(LPCSTR a, LPWSTR w, DWORD cch)
{
	int need;
	int got = kxu_a2w(CP_ACP, a, -1, w, (int) cch, &need);
	if ((DWORD) need >= cch)
		return need + 1;
	return got;
}

/* ---- structures ------------------------------------------------------- */

void kxu_logfont_w2a(const LOGFONTW *w, LOGFONTA *a)
{
	CopyMemory(a, w, FIELD_OFFSET(LOGFONTA, lfFaceName));
	a->lfFaceName[0] = 0;
	WideCharToMultiByte(CP_ACP, 0, w->lfFaceName, -1, a->lfFaceName, LF_FACESIZE, NULL, NULL);
	a->lfFaceName[LF_FACESIZE - 1] = 0;
}

void kxu_logfont_a2w(const LOGFONTA *a, LOGFONTW *w)
{
	CopyMemory(w, a, FIELD_OFFSET(LOGFONTW, lfFaceName));
	kxu_a2w(CP_ACP, a->lfFaceName, -1, w->lfFaceName, LF_FACESIZE, NULL);
}

static WCHAR byte_to_wchar(BYTE b)
{
	WCHAR c = b;
	MultiByteToWideChar(CP_ACP, 0, (const char *) &b, 1, &c, 1);
	return c;
}

void kxu_textmetric_a2w(const TEXTMETRICA *a, TEXTMETRICW *w)
{
	w->tmHeight = a->tmHeight;
	w->tmAscent = a->tmAscent;
	w->tmDescent = a->tmDescent;
	w->tmInternalLeading = a->tmInternalLeading;
	w->tmExternalLeading = a->tmExternalLeading;
	w->tmAveCharWidth = a->tmAveCharWidth;
	w->tmMaxCharWidth = a->tmMaxCharWidth;
	w->tmWeight = a->tmWeight;
	w->tmOverhang = a->tmOverhang;
	w->tmDigitizedAspectX = a->tmDigitizedAspectX;
	w->tmDigitizedAspectY = a->tmDigitizedAspectY;
	w->tmFirstChar = byte_to_wchar(a->tmFirstChar);
	w->tmLastChar = byte_to_wchar(a->tmLastChar);
	w->tmDefaultChar = byte_to_wchar(a->tmDefaultChar);
	w->tmBreakChar = byte_to_wchar(a->tmBreakChar);
	w->tmItalic = a->tmItalic;
	w->tmUnderlined = a->tmUnderlined;
	w->tmStruckOut = a->tmStruckOut;
	w->tmPitchAndFamily = a->tmPitchAndFamily;
	w->tmCharSet = a->tmCharSet;
	/* a symbol or OEM font has no meaningful Unicode range */
	if (a->tmLastChar == 0xFF && w->tmLastChar < 0x100)
		w->tmLastChar = 0xFFFF;
}

DEVMODEA *kxu_devmode_w2a(const DEVMODEW *w)
{
	DEVMODEA *a;
	DWORD fixed;
	if (!w)
		return NULL;
	/* the A structure is 64 bytes shorter: two 32-character names */
	fixed = w->dmSize > sizeof(DEVMODEW) ? sizeof(DEVMODEW) : w->dmSize;
	a = (DEVMODEA *) kxu_alloc(sizeof(DEVMODEA) + w->dmDriverExtra + 64);
	if (!a)
		return NULL;
	ZeroMemory(a, sizeof(DEVMODEA));
	WideCharToMultiByte(CP_ACP, 0, w->dmDeviceName, CCHDEVICENAME, (char *) a->dmDeviceName, CCHDEVICENAME, NULL,
	                    NULL);
	a->dmDeviceName[CCHDEVICENAME - 1] = 0;
	/* dmSpecVersion .. dmCollate */
	CopyMemory(&a->dmSpecVersion, &w->dmSpecVersion,
	           FIELD_OFFSET(DEVMODEW, dmFormName) - FIELD_OFFSET(DEVMODEW, dmSpecVersion));
	if (fixed > FIELD_OFFSET(DEVMODEW, dmFormName))
	{
		WideCharToMultiByte(CP_ACP, 0, w->dmFormName, CCHFORMNAME, (char *) a->dmFormName, CCHFORMNAME, NULL, NULL);
		a->dmFormName[CCHFORMNAME - 1] = 0;
	}
	if (fixed > FIELD_OFFSET(DEVMODEW, dmLogPixels))
		CopyMemory(&a->dmLogPixels, &w->dmLogPixels, fixed - FIELD_OFFSET(DEVMODEW, dmLogPixels));
	a->dmSize = (WORD) (fixed > FIELD_OFFSET(DEVMODEW, dmFormName) ? fixed - 64 : fixed - 32);
	a->dmDriverExtra = w->dmDriverExtra;
	CopyMemory((char *) a + a->dmSize, (const char *) w + w->dmSize, w->dmDriverExtra);
	return a;
}

void kxu_devmode_a2w(const DEVMODEA *a, DEVMODEW *w, DWORD wsize)
{
	DWORD fixed = a->dmSize > sizeof(DEVMODEA) ? sizeof(DEVMODEA) : a->dmSize;
	DWORD wfixed = fixed > FIELD_OFFSET(DEVMODEA, dmFormName) ? fixed + 64 : fixed + 32;
	if (wsize < wfixed + a->dmDriverExtra)
		return;
	ZeroMemory(w, wfixed);
	kxu_a2w(CP_ACP, (const char *) a->dmDeviceName, -1, w->dmDeviceName, CCHDEVICENAME, NULL);
	CopyMemory(&w->dmSpecVersion, &a->dmSpecVersion,
	           FIELD_OFFSET(DEVMODEA, dmFormName) - FIELD_OFFSET(DEVMODEA, dmSpecVersion));
	if (fixed > FIELD_OFFSET(DEVMODEA, dmFormName))
		kxu_a2w(CP_ACP, (const char *) a->dmFormName, -1, w->dmFormName, CCHFORMNAME, NULL);
	if (fixed > FIELD_OFFSET(DEVMODEA, dmLogPixels))
		CopyMemory(&w->dmLogPixels, &a->dmLogPixels, fixed - FIELD_OFFSET(DEVMODEA, dmLogPixels));
	w->dmSize = (WORD) wfixed;
	w->dmDriverExtra = a->dmDriverExtra;
	CopyMemory((char *) w + wfixed, (const char *) a + a->dmSize, a->dmDriverExtra);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void) reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(inst);
	return TRUE;
}
