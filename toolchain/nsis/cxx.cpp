// Minimal C++ runtime for NSIS plugins written in C++ (NSISdl): new/delete
// on the process heap. Stands in for libstdc++ in this cross build.
#include <windows.h>
void* operator new(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void* operator new[](size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void operator delete(void* p) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete[](void* p) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete(void* p, size_t) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete[](void* p, size_t) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
extern "C" void __cxa_pure_virtual() { ExitProcess(3); }
extern "C" int atexit(void (*)(void)) { return 0; } // plugins have no static destructors to run
