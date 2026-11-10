/*
 *  KernelEx kextls: cryptographic random numbers and TLS for Windows 9x.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  kextls.dll is the one place on the system that holds Mbed TLS. Other
 *  KernelEx modules (bcrypt.dll, winhttp.dll, the future SChannel provider)
 *  use it through this narrow C interface, so the TLS backend can be
 *  upgraded without touching them.
 *
 *  Security policy (not configurable through this API):
 *    - TLS 1.2 or TLS 1.3 only; AEAD cipher suites; ECDHE only.
 *    - The server certificate chain is always verified against the trust
 *      store, and the host name is always checked (and sent as SNI).
 *    - SHA-1 and RSA keys under 2048 bits are rejected in chains.
 *    - If the system clock is earlier than the trust store's build date,
 *      connections fail with KEXTLS_E_CLOCK rather than skipping date checks.
 */
#ifndef KEXTLS_H
#define KEXTLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(KEXTLS_STATIC)
#  ifdef KEXTLS_BUILD
#    define KEXTLS_API __declspec(dllexport)
#  else
#    define KEXTLS_API __declspec(dllimport)
#  endif
#else
#  define KEXTLS_API
#endif

#define KEXTLS_ABI_VERSION 1

/* ---------- errors ---------- */
#define KEXTLS_OK              0
#define KEXTLS_E_WANT_READ    -1   /* transport would block; retry */
#define KEXTLS_E_WANT_WRITE   -2
#define KEXTLS_E_CLOSED       -3   /* peer closed the connection cleanly */
#define KEXTLS_E_IO           -4   /* transport error */
#define KEXTLS_E_PROTOCOL     -5   /* handshake or record failure */
#define KEXTLS_E_CERT_UNTRUSTED  -10 /* chain does not end in a trusted root */
#define KEXTLS_E_CERT_NAME    -11  /* certificate does not match host name */
#define KEXTLS_E_CERT_DATE    -12  /* expired or not yet valid */
#define KEXTLS_E_CERT_OTHER   -13  /* weak algorithm, bad usage, revoked... */
#define KEXTLS_E_CLOCK        -14  /* system clock is implausible */
#define KEXTLS_E_NOMEM        -20
#define KEXTLS_E_PARAM        -21
#define KEXTLS_E_NO_ENTROPY   -22  /* no usable entropy source */
#define KEXTLS_E_NO_ROOTS     -23  /* trust store missing or empty */

/* ---------- random numbers ---------- */

/* Fills buf with len bytes from the per-process HMAC_DRBG (SHA-256),
 * seeded on first use from CryptoAPI, CPU timing jitter and system state.
 * Thread-safe. Returns KEXTLS_OK or KEXTLS_E_NO_ENTROPY. */
KEXTLS_API int kextls_random(void *buf, size_t len);

/* Entropy health of the last (re)seed, for diagnostics. */
KEXTLS_API void kextls_last_entropy_report(int *cryptoapi, int *jitter_distinct);

/* ---------- message digests and HMAC (used by bcrypt.dll) ---------- */
#define KEXTLS_MD_MD5     1   /* for checksums only; never for signatures */
#define KEXTLS_MD_SHA1    2
#define KEXTLS_MD_SHA256  3
#define KEXTLS_MD_SHA384  4
#define KEXTLS_MD_SHA512  5

typedef struct kextls_digest kextls_digest;

/* hmac_key == NULL: plain hash; otherwise HMAC with that key. */
KEXTLS_API kextls_digest *kextls_digest_new(int alg, const void *hmac_key, size_t key_len);
KEXTLS_API int kextls_digest_update(kextls_digest *d, const void *data, size_t len);
/* Writes the digest and resets d so it can hash again (same key). */
KEXTLS_API int kextls_digest_final(kextls_digest *d, void *out, size_t out_len);
KEXTLS_API kextls_digest *kextls_digest_dup(const kextls_digest *d);
KEXTLS_API void kextls_digest_free(kextls_digest *d);
KEXTLS_API size_t kextls_digest_size(int alg);   /* 0 if unknown */
KEXTLS_API size_t kextls_digest_block(int alg);

/* ---------- TLS client sessions ---------- */

/* Transport callbacks supplied by the caller (e.g. a Winsock socket).
 * send/recv return bytes transferred, 0 on orderly close (recv only),
 * KEXTLS_E_WANT_READ / KEXTLS_E_WANT_WRITE, or KEXTLS_E_IO. */
typedef int (*kextls_send_fn)(void *io, const unsigned char *buf, size_t len);
typedef int (*kextls_recv_fn)(void *io, unsigned char *buf, size_t len);

typedef struct kextls_session kextls_session;

/* host: DNS name or IP literal the certificate must match (required).
 * alpn: comma-separated protocol list such as "http/1.1", or NULL. */
KEXTLS_API kextls_session *kextls_client_new(const char *host, const char *alpn,
                                             kextls_send_fn send_fn, kextls_recv_fn recv_fn,
                                             void *io, int *error);

/* Run the handshake to completion (blocking transport) or one step
 * (non-blocking transport: returns WANT_READ/WANT_WRITE). */
KEXTLS_API int kextls_handshake(kextls_session *s);

/* Application data. Return bytes, 0 at clean close (read), or an error. */
KEXTLS_API int kextls_read(kextls_session *s, void *buf, size_t len);
KEXTLS_API int kextls_write(kextls_session *s, const void *buf, size_t len);

/* Bytes already decrypted and waiting in the session. */
KEXTLS_API size_t kextls_pending(kextls_session *s);

KEXTLS_API int kextls_close_notify(kextls_session *s);
KEXTLS_API void kextls_free(kextls_session *s);

typedef struct
{
	unsigned int struct_size;   /* set to sizeof(kextls_info) by the caller */
	char protocol[16];          /* "TLSv1.3", "TLSv1.2" */
	char cipher[64];            /* IANA-style suite name from Mbed TLS */
	char alpn[32];              /* negotiated ALPN or "" */
	char peer_subject[256];     /* leaf certificate subject DN */
	char peer_issuer[256];
	unsigned int verify_flags;  /* raw MBEDTLS_X509_BADCERT_* bits, 0 = OK */
	char last_error[128];       /* human-readable last failure */
} kextls_info;

KEXTLS_API int kextls_get_info(kextls_session *s, kextls_info *info);

/* Human-readable text for a KEXTLS_E_* code. */
KEXTLS_API const char *kextls_strerror(int code);

/* ---------- trust store ---------- */

/* Number of roots loaded (loading them on first call), or a negative
 * KEXTLS_E_* code. The store is <kextls.dll dir>\certs\roots.pem plus any
 * certs\local\*.pem; on test hosts, the KEXTLS_CA_FILE environment variable. */
KEXTLS_API int kextls_trust_store_count(void);

/* Replace the trust store for this process from PEM text (tests, tools). */
KEXTLS_API int kextls_trust_store_load_pem(const char *pem, size_t len);

KEXTLS_API int kextls_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* KEXTLS_H */
