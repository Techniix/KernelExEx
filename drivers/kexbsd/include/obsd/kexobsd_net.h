/*
 *  KernelExEx - kexbsd OpenBSD personality: mbuf(9), ifnet, ifqueue,
 *  ifmedia and Ethernet, as OpenBSD network drivers use them.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  The network glue (drivers/kexnet/obsd_net.c) implements these and turns
 *  them into NDIS calls: if_input becomes a receive indication, frames from
 *  NDIS arrive on if_snd and the driver's start routine is called.
 */
#ifndef KEXBSD_KEXOBSD_NET_H
#define KEXBSD_KEXOBSD_NET_H

#include "obsd/kexobsd.h"

#undef SLIST_ENTRY   /* the Windows type; OpenBSD code means the queue(3) macro */
#include "sys/queue.h"
#include "sys/ioctl.h"

/* ---- mbuf(9) -------------------------------------------------------------------- */
#define MSIZE 256
#define MCLBYTES 2048
#define MHLEN 160
#define MLEN 160

struct pkthdr
{
	int len;
	uint16_t csum_flags;
	uint16_t ether_vtag;
	uint16_t ph_mss;
	uint16_t ph_flowid;
	void *ph_cookie;
	uint8_t ph_loopcnt;
	unsigned int ph_rtableid;
	unsigned int ph_ifidx;
};

struct m_ext
{
	caddr_t ext_buf;
	unsigned int ext_size;
};

struct mbuf
{
	struct mbuf *m_next;
	struct mbuf *m_nextpkt;
	caddr_t m_data;
	unsigned int m_len;
	int m_flags;
	int m_type;
	struct pkthdr m_pkthdr;
	struct m_ext m_ext;     /* storage: m_dat, or the cluster when M_EXT */
	void *m_alloc;          /* what to free */
	char m_dat[MHLEN];
};
#define m_buf m_ext.ext_buf
#define m_bufsize m_ext.ext_size

#define M_EXT 0x0001
#define M_PKTHDR 0x0002
#define M_EOR 0x0004
#define M_EXTWR 0x0008
#define M_BCAST 0x0100
#define M_MCAST 0x0200
#define M_VLANTAG 0x0020
#define M_LOOP 0x0040
#define M_ACAST 0x0400
#define M_CONF 0x0800
#define M_ZEROIZE 0x2000

/* m_pkthdr.csum_flags */
#define M_IPV4_CSUM_OUT 0x0001
#define M_TCP_CSUM_OUT 0x0002
#define M_UDP_CSUM_OUT 0x0004
#define M_IPV4_CSUM_IN_OK 0x0008
#define M_IPV4_CSUM_IN_BAD 0x0010
#define M_TCP_CSUM_IN_OK 0x0020
#define M_TCP_CSUM_IN_BAD 0x0040
#define M_UDP_CSUM_IN_OK 0x0080
#define M_UDP_CSUM_IN_BAD 0x0100
#define M_ICMP_CSUM_OUT 0x0200
#define M_ICMP_CSUM_IN_OK 0x0400
#define M_ICMP_CSUM_IN_BAD 0x0800
#define M_TCP_TSO 0x8000
#define M_FLOWID 0x4000

#define MT_DATA 1
#define MT_CONTROL 6
#define MINCLSIZE (MHLEN + 1)
#define M_COPYALL 1000000000
#define M_DATABUF(m) ((m)->m_buf)
#define M_SIZE(m) ((m)->m_bufsize)
#define M_READONLY(m) 0
#define mtod(m, t) ((t) ((m)->m_data))
#define M_LEADINGSPACE(m) ((unsigned int) ((m)->m_data - (m)->m_buf))
#define M_TRAILINGSPACE(m) ((m)->m_bufsize - M_LEADINGSPACE(m) - (m)->m_len)

