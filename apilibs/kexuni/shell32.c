/*
 *  KernelExEx - kexuni.dll: SHELL32 wide functions. GPLv2.
 */
#include "uni.h"
#include <shellapi.h>
#include <shlobj.h>

HICON KXU(ExtractIconW)(HINSTANCE inst, LPCWSTR file, UINT idx)
{
	char *a = kxu_w2a(file);
	HICON r = ExtractIconA(inst, a, idx);
	kxu_free(a);
	return r;
}

UINT KXU(ExtractIconExW)(LPCWSTR file, int idx, HICON *large, HICON *small, UINT n)
{
	char *a = kxu_w2a(file);
	UINT r = ExtractIconExA(a, idx, large, small, n);
	kxu_free(a);
	return r;
}

HINSTANCE KXU(FindExecutableW)(LPCWSTR file, LPCWSTR dir, LPWSTR result)
{
	char *a = kxu_w2a(file), *b = kxu_w2a(dir), out[MAX_PATH * 2];
	HINSTANCE r;
	out[0] = 0;
	r = FindExecutableA(a, b, out);
	kxu_a2w(CP_ACP, out, -1, result, MAX_PATH, NULL);
	kxu_free(a);
	kxu_free(b);
	return r;
}

int KXU(ShellAboutW)(HWND hwnd, LPCWSTR app, LPCWSTR other, HICON icon)
{
	char *a = kxu_w2a(app), *b = kxu_w2a(other);
	int r = ShellAboutA(hwnd, a, b, icon);
	kxu_free(a);
	kxu_free(b);
	return r;
}

HINSTANCE KXU(ShellExecuteW)(HWND hwnd, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show)
{
	char *a = kxu_w2a(op), *b = kxu_w2a(file), *c = kxu_w2a(params), *d = kxu_w2a(dir);
	HINSTANCE r = ShellExecuteA(hwnd, a, b, c, d, show);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	kxu_free(d);
	return r;
}

BOOL KXU(ShellExecuteExW)(LPSHELLEXECUTEINFOW w)
{
	SHELLEXECUTEINFOA a;
	char *s[5];
	BOOL r;
	int i;
	CopyMemory(&a, w, sizeof(a));
	a.cbSize = sizeof(a);
	a.lpVerb = s[0] = kxu_w2a(w->lpVerb);
	a.lpFile = s[1] = kxu_w2a(w->lpFile);
	a.lpParameters = s[2] = kxu_w2a(w->lpParameters);
	a.lpDirectory = s[3] = kxu_w2a(w->lpDirectory);
	s[4] = NULL;
	if ((w->fMask & SEE_MASK_CLASSNAME) == SEE_MASK_CLASSNAME)
		a.lpClass = s[4] = kxu_w2a(w->lpClass);
	r = ShellExecuteExA(&a);
	w->hInstApp = a.hInstApp;
	w->hProcess = a.hProcess;
	for (i = 0; i < 5; i++)
		kxu_free(s[i]);
	return r;
}

DWORD_PTR KXU(SHGetFileInfoW)(LPCWSTR path, DWORD attr, SHFILEINFOW *sfi, UINT size, UINT flags)
{
	SHFILEINFOA a;
	char *p = (flags & SHGFI_PIDL) ? (char *) path : kxu_w2a(path);
	DWORD_PTR r;
	(void) size;
	ZeroMemory(&a, sizeof(a));
	r = SHGetFileInfoA(p, attr, sfi ? &a : NULL, sfi ? sizeof(a) : 0, flags);
	if (sfi)
	{
		sfi->hIcon = a.hIcon;
		sfi->iIcon = a.iIcon;
		sfi->dwAttributes = a.dwAttributes;
		kxu_a2w(CP_ACP, a.szDisplayName, -1, sfi->szDisplayName, MAX_PATH, NULL);
		kxu_a2w(CP_ACP, a.szTypeName, -1, sfi->szTypeName, 80, NULL);
	}
	if (!(flags & SHGFI_PIDL))
		kxu_free(p);
	return r;
}

BOOL KXU(SHGetNewLinkInfoW)(LPCWSTR target, LPCWSTR dir, LPWSTR name, BOOL *mustcopy, UINT flags)
{
	char *a = (flags & SHGNLI_PIDL) ? (char *) target : kxu_w2a(target), *b = kxu_w2a(dir), out[MAX_PATH * 2];
	BOOL r;
	/* only in newer shells: look it up */
	BOOL(WINAPI * fn)(LPCSTR, LPCSTR, LPSTR, BOOL *, UINT) =
		(BOOL(WINAPI *)(LPCSTR, LPCSTR, LPSTR, BOOL *, UINT)) GetProcAddress(GetModuleHandleA("shell32.dll"),
		                                                                       "SHGetNewLinkInfoA");
	out[0] = 0;
	if (!fn)
	{
		SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
		r = FALSE;
	}
	else
		r = fn(a, b, out, mustcopy, flags);
	if (r)
		kxu_a2w(CP_ACP, out, -1, name, MAX_PATH, NULL);
	if (!(flags & SHGNLI_PIDL))
		kxu_free(a);
	kxu_free(b);
	return r;
}

