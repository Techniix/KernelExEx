/*
 *  KernelExEx - kexrl.sys: Realtek RTL8139 family 10/100 Ethernet for
 *  Windows 98 SE and ME: RTL8139/A/B/C/C+/D, RTL8100/8100B/8100C/8101L,
 *  RTL8130/8138, and the many cards built on them (D-Link DFE-530TX+ and
 *  DFE-538TX, SMC 1211TX, Accton EN1207D, Allied Telesyn AT-2500TX, Corega,
 *  Planex, Encore, Genius ...). Onboard on boards such as the MSI 848P
 *  Neo2-V (RTL8100C). Windows 98 has no driver for these.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Our own driver, written from the chip's register description, in the
 *  OpenBSD driver style that obsd_net.c turns into an NDIS miniport. The
 *  chip receives into one ring buffer and sends from four fixed buffers.
 */
#include "bpfilter.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include "kexnet.h"

/* ---- registers ------------------------------------------------------------------------ */
#define RL_IDR0 0x00
#define RL_MAR0 0x08
#define RL_TSD0 0x10      /* + 4 * n */
#define RL_TSAD0 0x20     /* + 4 * n */
#define RL_RBSTART 0x30
#define RL_CR 0x37
#define RL_CAPR 0x38
#define RL_CBR 0x3a
#define RL_IMR 0x3c
#define RL_ISR 0x3e
#define RL_TCR 0x40
#define RL_RCR 0x44
#define RL_MPC 0x4c
#define RL_9346CR 0x50
#define RL_CONFIG1 0x52
#define RL_MSR 0x58
#define RL_CONFIG3 0x59
#define RL_CONFIG4 0x5a
#define RL_BMCR 0x62
#define RL_BMSR 0x64
#define RL_ANAR 0x66
#define RL_ANLPAR 0x68

#define RL_CR_BUFE 0x01
#define RL_CR_TE 0x04
#define RL_CR_RE 0x08
#define RL_CR_RST 0x10

#define RL_INT_ROK 0x0001
#define RL_INT_RER 0x0002
#define RL_INT_TOK 0x0004
#define RL_INT_TER 0x0008
#define RL_INT_RXOVW 0x0010
#define RL_INT_LINK 0x0020   /* packet underrun / link change */
#define RL_INT_FOVW 0x0040
#define RL_INT_SERR 0x8000
#define RL_INTRS (RL_INT_ROK | RL_INT_RER | RL_INT_TOK | RL_INT_TER | RL_INT_RXOVW | RL_INT_LINK | RL_INT_FOVW | RL_INT_SERR)

#define RL_TSD_OWN 0x00002000
#define RL_TSD_TUN 0x00004000
#define RL_TSD_TOK 0x00008000
#define RL_TSD_OWC 0x20000000
#define RL_TSD_TABT 0x40000000
#define RL_TSD_CRS 0x80000000
#define RL_TSD_THRESH(b) ((((b) / 32) & 0x3f) << 16)

#define RL_RCR_AAP 0x00000001
#define RL_RCR_APM 0x00000002
#define RL_RCR_AM 0x00000004
#define RL_RCR_AB 0x00000008
#define RL_RCR_WRAP 0x00000080
#define RL_RCR_MXDMA_UNLIM 0x00000700
#define RL_RCR_RBLEN_32K 0x00001000   /* 01: 16K, 10: 32K */
#define RL_RCR_RBLEN_SHIFT 11
#define RL_RCR_RXFTH_NONE 0x0000e000

#define RL_TCR_MXDMA_2048 0x00000700
#define RL_TCR_IFG_STD 0x03000000

#define RL_MSR_LINKB 0x04    /* 1: no link */
#define RL_MSR_SPEED10 0x08

#define RL_RXSTAT_ROK 0x0001

#define RL_RXBUF 32768                  /* ring, and RL_RCR RBLEN 10 */
#define RL_RXBUF_ALLOC (RL_RXBUF + 16 + 1536)   /* WRAP: a frame may run past the end */
#define RL_NTX 4
#define RL_TXBUF 1536

