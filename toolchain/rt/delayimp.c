/*
 * Delay-load helper for modules linked with lld's --delayload (the role of
 * delayimp.lib in the Visual C++ build). Version 2 descriptors: all fields
 * are RVAs. Like the Microsoft helper, a missing DLL or function raises a
 * structured exception rather than returning.
 */
#include <windows.h>

typedef struct
{
	DWORD grAttrs;      /* 1 = RVAs */
	DWORD rvaDLLName;
	DWORD rvaHmod;
	DWORD rvaIAT;
	DWORD rvaINT;
	DWORD rvaBoundIAT;
	DWORD rvaUnloadIAT;
	DWORD dwTimeStamp;
} KexDelayDescr;

extern IMAGE_DOS_HEADER __ImageBase;

#define KEX_DELAY_EXC(code) (0xC0000000 | (0x6D << 16) | (code)) /* "VcppException" */

FARPROC WINAPI __delayLoadHelper2(const KexDelayDescr* d, FARPROC* slot)
{
	BYTE* base = (BYTE*) &__ImageBase;
	HMODULE* phmod = (HMODULE*) (base + d->rvaHmod);
	IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*) (base + d->rvaIAT);
	IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*) (base + d->rvaINT);
	DWORD index = (DWORD) ((IMAGE_THUNK_DATA*) slot - iat);
	HMODULE hmod = *phmod;
	FARPROC proc;
	LPCSTR name;

	if (!hmod)
	{
		hmod = LoadLibraryA((LPCSTR) (base + d->rvaDLLName));
		if (!hmod)
			RaiseException(KEX_DELAY_EXC(ERROR_MOD_NOT_FOUND), 0, 0, NULL);
		*phmod = hmod; /* a racing thread stores the same handle; the extra reference is harmless */
	}

	if (names[index].u1.Ordinal & IMAGE_ORDINAL_FLAG)
		name = (LPCSTR) (names[index].u1.Ordinal & 0xFFFF);
	else
		name = (LPCSTR) ((IMAGE_IMPORT_BY_NAME*) (base + names[index].u1.AddressOfData))->Name;

	proc = GetProcAddress(hmod, name);
	if (!proc)
		RaiseException(KEX_DELAY_EXC(ERROR_PROC_NOT_FOUND), 0, 0, NULL);
	*slot = proc;
	return proc;
}
