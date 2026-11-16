/*
 *  KernelExEx - kexahci.mpd: SATA disks and CD/DVD drives on AHCI controllers
 *  for Windows 98 SE / ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  An NT SCSI miniport, loaded by Windows 98's SCSIPORT.PDR like kexata.mpd.
 *  Windows 98 has no driver for SATA controllers in AHCI mode (every board
 *  since about 2008 defaults to it, many cannot switch it off): their disks
 *  are simply not there. Each SATA port is a SCSI target; Windows' own disk
 *  and CD-ROM drivers sit on top and send SCSI commands, which become ATA
 *  commands (disks, always 48-bit LBA when the disk has it) or ATAPI packets
 *  (CD/DVD drives), both by DMA through the port's command list.
 *
 *  First version: one command at a time per adapter, completion polled in
 *  StartIo (the controller's interrupt stays off), ports up to 8.
 *
 *  Reference: Serial ATA AHCI 1.3.1 (Intel), ATA/ATAPI-8 (T13), SCSI
 *  SPC/SBC for the translation (as in kexata.c).
 */
#include <ntddk.h>
#undef _NTDDK_   /* srb.h: ScsiPort functions as imports from SCSIPORT.SYS */
#include <srb.h>
#include <scsi.h>
#include <stdarg.h>

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

/* ---- debug output (QEMU -debugcon) -------------------------------------------------- */
#ifdef KEX_DEBUGCON
static void dc(char c)
{
	__asm__ __volatile__("outb %0, $0xe9" : : "a"(c));
}

