/*
 *  KernelExEx - kexata.mpd: IDE/ATA and ATAPI for Windows 98 SE / ME, with
 *  48-bit LBA (disks over 128 GB).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  An NT SCSI miniport, loaded by Windows 98's SCSIPORT.PDR (the IOS SCSI
 *  layer), in place of ESDI_506.PDR for one IDE channel at a time. Windows'
 *  own disk and CD-ROM drivers (disktsd, cdvsd) sit on top: they send SCSI
 *  commands, which this driver turns into ATA commands for disks and passes
 *  through as ATAPI packets for CD/DVD drives.
 *
 *  Windows 98's ESDI_506.PDR uses 28-bit LBA only: past 128 GB (2^28
 *  sectors) it wraps around and overwrites the start of the disk. Here every
 *  disk command above that, or longer than 256 sectors, is a 48-bit command
 *  (READ/WRITE DMA EXT), and SCSI's 32-bit block numbers reach 2 TB.
 *
 *  One adapter per channel: it binds to the channel device nodes Windows
 *  makes for a PCI IDE controller (MF\GOODPRIMARY, MF\GOODSECONDARY, from
 *  mshdc.inf or KernelExEx's kexide.inf) and gets their ports and IRQ.
 *  Bus-master DMA (the controller's PCI bus master registers, in the same
 *  resources) when the BIOS set the drive up for a DMA mode; PIO otherwise
 *  and for ATAPI.
 *
 *  Reference: ATA/ATAPI-6 and later (T13), SFF-8038i (bus master IDE),
 *  SCSI SPC/SBC for the translation. Imports only what SCSIPORT.PDR has
 *  (net/tools/exports/win98se-wdm/scsiport.sys.txt).
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

/* ---- ATA ----------------------------------------------------------------------------- */
#define R_DATA 0
#define R_ERROR 1
#define R_FEATURE 1
#define R_COUNT 2
#define R_LBA0 3
#define R_LBA1 4
#define R_LBA2 5
#define R_DEVICE 6
#define R_STATUS 7
#define R_COMMAND 7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF 0x20
#define ST_DRDY 0x40
#define ST_BSY 0x80

#define ER_ABRT 0x04
#define ER_IDNF 0x10
#define ER_UNC 0x40
#define ER_ICRC 0x80

#define DC_NIEN 0x02
#define DC_SRST 0x04

#define BM_CMD 0
#define BM_STATUS 2
#define BM_PRD 4
#define BMC_START 0x01
#define BMC_READ 0x08   /* device to memory */
#define BMS_ACTIVE 0x01
#define BMS_ERROR 0x02
#define BMS_IRQ 0x04

#define CMD_READ_PIO 0x20
#define CMD_READ_PIO_EXT 0x24
#define CMD_READ_DMA_EXT 0x25
#define CMD_WRITE_PIO 0x30
#define CMD_WRITE_PIO_EXT 0x34
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_PACKET 0xa0
#define CMD_IDENTIFY_PACKET 0xa1
#define CMD_READ_DMA 0xc8
#define CMD_WRITE_DMA 0xca
#define CMD_FLUSH 0xe7
#define CMD_FLUSH_EXT 0xea
#define CMD_IDENTIFY 0xec

#define MAX_XFER 0x20000   /* 128 KB: 256 sectors */
#define NPRD 64

typedef struct
{
	ULONG addr;
	ULONG len;   /* bit 31: last entry; 0 in bits 0-15 means 64 KB */
} PRD;

enum
{
	PH_IDLE,
	PH_PIO_IN,
	PH_PIO_OUT,
	PH_DMA,
	PH_ATAPI,
	PH_NODATA
};

typedef struct
{
	int present, atapi, lba, lba48, dma, removable;
	ULONGLONG sectors;
	USHORT cyls, heads, spt;
	char model[41], fw[9];
	UCHAR sense_key, asc, ascq;
	UCHAR cdb_len;   /* ATAPI: 12 or 16 */
} DRIVE;

typedef struct
{
	PUCHAR cmd, ctl, bm;
	ULONG irq;
	DRIVE d[2];
	PRD *prd;
	ULONG prd_pa;
	/* the request in progress */
	PSCSI_REQUEST_BLOCK srb;
	int phase;
	PUCHAR buf;
	ULONG left;          /* bytes still to move */
	ULONG done_bytes;
	ULONG chunk;         /* PIO: bytes per DRQ block */
	UCHAR cmd_sent;
	int write;
	int channel;         /* 0 primary, 1 secondary (for the log) */
	int force_dma, no_dma;   /* AdapterSettings "DMA=FORCE" / "DMA=OFF" */
	int depth, want_next;    /* see enter() */
} CHAN;

/* ---- register access ----------------------------------------------------------------- */
static UCHAR rd(CHAN *c, int r)
{
	return ScsiPortReadPortUchar(c->cmd + r);
}

static void wr(CHAN *c, int r, UCHAR v)
{
	ScsiPortWritePortUchar(c->cmd + r, v);
}

static UCHAR alt(CHAN *c)
{
	return ScsiPortReadPortUchar(c->ctl);
}

static void devctl(CHAN *c, UCHAR v)
{
	ScsiPortWritePortUchar(c->ctl, v);
}

