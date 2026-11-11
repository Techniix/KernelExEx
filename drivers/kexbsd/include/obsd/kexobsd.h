/*
 *  KernelExEx - kexbsd OpenBSD personality: the kernel interfaces OpenBSD
 *  drivers use, on top of the kexbsd core.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Every <sys/...>, <net/...>, <netinet/...> and <dev/pci/pcivar.h> header
 *  an OpenBSD driver includes resolves to a stub in this directory that
 *  includes this file (and kexobsd_net.h for the network parts). Only what
 *  drivers actually use is here; the compiler says what is missing.
 */
#ifndef KEXBSD_KEXOBSD_H
#define KEXBSD_KEXOBSD_H

#define KEXBSD_OPENBSD 1
#ifndef _KERNEL
#define _KERNEL 1
#endif
#include "kexbsd/bus.h"

/* the FreeBSD spellings kern.h maps; OpenBSD uses these names differently */
#undef free
#undef realloc
#undef mtx_lock
#undef mtx_unlock
#undef mtx_lock_spin
#undef mtx_unlock_spin
#undef mtx_assert
#undef mtx_initialized
#undef tsleep
#undef mtx_sleep
#undef microuptime
#undef nanouptime
#undef bootverbose

/* ---- types --------------------------------------------------------------- */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef char *caddr_t;
typedef uint32_t paddr_t;
typedef uint32_t vaddr_t;
typedef uint32_t vsize_t;
typedef uint32_t psize_t;
typedef long ssize_t_obsd;
#ifndef __cplusplus
#include <stdbool.h>
#endif
#define delay(us) DELAY(us)

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __predict_true(x) __builtin_expect(!!(x), 1)
#define __predict_false(x) __builtin_expect(!!(x), 0)
#define __unused __attribute__((unused))
#define __used __attribute__((used))
#define __dead __attribute__((noreturn))
#define __P(x) x
#define __KERNEL_RCSID(n, s)
#define __FBSDID(s)

#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define ISSET(t, f) ((t) & (f))
#define SET(t, f) ((t) |= (f))
#define CLR(t, f) ((t) &= ~(f))
#define KASSERT(e) do { if (!(e)) kexbsd_printf("kexbsd: assertion failed: %s (%s:%d)\n", #e, __FILE__, __LINE__); } while (0)
#define KASSERTMSG(e, ...) KASSERT(e)
#define CTASSERT(x) _Static_assert(x, #x)
#define panic(...) do { kexbsd_printf("kexbsd: panic: " __VA_ARGS__); kexbsd_printf("\n"); } while (0)

/* x86 is little-endian */
#define htole16(x) ((uint16_t) (x))
#define htole32(x) ((uint32_t) (x))
#define htole64(x) ((uint64_t) (x))
#define letoh16(x) ((uint16_t) (x))
#define letoh32(x) ((uint32_t) (x))
#define letoh64(x) ((uint64_t) (x))
#define htolem16(p, v) (*(volatile uint16_t *) (p) = htole16(v))
#define htolem32(p, v) (*(volatile uint32_t *) (p) = htole32(v))
#define htolem64(p, v) (*(volatile uint64_t *) (p) = htole64(v))
#define lemtoh16(p) letoh16(*(volatile uint16_t *) (p))
#define lemtoh32(p) letoh32(*(volatile uint32_t *) (p))
#define le16toh letoh16
#define le32toh letoh32
#define le64toh letoh64
#define htobe16(x) __builtin_bswap16(x)
#define htobe32(x) __builtin_bswap32(x)
#define betoh16(x) __builtin_bswap16(x)
#define betoh32(x) __builtin_bswap32(x)
#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)
#define swap16(x) __builtin_bswap16(x)
#define swap32(x) __builtin_bswap32(x)

/* string functions: the driver links its own (drivers are freestanding) */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strlcpy(char *d, const char *s, size_t n);
#define bcopy(s, d, n) memmove((d), (s), (n))
#define bzero(d, n) memset((d), 0, (n))
#define bcmp(a, b, n) memcmp((a), (b), (n))

/* ---- malloc(9), OpenBSD flavour: free() takes the size ------------------- */
#define M_DONTWAIT M_NOWAIT
#define M_CANFAIL 0
#define free(p, t, s) kexbsd_free((p), (t))
static __inline__ void *mallocarray(size_t n, size_t s, struct malloc_type *t, int f)
{
	if (s && n > ((size_t) -1) / s)
		return NULL;
	return kexbsd_malloc(n * s, t, f);
}

/* ---- spl, locks ---------------------------------------------------------- *
 * Front ends serialize the driver (an NDIS miniport is serialized by NDIS,
 * timers included), so spl and the big lock are no-ops and mutexes are
 * plain spin locks. */
