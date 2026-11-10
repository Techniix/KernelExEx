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
    driver, a Control Panel applet and a tray icon. Carries IPX as well as
    TCP/IP for LAN games.
23. **WiFi with WPA2.** A built-in client (scan, choose, password) on
    wpa_supplicant (BSD). It works with cards whose 98 driver accepts the
    standard WPA key requests; a list of known-good cards is kept.
24. **7-Zip** built against KernelExEx, with Explorer context menus.

## Installer components

| Component | Contents | Status | Ticked by default |
|---|---|---|---|
| KernelEx core | Core, `kexbases`, `kexbasen`, VxD, Properties tab | builds | always installed |
| Modern TLS and crypto | `kextls`, `winhttp`, `bcrypt`, root certificates | builds | to be decided |
| Vista/7 APIs | `kexrt` | builds | to be decided |
| Clock sync | SNTP client | planned | to be decided |
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
| WiFi client | WPA/WPA2 on wpa_supplicant | planned | to be decided |
| 7-Zip | archiver with Explorer menus | planned | to be decided |
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
