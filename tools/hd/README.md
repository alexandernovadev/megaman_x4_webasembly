# HD con IA (8x)

Cómo funciona:

1. **Registro.** El juego anota cada primitiva que dibuja en una lista por búfer de pantalla
   (`runtime/hd.c`). Cada pieza de textura se reconoce por la huella de sus índices de color.
2. **Redibujado.** La página (`web/hd.js`) vuelve a dibujar el frame con la GPU a la resolución
   de la pantalla, con antialiasing. Cada pieza que tiene versión HD se dibuja con ella, y las
   demás con la textura original.
3. **Piezas nuevas.** Las piezas sin versión HD se capturan junto con sus vecinas en pantalla
   y se mandan al servidor local (`tools/serve.py`, carpeta `hd_queue/`). El servidor arranca
   `upscale.py --watch`, que las convierte mientras se juega.
4. **Conversión.** `upscale.py` escala 4x con la IA (4x-Fatal-Pixels), refina a 8x con
   bicúbico, recorta la pieza y la ajusta a los índices de la paleta original. Así la versión
   HD sigue los destellos y cambios de color del juego. El resultado se guarda en
   `hd_pack/<huella>.x4h`.

Para jugar con HD, abre `http://localhost:8765` con `python3 tools/serve.py 8765` en marcha.
Con `?sin-hd` en la URL se juega sin HD (sirve para pruebas).

Para precargar piezas sin navegador:

- Captura nativa: `X4_HDCAPTURE=dir X4_WIDE=1 ./build/x4 disc <frames> <bin> <segundos>`.
  Sin mando, recorre el modo demo de la pantalla de título. Con `X4_INPUT` se puede jugar
  con un guion de entrada.
- Conversión: `~/tools/hd-ai/venv/bin/python -I tools/hd/upscale.py hd_pack dir/*.bin`.
- Vista previa sin GPU: `X4_HDLIST=dir:frame` guarda la lista de dibujo de ese frame. Luego
  `python tools/hd/preview.py hd_pack dir/hdlist_N.bin salida.png 8`.

`compare.py` es la comparación de modelos que se usó para elegir la IA.

Las herramientas están en `~/tools/hd-ai`, fuera del proyecto:

- un venv con torch, spandrel y pillow;
- los modelos `.pth` de OpenModelDB (4x-Fatal-Pixels, 4x-NXbrz, 4x-Rebout y
  4x-Rebout-Blend; licencia CC-BY-NC);
- realesrgan-ncnn-vulkan v0.2.5.0 (xinntao, BSD-3).

Todo lo generado sale del disco del usuario (`hd_pack/`, `hd_queue/`, `hd_preview/`). Es de
uso local y no se distribuye.
