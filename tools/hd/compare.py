"""Compara modelos de IA de escalado 4x sobre poses grabadas (runtime/texdump.c).

Uso (con el Python que tiene torch y spandrel, ver tools/hd/README.md):
  python tools/hd/compare.py <salida> <dir modelos .pth> <realesrgan-ncnn-vulkan> pose1.tex pose2.tex ...

Para cada pose: rellena lo transparente con el color opaco más cercano (evita halos), escala el
color con cada modelo, escala también la silueta y la umbraliza, y ajusta cada píxel al color
más cercano de la paleta original (así la versión HD sigue respondiendo a los cambios de paleta
del juego). Genera comparacion.png (con paleta) y comparacion_sin_paleta.png (salida directa).
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
import torch
from PIL import Image, ImageDraw
from spandrel import ModelLoader

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texlib import c15, read_tex  # noqa: E402

PAD = 6
NCNN_MODELS = ['realesrgan-x4plus-anime', 'realesr-animevideov3']


def pose_arrays(t):
    pal = np.array([c15(c) for c in t['pal']], dtype=np.uint8)
    idx = np.array(t['px'], dtype=np.uint8).reshape(t['h'], t['w'])
    rgba = pal[idx]
    rgba[idx == 0] = 0
    return np.pad(rgba, ((PAD, PAD), (PAD, PAD), (0, 0))), pal


def bleed(rgba):
    """Extiende los colores opacos hacia lo transparente (dilatación repetida)."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    for _ in range(PAD + 8):
        if known.all():
            break
        acc = np.zeros_like(rgb); cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            k = np.roll(known, (dy, dx), (0, 1)); c = np.roll(rgb, (dy, dx), (0, 1))
            acc += c * k[..., None]; cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known = known | new
    return rgb.astype(np.uint8)


def snap(rgb, alpha, pal):
    """Cada píxel opaco al color más cercano de la paleta (sin el índice 0, transparente)."""
    colors = pal[1:, :3].astype(np.int32)
    d = ((rgb[..., None, :].astype(np.int32) - colors[None, None]) ** 2).sum(-1)
    out = np.zeros(rgb.shape[:2] + (4,), np.uint8)
    out[..., :3] = colors[d.argmin(-1)]
    out[..., 3] = np.where(alpha, 255, 0)
    return out


def run_torch(model, rgb, dev):
    x = torch.from_numpy(rgb).permute(2, 0, 1)[None].float().div(255).to(dev)
    with torch.no_grad():
        y = model(x)
    return (y[0].clamp(0, 1).permute(1, 2, 0).cpu().numpy() * 255 + 0.5).astype(np.uint8)


def run_ncnn(exe, name, rgb):
    with tempfile.TemporaryDirectory() as td:
        src, dst = os.path.join(td, 'in.png'), os.path.join(td, 'out.png')
        Image.fromarray(rgb).save(src)
        models = os.path.join(os.path.dirname(exe), 'models')
        subprocess.run([exe, '-i', src, '-o', dst, '-n', name, '-s', '4', '-m', models], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return np.array(Image.open(dst).convert('RGB'))


def main():
    out, mdir, ncnn, poses = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    os.makedirs(out, exist_ok=True)
    dev = torch.device('mps' if torch.backends.mps.is_available() else 'cpu')
    upscalers = [('Original', None)]
    for f in sorted(os.listdir(mdir)):
        if f.endswith('.pth'):
            m = ModelLoader().load_from_file(os.path.join(mdir, f)).model.eval().to(dev)
            upscalers.append((f[:-4], lambda rgb, m=m: run_torch(m, rgb, dev)))
    for n in NCNN_MODELS:
        upscalers.append((n, lambda rgb, n=n: run_ncnn(ncnn, n, rgb)))
    rows, raw_rows = [], []
    for p in poses:
        rgba, pal = pose_arrays(read_tex(p))
        rgb = bleed(rgba)
        mask = np.repeat(np.where(rgba[..., 3:] > 0, 255, 0).astype(np.uint8), 3, axis=2)
        near = rgba.repeat(4, 0).repeat(4, 1)
        row, raw = [near], [near]
        for name, up in upscalers[1:]:
            hd = up(rgb)
            a = up(mask).mean(-1) > 127
            row.append(snap(hd, a, pal))
            r = np.zeros(hd.shape[:2] + (4,), np.uint8); r[..., :3] = hd; r[..., 3] = np.where(a, 255, 0)
            raw.append(r)
            print(os.path.basename(p), name, 'ok', flush=True)
        rows.append(row); raw_rows.append(raw)
    names = [n for n, _ in upscalers]
    for fname, data in (('comparacion.png', rows), ('comparacion_sin_paleta.png', raw_rows)):
        cw = max(img.shape[1] for r in data for img in r) + 12
        heights = [max(img.shape[0] for img in r) + 12 for r in data]
        sheet = Image.new('RGBA', (cw * len(names), 28 + sum(heights)), (60, 62, 72, 255))
        dr = ImageDraw.Draw(sheet)
        for i, n in enumerate(names):
            dr.text((i * cw + 8, 8), n, fill=(235, 235, 240, 255))
        y = 28
        for r, h in zip(data, heights):
            for i, img in enumerate(r):
                im = Image.fromarray(img, 'RGBA')
                sheet.alpha_composite(im, (i * cw + 6, y + 6))
            y += h
        sheet.save(os.path.join(out, fname))
        print('guardado', os.path.join(out, fname))


if __name__ == '__main__':
    main()
