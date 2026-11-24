/*
 *  KernelExEx - kexinfo.exe: hardware report (docs/KEDCL.md, "Hardware report").
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Writes C:\KEXINFO.TXT and opens it: Windows, CPU, memory, firmware (BIOS date,
 *  SMBIOS and ACPI anchors), every device in the Configuration Manager tree with
 *  its hardware IDs, class, driver, status / problem code and IRQ, DMA, I/O and
 *  memory assignments, KernelExEx driver diagnostics (KernelEx\Diag) and the
 *  touchpad driver's log. "kexinfo /q" writes the file without opening it.
 *  "kexinfo /reinstall" first makes Windows forget (Enum key deleted), so that it
 *  sets them up as new at the next start, every device
 *  Windows recorded as "drivers not installed" (problem 28) or "not configured"
 *  (problem 1) that one of our INFs (INF\KernelExEx*.inf, INF\OTHER\KernelExEx*.inf) supports:
 *  Windows offers drivers only for new devices, so a device it met before
 *  KernelExEx was installed would never get ours. Setup runs it.
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

/* the files behind a device: its class key's DevLoader / NTMPDriver / PortDriver,
   and a display's DEFAULT\drv (vga.drv is Windows' standard VGA) */
static void driver_files(DEVINST d)
{
	char drv[128], key[200], v[128], line[600];
	static const char *names[] = { "DevLoader", "NTMPDriver", "PortDriver", "DriverDesc" };
	ULONG type, len = sizeof(drv) - 1;
	HKEY k;
	int i, n = 0;
	if (cmProp(d, CM_DRP_DRIVER, &type, drv, &len, 0) != 0 || !len)
		return;
	drv[len] = 0;
	line[0] = 0;
	for (i = 0; i < 5; i++)
	{
		DWORD t, l = sizeof(v) - 1;
		if (i < 4)
			wsprintfA(key, "System\\CurrentControlSet\\Services\\Class\\%s", drv);
		else
			wsprintfA(key, "System\\CurrentControlSet\\Services\\Class\\%s\\DEFAULT", drv);
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
			continue;
		if (RegQueryValueExA(k, i < 4 ? names[i] : "drv", NULL, &t, (BYTE *) v, &l) == ERROR_SUCCESS && t == REG_SZ && l > 1)
		{
			v[l] = 0;
			if (lstrlenA(line) + lstrlenA(v) + 20 < (int) sizeof(line))
			{
				wsprintfA(line + lstrlenA(line), "%s%s=%s", n++ ? ", " : "", i < 4 ? names[i] : "drv", v);
			}
		}
		RegCloseKey(k);
	}
	if (n)
	{
		ind();
		pf("    driver files: %s\r\n", line);
	}
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
	driver_files(d);
	resources(d);
	if (cmChild(&c, d, 0) == 0)
	{
		walk(c, depth + 1);
		while (cmSibling(&c, c, 0) == 0)
			walk(c, depth + 1);
	}
}

static void cm_load(void)
{
	HMODULE h = LoadLibraryA("cfgmgr32.dll");
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
	if (!cmLocate || !cmChild || !cmSibling || !cmProp || !cmStatus || !cmId)
		cmLocate = NULL;
}

