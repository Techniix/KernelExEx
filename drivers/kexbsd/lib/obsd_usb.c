/*
 *  KernelExEx - kexbsd OpenBSD personality: usbdi(9) on the Windows 98 SE
 *  USB stack. Linked into each driver that uses it (not part of
 *  kexbsd.sys), on top of kexbsd.sys's kexusb_* and worker exports.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#include "dev/usb/usbdi.h"

static const char *const errstr[] = {
	"NORMAL_COMPLETION", "IN_PROGRESS", "PENDING_REQUESTS", "NOT_STARTED", "INVAL", "NOMEM", "CANCELLED",
	"BAD_ADDRESS", "IN_USE", "NO_ADDR", "SET_ADDR_FAILED", "NO_POWER", "TOO_DEEP", "IOERROR", "NOT_CONFIGURED",
	"TIMEOUT", "SHORT_XFER", "STALLED", "INTERRUPTED",
};

const char *usbd_errstr(usbd_status err)
{
	return (unsigned) err < nitems(errstr) ? errstr[err] : "unknown error";
}

static usbd_status from_kexusb(int r)
{
	switch (r)
	{
	case KEXUSB_OK:
		return USBD_NORMAL_COMPLETION;
	case KEXUSB_STALLED:
		return USBD_STALLED;
	case KEXUSB_CANCELLED:
		return USBD_CANCELLED;
	case KEXUSB_TIMEOUT:
		return USBD_TIMEOUT;
	case KEXUSB_NOMEM:
		return USBD_NOMEM;
	case KEXUSB_INVAL:
		return USBD_INVAL;
	case KEXUSB_SHORT:
		return USBD_SHORT_XFER;
	default:
		return USBD_IOERROR;
	}
}

int usbd_ratecheck(struct timeval *last)
{
	(void) last;
	return 1;
}

int usbd_is_dying(struct usbd_device *dev)
{
	return dev->dying;
}

void usbd_deactivate(struct usbd_device *dev)
{
	dev->dying = 1;
}

/* ---- descriptors ---------------------------------------------------------------- */
usb_config_descriptor_t *usbd_get_config_descriptor(struct usbd_device *dev)
{
	return dev->cdesc;
}

usb_device_descriptor_t *usbd_get_device_descriptor(struct usbd_device *dev)
{
	return &dev->ddesc;
}

usb_interface_descriptor_t *usbd_get_interface_descriptor(struct usbd_interface *iface)
{
	return iface ? iface->idesc : NULL;
}

usb_endpoint_descriptor_t *usbd_interface2endpoint_descriptor(struct usbd_interface *iface, uint8_t index)
{
	return index < iface->nendpt ? iface->endpoints[index] : NULL;
}

/* walk a configuration's descriptors */
#define FOREACH_DESC(cd, p, end)                                                                     \
	for ((p) = (uint8_t *) (cd), (end) = (p) + UGETW((cd)->wTotalLength); (p) + 2 <= (end) && (p)[0] >= 2; \
	     (p) += (p)[0])

static usb_interface_descriptor_t *find_iface(usb_config_descriptor_t *cd, int ifno, int alt)
{
	uint8_t *p, *end;
	FOREACH_DESC(cd, p, end)
	{
		if (p[1] == UDESC_INTERFACE)
		{
			usb_interface_descriptor_t *id = (usb_interface_descriptor_t *) p;
			if (id->bInterfaceNumber == ifno && id->bAlternateSetting == alt)
				return id;
		}
	}
	return NULL;
}

int usbd_get_no_alts(usb_config_descriptor_t *cd, int ifaceno)
{
	uint8_t *p, *end;
	int n = 0;
	FOREACH_DESC(cd, p, end)
	{
		if (p[1] == UDESC_INTERFACE && ((usb_interface_descriptor_t *) p)->bInterfaceNumber == ifaceno)
			n++;
	}
	return n;
}

