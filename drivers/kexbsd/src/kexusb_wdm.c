/*
 *  KernelExEx - kexbsd.sys: USB below the BSD personalities, on the
 *  Windows 98 SE USB stack, and worker threads.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Only Microsoft's USB headers here; the OpenBSD side is obsd_usb.c.
 */
#define KEXBSD_BUILD
#include "kexbsd/kexusb.h"
#include <usb.h>
#include <usbdlib.h>   /* macros only: no usbd.sys imports, see build_config_request */
#include <usbioctl.h>

#define TAG 'busK'

struct kexusb_dev
{
	PDEVICE_OBJECT lower;
	USBD_CONFIGURATION_HANDLE config;
};

struct kexusb_xfer
{
	kexusb_dev *dev;
	PIRP irp;
	struct _URB_BULK_OR_INTERRUPT_TRANSFER urb;
	kexusb_done_fn done;
	void *ctx;
	int short_ok;
	volatile LONG busy;
};

static void *zalloc(SIZE_T n)
{
	void *p = ExAllocatePoolWithTag(NonPagedPool, n, TAG);
	if (p)
		RtlZeroMemory(p, n);
	return p;
}

static int map_status(NTSTATUS st, USBD_STATUS us)
{
	if (st == STATUS_CANCELLED || us == USBD_STATUS_CANCELED)
		return KEXUSB_CANCELLED;
	if (us == USBD_STATUS_STALL_PID || us == USBD_STATUS_ENDPOINT_HALTED)
		return KEXUSB_STALLED;
	if (us == USBD_STATUS_DEV_NOT_RESPONDING)
		return KEXUSB_TIMEOUT;
	if (NT_SUCCESS(st) && USBD_SUCCESS(us))
		return KEXUSB_OK;
	return KEXUSB_IOERROR;
}

/* PASSIVE_LEVEL only */
static int submit_sync(kexusb_dev *d, PURB urb)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	PIO_STACK_LOCATION sl;
	NTSTATUS st;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, d->lower, NULL, 0, NULL, 0, TRUE, &ev, &iosb);
	if (!irp)
		return KEXUSB_NOMEM;
	sl = IoGetNextIrpStackLocation(irp);
	sl->Parameters.Others.Argument1 = urb;
	st = IoCallDriver(d->lower, irp);
	if (st == STATUS_PENDING)
	{
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = iosb.Status;
	}
	return map_status(st, urb->UrbHeader.Status);
}

kexusb_dev *kexusb_open(PDEVICE_OBJECT lower)
{
	kexusb_dev *d = (kexusb_dev *) zalloc(sizeof(*d));
	if (d)
		d->lower = lower;
	return d;
}

void kexusb_close(kexusb_dev *d)
{
	if (d)
		ExFreePool(d);
}

int kexusb_get_descriptor(kexusb_dev *d, uint8_t type, uint8_t index, uint16_t langid, void *buf, unsigned int len,
                          unsigned int *got)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
	int r;
	RtlZeroMemory(&urb, sizeof(urb));
	UsbBuildGetDescriptorRequest((PURB) &urb, (USHORT) sizeof(urb), type, index, langid, buf, NULL, len, NULL);
	r = submit_sync(d, (PURB) &urb);
	if (got)
		*got = urb.TransferBufferLength;
	return r;
}

static void copy_pipes(PUSBD_INTERFACE_INFORMATION info, struct kexusb_iface_pipes *o)
{
	ULONG i;
	o->ifno = info->InterfaceNumber;
	o->alt = info->AlternateSetting;
	o->npipes = 0;
	for (i = 0; i < info->NumberOfPipes && i < KEXUSB_MAX_PIPES; i++)
	{
		PUSBD_PIPE_INFORMATION p = &info->Pipes[i];
		struct kexusb_pipe_info *q = &o->pipes[o->npipes++];
		q->ep = p->EndpointAddress;
		q->type = (uint8_t) p->PipeType;   /* UsbdPipeType* has the USB numbering */
		q->maxpkt = p->MaximumPacketSize;
		q->handle = p->PipeHandle;
	}
}

#define XFER_MAX (64 * 1024)

/* usbd.sys's USBD_ParseConfigurationDescriptorEx and
 * USBD_CreateConfigurationRequestEx, done here so kexbsd.sys does not
 * import usbd.sys (it must load on machines without USB, for kexem) */
