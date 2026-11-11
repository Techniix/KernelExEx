/*
 * KernelExEx kexrtwn, FreeBSD side: rtwn's USB attachment, the FreeBSD
 * net80211 functions rtwn calls, and the kexrtwn_f_* entry points the
 * OpenBSD side drives it with (see kexrtwn.h).
 *
 * The FreeBSD ieee80211com/vap/node structures here are FreeBSD's own
 * (compat/net80211, unmodified headers). There is one station vap and one
 * node, the BSS; OpenBSD's net80211 makes every 802.11 decision and tells
 * this side the outcome (channel, BSS, state, keys), which is what FreeBSD's
 * net80211 would otherwise tell rtwn.
 *
 * Copyright (c) 2026 KernelExEx contributors. GPLv2 (see COPYING).
 */
#include "kexfbsd.h"

/* rtwn's USB attachment, whole: its device table and probe/attach/detach are static */
#include "dev/rtwn/usb/rtwn_usb_attach.c"

#include <dev/usb/usb_device.h>
#include <net80211/ieee80211_ratectl.h>
#include "kexrtwn.h"
#include "kexbsd/kexusb.h"

#define KR_NAME "kexrtwn"

struct kexrtwn
{
	struct kexfbsd_device dev;
	struct usb_device udev;
	struct usb_attach_arg uaa;
	struct rtwn_usb_softc *uc;
	struct rtwn_softc *sc;
	struct ieee80211com *ic;
	struct ieee80211vap *vap;
	struct ieee80211_node *bss;
	void *osc;
	int bss_ht;
	struct ieee80211_key swkey;   /* "encrypted in software already" */
};

/* adapters, to find ours from FreeBSD's ic (ic_softc is rtwn's softc) */
static struct kexrtwn *krs[4];
static struct kexrtwn *kr_of(struct ieee80211com *ic)
{
	int i;
	for (i = 0; i < (int) nitems(krs); i++)
		if (krs[i] && krs[i]->ic == ic)
			return krs[i];
	return NULL;
}

/* ---- ciphers: names and sizes only, the work is done elsewhere --------------------- */
static const struct ieee80211_cipher kr_none = { "NONE", IEEE80211_CIPHER_NONE, 0, 0, 0 };
static const struct ieee80211_cipher kr_wep = { "WEP", IEEE80211_CIPHER_WEP, 4, 4, 0 };
static const struct ieee80211_cipher kr_tkip = { "TKIP", IEEE80211_CIPHER_TKIP, 8, 4, 8 };
static const struct ieee80211_cipher kr_ccmp = { "AES-CCM", IEEE80211_CIPHER_AES_CCM, 8, 8, 0 };
const struct ieee80211_cipher ieee80211_cipher_none = { "NONE", IEEE80211_CIPHER_NONE, 0, 0, 0 };

/* ---- rate control: a small AMRR-style climber over the node's rates --------------- */
struct kr_rc
{
	struct ieee80211_node *ni;
	uint8_t rates[16 + 32];   /* dot11 rates (500 kb/s) then MCS | IEEE80211_RATE_MCS */
	int n, cur, ok, fail, frames;
};
static struct kr_rc kr_rcs[4];

static struct kr_rc *rc_of(const struct ieee80211_node *ni, int create)
{
	int i;
	for (i = 0; i < (int) nitems(kr_rcs); i++)
		if (kr_rcs[i].ni == ni)
			return &kr_rcs[i];
	if (!create)
		return NULL;
	for (i = 0; i < (int) nitems(kr_rcs); i++)
		if (!kr_rcs[i].ni)
		{
			memset(&kr_rcs[i], 0, sizeof(kr_rcs[i]));
			kr_rcs[i].ni = __DECONST(struct ieee80211_node *, ni);
			return &kr_rcs[i];
		}
	return &kr_rcs[0];
}

static void rc_set(struct kr_rc *rc)
{
	uint8_t r = rc->rates[rc->cur];
	if (r & IEEE80211_RATE_MCS)
		rc->ni->ni_txrate = IEEE80211_NODE_TXRATE_INIT_HT(r & ~IEEE80211_RATE_MCS);
	else
		rc->ni->ni_txrate = IEEE80211_NODE_TXRATE_INIT_LEGACY(r);
}

static void rc_node_init(struct ieee80211_node *ni)
{
	struct kr_rc *rc = rc_of(ni, 1);
	int i, j;
	rc->n = 0;
	/* legacy rates, ascending (OFDM-only on 5 GHz arrives without CCK anyway) */
	for (i = 0; i < ni->ni_rates.rs_nrates && rc->n < 16; i++)
	{
		uint8_t r = ni->ni_rates.rs_rates[i] & IEEE80211_RATE_VAL;
		for (j = rc->n; j > 0 && rc->rates[j - 1] > r; j--)
			rc->rates[j] = rc->rates[j - 1];
		rc->rates[j] = r;
		rc->n++;
	}
	/* HT: MCS above the legacy rates (single stream first) */
	if (ni->ni_flags & IEEE80211_NODE_HT)
	{
		int base = rc->n;
		/* drop legacy rates above 6 Mb/s-equivalent MCS0: keep the climb monotonic */
		while (base > 0 && rc->rates[base - 1] > 12)
			base--;
		rc->n = base;
		for (i = 0; i < ni->ni_htrates.rs_nrates && rc->n < (int) sizeof(rc->rates); i++)
			rc->rates[rc->n++] = (ni->ni_htrates.rs_rates[i] & 0x7f) | IEEE80211_RATE_MCS;
	}
	if (rc->n == 0)
		rc->rates[rc->n++] = IEEE80211_IS_CHAN_5GHZ(ni->ni_chan) ? 12 : 2;
	rc->cur = rc->n / 2;
	rc->ok = rc->fail = rc->frames = 0;
	rc_set(rc);
}

