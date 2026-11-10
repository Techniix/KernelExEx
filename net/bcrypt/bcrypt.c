/*
 *  KernelEx bcrypt.dll: a CNG (Cryptography API: Next Generation) subset for
 *  Windows 9x, backed by kextls.dll.
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Implemented:
 *    BCryptOpenAlgorithmProvider / BCryptCloseAlgorithmProvider
 *      algorithms: RNG, MD5, SHA1, SHA256, SHA384, SHA512
 *      flags: BCRYPT_ALG_HANDLE_HMAC_FLAG, BCRYPT_HASH_REUSABLE_FLAG
 *    BCryptGenRandom (also with a NULL handle + BCRYPT_USE_SYSTEM_PREFERRED_RNG)
 *    BCryptGetProperty: ObjectLength, HashDigestLength, HashBlockLength,
 *      AlgorithmName
 *    BCryptCreateHash, BCryptHashData, BCryptFinishHash, BCryptDuplicateHash,
 *    BCryptDestroyHash, BCryptHash
 *  Anything else returns STATUS_NOT_SUPPORTED, so callers fall back cleanly.
 *
 *  Caller-supplied hash object buffers (pbHashObject) are accepted and
 *  ignored: state lives in kextls. ObjectLength reports a small size so
 *  callers that allocate it still work.
 */
#include <windows.h>
#include <string.h>
#include "kextls.h"

typedef LONG NTSTATUS_;
#define ST_SUCCESS            ((NTSTATUS_)0x00000000L)
#define ST_UNSUCCESSFUL       ((NTSTATUS_)0xC0000001L)
#define ST_INVALID_HANDLE     ((NTSTATUS_)0xC0000008L)
#define ST_INVALID_PARAMETER  ((NTSTATUS_)0xC000000DL)
#define ST_NO_MEMORY          ((NTSTATUS_)0xC0000017L)
#define ST_BUFFER_TOO_SMALL   ((NTSTATUS_)0xC0000023L)
#define ST_NOT_SUPPORTED      ((NTSTATUS_)0xC00000BBL)
#define ST_INVALID_BUFFER_SIZE ((NTSTATUS_)0xC0000206L)
#define ST_NOT_FOUND          ((NTSTATUS_)0xC0000225L)

#define BCRYPT_ALG_HANDLE_HMAC_FLAG       0x00000008
#define BCRYPT_HASH_REUSABLE_FLAG         0x00000020
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG   0x00000002
#define BCRYPT_RNG_USE_ENTROPY_IN_BUFFER  0x00000001

#define ALG_MAGIC  0x4B584147u /* 'KXAG' */
#define HASH_MAGIC 0x4B584853u /* 'KXHS' */

typedef struct
{
	unsigned int magic;
	int md;        /* KEXTLS_MD_*, 0 for RNG */
	ULONG flags;
	const WCHAR *name;
} alg_t;

typedef struct
{
	unsigned int magic;
	alg_t *alg;
	kextls_digest *d;
	int finished;
} hash_t;

static const WCHAR W_RNG[] = { 'R', 'N', 'G', 0 };
static const WCHAR W_MD5[] = { 'M', 'D', '5', 0 };
static const WCHAR W_SHA1[] = { 'S', 'H', 'A', '1', 0 };
static const WCHAR W_SHA256[] = { 'S', 'H', 'A', '2', '5', '6', 0 };
static const WCHAR W_SHA384[] = { 'S', 'H', 'A', '3', '8', '4', 0 };
static const WCHAR W_SHA512[] = { 'S', 'H', 'A', '5', '1', '2', 0 };
static const WCHAR W_OBJLEN[] = { 'O', 'b', 'j', 'e', 'c', 't', 'L', 'e', 'n', 'g', 't', 'h', 0 };
static const WCHAR W_DIGLEN[] = { 'H', 'a', 's', 'h', 'D', 'i', 'g', 'e', 's', 't', 'L', 'e', 'n', 'g', 't', 'h', 0 };
static const WCHAR W_BLKLEN[] = { 'H', 'a', 's', 'h', 'B', 'l', 'o', 'c', 'k', 'L', 'e', 'n', 'g', 't', 'h', 0 };
static const WCHAR W_ALGNAME[] = { 'A', 'l', 'g', 'o', 'r', 'i', 't', 'h', 'm', 'N', 'a', 'm', 'e', 0 };

