/*
 *  KernelEx kextls: TLS client sessions.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 */
#include <stdlib.h>
#include <string.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "psa/crypto.h"
#include "kextls.h"
#include "kextls_internal.h"
#include "w9xrt.h"

/* AEAD only; ChaCha20-Poly1305 first because Pentium-class CPUs have no
 * AES instructions and constant-time software AES is slow there. */
static const int suites[] = {
	MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
	MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	0
};

static const uint16_t groups[] = {
	MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_NONE
};

/* Chain policy: SHA-2 only, RSA >= 2048, ECDSA on P-256/P-384/P-521. */
static const mbedtls_x509_crt_profile chain_profile = {
	MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA256) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA384) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA512),
	MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_RSA) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_RSASSA_PSS) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_ECKEY) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_ECDSA),
	MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP256R1) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP384R1) |
	MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP521R1),
	2048,
};

#define MAX_ALPN 4

struct kextls_session
{
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	kextls_send_fn send_fn;
	kextls_recv_fn recv_fn;
	void *io;
	const char *alpn_list[MAX_ALPN + 1];
	char alpn_store[64];
	int last;            /* last KEXTLS_* result */
	char last_error[128];
};

static kextls_lock psa_lock;
static int psa_ready;

static int ensure_psa(void)
{
	int ok;
	kextls_lock_enter(&psa_lock);
	if (!psa_ready && psa_crypto_init() == PSA_SUCCESS)
		psa_ready = 1;
	ok = psa_ready;
	kextls_lock_leave(&psa_lock);
	return ok;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	kextls_session *s = (kextls_session *)ctx;
	int r = s->send_fn(s->io, buf, len);
	if (r == KEXTLS_E_WANT_WRITE)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	if (r == KEXTLS_E_WANT_READ)
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (r < 0)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	kextls_session *s = (kextls_session *)ctx;
	int r = s->recv_fn(s->io, buf, len);
	if (r == KEXTLS_E_WANT_READ)
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (r == KEXTLS_E_WANT_WRITE)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	if (r < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return r; /* 0 = EOF */
}

static void set_error(kextls_session *s, int mbed, const char *what)
{
	char buf[96];
	mbedtls_strerror(mbed, buf, sizeof(buf));
	w9x_snprintf(s->last_error, sizeof(s->last_error), "%s: %s (-0x%04X)", what, buf, (unsigned)-mbed);
}

static int map_verify_flags(unsigned int flags)
{
	if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
		return KEXTLS_E_CERT_NAME;
	if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
		return KEXTLS_E_CERT_DATE;
	if (flags & (MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_MISSING))
		return KEXTLS_E_CERT_UNTRUSTED;
	return KEXTLS_E_CERT_OTHER;
}

static int map_error(kextls_session *s, int r, const char *what)
{
	if (r >= 0)
		return r;
	switch (r)
	{
	case MBEDTLS_ERR_SSL_WANT_READ:
		return KEXTLS_E_WANT_READ;
	case MBEDTLS_ERR_SSL_WANT_WRITE:
		return KEXTLS_E_WANT_WRITE;
	case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
		return KEXTLS_E_CLOSED;
	case MBEDTLS_ERR_NET_SEND_FAILED:
	case MBEDTLS_ERR_NET_RECV_FAILED:
		set_error(s, r, what);
		return KEXTLS_E_IO;
	case MBEDTLS_ERR_X509_CERT_VERIFY_FAILED:
		set_error(s, r, what);
		return map_verify_flags(mbedtls_ssl_get_verify_result(&s->ssl));
	case MBEDTLS_ERR_SSL_ALLOC_FAILED:
		return KEXTLS_E_NOMEM;
	default:
		set_error(s, r, what);
		return KEXTLS_E_PROTOCOL;
	}
}

static int parse_alpn(kextls_session *s, const char *alpn)
{
	char *p;
	int n = 0;

	if (!alpn || !*alpn)
		return 1;
	if (strlen(alpn) >= sizeof(s->alpn_store))
		return 0;
	strcpy(s->alpn_store, alpn);
	p = s->alpn_store;
	while (*p && n < MAX_ALPN)
	{
		char *comma = strchr(p, ',');
		s->alpn_list[n++] = p;
		if (!comma)
			break;
		*comma = '\0';
		p = comma + 1;
	}
	s->alpn_list[n] = NULL;
	return 1;
}

kextls_session *kextls_client_new(const char *host, const char *alpn,
                                  kextls_send_fn send_fn, kextls_recv_fn recv_fn,
                                  void *io, int *error)
{
	kextls_session *s;
	mbedtls_x509_crt *roots;
	int nroots, r, e = KEXTLS_OK;

	if (error)
		*error = KEXTLS_OK;
	if (!host || !*host || !send_fn || !recv_fn)
	{
		if (error)
			*error = KEXTLS_E_PARAM;
		return NULL;
	}
	if (w9x_time(NULL) < kextls_trust_build_time() - 86400)
	{
		if (error)
			*error = KEXTLS_E_CLOCK;
		return NULL;
	}
	if (!ensure_psa())
	{
		if (error)
			*error = KEXTLS_E_NO_ENTROPY;
		return NULL;
	}
	roots = kextls_trust_roots(&nroots);
	if (!roots)
	{
		if (error)
			*error = nroots < 0 ? nroots : KEXTLS_E_NO_ROOTS;
		return NULL;
	}

	s = (kextls_session *)calloc(1, sizeof(*s));
	if (!s)
	{
		if (error)
			*error = KEXTLS_E_NOMEM;
		return NULL;
	}
	s->send_fn = send_fn;
	s->recv_fn = recv_fn;
	s->io = io;
	mbedtls_ssl_init(&s->ssl);
	mbedtls_ssl_config_init(&s->conf);

	r = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
	                                MBEDTLS_SSL_PRESET_DEFAULT);
	if (r != 0)
		goto fail;
	mbedtls_ssl_conf_min_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_3);
	mbedtls_ssl_conf_ciphersuites(&s->conf, suites);
	mbedtls_ssl_conf_groups(&s->conf, groups);
	mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&s->conf, roots, NULL);
	mbedtls_ssl_conf_cert_profile(&s->conf, &chain_profile);
	mbedtls_ssl_conf_rng(&s->conf, kextls_rng, NULL);
	if (!parse_alpn(s, alpn))
	{
		e = KEXTLS_E_PARAM;
		goto fail_e;
	}
	if (s->alpn_list[0] && (r = mbedtls_ssl_conf_alpn_protocols(&s->conf, s->alpn_list)) != 0)
		goto fail;

	if ((r = mbedtls_ssl_setup(&s->ssl, &s->conf)) != 0)
		goto fail;
	/* SNI and the name the certificate must match (never skipped). */
	if ((r = mbedtls_ssl_set_hostname(&s->ssl, host)) != 0)
		goto fail;
	mbedtls_ssl_set_bio(&s->ssl, s, bio_send, bio_recv, NULL);
	return s;

