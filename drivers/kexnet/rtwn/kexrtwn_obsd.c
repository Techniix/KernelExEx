/*
 *  KernelExEx - kexrtwn.sys: Realtek USB Wi-Fi for Windows 98 SE and ME,
 *  with FreeBSD's rtwn(4) driving the chip (RTL8188CU/CUS/RU, RTL8188EU,
 *  RTL8188FTV/FU, RTL8192CU, RTL8192EU, RTL8723BU, RTL8812AU, RTL8821AU:
 *  TP-Link Archer T2U/T2U Plus/T2U Nano, TL-WN722N v2/v3, TL-WN725N, ...)
 *  and OpenBSD's net80211 doing 802.11 (scanning, association, WPA2).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  This is the OpenBSD side (see kexrtwn.h): an OpenBSD-style driver whose
 *  hardware is "the FreeBSD side". The glue in obsd_net.c makes it an NDIS
 *  miniport; everything here runs on the driver thread except
 *  kexrtwn_o_input/tx_done, which only queue.
 *
 *  The receive path's CCMP handling (ccmp_decap) follows OpenBSD's
 *  urtwn(4) (ISC licence, Damien Bergamini, Stefan Sperling).
 */
#include "bpfilter.h"
#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/systm.h>
#include <sys/timeout.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net80211/ieee80211_var.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include "kexnet.h"
#include "kexrtwn.h"

struct kexrtwn_softc
{
	struct device sc_dev;
	struct ieee80211com sc_ic;
	int (*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);
	void *fh;                          /* the FreeBSD side */
	struct usbd_device *udev;
	struct kexrtwn_radio radio;
	struct timeout scan_to;
	struct timeout wd_to;
	/* frames from the FreeBSD side's threads */
	struct mutex rxq_mtx;
	struct mbuf_list rxq;
	struct task rx_task;
	struct task start_task;
	struct task restart_task;
	volatile int tx_pending;
	int hw_pairwise;                   /* the pairwise CCMP key is in the chip */
	int scanning;
	int running;
};

#define KEXRTWN_TXQ_MAX 48

static int kexrtwn_match(struct device *parent, void *match, void *aux);
static void kexrtwn_attach(struct device *parent, struct device *self, void *aux);
static int kexrtwn_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data);
static void kexrtwn_start(struct ifnet *ifp);
static void kexrtwn_watchdog(struct ifnet *ifp);
static int kexrtwn_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg);
static int kexrtwn_set_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k);
static void kexrtwn_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k);
static int kexrtwn_init(struct ifnet *ifp);
static void kexrtwn_stop(struct ifnet *ifp);

/* ---- match and attach ------------------------------------------------------------ */
static int kexrtwn_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa = (struct usb_attach_arg *) aux;
	(void) parent;
	(void) match;
	if (!uaa->iface)
		return UMATCH_NONE;
	return kexrtwn_f_match(uaa->vendor, uaa->product, uaa->device->config_index, uaa->iface->index)
	           ? UMATCH_VENDOR_PRODUCT
	           : UMATCH_NONE;
}

static void scan_to_fn(void *arg)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) arg;
	if (sc->sc_ic.ic_state == IEEE80211_S_SCAN)
		ieee80211_next_scan(&sc->sc_ic.ic_if);
}

static void rx_task_fn(void *arg);
static void start_task_fn(void *arg)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	if (ifq_is_oactive(&ifp->if_snd) && sc->tx_pending < KEXRTWN_TXQ_MAX / 2)
		ifq_clr_oactive(&ifp->if_snd);
	kexrtwn_start(ifp);
}

static void restart_task_fn(void *arg)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	printf("%s: resetting the adapter\n", sc->sc_dev.dv_xname);
	if (ifp->if_flags & IFF_RUNNING)
		kexrtwn_stop(ifp);
	if (ifp->if_flags & IFF_UP)
		kexrtwn_init(ifp);
}