static void rc_account(struct kr_rc *rc, int ok, int fail)
{
	rc->ok += ok;
	rc->fail += fail;
	rc->frames += ok + fail;
	if (rc->frames < 10)
		return;
	if (rc->fail * 3 > rc->frames && rc->cur > 0)
		rc->cur--;
	else if (rc->fail * 10 < rc->frames && rc->cur + 1 < rc->n)
		rc->cur++;
	rc->ok = rc->fail = rc->frames = 0;
	rc_set(rc);
}

static int rc_rate(struct ieee80211_node *ni, void *arg, uint32_t iarg)
{
	struct kr_rc *rc = rc_of(ni, 0);
	(void) arg;
	(void) iarg;
	if (!rc || rc->n == 0)
		rc_node_init(ni);
	return 0;
}

static void rc_tx_complete(const struct ieee80211_node *ni, const struct ieee80211_ratectl_tx_status *st)
{
	struct kr_rc *rc = rc_of(ni, 0);
	if (rc)
		rc_account(rc, st->status == IEEE80211_RATECTL_TX_SUCCESS, st->status != IEEE80211_RATECTL_TX_SUCCESS);
}

static void rc_tx_update(struct ieee80211vap *vap, struct ieee80211_ratectl_tx_stats *s)
{
	struct ieee80211_node *ni = (s->flags & IEEE80211_RATECTL_TX_STATS_NODE) ? s->ni : vap->iv_bss;
	struct kr_rc *rc = ni ? rc_of(ni, 0) : NULL;
	if (rc)
		rc_account(rc, s->nsuccess, s->nframes - s->nsuccess);
}

static void rc_nop_vap(struct ieee80211vap *vap)
{
	(void) vap;
}

static void rc_node_deinit(struct ieee80211_node *ni)
{
	struct kr_rc *rc = rc_of(ni, 0);
	if (rc)
		rc->ni = NULL;
}

static const struct ieee80211_ratectl kr_ratectl = {
	.ir_name = "kexrtwn",
	.ir_init = rc_nop_vap,
	.ir_deinit = rc_nop_vap,
	.ir_node_init = rc_node_init,
	.ir_node_deinit = rc_node_deinit,
	.ir_rate = rc_rate,
	.ir_tx_complete = rc_tx_complete,
	.ir_tx_update = rc_tx_update,
};

void ieee80211_ratectl_init(struct ieee80211vap *vap)
{
	vap->iv_rate = &kr_ratectl;
}

/* ---- channels --------------------------------------------------------------------- */
u_int ieee80211_ieee2mhz(u_int chan, u_int flags)
{
	if (flags & IEEE80211_CHAN_2GHZ)
		return chan == 14 ? 2484 : chan < 14 ? 2407 + chan * 5 : 2512 + (chan - 15) * 20;
	if (flags & IEEE80211_CHAN_5GHZ)
		return 5000 + chan * 5;
	return chan <= 14 ? (chan == 14 ? 2484 : 2407 + chan * 5) : 5000 + chan * 5;
}

static int add_chan(struct ieee80211_channel chans[], int maxchans, int *nchans, uint8_t ieee, uint32_t flags)
{
	struct ieee80211_channel *c;
	if (*nchans >= maxchans)
		return ENOBUFS;
	c = &chans[(*nchans)++];
	memset(c, 0, sizeof(*c));
	c->ic_ieee = ieee;
	c->ic_flags = flags;
	c->ic_freq = (uint16_t) ieee80211_ieee2mhz(ieee, flags);
	c->ic_maxregpower = 20;   /* dBm */
	c->ic_maxpower = 2 * 20;  /* 0.5 dBm units */
	c->ic_minpower = 0;
	return 0;
}

int ieee80211_add_channels_default_2ghz(struct ieee80211_channel chans[], int maxchans, int *nchans,
                                        const uint8_t bands[], int cbw_flags)
{
	uint8_t ch;
	(void) cbw_flags;   /* 20 MHz only (see kexrtwn_f_attach) */
	for (ch = 1; ch <= 13; ch++)
	{
		if (isset(bands, IEEE80211_MODE_11B))
			add_chan(chans, maxchans, nchans, ch, IEEE80211_CHAN_B);
		if (isset(bands, IEEE80211_MODE_11G))
			add_chan(chans, maxchans, nchans, ch, IEEE80211_CHAN_G);
		if (isset(bands, IEEE80211_MODE_11NG))
			add_chan(chans, maxchans, nchans, ch, IEEE80211_CHAN_G | IEEE80211_CHAN_HT20);
	}
	return 0;
}

int ieee80211_add_channel_list_5ghz(struct ieee80211_channel chans[], int maxchans, int *nchans,
                                    const uint8_t ieee[], int nieee, const uint8_t bands[], int cbw_flags)
{
	int i;
	(void) cbw_flags;
	for (i = 0; i < nieee; i++)
	{
		/* DFS channels: receive only until a beacon is heard (OpenBSD handles passive scanning) */
		uint32_t pass = (ieee[i] >= 52 && ieee[i] <= 144) ? IEEE80211_CHAN_PASSIVE : 0;
		if (isset(bands, IEEE80211_MODE_11A))
			add_chan(chans, maxchans, nchans, ieee[i], IEEE80211_CHAN_A | pass);
		if (isset(bands, IEEE80211_MODE_11NA))
			add_chan(chans, maxchans, nchans, ieee[i], IEEE80211_CHAN_A | IEEE80211_CHAN_HT20 | pass);
	}
	return 0;
}

enum ieee80211_phymode ieee80211_chan2mode(const struct ieee80211_channel *c)
{
	if (c == IEEE80211_CHAN_ANYC)
		return IEEE80211_MODE_AUTO;
	if (IEEE80211_IS_CHAN_HTA(c))
		return IEEE80211_MODE_11NA;
	if (IEEE80211_IS_CHAN_HTG(c))
		return IEEE80211_MODE_11NG;
	if (IEEE80211_IS_CHAN_A(c))
		return IEEE80211_MODE_11A;
	if (IEEE80211_IS_CHAN_ANYG(c))
		return IEEE80211_MODE_11G;
	if (IEEE80211_IS_CHAN_B(c))
		return IEEE80211_MODE_11B;
	return IEEE80211_MODE_11G;
}

