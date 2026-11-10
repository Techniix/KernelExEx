/* KernelExEx - Win32 DeviceIoControl interface for VxDs (see vmm.h). */
#ifndef KEX_VXD_VWIN32_H
#define KEX_VXD_VWIN32_H

#define DIOC_GETVERSION   0
#define DIOC_OPEN         DIOC_GETVERSION
#define DIOC_CLOSEHANDLE  ((DWORD)-1)

/* passed in ESI with W32_DEVICEIOCONTROL */
typedef struct DIOCParams
{
	DWORD Internal1;
	DWORD VMHandle;
	DWORD Internal2;
	DWORD dwIoControlCode;
	DWORD lpvInBuffer;
	DWORD cbInBuffer;
	DWORD lpvOutBuffer;
	DWORD cbOutBuffer;
	DWORD lpcbBytesReturned;
	DWORD lpoOverlapped;
	DWORD hDevice;
	DWORD tagProcess;
} DIOCPARAMETERS, *PDIOCPARAMETERS;

#endif
