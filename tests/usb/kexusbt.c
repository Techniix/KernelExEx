/*
 *  KernelExEx - kexusbt.sys: a test driver for USB host controller drivers.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Binds to a USB mass storage device (bulk-only transport, SCSI) and, from
 *  a system thread, runs INQUIRY, READ CAPACITY, a timed 4 MB read, and a
 *  write/read-back check of 64 KB at LBA 1000 (TEST IMAGES ONLY: it
 *  overwrites those sectors). Results go to the debug console (port 0xE9).
 *  Not shipped: for QEMU tests of kexehci (tests/usb/run_kexusbt.sh).
 */
#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <stdarg.h>
#include "kex_usbbusif.h"

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

static void out_c(char c)
{
	__asm__ __volatile__("outb %0, $0xe9" : : "a"(c));
}

static void out_n(unsigned long v, unsigned base)
{
	char b[12];
	int i = 0;
	do
	{
		unsigned d = (unsigned) (v % base);
		b[i++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v);
	while (i--)
		out_c(b[i]);
}

static void logf(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	for (; *f; f++)
	{
		if (*f != '%')
		{
			out_c(*f);
			continue;
		}
		f++;
		if (*f == 's')
		{
			const char *p = va_arg(ap, const char *);
			while (*p)
				out_c(*p++);
		}
		else if (*f == 'x')
			out_n(va_arg(ap, unsigned long), 16);
		else
			out_n(va_arg(ap, unsigned long), 10);
	}
	va_end(ap);
}

typedef struct
{
	PDEVICE_OBJECT self, lower;
	USBD_PIPE_HANDLE in, out;
	HANDLE thread;
	ULONG tag;
} EXT;

static NTSTATUS urb_sync(EXT *e, PURB urb)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS st;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, e->lower, NULL, 0, NULL, 0, TRUE, &ev, &iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;
	st = IoCallDriver(e->lower, irp);
	if (st == STATUS_PENDING)
	{
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = iosb.Status;
	}
	return st;
}

static NTSTATUS bulk(EXT *e, USBD_PIPE_HANDLE p, void *buf, ULONG len, ULONG *got, int in)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER u;
	NTSTATUS st;
	memset(&u, 0, sizeof(u));
	u.Hdr.Length = sizeof(u);
	u.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
	u.PipeHandle = p;
	u.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
	u.TransferBuffer = buf;
	u.TransferBufferLength = len;
	st = urb_sync(e, (PURB) &u);
	if (got)
		*got = u.TransferBufferLength;
	if (!NT_SUCCESS(st))
		logf("kexusbt: bulk %s %u: %x / %x\n", in ? "in" : "out", len, st, u.Hdr.Status);
	return st;
}

/* one SCSI command over bulk-only transport */
static int scsi(EXT *e, const UCHAR *cdb, int cdblen, void *data, ULONG len, int in)
{
	UCHAR cbw[31], csw[13];
	ULONG got = 0;
	memset(cbw, 0, sizeof(cbw));
	cbw[0] = 'U';
	cbw[1] = 'S';
	cbw[2] = 'B';
	cbw[3] = 'C';
	*(PULONG) (cbw + 4) = ++e->tag;
	*(PULONG) (cbw + 8) = len;
	cbw[12] = in ? 0x80 : 0;
	cbw[14] = (UCHAR) cdblen;
	memcpy(cbw + 15, cdb, cdblen);
	if (!NT_SUCCESS(bulk(e, e->out, cbw, 31, NULL, 0)))
		return -1;
	if (len && !NT_SUCCESS(bulk(e, in ? e->in : e->out, data, len, &got, in)))
		return -1;
	if (!NT_SUCCESS(bulk(e, e->in, csw, 13, &got, 1)) || got != 13 || csw[12])
	{
		logf("kexusbt: CSW status %u (%u bytes)\n", csw[12], got);
		return -1;
	}
	return 0;
}

static ULONG ms(void)
{
	return (ULONG) (KeQueryInterruptTime() / 10000);
}

