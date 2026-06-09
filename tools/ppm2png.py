#!/usr/bin/env python3
"""Convert a binary PPM (P6) — e.g. a QEMU `screendump` — to PNG, stdlib only.

Usage: ppm2png.py in.ppm out.png
"""
import sys, zlib, struct


def read_ppm(path):
    d = open(path, "rb").read()
    assert d[:2] == b"P6", "not a P6 PPM"
    i, vals = 2, []
    while len(vals) < 3:
        while i < len(d) and d[i] in b" \t\n\r":
            i += 1
        if d[i:i + 1] == b"#":
            while d[i] not in b"\n":
                i += 1
            continue
        s = i
        while d[i] not in b" \t\n\r":
            i += 1
        vals.append(int(d[s:i]))
    i += 1  # single whitespace after maxval
    w, h, _ = vals
    return w, h, d[i:i + w * h * 3]


def write_png(w, h, rgb, out):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                      # filter: none
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8-bit, truecolour
    idat = zlib.compress(bytes(raw), 9)
    open(out, "wb").write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


if __name__ == "__main__":
    w, h, rgb = read_ppm(sys.argv[1])
    write_png(w, h, rgb, sys.argv[2])
    print(f"ok {w}x{h} -> {sys.argv[2]}")
