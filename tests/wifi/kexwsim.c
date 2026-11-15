/*
 *  KernelExEx - kexwsim.sys: a simulated Wi-Fi adapter with simulated
 *  access points, for testing the Wi-Fi program and the WPA2 code in an
 *  emulator, which has no Wi-Fi hardware. Test builds only; never installed
 *  by setup.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  It is a net80211 driver like urtwn, bound to QEMU's USB serial adapter
 *  (0403:6001, "-device usb-serial"), whose USB side it never touches. Its
 *  "radio" hands net80211 the frames the access points below would send:
 *  beacons and probe responses while scanning; authentication and
 *  association responses; for the WPA2 network the authenticator's half of
 *  the 4-way handshake (PTK from the real PMK, MICs checked, GTK sent AES
 *  key-wrapped), after which every data frame is CCMP-encrypted both ways
 *  with net80211's own code. Behind each live access point sits a tiny
 *  router: DHCP (10.98.N.50 for the client), ARP and ping for 10.98.N.1.
 *
 *  So a test can scan, join an open and a WPA2 network, get an address and
 *  ping, through exactly the code a real Realtek adapter uses above the chip.
 */
#include "obsd/kexobsd_net.h"
#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/systm.h>
#include <sys/timeout.h>
#include <sys/device.h>
#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include <crypto/aes.h>
#include <crypto/key_wrap.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include "kexnet.h"

void kex80211_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, uint8_t *out, size_t olen);

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif

#define SEC_OPEN 0
#define SEC_WEP 1
#define SEC_WPA2 2
#define SEC_WPA3 3

struct sim_ap
{
	const char *ssid;
	int chan, dbm, sec;
	const char *pass;   /* WPA2 */
	int live;           /* answers authentication; has a router behind it */
	uint8_t bssid[6];
	uint8_t pmk[32];
};

static struct sim_ap aps[] = {
	{ "KexTest-WPA2", 6, -45, SEC_WPA2, "kexexpass1", 1, { 0x02, 0x4b, 0x45, 0x58, 0xa0, 0x02 } },
	{ "KexTest-Open", 1, -62, SEC_OPEN, NULL, 1, { 0x02, 0x4b, 0x45, 0x58, 0xa0, 0x01 } },
	{ "Neighbour-WPA3", 11, -74, SEC_WPA3, NULL, 0, { 0x02, 0x4b, 0x45, 0x58, 0xa0, 0x03 } },
	{ "OldRouter-WEP", 3, -81, SEC_WEP, NULL, 0, { 0x02, 0x4b, 0x45, 0x58, 0xa0, 0x04 } },
	{ "Caf\xc3\xa9 Guest", 11, -68, SEC_OPEN, NULL, 0, { 0x02, 0x4b, 0x45, 0x58, 0xa0, 0x05 } },
};
#define NAPS ((int) nitems(aps))

