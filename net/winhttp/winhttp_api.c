/*
 *  KernelEx winhttp.dll: the WinHTTP API for Windows 9x, on the portable
 *  HTTP core (whttp_core.c) and kextls.dll for TLS 1.2/1.3.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Scope of this version: synchronous use (no WINHTTP_FLAG_ASYNC), HTTP/1.1,
 *  one connection per request, automatic redirects, named / IE / direct
 *  proxies with CONNECT tunnelling for HTTPS.
 *
 *  Security rules that apply whatever the caller asks:
 *    - certificates are always verified; WINHTTP_OPTION_SECURITY_FLAGS
 *      requests to ignore certificate errors are refused;
 *    - TLS below 1.2 is never negotiated: WINHTTP_OPTION_SECURE_PROTOCOLS
 *      only narrows the TLS 1.2 / 1.3 set, and a mask that leaves neither
 *      makes secure requests fail with ERROR_WINHTTP_SECURE_FAILURE;
 *    - HTTPS -> HTTP redirects are refused unless the caller explicitly
 *      sets WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS.
 */
#define _WINHTTP_INTERNAL_
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include "whttp_core.h"
#include "kextls.h"
#include "w9xrt.h"

#define H_SESSION 1
#define H_CONNECT 2
#define H_REQUEST 3
#define HANDLE_MAGIC 0x4B585748u /* 'KXWH' */

#define TLS12 WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define TLS13 0x00002000 /* WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 */
#define MAX_REDIRECTS_DEFAULT 10

typedef struct
{
	unsigned int magic;
	int type;
	DWORD_PTR context;
} hbase;

typedef struct
{
	hbase h;
	char *agent;
	char *proxy_host;     /* NULL = direct */
	unsigned short proxy_port;
	char *bypass;         /* ';'-separated */
	whttp_timeouts t;
	DWORD secure_protocols;
	DWORD redirect_policy;
	DWORD max_redirects;
	DWORD disabled;       /* WINHTTP_DISABLE_* */
} session_t;

typedef struct
{
	hbase h;
	session_t *s;
	char *host;
	INTERNET_PORT port;   /* 0 = default for scheme */
} connect_t;

typedef struct
{
	hbase h;
	connect_t *c;
	char *verb;
	char *path;
	DWORD flags;
	char *headers;        /* CRLF-terminated lines added by the caller */
	whttp_timeouts t;
	DWORD secure_protocols, redirect_policy, max_redirects, disabled;
	/* live state */
	int secure;
	char cur_host[256];
	unsigned short cur_port;
	char cur_path[2048];
	whttp_conn *conn;
	whttp_response resp;
	int have_resp;
	long long total_len, sent_body;
	void *optional;       /* copy of the request body for redirects */
	DWORD optional_len;
} request_t;

/* ---------- helpers ---------- */

static void *xcalloc(size_t n)
{
	void *p = calloc(1, n);
	if (!p)
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	return p;
}

static char *xstrdup(const char *s)
{
	char *p;
	if (!s)
		return NULL;
	p = (char *)malloc(strlen(s) + 1);
	if (p)
		strcpy(p, s);
	return p;
}

static size_t wlen(const WCHAR *s, int max)
{
	size_t n = 0;
	if (!s)
		return 0;
	while ((max < 0 || (int)n < max) && s[n])
		n++;
	return n;
}

