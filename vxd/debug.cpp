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

extern "C" {
#include <basedef.h>
#include <vmm.h>
};
#include <string.h>
#include <malloc.h>
#include "debug.h"

#ifdef KEX_DEBUGCON
/* debug builds for emulators: messages go to I/O port 0xE9, which QEMU
 * (-debugcon file:...) and Bochs capture */
#include <vxdwraps.h>

void debug_printf(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	const char* p;
	va_start(ap, fmt);
	_Vsprintf(buf, (char*) fmt, ap);
	va_end(ap);
	for (p = "VKRNLEX: "; *p; p++)
		__asm__ volatile("outb %0, $0xe9" : : "a"(*p));
	for (p = buf; *p; p++)
		__asm__ volatile("outb %0, $0xe9" : : "a"(*p));
	__asm__ volatile("outb %0, $0xe9" : : "a"('\n'));
}
#else
void debug_printf(const char* fmt, ...)
{
	char* newfmt = (char*) alloca(strlen(fmt) + 10);
	strcpy(newfmt, "VKRNLEX: ");
	strcat(newfmt, fmt);
	strcat(newfmt, "\n");

    __asm lea  eax,(fmt + 4)
    __asm push eax
    __asm push newfmt
    VMMCall(_Debug_Printf_Service)
    __asm add esp, 2*4
}
#endif
