/*
 *  KernelExEx - kexvio.sys: the virtio network adapter (virtio-net) of
 *  QEMU/KVM, Proxmox, VirtualBox and other virtual machines, for Windows 98
 *  SE and ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Our own driver, written from the virtio specification (legacy PCI
 *  interface, which transitional devices such as QEMU's default
 *  virtio-net-pci and VirtualBox's offer), in the OpenBSD driver style that
 *  obsd_net.c turns into an NDIS miniport, like kexrl. No offloads: one
 *  receive and one transmit queue; each frame is two descriptors (the
 *  10-byte virtio_net_hdr, then the frame), as legacy devices require
 *  without ANY_LAYOUT. Features used: MAC (the address the host set) and
 *  STATUS (link up/down).
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

/* ---- legacy virtio PCI registers (I/O BAR 0) ------------------------------------------ */
#define VIO_HOST_FEATURES 0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN 0x08
#define VIO_QUEUE_SIZE 0x0c
#define VIO_QUEUE_SELECT 0x0e
#define VIO_QUEUE_NOTIFY 0x10
#define VIO_STATUS 0x12
#define VIO_ISR 0x13
#define VIO_CONFIG 0x14   /* device configuration (no MSI-X on Windows 9x) */

#define VIO_S_ACK 0x01
#define VIO_S_DRIVER 0x02
#define VIO_S_DRIVER_OK 0x04
#define VIO_S_FAILED 0x80

#define VIO_ISR_QUEUE 0x01
#define VIO_ISR_CONFIG 0x02

#define VIO_NET_F_MAC (1u << 5)
#define VIO_NET_F_STATUS (1u << 16)
#define VIO_NET_S_LINK_UP 1

