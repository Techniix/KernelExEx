/*
 *  KernelEx winhttp: portable HTTP/1.1 client core.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "whttp_core.h"
#include "kextls.h"
#include "w9xrt.h"

#define CONN_BUF 16384
#define MAX_HEADERS (64 * 1024)

struct whttp_conn
{
	whttp_sock *sock;
	kextls_session *tls;
	whttp_timeouts t;
	int tls_err;
	char tls_detail[160];
	size_t pos, len;
	unsigned char buf[CONN_BUF];
};

/* ---------- small string helpers ---------- */

static int ieq_n(const char *a, const char *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
	{
		int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
		if (ca != cb)
			return 0;
		if (!ca)
			return 1;
	}
	return 1;
}

static int contains_token(const char *value, const char *token)
{
	size_t tl = strlen(token);
	const char *p = value;
	while (*p)
	{
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (ieq_n(p, token, tl) && (p[tl] == '\0' || p[tl] == ',' || p[tl] == ' ' || p[tl] == ';' || p[tl] == '\t'))
			return 1;
		while (*p && *p != ',')
			p++;
	}
	return 0;
}

/* ---------- URLs ---------- */

int whttp_parse_url(const char *url, whttp_url *out)
{
	const char *p, *host, *host_end, *port_start = NULL, *path;
	size_t hl;

	memset(out, 0, sizeof(*out));
	if (ieq_n(url, "https://", 8))
	{
		out->secure = 1;
		out->port = 443;
		p = url + 8;
	}
	else if (ieq_n(url, "http://", 7))
	{
		out->port = 80;
		p = url + 7;
	}
	else
		return WHTTP_E_URL;

	host = p;
	/* userinfo is not supported (and would be a credential leak vector) */
	for (; *p && *p != '/' && *p != '?' && *p != '#'; p++)
		if (*p == '@')
			return WHTTP_E_URL;
	path = p;
	host_end = path;
	if (*host == '[')
		return WHTTP_E_URL; /* IPv6 literal: no IPv6 on Windows 9x */
	for (p = host; p < host_end; p++)
		if (*p == ':')
		{
			port_start = p + 1;
			host_end = p;
			break;
		}
	hl = (size_t)(host_end - host);
	if (hl == 0 || hl >= sizeof(out->host))
		return WHTTP_E_URL;
	memcpy(out->host, host, hl);
	out->host[hl] = '\0';
	for (p = out->host; *p; p++)
		if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7e)
			return WHTTP_E_URL; /* ASCII only; IDN is not supported yet */
	if (port_start)
	{
		unsigned long v = 0;
		if (port_start == path)
			return WHTTP_E_URL;
		for (p = port_start; p < path; p++)
		{
			if (*p < '0' || *p > '9')
				return WHTTP_E_URL;
			v = v * 10 + (unsigned long)(*p - '0');
			if (v > 65535)
				return WHTTP_E_URL;
		}
		if (v == 0)
			return WHTTP_E_URL;
		out->port = (unsigned short)v;
	}
	if (*path == '/' || *path == '?')
	{
		size_t pl = strcspn(path, "#");
		if (pl + 2 >= sizeof(out->path))
			return WHTTP_E_URL;
		if (*path == '?')
		{
			out->path[0] = '/';
			memcpy(out->path + 1, path, pl);
			out->path[pl + 1] = '\0';
		}
		else
		{
			memcpy(out->path, path, pl);
			out->path[pl] = '\0';
		}
	}
	else
		strcpy(out->path, "/");
	return WHTTP_OK;
}

/* ---------- raw I/O ---------- */

static int tls_send(void *io, const unsigned char *buf, size_t len)
{
	whttp_conn *c = (whttp_conn *)io;
	int r = whttp_sock_send(c->sock, buf, len, c->t.send_ms);
	return r < 0 ? KEXTLS_E_IO : r;
}

