#!/usr/bin/env python3
"""Turn a PE image linked by lld into a Windows 9x VxD (LE executable).

lld cannot write the LE format, so a VxD is linked as an ordinary
relocatable PE DLL first and converted here. The PE must:
  * import nothing (a VxD reaches the VMM through INT 20h dynamic links);
  * export the device descriptor block, <NAME>_DDB;
  * keep its base relocations (only 32-bit HIGHLOW relocations).

The whole image (every section except .reloc) becomes one LE object with
the flags Microsoft's linker gives LCODE (read, execute, preload, 32-bit;
ring-0 code can still write to it). The file layout follows what
Microsoft's linker writes, checked with ledump.py against the VxDs of
Windows 98 SE:

  MZ stub | LE header (0xC4 bytes) | object table | object page map |
  resident names | entry table | fixup page table | fixup records |
  (empty import tables) | data pages (512-aligned) | non-resident names

Usage: le_link.py <in.dll> <out.vxd> --name VKRNLEX --description "..."
                  [--device-id 0] [--ddk 0x40A] [--static]
"""
import argparse
import struct
import sys

PAGE = 0x1000
OBJ_FLAGS = 0x2045          # readable | executable | preload | 32-bit
MODULE_DYNAMIC = 0x38000    # VxD that VXDLDR may load and unload
MODULE_STATIC = 0x28000
IMAGE_REL_BASED_HIGHLOW = 3
IMAGE_REL_BASED_ABSOLUTE = 0

# The DOS stub Microsoft's linker puts in VxDs ("This program cannot be run in DOS mode.")
MZ_STUB = bytes.fromhex(
    "4d5a90000300000004000000ffff0000b8000000000000004000000000000000"
    "0000000000000000000000000000000000000000000000000000000080000000"
    "0e1fba0e00b409cd21b8014ccd21546869732070726f6772616d2063616e6e6f"
    "742062652072756e20696e20444f53206d6f64652e0d0d0a2400000000000000")
assert len(MZ_STUB) == 0x80


def read_pe(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        sys.exit(f"{path}: not a PE file")
    nsec, = struct.unpack_from("<H", d, pe + 6)
    optsz, = struct.unpack_from("<H", d, pe + 20)
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x10B:
        sys.exit("expected a 32-bit PE")
    image_base, = struct.unpack_from("<I", d, opt + 28)
    ndirs, = struct.unpack_from("<I", d, opt + 92)
    dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(ndirs)]
    secs = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        name = d[s:s + 8].rstrip(b"\0").decode()
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", d, s + 8)
        secs.append(dict(name=name, va=va, vsize=max(vsize, rawsize if not vsize else 0),
                         raw=d[rawptr:rawptr + min(rawsize, vsize or rawsize)]))
    return d, image_base, dirs, secs


def rva_bytes(secs, rva, n):
    for s in secs:
        if s["va"] <= rva < s["va"] + s["vsize"]:
            off = rva - s["va"]
            return (s["raw"][off:off + n]).ljust(n, b"\0")
    raise ValueError(f"RVA {rva:#x} not in any section")


def cstr_at(secs, rva):
    out = b""
    while True:
        c = rva_bytes(secs, rva + len(out), 1)
        if c == b"\0":
            return out.decode()
        out += c


def exports(secs, dirs):
    rva, size = dirs[0]
    if not rva:
        return {}
    e = rva_bytes(secs, rva, 40)
    base, nfunc, nnames, funcs, names, ords = struct.unpack_from("<IIIIII", e, 16)
    out = {}
    for i in range(nnames):
        nrva, = struct.unpack("<I", rva_bytes(secs, names + 4 * i, 4))
        idx, = struct.unpack("<H", rva_bytes(secs, ords + 2 * i, 2))
        frva, = struct.unpack("<I", rva_bytes(secs, funcs + 4 * idx, 4))
        out[cstr_at(secs, nrva)] = frva
    return out


