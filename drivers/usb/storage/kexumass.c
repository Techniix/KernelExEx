/*
 *  KernelExEx - kexumass.sys: USB flash drives, disks and card readers on
 *  Windows 98 SE / ME (USB mass storage, Bulk-Only Transport).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Windows 98 SE has the upper half already: NTMAP (ntmap.sys,
 *  ntmaphlp.pdr, ntmap.inf "1394/USB Disk") connects Windows' disk and
 *  CD-ROM layer (IOS) to WDM storage devices, as it does for 1394 disks.
 *  As in sbp2.inf, ntmap.sys is the upper filter on our device; the disks
 *  are our children, with IDs ending in USBSTOR\GenDisk or GenCdRom, which
 *  ntmap.inf takes; they get SCSI request blocks (IOCTL_SCSI_EXECUTE_*)
 *  and run each as a Bulk-Only command (CBW, data, CSW). Error recovery as
 *  the Bulk-Only spec says (clear halt, CSW, reset recovery); sense data on
 *  a CHECK CONDITION is fetched at once (auto sense). It runs on Windows'
 *  USB 1.1 stack and on KernelExEx's USB 2.0 driver alike.
 *
 *  Found in testing on Windows 98 SE:
 *  - URBs sent while START_DEVICE is still on its way back are not
 *    completed; the device is set up in the first relations query.
 *  - Windows' USBD loops forever on a SELECT_CONFIGURATION URB longer than
 *    its interfaces; and never completes requests with buffers on the
 *    stack (small buffers go through pool).
 *  - A disk the BIOS also offers through INT 13h (BIOS "USB legacy"
 *    storage, or QEMU's SeaBIOS) appears twice: once in MS-DOS
 *    compatibility mode. Plug such disks in after Windows started.
 *
 *  Reference: USB Mass Storage Class Bulk-Only Transport 1.0; SPC/SBC/MMC.
 *  Only kernel functions Windows 98 SE provides.
 */
#include <ntddk.h>
#undef _NTDDK_
#include <srb.h>
#include <scsi.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <stdarg.h>

#define TAG 0x7473754bu   /* "Kust" */

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

/* ---- log --------------------------------------------------------------------------- */
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

/* ---- structures -------------------------------------------------------------------- */
#define MAX_LUN 8
#define CHUNK 0x10000   /* per bulk URB */

enum
{
	EXT_FDO = 0x46535553,
	EXT_PDO = 0x50535553
};

struct FDO;

typedef struct
{
	int kind;
	struct FDO *fdo;
	PDEVICE_OBJECT self;
	int lun;
	UCHAR type;              /* SCSI peripheral type: 0 disk, 5 CD/DVD, 7 optical */
	UCHAR removable;
	char vendor[9], product[17], rev[5];
	int reported, removed, got_remove;
	LIST_ENTRY queue;        /* SRB IRPs waiting for the worker */
} PDOEXT;

typedef struct FDO
{
	int kind;
	PDEVICE_OBJECT self, pdo, lower;
	int index;
	USBD_PIPE_HANDLE in, out;
	USHORT in_max, out_max;
	UCHAR ifno, subclass, protocol;
	int nlun;
	PDOEXT *luns[MAX_LUN];
	WCHAR serial[40];
	ULONG tag;
	KSPIN_LOCK lock;
	KEVENT work, stopped;
	HANDLE thread;
	volatile int stop, gone, started, scanned;
	PUCHAR bounce;           /* for buffers no URB can take directly */
} FDO;

static PDRIVER_OBJECT g_drv;
static LONG g_index;

/* ---- USB requests (worker thread, PASSIVE_LEVEL) ----------------------------------- */
/* submit an URB and wait; cancel it after timeout_ms (the worker thread only:
 * Windows 98's USB stack does not complete URBs sent from START_DEVICE) */
static NTSTATUS urb_call(FDO *f, PURB urb, ULONG timeout_ms)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS st;
	LARGE_INTEGER t;
	if (f->gone)
		return STATUS_DEVICE_NOT_CONNECTED;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, f->lower, NULL, 0, NULL, 0, TRUE, &ev, &iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;
	st = IoCallDriver(f->lower, irp);
	if (st == STATUS_PENDING)
	{
		t.QuadPart = -(LONGLONG) timeout_ms * 10000;
		if (KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT)
		{
			logf("kexumass: URB %x timed out\n", urb->UrbHeader.Function);
			IoCancelIrp(irp);
			t.QuadPart = -5000 * 10000;
			if (KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT)
			{
				logf("kexumass: the USB stack did not cancel it: the device is treated as gone\n");
				f->gone = 1;   /* the IRP stays with the stack: leave it */
				return STATUS_DEVICE_NOT_CONNECTED;
			}
		}
		st = iosb.Status;
	}
	if (NT_SUCCESS(st) && !USBD_SUCCESS(urb->UrbHeader.Status))
		st = STATUS_UNSUCCESSFUL;
	return st;
}

/* Windows 98's USB 1.1 stack cannot take a buffer on the stack (it never
 * completes the request): small buffers go through f->bounce, from pool */
#define BOUNCE 1024

