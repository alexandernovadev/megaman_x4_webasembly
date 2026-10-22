// Voces: avisa a la plataforma de qué caja de diálogo está en pantalla, para que ponga su audio y
// su subtítulo (paquete local voice_pack/, ver tools/voice/).
//
// El texto del juego usa 16 bits por carácter; el nibble alto marca salto de línea (5), fin de
// caja (2, espera el botón) y fin de mensaje (A). Mientras hay un diálogo, 0x8013BC28 apunta al
// siguiente carácter y 0x8013BC30 dice en qué está la caja: 0x1D abriéndose, 0x2F escribiéndose,
// 0x41 esperando el botón (y entonces el puntero ya está en la caja siguiente).
#include "runtime.h"

#define TEXT_PTR 0x8013bc28u
#define TEXT_STATE 0x8013bc30u
#define MAX_BOX 400  // caracteres

static int box_end(uint32_t w) { return (w >> 12) == 0x2 || (w >> 12) == 0xa; }

// Huella de la caja que empieza en `s` (hasta su marca de fin, incluida): FNV-1a de 32 bits.
// tools/voice/make_voices.py calcula la misma a partir del disco.
static uint32_t box_key(uint32_t s) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < MAX_BOX; i++) {
        uint32_t w = RD16(s + 2u * i);
        h = (h ^ (w & 0xff)) * 16777619u;
        h = (h ^ (w >> 8)) * 16777619u;
        if (box_end(w)) break;
    }
    return h ? h : 1;
}

// Caja en pantalla: 0 si no hay diálogo; si no cambia, la misma de antes.
uint32_t voice_frame(void) {
    static uint32_t key, start;
    uint32_t p = RD32(TEXT_PTR), st = RD16(TEXT_STATE);
    if (!p) return key = start = 0;
    if (st != 0x1d && st != 0x2f) return key;
    // Abriéndose, el puntero está en el primer carácter de la caja o justo después; escribiéndose,
    // ya ha pasado al menos uno (y puede estar justo detrás de la marca de fin).
    uint32_t s = st == 0x1d ? p : p - 2;
    for (int i = 0; i < MAX_BOX && !box_end(RD16(s - 2)); i++) s -= 2;
    if (s != start) { start = s; key = box_key(s); }
    return key;
}
