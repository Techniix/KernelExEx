/*
 *  KernelExEx - XInput for Windows 98 SE / ME.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 *
 *  One binary, installed as xinput1_1.dll, xinput1_2.dll, xinput1_3.dll,
 *  xinput1_4.dll and xinput9_1_0.dll. Controllers are the game controllers
 *  Windows knows (joyGetPosEx), so any pad with a working Windows 98 driver
 *  appears as an XInput pad. Behaviour follows the XInput documentation;
 *  Wine's xinput was used as a reference for edge cases.
 *
 *  Mappings (kxpad.h) come from, in order, later lines winning:
 *    <KernelEx dir>\gamepad\gamecontrollerdb.txt   (shipped)
 *    <KernelEx dir>\gamepad\user.txt               (written by the configurator)
 *  A device with no mapping gets kxpad_default().
 *
 *  Not implemented: vibration (accepted and ignored), headset audio.
 */
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <stdio.h>
#include "kxpad.h"
#include "../../net/rt/w9xrt.h"

#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif
#ifndef ERROR_EMPTY
#define ERROR_EMPTY 4306
#endif

#ifndef XINPUT_KEYSTROKE_KEYDOWN
#define XINPUT_KEYSTROKE_KEYDOWN 1
#define XINPUT_KEYSTROKE_KEYUP 2
#define XINPUT_KEYSTROKE_REPEAT 4
#endif

#define XUSER_MAX_COUNT 4
#define XUSER_INDEX_ANY 0xFF

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } KX_GAMEPAD;
typedef struct { DWORD dwPacketNumber; KX_GAMEPAD Gamepad; } KX_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } KX_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; KX_GAMEPAD Gamepad; KX_VIBRATION Vibration; } KX_CAPS;
typedef struct { BYTE BatteryType, BatteryLevel; } KX_BATTERY;
typedef struct { WORD VirtualKey; WCHAR Unicode; WORD Flags; BYTE UserIndex, HidCode; } KX_KEYSTROKE;

typedef struct
{
	int present;
	UINT joyid;
	JOYCAPSA caps;
	kxpad_map map;
	DWORD packet;
	kxpad_out last;
	/* keystroke generation */
	unsigned long held;      /* VK_PAD bits currently down, see key table */
	DWORD repeat_at[24];
} pad_slot;

static CRITICAL_SECTION lock;
static pad_slot pads[XUSER_MAX_COUNT];
static DWORD next_scan;
static BOOL enabled = TRUE;
static char *db;
static size_t db_len;
static FILETIME db_time[2];
static char kexdir[MAX_PATH];

/* ---- mapping database --------------------------------------------- */

static void kex_dir(void)
{
	HKEY k;
	DWORD n = sizeof(kexdir), type;
	kexdir[0] = 0;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\KernelEx", 0, KEY_READ, &k) == ERROR_SUCCESS)
	{
		if (RegQueryValueExA(k, "InstallDir", NULL, &type, (BYTE *) kexdir, &n) != ERROR_SUCCESS || type != REG_SZ)
			kexdir[0] = 0;
		RegCloseKey(k);
	}
	if (!kexdir[0])
	{
		GetWindowsDirectoryA(kexdir, sizeof(kexdir) - 10);
		strcat(kexdir, "\\KernelEx");
	}
}

static int file_time(const char *path, FILETIME *t)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(path, &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		memset(t, 0, sizeof(*t));
		return 0;
	}
	FindClose(h);
	*t = fd.ftLastWriteTime;
	return 1;
}

static void append_file(const char *path)
{
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	DWORD size, got;
	char *nb;
	if (h == INVALID_HANDLE_VALUE)
		return;
	size = GetFileSize(h, NULL);
	if (size != 0xFFFFFFFF && size < 8 * 1024 * 1024)
	{
		nb = (char *) HeapReAlloc(GetProcessHeap(), 0, db, db_len + size + 2);
		if (nb)
		{
			db = nb;
			if (ReadFile(h, db + db_len, size, &got, NULL))
			{
				db_len += got;
				db[db_len++] = '\n';
			}
		}
	}
	CloseHandle(h);
}

