#!/usr/bin/env python3
"""Cross-build the KernelEx net modules for Windows 95/98/ME with clang + lld.

  kextls.dll   Mbed TLS + DRBG + KernelEx API library (RtlGenRandom, rand_s)
  bcrypt.dll   CNG subset (RNG, hashes, HMAC) on kextls
  winhttp.dll  WinHTTP subset on kextls + Winsock 1.1

Code generation: i586 (no CMOV, no MMX/SSE), so every Pentium-class and
later CPU runs it. PE images: subsystem and OS version 4.0, 4 KB file
alignment, fixed image bases below 2 GB (private per process on 9x), no
ASLR/NX flags, no mingw-w64 runtime (net/rt replaces the few pieces needed).

Usage: build_win9x.py --sdk <dir from prepare_sdk.py> --out <dir> [-j N]
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
from pathlib import Path

NET = Path(__file__).resolve().parent.parent
MBED = NET / "third_party" / "mbedtls"
TARGET = ["--target=i686-w64-windows-gnu", "-march=i586", "-mtune=pentium2"]

MODULES = {
    "kextls": {
        "base": "0x66000000",
        "sources": sorted((MBED / "library").glob("*.c")) + [
            NET / "kextls/src" / f for f in
            ("kex_entropy.c", "drbg.c", "kex_platform.c", "truststore.c", "session.c", "digest.c", "apilib.c", "dllmain.c")],
        "defines": ["-DKEXTLS_BUILD"],
        "libs": ["kernel32", "msvcrt"],
        "kill_at": False,
    },
    "bcrypt": {
        "base": "0x66800000",
        "sources": [NET / "bcrypt/bcrypt.c"],
        "defines": [],
        "libs": ["kextls", "kernel32", "msvcrt"],
        "kill_at": True,
    },
    "winhttp": {
        "base": "0x66A00000",
        "sources": [NET / "winhttp" / f for f in ("whttp_core.c", "whttp_sock.c", "winhttp_api.c")],
        "defines": [],
        "libs": ["kextls", "wsock32", "advapi32", "kernel32", "msvcrt"],
        "kill_at": True,
        "def": NET / "winhttp/winhttp.def",
    },
}
RT = [NET / "rt/w9xrt.c", NET / "rt/w9xrt_div.c", NET / "rt/chkstk.S"]
DLLENTRY = NET / "rt/dllentry.c"


def write_import_lib(name: str, objs, out: Path):
    """Import library with decorated stdcall symbols (_Fn@N) whose import
    names are undecorated (Fn), as in the Windows SDK, via llvm-dlltool -k."""
    exported = set()
    for o in objs:
        nm = subprocess.run(["llvm-nm", "--defined-only", "-g", o], capture_output=True, text=True).stdout
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] == "T":
                exported.add(parts[2])
    lines = [f"LIBRARY {name}.dll", "EXPORTS"]
    for sym in sorted(exported):
        if sym.startswith("_") and "@" in sym:
            lines.append(sym[1:])
    d = out / f"{name}.imp.def"
    d.write_text("\n".join(lines) + "\n")
    subprocess.run(["llvm-dlltool", "-m", "i386", "-k", "-d", str(d), "-l", str(out / f"lib{name}.a")], check=True)


def cflags(sdk: Path):
    mingw_inc = (sdk / "MINGW_INCLUDE").read_text().strip()
    epoch = (NET / "certs/EPOCH").read_text().strip()
    return TARGET + [
        "-O2", "-g0", "-std=gnu99", "-ffunction-sections", "-fdata-sections",
        "-fno-asynchronous-unwind-tables", "-fno-stack-protector", "-fno-ident",
        "-D_WIN32_WINNT=0x0410", "-DWINVER=0x0410", "-D__MSVCRT_VERSION__=0x600",
        "-D__USE_MINGW_ANSI_STDIO=0", "-DWIN32_LEAN_AND_MEAN",
        "-DMBEDTLS_CONFIG_FILE=\"kex_mbedtls_config.h\"", f"-DKEXTLS_TRUST_EPOCH={epoch}LL",
        "-nostdinc", "-isystem", subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True,
                                                check=True).stdout.strip() + "/include",
        "-isystem", str(sdk / "include"), "-isystem", str(Path(mingw_inc).parent / "crt"), "-isystem", mingw_inc,
        f"-I{NET}/kextls/config", f"-I{NET}/kextls/include", f"-I{NET}/kextls/src", f"-I{NET}/rt",
        f"-I{NET}/winhttp", f"-I{MBED}/include", f"-I{MBED}/library",
    ]


def compile_one(src: Path, obj: Path, flags, quiet: bool):
    if obj.exists() and obj.stat().st_mtime > src.stat().st_mtime and \
            obj.stat().st_mtime > (NET / "kextls/config/kex_mbedtls_config.h").stat().st_mtime:
        return None
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["clang", *flags, *(["-w"] if quiet else ["-Wall", "-Wno-unused-function"]), "-c", str(src), "-o", str(obj)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        return f"{src}:\n{p.stderr}"
    if p.stderr.strip() and not quiet:
        print(p.stderr.strip())
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    out = a.out.resolve()
    objdir = out / "obj"
    flags = cflags(a.sdk.resolve())

    jobs = []
    for src in RT + [DLLENTRY]:
        jobs.append((src, objdir / "rt" / (src.stem + ".o"), flags, False))
    for name, m in MODULES.items():
        for src in m["sources"]:
            quiet = MBED in src.parents
            jobs.append((src, objdir / name / (src.stem + ".o"), flags + m["defines"], quiet))
    errors = []
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        for err in ex.map(lambda j: compile_one(*j), jobs):
            if err:
                errors.append(err)
    if errors:
        print("\n".join(errors))
        return 1

    rt_objs = [str(objdir / "rt" / (s.stem + ".o")) for s in RT]
    for name, m in MODULES.items():
        objs = [str(objdir / name / (s.stem + ".o")) for s in m["sources"]]
        dll = out / f"{name}.dll"
        cmd = ["clang", *TARGET, "-fuse-ld=lld", "-nostdlib", "-shared", "-o", str(dll), *objs, *rt_objs,
               str(objdir / "rt" / "dllentry.o"),
               f"-L{a.sdk.resolve() / 'lib'}", f"-L{out}", *[f"-l{l}" for l in m["libs"]],
               "-Wl,--entry=DllMainCRTStartup@12",
               "-Wl,--subsystem,windows:4.0",
               "-Wl,--major-os-version,4", "-Wl,--minor-os-version,0",
               "-Wl,--major-image-version,4", "-Wl,--minor-image-version,0",
               f"-Wl,--image-base,{m['base']}",
               "-Wl,--disable-dynamicbase", "-Wl,--disable-nxcompat", "-Wl,--disable-high-entropy-va",
               "-Wl,--file-alignment,4096",
               "-Wl,--gc-sections", "-Wl,--no-insert-timestamp", "-Wl,--disable-auto-import",
               "-Wl,--disable-runtime-pseudo-reloc",
               f"-Wl,--out-implib,{out / ('lib' + name + '.a')}"]
        if m.get("kill_at"):
            cmd.append("-Wl,--kill-at")
        if m.get("def"):
            cmd.append(str(m["def"]))
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            print(f"link {name} failed:\n{p.stderr}")
            return 1
        if m.get("kill_at"):
            write_import_lib(name, objs, out)
        print(f"built {dll} ({dll.stat().st_size // 1024} KB)")

    # the on-target self-test program
    src = NET / "tests/win9x/kexnet_test.c"
    obj = objdir / "tests" / "kexnet_test.o"
    err = compile_one(src, obj, flags, False)
    if err:
        print(err)
        return 1
    exe = out / "kexnet_test.exe"
    cmd = ["clang", *TARGET, "-fuse-ld=lld", "-nostdlib", "-o", str(exe), str(obj), *rt_objs,
           f"-L{a.sdk.resolve() / 'lib'}", f"-L{out}", "-lwinhttp", "-lbcrypt", "-luser32", "-lkernel32", "-lmsvcrt",
           "-Wl,--entry=mainCRTStartup@0", "-Wl,--subsystem,console:4.0",
           "-Wl,--major-os-version,4", "-Wl,--minor-os-version,0", "-Wl,--image-base,0x400000",
           "-Wl,--disable-dynamicbase", "-Wl,--disable-nxcompat", "-Wl,--file-alignment,4096",
           "-Wl,--gc-sections", "-Wl,--no-insert-timestamp", "-Wl,--disable-auto-import"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(f"link kexnet_test failed:\n{p.stderr}")
        return 1
    print(f"built {exe} ({exe.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
