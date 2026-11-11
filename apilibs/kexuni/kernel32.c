/*
 *  KernelExEx - kexuni.dll: KERNEL32 wide functions. GPLv2.
 */
#include "uni.h"

#define NAMED_CREATE(fn, call)                                                                                         \
	char *a = kxu_w2a(name);                                                                                       \
	HANDLE h;                                                                                                      \
	h = call;                                                                                                      \
	kxu_free(a);                                                                                                   \
	return h

HANDLE KXU(CreateEventW)(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{
	NAMED_CREATE(CreateEventA, CreateEventA(sa, manual, initial, a));
}

HANDLE KXU(CreateMutexW)(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{
	NAMED_CREATE(CreateMutexA, CreateMutexA(sa, owner, a));
}

HANDLE KXU(CreateSemaphoreW)(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCWSTR name)
{
	NAMED_CREATE(CreateSemaphoreA, CreateSemaphoreA(sa, initial, max, a));
}

HANDLE KXU(CreateWaitableTimerW)(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name)
{
	NAMED_CREATE(CreateWaitableTimerA, CreateWaitableTimerA(sa, manual, a));
}

HANDLE KXU(CreateFileMappingW)(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name)
{
	NAMED_CREATE(CreateFileMappingA, CreateFileMappingA(file, sa, prot, hi, lo, a));
}

HANDLE KXU(CreateMailslotW)(LPCWSTR name, DWORD maxmsg, DWORD timeout, LPSECURITY_ATTRIBUTES sa)
{
	NAMED_CREATE(CreateMailslotA, CreateMailslotA(a, maxmsg, timeout, sa));
}

HANDLE KXU(OpenEventW)(DWORD access, BOOL inherit, LPCWSTR name)
{
	NAMED_CREATE(OpenEventA, OpenEventA(access, inherit, a));
}

HANDLE KXU(OpenMutexW)(DWORD access, BOOL inherit, LPCWSTR name)
{
	NAMED_CREATE(OpenMutexA, OpenMutexA(access, inherit, a));
}

HANDLE KXU(OpenSemaphoreW)(DWORD access, BOOL inherit, LPCWSTR name)
{
	NAMED_CREATE(OpenSemaphoreA, OpenSemaphoreA(access, inherit, a));
}

HANDLE KXU(OpenWaitableTimerW)(DWORD access, BOOL inherit, LPCWSTR name)
{
	NAMED_CREATE(OpenWaitableTimerA, OpenWaitableTimerA(access, inherit, a));
}

HANDLE KXU(OpenFileMappingW)(DWORD access, BOOL inherit, LPCWSTR name)
{
	NAMED_CREATE(OpenFileMappingA, OpenFileMappingA(access, inherit, a));
}

/* ---- named pipes (client side on 9x) ---------------------------------- */

BOOL KXU(CallNamedPipeW)(LPCWSTR name, LPVOID in, DWORD inlen, LPVOID out, DWORD outlen, LPDWORD read, DWORD timeout)
{
	char *a = kxu_w2a(name);
	BOOL r = CallNamedPipeA(a, in, inlen, out, outlen, read, timeout);
	kxu_free(a);
	return r;
}

BOOL KXU(WaitNamedPipeW)(LPCWSTR name, DWORD timeout)
{
	char *a = kxu_w2a(name);
	BOOL r = WaitNamedPipeA(a, timeout);
	kxu_free(a);
	return r;
}

BOOL KXU(GetNamedPipeHandleStateW)(HANDLE h, LPDWORD state, LPDWORD inst, LPDWORD maxc, LPDWORD timeout, LPWSTR user,
                                   DWORD cch)
{
	char buf[256];
	BOOL r = GetNamedPipeHandleStateA(h, state, inst, maxc, timeout, user ? buf : NULL, sizeof(buf));
	if (r && user)
		kxu_a2w(CP_ACP, buf, -1, user, (int) cch, NULL);
	return r;
}

/* ---- communications --------------------------------------------------- */

BOOL KXU(BuildCommDCBW)(LPCWSTR def, LPDCB dcb)
{
	char *a = kxu_w2a(def);
	BOOL r = BuildCommDCBA(a, dcb);
	kxu_free(a);
	return r;
}

BOOL KXU(BuildCommDCBAndTimeoutsW)(LPCWSTR def, LPDCB dcb, LPCOMMTIMEOUTS to)
{
	char *a = kxu_w2a(def);
	BOOL r = BuildCommDCBAndTimeoutsA(a, dcb, to);
	kxu_free(a);
	return r;
}

BOOL KXU(CommConfigDialogW)(LPCWSTR name, HWND hwnd, LPCOMMCONFIG cc)
{
	char *a = kxu_w2a(name);
	BOOL r = CommConfigDialogA(a, hwnd, cc);
	kxu_free(a);
	return r;
}

BOOL KXU(SetDefaultCommConfigW)(LPCWSTR name, LPCOMMCONFIG cc, DWORD size)
{
	char *a = kxu_w2a(name);
	BOOL r = SetDefaultCommConfigA(a, cc, size);
	kxu_free(a);
	return r;
}

/* ---- processes and environment ---------------------------------------- */

static char *env_block_w2a(LPCWSTR w)
{
	return kxu_w2a_n(CP_ACP, w, (int) kxu_multi_len_w(w), NULL);
}

BOOL KXU(CreateProcessW)(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES psa, LPSECURITY_ATTRIBUTES tsa, BOOL inherit,
                         DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW siw, LPPROCESS_INFORMATION pi)
{
	STARTUPINFOA sia;
	char *aapp = kxu_w2a(app), *acmd = kxu_w2a(cmd), *adir = kxu_w2a(dir);
	char *desk = NULL, *title = NULL, *aenv = NULL;
	BOOL r;

	CopyMemory(&sia, siw, sizeof(sia));
	sia.cb = sizeof(sia);
	sia.lpReserved = NULL;
	sia.lpDesktop = desk = kxu_w2a(siw->lpDesktop);
	sia.lpTitle = title = kxu_w2a(siw->lpTitle);
	if (env && (flags & CREATE_UNICODE_ENVIRONMENT))
	{
		aenv = env_block_w2a((LPCWSTR) env);
		env = aenv;
		flags &= ~CREATE_UNICODE_ENVIRONMENT;
	}
	r = CreateProcessA(aapp, acmd, psa, tsa, inherit, flags, env, adir, &sia, pi);
	kxu_free(aapp);
	kxu_free(acmd);
	kxu_free(adir);
	kxu_free(desk);
	kxu_free(title);
	kxu_free(aenv);
	return r;
}

DWORD KXU(GetEnvironmentVariableW)(LPCWSTR name, LPWSTR buf, DWORD cch)
{
	char *a = kxu_w2a(name);
	DWORD n = GetEnvironmentVariableA(a, NULL, 0), r = 0;
	if (n)
	{
		char *v = (char *) kxu_alloc(n + 1);
		if (v && GetEnvironmentVariableA(a, v, n + 1) < n + 1)
			r = kxu_ret_buf(v, buf, cch);
		kxu_free(v);
	}
	kxu_free(a);
	return r;
}

BOOL KXU(SetEnvironmentVariableW)(LPCWSTR name, LPCWSTR value)
{
	char *a = kxu_w2a(name), *v = kxu_w2a(value);
	BOOL r = SetEnvironmentVariableA(a, v);
	kxu_free(a);
	kxu_free(v);
	return r;
}

DWORD KXU(ExpandEnvironmentStringsW)(LPCWSTR src, LPWSTR dst, DWORD cch)
{
	char *a = kxu_w2a(src);
	DWORD n = ExpandEnvironmentStringsA(a, NULL, 0), r = 0;
	if (n)
	{
		char *v = (char *) kxu_alloc(n + 1);
		if (v && ExpandEnvironmentStringsA(a, v, n + 1))
		{
			/* this one counts the NUL in both cases */
			int need;
			kxu_a2w(CP_ACP, v, -1, dst, (int) cch, &need);
			r = need + 1;
		}
		kxu_free(v);
	}
	kxu_free(a);
	return r;
}

void KXU(FatalAppExitW)(UINT action, LPCWSTR msg)
{
	char *a = kxu_w2a(msg);
	FatalAppExitA(action, a);
	kxu_free(a);
}

/* ---- FormatMessage ---------------------------------------------------- */

DWORD KXU(FormatMessageW)(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPWSTR buf, DWORD size, va_list *args)
{
	char *asrc = NULL, *out = NULL;
	DWORD_PTR *conv = NULL;
	DWORD n, r = 0;
	DWORD_PTR *orig = (DWORD_PTR *) args;
	int i, cnt = 0;
	LPCVOID s = src;

	if (flags & FORMAT_MESSAGE_FROM_STRING)
		s = asrc = kxu_w2a((LPCWSTR) src);
	/* Inserts given as an argument array: convert each one that points at a
	 * wide string, so %1 .. %99 (type s by default) come out right. Numbers
	 * pass through. A va_list of wide strings cannot be converted without
	 * parsing every insert's type, which this does not attempt. */
	if (args && (flags & FORMAT_MESSAGE_ARGUMENT_ARRAY) && !(flags & FORMAT_MESSAGE_IGNORE_INSERTS))
	{
		DWORD_PTR *in = (DWORD_PTR *) args;
		const char *f = NULL;
		char *sys = NULL;
		if (flags & FORMAT_MESSAGE_FROM_STRING)
			f = (const char *) s;
		else if (FormatMessageA((flags & ~FORMAT_MESSAGE_ARGUMENT_ARRAY) | FORMAT_MESSAGE_IGNORE_INSERTS |
		                            FORMAT_MESSAGE_ALLOCATE_BUFFER,
		                        s, id, lang, (LPSTR) &sys, 0, NULL))
			f = sys;
		for (; f && *f; f++)
			if (f[0] == '%' && f[1] >= '1' && f[1] <= '9')
			{
				int k = f[1] - '0';
				if (f[2] >= '0' && f[2] <= '9')
					k = k * 10 + (f[2] - '0');
				if (k > cnt)
					cnt = k;
			}
		if (sys)
			LocalFree(sys);
		if (cnt)
		{
			conv = (DWORD_PTR *) kxu_alloc(cnt * sizeof(DWORD_PTR));
			for (i = 0; conv && i < cnt; i++)
			{
				conv[i] = in[i];
				if (in[i] > 0xFFFF && !kxu_bad_wstr((LPCWSTR) in[i], 32767))
					conv[i] = (DWORD_PTR) kxu_w2a((LPCWSTR) in[i]);
			}
			if (conv)
				args = (va_list *) conv;
		}
	}
	n = FormatMessageA(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER, s, id, lang, (LPSTR) &out, 0, args);
	if (n && out)
	{
		int need = kxu_a2w_len(CP_ACP, out, (int) n);
		if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
		{
			LPWSTR w = (LPWSTR) LocalAlloc(LMEM_FIXED, (need + 1) * sizeof(WCHAR));
			if (w)
			{
				kxu_a2w(CP_ACP, out, (int) n, w, need + 1, NULL);
				*(LPWSTR *) buf = w;
				r = need;
			}
		}
		else if ((DWORD) need < size)
		{
			kxu_a2w(CP_ACP, out, (int) n, buf, (int) size, NULL);
			r = need;
		}
		else
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
		LocalFree(out);
	}
	if (conv)
	{
		for (i = 0; i < cnt; i++)
			if (conv[i] != orig[i])
				kxu_free((void *) conv[i]);
		kxu_free(conv);
	}
	kxu_free(asrc);
	return r;
}

/* ---- national language support ---------------------------------------- */

#define NLS_FMT(call_a)                                                                                                \
	int n = call_a(lcid, flags, src, fmt_a, NULL, 0);                                                              \
	int r = 0;                                                                                                     \
	if (n)                                                                                                         \
	{                                                                                                              \
		char *o = (char *) kxu_alloc(n);                                                                       \
		if (o && call_a(lcid, flags, src, fmt_a, o, n))                                                        \
		{                                                                                                      \
			int need;                                                                                      \
			if (!cch)                                                                                      \
				r = kxu_a2w_len(CP_ACP, o, -1) + 1;                                                    \
			else                                                                                           \
			{                                                                                              \
				kxu_a2w(CP_ACP, o, -1, buf, cch, &need);                                               \
				if (need < cch)                                                                        \
					r = need + 1;                                                                  \
				else                                                                                   \
					SetLastError(ERROR_INSUFFICIENT_BUFFER);                                       \
			}                                                                                              \
		}                                                                                                      \
		kxu_free(o);                                                                                           \
	}

int KXU(GetDateFormatW)(LCID lcid, DWORD flags, const SYSTEMTIME *src, LPCWSTR fmt, LPWSTR buf, int cch)
{
	char *fmt_a = kxu_w2a(fmt);
	NLS_FMT(GetDateFormatA)
	kxu_free(fmt_a);
	return r;
}

int KXU(GetTimeFormatW)(LCID lcid, DWORD flags, const SYSTEMTIME *src, LPCWSTR fmt, LPWSTR buf, int cch)
{
	char *fmt_a = kxu_w2a(fmt);
	NLS_FMT(GetTimeFormatA)
	kxu_free(fmt_a);
	return r;
}

static void numfmt_w2a(const NUMBERFMTW *w, NUMBERFMTA *a, char **d, char **t)
{
	CopyMemory(a, w, sizeof(*a));
	a->lpDecimalSep = *d = kxu_w2a(w->lpDecimalSep);
	a->lpThousandSep = *t = kxu_w2a(w->lpThousandSep);
}

int KXU(GetNumberFormatW)(LCID lcid, DWORD flags, LPCWSTR value, const NUMBERFMTW *fmt, LPWSTR buf, int cch)
{
	char *src = kxu_w2a(value), *d = NULL, *t = NULL;
	NUMBERFMTA fa, *fmt_a = NULL;
	if (fmt)
	{
		numfmt_w2a(fmt, &fa, &d, &t);
		fmt_a = &fa;
	}
	{
		NLS_FMT(GetNumberFormatA)
		kxu_free(src);
		kxu_free(d);
		kxu_free(t);
		return r;
	}
}

int KXU(GetCurrencyFormatW)(LCID lcid, DWORD flags, LPCWSTR value, const CURRENCYFMTW *fmt, LPWSTR buf, int cch)
{
	char *src = kxu_w2a(value), *d = NULL, *t = NULL, *c = NULL;
	CURRENCYFMTA fa, *fmt_a = NULL;
	if (fmt)
	{
		CopyMemory(&fa, fmt, sizeof(fa));
		fa.lpDecimalSep = d = kxu_w2a(fmt->lpDecimalSep);
		fa.lpThousandSep = t = kxu_w2a(fmt->lpThousandSep);
		fa.lpCurrencySymbol = c = kxu_w2a(fmt->lpCurrencySymbol);
		fmt_a = &fa;
	}
	{
		NLS_FMT(GetCurrencyFormatA)
		kxu_free(src);
		kxu_free(d);
		kxu_free(t);
		kxu_free(c);
		return r;
	}
}

int KXU(GetCalendarInfoW)(LCID lcid, CALID cal, CALTYPE type, LPWSTR buf, int cch, LPDWORD value)
{
	char o[256];
	int n;
	if (type & CAL_RETURN_NUMBER)
		return GetCalendarInfoA(lcid, cal, type, NULL, 0, value);
	n = GetCalendarInfoA(lcid, cal, type, o, sizeof(o), NULL);
	if (!n)
		return 0;
	if (!cch)
		return kxu_a2w_len(CP_ACP, o, -1) + 1;
	{
		int need;
		kxu_a2w(CP_ACP, o, -1, buf, cch, &need);
		if (need >= cch)
		{
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		return need + 1;
	}
}

BOOL KXU(SetCalendarInfoW)(LCID lcid, CALID cal, CALTYPE type, LPCWSTR data)
{
	char *a = kxu_w2a(data);
	BOOL r = SetCalendarInfoA(lcid, cal, type, a);
	kxu_free(a);
	return r;
}

BOOL KXU(SetLocaleInfoW)(LCID lcid, LCTYPE type, LPCWSTR data)
{
	char *a = kxu_w2a(data);
	BOOL r = SetLocaleInfoA(lcid, type, a);
	kxu_free(a);
	return r;
}

BOOL KXU(GetCPInfoExW)(UINT cp, DWORD flags, LPCPINFOEXW info)
{
	CPINFOEXA a;
	if (!GetCPInfoExA(cp, flags, &a))
		return FALSE;
	info->MaxCharSize = a.MaxCharSize;
	CopyMemory(info->DefaultChar, a.DefaultChar, sizeof(info->DefaultChar));
	CopyMemory(info->LeadByte, a.LeadByte, sizeof(info->LeadByte));
	info->UnicodeDefaultChar = a.UnicodeDefaultChar;
	info->CodePage = a.CodePage;
	kxu_a2w(CP_ACP, a.CodePageName, -1, info->CodePageName, MAX_PATH, NULL);
	return TRUE;
}

/* enumeration callbacks: one thread-local slot per kind is enough because
 * the A enumerators call back synchronously on the calling thread */
static DWORD tls_cb = TLS_OUT_OF_INDEXES;

typedef struct
{
	FARPROC wcb;
	CALID cal;
} ENUMCTX;

static ENUMCTX *ctx_get(void)
{
	return tls_cb == TLS_OUT_OF_INDEXES ? NULL : (ENUMCTX *) TlsGetValue(tls_cb);
}

static ENUMCTX *ctx_push(ENUMCTX *c)
{
	ENUMCTX *prev;
	if (tls_cb == TLS_OUT_OF_INDEXES)
		tls_cb = TlsAlloc();
	prev = ctx_get();
	TlsSetValue(tls_cb, c);
	return prev;
}

static void ctx_pop(ENUMCTX *prev)
{
	TlsSetValue(tls_cb, prev);
}

static BOOL call_str_cb(LPSTR s)
{
	ENUMCTX *c = ctx_get();
	LPWSTR w = kxu_a2w_new(CP_ACP, s);
	BOOL r = w ? ((BOOL(CALLBACK *)(LPWSTR)) c->wcb)(w) : FALSE;
	kxu_free(w);
	return r;
}

static BOOL CALLBACK str_cb(LPSTR s)
{
	return call_str_cb(s);
}

static BOOL CALLBACK str_ex_cb(LPSTR s, CALID cal)
{
	ENUMCTX *c = ctx_get();
	LPWSTR w = kxu_a2w_new(CP_ACP, s);
	BOOL r = w ? ((BOOL(CALLBACK *)(LPWSTR, CALID)) c->wcb)(w, cal) : FALSE;
	kxu_free(w);
	return r;
}

#define ENUM_WRAP(callA, cbtype)                                                                                       \
	ENUMCTX c, *prev;                                                                                              \
	BOOL r;                                                                                                        \
	c.wcb = (FARPROC) cb;                                                                                          \
	prev = ctx_push(&c);                                                                                           \
	r = callA;                                                                                                     \
	ctx_pop(prev);                                                                                                 \
	return r

BOOL KXU(EnumCalendarInfoW)(CALINFO_ENUMPROCW cb, LCID lcid, CALID cal, CALTYPE type)
{
	ENUM_WRAP(EnumCalendarInfoA((CALINFO_ENUMPROCA) str_cb, lcid, cal, type), 0);
}

BOOL KXU(EnumCalendarInfoExW)(CALINFO_ENUMPROCEXW cb, LCID lcid, CALID cal, CALTYPE type)
{
	ENUM_WRAP(EnumCalendarInfoExA((CALINFO_ENUMPROCEXA) str_ex_cb, lcid, cal, type), 0);
}

BOOL KXU(EnumDateFormatsW)(DATEFMT_ENUMPROCW cb, LCID lcid, DWORD flags)
{
	ENUM_WRAP(EnumDateFormatsA((DATEFMT_ENUMPROCA) str_cb, lcid, flags), 0);
}

BOOL KXU(EnumDateFormatsExW)(DATEFMT_ENUMPROCEXW cb, LCID lcid, DWORD flags)
{
	ENUM_WRAP(EnumDateFormatsExA((DATEFMT_ENUMPROCEXA) str_ex_cb, lcid, flags), 0);
}

BOOL KXU(EnumTimeFormatsW)(TIMEFMT_ENUMPROCW cb, LCID lcid, DWORD flags)
{
	ENUM_WRAP(EnumTimeFormatsA((TIMEFMT_ENUMPROCA) str_cb, lcid, flags), 0);
}

BOOL KXU(EnumSystemLocalesW)(LOCALE_ENUMPROCW cb, DWORD flags)
{
	ENUM_WRAP(EnumSystemLocalesA((LOCALE_ENUMPROCA) str_cb, flags), 0);
}

BOOL KXU(EnumSystemCodePagesW)(CODEPAGE_ENUMPROCW cb, DWORD flags)
{
	ENUM_WRAP(EnumSystemCodePagesA((CODEPAGE_ENUMPROCA) str_cb, flags), 0);
}

/* ---- system information ----------------------------------------------- */

BOOL KXU(GetComputerNameW)(LPWSTR buf, LPDWORD size)
{
	char a[MAX_COMPUTERNAME_LENGTH * 2 + 2];
	DWORD n = sizeof(a);
	int need;
	if (!GetComputerNameA(a, &n))
		return FALSE;
	kxu_a2w(CP_ACP, a, -1, buf, (int) *size, &need);
	if ((DWORD) need >= *size)
	{
		*size = need + 1;
		SetLastError(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}
	*size = need;
	return TRUE;
}

BOOL KXU(SetComputerNameW)(LPCWSTR name)
{
	char *a = kxu_w2a(name);
	BOOL r = SetComputerNameA(a);
	kxu_free(a);
	return r;
}

UINT KXU(GetDriveTypeW)(LPCWSTR root)
{
	char *a = kxu_w2a(root);
	UINT r = GetDriveTypeA(a);
	kxu_free(a);
	return r;
}

DWORD KXU(GetLogicalDriveStringsW)(DWORD cch, LPWSTR buf)
{
	char a[26 * 4 + 1];
	DWORD n = GetLogicalDriveStringsA(sizeof(a), a), i;
	if (!n || n > sizeof(a))
		return n;
	if (n + 1 > cch)
		return n + 1;
	for (i = 0; i <= n; i++)
		buf[i] = (BYTE) a[i];   /* drive strings are ASCII */
	return n;
}

BOOL KXU(GetVolumeInformationW)(LPCWSTR root, LPWSTR vol, DWORD volcch, LPDWORD serial, LPDWORD maxlen, LPDWORD flags,
                                LPWSTR fs, DWORD fscch)
{
	char *a = kxu_w2a(root);
	char av[MAX_PATH + 1], af[MAX_PATH + 1];
	BOOL r = GetVolumeInformationA(a, vol ? av : NULL, sizeof(av), serial, maxlen, flags, fs ? af : NULL, sizeof(af));
	kxu_free(a);
	if (r)
	{
		if (vol)
			kxu_a2w(CP_ACP, av, -1, vol, (int) volcch, NULL);
		if (fs)
			kxu_a2w(CP_ACP, af, -1, fs, (int) fscch, NULL);
	}
	return r;
}

BOOL KXU(SetVolumeLabelW)(LPCWSTR root, LPCWSTR label)
{
	char *a = kxu_w2a(root), *l = kxu_w2a(label);
	BOOL r = SetVolumeLabelA(a, l);
	kxu_free(a);
	kxu_free(l);
	return r;
}

DWORD KXU(QueryDosDeviceW)(LPCWSTR dev, LPWSTR buf, DWORD cch)
{
	char *a = kxu_w2a(dev);
	char *o = (char *) kxu_alloc(cch * 2 + 2);
	DWORD n = o ? QueryDosDeviceA(a, o, cch * 2 + 2) : 0, r = 0;
	if (n)
	{
		int need = kxu_a2w_len(CP_ACP, o, (int) n);
		if ((DWORD) need <= cch)
		{
			MultiByteToWideChar(CP_ACP, 0, o, (int) n, buf, need);
			r = need;
		}
		else
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
	}
	kxu_free(o);
	kxu_free(a);
	return r;
}

/* kexuni must not call any function KernelEx forwards to it (the build
 * checks): with KernelEx active, IsBadStringPtrW would come straight back
 * here. Probe one page at a time with IsBadReadPtr instead. */
BOOL kxu_bad_wstr(LPCWSTR s, UINT_PTR max)
{
	const BYTE *p = (const BYTE *) s;
	UINT_PTR i;
	if (!max)
		return FALSE;
	if (!s)
		return TRUE;
	for (i = 0; i < max; i++, p += 2)
	{
		if (i == 0 || (((ULONG_PTR) p) & 0xFFF) == 0 || (((ULONG_PTR) p) & 0xFFF) == 0xFFF)
			if (IsBadReadPtr(p, 2))
				return TRUE;
		if (!*(const WCHAR *) p)
			return FALSE;
	}
	return FALSE;
}

BOOL KXU(IsBadStringPtrW)(LPCWSTR s, UINT_PTR max)
{
	return kxu_bad_wstr(s, max);
}

/* ---- profile (INI) files ---------------------------------------------- */

UINT KXU(GetPrivateProfileIntW)(LPCWSTR sec, LPCWSTR key, INT def, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *k = kxu_w2a(key), *f = kxu_w2a(file);
	UINT r = GetPrivateProfileIntA(s, k, def, f);
	kxu_free(s);
	kxu_free(k);
	kxu_free(f);
	return r;
}

UINT KXU(GetProfileIntW)(LPCWSTR sec, LPCWSTR key, INT def)
{
	return kxu_GetPrivateProfileIntW(sec, key, def, L"win.ini");
}

/* A results may be lists (several NUL-terminated strings); convert all of
 * them and apply the documented truncation rules in characters */
static DWORD profile_out(const char *o, DWORD n, LPWSTR buf, DWORD cch, BOOL list)
{
	int need = kxu_a2w_len(CP_ACP, o, (int) n);
	if (!cch)
		return 0;
	if ((DWORD) need < cch - (list ? 1 : 0))
	{
		if (need)
			MultiByteToWideChar(CP_ACP, 0, o, (int) n, buf, need);
		buf[need] = 0;
		if (list)
			buf[need + 1] = 0;
		return need;
	}
	/* truncated: cch - 1 (string) or cch - 2 (list) */
	{
		LPWSTR t = (LPWSTR) kxu_alloc((need + 1) * sizeof(WCHAR));
		DWORD keep = cch > (list ? 2u : 1u) ? cch - (list ? 2 : 1) : 0;
		if (t)
		{
			MultiByteToWideChar(CP_ACP, 0, o, (int) n, t, need);
			CopyMemory(buf, t, keep * sizeof(WCHAR));
			kxu_free(t);
		}
		buf[keep] = 0;
		if (list && keep + 1 < cch)
			buf[keep + 1] = 0;
		return keep;
	}
}

DWORD KXU(GetPrivateProfileStringW)(LPCWSTR sec, LPCWSTR key, LPCWSTR def, LPWSTR buf, DWORD cch, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *k = kxu_w2a(key), *d = kxu_w2a(def), *f = kxu_w2a(file);
	DWORD asz = cch * 2 + 2;
	char *o = (char *) kxu_alloc(asz);
	DWORD n = o ? GetPrivateProfileStringA(s, k, d, o, asz, f) : 0, r = 0;
	if (o)
		r = profile_out(o, n, buf, cch, !s || !k);
	kxu_free(o);
	kxu_free(s);
	kxu_free(k);
	kxu_free(d);
	kxu_free(f);
	return r;
}

DWORD KXU(GetProfileStringW)(LPCWSTR sec, LPCWSTR key, LPCWSTR def, LPWSTR buf, DWORD cch)
{
	return kxu_GetPrivateProfileStringW(sec, key, def, buf, cch, L"win.ini");
}

DWORD KXU(GetPrivateProfileSectionW)(LPCWSTR sec, LPWSTR buf, DWORD cch, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *f = kxu_w2a(file);
	DWORD asz = cch * 2 + 2;
	char *o = (char *) kxu_alloc(asz);
	DWORD n = o ? GetPrivateProfileSectionA(s, o, asz, f) : 0, r = 0;
	if (o)
		r = profile_out(o, n, buf, cch, TRUE);
	kxu_free(o);
	kxu_free(s);
	kxu_free(f);
	return r;
}

DWORD KXU(GetProfileSectionW)(LPCWSTR sec, LPWSTR buf, DWORD cch)
{
	return kxu_GetPrivateProfileSectionW(sec, buf, cch, L"win.ini");
}

DWORD KXU(GetPrivateProfileSectionNamesW)(LPWSTR buf, DWORD cch, LPCWSTR file)
{
	char *f = kxu_w2a(file);
	DWORD asz = cch * 2 + 2;
	char *o = (char *) kxu_alloc(asz);
	DWORD n = o ? GetPrivateProfileSectionNamesA(o, asz, f) : 0, r = 0;
	if (o)
		r = profile_out(o, n, buf, cch, TRUE);
	kxu_free(o);
	kxu_free(f);
	return r;
}

BOOL KXU(GetPrivateProfileStructW)(LPCWSTR sec, LPCWSTR key, LPVOID data, UINT size, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *k = kxu_w2a(key), *f = kxu_w2a(file);
	BOOL r = GetPrivateProfileStructA(s, k, data, size, f);
	kxu_free(s);
	kxu_free(k);
	kxu_free(f);
	return r;
}

BOOL KXU(WritePrivateProfileStructW)(LPCWSTR sec, LPCWSTR key, LPVOID data, UINT size, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *k = kxu_w2a(key), *f = kxu_w2a(file);
	BOOL r = WritePrivateProfileStructA(s, k, data, size, f);
	kxu_free(s);
	kxu_free(k);
	kxu_free(f);
	return r;
}

BOOL KXU(WritePrivateProfileStringW)(LPCWSTR sec, LPCWSTR key, LPCWSTR value, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *k = kxu_w2a(key), *v = kxu_w2a(value), *f = kxu_w2a(file);
	BOOL r = WritePrivateProfileStringA(s, k, v, f);
	kxu_free(s);
	kxu_free(k);
	kxu_free(v);
	kxu_free(f);
	return r;
}

BOOL KXU(WriteProfileStringW)(LPCWSTR sec, LPCWSTR key, LPCWSTR value)
{
	char *s = kxu_w2a(sec), *k = kxu_w2a(key), *v = kxu_w2a(value);
	BOOL r = WriteProfileStringA(s, k, v);
	kxu_free(s);
	kxu_free(k);
	kxu_free(v);
	return r;
}

BOOL KXU(WritePrivateProfileSectionW)(LPCWSTR sec, LPCWSTR data, LPCWSTR file)
{
	char *s = kxu_w2a(sec), *d = kxu_w2a_multi(data), *f = kxu_w2a(file);
	BOOL r = WritePrivateProfileSectionA(s, d, f);
	kxu_free(s);
	kxu_free(d);
	kxu_free(f);
	return r;
}

BOOL KXU(WriteProfileSectionW)(LPCWSTR sec, LPCWSTR data)
{
	char *s = kxu_w2a(sec), *d = kxu_w2a_multi(data);
	BOOL r = WriteProfileSectionA(s, d);
	kxu_free(s);
	kxu_free(d);
	return r;
}

/* ---- console ---------------------------------------------------------- */

static UINT out_cp(void)
{
	UINT cp = GetConsoleOutputCP();
	return cp ? cp : CP_OEMCP;
}

static UINT in_cp(void)
{
	UINT cp = GetConsoleCP();
	return cp ? cp : CP_OEMCP;
}

BOOL KXU(WriteConsoleW)(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID reserved)
{
	int alen;
	char *a = kxu_w2a_n(out_cp(), (LPCWSTR) buf, (int) n, &alen);
	DWORD w = 0;
	BOOL r = a ? WriteConsoleA(h, a, (DWORD) alen, &w, reserved) : FALSE;
	if (written)
		*written = r ? (w == (DWORD) alen ? n : (DWORD) kxu_wchars_in(out_cp(), a, (int) w)) : 0;
	kxu_free(a);
	return r;
}

BOOL KXU(ReadConsoleW)(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID ctrl)
{
	char *a = (char *) kxu_alloc(n * 2 + 1);
	DWORD got = 0;
	BOOL r = a ? ReadConsoleA(h, a, n, &got, ctrl) : FALSE;
	if (r)
	{
		int c = got ? MultiByteToWideChar(in_cp(), 0, a, (int) got, (LPWSTR) buf, (int) n) : 0;
		if (read)
			*read = (DWORD) c;
	}
	kxu_free(a);
	return r;
}

BOOL KXU(GetConsoleTitleW)(LPWSTR buf, DWORD cch)
{
	char a[1024];
	DWORD n = GetConsoleTitleA(a, sizeof(a));
	if (!n)
		return 0;
	return (BOOL) kxu_a2w(CP_ACP, a, -1, buf, (int) cch, NULL);
}

BOOL KXU(FillConsoleOutputCharacterW)(HANDLE h, WCHAR c, DWORD n, COORD at, LPDWORD written)
{
	char a[4];
	int len = WideCharToMultiByte(out_cp(), 0, &c, 1, a, sizeof(a), NULL, NULL);
	return FillConsoleOutputCharacterA(h, len ? a[0] : '?', n, at, written);
}

BOOL KXU(WriteConsoleOutputCharacterW)(HANDLE h, LPCWSTR s, DWORD n, COORD at, LPDWORD written)
{
	int alen;
	char *a = kxu_w2a_n(out_cp(), s, (int) n, &alen);
	DWORD w = 0;
	BOOL r = a ? WriteConsoleOutputCharacterA(h, a, (DWORD) alen, at, &w) : FALSE;
	if (written)
		*written = r ? (DWORD) kxu_wchars_in(out_cp(), a, (int) w) : 0;
	kxu_free(a);
	return r;
}

BOOL KXU(ReadConsoleOutputCharacterW)(HANDLE h, LPWSTR s, DWORD n, COORD at, LPDWORD read)
{
	char *a = (char *) kxu_alloc(n + 1);
	DWORD got = 0;
	BOOL r = a ? ReadConsoleOutputCharacterA(h, a, n, at, &got) : FALSE;
	if (r && read)
		*read = got ? (DWORD) MultiByteToWideChar(out_cp(), 0, a, (int) got, s, (int) n) : 0;
	kxu_free(a);
	return r;
}

static void ci_a2w(CHAR_INFO *ci, DWORD count, UINT cp)
{
	DWORD i;
	for (i = 0; i < count; i++)
	{
		CHAR c = ci[i].Char.AsciiChar;
		WCHAR w = (BYTE) c;
		MultiByteToWideChar(cp, 0, &c, 1, &w, 1);
		ci[i].Char.UnicodeChar = w;
	}
}

static void ci_w2a(CHAR_INFO *ci, DWORD count, UINT cp)
{
	DWORD i;
	for (i = 0; i < count; i++)
	{
		char a[4];
		WCHAR w = ci[i].Char.UnicodeChar;
		int len = WideCharToMultiByte(cp, 0, &w, 1, a, sizeof(a), NULL, NULL);
		ci[i].Char.UnicodeChar = 0;
		ci[i].Char.AsciiChar = len == 1 ? a[0] : '?';
	}
}

BOOL KXU(WriteConsoleOutputW)(HANDLE h, const CHAR_INFO *buf, COORD size, COORD at, PSMALL_RECT rect)
{
	DWORD count = (DWORD) size.X * (DWORD) size.Y;
	CHAR_INFO *copy = (CHAR_INFO *) kxu_alloc(count * sizeof(CHAR_INFO));
	BOOL r;
	if (!copy)
		return FALSE;
	CopyMemory(copy, buf, count * sizeof(CHAR_INFO));
	ci_w2a(copy, count, out_cp());
	r = WriteConsoleOutputA(h, copy, size, at, rect);
	kxu_free(copy);
	return r;
}

BOOL KXU(ReadConsoleOutputW)(HANDLE h, PCHAR_INFO buf, COORD size, COORD at, PSMALL_RECT rect)
{
	BOOL r = ReadConsoleOutputA(h, buf, size, at, rect);
	if (r)
		ci_a2w(buf, (DWORD) size.X * (DWORD) size.Y, out_cp());
	return r;
}

BOOL KXU(ScrollConsoleScreenBufferW)(HANDLE h, const SMALL_RECT *scroll, const SMALL_RECT *clip, COORD dest,
                                     const CHAR_INFO *fill)
{
	CHAR_INFO f = *fill;
	ci_w2a(&f, 1, out_cp());
	return ScrollConsoleScreenBufferA(h, scroll, clip, dest, &f);
}

static void ir_a2w(INPUT_RECORD *ir, DWORD n)
{
	DWORD i;
	for (i = 0; i < n; i++)
		if (ir[i].EventType == KEY_EVENT)
		{
			CHAR c = ir[i].Event.KeyEvent.uChar.AsciiChar;
			WCHAR w = (BYTE) c;
			if (c)
				MultiByteToWideChar(in_cp(), 0, &c, 1, &w, 1);
			ir[i].Event.KeyEvent.uChar.UnicodeChar = w;
		}
}

BOOL KXU(PeekConsoleInputW)(HANDLE h, PINPUT_RECORD buf, DWORD n, LPDWORD read)
{
	BOOL r = PeekConsoleInputA(h, buf, n, read);
	if (r)
		ir_a2w(buf, *read);
	return r;
}

BOOL KXU(WriteConsoleInputW)(HANDLE h, const INPUT_RECORD *buf, DWORD n, LPDWORD written)
{
	INPUT_RECORD *copy = (INPUT_RECORD *) kxu_alloc(n * sizeof(INPUT_RECORD));
	DWORD i;
	BOOL r;
	if (!copy)
		return FALSE;
	CopyMemory(copy, buf, n * sizeof(INPUT_RECORD));
	for (i = 0; i < n; i++)
		if (copy[i].EventType == KEY_EVENT)
		{
			char a[4];
			WCHAR w = copy[i].Event.KeyEvent.uChar.UnicodeChar;
			int len = w ? WideCharToMultiByte(in_cp(), 0, &w, 1, a, sizeof(a), NULL, NULL) : 0;
			copy[i].Event.KeyEvent.uChar.UnicodeChar = 0;
			copy[i].Event.KeyEvent.uChar.AsciiChar = len == 1 ? a[0] : 0;
		}
	r = WriteConsoleInputA(h, copy, n, written);
	kxu_free(copy);
	return r;
}