struct mbuf *obsd_m_gethdr(int how, int type);
struct mbuf *obsd_m_clget(struct mbuf *m, int how, unsigned int size);
struct mbuf *obsd_m_free(struct mbuf *m);
void obsd_m_freem(struct mbuf *m);
void obsd_m_adj(struct mbuf *m, int len);
int obsd_m_defrag(struct mbuf *m, int how);
void obsd_m_copydata(struct mbuf *m, int off, int len, void *cp);
struct mbuf *obsd_m_devget(const char *buf, int len, int off);
#define MGETHDR(m, how, type) ((m) = obsd_m_gethdr((how), (type)))
#define MGET(m, how, type) ((m) = obsd_m_gethdr((how), (type)))
#define m_gethdr(how, type) obsd_m_gethdr((how), (type))
#define MCLGETL(m, how, size) obsd_m_clget((m), (how), (size))
#define MCLGET(m, how) ((void) obsd_m_clget((m), (how), MCLBYTES))
#define m_free(m) obsd_m_free(m)
#define m_freem(m) obsd_m_freem(m)
#define m_adj(m, l) obsd_m_adj((m), (l))
#define m_defrag(m, how) obsd_m_defrag((m), (how))
#define m_copydata(m, o, l, c) obsd_m_copydata((m), (o), (l), (c))
#define m_devget(b, l, o) obsd_m_devget((b), (l), (o))
int obsd_m_dup_pkthdr(struct mbuf *to, struct mbuf *from, int how);
struct mbuf *obsd_m_pullup(struct mbuf *m, int len);
struct mbuf *obsd_m_prepend(struct mbuf *m, int len, int how);
void obsd_m_align(struct mbuf *m, int len);
struct mbuf *obsd_m_dup_pkt(struct mbuf *m, unsigned int adj, int how);
struct mbuf *obsd_m_split(struct mbuf *m, int len, int how);
void obsd_m_cat(struct mbuf *m, struct mbuf *n);
#define m_dup_pkthdr(t, f, h) obsd_m_dup_pkthdr((t), (f), (h))
#define m_pullup(m, l) obsd_m_pullup((m), (l))
#define M_PREPEND(m, l, h) ((m) = obsd_m_prepend((m), (l), (h)))
#define m_prepend(m, l, h) obsd_m_prepend((m), (l), (h))
#define m_align(m, l) obsd_m_align((m), (l))
#define m_dup_pkt(m, a, h) obsd_m_dup_pkt((m), (a), (h))
#define m_split(m, l, h) obsd_m_split((m), (l), (h))
#define m_cat(m, n) obsd_m_cat((m), (n))
#define m_leadingspace(m) M_LEADINGSPACE(m)
#define m_trailingspace(m) M_TRAILINGSPACE(m)
#define m_tag_find(m, t, p) ((struct m_tag *) NULL)
#define m_tag_get(t, l, h) ((struct m_tag *) NULL)
#define m_tag_delete(m, t) ((void) 0)
struct m_tag
{
	int m_tag_id;
	int m_tag_len;
};
#define PACKET_TAG_DLT 0x0100


/* the network glue loads a chain into a map */
int obsd_bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m, int flags);
#define bus_dmamap_load_mbuf(t, map, m, f) obsd_bus_dmamap_load_mbuf((t), (map), (m), (f))

struct mbuf_list
{
	struct mbuf *ml_head;
	struct mbuf *ml_tail;
	unsigned int ml_len;
};
#define MBUF_LIST_INITIALIZER() { NULL, NULL, 0 }
#define ml_init(ml) ((ml)->ml_head = (ml)->ml_tail = NULL, (ml)->ml_len = 0)
#define ml_len(ml) ((ml)->ml_len)
#define ml_empty(ml) ((ml)->ml_len == 0)
static __inline__ void ml_enqueue(struct mbuf_list *ml, struct mbuf *m)
{
	m->m_nextpkt = NULL;
	if (ml->ml_tail)
		ml->ml_tail->m_nextpkt = m;
	else
		ml->ml_head = m;
	ml->ml_tail = m;
	ml->ml_len++;
}
static __inline__ struct mbuf *ml_dequeue(struct mbuf_list *ml)
{
	struct mbuf *m = ml->ml_head;
	if (m)
	{
		ml->ml_head = m->m_nextpkt;
		if (!ml->ml_head)
			ml->ml_tail = NULL;
		m->m_nextpkt = NULL;
		ml->ml_len--;
	}
	return m;
}
static __inline__ unsigned int ml_purge(struct mbuf_list *ml)
{
	struct mbuf *m;
	unsigned int n = 0;
	while ((m = ml_dequeue(ml)) != NULL)
	{
		obsd_m_freem(m);
		n++;
	}
	return n;
}