static VOID NTAPI test_thread(PVOID ctx)
{
	EXT *e = (EXT *) ctx;
	UCHAR *buf = (UCHAR *) ExAllocatePoolWithTag(NonPagedPool, 0x20000, 'tsuK'), *cfg;
	struct _URB_CONTROL_DESCRIPTOR_REQUEST dr;
	PURB sel;
	ULONG sz, i, t0, t1, cap = 0;
	UCHAR cdb[10];
	PUSBD_INTERFACE_INFORMATION ii;
	KEX_USB_BUS_INTERFACE_USBDI_V1 bi;
	if (!buf)
		goto out;
	cfg = buf + 0x10000;
	memset(&dr, 0, sizeof(dr));
	dr.Hdr.Length = sizeof(dr);
	dr.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
	dr.DescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
	dr.TransferBuffer = cfg;
	dr.TransferBufferLength = 255;
	if (!NT_SUCCESS(urb_sync(e, (PURB) &dr)))
	{
		logf("kexusbt: no configuration descriptor\n");
		goto out;
	}
	logf("kexusbt: configuration descriptor, %u bytes\n", dr.TransferBufferLength);
	sz = sizeof(struct _URB_SELECT_CONFIGURATION) + sizeof(USBD_PIPE_INFORMATION);
	sel = (PURB) ExAllocatePoolWithTag(NonPagedPool, sz, 'tsuK');
	memset(sel, 0, sz);
	sel->UrbHeader.Length = (USHORT) sz;
	sel->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
	sel->UrbSelectConfiguration.ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR) cfg;
	ii = &sel->UrbSelectConfiguration.Interface;
	ii->Length = (USHORT) (sizeof(USBD_INTERFACE_INFORMATION) + sizeof(USBD_PIPE_INFORMATION));
	ii->NumberOfPipes = 2;
	ii->Pipes[0].MaximumTransferSize = ii->Pipes[1].MaximumTransferSize = 0x10000;
	if (!NT_SUCCESS(urb_sync(e, sel)))
	{
		logf("kexusbt: SELECT_CONFIGURATION failed %x\n", sel->UrbHeader.Status);
		goto out;
	}
	for (i = 0; i < ii->NumberOfPipes; i++)
	{
		logf("kexusbt: pipe %x type %u max %u\n", ii->Pipes[i].EndpointAddress, ii->Pipes[i].PipeType,
		     ii->Pipes[i].MaximumPacketSize);
		if (ii->Pipes[i].EndpointAddress & 0x80)
			e->in = ii->Pipes[i].PipeHandle;
		else
			e->out = ii->Pipes[i].PipeHandle;
	}
	/* the bus interface */
	{
		KEVENT ev;
		IO_STATUS_BLOCK iosb;
		PIRP irp;
		PIO_STACK_LOCATION sl;
		memset(&bi, 0, sizeof(bi));
		KeInitializeEvent(&ev, NotificationEvent, FALSE);
		irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, e->lower, NULL, 0, NULL, &ev, &iosb);
		if (irp)
		{
			NTSTATUS st;
			irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
			sl = IoGetNextIrpStackLocation(irp);
			sl->MinorFunction = IRP_MN_QUERY_INTERFACE;
			sl->Parameters.QueryInterface.InterfaceType = &KEX_USB_BUS_INTERFACE_USBDI_GUID;
			sl->Parameters.QueryInterface.Size = KEX_USBDI_V1_SIZE;
			sl->Parameters.QueryInterface.Version = 1;
			sl->Parameters.QueryInterface.Interface = (PINTERFACE) &bi;
			st = IoCallDriver(e->lower, irp);
			if (st == STATUS_PENDING)
			{
				KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
				st = iosb.Status;
			}
			if (NT_SUCCESS(st) && bi.Version >= 1)
				logf("kexusbt: bus interface v%u, high speed: %u\n", bi.Version, bi.IsDeviceHighSpeed(bi.BusContext));
			else
				logf("kexusbt: no bus interface (%x)\n", st);
		}
	}
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x12;   /* INQUIRY */
	cdb[4] = 36;
	if (scsi(e, cdb, 6, buf, 36, 1) == 0)
	{
		buf[32] = 0;
		logf("kexusbt: INQUIRY: %s\n", (char *) buf + 8);
	}
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x25;   /* READ CAPACITY(10) */
	if (scsi(e, cdb, 10, buf, 8, 1) == 0)
	{
		cap = (ULONG) buf[0] << 24 | (ULONG) buf[1] << 16 | (ULONG) buf[2] << 8 | buf[3];
		logf("kexusbt: capacity %u sectors\n", cap + 1);
	}
	/* timed read: 4 MB in 64 KB commands */
	t0 = ms();
	for (i = 0; i < 64; i++)
	{
		ULONG lba = i * 128;
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = 0x28;
		cdb[2] = (UCHAR) (lba >> 24);
		cdb[3] = (UCHAR) (lba >> 16);
		cdb[4] = (UCHAR) (lba >> 8);
		cdb[5] = (UCHAR) lba;
		cdb[8] = 128;
		if (scsi(e, cdb, 10, buf, 0x10000, 1))
			break;
	}
	t1 = ms();
	logf("kexusbt: read %u KB in %u ms\n", i * 64, t1 - t0);
	/* write a pattern at LBA 1000 and read it back */
	for (i = 0; i < 0x10000; i++)
		buf[i] = (UCHAR) (i * 7 + (i >> 9));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x2a;
	cdb[4] = 1000 >> 8;
	cdb[5] = 1000 & 0xff;
	cdb[8] = 128;
	if (scsi(e, cdb, 10, buf, 0x10000, 0) == 0)
	{
		memset(buf, 0, 0x10000);
		cdb[0] = 0x28;
		if (scsi(e, cdb, 10, buf, 0x10000, 1) == 0)
		{
			ULONG bad = 0;
			for (i = 0; i < 0x10000; i++)
				if (buf[i] != (UCHAR) (i * 7 + (i >> 9)))
					bad++;
			logf("kexusbt: write/read-back 64 KB: %s\n", bad ? "MISMATCH" : "OK");
		}
	}
	/* a short IN transfer: REQUEST SENSE asks 18, many devices send fewer */
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x03;
	cdb[4] = 18;
	logf("kexusbt: REQUEST SENSE: %s\n", scsi(e, cdb, 6, buf, 18, 1) == 0 ? "OK" : "failed");
	logf("kexusbt: done\n");
