/*
 *  KernelEx kexrt: Vista-era WS2_32 functions for Windows 9x.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  inet_pton / InetPtonW / inet_ntop / InetNtopW (text only, IPv4 + IPv6)
 *  and WSAPoll (on select). ws2_32.dll is loaded at run time, so kexrt still
 *  loads on Windows 95 without the Winsock 2 update.
 */
#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include "inet.h"
#include "kexrt.h"

#define AF_INET6_ 23
#ifndef WSAEAFNOSUPPORT
#define WSAEAFNOSUPPORT 10047
#endif

typedef int (WINAPI *select_fn)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
typedef int (WINAPI *lasterr_fn)(void);
typedef void (WINAPI *setlasterr_fn)(int);
typedef int (WINAPI *getsockopt_fn)(SOCKET, int, int, char *, int *);

static select_fn p_select;
static lasterr_fn p_WSAGetLastError;
static setlasterr_fn p_WSASetLastError;
static getsockopt_fn p_getsockopt;

static int ws2(void)
{
	if (!p_select)
	{
		HMODULE m = LoadLibraryA("WS2_32.DLL");
		if (!m)
			return 0;
		p_WSAGetLastError = (lasterr_fn)(void *)GetProcAddress(m, "WSAGetLastError");
		p_WSASetLastError = (setlasterr_fn)(void *)GetProcAddress(m, "WSASetLastError");
		p_getsockopt = (getsockopt_fn)(void *)GetProcAddress(m, "getsockopt");
		p_select = (select_fn)(void *)GetProcAddress(m, "select");
	}
	return p_select && p_WSAGetLastError && p_WSASetLastError && p_getsockopt;
}

static void set_wsa_error(int e)
{
	if (ws2())
		p_WSASetLastError(e);
	else
		SetLastError((DWORD)e);
}

/* ---------- text conversion ---------- */

static int WINAPI k_inet_pton(int af, const char *src, void *dst)
{
	if (!src || !dst)
	{
		set_wsa_error(WSAEFAULT);
		return -1;
	}
	if (af == AF_INET)
		return kx_pton4(src, (unsigned char *)dst);
	if (af == AF_INET6_)
		return kx_pton6(src, (unsigned char *)dst);
	set_wsa_error(WSAEAFNOSUPPORT);
	return -1;
}

static int WINAPI k_InetPtonW(int af, const WCHAR *src, void *dst)
{
	char a[64];
	int i;
	if (!src)
		return k_inet_pton(af, NULL, dst);
	for (i = 0; src[i]; i++)
	{
		if (i >= 63 || src[i] > 0x7F)
			return 0; /* not an address */
		a[i] = (char)src[i];
	}
	a[i] = 0;
	return k_inet_pton(af, a, dst);
}

