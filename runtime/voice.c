// Voces: avisa a la plataforma de qué caja de diálogo está en pantalla, para que ponga su audio y
// su subtítulo (paquete local voice_pack/, ver tools/voice/).
//
// El texto del juego usa 16 bits por carácter; el nibble alto marca salto de línea (5), fin de
// caja (2, espera el botón) y fin de mensaje (A). Mientras hay un diálogo, 0x8013BC28 apunta al
// siguiente carácter y 0x8013BC30 dice en qué está la caja: 0x1D abriéndose, 0x2F escribiéndose,
// 0x41 esperando el botón (y entonces el puntero ya está en la caja siguiente). 0x8013BC35 es la
// fase: 0 abriéndose, 1-2 escribiéndose, 3 esperando, 4 cerrándose (la caja aún se ve) y 5
// cerrado; al cerrar, el puntero no siempre vuelve a 0.
//
// Los mensajes cuya caja sale abajo de la pantalla llevan el bit 0x0800 en cada carácter.
#include "runtime.h"

#define TEXT_PTR 0x8013bc28u
#define TEXT_STATE 0x8013bc30u
#define TEXT_PHASE 0x8013bc35u
#define BOX_BOTTOM 0x0800u
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

static int bottom;
static uint32_t cur_key;

// Cajas que tienen voz (las dice la plataforma): en ellas no suena el pitido de las letras.
#define MAX_DUB 4096
static uint32_t dubbed[MAX_DUB];
static int n_dubbed;

void voice_dub(uint32_t key) {
    if (n_dubbed < MAX_DUB) dubbed[n_dubbed++] = key;
}

static int is_dubbed(uint32_t key) {
    for (int i = 0; i < n_dubbed; i++)
        if (dubbed[i] == key) return 1;
    return 0;
}

// Caja en pantalla: 0 si no hay diálogo; si no cambia, la misma de antes.
uint32_t voice_frame(void) {
    static uint32_t key, start;
    uint32_t p = RD32(TEXT_PTR), st = RD16(TEXT_STATE);
    if (!p || RD8(TEXT_PHASE) >= 5) return cur_key = key = start = 0;
    if (st != 0x1d && st != 0x2f) return key;
    // Abriéndose, el puntero está en el primer carácter de la caja o justo después; escribiéndose,
    // ya ha pasado al menos uno (y puede estar justo detrás de la marca de fin).
    uint32_t s = st == 0x1d ? p : p - 2;
    for (int i = 0; i < MAX_BOX && !box_end(RD16(s - 2)); i++) s -= 2;
    if (s != start) { start = s; key = box_key(s); bottom = (RD16(s) & BOX_BOTTOM) != 0; }
    return cur_key = key;
}

// Efecto de sonido (0x8001540C, r4 = canal, r5 = sonido). La rutina que escribe el texto
// (0x8002328C) pide el pitido de cada letra con la vuelta en 0x800234C8; si la caja tiene voz,
// se salta (no usa el valor que devuelve).
int voice_hook_sound(CPU *c, uint32_t addr) {
    (void)addr;
    if (c->r[31] != 0x800234c8u || !n_dubbed) return 0;
    return is_dubbed(voice_frame());
}

// La caja de voice_frame sale abajo de la pantalla.
int voice_bottom(void) { return bottom; }
