/*
 *  KernelExEx - kexac97.sys: AC'97 audio for Windows 98 SE and ME, on the
 *  audio controllers of Intel ICH chipsets (82801AA to ICH7, 6300ESB, the
 *  440MX) and their compatibles (nVidia nForce, SiS 7012, AMD 8111/CS5536),
 *  with any AC'97 codec: Realtek ALC650/655/658/850, Analog Devices
 *  AD1980/1981/1985/1986/1988, SigmaTel, Cirrus, VIA VT1616 ...
 *  Onboard on boards such as the MSI 848P Neo2-V (ICH5 + ALC655).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Our own driver, from the Intel ICH AC'97 controller and AC'97 2.3
 *  register descriptions. A WDM audio adapter for PortCls: a WaveCyclic
 *  miniport (playback, 48 kHz 16-bit stereo; KMixer converts the rest) and
 *  a Topology miniport (master volume and mute), which is what the volume
 *  control and every Windows audio API sit on.
 *
 *  Written in C against PortCls' COM interfaces: each object starts with
 *  its interface's vtable; methods are __stdcall with the object first.
 */
#include <ntddk.h>
typedef char TCHAR;
#include <portcls.h>
#include <ksmedia.h>

/* messages: QEMU's debug console in test builds, and always a copy that
 * start_device saves to the registry (HKLM\Software\KernelEx\Diag,
 * "kexac97") for testing on real machines */
static void dbg(const char *fmt, ...);
#define DPRINT(...) dbg(__VA_ARGS__)

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

/* the compiler's own calls (structure clears) */
void *__cdecl memset(void *d, int c, size_t n)
{
	unsigned char *p = (unsigned char *) d;
	while (n--)
		*p++ = (unsigned char) c;
	return d;
}

static int guid_eq(REFIID a, const GUID *b)
{
	return RtlCompareMemory(a, b, sizeof(GUID)) == sizeof(GUID);
}

/* ---- the controller ---------------------------------------------------------------------- */
/* native audio bus master (NABMBAR) */
#define PO_BDBAR 0x10
#define PO_CIV 0x14
#define PO_LVI 0x15
#define PO_SR 0x16
#define PO_PICB 0x18
#define PO_CR 0x1b
#define GLOB_CNT 0x2c
#define GLOB_STA 0x30
#define CAS 0x34
#define SR_DCH 0x01
#define SR_LVBCI 0x04
#define SR_BCIS 0x08
#define SR_FIFOE 0x10
#define SR_CLEAR (SR_LVBCI | SR_BCIS | SR_FIFOE)
#define CR_RPBM 0x01
#define CR_RR 0x02
#define CR_LVBIE 0x04
#define CR_FEIE 0x08
#define CR_IOCE 0x10
#define CNT_COLD 0x00000002   /* 1: out of cold reset */
#define CNT_WARM 0x00000004
#define STA_PCR 0x00000100    /* primary codec ready */
#define STA_POINT 0x00000040  /* PCM out interrupt */
/* the codec (NAMBAR) */
#define AC97_RESET 0x00
#define AC97_MASTER 0x02
#define AC97_HEADPHONE 0x04
#define AC97_PCM_OUT 0x18
#define AC97_POWER 0x26
#define AC97_EXT_ID 0x28
#define AC97_EXT_CTRL 0x2a
#define AC97_FRONT_RATE 0x2c
#define AC97_VENDOR1 0x7c
#define AC97_VENDOR2 0x7e

#define KSPROPERTY_TYPE_ALL (KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT)
#define NBDL 32
#define BUF_SIZE 0x8000   /* 32K cyclic buffer: 170 ms at 48 kHz, 1K per descriptor */
#define CHUNK (BUF_SIZE / NBDL)
#define RATE 48000

struct bdl
{
	ULONG addr;
	USHORT samples;   /* 16-bit samples, both channels */
	USHORT flags;     /* 0x8000: interrupt on completion */
};

