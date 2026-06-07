#!/usr/bin/env python3
"""Minimal 24-bit BMP -> 8-bit RGB PNG (stdlib zlib only; no Pillow/ImageMagick).
Used to turn QEMU screendumps (PPM -> BMP -> PNG) into README-embeddable images."""
import sys, struct, zlib

def bmp_to_png(src, dst):
    d = open(src, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w   = struct.unpack_from("<i", d, 18)[0]
    h   = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 24:
        raise SystemExit("only 24-bpp BMP supported, got %d" % bpp)
    bottom_up = h > 0
    h = abs(h)
    row_size = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(h):
        sy = (h - 1 - y) if bottom_up else y
        rs = off + sy * row_size
        raw.append(0)                       # PNG filter: none
        for x in range(w):
            b = d[rs + x*3 + 0]; g = d[rs + x*3 + 1]; r = d[rs + x*3 + 2]
            raw += bytes((r, g, b))
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    png  = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print("%s -> %s (%dx%d)" % (src, dst, w, h))

if __name__ == "__main__":
    bmp_to_png(sys.argv[1], sys.argv[2])
