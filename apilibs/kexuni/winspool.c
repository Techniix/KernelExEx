/*
 *  KernelExEx - kexuni.dll: WINSPOOL wide functions. GPLv2.
 *
 *  Most printer structures are flat records of string pointers plus
 *  numbers. One converter handles them all from a list of the offsets of
 *  their string (and DEVMODE) fields.
 */
#include "uni.h"
#include <winspool.h>
#include <stddef.h>

typedef struct
{
	unsigned short size;   /* of the W structure (same as A: pointers) */
	unsigned char nstr;
	unsigned short str[12];
	short devmode;         /* offset of a DEVMODE pointer, or -1 */
} LAYOUT;

#define O(t, f) ((unsigned short) offsetof(t, f))

static const LAYOUT printer_info_1 = { sizeof(PRINTER_INFO_1W), 3,
	{ O(PRINTER_INFO_1W, pDescription), O(PRINTER_INFO_1W, pName), O(PRINTER_INFO_1W, pComment) }, -1 };
static const LAYOUT printer_info_2 = { sizeof(PRINTER_INFO_2W), 11,
	{ O(PRINTER_INFO_2W, pServerName), O(PRINTER_INFO_2W, pPrinterName), O(PRINTER_INFO_2W, pShareName),
	  O(PRINTER_INFO_2W, pPortName), O(PRINTER_INFO_2W, pDriverName), O(PRINTER_INFO_2W, pComment),
	  O(PRINTER_INFO_2W, pLocation), O(PRINTER_INFO_2W, pSepFile), O(PRINTER_INFO_2W, pPrintProcessor),
	  O(PRINTER_INFO_2W, pDatatype), O(PRINTER_INFO_2W, pParameters) },
	O(PRINTER_INFO_2W, pDevMode) };
static const LAYOUT printer_info_5 = { sizeof(PRINTER_INFO_5W), 2,
	{ O(PRINTER_INFO_5W, pPrinterName), O(PRINTER_INFO_5W, pPortName) }, -1 };
static const LAYOUT job_info_1 = { sizeof(JOB_INFO_1W), 6,
	{ O(JOB_INFO_1W, pPrinterName), O(JOB_INFO_1W, pMachineName), O(JOB_INFO_1W, pUserName),
	  O(JOB_INFO_1W, pDocument), O(JOB_INFO_1W, pDatatype), O(JOB_INFO_1W, pStatus) }, -1 };
static const LAYOUT job_info_2 = { sizeof(JOB_INFO_2W), 10,
	{ O(JOB_INFO_2W, pPrinterName), O(JOB_INFO_2W, pMachineName), O(JOB_INFO_2W, pUserName),
	  O(JOB_INFO_2W, pDocument), O(JOB_INFO_2W, pNotifyName), O(JOB_INFO_2W, pDatatype),
	  O(JOB_INFO_2W, pPrintProcessor), O(JOB_INFO_2W, pParameters), O(JOB_INFO_2W, pDriverName),
	  O(JOB_INFO_2W, pStatus) },
	O(JOB_INFO_2W, pDevMode) };
static const LAYOUT doc_info_1 = { sizeof(DOC_INFO_1W), 3,
	{ O(DOC_INFO_1W, pDocName), O(DOC_INFO_1W, pOutputFile), O(DOC_INFO_1W, pDatatype) }, -1 };
static const LAYOUT driver_info_2 = { sizeof(DRIVER_INFO_2W), 5,
	{ O(DRIVER_INFO_2W, pName), O(DRIVER_INFO_2W, pEnvironment), O(DRIVER_INFO_2W, pDriverPath),
	  O(DRIVER_INFO_2W, pDataFile), O(DRIVER_INFO_2W, pConfigFile) }, -1 };
static const LAYOUT driver_info_3 = { sizeof(DRIVER_INFO_3W), 8,
	{ O(DRIVER_INFO_3W, pName), O(DRIVER_INFO_3W, pEnvironment), O(DRIVER_INFO_3W, pDriverPath),
	  O(DRIVER_INFO_3W, pDataFile), O(DRIVER_INFO_3W, pConfigFile), O(DRIVER_INFO_3W, pHelpFile),
	  O(DRIVER_INFO_3W, pMonitorName), O(DRIVER_INFO_3W, pDefaultDataType) }, -1 };
