/*
 * KernelExEx: FreeBSD's USB driver interface (usbdi(9): usb_config,
 * usbd_transfer_*, usbd_do_request) on kexusb, Windows 98's USBD.
 *
 * As on FreeBSD, a transfer's callback runs on a USB callback thread (one
 * per device here) holding the driver's mutex, with the state SETUP when
 * the driver starts the transfer and TRANSFERRED or ERROR when it is done;
 * the driver submits again from the callback. kexusb completes transfers
 * at DISPATCH_LEVEL, and only queues the callback from there.
 *
 * Copyright (c) 2026 KernelExEx contributors. GPLv2 (see COPYING).
 */
#include "kexfbsd.h"
#include "kexbsd/kexusb.h"
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_device.h>

struct usb_xfer
{
	struct usb_device *udev;
	struct usb_config cfg;
	void *priv_sc;
	struct mtx *mtx;
	void *pipe;
	uint8_t ep;
	int in;
	uint16_t maxpkt;
	unsigned int bufsize;
	void *own_buf;
	kexusb_xfer *kx;
	void *buf;
	unsigned int len;
	unsigned int actlen;
	usb_error_t error;
	uint8_t state;
	volatile LONG busy;        /* submitted to USBD */
	volatile LONG started;     /* usbd_transfer_start, until stop */
	volatile LONG stall;       /* clear the stall before the next submit */
	int zlp_sent;
	struct kexbsd_work work;   /* the callback */
	struct kexbsd_work reset;  /* clear stall, then submit */
};

static void run_callback(void *arg)
{
	struct usb_xfer *x = (struct usb_xfer *) arg;
	if (x->mtx)
		mtx_lock(x->mtx);
	/* a SETUP for a transfer stopped since it was queued is dropped;
	 * completions always reach the driver, which owns the buffers */
	if (x->state != USB_ST_SETUP || x->started)
		x->cfg.callback(x, x->error);
	if (x->mtx)
		mtx_unlock(x->mtx);
}

static void queue_callback(struct usb_xfer *x, uint8_t state, usb_error_t err)
{
	x->state = state;
	x->error = err;
	x->work.fn = run_callback;
	x->work.arg = x;
	kexbsd_worker_post((kexbsd_worker *) x->udev->worker, &x->work);
}

static usb_error_t map_status(int st)
{
	switch (st)
	{
	case KEXUSB_OK:
		return USB_ERR_NORMAL_COMPLETION;
	case KEXUSB_STALLED:
		return USB_ERR_STALLED;
	case KEXUSB_CANCELLED:
		return USB_ERR_CANCELLED;
	case KEXUSB_TIMEOUT:
		return USB_ERR_TIMEOUT;
	case KEXUSB_NOMEM:
		return USB_ERR_NOMEM;
	case KEXUSB_SHORT:
		return USB_ERR_SHORT_XFER;
	default:
		return USB_ERR_IOERROR;
	}
}

static void xfer_done(void *ctx, int status, unsigned int actlen)
{
	struct usb_xfer *x = (struct usb_xfer *) ctx;
	/* force_short_xfer: a transfer that fills its last packet ends with a ZLP */
	if (status == KEXUSB_OK && !x->in && x->cfg.flags.force_short_xfer && !x->zlp_sent && x->len &&
	    x->maxpkt && (x->len % x->maxpkt) == 0)
	{
		x->zlp_sent = 1;
		x->actlen = actlen;
		if (kexusb_xfer_submit(x->kx, x->pipe, 0, x->buf, 0, 1, xfer_done, x) == KEXUSB_OK)
			return;
		actlen = x->actlen;
	}
	else if (x->zlp_sent)
		actlen = x->actlen;
	x->actlen = actlen;
	InterlockedExchange(&x->busy, 0);
	if (status == KEXUSB_OK)
		queue_callback(x, USB_ST_TRANSFERRED, USB_ERR_NORMAL_COMPLETION);
	else
		queue_callback(x, USB_ST_ERROR, x->started ? map_status(status) : USB_ERR_CANCELLED);
}

