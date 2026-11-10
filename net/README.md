# KernelEx net

Modern cryptography and TLS for Windows 95/98/ME, as KernelEx modules.
See `docs/ARCHITECTURE.md` for the design.

## Build (Linux)

```sh
# 1. a Windows 9x SDK from mingw-w64's headers and .def files
git clone --depth 1 https://github.com/mingw-w64/mingw-w64 ../../mingw-w64
python3 tools/prepare_sdk.py ../../mingw-w64 ../../build/sdk

# 2. the DLLs and the self-test program
python3 tools/build_win9x.py --sdk ../../build/sdk --out ../../build/win9x

# 3. static checks for Windows 9x / Pentium compatibility
python3 tools/pecheck.py tools/win9x_exports.txt ../../build/win9x/*.dll ../../build/win9x/kexnet_test.exe

# 4. host tests (needs openssl and python3-cryptography)
tests/host/build.sh ../../build/host
python3 tests/host/run_tests.py ../../build/host/test_net [--live]
```

Needs clang, lld and llvm-dlltool (LLVM 18 tested), and Python 3.

## Install (on Windows 98/ME with KernelEx)

Copy `kextls.dll`, `bcrypt.dll`, `winhttp.dll` and `certs\roots.pem` to
`%windir%\KernelEx\`, add the `KnownDLLs` entries (`KEXTLS`, `BCRYPT`,
`WINHTTP`), and append `,kextls` to `contents=` in the `[DCFG1]` section of
`core.ini`. The NSIS installer (`KernelEx.nsi`) does the same.