/* the endpoint descriptors that follow an interface descriptor */
static void fill_endpoints(struct usbd_interface *iface)
{
	uint8_t *p = (uint8_t *) iface->idesc, *end = (uint8_t *) iface->device->cdesc + iface->device->cdesc_len;
	iface->nendpt = 0;
	for (p += p[0]; p + 2 <= end && p[0] >= 2 && p[1] != UDESC_INTERFACE; p += p[0])
		if (p[1] == UDESC_ENDPOINT && iface->nendpt < KEXUSB_MAX_PIPES)
			iface->endpoints[iface->nendpt++] = (usb_endpoint_descriptor_t *) p;
}

int usbd_iface_claimed(struct usbd_device *dev, int ifaceidx)
{
	return ifaceidx < dev->nifaces && dev->ifaces[ifaceidx].claimed;
}

void usbd_claim_iface(struct usbd_device *dev, int ifaceidx)
{
	if (ifaceidx < dev->nifaces)
		dev->ifaces[ifaceidx].claimed = 1;
}

usbd_status usbd_device2interface_handle(struct usbd_device *dev, uint8_t ifaceno, struct usbd_interface **iface)
{
	int i;
	for (i = 0; i < dev->nifaces; i++)
		if (dev->ifaces[i].idesc->bInterfaceNumber == ifaceno)
		{
			*iface = &dev->ifaces[i];
			return USBD_NORMAL_COMPLETION;
		}
	return USBD_INVAL;
}

/* ---- configurations ---------------------------------------------------------- */
static usbd_status read_config(struct usbd_device *dev, int index, usb_config_descriptor_t **out,
                               unsigned int *len)
{
	usb_config_descriptor_t head, *cd;
	unsigned int got = 0, total;
	int r = kexusb_get_descriptor(dev->kdev, UDESC_CONFIG, (uint8_t) index, 0, &head, sizeof(head), &got);
	if (r != KEXUSB_OK || got < sizeof(head))
		return r != KEXUSB_OK ? from_kexusb(r) : USBD_IOERROR;
	total = UGETW(head.wTotalLength);
	cd = (usb_config_descriptor_t *) kexbsd_malloc(total, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!cd)
		return USBD_NOMEM;
	r = kexusb_get_descriptor(dev->kdev, UDESC_CONFIG, (uint8_t) index, 0, cd, total, &got);
	if (r != KEXUSB_OK || got < total)
	{
		kexbsd_free(cd, M_DEVBUF);
		return r != KEXUSB_OK ? from_kexusb(r) : USBD_IOERROR;
	}
	*out = cd;
	*len = total;
	return USBD_NORMAL_COMPLETION;
}

/* build the interface table of a configuration (alternate 0), without
 * selecting it: for matching */
static void parse_ifaces(struct usbd_device *dev)
{
	uint8_t *p, *end;
	dev->nifaces = 0;
	FOREACH_DESC(dev->cdesc, p, end)
	{
		if (p[1] == UDESC_INTERFACE && ((usb_interface_descriptor_t *) p)->bAlternateSetting == 0 &&
		    dev->nifaces < USBD_MAX_IFACES)
		{
			struct usbd_interface *ifc = &dev->ifaces[dev->nifaces];
			memset(ifc, 0, sizeof(*ifc));
			ifc->device = dev;
			ifc->index = dev->nifaces;
			ifc->idesc = (usb_interface_descriptor_t *) p;
			fill_endpoints(ifc);
			dev->nifaces++;
		}
	}
}

