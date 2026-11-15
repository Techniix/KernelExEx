/*
 *  KernelExEx - the Wi-Fi drivers' side of the Wi-Fi program: saved
 *  networks, the list of networks in range, connection state and requests
 *  (connect, disconnect), through the registry (see kexwifi_reg.h).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Everything here runs on the driver thread, at PASSIVE_LEVEL: once at
 *  bring-up (kex80211_ctl_start), every 2 seconds after that
 *  (kex80211_ctl_tick) and when the adapter stops (kex80211_ctl_stop).
 *
 *  Saved networks become net80211's join list (OpenBSD's "ifconfig join"):
 *  net80211 then joins the best saved network in range by itself, and moves
 *  to another one when it goes away. A network set in the adapter's
 *  Advanced properties (the old way) still wins and turns this off.
 *  Saved networks carry the PSK, never the password, and only WPA2 (or
 *  open) networks can be saved; the WPA parameters are the same strict ones
 *  as kex80211_configure's.
 */
#include "obsd/kexobsd_net.h"
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ioctl.h>
#include "kexnet.h"
#include "kexwifi_reg.h"

void kex80211_wpaparams(struct ieee80211_wpaparams *wpa, const char *xname, int allow_tkip);
int kex80211_psk(const uint8_t *ssid, size_t slen, const char *pass, uint8_t out[32]);

/* ---- registry (Zw*; Windows 98 creates one level per call and has no
 * ZwDeleteValueKey, so values are overwritten, never deleted) ------------- */
static void wcat(WCHAR *d, size_t cap, const char *s)
{
	size_t n = 0;
	while (n < cap - 1 && d[n])
		n++;
	while (n < cap - 1 && *s)
		d[n++] = (WCHAR) (unsigned char) *s++;
	d[n] = 0;
}

/* \Registry\Machine\Software\KernelEx\WiFi[\sub[\name]], created as needed */
static HANDLE reg_open(const char *sub, const char *name, int create)
{
	static const char *const base[] = { "Software", "KernelEx", "WiFi" };
	WCHAR path[160];
	UNICODE_STRING us;
	OBJECT_ATTRIBUTES oa;
	HANDLE h = NULL;
	ULONG disp;
	int i, levels = 3 + (sub != NULL) + (name != NULL);
	path[0] = 0;
	wcat(path, nitems(path), "\\Registry\\Machine");
	for (i = 0; i < levels; i++)
	{
		wcat(path, nitems(path), "\\");
		wcat(path, nitems(path), i < 3 ? base[i] : i == 3 ? sub : name);
		if (!create && i < levels - 1)
			continue;
		if (h)
			ZwClose(h);
		h = NULL;
		RtlInitUnicodeString(&us, path);
		InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
		if (create ? !NT_SUCCESS(ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp))
		           : !NT_SUCCESS(ZwOpenKey(&h, KEY_ALL_ACCESS, &oa)))
			return NULL;
	}
	return h;
}

static void reg_set(HANDLE h, const char *name, ULONG type, const void *data, ULONG len)
{
	WCHAR w[32];
	UNICODE_STRING us;
	w[0] = 0;
	wcat(w, nitems(w), name);
	RtlInitUnicodeString(&us, w);
	ZwSetValueKey(h, &us, 0, type, (PVOID) data, len);
}

static void reg_dword(HANDLE h, const char *name, ULONG v)
{
	reg_set(h, name, REG_DWORD, &v, sizeof(v));
}

static void reg_sz(HANDLE h, const char *name, const char *s)
{
	WCHAR w[96];
	w[0] = 0;
	wcat(w, nitems(w), s);
	{
		ULONG n = 0;
		while (w[n])
			n++;
		reg_set(h, name, REG_SZ, w, (n + 1) * sizeof(WCHAR));
	}
}

/* a value's data; returns its length, or -1. REG_SZ comes back as UTF-16
 * from ntkern, so strings are narrowed here (key strings are hex) */
static int reg_get(HANDLE h, const char *name, ULONG *type, void *out, ULONG cap)
{
	union
	{
		KEY_VALUE_PARTIAL_INFORMATION i;
		UCHAR b[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 160];
	} u;
	WCHAR w[32];
	UNICODE_STRING us;
	ULONG got = 0, n;
	w[0] = 0;
	wcat(w, nitems(w), name);
	RtlInitUnicodeString(&us, w);
	if (!NT_SUCCESS(ZwQueryValueKey(h, &us, KeyValuePartialInformation, &u, sizeof(u), &got)))
		return -1;
	*type = u.i.Type;
	n = u.i.DataLength;
	if (n > cap)
		n = cap;
	memcpy(out, u.i.Data, n);
	return (int) n;
}

