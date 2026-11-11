/*
 *  KernelExEx - kexem.sys: NDIS 5 front end for the OpenBSD em driver.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Stage 1 (this file): attaches to Intel 8254x/8257x adapters, brings up
 *  the kexbsd framework, reads the adapter's identity through kexbsd's PCI
 *  and register access, and reports the MAC address and link state to NDIS
 *  so the adapter appears properly in Device Manager and Network. A kexbsd
 *  callout polls the link and a kexbsd task logs it, which exercises the
 *  framework's timers, DPCs and worker threads on real Windows 98.
 *
 *  Stage 2 replaces the stubbed send/receive with OpenBSD if_em.c through
 *  the kexbsd OpenBSD personality.
 */
#include <ndis.h>
#include "kexbsd/bus.h"

#define E1000_STATUS 0x0008
#define E1000_RAL0 0x5400
#define E1000_RAH0 0x5404
#define E1000_STATUS_LU 0x00000002   /* link up */
#define E1000_STATUS_SPEED_MASK 0x000000C0

#define MAX_LOOKAHEAD 1500
#define FRAME_SIZE 1514

typedef struct
{
	NDIS_HANDLE handle;
	device_t dev;
	struct resource *mem;
	UCHAR mac[6];
	int link;          /* 1 = up */
	ULONG speed;       /* in 100 bps units, as NDIS wants */
	struct callout link_poll;
	struct task link_task;
	ULONG packet_filter;
	ULONG lookahead;
	ULONG tx_ok, rx_ok;
} ADAPTER;

static NDIS_HANDLE wrapper;

/* NdisZeroMemory / NdisMoveMemory and struct copies compile to these */
void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char) c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

static const NDIS_OID supported[] = {
	OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
	OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
	OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
	OID_GEN_VENDOR_DESCRIPTION, OID_GEN_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD,
	OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS, OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS,
	OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
	OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
	OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
};

/* ---- link state, through kexbsd's callout and taskqueue ------------------ */

static void link_task_fn(void *arg, int pending)
{
	ADAPTER *a = (ADAPTER *) arg;
	(void) pending;
	device_printf(a->dev, "link %s, %lu Mbit/s\n", a->link ? "up" : "down", a->speed / 10000);
}

