"""Lectura de los .tex del modo grabar (runtime/texdump.c) y escritura de PNG sin dependencias."""
import struct
import zlib


def read_tex(path):
    d = open(path, 'rb').read()
    assert d[:4] in (b'X4TX', b'X4T2'), path
    w, h, depth, colors = struct.unpack_from('<4H', d, 4)
    frame, = struct.unpack_from('<I', d, 12)
    o = 16
    page = uv = clut = None
    if d[:4] == b'X4T2':
        tpx, tpy, uv0, cx, cy = struct.unpack_from('<5H', d, o); o += 10
        page, uv, clut = (tpx, tpy), (uv0 & 255, uv0 >> 8), (cx, cy)
    pal = list(struct.unpack_from(f'<{colors}H', d, o)); o += colors * 2
    if depth == 2:
        px = list(struct.unpack_from(f'<{w * h}H', d, o))
    else:
        px = list(d[o:o + w * h])
    return {'w': w, 'h': h, 'depth': depth, 'pal': pal, 'px': px, 'frame': frame,
            'page': page, 'uv': uv, 'clut': clut}


def c15(c):
    """Color de 15 bits de la PS1 a RGBA; 0x0000 es transparente."""
    if c == 0:
        return (0, 0, 0, 0)
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255)


def to_rgba(t, pal=None):
    pal = t['pal'] if pal is None else pal
    if t['depth'] == 2:
        return [c15(p) for p in t['px']]
    return [c15(pal[p]) for p in t['px']]


def write_png(path, w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for p in rgba[y * w:(y + 1) * w]:
            raw.extend(p)
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def read_png(path):
    """PNG RGBA/RGB de 8 bits sin entrelazado (lo que escriben write_png y Real-ESRGAN)."""
    d = open(path, 'rb').read()
    o, idat, w = 8, b'', 0
    while o < len(d):
        n, = struct.unpack_from('>I', d, o); tag = d[o + 4:o + 8]; data = d[o + 8:o + 8 + n]; o += 12 + n
        if tag == b'IHDR':
            w, h, bits, ctype = struct.unpack_from('>IIBB', data)
            assert bits == 8 and ctype in (2, 6), (path, bits, ctype)
            ch = 4 if ctype == 6 else 3
        elif tag == b'IDAT':
            idat += data
    raw = zlib.decompress(idat)
    stride = w * ch
    out, prev, i = [], bytearray(stride), 0
    for y in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            px = line[x * ch:x * ch + ch]
            out.append(tuple(px) + ((255,) if ch == 3 else ()))
    return w, h, out