static void kexrtwn_attach(struct device *parent, struct device *self, void *aux)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) self;
	struct usb_attach_arg *uaa = (struct usb_attach_arg *) aux;
	struct usbd_device *dev = uaa->device;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct kexrtwn_usbinfo ui;
	int i, j;
	(void) parent;

	sc->udev = dev;
	memset(&ui, 0, sizeof(ui));
	ui.kdev = dev->kdev;
	ui.ddesc = &dev->ddesc;
	ui.cdesc = dev->cdesc;
	ui.cdesc_len = dev->cdesc_len;
	ui.config_index = (uint8_t) dev->config_index;
	ui.iface_index = (uint8_t) uaa->iface->index;
	ui.speed = dev->speed == USB_SPEED_HIGH ? 2 : 1;
	ui.worker = dev->worker;
	for (i = 0; i < dev->nifaces; i++)
	{
		struct usbd_interface *ifc = &dev->ifaces[i];
		for (j = 0; j < ifc->nendpt && ui.nep < KEXRTWN_MAXEP; j++)
		{
			int k;
			uint8_t addr = ifc->endpoints[j]->bEndpointAddress;
			ui.ep[ui.nep].edesc = ifc->endpoints[j];
			ui.ep[ui.nep].iface_index = (uint8_t) i;
			for (k = 0; k < ifc->pipes.npipes; k++)
				if (ifc->pipes.pipes[k].ep == addr)
					ui.ep[ui.nep].pipe = ifc->pipes.pipes[k].handle;
			ui.nep++;
		}
	}
	printf("\n");
	sc->fh = kexrtwn_f_attach(&ui, sc->sc_dev.dv_xname, &sc->radio);
	if (!sc->fh)
	{
		printf("%s: the chip did not attach\n", sc->sc_dev.dv_xname);
		return;
	}
	kexrtwn_f_set_osc(sc->fh, sc);
	printf("%s: %s, %dT%dR, %d channels, address %s\n", sc->sc_dev.dv_xname, sc->radio.chipname,
	       sc->radio.ntxchains, sc->radio.nrxchains, sc->radio.nchan, ether_sprintf(sc->radio.macaddr));

	mtx_init(&sc->rxq_mtx, IPL_NET);
	ml_init(&sc->rxq);
	task_set(&sc->rx_task, rx_task_fn, sc);
	task_set(&sc->start_task, start_task_fn, sc);
	task_set(&sc->restart_task, restart_task_fn, sc);
	timeout_set(&sc->scan_to, scan_to_fn, sc);

	ic->ic_softc = sc;
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_SHPREAMBLE | IEEE80211_C_SHSLOT | IEEE80211_C_WEP | IEEE80211_C_RSN;
	IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->radio.macaddr);
	ic->ic_max_rssi = 100;   /* rxi_rssi: dBm + 100 */
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (i = 0; i < sc->radio.nchan; i++)
	{
		uint8_t n = sc->radio.chan[i].ieee;
		uint16_t f = sc->radio.chan[i].flags;
		if (n > IEEE80211_CHAN_MAX)
			continue;
		ic->ic_channels[n].ic_freq = sc->radio.chan[i].freq;
		if (f & KEXRTWN_CHAN_2GHZ)
			ic->ic_channels[n].ic_flags =
			    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
		else
		{
			ic->ic_channels[n].ic_flags = IEEE80211_CHAN_A;
			ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
		}
		if (f & KEXRTWN_CHAN_PASSIVE)
			ic->ic_channels[n].ic_flags |= IEEE80211_CHAN_PASSIVE;
		if ((f & KEXRTWN_CHAN_HT) && sc->radio.ht)
			ic->ic_channels[n].ic_flags |= IEEE80211_CHAN_HT;
	}
	if (sc->radio.ht)
	{
		ic->ic_flags |= IEEE80211_F_HTON;
		ic->ic_htcaps = IEEE80211_HTCAP_SGI20;
		for (i = 0; i < sc->radio.nrxchains && i < 4; i++)
			ic->ic_sup_mcs[i] = 0xff;
	}

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = kexrtwn_ioctl;
	ifp->if_start = kexrtwn_start;
	ifp->if_watchdog = kexrtwn_watchdog;
	memcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
	if_attach(ifp);
	ieee80211_ifattach(ifp);
	ic->ic_set_key = kexrtwn_set_key;
	ic->ic_delete_key = kexrtwn_delete_key;
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = kexrtwn_newstate;
	ieee80211_media_init(ifp, ieee80211_media_change, ieee80211_media_status);
}