/* ---- virtqueues --------------------------------------------------------------------- */
struct vring_desc
{
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __packed;
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_used_elem
{
	uint32_t id;
	uint32_t len;
} __packed;

#define VIO_ALIGN 4096
#define VIO_HDR 10          /* struct virtio_net_hdr, no MRG_RXBUF */
#define VIO_BUF 2048        /* per frame: header at 0, frame at VIO_DATA */
#define VIO_DATA 16
#define VIO_FRAME (ETHER_MAX_LEN - ETHER_CRC_LEN)   /* 1514 */
#define VIO_MAX_SLOTS 128

struct vio_queue
{
	int index;
	unsigned int size;            /* entries, from the device */
	unsigned int slots;           /* frames: two descriptors each */
	bus_dma_segment_t ring_seg, buf_seg;
	bus_dmamap_t ring_map, buf_map;
	caddr_t ring, bufs;
	struct vring_desc *desc;
	volatile uint16_t *avail_flags, *avail_idx, *avail_ring;
	volatile uint16_t *used_flags, *used_idx;
	volatile struct vring_used_elem *used_ring;
	uint16_t avail_shadow, used_last;
	unsigned int inflight;
	uint8_t busy[VIO_MAX_SLOTS];  /* tx: slot in use */
};

struct vio_softc
{
	struct device sc_dev;
	struct arpcom sc_ac;
	struct ifmedia sc_media;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
	bus_size_t sc_sz;
	bus_dma_tag_t sc_dmat;
	void *sc_ih;
	uint32_t features;
	struct vio_queue rxq, txq;
	volatile uint8_t isr;   /* causes the ISR saw, not yet handled */
	unsigned int tx_next;
	int ok;
};

#define R1(sc, r) bus_space_read_1((sc)->sc_st, (sc)->sc_sh, (r))
#define R2(sc, r) bus_space_read_2((sc)->sc_st, (sc)->sc_sh, (r))
#define R4(sc, r) bus_space_read_4((sc)->sc_st, (sc)->sc_sh, (r))
#define W1(sc, r, v) bus_space_write_1((sc)->sc_st, (sc)->sc_sh, (r), (v))
#define W2(sc, r, v) bus_space_write_2((sc)->sc_st, (sc)->sc_sh, (r), (v))
#define W4(sc, r, v) bus_space_write_4((sc)->sc_st, (sc)->sc_sh, (r), (v))
#define BARRIER() __sync_synchronize()

static int vio_match(struct device *parent, void *match, void *aux)
{
	struct pci_attach_args *pa = (struct pci_attach_args *) aux;
	(void) parent;
	(void) match;
	/* 1AF4:1000 is the transitional (legacy-capable) network device */
	return PCI_VENDOR(pa->pa_id) == 0x1af4 && PCI_PRODUCT(pa->pa_id) == 0x1000;
}

static int dma_buffer(struct vio_softc *sc, bus_size_t size, bus_dma_segment_t *seg, bus_dmamap_t *map, caddr_t *va)
{
	int nsegs;
	if (bus_dmamem_alloc(sc->sc_dmat, size, PAGE_SIZE, 0, seg, 1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO))
		return ENOMEM;
	if (bus_dmamem_map(sc->sc_dmat, seg, nsegs, size, va, BUS_DMA_NOWAIT))
		return ENOMEM;
	if (bus_dmamap_create(sc->sc_dmat, size, 1, size, 0, BUS_DMA_NOWAIT, map))
		return ENOMEM;
	if (bus_dmamap_load(sc->sc_dmat, *map, *va, size, NULL, BUS_DMA_NOWAIT))
		return ENOMEM;
	return 0;
}

static uint64_t buf_pa(struct vio_queue *q, unsigned int slot, unsigned int off)
{
	return (uint64_t) q->buf_map->dm_segs[0].ds_addr + slot * VIO_BUF + off;
}

/* the legacy layout: descriptors, available ring, then (aligned to 4 KB)
 * the used ring; the device is told the page number */
static int queue_setup(struct vio_softc *sc, struct vio_queue *q, int index, unsigned int max_slots)
{
	unsigned int n, avail_sz, used_off, total;
	q->index = index;
	W2(sc, VIO_QUEUE_SELECT, (uint16_t) index);
	n = R2(sc, VIO_QUEUE_SIZE);
	if (n == 0 || (n & (n - 1)))
	{
		printf(": queue %d has size %u\n", index, n);
		return ENXIO;
	}
	q->size = n;
	q->slots = n / 2 < max_slots ? n / 2 : max_slots;
	avail_sz = 6 + 2 * n;
	used_off = (16 * n + avail_sz + VIO_ALIGN - 1) & ~(VIO_ALIGN - 1);
	total = used_off + ((6 + 8 * n + VIO_ALIGN - 1) & ~(VIO_ALIGN - 1));
	if (dma_buffer(sc, total, &q->ring_seg, &q->ring_map, &q->ring) ||
	    dma_buffer(sc, q->slots * VIO_BUF, &q->buf_seg, &q->buf_map, &q->bufs))
		return ENOMEM;
	q->desc = (struct vring_desc *) q->ring;
	q->avail_flags = (volatile uint16_t *) (q->ring + 16 * n);
	q->avail_idx = q->avail_flags + 1;
	q->avail_ring = q->avail_flags + 2;
	q->used_flags = (volatile uint16_t *) (q->ring + used_off);
	q->used_idx = q->used_flags + 1;
	q->used_ring = (volatile struct vring_used_elem *) (q->ring + used_off + 4);
	W4(sc, VIO_QUEUE_PFN, (uint32_t) (q->ring_map->dm_segs[0].ds_addr / VIO_ALIGN));
	return 0;
}

static void queue_reset(struct vio_queue *q)
{
	memset(q->ring, 0, (size_t) q->ring_map->dm_mapsize);
	q->avail_shadow = q->used_last = 0;
	q->inflight = 0;
	memset(q->busy, 0, sizeof(q->busy));
}

static void notify(struct vio_softc *sc, struct vio_queue *q)
{
	BARRIER();
	W2(sc, VIO_QUEUE_NOTIFY, (uint16_t) q->index);
}

/* one frame (slot) = descriptors 2*slot (header) and 2*slot+1 (frame) */
static void post(struct vio_queue *q, unsigned int slot, unsigned int frame_len, int write)
{
	struct vring_desc *d = &q->desc[2 * slot];
	d[0].addr = buf_pa(q, slot, 0);
	d[0].len = VIO_HDR;
	d[0].flags = VRING_DESC_F_NEXT | (write ? VRING_DESC_F_WRITE : 0);
	d[0].next = (uint16_t) (2 * slot + 1);
	d[1].addr = buf_pa(q, slot, VIO_DATA);
	d[1].len = frame_len;
	d[1].flags = write ? VRING_DESC_F_WRITE : 0;
	d[1].next = 0;
	q->avail_ring[q->avail_shadow % q->size] = (uint16_t) (2 * slot);
	q->avail_shadow++;
	q->inflight++;
	BARRIER();
	*q->avail_idx = q->avail_shadow;
}

/* ---- link ------------------------------------------------------------------------- */
static void vio_link(struct vio_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	int state = LINK_STATE_FULL_DUPLEX;
	if ((sc->features & VIO_NET_F_STATUS) && !(R2(sc, VIO_CONFIG + 6) & VIO_NET_S_LINK_UP))
		state = LINK_STATE_DOWN;
	if (ifp->if_link_state != state)
	{
		ifp->if_link_state = state;
		if_link_state_change(ifp);
	}
}

static int vio_media_change(struct ifnet *ifp)
{
	(void) ifp;
	return 0;
}

static void vio_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	ifmr->ifm_status = IFM_AVALID | (LINK_STATE_IS_UP(ifp->if_link_state) ? IFM_ACTIVE : 0);
	ifmr->ifm_active = IFM_ETHER | IFM_1000_T | IFM_FDX;
}