#define IPL_NONE 0
#define IPL_NET 6
#define IPL_BIO 5
#define IPL_TTY 7
#define IPL_VM 8
#define IPL_HIGH 15
#define IPL_MPSAFE 0x100
#define IPL_SOFTNET 2
static __inline__ int obsd_spl(void) { return 0; }
#define splnet() obsd_spl()
#define splbio() obsd_spl()
#define spltty() obsd_spl()
#define splvm() obsd_spl()
#define splhigh() obsd_spl()
#define splsoftnet() obsd_spl()
#define splx(s) ((void) (s))
#define splassert(x) ((void) 0)
#define KERNEL_LOCK() ((void) 0)
#define KERNEL_UNLOCK() ((void) 0)
#define KERNEL_ASSERT_LOCKED() ((void) 0)
#define NET_ASSERT_LOCKED() ((void) 0)

struct mutex
{
	struct mtx m;
};
#define mtx_init(mx, ipl) kexbsd_mtx_init_obsd(mx)
#define mtx_enter(mx) kexbsd_mtx_lock(&(mx)->m)
#define mtx_leave(mx) kexbsd_mtx_unlock(&(mx)->m)
#define MUTEX_ASSERT_LOCKED(mx) ((void) 0)
#define MUTEX_ASSERT_UNLOCKED(mx) ((void) 0)
static __inline__ void kexbsd_mtx_init_obsd(struct mutex *mx)
{
	(mtx_init)(&mx->m, "obsd", NULL, MTX_SPIN | MTX_RECURSE);
}

struct rwlock
{
	struct mtx m;
};
#define RW_WRITE 0x01
#define RW_READ 0x02
#define RW_INTR 0x10
#define RW_NOSLEEP 0x40
#define rw_init(rw, name) (mtx_init)(&(rw)->m, (name), NULL, MTX_DEF | MTX_RECURSE)
#define rw_enter(rw, how) (kexbsd_mtx_lock(&(rw)->m), 0)
#define rw_enter_write(rw) kexbsd_mtx_lock(&(rw)->m)
#define rw_exit(rw) kexbsd_mtx_unlock(&(rw)->m)
#define rw_exit_write(rw) kexbsd_mtx_unlock(&(rw)->m)

/* ---- time and sleeping ---------------------------------------------------- */
#define tsleep(chan, pri, wmesg, timo) msleep((chan), NULL, (pri), (wmesg), (timo))
#define tsleep_nsec(chan, pri, wmesg, nsec) msleep((chan), NULL, (pri), (wmesg), (int) ((nsec) / 10000000))
#define INFSLP ((uint64_t) -1)
#define MSEC_TO_NSEC(ms) ((uint64_t) (ms) * 1000000)
#define SEC_TO_NSEC(s) ((uint64_t) (s) * 1000000000)
#define microuptime getmicrouptime
#define nanouptime getnanouptime

/* timeout(9): implemented by the front end (NDIS: miniport timers, which
 * NDIS serializes with the miniport's other entry points) */
struct timeout
{
	void (*to_func)(void *);
	void *to_arg;
	int to_set;
	unsigned int to_impl[40];   /* front end's timer object */
};
void obsd_timeout_set(struct timeout *t, void (*fn)(void *), void *arg);
int obsd_timeout_add_msec(struct timeout *t, int msec);
int obsd_timeout_del(struct timeout *t);
int obsd_timeout_pending(struct timeout *t);
#define timeout_set(t, fn, arg) obsd_timeout_set((t), (fn), (arg))
#define timeout_set_proc(t, fn, arg) obsd_timeout_set((t), (fn), (arg))
#define timeout_add(t, tk) obsd_timeout_add_msec((t), (tk) * (1000 / hz))
#define timeout_add_msec(t, ms) obsd_timeout_add_msec((t), (ms))
#define timeout_add_usec(t, us) obsd_timeout_add_msec((t), ((us) + 999) / 1000)
#define timeout_add_sec(t, s) obsd_timeout_add_msec((t), (s) * 1000)
#define timeout_del(t) obsd_timeout_del(t)
#define timeout_del_barrier(t) obsd_timeout_del(t)
#define timeout_pending(t) obsd_timeout_pending(t)
#define timeout_initialized(t) ((t)->to_set)

