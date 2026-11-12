#!/usr/bin/env python3
"""Generates the PS Vita LiveArea assets (sce_sys folder) from the game's icon and logo.

The Vita refuses to install a VPK whose LiveArea images are not 8-bit indexed, non-interlaced
PNGs of the exact expected size, so everything is resized and quantized here. This is plain
Python on purpose (no Pillow), so that the build has no extra dependencies.

usage: mklivearea.py <icon.png> <logo.png> <output sce_sys dir>
"""

import os
import struct
import sys
import zlib

BACKGROUND = (20, 21, 25)   # dark grey, same tone as the DJUI dark theme


# ---------------------------------------------------------------------------------------------
# PNG reading
# ---------------------------------------------------------------------------------------------

def read_png(path):
    """Returns (width, height, pixels) with pixels being a list of rows of (r, g, b, a) tuples."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('%s is not a PNG file' % path)

    pos = 8
    idat = []
    palette = None
    trns = None
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'PLTE':
            palette = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif kind == b'tRNS':
            trns = chunk
        elif kind == b'IDAT':
            idat.append(chunk)
        elif kind == b'IEND':
            break

    if interlace:
        raise ValueError('%s: interlaced PNGs are not supported' % path)
    if depth != 8:
        raise ValueError('%s: only 8 bit per channel PNGs are supported' % path)

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = width * channels
    raw = zlib.decompress(b''.join(idat))

    rows = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        prev = line

        if ctype == 6:
            row = [(line[i], line[i + 1], line[i + 2], line[i + 3]) for i in range(0, stride, 4)]
        elif ctype == 2:
            row = [(line[i], line[i + 1], line[i + 2], 255) for i in range(0, stride, 3)]
        elif ctype == 0:
            row = [(v, v, v, 255) for v in line]
        elif ctype == 4:
            row = [(line[i], line[i], line[i], line[i + 1]) for i in range(0, stride, 2)]
        else:
            row = []
            for v in line:
                r, g, b = palette[v]
                a = trns[v] if (trns is not None and v < len(trns)) else 255
                row.append((r, g, b, a))
        rows.append(row)

    return width, height, rows


# ---------------------------------------------------------------------------------------------
# image operations (images are lists of rows of premultiplied float (r, g, b, a) tuples)
# ---------------------------------------------------------------------------------------------

def premultiply(rows):
    out = []
    for row in rows:
        out.append([(r * a / 255.0, g * a / 255.0, b * a / 255.0, a / 255.0) for (r, g, b, a) in row])
    return out


def resize(rows, src_w, src_h, dst_w, dst_h):
    """Area averaging resize (good for downscaling), on premultiplied pixels."""
    # horizontal pass
    def spans(src, dst):
        result = []
        scale = src / float(dst)
        for d in range(dst):
            start = d * scale
            end = (d + 1) * scale
            first = int(start)
            last = min(int(end - 1e-9), src - 1)
            weights = []
            for s in range(first, last + 1):
                w = min(end, s + 1) - max(start, s)
                if w > 0:
                    weights.append((s, w))
            total = sum(w for _, w in weights) or 1.0
            result.append([(s, w / total) for s, w in weights])
        return result

    xspans = spans(src_w, dst_w)
    yspans = spans(src_h, dst_h)

    tmp = []
    for row in rows:
        new_row = []
        for span in xspans:
            r = g = b = a = 0.0
            for s, w in span:
                pr, pg, pb, pa = row[s]
                r += pr * w
                g += pg * w
                b += pb * w
                a += pa * w
            new_row.append((r, g, b, a))
        tmp.append(new_row)

    out = []
    for span in yspans:
        new_row = []
        for x in range(dst_w):
            r = g = b = a = 0.0
            for s, w in span:
                pr, pg, pb, pa = tmp[s][x]
                r += pr * w
                g += pg * w
                b += pb * w
                a += pa * w
            new_row.append((r, g, b, a))
        out.append(new_row)
    return out


def canvas(width, height, color):
    return [[(float(color[0]), float(color[1]), float(color[2]))] * width for _ in range(height)]


def gradient_canvas(width, height, top, bottom):
    rows = []
    for y in range(height):
        t = y / float(max(1, height - 1))
        c = tuple(top[i] * (1 - t) + bottom[i] * t for i in range(3))
        rows.append([c] * width)
    return rows


def blit(dst, src, x0, y0):
    """Alpha blends a premultiplied image onto an opaque RGB canvas."""
    for y, row in enumerate(src):
        dy = y + y0
        if dy < 0 or dy >= len(dst):
            continue
        drow = list(dst[dy])
        for x, (r, g, b, a) in enumerate(row):
            dx = x + x0
            if dx < 0 or dx >= len(drow):
                continue
            br, bg, bb = drow[dx]
            inv = 1.0 - a
            drow[dx] = (r + br * inv, g + bg * inv, b + bb * inv)
        dst[dy] = drow


def fit(src_w, src_h, max_w, max_h):
    scale = min(max_w / float(src_w), max_h / float(src_h))
    return max(1, int(round(src_w * scale))), max(1, int(round(src_h * scale)))


# ---------------------------------------------------------------------------------------------
# quantization + PNG writing
# ---------------------------------------------------------------------------------------------

def quantize(rows, max_colors=256):
    """Median cut on a 5 bit per channel histogram. Returns (palette, index rows)."""
    def clamp8(v):
        return 0 if v < 0 else (255 if v > 255 else int(v + 0.5))

    pixels = [[(clamp8(r), clamp8(g), clamp8(b)) for (r, g, b) in row] for row in rows]

    hist = {}
    for row in pixels:
        for (r, g, b) in row:
            key = (r >> 3, g >> 3, b >> 3)
            entry = hist.get(key)
            if entry is None:
                hist[key] = [1, r, g, b]
            else:
                entry[0] += 1
                entry[1] += r
                entry[2] += g
                entry[3] += b

    boxes = [list(hist.items())]
    while len(boxes) < max_colors:
        # split the box with the largest (range * population)
        best = None
        best_score = 0
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            ranges = []
            for c in range(3):
                values = [k[c] for k, _ in box]
                ranges.append(max(values) - min(values))
            score = (max(ranges) + 1) * sum(v[0] for _, v in box)
            if score > best_score:
                best_score = score
                best = (i, ranges.index(max(ranges)))
        if best is None:
            break
        i, channel = best
        box = sorted(boxes[i], key=lambda item: item[0][channel])
        total = sum(v[0] for _, v in box)
        acc = 0
        cut = 1
        for j, (_, v) in enumerate(box):
            acc += v[0]
            if acc >= total / 2:
                cut = max(1, min(len(box) - 1, j + 1))
                break
        boxes[i] = box[:cut]
        boxes.append(box[cut:])

    palette = []
    lookup = {}
    for index, box in enumerate(boxes):
        n = sum(v[0] for _, v in box)
        r = sum(v[1] for _, v in box) // n
        g = sum(v[2] for _, v in box) // n
        b = sum(v[3] for _, v in box) // n
        palette.append((r, g, b))
        for key, _ in box:
            lookup[key] = index

    indices = [bytes(lookup[(r >> 3, g >> 3, b >> 3)] for (r, g, b) in row) for row in pixels]
    return palette, indices


def write_indexed_png(path, width, height, palette, indices):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload) & 0xFFFFFFFF)

    raw = b''.join(b'\x00' + row for row in indices)
    plte = b''.join(bytes(c) for c in palette)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 3, 0, 0, 0))
    png += chunk(b'PLTE', plte)
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def save(path, rows):
    palette, indices = quantize(rows)
    write_indexed_png(path, len(rows[0]), len(rows), palette, indices)
    print('  %s (%dx%d, %d colors)' % (path, len(rows[0]), len(rows), len(palette)))


# ---------------------------------------------------------------------------------------------

TEMPLATE_XML = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>

  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1

    icon_path, logo_path, out_dir = sys.argv[1:4]
    contents_dir = os.path.join(out_dir, 'livearea', 'contents')
    os.makedirs(contents_dir, exist_ok=True)

    iw, ih, icon = read_png(icon_path)
    lw, lh, logo = read_png(logo_path)
    icon = premultiply(icon)
    logo = premultiply(logo)

    top = (38, 40, 48)
    bottom = BACKGROUND

    # icon0.png: 128x128 bubble icon
    img = canvas(128, 128, BACKGROUND)
    w, h = fit(iw, ih, 120, 120)
    blit(img, resize(icon, iw, ih, w, h), (128 - w) // 2, (128 - h) // 2)
    save(os.path.join(out_dir, 'icon0.png'), img)

    # startup.png: 280x158 gate image
    img = gradient_canvas(280, 158, top, bottom)
    w, h = fit(lw, lh, 260, 138)
    blit(img, resize(logo, lw, lh, w, h), (280 - w) // 2, (158 - h) // 2)
    save(os.path.join(contents_dir, 'startup.png'), img)

    # bg.png: 840x500 LiveArea background
    img = gradient_canvas(840, 500, top, bottom)
    w, h = fit(lw, lh, 560, 300)
    blit(img, resize(logo, lw, lh, w, h), (840 - w) // 2, 40)
    save(os.path.join(contents_dir, 'bg.png'), img)

    with open(os.path.join(contents_dir, 'template.xml'), 'w', newline='\n') as f:
        f.write(TEMPLATE_XML)

    return 0


if __name__ == '__main__':
    sys.exit(main())
