#!/usr/bin/env python3
"""Build drivers/kexnet/kexem.sys, the NDIS 5 front end for OpenBSD em.
Builds kexbsd.sys first (kexem imports from it).

usage: build_kexem.py [--out DIR] [--sdk DIR] [--debugcon]
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wdm  # noqa: E402

SRC = wdm.ROOT / "drivers" / "kexnet"
BSD = wdm.ROOT / "drivers" / "kexbsd"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(wdm.ROOT.parent / "build" / "kexnet"))
    ap.add_argument("--sdk", default=str(wdm.ROOT.parent / "build" / "sdk"))
    ap.add_argument("--debugcon", action="store_true")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    bsdout = out / "kexbsd"
    subprocess.run([sys.executable, Path(__file__).parent / "build_kexbsd.py", "--out", bsdout, "--sdk", a.sdk]
                   + (["--debugcon"] if a.debugcon else []), check=True)
    extra = ["-I", str(BSD / "include" / "obsd"), "-I", str(BSD / "include"), "-I", str(SRC / "em"),
             "-DNDIS_MINIPORT_DRIVER", "-DNDIS50_MINIPORT=1", "-DKEXBSD", "-DSMALL_KERNEL", "-fms-extensions", "-Wno-microsoft-anon-tag",   # ndis.h nests structs anonymously
             # OpenBSD's em sources, as they are: quiet the warnings they carry
             "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function",
             "-Wno-sign-compare", "-Wno-pointer-sign", "-Wno-parentheses-equality",
             "-Wno-tautological-constant-out-of-range-compare", "-Wno-unused-value"]
    if a.debugcon:
        extra.append("-DKEX_DEBUGCON")
    em = SRC / "em" / "dev" / "pci"
    objs = wdm.compile([SRC / "kexem.c", SRC / "obsd_net.c", em / "if_em.c", em / "if_em_hw.c",
                        wdm.ROOT / "net/rt/w9xrt_div.c"], out, Path(a.sdk), extra)
    sysf = wdm.link("kexem", objs, out, extra_tables={"kexbsd.sys": wdm.exports_of(bsdout / "kexbsd.sys")})
    print(wdm.run(["llvm-objdump", "-p", sysf]).split("DLL Name")[1:] and
          "imports: " + ", ".join(l.split(":")[1].strip() for l in wdm.run(["llvm-objdump", "-p", sysf]).splitlines()
                                  if "DLL Name" in l))
    print(f"{sysf}: {sysf.stat().st_size} bytes")


if __name__ == "__main__":
    main()
