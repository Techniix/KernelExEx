# Display: VESA driver (native resolution without a vendor driver)

`vesamini.drv` + `vesamini.vxd` from [vmdisp9x](https://github.com/JHRobotics/vmdisp9x)
(MIT, Jaroslav Hensl; based on Michal Necasek's driver), built by `build_vesa.sh` with
Open Watcom 2.0 at a pinned commit. It drives any graphics card through its VESA BIOS
(VBE 2.0+), e.g. Intel HD Graphics on Core i3/i5/i7 laptops, at the resolutions the
card's BIOS offers (the Dell Vostro 1440's 1366x768 panel included, if its BIOS lists it).
No 2D/3D acceleration.

Licence: the driver links Open Watcom's runtime (OWPL), so it is shipped as its own MIT
component next to KernelExEx, not linked with KernelExEx's GPL code.

Install by hand: Display Properties > Settings > Advanced > Adapter > Change >
"Have Disk", point to the folder with `kexvesa.inf`, pick the matching entry, restart.

Status: builds; not yet tested on hardware.