/* (re)load when either file changed; caller holds the lock */
static int load_db(void)
{
	char a[MAX_PATH + 40], b[MAX_PATH + 40];
	FILETIME t[2];
	if (!kexdir[0])
		kex_dir();
	w9x_snprintf(a, sizeof(a), "%s\\gamepad\\gamecontrollerdb.txt", kexdir);
	w9x_snprintf(b, sizeof(b), "%s\\gamepad\\user.txt", kexdir);
	file_time(a, &t[0]);
	file_time(b, &t[1]);
	if (db && !memcmp(t, db_time, sizeof(t)))
		return 0;
	if (!db)
		db = (char *) HeapAlloc(GetProcessHeap(), 0, 1);
	db_len = 0;
	if (db)
	{
		append_file(a);
		append_file(b);
	}
	memcpy(db_time, t, sizeof(t));
	return 1;
}

/* USB vendor/product from the joystick's OEM name ("VID_045E&PID_028E") */
static int joy_vid_pid(UINT id, const JOYCAPSA *caps, unsigned *vid, unsigned *pid)
{
	static const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
	char key[MAX_PATH], val[32], oem[MAX_PATH];
	int r;
	*vid = caps->wMid;
	*pid = caps->wPid;
	w9x_snprintf(key, sizeof(key), "System\\CurrentControlSet\\Control\\MediaResources\\joystick\\%s\\CurrentJoystickSettings",
	         caps->szRegKey);
	w9x_snprintf(val, sizeof(val), "Joystick%uOEMName", id + 1);
	for (r = 0; r < 2; r++)
	{
		HKEY k;
		DWORD n = sizeof(oem), type;
		if (RegOpenKeyExA(roots[r], key, 0, KEY_READ, &k) != ERROR_SUCCESS)
			continue;
		if (RegQueryValueExA(k, val, NULL, &type, (BYTE *) oem, &n) == ERROR_SUCCESS && type == REG_SZ)
		{
			unsigned v, p;
			char *s;
			oem[sizeof(oem) - 1] = 0;
			CharUpperA(oem);
			s = strstr(oem, "VID_");
			if (s && sscanf(s, "VID_%4x&PID_%4x", &v, &p) == 2)
			{
				*vid = v;
				*pid = p;
				RegCloseKey(k);
				return 1;
			}
		}
		RegCloseKey(k);
	}
	return 0;
}

/* ---- device scan ---------------------------------------------------- */

static void scan(void)
{
	UINT n = joyGetNumDevs(), id;
	int slot = 0, i;
	pad_slot fresh[XUSER_MAX_COUNT];
	memset(fresh, 0, sizeof(fresh));
	load_db();
	for (id = 0; id < n && id < 16 && slot < XUSER_MAX_COUNT; id++)
	{
		JOYINFOEX ji;
		JOYCAPSA caps;
		unsigned vid, pid;
		char guid[33];
		ji.dwSize = sizeof(ji);
		ji.dwFlags = JOY_RETURNALL;
		if (joyGetPosEx(id, &ji) != JOYERR_NOERROR)
			continue;
		if (joyGetDevCapsA(id, &caps, sizeof(caps)) != JOYERR_NOERROR)
			continue;
		joy_vid_pid(id, &caps, &vid, &pid);
		kxpad_usb_guid(vid, pid, guid);
		/* keep the slot of a controller that was already connected */
		for (i = 0; i < XUSER_MAX_COUNT; i++)
			if (pads[i].present && pads[i].joyid == id && !memcmp(pads[i].map.guid, guid, 32))
				break;
		if (i < XUSER_MAX_COUNT && !fresh[i].present)
		{
			fresh[i] = pads[i];
			continue;
		}
		while (slot < XUSER_MAX_COUNT && (fresh[slot].present || pads[slot].present))
			slot++;
		if (slot >= XUSER_MAX_COUNT)
			break;
		fresh[slot].present = 1;
		fresh[slot].joyid = id;
		fresh[slot].caps = caps;
		if (!kxpad_db_find(db, db_len, guid, &fresh[slot].map))
			kxpad_default(&fresh[slot].map, guid, caps.szPname, (int) caps.wNumAxes);
		memcpy(fresh[slot].map.guid, guid, 33);
	}
	memcpy(pads, fresh, sizeof(pads));
}

static void maybe_scan(void)
{
	DWORD now = GetTickCount();
	if ((LONG) (now - next_scan) >= 0)
	{
		scan();
		next_scan = now + 2000;   /* hot-plug is noticed within two seconds */
	}
}