static const LAYOUT monitor_info_2 = { sizeof(MONITOR_INFO_2W), 3,
	{ O(MONITOR_INFO_2W, pName), O(MONITOR_INFO_2W, pEnvironment), O(MONITOR_INFO_2W, pDLLName) }, -1 };
static const LAYOUT providor_info_1 = { sizeof(PROVIDOR_INFO_1W), 3,
	{ O(PROVIDOR_INFO_1W, pName), O(PROVIDOR_INFO_1W, pEnvironment), O(PROVIDOR_INFO_1W, pDLLName) }, -1 };

typedef struct
{
	void *rec;
	void *free[14];
	int nfree;
} CONV;

/* W record -> new A record. DRIVER_INFO_3's pDependentFiles is a list and
 * handled by the caller. */
static void *conv_rec(const LAYOUT *l, const void *w, CONV *c)
{
	int i;
	char *a = (char *) kxu_alloc(l->size);
	c->nfree = 0;
	c->rec = a;
	if (!a)
		return NULL;
	CopyMemory(a, w, l->size);
	for (i = 0; i < l->nstr; i++)
	{
		char *s = kxu_w2a(*(LPCWSTR *) ((const char *) w + l->str[i]));
		*(char **) (a + l->str[i]) = s;
		c->free[c->nfree++] = s;
	}
	if (l->devmode >= 0)
	{
		DEVMODEA *d = kxu_devmode_w2a(*(const DEVMODEW **) ((const char *) w + l->devmode));
		*(DEVMODEA **) (a + l->devmode) = d;
		c->free[c->nfree++] = d;
	}
	return a;
}

static void conv_free(CONV *c)
{
	int i;
	for (i = 0; i < c->nfree; i++)
		kxu_free(c->free[i]);
	kxu_free(c->rec);
}

static const LAYOUT *printer_layout(DWORD level)
{
	switch (level)
	{
	case 1: return &printer_info_1;
	case 2: return &printer_info_2;
	case 5: return &printer_info_5;
	default: return NULL;
	}
}

#define STR2(fn, a1, a2)                                                                                               \
	char *a = kxu_w2a(a1), *b = kxu_w2a(a2);                                                                       \
	BOOL r = fn;                                                                                                   \
	kxu_free(a);                                                                                                   \
	kxu_free(b);                                                                                                   \
	return r

BOOL KXU(OpenPrinterW)(LPWSTR name, LPHANDLE h, LPPRINTER_DEFAULTSW def)
{
	char *a = kxu_w2a(name), *dt = NULL;
	PRINTER_DEFAULTSA d, *pd = NULL;
	DEVMODEA *dm = NULL;
	BOOL r;
	if (def)
	{
		d.pDatatype = dt = kxu_w2a(def->pDatatype);
		d.pDevMode = dm = kxu_devmode_w2a(def->pDevMode);
		d.DesiredAccess = def->DesiredAccess;
		pd = &d;
	}
	r = OpenPrinterA(a, h, pd);
	kxu_free(a);
	kxu_free(dt);
	kxu_free(dm);
	return r;
}

BOOL KXU(ResetPrinterW)(HANDLE h, LPPRINTER_DEFAULTSW def)
{
	char *dt = NULL;
	PRINTER_DEFAULTSA d, *pd = NULL;
	DEVMODEA *dm = NULL;
	BOOL r;
	if (def)
	{
		d.pDatatype = dt = kxu_w2a(def->pDatatype);
		d.pDevMode = dm = kxu_devmode_w2a(def->pDevMode);
		d.DesiredAccess = def->DesiredAccess;
		pd = &d;
	}
	r = ResetPrinterA(h, pd);
	kxu_free(dt);
	kxu_free(dm);
	return r;
}

HANDLE KXU(AddPrinterW)(LPWSTR server, DWORD level, LPBYTE info)
{
	const LAYOUT *l = printer_layout(level);
	char *s;
	CONV c;
	HANDLE r;
	if (!l)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return NULL;
	}
	s = kxu_w2a(server);
	r = conv_rec(l, info, &c) ? AddPrinterA(s, level, (LPBYTE) c.rec) : NULL;
	conv_free(&c);
	kxu_free(s);
	return r;
}

