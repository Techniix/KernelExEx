/*
 *  KernelExEx - kexnet: OpenBSD network drivers as NDIS 5 miniports.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  An OpenBSD Ethernet driver (attach, ifnet, mbufs, if_input) runs
 *  unchanged under this glue:
 *
 *    NDIS MiniportInitialize  ->  match + attach, then SIOCSIFFLAGS(IFF_UP)
 *    NDIS MiniportSend        ->  frame copied into an mbuf on if_snd, if_qstart
 *    driver if_input          ->  NdisMEthIndicateReceive per frame
 *    driver ifq_clr_oactive   ->  NdisMSendResourcesAvailable
 *    driver timeout(9)        ->  NDIS miniport timers
 *    driver interrupt         ->  MiniportISR claims and masks (driver hook),
 *                                 MiniportHandleInterrupt runs the handler
 *    driver link state        ->  NDIS media connect / disconnect
 *
 *  The miniport is serialized: NDIS never runs two of its entry points at
 *  once (timers and the interrupt DPC included), which is what lets spl and
 *  the kernel lock be no-ops. Only the ISR runs concurrently, and it only
 *  touches the interrupt cause and mask registers.
 */
#include <ndis.h>
#include "obsd/kexobsd_net.h"
#include "dev/pci/pcireg.h"
#include "kexnet.h"

/* ---- freestanding string functions ----------------------------------------------- */
void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	if (p < q)
		while (n--)
			*p++ = *q++;
	else
		while (n--)
			p[n] = q[n];
	return d;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char) c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);
	if (n)
	{
		size_t c = l < n - 1 ? l : n - 1;
		memcpy(d, s, c);
		d[c] = 0;
	}
	return l;
}

/* ---- NDIS buffers ------------------------------------------------------------------- *
 * On Windows 9x an NDIS_BUFFER is NDIS's own structure, not an MDL, so the
 * NT macros (NdisQueryBuffer, NdisQueryPacket) must not be used; 98's
 * ndis.sys exports the functions. Only the first field, Next, is shared. */
__declspec(dllimport) VOID NTAPI kexnet_query_buffer(PNDIS_BUFFER b, PVOID *va, PUINT len)
    __asm__("_NdisQueryBuffer@12");

static PNDIS_BUFFER next_buffer(PNDIS_BUFFER b)
{
	return *(PNDIS_BUFFER *) b;
}

#ifdef KEX_DEBUGCON
#define TRACE(...) kexbsd_printf(__VA_ARGS__)
#else
#define TRACE(...) ((void) 0)
#endif

/* ---- adapters ----------------------------------------------------------------------- */
#define MAX_ADAPTERS 8
#define MAX_MULTI 32
#define FRAME_MAX 1514

typedef struct kexnet_adapter
{
	NDIS_HANDLE handle;
	const struct kexnet_driver *drv;
	device_t dev;
	struct device *sc;
	struct ifnet *ifp;
	struct pci_attach_args pa;
	/* interrupt */
	NDIS_MINIPORT_INTERRUPT intr;
	int intr_registered;
	int (*intr_fn)(void *);
	void *intr_arg;
	/* NDIS state */
	ULONG packet_filter;
	ULONG lookahead;
	int need_resources;
	int link_up;
	struct ether_multi multi[MAX_MULTI];
	int nmulti;
	/* the frame being indicated, for MiniportTransferData */
	unsigned char rxbuf[FRAME_MAX + 2];
	unsigned int rxlen;
	/* counters */
	ULONG tx_ok, tx_err, rx_ok, rx_err, rx_nobuf;
} KEXNET_ADAPTER;

static const struct kexnet_driver *the_driver;
static KEXNET_ADAPTER *adapters[MAX_ADAPTERS];
static KEXNET_ADAPTER *attaching;   /* during match/attach: timeouts and interrupts belong here */
static struct obsd_dma_tag
{
	int unused;
} dma_tag;

static KEXNET_ADAPTER *by_ifp(struct ifnet *ifp)
{
	int i;
	for (i = 0; i < MAX_ADAPTERS; i++)
		if (adapters[i] && adapters[i]->ifp == ifp)
			return adapters[i];
	return attaching && attaching->ifp == ifp ? attaching : NULL;
}

/* ---- PCI ------------------------------------------------------------------------------ */
static uint32_t cfg_read(device_t dev, int reg, int width)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) dev->frontend;
	uint32_t v = 0;
	NdisReadPciSlotInformation(a->handle, 0, (ULONG) reg, &v, (ULONG) width);
	return v;
}

static void cfg_write(device_t dev, int reg, uint32_t val, int width)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) dev->frontend;
	NdisWritePciSlotInformation(a->handle, 0, (ULONG) reg, &val, (ULONG) width);
}

int obsd_pci_matchbyid(struct pci_attach_args *pa, const struct pci_matchid *ids, int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (ids[i].pm_vid == PCI_VENDOR(pa->pa_id) && ids[i].pm_pid == PCI_PRODUCT(pa->pa_id))
			return 1;
	return 0;
}

int obsd_pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid, int *offset, pcireg_t *value)
{
	int off;
	(void) pc;
	if (pci_find_cap(tag, capid, &off))
		return 0;
	if (offset)
		*offset = off;
	if (value)
		*value = pci_read_config(tag, off, 4);
	return 1;
}

