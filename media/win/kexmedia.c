/*
 *  KernelExEx - kexmedia.exe: writes the KernelExEx Windows 98 install image
 *  (KEXSETUP.ISO, made by media/make_iso.sh) to a USB stick, and checks it.
 *  For Windows XP to 11 (the PC the stick is made on; Windows 98 cannot write
 *  whole disks). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  usage: kexmedia [IMAGE]   (default: KEXSETUP.ISO beside kexmedia.exe)
 *
 *  Safety: only USB or removable disks are offered, never the disk Windows
 *  runs from, and nothing is written until ERASE is typed. The stick's
 *  volumes are locked and dismounted first, so Windows holds nothing open.
 */
#include <windows.h>
#include <winioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (1024 * 1024)
#define MAXDISK 32

struct disk
{
	int num;
	LONGLONG size;
	char name[96];
};

/* console I/O straight through Windows (our small C runtime has no stdio streams) */
static void say(const char *s)
{
	DWORD w;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD) strlen(s), &w, NULL);
}

static void pr(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	say(b);
}

static int read_line(char *b, int n)
{
	DWORD got = 0;
	if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), b, (DWORD) n - 1, &got, NULL) || !got)
		return 0;
	b[got] = 0;
	return 1;
}

static void fail(const char *what)
{
	char msg[256];
	DWORD e = GetLastError();
	if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof(msg), NULL))
		sprintf(msg, "error %lu\n", e);
	pr("\n%s: %s", what, msg);
}

/* the physical disk a volume (\\.\C:) lives on; -1 if none or spanning several */
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
		d->num = n;
		d->size = len.Length.QuadPart;
		snprintf(d->name, sizeof(d->name) - 1, "%s %s", v, p);
		ok = 1;
	}
	CloseHandle(h);
	return ok;
}

/* lock and dismount every volume of disk n; the handles stay open until exit */
static int release_volumes(int n)
{
	char path[16];
	int c, ok = 1;
	for (c = 'A'; c <= 'Z'; c++)
	{
		HANDLE h;
		DWORD got;
		sprintf(path, "\\\\.\\%c:", c);
		if (volume_disk(path) != n)
			continue;
		h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
		                NULL);
		if (h == INVALID_HANDLE_VALUE)
		{
			fail("Could not open the stick's drive");
			ok = 0;
			continue;
		}
		if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL))
		{
			pr("\nDrive %c: is in use. Close every window and program using it, then try again.\n", c);
			ok = 0;
			continue;
		}
		DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
		pr("Drive %c: released.\n", c);
	}
	return ok;
}

