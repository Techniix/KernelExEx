/* KernelExEx - the services are declared in vmm.h */
#include "vmm.h"

#include <stdarg.h>
#ifdef __cplusplus
extern "C"
#endif
ULONG __cdecl _Vsprintf(char *out, char *fmt, va_list args);   /* toolchain/vxd/vxdwraps.c */
