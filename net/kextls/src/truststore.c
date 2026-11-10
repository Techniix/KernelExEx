/*
 *  KernelEx kextls: the trust store (root certificates).
 *  Copyright (C) 2026, KernelEx contributors
 *  Licensed under the GNU General Public License version 2.
 *
 *  Loaded once per process, on the first TLS connection:
 *    <dir of kextls.dll>\certs\roots.pem     the bundled Mozilla roots
 *    <dir of kextls.dll>\certs\local\*.pem   machine-local additions
 *  On the test host: the file named by KEXTLS_CA_FILE.
 *
 *  Windows 9x has no ACLs, so any program can change these files. That is
 *  a documented platform limit; a signed-bundle check is planned.
 */
#include <stdlib.h>
#include <string.h>
#include "mbedtls/x509_crt.h"
#include "kextls.h"
#include "kextls_internal.h"

#ifndef KEXTLS_TRUST_EPOCH
#error "build must define KEXTLS_TRUST_EPOCH (Unix time the root bundle was built)"
#endif

static kextls_lock store_lock;
static mbedtls_x509_crt roots;
static int roots_state;  /* 0 = not loaded, >0 = count, <0 = error */

long long kextls_trust_build_time(void)
{
	return (long long)KEXTLS_TRUST_EPOCH;
}

static int add_pem(const char *data, size_t len)
{
	/* mbedtls_x509_crt_parse needs the terminating NUL counted for PEM. */
	char *copy = (char *)malloc(len + 1);
	int ret;

	if (!copy)
		return KEXTLS_E_NOMEM;
	memcpy(copy, data, len);
	copy[len] = '\0';
	/* returns the number of certificates that failed to parse, or <0 */
	ret = mbedtls_x509_crt_parse(&roots, (const unsigned char *)copy, len + 1);
	free(copy);
	return ret < 0 ? KEXTLS_E_NO_ROOTS : KEXTLS_OK;
}

#ifdef _WIN32
#include <windows.h>

static char module_dir[MAX_PATH];

const char *kextls_module_dir(void)
{
	if (!module_dir[0])
	{
		HMODULE self = NULL;
		MEMORY_BASIC_INFORMATION mbi;
		char *slash;

		/* GetModuleHandleEx is not on 9x; find our base from an address. */
		if (VirtualQuery((void *)kextls_module_dir, &mbi, sizeof(mbi)))
			self = (HMODULE)mbi.AllocationBase;
		if (!GetModuleFileNameA(self, module_dir, sizeof(module_dir)))
			module_dir[0] = '\0';
		slash = strrchr(module_dir, '\\');
		if (slash)
			slash[1] = '\0';
	}
	return module_dir;
}

static int load_file(const char *path)
{
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	DWORD size, got = 0;
	char *buf;
	int ret;

	if (f == INVALID_HANDLE_VALUE)
		return KEXTLS_E_NO_ROOTS;
	size = GetFileSize(f, NULL);
	if (size == 0xFFFFFFFF || size > 8 * 1024 * 1024)
	{
		CloseHandle(f);
		return KEXTLS_E_NO_ROOTS;
	}
	buf = (char *)malloc(size ? size : 1);
	if (!buf)
	{
		CloseHandle(f);
		return KEXTLS_E_NOMEM;
	}
	if (!ReadFile(f, buf, size, &got, NULL) || got != size)
	{
		free(buf);
		CloseHandle(f);
		return KEXTLS_E_NO_ROOTS;
	}
	CloseHandle(f);
	ret = add_pem(buf, size);
	free(buf);
	return ret;
}

static void load_default(void)
{
	char path[MAX_PATH + 32];
	WIN32_FIND_DATAA fd;
	HANDLE find;
	const char *dir = kextls_module_dir();

	if (strlen(dir) + 32 > sizeof(path))
		return;
	strcpy(path, dir);
	strcat(path, "certs\\roots.pem");
	load_file(path);

	strcpy(path, dir);
	strcat(path, "certs\\local\\*.pem");
	find = FindFirstFileA(path, &fd);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			    strlen(dir) + strlen(fd.cFileName) + 16 < sizeof(path))
			{
				strcpy(path, dir);
				strcat(path, "certs\\local\\");
				strcat(path, fd.cFileName);
				load_file(path);
			}
		} while (FindNextFileA(find, &fd));
		FindClose(find);
	}
}

#else /* host */
#include <stdio.h>

const char *kextls_module_dir(void)
{
	return "./";
}

static void load_default(void)
{
	const char *path = getenv("KEXTLS_CA_FILE");
	FILE *f;
	long size;
	char *buf;

	if (!path || !(f = fopen(path, "rb")))
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char *)malloc((size_t)size + 1);
	if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size)
		add_pem(buf, (size_t)size);
	free(buf);
	fclose(f);
}
#endif

static int count_roots(void)
{
	int n = 0;
	mbedtls_x509_crt *c;

	for (c = &roots; c && c->raw.len; c = c->next)
		n++;
	return n;
}

mbedtls_x509_crt *kextls_trust_roots(int *count_or_error)
{
	kextls_lock_enter(&store_lock);
	if (roots_state == 0)
	{
		mbedtls_x509_crt_init(&roots);
		load_default();
		roots_state = count_roots();
		if (roots_state == 0)
			roots_state = KEXTLS_E_NO_ROOTS;
	}
	if (count_or_error)
		*count_or_error = roots_state;
	kextls_lock_leave(&store_lock);
	return roots_state > 0 ? &roots : NULL;
}

int kextls_trust_store_count(void)
{
	int n;
	kextls_trust_roots(&n);
	return n;
}

int kextls_trust_store_load_pem(const char *pem, size_t len)
{
	int ret;

	if (!pem)
		return KEXTLS_E_PARAM;
	kextls_lock_enter(&store_lock);
	if (roots_state != 0)
		mbedtls_x509_crt_free(&roots);
	mbedtls_x509_crt_init(&roots);
	ret = add_pem(pem, len);
	roots_state = count_roots();
	if (roots_state == 0)
		roots_state = KEXTLS_E_NO_ROOTS;
	kextls_lock_leave(&store_lock);
	return ret == KEXTLS_OK ? roots_state : ret;
}
