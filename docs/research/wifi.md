# WiFi on Windows 98 SE / ME: newer drivers, or Linux's

## The problem

Windows 98 has no WiFi stack of its own. A 98-era card's driver is an NDIS
Ethernet driver with private extras, and its vendor utility does the
association. WPA2 needs the 4-way handshake (EAPOL) and AES-CCMP keys.
Windows XP SP2 standardised the requests for that (the NDIS 5.1
`OID_802_11_*` set); 98 drivers mostly predate it, and modern adapters ship
no 98 driver at all.

So relying on vendor drivers caps us at a handful of cards. To support many
adapters "out of the box" we have to bring the drivers ourselves.

## Options

| # | Approach | Adapters | Effort | Verdict |
|---|---|---|---|---|
| 1 | wpa_supplicant on top of vendor 98 drivers | the few with NDIS 5.1 WPA requests | small | keep as a bonus path |
| 2 | Port Linux drivers (mac80211 + cfg80211 + a chip driver) into an NDIS miniport through a Linux-API shim | everything Linux supports, in principle | very large: mac80211 assumes the whole Linux kernel (RCU, workqueues, skbuffs, NAPI, firmware loader, crypto API) | reference, not the base |
| 3 | Port BSD drivers + their 802.11 layer (`net80211`) through a shim, as Haiku does with FreeBSD's | the main open chipsets | large but bounded: net80211 plus one driver is tens of thousands of lines, with a small OS interface | **chosen** (decided 2026-11-10) |
| 4 | Linux on another core owns the adapter (the multikernel split in `gpu.md`) and forwards Ethernet frames | anything Linux supports | rides on the GPU project | later, for multi-core machines |
| 5 | Run Linux drivers unmodified inside a tiny Linux kernel in a VxD | everything | enormous, fragile | no |

### Why the BSD route first

- **Proven pattern.** Haiku has run FreeBSD's WiFi drivers through a
  compatibility layer for years. The OS interface such a driver needs is
  small: memory, locks, timers, a taskqueue, `mbuf`s, bus DMA and USB
  transfers, and firmware loading.
- **WPA2 in software.** OpenBSD's `net80211` does WPA/WPA2-PSK itself (the
  handshake and CCMP), so no supplicant or card support is needed. FreeBSD's
  pairs with wpa_supplicant (BSD licence). Either way the 98 driver only has
  to send and receive raw 802.11 frames.
- **Licences fit.** ISC/BSD drivers can be combined with GPLv2. Linux
  drivers are GPLv2 too and can be read and ported where BSD lacks a chip.
- **On 98 it is an NDIS 5 miniport `.sys`.** NDIS on 98 SE loads WDM
  miniports, so the result shows up as an ordinary network adapter: TCP/IP,
  IPX and file sharing bind to it as to any Ethernet card. The WiFi client
  talks to it through private requests.

### First chipsets (open, firmware redistributable or not needed)

| Chipset | Bus | OpenBSD / FreeBSD | Linux | Why |
|---|---|---|---|---|
| Atheros AR9271 | USB | `athn(4)` | `ath9k_htc` | cheap dongles everywhere; open firmware |
| Atheros AR92xx/AR93xx | PCI, PCIe, mini-PCI | `athn(4)` | `ath9k` | no firmware; fits period laptops and desktops |
| Ralink RT2870/RT3070/RT5370 | USB | `run(4)` | `rt2800usb` | very common dongles |
| Realtek RTL8188CUS/RTL8192CU/RTL8188EU | USB | `urtwn(4)` | `rtl8xxxu` | the most common nano dongles |
| Intel 7260 and later | PCIe | `iwm(4)` / `iwx(4)` | `iwlwifi` | modern laptops; firmware is redistributable |

USB adapters depend on our USB work (`usbstor.sys` shares the same
groundwork, and xHCI for modern boards); PCI/PCIe cards do not.

### Layers

```
 WiFi client (Control Panel + tray)     scan, choose network, password
        | private NDIS requests
 kexwifi.sys  NDIS 5 miniport (one per adapter)
   +- 802.11 layer (OpenBSD net80211: scan, associate, WPA2 in software)
   +- chip driver (athn / run / urtwn / iwm), unchanged where possible
   +- BSD shim: mbuf, malloc, mutex, timeout, taskq, bus_dma, usbd_* -> WDM/USBD, firmware from \WINDOWS\SYSTEM\kexwifi
```

## Plan

