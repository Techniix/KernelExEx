/*
 *  KernelExEx - kexhda.sys: Intel High Definition Audio for Windows 98 SE and ME,
 *  on Intel ICH6 and later (5 Series / "Ibex Peak" in the Dell Vostro 1440), and
 *  compatible HD Audio controllers, with any HD Audio codec (IDT/SigmaTel 92HDxx,
 *  Realtek ALC2xx/8xx, Conexant, Analog Devices ...).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Our own driver, from the Intel High Definition Audio Specification 1.0a.
 *  The codec's widget graph is walked: every analog speaker, headphone and line
 *  out pin is routed to a DAC; all of them play our one stream. Headphones in
 *  silence the speaker (pin sense, polled). PortCls side as in kexhda.c: a
 *  WaveCyclic miniport (48 kHz 16-bit stereo; KMixer converts the rest) and a
 *  Topology miniport (master volume and mute).
 */
#include <ntddk.h>
typedef char TCHAR;
#include <portcls.h>
#include <ksmedia.h>
#include "../../kedcl/kexos.h"

/* messages (KEDCL log ring): QEMU's debug console in test builds, and saved to the
 * registry (HKLM\Software\KernelEx\Diag, "kexhda") for the hardware report */
static struct kexos_log hlog = { "", 0, "kexhda" };
#define DPRINT(...) kexos_log(&hlog, __VA_ARGS__)

