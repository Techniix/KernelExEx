/*
 *  KernelExEx - minimal Windows 9x VxD definitions for clang builds.
 *
 *  Written from public documentation, not from the Microsoft DDK:
 *  - VMM service numbers: Ralf Brown's Interrupt List, table 01265.
 *  - Control message numbers: 386SWAT VxD documentation (Sudley Place).
 *  - Device descriptor block layout and LE details: checked against the
 *    VxDs of Windows 98 SE with toolchain/vxd/ledump.py.
 *  Only what VKrnlEx.vxd uses is defined. GPLv2, as KernelEx.
 */
#ifndef KEX_VXD_VMM_H
#define KEX_VXD_VMM_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef ULONG HVM;
typedef ULONG HTHREAD;

#define VXD_SUCCESS TRUE
#define VXD_FAILURE FALSE

/* Windows 98 DDK level; VKrnlEx supports Windows 98 SE and ME only */
#define DDK_VERSION 0x040A

#define UNDEFINED_DEVICE_ID   0x00000
#define UNDEFINED_INIT_ORDER  0x80000000

/* system control messages */
#define SYS_CRITICAL_INIT        0x0000
#define DEVICE_INIT              0x0001
#define INIT_COMPLETE            0x0002
#define SYS_VM_INIT              0x0003
#define SYS_VM_TERMINATE         0x0004
#define SYSTEM_EXIT              0x0005
#define SYS_CRITICAL_EXIT        0x0006
#define CREATE_VM                0x0007
#define BEGIN_PM_APP             0x0015
#define END_PM_APP               0x0016
#define SYS_DYNAMIC_DEVICE_INIT  0x001B
#define SYS_DYNAMIC_DEVICE_EXIT  0x001C
#define CREATE_THREAD            0x001D
#define THREAD_INIT              0x001E
#define TERMINATE_THREAD         0x001F
#define DESTROY_THREAD           0x0021
#define W32_DEVICEIOCONTROL      0x0023
/* CREATE_PROCESS and DESTROY_PROCESS are not defined: their numbers are not
 * in the sources above, and KernelEx's handlers for them only return
 * success, which is also what the default case does. */

struct VxD_Desc_Block
{
	ULONG  DDB_Next;
	USHORT DDB_SDK_Version;
	USHORT DDB_Req_Device_Number;
	UCHAR  DDB_Dev_Major_Version;
	UCHAR  DDB_Dev_Minor_Version;
	USHORT DDB_Flags;
	UCHAR  DDB_Name[8];
	ULONG  DDB_Init_Order;
	ULONG  DDB_Control_Proc;
	ULONG  DDB_V86_API_Proc;
	ULONG  DDB_PM_API_Proc;
	ULONG  DDB_V86_API_CSIP;
	ULONG  DDB_PM_API_CSIP;
	ULONG  DDB_Reference_Data;
	ULONG  DDB_Service_Table_Ptr;
	ULONG  DDB_Service_Table_Size;
	ULONG  DDB_Win32_Service_Table;
	ULONG  DDB_Prev;
	ULONG  DDB_Size;
	ULONG  DDB_Reserved1;
	ULONG  DDB_Reserved2;
	ULONG  DDB_Reserved3;
};

/* pages */
#define PAGESHIFT    12
#define PAGESIZE     4096
#define PAGEMASK     0xFFF
#define PAGE(p)      ((ULONG)(p) >> PAGESHIFT)
#define NPAGES(n)    (((ULONG)(n) + PAGEMASK) >> PAGESHIFT)

/* _HeapAllocate flags. HEAPSWAP (pageable heap) is deliberately 0, i.e.
 * the locked heap: always valid, and VKrnlEx keeps its only allocation (a
 * kernel32 backup) just for the length of the patch. */
#define HEAPZEROINIT 0x00000001
#define HEAPSWAP     0

/* Service IDs: high word = VxD ID, low word = service number.
 * GetVxDServiceOrdinal(X) expands to __X, as in the DDK. */
#define VMM_DEVICE_ID 0x0001
#define __Get_VMM_Version        0x00010000
#define __Test_Sys_VM_Handle     0x00010004
#define __HeapAllocate           0x0001004F
#define __HeapFree               0x00010051
#define __HeapGetSize            0x00010052
#define __LinPageLock            0x00010063
#define __Hook_Device_Service    0x00010090
#define ___Debug_Printf_Service  0x0001012D
#define GetVxDServiceOrdinal(s)  __##s

/* the DDK's own names for the same numbers */
#define ___HeapAllocate          __HeapAllocate
#define ___HeapFree              __HeapFree
#define ___HeapGetSize           __HeapGetSize
#define ___LinPageLock           __LinPageLock

/* Dynamic link to a VxD service: INT 20h followed by the service ID. The
 * VMM rewrites the six bytes into CALL [addr] (or JMP [addr] when bit 15
 * of the service number is set) on first use. */
#define DL_JMP_MASK 0x8000
#define __KEX_B(v, n) (((v) >> (8 * (n))) & 0xFF)
#define __KEX_DYNALINK(id) \
	__asm int 20h \
	__asm _emit __KEX_B(id, 0) __asm _emit __KEX_B(id, 1) \
	__asm _emit __KEX_B(id, 2) __asm _emit __KEX_B(id, 3)
#define VMMCall(s)  __KEX_DYNALINK(__##s)
#define VMMJmp(s)   __KEX_DYNALINK(__##s | DL_JMP_MASK)
#define VxDCall(s)  VMMCall(s)
#define VxDJmp(s)   VMMJmp(s)

/* C-callable VMM services (toolchain/vxd/vxdwraps.c) */
void * __cdecl _HeapAllocate(ULONG nbytes, ULONG flags);
ULONG  __cdecl _HeapFree(void *hAddress, ULONG flags);
ULONG  __cdecl _HeapGetSize(void *hAddress, ULONG flags);
ULONG  __cdecl _LinPageLock(ULONG HLinPgNum, ULONG nPages, ULONG flags);
void   __cdecl _Debug_Printf_Service(const char *fmt, ...);

/* register-based services */
#pragma warning(disable:4035)
static __inline WORD Get_VMM_Version(void)
{
	VMMCall(Get_VMM_Version)
	__asm movzx eax, ax
}

static __inline BOOL Test_Sys_VM_Handle(HVM hvm)
{
	__asm mov ebx, [hvm]
	VMMCall(Test_Sys_VM_Handle)
	__asm setz al
	__asm movzx eax, al
}
#pragma warning(default:4035)

#ifdef __cplusplus
}
#endif

#endif
