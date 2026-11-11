/* Included into every file of a --debugcon build (toolchain/build_core.py):
 * debug-only printf output goes to the emulator debug port too. The C
 * runtime's own printf (kexcrt) is left alone. */
#ifndef KEXDBGCON_H
#define KEXDBGCON_H
#ifndef KEX_NO_PRINTF_REDIRECT
#ifdef __cplusplus
extern "C"
#endif
int __attribute__((cdecl)) kex_dbg_printf(const char* format, ...);
#define printf kex_dbg_printf
#endif
/* raw string to the debug port, usable before any runtime is up */
static __inline__ void kexdbg_puts(const char* s)
{
	for (; *s; s++)
		__asm__ volatile("outb %0, $0xe9" : : "a"(*s));
}
#define KEXDBG_PUTS(s) kexdbg_puts(s)
#endif
