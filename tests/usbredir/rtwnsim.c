/*
 * rtwnsim: a simulated Realtek RTL8821AU USB Wi-Fi stick (the TP-Link Archer
 * T2U Plus, 2357:0120) for QEMU (test tool), at USB 2.0 high speed or USB
 * 1.1 full speed. It has the real stick's endpoints and a register file
 * behind Realtek's vendor request 0x05 (with the eFuse read protocol and a
 * firmware download that reports success), so kexrtwn.sys's attach and
 * start run against it through Windows' USB stack or kexehci. No radio:
 * nothing comes back on the bulk-in endpoint. The log shows every register
 * access summary and what the driver sent.
 *
 *   rtwnsim listen:PATH [high|full] [VID:PID]
 *   (QEMU monitor) chardev-add socket,id=wifi,path=PATH
 *                  device_add usb-redir,chardev=wifi,id=wifi0[,bus=ehci.0]
 *
 * Uses libusbredirparser (LGPL); a test tool, never shipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <usbredirparser.h>

static int fd = -1;
static struct usbredirparser *parser;
static unsigned short vid = 0x2357, pid = 0x0120;
static int high_speed = 1;
static int configured;
static unsigned char regs[0x10000];
static unsigned char efuse[512];
static unsigned long n_rd, n_wr, n_bulk_out, bulk_out_bytes;
static int verbose;

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

/* ---- descriptors: one interface, ff/ff/ff, like the T2U Plus ----------------------- */
static unsigned char dev_desc[18] = {
	0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x57, 0x23, 0x20, 0x01, 0x00, 0x02, 0x01, 0x02, 0x03, 0x01 };

static unsigned char cfg_desc[] = {
	0x09, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x80, 0xfa,
	0x09, 0x04, 0x00, 0x00, 0x06, 0xff, 0xff, 0xff, 0x02,
	0x07, 0x05, 0x84, 0x02, 0x00, 0x02, 0x00,   /* bulk in */
	0x07, 0x05, 0x05, 0x02, 0x00, 0x02, 0x00,   /* bulk out */
	0x07, 0x05, 0x06, 0x02, 0x00, 0x02, 0x00,
	0x07, 0x05, 0x87, 0x03, 0x40, 0x00, 0x01,   /* interrupt in */
	0x07, 0x05, 0x08, 0x02, 0x00, 0x02, 0x00,
	0x07, 0x05, 0x09, 0x02, 0x00, 0x02, 0x00,
};