typedef struct adapter
{
	PUCHAR mixer;     /* NAMBAR */
	PUCHAR bm;        /* NABMBAR */
	KSPIN_LOCK codec_lock;
	USHORT vendor1, vendor2;
	USHORT master;    /* shadow of AC97_MASTER */
	int ready;
	PINTERRUPTSYNC isync;
	struct wave *wave;
} ADAPTER;

static ADAPTER g_ad;

static USHORT codec_read(ADAPTER *ad, UCHAR reg);

static int codec_wait(ADAPTER *ad)
{
	int i;
	/* the semaphore: reading CAS takes it, the next codec access gives it back */
	for (i = 0; i < 1000; i++)
	{
		if (!(READ_PORT_UCHAR(ad->bm + CAS) & 1))
			return 1;
		KeStallExecutionProcessor(10);
	}
	return 0;
}

static USHORT codec_read(ADAPTER *ad, UCHAR reg)
{
	KIRQL irql;
	USHORT v;
	KeAcquireSpinLock(&ad->codec_lock, &irql);
	codec_wait(ad);
	v = READ_PORT_USHORT((PUSHORT) (ad->mixer + reg));
	KeReleaseSpinLock(&ad->codec_lock, irql);
	return v;
}

static void codec_write(ADAPTER *ad, UCHAR reg, USHORT v)
{
	KIRQL irql;
	KeAcquireSpinLock(&ad->codec_lock, &irql);
	codec_wait(ad);
	WRITE_PORT_USHORT((PUSHORT) (ad->mixer + reg), v);
	KeReleaseSpinLock(&ad->codec_lock, irql);
}

static NTSTATUS hw_init(ADAPTER *ad)
{
	ULONG cnt;
	int i;
	/* out of cold reset (BIOSes leave it either way), interrupts off */
	cnt = READ_PORT_ULONG((PULONG) (ad->bm + GLOB_CNT));
	cnt &= ~(0x00300000 | 0x1);    /* 2-channel PCM, GIE off */
	if (!(cnt & CNT_COLD))
		cnt |= CNT_COLD;
	else
		cnt |= CNT_WARM;   /* already up: a warm reset wakes a sleeping codec */
	WRITE_PORT_ULONG((PULONG) (ad->bm + GLOB_CNT), cnt);
	for (i = 0; i < 500; i++)   /* the spec allows a long wait for the codec */
	{
		if (READ_PORT_ULONG((PULONG) (ad->bm + GLOB_STA)) & STA_PCR)
			break;
		KeStallExecutionProcessor(1000);
	}
	if (i == 500)
	{
		DPRINT("kexac97: no codec ready (GLOB_STA %08lx)\n", READ_PORT_ULONG((PULONG) (ad->bm + GLOB_STA)));
		return STATUS_DEVICE_NOT_READY;
	}
	codec_write(ad, AC97_RESET, 0);
	for (i = 0; i < 500; i++)
	{
		if ((codec_read(ad, AC97_POWER) & 0x000f) == 0x000f)   /* ADC, DAC, analog, Vref ready */
			break;
		KeStallExecutionProcessor(1000);
	}
	ad->vendor1 = codec_read(ad, AC97_VENDOR1);
	ad->vendor2 = codec_read(ad, AC97_VENDOR2);
	/* fixed 48 kHz: every codec does it, VRA or not; KMixer converts */
	if (codec_read(ad, AC97_EXT_ID) & 1)
	{
		codec_write(ad, AC97_EXT_CTRL, codec_read(ad, AC97_EXT_CTRL) | 1);
		codec_write(ad, AC97_FRONT_RATE, RATE);
	}
	ad->master = 0x0000;                    /* 0 dB, unmuted */
	codec_write(ad, AC97_MASTER, ad->master);
	codec_write(ad, AC97_HEADPHONE, 0x0000);
	codec_write(ad, AC97_PCM_OUT, 0x0808);   /* 0 dB */
	/* PCM out engine: stopped and reset */
	WRITE_PORT_UCHAR(ad->bm + PO_CR, 0);
	WRITE_PORT_UCHAR(ad->bm + PO_CR, CR_RR);
	for (i = 0; i < 100 && (READ_PORT_UCHAR(ad->bm + PO_CR) & CR_RR); i++)
		KeStallExecutionProcessor(10);
	WRITE_PORT_USHORT((PUSHORT) (ad->bm + PO_SR), SR_CLEAR);
	DPRINT("kexac97: codec %04x%04x ready\n", ad->vendor1, ad->vendor2);
	ad->ready = 1;
	return STATUS_SUCCESS;
}