/* freq, and HT or not: the closest variant in ic_channels */
static struct ieee80211_channel *find_chan(struct ieee80211com *ic, uint16_t freq, int ht)
{
	struct ieee80211_channel *best = NULL;
	int i;
	for (i = 0; i < ic->ic_nchans; i++)
	{
		struct ieee80211_channel *c = &ic->ic_channels[i];
		if (c->ic_freq != freq)
			continue;
		if (ht && IEEE80211_IS_CHAN_HT20(c))
			return c;
		if (!ht && !IEEE80211_IS_CHAN_HT(c) && !IEEE80211_IS_CHAN_B(c))
			return c;
		if (!best)
			best = c;
	}
	return best;
}

static const struct ieee80211_rateset rates_11b = { 4, { 2, 4, 11, 22 } };
static const struct ieee80211_rateset rates_11g = { 12, { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 } };
static const struct ieee80211_rateset rates_11a = { 8, { 12, 18, 24, 36, 48, 72, 96, 108 } };

const struct ieee80211_rateset *ieee80211_get_suprates(struct ieee80211com *ic, const struct ieee80211_channel *c)
{
	(void) ic;
	if (IEEE80211_IS_CHAN_5GHZ(c))
		return &rates_11a;
	if (IEEE80211_IS_CHAN_B(c))
		return &rates_11b;
	return &rates_11g;
}

/* ---- nodes -------------------------------------------------------------------------- */
static struct ieee80211_node *kr_node_alloc(struct ieee80211vap *vap, const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct ieee80211_node *ni = (struct ieee80211_node *) malloc(sizeof(*ni), M_80211_NODE, M_NOWAIT | M_ZERO);
	(void) vap;
	(void) mac;
	return ni;
}

/* the BSS node lives as long as the vap: "freeing" it only forgets the association */
static void kr_node_free(struct ieee80211_node *ni)
{
	(void) ni;
}

struct ieee80211_node *_ieee80211_ref_node(struct ieee80211_node *ni, const char *func, int line)
{
	(void) func;
	(void) line;
	if (ni)
		atomic_add_int(&ni->ni_refcnt, 1);
	return ni;
}

void _ieee80211_free_node(struct ieee80211_node *ni, const char *func, int line)
{
	(void) func;
	(void) line;
	if (ni && ni->ni_refcnt > 0)
		atomic_subtract_int(&ni->ni_refcnt, 1);
}

struct ieee80211_node *_ieee80211_find_rxnode(struct ieee80211com *ic, const struct ieee80211_frame_min *wh,
                                              const char *func, int line)
{
	struct kexrtwn *kr = kr_of(ic);
	uint8_t type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	(void) func;
	(void) line;
	if (!kr || !kr->vap || kr->vap->iv_state < IEEE80211_S_AUTH)
		return NULL;
	/* control frames have no transmitter address we could trust */
	if (type == IEEE80211_FC0_TYPE_CTL)
		return NULL;
	if (!IEEE80211_ADDR_EQ(wh->i_addr2, kr->bss->ni_macaddr))
		return NULL;
	return _ieee80211_ref_node(kr->bss, func, line);
}

void ieee80211_node_get_txrate(struct ieee80211_node *ni, struct ieee80211_node_txrate *txr)
{
	*txr = ni->ni_txrate;
}

void ieee80211_output_seqno_assign(struct ieee80211_node *ni, int tid, struct mbuf *m)
{
	/* OpenBSD's stack numbered the frame already: hand that number to the descriptor */
	const struct ieee80211_frame *wh = mtod(m, const struct ieee80211_frame *);
	(void) ni;
	(void) tid;
	M_SEQNO_SET(m, le16dec(wh->i_seq) >> IEEE80211_SEQ_SEQ_SHIFT);
}

bool ieee80211_ht_check_tx_ht(const struct ieee80211_node *ni)
{
	return (ni->ni_flags & IEEE80211_NODE_HT) != 0 && ni->ni_chan != IEEE80211_CHAN_ANYC &&
	       IEEE80211_IS_CHAN_HT(ni->ni_chan);
}

bool ieee80211_ht_check_tx_ht40(const struct ieee80211_node *ni)
{
	(void) ni;
	return false;
}

bool ieee80211_ht_check_tx_shortgi_20(const struct ieee80211_node *ni)
{
	return ieee80211_ht_check_tx_ht(ni) && (ni->ni_htcap & IEEE80211_HTCAP_SHORTGI20) &&
	       (ni->ni_ic->ic_htcaps & IEEE80211_HTCAP_SHORTGI20);
}

bool ieee80211_ht_check_tx_shortgi_40(const struct ieee80211_node *ni)
{
	(void) ni;
	return false;
}

int ieee80211_ht_get_node_ampdu_density(const struct ieee80211_node *ni)
{
	return (ni->ni_htparam & IEEE80211_HTCAP_MPDUDENSITY) >> IEEE80211_HTCAP_MPDUDENSITY_S;
}

bool ieee80211_vht_check_tx_bw(const struct ieee80211_node *ni, enum net80211_sta_rx_bw bw)
{
	(void) ni;
	return bw == NET80211_STA_RX_BW_20;
}

/* ---- keys ----------------------------------------------------------------------------- */
bool ieee80211_is_key_global(const struct ieee80211vap *vap, const struct ieee80211_key *key)
{
	return key >= &vap->iv_nw_keys[0] && key < &vap->iv_nw_keys[IEEE80211_WEP_NKID];
}

int ieee80211_crypto_get_key_wepidx(const struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	if (ieee80211_is_key_global(vap, k))
		return (int) (k - vap->iv_nw_keys);
	return -1;
}

/* the OpenBSD side protected the frame itself (software) or wrote the CCMP header for
 * the hardware to fill in; csum_data carries which (see kexrtwn_f_transmit) */
