/*
 *  KernelExEx - kexehci.sys: USB 2.0 (EHCI) host controllers on Windows 98 SE / ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Windows 98 SE has a USB 1.1 stack only. Every EHCI controller of the time
 *  comes with USB 1.1 companion controllers (UHCI or OHCI) on the same ports,
 *  so this driver takes the high-speed devices and gives every full- and
 *  low-speed device on a root port back to its companion, where Windows' own
 *  uhcd/openhci drivers keep running it as before.
 *
 *  It is a complete stack for its devices, in one driver: the host controller
 *  (async and periodic schedules: control, bulk and interrupt), the root hub,
 *  USB 2.0 hubs (with split transactions through their transaction
 *  translators, for full/low-speed devices behind them and behind the
 *  built-in "rate matching" hubs of later Intel chipsets), and a bus driver
 *  that gives each device a PDO with the same IDs Windows 98's usbhub.sys
 *  uses, accepting the same URBs and IOCTLs. Windows' own class drivers
 *  (hidusb.sys) and every KernelExEx USB driver run on it unchanged; the
 *  latter ask the USBDI bus interface whether their device runs at high
 *  speed.
 *
 *  Not yet: isochronous transfers (audio, webcams), composite devices as one
 *  PDO per interface, selective suspend.
 *
 *  Uses only kernel functions that Windows 98 SE provides
 *  (net/tools/exports/win98se-wdm). Reference: the EHCI 1.0 specification
 *  and the USB 2.0 specification (chapters 9 and 11).
 */
#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include "kex_usbbusif.h"
#include <stdarg.h>

#define TAG 0x6368654bu   /* "Kehc" */

/* ---- small runtime -------------------------------------------------------------------- */
void *__cdecl memset(void *d, int c, size_t n)
{
	unsigned char *p = (unsigned char *) d;
	while (n--)
		*p++ = (unsigned char) c;
	return d;
}

void *__cdecl memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = (unsigned char *) d;
	const unsigned char *q = (const unsigned char *) s;
	while (n--)
		*p++ = *q++;
	return d;
}

#define WMB() __asm__ __volatile__("lock; addl $0,0(%%esp)" ::: "memory")

static void *zalloc(SIZE_T n)
{
	void *p = ExAllocatePoolWithTag(NonPagedPool, n, TAG);
	if (p)
		memset(p, 0, n);
	return p;
}

static void delay_ms(ULONG ms)
{
	LARGE_INTEGER t;
	t.QuadPart = -(LONGLONG) ms * 10000;
	KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static ULONG now_ms(void)
{
	return (ULONG) (KeQueryInterruptTime() / 10000);
}

/* ---- log: memory ring, written to C:\WINDOWS\KEXUSB2.LOG for testing on real hardware ---- */
#define LOG_SIZE 16384
static char log_buf[LOG_SIZE];
static ULONG log_len;            /* total ever written */
static ULONG log_written;        /* log_len at the last file write */
static KSPIN_LOCK log_lock;

static void log_c(char c)
{
	log_buf[log_len % LOG_SIZE] = c;
	log_len++;
#ifdef KEX_DEBUGCON
	__asm__ __volatile__("outb %0, $0xe9" : : "a"(c));
#endif
}

static void log_n(unsigned long v, unsigned base, int width, int neg)
{
	char b[12];
	int i = 0;
	do
	{
		unsigned d = (unsigned) (v % base);
		b[i++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v);
	if (neg)
		log_c('-');
	while (width-- > i)
		log_c('0');
	while (i--)
		log_c(b[i]);
}

static void logf(const char *fmt, ...)
{
	va_list ap;
	KIRQL old;
	KeAcquireSpinLock(&log_lock, &old);
	va_start(ap, fmt);
	for (; *fmt; fmt++)
	{
		int width = 0;
		if (*fmt != '%')
		{
			log_c(*fmt);
			continue;
		}
		fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == 'l')
			fmt++;
		switch (*fmt)
		{
		case 's':
		{
			const char *p = va_arg(ap, const char *);
			while (p && *p)
				log_c(*p++);
			break;
		}
		case 'S':   /* wide string, ASCII part */
		{
			const WCHAR *p = va_arg(ap, const WCHAR *);
			while (p && *p)
			{
				log_c(*p < 0x80 ? (char) *p : '?');
				p++;
			}
			break;
		}
		case 'c':
			log_c((char) va_arg(ap, int));
			break;
		case 'x':
			log_n(va_arg(ap, unsigned long), 16, width, 0);
			break;
		case 'd':
		{
			long v = va_arg(ap, long);
			log_n(v < 0 ? (unsigned long) -v : (unsigned long) v, 10, width, v < 0);
			break;
		}
		default:
			log_n(va_arg(ap, unsigned long), 10, width, 0);
			break;
		}
	}
	va_end(ap);
	KeReleaseSpinLock(&log_lock, old);
}

static void log_flush(void)
{
	static const WCHAR *const paths[] = { L"\\SystemRoot\\KEXUSB2.LOG", L"\\DosDevices\\C:\\WINDOWS\\KEXUSB2.LOG" };
	static char out[LOG_SIZE];
	ULONG n, start, i, end;
	KIRQL old;
	KeAcquireSpinLock(&log_lock, &old);
	end = log_len;
	start = end > LOG_SIZE ? end - LOG_SIZE : 0;
	for (n = 0, i = start; i < end; i++)
		out[n++] = log_buf[i % LOG_SIZE];
	log_written = end;
	KeReleaseSpinLock(&log_lock, old);
	for (i = 0; i < 2; i++)
	{
		UNICODE_STRING us;
		OBJECT_ATTRIBUTES oa;
		IO_STATUS_BLOCK iosb;
		HANDLE h;
		RtlInitUnicodeString(&us, paths[i]);
		InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
		if (NT_SUCCESS(ZwCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
		                            FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL,
		                            0)))
		{
			ZwWriteFile(h, NULL, NULL, NULL, &iosb, out, n, NULL, NULL);
			ZwClose(h);
			return;
		}
	}
}

/* ---- EHCI registers and data structures ------------------------------------------------ */
#define CAP_LENGTH 0x00
#define CAP_HCSPARAMS 0x04
#define CAP_HCCPARAMS 0x08
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_USBINTR 0x08
#define OP_FRINDEX 0x0c
#define OP_CTRLDSSEG 0x10
#define OP_PERIODIC 0x14
#define OP_ASYNC 0x18
#define OP_CONFIGFLAG 0x40
#define OP_PORTSC(p) (0x44 + 4 * (p))

#define CMD_RS 0x01
#define CMD_HCRESET 0x02
#define CMD_PSE 0x10
#define CMD_ASE 0x20
#define CMD_IAAD 0x40
#define STS_USBINT 0x01
#define STS_ERRINT 0x02
#define STS_PCD 0x04
#define STS_FLR 0x08
#define STS_HSE 0x10
#define STS_IAA 0x20
#define STS_HALTED 0x1000
#define STS_INTMASK 0x3f

#define PS_CCS 0x0001
#define PS_CSC 0x0002
#define PS_PE 0x0004
#define PS_PEC 0x0008
#define PS_OCC 0x0020
#define PS_PR 0x0100
#define PS_LS_MASK 0x0c00
#define PS_LS_K 0x0400   /* low-speed device */
#define PS_PP 0x1000
#define PS_OWNER 0x2000
#define PS_RWC (PS_CSC | PS_PEC | PS_OCC)

/* qTD token */
#define TK_ACTIVE 0x80
#define TK_HALTED 0x40
#define TK_DBERR 0x20
#define TK_BABBLE 0x10
#define TK_XACT 0x08
#define TK_PID_OUT 0
#define TK_PID_IN 1
#define TK_PID_SETUP 2
#define TK_IOC 0x8000
#define TK_TOGGLE 0x80000000u
#define TK_CERR3 (3u << 10)
#define LP_T 1   /* terminate */
#define LP_QH 2  /* type QH */

/* speeds as in the QH endpoint characteristics */
#define SPEED_FULL 0
#define SPEED_LOW 1
#define SPEED_HIGH 2

typedef struct
{
	volatile ULONG next, alt, token, buf[5], bufhi[5], pad;
	volatile UCHAR setup[8];
} QTD;   /* 64 bytes */

typedef struct
{
	volatile ULONG hlink, epchar, epcaps, cur, next, alt, token, buf[5], bufhi[5];
	ULONG pad[15];
} QHW;   /* 128 bytes */

#define NTD 1024
#define NQH 256
#define QH_ASYNC_HEAD 0
#define QH_ANCHOR(period, i) (1 + (period) - 1 + (i))   /* periods 1..32: slots 1..63 */
#define QH_FIRST_FREE 64
#define MAX_PERIOD 32
#define MAX_PORTS 16
#define MAX_HUB_PORTS 15

/* ---- software structures --------------------------------------------------------------- */
struct HC;
struct DEV;

enum
{
	ROLE_DATA,
	ROLE_SETUP,
	ROLE_STATUS
};

typedef struct
{
	int next;   /* free list, or the next qTD of the same transfer */
	ULONG len;
	UCHAR role;
} TDSW;

typedef struct EP
{
	LIST_ENTRY all;      /* hc->eps */
	LIST_ENTRY sched;    /* async ring order, or the anchor's list */
	struct DEV *dev;
	int qh;
	UCHAR ea, type, ifno, alt;
	USHORT maxpkt, wmax;
	UCHAR interval, mult;
	int period, phase;
	int dummy;
	int linked, halted, scrub, closing;
	LIST_ENTRY xfers;
} EP;

typedef struct XFER
{
	LIST_ENTRY link;
	EP *ep;
	PIRP irp;
	PURB urb;
	UCHAR *va;          /* buffer through a system address, or NULL with mdl */
	PMDL mdl;
	ULONG bo;           /* byte offset of the buffer in its first page */
	ULONG len, actual;
	int in;
	int first, last, end;
	int short_seen;
	USBD_STATUS status;
	int cancel;
	USBD_STATUS cstatus;
	void (*done)(struct XFER *);
	void *ctx;
	KEVENT ev;          /* internal synchronous transfers */
	volatile LONG finished;
	UCHAR setup[8];
} XFER;

typedef struct DEV
{
	struct HC *hc;
	struct DEV *parent;
	int port;            /* 1-based on its parent (root hub or hub) */
	int addr;
	int speed;
	int tt_hub, tt_port;
	char path[32];
	USB_DEVICE_DESCRIPTOR dd;
	UCHAR *cfg;
	ULONG cfglen;
	UCHAR cfgval;
	EP *ep0;
	EP *eps[32];
	USHORT langid;
	WCHAR product[64];
	WCHAR serial[64];
	int has_serial;
	int gone, torn_down;
	/* hub */
	int is_hub;
	int nports;
	int pwrgood_ms;
	struct DEV *child[MAX_HUB_PORTS + 1];
	EP *hub_ep;
	XFER *hub_x;
	UCHAR hub_buf[8];
	volatile LONG hub_change;
	volatile LONG hub_pending;   /* status transfer submitted */
	/* device node */
	PDEVICE_OBJECT pdo;
	int pdo_removed;
} DEV;

typedef struct HC
{
	PDEVICE_OBJECT fdo, pdo, lower;
	int index;
	int started;
	/* registers */
	PUCHAR regs;
	ULONG regs_len;
	PHYSICAL_ADDRESS regs_pa;
	PUCHAR op;
	ULONG hcs, hcc;
	int nports, ncc;
	ULONG pci_bus, pci_slot;
	PKINTERRUPT intr;
	KDPC dpc;
	volatile LONG pending_sts;
	/* memory */
	volatile ULONG *flist;
	ULONG flist_pa;
	QHW *qhs;
	ULONG qh_pa;
	QTD *tds;
	ULONG td_pa;
	int ntd;
	TDSW tdsw[NTD];
	int td_free, td_nfree;
	UCHAR qh_used[NQH];
	LIST_ENTRY async_list;
	LIST_ENTRY anchor_list[64];
	int phase_rr[6];
	ULONG frame_hi;
	/* state */
	KSPIN_LOCK lock;
	LIST_ENTRY eps;
	FAST_MUTEX tree;
	DEV *root[MAX_PORTS + 1];
	UCHAR addr_used[128];
	int scrub_pending;
	LIST_ENTRY work;     /* IRPs the hub thread handles */
	/* hub thread */
	KEVENT ev, iaa;
	HANDLE thread;
	PVOID thread_obj;
	volatile int stop;
	int root_scan;       /* first scan: devices present at start */
	ULONG started_ms, flushed_ms;
	int pdo_count;
} HC;

enum
{
	EXT_FDO = 0x4f44,
	EXT_PDO = 0x4f50
};

typedef struct
{
	int kind;
	HC hc;
} FDOEXT;

typedef struct
{
	int kind;
	HC *hc;
	DEV *dev;
	int started;
	KSPIN_LOCK plock;
	LIST_ENTRY parked;   /* transfers sent after the device was unplugged */
} PDOEXT;

static int hc_count;
static PDRIVER_OBJECT g_drv;

#define TD(hc, i) (&(hc)->tds[i])
#define TDPA(hc, i) ((hc)->td_pa + (ULONG) (i) * sizeof(QTD))
#define QH(hc, i) (&(hc)->qhs[i])
#define QHPA(hc, i) ((hc)->qh_pa + (ULONG) (i) * sizeof(QHW))

static ULONG op_r(HC *hc, ULONG off)
{
	return *(volatile ULONG *) (hc->op + off);
}

static void op_w(HC *hc, ULONG off, ULONG v)
{
	*(volatile ULONG *) (hc->op + off) = v;
}

static ULONG cap_r(HC *hc, ULONG off)
{
	return *(volatile ULONG *) (hc->regs + off);
}

/* ---- PCI configuration ------------------------------------------------------------------- */
static ULONG cfg_r32(HC *hc, ULONG off)
{
	UCHAR c[256];   /* Windows 98 has no HalGetBusDataByOffset */
	memset(c, 0, sizeof(c));
	HalGetBusData(PCIConfiguration, hc->pci_bus, hc->pci_slot, c, sizeof(c));
	return *(PULONG) (c + (off & 0xfc));
}

static void cfg_w(HC *hc, ULONG off, ULONG v, ULONG len)
{
	HalSetBusDataByOffset(PCIConfiguration, hc->pci_bus, hc->pci_slot, &v, off, len);
}

/* find our controller by its memory BAR: Windows 98 gives WDM drivers no
 * bus/slot numbers (the same way kexac97 finds its device) */
static int pci_find(HC *hc, ULONG bar)
{
	ULONG bus, slot;
	for (bus = 0; bus < 8; bus++)
		for (slot = 0; slot < 256; slot++)
		{
			UCHAR c[0x14];
			PCI_SLOT_NUMBER sn;
			sn.u.AsULONG = 0;
			sn.u.bits.DeviceNumber = slot >> 3;
			sn.u.bits.FunctionNumber = slot & 7;
			if (HalGetBusData(PCIConfiguration, bus, sn.u.AsULONG, c, sizeof(c)) < sizeof(c) || *(PUSHORT) c == 0xffff)
				continue;
			if (c[0x0b] != 0x0c || c[0x0a] != 0x03 || c[0x09] != 0x20)
				continue;
			if ((*(PULONG) (c + 0x10) & ~0xfu) != bar)
				continue;
			hc->pci_bus = bus;
			hc->pci_slot = sn.u.AsULONG;
			logf("kexehci: PCI %u:%u.%u, %04x:%04x\n", bus, slot >> 3, slot & 7, *(PUSHORT) c,
			     *(PUSHORT) (c + 2));
			return 1;
		}
	return 0;
}

