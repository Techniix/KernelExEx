# KernelExEx

KernelExEx is a continuation of [KernelEx](https://sourceforge.net/projects/kernelex/),
the open-source compatibility layer that lets Windows 98 and Windows ME run
programs written for Windows 2000, XP and later. It starts from KernelEx 4.5.2
(Xeno86, 2011) and adds what a Windows 9x machine needs to be useful today:
modern encryption and TLS built into the operating system, newer Windows APIs,
and (planned) modern controllers, per-program audio, USB headsets, a retro-web
setup and Discord presence.

**Status: early test builds. Nothing here has run on Windows 9x yet, in an
emulator or on real hardware.** The code builds, passes static checks against
real Windows 98 SE system files and passes its test suite on Linux. Expect
bugs; back up before installing.

## What it does today

| Area | What you get |
|---|---|
| Base | Everything in KernelEx 4.5.2: API extensions for KERNEL32, USER32, GDI32, ADVAPI32, SHELL32 and others, per-program compatibility modes, in-memory kernel patching (no system files modified on disk) |
| TLS | `kextls.dll`: TLS 1.2 and 1.3 (Mbed TLS 3.6), ECDHE with AES-GCM and ChaCha20-Poly1305, Mozilla root store, full certificate and host name checking |
| Random numbers | A real CSPRNG (HMAC-DRBG) behind `RtlGenRandom` and `rand_s`. Stock KernelEx used a predictable generator |
| WinHTTP | `winhttp.dll`: synchronous WinHTTP with HTTPS, redirects, proxies (including the Internet Explorer proxy settings), chunked transfer |
| CNG | `bcrypt.dll`: `BCryptGenRandom`, MD5, SHA-1, SHA-2, HMAC |
| Vista/7 APIs | `kexrt.dll`: SRW locks, condition variables, `InitOnce*`, `Fls*`, `GetTickCount64`, `InitializeCriticalSectionEx`, `Create{Event,Mutex,Semaphore}Ex`, `inet_pton`, `inet_ntop`, `WSAPoll` and others |
| Installer | Optional (unticked) setting that sends plain HTTP and FTP through the [Protoweb](https://protoweb.org) proxy to browse restored 1990s websites. HTTPS is not proxied. Uninstall restores the previous proxy settings |
| Build | Everything builds on Linux with open-source tools: the KernelEx core and API libraries, the property sheet, `kexCOM`, the auxiliary DLLs, the `VKrnlEx.vxd` kernel driver, the new modules and the installer. No Microsoft compiler, DDK or Wine needed |
| Developer docs | Porting guide and a generated inventory of the 968 functions in 25 DLLs that KernelExEx adds or replaces |
| Checks | `util/patcheck` verifies KernelEx's kernel32 patch patterns against any `kernel32.dll` offline. `net/tools/pecheck.py` verifies each binary imports only functions Windows 98 SE has and uses only Pentium instructions. The build fails on two clang miscompiles of the old inline assembly (see `toolchain/build_core.py`) |

How the kernel driver is built: clang compiles it, lld links it as an
ordinary PE file, and `toolchain/vxd/le_link.py` converts that into the LE
format Windows 9x drivers use. Every build checks that the converted driver,
loaded the way Windows loads it, is byte-for-byte the same as the original.
The VxD definitions in `toolchain/vxd/include` are written from public
documentation, not copied from Microsoft's DDK.

### Security rules

The TLS stack will not be made "compatible" by weakening it. It never
disables certificate validation, accepts invalid certificates, ignores host
name mismatches, falls back below TLS 1.2 or enables obsolete ciphers.
Programs that ask WinHTTP to ignore certificate errors are refused.

Windows 9x itself has no memory protection between programs, no user
accounts and no security updates. Encrypted connections protect data in
transit; they do not make the machine safe to use for banking or anything
else you care about.

## Decisions

These settle how the planned work is done. They change only by editing this
list.

### Platform and build

1. **Targets.** Windows 98 SE and Windows ME. Windows 98 first edition and
   Windows 95 are not supported.
   Every release is tested in 86Box and on real hardware before it is tagged.
2. **One toolchain.** clang and lld on Linux (`toolchain/`, `net/tools/`) is
   the only maintained build. The Visual C++ 6 project files stay in the tree
   for reference and are not kept working.
3. **CPU.** Code is built for the Pentium (i586): no CMOV, MMX or SSE in
   shared code, so it runs on any Pentium-class CPU. Where speed matters
   (audio mixing, cryptography), a faster path is chosen at run time with
   CPUID.
4. **One API library per feature.** Each new feature is its own KernelEx API
   library or DLL, so it can be left out at install time and switched off per
   program on the KernelEx tab. Because KernelExEx ships its own core, the
   API library limit some people hit with Kex22 does not apply.
5. **Drop-in replacement.** KernelExEx installs over KernelEx 4.5.2 and reads
   the same `core.ini` and per-program settings. `kexGetKEXVersion` reports
   4.7.x, so programs that check for "4.5.2 or later" keep working and the
   number does not clash with the 4.6 betas of other forks.
6. **Licence.** GPL version 2 only, like KernelEx. Code can be taken from
   projects under GPLv2, LGPL 2.1 or later, MIT, BSD, zlib or ISC. That
   includes the Linux kernel (GPLv2), so Linux drivers can be studied and
   ported. Apache-2.0 and GPLv3-only code cannot be used. Separate programs we
   bundle (RetroZilla) stay under their own licences. Community drivers
   without a clear licence are not bundled; the installer links to them.

### Input

7. **Gamepads in three layers.**
   1. *Drivers* make a pad visible to Windows as a game controller. Pads that
      present as standard USB HID (most DirectInput-mode pads) need nothing.
      Xbox 360-protocol pads (most 8BitDo, many third-party pads) get a new
      WDM driver. Xbox One/Series protocol follows later. Support is grouped
      by protocol, not by brand. Reference: Linux `xpad`, `hid-sony`,
      `hid-playstation`, `hid-nintendo` (all GPLv2).
   2. *XInput*: `xinput1_1` to `xinput1_4` and `xinput9_1_0`, on top of the
      Windows 9x joystick and DirectInput interfaces, so games written for
      Xbox controllers work with any pad.
   3. *Gamepad Configurator*: button mapping, dead zones, a test screen and
      per-game profiles. Mappings are stored in SDL's `gamecontrollerdb.txt`
      format, so they work with SDL programs and can be shared with the
      community database.

   First test device: 8BitDo Ultimate 2C.

### Audio

8. **Per-program mixer.** A KernelEx API library replaces `waveOut*` and
   DirectSound for the programs it is enabled for and sends their sound to
   one mixer process that owns the sound card. That gives a volume and mute
   control per program and lets several programs play at once on cards that
   allow only one. Games that need direct hardware access (DirectSound3D in
   hardware, EAX) can be excluded per program. A tray icon opens the mixer.
9. **USB headsets and microphones.** Most USB headsets are USB Audio Class 1,
   which the Windows 98 SE `usbaudio.sys` driver can in principle run. Step
   one is a tested-device list from real hardware. Step two is our own USB
   audio driver (Class 1 and 2, playback and microphone, headset buttons),
   using Linux `snd-usb-audio` as the reference. Bluetooth is out of scope.

### Online

10. **Networking stays strict.** HTTPS is never routed through Protoweb or any
    other proxy that the user did not set up.
11. **Clock sync.** A small SNTP client keeps the clock right, because
    certificate checks fail when the date is wrong.
12. **Root certificate updates** are downloaded over TLS and accepted only
    with a valid KernelExEx signature.
13. **Browser.** RetroZilla is an optional component. It ships as stock
    RetroZilla with Protoweb settings, unless Protoweb confirms that the
    Flash Player in their edition may be redistributed.
14. **Discord presence.** A tray agent on the Windows 9x machine detects the
    running game and reports it over the LAN to a companion program on a
    modern PC (Windows, macOS or Linux, one binary each, written in Go), which
    sets Discord Rich Presence.
    - Games are recognised from the process list by executable name and file
      size, matched against a game list the user can edit, with the window
      title as a fallback.
    - The agent sends only the game title and start time.
    - The two sides pair with a code shown on screen and talk only on the
      local network.

### Graphics, SDL and research

15. **SDL.** SDL 1.2, SDL2 and SDL3 are built for Windows 9x with our
    toolchain. SDL3 is a maintained port, not a stack of API shims: DirectSound
    audio, Direct3D 9 or GDI video, DirectInput plus our XInput. KernelExEx
    never overrides an SDL DLL that a program ships itself. The builds go in
    the developer SDK and in an optional runtime component.
16. **GPU research** starts once the core runs on Windows 9x. Findings go in
    `docs/research/gpu.md`. Two directions are open: GPU emulation without a
    hypervisor, and using Linux GPU drivers on Windows 9x. Anything that
    needs a hypervisor becomes a separate project.
17. **Other modern hardware**, studied before anything is promised:
    - USB 3 (xHCI) host controllers, since many modern boards have no older
      USB controller;
    - USB mass storage (now planned: see decision 21);
    - AHCI/SATA, disks over 137 GB and more than 512 MB of RAM;
    - Intel HD Audio, the onboard sound on modern boards (Linux
      `snd-hda-intel` as reference);
    - modern network cards;
    - file sharing with modern PCs, which have SMB1 turned off.

    Existing community fixes are linked from the installer when their licence
    does not allow bundling.

### Documentation

18. **Porting guide.** `docs/PORTING.md` tells developers and AI agents how
    to build programs that run on Windows 9x with KernelExEx: compiler and
    linker settings, PE rules, what to avoid, and how to detect KernelExEx at
    run time. `docs/API.md` and `docs/api.json` list every API KernelExEx
    adds. They are generated from the API tables, so they cannot go stale,
    and the build fails if they are out of date.

### Installer

19. **Every optional feature is a checkbox** on the installer's components
    page; only the core is mandatory. Features that act per program can also
    be switched per program on the KernelEx tab of a program's Properties.
    Which boxes are ticked by default will be settled before the first
    release.
20. **No Microsoft redistributables required.** MSLU is no longer offered
    for download, so `kexuni.dll` implements the 208 functions KernelEx
    forwarded to it. A real `unicows.dll`, if installed, is still preferred.
21. **USB mass storage** with our own `usbstor.sys` (Linux `usb-storage` as
    reference), on 98 SE's own WDM-to-IOS bridge (`ntmap.sys`). No Windows ME
    files are copied.
22. **Virtual LAN (ZeroTier).** The ZeroTier client (MPL-2.0 since 1.16; the
    commercial network controller is not used) with a virtual network card
    driver, a Control Panel applet and a tray icon. IPX is a first-class
    citizen: setup binds Windows' IPX/SPX protocol to the virtual adapter,
    and the applet checks that the network's rules let IPX (EtherType
    0x8137) through and shows the exact rule to add when they do not.
23. **WiFi with WPA2, on our own drivers.** Windows 98 drivers for WiFi cards
    mostly cannot do WPA2, so KernelExEx brings its own: one NDIS miniport
    that hosts open WiFi drivers through a compatibility layer, the way
    Haiku runs FreeBSD's. WPA2 is done in software inside it, so the card's
    capabilities do not matter. See `docs/research/wifi.md`. A built-in
    client (scan, choose, password) sits on top.
24. **7-Zip** built against KernelExEx, with Explorer context menus.
25. **File sharing with modern PCs**: an SMB2/3 client, so Network
    Neighborhood and mapped drives reach Windows 10/11, Samba and NAS boxes
    that have SMB1 turned off. Signing and encryption on, as modern servers
    require.
26. **Disks over 137 GB and AHCI/SATA**: 48-bit LBA in our own IDE port
    driver and an AHCI driver, with FAT32 tools that handle large volumes.
27. **More than 512 MB of RAM**: our own fix for the VCACHE and memory
    manager limits, in VKrnlEx.vxd or a companion VxD.
28. **Intel HD Audio** driver for onboard sound, with Linux `snd-hda-intel`
    as the reference.
29. **FTP with TLS (FTPS) and SFTP** clients on the KernelExEx TLS stack.
30. **Bluetooth**, HID first: a USB Bluetooth adapter driver, the L2CAP and
    HID profiles and secure pairing, so pads (8BitDo), keyboards and mice
    connect without cables. Audio (A2DP) comes after.
31. **Update service**, last: root certificates, controller database and
    components.

32. **BSD drivers through one compatibility layer**, the same one the WiFi
    drivers use: USB 3 host controllers (FreeBSD `xhci`), network cards
    (FreeBSD `re` for Realtek 8111/8168, `em`/`igb` for Intel), Intel HD
    Audio (FreeBSD `snd_hda`, OpenBSD `azalia`), SATA/AHCI and NVMe (FreeBSD
    `ahci`, `nvme`), Bluetooth (NetBSD's stack: HCI, L2CAP, HID) and ACPI
    power-off and sleep (ACPICA, dual BSD/GPL). FreeBSD `umass` is the
    reference for `usbstor.sys`. Each is its own installer checkbox.
33. **Device Manager tells the truth.** Every KernelExEx driver is installed
    from an INF with the right device class, provider "KernelExEx" and a
    name that says what drives the device (for example "Realtek RTL8168
    Gigabit Ethernet (KernelExEx re)"). Driver files carry version
    resources, so Driver File Details shows file, version and origin.
    Failures reach Windows as Plug and Play problem codes, so the device
    shows the usual yellow mark and status text. Each driver also writes a
    short log to `%WINDIR%\KernelEx\logs` with the hardware IDs, chip
    revision, firmware loaded and the reason for any failure.
34. **Extra file systems, optional**: NTFS and ext2/3 drives, read-only first
    (NetBSD `ntfs`, FreeBSD `ext2fs`), so 98 can open drives from modern PCs.
35. **Clock sync** on OpenNTPD (ISC), and signed root-certificate updates
    with OpenBSD `signify` (ISC).
36. **SMB2/3** on libsmb2 (LGPL-2.1), see decision 25.
37. **libarchive** (BSD-2) for zip, tar, ISO and 7z reading, including
    archives opened as folders in Explorer; **zstd** (BSD) alongside 7-Zip.

38. **Command-line tools** (optional component, all GPLv2-compatible):
    BusyBox for Windows (`busybox-w32`, GPLv2: ls, grep, sed, awk, tar,
    find, diff and more), curl on the KernelExEx TLS stack, the 7-Zip and
    zstd command-line tools, OpenSSH `ssh`/`scp`/`sftp`, Info-ZIP
    `zip`/`unzip`, Lua, and two editors (Vim, OpenBSD `mg`).
39. **For era games and enthusiasts** (each optional):
    - ISO/CUE virtual CD-ROM drive (mount images from Explorer);
    - software MIDI synthesizer with SoundFonts (FluidSynth, LGPL), for
      sound cards with no hardware MIDI;
    - Sound Blaster emulation for DOS games in a DOS box on modern audio;
    - Glide wrapper (OpenGlide, LGPL) for 3dfx-only games;
    - gamepad-to-keyboard mapping for games without joystick support;
    - CPU idle (HLT) driver to keep modern CPUs cool, and a slowdown tool
      for speed-sensitive old games;
    - screenshot and video capture.
40. **More hardware, from BSD drivers** (via decision 32's layer):
    chipset identification INFs generated from the PCI ID database, so
    Device Manager names every device; USB 2 (EHCI); SD card readers
    (`sdhci`); PCI serial and parallel cards (`puc`); SMBus and temperature
    sensors (`ichsmb`, `coretemp`, `amdtemp`) with a small hardware monitor.
41. **Graphics**: a universal framebuffer display driver (VESA VBE, and a
    UEFI GOP handover for machines without legacy BIOS) for any modern
    card at native resolution in 32-bit colour, 2D only. 3D acceleration on
    modern GPUs follows `docs/research/gpu.md`; BSD has no GPU drivers of
    its own (it ports Linux's).

## Installer components

| Component | Contents | Status | Ticked by default |
|---|---|---|---|
| KernelEx core | Core, `kexbases`, `kexbasen`, VxD, Properties tab | builds | always installed |
| Modern TLS and crypto | `kextls`, `winhttp`, `bcrypt`, root certificates | builds | to be decided |
| Vista/7 APIs | `kexrt` | builds | to be decided |
| Clock sync | `kexntp.exe`: several servers, median, sanity checks | builds, in installer | to be decided |
| Root certificate updates | signed updater | planned | to be decided |
| Protoweb proxy | IE proxy for HTTP and FTP | in installer script | no (decided) |
| RetroZilla | browser with Protoweb settings | planned | to be decided |
| XInput | `xinput1_1`-`1_4`, `xinput9_1_0`, controller database | builds, in installer | to be decided |
| Xbox controller driver | `kexpad.sys`, 360 protocol (173 pads); later One/Series | builds, in installer; untested on hardware | to be decided |
| Gamepad Configurator | mapping, test, profiles | planned | to be decided |
| Per-program mixer | audio API library, mixer, tray icon | planned | to be decided |
| USB audio driver | headsets and microphones | research | to be decided |
| SDL runtimes | SDL 1.2, SDL2, SDL3 | planned | to be decided |
| Discord presence | tray agent (companion installed on the modern PC) | planned | to be decided |
| Unicode layer | `kexuni.dll` (MSLU replacement) | builds, in installer | always installed |
| USB mass storage | `usbstor.sys` | planned | to be decided |
| Virtual LAN | ZeroTier client, Control Panel applet, virtual adapter | planned | to be decided |
| WiFi | our WiFi drivers (WPA2 in software) and client | research | to be decided |
| 7-Zip | archiver with Explorer menus | planned | to be decided |
| SMB2/3 client | file sharing with modern PCs | planned | to be decided |
| Large disks | 48-bit LBA, AHCI | planned | to be decided |
| Memory fix | more than 512 MB of RAM | planned | to be decided |
| HD Audio | Intel HD Audio driver | planned | to be decided |
| FTPS / SFTP | file transfer clients | planned | to be decided |
| Bluetooth | adapter driver, HID, later audio | planned | to be decided |
| Updates | update service | planned, last | to be decided |
| USB 3 (xHCI) | FreeBSD xhci | planned | to be decided |
| Network cards | FreeBSD re, em, igb | planned | to be decided |
| SATA/AHCI, NVMe | FreeBSD ahci, nvme | planned | to be decided |
| Bluetooth stack | NetBSD bluetooth | planned | to be decided |
| ACPI power | ACPICA | planned | to be decided |
| NTFS / ext2 read | NetBSD ntfs, FreeBSD ext2fs | planned | to be decided |
| libarchive, zstd | archive formats | planned | to be decided |
| Command-line tools | BusyBox, curl, 7z, zstd, ssh, zip, Lua, editors | planned | to be decided |
| Game extras | virtual CD, MIDI synth, SB emulation, Glide, pad-to-keys, idle, capture | planned | to be decided |
| More hardware | chipset INFs, EHCI, SD, serial, sensors | planned | to be decided |
| Framebuffer display driver | VBE / GOP | planned | to be decided |
| Developer SDK | headers, import libraries, porting guide | planned | to be decided |

## Requirements

- Windows 98 SE or Windows ME.
- Pentium-class CPU or newer.
- A correct system clock (certificates cannot be checked against a wrong date).

## How to use it

### Test package (current)

Until a full installer is built, test builds are published as
`kexnet-x.y.zip`. Each has a `README.TXT` with exact steps. In short:

**Portable test, no KernelEx needed.** Copy the `portable` folder to the
Windows 98 machine and run, in an MS-DOS prompt:

```
kexnet_test selftest
kexnet_test get https://www.example.com/
```

**Installed into an existing KernelEx 4.5.2.**

1. Back up `C:\WINDOWS\KernelEx\core.ini`.
2. Copy the package's `KernelEx` folder contents into `C:\WINDOWS\KernelEx\`.
3. Run `kexnet-install.reg`.
4. In `core.ini`, section `[DCFG1]`, change the line to
   `contents=std,kexbases,kexbasen,kextls,kexrt`
5. Restart Windows and run `kexnet_test selftest`.

If Windows will not start, boot in Safe Mode (KernelEx does not load there)
and restore `core.ini`.

### Full installer

`KernelExEx-setup.exe` installs everything: it replaces KernelEx 4.5.2 if
present, and the uninstaller removes it again. Run it, follow the pages,
restart. Test it in 86Box (with a disk snapshot) before real hardware.

### Per-program settings

As in KernelEx: right-click a program or shortcut, choose Properties, and use
the KernelEx tab to pick a compatibility mode or turn KernelEx off for that
program.

### For developers

Programs can simply call `WinHttp*`, `BCrypt*` or the new KERNEL32 functions
as they would on Windows 7. `net/kextls/include/kextls.h` is a small direct
API for TLS client connections. [`docs/PORTING.md`](docs/PORTING.md) is the
porting guide (build settings, limits, run-time detection, a checklist for AI
agents); [`docs/API.md`](docs/API.md) and [`docs/api.json`](docs/api.json)
list every function KernelExEx adds or replaces, generated from the sources.

## Building (Linux)

Needs clang, lld and the LLVM tools (LLVM 18 tested), g++ (for one host
tool) and Python 3.

```sh
git clone --depth 1 https://github.com/mingw-w64/mingw-w64 ../mingw-w64
python3 net/tools/prepare_sdk.py ../mingw-w64 ../build/sdk

# KernelEx core, API libraries, Properties tab, auxiliary DLLs
python3 toolchain/build_core.py --sdk ../build/sdk --out ../build/core

# TLS, WinHTTP, CNG, kexrt
python3 net/tools/build_win9x.py --sdk ../build/sdk --out ../build/win9x

# static checks against Windows 98 SE
python3 net/tools/pecheck.py net/tools/exports/win98se --shared-arena \
    --also toolchain/optional_imports.txt ../build/core/*.dll ../build/core/auxiliary/*.dll
python3 net/tools/pecheck.py net/tools/exports/win98se ../build/win9x/*.dll

# tests (need openssl and python3-cryptography)
net/tests/host/build.sh ../build/host
python3 net/tests/host/run_tests.py ../build/host/test_net

# NSIS for Windows 9x installers (sources: github.com/kichik/nsis, github.com/SCons/scons)
python3 toolchain/nsis/build_nsis.py --nsis-src ../nsis --scons-src ../scons \
    --sdk ../build/sdk --core-lib ../build/core/lib --work ../build/xmingw --prefix ../build/nsis

# Xbox 360-protocol controller driver (WDM, imports checked against 98 SE)
python3 toolchain/wdm/build_kexpad.py --out ../build/kexpad --sdk ../build/sdk

# installer
python3 toolchain/build_installer.py --core ../build/core --net ../build/win9x \
    --kexpad ../build/kexpad --nsis ../build/nsis --out ../build/installer
```

Design notes for the network modules are in `net/docs/ARCHITECTURE.md`.

## Layout

| Path | Contents |
|---|---|
| `core/`, `vxd/`, `apilibs/`, `kexcontrol/`, `sheet/` | KernelEx 4.5.2 core, kernel patcher, API libraries, settings UI |
| `toolchain/` | Linux build: `build_core.py` (core, API libraries, VxD), `build_installer.py`, minimal C++ library, runtime pieces |
| `toolchain/vxd/` | VxD headers, PE-to-LE converter, LE dumper and checker |
| `toolchain/nsis/` | Cross-build of NSIS for Windows 9x installers |
| `toolchain/wdm/` | WDM driver build: `build_kexpad.py`, import libraries with exact names |
| `input/xinput/` | XInput DLL and the SDL-format mapping engine (`kxpad.c`) |
| `input/kexpad/` | HID minidriver for Xbox 360-protocol pads; `gen_inf.py` makes the INF from Linux `xpad` |
| `input/gamepad/` | Controller database shipped with XInput |
| `input/tests/` | Host tests for the mapping engine and the 360 report translation |
| `net/kextls/` | TLS, CSPRNG, trust store |
| `net/winhttp/`, `net/bcrypt/` | WinHTTP and CNG |
| `net/kexrt/` | Vista/7 KERNEL32 and WS2_32 additions |
| `net/rt/` | Small C runtime for Windows 9x targets |
| `net/third_party/mbedtls/` | Mbed TLS 3.6.7 (see `PROVENANCE.md`) |
| `net/certs/` | Mozilla root certificates |
| `net/tests/` | Host tests and the on-device test tool |
| `net/tools/exports/win98se/` | Export tables of the Windows 98 SE system DLLs |
| `util/patcheck/` | kernel32 pattern checker |

## Licence and credits

GPL version 2, as KernelEx (`License.txt`). KernelEx is by Xeno86 and
contributors. Mbed TLS is used under its GPL-2.0-or-later option. Wine and
ReactOS are used as behavioural references. Root certificates come from
Mozilla via certifi.

Not affiliated with Microsoft, Protoweb, Discord or 8BitDo.