static void dn(unsigned long v, unsigned base, int w)
{
	char b[12];
	int i = 0;
	do
	{
		unsigned d = (unsigned) (v % base);
		b[i++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v);
	while (w-- > i)
		dc('0');
	while (i--)
		dc(b[i]);
}

static void logf(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	for (; *f; f++)
	{
		int w = 0;
		if (*f != '%')
		{
			dc(*f);
			continue;
		}
		f++;
		while (*f >= '0' && *f <= '9')
			w = w * 10 + *f++ - '0';
		if (*f == 's')
		{
			const char *p = va_arg(ap, const char *);
			while (*p)
				dc(*p++);
		}
		else if (*f == 'x')
			dn(va_arg(ap, unsigned long), 16, w);
		else
			dn(va_arg(ap, unsigned long), 10, w);
	}
	va_end(ap);
}
#else
#define logf(...) ((void) 0)
#endif

/* ---- AHCI registers ------------------------------------------------------------------ */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define HBA_VS 0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

#define CAP_SSS (1u << 27)   /* staggered spin-up: ports need SUD */
#define CAP_S64A (1u << 31)
#define GHC_HR 0x1
#define GHC_IE 0x2
#define GHC_AE 0x80000000u
#define CAP2_BOH 0x1
#define BOHC_BOS 0x1
#define BOHC_OOS 0x2
#define BOHC_BB 0x10

#define P_CLB 0x00
#define P_CLBU 0x04
#define P_FB 0x08
#define P_FBU 0x0C
#define P_IS 0x10
#define P_IE 0x14
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SCTL 0x2C
#define P_SERR 0x30
#define P_SACT 0x34
#define P_CI 0x38

#define PCMD_ST 0x0001
#define PCMD_SUD 0x0002
#define PCMD_POD 0x0004
#define PCMD_FRE 0x0010
#define PCMD_FR 0x4000
#define PCMD_CR 0x8000
#define PCMD_ICC_ACTIVE 0x10000000u

#define PIS_TFES (1u << 30)   /* task file error */
#define PIS_FATAL 0x78000000u /* host bus fatal / data / interface errors */

#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80

#define SIG_ATAPI 0xEB140101u

/* ---- ATA ------------------------------------------------------------------------------ */
#define CMD_READ_DMA 0xC8
#define CMD_READ_DMA_EXT 0x25
#define CMD_WRITE_DMA 0xCA
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_PACKET 0xA0
#define CMD_IDENTIFY_PACKET 0xA1
#define CMD_FLUSH 0xE7
#define CMD_FLUSH_EXT 0xEA
#define CMD_IDENTIFY 0xEC
#define CMD_READ_FPDMA 0x60
#define CMD_WRITE_FPDMA 0x61
#define CAP_SNCQ (1u << 30)

#define ER_ABRT 0x04
#define ER_IDNF 0x10
#define ER_UNC 0x40
#define ER_ICRC 0x80

#define MAX_PORTS 8
#define NPRD 33                 /* 128 KB of 4 KB pages, plus one for an unaligned start */
#define MAX_XFER 0x20000

/* per port, in the uncached extension: command list (1 KB, 1 KB aligned),
 * received FISes (256 bytes, 256 aligned), one command table (128 aligned) */
#define CL_SIZE 0x400
#define FIS_SIZE 0x100
#define CT_SIZE (0x80 + NPRD * 16)
#define NSLOT 8                 /* queued (NCQ) commands per disk */
#define CT_STRIDE 0x300         /* CT_SIZE rounded to the 128-byte alignment */
#define PORT_MEM 0x2000         /* CL + FIS + NSLOT command tables, so every part stays aligned */
#define IDBUF_SIZE 512

typedef struct
{
	int present, atapi, lba48, removable;
	ULONGLONG sectors;
	char model[41], fw[9];
	UCHAR sense_key, asc, ascq;
	UCHAR cdb_len;
	int port;
	int tid;
	/* the command in flight (interrupt mode), not queued */
	PSCSI_REQUEST_BLOCK srb;
	int kind;
	ULONG bytes;
	/* queued (NCQ) reads and writes in flight, by tag */
	int ncq, qdepth;
	ULONG qactive;
	struct
	{
		PSCSI_REQUEST_BLOCK srb;
		ULONGLONG lba;
		ULONG count;
		int write;
	} q[NSLOT];
	/* requests waiting for the drive (Windows sends several at once) */
	PSCSI_REQUEST_BLOCK wait[NSLOT + 4];
	int nwait;
	/* the port's memory */
	PULONG cl;
	PUCHAR fis, ct;
	ULONG cl_pa, fis_pa, ct_pa;
} DRIVE;

typedef struct
{
	PUCHAR abar;
	ULONG cap, pi, nports;
	DRIVE d[MAX_PORTS];   /* by target id */
	int ntargets;
	PUCHAR idbuf;
	ULONG idbuf_pa;
	int depth, want_next;
	ULONG want_lu;   /* targets to ask Windows for another request (NextLuRequest) */
	int irq_mode;   /* commands complete in the interrupt (after HwInitialize) */
	int no_irq;     /* no IRQ from Windows, or AdapterSettings "POLL" */
	int no_ncq;     /* AdapterSettings "NONCQ" */
} HBA;

static ULONG r32(HBA *h, ULONG off)
{
	return ScsiPortReadRegisterUlong((PULONG) (h->abar + off));
}

static void w32(HBA *h, ULONG off, ULONG v)
{
	ScsiPortWriteRegisterUlong((PULONG) (h->abar + off), v);
}

static ULONG pr(HBA *h, DRIVE *d, ULONG reg)
{
	return r32(h, 0x100 + d->port * 0x80 + reg);
}

static void pw(HBA *h, DRIVE *d, ULONG reg, ULONG v)
{
	w32(h, 0x100 + d->port * 0x80 + reg, v);
}

/* ---- the request flow (as kexata: NextRequest only from the outermost call) ----------- */
static void set_sense(DRIVE *d, UCHAR key, UCHAR asc, UCHAR ascq)
{
	d->sense_key = key;
	d->asc = asc;
	d->ascq = ascq;
}

static void finish(HBA *h, PSCSI_REQUEST_BLOCK srb, UCHAR status)
{
	srb->SrbStatus = status;
	srb->ScsiStatus = status == SRB_STATUS_ERROR ? SCSISTAT_CHECK_CONDITION : SCSISTAT_GOOD;
	ScsiPortNotification(RequestComplete, h, srb);
	h->want_next = 1;
}

static void enter(HBA *h)
{
	h->depth++;
}

static void leave(HBA *h)
{
	if (h->depth == 1)
		while (h->want_next || h->want_lu)
		{
			ULONG lu = h->want_lu, t;
			h->want_lu = 0;
			for (t = 0; t < MAX_PORTS; t++)
				if (lu & (1u << t))
					ScsiPortNotification(NextLuRequest, h, 0, (UCHAR) t, 0);
			if (h->want_next)
			{
				h->want_next = 0;
				ScsiPortNotification(NextRequest, h, NULL);
			}
		}
	h->depth--;
}

static void fail_sense(HBA *h, PSCSI_REQUEST_BLOCK srb, DRIVE *d, UCHAR key, UCHAR asc)
{
	set_sense(d, key, asc, 0);
	srb->DataTransferLength = 0;
	finish(h, srb, SRB_STATUS_ERROR);
}

static void reply(PSCSI_REQUEST_BLOCK srb, const void *data, ULONG len)
{
	if (len > srb->DataTransferLength)
		len = srb->DataTransferLength;
	memcpy(srb->DataBuffer, data, len);
	srb->DataTransferLength = len;
}

static void put_be32(UCHAR *p, ULONG v)
{
	p[0] = (UCHAR) (v >> 24);
	p[1] = (UCHAR) (v >> 16);
	p[2] = (UCHAR) (v >> 8);
	p[3] = (UCHAR) v;
}

/* ---- ports ------------------------------------------------------------------------------ */
static int wait_clear(HBA *h, DRIVE *d, ULONG reg, ULONG bits, ULONG ms)
{
	ULONG i;
	for (i = 0; i < ms * 10; i++)
	{
		if (!(pr(h, d, reg) & bits))
			return 1;
		ScsiPortStallExecution(100);
	}
	return 0;
}

static void port_stop(HBA *h, DRIVE *d)
{
	ULONG cmd = pr(h, d, P_CMD);
	if (cmd & PCMD_ST)
		pw(h, d, P_CMD, cmd & ~PCMD_ST);
	wait_clear(h, d, P_CMD, PCMD_CR, 500);
	cmd = pr(h, d, P_CMD);
	if (cmd & PCMD_FRE)
		pw(h, d, P_CMD, cmd & ~PCMD_FRE);
	wait_clear(h, d, P_CMD, PCMD_FR, 500);
}

static void port_start(HBA *h, DRIVE *d)
{
	pw(h, d, P_SERR, 0xFFFFFFFFu);
	pw(h, d, P_IS, 0xFFFFFFFFu);
	pw(h, d, P_CMD, pr(h, d, P_CMD) | PCMD_FRE);
	/* a device that is still busy (or wants DRQ) has to settle first */
	wait_clear(h, d, P_TFD, TFD_BSY | TFD_DRQ, 2000);
	pw(h, d, P_CMD, pr(h, d, P_CMD) | PCMD_ST);
}

/* COMRESET: when a port's device is stuck (after an error it did not clear) */
static void port_reset(HBA *h, DRIVE *d)
{
	port_stop(h, d);
	pw(h, d, P_SCTL, (pr(h, d, P_SCTL) & ~0xFu) | 1);
	ScsiPortStallExecution(2000);
	pw(h, d, P_SCTL, pr(h, d, P_SCTL) & ~0xFu);
	{
		ULONG i;
		for (i = 0; i < 1000 && (pr(h, d, P_SSTS) & 0xF) != 3; i++)
			ScsiPortStallExecution(1000);
	}
	port_start(h, d);
}

/* scatter/gather: the PRDs of the command table; 0 when the buffer will not fit */
static int build_prdt(HBA *h, PUCHAR ct, PSCSI_REQUEST_BLOCK srb, PUCHAR va, ULONG len, ULONG *n)
{
	PULONG prd = (PULONG) (ct + 0x80);
	ULONG i = 0;
	while (len)
	{
		ULONG run = 0, pa;
		SCSI_PHYSICAL_ADDRESS p = srb ? ScsiPortGetPhysicalAddress(h, srb, va, &run)
		                              : ScsiPortGetPhysicalAddress(h, NULL, va, &run);
		pa = ScsiPortConvertPhysicalAddressToUlong(p);
		if (!run || run > len)
			run = len;
		if (i >= NPRD || (pa & 1) || (run & 1))
			return 0;
		prd[i * 4 + 0] = pa;
		prd[i * 4 + 1] = 0;
		prd[i * 4 + 2] = 0;
		prd[i * 4 + 3] = run - 1;
		i++;
		va += run;
		len -= run;
	}
	*n = i;
	return 1;
}

/* one command through slot 0; waits for it. Returns the task file (status in
 * bits 0-7, error in 8-15); TFD_ERR set on any failure, 0x80 (BSY) on time out */
static ULONG run_cmd(HBA *h, DRIVE *d, const UCHAR *fis, const UCHAR *acmd, int write, ULONG nprd, ULONG ms)
{
	ULONG i, is, flags;
	memset(d->ct, 0, 0x80);
	memcpy(d->ct, fis, 20);
	if (acmd)
		memcpy(d->ct + 0x40, acmd, 16);
	flags = 5 | (acmd ? 0x20 : 0) | (write ? 0x40 : 0) | (nprd << 16);
	d->cl[0] = flags;
	d->cl[1] = 0;
	d->cl[2] = d->ct_pa;
	d->cl[3] = 0;
	pw(h, d, P_IS, 0xFFFFFFFFu);
	pw(h, d, P_CI, 1);
	for (i = 0; i < ms * 20; i++)
	{
		is = pr(h, d, P_IS);
		if (is & (PIS_TFES | PIS_FATAL))
			break;
		if (!(pr(h, d, P_CI) & 1))
			return pr(h, d, P_TFD) & 0xFFFF;
		ScsiPortStallExecution(50);
	}
	{
		ULONG tfd = pr(h, d, P_TFD) & 0xFFFF;
		logf("kexahci: port %u: command %x failed, IS %x TFD %x SERR %x\n", d->port, fis[2], pr(h, d, P_IS), tfd,
		     pr(h, d, P_SERR));
		/* the port stops on an error: restart it (a reset when the device stays stuck) */
		port_stop(h, d);
		if (pr(h, d, P_TFD) & (TFD_BSY | TFD_DRQ))
			port_reset(h, d);
		else
			port_start(h, d);
		if (i >= ms * 20)
			return TFD_BSY | TFD_ERR;
		return tfd | TFD_ERR;
	}
}

static void h2d(UCHAR *f, UCHAR cmd, ULONGLONG lba, ULONG count, UCHAR feature)
{
	memset(f, 0, 20);
	f[0] = 0x27;   /* register host to device */
	f[1] = 0x80;   /* command */
	f[2] = cmd;
	f[3] = feature;
	f[4] = (UCHAR) lba;
	f[5] = (UCHAR) (lba >> 8);
	f[6] = (UCHAR) (lba >> 16);
	f[7] = 0x40;   /* LBA */
	f[8] = (UCHAR) (lba >> 24);
	f[9] = (UCHAR) (lba >> 32);
	f[10] = (UCHAR) (lba >> 40);
	f[12] = (UCHAR) count;
	f[13] = (UCHAR) (count >> 8);
}

static void ata_string(char *out, const USHORT *w, int words)
{
	int i, n = words * 2;
	for (i = 0; i < words; i++)
	{
		out[i * 2] = (char) (w[i] >> 8);
		out[i * 2 + 1] = (char) w[i];
	}
	out[n] = 0;
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = 0;
}

static void identify(HBA *h, DRIVE *d)
{
	UCHAR f[20];
	USHORT *id = (USHORT *) h->idbuf;
	PULONG prd = (PULONG) (d->ct + 0x80);
	ULONG t;
	memset(h->idbuf, 0, IDBUF_SIZE);
	prd[0] = h->idbuf_pa;
	prd[1] = prd[2] = 0;
	prd[3] = IDBUF_SIZE - 1;
	h2d(f, d->atapi ? CMD_IDENTIFY_PACKET : CMD_IDENTIFY, 0, 0, 0);
	f[7] = 0;
	t = run_cmd(h, d, f, NULL, 0, 1, 3000);
	if (t & TFD_ERR)
	{
		logf("kexahci: port %u: IDENTIFY failed (%x)\n", d->port, t);
		return;
	}
	ata_string(d->model, id + 27, 20);
	ata_string(d->fw, id + 23, 4);
	d->removable = (id[0] & 0x80) != 0;
	if (d->atapi)
	{
		d->cdb_len = (id[0] & 3) == 1 ? 16 : 12;
		d->present = 1;
		logf("kexahci: port %u: ATAPI %s\n", d->port, d->model);
		return;
	}
	d->lba48 = (id[83] & (1 << 10)) != 0;
	if (d->lba48)
		d->sectors = (ULONGLONG) id[100] | ((ULONGLONG) id[101] << 16) | ((ULONGLONG) id[102] << 32) |
		             ((ULONGLONG) id[103] << 48);
	else
		d->sectors = (ULONG) id[60] | ((ULONG) id[61] << 16);
	d->present = d->sectors != 0;
	if ((id[76] & 0x100) && (h->cap & CAP_SNCQ) && d->lba48 && !h->no_ncq)
	{
		int dq = (id[75] & 0x1F) + 1, hq = (int) ((h->cap >> 8) & 0x1F) + 1;
		d->qdepth = dq < hq ? dq : hq;
		if (d->qdepth > NSLOT)
			d->qdepth = NSLOT;
		d->ncq = d->qdepth > 1;
	}
	logf("kexahci: port %u: disk %s, %u MB, %s, NCQ %d\n", d->port, d->model, (ULONG) (d->sectors >> 11),
	     d->lba48 ? "48-bit" : "28-bit", d->ncq ? d->qdepth : 0);
}

/* ---- issuing: at once and completed in the interrupt, or (before Windows turns
 * interrupts on, and as a fallback) waited for here ------------------------------- */
enum { K_DISK = 1, K_FLUSH, K_ATAPI };

static void done_disk(HBA *h, DRIVE *d, PSCSI_REQUEST_BLOCK srb, ULONG t);
static void done_atapi(HBA *h, DRIVE *d, PSCSI_REQUEST_BLOCK srb, ULONG t);

static void complete(HBA *h, DRIVE *d, ULONG t)
{
	PSCSI_REQUEST_BLOCK srb = d->srb;
	int kind = d->kind;
	d->srb = NULL;
	if (!srb)
		return;
	if (kind == K_DISK)
		done_disk(h, d, srb, t);
	else if (kind == K_ATAPI)
		done_atapi(h, d, srb, t);
	else
	{
		srb->DataTransferLength = 0;
		finish(h, srb, SRB_STATUS_SUCCESS);
	}
}

#define PIE_ALL 0x7DC0007Fu   /* every completion and error interrupt of a port */

static void start_cmd(HBA *h, DRIVE *d, PSCSI_REQUEST_BLOCK srb, const UCHAR *fis, const UCHAR *acmd, int write,
                      ULONG nprd, int kind, ULONG bytes, ULONG ms)
{
	d->srb = srb;
	d->kind = kind;
	d->bytes = bytes;
	if (!h->irq_mode)
	{
		complete(h, d, run_cmd(h, d, fis, acmd, write, nprd, ms));
		return;
	}
	memset(d->ct, 0, 0x80);
	memcpy(d->ct, fis, 20);
	if (acmd)
		memcpy(d->ct + 0x40, acmd, 16);
	d->cl[0] = 5 | (acmd ? 0x20 : 0) | (write ? 0x40 : 0) | (nprd << 16);
	d->cl[1] = 0;
	d->cl[2] = d->ct_pa;
	d->cl[3] = 0;
	pw(h, d, P_IS, 0xFFFFFFFFu);
	pw(h, d, P_CI, 1);
	/* this port is busy; the others can take requests meanwhile */
	h->want_next = 1;
}

static int popcount(ULONG v);

/* ---- disks ------------------------------------------------------------------------------ */
/* a read or write as a queued (NCQ) command, in a free tag; pump() has made
 * sure there is one and that it does not overlap a write in flight */
static void rw_queued(HBA *h, PSCSI_REQUEST_BLOCK srb, DRIVE *d, ULONGLONG lba, ULONG count, int write)
{
	ULONG tag, nprd, bytes = count * 512, bit;
	PUCHAR ct;
	PULONG cl;
	for (tag = 0; tag < (ULONG) d->qdepth && (d->qactive & (1u << tag)); tag++)
		;
	ct = d->ct + tag * CT_STRIDE;
	if (tag >= (ULONG) d->qdepth || bytes > srb->DataTransferLength || bytes > MAX_XFER ||
	    !build_prdt(h, ct, srb, (PUCHAR) srb->DataBuffer, bytes, &nprd))
	{
		fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	memset(ct, 0, 0x80);
	ct[0] = 0x27;
	ct[1] = 0x80;
	ct[2] = write ? CMD_WRITE_FPDMA : CMD_READ_FPDMA;
	ct[3] = (UCHAR) count;   /* the count goes in the features */
	ct[11] = (UCHAR) (count >> 8);
	ct[4] = (UCHAR) lba;
	ct[5] = (UCHAR) (lba >> 8);
	ct[6] = (UCHAR) (lba >> 16);
	ct[7] = 0x40;
	ct[8] = (UCHAR) (lba >> 24);
	ct[9] = (UCHAR) (lba >> 32);
	ct[10] = (UCHAR) (lba >> 40);
	ct[12] = (UCHAR) (tag << 3);
	cl = d->cl + tag * 8;
	cl[0] = 5 | (write ? 0x40 : 0) | (nprd << 16);
	cl[1] = 0;
	cl[2] = d->ct_pa + tag * CT_STRIDE;
	cl[3] = 0;
	d->q[tag].srb = srb;
	d->q[tag].lba = lba;
	d->q[tag].count = count;
	d->q[tag].write = write;
	bit = 1u << tag;
	d->qactive |= bit;
	{
		static int most;
		if (popcount(d->qactive) > most)
			most = popcount(d->qactive), logf("kexahci: port %u: %d queued\n", d->port, most);
	}
	pw(h, d, P_SACT, bit);
	pw(h, d, P_CI, bit);
	h->want_next = 1;
}

static void rw(HBA *h, PSCSI_REQUEST_BLOCK srb, DRIVE *d, ULONGLONG lba, ULONG count, int write)
{
	UCHAR f[20];
	ULONG nprd, bytes = count * 512;
	if (!count)
	{
		srb->DataTransferLength = 0;
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	}
	if (lba + count > d->sectors)
	{
		fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x21);
		return;
	}
	if (h->irq_mode && d->ncq)
	{
		rw_queued(h, srb, d, lba, count, write);
		return;
	}
	if (bytes > srb->DataTransferLength || bytes > MAX_XFER ||
	    !build_prdt(h, d->ct, srb, (PUCHAR) srb->DataBuffer, bytes, &nprd))
	{
		fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	if (d->lba48)
		h2d(f, write ? CMD_WRITE_DMA_EXT : CMD_READ_DMA_EXT, lba, count, 0);
	else
	{
		h2d(f, write ? CMD_WRITE_DMA : CMD_READ_DMA, lba & 0x0FFFFFFF, count & 0xFF, 0);
		f[7] = (UCHAR) (0x40 | ((lba >> 24) & 0x0F));
		f[8] = f[9] = f[10] = 0;
	}
	start_cmd(h, d, srb, f, NULL, write, nprd, K_DISK, bytes, 10000);
}

static void done_disk(HBA *h, DRIVE *d, PSCSI_REQUEST_BLOCK srb, ULONG t)
{
	if (t & TFD_ERR)
	{
		UCHAR er = (UCHAR) (t >> 8);
		if (t & TFD_BSY)
			set_sense(d, SCSI_SENSE_NOT_READY, 0x04, 0);
		else if (er & ER_UNC)
			set_sense(d, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0);
		else if (er & ER_IDNF)
			set_sense(d, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
		else if (er & ER_ICRC)
			set_sense(d, SCSI_SENSE_ABORTED_COMMAND, 0x47, 0);
		else
			set_sense(d, SCSI_SENSE_ABORTED_COMMAND, 0, 0);
		srb->DataTransferLength = 0;
		finish(h, srb, SRB_STATUS_ERROR);
		return;
	}
	srb->DataTransferLength = d->bytes;
	finish(h, srb, SRB_STATUS_SUCCESS);
}

static void flush(HBA *h, PSCSI_REQUEST_BLOCK srb, DRIVE *d)
{
	UCHAR f[20];
	h2d(f, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH, 0, 0, 0);
	f[7] = 0;
	start_cmd(h, d, srb, f, NULL, 0, 0, K_FLUSH, 0, 30000);
}

static void ata_scsi(HBA *h, PSCSI_REQUEST_BLOCK srb, DRIVE *d)
{
	UCHAR *cdb = srb->Cdb;
	UCHAR b[40];
	memset(b, 0, sizeof(b));
	switch (cdb[0])
	{
	case SCSIOP_TEST_UNIT_READY:
	case SCSIOP_START_STOP_UNIT:
	case SCSIOP_MEDIUM_REMOVAL:
	case SCSIOP_RESERVE_UNIT:
	case SCSIOP_RELEASE_UNIT:
	case SCSIOP_VERIFY:
	case SCSIOP_SEEK:
	case SCSIOP_REZERO_UNIT:
		srb->DataTransferLength = 0;
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_REQUEST_SENSE:
		b[0] = 0x70;
		b[2] = d->sense_key;
		b[7] = 10;
		b[12] = d->asc;
		b[13] = d->ascq;
		set_sense(d, 0, 0, 0);
		reply(srb, b, 18);
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_INQUIRY:
	{
		int i;
		if (cdb[1] & 1)
		{
			fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
			return;
		}
		b[1] = d->removable ? 0x80 : 0;
		b[2] = 2;
		b[3] = 2;
		b[4] = 31;
		memcpy(b + 8, "ATA     ", 8);
		for (i = 0; i < 16; i++)
			b[16 + i] = (UCHAR) (d->model[i] ? d->model[i] : ' ');
		for (i = 0; i < 4; i++)
			b[32 + i] = (UCHAR) (d->fw[i] ? d->fw[i] : ' ');
		reply(srb, b, 36);
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	}
	case SCSIOP_READ_CAPACITY:
	{
		ULONGLONG last = d->sectors - 1;
		put_be32(b, last > 0xffffffffu ? 0xffffffffu : (ULONG) last);
		put_be32(b + 4, 512);
		reply(srb, b, 8);
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	}
	case SCSIOP_MODE_SENSE:
		b[0] = 3;
		reply(srb, b, 4);
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_MODE_SENSE10:
		b[1] = 6;
		reply(srb, b, 8);
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_READ6:
	case SCSIOP_WRITE6:
	{
		ULONG lba = ((ULONG) (cdb[1] & 0x1f) << 16) | ((ULONG) cdb[2] << 8) | cdb[3];
		rw(h, srb, d, lba, cdb[4] ? cdb[4] : 256, cdb[0] == SCSIOP_WRITE6);
		return;
	}
	case SCSIOP_READ:
	case SCSIOP_WRITE:
	{
		ULONG lba = ((ULONG) cdb[2] << 24) | ((ULONG) cdb[3] << 16) | ((ULONG) cdb[4] << 8) | cdb[5];
		rw(h, srb, d, lba, ((ULONG) cdb[7] << 8) | cdb[8], cdb[0] == SCSIOP_WRITE);
		return;
	}
	case SCSIOP_READ12:
	case SCSIOP_WRITE12:
	{
		ULONG lba = ((ULONG) cdb[2] << 24) | ((ULONG) cdb[3] << 16) | ((ULONG) cdb[4] << 8) | cdb[5];
		ULONG n = ((ULONG) cdb[6] << 24) | ((ULONG) cdb[7] << 16) | ((ULONG) cdb[8] << 8) | cdb[9];
		rw(h, srb, d, lba, n, cdb[0] == SCSIOP_WRITE12);
		return;
	}
	case SCSIOP_SYNCHRONIZE_CACHE:
		flush(h, srb, d);
		return;
	default:
		fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x20);
		return;
	}
}

/* ---- CD/DVD drives: the SCSI command as an ATAPI packet, by DMA ------------------------- */
static void atapi_scsi(HBA *h, PSCSI_REQUEST_BLOCK srb, DRIVE *d)
{
	UCHAR f[20], cdb[16];
	ULONG nprd = 0, len = srb->DataTransferLength;
	int write = (srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0;
	memset(cdb, 0, sizeof(cdb));
	memcpy(cdb, srb->Cdb, srb->CdbLength > 16 ? 16 : srb->CdbLength);
	if (cdb[0] == SCSIOP_MODE_SENSE || cdb[0] == SCSIOP_MODE_SELECT)
	{
		/* ATAPI knows only the 10-byte forms; Windows then uses those */
		fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x20);
		return;
	}
	if (!(srb->SrbFlags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)))
		len = 0;
	if (len && (len > MAX_XFER || !build_prdt(h, d->ct, srb, (PUCHAR) srb->DataBuffer, len, &nprd)))
	{
		fail_sense(h, srb, d, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	h2d(f, CMD_PACKET, 0, 0, len ? 1 : 0);   /* feature bit 0: DMA */
	f[7] = 0;
	f[5] = (UCHAR) (len > 0xfffe ? 0xfe : len);
	f[6] = (UCHAR) ((len > 0xfffe ? 0xfffe : len) >> 8);
	start_cmd(h, d, srb, f, cdb, write, nprd, K_ATAPI, len, 20000);
}

static void done_atapi(HBA *h, DRIVE *d, PSCSI_REQUEST_BLOCK srb, ULONG t)
{
	ULONG len = d->bytes;
	if (t & TFD_ERR)
	{
		/* the drive keeps its sense data: Windows asks for it with REQUEST SENSE */
		srb->DataTransferLength = 0;
		srb->SrbStatus = SRB_STATUS_ERROR;
		srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
		ScsiPortNotification(RequestComplete, h, srb);
		h->want_next = 1;
		return;
	}
	if (len)
		srb->DataTransferLength = d->cl[1] ? d->cl[1] : len;   /* PRDBC: bytes moved */
	finish(h, srb, SRB_STATUS_SUCCESS);
}

/* ---- the miniport ---------------------------------------------------------------------- */
static void startio_body(HBA *h, PSCSI_REQUEST_BLOCK srb)
{
	DRIVE *d;
	switch (srb->Function)
	{
	case SRB_FUNCTION_EXECUTE_SCSI:
		break;
	case SRB_FUNCTION_SHUTDOWN:
	case SRB_FUNCTION_FLUSH:
		if (srb->TargetId < h->ntargets && h->d[srb->TargetId].present && !h->d[srb->TargetId].atapi)
		{
			flush(h, srb, &h->d[srb->TargetId]);
			return;
		}
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	case SRB_FUNCTION_RESET_BUS:
	case SRB_FUNCTION_RESET_DEVICE:
		finish(h, srb, SRB_STATUS_SUCCESS);
		return;
	case SRB_FUNCTION_ABORT_COMMAND:
		finish(h, srb, SRB_STATUS_ABORT_FAILED);
		return;
	default:
		finish(h, srb, SRB_STATUS_INVALID_REQUEST);
		return;
	}
	if (srb->PathId != 0 || srb->TargetId >= h->ntargets || srb->Lun != 0 || !h->d[srb->TargetId].present)
	{
		finish(h, srb, SRB_STATUS_SELECTION_TIMEOUT);
		return;
	}
	d = &h->d[srb->TargetId];
	if (d->atapi)
		atapi_scsi(h, srb, d);
	else
		ata_scsi(h, srb, d);
}

static ULONG be32(const UCHAR *b)
{
	return ((ULONG) b[0] << 24) | ((ULONG) b[1] << 16) | ((ULONG) b[2] << 8) | b[3];
}

/* the sectors a read or write covers; 0 for anything else */
static int rw_range(PSCSI_REQUEST_BLOCK srb, ULONGLONG *lba, ULONG *count, int *write)
{
	UCHAR *c = srb->Cdb;
	if (srb->Function != SRB_FUNCTION_EXECUTE_SCSI)
		return 0;
	switch (c[0])
	{
	case SCSIOP_READ6:
	case SCSIOP_WRITE6:
		*lba = ((ULONG) (c[1] & 0x1F) << 16) | ((ULONG) c[2] << 8) | c[3];
		*count = c[4] ? c[4] : 256;
		*write = c[0] == SCSIOP_WRITE6;
		return 1;
	case SCSIOP_READ:
	case SCSIOP_WRITE:
		*lba = be32(c + 2);
		*count = ((ULONG) c[7] << 8) | c[8];
		*write = c[0] == SCSIOP_WRITE;
		return 1;
	case SCSIOP_READ12:
	case SCSIOP_WRITE12:
		*lba = be32(c + 2);
		*count = be32(c + 6);
		*write = c[0] == SCSIOP_WRITE12;
		return 1;
	}
	return 0;
}

static int popcount(ULONG v)
{
	int n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

/* can this request go to the drive now? Anything when it is idle; with commands
 * queued, only more reads and writes that keep the order of overlapping writes */
static int can_start(HBA *h, DRIVE *d, PSCSI_REQUEST_BLOCK srb)
{
	ULONGLONG lba;
	ULONG count, t;
	int write;
	if (!h->irq_mode)
		return 1;
	if (d->srb)
		return 0;
	if (!d->qactive)
		return 1;
	if (!d->ncq || !rw_range(srb, &lba, &count, &write) || !count || popcount(d->qactive) >= d->qdepth)
		return 0;
	for (t = 0; t < NSLOT; t++)
		if ((d->qactive & (1u << t)) && (write || d->q[t].write) && lba < d->q[t].lba + d->q[t].count &&
		    d->q[t].lba < lba + count)
			return 0;
	return 1;
}

static void pump(HBA *h, DRIVE *d)
{
	while (d->nwait && can_start(h, d, d->wait[0]))
	{
		PSCSI_REQUEST_BLOCK srb = d->wait[0];
		int i;
		d->nwait--;
		for (i = 0; i < d->nwait; i++)
			d->wait[i] = d->wait[i + 1];
		startio_body(h, srb);
	}
	/* room for another: ask Windows for the next request to this disk */
	if (h->irq_mode && d->ncq && d->qactive && !d->nwait && !d->srb && popcount(d->qactive) < d->qdepth)
		h->want_lu |= 1u << d->tid;
}

static BOOLEAN NTAPI hw_startio(PVOID ext, PSCSI_REQUEST_BLOCK srb)
{
	HBA *h = (HBA *) ext;
	enter(h);
	if (srb->PathId == 0 && srb->Lun == 0 && srb->TargetId < h->ntargets && h->d[srb->TargetId].present &&
	    srb->Function != SRB_FUNCTION_ABORT_COMMAND)
	{
		DRIVE *d = &h->d[srb->TargetId];
		if (d->nwait >= (int) (sizeof(d->wait) / sizeof(d->wait[0])))
			finish(h, srb, SRB_STATUS_BUSY);
		else
		{
			d->wait[d->nwait++] = srb;
			pump(h, d);
		}
	}
	else
		startio_body(h, srb);
	leave(h);
	return TRUE;
}

static BOOLEAN NTAPI hw_interrupt(PVOID ext)
{
	HBA *h = (HBA *) ext;
	ULONG is = r32(h, HBA_IS), i;
	if (!is || is == 0xFFFFFFFFu)
		return FALSE;
	enter(h);
	for (i = 0; i < (ULONG) h->ntargets; i++)
	{
		DRIVE *d = &h->d[i];
		ULONG pis;
		if (!(is & (1u << d->port)))
			continue;
		pis = pr(h, d, P_IS);
		pw(h, d, P_IS, pis);
		if (d->qactive)
		{
			if (pis & (PIS_TFES | PIS_FATAL))
			{
				/* a queued command failed: the drive dropped the whole queue. Put the
				 * requests back in line and go on without queuing on this disk */
				ULONG t;
				logf("kexahci: port %u: queued command failed, IS %x TFD %x SERR %x SACT %x; NCQ off\n",
				     d->port, pis, pr(h, d, P_TFD) & 0xFFFF, pr(h, d, P_SERR), pr(h, d, P_SACT));
				port_stop(h, d);
				port_reset(h, d);
				pw(h, d, P_IS, 0xFFFFFFFFu);
				pw(h, d, P_IE, PIE_ALL);
				d->ncq = 0;
				for (t = 0; t < NSLOT; t++)
					if (d->qactive & (1u << t))
					{
						int k;
						for (k = d->nwait; k > 0; k--)
							d->wait[k] = d->wait[k - 1];
						d->wait[0] = d->q[t].srb;
						d->nwait++;
					}
				d->qactive = 0;
			}
			else
			{
				ULONG doneq = d->qactive & ~pr(h, d, P_SACT), t;
				for (t = 0; t < NSLOT; t++)
					if (doneq & (1u << t))
					{
						d->qactive &= ~(1u << t);
						d->q[t].srb->DataTransferLength = d->q[t].count * 512;
						finish(h, d->q[t].srb, SRB_STATUS_SUCCESS);
					}
			}
			pump(h, d);
			continue;
		}
		if (!d->srb)
			continue;
		if (pis & (PIS_TFES | PIS_FATAL))
		{
			ULONG tfd = pr(h, d, P_TFD) & 0xFFFF;
			logf("kexahci: port %u: command %x failed, IS %x TFD %x SERR %x\n", d->port, d->ct[2], pis, tfd,
			     pr(h, d, P_SERR));
			port_stop(h, d);
			if (pr(h, d, P_TFD) & (TFD_BSY | TFD_DRQ))
				port_reset(h, d);
			else
				port_start(h, d);
			complete(h, d, tfd | TFD_ERR);
		}
		else if (!(pr(h, d, P_CI) & 1))
			complete(h, d, pr(h, d, P_TFD) & 0xFFFF);
		pump(h, d);
	}
	w32(h, HBA_IS, is);
	leave(h);
	return TRUE;
}

static BOOLEAN NTAPI hw_reset(PVOID ext, ULONG path)
{
	HBA *h = (HBA *) ext;
	int i;
	logf("kexahci: bus reset\n");
	for (i = 0; i < h->ntargets; i++)
		if (h->d[i].srb || h->d[i].qactive)
		{
			h->d[i].srb = NULL;
			h->d[i].qactive = 0;
			port_stop(h, &h->d[i]);
			port_reset(h, &h->d[i]);
			pw(h, &h->d[i], P_IE, PIE_ALL);
		}
	for (i = 0; i < h->ntargets; i++)
		h->d[i].nwait = 0;
	ScsiPortCompleteRequest(h, (UCHAR) path, SP_UNTAGGED, SP_UNTAGGED, SRB_STATUS_BUS_RESET);
	ScsiPortNotification(NextRequest, h, NULL);
	return TRUE;
}

static BOOLEAN NTAPI hw_initialize(PVOID ext)
{
	HBA *h = (HBA *) ext;
	int i;
	/* from here on Windows takes the controller's interrupt: commands complete there */
	for (i = 0; i < h->ntargets; i++)
		if (h->d[i].present)
		{
			pw(h, &h->d[i], P_IS, 0xFFFFFFFFu);
			pw(h, &h->d[i], P_IE, PIE_ALL);
		}
	w32(h, HBA_IS, 0xFFFFFFFFu);
	if (!h->no_irq)
	{
		w32(h, HBA_GHC, r32(h, HBA_GHC) | GHC_IE);
		h->irq_mode = 1;
	}
	logf("kexahci: %s\n", h->irq_mode ? "interrupts on" : "polled");
	return TRUE;
}

/* the AHCI controller this adapter is: the one whose BAR 5 Windows gave us,
 * else the next one on PCI (several controllers: one adapter each, in order) */
static int found_before;

static ULONG find_controller(HBA *h, ULONG given)
{
	ULONG bus, slot;
	int n = 0;
	for (bus = 0; bus < 8; bus++)
		for (slot = 0; slot < 256; slot++)
		{
			UCHAR cfg[0x40];
			ULONG bar5, pcmd;
			PCI_SLOT_NUMBER sn;
			sn.u.AsULONG = 0;
			sn.u.bits.DeviceNumber = slot >> 3;
			sn.u.bits.FunctionNumber = slot & 7;
			if (ScsiPortGetBusData(h, PCIConfiguration, bus, sn.u.AsULONG, cfg, sizeof(cfg)) < sizeof(cfg) ||
			    *(PUSHORT) cfg == 0xffff || *(PUSHORT) cfg == 0)
				continue;
			if (cfg[0x0b] != 0x01 || cfg[0x0a] != 0x06 || cfg[0x09] != 0x01)
				continue;
			bar5 = *(PULONG) (cfg + 0x24) & ~0xFu;
			if (!bar5)
				continue;
			if (given ? bar5 != given : n++ != found_before)
				continue;
			pcmd = *(PUSHORT) (cfg + 4);
			if ((pcmd & 6) != 6)
			{
				pcmd |= 6;   /* memory space, bus master */
				ScsiPortSetBusDataByOffset(h, PCIConfiguration, bus, sn.u.AsULONG, &pcmd, 4, 2);
			}
			logf("kexahci: AHCI %x:%x at %u:%u.%u, ABAR %x\n", *(PUSHORT) cfg, *(PUSHORT) (cfg + 2), bus, slot >> 3,
			     slot & 7, bar5);
			found_before++;
			return bar5;
		}
	return 0;
}

static ULONG NTAPI hw_find(PVOID ext, PVOID ctx, PVOID bus, PCHAR args, PPORT_CONFIGURATION_INFORMATION ci,
                           PBOOLEAN again)
{
	HBA *h = (HBA *) ext;
	ULONG i, abar_pa = 0, len = 0, base_pa;
	PUCHAR mem;
	(void) ctx;
	(void) bus;
	*again = FALSE;
	memset(h, 0, sizeof(*h));
	{
		/* AdapterSettings "POLL": no interrupts */
		PCHAR a = args;
		for (; a && *a; a++)
		{
			if ((a[0] | 0x20) == 'p' && (a[1] | 0x20) == 'o' && (a[2] | 0x20) == 'l' && (a[3] | 0x20) == 'l')
				h->no_irq = 1;
			if ((a[0] | 0x20) == 'n' && (a[1] | 0x20) == 'o' && (a[2] | 0x20) == 'n' && (a[3] | 0x20) == 'c' &&
			    (a[4] | 0x20) == 'q')
				h->no_ncq = 1;
		}
	}
	for (i = 0; i < ci->NumberOfAccessRanges; i++)
	{
		ACCESS_RANGE *r = &(*ci->AccessRanges)[i];
		ULONG s = ScsiPortConvertPhysicalAddressToUlong(r->RangeStart);
		logf("kexahci: range %u: %x+%x %s\n", i, s, r->RangeLength, r->RangeInMemory ? "memory" : "ports");
		if (!r->RangeInMemory || r->RangeLength < 0x100 || !s)
			continue;
		logf("kexahci: memory range %u: %x+%x\n", i, s, r->RangeLength);
		abar_pa = s;
	}
	abar_pa = find_controller(h, abar_pa);
	if (!abar_pa)
	{
		logf("kexahci: no AHCI controller\n");
		return SP_RETURN_NOT_FOUND;
	}
	h->abar = (PUCHAR) ScsiPortGetDeviceBase(h, ci->AdapterInterfaceType, ci->SystemIoBusNumber,
	                                         ScsiPortConvertUlongToPhysicalAddress(abar_pa), 0x1100, FALSE);
	/* Windows 98 leaves an AHCI controller's BAR 5 out of the device node's
	 * resources (only the IDE-style BAR 4 ports come through), and SCSIPORT
	 * then hands the physical address back unmapped: map it with the kernel */
	if ((ULONG) h->abar == abar_pa || (ULONG) h->abar < 0x80000000u)
	{
		PHYSICAL_ADDRESS pa;
		pa.QuadPart = abar_pa;
		h->abar = (PUCHAR) MmMapIoSpace(pa, 0x1100, MmNonCached);
		logf("kexahci: ABAR mapped by MmMapIoSpace\n");
	}
	logf("kexahci: irq level %u vector %u mode %u\n", ci->BusInterruptLevel, ci->BusInterruptVector,
	     ci->InterruptMode);
	logf("kexahci: %u ranges, interface %u, ABAR mapped at %x\n", ci->NumberOfAccessRanges, ci->AdapterInterfaceType,
	     (ULONG) h->abar);
	if (!h->abar)
		return SP_RETURN_ERROR;
	/* from the BIOS (BIOS/OS handoff), into AHCI mode, interrupts off */
	if (r32(h, HBA_CAP2) & CAP2_BOH)
	{
		w32(h, HBA_BOHC, r32(h, HBA_BOHC) | BOHC_OOS);
		for (i = 0; i < 250 && (r32(h, HBA_BOHC) & (BOHC_BOS | BOHC_BB)); i++)
			ScsiPortStallExecution(10000);
	}
	w32(h, HBA_GHC, r32(h, HBA_GHC) | GHC_AE);
	w32(h, HBA_GHC, (r32(h, HBA_GHC) | GHC_AE) & ~GHC_IE);
	h->cap = r32(h, HBA_CAP);
	h->pi = r32(h, HBA_PI);
	h->nports = (h->cap & 0x1F) + 1;
	logf("kexahci: version %x, CAP %x, ports implemented %x\n", r32(h, HBA_VS), h->cap, h->pi);
	/* the ports' memory: physically contiguous, uncached */
	mem = (PUCHAR) ScsiPortGetUncachedExtension(h, ci, MAX_PORTS * PORT_MEM + 0x400 + IDBUF_SIZE);
	if (!mem)
		return SP_RETURN_ERROR;
	base_pa = ScsiPortConvertPhysicalAddressToUlong(ScsiPortGetPhysicalAddress(h, NULL, mem, &len));
	/* 1 KB alignment for the first command list */
	{
		ULONG skip = (0x400 - (base_pa & 0x3FF)) & 0x3FF;
		mem += skip;
		base_pa += skip;
	}
	h->idbuf = mem + MAX_PORTS * PORT_MEM;
	h->idbuf_pa = base_pa + MAX_PORTS * PORT_MEM;
	for (i = 0; i < 32 && h->ntargets < MAX_PORTS; i++)
	{
		DRIVE *d = &h->d[h->ntargets];
		ULONG ssts, sig, o = h->ntargets * PORT_MEM;
		if (!(h->pi & (1u << i)))
			continue;
		d->port = (int) i;
		d->tid = h->ntargets;
		d->cl = (PULONG) (mem + o);
		d->cl_pa = base_pa + o;
		d->fis = mem + o + CL_SIZE;
		d->fis_pa = base_pa + o + CL_SIZE;
		d->ct = mem + o + CL_SIZE + FIS_SIZE;
		d->ct_pa = base_pa + o + CL_SIZE + FIS_SIZE;
		memset(mem + o, 0, PORT_MEM);
		port_stop(h, d);
		pw(h, d, P_CLB, d->cl_pa);
		pw(h, d, P_CLBU, 0);
		pw(h, d, P_FB, d->fis_pa);
		pw(h, d, P_FBU, 0);
		pw(h, d, P_IE, 0);
		if (h->cap & CAP_SSS)
			pw(h, d, P_CMD, pr(h, d, P_CMD) | PCMD_SUD);
		pw(h, d, P_CMD, (pr(h, d, P_CMD) | PCMD_POD | PCMD_ICC_ACTIVE));
		/* a device: the link up (DET = 3) within a second */
		{
			ULONG t;
			for (t = 0; t < 100 && (pr(h, d, P_SSTS) & 0xF) != 3; t++)
			{
				if (t >= 5 && !(pr(h, d, P_SSTS) & 0xF))
					break;   /* nothing attached: no need to wait the second out */
				ScsiPortStallExecution(10000);
			}
		}
		ssts = pr(h, d, P_SSTS);
		h->ntargets++;   /* the target id stays the port's place, device or not */
		if ((ssts & 0xF) != 3)
		{
			logf("kexahci: port %u: no device (SSTS %x)\n", i, ssts);
			continue;
		}
		port_start(h, d);
		sig = pr(h, d, P_SIG);
		d->atapi = sig == SIG_ATAPI;
		logf("kexahci: port %u: signature %x\n", i, sig);
		identify(h, d);
	}
	ci->MaximumTransferLength = MAX_XFER;
	ci->NumberOfPhysicalBreaks = NPRD - 1;
	ci->ScatterGather = TRUE;
	ci->Master = TRUE;
	ci->Dma32BitAddresses = TRUE;
	ci->AlignmentMask = 1;
	ci->NumberOfBuses = 1;
	ci->InitiatorBusId[0] = 7;
	ci->MaximumNumberOfTargets = (UCHAR) (h->ntargets > 7 ? 7 : h->ntargets);
	ci->CachesData = FALSE;
	ci->InterruptMode = LevelSensitive;
	if (!ci->BusInterruptLevel)
		h->no_irq = 1;
	if (h->ntargets > 7)
		h->ntargets = 7;   /* the initiator is id 7 */
	return SP_RETURN_FOUND;
}

ULONG NTAPI DriverEntry(PVOID drv, PVOID arg2)
{
	HW_INITIALIZATION_DATA hi;
	ULONG r;
	memset(&hi, 0, sizeof(hi));
	/* the Windows NT 4 layout, which is what Windows 98's SCSIPORT knows */
	hi.HwInitializationDataSize = FIELD_OFFSET(HW_INITIALIZATION_DATA, HwAdapterControl);
	hi.AdapterInterfaceType = Isa;   /* as kexata: Windows 98 hands the resources over from the device node */
	hi.HwInitialize = hw_initialize;
	hi.HwStartIo = hw_startio;
	hi.HwInterrupt = hw_interrupt;
	hi.HwFindAdapter = hw_find;
	hi.HwResetBus = hw_reset;
	hi.DeviceExtensionSize = sizeof(HBA);
	hi.NumberOfAccessRanges = 6;
	hi.MapBuffers = TRUE;
	hi.NeedPhysicalAddresses = TRUE;
	/* Windows 98 sends overlapping writes to a disk once asked (NextLuRequest) */
	hi.TaggedQueuing = TRUE;
	hi.MultipleRequestPerLu = TRUE;
	logf("kexahci: KernelExEx AHCI driver\n");
	r = ScsiPortInitialize(drv, arg2, &hi, NULL);
	logf("kexahci: ScsiPortInitialize %x\n", r);
	return r;
}
