/*
 *  KernelEx
 *  Copyright (C) 2008, Xeno86
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
#include <float.h>
#include "unifwd.h"
#include "kexcoresdk.h"
#include "common.h"

static HMODULE hUnicows;

/* kexuni.dll, KernelExEx's open replacement, sits next to kexbasen.dll */
static HMODULE load_kexuni(void)
{
	char path[MAX_PATH];
	char* slash;
	HMODULE h = GetModuleHandle("KEXUNI.DLL");
	if (h)
		return h;
	if (!GetModuleFileName(GetModuleHandle("KEXBASEN.DLL"), path, sizeof(path) - 16))
		return NULL;
	slash = strrchr(path, '\\');
	if (!slash)
		return NULL;
	strcpy(slash + 1, "KEXUNI.DLL");
	return LoadLibrary(path);
}

int unifwd_init(void)
{
	DWORD lasterror = GetLastError();
	hUnicows = GetModuleHandle("UNICOWS.DLL");
	if (!hUnicows)
	{
		//Some DLLs that are loaded in conjunction with
		//unicows.dll may blow the FPU's control word.
		//Save it here...
		unsigned int fpu_cw;
		fpu_cw = _control87(0, 0);
		hUnicows = LoadLibrary("UNICOWS.DLL");
		//...and restore afterwards
		if (_control87(0, 0) != fpu_cw)
			_control87(fpu_cw, 0xfffff);
	}
	//Microsoft no longer offers MSLU for download: without it, use kexuni
	if (!hUnicows)
		hUnicows = load_kexuni();
	SetLastError(lasterror);
	return (hUnicows != NULL);
}

void unicows_fatal_error(const char* func)
{
	char msg[256];
	strcpy(msg, "kexbasen: Failed to get unicows/kexuni proc addr: ");
	strcat(msg, func);
	fatal_error(msg);
}

unsigned long __stdcall GetUnicowsAddress(const char* name)
{
	if (!hUnicows)
		unifwd_init();
	PROC addr = kexGetProcAddress(hUnicows, name);
	if (!addr)
		unicows_fatal_error(name);
	return (unsigned long) addr;
}