static const char *WINAPI k_inet_ntop(int af, const void *src, char *dst, size_t size)
{
	size_t n;
	if (!src || !dst)
	{
		set_wsa_error(WSAEFAULT);
		return NULL;
	}
	if (af == AF_INET)
		n = kx_ntop4((const unsigned char *)src, dst, size);
	else if (af == AF_INET6_)
		n = kx_ntop6((const unsigned char *)src, dst, size);
	else
	{
		set_wsa_error(WSAEAFNOSUPPORT);
		return NULL;
	}
	if (!n)
	{
		set_wsa_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	return dst;
}

static const WCHAR *WINAPI k_InetNtopW(int af, const void *src, WCHAR *dst, size_t size)
{
	char a[64];
	size_t i;
	if (!dst || !k_inet_ntop(af, src, a, sizeof(a)))
		return NULL;
	if (strlen(a) + 1 > size)
	{
		set_wsa_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	for (i = 0; a[i]; i++)
		dst[i] = (WCHAR)a[i];
	dst[i] = 0;
	return dst;
}

/* ---------- WSAPoll ---------- */

typedef struct
{
	SOCKET fd;
	short events;
	short revents;
} pollfd_;

#define POLLRDNORM_ 0x0100
#define POLLRDBAND_ 0x0200
#define POLLIN_ (POLLRDNORM_ | POLLRDBAND_)
#define POLLPRI_ 0x0400
#define POLLWRNORM_ 0x0010
#define POLLWRBAND_ 0x0020
#define POLLOUT_ POLLWRNORM_
#define POLLERR_ 0x0001
#define POLLHUP_ 0x0002
#define POLLNVAL_ 0x0004

/* A Winsock fd_set is { u_int count; SOCKET array[] }: any length works. */
typedef struct
{
	u_int count;
	SOCKET fd[1];
} dyn_set;

static dyn_set *set_new(ULONG n)
{
	dyn_set *s = (dyn_set *)HeapAlloc(GetProcessHeap(), 0, sizeof(dyn_set) + n * sizeof(SOCKET));
	if (s)
		s->count = 0;
	return s;
}

static int set_has(const dyn_set *s, SOCKET fd)
{
	u_int i;
	for (i = 0; i < s->count; i++)
		if (s->fd[i] == fd)
			return 1;
	return 0;
}

static int WINAPI k_WSAPoll(pollfd_ *fds, ULONG n, INT timeout)
{
	dyn_set *r = NULL, *w = NULL, *x = NULL;
	struct timeval tv, *ptv = NULL;
	ULONG i;
	int ready = 0, res;

	if (!ws2())
	{
		SetLastError(WSANOTINITIALISED);
		return -1;
	}
	if (!fds && n)
	{
		p_WSASetLastError(WSAEFAULT);
		return -1;
	}
	r = set_new(n);
	w = set_new(n);
	x = set_new(n);
	if (!r || !w || !x)
	{
		p_WSASetLastError(WSAENOBUFS);
		res = -1;
		goto done;
	}
	for (i = 0; i < n; i++)
	{
		int type, len = sizeof(type);
		fds[i].revents = 0;
		if (fds[i].fd == INVALID_SOCKET)
			continue; /* ignored, as on Windows */
		if (p_getsockopt(fds[i].fd, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0)
		{
			fds[i].revents = POLLNVAL_;
			continue;
		}
		if (fds[i].events & (POLLIN_ | POLLPRI_))
			r->fd[r->count++] = fds[i].fd;
		if (fds[i].events & (POLLOUT_ | POLLWRBAND_))
			w->fd[w->count++] = fds[i].fd;
		x->fd[x->count++] = fds[i].fd;
	}
	for (i = 0; i < n; i++)
		if (fds[i].revents)
			ready++;
	if (ready)
		timeout = 0; /* report invalid sockets immediately, still poll the rest */
	if (timeout >= 0)
	{
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;
		ptv = &tv;
	}
	if (r->count + w->count + x->count == 0)
	{
		if (!ready && timeout > 0)
			Sleep((DWORD)timeout);
		res = ready;
		goto done;
	}
	res = p_select(0, (fd_set *)r, (fd_set *)w, (fd_set *)x, ptv);
	if (res < 0)
		goto done;
	for (i = 0; i < n; i++)
	{
		SOCKET fd = fds[i].fd;
		short ev = 0;
		if (fd == INVALID_SOCKET || fds[i].revents)
			continue;
		if (set_has(r, fd))
			ev |= fds[i].events & (POLLIN_ | POLLPRI_);
		if (set_has(w, fd))
			ev |= fds[i].events & (POLLOUT_ | POLLWRBAND_);
		if (set_has(x, fd))
			ev |= POLLERR_; /* failed connect or out-of-band data */
		fds[i].revents = ev;
		if (ev)
			ready++;
	}
	res = ready;
done:
	if (r)
		HeapFree(GetProcessHeap(), 0, r);
	if (w)
		HeapFree(GetProcessHeap(), 0, w);
	if (x)
		HeapFree(GetProcessHeap(), 0, x);
	return res;
}

/* sorted by strcmp */
const kexrt_api kexrt_ws2_32_apis[] = {
	{ "InetNtopW", (unsigned long)k_InetNtopW },
	{ "InetPtonW", (unsigned long)k_InetPtonW },
	{ "WSAPoll", (unsigned long)k_WSAPoll },
	{ "inet_ntop", (unsigned long)k_inet_ntop },
	{ "inet_pton", (unsigned long)k_inet_pton },
};
const int kexrt_ws2_32_count = sizeof(kexrt_ws2_32_apis) / sizeof(kexrt_ws2_32_apis[0]);