struct rl_softc
{
	struct device sc_dev;
	struct arpcom sc_ac;
	struct ifmedia sc_media;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
	bus_size_t sc_sz;
	bus_dma_tag_t sc_dmat;
	void *sc_ih;
	/* receive ring */
	bus_dma_segment_t rx_seg;
	bus_dmamap_t rx_map;
	caddr_t rx_buf;
	uint16_t rx_cur;
	/* transmit buffers */
	bus_dma_segment_t tx_seg;
	bus_dmamap_t tx_map;
	caddr_t tx_buf;
	int tx_prod, tx_cons, tx_cnt;
	unsigned int tx_len[RL_NTX];
	int tx_thresh;
	volatile uint16_t isr;   /* causes the ISR saw, not yet handled */
	struct timeout tick;
	int chip_cplus;          /* 8139C+ and later: no RX reset on overflow needed */
	uint8_t revid;
};

#define CSR_READ_1(sc, r) bus_space_read_1((sc)->sc_st, (sc)->sc_sh, (r))
#define CSR_READ_2(sc, r) bus_space_read_2((sc)->sc_st, (sc)->sc_sh, (r))
#define CSR_READ_4(sc, r) bus_space_read_4((sc)->sc_st, (sc)->sc_sh, (r))
#define CSR_WRITE_1(sc, r, v) bus_space_write_1((sc)->sc_st, (sc)->sc_sh, (r), (v))
#define CSR_WRITE_2(sc, r, v) bus_space_write_2((sc)->sc_st, (sc)->sc_sh, (r), (v))
#define CSR_WRITE_4(sc, r, v) bus_space_write_4((sc)->sc_st, (sc)->sc_sh, (r), (v))

/* PCI IDs: Realtek's own and the boards and cards built on the chip */
static const struct
{
	uint16_t vendor, product;
} rl_devs[] = {
	{ 0x10ec, 0x8139 },   /* RTL8139 / 8139A/B/C/C+/D, RTL8100/8100B/8100C, RTL8101L */
	{ 0x10ec, 0x8138 },   /* RTL8139B CardBus (RT8138) */
	{ 0x10ec, 0x8129 },   /* RTL8129 is different (external PHY); we do not claim it */
	{ 0x1113, 0x1211 },   /* Accton EN1207D, SMC 1211TX */
	{ 0x1186, 0x1300 },   /* D-Link DFE-538TX / DFE-530TX+ */
	{ 0x1186, 0x1340 },   /* D-Link DFE-690TXD CardBus */
	{ 0x1259, 0xa117 },   /* Allied Telesyn AT-2500TX */
	{ 0x1259, 0xa11e },   /* Allied Telesyn AT-2500TX/ACPI */
	{ 0x1500, 0x1360 },   /* Delta Electronics */
	{ 0x4033, 0x1360 },   /* Addtron Technology */
	{ 0x13d1, 0xab06 },   /* Abocom / Corega FE2000VX */
	{ 0x14ea, 0xab06 },   /* Planex FNW-3603-TX */
	{ 0x14ea, 0xab07 },   /* Planex FNW-3800-TX */
	{ 0x1743, 0x8139 },   /* Peppercon ROL/F-100 */
	{ 0x021b, 0x8139 },   /* Compaq HNE-300 */
	{ 0x1904, 0x8139 },   /* Hangzhou Silan SC92031 (8139 compatible) */
	{ 0x11db, 0x1234 },   /* Sega Dreamcast broadband adapter (in a PC: why not) */
	{ 0x1432, 0x9130 },   /* Edimax EN-9130TX */
	{ 0x16ec, 0xab06 },   /* US Robotics USR997902 */
	{ 0x18d2, 0x3000 },   /* Sitecom / Freecom FN 10/100 */
	{ 0x1737, 0x8139 },   /* Linksys */
	{ 0x126c, 0x1211 },   /* Northern Telecom */
};

static int rl_match(struct device *parent, void *match, void *aux)
{
	struct pci_attach_args *pa = (struct pci_attach_args *) aux;
	unsigned int i;
	(void) parent;
	(void) match;
	for (i = 0; i < nitems(rl_devs); i++)
		if (PCI_VENDOR(pa->pa_id) == rl_devs[i].vendor && PCI_PRODUCT(pa->pa_id) == rl_devs[i].product &&
		    !(rl_devs[i].vendor == 0x10ec && rl_devs[i].product == 0x8129))
			return 1;
	return 0;
}

