# Install media (USB stick / CD) — work in progress

## Making the stick

1. `make_iso.sh KEXSETUP.IMG REBIOS.EFI CSMWRAP.EFI KEXSETUP.ISO` builds one hybrid image
   for USB sticks and CDs.
2. On a Windows XP–11 PC, run `kexmedia.exe` (built by `build_core.py --only kexmedia`)
   with `KEXSETUP.ISO` beside it, as administrator. It lists only USB/removable disks
   (never the system disk), asks for ERASE, releases the stick's drives, writes the
   image and reads it back to check. Any raw image writer works too (`dd`, Rufus in
   DD mode).

## Booting

- **BIOS / CSM (e.g. Dell Vostro 1440):** isolinux → MEMDISK → the setup RAM disk. Tested in QEMU.
- **UEFI-only PCs:** `\EFI\BOOT\BOOTX64.EFI` is ReBIOS (GPLv2: SeaBIOS in a thin
  hypervisor; needs VT-x with EPT or AMD-V, and an IOMMU (VT-d / AMD-Vi), enabled in the
  firmware setup). Without those, start `\EFI\CSMWRAP\CSMWRAP.EFI` (CSMWrap, LGPL-2.1,
  deprecated upstream; needs at least 2 CPU threads) from the firmware's boot-from-file
  menu. Neither can be tested in stock QEMU; CSMWrap gets past its start-up checks in
  QEMU/OVMF with 2 CPUs.

Boots a RAM disk (syslinux MEMDISK) that prepares the laptop's disk (`dos/kexprep.c`),
copies Windows 98 and KernelExEx onto it, and lets Windows 98 Setup run from the hard disk
with `stage/MSBATCH.INF`. Prototype builder: `stage_extra.py` + `proto_image.py`.

## KEXMEM.SYS — more than 1 GB of RAM

`dos/kexmem.asm`, loaded first in CONFIG.SYS (`DEVICE=C:\KEXMEM.SYS [/MAX=n] [/NOCR] [/Q]`):

- hooks int 15h E820h/E801h/E881h/88h so DOS, HIMEM and Windows see RAM only below
  n MB (default 1024; 64–3072). Unpatched Windows 98/ME crash at start-up with much more.
  Nothing in Windows is patched.
- resets CR0/CR3/CR4/EFER left set by some CSM BIOSes ("While initializing device VCACHE"),
  checking with CPUID which registers exist. Idea from mintsuki's CREGFIX (0BSD);
  independent code. `/NOCR` skips it.

Tested in QEMU: an installed Windows 98 SE that crashed with 4 GB (fatal exception 06)
boots to the desktop with KEXMEM and reports 1022 MB. The media puts it in CONFIG.SYS;
for existing installs the KernelExEx installer has the component
"More than 1 GB of RAM (memory limit)". PATCHMEM (R. Loew) is proprietary and is not used.

## Planned: our own large-memory patch (beyond 1 GB)

KEXMEM caps Windows at about 1 GB. To use up to ~3.5 GB we need our own equivalent of
PATCHMEM (R. Loew). To do:

- Research thoroughly what limits Windows 9x above 1 GB: VMM's system arena (1 GB),
  VCACHE sizing, the page-frame tables, and drivers that map memory into the system arena.
- PATCHMEM is proprietary, so a clean-room process: study its behaviour (and, where lawful
  for interoperability, its patches) and write a specification. Implement only from that
  specification and from our own analysis of VMM.VXD/VCACHE.VXD on the user's own CD,
  without copying its code or patch bytes. Ship it as a patcher run on the user's own files.