/* ---- start and stop ----------------------------------------------------------------- */
static void vio_stop(struct vio_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ifp->if_timer = 0;
	/* a reset stops all DMA; the queues are given again at init */
	W1(sc, VIO_STATUS, 0);
}

static int vio_init(struct vio_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	unsigned int i;
	vio_stop(sc);
	W1(sc, VIO_STATUS, VIO_S_ACK);
	W1(sc, VIO_STATUS, VIO_S_ACK | VIO_S_DRIVER);
	W4(sc, VIO_GUEST_FEATURES, sc->features);
	queue_reset(&sc->rxq);
	queue_reset(&sc->txq);
	W2(sc, VIO_QUEUE_SELECT, 0);
	W4(sc, VIO_QUEUE_PFN, (uint32_t) (sc->rxq.ring_map->dm_segs[0].ds_addr / VIO_ALIGN));
	W2(sc, VIO_QUEUE_SELECT, 1);
	W4(sc, VIO_QUEUE_PFN, (uint32_t) (sc->txq.ring_map->dm_segs[0].ds_addr / VIO_ALIGN));
	/* no interrupt for each frame sent: transmit slots are reclaimed as
	 * new frames go out, and on the receive interrupt */
	*sc->txq.avail_flags = VRING_AVAIL_F_NO_INTERRUPT;
	for (i = 0; i < sc->rxq.slots; i++)
		post(&sc->rxq, i, VIO_FRAME, 1);
	sc->tx_next = 0;
	W1(sc, VIO_STATUS, VIO_S_ACK | VIO_S_DRIVER | VIO_S_DRIVER_OK);
	notify(sc, &sc->rxq);
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ifp->if_link_state = LINK_STATE_UNKNOWN;
	vio_link(sc);
	return 0;
}

/* ---- transmit ----------------------------------------------------------------------- */
static void vio_txeof(struct vio_softc *sc)
{
	struct vio_queue *q = &sc->txq;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	BARRIER();
	while (q->used_last != *q->used_idx)
	{
		volatile struct vring_used_elem *u = &q->used_ring[q->used_last % q->size];
		unsigned int slot = u->id / 2;
		if (slot < q->slots && q->busy[slot])
		{
			q->busy[slot] = 0;
			q->inflight--;
			ifp->if_opackets++;
		}
		q->used_last++;
	}
	if (q->inflight == 0)
		ifp->if_timer = 0;
	if (q->inflight < q->slots)
		ifq_clr_oactive(&ifp->if_snd);
}

