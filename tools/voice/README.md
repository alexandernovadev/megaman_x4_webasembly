# Voces en español

1. **Detección.** `runtime/voice.c` mira qué caja de diálogo está en pantalla y manda a la página
   la huella de su texto original.
2. **Guion.** `voice_lines/*.json` dice qué mensajes del disco se doblan: archivo, tabla, número de
   mensaje y, por cada caja, el subtítulo en español (`es`) y lo que se dice (`say`), además de la
   voz de cada personaje. Una caja vacía `{}` continúa la frase de la anterior.
3. **Generación.** `~/tools/voice/venv/bin/python -I tools/voice/make_voices.py voice_lines/poc.json voice_pack`
   crea `voice_pack/<huella>.wav` con Piper y `voice_pack/index.json`.
4. **Juego.** `tools/serve.py` sirve `voice_pack/` en `/voice/`. La página pone el audio y tapa la
   caja en inglés con el subtítulo.

Herramientas fuera del proyecto, en `~/tools/voice`: un venv con `piper-tts` y las voces de
`rhasspy/piper-voices` en `models/` (es_MX-claude-high mujer, es_MX-ald-medium hombre,
es_ES-davefx-medium hombre grave, es_ES-sharvard-medium M/F, es_AR-daniela-high mujer).

Mensajes conocidos: `PL00_U.ARC` tabla 0xBD000 (partida con X, 54 mensajes) y `PL01_U.ARC` tabla
0xB1000 (Zero, 48). La prueba de concepto dobla solo la primera conversación: X #30 y Zero #25.

El guion y los audios salen del texto del disco del usuario: son de uso local y no se distribuyen
(`voice_lines/` y `voice_pack/` están en `.gitignore`).