fail:
	e = r == MBEDTLS_ERR_SSL_ALLOC_FAILED ? KEXTLS_E_NOMEM : KEXTLS_E_PROTOCOL;
fail_e:
	if (error)
		*error = e;
	kextls_free(s);
	return NULL;
}

int kextls_handshake(kextls_session *s)
{
	int r;
	if (!s)
		return KEXTLS_E_PARAM;
	r = mbedtls_ssl_handshake(&s->ssl);
	s->last = map_error(s, r, "handshake");
	return s->last;
}

int kextls_read(kextls_session *s, void *buf, size_t len)
{
	int r;
	if (!s || (!buf && len))
		return KEXTLS_E_PARAM;
	for (;;)
	{
		r = mbedtls_ssl_read(&s->ssl, (unsigned char *)buf, len);
		/* TLS 1.3 NewSessionTicket arrives after the handshake: not data. */
		if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			continue;
		break;
	}
	if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return 0;
	s->last = map_error(s, r, "read");
	return s->last;
}

int kextls_write(kextls_session *s, const void *buf, size_t len)
{
	int r;
	if (!s || (!buf && len))
		return KEXTLS_E_PARAM;
	r = mbedtls_ssl_write(&s->ssl, (const unsigned char *)buf, len);
	s->last = map_error(s, r, "write");
	return s->last;
}