/* mbuf_queue: a locked mbuf_list */
struct mbuf_queue
{
	struct mutex mq_mtx;
	struct mbuf_list mq_list;
	unsigned int mq_maxlen;
	unsigned int mq_drops;
};
#define MBUF_QUEUE_INITIALIZER(len, ipl) { { { 0 } }, MBUF_LIST_INITIALIZER(), (len), 0 }
void obsd_mq_init(struct mbuf_queue *mq, unsigned int maxlen);
int obsd_mq_enqueue(struct mbuf_queue *mq, struct mbuf *m);
struct mbuf *obsd_mq_dequeue(struct mbuf_queue *mq);
unsigned int obsd_mq_purge(struct mbuf_queue *mq);
#define mq_init(mq, len, ipl) obsd_mq_init((mq), (len))
#define mq_enqueue(mq, m) obsd_mq_enqueue((mq), (m))
#define mq_dequeue(mq) obsd_mq_dequeue(mq)
#define mq_purge(mq) obsd_mq_purge(mq)
#define mq_len(mq) ((mq)->mq_list.ml_len)
#define mq_empty(mq) (mq_len(mq) == 0)
#define mq_full(mq) (mq_len(mq) >= (mq)->mq_maxlen)
#define mq_drops(mq) ((mq)->mq_drops)
#define IFQ_MAXLEN 256

/* ---- Ethernet ---------------------------------------------------------------------- */
#define ETHER_ADDR_LEN 6
#define ETHER_TYPE_LEN 2
#define ETHER_CRC_LEN 4
#define ETHER_HDR_LEN (ETHER_ADDR_LEN * 2 + ETHER_TYPE_LEN)
#define ETHER_MIN_LEN 64
#define ETHER_MAX_LEN 1518
#define ETHER_MAX_HARDMTU_LEN 65435
#define ETHER_VLAN_ENCAP_LEN 4
#define ETHER_ALIGN 2
#define ETHERMTU (ETHER_MAX_LEN - ETHER_HDR_LEN - ETHER_CRC_LEN)
#define ETHERMIN (ETHER_MIN_LEN - ETHER_HDR_LEN - ETHER_CRC_LEN)
#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_IPV6 0x86DD
#define ETHERTYPE_VLAN 0x8100
#define ETHERTYPE_EAPOL 0x888E
#define ETHERTYPE_PAE ETHERTYPE_EAPOL
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_QINQ 0x88A8
extern const uint8_t etherbroadcastaddr[ETHER_ADDR_LEN];
extern const uint8_t etheranyaddr[ETHER_ADDR_LEN];
uint32_t ether_crc32_le_update(uint32_t crc, const uint8_t *buf, size_t len);
uint32_t ether_crc32_le(const uint8_t *buf, size_t len);
uint32_t ether_crc32_be(const uint8_t *buf, size_t len);
#define ETHER_IS_MULTICAST(a) (*(a) & 0x01)
#define ETHER_IS_BROADCAST(a) (((a)[0] & (a)[1] & (a)[2] & (a)[3] & (a)[4] & (a)[5]) == 0xff)
#define ETHER_IS_EQ(a, b) (memcmp((a), (b), ETHER_ADDR_LEN) == 0)

struct ether_header
{
	uint8_t ether_dhost[ETHER_ADDR_LEN];
	uint8_t ether_shost[ETHER_ADDR_LEN];
	uint16_t ether_type;
} __packed;

struct ether_vlan_header
{
	uint8_t evl_dhost[ETHER_ADDR_LEN];
	uint8_t evl_shost[ETHER_ADDR_LEN];
	uint16_t evl_encap_proto;
	uint16_t evl_tag;
	uint16_t evl_proto;
} __packed;

/* headers ether_extract_headers() finds, for checksum and TSO offload */
struct ip;
struct ip6_hdr;
struct tcphdr;
struct udphdr;
struct ether_extracted
{
	struct ether_header *eh;
	struct ip *ip4;
	struct ip6_hdr *ip6;
	struct tcphdr *tcp;
	struct udphdr *udp;
	unsigned int iphlen;
	unsigned int tcphlen;
	unsigned int paylen;
	unsigned int evh_len;
	uint16_t evh_tag;
};
void obsd_ether_extract_headers(struct mbuf *m, struct ether_extracted *ext);
#define ether_extract_headers(m, e) obsd_ether_extract_headers((m), (e))
char *obsd_ether_sprintf(const unsigned char *ap);
#define ether_sprintf(a) obsd_ether_sprintf(a)

/* multicast list: the network glue keeps NDIS's list here */
struct ether_multi
{
	uint8_t enm_addrlo[ETHER_ADDR_LEN];
	uint8_t enm_addrhi[ETHER_ADDR_LEN];
	unsigned int enm_refcount;
	struct ether_multi *enm_next;
};
struct ether_multistep
{
	struct ether_multi *e_enm;
};
#define ETHER_NEXT_MULTI(step, enm) \
	do { if (((enm) = (step).e_enm) != NULL) (step).e_enm = (enm)->enm_next; } while (0)
