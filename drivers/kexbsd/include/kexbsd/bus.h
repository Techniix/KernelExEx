/*
 *  KernelExEx - kexbsd: bus_space(9), bus_dma(9), PCI and interrupts.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  A front end (NDIS miniport, IOS port driver, ...) creates the device_t
 *  from the resources Windows gives it at START_DEVICE and hands it to the
 *  BSD driver's probe and attach.
 */
#ifndef KEXBSD_BUS_H
#define KEXBSD_BUS_H

#include "kexbsd/kern.h"

/* ---- device_t ------------------------------------------------------------ */
#define KEXBSD_MAX_RES 8

struct resource
{
	int type;            /* SYS_RES_MEMORY, SYS_RES_IOPORT, SYS_RES_IRQ */
	int rid;
	uint32_t start;      /* physical address, port or vector */
	uint32_t size;
	volatile uint8_t *va;   /* mapped memory */
	/* interrupt */
	ULONG vector;
	KIRQL irql;
	KAFFINITY affinity;
	KINTERRUPT_MODE mode;
	BOOLEAN shared;
};

struct kexbsd_device
{
	const char *name;      /* "re" */
	int unit;
	char nameunit[16];     /* "re0" */
	const char *desc;
	void *softc;
	size_t softc_size;
	/* PCI location and IDs */
	ULONG bus, slot, function;
	uint16_t vendor, device, subvendor, subdevice;
	uint8_t revid, class_, subclass, progif;
	/* resources from START_DEVICE */
	struct resource res[KEXBSD_MAX_RES];
	int nres;
	void *frontend;        /* the front end's own context */
};
typedef struct kexbsd_device *device_t;

KEXBSD_API device_t kexbsd_device_create(const char *name, int unit, size_t softc_size);
KEXBSD_API void kexbsd_device_destroy(device_t dev);
#define device_get_softc(d) ((d)->softc)
#define device_get_unit(d) ((d)->unit)
#define device_get_name(d) ((d)->name)
#define device_get_nameunit(d) ((d)->nameunit)
#define device_set_desc(d, s) ((d)->desc = (s))
#define device_set_desc_copy(d, s) ((d)->desc = (s))
#define device_get_desc(d) ((d)->desc)
KEXBSD_API int device_printf(device_t dev, const char *fmt, ...);

/* probe results */
#define BUS_PROBE_SPECIFIC 0
#define BUS_PROBE_VENDOR (-10)
#define BUS_PROBE_DEFAULT (-20)
#define BUS_PROBE_GENERIC (-100)

/* ---- resources ----------------------------------------------------------- */
#define SYS_RES_IRQ 1
#define SYS_RES_IOPORT 4
#define SYS_RES_MEMORY 3
#define RF_ACTIVE 0x0002
#define RF_SHAREABLE 0x0004

KEXBSD_API struct resource *bus_alloc_resource_any(device_t dev, int type, int *rid, unsigned flags);
KEXBSD_API int bus_release_resource(device_t dev, int type, int rid, struct resource *r);
#define rman_get_start(r) ((r)->start)
#define rman_get_size(r) ((r)->size)
#define rman_get_rid(r) ((r)->rid)

/* PCI BARs: rid = PCIR_BAR(n) */
#define PCIR_BAR(n) (0x10 + (n) * 4)

/* ---- bus_space(9) --------------------------------------------------------- */
typedef int bus_space_tag_t;            /* 0 = I/O ports, 1 = memory */
typedef uintptr_t bus_space_handle_t;
typedef uint32_t bus_size_t;
typedef uint32_t bus_addr_t;
#define X86_BUS_SPACE_IO 0
#define X86_BUS_SPACE_MEM 1

#define rman_get_bustag(r) ((r)->type == SYS_RES_IOPORT ? X86_BUS_SPACE_IO : X86_BUS_SPACE_MEM)
#define rman_get_bushandle(r) ((r)->type == SYS_RES_IOPORT ? (bus_space_handle_t) (r)->start : (bus_space_handle_t) (r)->va)

static __inline__ uint8_t bus_space_read_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	return t == X86_BUS_SPACE_IO ? READ_PORT_UCHAR((PUCHAR) (h + o)) : *(volatile uint8_t *) (h + o);
}
static __inline__ uint16_t bus_space_read_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	return t == X86_BUS_SPACE_IO ? READ_PORT_USHORT((PUSHORT) (h + o)) : *(volatile uint16_t *) (h + o);
}
static __inline__ uint32_t bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	return t == X86_BUS_SPACE_IO ? READ_PORT_ULONG((PULONG) (h + o)) : *(volatile uint32_t *) (h + o);
}
static __inline__ void bus_space_write_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint8_t v)
{
	if (t == X86_BUS_SPACE_IO)
		WRITE_PORT_UCHAR((PUCHAR) (h + o), v);
	else
		*(volatile uint8_t *) (h + o) = v;
}
static __inline__ void bus_space_write_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint16_t v)
{
	if (t == X86_BUS_SPACE_IO)
		WRITE_PORT_USHORT((PUSHORT) (h + o), v);
	else
		*(volatile uint16_t *) (h + o) = v;
}
static __inline__ void bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint32_t v)
{
	if (t == X86_BUS_SPACE_IO)
		WRITE_PORT_ULONG((PULONG) (h + o), v);
	else
		*(volatile uint32_t *) (h + o) = v;
}
#define bus_space_barrier(t, h, o, l, f) __sync_synchronize()
#define BUS_SPACE_BARRIER_READ 1
#define BUS_SPACE_BARRIER_WRITE 2

