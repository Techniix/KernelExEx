/*
 *  KernelExEx - kexinfo.exe: hardware report (docs/KEDCL.md, "Hardware report").
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Writes C:\KEXINFO.TXT and opens it: Windows, CPU, memory, firmware (BIOS date,
 *  SMBIOS and ACPI anchors), every device in the Configuration Manager tree with
 *  its hardware IDs, class, driver, status / problem code and IRQ, DMA, I/O and
 *  memory assignments, KernelExEx driver diagnostics (KernelEx\Diag) and the
 *  touchpad driver's log. "kexinfo /q" writes the file without opening it.
 *  For Windows 98 / ME (cfgmgr32.dll loaded at run time).
 */
#include <windows.h>
#include "../drivers/input/touch/tp.h"
#include "../drivers/input/touch/kextouch.h"

static HANDLE out;

static void put(const char *s)
{
	DWORD w;
	WriteFile(out, s, (DWORD) lstrlenA(s), &w, NULL);
}

static void pf(const char *fmt, ...)
{
	char b[1100];
	va_list ap;
	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	put(b);
}

/* ---------------- system ---------------- */
static void cpuid(unsigned leaf, unsigned *r)
{
	__asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

static int has_cpuid(void)
{
	unsigned a, b;
	__asm__ __volatile__("pushfl\n\tpopl %0\n\tmovl %0, %1\n\txorl $0x200000, %0\n\tpushl %0\n\tpopfl\n\t"
	                     "pushfl\n\tpopl %0\n\tpushl %1\n\tpopfl"
	                     : "=&r"(a), "=&r"(b));
	return ((a ^ b) & 0x200000) != 0;
}

static void system_info(void)
{
	OSVERSIONINFOA ov;
	MEMORYSTATUS ms;
	SYSTEM_INFO si;
	char buf[MAX_PATH];
	ov.dwOSVersionInfoSize = sizeof(ov);
	GetVersionExA(&ov);
	pf("== System\r\nWindows %lu.%lu.%lu %s (platform %lu)\r\n", ov.dwMajorVersion, ov.dwMinorVersion,
	   ov.dwBuildNumber & 0xffff, ov.szCSDVersion, ov.dwPlatformId);
	GetSystemInfo(&si);
	if (has_cpuid())
	{
		unsigned r[4], brand[13], max;
		char vendor[13];
		cpuid(0, r);
		*(unsigned *) vendor = r[1];
		*(unsigned *) (vendor + 4) = r[3];
		*(unsigned *) (vendor + 8) = r[2];
		vendor[12] = 0;
		cpuid(1, r);
		pf("CPU: %s family %u model %u stepping %u, features %08lx %08lx, %lu processor(s) seen\r\n", vendor,
		   ((r[0] >> 8) & 15) + ((r[0] >> 20) & 0xff), ((r[0] >> 4) & 15) | (((r[0] >> 16) & 15) << 4), r[0] & 15,
		   (unsigned long) r[3], (unsigned long) r[2], si.dwNumberOfProcessors);
		cpuid(0x80000000u, r);
		max = r[0];
		if (max >= 0x80000004u)
		{
			cpuid(0x80000002u, brand);
			cpuid(0x80000003u, brand + 4);
			cpuid(0x80000004u, brand + 8);
			brand[12] = 0;
			{
				char *b = (char *) brand;
				while (*b == ' ')
					b++;
				pf("CPU name: %s\r\n", b);
			}
		}
	}
	ms.dwLength = sizeof(ms);
	GlobalMemoryStatus(&ms);
	pf("Memory: %lu MB total, %lu MB free (as Windows sees it)\r\n", (unsigned long) (ms.dwTotalPhys >> 20),
	   (unsigned long) (ms.dwAvailPhys >> 20));
	/* KEXMEM: the memory limit in CONFIG.SYS */
	GetWindowsDirectoryA(buf, sizeof(buf));
	lstrcpyA(buf + 3, "CONFIG.SYS");
	{
		HANDLE f = CreateFileA(buf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		char c[4096];
		DWORD n = 0;
		int found = 0;
		if (f != INVALID_HANDLE_VALUE)
		{
			ReadFile(f, c, sizeof(c) - 1, &n, NULL);
			CloseHandle(f);
			c[n] = 0;
			{
				DWORD i, j;
				static const char want[] = "KEXMEM.SYS";
				for (i = 0; i + 10 <= n && !found; i++)
				{
					for (j = 0; j < 10; j++)
					{
						char ch = c[i + j];
						if (ch >= 'a' && ch <= 'z')
							ch = (char) (ch - 32);
						if (ch != want[j])
							break;
					}
					found = j == 10;
				}
			}
		}
		pf("KEXMEM (memory limit) in CONFIG.SYS: %s\r\n\r\n", found ? "yes" : "no");
	}
}

/* ---------------- firmware ---------------- */
static void firmware_info(void)
{
	/* the first megabyte is mapped at linear 0 in Windows 98's system VM */
	const unsigned char *bios = (const unsigned char *) 0xf0000;
	unsigned i;
	put("== Firmware\r\n");
	if (IsBadReadPtr(bios, 0x10000))
	{
		put("BIOS area not readable (not Windows 98/ME?)\r\n\r\n");
		return;
	}
	{
		char date[9];
		memcpy(date, bios + 0xfff5, 8);
		date[8] = 0;
		pf("BIOS date: %s, model byte %02x\r\n", date, bios[0xfffe]);
	}
	for (i = 0; i < 0x10000; i += 16)
	{
		if (!memcmp(bios + i, "_SM_", 4))
		{
			const unsigned char *e = bios + i;
			pf("SMBIOS %u.%u at F000:%04x, table %lu bytes at %08lx, %u structures\r\n", e[6], e[7], i,
			   (unsigned long) *(const unsigned short *) (e + 0x16), (unsigned long) *(const unsigned *) (e + 0x18),
			   *(const unsigned short *) (e + 0x1c));
		}
		if (!memcmp(bios + i, "RSD PTR ", 8))
		{
			const unsigned char *r = bios + i;
			char oem[7];
			memcpy(oem, r + 9, 6);
			oem[6] = 0;
			pf("ACPI RSDP at F000:%04x: OEM \"%s\", revision %u, RSDT at %08lx\r\n", i, oem, r[15],
			   (unsigned long) *(const unsigned *) (r + 16));
		}
	}
	put("\r\n");
}

/* ---------------- devices (cfgmgr32) ---------------- */
typedef DWORD DEVINST, LOG_CONF, RES_DES;
typedef DWORD(WINAPI *pLocate)(DEVINST *, char *, ULONG);
typedef DWORD(WINAPI *pRel)(DEVINST *, DEVINST, ULONG);
typedef DWORD(WINAPI *pProp)(DEVINST, ULONG, ULONG *, void *, ULONG *, ULONG);
typedef DWORD(WINAPI *pStatus)(ULONG *, ULONG *, DEVINST, ULONG);
typedef DWORD(WINAPI *pId)(DEVINST, char *, ULONG, ULONG);
typedef DWORD(WINAPI *pFirstLc)(LOG_CONF *, DEVINST, ULONG);
typedef DWORD(WINAPI *pNextRd)(RES_DES *, DWORD, ULONG, ULONG *, ULONG);
typedef DWORD(WINAPI *pRdSize)(ULONG *, RES_DES, ULONG);
typedef DWORD(WINAPI *pRdData)(RES_DES, void *, ULONG, ULONG);
static pLocate cmLocate;
static pRel cmChild, cmSibling;
static pProp cmProp;
static pStatus cmStatus;
static pId cmId;
static pFirstLc cmFirstLc;
static pNextRd cmNextRd;
static pRdSize cmRdSize;
static pRdData cmRdData;

#define CM_DRP_DEVICEDESC 1
#define CM_DRP_HARDWAREID 2
#define CM_DRP_COMPATIBLEIDS 3
#define CM_DRP_CLASS 8
#define CM_DRP_DRIVER 10
#define CM_DRP_MFG 12
#define ALLOC_LOG_CONF 2
#define DN_HAS_PROBLEM 0x400
#define DN_STARTED 0x8

static int indent;

static void ind(void)
{
	int i;
	for (i = 0; i < indent && i < 12; i++)
		put("  ");
}

static void prop(DEVINST d, ULONG which, const char *label, int multi)
{
	char b[1024];
	ULONG type, len = sizeof(b) - 2;
	if (cmProp(d, which, &type, b, &len, 0) != 0 || !len)
		return;
	b[len] = b[len + 1] = 0;
	ind();
	if (multi)
	{
		char *p = b;
		pf("    %s:", label);
		while (*p)
		{
			pf(" %s", p);
			p += lstrlenA(p) + 1;
		}
		put("\r\n");
	}
	else
		pf("    %s: %s\r\n", label, b);
}

static void resources(DEVINST d)
{
	/* the allocated configuration; Windows 98 sometimes keeps it as forced or boot */
	static const ULONG kinds[] = { ALLOC_LOG_CONF, 4 /* FORCED */, 3 /* BOOT */ };
	static const char *const kname[] = { "", " (forced)", " (boot)" };
	LOG_CONF lc;
	RES_DES rd, prev;
	ULONG type, r = 0;
	unsigned k;
	if (!cmFirstLc)
		return;
	for (k = 0; k < 3; k++)
		if ((r = cmFirstLc(&lc, d, kinds[k])) == 0)
			break;
	if (k == 3)
	{
		if (r != 14 /* CR_NO_MORE_LOG_CONF: the device has no resources */)
		{
			ind();
			pf("    resources: (CM error %lu)\r\n", r);
		}
		return;
	}
	prev = lc;
	ind();
	pf("    resources%s:", kname[k]);
	while (cmNextRd(&rd, prev, 0 /* all */, &type, 0) == 0)
	{
		unsigned char data[256];
		ULONG size = 0;
		prev = rd;
		if (cmRdSize(&size, rd, 0) || size > sizeof(data) || cmRdData(rd, data, size, 0))
			continue;
		switch (type)
		{
		case 1:   /* memory: MEM_DES count, type, base(8), end(8) */
			pf(" MEM %08lx-%08lx", (unsigned long) *(DWORD *) (data + 8), (unsigned long) *(DWORD *) (data + 16));
			break;
		case 2:   /* I/O */
			pf(" IO %04lx-%04lx", (unsigned long) *(DWORD *) (data + 8), (unsigned long) *(DWORD *) (data + 16));
			break;
		case 3:   /* DMA: count, type, flags, channel */
			pf(" DMA %lu", (unsigned long) *(DWORD *) (data + 12));
			break;
		case 4:   /* IRQ: count, type, flags, number */
			pf(" IRQ %lu", (unsigned long) *(DWORD *) (data + 12));
			break;
		default:
			pf(" type%lu", type);
		}
	}
	put("\r\n");
}

static int ndev, nproblem;

static void walk(DEVINST d, int depth)
{
	DEVINST c;
	char id[512];
	ULONG status = 0, problem = 0;
	int i;
	ndev++;
	id[0] = 0;
	cmId(d, id, sizeof(id), 0);
	for (i = 0; i < depth && i < 8; i++)
		put("  ");
	cmStatus(&status, &problem, d, 0);
	pf("%s", id);
	if (status & DN_HAS_PROBLEM)
	{
		pf("   *** PROBLEM %lu ***", problem);
		nproblem++;
	}
	else if (!(status & DN_STARTED))
		put("   (not started)");
	put("\r\n");
	indent = depth;
	prop(d, CM_DRP_DEVICEDESC, "name", 0);
	prop(d, CM_DRP_MFG, "maker", 0);
	prop(d, CM_DRP_HARDWAREID, "hardware IDs", 1);
	prop(d, CM_DRP_COMPATIBLEIDS, "compatible IDs", 1);
	prop(d, CM_DRP_CLASS, "class", 0);
	prop(d, CM_DRP_DRIVER, "driver key", 0);
	resources(d);
	if (cmChild(&c, d, 0) == 0)
	{
		walk(c, depth + 1);
		while (cmSibling(&c, c, 0) == 0)
			walk(c, depth + 1);
	}
}

static void devices(void)
{
	HMODULE h = LoadLibraryA("cfgmgr32.dll");
	DEVINST root;
	put("== Devices (Configuration Manager tree)\r\n");
	if (h)
	{
		cmLocate = (pLocate) GetProcAddress(h, "CM_Locate_DevNodeA");
		cmChild = (pRel) GetProcAddress(h, "CM_Get_Child");
		cmSibling = (pRel) GetProcAddress(h, "CM_Get_Sibling");
		cmProp = (pProp) GetProcAddress(h, "CM_Get_DevNode_Registry_PropertyA");
		cmStatus = (pStatus) GetProcAddress(h, "CM_Get_DevNode_Status");
		cmId = (pId) GetProcAddress(h, "CM_Get_Device_IDA");
		cmFirstLc = (pFirstLc) GetProcAddress(h, "CM_Get_First_Log_Conf");
		cmNextRd = (pNextRd) GetProcAddress(h, "CM_Get_Next_Res_Des");
		cmRdSize = (pRdSize) GetProcAddress(h, "CM_Get_Res_Des_Data_Size");
		cmRdData = (pRdData) GetProcAddress(h, "CM_Get_Res_Des_Data");
	}
	if (!cmLocate || !cmChild || !cmSibling || !cmProp || !cmStatus || !cmId || !cmNextRd || !cmRdSize || !cmRdData)
	{
		put("cfgmgr32.dll is missing or incomplete\r\n\r\n");
		return;
	}
	if (cmLocate(&root, NULL, 0) != 0)
	{
		put("cannot find the device tree root\r\n\r\n");
		return;
	}
	walk(root, 0);
	pf("\r\n%d devices, %d with a problem\r\n\r\n", ndev, nproblem);
}

/* ---------------- KernelExEx diagnostics ---------------- */
static void diag(void)
{
	HKEY k;
	DWORD i;
	put("== KernelExEx driver diagnostics (HKLM\\Software\\KernelEx\\Diag)\r\n");
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\KernelEx\\Diag", 0, KEY_READ, &k) == ERROR_SUCCESS)
	{
		for (i = 0;; i++)
		{
			char name[128];
			static unsigned char val[16384];
			DWORD nl = sizeof(name), vl = sizeof(val) - 1, type;
			if (RegEnumValueA(k, i, name, &nl, NULL, &type, val, &vl) != ERROR_SUCCESS)
				break;
			if (type == REG_DWORD)
				pf("%s = %lu (0x%08lx)\r\n", name, *(DWORD *) val, *(DWORD *) val);
			else if (type == REG_SZ || type == REG_EXPAND_SZ)
			{
				val[vl] = 0;
				pf("%s:\r\n", name);
				put((char *) val);
				put("\r\n");
			}
			else
				pf("%s: %lu bytes of binary data\r\n", name, vl);
		}
		RegCloseKey(k);
	}
	{
		HKEY c;
		put("\r\nInstalled KernelExEx components:");
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\KernelEx\\Components", 0, KEY_READ, &c) == ERROR_SUCCESS)
		{
			for (i = 0;; i++)
			{
				char name[128];
				DWORD nl = sizeof(name);
				if (RegEnumValueA(c, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
					break;
				pf(" %s", name);
			}
			RegCloseKey(c);
		}
		put("\r\n\r\n");
	}
	/* the touchpad driver, if it runs */
	{
		HANDLE v = CreateFileA("\\\\.\\KEXTOUCH", 0, 0, NULL, 0, 0, NULL);
		put("== Touchpad driver (KEXTOUCH)\r\n");
		if (v == INVALID_HANDLE_VALUE)
			put("not loaded\r\n\r\n");
		else
		{
			struct kextouch_status s;
			static char log[8192];
			DWORD got = 0;
			ZeroMemory(&s, sizeof(s));
			if (DeviceIoControl(v, KEXTOUCH_IOCTL_STATUS, NULL, 0, &s, sizeof(s), &got, NULL) && got >= sizeof(s))
				pf("%s, %ux%u, stick %u; bytes %u packets %u posts %u errors %u resyncs %u\r\n", s.name, s.x_max,
				   s.y_max, s.has_stick, s.bytes, s.packets, s.posts, s.errors, s.resyncs);
			got = 0;
			if (DeviceIoControl(v, KEXTOUCH_IOCTL_LOG, NULL, 0, log, sizeof(log) - 1, &got, NULL))
			{
				DWORD i2;
				for (i2 = 0; i2 < got; i2++)
				{
					char two[3] = { 0, 0, 0 };
					if (log[i2] == '\n')
						two[0] = '\r', two[1] = '\n';
					else
						two[0] = log[i2];
					put(two);
				}
			}
			CloseHandle(v);
			put("\r\n");
		}
	}
}

void entrypoint(void)
{
	char path[MAX_PATH], cmd[MAX_PATH + 32];
	const char *c = GetCommandLineA();
	int quiet = 0;
	SYSTEMTIME t;
	for (; *c; c++)
		if ((*c == '/' || *c == '-') && (c[1] | 0x20) == 'q')
			quiet = 1;
	GetWindowsDirectoryA(path, sizeof(path));
	lstrcpyA(path + 3, "KEXINFO.TXT");
	out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (out == INVALID_HANDLE_VALUE)
	{
		MessageBoxA(NULL, "Cannot write C:\\KEXINFO.TXT", "KernelExEx hardware report", MB_ICONERROR);
		ExitProcess(1);
	}
	GetLocalTime(&t);
	pf("KernelExEx hardware report, %04u-%02u-%02u %02u:%02u\r\n\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
	system_info();
	firmware_info();
	devices();
	diag();
	CloseHandle(out);
	if (!quiet)
	{
		wsprintfA(cmd, "notepad.exe %s", path);
		WinExec(cmd, SW_SHOWNORMAL);
	}
	ExitProcess(0);
}