usbd_status usbd_set_config_index(struct usbd_device *dev, int index, int msg)
{
	struct kexusb_iface_pipes pipes[USBD_MAX_IFACES];
	int n = 0, i, j, r;
	(void) msg;
	if (dev->config_index != index || !dev->cdesc)
	{
		usb_config_descriptor_t *cd;
		unsigned int len;
		usbd_status e = read_config(dev, index, &cd, &len);
		if (e)
			return e;
		if (dev->cdesc)
			kexbsd_free(dev->cdesc, M_DEVBUF);
		dev->cdesc = cd;
		dev->cdesc_len = len;
		dev->config_index = index;
		parse_ifaces(dev);
	}
	r = kexusb_select_config(dev->kdev, dev->cdesc, pipes, USBD_MAX_IFACES, &n);
	if (r != KEXUSB_OK)
		return from_kexusb(r);
	for (i = 0; i < n; i++)
		for (j = 0; j < dev->nifaces; j++)
			if (dev->ifaces[j].idesc->bInterfaceNumber == pipes[i].ifno)
				dev->ifaces[j].pipes = pipes[i];
	return USBD_NORMAL_COMPLETION;
}

usbd_status usbd_set_config_no(struct usbd_device *dev, int no, int msg)
{
	int i;
	for (i = 0; i < dev->ddesc.bNumConfigurations; i++)
	{
		usb_config_descriptor_t head;
		unsigned int got = 0;
		if (kexusb_get_descriptor(dev->kdev, UDESC_CONFIG, (uint8_t) i, 0, &head, sizeof(head), &got) ==
		        KEXUSB_OK &&
		    head.bConfigurationValue == no)
			return usbd_set_config_index(dev, i, msg);
	}
	return USBD_INVAL;
}

usbd_status usbd_set_interface(struct usbd_interface *iface, int altidx)
{
	struct usbd_device *dev = iface->device;
	int ifno = iface->idesc->bInterfaceNumber;
	usb_interface_descriptor_t *id = find_iface(dev->cdesc, ifno, altidx);
	struct kexusb_iface_pipes pipes;
	int r;
	if (!id)
		return USBD_INVAL;
	r = kexusb_select_interface(dev->kdev, dev->cdesc, ifno, altidx, &pipes);
	if (r != KEXUSB_OK)
		return from_kexusb(r);
	iface->idesc = id;
	iface->altindex = altidx;
	iface->pipes = pipes;
	fill_endpoints(iface);
	return USBD_NORMAL_COMPLETION;
}

/* ---- control requests (worker thread: they wait) ------------------------------------ */
usbd_status usbd_do_request_flags(struct usbd_device *dev, usb_device_request_t *req, void *data, uint16_t flags,
                                  int *actlen, uint32_t timeout)
{
	unsigned int len = UGETW(req->wLength), got = 0;
	void *buf = NULL;
	int r;
	(void) timeout;
	if (dev->dying)
		return USBD_IOERROR;
	/* the USB stack needs non-paged memory; the caller's may be on the stack */
	if (len)
	{
		buf = kexbsd_malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (!buf)
			return USBD_NOMEM;
		if (!(req->bmRequestType & UT_READ))
			memcpy(buf, data, len);
	}
	r = kexusb_control(dev->kdev, req->bmRequestType, req->bRequest, UGETW(req->wValue), UGETW(req->wIndex), buf,
	                   len, &got);
	if (len)
	{
		if ((req->bmRequestType & UT_READ) && r == KEXUSB_OK)
			memcpy(data, buf, got < len ? got : len);
		kexbsd_free(buf, M_DEVBUF);
	}
	if (actlen)
		*actlen = (int) got;
	if (r == KEXUSB_OK && got < len && (req->bmRequestType & UT_READ) && !(flags & USBD_SHORT_XFER_OK))
		return USBD_SHORT_XFER;
	return from_kexusb(r);
}

usbd_status usbd_do_request(struct usbd_device *dev, usb_device_request_t *req, void *data)
{
	return usbd_do_request_flags(dev, req, data, 0, NULL, USBD_DEFAULT_TIMEOUT);
}

