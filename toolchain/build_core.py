#!/usr/bin/env python3
"""Cross-build the KernelEx core with clang + lld (no Visual C++ 6 needed).

  kexcrt        static C runtime used by every KernelEx module
  KernelEx.dll  the core (shared DLL at 0xBFA00000, all sections shared)
  kexbases.dll  shared API library     kexbasen.dll  non-shared API library
  sheet.dll     Properties-page shell extension
  kexCOM.dll, auxiliary DLLs (msimg32, pdh, psapi, userenv, uxtheme, wtsapi32)

The original projects (.dsp) stay usable; this script reads the same
sources. Differences from the MSVC build are handled here, not in the
sources, wherever possible:
  * API library sources are copied to <out>/gen with lower-case directory
    names and LF line ends, then util/prep (built for the host) fills in the
    generated API lists, exactly as the MSVC custom build step does.
  * A case-insensitive include overlay stands in for the Windows file system.
  * -fno-auto-import keeps clang from routing data references through
    .refptr stubs, which would clobber a register inside the hand-written
    kernel32 thunks. The build fails if any object contains such a stub.
  * PE section flags (SHARED) are set after linking.

Usage: build_core.py --sdk <dir from net/tools/prepare_sdk.py> --out <dir> [-j N] [--only NAME]
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ["--target=i686-w64-windows-gnu", "-march=i586", "-mtune=pentium2"]
IMAGE_SCN_MEM_SHARED = 0x10000000


def run(cmd, **kw):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if p.returncode != 0:
        sys.exit("FAILED: " + " ".join(str(c) for c in cmd) + "\n" + p.stdout + p.stderr)
    return p.stdout


class Build:
    def __init__(self, sdk: Path, out: Path, jobs: int):
        self.sdk, self.out, self.jobs = sdk, out, jobs
        self.obj = out / "obj"
        self.lib = out / "lib"
        self.gen = out / "gen"
        for d in (self.obj, self.lib, self.gen):
            d.mkdir(parents=True, exist_ok=True)
        mingw_inc = (sdk / "MINGW_INCLUDE").read_text().strip()
        res = run(["clang", "-print-resource-dir"]).strip()
        self.sysinc = ["-nostdinc", "-nostdinc++",
                       "-isystem", ROOT / "toolchain/cxx",
                       "-isystem", res + "/include",
                       "-isystem", sdk / "include",
                       "-isystem", Path(mingw_inc).parent / "crt",
                       "-isystem", mingw_inc,
                       "-isystem", Path(mingw_inc).parent / "ddk/include/ddk"]
        self.base = TARGET + [
            "-O2", "-g0", "-fms-extensions", "-fno-auto-import",
            "-fno-exceptions", "-fno-rtti", "-fno-strict-aliasing", "-fwrapv",
            "-fno-asynchronous-unwind-tables", "-fno-stack-protector", "-fno-ident",
            "-fno-builtin", "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
            "-mno-stack-arg-probe",
            "-DWIN32", "-D_WINDOWS", "-DNDEBUG", "-D_MBCS",
            "-DWINVER=0x0500", "-D__MSVCRT_VERSION__=0x600", "-D__USE_MINGW_ANSI_STDIO=0",
            "-D__CRT__NO_INLINE", "-D_CRTIMP=", "-D__NO_CTYPE_INLINES", "-D__NO_ISOCEXT",
            "-w",
        ]
        self.overlay = None

    # ---- compiling -------------------------------------------------
    def compile(self, name, sources, flags, cxx_for_c=False):
        """Compile sources into <out>/obj/<name>/; returns object paths."""
        odir = self.obj / name
        jobs = []
        for src in sources:
            src = Path(src)
            rel = re.sub(r"[^A-Za-z0-9_.]", "_", str(src.relative_to(src.anchor)))[-120:]
            obj = odir / (rel + ".o")
            jobs.append((src, obj))
        odir.mkdir(parents=True, exist_ok=True)

        def one(job):
            src, obj = job
            is_cxx = src.suffix.lower() == ".cpp" or cxx_for_c
            lang = ["-x", "c++", "-std=gnu++03"] if is_cxx else ["-x", "c", "-std=gnu89"]
            if src.suffix == ".S":
                lang = []
            cmd = ["clang", *self.base, *lang, *self.sysinc, *flags, "-c", src, "-o", obj]
            if self.overlay:
                cmd[1:1] = ["-ivfsoverlay", self.overlay]
            p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
            return None if p.returncode == 0 else f"{src}:\n{p.stderr}"

        with concurrent.futures.ThreadPoolExecutor(self.jobs) as ex:
            errors = [e for e in ex.map(one, jobs) if e]
        if errors:
            sys.exit(f"{name}: {len(errors)} file(s) failed\n" + "\n".join(errors[:8]))
        objs = [o for _, o in jobs]
        self.check_objects(name, objs)
        return objs

    def check_objects(self, name, objs):
        syms = run(["llvm-nm", *objs])
        bad = sorted(set(re.findall(r"\.refptr\.\S+", syms)))
        if bad:
            sys.exit(f"{name}: auto-import stubs present (would clobber registers in thunks): {bad[:5]}")

    def archive(self, name, objs):
        lib = self.lib / f"lib{name}.a"
        lib.unlink(missing_ok=True)
        run(["llvm-ar", "rcs", lib, *objs])
        return lib

    # ---- linking ---------------------------------------------------
    def link(self, name, objs, libs, base, entry, kind="dll", ext=None, defs=None, extra=(), implib=None,
             kill_at=False, shared_sections=False, subsystem="windows"):
        outfile = self.out / (name + (ext or (".dll" if kind == "dll" else ".exe")))
        cmd = ["clang", *TARGET, "-fuse-ld=lld", "-nostdlib", "-o", outfile, *objs]
        if kind == "dll":
            cmd.append("-shared")
        if defs:
            cmd.append(defs)
        cmd += [f"-L{self.lib}", f"-L{self.sdk / 'lib'}"]
        cmd += ["-Wl,--start-group", *[f"-l{l}" for l in libs], "-Wl,--end-group"]
        cmd += [f"-Wl,--entry={entry}" if entry else "-Wl,--no-entry",
                f"-Wl,--subsystem,{subsystem}:4.0",
                "-Wl,--major-os-version,4", "-Wl,--minor-os-version,0",
                "-Wl,--major-image-version,4", "-Wl,--minor-image-version,0",
                f"-Wl,--image-base,{base}",
                "-Wl,--disable-dynamicbase", "-Wl,--disable-nxcompat", "-Wl,--disable-high-entropy-va",
                "-Wl,--file-alignment,4096", "-Wl,--no-insert-timestamp",
                "-Wl,--disable-auto-import", "-Wl,--disable-runtime-pseudo-reloc",
                "-Wl,--enable-stdcall-fixup", "-Wl,--build-id=none",
                f"-Wl,-Map,{self.out / (name + '.map')}"]
        if kill_at:
            cmd.append("-Wl,--kill-at")
        if implib:
            cmd.append(f"-Wl,--out-implib,{self.lib / ('lib' + implib + '.a')}")
        cmd += list(extra)
        run(cmd)
        if shared_sections:
            self.mark_shared(outfile)
        return outfile

    @staticmethod
    def mark_shared(pe: Path):
        """Set IMAGE_SCN_MEM_SHARED on every section except .rsrc and .reloc,
        as core.def / the apilib .def files request (SECTIONS ... SHARED)."""
        data = bytearray(pe.read_bytes())
        off = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, off + 6)[0]
        opt = struct.unpack_from("<H", data, off + 20)[0]
        sec = off + 24 + opt
        for i in range(nsec):
            s = sec + 40 * i
            name = bytes(data[s:s + 8]).rstrip(b"\0").decode()
            if name in (".rsrc", ".reloc"):
                continue
            ch = struct.unpack_from("<I", data, s + 36)[0]
            struct.pack_into("<I", data, s + 36, ch | IMAGE_SCN_MEM_SHARED)
        pe.write_bytes(data)

    def resource(self, name, rc: Path, incs=()):
        """Compile a .rc file to a COFF object."""
        res = self.obj / name / (rc.stem + ".res.o")
        res.parent.mkdir(parents=True, exist_ok=True)
        mingw_inc = (self.sdk / "MINGW_INCLUDE").read_text().strip()
        windres = shutil.which("llvm-windres") or shutil.which("llvm-windres-18") or "/usr/lib/llvm-18/bin/llvm-windres"
        cmd = [windres, "--target=pe-i386", "-I", rc.parent, "-I", ROOT / "common",
               "-I", mingw_inc, "-I", Path(mingw_inc).parent / "crt", "-I", self.sdk / "include"]
        for i in incs:
            cmd += ["-I", i]
        cmd += ["-DNDEBUG", rc, "-o", res]
        run(cmd)
        return res

    # ---- helpers ---------------------------------------------------
    def implib_from_def(self, name, text):
        d = self.lib / f"{name}.def"
        d.write_text(text)
        run(["llvm-dlltool", "-m", "i386", "-k", "-d", d, "-l", self.lib / f"lib{name}.a"])

    def case_overlay(self, roots):
        """clang VFS overlay that makes the given trees case-insensitive."""
        def node(p: Path):
            if p.is_dir():
                return {"type": "directory", "name": p.name,
                        "contents": [node(c) for c in sorted(p.iterdir()) if c.name != ".git"]}
            return {"type": "file", "name": p.name, "external-contents": str(p)}
        ov = {"version": 0, "case-sensitive": "false", "roots": []}
        for r in roots:
            n = node(r)
            n["name"] = str(r)
            ov["roots"].append(n)
        path = self.out / "case-overlay.yaml"
        path.write_text(json.dumps(ov))
        self.overlay = path


# ---------------------------------------------------------------------------

def build_kexcrt(b: Build):
    d = ROOT / "kexcrt"
    # same file list as kexcrt/makefile (atox.c and strtox.c are templates)
    names = re.search(r"OBJ = (.*?)\n\n", (d / "makefile").read_text().replace("\\\n", " "), re.S).group(1).split()
    srcs = []
    for n in names:
        stem = d / n[:-2]
        srcs.append(stem.with_suffix(".cpp") if stem.with_suffix(".cpp").exists() else stem.with_suffix(".c"))
    renamed = [s for s in srcs if s.name in ("snprintf.c", "vsnprintf.c")]
    plain = [s for s in srcs if s not in renamed]
    flags = [f"-I{d}", "-D_CTYPE_DISABLE_MACROS"]
    objs = b.compile("kexcrt", plain + sorted((ROOT / "toolchain/rt").glob("*.[cS]")), flags)
    objs += b.compile("kexcrt", renamed, flags + ["-include", str(ROOT / "toolchain/rt/kexcrt_pre.h")])
    b.archive("kexcrt", objs)


def build_k32ord(b: Build):
    """Import library for the unnamed kernel32 exports KernelEx relies on."""
    hdr = ROOT / "common/k32ord.h"
    obj = b.obj / "k32ord.o"
    run(["clang", *b.base, "-x", "c", *b.sysinc, "-DK32ORD_IMPLIB", "-c", hdr, "-o", obj])
    deco = {}
    for line in run(["llvm-nm", "--defined-only", "-g", obj]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2].startswith("_") and "@" in parts[2]:
            deco[parts[2][1:].split("@")[0]] = parts[2][1:]
    lines = ["LIBRARY KERNEL32.dll", "EXPORTS"]
    for raw in (ROOT / "common/k32ord.def").read_text().splitlines():
        m = re.match(r"\s+(\w+)(?:\s+@(\d+)\s+NONAME)?\s*$", raw)
        if not m:
            continue
        name, ordinal = m.group(1), m.group(2)
        if name not in deco:
            sys.exit(f"k32ord: {name} is in k32ord.def but not declared in k32ord.h")
        lines.append(f"{deco[name]} @{ordinal} NONAME" if ordinal else deco[name])
    b.implib_from_def("k32ord", "\n".join(lines) + "\n")


def dsp_sources(dsp: Path, exts=(".c", ".cpp")):
    """Source files of a .dsp, in project order."""
    out = []
    for m in re.finditer(r"^SOURCE=(.+?)\s*$", dsp.read_text(errors="replace"), re.M):
        rel = m.group(1).strip('"').replace("\\", "/")
        if rel.lower().endswith(exts):
            out.append((dsp.parent / rel).resolve())
    return out


def ctor_order(srcs):
    """MSVC runs static constructors in link order; kexcrt's gcc start-up
    walks .ctors backwards. Reverse the objects so the order is unchanged."""
    return list(reversed(srcs))


def build_core(b: Build):
    d = ROOT / "core"
    release_skip = {"debug.cpp", "debugproto.cpp", "apihook.cpp"}
    srcs = [f for f in dsp_sources(d / "core.dsp") if f.name not in release_skip]
    objs = b.compile("core", ctor_order(srcs), [f"-I{d}", f"-I{ROOT / 'common'}", "-DKEXCORE_EXPORTS", "-D_USRDLL",
                                                "-D_WIN32_WINNT=0x0410"])
    res = b.resource("core", d / "Core.rc")
    b.link("KernelEx", [*objs, res], ["kexcrt", "k32ord", "kernel32", "user32", "gdi32", "advapi32", "comctl32"],
           base="0xBFA00000", entry="PreDllMain@12", implib="KernelEx", shared_sections=True,
           defs=ROOT / "toolchain/defs/KernelEx.def")


STEPS = {"kexcrt": build_kexcrt, "k32ord": build_k32ord, "core": build_core}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", action="append")
    a = ap.parse_args()
    b = Build(a.sdk.resolve(), a.out.resolve(), a.j)
    b.case_overlay([ROOT / x for x in ("core", "common", "kexcrt", "apilibs", "sheet", "kexCOM", "auxiliary",
                                       "kexcontrol", "verify")])
    for name, fn in STEPS.items():
        if a.only and name not in a.only:
            continue
        print(f"== {name}")
        fn(b)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