static int kmain(int argc, char **argv)
{
	char image[MAX_PATH], line[64], path[32];
	struct disk disks[MAXDISK];
	int nd = 0, i, sys, pick;
	HANDLE img, dev;
	LARGE_INTEGER isz, zero;
	BYTE *buf, *vbuf;
	LONGLONG done;
	DWORD got;
	OSVERSIONINFOA ov;

	say("KernelExEx: make a Windows 98 install USB stick\n"
	    "=============================================\n\n");
	ov.dwOSVersionInfoSize = sizeof(ov);
	GetVersionExA(&ov);
	if (ov.dwPlatformId != VER_PLATFORM_WIN32_NT || ov.dwMajorVersion < 5)
	{
		say("This needs Windows XP or later (Windows 98 and ME cannot write a whole USB stick).\n");
		return 1;
	}
	if (argc > 1)
		lstrcpynA(image, argv[1], sizeof(image));
	else
	{
		char *slash;
		GetModuleFileNameA(NULL, image, sizeof(image));
		slash = strrchr(image, '\\');
		strcpy(slash ? slash + 1 : image, "KEXSETUP.ISO");
	}
	img = CreateFileA(image, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (img == INVALID_HANDLE_VALUE || !GetFileSizeEx(img, &isz))
	{
		fail(image);
		say("Put KEXSETUP.ISO beside kexmedia.exe, or name it: kexmedia D:\\KEXSETUP.ISO\n");
		return 1;
	}
	pr("Image: %s (%lu MB)\n\n", image, (unsigned long) (isz.QuadPart >> 20));

	sys = system_disk();
	for (i = 0; i < MAXDISK && nd < MAXDISK; i++)
		if (i != sys && probe(i, &disks[nd]))
			nd++;
	if (!nd)
	{
		say("No USB stick found. Plug one in (it will be erased) and run this again.\n");
		return 1;
	}
	say("USB sticks and removable disks:\n");
	for (i = 0; i < nd; i++)
		pr("  %d)  %-40s %6lu MB\n", i + 1, disks[i].name, (unsigned long) (disks[i].size >> 20));
	say("\nWhich one? (number, or Enter to stop): ");
	if (!read_line(line, sizeof(line)))
		return 1;
	pick = atoi(line) - 1;
	if (pick < 0 || pick >= nd)
	{
		say("Nothing was changed.\n");
		return 1;
	}
	if (disks[pick].size < isz.QuadPart)
	{
		say("That stick is too small for the image.\n");
		return 1;
	}
	pr("\nEVERYTHING on %s (%lu MB) will be erased.\n"
	       "Type ERASE (in capitals) to go ahead, or anything else to stop: ",
	       disks[pick].name, (unsigned long) (disks[pick].size >> 20));
	if (!read_line(line, sizeof(line)) || strncmp(line, "ERASE", 5) || (line[5] && line[5] != '\n' && line[5] != '\r'))
	{
		say("Nothing was changed.\n");
		return 1;
	}

	if (!release_volumes(disks[pick].num))
		return 1;
	sprintf(path, "\\\\.\\PhysicalDrive%d", disks[pick].num);
	dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	                  FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
	if (dev == INVALID_HANDLE_VALUE)
	{
		fail("Could not open the stick (run kexmedia as administrator)");
		return 1;
	}
	buf = (BYTE *) VirtualAlloc(NULL, CHUNK, MEM_COMMIT, PAGE_READWRITE);
	vbuf = (BYTE *) VirtualAlloc(NULL, CHUNK, MEM_COMMIT, PAGE_READWRITE);
	if (!buf || !vbuf)
		return 1;

	/* write */
	for (done = 0; done < isz.QuadPart; done += CHUNK)
	{
		DWORD want = (DWORD) (isz.QuadPart - done < CHUNK ? isz.QuadPart - done : CHUNK), n = 0, w;
		memset(buf, 0, CHUNK);
		if (!ReadFile(img, buf, want, &n, NULL) || n != want)
		{
			fail("Reading the image");
			return 1;
		}
		w = (want + 511) & ~511u;   /* whole sectors */
		if (!WriteFile(dev, buf, w, &got, NULL) || got != w)
		{
			fail("Writing the stick");
			return 1;
		}
		pr("\rWriting  %3d%%", (int) ((done + want) * 100 / isz.QuadPart));
	}
	FlushFileBuffers(dev);

	/* verify */
	zero.QuadPart = 0;
	SetFilePointerEx(img, zero, NULL, FILE_BEGIN);
	SetFilePointerEx(dev, zero, NULL, FILE_BEGIN);
	say("\n");
	for (done = 0; done < isz.QuadPart; done += CHUNK)
	{
		DWORD want = (DWORD) (isz.QuadPart - done < CHUNK ? isz.QuadPart - done : CHUNK), n = 0, w;
		memset(buf, 0, CHUNK);
		ReadFile(img, buf, want, &n, NULL);
		w = (want + 511) & ~511u;
		if (!ReadFile(dev, vbuf, w, &got, NULL) || got != w || memcmp(buf, vbuf, want))
		{
			pr("\nThe stick does not read back what was written (at %lu MB): it may be faulty. Try another one.\n",
			       (unsigned long) (done >> 20));
			return 1;
		}
		pr("\rChecking %3d%%", (int) ((done + want) * 100 / isz.QuadPart));
	}
	DeviceIoControl(dev, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL);
	CloseHandle(dev);
	say("\n\nDone. The stick is ready: start the PC from it (BIOS or UEFI) to install Windows 98.\n"
	    "Windows may now say the stick needs formatting: do NOT format it, just remove it.\n");
	return 0;
}

/* entry point (no C runtime start-up): split the command line into at most one argument */
void __stdcall entrypoint(void)
{
	static char cmd[MAX_PATH * 2];
	char *argv[3] = { "kexmedia", NULL, NULL }, *p;
	int argc = 1;
	lstrcpynA(cmd, GetCommandLineA(), sizeof(cmd));
	p = cmd;
	if (*p == '"')
	{
		p = strchr(p + 1, '"');
		p = p ? p + 1 : cmd + strlen(cmd);
	}
	else
		while (*p && *p != ' ')
			p++;
	while (*p == ' ')
		p++;
	if (*p)
	{
		char *e;
		if (*p == '"')
		{
			p++;
			e = strchr(p, '"');
		}
		else
			e = strchr(p, ' ');
		if (e)
			*e = 0;
		argv[argc++] = p;
	}
	ExitProcess((UINT) kmain(argc, argv));
}
