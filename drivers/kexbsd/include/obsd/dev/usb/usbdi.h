/*
 *  KernelExEx - kexbsd OpenBSD personality: <dev/usb/usbdi.h>, the USB
 *  driver interface, implemented on the Windows 98 SE USB stack
 *  (src/obsd_usb.c over src/kexusb_wdm.c).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Status codes and flags follow OpenBSD's usbdi.h. Every callback, task
 *  and timeout of a USB device runs on that device's worker thread, one at
 *  a time, so drivers see the serialization they expect from splusb.
 */
#ifndef KEXBSD_OBSD_USBDI_H
#define KEXBSD_OBSD_USBDI_H

#include "obsd/kexobsd_net.h"
#include "dev/usb/usb.h"
#include "kexbsd/kexusb.h"

typedef enum
{
	USBD_NORMAL_COMPLETION = 0,
	USBD_IN_PROGRESS,
	USBD_PENDING_REQUESTS,
	USBD_NOT_STARTED,
	USBD_INVAL,
	USBD_NOMEM,
	USBD_CANCELLED,
	USBD_BAD_ADDRESS,
	USBD_IN_USE,
	USBD_NO_ADDR,
	USBD_SET_ADDR_FAILED,
	USBD_NO_POWER,
	USBD_TOO_DEEP,
	USBD_IOERROR,
	USBD_NOT_CONFIGURED,
	USBD_TIMEOUT,
	USBD_SHORT_XFER,
	USBD_STALLED,
	USBD_INTERRUPTED,
	USBD_ERROR_MAX
} usbd_status;

struct usbd_xfer;
typedef void (*usbd_callback)(struct usbd_xfer *, void *, usbd_status);

/* usbd_setup_xfer flags */
#define USBD_NO_COPY 0x01
#define USBD_SYNCHRONOUS 0x02
#define USBD_SHORT_XFER_OK 0x04
#define USBD_FORCE_SHORT_XFER 0x08
#define USBD_CATCH 0x10
/* usbd_open_pipe flags */
#define USBD_EXCLUSIVE_USE 0x01
#define USBD_DEFAULT_INTERVAL (-1)
#define USBD_NO_TIMEOUT 0
#define USBD_DEFAULT_TIMEOUT 5000

#define UMATCH_NONE 0
#define UMATCH_GENERIC 1
#define UMATCH_IFACECLASS 2
#define UMATCH_IFACECLASS_IFACESUBCLASS 3
#define UMATCH_IFACECLASS_IFACESUBCLASS_IFACEPROTO 4
#define UMATCH_DEVCLASS_DEVSUBCLASS 5
#define UMATCH_DEVCLASS_DEVSUBCLASS_DEVPROTO 6
#define UMATCH_VENDOR_IFACESUBCLASS 7
#define UMATCH_VENDOR_IFACESUBCLASS_IFACEPROTO 8
#define UMATCH_VENDOR_DEVCLASS_DEVPROTO 9
#define UMATCH_VENDOR_PRODUCT_CONF_IFACE 13
#define UMATCH_VENDOR_PRODUCT_REV 14
#define UMATCH_VENDOR_PRODUCT 13

#define USBD_MAX_IFACES 16
#define USBD_MAX_ALTS 8

struct usbd_device;
struct usbd_interface
{
	struct usbd_device *device;
	int index;                         /* position in the configuration */
	usb_interface_descriptor_t *idesc; /* current alternate setting */
	int altindex;
	int nendpt;
	usb_endpoint_descriptor_t *endpoints[KEXUSB_MAX_PIPES];
	struct kexusb_iface_pipes pipes;   /* handles from the USB stack */
	int claimed;
};

struct usbd_device
{
	kexusb_dev *kdev;
	kexbsd_worker *worker;
	usb_device_descriptor_t ddesc;
	usb_config_descriptor_t *cdesc;    /* the selected configuration, whole */
	unsigned int cdesc_len;
	int config_index;
	int nifaces;
	struct usbd_interface ifaces[USBD_MAX_IFACES];
	volatile int dying;
	int speed;
};

struct usbd_pipe
{
	struct usbd_interface *iface;
	struct usbd_device *device;
	void *handle;
	uint8_t ep;
	uint8_t type;
	int in;
	/* interrupt pipes opened with usbd_open_pipe_intr keep resubmitting */
	struct usbd_xfer *intr_xfer;
};

struct usbd_xfer
{
	struct usbd_device *device;
	struct usbd_pipe *pipe;
	kexusb_xfer *kx;
	void *priv;
	void *buffer;        /* caller's or usbd_alloc_buffer's */
	void *own_buffer;
	unsigned int own_size;
	uint32_t length;
	uint32_t actlen;
	uint16_t flags;
	uint32_t timeout;
	usbd_status status;
	usbd_callback callback;
	struct kexbsd_work work;   /* the callback, queued to the worker */
	KEVENT done;               /* for USBD_SYNCHRONOUS */
	volatile int busy;
};