/* ---- the interface ------------------------------------------------------------------ */
static int kexrtwn_init(struct ifnet *ifp)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int err;
	if (!sc->fh)
		return ENXIO;
	err = kexrtwn_f_init(sc->fh);   /* power, firmware (SYSTEM\KEXFW), MAC */
	if (err)
	{
		printf("%s: the adapter did not start (%d)\n", sc->sc_dev.dv_xname, err);
		return err;
	}
	sc->running = 1;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ifp->if_timer = 1;
	ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	return 0;
}

static void kexrtwn_stop(struct ifnet *ifp)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	ifp->if_timer = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	timeout_del(&sc->scan_to);
	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	sc->running = 0;
	if (sc->fh)
		kexrtwn_f_stop(sc->fh);
}

static int kexrtwn_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ifp->if_softc;
	int error = 0;
	switch (cmd)
	{
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
		{
			if (!(ifp->if_flags & IFF_RUNNING))
				error = kexrtwn_init(ifp);
			else if (sc->fh)
				kexrtwn_f_update_promisc(sc->fh, (ifp->if_flags & IFF_PROMISC) != 0);
		}
		else if (ifp->if_flags & IFF_RUNNING)
			kexrtwn_stop(ifp);
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
	}
	if (error == ENETRESET)
	{
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING))
		{
			kexrtwn_stop(ifp);
			kexrtwn_init(ifp);
		}
		error = 0;
	}
	return error;
}

static void kexrtwn_watchdog(struct ifnet *ifp)
{
	ifp->if_timer = (ifp->if_flags & IFF_RUNNING) ? 1 : 0;
	ieee80211_watchdog(ifp);
}

/* ---- state ----------------------------------------------------------------------------- */
static void fill_bss(struct kexrtwn_softc *sc, struct kexrtwn_bss *b)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	int i;
	memset(b, 0, sizeof(*b));
	IEEE80211_ADDR_COPY(b->macaddr, ni->ni_macaddr);
	IEEE80211_ADDR_COPY(b->bssid, ni->ni_bssid);
	b->freq = ni->ni_chan ? ni->ni_chan->ic_freq : 0;
	b->associd = ni->ni_associd & 0x3fff;
	b->intval = ni->ni_intval;
	b->capinfo = ni->ni_capinfo;
	b->nrates = (uint8_t) (ni->ni_rates.rs_nrates > 15 ? 15 : ni->ni_rates.rs_nrates);
	for (i = 0; i < b->nrates; i++)
		b->rates[i] = ni->ni_rates.rs_rates[i];
	b->ht = (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	if (b->ht)
	{
		int n = 0;
		while (n < 16 && isset(ni->ni_rxmcs, n))
			n++;
		b->nmcs = (uint8_t) n;
		b->htcap = ni->ni_htcaps;
		b->ampdu_param = ni->ni_ampdu_param;
	}
	b->shortpre = (ic->ic_flags & IEEE80211_F_SHPREAMBLE) != 0;
	b->shortslot = (ic->ic_flags & IEEE80211_F_SHSLOT) != 0;
}

