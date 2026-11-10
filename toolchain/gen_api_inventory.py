#!/usr/bin/env python3
"""Generate the list of APIs KernelExEx adds to Windows 98 SE / ME.

Reads the sources, not built binaries:
  * API library tables: apilibs/*/*/_*_apilist.c (DECL_TAB/DECL_API),
    net/kexrt and net/kextls apilib tables. Order follows the default
    configuration in apilibs/CORE.INI ([DCFG1] contents=...): a later
    library overrides an earlier one, as in the resolver.
  * Whole DLLs: net/winhttp/winhttp.def, net/bcrypt/bcrypt.c,
    net/kextls/include/kextls.h, auxiliary/*/*.def.
  * Windows 98 SE export tables (net/tools/exports/win98se) to tell new
    functions from replaced ones.

Writes docs/api.json (for tools and AI agents) and docs/API.md.
With --check, fails if either file is out of date; the build runs this.

Usage: gen_api_inventory.py [--check]
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXPORTS_98SE = ROOT / "net/tools/exports/win98se"
OUT_JSON = ROOT / "docs/api.json"
OUT_MD = ROOT / "docs/API.md"

KIND_TEXT = {
    "new": "added (not in Windows 98 SE)",
    "replaced": "replaced or extended (Windows 98 SE has a weaker version)",
    "stub": "stub: exists so programs load, fails with ERROR_CALL_NOT_IMPLEMENTED or similar",
    "mslu": "forwarded to the Microsoft Layer for Unicode (unicows.dll), or to KernelExEx's kexuni.dll when MSLU is not installed",
}

# whole DLLs that KernelExEx installs (not routed through API libraries)
DLL_NOTES = {
    "WINHTTP.DLL": "net/winhttp: synchronous WinHTTP with TLS 1.2/1.3 (kextls)",
    "BCRYPT.DLL": "net/bcrypt: CNG subset: RNG, MD5, SHA-1, SHA-2, HMAC",
    "KEXTLS.DLL": "net/kextls: direct TLS client, digest and random API (include/kextls.h)",
    "MSIMG32.DLL": "auxiliary/msimg32",
    "PDH.DLL": "auxiliary/pdh (stubs)",
    "PSAPI.DLL": "auxiliary/psapi",
    "UXTHEME.DLL": "auxiliary/uxtheme (no themes; functions report themes off)",
    "WTSAPI32.DLL": "auxiliary/wtsapi32",
    "USERENV.DLL": "auxiliary/userenv",
}


def text(p: Path):
    return p.read_text(encoding="latin-1").replace("\r\n", "\n")


def strip_comments(s: str):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def exports_98se():
    out = {}
    for f in EXPORTS_98SE.glob("*.txt"):
        dll = f.name[:-4].upper()
        out[dll] = set(text(f).split())
    return out


def default_contents():
    ini = text(ROOT / "apilibs/CORE.INI")
    m = re.search(r"^\[DCFG1\]\s*\ncontents=([^\n]+)", ini, re.M)
    return [x.strip() for x in m.group(1).split(",")]


def kexbase_tables(lib):
    """{dll: {name: impl}} from the generated API lists of kexbases/kexbasen."""
    tables = {}
    for f in sorted((ROOT / "apilibs" / lib).glob("*/_*_apilist.c")):
        s = strip_comments(text(f))
        tab = re.search(r'DECL_TAB\("([^"]+)",\s*(\w+)', s)
        if not tab:
            continue
        dll, arr = tab.group(1).upper(), tab.group(2)
        body = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % re.escape(arr), s, re.S)
        if not body:
            continue
        for name, impl in re.findall(r'DECL_API\("([^"]+)",\s*(\w+)\)', body.group(1)):
            tables.setdefault(dll, {}).setdefault(name, impl)  # first entry is the default one
    return tables


def c_named_tables(apilib_c: Path, arrays_dir: Path):
    """Tables of the net/ API libraries: { "Name", (unsigned long)impl } arrays."""
    s = strip_comments(text(apilib_c))
    tables = {}
    # kexrt style: tables[i].target_library = "X"; tables[i].named_apis = arr;
    for idx, dll in re.findall(r'tables\[(\d+)\]\.target_library\s*=\s*"([^"]+)"', s):
        m = re.search(r"tables\[%s\]\.named_apis\s*=\s*(\w+)" % idx, s)
        if m:
            tables[dll.upper()] = m.group(1)
    # kextls style: { "X.DLL", arr, ... }
    for dll, arr in re.findall(r'\{\s*"([A-Z0-9_]+\.DLL)",\s*(\w+)', s):
        tables[dll.upper()] = arr
    out = {}
    src = "\n".join(strip_comments(text(p)) for p in sorted(arrays_dir.glob("*.c")))
    for dll, arr in tables.items():
        body = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % re.escape(arr), src, re.S)
        if body:
            for name, impl in re.findall(r'\{\s*"([^"]+)",\s*\(unsigned long\)\s*(\w+)\s*\}', body.group(1)):
                out.setdefault(dll, {})[name] = impl
    return out


def classify(dll, name, impl, native):
    if impl.endswith("_stub"):
        return "stub"
    if impl.endswith("_fwd"):
        return "mslu"
    return "replaced" if name in native.get(dll, set()) else "new"


def def_exports(p: Path):
    s = text(p)
    s = s.split("EXPORTS", 1)[1] if "EXPORTS" in s else ""
    names = []
    for line in s.splitlines():
        line = line.split(";", 1)[0].strip()
        if not line or line.upper().startswith(("SECTIONS", "LIBRARY")):
            continue
        name = re.split(r"[\s=@]", line)[0]
        if name:
            names.append(name)
    return names


def build():
    native = exports_98se()
    apis = {}  # dll -> name -> entry

    providers = {
        "kexbases": lambda: kexbase_tables("kexbases"),
        "kexbasen": lambda: kexbase_tables("kexbasen"),
        "kextls": lambda: c_named_tables(ROOT / "net/kextls/src/apilib.c", ROOT / "net/kextls/src"),
        "kexrt": lambda: c_named_tables(ROOT / "net/kexrt/src/apilib.c", ROOT / "net/kexrt/src"),
    }
    order = default_contents()
    for lib in order:
        if lib not in providers:
            continue  # "std": the system's own functions
        for dll, names in providers[lib]().items():
            for name, impl in names.items():
                apis.setdefault(dll, {})[name] = {
                    "name": name, "kind": classify(dll, name, impl, native), "provider": lib}

    whole = {
        "WINHTTP.DLL": def_exports(ROOT / "net/winhttp/winhttp.def"),
        "BCRYPT.DLL": re.findall(r"__declspec\(dllexport\)[^\n(]*WINAPI\s+(BCrypt\w+)\s*\(",
                                 text(ROOT / "net/bcrypt/bcrypt.c")),
        "KEXTLS.DLL": re.findall(r"KEXTLS_API\s[^;(]*?\b(kextls_\w+)\s*\(",
                                 text(ROOT / "net/kextls/include/kextls.h")),
    }
    for d in sorted(p for p in (ROOT / "auxiliary").glob("*/*.def") if p.stem == p.parent.name):  # kord.def is an import list
        whole[d.stem.upper() + ".DLL"] = def_exports(d)
    for dll, names in whole.items():
        for name in names:
            kind = "replaced" if name in native.get(dll, set()) else "new"
            apis.setdefault(dll, {})[name] = {"name": name, "kind": kind, "provider": dll.lower()}

    doc = {
        "about": "APIs that KernelExEx adds to or replaces in Windows 98 SE and ME, in the default "
                 "configuration. Generated by toolchain/gen_api_inventory.py; do not edit.",
        "configuration": "DCFG1 contents=" + ",".join(order),
        "kinds": KIND_TEXT,
        "dlls": {dll: {"note": DLL_NOTES.get(dll, ""),
                       "functions": [apis[dll][n] for n in sorted(apis[dll], key=str.lower)]}
                 for dll in sorted(apis)},
    }
    return doc


def markdown(doc):
    out = ["# KernelExEx API inventory", "",
           "Generated from the sources by `toolchain/gen_api_inventory.py`; do not edit by hand.",
           "The same data is in [`api.json`](api.json). See [`PORTING.md`](PORTING.md) for how to use it.",
           "", f"Default configuration: `{doc['configuration']}`.", "", "Kinds:", ""]
    for k, v in doc["kinds"].items():
        out.append(f"* **{k}**: {v}")
    out += ["", "| DLL | added | replaced | stub | MSLU | provided by |", "|---|---|---|---|---|---|"]
    for dll, d in doc["dlls"].items():
        c = {k: 0 for k in doc["kinds"]}
        provs = set()
        for f in d["functions"]:
            c[f["kind"]] += 1
            provs.add(f["provider"])
        out.append(f"| {dll} | {c['new']} | {c['replaced']} | {c['stub']} | {c['mslu']} | "
                   f"{', '.join(sorted(provs))} |")
    for dll, d in doc["dlls"].items():
        out += ["", f"## {dll}", ""]
        if d["note"]:
            out += [d["note"], ""]
        for kind in doc["kinds"]:
            names = [f["name"] for f in d["functions"] if f["kind"] == kind]
            if names:
                out.append(f"**{kind}** ({len(names)}): " + ", ".join(f"`{n}`" for n in names))
                out.append("")
    return "\n".join(out).rstrip() + "\n"


def main():
    doc = build()
    js = json.dumps(doc, indent=1, sort_keys=False) + "\n"
    md = markdown(doc)
    if "--check" in sys.argv:
        stale = [p for p, new in ((OUT_JSON, js), (OUT_MD, md)) if not p.exists() or p.read_text() != new]
        if stale:
            sys.exit("API inventory out of date; run toolchain/gen_api_inventory.py: "
                     + ", ".join(str(p.relative_to(ROOT)) for p in stale))
        print("API inventory up to date")
        return
    OUT_JSON.parent.mkdir(exist_ok=True)
    OUT_JSON.write_text(js)
    OUT_MD.write_text(md)
    total = sum(len(d["functions"]) for d in doc["dlls"].values())
    print(f"{total} functions in {len(doc['dlls'])} DLLs -> {OUT_JSON.relative_to(ROOT)}, {OUT_MD.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
