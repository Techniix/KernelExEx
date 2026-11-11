#!/usr/bin/env python3
"""Build the OpenBSD network drivers in drivers/kexnet as NDIS 5 miniports:
kexem.sys (em, Intel PRO/1000), kexrndis.sys (urndis, USB RNDIS) and
kexcdce.sys (our own CDC ECM driver), kexurtwn.sys (urtwn + net80211),
kexrtwn.sys (FreeBSD rtwn on OpenBSD net80211: two personalities in one driver).
Builds kexbsd.sys first (they import from it).

usage: build_kexnet.py [--out DIR] [--sdk DIR] [--debugcon]
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
    extra = ["-I", str(BSD / "include" / "obsd"), "-I", str(BSD / "include"),
             "-DNDIS_MINIPORT_DRIVER", "-DNDIS50_MINIPORT=1", "-DKEXBSD", "-DSMALL_KERNEL", "-fms-extensions", "-Wno-microsoft-anon-tag",   # ndis.h nests structs anonymously
             # OpenBSD's em sources, as they are: quiet the warnings they carry
             "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function",
             "-Wno-sign-compare", "-Wno-pointer-sign", "-Wno-parentheses-equality",
             "-Wno-tautological-constant-out-of-range-compare", "-Wno-unused-value"]
    if a.debugcon:
        extra.append("-DKEX_DEBUGCON")
    em = SRC / "em" / "dev" / "pci"
    rt = wdm.ROOT / "net/rt/w9xrt_div.c"
    tables = {"kexbsd.sys": wdm.exports_of(bsdout / "kexbsd.sys")}
    USBLIB = [SRC / "obsd_net.c", BSD / "lib" / "obsd_usb.c", BSD / "lib" / "obsd_kern.c", rt]
    WIFI = USBLIB + sorted((SRC / "wifi" / "net80211").glob("*.c")) + sorted((SRC / "wifi" / "crypto").glob("*.c")) \
        + [SRC / "wifi" / "kex80211_rand.c", SRC / "wifi" / "kex80211_cfg.c", SRC / "wifi" / "kex80211_pbkdf2.c", BSD / "lib" / "subr_tree.c"]
    drivers = {
        # obsd_usb.c in every driver: the glue (obsd_net.c) serves PCI and USB drivers
        "kexem": ([SRC / "kexem.c", SRC / "obsd_net.c", BSD / "lib" / "obsd_usb.c", BSD / "lib" / "obsd_kern.c", em / "if_em.c", em / "if_em_hw.c", rt],
                  ["-I", str(SRC / "em")]),
        "kexrndis": ([SRC / "kexrndis.c", SRC / "urndis" / "dev" / "usb" / "if_urndis.c"] + USBLIB, ["-I", str(SRC / "urndis")]),
        "kexcdce": ([SRC / "cdce" / "kexcdce.c"] + USBLIB, ["-I", str(SRC)]),
        "kexurtwn": ([SRC / "urtwn" / "kexurtwn.c", SRC / "urtwn" / "dev" / "ic" / "rtwn.c"] + WIFI,
                     ["-I", str(SRC / "urtwn"), "-I", str(SRC / "wifi"), "-I", str(SRC)]),
        # the wireless stack alone, to prove it links (never installed)
        "kexwlan_linktest": ([SRC / "wifi" / "kexwlan_linktest.c"] + WIFI, ["-I", str(SRC / "wifi"), "-I", str(SRC)]),
    }
    # kexrtwn: FreeBSD's rtwn and its FreeBSD personality, compiled apart
    RT = SRC / "rtwn"
    fbsd_srcs = [p for p in sorted((RT / "dev" / "rtwn").rglob("*.c")) if p.name != "rtwn_usb_attach.c"] + [
        RT / "kexrtwn_fbsd.c", RT / "compat" / "kexfbsd_kern.c", RT / "compat" / "kexfbsd_usb.c",
        RT / "net80211" / "ieee80211_phy.c"]
    fbsd_flags = ["-I", str(RT / "compat"), "-I", str(RT / "compat" / "dev" / "usb"), "-I", str(RT),
                  "-I", str(BSD / "include"), "-include", str(RT / "compat" / "kexfbsd.h"), "-DKEXBSD",
                  "-Wno-unused-variable", "-Wno-unused-function", "-Wno-sign-compare", "-Wno-unused-but-set-variable",
                  "-Wno-pointer-sign", "-Wno-parentheses-equality", "-Wno-tautological-constant-out-of-range-compare",
                  "-Wno-unused-value", "-Wno-incompatible-pointer-types-discards-qualifiers", "-Wno-missing-braces"]
    if a.debugcon:
        fbsd_flags.append("-DKEX_DEBUGCON")
    drivers["kexrtwn"] = ([RT / "kexrtwn_obsd.c"] + WIFI, ["-I", str(RT), "-I", str(SRC / "wifi"), "-I", str(SRC)])
    for name, (srcs, inc) in drivers.items():
        dout = out / name
        dout.mkdir(exist_ok=True)
        objs = wdm.compile(srcs, dout, Path(a.sdk), extra + inc)
        if name == "kexrtwn":
            fout = dout / "fbsd"
            fout.mkdir(exist_ok=True)
            fobjs = wdm.compile(fbsd_srcs, fout, Path(a.sdk), fbsd_flags)
            # the FreeBSD side's net80211 names collide with OpenBSD's: rename them there
            def defined(o):
                return {l.split()[-1] for l in wdm.run(["llvm-nm", "--defined-only", "--extern-only", o]).splitlines()
                        if len(l.split()) == 3}
            odef = set().union(*(defined(o) for o in objs))
            fdef = set().union(*(defined(o) for o in fobjs))
            clash = sorted(odef & fdef)
            if clash:
                (fout / "rename.txt").write_text("".join(f"{c} {c[0]}kexfbsd_{c[1:]}\n" for c in clash))
                for o in fobjs:
                    wdm.run(["llvm-objcopy", f"--redefine-syms={fout / 'rename.txt'}", o])
            objs = objs + fobjs
        sysf = wdm.link(name, objs, dout, extra_tables=tables)
        (out / f"{name}.sys").write_bytes(sysf.read_bytes())
        imports = [l.split(":")[1].strip() for l in wdm.run(["llvm-objdump", "-p", sysf]).splitlines() if "DLL Name" in l]
        print(f"{out / (name + '.sys')}: {sysf.stat().st_size} bytes, imports {', '.join(imports)}")


if __name__ == "__main__":
    main()