usbd_status usbd_get_string_desc(struct usbd_device *dev, int index, int langid, usb_string_descriptor_t *sdesc,
                                 int *sizep)
{
	unsigned int got = 0;
	int r = kexusb_get_descriptor(dev->kdev, UDESC_STRING, (uint8_t) index, (uint16_t) langid, sdesc,
	                              sizeof(*sdesc), &got);
	if (sizep)
		*sizep = (int) got;
	return r == KEXUSB_OK && got >= 2 ? USBD_NORMAL_COMPLETION : (r ? from_kexusb(r) : USBD_SHORT_XFER);
}

/* ---- pipes ------------------------------------------------------------------------- */
usbd_status usbd_open_pipe(struct usbd_interface *iface, uint8_t address, uint8_t flags, struct usbd_pipe **pipe)
{
	int i;
	(void) flags;
	for (i = 0; i < iface->pipes.npipes; i++)
		if (iface->pipes.pipes[i].ep == address)
		{
			struct usbd_pipe *p = (struct usbd_pipe *) kexbsd_malloc(sizeof(*p), M_DEVBUF, M_NOWAIT | M_ZERO);
			if (!p)
				return USBD_NOMEM;
			p->iface = iface;
			p->device = iface->device;
			p->handle = iface->pipes.pipes[i].handle;
			p->ep = address;
			p->type = iface->pipes.pipes[i].type;
			p->maxpkt = iface->pipes.pipes[i].maxpkt;
			p->in = (address & UE_DIR_IN) != 0;
			*pipe = p;
			return USBD_NORMAL_COMPLETION;
		}
	return USBD_BAD_ADDRESS;
}

void usbd_abort_pipe(struct usbd_pipe *pipe)
{
	if (pipe)
		kexusb_pipe_abort(pipe->device->kdev, pipe->handle);
}

usbd_status usbd_close_pipe(struct usbd_pipe *pipe)
{
	if (!pipe)
		return USBD_NORMAL_COMPLETION;
	usbd_abort_pipe(pipe);
	if (pipe->intr_xfer)
		usbd_free_xfer(pipe->intr_xfer);
	kexbsd_free(pipe, M_DEVBUF);
	return USBD_NORMAL_COMPLETION;
}

usbd_status usbd_clear_endpoint_stall(struct usbd_pipe *pipe)
{
	return from_kexusb(kexusb_pipe_reset(pipe->device->kdev, pipe->handle));
}

static void stall_work(void *arg)
{
	struct usbd_pipe *pipe = (struct usbd_pipe *) arg;
	kexusb_pipe_reset(pipe->device->kdev, pipe->handle);
}

void usbd_clear_endpoint_stall_async(struct usbd_pipe *pipe)
{
	/* on the worker already, which is where the reset may wait */
	stall_work(pipe);
}

/* ---- transfers ---------------------------------------------------------------------- */
struct usbd_xfer *usbd_alloc_xfer(struct usbd_device *dev)
{
	struct usbd_xfer *x = (struct usbd_xfer *) kexbsd_malloc(sizeof(*x), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!x)
		return NULL;
	x->device = dev;
	x->kx = kexusb_xfer_alloc(dev->kdev);
	if (!x->kx)
	{
		kexbsd_free(x, M_DEVBUF);
		return NULL;
	}
	KeInitializeEvent(&x->done, NotificationEvent, FALSE);
	return x;
}

void usbd_free_buffer(struct usbd_xfer *x)
{
	if (x->own_buffer)
		kexbsd_free(x->own_buffer, M_DEVBUF);
	x->own_buffer = NULL;
	x->own_size = 0;
}

void usbd_free_xfer(struct usbd_xfer *x)
{
	if (!x)
		return;
	kexusb_xfer_cancel(x->kx);
	while (x->busy)
		DELAY(1000);
	usbd_free_buffer(x);
	kexusb_xfer_free(x->kx);
	kexbsd_free(x, M_DEVBUF);
}

void *usbd_alloc_buffer(struct usbd_xfer *x, uint32_t size)
{
	if (x->own_buffer && x->own_size >= size)
		return x->own_buffer;
	usbd_free_buffer(x);
	x->own_buffer = kexbsd_malloc(size, M_DEVBUF, M_NOWAIT | M_ZERO);
	x->own_size = x->own_buffer ? size : 0;
	return x->own_buffer;
}

