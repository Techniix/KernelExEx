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
            (self.out / f"{name}.errors.txt").write_text("\n".join(errors))
            sys.exit(f"{name}: {len(errors)} of {len(jobs)} file(s) failed, see {self.out / (name + '.errors.txt')}")
        (self.out / f"{name}.errors.txt").unlink(missing_ok=True)
        objs = [o for _, o in jobs]
        self.check_objects(name, objs)
        return objs

    def check_objects(self, name, objs):
        syms = run(["llvm-nm", *objs])
        bad = sorted(set(re.findall(r"\.refptr\.\S+", syms)))
        if "kex_seh_unsupported_filter" in syms:
            sys.exit(f"{name}: __except with a filter other than EXCEPTION_EXECUTE_HANDLER is not supported")
        if bad:
            sys.exit(f"{name}: auto-import stubs present (would clobber registers in thunks): {bad[:5]}")
        # Inline assembly naming a dllimport function ("call dword ptr [HeapAlloc]") is
        # miscompiled by clang into: mov reg,[__imp__X] / call dword ptr [reg], which
        # dereferences the function's code. See common/asmimp.h for the fix.
        bad = []
        for i in range(0, len(objs), 40):
            dis = run(["llvm-objdump", "-d", "-r", "--no-show-raw-insn", "--x86-asm-syntax=intel", *objs[i:i + 40]])
            lines = [l for l in dis.splitlines() if l.strip()]
            for k, line in enumerate(lines):
                m = re.search(r"IMAGE_REL_I386_DIR32\s+(__imp__\S+)", line)
                if not m or k == 0 or k + 1 >= len(lines):
                    continue
                load = re.search(r"\bmov\s+(e\w\w), dword ptr \[", lines[k - 1])
                if load and re.search(r"ptr \[%s\]" % load.group(1), lines[k + 1]):
                    bad.append(f"{m.group(1)}: {lines[k + 1].strip()}")
        if bad:
            sys.exit(f"{name}: inline assembly dereferences an imported function (use IMP() from asmimp.h):\n  "
                     + "\n  ".join(bad[:10]))

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

    def clean_def(self, d: Path):
        """lld does not accept BASE= on the LIBRARY line or a SECTIONS block.
        Returns (path of a cleaned copy, image base or None)."""
        text = d.read_text(errors="replace").replace("\r\n", "\n")
        m = re.search(r"BASE\s*=\s*(0x[0-9A-Fa-f]+)", text)
        text = re.sub(r"[ \t]+BASE\s*=\s*0x[0-9A-Fa-f]+", "", text)
        text = re.sub(r"^SECTIONS\n(?:[ \t]+.*\n)+", "", text, flags=re.M)
        out = self.obj / "defs" / d.name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        return out, (m.group(1) if m else None)

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

COMPAT = ["-include", str(ROOT / "toolchain/rt/msvc_compat.h")]


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
    # getaddrinfo fallbacks that the Platform SDK supplies inline in wspiapi.h
    mingw = Path((b.sdk / "MINGW_INCLUDE").read_text().strip()).parents[1]
    wsp = sorted((mingw / "mingw-w64-crt/libsrc/wspiapi").glob("*.c"))
    b.archive("wspiapi", b.compile("wspiapi", wsp, ["-D_WIN32_WINNT=0x0500"]))
    # GUID definitions (uuid.lib of the Platform SDK)
    am = (mingw / "mingw-w64-crt/Makefile.am").read_text().replace("\\\n", " ")
    uuid = [mingw / "mingw-w64-crt" / f for f in re.search(r"^src_libuuid=(.*)$", am, re.M).group(1).split()]
    b.archive("uuid", b.compile("uuid", uuid, ["-D_WIN32_WINNT=0x0601"]))


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
    # shell32 functions that Windows 9x exports by ordinal only (the names
    # appeared with Windows 2000/XP); ordinals are the documented ones.
    b.implib_from_def("shl32ord", "LIBRARY SHELL32.dll\nEXPORTS\n"
                      "SHGetSpecialFolderPath@16 @175 NONAME\n"
                      "ILFindLastID@4 @16 NONAME\nILRemoveLastID@4 @17 NONAME\nILClone@4 @18 NONAME\n"
                      "SHILCreateFromPath@12 @28 NONAME\nILFree@4 @155 NONAME\nSHCreateDirectory@8 @165 NONAME\n")


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
                                                "-D_WIN32_WINNT=0x0410", *COMPAT])
    res = b.resource("core", d / "Core.rc")
    b.link("KernelEx", [*objs, res], ["kexcrt", "k32ord", "kernel32", "user32", "gdi32", "advapi32", "comctl32"],
           base="0xBFA00000", entry="PreDllMain@12", implib="KernelEx", shared_sections=True,
           defs=ROOT / "toolchain/defs/KernelEx.def")


