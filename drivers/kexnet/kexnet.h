/*
 *  KernelExEx - kexnet: what a NIC front end tells the OpenBSD network glue.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#ifndef KEXNET_H
#define KEXNET_H

#define KEXNET_DRIVER_VERSION 0x00010000   /* reported as OID_GEN_VENDOR_DRIVER_VERSION */

struct device;
struct pci_attach_args;

struct kexnet_driver
{
	const char *name;           /* "em": interfaces are em0, em1 ... */
	const char *description;    /* OID_GEN_VENDOR_DESCRIPTION */
	size_t softc_size;
	int (*match)(struct device *parent, void *match, void *aux);
	void (*attach)(struct device *parent, struct device *self, void *aux);
	/* at interrupt level: is the interrupt ours? If so, note the cause and
	 * mask the device's interrupts; the driver's handler runs next, in the
	 * interrupt DPC, followed by intr_enable */
	int (*isr)(struct device *self);
	void (*intr_enable)(struct device *self);
	void (*shutdown)(struct device *self);   /* optional: quiesce DMA at power off */
};

NTSTATUS kexnet_register(PDRIVER_OBJECT drv, PUNICODE_STRING reg, const struct kexnet_driver *d);

#endif
