# Etapa 1 — Viabilidad del port de Mega Man X4 (PS1) a WebAssembly

Fecha: 2026-10-20 · Fuente: `Mega Man X4 (USA).bin` (SHA-256 `3ceab06a…cb613b`), copia propia del usuario.
Herramientas: `tools/iso_extract.py`, `tools/psx2elf.py`, REA + Ghidra 12.1.4.

## Disco

- Una sola pista de datos MODE2/2352, ISO9660, volumen `MEGAMAN_X4`, fecha 1997-08-20.
- Arranque (`SYSTEM.CNF`): `BOOT = cdrom:\SLUS_005.61;1`, `TCB=4`, `EVENT=16`, `STACK=801FFF00`.
- `/ARC/*.ARC` (138 archivos, 25 MB): datos del juego (niveles `ST*`, colisiones `COL*`, personajes `PL*`, fuentes, carga).
- `/STR/*.STR`: videos FMV (MDEC). Intro, rutas de X (`X1-X4`) y de Zero (`Z1-Z5`).
- `/XA/*.XA`: música (`BGM*`) y voces (`VOICE*`) en ADPCM XA.
- `ZNULL.DAT`: relleno, unos 37 MB.

## Ejecutable `SLUS_005.61`

| Campo | Valor |
|---|---|
| Formato | PS-X EXE ("North America area") |
| Carga | `0x80010000`, `0x11F800` bytes |
| Entrada | `0x800DAE8C` (crt0 de Psy-Q) |
| Pila | `0x801FFFF0` |
| `main()` | `0x80012024` |
| Rutas de desarrollo | `E:\ROCKX4\USA\ARC\...` |

Para REA/Ghidra se convierte a ELF32 MIPS LE con `e_flags = EF_MIPS_ARCH_32` (sin ABI). Es el único
encabezado que admite el carril MIPS de REA. Asumimos la ABI o32.

### Mapa de código (Ghidra, 4.835 funciones)

| Rango | Funciones | Contenido |
|---|---|---|
| `0x80012024` – `0x800DAE84` | 4.446 | Código propio del juego (Capcom) |
| `0x800DAE84` – `0x800EE3A4+` | 389 | crt0 y librerías Psy-Q de Sony |
| `0x800EE3A4` – `0x8012F800` | — | Datos |

Librerías identificadas por sus textos de depuración, con la función que los usa:

- libgpu: `ResetGraph` → `0x800e9e84`, `DrawSync`, `GPU timeout`
- libetc: `VSync: timeout` → `0x800e4f44`
- libcd: `CdInit` → `0x800e5b38`, `CdRead`
- libspu/libsnd: `SPU:T/O` → `0x800db180`, `_SsContResetAll`
- libpress (MDEC, videos): `MDEC_in_sync` → `0x800ed31c`
- libcard: `bu00:BASLUS-00561` (partidas guardadas)
- 51 llamadas directas a la BIOS (A0/B0/C0)

### Bucle principal (`0x80012024`, decompilado)

Bucle infinito con doble búfer. El índice de búfer vive en la memoria rápida (scratchpad) `0x1F800000`,
los entornos de dibujo y pantalla en `0x80166C10 + idx*0xA0` y hay una tabla de orden (OT) de 12 entradas.
Por cada frame: sincroniza la GPU y envía los entornos y la OT (funciones de libgpu en `0x800EAxxx`),
ejecuta unos 10 subsistemas del juego (`0x800168d8`, `0x80012328`, `0x80014780`…), espera a `VSync`
(`0x800ea20c`) y repite.

## Riesgos evaluados

| Riesgo | Resultado |
|---|---|
| Código cargado desde el CD (overlays) | **No hay indicios.** Ningún `.ARC` contiene patrones de función MIPS (`jr ra` = 0, prólogos = 0) y no hay llamadas `jal` coherentes fuera del ejecutable. Pendiente: confirmar que no haya saltos indirectos a la RAM cargada desde los `.ARC`. |
| Uso del GTE (3D) | **Bajo:** 259 instrucciones COP2 y 41 `lwc2`/`swc2` en 1,15 MB. Es un juego 2D, así que el GTE se usa poco. |
| Tamaño del código | 4.446 funciones del juego. Hay que recompilarlas de forma automática; a mano sería inviable. |
| Formato `.ARC` | Desconocido. Entropía de 4 a 6,5 bits/byte, así que hay datos comprimidos o empaquetados. Hay que investigarlo. |

## Conclusión

**Viable.** Todo el código está en un solo ejecutable sin overlays aparentes. Las librerías de Sony están
agrupadas en un bloque separado (389 funciones), así que se pueden sustituir por implementaciones propias
(WebGL/WebAudio). Y como el juego casi no usa el GTE, se reduce el riesgo más caro.

Estrategia propuesta:
1. Recompilación estática MIPS→C del rango del juego (`0x80012024`–`0x800DAE84`).
2. Sustitución de las funciones Psy-Q por implementaciones de alto nivel (HLE) en C/JS.
3. GPU por software o WebGL sobre una VRAM emulada de 1024×512.

## Siguientes pasos

- [ ] Identificar con precisión cada función Psy-Q (firmas de librería).
- [ ] Descifrar el formato `.ARC` (cabecera y compresión).
- [ ] Confirmar que no haya overlays por salto indirecto (tablas de punteros a funciones).
- [ ] Prototipo de recompilador: `entry` → `main` llega al primer `VSync`.
