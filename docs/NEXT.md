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
- Next (user asked): RAM disk for the swap file from RAM Windows cannot use: above the
  KEXMEM limit and above 4 GB (PAE/PSE-36 mapping in our own driver; PATCHMEM's family of
  tools had RAMDSK64 for this). As installer option(s).
- Open: first start after the KernelExEx installer can hang in QEMU in ISAPNP's VTD delay
  loop (PIT channel 0 reads never change, interrupts off); happens at 1 GB too, so not
  the memory patch. Reset -> fine. Investigate (which component touches the PIT?).
- Ideas to go beyond PATCHMEM: test long runs under memory load; system-arena pressure
  (PATCHMEM /A moves AGP/unfixed tables); Safe Mode above 1 GB without CONFIG.SYS.