static ULONG reg_get_dword(HANDLE h, const char *name, ULONG dflt)
{
	ULONG t, v = 0;
	return reg_get(h, name, &t, &v, sizeof(v)) == sizeof(v) ? v : dflt;
}

/* a REG_SZ (UTF-16 or ANSI) as ASCII */
static void reg_get_sz(HANDLE h, const char *name, char *out, size_t cap)
{
	unsigned char b[160];
	ULONG t;
	int n = reg_get(h, name, &t, b, sizeof(b)), i, k = 0;
	int wide = n >= 2 && b[1] == 0;
	out[0] = 0;
	if (n <= 0)
		return;
	for (i = 0; i + (wide ? 1 : 0) < n && (size_t) k + 1 < cap; i += wide ? 2 : 1)
	{
		if (!b[i])
			break;
		out[k++] = (char) b[i];
	}
	out[k] = 0;
}

/* ---- per adapter ------------------------------------------------------------------ */
#define MAX_WIFI 4

struct ctl
{
	struct ifnet *ifp;
	int fixed;       /* Advanced properties name a network */
	int off;         /* the user turned it off */
	/* what was last written, to write only changes */
	ULONG state, channel, bars;
	uint8_t ssid[32], bssid[6];
	int ssid_len;
	ULONG scan_seq;
	int written;
};
static struct ctl ctls[MAX_WIFI];

static struct ctl *ctl_of(struct ifnet *ifp, int add)
{
	int i;
	for (i = 0; i < MAX_WIFI; i++)
		if (ctls[i].ifp == ifp)
			return &ctls[i];
	if (add)
		for (i = 0; i < MAX_WIFI; i++)
			if (!ctls[i].ifp)
			{
				memset(&ctls[i], 0, sizeof(ctls[i]));
				ctls[i].ifp = ifp;
				return &ctls[i];
			}
	return NULL;
}

static HANDLE adapter_key(struct ifnet *ifp)
{
	return reg_open("Adapters", ifp->if_xname, 1);
}

/* signal 0-100 from a node's RSSI: rtwn reports dBm + 100 (ic_max_rssi
 * 100), urtwn dBm (ic_max_rssi 0), others a fraction of ic_max_rssi */
static unsigned int signal_of(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	int dbm;
	if (ic->ic_max_rssi == 100)
		dbm = (int) ni->ni_rssi - 100;
	else if (ic->ic_max_rssi == 0)
		dbm = (int) (int8_t) ni->ni_rssi;
	else
		return (unsigned int) ni->ni_rssi * 100 / ic->ic_max_rssi;
	/* -90 dBm and below is 0 %, -40 dBm and above is 100 % */
	dbm = (dbm + 90) * 2;
	return dbm < 0 ? 0 : dbm > 100 ? 100 : (unsigned int) dbm;
}

static int security_of(struct ieee80211_node *ni)
{
	if (ni->ni_rsnprotos & IEEE80211_PROTO_RSN)
	{
		if (ni->ni_rsnakms & (IEEE80211_AKM_PSK | IEEE80211_AKM_SHA256_PSK))
			return KEXWIFI_SEC_WPA2;
		if (ni->ni_rsnakms & IEEE80211_AKM_SAE)
			return KEXWIFI_SEC_WPA3;
		return KEXWIFI_SEC_ENT;
	}
	if (ni->ni_rsnprotos & IEEE80211_PROTO_WPA)
		return KEXWIFI_SEC_WPA;
	if (ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY)
		return KEXWIFI_SEC_WEP;
	return KEXWIFI_SEC_OPEN;
}

static int is_saved(struct ieee80211com *ic, const uint8_t *ssid, int len)
{
	struct ieee80211_ess *ess;
	TAILQ_FOREACH(ess, &ic->ic_ess, ess_next)
		if (ess->esslen == len && !memcmp(ess->essid, ssid, (size_t) len))
			return 1;
	return 0;
}