/* wait until not busy; returns the status, or 0xff on timeout */
static UCHAR wait_bsy(CHAN *c, ULONG us)
{
	UCHAR s;
	for (;;)
	{
		s = alt(c);
		if (s == 0xff)
			return s;
		if (!(s & ST_BSY))
			return s;
		if (us < 10)
			return 0xff;
		ScsiPortStallExecution(10);
		us -= 10;
	}
}

static UCHAR wait_drq(CHAN *c, ULONG us)
{
	UCHAR s;
	for (;;)
	{
		s = alt(c);
		if (!(s & ST_BSY) && (s & (ST_DRQ | ST_ERR)))
			return s;
		if (us < 10)
			return s;   /* timeout: no DRQ */
		ScsiPortStallExecution(10);
		us -= 10;
	}
}

static void select_drive(CHAN *c, int dev, UCHAR extra)
{
	wr(c, R_DEVICE, (UCHAR) (0xa0 | (dev << 4) | extra));
	alt(c);
	alt(c);
	alt(c);
	alt(c);   /* 400 ns */
}

static void soft_reset(CHAN *c)
{
	devctl(c, DC_NIEN | DC_SRST);
	ScsiPortStallExecution(10);
	devctl(c, DC_NIEN);
	ScsiPortStallExecution(2000);
	wait_bsy(c, 3000000);
}

static void ata_string(char *out, const USHORT *w, int words)
{
	int i, n = 0;
	for (i = 0; i < words; i++)
	{
		out[n++] = (char) (w[i] >> 8);
		out[n++] = (char) w[i];
	}
	out[n] = 0;
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0))
		out[--n] = 0;
}

/* IDENTIFY (PACKET) DEVICE by polling, before interrupts are on */
static int identify(CHAN *c, int dev, int atapi, USHORT *id)
{
	UCHAR s;
	int i;
	select_drive(c, dev, 0);
	if (wait_bsy(c, 100000) == 0xff)
		return 0;
	wr(c, R_COMMAND, atapi ? CMD_IDENTIFY_PACKET : CMD_IDENTIFY);
	ScsiPortStallExecution(1);
	s = wait_drq(c, 1000000);
	if ((s & ST_ERR) || !(s & ST_DRQ))
		return 0;
	ScsiPortReadPortBufferUshort((PUSHORT) (c->cmd + R_DATA), id, 256);
	for (i = 0; i < 4 && (alt(c) & ST_DRQ); i++)
		ScsiPortStallExecution(1);
	return 1;
}

/* SET FEATURES, transfer mode */
static int set_xfer_mode(CHAN *c, int dev, UCHAR mode)
{
	UCHAR s;
	select_drive(c, dev, 0);
	wait_bsy(c, 100000);
	wr(c, R_FEATURE, 0x03);
	wr(c, R_COUNT, mode);
	wr(c, R_COMMAND, 0xef);
	ScsiPortStallExecution(1);
	s = wait_bsy(c, 1000000);
	return !(s & (ST_ERR | ST_BSY));
}

static void probe(CHAN *c)
{
	USHORT id[256];
	int dev;
	devctl(c, DC_NIEN);
	select_drive(c, 0, 0);
	if (alt(c) == 0xff)
	{
		select_drive(c, 1, 0);
		if (alt(c) == 0xff)
		{
			logf("kexata: channel %u: empty (floating bus)\n", c->channel);
			return;
		}
	}
	soft_reset(c);
	for (dev = 0; dev < 2; dev++)
	{
		DRIVE *d = &c->d[dev];
		UCHAR m, h, s;
		memset(d, 0, sizeof(*d));
		select_drive(c, dev, 0);
		s = alt(c);
		m = rd(c, R_LBA1);
		h = rd(c, R_LBA2);
		if ((m == 0x14 && h == 0xeb) || (m == 0x69 && h == 0x96))
			d->atapi = 1;
		else if (s == 0 || s == 0xff || s == 0x7f)
			continue;
		if (!identify(c, dev, d->atapi, id))
		{
			if (!d->atapi && identify(c, dev, 1, id))
				d->atapi = 1;   /* the signature was lost */
			else
				continue;
		}
		d->present = 1;
		ata_string(d->model, id + 27, 20);
		ata_string(d->fw, id + 23, 4);
		d->removable = (id[0] & 0x80) != 0;
		if (d->atapi)
		{
			d->cdb_len = (id[0] & 3) == 1 ? 16 : 12;
			logf("kexata: channel %u %s: ATAPI \"%s\" %s\n", c->channel, dev ? "slave" : "master", d->model,
			     d->fw);
			continue;
		}
		d->lba = (id[49] & 0x200) != 0;
		d->lba48 = (id[83] & 0x400) && (id[86] & 0x400);
		if (d->lba48)
			d->sectors = (ULONGLONG) id[100] | (ULONGLONG) id[101] << 16 | (ULONGLONG) id[102] << 32 |
			             (ULONGLONG) id[103] << 48;
		if (!d->sectors && d->lba)
			d->sectors = (ULONG) id[60] | (ULONG) id[61] << 16;
		d->cyls = id[1];
		d->heads = id[3];
		d->spt = id[6];
		if (!d->sectors)
			d->sectors = (ULONG) d->cyls * d->heads * d->spt;
		/* DMA when the BIOS selected a DMA mode on the drive and the controller
		 * has bus master registers: the BIOS then also set the chipset's timings */
		if (c->bm && (id[49] & 0x100) && !c->no_dma)
		{
			int udma = (id[53] & 4) && (id[88] >> 8);
			int mwdma = (id[63] >> 8) & 7;
			if (!udma && !mwdma && c->force_dma && set_xfer_mode(c, dev, 0x22))
				mwdma = 4;   /* multiword DMA 2, chosen by us (only with DMA=FORCE) */
			if (udma || mwdma)
			{
				UCHAR bs = ScsiPortReadPortUchar(c->bm + BM_STATUS);
				d->dma = 1;
				ScsiPortWritePortUchar(c->bm + BM_STATUS, (UCHAR) (bs | (dev ? 0x40 : 0x20)));
			}
		}
		logf("kexata: channel %u %s: \"%s\" %s, %u MB, %s%s\n", c->channel, dev ? "slave" : "master", d->model,
		     d->fw, (ULONG) (d->sectors >> 11), d->lba48 ? "48-bit LBA" : d->lba ? "LBA" : "CHS",
		     d->dma ? ", DMA" : ", PIO");
	}
}

