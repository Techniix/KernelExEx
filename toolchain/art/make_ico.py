#!/usr/bin/env python3
"""Make a Windows 9x-compatible .ico from an SVG: 16, 32 and 48 pixel
images in 256 colours (8 bpp) and 16 colours (4 bpp, the standard VGA
palette), each with its AND mask. Windows 98 cannot read PNG-compressed or
32-bit alpha icons, which is what most tools write today.

usage: make_ico.py in.svg out.ico
"""
import io
import struct
import subprocess
import sys

from PIL import Image

VGA = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
       (0, 255, 255), (255, 255, 255)]


def render(svg, size):
    png = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), svg], check=True,
                         capture_output=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")


def entry(img, bpp):
    w, h = img.size
    alpha = img.getchannel("A")
    # composite on white for the colour part where partly transparent
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    flat = Image.alpha_composite(bg, img).convert("RGB")
    if bpp == 4:
        pal = Image.new("P", (1, 1))
        pal.putpalette(sum(VGA, ()) + (0, 0, 0) * (256 - 16))
        q = flat.quantize(palette=pal, dither=Image.Dither.NONE)
        palette = VGA
    else:
        q = flat.quantize(colors=255, dither=Image.Dither.NONE)
        p = q.getpalette()[:768]
        palette = [tuple(p[i:i + 3]) for i in range(0, len(p), 3)]
    ncol = 1 << bpp
    palette = (palette + [(0, 0, 0)] * ncol)[:ncol]
    px = q.load()
    a = alpha.load()
    rowbytes = ((w * bpp + 31) // 32) * 4
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(rowbytes)
        for x in range(w):
            v = 0 if a[x, y] < 128 else px[x, y]
            if bpp == 8:
                row[x] = v
            else:
                row[x // 2] |= (v << 4) if x % 2 == 0 else v
        xor += row
    mrow = ((w + 31) // 32) * 4
    andm = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(mrow)
        for x in range(w):
            if a[x, y] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        andm += row
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, bpp, 0, len(xor) + len(andm), 0, 0, ncol, 0)
    pal = b"".join(struct.pack("BBBB", b, g, r, 0) for r, g, b in palette)
    return hdr + pal + bytes(xor) + bytes(andm)


def main():
    svg, out = sys.argv[1], sys.argv[2]
    images = []
    for size, bpp in ((48, 8), (32, 8), (16, 8), (32, 4), (16, 4)):
        images.append((size, bpp, entry(render(svg, size), bpp)))
    data = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    body = b""
    for size, bpp, blob in images:
        data += struct.pack("<BBBBHHII", size % 256, size % 256, 0 if bpp == 8 else 16, 0, 1, bpp, len(blob), off)
        off += len(blob)
        body += blob
    open(out, "wb").write(data + body)


if __name__ == "__main__":
    main()
