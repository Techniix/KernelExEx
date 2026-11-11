/* KernelExEx: the parts of FreeBSD's struct usb_device that drivers look at,
 * filled in by kexfbsd_usb.c from the configuration descriptor. GPLv2. */
#ifndef KEXFBSD_USB_DEVICE_H
#define KEXFBSD_USB_DEVICE_H
#include <dev/usb/usb.h>
#define KEXFBSD_USB_MAX_EP 32
/* struct usb_endpoint is usbdi.h's; we use edesc and iface_index */
struct usb_device
{
	struct usb_endpoint endpoints[KEXFBSD_USB_MAX_EP];
	uint8_t endpoints_max;
	struct usb_device_descriptor ddesc;
	enum usb_dev_speed speed;
	void *pipe[KEXFBSD_USB_MAX_EP];   /* USBD pipe handles, by endpoints[] index */
	void *kexusb;   /* kexusb_dev * */
	void *worker;   /* the USB callback thread (kexbsd_worker *) */
	uint8_t *cdesc;
	struct usb_interface_descriptor *idesc[8];
	uint8_t nifaces;
};
#endif