/* read one controller; caller holds the lock */
static int poll(pad_slot *p, kxpad_out *out)
{
	JOYINFOEX ji;
	kxpad_raw raw;
	const JOYCAPSA *c = &p->caps;
	ji.dwSize = sizeof(ji);
	ji.dwFlags = JOY_RETURNALL | JOY_RETURNPOVCTS;
	if (joyGetPosEx(p->joyid, &ji) != JOYERR_NOERROR)
	{
		p->present = 0;
		return 0;
	}
	raw.axis[0] = kxpad_scale_axis(ji.dwXpos, c->wXmin, c->wXmax);
	raw.axis[1] = kxpad_scale_axis(ji.dwYpos, c->wYmin, c->wYmax);
	raw.axis[2] = kxpad_scale_axis(ji.dwZpos, c->wZmin, c->wZmax);
	raw.axis[3] = kxpad_scale_axis(ji.dwRpos, c->wRmin, c->wRmax);
	raw.axis[4] = kxpad_scale_axis(ji.dwUpos, c->wUmin, c->wUmax);
	raw.axis[5] = kxpad_scale_axis(ji.dwVpos, c->wVmin, c->wVmax);
	raw.buttons = ji.dwButtons;
	raw.hat = (c->wCaps & JOYCAPS_HASPOV) ? kxpad_pov_to_hat(ji.dwPOV) : 0;
	kxpad_apply(&p->map, &raw, out);
	if (memcmp(out, &p->last, sizeof(*out)))
	{
		p->last = *out;
		p->packet++;
	}
	return 1;
}

static pad_slot *get_pad(DWORD index, kxpad_out *out)
{
	pad_slot *p;
	if (index >= XUSER_MAX_COUNT)
		return NULL;
	maybe_scan();
	p = &pads[index];
	if (!p->present || !poll(p, out))
		return NULL;
	return p;
}

static void to_gamepad(const kxpad_out *o, KX_GAMEPAD *g, int with_guide)
{
	g->wButtons = (WORD) (with_guide ? o->buttons : (o->buttons & ~KXPAD_GUIDE_BIT));
	g->bLeftTrigger = o->lt;
	g->bRightTrigger = o->rt;
	g->sThumbLX = o->lx;
	g->sThumbLY = o->ly;
	g->sThumbRX = o->rx;
	g->sThumbRY = o->ry;
}

static DWORD get_state(DWORD index, KX_STATE *state, int with_guide)
{
	kxpad_out o;
	pad_slot *p;
	DWORD r;
	if (!state)
		return ERROR_BAD_ARGUMENTS;
	EnterCriticalSection(&lock);
	p = get_pad(index, &o);
	if (!p)
		r = index >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_DEVICE_NOT_CONNECTED;
	else
	{
		state->dwPacketNumber = p->packet;
		if (enabled)
			to_gamepad(&o, &state->Gamepad, with_guide);
		else
			memset(&state->Gamepad, 0, sizeof(state->Gamepad));   /* XInputEnable(FALSE): neutral */
		r = ERROR_SUCCESS;
	}
	LeaveCriticalSection(&lock);
	return r;
}

/* ---- exports -------------------------------------------------------- */

DWORD WINAPI XInputGetState(DWORD dwUserIndex, KX_STATE *pState)
{
	return get_state(dwUserIndex, pState, 0);
}

DWORD WINAPI XInputGetStateEx(DWORD dwUserIndex, KX_STATE *pState)
{
	return get_state(dwUserIndex, pState, 1);
}

DWORD WINAPI XInputSetState(DWORD dwUserIndex, KX_VIBRATION *pVibration)
{
	kxpad_out o;
	DWORD r;
	if (!pVibration)
		return ERROR_BAD_ARGUMENTS;
	EnterCriticalSection(&lock);
	r = get_pad(dwUserIndex, &o) ? ERROR_SUCCESS :
	    dwUserIndex >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_DEVICE_NOT_CONNECTED;
	LeaveCriticalSection(&lock);
	return r;   /* no rumble yet: accepted and ignored */
}

DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, KX_CAPS *pCaps)
{
	kxpad_out o;
	DWORD r;
	if (!pCaps || (dwFlags & ~1u))
		return ERROR_BAD_ARGUMENTS;
	EnterCriticalSection(&lock);
	if (get_pad(dwUserIndex, &o))
	{
		memset(pCaps, 0, sizeof(*pCaps));
		pCaps->Type = 1;       /* XINPUT_DEVTYPE_GAMEPAD */
		pCaps->SubType = 1;    /* XINPUT_DEVSUBTYPE_GAMEPAD */
		pCaps->Flags = 0;
		pCaps->Gamepad.wButtons = 0xF3FF;
		pCaps->Gamepad.bLeftTrigger = pCaps->Gamepad.bRightTrigger = 0xFF;
		pCaps->Gamepad.sThumbLX = pCaps->Gamepad.sThumbLY = (SHORT) 0xFFC0;
		pCaps->Gamepad.sThumbRX = pCaps->Gamepad.sThumbRY = (SHORT) 0xFFC0;
		r = ERROR_SUCCESS;
	}
	else
		r = dwUserIndex >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_DEVICE_NOT_CONNECTED;
	LeaveCriticalSection(&lock);
	return r;
}

