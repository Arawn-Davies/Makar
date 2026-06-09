#!/usr/bin/env python3
"""guitest_wallpaper_check.py -- assert the desktop wallpaper rendered.

The GUITEST build stages a ~/.mxrc Wallpaper= so the window manager loads a
wallpaper at startup and blits it (stretched) behind the icons instead of the
flat COL_DESK fill.  This checks the guitest screendump: with a wallpaper the
flat fill is gone, so almost no pixels equal COL_DESK exactly; a flat desktop
leaves a large contiguous COL_DESK area (the uncovered background).

Exit 0 if the wallpaper appears present (few COL_DESK pixels), 1 otherwise.
"""
import struct, sys

COL_DESK = (0x1e, 0x29, 0x3b)   # RGB(30,41,59) -- the flat desktop fill
THRESHOLD = 0.10                # >10% exact-COL_DESK pixels => looks flat


def load_bmp(path):
    d = open(path, "rb").read()
    if d[0:2] != b"BM":
        raise SystemExit("not a BMP")
    off = struct.unpack("<I", d[10:14])[0]
    w = struct.unpack("<i", d[18:22])[0]
    h = struct.unpack("<i", d[22:26])[0]
    bpp = struct.unpack("<H", d[28:30])[0]
    topdown = h < 0
    h = abs(h)
    bypp = bpp // 8
    stride = ((w * bypp) + 3) & ~3
    return d, off, w, h, bypp, stride, topdown


def main(argv):
    if len(argv) < 2:
        print("usage: guitest_wallpaper_check.py <screendump.bmp>", file=sys.stderr)
        return 2
    d, off, w, h, bypp, stride, topdown = load_bmp(argv[1])
    desk = 0
    total = w * h
    for y in range(h):
        sy = y if topdown else (h - 1 - y)
        row = off + sy * stride
        for x in range(w):
            p = row + x * bypp
            if d[p + 2] == COL_DESK[0] and d[p + 1] == COL_DESK[1] and d[p] == COL_DESK[2]:
                desk += 1
    frac = desk / total if total else 1.0
    print(f"COL_DESK pixels: {desk}/{total} = {frac:.1%} (threshold {THRESHOLD:.0%})")
    if frac < THRESHOLD:
        return 0   # wallpaper present
    return 1       # looks flat


if __name__ == "__main__":
    sys.exit(main(sys.argv))