static int kexrtwn_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ic->ic_softc;
	enum ieee80211_state ostate = ic->ic_state;
	struct kexrtwn_bss b;
	if (!sc->fh || !sc->running)
		return sc->sc_newstate(ic, nstate, arg);

	if (ostate == IEEE80211_S_SCAN && nstate != IEEE80211_S_SCAN)
	{
		timeout_del(&sc->scan_to);
		if (sc->scanning)
			kexrtwn_f_scan_end(sc->fh);
		sc->scanning = 0;
	}
	if (ostate == IEEE80211_S_RUN && nstate != IEEE80211_S_RUN)
		sc->hw_pairwise = 0;

	switch (nstate)
	{
	case IEEE80211_S_INIT:
		kexrtwn_f_newstate(sc->fh, KEXRTWN_S_INIT);
		break;
	case IEEE80211_S_SCAN:
		if (!sc->scanning)
		{
			kexrtwn_f_newstate(sc->fh, KEXRTWN_S_SCAN);
			kexrtwn_f_scan_start(sc->fh);
			sc->scanning = 1;
		}
		if (ic->ic_bss->ni_chan)
			kexrtwn_f_set_channel(sc->fh, ic->ic_bss->ni_chan->ic_freq);
		/* passive channels need the time to hear a beacon */
		timeout_add_msec(&sc->scan_to,
		                 (ic->ic_bss->ni_chan && (ic->ic_bss->ni_chan->ic_flags & IEEE80211_CHAN_PASSIVE)) ? 250
		                                                                                                   : 200);
		break;
	case IEEE80211_S_AUTH:
		fill_bss(sc, &b);
		kexrtwn_f_set_bss(sc->fh, &b);
		kexrtwn_f_set_channel(sc->fh, b.freq);
		kexrtwn_f_newstate(sc->fh, KEXRTWN_S_AUTH);
		break;
	case IEEE80211_S_ASSOC:
		kexrtwn_f_newstate(sc->fh, KEXRTWN_S_ASSOC);
		break;
	case IEEE80211_S_RUN:
		fill_bss(sc, &b);
		kexrtwn_f_set_bss(sc->fh, &b);
		kexrtwn_f_set_channel(sc->fh, b.freq);
		kexrtwn_f_newstate(sc->fh, KEXRTWN_S_RUN);
		break;
	}
	return sc->sc_newstate(ic, nstate, arg);
}

/* ---- keys: pairwise CCMP in the chip, everything else in software ---------------------- */
static int kexrtwn_set_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ic->ic_softc;
	if ((k->k_flags & IEEE80211_KEY_GROUP) || k->k_cipher != IEEE80211_CIPHER_CCMP || !sc->fh)
		return ieee80211_set_key(ic, ni, k);
	if (kexrtwn_f_set_key(sc->fh, ni->ni_macaddr, k->k_key, k->k_len, KEXRTWN_CIPHER_CCMP, 1, k->k_id) != 0)
	{
		sc->hw_pairwise = 0;
		return ieee80211_set_key(ic, ni, k);
	}
	sc->hw_pairwise = 1;
	return 0;
}

static void kexrtwn_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ic->ic_softc;
	if (!(k->k_flags & IEEE80211_KEY_GROUP) && k->k_cipher == IEEE80211_CIPHER_CCMP && sc->hw_pairwise && sc->fh)
	{
		kexrtwn_f_delete_key(sc->fh, 1, k->k_id);
		sc->hw_pairwise = 0;
		return;
	}
	ieee80211_delete_key(ic, ni, k);
}

/* ---- transmit ---------------------------------------------------------------------------- */
static int is_eapol(struct mbuf *m)
{
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	int hdrlen;
	uint8_t llc[8];
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA)
		return 0;
	hdrlen = ieee80211_get_hdrlen(wh);
	if (m->m_pkthdr.len < hdrlen + 8)
		return 0;
	m_copydata(m, hdrlen, 8, llc);
	return llc[0] == 0xaa && llc[1] == 0xaa && llc[6] == 0x88 && llc[7] == 0x8e;
}