/* A BAR is matched to the range Windows assigned by its address. */
int obsd_pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags, bus_space_tag_t *tagp,
                        bus_space_handle_t *handlep, bus_addr_t *basep, bus_size_t *sizep, bus_size_t maxsize)
{
	device_t dev = pa->pa_tag;
	pcireg_t bar = pci_read_config(dev, reg, 4);
	int io = PCI_MAPREG_TYPE(bar) == PCI_MAPREG_TYPE_IO;
	uint32_t addr = io ? PCI_MAPREG_IO_ADDR(bar) : PCI_MAPREG_MEM_ADDR(bar);
	int i;
	(void) type;
	(void) flags;
	for (i = 0; i < dev->nres; i++)
	{
		struct resource *r = &dev->res[i];
		if (r->type != (io ? SYS_RES_IOPORT : SYS_RES_MEMORY) || r->start != addr)
			continue;
		if (!io && !r->va)
		{
			PHYSICAL_ADDRESS p;
			p.QuadPart = r->start;
			r->va = (volatile uint8_t *) MmMapIoSpace(p, r->size, MmNonCached);
			if (!r->va)
				return ENOMEM;
		}
		if (tagp)
			*tagp = io ? X86_BUS_SPACE_IO : X86_BUS_SPACE_MEM;
		if (handlep)
			*handlep = io ? (bus_space_handle_t) r->start : (bus_space_handle_t) r->va;
		if (basep)
			*basep = r->start;
		if (sizep)
			*sizep = maxsize && maxsize < r->size ? maxsize : r->size;
		return 0;
	}
	device_printf(dev, "BAR %02x (%08x) has no range assigned by Windows\n", reg, (unsigned) bar);
	return ENXIO;
}

int obsd_pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ih)
{
	int i;
	for (i = 0; i < pa->pa_tag->nres; i++)
		if (pa->pa_tag->res[i].type == SYS_RES_IRQ)
		{
			ih->line = 0;
			ih->dev = pa->pa_tag;
			return 0;
		}
	return 1;
}

void *obsd_pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int level, int (*fn)(void *), void *arg,
                              const char *name)
{
	KEXNET_ADAPTER *a = attaching;
	struct resource *r = NULL;
	NDIS_STATUS st;
	int i;
	(void) pc;
	(void) level;
	(void) name;
	if (!a || a->intr_fn)
		return NULL;
	for (i = 0; i < ih.dev->nres; i++)
		if (ih.dev->res[i].type == SYS_RES_IRQ)
			r = &ih.dev->res[i];
	if (!r)
		return NULL;
	a->intr_fn = fn;
	a->intr_arg = arg;
	st = NdisMRegisterInterrupt(&a->intr, a->handle, r->vector, r->irql, TRUE, r->shared,
	                            r->mode == Latched ? NdisInterruptLatched : NdisInterruptLevelSensitive);
	if (st != NDIS_STATUS_SUCCESS)
	{
		device_printf(a->dev, "NdisMRegisterInterrupt failed (%08x)\n", (unsigned) st);
		a->intr_fn = NULL;
		return NULL;
	}
	a->intr_registered = 1;
	return a;
}

void obsd_pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) cookie;
	(void) pc;
	if (a && a->intr_registered)
	{
		NdisMDeregisterInterrupt(&a->intr);
		a->intr_registered = 0;
		a->intr_fn = NULL;
	}
}

/* ---- timeout(9) on NDIS miniport timers -------------------------------------------- */
CTASSERT(sizeof(NDIS_MINIPORT_TIMER) <= sizeof(((struct timeout *) 0)->to_impl));

static VOID NTAPI timeout_thunk(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
	struct timeout *t = (struct timeout *) ctx;
	(void) s1;
	(void) s2;
	(void) s3;
	t->to_set = 1;
	t->to_impl[sizeof(NDIS_MINIPORT_TIMER) / 4] = 0;   /* no longer pending */
	t->to_func(t->to_arg);
}

#define PENDING(t) ((t)->to_impl[sizeof(NDIS_MINIPORT_TIMER) / 4])

void obsd_timeout_set(struct timeout *t, void (*fn)(void *), void *arg)
{
	memset(t, 0, sizeof(*t));
	t->to_func = fn;
	t->to_arg = arg;
	if (!attaching)
	{
		kexbsd_printf("kexnet: timeout_set outside attach is not supported\n");
		return;
	}
	NdisMInitializeTimer((PNDIS_MINIPORT_TIMER) t->to_impl, attaching->handle, timeout_thunk, t);
	t->to_set = 1;
}

int obsd_timeout_add_msec(struct timeout *t, int msec)
{
	int was = (int) PENDING(t);
	if (!t->to_set)
		return 0;
	if (msec < 1)
		msec = 1;
	PENDING(t) = 1;
	/* 98 SE exports NdisSetTimer, which an NDIS_MINIPORT_TIMER (KTIMER, KDPC
	 * first) satisfies; its DPC runs the miniport's function serialized */
	NdisSetTimer((PNDIS_TIMER) t->to_impl, (UINT) msec);
	return !was;
}

int obsd_timeout_del(struct timeout *t)
{
	BOOLEAN cancelled = FALSE;
	if (!t->to_set)
		return 0;
	NdisMCancelTimer((PNDIS_MINIPORT_TIMER) t->to_impl, &cancelled);
	PENDING(t) = 0;
	return cancelled;
}

int obsd_timeout_pending(struct timeout *t)
{
	return t->to_set && PENDING(t);
}

/* ---- mbuf(9) -------------------------------------------------------------------------- *
 * Storage up to MCLBYTES never crosses a page: the allocation is padded and
 * the data aligned to its size, so a receive buffer is one DMA segment. */
struct mbuf *obsd_m_gethdr(int how, int type)
{
	struct mbuf *m = (struct mbuf *) kexbsd_malloc(sizeof(*m), M_DEVBUF, M_NOWAIT | M_ZERO);
	(void) how;
	(void) type;
	if (!m)
		return NULL;
	m->m_flags = M_PKTHDR;
	m->m_buf = m->m_data = m->m_dat;
	m->m_bufsize = MHLEN;
	return m;
}