static int tls_recv(void *io, unsigned char *buf, size_t len)
{
	whttp_conn *c = (whttp_conn *)io;
	int r = whttp_sock_recv(c->sock, buf, len, c->t.receive_ms);
	return r < 0 ? KEXTLS_E_IO : r;
}

static int conn_write_all(whttp_conn *c, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	while (len)
	{
		int r;
		if (c->tls)
		{
			r = kextls_write(c->tls, p, len);
			if (r < 0)
			{
				c->tls_err = r;
				return r == KEXTLS_E_IO ? WHTTP_E_IO : WHTTP_E_TLS;
			}
		}
		else
		{
			r = whttp_sock_send(c->sock, p, len, c->t.send_ms);
			if (r < 0)
				return r;
		}
		p += r;
		len -= (size_t)r;
	}
	return WHTTP_OK;
}

/* Refill the buffer if empty. Returns bytes buffered, 0 at EOF, <0 error. */
static int conn_fill(whttp_conn *c)
{
	int r;
	if (c->pos < c->len)
		return (int)(c->len - c->pos);
	c->pos = c->len = 0;
	if (c->tls)
	{
		r = kextls_read(c->tls, c->buf, sizeof(c->buf));
		if (r < 0)
		{
			if (r == KEXTLS_E_CLOSED)
				return 0;
			c->tls_err = r;
			return r == KEXTLS_E_IO ? WHTTP_E_IO : WHTTP_E_TLS;
		}
	}
	else
	{
		r = whttp_sock_recv(c->sock, c->buf, sizeof(c->buf), c->t.receive_ms);
		if (r < 0)
			return r;
	}
	c->len = (size_t)r;
	return r;
}

/* Read one line (without CRLF) into out. */
static int conn_read_line(whttp_conn *c, char *out, size_t max)
{
	size_t n = 0;
	for (;;)
	{
		int r = conn_fill(c);
		if (r < 0)
			return r;
		if (r == 0)
			return WHTTP_E_PROTOCOL; /* EOF in the middle of a header */
		while (c->pos < c->len)
		{
			char ch = (char)c->buf[c->pos++];
			if (ch == '\n')
			{
				if (n && out[n - 1] == '\r')
					n--;
				out[n] = '\0';
				return (int)n;
			}
			if (n + 1 >= max)
				return WHTTP_E_TOO_LARGE;
			out[n++] = ch;
		}
	}
}

/* ---------- connections ---------- */

static int proxy_connect(whttp_conn *c, const char *host, unsigned short port)
{
	char req[600], line[1024];
	int r, status = 0;

	w9x_snprintf(req, sizeof(req), "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n", host, (unsigned)port, host,
	             (unsigned)port);
	if ((r = conn_write_all(c, req, strlen(req))) != WHTTP_OK)
		return r;
	if ((r = conn_read_line(c, line, sizeof(line))) < 0)
		return r;
	if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12)
		return WHTTP_E_PROXY;
	status = atoi(line + 9);
	do
	{
		if ((r = conn_read_line(c, line, sizeof(line))) < 0)
			return r;
	} while (r > 0);
	if (status != 200)
		return WHTTP_E_PROXY;
	if (c->pos != c->len)
		return WHTTP_E_PROXY; /* proxy sent bytes before the tunnel opened */
	c->pos = c->len = 0;
	return WHTTP_OK;
}