static void do_submit(struct usb_xfer *x)
{
	int r;
	x->zlp_sent = 0;
	x->actlen = 0;
	r = kexusb_xfer_submit(x->kx, x->pipe, x->in, x->buf, x->len, x->in ? x->cfg.flags.short_xfer_ok || 1 : 1,
	                       xfer_done, x);
	if (r != KEXUSB_OK)
	{
		InterlockedExchange(&x->busy, 0);
		queue_callback(x, USB_ST_ERROR, map_status(r));
	}
}

static void reset_then_submit(void *arg)
{
	struct usb_xfer *x = (struct usb_xfer *) arg;
	kexusb_pipe_reset((kexusb_dev *) x->udev->kexusb, x->pipe);
	do_submit(x);
}

void usbd_transfer_submit(struct usb_xfer *x)
{
	if (!x)
		return;
	if (!x->started)
	{
		queue_callback(x, USB_ST_ERROR, USB_ERR_CANCELLED);
		return;
	}
	if (InterlockedExchange(&x->busy, 1))
		return;   /* already in flight */
	if (InterlockedExchange(&x->stall, 0))
	{
		/* clearing a stall waits for USBD: not from the callback's locked context */
		x->reset.fn = reset_then_submit;
		x->reset.arg = x;
		kexbsd_worker_post((kexbsd_worker *) x->udev->worker, &x->reset);
		return;
	}
	do_submit(x);
}

void usbd_transfer_start(struct usb_xfer *x)
{
	if (!x)
		return;
	if (InterlockedExchange(&x->started, 1) && x->busy)
		return;
	if (!x->busy && !x->work.queued)
		queue_callback(x, USB_ST_SETUP, USB_ERR_NORMAL_COMPLETION);
}

void usbd_transfer_stop(struct usb_xfer *x)
{
	if (!x)
		return;
	InterlockedExchange(&x->started, 0);
	if (x->busy)
		kexusb_xfer_cancel(x->kx);   /* the completion reports CANCELLED */
}

uint8_t usbd_transfer_pending(struct usb_xfer *x)
{
	return x && (x->busy || x->work.queued);
}

void usbd_transfer_drain(struct usb_xfer *x)
{
	int n;
	if (!x)
		return;
	usbd_transfer_stop(x);
	for (n = 0; n < 2000 && (x->busy || x->work.queued); n++)
		pause("usbdrn", 1);
}

void usbd_transfer_clear_stall(struct usb_xfer *x)
{
	if (x)
		InterlockedExchange(&x->stall, 1);
}

void usbd_xfer_set_stall(struct usb_xfer *x)
{
	if (x)
		InterlockedExchange(&x->stall, 1);
}

int usbd_xfer_is_stalled(struct usb_xfer *x)
{
	return x && x->stall;
}

static int find_endpoint(struct usb_device *udev, uint8_t iface_index, const struct usb_config *c, int *used)
{
	int i, nth = 0;
	for (i = 0; i < udev->endpoints_max; i++)
	{
		struct usb_endpoint_descriptor *ed = udev->endpoints[i].edesc;
		uint8_t addr;
		if (!ed || udev->endpoints[i].iface_index != iface_index || used[i])
			continue;
		addr = ed->bEndpointAddress;
		if ((ed->bmAttributes & UE_XFERTYPE) != c->type)
			continue;
		if (c->direction != UE_DIR_ANY && (addr & UE_DIR_IN) != (c->direction & UE_DIR_IN))
			continue;
		if (c->endpoint != UE_ADDR_ANY && UE_GET_ADDR(addr) != (c->endpoint & UE_ADDR))
			continue;
		if (nth++ < c->ep_index)
			continue;
		return i;
	}
	return -1;
}

