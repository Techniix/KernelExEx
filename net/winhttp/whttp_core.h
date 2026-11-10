/*
 *  KernelEx winhttp: portable HTTP/1.1 client core (no Windows types), used
 *  by the WinHTTP API layer and by the host tests.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 */
#ifndef WHTTP_CORE_H
#define WHTTP_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- errors (mapped to ERROR_WINHTTP_* by the API layer) ---------- */
#define WHTTP_OK               0
#define WHTTP_E_NAME          -1   /* host name not resolved */
#define WHTTP_E_CONNECT       -2   /* TCP connect failed or refused */
#define WHTTP_E_TIMEOUT       -3
#define WHTTP_E_IO            -4   /* connection reset or aborted */
#define WHTTP_E_PROTOCOL      -5   /* malformed HTTP */
#define WHTTP_E_PROXY         -6   /* proxy refused CONNECT */
#define WHTTP_E_URL           -7
#define WHTTP_E_NOMEM         -8
#define WHTTP_E_TOO_LARGE     -9   /* headers too long */
#define WHTTP_E_TLS          -20   /* see whttp_conn_tls_error() */

/* ---------- URLs ---------- */
typedef struct
{
	int secure;              /* https */
	char host[256];
	unsigned short port;
	char path[2048];         /* path + query, starting with '/' */
} whttp_url;

int whttp_parse_url(const char *url, whttp_url *out);

/* ---------- sockets (whttp_sock_*.c) ---------- */
typedef struct whttp_sock whttp_sock;

int whttp_sock_startup(void);
whttp_sock *whttp_sock_connect(const char *host, unsigned short port, int timeout_ms, int *error);
int whttp_sock_send(whttp_sock *s, const void *buf, size_t len, int timeout_ms);  /* bytes or <0 */
int whttp_sock_recv(whttp_sock *s, void *buf, size_t len, int timeout_ms);        /* bytes, 0 EOF, <0 */
void whttp_sock_close(whttp_sock *s);

/* ---------- connections ---------- */
typedef struct
{
	int connect_ms, send_ms, receive_ms;
} whttp_timeouts;

typedef struct whttp_conn whttp_conn;

/* Opens TCP to host:port, or to proxy_host:proxy_port (when proxy_host is
 * non-NULL). For secure connections through a proxy, an HTTP CONNECT tunnel
 * is made first. TLS is negotiated with kextls and always verified. */
whttp_conn *whttp_conn_open(const char *host, unsigned short port, int secure,
                            const char *proxy_host, unsigned short proxy_port,
                            const whttp_timeouts *t, int *error,
                            int *tls_error, char *detail, size_t detail_len);
void whttp_conn_close(whttp_conn *c);
int whttp_conn_tls_error(whttp_conn *c);            /* last KEXTLS_E_* code */
const char *whttp_conn_tls_detail(whttp_conn *c);   /* human-readable */
int whttp_conn_tls_info(whttp_conn *c, char *protocol, size_t plen, char *cipher, size_t clen);

/* ---------- requests and responses ---------- */
typedef struct
{
	int status;
	char reason[64];
	char *raw_headers;       /* "HTTP/1.1 200 OK\r\nName: value\r\n...\r\n\r\n" */
	size_t raw_len;
	long long content_length;  /* -1 = unknown */
	int chunked;
	int keep_alive;
	/* body state */
	long long remaining;     /* bytes left in body or current chunk */
	int body_done;
	int chunk_state;         /* internal */
} whttp_response;

/* target: origin-form "/path?q" or, for plain HTTP via a proxy,
 * absolute-form "http://host/path". extra_headers: CRLF-separated lines
 * (may be NULL). total_len: Content-Length to send, or -1 for none. */
int whttp_send_request(whttp_conn *c, const char *method, const char *target, const char *host_header,
                       const char *extra_headers, const void *body, size_t body_len, long long total_len);
int whttp_send_body(whttp_conn *c, const void *data, size_t len);
int whttp_read_response(whttp_conn *c, const char *method, whttp_response *r);
/* bytes read, 0 at end of body, <0 error */
int whttp_read_body(whttp_conn *c, whttp_response *r, void *buf, size_t len);
size_t whttp_body_available(whttp_conn *c, whttp_response *r);
void whttp_response_free(whttp_response *r);

/* Find a header (case-insensitive) in r->raw_headers; copies its value. */
int whttp_find_header(const whttp_response *r, const char *name, int index, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif
