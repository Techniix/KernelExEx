/*
 *  KernelExEx - Xbox 360 controller protocol -> HID game pad report. See x360.h.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2, as KernelEx.
 */
#include "x360.h"

const unsigned char x360_report_descriptor[] = {
	0x05, 0x01,             /* Usage Page (Generic Desktop) */
	0x09, 0x05,             /* Usage (Game Pad) */
	0xA1, 0x01,             /* Collection (Application) */
	0x05, 0x09,             /*   Usage Page (Button) */
	0x19, 0x01,             /*   Usage Minimum (1) */
	0x29, 0x0B,             /*   Usage Maximum (11) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x01,             /*   Logical Maximum (1) */
	0x75, 0x01,             /*   Report Size (1) */
	0x95, 0x0B,             /*   Report Count (11) */
	0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
	0x95, 0x05,             /*   Report Count (5) */
	0x81, 0x03,             /*   Input (Constant): padding */
	0x05, 0x01,             /*   Usage Page (Generic Desktop) */
	0x09, 0x39,             /*   Usage (Hat Switch) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x07,             /*   Logical Maximum (7) */
	0x35, 0x00,             /*   Physical Minimum (0) */
	0x46, 0x3B, 0x01,       /*   Physical Maximum (315) */
	0x65, 0x14,             /*   Unit (degrees) */
	0x75, 0x04,             /*   Report Size (4) */
	0x95, 0x01,             /*   Report Count (1) */
	0x81, 0x42,             /*   Input (Data, Variable, Absolute, Null State) */
	0x65, 0x00,             /*   Unit (none) */
	0x81, 0x03,             /*   Input (Constant): 4 bits padding */
	0x09, 0x01,             /*   Usage (Pointer) */
	0xA1, 0x00,             /*   Collection (Physical) */
	0x09, 0x30,             /*     Usage (X) */
	0x09, 0x31,             /*     Usage (Y) */
	0x09, 0x33,             /*     Usage (Rx) */
	0x09, 0x34,             /*     Usage (Ry) */
	0x16, 0x00, 0x80,       /*     Logical Minimum (-32768) */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
	0x36, 0x00, 0x80,       /*     Physical Minimum (-32768) */
	0x46, 0xFF, 0x7F,       /*     Physical Maximum (32767) */
	0x75, 0x10,             /*     Report Size (16) */
	0x95, 0x04,             /*     Report Count (4) */
	0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
	0xC0,                   /*   End Collection */
	0x09, 0x32,             /*   Usage (Z) */
	0x09, 0x35,             /*   Usage (Rz) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
	0x35, 0x00,             /*   Physical Minimum (0) */
	0x46, 0xFF, 0x00,       /*   Physical Maximum (255) */
	0x75, 0x08,             /*   Report Size (8) */
	0x95, 0x02,             /*   Report Count (2) */
	0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
	0xC0,                   /* End Collection */
};
const unsigned x360_report_descriptor_size = sizeof(x360_report_descriptor);

static int s16(const unsigned char *p)
{
	int v = p[0] | (p[1] << 8);
	return v >= 0x8000 ? v - 0x10000 : v;
}

static void put16(unsigned char *p, int v)
{
	p[0] = (unsigned char) (v & 0xFF);
	p[1] = (unsigned char) ((v >> 8) & 0xFF);
}

/* pkt[2]: 0 up, 1 down, 2 left, 3 right, 4 start, 5 back, 6 LS, 7 RS
 * pkt[3]: 0 LB, 1 RB, 2 guide, 4 A, 5 B, 6 X, 7 Y */
int x360_translate(const unsigned char *pkt, unsigned len, unsigned char report[X360_REPORT_SIZE])
{
	unsigned b = 0, hat;
	int up, down, left, right, y;
	if (len < 14 || pkt[0] != 0x00 || pkt[1] < 0x14)
		return 0;
	if (pkt[3] & 0x10) b |= 1u << 0;    /* A */
	if (pkt[3] & 0x20) b |= 1u << 1;    /* B */
	if (pkt[3] & 0x40) b |= 1u << 2;    /* X */
	if (pkt[3] & 0x80) b |= 1u << 3;    /* Y */
	if (pkt[3] & 0x01) b |= 1u << 4;    /* LB */
	if (pkt[3] & 0x02) b |= 1u << 5;    /* RB */
	if (pkt[2] & 0x20) b |= 1u << 6;    /* Back */
	if (pkt[2] & 0x10) b |= 1u << 7;    /* Start */
	if (pkt[2] & 0x40) b |= 1u << 8;    /* LS */
	if (pkt[2] & 0x80) b |= 1u << 9;    /* RS */
	if (pkt[3] & 0x04) b |= 1u << 10;   /* Guide */
	report[0] = (unsigned char) (b & 0xFF);
	report[1] = (unsigned char) (b >> 8);

	up = pkt[2] & 1; down = (pkt[2] >> 1) & 1; left = (pkt[2] >> 2) & 1; right = (pkt[2] >> 3) & 1;
	if (up && down) up = down = 0;          /* impossible combinations: treat as released */
	if (left && right) left = right = 0;
	if (up && right) hat = 1;
	else if (right && down) hat = 3;
	else if (down && left) hat = 5;
	else if (left && up) hat = 7;
	else if (up) hat = 0;
	else if (right) hat = 2;
	else if (down) hat = 4;
	else if (left) hat = 6;
	else hat = 8;
	report[2] = (unsigned char) hat;

	put16(report + 3, s16(pkt + 6));            /* X */
	y = s16(pkt + 8);                            /* controller: up = positive */
	put16(report + 5, y == -32768 ? 32767 : -y);
	put16(report + 7, s16(pkt + 10));           /* Rx */
	y = s16(pkt + 12);
	put16(report + 9, y == -32768 ? 32767 : -y);
	report[11] = pkt[4];                         /* LT */
	report[12] = pkt[5];                         /* RT */
	return 1;
}

void x360_led_packet(int player, unsigned char out[3])
{
	if (player < 1 || player > 4)
		player = 1;
	out[0] = 0x01;
	out[1] = 0x03;
	out[2] = (unsigned char) (0x05 + player);
}

void x360_rumble_packet(unsigned char strong, unsigned char weak, unsigned char out[8])
{
	out[0] = 0x00;
	out[1] = 0x08;
	out[2] = 0x00;
	out[3] = strong;
	out[4] = weak;
	out[5] = out[6] = out[7] = 0x00;
}
