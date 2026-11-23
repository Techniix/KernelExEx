# Install media (USB stick / CD) — work in progress

## Making the stick

**Users:** run `kexmedia.exe` on any Windows XP–11 PC (it asks for administrator
rights). One window: pick the Windows 98 CD (a CD drive, or an .iso file of it with
Browse), pick the USB stick, click *Make the USB stick*. It builds the stick from the
CD and writes and checks it; nothing else is needed (no Linux, no command line).
*Save as .iso file instead* makes an image for a CD or a virtual machine.
Only USB/removable disks are offered, never the system disk, and nothing is written
before the user confirms. The stick needs about 520 MB.

How: `win/mkimage.c` (portable C) reads the CD (ISO 9660, its El Torito boot floppy
with FAT12) and writes the stick: a base hybrid ISO (isolinux + MEMDISK, ReBIOS,
CSMWrap) with an empty 512 MB extent for `KEXSETUP.IMG`, which it fills with the
FAT16 setup disk (the floppy's DOS files, the CD's WIN98 folder, our stage) and then
patches the file's size and MEMDISK's cylinders. Every MB written is read back and
compared (CRC32). Nothing from Microsoft is in the program; it all comes from the CD.

**Building it:** `build_core.py` builds `kexmedia.exe`; `build_usbmaker.sh BUILD_CORE
KEXPREP.EXE KEXSETUP.EXE REBIOS.EFI CSMWRAP.EFI OUT.EXE` appends its payload
(`pack_media.py`: our stage files, the base ISO without its zero runs, our MBR).
`kexmedia /makeiso CD-OR-ISO OUT.ISO` works without a window (scripts, tests).
`tests/media/run.sh` builds a stick on the host from a CD image and checks it.

The Linux prototype (`stage_extra.py`, `proto_image.py`, `make_iso.sh`) does the same
by hand and is kept for development.

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
with `stage/MSBATCH.INF` and `/p i` (no Windows ACPI driver: KEXACPI reads batteries).

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
