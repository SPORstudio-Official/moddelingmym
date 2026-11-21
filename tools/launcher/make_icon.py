#!/usr/bin/env python3
"""Regenerate tools/launcher/ae.ico from the brand artwork.

Windows asks for a different icon size in a different place: 16 px in the
Explorer list view and the taskbar's overflow, 24 px at 125% DPI, 32 px in the
taskbar and Alt+Tab, 48 px on the desktop, 256 px in the Start menu and in the
large-icon view.  An .ico carries all of them, so this script decodes the source
PNG, trims the artwork to its rounded tile, and writes a multi-image .ico plus a
clean 256 px PNG.

No third-party modules: the PNG decoder and the ICO writer are here so the icon
can be rebuilt on any machine that can run the build.

    python3 tools/launcher/make_icon.py [source.png]

The source defaults to brand/ae-icon.png if present, otherwise the existing
ae-256.png is left alone.
"""
import os
import struct
import sys
import zlib

SIZES = [16, 24, 32, 48, 64, 128, 256]


def png_decode(path):
    """Minimal PNG -> [(r,g,b,a)] rows.  Handles 8-bit greyscale/RGB/RGBA/grey-alpha."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('%s is not a PNG' % path)
    i = 8
    idat = b''
    w = h = bitdepth = colortype = None
    while i < len(data):
        (length,) = struct.unpack('>I', data[i:i + 4])
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + length]
        i += 12 + length
        if typ == b'IHDR':
            w, h, bitdepth, colortype, _, _, interlace = struct.unpack('>IIBBBBB', chunk)
            if bitdepth != 8 or interlace != 0:
                raise SystemExit('only 8-bit, non-interlaced PNGs are supported')
        elif typ == b'IDAT':
            idat += chunk
        elif typ == b'IEND':
            break
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[colortype]
    raw = zlib.decompress(idat)
    stride = w * channels
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if filt == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 255
        elif filt == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif filt == 3:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif filt == 4:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = prev[x]
                c = prev[x - channels] if x >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line

    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            o = y * stride + x * channels
            if channels == 4:
                row.append(tuple(out[o:o + 4]))
            elif channels == 3:
                row.append((out[o], out[o + 1], out[o + 2], 255))
            elif channels == 1:
                v = out[o]
                row.append((v, v, v, 255))
            else:
                v = out[o]
                row.append((v, v, v, out[o + 1]))
        rows.append(row)
    return w, h, rows


def trim_to_tile(w, h, rows):
    """Crop the white margin around the rounded tile, then square it up."""
    def white(p):
        return p[0] > 245 and p[1] > 245 and p[2] > 245

    top = 0
    while top < h and all(white(rows[top][x]) for x in range(0, w, 4)):
        top += 1
    bottom = h - 1
    while bottom > 0 and all(white(rows[bottom][x]) for x in range(0, w, 4)):
        bottom -= 1
    left = 0
    while left < w and all(white(rows[y][left]) for y in range(top, bottom, 4)):
        left += 1
    right = w - 1
    while right > 0 and all(white(rows[y][right]) for y in range(top, bottom, 4)):
        right -= 1
    if right <= left or bottom <= top:
        return rows
    cx, cy = (left + right) // 2, (top + bottom) // 2
    side = max(right - left, bottom - top) + 1
    l = max(0, cx - side // 2)
    t = max(0, cy - side // 2)
    r = min(w, l + side)
    b = min(h, t + side)
    return [row[l:r] for row in rows[t:b]]


def resize(rows, size):
    """Box-filter downscale; good enough for icon art and dependency-free."""
    H, W = len(rows), len(rows[0])
    out = []
    for y in range(size):
        y0, y1 = y * H // size, max(y * H // size + 1, (y + 1) * H // size)
        row = []
        for x in range(size):
            x0, x1 = x * W // size, max(x * W // size + 1, (x + 1) * W // size)
            r = g = b = a = n = 0
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    p = rows[yy][xx]
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n += 1
            row.append((r // n, g // n, b // n, a // n))
        out.append(row)
    return out


def to_dib(rows):
    """One ICO image as a 32-bit bottom-up DIB plus its AND mask."""
    n = len(rows)
    header = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            r, g, b, a = rows[y][x]
            pixels += bytes((b, g, r, a))
    mask = bytearray()
    stride = ((n + 31) // 32) * 4
    for y in range(n - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(n):
            if rows[y][x][3] < 128:
                bits[x >> 3] |= 0x80 >> (x & 7)
        mask += bits
    return header + bytes(pixels) + bytes(mask)


def write_png(path, rows):
    n = len(rows)
    raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in rows)

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw, 9)) +
        chunk(b'IEND', b''))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'ae-source.png')
    dst = os.path.join(here, 'ae.ico')
    if not os.path.isfile(src):
        raise SystemExit('source artwork not found: %s' % src)
    w, h, rows = png_decode(src)
    rows = trim_to_tile(w, h, rows)
    images = [(s, resize(rows, s)) for s in SIZES]

    header = struct.pack('<HHH', 0, 1, len(images))
    entries = b''
    blobs = b''
    offset = 6 + 16 * len(images)
    for size, px in images:
        blob = to_dib(px)
        dim = 0 if size >= 256 else size
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(blob), offset)
        blobs += blob
        offset += len(blob)
    open(dst, 'wb').write(header + entries + blobs)
    write_png(os.path.join(here, 'ae-256.png'), images[-1][1])
    print('wrote %s (%d bytes, %d sizes) and ae-256.png'
          % (dst, len(header + entries + blobs), len(images)))


if __name__ == '__main__':
    main()