whttp_conn *whttp_conn_open(const char *host, unsigned short port, int secure,
                            const char *proxy_host, unsigned short proxy_port,
                            const whttp_timeouts *t, int *error,
                            int *tls_error, char *detail, size_t detail_len)
{
	whttp_conn *c = (whttp_conn *)calloc(1, sizeof(*c));
	int e = WHTTP_OK;

	if (tls_error)
		*tls_error = 0;
	if (detail && detail_len)
		detail[0] = '\0';
	if (!c)
	{
		*error = WHTTP_E_NOMEM;
		return NULL;
	}
	c->t = *t;
	c->sock = whttp_sock_connect(proxy_host ? proxy_host : host, proxy_host ? proxy_port : port, t->connect_ms, &e);
	if (!c->sock)
		goto fail;
	if (secure)
	{
		int r;
		if (proxy_host && (e = proxy_connect(c, host, port)) != WHTTP_OK)
			goto fail;
		c->tls = kextls_client_new(host, "http/1.1", tls_send, tls_recv, c, &r);
		if (!c->tls)
		{
			c->tls_err = r;
			e = WHTTP_E_TLS;
			goto fail;
		}
		r = kextls_handshake(c->tls);
		if (r != KEXTLS_OK)
		{
			kextls_info info;
			c->tls_err = r;
			info.struct_size = sizeof(info);
			if (kextls_get_info(c->tls, &info) == KEXTLS_OK)
				w9x_snprintf(c->tls_detail, sizeof(c->tls_detail), "%s; %s", kextls_strerror(r), info.last_error);
			e = r == KEXTLS_E_IO ? WHTTP_E_IO : WHTTP_E_TLS;
			goto fail;
		}
	}
	*error = WHTTP_OK;
	return c;

fail:
	*error = e;
	if (e == WHTTP_E_TLS && !c->tls_detail[0])
		w9x_snprintf(c->tls_detail, sizeof(c->tls_detail), "%s", kextls_strerror(c->tls_err));
	if (tls_error)
		*tls_error = c->tls_err;
	if (detail && detail_len)
		w9x_snprintf(detail, detail_len, "%s", c->tls_detail);
	whttp_conn_close(c);
	return NULL;
}

void whttp_conn_close(whttp_conn *c)
{
	if (!c)
		return;
	if (c->tls)
	{
		kextls_close_notify(c->tls);
		kextls_free(c->tls);
	}
	if (c->sock)
		whttp_sock_close(c->sock);
	memset(c, 0, sizeof(*c));
	free(c);
}

int whttp_conn_tls_error(whttp_conn *c) { return c ? c->tls_err : 0; }
const char *whttp_conn_tls_detail(whttp_conn *c) { return c ? c->tls_detail : ""; }

int whttp_conn_tls_info(whttp_conn *c, char *protocol, size_t plen, char *cipher, size_t clen)
{
	kextls_info info;
	if (!c || !c->tls)
		return -1;
	info.struct_size = sizeof(info);
	if (kextls_get_info(c->tls, &info) != KEXTLS_OK)
		return -1;
	w9x_snprintf(protocol, plen, "%s", info.protocol);
	w9x_snprintf(cipher, clen, "%s", info.cipher);
	return 0;
}

/* ---------- requests ---------- */

int whttp_send_request(whttp_conn *c, const char *method, const char *target, const char *host_header,
                       const char *extra_headers, const void *body, size_t body_len, long long total_len)
{
	size_t cap = 512 + strlen(method) + strlen(target) + strlen(host_header) +
	             (extra_headers ? strlen(extra_headers) : 0);
	char *req = (char *)malloc(cap);
	size_t n;
	int r;

	if (!req)
		return WHTTP_E_NOMEM;
	n = (size_t)w9x_snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, target, host_header);
	if (extra_headers && *extra_headers)
	{
		size_t el = strlen(extra_headers);
		memcpy(req + n, extra_headers, el);
		n += el;
		if (el < 2 || extra_headers[el - 2] != '\r' || extra_headers[el - 1] != '\n')
		{
			req[n++] = '\r';
			req[n++] = '\n';
		}
	}
	if (total_len >= 0)
		n += (size_t)w9x_snprintf(req + n, cap - n, "Content-Length: %lu\r\n", (unsigned long)total_len);
	req[n++] = '\r';
	req[n++] = '\n';
	r = conn_write_all(c, req, n);
	free(req);
	if (r == WHTTP_OK && body && body_len)
		r = conn_write_all(c, body, body_len);
	return r;
}

int whttp_send_body(whttp_conn *c, const void *data, size_t len)
{
	return conn_write_all(c, data, len);
}

