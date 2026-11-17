/*
 *  KernelExEx - KEXTOUCH.VXD: PS/2 touchpad / TrackPoint / wheel mouse driver for
 *  Windows 98 and ME, a VMOUSE mouse minidriver (replaces MSMOUSE.VXD through the
 *  mouse devnode's MouseDriver value). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  - talks to the device through the keyboard controller's aux port (D4h prefix),
 *    taking only bytes the controller marks as aux data (status bit 5), so the
 *    keyboard driver (VKD) keeps its own;
 *  - IRQ 12 through VPICD; packets go to the portable core (tp_core.c, tp_alps.c);
 *  - pointer motion and buttons: VMD_Post_Pointer_Message;
 *  - wheel and scrolling: queued for kextouch.exe, which turns them into
 *    mouse_event(MOUSEEVENTF_WHEEL / HWHEEL) (Windows 98 VMOUSE has no documented
 *    wheel path for minidrivers);
 *  - settings come from kextouch.exe (IOCTL), diagnostics go to a KEDCL log ring.
 */
#include "../../kedcl/kexos.h"
#include "tp.h"
#include "kextouch.h"

static struct kexos_log klog = { { 0 }, 0, "kextouch" };
static struct tp T;
static kx_u32 hirq;
static int polling = 1, running;
static kx_u32 *mouse_instance;
static int pend_wheel, pend_hwheel;
static kx_u32 n_bytes, n_packets, n_posts, n_errors;

void tp_log(struct tp *t, const char *fmt, ...)
{
	va_list ap;
	(void) t;
	va_start(ap, fmt);
	kexos_vlog(&klog, fmt, ap);
	va_end(ap);
}

/* ---- 8042 aux port (polled; IRQ 12 is masked while commands run) ---- */
#define KBC_DATA 0x60
#define KBC_STAT 0x64
#define ST_OBF 0x01
#define ST_IBF 0x02
#define ST_AUX 0x20

static int kbc_wait_write(void)
{
	kx_u32 t0 = kexos_ms();
	while (kexos_inb(KBC_STAT) & ST_IBF)
		if (kexos_ms() - t0 > 50)
			return -1;
	return 0;
}

static int aux_write(kx_u8 b)
{
	if (kbc_wait_write())
		return -1;
	kexos_outb(KBC_STAT, 0xd4);
	if (kbc_wait_write())
		return -1;
	kexos_outb(KBC_DATA, b);
	return 0;
}

/* next aux byte within ms; keyboard bytes are left for VKD (its IRQ 1 takes them) */
static int aux_read(kx_u32 ms)
{
	kx_u32 t0 = kexos_ms();
	for (;;)
	{
		kx_u8 st = kexos_inb(KBC_STAT);
		if ((st & (ST_OBF | ST_AUX)) == (ST_OBF | ST_AUX))
			return kexos_inb(KBC_DATA);
		if (kexos_ms() - t0 > ms)
			return -1;
	}
}

static int send_byte(kx_u8 b)
{
	int tries, r;
	for (tries = 0; tries < 3; tries++)
	{
		if (aux_write(b))
			return -1;
		r = aux_read(100);
		if (r == 0xfa)
			return 0;
		if (r != 0xfe)   /* not "resend" */
			return -1;
	}
	return -1;
}

int tp_ps2_command(struct tp *t, u8 *param, int command)
{
	int nsend = (command >> 12) & 0xf, nrecv = (command >> 8) & 0xf, i, r;
	(void) t;
	if (send_byte((kx_u8) command))
		goto fail;
	for (i = 0; i < nsend; i++)
		if (send_byte(param[i]))
			goto fail;
	for (i = 0; i < nrecv; i++)
	{
		/* reset takes up to a second; GETID may answer with one byte instead of two */
		r = aux_read((command & 0xff) == 0xff ? 1500 : (i && (command & 0xff) == 0xf2) ? 30 : 200);
		if (r < 0)
		{
			if ((command & 0xff) == 0xf2 && i)
				break;
			goto fail;
		}
		param[i] = (u8) r;
	}
	return 0;
fail:
	n_errors++;
	return -1;
}

/* ---- events ---- */
static void post(struct tp_out *o)
{
	kx_u32 buttons = 0;
	kx_u32 dx = (kx_u32) o->dx, dy = (kx_u32) o->dy, mi = (kx_u32) mouse_instance;
	if (o->buttons & 1)
		buttons |= 0x20;   /* button 1 */
	if (o->buttons & 2)
		buttons |= 0x08;   /* button 2 */
	if (o->buttons & 4)
		buttons |= 0x10;   /* button 3 */
	if (o->buttons & 8)
		buttons |= 0x04;   /* button 4 */
	pend_wheel += o->wheel;
	pend_hwheel += o->hwheel;
	if (!mouse_instance)
		return;
	/* VMD_Post_Pointer_Message: ESI = dx, EDI = dy, AL = buttons, EDX -> Mouse_Instance */
	__asm__ __volatile__("int $0x20\n\t.long 0x000C0003"
	                     : "+S"(dx), "+D"(dy), "+a"(buttons), "+d"(mi)
	                     :
	                     : "ebx", "ecx", "memory", "cc");
	if (++n_posts <= 3)
		kexos_log(&klog, "kextouch: post dx %d dy %d buttons %x wheel %d\n", o->dx, o->dy, o->buttons, o->wheel);
}

