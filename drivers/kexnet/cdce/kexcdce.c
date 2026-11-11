/*
 *  KernelExEx - kexcdce.sys: USB CDC Ethernet (ECM) network adapters for
 *  Windows 98 SE and ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Written for KernelExEx from the USB Class Definitions for Communications
 *  Devices 1.2 and its ECM subclass specification (not derived from any
 *  BSD or Linux driver). It is an OpenBSD-style driver so it runs on the
 *  kexbsd USB layer and the kexnet NDIS glue, like urndis does.
 *
 *  ECM in short: a Communications interface (class 2, subclass 6) carries
 *  functional descriptors (Union: which interface carries data; Ethernet
 *  Networking: MAC address string, segment size) and an optional interrupt
 *  endpoint for notifications. The Data interface (class 0x0A) has an
 *  alternate setting with no endpoints (traffic off) and one with a bulk
 *  pair (traffic on). Frames go raw over bulk, one per transfer, a transfer
 *  that fills whole packets ended by a zero-length packet.
 */
#include "obsd/kexobsd_net.h"
#include <dev/usb/usbdi.h>
#include "kexnet.h"

#define UISUBCLASS_ECM 6
#define UICLASS_CDC_DATA 0x0a
#define CDC_CS_INTERFACE 0x24
#define CDC_UNION 0x06
#define CDC_ETHERNET 0x0f
#define CDC_SET_ETHERNET_PACKET_FILTER 0x43
#define CDC_NOTIFY_NETWORK_CONNECTION 0x00
#define CDC_NOTIFY_SPEED_CHANGE 0x2a
/* packet filter bits (ECM 1.2, 6.2.4) */
#define ECM_PROMISCUOUS 0x01
#define ECM_ALL_MULTICAST 0x02
#define ECM_DIRECTED 0x04
#define ECM_BROADCAST 0x08
#define ECM_MULTICAST 0x10

#define RX_LIST 4
#define TX_LIST 4
#define BUFSZ 2048

struct cdce_chain
{
	struct cdce_softc *sc;
	struct usbd_xfer *xfer;
	char *buf;
	int busy;
};

struct cdce_softc
{
	struct device sc_dev;
	struct arpcom sc_ac;
	struct usbd_device *udev;
	struct usbd_interface *ctl, *data;
	int ctl_no, data_no;
	int data_alt;            /* the alternate setting with the bulk pair */
	int bulkin, bulkout, intr;
	struct usbd_pipe *in_pipe, *out_pipe, *intr_pipe;
	struct cdce_chain rx[RX_LIST], tx[TX_LIST];
	uint8_t intr_buf[16];
	unsigned int maxseg;
	int attached;
};

#define GET_IFP(sc) (&(sc)->sc_ac.ac_if)

int cdce_match(struct device *, void *, void *);
void cdce_attach(struct device *, struct device *, void *);
static int cdce_ioctl(struct ifnet *, u_long, caddr_t);
static void cdce_start(struct ifnet *);
static void cdce_init(struct cdce_softc *);
static void cdce_stop(struct cdce_softc *);

int cdce_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa = aux;
	usb_interface_descriptor_t *id;
	(void) parent;
	(void) match;
	if (!uaa->iface || !(id = usbd_get_interface_descriptor(uaa->iface)))
		return UMATCH_NONE;
	if (id->bInterfaceClass == UICLASS_CDC && id->bInterfaceSubClass == UISUBCLASS_ECM)
		return UMATCH_IFACECLASS_IFACESUBCLASS;
	return UMATCH_NONE;
}

