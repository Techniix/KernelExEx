#!/usr/bin/env python3
"""Prepare a Windows 9x cross SDK from a mingw-w64 source checkout.

Writes into OUT:
  include/_mingw.h, include/sdks/_mingw_ddk.h   configured for msvcrt 6.0 and
                                                 _WIN32_WINNT 0x0410 (Windows 98)
  lib/lib<dll>.a                                 import libraries made with
                                                 llvm-dlltool from mingw-w64's
                                                 i386 .def files

Only the import libraries are produced; no mingw-w64 runtime code is linked
into KernelEx modules. Whether an import actually exists on Windows 9x is
checked afterwards by tools/pecheck.py against tools/win9x_exports.txt.

Usage: prepare_sdk.py <mingw-w64 checkout> <out dir>
"""
import re
import subprocess
import sys
from pathlib import Path

DLLS = ("kernel32", "user32", "advapi32", "ws2_32", "wsock32", "msvcrt", "gdi32", "shell32",
        # needed by the KernelEx core build (toolchain/build_core.py)
        "comctl32", "comdlg32", "version", "winspool", "shlwapi", "ole32", "oleaut32", "rpcrt4",
        "usp10", "winmm", "cryptui", "mpr", "imm32", "msimg32", "psapi", "userenv", "uxtheme")


def configure_headers(mingw: Path, out: Path) -> None:
    crt = mingw / "mingw-w64-headers" / "crt"
    inc = out / "include"
    (inc / "sdks").mkdir(parents=True, exist_ok=True)
    text = (crt / "_mingw.h.in").read_text()
    text = text.replace("@DEFAULT_MSVCRT_VERSION@", "0x600")
    text = text.replace("@DEFAULT_WIN32_WINNT@", "0x0410")
    (inc / "_mingw.h").write_text(text)
    ddk_in = crt / "sdks" / "_mingw_ddk.h.in"  # absent in newer mingw-w64
    if ddk_in.is_file():
        (inc / "sdks" / "_mingw_ddk.h").write_text(re.sub(r"@DEFAULT_\w+@", "0", ddk_in.read_text()))


def def_text(mingw: Path, name: str) -> str:
    crtdir = mingw / "mingw-w64-crt"
    for candidate in (crtdir / "lib32" / f"{name}.def", crtdir / "lib-common" / f"{name}.def"):
        if candidate.is_file():
            return candidate.read_text()
    for candidate in (crtdir / "lib32" / f"{name}.def.in", crtdir / "lib-common" / f"{name}.def.in"):
        if candidate.is_file():
            return subprocess.run(
                ["clang", "-E", "-P", "-x", "c", "-DDEF_I386", f"-I{crtdir / 'def-include'}", str(candidate)],
                check=True, capture_output=True, text=True).stdout
    raise FileNotFoundError(name)


def import_libraries(mingw: Path, out: Path) -> None:
    lib = out / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    for name in DLLS:
        text = def_text(mingw, name)
        if name == "msvcrt":
            # mingw-w64 marks msvcrt's maths functions DATA because its own
            # runtime replaces them; we link msvcrt directly, so make them
            # ordinary imports.
            text = re.sub(r"\s+DATA\b", "", text)
        d = lib / f"{name}.def"
        d.write_text(text)
        subprocess.run(["llvm-dlltool", "-m", "i386", "-k", "-d", str(d), "-l", str(lib / f"lib{name}.a")],
                       check=True)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    mingw, out = Path(sys.argv[1]), Path(sys.argv[2])
    configure_headers(mingw, out)
    import_libraries(mingw, out)
    (out / "MINGW_INCLUDE").write_text(str(mingw / "mingw-w64-headers" / "include") + "\n")
    print(f"SDK ready in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
