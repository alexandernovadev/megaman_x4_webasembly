"""Convierte capturas de capas (runtime/hd.c) en piezas HD a 8x para el paquete HD.

Uso (con el Python que tiene torch y spandrel, ver tools/hd/README.md):
  python -I tools/hd/upscale.py <paquete> cap1.bin cap2.bin ...     una vez
  python -I tools/hd/upscale.py --watch <cola> <paquete>             servicio: procesa lo que
                                                                     llega a la cola y avisa
                                                                     por stdout ("HD <huella>")

Cada captura es una capa de la pantalla (un plano del fondo, un personaje...) alrededor de sus
piezas nuevas, y la lista de esas piezas (índices + paleta + posición). La capa entera se
mejora de una vez y después se recortan las piezas, así que las uniones entre piezas salen de
la misma imagen y no quedan costuras. Pasos: rellenar lo transparente con el color opaco
vecino (evita halos), escalar 4x con la IA (color y silueta por separado), refinar a 8x con
bicúbico, recortar cada pieza, deshacer el volteo con que se dibujó y ajustar cada píxel a un
índice de su paleta. El resultado son índices, así la versión HD sigue los cambios de paleta
del juego: <paquete>/<huella>.x4h = zlib("X4H1", u16 ancho, u16 alto, índices).
"""
import os
import struct
import sys
import time
import zlib

import numpy as np
import torch
import torch.nn.functional as F
from spandrel import ModelLoader

MODEL = os.path.expanduser('~/tools/hd-ai/models/4x-Fatal-Pixels.pth')
SCALE = 8
EDGE = 1  # píxeles HD del borde de la pieza que siguen la silueta original


def c15(c):
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def parse(buf):
    """Capas de un archivo de capturas; se para en un registro incompleto."""
    i = 0
    while i + 16 <= len(buf) and buf[i:i + 4] == b'X4S1':
        ln, = struct.unpack_from('<I', buf, i + 4)
        if i + ln > len(buf):
            break
        cw, ch, nt, _ = struct.unpack_from('<4H', buf, i + 8)
        o = i + 16
        canvas = np.frombuffer(buf, np.uint8, cw * ch * 4, o).reshape(ch, cw, 4); o += cw * ch * 4
        tiles = []
        for _ in range(nt):
            h, = struct.unpack_from('<Q', buf, o)
            x, y = struct.unpack_from('<2h', buf, o + 8)
            rw, rh, depth, flip = buf[o + 12:o + 16]
            colors, = struct.unpack_from('<H', buf, o + 16)
            o += 20
            pal = np.frombuffer(buf, '<u2', colors, o); o += colors * 2
            px = np.frombuffer(buf, np.uint8, rw * rh, o).reshape(rh, rw); o += rw * rh
            tiles.append(dict(hash=h, x=x, y=y, rw=rw, rh=rh, flip=flip, pal=pal, px=px))
        yield dict(canvas=canvas, tiles=tiles)
        i += ln