/* ---- link -------------------------------------------------------------------------------- */
static void rl_link(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	uint8_t msr = CSR_READ_1(sc, RL_MSR);
	int state;
	if (msr & RL_MSR_LINKB)
		state = LINK_STATE_DOWN;
	else
	{
		uint16_t common = CSR_READ_2(sc, RL_ANAR) & CSR_READ_2(sc, RL_ANLPAR);
		int fdx = (CSR_READ_2(sc, RL_BMCR) & 0x1000)   /* autonegotiating: what both sides offer */
		              ? ((msr & RL_MSR_SPEED10) ? (common & 0x0040) != 0 : (common & 0x0100) != 0)
		              : (CSR_READ_2(sc, RL_BMCR) & 0x0100) != 0;
		state = fdx ? LINK_STATE_FULL_DUPLEX : LINK_STATE_HALF_DUPLEX;
		ifp->if_baudrate = (msr & RL_MSR_SPEED10) ? IF_Mbps(10) : IF_Mbps(100);
	}
	if (ifp->if_link_state != state)
	{
		ifp->if_link_state = state;
		if_link_state_change(ifp);
	}
}

static int rl_media_change(struct ifnet *ifp)
{
	struct rl_softc *sc = (struct rl_softc *) ifp->if_softc;
	struct ifmedia *ifm = &sc->sc_media;
	uint16_t bmcr;
	switch (IFM_SUBTYPE(ifm->ifm_media))
	{
	case IFM_100_TX:
		bmcr = 0x2000 | ((ifm->ifm_media & IFM_FDX) ? 0x0100 : 0);
		break;
	case IFM_10_T:
		bmcr = (ifm->ifm_media & IFM_FDX) ? 0x0100 : 0;
		break;
	default:
		CSR_WRITE_2(sc, RL_ANAR, 0x01e1);   /* 10/100, half/full */
		bmcr = 0x1000 | 0x0200;             /* autonegotiate, restart */
		break;
	}
	CSR_WRITE_2(sc, RL_BMCR, bmcr);
	return 0;
}

static void rl_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct rl_softc *sc = (struct rl_softc *) ifp->if_softc;
	uint8_t msr = CSR_READ_1(sc, RL_MSR);
	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;
	if (msr & RL_MSR_LINKB)
	{
		ifmr->ifm_active |= IFM_NONE;
		return;
	}
	ifmr->ifm_status |= IFM_ACTIVE;
	ifmr->ifm_active |= (msr & RL_MSR_SPEED10) ? IFM_10_T : IFM_100_TX;
	if (ifp->if_link_state == LINK_STATE_FULL_DUPLEX)
		ifmr->ifm_active |= IFM_FDX;
	else
		ifmr->ifm_active |= IFM_HDX;
}

/* ---- receive filter ------------------------------------------------------------------------ */
static void rl_iff(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct arpcom *ac = &sc->sc_ac;
	uint32_t rcr, hash[2] = { 0, 0 };
	struct ether_multi *enm;
	struct ether_multistep step;

	rcr = RL_RCR_APM | RL_RCR_AB | RL_RCR_WRAP | RL_RCR_MXDMA_UNLIM | RL_RCR_RXFTH_NONE |
	      (2u << RL_RCR_RBLEN_SHIFT);   /* 32K ring */
	ifp->if_flags &= ~IFF_ALLMULTI;
	if ((ifp->if_flags & IFF_PROMISC) || ac->ac_multirangecnt > 0)
	{
		ifp->if_flags |= IFF_ALLMULTI;
		rcr |= RL_RCR_AM;
		if (ifp->if_flags & IFF_PROMISC)
			rcr |= RL_RCR_AAP;
		hash[0] = hash[1] = 0xffffffff;
	}
	else
	{
		rcr |= RL_RCR_AM;
		ETHER_FIRST_MULTI(step, ac, enm);
		while (enm != NULL)
		{
			uint32_t h = ether_crc32_be(enm->enm_addrlo, ETHER_ADDR_LEN) >> 26;
			hash[h >> 5] |= 1u << (h & 31);
			ETHER_NEXT_MULTI(step, enm);
		}
	}
	CSR_WRITE_4(sc, RL_MAR0, hash[0]);
	CSR_WRITE_4(sc, RL_MAR0 + 4, hash[1]);
	CSR_WRITE_4(sc, RL_RCR, rcr);
}

