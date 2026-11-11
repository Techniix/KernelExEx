#!/usr/bin/env python3
"""Build the KernelExEx audio drivers in drivers/audio as WDM drivers for
PortCls on Windows 98 SE / ME: kexac97.sys (AC'97 on Intel ICH and
compatibles).

usage: build_kexaudio.py [--out DIR] [--sdk DIR] [--debugcon]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wdm  # noqa: E402

SRC = wdm.ROOT / "drivers" / "audio"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(wdm.ROOT.parent / "build" / "kexaudio"))
    ap.add_argument("--sdk", default=str(wdm.ROOT.parent / "build" / "sdk"))
    ap.add_argument("--debugcon", action="store_true", help="print to port 0xE9 (QEMU -debugcon)")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    extra = ["-DKEX_DEBUGCON"] if a.debugcon else []
    rt = wdm.ROOT / "net/rt/w9xrt_div.c"   # 64-bit division helpers
    drivers = {"kexac97": [SRC / "ac97" / "kexac97.c", rt]}
    for name, srcs in drivers.items():
        dout = out / name
        dout.mkdir(exist_ok=True)
        objs = wdm.compile(srcs, dout, Path(a.sdk), extra)
        sysf = wdm.link(name, objs, dout)
        (out / f"{name}.sys").write_bytes(sysf.read_bytes())
        print(wdm.describe(out / f"{name}.sys") if hasattr(wdm, "describe") else f"{out / name}.sys")


if __name__ == "__main__":
    main()
