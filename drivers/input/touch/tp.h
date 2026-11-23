/*
 *  KernelExEx - touchpad and pointing-stick core (portable, no OS calls).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Protocol code is ported from Linux drivers/input/mouse (alps.c, synaptics.c,
 *  elantech.c, trackpoint.c; GPL-2.0-only; Neil Brown, Peter Osterlund, Dmitry
 *  Torokhov, Vojtech Pavlik, Sebastian Kapfer, Seth Forshee and others).
 *
 *  The platform (KEXTOUCH.VXD, or the host test harness) supplies:
 *    - tp_ps2_command(): one PS/2 command to the aux device, Linux encoding
 *      (bits 15:12 parameter bytes sent, 11:8 result bytes read, 7:0 command);
 *    - tp_log(): diagnostics (KEDCL: goes to the shared driver log);
 *    - a millisecond clock passed into tp_byte()/tp_tick().
 *  The core turns bytes from the device into pointer events (tp_out).
 */
#ifndef KEX_TP_H
#define KEX_TP_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;

/* PS/2 mouse commands (Linux psmouse.h encoding) */
#define PS2_SETSCALE11 0x00e6
#define PS2_SETSCALE21 0x00e7
#define PS2_SETRES 0x10e8
#define PS2_GETINFO 0x03e9
#define PS2_SETSTREAM 0x00ea
#define PS2_SETPOLL 0x00f0
#define PS2_RESET_WRAP 0x00ec
#define PS2_GETID 0x02f2
#define PS2_GETID1 0x01f2      /* ALPS nibble 0xa */
#define PS2_SETRATE 0x10f3
#define PS2_ENABLE 0x00f4
#define PS2_DISABLE 0x00f5
#define PS2_RESET_DIS 0x00f6
#define PS2_RESET_BAT 0x02ff

struct tp;
/* 0 on success; param holds the parameters going out and the results coming back */
int tp_ps2_command(struct tp *t, u8 *param, int command);
void tp_log(struct tp *t, const char *fmt, ...);

enum tp_kind
{
	TP_NONE,
	TP_PS2,          /* plain 3-byte mouse */
	TP_IMPS2,        /* IntelliMouse: 4 bytes, wheel */
	TP_EXPS2,        /* IntelliMouse Explorer: wheel + buttons 4/5 */
	TP_ALPS,
	TP_SYNAPTICS,
	TP_ELANTECH,
	TP_TRACKPOINT    /* stand-alone IBM/Lenovo TrackPoint */
};

/* what one touchpad report says (all protocols) */
struct tp_frame
{
	int fingers;           /* 0 = no contact */
	int x, y, z;           /* first contact, device units (0..x_max, 0..y_max; y grows down) */
	int x2, y2;            /* second contact when fingers >= 2 */
	int left, right, middle;
	int width;             /* 0 if unknown */
};

/* what goes to Windows */
struct tp_out
{
	int dx, dy;            /* mickeys, y grows down */
	int buttons;           /* bit 0 left, 1 right, 2 middle */
	int wheel;             /* + = away from the user (WHEEL_DELTA units: 120 a notch) */
	int hwheel;            /* + = right */
};

/* user settings (registry HKLM\System\CurrentControlSet\Services\Class\Mouse\<n>\KexTouch
 * or HKCU\Software\KernelExEx\Touch; the helper writes them, the VxD reads them) */
struct tp_settings
{
	int tap;               /* tap to click (1 finger left, 2 fingers right) */
	int tap_drag;          /* tap then touch = drag */
	int scroll_two;        /* two-finger scrolling */
	int scroll_edge;       /* right-edge vertical, bottom-edge horizontal scrolling */
	int scroll_reverse;    /* "natural" direction */
	int scroll_speed;      /* 1..20, 10 = default */
	int speed;             /* pointer speed 1..20, 10 = default */
	int stick_scroll;      /* TrackPoint: hold middle button + move = scroll */
	int stick_speed;       /* 1..20 */
	int palm;              /* ignore wide/heavy contacts */
};

struct tp_alps
{
	int proto, flags, quirks;
	u8 byte0, mask0;
	u8 e7[3], ec[3];
	int x_max, y_max, x_bits, y_bits;
	const u8 (*nibble)[2];   /* nibble commands: { cmd low byte, param } */
	const u16 *nibble_cmd;
	int addr_command;
	int prev_fin, multi_packet, second_touch;
	u8 multi_data[6];
	int v4_fingers, v4_x2, v4_y2;
};

struct tp_syn
{
	u32 identity, model, caps, ext0c;
	int newabs;
};

struct tp
{
	void *ctx;                 /* platform data */
	enum tp_kind kind;
	int pktsize;
	u8 packet[8];
	int pktcnt;
	int out_of_sync;
	u32 last_byte_ms;
	int x_max, y_max;          /* touchpad range */
	int has_stick;
	struct tp_alps alps;
	struct tp_syn syn;
	struct tp_settings set;
	/* gesture state */
	struct
	{
		int fingers, x, y, x2, y2;     /* previous frame */
		int touch_ms, moved, max_fingers;
		int tap_pending, tap_buttons;  /* click to send after the tap timeout */
		u32 tap_up_ms;
		int drag;                      /* tap-and-drag held */
		int repress;                   /* double tap: press again on the next tick */
		int scroll;                    /* 0 none, 1 two-finger, 2 right edge, 3 bottom edge */
		int acc_x, acc_y, acc_w, acc_h;  /* fractional remainders */
		int stick_mid, stick_scrolled, stick_buttons;
		int hw_buttons;
		int sent_buttons;
	} g;
	char name[48];             /* "ALPS v3 (Pinnacle) E7=73 02 64 EC=88 07 9d" */
};

/* detect the device and put it into its best mode; 0 = found something */
int tp_init(struct tp *t);
/* one byte from the aux port; returns 1 and fills *out when there is something to post */
int tp_byte(struct tp *t, u8 b, u32 now_ms, struct tp_out *out);
/* call every ~20 ms: tap timeouts, packet flushes; returns 1 with *out like tp_byte */
int tp_tick(struct tp *t, u32 now_ms, struct tp_out *out);
void tp_default_settings(struct tp_settings *s);

/* internal: protocol modules */
int tp_alps_detect(struct tp *t);
int tp_alps_init(struct tp *t);
int tp_alps_packet(struct tp *t, struct tp_frame *f, struct tp_out *stick, int *have_stick, int *have_frame);
int tp_alps_valid_byte(struct tp *t, int idx, u8 b);
int tp_syn_detect(struct tp *t);
int tp_syn_init(struct tp *t);
int tp_syn_packet(struct tp *t, struct tp_frame *f, struct tp_out *stick, int *have_stick, int *have_frame);
int tp_syn_valid_byte(struct tp *t, int idx, u8 b);
void tp_gesture(struct tp *t, const struct tp_frame *f, u32 now_ms, struct tp_out *out);
int tp_gesture_tick(struct tp *t, u32 now_ms, struct tp_out *out);
void tp_stick(struct tp *t, int dx, int dy, int buttons, struct tp_out *out);

#endif
