#!/usr/bin/env python3
"""Build the KernelExEx storage drivers in drivers/storage: kexata.mpd (IDE/ATA
and ATAPI with 48-bit LBA), NT SCSI miniports that Windows 98 SE's
SCSIPORT.PDR loads.

usage: build_kexstor.py [--out DIR] [--sdk DIR] [--debugcon]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wdm  # noqa: E402

SRC = wdm.ROOT / "drivers" / "storage"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(wdm.ROOT.parent / "build" / "kexstor"))
    ap.add_argument("--sdk", default=str(wdm.ROOT.parent / "build" / "sdk"))
    ap.add_argument("--debugcon", action="store_true", help="print to port 0xE9 (QEMU -debugcon)")
    ap.add_argument("-D", dest="defines", action="append", default=[], help="extra C define (tests)")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    extra = (["-DKEX_DEBUGCON"] if a.debugcon else []) + ["-D" + d for d in a.defines]
    rt = wdm.ROOT / "net/rt/w9xrt_div.c"   # 64-bit division helpers
    drivers = {"kexata": [SRC / "ata" / "kexata.c", rt]}
    for name, srcs in drivers.items():
        dout = out / name
        dout.mkdir(exist_ok=True)
        objs = wdm.compile(srcs, dout, Path(a.sdk), extra)
        sysf = wdm.link(name, objs, dout)
        (out / f"{name}.mpd").write_bytes(sysf.read_bytes())
        print(out / f"{name}.mpd")


if __name__ == "__main__":
    main()
