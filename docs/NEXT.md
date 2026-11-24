# Handoff (after test build 25, 2026-11-23)

Waiting for: Dell Vostro 1440 results with build 25 (install stick from
KernelExEx-USB-Maker.exe, then C:\KEXTOUCH.TXT, HD Audio, VESA, RAM shown).
Open: in QEMU (2 GB, Westmere) the first start after Setup sometimes stops in the
display driver's init (cirrus; BOOTLOG ends at DISPLAY.drv) and the next start works;
not seen with 512 MB/pentium2 runs. Check on hardware; if it recurs, step through it.

Next, in order:
1. Fix whatever the Vostro report shows (touchpad ALPS model, audio codec paths).
2. Keys: kexkeys done (Fn + USB media keys, test build 24). Still: NKRO check on hardware,
   installer component + Run key for kexkeys and kextouch, on-screen volume indicator.
3. kexinfo hardware report (docs/KEDCL.md), reading KEDCL logs (kextouch first).
4. Touchpad: Synaptics absolute mode and Elantech (port from Linux, as tp_alps.c);
   ALPS v6-v9; horizontal scroll (Shift+wheel?); installer component for kextouch.
5. Installer components for the VESA display driver; brightness keys.
6. Done (build 25): KernelExEx-USB-Maker.exe (media/win, pack_media.py).
7. Our own large-memory patch (media/README.md, clean-room).
8. Earlier backlog: batches C, D (exFAT), F; T2U Plus / AC'97 / LAN results from build 17.

Known facts: CSMWrap needs >= 2 CPUs; Windows 98 warm reboot under QEMU TCG can hang
in VTD (not our bug); NTKERN lacks READ_REGISTER_* and KeSetTimerEx (use volatile, KeSetTimer).

## After build 27 (memory)

- Done: "Use up to 2 GB" and "Use all RAM, up to ~3.5 GB" (kexpatch /mem: our range
  merge + PATCHMEM's VMM/VCACHE set, page-map-aware vxdfile). QEMU: 2048/3072/3560 MB.
- Done (build 28): KEXRAM.VXD RAM disk Z: with the swap file, from RAM above KEXMEM's
  limit and above 4 GB (PSE-36 window). Still to do: heavy-paging test that works in
  the emulator, report the RAM disk in kexinfo (registry), let users pick the letter,
  optionally TEMP on Z:.
- Open: first start after the KernelExEx installer can hang in QEMU in ISAPNP's VTD delay
  loop (PIT channel 0 reads never change, interrupts off); happens at 1 GB too, so not
  the memory patch. Reset -> fine. Investigate (which component touches the PIT?).
- Ideas to go beyond PATCHMEM: test long runs under memory load; Safe Mode above 1 GB
  without CONFIG.SYS.
- **For modern GPUs later (user's note): PATCHMEM's /A option.** Windows 9x puts AGP
  apertures and "unfixed" allocations in the system arena (C0000000-FFFFFFFF, shared with
  VxDs, the page database, VCACHE); a card with a large aperture/VRAM window fills it and
  Windows fails. /A redirects AGP and unfixed system-arena requests of 128 MB or more to
  the shared arena (80000000-BFFFFFFF). PATCHMEM.C: agp98[] (agp981d/agp982d: replaces
  `mov ebp,0C0000h` in VMM's _PageReserve arena choice with a call that picks 080000h
  for large requests), agpm[] for ME. Port it [Loew] into mempatch.c when GPU work starts;
  also look at mapping large BARs (256 MB+) for display drivers.

## Test target from 2026-11-23: Dell Latitude E6410 (the Vostro 1440 stays in mind)

From Dell's setup guide and reviews (to confirm with kexinfo on the machine):
- Core i5/i7 Arrandale (i5-520M/540M, i7-620M...), Mobile Intel QM57 (5 series PCH).
- Up to 8 GB DDR3 (2 SODIMM): the RAM disk above 4 GB matters here.
- Graphics: Intel HD (UMA) or NVIDIA NVS 3100M (512 MB): note the AGP/arena item above.
- LAN: Intel 82577LM (PCH gigabit; OpenBSD em(4) covers it: check kexem's ID list).
- Wi-Fi: Intel Centrino 6200/6300 (no driver yet) or Dell/Broadcom options.
- Audio: Intel HD Audio with IDT 92HD81B1C5 codec (kexhda: check the codec path).
- ALPS touchpad + trackstick (kextouch ALPS support; trackstick middle-button scroll).
- SD/MMC reader, ExpressCard/PC Card, FireWire, eSATA, legacy BIOS (no UEFI needed);
  SATA AHCI/IDE switch in the BIOS.

### First hardware report (2026-11-23): it is a Latitude E6510 (subsystem 1028:040B)

- Core i5 M460 (Arrandale), 5 series PCH (3B07 LPC), Intel HD graphics 8086:0046,
  2 GB seen (2 GB option), SMBIOS 2.6, ACPI (Dell).
- Not set up (problem 28) although our INFs list them: 82577LM 8086:10EA (kexem),
  HD Audio 8086:3B56 (kexhda), AHCI 8086:3B2F (kexahci, class match). Windows met them
  before KernelExEx and offers drivers only for new devices. Fixed for build 29:
  - setup's INFs go to INF, not INF\OTHER: in QEMU Windows' driver search did not
    find our INF in INF\OTHER at all (also after rebuilding its index); in INF it did;
  - kexinfo /reinstall (setup runs it last) makes Windows forget such devices and
    removes the driver index, so the next start sets them up from our INFs. Disk
    controllers are left alone (the boot disk: AHCI needs /reinstall /all, by hand).
    Tested in QEMU end to end with kexhda (problem 28 -> wizard finds our INF -> starts).
  - display still on vga.drv: most likely the same INF\OTHER cause (KexVesaRedetect
    forgot the card, Windows found no INF). kexinfo now prints each device's driver
    files (drv=, DevLoader, NTMPDriver) to confirm.
- Wi-Fi is an Atheros AR9285 168C:002B (an HP card): OpenBSD athn(4), ISC, PCI part
  needs no firmware. This replaces the Intel 6200 plan.
- Ricoh R5U822 SD host 1180:E822 (SDHCI, class 0805): a storage driver; QEMU has sdhci-pci.
- SMBus 3B30 and thermal 3B32: no driver needed (could get a null INF to clear the marks).
- Touchpad: ALPS v3 Pinnacle + stick detected, but 0 bytes received after set-up.
  kextouch now turns on the 8042 aux interrupt (command byte bit 1) after set-up and
  reports IRQ 12 count and the command byte; QEMU does not reproduce (the old driver
  receives there with the bit cleared too), so the next report decides.