void usbd_setup_xfer(struct usbd_xfer *x, struct usbd_pipe *pipe, void *priv, void *buffer, uint32_t length,
                     uint16_t flags, uint32_t timeout, usbd_callback cb)
{
	x->pipe = pipe;
	x->priv = priv;
	x->buffer = buffer ? buffer : x->own_buffer;
	x->length = length;
	x->flags = flags;
	x->timeout = timeout;
	x->callback = cb;
	x->actlen = 0;
	x->status = USBD_NOT_STARTED;
}

void usbd_get_xfer_status(struct usbd_xfer *x, void **priv, void **buffer, uint32_t *count, usbd_status *status)
{
	if (priv)
		*priv = x->priv;
	if (buffer)
		*buffer = x->buffer;
	if (count)
		*count = x->actlen;
	if (status)
		*status = x->status;
}

static void resubmit_intr(struct usbd_xfer *x);

/* the callback, on the worker */
static void xfer_callback(void *arg)
{
	struct usbd_xfer *x = (struct usbd_xfer *) arg;
	usbd_status st = x->status;
	x->busy = 0;
	if (x->callback)
		x->callback(x, x->priv, st);
	if (x->pipe && x->pipe->intr_xfer == x && st != USBD_CANCELLED && !x->device->dying)
		resubmit_intr(x);
}

/* completion, at DISPATCH_LEVEL */
static void xfer_done(void *ctx, int r, unsigned int actlen)
{
	struct usbd_xfer *x = (struct usbd_xfer *) ctx;
#ifdef KEX_DEBUGCON
	static int traced;
	if (traced < 24)
	{
		traced++;
		kexbsd_printf("usb: ep %02x done r=%d len %u of %u\n", x->pipe->ep, r, actlen, (unsigned) x->length);
	}
#endif
	x->actlen = actlen;
	x->status = from_kexusb(r);
	if (x->status == USBD_NORMAL_COMPLETION && x->pipe->in && actlen < x->length && !(x->flags & USBD_SHORT_XFER_OK))
		x->status = USBD_SHORT_XFER;
	/* an OUT transfer that ends on a packet boundary needs a zero-length
	 * packet to end it, when the driver asks (USBD_FORCE_SHORT_XFER) */
	if (x->status == USBD_NORMAL_COMPLETION && !x->pipe->in && (x->flags & USBD_FORCE_SHORT_XFER) &&
	    !x->zlp_sent && x->length && x->pipe->maxpkt && x->length % x->pipe->maxpkt == 0)
	{
		x->zlp_sent = 1;
		if (kexusb_xfer_submit(x->kx, x->pipe->handle, 0, x->buffer, 0, 1, xfer_done, x) == KEXUSB_OK)
			return;
	}
	x->actlen = x->zlp_sent ? x->length : actlen;
	if (x->flags & USBD_SYNCHRONOUS)
	{
		x->busy = 0;
		KeSetEvent(&x->done, IO_NO_INCREMENT, FALSE);
		return;
	}
	x->work.fn = xfer_callback;
	x->work.arg = x;
	kexbsd_worker_post(x->device->worker, &x->work);
}

usbd_status usbd_transfer(struct usbd_xfer *x)
{
	int r;
	if (x->device->dying)
		return USBD_IOERROR;
	if (!x->buffer && x->length)
		return USBD_INVAL;
	x->busy = 1;
	x->zlp_sent = 0;
	KeClearEvent(&x->done);
#ifdef KEX_DEBUGCON
	{
		static int traced;
		if (traced < 24)
		{
			traced++;
			kexbsd_printf("usb: ep %02x submit %u flags %x\n", x->pipe->ep, (unsigned) x->length, x->flags);
		}
	}
#endif
	r = kexusb_xfer_submit(x->kx, x->pipe->handle, x->pipe->in, x->buffer, x->length,
	                       !x->pipe->in || (x->flags & USBD_SHORT_XFER_OK), xfer_done, x);
	if (r != KEXUSB_OK)
	{
		x->busy = 0;
		return from_kexusb(r);
	}
	if (x->flags & USBD_SYNCHRONOUS)
	{
		KeWaitForSingleObject(&x->done, Executive, KernelMode, FALSE, NULL);
		if (x->callback)
			x->callback(x, x->priv, x->status);
		return x->status;
	}
	return USBD_IN_PROGRESS;
}