#define KR_TX_HWKEY 0x4b520001
struct ieee80211_key *ieee80211_crypto_encap(struct ieee80211_node *ni, struct mbuf *m)
{
	struct kexrtwn *kr = kr_of(ni->ni_ic);
	if (m->m_pkthdr.csum_data == KR_TX_HWKEY && ni->ni_ucastkey.wk_cipher &&
	    !(ni->ni_ucastkey.wk_flags & IEEE80211_KEY_SWCRYPT))
		return &ni->ni_ucastkey;
	return &kr->swkey;
}

void ieee80211_set_software_ciphers(struct ieee80211com *ic, uint32_t cipher_suite)
{
	(void) ic;
	(void) cipher_suite;
}

/* ---- receive and transmit -------------------------------------------------------------- */
#define KR_RX_VALID 0x01000000
int ieee80211_add_rx_params(struct mbuf *m, const struct ieee80211_rx_stats *rxs)
{
	int dbm = (int) rxs->c_rssi + rxs->c_nf;
	if (dbm > 0)
		dbm = 0;
	if (dbm < -127)
		dbm = -127;
	m->m_pkthdr.csum_data = KR_RX_VALID | (uint8_t) (int8_t) dbm | ((uint32_t) (uint8_t) rxs->c_nf << 8);
	return 1;
}

static int kr_input(struct ieee80211com *ic, struct mbuf *m)
{
	struct kexrtwn *kr = kr_of(ic);
	struct kexrtwn_rxinfo ri;
	memset(&ri, 0, sizeof(ri));
	if (m->m_pkthdr.csum_data & KR_RX_VALID)
	{
		ri.rssi = (int8_t) (m->m_pkthdr.csum_data & 0xff);
		ri.noise = (int8_t) ((m->m_pkthdr.csum_data >> 8) & 0xff);
	}
	else
		ri.rssi = ri.noise = -95;
	ri.decrypted = (m->m_flags & M_WEP) != 0;
	/* FreeBSD's flags and header fields mean nothing to the OpenBSD stack */
	m->m_flags &= M_EXT | M_PKTHDR | M_EOR;
	m->m_nextpkt = NULL;
	m->m_pkthdr.csum_flags = 0;
	m->m_pkthdr.ether_vtag = 0;
	m->m_pkthdr.tso_segsz = 0;
	m->m_pkthdr.flowid16 = 0;
	m->m_pkthdr.rcvif = NULL;
	m->m_pkthdr.loopcnt = 0;
	m->m_pkthdr.csum_data = 0;
	m->m_pkthdr.ifidx = 0;
	if (!kr || !kr->osc)
	{
		m_freem(m);
		return 0;
	}
	kexrtwn_o_input(kr->osc, m, &ri);
	return 0;
}

int ieee80211_input_mimo(struct ieee80211_node *ni, struct mbuf *m)
{
	return kr_input(ni->ni_ic, m);
}

int ieee80211_input_mimo_all(struct ieee80211com *ic, struct mbuf *m)
{
	return kr_input(ic, m);
}

void ieee80211_tx_complete(struct ieee80211_node *ni, struct mbuf *m, int status)
{
	struct kexrtwn *kr = ni ? kr_of(ni->ni_ic) : NULL;
	if (ni)
		ieee80211_free_node(ni);
	if (!m)
		return;
	m->m_flags &= M_EXT | M_PKTHDR | M_EOR;
	m->m_pkthdr.rcvif = NULL;
	if (kr && kr->osc)
		kexrtwn_o_tx_done(kr->osc, m, status == 0);
	else
		m_freem(m);
}

void ieee80211_process_callback(struct ieee80211_node *ni, struct mbuf *m, int status)
{
	(void) ni;
	(void) m;
	(void) status;
}

void ieee80211_radiotap_attach(struct ieee80211com *ic, struct ieee80211_radiotap_header *th, int tlen,
                               uint32_t tx_radiotap, struct ieee80211_radiotap_header *rh, int rlen,
                               uint32_t rx_radiotap)
{
	(void) ic;
	(void) th;
	(void) tlen;
	(void) tx_radiotap;
	(void) rh;
	(void) rlen;
	(void) rx_radiotap;
}

void ieee80211_radiotap_tx(struct ieee80211vap *vap, struct mbuf *m)
{
	(void) vap;
	(void) m;
}

/* ---- the common and vap bits ------------------------------------------------------------ */
extern void (*kexfbsd_sysinit_wlan_phy)(void *);

void ieee80211_ifattach(struct ieee80211com *ic)
{
	static int phy_done;
	if (!phy_done)
	{
		phy_done = 1;
		kexfbsd_sysinit_wlan_phy(NULL);   /* ieee80211_phy.c's rate tables */
	}
	IEEE80211_LOCK_INIT(ic, ic->ic_name);
	IEEE80211_TX_LOCK_INIT(ic, ic->ic_name);
	TAILQ_INIT(&ic->ic_vaps);
	ic->ic_node_alloc = kr_node_alloc;
	ic->ic_node_free = kr_node_free;
	ic->ic_bsschan = IEEE80211_CHAN_ANYC;
	ic->ic_curchan = ic->ic_nchans ? &ic->ic_channels[0] : IEEE80211_CHAN_ANYC;
	ic->ic_prevchan = ic->ic_curchan;
	ic->ic_curmode = ieee80211_chan2mode(ic->ic_curchan);
	ic->ic_rt = ieee80211_get_ratetable(ic->ic_curchan);
	ic->ic_txpowlimit = IEEE80211_TXPOWER_MAX;
	ic->ic_protmode = IEEE80211_PROT_CTSONLY;
	ic->ic_htprotmode = IEEE80211_PROT_RTSCTS;
}

void ieee80211_ifdetach(struct ieee80211com *ic)
{
	(void) ic;
}

static int kr_vap_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	(void) arg;
	vap->iv_state = nstate;
	return 0;
}

