# KernelExEx Driver Compatibility Layer (KEDCL) and hardware reporting

Goals (from the project owner, 2026-11-17):

- Every new driver should make the next driver easier to implement.
- Every new compatibility feature should make the next application easier to run.
- Porting a driver should become increasingly mechanical: a new driver needs a small
  platform-specific adaptation, not another custom compatibility implementation.

## What KEDCL is responsible for

| Responsibility | Where it lives today | Plan |
|---|---|---|
| Kernel primitive compatibility (locks, events, threads) | `drivers/kexbsd/src/kern.c`, `irqlock.h` (OpenBSD API on WDM); `toolchain/vxd/vxdrt.c` (VxD); ACPICA's `osl98.c` | One `kexos.h` C API with a WDM and a VxD backend; kexbsd and osl98 become thin users of it |
| Memory allocation and DMA abstraction | `kexbsd/src/obsd_dma.c` (bus_dma), PortCls DMA in the audio drivers, `MmAllocateContiguousMemory` in kexehci/kexhda | `kexos_dma_alloc()` (contiguous, below 4 GB, physical address), `kexos_dma_map()` |
| Interrupt and deferred-work handling | kexbsd (isr + DPC), kexnet's `isr`/`intr_enable` split, VPICD in VxDs | `kexos_irq_attach(isr, dpc)` with the same two-stage model everywhere |
| Timer and scheduling translation | kexbsd `timeout(9)` on KeSetTimer; kexhda one-shot re-armed timers (Windows 98 has no periodic KeSetTimerEx) | `kexos_timer_*` (one-shot, re-armed), `kexos_sleep`, `kexos_stall` |
| PCI configuration and resource management | kexbsd `bus.c` (pci_conf_read/write, BARs), PortCls resource lists, SCSI miniport config | `kexos_pci_*`: config space, BAR map (MMIO via `MmMapIoSpace` + volatile access: Windows 98 NTKERN has no `READ_REGISTER_*`) |
| USB and network bus abstractions | `kexbsd/src/kexusb_wdm.c`, `lib/obsd_usb.c`; `kexnet/obsd_net.c` (NDIS miniport glue) | Keep: these are the OpenBSD personality of KEDCL |
| Driver lifecycle and device enumeration | per driver (PnP IRPs, VMOUSE/VPICD init, SCSI miniport) | `kexos_driver` descriptor (match IDs, probe, attach, detach, suspend/resume) |
| Logging, tracing and crash diagnostics | per-driver `HKLM\Software\KernelEx\Diag\<driver>` values; debugcon builds (`--debugcon`) | `kexos_log()` into a shared ring buffer (KEXLOG), copied to the Diag key and readable live by kexinfo; panic/assert records the last N log lines |

Personalities on top of the core: **OpenBSD** (kexbsd: NICs, Wi-Fi, USB) and **Linux**
(new: the touchpad protocol code ported from Linux `drivers/input/mouse`, GPLv2). A
personality maps a source OS's driver API onto `kexos`; a ported driver then needs only
its bus glue.

### Rules for new drivers (from now on)

1. Hardware logic goes into a portable core with no OS calls, unit-tested on the build
   host (as `kextouch/core`, ACPICA, the storage decoders).
2. OS access goes through `kexos.h` (or an existing personality), never straight to
   NTKERN/VMM, except inside the kexos backends.
3. Every driver logs through `kexos_log()` and reports its state (device found, IDs,
   revision, mode, errors) so it appears in the hardware report.
4. A workaround found once (Windows 98 kernel export gaps, timer limits, 8042 sharing...)
   goes into the backend so the next driver gets it for free.

### Migration (done opportunistically, not as a rewrite)

1. `kexos.h` + WDM backend, extracted from kexbsd/kern.c and kexhda (first: log, timers,
   MMIO, contiguous DMA).
2. VxD backend from vxdrt.c/osl98.c (KEXACPI, the touchpad VxD).
3. Existing drivers move over when they are next changed.

## Hardware report (kexinfo)

A tool (`kexinfo.exe`, plus a "Hardware report" button in the KernelExEx control panel)
that exports one text file with:

- PCI/PCIe devices: bus/device/function, vendor/device/subsystem IDs, revision, class,
  BARs (memory and I/O assignments), IRQ line/pin, capabilities (MSI, PCIe link, power).
- USB topology: controllers, hubs, ports, devices (VID/PID, class, speed), from kexusb.
- Hardware IDs, chip revisions and driver binding: every devnode (Configuration Manager)
  with its hardware/compatible IDs, the INF and driver it is bound to, status/problem code.
- Interrupt, DMA and memory/I/O resource assignments per device, and conflicts.
- Firmware: BIOS vendor/version/date (SMBIOS), ACPI tables (signatures, OEM IDs,
  revisions), boot mode (BIOS / CSM / ReBIOS / CSMWrap), KEXMEM state.
- Driver logs: the KEXLOG ring and every `KernelEx\Diag` key.
- Device health: problem codes, error counters each driver reports (DMA errors, timeouts,
  resets), battery health from KEXACPI.

The report is plain text so it can be attached to a bug report or pasted into a chat.
