/*
 *  KernelExEx - kexmedia.exe: the install USB stick maker. One window: pick the
 *  Windows 98 CD (a drive, or an .iso file), pick the USB stick, click Make. It
 *  builds the stick from the CD (mkimage.c) and writes and checks it. Everything
 *  else it needs is appended to the program (media/pack_media.py), so it is one
 *  file. For Windows XP to 11 (Windows 98 and ME cannot write whole disks).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Safety: only USB or removable disks are offered, never the disk Windows runs
 *  from; nothing is written before the user confirms; the stick's volumes are
 *  locked and dismounted first.
 */
#include <windows.h>
#include <winioctl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mkimage.h"

#define MAXDISK 32
#define MAXSRC 32
#define WM_PROGRESS (WM_APP + 1)
#define WM_FINISHED (WM_APP + 2)
#define IOBUF (1024 * 1024)

enum { ID_SRC = 100, ID_BROWSE, ID_DST, ID_REFRESH, ID_MAKE, ID_SAVE, ID_PROG, ID_STATUS };

struct disk
{
	int num;
	LONGLONG size;
	char name[96];
};

static HINSTANCE inst;
static HWND wnd, h_src, h_dst, h_make, h_save, h_browse, h_refresh, h_prog, h_status;
static HFONT font;
static struct disk disks[MAXDISK];
static int ndisks;
static char sources[MAXSRC][MAX_PATH];   /* "D:" or an .iso path */
static int nsources;
static struct mk_file files[64];
static int nfiles;
static BYTE *payload;
static volatile int busy;

/* the job, run on a worker thread */
static struct
{
	struct mk_job job;
	char src[MAX_PATH];
	char dst[MAX_PATH];   /* \\.\PhysicalDriveN or a file */
	int disk;             /* >= 0: a disk number */
	HANDLE hsrc, hdst;
	BYTE *srcbuf, *dstbuf;
	HANDLE locks[26];
	int nlocks;
} W;

/* ------------------------------------------------------------------ disks */

static int volume_disk(const char *path)
{
	HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	VOLUME_DISK_EXTENTS ext;
	DWORD got;
	int d = -1;
	if (h == INVALID_HANDLE_VALUE)
		return -1;
	if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof(ext), &got, NULL) &&
	    ext.NumberOfDiskExtents == 1)
		d = (int) ext.Extents[0].DiskNumber;
	CloseHandle(h);
	return d;
}

static int system_disk(void)
{
	char win[MAX_PATH], path[16];
	if (!GetWindowsDirectoryA(win, sizeof(win)))
		return -1;
	sprintf(path, "\\\\.\\%c:", win[0]);
	return volume_disk(path);
}

static int probe(int n, struct disk *d)
{
	char path[32];
	HANDLE h;
	STORAGE_PROPERTY_QUERY q;
	BYTE buf[1024];
	STORAGE_DEVICE_DESCRIPTOR *dd = (STORAGE_DEVICE_DESCRIPTOR *) buf;
	GET_LENGTH_INFORMATION len;
	DWORD got;
	int ok = 0;
	sprintf(path, "\\\\.\\PhysicalDrive%d", n);
	h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	memset(&q, 0, sizeof(q));
	q.PropertyId = StorageDeviceProperty;
	q.QueryType = PropertyStandardQuery;
	memset(buf, 0, sizeof(buf));
	if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, NULL) &&
	    (dd->BusType == BusTypeUsb || dd->RemovableMedia) &&
	    DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), &got, NULL) &&
	    len.Length.QuadPart > 0)
	{
		const char *v = dd->VendorIdOffset ? (const char *) buf + dd->VendorIdOffset : "";
		const char *p = dd->ProductIdOffset ? (const char *) buf + dd->ProductIdOffset : "";
		char letters[40] = "";
		int c;
		d->num = n;
		d->size = len.Length.QuadPart;
		for (c = 'A'; c <= 'Z'; c++)
		{
			char vp[16];
			sprintf(vp, "\\\\.\\%c:", c);
			if (GetLogicalDrives() & (1u << (c - 'A')) && volume_disk(vp) == n && strlen(letters) < 30)
				sprintf(letters + strlen(letters), "%s%c:", letters[0] ? " " : "", c);
		}
		snprintf(d->name, sizeof(d->name) - 1, "%s%s%s %s", letters, letters[0] ? "  " : "", v, p);
		ok = 1;
	}
	CloseHandle(h);
	return ok;
}

