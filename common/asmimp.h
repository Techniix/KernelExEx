/*
 *  KernelEx
 *
 *  Calling imported functions from inline assembly, portably.
 *
 *  Visual C++ treats the name of a __declspec(dllimport) function inside an
 *  __asm block as its import-table slot, so
 *      call dword ptr [HeapAlloc]
 *  calls through the slot. clang does not: it first loads the slot into a
 *  scratch register and then dereferences the function's code, which both
 *  crashes and destroys a register. Name the slot explicitly instead:
 *
 *      KEX_ASM_IMPORT(HeapAlloc, 12);      // at file scope; 12 = argument bytes
 *      ...
 *      call dword ptr [IMP(HeapAlloc)]     // inside __asm
 */

#ifndef __ASMIMP_H
#define __ASMIMP_H

#if defined(__clang__) || defined(__GNUC__)
#ifdef __cplusplus
#define KEX_ASM_IMPORT(fn, bytes) extern "C" void* __kex_imp_##fn __asm__("__imp__" #fn "@" #bytes)
#else
#define KEX_ASM_IMPORT(fn, bytes) extern void* __kex_imp_##fn __asm__("__imp__" #fn "@" #bytes)
#endif
#define IMP(fn) __kex_imp_##fn
#else
#define KEX_ASM_IMPORT(fn, bytes) extern int __kex_asm_import_unused
#define IMP(fn) fn
#endif

#endif
