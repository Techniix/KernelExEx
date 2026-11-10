# Porting guide: building programs for Windows 98 SE / ME with KernelExEx

This guide is for developers and for AI coding agents. It says what a
program can rely on when it runs on Windows 98 SE or Windows ME with
KernelExEx installed, how to build it, and how to check the result before
anyone boots a real machine.

The exact list of functions is generated from the sources:
[`API.md`](API.md) for people, [`api.json`](api.json) for tools. When this
guide and the inventory disagree, the inventory is right.

## 1. How KernelExEx works, from a program's point of view

* A program built for Windows 2000, XP or later imports functions that
  Windows 98 does not have. Without KernelEx the loader refuses to start it.
* KernelExEx patches the Windows loader in memory. When a module is loaded,
  each import is resolved first against KernelExEx's API libraries
  (`kexbases`, `kexbasen`, `kextls`, `kexrt`, in that order, the later one
  winning) and only then against the system DLL. Nothing on disk is changed.
* Some functionality comes as whole DLLs that Windows 98 lacks: `winhttp.dll`,
  `bcrypt.dll`, `psapi.dll`, `uxtheme.dll`, `userenv.dll`, `wtsapi32.dll`,
  `pdh.dll`, plus KernelExEx's own `kextls.dll`.
* Users can change behaviour per program on the KernelEx tab of its
  Properties: turn KernelEx off, or choose a compatibility mode that reports
  a different Windows version (Windows 2000, XP, Vista and others).

So most programs need no special build at all. This guide matters when a
program fails, or when you build something specifically to run well on
Windows 98.

## 2. Function kinds

Every function in the inventory has one of four kinds:

| Kind | Meaning | What to do |
|---|---|---|
| `new` | Windows 98 SE does not have it; KernelExEx implements it | Use it |
| `replaced` | Windows 98 SE has a version with bugs or missing features; KernelExEx's replaces it | Use it |
| `stub` | Exists only so programs load; returns failure (usually `ERROR_CALL_NOT_IMPLEMENTED`) | Handle the failure; do not depend on it |
| `mslu` | Unicode (`...W`) function forwarded to the Microsoft Layer for Unicode | Works. Uses `unicows.dll` if installed, otherwise KernelExEx's own `kexuni.dll` (always installed) |

## 3. Targets and hard limits

| Area | Rule |
|---|---|
| OS | Windows 98 SE (4.10.2222) and Windows ME (4.90.3000). Not Windows 95 or 98 first edition |
| CPU | Assume a Pentium (i586). Use newer instructions only behind a CPUID check. Windows 98 saves FPU/SSE state, but programs that need AVX will not run (the OS does not enable it) |
| Processors | One. Threads work; they never run in parallel |
| Memory | Plan for 512 MB to 1 GB of RAM and about 2 GB of user address space. The top 2 GB are shared by all programs (the system arena); a bad pointer there can damage other programs |
| Process isolation | Weak. There are no user accounts, no ACLs and no service sandboxing. Never claim a security boundary that Windows 98 cannot provide |
| Unicode | The system is ANSI (one code page). `...W` functions are emulated; file names are still limited to the ANSI code page |
| Files | FAT32: files up to 4 GB, no hard links, no ACLs, no alternate streams |
| Networking | IPv4 only. Winsock 2 is present (98 SE) |
| Services | No Windows NT service control manager |

## 4. Threads and synchronisation

Available (from `kexrt`): slim reader/writer locks, condition variables (with
critical sections and SRW locks), one-time initialisation (`InitOnce*`),
fiber-local storage (`Fls*`), `GetTickCount64`, `InitializeCriticalSectionEx`,
`Create{Event,Mutex,Semaphore}Ex`, 64-bit interlocked compare-exchange.

Not available: thread pools (`CreateThreadpool*`), keyed events, waits on
addresses (`WaitOnAddress`), processor groups.

Implicit thread-local storage (`__declspec(thread)`, `thread_local` in
compilers that use the PE TLS directory) works only in the EXE and in DLLs
loaded at start-up. A DLL loaded later with `LoadLibrary` must use
`TlsAlloc`/`TlsGetValue` or `Fls*`.

## 5. Networking and cryptography

* **HTTPS**: use WinHTTP. KernelExEx's `winhttp.dll` speaks TLS 1.2 and 1.3
  (Mbed TLS) with the Mozilla root store and full certificate and host name
  checking. It is synchronous only: `WINHTTP_FLAG_ASYNC` and status callbacks
  are not supported. Proxy settings come from Internet Explorer's.
* **Certificate policy cannot be relaxed.** Requests to ignore certificate
  errors (`SECURITY_FLAG_IGNORE_*`) are refused, and protocols below TLS 1.2
  are never offered. Do not try to work around this.
* **Raw TLS**: `kextls.dll` has a small client API (see
  `net/kextls/include/kextls.h`): you provide send/receive callbacks,
  it does the TLS.
* **Hashes and random numbers**: `bcrypt.dll` provides `BCryptGenRandom`, MD5,
  SHA-1, SHA-256/384/512 and HMAC. `RtlGenRandom` (`SystemFunction036`) and
  `rand_s` come from a real CSPRNG.
* **Not available**: SChannel/SSPI TLS (so WinINet and Internet Explorer still
  use their old TLS), asymmetric CNG (`BCrypt*KeyPair`, `NCrypt*`), DPAPI,
  IPv6.

## 6. Building

### clang / LLVM (recommended)

The same flags KernelExEx itself uses (`net/tools/build_win9x.py`):

