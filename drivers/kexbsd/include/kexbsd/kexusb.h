/*
 *  KernelExEx - kexbsd.sys: USB below the BSD personalities.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  A neutral interface to the Windows 98 SE USB stack (URBs sent to the
 *  device's lower device object), so the OpenBSD usbdi layer never sees
 *  Microsoft's USB headers and the reverse. Synchronous calls need
 *  PASSIVE_LEVEL; transfer completions run at DISPATCH_LEVEL.
 */
#ifndef KEXBSD_KEXUSB_H
#define KEXBSD_KEXUSB_H

#include "kexbsd/kern.h"

typedef struct kexusb_dev kexusb_dev;
typedef struct kexusb_xfer kexusb_xfer;

enum
{
	KEXUSB_OK = 0,
	KEXUSB_STALLED,
	KEXUSB_CANCELLED,
	KEXUSB_TIMEOUT,
	KEXUSB_NOMEM,
	KEXUSB_INVAL,
	KEXUSB_IOERROR,
	KEXUSB_SHORT,   /* short transfer where one was not allowed */
};

#define KEXUSB_MAX_PIPES 16

struct kexusb_pipe_info
{
	uint8_t ep;          /* bEndpointAddress */
	uint8_t type;        /* UE_CONTROL 0, UE_ISOCHRONOUS 1, UE_BULK 2, UE_INTERRUPT 3 */
	uint16_t maxpkt;
	void *handle;        /* USBD_PIPE_HANDLE */
};

/* result of selecting a configuration or an alternate setting */
struct kexusb_iface_pipes
{
	int ifno;
	int alt;
	int npipes;
	struct kexusb_pipe_info pipes[KEXUSB_MAX_PIPES];
};

typedef void (*kexusb_done_fn)(void *ctx, int status, unsigned int actlen);

KEXBSD_API kexusb_dev *kexusb_open(PDEVICE_OBJECT lower);
KEXBSD_API void kexusb_close(kexusb_dev *d);
KEXBSD_API int kexusb_get_descriptor(kexusb_dev *d, uint8_t type, uint8_t index, uint16_t langid, void *buf,
                                     unsigned int len, unsigned int *got);
/* select a configuration (full descriptor, alternate 0 everywhere); fills
 * one entry per interface, up to max */
KEXBSD_API int kexusb_select_config(kexusb_dev *d, const void *cdesc, struct kexusb_iface_pipes *out, int max,
                                    int *nout);
KEXBSD_API int kexusb_select_interface(kexusb_dev *d, const void *cdesc, int ifno, int alt,
                                       struct kexusb_iface_pipes *out);
/* a control transfer on the default pipe; bmRequestType/bRequest as in USB 2.0 ch. 9 */
KEXBSD_API int kexusb_control(kexusb_dev *d, uint8_t rt, uint8_t req, uint16_t value, uint16_t index, void *data,
                              unsigned int len, unsigned int *actlen);
KEXBSD_API int kexusb_pipe_abort(kexusb_dev *d, void *pipe);
KEXBSD_API int kexusb_pipe_reset(kexusb_dev *d, void *pipe);

KEXBSD_API kexusb_xfer *kexusb_xfer_alloc(kexusb_dev *d);
KEXBSD_API void kexusb_xfer_free(kexusb_xfer *x);
/* bulk or interrupt; done(ctx, status, actlen) runs at DISPATCH_LEVEL */
KEXBSD_API int kexusb_xfer_submit(kexusb_xfer *x, void *pipe, int in, void *buf, unsigned int len, int short_ok,
                                  kexusb_done_fn done, void *ctx);
KEXBSD_API void kexusb_xfer_cancel(kexusb_xfer *x);

/* ---- worker threads: where BSD driver code runs ------------------------------- */
struct kexbsd_work
{
	struct kexbsd_work *next;
	void (*fn)(void *);
	void *arg;
	volatile int queued;
};
typedef struct kexbsd_worker kexbsd_worker;
KEXBSD_API kexbsd_worker *kexbsd_worker_create(const char *name);
KEXBSD_API void kexbsd_worker_destroy(kexbsd_worker *w);
/* queue (any IRQL <= DISPATCH); a work item already queued is not queued twice */
KEXBSD_API void kexbsd_worker_post(kexbsd_worker *w, struct kexbsd_work *wk);
/* run fn(arg) on the worker and wait (PASSIVE_LEVEL; runs inline when called on the worker) */
KEXBSD_API void kexbsd_worker_call(kexbsd_worker *w, void (*fn)(void *), void *arg);
KEXBSD_API int kexbsd_worker_is_current(kexbsd_worker *w);

#endif
