# OpenBSD urndis(4) in kexrndis.sys

Source: OpenBSD `src/sys/dev/usb/if_urndis.c`, `if_urndisreg.h` and
`src/sys/dev/rndis.h` (master, fetched 2026-11-11). ISC licence (Jonathan
Armani, 2010), notices kept in the files.

## Local change (`kexbsd.patch`)

`urndis_init()` sends REMOTE_NDIS_INITIALIZE, which puts the device back in
the RNDIS "initialized" state, and never sets the packet filter again, so
the device passes no data (QEMU usb-net and Linux's RNDIS gadget, which
Android tethering uses, both enforce the RNDIS state machine). The patch
sets the filter after the INITIALIZE, with DIRECTED added so unicast frames
arrive. Worth offering upstream.

OpenBSD's cdce(4) (CDC Ethernet) is not used: it carries Bill Paul's
4-clause BSD licence.
