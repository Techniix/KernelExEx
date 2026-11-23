/*
 *  KernelExEx - kexkeys.exe: multimedia keys for Windows 98 / ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  - laptop / PS/2 keyboards: Fn + volume and media keys (extended scancodes) through
 *    the kexkeyh.dll keyboard hook;
 *  - USB keyboards: the HID Consumer Control collection (usage page 0x0C), read with
 *    Windows 98's hid.dll / setupapi.dll (loaded at run time);
 *  - volume and mute: the master volume (mixer API), so any sound driver works;
 *  - play/pause, stop, next, previous: Winamp, when it runs (Windows 98 has no
 *    WM_APPCOMMAND).
 *  Started from the Run key; one copy at a time.
 */
#include <windows.h>
#include <mmsystem.h>
#include "kexkeys.h"

static HWND wnd;
static UINT msg_key;

/* ---------------- actions ---------------- */
static int mixer_control(DWORD type, MIXERCONTROLA *mc, HMIXER *hm)
{
	MIXERLINEA ml;
	MIXERLINECONTROLSA mlc;
	if (mixerOpen(hm, 0, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR)
		return 0;
	ZeroMemory(&ml, sizeof(ml));
	ml.cbStruct = sizeof(ml);
	ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
	if (mixerGetLineInfoA((HMIXEROBJ) *hm, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE) != MMSYSERR_NOERROR)
		goto fail;
	ZeroMemory(&mlc, sizeof(mlc));
	mlc.cbStruct = sizeof(mlc);
	mlc.dwLineID = ml.dwLineID;
	mlc.dwControlType = type;
	mlc.cControls = 1;
	mlc.cbmxctrl = sizeof(*mc);
	mlc.pamxctrl = mc;
	mc->cbStruct = sizeof(*mc);
	if (mixerGetLineControlsA((HMIXEROBJ) *hm, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) != MMSYSERR_NOERROR)
		goto fail;
	return 1;
fail:
	mixerClose(*hm);
	return 0;
}

static void volume(int step)   /* step: -1 down, +1 up, 0 toggle mute */
{
	MIXERCONTROLA mc;
	MIXERCONTROLDETAILS d;
	HMIXER hm;
	if (step == 0)
	{
		MIXERCONTROLDETAILS_BOOLEAN b;
		if (!mixer_control(MIXERCONTROL_CONTROLTYPE_MUTE, &mc, &hm))
			return;
		ZeroMemory(&d, sizeof(d));
		d.cbStruct = sizeof(d);
		d.dwControlID = mc.dwControlID;
		d.cChannels = 1;
		d.cbDetails = sizeof(b);
		d.paDetails = &b;
		if (mixerGetControlDetailsA((HMIXEROBJ) hm, &d, MIXER_GETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR)
		{
			b.fValue = !b.fValue;
			mixerSetControlDetails((HMIXEROBJ) hm, &d, MIXER_SETCONTROLDETAILSF_VALUE);
		}
	}
	else
	{
		MIXERCONTROLDETAILS_UNSIGNED u[2];
		long v, range, i;
		if (!mixer_control(MIXERCONTROL_CONTROLTYPE_VOLUME, &mc, &hm))
			return;
		ZeroMemory(&d, sizeof(d));
		d.cbStruct = sizeof(d);
		d.dwControlID = mc.dwControlID;
		d.cChannels = 2;
		d.cbDetails = sizeof(u[0]);
		d.paDetails = u;
		if (mixerGetControlDetailsA((HMIXEROBJ) hm, &d, MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR)
		{
			d.cChannels = 1;   /* a mono control */
			if (mixerGetControlDetailsA((HMIXEROBJ) hm, &d, MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR)
				goto done;
		}
		range = (long) (mc.Bounds.dwMaximum - mc.Bounds.dwMinimum);
		for (i = 0; i < (long) d.cChannels; i++)
		{
			v = (long) u[i].dwValue + step * (range / 16);
			if (v < (long) mc.Bounds.dwMinimum)
				v = (long) mc.Bounds.dwMinimum;
			if (v > (long) mc.Bounds.dwMaximum)
				v = (long) mc.Bounds.dwMaximum;
			u[i].dwValue = (DWORD) v;
		}
		mixerSetControlDetails((HMIXEROBJ) hm, &d, MIXER_SETCONTROLDETAILSF_VALUE);
		/* turning the volume up ends a mute */
		if (step > 0)
		{
			MIXERCONTROLA mm;
			HMIXER h2;
			if (mixer_control(MIXERCONTROL_CONTROLTYPE_MUTE, &mm, &h2))
			{
				MIXERCONTROLDETAILS_BOOLEAN b;
				MIXERCONTROLDETAILS d2;
				ZeroMemory(&d2, sizeof(d2));
				d2.cbStruct = sizeof(d2);
				d2.dwControlID = mm.dwControlID;
				d2.cChannels = 1;
				d2.cbDetails = sizeof(b);
				d2.paDetails = &b;
				b.fValue = 0;
				mixerSetControlDetails((HMIXEROBJ) h2, &d2, MIXER_SETCONTROLDETAILSF_VALUE);
				mixerClose(h2);
			}
		}
	}
done:
	mixerClose(hm);
}

static void media(int a)
{
	HWND w = FindWindowA("Winamp v1.x", NULL);
	if (!w)
		return;
	switch (a)
	{
	case KK_PLAY:
		/* WM_USER 104: 1 playing, 3 paused, 0 stopped */
		PostMessageA(w, WM_COMMAND, SendMessageA(w, WM_USER, 0, 104) == 1 ? 40046 : 40045, 0);
		break;
	case KK_STOP: PostMessageA(w, WM_COMMAND, 40047, 0); break;
	case KK_NEXT: PostMessageA(w, WM_COMMAND, 40048, 0); break;
	case KK_PREV: PostMessageA(w, WM_COMMAND, 40044, 0); break;
	}
}

static void act(int a)
{
	switch (a)
	{
	case KK_MUTE: volume(0); break;
	case KK_VOLDOWN: volume(-1); break;
	case KK_VOLUP: volume(1); break;
	default: media(a);
	}
}

/* ---------------- USB: HID consumer control ---------------- */
typedef struct
{
	DWORD cbSize;
	GUID InterfaceClassGuid;
	DWORD Flags;
	ULONG_PTR Reserved;
} KK_IFDATA;
typedef struct
{
	USHORT Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength, Reserved[17],
	    NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices,
	    NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices, NumberFeatureButtonCaps,
	    NumberFeatureValueCaps, NumberFeatureDataIndices;
} KK_CAPS;

static void(WINAPI *pHidD_GetHidGuid)(GUID *);
static BOOLEAN(WINAPI *pHidD_GetPreparsedData)(HANDLE, PVOID *);
static LONG(WINAPI *pHidP_GetCaps)(PVOID, KK_CAPS *);
static LONG(WINAPI *pHidP_GetUsages)(int, USHORT, USHORT, USHORT *, ULONG *, PVOID, PCHAR, ULONG);
static HANDLE(WINAPI *pSetupDiGetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
static BOOL(WINAPI *pSetupDiEnumDeviceInterfaces)(HANDLE, PVOID, const GUID *, DWORD, KK_IFDATA *);
static BOOL(WINAPI *pSetupDiGetDeviceInterfaceDetailA)(HANDLE, KK_IFDATA *, PVOID, DWORD, DWORD *, PVOID);
static BOOL(WINAPI *pSetupDiDestroyDeviceInfoList)(HANDLE);

#define MAXDEV 8
static char opened[MAXDEV][260];
static int nopen;

struct dev
{
	HANDLE h;
	PVOID pp;
	ULONG len;
};

static int usage_action(USHORT u)
{
	switch (u)
	{
	case 0xe2: return KK_MUTE;
	case 0xe9: return KK_VOLUP;
	case 0xea: return KK_VOLDOWN;
	case 0xcd: return KK_PLAY;
	case 0xb7: return KK_STOP;
	case 0xb5: return KK_NEXT;
	case 0xb6: return KK_PREV;
	}
	return 0;
}

static DWORD WINAPI reader(LPVOID p)
{
	struct dev *d = (struct dev *) p;
	char rep[64];
	USHORT now[16], before[16];
	ULONG nb = 0;
	for (;;)
	{
		DWORD got = 0;
		ULONG n = 16, i, j;
		if (!ReadFile(d->h, rep, d->len > sizeof(rep) ? sizeof(rep) : d->len, &got, NULL) || !got)
			break;   /* unplugged */
		if (pHidP_GetUsages(0 /* input */, 0x0c, 0, now, &n, d->pp, rep, got) != 0x00110000L)
			continue;
		for (i = 0; i < n; i++)
		{
			int was = 0;
			for (j = 0; j < nb; j++)
				if (before[j] == now[i])
					was = 1;
			if (!was && usage_action(now[i]))
				PostMessageA(wnd, msg_key, (WPARAM) usage_action(now[i]), 0);
		}
		memcpy(before, now, n * sizeof(USHORT));
		nb = n;
	}
	CloseHandle(d->h);
	return 0;
}

static void scan_hid(void)
{
	GUID g;
	HANDLE set;
	KK_IFDATA ifd;
	DWORD i;
	if (!pHidD_GetHidGuid)
		return;
	pHidD_GetHidGuid(&g);
	set = pSetupDiGetClassDevsA(&g, NULL, NULL, 0x12 /* PRESENT | DEVICEINTERFACE */);
	if (set == INVALID_HANDLE_VALUE)
		return;
	for (i = 0;; i++)
	{
		struct
		{
			DWORD cbSize;
			char path[260];
		} det;
		HANDLE h;
		PVOID pp;
		KK_CAPS caps;
		int k, seen = 0;
		ifd.cbSize = sizeof(ifd);
		if (!pSetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd))
			break;
		det.cbSize = 5;   /* sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) on 32-bit */
		if (!pSetupDiGetDeviceInterfaceDetailA(set, &ifd, &det, sizeof(det), NULL, NULL))
			continue;
		for (k = 0; k < nopen; k++)
			if (!lstrcmpiA(opened[k], det.path))
				seen = 1;
		if (seen || nopen >= MAXDEV)
			continue;
		h = CreateFileA(det.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE)
			continue;   /* keyboards and mice are opened by Windows itself */
		if (!pHidD_GetPreparsedData(h, &pp) || pHidP_GetCaps(pp, &caps) != 0x00110000L || caps.UsagePage != 0x0c ||
		    caps.Usage != 0x01)
		{
			CloseHandle(h);
			continue;
		}
		{
			struct dev *d = (struct dev *) HeapAlloc(GetProcessHeap(), 0, sizeof(*d));
			DWORD tid;
			if (!d)
			{
				CloseHandle(h);
				continue;
			}
			d->h = h;
			d->pp = pp;
			d->len = caps.InputReportByteLength;
			lstrcpynA(opened[nopen++], det.path, 260);
			CloseHandle(CreateThread(NULL, 0, reader, d, 0, &tid));
		}
	}
	pSetupDiDestroyDeviceInfoList(set);
}

static void load_hid(void)
{
	HMODULE h = LoadLibraryA("hid.dll"), s = LoadLibraryA("setupapi.dll");
	if (!h || !s)
		return;
	*(FARPROC *) &pHidD_GetPreparsedData = GetProcAddress(h, "HidD_GetPreparsedData");
	*(FARPROC *) &pHidP_GetCaps = GetProcAddress(h, "HidP_GetCaps");
	*(FARPROC *) &pHidP_GetUsages = GetProcAddress(h, "HidP_GetUsages");
	*(FARPROC *) &pSetupDiGetClassDevsA = GetProcAddress(s, "SetupDiGetClassDevsA");
	*(FARPROC *) &pSetupDiEnumDeviceInterfaces = GetProcAddress(s, "SetupDiEnumDeviceInterfaces");
	*(FARPROC *) &pSetupDiGetDeviceInterfaceDetailA = GetProcAddress(s, "SetupDiGetDeviceInterfaceDetailA");
	*(FARPROC *) &pSetupDiDestroyDeviceInfoList = GetProcAddress(s, "SetupDiDestroyDeviceInfoList");
	if (pHidD_GetPreparsedData && pHidP_GetCaps && pHidP_GetUsages && pSetupDiGetClassDevsA &&
	    pSetupDiEnumDeviceInterfaces && pSetupDiGetDeviceInterfaceDetailA && pSetupDiDestroyDeviceInfoList)
		*(FARPROC *) &pHidD_GetHidGuid = GetProcAddress(h, "HidD_GetHidGuid");
}

/* ---------------- window ---------------- */
static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == msg_key)
	{
		act((int) wp);
		return 0;
	}
	if (m == WM_TIMER)
	{
		scan_hid();   /* newly plugged USB keyboards */
		return 0;
	}
	return DefWindowProcA(w, m, wp, lp);
}

void entrypoint(void)
{
	WNDCLASSA wc;
	MSG m;
	HMODULE hk;
	BOOL(WINAPI * hook)(HWND);
	if (FindWindowA(KK_CLASS, NULL))
		ExitProcess(0);
	msg_key = RegisterWindowMessageA(KK_MESSAGE);
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = proc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = KK_CLASS;
	RegisterClassA(&wc);
	wnd = CreateWindowA(KK_CLASS, "KernelExEx keys", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
	hk = LoadLibraryA("kexkeyh.dll");
	*(FARPROC *) &hook = hk ? GetProcAddress(hk, "kk_hook@4") : NULL;
	if (!hook && hk)
		*(FARPROC *) &hook = GetProcAddress(hk, "kk_hook");
	if (hook)
		hook(wnd);
	load_hid();
	scan_hid();
	SetTimer(wnd, 1, 5000, NULL);
	while (GetMessageA(&m, NULL, 0, 0) > 0)
	{
		TranslateMessage(&m);
		DispatchMessageA(&m);
	}
	ExitProcess(0);
}
