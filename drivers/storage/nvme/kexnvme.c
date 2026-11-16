/*
 *  KernelExEx - kexnvme.mpd: NVMe SSDs (M.2, U.2, PCIe cards) for Windows 98
 *  SE / ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  An NT SCSI miniport for Windows 98's SCSIPORT.PDR, like kexata and
 *  kexahci: the SSD's namespace 1 is a SCSI disk (target 0), Windows' disk
 *  driver sits on top. Reads and writes go through I/O queue 1, up to NSLOT
 *  at once, and complete in the interrupt; I/O queue 2 is waited on (before
 *  Windows turns the interrupt on, without one, and for the slow paths).
 *  512-byte blocks only: Windows 98's disk stack knows no other sector size.
 *  Showing 512-byte sectors on a 4 KB-block namespace (read, merge, write
 *  back partial blocks) was tried and corrupted data in QEMU, cause not yet
 *  found; see README. Most SSDs ship with 512-byte blocks or can be
 *  reformatted to them.
 *
 *  Reference: NVM Express Base Specification 1.4 (NVM Express, Inc.).
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

/* ---- NVMe ------------------------------------------------------------------------------- */
#define R_CAP 0x00
#define R_VS 0x08
#define R_INTMS 0x0C
#define R_INTMC 0x10
#define R_CC 0x14
#define R_CSTS 0x1C
#define R_AQA 0x24
#define R_ASQ 0x28
#define R_ACQ 0x30

#define CC_EN 0x1
#define CC_IOSQES (6u << 16)   /* 64-byte submission entries */
#define CC_IOCQES (4u << 20)   /* 16-byte completion entries */
#define CSTS_RDY 0x1
#define CSTS_CFS 0x2

#define ADM_CREATE_SQ 0x01
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY 0x06
#define IO_FLUSH 0x00
#define IO_WRITE 0x01
#define IO_READ 0x02

#define QDEPTH 16
#define NSLOT 8                 /* reads and writes in flight at once */
#define PAGE 4096
#define MAX_XFER 0x20000
#define NPAGES (MAX_XFER / PAGE + 1)
#define PRPL 512                /* bytes of PRP list per command: NPAGES entries fit */

typedef struct
{
	ULONG *sq;   /* QDEPTH entries of 16 dwords */
	ULONG *cq;   /* QDEPTH entries of 4 dwords */
	ULONG sq_pa, cq_pa;
	ULONG sq_tail, cq_head, phase;
	USHORT qid, cid;
} QUEUE;

enum { K_RW = 1, K_FLUSH };

typedef struct
{
	PUCHAR bar;
	ULONG dstrd;
	ULONG timeout_ms;
	QUEUE adm, io, sync;   /* io: queued, completes in the interrupt; sync: waited on */
	PUCHAR idbuf;
	ULONG idbuf_pa;
	PUCHAR prp;            /* NSLOT + 1 PRP lists of PRPL bytes; the last for the sync queue */
	ULONG prp_pa;
	int present;
	ULONGLONG blocks;      /* in the namespace's own blocks */
	ULONG lba_shift;
	char model[41], fw[9];
	UCHAR sense_key, asc, ascq;
	int depth, want_next, want_lu;
	int irq_mode, no_irq, no_queue, takeover;
	/* commands in flight on queue 1, by command id */
	ULONG active;
	int barrier;           /* a flush in flight: nothing else starts */
	struct
	{
		PSCSI_REQUEST_BLOCK srb;
		ULONGLONG lba;     /* 512-byte sectors */
		ULONG count;
		int write, kind;
	} q[NSLOT];
	PSCSI_REQUEST_BLOCK wait[NSLOT + 4];
	int nwait;
} NVME;

static ULONG r32(NVME *n, ULONG off)
{
	return ScsiPortReadRegisterUlong((PULONG) (n->bar + off));
}

static void w32(NVME *n, ULONG off, ULONG v)
{
	ScsiPortWriteRegisterUlong((PULONG) (n->bar + off), v);
}

static void ring_sq(NVME *n, QUEUE *q)
{
	w32(n, 0x1000 + (2 * q->qid) * (4u << n->dstrd), q->sq_tail);
}

static void ring_cq(NVME *n, QUEUE *q)
{
	w32(n, 0x1000 + (2 * q->qid + 1) * (4u << n->dstrd), q->cq_head);
}

