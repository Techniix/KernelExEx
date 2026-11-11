/*
 *  KernelExEx - kexrndis.sys: USB RNDIS network adapters (Android phone
 *  tethering, many USB Ethernet gadgets) for Windows 98 SE and ME, with
 *  OpenBSD's urndis(4) driver.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  if_urndis.c is OpenBSD's, unchanged. It runs on its device's worker
 *  thread (kexbsd/lib/obsd_usb.c), so its blocking control requests and
 *  its transfer callbacks are serialized the way splusb would serialize
 *  them.
 */
/* no ndis.h here: rndis.h defines the same OIDs */
#include "obsd/kexobsd_net.h"
#include <dev/usb/usbdi.h>
#include <dev/rndis.h>
#include <dev/usb/if_urndisreg.h>
#include "kexnet.h"

int urndis_match(struct device *, void *, void *);
void urndis_attach(struct device *, struct device *, void *);

static const struct kexnet_driver urndis_driver = {
	"urndis",
	"USB RNDIS network adapter (KernelExEx, OpenBSD urndis)",
	sizeof(struct urndis_softc),
	urndis_match,
	urndis_attach,
	NULL,
	NULL,
	NULL,
	1,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &urndis_driver);
}