static NTSTATUS bulk(FDO *f, int in, void *buf, ULONG len, ULONG *got, ULONG timeout_ms, USBD_STATUS *us)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER u;
	NTSTATUS st;
	void *orig = buf;
	if (len && len <= BOUNCE && f->bounce)
	{
		if (!in)
			memcpy(f->bounce, buf, len);
		buf = f->bounce;
	}
	memset(&u, 0, sizeof(u));
	u.Hdr.Length = sizeof(u);
	u.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
	u.PipeHandle = in ? f->in : f->out;
	u.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
	u.TransferBuffer = buf;
	u.TransferBufferLength = len;
	st = urb_call(f, (PURB) &u, timeout_ms);
	if (got)
		*got = u.TransferBufferLength;
	if (us)
		*us = u.Hdr.Status;
	if (buf != orig && in)
		memcpy(orig, buf, u.TransferBufferLength <= len ? u.TransferBufferLength : len);
	return st;
}

static NTSTATUS reset_pipe(FDO *f, USBD_PIPE_HANDLE p)
{
	struct _URB_PIPE_REQUEST u;
	memset(&u, 0, sizeof(u));
	u.Hdr.Length = sizeof(u);
	u.Hdr.Function = URB_FUNCTION_RESET_PIPE;   /* host side and CLEAR_FEATURE(HALT) */
	u.PipeHandle = p;
	return urb_call(f, (PURB) &u, 5000);
}

static NTSTATUS class_req(FDO *f, int in, UCHAR req, USHORT val, void *buf, ULONG len, ULONG *got)
{
	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST u;
	NTSTATUS st;
	void *orig = buf;
	if (len && len <= BOUNCE && f->bounce)
	{
		if (!in)
			memcpy(f->bounce, buf, len);
		buf = f->bounce;
	}
	memset(&u, 0, sizeof(u));
	u.Hdr.Length = sizeof(u);
	u.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
	u.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
	u.TransferBuffer = buf;
	u.TransferBufferLength = len;
	u.Request = req;
	u.Value = val;
	u.Index = f->ifno;
	st = urb_call(f, (PURB) &u, 5000);
	if (got)
		*got = u.TransferBufferLength;
	if (buf != orig && in)
		memcpy(orig, buf, u.TransferBufferLength <= len ? u.TransferBufferLength : len);
	return st;
}

static void reset_recovery(FDO *f)
{
	logf("kexumass: reset recovery\n");
	class_req(f, 0, 0xff, 0, NULL, 0, NULL);   /* Bulk-Only Mass Storage Reset */
	reset_pipe(f, f->in);
	reset_pipe(f, f->out);
}

/* ---- one Bulk-Only command --------------------------------------------------------- */
enum
{
	BOT_OK,
	BOT_FAILED,    /* CSW status 1: CHECK CONDITION */
	BOT_ERROR,     /* transport error, after reset recovery */
	BOT_GONE
};

static int bot(FDO *f, int lun, const UCHAR *cdb, int cdblen, PUCHAR data, ULONG len, int in, ULONG *residue)
{
	UCHAR cbw[31], csw[13];
	ULONG got = 0, done = 0, tag, i;
	USBD_STATUS us = 0;
	NTSTATUS st;
	int stalled = 0;
	memset(cbw, 0, sizeof(cbw));
	tag = ++f->tag;
	cbw[0] = 'U';
	cbw[1] = 'S';
	cbw[2] = 'B';
	cbw[3] = 'C';
	*(PULONG) (cbw + 4) = tag;
	*(PULONG) (cbw + 8) = len;
	cbw[12] = (UCHAR) (in && len ? 0x80 : 0);
	cbw[13] = (UCHAR) lun;
	cbw[14] = (UCHAR) (cdblen > 16 ? 16 : cdblen);
	/* old ATAPI-command-set devices want 12-byte commands */
	if (f->subclass != 6 && cbw[14] < 12)
		cbw[14] = 12;
	memcpy(cbw + 15, cdb, cdblen > 16 ? 16 : cdblen);
	st = bulk(f, 0, cbw, 31, NULL, 5000, &us);
	if (!NT_SUCCESS(st))
	{
		if (f->gone || st == STATUS_DEVICE_NOT_CONNECTED)
			return BOT_GONE;
		reset_recovery(f);
		return BOT_ERROR;
	}
	/* data stage, in pieces of at most CHUNK */
	while (done < len)
	{
		ULONG n = len - done > CHUNK ? CHUNK : len - done;
		got = 0;
		st = bulk(f, in, data + done, n, &got, 20000, &us);
		if (!NT_SUCCESS(st))
		{
			if (f->gone || st == STATUS_DEVICE_NOT_CONNECTED)
				return BOT_GONE;
			if (us == USBD_STATUS_STALL_PID || us == USBD_STATUS_ENDPOINT_HALTED)
			{
				/* the device ends the data stage early: clear it, read the CSW */
				reset_pipe(f, in ? f->in : f->out);
				stalled = 1;
				break;
			}
			reset_recovery(f);
			return BOT_ERROR;
		}
		done += got;
		if (got < n)
			break;   /* short: the device has no more */
	}
	/* status: retry once after a stall */
	for (i = 0; i < 2; i++)
	{
		got = 0;
		st = bulk(f, 1, csw, 13, &got, 20000, &us);
		if (NT_SUCCESS(st))
			break;
		if (f->gone || st == STATUS_DEVICE_NOT_CONNECTED)
			return BOT_GONE;
		if (us != USBD_STATUS_STALL_PID && us != USBD_STATUS_ENDPOINT_HALTED)
			break;
		reset_pipe(f, f->in);
	}
	if (!NT_SUCCESS(st) || got != 13 || csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' ||
	    *(PULONG) (csw + 4) != tag || csw[12] == 2)
	{
		logf("kexumass: bad CSW (%u bytes, status %u) after command %x\n", got, csw[12], cdb[0]);
		reset_recovery(f);
		return BOT_ERROR;
	}
	if (residue)
	{
		ULONG r = *(PULONG) (csw + 8);
		/* trust what was really moved when the device's residue disagrees */
		*residue = len - done > r ? len - done : r;
		if (*residue > len)
			*residue = len;
	}
	(void) stalled;
	return csw[12] == 0 ? BOT_OK : BOT_FAILED;
}

