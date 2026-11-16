# KEXACPI.VXD and kexpower: laptop batteries for Windows 98 SE / ME

Windows 98 reads a laptop battery through the APM BIOS (or through its own
ACPI driver, which setup turns off on most machines because it cannot cope
with today's ACPI tables). Laptops from about 2005 on have no APM BIOS: the
battery is described only by ACPI control methods, so Windows 98 shows no
battery at all, or a wrong one. KernelExEx reads those methods itself.

- **KEXACPI.VXD** runs ACPICA (Intel's reference ACPI interpreter, in
  `acpica/`, used under its BSD-3-Clause OR GPL-2.0-only terms) inside
  Windows 98 and evaluates the firmware's methods: `_STA`, `_BIX` (ACPI 4.0
  and later) or `_BIF`, and `_BST` for each battery (`PNP0C0A`), `_PSR` for
  the AC adapter (`ACPI0003`), `_LID` for the lid (`PNP0C0D`). Laptops keep
  the battery data in the embedded controller, so KEXACPI has its own EC
  handler (ports from the `ECDT` or the `PNP0C09` device's `_CRS`, polled,
  with the firmware's global lock when `_GLK` asks for it).
- **kexpower.exe** shows it: a taskbar icon filled to the charge (green,
  yellow under 40 %, red under 15 %; a plug on AC power, a bolt when
  charging), and a window with each battery's charge, state, time left or
  to full, capacity against new (the battery's health), rate, voltage,
  charge cycles, model, type, maker and serial number. It warns when the
  charge falls to a set level, can shut Windows down when the battery is
  nearly empty (after a minute's notice that can be cancelled), and can turn
  the screen off or shut down when the lid closes. **kexpower.cpl** is its
  Control Panel icon ("Battery").

Settings: `HKEY_CURRENT_USER\Software\KernelExEx\Power` (`TrayIcon`, `Warn`,
`WarnPercent`, `Critical`, `CriticalPercent`, `Lid`: 0 nothing, 1 screen off,
2 shut down). The program's interface to the driver is `kexacpi.h`.

## How it runs

- The machine stays in legacy mode: no ACPI interrupt (SCI), no events, no
  sleep states. All work happens inside the program's `DeviceIoControl`
  call, one at a time, on the driver's own 64 KB stack (AML nests deeply;
  a Windows 98 ring-0 thread stack is a few KB). ACPICA starts on the first
  call (about a quarter of a second); a reading takes a few milliseconds.
- Windows loads KEXACPI.VXD from `WINDOWS\SYSTEM` the first time
  kexpower.exe opens `\\.\KEXACPI.VXD`; it stays loaded.
- If the machine is in ACPI mode and has an SMI command port, Windows' own
  ACPI driver switched it there: KEXACPI leaves it alone and kexpower says
  that Windows shows the battery itself.
- An embedded controller that stops answering costs 200 ms per byte; after
  8 time-outs in a row KEXACPI stops asking it (until "read again").
- Physical memory is mapped with `_MapPhysToLinear`, which never gives the
  address space back, so every mapping is kept and reused.

## What it cannot do (yet)

- Power and sleep buttons: in legacy mode they reach the firmware, not
  Windows (they signal through the SCI). Real standby needs Windows' own
  power management, which Windows 98 does not have on these machines.
- Windows' own battery meter (Control Panel, Power Management) stays empty:
  it reads APM through VPOWERD. Programs that ask Windows for the battery
  (`GetSystemPowerStatus`) get no answer either; feeding them from KEXACPI
  is planned.
- Batteries on smart battery systems (`SBS`, `ACPI0002`) rather than
  control-method batteries: rare on laptops from 2005 on.

## Tests (QEMU, Korean 98 SE)

QEMU has no battery, so `tests/acpi/` adds simulated hardware as extra ACPI
tables (`-acpitable file=...aml`, compiled with ACPICA's `iasl`):

- `battest.asl`: a modern battery (`_BIX`, mWh) that drains on every
  reading, an older one (`_BIF` only, mAh, a serial number given as a
  number) that charges, an AC adapter and a lid. `tests/acpi/acpitest.c`
  reads everything: all values as expected; charge, time left and time to
  full computed right; first call 243 ms, then 3 to 7 ms.
- `ectest.asl`: a battery whose `_BST` reads an embedded controller.
  QEMU has none, so this tests the failure path: the EC's `_REG` runs, the
  silent EC is given up on after two readings, the other batteries keep
  working.
- `lowbat.asl`: one battery, nearly empty: kexpower's warning at 10 %, the
  critical notice and its countdown, and Windows shutting down at the end.
- kexpower's window and icon with both batteries.

Not tested on a real laptop yet: the embedded controller code in
particular needs real hardware.
