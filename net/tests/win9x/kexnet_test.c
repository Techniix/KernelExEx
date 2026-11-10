/*
 *  KernelEx net: self-test for Windows 95/98/ME (run on the target machine).
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *    kexnet_test selftest            known-answer tests for bcrypt, CrackUrl,
 *                                    and which module serves RtlGenRandom
 *    kexnet_test probe <allowlist>   check every dll!function in the list
 *                                    exists on this system
 *    kexnet_test get <url> [proxy]   WinHTTP GET; prints status, TLS errors
 *
 *  Output goes to the console and is appended to kexnet_test.log.
 *  No C runtime start-up: entry point mainCRTStartup below.
 */
#define _WINHTTP_INTERNAL_
#include <windows.h>
#include <winhttp.h>
#include <string.h>
#include "w9xrt.h"

typedef LONG (WINAPI *bc_open_t)(void **, const WCHAR *, const WCHAR *, ULONG);
LONG WINAPI BCryptOpenAlgorithmProvider(void **, const WCHAR *, const WCHAR *, ULONG);
LONG WINAPI BCryptCloseAlgorithmProvider(void *, ULONG);
LONG WINAPI BCryptGenRandom(void *, UCHAR *, ULONG, ULONG);
LONG WINAPI BCryptHash(void *, UCHAR *, ULONG, UCHAR *, ULONG, UCHAR *, ULONG);

static HANDLE out, logf;
static int failures;

static void say(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	DWORD n;
	va_start(ap, fmt);
	w9x_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	WriteFile(out, buf, (DWORD)strlen(buf), &n, NULL);
	if (logf != INVALID_HANDLE_VALUE)
		WriteFile(logf, buf, (DWORD)strlen(buf), &n, NULL);
}

