# Where work stopped (2026-11-22, test build 24)

Waiting for: Dell Vostro 1440 results (C:\KEXTOUCH.TXT, HD Audio speaker/headphones,
RAM shown with KEXMEM, display modes offered by the VESA driver, USB stick boot).

Next, in order:
1. Fix whatever the Vostro report shows (touchpad ALPS model, audio codec paths).
2. Keys: kexkeys done (Fn + USB media keys, test build 24). Still: NKRO check on hardware,
   installer component + Run key for kexkeys and kextouch, on-screen volume indicator.
3. kexinfo hardware report (docs/KEDCL.md), reading KEDCL logs (kextouch first).
4. Touchpad: Synaptics absolute mode and Elantech (port from Linux, as tp_alps.c);
   ALPS v6-v9; horizontal scroll (Shift+wheel?); installer component for kextouch.
5. Installer components for the VESA display driver; brightness keys.
6. kexmedia: build the stick image on Windows (port proto_image.py + ISO writing),
   so Linux/WSL is not needed.
7. Our own large-memory patch (media/README.md, clean-room).
8. Earlier backlog: batches C, D (exFAT), F; T2U Plus / AC'97 / LAN results from build 17.

Known facts: CSMWrap needs >= 2 CPUs; Windows 98 warm reboot under QEMU TCG can hang
in VTD (not our bug); NTKERN lacks READ_REGISTER_* and KeSetTimerEx (use volatile, KeSetTimer).
