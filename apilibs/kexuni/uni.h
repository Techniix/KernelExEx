/*
 *  KernelExEx - kexuni.dll: open replacement for the parts of the Microsoft
 *  Layer for Unicode (unicows.dll) that KernelEx forwards to.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 *
 *  Every function converts its strings to the ANSI code page, calls the
 *  Windows 9x ANSI function and converts the results back. Counts are in
 *  characters on the wide side and in bytes on the ANSI side, which differ
 *  on double-byte (Chinese, Japanese, Korean) systems; the helpers here
 *  keep the two apart.
 *
 *  kexbasen loads kexuni.dll only when unicows.dll is not installed.
 */
#ifndef KEXUNI_H
#define KEXUNI_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define KXU(name) WINAPI kxu_##name

#define IS_INTRES(p) ((((ULONG_PTR) (p)) >> 16) == 0)

void *kxu_alloc(size_t n);
void kxu_free(void *p);

/* wide -> new ANSI string (NULL stays NULL); free with kxu_free */
char *kxu_w2a(LPCWSTR w);
char *kxu_w2a_cp(UINT cp, LPCWSTR w);
/* first cch characters (-1: to the NUL); *alen gets the byte count, no NUL */
char *kxu_w2a_n(UINT cp, LPCWSTR w, int cch, int *alen);
/* resource name or atom: integer IDs pass through */
char *kxu_res(LPCWSTR w);
void kxu_res_free(char *a);
/* double-NUL-terminated lists */
char *kxu_w2a_multi(LPCWSTR w);
size_t kxu_multi_len_w(LPCWSTR w);   /* in WCHARs, including the final NUL */

/* ANSI -> wide into a caller buffer of cchw characters. Returns characters
 * written, not counting the NUL; the result is always terminated when
 * cchw > 0. *need (if given) gets the characters needed, without the NUL. */
int kxu_a2w(UINT cp, LPCSTR a, int alen, LPWSTR w, int cchw, int *need);
/* characters a string of alen bytes (-1: to the NUL) needs, without the NUL */
int kxu_a2w_len(UINT cp, LPCSTR a, int alen);
/* new wide string; free with kxu_free */
LPWSTR kxu_a2w_new(UINT cp, LPCSTR a);
/* number of wide characters in the first nbytes of an ANSI string */
int kxu_wchars_in(UINT cp, LPCSTR a, int nbytes);
/* bytes taken by the first nchars wide characters of an ANSI string */
int kxu_bytes_for(UINT cp, LPCSTR a, int alen, int nchars);

/* typical "copy result to a W buffer of size cch" with the usual Win32
 * return rules: success -> chars copied (no NUL); too small -> chars needed
 * (with the NUL) */
DWORD kxu_ret_buf(LPCSTR a, LPWSTR w, DWORD cch);

/* structures */
void kxu_logfont_w2a(const LOGFONTW *w, LOGFONTA *a);
void kxu_logfont_a2w(const LOGFONTA *a, LOGFONTW *w);
void kxu_textmetric_a2w(const TEXTMETRICA *a, TEXTMETRICW *w);
DEVMODEA *kxu_devmode_w2a(const DEVMODEW *w);   /* NULL stays NULL */
void kxu_devmode_a2w(const DEVMODEA *a, DEVMODEW *w, DWORD wsize);

#endif