/* the RSN element every WPA2 access point here sends: CCMP, PSK */
static const uint8_t rsn_ie[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
	                              0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
static const uint8_t rsn_ie_sae[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
	                                  0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0xc0, 0x00 };
static const uint8_t rates[] = { 0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
static const uint8_t xrates[] = { 0x32, 0x04, 0x30, 0x48, 0x60, 0x6c };

struct wsim_softc
{
	struct device sc_dev;
	struct ieee80211com sc_ic;
	int (*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);
	struct timeout scan_to, beacon_to, rx_to;
	struct mbuf_queue rxq;
	uint16_t seq;
	/* the association in progress or done */
	int cur;            /* index into aps, -1 */
	int hs;             /* handshake: 0 none, 1 msg1 sent, 2 msg3 sent, 3 done */
	uint64_t replay;
	uint8_t anonce[32];
	struct ieee80211_ptk ptk;
	uint8_t gtk[16];
	struct ieee80211_key aptk;   /* the access point's copy of the TK */
	int aptk_set;
	uint32_t rx_frames, tx_frames, dhcp_acks, pings;
};

static int ap_index(const uint8_t *bssid)
{
	int i;
	for (i = 0; i < NAPS; i++)
		if (!memcmp(aps[i].bssid, bssid, 6))
			return i;
	return -1;
}

/* ---- frames to the station ------------------------------------------------------- */
static struct mbuf *mk(const void *data, int len)
{
	struct mbuf *m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (!m)
		return NULL;
	if (len > MHLEN)
	{
		MCLGET(m, M_DONTWAIT);
		if (!(m->m_flags & M_EXT))
		{
			m_freem(m);
			return NULL;
		}
	}
	memcpy(mtod(m, void *), data, (size_t) len);
	m->m_len = m->m_pkthdr.len = len;
	return m;
}

static void hdr(struct wsim_softc *sc, uint8_t *f, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2,
                const uint8_t *a3)
{
	f[0] = fc0;
	f[1] = fc1;
	f[2] = f[3] = 0;
	memcpy(f + 4, a1, 6);
	memcpy(f + 10, a2, 6);
	memcpy(f + 16, a3, 6);
	f[22] = (uint8_t) (sc->seq << 4);
	f[23] = (uint8_t) (sc->seq >> 4);
	sc->seq++;
}

/* queued, and handed to net80211 from rx_to, never from inside its calls */
static void queue_rx(struct wsim_softc *sc, struct mbuf *m)
{
	if (!m)
		return;
	if (mq_enqueue(&sc->rxq, m) == 0)
		timeout_add_msec(&sc->rx_to, 1);
}

static void rx_run(void *arg)
{
	struct wsim_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct mbuf *m;
	while ((m = mq_dequeue(&sc->rxq)) != NULL)
	{
		struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
		struct ieee80211_rxinfo rxi;
		struct ieee80211_node *ni;
		int i = ap_index(wh->i_addr2);
		if (!(ifp->if_flags & IFF_RUNNING))
		{
			m_freem(m);
			continue;
		}
		memset(&rxi, 0, sizeof(rxi));
		rxi.rxi_rssi = 100 + (i >= 0 ? aps[i].dbm : -90);
		rxi.rxi_chan = (uint8_t) (i >= 0 ? aps[i].chan : 0);
		ni = ieee80211_find_rxnode(ic, wh);
		sc->rx_frames++;
		ieee80211_input(ifp, m, ni, &rxi);
		ieee80211_release_node(ic, ni);
	}
}

static int ies(const struct sim_ap *ap, uint8_t *p)
{
	int n = 0, l = (int) strlen(ap->ssid);
	p[n++] = IEEE80211_ELEMID_SSID;
	p[n++] = (uint8_t) l;
	memcpy(p + n, ap->ssid, (size_t) l);
	n += l;
	memcpy(p + n, rates, sizeof(rates));
	n += sizeof(rates);
	p[n++] = IEEE80211_ELEMID_DSPARMS;
	p[n++] = 1;
	p[n++] = (uint8_t) ap->chan;
	memcpy(p + n, xrates, sizeof(xrates));
	n += sizeof(xrates);
	if (ap->sec == SEC_WPA2)
	{
		memcpy(p + n, rsn_ie, sizeof(rsn_ie));
		n += sizeof(rsn_ie);
	}
	else if (ap->sec == SEC_WPA3)
	{
		memcpy(p + n, rsn_ie_sae, sizeof(rsn_ie_sae));
		n += sizeof(rsn_ie_sae);
	}
	return n;
}

static void send_beacon(struct wsim_softc *sc, const struct sim_ap *ap, int probe_resp)
{
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint8_t f[256];
	int n = 24;
	uint16_t cap = IEEE80211_CAPINFO_ESS | IEEE80211_CAPINFO_SHORT_SLOTTIME;
	if (ap->sec != SEC_OPEN)
		cap |= IEEE80211_CAPINFO_PRIVACY;
	hdr(sc, f, IEEE80211_FC0_TYPE_MGT | (probe_resp ? IEEE80211_FC0_SUBTYPE_PROBE_RESP : IEEE80211_FC0_SUBTYPE_BEACON),
	    0, probe_resp ? sc->sc_ic.ic_myaddr : bcast, ap->bssid, ap->bssid);
	memset(f + n, 0, 8);   /* timestamp */
	n += 8;
	f[n++] = 100;          /* beacon interval */
	f[n++] = 0;
	f[n++] = (uint8_t) cap;
	f[n++] = (uint8_t) (cap >> 8);
	n += ies(ap, f + n);
	queue_rx(sc, mk(f, n));
}

static void beacons_on(struct wsim_softc *sc, int chan, int probe_resp)
{
	int i;
	for (i = 0; i < NAPS; i++)
		if (aps[i].chan == chan)
			send_beacon(sc, &aps[i], probe_resp);
}

/* an Ethernet frame from the access point's router to the station: an
 * 802.11 data frame from the DS, CCMP-encrypted once keys are in place */
static void send_data(struct wsim_softc *sc, const struct sim_ap *ap, uint16_t type, const void *body, int len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t f[1600];
	struct mbuf *m;
	if (len > (int) sizeof(f) - 32)
		return;
	hdr(sc, f, IEEE80211_FC0_TYPE_DATA, IEEE80211_FC1_DIR_FROMDS, ic->ic_myaddr, ap->bssid, ap->bssid);
	f[24] = 0xaa;
	f[25] = 0xaa;
	f[26] = 0x03;
	f[27] = f[28] = f[29] = 0;
	f[30] = (uint8_t) (type >> 8);
	f[31] = (uint8_t) type;
	memcpy(f + 32, body, (size_t) len);
	m = mk(f, 32 + len);
	if (m && sc->aptk_set && type != ETHERTYPE_EAPOL)
	{
		mtod(m, uint8_t *)[1] |= IEEE80211_FC1_PROTECTED;
		m = ieee80211_ccmp_encrypt(ic, m, &sc->aptk);
	}
	queue_rx(sc, m);
}

/* ---- the authenticator's half of the 4-way handshake ------------------------------ */
static void send_eapol(struct wsim_softc *sc, uint16_t info, const uint8_t *data, int dlen)
{
	uint8_t buf[256];
	struct ieee80211_eapol_key *k = (struct ieee80211_eapol_key *) buf;
	uint8_t *kd = (uint8_t *) (k + 1);
	memset(buf, 0, sizeof(buf));
	k->version = 2;
	k->type = EAPOL_KEY;
	k->desc = EAPOL_KEY_DESC_IEEE80211;
	BE_WRITE_2(k->info, info);
	BE_WRITE_2(k->keylen, 16);
	sc->replay++;
	BE_WRITE_8(k->replaycnt, sc->replay);
	memcpy(k->nonce, sc->anonce, 32);
	if (dlen)
	{
		aes_key_wrap_ctx ctx;
		uint8_t plain[128];
		int n = dlen;
		memcpy(plain, data, (size_t) dlen);
		if (n < 16 || (n & 7))
		{
			int pad = n < 16 ? 16 - n : 8 - (n & 7);
			plain[n++] = IEEE80211_ELEMID_VENDOR;
			memset(plain + n, 0, (size_t) (pad - 1));
			n += pad - 1;
		}
		aes_key_wrap_set_key_wrap_only(&ctx, sc->ptk.kek, 16);
		aes_key_wrap(&ctx, plain, (size_t) (n / 8), kd);
		explicit_bzero(&ctx, sizeof(ctx));
		explicit_bzero(plain, sizeof(plain));
		dlen = n + 8;
	}
	BE_WRITE_2(k->paylen, dlen);
	BE_WRITE_2(k->len, sizeof(*k) + dlen - 4);
	if (info & EAPOL_KEY_KEYMIC)
		ieee80211_eapol_key_mic(k, sc->ptk.kck);
	send_data(sc, &aps[sc->cur], ETHERTYPE_EAPOL, buf, (int) sizeof(*k) + dlen);
}

static void eapol_in(struct wsim_softc *sc, uint8_t *p, int len)
{
	struct ieee80211_eapol_key *k = (struct ieee80211_eapol_key *) p;
	struct sim_ap *ap = &aps[sc->cur];
	uint16_t info;
	if (len < (int) sizeof(*k) || k->type != EAPOL_KEY || ap->sec != SEC_WPA2)
		return;
	info = BE_READ_2(k->info);
	if (BE_READ_8(k->replaycnt) != sc->replay)
	{
		printf("kexwsim: EAPOL replay counter %u, expected %u\n", (unsigned) BE_READ_8(k->replaycnt),
		       (unsigned) sc->replay);
		return;
	}
	if (sc->hs == 1)
	{
		/* message 2: SNonce; the PTK; the MIC proves the station has the PMK */
		uint8_t kd[128];
		int n = 0;
		ieee80211_derive_ptk(IEEE80211_AKM_PSK, ap->pmk, ap->bssid, sc->sc_ic.ic_myaddr, sc->anonce, k->nonce,
		                     &sc->ptk);
		if (ieee80211_eapol_key_check_mic(k, sc->ptk.kck))
		{
			printf("kexwsim: \"%s\": message 2 MIC wrong (wrong password?)\n", ap->ssid);
			return;
		}
		printf("kexwsim: \"%s\": message 2 MIC good, sending message 3\n", ap->ssid);
		memcpy(kd, rsn_ie, sizeof(rsn_ie));
		n = sizeof(rsn_ie);
		kd[n++] = IEEE80211_ELEMID_VENDOR;   /* GTK KDE */
		kd[n++] = 22;
		kd[n++] = 0x00;
		kd[n++] = 0x0f;
		kd[n++] = 0xac;
		kd[n++] = IEEE80211_KDE_GTK;
		kd[n++] = 1;   /* key id 1 */
		kd[n++] = 0;
		memcpy(kd + n, sc->gtk, 16);
		n += 16;
		/* the AP's copy of the TK, so the station's first encrypted
		 * frame (message 4 may be one) can be read */
		memset(&sc->aptk, 0, sizeof(sc->aptk));
		sc->aptk.k_cipher = IEEE80211_CIPHER_CCMP;
		sc->aptk.k_flags = IEEE80211_KEY_TX;
		sc->aptk.k_len = 16;
		memcpy(sc->aptk.k_key, sc->ptk.tk, 16);
		sc->hs = 2;
		send_eapol(sc, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_INSTALL | EAPOL_KEY_KEYACK |
		                   EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE | EAPOL_KEY_ENCRYPTED,
		           kd, n);
		explicit_bzero(kd, sizeof(kd));
		if (ieee80211_ccmp_set_key(&sc->sc_ic, &sc->aptk) == 0)
			sc->aptk_set = 1;
	}
	else if (sc->hs == 2 && (info & EAPOL_KEY_KEYMIC))
	{
		if (ieee80211_eapol_key_check_mic(k, sc->ptk.kck))
		{
			printf("kexwsim: message 4 MIC wrong\n");
			return;
		}
		sc->hs = 3;
		printf("kexwsim: \"%s\": 4-way handshake complete, CCMP on\n", ap->ssid);
	}
}

/* ---- the router behind a live access point ---------------------------------------- */
static uint16_t csum(const uint8_t *p, int n)
{
	uint32_t s = 0;
	int i;
	for (i = 0; i + 1 < n; i += 2)
		s += (uint32_t) (p[i] << 8 | p[i + 1]);
	if (n & 1)
		s += (uint32_t) (p[n - 1] << 8);
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (uint16_t) ~s;
}

static void ip_out(struct wsim_softc *sc, const struct sim_ap *ap, uint8_t *ip, int len)
{
	uint16_t c;
	ip[10] = ip[11] = 0;
	c = csum(ip, 20);
	ip[10] = (uint8_t) (c >> 8);
	ip[11] = (uint8_t) c;
	send_data(sc, ap, ETHERTYPE_IP, ip, len);
}

static void router_in(struct wsim_softc *sc, uint16_t type, uint8_t *p, int len)
{
	const struct sim_ap *ap = &aps[sc->cur];
	uint8_t net = (uint8_t) (sc->cur + 1);   /* 10.98.<n>.x */
	uint8_t gw[4] = { 10, 98, net, 1 }, cli[4] = { 10, 98, net, 50 };
	uint8_t out[600];
	if (type == ETHERTYPE_ARP && len >= 28 && p[7] == 1 && !memcmp(p + 24, gw, 4))
	{
		memcpy(out, p, 28);
		out[7] = 2;
		memcpy(out + 8, ap->bssid, 6);
		memcpy(out + 14, gw, 4);
		memcpy(out + 18, p + 8, 10);
		send_data(sc, ap, ETHERTYPE_ARP, out, 28);
		return;
	}
	if (type != ETHERTYPE_IP || len < 28 || (p[0] >> 4) != 4)
		return;
	if (p[9] == IPPROTO_ICMP && !memcmp(p + 16, gw, 4) && p[20] == 8)
	{
		int n = len > (int) sizeof(out) ? (int) sizeof(out) : len;
		uint16_t c;
		memcpy(out, p, (size_t) n);
		memcpy(out + 12, gw, 4);
		memcpy(out + 16, p + 12, 4);
		out[8] = 64;
		out[20] = 0;
		out[22] = out[23] = 0;
		c = csum(out + 20, n - 20);
		out[22] = (uint8_t) (c >> 8);
		out[23] = (uint8_t) c;
		sc->pings++;
		ip_out(sc, ap, out, n);
		return;
	}
	if (p[9] == IPPROTO_UDP && len >= 28 + 240 && p[22] == 0 && p[23] == 67)
	{
		const uint8_t *b = p + 28, *o = b + 240, *end = p + len;
		int mtype = 0, n;
		uint8_t *r = out + 28;
		while (o < end && *o != 255)
		{
			if (*o == 0)
			{
				o++;
				continue;
			}
			if (o + 1 >= end)
				break;
			if (*o == 53 && o[1] >= 1)
				mtype = o[2];
			o += 2 + o[1];
		}
		if (mtype != 1 && mtype != 3)
			return;
		memset(out, 0, sizeof(out));
		r[0] = 2;   /* BOOTREPLY */
		r[1] = 1;
		r[2] = 6;
		memcpy(r + 4, b + 4, 4);   /* xid */
		memcpy(r + 10, b + 10, 2); /* flags */
		memcpy(r + 16, cli, 4);    /* yiaddr */
		memcpy(r + 20, gw, 4);     /* siaddr */
		memcpy(r + 28, b + 28, 16);/* chaddr */
		r[236] = 99;
		r[237] = 130;
		r[238] = 83;
		r[239] = 99;
		n = 240;
		r[n++] = 53;
		r[n++] = 1;
		r[n++] = (uint8_t) (mtype == 1 ? 2 : 5);   /* OFFER / ACK */
		r[n++] = 54;
		r[n++] = 4;
		memcpy(r + n, gw, 4);
		n += 4;
		r[n++] = 51;
		r[n++] = 4;
		r[n++] = 0;
		r[n++] = 1;
		r[n++] = 0x51;
		r[n++] = 0x80;   /* one day */
		r[n++] = 1;
		r[n++] = 4;
		r[n++] = 255;
		r[n++] = 255;
		r[n++] = 255;
		r[n++] = 0;
		r[n++] = 3;
		r[n++] = 4;
		memcpy(r + n, gw, 4);
		n += 4;
		r[n++] = 6;
		r[n++] = 4;
		memcpy(r + n, gw, 4);
		n += 4;
		r[n++] = 255;
		while (n < 300)
			r[n++] = 0;
		/* UDP 67 -> 68, no checksum */
		out[20] = 0;
		out[21] = 67;
		out[22] = 0;
		out[23] = 68;
		out[24] = (uint8_t) ((8 + n) >> 8);
		out[25] = (uint8_t) (8 + n);
		/* IPv4 */
		out[0] = 0x45;
		out[2] = (uint8_t) ((28 + n) >> 8);
		out[3] = (uint8_t) (28 + n);
		out[8] = 64;
		out[9] = IPPROTO_UDP;
		memcpy(out + 12, gw, 4);
		memset(out + 16, 0xff, 4);
		if (mtype == 3)
			sc->dhcp_acks++;
		printf("kexwsim: \"%s\": DHCP %s 10.98.%u.50\n", ap->ssid, mtype == 1 ? "offer" : "ack", net);
		ip_out(sc, ap, out, 28 + n);
	}
}

/* ---- frames from the station ------------------------------------------------------- */
static void assoc_resp(struct wsim_softc *sc, int reassoc)
{
	struct sim_ap *ap = &aps[sc->cur];
	uint8_t f[128];
	int n = 24;
	uint16_t cap = IEEE80211_CAPINFO_ESS | IEEE80211_CAPINFO_SHORT_SLOTTIME;
	if (ap->sec != SEC_OPEN)
		cap |= IEEE80211_CAPINFO_PRIVACY;
	hdr(sc, f, IEEE80211_FC0_TYPE_MGT | (reassoc ? IEEE80211_FC0_SUBTYPE_REASSOC_RESP : IEEE80211_FC0_SUBTYPE_ASSOC_RESP),
	    0, sc->sc_ic.ic_myaddr, ap->bssid, ap->bssid);
	f[n++] = (uint8_t) cap;
	f[n++] = (uint8_t) (cap >> 8);
	f[n++] = 0;   /* status: success */
	f[n++] = 0;
	f[n++] = 1;   /* AID 1 */
	f[n++] = 0xc0;
	memcpy(f + n, rates, sizeof(rates));
	n += sizeof(rates);
	memcpy(f + n, xrates, sizeof(xrates));
	n += sizeof(xrates);
	queue_rx(sc, mk(f, n));
	sc->hs = 0;
	sc->replay = 0;
	sc->aptk_set = 0;
	if (ap->sec == SEC_WPA2)
	{
		arc4random_buf(sc->anonce, sizeof(sc->anonce));
		arc4random_buf(sc->gtk, sizeof(sc->gtk));
		sc->hs = 1;
		send_eapol(sc, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYACK, NULL, 0);
	}
}

static void mgmt_out(struct wsim_softc *sc, const uint8_t *f, int len)
{
	int sub = f[0] & IEEE80211_FC0_SUBTYPE_MASK, i;
	struct ieee80211com *ic = &sc->sc_ic;
	if (len < 24)
		return;
	switch (sub)
	{
	case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
		if (ic->ic_bss && ic->ic_bss->ni_chan)
			beacons_on(sc, (int) ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan), 1);
		break;
	case IEEE80211_FC0_SUBTYPE_AUTH:
		i = ap_index(f + 4);
		if (i >= 0 && aps[i].live && len >= 30)
		{
			uint8_t r[30];
			hdr(sc, r, IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_AUTH, 0, ic->ic_myaddr, aps[i].bssid,
			    aps[i].bssid);
			r[24] = 0;   /* open system */
			r[25] = 0;
			r[26] = 2;   /* sequence 2 */
			r[27] = 0;
			r[28] = 0;   /* success */
			r[29] = 0;
			queue_rx(sc, mk(r, 30));
		}
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
	case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
		i = ap_index(f + 4);
		if (i >= 0 && aps[i].live)
		{
			sc->cur = i;
			printf("kexwsim: association with \"%s\"\n", aps[i].ssid);
			assoc_resp(sc, sub == IEEE80211_FC0_SUBTYPE_REASSOC_REQ);
		}
		break;
	case IEEE80211_FC0_SUBTYPE_DEAUTH:
	case IEEE80211_FC0_SUBTYPE_DISASSOC:
		if (sc->cur >= 0)
			printf("kexwsim: \"%s\": station left\n", aps[sc->cur].ssid);
		sc->cur = -1;
		sc->hs = 0;
		if (sc->aptk_set)
			ieee80211_ccmp_delete_key(ic, &sc->aptk);
		sc->aptk_set = 0;
		break;
	}
}

/* a data frame as net80211 sent it (encrypted when keys are in place):
 * decrypted with the access point's key, then handed to the router */
static void data_out(struct wsim_softc *sc, struct mbuf *m)
{
	static uint8_t f[1700];
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	int len, hl = 24;
	if (sc->cur < 0)
	{
		m_freem(m);
		return;
	}
	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
	{
		if (!sc->aptk_set)
		{
			m_freem(m);
			return;
		}
		m = ieee80211_ccmp_decrypt(&sc->sc_ic, m, &sc->aptk);
		if (!m)
		{
			printf("kexwsim: a frame from the station did not decrypt\n");
			return;
		}
	}
	len = m->m_pkthdr.len;
	if (len > (int) sizeof(f))
	{
		m_freem(m);
		return;
	}
	m_copydata(m, 0, len, f);
	m_freem(m);
	if ((f[0] & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_QOS)
		hl += 2;
	if (len < hl + 8 || f[hl] != 0xaa || f[hl + 1] != 0xaa)
		return;
	{
		uint16_t type = (uint16_t) (f[hl + 6] << 8 | f[hl + 7]);
		uint8_t *p = f + hl + 8;
		int n = len - hl - 8;
		if (type == ETHERTYPE_EAPOL)
			eapol_in(sc, p, n);
		else if (aps[sc->cur].sec == SEC_OPEN || sc->hs == 3)
			router_in(sc, type, p, n);
	}
}

static void wsim_start(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct mbuf *m;
	if (!(ifp->if_flags & IFF_RUNNING))
		return;
	for (;;)
	{
		m = mq_dequeue(&ic->ic_mgtq);
		if (m)
		{
			static uint8_t f[512];
			int len = m->m_pkthdr.len > (int) sizeof(f) ? (int) sizeof(f) : m->m_pkthdr.len;
			ni = m->m_pkthdr.ph_cookie;
			m_copydata(m, 0, len, f);
			m_freem(m);
			sc->tx_frames++;
			mgmt_out(sc, f, len);
			if (ni)
				ieee80211_release_node(ic, ni);
			continue;
		}
		if (ic->ic_state != IEEE80211_S_RUN)
			break;
		m = ifq_dequeue(&ifp->if_snd);
		if (!m)
			break;
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL)
			continue;
		{
			struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
			if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
			{
				/* software CCMP, as a radio without hardware crypto */
				struct ieee80211_key *k = ieee80211_get_txkey(ic, wh, ni);
				if ((m = ieee80211_encrypt(ic, m, k)) == NULL)
				{
					ieee80211_release_node(ic, ni);
					continue;
				}
			}
		}
		sc->tx_frames++;
		data_out(sc, m);
		ieee80211_release_node(ic, ni);
	}
}

/* ---- scanning and state ----------------------------------------------------------- */
static void scan_run(void *arg)
{
	struct wsim_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	if (ic->ic_state == IEEE80211_S_SCAN)
		ieee80211_next_scan(&ic->ic_if);
}

static void beacon_run(void *arg)
{
	struct wsim_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	if (ic->ic_state == IEEE80211_S_RUN && sc->cur >= 0)
	{
		send_beacon(sc, &aps[sc->cur], 0);
		timeout_add_msec(&sc->beacon_to, 500);
	}
}

static int wsim_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct wsim_softc *sc = ic->ic_softc;
	timeout_del(&sc->scan_to);
	timeout_del(&sc->beacon_to);
	if (nstate == IEEE80211_S_SCAN)
	{
		/* the access points on this channel answer at once */
		beacons_on(sc, (int) ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan), 0);
		timeout_add_msec(&sc->scan_to, 120);
	}
	else if (nstate == IEEE80211_S_RUN)
		timeout_add_msec(&sc->beacon_to, 500);
	else if (nstate == IEEE80211_S_INIT)
		sc->cur = -1;
	return sc->sc_newstate(ic, nstate, arg);
}