usb_error_t usbd_transfer_setup(struct usb_device *udev, const uint8_t *ifaces, struct usb_xfer **pxfer,
                                const struct usb_config *setup, uint16_t n, void *priv_sc, struct mtx *priv_mtx)
{
	int used[KEXFBSD_USB_MAX_EP];
	uint16_t i;
	memset(used, 0, sizeof(used));
	for (i = 0; i < n; i++)
		pxfer[i] = NULL;
	for (i = 0; i < n; i++)
	{
		const struct usb_config *c = &setup[i];
		struct usb_xfer *x;
		int e;
		if (!c->callback)
			continue;
		e = find_endpoint(udev, ifaces[c->if_index], c, used);
		if (e < 0)
		{
			kexbsd_printf("usb: no endpoint for transfer %u (type %u, ep %u, dir %02x)\n", i, c->type, c->endpoint,
			              c->direction);
			usbd_transfer_unsetup(pxfer, n);
			return USB_ERR_NO_PIPE;
		}
		used[e] = 1;
		x = (struct usb_xfer *) malloc(sizeof(*x), M_USBDEV, M_WAITOK | M_ZERO);
		if (!x)
			goto nomem;
		x->udev = udev;
		x->cfg = *c;
		x->priv_sc = priv_sc;
		x->mtx = priv_mtx;
		x->pipe = udev->pipe[e];
		x->ep = udev->endpoints[e].edesc->bEndpointAddress;
		x->in = (x->ep & UE_DIR_IN) != 0;
		x->maxpkt = UGETW(udev->endpoints[e].edesc->wMaxPacketSize) & 0x7ff;
		x->bufsize = c->bufsize ? c->bufsize : x->maxpkt;
		if (!c->flags.ext_buffer)
		{
			x->own_buf = malloc(x->bufsize, M_USBDEV, M_WAITOK);
			if (!x->own_buf)
			{
				free(x, M_USBDEV);
				goto nomem;
			}
		}
		x->buf = x->own_buf;
		x->len = x->bufsize;
		x->kx = kexusb_xfer_alloc((kexusb_dev *) udev->kexusb);
		if (!x->kx)
		{
			free(x->own_buf, M_USBDEV);
			free(x, M_USBDEV);
			goto nomem;
		}
		pxfer[i] = x;
	}
	return USB_ERR_NORMAL_COMPLETION;
nomem:
	usbd_transfer_unsetup(pxfer, n);
	return USB_ERR_NOMEM;
}

void usbd_transfer_unsetup(struct usb_xfer **pxfer, uint16_t n)
{
	uint16_t i;
	for (i = 0; i < n; i++)
	{
		struct usb_xfer *x = pxfer[i];
		if (!x)
			continue;
		usbd_transfer_drain(x);
		kexusb_xfer_free(x->kx);
		if (x->own_buf)
			free(x->own_buf, M_USBDEV);
		free(x, M_USBDEV);
		pxfer[i] = NULL;
	}
}

void *usbd_xfer_softc(struct usb_xfer *x)
{
	return x->priv_sc;
}

uint8_t usbd_xfer_state(struct usb_xfer *x)
{
	return x->state;
}

void usbd_xfer_set_frame_data(struct usb_xfer *x, usb_frcount_t fr, void *ptr, usb_frlength_t len)
{
	(void) fr;
	x->buf = ptr;
	x->len = len;
}

void usbd_xfer_set_frame_len(struct usb_xfer *x, usb_frcount_t fr, usb_frlength_t len)
{
	(void) fr;
	x->len = len;
}

void *usbd_xfer_get_frame_buffer(struct usb_xfer *x, usb_frcount_t fr)
{
	(void) fr;
	return x->buf;
}

void usbd_xfer_set_frames(struct usb_xfer *x, usb_frcount_t n)
{
	(void) x;
	(void) n;
}

usb_frlength_t usbd_xfer_max_len(struct usb_xfer *x)
{
	return x->bufsize;
}

usb_frlength_t usbd_xfer_max_framelen(struct usb_xfer *x)
{
	return x->maxpkt;
}

