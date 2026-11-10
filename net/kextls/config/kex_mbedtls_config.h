/*
 *  KernelEx kextls: Mbed TLS 3.6 configuration for Windows 95/98/ME
 *  (and for the host test build on Linux).
 *  Licensed under the GNU General Public License version 2.
 *
 *  Policy encoded here:
 *    - TLS 1.3 and TLS 1.2 client only; no SSL 3.0 / TLS 1.0 / TLS 1.1 code
 *      exists in Mbed TLS 3.6 at all.
 *    - TLS 1.2 limited to ECDHE key exchange with AEAD ciphers (GCM,
 *      ChaCha20-Poly1305). No static RSA, no CBC, no RC4/3DES/NULL.
 *    - Certificates: RSA (PKCS#1 v1.5 and PSS), ECDSA P-256/P-384/P-521,
 *      SHA-256/384/512. SHA-1 is compiled for parsing old roots only; the
 *      verification profile in kextls rejects SHA-1 signatures.
 *    - No run-time CPU feature probing (AES-NI, VIA PadLock): Pentium-class
 *      targets have neither, and probing on unusual CPUs is a risk.
 *    - Randomness only from kextls's DRBG (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG).
 */
#ifndef KEX_MBEDTLS_CONFIG_H
#define KEX_MBEDTLS_CONFIG_H

/* ---------- system ---------- */
#define MBEDTLS_HAVE_ASM            /* i386 bignum asm: plain integer instructions */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C

#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO  long long
#define MBEDTLS_PLATFORM_TIME_MACRO       w9x_time
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO   w9x_snprintf
#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO  w9x_vsnprintf

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_NO_PLATFORM_ENTROPY

#define MBEDTLS_ERROR_C
#define MBEDTLS_DEPRECATED_REMOVED

/* ---------- crypto primitives ---------- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C               /* bcrypt checksums only; not used by TLS */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT   /* RSA-PSS: required for RSA servers in TLS 1.3 */

/* ---------- encodings, keys, certificates ---------- */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---------- PSA (required for TLS 1.3) ---------- */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_USE_PSA_CRYPTO

/* ---------- TLS ---------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
/* Record buffers: 16 KB in (servers may send full records), 4 KB out. */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* ---------- hooks supplied by kextls ---------- */
#include "w9xrt.h"

#endif /* KEX_MBEDTLS_CONFIG_H */