static int isr(void *ref)
{
	int ours = 0;
	struct tp_out o;
	(void) ref;
	for (;;)
	{
		kx_u8 st = kexos_inb(KBC_STAT);
		if ((st & (ST_OBF | ST_AUX)) != (ST_OBF | ST_AUX))
			break;
		ours = 1;
		if (polling)
			break;   /* a command is running: its reader takes the byte */
		n_bytes++;
		if (tp_byte(&T, kexos_inb(KBC_DATA), kexos_ms(), &o))
		{
			n_packets++;
			post(&o);
		}
	}
	return ours || 1;   /* IRQ 12 is the aux port's alone */
}

static void tick(void *ref)
{
	struct tp_out o;
	(void) ref;
	if (!running)
		return;
	if (!polling)
	{
		kx_u32 f = kexos_irq_save();
		if (tp_tick(&T, kexos_ms(), &o))
			post(&o);
		kexos_irq_restore(f);
	}
	kexos_timer(20, tick, 0);
}

static int start_device(void)
{
	int r;
	polling = 1;
	if (hirq)
		kexos_irq_mask(hirq, 1);
	r = tp_init(&T);
	polling = 0;
	if (hirq)
		kexos_irq_mask(hirq, 0);
	return r;
}

/* ---- VMOUSE minidriver interface (Windows 95 DDK, "Mouse Minidrivers") ---- */
/* Mouse_Instance: WORD Flags, WORD IO_Base, BYTE IRQNumber, BYTE PortNum, WORD MouseType,
 * DWORD Reference, hDevNode, hIRQ, HWINT, Reset, Disable, Initialize, LoadHandle */
#define MIF_DETECTED 0x0004
#define MIF_NOIRQ 0x0080     /* VMOUSE must not virtualize the IRQ: we do */
#define VMD_TYPE_PS2 4

static int mouse_init(kx_u32 *mi)
{
	kexos_log(&klog, "kextouch: init, instance %x\n", (kx_u32) mi);
	if (running)
	{
		/* loaded again (kextouch.exe opening the device): keep VMOUSE's instance */
		if (mi && !mouse_instance)
			mouse_instance = mi;
		return 0;
	}
	if (!hirq)
	{
		hirq = kexos_irq_attach(12, 0, isr, 0);
		if (!hirq)
			kexos_log(&klog, "kextouch: IRQ 12 is taken (another mouse driver?)\n");
	}
	if (!hirq || start_device())
	{
		kexos_log(&klog, "kextouch: no device\n");
		return -1;
	}
	if (mi)
	{
		kx_u16 *w = (kx_u16 *) mi;
		kx_u8 *b = (kx_u8 *) mi;
		w[0] |= MIF_DETECTED | MIF_NOIRQ;   /* MI_Flags */
		w[1] = KBC_DATA;                     /* MI_IO_Base */
		b[4] = 12;                           /* MI_IRQNumber */
		w[3] = VMD_TYPE_PS2;                 /* MI_MouseType */
		mi[4] = hirq;                        /* MI_hIRQ */
	}
	mouse_instance = mi;
	running = 1;
	kexos_timer(20, tick, 0);
	kexos_log(&klog, "kextouch: running: %s\n", T.name);
	return 0;
}

/* ---- IOCTLs from kextouch.exe ---- */
typedef struct
{
	kx_u32 Internal1, VMHandle, Internal2, dwIoControlCode, lpvInBuffer, cbInBuffer, lpvOutBuffer, cbOutBuffer,
	    lpcbBytesReturned, lpoOverlapped, hDevice, tagProcess;
} DIOCPARAMS;

