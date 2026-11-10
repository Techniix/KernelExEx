#include <windows.h>
/* EXE entry for NSIS UI resource files: makensis only reads their dialogs. */
void WinMainCRTStartup(void) { ExitProcess(0); }
void mainCRTStartup(void) { ExitProcess(0); }
