/*
 *  KernelEx winhttp: blocking sockets with timeouts.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Windows: Winsock 1.1 (wsock32.dll), so the module also loads on a
 *  Windows 95 machine without the Winsock 2 update. Only socket, connect,
 *  select, send, recv, ioctlsocket and gethostbyname are used: all present
 *  in every Windows 9x Winsock. Names resolve over IPv4 (no IPv6 on 9x).
 *  Timeouts are done with select(), not SO_RCVTIMEO, whose behaviour varies
 *  across Windows 9x stacks.
 */
#include <stdlib.h>
#include <string.h>
#include "whttp_core.h"

#ifdef _WIN32
#include <winsock.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define last_error() WSAGetLastError()
#define WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define last_error() errno
#define WOULD_BLOCK(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK || (e) == EAGAIN)
#endif

struct whttp_sock
{
	sock_t fd;
};

int whttp_sock_startup(void)
{
#ifdef _WIN32
	static int started;
	if (!started)
	{
		WSADATA wd;
		if (WSAStartup(MAKEWORD(1, 1), &wd) != 0)
			return WHTTP_E_IO;
		started = 1;
	}
#endif
	return WHTTP_OK;
}

static int set_nonblocking(sock_t fd, int on)
{
#ifdef _WIN32
	u_long v = on ? 1 : 0;
	return ioctlsocket(fd, FIONBIO, &v) == 0 ? 0 : -1;
#else
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0 ? 0 : -1;
#endif
}

/* wait until readable (want_write = 0) or writable; 1 ready, 0 timeout, <0 error */
static int wait_fd(sock_t fd, int want_write, int timeout_ms)
{
	fd_set set, err;
	struct timeval tv, *ptv = NULL;
	int r;

	FD_ZERO(&set);
	FD_ZERO(&err);
	FD_SET(fd, &set);
	FD_SET(fd, &err);
	if (timeout_ms > 0)
	{
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		ptv = &tv;
	}
	r = select((int)fd + 1, want_write ? NULL : &set, want_write ? &set : NULL, &err, ptv);
	if (r < 0)
		return -1;
	if (r == 0)
		return 0;
	if (FD_ISSET(fd, &err) && !FD_ISSET(fd, &set))
		return -1;
	return 1;
}

static int resolve(const char *host, struct in_addr *out)
{
	unsigned long a = inet_addr(host);
	struct hostent *he;

	if (a != INADDR_NONE || !strcmp(host, "255.255.255.255"))
	{
		out->s_addr = a;
		return 0;
	}
	he = gethostbyname(host); /* not thread-safe on 9x: copy at once */
	if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0])
		return -1;
	memcpy(out, he->h_addr_list[0], sizeof(*out));
	return 0;
}

whttp_sock *whttp_sock_connect(const char *host, unsigned short port, int timeout_ms, int *error)
{
	struct sockaddr_in sa;
	whttp_sock *s;
	sock_t fd;
	int r;

	*error = whttp_sock_startup();
	if (*error != WHTTP_OK)
		return NULL;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (resolve(host, &sa.sin_addr) != 0)
	{
		*error = WHTTP_E_NAME;
		return NULL;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == BAD_SOCK)
	{
		*error = WHTTP_E_IO;
		return NULL;
	}
	{
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	}
	if (set_nonblocking(fd, 1) != 0)
	{
		sock_close(fd);
		*error = WHTTP_E_IO;
		return NULL;
	}
	r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (r != 0)
	{
		int e = last_error();
		if (!WOULD_BLOCK(e))
		{
			sock_close(fd);
			*error = WHTTP_E_CONNECT;
			return NULL;
		}
		r = wait_fd(fd, 1, timeout_ms);
		if (r <= 0)
		{
			sock_close(fd);
			*error = r == 0 ? WHTTP_E_TIMEOUT : WHTTP_E_CONNECT;
			return NULL;
		}
		{
			int so = 0;
#ifdef _WIN32
			int len = sizeof(so);
#else
			socklen_t len = sizeof(so);
#endif
			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so, &len) != 0 || so != 0)
			{
				sock_close(fd);
				*error = WHTTP_E_CONNECT;
				return NULL;
			}
		}
	}
	s = (whttp_sock *)malloc(sizeof(*s));
	if (!s)
	{
		sock_close(fd);
		*error = WHTTP_E_NOMEM;
		return NULL;
	}
	s->fd = fd;
	*error = WHTTP_OK;
	return s;
}

int whttp_sock_send(whttp_sock *s, const void *buf, size_t len, int timeout_ms)
{
	for (;;)
	{
		int r = (int)send(s->fd, (const char *)buf, (int)(len > 65536 ? 65536 : len), 0);
		if (r >= 0)
			return r;
		if (!WOULD_BLOCK(last_error()))
			return WHTTP_E_IO;
		r = wait_fd(s->fd, 1, timeout_ms);
		if (r == 0)
			return WHTTP_E_TIMEOUT;
		if (r < 0)
			return WHTTP_E_IO;
	}
}

int whttp_sock_recv(whttp_sock *s, void *buf, size_t len, int timeout_ms)
{
	for (;;)
	{
		int r = (int)recv(s->fd, (char *)buf, (int)(len > 65536 ? 65536 : len), 0);
		if (r >= 0)
			return r;
		if (!WOULD_BLOCK(last_error()))
			return WHTTP_E_IO;
		r = wait_fd(s->fd, 0, timeout_ms);
		if (r == 0)
			return WHTTP_E_TIMEOUT;
		if (r < 0)
			return WHTTP_E_IO;
	}
}

void whttp_sock_close(whttp_sock *s)
{
	if (!s)
		return;
	sock_close(s->fd);
	free(s);
}
