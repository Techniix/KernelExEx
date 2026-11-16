/*
 * ackex98.h: ACPICA host configuration for KernelExEx's KEXACPI.VXD
 * (Windows 98 SE / ME, ring 0, 32-bit, clang). Part of KernelExEx, used
 * with ACPICA under its BSD-3-Clause OR GPL-2.0-only terms (GPLv2 here).
 *
 * SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only
 */
#ifndef __ACKEX98_H__
#define __ACKEX98_H__

#define ACPI_32BIT_PHYSICAL_ADDRESS
#define ACPI_MACHINE_WIDTH          32
#define ACPI_USE_NATIVE_DIVIDE      /* kexcrt has the 64-bit division helpers */
#define ACPI_USE_NATIVE_MATH64

#define COMPILER_DEPENDENT_INT64    long long
#define COMPILER_DEPENDENT_UINT64   unsigned long long

/* every ACPICA call is made from one DeviceIoControl at a time (the VxD
 * serialises them), so no locks are needed */
#define ACPI_SINGLE_THREADED
#define ACPI_USE_LOCAL_CACHE
#define ACPI_USE_DO_WHILE_0
#define ACPI_IGNORE_PACKAGE_RESOLUTION_ERRORS

/* ACPICA's own C library pieces (utclib.c); no host C library */
#undef ACPI_USE_SYSTEM_CLIBRARY
#undef ACPI_DEBUG_OUTPUT
#undef ACPI_DBG_TRACK_ALLOCATIONS

#include <stdarg.h>

/* the FACS global lock, as on other x86 hosts */
int AcpiOsAcquireGlobalLock(void *Facs);
int AcpiOsReleaseGlobalLock(void *Facs);
#define ACPI_ACQUIRE_GLOBAL_LOCK(FacsPtr, Acq) Acq = AcpiOsAcquireGlobalLock(FacsPtr)
#define ACPI_RELEASE_GLOBAL_LOCK(FacsPtr, Pnd) Pnd = AcpiOsReleaseGlobalLock(FacsPtr)

#define ACPI_FLUSH_CPU_CACHE() __asm__ __volatile__("wbinvd" ::: "memory")

#endif