BOOL KXU(SetPrinterW)(HANDLE h, DWORD level, LPBYTE info, DWORD cmd)
{
	const LAYOUT *l;
	CONV c;
	BOOL r;
	if (level == 0 || !info)
		return SetPrinterA(h, level, info, cmd);
	if (!(l = printer_layout(level)))
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	r = conv_rec(l, info, &c) ? SetPrinterA(h, level, (LPBYTE) c.rec, cmd) : FALSE;
	conv_free(&c);
	return r;
}

BOOL KXU(SetJobW)(HANDLE h, DWORD id, DWORD level, LPBYTE info, DWORD cmd)
{
	const LAYOUT *l = level == 1 ? &job_info_1 : level == 2 ? &job_info_2 : NULL;
	CONV c;
	BOOL r;
	if (level == 0 || !info)
		return SetJobA(h, id, level, info, cmd);
	if (!l)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	r = conv_rec(l, info, &c) ? SetJobA(h, id, level, (LPBYTE) c.rec, cmd) : FALSE;
	conv_free(&c);
	return r;
}

DWORD KXU(StartDocPrinterW)(HANDLE h, DWORD level, LPBYTE info)
{
	CONV c;
	DWORD r;
	if (level != 1)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return 0;
	}
	r = conv_rec(&doc_info_1, info, &c) ? StartDocPrinterA(h, level, (LPBYTE) c.rec) : 0;
	conv_free(&c);
	return r;
}

BOOL KXU(AddPrinterDriverW)(LPWSTR server, DWORD level, LPBYTE info)
{
	const LAYOUT *l = level == 2 ? &driver_info_2 : level == 3 ? &driver_info_3 : NULL;
	char *s, *deps = NULL;
	CONV c;
	BOOL r = FALSE;
	if (!l)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	s = kxu_w2a(server);
	if (conv_rec(l, info, &c))
	{
		if (level == 3)
			((DRIVER_INFO_3A *) c.rec)->pDependentFiles = deps =
				kxu_w2a_multi(((DRIVER_INFO_3W *) info)->pDependentFiles);
		r = AddPrinterDriverA(s, level, (LPBYTE) c.rec);
	}
	conv_free(&c);
	kxu_free(deps);
	kxu_free(s);
	return r;
}

BOOL KXU(AddMonitorW)(LPWSTR server, DWORD level, LPBYTE info)
{
	char *s;
	CONV c;
	BOOL r;
	if (level != 2)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	s = kxu_w2a(server);
	r = conv_rec(&monitor_info_2, info, &c) ? AddMonitorA(s, level, (LPBYTE) c.rec) : FALSE;
	conv_free(&c);
	kxu_free(s);
	return r;
}

BOOL KXU(AddPrintProvidorW)(LPWSTR server, DWORD level, LPBYTE info)
{
	char *s;
	CONV c;
	BOOL r;
	if (level != 1)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	s = kxu_w2a(server);
	r = conv_rec(&providor_info_1, info, &c) ? AddPrintProvidorA(s, level, (LPBYTE) c.rec) : FALSE;
	conv_free(&c);
	kxu_free(s);
	return r;
}

