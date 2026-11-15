/*
 *  KernelExEx - kexwifi.cpl: the "Wi-Fi" icon in Control Panel. It opens
 *  kexwifi.exe (from the same folder), which does all the work.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include <windows.h>
#include <cpl.h>
#include <string.h>
#include "resource.h"

static HINSTANCE inst;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
	(void) r;
	if (reason == DLL_PROCESS_ATTACH)
		inst = h;
	return TRUE;
}

LONG CALLBACK CPlApplet(HWND parent, UINT msg, LPARAM lp1, LPARAM lp2)
{
	(void) lp1;
	switch (msg)
	{
	case CPL_INIT:
		return TRUE;
	case CPL_GETCOUNT:
		return 1;
	case CPL_INQUIRE:
	{
		CPLINFO *ci = (CPLINFO *) lp2;
		ci->idIcon = IDI_WIFI;
		ci->idName = 1;
		ci->idInfo = 2;
		ci->lData = 0;
		return 0;
	}
	case CPL_DBLCLK:
	{
		char path[MAX_PATH];
		char *slash;
		GetModuleFileName(inst, path, sizeof(path));
		slash = strrchr(path, '\\');
		if (slash)
			strcpy(slash + 1, "kexwifi.exe");
		if ((UINT) ShellExecute(parent, NULL, path, NULL, NULL, SW_SHOWNORMAL) <= 32)
			MessageBox(parent, "kexwifi.exe is missing. Reinstall KernelExEx (Wi-Fi).", "Wi-Fi", MB_OK | MB_ICONERROR);
		return 0;
	}
	}
	return 0;
}