/* the class-specific descriptors that follow the communications interface */
static uint8_t *find_func(struct cdce_softc *sc, uint8_t subtype)
{
	usb_config_descriptor_t *cd = usbd_get_config_descriptor(sc->udev);
	uint8_t *p = (uint8_t *) cd, *end = p + UGETW(cd->wTotalLength);
	int in_ctl = 0;
	for (; p + 3 <= end && p[0] >= 2; p += p[0])
	{
		if (p[1] == UDESC_INTERFACE)
			in_ctl = ((usb_interface_descriptor_t *) p)->bInterfaceNumber == sc->ctl_no;
		else if (in_ctl && p[1] == CDC_CS_INTERFACE && p[2] == subtype)
			return p;
	}
	return NULL;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* iMACAddress: a string of 12 hex digits */
static int read_mac(struct cdce_softc *sc, int index, uint8_t *mac)
{
	usb_string_descriptor_t sd;
	int len = 0, i;
	if (!index || usbd_get_string_desc(sc->udev, index, 0x0409, &sd, &len) || len < 2 + 24)
		return -1;
	for (i = 0; i < 6; i++)
	{
		int hi = hexval(UGETW(sd.bString[2 * i])), lo = hexval(UGETW(sd.bString[2 * i + 1]));
		if (hi < 0 || lo < 0)
			return -1;
		mac[i] = (uint8_t) (hi << 4 | lo);
	}
	return 0;
}

void cdce_attach(struct device *parent, struct device *self, void *aux)
{
	struct cdce_softc *sc = (struct cdce_softc *) self;
	struct usb_attach_arg *uaa = aux;
	struct ifnet *ifp = GET_IFP(sc);
	uint8_t *u, *eth;
	int i, j, nalts;
	(void) parent;

	sc->udev = uaa->device;
	sc->ctl = uaa->iface;
	sc->ctl_no = usbd_get_interface_descriptor(sc->ctl)->bInterfaceNumber;
	sc->bulkin = sc->bulkout = sc->intr = -1;

	u = find_func(sc, CDC_UNION);
	eth = find_func(sc, CDC_ETHERNET);
	if (!u || u[0] < 5 || !eth || eth[0] < 13)
	{
		printf(": no union or Ethernet functional descriptor\n");
		return;
	}
	sc->data_no = u[4];   /* bSlaveInterface0 */
	if (usbd_device2interface_handle(sc->udev, (uint8_t) sc->data_no, &sc->data))
	{
		printf(": no data interface %d\n", sc->data_no);
		return;
	}
	for (i = 0; i < uaa->nifaces; i++)
		if (uaa->ifaces[i] == sc->data)
			usbd_claim_iface(sc->udev, i);
	sc->maxseg = UGETW(eth + 8);
	if (sc->maxseg < ETHER_MAX_LEN - ETHER_CRC_LEN || sc->maxseg > BUFSZ)
		sc->maxseg = ETHER_MAX_LEN - ETHER_CRC_LEN;

	/* notifications, if the device has the endpoint */
	for (i = 0; i < usbd_get_interface_descriptor(sc->ctl)->bNumEndpoints; i++)
	{
		usb_endpoint_descriptor_t *ed = usbd_interface2endpoint_descriptor(sc->ctl, (uint8_t) i);
		if (ed && UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN && UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT)
			sc->intr = ed->bEndpointAddress;
	}

	/* the data alternate setting with a bulk pair */
	nalts = usbd_get_no_alts(usbd_get_config_descriptor(sc->udev), sc->data_no);
	for (j = 0; j < nalts && sc->bulkin < 0; j++)
	{
		if (usbd_set_interface(sc->data, j))
			continue;
		sc->bulkout = -1;
		for (i = 0; i < usbd_get_interface_descriptor(sc->data)->bNumEndpoints; i++)
		{
			usb_endpoint_descriptor_t *ed = usbd_interface2endpoint_descriptor(sc->data, (uint8_t) i);
			if (!ed || UE_GET_XFERTYPE(ed->bmAttributes) != UE_BULK)
				continue;
			if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN)
				sc->bulkin = ed->bEndpointAddress;
			else
				sc->bulkout = ed->bEndpointAddress;
		}
		if (sc->bulkin >= 0 && sc->bulkout >= 0)
			sc->data_alt = j;
		else
			sc->bulkin = -1;
	}
	usbd_set_interface(sc->data, 0);   /* traffic off until the interface comes up */
	if (sc->bulkin < 0)
	{
		printf(": no bulk endpoints\n");
		return;
	}

	if (read_mac(sc, eth[3], sc->sc_ac.ac_enaddr))
	{
		/* a locally administered address rather than none */
		static const uint8_t base[6] = { 0x02, 0x4b, 0x45, 0x58, 0x00, 0x00 };
		memcpy(sc->sc_ac.ac_enaddr, base, 6);
		sc->sc_ac.ac_enaddr[5] = (uint8_t) sc->sc_dev.dv_xname[strlen(sc->sc_dev.dv_xname) - 1];
		printf(", no MAC address string; using");
	}
	printf(", address %s\n", ether_sprintf(sc->sc_ac.ac_enaddr));

	ifp->if_softc = sc;
	strlcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = cdce_ioctl;
	ifp->if_start = cdce_start;
	if_attach(ifp);
	ether_ifattach(ifp);
	sc->attached = 1;
}

