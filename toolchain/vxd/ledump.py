#!/usr/bin/env python3
"""Dump a Windows VxD (LE executable): header, objects, page map, names,
entry table, fixups and the DDB. Used to check what le_link.py writes
against VxDs built by Microsoft's linker.

Usage: ledump.py <file.vxd> [--fixups]
"""
import struct
import sys

HDR = [  # (offset, size, name)
    (0x02, 1, "byte_order"), (0x03, 1, "word_order"), (0x04, 4, "format_level"), (0x08, 2, "cpu"),
    (0x0A, 2, "os"), (0x0C, 4, "module_version"), (0x10, 4, "module_flags"), (0x14, 4, "pages"),
    (0x18, 4, "eip_obj"), (0x1C, 4, "eip"), (0x20, 4, "esp_obj"), (0x24, 4, "esp"), (0x28, 4, "page_size"),
    (0x2C, 4, "last_page_bytes"), (0x30, 4, "fixup_size"), (0x34, 4, "fixup_cksum"), (0x38, 4, "loader_size"),
    (0x3C, 4, "loader_cksum"), (0x40, 4, "obj_table"), (0x44, 4, "obj_count"), (0x48, 4, "obj_page_table"),
    (0x4C, 4, "iter_pages"), (0x50, 4, "rsrc_table"), (0x54, 4, "rsrc_count"), (0x58, 4, "resname_table"),
    (0x5C, 4, "entry_table"), (0x60, 4, "directives"), (0x64, 4, "directive_count"), (0x68, 4, "fixup_page_table"),
    (0x6C, 4, "fixup_rec_table"), (0x70, 4, "import_mod_table"), (0x74, 4, "import_mod_count"),
    (0x78, 4, "import_proc_table"), (0x7C, 4, "page_cksum_table"), (0x80, 4, "data_pages"), (0x84, 4, "preload_pages"),
    (0x88, 4, "nonres_table"), (0x8C, 4, "nonres_len"), (0x90, 4, "nonres_cksum"), (0x94, 4, "autodata_obj"),
    (0x98, 4, "debug_off"), (0x9C, 4, "debug_len"), (0xA0, 4, "inst_preload"), (0xA4, 4, "inst_demand"),
    (0xA8, 4, "heap"), (0xAC, 4, "stack"), (0xB0, 4, "res_b0"), (0xB4, 4, "res_b4"), (0xB8, 4, "res_b8"),
    (0xBC, 4, "res_bc"), (0xC0, 4, "winres_off"), (0xC4, 4, "winres_len"), (0xC8, 2, "device_id"),
    (0xCA, 2, "ddk_version"),
]