#define ETHER_FIRST_MULTI(step, ac, enm) \
	do { (step).e_enm = (ac)->ac_multiaddrs; ETHER_NEXT_MULTI((step), (enm)); } while (0)

/* ---- ifnet ---------------------------------------------------------------------- */
#define IFNAMSIZ 16
#define IFF_UP 0x0001
#define IFF_BROADCAST 0x0002
#define IFF_DEBUG 0x0004
#define IFF_LOOPBACK 0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_STATICARP 0x0020
#define IFF_RUNNING 0x0040
#define IFF_NOARP 0x0080
#define IFF_PROMISC 0x0100
#define IFF_ALLMULTI 0x0200
#define IFF_OACTIVE 0x0400
#define IFF_SIMPLEX 0x0800
#define IFF_LINK0 0x1000
#define IFF_LINK1 0x2000
#define IFF_LINK2 0x4000
#define IFF_MULTICAST 0x8000
#define IFXF_MPSAFE 0x1
#define IFXF_MBUF_64BIT 0x2

#define IFCAP_CSUM_IPv4 0x00000001
#define IFCAP_CSUM_TCPv4 0x00000002
#define IFCAP_CSUM_UDPv4 0x00000004
#define IFCAP_VLAN_MTU 0x00000010
#define IFCAP_VLAN_HWTAGGING 0x00000020
#define IFCAP_CSUM_TCPv6 0x00000080
#define IFCAP_CSUM_UDPv6 0x00000100
#define IFCAP_TSOv4 0x00001000
#define IFCAP_TSOv6 0x00002000
#define IFCAP_LRO 0x00004000
#define IFCAP_WOL 0x00008000

#define LINK_STATE_UNKNOWN 0
#define LINK_STATE_INVALID 1
#define LINK_STATE_DOWN 2
#define LINK_STATE_KALIVE_DOWN 3
#define LINK_STATE_UP 4
#define LINK_STATE_HALF_DUPLEX 5
#define LINK_STATE_FULL_DUPLEX 6
#define LINK_STATE_IS_UP(s) ((s) >= LINK_STATE_UP || (s) == LINK_STATE_UNKNOWN)

#define IF_Kbps(x) ((uint64_t) (x) * 1000)
#define IF_Mbps(x) (IF_Kbps((x) * 1000))
#define IF_Gbps(x) (IF_Mbps((x) * 1000))

struct ifnet;
struct ifqueue
{
	struct ifnet *ifq_if;
	struct mbuf_list ifq_ml;
	unsigned int ifq_maxlen;
	volatile unsigned int ifq_oactive;
	uint64_t ifq_qdrops;
};
struct ifiqueue
{
	struct ifnet *ifiq_if;
};

struct if_rxring
{
	int rxr_adjusted;
	unsigned int rxr_alive;
	unsigned int rxr_cwm;
	unsigned int rxr_lwm;
	unsigned int rxr_hwm;
};
struct if_rxring_info
{
	char ifr_name[16];
	unsigned int ifr_size;
	struct if_rxring ifr_info;
};
static __inline__ void if_rxr_init(struct if_rxring *rxr, unsigned int lwm, unsigned int hwm)
{
	rxr->rxr_adjusted = 0;
	rxr->rxr_alive = 0;
	rxr->rxr_lwm = lwm;
	rxr->rxr_hwm = hwm;
	rxr->rxr_cwm = hwm;   /* no livelock adaptation: fill the whole ring */
}
static __inline__ unsigned int if_rxr_get(struct if_rxring *rxr, unsigned int max)
{
	unsigned int n = rxr->rxr_cwm > rxr->rxr_alive ? rxr->rxr_cwm - rxr->rxr_alive : 0;
	if (n > max)
		n = max;
	rxr->rxr_alive += n;
	return n;
}
#define if_rxr_put(rxr, n) ((rxr)->rxr_alive -= (n))
#define if_rxr_inuse(rxr) ((rxr)->rxr_alive)
#define if_rxr_needrefill(rxr) ((rxr)->rxr_alive < (rxr)->rxr_lwm)
#define if_rxr_livelocked(rxr) ((void) 0)
#define if_rxr_cwm(rxr) ((rxr)->rxr_cwm)

