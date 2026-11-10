/* Host tests for the gamepad mapping engine (input/xinput/kxpad.c). */
#include "../xinput/kxpad.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	kxpad_map m, m2;
	kxpad_raw raw;
	kxpad_out o;
	char line[512], guid[33];

	/* GUID layout matches SDL 2 (Xbox 360 pad 045e:028e) */
	kxpad_usb_guid(0x045e, 0x028e, guid);
	CHECK(!strcmp(guid, "030000005e0400008e02000000000000"));

	/* parse, including modifiers, unknown keys and a missing trailing comma */
	CHECK(kxpad_parse("030000005e0400008e02000000000000,X360 Controller,a:b0,b:b1,x:b2,y:b3,"
	                  "back:b6,start:b7,guide:b10,leftshoulder:b4,rightshoulder:b5,leftstick:b8,"
	                  "rightstick:b9,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,"
	                  "rightx:a4,righty:a5~,lefttrigger:+a2,righttrigger:-a2,crc:1234,platform:Windows 9x", &m) == 0);
	CHECK(!strcmp(m.name, "X360 Controller"));
	CHECK(m.bind[KXT_A].type == KXS_BUTTON && m.bind[KXT_A].index == 0);
	CHECK(m.bind[KXT_DPLEFT].type == KXS_HAT && m.bind[KXT_DPLEFT].hatmask == 8);
	CHECK(m.bind[KXT_RIGHTY].invert == 1);
	CHECK(m.bind[KXT_LEFTTRIGGER].half == 1 && m.bind[KXT_RIGHTTRIGGER].half == -1);
	CHECK(kxpad_parse("# comment", &m2) != 0);
	CHECK(kxpad_parse("short,x", &m2) != 0);

	/* format round-trips */
	CHECK(kxpad_format(&m, line, sizeof(line)) > 0);
	CHECK(kxpad_parse(line, &m2) == 0 && !memcmp(m.bind, m2.bind, sizeof(m.bind)));

	/* apply: centred sticks, Y up positive, combined-axis triggers, hat diagonal */
	memset(&raw, 0, sizeof(raw));
	raw.axis[1] = -32768;             /* stick pushed fully up */
	raw.axis[2] = 32767;              /* combined axis fully one way: left trigger */
	raw.axis[5] = 32767;              /* inverted right Y: down on the joystick = up after ~, then flipped */
	raw.buttons = (1u << 0) | (1u << 10);
	raw.hat = 1 | 2;
	kxpad_apply(&m, &raw, &o);
	CHECK(o.ly == 32767);
	CHECK(o.lx == 0);
	CHECK(o.lt == 255 && o.rt == 0);
	CHECK(o.buttons & 0x1000);        /* A */
	CHECK(o.buttons & KXPAD_GUIDE_BIT);
	CHECK((o.buttons & 0x0001) && (o.buttons & 0x0008) && !(o.buttons & 0x0002));
	CHECK(o.ry == 32766);             /* inverted (-32767), then Y flipped */

	/* scaling and POV */
	CHECK(kxpad_scale_axis(0, 0, 65535) == -32768);
	CHECK(kxpad_scale_axis(65535, 0, 65535) == 32767);
	CHECK(kxpad_scale_axis(32768, 0, 65535) == 0 || kxpad_scale_axis(32768, 0, 65535) == 1);
	CHECK(kxpad_pov_to_hat(0xFFFF) == 0);
	CHECK(kxpad_pov_to_hat(0) == 1);
	CHECK(kxpad_pov_to_hat(4500) == 3);
	CHECK(kxpad_pov_to_hat(27000) == 8);
	CHECK(kxpad_pov_to_hat(31500) == 9);

	/* database: later lines win, other platforms skipped */
	{
		const char *db =
			"# test\n"
			"030000005e0400008e02000000000000,Old,a:b1,platform:Windows 9x,\n"
			"030000005e0400008e02000000000000,Linux one,a:b5,platform:Linux,\n"
			"030000005e0400008e02000000000000,New,a:b2,platform:Windows 9x,\n";
		CHECK(kxpad_db_find(db, strlen(db), guid, &m2) == 1);
		CHECK(!strcmp(m2.name, "New") && m2.bind[KXT_A].index == 2);
		CHECK(kxpad_db_find(db, strlen(db), "03000000000000000000000000000000", &m2) == 0);
	}

	/* default mapping is usable */
	kxpad_default(&m2, guid, "Generic", 6);
	CHECK(m2.bind[KXT_LEFTX].type == KXS_AXIS && m2.bind[KXT_LEFTTRIGGER].index == 4);
	kxpad_default(&m2, guid, "Generic", 4);
	CHECK(m2.bind[KXT_LEFTTRIGGER].type == KXS_BUTTON && m2.bind[KXT_START].index == 9);

	printf(fails ? "kxpad: %d FAILED\n" : "kxpad: all tests passed\n", fails);
	return fails != 0;
}
