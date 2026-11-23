# Credits

KernelExEx stands on the work of many people. Thank you.

## Rudolph R. Loew (1952-2019)

Rudy Loew's patches kept Windows 95, 98 and ME useful on hardware their makers
never imagined: more than 512 MB and up to 4 GB of RAM (PATCHMEM), disks over
137 GB, partitions over 1 TB, SATA and AHCI controllers, and much more. For many
of us his work was the reason those systems still ran at all.

KernelExEx's "more than 1 GB of RAM" patch (`tools/kexpatch/mempatch.c`) builds on
his PATCHMEM (Windows 9x RAM Limitation Patch, version 7.2, 2007-2017). After his
passing, his family published PATCHMEM's source on the memorial site
(rloewelectronics.com) and said they hope people will build on it, giving
credit to him. The patches marked `[Loew]` in our sources make the same changes
as PATCHMEM's Windows 98 tables; we wrote our own pattern definitions and
tooling, and added our own fixes where we found further problems.

If Rudy Loew's family asks, we will remove this part of KernelExEx (or the whole
project) without delay. Contact us through the project's GitHub issues.

## Others

- **Xeno86 and contributors**: KernelEx, which KernelExEx continues.
- **Jaroslav Hensl (JHRobotics)**: patcher9x (MIT) - our VxD container code's W4
  decompression follows it; vmdisp9x (MIT) - the VESA display driver.
- **mintsuki**: CREGFIX (0BSD), the idea behind KEXMEM's control-register reset.
- The OpenBSD and FreeBSD projects (network and Wi-Fi drivers), Linux (touchpad
  protocols), ACPICA, Mbed TLS, Wine and ReactOS (references), Mozilla/certifi
  (root certificates), syslinux, SeaBIOS/ReBIOS and CSMWrap (install media).

See each component's README for the details and licences.