/* SHFileOperation: lists in, aborted flag and name mappings out */
int KXU(SHFileOperationW)(LPSHFILEOPSTRUCTW w)
{
	SHFILEOPSTRUCTA a;
	char *from = kxu_w2a_multi(w->pFrom), *to = kxu_w2a_multi(w->pTo), *title = NULL;
	int r;
	a.hwnd = w->hwnd;
	a.wFunc = w->wFunc;
	a.pFrom = from;
	a.pTo = to;
	a.fFlags = w->fFlags & ~FOF_WANTMAPPINGHANDLE;   /* the A mapping table is not convertible */
	a.fAnyOperationsAborted = FALSE;
	a.hNameMappings = NULL;
	a.lpszProgressTitle = NULL;
	if (w->fFlags & FOF_SIMPLEPROGRESS)
		a.lpszProgressTitle = title = kxu_w2a(w->lpszProgressTitle);
	r = SHFileOperationA(&a);
	w->fAnyOperationsAborted = a.fAnyOperationsAborted;
	w->hNameMappings = NULL;
	kxu_free(from);
	kxu_free(to);
	kxu_free(title);
	return r;
}

BOOL KXU(Shell_NotifyIconW)(DWORD msg, PNOTIFYICONDATAW w)
{
	/* the 98 SE / ME shell knows the first, 88-byte layout */
	NOTIFYICONDATAA a;
	ZeroMemory(&a, sizeof(a));
	a.cbSize = NOTIFYICONDATAA_V1_SIZE;
	a.hWnd = w->hWnd;
	a.uID = w->uID;
	a.uFlags = w->uFlags & (NIF_MESSAGE | NIF_ICON | NIF_TIP);
	a.uCallbackMessage = w->uCallbackMessage;
	a.hIcon = w->hIcon;
	if (a.uFlags & NIF_TIP)
	{
		WideCharToMultiByte(CP_ACP, 0, w->szTip, -1, a.szTip, 64, NULL, NULL);
		a.szTip[63] = 0;
	}
	return Shell_NotifyIconA(msg, &a);
}

/* ---- SHBrowseForFolder ------------------------------------------------ */

typedef struct
{
	BFFCALLBACK cb;
	LPARAM lp;
	WNDPROC old;
} BFFCTX;

static const char bff_prop[] = "kexuni.bff";

/* the 9x dialog only knows the A messages: translate the W ones */
static LRESULT CALLBACK bff_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	BFFCTX *c = (BFFCTX *) GetPropA(hwnd, bff_prop);
	WNDPROC old = c ? c->old : DefWindowProcA;
	if (msg == BFFM_SETSTATUSTEXTW || (msg == BFFM_SETSELECTIONW && wp))
	{
		char *a = kxu_w2a((LPCWSTR) lp);
		LRESULT r = CallWindowProcA(old, hwnd, msg == BFFM_SETSTATUSTEXTW ? BFFM_SETSTATUSTEXTA : BFFM_SETSELECTIONA,
		                            wp, (LPARAM) a);
		kxu_free(a);
		return r;
	}
	if (msg == BFFM_SETSELECTIONW)
		msg = BFFM_SETSELECTIONA;   /* a PIDL: no conversion */
	if (msg == WM_NCDESTROY && c)
	{
		SetWindowLongA(hwnd, GWL_WNDPROC, (LONG) c->old);
		RemovePropA(hwnd, bff_prop);
	}
	return CallWindowProcA(old, hwnd, msg, wp, lp);
}

static int CALLBACK bff_cb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
	BFFCTX *c = (BFFCTX *) data;
	if (msg == BFFM_INITIALIZED && !GetPropA(hwnd, bff_prop))
	{
		SetPropA(hwnd, bff_prop, (HANDLE) c);
		c->old = (WNDPROC) SetWindowLongA(hwnd, GWL_WNDPROC, (LONG) bff_subclass);
	}
	if (!c->cb)
		return 0;
	if (msg == BFFM_VALIDATEFAILEDA)
	{
		LPWSTR w = kxu_a2w_new(CP_ACP, (LPCSTR) lp);
		int r = c->cb(hwnd, BFFM_VALIDATEFAILEDW, (LPARAM) w, c->lp);
		kxu_free(w);
		return r;
	}
	return c->cb(hwnd, msg, lp, c->lp);
}

LPITEMIDLIST KXU(SHBrowseForFolderW)(LPBROWSEINFOW w)
{
	BROWSEINFOA a;
	BFFCTX c;
	char disp[MAX_PATH * 2], *title = kxu_w2a(w->lpszTitle);
	LPITEMIDLIST r;
	disp[0] = 0;
	a.hwndOwner = w->hwndOwner;
	a.pidlRoot = w->pidlRoot;
	a.pszDisplayName = disp;
	a.lpszTitle = title;
	a.ulFlags = w->ulFlags;
	c.cb = w->lpfn;
	c.lp = w->lParam;
	c.old = NULL;
	a.lpfn = bff_cb;
	a.lParam = (LPARAM) &c;
	a.iImage = 0;
	r = SHBrowseForFolderA(&a);
	if (w->pszDisplayName)
		kxu_a2w(CP_ACP, disp, -1, w->pszDisplayName, MAX_PATH, NULL);
	w->iImage = a.iImage;
	kxu_free(title);
	return r;
}
