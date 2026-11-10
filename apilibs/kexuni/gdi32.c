/*
 *  KernelExEx - kexuni.dll: GDI32 wide functions. GPLv2.
 */
#include "uni.h"

/* ---- device contexts and documents ------------------------------------ */

HDC KXU(CreateDCW)(LPCWSTR drv, LPCWSTR dev, LPCWSTR port, const DEVMODEW *dm)
{
	char *a = kxu_w2a(drv), *b = kxu_w2a(dev), *c = kxu_w2a(port);
	DEVMODEA *d = kxu_devmode_w2a(dm);
	HDC r = CreateDCA(a, b, c, d);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	kxu_free(d);
	return r;
}

HDC KXU(CreateICW)(LPCWSTR drv, LPCWSTR dev, LPCWSTR port, const DEVMODEW *dm)
{
	char *a = kxu_w2a(drv), *b = kxu_w2a(dev), *c = kxu_w2a(port);
	DEVMODEA *d = kxu_devmode_w2a(dm);
	HDC r = CreateICA(a, b, c, d);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	kxu_free(d);
	return r;
}

HDC KXU(ResetDCW)(HDC hdc, const DEVMODEW *dm)
{
	DEVMODEA *d = kxu_devmode_w2a(dm);
	HDC r = ResetDCA(hdc, d);
	kxu_free(d);
	return r;
}

int KXU(StartDocW)(HDC hdc, const DOCINFOW *di)
{
	DOCINFOA a;
	char *n, *o, *t;
	int r;
	ZeroMemory(&a, sizeof(a));
	a.cbSize = sizeof(a);
	a.lpszDocName = n = kxu_w2a(di->lpszDocName);
	a.lpszOutput = o = kxu_w2a(di->lpszOutput);
	if (di->cbSize >= sizeof(DOCINFOW))
	{
		a.lpszDatatype = t = kxu_w2a(di->lpszDatatype);
		a.fwType = di->fwType;
	}
	else
		t = NULL;
	r = StartDocA(hdc, &a);
	kxu_free(n);
	kxu_free(o);
	kxu_free(t);
	return r;
}

/* ---- fonts ------------------------------------------------------------ */

HFONT KXU(CreateFontIndirectW)(const LOGFONTW *lf)
{
	LOGFONTA a;
	if (!lf)
		return NULL;
	kxu_logfont_w2a(lf, &a);
	return CreateFontIndirectA(&a);
}

HFONT KXU(CreateFontW)(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD under, DWORD strike,
                       DWORD cs, DWORD prec, DWORD clip, DWORD quality, DWORD pitch, LPCWSTR face)
{
	char *a = kxu_w2a(face);
	HFONT r = CreateFontA(h, w, esc, orient, weight, italic, under, strike, cs, prec, clip, quality, pitch, a);
	kxu_free(a);
	return r;
}

BOOL KXU(CreateScalableFontResourceW)(DWORD hidden, LPCWSTR res, LPCWSTR file, LPCWSTR path)
{
	char *a = kxu_w2a(res), *b = kxu_w2a(file), *c = kxu_w2a(path);
	BOOL r = CreateScalableFontResourceA(hidden, a, b, c);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	return r;
}

BOOL KXU(GetTextMetricsW)(HDC hdc, LPTEXTMETRICW tm)
{
	TEXTMETRICA a;
	if (!GetTextMetricsA(hdc, &a))
		return FALSE;
	kxu_textmetric_a2w(&a, tm);
	return TRUE;
}

int KXU(GetTextFaceW)(HDC hdc, int cch, LPWSTR buf)
{
	char a[LF_FACESIZE * 2];
	if (!GetTextFaceA(hdc, sizeof(a), a))
		return 0;
	if (!buf)
		return kxu_a2w_len(CP_ACP, a, -1) + 1;
	return kxu_a2w(CP_ACP, a, -1, buf, cch, NULL) + 1;
}