static int add_header_line(whttp_response *r, const char *line, size_t *cap)
{
	size_t l = strlen(line);
	if (r->raw_len + l + 3 > MAX_HEADERS)
		return WHTTP_E_TOO_LARGE;
	if (r->raw_len + l + 3 > *cap)
	{
		size_t nc = *cap * 2;
		char *p;
		while (nc < r->raw_len + l + 3)
			nc *= 2;
		p = (char *)realloc(r->raw_headers, nc);
		if (!p)
			return WHTTP_E_NOMEM;
		r->raw_headers = p;
		*cap = nc;
	}
	memcpy(r->raw_headers + r->raw_len, line, l);
	r->raw_len += l;
	r->raw_headers[r->raw_len++] = '\r';
	r->raw_headers[r->raw_len++] = '\n';
	r->raw_headers[r->raw_len] = '\0';
	return WHTTP_OK;
}

int whttp_read_response(whttp_conn *c, const char *method, whttp_response *r)
{
	char line[8192], value[256];
	int n, http10;
	size_t cap;

	for (;;)
	{
		memset(r, 0, sizeof(*r));
		cap = 2048;
		r->raw_headers = (char *)malloc(cap);
		if (!r->raw_headers)
			return WHTTP_E_NOMEM;
		r->raw_headers[0] = '\0';

		if ((n = conn_read_line(c, line, sizeof(line))) < 0)
			return n;
		if (strncmp(line, "HTTP/1.", 7) != 0 || n < 12 || line[8] != ' ')
			return WHTTP_E_PROTOCOL;
		http10 = line[7] == '0';
		r->status = atoi(line + 9);
		if (r->status < 100 || r->status > 999)
			return WHTTP_E_PROTOCOL;
		w9x_snprintf(r->reason, sizeof(r->reason), "%s", n > 13 ? line + 13 : "");
		if ((n = add_header_line(r, line, &cap)) != WHTTP_OK)
			return n;
		for (;;)
		{
			if ((n = conn_read_line(c, line, sizeof(line))) < 0)
				return n;
			if (n == 0)
				break;
			if ((n = add_header_line(r, line, &cap)) != WHTTP_OK)
				return n;
		}
		add_header_line(r, "", &cap);
		if (r->status >= 100 && r->status < 200 && r->status != 101)
		{
			whttp_response_free(r); /* 100 Continue etc.: read the real response */
			continue;
		}
		break;
	}

	r->content_length = -1;
	r->keep_alive = !http10;
	if (whttp_find_header(r, "Connection", 0, value, sizeof(value)) == 0)
	{
		if (contains_token(value, "close"))
			r->keep_alive = 0;
		else if (contains_token(value, "keep-alive"))
			r->keep_alive = 1;
	}
	if (whttp_find_header(r, "Transfer-Encoding", 0, value, sizeof(value)) == 0 && contains_token(value, "chunked"))
		r->chunked = 1;
	else if (whttp_find_header(r, "Content-Length", 0, value, sizeof(value)) == 0)
	{
		long long v = 0;
		const char *p = value;
		if (!*p)
			return WHTTP_E_PROTOCOL;
		for (; *p; p++)
		{
			if (*p < '0' || *p > '9' || v > (1LL << 50))
				return WHTTP_E_PROTOCOL;
			v = v * 10 + (*p - '0');
		}
		r->content_length = v;
	}

	if (!strcmp(method, "HEAD") || r->status == 204 || r->status == 304)
	{
		r->body_done = 1;
		r->content_length = 0;
	}
	else if (r->chunked)
		r->chunk_state = 0;
	else if (r->content_length >= 0)
	{
		r->remaining = r->content_length;
		if (r->remaining == 0)
			r->body_done = 1;
	}
	else
		r->keep_alive = 0; /* body runs to EOF */
	return WHTTP_OK;
}