void WINAPI XInputEnable(BOOL enable)
{
	enabled = enable;
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID *pRender, GUID *pCapture)
{
	kxpad_out o;
	DWORD r;
	if (!pRender || !pCapture)
		return ERROR_BAD_ARGUMENTS;
	EnterCriticalSection(&lock);
	r = get_pad(dwUserIndex, &o) ? ERROR_SUCCESS :
	    dwUserIndex >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_DEVICE_NOT_CONNECTED;
	LeaveCriticalSection(&lock);
	if (r == ERROR_SUCCESS)
	{
		memset(pRender, 0, sizeof(*pRender));   /* GUID_NULL: no headset */
		memset(pCapture, 0, sizeof(*pCapture));
	}
	return r;
}

DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRender, UINT *pRenderCount,
                                     LPWSTR pCapture, UINT *pCaptureCount)
{
	kxpad_out o;
	DWORD r;
	EnterCriticalSection(&lock);
	r = get_pad(dwUserIndex, &o) ? ERROR_SUCCESS :
	    dwUserIndex >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_DEVICE_NOT_CONNECTED;
	LeaveCriticalSection(&lock);
	if (r == ERROR_SUCCESS)
	{
		if (pRender && pRenderCount && *pRenderCount) pRender[0] = 0;
		if (pCapture && pCaptureCount && *pCaptureCount) pCapture[0] = 0;
		if (pRenderCount) *pRenderCount = 0;
		if (pCaptureCount) *pCaptureCount = 0;
	}
	return r;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType, KX_BATTERY *pInfo)
{
	kxpad_out o;
	DWORD r;
	if (!pInfo)
		return ERROR_BAD_ARGUMENTS;
	EnterCriticalSection(&lock);
	r = get_pad(dwUserIndex, &o) ? ERROR_SUCCESS :
	    dwUserIndex >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_DEVICE_NOT_CONNECTED;
	LeaveCriticalSection(&lock);
	if (r == ERROR_SUCCESS)
	{
		/* devType 1 = headset: none */
		pInfo->BatteryType = devType == 1 ? 0 /* DISCONNECTED */ : 1 /* WIRED */;
		pInfo->BatteryLevel = devType == 1 ? 0 : 3 /* FULL */;
	}
	return r;
}

/* keystrokes: XInputGetKeystroke reports presses, releases and repeats */
static const struct { WORD vk; unsigned short bit; } key_buttons[] = {
	{ 0x5800, 0x1000 }, { 0x5801, 0x2000 }, { 0x5802, 0x4000 }, { 0x5803, 0x8000 },
	{ 0x5804, 0x0200 }, { 0x5805, 0x0100 }, { 0x5810, 0x0001 }, { 0x5811, 0x0002 },
	{ 0x5812, 0x0004 }, { 0x5813, 0x0008 }, { 0x5814, 0x0010 }, { 0x5815, 0x0020 },
	{ 0x5816, 0x0040 }, { 0x5817, 0x0080 },
};
#define N_KEY_BUTTONS (sizeof(key_buttons) / sizeof(key_buttons[0]))
#define TRIGGER_THRESHOLD 30
#define STICK_THRESHOLD 20000

/* direction of a stick as an index 0..7 (up, down, right, left, upleft, upright, downright, downleft) or -1 */
static int stick_dir(int x, int y)
{
	int up = y > STICK_THRESHOLD, down = y < -STICK_THRESHOLD;
	int right = x > STICK_THRESHOLD, left = x < -STICK_THRESHOLD;
	if (up && left) return 4;
	if (up && right) return 5;
	if (down && right) return 6;
	if (down && left) return 7;
	if (up) return 0;
	if (down) return 1;
	if (right) return 2;
	if (left) return 3;
	return -1;
}

/* the set of "keys" held now, one bit per entry of a virtual key list:
 * 0..13 buttons, 14 LT, 15 RT, 16 left stick, 17 right stick (direction kept separately) */
