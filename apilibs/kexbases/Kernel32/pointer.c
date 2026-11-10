/*
 *  KernelEx
 *  Copyright (C) 2010, Xeno86, Tihiy
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

#include "common.h"

BOOL WINAPI SystemFunction036_new(PVOID pbBuffer, ULONG dwLen);

static LONG get_pointer_obfuscator(void)
{
	static LONG obfuscator;

	if (obfuscator == 0)
	{
		LONG rand = 0;
		/* SystemFunction036_new now fails instead of returning guessable
		 * bytes when CryptoAPI is missing; fall back to timing then, since
		 * this cookie must exist. (Still one cookie system-wide: see the
		 * FIXME below; tracked in net/docs/ARCHITECTURE.md.) */
		if (!SystemFunction036_new(&rand, sizeof(rand)))
			rand = (LONG)(GetTickCount() * 2654435761u) ^ (LONG)GetCurrentThreadId();
		rand |= 0xc0000000;
		InterlockedCompareExchange(&obfuscator, rand, 0);
	}

	return obfuscator;
}

/* FIXME: EncodePointer/DecodePointer should use per-process obfuscator */

/* MAKE_EXPORT XorPointer=EncodePointer */
/* MAKE_EXPORT XorPointer=DecodePointer */
/* MAKE_EXPORT XorPointer=EncodeSystemPointer */
/* MAKE_EXPORT XorPointer=DecodeSystemPointer */
PVOID WINAPI XorPointer(PVOID ptr)
{
	LONG ptrval = (LONG) ptr;
	return (PVOID)(ptrval ^ get_pointer_obfuscator());
}
