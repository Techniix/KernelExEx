# OpenBSD em(4) in kexem.sys

Source: OpenBSD `src/sys/dev/pci` (master, fetched 2026-11-11):
`if_em.c`, `if_em.h`, `if_em_hw.c`, `if_em_hw.h`, `if_em_osdep.h`, `if_em_soc.h`,
`pcidevs.h`, and `src/sys/dev/mii/rgephyreg.h`.

Licences: the em files are Intel's 3-clause BSD licence (2001-2003 copyright
notices kept in each file); `if_em_soc.h` is ISC; `pcidevs.h` and
`rgephyreg.h` are BSD-style. All are compatible with GPLv2, and the
notices travel with the files.

## Local change

One change, in `kexbsd.patch`: `em_intr()` takes the interrupt cause from
`kexem_take_icr()` instead of reading ICR. On Windows 9x the NDIS ISR has to
claim (or decline) a shared PCI interrupt at interrupt level, which means
reading ICR there. Reading ICR clears it, so the ISR keeps the causes and
the handler, running later in NDIS's interrupt DPC, collects them.
Everything else is unmodified; `-DSMALL_KERNEL` drops MSI-X (Windows 9x has
line interrupts only), and the bpf/vlan/kstat config headers say "not
configured".

## Not supported

EP80579 (ICP_xxxx) parts need their GCU companion PCI function. Their IDs
are left out of `kexem.inf` and the GCU hooks are stubs (`kexem.c`).

## Updating

Replace the files, re-apply `kexbsd.patch`, regenerate the INF:

    python3 drivers/kexnet/gen_inf.py <if_em.c> <pcidevs> drivers/kexnet/kexem.inf
