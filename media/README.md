# Install media (USB stick / CD) — work in progress

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