static void link_poll_fn(void *arg)
{
	ADAPTER *a = (ADAPTER *) arg;
	uint32_t st = bus_read_4(a->mem, E1000_STATUS);
	int up = (st & E1000_STATUS_LU) != 0;
	static const ULONG speeds[4] = { 100000, 1000000, 10000000, 10000000 };
	ULONG speed = speeds[(st & E1000_STATUS_SPEED_MASK) >> 6];
	if (up != a->link || speed != a->speed)
	{
		a->link = up;
		a->speed = speed;
		NdisMIndicateStatus(a->handle, up ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
		NdisMIndicateStatusComplete(a->handle);
		taskqueue_enqueue(taskqueue_thread, &a->link_task);
	}
	callout_reset(&a->link_poll, hz, link_poll_fn, a);
}

/* ---- NDIS entry points --------------------------------------------------- */

static NDIS_STATUS NTAPI em_initialize(PNDIS_STATUS open_err, PUINT medium_index, PNDIS_MEDIUM media, UINT nmedia,
                                       NDIS_HANDLE handle, NDIS_HANDLE config)
{
	ADAPTER *a;
	UINT i;
	NDIS_STATUS st;
	PNDIS_RESOURCE_LIST res;
	UINT rsize = 0;
	uint32_t ral, rah;
	(void) open_err;
	(void) config;

	for (i = 0; i < nmedia; i++)
		if (media[i] == NdisMedium802_3)
			break;
	if (i == nmedia)
		return NDIS_STATUS_UNSUPPORTED_MEDIA;
	*medium_index = i;

	if (!NT_SUCCESS(kexbsd_framework_init()))
		return NDIS_STATUS_RESOURCES;
	if (NdisAllocateMemoryWithTag((PVOID *) &a, sizeof(*a), 'mexK') != NDIS_STATUS_SUCCESS)
		return NDIS_STATUS_RESOURCES;
	NdisZeroMemory(a, sizeof(*a));
	a->handle = handle;
	a->lookahead = MAX_LOOKAHEAD;
	a->dev = kexbsd_device_create("em", 0, 0);
	if (!a->dev)
		goto fail;

	/* resources Windows assigned: memory BAR(s), I/O BAR, interrupt */
	NdisMQueryAdapterResources(&st, config, NULL, &rsize);
	if (!rsize || NdisAllocateMemoryWithTag((PVOID *) &res, rsize, 'mexK') != NDIS_STATUS_SUCCESS)
		goto fail;
	NdisMQueryAdapterResources(&st, config, res, &rsize);
	if (st != NDIS_STATUS_SUCCESS)
	{
		NdisFreeMemory(res, rsize, 0);
		goto fail;
	}
	for (i = 0; i < res->Count && a->dev->nres < KEXBSD_MAX_RES; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &res->PartialDescriptors[i];
		struct resource *r = &a->dev->res[a->dev->nres];
		if (d->Type == CmResourceTypeMemory)
		{
			r->type = SYS_RES_MEMORY;
			r->start = d->u.Memory.Start.LowPart;
			r->size = d->u.Memory.Length;
			a->dev->nres++;
		}
		else if (d->Type == CmResourceTypePort)
		{
			r->type = SYS_RES_IOPORT;
			r->start = d->u.Port.Start.LowPart;
			r->size = d->u.Port.Length;
			a->dev->nres++;
		}
		else if (d->Type == CmResourceTypeInterrupt)
		{
			r->type = SYS_RES_IRQ;
			r->vector = d->u.Interrupt.Vector;
			r->irql = (KIRQL) d->u.Interrupt.Level;
			r->affinity = d->u.Interrupt.Affinity;
			r->mode = (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
			r->shared = d->ShareDisposition == CmResourceShareShared;
			a->dev->nres++;
		}
	}
	NdisFreeMemory(res, rsize, 0);

	/* PCI identity, through kexbsd (HalGetBusData). NDIS gives us the slot. */
	{
		UCHAR cfg[64];
		ULONG got = NdisReadPciSlotInformation(handle, 0, 0, cfg, sizeof(cfg));
		if (got >= 0x30)
		{
			a->dev->vendor = (uint16_t) (cfg[0] | (cfg[1] << 8));
			a->dev->device = (uint16_t) (cfg[2] | (cfg[3] << 8));
			a->dev->revid = cfg[8];
			a->dev->subvendor = (uint16_t) (cfg[0x2C] | (cfg[0x2D] << 8));
			a->dev->subdevice = (uint16_t) (cfg[0x2E] | (cfg[0x2F] << 8));
		}
	}
	device_printf(a->dev, "Intel %04x:%04x rev %02x, subsystem %04x:%04x, %d resources\n", a->dev->vendor,
	              a->dev->device, a->dev->revid, a->dev->subvendor, a->dev->subdevice, a->dev->nres);

	{
		int rid = PCIR_BAR(0);
		a->mem = bus_alloc_resource_any(a->dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
		if (!a->mem)
		{
			device_printf(a->dev, "cannot map BAR0\n");
			goto fail;
		}
	}

	/* the adapter's address: receive address 0, loaded from the EEPROM at reset */
	ral = bus_read_4(a->mem, E1000_RAL0);
	rah = bus_read_4(a->mem, E1000_RAH0);
	for (i = 0; i < 4; i++)
		a->mac[i] = (UCHAR) (ral >> (8 * i));
	a->mac[4] = (UCHAR) rah;
	a->mac[5] = (UCHAR) (rah >> 8);
	device_printf(a->dev, "address %02x:%02x:%02x:%02x:%02x:%02x\n", a->mac[0], a->mac[1], a->mac[2], a->mac[3],
	              a->mac[4], a->mac[5]);

	NdisMSetAttributesEx(handle, a, 0, NDIS_ATTRIBUTE_BUS_MASTER, NdisInterfacePci);

	TASK_INIT(&a->link_task, 0, link_task_fn, a);
	callout_init(&a->link_poll, 1);
	a->link = -1;
	link_poll_fn(a);
	return NDIS_STATUS_SUCCESS;

fail:
	if (a->dev)
		kexbsd_device_destroy(a->dev);
	NdisFreeMemory(a, sizeof(*a), 0);
	return NDIS_STATUS_ADAPTER_NOT_FOUND;
}

static VOID NTAPI em_halt(NDIS_HANDLE ctx)
{
	ADAPTER *a = (ADAPTER *) ctx;
	callout_drain(&a->link_poll);
	taskqueue_drain(taskqueue_thread, &a->link_task);
	kexbsd_device_destroy(a->dev);
	NdisFreeMemory(a, sizeof(*a), 0);
}

static NDIS_STATUS NTAPI em_reset(PBOOLEAN addressing_reset, NDIS_HANDLE ctx)
{
	(void) ctx;
	*addressing_reset = FALSE;
	return NDIS_STATUS_SUCCESS;
}

/* stage 1: frames are accepted and dropped */
static NDIS_STATUS NTAPI em_send(NDIS_HANDLE ctx, PNDIS_PACKET pkt, UINT flags)
{
	ADAPTER *a = (ADAPTER *) ctx;
	(void) pkt;
	(void) flags;
	a->tx_ok++;
	return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS answer(PVOID buf, ULONG len, PULONG written, PULONG needed, const void *data, ULONG size)
{
	if (len < size)
	{
		*needed = size;
		return NDIS_STATUS_INVALID_LENGTH;
	}
	NdisMoveMemory(buf, data, size);
	*written = size;
	return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NTAPI em_query(NDIS_HANDLE ctx, NDIS_OID oid, PVOID buf, ULONG len, PULONG written,
                                  PULONG needed)
{
	ADAPTER *a = (ADAPTER *) ctx;
	ULONG v = 0;
	static const char desc[] = "Intel PRO/1000 (KernelExEx em)";
	*written = 0;
	*needed = 0;
	switch (oid)
	{
	case OID_GEN_SUPPORTED_LIST:
		return answer(buf, len, written, needed, supported, sizeof(supported));
	case OID_GEN_HARDWARE_STATUS:
		v = NdisHardwareStatusReady;
		break;
	case OID_GEN_MEDIA_SUPPORTED:
	case OID_GEN_MEDIA_IN_USE:
		v = NdisMedium802_3;
		break;
	case OID_GEN_MAXIMUM_LOOKAHEAD:
	case OID_GEN_CURRENT_LOOKAHEAD:
		v = a->lookahead;
		break;
	case OID_GEN_MAXIMUM_FRAME_SIZE:
		v = 1500;
		break;
	case OID_GEN_MAXIMUM_TOTAL_SIZE:
	case OID_GEN_TRANSMIT_BLOCK_SIZE:
	case OID_GEN_RECEIVE_BLOCK_SIZE:
		v = FRAME_SIZE;
		break;
	case OID_GEN_LINK_SPEED:
		v = a->speed ? a->speed : 10000000;
		break;
	case OID_GEN_TRANSMIT_BUFFER_SPACE:
	case OID_GEN_RECEIVE_BUFFER_SPACE:
		v = FRAME_SIZE * 64;
		break;
	case OID_GEN_VENDOR_ID:
		v = (ULONG) a->mac[0] | ((ULONG) a->mac[1] << 8) | ((ULONG) a->mac[2] << 16);
		break;
	case OID_GEN_VENDOR_DESCRIPTION:
		return answer(buf, len, written, needed, desc, sizeof(desc));
	case OID_GEN_DRIVER_VERSION:
		v = 0x0500;
		return answer(buf, len, written, needed, &v, 2);
	case OID_GEN_CURRENT_PACKET_FILTER:
		v = a->packet_filter;
		break;
	case OID_GEN_MAC_OPTIONS:
		v = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_NO_LOOPBACK;
		break;
	case OID_GEN_MEDIA_CONNECT_STATUS:
		v = a->link == 1 ? NdisMediaStateConnected : NdisMediaStateDisconnected;
		break;
	case OID_GEN_MAXIMUM_SEND_PACKETS:
		v = 1;
		break;
	case OID_GEN_XMIT_OK:
		v = a->tx_ok;
		break;
	case OID_GEN_RCV_OK:
		v = a->rx_ok;
		break;
	case OID_GEN_XMIT_ERROR:
	case OID_GEN_RCV_ERROR:
	case OID_GEN_RCV_NO_BUFFER:
	case OID_802_3_RCV_ERROR_ALIGNMENT:
	case OID_802_3_XMIT_ONE_COLLISION:
	case OID_802_3_XMIT_MORE_COLLISIONS:
		v = 0;
		break;
	case OID_802_3_PERMANENT_ADDRESS:
	case OID_802_3_CURRENT_ADDRESS:
		return answer(buf, len, written, needed, a->mac, 6);
	case OID_802_3_MULTICAST_LIST:
		*written = 0;
		return NDIS_STATUS_SUCCESS;
	case OID_802_3_MAXIMUM_LIST_SIZE:
		v = 32;
		break;
	default:
		return NDIS_STATUS_NOT_SUPPORTED;
	}
	return answer(buf, len, written, needed, &v, sizeof(v));
}

static NDIS_STATUS NTAPI em_set(NDIS_HANDLE ctx, NDIS_OID oid, PVOID buf, ULONG len, PULONG read, PULONG needed)
{
	ADAPTER *a = (ADAPTER *) ctx;
	*read = 0;
	*needed = 0;
	switch (oid)
	{
	case OID_GEN_CURRENT_PACKET_FILTER:
		if (len < 4)
		{
			*needed = 4;
			return NDIS_STATUS_INVALID_LENGTH;
		}
		a->packet_filter = *(ULONG *) buf;
		*read = 4;
		return NDIS_STATUS_SUCCESS;
	case OID_GEN_CURRENT_LOOKAHEAD:
		if (len < 4)
		{
			*needed = 4;
			return NDIS_STATUS_INVALID_LENGTH;
		}
		a->lookahead = *(ULONG *) buf;
		*read = 4;
		return NDIS_STATUS_SUCCESS;
	case OID_802_3_MULTICAST_LIST:
		*read = len;
		return NDIS_STATUS_SUCCESS;
	default:
		return NDIS_STATUS_NOT_SUPPORTED;
	}
}

static BOOLEAN NTAPI em_check_hang(NDIS_HANDLE ctx)
{
	(void) ctx;
	return FALSE;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	NDIS_MINIPORT_CHARACTERISTICS mc;
	NDIS_STATUS st;
	NdisMInitializeWrapper(&wrapper, drv, reg, NULL);
	NdisZeroMemory(&mc, sizeof(mc));
	mc.MajorNdisVersion = 5;
	mc.MinorNdisVersion = 0;
	mc.InitializeHandler = em_initialize;
	mc.HaltHandler = em_halt;
	mc.ResetHandler = em_reset;
	mc.SendHandler = em_send;
	mc.QueryInformationHandler = em_query;
	mc.SetInformationHandler = em_set;
	mc.CheckForHangHandler = em_check_hang;
	st = NdisMRegisterMiniport(wrapper, &mc, sizeof(mc));
	if (st != NDIS_STATUS_SUCCESS)
		NdisTerminateWrapper(wrapper, NULL);
	return st;
}