/* ---- autoconf(9) ---------------------------------------------------------- */
struct device
{
	char dv_xname[16];
	void *dv_frontend;   /* the kexbsd device_t */
};
struct cfattach
{
	size_t ca_devsize;
	int (*ca_match)(struct device *, void *, void *);
	void (*ca_attach)(struct device *, struct device *, void *);
	int (*ca_detach)(struct device *, int);
	int (*ca_activate)(struct device *, int);
};
struct cfdriver
{
	void **cd_devs;
	const char *cd_name;
	int cd_class;
};
#define DV_IFNET 1
#define DV_DISK 2
#define DVACT_SUSPEND 3
#define DVACT_RESUME 4
#define DVACT_POWERDOWN 6
#define DVACT_WAKEUP 5
#define DVACT_QUIESCE 2
#define DVACT_DEACTIVATE 1
#define config_activate_children(dev, act) 0
#define config_defer(dev, fn) (fn)(dev)
#define config_mountroot(dev, fn) (fn)(dev)

/* ---- PCI ---------------------------------------------------------------------- */
typedef uint32_t pcireg_t;
typedef device_t pcitag_t;
typedef void *pci_chipset_tag_t;
typedef struct
{
	int line;        /* 0 = INTx (the only kind on Windows 9x) */
	device_t dev;
} pci_intr_handle_t;
typedef struct obsd_dma_tag *bus_dma_tag_t;

struct pci_attach_args
{
	pci_chipset_tag_t pa_pc;
	pcitag_t pa_tag;
	pcireg_t pa_id;
	pcireg_t pa_class;
	bus_dma_tag_t pa_dmat;
	bus_dma_tag_t pa_dmat64;
	int pa_flags;
	int pa_bus, pa_device, pa_function;
	bus_space_tag_t pa_iot, pa_memt;
};
#define PCI_FLAGS_IO_ENABLED 0x01
#define PCI_FLAGS_MEM_ENABLED 0x02
#define PCI_FLAGS_MSI_ENABLED 0x20
#define PCI_FLAGS_MSIVEC_ENABLED 0x40

struct pci_matchid
{
	uint16_t pm_vid;
	uint16_t pm_pid;
};

#define pci_conf_read(pc, tag, reg) pci_read_config((tag), (reg), 4)
#define pci_conf_write(pc, tag, reg, v) pci_write_config((tag), (reg), (v), 4)
int obsd_pci_matchbyid(struct pci_attach_args *pa, const struct pci_matchid *ids, int n);
int obsd_pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid, int *offset, pcireg_t *value);
int obsd_pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags, bus_space_tag_t *tagp,
                        bus_space_handle_t *handlep, bus_addr_t *basep, bus_size_t *sizep, bus_size_t maxsize);
#define pci_matchbyid(pa, ids, n) obsd_pci_matchbyid((pa), (ids), (n))
#define pci_get_capability(pc, tag, id, off, val) obsd_pci_get_capability((pc), (tag), (id), (off), (val))
#define pci_mapreg_map(pa, reg, type, fl, t, h, b, s, m) obsd_pci_mapreg_map((pa), (reg), (type), (fl), (t), (h), (b), (s), (m))
#define pci_mapreg_type(pc, tag, reg) (pci_conf_read((pc), (tag), (reg)) & 0x7)
#define bus_space_unmap(t, h, s) ((void) 0)

/* interrupts: the front end owns the line; establish hands it the handler */
int obsd_pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ih);
void *obsd_pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int level, int (*fn)(void *), void *arg,
                              const char *name);
void obsd_pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie);
#define pci_intr_map(pa, ih) obsd_pci_intr_map((pa), (ih))
#define pci_intr_map_msi(pa, ih) (-1)
#define pci_intr_map_msix(pa, vec, ih) (-1)
#define pci_intr_string(pc, ih) "irq"
#define pci_intr_establish(pc, ih, lvl, fn, arg, name) obsd_pci_intr_establish((pc), (ih), (lvl), (fn), (arg), (name))
#define pci_intr_establish_cpu(pc, ih, lvl, ci, fn, arg, name) obsd_pci_intr_establish((pc), (ih), (lvl), (fn), (arg), (name))
#define pci_intr_disestablish(pc, c) obsd_pci_intr_disestablish((pc), (c))
#define intr_barrier(c) ((void) 0)

/* ---- bus_dma(9), OpenBSD/NetBSD flavour (kexbsd.sys) ------------------------------ */
typedef struct
{
	bus_addr_t ds_addr;
	bus_size_t ds_len;
} bus_dma_segment_t;

struct obsd_dmamap
{
	bus_size_t dm_mapsize;   /* size of the loaded mapping */
	int dm_nsegs;
	bus_size_t _dm_size;
	int _dm_segcnt;
	bus_size_t _dm_maxsegsz;
	bus_size_t _dm_boundary;
	bus_dma_segment_t dm_segs[1];   /* _dm_segcnt of them */
};
typedef struct obsd_dmamap *bus_dmamap_t;

