/*
 *  KernelExEx - Wi-Fi settings for net80211 drivers: network name and
 *  passphrase from the adapter's Advanced properties.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Security defaults follow the KernelExEx brief: WPA2 (RSN) with CCMP
 *  only. WPA1/TKIP is accepted only when the user turns on "Allow older
 *  WPA (TKIP)"; WEP and open-with-password fallbacks are never used. A
 *  passphrase of 8-63 characters becomes the PSK through PBKDF2-HMAC-SHA1
 *  (4096 rounds, the SSID as salt), as IEEE 802.11i specifies; 64 hex
 *  digits are taken as the PSK itself. An empty passphrase means an open
 *  network, chosen explicitly by the user.
 */
#include "obsd/kexobsd_net.h"
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ioctl.h>
#include "kexnet.h"

void kex80211_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, uint8_t *out, size_t olen);
#define pbkdf2_sha1 kex80211_pbkdf2_sha1

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int kex80211_configure(struct ifnet *ifp, const char *ssid, const char *pass, int allow_tkip)
{
	struct ieee80211_nwid nwid;
	struct ieee80211_wpapsk psk;
	struct ieee80211_wpaparams wpa;
	struct ifreq ifr;
	size_t slen = strlen(ssid), plen = strlen(pass), i;
	int e;

	if (slen == 0)
	{
		printf("%s: no network name set (adapter properties > Advanced)\n", ifp->if_xname);
		return EINVAL;
	}
	if (slen > IEEE80211_NWID_LEN)
		return EINVAL;
	memset(&nwid, 0, sizeof(nwid));
	nwid.i_len = (uint8_t) slen;
	memcpy(nwid.i_nwid, ssid, slen);
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_data = (caddr_t) &nwid;
	e = ifp->if_ioctl(ifp, SIOCS80211NWID, (caddr_t) &ifr);
	if (e && e != ENETRESET)
		return e;

	memset(&psk, 0, sizeof(psk));
	memset(&wpa, 0, sizeof(wpa));
	strlcpy(psk.i_name, ifp->if_xname, sizeof(psk.i_name));
	strlcpy(wpa.i_name, ifp->if_xname, sizeof(wpa.i_name));
	if (plen == 64)
	{
		for (i = 0; i < 32; i++)
		{
			int hi = hexval(pass[2 * i]), lo = hexval(pass[2 * i + 1]);
			if (hi < 0 || lo < 0)
				return EINVAL;
			psk.i_psk[i] = (uint8_t) (hi << 4 | lo);
		}
	}
	else if (plen >= 8 && plen <= 63)
		pbkdf2_sha1(pass, plen, (const uint8_t *) ssid, slen, psk.i_psk, sizeof(psk.i_psk));
	else if (plen)
	{
		printf("%s: a WPA passphrase has 8 to 63 characters\n", ifp->if_xname);
		return EINVAL;
	}

	if (plen)
	{
		psk.i_enabled = 1;
		e = ifp->if_ioctl(ifp, SIOCS80211WPAPSK, (caddr_t) &psk);
		explicit_bzero(&psk, sizeof(psk));
		if (e && e != ENETRESET)
			return e;
		wpa.i_enabled = 1;
		wpa.i_protos = IEEE80211_WPA_PROTO_WPA2 | (allow_tkip ? IEEE80211_WPA_PROTO_WPA1 : 0);
		wpa.i_akms = IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_SHA256_PSK;
		wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP | (allow_tkip ? IEEE80211_WPA_CIPHER_TKIP : 0);
		wpa.i_groupcipher = allow_tkip ? IEEE80211_WPA_CIPHER_TKIP : IEEE80211_WPA_CIPHER_CCMP;
		e = ifp->if_ioctl(ifp, SIOCS80211WPAPARMS, (caddr_t) &wpa);
		if (e && e != ENETRESET)
			return e;
		printf("%s: network \"%s\", WPA2%s\n", ifp->if_xname, ssid, allow_tkip ? " (WPA/TKIP allowed)" : "");
	}
	else
		printf("%s: network \"%s\", open (no encryption)\n", ifp->if_xname, ssid);
	return 0;
}
