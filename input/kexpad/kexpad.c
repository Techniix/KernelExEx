/*
 *  KernelExEx - kexpad.sys: Xbox 360-protocol USB controllers on Windows 98 SE / ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 *
 *  A HID minidriver, the same kind of driver as Microsoft's hidusb.sys: it
 *  talks to the controller's interrupt endpoints and presents it to
 *  HIDCLASS as a standard HID game pad (x360.c). Windows' own HID game
 *  controller support, DirectInput and the joystick API then see it, and
 *  KernelExEx's XInput sees it through them.
 *
 *  Uses only kernel functions that Windows 98 SE provides
 *  (net/tools/exports/win98se-wdm): no IoAllocateWorkItem, no KeSetTimer,
 *  no MmGetSystemAddressForMdlSafe.
 *
 *  Protocol reference: Linux drivers/input/joystick/xpad.c.
 */
#include <wdm.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include "hidmini.h"
#include "x360.h"

#define KEXPAD_TAG 'dpxK'
#define XFER_SIZE 32

typedef struct
{
	USB_DEVICE_DESCRIPTOR dev;
	USBD_PIPE_HANDLE in_pipe;
	USBD_PIPE_HANDLE out_pipe;
	USHORT in_size;
	int started;
	int removed;
	LONG out_busy;
	unsigned char rumble[2];   /* last strong/weak sent */
} PAD;

typedef struct
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER urb;
	PDEVICE_OBJECT fdo;
	unsigned char buf[XFER_SIZE];
} READ_CTX;

typedef struct
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER urb;
	unsigned char buf[8];
} WRITE_CTX;

static PAD *pad_of(PDEVICE_OBJECT fdo)
{
	return (PAD *) ((PHID_DEVICE_EXTENSION) fdo->DeviceExtension)->MiniDeviceExtension;
}

static PDEVICE_OBJECT lower_of(PDEVICE_OBJECT fdo)
{
	return ((PHID_DEVICE_EXTENSION) fdo->DeviceExtension)->NextDeviceObject;
}

static void zero(void *p, ULONG n)
{
	unsigned char *b = (unsigned char *) p;
	while (n--) *b++ = 0;
}

static void copy(void *d, const void *s, ULONG n)
{
	unsigned char *a = (unsigned char *) d;
	const unsigned char *b = (const unsigned char *) s;
	while (n--) *a++ = *b++;
}

/* ---- synchronous URB submission (PASSIVE_LEVEL only) ------------------ */

static NTSTATUS submit_urb_sync(PDEVICE_OBJECT fdo, PURB urb)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	PIO_STACK_LOCATION sl;
	NTSTATUS st;

	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, lower_of(fdo), NULL, 0, NULL, 0, TRUE,
	                                    &ev, &iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	sl = IoGetNextIrpStackLocation(irp);
	sl->Parameters.Others.Argument1 = urb;
	st = IoCallDriver(lower_of(fdo), irp);
	if (st == STATUS_PENDING)
	{
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = iosb.Status;
	}
	if (NT_SUCCESS(st) && !USBD_SUCCESS(urb->UrbHeader.Status))
		st = STATUS_UNSUCCESSFUL;
	return st;
}

static NTSTATUS get_descriptor(PDEVICE_OBJECT fdo, UCHAR type, PVOID buf, ULONG len, ULONG *got)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
	NTSTATUS st;
	zero(&urb, sizeof(urb));
	UsbBuildGetDescriptorRequest((PURB) &urb, (USHORT) sizeof(urb), type, 0, 0, buf, NULL, len, NULL);
	st = submit_urb_sync(fdo, (PURB) &urb);
	if (got)
		*got = urb.TransferBufferLength;
	return st;
}

