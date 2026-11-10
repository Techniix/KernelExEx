#!/usr/bin/env python3
"""Build input/kexpad/kexpad.sys, a WDM HID minidriver for Windows 98 SE / ME.

usage: build_kexpad.py [--out DIR] [--sdk DIR]

Compiles with clang against mingw-w64's DDK headers (Windows 2000 baseline),
links a native-subsystem PE with import libraries made from
input/kexpad/imp/*.def, then checks every import against the Windows 98 SE
export tables in net/tools/exports/win98se-wdm and rejects FPU/SIMD code.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "input" / "kexpad"
EXPORTS = ROOT / "net" / "tools" / "exports" / "win98se-wdm"
MINGW = ROOT.parent / "thirdparty" / "mingw-w64" / "mingw-w64-headers"
HERE = Path(__file__).resolve().parent

TABLE = {"ntoskrnl.exe": "ntoskrnl.exe.txt", "hal.dll": "hal.dll.txt", "usbd.sys": "usbd.sys.txt",
         "hidclass.sys": "hidclass.sys.txt", "hidparse.sys": "hidparse.sys.txt"}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode or "warning" in r.stderr:
        sys.exit(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def fix_header(path):
    """Match Microsoft's 98 SE drivers (hidusb.sys): WDM_DRIVER, no NX/ASLR
    flags, subsystem 1.0, and a valid PE checksum."""
    d = bytearray(path.read_bytes())
    pe = int.from_bytes(d[0x3C:0x40], "little")
    opt = pe + 24
    for off, val in ((opt + 40, 4), (opt + 42, 0), (opt + 48, 1), (opt + 50, 0)):   # OS, subsystem versions
        d[off:off + 2] = val.to_bytes(2, "little")
    d[opt + 70:opt + 72] = (0x2000).to_bytes(2, "little")                          # IMAGE_DLLCHARACTERISTICS_WDM_DRIVER
    d[opt + 64:opt + 68] = b"\0\0\0\0"
    s = 0
    for i in range(0, len(d) - 1, 2):
        s += d[i] | (d[i + 1] << 8)
        s = (s & 0xFFFF) + (s >> 16)
    if len(d) & 1:
        s += d[-1]
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    d[opt + 64:opt + 68] = (s + len(d)).to_bytes(4, "little")
    path.write_bytes(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT.parent / "build" / "kexpad"))
    ap.add_argument("--sdk", default=str(ROOT.parent / "build" / "sdk"))
    a = ap.parse_args()
    out, sdk = Path(a.out), Path(a.sdk)
    out.mkdir(parents=True, exist_ok=True)
    res = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
    cflags = ["clang", "--target=i686-w64-windows-gnu", "-march=i586", "-O2", "-ffreestanding", "-fno-builtin",
              "-fno-stack-protector", "-fno-auto-import", "-mgeneral-regs-only", "-fno-jump-tables", "-nostdinc",
              "-isystem", f"{res}/include", "-isystem", str(sdk / "include"),
              "-isystem", str(MINGW / "crt"), "-isystem", str(MINGW / "include"),
              "-I", str(MINGW / "ddk" / "include" / "ddk"), "-I", str(MINGW / "ddk" / "include"),
              "-D_WIN32_WINNT=0x0500", "-DNTDDI_VERSION=0x05000000",
              # wdm.h tests NTDDI_WINVISTA, which sdkddkver.h spells NTDDI_VISTA
              "-include", "sdkddkver.h", "-DNTDDI_WINVISTA=0x06000000",
              "-Wall", "-Wno-pragma-pack"]
    objs = []
    for c in ("kexpad.c", "x360.c"):
        o = out / (c[:-2] + ".o")
        run(cflags + ["-c", str(SRC / c), "-o", str(o)])
        objs.append(str(o))
    libs = []
    for d in sorted((SRC / "imp").glob("*.def")):
        lib = out / f"lib{d.stem}.a"
        if d.stem == "usbd98":   # decorated export names: see implib_exact.py
            run([sys.executable, str(HERE / "implib_exact.py"), str(d), str(lib)])
        else:
            run(["llvm-dlltool", "-m", "i386", "-k", "-d", str(d), "-l", str(lib)])
        libs.append(str(lib))
    sysf = out / "kexpad.sys"
    run(["clang", "--target=i686-w64-windows-gnu", "-nostdlib", "-fuse-ld=lld", "-Wl,--subsystem,native",
         "-Wl,--entry,_DriverEntry@8", "-Wl,--image-base,0x10000", "-Wl,--dynamicbase",
         "-Wl,--disable-auto-import", "-Wl,--major-subsystem-version,1",
         "-Wl,--minor-subsystem-version,0", "-Wl,--major-os-version,4", "-Wl,--minor-os-version,0",
         *objs, *libs, "-o", str(sysf)])
    fix_header(sysf)

    # imports must exist on 98 SE
    dump = run(["llvm-objdump", "-p", str(sysf)])
    bad, dll, n = [], None, 0
    for line in dump.splitlines():
        m = re.match(r"\s*DLL Name: (\S+)", line)
        if m:
            dll = m.group(1).lower()
            if dll not in TABLE:
                bad.append(f"{dll} (whole DLL)")
                dll = None
            else:
                names = set((EXPORTS / TABLE[dll]).read_text().split())
            continue
        m = re.match(r"\s+\d+\s+(\S+)$", line)
        if dll and m:
            n += 1
            if m.group(1) not in names:
                bad.append(f"{dll}!{m.group(1)}")
    if bad:
        sys.exit("imports missing on Windows 98 SE: " + ", ".join(bad))
    dis = run(["llvm-objdump", "-d", "--no-show-raw-insn", str(sysf)])
    fpu = [l for l in dis.splitlines() if re.search(r"^\s+[0-9a-f]+:\s+(f[a-z]+\s|.*%x?mm[0-9])", l)]
    if fpu:
        sys.exit("FPU/SIMD instructions in a driver:\n" + "\n".join(fpu[:10]))
    (out / "kexpad.inf").write_bytes((SRC / "kexpad.inf").read_bytes())
    print(f"{sysf}: {sysf.stat().st_size} bytes, {n} imports, all present on 98 SE")


if __name__ == "__main__":
    main()