/* ---- GUIDs, defined here (the headers' DEFINE_GUIDSTRUCT machinery is C++) ------------- */
#define KGUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
KGUID(G_IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
KGUID(G_IID_IMiniport, 0xb4c90a24, 0x5791, 0x11d0, 0x86, 0xf9, 0x00, 0xa0, 0xc9, 0x11, 0xb5, 0x44);
KGUID(G_IID_IMiniportWaveCyclic, 0xb4c90a27, 0x5791, 0x11d0, 0x86, 0xf9, 0x00, 0xa0, 0xc9, 0x11, 0xb5, 0x44);
KGUID(G_IID_IMiniportWaveCyclicStream, 0xb4c90a28, 0x5791, 0x11d0, 0x86, 0xf9, 0x00, 0xa0, 0xc9, 0x11, 0xb5, 0x44);
KGUID(G_IID_IMiniportTopology, 0xb4c90a31, 0x5791, 0x11d0, 0x86, 0xf9, 0x00, 0xa0, 0xc9, 0x11, 0xb5, 0x44);
KGUID(G_CLSID_PortWaveCyclic, 0xb4c90a2a, 0x5791, 0x11d0, 0x86, 0xf9, 0x00, 0xa0, 0xc9, 0x11, 0xb5, 0x44);
KGUID(G_CLSID_PortTopology, 0xb4c90a32, 0x5791, 0x11d0, 0x86, 0xf9, 0x00, 0xa0, 0xc9, 0x11, 0xb5, 0x44);
KGUID(G_KSDATAFORMAT_TYPE_AUDIO, 0x73647561, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
KGUID(G_KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
KGUID(G_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX, 0x05589f81, 0xc356, 0x11ce, 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59,
      0x5a);
KGUID(G_KSCATEGORY_AUDIO, 0x6994ad04, 0x93ef, 0x11d0, 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
KGUID(G_KSCATEGORY_RENDER, 0x65e8773e, 0x8f56, 0x11d0, 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
KGUID(G_KSCATEGORY_TOPOLOGY, 0xdda54a40, 0x1e4c, 0x11d1, 0xa0, 0x50, 0x40, 0x57, 0x05, 0xc1, 0x00, 0x00);
KGUID(G_KSNODETYPE_DAC, 0x507ae360, 0xc554, 0x11d0, 0x8a, 0x2b, 0x00, 0xa0, 0xc9, 0x25, 0x5a, 0xc1);
KGUID(G_KSNODETYPE_VOLUME, 0x3a5acc00, 0xc557, 0x11d0, 0x8a, 0x2b, 0x00, 0xa0, 0xc9, 0x25, 0x5a, 0xc1);
KGUID(G_KSNODETYPE_MUTE, 0x02b223c0, 0xc557, 0x11d0, 0x8a, 0x2b, 0x00, 0xa0, 0xc9, 0x25, 0x5a, 0xc1);
KGUID(G_KSNODETYPE_SPEAKER, 0xdff21ce1, 0xf70f, 0x11d0, 0xb9, 0x17, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
KGUID(G_KSPROPSETID_Audio, 0x45ffaaa0, 0x6e1b, 0x11d0, 0xbc, 0xf2, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
KGUID(G_KSPROPTYPESETID_General, 0x97e99ba0, 0xbdea, 0x11cf, 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00);
KGUID(G_KSAUDFNAME_MASTER_VOLUME, 0x185fedf8, 0x9905, 0x11d1, 0x95, 0xa9, 0x00, 0xc0, 0x4f, 0xb9, 0x25, 0xd3);
KGUID(G_KSAUDFNAME_MASTER_MUTE, 0x185fedf9, 0x9905, 0x11d1, 0x95, 0xa9, 0x00, 0xc0, 0x4f, 0xb9, 0x25, 0xd3);


static int guid_eq(REFIID a, const GUID *b)
{
	return RtlCompareMemory(a, b, sizeof(GUID)) == sizeof(GUID);
}

/* ---- the controller (Intel High Definition Audio spec 1.0a) -------------------------------- */
#define GCAP 0x00
#define GCTL 0x08
#define STATESTS 0x0e
#define INTCTL 0x20
#define INTSTS 0x24
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4a
#define CORBCTL 0x4c
#define CORBSIZE 0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5a
#define RIRBCTL 0x5c
#define RIRBSTS 0x5d
#define RIRBSIZE 0x5e
#define IC 0x60
#define IR 0x64
#define ICS 0x68
#define SD(n) (0x80 + 0x20 * (n))
#define SD_CTL 0x00
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1c
#define CTL_SRST 0x01
#define CTL_RUN 0x02
#define CTL_IOCE 0x04
#define CTL_FEIE 0x08
#define CTL_DEIE 0x10
#define STS_CLEAR 0x1c          /* BCIS, FIFOE, DESE */
#define STREAM_TAG 1
#define FMT_48K_16_2 0x0011     /* 48 kHz, 16 bits, 2 channels */

/* codec verbs */
#define V_GET_PARAM 0xf00
#define V_GET_CONN_SEL 0xf01
#define V_GET_CONN 0xf02
#define V_SET_CONN_SEL 0x701
#define V_SET_POWER 0x705
#define V_SET_STREAM 0x706
#define V_SET_PINCTL 0x707
#define V_GET_SENSE 0xf09
#define V_SET_EAPD 0x70c
#define V_GET_EAPD 0xf0c
#define V_GET_CONFIG 0xf1c
#define V_RESET 0x7ff
#define V4_SET_FMT 0x2
#define V4_SET_AMP 0x3
#define P_VENDOR 0x00
#define P_NODES 0x04
#define P_FGTYPE 0x05
#define P_WCAPS 0x09
#define P_PINCAPS 0x0c
#define P_INAMP 0x0d
#define P_CONNLEN 0x0e
#define P_OUTAMP 0x12
#define W_OUT 0
#define W_IN 1
#define W_MIXER 2
#define W_SELECTOR 3
#define W_PIN 4
#define DEV_LINEOUT 0
#define DEV_SPEAKER 1
#define DEV_HP 2

#define KSPROPERTY_TYPE_ALL (KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT)
#define NBDL 32
#define BUF_SIZE 0x8000   /* 32K cyclic buffer: 170 ms at 48 kHz, 1K per descriptor */
#define CHUNK (BUF_SIZE / NBDL)
#define RATE 48000
#define MAXNODE 0x80
#define MAXOUT 4         /* output paths driven: speaker, headphones, line outs */

struct bdl
{
	ULONG addr_lo, addr_hi;
	ULONG len;
	ULONG ioc;   /* bit 0: interrupt on completion */
};

/* one output path: pin <- ... <- DAC */
struct opath
{
	UCHAR pin, dac, dev;
	UCHAR amp_nid;      /* where the volume is set (DAC out amp, else pin out amp); 0: none */
	ULONG ampcaps;
	UCHAR hp_muted;     /* speaker / line out silenced because headphones are in */
};

typedef struct adapter
{
	volatile void *mmio;
	ULONG mmio_len;
	KSPIN_LOCK codec_lock;
	volatile ULONG *corb, *rirb;   /* command rings (NULL: immediate command interface) */
	int corb_n, rirb_n, rirb_rp;
	int caddr;          /* the codec we drive */
	ULONG vendor;
	int out_sd;         /* stream descriptor index of the first output stream */
	int npath;
	struct opath path[MAXOUT];
	ULONG wcaps[MAXNODE];
	LONG vol[2];        /* master volume, 1/65536 dB, per channel */
	int muted;
	int ready;
	PINTERRUPTSYNC isync;
	struct wave *wave;
} ADAPTER;

static ADAPTER g_ad;

/* plain volatile access (as kexehci.c): Windows 98's NTKERN has no READ_REGISTER_* */
#define R8(o) kexos_rd8(ad->mmio, (o))
#define R16(o) kexos_rd16(ad->mmio, (o))
#define R32(o) kexos_rd32(ad->mmio, (o))
#define W8(o, v) kexos_wr8(ad->mmio, (o), (kx_u8) (v))
#define W16(o, v) kexos_wr16(ad->mmio, (o), (kx_u16) (v))
#define W32(o, v) kexos_wr32(ad->mmio, (o), (kx_u32) (v))

/* ring sizes from the size register's capability bits: 256, else 16, else 2 entries */
static int ring_size(UCHAR caps, UCHAR *code)
{
	/* no capability bit at all (QEMU) breaks the spec: such controllers use 256 */
	if ((caps & 0x40) || !(caps & 0x70)) { *code = 2; return 256; }
	if (caps & 0x20) { *code = 1; return 16; }
	*code = 0;
	return 2;
}

/* CORB/RIRB (the usual way; polled, no RIRB interrupt) */
static int rings_init(ADAPTER *ad)
{
	kx_u32 pa;
	UCHAR code;
	int i;
	ad->corb = (volatile ULONG *) kexos_dma_alloc(4096, &pa);
	if (!ad->corb)
		return 0;
	ad->rirb = ad->corb + 256;   /* CORB 1K, RIRB 2K, one page */
	W8(CORBCTL, 0);
	W8(RIRBCTL, 0);
	for (i = 0; i < 1000 && ((R8(CORBCTL) & 2) || (R8(RIRBCTL) & 2)); i++)
		kexos_stall_us(10);
	ad->corb_n = ring_size(R8(CORBSIZE) >> 4, &code);
	W8(CORBSIZE, (R8(CORBSIZE) & ~3) | code);
	W32(CORBLBASE, pa);
	W32(CORBUBASE, 0);
	/* read pointer reset: set bit 15, see it, clear it (some controllers never show it: go on) */
	W16(CORBRP, 0x8000);
	for (i = 0; i < 1000 && !(R16(CORBRP) & 0x8000); i++)
		kexos_stall_us(1);
	W16(CORBRP, 0);
	for (i = 0; i < 1000 && (R16(CORBRP) & 0x8000); i++)
		kexos_stall_us(1);
	W16(CORBWP, 0);
	ad->rirb_n = ring_size(R8(RIRBSIZE) >> 4, &code);
	W8(RIRBSIZE, (R8(RIRBSIZE) & ~3) | code);
	W32(RIRBLBASE, pa + 1024);
	W32(RIRBUBASE, 0);
	W16(RIRBWP, 0x8000);
	W16(RINTCNT, 0xff);
	W8(RIRBSTS, 0x05);
	ad->rirb_rp = 0;
	W8(CORBCTL, 2);
	W8(RIRBCTL, 2);
	DPRINT("kexhda: CORB %d / RIRB %d entries at %08lx\n", ad->corb_n, ad->rirb_n, pa);
	return 1;
}

static ULONG cmd_raw(ADAPTER *ad, ULONG c, int *ok)
{
	KIRQL irql;
	ULONG r = 0xffffffff;
	int i, done = 0;
	KeAcquireSpinLock(&ad->codec_lock, &irql);
	if (ad->corb)
	{
		int wp = (R16(CORBWP) + 1) % ad->corb_n;
		ad->corb[wp] = c;
		W16(CORBWP, wp);
		for (i = 0; i < 10000 && !done; i++)
		{
			while ((R8(RIRBWP) % ad->rirb_n) != ad->rirb_rp)
			{
				ad->rirb_rp = (ad->rirb_rp + 1) % ad->rirb_n;
				if (ad->rirb[ad->rirb_rp * 2 + 1] & 0x10)   /* unsolicited: not our answer */
					continue;
				r = ad->rirb[ad->rirb_rp * 2];
				done = 1;
				break;
			}
			if (!done)
				kexos_stall_us(1);
		}
		W8(RIRBSTS, 0x05);
	}
	else
	{
		/* the Immediate Command interface */
		for (i = 0; i < 1000 && (R16(ICS) & 1); i++)
			kexos_stall_us(1);
		W16(ICS, 2);              /* clear IRV */
		W32(IC, c);
		W16(ICS, 1);              /* ICB: send */
		for (i = 0; i < 1000; i++)
		{
			if ((R16(ICS) & 3) == 2)
			{
				r = R32(IR);
				done = 1;
				break;
			}
			kexos_stall_us(1);
		}
	}
	KeReleaseSpinLock(&ad->codec_lock, irql);
	if (ok)
		*ok = done;
	return r;
}

static ULONG verb(ADAPTER *ad, int nid, ULONG v12, ULONG payload)
{
	return cmd_raw(ad, ((ULONG) ad->caddr << 28) | ((ULONG) nid << 20) | (v12 << 8) | (payload & 0xff), NULL);
}

static ULONG verb4(ADAPTER *ad, int nid, ULONG v4, ULONG payload)
{
	return cmd_raw(ad, ((ULONG) ad->caddr << 28) | ((ULONG) nid << 20) | (v4 << 16) | (payload & 0xffff), NULL);
}

#define PARAM(nid, p) verb(ad, nid, V_GET_PARAM, p)
#define WTYPE(nid) ((ad->wcaps[nid] >> 20) & 0xf)

/* connection list entry i of nid (short form; long form lists are rare and not followed) */
static int conn_count(ADAPTER *ad, int nid)
{
	ULONG l;
	if (!(ad->wcaps[nid] & (1u << 8)))
		return 0;
	l = PARAM(nid, P_CONNLEN);
	if (l & 0x80)
		return 0;
	return (int) (l & 0x7f);
}

static int conn_get(ADAPTER *ad, int nid, int i)
{
	ULONG r = verb(ad, nid, V_GET_CONN, (ULONG) (i & ~3));
	int e = (int) ((r >> (8 * (i & 3))) & 0xff);
	return e & 0x7f;   /* bit 7 (a range) is not followed */
}

static ULONG ampcaps(ADAPTER *ad, int nid, int out)
{
	/* without "amp parameter override" the function group's caps apply */
	ULONG c = (ad->wcaps[nid] & (1u << 3)) ? PARAM(nid, out ? P_OUTAMP : P_INAMP) : 0;
	if (!c)
		c = PARAM(1, out ? P_OUTAMP : P_INAMP);
	return c;
}

/* unmute an amp at 0 dB (its offset); index = input index for input amps */
static void amp_unmute(ADAPTER *ad, int nid, int out, int index)
{
	ULONG caps = ampcaps(ad, nid, out);
	ULONG g = caps & 0x7f;
	verb4(ad, nid, V4_SET_AMP, (out ? 0x8000 : 0x4000) | 0x3000 | ((ULONG) index << 8) | g);
}

/* from a pin, find a DAC (depth-first, at most 5 hops); sets selectors and unmutes on the way */
static int find_dac(ADAPTER *ad, int nid, int depth, int *dac)
{
	int n, i;
	if (depth > 5 || nid <= 0 || nid >= MAXNODE)
		return 0;
	if (WTYPE(nid) == W_OUT)
	{
		*dac = nid;
		return 1;
	}
	if (WTYPE(nid) != W_PIN && WTYPE(nid) != W_MIXER && WTYPE(nid) != W_SELECTOR && WTYPE(nid) != 0xf)
		return 0;
	n = conn_count(ad, nid);
	for (i = 0; i < n; i++)
	{
		int c = conn_get(ad, nid, i);
		if (c == nid || !find_dac(ad, c, depth + 1, dac))
			continue;
		if (n > 1 && WTYPE(nid) != W_MIXER)
			verb(ad, nid, V_SET_CONN_SEL, (ULONG) i);
		if (ad->wcaps[nid] & 2)   /* input amp: mixers have one per input */
			amp_unmute(ad, nid, 0, WTYPE(nid) == W_MIXER ? i : 0);
		if ((ad->wcaps[nid] & 4) && WTYPE(nid) != W_PIN)
			amp_unmute(ad, nid, 1, 0);
		return 1;
	}
	return 0;
}

/* the master volume onto every path's amp: 0 dB at the top, 1.5 dB steps down */
static void apply_volume(ADAPTER *ad)
{
	int p, ch;
	for (p = 0; p < ad->npath; p++)
	{
		struct opath *o = &ad->path[p];
		ULONG caps = o->ampcaps, off = caps & 0x7f, steps = (caps >> 8) & 0x7f;
		LONG stepdb = (LONG) (((caps >> 16) & 0x7f) + 1) * 65536 / 4;   /* 0.25 dB units */
		if (!o->amp_nid)
			continue;
		for (ch = 0; ch < 2; ch++)
		{
			LONG g = (LONG) off + (stepdb ? ad->vol[ch] / stepdb : 0);
			ULONG mute = (ad->muted || o->hp_muted || g <= 0) && (caps & 0x80000000u) ? 0x80 : 0;
			if (g < 0)
				g = 0;
			if (g > (LONG) steps)
				g = (LONG) steps;
			verb4(ad, o->amp_nid, V4_SET_AMP, 0x8000 | (ch ? 0x1000 : 0x2000) | mute | (ULONG) g);
		}
		/* without a mute bit, silence a muted path at the pin */
		if (!(caps & 0x80000000u) && (ad->muted || o->hp_muted))
			verb(ad, o->pin, V_SET_PINCTL, 0);
	}
}

static void pins_on(ADAPTER *ad)
{
	int p;
	for (p = 0; p < ad->npath; p++)
	{
		struct opath *o = &ad->path[p];
		ULONG pc = PARAM(o->pin, P_PINCAPS);
		verb(ad, o->pin, V_SET_PINCTL, o->hp_muted ? 0 : (o->dev == DEV_HP && (pc & (1u << 3)) ? 0xc0 : 0x40));
		if (ad->wcaps[o->pin] & 4)
			amp_unmute(ad, o->pin, 1, 0);
		if (pc & (1u << 16))   /* EAPD: the external amplifier on */
			verb(ad, o->pin, V_SET_EAPD, verb(ad, o->pin, V_GET_EAPD, 0) | 2);
	}
}

/* headphones in: speaker and line out off (polled twice a second; works without unsolicited events) */
static void hp_poll(ADAPTER *ad)
{
	int p, hp = 0, changed = 0;
	for (p = 0; p < ad->npath; p++)
		if (ad->path[p].dev == DEV_HP && (PARAM(ad->path[p].pin, P_PINCAPS) & (1u << 2)) &&
		    (verb(ad, ad->path[p].pin, V_GET_SENSE, 0) & 0x80000000u))
			hp = 1;
	for (p = 0; p < ad->npath; p++)
		if (ad->path[p].dev != DEV_HP && ad->path[p].hp_muted != hp)
		{
			ad->path[p].hp_muted = (UCHAR) hp;
			changed = 1;
		}
	if (changed)
	{
		DPRINT("kexhda: headphones %s\n", hp ? "in" : "out");
		pins_on(ad);
		apply_volume(ad);
	}
}

/* headphone sense, twice a second (KEDCL one-shot timer, re-armed) */
static void hp_tick(void *ref)
{
	ADAPTER *ad = (ADAPTER *) ref;
	hp_poll(ad);
	kexos_timer(500, hp_tick, ad);
}

/* walk one codec: its audio function group, widgets and output paths */
static int codec_probe(ADAPTER *ad, int addr)
{
	ULONG sub, afg = 0, n;
	int i, start, count, dev_order;
	ad->caddr = addr;
	ad->vendor = PARAM(0, P_VENDOR);
	sub = PARAM(0, P_NODES);
	start = (int) ((sub >> 16) & 0xff);
	count = (int) (sub & 0xff);
	for (i = start; i < start + count; i++)
		if ((PARAM(i, P_FGTYPE) & 0xff) == 1)
		{
			afg = (ULONG) i;
			break;
		}
	DPRINT("kexhda: codec %d vendor %08lx, AFG %lu\n", addr, ad->vendor, afg);
	if (!afg)
		return 0;
	verb(ad, (int) afg, V_RESET, 0);
	kexos_stall_us(1000);
	verb(ad, (int) afg, V_SET_POWER, 0);
	kexos_stall_us(10000);
	n = PARAM((int) afg, P_NODES);
	start = (int) ((n >> 16) & 0xff);
	count = (int) (n & 0xff);
	RtlZeroMemory(ad->wcaps, sizeof(ad->wcaps));
	for (i = start; i < start + count && i < MAXNODE; i++)
	{
		ad->wcaps[i] = PARAM(i, P_WCAPS);
		if (ad->wcaps[i] & (1u << 10))
			verb(ad, i, V_SET_POWER, 0);
	}
	/* speaker first, then headphones, then line outs */
	ad->npath = 0;
	for (dev_order = 0; dev_order < 3; dev_order++)
	{
		int want = dev_order == 0 ? DEV_SPEAKER : dev_order == 1 ? DEV_HP : DEV_LINEOUT;
		for (i = start; i < start + count && i < MAXNODE && ad->npath < MAXOUT; i++)
		{
			ULONG cfg;
			int dac = 0;
			struct opath *o;
			if (WTYPE(i) != W_PIN || (ad->wcaps[i] & (1u << 9)))   /* analog pins only */
				continue;
			cfg = verb(ad, i, V_GET_CONFIG, 0);
			if ((cfg >> 30) == 1 || (int) ((cfg >> 20) & 0xf) != want)   /* not connected / other device */
				continue;
			if (!(PARAM(i, P_PINCAPS) & (1u << 4)))   /* no output */
				continue;
			if (!find_dac(ad, i, 0, &dac))
				continue;
			o = &ad->path[ad->npath++];
			o->pin = (UCHAR) i;
			o->dac = (UCHAR) dac;
			o->dev = (UCHAR) want;
			o->amp_nid = (ad->wcaps[dac] & 4) ? (UCHAR) dac : (ad->wcaps[i] & 4) ? (UCHAR) i : 0;
			o->ampcaps = o->amp_nid ? ampcaps(ad, o->amp_nid, 1) : 0;
			DPRINT("kexhda: %s pin %02x (config %08lx) <- DAC %02x, volume on %02x\n",
			       want == DEV_SPEAKER ? "speaker" : want == DEV_HP ? "headphones" : "line out", i, cfg, dac,
			       o->amp_nid);
		}
	}
	return ad->npath > 0;
}

static NTSTATUS hw_init(ADAPTER *ad)
{
	USHORT codecs;
	int i;
	/* controller reset: CRST 0, then 1, then give codecs time to announce themselves */
	W32(INTCTL, 0);
	W8(CORBCTL, 0);
	W8(RIRBCTL, 0);
	for (i = 0; i < 1000 && ((R8(CORBCTL) & 2) || (R8(RIRBCTL) & 2)); i++)
		kexos_stall_us(10);
	W32(GCTL, R32(GCTL) & ~1u);
	for (i = 0; i < 100 && (R32(GCTL) & 1); i++)
		kexos_stall_us(100);
	kexos_stall_us(100);
	W32(GCTL, R32(GCTL) | 1);
	for (i = 0; i < 100 && !(R32(GCTL) & 1); i++)
		kexos_stall_us(100);
	if (!(R32(GCTL) & 1))
	{
		DPRINT("kexhda: controller does not leave reset\n");
		return STATUS_DEVICE_NOT_READY;
	}
	kexos_stall_us(1000);   /* the spec asks for at least 521 us */
	codecs = R16(STATESTS);
	W16(STATESTS, codecs);
	ad->out_sd = (R16(GCAP) >> 8) & 0xf;
	if (!ad->corb && !rings_init(ad))
		DPRINT("kexhda: no memory for the command rings, using immediate commands\n");   /* output streams follow the input streams */
	DPRINT("kexhda: GCAP %04x, codecs %04x\n", R16(GCAP), codecs);
	for (i = 0; i < 15; i++)
		if ((codecs & (1u << i)) && codec_probe(ad, i))
			break;
	if (i == 15)
	{
		DPRINT("kexhda: no codec with an analog output\n");
		return STATUS_DEVICE_NOT_READY;
	}
	ad->vol[0] = ad->vol[1] = 0;
	pins_on(ad);
	apply_volume(ad);
	/* every path's DAC on our stream */
	for (i = 0; i < ad->npath; i++)
	{
		verb4(ad, ad->path[i].dac, V4_SET_FMT, FMT_48K_16_2);
		verb(ad, ad->path[i].dac, V_SET_STREAM, STREAM_TAG << 4);
	}
	/* output stream 0: stopped and reset */
	W8(SD(ad->out_sd) + SD_CTL, 0);
	W8(SD(ad->out_sd) + SD_STS, STS_CLEAR);
	ad->ready = 1;
	hp_poll(ad);
	return STATUS_SUCCESS;
}

static void sd_reset(ADAPTER *ad)
{
	int o = SD(ad->out_sd), i;
	W8(o + SD_CTL, R8(o + SD_CTL) & ~CTL_RUN);
	for (i = 0; i < 1000 && (R8(o + SD_CTL) & CTL_RUN); i++)
		kexos_stall_us(10);
	W8(o + SD_CTL, CTL_SRST);
	for (i = 0; i < 1000 && !(R8(o + SD_CTL) & CTL_SRST); i++)
		kexos_stall_us(10);
	W8(o + SD_CTL, 0);
	for (i = 0; i < 1000 && (R8(o + SD_CTL) & CTL_SRST); i++)
		kexos_stall_us(10);
	W8(o + SD_STS, STS_CLEAR);
}

/* ---- common COM plumbing ---------------------------------------------------------------- */
#define THISOF(type, p) ((type *) (p))

/* ===================================================================================== *
 * wave: IMiniportWaveCyclic and its one stream                                         *
 * ===================================================================================== */
typedef struct stream
{
	IMiniportWaveCyclicStreamVtbl *lpVtbl;
	LONG ref;
	struct wave *wave;
	PDMACHANNEL dma;
	PSERVICEGROUP sg;
	struct bdl *bdl;            /* in the DMA buffer's tail */
	PHYSICAL_ADDRESS bdl_pa;
	PUCHAR buf;
	PHYSICAL_ADDRESS buf_pa;
	KSSTATE state;
	ULONG bufsize;
} STREAM;

typedef struct wave
{
	IMiniportWaveCyclicVtbl *lpVtbl;
	LONG ref;
	ADAPTER *ad;
	PPORTWAVECYCLIC port;
	STREAM *stream;
} WAVE;

/* -- stream ------------------------------------------------------------------------------- */
static NTSTATUS STDMETHODCALLTYPE st_QueryInterface(IMiniportWaveCyclicStream *This, REFIID iid, PVOID *out)
{
	if (guid_eq(iid, &G_IID_IUnknown) || guid_eq(iid, &G_IID_IMiniportWaveCyclicStream))
	{
		*out = This;
		InterlockedIncrement(&THISOF(STREAM, This)->ref);
		return STATUS_SUCCESS;
	}
	*out = NULL;
	return STATUS_INVALID_PARAMETER;
}

static ULONG STDMETHODCALLTYPE st_AddRef(IMiniportWaveCyclicStream *This)
{
	return InterlockedIncrement(&THISOF(STREAM, This)->ref);
}

static void stream_stop_hw(STREAM *s)
{
	sd_reset(s->wave->ad);
}

static ULONG STDMETHODCALLTYPE st_Release(IMiniportWaveCyclicStream *This)
{
	STREAM *s = THISOF(STREAM, This);
	LONG r = InterlockedDecrement(&s->ref);
	if (r == 0)
	{
		stream_stop_hw(s);
		if (s->wave->stream == s)
			s->wave->stream = NULL;
		if (s->sg)
			s->sg->lpVtbl->Release(s->sg);
		if (s->dma)
		{
			s->dma->lpVtbl->FreeBuffer(s->dma);
			s->dma->lpVtbl->Release(s->dma);
		}
		ExFreePool(s);
	}
	return (ULONG) r;
}

static int format_ok(PKSDATAFORMAT f)
{
	PWAVEFORMATEX wf;
	if (f->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX))
		return 0;
	if (!guid_eq(&f->MajorFormat, &G_KSDATAFORMAT_TYPE_AUDIO) || !guid_eq(&f->SubFormat, &G_KSDATAFORMAT_SUBTYPE_PCM) ||
	    !guid_eq(&f->Specifier, &G_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
		return 0;
	wf = &((PKSDATAFORMAT_WAVEFORMATEX) f)->WaveFormatEx;
	return wf->nChannels == 2 && wf->wBitsPerSample == 16 && wf->nSamplesPerSec == RATE;
}

static NTSTATUS STDMETHODCALLTYPE st_SetFormat(IMiniportWaveCyclicStream *This, PKSDATAFORMAT f)
{
	(void) This;
	return format_ok(f) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static ULONG STDMETHODCALLTYPE st_SetNotificationFreq(IMiniportWaveCyclicStream *This, ULONG interval, PULONG frame)
{
	(void) This;
	(void) interval;
	/* we interrupt once per descriptor: CHUNK bytes (about 5.3 ms) */
	*frame = CHUNK;
	return CHUNK * 1000 / (RATE * 4) ? CHUNK * 1000 / (RATE * 4) : 1;
}

static void bdl_fill(STREAM *s)
{
	int i;
	for (i = 0; i < NBDL; i++)
	{
		s->bdl[i].addr_lo = s->buf_pa.LowPart + (ULONG) i * CHUNK;
		s->bdl[i].addr_hi = 0;
		s->bdl[i].len = CHUNK;
		s->bdl[i].ioc = 1;
	}
}

static NTSTATUS STDMETHODCALLTYPE st_SetState(IMiniportWaveCyclicStream *This, KSSTATE state)
{
	STREAM *s = THISOF(STREAM, This);
	ADAPTER *ad = s->wave->ad;
	int o = SD(ad->out_sd);
	if (state == s->state)
		return STATUS_SUCCESS;
	switch (state)
	{
	case KSSTATE_RUN:
		if (s->state != KSSTATE_PAUSE)
			break;
		if (!(R8(o + SD_CTL) & CTL_RUN) && R32(o + SD_LPIB) == 0)
		{
			/* fresh start (after a reset): program the descriptor */
			W32(o + SD_BDPL, s->bdl_pa.LowPart);
			W32(o + SD_BDPU, 0);
			W32(o + SD_CBL, BUF_SIZE);
			W16(o + SD_LVI, NBDL - 1);
			W16(o + SD_FMT, FMT_48K_16_2);
			W8(o + SD_CTL + 2, STREAM_TAG << 4);
		}
		W8(o + SD_STS, STS_CLEAR);
		W32(INTCTL, R32(INTCTL) | 0x80000000u | (1u << ad->out_sd));
		W8(o + SD_CTL, CTL_RUN | CTL_IOCE | CTL_FEIE | CTL_DEIE);
		break;
	case KSSTATE_PAUSE:
		if (s->state == KSSTATE_RUN)
			W8(o + SD_CTL, R8(o + SD_CTL) & ~CTL_RUN);
		break;
	case KSSTATE_STOP:
	case KSSTATE_ACQUIRE:
		if (s->state == KSSTATE_RUN || s->state == KSSTATE_PAUSE)
			sd_reset(ad);   /* back to position 0, as the port expects */
		break;
	}
	s->state = state;
	return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE st_GetPosition(IMiniportWaveCyclicStream *This, PULONG pos)
{
	STREAM *s = THISOF(STREAM, This);
	ADAPTER *ad = s->wave->ad;
	*pos = R32(SD(ad->out_sd) + SD_LPIB) % BUF_SIZE;
	return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE st_NormalizePhysicalPosition(IMiniportWaveCyclicStream *This, PLONGLONG p)
{
	(void) This;
	/* bytes -> 100 ns units */
	*p = (*p * 10000000) / (RATE * 4);
	return STATUS_SUCCESS;
}

static void STDMETHODCALLTYPE st_Silence(IMiniportWaveCyclicStream *This, PVOID buf, ULONG n)
{
	(void) This;
	RtlZeroMemory(buf, n);
}

static IMiniportWaveCyclicStreamVtbl stream_vtbl = {
	st_QueryInterface, st_AddRef, st_Release, st_SetFormat, st_SetNotificationFreq,
	st_SetState, st_GetPosition, st_NormalizePhysicalPosition, st_Silence,
};

/* -- the wave filter's description ------------------------------------------------------------ */
static KSDATARANGE_AUDIO pcm_range = {
	{ { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
	  { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } },   /* TYPE_AUDIO */
	  { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } },   /* SUBTYPE_PCM */
	  { 0x05589f81, 0xc356, 0x11ce, { 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } } } }, /* WAVEFORMATEX */
	2, 16, 16, RATE, RATE
};
static PKSDATARANGE pcm_ranges[] = { (PKSDATARANGE) &pcm_range };

static KSDATARANGE analog_range = { {
	sizeof(KSDATARANGE), 0, 0, 0,
	{ 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } },
	{ 0x6dba3190, 0x67bd, 0x11cf, { 0xa0, 0xf7, 0x00, 0x20, 0xaf, 0xd1, 0x56, 0xe4 } },   /* SUBTYPE_ANALOG */
	{ 0x0f6417d6, 0xc318, 0x11d0, { 0xa4, 0x3f, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } }    /* SPECIFIER_NONE */
} };
static PKSDATARANGE analog_ranges[] = { &analog_range };

enum
{
	WPIN_RENDER,   /* PCM in from the system */
	WPIN_OUT       /* analog out, to the topology filter */
};
static PCPIN_DESCRIPTOR wave_pins[2];
static PCNODE_DESCRIPTOR wave_nodes[1];
static PCCONNECTION_DESCRIPTOR wave_conns[] = {
	{ PCFILTER_NODE, WPIN_RENDER, 0, 1 },
	{ 0, 0, PCFILTER_NODE, WPIN_OUT },
};
static GUID wave_categories[3];
static PCFILTER_DESCRIPTOR wave_filter;

static void wave_describe(void)
{
	PCPIN_DESCRIPTOR *p = &wave_pins[WPIN_RENDER];
	p->MaxGlobalInstanceCount = 1;
	p->MaxFilterInstanceCount = 1;
	p->KsPinDescriptor.DataRangesCount = 1;
	p->KsPinDescriptor.DataRanges = pcm_ranges;
	p->KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
	p->KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
	p->KsPinDescriptor.Category = &G_KSCATEGORY_AUDIO;
	p = &wave_pins[WPIN_OUT];
	p->KsPinDescriptor.DataRangesCount = 1;
	p->KsPinDescriptor.DataRanges = analog_ranges;
	p->KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
	p->KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
	p->KsPinDescriptor.Category = &G_KSCATEGORY_AUDIO;
	wave_nodes[0].Type = &G_KSNODETYPE_DAC;
	wave_categories[0] = G_KSCATEGORY_AUDIO;
	wave_categories[1] = G_KSCATEGORY_RENDER;
	wave_filter.Version = 0;
	wave_filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
	wave_filter.PinCount = 2;
	wave_filter.Pins = wave_pins;
	wave_filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
	wave_filter.NodeCount = 1;
	wave_filter.Nodes = wave_nodes;
	wave_filter.ConnectionCount = 2;
	wave_filter.Connections = wave_conns;
	wave_filter.CategoryCount = 2;
	wave_filter.Categories = wave_categories;
}

/* -- wave miniport ---------------------------------------------------------------------------- */
static NTSTATUS STDMETHODCALLTYPE wv_QueryInterface(IMiniportWaveCyclic *This, REFIID iid, PVOID *out)
{
	if (guid_eq(iid, &G_IID_IUnknown) || guid_eq(iid, &G_IID_IMiniport) || guid_eq(iid, &G_IID_IMiniportWaveCyclic))
	{
		*out = This;
		InterlockedIncrement(&THISOF(WAVE, This)->ref);
		return STATUS_SUCCESS;
	}
	*out = NULL;
	return STATUS_INVALID_PARAMETER;
}

static ULONG STDMETHODCALLTYPE wv_AddRef(IMiniportWaveCyclic *This)
{
	return InterlockedIncrement(&THISOF(WAVE, This)->ref);
}

static ULONG STDMETHODCALLTYPE wv_Release(IMiniportWaveCyclic *This)
{
	WAVE *w = THISOF(WAVE, This);
	LONG r = InterlockedDecrement(&w->ref);
	if (r == 0)
	{
		if (w->port)
			w->port->lpVtbl->Release(w->port);
		if (w->ad->wave == w)
			w->ad->wave = NULL;
		ExFreePool(w);
	}
	return (ULONG) r;
}

static NTSTATUS STDMETHODCALLTYPE wv_GetDescription(IMiniportWaveCyclic *This, PPCFILTER_DESCRIPTOR *d)
{
	(void) This;
	*d = &wave_filter;
	return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE wv_DataRangeIntersection(IMiniportWaveCyclic *This, ULONG pin, PKSDATARANGE range,
                                                           PKSDATARANGE matching, ULONG outlen, PVOID result,
                                                           PULONG resultlen)
{
	(void) This;
	(void) pin;
	(void) range;
	(void) matching;
	(void) outlen;
	(void) result;
	(void) resultlen;
	return STATUS_NOT_IMPLEMENTED;   /* PortCls' default does it */
}

static NTSTATUS STDMETHODCALLTYPE wv_Init(IMiniportWaveCyclic *This, PUNKNOWN adapter, PRESOURCELIST res,
                                          PPORTWAVECYCLIC port)
{
	WAVE *w = THISOF(WAVE, This);
	(void) adapter;
	(void) res;
	w->port = port;
	port->lpVtbl->AddRef(port);
	return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE wv_NewStream(IMiniportWaveCyclic *This, PMINIPORTWAVECYCLICSTREAM *out,
                                               PUNKNOWN outer, POOL_TYPE pool, ULONG pin, BOOLEAN capture,
                                               PKSDATAFORMAT fmt, PDMACHANNEL *dmaout, PSERVICEGROUP *sgout)
{
	WAVE *w = THISOF(WAVE, This);
	STREAM *s;
	NTSTATUS st;
	(void) outer;
	(void) pool;
	if (capture || pin != WPIN_RENDER || !format_ok(fmt))
		return STATUS_INVALID_PARAMETER;
	if (w->stream)
		return STATUS_INSUFFICIENT_RESOURCES;   /* one hardware stream: KMixer mixes */
	s = (STREAM *) ExAllocatePool(NonPagedPool, sizeof(*s));
	if (!s)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlZeroMemory(s, sizeof(*s));
	s->lpVtbl = &stream_vtbl;
	s->ref = 1;
	s->wave = w;
	s->state = KSSTATE_STOP;
	/* one buffer: the audio, then the descriptor list */
	st = w->port->lpVtbl->NewMasterDmaChannel(w->port, &s->dma, NULL, NULL, BUF_SIZE + NBDL * sizeof(struct bdl),
	                                          TRUE, FALSE, Width32Bits, MaximumDmaSpeed);
	if (!NT_SUCCESS(st))
		goto fail;
	st = s->dma->lpVtbl->AllocateBuffer(s->dma, BUF_SIZE + NBDL * sizeof(struct bdl), NULL);
	if (!NT_SUCCESS(st))
		goto fail;
	s->buf = (PUCHAR) s->dma->lpVtbl->SystemAddress(s->dma);
	/* not IDmaChannel::PhysicalAddress: it returns a struct the C++ way */
	s->buf_pa = MmGetPhysicalAddress(s->buf);
	s->bdl = (struct bdl *) (s->buf + BUF_SIZE);
	s->bdl_pa = MmGetPhysicalAddress(s->bdl);
	s->dma->lpVtbl->SetBufferSize(s->dma, BUF_SIZE);
	RtlZeroMemory(s->buf, BUF_SIZE);
	bdl_fill(s);
	st = PcNewServiceGroup(&s->sg, NULL);
	if (!NT_SUCCESS(st))
		goto fail;
	w->stream = s;
	*out = (PMINIPORTWAVECYCLICSTREAM) s;
	*dmaout = s->dma;
	s->dma->lpVtbl->AddRef(s->dma);
	*sgout = s->sg;
	s->sg->lpVtbl->AddRef(s->sg);
	DPRINT("kexhda: stream open, buffer at %08lx\n", s->buf_pa.LowPart);
	return STATUS_SUCCESS;
fail:
	DPRINT("kexhda: NewStream failed %08lx\n", st);
	if (s->sg)
		s->sg->lpVtbl->Release(s->sg);
	if (s->dma)
	{
		s->dma->lpVtbl->FreeBuffer(s->dma);
		s->dma->lpVtbl->Release(s->dma);
	}
	ExFreePool(s);
	return st;
}

static IMiniportWaveCyclicVtbl wave_vtbl = {
	wv_QueryInterface, wv_AddRef, wv_Release, wv_GetDescription, wv_DataRangeIntersection, wv_Init, wv_NewStream,
};

/* ===================================================================================== *
 * topology: master volume and mute                                                     *
 * ===================================================================================== */
typedef struct topo
{
	IMiniportTopologyVtbl *lpVtbl;
	LONG ref;
	ADAPTER *ad;
} TOPO;

enum
{
	TPIN_WAVE,   /* from the wave filter */
	TPIN_OUT     /* line out / speakers */
};
enum
{
	TNODE_VOLUME,
	TNODE_MUTE
};

#define DB(x) ((LONG) ((x) * 65536))
#define VOL_MIN (-DB(46.5))
#define VOL_STEP DB(1.5)

static LONG vol_get(ADAPTER *ad, ULONG ch)
{
	return ad->vol[ch ? 1 : 0];
}

static void vol_set(ADAPTER *ad, ULONG ch, LONG v)
{
	if (v > 0)
		v = 0;
	if (v < VOL_MIN)
		v = VOL_MIN;
	if (ch == 0 || ch == (ULONG) -1)
		ad->vol[0] = v;
	if (ch == 1 || ch == (ULONG) -1)
		ad->vol[1] = v;
	apply_volume(ad);
}

static NTSTATUS basic_support_volume(PPCPROPERTY_REQUEST r)
{
	ULONG need = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
	if (r->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
	{
		PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION) r->Value;
		d->AccessFlags = KSPROPERTY_TYPE_ALL;
		d->DescriptionSize = need;
		d->PropTypeSet.Set = G_KSPROPTYPESETID_General;
		d->PropTypeSet.Id = VT_I4;
		d->PropTypeSet.Flags = 0;
		d->MembersListCount = 1;
		d->Reserved = 0;
		if (r->ValueSize >= need)
		{
			PKSPROPERTY_MEMBERSHEADER m = (PKSPROPERTY_MEMBERSHEADER) (d + 1);
			PKSPROPERTY_STEPPING_LONG s = (PKSPROPERTY_STEPPING_LONG) (m + 1);
			m->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
			m->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
			m->MembersCount = 1;
			m->Flags = 0;
			s->SteppingDelta = VOL_STEP;
			s->Reserved = 0;
			s->Bounds.SignedMinimum = VOL_MIN;
			s->Bounds.SignedMaximum = 0;
			r->ValueSize = need;
		}
		else
			r->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
		return STATUS_SUCCESS;
	}
	if (r->ValueSize >= sizeof(ULONG))
	{
		*(PULONG) r->Value = KSPROPERTY_TYPE_ALL;
		r->ValueSize = sizeof(ULONG);
		return STATUS_SUCCESS;
	}
	r->ValueSize = need;
	return STATUS_BUFFER_OVERFLOW;
}

static NTSTATUS NTAPI prop_volume(PPCPROPERTY_REQUEST r)
{
	ADAPTER *ad = &g_ad;
	ULONG ch;
	if (r->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
		return basic_support_volume(r);
	if (r->InstanceSize < sizeof(ULONG) || r->ValueSize < sizeof(LONG))
		return STATUS_INVALID_PARAMETER;
	ch = *(PULONG) r->Instance;   /* the channel */
	if (ch > 1 && ch != (ULONG) -1)
		return STATUS_INVALID_PARAMETER;
	if (r->Verb & KSPROPERTY_TYPE_GET)
		*(PLONG) r->Value = vol_get(ad, ch == (ULONG) -1 ? 0 : ch);
	else if (r->Verb & KSPROPERTY_TYPE_SET)
		vol_set(ad, ch, *(PLONG) r->Value);
	r->ValueSize = sizeof(LONG);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI prop_mute(PPCPROPERTY_REQUEST r)
{
	ADAPTER *ad = &g_ad;
	if (r->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
	{
		if (r->ValueSize >= sizeof(ULONG))
		{
			*(PULONG) r->Value = KSPROPERTY_TYPE_ALL;
			r->ValueSize = sizeof(ULONG);
			return STATUS_SUCCESS;
		}
		return STATUS_BUFFER_TOO_SMALL;
	}
	if (r->ValueSize < sizeof(BOOL))
		return STATUS_INVALID_PARAMETER;
	if (r->Verb & KSPROPERTY_TYPE_GET)
		*(PBOOL) r->Value = ad->muted != 0;
	else if (r->Verb & KSPROPERTY_TYPE_SET)
	{
		ad->muted = *(PBOOL) r->Value ? 1 : 0;
		apply_volume(ad);
		if (!ad->muted)
			pins_on(ad);
	}
	r->ValueSize = sizeof(BOOL);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI prop_cpu(PPCPROPERTY_REQUEST r)
{
	if (r->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
	{
		if (r->ValueSize >= sizeof(ULONG))
		{
			*(PULONG) r->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
			r->ValueSize = sizeof(ULONG);
			return STATUS_SUCCESS;
		}
		return STATUS_BUFFER_TOO_SMALL;
	}
	if (r->ValueSize < sizeof(LONG))
		return STATUS_BUFFER_TOO_SMALL;
	*(PLONG) r->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
	r->ValueSize = sizeof(LONG);
	return STATUS_SUCCESS;
}

#define ALLVERBS (PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT)
static PCPROPERTY_ITEM vol_props[2], mute_props[2];
static PCAUTOMATION_TABLE vol_auto, mute_auto;
static PCPIN_DESCRIPTOR topo_pins[2];
static PCNODE_DESCRIPTOR topo_nodes[2];
static PCCONNECTION_DESCRIPTOR topo_conns[] = {
	{ PCFILTER_NODE, TPIN_WAVE, TNODE_VOLUME, 1 },
	{ TNODE_VOLUME, 0, TNODE_MUTE, 1 },
	{ TNODE_MUTE, 0, PCFILTER_NODE, TPIN_OUT },
};
static GUID topo_categories[2];
static PCFILTER_DESCRIPTOR topo_filter;

static void topo_describe(void)
{
	vol_props[0].Set = &G_KSPROPSETID_Audio;
	vol_props[0].Id = KSPROPERTY_AUDIO_VOLUMELEVEL;
	vol_props[0].Flags = ALLVERBS;
	vol_props[0].Handler = prop_volume;
	vol_props[1].Set = &G_KSPROPSETID_Audio;
	vol_props[1].Id = KSPROPERTY_AUDIO_CPU_RESOURCES;
	vol_props[1].Flags = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;
	vol_props[1].Handler = prop_cpu;
	mute_props[0].Set = &G_KSPROPSETID_Audio;
	mute_props[0].Id = KSPROPERTY_AUDIO_MUTE;
	mute_props[0].Flags = ALLVERBS;
	mute_props[0].Handler = prop_mute;
	mute_props[1] = vol_props[1];
	vol_auto.PropertyItemSize = sizeof(PCPROPERTY_ITEM);
	vol_auto.PropertyCount = 2;
	vol_auto.Properties = vol_props;
	vol_auto.MethodItemSize = sizeof(PCMETHOD_ITEM);
	vol_auto.EventItemSize = sizeof(PCEVENT_ITEM);
	mute_auto = vol_auto;
	mute_auto.Properties = mute_props;

	topo_pins[TPIN_WAVE].KsPinDescriptor.DataRangesCount = 1;
	topo_pins[TPIN_WAVE].KsPinDescriptor.DataRanges = analog_ranges;
	topo_pins[TPIN_WAVE].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
	topo_pins[TPIN_WAVE].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
	topo_pins[TPIN_WAVE].KsPinDescriptor.Category = &G_KSCATEGORY_AUDIO;
	topo_pins[TPIN_OUT].KsPinDescriptor.DataRangesCount = 1;
	topo_pins[TPIN_OUT].KsPinDescriptor.DataRanges = analog_ranges;
	topo_pins[TPIN_OUT].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
	topo_pins[TPIN_OUT].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
	topo_pins[TPIN_OUT].KsPinDescriptor.Category = &G_KSNODETYPE_SPEAKER;
	topo_nodes[TNODE_VOLUME].AutomationTable = &vol_auto;
	topo_nodes[TNODE_VOLUME].Type = &G_KSNODETYPE_VOLUME;
	topo_nodes[TNODE_VOLUME].Name = &G_KSAUDFNAME_MASTER_VOLUME;
	topo_nodes[TNODE_MUTE].AutomationTable = &mute_auto;
	topo_nodes[TNODE_MUTE].Type = &G_KSNODETYPE_MUTE;
	topo_nodes[TNODE_MUTE].Name = &G_KSAUDFNAME_MASTER_MUTE;
	topo_categories[0] = G_KSCATEGORY_AUDIO;
	topo_categories[1] = G_KSCATEGORY_TOPOLOGY;
	topo_filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
	topo_filter.PinCount = 2;
	topo_filter.Pins = topo_pins;
	topo_filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
	topo_filter.NodeCount = 2;
	topo_filter.Nodes = topo_nodes;
	topo_filter.ConnectionCount = 3;
	topo_filter.Connections = topo_conns;
	topo_filter.CategoryCount = 2;
	topo_filter.Categories = topo_categories;
}

static NTSTATUS STDMETHODCALLTYPE tp_QueryInterface(IMiniportTopology *This, REFIID iid, PVOID *out)
{
	DPRINT("kexhda: topo QI %08lx\n", iid->Data1);
	if (guid_eq(iid, &G_IID_IUnknown) || guid_eq(iid, &G_IID_IMiniport) || guid_eq(iid, &G_IID_IMiniportTopology))
	{
		*out = This;
		InterlockedIncrement(&THISOF(TOPO, This)->ref);
		return STATUS_SUCCESS;
	}
	*out = NULL;
	return STATUS_INVALID_PARAMETER;
}

static ULONG STDMETHODCALLTYPE tp_AddRef(IMiniportTopology *This)
{
	return InterlockedIncrement(&THISOF(TOPO, This)->ref);
}

static ULONG STDMETHODCALLTYPE tp_Release(IMiniportTopology *This)
{
	TOPO *t = THISOF(TOPO, This);
	LONG r = InterlockedDecrement(&t->ref);
	if (r == 0)
		ExFreePool(t);
	return (ULONG) r;
}

static NTSTATUS STDMETHODCALLTYPE tp_GetDescription(IMiniportTopology *This, PPCFILTER_DESCRIPTOR *d)
{
	DPRINT("kexhda: topo description\n");
	(void) This;
	*d = &topo_filter;
	return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE tp_DataRangeIntersection(IMiniportTopology *This, ULONG pin, PKSDATARANGE range,
                                                           PKSDATARANGE matching, ULONG outlen, PVOID result,
                                                           PULONG resultlen)
{
	(void) This;
	(void) pin;
	(void) range;
	(void) matching;
	(void) outlen;
	(void) result;
	(void) resultlen;
	return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS STDMETHODCALLTYPE tp_Init(IMiniportTopology *This, PUNKNOWN adapter, PRESOURCELIST res,
                                          PPORTTOPOLOGY port)
{
	(void) This;
	(void) adapter;
	(void) res;
	(void) port;
	return STATUS_SUCCESS;
}

static IMiniportTopologyVtbl topo_vtbl = {
	tp_QueryInterface, tp_AddRef, tp_Release, tp_GetDescription, tp_DataRangeIntersection, tp_Init,
};

/* ===================================================================================== *
 * interrupt                                                                             *
 * ===================================================================================== */
static NTSTATUS NTAPI isr(PINTERRUPTSYNC sync, PVOID ctx)
{
	ADAPTER *ad = (ADAPTER *) ctx;
	ULONG is;
	UCHAR st;
	(void) sync;
	if (!ad->ready)
		return STATUS_UNSUCCESSFUL;
	is = R32(INTSTS);
	if (!(is & 0x80000000u) || is == 0xffffffffu)
		return STATUS_UNSUCCESSFUL;   /* not ours: the line is shared */
	if (is & 0x40000000u)
		W8(RIRBSTS, 0x05);
	st = R8(SD(ad->out_sd) + SD_STS);
	W8(SD(ad->out_sd) + SD_STS, st & STS_CLEAR);
	if ((is & (1u << ad->out_sd)) && ad->wave && ad->wave->stream && ad->wave->port)
		ad->wave->port->lpVtbl->Notify(ad->wave->port, ad->wave->stream->sg);
	return STATUS_SUCCESS;
}

/* ===================================================================================== *
 * adapter: start, ports, registration                                                   *
 * ===================================================================================== */
static NTSTATUS new_port_miniport(PDEVICE_OBJECT dev, PIRP irp, const GUID *clsid, PUNKNOWN miniport,
                                  PRESOURCELIST res, PCWSTR name, PPORT *out)
{
	PPORT port;
	NTSTATUS st = PcNewPort(&port, clsid);
	if (!NT_SUCCESS(st))
		return st;
	DPRINT("kexhda: port %lx made, init\n", (ULONG) port);
	st = port->lpVtbl->Init(port, dev, irp, miniport, NULL, res);
	DPRINT("kexhda: port init %08lx\n", st);
	if (NT_SUCCESS(st))
		st = PcRegisterSubdevice(dev, (PWCHAR) name, (PUNKNOWN) port);
	if (!NT_SUCCESS(st))
	{
		port->lpVtbl->Release(port);
		return st;
	}
	*out = port;
	return STATUS_SUCCESS;
}

static NTSTATUS start_body(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res);

static NTSTATUS NTAPI start_device(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res)
{
	NTSTATUS st = start_body(dev, irp, res);
	DPRINT("kexhda: start result %08lx\n", st);
	kexos_log_save(&hlog);
	return st;
}

static NTSTATUS start_body(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res)
{
	ADAPTER *ad = &g_ad;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR mem;
	PRESOURCELIST io = NULL;
	WAVE *w;
	TOPO *t;
	PPORT pwave = NULL, ptopo = NULL;
	NTSTATUS st;

	DPRINT("kexhda: start, %lu memory ranges, %lu interrupts\n", res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypeMemory),
	       res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypeInterrupt));
	if (res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypeMemory) < 1 ||
	    res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypeInterrupt) < 1)
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	mem = res->lpVtbl->FindTranslatedEntry(res, CmResourceTypeMemory, 0);
	RtlZeroMemory(ad, sizeof(*ad));
	KeInitializeSpinLock(&ad->codec_lock);
	ad->mmio_len = mem->u.Memory.Length;
	ad->mmio = kexos_mmio_map(mem->u.Memory.Start.LowPart, (kx_u32) mem->u.Memory.Start.HighPart, ad->mmio_len);
	if (!ad->mmio)
		return STATUS_INSUFFICIENT_RESOURCES;
	DPRINT("kexhda: registers at %08lx\n", mem->u.Memory.Start.LowPart);
	st = hw_init(ad);
	if (!NT_SUCCESS(st))
		return st;
	kexos_timer(500, hp_tick, ad);   /* headphone sense */

	st = PcNewInterruptSync(&ad->isync, NULL, res, 0, InterruptSyncModeNormal);
	if (NT_SUCCESS(st))
		st = ad->isync->lpVtbl->RegisterServiceRoutine(ad->isync, isr, ad, FALSE);
	if (NT_SUCCESS(st))
		st = ad->isync->lpVtbl->Connect(ad->isync);
	if (!NT_SUCCESS(st))
	{
		DPRINT("kexhda: interrupt %08lx\n", st);
		return st;
	}
	(void) io;
	DPRINT("kexhda: interrupt connected\n");

	wave_describe();
	topo_describe();
	w = (WAVE *) ExAllocatePool(NonPagedPool, sizeof(*w));
	t = (TOPO *) ExAllocatePool(NonPagedPool, sizeof(*t));
	if (!w || !t)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlZeroMemory(w, sizeof(*w));
	RtlZeroMemory(t, sizeof(*t));
	w->lpVtbl = &wave_vtbl;
	w->ref = 1;
	w->ad = ad;
	ad->wave = w;
	t->lpVtbl = &topo_vtbl;
	t->ref = 1;
	t->ad = ad;

	st = new_port_miniport(dev, irp, &G_CLSID_PortTopology, (PUNKNOWN) t, res, L"Topology", &ptopo);
	DPRINT("kexhda: topology port %08lx\n", st);
	if (NT_SUCCESS(st))
		st = new_port_miniport(dev, irp, &G_CLSID_PortWaveCyclic, (PUNKNOWN) w, res, L"Wave", &pwave);
	DPRINT("kexhda: wave port %08lx\n", st);
	if (NT_SUCCESS(st))
		st = PcRegisterPhysicalConnection(dev, (PUNKNOWN) pwave, WPIN_OUT, (PUNKNOWN) ptopo, TPIN_WAVE);
	/* the ports hold the miniports now */
	((IMiniportWaveCyclic *) w)->lpVtbl->Release((IMiniportWaveCyclic *) w);
	((IMiniportTopology *) t)->lpVtbl->Release((IMiniportTopology *) t);
	if (pwave)
		pwave->lpVtbl->Release(pwave);
	if (ptopo)
		ptopo->lpVtbl->Release(ptopo);
	DPRINT("kexhda: started %08lx\n", st);
	return st;
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	return PcAddAdapterDevice(drv, pdo, start_device, 3, 0);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	DPRINT("kexhda: loaded\n");
	return PcInitializeAdapterDriver(drv, reg, (PDRIVER_ADD_DEVICE) add_device);
}
