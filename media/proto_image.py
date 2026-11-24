#!/usr/bin/env python3
"""Prototype (host, Linux): build KEXSETUP.IMG, the FAT16 disk image the
install media boots into (through MEMDISK), from the user's own Windows 98
CD. Usage: proto_image.py BOOTFLOPPY.IMG WIN98DIR EXTRA_DIR OUT.IMG
EXTRA_DIR's files go to the image's root (our DOS stage)."""
import os, shutil, struct, subprocess, sys, tempfile

SPT, HEADS, START = 63, 255, 63


def chs(lba):
    c = lba // (SPT * HEADS)
    if c > 1023:
        return bytes([254, 0xFF, 0xFF])
    h = (lba // SPT) % HEADS
    s = lba % SPT + 1
    return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])


def run(*a):
    subprocess.run([str(x) for x in a], check=True)


def main():
    floppy, win98, extra, out = sys.argv[1:5]
    mbr = bytearray(open(os.path.join(os.path.dirname(__file__), "mbr.bin"), "rb").read())
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(win98) for f in fs)
    size += sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(extra) for f in fs)
    # whole cylinders of 255 heads x 63 sectors, so MEMDISK and the BPB agree on the geometry
    cyl = SPT * HEADS
    nsec = ((size + 48 * 1024 * 1024) // 512 + START + cyl - 1) // cyl * cyl - START
    tmp = tempfile.mkdtemp()
    part = os.path.join(tmp, "part.img")
    run("mkfs.fat", "-F", "16", "-C", "-h", START, "-g", f"{HEADS}/{SPT}", "-M", "0xF8", "-D", "0x80",
        "-n", "KEXSETUP", "-i", "4B455831", part, nsec // 2)
    vbr = bytearray(open(part, "rb").read(512))
    fl = open(floppy, "rb").read(512)
    # the user's own boot code (from their CD's boot floppy) after our BPB
    vbr[0x3E:0x1FE] = fl[0x3E:0x1FE]
    vbr[0:3] = fl[0:3]
    vbr[3:11] = b"MSWIN4.1"
    with open(part, "r+b") as f:
        f.write(vbr)
    # IO.SYS first and in one piece, then the rest
    io = os.path.join(tmp, "IO.SYS")
    # every file of the CD's boot floppy (IO.SYS first, in one piece), except its CD-ROM
    # helpers: they bring the edition's console and keyboard drivers (Korean: BILING.SYS,
    # MSHBIOS.COM)
    listing = subprocess.run(["mdir", "-a", "-b", "-i", floppy, "::/"], capture_output=True, text=True).stdout
    names = [l.strip().split("/")[-1] for l in listing.splitlines() if l.strip()]
    skip = {"JO.SYS", "AUTOEXEC.BAT", "CONFIG.SYS", "MSCDEX.EXE", "FINDCD.EXE", "OAKCDROM.SYS", "BTCDROM.SYS",
            "BTDOSM.SYS", "FLASHPT.SYS", "ASPI2DOS.SYS", "ASPI4DOS.SYS", "ASPI8DOS.SYS", "ASPI8U2.SYS", "ASPICD.SYS",
            "EMM386.EXE", "SETUP.EXE", "OEMSETUP.EXE"}
    names = [n for n in names if n.upper() not in skip and "." in n]
    names.sort(key=lambda n: (n.upper() != "IO.SYS", n.upper() != "MSDOS.SYS", n))
    for n in names:
        run("mcopy", "-i", floppy, f"::/{n}", os.path.join(tmp, n))
    for n in names:
        run("mcopy", "-i", part, os.path.join(tmp, n), "::/")
    # its language set-up: CONFIG.SYS [COMMON], and AUTOEXEC.BAT up to the CD-ROM part.
    # The AUTOEXEC part (e.g. MSHBIOS) is for the RAM disk only: on the hard disk's first
    # start it crashed (jump into video memory), and Windows Setup sets the language up itself.
    def floppy_text(name):
        return subprocess.run(["mtype", "-i", floppy, "::/" + name], capture_output=True).stdout.decode("latin-1")
    common, sect = [], None
    for l in floppy_text("CONFIG.SYS").splitlines():
        t = l.strip()
        if t.startswith("["):
            sect = t.upper()
            continue
        u = t.upper()
        if sect == "[COMMON]" and t and not u.startswith(("FILES", "BUFFERS", "LASTDRIVE", "DOS=", "STACKS")):
            if t.lower() not in [c.lower() for c in common]:
                common.append(t)
    prefix = []
    for l in floppy_text("AUTOEXEC.BAT").splitlines():
        t = l.strip()
        u = t.upper()
        if "MSCDEX" in u or u.startswith("IF "):
            break
        if not t or u.startswith(("@ECHO", "ECHO", "PATH", "SET TEMP", "SET TMP", "CLS")):
            continue
        prefix.append(t)
    core = {"IO.SYS", "MSDOS.SYS", "COMMAND.COM", "HIMEM.SYS", "EXTRACT.EXE", "FDISK.EXE", "DRVSPACE.BIN"}
    lang = [n for n in names if n.upper() not in core]
    os.makedirs(os.path.join(tmp, "LANG"), exist_ok=True)
    for n in lang:
        os.replace(os.path.join(tmp, n), os.path.join(tmp, "LANG", n))
    for name, add, front in (("CONFIG.SYS", common, False), ("AUTOEXEC.BAT", prefix, True),
                             ("KEXINST/T_CONFIG.SYS", common, False), ("KEXINST/T_AUTOEX.BAT", [], True)):
        src = os.path.join(extra, name)
        ours = open(src, "rb").read().decode("latin-1").splitlines() if os.path.exists(src) else []
        if front:
            lines = ours[:1] + add + ours[1:]   # after our @echo off
        else:
            lines = ours + add
        out_name = os.path.join(tmp, name.replace("/", "_"))
        open(out_name, "wb").write(("\r\n".join(lines) + "\r\n").encode("latin-1"))
    run("mattrib", "-i", part, "+s", "+h", "+r", "::/IO.SYS", "::/MSDOS.SYS")
    for f in sorted(os.listdir(extra)):
        if f.upper() in ("CONFIG.SYS", "AUTOEXEC.BAT"):
            continue
        run("mcopy", "-s", "-i", part, os.path.join(extra, f), "::/")
    for f in ("CONFIG.SYS", "AUTOEXEC.BAT"):
        run("mcopy", "-i", part, os.path.join(tmp, f), "::/")
    for f in ("T_CONFIG.SYS", "T_AUTOEX.BAT"):
        run("mcopy", "-o", "-i", part, os.path.join(tmp, "KEXINST_" + f), "::/KEXINST/" + f)
    run("mmd", "-i", part, "::/KEXINST/LANG")
    for n in lang:
        run("mcopy", "-i", part, os.path.join(tmp, "LANG", n), "::/KEXINST/LANG/")
    run("mcopy", "-s", "-i", part, win98, "::/WIN98")
    # the disk: our MBR, one active FAT16 partition from sector 63
    total = START + nsec
    entry = bytes([0x80]) + chs(START) + bytes([0x06]) + chs(total - 1) + struct.pack("<II", START, nsec)
    mbr[0x1B8:0x1BC] = b"KXSU"
    mbr[0x1BE:0x1CE] = entry
    with open(out, "wb") as f:
        f.write(mbr)
        f.write(bytes(512 * (START - 1)))
        with open(part, "rb") as p:
            while True:
                b = p.read(1 << 20)
                if not b:
                    break
                f.write(b)
    print(f"{out}: {total * 512 // 1048576} MB")


if __name__ == "__main__":
    # main() works in a temporary folder of ~150 MB: always remove it, also on errors
    _mkdtemp = tempfile.mkdtemp
    _made = []
    tempfile.mkdtemp = lambda *a, **k: _made.append(_mkdtemp(*a, **k)) or _made[-1]
    try:
        main()
    finally:
        for d in _made:
            shutil.rmtree(d, ignore_errors=True)