static void devices(void)
{
	DEVINST root;
	put("== Devices (Configuration Manager tree)\r\n");
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

/* ---------------- /reinstall ---------------- */
/* hardware and compatible IDs from the models sections of our INFs, upper case,
   each 0-terminated, the list ending with an empty string */
static char *ids;
static DWORD ids_len, ids_cap;
static int nreinstall, reinstall_disks;

/* a disk controller (PCI class 01): may hold the disk Windows starts from */
static int is_disk_ctl(DEVINST d)
{
	char b[1024];
	ULONG type, len = sizeof(b) - 2;
	char *p;
	if (cmProp(d, CM_DRP_COMPATIBLEIDS, &type, b, &len, 0) != 0 || !len)
		return 0;
	b[len] = b[len + 1] = 0;
	for (p = b; *p; p++)
		if (*p == ',')
			*p = 0;
	for (p = b; *p; p += lstrlenA(p) + 1)
	{
		/* PCI\CC_01, PCI\CC_0106, PCI\CC_010601 ... */
		static const char pre[] = "PCI\\CC_01";
		int i;
		for (i = 0; pre[i] && p[i] && ((p[i] | 0x20) == (pre[i] | 0x20)); i++)
			;
		if (!pre[i])
			return 1;
	}
	return 0;
}

static void id_add(const char *s, int n)
{
	char *p;
	int i;
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
		n--;
	while (n && (*s == ' ' || *s == '\t'))
		s++, n--;
	if (n <= 0 || n > 200)
		return;
	if (ids_len + n + 2 > ids_cap)
	{
		DWORD cap = ids_cap ? ids_cap * 2 : 65536;
		char *q = (char *) HeapAlloc(GetProcessHeap(), 0, cap);
		if (!q)
			return;
		if (ids)
		{
			CopyMemory(q, ids, ids_len);
			HeapFree(GetProcessHeap(), 0, ids);
		}
		ids = q, ids_cap = cap;
	}
	p = ids + ids_len;
	for (i = 0; i < n; i++)
		p[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
	p[n] = 0;
	ids_len += n + 1;
	ids[ids_len] = 0;
}

static int id_known(const char *s)
{
	const char *p;
	char u[256];
	int i;
	for (i = 0; s[i] && i < 255; i++)
		u[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
	u[i] = 0;
	for (p = ids; p && *p; p += lstrlenA(p) + 1)
		if (!lstrcmpA(p, u))
			return 1;
	return 0;
}

static int sect_is(const char *line, const char *names)
{
	/* line is "[name]"; names: 0-separated list of model section names */
	char n[128];
	int i = 0;
	const char *p;
	line++;
	while (*line && *line != ']' && i < 127)
		n[i++] = *line++;
	n[i] = 0;
	for (p = names; *p; p += lstrlenA(p) + 1)
		if (!lstrcmpiA(p, n))
			return 1;
	return 0;
}

static void inf_ids(const char *path)
{
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	DWORD size, got;
	char *t, *line, *end, models[1024];
	int mlen = 0, pass, insect;
	if (f == INVALID_HANDLE_VALUE)
		return;
	size = GetFileSize(f, NULL);
	t = (size && size < 4000000) ? (char *) HeapAlloc(GetProcessHeap(), 0, size + 1) : NULL;
	if (!t || !ReadFile(f, t, size, &got, NULL))
	{
		CloseHandle(f);
		return;
	}
	CloseHandle(f);
	t[got] = 0;
	models[0] = 0;
	/* pass 0: the section names [Manufacturer] lists; pass 1: their lines */
	for (pass = 0; pass < 2; pass++)
	{
		insect = 0;
		for (line = t; *line; line = *end ? end + 1 : end)
		{
			char *eq, *c;
			for (end = line; *end && *end != '\n'; end++)
				;
			while (*line == ' ' || *line == '\t')
				line++;
			if (*line == '[')
			{
				if (pass == 0)
				{
					char n[16];
					int i;
					for (i = 0; i < 15 && line[i + 1] && line[i + 1] != ']'; i++)
						n[i] = line[i + 1];
					n[i] = 0;
					insect = !lstrcmpiA(n, "Manufacturer");
				}
				else
					insect = sect_is(line, models);
				continue;
			}
			if (!insect || *line == ';')
				continue;
			for (c = line; c < end && *c != ';'; c++)
				;
			for (eq = line; eq < c && *eq != '='; eq++)
				;
			if (eq >= c)
				continue;
			eq++;
			if (pass == 0)
			{
				/* %mfg%=Models[,decoration...]: the first name only (Windows 98) */
				char *e = eq;
				while (e < c && *e != ',')
					e++;
				while (eq < e && (*eq == ' ' || *eq == '\t'))
					eq++;
				while (e > eq && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
					e--;
				if (e > eq && mlen + (e - eq) + 2 < (int) sizeof(models))
				{
					CopyMemory(models + mlen, eq, e - eq);
					mlen += (int) (e - eq);
					models[mlen++] = 0;
					models[mlen] = 0;
				}
			}
			else
			{
				/* desc = install-section, id[, compatible id...] */
				char *a = eq;
				while (a < c && *a != ',')
					a++;
				while (a < c)
				{
					char *b = ++a;
					while (a < c && *a != ',')
						a++;
					id_add(b, (int) (a - b));
				}
			}
		}
	}
	HeapFree(GetProcessHeap(), 0, t);
}

static int ids_match(DEVINST d, ULONG which)
{
	char b[1024];
	ULONG type, len = sizeof(b) - 2;
	char *p;
	if (cmProp(d, which, &type, b, &len, 0) != 0 || !len)
		return 0;
	b[len] = b[len + 1] = 0;
	/* REG_MULTI_SZ, or one comma-separated string */
	for (p = b; *p; p++)
		if (*p == ',')
			*p = 0;
	for (p = b; *p; p += lstrlenA(p) + 1)
		if (id_known(p))
			return 1;
	return 0;
}

static void mark(DEVINST d)
{
	char id[512], key[600];
	ULONG status = 0, problem = 0;
	DEVINST c;
	if (cmStatus(&status, &problem, d, 0) == 0 && (status & DN_HAS_PROBLEM) && (problem == 28 || problem == 1) &&
		(ids_match(d, CM_DRP_HARDWAREID) || ids_match(d, CM_DRP_COMPATIBLEIDS)))
	{
		id[0] = 0;
		cmId(d, id, sizeof(id), 0);
		wsprintfA(key, "Enum\\%s", id);
		if (!reinstall_disks && is_disk_ctl(d))
			pf("  %s: a disk controller, left alone (kexinfo /reinstall /all includes it)\r\n", id);
		else if (id[0] && RegDeleteKeyA(HKEY_LOCAL_MACHINE, key) == ERROR_SUCCESS)
		{
			/* forgotten: at the next start Windows meets it as a new device and finds
			   our INF (setting CONFIGFLAG_REINSTALL is not enough: Windows 98 puts
			   FAILEDINSTALL back at start-up and asks nothing) */
			pf("  %s\r\n", id);
			nreinstall++;
		}
	}
	if (cmChild(&c, d, 0) == 0)
	{
		mark(c);
		while (cmSibling(&c, c, 0) == 0)
			mark(c);
	}
}

static void reinstall(void)
{
	char dir[MAX_PATH], path[MAX_PATH];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	DEVINST root;
	put("== Devices set up again at the next start (kexinfo /reinstall)\r\n");
	{
		static const char *sub[] = { "\\INF\\OTHER\\", "\\INF\\" };
		int j;
		for (j = 0; j < 2; j++)
		{
			GetWindowsDirectoryA(dir, sizeof(dir));
			lstrcatA(dir, sub[j]);
			wsprintfA(path, "%sKernelExEx*.inf", dir);
			h = FindFirstFileA(path, &fd);
			if (h == INVALID_HANDLE_VALUE)
				continue;
			do
			{
				wsprintfA(path, "%s%s", dir, fd.cFileName);
				inf_ids(path);
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	}
	if (!ids || !cmLocate || cmLocate(&root, NULL, 0) != 0)
	{
		put("  (no KernelExEx INFs or no device tree)\r\n\r\n");
		return;
	}
	mark(root);
	pf("%d device(s)\r\n", nreinstall);
	/* Windows' driver index: it is rebuilt when missing. Without this, an INF added to
	   INF\OTHER is not seen (the INF folder's own date does not change) and the
	   wizard finds no driver for a device Windows meets again */
	GetWindowsDirectoryA(dir, sizeof(dir));
	wsprintfA(path, "%s\\INF\\DRVIDX.BIN", dir);
	DeleteFileA(path);
	wsprintfA(path, "%s\\INF\\DRVDATA.BIN", dir);
	DeleteFileA(path);
	put("driver index (INF\\DRVIDX.BIN, DRVDATA.BIN) removed: Windows rebuilds it\r\n\r\n");
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
			{
				pf("%s, %ux%u, stick %u; bytes %u packets %u posts %u errors %u resyncs %u\r\n", s.name, s.x_max,
				   s.y_max, s.has_stick, s.bytes, s.packets, s.posts, s.errors, s.resyncs);
				pf("IRQ 12 interrupts %u; 8042 command byte %x (was %x)\r\n", s.irqs, s.kbc_cmd, s.kbc_cmd_was);
			}
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
	int quiet = 0, re = 0;
	SYSTEMTIME t;
	for (; *c; c++)
		if (*c == '/' || *c == '-')
		{
			if ((c[1] | 0x20) == 'q')
				quiet = 1;
			if ((c[1] | 0x20) == 'r')
				re = quiet = 1;
			if ((c[1] | 0x20) == 'a' && (c[2] | 0x20) == 'l')
				reinstall_disks = 1;
		}
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
	cm_load();
	if (re)
		reinstall();
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
