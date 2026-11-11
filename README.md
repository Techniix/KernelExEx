# KernelExEx hardware test builds

Test builds for real Windows 98 SE / ME machines. They are not releases. Each zip has a README.TXT with
the install and test steps and what to send back (for network drivers: `C:\WINDOWS\KEXNET.LOG`).

Each build's source is tagged `hwtestN` on `main`. The newest build is at the top.

| Build | Zip | Source | What's new |
|---|---|---|---|
| 5 | [KernelExEx-4.7.0-hwtest5.zip](KernelExEx-4.7.0-hwtest5.zip) | `hwtest5` (4357967) | a second Realtek Wi-Fi driver, from FreeBSD (rtwn), for newer adapters including the TP-Link Archer T2U Plus (RTL8821AU), T2U, T2U Nano and other RTL8812AU/8821AU/8188FU/8723BU sticks. It now handles the older Realtek chips too; build 4's driver stays for the few it lacks. |
| 4 | [KernelExEx-4.7.0-hwtest4.zip](KernelExEx-4.7.0-hwtest4.zip) | `hwtest4` (df39095) | Wi-Fi for Realtek USB adapters such as the TP-Link TL-WN722N v2/v3 and TL-WN725N (OpenBSD's urtwn driver and wireless stack, WPA2), see step 8; USB CDC Ethernet adapters, see step 9; and every network driver now writes its messages to C:\WINDOWS\KEXNET.LOG. |
| 3 | [KernelExEx-4.7.0-hwtest3.zip](KernelExEx-4.7.0-hwtest3.zip) | `hwtest3` (ab5b6d7) | USB networking, including USB tethering from an Android phone (OpenBSD's urndis driver), see step 7. |
| 2 | [KernelExEx-4.7.0-hwtest2.zip](KernelExEx-4.7.0-hwtest2.zip) | `hwtest2` (cee005b) | a driver for Intel PRO/1000 network adapters (OpenBSD's em driver running on Windows 98), see step 6. |
| 1 | [KernelExEx-4.7.0-hwtest1.zip](KernelExEx-4.7.0-hwtest1.zip) | `hwtest1` (5004ae3) | KernelExEx core, modern TLS, the Unicode layer and the self-tests. |
