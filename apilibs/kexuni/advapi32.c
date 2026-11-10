/*
 *  KernelExEx - kexuni.dll: ADVAPI32 wide functions. GPLv2.
 */
#include "uni.h"

BOOL KXU(GetUserNameW)(LPWSTR buf, LPDWORD cch)
{
	char a[512];
	DWORD n = sizeof(a);
	int need;
	if (!GetUserNameA(a, &n))
		return FALSE;
	kxu_a2w(CP_ACP, a, -1, buf, buf ? (int) *cch : 0, &need);
	if (!buf || (DWORD) need + 1 > *cch)
	{
		*cch = need + 1;
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	*cch = need + 1;
	return TRUE;
}

/* Statistical test in the spirit of the Win32 function; tests asked for
 * through *result are answered individually. */
BOOL KXU(IsTextUnicode)(const VOID *buf, int len, LPINT result)
{
	const BYTE *b = (const BYTE *) buf;
	int want = result ? *result : 0xFFFF, got = 0, i, nul_even = 0, nul_odd = 0, ascii = 0, n = len / 2;
	const WCHAR *w = (const WCHAR *) buf;
	if (len < 2)
	{
		if (result)
			*result = 0;
		return FALSE;
	}
	if (w[0] == 0xFEFF)
		got |= IS_TEXT_UNICODE_SIGNATURE;
	if (w[0] == 0xFFFE)
		got |= IS_TEXT_UNICODE_REVERSE_SIGNATURE;
	for (i = 0; i + 1 < len; i += 2)
	{
		if (!b[i])
			nul_even++;
		if (!b[i + 1])
			nul_odd++;
	}
	for (i = 0; i < n; i++)
		if (w[i] < 0x80 && w[i] >= 0x09)
			ascii++;
	if (n && ascii * 10 >= n * 9)
		got |= IS_TEXT_UNICODE_ASCII16;
	if (n && nul_even * 10 >= n * 9 && nul_odd * 10 < n)
		got |= IS_TEXT_UNICODE_REVERSE_ASCII16;
	if (n && nul_odd > n / 3 && nul_even < n / 10)
		got |= IS_TEXT_UNICODE_STATISTICS;
	if (n && nul_even > n / 3 && nul_odd < n / 10)
		got |= IS_TEXT_UNICODE_REVERSE_STATISTICS;
	for (i = 0; i < n; i++)
		if (w[i] == 0)
		{
			got |= IS_TEXT_UNICODE_NULL_BYTES;
			break;
		}
	if (len & 1)
		got |= IS_TEXT_UNICODE_ODD_LENGTH;
	if (result)
		*result = got & want;
	if (got & (IS_TEXT_UNICODE_REVERSE_SIGNATURE | IS_TEXT_UNICODE_REVERSE_ASCII16 |
	           IS_TEXT_UNICODE_REVERSE_STATISTICS | IS_TEXT_UNICODE_ODD_LENGTH))
		return (got & IS_TEXT_UNICODE_SIGNATURE) != 0;
	return (got & (IS_TEXT_UNICODE_SIGNATURE | IS_TEXT_UNICODE_ASCII16 | IS_TEXT_UNICODE_STATISTICS)) != 0;
}

LONG KXU(RegConnectRegistryW)(LPCWSTR machine, HKEY key, PHKEY out)
{
	char *a = kxu_w2a(machine);
	LONG r = RegConnectRegistryA(a, key, out);
	kxu_free(a);
	return r;
}

LONG KXU(RegDeleteValueW)(HKEY key, LPCWSTR name)
{
	char *a = kxu_w2a(name);
	LONG r = RegDeleteValueA(key, a);
	kxu_free(a);
	return r;
}

LONG KXU(RegEnumKeyExW)(HKEY key, DWORD idx, LPWSTR name, LPDWORD namecch, LPDWORD reserved, LPWSTR cls,
                        LPDWORD clscch, PFILETIME ft)
{
	char an[512], ac[512];
	DWORD nl = sizeof(an), cl = sizeof(ac);
	LONG r = RegEnumKeyExA(key, idx, an, &nl, reserved, cls ? ac : NULL, cls ? &cl : NULL, ft);
	int need;
	if (r != ERROR_SUCCESS)
		return r;
	kxu_a2w(CP_ACP, an, -1, name, (int) *namecch, &need);
	if ((DWORD) need >= *namecch)
		return ERROR_MORE_DATA;
	*namecch = need;
	if (cls && clscch)
	{
		kxu_a2w(CP_ACP, ac, -1, cls, (int) *clscch, &need);
		if ((DWORD) need >= *clscch)
			return ERROR_MORE_DATA;
		*clscch = need;
	}
	return ERROR_SUCCESS;
}

LONG KXU(RegEnumKeyW)(HKEY key, DWORD idx, LPWSTR name, DWORD cch)
{
	DWORD n = cch;
	return kxu_RegEnumKeyExW(key, idx, name, &n, NULL, NULL, NULL, NULL);
}

LONG KXU(RegLoadKeyW)(HKEY key, LPCWSTR sub, LPCWSTR file)
{
	char *a = kxu_w2a(sub), *b = kxu_w2a(file);
	LONG r = RegLoadKeyA(key, a, b);
	kxu_free(a);
	kxu_free(b);
	return r;
}

LONG KXU(RegUnLoadKeyW)(HKEY key, LPCWSTR sub)
{
	char *a = kxu_w2a(sub);
	LONG r = RegUnLoadKeyA(key, a);
	kxu_free(a);
	return r;
}

LONG KXU(RegReplaceKeyW)(HKEY key, LPCWSTR sub, LPCWSTR newf, LPCWSTR oldf)
{
	char *a = kxu_w2a(sub), *b = kxu_w2a(newf), *c = kxu_w2a(oldf);
	LONG r = RegReplaceKeyA(key, a, b, c);
	kxu_free(a);
	kxu_free(b);
	kxu_free(c);
	return r;
}

LONG KXU(RegSaveKeyW)(HKEY key, LPCWSTR file, LPSECURITY_ATTRIBUTES sa)
{
	char *a = kxu_w2a(file);
	LONG r = RegSaveKeyA(key, a, sa);
	kxu_free(a);
	return r;
}

LONG KXU(RegQueryInfoKeyW)(HKEY key, LPWSTR cls, LPDWORD clscch, LPDWORD reserved, LPDWORD subkeys, LPDWORD maxsub,
                           LPDWORD maxcls, LPDWORD values, LPDWORD maxname, LPDWORD maxdata, LPDWORD sd,
                           PFILETIME ft)
{
	char ac[512];
	DWORD cl = sizeof(ac);
	/* lengths come back in bytes, an upper bound on characters */
	LONG r = RegQueryInfoKeyA(key, cls ? ac : NULL, cls ? &cl : NULL, reserved, subkeys, maxsub, maxcls, values,
	                          maxname, maxdata, sd, ft);
	if (r == ERROR_SUCCESS && cls && clscch)
	{
		int need;
		kxu_a2w(CP_ACP, ac, -1, cls, (int) *clscch, &need);
		if ((DWORD) need >= *clscch)
			return ERROR_MORE_DATA;
		*clscch = need;
	}
	return r;
}

LONG KXU(RegQueryValueW)(HKEY key, LPCWSTR sub, LPWSTR data, PLONG cb)
{
	char *a = kxu_w2a(sub);
	LONG n = 0, r;
	char *v;
	r = RegQueryValueA(key, a, NULL, &n);
	if (r != ERROR_SUCCESS)
	{
		kxu_free(a);
		return r;
	}
	v = (char *) kxu_alloc(n + 1);
	r = v ? RegQueryValueA(key, a, v, &n) : ERROR_NOT_ENOUGH_MEMORY;
	if (r == ERROR_SUCCESS)
	{
		int need = kxu_a2w_len(CP_ACP, v, -1);
		LONG bytes = (need + 1) * (LONG) sizeof(WCHAR);
		if (data && cb && *cb >= bytes)
			kxu_a2w(CP_ACP, v, -1, data, need + 1, NULL);
		else if (data)
			r = ERROR_MORE_DATA;
		if (cb)
			*cb = bytes;
	}
	kxu_free(v);
	kxu_free(a);
	return r;
}

LONG KXU(RegSetValueW)(HKEY key, LPCWSTR sub, DWORD type, LPCWSTR data, DWORD cb)
{
	char *a = kxu_w2a(sub), *d = kxu_w2a(data);
	LONG r = RegSetValueA(key, a, type, d, d ? (DWORD) lstrlenA(d) : 0);
	(void) cb;
	kxu_free(a);
	kxu_free(d);
	return r;
}

static int is_string_type(DWORD t)
{
	return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ;
}

LONG KXU(RegQueryMultipleValuesW)(HKEY key, PVALENTW list, DWORD num, LPWSTR buf, LPDWORD total)
{
	DWORD i, need = 0;
	LONG r = ERROR_SUCCESS;
	char *p = (char *) buf;
	/* first pass: sizes */
	for (i = 0; i < num; i++)
	{
		char *name = kxu_w2a(list[i].ve_valuename);
		DWORD type, cb = 0;
		LONG q = RegQueryValueExA(key, name, NULL, &type, NULL, &cb);
		char *v;
		if (q != ERROR_SUCCESS)
		{
			kxu_free(name);
			return q;
		}
		v = (char *) kxu_alloc(cb + 2);
		if (!v)
		{
			kxu_free(name);
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		RegQueryValueExA(key, name, NULL, &type, (BYTE *) v, &cb);
		v[cb] = v[cb + 1] = 0;
		list[i].ve_type = type;
		if (is_string_type(type))
		{
			int w = cb ? MultiByteToWideChar(CP_ACP, 0, v, (int) cb, NULL, 0) : 0;
			list[i].ve_valuelen = (DWORD) w * sizeof(WCHAR);
		}
		else
			list[i].ve_valuelen = cb;
		if (buf && total && need + list[i].ve_valuelen <= *total)
		{
			if (is_string_type(type))
			{
				if (cb)
					MultiByteToWideChar(CP_ACP, 0, v, (int) cb, (LPWSTR) p, (int) (list[i].ve_valuelen / 2));
			}
			else
				CopyMemory(p, v, cb);
			list[i].ve_valueptr = (DWORD_PTR) p;
			p += list[i].ve_valuelen;
		}
		else
			r = ERROR_MORE_DATA;
		need += list[i].ve_valuelen;
		kxu_free(v);
		kxu_free(name);
	}
	if (total)
		*total = need;
	return buf ? r : ERROR_SUCCESS;
}