static void hex_name(const uint8_t *ssid, int len, char *out)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	for (i = 0; i < len; i++)
	{
		out[2 * i] = hx[ssid[i] >> 4];
		out[2 * i + 1] = hx[ssid[i] & 15];
	}
	out[2 * len] = 0;
}

/* a saved network's settings from Networks\<hex>; 0 if it is there */
static int saved_network(const uint8_t *ssid, int len, char *key, size_t keycap, int *tkip)
{
	char name[65];
	HANDLE h;
	hex_name(ssid, len, name);
	h = reg_open("Networks", name, 0);
	if (!h)
		return ENOENT;
	reg_get_sz(h, "Key", key, keycap);
	*tkip = reg_get_dword(h, "TKIP", 0) == 1;
	ZwClose(h);
	return 0;
}

static int do_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	int e = ifp->if_ioctl(ifp, cmd, (caddr_t) data);
	return e == ENETRESET ? 0 : e;
}

/* the join list from Networks: every saved network, replacing the old list */
static int load_networks(struct ifnet *ifp)
{
	struct ieee80211_join join;
	struct ifreq ifr;
	HANDLE h;
	ULONG idx;
	int n = 0;
	memset(&join, 0, sizeof(join));
	memset(&ifr, 0, sizeof(ifr));
	join.i_flags = IEEE80211_JOIN_DEL | IEEE80211_JOIN_DEL_ALL;
	ifr.ifr_data = (caddr_t) &join;
	do_ioctl(ifp, SIOCS80211JOIN, &ifr);

	h = reg_open("Networks", NULL, 1);
	if (!h)
		return 0;
	for (idx = 0; idx < 64; idx++)
	{
		union
		{
			KEY_BASIC_INFORMATION i;
			UCHAR b[sizeof(KEY_BASIC_INFORMATION) + 160];
		} u;
		ULONG got = 0, k;
		char name[70], key[80];
		uint8_t ssid[32];
		HANDLE nh;
		ULONG t;
		int slen, tkip;
		if (!NT_SUCCESS(ZwEnumerateKey(h, idx, KeyBasicInformation, &u, sizeof(u), &got)))
			break;
		for (k = 0; k < u.i.NameLength / 2 && k < sizeof(name) - 1; k++)
			name[k] = (char) u.i.Name[k];
		name[k] = 0;
		nh = reg_open("Networks", name, 0);
		if (!nh)
			continue;
		slen = reg_get(nh, "SSID", &t, ssid, sizeof(ssid));
		reg_get_sz(nh, "Key", key, sizeof(key));
		tkip = reg_get_dword(nh, "TKIP", 0) == 1;
		/* "Connect automatically" off: joined only when asked (CONNECT) */
		if (reg_get_dword(nh, "Auto", 1) == 0)
			slen = 0;
		ZwClose(nh);
		if (slen <= 0)
		{
			explicit_bzero(key, sizeof(key));
			continue;
		}
		memset(&join, 0, sizeof(join));
		join.i_len = (uint8_t) slen;
		memcpy(join.i_nwid, ssid, (size_t) slen);
		if (key[0])
		{
			if (kex80211_psk(ssid, (size_t) slen, key, join.i_wpapsk.i_psk))
				continue;
			strlcpy(join.i_wpapsk.i_name, ifp->if_xname, sizeof(join.i_wpapsk.i_name));
			join.i_wpapsk.i_enabled = 1;
			kex80211_wpaparams(&join.i_wpaparams, ifp->if_xname, tkip);
			join.i_flags = IEEE80211_JOIN_WPA | IEEE80211_JOIN_WPAPSK;
		}
		else
			join.i_flags = IEEE80211_JOIN_WPA;   /* i_enabled 0: WPA off, open */
		explicit_bzero(key, sizeof(key));
		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_data = (caddr_t) &join;
		if (do_ioctl(ifp, SIOCS80211JOIN, &ifr) == 0)
			n++;
		explicit_bzero(&join, sizeof(join));
	}
	ZwClose(h);
	return n;
}

/* "join the best saved network": no fixed name, auto-join on */
static void auto_join(struct ifnet *ifp)
{
	struct ieee80211_nwid nwid;
	struct ifreq ifr;
	int n = load_networks(ifp);
	memset(&nwid, 0, sizeof(nwid));
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_data = (caddr_t) &nwid;
	do_ioctl(ifp, SIOCS80211NWID, &ifr);
	printf("%s: %d saved network%s\n", ifp->if_xname, n, n == 1 ? "" : "s");
}