struct ifreq
{
	char ifr_name[IFNAMSIZ];
	union
	{
		int ifru_flags;
		int ifru_mtu;
		caddr_t ifru_data;
		unsigned int ifru_media;
	} ifr_ifru;
};
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_mtu ifr_ifru.ifru_mtu
#define ifr_data ifr_ifru.ifru_data
#define ifr_media ifr_ifru.ifru_media

struct if_sffpage
{
	char sff_ifname[IFNAMSIZ];
	uint8_t sff_addr;
	uint8_t sff_page;
	uint8_t sff_data[256];
};
#define IFSFF_ADDR_EEPROM 0xa0
#define IFSFF_ADDR_DDM 0xa2

struct ifmediareq;
struct sockaddr;
struct rtentry;
struct ifnet
{
	void *if_softc;
	char if_xname[IFNAMSIZ];
	int if_flags;
	int if_xflags;
	int if_timer;
	unsigned int if_hardmtu;
	unsigned int if_mtu;
	uint64_t if_baudrate;
	uint32_t if_capabilities;
	int if_link_state;
	void *if_bpf;
	struct ifqueue if_snd;
	struct ifiqueue if_rcv;
	int (*if_ioctl)(struct ifnet *, u_long, caddr_t);
	int (*if_output)(struct ifnet *, struct mbuf *, struct sockaddr *, struct rtentry *);
	unsigned int if_index;
	int if_type;
	unsigned char if_addrlen, if_hdrlen;
	unsigned short if_rtlabelid;
	void *if_l2com;
	void (*if_qstart)(struct ifqueue *);
	void (*if_start)(struct ifnet *);
	void (*if_watchdog)(struct ifnet *);
	uint64_t if_ipackets, if_opackets, if_ierrors, if_oerrors, if_collisions, if_iqdrops;
	uint64_t if_ibytes, if_obytes, if_imcasts, if_omcasts;
	int if_priority;
	struct
	{
		uint64_t ifi_ipackets, ifi_opackets, ifi_ierrors, ifi_oerrors, ifi_collisions, ifi_iqdrops;
	} if_data;
};
#define if_ipackets if_ipackets

struct arpcom
{
	struct ifnet ac_if;
	uint8_t ac_enaddr[ETHER_ADDR_LEN];
	struct ether_multi *ac_multiaddrs;
	int ac_multicnt;
	int ac_multirangecnt;
};

/* SIOC* values: the glue only issues SIOCSIFFLAGS; the rest just compile */
#define SIOCSIFADDR 0x8001
#define SIOCSIFFLAGS 0x8002
#define SIOCADDMULTI 0x8003
#define SIOCDELMULTI 0x8004
#define SIOCSIFMEDIA 0x8005
#define SIOCGIFMEDIA 0x8006
#define SIOCGIFRXR 0x8007
#define SIOCGIFSFFPAGE 0x8008
#define SIOCSIFMTU 0x8009
#undef ENETRESET
#undef ENOTTY
#undef ENOBUFS
#undef ENOEXEC
#undef EOVERFLOW
#define ENETRESET 52
#define ENOTTY 25
#define ENOBUFS 55
#define ENOEXEC 8
#define EOVERFLOW 84