static PUSB_INTERFACE_DESCRIPTOR find_iface(PUSB_CONFIGURATION_DESCRIPTOR cfg, int ifno, int alt)
{
	PUCHAR p = (PUCHAR) cfg, end = p + cfg->wTotalLength;
	while (p + 2 <= end && p[0] >= 2)
	{
		if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE && p + sizeof(USB_INTERFACE_DESCRIPTOR) <= end)
		{
			PUSB_INTERFACE_DESCRIPTOR d = (PUSB_INTERFACE_DESCRIPTOR) p;
			if ((ifno < 0 || d->bInterfaceNumber == ifno) && (alt < 0 || d->bAlternateSetting == alt))
				return d;
		}
		p += p[0];
	}
	return NULL;
}

static PURB build_config_request(PUSB_CONFIGURATION_DESCRIPTOR cfg, PUSB_INTERFACE_DESCRIPTOR *ifs, int n,
                                 PUSBD_INTERFACE_INFORMATION *infos)
{
	ULONG size = sizeof(struct _URB_SELECT_CONFIGURATION) - sizeof(USBD_INTERFACE_INFORMATION);
	PURB urb;
	PUCHAR q;
	int i;
	ULONG j;
	for (i = 0; i < n; i++)
		size += GET_USBD_INTERFACE_SIZE(ifs[i]->bNumEndpoints);
	urb = (PURB) zalloc(size);
	if (!urb)
		return NULL;
	urb->UrbHeader.Length = (USHORT) size;
	urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
	urb->UrbSelectConfiguration.ConfigurationDescriptor = cfg;
	q = (PUCHAR) &urb->UrbSelectConfiguration.Interface;
	for (i = 0; i < n; i++)
	{
		PUSBD_INTERFACE_INFORMATION info = (PUSBD_INTERFACE_INFORMATION) q;
		info->Length = (USHORT) GET_USBD_INTERFACE_SIZE(ifs[i]->bNumEndpoints);
		info->InterfaceNumber = ifs[i]->bInterfaceNumber;
		info->AlternateSetting = ifs[i]->bAlternateSetting;
		info->NumberOfPipes = ifs[i]->bNumEndpoints;
		for (j = 0; j < info->NumberOfPipes; j++)
			info->Pipes[j].MaximumTransferSize = XFER_MAX;
		infos[i] = info;
		q += info->Length;
	}
	return urb;
}

int kexusb_select_config(kexusb_dev *d, const void *cdesc, struct kexusb_iface_pipes *out, int max, int *nout)
{
	PUSB_CONFIGURATION_DESCRIPTOR cfg = (PUSB_CONFIGURATION_DESCRIPTOR) cdesc;
	PUSB_INTERFACE_DESCRIPTOR ifs[KEXUSB_MAX_PIPES];
	PUSBD_INTERFACE_INFORMATION infos[KEXUSB_MAX_PIPES];
	PURB urb;
	int n = 0, i, r;
	/* alternate setting 0 of every interface */
	PUCHAR p = (PUCHAR) cfg, end = p + cfg->wTotalLength;
	while (p + 2 <= end && p[0] >= 2 && n < KEXUSB_MAX_PIPES)
	{
		if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE && ((PUSB_INTERFACE_DESCRIPTOR) p)->bAlternateSetting == 0)
			ifs[n++] = (PUSB_INTERFACE_DESCRIPTOR) p;
		p += p[0];
	}
	urb = build_config_request(cfg, ifs, n, infos);
	if (!urb)
		return KEXUSB_NOMEM;
	r = submit_sync(d, urb);
	if (r == KEXUSB_OK)
	{
		d->config = urb->UrbSelectConfiguration.ConfigurationHandle;
		for (i = 0; i < n && i < max; i++)
			copy_pipes(infos[i], &out[i]);
		*nout = i;
	}
	ExFreePool(urb);
	return r;
}

