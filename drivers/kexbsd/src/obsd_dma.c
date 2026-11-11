/*
 *  KernelExEx - kexbsd.sys: bus_dma(9) for the OpenBSD personality.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  OpenBSD/NetBSD bus_dma: maps carry their segments (dm_segs), memory is
 *  allocated as segments and mapped separately. On a 32-bit 9x machine
 *  every page is below 4 GB and x86 DMA is cache-coherent, so loads are a
 *  walk over the physical pages and syncs are barriers.
 */
#define KEXBSD_BUILD
#include "obsd/kexobsd.h"

/* dmamem allocations, so map/free can find the virtual address of a segment */
struct dmamem
{
	struct dmamem *next;
	bus_addr_t phys;
	void *va;
	bus_size_t size;
};
static struct dmamem *dmamem_list;
static KSPIN_LOCK dmamem_lock;
static int dmamem_lock_ready;

static void list_lock(KIRQL *irql)
{
	if (!dmamem_lock_ready)
	{
		KeInitializeSpinLock(&dmamem_lock);
		dmamem_lock_ready = 1;
	}
	KeAcquireSpinLock(&dmamem_lock, irql);
}

int obsd_bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegs, bus_size_t maxsegsz, bus_size_t boundary,
                           int flags, bus_dmamap_t *mapp)
{
	bus_dmamap_t m;
	(void) t;
	(void) flags;
	if (nsegs < 1)
		return EINVAL;
	m = (bus_dmamap_t) kexbsd_malloc(sizeof(*m) + (nsegs - 1) * sizeof(bus_dma_segment_t), M_DEVBUF,
	                                 M_NOWAIT | M_ZERO);
	if (!m)
		return ENOMEM;
	m->_dm_size = size;
	m->_dm_segcnt = nsegs;
	m->_dm_maxsegsz = maxsegsz ? maxsegsz : size;
	m->_dm_boundary = boundary;
	*mapp = m;
	return 0;
}

void obsd_bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t m)
{
	(void) t;
	kexbsd_free(m, M_DEVBUF);
}

/* add one physically contiguous piece, merging with the previous segment */
static int add_piece(bus_dmamap_t m, bus_addr_t pa, bus_size_t len)
{
	while (len)
	{
		bus_size_t chunk = len;
		bus_dma_segment_t *last = m->dm_nsegs ? &m->dm_segs[m->dm_nsegs - 1] : NULL;
		if (m->_dm_boundary)
		{
			bus_size_t to_edge = m->_dm_boundary - (pa & (m->_dm_boundary - 1));
			if (chunk > to_edge)
				chunk = to_edge;
		}
		if (last && last->ds_addr + last->ds_len == pa && last->ds_len + chunk <= m->_dm_maxsegsz &&
		    (!m->_dm_boundary ||
		     (last->ds_addr & ~(m->_dm_boundary - 1)) == ((pa + chunk - 1) & ~(m->_dm_boundary - 1))))
			last->ds_len += chunk;
		else
		{
			if (chunk > m->_dm_maxsegsz)
				chunk = m->_dm_maxsegsz;
			if (m->dm_nsegs == m->_dm_segcnt)
				return EFBIG;
			m->dm_segs[m->dm_nsegs].ds_addr = pa;
			m->dm_segs[m->dm_nsegs].ds_len = chunk;
			m->dm_nsegs++;
		}
		pa += chunk;
		len -= chunk;
	}
	return 0;
}

/* load a buffer, appending to whatever the map already holds (the network
 * glue uses this for mbuf chains) */
int obsd_bus_dmamap_load_more(bus_dmamap_t m, void *buf, bus_size_t len)
{
	ULONG_PTR va = (ULONG_PTR) buf;
	if (m->dm_mapsize + len > m->_dm_size)
		return EFBIG;
	m->dm_mapsize += len;
	while (len)
	{
		PHYSICAL_ADDRESS pa = MmGetPhysicalAddress((PVOID) va);
		bus_size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
		int e;
		if (chunk > len)
			chunk = len;
		if (pa.HighPart)
			return EINVAL;
		e = add_piece(m, pa.LowPart, chunk);
		if (e)
			return e;
		va += chunk;
		len -= chunk;
	}
	return 0;
}

