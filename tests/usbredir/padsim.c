/*
 * padsim: a simulated Xbox 360-protocol USB controller for QEMU (test tool).
 *
 * Speaks the usbredir protocol (libusbredirparser, LGPL) to a QEMU usb-redir
 * device, so the Windows 98 test VM sees a real USB game pad being plugged
 * in: the whole path from the New Hardware wizard through kexpad.sys to the
 * HID game controller can be tested without hardware.
 *
 *   qemu ... -chardev socket,id=pad,path=pad.sock,server=on,wait=off \
 *            -device usb-redir,chardev=pad,id=pad0
 *   padsim pad.sock [VID:PID] [product string]
 * or, hot-plugged into a running VM (HMP):
 *   padsim listen:pad.sock ... &
 *   chardev-add socket,id=pad,path=pad.sock
 *   device_add usb-redir,chardev=pad,id=pad0
 *
 * Connecting plugs the pad in, quitting (or SIGTERM) unplugs it. Reports:
 * button A pressed every other second, the left stick going round.
 * Lines on stdin change that: "a 1", "a 0", "x 12000", "y -5000", "quit".
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <usbredirparser.h>

static int fd = -1;
static struct usbredirparser *parser;
static unsigned short vid = 0x045e, pid = 0x028e;
static const char *product = "Controller";
static int configured, in_started;
static int manual;               /* stdin took over the report */
static int btn_a;
static short lx, ly;
static unsigned long out_count;

