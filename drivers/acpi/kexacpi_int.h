/* KernelExEx - KEXACPI.VXD internals shared by its source files. GPLv2. */
#ifndef KEXACPI_INT_H
#define KEXACPI_INT_H

typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;

void *vmm_heap_alloc(ULONG n);
void vmm_heap_free(void *p);
ULONG vmm_time_ms(void);
UCHAR kex_inb(USHORT p);
void kex_outb(USHORT p, UCHAR v);
void kex_dbg_char(char c);
int AcpiOsAcquireGlobalLock(void *Facs);
int AcpiOsReleaseGlobalLock(void *Facs);

#endif