/* the chip encrypts with the CAM key; we write the CCMP header (as urtwn does) */
static struct mbuf *ccmp_header(struct mbuf *m, struct ieee80211_key *k)
{
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	int hdrlen = ieee80211_get_hdrlen(wh);
	struct mbuf *n;
	uint8_t *ivp;
	/* a fresh mbuf: header, CCMP header, then the body */
	n = m_gethdr(M_DONTWAIT, MT_DATA);
	if (!n)
		goto drop;
	if (!MCLGETL(n, M_DONTWAIT, m->m_pkthdr.len + IEEE80211_CCMP_HDRLEN))
	{
		m_freem(n);
		goto drop;
	}
	m_copydata(m, 0, hdrlen, mtod(n, caddr_t));
	ivp = mtod(n, uint8_t *) + hdrlen;
	k->k_tsc++;
	ivp[0] = (uint8_t) k->k_tsc;
	ivp[1] = (uint8_t) (k->k_tsc >> 8);
	ivp[2] = 0;
	ivp[3] = (uint8_t) (k->k_id << 6) | IEEE80211_WEP_EXTIV;
	ivp[4] = (uint8_t) (k->k_tsc >> 16);
	ivp[5] = (uint8_t) (k->k_tsc >> 24);
	ivp[6] = (uint8_t) (k->k_tsc >> 32);
	ivp[7] = (uint8_t) (k->k_tsc >> 40);
	m_copydata(m, hdrlen, m->m_pkthdr.len - hdrlen, ivp + IEEE80211_CCMP_HDRLEN);
	n->m_len = n->m_pkthdr.len = m->m_pkthdr.len + IEEE80211_CCMP_HDRLEN;
	m_freem(m);
	return n;
drop:
	m_freem(m);
	return NULL;
}

static void kexrtwn_start(struct ifnet *ifp)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct mbuf *m;

	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd) || !sc->fh)
		return;
	for (;;)
	{
		struct ieee80211_frame *wh;
		int hwkey = 0, eapol;
		if (sc->tx_pending >= KEXRTWN_TXQ_MAX)
		{
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		m = mq_dequeue(&ic->ic_mgtq);
		if (m)
		{
			ni = (struct ieee80211_node *) m->m_pkthdr.ph_cookie;
			goto sendit;
		}
		if (ic->ic_state != IEEE80211_S_RUN)
			break;
		m = ifq_dequeue(&ifp->if_snd);
		if (!m)
			break;
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL)
			continue;
	sendit:
		wh = mtod(m, struct ieee80211_frame *);
		eapol = is_eapol(m);
		if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
		{
			struct ieee80211_key *k = ieee80211_get_txkey(ic, wh, ni);
			if (sc->hw_pairwise && k->k_cipher == IEEE80211_CIPHER_CCMP && !(k->k_flags & IEEE80211_KEY_GROUP))
			{
				m = ccmp_header(m, k);
				hwkey = 1;
			}
			else
				m = ieee80211_encrypt(ic, m, k);
			if (!m)
			{
				ieee80211_release_node(ic, ni);
				ifp->if_oerrors++;
				continue;
			}
		}
		ieee80211_release_node(ic, ni);   /* the FreeBSD side has its own BSS node */
		__atomic_fetch_add(&sc->tx_pending, 1, __ATOMIC_SEQ_CST);
		if (kexrtwn_f_transmit(sc->fh, m, 1, eapol, hwkey) != 0)
		{
			/* the FreeBSD side freed the frame */
			__atomic_fetch_sub(&sc->tx_pending, 1, __ATOMIC_SEQ_CST);
			ifp->if_oerrors++;
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		ifp->if_opackets++;
	}
}

void kexrtwn_o_tx_done(void *osc, struct mbuf *m, int ok)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) osc;
	if (!ok)
		sc->sc_ic.ic_if.if_oerrors++;
	m_freem(m);
	__atomic_fetch_sub(&sc->tx_pending, 1, __ATOMIC_SEQ_CST);
	task_add(systq, &sc->start_task);
}

void kexrtwn_o_restart(void *osc)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) osc;
	task_add(systq, &sc->restart_task);
}

int kexrtwn_o_foreach_mcast(void *osc, void (*fn)(void *arg, const uint8_t *mac), void *arg)
{
	(void) osc;
	(void) fn;
	(void) arg;
	return 0;   /* the FreeBSD side accepts all multicast (ic_allmulti) */
}

