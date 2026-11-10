/* Host tests for the Xbox 360 packet translation (input/kexpad/x360.c). */
#include "../kexpad/x360.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* minimal HID report descriptor walk: total input bits must equal the report size */
static unsigned input_bits(const unsigned char *d, unsigned n)
{
	unsigned i = 0, size = 0, count = 0, bits = 0;
	while (i < n)
	{
		unsigned char pfx = d[i];
		unsigned len = pfx & 3, v = 0, k;
		if (len == 3) len = 4;
		for (k = 0; k < len; k++) v |= (unsigned) d[i + 1 + k] << (8 * k);
		switch (pfx & 0xFC)
		{
		case 0x74: size = v; break;          /* Report Size */
		case 0x94: count = v; break;         /* Report Count */
		case 0x80: bits += size * count; break; /* Input */
		}
		i += 1 + len;
	}
	return bits;
}

int main(void)
{
	unsigned char pkt[20], r[X360_REPORT_SIZE], led[3], rum[8];
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x00; pkt[1] = 0x14;

	CHECK(input_bits(x360_report_descriptor, x360_report_descriptor_size) == X360_REPORT_SIZE * 8);

	/* idle */
	CHECK(x360_translate(pkt, 20, r) == 1);
	CHECK(r[0] == 0 && r[1] == 0 && r[2] == 8);

	/* A + guide, dpad up+right, left stick full up, right stick full left, triggers */
	pkt[3] = 0x10 | 0x04;
	pkt[2] = 0x01 | 0x08;
	pkt[8] = 0xFF; pkt[9] = 0x7F;            /* LY = 32767 (up) */
	pkt[10] = 0x00; pkt[11] = 0x80;          /* RX = -32768 */
	pkt[4] = 200; pkt[5] = 17;
	CHECK(x360_translate(pkt, 20, r) == 1);
	CHECK(r[0] == 0x01 && r[1] == 0x04);     /* button 1 and button 11 */
	CHECK(r[2] == 1);                         /* up-right */
	CHECK(r[5] == 0x01 && r[6] == 0x80);     /* Y = -32767 (HID: up is negative) */
	CHECK(r[7] == 0x00 && r[8] == 0x80);     /* Rx = -32768 */
	CHECK(r[11] == 200 && r[12] == 17);

	/* LY = -32768 (fully down) maps to +32767, not overflow */
	pkt[8] = 0x00; pkt[9] = 0x80;
	x360_translate(pkt, 20, r);
	CHECK(r[5] == 0xFF && r[6] == 0x7F);

	/* other packet types and short packets are not input */
	pkt[0] = 0x01; pkt[1] = 0x03;
	CHECK(x360_translate(pkt, 3, r) == 0);
	pkt[0] = 0x00; pkt[1] = 0x14;
	CHECK(x360_translate(pkt, 10, r) == 0);

	x360_led_packet(1, led);
	CHECK(led[0] == 1 && led[1] == 3 && led[2] == 6);
	x360_rumble_packet(255, 10, rum);
	CHECK(rum[1] == 8 && rum[3] == 255 && rum[4] == 10);

	printf(fails ? "x360: %d FAILED\n" : "x360: all tests passed\n", fails);
	return fails != 0;
}