static void resubmit_intr(struct usbd_xfer *x)
{
	usbd_setup_xfer(x, x->pipe, x->priv, x->buffer, x->length, x->flags, USBD_NO_TIMEOUT, x->callback);
	usbd_transfer(x);
}

usbd_status usbd_open_pipe_intr(struct usbd_interface *iface, uint8_t address, uint8_t flags,
                                struct usbd_pipe **pipe, void *priv, void *buffer, uint32_t len,
                                usbd_callback cb, int ival)
{
	struct usbd_xfer *x;
	usbd_status e = usbd_open_pipe(iface, address, flags, pipe);
	(void) ival;
	if (e)
		return e;
	x = usbd_alloc_xfer(iface->device);
	if (!x)
	{
		usbd_close_pipe(*pipe);
		return USBD_NOMEM;
	}
	if (!buffer)
		buffer = usbd_alloc_buffer(x, len);
	(*pipe)->intr_xfer = x;
	usbd_setup_xfer(x, *pipe, priv, buffer, len, USBD_SHORT_XFER_OK, USBD_NO_TIMEOUT, cb);
	e = usbd_transfer(x);
	return e == USBD_IN_PROGRESS ? USBD_NORMAL_COMPLETION : e;
}

const struct usb_devno *usbd_match_device(const struct usb_devno *tbl, unsigned int nentries, unsigned int sz,
                                          uint16_t vendor, uint16_t product)
{
	while (nentries--)
	{
		if (tbl->ud_vendor == vendor && (tbl->ud_product == product || tbl->ud_product == 0xffff))
			return tbl;
		tbl = (const struct usb_devno *) ((const char *) tbl + sz);
	}
	return NULL;
}

/* ---- tasks ------------------------------------------------------------------------- */
static void task_thunk(void *arg)
{
	struct usb_task *t = (struct usb_task *) arg;
	t->fun(t->arg);
}

void usb_add_task(struct usbd_device *dev, struct usb_task *t)
{
	t->work.fn = task_thunk;
	t->work.arg = t;
	kexbsd_worker_post(dev->worker, &t->work);
}

void usb_rem_task(struct usbd_device *dev, struct usb_task *t)
{
	(void) dev;
	(void) t;   /* a queued task still runs; drivers tolerate that (dying checks) */
}

