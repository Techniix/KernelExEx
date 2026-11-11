/*
 * KernelExEx: the FreeBSD kernel personality, for FreeBSD drivers run on
 * Windows 98 SE / ME (first user: FreeBSD's rtwn Realtek Wi-Fi driver).
 * Force-included before every FreeBSD source; the <sys/...> headers those
 * sources include are empty stubs.
 *
 * The 802.11 side is not FreeBSD's net80211: FreeBSD's net80211 headers are
 * used as they are, for the structures, and the few net80211 functions the
 * driver calls are provided by the bridge (kexrtwn_bridge.c) on top of
 * OpenBSD's net80211, which does WPA in the kernel.
 *
 * Copyright (c) 2026 KernelExEx contributors. GPLv2 (see COPYING).
 */
#ifndef KEXFBSD_H
#define KEXFBSD_H

#define _KERNEL 1
#define __FreeBSD__ 15
#define __FreeBSD_version 1500000
#define INVARIANTS_OFF 1

#include "kexbsd/kern.h"
#include <stdbool.h>

/* ntddk.h leaves these behind */
#undef PAGE_SIZE
#undef min
#undef max
#undef ALIGN
#undef DELETE
#undef LIST_ENTRY
#undef SLIST_ENTRY
#undef IN
#undef OUT
#undef OPTIONAL
#undef ERROR
#include "sys/queue.h"

/* ---- types and compiler words ------------------------------------------------ */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef char *caddr_t;
typedef const char *c_caddr_t;
typedef int64_t sbintime_t;
typedef uint64_t *counter_u64_t;
typedef int boolean_t;
typedef uint32_t in_addr_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

#define __packed __attribute__((__packed__))
#define __aligned(x) __attribute__((__aligned__(x)))
#define __unused __attribute__((__unused__))
#define __used __attribute__((__used__))
#define __dead2 __attribute__((__noreturn__))
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#define __predict_true(x) __builtin_expect(!!(x), 1)
#define __predict_false(x) __builtin_expect(!!(x), 0)
#define __DECONST(type, var) ((type) (uintptr_t) (const void *) (var))
#define __DEVOLATILE(type, var) ((type) (uintptr_t) (volatile void *) (var))
#define __containerof(x, s, m) ((s *) ((char *) (x) - offsetof(s, m)))
#define __offsetof(t, m) offsetof(t, m)
#define __bitcount32(x) __builtin_popcount(x)
#define __FBSDID(s)
#define __KERNEL_RCSID(n, s)
#define __BEGIN_DECLS
#define __END_DECLS
#define __CONCAT(a, b) a##b
#define __STRING(x) #x
#define __XSTRING(x) __STRING(x)
#define __restrict restrict
#define __nonstring
#define __diagused __unused
#define __always_inline inline __attribute__((__always_inline__))
#define __noinline __attribute__((__noinline__))
#define __section(x) __attribute__((__section__(x)))

#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define roundup2(x, y) (((x) + ((y) - 1)) & (~((y) - 1)))
#define rounddown(x, y) (((x) / (y)) * (y))
#define rounddown2(x, y) ((x) & (~((y) - 1)))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline u_int min(u_int a, u_int b) { return a < b ? a : b; }
static inline u_int max(u_int a, u_int b) { return a > b ? a : b; }
#define NBBY 8
#define setbit(a, i) (((uint8_t *) (a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i) (((uint8_t *) (a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i) (((const uint8_t *) (a))[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i) ((((const uint8_t *) (a))[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)
#define PAGE_SIZE 4096
#define IFNAMSIZ 16
#define MAXCOMLEN 19
#ifndef INT_MAX
#define INT_MAX 0x7fffffff
#endif
#ifndef UINT_MAX
#define UINT_MAX 0xffffffffU
#endif
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

#define KASSERT(exp, msg) do { } while (0)
#define CTASSERT(x) _Static_assert(x, "compile-time assertion failed")
#define MPASS(x) do { } while (0)
#define WITNESS_WARN(...) do { } while (0)
#define KDB_WHY_UNSET 0
void kexfbsd_panic(const char *fmt, ...) __dead2;
#define panic kexfbsd_panic