void usbd_xfer_status(struct usb_xfer *x, int *actlen, int *sumlen, int *aframes, int *nframes)
{
	if (actlen)
		*actlen = (int) x->actlen;
	if (sumlen)
		*sumlen = (int) x->len;
	if (aframes)
		*aframes = x->state == USB_ST_TRANSFERRED ? 1 : 0;
	if (nframes)
		*nframes = 1;
}

enum usb_dev_speed usbd_get_speed(struct usb_device *udev)
{
	return udev->speed;
}

static const char *const errs[] = { "NORMAL_COMPLETION", "PENDING_REQUESTS", "NOT_STARTED", "INVAL", "NOMEM",
	                                "CANCELLED", "BAD_ADDRESS", "BAD_BUFSIZE", "BAD_FLAG", "NO_CALLBACK",
	                                "IN_USE", "NO_ADDR", "NO_PIPE", "ZERO_NFRAMES", "ZERO_MAXP", "SET_ADDR_FAILED",
	                                "NO_POWER", "TOO_DEEP", "IOERROR", "NOT_CONFIGURED", "TIMEOUT", "SHORT_XFER",
	                                "STALLED", "INTERRUPTED", "DMA_LOAD_FAILED", "BAD_CONTEXT", "NO_ROOT_HUB",
	                                "NO_INTR_THREAD", "NOT_LOCKED" };

const char *usbd_errstr(usb_error_t e)
{
	return (unsigned) e < nitems(errs) ? errs[e] : "USB_ERR_UNKNOWN";
}

void usb_pause_mtx(struct mtx *m, int t)
{
	if (m)
		mtx_unlock(m);
	pause("usbpau", t > 0 ? t : 1);
	if (m)
		mtx_lock(m);
}

usb_error_t usbd_do_request_flags(struct usb_device *udev, struct mtx *m, struct usb_device_request *req, void *data,
                                  uint16_t flags, uint16_t *actlen, usb_timeout_t timeout)
{
	unsigned int got = 0;
	int r, held = m && mtx_owned(m);
	(void) flags;
	(void) timeout;
	if (held)
		mtx_unlock(m);
	if (KeGetCurrentIrql() > PASSIVE_LEVEL)
	{
		kexbsd_printf("usb: control request at raised IRQL (req %02x)\n", req->bRequest);
		r = KEXUSB_INVAL;
	}
	else
		r = kexusb_control((kexusb_dev *) udev->kexusb, req->bmRequestType, req->bRequest, UGETW(req->wValue),
		                   UGETW(req->wIndex), data, UGETW(req->wLength), &got);
	if (held)
		mtx_lock(m);
	if (actlen)
		*actlen = (uint16_t) got;
	return map_status(r);
}

int usbd_lookup_id_by_uaa(const struct usb_device_id *id, usb_size_t sizeof_id, struct usb_attach_arg *uaa)
{
	usb_size_t i, n = sizeof_id / sizeof(*id);
	for (i = 0; i < n; i++, id++)
	{
		if (id->match_flag_vendor && id->idVendor != uaa->info.idVendor)
			continue;
		if (id->match_flag_product && id->idProduct != uaa->info.idProduct)
			continue;
		if (id->match_flag_dev_lo && id->bcdDevice_lo > uaa->info.bcdDevice)
			continue;
		if (id->match_flag_dev_hi && id->bcdDevice_hi < uaa->info.bcdDevice)
			continue;
		if (id->match_flag_dev_class && id->bDeviceClass != uaa->info.bDeviceClass)
			continue;
		if (id->match_flag_int_class && id->bInterfaceClass != uaa->info.bInterfaceClass)
			continue;
		if (id->match_flag_int_subclass && id->bInterfaceSubClass != uaa->info.bInterfaceSubClass)
			continue;
		if (id->match_flag_int_protocol && id->bInterfaceProtocol != uaa->info.bInterfaceProtocol)
			continue;
		uaa->driver_info = id->driver_info;
		return 0;
	}
	return ENXIO;
}

void device_set_usb_desc(device_t dev)
{
	device_set_desc(dev, "USB device");
}
