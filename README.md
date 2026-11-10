# KernelExEx

KernelExEx is a continuation of [KernelEx](https://sourceforge.net/projects/kernelex/),
the open-source compatibility layer that lets Windows 98 and Windows ME run
programs written for Windows 2000, XP and later. It starts from KernelEx 4.5.2
(Xeno86, 2011) and adds what a Windows 9x machine needs to be useful today:
modern encryption and TLS built into the operating system, newer Windows APIs,
and (planned) modern controllers, a retro-web setup and Discord presence.

**Status: early test builds. Nothing here has run on real Windows 9x hardware
yet.** The new code builds, passes static checks against real Windows 98 SE
system files and passes its test suite on Linux. Expect bugs; back up before
installing.

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
| Tooling | Builds on Linux with clang. `util/patcheck` verifies KernelEx's kernel32 patch patterns against any `kernel32.dll` offline. `net/tools/pecheck.py` verifies each binary imports only functions Windows 98 SE has and uses only Pentium instructions |

### Security rules

The TLS stack will not be made "compatible" by weakening it. It never
disables certificate validation, accepts invalid certificates, ignores host
name mismatches, falls back below TLS 1.2 or enables obsolete ciphers.
Programs that ask WinHTTP to ignore certificate errors are refused.

Windows 9x itself has no memory protection between programs, no user
accounts and no security updates. Encrypted connections protect data in
transit; they do not make the machine safe to use for banking or anything
else you care about.

## What it is expected to do (roadmap)

Planned, roughly in order. None of this is implemented yet.

1. **System-wide TLS for old programs**: an SChannel provider so Internet
   Explorer, WinINet programs and Outlook Express use the new TLS stack.
   Asynchronous WinHTTP. Validation against
   [OpenCE](https://github.com/Techniix/OpenCE_Win9x).
2. **Modern gamepads**: `xinput1_3.dll` / `xinput9_1_0.dll` on top of the
   Windows 9x joystick layer, a bundled gamepad configurator (mapping, dead
   zones, test screen), then a driver for Xbox-protocol controllers that
   Windows 98 cannot use natively. Target: broad support, starting with
   HID pads and the 8BitDo Ultimate 2C.
3. **Retro web**: optional bundled RetroZilla set up for Protoweb.
4. **Discord presence**: a tray agent that detects running games and reports
   them over the LAN to a companion program on a modern PC, which sets
   Discord Rich Presence. (The official Discord client cannot run on 9x.)
5. **SDL**: keep SDL 1.2 and SDL2 programs working; investigate an SDL3 port.
6. **USB audio**: research into USB Audio Class 2 support.
7. **More APIs**: `GetAdaptersAddresses`, IPv6 name lookup, vectored
   exception handling, `api-ms-win-*` API sets, I/O completion ports.

## Requirements

- Windows 98, 98 SE or ME. Windows 95 is not supported by KernelEx.
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

`KernelEx.nsi` builds the complete installer (KernelEx core plus the new
modules and the optional Protoweb setting). It needs NSIS and the KernelEx
core binaries, which still build with the original Visual C++ 6 projects
(see `COMPILE.TXT`). The updated installer script has not been compiled yet.

### Per-program settings

As in KernelEx: right-click a program or shortcut, choose Properties, and use
the KernelEx tab to pick a compatibility mode or turn KernelEx off for that
program.

### For developers

Programs can simply call `WinHttp*`, `BCrypt*` or the new KERNEL32 functions
as they would on Windows 7. `net/kextls/include/kextls.h` is a small direct
API for TLS client connections.

## Building the new modules (Linux)

Needs clang, lld, llvm-dlltool (LLVM 18 tested) and Python 3.

```sh
cd net
git clone --depth 1 https://github.com/mingw-w64/mingw-w64 ../../mingw-w64
python3 tools/prepare_sdk.py ../../mingw-w64 ../../build/sdk
python3 tools/build_win9x.py --sdk ../../build/sdk --out ../../build/win9x
python3 tools/pecheck.py tools/exports/win98se ../../build/win9x/*.dll

# tests (need openssl and python3-cryptography)
tests/host/build.sh ../../build/host
python3 tests/host/run_tests.py ../../build/host/test_net
```

Design notes are in `net/docs/ARCHITECTURE.md`.

## Layout

| Path | Contents |
|---|---|
| `core/`, `vxd/`, `apilibs/`, `kexcontrol/`, `sheet/` | KernelEx 4.5.2 core, kernel patcher, API libraries, settings UI |
| `net/kextls/` | TLS, CSPRNG, trust store |
| `net/winhttp/`, `net/bcrypt/` | WinHTTP and CNG |
| `net/kexrt/` | Vista/7 KERNEL32 and WS2_32 additions |
| `net/rt/` | Small C runtime for Windows 9x targets |
| `net/third_party/mbedtls/` | Mbed TLS 3.6.7 (see `PROVENANCE.md`) |
| `net/certs/` | Mozilla root certificates |
| `net/tests/` | Host tests and the on-device test tool |
| `util/patcheck/` | kernel32 pattern checker |

## Licence and credits

GPL version 2, as KernelEx (`License.txt`). KernelEx is by Xeno86 and
contributors. Mbed TLS is used under its GPL-2.0-or-later option. Wine and
ReactOS are used as behavioural references. Root certificates come from
Mozilla via certifi.

Not affiliated with Microsoft, Protoweb, Discord or 8BitDo.