1. Shim and `net80211` building with our WDM toolchain; host tests for the
   handshake and CCMP against known vectors.
2. `run(4)` or `athn(4)` USB first: USB transfers are easier to get right than
   PCI DMA, and one dongle covers testing on 86Box (USB passthrough) and real
   hardware.
3. NDIS miniport glue, then the client and the installer component.
4. PCI `athn(4)`, then `urtwn(4)` and `iwm(4)`.
5. Option 1 (wpa_supplicant over vendor drivers) as an extra for cards we
   do not cover.


## Port status (2026-11-11)

**Stack:** OpenBSD net80211 (master) and the crypto it needs (sha1, sha2,
hmac, md5, aes, rijndael, arc4, key_wrap, cmac, michael) are vendored in
`drivers/kexnet/wifi/`, unmodified, and compile and link into a 98 SE
driver (`kexwlan_linktest.sys`, 198 KB, imports only kexbsd/ntoskrnl/ndis/
hal). Licences: net80211 ISC / 2-clause BSD; crypto public domain, ISC,
MIT-style (aes.c, Thomas Pornin) or 3-clause BSD (sha2). OpenBSD's
`net/if_media.h` is 4-clause (Stone/Thorpe), so the IEEE 802.11 media words
are our own definitions (kexobsd_net.h); nothing outside the driver
depends on their values.

**What net80211 gives:** scanning, association, WPA/WPA2-PSK with the
EAPOL 4-way and group handshakes in the kernel (no supplicant needed for
home networks), CCMP/TKIP, rate control (AMRR, RA for 11n), and 802.11 to
Ethernet conversion, so NDIS sees an ordinary Ethernet adapter through the
existing glue.

**Kernel randomness:** 98 has no kernel RNG, and WPA nonces must be
unpredictable: `wifi/kex80211_rand.c` is a SHA-256 hash DRBG seeded from
RDSEED/RDRAND when present, TSC jitter across 64 timer ticks, system time
and the performance counter, stirred on every request, state forwarded
after every request.

**Configuration:** network name and passphrase are the adapter's Advanced
properties (NetworkName, Passphrase, AllowTKIP; read at initialize, PSK
from the passphrase with PBKDF2-HMAC-SHA1 in the driver, as `ifconfig`
does on OpenBSD; WPA2/CCMP only unless AllowTKIP is set). Next is a Control Panel applet with a scan list, writing the same
values and asking the driver to reassociate (private OID).

**Firmware and file access at boot:** on 98 SE, a driver's ZwCreateFile
deadlocks while its device start is still in progress: from
MiniportInitialize, from another thread, and from the driver thread just
after initialize has returned. It works from the check-for-hang timer
onwards (tested: `\SystemRoot\KEXNET.LOG` written, `8188EU.FW` read in
full, boot completes). So radio drivers, which load firmware when the
interface comes up, are brought up from the fifth check-for-hang beat
(about 10 s after initialize); until then the adapter reports
disconnected. Firmware is looked for in `SYSTEM\KEXFW\` under the long
OpenBSD name, then as 8.3 (`urtwn-rtl8188eu` -> `8188EU.FW`).

**Driver log:** every adapter writes the driver message ring to
`C:\WINDOWS\KEXNET.LOG` every 10 s (overwritten), so a test on real
hardware can be read back without a debugger.

**Radio drivers (ISC, all by or derived from Damien Bergamini):**

| Driver | Chips | Typical adapters | Firmware |
|---|---|---|---|
| urtwn + ic/rtwn | RTL8188CUS/CE, RTL8188EU, RTL8192CU | most cheap "nano" dongles (pre-2018), TP-Link TL-WN725N v2/v3 | Realtek blobs, redistributable binary |
| run | RT2870, RT3070/3071/3072, RT5370/5372 | many 2009-2016 dongles, popular in retro setups | rt2870.bin / rt3070, Ralink/MediaTek redistributable |
| athn (USB) | AR9271, AR7010+AR9280 | TP-Link TL-WN722N v1, Alfa AWUS036NHA | open-ath9k-htc-firmware: open source (ClearBSD/ISC) |
| rsu | RTL8188SU/8191SU/8192SU | older Realtek | Realtek blob |
| otus | AR9170 | old Netgear WNDA3100 | Atheros, redistributable |

Firmware is shipped as separate files next to the driver (mere
aggregation, as linux-firmware is), and is only what each vendor allows
redistributing. None of this can be tested in QEMU (no emulated USB
Wi-Fi); it needs real adapters.
