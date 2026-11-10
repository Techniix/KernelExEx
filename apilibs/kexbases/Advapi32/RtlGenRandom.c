/*
 *  KernelEx
 *  Copyright (C) 2009 Tihiy
 *  Copyright (C) 2026 KernelEx contributors
 *
 *  This file is part of KernelEx source code.
 *
 *  KernelEx is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published
 *  by the Free Software Foundation; version 2 of the License.
 *
 *  KernelEx is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <windows.h>

/*
 * Fallback RtlGenRandom (ADVAPI32!SystemFunction036).
 *
 * The normal implementation is in kextls.dll (net/kextls), which CORE.INI
 * lists after kexbases so that it takes precedence: a per-process
 * SP 800-90A HMAC_DRBG seeded from CryptoAPI and CPU timing jitter.
 *
 * This copy is used only when kextls is absent. It used to be the KISS
 * generator seeded from GetTickCount, the thread and process ids and the
 * desktop colour, with its state in kexbases' shared data section: fast,
 * but predictable, and shared by every process on the machine. It now asks
 * CryptoAPI directly on each call and fails (returns FALSE) when CryptoAPI
 * is unavailable, instead of returning guessable bytes. No state is kept:
 * kexbases is loaded in the shared arena, so a cached provider handle would
 * leak between processes.
 */

#ifndef CRYPT_VERIFYCONTEXT
#define CRYPT_VERIFYCONTEXT 0xF0000000
#endif
#ifndef PROV_RSA_FULL
#define PROV_RSA_FULL 1
#endif

typedef BOOL (WINAPI *CryptAcquireContextA_t)(ULONG_PTR*, LPCSTR, LPCSTR, DWORD, DWORD);
typedef BOOL (WINAPI *CryptGenRandom_t)(ULONG_PTR, DWORD, BYTE*);
typedef BOOL (WINAPI *CryptReleaseContext_t)(ULONG_PTR, DWORD);

/* MAKE_EXPORT SystemFunction036_new=SystemFunction036 */
BOOL WINAPI SystemFunction036_new(PVOID pbBuffer, ULONG dwLen)
{
	HMODULE adv;
	CryptAcquireContextA_t acquire;
	CryptGenRandom_t gen;
	CryptReleaseContext_t release;
	ULONG_PTR prov = 0;
	BOOL ok = FALSE;

	if (!pbBuffer || !dwLen)
		return FALSE;
	adv = LoadLibraryA("ADVAPI32.DLL");
	if (!adv)
		return FALSE;
	acquire = (CryptAcquireContextA_t) GetProcAddress(adv, "CryptAcquireContextA");
	gen = (CryptGenRandom_t) GetProcAddress(adv, "CryptGenRandom");
	release = (CryptReleaseContext_t) GetProcAddress(adv, "CryptReleaseContext");
	if (acquire && gen && release
			&& acquire(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
	{
		ok = gen(prov, dwLen, (BYTE*) pbBuffer);
		release(prov, 0);
	}
	FreeLibrary(adv);
	if (!ok)
		memset(pbBuffer, 0, dwLen);
	return ok;
}