def prepare_apilibs(b: Build):
    """Copy apilibs to <out>/gen/apilibs (lower-case directories, LF line
    ends), then run util/prep to regenerate the API lists there."""
    host = b.out / "host"
    host.mkdir(exist_ok=True)
    prep = host / "prep"
    run(["g++", "-std=gnu++98", "-w", "-O1", f"-I{ROOT / 'toolchain/hostshim'}", ROOT / "util/prep/prep.cpp",
         "-o", prep])
    dst = b.gen / "apilibs"
    shutil.rmtree(dst, ignore_errors=True)
    for lib in ("kexbases", "kexbasen"):
        src = ROOT / "apilibs" / lib
        for root, _, files in os.walk(src):
            rel = Path(root).relative_to(ROOT / "apilibs")
            out = dst / str(rel).lower()
            out.mkdir(parents=True, exist_ok=True)
            for f in files:
                data = (Path(root) / f).read_bytes()
                if f == "dirlist" or f.lower().endswith((".c", ".cpp", ".h", ".hpp", ".def", ".rc", ".inc")):
                    data = data.replace(b"\r\n", b"\n")
                (out / f).write_bytes(data)
        run([prep, "."], cwd=dst / lib)
    for link in ("common", "kexcrt", "core"):
        l = b.gen / link
        if not l.exists():
            l.symlink_to(ROOT / link)


def build_apilib(b: Build, name, base, shared, libs, delay=()):
    d = b.gen / "apilibs" / name
    dirs = [x.strip() for x in (d / "dirlist").read_text().split() if x.strip()]
    srcs = sorted(d.glob("*.c")) + sorted(d.glob("*.cpp"))
    for sub in dirs:
        for root, _, files in os.walk(d / sub):
            srcs += [Path(root) / f for f in sorted(files) if f.lower().endswith((".c", ".cpp"))]
    srcs += [ROOT / "common/common.c"] + ([ROOT / "common/pemanip.cpp"] if False else [])
    in_dsp = {p.name.lower() for p in dsp_sources(ROOT / "apilibs" / name / f"{name}.dsp")}
    srcs = [s for s in srcs if s.name.lower() in in_dsp]
    flags = [f"-I{d}", f"-I{ROOT / 'common'}", "-D_USRDLL", "-D_WIN32_WINNT=0x0500", "-D_SSIZE_T_DEFINED", *COMPAT]
    objs = b.compile(name, ctor_order(srcs), flags, cxx_for_c=True)  # /TP: everything is C++
    res = b.resource(name, d / f"{name}.rc")
    extra = [f"-Wl,--delayload={x}" for x in delay]
    b.link(name, [*objs, res], libs + ["KernelEx", "kexcrt", "k32ord"], base=base, entry="__DllMainCRTStartup@12",
           shared_sections=shared, extra=extra)


def build_kexbases(b: Build):
    build_apilib(b, "kexbases", "0xBFA40000", True,
                 ["kernel32", "user32", "gdi32", "winspool", "comdlg32", "advapi32", "shell32", "version"])


def build_kexbasen(b: Build):
    build_apilib(b, "kexbasen", "0x7D000000", False,
                 # usp10 before gdi32: mingw-w64's gdi32 also lists the Script* functions
                 # (Windows 10 forwards them), which 9x gdi32 does not have
                 ["usp10", "kernel32", "user32", "gdi32", "winspool", "cryptui", "comdlg32", "advapi32", "shlwapi",
                  "shl32ord", "shell32", "rpcrt4", "winmm", "ws2_32", "wspiapi"],
                 delay=["shell32.dll", "rpcrt4.dll", "usp10.dll", "comdlg32.dll", "winspool.drv", "shlwapi.dll",
                        "winmm.dll", "ws2_32.dll", "cryptui.dll"])