/* Chunked framing: move to a state where data can be read or the body is done. */
static int chunk_advance(whttp_conn *c, whttp_response *r)
{
	char line[1024];
	int n;

	while (!r->body_done && !(r->chunk_state == 1 && r->remaining > 0))
	{
		if (r->chunk_state == 2) /* CRLF after chunk data */
		{
			if ((n = conn_read_line(c, line, sizeof(line))) < 0)
				return n;
			if (n != 0)
				return WHTTP_E_PROTOCOL;
			r->chunk_state = 0;
		}
		else if (r->chunk_state == 0) /* size line */
		{
			unsigned long long v = 0;
			const char *p;
			int digits = 0;
			if ((n = conn_read_line(c, line, sizeof(line))) < 0)
				return n;
			for (p = line; isxdigit((unsigned char)*p); p++, digits++)
			{
				int d = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
				if (digits >= 15)
					return WHTTP_E_PROTOCOL;
				v = v * 16 + (unsigned)d;
			}
			if (!digits)
				return WHTTP_E_PROTOCOL;
			if (v == 0)
				r->chunk_state = 3;
			else
			{
				r->remaining = (long long)v;
				r->chunk_state = 1;
			}
		}
		else if (r->chunk_state == 3) /* trailers until empty line */
		{
			if ((n = conn_read_line(c, line, sizeof(line))) < 0)
				return n;
			if (n == 0)
				r->body_done = 1;
		}
		else if (r->chunk_state == 1 && r->remaining == 0)
			r->chunk_state = 2;
	}
	return WHTTP_OK;
}

int whttp_read_body(whttp_conn *c, whttp_response *r, void *buf, size_t len)
{
	size_t avail, take;
	int n;

	if (r->body_done || len == 0)
		return 0;
	if (r->chunked && (n = chunk_advance(c, r)) != WHTTP_OK)
		return n;
	if (r->body_done)
		return 0;
	n = conn_fill(c);
	if (n < 0)
		return n;
	if (n == 0)
	{
		if (r->chunked || r->content_length >= 0)
			return WHTTP_E_IO; /* connection ended before the body did */
		r->body_done = 1;
		return 0;
	}
	avail = c->len - c->pos;
	take = len < avail ? len : avail;
	if ((r->chunked || r->content_length >= 0) && (long long)take > r->remaining)
		take = (size_t)r->remaining;
	memcpy(buf, c->buf + c->pos, take);
	c->pos += take;
	if (r->chunked || r->content_length >= 0)
	{
		r->remaining -= (long long)take;
		if (!r->chunked && r->remaining == 0)
			r->body_done = 1;
	}
	return (int)take;
}

size_t whttp_body_available(whttp_conn *c, whttp_response *r)
{
	size_t avail;
	int n;

	if (r->body_done)
		return 0;
	if (r->chunked && chunk_advance(c, r) != WHTTP_OK)
		return 0;
	if (r->body_done)
		return 0;
	n = conn_fill(c);
	if (n <= 0)
		return 0;
	avail = c->len - c->pos;
	if ((r->chunked || r->content_length >= 0) && (long long)avail > r->remaining)
		avail = (size_t)r->remaining;
	return avail;
}

void whttp_response_free(whttp_response *r)
{
	free(r->raw_headers);
	r->raw_headers = NULL;
	r->raw_len = 0;
}

int whttp_find_header(const whttp_response *r, const char *name, int index, char *out, size_t outlen)
{
	const char *p, *end;
	size_t nl = strlen(name);
	int seen = 0;

	if (!r->raw_headers)
		return -1;
	p = strstr(r->raw_headers, "\r\n");
	while (p)
	{
		p += 2;
		end = strstr(p, "\r\n");
		if (!end || end == p)
			break;
		if ((size_t)(end - p) > nl && p[nl] == ':' && ieq_n(p, name, nl))
		{
			if (seen++ == index)
			{
				const char *v = p + nl + 1;
				size_t vl;
				while (v < end && (*v == ' ' || *v == '\t'))
					v++;
				vl = (size_t)(end - v);
				while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t'))
					vl--;
				if (vl >= outlen)
					vl = outlen - 1;
				memcpy(out, v, vl);
				out[vl] = '\0';
				return 0;
			}
		}
		p = end;
	}
	return -1;
}
