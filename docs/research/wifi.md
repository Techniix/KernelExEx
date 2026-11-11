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
