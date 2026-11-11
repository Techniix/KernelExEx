#!/usr/bin/env python3
"""Build drivers/kexbsd: kexbsd.sys, the BSD kernel API export driver.

usage: build_kexbsd.py [--out DIR] [--sdk DIR] [--debugcon]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wdm  # noqa: E402

SRC = wdm.ROOT / "drivers" / "kexbsd"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(wdm.ROOT.parent / "build" / "kexbsd"))
    ap.add_argument("--sdk", default=str(wdm.ROOT.parent / "build" / "sdk"))
    ap.add_argument("--debugcon", action="store_true")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    extra = ["-I", str(SRC / "include")] + (["-DKEX_DEBUGCON"] if a.debugcon else [])
    # 64-bit division helpers come from the 9x runtime (freestanding C)
    objs = wdm.compile(sorted((SRC / "src").glob("*.c")) + [wdm.ROOT / "net/rt/w9xrt_div.c"], out, Path(a.sdk), extra)
    sysf = wdm.link("kexbsd", objs, out, deffile=None, base="0x10000")
    exports = wdm.run(["llvm-objdump", "-p", sysf]).count("\n") and \
        [l.split()[-1] for l in wdm.run(["llvm-objdump", "-p", sysf]).split("Export Table")[-1].splitlines()
         if l.strip()[:1].isdigit() and len(l.split()) == 3]
    print(f"{sysf}: {sysf.stat().st_size} bytes, {len(exports)} exports")


if __name__ == "__main__":
    main()