/* bus_read_N(res, offset), as most modern drivers use */
#define bus_read_1(r, o) bus_space_read_1(rman_get_bustag(r), rman_get_bushandle(r), (o))
#define bus_read_2(r, o) bus_space_read_2(rman_get_bustag(r), rman_get_bushandle(r), (o))
#define bus_read_4(r, o) bus_space_read_4(rman_get_bustag(r), rman_get_bushandle(r), (o))
#define bus_write_1(r, o, v) bus_space_write_1(rman_get_bustag(r), rman_get_bushandle(r), (o), (v))
#define bus_write_2(r, o, v) bus_space_write_2(rman_get_bustag(r), rman_get_bushandle(r), (o), (v))
#define bus_write_4(r, o, v) bus_space_write_4(rman_get_bustag(r), rman_get_bushandle(r), (o), (v))
#define bus_barrier(r, o, l, f) __sync_synchronize()

/* ---- PCI configuration space ------------------------------------------------ */
KEXBSD_API uint32_t pci_read_config(device_t dev, int reg, int width);
KEXBSD_API void pci_write_config(device_t dev, int reg, uint32_t val, int width);
KEXBSD_API int pci_enable_busmaster(device_t dev);
KEXBSD_API int pci_find_cap(device_t dev, int cap, int *capreg);
#define pci_get_vendor(d) ((d)->vendor)
#define pci_get_device(d) ((d)->device)
#define pci_get_subvendor(d) ((d)->subvendor)
#define pci_get_subdevice(d) ((d)->subdevice)
#define pci_get_revid(d) ((d)->revid)
#define pci_get_class(d) ((d)->class_)
#define pci_get_subclass(d) ((d)->subclass)
#define pci_get_progif(d) ((d)->progif)
#define pci_msi_count(d) 0      /* Windows 9x: line interrupts only */
#define pci_msix_count(d) 0
#define PCIR_COMMAND 0x04
#define PCIM_CMD_PORTEN 0x0001
#define PCIM_CMD_MEMEN 0x0002
#define PCIM_CMD_BUSMASTEREN 0x0004
#define PCIR_CAP_PTR 0x34
#define PCIY_PMG 0x01
#define PCIY_EXPRESS 0x10

/* ---- interrupts --------------------------------------------------------------- */
typedef int driver_filter_t(void *arg);
typedef void driver_intr_t(void *arg);
#define FILTER_STRAY 0x01
#define FILTER_HANDLED 0x02
#define FILTER_SCHEDULE_THREAD 0x04
#define INTR_TYPE_NET 4
#define INTR_TYPE_BIO 8
#define INTR_TYPE_AV 16
#define INTR_TYPE_MISC 32
#define INTR_MPSAFE 512

KEXBSD_API int bus_setup_intr(device_t dev, struct resource *r, int flags, driver_filter_t *filter,
                              driver_intr_t *handler, void *arg, void **cookiep);
KEXBSD_API int bus_teardown_intr(device_t dev, struct resource *r, void *cookie);

/* ---- bus_dma(9) ---------------------------------------------------------------- */
typedef struct kexbsd_dma_tag *bus_dma_tag_t;
typedef struct kexbsd_dmamap *bus_dmamap_t;
typedef struct
{
	bus_addr_t ds_addr;
	bus_size_t ds_len;
} bus_dma_segment_t;
typedef void bus_dmamap_callback_t(void *arg, bus_dma_segment_t *segs, int nseg, int error);

#define BUS_SPACE_MAXADDR 0xFFFFFFFFu
#define BUS_SPACE_MAXADDR_32BIT 0xFFFFFFFFu
#define BUS_SPACE_MAXSIZE_32BIT 0xFFFFFFFFu
#define BUS_SPACE_UNRESTRICTED (~0)
#define BUS_DMA_WAITOK 0x00
#define BUS_DMA_NOWAIT 0x01
#define BUS_DMA_ALLOCNOW 0x02
#define BUS_DMA_COHERENT 0x04
#define BUS_DMA_ZERO 0x08
#define BUS_DMASYNC_PREREAD 1
#define BUS_DMASYNC_POSTREAD 2
#define BUS_DMASYNC_PREWRITE 4
#define BUS_DMASYNC_POSTWRITE 8
#define bus_get_dma_tag(dev) ((bus_dma_tag_t) NULL)

KEXBSD_API int bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment, bus_addr_t boundary,
                                  bus_addr_t lowaddr, bus_addr_t highaddr, void *filter, void *filterarg,
                                  bus_size_t maxsize, int nsegments, bus_size_t maxsegsz, int flags,
                                  void *lockfunc, void *lockarg, bus_dma_tag_t *dmat);
KEXBSD_API int bus_dma_tag_destroy(bus_dma_tag_t t);
KEXBSD_API int bus_dmamem_alloc(bus_dma_tag_t t, void **vaddr, int flags, bus_dmamap_t *mapp);
KEXBSD_API void bus_dmamem_free(bus_dma_tag_t t, void *vaddr, bus_dmamap_t map);
KEXBSD_API int bus_dmamap_create(bus_dma_tag_t t, int flags, bus_dmamap_t *mapp);
KEXBSD_API int bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map);
KEXBSD_API int bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf, bus_size_t len,
                               bus_dmamap_callback_t *cb, void *arg, int flags);
KEXBSD_API void bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map);
KEXBSD_API void bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t map, int op);

#endif
