"""Shared build steps for KernelExEx WDM drivers (Windows 98 SE / ME).

compile(): clang against mingw-w64's DDK headers, Windows 2000 baseline.
link():    native-subsystem PE; import libraries are generated from the
           symbols the objects actually import, then every import is checked
           against the 98 SE tables in net/tools/exports/win98se-wdm; header
           fields and checksum follow Microsoft's 98 SE drivers.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPORTS = ROOT / "net" / "tools" / "exports" / "win98se-wdm"
MINGW = ROOT.parent / "thirdparty" / "mingw-w64" / "mingw-w64-headers"

# import module -> export table file; the module a symbol comes from is
# looked up in these tables, in this order (HAL first: Microsoft's 98 SE
# drivers import KfAcquireSpinLock and friends from HAL.DLL)
MODULES = ["hal.dll", "ntoskrnl.exe", "usbd.sys", "hidclass.sys", "hidparse.sys", "ndis.sys", "ks.sys",
           "portcls.sys", "stream.sys", "wmilib.sys"]


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode or "warning:" in r.stderr:
        sys.exit(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def cflags(sdk: Path, extra=()):
    res = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
    return ["clang", "--target=i686-w64-windows-gnu", "-march=i586", "-O2", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-fno-auto-import", "-mgeneral-regs-only", "-fno-jump-tables",
            "-mno-stack-arg-probe", "-nostdinc",
            "-isystem", f"{res}/include", "-isystem", str(sdk / "include"),
            "-isystem", str(MINGW / "crt"), "-isystem", str(MINGW / "include"),
            "-I", str(ROOT / "toolchain" / "wdm" / "include"),
            "-I", str(MINGW / "ddk" / "include" / "ddk"), "-I", str(MINGW / "ddk" / "include"),
            "-D_WIN32_WINNT=0x0500", "-DNTDDI_VERSION=0x05000000",
            "-include", "sdkddkver.h", "-DNTDDI_WINVISTA=0x06000000",
            "-Wall", "-Wno-pragma-pack", *extra]


def compile(sources, out: Path, sdk: Path, extra=()):
    objs = []
    for c in sources:
        o = out / (Path(c).stem + ".o")
        run(cflags(sdk, extra) + ["-c", str(c), "-o", str(o)])
        objs.append(o)
    return objs


def tables(extra=None):
    """Export tables: Windows' modules, then our own export drivers
    (extra maps a module name such as kexbsd.sys to its export names)."""
    t = {}
    for m in MODULES:
        f = EXPORTS / f"{m}.txt"
        if f.exists():
            t[m] = set(f.read_text().split())
    t.update(extra or {})
    return t


def exports_of(sysf: Path):
    """Names exported by a driver we built (for drivers that import from it)."""
    txt = run(["llvm-objdump", "-p", sysf]).split("Export Table")[-1]
    return {l.split()[-1] for l in txt.splitlines() if l.strip()[:1].isdigit() and len(l.split()) == 3}


def make_import_libs(objs, out: Path, local_exports=(), extra_tables=None):
    """One import library per module, from the __imp_ symbols the objects use.
    Names keep their exact export spelling (see implib_exact.py)."""
    nm = run(["llvm-nm", "-u", *objs])
    wanted = sorted(set(re.findall(r"__imp_(\S+)", nm)))
    t = tables(extra_tables)
    per = {}
    missing = []
    for sym in wanted:
        if sym.startswith("@"):            # fastcall: @Name@N
            name, deco = sym.split("@")[1], sym
        else:                              # stdcall/cdecl: _Name@N or _Name
            deco = sym[1:]
            name = deco.split("@")[0]
        for m, names in t.items():
            if name in names or ("_" + deco) in names:
                per.setdefault(m, []).append((deco, ("_" + deco) in names))
                break
        else:
            if name not in local_exports:
                missing.append(name)
    if missing:
        sys.exit("functions not available to WDM drivers on Windows 98 SE: " + ", ".join(sorted(missing)))
    libs = []
    for m, syms in per.items():
        stem = m.split(".")[0]
        plain = [d for d, exact in syms if not exact]
        exact = [d for d, ex in syms if ex]
        if plain:
            d = out / f"{stem}.def"
            d.write_text(f"LIBRARY {m}\nEXPORTS\n" + "\n".join(plain) + "\n")
            lib = out / f"lib{stem}.a"
            run(["llvm-dlltool", "-m", "i386", "-k", "-d", d, "-l", lib])
            libs.append(lib)
        if exact:
            d = out / f"{stem}_exact.def"
            d.write_text(f"LIBRARY {m}\nEXPORTS\n" + "\n".join(exact) + "\n")
            lib = out / f"lib{stem}_exact.a"
            run([sys.executable, Path(__file__).parent / "implib_exact.py", d, lib])
            libs.append(lib)
    return libs


def fix_header(path: Path, epoch: int):
    """Match Microsoft's 98 SE drivers: OS 4.0, subsystem 1.0, WDM_DRIVER,
    a fixed non-zero time stamp (see build_core.py set_timestamp) and a
    valid PE checksum."""
    d = bytearray(path.read_bytes())
    pe = int.from_bytes(d[0x3C:0x40], "little")
    d[pe + 8:pe + 12] = epoch.to_bytes(4, "little")
    opt = pe + 24
    for off, val in ((opt + 40, 4), (opt + 42, 0), (opt + 48, 1), (opt + 50, 0)):
        d[off:off + 2] = val.to_bytes(2, "little")
    d[opt + 70:opt + 72] = (0x2000).to_bytes(2, "little")
    d[opt + 64:opt + 68] = b"\0\0\0\0"
    s = 0
    for i in range(0, len(d) - 1, 2):
        s += d[i] | (d[i + 1] << 8)
        s = (s & 0xFFFF) + (s >> 16)
    if len(d) & 1:
        s += d[-1]
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    d[opt + 64:opt + 68] = (s + len(d)).to_bytes(4, "little")
    path.write_bytes(d)


def link(name, objs, out: Path, entry="_DriverEntry@8", deffile=None, base="0x10000", local_exports=(),
         extra_tables=None):
    libs = make_import_libs(objs, out, local_exports, extra_tables)
    sysf = out / f"{name}.sys"
    cmd = ["clang", "--target=i686-w64-windows-gnu", "-nostdlib", "-fuse-ld=lld", "-Wl,--subsystem,native",
           f"-Wl,--entry,{entry}", f"-Wl,--image-base,{base}", "-Wl,--dynamicbase", "-Wl,--disable-auto-import",
           *objs, *libs, "-o", sysf]
    if deffile:
        cmd += ["-shared", deffile, f"-Wl,--out-implib,{out / ('lib' + name + '.a')}"]
    run(cmd)
    fix_header(sysf, int((ROOT / "net/certs/EPOCH").read_text().strip()))
    dis = run(["llvm-objdump", "-d", "--no-show-raw-insn", sysf])
    fpu = [l for l in dis.splitlines() if re.search(r"^\s+[0-9a-f]+:\s+(f[a-z]+\s|.*%x?mm[0-9])", l)]
    if fpu:
        sys.exit(f"{name}: FPU/SIMD instructions in a driver:\n" + "\n".join(fpu[:10]))
    return sysf
