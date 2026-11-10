/*
 *  KernelEx kextls: DLL entry point.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  KernelEx's core loads every API library once at boot (inside MPREXE) just
 *  to read get_api_table(), so attach must stay cheap: no entropy gathering,
 *  no file access. All real work happens on first use.
 */
#include <windows.h>
#include "kextls_internal.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(inst);
		kextls_platform_init();
		break;
	case DLL_PROCESS_DETACH:
		/* On process exit (reserved != NULL) other threads are gone or
		 * frozen; leave the locks alone rather than risk a deadlock. */
		if (!reserved)
			kextls_platform_shutdown();
		break;
	}
	return TRUE;
}
