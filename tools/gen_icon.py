#!/usr/bin/env python3
"""Generate assets/deadair.ico : a lo-fi oscilloscope mark."""
from PIL import Image, ImageDraw
import math

def make(size):
    s = size
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # body
    d.rectangle([0, 0, s - 1, s - 1], fill=(10, 14, 11, 255))
    b = max(1, s // 16)
    d.rectangle([0, 0, s - 1, s - 1], outline=(196, 168, 96, 255), width=b)
    # phosphor screen
    m = max(2, s // 8)
    d.rectangle([m, m, s - 1 - m, s - 1 - m], fill=(6, 26, 12, 255))
    # waveform
    pts = []
    n = max(8, s // 2)
    for i in range(n + 1):
        x = m + (s - 1 - 2 * m) * i / n
        y = s / 2 + math.sin(i / n * math.pi * 3) * (s * 0.22)
        pts.append((x, y))
    lw = max(1, s // 16)
    d.line(pts, fill=(110, 240, 130, 255), width=lw)
    # red peak dot
    r = max(1, s // 16)
    d.ellipse([s - m - 3 * r, m, s - m - r, m + 2 * r], fill=(230, 60, 40, 255))
    return im

import struct, io

def ico_entry(im):
    """32bpp BMP icon entry (BITMAPINFOHEADER + BGRA + AND mask)."""
    w, h = im.size
    px = im.load()
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    xor = b"".join(rows)
    mask_row = ((w + 31) // 32) * 4
    mask = b"".join(b"\x00" * mask_row for _ in range(h))
    return hdr + xor + mask

sizes = [16, 32, 48, 64, 128, 256]
images = [make(s) for s in sizes]
entries = [ico_entry(im) for im in images]
out = bytearray()
out += struct.pack("<HHH", 0, 1, len(images))
offset = 6 + 16 * len(images)
for im, data in zip(images, entries):
    w, h = im.size
    out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
for data in entries:
    out += data
open("assets/deadair.ico", "wb").write(bytes(out))
print("wrote assets/deadair.ico", len(out), "bytes")