int ieee80211_vap_setup(struct ieee80211com *ic, struct ieee80211vap *vap, const char name[IFNAMSIZ], int unit,
                        enum ieee80211_opmode opmode, int flags, const uint8_t bssid[IEEE80211_ADDR_LEN])
{
	int m;
	(void) name;
	(void) unit;
	(void) flags;
	(void) bssid;
	vap->iv_ic = ic;
	vap->iv_opmode = opmode;
	vap->iv_state = IEEE80211_S_INIT;
	vap->iv_newstate = kr_vap_newstate;
	vap->iv_flags = IEEE80211_F_WME | IEEE80211_F_SHPREAMBLE;
	vap->iv_flags_ht = IEEE80211_FHT_HT | IEEE80211_FHT_SHORTGI20;
	vap->iv_rtsthreshold = IEEE80211_RTS_MAX;
	vap->iv_bmissthreshold = 7;
	for (m = 0; m < IEEE80211_MODE_MAX; m++)
	{
		struct ieee80211_txparam *tp = &vap->iv_txparms[m];
		int ofdm = m == IEEE80211_MODE_11A || m == IEEE80211_MODE_11NA || m == IEEE80211_MODE_VHT_5GHZ;
		tp->ucastrate = IEEE80211_FIXED_RATE_NONE;
		tp->mgmtrate = ofdm ? 12 : 2;
		tp->mcastrate = ofdm ? 12 : 2;
		tp->maxretry = IEEE80211_TXMAX_DEFAULT;
	}
	vap->iv_bss = ic->ic_node_alloc(vap, ic->ic_macaddr);
	if (!vap->iv_bss)
		return ENOMEM;
	vap->iv_bss->ni_vap = vap;
	vap->iv_bss->ni_ic = ic;
	vap->iv_bss->ni_refcnt = 1;
	vap->iv_bss->ni_chan = IEEE80211_CHAN_ANYC;
	vap->iv_bss->ni_txparms = &vap->iv_txparms[IEEE80211_MODE_11G];
	return 0;
}

int ieee80211_vap_attach(struct ieee80211vap *vap, ifm_change_cb_t mc, ifm_stat_cb_t ms,
                         const uint8_t macaddr[IEEE80211_ADDR_LEN])
{
	struct ieee80211com *ic = vap->iv_ic;
	(void) mc;
	(void) ms;
	IEEE80211_ADDR_COPY(vap->iv_myaddr, macaddr);
	IEEE80211_ADDR_COPY(vap->iv_bss->ni_macaddr, macaddr);
	TAILQ_INSERT_TAIL(&ic->ic_vaps, vap, iv_next);
	return 1;
}

void ieee80211_vap_detach(struct ieee80211vap *vap)
{
	(void) vap;
}

int ieee80211_media_change(struct ifnet *ifp)
{
	(void) ifp;
	return 0;
}

void ieee80211_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	(void) ifp;
	(void) imr;
}

void ieee80211_start_all(struct ieee80211com *ic)
{
	(void) ic;   /* the OpenBSD side starts scanning once init returns */
}

void ieee80211_stop(struct ieee80211vap *vap)
{
	(void) vap;
}

void ieee80211_stop_locked(struct ieee80211vap *vap)
{
	(void) vap;
}

void ieee80211_restart_all(struct ieee80211com *ic)
{
	struct kexrtwn *kr = kr_of(ic);
	if (kr && kr->osc)
		kexrtwn_o_restart(kr->osc);
}

void ieee80211_suspend_all(struct ieee80211com *ic)
{
	(void) ic;
}

void ieee80211_resume_all(struct ieee80211com *ic)
{
	(void) ic;
}

struct mbuf *ieee80211_beacon_alloc(struct ieee80211_node *ni)
{
	(void) ni;
	return NULL;   /* station only */
}

int ieee80211_beacon_update(struct ieee80211_node *ni, struct mbuf *m, int mcast)
{
	(void) ni;
	(void) m;
	(void) mcast;
	return 0;
}

int ieee80211_ibss_merge(struct ieee80211_node *ni)
{
	(void) ni;
	return 0;
}

void ieee80211_dfs_notify_radar(struct ieee80211com *ic, struct ieee80211_channel *c)
{
	(void) ic;
	(void) c;
}

/* 802.11 default EDCA parameters for a station (the AP's come later) */
void ieee80211_wme_ic_getparams(struct ieee80211com *ic, struct chanAccParams *wp)
{
	static const struct
	{
		uint8_t aifsn, logcwmin, logcwmax;
		uint16_t txop;
	} def[WME_NUM_AC] = { { 3, 4, 10, 0 }, { 7, 4, 10, 0 }, { 2, 3, 4, 94 }, { 2, 2, 3, 47 } };
	int ac;
	(void) ic;
	memset(wp, 0, sizeof(*wp));
	for (ac = 0; ac < WME_NUM_AC; ac++)
	{
		wp->cap_wmeParams[ac].wmep_aifsn = def[ac].aifsn;
		wp->cap_wmeParams[ac].wmep_logcwmin = def[ac].logcwmin;
		wp->cap_wmeParams[ac].wmep_logcwmax = def[ac].logcwmax;
		wp->cap_wmeParams[ac].wmep_txopLimit = def[ac].txop;
	}
}

void net80211_ic_printf(const struct ieee80211com *ic, const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	kexbsd_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	kexbsd_printf("%s: %s", ic->ic_name ? ic->ic_name : KR_NAME, buf);
}

/* ---- interface odds and ends ------------------------------------------------------------- */
void kexfbsd_if_inc_counter(struct ifnet *ifp, ift_counter c, int64_t v)
{
	(void) ifp;
	(void) c;
	(void) v;
}

struct llm_iter
{
	iflladdr_cb_t *cb;
	void *arg;
	u_int n;
};

static void llm_one(void *arg, const uint8_t *mac)
{
	struct llm_iter *it = (struct llm_iter *) arg;
	struct sockaddr_dl sdl;
	memset(&sdl, 0, sizeof(sdl));
	sdl.sdl_alen = ETHER_ADDR_LEN;
	memcpy(sdl.sdl_data, mac, ETHER_ADDR_LEN);
	it->n += it->cb(it->arg, &sdl, it->n);
}