/* ---- start and stop -------------------------------------------------------------------------- */
static void rl_reset(struct rl_softc *sc)
{
	int i;
	CSR_WRITE_1(sc, RL_CR, RL_CR_RST);
	for (i = 0; i < 1000; i++)
	{
		DELAY(10);
		if (!(CSR_READ_1(sc, RL_CR) & RL_CR_RST))
			break;
	}
	if (i == 1000)
		printf("%s: reset timed out\n", sc->sc_dev.dv_xname);
}

static void rl_stop(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	timeout_del(&sc->tick);
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	CSR_WRITE_2(sc, RL_IMR, 0);
	CSR_WRITE_1(sc, RL_CR, 0);
	CSR_WRITE_2(sc, RL_ISR, 0xffff);
	sc->tx_prod = sc->tx_cons = sc->tx_cnt = 0;
}

static void rl_init(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	int i;
	rl_stop(sc);
	rl_reset(sc);
	/* the address the stack wants (NetworkAddress can override the EEPROM's) */
	CSR_WRITE_1(sc, RL_9346CR, 0xc0);
	for (i = 0; i < ETHER_ADDR_LEN; i++)
		CSR_WRITE_1(sc, RL_IDR0 + i, sc->sc_ac.ac_enaddr[i]);
	CSR_WRITE_1(sc, RL_9346CR, 0x00);
	CSR_WRITE_4(sc, RL_RBSTART, (uint32_t) sc->rx_map->dm_segs[0].ds_addr);
	for (i = 0; i < RL_NTX; i++)
		CSR_WRITE_4(sc, RL_TSAD0 + 4 * i, (uint32_t) sc->tx_map->dm_segs[0].ds_addr + i * RL_TXBUF);
	CSR_WRITE_1(sc, RL_CR, RL_CR_RE | RL_CR_TE);
	CSR_WRITE_4(sc, RL_TCR, RL_TCR_MXDMA_2048 | RL_TCR_IFG_STD);
	rl_iff(sc);
	sc->rx_cur = 0;
	CSR_WRITE_2(sc, RL_CAPR, (uint16_t) (0 - 16));
	CSR_WRITE_4(sc, RL_MPC, 0);
	sc->tx_thresh = 64;   /* raised on underruns */
	CSR_WRITE_2(sc, RL_ISR, 0xffff);
	CSR_WRITE_2(sc, RL_IMR, RL_INTRS);
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ifp->if_link_state = LINK_STATE_UNKNOWN;
	rl_link(sc);
	timeout_add_sec(&sc->tick, 1);
}

static void rl_tick(void *arg)
{
	struct rl_softc *sc = (struct rl_softc *) arg;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	rl_link(sc);
	ifp->if_ierrors += CSR_READ_4(sc, RL_MPC);   /* missed: no room in the ring */
	CSR_WRITE_4(sc, RL_MPC, 0);
	if (ifp->if_flags & IFF_RUNNING)
		timeout_add_sec(&sc->tick, 1);
}