/* one command, waited for: returns its status (0 = success), 0xFFFF on time out */
static ULONG submit(NVME *n, QUEUE *q, ULONG *cmd, ULONG ms, ULONG *dw0)
{
	ULONG *e = q->sq + q->sq_tail * 16, i;
	USHORT cid = ++q->cid;
	cmd[0] = (cmd[0] & 0xFFFF) | ((ULONG) cid << 16);
	memcpy(e, cmd, 64);
	q->sq_tail = (q->sq_tail + 1) % QDEPTH;
	ring_sq(n, q);
	for (i = 0; i < ms * 20; i++)
	{
		ULONG *c = q->cq + q->cq_head * 4;
		if (((c[3] >> 16) & 1) == q->phase)
		{
			ULONG status = (c[3] >> 17) & 0x7FFF;
			if (dw0)
				*dw0 = c[0];
			q->cq_head = (q->cq_head + 1) % QDEPTH;
			if (!q->cq_head)
				q->phase ^= 1;
			ring_cq(n, q);
			if ((c[3] & 0xFFFF) != cid)
				logf("kexnvme: completion for %u, expected %u\n", c[3] & 0xFFFF, cid);
			return status;
		}
		if (r32(n, R_CSTS) & CSTS_CFS)
			break;
		ScsiPortStallExecution(50);
	}
	logf("kexnvme: queue %u: command %x timed out (CSTS %x)\n", q->qid, cmd[0] & 0xFF, r32(n, R_CSTS));
	return 0xFFFF;
}

/* ---- the request flow (as kexata: NextRequest only from the outermost call) ----------- */
static void set_sense(NVME *n, UCHAR key, UCHAR asc, UCHAR ascq)
{
	n->sense_key = key;
	n->asc = asc;
	n->ascq = ascq;
}

static void finish(NVME *n, PSCSI_REQUEST_BLOCK srb, UCHAR status)
{
	srb->SrbStatus = status;
	srb->ScsiStatus = status == SRB_STATUS_ERROR ? SCSISTAT_CHECK_CONDITION : SCSISTAT_GOOD;
	ScsiPortNotification(RequestComplete, n, srb);
	n->want_next = 1;
}

static void enter(NVME *n)
{
	n->depth++;
}

static void leave(NVME *n)
{
	if (n->depth == 1)
		while (n->want_next || n->want_lu)
		{
			if (n->want_lu)
			{
				n->want_lu = 0;
				ScsiPortNotification(NextLuRequest, n, 0, 0, 0);
			}
			if (n->want_next)
			{
				n->want_next = 0;
				ScsiPortNotification(NextRequest, n, NULL);
			}
		}
	n->depth--;
}