/* ---- string and memory ---------------------------------------------------------- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
#define bcopy(s, d, n) memmove((d), (s), (n))
#define bzero(d, n) memset((d), 0, (n))
#define bcmp(a, b, n) memcmp((a), (b), (n))
#define explicit_bzero(d, n) memset((d), 0, (n))

/* ---- byte order (x86: little endian) -------------------------------------------- */
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
#define _BYTE_ORDER _LITTLE_ENDIAN
#define BYTE_ORDER _BYTE_ORDER
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#define bswap64(x) __builtin_bswap64(x)
#define htole16(x) ((uint16_t) (x))
#define htole32(x) ((uint32_t) (x))
#define htole64(x) ((uint64_t) (x))
#define le16toh(x) ((uint16_t) (x))
#define le32toh(x) ((uint32_t) (x))
#define le64toh(x) ((uint64_t) (x))
#define htobe16(x) bswap16(x)
#define htobe32(x) bswap32(x)
#define htobe64(x) bswap64(x)
#define be16toh(x) bswap16(x)
#define be32toh(x) bswap32(x)
#define be64toh(x) bswap64(x)
#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)
static inline uint16_t le16dec(const void *p) { const uint8_t *b = (const uint8_t *) p; return (uint16_t) (b[0] | b[1] << 8); }
static inline uint32_t le32dec(const void *p) { const uint8_t *b = (const uint8_t *) p; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t) b[3] << 24; }
static inline void le16enc(void *p, uint16_t v) { uint8_t *b = (uint8_t *) p; b[0] = (uint8_t) v; b[1] = (uint8_t) (v >> 8); }
static inline void le32enc(void *p, uint32_t v) { uint8_t *b = (uint8_t *) p; b[0] = (uint8_t) v; b[1] = (uint8_t) (v >> 8); b[2] = (uint8_t) (v >> 16); b[3] = (uint8_t) (v >> 24); }
static inline uint16_t be16dec(const void *p) { const uint8_t *b = (const uint8_t *) p; return (uint16_t) (b[0] << 8 | b[1]); }
static inline uint32_t be32dec(const void *p) { const uint8_t *b = (const uint8_t *) p; return (uint32_t) b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static inline void be16enc(void *p, uint16_t v) { uint8_t *b = (uint8_t *) p; b[0] = (uint8_t) (v >> 8); b[1] = (uint8_t) v; }
static inline void be32enc(void *p, uint32_t v) { uint8_t *b = (uint8_t *) p; b[0] = (uint8_t) (v >> 24); b[1] = (uint8_t) (v >> 16); b[2] = (uint8_t) (v >> 8); b[3] = (uint8_t) v; }

/* ---- atomics (one CPU on 98; these must still be atomic against DPCs) ----------- */
#define atomic_add_int(p, v) ((void) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST))
#define atomic_subtract_int(p, v) ((void) __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST))
#define atomic_fetchadd_int(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define atomic_set_int(p, v) ((void) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST))
#define atomic_clear_int(p, v) ((void) __atomic_fetch_and((p), ~(v), __ATOMIC_SEQ_CST))
#define atomic_load_int(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_store_int(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define atomic_cmpset_int(p, o, n) __sync_bool_compare_and_swap((p), (o), (n))
#define atomic_load_ptr(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_store_rel_int atomic_store_int
#define atomic_load_acq_int atomic_load_int
#define refcount_init(p, v) (*(p) = (v))
#define refcount_acquire(p) atomic_add_int((p), 1)
#define refcount_release(p) (atomic_fetchadd_int((p), -1) == 1)

/* ---- kernel services ------------------------------------------------------------ */
#define MSEC_2_TICKS(m) ((int) (((m) * hz + 999) / 1000))
#define TICKS_2_MSEC(t) ((t) * 1000 / hz)
#define TICKS_2_USEC(t) ((t) * (1000000 / hz))
#define USEC_2_TICKS(u) ((int) (((u) * hz + 999999) / 1000000))
#define SBT_1MS ((sbintime_t) 4294967)
#define C_PREL(x) 0
#define C_HARDCLOCK 0
#define log(level, ...) kexbsd_printf(__VA_ARGS__)
#define LOG_ERR 3
#define LOG_INFO 6
#define LOG_DEBUG 7
#define uprintf kexbsd_printf
#define vprintf(fmt, ap) kexfbsd_vprintf((fmt), (ap))
int kexfbsd_vprintf(const char *fmt, va_list ap);
#define arc4random() kexfbsd_random()
uint32_t kexfbsd_random(void);
#define arc4random_buf(p, n) kexfbsd_random_buf((p), (n))
void kexfbsd_random_buf(void *p, size_t n);
#define priv_check(td, p) 0
#define curthread ((void *) 0)
#define PRIV_NET80211_VAP_GETKEY 0
#define PRIV_NET80211_VAP_MANAGE 0
#define PRIV_NET80211_VAP_SETMAC 0
#define PRIV_NET80211_CREATE_VAP 0
#define getmicrotime microtime
#define getbinuptime(x) do { } while (0)
extern int kexfbsd_cold;
#define cold kexfbsd_cold

/* FreeBSD's malloc types: kexfbsd_kern.c defines them */
extern struct malloc_type M_USBDEV[1], M_80211_NODE[1], M_80211_VAP[1], M_80211_COM[1], M_80211_NODE_IE[1],
    M_80211_SCAN[1], M_80211_CRYPTO[1], M_80211_RATECTL[1], M_80211_MESH_GT_RT[1], M_80211_MESH_PERR[1],
    M_80211_MESH_PREP[1], M_80211_MESH_PREQ[1], M_80211_MESH_RT[1], M_USB[1];
#define mallocarray(n, s, t, f) malloc((n) * (s), (t), (f))

/* rwlock: one writer at a time, as a mutex */
struct rwlock
{
	struct mtx m;
};
#define rw_init(l, n) mtx_init(&(l)->m, (n), NULL, MTX_DEF)
#define rw_destroy(l) mtx_destroy(&(l)->m)
#define rw_rlock(l) mtx_lock(&(l)->m)
#define rw_runlock(l) mtx_unlock(&(l)->m)
#define rw_wlock(l) mtx_lock(&(l)->m)
#define rw_wunlock(l) mtx_unlock(&(l)->m)
#define rw_assert(l, w) do { } while (0)
#define RA_LOCKED 0
#define RA_WLOCKED 0
#define RA_UNLOCKED 0
#define mtx_trylock(m) (mtx_lock(m), 1)
#define mtx_recursed(m) 0

/* condvars on msleep/wakeup */
struct cv
{
	int unused;
};
#define cv_init(c, n) do { } while (0)
#define cv_destroy(c) do { } while (0)
#define cv_wait(c, m) msleep((c), (m), 0, "cv", 0)
#define cv_timedwait(c, m, t) msleep((c), (m), 0, "cv", (t))
#define cv_signal(c) wakeup_one(c)
#define cv_broadcast(c) wakeup(c)

/* taskqueue(9) timeout tasks, on kexbsd's taskqueue and callout */
struct timeout_task
{
	struct taskqueue *q;
	struct task t;
	struct callout c;
	int f;
};
#define TIMEOUT_TASK_INIT(queue, timeout_task, priority, func, context)                          \
	do                                                                                             \
	{                                                                                              \
		TASK_INIT(&(timeout_task)->t, priority, func, context);                                    \
		callout_init(&(timeout_task)->c, 1);                                                       \
		(timeout_task)->q = (queue);                                                               \
		(timeout_task)->f = 0;                                                                     \
	} while (0)
int taskqueue_enqueue_timeout(struct taskqueue *q, struct timeout_task *t, int ticks_);
int taskqueue_cancel_timeout(struct taskqueue *q, struct timeout_task *t, u_int *pend);
void taskqueue_drain_timeout(struct taskqueue *q, struct timeout_task *t);
void taskqueue_block(struct taskqueue *q);
void taskqueue_unblock(struct taskqueue *q);
void taskqueue_drain_all(struct taskqueue *q);
#define taskqueue_enqueue_fast taskqueue_enqueue

/* counter(9) */
#define counter_u64_alloc(f) ((counter_u64_t) malloc(sizeof(uint64_t), M_DEVBUF, M_WAITOK | M_ZERO))
#define counter_u64_free(c) free((c), M_DEVBUF)
#define counter_u64_add(c, v) (*(c) += (v))
#define counter_u64_fetch(c) (*(c))
#define counter_u64_zero(c) (*(c) = 0)

/* sysctl(9): nothing to tune at run time on 98 */
struct sysctl_ctx_list
{
	int unused;
};
struct sysctl_oid
{
	int unused;
};
struct sysctl_oid_list
{
	int unused;
};
struct sysctl_req
{
	void *newptr;
};
#define SYSCTL_HANDLER_ARGS struct sysctl_oid *oidp, void *arg1, intmax_t arg2, struct sysctl_req *req
#define SYSCTL_CHILDREN(o) ((struct sysctl_oid_list *) 0)
#define SYSCTL_STATIC_CHILDREN(o) ((struct sysctl_oid_list *) 0)
#define SYSCTL_ADD_INT(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_UINT(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_U32(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_U16(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_U8(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_PROC(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_NODE(...) ((struct sysctl_oid *) 0)
#define SYSCTL_ADD_STRING(...) ((struct sysctl_oid *) 0)
#define SYSCTL_DECL(n) struct __hack
#define SYSCTL_NODE(...) struct __hack
#define SYSCTL_INT(...) struct __hack
#define SYSCTL_UINT(...) struct __hack
#define SYSCTL_PROC(...) struct __hack
#define TUNABLE_INT(...) struct __hack
#define sysctl_handle_int(oidp, p, a, req) 0
#define sysctl_ctx_init(c) do { } while (0)
#define sysctl_ctx_free(c) do { } while (0)
#define CTLFLAG_RW 0
#define CTLFLAG_RD 0
#define CTLFLAG_RDTUN 0
#define CTLFLAG_RWTUN 0
#define CTLFLAG_MPSAFE 0
#define CTLFLAG_NEEDGIANT 0
#define CTLTYPE_INT 0
#define CTLTYPE_UINT 0
#define CTLTYPE_U32 0
#define OID_AUTO 0

/* modules, devices, drivers */
#define MODULE_VERSION(...) struct __hack
#define MODULE_DEPEND(...) struct __hack
#define DRIVER_MODULE(...) struct __hack
#define MODULE_PNP_INFO(...) struct __hack
#define DEVMETHOD(name, func) { #name, (void (*)(void)) (func) }
#define DEVMETHOD_END { 0, 0 }
#define BUS_PROBE_DEFAULT 0
#define BUS_PROBE_GENERIC (-100)
#define SI_SUB_DRIVERS 0
#define SI_ORDER_ANY 0
/* SYSINIT: a global pointer to the function; we call the few we need */
#define SYSINIT(name, sub, order, func, arg) void (*kexfbsd_sysinit_##name)(void *) = (void (*)(void *)) (func)
#define SYSUNINIT(...) struct __hack
typedef struct kexfbsd_device *device_t;
typedef struct
{
	const char *name;
	void (*func)(void);
} device_method_t;
typedef struct
{
	const char *name;
	const device_method_t *methods;
	size_t size;
} driver_t;
typedef int device_probe_t(device_t);
typedef int device_attach_t(device_t);
typedef int device_detach_t(device_t);
typedef int device_suspend_t(device_t);
typedef int device_resume_t(device_t);
typedef int device_shutdown_t(device_t);
struct kexfbsd_device
{
	char nameunit[16];
	const char *name;
	int unit;
	void *softc;
	void *ivars;
	char desc[64];
};
#define device_get_softc(d) ((d)->softc)
#define device_get_ivars(d) ((d)->ivars)
#define device_get_nameunit(d) ((const char *) (d)->nameunit)
#define device_get_name(d) ((d)->name)
#define device_get_unit(d) ((d)->unit)
#define device_get_sysctl_ctx(d) ((struct sysctl_ctx_list *) 0)
#define device_get_sysctl_tree(d) ((struct sysctl_oid *) 0)
#define device_set_desc(d, s) strlcpy((d)->desc, (s), sizeof((d)->desc))
#define device_set_desc_copy device_set_desc
#define device_printf(d, ...) (kexbsd_printf("%s: ", (d)->nameunit), kexbsd_printf(__VA_ARGS__))
#define bus_generic_detach(d) 0
#define bus_generic_print_child NULL
#define bus_generic_shutdown NULL
#define bus_get_dma_tag(d) NULL

/* firmware(9), from SYSTEM\KEXFW (see obsd_loadfirmware) */
struct firmware
{
	const char *name;
	const void *data;
	size_t datasize;
	unsigned int version;
};
const struct firmware *firmware_get(const char *name);
void firmware_put(const struct firmware *fw, int flags);
#define FIRMWARE_UNLOAD 1
int obsd_loadfirmware(const char *name, unsigned char **buf, size_t *len);

#define SET_DECLARE(set, ptype) struct __hack
#define DATA_SET(set, sym) struct __hack
#define TEXT_SET(set, sym) struct __hack

/* eventhandler(9): nothing registers here */
#define EVENTHANDLER_DECLARE(...) struct __hack
#define EVENTHANDLER_INVOKE(...) do { } while (0)
#define EVENTHANDLER_REGISTER(...) NULL
#define EVENTHANDLER_DEREGISTER(...) do { } while (0)
typedef void *eventhandler_tag;

/* ---- mbufs ---------------------------------------------------------------------- *
 * The same objects as the OpenBSD personality's mbufs (obsd_net.c); this is that
 * layout under FreeBSD's field names. The bridge clears FreeBSD's flags when an
 * mbuf crosses over. */
#define MSIZE 256
#define MCLBYTES 2048
#define MJUMPAGESIZE 4096
#define MJUM9BYTES (9 * 1024)
#define MJUM16BYTES (16 * 1024)
#define MHLEN 160
#define MLEN MHLEN
struct ifnet;
struct pkthdr
{
	int len;
	uint16_t csum_flags;
	uint16_t ether_vtag;
	uint16_t tso_segsz;
	uint16_t flowid16;
	struct ifnet *rcvif;
	uint8_t loopcnt;
	uint32_t csum_data;
	union
	{
		uint32_t ifidx;
		union
		{
			void *ptr;
			uint32_t thirtytwo[1];
		} PH_loc;
	};
};
struct kexfbsd_m_ext
{
	caddr_t ext_buf;
	unsigned int ext_size;
};
struct mbuf
{
	struct mbuf *m_next;
	struct mbuf *m_nextpkt;
	caddr_t m_data;
	unsigned int m_len;
	int m_flags;
	int m_type;
	struct pkthdr m_pkthdr;
	struct kexfbsd_m_ext m_ext;
	void *m_alloc;
	char m_dat[MHLEN];
};
#define M_EXT 0x00000001
#define M_PKTHDR 0x00000002
#define M_EOR 0x00000004
#define M_RDONLY 0x00000008
#define M_BCAST 0x00000100   /* the OpenBSD personality's values */
#define M_MCAST 0x00000200
#define M_PROTO1 0x00001000
#define M_PROTO2 0x00002000
#define M_PROTO3 0x00004000
#define M_PROTO4 0x00008000
#define M_PROTO5 0x00010000
#define M_PROTO6 0x00020000
#define M_PROTO7 0x00040000
#define M_PROTO8 0x00080000
#define M_PROTO9 0x00100000
#define M_PROTO10 0x00200000
#define M_PROTO11 0x00400000
#define M_PROTO12 0x00800000
#define M_COPYFLAGS (M_PKTHDR | M_EOR | M_RDONLY | M_BCAST | M_MCAST | 0x00fff000)
#define MT_DATA 1
#define MT_HEADER MT_DATA
#define MT_NOINIT 0
#define mtod(m, t) ((t) ((m)->m_data))
#define M_ASSERTPKTHDR(m) do { } while (0)
#define M_LEADINGSPACE(m) kexfbsd_m_leadingspace(m)
#define M_TRAILINGSPACE(m) kexfbsd_m_trailingspace(m)
#define M_WRITABLE(m) 1
#define M_PREPEND(m, plen, how) ((m) = kexfbsd_m_prepend((m), (plen), (how)))
#define M_ALIGN(m, len) do { } while (0)
#define m_align(m, len) do { } while (0)
#define MH_ALIGN(m, len) do { } while (0)
#define M_MOVE_PKTHDR(to, from) kexfbsd_m_move_pkthdr((to), (from))
#define m_tag_delete_chain(m, t) do { } while (0)
#define m_tag_locate(m, c, t, s) NULL
struct m_tag
{
	int unused;
};
int kexfbsd_m_leadingspace(struct mbuf *m);
int kexfbsd_m_trailingspace(struct mbuf *m);
struct mbuf *kexfbsd_m_prepend(struct mbuf *m, int len, int how);
void kexfbsd_m_move_pkthdr(struct mbuf *to, struct mbuf *from);
struct mbuf *kexfbsd_m_get(int how, int type);
struct mbuf *kexfbsd_m_gethdr(int how, int type);
struct mbuf *kexfbsd_m_getcl(int how, int type, int flags);
struct mbuf *kexfbsd_m_getjcl(int how, int type, int flags, int size);
struct mbuf *kexfbsd_m_get2(int size, int how, short type, int flags);
struct mbuf *kexfbsd_m_get3(int size, int how, short type, int flags);
struct mbuf *kexfbsd_m_getm(struct mbuf *m, int len, int how, short type);
void kexfbsd_m_freem(struct mbuf *m);
struct mbuf *kexfbsd_m_free(struct mbuf *m);
void kexfbsd_m_adj(struct mbuf *m, int len);
void kexfbsd_m_copydata(const struct mbuf *m, int off, int len, caddr_t cp);
void kexfbsd_m_copyback(struct mbuf *m, int off, int len, c_caddr_t cp);
int kexfbsd_m_dup_pkthdr(struct mbuf *to, const struct mbuf *from, int how);
struct mbuf *kexfbsd_m_defrag(struct mbuf *m, int how);
struct mbuf *kexfbsd_m_pullup(struct mbuf *m, int len);
struct mbuf *kexfbsd_m_dup(const struct mbuf *m, int how);
struct mbuf *kexfbsd_m_copym(struct mbuf *m, int off, int len, int how);
int kexfbsd_m_append(struct mbuf *m, int len, c_caddr_t cp);
void kexfbsd_m_cat(struct mbuf *m, struct mbuf *n);
u_int kexfbsd_m_length(struct mbuf *m, struct mbuf **last);
#define m_get kexfbsd_m_get
#define m_gethdr kexfbsd_m_gethdr
#define m_getcl kexfbsd_m_getcl
#define m_getjcl kexfbsd_m_getjcl
#define m_get2 kexfbsd_m_get2
#define m_get3 kexfbsd_m_get3
#define m_getm kexfbsd_m_getm
#define m_freem kexfbsd_m_freem
#define m_free kexfbsd_m_free
#define m_adj kexfbsd_m_adj
#define m_copydata kexfbsd_m_copydata
#define m_copyback kexfbsd_m_copyback
#define m_dup_pkthdr kexfbsd_m_dup_pkthdr
#define m_defrag kexfbsd_m_defrag
#define m_pullup kexfbsd_m_pullup
#define m_dup kexfbsd_m_dup
#define m_copym kexfbsd_m_copym
#define m_append kexfbsd_m_append
#define m_cat kexfbsd_m_cat
#define m_length kexfbsd_m_length
#define M_COPYALL 1000000000
#define M_NOWAIT_ 0

struct mbufq
{
	struct mbuf *head, *tail;
	int len, maxlen;
};
static inline void mbufq_init(struct mbufq *q, int maxlen)
{
	q->head = q->tail = NULL;
	q->len = 0;
	q->maxlen = maxlen;
}
static inline int mbufq_full(const struct mbufq *q) { return q->len >= q->maxlen; }
static inline int mbufq_len(const struct mbufq *q) { return q->len; }
static inline struct mbuf *mbufq_first(const struct mbufq *q) { return q->head; }
static inline int mbufq_enqueue(struct mbufq *q, struct mbuf *m)
{
	if (mbufq_full(q))
		return ENOBUFS;
	m->m_nextpkt = NULL;
	if (q->tail)
		q->tail->m_nextpkt = m;
	else
		q->head = m;
	q->tail = m;
	q->len++;
	return 0;
}
static inline struct mbuf *mbufq_dequeue(struct mbufq *q)
{
	struct mbuf *m = q->head;
	if (m)
	{
		q->head = m->m_nextpkt;
		if (!q->head)
			q->tail = NULL;
		m->m_nextpkt = NULL;
		q->len--;
	}
	return m;
}
static inline void mbufq_prepend(struct mbufq *q, struct mbuf *m)
{
	m->m_nextpkt = q->head;
	if (!q->tail)
		q->tail = m;
	q->head = m;
	q->len++;
}
static inline struct mbuf *mbufq_flush(struct mbufq *q)
{
	struct mbuf *m = q->head;
	q->head = q->tail = NULL;
	q->len = 0;
	return m;
}
static inline void mbufq_drain(struct mbufq *q)
{
	struct mbuf *m, *n;
	n = mbufq_flush(q);
	while ((m = n) != NULL)
	{
		n = m->m_nextpkt;
		m_freem(m);
	}
}
#ifndef ENOBUFS
#define ENOBUFS 55
#endif

/* ---- network interface bits net80211's headers name --------------------------- */
#define ETHER_ADDR_LEN 6
#define ETHER_TYPE_LEN 2
#define ETHER_CRC_LEN 4
#define ETHER_HDR_LEN (ETHER_ADDR_LEN * 2 + ETHER_TYPE_LEN)
#define ETHER_MAX_LEN 1518
#define ETHERMTU (ETHER_MAX_LEN - ETHER_HDR_LEN - ETHER_CRC_LEN)
#define ETHERTYPE_PAE 0x888e
#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV6 0x86dd
struct ether_header
{
	uint8_t ether_dhost[ETHER_ADDR_LEN];
	uint8_t ether_shost[ETHER_ADDR_LEN];
	uint16_t ether_type;
} __packed;
struct ether_addr
{
	uint8_t octet[ETHER_ADDR_LEN];
};
extern const uint8_t kexfbsd_etherbroadcastaddr[ETHER_ADDR_LEN];
#define ETHER_IS_MULTICAST(a) (*(a) & 0x01)
#define ETHER_IS_BROADCAST(a) (((a)[0] & (a)[1] & (a)[2] & (a)[3] & (a)[4] & (a)[5]) == 0xff)

typedef enum
{
	IFCOUNTER_IPACKETS,
	IFCOUNTER_IERRORS,
	IFCOUNTER_OPACKETS,
	IFCOUNTER_OERRORS,
	IFCOUNTER_COLLISIONS,
	IFCOUNTER_IBYTES,
	IFCOUNTER_OBYTES,
	IFCOUNTER_IMCASTS,
	IFCOUNTER_OMCASTS,
	IFCOUNTER_IQDROPS,
	IFCOUNTER_OQDROPS,
	IFCOUNTER_NOPROTO,
	IFCOUNTERS
} ift_counter;
struct ifnet;
typedef struct ifnet *if_t;
struct sbuf;
void kexfbsd_if_inc_counter(struct ifnet *ifp, ift_counter c, int64_t v);
#define if_inc_counter kexfbsd_if_inc_counter
#define if_getflags(ifp) kexfbsd_if_getflags(ifp)
int kexfbsd_if_getflags(struct ifnet *ifp);
#define if_getsoftc(ifp) kexfbsd_if_getsoftc(ifp)
void *kexfbsd_if_getsoftc(struct ifnet *ifp);
#define if_name(ifp) "wlan0"
#define IFF_UP 0x1
#define IFF_RUNNING 0x40
#define IFF_DRV_RUNNING 0x40
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_DRV_OACTIVE 0x400
struct ifqueue
{
	struct mbuf *ifq_head, *ifq_tail;
	int ifq_len, ifq_maxlen;
	struct mtx ifq_mtx;
};
struct ifmedia
{
	int unused;
};
struct ifmediareq;
typedef int (*ifm_change_cb_t)(struct ifnet *);
typedef void (*ifm_stat_cb_t)(struct ifnet *, struct ifmediareq *);
#define MTX_NETWORK_LOCK "network driver"
extern int ifqmaxlen;
#define ESHUTDOWN 58
#define fls(x) ((x) ? 32 - __builtin_clz((unsigned) (x)) : 0)
#define ffs(x) __builtin_ffs(x)
#define mtodo(m, o) ((void *) (((m)->m_data) + (o)))
struct ifmediareq
{
	int unused;
};
struct ifreq
{
	char ifr_name[IFNAMSIZ];
	void *ifr_data;
	int ifr_reqcap;
};
struct bpf_if;
struct route
{
	int unused;
};
struct sockaddr
{
	uint8_t sa_len;
	uint8_t sa_family;
	char sa_data[14];
};
struct sockaddr_dl
{
	uint8_t sdl_len;
	uint8_t sdl_family;
	uint16_t sdl_index;
	uint8_t sdl_type;
	uint8_t sdl_nlen;
	uint8_t sdl_alen;
	uint8_t sdl_slen;
	char sdl_data[46];
};
#define LLADDR(s) ((uint8_t *) ((s)->sdl_data + (s)->sdl_nlen))
typedef u_int iflladdr_cb_t(void *, struct sockaddr_dl *, u_int);
u_int kexfbsd_if_foreach_llmaddr(struct ifnet *ifp, iflladdr_cb_t *cb, void *arg);
#define if_foreach_llmaddr kexfbsd_if_foreach_llmaddr
/* checksum offload: none on this side (the OpenBSD stack checks) */
#define IFCAP_RXCSUM 0x0001
#define IFCAP_RXCSUM_IPV6 0x0002
#define SIOCSIFCAP 0x8020691e
#define CSUM_IP_CHECKED 0x0100
#define CSUM_IP_VALID 0x0200
#define CSUM_DATA_VALID 0x0400
#define CSUM_PSEUDO_HDR 0x0800
#define if_setcapabilities(ifp, c) do { (void) (ifp); } while (0)
#define if_setcapenablebit(ifp, s, c) do { (void) (ifp); } while (0)
#define if_getcapenable(ifp) 0
#define if_togglecapenable(ifp, c) do { } while (0)
struct in_addr
{
	in_addr_t s_addr;
};
struct epoch_tracker
{
	int unused;
};
#define NET_EPOCH_ENTER(et) do { (void) &(et); } while (0)
#define NET_EPOCH_EXIT(et) do { (void) &(et); } while (0)
#define NET_EPOCH_ASSERT() do { } while (0)
#define bpf_mtap2(...) do { } while (0)
#define bpf_mtap(...) do { } while (0)
#define bpf_peers_present(x) 0
#define bpf_peers_present_if(x) 0
#define BPF_MTAP(...) do { } while (0)
#define DLT_IEEE802_11_RADIO 127
#define DEBUGNET_DEFINE(x) struct __hack

/* AES for the headers' prototypes; the bridge does no FreeBSD-side crypto */
typedef struct
{
	int unused;
} rijndael_ctx;

#endif
