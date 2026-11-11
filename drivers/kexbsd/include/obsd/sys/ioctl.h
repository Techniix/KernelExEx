/* KernelExEx: OpenBSD <sys/ioctl.h>: ioctl numbers only need to compile */
#include "obsd/kexobsd_net.h"
#ifndef _IOC
#define _IOC(inout, group, num, len) ((unsigned long) ((inout) | (((len) & 0x1fff) << 16) | ((group) << 8) | (num)))
#define _IO(g, n) _IOC(0x20000000UL, (g), (n), 0)
#define _IOR(g, n, t) _IOC(0x40000000UL, (g), (n), sizeof(t))
#define _IOW(g, n, t) _IOC(0x80000000UL, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(0xc0000000UL, (g), (n), sizeof(t))
#endif