static int string_desc(int idx, unsigned char *out)
{
	const char *s = idx == 1 ? "Realtek" : idx == 2 ? "802.11ac WLAN Adapter" : idx == 3 ? "00e04c000001" : NULL;
	int n, i;
	if (idx == 0)
	{
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
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

/* ---- the chip -------------------------------------------------------------------------- */
static unsigned rd32(unsigned a) { return regs[a] | regs[a + 1] << 8 | regs[a + 2] << 16 | (unsigned) regs[a + 3] << 24; }
static void wr32(unsigned a, unsigned v) { regs[a] = v; regs[a + 1] = v >> 8; regs[a + 2] = v >> 16; regs[a + 3] = v >> 24; }

static void chip_reset(void)
{
	memset(regs, 0, sizeof(regs));
	memset(efuse, 0xff, sizeof(efuse));   /* an empty eFuse: the driver uses its defaults */
	wr32(0xf0, 0x00000000);   /* SYS_CFG: not a test chip, cut A */
	regs[0xfc] = 0x00;
	/* power: the chip reports its blocks ready, so the power-on sequence's polls end */
	regs[0x04] = 0x02;
	regs[0x06] = 0x02;   /* APS_FSMCO SUS_HOST */
	regs[0x41a] = regs[0x41b] = 0xff;   /* TXPKT_EMPTY: every queue empty */
}

/* side effects of a write to [a, a+n) */
static void chip_written(unsigned a, unsigned n)
{
	unsigned v;
	if (a <= 0x33 && a + n > 0x30)   /* EFUSE_CTRL: a read request completes at once */
	{
		v = rd32(0x30);
		if (!(v & 0x80000000))
		{
			unsigned addr = (v >> 8) & 0x3ff;
			v = (v & 0xffffff00) | (addr < sizeof(efuse) ? efuse[addr] : 0xff) | 0x80000000;
			wr32(0x30, v);
		}
		else
			wr32(0x30, v & ~0x80000000u);   /* a write request completes too */
	}
	if (a <= 0x83 && a + n > 0x80)   /* MCUFWDL: checksum and firmware ready once download is off */
	{
		v = rd32(0x80);
		if (!(v & 0x1))
			v |= 0x4 | 0x2 | 0x40;
		wr32(0x80, v);
	}
	if (a <= 0x07 && a + n > 0x04)   /* APS_FSMCO: power-on / power-off requests are done at once */
	{
		regs[0x05] &= ~(0x01 | 0x02);
		regs[0x06] |= 0x02;        /* SUS_HOST: the host side is powered */
	}
	if (a <= 0x1e3 && a + n > 0x1e0)   /* LLT_INIT: the table write is done at once */
		regs[0x1e3] &= 0x3f;
}

/* ---- usbredir plumbing ----------------------------------------------------------------- */
static int do_read(void *priv, uint8_t *data, int count)
{
	int r = (int) read(fd, data, count);
	(void) priv;
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
	(void) priv;
	if (r < 0)
		return errno == EAGAIN ? 0 : -1;
	return r;
}

static void p_log(void *priv, int level, const char *msg)
{
	(void) priv;
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
	unsigned char *p = cfg_desc, *end = cfg_desc + sizeof(cfg_desc);
	(void) priv;
	logf_("hello from \"%s\"\n", h->version);
	memset(&ii, 0, sizeof(ii));
	memset(&ei, 0, sizeof(ei));
	memset(ei.type, usb_redir_type_invalid, sizeof(ei.type));
	ei.type[0] = ei.type[16] = usb_redir_type_control;
	ei.max_packet_size[0] = ei.max_packet_size[16] = 64;
	for (; p < end; p += p[0])
	{
		if (p[1] == 4)
		{
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
			ei.interface[i] = 0;
			ei.max_packet_size[i] = (uint16_t) (p[4] | (p[5] << 8));
		}
	}
	memset(&dc, 0, sizeof(dc));
	dc.speed = high_speed ? usb_redir_speed_high : usb_redir_speed_full;
	dc.vendor_id = vid;
	dc.product_id = pid;
	dc.device_version_bcd = 0x0200;
	usbredirparser_send_interface_info(parser, &ii);
	usbredirparser_send_ep_info(parser, &ei);
	usbredirparser_send_device_connect(parser, &dc);
	flush();
	logf_("plugged in as %04x:%04x, %s speed\n", vid, pid, high_speed ? "high" : "full");
}

static void p_reset(void *priv)
{
	(void) priv;
	logf_("reset\n");
	configured = 0;
}

static void p_set_configuration(void *priv, uint64_t id, struct usb_redir_set_configuration_header *h)
{
	struct usb_redir_configuration_status_header st = { usb_redir_success, h->configuration };
	(void) priv;
	logf_("set configuration %u\n", h->configuration);
	configured = h->configuration;
	usbredirparser_send_configuration_status(parser, id, &st);
	flush();
}

static void p_get_configuration(void *priv, uint64_t id)
{
	struct usb_redir_configuration_status_header st = { usb_redir_success, (uint8_t) configured };
	(void) priv;
	usbredirparser_send_configuration_status(parser, id, &st);
	flush();
}

static void p_set_alt(void *priv, uint64_t id, struct usb_redir_set_alt_setting_header *h)
{
	struct usb_redir_alt_setting_status_header st = { usb_redir_success, h->interface, h->alt };
	(void) priv;
	usbredirparser_send_alt_setting_status(parser, id, &st);
	flush();
}

static void p_get_alt(void *priv, uint64_t id, struct usb_redir_get_alt_setting_header *h)
{
	struct usb_redir_alt_setting_status_header st = { usb_redir_success, h->interface, 0 };
	(void) priv;
	usbredirparser_send_alt_setting_status(parser, id, &st);
	flush();
}

static void p_start_int(void *priv, uint64_t id, struct usb_redir_start_interrupt_receiving_header *h)
{
	struct usb_redir_interrupt_receiving_status_header st = { usb_redir_success, h->endpoint };
	(void) priv;
	logf_("start interrupt receiving on %02x\n", h->endpoint);
	usbredirparser_send_interrupt_receiving_status(parser, id, &st);
	flush();
}

static void p_stop_int(void *priv, uint64_t id, struct usb_redir_stop_interrupt_receiving_header *h)
{
	struct usb_redir_interrupt_receiving_status_header st = { usb_redir_success, h->endpoint };
	(void) priv;
	usbredirparser_send_interrupt_receiving_status(parser, id, &st);
	flush();
}

/* bulk-in requests stay pending (no radio) until cancelled */
#define MAXPEND 64
static struct { uint64_t id; struct usb_redir_bulk_packet_header h; int used; } pend[MAXPEND];

static void p_cancel(void *priv, uint64_t id)
{
	int i;
	(void) priv;
	for (i = 0; i < MAXPEND; i++)
		if (pend[i].used && pend[i].id == id)
		{
			pend[i].used = 0;
			pend[i].h.status = usb_redir_cancelled;
			pend[i].h.length = 0;
			usbredirparser_send_bulk_packet(parser, id, &pend[i].h, NULL, 0);
			flush();
			return;
		}
}

static void p_control(void *priv, uint64_t id, struct usb_redir_control_packet_header *h, uint8_t *data, int len)
{
	unsigned char buf[512];
	int n = 0;
	struct usb_redir_control_packet_header r = *h;
	(void) priv;
	r.status = usb_redir_success;
	if (h->requesttype == 0x80 && h->request == 6)
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
		else if (type == 6)   /* device qualifier: we are a 2.0 device */
		{
			unsigned char q[10] = { 10, 6, 0x00, 0x02, 0, 0, 0, 0x40, 1, 0 };
			memcpy(buf, q, 10);
			n = 10;
		}
		else
			n = -1;
		logf_("get descriptor %02x/%u len %u -> %d\n", type, idx, h->length, n);
	}
	else if (h->requesttype == 0x80 && h->request == 0)
	{
		buf[0] = 0; buf[1] = 0;
		n = 2;
	}
	else if (h->requesttype == 0xc0 && h->request == 0x05)   /* register read */
	{
		unsigned a = h->value, i;
		n = h->length;
		for (i = 0; i < (unsigned) n && a + i < sizeof(regs); i++)
			buf[i] = regs[a + i];
		n_rd++;
		if (verbose)
			logf_("rd %04x/%d = %08x\n", a, n, n >= 4 ? rd32(a) : n == 2 ? (regs[a] | regs[a + 1] << 8) : regs[a]);
	}
	else if (h->requesttype == 0x40 && h->request == 0x05)   /* register write */
	{
		unsigned a = h->value, i;
		for (i = 0; i < (unsigned) len && a + i < sizeof(regs); i++)
			regs[a + i] = data[i];
		n_wr++;
		if (verbose && (a < 0x1000 || a >= 0x2000))
			logf_("wr %04x/%d\n", a, len);
		chip_written(a, (unsigned) len);
		n = 0;
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

static void p_bulk(void *priv, uint64_t id, struct usb_redir_bulk_packet_header *h, uint8_t *data, int len)
{
	struct usb_redir_bulk_packet_header r = *h;
	int i;
	(void) priv;
	if (h->endpoint & 0x80)
	{
		for (i = 0; i < MAXPEND; i++)
			if (!pend[i].used)
			{
				pend[i].used = 1;
				pend[i].id = id;
				pend[i].h = *h;
				break;
			}
		if (i == MAXPEND)
			logf_("too many pending bulk-in requests\n");
	}
	else
	{
		n_bulk_out++;
		bulk_out_bytes += (unsigned long) len;
		if (verbose)
			logf_("bulk out %02x len %d\n", h->endpoint, len);
		r.status = usb_redir_success;
		usbredirparser_send_bulk_packet(parser, id, &r, NULL, 0);
		flush();
	}
	usbredirparser_free_packet_data(parser, data);
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
	int ls;
	time_t last = 0;
	int i;

	if (argc < 2 || strncmp(argv[1], "listen:", 7))
	{
		fprintf(stderr, "usage: rtwnsim listen:PATH [high|full] [VID:PID] [-v]\n");
		return 2;
	}
	for (i = 2; i < argc; i++)
	{
		if (!strcmp(argv[i], "full"))
			high_speed = 0;
		else if (!strcmp(argv[i], "high"))
			high_speed = 1;
		else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (sscanf(argv[i], "%x:%x", &v, &p) == 2)
		{
			vid = (unsigned short) v;
			pid = (unsigned short) p;
		}
	}
	dev_desc[8] = (unsigned char) vid; dev_desc[9] = (unsigned char) (vid >> 8);
	dev_desc[10] = (unsigned char) pid; dev_desc[11] = (unsigned char) (pid >> 8);
	cfg_desc[2] = sizeof(cfg_desc);
	if (!high_speed)   /* full speed: 64-byte bulk packets, a 1.1 device */
	{
		unsigned char *q = cfg_desc;
		dev_desc[2] = 0x10; dev_desc[3] = 0x01;
		for (; q < cfg_desc + sizeof(cfg_desc); q += q[0])
			if (q[1] == 5 && (q[3] & 3) == 2)
			{
				q[4] = 0x40;
				q[5] = 0x00;
			}
	}
	chip_reset();

	ls = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
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
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	signal(SIGTERM, unplug);
	signal(SIGINT, unplug);

	parser = usbredirparser_create();
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
	parser->bulk_packet_func = p_bulk;
	usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
	usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
	usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
	usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
	usbredirparser_init(parser, "kexex rtwnsim 1.0", caps, USB_REDIR_CAPS_SIZE, usbredirparser_fl_usb_host);
	flush();
	for (;;)
	{
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, 100) > 0)
			if (usbredirparser_do_read(parser) == usbredirparser_read_io_error)
			{
				logf_("read error\n");
				return 1;
			}
		flush();
		if (time(NULL) - last >= 5)
		{
			last = time(NULL);
			logf_("so far: %lu register reads, %lu writes, %lu bulk-out packets (%lu bytes), MCUFWDL %08x\n", n_rd, n_wr,
			      n_bulk_out, bulk_out_bytes, rd32(0x80));
		}
	}
}