uint32_t kexrtwn_o_random(void)
{
	return arc4random();
}

/* ---- receive ------------------------------------------------------------------------------- */
void kexrtwn_o_input(void *osc, struct mbuf *m, const struct kexrtwn_rxinfo *ri)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) osc;
	int rssi = ri->rssi + 100;
	if (rssi < 0)
		rssi = 0;
	if (rssi > 100)
		rssi = 100;
	/* the receive details ride in the packet header until the driver thread takes it */
	m->m_pkthdr.ph_flowid = (uint16_t) rssi;
	m->m_pkthdr.ph_mss = (uint16_t) (ri->decrypted ? 1 : 0);
	mtx_enter(&sc->rxq_mtx);
	if (ml_len(&sc->rxq) > 256)
	{
		mtx_leave(&sc->rxq_mtx);
		m_freem(m);
		return;
	}
	ml_enqueue(&sc->rxq, m);
	mtx_leave(&sc->rxq_mtx);
	task_add(systq, &sc->rx_task);
}

/* the chip decrypted a CCMP frame: check its PN, strip the MIC (urtwn_ccmp_decap) */
static int ccmp_decap(struct ieee80211com *ic, struct mbuf *m, struct ieee80211_node *ni)
{
	struct ieee80211_key *k;
	struct ieee80211_frame *wh;
	uint64_t pn, *prsc;
	uint8_t *ivp, tid;
	int hdrlen, hasqos;
	k = ieee80211_get_rxkey(ic, m, ni);
	if (!k)
		return 1;
	wh = mtod(m, struct ieee80211_frame *);
	hdrlen = ieee80211_get_hdrlen(wh);
	ivp = (uint8_t *) wh + hdrlen;
	if (!(ivp[3] & IEEE80211_WEP_EXTIV))
		return 1;
	hasqos = ieee80211_has_qos(wh);
	tid = hasqos ? ieee80211_get_qos(wh) & IEEE80211_QOS_TID : 0;
	prsc = &k->k_rsc[tid];
	pn = (uint64_t) ivp[0] | (uint64_t) ivp[1] << 8 | (uint64_t) ivp[4] << 16 | (uint64_t) ivp[5] << 24 |
	     (uint64_t) ivp[6] << 32 | (uint64_t) ivp[7] << 40;
	if (pn <= *prsc)
	{
		ic->ic_stats.is_ccmp_replays++;
		return 1;
	}
	m_adj(m, -IEEE80211_CCMP_MICLEN);
	return 0;
}

static void rx_task_fn(void *arg)
{
	struct kexrtwn_softc *sc = (struct kexrtwn_softc *) arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct mbuf_list ml;
	struct mbuf *m;

	mtx_enter(&sc->rxq_mtx);
	ml = sc->rxq;
	ml_init(&sc->rxq);
	mtx_leave(&sc->rxq_mtx);

	while ((m = ml_dequeue(&ml)) != NULL)
	{
		struct ieee80211_rxinfo rxi;
		struct ieee80211_frame *wh;
		struct ieee80211_node *ni;
		int decrypted = m->m_pkthdr.ph_mss;
		memset(&rxi, 0, sizeof(rxi));
		rxi.rxi_rssi = m->m_pkthdr.ph_flowid;
		m->m_pkthdr.ph_flowid = 0;
		m->m_pkthdr.ph_mss = 0;
		if (!(ifp->if_flags & IFF_RUNNING) || m->m_pkthdr.len < (int) sizeof(struct ieee80211_frame_min))
		{
			m_freem(m);
			continue;
		}
		wh = mtod(m, struct ieee80211_frame *);
		ni = ieee80211_find_rxnode(ic, wh);
		if (decrypted && (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_CTL &&
		    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) && (ni->ni_flags & IEEE80211_NODE_RXPROT) &&
		    !IEEE80211_IS_MULTICAST(wh->i_addr1) && ni->ni_pairwise_key.k_cipher == IEEE80211_CIPHER_CCMP)
		{
			if (ccmp_decap(ic, m, ni) != 0)
			{
				ifp->if_ierrors++;
				m_freem(m);
				ieee80211_release_node(ic, ni);
				continue;
			}
			rxi.rxi_flags |= IEEE80211_RXI_HWDEC;
		}
		ieee80211_input(ifp, m, ni, &rxi);
		ieee80211_release_node(ic, ni);
	}
}

