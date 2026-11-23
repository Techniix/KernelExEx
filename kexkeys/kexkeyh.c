/*
 *  KernelExEx - kexkeyh.dll: kexkeys.exe's keyboard hook (laptop Fn / multimedia keys
 *  on PS/2 keyboards). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Multimedia keys send extended (E0) scancodes that Windows 98 has no virtual keys
 *  for. The hook looks at the scancode and extended bit in lParam, tells kexkeys.exe
 *  (a registered window message) and swallows the key so it does not type a letter.
 */
#include <windows.h>
#include "kexkeys.h"

/* the hook runs in every program: no shared variables, kexkeys.exe's window
 * (class KK_CLASS) holds the hook handle as a window property */
static HHOOK hook = NULL;   /* valid in kexkeys.exe only */
static HINSTANCE self;
static UINT msg_key;

/* scancodes (extended) -> KK_* actions */
static int action_of(unsigned scan)
{
	switch (scan)
	{
	case 0x20: return KK_MUTE;
	case 0x2e: return KK_VOLDOWN;
	case 0x30: return KK_VOLUP;
	case 0x22: return KK_PLAY;
	case 0x24: return KK_STOP;
	case 0x19: return KK_NEXT;
	case 0x10: return KK_PREV;
	}
	return 0;
}

static LRESULT CALLBACK kbd(int code, WPARAM wp, LPARAM lp)
{
	HWND target = FindWindowA(KK_CLASS, NULL);
	HHOOK h = target ? (HHOOK) GetPropA(target, "KexKeysHook") : hook;
	if (code == HC_ACTION)
	{
		unsigned scan = (unsigned) (lp >> 16) & 0xff;
		int ext = (lp & (1L << 24)) != 0, up = (lp & (1L << 31)) != 0, a;
		if (ext && (a = action_of(scan)) != 0)
		{
			if (!up && !(lp & (1L << 30)) && target)   /* first press, not auto-repeat... */
				PostMessageA(target, msg_key, (WPARAM) a, 0);
			else if (!up && target && (a == KK_VOLUP || a == KK_VOLDOWN))   /* ...but volume repeats */
				PostMessageA(target, msg_key, (WPARAM) a, 0);
			return 1;   /* swallowed */
		}
	}
	return CallNextHookEx(h, code, wp, lp);
}

__declspec(dllexport) BOOL WINAPI kk_hook(HWND to)
{
	msg_key = RegisterWindowMessageA(KK_MESSAGE);
	if (!hook)
		hook = SetWindowsHookExA(WH_KEYBOARD, kbd, self, 0);
	if (hook)
		SetPropA(to, "KexKeysHook", (HANDLE) hook);
	return hook != NULL;
}

__declspec(dllexport) void WINAPI kk_unhook(HWND to)
{
	if (hook)
		UnhookWindowsHookEx(hook);
	hook = NULL;
	RemovePropA(to, "KexKeysHook");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
	(void) r;
	if (reason == DLL_PROCESS_ATTACH)
	{
		self = h;
		msg_key = RegisterWindowMessageA(KK_MESSAGE);
	}
	return TRUE;
}
