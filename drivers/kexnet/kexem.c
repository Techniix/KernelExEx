/*
 *  KernelExEx - kexem.sys: Intel PRO/1000 (8254x, 8257x, 8258x, ICH/PCH,
 *  I21x/I350) for Windows 98 SE and ME, with OpenBSD's em(4) driver.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  if_em.c and if_em_hw.c are OpenBSD's, unchanged but for one line in
 *  em_intr (see em/README.md): the interrupt cause is read in the ISR,
 *  where the line must be claimed, and handed to em_intr in the DPC.
 */
#include <ndis.h>
#include <dev/pci/if_em.h>
#include <dev/pci/if_em_soc.h>
#include "kexnet.h"

int em_probe(struct device *, void *, void *);
void em_attach(struct device *, struct device *, void *);
void em_enable_intr(struct em_softc *);
void em_disable_intr(struct em_softc *);

struct kexem_softc
{
	struct em_softc em;      /* first: the driver casts struct device * to it */
	volatile uint32_t icr;   /* causes read by the ISR, not yet handled */
};

/* em_intr's cause register read (the patched line) */
uint32_t kexem_take_icr(struct em_softc *sc)
{
	return __sync_lock_test_and_set(&((struct kexem_softc *) sc)->icr, 0);
}

static int kexem_isr(struct device *self)
{
	struct kexem_softc *k = (struct kexem_softc *) self;
	struct em_softc *sc = &k->em;
	uint32_t icr;
	if (!sc->hw.back)
		return 0;   /* registers not mapped yet */
	icr = E1000_READ_REG(&sc->hw, ICR);
	if (!icr)
		return 0;
	/* reading ICR cleared the causes: keep them for the DPC either way */
	__sync_fetch_and_or(&k->icr, icr);
	if (sc->hw.mac_type >= em_82571 ? !(icr & E1000_ICR_INT_ASSERTED) : !(icr & E1000_READ_REG(&sc->hw, IMS)))
		return 0;   /* only masked causes: someone else on a shared line */
	E1000_WRITE_REG(&sc->hw, IMC, 0xffffffff);
	return 1;
}

static void kexem_intr_enable(struct device *self)
{
	struct em_softc *sc = (struct em_softc *) self;
	if (sc->sc_ac.ac_if.if_flags & IFF_RUNNING)
		em_enable_intr(sc);
}

static void kexem_shutdown(struct device *self)
{
	struct em_softc *sc = (struct em_softc *) self;
	if (sc->hw.back)
	{
		em_disable_intr(sc);
		em_reset_hw(&sc->hw);   /* stops DMA before Windows powers off */
	}
}

/* EP80579 (ICP_xxxx) needs its GCU, a separate PCI function; not supported,
 * and its IDs are left out of the INF */
void *em_lookup_gcu(struct device *self)
{
	(void) self;
	return NULL;
}

int em_attach_miibus(struct device *self)
{
	(void) self;
	return 0;
}

int gcu_miibus_readreg(struct em_hw *hw, int phy, int reg)
{
	(void) hw;
	(void) phy;
	(void) reg;
	return 0;
}

void gcu_miibus_writereg(struct em_hw *hw, int phy, int reg, int val)
{
	(void) hw;
	(void) phy;
	(void) reg;
	(void) val;
}

static const struct kexnet_driver em_driver = {
	"em",
	"Intel PRO/1000 (KernelExEx, OpenBSD em)",
	sizeof(struct kexem_softc),
	em_probe,
	em_attach,
	kexem_isr,
	kexem_intr_enable,
	kexem_shutdown,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &em_driver);
}