u_int kexfbsd_if_foreach_llmaddr(struct ifnet *ifp, iflladdr_cb_t *cb, void *arg)
{
	struct kexrtwn *kr = (struct kexrtwn *) ifp;   /* iv_ifp points at us */
	struct llm_iter it = { cb, arg, 0 };
	if (kr && kr->osc)
		kexrtwn_o_foreach_mcast(kr->osc, llm_one, &it);
	return it.n;
}

/* =====================================================================================
 * kexrtwn_f_*: the OpenBSD side's handle on rtwn
 * ===================================================================================== */
int kexrtwn_f_ndevs(void)
{
	return (int) nitems(rtwn_devs);
}

void kexrtwn_f_dev(int i, uint16_t *vid, uint16_t *pid)
{
	*vid = rtwn_devs[i].idVendor;
	*pid = rtwn_devs[i].idProduct;
}

static void fill_uaa(struct usb_attach_arg *uaa, const usb_device_descriptor_t *dd, uint16_t vid, uint16_t pid,
                     int config_index, int iface_index)
{
	memset(uaa, 0, sizeof(*uaa));
	uaa->usb_mode = USB_MODE_HOST;
	uaa->info.idVendor = vid;
	uaa->info.idProduct = pid;
	if (dd)
	{
		uaa->info.bcdDevice = UGETW(dd->bcdDevice);
		uaa->info.bDeviceClass = dd->bDeviceClass;
		uaa->info.bDeviceSubClass = dd->bDeviceSubClass;
		uaa->info.bDeviceProtocol = dd->bDeviceProtocol;
	}
	uaa->info.bIfaceIndex = (uint8_t) iface_index;
	uaa->info.bIfaceNum = (uint8_t) iface_index;
	uaa->info.bConfigIndex = (uint8_t) config_index;
	uaa->info.bConfigNum = (uint8_t) (config_index + 1);
}

int kexrtwn_f_match(uint16_t vid, uint16_t pid, int config_index, int iface_index)
{
	struct kexfbsd_device d;
	struct usb_attach_arg uaa;
	fill_uaa(&uaa, NULL, vid, pid, config_index, iface_index);
	memset(&d, 0, sizeof(d));
	d.ivars = &uaa;
	return rtwn_usb_match(&d) == 0;
}

void *kexrtwn_f_attach(const struct kexrtwn_usbinfo *ui, const char *name, struct kexrtwn_radio *out)
{
	const usb_device_descriptor_t *dd = (const usb_device_descriptor_t *) ui->ddesc;
	struct kexrtwn *kr;
	struct ieee80211com *ic;
	static const uint8_t zero[IEEE80211_ADDR_LEN];
	int i, n = 0;

	kr = (struct kexrtwn *) malloc(sizeof(*kr), M_DEVBUF, M_WAITOK | M_ZERO);
	if (!kr)
		return NULL;
	strlcpy(kr->dev.nameunit, name, sizeof(kr->dev.nameunit));
	kr->dev.name = "rtwn";
	kr->uc = (struct rtwn_usb_softc *) malloc(sizeof(*kr->uc), M_DEVBUF, M_WAITOK | M_ZERO);
	if (!kr->uc)
		goto fail;
	kr->dev.softc = kr->uc;
	kr->dev.ivars = &kr->uaa;

	/* the FreeBSD view of the device: endpoints with their pipes */
	memcpy(&kr->udev.ddesc, dd, sizeof(kr->udev.ddesc));
	kr->udev.speed = ui->speed >= 2 ? USB_SPEED_HIGH : USB_SPEED_FULL;
	kr->udev.kexusb = ui->kdev;
	kr->udev.cdesc = (uint8_t *) __DECONST(void *, ui->cdesc);
	for (i = 0; i < ui->nep && n < KEXFBSD_USB_MAX_EP; i++)
	{
		kr->udev.endpoints[n].edesc = (struct usb_endpoint_descriptor *) __DECONST(void *, ui->ep[i].edesc);
		kr->udev.endpoints[n].iface_index = ui->ep[i].iface_index;
		kr->udev.pipe[n] = ui->ep[i].pipe;
		n++;
	}
	kr->udev.endpoints_max = (uint8_t) n;
	kr->udev.worker = kexbsd_worker_create("rtwnusb");
	if (!kr->udev.worker)
		goto fail;
	fill_uaa(&kr->uaa, dd, UGETW(dd->idVendor), UGETW(dd->idProduct), ui->config_index, ui->iface_index);
	if (usbd_lookup_id_by_uaa(rtwn_devs, sizeof(rtwn_devs), &kr->uaa))
		goto fail;
	kr->uaa.device = &kr->udev;

	if (rtwn_usb_attach(&kr->dev) != 0)
		goto fail;
	kr->sc = &kr->uc->uc_sc;
	ic = kr->ic = &kr->sc->sc_ic;
	for (i = 0; i < (int) nitems(krs); i++)
		if (!krs[i])
		{
			krs[i] = kr;
			break;
		}

	/* one station vap; its "ifnet" is us (only if_foreach_llmaddr looks at it) */
	kr->vap = ic->ic_vap_create(ic, "wlan", 0, IEEE80211_M_STA, 0, zero, ic->ic_macaddr);
	if (!kr->vap)
	{
		device_printf(&kr->dev, "cannot create the station interface\n");
		goto fail_attached;
	}
	kr->vap->iv_ifp = (struct ifnet *) kr;
	kr->bss = kr->vap->iv_bss;
	kr->swkey.wk_cipher = &kr_none;
	kr->swkey.wk_flags = IEEE80211_KEY_SWCRYPT;

	memset(out, 0, sizeof(*out));
	IEEE80211_ADDR_COPY(out->macaddr, ic->ic_macaddr);
	out->ntxchains = kr->sc->ntxchains;
	out->nrxchains = kr->sc->nrxchains;
	out->ht = (ic->ic_htcaps & IEEE80211_HTC_HT) != 0;
	strlcpy(out->chipname, kr->sc->name ? kr->sc->name : "Realtek", sizeof(out->chipname));
	for (i = 0; i < ic->ic_nchans && out->nchan < KEXRTWN_MAXCHAN; i++)
	{
		const struct ieee80211_channel *c = &ic->ic_channels[i];
		int j;
		for (j = 0; j < out->nchan; j++)
			if (out->chan[j].freq == c->ic_freq)
				break;
		if (j == out->nchan)
		{
			out->chan[j].freq = c->ic_freq;
			out->chan[j].ieee = c->ic_ieee;
			out->chan[j].flags = IEEE80211_IS_CHAN_2GHZ(c) ? KEXRTWN_CHAN_2GHZ : KEXRTWN_CHAN_5GHZ;
			out->nchan++;
		}
		if (IEEE80211_IS_CHAN_PASSIVE(c))
			out->chan[j].flags |= KEXRTWN_CHAN_PASSIVE;
		if (IEEE80211_IS_CHAN_HT(c))
			out->chan[j].flags |= KEXRTWN_CHAN_HT;
	}
	return kr;

fail_attached:
	rtwn_usb_detach(&kr->dev);
fail:
	if (kr->udev.worker)
		kexbsd_worker_destroy((kexbsd_worker *) kr->udev.worker);
	if (kr->uc)
		free(kr->uc, M_DEVBUF);
	free(kr, M_DEVBUF);
	return NULL;
}