UINT KXU(GetOutlineTextMetricsW)(HDC hdc, UINT size, LPOUTLINETEXTMETRICW out)
{
	UINT asz = GetOutlineTextMetricsA(hdc, 0, NULL), need, pos;
	OUTLINETEXTMETRICA *a;
	OUTLINETEXTMETRICW w;
	const char *names[4];
	int i, len[4];
	PSTR *offs[4];
	if (!asz)
		return 0;
	a = (OUTLINETEXTMETRICA *) kxu_alloc(asz);
	if (!a || !GetOutlineTextMetricsA(hdc, asz, a))
	{
		kxu_free(a);
		return 0;
	}
	ZeroMemory(&w, sizeof(w));
	kxu_textmetric_a2w(&a->otmTextMetrics, &w.otmTextMetrics);
	/* every field after the text metrics has the same type in both */
	CopyMemory(&w.otmFiller, &a->otmFiller, (const char *) &a->otmpFamilyName - (const char *) &a->otmFiller);
	offs[0] = &a->otmpFamilyName;
	offs[1] = &a->otmpFaceName;
	offs[2] = &a->otmpStyleName;
	offs[3] = &a->otmpFullName;
	need = sizeof(w);
	for (i = 0; i < 4; i++)
	{
		names[i] = *offs[i] ? (const char *) a + (ULONG_PTR) *offs[i] : "";
		len[i] = kxu_a2w_len(CP_ACP, names[i], -1) + 1;
		need += len[i] * sizeof(WCHAR);
	}
	if (!out)
	{
		kxu_free(a);
		return need;
	}
	if (size < sizeof(w))
	{
		kxu_free(a);
		return 0;
	}
	pos = sizeof(w);
	{
		PSTR *wofs[4];
		wofs[0] = &w.otmpFamilyName;
		wofs[1] = &w.otmpFaceName;
		wofs[2] = &w.otmpStyleName;
		wofs[3] = &w.otmpFullName;
		for (i = 0; i < 4; i++)
		{
			if (pos + len[i] * sizeof(WCHAR) <= size)
			{
				kxu_a2w(CP_ACP, names[i], -1, (LPWSTR) ((char *) out + pos), len[i], NULL);
				*wofs[i] = (PSTR) (ULONG_PTR) pos;
				pos += len[i] * sizeof(WCHAR);
			}
			else
				*wofs[i] = NULL;
		}
	}
	w.otmSize = need;
	CopyMemory(out, &w, sizeof(w));
	kxu_free(a);
	return need <= size ? need : pos;
}

DWORD KXU(GetKerningPairsW)(HDC hdc, DWORD n, LPKERNINGPAIR pairs)
{
	DWORD r = GetKerningPairsA(hdc, n, pairs), i;
	if (pairs)
		for (i = 0; i < r; i++)
		{
			char c[2];
			WCHAR w[2] = { 0, 0 };
			c[0] = (char) pairs[i].wFirst;
			c[1] = (char) pairs[i].wSecond;
			MultiByteToWideChar(CP_ACP, 0, c, 1, &w[0], 1);
			MultiByteToWideChar(CP_ACP, 0, c + 1, 1, &w[1], 1);
			pairs[i].wFirst = w[0] ? w[0] : (BYTE) c[0];
			pairs[i].wSecond = w[1] ? w[1] : (BYTE) c[1];
		}
	return r;
}

/* byte index where each wide character starts in its ANSI form */
static int *char_starts(const char *a, int alen, int nchars)
{
	int *idx = (int *) kxu_alloc((nchars + 1) * sizeof(int)), b = 0, c = 0;
	if (!idx)
		return NULL;
	while (c < nchars && b < alen)
	{
		idx[c++] = b;
		b += IsDBCSLeadByteEx(CP_ACP, (BYTE) a[b]) && b + 1 < alen ? 2 : 1;
	}
	while (c <= nchars)
		idx[c++] = b;
	return idx;
}

BOOL KXU(GetTextExtentExPointW)(HDC hdc, LPCWSTR s, int cch, int maxext, LPINT fit, LPINT dx, LPSIZE size)
{
	int alen, afit = 0, *adx = NULL, *starts, i;
	char *a = kxu_w2a_n(CP_ACP, s, cch, &alen);
	BOOL r;
	if (!a)
		return FALSE;
	if (dx)
		adx = (int *) kxu_alloc((alen + 1) * sizeof(int));
	r = GetTextExtentExPointA(hdc, a, alen, maxext, fit ? &afit : NULL, adx, size);
	if (r)
	{
		starts = char_starts(a, alen, cch);
		if (fit)
			*fit = kxu_wchars_in(CP_ACP, a, afit);
		if (dx && adx && starts)
		{
			int n = fit ? *fit : cch;
			for (i = 0; i < n; i++)
				dx[i] = adx[starts[i + 1] - 1];   /* extent through the character's last byte */
		}
		kxu_free(starts);
	}
	kxu_free(adx);
	kxu_free(a);
	return r;
}