static int wsim_init(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	(void) sc;
	return 0;
}

static void wsim_stop(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	ifp->if_flags &= ~IFF_RUNNING;
	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	timeout_del(&sc->scan_to);
	timeout_del(&sc->beacon_to);
	mq_purge(&sc->rxq);
	if (sc->aptk_set)
		ieee80211_ccmp_delete_key(ic, &sc->aptk);
	sc->aptk_set = 0;
	sc->cur = -1;
}

static int wsim_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
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
				wsim_init(ifp);
		}
		else if (ifp->if_flags & IFF_RUNNING)
			wsim_stop(ifp);
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
	}
	if (error == ENETRESET)
	{
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING))
		{
			wsim_stop(ifp);
			wsim_init(ifp);
		}
		error = 0;
	}
	return error;
}

static int wsim_media_change(struct ifnet *ifp)
{
	int e = ieee80211_media_change(ifp);
	return e == ENETRESET ? 0 : e;
}

static void wsim_watchdog(struct ifnet *ifp)
{
	ifp->if_timer = 0;
	ieee80211_watchdog(ifp);
}

/* ---- attach --------------------------------------------------------------------- */
static int wsim_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa = aux;
	(void) parent;
	(void) match;
	if (uaa->iface == NULL)
		return UMATCH_NONE;
	return uaa->vendor == 0x0403 && uaa->product == 0x6001 ? UMATCH_VENDOR_PRODUCT : UMATCH_NONE;
}

