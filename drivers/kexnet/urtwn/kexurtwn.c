/*
 *  KernelExEx - kexurtwn.sys: Realtek USB Wi-Fi (RTL8188CUS/CE, RTL8188EU,
 *  RTL8188FTV, RTL8192CU, RTL8192EU; TP-Link TL-WN722N v2/v3, TL-WN725N
 *  v2/v3 and many others) for Windows 98 SE and ME, with OpenBSD's urtwn(4)
 *  and net80211.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  if_urtwn.c and rtwn.c are OpenBSD's, unchanged. They are compiled into
 *  this file so the front end knows the softc size. Firmware comes from
 *  WINDOWS\SYSTEM\KEXFW (see kexurtwn.inf).
 */
#include "dev/usb/if_urtwn.c"
#include "kexnet.h"

static const struct kexnet_driver urtwn_driver = {
	"urtwn",
	"Realtek USB Wi-Fi (KernelExEx, OpenBSD urtwn)",
	sizeof(struct urtwn_softc),
	urtwn_match,
	urtwn_attach,
	NULL,
	NULL,
	NULL,
	1,
	kex80211_configure,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &urtwn_driver);
}