/* ---- SCSI side ----------------------------------------------------------------------- */
static void set_sense(DRIVE *d, UCHAR key, UCHAR asc, UCHAR ascq)
{
	d->sense_key = key;
	d->asc = asc;
	d->ascq = ascq;
}

static void finish(CHAN *c, PSCSI_REQUEST_BLOCK srb, UCHAR status)
{
	srb->SrbStatus = status;
	if (status == SRB_STATUS_ERROR)
		srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
	else
		srb->ScsiStatus = SCSISTAT_GOOD;
	c->srb = NULL;
	c->phase = PH_IDLE;
	ScsiPortNotification(RequestComplete, c, srb);
	c->want_next = 1;
}

/* Windows 98's SCSIPORT starts the next request (StartIo) right inside
 * NextRequest, and lets our interrupt nest: a PIO write's first sector
 * interrupts at once. Calling NextRequest from inside StartIo or the ISR
 * then nests one level deeper per request until the stack overflows. So
 * StartIo and the ISR only note that a request finished, and the outermost
 * one asks for the next request in a loop. */
static void enter(CHAN *c)
{
	c->depth++;
}

static void leave(CHAN *c)
{
	if (c->depth == 1)
		while (c->want_next)
		{
			c->want_next = 0;
			ScsiPortNotification(NextRequest, c, NULL);
		}
	c->depth--;
}

static void fail_sense(CHAN *c, PSCSI_REQUEST_BLOCK srb, UCHAR key, UCHAR asc)
{
	set_sense(&c->d[srb->TargetId], key, asc, 0);
	srb->DataTransferLength = 0;
	finish(c, srb, SRB_STATUS_ERROR);
}

