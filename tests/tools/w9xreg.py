#!/usr/bin/env python3
"""tests/tools/w9xreg.py: read values from a Windows 9x registry file (SYSTEM.DAT /
USER.DAT, "CREG") by key name: test use only. Copyright (C) 2026, KernelExEx contributors. GPLv2.

usage: w9xreg.py FILE KEYNAME   -> every key with that last path element, and its values

Format (from the file itself): "CREG" header, then an RGKN block of the key tree, then
RGDB blocks; each RGDB key record: dword size, word id, word block, dword used,
word name length, word value count, dword 0, name, then values: dword type, dword 0,
word name length, word data length, name, data.
"""
import struct
import sys


def records(d):
    pos = d.find(b"RGDB")
    while pos != -1:
        size, = struct.unpack_from("<I", d, pos + 4)
        end, p = pos + size, pos + 0x20
        while p + 20 <= end:
            rsize, kid, blk, used, nlen, nval = struct.unpack_from("<IHHIHH", d, p)
            if rsize < 20 or p + rsize > end:
                break
            if 20 <= used <= rsize:
                name = d[p + 20:p + 20 + nlen].decode("latin-1")
                q, vals, lim = p + 20 + nlen, [], p + used
                for _ in range(nval):
                    if q + 12 > lim:
                        break
                    t, _z, vn, vd = struct.unpack_from("<IIHH", d, q)
                    vals.append((d[q + 12:q + 12 + vn].decode("latin-1"), t, d[q + 12 + vn:q + 12 + vn + vd]))
                    q += 12 + vn + vd
                yield name, vals
            p += rsize
        pos = d.find(b"RGDB", end)


def show(t, v):
    if t == 1:
        return repr(v.rstrip(b"\0").decode("latin-1"))
    if t in (3, 4) and len(v) == 4:
        return f"{struct.unpack('<I', v)[0]:#x} ({'dword' if t == 4 else 'binary'})"
    return v.hex()


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    for name, vals in records(d):
        if name.lower() == sys.argv[2].lower():
            print(f"[{name}]")
            for vn, t, v in vals:
                print(f"  {vn or '@'} = {show(t, v)}")