/* take the controller from the BIOS (EHCI 1.0 section 5.1, USBLEGSUP) */
static void bios_handoff(HC *hc)
{
	ULONG eecp = (hc->hcc >> 8) & 0xff;
	int guard = 0;
	while (eecp >= 0x40 && guard++ < 16)
	{
		ULONG cap = cfg_r32(hc, eecp);
		if ((cap & 0xff) == 1)
		{
			int i;
			if (cap & (1u << 16))
				logf("kexehci: BIOS owns the controller, asking for it\n");
			cfg_w(hc, eecp + 3, 1, 1);
			for (i = 0; i < 100 && (cfg_r32(hc, eecp) & (1u << 16)); i++)
				delay_ms(10);
			if (cfg_r32(hc, eecp) & (1u << 16))
			{
				logf("kexehci: BIOS did not let go in 1 s, taking it\n");
				cfg_w(hc, eecp + 2, 0, 1);
			}
			/* no more SMIs from the controller; clear the SMI events */
			cfg_w(hc, eecp + 4, 0xe0000000u, 4);
		}
		eecp = (cap >> 8) & 0xff;
	}
}

/* ---- descriptor pools ------------------------------------------------------------------- */
static int td_alloc(HC *hc)
{
	int t = hc->td_free;
	if (t < 0)
		return -1;
	hc->td_free = hc->tdsw[t].next;
	hc->td_nfree--;
	hc->tdsw[t].next = -1;
	memset((void *) TD(hc, t), 0, sizeof(QTD));
	return t;
}

static void td_free(HC *hc, int t)
{
	TD(hc, t)->token = 0;
	hc->tdsw[t].next = hc->td_free;
	hc->td_free = t;
	hc->td_nfree++;
}

static int qh_alloc(HC *hc)
{
	int i;
	for (i = QH_FIRST_FREE; i < NQH; i++)
		if (!hc->qh_used[i])
		{
			hc->qh_used[i] = 1;
			memset((void *) QH(hc, i), 0, sizeof(QHW));
			return i;
		}
	return -1;
}

static int td_index(HC *hc, ULONG pa)
{
	pa &= ~0x1fu;
	if (pa < hc->td_pa || pa >= hc->td_pa + (ULONG) hc->ntd * sizeof(QTD))
		return -1;
	return (int) ((pa - hc->td_pa) / sizeof(QTD));
}

/* ---- endpoints and the schedules ------------------------------------------------------- */
static void qh_setup(HC *hc, EP *ep)
{
	DEV *d = ep->dev;
	QHW *h = QH(hc, ep->qh);
	ULONG ch, caps;
	ch = (ULONG) d->addr | ((ULONG) (ep->ea & 0x0f) << 8) | ((ULONG) d->speed << 12) | ((ULONG) ep->maxpkt << 16);
	caps = (ULONG) (ep->mult ? ep->mult : 1) << 30;
	if (ep->type == USB_ENDPOINT_TYPE_CONTROL)
		ch |= 1u << 14;   /* data toggle from the qTD */
	if (d->speed != SPEED_HIGH)
	{
		caps |= ((ULONG) d->tt_hub << 16) | ((ULONG) d->tt_port << 23);
		if (ep->type == USB_ENDPOINT_TYPE_CONTROL)
			ch |= 1u << 27;
	}
	if (ep->type == USB_ENDPOINT_TYPE_INTERRUPT)
	{
		if (d->speed == SPEED_HIGH)
		{
			int uf = 1 << ((ep->interval ? ep->interval : 1) - 1);
			caps |= uf >= 8 ? 0x01 : uf == 4 ? 0x11 : uf == 2 ? 0x55 : 0xff;
		}
		else
			caps |= 0x01 | (0x1c << 8);   /* start split in uframe 0, complete splits in 2..4 */
	}
	else
		ch |= (d->speed == SPEED_HIGH ? 4u : 0u) << 28;   /* NAK reload: HS async only */
	h->epchar = ch;
	h->epcaps = caps;
}

static int period_of(EP *ep)
{
	int p;
	if (ep->dev->speed == SPEED_HIGH)
	{
		int uf = 1 << ((ep->interval ? (ep->interval > 16 ? 16 : ep->interval) : 1) - 1);
		p = uf / 8;
		if (p < 1)
			p = 1;
	}
	else
		p = ep->interval ? ep->interval : 1;
	{
		int q = 1;
		while (q * 2 <= p && q < MAX_PERIOD)
			q *= 2;
		return q;
	}
}

static int level_of(int period)
{
	int l = 0;
	while ((1 << l) < period)
		l++;
	return l;
}

/* hc->lock held */
static void sched_link(HC *hc, EP *ep)
{
	QHW *h = QH(hc, ep->qh);
	if (ep->linked)
		return;
	if (ep->type == USB_ENDPOINT_TYPE_INTERRUPT)
	{
		int a = QH_ANCHOR(ep->period, ep->phase);
		QHW *an = QH(hc, a);
		h->hlink = an->hlink;
		WMB();
		an->hlink = QHPA(hc, ep->qh) | LP_QH;
		InsertHeadList(&hc->anchor_list[a], &ep->sched);
	}
	else
	{
		QHW *hd = QH(hc, QH_ASYNC_HEAD);
		h->hlink = hd->hlink;
		WMB();
		hd->hlink = QHPA(hc, ep->qh) | LP_QH;
		InsertHeadList(&hc->async_list, &ep->sched);
	}
	ep->linked = 1;
}

/* hc->lock held; the caller waits (IAA or a frame) before touching the QH */
static void sched_unlink(HC *hc, EP *ep)
{
	QHW *h = QH(hc, ep->qh), *prev;
	LIST_ENTRY *head;
	int anchor;
	if (!ep->linked)
		return;
	if (ep->type == USB_ENDPOINT_TYPE_INTERRUPT)
	{
		anchor = QH_ANCHOR(ep->period, ep->phase);
		head = &hc->anchor_list[anchor];
	}
	else
	{
		anchor = QH_ASYNC_HEAD;
		head = &hc->async_list;
	}
	if (ep->sched.Blink == head)
		prev = QH(hc, anchor);
	else
		prev = QH(hc, CONTAINING_RECORD(ep->sched.Blink, EP, sched)->qh);
	prev->hlink = h->hlink;
	RemoveEntryList(&ep->sched);
	ep->linked = 0;
}

/* hc->lock held */
static EP *ep_create(HC *hc, DEV *d, UCHAR ea, UCHAR type, USHORT wmax, UCHAR interval)
{
	EP *ep = (EP *) zalloc(sizeof(EP));
	QHW *h;
	if (!ep)
		return NULL;
	ep->qh = qh_alloc(hc);
	ep->dummy = td_alloc(hc);
	if (ep->qh < 0 || ep->dummy < 0)
	{
		if (ep->qh >= 0)
			hc->qh_used[ep->qh] = 0;
		if (ep->dummy >= 0)
			td_free(hc, ep->dummy);
		ExFreePool(ep);
		return NULL;
	}
	ep->dev = d;
	ep->ea = ea;
	ep->type = type;
	ep->wmax = wmax;
	ep->maxpkt = wmax & 0x7ff;
	ep->mult = d->speed == SPEED_HIGH && type == USB_ENDPOINT_TYPE_INTERRUPT ? (UCHAR) (1 + ((wmax >> 11) & 3)) : 1;
	ep->interval = interval;
	InitializeListHead(&ep->xfers);
	if (type == USB_ENDPOINT_TYPE_INTERRUPT)
	{
		int l;
		ep->period = period_of(ep);
		l = level_of(ep->period);
		ep->phase = hc->phase_rr[l]++ % ep->period;
	}
	TD(hc, ep->dummy)->next = LP_T;
	TD(hc, ep->dummy)->alt = LP_T;
	h = QH(hc, ep->qh);
	qh_setup(hc, ep);
	h->next = TDPA(hc, ep->dummy);
	h->alt = LP_T;
	h->token = 0;
	InsertTailList(&hc->eps, &ep->all);
	sched_link(hc, ep);
	return ep;
}

/* ---- transfers --------------------------------------------------------------------------- */
static ULONG buf_pa(XFER *x, ULONG off)
{
	if (x->mdl)
	{
		ULONG o = x->bo + off;
		PULONG pfn = (PULONG) (x->mdl + 1);
		return (pfn[o >> 12] << 12) | (o & 0xfff);
	}
	return MmGetPhysicalAddress(x->va + off).LowPart;
}

static void td_data(HC *hc, int t, XFER *x, ULONG off, ULONG len)
{
	QTD *q = TD(hc, t);
	int k;
	q->buf[0] = len ? buf_pa(x, off) : 0;
	for (k = 1; k < 5; k++)
	{
		ULONG p = ((((x->bo + off) >> 12) + (ULONG) k) << 12) - x->bo;
		q->buf[k] = p < off + len ? buf_pa(x, p) : 0;
		q->bufhi[k] = 0;
	}
	q->bufhi[0] = 0;
	hc->tdsw[t].len = len;
	hc->tdsw[t].role = ROLE_DATA;
}

static ULONG chunk_len(XFER *x, ULONG off, ULONG rem, USHORT maxpkt)
{
	ULONG max = 0x5000 - ((x->bo + off) & 0xfff);
	if (rem <= max)
		return rem;
	return max - max % maxpkt;
}

static int tds_needed(XFER *x, USHORT maxpkt)
{
	ULONG off = 0;
	int n = 0;
	if (!x->len)
		return 1;
	while (off < x->len)
	{
		off += chunk_len(x, off, x->len - off, maxpkt);
		n++;
	}
	return n;
}

/* hc->lock held. The endpoint's dummy qTD becomes the first qTD and a new
 * dummy ends the chain; the first qTD is activated last, which appends the
 * whole transfer atomically. */
static USBD_STATUS xfer_submit(HC *hc, EP *ep, XFER *x, int control)
{
	int ndata = tds_needed(x, ep->maxpkt), n = ndata + (control ? 2 : 0), i, nd, t[1024 / 4];
	ULONG off = 0, first_token = 0;
	int toggle = 1;
	if (n > (int) (sizeof(t) / sizeof(t[0])) || hc->td_nfree < n)
		return USBD_STATUS_INSUFFICIENT_RESOURCES;
	t[0] = ep->dummy;
	for (i = 1; i < n; i++)
		t[i] = td_alloc(hc);
	nd = td_alloc(hc);
	TD(hc, nd)->next = LP_T;
	TD(hc, nd)->alt = LP_T;
	TD(hc, nd)->token = 0;
	x->ep = ep;
	x->actual = 0;
	x->short_seen = 0;
	x->status = USBD_STATUS_SUCCESS;
	for (i = 0; i < n; i++)
	{
		QTD *q = TD(hc, t[i]);
		ULONG next = i + 1 < n ? TDPA(hc, t[i + 1]) : TDPA(hc, nd);
		ULONG tok;
		if (control && i == 0)
		{
			memcpy((void *) q->setup, x->setup, 8);
			q->buf[0] = TDPA(hc, t[i]) + 56;
			q->buf[1] = q->buf[2] = q->buf[3] = q->buf[4] = 0;
			hc->tdsw[t[i]].len = 8;
			hc->tdsw[t[i]].role = ROLE_SETUP;
			q->alt = LP_T;
			tok = (8u << 16) | TK_CERR3 | (TK_PID_SETUP << 8) | TK_ACTIVE;
		}
		else if (control && i == n - 1)
		{
			q->buf[0] = q->buf[1] = q->buf[2] = q->buf[3] = q->buf[4] = 0;
			hc->tdsw[t[i]].len = 0;
			hc->tdsw[t[i]].role = ROLE_STATUS;
			q->alt = LP_T;
			tok = TK_TOGGLE | TK_IOC | TK_CERR3 | ((x->in && x->len ? TK_PID_OUT : TK_PID_IN) << 8) | TK_ACTIVE;
		}
		else
		{
			ULONG len;
			if (control && !x->len)
			{
				/* no data stage: the setup is followed by the status qTD; this one is unused */
				continue;
			}
			len = chunk_len(x, off, x->len - off, ep->maxpkt);
			td_data(hc, t[i], x, off, len);
			off += len;
			tok = (len << 16) | TK_CERR3 | ((x->in ? TK_PID_IN : TK_PID_OUT) << 8) | TK_ACTIVE;
			if (control)
			{
				ULONG pk = (len + ep->maxpkt - 1) / ep->maxpkt;
				if (toggle)
					tok |= TK_TOGGLE;
				toggle ^= (int) (pk & 1);
				q->alt = x->in ? TDPA(hc, t[n - 1]) : LP_T;   /* short IN: on to the status stage */
			}
			else
			{
				q->alt = x->in ? TDPA(hc, nd) : LP_T;
				if (i == n - 1)
					tok |= TK_IOC;
			}
		}
		q->next = next;
		if (i == 0)
			first_token = tok;
		else
			q->token = tok;
		hc->tdsw[t[i]].next = i + 1 < n ? t[i + 1] : -1;
	}
	if (control && !x->len)
	{
		/* drop the data placeholder: setup -> status */
		int unused = t[1];
		TD(hc, t[0])->next = TDPA(hc, t[2]);
		hc->tdsw[t[0]].next = t[2];
		td_free(hc, unused);
	}
	x->first = t[0];
	x->last = t[n - 1];
	x->end = nd;
	ep->dummy = nd;
	InsertTailList(&ep->xfers, &x->link);
	WMB();
	TD(hc, t[0])->token = first_token;
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS token_error(ULONG tok)
{
	if (tok & TK_BABBLE)
		return USBD_STATUS_BABBLE_DETECTED;
	if (tok & TK_DBERR)
		return USBD_STATUS_DATA_BUFFER_ERROR;
	if (tok & TK_XACT)
		return USBD_STATUS_DEV_NOT_RESPONDING;
	return USBD_STATUS_STALL_PID;
}

/* 1 when the transfer is finished (status and actual set) */
static int xfer_check(HC *hc, XFER *x)
{
	int t;
	ULONG actual = 0;
	for (t = x->first; t >= 0; t = hc->tdsw[t].next)
	{
		ULONG tok = TD(hc, t)->token, rem;
		int role = hc->tdsw[t].role;
		if (tok & TK_ACTIVE)
		{
			if (x->short_seen && role == ROLE_DATA)
				continue;   /* skipped after a short packet */
			return 0;
		}
		if (tok & TK_HALTED)
		{
			x->status = token_error(tok);
			x->actual = actual;
			return 1;
		}
		rem = (tok >> 16) & 0x7fff;
		if (role == ROLE_DATA)
		{
			actual += hc->tdsw[t].len - rem;
			if (rem && x->in)
			{
				if (hc->tdsw[x->last].role == ROLE_STATUS)
				{
					x->short_seen = 1;
					continue;
				}
				break;
			}
		}
	}
	x->actual = actual;
	x->status = USBD_STATUS_SUCCESS;
	return 1;
}

static void xfer_free_tds(HC *hc, XFER *x)
{
	int t = x->first;
	while (t >= 0)
	{
		int n = hc->tdsw[t].next;
		td_free(hc, t);
		t = n;
	}
	x->first = -1;
}

static ULONG ep_resume_pa(HC *hc, EP *ep)
{
	if (!IsListEmpty(&ep->xfers))
		return TDPA(hc, CONTAINING_RECORD(ep->xfers.Flink, XFER, link)->first);
	return TDPA(hc, ep->dummy);
}

/* hc->lock held: move finished transfers to done */
static void ep_scan(HC *hc, EP *ep, LIST_ENTRY *done)
{
	while (!IsListEmpty(&ep->xfers))
	{
		XFER *x = CONTAINING_RECORD(ep->xfers.Flink, XFER, link);
		if (x->cancel || ep->halted)
			break;
		if (!xfer_check(hc, x))
			break;
		if (x->irp && IoSetCancelRoutine(x->irp, NULL) == NULL)
		{
			/* being cancelled right now: the hub thread completes it */
			x->cancel = 1;
			x->cstatus = USBD_STATUS_CANCELED;
			ep->scrub = 1;
			hc->scrub_pending = 1;
			KeSetEvent(&hc->ev, 0, FALSE);
			break;
		}
		RemoveEntryList(&x->link);
		xfer_free_tds(hc, x);
		if (x->status != USBD_STATUS_SUCCESS)
		{
			QHW *h = QH(hc, ep->qh);
			if (ep->type == USB_ENDPOINT_TYPE_CONTROL)
			{
				/* a stall on a control pipe ends with the next setup: restart the queue */
				h->next = ep_resume_pa(hc, ep);
				h->alt = LP_T;
				WMB();
				h->token = 0;
			}
			else
				ep->halted = 1;   /* until RESET_PIPE */
		}
		InsertTailList(done, &x->link);
	}
}

static NTSTATUS nt_status(USBD_STATUS us)
{
	if (USBD_SUCCESS(us))
		return STATUS_SUCCESS;
	if (us == USBD_STATUS_CANCELED)
		return STATUS_CANCELLED;
	if (us == USBD_STATUS_DEVICE_GONE)
		return STATUS_DEVICE_NOT_CONNECTED;
	if (us == USBD_STATUS_INSUFFICIENT_RESOURCES)
		return STATUS_INSUFFICIENT_RESOURCES;
	if (us == USBD_STATUS_NOT_SUPPORTED)
		return STATUS_NOT_SUPPORTED;
	if (us == USBD_STATUS_INVALID_PARAMETER || us == USBD_STATUS_INVALID_PIPE_HANDLE)
		return STATUS_INVALID_PARAMETER;
	return STATUS_UNSUCCESSFUL;
}

/* outside hc->lock */
static void complete_list(LIST_ENTRY *done)
{
	while (!IsListEmpty(done))
	{
		XFER *x = CONTAINING_RECORD(RemoveHeadList(done), XFER, link);
		if (x->irp)
		{
			PIRP irp = x->irp;
			PURB urb = x->urb;
			urb->UrbHeader.Status = x->status;
			urb->UrbControlTransfer.TransferBufferLength = x->actual;   /* same place in every transfer URB */
			irp->IoStatus.Status = nt_status(x->status);
			irp->IoStatus.Information = 0;
			ExFreePool(x);
			IoCompleteRequest(irp, IO_NO_INCREMENT);
		}
		else
		{
			x->finished = 1;
			if (x->done)
				x->done(x);
		}
	}
}

static VOID NTAPI hc_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
	HC *hc = (HC *) ctx;
	LONG sts = InterlockedExchange(&hc->pending_sts, 0);
	LIST_ENTRY done, *e;
	(void) dpc;
	(void) a1;
	(void) a2;
	InitializeListHead(&done);
	if (sts & STS_FLR)
		hc->frame_hi++;
	if (sts & (STS_USBINT | STS_ERRINT))
	{
		KeAcquireSpinLockAtDpcLevel(&hc->lock);
		for (e = hc->eps.Flink; e != &hc->eps; e = e->Flink)
			ep_scan(hc, CONTAINING_RECORD(e, EP, all), &done);
		KeReleaseSpinLockFromDpcLevel(&hc->lock);
		complete_list(&done);
	}
	if (sts & STS_IAA)
		KeSetEvent(&hc->iaa, 0, FALSE);
	if (sts & STS_PCD)
		KeSetEvent(&hc->ev, 0, FALSE);
	if (sts & STS_HSE)
		logf("kexehci: host system error, controller halted\n");
}