static kx_u32 ioctl(DIOCPARAMS *p)
{
	kx_u32 n = 0;
	void *out = (void *) p->lpvOutBuffer;
	switch (p->dwIoControlCode)
	{
	case 0:
	case (kx_u32) -1:
		return 0;
	case KEXTOUCH_IOCTL_STATUS:
	{
		struct kextouch_status *s = (struct kextouch_status *) out;
		if (p->cbOutBuffer < sizeof(*s))
			return 122;
		memset(s, 0, sizeof(*s));
		s->size = sizeof(*s);
		s->running = running;
		s->kind = (kx_u32) T.kind;
		strcpy(s->name, T.name);
		s->x_max = (kx_u32) T.x_max;
		s->y_max = (kx_u32) T.y_max;
		s->has_stick = (kx_u32) T.has_stick;
		s->bytes = n_bytes;
		s->packets = n_packets;
		s->posts = n_posts;
		s->errors = n_errors;
		s->resyncs = (kx_u32) T.out_of_sync;
		memcpy(&s->set, &T.set, sizeof(s->set));
		n = sizeof(*s);
		break;
	}
	case KEXTOUCH_IOCTL_SET:
		if (p->cbInBuffer < sizeof(struct tp_settings))
			return 122;
		{
			kx_u32 f = kexos_irq_save();
			memcpy(&T.set, (void *) p->lpvInBuffer, sizeof(struct tp_settings));
			kexos_irq_restore(f);
		}
		break;
	case KEXTOUCH_IOCTL_WHEEL:
	{
		struct kextouch_wheel *w = (struct kextouch_wheel *) out;
		kx_u32 f;
		if (p->cbOutBuffer < sizeof(*w))
			return 122;
		f = kexos_irq_save();
		w->wheel = pend_wheel;
		w->hwheel = pend_hwheel;
		pend_wheel = pend_hwheel = 0;
		kexos_irq_restore(f);
		n = sizeof(*w);
		break;
	}
	case KEXTOUCH_IOCTL_LOG:
		n = kexos_log_read(&klog, (char *) out, p->cbOutBuffer);
		break;
	case KEXTOUCH_IOCTL_RESET:
	{
		running = 0;
		kexos_delay_ms(50);
		n = (kx_u32) start_device();
		running = 1;
		kexos_timer(20, tick, 0);
		n = 0;
		break;
	}
	default:
		return 50;
	}
	if (p->lpcbBytesReturned)
		*(kx_u32 *) p->lpcbBytesReturned = n;
	return 0;
}

#define SYS_DYNAMIC_DEVICE_INIT 0x1B
#define SYS_DYNAMIC_DEVICE_EXIT 0x1C
#define W32_DEVICEIOCONTROL 0x23

__attribute__((used)) kx_u32 __stdcall kextouch_dispatch(kx_u32 msg, kx_u32 ebx, kx_u32 ecx, kx_u32 edx, kx_u32 esi,
                                                         kx_u32 edi)
{
	(void) ebx;
	(void) edx;
	(void) edi;
	switch (msg)
	{
	case SYS_DYNAMIC_DEVICE_INIT:
		/* VMOUSE: ECX -> Mouse_Instance (0 when loaded by kextouch.exe for diagnostics) */
		/* only a system-arena pointer can be VMOUSE's Mouse_Instance */
		return mouse_init(ecx >= 0xc0000000u && ecx < 0xfff00000u ? (kx_u32 *) ecx : 0) ? 1 : 0;
	case W32_DEVICEIOCONTROL:
		return ioctl((DIOCPARAMS *) esi);
	case SYS_DYNAMIC_DEVICE_EXIT:
		return 1;   /* stay: IRQ and timers cannot be released safely */
	default:
		return 0;
	}
}

__attribute__((naked)) void KEXTOUCH_CTRL(void)
{
	__asm__("pushl %eax\n\t"
	        "pushl %edi\n\t"
	        "pushl %esi\n\t"
	        "pushl %edx\n\t"
	        "pushl %ecx\n\t"
	        "pushl %ebx\n\t"
	        "pushl %eax\n\t"
	        "call _kextouch_dispatch@24\n\t"
	        "popl %ecx\n\t"
	        "cmpl $0x23, %ecx\n\t"
	        "je 1f\n\t"
	        "cmpl $1, %eax\n\t"
	        "cmc\n\t"
	        "ret\n"
	        "1:\n\t"
	        "clc\n\t"
	        "ret");
}

struct VxD_Desc_Block
{
	kx_u32 DDB_Next;
	kx_u16 DDB_SDK_Version, DDB_Req_Device_Number;
	kx_u8 DDB_Dev_Major_Version, DDB_Dev_Minor_Version;
	kx_u16 DDB_Flags;
	kx_u8 DDB_Name[8];
	kx_u32 DDB_Init_Order, DDB_Control_Proc, DDB_V86_API_Proc, DDB_PM_API_Proc, DDB_V86_API_CSIP, DDB_PM_API_CSIP,
	    DDB_Reference_Data, DDB_Service_Table_Ptr, DDB_Service_Table_Size, DDB_Win32_Service_Table, DDB_Prev, DDB_Size,
	    DDB_Reserved1, DDB_Reserved2, DDB_Reserved3;
};

struct VxD_Desc_Block KEXTOUCH_DDB = {
	0, 0x030A, 0x0000, 1, 0, 0,
	{ 'K', 'E', 'X', 'T', 'O', 'U', 'C', 'H' }, 0x80000000u, (kx_u32) KEXTOUCH_CTRL, 0, 0,
	0, 0, 0, 0, 0, 0, 0x50726576, sizeof(struct VxD_Desc_Block),
	0x52737631, 0x52737632, 0x52737633,
};