/* UTF-16 -> UTF-8. len < 0: NUL-terminated. */
static char *w2u8(const WCHAR *w, int len)
{
	size_t n = wlen(w, len), i, o = 0;
	char *s = (char *)malloc(n * 3 + 1);
	if (!s)
		return NULL;
	for (i = 0; i < n; i++)
	{
		unsigned int c = w[i];
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
		{
			c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
			i++;
			s = (char *)realloc(s, n * 3 + 2 + o); /* rare; ensure room for 4 bytes */
			if (!s)
				return NULL;
		}
		if (c < 0x80)
			s[o++] = (char)c;
		else if (c < 0x800)
		{
			s[o++] = (char)(0xC0 | (c >> 6));
			s[o++] = (char)(0x80 | (c & 0x3F));
		}
		else if (c < 0x10000)
		{
			s[o++] = (char)(0xE0 | (c >> 12));
			s[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			s[o++] = (char)(0x80 | (c & 0x3F));
		}
		else
		{
			s[o++] = (char)(0xF0 | (c >> 18));
			s[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
			s[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			s[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	s[o] = '\0';
	return s;
}

/* Request targets: UTF-8 bytes above 0x7F and spaces are percent-encoded. */
static char *w2target(const WCHAR *w)
{
	char *u = w2u8(w, -1), *out;
	size_t i, o = 0;
	static const char hex[] = "0123456789ABCDEF";
	if (!u)
		return NULL;
	out = (char *)malloc(strlen(u) * 3 + 2);
	if (!out)
	{
		free(u);
		return NULL;
	}
	if (u[0] != '/')
		out[o++] = '/';
	for (i = 0; u[i]; i++)
	{
		unsigned char c = (unsigned char)u[i];
		if (c > 0x7E || c == ' ')
		{
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
		else
			out[o++] = (char)c;
	}
	out[o] = '\0';
	free(u);
	return out;
}

static hbase *get_handle(HINTERNET h, int type)
{
	hbase *b = (hbase *)h;
	if (!b || IsBadReadPtr(b, sizeof(*b)) || b->magic != HANDLE_MAGIC)
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	if (type && b->type != type)
	{
		SetLastError(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
		return NULL;
	}
	return b;
}

static DWORD map_tls(int tls)
{
	switch (tls)
	{
	case KEXTLS_E_CERT_UNTRUSTED: return ERROR_WINHTTP_SECURE_INVALID_CA;
	case KEXTLS_E_CERT_NAME: return ERROR_WINHTTP_SECURE_CERT_CN_INVALID;
	case KEXTLS_E_CERT_DATE: return ERROR_WINHTTP_SECURE_CERT_DATE_INVALID;
	case KEXTLS_E_CERT_OTHER: return ERROR_WINHTTP_SECURE_INVALID_CERT;
	case KEXTLS_E_PROTOCOL: return ERROR_WINHTTP_SECURE_CHANNEL_ERROR;
	case KEXTLS_E_IO: return ERROR_WINHTTP_CONNECTION_ERROR;
	default: return ERROR_WINHTTP_SECURE_FAILURE; /* clock, no roots, no entropy */
	}
}

static DWORD map_err(int e, int tls)
{
	switch (e)
	{
	case WHTTP_E_NAME: return ERROR_WINHTTP_NAME_NOT_RESOLVED;
	case WHTTP_E_CONNECT: return ERROR_WINHTTP_CANNOT_CONNECT;
	case WHTTP_E_TIMEOUT: return ERROR_WINHTTP_TIMEOUT;
	case WHTTP_E_IO: return ERROR_WINHTTP_CONNECTION_ERROR;
	case WHTTP_E_PROTOCOL: return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
	case WHTTP_E_PROXY: return ERROR_WINHTTP_CANNOT_CONNECT;
	case WHTTP_E_URL: return ERROR_WINHTTP_INVALID_URL;
	case WHTTP_E_NOMEM: return ERROR_NOT_ENOUGH_MEMORY;
	case WHTTP_E_TOO_LARGE: return ERROR_WINHTTP_HEADER_SIZE_OVERFLOW;
	case WHTTP_E_TLS: return map_tls(tls);
	default: return ERROR_WINHTTP_INTERNAL_ERROR;
	}
}

static int fail(DWORD err)
{
	SetLastError(err);
	return FALSE;
}

/* ---------- proxy configuration ---------- */

static void parse_proxy(const char *list, int secure, char **host, unsigned short *port)
{
	/* forms: "host:port", "http://host:port", "http=h:p;https=h:p" */
	const char *p = list, *pick = NULL;
	*host = NULL;
	if (!list || !*list)
		return;
	while (*p)
	{
		const char *end = p + strcspn(p, "; ");
		const char *eq = memchr(p, '=', (size_t)(end - p));
		if (!eq)
		{
			if (!pick)
				pick = p;
		}
		else if ((size_t)(eq - p) == (secure ? 5u : 4u) && !_strnicmp(p, secure ? "https" : "http", (size_t)(eq - p)))
			pick = eq + 1;
		p = *end ? end + 1 : end;
	}
	if (!pick)
		return;
	{
		char buf[300];
		const char *start = pick, *colon;
		size_t n = strcspn(pick, "; ");
		if (n >= sizeof(buf))
			return;
		memcpy(buf, start, n);
		buf[n] = '\0';
		start = buf;
		if (!_strnicmp(start, "http://", 7))
			start += 7;
		colon = strrchr(start, ':');
		*port = 80;
		if (colon)
		{
			*port = (unsigned short)atoi(colon + 1);
			((char *)colon)[0] = '\0';
		}
		if (*start)
			*host = xstrdup(start);
	}
}

static int bypassed(const char *bypass, const char *host)
{
	const char *p = bypass;
	if (!bypass)
		return 0;
	while (*p)
	{
		size_t n = strcspn(p, "; ,");
		if (n)
		{
			if (n == 7 && !_strnicmp(p, "<local>", 7))
			{
				if (!strchr(host, '.'))
					return 1;
			}
			else if (p[0] == '*')
			{
				size_t hl = strlen(host), sl = n - 1;
				if (hl >= sl && !_strnicmp(host + hl - sl, p + 1, sl))
					return 1;
			}
			else if (strlen(host) == n && !_strnicmp(host, p, n))
				return 1;
		}
		p += n;
		while (*p == ';' || *p == ' ' || *p == ',')
			p++;
	}
	return 0;
}

/* IE settings: HKCU\...\Internet Settings ProxyEnable/ProxyServer/ProxyOverride */
static void ie_proxy(char **server, char **bypass, int *enabled)
{
	HKEY k;
	char buf[1024];
	DWORD type, size, en = 0;

	*server = *bypass = NULL;
	*enabled = 0;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0,
	                  KEY_READ, &k) != ERROR_SUCCESS)
		return;
	size = sizeof(en);
	if (RegQueryValueExA(k, "ProxyEnable", NULL, &type, (BYTE *)&en, &size) == ERROR_SUCCESS)
		*enabled = (type == REG_DWORD || type == REG_BINARY) && en != 0;
	size = sizeof(buf);
	if (RegQueryValueExA(k, "ProxyServer", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
		*server = xstrdup(buf);
	size = sizeof(buf);
	if (RegQueryValueExA(k, "ProxyOverride", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
		*bypass = xstrdup(buf);
	RegCloseKey(k);
}

/* ---------- sessions, connections, requests ---------- */

HINTERNET WINAPI WinHttpOpen(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
	session_t *s;

	if (flags & WINHTTP_FLAG_ASYNC)
	{
		/* asynchronous mode is planned (phase N4); fail cleanly for now */
		SetLastError(ERROR_NOT_SUPPORTED);
		return NULL;
	}
	s = (session_t *)xcalloc(sizeof(*s));
	if (!s)
		return NULL;
	s->h.magic = HANDLE_MAGIC;
	s->h.type = H_SESSION;
	s->agent = agent ? w2u8(agent, -1) : NULL;
	s->t.connect_ms = 60000;
	s->t.send_ms = 30000;
	s->t.receive_ms = 30000;
	s->secure_protocols = TLS12 | TLS13;
	s->redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	s->max_redirects = MAX_REDIRECTS_DEFAULT;

	if (access == WINHTTP_ACCESS_TYPE_NAMED_PROXY && proxy)
	{
		/* stored raw; the scheme-specific entry is picked per request */
		s->proxy_host = w2u8(proxy, -1);
		s->bypass = bypass ? w2u8(bypass, -1) : NULL;
	}
	else if (access == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
	{
		char *srv, *byp;
		int en;
		ie_proxy(&srv, &byp, &en);
		if (en && srv)
		{
			s->proxy_host = srv;
			s->bypass = byp;
		}
		else
		{
			free(srv);
			free(byp);
		}
	}
	/* WINHTTP_ACCESS_TYPE_DEFAULT_PROXY / NO_PROXY: direct (Windows 9x has
	 * no netsh WinHTTP proxy configuration). */
	return (HINTERNET)s;
}

HINTERNET WINAPI WinHttpConnect(HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD reserved)
{
	session_t *s = (session_t *)get_handle(session, H_SESSION);
	connect_t *c;
	char *host;
	const char *p;

	(void)reserved;
	if (!s)
		return NULL;
	if (!server || !*server)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	host = w2u8(server, -1);
	if (!host)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	for (p = host; *p; p++)
		if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7F || *p == '/' || *p == '@')
		{
			free(host);
			SetLastError(ERROR_WINHTTP_INVALID_URL); /* IDN host names are not supported yet */
			return NULL;
		}
	c = (connect_t *)xcalloc(sizeof(*c));
	if (!c)
	{
		free(host);
		return NULL;
	}
	c->h.magic = HANDLE_MAGIC;
	c->h.type = H_CONNECT;
	c->s = s;
	c->host = host;
	c->port = port;
	return (HINTERNET)c;
}

HINTERNET WINAPI WinHttpOpenRequest(HINTERNET connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                                    LPCWSTR referrer, LPCWSTR *accept_types, DWORD flags)
{
	connect_t *c = (connect_t *)get_handle(connect, H_CONNECT);
	request_t *r;

	(void)version; /* always HTTP/1.1 */
	if (!c)
		return NULL;
	r = (request_t *)xcalloc(sizeof(*r));
	if (!r)
		return NULL;
	r->h.magic = HANDLE_MAGIC;
	r->h.type = H_REQUEST;
	r->c = c;
	r->verb = verb && *verb ? w2u8(verb, -1) : xstrdup("GET");
	r->path = object && *object ? w2target(object) : xstrdup("/");
	r->flags = flags;
	r->t = c->s->t;
	r->secure_protocols = c->s->secure_protocols;
	r->redirect_policy = c->s->redirect_policy;
	r->max_redirects = c->s->max_redirects;
	r->disabled = c->s->disabled;
	r->headers = xstrdup("");
	if (!r->verb || !r->path || !r->headers)
	{
		free(r->verb);
		free(r->path);
		free(r->headers);
		free(r);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if (referrer && *referrer)
	{
		char *v = w2u8(referrer, -1), *n;
		if (v && (n = (char *)malloc(strlen(r->headers) + strlen(v) + 16)))
		{
			strcpy(n, r->headers);
			strcat(n, "Referer: ");
			strcat(n, v);
			strcat(n, "\r\n");
			free(r->headers);
			r->headers = n;
		}
		free(v);
	}
	if (accept_types && accept_types[0])
	{
		size_t need = 16, i;
		char *n;
		for (i = 0; accept_types[i]; i++)
			need += wlen(accept_types[i], -1) * 3 + 2;
		n = (char *)malloc(strlen(r->headers) + need);
		if (n)
		{
			strcpy(n, r->headers);
			strcat(n, "Accept: ");
			for (i = 0; accept_types[i]; i++)
			{
				char *v = w2u8(accept_types[i], -1);
				if (i)
					strcat(n, ", ");
				if (v)
					strcat(n, v);
				free(v);
			}
			strcat(n, "\r\n");
			free(r->headers);
			r->headers = n;
		}
	}
	return (HINTERNET)r;
}

static void request_reset(request_t *r)
{
	if (r->have_resp)
		whttp_response_free(&r->resp);
	r->have_resp = 0;
	if (r->conn)
		whttp_conn_close(r->conn);
	r->conn = NULL;
}

WINBOOL WINAPI WinHttpCloseHandle(HINTERNET h)
{
	hbase *b = get_handle(h, 0);
	if (!b)
		return FALSE;
	b->magic = 0;
	switch (b->type)
	{
	case H_SESSION:
	{
		session_t *s = (session_t *)b;
		free(s->agent);
		free(s->proxy_host);
		free(s->bypass);
		break;
	}
	case H_CONNECT:
		free(((connect_t *)b)->host);
		break;
	case H_REQUEST:
	{
		request_t *r = (request_t *)b;
		request_reset(r);
		free(r->verb);
		free(r->path);
		free(r->headers);
		free(r->optional);
		break;
	}
	}
	free(b);
	return TRUE;
}

/* Header names are compared case-insensitively. Replace or add one line. */
static int set_header(request_t *r, const char *line, DWORD mods)
{
	const char *colon = strchr(line, ':');
	size_t nl;
	char *p, *n;

	if (!colon || colon == line)
		return fail(ERROR_WINHTTP_INVALID_HEADER);
	nl = (size_t)(colon - line);
	for (p = r->headers; *p;)
	{
		char *end = strstr(p, "\r\n");
		if (!end)
			break;
		if ((size_t)(end - p) > nl && p[nl] == ':' && !_strnicmp(p, line, nl))
		{
			if (mods & WINHTTP_ADDREQ_FLAG_ADD_IF_NEW)
				return fail(ERROR_WINHTTP_HEADER_ALREADY_EXISTS);
			if (mods & (WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_COALESCE_WITH_COMMA))
			{
				memmove(p, end + 2, strlen(end + 2) + 1);
				continue;
			}
		}
		p = end + 2;
	}
	if ((mods & WINHTTP_ADDREQ_FLAG_REPLACE) && colon[1] == '\0')
		return TRUE; /* REPLACE with an empty value removes the header */
	n = (char *)malloc(strlen(r->headers) + strlen(line) + 3);
	if (!n)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	strcpy(n, r->headers);
	strcat(n, line);
	strcat(n, "\r\n");
	free(r->headers);
	r->headers = n;
	return TRUE;
}

static int add_headers(request_t *r, LPCWSTR headers, DWORD len, DWORD mods)
{
	char *all, *p;
	int ok = TRUE;

	if (!headers)
		return TRUE;
	all = w2u8(headers, len == (DWORD)-1 ? -1 : (int)len);
	if (!all)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	for (p = all; *p && ok;)
	{
		size_t n = strcspn(p, "\r\n");
		if (n)
		{
			char save = p[n];
			p[n] = '\0';
			if (strpbrk(p, "\r\n"))
				ok = fail(ERROR_WINHTTP_INVALID_HEADER);
			else
				ok = set_header(r, p, mods);
			p[n] = save;
		}
		p += n;
		while (*p == '\r' || *p == '\n')
			p++;
	}
	free(all);
	return ok;
}

WINBOOL WINAPI WinHttpAddRequestHeaders(HINTERNET req, LPCWSTR headers, DWORD len, DWORD mods)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	if (!r)
		return FALSE;
	if (!headers)
		return fail(ERROR_INVALID_PARAMETER);
	return add_headers(r, headers, len, mods);
}

static int resolve_target(request_t *r)
{
	connect_t *c = r->c;
	r->secure = (r->flags & WINHTTP_FLAG_SECURE) != 0;
	w9x_snprintf(r->cur_host, sizeof(r->cur_host), "%s", c->host);
	r->cur_port = c->port ? c->port : (r->secure ? 443 : 80);
	w9x_snprintf(r->cur_path, sizeof(r->cur_path), "%s", r->path);
	return TRUE;
}

static int open_and_send(request_t *r, const void *optional, DWORD optional_len)
{
	session_t *s = r->c->s;
	char *proxy_host = NULL, host_header[300], *extra, target[2400];
	unsigned short proxy_port = 0;
	int e, tls = 0, use_proxy;
	size_t extra_len;

	request_reset(r);
	if (r->secure && !(r->secure_protocols & (TLS12 | TLS13)))
		return fail(ERROR_WINHTTP_SECURE_FAILURE);
	use_proxy = s->proxy_host && !bypassed(s->bypass, r->cur_host);
	if (use_proxy)
		parse_proxy(s->proxy_host, r->secure, &proxy_host, &proxy_port);
	r->conn = whttp_conn_open(r->cur_host, r->cur_port, r->secure, proxy_host, proxy_port, &r->t, &e, &tls, NULL, 0);
	if (!r->conn)
	{
		free(proxy_host);
		return fail(map_err(e, tls));
	}
	if ((r->secure && r->cur_port == 443) || (!r->secure && r->cur_port == 80))
		w9x_snprintf(host_header, sizeof(host_header), "%s", r->cur_host);
	else
		w9x_snprintf(host_header, sizeof(host_header), "%s:%u", r->cur_host, (unsigned)r->cur_port);
	if (proxy_host && !r->secure)
		w9x_snprintf(target, sizeof(target), "http://%s%s", host_header, r->cur_path);
	else
		w9x_snprintf(target, sizeof(target), "%s", r->cur_path);
	free(proxy_host);

	extra_len = strlen(r->headers) + (s->agent ? strlen(s->agent) : 0) + 64;
	extra = (char *)malloc(extra_len);
	if (!extra)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	extra[0] = '\0';
	if (s->agent && *s->agent && !strstr(r->headers, "User-Agent:"))
		w9x_snprintf(extra, extra_len, "User-Agent: %s\r\n", s->agent);
	strcat(extra, r->headers);
	if (r->disabled & WINHTTP_DISABLE_KEEP_ALIVE)
		strcat(extra, "Connection: close\r\n");
	e = whttp_send_request(r->conn, r->verb, target, host_header, extra, optional, optional_len,
	                       (r->total_len > 0 || strcmp(r->verb, "GET")) ? r->total_len : -1);
	free(extra);
	if (e != WHTTP_OK)
		return fail(map_err(e, whttp_conn_tls_error(r->conn)));
	r->sent_body = optional_len;
	return TRUE;
}

WINBOOL WINAPI WinHttpSendRequest(HINTERNET req, LPCWSTR headers, DWORD hlen, LPVOID optional, DWORD olen,
                                  DWORD total, DWORD_PTR context)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	if (!r)
		return FALSE;
	r->h.context = context;
	if (headers && !add_headers(r, headers, hlen, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
		return FALSE;
	if (total < olen)
		total = olen;
	r->total_len = total;
	free(r->optional);
	r->optional = NULL;
	r->optional_len = 0;
	if (olen && total == olen)
	{
		/* keep a copy so a 307/308 redirect can resend the body */
		r->optional = malloc(olen);
		if (r->optional)
		{
			memcpy(r->optional, optional, olen);
			r->optional_len = olen;
		}
	}
	resolve_target(r);
	return open_and_send(r, optional, olen);
}

WINBOOL WINAPI WinHttpWriteData(HINTERNET req, LPCVOID buf, DWORD len, LPDWORD written)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	int e;
	if (!r)
		return FALSE;
	if (!r->conn || r->have_resp)
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
	if (r->sent_body + (long long)len > r->total_len)
		return fail(ERROR_INVALID_PARAMETER);
	e = whttp_send_body(r->conn, buf, len);
	if (e != WHTTP_OK)
		return fail(map_err(e, whttp_conn_tls_error(r->conn)));
	r->sent_body += len;
	if (written)
		*written = len;
	return TRUE;
}

static int is_redirect(int status)
{
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Follow a Location header. Returns 1 if a new request was sent, 0 if the
 * response should be returned to the caller as is, -1 on error. */
static int follow_redirect(request_t *r)
{
	char loc[2048];
	whttp_url u;
	int to_secure;

	if (whttp_find_header(&r->resp, "Location", 0, loc, sizeof(loc)) != 0 || !loc[0])
		return 0;
	if (loc[0] == '/' && loc[1] != '/')
	{
		/* relative to the current origin */
		w9x_snprintf(r->cur_path, sizeof(r->cur_path), "%s", loc);
		to_secure = r->secure;
	}
	else
	{
		if (whttp_parse_url(loc, &u) != WHTTP_OK)
			return 0; /* unusable Location: hand the 3xx to the caller */
		to_secure = u.secure;
		if (r->secure && !to_secure && r->redirect_policy != WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS)
			return 0;
		w9x_snprintf(r->cur_host, sizeof(r->cur_host), "%s", u.host);
		r->cur_port = u.port;
		w9x_snprintf(r->cur_path, sizeof(r->cur_path), "%s", u.path);
	}
	r->secure = to_secure;
	if (r->resp.status == 303 || ((r->resp.status == 301 || r->resp.status == 302) && !strcmp(r->verb, "POST")))
	{
		free(r->verb);
		r->verb = xstrdup("GET");
		r->total_len = 0;
		free(r->optional);
		r->optional = NULL;
		r->optional_len = 0;
	}
	else if (r->total_len > (long long)r->optional_len)
		return 0; /* body was streamed with WriteData: cannot resend it */
	return open_and_send(r, r->optional, r->optional_len) ? 1 : -1;
}

WINBOOL WINAPI WinHttpReceiveResponse(HINTERNET req, LPVOID reserved)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	DWORD hops = 0;
	int e;

	(void)reserved;
	if (!r)
		return FALSE;
	if (!r->conn)
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
	if (r->sent_body < r->total_len)
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
	for (;;)
	{
		e = whttp_read_response(r->conn, r->verb, &r->resp);
		if (e != WHTTP_OK)
		{
			whttp_response_free(&r->resp);
			return fail(map_err(e, whttp_conn_tls_error(r->conn)));
		}
		r->have_resp = 1;
		if (!is_redirect(r->resp.status) || r->redirect_policy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER ||
		    (r->disabled & WINHTTP_DISABLE_REDIRECTS))
			return TRUE;
		if (++hops > r->max_redirects)
			return fail(ERROR_WINHTTP_REDIRECT_FAILED);
		e = follow_redirect(r);
		if (e == 0)
			return TRUE;
		if (e < 0)
			return FALSE;
	}
}

WINBOOL WINAPI WinHttpQueryDataAvailable(HINTERNET req, LPDWORD avail)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	if (!r)
		return FALSE;
	if (!r->have_resp)
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
	if (avail)
		*avail = (DWORD)whttp_body_available(r->conn, &r->resp);
	return TRUE;
}

WINBOOL WINAPI WinHttpReadData(HINTERNET req, LPVOID buf, DWORD len, LPDWORD read)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	int n;
	if (!r)
		return FALSE;
	if (read)
		*read = 0;
	if (!r->have_resp)
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
	if (!buf && len)
		return fail(ERROR_INVALID_PARAMETER);
	n = whttp_read_body(r->conn, &r->resp, buf, len);
	if (n < 0)
		return fail(map_err(n, whttp_conn_tls_error(r->conn)));
	if (read)
		*read = (DWORD)n;
	return TRUE;
}

/* ---------- headers ---------- */

static const char *query_name(DWORD id)
{
	switch (id)
	{
	case WINHTTP_QUERY_CONTENT_TYPE: return "Content-Type";
	case WINHTTP_QUERY_CONTENT_LENGTH: return "Content-Length";
	case WINHTTP_QUERY_CONTENT_LANGUAGE: return "Content-Language";
	case WINHTTP_QUERY_DATE: return "Date";
	case WINHTTP_QUERY_EXPIRES: return "Expires";
	case WINHTTP_QUERY_LAST_MODIFIED: return "Last-Modified";
	case WINHTTP_QUERY_PRAGMA: return "Pragma";
	case WINHTTP_QUERY_CONNECTION: return "Connection";
	case WINHTTP_QUERY_CONTENT_ENCODING: return "Content-Encoding";
	case WINHTTP_QUERY_LOCATION: return "Location";
	case WINHTTP_QUERY_RETRY_AFTER: return "Retry-After";
	case WINHTTP_QUERY_SERVER: return "Server";
	case WINHTTP_QUERY_WWW_AUTHENTICATE: return "WWW-Authenticate";
	case WINHTTP_QUERY_PROXY_AUTHENTICATE: return "Proxy-Authenticate";
	case WINHTTP_QUERY_ACCEPT_RANGES: return "Accept-Ranges";
	case WINHTTP_QUERY_SET_COOKIE: return "Set-Cookie";
	case WINHTTP_QUERY_CONTENT_DISPOSITION: return "Content-Disposition";
	case WINHTTP_QUERY_AGE: return "Age";
	case WINHTTP_QUERY_CACHE_CONTROL: return "Cache-Control";
	case WINHTTP_QUERY_CONTENT_LOCATION: return "Content-Location";
	case WINHTTP_QUERY_CONTENT_RANGE: return "Content-Range";
	case WINHTTP_QUERY_ETAG: return "ETag";
	case WINHTTP_QUERY_TRANSFER_ENCODING: return "Transfer-Encoding";
	case WINHTTP_QUERY_UPGRADE: return "Upgrade";
	case WINHTTP_QUERY_VARY: return "Vary";
	case WINHTTP_QUERY_VIA: return "Via";
	case WINHTTP_QUERY_WARNING: return "Warning";
	default: return NULL;
	}
}

/* Copy a byte string out as UTF-16 (bytes taken as Latin-1). */
static int out_wide(const char *v, size_t n, LPVOID buf, LPDWORD len)
{
	DWORD need = (DWORD)((n + 1) * sizeof(WCHAR));
	size_t i;
	if (!buf || *len < need)
	{
		*len = need;
		return fail(ERROR_INSUFFICIENT_BUFFER);
	}
	for (i = 0; i < n; i++)
		((WCHAR *)buf)[i] = (unsigned char)v[i];
	((WCHAR *)buf)[n] = 0;
	*len = (DWORD)(n * sizeof(WCHAR));
	return TRUE;
}

WINBOOL WINAPI WinHttpQueryHeaders(HINTERNET req, DWORD level, LPCWSTR name, LPVOID buf, LPDWORD len,
                                   LPDWORD index)
{
	request_t *r = (request_t *)get_handle(req, H_REQUEST);
	DWORD id = level & 0xFFFF, mods = level & 0xFFFF0000u;
	char value[8192];
	const char *hname;
	char *custom = NULL;
	int idx = index ? (int)*index : 0, found;

	if (!r)
		return FALSE;
	if (!len)
		return fail(ERROR_INVALID_PARAMETER);
	if (!r->have_resp)
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
	if (mods & WINHTTP_QUERY_FLAG_REQUEST_HEADERS)
		return fail(ERROR_WINHTTP_HEADER_NOT_FOUND); /* not kept in this version */

	if (id == WINHTTP_QUERY_STATUS_CODE)
	{
		if (mods & WINHTTP_QUERY_FLAG_NUMBER)
		{
			if (!buf || *len < sizeof(DWORD))
			{
				*len = sizeof(DWORD);
				return fail(ERROR_INSUFFICIENT_BUFFER);
			}
			*(DWORD *)buf = (DWORD)r->resp.status;
			*len = sizeof(DWORD);
			return TRUE;
		}
		w9x_snprintf(value, sizeof(value), "%d", r->resp.status);
		return out_wide(value, strlen(value), buf, len);
	}
	if (id == WINHTTP_QUERY_STATUS_TEXT)
		return out_wide(r->resp.reason, strlen(r->resp.reason), buf, len);
	if (id == WINHTTP_QUERY_VERSION)
		return out_wide(r->resp.raw_headers, 8, buf, len);
	if (id == WINHTTP_QUERY_RAW_HEADERS_CRLF)
		return out_wide(r->resp.raw_headers, r->resp.raw_len, buf, len);
	if (id == WINHTTP_QUERY_RAW_HEADERS)
	{
		/* NUL-separated lines, double NUL at the end */
		DWORD need = (DWORD)((r->resp.raw_len + 1) * sizeof(WCHAR));
		size_t i, o = 0;
		if (!buf || *len < need)
		{
			*len = need;
			return fail(ERROR_INSUFFICIENT_BUFFER);
		}
		for (i = 0; i < r->resp.raw_len; i++)
		{
			char ch = r->resp.raw_headers[i];
			if (ch == '\r')
				continue;
			((WCHAR *)buf)[o++] = ch == '\n' ? 0 : (unsigned char)ch;
		}
		((WCHAR *)buf)[o] = 0;
		*len = (DWORD)(o * sizeof(WCHAR));
		return TRUE;
	}

	if (id == WINHTTP_QUERY_CUSTOM)
	{
		if (!name)
			return fail(ERROR_INVALID_PARAMETER);
		custom = w2u8(name, -1);
		hname = custom;
	}
	else
		hname = query_name(id);
	if (!hname)
		return fail(ERROR_WINHTTP_HEADER_NOT_FOUND);
	found = whttp_find_header(&r->resp, hname, idx, value, sizeof(value)) == 0;
	free(custom);
	if (!found)
		return fail(ERROR_WINHTTP_HEADER_NOT_FOUND);
	if (mods & WINHTTP_QUERY_FLAG_NUMBER)
	{
		if (!buf || *len < sizeof(DWORD))
		{
			*len = sizeof(DWORD);
			return fail(ERROR_INSUFFICIENT_BUFFER);
		}
		*(DWORD *)buf = (DWORD)strtoul(value, NULL, 10);
		*len = sizeof(DWORD);
	}
	else if (mods & 0x08000000 /* WINHTTP_QUERY_FLAG_NUMBER64 */)
	{
		unsigned long long v = 0;
		const char *q;
		if (!buf || *len < sizeof(v))
		{
			*len = sizeof(v);
			return fail(ERROR_INSUFFICIENT_BUFFER);
		}
		for (q = value; *q >= '0' && *q <= '9'; q++)
			v = v * 10 + (unsigned)(*q - '0');
		memcpy(buf, &v, sizeof(v));
		*len = sizeof(v);
	}
	else if (!out_wide(value, strlen(value), buf, len))
		return FALSE;
	if (index)
		(*index)++;
	return TRUE;
}

/* ---------- options ---------- */

static int set_dword_opt(DWORD *slot, LPVOID buf, DWORD len)
{
	if (!buf || len < sizeof(DWORD))
		return fail(ERROR_INSUFFICIENT_BUFFER);
	*slot = *(DWORD *)buf;
	return TRUE;
}

WINBOOL WINAPI WinHttpSetOption(HINTERNET h, DWORD option, LPVOID buf, DWORD len)
{
	hbase *b = get_handle(h, 0);
	session_t *s = NULL;
	request_t *r = NULL;
	DWORD v, *secure, *redirect, *maxr, *disabled;
	whttp_timeouts *t;

	if (!b)
		return FALSE;
	if (b->type == H_SESSION)
		s = (session_t *)b;
	else if (b->type == H_REQUEST)
		r = (request_t *)b;
	if (!s && !r)
	{
		/* connect handles carry no options in this implementation */
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
	}
	secure = s ? &s->secure_protocols : &r->secure_protocols;
	redirect = s ? &s->redirect_policy : &r->redirect_policy;
	maxr = s ? &s->max_redirects : &r->max_redirects;
	disabled = s ? &s->disabled : &r->disabled;
	t = s ? &s->t : &r->t;

	switch (option)
	{
	case WINHTTP_OPTION_SECURE_PROTOCOLS:
		if (!set_dword_opt(&v, buf, len))
			return FALSE;
		*secure = v & (TLS12 | TLS13); /* older protocols are never offered */
		return TRUE;
	case WINHTTP_OPTION_REDIRECT_POLICY:
		if (!set_dword_opt(&v, buf, len))
			return FALSE;
		if (v > WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS)
			return fail(ERROR_INVALID_PARAMETER);
		*redirect = v;
		return TRUE;
	case WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS:
		return set_dword_opt(maxr, buf, len);
	case WINHTTP_OPTION_DISABLE_FEATURE:
		if (!set_dword_opt(&v, buf, len))
			return FALSE;
		*disabled |= v;
		return TRUE;
	case WINHTTP_OPTION_CONNECT_TIMEOUT:
		return set_dword_opt((DWORD *)&t->connect_ms, buf, len);
	case WINHTTP_OPTION_SEND_TIMEOUT:
		return set_dword_opt((DWORD *)&t->send_ms, buf, len);
	case WINHTTP_OPTION_RECEIVE_TIMEOUT:
	case WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT:
		return set_dword_opt((DWORD *)&t->receive_ms, buf, len);
	case WINHTTP_OPTION_RESOLVE_TIMEOUT:
	case WINHTTP_OPTION_CONNECT_RETRIES:
		return set_dword_opt(&v, buf, len); /* accepted, no effect */
	case WINHTTP_OPTION_SECURITY_FLAGS:
		/* Only requests that do not weaken verification are accepted. */
		if (!set_dword_opt(&v, buf, len))
			return FALSE;
		if (v & (SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
		         SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE))
		{
			OutputDebugStringA("winhttp(KernelEx): refusing to disable certificate checks\n");
			return fail(ERROR_INVALID_PARAMETER);
		}
		return TRUE;
	case WINHTTP_OPTION_USER_AGENT:
		if (!s)
			return fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
		free(s->agent);
		s->agent = buf ? w2u8((const WCHAR *)buf, (int)len) : NULL;
		return TRUE;
	case WINHTTP_OPTION_CONTEXT_VALUE:
		if (!buf || len < sizeof(DWORD_PTR))
			return fail(ERROR_INSUFFICIENT_BUFFER);
		b->context = *(DWORD_PTR *)buf;
		return TRUE;
	case WINHTTP_OPTION_ENABLE_FEATURE:
	case 118: /* WINHTTP_OPTION_DECOMPRESSION: we never ask for compression */
	case 133: /* WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL (HTTP/2): HTTP/1.1 only */
		return TRUE;
	default:
		return fail(ERROR_WINHTTP_INVALID_OPTION);
	}
}

WINBOOL WINAPI WinHttpQueryOption(HINTERNET h, DWORD option, LPVOID buf, LPDWORD len)
{
	hbase *b = get_handle(h, 0);
	DWORD v;

	if (!b)
		return FALSE;
	if (!len)
		return fail(ERROR_INVALID_PARAMETER);
	switch (option)
	{
	case WINHTTP_OPTION_SECURE_PROTOCOLS:
		v = b->type == H_SESSION ? ((session_t *)b)->secure_protocols
		    : b->type == H_REQUEST ? ((request_t *)b)->secure_protocols : TLS12 | TLS13;
		break;
	case WINHTTP_OPTION_CONTEXT_VALUE:
		if (!buf || *len < sizeof(DWORD_PTR))
		{
			*len = sizeof(DWORD_PTR);
			return fail(ERROR_INSUFFICIENT_BUFFER);
		}
		*(DWORD_PTR *)buf = b->context;
		*len = sizeof(DWORD_PTR);
		return TRUE;
	case WINHTTP_OPTION_URL:
		if (b->type == H_REQUEST)
		{
			request_t *r = (request_t *)b;
			char url[2600];
			size_t n;
			n = (size_t)w9x_snprintf(url, sizeof(url), "%s://%s:%u%s", r->secure ? "https" : "http", r->cur_host,
			                         (unsigned)r->cur_port, r->cur_path);
			return out_wide(url, n < sizeof(url) ? n : sizeof(url) - 1, buf, len);
		}
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
	default:
		return fail(ERROR_WINHTTP_INVALID_OPTION);
	}
	if (!buf || *len < sizeof(DWORD))
	{
		*len = sizeof(DWORD);
		return fail(ERROR_INSUFFICIENT_BUFFER);
	}
	*(DWORD *)buf = v;
	*len = sizeof(DWORD);
	return TRUE;
}

WINBOOL WINAPI WinHttpSetTimeouts(HINTERNET h, int resolve, int connect, int send, int receive)
{
	hbase *b = get_handle(h, 0);
	whttp_timeouts *t;
	(void)resolve;
	if (!b)
		return FALSE;
	if (connect < -1 || send < -1 || receive < -1)
		return fail(ERROR_INVALID_PARAMETER);
	if (b->type == H_SESSION)
		t = &((session_t *)b)->t;
	else if (b->type == H_REQUEST)
		t = &((request_t *)b)->t;
	else
		return fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
	/* -1 / 0 mean infinite in WinHTTP; whttp treats <= 0 as no timeout */
	t->connect_ms = connect > 0 ? connect : 0;
	t->send_ms = send > 0 ? send : 0;
	t->receive_ms = receive > 0 ? receive : 0;
	return TRUE;
}

WINHTTP_STATUS_CALLBACK WINAPI WinHttpSetStatusCallback(HINTERNET h, WINHTTP_STATUS_CALLBACK cb, DWORD flags,
                                                        DWORD_PTR reserved)
{
	(void)cb;
	(void)flags;
	(void)reserved;
	if (!get_handle(h, 0))
		return WINHTTP_INVALID_STATUS_CALLBACK;
	/* Synchronous-only build: callbacks are accepted but never invoked. */
	return NULL;
}

WINBOOL WINAPI WinHttpCheckPlatform(void)
{
	return TRUE;
}

/* ---------- URLs ---------- */

/* Component rules follow Wine's set_component (dlls/winhttp/url.c, LGPL):
 * NULL buffer + nonzero length -> pointer into the URL; buffer + zero length
 * -> invalid parameter; too small -> keep going, report overflow at the end. */
static int put_component(LPWSTR *ptr, DWORD *len, const WCHAR *src, size_t n, int *overflow)
{
	if (*ptr && !*len)
		return fail(ERROR_INVALID_PARAMETER);
	if (!*len)
		return TRUE;
	if (!*ptr)
	{
		*ptr = (LPWSTR)src;
		*len = (DWORD)n;
		return TRUE;
	}
	if (n >= *len)
	{
		*len = (DWORD)(n + 1);
		*overflow = 1;
		return TRUE;
	}
	memcpy(*ptr, src, n * sizeof(WCHAR));
	(*ptr)[n] = 0;
	*len = (DWORD)n;
	return TRUE;
}

static int w_ieq(const WCHAR *w, const char *a, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
	{
		WCHAR c = w[i];
		if (c >= 'A' && c <= 'Z')
			c = (WCHAR)(c + 32);
		if (c != (WCHAR)a[i])
			return 0;
	}
	return 1;
}

WINBOOL WINAPI WinHttpCrackUrl(LPCWSTR url, DWORD url_len, DWORD flags, LPURL_COMPONENTS uc)
{
	size_t n, scheme_len, i;
	const WCHAR *p, *host, *host_end, *path, *extra, *end, *user = NULL, *user_end = NULL;
	INTERNET_SCHEME scheme;
	INTERNET_PORT port;
	int overflow = 0;

	(void)flags; /* ICU_DECODE / ICU_ESCAPE: not applied */
	if (!url || !uc || uc->dwStructSize != sizeof(URL_COMPONENTS))
		return fail(ERROR_INVALID_PARAMETER);
	n = url_len ? url_len : wlen(url, -1);
	end = url + n;
	for (scheme_len = 0; scheme_len < n && url[scheme_len] != ':'; scheme_len++)
		;
	if (scheme_len + 3 > n || url[scheme_len + 1] != '/' || url[scheme_len + 2] != '/')
		return fail(ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
	if (scheme_len == 5 && w_ieq(url, "https", 5))
	{
		scheme = INTERNET_SCHEME_HTTPS;
		port = INTERNET_DEFAULT_HTTPS_PORT;
	}
	else if (scheme_len == 4 && w_ieq(url, "http", 4))
	{
		scheme = INTERNET_SCHEME_HTTP;
		port = INTERNET_DEFAULT_HTTP_PORT;
	}
	else
		return fail(ERROR_WINHTTP_UNRECOGNIZED_SCHEME);

	host = url + scheme_len + 3;
	for (p = host; p < end && *p != '/' && *p != '?' && *p != '#'; p++)
		if (*p == '@')
		{
			user = host;
			user_end = p;
		}
	path = p;
	if (user)
		host = user_end + 1;
	host_end = path;
	for (p = host; p < path; p++)
		if (*p == ':')
		{
			unsigned long v = 0;
			const WCHAR *q;
			host_end = p;
			for (q = p + 1; q < path; q++)
			{
				if (*q < '0' || *q > '9')
					return fail(ERROR_WINHTTP_INVALID_URL);
				v = v * 10 + (unsigned long)(*q - '0');
				if (v > 65535)
					return fail(ERROR_WINHTTP_INVALID_URL);
			}
			if (q > p + 1)
				port = (INTERNET_PORT)v;
			break;
		}
	if (host_end == host)
		return fail(ERROR_WINHTTP_INVALID_URL);
	for (extra = path; extra < end && *extra != '?' && *extra != '#'; extra++)
		;

	uc->nScheme = scheme;
	uc->nPort = port;
	if (!put_component(&uc->lpszScheme, &uc->dwSchemeLength, url, scheme_len, &overflow) ||
	    !put_component(&uc->lpszHostName, &uc->dwHostNameLength, host, (size_t)(host_end - host), &overflow) ||
	    !put_component(&uc->lpszUrlPath, &uc->dwUrlPathLength, path, (size_t)(extra - path), &overflow) ||
	    !put_component(&uc->lpszExtraInfo, &uc->dwExtraInfoLength, extra, (size_t)(end - extra), &overflow))
		return FALSE;
	if (user)
	{
		const WCHAR *colon = user;
		for (i = 0; user + i < user_end && user[i] != ':'; i++)
			;
		colon = user + i;
		if (!put_component(&uc->lpszUserName, &uc->dwUserNameLength, user, (size_t)(colon - user), &overflow) ||
		    !put_component(&uc->lpszPassword, &uc->dwPasswordLength, colon < user_end ? colon + 1 : user_end,
		                   colon < user_end ? (size_t)(user_end - colon - 1) : 0, &overflow))
			return FALSE;
	}
	else
	{
		if (!put_component(&uc->lpszUserName, &uc->dwUserNameLength, host, 0, &overflow) ||
		    !put_component(&uc->lpszPassword, &uc->dwPasswordLength, host, 0, &overflow))
			return FALSE;
	}
	return overflow ? fail(ERROR_INSUFFICIENT_BUFFER) : TRUE;
}

/* ---------- proxy discovery ---------- */

static LPWSTR global_wide(const char *s)
{
	size_t n, i;
	LPWSTR w;
	if (!s)
		return NULL;
	n = strlen(s);
	w = (LPWSTR)GlobalAlloc(GMEM_FIXED, (n + 1) * sizeof(WCHAR));
	if (!w)
		return NULL;
	for (i = 0; i <= n; i++)
		w[i] = (unsigned char)s[i];
	return w;
}

WINBOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *cfg)
{
	char *srv, *byp;
	int en;
	if (!cfg)
		return fail(ERROR_INVALID_PARAMETER);
	memset(cfg, 0, sizeof(*cfg));
	ie_proxy(&srv, &byp, &en);
	if (en && srv)
	{
		cfg->lpszProxy = global_wide(srv);
		cfg->lpszProxyBypass = global_wide(byp);
	}
	free(srv);
	free(byp);
	return TRUE;
}

WINBOOL WINAPI WinHttpGetDefaultProxyConfiguration(WINHTTP_PROXY_INFO *info)
{
	if (!info)
		return fail(ERROR_INVALID_PARAMETER);
	memset(info, 0, sizeof(*info));
	info->dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(inst);
	return TRUE;
}