/* lock and dismount every volume of disk n (kept locked until the job ends) */
static int release_volumes(int n, char *err, int errlen)
{
	int c;
	for (c = 'A'; c <= 'Z'; c++)
	{
		char path[16];
		HANDLE h;
		DWORD got;
		if (!(GetLogicalDrives() & (1u << (c - 'A'))))
			continue;
		sprintf(path, "\\\\.\\%c:", c);
		if (volume_disk(path) != n)
			continue;
		h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
		                NULL);
		if (h == INVALID_HANDLE_VALUE || !DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL))
		{
			snprintf(err, (size_t) errlen, "Drive %c: on the stick is in use. Close every window and program "
			         "using it, then try again.", c);
			if (h != INVALID_HANDLE_VALUE)
				CloseHandle(h);
			return 0;
		}
		DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
		W.locks[W.nlocks++] = h;
	}
	return 1;
}

/* ------------------------------------------------------------------ the job's I/O */

static int seek(HANDLE h, ULONGLONG off)
{
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG) off;
	return SetFilePointerEx(h, li, NULL, FILE_BEGIN) ? 0 : -1;
}

static int src_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	BYTE *p = buf;
	(void) ctx;
	while (count)
	{
		DWORD n = count > 32 ? 32 : count, got = 0;
		if (seek(W.hsrc, (ULONGLONG) lba * 2048) || !ReadFile(W.hsrc, W.srcbuf, n * 2048, &got, NULL) ||
		    got != n * 2048)
			return -1;
		memcpy(p, W.srcbuf, n * 2048);
		p += n * 2048;
		lba += n;
		count -= n;
	}
	return 0;
}