struct mbuf *obsd_m_clget(struct mbuf *m, int how, unsigned int size)
{
	struct mbuf *n = m;
	unsigned int align = size <= MCLBYTES ? MCLBYTES : PAGE_SIZE;
	void *p;
	if (!n && !(n = obsd_m_gethdr(how, MT_DATA)))
		return NULL;
	p = kexbsd_malloc(size + align, M_DEVBUF, M_NOWAIT);
	if (!p)
	{
		if (!m)
			obsd_m_free(n);
		return NULL;
	}
	n->m_alloc = p;
	n->m_buf = n->m_data = (caddr_t) (((ULONG_PTR) p + align - 1) & ~(ULONG_PTR) (align - 1));
	n->m_bufsize = size;
	n->m_flags |= M_EXT;
	return n;
}

struct mbuf *obsd_m_free(struct mbuf *m)
{
	struct mbuf *next;
	if (!m)
		return NULL;
	next = m->m_next;
	if (m->m_alloc)
		kexbsd_free(m->m_alloc, M_DEVBUF);
	kexbsd_free(m, M_DEVBUF);
	return next;
}

void obsd_m_freem(struct mbuf *m)
{
	while (m)
		m = obsd_m_free(m);
}

void obsd_m_adj(struct mbuf *m, int len)
{
	struct mbuf *n;
	if (!m)
		return;
	if (len >= 0)
	{
		int left = len;
		for (n = m; n && left; n = n->m_next)
		{
			int c = (int) n->m_len < left ? (int) n->m_len : left;
			n->m_data += c;
			n->m_len -= c;
			left -= c;
		}
		if (m->m_flags & M_PKTHDR)
			m->m_pkthdr.len -= len - left;
	}
	else
	{
		int total = 0, keep, trim = -len;
		for (n = m; n; n = n->m_next)
			total += n->m_len;
		keep = total > trim ? total - trim : 0;
		for (n = m; n; n = n->m_next)
		{
			if ((int) n->m_len >= keep)
			{
				n->m_len = keep;
				keep = 0;
			}
			else
				keep -= n->m_len;
		}
		if (m->m_flags & M_PKTHDR)
			m->m_pkthdr.len = total > trim ? total - trim : 0;
	}
}

void obsd_m_copydata(struct mbuf *m, int off, int len, void *cp)
{
	unsigned char *d = (unsigned char *) cp;
	for (; m && off >= (int) m->m_len; m = m->m_next)
		off -= m->m_len;
	for (; m && len > 0; m = m->m_next, off = 0)
	{
		int c = (int) m->m_len - off;
		if (c > len)
			c = len;
		memcpy(d, m->m_data + off, c);
		d += c;
		len -= c;
	}
}

/* coalesce a chain into the first mbuf's (new) storage */
int obsd_m_defrag(struct mbuf *m, int how)
{
	int len = m->m_pkthdr.len;
	void *p, *old = m->m_alloc;
	unsigned int align = MCLBYTES;
	caddr_t data;
	(void) how;
	if (!m->m_next)
		return 0;
	p = kexbsd_malloc(len + align, M_DEVBUF, M_NOWAIT);
	if (!p)
		return ENOBUFS;
	data = (caddr_t) (((ULONG_PTR) p + align - 1) & ~(ULONG_PTR) (align - 1));
	obsd_m_copydata(m, 0, len, data);
	obsd_m_freem(m->m_next);
	m->m_next = NULL;
	if (old)
		kexbsd_free(old, M_DEVBUF);
	m->m_alloc = p;
	m->m_buf = m->m_data = data;
	m->m_bufsize = len;
	m->m_len = len;
	m->m_flags |= M_EXT;
	return 0;
}

struct mbuf *obsd_m_devget(const char *buf, int len, int off)
{
	struct mbuf *m = obsd_m_clget(NULL, M_NOWAIT, len + off);
	if (!m)
		return NULL;
	m->m_data += off;
	memcpy(m->m_data, buf, len);
	m->m_len = m->m_pkthdr.len = len;
	return m;
}

int obsd_bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m, int flags)
{
	int e = 0;
	(void) t;
	(void) flags;
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	for (; m && !e; m = m->m_next)
		if (m->m_len)
			e = obsd_bus_dmamap_load_more(map, m->m_data, m->m_len);
	if (e)
	{
		map->dm_nsegs = 0;
		map->dm_mapsize = 0;
	}
	return e;
}

/* ---- Ethernet helpers -------------------------------------------------------------- */
void obsd_ether_extract_headers(struct mbuf *m, struct ether_extracted *ext)
{
	/* offload is never requested by this glue, so there is nothing to find */
	memset(ext, 0, sizeof(*ext));
	if (m && m->m_len >= ETHER_HDR_LEN)
		ext->eh = mtod(m, struct ether_header *);
}

char *obsd_ether_sprintf(const unsigned char *ap)
{
	static char buf[3 * ETHER_ADDR_LEN];
	kexbsd_snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", ap[0], ap[1], ap[2], ap[3], ap[4], ap[5]);
	return buf;
}

int obsd_ether_ioctl(struct ifnet *ifp, struct arpcom *ac, u_long cmd, caddr_t data)
{
	(void) ifp;
	(void) ac;
	(void) data;
	/* multicast changes arrive from NDIS; the glue then asks for a filter reset */
	return cmd == SIOCADDMULTI || cmd == SIOCDELMULTI ? ENETRESET : ENOTTY;
}

/* ---- ifnet ----------------------------------------------------------------------------- */
void obsd_if_attach(struct ifnet *ifp)
{
	if (attaching)
		attaching->ifp = ifp;
	ifp->if_snd.ifq_if = ifp;
	ifp->if_rcv.ifiq_if = ifp;
	if (!ifp->if_snd.ifq_maxlen)
		ifp->if_snd.ifq_maxlen = 256;
}

