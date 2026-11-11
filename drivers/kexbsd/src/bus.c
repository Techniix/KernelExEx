/*
 *  KernelExEx - kexbsd.sys: devices, resources, PCI, interrupts, DMA.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#define KEXBSD_BUILD
#include "kexbsd/bus.h"

static void zero(void *p, size_t n)
{
	unsigned char *b = (unsigned char *) p;
	while (n--)
		*b++ = 0;
}

static void copy(void *d, const void *s, size_t n)
{
	unsigned char *a = (unsigned char *) d;
	const unsigned char *b = (const unsigned char *) s;
	while (n--)
		*a++ = *b++;
}

/* ---- devices ------------------------------------------------------------- */

device_t kexbsd_device_create(const char *name, int unit, size_t softc_size)
{
	device_t d = (device_t) kexbsd_malloc(sizeof(*d), M_DEVBUF, M_WAITOK | M_ZERO);
	if (!d)
		return NULL;
	d->name = name;
	d->unit = unit;
	kexbsd_snprintf(d->nameunit, sizeof(d->nameunit), "%s%d", name, unit);
	if (softc_size)
	{
		d->softc = kexbsd_malloc(softc_size, M_DEVBUF, M_WAITOK | M_ZERO);
		if (!d->softc)
		{
			kexbsd_free(d, M_DEVBUF);
			return NULL;
		}
		d->softc_size = softc_size;
	}
	return d;
}

void kexbsd_device_destroy(device_t d)
{
	int i;
	if (!d)
		return;
	for (i = 0; i < d->nres; i++)
		if (d->res[i].va)
			MmUnmapIoSpace((PVOID) d->res[i].va, d->res[i].size);
	kexbsd_free(d->softc, M_DEVBUF);
	kexbsd_free(d, M_DEVBUF);
}

int device_printf(device_t dev, const char *fmt, ...)
{
	char line[240];
	va_list ap;
	va_start(ap, fmt);
	kexbsd_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	return kexbsd_printf("%s: %s", dev->nameunit, line);
}

/* ---- resources -------------------------------------------------------------- *
 * The front end fills dev->res[] from START_DEVICE (memory and port ranges in
 * BAR order, then the interrupt). rid PCIR_BAR(n) picks the n-th range of the
 * requested type; rid 0 for SYS_RES_IRQ picks the interrupt. */

struct resource *bus_alloc_resource_any(device_t dev, int type, int *rid, unsigned flags)
{
	int i, want = 0, seen = 0;
	(void) flags;
	if (type == SYS_RES_MEMORY || type == SYS_RES_IOPORT)
	{
		/* BARs are 4 bytes apart; count every BAR the resource list holds */
		want = rid && *rid >= 0x10 ? (*rid - 0x10) / 4 : 0;
		for (i = 0; i < dev->nres; i++)
		{
			struct resource *r = &dev->res[i];
			if (r->type != SYS_RES_MEMORY && r->type != SYS_RES_IOPORT)
				continue;
			if (seen++ != want)
				continue;
			if (r->type != type)
				return NULL;
			if (type == SYS_RES_MEMORY && !r->va)
			{
				PHYSICAL_ADDRESS pa;
				pa.QuadPart = r->start;
				r->va = (volatile uint8_t *) MmMapIoSpace(pa, r->size, MmNonCached);
				if (!r->va)
					return NULL;
			}
			r->rid = *rid;
			return r;
		}
		return NULL;
	}
	if (type == SYS_RES_IRQ)
		for (i = 0; i < dev->nres; i++)
			if (dev->res[i].type == SYS_RES_IRQ)
				return &dev->res[i];
	return NULL;
}

int bus_release_resource(device_t dev, int type, int rid, struct resource *r)
{
	(void) dev;
	(void) type;
	(void) rid;
	if (r && r->type == SYS_RES_MEMORY && r->va)
	{
		MmUnmapIoSpace((PVOID) r->va, r->size);
		r->va = NULL;
	}
	return 0;
}

/* ---- PCI configuration space ------------------------------------------------- */

static ULONG slot_number(device_t d)
{
	return (d->slot & 0x1F) | ((d->function & 7) << 5);
}