void kexrtwn_f_set_osc(void *h, void *osc)
{
	((struct kexrtwn *) h)->osc = osc;
}

void kexrtwn_f_detach(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	if (!kr)
		return;
	kr->osc = NULL;
	rtwn_usb_detach(&kr->dev);
	{
		int i;
		for (i = 0; i < (int) nitems(krs); i++)
			if (krs[i] == kr)
				krs[i] = NULL;
	}
	kexbsd_worker_destroy((kexbsd_worker *) kr->udev.worker);
	free(kr->uc, M_DEVBUF);
	free(kr, M_DEVBUF);
}

int kexrtwn_f_init(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	kr->ic->ic_nrunning = 1;
	kr->ic->ic_parent(kr->ic);   /* rtwn_parent: rtwn_init */
	return (kr->sc->sc_flags & RTWN_RUNNING) ? 0 : EIO;
}

void kexrtwn_f_stop(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	kr->ic->ic_nrunning = 0;
	kr->ic->ic_parent(kr->ic);   /* rtwn_stop */
}

int kexrtwn_f_running(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	return (kr->sc->sc_flags & RTWN_RUNNING) != 0;
}

void kexrtwn_f_set_channel(void *h, uint16_t freq)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	struct ieee80211com *ic = kr->ic;
	int assoc = kr->vap->iv_state >= IEEE80211_S_AUTH;
	struct ieee80211_channel *c = find_chan(ic, freq, assoc && kr->bss_ht);
	if (!c)
		return;
	IEEE80211_LOCK(ic);
	ic->ic_prevchan = ic->ic_curchan;
	ic->ic_curchan = c;
	ic->ic_curmode = ieee80211_chan2mode(c);
	ic->ic_rt = ieee80211_get_ratetable(c);
	IEEE80211_UNLOCK(ic);
	ic->ic_set_channel(ic);
}

void kexrtwn_f_scan_start(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	kr->ic->ic_flags |= IEEE80211_F_SCAN;
	kr->ic->ic_scan_start(kr->ic);
}

void kexrtwn_f_scan_end(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	kr->ic->ic_scan_end(kr->ic);
	kr->ic->ic_flags &= ~IEEE80211_F_SCAN;
}

void kexrtwn_f_set_bss(void *h, const struct kexrtwn_bss *b)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	struct ieee80211com *ic = kr->ic;
	struct ieee80211_node *ni = kr->bss;
	struct ieee80211_channel *c;
	int i;
	kr->bss_ht = b->ht && (ic->ic_htcaps & IEEE80211_HTC_HT);
	c = find_chan(ic, b->freq, kr->bss_ht);
	IEEE80211_LOCK(ic);
	IEEE80211_ADDR_COPY(ni->ni_macaddr, b->macaddr);
	IEEE80211_ADDR_COPY(ni->ni_bssid, b->bssid);
	ni->ni_chan = c ? c : IEEE80211_CHAN_ANYC;
	ic->ic_bsschan = ni->ni_chan;
	ni->ni_associd = b->associd ? (b->associd | 0xc000) : 0;
	ni->ni_intval = b->intval ? b->intval : 100;
	ni->ni_capinfo = b->capinfo;
	ni->ni_rates.rs_nrates = b->nrates > IEEE80211_RATE_MAXSIZE ? IEEE80211_RATE_MAXSIZE : b->nrates;
	for (i = 0; i < ni->ni_rates.rs_nrates; i++)
		ni->ni_rates.rs_rates[i] = b->rates[i];
	ni->ni_flags &= ~(IEEE80211_NODE_HT | IEEE80211_NODE_QOS);
	ni->ni_htrates.rs_nrates = 0;
	if (kr->bss_ht)
	{
		int nmcs = b->nmcs;
		if (nmcs > 8 * kr->sc->ntxchains)
			nmcs = 8 * kr->sc->ntxchains;
		ni->ni_flags |= IEEE80211_NODE_HT | IEEE80211_NODE_QOS;
		for (i = 0; i < nmcs && i < IEEE80211_HTRATE_MAXSIZE; i++)
			ni->ni_htrates.rs_rates[i] = (uint8_t) i;
		ni->ni_htrates.rs_nrates = (uint8_t) i;
		ni->ni_htcap = b->htcap;
		ni->ni_htparam = b->ampdu_param;
	}
	ni->ni_txparms = &kr->vap->iv_txparms[ieee80211_chan2mode(ni->ni_chan)];
	if (b->shortpre)
		ic->ic_flags |= IEEE80211_F_SHPREAMBLE;
	else
		ic->ic_flags &= ~IEEE80211_F_SHPREAMBLE;
	if (b->shortslot)
		ic->ic_flags |= IEEE80211_F_SHSLOT;
	else
		ic->ic_flags &= ~IEEE80211_F_SHSLOT;
	IEEE80211_UNLOCK(ic);
}