static void set_filter(struct cdce_softc *sc)
{
	struct ifnet *ifp = GET_IFP(sc);
	usb_device_request_t req;
	uint16_t f = ECM_DIRECTED | ECM_BROADCAST | ECM_ALL_MULTICAST;
	if (ifp->if_flags & IFF_PROMISC)
		f |= ECM_PROMISCUOUS;
	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = CDC_SET_ETHERNET_PACKET_FILTER;
	USETW(req.wValue, f);
	USETW(req.wIndex, sc->ctl_no);
	USETW(req.wLength, 0);
	usbd_do_request(sc->udev, &req, NULL);   /* optional for devices: ignore a stall */
}

static void cdce_rxeof(struct usbd_xfer *xfer, void *priv, usbd_status status);

static void rx_submit(struct cdce_chain *c)
{
	usbd_setup_xfer(c->xfer, c->sc->in_pipe, c, c->buf, BUFSZ, USBD_SHORT_XFER_OK | USBD_NO_COPY, USBD_NO_TIMEOUT,
	                cdce_rxeof);
	usbd_transfer(c->xfer);
}

static void cdce_rxeof(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct cdce_chain *c = priv;
	struct cdce_softc *sc = c->sc;
	struct ifnet *ifp = GET_IFP(sc);
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	uint32_t len;
	if (status == USBD_CANCELLED || usbd_is_dying(sc->udev) || !(ifp->if_flags & IFF_RUNNING))
		return;
	if (status != USBD_NORMAL_COMPLETION)
	{
		ifp->if_ierrors++;
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->in_pipe);
		rx_submit(c);
		return;
	}
	usbd_get_xfer_status(xfer, NULL, NULL, &len, NULL);
	if (len >= ETHER_HDR_LEN && len <= ETHER_MAX_LEN)
	{
		struct mbuf *m = m_devget(c->buf, (int) len, ETHER_ALIGN);
		if (m)
		{
			ml_enqueue(&ml, m);
			if_input(ifp, &ml);
		}
		else
			ifp->if_iqdrops++;
	}
	else
		ifp->if_ierrors++;
	rx_submit(c);
}

static void cdce_txeof(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct cdce_chain *c = priv;
	struct cdce_softc *sc = c->sc;
	struct ifnet *ifp = GET_IFP(sc);
	(void) xfer;
	c->busy = 0;
	if (status == USBD_CANCELLED || usbd_is_dying(sc->udev))
		return;
	if (status != USBD_NORMAL_COMPLETION)
	{
		ifp->if_oerrors++;
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->out_pipe);
	}
	else
		ifp->if_opackets++;
	ifq_clr_oactive(&ifp->if_snd);
	cdce_start(ifp);
}

static void cdce_start(struct ifnet *ifp)
{
	struct cdce_softc *sc = ifp->if_softc;
	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
		return;
	while (!ifq_empty(&ifp->if_snd))
	{
		struct cdce_chain *c = NULL;
		struct mbuf *m;
		int i, len;
		for (i = 0; i < TX_LIST; i++)
			if (!sc->tx[i].busy)
			{
				c = &sc->tx[i];
				break;
			}
		if (!c)
		{
			ifq_set_oactive(&ifp->if_snd);
			return;
		}
		m = ifq_dequeue(&ifp->if_snd);
		len = m->m_pkthdr.len;
		if (len > (int) sc->maxseg)
		{
			m_freem(m);
			ifp->if_oerrors++;
			continue;
		}
		m_copydata(m, 0, len, c->buf);
		m_freem(m);
		c->busy = 1;
		usbd_setup_xfer(c->xfer, sc->out_pipe, c, c->buf, (uint32_t) len, USBD_FORCE_SHORT_XFER | USBD_NO_COPY,
		                10000, cdce_txeof);
		if (usbd_transfer(c->xfer) != USBD_IN_PROGRESS)
		{
			c->busy = 0;
			ifp->if_oerrors++;
		}
	}
}