static BOOLEAN NTAPI hc_isr(PKINTERRUPT in, PVOID ctx)
{
	HC *hc = (HC *) ctx;
	ULONG sts;
	(void) in;
	if (!hc->op)
		return FALSE;
	sts = op_r(hc, OP_USBSTS) & STS_INTMASK & op_r(hc, OP_USBINTR);
	if (!sts || sts == STS_INTMASK)
		return FALSE;
	op_w(hc, OP_USBSTS, sts);
	InterlockedOr(&hc->pending_sts, (LONG) sts);
	KeInsertQueueDpc(&hc->dpc, NULL, NULL);
	return TRUE;
}

/* ---- the hub thread: quiescing endpoints, cancelling, closing ---------------------------- */
static int hc_alive(HC *hc)
{
	return hc->op && !(op_r(hc, OP_USBSTS) & STS_HALTED);
}

/* wait until the controller no longer uses unlinked QHs */
static void quiesce_wait(HC *hc, int async, int periodic)
{
	if (!hc_alive(hc))
		return;
	if (async)
	{
		LARGE_INTEGER t;
		t.QuadPart = -50 * 10000;
		KeResetEvent(&hc->iaa);
		op_w(hc, OP_USBCMD, op_r(hc, OP_USBCMD) | CMD_IAAD);
		if (KeWaitForSingleObject(&hc->iaa, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT)
			logf("kexehci: no async advance in 50 ms\n");
	}
	if (periodic)
		delay_ms(2);   /* the frame in progress ends: periodic QHs are read once per frame */
}

/* hc->lock held, endpoint unlinked and quiet: take out cancelled transfers,
 * relink the rest */
static void ep_cleanup(HC *hc, EP *ep, LIST_ENTRY *done)
{
	QHW *h = QH(hc, ep->qh);
	LIST_ENTRY *e, *nx;
	int cur = td_index(hc, h->cur), cur_cancelled = 0;
	ep_scan(hc, ep, done);
	for (e = ep->xfers.Flink; e != &ep->xfers; e = nx)
	{
		XFER *x = CONTAINING_RECORD(e, XFER, link);
		int t;
		nx = e->Flink;
		if (!x->cancel && !ep->closing)
			continue;
		for (t = x->first; t >= 0; t = hc->tdsw[t].next)
			if (t == cur)
				cur_cancelled = 1;
		RemoveEntryList(&x->link);
		if (x->irp)
			IoSetCancelRoutine(x->irp, NULL);
		xfer_free_tds(hc, x);
		x->status = x->cancel ? x->cstatus : USBD_STATUS_CANCELED;
		x->actual = 0;
		InsertTailList(done, &x->link);
	}
	/* relink the remaining transfers: each one ends where the next one starts */
	for (e = ep->xfers.Flink; e != &ep->xfers; e = e->Flink)
	{
		XFER *x = CONTAINING_RECORD(e, XFER, link);
		int end = e->Flink != &ep->xfers ? CONTAINING_RECORD(e->Flink, XFER, link)->first : ep->dummy;
		int t;
		x->end = end;
		TD(hc, x->last)->next = TDPA(hc, end);
		if (hc->tdsw[x->last].role != ROLE_STATUS && x->in)
			for (t = x->first; t >= 0; t = hc->tdsw[t].next)
				TD(hc, t)->alt = TDPA(hc, end);
		for (t = x->first; t >= 0; t = hc->tdsw[t].next)
			if (t == cur && t == x->last)
			{
				h->next = TDPA(hc, end);
				if (hc->tdsw[t].role != ROLE_STATUS && x->in)
					h->alt = (h->alt & 0x1e) | TDPA(hc, end);
			}
	}
	if (!ep->halted && !ep->closing)
	{
		ULONG tok = h->token;
		if (!(tok & TK_ACTIVE) || cur_cancelled)
		{
			h->next = ep_resume_pa(hc, ep);
			h->alt = LP_T;
			WMB();
			h->token = tok & TK_TOGGLE;
		}
	}
	ep->scrub = 0;
}

/* thread: handle every endpoint marked for scrubbing */
static void do_scrubs(HC *hc)
{
	KIRQL old;
	LIST_ENTRY *e, done;
	int async = 0, periodic = 0;
	InitializeListHead(&done);
	KeAcquireSpinLock(&hc->lock, &old);
	if (!hc->scrub_pending)
	{
		KeReleaseSpinLock(&hc->lock, old);
		return;
	}
	hc->scrub_pending = 0;
	for (e = hc->eps.Flink; e != &hc->eps; e = e->Flink)
	{
		EP *ep = CONTAINING_RECORD(e, EP, all);
		if (!ep->scrub || ep->closing)
			continue;
		if (ep->type == USB_ENDPOINT_TYPE_INTERRUPT)
			periodic = 1;
		else
			async = 1;
		sched_unlink(hc, ep);
	}
	KeReleaseSpinLock(&hc->lock, old);
	if (!async && !periodic)
		return;
	quiesce_wait(hc, async, periodic);
	KeAcquireSpinLock(&hc->lock, &old);
	for (e = hc->eps.Flink; e != &hc->eps; e = e->Flink)
	{
		EP *ep = CONTAINING_RECORD(e, EP, all);
		if (!ep->scrub || ep->closing)
			continue;
		ep_cleanup(hc, ep, &done);
		sched_link(hc, ep);
	}
	KeReleaseSpinLock(&hc->lock, old);
	complete_list(&done);
}

/* thread: cancel everything on the endpoint and free it */
static void ep_close(HC *hc, EP *ep, USBD_STATUS why)
{
	KIRQL old;
	LIST_ENTRY done, *e;
	if (!ep)
		return;
	InitializeListHead(&done);
	KeAcquireSpinLock(&hc->lock, &old);
	for (e = ep->xfers.Flink; e != &ep->xfers; e = e->Flink)
	{
		XFER *x = CONTAINING_RECORD(e, XFER, link);
		if (!x->cancel)
		{
			x->cancel = 1;
			x->cstatus = why;
		}
	}
	ep->closing = 1;
	sched_unlink(hc, ep);
	KeReleaseSpinLock(&hc->lock, old);
	quiesce_wait(hc, ep->type != USB_ENDPOINT_TYPE_INTERRUPT, ep->type == USB_ENDPOINT_TYPE_INTERRUPT);
	KeAcquireSpinLock(&hc->lock, &old);
	ep_cleanup(hc, ep, &done);
	RemoveEntryList(&ep->all);
	td_free(hc, ep->dummy);
	hc->qh_used[ep->qh] = 0;
	KeReleaseSpinLock(&hc->lock, old);
	complete_list(&done);
	ExFreePool(ep);
}

/* thread: reset the host side of an endpoint (data toggle 0, halt cleared) */
static void ep_reset(HC *hc, EP *ep)
{
	KIRQL old;
	LIST_ENTRY done;
	QHW *h = QH(hc, ep->qh);
	InitializeListHead(&done);
	KeAcquireSpinLock(&hc->lock, &old);
	sched_unlink(hc, ep);
	KeReleaseSpinLock(&hc->lock, old);
	quiesce_wait(hc, ep->type != USB_ENDPOINT_TYPE_INTERRUPT, ep->type == USB_ENDPOINT_TYPE_INTERRUPT);
	KeAcquireSpinLock(&hc->lock, &old);
	ep->halted = 0;
	ep_cleanup(hc, ep, &done);
	if (!(h->token & TK_ACTIVE))
	{
		h->next = ep_resume_pa(hc, ep);
		h->alt = LP_T;
		WMB();
		h->token = 0;
	}
	sched_link(hc, ep);
	KeReleaseSpinLock(&hc->lock, old);
	complete_list(&done);
}

/* ---- synchronous control transfers for the hub thread ------------------------------------ */
static void sync_done(XFER *x)
{
	KeSetEvent(&x->ev, 0, FALSE);
}

static USBD_STATUS ctrl_ep(HC *hc, EP *ep, UCHAR rt, UCHAR req, USHORT val, USHORT idx, void *buf, USHORT len,
                           ULONG *actual, ULONG timeout_ms)
{
	XFER *x = (XFER *) zalloc(sizeof(XFER));
	KIRQL old;
	USBD_STATUS st;
	ULONG t0 = now_ms();
	if (!x)
		return USBD_STATUS_INSUFFICIENT_RESOURCES;
	if (!ep || ep->dev->gone)
	{
		ExFreePool(x);
		return USBD_STATUS_DEVICE_GONE;
	}
	KeInitializeEvent(&x->ev, NotificationEvent, FALSE);
	x->setup[0] = rt;
	x->setup[1] = req;
	x->setup[2] = (UCHAR) val;
	x->setup[3] = (UCHAR) (val >> 8);
	x->setup[4] = (UCHAR) idx;
	x->setup[5] = (UCHAR) (idx >> 8);
	x->setup[6] = (UCHAR) len;
	x->setup[7] = (UCHAR) (len >> 8);
	x->va = (UCHAR *) buf;
	x->bo = (ULONG) (ULONG_PTR) buf & 0xfff;
	x->len = len;
	x->in = (rt & 0x80) != 0;
	x->done = sync_done;
	KeAcquireSpinLock(&hc->lock, &old);
	st = xfer_submit(hc, ep, x, 1);
	KeReleaseSpinLock(&hc->lock, old);
	if (st != USBD_STATUS_SUCCESS)
	{
		ExFreePool(x);
		return st;
	}
	for (;;)
	{
		LARGE_INTEGER t;
		t.QuadPart = -20 * 10000;
		if (KeWaitForSingleObject(&x->ev, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT)
			break;
		do_scrubs(hc);
		if (now_ms() - t0 > timeout_ms && !x->cancel)
		{
			KeAcquireSpinLock(&hc->lock, &old);
			if (!x->finished)
			{
				x->cancel = 1;
				x->cstatus = USBD_STATUS_DEV_NOT_RESPONDING;
				ep->scrub = 1;
				hc->scrub_pending = 1;
			}
			KeReleaseSpinLock(&hc->lock, old);
			do_scrubs(hc);
		}
	}
	st = x->status;
	if (actual)
		*actual = x->actual;
	ExFreePool(x);
	return st;
}

static USBD_STATUS ctrl(DEV *d, UCHAR rt, UCHAR req, USHORT val, USHORT idx, void *buf, USHORT len, ULONG *actual)
{
	return ctrl_ep(d->hc, d->ep0, rt, req, val, idx, buf, len, actual, 5000);
}

/* ---- root ports and hubs ---------------------------------------------------------------- */
#define HUB_PORT_CONNECTION 0x0001
#define HUB_PORT_ENABLE 0x0002
#define HUB_PORT_LOW_SPEED 0x0200
#define HUB_PORT_HIGH_SPEED 0x0400
#define HUB_C_CONNECTION 16
#define HUB_C_ENABLE 17
#define HUB_C_SUSPEND 18
#define HUB_C_OVER_CURRENT 19
#define HUB_C_RESET 20
#define HUB_PORT_RESET 4
#define HUB_PORT_POWER 8

static DEV *enumerate(HC *hc, DEV *parent, int port, int speed);
static void dev_remove(HC *hc, DEV *d);

static void port_w(HC *hc, int p, ULONG set, ULONG clear)
{
	ULONG v = op_r(hc, OP_PORTSC(p));
	v &= ~(PS_RWC | clear);
	op_w(hc, OP_PORTSC(p), v | set);
}

static void root_release(HC *hc, int p, const char *why)
{
	if (!hc->ncc)
	{
		logf("kexehci: port %u: %s device, but this controller has no USB 1.1 companion\n", p + 1, why);
		return;
	}
	logf("kexehci: port %u: %s device, handed to the USB 1.1 controller\n", p + 1, why);
	port_w(hc, p, PS_OWNER, 0);
}

static void root_port(HC *hc, int p)
{
	ULONG s = op_r(hc, OP_PORTSC(p));
	int i;
	if (!hc->root_scan && !(s & PS_CSC))
		return;
	if ((s & PS_OWNER) && !(s & PS_CSC))
		return;
	port_w(hc, p, s & PS_CSC, 0);
	if (hc->root[p + 1])
	{
		logf("kexehci: port %u: disconnected\n", p + 1);
		dev_remove(hc, hc->root[p + 1]);
		hc->root[p + 1] = NULL;
	}
	if (!(s & PS_CCS) || (s & PS_OWNER))
		return;
	delay_ms(100);   /* debounce */
	s = op_r(hc, OP_PORTSC(p));
	if (!(s & PS_CCS))
		return;
	if ((s & PS_LS_MASK) == PS_LS_K)
	{
		root_release(hc, p, "low-speed");
		return;
	}
	port_w(hc, p, PS_PR, PS_PE);
	delay_ms(50);
	port_w(hc, p, 0, PS_PR);
	for (i = 0; i < 20 && (op_r(hc, OP_PORTSC(p)) & PS_PR); i++)
		delay_ms(1);
	delay_ms(10);
	s = op_r(hc, OP_PORTSC(p));
	port_w(hc, p, s & PS_CSC, 0);
	if (!(s & PS_CCS))
		return;
	if (!(s & PS_PE))
	{
		root_release(hc, p, "full-speed");
		return;
	}
	logf("kexehci: port %u: high-speed device\n", p + 1);
	hc->root[p + 1] = enumerate(hc, NULL, p + 1, SPEED_HIGH);
	if (!hc->root[p + 1])
		port_w(hc, p, 0, PS_PE);
}

static USBD_STATUS hub_status(DEV *h, int port, USHORT *st, USHORT *ch)
{
	UCHAR b[4];
	ULONG got = 0;
	USBD_STATUS r = ctrl(h, 0xa3, USB_REQUEST_GET_STATUS, 0, (USHORT) port, b, 4, &got);
	if (USBD_SUCCESS(r) && got < 4)
		r = USBD_STATUS_ERROR_SHORT_TRANSFER;
	*st = (USHORT) (b[0] | (b[1] << 8));
	*ch = (USHORT) (b[2] | (b[3] << 8));
	return r;
}

static void hub_feature(DEV *h, int set, USHORT feature, int port)
{
	ctrl(h, 0x23, set ? USB_REQUEST_SET_FEATURE : USB_REQUEST_CLEAR_FEATURE, feature, (USHORT) port, NULL, 0, NULL);
}

/* reset a hub port; returns the speed of the device, or -1 */
static int hub_reset(DEV *h, int port)
{
	USHORT st = 0, ch = 0;
	int i;
	hub_feature(h, 1, HUB_PORT_RESET, port);
	for (i = 0; i < 50; i++)
	{
		delay_ms(10);
		if (!USBD_SUCCESS(hub_status(h, port, &st, &ch)))
			return -1;
		if (ch & 0x10)
			break;
	}
	hub_feature(h, 0, HUB_C_RESET, port);
	if (!(st & HUB_PORT_ENABLE) || !(st & HUB_PORT_CONNECTION))
		return -1;
	delay_ms(10);
	return st & HUB_PORT_LOW_SPEED ? SPEED_LOW : st & HUB_PORT_HIGH_SPEED ? SPEED_HIGH : SPEED_FULL;
}

static void hub_port(HC *hc, DEV *h, int port)
{
	USHORT st, ch;
	int speed;
	if (!USBD_SUCCESS(hub_status(h, port, &st, &ch)))
		return;
	if (ch & 0x02)
		hub_feature(h, 0, HUB_C_ENABLE, port);
	if (ch & 0x04)
		hub_feature(h, 0, HUB_C_SUSPEND, port);
	if (ch & 0x08)
	{
		hub_feature(h, 0, HUB_C_OVER_CURRENT, port);
		logf("kexehci: hub %s port %u: over-current\n", h->path, port);
	}
	if (ch & 0x10)
		hub_feature(h, 0, HUB_C_RESET, port);
	if (!(ch & 0x01))
		return;
	hub_feature(h, 0, HUB_C_CONNECTION, port);
	if (h->child[port])
	{
		logf("kexehci: hub %s port %u: disconnected\n", h->path, port);
		dev_remove(hc, h->child[port]);
		h->child[port] = NULL;
	}
	if (!(st & HUB_PORT_CONNECTION))
		return;
	delay_ms(100);
	speed = hub_reset(h, port);
	if (speed < 0)
	{
		logf("kexehci: hub %s port %u: reset failed\n", h->path, port);
		return;
	}
	logf("kexehci: hub %s port %u: %s-speed device\n", h->path, port,
	     speed == SPEED_HIGH ? "high" : speed == SPEED_FULL ? "full" : "low");
	h->child[port] = enumerate(hc, h, port, speed);
}

static void hub_status_done(XFER *x)
{
	DEV *h = (DEV *) x->ctx;
	if (x->status == USBD_STATUS_SUCCESS)
	{
		LONG bits = 0;
		ULONG i;
		for (i = 0; i < x->actual && i < 4; i++)
			bits |= (LONG) h->hub_buf[i] << (8 * i);
		InterlockedOr(&h->hub_change, bits);
	}
	else if (x->status != USBD_STATUS_CANCELED && x->status != USBD_STATUS_DEVICE_GONE)
		InterlockedOr(&h->hub_change, 0x10000);   /* error: poll all ports */
	InterlockedExchange(&h->hub_pending, 0);
	KeSetEvent(&h->hc->ev, 0, FALSE);
}

static void hub_submit(HC *hc, DEV *h)
{
	KIRQL old;
	XFER *x = h->hub_x;
	if (h->gone || !h->hub_ep || InterlockedExchange(&h->hub_pending, 1))
		return;
	memset(x, 0, sizeof(*x));
	x->va = h->hub_buf;
	x->bo = (ULONG) (ULONG_PTR) h->hub_buf & 0xfff;
	x->len = (ULONG) (h->nports / 8 + 1);
	x->in = 1;
	x->done = hub_status_done;
	x->ctx = h;
	KeAcquireSpinLock(&hc->lock, &old);
	if (h->hub_ep->halted)
	{
		KeReleaseSpinLock(&hc->lock, old);
		ep_reset(hc, h->hub_ep);
		ctrl(h, 0x02, USB_REQUEST_CLEAR_FEATURE, 0, h->hub_ep->ea, NULL, 0, NULL);
		KeAcquireSpinLock(&hc->lock, &old);
	}
	if (xfer_submit(hc, h->hub_ep, x, 0) != USBD_STATUS_SUCCESS)
		InterlockedExchange(&h->hub_pending, 0);
	KeReleaseSpinLock(&hc->lock, old);
}

static int hub_init(HC *hc, DEV *h)
{
	UCHAR d[16];
	ULONG got = 0, off;
	int p;
	KIRQL old;
	if (!USBD_SUCCESS(ctrl(h, 0x00, USB_REQUEST_SET_CONFIGURATION, h->cfgval, 0, NULL, 0, NULL)))
		return 0;
	if (!USBD_SUCCESS(ctrl(h, 0xa0, USB_REQUEST_GET_DESCRIPTOR, 0x2900, 0, d, sizeof(d), &got)) || got < 7)
		return 0;
	h->nports = d[2] > MAX_HUB_PORTS ? MAX_HUB_PORTS : d[2];
	h->pwrgood_ms = d[5] * 2;
	h->is_hub = 1;
	/* the status-change endpoint: the first interrupt IN endpoint */
	for (off = 0; off + 2 <= h->cfglen && h->cfg[off]; off += h->cfg[off])
	{
		PUSB_ENDPOINT_DESCRIPTOR e = (PUSB_ENDPOINT_DESCRIPTOR) (h->cfg + off);
		if (e->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE && (e->bmAttributes & 3) == USB_ENDPOINT_TYPE_INTERRUPT &&
		    (e->bEndpointAddress & 0x80))
		{
			KeAcquireSpinLock(&hc->lock, &old);
			h->hub_ep = ep_create(hc, h, e->bEndpointAddress, USB_ENDPOINT_TYPE_INTERRUPT, e->wMaxPacketSize,
			                      e->bInterval);
			KeReleaseSpinLock(&hc->lock, old);
			break;
		}
	}
	h->hub_x = (XFER *) zalloc(sizeof(XFER));
	if (!h->hub_ep || !h->hub_x)
		return 0;
	logf("kexehci: hub %s: %u ports%s\n", h->path, h->nports,
	     h->speed == SPEED_HIGH ? (h->dd.bDeviceProtocol == 2 ? ", USB 2.0, multi-TT" : ", USB 2.0") : "");
	for (p = 1; p <= h->nports; p++)
		hub_feature(h, 1, HUB_PORT_POWER, p);
	delay_ms((ULONG) h->pwrgood_ms + 100);
	h->hub_change = 0x10000;   /* look at every port once */
	hub_submit(hc, h);
	return 1;
}

static void hubs_service(HC *hc, DEV *d)
{
	int p;
	if (!d || d->gone)
		return;
	if (d->is_hub)
	{
		LONG ch = InterlockedExchange(&d->hub_change, 0);
		if (ch)
		{
			for (p = 1; p <= d->nports; p++)
				if ((ch & 0x10000) || (ch & (1 << p)))
					hub_port(hc, d, p);
			if (d->gone)
				return;
		}
		hub_submit(hc, d);
		for (p = 1; p <= d->nports; p++)
			hubs_service(hc, d->child[p]);
	}
}

/* ---- enumeration --------------------------------------------------------------------------- */
static int addr_alloc(HC *hc)
{
	int a;
	for (a = 1; a < 128; a++)
		if (!hc->addr_used[a])
		{
			hc->addr_used[a] = 1;
			return a;
		}
	return 0;
}

static void get_string(DEV *d, UCHAR idx, WCHAR *out, int max)
{
	UCHAR b[130];
	ULONG got = 0;
	int i, n;
	out[0] = 0;
	if (!idx || !d->langid)
		return;
	if (!USBD_SUCCESS(ctrl(d, 0x80, USB_REQUEST_GET_DESCRIPTOR, (USHORT) (0x0300 | idx), d->langid, b, sizeof(b), &got)) ||
	    got < 2)
		return;
	if (b[0] < got)
		got = b[0];
	n = (int) (got - 2) / 2;
	if (n > max - 1)
		n = max - 1;
	for (i = 0; i < n; i++)
		out[i] = (WCHAR) (b[2 + 2 * i] | (b[3 + 2 * i] << 8));
	out[n] = 0;
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = 0;
}

static NTSTATUS pdo_create(HC *hc, DEV *d);

static DEV *enumerate(HC *hc, DEV *parent, int port, int speed)
{
	DEV *d = (DEV *) zalloc(sizeof(DEV));
	KIRQL old;
	ULONG got = 0;
	UCHAR c9[9];
	USBD_STATUS st;
	int i, a;
	if (!d)
		return NULL;
	d->hc = hc;
	d->parent = parent;
	d->port = port;
	d->speed = speed;
	if (speed != SPEED_HIGH && parent)
	{
		if (parent->speed == SPEED_HIGH)
		{
			d->tt_hub = parent->addr;
			d->tt_port = port;
		}
		else
		{
			d->tt_hub = parent->tt_hub;
			d->tt_port = parent->tt_port;
		}
	}
	{
		/* path: "3" on root port 3, "3.2" behind a hub on it */
		char tmp[32];
		int n = 0, l = 0;
		DEV *x;
		for (x = d; x && n < 6; x = x->parent)
			tmp[n++] = (char) x->port;
		while (n--)
		{
			if (l)
				d->path[l++] = '.';
			if (tmp[n] >= 10)
				d->path[l++] = (char) ('0' + tmp[n] / 10);
			d->path[l++] = (char) ('0' + tmp[n] % 10);
		}
		d->path[l] = 0;
	}
	KeAcquireSpinLock(&hc->lock, &old);
	d->ep0 = ep_create(hc, d, 0, USB_ENDPOINT_TYPE_CONTROL, speed == SPEED_LOW ? 8 : 64, 0);
	KeReleaseSpinLock(&hc->lock, old);
	if (!d->ep0)
		goto fail;
	/* the first 8 bytes of the device descriptor tell the default pipe's packet size */
	for (i = 0; i < 3; i++)
	{
		st = ctrl(d, 0x80, USB_REQUEST_GET_DESCRIPTOR, 0x0100, 0, &d->dd, 8, &got);
		if (USBD_SUCCESS(st) && got >= 8)
			break;
		delay_ms(50);
	}
	if (i == 3)
	{
		logf("kexehci: %s: no device descriptor (%08x)\n", d->path, st);
		goto fail;
	}
	a = addr_alloc(hc);
	if (!a || !USBD_SUCCESS(ctrl(d, 0x00, USB_REQUEST_SET_ADDRESS, (USHORT) a, 0, NULL, 0, NULL)))
	{
		logf("kexehci: %s: SET_ADDRESS failed\n", d->path);
		if (a)
			hc->addr_used[a] = 0;
		goto fail;
	}
	delay_ms(10);
	KeAcquireSpinLock(&hc->lock, &old);
	d->addr = a;
	d->ep0->maxpkt = d->dd.bMaxPacketSize0 ? d->dd.bMaxPacketSize0 : 8;
	qh_setup(hc, d->ep0);   /* the QH is idle: a new address and packet size are safe */
	KeReleaseSpinLock(&hc->lock, old);
	if (!USBD_SUCCESS(ctrl(d, 0x80, USB_REQUEST_GET_DESCRIPTOR, 0x0100, 0, &d->dd, 18, &got)) || got < 18)
		goto fail;
	if (!USBD_SUCCESS(ctrl(d, 0x80, USB_REQUEST_GET_DESCRIPTOR, 0x0200, 0, c9, 9, &got)) || got < 9)
		goto fail;
	d->cfglen = c9[2] | (c9[3] << 8);
	if (d->cfglen < 9 || d->cfglen > 4096)
		goto fail;
	d->cfg = (UCHAR *) zalloc(d->cfglen);
	if (!d->cfg || !USBD_SUCCESS(ctrl(d, 0x80, USB_REQUEST_GET_DESCRIPTOR, 0x0200, 0, d->cfg, (USHORT) d->cfglen, &got)))
		goto fail;
	d->cfglen = got;
	d->cfgval = c9[5];
	{
		UCHAR l[4];
		if (USBD_SUCCESS(ctrl(d, 0x80, USB_REQUEST_GET_DESCRIPTOR, 0x0300, 0, l, 4, &got)) && got >= 4)
			d->langid = (USHORT) (l[2] | (l[3] << 8));
	}
	get_string(d, d->dd.iProduct, d->product, 64);
	get_string(d, d->dd.iSerialNumber, d->serial, 64);
	d->has_serial = d->serial[0] != 0;
	for (i = 0; d->serial[i]; i++)
		if (d->serial[i] <= 0x20 || d->serial[i] >= 0x7f || d->serial[i] == ',' || d->serial[i] == '\\')
			d->has_serial = 0;
	logf("kexehci: %s: address %u, %04x:%04x rev %04x class %02x, \"%S\"\n", d->path, a, d->dd.idVendor,
	     d->dd.idProduct, d->dd.bcdDevice, d->dd.bDeviceClass, d->product);
	if (d->dd.bDeviceClass == USB_DEVICE_CLASS_HUB)
	{
		if (!hub_init(hc, d))
		{
			logf("kexehci: %s: hub setup failed\n", d->path);
			goto fail;
		}
		return d;
	}
	if (!NT_SUCCESS(pdo_create(hc, d)))
		goto fail;
	return d;
fail:
	dev_remove(hc, d);
	return NULL;
}

static void dev_free(DEV *d)
{
	if (d->cfg)
		ExFreePool(d->cfg);
	if (d->hub_x)
		ExFreePool(d->hub_x);
	ExFreePool(d);
}

/* thread: the device is gone (or failed): close everything, then let PnP remove its PDO */
static void dev_remove(HC *hc, DEV *d)
{
	int i;
	if (!d)
		return;
#ifdef KEX_DEBUGCON
	logf("kexehci: %s: tearing down\n", d->path);
#endif
	ExAcquireFastMutex(&hc->tree);
	d->gone = 1;
	ExReleaseFastMutex(&hc->tree);
	for (i = 1; i <= d->nports; i++)
		if (d->child[i])
		{
			dev_remove(hc, d->child[i]);
			d->child[i] = NULL;
		}
	for (i = 0; i < 32; i++)
	{
		ep_close(hc, d->eps[i], USBD_STATUS_DEVICE_GONE);
		d->eps[i] = NULL;
	}
	ep_close(hc, d->hub_ep, USBD_STATUS_DEVICE_GONE);
	d->hub_ep = NULL;
	ep_close(hc, d->ep0, USBD_STATUS_DEVICE_GONE);
	d->ep0 = NULL;
	if (d->addr)
		hc->addr_used[d->addr] = 0;
#ifdef KEX_DEBUGCON
	logf("kexehci: %s: torn down\n", d->path);
#endif
	ExAcquireFastMutex(&hc->tree);
	d->torn_down = 1;
	if (d->pdo)
	{
		ExReleaseFastMutex(&hc->tree);
		if (!hc->stop)
			IoInvalidateDeviceRelations(hc->pdo, BusRelations);
		return;   /* freed with its PDO */
	}
	ExReleaseFastMutex(&hc->tree);
	dev_free(d);
}

/* ---- URBs that need the hub thread (configuration, pipes, ports) ------------------------- */
static PUSB_INTERFACE_DESCRIPTOR find_iface(UCHAR *cfg, ULONG len, int ifno, int alt, ULONG *at)
{
	ULONG off;
	for (off = 0; off + 2 <= len && cfg[off] >= 2; off += cfg[off])
	{
		PUSB_INTERFACE_DESCRIPTOR i = (PUSB_INTERFACE_DESCRIPTOR) (cfg + off);
		if (i->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE && i->bInterfaceNumber == ifno &&
		    i->bAlternateSetting == alt)
		{
			*at = off;
			return i;
		}
	}
	return NULL;
}

static int ep_slot(UCHAR ea)
{
	return (ea & 0x0f) | (ea & 0x80 ? 16 : 0);
}

/* open the pipes of one interface setting and describe them to the caller */
static USBD_STATUS open_iface(HC *hc, DEV *d, UCHAR *cfg, ULONG cfglen, PUSBD_INTERFACE_INFORMATION ii, ULONG room)
{
	ULONG at, off;
	PUSB_INTERFACE_DESCRIPTOR id = find_iface(cfg, cfglen, ii->InterfaceNumber, ii->AlternateSetting, &at);
	ULONG n = 0, need;
	KIRQL old;
	if (!id)
		return USBD_STATUS_INVALID_PARAMETER;
	need = sizeof(USBD_INTERFACE_INFORMATION) +
	       (id->bNumEndpoints ? id->bNumEndpoints - 1 : 0) * sizeof(USBD_PIPE_INFORMATION);
	if (need > room)
		return USBD_STATUS_INVALID_PARAMETER;
	ii->Length = (USHORT) need;
	ii->Class = id->bInterfaceClass;
	ii->SubClass = id->bInterfaceSubClass;
	ii->Protocol = id->bInterfaceProtocol;
	ii->InterfaceHandle = (USBD_INTERFACE_HANDLE) d;
	for (off = at + cfg[at]; off + 2 <= cfglen && cfg[off] >= 2 && n < id->bNumEndpoints; off += cfg[off])
	{
		PUSB_ENDPOINT_DESCRIPTOR e = (PUSB_ENDPOINT_DESCRIPTOR) (cfg + off);
		PUSBD_PIPE_INFORMATION pi = &ii->Pipes[n];
		EP *ep;
		if (e->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
			break;
		if (e->bDescriptorType != USB_ENDPOINT_DESCRIPTOR_TYPE)
			continue;
		ep_close(hc, d->eps[ep_slot(e->bEndpointAddress)], USBD_STATUS_CANCELED);
		d->eps[ep_slot(e->bEndpointAddress)] = NULL;
		KeAcquireSpinLock(&hc->lock, &old);
		ep = ep_create(hc, d, e->bEndpointAddress, (UCHAR) (e->bmAttributes & 3), e->wMaxPacketSize, e->bInterval);
		if (ep)
		{
			ep->ifno = ii->InterfaceNumber;
			ep->alt = ii->AlternateSetting;
		}
		d->eps[ep_slot(e->bEndpointAddress)] = ep;
		KeReleaseSpinLock(&hc->lock, old);
		if (!ep)
			return USBD_STATUS_INSUFFICIENT_RESOURCES;
		pi->MaximumPacketSize = e->wMaxPacketSize & 0x7ff;
		pi->EndpointAddress = e->bEndpointAddress;
		pi->Interval = e->bInterval;
		pi->PipeType = (USBD_PIPE_TYPE) (e->bmAttributes & 3);
		pi->PipeHandle = (USBD_PIPE_HANDLE) ep;
		if (!pi->MaximumTransferSize)
			pi->MaximumTransferSize = USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE;
		n++;
	}
	ii->NumberOfPipes = n;
	return USBD_STATUS_SUCCESS;
}

static void close_pipes(HC *hc, DEV *d, int ifno)
{
	int i;
	for (i = 1; i < 32; i++)
		if (d->eps[i] && i != 16 && (ifno < 0 || d->eps[i]->ifno == ifno))
		{
			ep_close(hc, d->eps[i], USBD_STATUS_CANCELED);
			d->eps[i] = NULL;
		}
}

static EP *pipe_of(DEV *d, USBD_PIPE_HANDLE h)
{
	int i;
	if (!h || h == (USBD_PIPE_HANDLE) d->ep0)
		return d->ep0;
	for (i = 0; i < 32; i++)
		if (d->eps[i] && (USBD_PIPE_HANDLE) d->eps[i] == h)
			return d->eps[i];
	return NULL;
}

static USBD_STATUS select_config(HC *hc, DEV *d, PURB urb)
{
	PUSB_CONFIGURATION_DESCRIPTOR cd = urb->UrbSelectConfiguration.ConfigurationDescriptor;
	PUCHAR end = (PUCHAR) urb + urb->UrbHeader.Length;
	PUSBD_INTERFACE_INFORMATION ii = &urb->UrbSelectConfiguration.Interface;
	ULONG cdlen;
	USBD_STATUS st;
	int n;
	close_pipes(hc, d, -1);
	if (!cd)
	{
		st = ctrl(d, 0x00, USB_REQUEST_SET_CONFIGURATION, 0, 0, NULL, 0, NULL);
		logf("kexehci: %s: unconfigured\n", d->path);
		return st;
	}
	cdlen = cd->wTotalLength;
	st = ctrl(d, 0x00, USB_REQUEST_SET_CONFIGURATION, cd->bConfigurationValue, 0, NULL, 0, NULL);
	if (!USBD_SUCCESS(st))
	{
		logf("kexehci: %s: SET_CONFIGURATION %u failed (%08x)\n", d->path, cd->bConfigurationValue, st);
		return st;
	}
	for (n = 0; n < cd->bNumInterfaces && (PUCHAR) ii + sizeof(USBD_INTERFACE_INFORMATION) <= end; n++)
	{
		ULONG room = (ULONG) (end - (PUCHAR) ii);
		USHORT given = ii->Length;
		if (ii->AlternateSetting)
			ctrl(d, 0x01, USB_REQUEST_SET_INTERFACE, ii->AlternateSetting, ii->InterfaceNumber, NULL, 0, NULL);
		st = open_iface(hc, d, (PUCHAR) cd, cdlen, ii, room);
		if (!USBD_SUCCESS(st))
			return st;
		ii = (PUSBD_INTERFACE_INFORMATION) ((PUCHAR) ii + (given >= sizeof(USBD_INTERFACE_INFORMATION) ? given : ii->Length));
	}
	urb->UrbSelectConfiguration.ConfigurationHandle = (USBD_CONFIGURATION_HANDLE) d;
	logf("kexehci: %s: configuration %u, %u interface(s)\n", d->path, cd->bConfigurationValue, n);
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS select_iface(HC *hc, DEV *d, PURB urb)
{
	PUSBD_INTERFACE_INFORMATION ii = &urb->UrbSelectInterface.Interface;
	ULONG room = (ULONG) ((PUCHAR) urb + urb->UrbHeader.Length - (PUCHAR) ii);
	USBD_STATUS st;
	close_pipes(hc, d, ii->InterfaceNumber);
	st = ctrl(d, 0x01, USB_REQUEST_SET_INTERFACE, ii->AlternateSetting, ii->InterfaceNumber, NULL, 0, NULL);
	if (!USBD_SUCCESS(st))
		return st;
	return open_iface(hc, d, d->cfg, d->cfglen, ii, room);
}

/* reset the device's port and give it back its address and configuration
 * (IOCTL_INTERNAL_USB_RESET_PORT, as Windows' usbhub does) */
static NTSTATUS reset_device(HC *hc, DEV *d)
{
	int i, speed;
	KIRQL old;
	if (d->parent)
		speed = hub_reset(d->parent, d->port);
	else
	{
		int p = d->port - 1;
		port_w(hc, p, PS_PR, PS_PE);
		delay_ms(50);
		port_w(hc, p, 0, PS_PR);
		for (i = 0; i < 20 && (op_r(hc, OP_PORTSC(p)) & PS_PR); i++)
			delay_ms(1);
		delay_ms(10);
		speed = op_r(hc, OP_PORTSC(p)) & PS_PE ? SPEED_HIGH : -1;
		port_w(hc, p, op_r(hc, OP_PORTSC(p)) & PS_CSC, 0);
	}
	if (speed != d->speed)
		return STATUS_UNSUCCESSFUL;
	KeAcquireSpinLock(&hc->lock, &old);
	i = d->addr;
	d->addr = 0;
	qh_setup(hc, d->ep0);
	KeReleaseSpinLock(&hc->lock, old);
	if (!USBD_SUCCESS(ctrl(d, 0x00, USB_REQUEST_SET_ADDRESS, (USHORT) i, 0, NULL, 0, NULL)))
		return STATUS_UNSUCCESSFUL;
	delay_ms(10);
	KeAcquireSpinLock(&hc->lock, &old);
	d->addr = i;
	qh_setup(hc, d->ep0);
	KeReleaseSpinLock(&hc->lock, old);
	{
		int cfgd = 0, s;
		for (s = 1; s < 32; s++)
			if (d->eps[s])
				cfgd = 1;
		if (cfgd)
			ctrl(d, 0x00, USB_REQUEST_SET_CONFIGURATION, d->cfgval, 0, NULL, 0, NULL);
		for (s = 1; s < 32; s++)
			if (d->eps[s])
			{
				if (d->eps[s]->alt)
					ctrl(d, 0x01, USB_REQUEST_SET_INTERFACE, d->eps[s]->alt, d->eps[s]->ifno, NULL, 0, NULL);
				ep_reset(hc, d->eps[s]);
			}
	}
	logf("kexehci: %s: port reset by its driver\n", d->path);
	return STATUS_SUCCESS;
}

static void work_irp(HC *hc, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	PDOEXT *pe = (PDOEXT *) sl->DeviceObject->DeviceExtension;
	DEV *d = pe->dev;
	NTSTATUS st = STATUS_SUCCESS;
	if (sl->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB)
	{
		PURB urb = (PURB) sl->Parameters.Others.Argument1;
		USBD_STATUS us = USBD_STATUS_SUCCESS;
		if (d->gone)
			us = USBD_STATUS_DEVICE_GONE;
		else
			switch (urb->UrbHeader.Function)
			{
			case URB_FUNCTION_SELECT_CONFIGURATION:
				us = select_config(hc, d, urb);
				break;
			case URB_FUNCTION_SELECT_INTERFACE:
				us = select_iface(hc, d, urb);
				break;
			case URB_FUNCTION_ABORT_PIPE:
			case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
			case URB_FUNCTION_SYNC_RESET_PIPE:
			case URB_FUNCTION_SYNC_CLEAR_STALL:
			{
				EP *ep = pipe_of(d, urb->UrbPipeRequest.PipeHandle);
				USHORT f = urb->UrbHeader.Function;
				if (!ep)
				{
					us = USBD_STATUS_INVALID_PIPE_HANDLE;
					break;
				}
				if (f == URB_FUNCTION_ABORT_PIPE)
				{
					KIRQL old;
					LIST_ENTRY *e;
					KeAcquireSpinLock(&hc->lock, &old);
					for (e = ep->xfers.Flink; e != &ep->xfers; e = e->Flink)
					{
						XFER *x = CONTAINING_RECORD(e, XFER, link);
						if (!x->cancel)
						{
							x->cancel = 1;
							x->cstatus = USBD_STATUS_CANCELED;
						}
					}
					ep->scrub = 1;
					hc->scrub_pending = 1;
					KeReleaseSpinLock(&hc->lock, old);
					do_scrubs(hc);
					break;
				}
				if (f != URB_FUNCTION_SYNC_CLEAR_STALL && ep != d->ep0)
					ep_reset(hc, ep);
				if (f != URB_FUNCTION_SYNC_RESET_PIPE && ep->type != USB_ENDPOINT_TYPE_ISOCHRONOUS && ep != d->ep0)
					us = ctrl(d, 0x02, USB_REQUEST_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */, ep->ea, NULL, 0, NULL);
				break;
			}
			default:
				us = USBD_STATUS_INVALID_URB_FUNCTION;
			}
		urb->UrbHeader.Status = us;
		st = nt_status(us);
	}
	else   /* RESET_PORT / CYCLE_PORT / ENABLE_PORT */
		st = d->gone ? STATUS_DEVICE_NOT_CONNECTED : reset_device(hc, d);
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
}

/* ---- the hub thread ------------------------------------------------------------------------ */
static VOID NTAPI hub_thread(PVOID ctx)
{
	HC *hc = (HC *) ctx;
	int p;
	KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
	hc->root_scan = 1;
	for (;;)
	{
		LARGE_INTEGER t;
		KIRQL old;
		t.QuadPart = -250 * 10000;
		KeWaitForSingleObject(&hc->ev, Executive, KernelMode, FALSE, &t);
		if (hc->stop)
			break;
		for (;;)
		{
			PIRP irp = NULL;
			KeAcquireSpinLock(&hc->lock, &old);
			if (!IsListEmpty(&hc->work))
				irp = CONTAINING_RECORD(RemoveHeadList(&hc->work), IRP, Tail.Overlay.ListEntry);
			KeReleaseSpinLock(&hc->lock, old);
			if (!irp)
				break;
			work_irp(hc, irp);
		}
		do_scrubs(hc);
		for (p = 0; p < hc->nports; p++)
			root_port(hc, p);
		hc->root_scan = 0;
		for (p = 1; p <= hc->nports; p++)
			hubs_service(hc, hc->root[p]);
		/* the log, for testing on real hardware: once things settle, and then when it changes */
		if (now_ms() - hc->started_ms > 20000 && log_len != log_written && now_ms() - hc->flushed_ms > 10000 &&
		    hc->index == 0)
		{
			log_flush();
			hc->flushed_ms = now_ms();
		}
	}
	for (p = 1; p <= hc->nports; p++)
		if (hc->root[p])
		{
			dev_remove(hc, hc->root[p]);
			hc->root[p] = NULL;
		}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

static void queue_work(HC *hc, PIRP irp)
{
	KIRQL old;
	IoMarkIrpPending(irp);
	KeAcquireSpinLock(&hc->lock, &old);
	InsertTailList(&hc->work, &irp->Tail.Overlay.ListEntry);
	KeReleaseSpinLock(&hc->lock, old);
	KeSetEvent(&hc->ev, 0, FALSE);
}

/* ---- transfer URBs from function drivers ------------------------------------------------- */
static VOID NTAPI xfer_cancel(PDEVICE_OBJECT dobj, PIRP irp)
{
	PDOEXT *pe = (PDOEXT *) dobj->DeviceExtension;
	HC *hc = pe->hc;
	KIRQL old;
	LIST_ENTRY *e, *f;
	IoReleaseCancelSpinLock(irp->CancelIrql);
	KeAcquireSpinLock(&hc->lock, &old);
	for (e = hc->eps.Flink; e != &hc->eps; e = e->Flink)
	{
		EP *ep = CONTAINING_RECORD(e, EP, all);
		for (f = ep->xfers.Flink; f != &ep->xfers; f = f->Flink)
		{
			XFER *x = CONTAINING_RECORD(f, XFER, link);
			if (x->irp == irp && !x->cancel)
			{
				x->cancel = 1;
				x->cstatus = USBD_STATUS_CANCELED;
				ep->scrub = 1;
				hc->scrub_pending = 1;
			}
		}
	}
	KeReleaseSpinLock(&hc->lock, old);
	KeSetEvent(&hc->ev, 0, FALSE);
}

static NTSTATUS fail_urb(PIRP irp, PURB urb, USBD_STATUS us)
{
	NTSTATUS st = nt_status(us);
	urb->UrbHeader.Status = us;
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static NTSTATUS submit_urb(HC *hc, DEV *d, PIRP irp, PURB urb, EP *ep, int control, const UCHAR *setup)
{
	XFER *x;
	KIRQL old;
	USBD_STATUS us;
	struct _URB_CONTROL_TRANSFER *ct = &urb->UrbControlTransfer;   /* same layout for all transfer URBs */
	if (!ep)
		return fail_urb(irp, urb, USBD_STATUS_INVALID_PIPE_HANDLE);
	if (ep->type == USB_ENDPOINT_TYPE_ISOCHRONOUS)
		return fail_urb(irp, urb, USBD_STATUS_NOT_SUPPORTED);
	x = (XFER *) ExAllocatePoolWithTag(NonPagedPool, sizeof(XFER), TAG);
	if (!x)
		return fail_urb(irp, urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
	memset(x, 0, sizeof(*x));
	x->irp = irp;
	x->urb = urb;
	x->len = ct->TransferBufferLength;
	if (x->len && ct->TransferBufferMDL)
	{
		PMDL m = ct->TransferBufferMDL;
		if (m->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL))
			x->va = (UCHAR *) m->MappedSystemVa;
		else
		{
			x->mdl = m;
			x->bo = m->ByteOffset;
		}
	}
	else
		x->va = (UCHAR *) ct->TransferBuffer;
	if (x->va)
		x->bo = (ULONG) (ULONG_PTR) x->va & 0xfff;
	if (x->len && !x->va && !x->mdl)
	{
		ExFreePool(x);
		return fail_urb(irp, urb, USBD_STATUS_INVALID_PARAMETER);
	}
	if (control)
	{
		USHORT wl;
		memcpy(x->setup, setup, 8);
		wl = (USHORT) (x->setup[6] | (x->setup[7] << 8));
		if (wl < x->len)
			x->len = wl;
		x->in = (x->setup[0] & 0x80) != 0;
	}
	else
		x->in = (ep->ea & 0x80) != 0;
	IoMarkIrpPending(irp);
	IoSetCancelRoutine(irp, xfer_cancel);
	if (irp->Cancel && IoSetCancelRoutine(irp, NULL))
	{
		ExFreePool(x);
		fail_urb(irp, urb, USBD_STATUS_CANCELED);
		return STATUS_PENDING;
	}
	KeAcquireSpinLock(&hc->lock, &old);
	us = d->gone ? USBD_STATUS_DEVICE_GONE : xfer_submit(hc, ep, x, control);
	KeReleaseSpinLock(&hc->lock, old);
	if (us != USBD_STATUS_SUCCESS)
	{
		if (IoSetCancelRoutine(irp, NULL))
		{
			ExFreePool(x);
			fail_urb(irp, urb, us);
		}
		else
		{
			/* the cancel routine found nothing; complete it here */
			ExFreePool(x);
			fail_urb(irp, urb, USBD_STATUS_CANCELED);
		}
	}
	return STATUS_PENDING;
}

static ULONG frame_number(HC *hc)
{
	ULONG f = (op_r(hc, OP_FRINDEX) >> 3) & 0x3ff;
	return (hc->frame_hi << 10) | f;
}

/* A transfer for an unplugged device waits until its driver cancels it or
 * PnP removes the device, as it would on a device that stopped answering:
 * failing it at once makes drivers that resubmit from their completion
 * routine (hidclass) recurse until the stack overflows. */
static VOID NTAPI park_cancel(PDEVICE_OBJECT dobj, PIRP irp)
{
	PDOEXT *pe = (PDOEXT *) dobj->DeviceExtension;
	KIRQL old;
	IoReleaseCancelSpinLock(irp->CancelIrql);
	KeAcquireSpinLock(&pe->plock, &old);
	RemoveEntryList(&irp->Tail.Overlay.ListEntry);
	KeReleaseSpinLock(&pe->plock, old);
	((PURB) IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1)->UrbHeader.Status = USBD_STATUS_CANCELED;
	irp->IoStatus.Status = STATUS_CANCELLED;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS park(PDOEXT *pe, PIRP irp)
{
	KIRQL old;
	IoMarkIrpPending(irp);
	KeAcquireSpinLock(&pe->plock, &old);
	IoSetCancelRoutine(irp, park_cancel);
	if (irp->Cancel && IoSetCancelRoutine(irp, NULL))
	{
		KeReleaseSpinLock(&pe->plock, old);
		irp->IoStatus.Status = STATUS_CANCELLED;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_PENDING;
	}
	InsertTailList(&pe->parked, &irp->Tail.Overlay.ListEntry);
	KeReleaseSpinLock(&pe->plock, old);
	return STATUS_PENDING;
}

static void unpark_all(PDOEXT *pe)
{
	for (;;)
	{
		KIRQL old;
		PIRP irp = NULL;
		KeAcquireSpinLock(&pe->plock, &old);
		while (!IsListEmpty(&pe->parked))
		{
			PIRP i = CONTAINING_RECORD(RemoveHeadList(&pe->parked), IRP, Tail.Overlay.ListEntry);
			if (IoSetCancelRoutine(i, NULL))
			{
				irp = i;
				break;
			}
			InitializeListHead(&i->Tail.Overlay.ListEntry);   /* its cancel routine is running */
		}
		KeReleaseSpinLock(&pe->plock, old);
		if (!irp)
			return;
		((PURB) IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1)->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
		irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
}

static NTSTATUS pdo_urb(PDOEXT *pe, PIRP irp, PURB urb)
{
	DEV *d = pe->dev;
	HC *hc = pe->hc;
	USHORT fn = urb->UrbHeader.Function;
	UCHAR s[8];
	struct _URB_CONTROL_TRANSFER *ct = &urb->UrbControlTransfer;
	UCHAR rcpt = 0, type = 0, req = 0;
	USHORT val = 0, idx = 0, len;
	int in = 0;
	if (d->gone)
	{
		if (fn == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER || fn == URB_FUNCTION_CONTROL_TRANSFER ||
		    fn == URB_FUNCTION_ISOCH_TRANSFER)
			return park(pe, irp);
		return fail_urb(irp, urb, USBD_STATUS_DEVICE_GONE);
	}
	urb->UrbHeader.UsbdDeviceHandle = d;
	switch (fn)
	{
	case URB_FUNCTION_SELECT_CONFIGURATION:
	case URB_FUNCTION_SELECT_INTERFACE:
	case URB_FUNCTION_ABORT_PIPE:
	case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
	case URB_FUNCTION_SYNC_RESET_PIPE:
	case URB_FUNCTION_SYNC_CLEAR_STALL:
		queue_work(hc, irp);
		return STATUS_PENDING;
	case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
		urb->UrbGetCurrentFrameNumber.FrameNumber = frame_number(hc);
		return fail_urb(irp, urb, USBD_STATUS_SUCCESS);
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
		return submit_urb(hc, d, irp, urb, pipe_of(d, urb->UrbBulkOrInterruptTransfer.PipeHandle), 0, NULL);
	case URB_FUNCTION_CONTROL_TRANSFER:
	{
		EP *ep = ct->TransferFlags & USBD_DEFAULT_PIPE_TRANSFER ? d->ep0 : pipe_of(d, ct->PipeHandle);
		if (ep && ep->type != USB_ENDPOINT_TYPE_CONTROL)
			ep = NULL;
		return submit_urb(hc, d, irp, urb, ep, 1, ct->SetupPacket);
	}
	case URB_FUNCTION_ISOCH_TRANSFER:
		return fail_urb(irp, urb, USBD_STATUS_NOT_SUPPORTED);
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
		rcpt = fn == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ? 0 : fn == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ? 1 : 2;
		in = 1;
		req = USB_REQUEST_GET_DESCRIPTOR;
		goto desc;
	case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
	case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
	case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
		rcpt = fn == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE ? 0 : fn == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE ? 1 : 2;
		req = USB_REQUEST_SET_DESCRIPTOR;
	desc:
		val = (USHORT) (urb->UrbControlDescriptorRequest.Index | (urb->UrbControlDescriptorRequest.DescriptorType << 8));
		idx = urb->UrbControlDescriptorRequest.LanguageId;
		break;
	case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
	case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
	case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
	case URB_FUNCTION_SET_FEATURE_TO_OTHER:
	case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
	case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
	case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
	case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
		rcpt = fn == URB_FUNCTION_SET_FEATURE_TO_DEVICE || fn == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE       ? 0
		       : fn == URB_FUNCTION_SET_FEATURE_TO_INTERFACE || fn == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE ? 1
		       : fn == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT || fn == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT   ? 2
		                                                                                                      : 3;
		req = fn >= URB_FUNCTION_SET_FEATURE_TO_DEVICE && fn <= URB_FUNCTION_SET_FEATURE_TO_ENDPOINT ? USB_REQUEST_SET_FEATURE
		      : fn == URB_FUNCTION_SET_FEATURE_TO_OTHER ? USB_REQUEST_SET_FEATURE
		                                                : USB_REQUEST_CLEAR_FEATURE;
		val = urb->UrbControlFeatureRequest.FeatureSelector;
		idx = urb->UrbControlFeatureRequest.Index;
		ct->TransferBufferLength = 0;
		ct->TransferBuffer = NULL;
		ct->TransferBufferMDL = NULL;
		break;
	case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
	case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
	case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
	case URB_FUNCTION_GET_STATUS_FROM_OTHER:
		rcpt = fn == URB_FUNCTION_GET_STATUS_FROM_OTHER ? 3 : (UCHAR) (fn - URB_FUNCTION_GET_STATUS_FROM_DEVICE);
		req = USB_REQUEST_GET_STATUS;
		in = 1;
		idx = urb->UrbControlGetStatusRequest.Index;
		break;
	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
	case URB_FUNCTION_VENDOR_OTHER:
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
	{
		struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *vc = &urb->UrbControlVendorClassRequest;
		int vendor = fn == URB_FUNCTION_VENDOR_DEVICE || fn == URB_FUNCTION_VENDOR_INTERFACE ||
		             fn == URB_FUNCTION_VENDOR_ENDPOINT || fn == URB_FUNCTION_VENDOR_OTHER;
		rcpt = fn == URB_FUNCTION_VENDOR_DEVICE || fn == URB_FUNCTION_CLASS_DEVICE       ? 0
		       : fn == URB_FUNCTION_VENDOR_INTERFACE || fn == URB_FUNCTION_CLASS_INTERFACE ? 1
		       : fn == URB_FUNCTION_VENDOR_ENDPOINT || fn == URB_FUNCTION_CLASS_ENDPOINT   ? 2
		                                                                                   : 3;
		type = (UCHAR) ((vendor ? 0x40 : 0x20) | vc->RequestTypeReservedBits);
		in = (vc->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
		req = vc->Request;
		val = vc->Value;
		idx = vc->Index;
		break;
	}
	case URB_FUNCTION_GET_CONFIGURATION:
		in = 1;
		req = USB_REQUEST_GET_CONFIGURATION;
		break;
	case URB_FUNCTION_GET_INTERFACE:
		rcpt = 1;
		in = 1;
		req = USB_REQUEST_GET_INTERFACE;
		idx = urb->UrbControlGetInterfaceRequest.Interface;
		break;
	default:
		logf("kexehci: %s: URB function %04x not supported\n", d->path, fn);
		return fail_urb(irp, urb, USBD_STATUS_INVALID_URB_FUNCTION);
	}
	len = (USHORT) (ct->TransferBufferLength > 0xffff ? 0xffff : ct->TransferBufferLength);
	s[0] = (UCHAR) ((in ? 0x80 : 0) | type | rcpt);
	s[1] = req;
	s[2] = (UCHAR) val;
	s[3] = (UCHAR) (val >> 8);
	s[4] = (UCHAR) idx;
	s[5] = (UCHAR) (idx >> 8);
	s[6] = (UCHAR) len;
	s[7] = (UCHAR) (len >> 8);
	return submit_urb(hc, d, irp, urb, d->ep0, 1, s);
}

/* ---- USBDI bus interface: lets drivers ask for high speed -------------------------------- */
static VOID NTAPI bi_ref(PVOID ctx)
{
	(void) ctx;
}

static VOID NTAPI bi_version(PVOID ctx, PVOID vip, PULONG caps)
{
	PUSBD_VERSION_INFORMATION vi = (PUSBD_VERSION_INFORMATION) vip;
	(void) ctx;
	if (vi)
	{
		vi->USBDI_Version = 0x500;
		vi->Supported_USB_Version = 0x200;
	}
	if (caps)
		*caps = 0;
}

static NTSTATUS NTAPI bi_bustime(PVOID ctx, PULONG frame)
{
	DEV *d = (DEV *) ctx;
	if (frame)
		*frame = frame_number(d->hc);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI bi_iso(PVOID ctx, PVOID urb)
{
	(void) ctx;
	(void) urb;
	return STATUS_NOT_SUPPORTED;
}

static NTSTATUS NTAPI bi_businfo(PVOID ctx, ULONG level, PVOID buf, PULONG len, PULONG actual)
{
	(void) ctx;
	(void) level;
	(void) buf;
	(void) len;
	(void) actual;
	return STATUS_NOT_SUPPORTED;
}

static BOOLEAN NTAPI bi_highspeed(PVOID ctx)
{
	return ((DEV *) ctx)->speed == SPEED_HIGH;
}

static const GUID g_bus_usb = { 0x9d7debbc, 0xc85d, 0x11d1, { 0x9e, 0xb4, 0x00, 0x60, 0x08, 0xc3, 0xa1, 0x9a } };

static int guid_eq(const GUID *a, const GUID *b)
{
	return RtlCompareMemory(a, b, sizeof(GUID)) == sizeof(GUID);
}

/* ---- PDOs ------------------------------------------------------------------------------------ */
static WCHAR *put_s(WCHAR *p, const char *s)
{
	while (*s)
		*p++ = (WCHAR) *s++;
	return p;
}

static WCHAR *put_x(WCHAR *p, ULONG v, int digits)
{
	while (digits--)
	{
		ULONG d = (v >> (4 * digits)) & 0xf;
		*p++ = (WCHAR) (d < 10 ? '0' + d : 'A' + d - 10);
	}
	return p;
}

static WCHAR *put_vidpid(WCHAR *p, DEV *d)
{
	p = put_s(p, "USB\\VID_");
	p = put_x(p, d->dd.idVendor, 4);
	p = put_s(p, "&PID_");
	return put_x(p, d->dd.idProduct, 4);
}

static void class_triple(DEV *d, UCHAR *c, UCHAR *s, UCHAR *p)
{
	ULONG off;
	*c = d->dd.bDeviceClass;
	*s = d->dd.bDeviceSubClass;
	*p = d->dd.bDeviceProtocol;
	if (*c)
		return;
	for (off = 0; off + 2 <= d->cfglen && d->cfg[off] >= 2; off += d->cfg[off])
	{
		PUSB_INTERFACE_DESCRIPTOR i = (PUSB_INTERFACE_DESCRIPTOR) (d->cfg + off);
		if (i->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
		{
			*c = i->bInterfaceClass;
			*s = i->bInterfaceSubClass;
			*p = i->bInterfaceProtocol;
			return;
		}
	}
}

static NTSTATUS query_id(DEV *d, BUS_QUERY_ID_TYPE t, PWCHAR *out)
{
	WCHAR *b = (WCHAR *) ExAllocatePoolWithTag(PagedPool, 512, TAG), *p;
	if (!b)
		return STATUS_INSUFFICIENT_RESOURCES;
	p = b;
	switch (t)
	{
	case BusQueryDeviceID:
		p = put_vidpid(p, d);
		*p++ = 0;
		break;
	case BusQueryHardwareIDs:
		p = put_vidpid(p, d);
		p = put_s(p, "&REV_");
		p = put_x(p, d->dd.bcdDevice, 4);
		*p++ = 0;
		p = put_vidpid(p, d);
		*p++ = 0;
		*p++ = 0;
		break;
	case BusQueryCompatibleIDs:
	{
		UCHAR c, s, pr;
		class_triple(d, &c, &s, &pr);
		p = put_s(p, "USB\\CLASS_");
		p = put_x(p, c, 2);
		p = put_s(p, "&SUBCLASS_");
		p = put_x(p, s, 2);
		p = put_s(p, "&PROT_");
		p = put_x(p, pr, 2);
		*p++ = 0;
		p = put_s(p, "USB\\CLASS_");
		p = put_x(p, c, 2);
		p = put_s(p, "&SUBCLASS_");
		p = put_x(p, s, 2);
		*p++ = 0;
		p = put_s(p, "USB\\CLASS_");
		p = put_x(p, c, 2);
		*p++ = 0;
		*p++ = 0;
		break;
	}
	case BusQueryInstanceID:
		if (d->has_serial)
		{
			int i;
			for (i = 0; d->serial[i] && i < 60; i++)
				*p++ = d->serial[i];
		}
		else
		{
			const char *s;
			p = put_s(p, "EHCI");
			*p++ = (WCHAR) ('0' + d->hc->index);
			*p++ = '&';
			for (s = d->path; *s; s++)
				*p++ = *s == '.' ? '&' : (WCHAR) *s;
		}
		*p++ = 0;
		break;
	default:
		ExFreePool(b);
		return STATUS_NOT_SUPPORTED;
	}
	*out = b;
	return STATUS_SUCCESS;
}

static NTSTATUS pdo_pnp(PDOEXT *pe, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	DEV *d = pe->dev;
	NTSTATUS st = irp->IoStatus.Status;
#ifdef KEX_DEBUGCON
	logf("kexehci: %s: PnP %02x%s\n", d->path, sl->MinorFunction, d->gone ? " (gone)" : "");
#endif
	switch (sl->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
		pe->started = 1;
		st = STATUS_SUCCESS;
		break;
	case IRP_MN_QUERY_STOP_DEVICE:
	case IRP_MN_QUERY_REMOVE_DEVICE:
	case IRP_MN_CANCEL_STOP_DEVICE:
	case IRP_MN_CANCEL_REMOVE_DEVICE:
	case IRP_MN_STOP_DEVICE:
		st = STATUS_SUCCESS;
		break;
	case IRP_MN_SURPRISE_REMOVAL:
		unpark_all(pe);
		st = STATUS_SUCCESS;
		break;
	case IRP_MN_REMOVE_DEVICE:
	{
		HC *hc = pe->hc;
		pe->started = 0;
		unpark_all(pe);
		ExAcquireFastMutex(&hc->tree);
		if (d->gone && d->torn_down)
		{
			PDEVICE_OBJECT pdo = d->pdo;
			d->pdo = NULL;
			ExReleaseFastMutex(&hc->tree);
			logf("kexehci: %s: removed\n", d->path);
			dev_free(d);
			irp->IoStatus.Status = STATUS_SUCCESS;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			IoDeleteDevice(pdo);
			return STATUS_SUCCESS;
		}
		ExReleaseFastMutex(&hc->tree);
		st = STATUS_SUCCESS;
		break;
	}
	case IRP_MN_QUERY_ID:
	{
		PWCHAR id = NULL;
		st = query_id(d, sl->Parameters.QueryId.IdType, &id);
		if (NT_SUCCESS(st))
			irp->IoStatus.Information = (ULONG_PTR) id;
		else if (st == STATUS_NOT_SUPPORTED)
			st = irp->IoStatus.Status;
		break;
	}
	case IRP_MN_QUERY_DEVICE_TEXT:
		if (sl->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription ||
		    sl->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation)
		{
			WCHAR *b = (WCHAR *) ExAllocatePoolWithTag(PagedPool, 160, TAG), *p = b;
			if (!b)
			{
				st = STATUS_INSUFFICIENT_RESOURCES;
				break;
			}
			if (sl->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
			{
				if (d->product[0])
				{
					int i;
					for (i = 0; d->product[i] && i < 64; i++)
						*p++ = d->product[i];
				}
				else
					p = put_s(p, "USB Device");
			}
			else
			{
				p = put_s(p, "USB 2.0 port ");
				p = put_s(p, d->path);
			}
			*p = 0;
			irp->IoStatus.Information = (ULONG_PTR) b;
			st = STATUS_SUCCESS;
		}
		break;
	case IRP_MN_QUERY_CAPABILITIES:
	{
		PDEVICE_CAPABILITIES c = sl->Parameters.DeviceCapabilities.Capabilities;
		int i;
		c->LockSupported = FALSE;
		c->EjectSupported = FALSE;
		c->Removable = TRUE;
		c->DockDevice = FALSE;
		c->UniqueID = d->has_serial ? TRUE : FALSE;
		c->SilentInstall = FALSE;
		c->RawDeviceOK = FALSE;
		c->SurpriseRemovalOK = TRUE;
		c->Address = (ULONG) d->port;
		c->UINumber = (ULONG) d->port;
		c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
		for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++)
			c->DeviceState[i] = PowerDeviceD3;
		c->SystemWake = PowerSystemUnspecified;
		c->DeviceWake = PowerDeviceUnspecified;
		st = STATUS_SUCCESS;
		break;
	}
	case IRP_MN_QUERY_BUS_INFORMATION:
	{
		PPNP_BUS_INFORMATION bi = (PPNP_BUS_INFORMATION) ExAllocatePoolWithTag(PagedPool, sizeof(*bi), TAG);
		if (!bi)
		{
			st = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		bi->BusTypeGuid = g_bus_usb;
		bi->LegacyBusType = PNPBus;
		bi->BusNumber = (ULONG) pe->hc->index;
		irp->IoStatus.Information = (ULONG_PTR) bi;
		st = STATUS_SUCCESS;
		break;
	}
	case IRP_MN_QUERY_DEVICE_RELATIONS:
		if (sl->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation)
		{
			PDEVICE_RELATIONS r = (PDEVICE_RELATIONS) ExAllocatePoolWithTag(PagedPool, sizeof(*r), TAG);
			if (!r)
			{
				st = STATUS_INSUFFICIENT_RESOURCES;
				break;
			}
			r->Count = 1;
			r->Objects[0] = d->pdo;
			ObReferenceObject(d->pdo);
			irp->IoStatus.Information = (ULONG_PTR) r;
			st = STATUS_SUCCESS;
		}
		break;
	case IRP_MN_QUERY_RESOURCES:
	case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
		st = STATUS_SUCCESS;
		break;
	case IRP_MN_QUERY_INTERFACE:
		if (guid_eq(sl->Parameters.QueryInterface.InterfaceType, &KEX_USB_BUS_INTERFACE_USBDI_GUID) &&
		    sl->Parameters.QueryInterface.Size >= KEX_USBDI_V0_SIZE)
		{
			KEX_USB_BUS_INTERFACE_USBDI_V1 *bi = (KEX_USB_BUS_INTERFACE_USBDI_V1 *) sl->Parameters.QueryInterface.Interface;
			int v1 = sl->Parameters.QueryInterface.Version >= 1 && sl->Parameters.QueryInterface.Size >= KEX_USBDI_V1_SIZE;
			bi->Size = v1 ? KEX_USBDI_V1_SIZE : KEX_USBDI_V0_SIZE;
			bi->Version = (USHORT) (v1 ? 1 : 0);
			bi->BusContext = d;
			bi->InterfaceReference = bi_ref;
			bi->InterfaceDereference = bi_ref;
			bi->GetUSBDIVersion = bi_version;
			bi->QueryBusTime = bi_bustime;
			bi->SubmitIsoOutUrb = bi_iso;
			bi->QueryBusInformation = bi_businfo;
			if (v1)
				bi->IsDeviceHighSpeed = bi_highspeed;
			st = STATUS_SUCCESS;
		}
		break;
	default:
		break;
	}
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static NTSTATUS pdo_ioctl(PDOEXT *pe, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	DEV *d = pe->dev;
	NTSTATUS st;
	switch (sl->Parameters.DeviceIoControl.IoControlCode)
	{
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
	{
		PURB urb = (PURB) sl->Parameters.Others.Argument1;
		if (!urb)
		{
			st = STATUS_INVALID_PARAMETER;
			break;
		}
		return pdo_urb(pe, irp, urb);
	}
	case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
		if (sl->Parameters.Others.Argument1)
			*(PULONG) sl->Parameters.Others.Argument1 = d->gone ? 0 : USBD_PORT_ENABLED | USBD_PORT_CONNECTED;
		st = STATUS_SUCCESS;
		break;
	case IOCTL_INTERNAL_USB_RESET_PORT:
	case IOCTL_INTERNAL_USB_ENABLE_PORT:
	case IOCTL_INTERNAL_USB_CYCLE_PORT:
		if (d->gone)
		{
			st = STATUS_DEVICE_NOT_CONNECTED;
			break;
		}
		queue_work(pe->hc, irp);
		return STATUS_PENDING;
	case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
		if (sl->Parameters.Others.Argument1)
			(*(PULONG) sl->Parameters.Others.Argument1)++;
		st = STATUS_SUCCESS;
		break;
	case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO:
		if (sl->Parameters.Others.Argument1)
			*(PVOID *) sl->Parameters.Others.Argument1 = d->pdo;
		if (sl->Parameters.Others.Argument2)
			*(PVOID *) sl->Parameters.Others.Argument2 = pe->hc->fdo;
		st = STATUS_SUCCESS;
		break;
	default:
		st = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static NTSTATUS pdo_create(HC *hc, DEV *d)
{
	WCHAR name[40], *p;
	UNICODE_STRING us;
	PDEVICE_OBJECT pdo;
	PDOEXT *pe;
	NTSTATUS st;
	p = put_s(name, "\\Device\\KexUsb2_");
	p = put_x(p, (ULONG) hc->index, 1);
	*p++ = '_';
	p = put_x(p, (ULONG) hc->pdo_count++, 4);
	*p = 0;
	RtlInitUnicodeString(&us, name);
	st = IoCreateDevice(g_drv, sizeof(PDOEXT), &us, FILE_DEVICE_UNKNOWN, 0, FALSE, &pdo);
	if (!NT_SUCCESS(st))
	{
		logf("kexehci: %s: IoCreateDevice %08x\n", d->path, st);
		return st;
	}
	pe = (PDOEXT *) pdo->DeviceExtension;
	pe->kind = EXT_PDO;
	pe->hc = hc;
	pe->dev = d;
	KeInitializeSpinLock(&pe->plock);
	InitializeListHead(&pe->parked);
	pdo->Flags &= ~DO_DEVICE_INITIALIZING;
	ExAcquireFastMutex(&hc->tree);
	d->pdo = pdo;
	ExReleaseFastMutex(&hc->tree);
	IoInvalidateDeviceRelations(hc->pdo, BusRelations);
	return STATUS_SUCCESS;
}

/* ---- FDO: the controller --------------------------------------------------------------------- */
static void collect(DEV *d, PDEVICE_RELATIONS r)
{
	int i;
	if (!d || d->gone)
		return;
	if (d->pdo)
	{
		r->Objects[r->Count++] = d->pdo;
		ObReferenceObject(d->pdo);
	}
	for (i = 1; i <= d->nports; i++)
		collect(d->child[i], r);
}

static void hc_halt(HC *hc)
{
	int i;
	if (!hc->op)
		return;
	op_w(hc, OP_USBINTR, 0);
	op_w(hc, OP_USBCMD, op_r(hc, OP_USBCMD) & ~(CMD_RS | CMD_PSE | CMD_ASE));
	for (i = 0; i < 20 && !(op_r(hc, OP_USBSTS) & STS_HALTED); i++)
		KeStallExecutionProcessor(1000);
	/* every port back to the USB 1.1 controllers, for the BIOS and DOS */
	op_w(hc, OP_CONFIGFLAG, 0);
}

static NTSTATUS hc_start(HC *hc, PCM_RESOURCE_LIST raw, PCM_RESOURCE_LIST tr)
{
	ULONG i, vector = 0, level = 0, mem_raw = 0, pa;
	KAFFINITY aff = 0;
	KINTERRUPT_MODE mode = LevelSensitive;
	BOOLEAN shared = TRUE;
	PHYSICAL_ADDRESS hi;
	NTSTATUS st;
	int p;
	if (!tr || !tr->Count)
		return STATUS_INSUFFICIENT_RESOURCES;
	for (i = 0; i < tr->List[0].PartialResourceList.Count; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &tr->List[0].PartialResourceList.PartialDescriptors[i];
		PCM_PARTIAL_RESOURCE_DESCRIPTOR r =
		    raw && raw->Count && i < raw->List[0].PartialResourceList.Count ? &raw->List[0].PartialResourceList.PartialDescriptors[i] : d;
		if (d->Type == CmResourceTypeMemory && !hc->regs_len)
		{
			hc->regs_pa = d->u.Memory.Start;
			hc->regs_len = d->u.Memory.Length;
			mem_raw = r->u.Memory.Start.LowPart;
		}
		else if (d->Type == CmResourceTypeInterrupt)
		{
			vector = d->u.Interrupt.Vector;
			level = d->u.Interrupt.Level;
			aff = d->u.Interrupt.Affinity;
			mode = d->Flags & CM_RESOURCE_INTERRUPT_LATCHED ? Latched : LevelSensitive;
			shared = d->ShareDisposition == CmResourceShareShared;
		}
	}
	if (!hc->regs_len || !vector)
	{
		logf("kexehci: no memory or interrupt resource\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	if (pci_find(hc, mem_raw))
		cfg_w(hc, 0x04, (cfg_r32(hc, 0x04) & 0xffff) | 0x06, 2);   /* memory space, bus master */
	else
		logf("kexehci: controller at %08x not found on PCI\n", mem_raw);
	hc->regs = (PUCHAR) MmMapIoSpace(hc->regs_pa, hc->regs_len, MmNonCached);
	if (!hc->regs)
		return STATUS_INSUFFICIENT_RESOURCES;
	hc->hcs = cap_r(hc, CAP_HCSPARAMS);
	hc->hcc = cap_r(hc, CAP_HCCPARAMS);
	hc->nports = (int) (hc->hcs & 0x0f);
	if (hc->nports > MAX_PORTS)
		hc->nports = MAX_PORTS;
	hc->ncc = (int) ((hc->hcs >> 12) & 0x0f);
	logf("kexehci: EHCI %x.%02x at %08x irq %u: %u ports, %u companion controllers%s\n",
	     (cap_r(hc, 0) >> 24) & 0xff, (cap_r(hc, 0) >> 16) & 0xff, hc->regs_pa.LowPart, level, hc->nports, hc->ncc,
	     hc->hcc & 1 ? ", 64-bit" : "");
	if (hc->pci_bus != 0xffffffff)
		bios_handoff(hc);
	hc->op = hc->regs + (cap_r(hc, CAP_LENGTH) & 0xff);
	/* stop and reset */
	op_w(hc, OP_USBINTR, 0);
	op_w(hc, OP_USBCMD, 0);
	for (i = 0; i < 20 && !(op_r(hc, OP_USBSTS) & STS_HALTED); i++)
		delay_ms(1);
	op_w(hc, OP_USBCMD, CMD_HCRESET);
	for (i = 0; i < 250 && (op_r(hc, OP_USBCMD) & CMD_HCRESET); i++)
		delay_ms(1);
	if (op_r(hc, OP_USBCMD) & CMD_HCRESET)
	{
		logf("kexehci: reset did not finish\n");
		return STATUS_DEVICE_NOT_READY;
	}
	/* memory: frame list, QHs, qTDs (all below 4 GB, physically contiguous) */
	hi.QuadPart = 0xffffffff;
	if (!hc->flist)
	{
		hc->flist = (volatile ULONG *) MmAllocateContiguousMemory(4096, hi);
		hc->qhs = (QHW *) MmAllocateContiguousMemory(NQH * sizeof(QHW), hi);
		hc->ntd = NTD;
		hc->tds = (QTD *) MmAllocateContiguousMemory(NTD * sizeof(QTD), hi);
		if (!hc->tds)
		{
			hc->ntd = NTD / 2;
			hc->tds = (QTD *) MmAllocateContiguousMemory(hc->ntd * sizeof(QTD), hi);
		}
		if (!hc->flist || !hc->qhs || !hc->tds)
		{
			logf("kexehci: no memory for the schedules\n");
			return STATUS_INSUFFICIENT_RESOURCES;
		}
	}
	hc->flist_pa = MmGetPhysicalAddress((PVOID) hc->flist).LowPart;
	hc->qh_pa = MmGetPhysicalAddress(hc->qhs).LowPart;
	hc->td_pa = MmGetPhysicalAddress(hc->tds).LowPart;
	memset((void *) hc->qhs, 0, NQH * sizeof(QHW));
	memset((void *) hc->tds, 0, (SIZE_T) hc->ntd * sizeof(QTD));
	hc->td_free = -1;
	hc->td_nfree = 0;
	for (p = hc->ntd - 1; p >= 0; p--)
		td_free(hc, p);
	memset(hc->qh_used, 0, sizeof(hc->qh_used));
	for (p = 0; p < QH_FIRST_FREE; p++)
		hc->qh_used[p] = 1;
	InitializeListHead(&hc->async_list);
	for (p = 0; p < 64; p++)
		InitializeListHead(&hc->anchor_list[p]);
	/* async ring head, halted and empty */
	{
		QHW *h = QH(hc, QH_ASYNC_HEAD);
		h->hlink = QHPA(hc, QH_ASYNC_HEAD) | LP_QH;
		h->epchar = (1u << 15) | (SPEED_HIGH << 12) | (64u << 16);
		h->epcaps = 1u << 30;
		h->next = h->alt = LP_T;
		h->token = TK_HALTED;
	}
	/* periodic tree of halted anchor QHs: 32 -> 16 -> ... -> 1 */
	{
		int per;
		for (per = 1; per <= MAX_PERIOD; per *= 2)
			for (p = 0; p < per; p++)
			{
				QHW *h = QH(hc, QH_ANCHOR(per, p));
				h->hlink = per == 1 ? LP_T : QHPA(hc, QH_ANCHOR(per / 2, p % (per / 2))) | LP_QH;
				h->epchar = (SPEED_HIGH << 12) | (64u << 16);
				h->epcaps = (1u << 30) | 0x01;
				h->next = h->alt = LP_T;
				h->token = TK_HALTED;
			}
		for (p = 0; p < 1024; p++)
			hc->flist[p] = QHPA(hc, QH_ANCHOR(MAX_PERIOD, p % MAX_PERIOD)) | LP_QH;
	}
	/* interrupt */
	KeInitializeDpc(&hc->dpc, hc_dpc, hc);
	st = IoConnectInterrupt(&hc->intr, hc_isr, hc, NULL, vector, (KIRQL) level, (KIRQL) level, mode, shared, aff, FALSE);
	if (!NT_SUCCESS(st))
	{
		logf("kexehci: IoConnectInterrupt %08x\n", st);
		return st;
	}
	/* run */
	if (hc->hcc & 1)
		op_w(hc, OP_CTRLDSSEG, 0);
	op_w(hc, OP_PERIODIC, hc->flist_pa);
	op_w(hc, OP_ASYNC, QHPA(hc, QH_ASYNC_HEAD));
	op_w(hc, OP_USBCMD, (1u << 16) | CMD_PSE | CMD_ASE | CMD_RS);   /* interrupts within one microframe */
	op_w(hc, OP_CONFIGFLAG, 1);   /* every port to us: we pass on the USB 1.1 devices */
	(void) op_r(hc, OP_CONFIGFLAG);
	delay_ms(5);
	op_w(hc, OP_USBSTS, STS_INTMASK);
	op_w(hc, OP_USBINTR, STS_USBINT | STS_ERRINT | STS_PCD | STS_FLR | STS_HSE | STS_IAA);
	if (hc->hcs & 0x10)
	{
		for (p = 0; p < hc->nports; p++)
			port_w(hc, p, PS_PP, 0);
		delay_ms(20);
	}
	logf("kexehci: running, USBSTS %08x\n", op_r(hc, OP_USBSTS));
	(void) pa;
	/* the hub thread */
	hc->stop = 0;
	hc->started_ms = now_ms();
	st = PsCreateSystemThread(&hc->thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, hub_thread, hc);
	if (!NT_SUCCESS(st))
	{
		logf("kexehci: PsCreateSystemThread %08x\n", st);
		hc_halt(hc);
		return st;
	}
	ObReferenceObjectByHandle(hc->thread, THREAD_ALL_ACCESS, NULL, KernelMode, &hc->thread_obj, NULL);
	hc->started = 1;
	return STATUS_SUCCESS;
}

static void hc_stop(HC *hc)
{
	if (!hc->started)
		return;
	hc->stop = 1;
	KeSetEvent(&hc->ev, 0, FALSE);
	if (hc->thread_obj)
	{
		KeWaitForSingleObject(hc->thread_obj, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(hc->thread_obj);
		hc->thread_obj = NULL;
	}
	if (hc->thread)
	{
		ZwClose(hc->thread);
		hc->thread = NULL;
	}
	hc_halt(hc);
	if (hc->intr)
	{
		IoDisconnectInterrupt(hc->intr);
		hc->intr = NULL;
	}
	hc->started = 0;
}

static NTSTATUS NTAPI on_done(PDEVICE_OBJECT d, PIRP irp, PVOID ev)
{
	(void) d;
	(void) irp;
	KeSetEvent((PKEVENT) ev, 0, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS call_down_sync(HC *hc, PIRP irp)
{
	KEVENT ev;
	NTSTATUS st;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(irp);
	IoSetCompletionRoutine(irp, on_done, &ev, TRUE, TRUE, TRUE);
	st = IoCallDriver(hc->lower, irp);
	if (st == STATUS_PENDING)
	{
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = irp->IoStatus.Status;
	}
	return st;
}

static NTSTATUS fdo_pnp(FDOEXT *fe, PIRP irp)
{
	HC *hc = &fe->hc;
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;
	switch (sl->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
		st = call_down_sync(hc, irp);
		if (NT_SUCCESS(st))
			st = hc_start(hc, sl->Parameters.StartDevice.AllocatedResources,
			              sl->Parameters.StartDevice.AllocatedResourcesTranslated);
		irp->IoStatus.Status = st;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	case IRP_MN_QUERY_STOP_DEVICE:
		/* no resource rebalancing while devices run */
		irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_UNSUCCESSFUL;
	case IRP_MN_QUERY_DEVICE_RELATIONS:
		if (sl->Parameters.QueryDeviceRelations.Type == BusRelations)
		{
			PDEVICE_RELATIONS r =
			    (PDEVICE_RELATIONS) ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) + 128 * sizeof(PVOID), TAG);
			int p;
			if (!r)
				break;
			r->Count = 0;
			ExAcquireFastMutex(&hc->tree);
			for (p = 1; p <= hc->nports; p++)
				collect(hc->root[p], r);
			ExReleaseFastMutex(&hc->tree);
			irp->IoStatus.Information = (ULONG_PTR) r;
			irp->IoStatus.Status = STATUS_SUCCESS;
		}
		break;
	case IRP_MN_STOP_DEVICE:
		hc_stop(hc);
		irp->IoStatus.Status = STATUS_SUCCESS;
		break;
	case IRP_MN_SURPRISE_REMOVAL:
		hc_stop(hc);
		irp->IoStatus.Status = STATUS_SUCCESS;
		break;
	case IRP_MN_REMOVE_DEVICE:
	{
		PDEVICE_OBJECT lower = hc->lower;
		hc_stop(hc);
		irp->IoStatus.Status = STATUS_SUCCESS;
		IoSkipCurrentIrpStackLocation(irp);
		st = IoCallDriver(lower, irp);
		if (hc->regs)
			MmUnmapIoSpace(hc->regs, hc->regs_len);
		hc->regs = NULL;
		hc->op = NULL;
		if (hc->flist)
			MmFreeContiguousMemory((PVOID) hc->flist);
		if (hc->qhs)
			MmFreeContiguousMemory(hc->qhs);
		if (hc->tds)
			MmFreeContiguousMemory(hc->tds);
		IoDetachDevice(lower);
		IoDeleteDevice(hc->fdo);
		return st;
	}
	default:
		break;
	}
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(hc->lower, irp);
}

/* ---- dispatch ------------------------------------------------------------------------------- */
static NTSTATUS NTAPI d_pnp(PDEVICE_OBJECT dobj, PIRP irp)
{
	if (*(int *) dobj->DeviceExtension == EXT_FDO)
		return fdo_pnp((FDOEXT *) dobj->DeviceExtension, irp);
	return pdo_pnp((PDOEXT *) dobj->DeviceExtension, irp);
}

static NTSTATUS NTAPI d_power(PDEVICE_OBJECT dobj, PIRP irp)
{
	PoStartNextPowerIrp(irp);
	if (*(int *) dobj->DeviceExtension == EXT_FDO)
	{
		IoSkipCurrentIrpStackLocation(irp);
		return PoCallDriver(((FDOEXT *) dobj->DeviceExtension)->hc.lower, irp);
	}
	{
		PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
		NTSTATUS st = irp->IoStatus.Status;
		if (sl->MinorFunction == IRP_MN_SET_POWER || sl->MinorFunction == IRP_MN_QUERY_POWER)
			st = STATUS_SUCCESS;
		else if (sl->MinorFunction == IRP_MN_WAIT_WAKE)
			st = STATUS_NOT_SUPPORTED;
		irp->IoStatus.Status = st;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	}
}

static NTSTATUS NTAPI d_internal(PDEVICE_OBJECT dobj, PIRP irp)
{
	if (*(int *) dobj->DeviceExtension == EXT_FDO)
	{
		IoSkipCurrentIrpStackLocation(irp);
		return IoCallDriver(((FDOEXT *) dobj->DeviceExtension)->hc.lower, irp);
	}
	return pdo_ioctl((PDOEXT *) dobj->DeviceExtension, irp);
}

static NTSTATUS NTAPI d_other(PDEVICE_OBJECT dobj, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;
	if (*(int *) dobj->DeviceExtension == EXT_FDO)
	{
		if (sl->MajorFunction == IRP_MJ_CREATE || sl->MajorFunction == IRP_MJ_CLOSE)
		{
			irp->IoStatus.Status = STATUS_SUCCESS;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			return STATUS_SUCCESS;
		}
		IoSkipCurrentIrpStackLocation(irp);
		return IoCallDriver(((FDOEXT *) dobj->DeviceExtension)->hc.lower, irp);
	}
	st = sl->MajorFunction == IRP_MJ_CREATE || sl->MajorFunction == IRP_MJ_CLOSE ? STATUS_SUCCESS : irp->IoStatus.Status;
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static NTSTATUS NTAPI d_shutdown(PDEVICE_OBJECT dobj, PIRP irp)
{
	if (*(int *) dobj->DeviceExtension == EXT_FDO)
		hc_halt(&((FDOEXT *) dobj->DeviceExtension)->hc);
	irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	PDEVICE_OBJECT fdo;
	FDOEXT *fe;
	HC *hc;
	NTSTATUS st = IoCreateDevice(drv, sizeof(FDOEXT), NULL, FILE_DEVICE_CONTROLLER, 0, FALSE, &fdo);
	if (!NT_SUCCESS(st))
		return st;
	fe = (FDOEXT *) fdo->DeviceExtension;
	memset(fe, 0, sizeof(*fe));
	fe->kind = EXT_FDO;
	hc = &fe->hc;
	hc->fdo = fdo;
	hc->pdo = pdo;
	hc->index = hc_count++;
	hc->pci_bus = 0xffffffff;
	KeInitializeSpinLock(&hc->lock);
	ExInitializeFastMutex(&hc->tree);
	KeInitializeEvent(&hc->ev, SynchronizationEvent, FALSE);
	KeInitializeEvent(&hc->iaa, NotificationEvent, FALSE);
	InitializeListHead(&hc->eps);
	InitializeListHead(&hc->work);
	hc->lower = IoAttachDeviceToDeviceStack(fdo, pdo);
	if (!hc->lower)
	{
		IoDeleteDevice(fdo);
		return STATUS_NO_SUCH_DEVICE;
	}
	IoRegisterShutdownNotification(fdo);
	fdo->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}

static VOID NTAPI unload(PDRIVER_OBJECT drv)
{
	(void) drv;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	int i;
	(void) reg;
	g_drv = drv;
	KeInitializeSpinLock(&log_lock);
	logf("kexehci: KernelExEx USB 2.0 driver\n");
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv->MajorFunction[i] = d_other;
	drv->MajorFunction[IRP_MJ_PNP] = d_pnp;
	drv->MajorFunction[IRP_MJ_POWER] = d_power;
	drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = d_internal;
	drv->MajorFunction[IRP_MJ_SHUTDOWN] = d_shutdown;
	drv->DriverExtension->AddDevice = add_device;
	drv->DriverUnload = unload;
	return STATUS_SUCCESS;
}
