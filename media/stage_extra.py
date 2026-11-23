#!/usr/bin/env python3
"""Collect the files KernelExEx adds to the setup RAM disk (the EXTRA_DIR of proto_image.py).

Usage: stage_extra.py BUILD_CORE KEXPREP.EXE KEXSETUP.EXE OUT_DIR [MSBATCH.INF]

BUILD_CORE is build_core.py's output (KEXMEM.SYS); KEXSETUP.EXE is the KernelExEx
installer. MSBATCH.INF replaces media/stage/MSBATCH.INF (tests only: an answer file
with a product key must never be shipped)."""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STAGE = os.path.join(HERE, "stage")

# file in media/stage -> path on the RAM disk
LAYOUT = {
    "AUTOEXEC.BAT": "AUTOEXEC.BAT",     # the stage script
    "CONFIG.SYS": "CONFIG.SYS",
    "MSBATCH.INF": "KEXINST/MSBATCH.INF",
    "T_AUTOEX.BAT": "KEXINST/T_AUTOEX.BAT",
    "T_CONFIG.SYS": "KEXINST/T_CONFIG.SYS",
    "T_MSDOS.SYS": "KEXINST/T_MSDOS.SYS",
}


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    core, kexprep, kexsetup, out = sys.argv[1:5]
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "KEXINST"))
    for src, dst in LAYOUT.items():
        shutil.copyfile(os.path.join(STAGE, src), os.path.join(out, dst))
    if len(sys.argv) == 6:
        shutil.copyfile(sys.argv[5], os.path.join(out, "KEXINST/MSBATCH.INF"))
    shutil.copyfile(os.path.join(core, "KEXMEM.SYS"), os.path.join(out, "KEXMEM.SYS"))
    shutil.copyfile(os.path.join(core, "KEXLOG.COM"), os.path.join(out, "KEXLOG.COM"))
    shutil.copyfile(kexprep, os.path.join(out, "KEXPREP.EXE"))
    shutil.copyfile(kexsetup, os.path.join(out, "KEXINST/KEXSETUP.EXE"))
    print(out)


if __name__ == "__main__":
    main()
