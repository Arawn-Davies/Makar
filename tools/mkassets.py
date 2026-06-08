#!/usr/bin/env python3
"""mkassets.py -- generate Makar's example wallpapers + logos.

Outputs (committed, so the build needs nothing at run time):
  data/backgrounds/<colour>.bmp   12 gradient wallpapers (one per major colour)
  data/logos/makar-leaf-light.{png,bmp}   green leaf/violin mark, light theme
  data/logos/makar-leaf-dark.{png,bmp}     same mark, dark theme
  data/logos/makar-mark.png                compact mark (README / favicon)
  docs/img/makar-logo.png                   copy of the light logo for the README

Stdlib only (zlib for PNG, struct for BMP).  Backgrounds are 640x360 (the WM
scales them to fit); the image viewer (mximg) decodes the BMPs directly.  Public
domain.  Run from the repo root: python3 tools/mkassets.py
"""
import os, struct, zlib, math

# ---- writers -----------------------------------------------------------------

def write_png(path, w, h, rgb):           # rgb: list of rows, each w*(r,g,b)
    raw = bytearray()
    for y in range(h):
        raw.append(0)                      # filter: none
        row = rgb[y]
        for (r, g, b) in row:
            raw += bytes((r, g, b))
    comp = zlib.compress(bytes(raw), 9)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", comp))
        f.write(chunk(b"IEND", b""))

def write_bmp(path, w, h, rgb):
    row_bytes = (w * 3 + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):         # bottom-up
        row = bytearray()
        for (r, g, b) in rgb[y]:
            row += bytes((b, g, r))        # BGR
        row += b"\x00" * (row_bytes - len(row))
        data += row
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr); f.write(dib); f.write(data)

# ---- canvas helpers ----------------------------------------------------------

def canvas(w, h, fill):
    return [[fill for _ in range(w)] for _ in range(h)]

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

# ---- 12 colour backgrounds (diagonal gradient: dark -> colour) --------------

COLOURS = {
    "red":     (0xd0, 0x33, 0x2e), "orange": (0xe0, 0x77, 0x20),
    "amber":   (0xe6, 0xb0, 0x20), "lime":   (0x7c, 0xc4, 0x33),
    "green":   (0x2f, 0x9e, 0x44), "teal":   (0x14, 0xa0, 0x8a),
    "cyan":    (0x22, 0xb0, 0xc8), "blue":   (0x2f, 0x6f, 0xd0),
    "indigo":  (0x4b, 0x3f, 0xc0), "violet": (0x8a, 0x46, 0xc8),
    "magenta": (0xc0, 0x3f, 0x9a), "slate":  (0x4a, 0x55, 0x66),
}

def make_background(base):
    w, h = 640, 360
    dark = tuple(c // 5 for c in base)     # near-black tint of the hue
    img = canvas(w, h, base)
    for y in range(h):
        for x in range(w):
            t = (x / w + y / h) / 2.0      # diagonal 0..1
            img[y][x] = lerp(dark, base, t)
    return w, h, img

# ---- logo: a green stencil leaf shaped like a violin + "MAKAR" ---------------

GREEN = (0x3a, 0xa3, 0x4a)

# 5x7 uppercase glyphs for the wordmark (M A K A R).
FONT = {
 'M': ["X...X","XX.XX","X.X.X","X.X.X","X...X","X...X","X...X"],
 'A': [".XXX.","X...X","X...X","XXXXX","X...X","X...X","X...X"],
 'K': ["X...X","X..X.","X.X..","XX...","X.X..","X..X.","X...X"],
 'R': ["XXXX.","X...X","X...X","XXXX.","X.X..","X..X.","X...X"],
 ' ': ["....." for _ in range(7)],
}

def draw_text(img, x, y, s, scale, col):
    for ch in s:
        g = FONT.get(ch, FONT[' '])
        for r in range(7):
            for c in range(5):
                if g[r][c] == 'X':
                    for dy in range(scale):
                        for dx in range(scale):
                            px, py = x + c*scale + dx, y + r*scale + dy
                            if 0 <= py < len(img) and 0 <= px < len(img[0]):
                                img[py][px] = col
        x += (5 + 1) * scale

def in_violin(nx, ny):
    """Unit-ish violin/leaf body: two stacked bulbs (figure-8) + pointed tips.
    nx,ny in [-1,1]; returns True inside the silhouette."""
    # upper bout (smaller) centred at y=-0.45, lower bout (larger) at y=0.45
    upper = (nx / 0.55) ** 2 + ((ny + 0.45) / 0.50) ** 2 <= 1.0
    lower = (nx / 0.70) ** 2 + ((ny - 0.40) / 0.58) ** 2 <= 1.0
    waist = abs(nx) <= 0.30 and -0.55 <= ny <= 0.55          # connect the bouts
    tip   = abs(nx) <= (0.10 * (1.0 - abs(ny))) and abs(ny) <= 1.0  # leaf points
    return upper or lower or waist or tip

def make_logo(bg, fg_text, accent=GREEN, w=480, h=300):
    img = canvas(w, h, bg)
    cx, cy, R = w // 2, 132, 120
    for y in range(h):
        for x in range(w):
            nx = (x - cx) / R
            ny = (y - cy) / R
            if abs(nx) <= 1.2 and abs(ny) <= 1.2 and in_violin(nx, ny):
                img[y][x] = accent
    # central vein (leaf) -> doubles as the violin's neck/strings
    for y in range(cy - R, cy + R):
        if 0 <= y < h:
            if in_violin(0.0, (y - cy) / R):
                img[y][cx] = bg
    # two f-hole slits (violin cue)
    for y in range(cy + 6, cy + 46):
        for x in (cx - 26, cx + 25):
            if 0 <= y < h and in_violin((x - cx) / R, (y - cy) / R):
                img[y][x] = bg
    # wordmark
    scale = 6
    tw = (5 + 1) * scale * len("MAKAR")
    draw_text(img, (w - tw) // 2, h - 56, "MAKAR", scale, fg_text)
    return w, h, img

# ---- main --------------------------------------------------------------------

def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    bgdir = os.path.join(root, "data", "backgrounds")
    logodir = os.path.join(root, "data", "logos")
    imgdir = os.path.join(root, "docs", "img")
    for d in (bgdir, logodir, imgdir):
        os.makedirs(d, exist_ok=True)

    for name, base in COLOURS.items():
        w, h, img = make_background(base)
        write_bmp(os.path.join(bgdir, name + ".bmp"), w, h, img)
        print("bg", name)

    DARK = (0x24, 0x28, 0x2e); WHITE = (0xf0, 0xf2, 0xf4); GREY = (0x2a, 0x2e, 0x34)
    for nm, bg, fg in (("makar-leaf-light", WHITE, GREY),
                       ("makar-leaf-dark",  DARK,  WHITE)):
        w, h, img = make_logo(bg, fg)
        write_png(os.path.join(logodir, nm + ".png"), w, h, img)
        write_bmp(os.path.join(logodir, nm + ".bmp"), w, h, img)
        print("logo", nm)

    # compact mark for README / favicon (transparent-ish: white bg)
    w, h, img = make_logo(WHITE, GREY, w=240, h=240)
    write_png(os.path.join(logodir, "makar-mark.png"), w, h, img)
    # README / Pages logo
    w, h, img = make_logo(WHITE, GREY)
    write_png(os.path.join(imgdir, "makar-logo.png"), w, h, img)
    print("done")

if __name__ == "__main__":
    main()