uint32_t pci_read_config(device_t dev, int reg, int width)
{
	unsigned char cfg[256];
	uint32_t v = 0;
	int i;
	if (reg < 0 || reg + width > 256)
		return 0xFFFFFFFFu;
	if (dev->cfg_read)
		return dev->cfg_read(dev, reg, width);
	/* HalGetBusData reads from offset 0; writes go through ...ByOffset */
	if (HalGetBusData(PCIConfiguration, dev->bus, slot_number(dev), cfg, reg + width) < (ULONG) (reg + width))
		return 0xFFFFFFFFu;
	for (i = width - 1; i >= 0; i--)
		v = (v << 8) | cfg[reg + i];
	return v;
}

void pci_write_config(device_t dev, int reg, uint32_t val, int width)
{
	if (dev->cfg_write)
	{
		dev->cfg_write(dev, reg, val, width);
		return;
	}
	HalSetBusDataByOffset(PCIConfiguration, dev->bus, slot_number(dev), &val, (ULONG) reg, (ULONG) width);
}

int pci_enable_busmaster(device_t dev)
{
	uint32_t cmd = pci_read_config(dev, PCIR_COMMAND, 2);
	pci_write_config(dev, PCIR_COMMAND, cmd | PCIM_CMD_BUSMASTEREN, 2);
	return 0;
}

int pci_find_cap(device_t dev, int cap, int *capreg)
{
	int ptr, guard = 48;
	if (!(pci_read_config(dev, 0x06, 2) & 0x10))   /* status: capabilities list */
		return ENXIO;
	ptr = (int) pci_read_config(dev, PCIR_CAP_PTR, 1) & 0xFC;
	while (ptr && guard--)
	{
		if ((int) pci_read_config(dev, ptr, 1) == cap)
		{
			if (capreg)
				*capreg = ptr;
			return 0;
		}
		ptr = (int) pci_read_config(dev, ptr + 1, 1) & 0xFC;
	}
	return ENOENT;
}

/* ---- interrupts ---------------------------------------------------------------- *
 * Windows 9x has line interrupts only, often shared. The BSD driver's filter
 * runs in the ISR (it must quiet the device, as FreeBSD filters do); the
 * handler, if any, runs from a DPC. A driver without a filter is refused:
 * the ISR could not stop a level-triggered line from re-firing. */

typedef struct
{
	PKINTERRUPT obj;
	KDPC dpc;
	driver_filter_t *filter;
	driver_intr_t *handler;
	void *arg;
	volatile LONG count;
} INTR;

static VOID NTAPI intr_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
	INTR *in = (INTR *) ctx;
	(void) dpc;
	(void) a1;
	(void) a2;
	if (in->handler)
		in->handler(in->arg);
}

static BOOLEAN NTAPI intr_isr(PKINTERRUPT obj, PVOID ctx)
{
	INTR *in = (INTR *) ctx;
	int r;
	(void) obj;
	r = in->filter(in->arg);
	if (r & FILTER_STRAY)
		return FALSE;
	in->count++;
	if ((r & FILTER_SCHEDULE_THREAD) && in->handler)
		KeInsertQueueDpc(&in->dpc, NULL, NULL);
	return TRUE;
}

int bus_setup_intr(device_t dev, struct resource *r, int flags, driver_filter_t *filter, driver_intr_t *handler,
                   void *arg, void **cookiep)
{
	INTR *in;
	NTSTATUS st;
	(void) flags;
	if (!r || r->type != SYS_RES_IRQ)
		return EINVAL;
	if (!filter)
	{
		device_printf(dev, "driver has no interrupt filter; not supported on Windows 9x\n");
		return ENOTSUP;
	}
	in = (INTR *) kexbsd_malloc(sizeof(*in), M_DEVBUF, M_WAITOK | M_ZERO);
	if (!in)
		return ENOMEM;
	in->filter = filter;
	in->handler = handler;
	in->arg = arg;
	KeInitializeDpc(&in->dpc, intr_dpc, in);
	st = IoConnectInterrupt(&in->obj, intr_isr, in, NULL, r->vector, r->irql, r->irql, r->mode, r->shared,
	                        r->affinity, FALSE);
	if (!NT_SUCCESS(st))
	{
		device_printf(dev, "IoConnectInterrupt failed (%08x)\n", (unsigned) st);
		kexbsd_free(in, M_DEVBUF);
		return ENXIO;
	}
	*cookiep = in;
	return 0;
}