int kexusb_select_interface(kexusb_dev *d, const void *cdesc, int ifno, int alt, struct kexusb_iface_pipes *out)
{
	PUSB_CONFIGURATION_DESCRIPTOR cfg = (PUSB_CONFIGURATION_DESCRIPTOR) cdesc;
	PUSB_INTERFACE_DESCRIPTOR ifd = find_iface(cfg, ifno, alt);
	USHORT size;
	PURB urb;
	PUSBD_INTERFACE_INFORMATION info;
	int r;
	ULONG j;
	if (!ifd)
		return KEXUSB_INVAL;
	size = (USHORT) GET_SELECT_INTERFACE_REQUEST_SIZE(ifd->bNumEndpoints);
	urb = (PURB) zalloc(size);
	if (!urb)
		return KEXUSB_NOMEM;
	urb->UrbHeader.Length = size;
	urb->UrbHeader.Function = URB_FUNCTION_SELECT_INTERFACE;
	urb->UrbSelectInterface.ConfigurationHandle = d->config;
	info = &urb->UrbSelectInterface.Interface;
	info->Length = (USHORT) GET_USBD_INTERFACE_SIZE(ifd->bNumEndpoints);
	info->InterfaceNumber = (UCHAR) ifno;
	info->AlternateSetting = (UCHAR) alt;
	info->NumberOfPipes = ifd->bNumEndpoints;
	for (j = 0; j < ifd->bNumEndpoints; j++)
		info->Pipes[j].MaximumTransferSize = XFER_MAX;
	r = submit_sync(d, urb);
	if (r == KEXUSB_OK)
		copy_pipes(info, out);
	ExFreePool(urb);
	return r;
}

int kexusb_control(kexusb_dev *d, uint8_t rt, uint8_t req, uint16_t value, uint16_t index, void *data,
                   unsigned int len, unsigned int *actlen)
{
	union
	{
		struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST vc;
		struct _URB_CONTROL_FEATURE_REQUEST feat;
		struct _URB_CONTROL_GET_STATUS_REQUEST stat;
		struct _URB_CONTROL_DESCRIPTOR_REQUEST desc;
	} u;
	int type = rt & 0x60, recip = rt & 0x1f, in = rt & 0x80;
	int r;
	unsigned int got = 0;
	RtlZeroMemory(&u, sizeof(u));
	if (type == 0x00)   /* standard */
	{
		static const USHORT clear_fn[] = { URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE, URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE,
		                                   URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT, URB_FUNCTION_CLEAR_FEATURE_TO_OTHER };
		static const USHORT set_fn[] = { URB_FUNCTION_SET_FEATURE_TO_DEVICE, URB_FUNCTION_SET_FEATURE_TO_INTERFACE,
		                                 URB_FUNCTION_SET_FEATURE_TO_ENDPOINT, URB_FUNCTION_SET_FEATURE_TO_OTHER };
		static const USHORT stat_fn[] = { URB_FUNCTION_GET_STATUS_FROM_DEVICE, URB_FUNCTION_GET_STATUS_FROM_INTERFACE,
		                                  URB_FUNCTION_GET_STATUS_FROM_ENDPOINT, URB_FUNCTION_GET_STATUS_FROM_OTHER };
		if (recip > 3)
			return KEXUSB_INVAL;
		switch (req)
		{
		case 0x00:   /* GET_STATUS */
			u.stat.Hdr.Length = sizeof(u.stat);
			u.stat.Hdr.Function = stat_fn[recip];
			u.stat.TransferBuffer = data;
			u.stat.TransferBufferLength = len;
			u.stat.Index = index;
			r = submit_sync(d, (PURB) &u);
			got = u.stat.TransferBufferLength;
			break;
		case 0x01:   /* CLEAR_FEATURE */
		case 0x03:   /* SET_FEATURE */
			u.feat.Hdr.Length = sizeof(u.feat);
			u.feat.Hdr.Function = req == 1 ? clear_fn[recip] : set_fn[recip];
			u.feat.FeatureSelector = value;
			u.feat.Index = index;
			r = submit_sync(d, (PURB) &u);
			break;
		case 0x06:   /* GET_DESCRIPTOR */
			return kexusb_get_descriptor(d, (uint8_t) (value >> 8), (uint8_t) value, index, data, len, actlen);
		default:     /* SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE belong to the stack */
			return KEXUSB_INVAL;
		}
	}
	else
	{
		static const USHORT class_fn[] = { URB_FUNCTION_CLASS_DEVICE, URB_FUNCTION_CLASS_INTERFACE,
		                                   URB_FUNCTION_CLASS_ENDPOINT, URB_FUNCTION_CLASS_OTHER };
		static const USHORT vendor_fn[] = { URB_FUNCTION_VENDOR_DEVICE, URB_FUNCTION_VENDOR_INTERFACE,
		                                    URB_FUNCTION_VENDOR_ENDPOINT, URB_FUNCTION_VENDOR_OTHER };
		if (recip > 3 || type == 0x60)
			return KEXUSB_INVAL;
		u.vc.Hdr.Length = sizeof(u.vc);
		u.vc.Hdr.Function = type == 0x20 ? class_fn[recip] : vendor_fn[recip];
		u.vc.TransferFlags = (in ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT) | USBD_SHORT_TRANSFER_OK;
		u.vc.TransferBuffer = len ? data : NULL;
		u.vc.TransferBufferLength = len;
		u.vc.Request = req;
		u.vc.Value = value;
		u.vc.Index = index;
		r = submit_sync(d, (PURB) &u);
		got = u.vc.TransferBufferLength;
	}
	if (actlen)
		*actlen = got;
	return r;
}