static void fail_sense(NVME *n, PSCSI_REQUEST_BLOCK srb, UCHAR key, UCHAR asc)
{
	set_sense(n, key, asc, 0);
	srb->DataTransferLength = 0;
	finish(n, srb, SRB_STATUS_ERROR);
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

/* PRP1/PRP2 for a buffer: the first page may start anywhere (dword aligned),
 * every further page must start a page; past two pages PRP2 is a list */
static int build_prp(NVME *n, int list, PSCSI_REQUEST_BLOCK srb, PUCHAR va, ULONG len, ULONG *prp1, ULONG *prp2)
{
	ULONG *pl = (ULONG *) (n->prp + list * PRPL);
	ULONG pages[NPAGES + 1], np = 0;
	while (len)
	{
		ULONG run = 0, pa, take;
		pa = ScsiPortConvertPhysicalAddressToUlong(ScsiPortGetPhysicalAddress(n, srb, va, &run));

		if (!run || run > len)
			run = len;
		while (run)
		{
			if (np > NPAGES || (pa & 3))
				return 0;
			if (np && (pa & (PAGE - 1)))
				return 0;   /* a later page that does not start a page */
			pages[np++] = pa;
			take = PAGE - (pa & (PAGE - 1));
			if (take > run)
				take = run;
			pa += take;
			va += take;
			run -= take;
			len -= take;
		}
	}
	*prp1 = pages[0];
	*prp2 = 0;
	if (np == 2)
		*prp2 = pages[1];
	else if (np > 2)
	{
		ULONG i;
		for (i = 1; i < np; i++)
		{
			pl[(i - 1) * 2] = pages[i];
			pl[(i - 1) * 2 + 1] = 0;
		}
		*prp2 = n->prp_pa + list * PRPL;
	}
	return 1;
}

/* ---- reads and writes --------------------------------------------------------------- */
static void io_cmd(ULONG *cmd, int write, ULONGLONG blk, ULONG nblk, ULONG prp1, ULONG prp2)
{
	memset(cmd, 0, 64);
	cmd[0] = write ? IO_WRITE : IO_READ;
	cmd[1] = 1;   /* namespace 1 */
	cmd[6] = prp1;
	cmd[8] = prp2;
	cmd[10] = (ULONG) blk;
	cmd[11] = (ULONG) (blk >> 32);
	cmd[12] = nblk - 1;
}

static void io_error(NVME *n, PSCSI_REQUEST_BLOCK srb, int write, ULONGLONG lba, ULONG count, ULONG st)
{
	logf("kexnvme: %s of %u at %u: status %x\n", write ? "write" : "read", count, (ULONG) lba, st);
	set_sense(n, st == 0xFFFF ? SCSI_SENSE_NOT_READY : SCSI_SENSE_MEDIUM_ERROR, st == 0xFFFF ? 0x04 : 0x11, 0);
	srb->DataTransferLength = 0;
	finish(n, srb, SRB_STATUS_ERROR);
}

/* waited for on queue 2 */
static void rw_sync(NVME *n, PSCSI_REQUEST_BLOCK srb, ULONGLONG lba, ULONG count, int write)
{
	ULONG cmd[16], prp1, prp2, st;
	if (!build_prp(n, NSLOT, srb, (PUCHAR) srb->DataBuffer, count * 512, &prp1, &prp2))
	{
		fail_sense(n, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	io_cmd(cmd, write, lba, count, prp1, prp2);
	if ((st = submit(n, &n->sync, cmd, n->timeout_ms, NULL)) != 0)
	{
		io_error(n, srb, write, lba, count, st);
		return;
	}
	srb->DataTransferLength = count * 512;
	finish(n, srb, SRB_STATUS_SUCCESS);
}

/* on queue 1, under command id `slot`; completes in the interrupt */
static void issue(NVME *n, int slot, ULONG *cmd)
{
	ULONG *e = n->io.sq + n->io.sq_tail * 16;
	cmd[0] = (cmd[0] & 0xFFFF) | ((ULONG) slot << 16);
	memcpy(e, cmd, 64);
	n->io.sq_tail = (n->io.sq_tail + 1) % QDEPTH;
	n->active |= 1u << slot;
	{
		static int most;
		int c = 0;
		ULONG v;
		for (v = n->active; v; v &= v - 1)
			c++;
		if (c > most)
			most = c, logf("kexnvme: %d in flight\n", most);
	}
	ring_sq(n, &n->io);
	n->want_next = 1;
}

static int free_slot(NVME *n)
{
	int i;
	for (i = 0; i < NSLOT; i++)
		if (!(n->active & (1u << i)))
			return i;
	return -1;
}

/* lba and count in 512-byte sectors */
static void rw(NVME *n, PSCSI_REQUEST_BLOCK srb, ULONGLONG lba, ULONG count, int write)
{
	ULONG cmd[16], prp1, prp2, bytes = count * 512;
	int slot;
	if (!count)
	{
		srb->DataTransferLength = 0;
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	}
	if (lba + count > n->blocks)
	{
		fail_sense(n, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x21);
		return;
	}
	if (bytes > srb->DataTransferLength || bytes > MAX_XFER)
	{
		fail_sense(n, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	if (!n->irq_mode || (slot = free_slot(n)) < 0)
	{
		rw_sync(n, srb, lba, count, write);
		return;
	}
	if (!build_prp(n, slot, srb, (PUCHAR) srb->DataBuffer, bytes, &prp1, &prp2))
	{
		fail_sense(n, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
		return;
	}
	io_cmd(cmd, write, lba, count, prp1, prp2);
	n->q[slot].srb = srb;
	n->q[slot].lba = lba;
	n->q[slot].count = count;
	n->q[slot].write = write;
	n->q[slot].kind = K_RW;
	issue(n, slot, cmd);
}

static void flush(NVME *n, PSCSI_REQUEST_BLOCK srb)
{
	ULONG cmd[16];
	int slot;
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = IO_FLUSH;
	cmd[1] = 1;
	if (n->irq_mode && (slot = free_slot(n)) >= 0)
	{
		n->q[slot].srb = srb;
		n->q[slot].kind = K_FLUSH;
		n->barrier = 1;
		issue(n, slot, cmd);
		return;
	}
	submit(n, &n->sync, cmd, 30000, NULL);
	srb->DataTransferLength = 0;
	finish(n, srb, SRB_STATUS_SUCCESS);
}

static void scsi(NVME *n, PSCSI_REQUEST_BLOCK srb)
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
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_REQUEST_SENSE:
		b[0] = 0x70;
		b[2] = n->sense_key;
		b[7] = 10;
		b[12] = n->asc;
		b[13] = n->ascq;
		set_sense(n, 0, 0, 0);
		reply(srb, b, 18);
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_INQUIRY:
	{
		int i;
		if (cdb[1] & 1)
		{
			fail_sense(n, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x24);
			return;
		}
		b[2] = 2;
		b[3] = 2;
		b[4] = 31;
		memcpy(b + 8, "NVMe    ", 8);
		for (i = 0; i < 16; i++)
			b[16 + i] = (UCHAR) (n->model[i] ? n->model[i] : ' ');
		for (i = 0; i < 4; i++)
			b[32 + i] = (UCHAR) (n->fw[i] ? n->fw[i] : ' ');
		reply(srb, b, 36);
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	}
	case SCSIOP_READ_CAPACITY:
	{
		ULONGLONG last = n->blocks - 1;
		put_be32(b, last > 0xffffffffu ? 0xffffffffu : (ULONG) last);
		put_be32(b + 4, 512);
		reply(srb, b, 8);
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	}
	case SCSIOP_MODE_SENSE:
		b[0] = 3;
		reply(srb, b, 4);
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_MODE_SENSE10:
		b[1] = 6;
		reply(srb, b, 8);
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	case SCSIOP_READ6:
	case SCSIOP_WRITE6:
	{
		ULONG lba = ((ULONG) (cdb[1] & 0x1f) << 16) | ((ULONG) cdb[2] << 8) | cdb[3];
		rw(n, srb, lba, cdb[4] ? cdb[4] : 256, cdb[0] == SCSIOP_WRITE6);
		return;
	}
	case SCSIOP_READ:
	case SCSIOP_WRITE:
	{
		ULONG lba = ((ULONG) cdb[2] << 24) | ((ULONG) cdb[3] << 16) | ((ULONG) cdb[4] << 8) | cdb[5];
		rw(n, srb, lba, ((ULONG) cdb[7] << 8) | cdb[8], cdb[0] == SCSIOP_WRITE);
		return;
	}
	case SCSIOP_READ12:
	case SCSIOP_WRITE12:
	{
		ULONG lba = ((ULONG) cdb[2] << 24) | ((ULONG) cdb[3] << 16) | ((ULONG) cdb[4] << 8) | cdb[5];
		ULONG c = ((ULONG) cdb[6] << 24) | ((ULONG) cdb[7] << 16) | ((ULONG) cdb[8] << 8) | cdb[9];
		rw(n, srb, lba, c, cdb[0] == SCSIOP_WRITE12);
		return;
	}
	case SCSIOP_SYNCHRONIZE_CACHE:
		flush(n, srb);
		return;
	default:
		fail_sense(n, srb, SCSI_SENSE_ILLEGAL_REQUEST, 0x20);
		return;
	}
}

static void startio_body(NVME *n, PSCSI_REQUEST_BLOCK srb)
{
	switch (srb->Function)
	{
	case SRB_FUNCTION_EXECUTE_SCSI:
		break;
	case SRB_FUNCTION_SHUTDOWN:
	case SRB_FUNCTION_FLUSH:
		if (n->present)
		{
			flush(n, srb);
			return;
		}
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	case SRB_FUNCTION_RESET_BUS:
	case SRB_FUNCTION_RESET_DEVICE:
		finish(n, srb, SRB_STATUS_SUCCESS);
		return;
	case SRB_FUNCTION_ABORT_COMMAND:
		finish(n, srb, SRB_STATUS_ABORT_FAILED);
		return;
	default:
		finish(n, srb, SRB_STATUS_INVALID_REQUEST);
		return;
	}
	if (srb->PathId != 0 || srb->TargetId != 0 || srb->Lun != 0 || !n->present)
	{
		finish(n, srb, SRB_STATUS_SELECTION_TIMEOUT);
		return;
	}
	scsi(n, srb);
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

/* can this request start now? Anything when nothing is in flight; otherwise
 * only reads and writes of whole blocks that keep the order of overlapping writes */
static int can_start(NVME *n, PSCSI_REQUEST_BLOCK srb)
{
	ULONGLONG lba;
	ULONG count, t;
	int write;
	if (!n->active)
		return 1;
	if (n->barrier || n->no_queue || free_slot(n) < 0 || !rw_range(srb, &lba, &count, &write) || !count)
		return 0;
	for (t = 0; t < NSLOT; t++)
		if ((n->active & (1u << t)) && n->q[t].kind == K_RW && (write || n->q[t].write) &&
		    lba < n->q[t].lba + n->q[t].count && n->q[t].lba < lba + count)
			return 0;
	return 1;
}

static void pump(NVME *n)
{
	while (n->nwait && can_start(n, n->wait[0]))
	{
		PSCSI_REQUEST_BLOCK srb = n->wait[0];
		int i;
		n->nwait--;
		for (i = 0; i < n->nwait; i++)
			n->wait[i] = n->wait[i + 1];
		startio_body(n, srb);
	}
	/* room for another: ask Windows for the next request */
	if (n->irq_mode && !n->no_queue && n->active && !n->barrier && !n->nwait && free_slot(n) >= 0)
		n->want_lu = 1;
}

static BOOLEAN NTAPI hw_startio(PVOID ext, PSCSI_REQUEST_BLOCK srb)
{
	NVME *n = (NVME *) ext;
	enter(n);
	if (srb->PathId == 0 && srb->TargetId == 0 && srb->Lun == 0 && n->present &&
	    srb->Function != SRB_FUNCTION_ABORT_COMMAND)
	{
		if (n->nwait >= (int) (sizeof(n->wait) / sizeof(n->wait[0])))
			finish(n, srb, SRB_STATUS_BUSY);
		else
		{
			n->wait[n->nwait++] = srb;
			pump(n);
		}
	}
	else
		startio_body(n, srb);
	leave(n);
	return TRUE;
}

static BOOLEAN NTAPI hw_interrupt(PVOID ext)
{
	NVME *n = (NVME *) ext;
	QUEUE *q = &n->io;
	int any = 0;
	if (!n->irq_mode)
		return FALSE;
	enter(n);
	for (;;)
	{
		ULONG *c = q->cq + q->cq_head * 4, cid, st;
		if (((c[3] >> 16) & 1) != q->phase)
			break;
		any = 1;
		cid = c[3] & 0xFFFF;
		st = (c[3] >> 17) & 0x7FFF;
		q->cq_head = (q->cq_head + 1) % QDEPTH;
		if (!q->cq_head)
			q->phase ^= 1;
		if (cid >= NSLOT || !(n->active & (1u << cid)))
		{
			logf("kexnvme: stray completion %x\n", cid);
			continue;
		}
		n->active &= ~(1u << cid);
		if (n->q[cid].kind == K_FLUSH)
		{
			n->barrier = 0;
			n->q[cid].srb->DataTransferLength = 0;
			finish(n, n->q[cid].srb, SRB_STATUS_SUCCESS);
		}
		else if (st)
			io_error(n, n->q[cid].srb, n->q[cid].write, n->q[cid].lba, n->q[cid].count, st);
		else
		{
			n->q[cid].srb->DataTransferLength = n->q[cid].count * 512;
			finish(n, n->q[cid].srb, SRB_STATUS_SUCCESS);
		}
	}
	if (any)
	{
		ring_cq(n, q);
		pump(n);
	}
	leave(n);
	return (BOOLEAN) any;
}

static int controller_up(NVME *n);

static BOOLEAN NTAPI hw_reset(PVOID ext, ULONG path)
{
	NVME *n = (NVME *) ext;
	logf("kexnvme: bus reset (%x in flight)\n", n->active);
	/* the controller may still own commands: start it over */
	n->active = 0;
	n->barrier = 0;
	n->nwait = 0;
	n->present = controller_up(n);
	if (n->irq_mode)
		w32(n, R_INTMC, 1);
	ScsiPortCompleteRequest(n, (UCHAR) path, SP_UNTAGGED, SP_UNTAGGED, SRB_STATUS_BUS_RESET);
	ScsiPortNotification(NextRequest, n, NULL);
	return TRUE;
}

static BOOLEAN NTAPI hw_initialize(PVOID ext)
{
	NVME *n = (NVME *) ext;
	/* from here on Windows takes the interrupt: queue 1's commands complete there */
	if (n->present && !n->no_irq)
	{
		n->irq_mode = 1;
		w32(n, R_INTMC, 1);
	}
	logf("kexnvme: %s\n", n->irq_mode ? (n->no_queue ? "interrupts on, one at a time" : "interrupts on") : "polled");
	return TRUE;
}

static int found_before;

static ULONG find_controller(NVME *n)
{
	ULONG bus, slot;
	int k = 0;
	for (bus = 0; bus < 16; bus++)
		for (slot = 0; slot < 256; slot++)
		{
			UCHAR cfg[0x40];
			ULONG bar0, bar1, pcmd;
			PCI_SLOT_NUMBER sn;
			sn.u.AsULONG = 0;
			sn.u.bits.DeviceNumber = slot >> 3;
			sn.u.bits.FunctionNumber = slot & 7;
			if (ScsiPortGetBusData(n, PCIConfiguration, bus, sn.u.AsULONG, cfg, sizeof(cfg)) < sizeof(cfg) ||
			    *(PUSHORT) cfg == 0xffff || *(PUSHORT) cfg == 0)
				continue;
			if (cfg[0x0b] != 0x01 || cfg[0x0a] != 0x08 || cfg[0x09] != 0x02)
				continue;
			if (k++ != found_before)
				continue;
			found_before++;
			bar0 = *(PULONG) (cfg + 0x10);
			bar1 = (bar0 & 6) == 4 ? *(PULONG) (cfg + 0x14) : 0;
			logf("kexnvme: NVMe %x:%x at %u:%u.%u, BAR0 %x:%x\n", *(PUSHORT) cfg, *(PUSHORT) (cfg + 2), bus, slot >> 3,
			     slot & 7, bar1, bar0);
			if (bar1 || !(bar0 & ~0xFu))
			{
				logf("kexnvme: registers above 4 GB: not reachable from Windows 98\n");
				return 0;
			}
			pcmd = *(PUSHORT) (cfg + 4);
			if ((pcmd & 6) != 6)
			{
				pcmd |= 6;
				ScsiPortSetBusDataByOffset(n, PCIConfiguration, bus, sn.u.AsULONG, &pcmd, 4, 2);
			}
			return bar0 & ~0xFu;
		}
	return 0;
}

static void ascii(char *out, const UCHAR *in, int len)
{
	int i;
	for (i = 0; i < len; i++)
		out[i] = (char) (in[i] >= 32 && in[i] < 127 ? in[i] : ' ');
	out[len] = 0;
	while (len > 0 && out[len - 1] == ' ')
		out[--len] = 0;
}

static int controller_up(NVME *n)
{
	ULONG cap_lo = r32(n, R_CAP), cap_hi = r32(n, R_CAP + 4), i, cmd[16], st;
	n->dstrd = cap_hi & 0xF;
	n->timeout_ms = ((cap_lo >> 24) & 0xFF) * 500 + 1000;
	logf("kexnvme: version %x, CAP %x:%x\n", r32(n, R_VS), cap_hi, cap_lo);
	{
		QUEUE *qs[3];
		int k;
		qs[0] = &n->adm;
		qs[1] = &n->io;
		qs[2] = &n->sync;
		for (k = 0; k < 3; k++)
		{
			memset(qs[k]->sq, 0, PAGE);
			memset(qs[k]->cq, 0, PAGE);
			qs[k]->sq_tail = qs[k]->cq_head = 0;
			qs[k]->phase = 1;
		}
	}
	if ((cap_hi >> 16) & 0xF)
	{
		logf("kexnvme: needs pages over 4 KB\n");
		return 0;
	}
	/* reset: disabled, then the admin queues, then enabled */
	w32(n, R_CC, r32(n, R_CC) & ~CC_EN);
	for (i = 0; i < n->timeout_ms && (r32(n, R_CSTS) & CSTS_RDY); i++)
		ScsiPortStallExecution(1000);
	w32(n, R_INTMS, 0xFFFFFFFFu);
	w32(n, R_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
	w32(n, R_ASQ, n->adm.sq_pa);
	w32(n, R_ASQ + 4, 0);
	w32(n, R_ACQ, n->adm.cq_pa);
	w32(n, R_ACQ + 4, 0);
	w32(n, R_CC, CC_IOSQES | CC_IOCQES | CC_EN);
	for (i = 0; i < n->timeout_ms && !(r32(n, R_CSTS) & CSTS_RDY); i++)
		ScsiPortStallExecution(1000);
	if (!(r32(n, R_CSTS) & CSTS_RDY) || (r32(n, R_CSTS) & CSTS_CFS))
	{
		logf("kexnvme: controller did not come up (CSTS %x)\n", r32(n, R_CSTS));
		return 0;
	}
	/* identify the controller */
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = ADM_IDENTIFY;
	cmd[6] = n->idbuf_pa;
	cmd[10] = 1;
	if ((st = submit(n, &n->adm, cmd, n->timeout_ms, NULL)) != 0)
	{
		logf("kexnvme: identify controller: status %x\n", st);
		return 0;
	}
	ascii(n->model, n->idbuf + 24, 40);
	ascii(n->fw, n->idbuf + 64, 8);
	/* namespace 1 */
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = ADM_IDENTIFY;
	cmd[1] = 1;
	cmd[6] = n->idbuf_pa;
	cmd[10] = 0;
	if ((st = submit(n, &n->adm, cmd, n->timeout_ms, NULL)) != 0)
	{
		logf("kexnvme: identify namespace 1: status %x\n", st);
		return 0;
	}
	n->blocks = *(ULONGLONG *) n->idbuf;
	{
		UCHAR fmt = n->idbuf[26] & 0xF;
		n->lba_shift = (n->idbuf[128 + fmt * 4 + 2]);
	}
	logf("kexnvme: %s, %u MB, %u-byte blocks\n", n->model, (ULONG) ((n->blocks << n->lba_shift) >> 20), 1u << n->lba_shift);
	if (n->lba_shift != 9)
	{
		/* say whether the drive can be reformatted to 512-byte blocks (that erases it) */
		UCHAR k, nf = n->idbuf[25];
		int can = 0;
		for (k = 0; k <= nf && k < 16; k++)
			if (n->idbuf[128 + k * 4 + 2] == 9 && !*(PUSHORT) (n->idbuf + 128 + k * 4))
				can = 1;
		logf("kexnvme: %u-byte blocks: Windows 98 needs 512 (%s)\n", 1u << n->lba_shift,
		     can ? "the drive offers a 512-byte format" : "the drive offers no 512-byte format");
		return 0;
	}
	/* the I/O queue pairs, completion queue first: 1 with the interrupt (vector 0,
	 * masked in INTMS until Windows takes it), 2 without */
	for (i = 1; i <= 2; i++)
	{
		QUEUE *q = i == 1 ? &n->io : &n->sync;
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = ADM_CREATE_CQ;
		cmd[6] = q->cq_pa;
		cmd[10] = ((QDEPTH - 1) << 16) | i;
		cmd[11] = i == 1 ? 3 : 1;   /* physically contiguous, interrupts on queue 1 */
		if ((st = submit(n, &n->adm, cmd, n->timeout_ms, NULL)) != 0)
		{
			logf("kexnvme: create I/O completion queue %u: status %x\n", i, st);
			return 0;
		}
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = ADM_CREATE_SQ;
		cmd[6] = q->sq_pa;
		cmd[10] = ((QDEPTH - 1) << 16) | i;
		cmd[11] = (i << 16) | 1;   /* its completion queue, physically contiguous */
		if ((st = submit(n, &n->adm, cmd, n->timeout_ms, NULL)) != 0)
		{
			logf("kexnvme: create I/O submission queue %u: status %x\n", i, st);
			return 0;
		}
	}
	return 1;
}

static ULONG NTAPI hw_find(PVOID ext, PVOID ctx, PVOID bus, PCHAR args, PPORT_CONFIGURATION_INFORMATION ci,
                           PBOOLEAN again)
{
	NVME *n = (NVME *) ext;
	ULONG bar_pa, len = 0, pa;
	PUCHAR mem;
	PHYSICAL_ADDRESS p;
	(void) ctx;
	(void) bus;
	*again = FALSE;
	memset(n, 0, sizeof(*n));
	{
		/* AdapterSettings: POLL (no interrupts), NOQUEUE (one command at a time) */
		PCHAR a = args;
		for (; a && *a; a++)
		{
			if ((a[0] | 0x20) == 'p' && (a[1] | 0x20) == 'o' && (a[2] | 0x20) == 'l' && (a[3] | 0x20) == 'l')
				n->no_irq = 1;
			if ((a[0] | 0x20) == 'n' && (a[1] | 0x20) == 'o' && (a[2] | 0x20) == 'q' && (a[3] | 0x20) == 'u')
				n->no_queue = 1;
			if ((a[0] | 0x20) == 't' && (a[1] | 0x20) == 'a' && (a[2] | 0x20) == 'k' && (a[3] | 0x20) == 'e')
				n->takeover = 1;
		}
	}
	bar_pa = find_controller(n);
	if (!bar_pa)
		return SP_RETURN_NOT_FOUND;
	/* as on AHCI: Windows 98 does not hand the register block over; map it */
	p.QuadPart = bar_pa;
	n->bar = (PUCHAR) MmMapIoSpace(p, 0x2000, MmNonCached);
	if (!n->bar)
		return SP_RETURN_ERROR;
	/* a controller the BIOS has switched on is serving int 13h (perhaps as the
	 * disk Windows starts from): resetting it would pull it from under the
	 * BIOS, and Windows still calls the BIOS for that disk while it starts
	 * (seen: a protection error, then a damaged FAT). Leave it to the BIOS;
	 * Windows reaches it through int 13h (compatibility mode). TAKEOVER: use
	 * it anyway (for an SSD that holds data only) */
#ifdef KEX_TEST_TAKEOVER
	n->takeover = 1;
#endif
	if ((r32(n, R_CC) & CC_EN) && !n->takeover)
	{
		logf("kexnvme: the BIOS is using this SSD; left to it (AdapterSettings TAKEOVER for a data-only SSD)\n");
		return SP_RETURN_NOT_FOUND;
	}
	/* nine pages: three queue pairs, identify, PRP lists (two pages) */
	mem = (PUCHAR) ScsiPortGetUncachedExtension(n, ci, 10 * PAGE);
	if (!mem)
		return SP_RETURN_ERROR;
	pa = ScsiPortConvertPhysicalAddressToUlong(ScsiPortGetPhysicalAddress(n, NULL, mem, &len));
	{
		ULONG skip = (PAGE - (pa & (PAGE - 1))) & (PAGE - 1);
		mem += skip;
		pa += skip;
	}
	memset(mem, 0, 9 * PAGE);
	n->adm.sq = (ULONG *) mem;
	n->adm.sq_pa = pa;
	n->adm.cq = (ULONG *) (mem + PAGE);
	n->adm.cq_pa = pa + PAGE;
	n->io.sq = (ULONG *) (mem + 2 * PAGE);
	n->io.sq_pa = pa + 2 * PAGE;
	n->io.cq = (ULONG *) (mem + 3 * PAGE);
	n->io.cq_pa = pa + 3 * PAGE;
	n->sync.sq = (ULONG *) (mem + 4 * PAGE);
	n->sync.sq_pa = pa + 4 * PAGE;
	n->sync.cq = (ULONG *) (mem + 5 * PAGE);
	n->sync.cq_pa = pa + 5 * PAGE;
	n->idbuf = mem + 6 * PAGE;
	n->idbuf_pa = pa + 6 * PAGE;
	n->prp = mem + 7 * PAGE;
	n->prp_pa = pa + 7 * PAGE;
	n->adm.qid = 0;
	n->io.qid = 1;
	n->sync.qid = 2;
	n->present = controller_up(n);
	logf("kexnvme: irq level %u vector %u\n", ci->BusInterruptLevel, ci->BusInterruptVector);
	if (!ci->BusInterruptLevel)
		n->no_irq = 1;
#ifdef KEX_TEST_POLL
	n->no_irq = 1;
#endif
	ci->MaximumTransferLength = MAX_XFER;
	ci->NumberOfPhysicalBreaks = NPAGES - 1;
	ci->ScatterGather = TRUE;
	ci->Master = TRUE;
	ci->Dma32BitAddresses = TRUE;
	ci->AlignmentMask = 3;
	ci->NumberOfBuses = 1;
	ci->InitiatorBusId[0] = 7;
	ci->MaximumNumberOfTargets = 1;
	ci->CachesData = FALSE;
	ci->InterruptMode = LevelSensitive;
	return SP_RETURN_FOUND;
}

ULONG NTAPI DriverEntry(PVOID drv, PVOID arg2)
{
	HW_INITIALIZATION_DATA hi;
	ULONG r;
	memset(&hi, 0, sizeof(hi));
	hi.HwInitializationDataSize = FIELD_OFFSET(HW_INITIALIZATION_DATA, HwAdapterControl);
	hi.AdapterInterfaceType = Isa;
	hi.HwInitialize = hw_initialize;
	hi.HwStartIo = hw_startio;
	hi.HwInterrupt = hw_interrupt;
	hi.HwFindAdapter = hw_find;
	hi.HwResetBus = hw_reset;
	hi.DeviceExtensionSize = sizeof(NVME);
	hi.NumberOfAccessRanges = 6;
	hi.MapBuffers = TRUE;
	hi.NeedPhysicalAddresses = TRUE;
	/* Windows 98 sends a disk more requests at once when asked (NextLuRequest) */
	hi.TaggedQueuing = TRUE;
	hi.MultipleRequestPerLu = TRUE;
	logf("kexnvme: KernelExEx NVMe driver\n");
	r = ScsiPortInitialize(drv, arg2, &hi, NULL);
	logf("kexnvme: ScsiPortInitialize %x\n", r);
	return r;
}