void obsd_ether_ifattach(struct ifnet *ifp)
{
	ifp->if_mtu = ETHERMTU;
	if (!ifp->if_hardmtu)
		ifp->if_hardmtu = ETHERMTU;
}

void obsd_if_detach(struct ifnet *ifp)
{
	ifq_purge(&ifp->if_snd);
}

void obsd_if_link_state_change(struct ifnet *ifp)
{
	KEXNET_ADAPTER *a = by_ifp(ifp);
	int up = LINK_STATE_IS_UP(ifp->if_link_state) && ifp->if_link_state != LINK_STATE_UNKNOWN;
	if (!a)
		return;
	device_printf(a->dev, "link %s", up ? "up" : "down");
	if (up)
		kexbsd_printf(", %lu Mbit/s", (unsigned long) (ifp->if_baudrate / 1000000));
	kexbsd_printf("\n");
	if (up == a->link_up)
		return;
	a->link_up = up;
	if (a != attaching)   /* NDIS asks for the state itself after initialize */
	{
		NdisMIndicateStatus(a->handle, up ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
		NdisMIndicateStatusComplete(a->handle);
	}
}

void obsd_ifq_clr_oactive(struct ifqueue *ifq)
{
	KEXNET_ADAPTER *a = by_ifp(ifq->ifq_if);
	ifq->ifq_oactive = 0;
	if (a && a->need_resources)
	{
		a->need_resources = 0;
		NdisMSendResourcesAvailable(a->handle);
	}
}

int obsd_ifiq_input(struct ifiqueue *ifiq, struct mbuf_list *ml)
{
	KEXNET_ADAPTER *a = by_ifp(ifiq->ifiq_if);
	struct mbuf *m;
	int any = 0;
	while ((m = ml_dequeue(ml)) != NULL)
	{
		int len = m->m_pkthdr.len;
		if (!a || len < ETHER_HDR_LEN || len > FRAME_MAX)
		{
			if (a)
				a->rx_err++;
			obsd_m_freem(m);
			continue;
		}
		obsd_m_copydata(m, 0, len, a->rxbuf);
		obsd_m_freem(m);
		a->rxlen = (unsigned int) len;
		a->rx_ok++;
		if (a->rx_ok <= 8)
			TRACE("kexnet: receive %d bytes, type %02x%02x\n", len, a->rxbuf[12], a->rxbuf[13]);
		any = 1;
		/* the whole frame is the lookahead, so protocols rarely transfer */
		NdisMEthIndicateReceive(a->handle, (NDIS_HANDLE) a, a->rxbuf, ETHER_HDR_LEN, a->rxbuf + ETHER_HDR_LEN,
		                        len - ETHER_HDR_LEN, len - ETHER_HDR_LEN);
	}
	if (any)
		NdisMEthIndicateReceiveComplete(a->handle);
	return 0;
}

/* ---- ifmedia ---------------------------------------------------------------------- */
void obsd_ifmedia_init(struct ifmedia *ifm, uint64_t dontcare, int (*change)(struct ifnet *),
                       void (*status)(struct ifnet *, struct ifmediareq *))
{
	memset(ifm, 0, sizeof(*ifm));
	ifm->ifm_mask = dontcare;
	ifm->ifm_change = change;
	ifm->ifm_status = status;
}

void obsd_ifmedia_add(struct ifmedia *ifm, uint64_t mword, int data, void *aux)
{
	if (ifm->ifm_count < IFMEDIA_MAX)
	{
		ifm->ifm_list[ifm->ifm_count].ifm_media = mword;
		ifm->ifm_list[ifm->ifm_count].ifm_data = (unsigned int) data;
		ifm->ifm_list[ifm->ifm_count].ifm_aux = aux;
		ifm->ifm_count++;
	}
}

void obsd_ifmedia_set(struct ifmedia *ifm, uint64_t mword)
{
	int i;
	ifm->ifm_media = mword;
	for (i = 0; i < ifm->ifm_count; i++)
		if (ifm->ifm_list[i].ifm_media == mword)
			ifm->ifm_cur = &ifm->ifm_list[i];
}

/* ---- NDIS entry points --------------------------------------------------------------- */
static const NDIS_OID supported[] = {
	OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
	OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
	OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
	OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_DRIVER_VERSION,
	OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS,
	OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_XMIT_OK, OID_GEN_RCV_OK,
	OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER, OID_802_3_PERMANENT_ADDRESS,
	OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
	OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
};

static VOID NTAPI mp_shutdown(PVOID ctx);

static void set_flags(KEXNET_ADAPTER *a, int flags)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	a->ifp->if_flags = (a->ifp->if_flags & ~(IFF_UP | IFF_PROMISC | IFF_ALLMULTI)) | flags;
	ifr.ifr_flags = a->ifp->if_flags;
	a->ifp->if_ioctl(a->ifp, SIOCSIFFLAGS, (caddr_t) &ifr);
}

static int filter_flags(ULONG f)
{
	int fl = IFF_UP;
	if (f & NDIS_PACKET_TYPE_PROMISCUOUS)
		fl |= IFF_PROMISC;
	if (f & NDIS_PACKET_TYPE_ALL_MULTICAST)
		fl |= IFF_ALLMULTI;
	return fl;
}