static void write_scan(struct ctl *c, HANDLE h)
{
	struct ieee80211com *ic = (struct ieee80211com *) c->ifp;
	static struct kexwifi_ap list[KEXWIFI_MAX_AP];
	struct ieee80211_node *ni;
	int n = 0, i;
	RBT_FOREACH(ni, ieee80211_tree, &ic->ic_tree)
	{
		struct kexwifi_ap *ap;
		if (!(ni->ni_capinfo & IEEE80211_CAPINFO_ESS) || ni->ni_esslen == 0)
			continue;   /* stations, ad-hoc networks and hidden networks */
		/* one entry per network name: the strongest access point */
		for (i = 0; i < n; i++)
			if (list[i].ssid_len == ni->ni_esslen && !memcmp(list[i].ssid, ni->ni_essid, ni->ni_esslen))
				break;
		if (i < n)
		{
			if (signal_of(ic, ni) <= list[i].signal)
				continue;
		}
		else if (n == KEXWIFI_MAX_AP)
			continue;
		else
			n++;
		ap = &list[i];
		memset(ap, 0, sizeof(*ap));
		ap->ssid_len = ni->ni_esslen;
		memcpy(ap->ssid, ni->ni_essid, ni->ni_esslen);
		memcpy(ap->bssid, ni->ni_bssid, 6);
		ap->channel = (unsigned char) ieee80211_chan2ieee(ic, ni->ni_chan);
		ap->signal = (unsigned char) signal_of(ic, ni);
		ap->security = (unsigned char) security_of(ni);
		ap->saved = (unsigned char) is_saved(ic, ni->ni_essid, ni->ni_esslen);
	}
	reg_set(h, "Scan", REG_BINARY, list, (ULONG) (n * sizeof(list[0])));
	reg_dword(h, "ScanSeq", ++c->scan_seq);
}

static void write_status(struct ctl *c, HANDLE h, int force)
{
	struct ieee80211com *ic = (struct ieee80211com *) c->ifp;
	struct ifnet *ifp = c->ifp;
	ULONG state, channel = 0, bars = 0, sig = 0;
	const uint8_t *ssid = NULL, *bssid = NULL;
	int ssid_len = 0;
	static const uint8_t zero[6];

	if (c->off || !(ifp->if_flags & IFF_UP))
		state = KEXWIFI_OFF;
	else if (ic->ic_state == IEEE80211_S_RUN && ifp->if_link_state == LINK_STATE_UP)
		state = KEXWIFI_CONNECTED;
	else if (ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC ||
	         ic->ic_state == IEEE80211_S_RUN)
		state = KEXWIFI_CONNECTING;
	else
		state = KEXWIFI_SEARCHING;
	if (state >= KEXWIFI_CONNECTING && ic->ic_bss)
	{
		ssid = ic->ic_bss->ni_essid;
		ssid_len = ic->ic_bss->ni_esslen;
		bssid = ic->ic_bss->ni_bssid;
		channel = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
		sig = signal_of(ic, ic->ic_bss);
		bars = (sig + 19) / 20;   /* 0-5 */
	}
	else if (state == KEXWIFI_SEARCHING && ic->ic_des_esslen)
	{
		ssid = ic->ic_des_essid;
		ssid_len = ic->ic_des_esslen;
	}
	if (!force && c->written && state == c->state && channel == c->channel && bars == c->bars &&
	    ssid_len == c->ssid_len && (!ssid_len || !memcmp(ssid, c->ssid, (size_t) ssid_len)) &&
	    !memcmp(bssid ? bssid : zero, c->bssid, 6))
		return;
	if (state != c->state || !c->written)
		printf("%s: Wi-Fi %s%s%.*s%s\n", ifp->if_xname,
		       state == KEXWIFI_OFF ? "off" : state == KEXWIFI_SEARCHING ? "searching" :
		       state == KEXWIFI_CONNECTING ? "connecting" : "connected",
		       ssid_len ? " (\"" : "", ssid_len, ssid ? (const char *) ssid : "", ssid_len ? "\")" : "");
	c->written = 1;
	c->state = state;
	c->channel = channel;
	c->bars = bars;
	c->ssid_len = ssid_len;
	if (ssid_len)
		memcpy(c->ssid, ssid, (size_t) ssid_len);
	memcpy(c->bssid, bssid ? bssid : zero, 6);
	reg_dword(h, "State", state);
	reg_set(h, "SSID", REG_BINARY, ssid_len ? (const void *) ssid : (const void *) zero, (ULONG) ssid_len);
	reg_set(h, "BSSID", REG_BINARY, bssid ? bssid : zero, 6);
	reg_dword(h, "Channel", channel);
	reg_dword(h, "Signal", sig);
}

