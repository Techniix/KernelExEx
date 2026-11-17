/*
 *  KernelExEx - KEXTOUCH.VXD <-> kextouch.exe interface (DeviceIoControl on \\.\KEXTOUCH).
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 */
#ifndef KEXTOUCH_H
#define KEXTOUCH_H

#define KEXTOUCH_IOCTL_STATUS 0x4b540001   /* out: struct kextouch_status */
#define KEXTOUCH_IOCTL_SET 0x4b540002      /* in: struct tp_settings */
#define KEXTOUCH_IOCTL_WHEEL 0x4b540003    /* out: struct kextouch_wheel (and clears it) */
#define KEXTOUCH_IOCTL_LOG 0x4b540004      /* out: the driver log, oldest first */
#define KEXTOUCH_IOCTL_RESET 0x4b540005    /* detect and set up the device again */

struct kextouch_status
{
	unsigned size, running, kind;
	char name[48];
	unsigned x_max, y_max, has_stick;
	unsigned bytes, packets, posts, errors, resyncs;
	struct tp_settings set;
};

struct kextouch_wheel
{
	int wheel, hwheel;   /* WHEEL_DELTA units (120 a notch) */
};

#endif
