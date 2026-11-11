/*
 *  KernelExEx - link check for the wireless stack: a driver binary that
 *  pulls in net80211's entry points, so every symbol it needs is known to
 *  resolve against kexbsd and the 98 SE kernel before a radio driver
 *  exists. Never installed.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "obsd/kexobsd_net.h"
#include <dev/usb/usbdi.h>
#include <net80211/ieee80211_var.h>
#include "kexnet.h"

static int lt_match(struct device *p, void *m, void *aux)
{
	(void) p;
	(void) m;
	(void) aux;
	return 0;
}

static void lt_attach(struct device *p, struct device *self, void *aux)
{
	struct ieee80211com *ic = (struct ieee80211com *) aux;
	(void) p;
	(void) self;
	ieee80211_ifattach(&ic->ic_if);
	ieee80211_media_init(&ic->ic_if, NULL, NULL);
	ieee80211_input(&ic->ic_if, NULL, NULL, NULL);
	ieee80211_encap(&ic->ic_if, NULL, NULL);
	ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	ieee80211_ioctl(&ic->ic_if, 0, NULL);
	ieee80211_watchdog(&ic->ic_if);
	ieee80211_ifdetach(&ic->ic_if);
}

static const struct kexnet_driver lt = { "wlt", "link test", 64, lt_match, lt_attach, NULL, NULL, NULL, 1 };

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &lt);
}