/* ---- transmit ---------------------------------------------------------------------------------- */
static void rl_start(struct ifnet *ifp)
{
	struct rl_softc *sc = (struct rl_softc *) ifp->if_softc;
	struct mbuf *m;
	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
		return;
	for (;;)
	{
		unsigned int len;
		caddr_t buf;
		if (sc->tx_cnt == RL_NTX)
		{
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		m = ifq_dequeue(&ifp->if_snd);
		if (!m)
			break;
		len = m->m_pkthdr.len;
		if (len > RL_TXBUF)
		{
			m_freem(m);
			ifp->if_oerrors++;
			continue;
		}
		buf = sc->tx_buf + sc->tx_prod * RL_TXBUF;
		m_copydata(m, 0, len, buf);
		if (len < ETHER_MIN_LEN - ETHER_CRC_LEN)   /* the chip does not pad short frames */
		{
			memset(buf + len, 0, ETHER_MIN_LEN - ETHER_CRC_LEN - len);
			len = ETHER_MIN_LEN - ETHER_CRC_LEN;
		}
		m_freem(m);
		bus_dmamap_sync(sc->sc_dmat, sc->tx_map, sc->tx_prod * RL_TXBUF, RL_TXBUF, BUS_DMASYNC_PREWRITE);
		sc->tx_len[sc->tx_prod] = len;
		CSR_WRITE_4(sc, RL_TSD0 + 4 * sc->tx_prod, len | RL_TSD_THRESH(sc->tx_thresh));
		sc->tx_prod = (sc->tx_prod + 1) % RL_NTX;
		sc->tx_cnt++;
		ifp->if_timer = 5;
	}
}

static void rl_txeof(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	while (sc->tx_cnt > 0)
	{
		uint32_t tsd = CSR_READ_4(sc, RL_TSD0 + 4 * sc->tx_cons);
		if (!(tsd & (RL_TSD_TOK | RL_TSD_TUN | RL_TSD_TABT)))
			break;
		if (tsd & RL_TSD_TOK)
			ifp->if_opackets++;
		else
			ifp->if_oerrors++;
		if (tsd & RL_TSD_TUN)
		{
			/* underrun: start sending later next time */
			if (sc->tx_thresh < 1536)
				sc->tx_thresh += 64;
		}
		if (tsd & RL_TSD_TABT)
			CSR_WRITE_4(sc, RL_TCR, CSR_READ_4(sc, RL_TCR) | 0x01);   /* clear abort: retransmit */
		ifp->if_collisions += (tsd >> 24) & 0x0f;
		sc->tx_cons = (sc->tx_cons + 1) % RL_NTX;
		sc->tx_cnt--;
	}
	if (sc->tx_cnt == 0)
		ifp->if_timer = 0;
	if (sc->tx_cnt < RL_NTX)
		ifq_clr_oactive(&ifp->if_snd);
}

static void rl_watchdog(struct ifnet *ifp)
{
	struct rl_softc *sc = (struct rl_softc *) ifp->if_softc;
	printf("%s: transmit timeout\n", sc->sc_dev.dv_xname);
	ifp->if_oerrors++;
	rl_txeof(sc);
	if (sc->tx_cnt)
		rl_init(sc);
}

/* ---- receive ----------------------------------------------------------------------------------- */
static void rl_rx_reset(struct rl_softc *sc)
{
	/* the older chips lose their place after an overflow: restart receive */
	CSR_WRITE_1(sc, RL_CR, RL_CR_TE);
	CSR_WRITE_1(sc, RL_CR, RL_CR_RE | RL_CR_TE);
	rl_iff(sc);
	sc->rx_cur = 0;
	CSR_WRITE_4(sc, RL_RBSTART, (uint32_t) sc->rx_map->dm_segs[0].ds_addr);
	CSR_WRITE_2(sc, RL_CAPR, (uint16_t) (0 - 16));
}

static void rl_rxeof(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	int budget = 64;
	bus_dmamap_sync(sc->sc_dmat, sc->rx_map, 0, RL_RXBUF_ALLOC, BUS_DMASYNC_POSTREAD);
	while (!(CSR_READ_1(sc, RL_CR) & RL_CR_BUFE) && budget-- > 0)
	{
		uint8_t *p = (uint8_t *) sc->rx_buf + sc->rx_cur;
		uint16_t status = (uint16_t) (p[0] | p[1] << 8);
		uint16_t len = (uint16_t) (p[2] | p[3] << 8);
		struct mbuf *m;
		/* 0xfff0: the chip is still writing this frame's header */
		if (len == 0xfff0)
			break;
		if (!(status & RL_RXSTAT_ROK) || len < ETHER_MIN_LEN || len > ETHER_MAX_LEN + 4)
		{
			ifp->if_ierrors++;
			rl_rx_reset(sc);
			break;
		}
		/* WRAP is on: the frame is contiguous even past the ring's end */
		m = m_devget((char *) p + 4, len - ETHER_CRC_LEN, ETHER_ALIGN);
		if (m)
			ml_enqueue(&ml, m);
		else
			ifp->if_iqdrops++;
		sc->rx_cur = (uint16_t) (((sc->rx_cur + len + 4 + 3) & ~3u) % RL_RXBUF);
		CSR_WRITE_2(sc, RL_CAPR, (uint16_t) (sc->rx_cur - 16));
	}
	bus_dmamap_sync(sc->sc_dmat, sc->rx_map, 0, RL_RXBUF_ALLOC, BUS_DMASYNC_PREREAD);
	if_input(ifp, &ml);
}

/* ---- interrupts --------------------------------------------------------------------------------- */
static int rl_isr(struct device *self)
{
	struct rl_softc *sc = (struct rl_softc *) self;
	uint16_t isr;
	if (!sc->sc_sh)
		return 0;
	isr = CSR_READ_2(sc, RL_ISR);
	if (isr == 0 || isr == 0xffff)
		return 0;   /* not ours (shared line), or the card is gone */
	/* only causes we enabled assert the line: status bits that are set but
	 * masked (link, timer, ...) mean another device on a shared line is
	 * interrupting. Claiming it starves that device's handler and the line
	 * stays asserted: an interrupt storm, a hang at boot. */
	if (!(isr & CSR_READ_2(sc, RL_IMR)))
		return 0;
	CSR_WRITE_2(sc, RL_IMR, 0);
	__sync_fetch_and_or(&sc->isr, isr);
	return 1;
}

static int rl_intr(void *arg)
{
	struct rl_softc *sc = (struct rl_softc *) arg;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	uint16_t isr = __sync_lock_test_and_set(&sc->isr, 0) | CSR_READ_2(sc, RL_ISR);
	if (!isr)
		return 0;
	CSR_WRITE_2(sc, RL_ISR, isr);   /* acknowledge before reading the ring */
	if (!(ifp->if_flags & IFF_RUNNING))
		return 1;
	if (isr & RL_INT_SERR)
	{
		printf("%s: PCI system error, resetting\n", sc->sc_dev.dv_xname);
		rl_init(sc);
		return 1;
	}
	if (isr & (RL_INT_ROK | RL_INT_RER | RL_INT_RXOVW | RL_INT_FOVW))
	{
		rl_rxeof(sc);
		if ((isr & (RL_INT_RXOVW | RL_INT_FOVW)) && !sc->chip_cplus)
		{
			ifp->if_ierrors++;
			rl_rx_reset(sc);
		}
	}
	if (isr & (RL_INT_TOK | RL_INT_TER))
		rl_txeof(sc);
	if (isr & RL_INT_LINK)
		rl_link(sc);
	if (!ifq_empty(&ifp->if_snd))
		rl_start(ifp);
	return 1;
}

static void rl_intr_enable(struct device *self)
{
	struct rl_softc *sc = (struct rl_softc *) self;
	if (sc->sc_ac.ac_if.if_flags & IFF_RUNNING)
		CSR_WRITE_2(sc, RL_IMR, RL_INTRS);
}

static void rl_shutdown(struct device *self)
{
	struct rl_softc *sc = (struct rl_softc *) self;
	if (sc->sc_sh)
	{
		CSR_WRITE_2(sc, RL_IMR, 0);
		CSR_WRITE_1(sc, RL_CR, 0);
	}
}

/* ---- ioctl --------------------------------------------------------------------------------------- */
static int rl_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct rl_softc *sc = (struct rl_softc *) ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *) data;
	int error = 0;
	switch (cmd)
	{
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (!(ifp->if_flags & IFF_RUNNING))
			rl_init(sc);
		break;
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
		{
			if (ifp->if_flags & IFF_RUNNING)
				error = ENETRESET;
			else
				rl_init(sc);
		}
		else if (ifp->if_flags & IFF_RUNNING)
			rl_stop(sc);
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;
	default:
		error = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
	}
	if (error == ENETRESET)
	{
		if (ifp->if_flags & IFF_RUNNING)
			rl_iff(sc);
		error = 0;
	}
	return error;
}