static NDIS_STATUS NTAPI mp_initialize(PNDIS_STATUS open_err, PUINT medium_index, PNDIS_MEDIUM media, UINT nmedia,
                                       NDIS_HANDLE handle, NDIS_HANDLE config)
{
	const struct kexnet_driver *drv = the_driver;
	KEXNET_ADAPTER *a = NULL;
	PNDIS_RESOURCE_LIST res = NULL;
	UINT i, rsize = 0;
	NDIS_STATUS st;
	int slot;
	unsigned char cfg[0x40];
	(void) open_err;

	for (i = 0; i < nmedia; i++)
		if (media[i] == NdisMedium802_3)
			break;
	if (i == nmedia)
		return NDIS_STATUS_UNSUPPORTED_MEDIA;
	*medium_index = i;
	for (slot = 0; slot < MAX_ADAPTERS && adapters[slot]; slot++)
		;
	if (slot == MAX_ADAPTERS)
		return NDIS_STATUS_RESOURCES;
	if (!NT_SUCCESS(kexbsd_framework_init()))
		return NDIS_STATUS_RESOURCES;

	a = (KEXNET_ADAPTER *) kexbsd_malloc(sizeof(*a), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!a)
		return NDIS_STATUS_RESOURCES;
	a->handle = handle;
	a->drv = drv;
	a->lookahead = FRAME_MAX - ETHER_HDR_LEN;
	a->dev = kexbsd_device_create(drv->name, slot, 0);
	if (!a->dev)
		goto fail;
	a->dev->frontend = a;
	a->dev->cfg_read = cfg_read;
	a->dev->cfg_write = cfg_write;

	/* the ranges and interrupt Windows assigned */
	NdisMQueryAdapterResources(&st, config, NULL, &rsize);
	if (!rsize || NdisAllocateMemoryWithTag((PVOID *) &res, rsize, 'tenK') != NDIS_STATUS_SUCCESS)
		goto fail;
	NdisMQueryAdapterResources(&st, config, res, &rsize);
	if (st != NDIS_STATUS_SUCCESS)
		goto fail;
	for (i = 0; i < res->Count && a->dev->nres < KEXBSD_MAX_RES; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &res->PartialDescriptors[i];
		struct resource *r = &a->dev->res[a->dev->nres];
		if (d->Type == CmResourceTypeMemory)
		{
			r->type = SYS_RES_MEMORY;
			r->start = d->u.Memory.Start.LowPart;
			r->size = d->u.Memory.Length;
		}
		else if (d->Type == CmResourceTypePort)
		{
			r->type = SYS_RES_IOPORT;
			r->start = d->u.Port.Start.LowPart;
			r->size = d->u.Port.Length;
		}
		else if (d->Type == CmResourceTypeInterrupt)
		{
			r->type = SYS_RES_IRQ;
			r->vector = d->u.Interrupt.Vector;
			r->irql = (KIRQL) d->u.Interrupt.Level;
			r->affinity = d->u.Interrupt.Affinity;
			r->mode = (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
			r->shared = d->ShareDisposition == CmResourceShareShared;
		}
		else
			continue;
		a->dev->nres++;
	}
	NdisFreeMemory(res, rsize, 0);
	res = NULL;

	/* identity */
	if (NdisReadPciSlotInformation(handle, 0, 0, cfg, sizeof(cfg)) < 0x30)
		goto fail;
	a->dev->vendor = (uint16_t) (cfg[0] | (cfg[1] << 8));
	a->dev->device = (uint16_t) (cfg[2] | (cfg[3] << 8));
	a->dev->revid = cfg[8];
	a->dev->progif = cfg[9];
	a->dev->subclass = cfg[10];
	a->dev->class_ = cfg[11];
	a->dev->subvendor = (uint16_t) (cfg[0x2C] | (cfg[0x2D] << 8));
	a->dev->subdevice = (uint16_t) (cfg[0x2E] | (cfg[0x2F] << 8));
	a->pa.pa_pc = NULL;
	a->pa.pa_tag = a->dev;
	a->pa.pa_id = (pcireg_t) a->dev->vendor | ((pcireg_t) a->dev->device << 16);
	a->pa.pa_class = (pcireg_t) a->dev->revid | ((pcireg_t) cfg[9] << 8) | ((pcireg_t) cfg[10] << 16) |
	                 ((pcireg_t) cfg[11] << 24);
	a->pa.pa_dmat = a->pa.pa_dmat64 = &dma_tag;
	a->pa.pa_flags = PCI_FLAGS_IO_ENABLED | PCI_FLAGS_MEM_ENABLED;
	a->pa.pa_iot = X86_BUS_SPACE_IO;
	a->pa.pa_memt = X86_BUS_SPACE_MEM;
	device_printf(a->dev, "%04x:%04x rev %02x, subsystem %04x:%04x\n", a->dev->vendor, a->dev->device,
	              a->dev->revid, a->dev->subvendor, a->dev->subdevice);

	/* bus master, interrupts through NDIS; must precede NdisMRegisterInterrupt */
	NdisMSetAttributesEx(handle, a, 0, NDIS_ATTRIBUTE_BUS_MASTER, NdisInterfacePci);
	pci_enable_busmaster(a->dev);

	a->sc = (struct device *) kexbsd_malloc(drv->softc_size, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!a->sc)
		goto fail;
	kexbsd_snprintf(a->sc->dv_xname, sizeof(a->sc->dv_xname), "%s%d", drv->name, slot);
	a->sc->dv_frontend = a->dev;

	attaching = a;
	if (!drv->match(NULL, NULL, &a->pa))
	{
		device_printf(a->dev, "not supported by the %s driver\n", drv->name);
		attaching = NULL;
		goto fail;
	}
	kexbsd_printf("%s at pci", a->sc->dv_xname);   /* the driver continues the line */
	drv->attach(NULL, a->sc, &a->pa);
	if (!a->ifp)
	{
		attaching = NULL;
		device_printf(a->dev, "attach failed\n");
		goto fail_attached;
	}
	strlcpy(a->ifp->if_xname, a->sc->dv_xname, sizeof(a->ifp->if_xname));

	/* a locally administered address from the adapter's Advanced properties */
	{
		PVOID addr;
		UINT alen;
		NDIS_HANDLE ch;
		NdisOpenConfiguration(&st, &ch, config);
		if (st == NDIS_STATUS_SUCCESS)
		{
			NdisReadNetworkAddress(&st, &addr, &alen, ch);
			if (st == NDIS_STATUS_SUCCESS && alen == ETHER_ADDR_LEN && !(((UCHAR *) addr)[0] & 1))
			{
				memcpy(((struct arpcom *) a->ifp)->ac_enaddr, addr, ETHER_ADDR_LEN);
				device_printf(a->dev, "using network address %s from the registry\n",
				              obsd_ether_sprintf((const unsigned char *) addr));
			}
			NdisCloseConfiguration(ch);
		}
	}

	adapters[slot] = a;
	NdisMRegisterAdapterShutdownHandler(handle, a, mp_shutdown);
	/* bring it up; protocols set the real packet filter when they bind */
	set_flags(a, IFF_UP);
	attaching = NULL;
	return NDIS_STATUS_SUCCESS;

fail_attached:
	if (a->intr_registered)
		NdisMDeregisterInterrupt(&a->intr);
fail:
	if (res)
		NdisFreeMemory(res, rsize, 0);
	if (a)
	{
		if (a->sc)
			kexbsd_free(a->sc, M_DEVBUF);
		if (a->dev)
			kexbsd_device_destroy(a->dev);
		kexbsd_free(a, M_DEVBUF);
	}
	return NDIS_STATUS_ADAPTER_NOT_FOUND;
}

static VOID NTAPI mp_halt(NDIS_HANDLE ctx)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	int i;
	set_flags(a, 0);   /* the driver's stop: interrupts off, rings freed */
	if (a->intr_registered)
		NdisMDeregisterInterrupt(&a->intr);
	for (i = 0; i < MAX_ADAPTERS; i++)
		if (adapters[i] == a)
			adapters[i] = NULL;
	/* the softc and its DMA memory stay: OpenBSD drivers do not all detach
	 * cleanly, and Windows 9x halts adapters only at shutdown */
}

