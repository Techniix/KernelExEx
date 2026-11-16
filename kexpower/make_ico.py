#!/usr/bin/env python3
"""Draw kexpower.ico (a battery, 16/32/48 px, 8-bit and 4-bit colour with an
AND mask, as Windows 98 reads them). Run: python3 make_ico.py kexpower.ico"""
import struct, sys
from PIL import Image, ImageDraw


def battery(size):
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    s = size / 16.0
    body = [round(1 * s), round(4 * s), round(13 * s), round(12 * s)]
    d.rectangle(body, fill=(255, 255, 255, 255), outline=(0, 0, 0, 255), width=max(1, round(s)))
    d.rectangle([round(13 * s), round(6 * s), round(15 * s), round(10 * s)], fill=(0, 0, 0, 255))
    inner = [body[0] + round(2 * s), body[1] + round(2 * s), body[0] + round(8 * s), body[3] - round(2 * s)]
    d.rectangle(inner, fill=(0, 160, 0, 255))
    return im


def entry(im, bpp):
    size = im.size[0]
    colors = 256 if bpp == 8 else 16
    rgb = im.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    pal = rgb.getpalette()[: colors * 3] + [0] * (colors * 3 - len(rgb.getpalette()[: colors * 3]))
    alpha = im.getchannel("A")
    rowbytes = ((size * bpp + 31) // 32) * 4
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray()
        if bpp == 8:
            for x in range(size):
                row.append(rgb.getpixel((x, y)) if alpha.getpixel((x, y)) >= 128 else 0)
        else:
            for x in range(0, size, 2):
                a = rgb.getpixel((x, y)) if alpha.getpixel((x, y)) >= 128 else 0
                b = rgb.getpixel((x + 1, y)) if alpha.getpixel((x + 1, y)) >= 128 else 0
                row.append((a << 4) | b)
        xor += row + bytes(rowbytes - len(row))
    maskrow = ((size + 31) // 32) * 4
    andm = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(maskrow)
        for x in range(size):
            if alpha.getpixel((x, y)) < 128:
                bits[x // 8] |= 0x80 >> (x % 8)
        andm += bits
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, bpp, 0, len(xor) + len(andm), 0, 0, colors, 0)
    palb = b"".join(struct.pack("BBBB", pal[i * 3 + 2], pal[i * 3 + 1], pal[i * 3], 0) for i in range(colors))
    return size, colors, bpp, hdr + palb + bytes(xor) + bytes(andm)


def main():
    ents = [entry(battery(s), b) for s, b in ((48, 8), (32, 8), (16, 8), (32, 4), (16, 4))]
    out = bytearray(struct.pack("<HHH", 0, 1, len(ents)))
    off = 6 + 16 * len(ents)
    for size, colors, bpp, data in ents:
        out += struct.pack("<BBBBHHII", size % 256, size % 256, colors % 256, 0, 1, bpp, len(data), off)
        off += len(data)
    for e in ents:
        out += e[3]
    open(sys.argv[1], "wb").write(out)


if __name__ == "__main__":
    main()
