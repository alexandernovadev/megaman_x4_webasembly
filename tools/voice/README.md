# Voces en español

1. **Detección.** `runtime/voice.c` mira qué caja de diálogo está en pantalla y manda a la página
   la huella de su texto original.
2. **Grabación.** La página `/grabar` (`web/grabar.html`) muestra unos textos por personaje, cada
   uno con un consejo de cómo decirlo (fuerte, suave…), y guarda cada toma en WAV en `voice_refs/`
   (`x_1.wav`, `zero_3.wav`…). Son las voces que se imitan.
3. **Guion.** `voice_lines/*.json` dice qué mensajes del disco se doblan: archivo, tabla, número de
   mensaje y, por cada caja, el subtítulo en español (`es`), lo que se dice (`say`, con los nombres
   en inglés escritos como suenan) y, si hace falta, qué toma imitar (`ref`). Una caja vacía `{}`
   continúa la frase de la anterior. En `voices`, cada personaje lleva sus tomas y sus efectos.
4. **Generación.** `~/tools/voice/cb/bin/python -I tools/voice/make_voices.py voice_lines/intro.json voice_pack`
   limpia las tomas (`voice_refs/clean/`: sin silencios, sin ruido, mismo volumen), imita cada voz
   con Chatterbox, le pone los efectos (tono, radio, coro, distorsión, eco y un realce para que
   suene nítida) y crea `voice_pack/<huella>.wav` y `voice_pack/index.json`. Añadiendo nombres de
   personaje al final (`… voice_pack ZERO`) solo se rehacen esos.
5. **Juego.** `tools/serve.py` sirve `voice_pack/` en `/voice/`. La página pone el audio y tapa la
   caja en inglés con el subtítulo.

Herramientas fuera del proyecto, en `~/tools/voice`:

- `cb/`: Python 3.12 con `chatterbox-tts` 0.1.7 (Resemble AI, MIT; el modelo se descarga solo la
  primera vez), `pedalboard` (efectos), `noisereduce` y `setuptools<81` (Chatterbox necesita su
  `pkg_resources`). Ojo: `numpy` tiene que quedarse en 1.x.
- `venv/` y `models/`: Piper, la primera prueba (voces sintéticas de `rhasspy/piper-voices`); sigue
  funcionando con `"engine": "piper"`.

Chatterbox a veces se inventa un trozo al final en frases muy cortas: si una dura de más, se
vuelve a generar.

Mensajes conocidos: `PL00_U.ARC` tabla 0xBD000 (partida con X, 54 mensajes) y `PL01_U.ARC` tabla
0xB1000 (Zero, 48). Doblado: el primer nivel entero. X: #30 (Cuartel), #46 (Dragoon), #47, #52 y
#31 (Coronel). Zero: #25 (Cuartel), #41 (Iris), #39 (Dragoon), #40, #47 y #26 (Coronel).

El guion y los audios salen del texto del disco del usuario, y las tomas son la voz de quien
graba: todo es de uso local y no se distribuye (`voice_lines/`, `voice_pack/` y `voice_refs/`
están en `.gitignore`).
