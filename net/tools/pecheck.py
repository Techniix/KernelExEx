#!/usr/bin/env python3
"""Static checks that a PE built for KernelEx net can load and run on Windows 9x
on Pentium-class CPUs.

  - PE header: i386, subsystem and OS version <= 4.x, image base below 2 GB,
    no TLS directory (static TLS does not work in DLLs loaded by LoadLibrary
    on 9x), no ASLR/NX flags, 4 KB file alignment.
  - Imports: every imported function must be listed in win9x_exports.txt
    (functions known to exist on Windows 95/98/ME), or be exported by one of
    the modules being checked together.
  - Code: no instruction newer than the Pentium: CMOV/FCMOV/FCOMI (P6),
    multi-byte NOP (0F 1F, P6), MMX/SSE registers, SYSENTER, RDTSCP.

Usage: pecheck.py <allowlist file | exports dir> [--shared-arena] <dll>...

  exports dir: e.g. tools/exports/win98se, export tables dumped from a real
  installation (names only).
"""
import re
import struct
import subprocess
import sys
from pathlib import Path


def rva_to_off(sections, rva):
    for va, vsize, raw, rsize in sections:
        if va <= rva < va + max(vsize, rsize):
            return rva - va + raw
    raise ValueError(f"rva {rva:#x} not in a section")


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii")


def parse(path):
    d = Path(path).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0", "not a PE file"
    machine, nsect, _, _, _, optsize, chars = struct.unpack_from("<HHIIIHH", d, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    info = {
        "machine": machine, "magic": magic, "characteristics": chars,
        "image_base": struct.unpack_from("<I", d, opt + 28)[0],
        "file_align": struct.unpack_from("<I", d, opt + 36)[0],
        "os_ver": struct.unpack_from("<HH", d, opt + 40),
        "subsys_ver": struct.unpack_from("<HH", d, opt + 48),
        "subsystem": struct.unpack_from("<H", d, opt + 68)[0],
        "dll_chars": struct.unpack_from("<H", d, opt + 70)[0],
    }
    ndirs = struct.unpack_from("<I", d, opt + 92)[0]
    dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(ndirs)]
    sec = opt + optsize
    sections = []
    for i in range(nsect):
        name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, sec + 40 * i)
        sections.append((va, vsize, raw, rsize))
    info["tls"] = dirs[9][0] if ndirs > 9 else 0

    imports = {}
    if dirs[1][0]:
        off = rva_to_off(sections, dirs[1][0])
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", d, off)
            if not name_rva:
                break
            dll = cstr(d, rva_to_off(sections, name_rva)).lower()
            thunk = rva_to_off(sections, oft or ft)
            names = imports.setdefault(dll, [])
            while True:
                v = struct.unpack_from("<I", d, thunk)[0]
                if not v:
                    break
                if v & 0x80000000:
                    names.append(f"#{v & 0xFFFF}")
                else:
                    names.append(cstr(d, rva_to_off(sections, v) + 2))
                thunk += 4
            off += 20
    exports = []
    if dirs[0][0]:
        off = rva_to_off(sections, dirs[0][0])
        nnames, _, names_rva = struct.unpack_from("<III", d, off + 24)[0], 0, struct.unpack_from("<I", d, off + 32)[0]
        for i in range(nnames):
            exports.append(cstr(d, rva_to_off(sections, struct.unpack_from("<I", d, rva_to_off(sections, names_rva) + 4 * i)[0])))
    info["imports"] = imports
    info["exports"] = exports
    return info


BAD_INSN = re.compile(r"\b(cmov\w*|fcmov\w*|fcomip?|fucomip?|sysenter|sysexit|rdtscp|nopw|nopl)\b|%x?mm\d|%ymm\d")


def code_check(path):
    out = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", path], capture_output=True, text=True,
                         check=True).stdout
    bad = []
    for line in out.splitlines():
        if BAD_INSN.search(line):
            bad.append(line.strip())
    # multi-byte NOP also shows up as raw 0f 1f; double-check with raw bytes
    raw = subprocess.run(["llvm-objdump", "-d", path], capture_output=True, text=True, check=True).stdout
    for line in raw.splitlines():
        if re.search(r":\s+0f 1f ", line):
            bad.append(line.strip())
    return bad


def main():
    allow = set()
    src = Path(sys.argv[1])
    if src.is_dir():
        # a directory of real export tables: <dll name>.txt, one export per line
        # <dll>.txt holds names, <dll>.ord holds ordinals as "#N"
        for f in list(src.glob("*.txt")) + list(src.glob("*.ord")):
            for fn in f.read_text().split():
                allow.add((f.stem.lower(), fn))
    else:
        for line in src.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                dll, fn = line.split("!", 1)
                allow.add((dll.lower(), fn))
    dlls = [a for a in sys.argv[2:] if a != "--shared-arena"]
    shared_ok = "--shared-arena" in sys.argv  # KernelEx core and shared API libraries live above 2 GB
    infos = {Path(p).name.lower(): parse(p) for p in dlls}
    ours = {(n, e) for n, i in infos.items() for e in i["exports"]}
    failed = False
    for name, i in infos.items():
        problems = []
        if i["machine"] != 0x14C or i["magic"] != 0x10B:
            problems.append("not a 32-bit i386 PE")
        if i["subsys_ver"][0] > 4 or i["os_ver"][0] > 4:
            problems.append(f"subsystem {i['subsys_ver']} / OS {i['os_ver']} newer than 4.x (9x refuses to load)")
        if i["image_base"] >= 0x80000000 and not shared_ok:
            problems.append(f"image base {i['image_base']:#x} is in the 9x shared arena")
        if i["tls"]:
            problems.append("has a TLS directory (static TLS fails in 9x LoadLibrary)")
        if i["dll_chars"] & 0x0140:
            problems.append(f"DllCharacteristics {i['dll_chars']:#x} sets DYNAMIC_BASE/NX_COMPAT")
        if i["file_align"] != 4096:
            problems.append(f"file alignment {i['file_align']} (4096 lets 9x map sections in place)")
        missing = []
        for dll, fns in i["imports"].items():
            for fn in fns:
                if (dll, fn) not in allow and (dll, fn) not in ours:
                    missing.append(f"{dll}!{fn}")
        if missing:
            problems.append("imports not known on Windows 9x: " + ", ".join(missing))
        bad = code_check(dlls[[Path(p).name.lower() for p in dlls].index(name)])
        if bad:
            problems.append(f"{len(bad)} instructions newer than Pentium, e.g.: " + " | ".join(bad[:5]))
        nimp = sum(len(v) for v in i["imports"].values())
        print(f"{name}: base {i['image_base']:#x}, subsystem {i['subsys_ver'][0]}.{i['subsys_ver'][1]}, "
              f"{len(i['exports'])} exports, {nimp} imports from {', '.join(sorted(i['imports']))}")
        for p in problems:
            print(f"  PROBLEM: {p}")
        failed |= bool(problems)
    print("PE CHECK FAILED" if failed else "PE CHECK OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
