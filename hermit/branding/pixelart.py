"""Pixel-art icons from 16x16 text grids: crisp PNG, SVG and ICO without any renderer.

Each grid character is one pixel ('.' is transparent). Sizes that are a multiple of 16 scale the
pixels exactly; other sizes use the largest whole scale and centre the picture (20 and 24 px
stretch instead, so the small Windows sizes are not left tiny).
"""
import struct
import zlib

PALETTE = {
    'K': (0x1E, 0x2A, 0x44),   # ink outline
    'W': (0xFF, 0xF1, 0xDC),   # cream
    'T': (0x4F, 0xC7, 0xBE),   # teal
    't': (0x2B, 0x8F, 0x88),   # dark teal
    'S': (0xBF, 0xF0, 0xEC),   # teal shine
    'R': (0xFF, 0x7A, 0x5C),   # coral
    'r': (0xC9, 0x4F, 0x3A),   # dark coral
    'A': (0xFF, 0xC8, 0x57),   # amber
    'a': (0xD9, 0x9A, 0x2B),   # dark amber
    'Y': (0xFF, 0xE6, 0xA8),   # amber shine
    'G': (0x8C, 0x96, 0xA8),   # slate
    'g': (0x5E, 0x68, 0x78),   # dark slate
    'L': (0xD5, 0xDA, 0xE3),   # slate shine
    'w': (0xFF, 0xFF, 0xFF),   # white
}


def parse(text):
    rows = [r.strip() for r in text.strip('\n').split('\n') if r.strip()]
    assert all(len(r) == 16 for r in rows) and len(rows) == 16, 'grids are 16x16'
    return rows


def recolor(grid, mapping):
    return [''.join(mapping.get(c, c) for c in row) for row in grid]


def overlay(grid, patch, x0, y0):
    rows = [list(r) for r in grid]
    for dy, row in enumerate(patch):
        for dx, ch in enumerate(row):
            if ch != '.':
                rows[y0 + dy][x0 + dx] = ch
    return [''.join(r) for r in rows]


def _layout(size, n=16):
    """(pixel edges along one axis) for a picture of n pixels in a size x size image."""
    scale = size // n
    if scale >= 2 or size == n:
        pad = (size - scale * n) // 2
        return [pad + i * scale for i in range(n + 1)]
    # 20 and 24 px: stretch, rounding each pixel edge
    return [round(i * size / n) for i in range(n + 1)]


def png(grid, size):
    edges = _layout(size)
    n = len(grid)
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        gy = next((i for i in range(n) if edges[i] <= y < edges[i + 1]), None)
        for x in range(size):
            gx = next((i for i in range(n) if edges[i] <= x < edges[i + 1]), None)
            c = PALETTE.get(grid[gy][gx]) if gy is not None and gx is not None else None
            raw.extend((*c, 255) if c else (0, 0, 0, 0))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def runs(grid):
    """(colour char, x, y, length) horizontal runs of one colour."""
    for y, row in enumerate(grid):
        x = 0
        while x < len(row):
            ch = row[x]
            if ch not in PALETTE:
                x += 1
                continue
            n = 1
            while x + n < len(row) and row[x + n] == ch:
                n += 1
            yield ch, x, y, n
            x += n


def svg(grid):
    rects = ''.join('<rect x="%d" y="%d" width="%d" height="1" fill="#%02X%02X%02X"/>' % (x, y, n, *PALETTE[ch])
                    for ch, x, y, n in runs(grid))
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="256" height="256" '
            'shape-rendering="crispEdges">%s</svg>\n' % rects)


def ico(grid, sizes=(16, 20, 24, 32, 40, 48, 64, 128, 256)):
    images = [png(grid, s) for s in sizes]
    head = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b'', b''
    for s, data in zip(sizes, images):
        dim = 0 if s >= 256 else s
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    return head + entries + blobs