int obsd_bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t m, void *buf, bus_size_t len, void *proc, int flags)
{
	int e;
	(void) t;
	(void) proc;
	(void) flags;
	m->dm_nsegs = 0;
	m->dm_mapsize = 0;
	e = obsd_bus_dmamap_load_more(m, buf, len);
	if (e)
	{
		m->dm_nsegs = 0;
		m->dm_mapsize = 0;
	}
	return e;
}

int obsd_bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t m, bus_dma_segment_t *segs, int nsegs, bus_size_t size,
                             int flags)
{
	int i, e = 0;
	(void) t;
	(void) flags;
	m->dm_nsegs = 0;
	m->dm_mapsize = size;
	for (i = 0; i < nsegs && !e; i++)
		e = add_piece(m, segs[i].ds_addr, segs[i].ds_len);
	if (e)
	{
		m->dm_nsegs = 0;
		m->dm_mapsize = 0;
	}
	return e;
}

void obsd_bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t m)
{
	(void) t;
	m->dm_nsegs = 0;
	m->dm_mapsize = 0;
}

int obsd_bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align, bus_size_t boundary,
                          bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags)
{
	struct dmamem *d;
	PHYSICAL_ADDRESS hi;
	KIRQL irql;
	(void) t;
	(void) nsegs;
	(void) boundary;   /* one contiguous block; boundaries above its size never matter */
	(void) flags;
	if (align > PAGE_SIZE)
		return EINVAL;   /* contiguous memory is page-aligned; nothing here asks for more */
	d = (struct dmamem *) kexbsd_malloc(sizeof(*d), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!d)
		return ENOMEM;
	hi.QuadPart = 0xFFFFFFFFu;
	d->size = (size + PAGE_SIZE - 1) & ~(bus_size_t) (PAGE_SIZE - 1);
	d->va = MmAllocateContiguousMemory(d->size, hi);
	if (!d->va)
	{
		kexbsd_free(d, M_DEVBUF);
		return ENOMEM;
	}
	d->phys = MmGetPhysicalAddress(d->va).LowPart;
	memset(d->va, 0, d->size);
	list_lock(&irql);
	d->next = dmamem_list;
	dmamem_list = d;
	KeReleaseSpinLock(&dmamem_lock, irql);
	segs[0].ds_addr = d->phys;
	segs[0].ds_len = size;
	*rsegs = 1;
	return 0;
}

static struct dmamem *find(bus_addr_t phys, int unlink)
{
	struct dmamem **pp, *d;
	KIRQL irql;
	list_lock(&irql);
	for (pp = &dmamem_list; (d = *pp) != NULL; pp = &d->next)
		if (d->phys == phys)
		{
			if (unlink)
				*pp = d->next;
			break;
		}
	KeReleaseSpinLock(&dmamem_lock, irql);
	return d;
}

void obsd_bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	struct dmamem *d;
	(void) t;
	(void) nsegs;
	d = find(segs[0].ds_addr, 1);
	if (d)
	{
		MmFreeContiguousMemory(d->va);
		kexbsd_free(d, M_DEVBUF);
	}
}

int obsd_bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size, caddr_t *kvap, int flags)
{
	struct dmamem *d;
	(void) t;
	(void) nsegs;
	(void) size;
	(void) flags;
	d = find(segs[0].ds_addr, 0);
	if (!d)
		return EINVAL;
	*kvap = (caddr_t) d->va;
	return 0;
}

void obsd_bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size)
{
	(void) t;
	(void) kva;
	(void) size;   /* the mapping lives as long as the allocation */
}
