#!/usr/bin/env python3
"""Build the KernelExEx installer on Linux with NSIS (makensis).

Stages the outputs of toolchain/build_core.py and net/tools/build_win9x.py
in the directory layout KernelEx.nsi expects (the layout the Visual C++
build used to leave behind), then runs makensis on it.

makensis comes from NSIS 3.x built for this host, with ANSI stubs and the
Banner, nsDialogs, NSISdl and System plug-ins cross-compiled for Windows 9x
(see toolchain/README.md). ANSI installers run on Windows 98 SE and ME;
Unicode NSIS installers do not.

Usage: build_installer.py --core <build/core> --net <build/win9x> --nsis <prefix> --out <dir>
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# path in the installer script -> where it comes from
# ("core:" = build_core.py output, "net:" = build_win9x.py output,
#  "kexpad:" = toolchain/wdm/build_kexpad.py output, "kexnet:" = build_kexnet.py
#  output, else repository)
LAYOUT = {
    "Core/Release/KernelEx.dll": "core:KernelEx.dll",
    "apilibs/kexbases/Release/kexbases.dll": "core:kexbases.dll",
    "apilibs/kexbasen/Release/kexbasen.dll": "core:kexbasen.dll",
    "sheet/Release/sheet.dll": "core:sheet.dll",
    "kexCOM/Release/kexCOM.dll": "core:kexCOM.dll",
    "vxd/Release/VKrnlEx.vxd": "core:VKrnlEx.vxd",
    "verify/Release/verify.exe": "core:verify.exe",
    "auxiliary/msimg32.dll": "core:auxiliary/msimg32.dll",
    "auxiliary/pdh.dll": "core:auxiliary/pdh.dll",
    "auxiliary/psapi.dll": "core:auxiliary/psapi.dll",
    "auxiliary/uxtheme.dll": "core:auxiliary/uxtheme.dll",
    "auxiliary/wtsapi32.dll": "core:auxiliary/wtsapi32.dll",
    "auxiliary/userenv.dll": "core:auxiliary/userenv.dll",
    "net-out/kextls.dll": "net:kextls.dll",
    "net-out/bcrypt.dll": "net:bcrypt.dll",
    "net-out/winhttp.dll": "net:winhttp.dll",
    "net-out/kexrt.dll": "net:kexrt.dll",
    "net-out/xinput1_3.dll": "net:xinput1_3.dll",
    "net-out/kexuni.dll": "net:kexuni.dll",
    "net-out/kexntp.exe": "net:kexntp.exe",
    "input/gamepad/gamecontrollerdb.txt": "input/gamepad/gamecontrollerdb.txt",
    "input/kexpad/kexpad.sys": "kexpad:kexpad.sys",
    "input/kexpad/kexpad.inf": "input/kexpad/kexpad.inf",
    "drivers/kexnet/kexem.sys": "kexnet:kexem.sys",
    "drivers/kexnet/kexbsd.sys": "kexnet:kexbsd/kexbsd.sys",
    "drivers/kexnet/kexem.inf": "drivers/kexnet/kexem.inf",
    "drivers/kexnet/kexrndis.sys": "kexnet:kexrndis.sys",
    "drivers/kexnet/kexrndis.inf": "drivers/kexnet/kexrndis.inf",
    "net/certs/roots.pem": "net/certs/roots.pem",
    "apilibs/core.ini": "apilibs/CORE.INI",
    "apilibs/settings.reg": "apilibs/settings.reg",
    "license.txt": "License.txt",
    "License.txt": "License.txt",  # the script uses both spellings
    "Release Notes.txt": "Release Notes.txt",
    "util/sdbcreate/sdbdb/KernelEx.sdb": "util/sdbcreate/sdbdb/KernelEx.sdb",
    "util/sdbcreate/sdbdb/kexsdb.i.reg": "util/sdbcreate/sdbdb/kexsdb.i.reg",
    "util/sdbcreate/sdbdb/kexsdb.u.reg": "util/sdbcreate/sdbdb/kexsdb.u.reg",
    "KernelEx.nsi": "KernelEx.nsi",
    "UpdateDLL.nsh": "UpdateDLL.nsh",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True, type=Path)
    ap.add_argument("--net", required=True, type=Path)
    ap.add_argument("--kexpad", type=Path, default=ROOT.parent / "build" / "kexpad")
    ap.add_argument("--kexnet", type=Path, default=ROOT.parent / "build" / "kexnet")
    ap.add_argument("--nsis", required=True, type=Path, help="NSIS install prefix (bin/makensis)")
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()

    stage = a.out / "stage"
    shutil.rmtree(stage, ignore_errors=True)
    missing = []
    for dst, src in LAYOUT.items():
        if src.startswith("core:"):
            path = a.core / src[5:]
        elif src.startswith("net:"):
            path = a.net / src[4:]
        elif src.startswith("kexpad:"):
            path = a.kexpad / src[7:]
        elif src.startswith("kexnet:"):
            path = a.kexnet / src[7:]
        else:
            path = ROOT / src
        if not path.is_file():
            missing.append(str(path))
            continue
        (stage / dst).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, stage / dst)
    if missing:
        sys.exit("missing inputs:\n  " + "\n  ".join(missing))

    makensis = a.nsis / "bin" / "makensis"
    # -WX: any warning fails the build (one once hid the kextls install line)
    p = subprocess.run([str(makensis), "-V2", "-WX", "-INPUTCHARSET", "CP1252", "KernelEx.nsi"], cwd=stage,
                       capture_output=True, text=True)
    sys.stdout.write(p.stdout[-4000:])
    if p.returncode:
        sys.stderr.write(p.stderr)
        sys.exit("makensis failed")
    built = stage / "KernelEx-dev.exe"
    a.out.mkdir(parents=True, exist_ok=True)
    final = a.out / "KernelExEx-setup.exe"
    shutil.move(built, final)
    print(f"{final}: {final.stat().st_size} bytes")


if __name__ == "__main__":
    main()
