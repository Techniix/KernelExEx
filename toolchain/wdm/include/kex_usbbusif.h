/*
 *  KernelExEx - the USBDI bus interface (IRP_MN_QUERY_INTERFACE with
 *  USB_BUS_INTERFACE_USBDI_GUID), version 1, for Windows 2000 era targets:
 *  mingw-w64's usbbusif.h only defines it for XP and later. Same layout as
 *  the Windows XP DDK. KernelExEx's EHCI driver offers it; KernelExEx USB
 *  drivers ask it whether their device runs at high speed.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#ifndef KEX_USBBUSIF_H
#define KEX_USBBUSIF_H

static const GUID KEX_USB_BUS_INTERFACE_USBDI_GUID = { 0xb1a96a13, 0x3de0, 0x4574,
                                                       { 0x9b, 0x01, 0xc0, 0x8f, 0xea, 0xb3, 0x18, 0xd6 } };

typedef VOID (NTAPI *KEX_USBDI_GETUSBDI_VERSION)(PVOID ctx, PVOID version_info, PULONG hcd_caps);
typedef NTSTATUS (NTAPI *KEX_USBDI_QUERY_BUS_TIME)(PVOID ctx, PULONG frame);
typedef NTSTATUS (NTAPI *KEX_USBDI_SUBMIT_ISO_OUT_URB)(PVOID ctx, PVOID urb);
typedef NTSTATUS (NTAPI *KEX_USBDI_QUERY_BUS_INFORMATION)(PVOID ctx, ULONG level, PVOID buf, PULONG len, PULONG actual);
typedef BOOLEAN (NTAPI *KEX_USBDI_IS_DEVICE_HIGH_SPEED)(PVOID ctx);

typedef struct
{
	USHORT Size;
	USHORT Version;
	PVOID BusContext;
	PINTERFACE_REFERENCE InterfaceReference;
	PINTERFACE_DEREFERENCE InterfaceDereference;
	KEX_USBDI_GETUSBDI_VERSION GetUSBDIVersion;
	KEX_USBDI_QUERY_BUS_TIME QueryBusTime;
	KEX_USBDI_SUBMIT_ISO_OUT_URB SubmitIsoOutUrb;
	KEX_USBDI_QUERY_BUS_INFORMATION QueryBusInformation;
	KEX_USBDI_IS_DEVICE_HIGH_SPEED IsDeviceHighSpeed;   /* version 1 */
} KEX_USB_BUS_INTERFACE_USBDI_V1;

#define KEX_USBDI_V0_SIZE ((USHORT) FIELD_OFFSET(KEX_USB_BUS_INTERFACE_USBDI_V1, IsDeviceHighSpeed))
#define KEX_USBDI_V1_SIZE ((USHORT) sizeof(KEX_USB_BUS_INTERFACE_USBDI_V1))

#endif
