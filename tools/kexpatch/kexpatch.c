/*
 *  KernelExEx - kexpatch.exe: applies and removes KernelExEx's patches to the
 *  user's own Windows files. Runs on Windows 98/ME (the installer calls it).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  kexpatch /mem on      patch VMM32.VXD for more than 1 GB of RAM (mempatch.c)
 *  kexpatch /mem off     put the original VMM32.VXD back
 *  kexpatch /mem status  exit code 0 = not patched, 1 = patched, 2 = unknown
 *
 *  Safety: the original is kept once as SYSTEM\VMM32.KEX (never overwritten);
 *  the patched file is written as VMM32.NEW and swapped in by WININIT at the
 *  next start (Windows has VMM32.VXD open). Everything is checked first: a
 *  VMM32.VXD that does not match every patch exactly once is left alone.
 *  From the install stick, if Windows no longer starts: KEXUNDO (copies
 *  VMM32.KEX back).
 *  Exit codes: 0 ok, 1 already in that state, 3 not this Windows, 4 file error.
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mempatch.h"

static char sysdir[MAX_PATH];

static void say(const char *s)
{
	DWORD w;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD) strlen(s), &w, NULL);
}

static uint8_t *read_file(const char *path, DWORD *len)
{
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	uint8_t *b = NULL;
	DWORD got = 0;
	if (h == INVALID_HANDLE_VALUE)
		return NULL;
	*len = GetFileSize(h, NULL);
	if (*len != INVALID_FILE_SIZE && *len && (b = malloc(*len)) && (!ReadFile(h, b, *len, &got, NULL) || got != *len))
	{
		free(b);
		b = NULL;
	}
	CloseHandle(h);
	return b;
}

static int write_file(const char *path, const uint8_t *b, DWORD len)
{
	HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD w = 0;
	int ok;
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	ok = WriteFile(h, b, len, &w, NULL) && w == len;
	ok = CloseHandle(h) && ok;
	if (!ok)
		DeleteFileA(path);
	return ok;
}

static void log_patch(const char *what, int r)
{
	static const char *const res[] = { "patched", "already patched", "not found", "found twice", "no VMM" };
	char line[160];
	sprintf(line, "  %s: %s\r\n", what, r >= 0 && r <= 4 ? res[r] : "?");
	say(line);
}

/* swap `src` in for `dst` at the next start (WININIT.INI [rename]) */
static int at_restart(const char *dst, const char *src)
{
	char wininit[MAX_PATH];
	GetWindowsDirectoryA(wininit, sizeof(wininit));
	strcat(wininit, "\\WININIT.INI");
	return WritePrivateProfileStringA("rename", dst, src, wininit) != 0;
}

static int mem(const char *how)
{
	char vxd[MAX_PATH], bak[MAX_PATH], newf[MAX_PATH];
	DWORD len;
	uint8_t *b;
	struct vxdfile f;
	int st, rc;
	sprintf(vxd, "%s\\VMM32.VXD", sysdir);
	sprintf(bak, "%s\\VMM32.KEX", sysdir);
	sprintf(newf, "%s\\VMM32.NEW", sysdir);
	if (!strcmp(how, "off"))
	{
		if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES)
		{
			say("No saved original VMM32.VXD (VMM32.KEX): nothing to undo.\r\n");
			return 1;
		}
		if (!CopyFileA(bak, newf, FALSE) || !at_restart(vxd, newf))
		{
			say("Could not prepare the original VMM32.VXD.\r\n");
			return 4;
		}
		say("The original VMM32.VXD comes back at the next start.\r\n");
		return 0;
	}
	if (!(b = read_file(vxd, &len)))
	{
		say("Cannot read VMM32.VXD.\r\n");
		return 4;
	}
	rc = vf_load(&f, b, len);
	if (rc)
	{
		free(b);
		say("VMM32.VXD is not in a known format.\r\n");
		return 3;
	}
	st = mem_state(&f);
	if (!strcmp(how, "status"))
	{
		say(st == 1 ? "patched\r\n" : st == 0 ? "not patched\r\n" : "unknown VMM32.VXD\r\n");
		rc = st == 1 ? 1 : st == 0 ? 0 : 2;
	}
	else if (st == 1)
	{
		say("VMM32.VXD is patched already.\r\n");
		rc = 1;
	}
	else if (st < 0)
	{
		say("This VMM32.VXD does not match the patch (another Windows version, or patched by another "
		    "program). Nothing was changed.\r\n");
		rc = 3;
	}
	else if (mem_apply(&f, log_patch))
		rc = 3;
	else
	{
		/* keep the original once; never overwrite an existing backup */
		if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES && !write_file(bak, b, len))
		{
			say("Could not save the original as VMM32.KEX. Nothing was changed.\r\n");
			rc = 4;
		}
		else if (!write_file(newf, f.data, (DWORD) f.size) || !at_restart(vxd, newf))
		{
			say("Could not write the patched VMM32.VXD. Nothing was changed.\r\n");
			rc = 4;
		}
		else
		{
			say("Patched: it takes effect at the next start (original saved as VMM32.KEX).\r\n");
			rc = 0;
		}
	}
	vf_free(&f);
	free(b);
	return rc;
}

static int run(int argc, char **argv)
{
	GetSystemDirectoryA(sysdir, sizeof(sysdir));
	if (argc == 3 && !lstrcmpiA(argv[1], "/mem"))
		return mem(argv[2][0] == 'o' && argv[2][1] == 'n' ? "on" : !lstrcmpiA(argv[2], "off") ? "off" : "status");
	say("usage: kexpatch /mem on|off|status\r\n");
	return 2;
}

void __stdcall entrypoint(void)
{
	static char cmd[512];
	char *argv[8], *p = cmd;
	int argc = 0;
	lstrcpynA(cmd, GetCommandLineA(), sizeof(cmd));
	while (*p && argc < 8)
	{
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (*p == '"')
		{
			argv[argc++] = ++p;
			while (*p && *p != '"')
				p++;
		}
		else
		{
			argv[argc++] = p;
			while (*p && *p != ' ')
				p++;
		}
		if (*p)
			*p++ = 0;
	}
	ExitProcess((UINT) run(argc, argv));
}