/* ---- mbufs for the FreeBSD side ------------------------------------------------------------- */
struct mbuf *kexrtwn_o_m_getjcl(int size)
{
	struct mbuf *m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (!m)
		return NULL;
	if (size > MHLEN && !MCLGETL(m, M_DONTWAIT, size))
	{
		m_freem(m);
		return NULL;
	}
	m->m_len = m->m_pkthdr.len = 0;
	return m;
}

void kexrtwn_o_m_freem(struct mbuf *m)
{
	m_freem(m);
}

void kexrtwn_o_m_adj(struct mbuf *m, int len)
{
	m_adj(m, len);
}

void kexrtwn_o_m_copydata(const struct mbuf *m, int off, int len, void *cp)
{
	m_copydata((struct mbuf *) m, off, len, cp);
}

void kexrtwn_o_m_copyback(struct mbuf *m, int off, int len, const void *cp)
{
	const uint8_t *src = (const uint8_t *) cp;
	struct mbuf *n = m;
	int total = 0;
	while (n && off >= (int) n->m_len)
	{
		off -= n->m_len;
		n = n->m_next;
	}
	while (n && len > 0)
	{
		int room = (int) M_TRAILINGSPACE(n) + (int) n->m_len - off;
		int k = len < room ? len : room;
		if (k <= 0)
			break;
		memcpy(mtod(n, uint8_t *) + off, src, k);
		if (off + k > (int) n->m_len)
			n->m_len = off + k;
		src += k;
		len -= k;
		total += k;
		off = 0;
		n = n->m_next;
	}
	if (m->m_flags & M_PKTHDR)
	{
		int l = 0;
		for (n = m; n; n = n->m_next)
			l += n->m_len;
		m->m_pkthdr.len = l;
	}
	(void) total;
}

struct mbuf *kexrtwn_o_m_defrag(struct mbuf *m)
{
	if (m_defrag(m, M_DONTWAIT) != 0)
	{
		m_freem(m);
		return NULL;
	}
	return m;
}

struct mbuf *kexrtwn_o_m_prepend(struct mbuf *m, int len)
{
	return m_prepend(m, len, M_DONTWAIT);
}

struct mbuf *kexrtwn_o_m_pullup(struct mbuf *m, int len)
{
	return m_pullup(m, len);
}

int kexrtwn_o_m_append(struct mbuf *m, int len, const void *cp)
{
	struct mbuf *t = m;
	while (t->m_next)
		t = t->m_next;
	if ((int) M_TRAILINGSPACE(t) < len)
		return 0;
	memcpy(mtod(t, uint8_t *) + t->m_len, cp, len);
	t->m_len += len;
	if (m->m_flags & M_PKTHDR)
		m->m_pkthdr.len += len;
	return 1;
}

struct mbuf *kexrtwn_o_m_dup(const struct mbuf *m)
{
	return m_dup_pkt((struct mbuf *) m, 0, M_DONTWAIT);
}

void kexrtwn_o_m_cat(struct mbuf *m, struct mbuf *n)
{
	m_cat(m, n);
}

/* ---- the driver -------------------------------------------------------------------------------- */
static const struct kexnet_driver kexrtwn_driver = {
	"rtwn",
	"Realtek USB Wi-Fi (KernelExEx, FreeBSD rtwn)",
	sizeof(struct kexrtwn_softc),
	kexrtwn_match,
	kexrtwn_attach,
	NULL,
	NULL,
	NULL,
	1,
	kex80211_configure,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &kexrtwn_driver);
}
