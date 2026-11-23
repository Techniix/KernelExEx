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
"More than 1 GB of RAM (memory limit)".

Measured in QEMU (Korean Windows 98 SE, 2 GB machine, Westmere CPU, 2026-11-23), before
the VMM patch below:

| Windows sees | VCACHE limit | Result |
|---|---|---|
| 1024 MB (KEXMEM default) | 256 MB | starts, 9 of 9 boots |
| 1280 MB (KEXMEM /MAX=1280) | 256 MB or 64 MB | "Insufficient memory to initialize Windows" |
| 1536 / 1792 MB | 256 MB | same |

## kexpatch /mem: more than 1 GB (installer options, experimental)

`tools/kexpatch` patches the user's own VMM32.VXD (copy kept as SYSTEM\VMM32.KEX; the new
file goes in at the next start through WININIT.INI). Details in `docs/research/memory.md`:

- ours: memory blocks from the loader (at most 64 MB each, one page apart) are merged, so
  VMM's 16-entry range table no longer caps Windows at 16 x 64 MB = 1 GB;
- from Rudolph R. Loew's PATCHMEM (see CREDITS.md): a stale flag in page-table records
  (crash at ~1.25 GB); more start-up pages with the record pool moved and enlarged
  (needed above ~2 GB); VCACHE's ceiling lowered to 512 MB.

Installer components: "Use up to 2 GB of RAM" (KEXMEM `/MAX=2048`) and "Use all RAM, up
to about 3.5 GB" (`/MAX=4095`); both also give HIMEM `/NUMHANDLES=128`. Tested in QEMU at
2048, 3072 and 3560 MB. Safe Mode skips CONFIG.SYS, so above 1 GB it needs
"Step-by-step confirmation" with KEXMEM loaded (not tested).

**If Windows no longer starts:** boot the install stick. When it finds a patched Windows it
offers to undo the patch (press U): `KEXUNDO.COM` copies VMM32.KEX back and removes `/MAX=`
from the KEXMEM line, so Windows is limited to 1 GB again. By hand: `KEXUNDO D:`.