def build_sheet(b: Build):
    d = ROOT / "sheet"
    objs = b.compile("sheet", ctor_order(dsp_sources(d / "sheet.dsp")),
                     [f"-I{d}", f"-I{ROOT / 'common'}", "-D_USRDLL", "-DSHEET_EXPORTS", "-D_WIN32_WINNT=0x0410", *COMPAT,
                      "-include", "stdio.h"])  # the VC6 headers pulled in stdio.h indirectly
    res = b.resource("sheet", d / "sheet.rc")
    b.link("sheet", [*objs, res], ["kexcrt", "kernel32", "user32", "gdi32", "comctl32", "advapi32", "shell32",
                                   "shlwapi", "ole32", "uuid"],
           base="0x7D200000", entry="__DllMainCRTStartup@12", defs=d / "server.def", kill_at=True)


def build_kexcom(b: Build):
    d = ROOT / "kexCOM"
    objs = b.compile("kexCOM", ctor_order(dsp_sources(d / "kexCOM.dsp")),
                     [f"-I{d}", f"-I{ROOT / 'common'}", "-D_USRDLL", "-DKEXCOM_EXPORTS", "-D_WIN32_WINNT=0x0410", *COMPAT,
                      "-include", "shlobj.h"])  # CLSID_ShellLink lives in shobjidl.h in mingw-w64
    # the MSVC project uses /entry:DllMain@12 (no static constructors)
    b.link("kexCOM", objs, ["kexcrt", "kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32",
                            "uuid"],
           base="0x7D300000", entry="DllMain@12", defs=d / "server.def", kill_at=True)


AUX = {
    # name: (libs, kill_at, resource)
    "msimg32": (["kernel32", "gdi32"], True, "msimgme.rc"),
    "pdh": (["k32ord", "kernel32"], False, None),
    "psapi": (["kernel32", "KernelEx"], False, None),
    "uxtheme": (["k32ord", "kernel32", "user32", "gdi32"], False, None),
    "wtsapi32": (["kernel32", "gdi32"], True, None),
    "userenv": (["kernel32", "gdi32", "shell32", "shlwapi"], False, None),
}


def build_auxiliary(b: Build):
    (b.out / "auxiliary").mkdir(exist_ok=True)
    base = 0x7D400000
    for name, (libs, kill_at, rc) in AUX.items():
        d = ROOT / "auxiliary" / name
        objs = b.compile("aux_" + name, sorted(d.glob("*.c")), [f"-I{d}", f"-I{ROOT / 'common'}",
                                                                "-D_WIN32_WINNT=0x0500", *COMPAT])
        if rc:
            objs.append(b.resource("aux_" + name, d / rc))
        has_main = "DllMain" in "".join(f.read_text(errors="replace") for f in d.glob("*.c"))
        defs, def_base = b.clean_def(d / f"{name}.def")
        out = b.link(name, objs, libs + ["kexcrt"], base=def_base or hex(base),
                     entry="DllMain@12" if has_main else None, defs=defs, kill_at=kill_at)
        shutil.move(out, b.out / "auxiliary" / out.name)
        base += 0x100000


def build_verify(b: Build):
    d = ROOT / "verify"
    objs = b.compile("verify", [d / "main.c"], [f"-I{d}", f"-I{ROOT / 'common'}", "-D_WIN32_WINNT=0x0410", *COMPAT])
    res = b.resource("verify", d / "verify.rc")
    b.link("verify", [*objs, res], ["kexcrt", "kernel32", "user32", "advapi32", "comctl32", "shell32"],
           base="0x400000", entry="entrypoint", kind="exe")


STEPS = {"kexcrt": build_kexcrt, "k32ord": build_k32ord, "core": build_core, "gen": prepare_apilibs,
         "kexbases": build_kexbases, "kexbasen": build_kexbasen, "sheet": build_sheet, "kexCOM": build_kexcom,
         "auxiliary": build_auxiliary, "verify": build_verify}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", action="append")
    a = ap.parse_args()
    b = Build(a.sdk.resolve(), a.out.resolve(), a.j)
    mingw_inc = Path((b.sdk / "MINGW_INCLUDE").read_text().strip())
    TREES = [ROOT / x for x in ("core", "common", "kexcrt", "sheet", "kexCOM", "auxiliary", "kexcontrol", "verify")]
    TREES += [mingw_inc, mingw_inc.parent / "crt"]  # sources say <RpcProxy.h>, mingw-w64 has rpcproxy.h
    b.case_overlay(TREES + ([b.gen / "apilibs"] if (b.gen / "apilibs").exists() else []))
    for name, fn in STEPS.items():
        if a.only and name not in a.only:
            continue
        print(f"== {name}", flush=True)
        fn(b)
        if name == "gen":
            b.case_overlay(TREES + [b.gen / "apilibs"])
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
