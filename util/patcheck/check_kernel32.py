#!/usr/bin/env python3
"""Check KernelEx's kernel32 byte patterns against a kernel32.dll file.

KernelEx locates undocumented kernel32 internals by byte patterns, in two
places: the VxD (vxd/patch_kernel32.cpp, patched at boot) and the core
(core/internals.cpp, read at init). A pattern that matches zero times or
more than once makes KernelEx refuse to start. Windows 9x always loads
kernel32 at its preferred base, so the bytes in the file are the bytes in
memory, and the check can run offline against any kernel32 build
(localised, service-packed, ME...).

Patterns are read from the C++ sources, so this tool never drifts from them.
Values: 0..255 must match, -1 and -2 match anything (-2 marks the address
the code extracts).

Usage: check_kernel32.py <KernelEx source root> <kernel32.dll> [...]
Exit status 1 if any pattern is not unique.
"""
import re
import struct
import sys
from pathlib import Path

# core/internals.cpp: where each finder searches (see the find_* functions)
INTERNALS_REGIONS = {
    "find_krnl32lock": ("export", "VirtualQueryEx", None),      # size = pattern length
    "find_curPDB": ("export", "GetCurrentProcessId", None),
    "find_mod_table": ("ordinal", 23, 0x20),
    "find_MRFromHLib": ("ordinal", 23, 0x20),
    "find_pimteMax": ("section", ".text", None),
    "find_TIDtoTDB": ("section", ".text", None),
    "find_MRLoadTree": ("section", ".text", None),
    "find_FreeLibTree": ("export", "FreeLibrary", 0x80),
    "find_FreeLibRemove": ("section", ".text", None),
    "find_AllocHandle": ("export", "OpenProcess", 0x80),
}


class PE:
    def __init__(self, path):
        d = self.d = Path(path).read_bytes()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsect, optsize = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<I", d, opt + 28)[0]
        self.sections = {}
        self.secs = []
        for i in range(nsect):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, opt + optsize + 40 * i)
            n = name.rstrip(b"\0").decode("ascii", "replace")
            self.sections[n] = (va, vsize, raw, rsize)
            self.secs.append((va, vsize, raw, rsize))
        exp_rva = struct.unpack_from("<I", d, opt + 96)[0]
        e = self.off(exp_rva)
        ord_base, nfuncs, nnames, funcs, names, ords = struct.unpack_from("<IIIIII", d, e + 16)
        self.by_ord = {}
        self.by_name = {}
        for i in range(nfuncs):
            self.by_ord[ord_base + i] = struct.unpack_from("<I", d, self.off(funcs) + 4 * i)[0]
        for i in range(nnames):
            nm_off = self.off(struct.unpack_from("<I", d, self.off(names) + 4 * i)[0])
            nm = d[nm_off:d.index(b"\0", nm_off)].decode()
            o = struct.unpack_from("<H", d, self.off(ords) + 2 * i)[0]
            self.by_name[nm] = self.by_ord[ord_base + o]

    def off(self, rva):
        for va, vsize, raw, rsize in self.secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(hex(rva))

    def region(self, kind, key, size, pat_len):
        if kind == "section":
            va, vsize, raw, rsize = self.sections[key]
            return raw, min(vsize, rsize), va
        rva = self.by_name[key] if kind == "export" else self.by_ord[key]
        return self.off(rva), size or pat_len, rva


def find(data, start, size, pat):
    hits = []
    end = start + size - len(pat)
    first = pat[0]
    for i in range(start, end + 1):
        if first >= 0 and data[i] != first:
            continue
        if all(p < 0 or data[i + j] == p for j, p in enumerate(pat)):
            hits.append(i)
    return hits


ARRAY = re.compile(r"static const short (\w+)\[\]\s*=\s*\{([^}]*)\}", re.S)
FUNC = re.compile(r"^\S[^\n;]*?\b(\w+)\s*\([^;{]*\)\s*\{", re.M)


def patterns(src, kinds):
    """(function, array name, values) for every pattern array in a file."""
    text = src.read_text(errors="replace")
    funcs = [(m.start(), m.group(1)) for m in FUNC.finditer(text)]
    out = []
    for m in ARRAY.finditer(text):
        if m.group(1) not in kinds:
            continue
        owner = [f for pos, f in funcs if pos < m.start()]
        vals = [int(v, 0) for v in re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", m.group(2))]
        out.append((owner[-1] if owner else "?", m.group(1), vals))
    return out


def main():
    root = Path(sys.argv[1])
    bad = 0
    vxd = patterns(root / "vxd/patch_kernel32.cpp", {"pattern"})
    core = patterns(root / "core/internals.cpp", {"pat"})
    for path in sys.argv[2:]:
        pe = PE(path)
        print(f"== {path} (image base {pe.base:#x})")
        va, vsize, raw, rsize = pe.sections[".text"]
        for func, _, pat in vxd:
            hits = find(pe.d, raw, min(vsize, rsize), pat)
            ok = len(hits) == 1
            bad += not ok
            where = f"at {pe.base + va + hits[0] - raw:#x}" if ok else ""
            print(f"  {'OK  ' if ok else 'FAIL'} vxd  {func:40s} {len(hits)} match {where}")
        for func, _, pat in core:
            spec = INTERNALS_REGIONS.get(func)
            if not spec:
                print(f"  ???  core {func}: no search region known (update INTERNALS_REGIONS)")
                bad += 1
                continue
            start, size, rva = pe.region(spec[0], spec[1], spec[2], len(pat))
            hits = find(pe.d, start, size, pat)
            ok = len(hits) == 1
            bad += not ok
            print(f"  {'OK  ' if ok else 'FAIL'} core {func:40s} {len(hits)} match in {spec[0]} {spec[1]}")
    print("ALL PATTERNS UNIQUE" if not bad else f"{bad} PATTERN PROBLEMS")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