static void wsim_attach(struct device *parent, struct device *self, void *aux)
{
	struct wsim_softc *sc = (struct wsim_softc *) self;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int i;
	(void) parent;
	(void) aux;
	sc->cur = -1;
	mq_init(&sc->rxq, 64, IPL_NET);
	timeout_set(&sc->scan_to, scan_run, sc);
	timeout_set(&sc->beacon_to, beacon_run, sc);
	timeout_set(&sc->rx_to, rx_run, sc);
	for (i = 0; i < NAPS; i++)
		if (aps[i].pass)
			kex80211_pbkdf2_sha1(aps[i].pass, strlen(aps[i].pass), (const uint8_t *) aps[i].ssid, strlen(aps[i].ssid),
			                     aps[i].pmk, 32);

	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_SHPREAMBLE | IEEE80211_C_SHSLOT | IEEE80211_C_RSN;
	ic->ic_max_rssi = 100;   /* RSSI is dBm + 100, as kexrtwn's */
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (i = 1; i <= 13; i++)
	{
		ic->ic_channels[i].ic_freq = ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}
	ic->ic_myaddr[0] = 0x02;
	ic->ic_myaddr[1] = 0x4b;
	ic->ic_myaddr[2] = 0x45;
	ic->ic_myaddr[3] = 0x58;
	ic->ic_myaddr[4] = 0x5e;
	ic->ic_myaddr[5] = 0x01;
	ic->ic_softc = sc;

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = wsim_ioctl;
	ifp->if_start = wsim_start;
	ifp->if_watchdog = wsim_watchdog;
	memcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
	if_attach(ifp);
	ieee80211_ifattach(ifp);
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = wsim_newstate;
	ieee80211_media_init(ifp, wsim_media_change, ieee80211_media_status);
	printf("%s: simulated Wi-Fi adapter, %d simulated access points\n", sc->sc_dev.dv_xname, NAPS);
}

static const struct kexnet_driver wsim_driver = {
	"wsim",
	"Simulated Wi-Fi adapter (KernelExEx test)",
	sizeof(struct wsim_softc),
	wsim_match,
	wsim_attach,
	NULL,
	NULL,
	NULL,
	1,
	kex80211_configure,
	&kex80211_ctl_ops,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &wsim_driver);
}
