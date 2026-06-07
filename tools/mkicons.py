#!/usr/bin/env python3
"""mkicons.py -- generate the Makar desktop icon set as flat 24-bpp BMP tiles.

Outputs 32x26 BMPs (the size the window manager blits 1:1 into each icon tile)
under data/icons/<name>.bmp.  Stdlib only; the BMPs are committed so the build
has no dependency on this script -- it exists so the art is reproducible and
tweakable.  Decoded by src/userspace/img_bmp.c (24-bpp, BGR, bottom-up).

Run from the repo root:  python3 tools/mkicons.py
"""
import os, struct

W, H = 32, 26
BG = (0x2a, 0x38, 0x50)          # tile background (matches wm.c icon_glyph bg)

def new_img():
    return [[BG for _ in range(W)] for _ in range(H)]

def rect(img, x, y, w, h, c):
    for yy in range(y, y + h):
        if 0 <= yy < H:
            for xx in range(x, x + w):
                if 0 <= xx < W:
                    img[yy][xx] = c

def outline(img, x, y, w, h, c):
    rect(img, x, y, w, 1, c); rect(img, x, y + h - 1, w, 1, c)
    rect(img, x, y, 1, h, c); rect(img, x + w - 1, y, 1, h, c)

def disc(img, cx, cy, r, c):
    for yy in range(cy - r, cy + r + 1):
        for xx in range(cx - r, cx + r + 1):
            if 0 <= xx < W and 0 <= yy < H and (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r:
                img[yy][xx] = c

def ring(img, cx, cy, r, c):
    for yy in range(cy - r, cy + r + 1):
        for xx in range(cx - r, cx + r + 1):
            d = (xx - cx) ** 2 + (yy - cy) ** 2
            if 0 <= xx < W and 0 <= yy < H and (r - 1) ** 2 <= d <= r * r:
                img[yy][xx] = c

def write_bmp(path, img):
    row_bytes = (W * 3 + 3) & ~3
    data = bytearray()
    for y in range(H - 1, -1, -1):          # bottom-up
        row = bytearray()
        for x in range(W):
            r, g, b = img[y][x]
            row += bytes((b, g, r))          # BGR
        row += b"\x00" * (row_bytes - len(row))
        data += row
    size = 54 + len(data)
    hdr = struct.pack("<2sIHHI", b"BM", size, 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr); f.write(dib); f.write(data)

# ---- per-icon art (flat, recognisable at 32x26) -------------------------------

def i_terminal(im):
    rect(im, 1, 2, 30, 22, (0x10, 0x16, 0x20)); outline(im, 1, 2, 30, 22, (0x4c, 0x8d, 0xff))
    rect(im, 4, 6, 3, 3, (0x8a, 0xe2, 0x34)); rect(im, 7, 9, 3, 3, (0x8a, 0xe2, 0x34))
    rect(im, 4, 12, 3, 3, (0x8a, 0xe2, 0x34)); rect(im, 12, 16, 10, 3, (0x8a, 0xe2, 0x34))

def i_files(im):
    rect(im, 2, 4, 12, 4, (0xc8, 0x8a, 0x20))
    rect(im, 1, 7, 30, 16, (0xf0, 0xa8, 0x30)); rect(im, 1, 10, 30, 2, (0xc8, 0x8a, 0x20))

def i_editor(im):
    rect(im, 6, 2, 20, 22, (0xe6, 0xea, 0xf0)); outline(im, 6, 2, 20, 22, (0x35, 0xc7, 0x59))
    for k in range(4): rect(im, 9, 6 + k * 4, 14, 2, (0x90, 0xa0, 0xb5))

def i_tasks(im):
    rect(im, 2, 2, 28, 22, (0x1b, 0x22, 0x2e))
    for k, w in enumerate((22, 13, 18)): rect(im, 4, 5 + k * 6, w, 4, (0x9b, 0x6c, 0xff))

def i_doom(im):
    rect(im, 3, 2, 26, 22, (0xc0, 0x40, 0x40))
    rect(im, 8, 8, 5, 6, (0x20, 0x06, 0x06)); rect(im, 19, 8, 5, 6, (0x20, 0x06, 0x06))
    rect(im, 10, 18, 12, 3, (0x20, 0x06, 0x06))

def i_about(im):
    disc(im, 16, 13, 11, (0x35, 0x6a, 0xa8))
    rect(im, 15, 6, 3, 3, (0xff, 0xff, 0xff)); rect(im, 15, 11, 3, 9, (0xff, 0xff, 0xff))

def i_clock(im):
    disc(im, 16, 13, 11, (0x10, 0x2a, 0x26)); ring(im, 16, 13, 11, (0x40, 0xc0, 0xb0))
    rect(im, 15, 6, 2, 8, (0xff, 0xff, 0xff)); rect(im, 16, 12, 7, 2, (0xff, 0xff, 0xff))

def i_calc(im):
    rect(im, 4, 2, 24, 22, (0x20, 0x24, 0x2c)); rect(im, 7, 5, 18, 5, (0x8a, 0xe2, 0x34))
    for r in range(3):
        for k in range(3): rect(im, 7 + k * 6, 12 + r * 4, 4, 3, (0xe0, 0x80, 0x40))

def i_net(im):
    rect(im, 4, 16, 5, 7, (0x4c, 0xb0, 0xff)); rect(im, 13, 11, 5, 12, (0x4c, 0xb0, 0xff))
    rect(im, 22, 5, 5, 18, (0x4c, 0xb0, 0xff))

def i_disk(im):
    rect(im, 2, 3, 28, 20, (0x20, 0x1c, 0x10)); outline(im, 5, 6, 22, 14, (0xc0, 0xa0, 0x40))
    ring(im, 16, 13, 6, (0x80, 0x6a, 0x28)); rect(im, 15, 12, 3, 3, (0xc0, 0xa0, 0x40))

def i_install(im):
    rect(im, 2, 2, 28, 22, (0x30, 0x16, 0x16))
    rect(im, 14, 4, 4, 9, (0xff, 0xc0, 0xc0))
    rect(im, 10, 12, 12, 2, (0xff, 0xc0, 0xc0)); rect(im, 12, 14, 8, 2, (0xff, 0xc0, 0xc0))
    rect(im, 14, 16, 4, 2, (0xff, 0xc0, 0xc0)); rect(im, 5, 20, 22, 3, (0xff, 0x70, 0x70))

def i_image(im):
    rect(im, 2, 2, 28, 22, (0x10, 0x22, 0x12)); outline(im, 4, 4, 24, 18, (0x70, 0xb0, 0x70))
    disc(im, 11, 10, 3, (0xff, 0xe0, 0x60)); rect(im, 5, 15, 22, 6, (0x40, 0x90, 0x50))

ICONS = [
    ("terminal", i_terminal), ("files", i_files), ("editor", i_editor),
    ("tasks", i_tasks), ("doom", i_doom), ("about", i_about),
    ("clock", i_clock), ("calc", i_calc), ("net", i_net),
    ("disk", i_disk), ("install", i_install), ("image", i_image),
]

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "data", "icons")
    os.makedirs(out, exist_ok=True)
    for name, fn in ICONS:
        im = new_img(); fn(im)
        write_bmp(os.path.join(out, name + ".bmp"), im)
        print("wrote", name + ".bmp")

if __name__ == "__main__":
    main()
