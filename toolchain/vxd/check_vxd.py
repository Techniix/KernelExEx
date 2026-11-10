#!/usr/bin/env python3
"""Check a VxD produced by le_link.py against the PE it came from.

1. Loads the PE (applying its base relocations) and the LE (applying its
   fixups, the way the VMM loader does) at the same address and requires
   the two memory images to be identical.
2. Lists every INT 20h dynamic link in the code and requires each service
   ID to be one of the known IDs passed with --allow.

Usage: check_vxd.py <in.pe> <out.vxd> --allow 0x00010000,0x00010004,...
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import ledump  # noqa: E402
import le_link  # noqa: E402

LOAD = 0xC1230000


def pe_image(path):
    d, image_base, dirs, secs = le_link.read_pe(path)
    body = [s for s in secs if s["name"] != ".reloc"]
    start = min(s["va"] for s in body)
    end = max(s["va"] + s["vsize"] for s in body)
    img = bytearray(end - start)
    for s in body:
        img[s["va"] - start:s["va"] - start + len(s["raw"])] = s["raw"]
    delta = (LOAD - start) - image_base
    for rva in le_link.base_relocs(secs, dirs):
        off = rva - start
        if 0 <= off < len(img):
            v, = struct.unpack_from("<I", img, off)
            struct.pack_into("<I", img, off, (v + delta) & 0xFFFFFFFF)
    return img


def le_image(path):
    data = open(path, "rb").read()
    h, objs, pmap, names, entries, fix, nonres = ledump.parse(data)
    assert len(objs) == 1, "expected one object"
    img = bytearray(ledump.object_bytes(data, h, objs, pmap, 1))
    for page, src, fl, soff, obj, toff in fix:
        assert src == 0x07 and obj == 1, (src, obj)
        at = (page - 1) * h["page_size"] + soff
        val = struct.pack("<I", LOAD + toff)
        for i in range(4):  # a straddling field is written piecewise from both pages
            if 0 <= at + i < len(img) and (at + i) // h["page_size"] == page - 1:
                img[at + i] = val[i]
    return img, entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pe")
    ap.add_argument("vxd")
    ap.add_argument("--allow", required=True)
    a = ap.parse_args()
    allow = {int(x, 0) for x in a.allow.split(",")}
    p = pe_image(a.pe)
    l, entries = le_image(a.vxd)
    if p != l:
        diff = [i for i in range(min(len(p), len(l))) if p[i] != l[i]]
        sys.exit(f"FAIL: images differ (sizes {len(p)}/{len(l)}, first difference at {diff[:5]})")
    links = {}
    for m in re.finditer(rb"\xcd\x20(....)", bytes(p), re.S):
        sid, = struct.unpack("<I", m.group(1))
        links.setdefault(sid, []).append(m.start())
    bad = {hex(k): v for k, v in links.items() if (k & ~0x8000) not in allow}
    print(f"images identical ({len(p)} bytes); dynamic links:",
          ", ".join(f"{k:#010x}x{len(v)}" for k, v in sorted(links.items())))
    if bad:
        sys.exit(f"FAIL: unknown INT 20h service IDs (or stray CD 20 bytes) at {bad}")
    print("OK")


if __name__ == "__main__":
    main()
