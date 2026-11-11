#!/usr/bin/env python3
"""Build the KernelExEx USB host controller drivers in drivers/usb as WDM
drivers for Windows 98 SE / ME: kexehci.sys (USB 2.0, EHCI). With --tests
also tests/usb/kexusbt.sys (a mass storage test driver, not shipped).

usage: build_kexusb.py [--out DIR] [--sdk DIR] [--debugcon] [--tests]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wdm  # noqa: E402

SRC = wdm.ROOT / "drivers" / "usb"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(wdm.ROOT.parent / "build" / "kexusb"))
    ap.add_argument("--sdk", default=str(wdm.ROOT.parent / "build" / "sdk"))
    ap.add_argument("--debugcon", action="store_true", help="print to port 0xE9 (QEMU -debugcon)")
    ap.add_argument("--tests", action="store_true", help="also build tests/usb/kexusbt.sys")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    extra = ["-DKEX_DEBUGCON"] if a.debugcon else []
    rt = wdm.ROOT / "net/rt/w9xrt_div.c"   # 64-bit division helpers
    drivers = {"kexehci": [SRC / "ehci" / "kexehci.c", rt]}
    if a.tests:
        drivers["kexusbt"] = [wdm.ROOT / "tests" / "usb" / "kexusbt.c", rt]
    for name, srcs in drivers.items():
        dout = out / name
        dout.mkdir(exist_ok=True)
        objs = wdm.compile(srcs, dout, Path(a.sdk), extra)
        sysf = wdm.link(name, objs, dout)
        (out / f"{name}.sys").write_bytes(sysf.read_bytes())
        print(wdm.describe(out / f"{name}.sys") if hasattr(wdm, "describe") else f"{out / name}.sys")


if __name__ == "__main__":
    main()
