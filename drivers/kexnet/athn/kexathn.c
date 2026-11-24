/*
 *  KernelExEx - kexathn.sys: Atheros 802.11n PCI and PCIe Wi-Fi (AR5416,
 *  AR5418, AR9160, AR9280, AR9281, AR9285, AR9227, AR2427, AR9287; the
 *  Latitude E6510's AR9285 among them) for Windows 98 SE and ME, with
 *  OpenBSD's athn(4) and net80211. These chips need no firmware file.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  athn.c, ar5008.c, ar5416.c, ar9280.c, ar9285.c, ar9287.c and
 *  if_athn_pci.c are OpenBSD's (ISC licence, Damien Bergamini and Atheros),
 *  unchanged; see README.md. if_athn_pci.c is compiled into this file so the
 *  front end knows the softc.
 *
 *  Interrupts: kexnet claims and masks an interrupt in the ISR and runs the
 *  driver's handler (athn_intr) in the interrupt DPC. athn_intr reads the
 *  cause registers itself, so the ISR must not clear them, and must not mask
 *  with AR_IER either: with the MAC's interrupt output off the asynchronous
 *  cause bit drops and athn_intr would find nothing. The ISR switches off the
 *  host interface's line enables only (AR_INTR_ASYNC_ENABLE/SYNC_ENABLE, as
 *  Linux's ath9k does), which leave the cause registers as they are.
 */
#include "dev/pci/if_athn_pci.c"
#include "kexnet.h"

/* athn.c's, not in athnvar.h */
void athn_disable_interrupts(struct athn_softc *);
void athn_stop(struct ifnet *, int);

static int kexathn_isr(struct device *self)
{
	struct athn_pci_softc *psc = (struct athn_pci_softc *) self;
	struct athn_softc *sc = &psc->sc_sc;
	uint32_t async, sync;
	if (!psc->sc_mapsize || !psc->sc_ih)
		return 0;   /* registers not mapped, or attach not this far */
	async = AR_READ(sc, AR_INTR_ASYNC_CAUSE);
	if (async == AR_INTR_SPURIOUS)
		return 0;   /* card gone or powered down */
	sync = AR_READ(sc, AR_INTR_SYNC_CAUSE);
	if (!(async & AR_INTR_MAC_IRQ) && (sync == AR_INTR_SPURIOUS || !(sync & sc->isync)))
		return 0;   /* someone else on a shared line */
	AR_WRITE(sc, AR_INTR_ASYNC_ENABLE, 0);
	AR_WRITE(sc, AR_INTR_SYNC_ENABLE, 0);
	(void) AR_READ(sc, AR_INTR_SYNC_ENABLE);   /* flush: the line drops now */
	return 1;
}

/* after athn_intr: the line enables back as athn_enable_interrupts sets them,
 * unless the driver has the interface down (it switched interrupts off itself) */
static void kexathn_intr_enable(struct device *self)
{
	struct athn_softc *sc = &((struct athn_pci_softc *) self)->sc_sc;
	if (!(sc->sc_ic.ic_if.if_flags & IFF_RUNNING))
		return;
	AR_WRITE(sc, AR_INTR_ASYNC_ENABLE, AR_INTR_MAC_IRQ);
	AR_WRITE(sc, AR_INTR_SYNC_ENABLE, sc->isync);
	AR_WRITE_BARRIER(sc);
}

static void kexathn_shutdown(struct device *self)
{
	struct athn_pci_softc *psc = (struct athn_pci_softc *) self;
	struct athn_softc *sc = &psc->sc_sc;
	if (psc->sc_mapsize && psc->sc_ih)
	{
		/* no DMA and no interrupts when Windows powers off or restarts */
		athn_disable_interrupts(sc);
		athn_stop(&sc->sc_ic.ic_if, 1);
	}
}

static const struct kexnet_driver athn_driver = {
	"athn",
	"Atheros Wi-Fi (KernelExEx, OpenBSD athn)",
	sizeof(struct athn_pci_softc),
	athn_pci_match,
	athn_pci_attach,
	kexathn_isr,
	kexathn_intr_enable,
	kexathn_shutdown,
	0,
	kex80211_configure,
	&kex80211_ctl_ops,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &athn_driver);
}