int kex80211_ctl_start(struct ifnet *ifp, const char *desc, int fixed)
{
	struct ctl *c = ctl_of(ifp, 1);
	HANDLE h;
	if (!c)
		return 0;
	c->fixed = fixed;
	c->off = 0;
	c->written = 0;
	h = adapter_key(ifp);
	if (!h)
	{
		printf("%s: cannot open the Wi-Fi registry key\n", ifp->if_xname);
		return 0;
	}
	reg_sz(h, "Name", desc);
	reg_dword(h, "Present", 1);
	reg_dword(h, "Fixed", (ULONG) fixed);
	reg_dword(h, "Request", 0);
	c->scan_seq = reg_get_dword(h, "ScanSeq", 0);
	if (!fixed)
		auto_join(ifp);
	write_status(c, h, 1);
	ZwClose(h);
	return 1;
}

/* returns KEXNET_WIFI_*: what the glue should do with the interface */
int kex80211_ctl_tick(struct ifnet *ifp)
{
	struct ctl *c = ctl_of(ifp, 0);
	HANDLE h;
	ULONG req;
	int action = KEXNET_WIFI_NONE;
	if (!c)
		return action;
	h = adapter_key(ifp);
	if (!h)
		return action;
	req = reg_get_dword(h, "Request", 0);
	if (req != KEXWIFI_REQ_NONE)
	{
		reg_dword(h, "Request", 0);
		switch (req)
		{
		case KEXWIFI_REQ_RELOAD:
			if (!c->fixed)
			{
				auto_join(ifp);
				action = c->off ? KEXNET_WIFI_NONE : KEXNET_WIFI_RESTART;
			}
			break;
		case KEXWIFI_REQ_CONNECT:
		{
			uint8_t ssid[32];
			char key[80];
			ULONG t;
			int len = reg_get(h, "Target", &t, ssid, sizeof(ssid)), tkip = 0;
			if (len <= 0 || saved_network(ssid, len, key, sizeof(key), &tkip))
			{
				printf("%s: connect: not a saved network\n", ifp->if_xname);
				break;
			}
			/* a fixed name for now, so net80211 takes this one; the
			 * join list stays for later (RELOAD or the next start) */
			load_networks(ifp);
			if (kex80211_configure_bin(ifp, ssid, (size_t) len, key, tkip) == 0)
			{
				c->off = 0;
				action = KEXNET_WIFI_RESTART;
			}
			explicit_bzero(key, sizeof(key));
			break;
		}
		case KEXWIFI_REQ_OFF:
			c->off = 1;
			action = KEXNET_WIFI_DOWN;
			break;
		case KEXWIFI_REQ_ON:
			if (c->off)
			{
				c->off = 0;
				if (!c->fixed)
					auto_join(ifp);
				action = KEXNET_WIFI_UP;
			}
			break;
		case KEXWIFI_REQ_SCAN:
			write_scan(c, h);
			break;
		}
	}
	if (action == KEXNET_WIFI_NONE)
		write_status(c, h, 0);
	ZwClose(h);
	return action;
}

/* after the glue acted: the new state at once */
void kex80211_ctl_after(struct ifnet *ifp)
{
	struct ctl *c = ctl_of(ifp, 0);
	HANDLE h;
	if (!c || !(h = adapter_key(ifp)))
		return;
	write_status(c, h, 0);
	ZwClose(h);
}

void kex80211_ctl_stop(struct ifnet *ifp)
{
	struct ctl *c = ctl_of(ifp, 0);
	HANDLE h;
	if (!c)
		return;
	h = adapter_key(ifp);
	if (h)
	{
		reg_dword(h, "Present", 0);
		reg_dword(h, "State", KEXWIFI_OFF);
		ZwClose(h);
	}
	c->ifp = NULL;
}

const struct kexnet_wifi_ops kex80211_ctl_ops = {
	kex80211_ctl_start,
	kex80211_ctl_tick,
	kex80211_ctl_after,
	kex80211_ctl_stop,
};