/* ---- SRBs ---------------------------------------------------------------------------- */
static PUCHAR srb_buffer(PIRP irp, PSCSI_REQUEST_BLOCK srb)
{
	PMDL m = irp->MdlAddress;
	PUCHAR sys;
	if (!srb->DataTransferLength)
		return NULL;
	if (!m)
		return (PUCHAR) srb->DataBuffer;
	if (m->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL))
		sys = (PUCHAR) m->MappedSystemVa;
	else
		sys = (PUCHAR) MmMapLockedPages(m, KernelMode);
	if (!sys)
		return NULL;
	return sys + ((PUCHAR) srb->DataBuffer - (PUCHAR) MmGetMdlVirtualAddress(m));
}

static void run_srb(FDO *f, PDOEXT *p, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	PSCSI_REQUEST_BLOCK srb = sl->Parameters.Scsi.Srb;
	PUCHAR buf = srb_buffer(irp, srb);
	ULONG residue = 0;
	int in = (srb->SrbFlags & SRB_FLAGS_DATA_IN) != 0, r;
	UCHAR cdb[16];
	memset(cdb, 0, sizeof(cdb));
	memcpy(cdb, srb->Cdb, srb->CdbLength > 16 ? 16 : srb->CdbLength);
	if (srb->DataTransferLength && !buf)
	{
		srb->SrbStatus = SRB_STATUS_ERROR;
		irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return;
	}
	r = bot(f, p->lun, cdb, srb->CdbLength, buf, srb->DataTransferLength, in, &residue);
	srb->ScsiStatus = SCSISTAT_GOOD;
	switch (r)
	{
	case BOT_OK:
		srb->SrbStatus = SRB_STATUS_SUCCESS;
		if (residue)
		{
			srb->DataTransferLength -= residue;
			srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;   /* an underrun, in SCSIPORT terms */
		}
		irp->IoStatus.Status = STATUS_SUCCESS;
		break;
	case BOT_FAILED:
		srb->SrbStatus = SRB_STATUS_ERROR;
		srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
		srb->DataTransferLength -= residue;
		/* auto sense */
		if (srb->SenseInfoBuffer && srb->SenseInfoBufferLength && !(srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE) &&
		    cdb[0] != SCSIOP_REQUEST_SENSE)
		{
			UCHAR rs[12], sense[18];
			ULONG sres = 0;
			memset(rs, 0, sizeof(rs));
			memset(sense, 0, sizeof(sense));
			rs[0] = SCSIOP_REQUEST_SENSE;
			rs[1] = (UCHAR) (f->subclass != 6 ? p->lun << 5 : 0);
			rs[4] = 18;
			if (bot(f, p->lun, rs, f->subclass == 6 ? 6 : 12, sense, 18, 1, &sres) == BOT_OK)
			{
				ULONG n = 18 - sres;
				if (n > srb->SenseInfoBufferLength)
					n = srb->SenseInfoBufferLength;
				memcpy(srb->SenseInfoBuffer, sense, n);
				srb->SenseInfoBufferLength = (UCHAR) n;
				srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
				logf("kexumass: LUN %u command %x: sense %x/%x/%x\n", p->lun, cdb[0], sense[2] & 15, sense[12],
				     sense[13]);
			}
		}
		irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
		break;
	case BOT_GONE:
		srb->SrbStatus = SRB_STATUS_NO_DEVICE;
		srb->DataTransferLength = 0;
		irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
		break;
	default:
		srb->SrbStatus = SRB_STATUS_BUS_RESET;
		srb->DataTransferLength = 0;
		irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
		break;
	}
	irp->IoStatus.Information = srb->DataTransferLength;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
}

/* the worker: every SRB of every LUN, one at a time */
static void scan_luns(FDO *f);
static NTSTATUS configure(FDO *f);

