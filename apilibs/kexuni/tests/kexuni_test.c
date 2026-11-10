/*
 *  KernelExEx - kexuni.dll tests. Run on Windows 9x, or under Wine on the
 *  build machine (Wine's ANSI functions behave like Windows'). GPLv2.
 */
#include <windows.h>
#include "../../../net/rt/w9xrt.h"

static HMODULE uni;
static int fails, checks;

#define FN(type, name, args) type (WINAPI *name) args = (type (WINAPI *) args) GetProcAddress(uni, #name)

static void out(const char *s)
{
	DWORD n;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &n, NULL);
}

static void check(int ok, const char *what)
{
	char line[256];
	checks++;
	if (!ok)
		fails++;
	w9x_snprintf(line, sizeof(line), "%s %s\r\n", ok ? "ok  " : "FAIL", what);
	out(line);
}

static int weq(const WCHAR *a, const WCHAR *b)
{
	return lstrcmpW(a, b) == 0;
}

int mainCRTStartup(void)
{
	WCHAR buf[256];
	char path[MAX_PATH], line[128];
	uni = LoadLibraryA("kexuni.dll");
	check(uni != NULL, "load kexuni.dll");
	if (!uni)
		ExitProcess(1);
	{
		FN(HANDLE, CreateEventW, (LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR));
		FN(HANDLE, OpenEventW, (DWORD, BOOL, LPCWSTR));
		HANDLE e = CreateEventW(NULL, TRUE, FALSE, L"kexuni_test_event"), o;
		o = OpenEventW(EVENT_ALL_ACCESS, FALSE, L"kexuni_test_event");
		check(e && o, "CreateEventW / OpenEventW share a name");
		CloseHandle(o);
		CloseHandle(e);
	}
	{
		FN(BOOL, SetEnvironmentVariableW, (LPCWSTR, LPCWSTR));
		FN(DWORD, GetEnvironmentVariableW, (LPCWSTR, LPWSTR, DWORD));
		FN(DWORD, ExpandEnvironmentStringsW, (LPCWSTR, LPWSTR, DWORD));
		DWORD n;
		SetEnvironmentVariableW(L"KXU_T", L"hello");
		n = GetEnvironmentVariableW(L"KXU_T", buf, 256);
		check(n == 5 && weq(buf, L"hello"), "GetEnvironmentVariableW returns length without NUL");
		n = GetEnvironmentVariableW(L"KXU_T", buf, 3);
		check(n == 6, "GetEnvironmentVariableW too small returns size with NUL");
		n = ExpandEnvironmentStringsW(L"[%KXU_T%]", buf, 256);
		check(n == 8 && weq(buf, L"[hello]"), "ExpandEnvironmentStringsW counts the NUL");
	}
	{
		FN(BOOL, WritePrivateProfileStringW, (LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR));
		FN(DWORD, GetPrivateProfileStringW, (LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR));
		FN(UINT, GetPrivateProfileIntW, (LPCWSTR, LPCWSTR, INT, LPCWSTR));
		WCHAR ini[MAX_PATH];
		DWORD n;
		GetTempPathA(sizeof(path), path);
		lstrcatA(path, "kexuni_test.ini");
		DeleteFileA(path);
		MultiByteToWideChar(CP_ACP, 0, path, -1, ini, MAX_PATH);
		WritePrivateProfileStringW(L"S", L"a", L"one", ini);
		WritePrivateProfileStringW(L"S", L"b", L"42", ini);
		n = GetPrivateProfileStringW(L"S", L"a", L"x", buf, 256, ini);
		check(n == 3 && weq(buf, L"one"), "GetPrivateProfileStringW value");
		n = GetPrivateProfileStringW(L"S", NULL, L"", buf, 256, ini);
		check(n == 4 && buf[0] == 'a' && buf[1] == 0 && buf[2] == 'b' && buf[3] == 0 && buf[4] == 0,
		      "GetPrivateProfileStringW key list");
		n = GetPrivateProfileStringW(L"S", L"a", L"x", buf, 3, ini);
		check(n == 2 && weq(buf, L"on"), "GetPrivateProfileStringW truncates to cch-1");
		check(GetPrivateProfileIntW(L"S", L"b", 0, ini) == 42, "GetPrivateProfileIntW");
		DeleteFileA(path);
	}
	{
		FN(DWORD, FormatMessageW, (DWORD, LPCVOID, DWORD, DWORD, LPWSTR, DWORD, va_list *));
		DWORD_PTR args[2];
		DWORD n;
		LPWSTR p = NULL;
		args[0] = (DWORD_PTR) L"world";
		args[1] = 7;
		n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, L"hi %1, %2!d!", 0, 0, buf,
		                   256, (va_list *) args);
		check(n == 11 && weq(buf, L"hi world, 7"), "FormatMessageW with string and number inserts");
		n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
		                   L"x %1 y", 0, 0, (LPWSTR) &p, 0, NULL);
		check(n == 6 && p && weq(p, L"x %1 y"), "FormatMessageW allocates");
		LocalFree(p);
	}
	{
		int (WINAPIV *wsprintfW)(LPWSTR, LPCWSTR, ...) =
			(int (WINAPIV *)(LPWSTR, LPCWSTR, ...)) GetProcAddress(uni, "wsprintfW");
		int n = wsprintfW(buf, L"[%5d|%-3s|%hs|%04X|%c]", -42, L"ab", "cd", 0xBEEF, L'Z');
		check(n == 21 && weq(buf, L"[  -42|ab |cd|BEEF|Z]"), "wsprintfW");
	}
	{
		FN(int, GetDateFormatW, (LCID, DWORD, const SYSTEMTIME *, LPCWSTR, LPWSTR, int));
		SYSTEMTIME st;
		int n;
		ZeroMemory(&st, sizeof(st));
		st.wYear = 2026;
		st.wMonth = 11;
		st.wDay = 10;
		n = GetDateFormatW(0x0409, 0, &st, L"yyyy-MM-dd", buf, 256);
		check(n == 11 && weq(buf, L"2026-11-10"), "GetDateFormatW counts the NUL");
		n = GetDateFormatW(0x0409, 0, &st, L"yyyy-MM-dd", NULL, 0);
		check(n == 11, "GetDateFormatW size query");
	}
	{
		FN(BOOL, GetComputerNameW, (LPWSTR, LPDWORD));
		DWORD n = 256;
		check(GetComputerNameW(buf, &n) && n > 0 && (DWORD) lstrlenW(buf) == n, "GetComputerNameW");
	}
	{
		FN(UINT, RegisterWindowMessageW, (LPCWSTR));
		check(RegisterWindowMessageW(L"kexuni_msg") == RegisterWindowMessageA("kexuni_msg"),
		      "RegisterWindowMessageW matches the A name");
	}
	{
		FN(HFONT, CreateFontIndirectW, (const LOGFONTW *));
		FN(BOOL, GetTextMetricsW, (HDC, LPTEXTMETRICW));
		FN(int, GetTextFaceW, (HDC, int, LPWSTR));
		FN(BOOL, GetTextExtentExPointW, (HDC, LPCWSTR, int, int, LPINT, LPINT, LPSIZE));
		LOGFONTW lf;
		HDC dc = CreateCompatibleDC(NULL);
		HFONT f;
		TEXTMETRICW tm;
		int fit = 0, dx[8];
		SIZE sz;
		ZeroMemory(&lf, sizeof(lf));
		lf.lfHeight = -12;
		lstrcpyW(lf.lfFaceName, L"Arial");
		f = CreateFontIndirectW(&lf);
		SelectObject(dc, f);
		check(f && GetTextMetricsW(dc, &tm) && tm.tmHeight > 0, "CreateFontIndirectW / GetTextMetricsW");
		check(GetTextFaceW(dc, 64, buf) > 0 && buf[0], "GetTextFaceW");
		check(GetTextExtentExPointW(dc, L"abcd", 4, 10000, &fit, dx, &sz) && fit == 4 && dx[3] == sz.cx &&
		          dx[0] < dx[3],
		      "GetTextExtentExPointW extents");
		DeleteDC(dc);
		DeleteObject(f);
	}
	{
		FN(LONG, RegQueryValueW, (HKEY, LPCWSTR, LPWSTR, PLONG));
		HKEY k;
		LONG cb = sizeof(buf);
		RegCreateKeyA(HKEY_CURRENT_USER, "Software\\KernelExEx\\kexuni_test", &k);
		RegSetValueA(k, NULL, REG_SZ, "default", 7);
		check(RegQueryValueW(k, NULL, buf, &cb) == ERROR_SUCCESS && weq(buf, L"default") && cb == 16,
		      "RegQueryValueW returns bytes with NUL");
		RegCloseKey(k);
		RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\KernelExEx\\kexuni_test");
	}
	w9x_snprintf(line, sizeof(line), "%d/%d passed\r\n", checks - fails, checks);
	out(line);
	ExitProcess(fails ? 1 : 0);
	return 0;
}
