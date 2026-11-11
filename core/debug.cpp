/*
 *  KernelEx
 *  Copyright (C) 2009, Xeno86
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

#include <stdarg.h>
#include <windows.h>
#include "debug.h"

extern "C"
int vsnprintf(char *buffer, size_t n, const char *format, va_list ap);

void dbgvprintf(const char* format, void* _argp)
{
	char msg[DEBUGMSG_MAXLEN];
	va_list argp = (va_list) _argp;
	int cnt = vsnprintf(msg, sizeof(msg), format, argp);

#ifdef KEX_DEBUGCON
	/* emulator debug console (QEMU -debugcon, Bochs port E9): Windows 9x
	 * does not trap ring-3 access to this port */
	{
		const char* p;
		for (p = msg; *p && p < msg + sizeof(msg); p++)
			__asm__ volatile("outb %0, $0xe9" : : "a"(*p));
		if (cnt > 0 && msg[cnt < (int) sizeof(msg) ? cnt - 1 : sizeof(msg) - 2] != '\n')
			__asm__ volatile("outb %0, $0xe9" : : "a"('\n'));
	}
#else
	OutputDebugString(msg);
#endif
}

void dbgprintf(const char* format, ...)
{
	va_list argp;
	va_start(argp, format);
	dbgvprintf(format, argp);
	va_end(argp);
}


#ifdef KEX_DEBUGCON
extern "C" int __cdecl kex_dbg_printf(const char* format, ...)
{
	va_list argp;
	va_start(argp, format);
	dbgvprintf(format, argp);
	va_end(argp);
	return 0;
}
#endif
