/*
 *  KernelExEx - kexntp.exe: keeps the Windows 9x clock right (SNTP, RFC 4330).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Runs at startup (HKLM ...\Run), hidden from the task list. Every few
 *  hours it asks several NTP servers for the time, takes the median of the
 *  answers that agree, and sets the clock.
 *
 *  SNTP is not authenticated, so the result is checked before it is used:
 *   - at least two servers must agree within 2 seconds;
 *   - a correction of more than a day is only applied when the current
 *     clock is clearly wrong (before this build's date), as after a dead
 *     CMOS battery. Otherwise it is logged and ignored.
 *
 *  Settings: HKLM\Software\KernelEx\TimeSync
 *    Servers  (string, space separated, default "pool.ntp.org time.cloudflare.com time.google.com")
 *    Interval (DWORD, hours, default 6)
 *    Enabled  (DWORD, default 1)
 *  Log: %WINDIR%\KernelEx\logs\kexntp.log
 *
 *  kexntp.exe /once   sync now and exit (prints the result)
 */
#include <winsock2.h>
#include <windows.h>
#include "../rt/w9xrt.h"

#define NTP_PORT 123
#define NTP_UNIX_DELTA 2208988800u   /* 1900 -> 1970 */
#define MAX_SERVERS 8
#define AGREE_MS 2000
#define BIG_STEP_S (24 * 3600)

#ifndef KEXNTP_BUILD_EPOCH
#define KEXNTP_BUILD_EPOCH 0
#endif

static char logpath[MAX_PATH];
static int console;

static void logf(const char *fmt, ...)
{
	char line[512], stamp[64];
	SYSTEMTIME st;
	va_list ap;
	HANDLE h;
	DWORD n;
	GetLocalTime(&st);
	w9x_snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay, st.wHour,
	             st.wMinute, st.wSecond);
	va_start(ap, fmt);
	w9x_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (console)
	{
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &n, NULL);
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &n, NULL);
	}
	if (!logpath[0])
		return;
	h = CreateFileA(logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	if (GetFileSize(h, NULL) > 64 * 1024)
	{
		SetFilePointer(h, 0, NULL, FILE_BEGIN);
		SetEndOfFile(h);
	}
	SetFilePointer(h, 0, NULL, FILE_END);
	WriteFile(h, stamp, lstrlenA(stamp), &n, NULL);
	WriteFile(h, line, lstrlenA(line), &n, NULL);
	WriteFile(h, "\r\n", 2, &n, NULL);
	CloseHandle(h);
}