static int pipe_request(kexusb_dev *d, void *pipe, USHORT fn)
{
	struct _URB_PIPE_REQUEST urb;
	RtlZeroMemory(&urb, sizeof(urb));
	urb.Hdr.Length = sizeof(urb);
	urb.Hdr.Function = fn;
	urb.PipeHandle = (USBD_PIPE_HANDLE) pipe;
	return submit_sync(d, (PURB) &urb);
}

int kexusb_pipe_abort(kexusb_dev *d, void *pipe)
{
	return pipe_request(d, pipe, URB_FUNCTION_ABORT_PIPE);
}

int kexusb_pipe_reset(kexusb_dev *d, void *pipe)
{
	return pipe_request(d, pipe, URB_FUNCTION_RESET_PIPE);
}

kexusb_xfer *kexusb_xfer_alloc(kexusb_dev *d)
{
	kexusb_xfer *x = (kexusb_xfer *) zalloc(sizeof(*x));
	if (!x)
		return NULL;
	x->dev = d;
	x->irp = IoAllocateIrp(d->lower->StackSize, FALSE);
	if (!x->irp)
	{
		ExFreePool(x);
		return NULL;
	}
	return x;
}

void kexusb_xfer_free(kexusb_xfer *x)
{
	if (!x)
		return;
	IoFreeIrp(x->irp);
	ExFreePool(x);
}

static NTSTATUS NTAPI xfer_done(PDEVICE_OBJECT dev, PIRP irp, PVOID context)
{
	kexusb_xfer *x = (kexusb_xfer *) context;
	int r = map_status(irp->IoStatus.Status, x->urb.Hdr.Status);
	unsigned int got = x->urb.TransferBufferLength;
	(void) dev;
	InterlockedExchange(&x->busy, 0);
	x->done(x->ctx, r, got);
	return STATUS_MORE_PROCESSING_REQUIRED;   /* the IRP is ours, reused */
}

int kexusb_xfer_submit(kexusb_xfer *x, void *pipe, int in, void *buf, unsigned int len, int short_ok,
                       kexusb_done_fn done, void *ctx)
{
	PIO_STACK_LOCATION sl;
	if (InterlockedExchange(&x->busy, 1))
		return KEXUSB_INVAL;
	RtlZeroMemory(&x->urb, sizeof(x->urb));
	UsbBuildInterruptOrBulkTransferRequest((PURB) &x->urb, (USHORT) sizeof(x->urb), (USBD_PIPE_HANDLE) pipe, buf,
	                                       NULL, len,
	                                       (in ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT) |
	                                           (short_ok ? USBD_SHORT_TRANSFER_OK : 0),
	                                       NULL);
	x->done = done;
	x->ctx = ctx;
	x->short_ok = short_ok;
	/* reinitialise the IRP for another trip (98 SE has no IoReuseIrp) */
	IoInitializeIrp(x->irp, IoSizeOfIrp(x->dev->lower->StackSize), x->dev->lower->StackSize);
	sl = IoGetNextIrpStackLocation(x->irp);
	sl->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	sl->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
	sl->Parameters.Others.Argument1 = &x->urb;
	IoSetCompletionRoutine(x->irp, xfer_done, x, TRUE, TRUE, TRUE);
	IoCallDriver(x->dev->lower, x->irp);
	return KEXUSB_OK;
}