static VOID NTAPI worker(PVOID ctx)
{
	FDO *f = (FDO *) ctx;
	logf("kexumass: device %u: worker\n", f->index);
	{
		/* a device plugged in while Windows runs gets no relations query by
		 * itself: ask for one (the setup happens there) */
		LARGE_INTEGER t;
		t.QuadPart = -500 * 10000;
		KeDelayExecutionThread(KernelMode, FALSE, &t);
		if (!f->scanned)
			IoInvalidateDeviceRelations(f->pdo, BusRelations);
	}
	for (;;)
	{
		PIRP irp = NULL;
		PDOEXT *p = NULL;
		KIRQL old;
		int i;
		KeWaitForSingleObject(&f->work, Executive, KernelMode, FALSE, NULL);
		for (;;)
		{
			irp = NULL;
			KeAcquireSpinLock(&f->lock, &old);
			for (i = 0; i < f->nlun && !irp; i++)
				if (f->luns[i] && !IsListEmpty(&f->luns[i]->queue))
				{
					p = f->luns[i];
					irp = CONTAINING_RECORD(RemoveHeadList(&p->queue), IRP, Tail.Overlay.ListEntry);
				}
			KeReleaseSpinLock(&f->lock, old);
			if (!irp)
				break;
			if (f->stop || f->gone)
			{
				PSCSI_REQUEST_BLOCK srb = IoGetCurrentIrpStackLocation(irp)->Parameters.Scsi.Srb;
				srb->SrbStatus = SRB_STATUS_NO_DEVICE;
				irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
				IoCompleteRequest(irp, IO_NO_INCREMENT);
				continue;
			}
			run_srb(f, p, irp);
		}
		if (f->stop)
			break;
	}
	KeSetEvent(&f->stopped, 0, FALSE);
	PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- the child devices (one per LUN) ----------------------------------------------- */
static WCHAR *put_s(WCHAR *p, const char *s)
{
	while (*s)
		*p++ = (WCHAR) *s++;
	return p;
}

static WCHAR *put_id(WCHAR *p, const char *s, int n)
{
	int i;
	for (i = 0; i < n && s[i]; i++)
	{
		char c = s[i];
		*p++ = (WCHAR) (c <= ' ' || c == ',' || c > 0x7e ? '_' : c);
	}
	return p;
}

static WCHAR *dev_id(WCHAR *p, PDOEXT *x)
{
	p = put_s(p, x->type == 5 ? "USBSTOR\\CdRom&Ven_" : x->type == 7 ? "USBSTOR\\Optical&Ven_" : "USBSTOR\\Disk&Ven_");
	p = put_id(p, x->vendor, 8);
	p = put_s(p, "&Prod_");
	p = put_id(p, x->product, 16);
	p = put_s(p, "&Rev_");
	return put_id(p, x->rev, 4);
}

static const char *generic_id(PDOEXT *x)
{
	return x->type == 5 ? "GenCdRom" : x->type == 7 ? "GenOptical" : "USBSTOR\\GenDisk";
}

static NTSTATUS pdo_query_id(PDOEXT *x, BUS_QUERY_ID_TYPE t, PWCHAR *out)
{
	WCHAR *b = (WCHAR *) ExAllocatePoolWithTag(PagedPool, 512, TAG), *p = b;
	if (!b)
		return STATUS_INSUFFICIENT_RESOURCES;
	switch (t)
	{
	case BusQueryDeviceID:
		p = dev_id(p, x);
		*p++ = 0;
		break;
	case BusQueryHardwareIDs:
		p = dev_id(p, x);
		*p++ = 0;
		p = put_s(p, generic_id(x));
		*p++ = 0;
		*p++ = 0;
		break;
	case BusQueryCompatibleIDs:
		p = put_s(p, generic_id(x));
		*p++ = 0;
		if (x->type == 0)
		{
			p = put_s(p, "GenDisk");
			*p++ = 0;
		}
		*p++ = 0;
		break;
	case BusQueryInstanceID:
	{
		int i;
		FDO *f = x->fdo;
		if (f->serial[0])
			for (i = 0; f->serial[i] && i < 32; i++)
				*p++ = f->serial[i];
		else
		{
			p = put_s(p, "KEX");
			*p++ = (WCHAR) ('0' + f->index % 10);
		}
		*p++ = '&';
		*p++ = (WCHAR) ('0' + x->lun);
		*p++ = 0;
		break;
	}
	default:
		ExFreePool(b);
		return STATUS_NOT_SUPPORTED;
	}
	*out = b;
	return STATUS_SUCCESS;
}

static NTSTATUS pdo_pnp(PDOEXT *x, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st = irp->IoStatus.Status;
	switch (sl->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
	case IRP_MN_QUERY_STOP_DEVICE:
	case IRP_MN_QUERY_REMOVE_DEVICE:
	case IRP_MN_CANCEL_STOP_DEVICE:
	case IRP_MN_CANCEL_REMOVE_DEVICE:
	case IRP_MN_STOP_DEVICE:
	case IRP_MN_SURPRISE_REMOVAL:
	case IRP_MN_QUERY_RESOURCES:
	case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
		st = STATUS_SUCCESS;
		break;
	case IRP_MN_REMOVE_DEVICE:
		x->got_remove = 1;
		if (x->fdo->gone || x->removed)
		{
			PDEVICE_OBJECT d = x->self;
			irp->IoStatus.Status = STATUS_SUCCESS;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			IoDeleteDevice(d);
			return STATUS_SUCCESS;
		}
		st = STATUS_SUCCESS;
		break;
	case IRP_MN_QUERY_ID:
	{
		PWCHAR id = NULL;
		st = pdo_query_id(x, sl->Parameters.QueryId.IdType, &id);
		if (NT_SUCCESS(st))
			irp->IoStatus.Information = (ULONG_PTR) id;
		else
			st = irp->IoStatus.Status;
		break;
	}
	case IRP_MN_QUERY_DEVICE_TEXT:
		if (sl->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
		{
			WCHAR *b = (WCHAR *) ExAllocatePoolWithTag(PagedPool, 128, TAG), *p = b;
			int i;
			if (!b)
			{
				st = STATUS_INSUFFICIENT_RESOURCES;
				break;
			}
			for (i = 0; i < 8 && x->vendor[i]; i++)
				*p++ = (WCHAR) x->vendor[i];
			*p++ = ' ';
			for (i = 0; i < 16 && x->product[i]; i++)
				*p++ = (WCHAR) x->product[i];
			p = put_s(p, " USB Device");
			*p = 0;
			irp->IoStatus.Information = (ULONG_PTR) b;
			st = STATUS_SUCCESS;
		}
		break;
	case IRP_MN_QUERY_CAPABILITIES:
	{
		PDEVICE_CAPABILITIES c = sl->Parameters.DeviceCapabilities.Capabilities;
		int i;
		c->Removable = TRUE;
		c->EjectSupported = FALSE;
		c->LockSupported = FALSE;
		c->UniqueID = x->fdo->serial[0] ? TRUE : FALSE;
		c->SilentInstall = TRUE;
		c->SurpriseRemovalOK = TRUE;
		c->RawDeviceOK = FALSE;
		c->Address = (ULONG) x->lun;
		c->UINumber = (ULONG) x->lun;
		c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
		for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++)
			c->DeviceState[i] = PowerDeviceD3;
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
			r->Objects[0] = x->self;
			ObReferenceObject(x->self);
			irp->IoStatus.Information = (ULONG_PTR) r;
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

/* IRP_MJ_SCSI: SRBs from NTMAP */
static NTSTATUS pdo_scsi(PDOEXT *x, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	PSCSI_REQUEST_BLOCK srb = sl->Parameters.Scsi.Srb;
	FDO *f = x->fdo;
	KIRQL old;
	if (!srb)
	{
		/* a device I/O control (IOCTL_DISK_GET_DRIVE_GEOMETRY and the like): not ours */
		NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
		logf("kexumass: LUN %u: ioctl %x\n", x->lun, sl->Parameters.DeviceIoControl.IoControlCode);
		irp->IoStatus.Status = st;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	}
	switch (srb->Function)
	{
	case SRB_FUNCTION_EXECUTE_SCSI:
		if (f->gone || x->removed)
		{
			srb->SrbStatus = SRB_STATUS_NO_DEVICE;
			irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			return STATUS_DEVICE_NOT_CONNECTED;
		}
		IoMarkIrpPending(irp);
		srb->SrbStatus = SRB_STATUS_PENDING;
		KeAcquireSpinLock(&f->lock, &old);
		InsertTailList(&x->queue, &irp->Tail.Overlay.ListEntry);
		KeReleaseSpinLock(&f->lock, old);
		KeSetEvent(&f->work, 0, FALSE);
		return STATUS_PENDING;
	case SRB_FUNCTION_CLAIM_DEVICE:
		srb->DataBuffer = x->self;
		/* fall through */
	case SRB_FUNCTION_RELEASE_DEVICE:
	case SRB_FUNCTION_RELEASE_QUEUE:
	case SRB_FUNCTION_FLUSH_QUEUE:
	case SRB_FUNCTION_LOCK_QUEUE:
	case SRB_FUNCTION_UNLOCK_QUEUE:
	case SRB_FUNCTION_FLUSH:
	case SRB_FUNCTION_SHUTDOWN:
		srb->SrbStatus = SRB_STATUS_SUCCESS;
		irp->IoStatus.Status = STATUS_SUCCESS;
		break;
	default:
		logf("kexumass: LUN %u: SRB function %x\n", x->lun, srb->Function);
		srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
		irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}
	{
		NTSTATUS st = irp->IoStatus.Status;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	}
}

/* ---- the USB device ------------------------------------------------------------------ */
static NTSTATUS get_desc(FDO *f, UCHAR type, UCHAR idx, USHORT lang, void *buf, ULONG len, ULONG *got)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST u;
	NTSTATUS st;
	void *orig = buf;
	if (len <= BOUNCE && f->bounce)
		buf = f->bounce;
	memset(&u, 0, sizeof(u));
	u.Hdr.Length = sizeof(u);
	u.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
	u.DescriptorType = type;
	u.Index = idx;
	u.LanguageId = lang;
	u.TransferBuffer = buf;
	u.TransferBufferLength = len;
	st = urb_call(f, (PURB) &u, 5000);
	if (got)
		*got = u.TransferBufferLength;
	if (buf != orig)
		memcpy(orig, buf, u.TransferBufferLength <= len ? u.TransferBufferLength : len);
	return st;
}

/* select the configuration, find the mass storage interface and its bulk pipes */
static NTSTATUS configure(FDO *f)
{
	UCHAR *cfg = (UCHAR *) ExAllocatePoolWithTag(NonPagedPool, 1024, TAG);
	USB_DEVICE_DESCRIPTOR dd;
	ULONG got = 0, off, i, sz;
	PUSB_INTERFACE_DESCRIPTOR id = NULL;
	PURB sel;
	PUSBD_INTERFACE_INFORMATION ii;
	NTSTATUS st;
	if (!cfg)
		return STATUS_INSUFFICIENT_RESOURCES;
	if (NT_SUCCESS(get_desc(f, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, &dd, sizeof(dd), &got)) && dd.iSerialNumber)
	{
		UCHAR s[66];
		USHORT lang = 0x409;
		UCHAR l[4];
		if (NT_SUCCESS(get_desc(f, USB_STRING_DESCRIPTOR_TYPE, 0, 0, l, 4, &got)) && got >= 4)
			lang = (USHORT) (l[2] | (l[3] << 8));
		if (NT_SUCCESS(get_desc(f, USB_STRING_DESCRIPTOR_TYPE, dd.iSerialNumber, lang, s, sizeof(s), &got)) && got > 2)
		{
			int n = (int) (got - 2) / 2, k, ok = 1;
			if (n > 32)
				n = 32;
			for (k = 0; k < n; k++)
			{
				WCHAR c = (WCHAR) (s[2 + 2 * k] | (s[3 + 2 * k] << 8));
				if (c <= ' ' || c > 0x7e || c == ',' || c == '\\')
					ok = 0;
				f->serial[k] = c;
			}
			f->serial[n] = 0;
			if (!ok)
				f->serial[0] = 0;
		}
	}
	st = get_desc(f, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, cfg, 1024, &got);
	if (!NT_SUCCESS(st) || got < 9)
	{
		ExFreePool(cfg);
		return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
	}
	if (got > ((PUSB_CONFIGURATION_DESCRIPTOR) cfg)->wTotalLength)
		got = ((PUSB_CONFIGURATION_DESCRIPTOR) cfg)->wTotalLength;
	for (off = 0; off + 2 <= got && cfg[off] >= 2; off += cfg[off])
	{
		PUSB_INTERFACE_DESCRIPTOR d = (PUSB_INTERFACE_DESCRIPTOR) (cfg + off);
		if (d->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE && d->bInterfaceClass == 8 && d->bAlternateSetting == 0 &&
		    d->bInterfaceProtocol == 0x50)
		{
			id = d;
			break;
		}
	}
	if (!id)
	{
		logf("kexumass: no Bulk-Only mass storage interface\n");
		ExFreePool(cfg);
		return STATUS_NOT_SUPPORTED;
	}
	f->ifno = id->bInterfaceNumber;
	f->subclass = id->bInterfaceSubClass;
	f->protocol = id->bInterfaceProtocol;
	/* one request for every interface of the configuration (composite devices
	 * keep theirs, unused) */
	{
		PUSB_CONFIGURATION_DESCRIPTOR cd = (PUSB_CONFIGURATION_DESCRIPTOR) cfg;
		ULONG nif = cd->bNumInterfaces, maxpipes = 0;
		for (off = 0; off + 2 <= got && cfg[off] >= 2; off += cfg[off])
		{
			PUSB_INTERFACE_DESCRIPTOR d = (PUSB_INTERFACE_DESCRIPTOR) (cfg + off);
			if (d->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE && d->bAlternateSetting == 0 && d->bNumEndpoints > maxpipes)
				maxpipes = d->bNumEndpoints;
		}
		if (!maxpipes)
			maxpipes = 1;
		sz = sizeof(struct _URB_SELECT_CONFIGURATION) +
		     nif * (sizeof(USBD_INTERFACE_INFORMATION) + maxpipes * sizeof(USBD_PIPE_INFORMATION));
		sel = (PURB) ExAllocatePoolWithTag(NonPagedPool, sz, TAG);
		if (!sel)
		{
			ExFreePool(cfg);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		memset(sel, 0, sz);
		sel->UrbHeader.Length = (USHORT) sz;
		sel->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
		sel->UrbSelectConfiguration.ConfigurationDescriptor = cd;
		ii = &sel->UrbSelectConfiguration.Interface;
		for (off = 0, i = 0; off + 2 <= got && cfg[off] >= 2 && i < nif; off += cfg[off])
		{
			PUSB_INTERFACE_DESCRIPTOR d = (PUSB_INTERFACE_DESCRIPTOR) (cfg + off);
			ULONG k;
			if (d->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE || d->bAlternateSetting)
				continue;
			ii->InterfaceNumber = d->bInterfaceNumber;
			ii->NumberOfPipes = d->bNumEndpoints;
			ii->Length = (USHORT) (sizeof(USBD_INTERFACE_INFORMATION) +
			                       (d->bNumEndpoints ? d->bNumEndpoints - 1 : 0) * sizeof(USBD_PIPE_INFORMATION));
			for (k = 0; k < d->bNumEndpoints; k++)
				ii->Pipes[k].MaximumTransferSize = CHUNK;
			ii = (PUSBD_INTERFACE_INFORMATION) ((PUCHAR) ii + ii->Length);
			i++;
		}
		/* exactly the interfaces given: Windows 98's USBD walks to the end of
		 * the URB and loops forever on an empty entry */
		sel->UrbHeader.Length = (USHORT) ((PUCHAR) ii - (PUCHAR) sel);
		st = urb_call(f, sel, 5000);
	}
	if (!NT_SUCCESS(st))
	{
		logf("kexumass: SELECT_CONFIGURATION failed %x / %x\n", st, sel->UrbHeader.Status);
		ExFreePool(sel);
		ExFreePool(cfg);
		return st;
	}
	ii = &sel->UrbSelectConfiguration.Interface;
	for (i = 0; i < ((PUSB_CONFIGURATION_DESCRIPTOR) cfg)->bNumInterfaces; i++)
	{
		ULONG k;
		if (ii->InterfaceNumber == f->ifno)
			for (k = 0; k < ii->NumberOfPipes; k++)
				if (ii->Pipes[k].PipeType == UsbdPipeTypeBulk)
				{
					if (ii->Pipes[k].EndpointAddress & 0x80)
					{
						if (!f->in)
						{
							f->in = ii->Pipes[k].PipeHandle;
							f->in_max = ii->Pipes[k].MaximumPacketSize;
						}
					}
					else if (!f->out)
					{
						f->out = ii->Pipes[k].PipeHandle;
						f->out_max = ii->Pipes[k].MaximumPacketSize;
					}
				}
		ii = (PUSBD_INTERFACE_INFORMATION) ((PUCHAR) ii + ii->Length);
	}
	ExFreePool(sel);
	ExFreePool(cfg);
	if (!f->in || !f->out)
		return STATUS_NOT_SUPPORTED;
	logf("kexumass: interface %u, subclass %x, bulk packets %u/%u\n", f->ifno, f->subclass, f->in_max, f->out_max);
	return STATUS_SUCCESS;
}

static void trim(char *s, int n)
{
	s[n] = 0;
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0))
		s[--n] = 0;
}

/* find the logical units and create a child device for each */
static void scan_luns(FDO *f)
{
	UCHAR maxlun = 0, inq[36];
	ULONG got = 0;
	int l;
	if (NT_SUCCESS(class_req(f, 1, 0xfe, 0, &maxlun, 1, &got)) && got == 1 && maxlun < MAX_LUN)
		;
	else
	{
		maxlun = 0;   /* many devices stall GET MAX LUN: one unit */
		reset_pipe(f, f->in);
	}
	for (l = 0; l <= maxlun; l++)
	{
		UCHAR cdb[12];
		ULONG res = 0;
		int r, tries;
		PDEVICE_OBJECT d;
		PDOEXT *x;
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = SCSIOP_INQUIRY;
		cdb[1] = (UCHAR) (f->subclass != 6 ? l << 5 : 0);
		cdb[4] = 36;
		memset(inq, 0, sizeof(inq));
		for (tries = 0, r = BOT_ERROR; tries < 3 && r != BOT_OK && r != BOT_GONE; tries++)
			r = bot(f, l, cdb, 6, inq, 36, 1, &res);
		if (r != BOT_OK || (inq[0] & 0xe0))
		{
			logf("kexumass: LUN %u: no INQUIRY answer\n", l);
			continue;
		}
		{
			static LONG count;
			WCHAR name[32], *q = put_s(name, "\\Device\\KexUsbStor");
			LONG n = InterlockedIncrement(&count);
			UNICODE_STRING us;
			int k;
			for (k = 12; k >= 0; k -= 4)
				*q++ = (WCHAR) ("0123456789ABCDEF"[(n >> k) & 15]);
			*q = 0;
			RtlInitUnicodeString(&us, name);
			if (!NT_SUCCESS(IoCreateDevice(g_drv, sizeof(PDOEXT), &us, FILE_DEVICE_MASS_STORAGE, 0, FALSE, &d)))
				continue;
		}
		x = (PDOEXT *) d->DeviceExtension;
		memset(x, 0, sizeof(*x));
		x->kind = EXT_PDO;
		x->fdo = f;
		x->self = d;
		x->lun = l;
		x->type = inq[0] & 0x1f;
		x->removable = (inq[1] & 0x80) != 0;
		memcpy(x->vendor, inq + 8, 8);
		trim(x->vendor, 8);
		memcpy(x->product, inq + 16, 16);
		trim(x->product, 16);
		memcpy(x->rev, inq + 32, 4);
		trim(x->rev, 4);
		InitializeListHead(&x->queue);
		d->Flags |= DO_DIRECT_IO;
		d->Flags &= ~DO_DEVICE_INITIALIZING;
		f->luns[l] = x;
		if (l + 1 > f->nlun)
			f->nlun = l + 1;
		logf("kexumass: LUN %u: type %u%s \"%s %s %s\"\n", l, x->type, x->removable ? " removable" : "", x->vendor,
		     x->product, x->rev);
	}
}

static NTSTATUS NTAPI on_done(PDEVICE_OBJECT d, PIRP irp, PVOID ev)
{
	(void) d;
	(void) irp;
	KeSetEvent((PKEVENT) ev, 0, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS call_down_sync(FDO *f, PIRP irp)
{
	KEVENT ev;
	NTSTATUS st;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(irp);
	IoSetCompletionRoutine(irp, on_done, &ev, TRUE, TRUE, TRUE);
	st = IoCallDriver(f->lower, irp);
	if (st == STATUS_PENDING)
	{
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = irp->IoStatus.Status;
	}
	return st;
}

static void stop_worker(FDO *f)
{
	if (!f->thread)
		return;
	f->stop = 1;
	KeSetEvent(&f->work, 0, FALSE);
	KeWaitForSingleObject(&f->stopped, Executive, KernelMode, FALSE, NULL);
	ZwClose(f->thread);
	f->thread = NULL;
}

static NTSTATUS fdo_pnp(FDO *f, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;
	switch (sl->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
		st = call_down_sync(f, irp);
		logf("kexumass: device %u start: %x\n", f->index, st);
		if (NT_SUCCESS(st))
		{
			f->started = 1;
			st = PsCreateSystemThread(&f->thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, worker, f);
		}
		irp->IoStatus.Status = st;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	case IRP_MN_QUERY_DEVICE_RELATIONS:
		if (sl->Parameters.QueryDeviceRelations.Type == BusRelations && f->started && !f->scanned)
		{
			/* the device is set up here, the first time Windows asks for the
			 * children: after START_DEVICE (Windows 98's USB stack does not
			 * complete requests sent from it), and before NTMAP above us looks
			 * at the children */
			f->scanned = 1;
			if (NT_SUCCESS(configure(f)))
				scan_luns(f);
		}
		if (sl->Parameters.QueryDeviceRelations.Type == BusRelations)
		{
			PDEVICE_RELATIONS r =
			    (PDEVICE_RELATIONS) ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) + MAX_LUN * sizeof(PVOID), TAG);
			int i;
			if (!r)
				break;
			r->Count = 0;
			for (i = 0; i < f->nlun; i++)
				if (f->luns[i] && !f->gone)
				{
					r->Objects[r->Count++] = f->luns[i]->self;
					ObReferenceObject(f->luns[i]->self);
					f->luns[i]->reported = 1;
				}
			irp->IoStatus.Information = (ULONG_PTR) r;
			irp->IoStatus.Status = STATUS_SUCCESS;
		}
		break;
	case IRP_MN_QUERY_STOP_DEVICE:
		irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_UNSUCCESSFUL;
	case IRP_MN_SURPRISE_REMOVAL:
		f->gone = 1;
		irp->IoStatus.Status = STATUS_SUCCESS;
		break;
	case IRP_MN_REMOVE_DEVICE:
	{
		PDEVICE_OBJECT lower = f->lower;
		int i;
		f->gone = 1;
		stop_worker(f);
		for (i = 0; i < f->nlun; i++)
			if (f->luns[i])
			{
				f->luns[i]->removed = 1;
				/* PnP removes the children before the parent: what is left goes now */
				if (!f->luns[i]->reported || f->luns[i]->got_remove)
					IoDeleteDevice(f->luns[i]->self);
			}
		if (f->bounce)
			ExFreePool(f->bounce);
		irp->IoStatus.Status = STATUS_SUCCESS;
		IoSkipCurrentIrpStackLocation(irp);
		st = IoCallDriver(lower, irp);
		IoDetachDevice(lower);
		IoDeleteDevice(f->self);
		return st;
	}
	default:
		break;
	}
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(f->lower, irp);
}

/* ---- dispatch ------------------------------------------------------------------------- */
static NTSTATUS NTAPI d_pnp(PDEVICE_OBJECT d, PIRP irp)
{
	if (*(int *) d->DeviceExtension == EXT_FDO)
		return fdo_pnp((FDO *) d->DeviceExtension, irp);
	return pdo_pnp((PDOEXT *) d->DeviceExtension, irp);
}

static NTSTATUS NTAPI d_power(PDEVICE_OBJECT d, PIRP irp)
{
	PoStartNextPowerIrp(irp);
	if (*(int *) d->DeviceExtension == EXT_FDO)
	{
		IoSkipCurrentIrpStackLocation(irp);
		return PoCallDriver(((FDO *) d->DeviceExtension)->lower, irp);
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

static NTSTATUS NTAPI d_scsi(PDEVICE_OBJECT d, PIRP irp)
{
	if (*(int *) d->DeviceExtension == EXT_FDO)
	{
		PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
		ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
		if (code != IOCTL_INTERNAL_USB_SUBMIT_URB && (code >> 16) == 0x1b)
		{
			PSCSI_REQUEST_BLOCK srb = sl->Parameters.Scsi.Srb;
			logf("kexumass: FDO SRB ioctl %x fn %x lun %u/%u/%u cdb %x\n", code, srb ? srb->Function : 0xff,
			     srb ? srb->PathId : 0, srb ? srb->TargetId : 0, srb ? srb->Lun : 0, srb ? srb->Cdb[0] : 0);
			if (srb)
				srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
			irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			return STATUS_DEVICE_NOT_CONNECTED;
		}
		IoSkipCurrentIrpStackLocation(irp);
		return IoCallDriver(((FDO *) d->DeviceExtension)->lower, irp);
	}
	return pdo_scsi((PDOEXT *) d->DeviceExtension, irp);
}

static NTSTATUS NTAPI d_other(PDEVICE_OBJECT d, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;
	if (*(int *) d->DeviceExtension == EXT_FDO && sl->MajorFunction != IRP_MJ_CREATE && sl->MajorFunction != IRP_MJ_CLOSE)
	{
		IoSkipCurrentIrpStackLocation(irp);
		return IoCallDriver(((FDO *) d->DeviceExtension)->lower, irp);
	}
	if (sl->MajorFunction == IRP_MJ_CREATE || sl->MajorFunction == IRP_MJ_CLOSE || sl->MajorFunction == IRP_MJ_CLEANUP)
		st = STATUS_SUCCESS;
	else
	{
		if (*(int *) d->DeviceExtension == EXT_PDO)
			logf("kexumass: IRP major %x\n", sl->MajorFunction);
		st = sl->MajorFunction == IRP_MJ_SYSTEM_CONTROL ? irp->IoStatus.Status : STATUS_INVALID_DEVICE_REQUEST;
	}
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	PDEVICE_OBJECT d;
	FDO *f;
	NTSTATUS st = IoCreateDevice(drv, sizeof(FDO), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &d);
	if (!NT_SUCCESS(st))
		return st;
	f = (FDO *) d->DeviceExtension;
	memset(f, 0, sizeof(*f));
	f->kind = EXT_FDO;
	f->self = d;
	f->pdo = pdo;
	f->index = (int) InterlockedIncrement(&g_index) - 1;
	f->bounce = (PUCHAR) ExAllocatePoolWithTag(NonPagedPool, BOUNCE, TAG);
	KeInitializeSpinLock(&f->lock);
	KeInitializeEvent(&f->work, SynchronizationEvent, FALSE);
	KeInitializeEvent(&f->stopped, NotificationEvent, FALSE);
	f->lower = IoAttachDeviceToDeviceStack(d, pdo);
	if (!f->lower)
	{
		IoDeleteDevice(d);
		return STATUS_NO_SUCH_DEVICE;
	}
	d->Flags |= DO_DIRECT_IO;
	d->Flags &= ~DO_DEVICE_INITIALIZING;
	logf("kexumass: device %u added\n", f->index);
	return STATUS_SUCCESS;
}

static VOID NTAPI unload(PDRIVER_OBJECT d)
{
	(void) d;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	int i;
	(void) reg;
	g_drv = drv;
	logf("kexumass: KernelExEx USB mass storage driver\n");
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv->MajorFunction[i] = d_other;
	drv->MajorFunction[IRP_MJ_PNP] = d_pnp;
	drv->MajorFunction[IRP_MJ_POWER] = d_power;
	drv->MajorFunction[IRP_MJ_SCSI] = d_scsi;   /* = IRP_MJ_INTERNAL_DEVICE_CONTROL */
	drv->DriverExtension->AddDevice = add_device;
	drv->DriverUnload = unload;
	return STATUS_SUCCESS;
}