/* milliseconds since 1970 from the system clock */
static long long now_ms(void)
{
	FILETIME ft;
	ULARGE_INTEGER u;
	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return (long long) ((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static BOOL set_ms(long long ms)
{
	ULARGE_INTEGER u;
	FILETIME ft;
	SYSTEMTIME st;
	u.QuadPart = (ULONGLONG) ms * 10000ULL + 116444736000000000ULL;
	ft.dwLowDateTime = u.LowPart;
	ft.dwHighDateTime = u.HighPart;
	return FileTimeToSystemTime(&ft, &st) && SetSystemTime(&st);
}

static long long ntp_ms(const unsigned char *p)
{
	unsigned long sec = ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) | ((unsigned long) p[2] << 8) | p[3];
	unsigned long frac = ((unsigned long) p[4] << 24) | ((unsigned long) p[5] << 16) | ((unsigned long) p[6] << 8) | p[7];
	return ((long long) sec - NTP_UNIX_DELTA) * 1000 + (long long) (((unsigned long long) frac * 1000) >> 32);
}

/* one SNTP exchange; returns 1 and the clock offset (server - local, ms) */
static int query(const char *host, long long *offset, long long *rtt)
{
	struct hostent *he = gethostbyname(host);
	struct sockaddr_in sa;
	unsigned char pkt[48];
	SOCKET s;
	fd_set rd;
	struct timeval tv = { 3, 0 };
	long long t1, t4, t2, t3;
	int n;
	if (!he || he->h_addrtype != AF_INET)
	{
		logf("%s: name lookup failed", host);
		return 0;
	}
	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		return 0;
	ZeroMemory(&sa, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(NTP_PORT);
	CopyMemory(&sa.sin_addr, he->h_addr_list[0], 4);
	ZeroMemory(pkt, sizeof(pkt));
	pkt[0] = 0x23;   /* LI 0, version 4, mode 3 (client) */
	t1 = now_ms();
	/* transmit timestamp = our time, echoed back as the originate stamp */
	{
		unsigned long long v = (unsigned long long) (t1 / 1000 + NTP_UNIX_DELTA);
		unsigned long frac = (unsigned long) ((((unsigned long long) (t1 % 1000)) << 32) / 1000);
		pkt[40] = (unsigned char) (v >> 24);
		pkt[41] = (unsigned char) (v >> 16);
		pkt[42] = (unsigned char) (v >> 8);
		pkt[43] = (unsigned char) v;
		pkt[44] = (unsigned char) (frac >> 24);
		pkt[45] = (unsigned char) (frac >> 16);
		pkt[46] = (unsigned char) (frac >> 8);
		pkt[47] = (unsigned char) frac;
	}
	{
		unsigned char sent[8];
		CopyMemory(sent, pkt + 40, 8);
		if (sendto(s, (const char *) pkt, sizeof(pkt), 0, (struct sockaddr *) &sa, sizeof(sa)) != sizeof(pkt))
		{
			closesocket(s);
			return 0;
		}
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		if (select(0, &rd, NULL, NULL, &tv) != 1)
		{
			logf("%s: no answer", host);
			closesocket(s);
			return 0;
		}
		n = recv(s, (char *) pkt, sizeof(pkt), 0);
		t4 = now_ms();
		closesocket(s);
		/* a reply to this request: server mode, synchronised, our stamp echoed */
		if (n < 48 || (pkt[0] & 7) != 4 || pkt[1] == 0 || pkt[1] > 15 || (pkt[0] >> 6) == 3 ||
		    memcmp(pkt + 24, sent, 8) != 0)
		{
			logf("%s: bad or unsynchronised reply", host);
			return 0;
		}
	}
	t2 = ntp_ms(pkt + 32);
	t3 = ntp_ms(pkt + 40);
	*offset = ((t2 - t1) + (t3 - t4)) / 2;
	*rtt = (t4 - t1) - (t3 - t2);
	return 1;
}

static void sort(long long *v, int n)
{
	int i, j;
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && v[j - 1] > v[j]; j--)
		{
			long long t = v[j];
			v[j] = v[j - 1];
			v[j - 1] = t;
		}
}

static void read_settings(char *servers, DWORD size, DWORD *interval, DWORD *enabled)
{
	HKEY k;
	DWORD n, type;
	lstrcpyA(servers, "pool.ntp.org time.cloudflare.com time.google.com");
	*interval = 6;
	*enabled = 1;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\KernelEx\\TimeSync", 0, KEY_READ, &k) != ERROR_SUCCESS)
		return;
	n = size;
	if (RegQueryValueExA(k, "Servers", NULL, &type, (BYTE *) servers, &n) != ERROR_SUCCESS || type != REG_SZ)
		lstrcpyA(servers, "pool.ntp.org time.cloudflare.com time.google.com");
	n = sizeof(DWORD);
	RegQueryValueExA(k, "Interval", NULL, NULL, (BYTE *) interval, &n);
	n = sizeof(DWORD);
	RegQueryValueExA(k, "Enabled", NULL, NULL, (BYTE *) enabled, &n);
	RegCloseKey(k);
	if (*interval < 1)
		*interval = 1;
}

/* returns 1 when the clock is now right */
static int sync_once(void)
{
	char servers[512], *p, *name[MAX_SERVERS];
	long long off[MAX_SERVERS], rtt, median, best[MAX_SERVERS];
	DWORD interval, enabled;
	int n = 0, i, good = 0;
	read_settings(servers, sizeof(servers), &interval, &enabled);
	for (p = servers; *p && n < MAX_SERVERS;)
	{
		while (*p == ' ')
			*p++ = 0;
		if (!*p)
			break;
		name[n++] = p;
		while (*p && *p != ' ')
			p++;
	}
	for (i = 0; i < n; i++)
		if (query(name[i], &off[good], &rtt))
		{
			logf("%s: offset %ld ms, round trip %ld ms", name[i], (long) off[good], (long) rtt);
			good++;
		}
	if (good < 2)
	{
		logf("fewer than two servers answered; clock not changed");
		return 0;
	}
	for (i = 0; i < good; i++)
		best[i] = off[i];
	sort(best, good);
	median = best[good / 2];
	{
		int agree = 0;
		for (i = 0; i < good; i++)
			if (off[i] - median < AGREE_MS && median - off[i] < AGREE_MS)
				agree++;
		if (agree < 2)
		{
			logf("servers disagree; clock not changed");
			return 0;
		}
	}
	if (median > -500 && median < 500)
	{
		logf("clock is right (off by %ld ms)", (long) median);
		return 1;
	}
	if ((median > (long long) BIG_STEP_S * 1000 || median < -(long long) BIG_STEP_S * 1000) &&
	    now_ms() / 1000 >= (long long) KEXNTP_BUILD_EPOCH)
	{
		logf("correction of %ld s refused: more than a day and the clock looks plausible", (long) (median / 1000));
		return 0;
	}
	if (!set_ms(now_ms() + median))
	{
		logf("SetSystemTime failed (%lu)", GetLastError());
		return 0;
	}
	logf("clock corrected by %ld ms", (long) median);
	return 1;
}

typedef DWORD(WINAPI *RSP_t)(DWORD, DWORD);

int mainCRTStartup(void)
{
	WSADATA wsa;
	char *cmd = GetCommandLineA();
	DWORD interval, enabled;
	char servers[512];
	int once = 0;
	for (; *cmd; cmd++)
		if ((cmd[0] == '/' || cmd[0] == '-') && (cmd[1] == 'o' || cmd[1] == 'O'))
			once = 1;
	GetWindowsDirectoryA(logpath, sizeof(logpath) - 32);
	lstrcatA(logpath, "\\KernelEx");
	CreateDirectoryA(logpath, NULL);
	lstrcatA(logpath, "\\logs");
	CreateDirectoryA(logpath, NULL);
	lstrcatA(logpath, "\\kexntp.log");
	if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0)
		ExitProcess(2);
	if (once)
	{
		/* a GUI program: open a console for the report */
		AllocConsole();
		console = 1;
		ExitProcess(sync_once() ? 0 : 1);
	}
	{
		/* keep running when the user logs off (Windows 9x service process) */
		RSP_t rsp = (RSP_t) GetProcAddress(GetModuleHandleA("KERNEL32.DLL"), "RegisterServiceProcess");
		if (rsp)
			rsp(0, 1);
	}
	/* give dial-up and DHCP a moment after startup */
	Sleep(30000);
	for (;;)
	{
		read_settings(servers, sizeof(servers), &interval, &enabled);
		if (enabled)
		{
			int ok = sync_once(), tries = 0;
			/* not online yet: retry every 10 minutes for the first hour */
			while (!ok && tries++ < 6)
			{
				Sleep(10 * 60 * 1000);
				ok = sync_once();
			}
		}
		Sleep(interval * 3600 * 1000);
	}
}