DWORD KXU(GetCharacterPlacementW)(HDC hdc, LPCWSTR s, int cch, int maxext, LPGCP_RESULTSW res, DWORD flags)
{
	int alen, i, *starts;
	char *a = kxu_w2a_n(CP_ACP, s, cch, &alen);
	GCP_RESULTSA ra;
	DWORD r;
	UINT *aorder = NULL;
	int *adx = NULL, *acaret = NULL;
	char *aclass = NULL;
	if (!a)
		return 0;
	ZeroMemory(&ra, sizeof(ra));
	ra.lStructSize = sizeof(ra);
	ra.nGlyphs = (UINT) alen;
	if (res->lpOrder)
		ra.lpOrder = aorder = (UINT *) kxu_alloc(alen * sizeof(UINT) + 4);
	if (res->lpDx)
		ra.lpDx = adx = (int *) kxu_alloc(alen * sizeof(int) + 4);
	if (res->lpCaretPos)
		ra.lpCaretPos = acaret = (int *) kxu_alloc(alen * sizeof(int) + 4);
	if (res->lpClass)
		ra.lpClass = aclass = (char *) kxu_alloc(alen + 4);
	/* glyph indices are per output glyph, which for the code pages 9x
	 * supports means per ANSI character; we report them per wide one */
	if (res->lpGlyphs)
		ra.lpGlyphs = (LPWSTR) kxu_alloc(alen * sizeof(WCHAR) + 4);
	r = GetCharacterPlacementA(hdc, a, alen, maxext, &ra, flags);
	starts = char_starts(a, alen, cch);
	if (r && starts)
	{
		UINT n = (UINT) cch < res->nGlyphs ? (UINT) cch : res->nGlyphs;
		for (i = 0; i < (int) n; i++)
		{
			int b0 = starts[i], b1 = starts[i + 1], b;
			if (res->lpDx && adx)
			{
				int sum = 0;
				for (b = b0; b < b1; b++)
					sum += adx[b];
				res->lpDx[i] = sum;
			}
			if (res->lpCaretPos && acaret)
				res->lpCaretPos[i] = acaret[b0];
			if (res->lpClass && aclass)
				res->lpClass[i] = aclass[b0];
			if (res->lpOrder && aorder)
				res->lpOrder[i] = (UINT) kxu_wchars_in(CP_ACP, a, (int) aorder[b0]);
			if (res->lpGlyphs && ra.lpGlyphs)
				res->lpGlyphs[i] = ((LPWSTR) ra.lpGlyphs)[ra.nGlyphs == (UINT) alen ? b0 : (i < (int) ra.nGlyphs ? i : 0)];
		}
		res->nGlyphs = n;
		res->nMaxFit = kxu_wchars_in(CP_ACP, a, ra.nMaxFit);
		if (res->lpOutString)
			kxu_a2w(CP_ACP, a, alen, res->lpOutString, cch + 1, NULL);
	}
	kxu_free(starts);
	kxu_free(aorder);
	kxu_free(adx);
	kxu_free(acaret);
	kxu_free(aclass);
	kxu_free(ra.lpGlyphs);
	kxu_free(a);
	return r;
}

/* ---- metafiles -------------------------------------------------------- */

HENHMETAFILE KXU(CopyEnhMetaFileW)(HENHMETAFILE src, LPCWSTR file)
{
	char *a = kxu_w2a(file);
	HENHMETAFILE r = CopyEnhMetaFileA(src, a);
	kxu_free(a);
	return r;
}

HMETAFILE KXU(CopyMetaFileW)(HMETAFILE src, LPCWSTR file)
{
	char *a = kxu_w2a(file);
	HMETAFILE r = CopyMetaFileA(src, a);
	kxu_free(a);
	return r;
}

HDC KXU(CreateEnhMetaFileW)(HDC ref, LPCWSTR file, const RECT *rc, LPCWSTR desc)
{
	char *a = kxu_w2a(file), *d = kxu_w2a_multi(desc);
	HDC r = CreateEnhMetaFileA(ref, a, rc, d);
	kxu_free(a);
	kxu_free(d);
	return r;
}

HDC KXU(CreateMetaFileW)(LPCWSTR file)
{
	char *a = kxu_w2a(file);
	HDC r = CreateMetaFileA(a);
	kxu_free(a);
	return r;
}

HENHMETAFILE KXU(GetEnhMetaFileW)(LPCWSTR file)
{
	char *a = kxu_w2a(file);
	HENHMETAFILE r = GetEnhMetaFileA(a);
	kxu_free(a);
	return r;
}