/* interrupt OUT, best effort; may be called at DISPATCH_LEVEL */
static NTSTATUS NTAPI write_done(PDEVICE_OBJECT dev, PIRP irp, PVOID context)
{
	PAD *p = (PAD *) context;
	(void) dev;
	ExFreePool(irp->UserIosb);   /* the WRITE_CTX, see send_out */
	IoFreeIrp(irp);
	InterlockedExchange(&p->out_busy, 0);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static void send_out(PDEVICE_OBJECT fdo, const unsigned char *data, ULONG len)
{
	PAD *p = pad_of(fdo);
	PDEVICE_OBJECT lower = lower_of(fdo);
	WRITE_CTX *ctx;
	PIRP irp;
	PIO_STACK_LOCATION sl;

	if (!p->out_pipe || p->removed || len > sizeof(ctx->buf))
		return;
	if (InterlockedExchange(&p->out_busy, 1))
		return;   /* one write in flight is enough for LEDs and rumble */
	ctx = (WRITE_CTX *) ExAllocatePoolWithTag(NonPagedPool, sizeof(*ctx), KEXPAD_TAG);
	irp = ctx ? IoAllocateIrp(lower->StackSize, FALSE) : NULL;
	if (!irp)
	{
		if (ctx)
			ExFreePool(ctx);
		InterlockedExchange(&p->out_busy, 0);
		return;
	}
	zero(ctx, sizeof(*ctx));
	copy(ctx->buf, data, len);
	UsbBuildInterruptOrBulkTransferRequest((PURB) &ctx->urb, (USHORT) sizeof(ctx->urb), p->out_pipe, ctx->buf, NULL,
	                                       len, USBD_TRANSFER_DIRECTION_OUT, NULL);
	irp->UserIosb = (PIO_STATUS_BLOCK) ctx;   /* carried to write_done */
	sl = IoGetNextIrpStackLocation(irp);
	sl->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	sl->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
	sl->Parameters.Others.Argument1 = &ctx->urb;
	IoSetCompletionRoutine(irp, write_done, p, TRUE, TRUE, TRUE);
	IoCallDriver(lower, irp);
}

/* ---- device start ----------------------------------------------------- */

static NTSTATUS select_config(PDEVICE_OBJECT fdo)
{
	PAD *p = pad_of(fdo);
	USB_CONFIGURATION_DESCRIPTOR head;
	PUSB_CONFIGURATION_DESCRIPTOR cfg;
	PUSB_INTERFACE_DESCRIPTOR ifd;
	USBD_INTERFACE_LIST_ENTRY list[2];
	PURB urb;
	NTSTATUS st;
	ULONG got, i;

	st = get_descriptor(fdo, USB_DEVICE_DESCRIPTOR_TYPE, &p->dev, sizeof(p->dev), &got);
	if (!NT_SUCCESS(st))
		return st;
	st = get_descriptor(fdo, USB_CONFIGURATION_DESCRIPTOR_TYPE, &head, sizeof(head), &got);
	if (!NT_SUCCESS(st) || head.wTotalLength < sizeof(head))
		return NT_SUCCESS(st) ? STATUS_DEVICE_DATA_ERROR : st;
	cfg = (PUSB_CONFIGURATION_DESCRIPTOR) ExAllocatePoolWithTag(NonPagedPool, head.wTotalLength, KEXPAD_TAG);
	if (!cfg)
		return STATUS_INSUFFICIENT_RESOURCES;
	st = get_descriptor(fdo, USB_CONFIGURATION_DESCRIPTOR_TYPE, cfg, head.wTotalLength, &got);
	if (!NT_SUCCESS(st))
	{
		ExFreePool(cfg);
		return st;
	}
	/* the game pad interface: vendor class 0xFF, subclass 0x5D, protocol 0x01 */
	ifd = USBD_ParseConfigurationDescriptorEx(cfg, cfg, -1, -1, 0xFF, 0x5D, 0x01);
	if (!ifd)
		ifd = USBD_ParseConfigurationDescriptorEx(cfg, cfg, 0, 0, -1, -1, -1);
	if (!ifd)
	{
		ExFreePool(cfg);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}
	list[0].InterfaceDescriptor = ifd;
	list[0].Interface = NULL;
	list[1].InterfaceDescriptor = NULL;
	list[1].Interface = NULL;
	urb = USBD_CreateConfigurationRequestEx(cfg, list);
	if (!urb)
	{
		ExFreePool(cfg);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	st = submit_urb_sync(fdo, urb);
	if (NT_SUCCESS(st))
	{
		PUSBD_INTERFACE_INFORMATION info = list[0].Interface;
		for (i = 0; i < info->NumberOfPipes; i++)
		{
			PUSBD_PIPE_INFORMATION pi = &info->Pipes[i];
			if (pi->PipeType != UsbdPipeTypeInterrupt)
				continue;
			if (USB_ENDPOINT_DIRECTION_IN(pi->EndpointAddress) && !p->in_pipe)
			{
				p->in_pipe = pi->PipeHandle;
				p->in_size = (USHORT) (pi->MaximumPacketSize < XFER_SIZE ? XFER_SIZE : pi->MaximumPacketSize);
				if (p->in_size > XFER_SIZE)
					p->in_size = XFER_SIZE;
			}
			else if (!USB_ENDPOINT_DIRECTION_IN(pi->EndpointAddress) && !p->out_pipe)
				p->out_pipe = pi->PipeHandle;
		}
		if (!p->in_pipe)
			st = STATUS_DEVICE_CONFIGURATION_ERROR;
	}
	ExFreePool(urb);
	ExFreePool(cfg);
	return st;
}

static NTSTATUS NTAPI start_done(PDEVICE_OBJECT dev, PIRP irp, PVOID context)
{
	(void) dev;
	(void) irp;
	KeSetEvent((PKEVENT) context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS NTAPI pnp(PDEVICE_OBJECT fdo, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	PAD *p = pad_of(fdo);
	NTSTATUS st;

	switch (sl->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
	{
		KEVENT ev;
		KeInitializeEvent(&ev, NotificationEvent, FALSE);
		IoCopyCurrentIrpStackLocationToNext(irp);
		IoSetCompletionRoutine(irp, start_done, &ev, TRUE, TRUE, TRUE);
		st = IoCallDriver(lower_of(fdo), irp);
		if (st == STATUS_PENDING)
			KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = irp->IoStatus.Status;
		if (NT_SUCCESS(st))
		{
			st = select_config(fdo);
			if (NT_SUCCESS(st))
			{
				unsigned char led[3];
				p->started = 1;
				x360_led_packet(1, led);
				send_out(fdo, led, sizeof(led));
			}
		}
		irp->IoStatus.Status = st;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return st;
	}
	case IRP_MN_REMOVE_DEVICE:
	case IRP_MN_SURPRISE_REMOVAL:
		p->removed = 1;
		p->started = 0;
		/* fall through */
	case IRP_MN_STOP_DEVICE:
		if (sl->MinorFunction == IRP_MN_STOP_DEVICE)
			p->started = 0;
		/* fall through */
	default:
		IoSkipCurrentIrpStackLocation(irp);
		return IoCallDriver(lower_of(fdo), irp);
	}
}

/* ---- HID requests ----------------------------------------------------- */

static NTSTATUS NTAPI read_done(PDEVICE_OBJECT dev, PIRP irp, PVOID context);

static NTSTATUS submit_read(PDEVICE_OBJECT fdo, PIRP irp, READ_CTX *ctx)
{
	PAD *p = pad_of(fdo);
	PIO_STACK_LOCATION sl;
	zero(&ctx->urb, sizeof(ctx->urb));
	UsbBuildInterruptOrBulkTransferRequest((PURB) &ctx->urb, (USHORT) sizeof(ctx->urb), p->in_pipe, ctx->buf, NULL,
	                                       p->in_size, USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
	sl = IoGetNextIrpStackLocation(irp);
	sl->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	sl->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
	sl->Parameters.Others.Argument1 = &ctx->urb;
	IoSetCompletionRoutine(irp, read_done, ctx, TRUE, TRUE, TRUE);
	return IoCallDriver(lower_of(fdo), irp);
}

static NTSTATUS NTAPI read_done(PDEVICE_OBJECT dev, PIRP irp, PVOID context)
{
	READ_CTX *ctx = (READ_CTX *) context;
	PAD *p = pad_of(ctx->fdo);
	PIO_STACK_LOCATION sl;
	(void) dev;

	/* internal_ioctl marked the IRP pending and returned STATUS_PENDING */
	if (NT_SUCCESS(irp->IoStatus.Status) && !p->removed)
	{
		unsigned char report[X360_REPORT_SIZE];
		if (!x360_translate(ctx->buf, ctx->urb.TransferBufferLength, report))
		{
			/* LED, rumble or link status packet: read again with the same IRP */
			submit_read(ctx->fdo, irp, ctx);
			return STATUS_MORE_PROCESSING_REQUIRED;
		}
		sl = IoGetCurrentIrpStackLocation(irp);
		if (sl->Parameters.DeviceIoControl.OutputBufferLength >= X360_REPORT_SIZE)
		{
			copy(irp->UserBuffer, report, X360_REPORT_SIZE);
			irp->IoStatus.Information = X360_REPORT_SIZE;
		}
		else
		{
			irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
			irp->IoStatus.Information = 0;
		}
	}
	else if (NT_SUCCESS(irp->IoStatus.Status))
		irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
	ExFreePool(ctx);
	return STATUS_SUCCESS;
}

/* output report: rumble, 2 bytes (strong, weak) */
static NTSTATUS write_report(PDEVICE_OBJECT fdo, PIRP irp)
{
	/* HIDCLASS passes a HID_XFER_PACKET: { PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; } */
	struct { PUCHAR buf; ULONG len; UCHAR id; } *xfer = irp->UserBuffer;
	PAD *p = pad_of(fdo);
	if (xfer && xfer->buf && xfer->len >= 2)
	{
		unsigned char pkt[8];
		p->rumble[0] = xfer->buf[xfer->len - 2];
		p->rumble[1] = xfer->buf[xfer->len - 1];
		x360_rumble_packet(p->rumble[0], p->rumble[1], pkt);
		send_out(fdo, pkt, sizeof(pkt));
		irp->IoStatus.Information = xfer->len;
		return STATUS_SUCCESS;
	}
	return STATUS_INVALID_PARAMETER;
}

static NTSTATUS NTAPI internal_ioctl(PDEVICE_OBJECT fdo, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	PAD *p = pad_of(fdo);
	ULONG outlen = sl->Parameters.DeviceIoControl.OutputBufferLength;
	NTSTATUS st = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;

	switch (sl->Parameters.DeviceIoControl.IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
	{
		HID_DESCRIPTOR d;
		if (outlen < sizeof(d))
		{
			st = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		zero(&d, sizeof(d));
		d.bLength = sizeof(d);
		d.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
		d.bcdHID = 0x0110;
		d.bNumDescriptors = 1;
		d.DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
		d.DescriptorList[0].wReportLength = (USHORT) x360_report_descriptor_size;
		copy(irp->UserBuffer, &d, sizeof(d));
		irp->IoStatus.Information = sizeof(d);
		break;
	}
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		if (outlen < x360_report_descriptor_size)
		{
			st = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		copy(irp->UserBuffer, x360_report_descriptor, x360_report_descriptor_size);
		irp->IoStatus.Information = x360_report_descriptor_size;
		break;
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
	{
		PHID_DEVICE_ATTRIBUTES a = (PHID_DEVICE_ATTRIBUTES) irp->UserBuffer;
		if (outlen < sizeof(*a))
		{
			st = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		zero(a, sizeof(*a));
		a->Size = sizeof(*a);
		a->VendorID = p->dev.idVendor;
		a->ProductID = p->dev.idProduct;
		a->VersionNumber = p->dev.bcdDevice;
		irp->IoStatus.Information = sizeof(*a);
		break;
	}
	case IOCTL_HID_READ_REPORT:
	{
		READ_CTX *ctx;
		if (!p->started || !p->in_pipe)
		{
			st = STATUS_DEVICE_NOT_READY;
			break;
		}
		ctx = (READ_CTX *) ExAllocatePoolWithTag(NonPagedPool, sizeof(*ctx), KEXPAD_TAG);
		if (!ctx)
		{
			st = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		ctx->fdo = fdo;
		/* always pending: read_done may resubmit the same IRP before it completes */
		IoMarkIrpPending(irp);
		submit_read(fdo, irp, ctx);
		return STATUS_PENDING;
	}
	case IOCTL_HID_WRITE_REPORT:
		st = write_report(fdo, irp);
		break;
	case IOCTL_HID_ACTIVATE_DEVICE:
	case IOCTL_HID_DEACTIVATE_DEVICE:
		break;
	default:
		st = STATUS_NOT_SUPPORTED;   /* GET_STRING and others: HIDCLASS copes */
		break;
	}
	irp->IoStatus.Status = st;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static NTSTATUS NTAPI power(PDEVICE_OBJECT fdo, PIRP irp)
{
	PoStartNextPowerIrp(irp);
	IoSkipCurrentIrpStackLocation(irp);
	return PoCallDriver(lower_of(fdo), irp);
}

static NTSTATUS NTAPI pass_down(PDEVICE_OBJECT fdo, PIRP irp)
{
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(lower_of(fdo), irp);
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT fdo)
{
	PAD *p = pad_of(fdo);
	(void) drv;
	zero(p, sizeof(*p));
	fdo->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}

static VOID NTAPI unload(PDRIVER_OBJECT drv)
{
	(void) drv;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regpath)
{
	HID_MINIDRIVER_REGISTRATION reg;
	ULONG i;
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv->MajorFunction[i] = pass_down;
	drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = internal_ioctl;
	drv->MajorFunction[IRP_MJ_PNP] = pnp;
	drv->MajorFunction[IRP_MJ_POWER] = power;
	drv->DriverExtension->AddDevice = add_device;
	drv->DriverUnload = unload;

	zero(&reg, sizeof(reg));
	reg.Revision = HID_REVISION;
	reg.DriverObject = drv;
	reg.RegistryPath = regpath;
	reg.DeviceExtensionSize = sizeof(PAD);
	reg.DevicesArePolled = FALSE;
	return HidRegisterMinidriver(&reg);
}

/* the compiler emits memcpy for structure copies; a driver has no CRT */
void *__cdecl memcpy(void *d, const void *s, size_t n)
{
	copy(d, s, (ULONG) n);
	return d;
}