int bus_teardown_intr(device_t dev, struct resource *r, void *cookie)
{
	INTR *in = (INTR *) cookie;
	(void) dev;
	(void) r;
	if (!in)
		return 0;
	IoDisconnectInterrupt(in->obj);
	KeRemoveQueueDpc(&in->dpc);
	kexbsd_free(in, M_DEVBUF);
	return 0;
}

/* ---- bus_dma(9) ------------------------------------------------------------------ */

struct kexbsd_dma_tag
{
	bus_size_t alignment;
	bus_addr_t boundary;
	bus_addr_t lowaddr;
	bus_size_t maxsize;
	int nsegments;
	bus_size_t maxsegsz;
};

#define MAX_SEGS 64

struct kexbsd_dmamap
{
	/* bus_dmamem_alloc memory */
	void *mem_base;     /* as allocated, before alignment */
	void *mem_va;
	bus_size_t mem_size;
	/* bounce buffer for loads that do not fit the tag */
	void *bounce_va;
	bus_size_t bounce_size;
	void *orig;
	bus_size_t orig_len;
	int bounced;
};

int bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment, bus_addr_t boundary, bus_addr_t lowaddr,
                       bus_addr_t highaddr, void *filter, void *filterarg, bus_size_t maxsize, int nsegments,
                       bus_size_t maxsegsz, int flags, void *lockfunc, void *lockarg, bus_dma_tag_t *dmat)
{
	bus_dma_tag_t t = (bus_dma_tag_t) kexbsd_malloc(sizeof(*t), M_DEVBUF, M_WAITOK | M_ZERO);
	(void) highaddr;
	(void) filter;
	(void) filterarg;
	(void) flags;
	(void) lockfunc;
	(void) lockarg;
	if (!t)
		return ENOMEM;
	t->alignment = alignment ? alignment : 1;
	t->boundary = boundary;
	t->lowaddr = lowaddr;
	t->maxsize = maxsize;
	t->nsegments = nsegments > MAX_SEGS ? MAX_SEGS : nsegments;
	t->maxsegsz = maxsegsz;
	/* inherit the parent's stricter limits */
	if (parent)
	{
		if (parent->lowaddr < t->lowaddr)
			t->lowaddr = parent->lowaddr;
		if (parent->alignment > t->alignment)
			t->alignment = parent->alignment;
	}
	*dmat = t;
	return 0;
}

int bus_dma_tag_destroy(bus_dma_tag_t t)
{
	kexbsd_free(t, M_DEVBUF);
	return 0;
}

static void *contig_alloc(bus_size_t size, bus_addr_t lowaddr, bus_size_t align, void **base)
{
	PHYSICAL_ADDRESS hi;
	unsigned char *p;
	bus_size_t extra = align > PAGE_SIZE ? align : 0;
	hi.QuadPart = lowaddr ? lowaddr : 0xFFFFFFFFu;
	p = (unsigned char *) MmAllocateContiguousMemory(size + extra, hi);
	*base = p;
	if (!p)
		return NULL;
	if (extra)
		p = (unsigned char *) (((ULONG_PTR) p + align - 1) & ~(ULONG_PTR) (align - 1));
	return p;
}

int bus_dmamap_create(bus_dma_tag_t t, int flags, bus_dmamap_t *mapp)
{
	bus_dmamap_t m = (bus_dmamap_t) kexbsd_malloc(sizeof(*m), M_DEVBUF, M_WAITOK | M_ZERO);
	(void) t;
	(void) flags;
	if (!m)
		return ENOMEM;
	*mapp = m;
	return 0;
}

int bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t m)
{
	(void) t;
	if (!m)
		return 0;
	if (m->bounce_va)
		MmFreeContiguousMemory(m->bounce_va);
	kexbsd_free(m, M_DEVBUF);
	return 0;
}

