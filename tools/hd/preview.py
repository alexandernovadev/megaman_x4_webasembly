"""Vista previa del HD sin navegador: dibuja una lista de dibujo guardada con X4_HDLIST
(runtime/main.c) usando las piezas del paquete, por CPU y a escala S (por defecto 4x).

  X4_HDLIST=dir:3000,5000 ./build/x4 disc 5001 ...
  python3 tools/hd/preview.py hd_pack dir/hdlist_05000.bin salida.png [escala]

Solo dibuja como la GPU lo esencial (rectángulos y cuadriláteros alineados, sin mezclas): sirve
para comprobar que cada pieza HD cae en su sitio, con su orientación y su paleta.
"""
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image


def c15(c):
    c = np.asarray(c, np.uint32)
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return np.stack([r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2], -1).astype(np.float32)


def main():
    pack, path, out = sys.argv[1:4]
    S = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    buf = open(path, 'rb').read()
    nv, nb, margin, npal = struct.unpack_from('<4i', buf, 0)
    o = 16
    V = np.frombuffer(buf, np.uint8, nv * 32, o).reshape(nv, 32); o += nv * 32
    o += nb * 28
    vram = np.frombuffer(buf, '<u2', 1024 * 512, o).reshape(512, 1024); o += 1024 * 512 * 2
    pals = np.frombuffer(buf, '<u2', npal * 256, o).reshape(npal, 256); o += npal * 512
    nid, = struct.unpack_from('<i', buf, o); o += 4
    id_hash = np.frombuffer(buf, '<u8', nid, o)
    fl = V[:, :16].copy().view('<f4').reshape(nv, 4)
    it = V[:, 16:].copy().view('<u4').reshape(nv, 4)
    W, H = (320 + 2 * margin) * S, 240 * S
    img = np.zeros((H, W, 3), np.float32)
    cache, hits, total = {}, 0, 0
    names = {}
    for k in range(0, nv - 5, 6):
        q = fl[k:k + 6]; a = [int(x) for x in it[k]]
        flags = a[0] >> 24
        xs, ys = q[:, 0] + margin, q[:, 1]
        x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
        X0, X1, Y0, Y1 = int(round(x0 * S)), int(round(x1 * S)), int(round(y0 * S)), int(round(y1 * S))
        X0c, Y0c, X1c, Y1c = max(X0, 0), max(Y0, 0), min(X1, W), min(Y1, H)
        if X1c <= X0c or Y1c <= Y0c:
            continue
        gx, gy = np.meshgrid(np.arange(X0c, X1c) + 0.5, np.arange(Y0c, Y1c) + 0.5)
        if not flags & 1:
            if flags & 4:
                continue
            img[Y0c:Y1c, X0c:X1c] = np.array([a[0] & 255, (a[0] >> 8) & 255, (a[0] >> 16) & 255], np.float32)
            continue
        # u, v en las esquinas: interpolación lineal en x e y (rectángulos y quads alineados).
        def at(px, py):
            for i in range(6):
                if abs(xs[i] - px) < 1e-3 and abs(ys[i] - py) < 1e-3:
                    return q[i, 2], q[i, 3]
            return None
        c00, c10, c01 = at(x0, y0), at(x1, y0), at(x0, y1)
        if not (c00 and c10 and c01):
            continue
        fx = (gx / S - x0) / max(x1 - x0, 1e-6); fy = (gy / S - y0) / max(y1 - y0, 1e-6)
        u = c00[0] + (c10[0] - c00[0]) * fx; v = c00[1] + (c01[1] - c00[1]) * fy
        tex = a[1]; pal = pals[(tex >> 7) & 32767] if (tex >> 5) & 3 < 2 else None
        tpx, tpy, depth = (tex & 15) * 64, ((tex >> 4) & 1) * 256, (tex >> 5) & 3
        hid = a[2] & 0xffff
        total += 1
        idx = None
        if hid and (flags & 1):
            names.setdefault(hid, None)
        # Pieza HD: buscar por huella (la tabla id -> huella no está aquí: se recalcula).
        u0, v0 = (a[2] >> 16) & 255, a[2] >> 24
        rw, rh = ((a[3] >> 20) & 15) + 1, ((a[3] >> 24) & 15) + 1
        h = None
        if hid and depth <= 1:
            h = int(id_hash[hid])
            f = os.path.join(pack, '%016x.x4h' % h)
            if h not in cache:
                cache[h] = None
                if os.path.exists(f):
                    d = zlib.decompress(open(f, 'rb').read())
                    w8, h8 = struct.unpack_from('<2H', d, 4)
                    cache[h] = np.frombuffer(d, np.uint8, w8 * h8, 8).reshape(h8, w8)
            hd = cache[h]
            if hd is not None:
                hits += 1
                tx = np.clip(((u - u0) * 8).astype(int), 0, rw * 8 - 1)
                ty = np.clip(((v - v0) * 8).astype(int), 0, rh * 8 - 1)
                idx = hd[ty, tx].astype(np.int64)
        if idx is None:
            uu = np.floor(u).astype(int) & 255; vv = (tpy + (np.floor(v).astype(int) & 255)) & 511
            if depth == 0:
                idx = (vram[vv, (tpx + uu // 4) & 1023].astype(np.int64) >> ((uu & 3) * 4)) & 15
            elif depth == 1:
                idx = (vram[vv, (tpx + uu // 2) & 1023].astype(np.int64) >> ((uu & 1) * 8)) & 255
            else:
                col = vram[vv, (tpx + uu) & 1023]
                m = col != 0
                img[Y0c:Y1c, X0c:X1c][m] = c15(col[m])
                continue
        col = pal[idx & 255]
        m = col != 0
        rgb = c15(col)
        if not flags & 2:
            rgb = np.minimum(rgb * (np.array([a[0] & 255, (a[0] >> 8) & 255, (a[0] >> 16) & 255], np.float32) / 128), 255)
        img[Y0c:Y1c, X0c:X1c][m] = rgb[m]
    Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(out)
    print('piezas %d, con HD %d' % (total, hits))


if __name__ == '__main__':
    main()