#define BUS_DMA_WAITOK 0x0000
#define BUS_DMA_NOWAIT 0x0001
#define BUS_DMA_ALLOCNOW 0x0002
#define BUS_DMA_COHERENT 0x0004
#define BUS_DMA_BUS1 0x0010
#define BUS_DMA_READ 0x0200
#define BUS_DMA_WRITE 0x0400
#define BUS_DMA_STREAMING 0x0100
#define BUS_DMA_ZERO 0x1000
#define BUS_DMA_64BIT 0x2000
#define BUS_DMA_NOCACHE 0x0800
#define BUS_DMASYNC_PREREAD 0x01
#define BUS_DMASYNC_POSTREAD 0x02
#define BUS_DMASYNC_PREWRITE 0x04
#define BUS_DMASYNC_POSTWRITE 0x08

KEXBSD_API int obsd_bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegs, bus_size_t maxsegsz,
                                      bus_size_t boundary, int flags, bus_dmamap_t *mapp);
KEXBSD_API void obsd_bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map);
KEXBSD_API int obsd_bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf, bus_size_t len, void *proc,
                                    int flags);
KEXBSD_API int obsd_bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t map, bus_dma_segment_t *segs, int nsegs,
                                        bus_size_t size, int flags);
KEXBSD_API void obsd_bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map);
KEXBSD_API int obsd_bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align, bus_size_t boundary,
                                     bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags);
KEXBSD_API void obsd_bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs);
KEXBSD_API int obsd_bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size, caddr_t *kvap,
                                   int flags);
KEXBSD_API void obsd_bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size);
/* append a buffer to a loaded map (the network glue loads mbuf chains with it) */
KEXBSD_API int obsd_bus_dmamap_load_more(bus_dmamap_t map, void *buf, bus_size_t len);
#define bus_dmamap_create(t, s, n, m, b, f, p) obsd_bus_dmamap_create((t), (s), (n), (m), (b), (f), (p))
#define bus_dmamap_destroy(t, m) obsd_bus_dmamap_destroy((t), (m))
#define bus_dmamap_load(t, m, b, l, p, f) obsd_bus_dmamap_load((t), (m), (b), (l), (p), (f))
#define bus_dmamap_load_raw(t, m, s, n, sz, f) obsd_bus_dmamap_load_raw((t), (m), (s), (n), (sz), (f))
#define bus_dmamap_unload(t, m) obsd_bus_dmamap_unload((t), (m))
#define bus_dmamap_sync(t, m, o, l, op) __sync_synchronize()   /* x86 DMA is coherent */
#define bus_dmamem_alloc(t, s, a, b, sg, n, r, f) obsd_bus_dmamem_alloc((t), (s), (a), (b), (sg), (n), (r), (f))
#define bus_dmamem_free(t, sg, n) obsd_bus_dmamem_free((t), (sg), (n))
#define bus_dmamem_map(t, sg, n, s, k, f) obsd_bus_dmamem_map((t), (sg), (n), (s), (k), (f))
#define bus_dmamem_unmap(t, k, s) obsd_bus_dmamem_unmap((t), (k), (s))

/* ---- atomics ------------------------------------------------------------------ */
#define atomic_inc_int(p) __sync_fetch_and_add((p), 1)
#define atomic_dec_int(p) __sync_fetch_and_sub((p), 1)
#define atomic_add_int(p, v) __sync_fetch_and_add((p), (v))
#define atomic_sub_int(p, v) __sync_fetch_and_sub((p), (v))
#define atomic_inc_long(p) __sync_fetch_and_add((p), 1)
#define atomic_dec_long(p) __sync_fetch_and_sub((p), 1)
#define atomic_add_long(p, v) __sync_fetch_and_add((p), (v))
#define atomic_setbits_int(p, v) __sync_fetch_and_or((p), (v))
#define atomic_clearbits_int(p, v) __sync_fetch_and_and((p), ~(v))
#define atomic_cas_uint(p, o, n) __sync_val_compare_and_swap((p), (o), (n))
#define atomic_swap_uint(p, v) __sync_lock_test_and_set((p), (v))
#define atomic_load_int(p) (*(volatile unsigned int *) (p))
#define atomic_store_int(p, v) (*(volatile unsigned int *) (p) = (v))
#define membar_producer() __sync_synchronize()
#define membar_consumer() __sync_synchronize()
#define membar_sync() __sync_synchronize()
#define READ_ONCE(x) (*(volatile __typeof__(x) *) &(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *) &(x) = (v))

#endif