/* ICH4 and later only decode the legacy I/O BARs with IOSE (config 0x41 bit 0) set */
static void enable_io(ADAPTER *ad, ULONG iobase)
{
	ULONG bus, slot;
	for (bus = 0; bus < 4; bus++)
		for (slot = 0; slot < 256; slot++)   /* device << 3 | function */
		{
			UCHAR cfg[0x44];
			PCI_SLOT_NUMBER sn;
			ULONG n;
			sn.u.AsULONG = 0;
			sn.u.bits.DeviceNumber = slot >> 3;
			sn.u.bits.FunctionNumber = slot & 7;
			n = HalGetBusData(PCIConfiguration, bus, sn.u.AsULONG, cfg, sizeof(cfg));
			if (n < sizeof(cfg) || *(PUSHORT) cfg == 0xffff)
				continue;
			if (cfg[0x0b] != 0x04 || cfg[0x0a] != 0x01)   /* multimedia, audio */
				continue;
			if (((*(PULONG) (cfg + 0x10)) & ~3u) != iobase)
				continue;
			/* only ICH4 and later have IOSE there (82801DB 24C5, EB 24D5, 6300ESB 25A6, ICH6 266E, ICH7 27DE) */
			if (*(PUSHORT) cfg == 0x8086 && !(cfg[0x41] & 1) &&
			    (*(PUSHORT) (cfg + 2) == 0x24c5 || *(PUSHORT) (cfg + 2) == 0x24d5 || *(PUSHORT) (cfg + 2) == 0x25a6 ||
			     *(PUSHORT) (cfg + 2) == 0x266e || *(PUSHORT) (cfg + 2) == 0x27de))
			{
				UCHAR v = cfg[0x41] | 1;
				HalSetBusDataByOffset(PCIConfiguration, bus, sn.u.AsULONG, &v, 0x41, 1);
				DPRINT("kexac97: IOSE set on %lu:%lu.%lu\n", bus, slot >> 3, slot & 7);
			}
			(void) ad;
			return;
		}
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
	ADAPTER *ad = s->wave->ad;
	int i;
	WRITE_PORT_UCHAR(ad->bm + PO_CR, 0);
	for (i = 0; i < 100 && !(READ_PORT_USHORT((PUSHORT) (ad->bm + PO_SR)) & SR_DCH); i++)
		KeStallExecutionProcessor(10);
	WRITE_PORT_USHORT((PUSHORT) (ad->bm + PO_SR), SR_CLEAR);
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
		s->bdl[i].addr = s->buf_pa.LowPart + (ULONG) i * CHUNK;
		s->bdl[i].samples = CHUNK / 2;
		s->bdl[i].flags = 0x8000;
	}
}

