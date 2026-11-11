# kexbsd: running BSD drivers on Windows 98 SE / ME

One compatibility layer lets KernelExEx use FreeBSD, OpenBSD and NetBSD
drivers unchanged or nearly so: WiFi (decision 23), USB 3 and USB 2 host
controllers, network cards, AHCI and NVMe, HD Audio, Bluetooth, SD readers,
sensors and, later, the BSD graphics drivers (decisions 32, 40, 41). This is
the design; it follows the proven pattern of Haiku's FreeBSD compatibility
layer and FreeBSD's own LinuxKPI.

## Shape

```
  Windows 98 class driver / stack      NDIS (network), IOS (disks), HIDCLASS,
        |                              USBD, portcls (audio), VxD (display)
  front end: one per device class      kexbsd_ndis.sys, kexbsd_disk.sys, ...
        |                              (WDM or NDIS miniport, INF-installed)
  kexbsd.sys: the BSD kernel API       export driver shared by all front ends
        |
  BSD driver, compiled as is           if_re.c, xhci.c, ahci.c, hdac.c, ...
```

- **kexbsd.sys** is a WDM *export driver* (a kernel-mode DLL other drivers
  import), loaded once. It implements the BSD kernel interfaces drivers use.
- A **front end** per device class turns the BSD driver into what Windows 98
  expects: an NDIS miniport for `ifnet` drivers, an IOS port driver for
  `cam`/`ata` disk drivers, a HID minidriver for input, a USB host
  controller driver for `xhci`/`ehci`, a portcls miniport for `sound`.
- **BSD drivers** are compiled from their upstream sources with a small,
  documented patch set, so updates can be pulled in.

## BSD kernel API, mapped onto Windows 98 SE WDM

Everything below exists on 98 SE (see `net/tools/exports/win98se-wdm`;
names there are proven by drivers Microsoft shipped).

| BSD interface | Windows 98 SE WDM |
|---|---|
| `malloc(9)`, `uma(9)` | `ExAllocatePoolWithTag` (NonPaged), size-class caches on top |
| `mtx(9)` spin / default | `KeAcquireSpinLock` (`KfAcquireSpinLock` in HAL) / spin lock + event for sleepable |
| `sx(9)`, `rw(9)` | fast mutex or event-based reader/writer lock |
| `callout(9)` | `KeInitializeTimer` + `KeSetTimer` + DPC |
| `taskqueue(9)`, `ithread` | worker thread per queue (`PsCreateSystemThread`, events) |
| interrupt filter / handler | `IoConnectInterrupt` ISR runs the filter, DPC runs the handler |
| `bus_space(9)` | `READ_REGISTER_*` / `READ_PORT_*` on `MmMapIoSpace` or I/O ranges from the start resources |
| `bus_dma(9)` | `MmAllocateContiguousMemory` (below 4 GB), `MmGetPhysicalAddress`; bounce buffers for mbufs |
| PCI config (`pci_read_config`) | `HalGetBusData` / `HalSetBusDataByOffset` |
| `newbus` probe / attach / detach | PnP `START_DEVICE` / `REMOVE_DEVICE` in the front end |
| `mbuf(9)` | own implementation; NDIS packets copied in and out at the edge |
| `firmware(9)` | files from `%WINDIR%\SYSTEM\kexbsd\firmware`, read at PASSIVE_LEVEL |
| `sysctl`, `device_printf`, `bootverbose` | registry `HKLM\...\kexbsd\<driver>` and the driver log |
| `DELAY()` | `KeStallExecutionProcessor` |
| `ticks`, `getmicrouptime`, `getnanotime` | `KeQueryInterruptTime`, `KeQuerySystemTime` |

`HalGetBusData` and `HalSetBusDataByOffset` are in NTKERN's name table but no
driver Microsoft shipped imports them; the first front end confirms them at
run time (PCI configuration reads during attach).

Not available on 98 SE and avoided: `IoAllocateWorkItem`, `IoQueueWorkItem`,
`MmMapLockedPagesSpecifyCache`. Memory above 4 GB is never used (Windows 9x
cannot address it).

## What Device Manager shows (decision 33)

- Each device comes from an INF in the right class (Net, HDC, USB, MEDIA,
  System ...), provider "KernelExEx", described as
  `<chip name> (KernelExEx <bsd driver>)`, e.g.
  "Realtek RTL8168 Gigabit Ethernet (KernelExEx re)". INFs are generated from
  the BSD drivers' own device tables, as `kexpad.inf` is from xpad.
- `.sys` files carry VERSIONINFO (company KernelExEx, original BSD driver and
  upstream revision in the comments), so Driver File Details is meaningful.
- A failed attach becomes a PnP failure (`STATUS_DEVICE_CONFIGURATION_ERROR`
  and friends) so the device gets the yellow mark and Windows' own problem
  text. The reason, the hardware IDs, chip revision and firmware file go to
  `%WINDIR%\KernelEx\logs\kexbsd-<driver>.log`.
- A General-tab property page (a small `EnumPropProvider` DLL) shows the BSD
  driver name, version, firmware and the last log lines.

## Order of work

1. kexbsd.sys core: memory, locks, callout, taskqueue, bus_space, bus_dma,
   PCI config, interrupts; host tests where possible; a null driver.
2. Network front end (NDIS 5 miniport) with FreeBSD `re`: a simple, very
   common PCI chip that 86Box and QEMU can't emulate but real boards have;
   `em` (Intel 8254x) can be tested in QEMU (`-nic model=e1000`).
3. WiFi: net80211 and `athn`/`run`/`urtwn` (docs/research/wifi.md), which
   need the USB front end.
4. xHCI and EHCI host controllers (USB front end, replaces 98's USBD role for
   those controllers).
5. Storage: AHCI and NVMe through IOS.
6. HD Audio, Bluetooth, SD, sensors, then graphics (`drm`).

## Licences

FreeBSD and OpenBSD drivers are BSD-2/3-clause or ISC; NetBSD is BSD-2-clause.
All can be combined with GPLv2. Files under the 4-clause BSD licence (with
the advertising clause) are not used. Each imported driver keeps its licence
header and is listed in `third_party/bsd/PROVENANCE.md` with the upstream
revision.