static VOID NTAPI mp_shutdown(PVOID ctx)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	if (a->drv->shutdown)
		a->drv->shutdown(a->sc);
}

static NDIS_STATUS NTAPI mp_reset(PBOOLEAN addressing_reset, NDIS_HANDLE ctx)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	*addressing_reset = FALSE;
	device_printf(a->dev, "reset requested by NDIS\n");
	set_flags(a, 0);
	set_flags(a, filter_flags(a->packet_filter));
	return NDIS_STATUS_SUCCESS;
}

static BOOLEAN NTAPI mp_check_hang(NDIS_HANDLE ctx)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	/* OpenBSD's if_timer watchdog, at NDIS's two-second cadence */
	if (a->ifp->if_timer > 0)
	{
		a->ifp->if_timer -= 2;
		if (a->ifp->if_timer <= 0)
		{
			a->ifp->if_timer = 0;
			if (a->ifp->if_watchdog)
				a->ifp->if_watchdog(a->ifp);
		}
	}
	return FALSE;
}

static VOID NTAPI mp_isr(PBOOLEAN recognized, PBOOLEAN queue, NDIS_HANDLE ctx)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	*recognized = *queue = (BOOLEAN) (a->intr_fn && a->drv->isr(a->sc));
}

static VOID NTAPI mp_handle_interrupt(NDIS_HANDLE ctx)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	if (a->intr_fn)
		a->intr_fn(a->intr_arg);
	if (a->drv->intr_enable)
		a->drv->intr_enable(a->sc);
}