static WORD vk_for(int k, int ldir, int rdir)
{
	if (k < (int) N_KEY_BUTTONS) return key_buttons[k].vk;
	if (k == 14) return 0x5806;
	if (k == 15) return 0x5807;
	if (k == 16) return (WORD) (0x5820 + ldir);
	return (WORD) (0x5830 + rdir);
}

static int keystroke_for(DWORD index, pad_slot *p, const kxpad_out *o, KX_KEYSTROKE *ks)
{
	static int ldir_prev[XUSER_MAX_COUNT] = { -1, -1, -1, -1 }, rdir_prev[XUSER_MAX_COUNT] = { -1, -1, -1, -1 };
	unsigned long now_held = 0;
	int ldir = stick_dir(o->lx, o->ly), rdir = stick_dir(o->rx, o->ry);
	DWORD now = GetTickCount();
	int k;
	for (k = 0; k < (int) N_KEY_BUTTONS; k++)
		if (o->buttons & key_buttons[k].bit)
			now_held |= 1ul << k;
	if (o->lt > TRIGGER_THRESHOLD) now_held |= 1ul << 14;
	if (o->rt > TRIGGER_THRESHOLD) now_held |= 1ul << 15;
	if (ldir >= 0) now_held |= 1ul << 16;
	if (rdir >= 0) now_held |= 1ul << 17;
	/* a stick that changed direction releases the old one first */
	if ((p->held & (1ul << 16)) && ldir != ldir_prev[index]) now_held &= ~(1ul << 16);
	if ((p->held & (1ul << 17)) && rdir != rdir_prev[index]) now_held &= ~(1ul << 17);
	for (k = 0; k < 18; k++)
	{
		unsigned long bit = 1ul << k;
		int was = (p->held & bit) != 0, is = (now_held & bit) != 0;
		if (was == is && !(is && (LONG) (now - p->repeat_at[k]) >= 0))
			continue;
		memset(ks, 0, sizeof(*ks));
		ks->UserIndex = (BYTE) index;
		if (!was && is)
		{
			ks->VirtualKey = vk_for(k, ldir, rdir);
			ks->Flags = XINPUT_KEYSTROKE_KEYDOWN;
			p->held |= bit;
			p->repeat_at[k] = now + 400;
			if (k == 16) ldir_prev[index] = ldir;
			if (k == 17) rdir_prev[index] = rdir;
		}
		else if (was && !is)
		{
			ks->VirtualKey = vk_for(k, ldir_prev[index], rdir_prev[index]);
			ks->Flags = XINPUT_KEYSTROKE_KEYUP;
			p->held &= ~bit;
		}
		else
		{
			ks->VirtualKey = vk_for(k, ldir, rdir);
			ks->Flags = XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT;
			p->repeat_at[k] = now + 100;
		}
		return 1;
	}
	return 0;
}

DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, KX_KEYSTROKE *pKeystroke)
{
	DWORD first, last, i, r = ERROR_DEVICE_NOT_CONNECTED;
	(void) dwReserved;
	if (!pKeystroke)
		return ERROR_BAD_ARGUMENTS;
	if (dwUserIndex == XUSER_INDEX_ANY)
		first = 0, last = XUSER_MAX_COUNT - 1;
	else if (dwUserIndex < XUSER_MAX_COUNT)
		first = last = dwUserIndex;
	else
		return ERROR_BAD_ARGUMENTS;
	EnterCriticalSection(&lock);
	for (i = first; i <= last; i++)
	{
		kxpad_out o;
		pad_slot *p = get_pad(i, &o);
		if (!p)
			continue;
		r = ERROR_EMPTY;
		if (enabled && keystroke_for(i, p, &o, pKeystroke))
		{
			r = ERROR_SUCCESS;
			break;
		}
	}
	LeaveCriticalSection(&lock);
	return r;
}

/* xinput1_3 ordinals 101-103: no guide-button waits or power-off on this hardware */
DWORD WINAPI XInputWaitForGuideButton(DWORD dwUserIndex, DWORD dwFlag, LPVOID pVoid)
{
	(void) dwUserIndex; (void) dwFlag; (void) pVoid;
	return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI XInputCancelGuideButtonWait(DWORD dwUserIndex)
{
	(void) dwUserIndex;
	return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI XInputPowerOffController(DWORD dwUserIndex)
{
	(void) dwUserIndex;
	return ERROR_NOT_SUPPORTED;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void) reserved;
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(inst);
		InitializeCriticalSection(&lock);
		next_scan = GetTickCount();
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		DeleteCriticalSection(&lock);
		if (db)
			HeapFree(GetProcessHeap(), 0, db);
	}
	return TRUE;
}
