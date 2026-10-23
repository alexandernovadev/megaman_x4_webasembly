# Mega Man X4 · WebAssembly

*Mega Man X4* (PlayStation, versión USA) recompilado de MIPS a C y a WebAssembly, para jugarlo en
el navegador desde tu propio disco. Incluye HD con IA a 8x, panorámica 16:9, trucos con un modo
"Completo" y una prueba de voces en español.

## Jugar en un comando

```bash
./jugar.sh "ruta/a/Mega Man X4 (USA).bin"   # la primera vez
./jugar.sh                                  # las siguientes
```

Pone tu disco en `disco/`, lo extrae, compila la versión web (la primera vez tarda unos minutos),
arranca el servidor y abre el navegador: pulsa **Iniciar juego** y listo.

Necesitas tu propia copia de `Mega Man X4 (USA).bin`, `python3`, un compilador de C (en macOS,
`xcode-select --install`) y [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
(el script te dice cómo instalarlo si falta). Este repositorio no contiene nada del juego, y tu
disco no sale de tu equipo.

A mano: `make web` y `python3 tools/serve.py 8765`, y en http://localhost:8765 eliges el disco.

## Documentación

- [docs/01-viabilidad.md](docs/01-viabilidad.md): estudio inicial del disco y del ejecutable.
- [docs/02-proyecto.md](docs/02-proyecto.md): cómo está hecho todo (recompilador, runtime, web,
  HD, trucos, voces), cómo jugar y cómo probar.
- [tools/hd/README.md](tools/hd/README.md): el HD con IA paso a paso.
- [tools/voice/README.md](tools/voice/README.md): las voces en español.
- [web/THIRD_PARTY.md](web/THIRD_PARTY.md): código y datos de terceros.
