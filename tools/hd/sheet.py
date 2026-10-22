"""Hoja de muestras: python3 -I tools/hd/sheet.py <carpeta .tex> <salida.png> <paleta|*> [máx]
Coloca los trozos (de mayor a menor) en filas, a 2x, sobre fondo gris para ver la transparencia."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texlib import read_tex, to_rgba, write_png

d, out, pal = sys.argv[1], sys.argv[2], sys.argv[3]
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 120
files = [f for f in os.listdir(d) if pal == '*' or f[:-4].endswith(pal)]
texs = sorted((read_tex(os.path.join(d, f)) for f in files), key=lambda t: -t['w'] * t['h'])[:limit]
W, S, x, y, rowh, placed = 1400, 2, 4, 4, 0, []
for t in texs:
    w, h = t['w'] * S, t['h'] * S
    if x + w > W:
        x, y, rowh = 4, y + rowh + 4, 0
    placed.append((t, x, y)); x += w + 4; rowh = max(rowh, h)
H = y + rowh + 4
img = [(70, 70, 80, 255)] * (W * H)
for t, ox, oy in placed:
    px = to_rgba(t)
    for j in range(t['h'] * S):
        for i in range(t['w'] * S):
            p = px[(j // S) * t['w'] + i // S]
            if p[3]:
                img[(oy + j) * W + ox + i] = p
write_png(out, W, H, img)
print(len(files), 'trozos;', len(placed), 'en la hoja', W, 'x', H)
