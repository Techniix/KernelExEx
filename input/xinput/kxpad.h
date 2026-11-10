/*
 *  KernelExEx - gamepad mapping engine (shared by XInput and the configurator).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 *
 *  Mappings use SDL's gamecontrollerdb.txt line format:
 *      GUID,Name,a:b0,b:b1,...,leftx:a0,lefty:a1,dpup:h0.1,...,platform:Windows 9x,
 *  Input numbering is that of the Windows 9x joystick API (joyGetPosEx):
 *      a0..a5 = X, Y, Z, R, U, V axes   b0..b31 = buttons   h0 = POV hat
 *  Modifiers as in SDL: +a2 / -a2 (half axis), a2~ (inverted).
 *  The GUID is SDL's: bus 0x0003 (USB) + vendor + product, little-endian.
 *  This file has no Windows dependencies so it can be tested on any host.
 */
#ifndef KXPAD_H
#define KXPAD_H

#include <stddef.h>

enum kxpad_target
{
	KXT_A, KXT_B, KXT_X, KXT_Y, KXT_BACK, KXT_GUIDE, KXT_START,
	KXT_LEFTSTICK, KXT_RIGHTSTICK, KXT_LEFTSHOULDER, KXT_RIGHTSHOULDER,
	KXT_DPUP, KXT_DPDOWN, KXT_DPLEFT, KXT_DPRIGHT,
	KXT_LEFTX, KXT_LEFTY, KXT_RIGHTX, KXT_RIGHTY, KXT_LEFTTRIGGER, KXT_RIGHTTRIGGER,
	KXT_COUNT
};

enum kxpad_src_type { KXS_NONE = 0, KXS_BUTTON, KXS_AXIS, KXS_HAT };

typedef struct
{
	unsigned char type;      /* kxpad_src_type */
	unsigned char index;     /* button 0..31, axis 0..5, hat 0 */
	unsigned char hatmask;   /* 1 up, 2 right, 4 down, 8 left */
	signed char half;        /* 0 full axis, +1 positive half, -1 negative half */
	unsigned char invert;
} kxpad_bind;

typedef struct
{
	char guid[33];
	char name[64];
	kxpad_bind bind[KXT_COUNT];
} kxpad_map;

/* raw input: axes -32768..32767 (as read, not yet inverted), button bits, hat bits */
typedef struct
{
	int axis[6];
	unsigned long buttons;
	unsigned hat;            /* 1 up, 2 right, 4 down, 8 left; 0 centred */
} kxpad_raw;

/* XInput-shaped result */
typedef struct
{
	unsigned short buttons;  /* XINPUT_GAMEPAD_* bits */
	unsigned char lt, rt;
	short lx, ly, rx, ry;    /* y up = positive, as XInput */
} kxpad_out;

#define KXPAD_GUIDE_BIT 0x0400   /* XInputGetStateEx only */

/* parse one mapping line; returns 0 on success */
int kxpad_parse(const char *line, kxpad_map *m);
/* write a mapping line (no newline); returns length, or -1 if it does not fit */
int kxpad_format(const kxpad_map *m, char *out, size_t outlen);
/* default mapping for a device with nothing in the database */
void kxpad_default(kxpad_map *m, const char *guid, const char *name, int naxes);
/* SDL GUID for a USB device */
void kxpad_usb_guid(unsigned vid, unsigned pid, char out[33]);
/* apply a mapping */
void kxpad_apply(const kxpad_map *m, const kxpad_raw *raw, kxpad_out *out);
/* POV angle in hundredths of a degree (0xFFFF = centred) -> hat bits */
unsigned kxpad_pov_to_hat(unsigned long pov);
/* winmm axis value in [lo, hi] -> -32768..32767 */
int kxpad_scale_axis(unsigned long v, unsigned long lo, unsigned long hi);

/* database: lines of mappings, later lines override earlier ones with the same GUID */
int kxpad_db_find(const char *db, size_t len, const char *guid, kxpad_map *m);

extern const char *const kxpad_target_names[KXT_COUNT];

#endif