def base_relocs(secs, dirs):
    rva, size = dirs[5] if len(dirs) > 5 else (0, 0)
    blob = b"".join(rva_bytes(secs, rva + i, 1) for i in range(size)) if size else b""
    out, p = [], 0
    while p + 8 <= len(blob):
        page, bsize = struct.unpack_from("<II", blob, p)
        if bsize < 8:
            break
        for i in range((bsize - 8) // 2):
            ent, = struct.unpack_from("<H", blob, p + 8 + 2 * i)
            typ, off = ent >> 12, ent & 0xFFF
            if typ == IMAGE_REL_BASED_ABSOLUTE:
                continue
            if typ != IMAGE_REL_BASED_HIGHLOW:
                sys.exit(f"unsupported base relocation type {typ} at {page + off:#x}")
            out.append(page + off)
        p += bsize
    return sorted(out)


def pstr(s):
    b = s.encode("latin-1")
    if len(b) > 255:
        sys.exit(f"name too long: {s}")
    return bytes([len(b)]) + b


def build(pe_path, out_path, name, description, device_id, ddk, static):
    d, image_base, dirs, secs = read_pe(pe_path)
    if len(dirs) > 1 and dirs[1][0]:
        sys.exit("the VxD image imports from a DLL; a VxD must reach the system only through INT 20h")
    for idx, what in ((9, "TLS"), (13, "delay-load imports")):
        if len(dirs) > idx and dirs[idx][0]:
            sys.exit(f"the VxD image has a {what} directory")
    ddb_name = f"{name}_DDB"
    exp = exports(secs, dirs)
    if ddb_name not in exp:
        sys.exit(f"{pe_path} does not export {ddb_name} (exports: {sorted(exp)})")

    body = [s for s in secs if s["name"] != ".reloc"]
    start = min(s["va"] for s in body)
    end = max(s["va"] + s["vsize"] for s in body)
    image = bytearray(end - start)
    for s in body:
        image[s["va"] - start:s["va"] - start + len(s["raw"])] = s["raw"]

    # base relocations -> internal 32-bit offset fixups into object 1
    fixups = []  # (object offset of the field, target object offset)
    for rva in base_relocs(secs, dirs):
        if not (start <= rva and rva + 4 <= end):
            if any(s["name"] == ".reloc" and s["va"] <= rva < s["va"] + s["vsize"] for s in secs):
                continue
            sys.exit(f"relocation at {rva:#x} outside the image")
        off = rva - start
        value, = struct.unpack_from("<I", image, off)
        target = value - image_base - start
        if not 0 <= target <= len(image):
            sys.exit(f"relocation at {rva:#x} points outside the image ({value:#x})")
        struct.pack_into("<I", image, off, 0)  # the loader stores the address
        fixups.append((off, target))

    npages = max(1, (len(image) + PAGE - 1) // PAGE)
    last_page_bytes = len(image) - (npages - 1) * PAGE

    # fixup records per page; a field that straddles a page boundary is listed
    # in both pages (negative source offset in the second), as Microsoft does
    per_page = [bytearray() for _ in range(npages)]

    def record(page, src_off, target):
        flags = 0x00  # internal reference, 8-bit object number
        tgt = struct.pack("<H", target) if target < 0x10000 else struct.pack("<I", target)
        if target >= 0x10000:
            flags |= 0x10  # 32-bit target offset
        per_page[page] += struct.pack("<BBhB", 0x07, flags, src_off, 1) + tgt

    for off, target in fixups:
        page = off // PAGE
        record(page, off - page * PAGE, target)
        if (off + 3) // PAGE != page:
            record(page + 1, off - (page + 1) * PAGE, target)

    # ---- tables ------------------------------------------------------
    HDR = 0xC4
    obj_table = struct.pack("<6I", len(image), 0, OBJ_FLAGS, 1, npages, 0)
    page_map = b"".join(bytes([(i >> 16) & 0xFF, (i >> 8) & 0xFF, i & 0xFF, 0]) for i in range(1, npages + 1))
    resnames = pstr(name) + struct.pack("<H", 0) + b"\0"
    entry = struct.pack("<BBHBI", 1, 3, 1, 3, exp[ddb_name] - start) + b"\0"
    fix_page_table = b""
    pos = 0
    for p in per_page:
        fix_page_table += struct.pack("<I", pos)
        pos += len(p)
    fix_page_table += struct.pack("<I", pos)
    fix_records = b"".join(bytes(p) for p in per_page)
    nonres = pstr(description) + struct.pack("<H", 0) + pstr(ddb_name) + struct.pack("<H", 1) + b"\0"

    o_obj = HDR
    o_pmap = o_obj + len(obj_table)
    o_res = o_pmap + len(page_map)
    o_entry = o_res + len(resnames)
    o_fpt = o_entry + len(entry)
    o_frec = o_fpt + len(fix_page_table)
    o_imp = o_frec + len(fix_records)
    le_end = 0x80 + o_imp
    data_off = (le_end + 0x1FF) & ~0x1FF
    pages_blob = bytes(image) + b""  # last page is short, as in LE
    nonres_off = data_off + len(pages_blob)

    h = bytearray(HDR)
    h[0:2] = b"LE"
    struct.pack_into("<BBI", h, 2, 0, 0, 0)
    struct.pack_into("<HHI", h, 0x08, 2, 4, 0)          # 386, "Windows 386" (VxD), version
    struct.pack_into("<I", h, 0x10, MODULE_STATIC if static else MODULE_DYNAMIC)
    struct.pack_into("<I", h, 0x14, npages)
    struct.pack_into("<II", h, 0x28, PAGE, last_page_bytes)
    struct.pack_into("<II", h, 0x30, o_imp - o_fpt, 0)   # fixup section size, checksum
    struct.pack_into("<II", h, 0x38, o_fpt - o_obj, 0)   # loader section size, checksum
    struct.pack_into("<IIII", h, 0x40, o_obj, 1, o_pmap, 0)
    struct.pack_into("<II", h, 0x50, 0, 0)
    struct.pack_into("<II", h, 0x58, o_res, o_entry)
    struct.pack_into("<II", h, 0x60, 0, 0)
    struct.pack_into("<II", h, 0x68, o_fpt, o_frec)
    struct.pack_into("<IIII", h, 0x70, o_imp, 0, o_imp, 0)
    struct.pack_into("<II", h, 0x80, data_off, npages)   # data pages (file offset), preload pages
    struct.pack_into("<III", h, 0x88, nonres_off, len(nonres), 0)
    struct.pack_into("<HH", h, 0xC0, device_id, ddk)

    out = bytearray(MZ_STUB) + h + obj_table + page_map + resnames + entry + fix_page_table + fix_records
    assert len(out) == le_end
    out += b"\0" * (data_off - len(out))
    out += pages_blob + nonres
    open(out_path, "wb").write(out)
    return dict(pages=npages, fixups=len(fixups), size=len(out), ddb=exp[ddb_name] - start)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pe")
    ap.add_argument("out")
    ap.add_argument("--name", required=True)
    ap.add_argument("--description", required=True)
    ap.add_argument("--device-id", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--ddk", type=lambda x: int(x, 0), default=0x40A)
    ap.add_argument("--static", action="store_true", help="static-only VxD (default: dynamic, as KernelEx.def)")
    a = ap.parse_args()
    info = build(a.pe, a.out, a.name, a.description, a.device_id, a.ddk, a.static)
    print(f"{a.out}: {info['size']} bytes, {info['pages']} page(s), {info['fixups']} fixups, DDB at {info['ddb']:#x}")


if __name__ == "__main__":
    main()