void obsd_if_attach(struct ifnet *ifp);
void obsd_ether_ifattach(struct ifnet *ifp);
void obsd_if_detach(struct ifnet *ifp);
void obsd_if_link_state_change(struct ifnet *ifp);
int obsd_ifiq_input(struct ifiqueue *ifiq, struct mbuf_list *ml);
void obsd_ifq_clr_oactive(struct ifqueue *ifq);
int obsd_ether_ioctl(struct ifnet *ifp, struct arpcom *ac, u_long cmd, caddr_t data);
/* a function, as in OpenBSD: a macro would also rewrite ifp->if_start(ifp) */
void if_start(struct ifnet *ifp);
#define if_attach(ifp) obsd_if_attach(ifp)
#define IF_WIRELESS_DEFAULT_PRIORITY 4
#define if_addgroup(ifp, g) 0
#define if_deactivate(ifp) ((void) 0)
#define rtm_80211info(ifp, d) ((void) 0)
struct if_ieee80211_data
{
	uint8_t ifie_channel;
	uint8_t ifie_nwid_len;
	uint8_t ifie_nwid[32];
	uint8_t ifie_addr[6];
	uint32_t ifie_flags;
	uint32_t ifie_xflags;
};
int obsd_if_enqueue(struct ifnet *ifp, struct mbuf *m);
#define if_enqueue(ifp, m) obsd_if_enqueue((ifp), (m))
static __inline__ int ifq_enqueue(struct ifqueue *ifq, struct mbuf *m)
{
	if (ml_len(&ifq->ifq_ml) >= ifq->ifq_maxlen)
	{
		ifq->ifq_qdrops++;
		obsd_m_freem(m);
		return ENOBUFS;
	}
	ml_enqueue(&ifq->ifq_ml, m);
	return 0;
}
int obsd_ether_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst, struct rtentry *rt);
#define ether_output(ifp, m, d, rt) obsd_ether_output((ifp), (m), (d), (rt))
int obsd_ether_addmulti(struct ifreq *ifr, struct arpcom *ac);
int obsd_ether_delmulti(struct ifreq *ifr, struct arpcom *ac);
#define ether_addmulti(ifr, ac) obsd_ether_addmulti((ifr), (ac))
#define ether_delmulti(ifr, ac) obsd_ether_delmulti((ifr), (ac))
#define ether_ifattach(ifp) obsd_ether_ifattach(ifp)
#define if_detach(ifp) obsd_if_detach(ifp)
#define ether_ifdetach(ifp) ((void) 0)
#define if_link_state_change(ifp) obsd_if_link_state_change(ifp)
#define if_input(ifp, ml) obsd_ifiq_input(&(ifp)->if_rcv, (ml))
#define ifiq_input(ifiq, ml) obsd_ifiq_input((ifiq), (ml))
#define ether_ioctl(ifp, ac, cmd, data) obsd_ether_ioctl((ifp), (ac), (cmd), (data))
#define if_rxr_ioctl(ifri, name, size, rxr) ENOTTY
#define if_attach_queues(ifp, n) ((void) 0)
#define if_attach_iqueues(ifp, n) ((void) 0)

#define ifq_init_maxlen(ifq, n) ((ifq)->ifq_maxlen = (n))
#define ifq_set_maxlen(ifq, n) ((ifq)->ifq_maxlen = (n))
#define ifq_set_oactive(ifq) ((ifq)->ifq_oactive = 1)
#define ifq_clr_oactive(ifq) obsd_ifq_clr_oactive(ifq)
#define ifq_is_oactive(ifq) ((ifq)->ifq_oactive)
#define ifq_dequeue(ifq) ml_dequeue(&(ifq)->ifq_ml)
#define ifq_purge(ifq) ml_purge(&(ifq)->ifq_ml)
#define ifq_empty(ifq) ml_empty(&(ifq)->ifq_ml)
#define ifq_len(ifq) ml_len(&(ifq)->ifq_ml)
#define ifq_restart(ifq) ((ifq)->ifq_if->if_qstart(ifq))
#define ifq_barrier(ifq) ((void) 0)
#define ifq_serialize(ifq, t) ((void) 0)

/* ---- ifmedia ----------------------------------------------------------------------- */
#define IFM_ETHER 0x00000100
#define IFM_TMASK 0x0000000f
#define IFM_NMASK 0x0000ff00
#define IFM_IMASK 0xff00000000000000ULL
#define IFM_OMASK 0x0000ffff00000000ULL
#define IFM_GMASK 0x00ff000000000000ULL
#define IFM_MMASK 0x0000000000ff0000ULL
#define IFM_AUTO 0
#define IFM_MANUAL 1
#define IFM_NONE 2
#define IFM_10_T 3
#define IFM_10_2 4
#define IFM_10_5 5
#define IFM_100_TX 6
#define IFM_100_FX 7
#define IFM_100_T4 8
#define IFM_100_VG 9
#define IFM_100_T2 10
#define IFM_1000_SX 11
#define IFM_10_STP 12
#define IFM_10_FL 13
#define IFM_1000_LX 14
#define IFM_1000_CX 15
#define IFM_1000_T 16
#define IFM_1000_TX IFM_1000_T
#define IFM_HPNA_1 17
#define IFM_10G_LR 18
#define IFM_10G_SR 19
#define IFM_10G_CX4 20
#define IFM_2500_SX 21
#define IFM_2500_T 34
#define IFM_5000_T 35
#define IFM_1000_SGMII 36
#define IFM_1000_KX 29
#define IFM_2500_KX 33
#define IFM_FDX 0x0000010000000000ULL
#define IFM_HDX 0x0000020000000000ULL
#define IFM_FLOW 0x0000040000000000ULL
#define IFM_FLAG0 0x0000100000000000ULL
#define IFM_LOOP 0x0000400000000000ULL
#define IFM_ETH_MASTER 0x0000000000010000ULL
#define IFM_ETH_RXPAUSE 0x0000000000020000ULL
#define IFM_ETH_TXPAUSE 0x0000000000040000ULL
#define IFM_AVALID 0x0000000000000001ULL
#define IFM_ACTIVE 0x0000000000000002ULL
#define IFM_TYPE(x) ((x) & IFM_NMASK)
#define IFM_SUBTYPE(x) ((x) & 0x00000000000000ffULL)
#define IFM_INST(x) (((x) & IFM_IMASK) >> 56)
#define IFM_OPTIONS(x) ((x) & (IFM_OMASK | IFM_GMASK))

