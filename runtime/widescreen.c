// Panorámica 16:9: el juego dibuja 426x240 en vez de 320x240 (53 píxeles más por lado).
// Las rutinas y estructuras del juego que se tocan están documentadas en el análisis de
// MegaManX4Recomp (MIT); la implementación es propia. Con ws_reveal = 0 todo es idéntico al
// original. Ver también los parches de tools/recomp.py y el lienzo ancho de gpu.c.
//
// Fondos: tres capas, cada una con su cámara en 0x801419B0 + capa*0x54 (x en +10, y en +14)
// y un anillo de 32x32 tiles en 0x801441C8 + capa*0x800. El juego solo carga en el anillo
// las columnas de su ventana de 320 píxeles; aquí se cargan además las de los márgenes.
#include "runtime.h"

#define REVEAL 53  // (426 - 320) / 2

int ws_reveal, ws_cols;

void ws_set(int on) {
    ws_reveal = on ? REVEAL : 0;
    ws_cols = (ws_reveal + 15) / 16;
}

#define LAYERS 0x801419B0u
#define RING 0x801441C8u

// Copia de la lógica de la rutina de columnas del juego (0x80027344) para un tile, sin su
// efecto secundario de mover la cámara cuando la columna sale del tramo de pantallas.
static void load_tile(int layer, int x, int y) {
    uint32_t rec = LAYERS + layer * 0x54;
    uint32_t map = RD32(0x1F800004), tiles = RD32(0x1F800008);
    int width = RD8(0x80172224), stride = RD16(0x8013BD48);
    int first = RD8(rec + 0x4D), last = RD8(rec + 0x4E);
    if (!width || first > last || y < 0) return;
    int sx = x >> 8, sy = y >> 8;  // x puede ser negativa al principio del mapa
    uint32_t slot = RING + (layer << 11) + ((y & 511) >> 4) * 64 + ((x & 511) >> 4) * 2;
    // Fuera del tramo de pantallas de la capa: los fondos se repiten (como hace el juego);
    // el primer plano queda vacío (tile 0), así se ve el fondo o negro, igual a ambos lados.
    if (sx < first || sx > last) {
        if (layer == 0) {
            WR16(slot, 0);
            return;
        } else {
            int period = last - first + 1;
            sx = first + ((sx - first) % period + period) % period;
        }
    }
    uint32_t screen = RD8(map + layer * stride + width * sy + sx);
    uint16_t t = RD16(tiles + screen * 512 + ((y >> 4) & 15) * 32 + ((x >> 4) & 15) * 2);
    WR16(slot, t);
}

// Antes de dibujar la capa a0: rellena las columnas de los márgenes. También las de los
// bordes camX-16 / camX+336: el juego las carga al desplazarse, pero no al empezar un nivel.
int ws_hook_bg_layer(CPU *c, uint32_t addr) {
    (void)addr;
    int layer = (int)c->r[4];
    if (!ws_cols || layer < 0 || layer > 2) return 0;
    uint32_t rec = LAYERS + layer * 0x54;
    int camx = (int16_t)RD16(rec + 10), camy = (int16_t)RD16(rec + 14);
    for (int k = 0; k <= ws_cols; k++) {
        int left = (camx - 16) - k * 16, right = (camx + 336) + k * 16;
        for (int y = (camy - 16) & ~15; y <= camy + 256; y += 16) {
            load_tile(layer, left, y);
            load_tile(layer, right, y);
        }
    }
    return 0;
}

// Ventanas de actividad/visibilidad de actores (0x8002B1E8 y 0x8002B318): a1 es el margen
// horizontal. Los actores del mundo (byte +0x14 >= 0) lo amplían con los píxeles extra; los
// elementos fijos en pantalla no.
int ws_hook_actor_bounds(CPU *c, uint32_t addr) {
    (void)addr;
    if (ws_reveal && (int8_t)RD8(c->r[4] + 0x14) >= 0) c->r[5] += (uint32_t)ws_reveal;
    return 0;
}

// Variantes con márgenes fijos: equivalen a las de parámetro con (64,64), (32,32) y (96,80).
// En panorámica se redirigen a ellas para que también se amplíen.
int ws_hook_fixed_window(CPU *c, uint32_t addr) {
    if (!ws_reveal || (int8_t)RD8(c->r[4] + 0x14) < 0) return 0;
    if (addr == 0x8002b160u) { c->r[5] = 64; c->r[6] = 64; dispatch(c, 0x8002b1e8u); return 1; }
    c->r[5] = addr == 0x8002b288u ? 32 : 96;
    c->r[6] = addr == 0x8002b288u ? 32 : 80;
    dispatch(c, 0x8002b318u);
    return 1;
}