static void vio_start(struct ifnet *ifp)
{
	struct vio_softc *sc = (struct vio_softc *) ifp->if_softc;
	struct vio_queue *q = &sc->txq;
	struct mbuf *m;
	int sent = 0;
	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
		return;
	vio_txeof(sc);
	for (;;)
	{
		unsigned int len, slot, tries;
		caddr_t buf;
		if (q->inflight >= q->slots)
		{
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		for (tries = 0, slot = sc->tx_next; tries < q->slots && q->busy[slot]; tries++)
			slot = (slot + 1) % q->slots;
		if (q->busy[slot])
		{
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		m = ifq_dequeue(&ifp->if_snd);
		if (!m)
			break;
		len = m->m_pkthdr.len;
		if (len > VIO_FRAME)
		{
			m_freem(m);
			ifp->if_oerrors++;
			continue;
		}
		buf = q->bufs + slot * VIO_BUF;
		memset(buf, 0, VIO_HDR);   /* no checksum offload, no segmentation */
		m_copydata(m, 0, len, buf + VIO_DATA);
		m_freem(m);
		q->busy[slot] = 1;
		post(q, slot, len, 0);
		sc->tx_next = (slot + 1) % q->slots;
		sent++;
		ifp->if_timer = 5;
	}
	if (sent)
		notify(sc, q);
}

static void vio_watchdog(struct ifnet *ifp)
{
	struct vio_softc *sc = (struct vio_softc *) ifp->if_softc;
	vio_txeof(sc);
	if (sc->txq.inflight)
	{
		printf("%s: transmit timeout\n", sc->sc_dev.dv_xname);
		ifp->if_oerrors++;
		vio_init(sc);
	}
}

/* ---- receive ------------------------------------------------------------------------ */
static void vio_rxeof(struct vio_softc *sc)
{
	struct vio_queue *q = &sc->rxq;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	int reposted = 0;
	BARRIER();
	while (q->used_last != *q->used_idx)
	{
		volatile struct vring_used_elem *u = &q->used_ring[q->used_last % q->size];
		unsigned int slot = u->id / 2, len = u->len;
		q->used_last++;
		if (slot >= q->slots)
			continue;
		q->inflight--;
		if (len > VIO_HDR + ETHER_HDR_LEN && len <= VIO_HDR + VIO_FRAME)
		{
			struct mbuf *m = m_devget(q->bufs + slot * VIO_BUF + VIO_DATA, (int) (len - VIO_HDR), ETHER_ALIGN);
			if (m)
				ml_enqueue(&ml, m);
			else
				ifp->if_iqdrops++;
		}
		else
			ifp->if_ierrors++;
		post(q, slot, VIO_FRAME, 1);
		reposted++;
	}
	if (reposted)
		notify(sc, q);
	if_input(ifp, &ml);
}

/* ---- interrupts --------------------------------------------------------------------- */
static int vio_isr(struct device *self)
{
	struct vio_softc *sc = (struct vio_softc *) self;
	uint8_t isr;
	if (!sc->ok)
		return 0;
	/* reading ISR clears it and lowers the line: 0 means another device
	 * on a shared line */
	isr = R1(sc, VIO_ISR);
	if (isr == 0 || isr == 0xff)
		return 0;
	__sync_fetch_and_or(&sc->isr, isr);
	return 1;
}

static int vio_intr(void *arg)
{
	struct vio_softc *sc = (struct vio_softc *) arg;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	uint8_t isr = __sync_lock_test_and_set(&sc->isr, 0);
	if (!(ifp->if_flags & IFF_RUNNING))
		return 1;
	if (isr & VIO_ISR_CONFIG)
		vio_link(sc);
	vio_rxeof(sc);
	vio_txeof(sc);
	if (!ifq_empty(&ifp->if_snd))
		vio_start(ifp);
	return 1;
}

static void vio_intr_enable(struct device *self)
{
	(void) self;   /* nothing is masked: reading ISR lowered the line */
}

static void vio_shutdown(struct device *self)
{
	struct vio_softc *sc = (struct vio_softc *) self;
	if (sc->ok)
		W1(sc, VIO_STATUS, 0);
}

/* ---- ioctl ------------------------------------------------------------------------ */
static int vio_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct vio_softc *sc = (struct vio_softc *) ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *) data;
	int error = 0;
	switch (cmd)
	{
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (!(ifp->if_flags & IFF_RUNNING))
			vio_init(sc);
		break;
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
		{
			if (!(ifp->if_flags & IFF_RUNNING))
				vio_init(sc);
		}
		else if (ifp->if_flags & IFF_RUNNING)
			vio_stop(sc);
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;
	default:
		error = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
	}
	/* no control queue: the device passes every frame, the stack filters */
	if (error == ENETRESET)
		error = 0;
	return error;
}

