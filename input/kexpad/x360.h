/*
 *  KernelExEx - Xbox 360 controller protocol -> HID game pad report.
 *  Packet layout as documented by the Linux xpad driver (GPLv2).
 *  No Windows dependencies; tested on the host (input/tests).
 */
#ifndef KEX_X360_H
#define KEX_X360_H

/* HID input report (no report ID), 13 bytes:
 *   0-1  buttons 1..11: A B X Y LB RB Back Start LS RS Guide
 *   2    hat switch 0..7 (0 = up, clockwise), 8 = centred (low nibble)
 *   3-10 X, Y, Rx, Ry: signed 16-bit, Y down = positive (HID convention)
 *   11   Z  = left trigger 0..255
 *   12   Rz = right trigger 0..255
 */
#define X360_REPORT_SIZE 13

extern const unsigned char x360_report_descriptor[];
extern const unsigned x360_report_descriptor_size;

/* returns 1 and fills 'report' when 'pkt' is an input packet */
int x360_translate(const unsigned char *pkt, unsigned len, unsigned char report[X360_REPORT_SIZE]);

/* LED command for player 1..4 (xpad: 0x06..0x09 = "N on") */
void x360_led_packet(int player, unsigned char out[3]);

/* rumble packet; strong/weak 0..255 */
void x360_rumble_packet(unsigned char strong, unsigned char weak, unsigned char out[8]);

#endif
