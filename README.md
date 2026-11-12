# KernelExEx hardware test builds

Test builds for real Windows 98 SE / ME machines. They are not releases. Each zip has a README.TXT with
the install and test steps and what to send back (for network drivers: `C:\WINDOWS\KEXNET.LOG`).

Each build links to the `main` commit it was built from. The newest build is at the top.

| Build | Zip | Source | What's new |
|---|---|---|---|
| 10 | [KernelExEx-1.0.0-testbuild10.zip](KernelExEx-1.0.0-testbuild10.zip) | [9a62e54](https://github.com/Techniix/KernelExEx/commit/9a62e541ac61976922ae5f6212964bd31e2b76e4) | Fixes from the first test on the MSI 848P Neo2-V: RTL8139 driver no longer claims shared interrupts (boot hang, Code 11); AC'97 start diagnostics in the registry (Code 10). |
| 9 | [KernelExEx-1.0.0-testbuild9.zip](KernelExEx-1.0.0-testbuild9.zip) | [e16e7be](https://github.com/Techniix/KernelExEx/commit/e16e7befa5021a7529562683ed575809735021ba) | Disks over 128 GB (own IDE/ATA driver with 48-bit LBA), see step 14; chipset INFs now set up the IDE channels (step 11, please redo); first build under the 1.0.0 version scheme. |
| 8 | [KernelExEx-4.7.0-hwtest8.zip](KernelExEx-4.7.0-hwtest8.zip) | [d82a483](https://github.com/Techniix/KernelExEx/commit/d82a483ec55343aeee9dff4e329e3c49ab604a60) | USB 2.0 (EHCI, the ICH5 on the MSI 848P Neo2-V and any other), see step 13; the USB network and Wi-Fi drivers use their USB 2.0 settings on it. |
| 7 | [KernelExEx-4.7.0-hwtest7.zip](KernelExEx-4.7.0-hwtest7.zip) | [48dbba7](https://github.com/Techniix/KernelExEx/commit/48dbba74b5b3ef94a23ec6b968b732c96cd88442) | AC'97 sound (ICH5 + ALC655 on the MSI 848P Neo2-V; Intel, nVidia, AMD), see step 12; every driver rebuilt with the fix for memory Windows 98 leaves uncleared. |
| 6 | [KernelExEx-4.7.0-hwtest6.zip](KernelExEx-4.7.0-hwtest6.zip) | [7ecf72e](https://github.com/Techniix/KernelExEx/commit/7ecf72eea870604fdcd25697425622f06b980791) | For the MSI 848P Neo2-V and similar boards: a driver for the onboard Realtek RTL8100C (every RTL8139-family chip), see step 10, and chipset INFs naming every part of Intel ICH2 to ICH10, see step 11. |
| 5 | [KernelExEx-4.7.0-hwtest5.zip](KernelExEx-4.7.0-hwtest5.zip) | [4357967](https://github.com/Techniix/KernelExEx/commit/43579671a318238bc4c191345c44948c9e30bc2d) | a second Realtek Wi-Fi driver, from FreeBSD (rtwn), for newer adapters including the TP-Link Archer T2U Plus (RTL8821AU), T2U, T2U Nano and other RTL8812AU/8821AU/8188FU/8723BU sticks. It now handles the older Realtek chips too; build 4's driver stays for the few it lacks. |
| 4 | [KernelExEx-4.7.0-hwtest4.zip](KernelExEx-4.7.0-hwtest4.zip) | [df39095](https://github.com/Techniix/KernelExEx/commit/df39095697083e8bfa790b16807d2d71fc116eea) | Wi-Fi for Realtek USB adapters such as the TP-Link TL-WN722N v2/v3 and TL-WN725N (OpenBSD's urtwn driver and wireless stack, WPA2), see step 8; USB CDC Ethernet adapters, see step 9; and every network driver now writes its messages to C:\WINDOWS\KEXNET.LOG. |
| 3 | [KernelExEx-4.7.0-hwtest3.zip](KernelExEx-4.7.0-hwtest3.zip) | [ab5b6d7](https://github.com/Techniix/KernelExEx/commit/ab5b6d7dc105fa15c580bbc21aced04769d7deaa) | USB networking, including USB tethering from an Android phone (OpenBSD's urndis driver), see step 7. |
| 2 | [KernelExEx-4.7.0-hwtest2.zip](KernelExEx-4.7.0-hwtest2.zip) | [cee005b](https://github.com/Techniix/KernelExEx/commit/cee005b30d3e754e775816507e9a73b37245867b) | a driver for Intel PRO/1000 network adapters (OpenBSD's em driver running on Windows 98), see step 6. |
| 1 | [KernelExEx-4.7.0-hwtest1.zip](KernelExEx-4.7.0-hwtest1.zip) | [5004ae3](https://github.com/Techniix/KernelExEx/commit/5004ae33a0af256b360d03672f6f8d13103dc8e3) | KernelExEx core, modern TLS, the Unicode layer and the self-tests. |