static NDIS_STATUS NTAPI mp_send(NDIS_HANDLE ctx, PNDIS_PACKET pkt, UINT flags)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	struct ifnet *ifp = a->ifp;
	PNDIS_BUFFER buf;
	PVOID va0;
	UINT len0, total;
	struct mbuf *m;
	caddr_t d;
	(void) flags;
	if (!(ifp->if_flags & IFF_RUNNING) || !a->link_up)
	{
		a->tx_err++;
		TRACE("kexnet: send dropped (flags %x, link %d)\n", ifp->if_flags, a->link_up);
		return NDIS_STATUS_SUCCESS;   /* dropped, as a cable-less adapter would */
	}
	if (ifq_is_oactive(&ifp->if_snd))
	{
		a->need_resources = 1;
		return NDIS_STATUS_RESOURCES;   /* NDIS queues it until SendResourcesAvailable */
	}
	NdisGetFirstBufferFromPacket(pkt, &buf, &va0, &len0, &total);
	if (total < ETHER_HDR_LEN || total > FRAME_MAX)
	{
		a->tx_err++;
		return NDIS_STATUS_FAILURE;
	}
	m = obsd_m_clget(NULL, M_NOWAIT, total);
	if (!m)
	{
		a->need_resources = 1;
		return NDIS_STATUS_RESOURCES;
	}
	d = m->m_data;
	for (; buf && d < m->m_data + total; buf = next_buffer(buf))
	{
		PVOID va;
		UINT len;
		kexnet_query_buffer(buf, &va, &len);
		if (len > (UINT) (m->m_data + total - d))
			len = (UINT) (m->m_data + total - d);
		memcpy(d, va, len);
		d += len;
	}
	m->m_len = m->m_pkthdr.len = (int) total;
	ml_enqueue(&ifp->if_snd.ifq_ml, m);
	ifp->if_qstart(&ifp->if_snd);
	a->tx_ok++;
	if (a->tx_ok <= 8)
		TRACE("kexnet: send %u bytes, type %02x%02x\n", total, (unsigned char) m->m_data[12],
		      (unsigned char) m->m_data[13]);
	return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NTAPI mp_transfer(PNDIS_PACKET pkt, PUINT copied, NDIS_HANDLE ctx, NDIS_HANDLE rctx, UINT off,
                                     UINT len)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	PNDIS_BUFFER buf;
	PVOID va0;
	UINT len0, total;
	const unsigned char *s;
	UINT avail;
	(void) rctx;
	*copied = 0;
	if (ETHER_HDR_LEN + off > a->rxlen)
		return NDIS_STATUS_FAILURE;
	s = a->rxbuf + ETHER_HDR_LEN + off;
	avail = a->rxlen - ETHER_HDR_LEN - off;
	if (len > avail)
		len = avail;
	NdisGetFirstBufferFromPacket(pkt, &buf, &va0, &len0, &total);
	for (; buf && len; buf = next_buffer(buf))
	{
		PVOID va;
		UINT blen, c;
		kexnet_query_buffer(buf, &va, &blen);
		c = blen < len ? blen : len;
		memcpy(va, s, c);
		s += c;
		len -= c;
		*copied += c;
	}
	return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS answer(PVOID buf, ULONG len, PULONG written, PULONG needed, const void *data, ULONG size)
{
	if (len < size)
	{
		*needed = size;
		return NDIS_STATUS_INVALID_LENGTH;
	}
	memcpy(buf, data, size);
	*written = size;
	return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS query(KEXNET_ADAPTER *a, NDIS_OID oid, PVOID buf, ULONG len, PULONG written, PULONG needed);

static NDIS_STATUS NTAPI mp_query(NDIS_HANDLE ctx, NDIS_OID oid, PVOID buf, ULONG len, PULONG written,
                                  PULONG needed)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	NDIS_STATUS st = query(a, oid, buf, len, written, needed);
	TRACE("kexnet: query %08lx len %lu -> %08lx (%lu)\n", (unsigned long) oid, (unsigned long) len,
	      (unsigned long) st, (unsigned long) (st == NDIS_STATUS_SUCCESS ? *written : *needed));
	return st;
}

static NDIS_STATUS query(KEXNET_ADAPTER *a, NDIS_OID oid, PVOID buf, ULONG len, PULONG written, PULONG needed)
{
	struct arpcom *ac = (struct arpcom *) a->ifp;
	ULONG v = 0;
	*written = 0;
	*needed = 0;
	switch (oid)
	{
	case OID_GEN_SUPPORTED_LIST:
		return answer(buf, len, written, needed, supported, sizeof(supported));
	case OID_GEN_HARDWARE_STATUS:
		v = NdisHardwareStatusReady;
		break;
	case OID_GEN_MEDIA_SUPPORTED:
	case OID_GEN_MEDIA_IN_USE:
		v = NdisMedium802_3;
		break;
	case OID_GEN_MAXIMUM_LOOKAHEAD:
		v = FRAME_MAX - ETHER_HDR_LEN;
		break;
	case OID_GEN_CURRENT_LOOKAHEAD:
		v = a->lookahead;
		break;
	case OID_GEN_MAXIMUM_FRAME_SIZE:
		v = ETHERMTU;
		break;
	case OID_GEN_MAXIMUM_TOTAL_SIZE:
	case OID_GEN_TRANSMIT_BLOCK_SIZE:
	case OID_GEN_RECEIVE_BLOCK_SIZE:
		v = FRAME_MAX;
		break;
	case OID_GEN_LINK_SPEED:   /* 100 bit/s units */
		v = a->link_up && a->ifp->if_baudrate ? (ULONG) (a->ifp->if_baudrate / 100) : 100000;
		break;
	case OID_GEN_TRANSMIT_BUFFER_SPACE:
	case OID_GEN_RECEIVE_BUFFER_SPACE:
		v = FRAME_MAX * 256;
		break;
	case OID_GEN_VENDOR_ID:
		v = (ULONG) ac->ac_enaddr[0] | ((ULONG) ac->ac_enaddr[1] << 8) | ((ULONG) ac->ac_enaddr[2] << 16);
		break;
	case OID_GEN_VENDOR_DESCRIPTION:
		return answer(buf, len, written, needed, a->drv->description, (ULONG) strlen(a->drv->description) + 1);
	case OID_GEN_VENDOR_DRIVER_VERSION:
		v = KEXNET_DRIVER_VERSION;
		break;
	case OID_GEN_DRIVER_VERSION:
		v = 0x0500;
		return answer(buf, len, written, needed, &v, 2);
	case OID_GEN_CURRENT_PACKET_FILTER:
		v = a->packet_filter;
		break;
	case OID_GEN_MAC_OPTIONS:
		v = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_NO_LOOPBACK;
		break;
	case OID_GEN_MEDIA_CONNECT_STATUS:
		v = a->link_up ? NdisMediaStateConnected : NdisMediaStateDisconnected;
		break;
	case OID_GEN_MAXIMUM_SEND_PACKETS:
		v = 1;
		break;
	case OID_GEN_XMIT_OK:
		v = a->tx_ok;
		break;
	case OID_GEN_RCV_OK:
		v = a->rx_ok;
		break;
	case OID_GEN_XMIT_ERROR:
		v = a->tx_err;
		break;
	case OID_GEN_RCV_ERROR:
		v = a->rx_err;
		break;
	case OID_GEN_RCV_NO_BUFFER:
		v = a->rx_nobuf;
		break;
	case OID_802_3_RCV_ERROR_ALIGNMENT:
	case OID_802_3_XMIT_ONE_COLLISION:
	case OID_802_3_XMIT_MORE_COLLISIONS:
		v = 0;
		break;
	case OID_802_3_PERMANENT_ADDRESS:
	case OID_802_3_CURRENT_ADDRESS:
		return answer(buf, len, written, needed, ac->ac_enaddr, ETHER_ADDR_LEN);
	case OID_802_3_MULTICAST_LIST:
	{
		int i;
		if (len < (ULONG) a->nmulti * ETHER_ADDR_LEN)
		{
			*needed = a->nmulti * ETHER_ADDR_LEN;
			return NDIS_STATUS_INVALID_LENGTH;
		}
		for (i = 0; i < a->nmulti; i++)
			memcpy((UCHAR *) buf + i * ETHER_ADDR_LEN, a->multi[i].enm_addrlo, ETHER_ADDR_LEN);
		*written = a->nmulti * ETHER_ADDR_LEN;
		return NDIS_STATUS_SUCCESS;
	}
	case OID_802_3_MAXIMUM_LIST_SIZE:
		v = MAX_MULTI;
		break;
	default:
		return NDIS_STATUS_NOT_SUPPORTED;
	}
	return answer(buf, len, written, needed, &v, sizeof(v));
}