HMETAFILE KXU(GetMetaFileW)(LPCWSTR file)
{
	char *a = kxu_w2a(file);
	HMETAFILE r = GetMetaFileA(a);
	kxu_free(a);
	return r;
}

UINT KXU(GetEnhMetaFileDescriptionW)(HENHMETAFILE emf, UINT cch, LPWSTR buf)
{
	UINT n = GetEnhMetaFileDescriptionA(emf, 0, NULL), need;
	char *a;
	if (n == 0 || n == GDI_ERROR)
		return n;
	a = (char *) kxu_alloc(n + 2);
	if (!a)
		return GDI_ERROR;
	n = GetEnhMetaFileDescriptionA(emf, n, a);
	need = (UINT) kxu_a2w_len(CP_ACP, a, (int) n);
	if (buf)
	{
		if (need > cch)
			need = GDI_ERROR;
		else if (need)
			MultiByteToWideChar(CP_ACP, 0, a, (int) n, buf, (int) cch);
	}
	kxu_free(a);
	return need;
}

/* ---- colour management ------------------------------------------------ */

static void lcs_w2a(const LOGCOLORSPACEW *w, LOGCOLORSPACEA *a)
{
	CopyMemory(a, w, FIELD_OFFSET(LOGCOLORSPACEA, lcsFilename));
	a->lcsSize = sizeof(*a);
	a->lcsFilename[0] = 0;
	WideCharToMultiByte(CP_ACP, 0, w->lcsFilename, -1, a->lcsFilename, MAX_PATH, NULL, NULL);
	a->lcsFilename[MAX_PATH - 1] = 0;
}

HCOLORSPACE KXU(CreateColorSpaceW)(LPLOGCOLORSPACEW lcs)
{
	LOGCOLORSPACEA a;
	lcs_w2a(lcs, &a);
	return CreateColorSpaceA(&a);
}

BOOL KXU(GetLogColorSpaceW)(HCOLORSPACE cs, LPLOGCOLORSPACEW lcs, DWORD size)
{
	LOGCOLORSPACEA a;
	if (size < sizeof(*lcs) || !GetLogColorSpaceA(cs, &a, sizeof(a)))
		return FALSE;
	CopyMemory(lcs, &a, FIELD_OFFSET(LOGCOLORSPACEA, lcsFilename));
	lcs->lcsSize = sizeof(*lcs);
	kxu_a2w(CP_ACP, a.lcsFilename, -1, lcs->lcsFilename, MAX_PATH, NULL);
	return TRUE;
}

BOOL KXU(GetICMProfileW)(HDC hdc, LPDWORD cch, LPWSTR buf)
{
	char a[MAX_PATH * 2];
	DWORD n = sizeof(a);
	int need;
	if (!GetICMProfileA(hdc, &n, a))
		return FALSE;
	need = kxu_a2w_len(CP_ACP, a, -1) + 1;
	if (!buf || (DWORD) need > *cch)
	{
		*cch = (DWORD) need;
		if (buf)
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return buf ? FALSE : TRUE;
	}
	kxu_a2w(CP_ACP, a, -1, buf, (int) *cch, NULL);
	*cch = (DWORD) need;
	return TRUE;
}

BOOL KXU(SetICMProfileW)(HDC hdc, LPWSTR file)
{
	char *a = kxu_w2a(file);
	BOOL r = SetICMProfileA(hdc, a);
	kxu_free(a);
	return r;
}

typedef struct
{
	ICMENUMPROCW cb;
	LPARAM lp;
} ICMCTX;

static int CALLBACK icm_cb(LPSTR file, LPARAM lp)
{
	ICMCTX *c = (ICMCTX *) lp;
	LPWSTR w = kxu_a2w_new(CP_ACP, file);
	int r = w ? c->cb(w, c->lp) : 0;
	kxu_free(w);
	return r;
}

int KXU(EnumICMProfilesW)(HDC hdc, ICMENUMPROCW cb, LPARAM lp)
{
	ICMCTX c;
	c.cb = cb;
	c.lp = lp;
	return EnumICMProfilesA(hdc, icm_cb, (LPARAM) &c);
}

BOOL KXU(UpdateICMRegKeyW)(DWORD reserved, LPWSTR cmid, LPWSTR file, UINT cmd)
{
	char *a = kxu_w2a(cmid), *b = kxu_w2a(file);
	BOOL r = UpdateICMRegKeyA(reserved, a, b, cmd);
	kxu_free(a);
	kxu_free(b);
	return r;
}