static int weq(const WCHAR *a, const WCHAR *b)
{
	if (!a || !b)
		return 0;
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static size_t wlen(const WCHAR *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static void *zalloc(size_t n)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
}

static void zfree(void *p)
{
	if (p)
		HeapFree(GetProcessHeap(), 0, p);
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptOpenAlgorithmProvider(void **phAlg, const WCHAR *alg_id,
                                                                    const WCHAR *impl, ULONG flags)
{
	alg_t *a;
	int md;
	const WCHAR *name;

	(void)impl; /* only the built-in provider exists */
	if (!phAlg || !alg_id)
		return ST_INVALID_PARAMETER;
	*phAlg = NULL;
	if (flags & ~(BCRYPT_ALG_HANDLE_HMAC_FLAG | BCRYPT_HASH_REUSABLE_FLAG))
		return ST_NOT_SUPPORTED;
	if (weq(alg_id, W_RNG)) { md = 0; name = W_RNG; }
	else if (weq(alg_id, W_MD5)) { md = KEXTLS_MD_MD5; name = W_MD5; }
	else if (weq(alg_id, W_SHA1)) { md = KEXTLS_MD_SHA1; name = W_SHA1; }
	else if (weq(alg_id, W_SHA256)) { md = KEXTLS_MD_SHA256; name = W_SHA256; }
	else if (weq(alg_id, W_SHA384)) { md = KEXTLS_MD_SHA384; name = W_SHA384; }
	else if (weq(alg_id, W_SHA512)) { md = KEXTLS_MD_SHA512; name = W_SHA512; }
	else
		return ST_NOT_FOUND;
	if (md == 0 && flags)
		return ST_INVALID_PARAMETER;
	a = (alg_t *)zalloc(sizeof(*a));
	if (!a)
		return ST_NO_MEMORY;
	a->magic = ALG_MAGIC;
	a->md = md;
	a->flags = flags;
	a->name = name;
	*phAlg = a;
	return ST_SUCCESS;
}

static alg_t *as_alg(void *h)
{
	alg_t *a = (alg_t *)h;
	if (!a || IsBadReadPtr(a, sizeof(*a)) || a->magic != ALG_MAGIC)
		return NULL;
	return a;
}

static hash_t *as_hash(void *h)
{
	hash_t *x = (hash_t *)h;
	if (!x || IsBadReadPtr(x, sizeof(*x)) || x->magic != HASH_MAGIC)
		return NULL;
	return x;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptCloseAlgorithmProvider(void *hAlg, ULONG flags)
{
	alg_t *a = as_alg(hAlg);
	(void)flags;
	if (!a)
		return ST_INVALID_HANDLE;
	a->magic = 0;
	zfree(a);
	return ST_SUCCESS;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptGenRandom(void *hAlg, UCHAR *buf, ULONG len, ULONG flags)
{
	if (hAlg)
	{
		alg_t *a = as_alg(hAlg);
		if (!a || a->md != 0)
			return ST_INVALID_HANDLE;
	}
	else if (!(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
		return ST_INVALID_HANDLE;
	if (!buf && len)
		return ST_INVALID_PARAMETER;
	/* BCRYPT_RNG_USE_ENTROPY_IN_BUFFER is ignored, as on Windows 8+. */
	return kextls_random(buf, len) == KEXTLS_OK ? ST_SUCCESS : ST_UNSUCCESSFUL;
}

static NTSTATUS_ put_ulong(UCHAR *out, ULONG cb, ULONG *result, ULONG value)
{
	if (result)
		*result = sizeof(ULONG);
	if (!out)
		return ST_SUCCESS;
	if (cb < sizeof(ULONG))
		return ST_BUFFER_TOO_SMALL;
	memcpy(out, &value, sizeof(value));
	return ST_SUCCESS;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptGetProperty(void *h, const WCHAR *prop, UCHAR *out, ULONG cb,
                                                          ULONG *result, ULONG flags)
{
	alg_t *a = as_alg(h);
	hash_t *x = a ? NULL : as_hash(h);
	(void)flags;
	if (x)
		a = x->alg;
	if (!a)
		return ST_INVALID_HANDLE;
	if (!prop)
		return ST_INVALID_PARAMETER;
	if (weq(prop, W_ALGNAME))
	{
		ULONG need = (ULONG)((wlen(a->name) + 1) * sizeof(WCHAR));
		if (result)
			*result = need;
		if (!out)
			return ST_SUCCESS;
		if (cb < need)
			return ST_BUFFER_TOO_SMALL;
		memcpy(out, a->name, need);
		return ST_SUCCESS;
	}
	if (a->md == 0)
		return ST_NOT_SUPPORTED;
	if (weq(prop, W_OBJLEN))
		return put_ulong(out, cb, result, 16);
	if (weq(prop, W_DIGLEN))
		return put_ulong(out, cb, result, (ULONG)kextls_digest_size(a->md));
	if (weq(prop, W_BLKLEN))
		return put_ulong(out, cb, result, (ULONG)kextls_digest_block(a->md));
	return ST_NOT_SUPPORTED;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptCreateHash(void *hAlg, void **phHash, UCHAR *obj, ULONG cbobj,
                                                         UCHAR *secret, ULONG cbsecret, ULONG flags)
{
	alg_t *a = as_alg(hAlg);
	hash_t *x;
	int hmac;

	(void)obj;
	(void)cbobj;
	if (!a || a->md == 0)
		return ST_INVALID_HANDLE;
	if (!phHash || (flags & ~BCRYPT_HASH_REUSABLE_FLAG))
		return ST_INVALID_PARAMETER;
	hmac = (a->flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;
	if (!hmac && (secret || cbsecret))
		return ST_INVALID_PARAMETER;
	x = (hash_t *)zalloc(sizeof(*x));
	if (!x)
		return ST_NO_MEMORY;
	x->d = kextls_digest_new(a->md, hmac ? (secret ? (const void *)secret : (const void *)"") : NULL,
	                         hmac ? cbsecret : 0);
	if (!x->d)
	{
		zfree(x);
		return ST_NO_MEMORY;
	}
	x->magic = HASH_MAGIC;
	x->alg = a;
	*phHash = x;
	return ST_SUCCESS;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptHashData(void *hHash, UCHAR *data, ULONG len, ULONG flags)
{
	hash_t *x = as_hash(hHash);
	(void)flags;
	if (!x || x->finished)
		return ST_INVALID_HANDLE;
	return kextls_digest_update(x->d, data, len) == KEXTLS_OK ? ST_SUCCESS : ST_INVALID_PARAMETER;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptFinishHash(void *hHash, UCHAR *out, ULONG len, ULONG flags)
{
	hash_t *x = as_hash(hHash);
	(void)flags;
	if (!x || x->finished)
		return ST_INVALID_HANDLE;
	if (!out || len != kextls_digest_size(x->alg->md))
		return ST_INVALID_PARAMETER;
	if (kextls_digest_final(x->d, out, len) != KEXTLS_OK)
		return ST_UNSUCCESSFUL;
	/* Without the reusable flag the hash may not be used again (CNG rule). */
	if (!(x->alg->flags & BCRYPT_HASH_REUSABLE_FLAG))
		x->finished = 1;
	return ST_SUCCESS;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptDuplicateHash(void *hHash, void **phNew, UCHAR *obj, ULONG cbobj,
                                                            ULONG flags)
{
	hash_t *x = as_hash(hHash), *y;
	(void)obj;
	(void)cbobj;
	(void)flags;
	if (!x || x->finished)
		return ST_INVALID_HANDLE;
	if (!phNew)
		return ST_INVALID_PARAMETER;
	y = (hash_t *)zalloc(sizeof(*y));
	if (!y)
		return ST_NO_MEMORY;
	y->d = kextls_digest_dup(x->d);
	if (!y->d)
	{
		zfree(y);
		return ST_NO_MEMORY;
	}
	y->magic = HASH_MAGIC;
	y->alg = x->alg;
	*phNew = y;
	return ST_SUCCESS;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptDestroyHash(void *hHash)
{
	hash_t *x = as_hash(hHash);
	if (!x)
		return ST_INVALID_HANDLE;
	kextls_digest_free(x->d);
	x->magic = 0;
	zfree(x);
	return ST_SUCCESS;
}

__declspec(dllexport) NTSTATUS_ WINAPI BCryptHash(void *hAlg, UCHAR *secret, ULONG cbsecret, UCHAR *in, ULONG cbin,
                                                   UCHAR *out, ULONG cbout)
{
	void *h = NULL;
	NTSTATUS_ s = BCryptCreateHash(hAlg, &h, NULL, 0, secret, cbsecret, 0);
	if (s != ST_SUCCESS)
		return s;
	s = BCryptHashData(h, in, cbin, 0);
	if (s == ST_SUCCESS)
		s = BCryptFinishHash(h, out, cbout, 0);
	BCryptDestroyHash(h);
	return s;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(inst);
	return TRUE;
}