static void logf_(const char *fmt, ...)
{
	va_list ap;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	fprintf(stderr, "[%ld.%03ld] ", (long) tv.tv_sec % 1000, (long) tv.tv_usec / 1000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ---- the descriptors of a wired Xbox 360 controller -------------------------------- */
static unsigned char dev_desc[18] = {
	0x12, 0x01, 0x00, 0x02, 0xff, 0xff, 0xff, 0x08, 0x5e, 0x04, 0x8e, 0x02, 0x14, 0x01, 0x01, 0x02, 0x03, 0x01 };

static const unsigned char cfg_desc[] = {
	0x09, 0x02, 0x99, 0x00, 0x04, 0x01, 0x00, 0xa0, 0xfa,
	/* interface 0: the pad (ff/5d/01) */
	0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x5d, 0x01, 0x00,
	0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,
	0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
	0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,
	/* interface 1: headset (ff/5d/03) */
	0x09, 0x04, 0x01, 0x00, 0x04, 0xff, 0x5d, 0x03, 0x00,
	0x1b, 0x21, 0x00, 0x01, 0x01, 0x01, 0x82, 0x40, 0x01, 0x02, 0x20, 0x16, 0x83, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x16, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x02,
	0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04,
	0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x40,
	0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x10,
	/* interface 2: plug-in module (ff/5d/02) */
	0x09, 0x04, 0x02, 0x00, 0x01, 0xff, 0x5d, 0x02, 0x00,
	0x09, 0x21, 0x00, 0x01, 0x01, 0x22, 0x84, 0x07, 0x00,
	0x07, 0x05, 0x84, 0x03, 0x20, 0x00, 0x10,
	/* interface 3: security (ff/fd/13) */
	0x09, 0x04, 0x03, 0x00, 0x00, 0xff, 0xfd, 0x13, 0x04,
	0x06, 0x41, 0x00, 0x01, 0x01, 0x03,
};

static int string_desc(int idx, unsigned char *out)
{
	const char *s;
	int n, i;
	if (idx == 0)
	{
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	s = idx == 1 ? (vid == 0x045e ? "Microsoft" : "8BitDo") : idx == 2 ? product : idx == 3 ? "0A1B2C3D" : NULL;
	if (!s)
		return -1;
	n = (int) strlen(s);
	out[0] = (unsigned char) (2 + 2 * n);
	out[1] = 3;
	for (i = 0; i < n; i++)
	{
		out[2 + 2 * i] = (unsigned char) s[i];
		out[3 + 2 * i] = 0;
	}
	return 2 + 2 * n;
}

/* ---- usbredir plumbing --------------------------------------------------------------- */
static int do_read(void *priv, uint8_t *data, int count)
{
	int r = (int) read(fd, data, count);
	if (r == 0)
	{
		logf_("QEMU closed the connection\n");
		exit(0);
	}
	if (r < 0)
		return errno == EAGAIN ? 0 : -1;
	return r;
}

static int do_write(void *priv, uint8_t *data, int count)
{
	int r = (int) write(fd, data, count);
	if (r < 0)
		return errno == EAGAIN ? 0 : -1;
	return r;
}

static void p_log(void *priv, int level, const char *msg)
{
	if (level <= 2)
		logf_("usbredir: %s\n", msg);
}

static void flush(void)
{
	while (usbredirparser_has_data_to_write(parser))
		if (usbredirparser_do_write(parser) < 0)
			break;
}

#define EP2I(ep) ((((ep) & 0x80) >> 3) | ((ep) & 0x0f))

static void p_hello(void *priv, struct usb_redir_hello_header *h)
{
	struct usb_redir_interface_info_header ii;
	struct usb_redir_ep_info_header ei;
	struct usb_redir_device_connect_header dc;
	const unsigned char *p = cfg_desc, *end = cfg_desc + sizeof(cfg_desc);
	int cur_if = 0;

	logf_("hello from \"%s\"\n", h->version);
	memset(&ii, 0, sizeof(ii));
	memset(&ei, 0, sizeof(ei));
	memset(ei.type, usb_redir_type_invalid, sizeof(ei.type));
	ei.type[0] = ei.type[16] = usb_redir_type_control;
	ei.max_packet_size[0] = ei.max_packet_size[16] = 8;
	for (; p < end; p += p[0])
	{
		if (p[1] == 4)
		{
			cur_if = p[2];
			ii.interface[ii.interface_count] = p[2];
			ii.interface_class[ii.interface_count] = p[5];
			ii.interface_subclass[ii.interface_count] = p[6];
			ii.interface_protocol[ii.interface_count] = p[7];
			ii.interface_count++;
		}
		else if (p[1] == 5)
		{
			int i = EP2I(p[2]);
			ei.type[i] = p[3] & 3;
			ei.interval[i] = p[6];
			ei.interface[i] = (uint8_t) cur_if;
			ei.max_packet_size[i] = (uint16_t) (p[4] | (p[5] << 8));
		}
	}
	memset(&dc, 0, sizeof(dc));
	dc.speed = usb_redir_speed_full;
	dc.device_class = 0xff;
	dc.device_subclass = 0xff;
	dc.device_protocol = 0xff;
	dc.vendor_id = vid;
	dc.product_id = pid;
	dc.device_version_bcd = 0x0114;
	usbredirparser_send_interface_info(parser, &ii);
	usbredirparser_send_ep_info(parser, &ei);
	usbredirparser_send_device_connect(parser, &dc);
	flush();
	logf_("plugged in as %04x:%04x \"%s\"\n", vid, pid, product);
}

static void p_reset(void *priv)
{
	logf_("reset\n");
	configured = 0;
	in_started = 0;
}

static void p_set_configuration(void *priv, uint64_t id, struct usb_redir_set_configuration_header *h)
{
	struct usb_redir_configuration_status_header st = { usb_redir_success, h->configuration };
	logf_("set configuration %u\n", h->configuration);
	configured = h->configuration;
	usbredirparser_send_configuration_status(parser, id, &st);
	flush();
}

static void p_get_configuration(void *priv, uint64_t id)
{
	struct usb_redir_configuration_status_header st = { usb_redir_success, (uint8_t) configured };
	usbredirparser_send_configuration_status(parser, id, &st);
	flush();
}

static void p_set_alt(void *priv, uint64_t id, struct usb_redir_set_alt_setting_header *h)
{
	struct usb_redir_alt_setting_status_header st = { usb_redir_success, h->interface, h->alt };
	logf_("set interface %u alt %u\n", h->interface, h->alt);
	usbredirparser_send_alt_setting_status(parser, id, &st);
	flush();
}

static void p_get_alt(void *priv, uint64_t id, struct usb_redir_get_alt_setting_header *h)
{
	struct usb_redir_alt_setting_status_header st = { usb_redir_success, h->interface, 0 };
	usbredirparser_send_alt_setting_status(parser, id, &st);
	flush();
}

static void send_report(void);

static void p_start_int(void *priv, uint64_t id, struct usb_redir_start_interrupt_receiving_header *h)
{
	struct usb_redir_interrupt_receiving_status_header st = { usb_redir_success, h->endpoint };
	logf_("start interrupt receiving on %02x\n", h->endpoint);
	usbredirparser_send_interrupt_receiving_status(parser, id, &st);
	flush();
	if (h->endpoint == 0x81)
	{
		/* what a real pad says first: LED state, headset, then a report */
		static uint8_t led[3] = { 0x01, 0x03, 0x0e }, hs[3] = { 0x03, 0x03, 0x03 }, att[3] = { 0x08, 0x03, 0x00 };
		struct usb_redir_interrupt_packet_header ip = { 0x81, usb_redir_success, 3 };
		usbredirparser_send_interrupt_packet(parser, 0, &ip, led, 3);
		usbredirparser_send_interrupt_packet(parser, 0, &ip, hs, 3);
		usbredirparser_send_interrupt_packet(parser, 0, &ip, att, 3);
		flush();
		in_started = 1;
		send_report();
	}
}

static void p_stop_int(void *priv, uint64_t id, struct usb_redir_stop_interrupt_receiving_header *h)
{
	struct usb_redir_interrupt_receiving_status_header st = { usb_redir_success, h->endpoint };
	logf_("stop interrupt receiving on %02x\n", h->endpoint);
	if (h->endpoint == 0x81)
		in_started = 0;
	usbredirparser_send_interrupt_receiving_status(parser, id, &st);
	flush();
}

static void p_cancel(void *priv, uint64_t id)
{
	logf_("cancel %llu\n", (unsigned long long) id);
}

static void p_control(void *priv, uint64_t id, struct usb_redir_control_packet_header *h, uint8_t *data, int len)
{
	unsigned char buf[512];
	int n = 0;
	struct usb_redir_control_packet_header r = *h;
	r.status = usb_redir_success;

	if (h->requesttype == 0x80 && h->request == 6)   /* GET_DESCRIPTOR */
	{
		int type = h->value >> 8, idx = h->value & 0xff;
		if (type == 1)
		{
			memcpy(buf, dev_desc, sizeof(dev_desc));
			n = sizeof(dev_desc);
		}
		else if (type == 2)
		{
			memcpy(buf, cfg_desc, sizeof(cfg_desc));
			n = sizeof(cfg_desc);
		}
		else if (type == 3)
			n = string_desc(idx, buf);
		else
			n = -1;
		logf_("get descriptor %02x/%u len %u -> %d\n", type, idx, h->length, n);
	}
	else if (h->requesttype == 0x80 && h->request == 0)   /* GET_STATUS */
	{
		buf[0] = 0; buf[1] = 0;
		n = 2;
	}
	else if (h->requesttype == 0x80 && h->request == 8)   /* GET_CONFIGURATION */
	{
		buf[0] = (unsigned char) configured;
		n = 1;
	}
	else if (h->requesttype == 0xc1 && h->request == 1)   /* the pad's serial/capabilities query */
	{
		memset(buf, 0, 20);
		n = h->length < 20 ? h->length : 20;
	}
	else if (!(h->requesttype & 0x80))
	{
		logf_("control out %02x/%02x value %04x index %04x len %d\n", h->requesttype, h->request, h->value, h->index, len);
		n = 0;
	}
	else
	{
		logf_("control in %02x/%02x value %04x index %04x: stall\n", h->requesttype, h->request, h->value, h->index);
		n = -1;
	}
	if (n < 0)
	{
		r.status = usb_redir_stall;
		r.length = 0;
		usbredirparser_send_control_packet(parser, id, &r, NULL, 0);
	}
	else if (h->requesttype & 0x80)
	{
		if (n > h->length)
			n = h->length;
		r.length = (uint16_t) n;
		usbredirparser_send_control_packet(parser, id, &r, buf, n);
	}
	else
		usbredirparser_send_control_packet(parser, id, &r, NULL, 0);
	flush();
	usbredirparser_free_packet_data(parser, data);
}

static void p_interrupt(void *priv, uint64_t id, struct usb_redir_interrupt_packet_header *h, uint8_t *data, int len)
{
	struct usb_redir_interrupt_packet_header r = *h;
	int i;
	if (!(h->endpoint & 0x80))
	{
		out_count++;
		fprintf(stderr, "[out %02x #%lu]", h->endpoint, out_count);
		for (i = 0; i < len; i++)
			fprintf(stderr, " %02x", data[i]);
		fprintf(stderr, "\n");
		r.status = usb_redir_success;
		usbredirparser_send_interrupt_packet(parser, id, &r, NULL, 0);
		flush();
	}
	usbredirparser_free_packet_data(parser, data);
}

/* ---- reports -------------------------------------------------------------------------- */
static void send_report(void)
{
	uint8_t r[20];
	struct usb_redir_interrupt_packet_header ip = { 0x81, usb_redir_success, 20 };
	unsigned short buttons = btn_a ? 0x1000 : 0;   /* A */
	if (!in_started)
		return;
	memset(r, 0, sizeof(r));
	r[0] = 0x00;
	r[1] = 0x14;
	r[2] = (uint8_t) buttons;
	r[3] = (uint8_t) (buttons >> 8);
	r[6] = (uint8_t) lx; r[7] = (uint8_t) (lx >> 8);
	r[8] = (uint8_t) ly; r[9] = (uint8_t) (ly >> 8);
	usbredirparser_send_interrupt_packet(parser, 0, &ip, r, 20);
	flush();
	logf_("report: A %d, stick %d,%d\n", btn_a, lx, ly);
}

static void tick(void)
{
	static int last_a = -1, last_x = 99999, last_y = 99999;
	struct timeval tv;
	double t;
	gettimeofday(&tv, NULL);
	t = tv.tv_sec % 1000 + tv.tv_usec / 1e6;
	if (!manual)
	{
		btn_a = ((long) t) & 1;
		lx = (short) (20000 * cos(t));
		ly = (short) (20000 * sin(t));
	}
	/* a pad only reports changes; the stick moves in steps so the log stays readable */
	if (btn_a != last_a || abs(lx - last_x) > 2000 || abs(ly - last_y) > 2000)
	{
		last_a = btn_a;
		last_x = lx;
		last_y = ly;
		send_report();
	}
}

static void on_stdin(void)
{
	char line[128];
	int v;
	if (!fgets(line, sizeof(line), stdin))
		return;
	manual = 1;
	if (sscanf(line, "a %d", &v) == 1)
		btn_a = v;
	else if (sscanf(line, "x %d", &v) == 1)
		lx = (short) v;
	else if (sscanf(line, "y %d", &v) == 1)
		ly = (short) v;
	else if (!strncmp(line, "auto", 4))
		manual = 0;
	else if (!strncmp(line, "quit", 4))
		exit(0);
	send_report();
}

static void unplug(int sig)
{
	(void) sig;
	if (parser)
	{
		usbredirparser_send_device_disconnect(parser);
		flush();
	}
	_exit(0);
}

int main(int argc, char **argv)
{
	struct sockaddr_un sa;
	uint32_t caps[USB_REDIR_CAPS_SIZE] = { 0 };
	unsigned v, p;

	if (argc < 2)
	{
		fprintf(stderr, "usage: padsim socket [VID:PID] [product]\n");
		return 2;
	}
	if (argc > 2 && sscanf(argv[2], "%x:%x", &v, &p) == 2)
	{
		vid = (unsigned short) v;
		pid = (unsigned short) p;
		dev_desc[8] = (unsigned char) vid; dev_desc[9] = (unsigned char) (vid >> 8);
		dev_desc[10] = (unsigned char) pid; dev_desc[11] = (unsigned char) (pid >> 8);
	}
	if (argc > 3)
		product = argv[3];

	/* "listen:PATH": wait for QEMU to connect (hot-plugged with chardev-add
	 * and device_add); otherwise connect to QEMU's server socket */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (!strncmp(argv[1], "listen:", 7))
	{
		int ls = fd;
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1] + 7);
		unlink(sa.sun_path);
		if (bind(ls, (struct sockaddr *) &sa, sizeof(sa)) < 0 || listen(ls, 1) < 0)
		{
			perror("listen");
			return 1;
		}
		logf_("waiting for QEMU on %s\n", sa.sun_path);
		fd = accept(ls, NULL, NULL);
		close(ls);
	}
	else
	{
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
		if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0)
		{
			perror("connect");
			return 1;
		}
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);   /* the parser reads until nothing is left */
	signal(SIGTERM, unplug);
	signal(SIGINT, unplug);

	parser = usbredirparser_create();
	parser->priv = NULL;
	parser->log_func = p_log;
	parser->read_func = do_read;
	parser->write_func = do_write;
	parser->hello_func = p_hello;
	parser->reset_func = p_reset;
	parser->set_configuration_func = p_set_configuration;
	parser->get_configuration_func = p_get_configuration;
	parser->set_alt_setting_func = p_set_alt;
	parser->get_alt_setting_func = p_get_alt;
	parser->start_interrupt_receiving_func = p_start_int;
	parser->stop_interrupt_receiving_func = p_stop_int;
	parser->cancel_data_packet_func = p_cancel;
	parser->control_packet_func = p_control;
	parser->interrupt_packet_func = p_interrupt;
	usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
	usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
	usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
	usbredirparser_init(parser, "kexex padsim 1.0", caps, USB_REDIR_CAPS_SIZE, usbredirparser_fl_usb_host);
	flush();

	for (;;)
	{
		struct pollfd pf[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
		int r = poll(pf, 2, 20);
		if (r > 0 && (pf[0].revents & (POLLIN | POLLHUP)))
			if (usbredirparser_do_read(parser) == usbredirparser_read_io_error)
			{
				logf_("read error\n");
				return 1;
			}
		if (r > 0 && (pf[1].revents & POLLIN))
			on_stdin();
		flush();
		tick();
	}
}
