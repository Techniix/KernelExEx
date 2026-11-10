/*
 *  KernelEx kexrt: KernelEx API library interface and DLL entry.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  A non-shared API library (image base below 2 GB): loaded into a process
 *  the first time it binds to one of these functions, with private state.
 *  Thread notifications are kept on (no DisableThreadLibraryCalls) because
 *  FLS callbacks and per-thread wait events are cleaned up at thread exit.
 */
#include <windows.h>
#include "kexrt.h"

typedef struct
{
	const char *target_library;
	const kexrt_api *named_apis;
	int named_apis_count;
	const void *ordinal_apis;
	int ordinal_apis_count;
} apilib_api_table;

static apilib_api_table tables[3];

__declspec(dllexport) const apilib_api_table *get_api_table(void)
{
	tables[0].target_library = "KERNEL32.DLL";
	tables[0].named_apis = kexrt_kernel32_apis;
	tables[0].named_apis_count = kexrt_kernel32_count;
	tables[1].target_library = "WS2_32.DLL";
	tables[1].named_apis = kexrt_ws2_32_apis;
	tables[1].named_apis_count = kexrt_ws2_32_count;
	tables[2].target_library = NULL;
	return tables;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	switch (reason)
	{
	case DLL_THREAD_DETACH:
		kexrt_fls_thread_exit();
		kx_sync_thread_detach();
		break;
	case DLL_PROCESS_DETACH:
		if (!reserved) /* FreeLibrary, not process exit */
		{
			kexrt_fls_thread_exit();
			kx_sync_thread_detach();
		}
		break;
	}
	return TRUE;
}
