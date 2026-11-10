/* Forced include for kexcrt's own snprintf.c / vsnprintf.c.
 * mingw-w64's <stdio.h> defines snprintf and vsnprintf inline in terms of
 * __ms_snprintf / __ms_vsnprintf. Rename kexcrt's definitions out of the
 * way; stdio_glue.c connects the two. */
#include <stdio.h>
#define vsnprintf kexcrt_vsnprintf
#define snprintf kexcrt_snprintf