struct usb_attach_arg
{
	int port;
	int configno;
	int ifaceno;
	uint16_t vendor;
	uint16_t product;
	uint16_t release;
	struct usbd_device *device;
	struct usbd_interface *iface;
	struct usbd_interface **ifaces;
	int nifaces;
};

usbd_status usbd_open_pipe(struct usbd_interface *iface, uint8_t address, uint8_t flags, struct usbd_pipe **pipe);
usbd_status usbd_open_pipe_intr(struct usbd_interface *iface, uint8_t address, uint8_t flags,
                                struct usbd_pipe **pipe, void *priv, void *buffer, uint32_t len,
                                usbd_callback cb, int ival);
usbd_status usbd_close_pipe(struct usbd_pipe *pipe);
void usbd_abort_pipe(struct usbd_pipe *pipe);
usbd_status usbd_clear_endpoint_stall(struct usbd_pipe *pipe);
void usbd_clear_endpoint_stall_async(struct usbd_pipe *pipe);
struct usbd_xfer *usbd_alloc_xfer(struct usbd_device *dev);
void usbd_free_xfer(struct usbd_xfer *xfer);
void *usbd_alloc_buffer(struct usbd_xfer *xfer, uint32_t size);
void usbd_free_buffer(struct usbd_xfer *xfer);
void usbd_setup_xfer(struct usbd_xfer *xfer, struct usbd_pipe *pipe, void *priv, void *buffer, uint32_t length,
                     uint16_t flags, uint32_t timeout, usbd_callback cb);
usbd_status usbd_transfer(struct usbd_xfer *xfer);
void usbd_get_xfer_status(struct usbd_xfer *xfer, void **priv, void **buffer, uint32_t *count,
                          usbd_status *status);
usbd_status usbd_do_request(struct usbd_device *dev, usb_device_request_t *req, void *data);
usbd_status usbd_do_request_flags(struct usbd_device *dev, usb_device_request_t *req, void *data, uint16_t flags,
                                  int *actlen, uint32_t timeout);
usb_interface_descriptor_t *usbd_get_interface_descriptor(struct usbd_interface *iface);
usb_endpoint_descriptor_t *usbd_interface2endpoint_descriptor(struct usbd_interface *iface, uint8_t index);
usb_config_descriptor_t *usbd_get_config_descriptor(struct usbd_device *dev);
usb_device_descriptor_t *usbd_get_device_descriptor(struct usbd_device *dev);
usbd_status usbd_device2interface_handle(struct usbd_device *dev, uint8_t ifaceno, struct usbd_interface **iface);
usbd_status usbd_set_interface(struct usbd_interface *iface, int altidx);
usbd_status usbd_set_config_no(struct usbd_device *dev, int no, int msg);
usbd_status usbd_set_config_index(struct usbd_device *dev, int index, int msg);
int usbd_get_no_alts(usb_config_descriptor_t *cdesc, int ifaceno);
int usbd_iface_claimed(struct usbd_device *dev, int ifaceidx);
void usbd_claim_iface(struct usbd_device *dev, int ifaceidx);
int usbd_is_dying(struct usbd_device *dev);
void usbd_deactivate(struct usbd_device *dev);
const char *usbd_errstr(usbd_status err);
int usbd_ratecheck(struct timeval *last);
#define usbd_ref_incr(dev) ((void) 0)
#define usbd_ref_decr(dev) ((void) 0)
#define usbd_ref_wait(dev) ((void) 0)

/* usb_task: runs on the device's worker */
struct usb_task
{
	struct kexbsd_work work;
	void (*fun)(void *);
	void *arg;
	int type;
};
#define USB_TASK_TYPE_GENERIC 0
#define USB_TASK_TYPE_EXPLORE 1
#define USB_TASK_TYPE_ABORT 2
#define usb_init_task(t, f, a, ty) ((t)->fun = (f), (t)->arg = (a), (t)->type = (ty), (t)->work.queued = 0)
void usb_add_task(struct usbd_device *dev, struct usb_task *task);
void usb_rem_task(struct usbd_device *dev, struct usb_task *task);
#define usb_rem_wait_task(dev, t) usb_rem_task((dev), (t))
#define usb_wait_task(dev, t) ((void) 0)

/* the glue: bring up a device and attach the driver that matches one of
 * its interfaces; returns the matched interface's attach args (in *uaa) */
typedef int (*obsd_usb_match_fn)(struct device *, void *, void *);
typedef void (*obsd_usb_attach_fn)(struct device *, struct device *, void *);
int obsd_usb_attach(PDEVICE_OBJECT lower, kexbsd_worker *worker, obsd_usb_match_fn match,
                    obsd_usb_attach_fn attach, struct device *sc, struct usbd_device **devp);
void obsd_usb_detach(struct usbd_device *dev);

#define UE_GET_DIR(a) ((a) & 0x80)
#define UE_GET_ADDR(a) ((a) & UE_ADDR)

#endif
