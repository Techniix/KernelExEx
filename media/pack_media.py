#!/usr/bin/env python3
"""Make kexmedia.exe's payload: everything it needs to build the install stick from
the user's own Windows 98 CD, with nothing from Microsoft in it.

Usage: pack_media.py STAGE_DIR REBIOS.EFI CSMWRAP.EFI OUT [KEXMEDIA.EXE]

STAGE_DIR is stage_extra.py's output (our DOS stage: AUTOEXEC.BAT, CONFIG.SYS,
KEXMEM.SYS, KEXPREP.EXE, KEXINST/...). With KEXMEDIA.EXE, OUT is that program with
the payload appended (the one file users download); without, OUT is the bare payload
(for tests: tests/media).

The payload holds:
  BASE     the hybrid ISO of make_iso.sh with an empty 512 MB KEXSETUP.IMG, as
           segments (zero runs are not stored) and the places kexmedia patches:
           KEXSETUP.IMG's size in its directory records, MEMDISK's cylinders in
           isolinux.cfg
  MBR.BIN  our master boot record (media/dos/mbr.asm)
  STAGE/*  the stage files
Format (little-endian), read by media/win/mkimage.c:
  entries: u32 name length, name, u32 size, data
  trailer: "KXPAY1\\0\\0", u32 count, u32 bytes of the entries
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = 512 * 1024 * 1024
SEC = 2048


def dir_records(iso, lba, size):
    """(offset of the record, name bytes, flags, extent, length) of a directory"""
    for s in range((size + SEC - 1) // SEC):
        base = (lba + s) * SEC
        o = 0
        while o < SEC:
            rl = iso[base + o]
            if rl == 0:
                break
            rec = iso[base + o:base + o + rl]
            nl = rec[32]
            yield base + o, bytes(rec[33:33 + nl]), rec[25], struct.unpack_from("<I", rec, 2)[0], \
                struct.unpack_from("<I", rec, 10)[0]
            o += rl


def roots(iso):
    """the primary and Joliet root directories: (lba, size, joliet)"""
    out = []
    s = 16
    while True:
        d = iso[s * SEC:(s + 1) * SEC]
        if d[0] == 255:
            break
        if d[1:6] == b"CD001" and d[0] in (1, 2):
            joliet = d[0] == 2 and d[88:90] == b"%/"
            if d[0] == 1 or joliet:
                out.append((struct.unpack_from("<I", d, 156 + 2)[0], struct.unpack_from("<I", d, 156 + 10)[0], joliet))
        s += 1
    return out


def name_of(raw, joliet):
    n = raw.decode("utf-16-be") if joliet else raw.decode("latin-1")
    return n.split(";")[0].rstrip(".").upper()


def find(iso, lba, size, joliet, path):
    parts = path.split("/")
    for i, part in enumerate(parts):
        for off, raw, flags, ext, ln in dir_records(iso, lba, size):
            if raw in (b"\0", b"\1"):
                continue
            if name_of(raw, joliet) == part:
                if i == len(parts) - 1:
                    return off, ext, ln
                lba, size = ext, ln
                break
        else:
            return None
    return None


def base_blob(iso):
    patches, ph = [], None
    cfg = None
    for lba, size, joliet in roots(iso):
        r = find(iso, lba, size, joliet, "KEXSETUP.IMG")
        if not r:
            sys.exit("pack_media: KEXSETUP.IMG not found in the ISO")
        off, ext, ln = r
        if ln != CAP:
            sys.exit("pack_media: the placeholder is %d bytes, not %d" % (ln, CAP))
        if ph not in (None, ext):
            sys.exit("pack_media: the directories disagree about KEXSETUP.IMG")
        ph = ext
        patches.append((off + 10, 1))
        if not joliet:
            c = find(iso, lba, size, joliet, "ISOLINUX/ISOLINUX.CFG")
            if c:
                cfg = c
    if not cfg:
        sys.exit("pack_media: isolinux.cfg not found")
    text = iso[cfg[1] * SEC:cfg[1] * SEC + cfg[2]]
    t = text.find(b"c=@@@@@@")
    if t < 0 or text.count(b"c=@@@@@@") != 1:
        sys.exit("pack_media: the cylinder token is not in isolinux.cfg (KEX_CTOKEN=1 make_iso.sh)")
    patches.append((cfg[1] * SEC + t, 2))
    ph_off = ph * SEC
    # segments: the ISO without the placeholder; runs of >= 64 KB of zeros are not stored
    zero = bytes(SEC)
    end = len(iso)
    blocks = []
    for o in list(range(0, ph_off, SEC)) + list(range(ph_off + CAP, end, SEC)):
        blocks.append((o, iso[o:o + SEC] == zero))
    # merge into runs
    runs = []
    for o, z in blocks:
        if runs and runs[-1][0] + runs[-1][1] == o and runs[-1][2] == z:
            runs[-1][1] += SEC
        else:
            runs.append([o, SEC, z])
    # short zero runs are stored as data
    merged = []
    for o, ln, z in runs:
        z = z and ln >= 64 * 1024
        if merged and merged[-1][0] + merged[-1][1] == o and merged[-1][2] == z:
            merged[-1][1] += ln
        else:
            merged.append([o, ln, z])
    out = bytearray(b"KXBASE1\0")
    out += struct.pack("<IIQQQ", len(merged), len(patches), len(iso), ph_off, CAP)
    for o, t in patches:
        out += struct.pack("<QII", o, t, 0)
    for o, ln, z in merged:
        out += struct.pack("<QQII", o, ln, 1 if z else 0, 0)
        if not z:
            out += iso[o:o + ln]
    return bytes(out)


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    stage, rebios, csmwrap, out = sys.argv[1:5]
    with tempfile.TemporaryDirectory() as w:
        ph = os.path.join(w, "KEXSETUP.IMG")
        with open(ph, "wb") as f:
            f.truncate(CAP)
        iso_path = os.path.join(w, "base.iso")
        subprocess.run(["sh", os.path.join(HERE, "make_iso.sh"), ph, rebios, csmwrap, iso_path], check=True,
                       env=dict(os.environ, KEX_CTOKEN="1"), stdout=subprocess.DEVNULL)
        iso = open(iso_path, "rb").read()
    entries = [("BASE", base_blob(iso)), ("MBR.BIN", open(os.path.join(HERE, "mbr.bin"), "rb").read())]
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            rel = os.path.relpath(p, stage).replace(os.sep, "/")
            entries.append(("STAGE/" + rel, open(p, "rb").read()))
    blob = bytearray()
    for n, d in entries:
        nb = n.encode("ascii")
        blob += struct.pack("<I", len(nb)) + nb + struct.pack("<I", len(d)) + d
    payload = bytes(blob) + b"KXPAY1\0\0" + struct.pack("<II", len(entries), len(blob))
    with open(out, "wb") as f:
        if len(sys.argv) == 6:
            f.write(open(sys.argv[5], "rb").read())
        f.write(payload)
    print("%s: %d files, %.1f MB (stick needs %d MB)" % (out, len(entries), os.path.getsize(out) / 1048576,
                                                         len(iso) >> 20))


if __name__ == "__main__":
    main()