/* ---- attach ------------------------------------------------------------------------------------- */
static int dma_buffer(struct rl_softc *sc, bus_size_t size, bus_dma_segment_t *seg, bus_dmamap_t *map, caddr_t *va)
{
	int nsegs;
	if (bus_dmamem_alloc(sc->sc_dmat, size, 16, 0, seg, 1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO))
		return ENOMEM;
	if (bus_dmamem_map(sc->sc_dmat, seg, nsegs, size, va, BUS_DMA_NOWAIT))
		return ENOMEM;
	if (bus_dmamap_create(sc->sc_dmat, size, 1, size, 0, BUS_DMA_NOWAIT, map))
		return ENOMEM;
	if (bus_dmamap_load(sc->sc_dmat, *map, *va, size, NULL, BUS_DMA_NOWAIT))
		return ENOMEM;
	return 0;
}

static void rl_attach(struct device *parent, struct device *self, void *aux)
{
	struct rl_softc *sc = (struct rl_softc *) self;
	struct pci_attach_args *pa = (struct pci_attach_args *) aux;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	pci_intr_handle_t ih;
	int i;
	(void) parent;

	sc->sc_dmat = pa->pa_dmat;
	sc->revid = PCI_REVISION(pa->pa_class);
	/* registers: memory BAR (1), else I/O BAR (0) */
	if (pci_mapreg_map(pa, PCI_MAPREG_START + 4, PCI_MAPREG_TYPE_MEM | PCI_MAPREG_MEM_TYPE_32BIT, 0, &sc->sc_st,
	                   &sc->sc_sh, NULL, &sc->sc_sz, 0) &&
	    pci_mapreg_map(pa, PCI_MAPREG_START, PCI_MAPREG_TYPE_IO, 0, &sc->sc_st, &sc->sc_sh, NULL, &sc->sc_sz, 0))
	{
		printf(": cannot map registers\n");
		sc->sc_sh = 0;
		return;
	}
	/* power on (some BIOSes leave the chip in D3 or with PCI power management engaged) */
	CSR_WRITE_1(sc, RL_9346CR, 0xc0);
	CSR_WRITE_1(sc, RL_CONFIG1, CSR_READ_1(sc, RL_CONFIG1) & ~0x03);
	CSR_WRITE_1(sc, RL_9346CR, 0x00);
	rl_reset(sc);
	/* the TCR hardware version: 8139C+ (and the 8100/8101 that share its MAC) handle overflow themselves */
	{
		uint32_t hwrev = CSR_READ_4(sc, RL_TCR) & 0x7cc00000;
		/* 8139C, 8100B/8139D, 8139C+, 8101, 8100: the newer MACs */
		sc->chip_cplus = hwrev == 0x74000000 || hwrev == 0x74400000 || hwrev == 0x74800000 ||
		                 hwrev == 0x74c00000 || hwrev == 0x78800000;
		printf(": RTL8139 family (hw %08lx%s)", (unsigned long) hwrev, sc->chip_cplus ? ", C or later" : "");
	}
	for (i = 0; i < ETHER_ADDR_LEN; i++)
		sc->sc_ac.ac_enaddr[i] = CSR_READ_1(sc, RL_IDR0 + i);

	if (dma_buffer(sc, RL_RXBUF_ALLOC, &sc->rx_seg, &sc->rx_map, &sc->rx_buf) ||
	    dma_buffer(sc, RL_NTX * RL_TXBUF, &sc->tx_seg, &sc->tx_map, &sc->tx_buf))
	{
		printf(": cannot allocate DMA memory\n");
		return;
	}
	if (pci_intr_map(pa, &ih))
	{
		printf(": cannot map interrupt\n");
		return;
	}
	sc->sc_ih = pci_intr_establish(pa->pa_pc, ih, IPL_NET, rl_intr, sc, sc->sc_dev.dv_xname);
	if (!sc->sc_ih)
	{
		printf(": cannot establish interrupt\n");
		return;
	}
	printf(", address %s\n", ether_sprintf(sc->sc_ac.ac_enaddr));
	timeout_set(&sc->tick, rl_tick, sc);

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = rl_ioctl;
	ifp->if_start = rl_start;
	ifp->if_watchdog = rl_watchdog;
	ifp->if_baudrate = IF_Mbps(100);
	memcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
	ifmedia_init(&sc->sc_media, 0, rl_media_change, rl_media_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_100_TX | IFM_FDX, 0, NULL);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_100_TX, 0, NULL);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_10_T | IFM_FDX, 0, NULL);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_10_T, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);
	if_attach(ifp);
	ether_ifattach(ifp);
}

static const struct kexnet_driver rl_driver = {
	"rl",
	"Realtek RTL8139/810x Fast Ethernet (KernelExEx)",
	sizeof(struct rl_softc),
	rl_match,
	rl_attach,
	rl_isr,
	rl_intr_enable,
	rl_shutdown,
	0,
	NULL,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &rl_driver);
}
