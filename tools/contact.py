"""Hoja de contactos: junta frames PPM en una cuadrícula (uso: contact.py dir salida.ppm f1 f2 ...)."""
import sys


def load(path):
    d = open(path, 'rb').read()
    head, px = d.split(b'\n', 1)
    _, w, h, _ = head.split()
    return int(w), int(h), px


def main():
    folder, out, frames = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3:]]
    cols, cw, ch = 4, 320, 240
    rows = (len(frames) + cols - 1) // cols
    W, H = cols * cw, rows * ch
    canvas = bytearray(W * H * 3)
    for i, f in enumerate(frames):
        w, h, px = load(f'{folder}/frame_{f:05d}.ppm')
        ox, oy = (i % cols) * cw, (i // cols) * ch
        for y in range(min(h, ch)):
            for x in range(cw):
                sx = x * w // cw  # escala horizontal simple para anchos != 320
                o = ((oy + y) * W + ox + x) * 3
                canvas[o:o + 3] = px[(y * w + sx) * 3:(y * w + sx) * 3 + 3]
    open(out, 'wb').write(b'P6 %d %d 255\n' % (W, H) + canvas)


if __name__ == '__main__':
    main()
