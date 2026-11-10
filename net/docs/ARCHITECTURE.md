# KernelEx net: architecture and decisions

KernelEx net gives Windows 95/98/ME modern cryptography and TLS as an
operating-system service: one TLS stack (`kextls.dll`) shared by every
process, exposed through the Windows APIs that applications already call
(`bcrypt.dll`, `winhttp.dll`, and `ADVAPI32!RtlGenRandom` via the KernelEx
resolver).

```
 applications ──► winhttp.dll ─┐
              ──► bcrypt.dll  ─┼─► kextls.dll (Mbed TLS 3.6 LTS, DRBG, trust store)
              ──► ADVAPI32!RtlGenRandom, MSVCRT!rand_s ─┘   (KernelEx API library)
                                   │
                                   ▼
               Winsock 1.1 (wsock32), CryptoAPI (seed source), kernel32
```

## Decisions

| # | Decision | Why | Alternatives rejected |
|---|----------|-----|-----------------------|
| 1 | Base the project on Xeno86's SVN history (hatarch/KernelEx mirror, 4.5.2 + 7 later commits). Ley0k's 2013 fork is kept as branch `ley0k-4.6b1` for cherry-picking. | Cleanest provenance; the fork adds risky subsystems and is unfinished. | Building on metaxor/KernelEx directly. |
| 2 | New modules are C99, cross-built with clang + lld for `i686-w64-windows-gnu`, `-march=i586`. | MSVC 6 cannot compile Mbed TLS 3.x; clang gives a reproducible Linux build. | MSVC 6, Open Watcom. |
| 3 | No mingw-w64 runtime. `net/rt` supplies the entry point, stack probes (`_alloca`, `___chkstk_ms`), 64-bit division, and time functions; everything else comes from msvcrt.dll. | Avoids runtime APIs Windows 9x lacks; keeps the binaries small. | Linking libmingwex / libgcc. |
| 4 | Mbed TLS 3.6.7 (LTS until March 2027), vendored unmodified; config in `kextls/config`. Move to 4.1 LTS (to March 2029) before then. | TLS 1.3, small, C99, GPL-2.0+ option, already proven on 98 by OpenCE. | OpenSSL (licence, size), BearSSL (no TLS 1.3), wolfSSL (GPLv3). |
| 5 | One process-wide DRBG: SP 800-90A HMAC_DRBG (SHA-256), seeded from CryptoAPI + RDTSC jitter + system state through SHA-512. State lives in kextls.dll's data section, which is private per process because the image base is below 2 GB. | Replaces the predictable, shared KISS generator in kexbases. | Trusting CryptGenRandom alone (unaudited on 9x). |
| 6 | The security policy is fixed in code: TLS 1.2/1.3 only, AEAD + ECDHE only, verification always on, SHA-1 and RSA < 2048 rejected in chains, a clock sanity check. WinHTTP requests to ignore certificate errors are refused. | Users of 9x cannot easily audit per-app settings; a safe default must not be switchable. | Honouring `SECURITY_FLAG_IGNORE_*`. |
| 7 | ChaCha20-Poly1305 is offered first. | Pentium-class CPUs have no AES instructions. | AES-GCM first. |
| 8 | winhttp uses Winsock 1.1 (wsock32.dll). | Loads on Windows 95 without the Winsock 2 update. | ws2_32. |
| 9 | DLL names reach applications through KernelEx's `KnownDLLs` mechanism (`HKLM\Software\KernelEx\KnownDLLs`). The same files also work side-by-side in an application's folder without KernelEx (portable mode). | No system files are replaced; uninstall is clean. | Copying into `%windir%\system`. |
| 10 | Behaviour of WinHTTP edge cases follows Wine's implementation (LGPL; consulted, not copied). | Wine's tests encode the real Windows behaviour. | Guessing from the documentation. |

## Testing

- `tests/host/run_tests.py` runs the portable code on Linux: unit tests
  (division helpers, gmtime, URL parsing, DRBG statistics), HTTP framing
  against a local server, a 16-case TLS matrix against `openssl s_server`
  (protocol versions, cipher policy, expired/wrong-name/untrusted/SHA-1/
  RSA-1024 certificates, missing roots, wrong clock), and live HTTPS through
  a CONNECT proxy.
- `tools/pecheck.py` checks the Windows binaries: PE header fields Windows 9x
  needs, every import against `tools/win9x_exports.txt`, and no instruction
  newer than the Pentium.
- `tests/win9x/kexnet_test.exe` runs on the target machine (selftest, import
  probe, HTTPS GET).

## Known limits and follow-up

- WinHTTP: synchronous only; HTTP/1.1; one connection per request; no
  request-header queries; no IDN; IPv4 only.
- No revocation checking (OCSP stapling planned).
- The trust store is a plain file (no ACLs on 9x); a signed-bundle updater is planned.
- `EncodePointer` in kexbases still uses one system-wide cookie (old FIXME);
  it should move into a per-process module.
- An SChannel SSP (TLS for WinINet/IE) is the next large piece.
