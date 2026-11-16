/*
 *  KernelExEx - KEXACPI.VXD's DeviceIoControl interface (shared with the
 *  power programs). Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  Open with CreateFile("\\\\.\\KEXACPI.VXD", 0, 0, NULL, 0, 0, NULL): Windows
 *  loads the VxD from WINDOWS\SYSTEM the first time. Then
 *  DeviceIoControl(h, KEXACPI_IOCTL_STATUS, NULL, 0, &st, sizeof(st), &n, NULL)
 *  reads the batteries, the AC adapter and the lid from the firmware's ACPI
 *  methods (_BIX/_BIF, _BST, _PSR, _LID). Each call reads them anew, so
 *  calling about every 10 seconds is plenty.
 */
#ifndef KEXACPI_H
#define KEXACPI_H

#define KEXACPI_VERSION 0x00010000u

#define KEXACPI_IOCTL_STATUS 0x4B500001u   /* out: KEXACPI_STATUS */
#define KEXACPI_IOCTL_REREAD 0x4B500002u   /* read the batteries' fixed data (_BIX/_BIF) again next time */

/* KEXACPI_STATUS.state */
#define KEXACPI_ST_RUNNING 1     /* reading the firmware's ACPI methods */
#define KEXACPI_ST_WINACPI 2     /* Windows' own ACPI driver runs this machine: left alone */
#define KEXACPI_ST_NOACPI 3      /* no ACPI tables found */
#define KEXACPI_ST_FAILED 4      /* starting ACPICA failed (acpi_status says why) */

#define KEXACPI_UNKNOWN 0xFFFFFFFFu
#define KEXACPI_MAXBAT 4

/* KEXACPI_BATTERY.state (as in _BST) */
#define KEXACPI_BAT_DISCHARGING 1u
#define KEXACPI_BAT_CHARGING 2u
#define KEXACPI_BAT_CRITICAL 4u

typedef struct
{
	unsigned long present;       /* 0: the bay is empty */
	unsigned long units;         /* 0: mW and mWh; 1: mA and mAh */
	unsigned long design_cap;    /* KEXACPI_UNKNOWN when the firmware does not say */
	unsigned long full_cap;      /* last full charge */
	unsigned long design_volt;   /* mV */
	unsigned long warn_cap, low_cap;
	unsigned long cycles;        /* charge cycles (_BIX only) */
	unsigned long rechargeable;
	unsigned long state;         /* KEXACPI_BAT_* */
	unsigned long rate;          /* present rate, mW or mA */
	unsigned long remaining;     /* mWh or mAh */
	unsigned long voltage;       /* mV */
	unsigned long percent;       /* 0..100, or KEXACPI_UNKNOWN */
	unsigned long minutes_left;  /* discharging: time left; else KEXACPI_UNKNOWN */
	unsigned long minutes_full;  /* charging: time to full; else KEXACPI_UNKNOWN */
	unsigned long has_bix;       /* the firmware has _BIX (ACPI 4.0 and later) */
	char model[32], serial[32], type[16], oem[32];
	char path[32];               /* where it is in the ACPI namespace, e.g. \_SB_.BAT0 */
} KEXACPI_BATTERY;

typedef struct
{
	unsigned long size;          /* sizeof(KEXACPI_STATUS) */
	unsigned long version;       /* KEXACPI_VERSION */
	unsigned long state;         /* KEXACPI_ST_* */
	unsigned long acpi_status;   /* ACPICA status of the last failure, 0 if none */
	long ac;                     /* 1: on mains; 0: on battery; -1: no AC adapter device */
	long lid;                    /* 1: open; 0: closed; -1: no lid device */
	unsigned long nbat;          /* batteries found (bays, present or not) */
	unsigned long have_ec;       /* an embedded controller is in use */
	KEXACPI_BATTERY bat[KEXACPI_MAXBAT];
} KEXACPI_STATUS;

#endif