```sh
clang --target=i686-w64-windows-gnu -march=i586 -O2 \
      -fno-stack-protector -mno-stack-arg-probe -c app.c
clang --target=i686-w64-windows-gnu -fuse-ld=lld app.o -o app.exe \
      -Wl,--subsystem,windows:4.0 -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
      -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -lkernel32 -luser32
```

* Subsystem and OS version 4.0 keep the file loadable even with KernelEx
  switched off for it. KernelExEx itself accepts higher versions.
* ASLR and DEP flags do nothing on Windows 98; turning them off keeps the
  checker below quiet.
* For DLLs, give each one a fixed, distinct base address below 2 GB:
  relocating on Windows 98 costs memory in every process.
* Link against `msvcrt.dll`, the C runtime Windows 98 SE ships. Avoid
  functions it lacks (the `_s` "secure" functions, most C99 additions in
  mingw-w64's own library are fine because they are linked statically).
  The Universal CRT (`ucrtbase.dll`, `api-ms-win-crt-*`) does not exist on
  Windows 98.
* Do not use `-D__USE_MINGW_ANSI_STDIO=1` unless you link mingw-w64's stdio
  statically.

### MinGW-w64 GCC

Same rules: `-march=i586`, the linker flags above, msvcrt. Avoid winpthreads
if you can (it works, but pulls in functions that only exist through
KernelExEx); use Win32 threads.

### Microsoft Visual C++

* Visual C++ 6 to 2005 produce Windows 98-compatible programs directly.
* Later versions import functions Windows 98 lacks (for example
  `EncodePointer`, `InitializeCriticalSectionEx`, `FlsAlloc`) and set
  subsystem 5.01 or 6.0. KernelExEx supplies many of these functions and
  accepts the newer subsystem, so such programs often run, but check the
  imports (below) against the inventory.

### Rust, Go and others

Not covered yet. Their standard libraries assume Windows 7 or later and
need functions KernelExEx does not provide (thread pools, `WaitOnAddress`,
IOCP for networking, among others).

## 7. Detecting KernelExEx at run time

```c
typedef unsigned long (*kexGetKEXVersion_t)(void);
HMODULE k = GetModuleHandleA("KERNELEX.DLL");
kexGetKEXVersion_t ver = k ? (kexGetKEXVersion_t) GetProcAddress(k, "kexGetKEXVersion") : NULL;
unsigned long v = ver ? ver() : 0;   /* 0x0407xxxx or higher: KernelExEx */
```

Version codes: KernelEx 4.5.2 reports `0x04050078`; KernelExEx reports
4.7.x (`0x0407xxxx`). Prefer testing for the feature you need instead:
`GetProcAddress(GetModuleHandleA("kernel32.dll"), "AcquireSRWLockExclusive")`,
or `LoadLibraryA("winhttp.dll")`. On Windows 98 a function that KernelExEx
provides is found by `GetProcAddress` only when KernelEx is enabled for the
program.

## 8. Checking a build before testing on Windows 98

`net/tools/pecheck.py` reads a PE file and reports every import that
Windows 98 SE does not export, plus instructions newer than the Pentium:

```sh
python3 net/tools/pecheck.py net/tools/exports/win98se app.exe
```

An import that pecheck flags is fine if [`api.json`](api.json) lists it as
`new` or `replaced` under the same DLL. This filter (run from the repository
root) prints `check` for every import that neither Windows 98 SE nor
KernelExEx provides:

```sh
python3 - app.exe <<'EOF'
import json, subprocess, sys
api = json.load(open("docs/api.json"))["dlls"]
ok = {(d.lower(), f["name"]) for d, v in api.items() for f in v["functions"] if f["kind"] in ("new", "replaced", "mslu")}
import glob, os
for t in glob.glob("net/tools/exports/win98se/*.txt"):      # native Windows 98 SE exports
    ok |= {(os.path.basename(t)[:-4].lower(), n) for n in open(t).read().split()}
out = subprocess.run(["llvm-readobj", "--coff-imports", sys.argv[1]], capture_output=True, text=True).stdout
dll = None
for line in out.splitlines():
    line = line.strip()
    if line.startswith("Name:") and line.lower().endswith((".dll", ".drv")): dll = line.split()[-1].lower()
    elif line.startswith("Symbol:") and dll:
        name = line.split()[1]          # "Symbol: Name (hint)"
        print(("ok      " if (dll, name) in ok else "check   ") + f"{dll}!{name}")
EOF
```

Then test on a real Windows 98 SE install (86Box or QEMU work well).

## 9. Checklist for AI agents

When asked to make code run on Windows 98 with KernelExEx:

1. Target `i686`, `-march=i586`, subsystem 4.0, `msvcrt.dll`; no UCRT.
2. Look up every Windows API you call in `docs/api.json`. Treat `stub` as
   "will fail at run time" and `mslu` as working (through `unicows.dll` or `kexuni.dll`); for anything not
   listed, check `net/tools/exports/win98se/<dll>.txt` (native Windows 98 SE).
3. No thread pools, IOCP, `WaitOnAddress`, IPv6, asymmetric CNG, SChannel.
4. One CPU: do not spin-wait on another thread.
5. Use WinHTTP for HTTPS; never disable certificate checks.
6. Prefer ANSI (`...A`) functions for file and window APIs.
7. Run `pecheck.py` on every binary you produce.
8. Say clearly in your answer what you could not verify on a real Windows 98
   system.

## 10. Keeping this guide current

* `docs/API.md` and `docs/api.json` are generated by
  `toolchain/gen_api_inventory.py` and checked by the build
  (`toolchain/build_core.py` fails if they are stale). After adding or
  removing an API, run the generator and commit its output.
* Update this file when a limit in sections 3 to 6 changes: for example when
  SChannel, asynchronous WinHTTP or IPv6 arrive.
