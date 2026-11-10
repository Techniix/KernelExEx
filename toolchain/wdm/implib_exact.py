#!/usr/bin/env python3
"""Make an i386 import library whose import names keep their stdcall decoration.

Windows 98 SE's usbd.sys exports some functions as "_Name@N" (the decorated
symbol itself), which llvm-dlltool cannot express: it always strips the
prefix or the "@N". This builds the library with llvm-dlltool from
"Name@N" entries, then sets every short import record's name type to
IMPORT_OBJECT_NAME (1), so the import name is the symbol name verbatim:
"_Name@N".

usage: implib_exact.py in.def out.a
"""
import subprocess, sys

def main():
    src, out = sys.argv[1], sys.argv[2]
    subprocess.run(["llvm-dlltool", "-m", "i386", "-d", src, "-l", out], check=True)
    data = bytearray(open(out, "rb").read())
    pos, n = 8, 0                       # skip "!<arch>\n"
    while pos + 60 <= len(data):
        size = int(data[pos + 48:pos + 58].decode().strip())
        body = pos + 60
        if data[body:body + 4] == b"\0\0\xff\xff" and int.from_bytes(data[body + 6:body + 8], "little") == 0x14C:
            t = int.from_bytes(data[body + 18:body + 20], "little")
            t = (t & ~0x1C) | (1 << 2)  # name type: IMPORT_OBJECT_NAME
            data[body + 18:body + 20] = t.to_bytes(2, "little")
            n += 1
        pos = body + size + (size & 1)
    open(out, "wb").write(data)
    print(f"{out}: {n} imports with exact names")

if __name__ == "__main__":
    main()
