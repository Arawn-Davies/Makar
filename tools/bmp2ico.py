#!/usr/bin/env python3
"""bmp2ico.py -- convert 24/32-bpp uncompressed BMP tiles to single-entry .ico.

Used to generate the Makar desktop icon set (data/icons/*.ico) from the hand-made
BMP tiles, so the window manager's shared ICO loader (src/userspace/img_ico.c)
has real .ico assets to render.  Emits a 24-bpp BMP-DIB entry with an all-zero
(fully opaque) AND mask -- the icons are opaque tiles, matching the GUI's
non-alpha compositing.

Usage:  tools/bmp2ico.py out.ico in.bmp [in.bmp ...]   # multi-size -> one .ico
        tools/bmp2ico.py --batch DIR                    # DIR/*.bmp -> DIR/*.ico
"""
import struct, sys, os, glob


def read_bmp(path):
    d = open(path, "rb").read()
    if d[0:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack("<I", d[10:14])[0]
    w = struct.unpack("<i", d[18:22])[0]
    h = struct.unpack("<i", d[22:26])[0]
    bpp = struct.unpack("<H", d[28:30])[0]
    comp = struct.unpack("<I", d[30:34])[0]
    topdown = h < 0
    h = abs(h)
    if comp != 0 or bpp not in (24, 32):
        raise ValueError(f"{path}: need 24/32-bpp uncompressed")
    bypp = bpp // 8
    stride = ((w * bypp) + 3) & ~3
    rgb = [[None] * w for _ in range(h)]
    for y in range(h):
        srow = y if topdown else (h - 1 - y)
        row = d[off + srow * stride:]
        for x in range(w):
            px = row[x * bypp:x * bypp + bypp]
            rgb[y][x] = (px[2], px[1], px[0])  # BGR -> RGB
    return w, h, rgb


def dib_entry(w, h, rgb):
    """24-bpp bottom-up XOR bitmap + all-zero AND mask, BITMAPINFOHEADER."""
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 24, 0, 0, 0, 0, 0, 0)
    xstride = ((w * 24 + 31) // 32) * 4
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b = rgb[y][x]
            row += bytes([b, g, r])
        while len(row) < xstride:
            row.append(0)
        xor += row
    andstride = ((w + 31) // 32) * 4
    return hdr + bytes(xor) + bytes(andstride * h)


def write_ico(out, bmps):
    entries = []
    for p in bmps:
        w, h, rgb = read_bmp(p)
        entries.append((w, h, dib_entry(w, h, rgb)))
    blob = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    body = b""
    for (w, h, data) in entries:
        blob += struct.pack("<BBBBHHII", w & 0xFF, h & 0xFF, 0, 0, 1, 24, len(data), off)
        body += data
        off += len(data)
    open(out, "wb").write(blob + body)
    print(f"wrote {out} ({len(entries)} entr{'y' if len(entries)==1 else 'ies'})")


def main(argv):
    if len(argv) >= 3 and argv[1] == "--batch":
        d = argv[2]
        for bmp in sorted(glob.glob(os.path.join(d, "*.bmp"))):
            write_ico(os.path.splitext(bmp)[0] + ".ico", [bmp])
        return 0
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    write_ico(argv[1], argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