/* ---- attach ----------------------------------------------------------------------- */
static void vio_attach(struct device *parent, struct device *self, void *aux)
{
	struct vio_softc *sc = (struct vio_softc *) self;
	struct pci_attach_args *pa = (struct pci_attach_args *) aux;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	pci_intr_handle_t ih;
	uint32_t host;
	int i;
	(void) parent;

	sc->sc_dmat = pa->pa_dmat;
	if (pci_mapreg_map(pa, PCI_MAPREG_START, PCI_MAPREG_TYPE_IO, 0, &sc->sc_st, &sc->sc_sh, NULL, &sc->sc_sz, 0))
	{
		printf(": no legacy I/O registers (a modern-only virtio device: set disable-legacy=off)\n");
		return;
	}
	W1(sc, VIO_STATUS, 0);   /* reset */
	W1(sc, VIO_STATUS, VIO_S_ACK);
	W1(sc, VIO_STATUS, VIO_S_ACK | VIO_S_DRIVER);
	host = R4(sc, VIO_HOST_FEATURES);
	sc->features = host & (VIO_NET_F_MAC | VIO_NET_F_STATUS);
	W4(sc, VIO_GUEST_FEATURES, sc->features);
	if (queue_setup(sc, &sc->rxq, 0, 64) || queue_setup(sc, &sc->txq, 1, 32))
	{
		W1(sc, VIO_STATUS, VIO_S_FAILED);
		printf(": cannot set up the queues\n");
		return;
	}
	if (sc->features & VIO_NET_F_MAC)
		for (i = 0; i < ETHER_ADDR_LEN; i++)
			sc->sc_ac.ac_enaddr[i] = R1(sc, VIO_CONFIG + i);
	else
	{
	{
		/* the host gave none: a locally administered one from the TSC */
		uint32_t lo, hi;
		__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
		sc->sc_ac.ac_enaddr[0] = 0x02;
		sc->sc_ac.ac_enaddr[1] = 0x4b;
		sc->sc_ac.ac_enaddr[2] = (uint8_t) (hi ^ lo >> 24);
		sc->sc_ac.ac_enaddr[3] = (uint8_t) (lo >> 16);
		sc->sc_ac.ac_enaddr[4] = (uint8_t) (lo >> 8);
		sc->sc_ac.ac_enaddr[5] = (uint8_t) lo;
	}
	}
	if (pci_intr_map(pa, &ih))
	{
		printf(": cannot map interrupt\n");
		return;
	}
	sc->sc_ih = pci_intr_establish(pa->pa_pc, ih, IPL_NET, vio_intr, sc, sc->sc_dev.dv_xname);
	if (!sc->sc_ih)
	{
		printf(": cannot establish interrupt\n");
		return;
	}
	sc->ok = 1;
	W1(sc, VIO_STATUS, 0);   /* quiet until the stack brings it up */
	printf(": virtio-net, queues %u/%u, features %08lx, address %s\n", sc->rxq.size, sc->txq.size,
	       (unsigned long) host, ether_sprintf(sc->sc_ac.ac_enaddr));

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = vio_ioctl;
	ifp->if_start = vio_start;
	ifp->if_watchdog = vio_watchdog;
	ifp->if_baudrate = IF_Gbps(1);
	memcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
	ifmedia_init(&sc->sc_media, 0, vio_media_change, vio_media_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);
	if_attach(ifp);
	ether_ifattach(ifp);
}

static const struct kexnet_driver vio_driver = {
	"vio",
	"virtio network adapter (KernelExEx)",
	sizeof(struct vio_softc),
	vio_match,
	vio_attach,
	vio_isr,
	vio_intr_enable,
	vio_shutdown,
	0,
	NULL,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &vio_driver);
}
