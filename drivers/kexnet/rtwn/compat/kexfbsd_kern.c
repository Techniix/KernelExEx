/*
 * KernelExEx: FreeBSD kernel services for the FreeBSD personality that
 * kexbsd.sys does not export (it has mtx, callout, taskqueue, sleep, time,
 * malloc, printf): timeout tasks, firmware(9), mbufs (the OpenBSD
 * personality's, see kexfbsd.h), panic, randomness.
 *
 * Copyright (c) 2026 KernelExEx contributors. GPLv2 (see COPYING).
 */
#include "kexfbsd.h"
#include "../kexrtwn.h"

int kexfbsd_cold;
struct malloc_type M_USBDEV[1], M_80211_NODE[1], M_80211_VAP[1], M_80211_COM[1], M_80211_NODE_IE[1], M_80211_SCAN[1],
    M_80211_CRYPTO[1], M_80211_RATECTL[1], M_80211_MESH_GT_RT[1], M_80211_MESH_PERR[1], M_80211_MESH_PREP[1],
    M_80211_MESH_PREQ[1], M_80211_MESH_RT[1], M_USB[1];
int ifqmaxlen = 50;
const uint8_t kexfbsd_etherbroadcastaddr[ETHER_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

void kexfbsd_panic(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	kexbsd_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	kexbsd_printf("panic: %s\n", buf);
	KeBugCheckEx(0xDEADDEAD, 0x4B4558 /* "KEX" */, 0, 0, 0);
	for (;;)
		;
}

int kexfbsd_vprintf(const char *fmt, va_list ap)
{
	char buf[256];
	kexbsd_vsnprintf(buf, sizeof(buf), fmt, ap);
	return kexbsd_printf("%s", buf);
}

uint32_t kexfbsd_random(void)
{
	return kexrtwn_o_random();
}

void kexfbsd_random_buf(void *p, size_t n)
{
	uint8_t *b = (uint8_t *) p;
	while (n)
	{
		uint32_t r = kexrtwn_o_random();
		size_t k = n < 4 ? n : 4;
		memcpy(b, &r, k);
		b += k;
		n -= k;
	}
}

/* ---- timeout tasks: a callout that enqueues the task -------------------------- */
static void timeout_task_fire(void *arg)
{
	struct timeout_task *t = (struct timeout_task *) arg;
	t->f = 0;
	taskqueue_enqueue(t->q, &t->t);
}

int taskqueue_enqueue_timeout(struct taskqueue *q, struct timeout_task *t, int ticks_)
{
	t->q = q;
	if (ticks_ <= 0)
	{
		callout_stop(&t->c);
		t->f = 0;
		return taskqueue_enqueue(q, &t->t);
	}
	t->f = 1;
	callout_reset(&t->c, ticks_, timeout_task_fire, t);
	return 0;
}

int taskqueue_cancel_timeout(struct taskqueue *q, struct timeout_task *t, u_int *pend)
{
	unsigned int p = 0;
	int stopped = callout_stop(&t->c);
	t->f = 0;
	taskqueue_cancel(q, &t->t, &p);
	if (pend)
		*pend = p + (stopped ? 1 : 0);
	return 0;
}

void taskqueue_drain_timeout(struct taskqueue *q, struct timeout_task *t)
{
	callout_drain(&t->c);
	taskqueue_drain(q, &t->t);
}

void taskqueue_block(struct taskqueue *q)
{
	(void) q;
}

void taskqueue_unblock(struct taskqueue *q)
{
	(void) q;
}

/* ---- firmware(9): files in SYSTEM\KEXFW, under 8.3 names ------------------------ */
static const struct
{
	const char *name, *file;
} fwnames[] = {
	{ "rtwn-rtl8188eufw", "RW88EU" }, { "rtwn-rtl8188eefw", "RW88EE" },     { "rtwn-rtl8188fufw", "RW88FU" },
	{ "rtwn-rtl8192cfwE", "RW92CE" }, { "rtwn-rtl8192cfwE_B", "RW92CEB" }, { "rtwn-rtl8192cfwT", "RW92CT" },
	{ "rtwn-rtl8192cfwU", "RW92CU" }, { "rtwn-rtl8192eufw", "RW92EU" },     { "rtwn-rtl8723bufw", "RW23BU" },
	{ "rtwn-rtl8812aufw", "RW12AU" }, { "rtwn-rtl8821aufw", "RW21AU" },
};

const struct firmware *firmware_get(const char *name)
{
	struct firmware *fw;
	unsigned char *buf;
	size_t len;
	unsigned int i;
	const char *file = name;
	for (i = 0; i < nitems(fwnames); i++)
		if (!strcmp(fwnames[i].name, name))
			file = fwnames[i].file;   /* RW21AU -> RW21AU.FW (obsd_loadfirmware) */
	if (obsd_loadfirmware(file, &buf, &len))
	{
		kexbsd_printf("firmware %s (%s.FW) not found in SYSTEM\\KEXFW\n", name, file);
		return NULL;
	}
	fw = (struct firmware *) malloc(sizeof(*fw), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!fw)
	{
		free(buf, M_DEVBUF);
		return NULL;
	}
	fw->name = name;
	fw->data = buf;
	fw->datasize = len;
	return fw;
}

void firmware_put(const struct firmware *fw, int flags)
{
	(void) flags;
	if (!fw)
		return;
	free(__DECONST(void *, fw->data), M_DEVBUF);
	free(__DECONST(void *, fw), M_DEVBUF);
}

/* ---- mbufs: the OpenBSD personality owns them ------------------------------------ */
struct mbuf *kexfbsd_m_get(int how, int type)
{
	(void) how;
	(void) type;
	return kexrtwn_o_m_getjcl(MHLEN);
}

struct mbuf *kexfbsd_m_gethdr(int how, int type)
{
	(void) how;
	(void) type;
	return kexrtwn_o_m_getjcl(MHLEN);
}

struct mbuf *kexfbsd_m_getcl(int how, int type, int flags)
{
	(void) how;
	(void) type;
	(void) flags;
	return kexrtwn_o_m_getjcl(MCLBYTES);
}

struct mbuf *kexfbsd_m_getjcl(int how, int type, int flags, int size)
{
	(void) how;
	(void) type;
	(void) flags;
	return kexrtwn_o_m_getjcl(size);
}

struct mbuf *kexfbsd_m_get2(int size, int how, short type, int flags)
{
	(void) how;
	(void) type;
	(void) flags;
	return kexrtwn_o_m_getjcl(size);
}

struct mbuf *kexfbsd_m_get3(int size, int how, short type, int flags)
{
	return kexfbsd_m_get2(size, how, type, flags);
}

/* a single buffer large enough for len (we have no small-mbuf chains to build) */
struct mbuf *kexfbsd_m_getm(struct mbuf *m, int len, int how, short type)
{
	struct mbuf *n;
	(void) how;
	(void) type;
	n = kexrtwn_o_m_getjcl(len);
	if (!n)
		return NULL;
	if (m)
	{
		struct mbuf *t = m;
		while (t->m_next)
			t = t->m_next;
		n->m_flags &= ~M_PKTHDR;
		t->m_next = n;
		return m;
	}
	return n;
}

void kexfbsd_m_freem(struct mbuf *m)
{
	kexrtwn_o_m_freem(m);
}

struct mbuf *kexfbsd_m_free(struct mbuf *m)
{
	struct mbuf *n;
	if (!m)
		return NULL;
	n = m->m_next;
	m->m_next = NULL;
	kexrtwn_o_m_freem(m);
	return n;
}

void kexfbsd_m_adj(struct mbuf *m, int len)
{
	kexrtwn_o_m_adj(m, len);
}

void kexfbsd_m_copydata(const struct mbuf *m, int off, int len, caddr_t cp)
{
	kexrtwn_o_m_copydata(m, off, len, cp);
}

void kexfbsd_m_copyback(struct mbuf *m, int off, int len, c_caddr_t cp)
{
	kexrtwn_o_m_copyback(m, off, len, cp);
}

int kexfbsd_m_dup_pkthdr(struct mbuf *to, const struct mbuf *from, int how)
{
	(void) how;
	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	to->m_pkthdr = from->m_pkthdr;
	return 1;
}

void kexfbsd_m_move_pkthdr(struct mbuf *to, struct mbuf *from)
{
	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	to->m_pkthdr = from->m_pkthdr;
	from->m_flags &= ~M_PKTHDR;
}

struct mbuf *kexfbsd_m_defrag(struct mbuf *m, int how)
{
	(void) how;
	return kexrtwn_o_m_defrag(m);
}

struct mbuf *kexfbsd_m_prepend(struct mbuf *m, int len, int how)
{
	(void) how;
	return kexrtwn_o_m_prepend(m, len);
}

struct mbuf *kexfbsd_m_pullup(struct mbuf *m, int len)
{
	return kexrtwn_o_m_pullup(m, len);
}

struct mbuf *kexfbsd_m_dup(const struct mbuf *m, int how)
{
	(void) how;
	return kexrtwn_o_m_dup(m);
}

int kexfbsd_m_append(struct mbuf *m, int len, c_caddr_t cp)
{
	return kexrtwn_o_m_append(m, len, cp);
}

void kexfbsd_m_cat(struct mbuf *m, struct mbuf *n)
{
	kexrtwn_o_m_cat(m, n);
}

u_int kexfbsd_m_length(struct mbuf *m, struct mbuf **last)
{
	u_int len = 0;
	struct mbuf *t = m;
	for (; m; m = m->m_next)
	{
		len += m->m_len;
		t = m;
	}
	if (last)
		*last = t;
	return len;
}

/* m_ext describes the storage of every mbuf, m_dat or cluster (obsd_net.c) */
int kexfbsd_m_leadingspace(struct mbuf *m)
{
	return (int) (m->m_data - m->m_ext.ext_buf);
}

int kexfbsd_m_trailingspace(struct mbuf *m)
{
	return (int) (m->m_ext.ext_buf + m->m_ext.ext_size - (m->m_data + m->m_len));
}

struct mbuf *kexfbsd_m_copym(struct mbuf *m, int off, int len, int how)
{
	struct mbuf *n;
	u_int total = kexfbsd_m_length(m, NULL);
	(void) how;
	if (len == M_COPYALL)
		len = (int) total - off;
	n = kexrtwn_o_m_getjcl(len);
	if (!n)
		return NULL;
	kexrtwn_o_m_copydata(m, off, len, n->m_data);
	n->m_len = n->m_pkthdr.len = len;
	return n;
}