/* IEEE 802.11 media words. KernelExEx's own values (no ABI outside the
 * driver binary depends on them); subtypes are laid out so the MCS words
 * are ranges, as net80211 assumes. */
#define IFM_IEEE80211 0x00000400
#define IFM_IEEE80211_FH1 3
#define IFM_IEEE80211_FH2 4
#define IFM_IEEE80211_DS1 5
#define IFM_IEEE80211_DS2 6
#define IFM_IEEE80211_DS5 7
#define IFM_IEEE80211_DS11 8
#define IFM_IEEE80211_DS22 9
#define IFM_IEEE80211_OFDM6 10
#define IFM_IEEE80211_OFDM9 11
#define IFM_IEEE80211_OFDM12 12
#define IFM_IEEE80211_OFDM18 13
#define IFM_IEEE80211_OFDM24 14
#define IFM_IEEE80211_OFDM36 15
#define IFM_IEEE80211_OFDM48 16
#define IFM_IEEE80211_OFDM54 17
#define IFM_IEEE80211_OFDM72 18
#define IFM_IEEE80211_HT_MCS0 20
#define IFM_IEEE80211_HT_MCS76 (IFM_IEEE80211_HT_MCS0 + 76)
#define IFM_IEEE80211_VHT_MCS0 100
#define IFM_IEEE80211_VHT_MCS9 (IFM_IEEE80211_VHT_MCS0 + 9)
#define IFM_IEEE80211_HE_MCS0 110
#define IFM_IEEE80211_HE_MCS11 (IFM_IEEE80211_HE_MCS0 + 11)
#define IFM_IEEE80211_ADHOC 0x0000000100000000ULL
#define IFM_IEEE80211_HOSTAP 0x0000000200000000ULL
#define IFM_IEEE80211_IBSS 0x0000000400000000ULL
#define IFM_IEEE80211_MONITOR 0x0000000800000000ULL
#define IFM_IEEE80211_11A 0x0000000000010000ULL
#define IFM_IEEE80211_11B 0x0000000000020000ULL
#define IFM_IEEE80211_11G 0x0000000000030000ULL
#define IFM_IEEE80211_FH 0x0000000000040000ULL
#define IFM_IEEE80211_11N 0x0000000000050000ULL
#define IFM_IEEE80211_11AC 0x0000000000060000ULL
#define IFM_IEEE80211_11AX 0x0000000000070000ULL
#define IFM_MODE(x) ((x) & IFM_MMASK)
#define IFM_INST_ANY ((uint64_t) -1)
#define IFM_MAKEWORD(type, subtype, options, instance) \
	((uint64_t) (type) | (uint64_t) (subtype) | (uint64_t) (options) | ((uint64_t) (instance) << 56))

