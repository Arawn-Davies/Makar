#!/usr/bin/env python3
"""Convert a binary PPM (P6, what `qemu` screendump writes) to a 24-bit BMP.

Pure stdlib (no PIL) so it runs in the CI container.  Usage:
    ppm2bmp.py in.ppm out.bmp
"""
import struct
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError("not a P6 PPM")
    # Parse the header: P6 <w> <h> <maxval>, whitespace-separated, then 1 byte.
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":   # comment line
            while idx < len(data) and data[idx] not in b"\r\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        fields.append(int(data[start:idx]))
    w, h, _maxv = fields
    idx += 1  # single whitespace after maxval
    return w, h, data[idx:idx + w * h * 3]


def write_bmp(path, w, h, rgb):
    row_pad = (-w * 3) % 4
    img_size = (w * 3 + row_pad) * h
    with open(path, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", 14 + 40 + img_size, 0, 0, 14 + 40))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, img_size, 2835, 2835, 0, 0))
        pad = b"\x00" * row_pad
        for y in range(h - 1, -1, -1):          # BMP rows are bottom-up
            row = y * w * 3
            for x in range(w):
                p = row + x * 3
                f.write(bytes((rgb[p + 2], rgb[p + 1], rgb[p])))  # BGR
            f.write(pad)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: ppm2bmp.py in.ppm out.bmp")
    w, h, rgb = read_ppm(sys.argv[1])
    write_bmp(sys.argv[2], w, h, rgb)


if __name__ == "__main__":
    main()
