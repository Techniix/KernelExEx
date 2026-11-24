/* tests/ramdisk/memhog.c: paging test for the RAM disk. MEMHOG.EXE commits 600 MB (or the
   MB given on the command line), writes a pattern to every page twice, reads it all back
   and writes the result and the time to C:\MEMHOG.TXT. Run with Windows limited to far
   less RAM so it must page. No C runtime (Windows 98's kernel32/user32 only). */
#include <windows.h>
void __stdcall start(void)
{
	char *c = GetCommandLineA(), buf[128];
	unsigned mb = 0, i, pass, bad = 0, n, t0 = GetTickCount(), *p;
	DWORD w;
	HANDLE f;
	while (*c && *c != ' ') c++;
	while (*c == ' ') c++;
	while (*c >= '0' && *c <= '9') mb = mb * 10 + (unsigned) (*c++ - '0');
	if (!mb) mb = 600;
	n = mb * 256;
	p = (unsigned *) VirtualAlloc(0, (SIZE_T) mb << 20, MEM_COMMIT, PAGE_READWRITE);
	if (p)
	{
		for (pass = 0; pass < 2; pass++)
			for (i = 0; i < n; i++)
				p[i * 1024] = i * 2654435761u + pass, p[i * 1024 + 1023] = ~i;
		for (i = 0; i < n; i++)
			if (p[i * 1024] != i * 2654435761u + 1 || p[i * 1024 + 1023] != ~i)
				bad++;
		wsprintfA(buf, "%u MB, %u pages, %u bad, %u ms\r\n", mb, n, bad, GetTickCount() - t0);
	}
	else
		wsprintfA(buf, "VirtualAlloc %u MB failed: %lu\r\n", mb, GetLastError());
	f = CreateFileA("C:\\MEMHOG.TXT", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	WriteFile(f, buf, lstrlenA(buf), &w, 0);
	CloseHandle(f);
	ExitProcess(bad != 0);
}
