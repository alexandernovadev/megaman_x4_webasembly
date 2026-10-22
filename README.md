# Mega Man X4 · WebAssembly

*Mega Man X4* (PlayStation, versión USA) recompilado de MIPS a C y a WebAssembly, para jugarlo en
el navegador desde tu propio disco. Incluye HD con IA a 8x, panorámica 16:9, trucos con un modo
"Completo" y una prueba de voces en español.

```bash
make web
python3 tools/serve.py 8765   # y abre http://localhost:8765
```

Necesitas tu copia de `Mega Man X4 (USA).bin`. Se lee en tu navegador y no se sube a ningún sitio.
Este repositorio no contiene nada del juego.

## Documentación

- [docs/01-viabilidad.md](docs/01-viabilidad.md): estudio inicial del disco y del ejecutable.
- [docs/02-proyecto.md](docs/02-proyecto.md): cómo está hecho todo (recompilador, runtime, web,
  HD, trucos, voces), cómo jugar y cómo probar.
- [tools/hd/README.md](tools/hd/README.md): el HD con IA paso a paso.
- [tools/voice/README.md](tools/voice/README.md): las voces en español.
- [web/THIRD_PARTY.md](web/THIRD_PARTY.md): código y datos de terceros.