static int dst_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
	const BYTE *p = buf;
	(void) ctx;
	while (len)
	{
		DWORD n = len > IOBUF ? IOBUF : len, got = 0;
		memcpy(W.dstbuf, p, n);   /* unbuffered disk I/O wants an aligned buffer */
		if (seek(W.hdst, off) || !WriteFile(W.hdst, W.dstbuf, n, &got, NULL) || got != n)
			return -1;
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int dst_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	BYTE *p = buf;
	(void) ctx;
	while (len)
	{
		DWORD n = len > IOBUF ? IOBUF : len, got = 0;
		if (seek(W.hdst, off) || !ReadFile(W.hdst, W.dstbuf, n, &got, NULL) || got != n)
			return -1;
		memcpy(p, W.dstbuf, n);
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static void on_progress(void *ctx, int pct, const char *what)
{
	static int last = -1;
	static const char *lastw;
	(void) ctx;
	if (pct != last || what != lastw)
		PostMessageA(wnd, WM_PROGRESS, (WPARAM) pct, (LPARAM) what);
	last = pct;
	lastw = what;
}

static int open_source(void)
{
	char path[MAX_PATH + 8];
	if (W.src[1] == ':' && !W.src[2])
		sprintf(path, "\\\\.\\%c:", W.src[0]);
	else
		lstrcpynA(path, W.src, sizeof(path));
	W.hsrc = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (W.hsrc == INVALID_HANDLE_VALUE)
	{
		snprintf(W.job.error, sizeof(W.job.error), W.src[1] == ':' && !W.src[2] ?
		         "Cannot read the CD in drive %s. Is the Windows 98 CD in it?" : "Cannot open %s.", W.src);
		return -1;
	}
	return 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
	int rc = -1, i;
	DWORD got;
	(void) arg;
	W.srcbuf = VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT, PAGE_READWRITE);
	W.dstbuf = VirtualAlloc(NULL, IOBUF, MEM_COMMIT, PAGE_READWRITE);
	W.hsrc = W.hdst = INVALID_HANDLE_VALUE;
	W.nlocks = 0;
	if (!W.srcbuf || !W.dstbuf)
	{
		strcpy(W.job.error, "Out of memory.");
		goto out;
	}
	if (open_source() || mk_check_source(&W.job))
		goto out;
	if (W.disk >= 0)
	{
		if (!release_volumes(W.disk, W.job.error, sizeof(W.job.error)))
			goto out;
		W.hdst = CreateFileA(W.dst, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		                     OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
	}
	else
		W.hdst = CreateFileA(W.dst, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (W.hdst == INVALID_HANDLE_VALUE)
	{
		snprintf(W.job.error, sizeof(W.job.error), W.disk >= 0 ? "Cannot open the USB stick for writing." :
		         "Cannot create %s.", W.dst);
		goto out;
	}
	if (mk_build(&W.job))
		goto out;
	if (W.disk >= 0)
	{
		FlushFileBuffers(W.hdst);
		DeviceIoControl(W.hdst, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL);
	}
	else if (seek(W.hdst, W.job.out_size) || !SetEndOfFile(W.hdst))
	{
		strcpy(W.job.error, "Writing the file failed (is the disk full?).");
		goto out;
	}
	rc = 0;
out:
	if (W.hsrc != INVALID_HANDLE_VALUE)
		CloseHandle(W.hsrc);
	if (W.hdst != INVALID_HANDLE_VALUE)
		CloseHandle(W.hdst);
	for (i = 0; i < W.nlocks; i++)
		CloseHandle(W.locks[i]);
	if (W.srcbuf)
		VirtualFree(W.srcbuf, 0, MEM_RELEASE);
	if (W.dstbuf)
		VirtualFree(W.dstbuf, 0, MEM_RELEASE);
	PostMessageA(wnd, WM_FINISHED, (WPARAM) rc, 0);
	return 0;
}

/* ------------------------------------------------------------------ the window */

static char last_iso[MAX_PATH];   /* kept across refreshes (a stick plugged in, Refresh) */

static void fill_sources(const char *select)
{
	int c, i, sel = -1, cur = (int) SendMessageA(h_src, CB_GETCURSEL, 0, 0);
	char keep[MAX_PATH] = "";
	DWORD mask = GetLogicalDrives();
	if (select)
		lstrcpynA(last_iso, select, sizeof(last_iso));
	else if (cur >= 0 && cur < nsources)
		lstrcpynA(keep, sources[cur], sizeof(keep));   /* a refresh keeps the choice */
	SendMessageA(h_src, CB_RESETCONTENT, 0, 0);
	nsources = 0;
	for (c = 'A'; c <= 'Z' && nsources < MAXSRC - 1; c++)
	{
		char root[4] = { (char) c, ':', '\\', 0 }, label[64] = "", item[128];
		if (!(mask & (1u << (c - 'A'))) || GetDriveTypeA(root) != DRIVE_CDROM)
			continue;
		if (!GetVolumeInformationA(root, label, sizeof(label), NULL, NULL, NULL, NULL, 0))
			strcpy(label, "no disc");
		snprintf(item, sizeof(item), "CD drive %c:  (%s)", c, label);
		sprintf(sources[nsources], "%c:", c);
		SendMessageA(h_src, CB_ADDSTRING, 0, (LPARAM) item);
		nsources++;
	}
	if (last_iso[0] && nsources < MAXSRC)
	{
		lstrcpynA(sources[nsources], last_iso, MAX_PATH);
		SendMessageA(h_src, CB_ADDSTRING, 0, (LPARAM) last_iso);
		if (select)
			sel = nsources;
		nsources++;
	}
	for (i = 0; i < nsources && sel < 0 && keep[0]; i++)
		if (!lstrcmpiA(sources[i], keep))
			sel = i;
	for (i = 0; i < nsources && sel < 0; i++)
	{
		char root[4] = { sources[i][0], ':', '\\', 0 }, label[64];
		if (GetVolumeInformationA(root, label, sizeof(label), NULL, NULL, NULL, NULL, 0))
			sel = i;   /* the first drive with a disc in it */
	}
	SendMessageA(h_src, CB_SETCURSEL, (WPARAM) (sel >= 0 ? sel : 0), 0);
}

static void fill_disks(void)
{
	int i, sys = system_disk(), cur = (int) SendMessageA(h_dst, CB_GETCURSEL, 0, 0), keep = -1, sel = 0;
	char item[160];
	if (cur >= 0 && cur < ndisks)
		keep = disks[cur].num;   /* a refresh keeps the choice */
	SendMessageA(h_dst, CB_RESETCONTENT, 0, 0);
	ndisks = 0;
	for (i = 0; i < MAXDISK && ndisks < MAXDISK; i++)
		if (i != sys && probe(i, &disks[ndisks]))
		{
			snprintf(item, sizeof(item), "%s  (%lu MB)", disks[ndisks].name, (unsigned long) (disks[ndisks].size >> 20));
			SendMessageA(h_dst, CB_ADDSTRING, 0, (LPARAM) item);
			if (disks[ndisks].num == keep)
				sel = ndisks;
			ndisks++;
		}
	if (!ndisks)
		SendMessageA(h_dst, CB_ADDSTRING, 0, (LPARAM) "(no USB stick found: plug one in)");
	SendMessageA(h_dst, CB_SETCURSEL, (WPARAM) sel, 0);
}

static void status(const char *s)
{
	SetWindowTextA(h_status, s);
}

static void enable(int on)
{
	EnableWindow(h_src, on);
	EnableWindow(h_dst, on);
	EnableWindow(h_make, on);
	EnableWindow(h_save, on);
	EnableWindow(h_browse, on);
	EnableWindow(h_refresh, on);
}

static void start(int to_file)
{
	int s = (int) SendMessageA(h_src, CB_GETCURSEL, 0, 0), d = (int) SendMessageA(h_dst, CB_GETCURSEL, 0, 0);
	char msg[400];
	DWORD tid;
	HANDLE th;
	if (s < 0 || s >= nsources)
	{
		MessageBoxA(wnd, "Put the Windows 98 CD in a drive, or choose an .iso file of it with \"Browse...\".",
		            "KernelExEx", MB_ICONINFORMATION);
		return;
	}
	memset(&W, 0, sizeof(W));
	lstrcpynA(W.src, sources[s], sizeof(W.src));
	if (to_file)
	{
		OPENFILENAMEA of;
		memset(&of, 0, sizeof(of));
		strcpy(W.dst, "KEXSETUP.ISO");
		of.lStructSize = sizeof(of);
		of.hwndOwner = wnd;
		of.lpstrFilter = "Disc images (*.iso)\0*.iso\0All files\0*.*\0";
		of.lpstrFile = W.dst;
		of.nMaxFile = sizeof(W.dst);
		of.lpstrDefExt = "iso";
		of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
		if (!GetSaveFileNameA(&of))
			return;
		W.disk = -1;
	}
	else
	{
		if (d < 0 || d >= ndisks)
		{
			MessageBoxA(wnd, "Plug in a USB stick (it will be erased), then click Refresh.", "KernelExEx",
			            MB_ICONINFORMATION);
			return;
		}
		if ((ULONGLONG) disks[d].size < mk_needed(files, nfiles))
		{
			snprintf(msg, sizeof(msg), "That stick is too small: it needs at least %lu MB.",
			         (unsigned long) (mk_needed(files, nfiles) >> 20) + 1);
			MessageBoxA(wnd, msg, "KernelExEx", MB_ICONWARNING);
			return;
		}
		snprintf(msg, sizeof(msg), "EVERYTHING on this USB stick will be erased:\n\n    %s  (%lu MB)\n\n"
		         "Make the Windows 98 install stick on it?", disks[d].name, (unsigned long) (disks[d].size >> 20));
		if (MessageBoxA(wnd, msg, "KernelExEx", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
			return;
		W.disk = disks[d].num;
		sprintf(W.dst, "\\\\.\\PhysicalDrive%d", W.disk);
	}
	W.job.src.read = src_read;
	W.job.out.write = dst_write;
	W.job.out.read = dst_read;
	W.job.files = files;
	W.job.nfiles = nfiles;
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		W.job.dos_date = (WORD) (((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
		W.job.dos_time = (WORD) ((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
	}
	W.job.progress = on_progress;
	busy = 1;
	enable(0);
	SendMessageA(h_prog, PBM_SETPOS, 0, 0);
	status("Reading the CD...");
	th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
	if (th)
		CloseHandle(th);
	else
	{
		busy = 0;
		enable(1);
		status("Could not start.");
	}
}

static void browse(void)
{
	OPENFILENAMEA of;
	char path[MAX_PATH] = "";
	memset(&of, 0, sizeof(of));
	of.lStructSize = sizeof(of);
	of.hwndOwner = wnd;
	of.lpstrFilter = "Disc images (*.iso)\0*.iso\0All files\0*.*\0";
	of.lpstrFile = path;
	of.nMaxFile = sizeof(path);
	of.lpstrTitle = "The Windows 98 CD image";
	of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
	if (GetOpenFileNameA(&of))
		fill_sources(path);
}

static HWND control(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, wnd, (HMENU) (INT_PTR) id, inst,
	                         NULL);
	SendMessageA(c, WM_SETFONT, (WPARAM) font, TRUE);
	return c;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m)
	{
	case WM_CREATE:
		wnd = h;
		control("STATIC", "Make a USB stick that installs Windows 98 with KernelExEx (BIOS and UEFI PCs).",
		        0, 16, 12, 470, 20, 0);
		control("STATIC", "1.  Your Windows 98 CD (or an .iso file of it):", 0, 16, 44, 470, 18, 0);
		h_src = control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 16, 64, 370, 200, ID_SRC);
		h_browse = control("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 394, 63, 92, 25, ID_BROWSE);
		control("STATIC", "2.  The USB stick (everything on it is erased):", 0, 16, 100, 470, 18, 0);
		h_dst = control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 16, 120, 370, 200, ID_DST);
		h_refresh = control("BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP, 394, 119, 92, 25, ID_REFRESH);
		h_make = control("BUTTON", "Make the USB stick", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 160, 180, 30, ID_MAKE);
		h_save = control("BUTTON", "Save as .iso file instead...", BS_PUSHBUTTON | WS_TABSTOP, 206, 160, 180, 30,
		                 ID_SAVE);
		h_prog = control(PROGRESS_CLASSA, "", 0, 16, 204, 470, 18, ID_PROG);
		SendMessageA(h_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
		h_status = control("STATIC", "", SS_LEFT, 16, 230, 470, 40, ID_STATUS);
		fill_sources(NULL);
		fill_disks();
		if (!nfiles)
		{
			status("This copy of kexmedia.exe is incomplete: download it again.");
			enable(0);
		}
		else
			status("Ready. Nothing is written until you confirm.");
		return 0;
	case WM_COMMAND:
		if (busy)
			return 0;
		switch (LOWORD(wp))
		{
		case ID_BROWSE: browse(); break;
		case ID_REFRESH: fill_sources(NULL); fill_disks(); break;
		case ID_MAKE: start(0); break;
		case ID_SAVE: start(1); break;
		}
		return 0;
	case WM_DEVICECHANGE:
		if (!busy && (wp == 0x8000 || wp == 0x8004))   /* DBT_DEVICEARRIVAL, DBT_DEVICEREMOVECOMPLETE */
		{
			fill_sources(NULL);
			fill_disks();
		}
		return TRUE;
	case WM_PROGRESS:
	{
		char s[96];
		SendMessageA(h_prog, PBM_SETPOS, wp, 0);
		snprintf(s, sizeof(s), "%s...  %d%%", (const char *) lp, (int) wp);
		status(s);
		return 0;
	}
	case WM_FINISHED:
		busy = 0;
		enable(1);
		if (wp == 0)
		{
			SendMessageA(h_prog, PBM_SETPOS, 100, 0);
			if (W.disk >= 0)
			{
				status("Done: the stick is ready.");
				MessageBoxA(h, "The USB stick is ready.\n\nStart the PC from it (BIOS or UEFI boot menu) to install "
				            "Windows 98.\n\nWindows may say the stick needs formatting: do NOT format it, just "
				            "remove it.", "KernelExEx", MB_ICONINFORMATION);
			}
			else
			{
				status("Done: the .iso file is ready (burn it to a CD, or write it to a stick with this program "
				       "or Rufus in DD mode).");
			}
		}
		else
		{
			SendMessageA(h_prog, PBM_SETPOS, 0, 0);
			status(W.job.error[0] ? W.job.error : "Failed.");
			MessageBoxA(h, W.job.error[0] ? W.job.error : "Failed.", "KernelExEx", MB_ICONERROR);
		}
		return 0;
	case WM_CLOSE:
		if (busy && MessageBoxA(h, "The stick is being written. Stop now? (The stick will not work until made "
		                        "again.)", "KernelExEx", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
			return 0;
		DestroyWindow(h);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(h, m, wp, lp);
}

/* the payload appended to this program */
static void load_payload(void)
{
	char self[MAX_PATH];
	HANDLE f;
	LARGE_INTEGER sz;
	DWORD got;
	if (!GetModuleFileNameA(NULL, self, sizeof(self)))
		return;
	f = CreateFileA(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	if (GetFileSizeEx(f, &sz) && sz.QuadPart < 256 * 1024 * 1024 &&
	    (payload = VirtualAlloc(NULL, (SIZE_T) sz.QuadPart, MEM_COMMIT, PAGE_READWRITE)) &&
	    ReadFile(f, payload, (DWORD) sz.QuadPart, &got, NULL) && got == (DWORD) sz.QuadPart)
		nfiles = mk_payload(payload, (size_t) sz.QuadPart, files, 64);
	CloseHandle(f);
}

/* next argument of a command line (quotes allowed); NULL at the end */
static char *next_arg(char **p)
{
	char *a;
	while (**p == ' ')
		(*p)++;
	if (!**p)
		return NULL;
	if (**p == '"')
	{
		a = ++*p;
		while (**p && **p != '"')
			(*p)++;
	}
	else
	{
		a = *p;
		while (**p && **p != ' ')
			(*p)++;
	}
	if (**p)
		*(*p)++ = 0;
	return a;
}

/* kexmedia /makeiso CD-OR-ISO OUT.ISO: no window (tests, scripts); exit code 0 = done */
static int headless(char *cmd)
{
	char *p = cmd, *src, *dst;
	next_arg(&p);                    /* the program */
	if (!(src = next_arg(&p)) || lstrcmpiA(src, "/makeiso") || !(src = next_arg(&p)) || !(dst = next_arg(&p)))
		return -1;
	memset(&W, 0, sizeof(W));
	lstrcpynA(W.src, src, sizeof(W.src));
	lstrcpynA(W.dst, dst, sizeof(W.dst));
	W.disk = -1;
	W.job.src.read = src_read;
	W.job.out.write = dst_write;
	W.job.out.read = dst_read;
	W.job.files = files;
	W.job.nfiles = nfiles;
	W.job.dos_date = (46 << 9) | (11 << 5) | 22;   /* fixed: the output is reproducible */
	W.job.dos_time = 12 << 11;
	if (!nfiles)
		strcpy(W.job.error, "no payload");
	else
	{
		wnd = NULL;
		worker(NULL);
	}
	return 0;
}

void __stdcall entrypoint(void)
{
	static char cmd[2048];
	WNDCLASSA wc;
	MSG msg;
	RECT r = { 0, 0, 502, 282 };
	OSVERSIONINFOA ov;
	INITCOMMONCONTROLSEX icc;
	NONCLIENTMETRICSA ncm;
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

	inst = GetModuleHandleA(NULL);
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);
	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
		font = CreateFontIndirectA(&ncm.lfMessageFont);
	if (!font)
		font = (HFONT) GetStockObject(DEFAULT_GUI_FONT);
	load_payload();
	lstrcpynA(cmd, GetCommandLineA(), sizeof(cmd));
	if (headless(cmd) == 0)
	{
		DWORD w;
		HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
		const char *e = W.job.error[0] ? W.job.error : "done";
		WriteFile(out, e, (DWORD) strlen(e), &w, NULL);
		WriteFile(out, "\r\n", 2, &w, NULL);
		ExitProcess(W.job.error[0] ? 1 : 0);
	}
	/* (/makeiso from an .iso file also runs on Windows 98 with KernelExEx: the tests use that) */
	ov.dwOSVersionInfoSize = sizeof(ov);
	GetVersionExA(&ov);
	if ((ov.dwPlatformId != VER_PLATFORM_WIN32_NT || ov.dwMajorVersion < 5) && !strstr(GetCommandLineA(), "/test98"))
	{
		MessageBoxA(NULL, "This needs Windows XP or later: Windows 98 and ME cannot write a whole USB stick.",
		            "KernelExEx", MB_ICONERROR);
		ExitProcess(1);
	}
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
	if (!wc.hIcon)
		wc.hIcon = LoadIconA(NULL, (LPCSTR) IDI_APPLICATION);
	wc.hCursor = LoadCursorA(NULL, (LPCSTR) IDC_ARROW);
	wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
	wc.lpszClassName = "KexMediaWnd";
	RegisterClassA(&wc);
	AdjustWindowRect(&r, style, FALSE);
	CreateWindowExA(WS_EX_CONTROLPARENT, "KexMediaWnd", "KernelExEx - Windows 98 install stick", style, CW_USEDEFAULT,
	                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
	ShowWindow(wnd, SW_SHOW);
	UpdateWindow(wnd);
	while (GetMessageA(&msg, NULL, 0, 0) > 0)
		if (!IsDialogMessageA(wnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	ExitProcess(0);
}
