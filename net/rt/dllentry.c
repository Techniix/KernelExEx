/*
 *  KernelEx net runtime: DLL entry point (DLLs only; EXEs define their own).
 *  Licensed under the GNU General Public License version 2.
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved);

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return DllMain(inst, reason, reserved);
}