size_t kextls_pending(kextls_session *s)
{
	return s ? mbedtls_ssl_get_bytes_avail(&s->ssl) : 0;
}

int kextls_close_notify(kextls_session *s)
{
	if (!s)
		return KEXTLS_E_PARAM;
	return map_error(s, mbedtls_ssl_close_notify(&s->ssl), "close");
}

void kextls_free(kextls_session *s)
{
	if (!s)
		return;
	mbedtls_ssl_free(&s->ssl);
	mbedtls_ssl_config_free(&s->conf);
	memset(s, 0, sizeof(*s));
	free(s);
}

static void copy_str(char *dst, size_t size, const char *src)
{
	if (!src)
		src = "";
	strncpy(dst, src, size - 1);
	dst[size - 1] = '\0';
}

int kextls_get_info(kextls_session *s, kextls_info *info)
{
	const mbedtls_x509_crt *peer;
	unsigned int size;

	if (!s || !info || info->struct_size < sizeof(kextls_info))
		return KEXTLS_E_PARAM;
	size = info->struct_size;
	memset(info, 0, sizeof(*info));
	info->struct_size = size;
	copy_str(info->protocol, sizeof(info->protocol), mbedtls_ssl_get_version(&s->ssl));
	copy_str(info->cipher, sizeof(info->cipher), mbedtls_ssl_get_ciphersuite(&s->ssl));
	copy_str(info->alpn, sizeof(info->alpn), mbedtls_ssl_get_alpn_protocol(&s->ssl));
	peer = mbedtls_ssl_get_peer_cert(&s->ssl);
	if (peer)
	{
		mbedtls_x509_dn_gets(info->peer_subject, sizeof(info->peer_subject), &peer->subject);
		mbedtls_x509_dn_gets(info->peer_issuer, sizeof(info->peer_issuer), &peer->issuer);
	}
	info->verify_flags = mbedtls_ssl_get_verify_result(&s->ssl);
	copy_str(info->last_error, sizeof(info->last_error), s->last_error);
	return KEXTLS_OK;
}

const char *kextls_strerror(int code)
{
	switch (code)
	{
	case KEXTLS_OK: return "success";
	case KEXTLS_E_WANT_READ: return "operation would block (read)";
	case KEXTLS_E_WANT_WRITE: return "operation would block (write)";
	case KEXTLS_E_CLOSED: return "connection closed by peer";
	case KEXTLS_E_IO: return "network I/O error";
	case KEXTLS_E_PROTOCOL: return "TLS protocol error";
	case KEXTLS_E_CERT_UNTRUSTED: return "certificate is not signed by a trusted root";
	case KEXTLS_E_CERT_NAME: return "certificate does not match the host name";
	case KEXTLS_E_CERT_DATE: return "certificate has expired or is not yet valid";
	case KEXTLS_E_CERT_OTHER: return "certificate uses a rejected algorithm or usage";
	case KEXTLS_E_CLOCK: return "system clock is earlier than the trust store; set the date";
	case KEXTLS_E_NOMEM: return "out of memory";
	case KEXTLS_E_PARAM: return "invalid parameter";
	case KEXTLS_E_NO_ENTROPY: return "no usable entropy source";
	case KEXTLS_E_NO_ROOTS: return "trust store is missing or empty";
	default: return "unknown error";
	}
}

int kextls_abi_version(void)
{
	return KEXTLS_ABI_VERSION;
}