def bleed(rgba):
    """Rellena lo transparente con los colores opacos cercanos (pirámide: media de los opacos a
    escalas cada vez más bastas, y de vuelta hacia arriba solo donde no había nada)."""
    rgb = rgba[..., :3].astype(np.float32)
    a = (rgba[..., 3] > 0).astype(np.float32)
    if a.all() or not a.any():
        return rgba[..., :3].copy()
    levels = []
    c, w = rgb * a[..., None], a
    while min(w.shape) > 1:
        levels.append((c, w))
        h2, w2 = (w.shape[0] + 1) // 2 * 2, (w.shape[1] + 1) // 2 * 2
        cp = np.zeros((h2, w2, 3), np.float32); cp[:c.shape[0], :c.shape[1]] = c
        wp = np.zeros((h2, w2), np.float32); wp[:w.shape[0], :w.shape[1]] = w
        c = cp.reshape(h2 // 2, 2, w2 // 2, 2, 3).sum((1, 3))
        w = wp.reshape(h2 // 2, 2, w2 // 2, 2).sum((1, 3))
    fill = c / np.maximum(w, 1e-6)[..., None]
    for c, w in reversed(levels):
        up = fill.repeat(2, 0).repeat(2, 1)[:w.shape[0], :w.shape[1]]
        mine = c / np.maximum(w, 1e-6)[..., None]
        fill = np.where((w > 0)[..., None], mine, up)
    return np.where(a[..., None] > 0, rgb, fill).clip(0, 255).astype(np.uint8)


class Upscaler:
    def __init__(self):
        self.dev = torch.device('mps' if torch.backends.mps.is_available() else 'cpu')
        self.net = ModelLoader().load_from_file(MODEL).model.eval().to(self.dev)

    def run(self, imgs):
        """imgs: N x H x W x 3 uint8 -> N x 3 x 8H x 8W float (0..1) en la GPU."""
        x = torch.from_numpy(np.stack(imgs)).permute(0, 3, 1, 2).float().div(255).to(self.dev)
        with torch.no_grad():
            y = self.net(x)
            y = F.interpolate(y, scale_factor=SCALE // 4, mode='bicubic', align_corners=False)
        return y.clamp(0, 1)

    def process(self, scenes, pack):
        done = []
        for sc in scenes:
            cv = sc['canvas']
            ch, cw = cv.shape[:2]
            # Tamaño múltiplo de 16 (menos formas distintas para la GPU).
            H, W = (ch + 15) // 16 * 16, (cw + 15) // 16 * 16
            pad = np.zeros((H, W, 4), np.uint8); pad[:ch, :cw] = cv
            res = self.run([bleed(pad), np.repeat(pad[..., 3:], 3, 2)])
            for t in sc['tiles']:
                x, y, rw, rh = t['x'] * SCALE, t['y'] * SCALE, t['rw'] * SCALE, t['rh'] * SCALE
                col = res[0, :, y:y + rh, x:x + rw].permute(1, 2, 0).cpu().numpy() * 255
                alpha = res[1, :, y:y + rh, x:x + rw].mean(0).cpu().numpy() > 0.5
                if t['flip'] & 1: col, alpha = col[:, ::-1], alpha[:, ::-1]
                if t['flip'] & 2: col, alpha = col[::-1], alpha[::-1]
                # Borde de la pieza: la silueta sigue a la original, para que no queden rendijas
                # de un píxel con la vecina.
                lr = (t['pal'][t['px']] != 0).repeat(SCALE, 0).repeat(SCALE, 1)
                band = np.ones(alpha.shape, bool)
                band[EDGE:-EDGE, EDGE:-EDGE] = False
                alpha = np.where(band, lr, alpha)
                write(pack, t, snap(col, alpha, t))
                done.append(t['hash'])
        return done


def snap(col, alpha, t):
    """Cada píxel al índice usado por la pieza de color más cercano. Si varios índices tienen
    ese mismo color (paletas con colores repetidos), el del texel original más cercano: así
    la pieza sigue bien cuando el juego cambia la paleta y esos índices dejan de coincidir."""
    pal, px = t['pal'], t['px']
    used = np.unique(px)
    opaque = [int(i) for i in used if pal[i]]
    clear = [int(i) for i in used if not pal[i]] or [i for i in range(len(pal)) if not pal[i]] or [0]
    out = np.full(alpha.shape, clear[0], np.uint8)
    if opaque:
        colors = np.array([c15(int(pal[i])) for i in opaque], np.float32)
        d = ((col[..., None, :] - colors[None, None]) ** 2).sum(-1)
        idx = np.array(opaque, np.uint8)[d.argmin(-1)]
        near = px.repeat(SCALE, 0).repeat(SCALE, 1)
        allc = np.array([c15(int(c)) for c in pal], np.float32)
        same = (allc[near] == allc[idx]).all(-1) & (pal[near] != 0)
        idx = np.where(same, near, idx)
        out[alpha] = idx[alpha]
    return out


def write(pack, t, idx):
    h, w = idx.shape
    data = zlib.compress(b'X4H1' + struct.pack('<2H', w, h) + idx.tobytes(), 6)
    tmp = os.path.join(pack, '%016x.tmp' % t['hash'])
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, os.path.join(pack, '%016x.x4h' % t['hash']))


def pending(pack, files):
    """Capas con piezas que aún no están en el paquete (cada pieza, una sola vez)."""
    seen, out = set(), []
    for f in files:
        with open(f, 'rb') as fh:
            buf = fh.read()
        for sc in parse(buf):
            tiles = [t for t in sc['tiles']
                     if t['hash'] not in seen and not os.path.exists(os.path.join(pack, '%016x.x4h' % t['hash']))]
            seen.update(t['hash'] for t in tiles)
            if tiles:
                sc['tiles'] = tiles
                out.append(sc)
    return out


def main():
    if sys.argv[1] == '--watch':
        queue, pack = sys.argv[2], sys.argv[3]
        os.makedirs(queue, exist_ok=True); os.makedirs(pack, exist_ok=True)
        try:
            os.nice(10)
        except OSError:
            pass
        up = Upscaler()
        print('LISTO', flush=True)
        while True:
            files = sorted(os.path.join(queue, f) for f in os.listdir(queue) if f.endswith('.bin'))
            if not files:
                time.sleep(0.3)
                continue
            files = files[:8]
            for sc in pending(pack, files):
                for h in up.process([sc], pack):
                    print('HD %016x' % h, flush=True)
            for f in files:
                os.remove(f)
    pack, files = sys.argv[1], sys.argv[2:]
    os.makedirs(pack, exist_ok=True)
    scenes = pending(pack, files)
    total = sum(len(s['tiles']) for s in scenes)
    print('%d capas, %d piezas nuevas' % (len(scenes), total), flush=True)
    up = Upscaler()
    t, n = time.time(), 0
    for k, sc in enumerate(scenes):
        n += len(up.process([sc], pack))
        if k % 50 == 49 or k == len(scenes) - 1:
            print('%d/%d piezas (%.0f s)' % (n, total, time.time() - t), flush=True)


if __name__ == '__main__':
    main()