def parse(data):
    le = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[le:le + 2] == b"LE", "not an LE file"
    h = {"le": le}
    for off, size, name in HDR:
        h[name] = struct.unpack_from({1: "<B", 2: "<H", 4: "<I"}[size], data, le + off)[0]
    objs = []
    for i in range(h["obj_count"]):
        vsize, base, flags, pidx, npages, _ = struct.unpack_from("<6I", data, le + h["obj_table"] + 24 * i)
        objs.append(dict(vsize=vsize, base=base, flags=flags, page_index=pidx, pages=npages))
    pmap = []
    for i in range(h["pages"]):
        b = data[le + h["obj_page_table"] + 4 * i: le + h["obj_page_table"] + 4 * i + 4]
        pmap.append(((b[0] << 16) | (b[1] << 8) | b[2], b[3]))  # LE: 24-bit page number, high byte first
    names = []
    p = le + h["resname_table"]
    while data[p]:
        n = data[p]
        names.append((data[p + 1:p + 1 + n].decode("latin-1"), struct.unpack_from("<H", data, p + 1 + n)[0]))
        p += 3 + n
    entries = []
    p, ordinal = le + h["entry_table"], 1
    while data[p]:
        cnt, typ = data[p], data[p + 1]
        p += 2
        if typ == 0:
            ordinal += cnt
            continue
        obj = struct.unpack_from("<H", data, p)[0]
        p += 2
        for _ in range(cnt):
            if typ == 3:
                fl, off = struct.unpack_from("<BI", data, p)
                p += 5
            else:
                raise ValueError(f"entry bundle type {typ}")
            entries.append((ordinal, obj, fl, off))
            ordinal += 1
    fpt = [struct.unpack_from("<I", data, le + h["fixup_page_table"] + 4 * i)[0] for i in range(h["pages"] + 1)]
    fix = []
    for pg in range(h["pages"]):
        p = le + h["fixup_rec_table"] + fpt[pg]
        end = le + h["fixup_rec_table"] + fpt[pg + 1]
        while p < end:
            src, fl = data[p], data[p + 1]
            p += 2
            if src & 0x20:  # source list: count now, offsets after the target
                count = data[p]
                p += 1
            else:
                count = 0
                soff = struct.unpack_from("<h", data, p)[0]
                p += 2
            assert fl & 3 == 0, "only internal references handled"
            if fl & 0x40:
                obj = struct.unpack_from("<H", data, p)[0]
                p += 2
            else:
                obj = data[p]
                p += 1
            if (src & 0xF) == 2:  # selector fixup: no offset
                toff = None
            elif fl & 0x10:
                toff = struct.unpack_from("<I", data, p)[0]
                p += 4
            else:
                toff = struct.unpack_from("<H", data, p)[0]
                p += 2
            if count:
                for _ in range(count):
                    fix.append((pg + 1, src, fl, struct.unpack_from("<h", data, p)[0], obj, toff))
                    p += 2
            else:
                fix.append((pg + 1, src, fl, soff, obj, toff))
    nonres = []
    p = h["nonres_table"]
    while h["nonres_len"] and data[p]:
        n = data[p]
        nonres.append((data[p + 1:p + 1 + n].decode("latin-1"), struct.unpack_from("<H", data, p + 1 + n)[0]))
        p += 3 + n
    return h, objs, pmap, names, entries, fix, nonres


def object_bytes(data, h, objs, pmap, n):
    o = objs[n - 1]
    out = bytearray()
    for i in range(o["pages"]):
        num, fl = pmap[o["page_index"] - 1 + i]
        start = h["data_pages"] + (num - 1) * h["page_size"]
        size = h["last_page_bytes"] if num == h["pages"] else h["page_size"]
        out += data[start:start + size].ljust(h["page_size"], b"\0") if fl == 0 else b"\0" * h["page_size"]
    return bytes(out[:o["vsize"]].ljust(o["vsize"], b"\0"))


def main():
    data = open(sys.argv[1], "rb").read()
    h, objs, pmap, names, entries, fix, nonres = parse(data)
    print("header:", {k: (hex(v) if isinstance(v, int) else v) for k, v in h.items()})
    for i, o in enumerate(objs, 1):
        print(f"object {i}: vsize={o['vsize']:#x} base={o['base']:#x} flags={o['flags']:#06x} "
              f"pages {o['page_index']}+{o['pages']}")
    print("page map flags:", sorted(set(f for _, f in pmap)), "numbers sequential:",
          [n for n, _ in pmap] == list(range(1, len(pmap) + 1)))
    print("resident names:", names)
    print("non-resident names:", nonres)
    print("entries:", entries)
    kinds = {}
    for f in fix:
        kinds[(f[1], f[2])] = kinds.get((f[1], f[2]), 0) + 1
    print("fixups:", len(fix), "by (src,flags):", {f"{a:#x},{b:#x}": c for (a, b), c in kinds.items()})
    print("fixups crossing a page:", sum(1 for f in fix if f[3] < 0 or f[3] > h["page_size"] - 4))
    if "--fixups" in sys.argv:
        for f in fix[:40]:
            print("  ", f)
    for ordinal, obj, fl, off in entries:
        if ordinal == 1:
            b = object_bytes(data, h, objs, pmap, obj)
            (nxt, sdk, devno, maj, mnr, flags, name, order, ctrl, v86, pm, v86ip, pmip, ref, svc, nsvc, w32,
             prev, size, r1, r2, r3) = struct.unpack_from("<IHHBBH8sIIIIIIIIIIIIIII", b, off)
            print(f"DDB @obj{obj}+{off:#x}: sdk={sdk:#x} devid={devno:#x} ver={maj}.{mnr} flags={flags:#x} "
                  f"name={name!r} order={order:#x} ctrl={ctrl:#x} prev={prev:#x} size={size:#x} "
                  f"rsv={r1:#x},{r2:#x},{r3:#x}")


if __name__ == "__main__":
    main()
