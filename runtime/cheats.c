// Modo "Completo": en la selección de personaje, X o Zero empiezan la partida como si ya
// hubieran recogido todo: las 8 armas, los 8 corazones, los 4 tanques llenos, la armadura entera
// (X) y la versión especial del personaje (Ultimate Armor / Zero Negro).
//
// Estado de la partida (0x801721C0, se guarda en la tarjeta):
//   2203 personaje (0 = X, 1 = Zero)       2206 vida máxima (32 + 2 por corazón)
//   2207 partes de armadura (bits, 0x0F)   2208 tipo de disparo del brazo (2 = plasma)
//   2219 armas de los jefes (bits)         221A corazones (bits 0-7) y tanques (bits 12-15)
//   221C/221D Sub Tanks, 221E Weapon Tank (carga, máx. 32)   21F7 versión especial
// Al empezar cada nivel, 0x80035240 copia ese estado al jugador (0x801418C8).
//
// Las armas no se pueden dar en 2219: para el juego, tener el arma de un jefe es haberlo
// vencido (no habría jefes y se abriría el final). Así que 2219 sigue contando los jefes de
// verdad y la partida Completo lleva una marca propia, FULL_MARK, en un bit libre de 221A (se
// guarda en la tarjeta con el resto); con ella el jugador y el menú de pausa ven las 8 armas
// (parches en tools/recomp.py).
#include "runtime.h"

int full_mode;            // la plataforma lo cambia con x4_full_toggle (tecla Tab en la web)
static int full_pending;  // se aplica al empezar el primer nivel tras elegir personaje
static long select_seen = -100;
static int select_char;
static int special_before;  // versión especial antes de activar el modo (por si se puso con el código)

#define GS 0x80170000u
#define FULL_MARK 0x0100u

// Rutinas de la selección de personaje que buscan los códigos de X (0x80029A48) y Zero
// (0x80029BD8): corren cada frame con el cursor sobre ese personaje.
int cheat_hook_select(CPU *c, uint32_t addr) {
    (void)c;
    select_seen = frame_count;
    select_char = addr == 0x80029bd8u;
    // La versión especial decide qué gráficos carga el nivel: tiene que estar puesta antes.
    if (full_mode) { full_pending = 1; WR8(GS + 0x21f7, 1); }
    return 0;
}

// Inicio del jugador al empezar un nivel.
int cheat_hook_player(CPU *c, uint32_t addr) {
    (void)c; (void)addr;
    if (!full_pending) return 0;
    full_pending = 0;
    if (!full_mode) return 0;
    WR8(GS + 0x2206, 32 + 2 * 8);
    WR16(GS + 0x221a, 0xf0ffu | FULL_MARK);
    WR8(GS + 0x221c, 32);
    WR8(GS + 0x221d, 32);
    WR8(GS + 0x221e, 32);
    WR8(GS + 0x21f7, 1);
    if (RD8(GS + 0x2203) == 0) {
        WR8(GS + 0x2207, 0x0f);
        WR8(GS + 0x2208, 2);
    }
    return 0;
}

uint32_t cheat_weapons(uint32_t defeated) {
    return RD16(GS + 0x221a) & FULL_MARK ? 0xff : defeated;
}

// Para la página: bit 0 = en la selección, bit 1 = modo completo, bit 2 = cursor en Zero.
int cheat_state(void) {
    int on_select = frame_count - select_seen < 10;
    return on_select | full_mode << 1 | select_char << 2;
}

// Solo cambia mientras se está en la selección de personaje.
int cheat_full_toggle(void) {
    if (frame_count - select_seen < 10) {
        full_mode = !full_mode;
        if (full_mode) special_before = RD8(GS + 0x21f7);
        else WR8(GS + 0x21f7, special_before);
    }
    return cheat_state();
}
