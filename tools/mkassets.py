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
import os, struct, zlib, math, shutil

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

# ---- PNG reader (8-bit RGBA, non-interlaced) + scaler ------------------------

def read_png_rgba(path):
    d = open(path, "rb").read(); assert d[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8; w = h = bd = ct = 0; idat = bytearray()
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos+4])[0]; typ = d[pos+4:pos+8]
        ch = d[pos+8:pos+8+ln]; pos += 12 + ln
        if   typ == b"IHDR": w, h, bd, ct = struct.unpack(">IIBB", ch[:10])
        elif typ == b"IDAT": idat += ch
        elif typ == b"IEND": break
    assert bd == 8 and ct == 6, (bd, ct)           # 8-bit RGBA only
    raw = zlib.decompress(bytes(idat)); bpp = 4; st = w * bpp
    out = []; prev = bytearray(st); p = 0
    for _y in range(h):
        f = raw[p]; p += 1; line = bytearray(raw[p:p+st]); p += st
        for i in range(st):                         # un-filter scanline
            a = line[i-bpp] if i >= bpp else 0; b = prev[i]
            c = prev[i-bpp] if i >= bpp else 0; x = line[i]
            if   f == 0: v = x
            elif f == 1: v = x + a
            elif f == 2: v = x + b
            elif f == 3: v = x + ((a + b) >> 1)
            else:
                pp = a + b - c; pa = abs(pp-a); pb = abs(pp-b); pc = abs(pp-c)
                v = x + (a if (pa <= pb and pa <= pc) else (b if pb <= pc else c))
            line[i] = v & 0xff
        prev = line
        out.append([(line[x*4], line[x*4+1], line[x*4+2], line[x*4+3]) for x in range(w)])
    return w, h, out

def scale_rgba(rows, sw, sh, dw, dh):               # nearest-neighbour
    return [[rows[min(sh-1, y*sh//dh)][min(sw-1, x*sw//dw)] for x in range(dw)]
            for y in range(dh)]

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

_LOGO = None
def load_logo():
    """Decode the source emblem (data/logos/makar-logo-src.png) once."""
    global _LOGO
    if _LOGO is None:
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        _LOGO = read_png_rgba(os.path.join(root, "data", "logos", "makar-logo-src.png"))
    return _LOGO

def blit_disc(img, dst_x, dst_y, size):
    """Composite the green-disc emblem (scaled to size x size) onto img with a
    circular mask, so the source's opaque black corners are dropped and only the
    disc + its border ring land on the target background."""
    lw, lh, lr = load_logo()
    s = scale_rgba(lr, lw, lh, size, size)
    c = size / 2.0
    rmask2 = (size * 0.488) ** 2          # just outside the disc, inside corners
    for y in range(size):
        for x in range(size):
            if (x - c) ** 2 + (y - c) ** 2 > rmask2:
                continue
            r, g, b, a = s[y][x]
            px, py = dst_x + x, dst_y + y
            if 0 <= py < len(img) and 0 <= px < len(img[0]) and a > 8:
                img[py][px] = (r, g, b) if a >= 250 else lerp(img[py][px], (r, g, b), a / 255.0)

def write_emblem_header(path, size=128):
    """Emit a kernel C header with the disc emblem (size x size, row-major, one
    0x00RRGGBB word per pixel; 0xFF000000 = transparent outside the disc).  The
    boot splash draws this before the VFS is up, so it must be embedded."""
    lw, lh, lr = load_logo()
    s = scale_rgba(lr, lw, lh, size, size)
    c = size / 2.0; rmask2 = (size * 0.49) ** 2
    vals = []
    for y in range(size):
        for x in range(size):
            if (x - c) ** 2 + (y - c) ** 2 > rmask2:
                vals.append(0xFF000000)
            else:
                r, g, b, _a = s[y][x]
                vals.append((r << 16) | (g << 8) | b)
    with open(path, "w") as f:
        f.write("/* AUTO-GENERATED by tools/mkassets.py -- do not edit by hand. */\n")
        f.write("#ifndef KERNEL_LOGO_EMBLEM_H\n#define KERNEL_LOGO_EMBLEM_H\n#include <stdint.h>\n\n")
        f.write("/* The Makar disc emblem, %dx%d, row-major, one 0x00RRGGBB word per\n" % (size, size))
        f.write(" * pixel.  0xFF000000 = transparent (outside the disc) -- the boot\n")
        f.write(" * splash skips those. */\n")
        f.write("#define LOGO_EMBLEM_W %du\n#define LOGO_EMBLEM_H %du\n\n" % (size, size))
        f.write("static const uint32_t logo_emblem[] = {\n")
        for i in range(0, len(vals), 12):
            f.write("    " + ", ".join("0x%08X" % v for v in vals[i:i+12]) + ",\n")
        f.write("};\n\n#endif /* KERNEL_LOGO_EMBLEM_H */\n")

def make_logo(bg, fg_text, w=480, h=300):
    """The branded 'logo + MAKAR text' mark: the disc emblem over a flat
    background with the wordmark below.  Used for makar-leaf-{light,dark}."""
    img = canvas(w, h, bg)
    size = min(w - 40, h - 86)
    blit_disc(img, (w - size) // 2, 8, size)
    scale = max(3, w // 80)
    tw = (5 + 1) * scale * len("MAKAR")
    draw_text(img, (w - tw) // 2, 8 + size + 12, "MAKAR", scale, fg_text)
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

    # Compact mark + README/Pages logo: the exact polished source emblem.
    src = os.path.join(logodir, "makar-logo-src.png")
    shutil.copyfile(src, os.path.join(logodir, "makar-mark.png"))
    shutil.copyfile(src, os.path.join(imgdir, "makar-logo.png"))

    # Kernel-embedded emblem for the graphical boot splash.
    write_emblem_header(os.path.join(root, "src", "kernel", "include",
                                     "kernel", "logo_emblem.h"))
    print("emblem header")
    print("done")

if __name__ == "__main__":
    main()