/* NETWORK_CONNECTION and CONNECTION_SPEED_CHANGE (CDC 1.2, 6.3) */
static void cdce_intr(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct cdce_softc *sc = priv;
	struct ifnet *ifp = GET_IFP(sc);
	uint8_t *n = sc->intr_buf;
	uint32_t len;
	if (status != USBD_NORMAL_COMPLETION)
	{
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->intr_pipe);
		return;
	}
	usbd_get_xfer_status(xfer, NULL, NULL, &len, NULL);
	if (len < 8 || n[0] != (UT_READ_CLASS_INTERFACE))
		return;
	if (n[1] == CDC_NOTIFY_NETWORK_CONNECTION)
	{
		int st = (n[2] | n[3] << 8) ? LINK_STATE_UP : LINK_STATE_DOWN;
		if (st != ifp->if_link_state)
		{
			ifp->if_link_state = st;
			if_link_state_change(ifp);
		}
	}
	else if (n[1] == CDC_NOTIFY_SPEED_CHANGE && len >= 16)
		ifp->if_baudrate = (uint64_t) (n[8] | n[9] << 8 | n[10] << 16 | (uint32_t) n[11] << 24);
}

static void cdce_init(struct cdce_softc *sc)
{
	struct ifnet *ifp = GET_IFP(sc);
	int i;
	if (usbd_set_interface(sc->data, sc->data_alt))
	{
		printf("%s: cannot select the data alternate setting\n", sc->sc_dev.dv_xname);
		return;
	}
	if (usbd_open_pipe(sc->data, (uint8_t) sc->bulkin, USBD_EXCLUSIVE_USE, &sc->in_pipe) ||
	    usbd_open_pipe(sc->data, (uint8_t) sc->bulkout, USBD_EXCLUSIVE_USE, &sc->out_pipe))
	{
		printf("%s: cannot open the bulk pipes\n", sc->sc_dev.dv_xname);
		cdce_stop(sc);
		return;
	}
	if (sc->intr >= 0)
		usbd_open_pipe_intr(sc->ctl, (uint8_t) sc->intr, USBD_SHORT_XFER_OK, &sc->intr_pipe, sc, sc->intr_buf,
		                    sizeof(sc->intr_buf), cdce_intr, USBD_DEFAULT_INTERVAL);
	for (i = 0; i < RX_LIST; i++)
	{
		struct cdce_chain *c = &sc->rx[i];
		c->sc = sc;
		if (!c->xfer && !(c->xfer = usbd_alloc_xfer(sc->udev)))
			break;
		if (!c->buf && !(c->buf = usbd_alloc_buffer(c->xfer, BUFSZ)))
			break;
	}
	for (i = 0; i < TX_LIST; i++)
	{
		struct cdce_chain *c = &sc->tx[i];
		c->sc = sc;
		c->busy = 0;
		if (!c->xfer && !(c->xfer = usbd_alloc_xfer(sc->udev)))
			break;
		if (!c->buf && !(c->buf = usbd_alloc_buffer(c->xfer, BUFSZ)))
			break;
	}
	set_filter(sc);
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	for (i = 0; i < RX_LIST; i++)
		if (sc->rx[i].buf)
			rx_submit(&sc->rx[i]);
}

static void cdce_stop(struct cdce_softc *sc)
{
	struct ifnet *ifp = GET_IFP(sc);
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	if (sc->intr_pipe)
	{
		usbd_close_pipe(sc->intr_pipe);
		sc->intr_pipe = NULL;
	}
	if (sc->in_pipe)
	{
		usbd_close_pipe(sc->in_pipe);
		sc->in_pipe = NULL;
	}
	if (sc->out_pipe)
	{
		usbd_close_pipe(sc->out_pipe);
		sc->out_pipe = NULL;
	}
	usbd_set_interface(sc->data, 0);
}

static int cdce_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct cdce_softc *sc = ifp->if_softc;
	(void) data;
	if (usbd_is_dying(sc->udev))
		return ENXIO;
	switch (cmd)
	{
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
		{
			if (ifp->if_flags & IFF_RUNNING)
				set_filter(sc);
			else
				cdce_init(sc);
		}
		else if (ifp->if_flags & IFF_RUNNING)
			cdce_stop(sc);
		return 0;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;   /* all multicast is on (ECM multicast filters are optional) */
	default:
		return ENOTTY;
	}
}

static const struct kexnet_driver cdce_driver = {
	"cdce",
	"USB CDC Ethernet adapter (KernelExEx)",
	sizeof(struct cdce_softc),
	cdce_match,
	cdce_attach,
	NULL,
	NULL,
	NULL,
	1,
};

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	return kexnet_register(drv, reg, &cdce_driver);
}