out:
	PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS NTAPI on_done(PDEVICE_OBJECT d, PIRP irp, PVOID ev)
{
	(void) d;
	(void) irp;
	KeSetEvent((PKEVENT) ev, 0, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS NTAPI d_pnp(PDEVICE_OBJECT dobj, PIRP irp)
{
	EXT *e = (EXT *) dobj->DeviceExtension;
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	if (sl->MinorFunction == IRP_MN_START_DEVICE)
	{
		KEVENT ev;
		NTSTATUS st;
		KeInitializeEvent(&ev, NotificationEvent, FALSE);
		IoCopyCurrentIrpStackLocationToNext(irp);
		IoSetCompletionRoutine(irp, on_done, &ev, TRUE, TRUE, TRUE);
		st = IoCallDriver(e->lower, irp);
		if (st == STATUS_PENDING)
			KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = irp->IoStatus.Status;
		if (NT_SUCCESS(st))
			PsCreateSystemThread(&e->thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, test_thread, e);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	}
	if (sl->MinorFunction == IRP_MN_REMOVE_DEVICE)
	{
		PDEVICE_OBJECT lower = e->lower;
		NTSTATUS st;
		IoSkipCurrentIrpStackLocation(irp);
		st = IoCallDriver(lower, irp);
		IoDetachDevice(lower);
		IoDeleteDevice(dobj);
		return st;
	}
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(e->lower, irp);
}

static NTSTATUS NTAPI d_pass(PDEVICE_OBJECT dobj, PIRP irp)
{
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(((EXT *) dobj->DeviceExtension)->lower, irp);
}

static NTSTATUS NTAPI d_power(PDEVICE_OBJECT dobj, PIRP irp)
{
	PoStartNextPowerIrp(irp);
	IoSkipCurrentIrpStackLocation(irp);
	return PoCallDriver(((EXT *) dobj->DeviceExtension)->lower, irp);
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	PDEVICE_OBJECT fdo;
	EXT *e;
	NTSTATUS st = IoCreateDevice(drv, sizeof(EXT), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
	if (!NT_SUCCESS(st))
		return st;
	e = (EXT *) fdo->DeviceExtension;
	memset(e, 0, sizeof(*e));
	e->self = fdo;
	e->lower = IoAttachDeviceToDeviceStack(fdo, pdo);
	fdo->Flags &= ~DO_DEVICE_INITIALIZING;
	logf("kexusbt: attached\n");
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
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv->MajorFunction[i] = d_pass;
	drv->MajorFunction[IRP_MJ_PNP] = d_pnp;
	drv->MajorFunction[IRP_MJ_POWER] = d_power;
	drv->DriverExtension->AddDevice = add_device;
	drv->DriverUnload = unload;
	return STATUS_SUCCESS;
}