static void ata_error(CHAN *c, PSCSI_REQUEST_BLOCK srb, UCHAR st)
{
	UCHAR er = rd(c, R_ERROR);
	DRIVE *d = &c->d[srb->TargetId];
	logf("kexata: channel %u: command %x failed, status %x error %x\n", c->channel, srb->Cdb[0], st, er);
	if (er & ER_UNC)
		set_sense(d, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0);
	else if (er & ER_IDNF)
		set_sense(d, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
	else if (er & ER_ICRC)
		set_sense(d, SCSI_SENSE_ABORTED_COMMAND, 0x47, 0);
	else if (er & ER_ABRT)
		set_sense(d, SCSI_SENSE_ABORTED_COMMAND, 0, 0);
	else
		set_sense(d, SCSI_SENSE_HARDWARE_ERROR, 0x44, 0);
	srb->DataTransferLength = c->done_bytes;
	finish(c, srb, SRB_STATUS_ERROR);
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

/* bus master scatter/gather list: 1 when the buffer can go by DMA */
static int build_prd(CHAN *c, PSCSI_REQUEST_BLOCK srb, PUCHAR va, ULONG len)
{
	int n = 0;
	if (((ULONG_PTR) va & 1) || (len & 1))
		return 0;
	while (len)
	{
		ULONG run = 0, pa;
		SCSI_PHYSICAL_ADDRESS p = ScsiPortGetPhysicalAddress(c, srb, va, &run);
		pa = ScsiPortConvertPhysicalAddressToUlong(p);
		if (run > len)
			run = len;
		while (run)
		{
			/* entries of at most 64 KB that do not cross a 64 KB boundary */
			ULONG piece = 0x10000 - (pa & 0xffff);
			if (piece > run)
				piece = run;
			if (n == NPRD)
				return 0;
			c->prd[n].addr = pa;
			c->prd[n].len = piece & 0xffff;
			n++;
			pa += piece;
			va += piece;
			run -= piece;
			len -= piece;
		}
	}
	if (!n)
		return 0;
	c->prd[n - 1].len |= 0x80000000u;
	return 1;
}

static void issue_rw(CHAN *c, PSCSI_REQUEST_BLOCK srb, ULONGLONG lba, ULONG count, int write)
{
	DRIVE *d = &c->d[srb->TargetId];
	int dev = srb->TargetId, ext;
	UCHAR cmd;
	if (lba + count > d->sectors)
	{
		fail_sense(c, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x21);
		return;
	}
	if (!count)
	{
		srb->DataTransferLength = 0;
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	}
	if (count > MAX_XFER / 512 || count * 512 > srb->DataTransferLength)
	{
		fail_sense(c, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	ext = d->lba48 && (lba + count > 0x0fffffff || count > 256);
	if (!d->lba48 && (lba + count > 0x0fffffff || count > 256))
	{
		fail_sense(c, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x21);
		return;
	}
	c->buf = (PUCHAR) srb->DataBuffer;
	c->left = count * 512;
	c->done_bytes = 0;
	c->chunk = 512;
	c->write = write;
	c->phase = PH_PIO_IN;
	if (d->dma && build_prd(c, srb, c->buf, c->left))
	{
		c->phase = PH_DMA;
		cmd = ext ? (write ? CMD_WRITE_DMA_EXT : CMD_READ_DMA_EXT) : (write ? CMD_WRITE_DMA : CMD_READ_DMA);
	}
	else
	{
		c->phase = write ? PH_PIO_OUT : PH_PIO_IN;
		cmd = ext ? (write ? CMD_WRITE_PIO_EXT : CMD_READ_PIO_EXT) : (write ? CMD_WRITE_PIO : CMD_READ_PIO);
	}
	if (wait_bsy(c, 30000) & ST_BSY)
	{
		fail_sense(c, srb, SCSI_SENSE_NOT_READY, 0x04);
		return;
	}
	if (ext)
	{
		select_drive(c, dev, 0x40);
		wr(c, R_COUNT, (UCHAR) (count >> 8));
		wr(c, R_LBA0, (UCHAR) (lba >> 24));
		wr(c, R_LBA1, (UCHAR) (lba >> 32));
		wr(c, R_LBA2, (UCHAR) (lba >> 40));
		wr(c, R_COUNT, (UCHAR) count);
		wr(c, R_LBA0, (UCHAR) lba);
		wr(c, R_LBA1, (UCHAR) (lba >> 8));
		wr(c, R_LBA2, (UCHAR) (lba >> 16));
	}
	else if (d->lba)
	{
		select_drive(c, dev, (UCHAR) (0x40 | ((lba >> 24) & 0x0f)));
		wr(c, R_COUNT, (UCHAR) count);
		wr(c, R_LBA0, (UCHAR) lba);
		wr(c, R_LBA1, (UCHAR) (lba >> 8));
		wr(c, R_LBA2, (UCHAR) (lba >> 16));
	}
	else
	{
		/* old CHS-only drives */
		ULONG l = (ULONG) lba, sec = l % d->spt + 1, t = l / d->spt, head = t % d->heads, cyl = t / d->heads;
		select_drive(c, dev, (UCHAR) head);
		wr(c, R_COUNT, (UCHAR) count);
		wr(c, R_LBA0, (UCHAR) sec);
		wr(c, R_LBA1, (UCHAR) cyl);
		wr(c, R_LBA2, (UCHAR) (cyl >> 8));
	}
	c->srb = srb;
	if (c->phase == PH_DMA)
	{
		ScsiPortWritePortUlong((PULONG) (c->bm + BM_PRD), c->prd_pa);
		ScsiPortWritePortUchar(c->bm + BM_CMD, write ? 0 : BMC_READ);
		ScsiPortWritePortUchar(c->bm + BM_STATUS,
		                       (UCHAR) (ScsiPortReadPortUchar(c->bm + BM_STATUS) | BMS_ERROR | BMS_IRQ));
		wr(c, R_COMMAND, cmd);
		ScsiPortWritePortUchar(c->bm + BM_CMD, (UCHAR) ((write ? 0 : BMC_READ) | BMC_START));
		return;
	}
	/* PIO, polled, with the drive's interrupt off: a fallback (DMA is the
	 * normal case), and Windows 98's SCSIPORT lets a PIO interrupt nest inside
	 * StartIo, which interrupt-driven PIO does not survive */
	devctl(c, DC_NIEN);
	wr(c, R_COMMAND, cmd);
	ScsiPortStallExecution(1);
	while (c->left)
	{
		UCHAR st = wait_drq(c, 5000000);
		if ((st & (ST_ERR | ST_DF)) || !(st & ST_DRQ))
		{
			devctl(c, 0);
			ata_error(c, srb, st);
			return;
		}
		if (write)
			ScsiPortWritePortBufferUshort((PUSHORT) (c->cmd + R_DATA), (PUSHORT) c->buf, 256);
		else
			ScsiPortReadPortBufferUshort((PUSHORT) (c->cmd + R_DATA), (PUSHORT) c->buf, 256);
		c->buf += 512;
		c->left -= 512;
		c->done_bytes += 512;
		alt(c);   /* 400 ns before BSY is valid */
	}
	{
		UCHAR st = wait_bsy(c, 5000000);
		rd(c, R_STATUS);   /* clear INTRQ */
		devctl(c, 0);
		if (st & (ST_ERR | ST_DF | ST_BSY))
		{
			ata_error(c, srb, st);
			return;
		}
	}
	srb->DataTransferLength = c->done_bytes;
	finish(c, srb, SRB_STATUS_SUCCESS);
}

static void issue_flush(CHAN *c, PSCSI_REQUEST_BLOCK srb)
{
	DRIVE *d = &c->d[srb->TargetId];
	select_drive(c, srb->TargetId, 0x40);
	wait_bsy(c, 30000);
	c->srb = srb;
	c->phase = PH_NODATA;
	c->done_bytes = 0;
	srb->DataTransferLength = 0;
	wr(c, R_COMMAND, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
}

static void ata_scsi(CHAN *c, PSCSI_REQUEST_BLOCK srb)
{
	DRIVE *d = &c->d[srb->TargetId];
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
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_REQUEST_SENSE:
		b[0] = 0x70;
		b[2] = d->sense_key;
		b[7] = 10;
		b[12] = d->asc;
		b[13] = d->ascq;
		set_sense(d, 0, 0, 0);
		reply(srb, b, 18);
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_INQUIRY:
	{
		int i;
		if (cdb[1] & 1)
		{
			fail_sense(c, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
			return;
		}
		b[0] = 0;   /* direct access */
		b[1] = d->removable ? 0x80 : 0;
		b[2] = 2;
		b[3] = 2;
		b[4] = 31;
		memcpy(b + 8, "ATA     ", 8);
		for (i = 0; i < 16; i++)
			b[16 + i] = (UCHAR) (d->model[i] ? d->model[i] : ' ');
		for (i = 0; i < 4; i++)
			b[32 + i] = (UCHAR) (d->fw[i] ? d->fw[i] : ' ');
		for (i = 16; i < 32 && d->model[i - 16]; i++)
			;
		reply(srb, b, 36);
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	}
	case SCSIOP_READ_CAPACITY:
	{
		ULONGLONG last = d->sectors - 1;
		put_be32(b, last > 0xffffffffu ? 0xffffffffu : (ULONG) last);
		put_be32(b + 4, 512);
		reply(srb, b, 8);
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	}
	case SCSIOP_MODE_SENSE:
		b[0] = 3;   /* no block descriptors, no pages */
		reply(srb, b, 4);
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_MODE_SENSE10:
		b[1] = 6;
		reply(srb, b, 8);
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_READ6:
	case SCSIOP_WRITE6:
	{
		ULONG lba = ((ULONG) (cdb[1] & 0x1f) << 16) | ((ULONG) cdb[2] << 8) | cdb[3];
		ULONG n = cdb[4] ? cdb[4] : 256;
		issue_rw(c, srb, lba, n, cdb[0] == SCSIOP_WRITE6);
		return;
	}
	case SCSIOP_READ:
	case SCSIOP_WRITE:
	{
		ULONG lba = ((ULONG) cdb[2] << 24) | ((ULONG) cdb[3] << 16) | ((ULONG) cdb[4] << 8) | cdb[5];
		ULONG n = ((ULONG) cdb[7] << 8) | cdb[8];
		issue_rw(c, srb, lba, n, cdb[0] == SCSIOP_WRITE);
		return;
	}
	case SCSIOP_READ12:
	case SCSIOP_WRITE12:
	{
		ULONG lba = ((ULONG) cdb[2] << 24) | ((ULONG) cdb[3] << 16) | ((ULONG) cdb[4] << 8) | cdb[5];
		ULONG n = ((ULONG) cdb[6] << 24) | ((ULONG) cdb[7] << 16) | ((ULONG) cdb[8] << 8) | cdb[9];
		issue_rw(c, srb, lba, n, cdb[0] == SCSIOP_WRITE12);
		return;
	}
	case SCSIOP_SYNCHRONIZE_CACHE:
		issue_flush(c, srb);
		return;
	default:
		fail_sense(c, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x20);
		return;
	}
}

/* ---- ATAPI: packets, PIO ----------------------------------------------------------- */
static void atapi_scsi(CHAN *c, PSCSI_REQUEST_BLOCK srb)
{
	DRIVE *d = &c->d[srb->TargetId];
	UCHAR cdb[16], s;
	ULONG bc;
	memset(cdb, 0, sizeof(cdb));
	memcpy(cdb, srb->Cdb, srb->CdbLength > 16 ? 16 : srb->CdbLength);
	/* ATAPI devices only know the 10-byte MODE commands */
	if (cdb[0] == SCSIOP_MODE_SENSE || cdb[0] == SCSIOP_MODE_SELECT)
	{
		/* not translated: report "invalid opcode", Windows then uses the 10-byte form */
		fail_sense(c, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x20);
		return;
	}
	c->buf = (PUCHAR) srb->DataBuffer;
	c->left = srb->DataTransferLength;
	c->done_bytes = 0;
	c->write = (srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0;
	c->phase = PH_ATAPI;
	select_drive(c, srb->TargetId, 0);
	if (wait_bsy(c, 30000) & ST_BSY)
	{
		fail_sense(c, srb, SCSI_SENSE_NOT_READY, 0x04);
		return;
	}
	bc = c->left > 0xfffe ? 0xfffe : (c->left ? c->left : 2);
	bc &= ~1u;
	wr(c, R_FEATURE, 0);
	wr(c, R_LBA1, (UCHAR) bc);
	wr(c, R_LBA2, (UCHAR) (bc >> 8));
	c->srb = srb;
	wr(c, R_COMMAND, CMD_PACKET);
	ScsiPortStallExecution(1);
	s = wait_drq(c, 50000);
	if (s & ST_ERR)
	{
		UCHAR er = rd(c, R_ERROR);
		set_sense(d, (UCHAR) (er >> 4), 0, 0);
		srb->DataTransferLength = 0;
		finish(c, srb, SRB_STATUS_ERROR);
		return;
	}
	if (!(s & ST_DRQ))
	{
		logf("kexata: channel %u: ATAPI device did not take the packet (status %x)\n", c->channel, s);
		srb->DataTransferLength = 0;
		finish(c, srb, SRB_STATUS_TIMEOUT);
		return;
	}
	ScsiPortWritePortBufferUshort((PUSHORT) (c->cmd + R_DATA), (PUSHORT) cdb, d->cdb_len / 2);
}

static void atapi_intr(CHAN *c, UCHAR st)
{
	PSCSI_REQUEST_BLOCK srb = c->srb;
	UCHAR reason = rd(c, R_COUNT) & 3;   /* bit 0 CoD, bit 1 IO */
	if (st & ST_DRQ)
	{
		ULONG bc = rd(c, R_LBA1) | ((ULONG) rd(c, R_LBA2) << 8), n, words;
		if (reason & 1)
			return;   /* a command phase again: ignore */
		n = bc < c->left ? bc : c->left;
		words = (n + 1) / 2;
		if (reason & 2)
		{
			if (n)
				ScsiPortReadPortBufferUshort((PUSHORT) (c->cmd + R_DATA), (PUSHORT) c->buf, n / 2);
			if (n & 1)
			{
				USHORT w = ScsiPortReadPortUshort((PUSHORT) (c->cmd + R_DATA));
				c->buf[n - 1] = (UCHAR) w;
			}
			for (; words * 2 < bc; words++)
				ScsiPortReadPortUshort((PUSHORT) (c->cmd + R_DATA));   /* more than the buffer holds */
		}
		else
		{
			if (n)
				ScsiPortWritePortBufferUshort((PUSHORT) (c->cmd + R_DATA), (PUSHORT) c->buf, words);
			for (; words * 2 < bc; words++)
				ScsiPortWritePortUshort((PUSHORT) (c->cmd + R_DATA), 0);
		}
		c->buf += n;
		c->left -= n;
		c->done_bytes += n;
		return;
	}
	/* status phase */
	srb->DataTransferLength = c->done_bytes;
	if (st & ST_ERR)
	{
		UCHAR er = rd(c, R_ERROR);
		set_sense(&c->d[srb->TargetId], (UCHAR) (er >> 4), 0, 0);
		/* Windows asks the drive itself with REQUEST SENSE */
		finish(c, srb, SRB_STATUS_ERROR);
		return;
	}
	finish(c, srb, c->left ? SRB_STATUS_DATA_OVERRUN : SRB_STATUS_SUCCESS);
}

/* ---- miniport entry points ----------------------------------------------------------- */
static BOOLEAN interrupt_body(CHAN *c);

static BOOLEAN NTAPI hw_interrupt(PVOID ext)
{
	CHAN *c = (CHAN *) ext;
	BOOLEAN r;
	enter(c);
	r = interrupt_body(c);
	leave(c);
	return r;
}

static BOOLEAN interrupt_body(CHAN *c)
{
	PSCSI_REQUEST_BLOCK srb = c->srb;
	UCHAR st, bs = 0;
	if (c->phase == PH_PIO_IN || c->phase == PH_PIO_OUT)
		return FALSE;   /* polled in StartIo */
	if (c->bm)
	{
		bs = ScsiPortReadPortUchar(c->bm + BM_STATUS);
		if (c->phase == PH_DMA && !(bs & BMS_IRQ))
			return FALSE;
	}
	if (!srb)
	{
		rd(c, R_STATUS);   /* acknowledge a stray interrupt */
		return FALSE;
	}
	if (alt(c) & ST_BSY)
		return FALSE;
	st = rd(c, R_STATUS);   /* acknowledges INTRQ */
	switch (c->phase)
	{
	case PH_DMA:
		ScsiPortWritePortUchar(c->bm + BM_CMD, (UCHAR) (c->write ? 0 : BMC_READ));
		ScsiPortWritePortUchar(c->bm + BM_STATUS, (UCHAR) (bs | BMS_ERROR | BMS_IRQ));
		if ((st & (ST_ERR | ST_DF)) || (bs & BMS_ERROR))
		{
			c->done_bytes = 0;
			ata_error(c, srb, st);
			return TRUE;
		}
		srb->DataTransferLength = c->left;
		finish(c, srb, SRB_STATUS_SUCCESS);
		return TRUE;
	case PH_NODATA:
		if (st & (ST_ERR | ST_DF))
			ata_error(c, srb, st);
		else
			finish(c, srb, SRB_STATUS_SUCCESS);
		return TRUE;
	case PH_ATAPI:
		atapi_intr(c, st);
		return TRUE;
	}
	return TRUE;
}

static void startio_body(CHAN *c, PSCSI_REQUEST_BLOCK srb);

static BOOLEAN NTAPI hw_startio(PVOID ext, PSCSI_REQUEST_BLOCK srb)
{
	CHAN *c = (CHAN *) ext;
	enter(c);
	startio_body(c, srb);
	leave(c);
	return TRUE;
}

static void startio_body(CHAN *c, PSCSI_REQUEST_BLOCK srb)
{
	DRIVE *d;
	switch (srb->Function)
	{
	case SRB_FUNCTION_EXECUTE_SCSI:
		break;
	case SRB_FUNCTION_SHUTDOWN:
	case SRB_FUNCTION_FLUSH:
		if (srb->TargetId < 2 && c->d[srb->TargetId].present && !c->d[srb->TargetId].atapi)
		{
			issue_flush(c, srb);
			return;
		}
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	case SRB_FUNCTION_RESET_BUS:
	case SRB_FUNCTION_RESET_DEVICE:
		soft_reset(c);
		devctl(c, 0);
		finish(c, srb, SRB_STATUS_SUCCESS);
		return;
	case SRB_FUNCTION_ABORT_COMMAND:
		finish(c, srb, SRB_STATUS_ABORT_FAILED);
		return;
	default:
		finish(c, srb, SRB_STATUS_INVALID_REQUEST);
		return;
	}
	if (srb->PathId != 0 || srb->TargetId > 1 || srb->Lun != 0 || !c->d[srb->TargetId].present)
	{
		finish(c, srb, SRB_STATUS_SELECTION_TIMEOUT);
		return;
	}
	d = &c->d[srb->TargetId];
	if (d->atapi)
		atapi_scsi(c, srb);
	else
		ata_scsi(c, srb);
	return;
}

static BOOLEAN NTAPI hw_reset(PVOID ext, ULONG path)
{
	CHAN *c = (CHAN *) ext;
	logf("kexata: channel %u: bus reset\n", c->channel);
	if (c->bm)
		ScsiPortWritePortUchar(c->bm + BM_CMD, 0);
	soft_reset(c);
	devctl(c, 0);
	c->srb = NULL;
	c->phase = PH_IDLE;
	ScsiPortCompleteRequest(c, (UCHAR) path, SP_UNTAGGED, SP_UNTAGGED, SRB_STATUS_BUS_RESET);
	c->want_next = 0;
	ScsiPortNotification(NextRequest, c, NULL);
	return TRUE;
}

static BOOLEAN NTAPI hw_initialize(PVOID ext)
{
	CHAN *c = (CHAN *) ext;
	devctl(c, 0);   /* interrupts on */
	return TRUE;
}

/* The bus master registers are half of the PCI IDE controller's BAR 4;
 * Windows' channel device nodes do not always carry them, so find the
 * controller that owns this channel's ports. */
static ULONG find_busmaster(CHAN *c, ULONG cmd)
{
	ULONG bus, slot;
	for (bus = 0; bus < 4; bus++)
		for (slot = 0; slot < 256; slot++)
		{
			UCHAR cfg[0x40];
			PCI_SLOT_NUMBER sn;
			ULONG bar4, chan, pcmd;
			UCHAR pi;
			sn.u.AsULONG = 0;
			sn.u.bits.DeviceNumber = slot >> 3;
			sn.u.bits.FunctionNumber = slot & 7;
			if (ScsiPortGetBusData(c, PCIConfiguration, bus, sn.u.AsULONG, cfg, sizeof(cfg)) < sizeof(cfg) ||
			    *(PUSHORT) cfg == 0xffff || *(PUSHORT) cfg == 0)
				continue;
			if (cfg[0x0b] != 0x01 || cfg[0x0a] != 0x01)
				continue;
			pi = cfg[0x09];
			bar4 = *(PULONG) (cfg + 0x20);
			if (!(bar4 & 1) || !(bar4 & ~3u))
				continue;
			if (cmd == 0x1f0 && !(pi & 1))
				chan = 0;
			else if (cmd == 0x170 && !(pi & 4))
				chan = 1;
			else if ((pi & 1) && (*(PULONG) (cfg + 0x10) & ~3u) == cmd)
				chan = 0;
			else if ((pi & 4) && (*(PULONG) (cfg + 0x18) & ~3u) == cmd)
				chan = 1;
			else
				continue;
			pcmd = *(PUSHORT) (cfg + 4);
			if (!(pcmd & 4))
			{
				/* bus master enable */
				pcmd |= 5;
				ScsiPortSetBusDataByOffset(c, PCIConfiguration, bus, sn.u.AsULONG, &pcmd, 4, 2);
			}
			logf("kexata: PCI IDE %x:%x at %u:%u.%u, prog-if %x\n", *(PUSHORT) cfg, *(PUSHORT) (cfg + 2), bus,
			     slot >> 3, slot & 7, pi);
			return (bar4 & ~3u) + chan * 8;
		}
	return 0;
}

static int arg_has(PCHAR args, const char *word)
{
	int n = 0;
	if (!args)
		return 0;
	while (word[n])
		n++;
	for (; *args; args++)
	{
		int i;
		for (i = 0; i < n; i++)
			if ((args[i] | 0x20) != (word[i] | 0x20))
				break;
		if (i == n)
			return 1;
	}
	return 0;
}

static ULONG NTAPI hw_find(PVOID ext, PVOID ctx, PVOID bus, PCHAR args, PPORT_CONFIGURATION_INFORMATION ci,
                           PBOOLEAN again)
{
	CHAN *c = (CHAN *) ext;
	ULONG i, cmd = 0, ctl = 0, bm = 0;
	(void) ctx;
	(void) bus;
	*again = FALSE;
	memset(c, 0, sizeof(*c));
	/* the channel's resources from Windows: command block (8 ports), control
	 * (1 or 2 ports, at the command block's base + 0x206 in legacy mode), bus
	 * master (8 ports, the channel's half of the controller's) */
	for (i = 0; i < ci->NumberOfAccessRanges; i++)
	{
		ACCESS_RANGE *r = &(*ci->AccessRanges)[i];
		ULONG s = ScsiPortConvertPhysicalAddressToUlong(r->RangeStart);
		if (!r->RangeLength || r->RangeLength > 0x100)
			continue;   /* unused entries */
		logf("kexata: range %u: %x+%u\n", i, s, r->RangeLength);
		if (r->RangeLength >= 8 && !cmd)
			cmd = s;
		else if (r->RangeLength <= 4 && !ctl)
			ctl = s;
		else if (r->RangeLength >= 8 && !bm)
			bm = s;
	}
	if (!cmd)
	{
		logf("kexata: no resources\n");
		return SP_RETURN_NOT_FOUND;
	}
	if (!bm)
		bm = find_busmaster(c, cmd);
	c->force_dma = arg_has(args, "dma=force");
#ifdef KEX_ATA_FORCE_DMA
	c->force_dma = 1;   /* QEMU tests: SeaBIOS selects no DMA mode */
#endif
	c->no_dma = arg_has(args, "dma=off");
	if (!ctl)
		ctl = cmd + 0x206;
	if ((ctl & 3) == 2)
		;   /* 3F6 / 376 */
	else if (ctl == cmd + 0x204 || (ctl & 3) == 0)
		ctl += 2;   /* a 4-port range at 3F4: the register is at +2 */
	c->channel = cmd == 0x170 ? 1 : 0;
	c->cmd = (PUCHAR) ScsiPortGetDeviceBase(c, ci->AdapterInterfaceType, ci->SystemIoBusNumber,
	                                        ScsiPortConvertUlongToPhysicalAddress(cmd), 8, TRUE);
	c->ctl = (PUCHAR) ScsiPortGetDeviceBase(c, ci->AdapterInterfaceType, ci->SystemIoBusNumber,
	                                        ScsiPortConvertUlongToPhysicalAddress(ctl), 1, TRUE);
	if (bm)
		c->bm = (PUCHAR) ScsiPortGetDeviceBase(c, ci->AdapterInterfaceType, ci->SystemIoBusNumber,
		                                       ScsiPortConvertUlongToPhysicalAddress(bm), 8, TRUE);
	if (!c->cmd || !c->ctl)
		return SP_RETURN_ERROR;
	if (!ci->BusInterruptLevel)
		ci->BusInterruptLevel = c->channel ? 15 : 14;
	c->irq = ci->BusInterruptLevel;
	logf("kexata: channel at %x/%x, bus master %x, irq %u\n", cmd, ctl, bm, c->irq);
	ci->MaximumTransferLength = MAX_XFER;
	ci->NumberOfPhysicalBreaks = NPRD - 1;
	ci->ScatterGather = TRUE;
	ci->Master = TRUE;
	ci->Dma32BitAddresses = TRUE;
	ci->AlignmentMask = 1;
	ci->NumberOfBuses = 1;
	ci->InitiatorBusId[0] = 2;
	ci->MaximumNumberOfTargets = 2;
	ci->CachesData = FALSE;
	ci->InterruptMode = Latched;
	if (bm)
	{
		c->prd = (PRD *) ScsiPortGetUncachedExtension(c, ci, NPRD * sizeof(PRD) * 2);
		if (c->prd)
		{
			ULONG len = 0;
			ULONG pa = ScsiPortConvertPhysicalAddressToUlong(ScsiPortGetPhysicalAddress(c, NULL, c->prd, &len));
			/* the table must not cross a 64 KB boundary: use the half that does not */
			if ((pa & 0xffff) + NPRD * sizeof(PRD) > 0x10000)
			{
				c->prd += NPRD;
				pa += NPRD * sizeof(PRD);
			}
			c->prd_pa = pa;
		}
		else
			c->bm = NULL;
	}
	probe(c);
	return SP_RETURN_FOUND;
}

ULONG NTAPI DriverEntry(PVOID drv, PVOID arg2)
{
	HW_INITIALIZATION_DATA h;
	ULONG r;
	memset(&h, 0, sizeof(h));
	/* the Windows NT 4 layout, which is what Windows 98's SCSIPORT knows */
	h.HwInitializationDataSize = FIELD_OFFSET(HW_INITIALIZATION_DATA, HwAdapterControl);
	h.AdapterInterfaceType = Isa;
	h.HwInitialize = hw_initialize;
	h.HwStartIo = hw_startio;
	h.HwInterrupt = hw_interrupt;
	h.HwFindAdapter = hw_find;
	h.HwResetBus = hw_reset;
	h.DeviceExtensionSize = sizeof(CHAN);
	h.NumberOfAccessRanges = 3;
	h.MapBuffers = TRUE;
	h.NeedPhysicalAddresses = TRUE;
	logf("kexata: KernelExEx IDE/ATA driver\n");
	r = ScsiPortInitialize(drv, arg2, &h, NULL);
	logf("kexata: ScsiPortInitialize %x\n", r);
	return r;
}