static void check(int ok, const char *what)
{
	say("%s %s\r\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static void hex(const unsigned char *p, size_t n, char *o)
{
	static const char h[] = "0123456789abcdef";
	size_t i;
	for (i = 0; i < n; i++)
	{
		o[2 * i] = h[p[i] >> 4];
		o[2 * i + 1] = h[p[i] & 15];
	}
	o[2 * n] = 0;
}

static void kat(const WCHAR *alg, int hmac, const char *key, const char *msg, const char *want, const char *name)
{
	void *a = NULL;
	unsigned char d[64];
	char got[129];
	size_t dl = strlen(want) / 2;
	LONG s = BCryptOpenAlgorithmProvider(&a, alg, NULL, hmac ? 8 : 0);
	if (s == 0)
		s = BCryptHash(a, (UCHAR *)key, key ? (ULONG)strlen(key) : 0, (UCHAR *)msg, (ULONG)strlen(msg), d, (ULONG)dl);
	if (a)
		BCryptCloseAlgorithmProvider(a, 0);
	got[0] = 0;
	if (s == 0)
		hex(d, dl, got);
	check(s == 0 && !strcmp(got, want), name);
	if (s != 0 || strcmp(got, want))
		say("     status %08lx got %s\r\n", (unsigned long)s, got);
}

static void module_of(void *fn, char *name, DWORD size)
{
	MEMORY_BASIC_INFORMATION mbi;
	name[0] = 0;
	if (fn && VirtualQuery(fn, &mbi, sizeof(mbi)))
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, size);
}

static int selftest(void)
{
	static const WCHAR SHA256[] = { 'S', 'H', 'A', '2', '5', '6', 0 };
	static const WCHAR SHA1[] = { 'S', 'H', 'A', '1', 0 };
	static const WCHAR MD5[] = { 'M', 'D', '5', 0 };
	unsigned char r1[32], r2[32];
	char where[MAX_PATH];
	typedef BOOLEAN (WINAPI *rtlgen_t)(PVOID, ULONG);
	rtlgen_t rtlgen;

	say("== bcrypt known answers\r\n");
	kat(SHA256, 0, NULL, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(abc)");
	kat(SHA1, 0, NULL, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1(abc)");
	kat(MD5, 0, NULL, "abc", "900150983cd24fb0d6963f7d28e17f72", "MD5(abc)");
	/* RFC 4231 test case 2 */
	kat(SHA256, 1, "Jefe", "what do ya want for nothing?",
	    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC-SHA-256 (RFC 4231 #2)");

	say("== random numbers\r\n");
	check(BCryptGenRandom(NULL, r1, sizeof(r1), 2) == 0 && BCryptGenRandom(NULL, r2, sizeof(r2), 2) == 0 &&
	      memcmp(r1, r2, sizeof(r1)) != 0, "BCryptGenRandom (system preferred RNG)");
	rtlgen = (rtlgen_t)(void *)GetProcAddress(LoadLibraryA("ADVAPI32.DLL"), "SystemFunction036");
	module_of((void *)rtlgen, where, sizeof(where));
	say("     ADVAPI32!SystemFunction036 is served by: %s\r\n", rtlgen ? where : "(not exported)");
	CharLowerA(where);
	check(rtlgen && strstr(where, "kextls.dll") != NULL,
	      "RtlGenRandom routed to kextls.dll by KernelEx (needs CORE.INI contents=...,kextls)");
	if (rtlgen)
		check(rtlgen(r1, sizeof(r1)) && memcmp(r1, r2, sizeof(r1)) != 0, "RtlGenRandom produces output");

	say("== WinHttpCrackUrl\r\n");
	{
		static const WCHAR url[] = { 'h', 't', 't', 'p', 's', ':', '/', '/', 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.',
		                             'o', 'r', 'g', ':', '8', '4', '4', '3', '/', 'a', '/', 'b', '?', 'q', '=', '1', 0 };
		URL_COMPONENTS uc;
		memset(&uc, 0, sizeof(uc));
		uc.dwStructSize = sizeof(uc);
		uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = uc.dwSchemeLength = (DWORD)-1;
		check(WinHttpCrackUrl(url, 0, 0, &uc) && uc.nScheme == INTERNET_SCHEME_HTTPS && uc.nPort == 8443 &&
		      uc.dwHostNameLength == 11 && uc.dwUrlPathLength == 4 && uc.dwExtraInfoLength == 4,
		      "crack https URL with port, path and query");
	}
	say(failures ? "SELFTEST FAILED (%d)\r\n" : "SELFTEST OK\r\n", failures);
	return failures ? 1 : 0;
}

static int probe(const char *list)
{
	HANDLE f = CreateFileA(list, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	static char buf[65536];
	DWORD n = 0;
	char *p, *line;
	int missing = 0, total = 0;

	if (f == INVALID_HANDLE_VALUE || !ReadFile(f, buf, sizeof(buf) - 1, &n, NULL))
	{
		say("cannot read %s\r\n", list);
		return 2;
	}
	CloseHandle(f);
	buf[n] = 0;
	for (line = buf; line && *line; line = p)
	{
		char *bang, *hash;
		p = strchr(line, '\n');
		if (p)
			*p++ = 0;
		if ((hash = strchr(line, '#')))
			*hash = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		{
			size_t l = strlen(line);
			while (l && (line[l - 1] == '\r' || line[l - 1] == ' '))
				line[--l] = 0;
		}
		if (!*line || !(bang = strchr(line, '!')))
			continue;
		*bang = 0;
		total++;
		{
			HMODULE m = LoadLibraryA(line);
			if (!m || !GetProcAddress(m, bang + 1))
			{
				say("MISSING %s!%s\r\n", line, bang + 1);
				missing++;
			}
		}
	}
	say("probe: %d of %d functions present\r\n", total - missing, total);
	return missing ? 1 : 0;
}

static WCHAR *widen(const char *s)
{
	static WCHAR w[2048];
	int i;
	for (i = 0; s[i] && i < 2047; i++)
		w[i] = (unsigned char)s[i];
	w[i] = 0;
	return w;
}

static int get(const char *url, const char *proxy)
{
	static const WCHAR agent[] = { 'k', 'e', 'x', 'n', 'e', 't', '_', 't', 'e', 's', 't', '/', '1', 0 };
	static WCHAR host[256], path[2048], wproxy[300];
	static char body[16384];
	URL_COMPONENTS uc;
	HINTERNET s, c, r;
	DWORD status = 0, sz = sizeof(status), got, total = 0, t0 = GetTickCount();
	DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000;
	int i;

	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;
	uc.dwExtraInfoLength = (DWORD)-1;
	if (!WinHttpCrackUrl(widen(url), 0, 0, &uc))
	{
		say("FAIL crack url: error %lu\r\n", GetLastError());
		return 1;
	}
	if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
	{
		size_t pl = uc.dwUrlPathLength;
		for (i = 0; i < (int)uc.dwExtraInfoLength && pl + (size_t)i < 2047; i++)
			path[pl + (size_t)i] = uc.lpszExtraInfo[i];
		path[pl + (size_t)i] = 0;
	}
	if (proxy)
	{
		WCHAR *w = widen(proxy);
		for (i = 0; w[i] && i < 299; i++)
			wproxy[i] = w[i];
		wproxy[i] = 0;
	}
	s = WinHttpOpen(agent, proxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                proxy ? wproxy : NULL, NULL, 0);
	if (!s)
	{
		say("FAIL WinHttpOpen: %lu\r\n", GetLastError());
		return 1;
	}
	WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
	c = WinHttpConnect(s, host, uc.nPort, 0);
	r = c ? WinHttpOpenRequest(c, NULL, path, NULL, NULL, NULL,
	                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
	if (!r || !WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL))
	{
		DWORD e = GetLastError();
		say("FAIL request: WinHTTP error %lu (%s)\r\n", e,
		    e == ERROR_WINHTTP_SECURE_INVALID_CA ? "certificate not trusted" :
		    e == ERROR_WINHTTP_SECURE_CERT_CN_INVALID ? "certificate name mismatch" :
		    e == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID ? "certificate expired or clock wrong" :
		    e == ERROR_WINHTTP_SECURE_CHANNEL_ERROR ? "TLS handshake failed" :
		    e == ERROR_WINHTTP_SECURE_FAILURE ? "TLS setup failed (clock, roots or entropy)" :
		    e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "DNS lookup failed" :
		    e == ERROR_WINHTTP_CANNOT_CONNECT ? "cannot connect" :
		    e == ERROR_WINHTTP_TIMEOUT ? "timeout" : "see winhttp.h");
		return 1;
	}
	WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
	while (WinHttpReadData(r, body, sizeof(body), &got) && got)
		total += got;
	say("OK status=%lu bytes=%lu time=%lums\r\n", status, total, GetTickCount() - t0);
	WinHttpCloseHandle(r);
	WinHttpCloseHandle(c);
	WinHttpCloseHandle(s);
	return 0;
}

/* minimal argv splitting: spaces separate, double quotes group */
static int split(char *cmd, char **argv, int max)
{
	int argc = 0;
	while (*cmd && argc < max)
	{
		while (*cmd == ' ' || *cmd == '\t')
			cmd++;
		if (!*cmd)
			break;
		if (*cmd == '"')
		{
			argv[argc++] = ++cmd;
			while (*cmd && *cmd != '"')
				cmd++;
		}
		else
		{
			argv[argc++] = cmd;
			while (*cmd && *cmd != ' ' && *cmd != '\t')
				cmd++;
		}
		if (*cmd)
			*cmd++ = 0;
	}
	return argc;
}

void WINAPI mainCRTStartup(void)
{
	static char cmd[4096];
	char *argv[8];
	int argc, rc = 2;

	lstrcpynA(cmd, GetCommandLineA(), sizeof(cmd));
	argc = split(cmd, argv, 8);
	out = GetStdHandle(STD_OUTPUT_HANDLE);
	logf = CreateFileA("kexnet_test.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
	if (logf != INVALID_HANDLE_VALUE)
		SetFilePointer(logf, 0, NULL, FILE_END);
	if (argc >= 2 && !strcmp(argv[1], "selftest"))
		rc = selftest();
	else if (argc >= 3 && !strcmp(argv[1], "probe"))
		rc = probe(argv[2]);
	else if (argc >= 3 && !strcmp(argv[1], "get"))
		rc = get(argv[2], argc >= 4 ? argv[3] : NULL);
	else
		say("usage: kexnet_test selftest | probe <allowlist> | get <url> [proxy host:port]\r\n");
	if (logf != INVALID_HANDLE_VALUE)
		CloseHandle(logf);
	ExitProcess((UINT)rc);
}
