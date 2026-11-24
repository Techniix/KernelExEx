# OpenBSD athn(4) in kexathn.sys

Atheros 802.11n PCI and PCIe Wi-Fi: AR5416, AR5418, AR9160, AR9280, AR9281
(AR928X), AR9285, AR9227, AR2427 and AR9287. The Latitude E6510's card is an
AR9285 (168C:002B). These chips need no firmware file.

Source: OpenBSD `src` (GitHub mirror, master, commit 6ff61ed9, fetched
2026-11-24): `sys/dev/ic/athn.c`, `athnreg.h`, `athnvar.h`, `ar5008.c`,
`ar5008reg.h`, `ar5416.c`, `ar5416reg.h`, `ar9280.c`, `ar9280reg.h`, `ar9285.c`,
`ar9285reg.h`, `ar9287.c`, `ar9287reg.h`, `ar9003.c`, `ar9003reg.h`,
`ar9380.c`, `ar9380reg.h` (linked because athn.c refers to the AR9380 code;
no AR9380 PCI ID is matched), and `sys/dev/pci/if_athn_pci.c`. `pcidevs.h` is
the copy em uses.

Licence: ISC (Damien Bergamini; Atheros Communications), compatible with
GPLv2; the notices are kept in each file.

## Local changes

None to the OpenBSD files. `athn_usb.h` is the config header ("no athn_usb").
`kexathn.c` is the front end:

- The ISR checks AR_INTR_ASYNC_CAUSE / AR_INTR_SYNC_CAUSE without clearing
  anything and, if the interrupt is ours, switches off only the host
  interface's line enables (AR_INTR_ASYNC_ENABLE, AR_INTR_SYNC_ENABLE).
  athn_intr then runs in the interrupt DPC and reads the causes itself;
  masking with AR_IER instead would drop the asynchronous cause bit, and
  athn_intr would find nothing. The enables come back after the DPC while
  the interface is running (as athn_enable_interrupts sets them).
- Shutdown disables interrupts and stops the interface (no DMA at power off).

kexbsd gained what athn needed: `m_makespace` (obsd_net.c: the CCMP header
for hardware encryption) and the PCI_CACHELINE / PCI_LATTIMER and
pci_product_id_t definitions in pcireg.h.

## Testing

QEMU has no Atheros device: the first test is the E6510. The driver writes
its messages, like the other network drivers, to C:\WINDOWS\KEXNET.LOG.

## Updating

Replace the files and regenerate the INF:

    python3 drivers/kexnet/athn/gen_inf.py drivers/kexnet/athn/dev/pci/if_athn_pci.c \
        drivers/kexnet/athn/dev/pci/pcidevs.h drivers/kexnet/athn/kexathn.inf