void kexusb_xfer_cancel(kexusb_xfer *x)
{
	if (x && x->busy)
		IoCancelIrp(x->irp);
}

/* ---- worker threads ------------------------------------------------------------- */
struct kexbsd_worker
{
	KSPIN_LOCK lock;
	KEVENT wake;
	struct kexbsd_work *head, *tail;
	PETHREAD thread;   /* PsGetCurrentThread() of the worker */
	volatile int stop;
	KEVENT stopped;
};

static VOID NTAPI worker_main(PVOID arg)
{
	kexbsd_worker *w = (kexbsd_worker *) arg;
	w->thread = PsGetCurrentThread();
	for (;;)
	{
		struct kexbsd_work *wk;
		KIRQL irql;
		KeWaitForSingleObject(&w->wake, Executive, KernelMode, FALSE, NULL);
		for (;;)
		{
			KeAcquireSpinLock(&w->lock, &irql);
			wk = w->head;
			if (wk)
			{
				w->head = wk->next;
				if (!w->head)
					w->tail = NULL;
				wk->queued = 0;
			}
			KeReleaseSpinLock(&w->lock, irql);
			if (!wk)
				break;
			wk->fn(wk->arg);
		}
		if (w->stop)
		{
			KeSetEvent(&w->stopped, IO_NO_INCREMENT, FALSE);
			PsTerminateSystemThread(STATUS_SUCCESS);
		}
	}
}

kexbsd_worker *kexbsd_worker_create(const char *name)
{
	kexbsd_worker *w = (kexbsd_worker *) zalloc(sizeof(*w));
	HANDLE h;
	(void) name;
	if (!w)
		return NULL;
	KeInitializeSpinLock(&w->lock);
	KeInitializeEvent(&w->wake, SynchronizationEvent, FALSE);
	KeInitializeEvent(&w->stopped, NotificationEvent, FALSE);
	if (!NT_SUCCESS(PsCreateSystemThread(&h, 0, NULL, NULL, NULL, worker_main, w)))
	{
		ExFreePool(w);
		return NULL;
	}
	ZwClose(h);
	return w;
}

void kexbsd_worker_destroy(kexbsd_worker *w)
{
	if (!w)
		return;
	w->stop = 1;
	KeSetEvent(&w->wake, IO_NO_INCREMENT, FALSE);
	KeWaitForSingleObject(&w->stopped, Executive, KernelMode, FALSE, NULL);
	ExFreePool(w);
}

void kexbsd_worker_post(kexbsd_worker *w, struct kexbsd_work *wk)
{
	KIRQL irql;
	KeAcquireSpinLock(&w->lock, &irql);
	if (!wk->queued)
	{
		wk->queued = 1;
		wk->next = NULL;
		if (w->tail)
			w->tail->next = wk;
		else
			w->head = wk;
		w->tail = wk;
	}
	KeReleaseSpinLock(&w->lock, irql);
	KeSetEvent(&w->wake, IO_NO_INCREMENT, FALSE);
}

int kexbsd_worker_is_current(kexbsd_worker *w)
{
	return w && w->thread == PsGetCurrentThread();
}

struct sync_call
{
	struct kexbsd_work wk;
	void (*fn)(void *);
	void *arg;
	KEVENT done;
};

static void sync_thunk(void *p)
{
	struct sync_call *c = (struct sync_call *) p;
	c->fn(c->arg);
	KeSetEvent(&c->done, IO_NO_INCREMENT, FALSE);
}

void kexbsd_worker_call(kexbsd_worker *w, void (*fn)(void *), void *arg)
{
	struct sync_call c;
	if (kexbsd_worker_is_current(w))
	{
		fn(arg);
		return;
	}
	RtlZeroMemory(&c, sizeof(c));
	c.fn = fn;
	c.arg = arg;
	c.wk.fn = sync_thunk;
	c.wk.arg = &c;
	KeInitializeEvent(&c.done, NotificationEvent, FALSE);
	kexbsd_worker_post(w, &c.wk);
	KeWaitForSingleObject(&c.done, Executive, KernelMode, FALSE, NULL);
}