static NTSTATUS STDMETHODCALLTYPE st_SetState(IMiniportWaveCyclicStream *This, KSSTATE state)
{
	STREAM *s = THISOF(STREAM, This);
	ADAPTER *ad = s->wave->ad;
	if (state == s->state)
		return STATUS_SUCCESS;
	switch (state)
	{
	case KSSTATE_RUN:
		if (s->state != KSSTATE_PAUSE)
			break;
		{
			UCHAR civ = READ_PORT_UCHAR(ad->bm + PO_CIV);
			WRITE_PORT_ULONG((PULONG) (ad->bm + PO_BDBAR), s->bdl_pa.LowPart);
			WRITE_PORT_UCHAR(ad->bm + PO_LVI, (UCHAR) ((civ + NBDL - 1) % NBDL));
			WRITE_PORT_USHORT((PUSHORT) (ad->bm + PO_SR), SR_CLEAR);
			WRITE_PORT_UCHAR(ad->bm + PO_CR, CR_RPBM | CR_IOCE | CR_FEIE | CR_LVBIE);
			/* GIE: the controller's interrupt to the CPU */
			WRITE_PORT_ULONG((PULONG) (ad->bm + GLOB_CNT), READ_PORT_ULONG((PULONG) (ad->bm + GLOB_CNT)) | 1);
		}
		break;
	case KSSTATE_PAUSE:
		if (s->state == KSSTATE_RUN)
			WRITE_PORT_UCHAR(ad->bm + PO_CR, READ_PORT_UCHAR(ad->bm + PO_CR) & ~CR_RPBM);
		break;
	case KSSTATE_STOP:
	case KSSTATE_ACQUIRE:
		if (s->state == KSSTATE_RUN || s->state == KSSTATE_PAUSE)
		{
			/* stopping rewinds: a reset puts CIV back to 0, as the port expects position 0 */
			stream_stop_hw(s);
			WRITE_PORT_UCHAR(ad->bm + PO_CR, CR_RR);
			KeStallExecutionProcessor(20);
			WRITE_PORT_UCHAR(ad->bm + PO_CR, 0);
		}
		break;
	}
	s->state = state;
	return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE st_GetPosition(IMiniportWaveCyclicStream *This, PULONG pos)
{
	STREAM *s = THISOF(STREAM, This);
	ADAPTER *ad = s->wave->ad;
	UCHAR civ, civ2;
	USHORT picb;
	int tries = 0;
	do
	{
		civ = READ_PORT_UCHAR(ad->bm + PO_CIV) % NBDL;
		picb = READ_PORT_USHORT((PUSHORT) (ad->bm + PO_PICB));
		civ2 = READ_PORT_UCHAR(ad->bm + PO_CIV) % NBDL;
	} while (civ != civ2 && ++tries < 4);
	if (picb > CHUNK / 2)
		picb = CHUNK / 2;
	*pos = (ULONG) civ * CHUNK + (CHUNK - (ULONG) picb * 2);
	if (*pos >= BUF_SIZE)
		*pos -= BUF_SIZE;
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
	DPRINT("kexac97: stream open, buffer at %08lx\n", s->buf_pa.LowPart);
	return STATUS_SUCCESS;
fail:
	DPRINT("kexac97: NewStream failed %08lx\n", st);
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

/* master: 5-bit attenuation per channel, 1.5 dB steps, bit 15 mute */
static LONG vol_get(ADAPTER *ad, ULONG ch)
{
	USHORT att = ch == 0 ? (ad->master >> 8) & 0x1f : ad->master & 0x1f;
	return -(LONG) att * VOL_STEP;
}

static void vol_set(ADAPTER *ad, ULONG ch, LONG v)
{
	LONG att;
	if (v > 0)
		v = 0;
	if (v < VOL_MIN)
		v = VOL_MIN;
	att = (-v + VOL_STEP / 2) / VOL_STEP;
	if (att > 31)
		att = 31;
	if (ch == 0 || ch == (ULONG) -1)
		ad->master = (USHORT) ((ad->master & ~0x1f00) | (att << 8));
	if (ch == 1 || ch == (ULONG) -1)
		ad->master = (USHORT) ((ad->master & ~0x001f) | att);
	codec_write(ad, AC97_MASTER, ad->master);
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
		*(PBOOL) r->Value = (ad->master & 0x8000) != 0;
	else if (r->Verb & KSPROPERTY_TYPE_SET)
	{
		ad->master = (USHORT) (*(PBOOL) r->Value ? ad->master | 0x8000 : ad->master & ~0x8000);
		codec_write(ad, AC97_MASTER, ad->master);
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
	DPRINT("kexac97: topo QI %08lx\n", iid->Data1);
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
	DPRINT("kexac97: topo description\n");
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
	USHORT sr;
	(void) sync;
	if (!ad->ready)
		return STATUS_UNSUCCESSFUL;
	sr = READ_PORT_USHORT((PUSHORT) (ad->bm + PO_SR));
	if (!(sr & SR_CLEAR))
		return STATUS_UNSUCCESSFUL;   /* not ours: the line is shared */
	WRITE_PORT_USHORT((PUSHORT) (ad->bm + PO_SR), sr & SR_CLEAR);
	if (ad->wave && ad->wave->stream)
	{
		STREAM *s = ad->wave->stream;
		if (s->state == KSSTATE_RUN)
		{
			/* keep the last valid index one behind the current one: the ring never runs out */
			UCHAR civ = READ_PORT_UCHAR(ad->bm + PO_CIV);
			WRITE_PORT_UCHAR(ad->bm + PO_LVI, (UCHAR) ((civ + NBDL - 1) % NBDL));
			if (sr & SR_DCH)   /* halted (we were late): run again */
				WRITE_PORT_UCHAR(ad->bm + PO_CR, READ_PORT_UCHAR(ad->bm + PO_CR) | CR_RPBM);
		}
		if (ad->wave->port)
			ad->wave->port->lpVtbl->Notify(ad->wave->port, s->sg);
	}
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
	DPRINT("kexac97: port %lx made, init\n", (ULONG) port);
	st = port->lpVtbl->Init(port, dev, irp, miniport, NULL, res);
	DPRINT("kexac97: port init %08lx\n", st);
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
static void diag_save(void);

static NTSTATUS NTAPI start_device(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res)
{
	NTSTATUS st = start_body(dev, irp, res);
	DPRINT("kexac97: start result %08lx\n", st);
	diag_save();
	return st;
}

static NTSTATUS start_body(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res)
{
	ADAPTER *ad = &g_ad;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR io0, io1;
	PRESOURCELIST io;
	WAVE *w;
	TOPO *t;
	PPORT pwave = NULL, ptopo = NULL;
	NTSTATUS st;

	DPRINT("kexac97: start, %lu I/O ranges, %lu interrupts\n", res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypePort),
	       res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypeInterrupt));
	if (res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypePort) < 2 ||
	    res->lpVtbl->NumberOfEntriesOfType(res, CmResourceTypeInterrupt) < 1)
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	io0 = res->lpVtbl->FindTranslatedEntry(res, CmResourceTypePort, 0);
	io1 = res->lpVtbl->FindTranslatedEntry(res, CmResourceTypePort, 1);
	RtlZeroMemory(ad, sizeof(*ad));
	KeInitializeSpinLock(&ad->codec_lock);
	/* BAR0 is the mixer (256 bytes), BAR1 the bus master (64 bytes) */
	if (io0->u.Port.Length >= io1->u.Port.Length)
	{
		ad->mixer = (PUCHAR) io0->u.Port.Start.LowPart;
		ad->bm = (PUCHAR) io1->u.Port.Start.LowPart;
	}
	else
	{
		ad->mixer = (PUCHAR) io1->u.Port.Start.LowPart;
		ad->bm = (PUCHAR) io0->u.Port.Start.LowPart;
	}
	DPRINT("kexac97: mixer at %lx, bus master at %lx\n", (ULONG) ad->mixer, (ULONG) ad->bm);
	enable_io(ad, (ULONG) ad->mixer);
	st = hw_init(ad);
	if (!NT_SUCCESS(st))
		return st;

	st = PcNewInterruptSync(&ad->isync, NULL, res, 0, InterruptSyncModeNormal);
	if (NT_SUCCESS(st))
		st = ad->isync->lpVtbl->RegisterServiceRoutine(ad->isync, isr, ad, FALSE);
	if (NT_SUCCESS(st))
		st = ad->isync->lpVtbl->Connect(ad->isync);
	if (!NT_SUCCESS(st))
	{
		DPRINT("kexac97: interrupt %08lx\n", st);
		return st;
	}
	(void) io;
	DPRINT("kexac97: interrupt connected\n");

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
	DPRINT("kexac97: topology port %08lx\n", st);
	if (NT_SUCCESS(st))
		st = new_port_miniport(dev, irp, &G_CLSID_PortWaveCyclic, (PUNKNOWN) w, res, L"Wave", &pwave);
	DPRINT("kexac97: wave port %08lx\n", st);
	if (NT_SUCCESS(st))
		st = PcRegisterPhysicalConnection(dev, (PUNKNOWN) pwave, WPIN_OUT, (PUNKNOWN) ptopo, TPIN_WAVE);
	/* the ports hold the miniports now */
	((IMiniportWaveCyclic *) w)->lpVtbl->Release((IMiniportWaveCyclic *) w);
	((IMiniportTopology *) t)->lpVtbl->Release((IMiniportTopology *) t);
	if (pwave)
		pwave->lpVtbl->Release(pwave);
	if (ptopo)
		ptopo->lpVtbl->Release(ptopo);
	DPRINT("kexac97: started %08lx\n", st);
	return st;
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	return PcAddAdapterDevice(drv, pdo, start_device, 3, 0);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	DPRINT("kexac97: loaded\n");
	return PcInitializeAdapterDriver(drv, reg, (PDRIVER_ADD_DEVICE) add_device);
}

#include <stdarg.h>
static char diag[2048];
static ULONG diag_len;

/* %s %c %x %lx %u %lu %d %ld, nothing more */
static void out_c(char c)
{
#ifdef KEX_DEBUGCON
	WRITE_PORT_UCHAR((PUCHAR) 0xe9, (UCHAR) c);
#endif
	if (c == '\n' && diag_len + 2 < sizeof(diag))
		diag[diag_len++] = '\r';
	if (diag_len + 1 < sizeof(diag))
		diag[diag_len++] = c;
}

static void diag_save(void)
{
	UNICODE_STRING us, name;
	OBJECT_ATTRIBUTES oa;
	HANDLE h;
	ULONG disp;
	static const WCHAR *const keys[] = { L"\\Registry\\Machine\\Software\\KernelEx",
	                                     L"\\Registry\\Machine\\Software\\KernelEx\\Diag" };
	int i;
	for (i = 0; i < 2; i++)
	{
		RtlInitUnicodeString(&us, keys[i]);
		InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
		if (!NT_SUCCESS(ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp)))
			return;
		if (i == 1)
		{
			static WCHAR w[sizeof(diag)];
			ULONG k;
			for (k = 0; k < diag_len; k++)
				w[k] = (WCHAR) (UCHAR) diag[k];
			w[k] = 0;
			RtlInitUnicodeString(&name, L"kexac97");
			ZwSetValueKey(h, &name, 0, REG_SZ, w, (k + 1) * sizeof(WCHAR));
		}
		ZwClose(h);
	}
}

static void out_n(unsigned long v, unsigned base, int neg)
{
	char b[12];
	int i = 0;
	if (neg)
		out_c('-');
	do
	{
		b[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (i--)
		out_c(b[i]);
}

static void dbg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	for (; *fmt; fmt++)
	{
		if (*fmt != '%')
		{
			out_c(*fmt);
			continue;
		}
		fmt++;
		while (*fmt == '0' || (*fmt >= '1' && *fmt <= '9'))
			fmt++;
		if (*fmt == 'l')
			fmt++;
		switch (*fmt)
		{
		case 's':
		{
			const char *p = va_arg(ap, const char *);
			while (*p)
				out_c(*p++);
			break;
		}
		case 'c':
			out_c((char) va_arg(ap, int));
			break;
		case 'x':
			out_n(va_arg(ap, unsigned long), 16, 0);
			break;
		case 'd':
		{
			long v = va_arg(ap, long);
			out_n(v < 0 ? (unsigned long) -v : (unsigned long) v, 10, v < 0);
			break;
		}
		default:
			out_n(va_arg(ap, unsigned long), 10, 0);
			break;
		}
	}
	va_end(ap);
}