/* ---- bring-up ------------------------------------------------------------------------ */
int obsd_usb_attach(PDEVICE_OBJECT lower, kexbsd_worker *worker, obsd_usb_match_fn match,
                    obsd_usb_attach_fn attach, struct device *sc, struct usbd_device **devp)
{
	struct usbd_device *dev;
	unsigned int got = 0;
	int ci, i, best = 0, best_ci = -1, best_if = -1;
	struct usbd_interface *ifptrs[USBD_MAX_IFACES];
	struct usb_attach_arg uaa;

	dev = (struct usbd_device *) kexbsd_malloc(sizeof(*dev), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!dev)
		return ENOMEM;
	dev->kdev = kexusb_open(lower);
	dev->worker = worker;
	dev->config_index = -1;
	/* high speed on KernelExEx's USB 2.0 driver, full speed on Windows' own stack */
	dev->speed = dev->kdev && kexusb_high_speed(dev->kdev) ? USB_SPEED_HIGH : USB_SPEED_FULL;
	if (!dev->kdev ||
	    kexusb_get_descriptor(dev->kdev, UDESC_DEVICE, 0, 0, &dev->ddesc, sizeof(dev->ddesc), &got) != KEXUSB_OK ||
	    got < sizeof(dev->ddesc))
	{
		kexbsd_printf("%s: cannot read the device descriptor\n", sc->dv_xname);
		goto fail;
	}
	kexbsd_printf("%s: USB %04x:%04x, %d configuration(s), %s\n", sc->dv_xname, UGETW(dev->ddesc.idVendor),
	              UGETW(dev->ddesc.idProduct), dev->ddesc.bNumConfigurations,
	              dev->speed == USB_SPEED_HIGH ? "high speed (USB 2.0)" : "full speed (USB 1.1)");

	/* the best match over every interface of every configuration */
	for (ci = 0; ci < dev->ddesc.bNumConfigurations; ci++)
	{
		usb_config_descriptor_t *cd;
		unsigned int len;
		if (read_config(dev, ci, &cd, &len))
			continue;
		if (dev->cdesc)
			kexbsd_free(dev->cdesc, M_DEVBUF);
		dev->cdesc = cd;
		dev->cdesc_len = len;
		dev->config_index = ci;
		parse_ifaces(dev);
		for (i = 0; i < dev->nifaces; i++)
			ifptrs[i] = &dev->ifaces[i];
		for (i = 0; i < dev->nifaces; i++)
		{
			int m;
			memset(&uaa, 0, sizeof(uaa));
			uaa.device = dev;
			uaa.iface = &dev->ifaces[i];
			uaa.ifaces = ifptrs;
			uaa.nifaces = dev->nifaces;
			uaa.vendor = UGETW(dev->ddesc.idVendor);
			uaa.product = UGETW(dev->ddesc.idProduct);
			uaa.release = UGETW(dev->ddesc.bcdDevice);
			uaa.configno = cd->bConfigurationValue;
			uaa.ifaceno = dev->ifaces[i].idesc->bInterfaceNumber;
			m = match(NULL, NULL, &uaa);
			if (m > best)
			{
				best = m;
				best_ci = ci;
				best_if = i;
			}
		}
	}
	if (best_ci < 0)
	{
		kexbsd_printf("%s: no interface the driver supports\n", sc->dv_xname);
		goto fail;
	}
	dev->config_index = -1;   /* force a re-read: select the chosen one */
	if (usbd_set_config_index(dev, best_ci, 0))
	{
		kexbsd_printf("%s: cannot select configuration %d\n", sc->dv_xname, best_ci);
		goto fail;
	}
	for (i = 0; i < dev->nifaces; i++)
		ifptrs[i] = &dev->ifaces[i];
	memset(&uaa, 0, sizeof(uaa));
	uaa.device = dev;
	uaa.iface = &dev->ifaces[best_if];
	uaa.ifaces = ifptrs;
	uaa.nifaces = dev->nifaces;
	uaa.vendor = UGETW(dev->ddesc.idVendor);
	uaa.product = UGETW(dev->ddesc.idProduct);
	uaa.release = UGETW(dev->ddesc.bcdDevice);
	uaa.configno = dev->cdesc->bConfigurationValue;
	uaa.ifaceno = uaa.iface->idesc->bInterfaceNumber;
	usbd_claim_iface(dev, best_if);
	kexbsd_printf("%s at usb configuration %d interface %d", sc->dv_xname, uaa.configno, uaa.ifaceno);
	attach(NULL, sc, &uaa);   /* the driver continues the line */
	*devp = dev;
	return 0;

fail:
	if (dev->cdesc)
		kexbsd_free(dev->cdesc, M_DEVBUF);
	kexusb_close(dev->kdev);
	kexbsd_free(dev, M_DEVBUF);
	return ENXIO;
}

void obsd_usb_detach(struct usbd_device *dev)
{
	dev->dying = 1;
}