int bus_dmamem_alloc(bus_dma_tag_t t, void **vaddr, int flags, bus_dmamap_t *mapp)
{
	bus_dmamap_t m;
	int e = bus_dmamap_create(t, flags, &m);
	if (e)
		return e;
	m->mem_va = contig_alloc(t->maxsize, t->lowaddr, t->alignment, &m->mem_base);
	if (!m->mem_va)
	{
		kexbsd_free(m, M_DEVBUF);
		return ENOMEM;
	}
	m->mem_size = t->maxsize;
	if (flags & BUS_DMA_ZERO)
		zero(m->mem_va, t->maxsize);
	*vaddr = m->mem_va;
	*mapp = m;
	return 0;
}

void bus_dmamem_free(bus_dma_tag_t t, void *vaddr, bus_dmamap_t m)
{
	(void) vaddr;
	if (m && m->mem_base)
		MmFreeContiguousMemory(m->mem_base);
	if (m)
	{
		m->mem_base = NULL;
		bus_dmamap_destroy(t, m);
	}
}

/* split a virtual range into physically contiguous segments */
static int segments(bus_dma_tag_t t, void *buf, bus_size_t len, bus_dma_segment_t *seg)
{
	ULONG_PTR va = (ULONG_PTR) buf;
	int n = 0;
	while (len)
	{
		PHYSICAL_ADDRESS pa = MmGetPhysicalAddress((PVOID) va);
		bus_size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
		if (chunk > len)
			chunk = len;
		if (pa.HighPart || (t->lowaddr && pa.LowPart + chunk - 1 > t->lowaddr))
			return -1;
		if (n && seg[n - 1].ds_addr + seg[n - 1].ds_len == pa.LowPart &&
		    (!t->maxsegsz || seg[n - 1].ds_len + chunk <= t->maxsegsz) &&
		    (!t->boundary || (seg[n - 1].ds_addr & ~(t->boundary - 1)) == ((pa.LowPart + chunk - 1) & ~(t->boundary - 1))))
			seg[n - 1].ds_len += chunk;
		else
		{
			if (n == t->nsegments)
				return -1;
			seg[n].ds_addr = pa.LowPart;
			seg[n].ds_len = chunk;
			n++;
		}
		if (n == 1 && (seg[0].ds_addr & (t->alignment - 1)))
			return -1;
		va += chunk;
		len -= chunk;
	}
	return n;
}

int bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t m, void *buf, bus_size_t len, bus_dmamap_callback_t *cb, void *arg,
                    int flags)
{
	bus_dma_segment_t seg[MAX_SEGS];
	int n;
	(void) flags;
	if (len > t->maxsize)
	{
		cb(arg, seg, 0, EFBIG);
		return EINVAL;
	}
	n = segments(t, buf, len, seg);
	m->bounced = 0;
	if (n < 0)
	{
		/* does not fit: one contiguous bounce buffer inside the tag's limits */
		if (!m->bounce_va || m->bounce_size < len)
		{
			void *base;
			if (m->bounce_va)
				MmFreeContiguousMemory(m->bounce_va);
			m->bounce_va = contig_alloc(t->maxsize, t->lowaddr, PAGE_SIZE, &base);
			m->bounce_size = m->bounce_va ? t->maxsize : 0;
			if (!m->bounce_va)
			{
				cb(arg, seg, 0, ENOMEM);
				return ENOMEM;
			}
		}
		m->orig = buf;
		m->orig_len = len;
		m->bounced = 1;
		n = segments(t, m->bounce_va, len, seg);
		if (n < 0)
		{
			cb(arg, seg, 0, EINVAL);
			return EINVAL;
		}
	}
	cb(arg, seg, n, 0);
	return 0;
}

void bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t m)
{
	(void) t;
	m->bounced = 0;
	m->orig = NULL;
}

void bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t m, int op)
{
	(void) t;
	/* x86 DMA is cache-coherent; only bounce buffers need copying */
	if (!m || !m->bounced)
	{
		__sync_synchronize();
		return;
	}
	if (op & BUS_DMASYNC_PREWRITE)
		copy(m->bounce_va, m->orig, m->orig_len);
	if (op & BUS_DMASYNC_POSTREAD)
		copy(m->orig, m->bounce_va, m->orig_len);
	__sync_synchronize();
}