static NDIS_STATUS set(KEXNET_ADAPTER *a, NDIS_OID oid, PVOID buf, ULONG len, PULONG read, PULONG needed);

static NDIS_STATUS NTAPI mp_set(NDIS_HANDLE ctx, NDIS_OID oid, PVOID buf, ULONG len, PULONG read, PULONG needed)
{
	KEXNET_ADAPTER *a = (KEXNET_ADAPTER *) ctx;
	NDIS_STATUS st = set(a, oid, buf, len, read, needed);
	TRACE("kexnet: set %08lx len %lu value %08lx -> %08lx\n", (unsigned long) oid, (unsigned long) len,
	      len >= 4 ? (unsigned long) *(ULONG *) buf : 0ul, (unsigned long) st);
	return st;
}

static NDIS_STATUS set(KEXNET_ADAPTER *a, NDIS_OID oid, PVOID buf, ULONG len, PULONG read, PULONG needed)
{
	struct arpcom *ac = (struct arpcom *) a->ifp;
	*read = 0;
	*needed = 0;
	switch (oid)
	{
	case OID_GEN_CURRENT_PACKET_FILTER:
	{
		ULONG f;
		if (len < 4)
		{
			*needed = 4;
			return NDIS_STATUS_INVALID_LENGTH;
		}
		f = *(ULONG *) buf;
		if (f & ~(ULONG) (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST |
		                  NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS))
			return NDIS_STATUS_NOT_SUPPORTED;
		*read = 4;
		if (filter_flags(f) != filter_flags(a->packet_filter))
		{
			a->packet_filter = f;
			set_flags(a, filter_flags(f));   /* the driver reprograms its filter (em_iff) */
		}
		else
			a->packet_filter = f;
		return NDIS_STATUS_SUCCESS;
	}
	case OID_GEN_CURRENT_LOOKAHEAD:
		if (len < 4)
		{
			*needed = 4;
			return NDIS_STATUS_INVALID_LENGTH;
		}
		a->lookahead = *(ULONG *) buf;
		*read = 4;
		return NDIS_STATUS_SUCCESS;
	case OID_802_3_MULTICAST_LIST:
	{
		int i, n = (int) (len / ETHER_ADDR_LEN);
		struct ifreq ifr;
		if (len % ETHER_ADDR_LEN)
			return NDIS_STATUS_INVALID_LENGTH;
		if (n > MAX_MULTI)
			return NDIS_STATUS_MULTICAST_FULL;
		ac->ac_multiaddrs = NULL;
		for (i = n - 1; i >= 0; i--)
		{
			memcpy(a->multi[i].enm_addrlo, (UCHAR *) buf + i * ETHER_ADDR_LEN, ETHER_ADDR_LEN);
			memcpy(a->multi[i].enm_addrhi, a->multi[i].enm_addrlo, ETHER_ADDR_LEN);
			a->multi[i].enm_next = ac->ac_multiaddrs;
			ac->ac_multiaddrs = &a->multi[i];
		}
		a->nmulti = n;
		ac->ac_multicnt = n;
		ac->ac_multirangecnt = 0;
		*read = len;
		memset(&ifr, 0, sizeof(ifr));
		a->ifp->if_ioctl(a->ifp, SIOCADDMULTI, (caddr_t) &ifr);   /* ENETRESET -> the driver's iff */
		return NDIS_STATUS_SUCCESS;
	}
	default:
		return NDIS_STATUS_NOT_SUPPORTED;
	}
}

NTSTATUS kexnet_register(PDRIVER_OBJECT drv, PUNICODE_STRING reg, const struct kexnet_driver *d)
{
	NDIS_MINIPORT_CHARACTERISTICS mc;
	NDIS_HANDLE wrapper;
	NDIS_STATUS st;
	the_driver = d;
	NdisMInitializeWrapper(&wrapper, drv, reg, NULL);
	memset(&mc, 0, sizeof(mc));
	mc.MajorNdisVersion = 5;
	mc.MinorNdisVersion = 0;
	mc.InitializeHandler = mp_initialize;
	mc.HaltHandler = mp_halt;
	mc.ResetHandler = mp_reset;
	mc.SendHandler = mp_send;
	mc.TransferDataHandler = mp_transfer;
	mc.QueryInformationHandler = mp_query;
	mc.SetInformationHandler = mp_set;
	mc.CheckForHangHandler = mp_check_hang;
	mc.ISRHandler = mp_isr;
	mc.HandleInterruptHandler = mp_handle_interrupt;
	st = NdisMRegisterMiniport(wrapper, &mc, sizeof(mc));
	if (st != NDIS_STATUS_SUCCESS)
		NdisTerminateWrapper(wrapper, NULL);
	return st;
}
