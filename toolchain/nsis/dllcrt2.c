/* DLL entry for the NSIS plugins built with this cross setup. */
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);
BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p) { return DllMain(h, r, p); }