int kexrtwn_f_newstate(void *h, int nstate)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	struct ieee80211com *ic = kr->ic;
	struct ieee80211vap *vap = kr->vap;
	enum ieee80211_state fs, os = vap->iv_state;
	int err;
	switch (nstate)
	{
	case KEXRTWN_S_SCAN:
		fs = IEEE80211_S_SCAN;
		break;
	case KEXRTWN_S_AUTH:
		fs = IEEE80211_S_AUTH;
		break;
	case KEXRTWN_S_ASSOC:
		fs = IEEE80211_S_ASSOC;
		break;
	case KEXRTWN_S_RUN:
		fs = IEEE80211_S_RUN;
		break;
	default:
		fs = IEEE80211_S_INIT;
		break;
	}
	if (fs == os)
		return 0;
	/* leaving the association: forget the BSS node's MAC ID (rtwn_node_free) */
	if (os == IEEE80211_S_RUN && RTWN_NODE(kr->bss)->id != RTWN_MACID_UNDEFINED)
	{
		ic->ic_node_free(kr->bss);
		RTWN_NODE(kr->bss)->id = RTWN_MACID_UNDEFINED;
		rc_node_deinit(kr->bss);
	}
	/* joining: a MAC ID and rates for the BSS (rtwn_newassoc, as net80211 would) */
	if (fs == IEEE80211_S_RUN)
	{
		ic->ic_newassoc(kr->bss, 1);
		rc_node_init(kr->bss);
		if (ic->ic_wme.wme_update)
			ic->ic_wme.wme_update(ic);
	}
	IEEE80211_LOCK(ic);
	err = vap->iv_newstate(vap, fs, -1);
	IEEE80211_UNLOCK(ic);
	return err;
}

int kexrtwn_f_transmit(void *h, struct mbuf *m, int tobss, int eapol, int hwkey)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	struct ieee80211_node *ni = ieee80211_ref_node(kr->bss);
	const struct ieee80211_frame *wh = mtod(m, const struct ieee80211_frame *);
	int err, ac = WME_AC_BE;
	(void) tobss;
	if (IEEE80211_QOS_HAS_SEQ(wh))
	{
		static const uint8_t tid2ac[8] = { WME_AC_BE, WME_AC_BK, WME_AC_BK, WME_AC_BE,
			                               WME_AC_VI, WME_AC_VI, WME_AC_VO, WME_AC_VO };
		const uint8_t *qos = ((const struct ieee80211_qosframe *) wh)->i_qos;
		ac = tid2ac[qos[0] & IEEE80211_QOS_TID];
	}
	if (!ni->ni_txparms)
		ni->ni_txparms = &kr->vap->iv_txparms[ieee80211_chan2mode(kr->ic->ic_curchan)];
	m->m_flags &= M_EXT | M_PKTHDR | M_EOR;
	if (eapol)
		m->m_flags |= M_EAPOL;
	m->m_pkthdr.rcvif = (struct ifnet *) ni;
	m->m_pkthdr.csum_data = hwkey ? KR_TX_HWKEY : 0;
	M_WME_SETAC(m, ac);
	M_SEQNO_SET(m, le16dec(wh->i_seq) >> IEEE80211_SEQ_SEQ_SHIFT);
	err = kr->ic->ic_transmit(kr->ic, m);
	if (err)
	{
		ieee80211_free_node(ni);
		m->m_flags &= M_EXT | M_PKTHDR | M_EOR;
		m->m_pkthdr.rcvif = NULL;
		kexrtwn_o_m_freem(m);
	}
	return err;
}

int kexrtwn_f_set_key(void *h, const uint8_t *mac, const uint8_t *key, int keylen, int cipher, int pairwise,
                      int keyid)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	struct ieee80211vap *vap = kr->vap;
	struct ieee80211_key *k = pairwise ? &kr->bss->ni_ucastkey : &vap->iv_nw_keys[keyid & 3];
	ieee80211_keyix keyix = 0, rxkeyix = 0;
	if (keylen > IEEE80211_KEYBUF_SIZE)
		return EINVAL;
	memset(k, 0, sizeof(*k));
	k->wk_cipher = cipher == KEXRTWN_CIPHER_CCMP ? &kr_ccmp : cipher == KEXRTWN_CIPHER_TKIP ? &kr_tkip : &kr_wep;
	k->wk_keylen = (uint8_t) keylen;
	memcpy(k->wk_key, key, keylen);
	IEEE80211_ADDR_COPY(k->wk_macaddr, pairwise ? mac : kexfbsd_etherbroadcastaddr);
	k->wk_flags = IEEE80211_KEY_XMIT | IEEE80211_KEY_RECV | (pairwise ? 0 : IEEE80211_KEY_GROUP);
	if (!vap->iv_key_alloc(vap, k, &keyix, &rxkeyix))
		return ENOSPC;
	k->wk_keyix = keyix;
	k->wk_rxkeyix = rxkeyix;
	if (k->wk_flags & IEEE80211_KEY_SWCRYPT)
		return EOPNOTSUPP;   /* the OpenBSD side does it in software */
	if (!vap->iv_key_set(vap, k))
		return EIO;
	return 0;
}

void kexrtwn_f_delete_key(void *h, int pairwise, int keyid)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	struct ieee80211vap *vap = kr->vap;
	struct ieee80211_key *k = pairwise ? &kr->bss->ni_ucastkey : &vap->iv_nw_keys[keyid & 3];
	if (!k->wk_cipher)
		return;
	if (!(k->wk_flags & IEEE80211_KEY_SWCRYPT))
		vap->iv_key_delete(vap, k);
	memset(k, 0, sizeof(*k));
}

void kexrtwn_f_update_promisc(void *h, int on)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	kr->ic->ic_promisc = on ? 1 : 0;
	if (kr->ic->ic_update_promisc)
		kr->ic->ic_update_promisc(kr->ic);
}

void kexrtwn_f_update_mcast(void *h)
{
	struct kexrtwn *kr = (struct kexrtwn *) h;
	if (kr->ic->ic_update_mcast)
		kr->ic->ic_update_mcast(kr->ic);
}