struct ifmedia_entry
{
	TAILQ_ENTRY(ifmedia_entry) ifm_list;
	uint64_t ifm_media;
	unsigned int ifm_data;
	void *ifm_aux;
};
TAILQ_HEAD(ifmedia_list, ifmedia_entry);
typedef int (*ifm_change_cb_t)(struct ifnet *);
typedef void (*ifm_stat_cb_t)(struct ifnet *, struct ifmediareq *);
struct ifmedia
{
	uint64_t ifm_mask;
	uint64_t ifm_media;
	struct ifmedia_entry *ifm_cur;
	struct ifmedia_list ifm_list;
	size_t ifm_nwords;
	ifm_change_cb_t ifm_change_cb;
	ifm_stat_cb_t ifm_status_cb;
};
struct ifmediareq
{
	char ifm_name[IFNAMSIZ];
	uint64_t ifm_current;
	uint64_t ifm_mask;
	uint64_t ifm_status;
	uint64_t ifm_active;
	int ifm_count;
	uint64_t *ifm_ulist;
};
void obsd_ifmedia_init(struct ifmedia *ifm, uint64_t dontcare, ifm_change_cb_t change, ifm_stat_cb_t status);
void obsd_ifmedia_add(struct ifmedia *ifm, uint64_t mword, int data, void *aux);
void obsd_ifmedia_set(struct ifmedia *ifm, uint64_t mword);
void obsd_ifmedia_delete_instance(struct ifmedia *ifm, uint64_t inst);
uint64_t obsd_ifmedia_baudrate(uint64_t mword);
int obsd_ifmedia_match(struct ifmedia *ifm, uint64_t target, uint64_t mask);
#define ifmedia_init(ifm, d, c, s) obsd_ifmedia_init((ifm), (d), (c), (s))
#define ifmedia_add(ifm, w, d, a) obsd_ifmedia_add((ifm), (w), (d), (a))
#define ifmedia_set(ifm, w) obsd_ifmedia_set((ifm), (w))
#define ifmedia_ioctl(ifp, ifr, ifm, cmd) ENOTTY
#define ifmedia_delete_instance(ifm, i) obsd_ifmedia_delete_instance((ifm), (i))
#define ifmedia_baudrate(w) obsd_ifmedia_baudrate(w)
#define ifmedia_match(ifm, t, m) obsd_ifmedia_match((ifm), (t), (m))

/* ---- bpf, kstat: not configured ---------------------------------------------------- */
#define BPF_DIRECTION_IN 1
#define BPF_DIRECTION_OUT 2
#define bpf_mtap_ether(b, m, d) ((void) 0)
#define bpf_mtap(b, m, d) ((void) 0)
#define bpf_mtap_hdr(b, h, l, m, d) ((void) 0)
#define DLT_EN10MB 1
#define DLT_IEEE802_11 105
#define DLT_IEEE802_11_RADIO 127
struct kstat;

/* ---- IP headers (offload paths reference them; the glue never sets
 * offload flags, so they only need to compile) ------------------------------------- */
struct in_addr
{
	uint32_t s_addr;
};
struct ip
{
	uint8_t ip_hl : 4, ip_v : 4;
	uint8_t ip_tos;
	uint16_t ip_len;
	uint16_t ip_id;
	uint16_t ip_off;
	uint8_t ip_ttl;
	uint8_t ip_p;
	uint16_t ip_sum;
	struct in_addr ip_src, ip_dst;
} __packed;
struct ip6_hdr
{
	uint32_t ip6_flow;
	uint16_t ip6_plen;
	uint8_t ip6_nxt;
	uint8_t ip6_hlim;
	uint8_t ip6_src[16], ip6_dst[16];
} __packed;
struct tcphdr
{
	uint16_t th_sport, th_dport;
	uint32_t th_seq, th_ack;
	uint8_t th_x2_off;
	uint8_t th_flags;
	uint16_t th_win, th_sum, th_urp;
} __packed;
struct udphdr
{
	uint16_t uh_sport, uh_dport, uh_ulen, uh_sum;
} __packed;
#define IPPROTO_IP 0
#define IPTOS_DSCP_CS0 0x00
#define IPTOS_DSCP_CS1 0x20
#define IPTOS_DSCP_AF11 0x28
#define IPTOS_DSCP_AF12 0x30
#define IPTOS_DSCP_AF13 0x38
#define IPTOS_DSCP_CS2 0x40
#define IPTOS_DSCP_AF21 0x48
#define IPTOS_DSCP_AF22 0x50
#define IPTOS_DSCP_AF23 0x58
#define IPTOS_DSCP_CS3 0x60
#define IPTOS_DSCP_AF31 0x68
#define IPTOS_DSCP_AF32 0x70
#define IPTOS_DSCP_AF33 0x78
#define IPTOS_DSCP_CS4 0x80
#define IPTOS_DSCP_AF41 0x88
#define IPTOS_DSCP_AF42 0x90
#define IPTOS_DSCP_AF43 0x98
#define IPTOS_DSCP_CS5 0xa0
#define IPTOS_DSCP_VA 0xb0
#define IPTOS_DSCP_EF 0xb8
#define IPTOS_DSCP_CS6 0xc0
#define IPTOS_DSCP_CS7 0xe0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IP_MAXPACKET 65535
#define tcpstat_add(a, b) ((void) 0)
#define tcpstat_inc(a) ((void) 0)

#endif
