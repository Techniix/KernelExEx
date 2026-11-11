/*
 * KernelExEx kexrtwn: FreeBSD's rtwn(4) Realtek USB Wi-Fi driver, with
 * OpenBSD's net80211 (scanning, association, WPA/WPA2 in the kernel).
 *
 * Two halves that share no headers:
 *   FreeBSD side (FreeBSD personality, compat/kexfbsd.h): rtwn itself, the
 *     FreeBSD USB layer (compat/kexfbsd_usb.c) and the FreeBSD net80211
 *     functions rtwn calls (kexrtwn_fbsd.c, which owns the FreeBSD ic/vap);
 *   OpenBSD side (OpenBSD personality): the 802.11 stack, the NDIS glue
 *     (obsd_net.c) and kexrtwn_obsd.c, which drives the FreeBSD side.
 * This header is the only thing both include: plain C types only.
 *
 * Threads: the OpenBSD side runs on the driver thread (systq). It calls
 * the FreeBSD side from there, at PASSIVE_LEVEL, holding no locks. The
 * FreeBSD side calls back (frames received, transmit done) from its USB
 * callback thread or taskqueue; those calls only queue work for the driver
 * thread.
 *
 * Copyright (c) 2026 KernelExEx contributors. GPLv2 (see COPYING).
 */
#ifndef KEXRTWN_H
#define KEXRTWN_H

#include <stdint.h>
#include <stddef.h>

struct mbuf;   /* the same mbufs on both sides (see kexfbsd.h) */

#define KEXRTWN_MAXCHAN 64
#define KEXRTWN_MAXEP 16

/* what the OpenBSD side hands over at attach */
struct kexrtwn_usbinfo
{
	void *kdev;                /* kexusb_dev * */
	const void *ddesc;         /* device descriptor (18 bytes) */
	const void *cdesc;         /* the selected configuration, whole */
	unsigned int cdesc_len;
	uint8_t config_index;
	uint8_t iface_index;       /* the interface the driver matched */
	int speed;                 /* USB_SPEED_* as in usb.h (FULL = 1, HIGH = 2) */
	int nep;
	struct
	{
		const void *edesc;     /* endpoint descriptor in cdesc */
		void *pipe;            /* USBD pipe handle */
		uint8_t iface_index;
	} ep[KEXRTWN_MAXEP];
	void *worker;              /* the driver thread (kexbsd_worker *) */
};

/* channel flags, ours */
#define KEXRTWN_CHAN_2GHZ 0x0001
#define KEXRTWN_CHAN_5GHZ 0x0002
#define KEXRTWN_CHAN_PASSIVE 0x0004
#define KEXRTWN_CHAN_HT 0x0008

/* what the FreeBSD side reports back */
struct kexrtwn_radio
{
	uint8_t macaddr[6];
	int ntxchains, nrxchains;
	int ht;                    /* 11n */
	int nchan;
	struct
	{
		uint16_t freq;
		uint8_t ieee;
		uint16_t flags;
	} chan[KEXRTWN_MAXCHAN];
	char chipname[32];
};

/* the BSS, from OpenBSD's ic_bss, before AUTH/ASSOC/RUN */
struct kexrtwn_bss
{
	uint8_t macaddr[6];
	uint8_t bssid[6];
	uint16_t freq;
	uint16_t associd;
	uint16_t intval;
	uint16_t capinfo;
	int ht;                    /* HT association */
	uint8_t nrates;
	uint8_t rates[15];         /* 500 kb/s units, 0x80 = basic */
	uint8_t nmcs;              /* MCS 0 .. nmcs-1 usable */
	uint16_t htcap;
	uint8_t ampdu_param;
	int shortpre, shortslot;
};

enum
{
	KEXRTWN_S_INIT,
	KEXRTWN_S_SCAN,
	KEXRTWN_S_AUTH,
	KEXRTWN_S_ASSOC,
	KEXRTWN_S_RUN
};

/* receive details for one frame */
struct kexrtwn_rxinfo
{
	int rssi;                  /* dBm */
	int noise;
	int decrypted;             /* the hardware decrypted it */
	uint64_t tsf;
};

/* ---- FreeBSD side, called by the OpenBSD side (driver thread) -------------- */
int kexrtwn_f_match(uint16_t vid, uint16_t pid, int config_index, int iface_index);
void *kexrtwn_f_attach(const struct kexrtwn_usbinfo *ui, const char *name, struct kexrtwn_radio *out);
void kexrtwn_f_set_osc(void *h, void *osc);
void kexrtwn_f_detach(void *h);
int kexrtwn_f_init(void *h);                         /* power on, firmware, MAC */
void kexrtwn_f_stop(void *h);
int kexrtwn_f_running(void *h);
void kexrtwn_f_set_bss(void *h, const struct kexrtwn_bss *bss);
int kexrtwn_f_newstate(void *h, int nstate);
void kexrtwn_f_scan_start(void *h);
void kexrtwn_f_scan_end(void *h);
void kexrtwn_f_set_channel(void *h, uint16_t freq);
/* an 802.11 frame; tobss: addressed to the BSS node (else broadcast node);
 * eapol: an EAPOL frame (sent at a basic rate, never aggregated);
 * hwkey: the frame is protected and the pairwise key is in hardware */
int kexrtwn_f_transmit(void *h, struct mbuf *m, int tobss, int eapol, int hwkey);
int kexrtwn_f_set_key(void *h, const uint8_t *mac, const uint8_t *key, int keylen, int cipher, int pairwise,
                      int keyid);
void kexrtwn_f_delete_key(void *h, int pairwise, int keyid);
void kexrtwn_f_update_promisc(void *h, int on);
void kexrtwn_f_update_mcast(void *h);
#define KEXRTWN_CIPHER_CCMP 3
#define KEXRTWN_CIPHER_TKIP 2
#define KEXRTWN_CIPHER_WEP 1

/* iteration over the device table, for the INF generator and logs */
int kexrtwn_f_ndevs(void);
void kexrtwn_f_dev(int i, uint16_t *vid, uint16_t *pid);

/* ---- OpenBSD side, called by the FreeBSD side (any thread) ----------------- */
void kexrtwn_o_input(void *osc, struct mbuf *m, const struct kexrtwn_rxinfo *ri);
void kexrtwn_o_tx_done(void *osc, struct mbuf *m, int ok);
void kexrtwn_o_restart(void *osc);                   /* the chip wants a full reset */
int kexrtwn_o_foreach_mcast(void *osc, void (*fn)(void *arg, const uint8_t *mac), void *arg);
uint32_t kexrtwn_o_random(void);
/* mbufs, implemented by the OpenBSD personality (obsd_net.c) */
struct mbuf *kexrtwn_o_m_getjcl(int size);           /* packet header + room for size */
void kexrtwn_o_m_freem(struct mbuf *m);
void kexrtwn_o_m_adj(struct mbuf *m, int len);
void kexrtwn_o_m_copydata(const struct mbuf *m, int off, int len, void *cp);
void kexrtwn_o_m_copyback(struct mbuf *m, int off, int len, const void *cp);
struct mbuf *kexrtwn_o_m_defrag(struct mbuf *m);
struct mbuf *kexrtwn_o_m_prepend(struct mbuf *m, int len);
struct mbuf *kexrtwn_o_m_pullup(struct mbuf *m, int len);
int kexrtwn_o_m_append(struct mbuf *m, int len, const void *cp);
struct mbuf *kexrtwn_o_m_dup(const struct mbuf *m);
void kexrtwn_o_m_cat(struct mbuf *m, struct mbuf *n);

#endif