BOOL KXU(AddJobW)(HANDLE h, DWORD level, LPBYTE data, DWORD cb, LPDWORD needed)
{
	/* ADDJOB_INFO_1: a path pointer and a job id; the path lives after it */
	BYTE buf[sizeof(ADDJOB_INFO_1A) + MAX_PATH * 2];
	DWORD n = 0, need;
	ADDJOB_INFO_1A *a = (ADDJOB_INFO_1A *) buf;
	int len;
	if (level != 1)
	{
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	if (!AddJobA(h, level, buf, sizeof(buf), &n))
		return FALSE;
	len = kxu_a2w_len(CP_ACP, a->Path, -1) + 1;
	need = sizeof(ADDJOB_INFO_1W) + len * sizeof(WCHAR);
	if (needed)
		*needed = need;
	if (cb < need || !data)
	{
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	{
		ADDJOB_INFO_1W *w = (ADDJOB_INFO_1W *) data;
		w->JobId = a->JobId;
		w->Path = (LPWSTR) (w + 1);
		kxu_a2w(CP_ACP, a->Path, -1, w->Path, len, NULL);
	}
	return TRUE;
}

BOOL KXU(AddPortW)(LPWSTR server, HWND hwnd, LPWSTR monitor)
{
	STR2(AddPortA(a, hwnd, b), server, monitor);
}

BOOL KXU(ConfigurePortW)(LPWSTR server, HWND hwnd, LPWSTR port)
{
	STR2(ConfigurePortA(a, hwnd, b), server, port);
}

BOOL KXU(DeletePortW)(LPWSTR server, HWND hwnd, LPWSTR port)
{
	STR2(DeletePortA(a, hwnd, b), server, port);
}

BOOL KXU(AddPrintProcessorW)(LPWSTR server, LPWSTR env, LPWSTR path, LPWSTR name)
{
	char *a = kxu_w2a(server), *b = kxu_w2a(env), *c = kxu_w2a(path), *d = kxu_w2a(name);
	BOOL r = AddPrintProcessorA(a, b, c, d);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	kxu_free(d);
	return r;
}

BOOL KXU(DeleteMonitorW)(LPWSTR server, LPWSTR env, LPWSTR name)
{
	char *a = kxu_w2a(server), *b = kxu_w2a(env), *c = kxu_w2a(name);
	BOOL r = DeleteMonitorA(a, b, c);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	return r;
}

BOOL KXU(DeletePrintProcessorW)(LPWSTR server, LPWSTR env, LPWSTR name)
{
	char *a = kxu_w2a(server), *b = kxu_w2a(env), *c = kxu_w2a(name);
	BOOL r = DeletePrintProcessorA(a, b, c);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	return r;
}

BOOL KXU(DeletePrintProvidorW)(LPWSTR server, LPWSTR env, LPWSTR name)
{
	char *a = kxu_w2a(server), *b = kxu_w2a(env), *c = kxu_w2a(name);
	BOOL r = DeletePrintProvidorA(a, b, c);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	return r;
}

BOOL KXU(DeletePrinterDriverW)(LPWSTR server, LPWSTR env, LPWSTR name)
{
	char *a = kxu_w2a(server), *b = kxu_w2a(env), *c = kxu_w2a(name);
	BOOL r = DeletePrinterDriverA(a, b, c);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	return r;
}

static BOOL dir_query(BOOL (WINAPI *fn)(LPSTR, LPSTR, DWORD, LPBYTE, DWORD, LPDWORD), LPWSTR server, LPWSTR env,
                      DWORD level, LPBYTE out, DWORD cb, LPDWORD needed)
{
	char *a = kxu_w2a(server), *b = kxu_w2a(env), dir[MAX_PATH * 2];
	DWORD n = 0, need;
	BOOL r = fn(a, b, level, (LPBYTE) dir, sizeof(dir), &n);
	kxu_free(a);
	kxu_free(b);
	if (!r)
		return FALSE;
	need = (kxu_a2w_len(CP_ACP, dir, -1) + 1) * sizeof(WCHAR);
	if (needed)
		*needed = need;
	if (cb < need || !out)
	{
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	kxu_a2w(CP_ACP, dir, -1, (LPWSTR) out, (int) (cb / sizeof(WCHAR)), NULL);
	return TRUE;
}

BOOL KXU(GetPrinterDriverDirectoryW)(LPWSTR server, LPWSTR env, DWORD level, LPBYTE out, DWORD cb, LPDWORD needed)
{
	return dir_query(GetPrinterDriverDirectoryA, server, env, level, out, cb, needed);
}

BOOL KXU(GetPrintProcessorDirectoryW)(LPWSTR server, LPWSTR env, DWORD level, LPBYTE out, DWORD cb, LPDWORD needed)
{
	return dir_query(GetPrintProcessorDirectoryA, server, env, level, out, cb, needed);
}

DWORD KXU(SetPrinterDataW)(HANDLE h, LPWSTR name, DWORD type, LPBYTE data, DWORD cb)
{
	char *a = kxu_w2a(name), *s = NULL;
	DWORD r;
	if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)
	{
		int alen;
		s = kxu_w2a_n(CP_ACP, (LPCWSTR) data, (int) (cb / sizeof(WCHAR)), &alen);
		data = (LPBYTE) s;
		cb = (DWORD) alen;
	}
	r = SetPrinterDataA(h, a, type, data, cb);
	kxu_free(a);
	kxu_free(s);
	return r;
}

/* ---- device modes ----------------------------------------------------- */

static LONG docprops(HWND hwnd, HANDLE h, LPWSTR dev, PDEVMODEW out, PDEVMODEW in, DWORD mode, BOOL adv)
{
	char *d = kxu_w2a(dev);
	DEVMODEA *ain = kxu_devmode_w2a(in), *aout = NULL;
	LONG size, r;
	size = DocumentPropertiesA(hwnd, h, d, NULL, NULL, 0);
	if (size <= 0)
	{
		kxu_free(d);
		kxu_free(ain);
		return size;
	}
	if (!(mode & ~DM_IN_PROMPT & 0xFFFF) && !adv)   /* size query */
	{
		kxu_free(d);
		kxu_free(ain);
		return size + 64;
	}
	if (out || adv)
		aout = (DEVMODEA *) kxu_alloc(size);
	if (adv)
		r = AdvancedDocumentPropertiesA(hwnd, h, d, aout, ain);
	else
		r = DocumentPropertiesA(hwnd, h, d, aout, ain, mode);
	if (r > 0 && aout && out)
		kxu_devmode_a2w(aout, out, aout->dmSize + 64 + aout->dmDriverExtra);
	kxu_free(d);
	kxu_free(ain);
	kxu_free(aout);
	return r;
}

LONG KXU(DocumentPropertiesW)(HWND hwnd, HANDLE h, LPWSTR dev, PDEVMODEW out, PDEVMODEW in, DWORD mode)
{
	if (mode == 0)
	{
		char *d = kxu_w2a(dev);
		LONG size = DocumentPropertiesA(hwnd, h, d, NULL, NULL, 0);
		kxu_free(d);
		return size > 0 ? size + 64 : size;
	}
	return docprops(hwnd, h, dev, out, in, mode, FALSE);
}

LONG KXU(AdvancedDocumentPropertiesW)(HWND hwnd, HANDLE h, LPWSTR dev, PDEVMODEW out, PDEVMODEW in)
{
	return docprops(hwnd, h, dev, out, in, 0, TRUE);
}

/* string capabilities and their per-entry sizes in characters */
static int cap_strlen(WORD cap)
{
	switch (cap)
	{
	case DC_PAPERNAMES: return 64;
	case DC_BINNAMES: return 24;
	case DC_FILEDEPENDENCIES: return 64;
	case 0x0021 /* DC_MEDIAREADY */: return 64;
	case 0x0023 /* DC_MEDIATYPENAMES */: return 64;
	case 0x0020 /* DC_PERSONALITY */: return 32;
	default: return 0;
	}
}

int KXU(DeviceCapabilitiesW)(LPCWSTR dev, LPCWSTR port, WORD cap, LPWSTR out, const DEVMODEW *dm)
{
	char *d = kxu_w2a(dev), *p = kxu_w2a(port), *buf = NULL;
	DEVMODEA *adm = kxu_devmode_w2a(dm);
	int each = cap_strlen(cap), r;
	if (cap == DC_SIZE || cap == DC_EXTRA)
	{
		r = DeviceCapabilitiesA(d, p, cap, NULL, adm);
		if (cap == DC_SIZE && r > 0)
			r += 64;
	}
	else if (each && out)
	{
		r = DeviceCapabilitiesA(d, p, cap, NULL, adm);
		if (r > 0 && (buf = (char *) kxu_alloc(r * each + 1)))
		{
			int i;
			r = DeviceCapabilitiesA(d, p, cap, buf, adm);
			for (i = 0; i < r; i++)
			{
				char tmp[65];
				CopyMemory(tmp, buf + i * each, each);
				tmp[each] = 0;
				kxu_a2w(CP_ACP, tmp, -1, out + i * each, each, NULL);
			}
		}
	}
	else
		r = DeviceCapabilitiesA(d, p, cap, (LPSTR) out, adm);
	kxu_free(buf);
	kxu_free(d);
	kxu_free(p);
	kxu_free(adm);
	return r;
}
