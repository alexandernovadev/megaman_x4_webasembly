#pragma once
#include <stdint.h>

// Lista de dibujo para el HD (ver hd.c). Un vértice ocupa 32 bytes:
//   x, y: píxeles relativos al búfer de pantalla; u, v: texels en la página
//   col: RGB | banderas << 24 (1 textura, 2 sin modular, 4 semitransparente, modo << 3)
//   tex: página x/64 | página y/256 << 4 | profundidad << 5 | paleta << 7 (fila de la tabla de
//        paletas del frame, con los colores que había al dibujar)
//   hd:  id de la pieza (0 = ninguna) | u0 << 16 | v0 << 24 (origen de su región)
//   twin: ventana de textura (E2, 20 bits) | (ancho-1) << 20 | (alto-1) << 24 de la región
typedef struct { float x, y, u, v; uint32_t col, tex, hd, twin; } HdVert;
// Tramo de vértices con la misma mezcla (0 opaco, 1 + modo semitransparente) y recorte.
typedef struct { uint32_t first, count; int32_t blend, sx0, sy0, sx1, sy1; } HdBatch;
typedef struct { const HdVert *v; int nv; const HdBatch *b; int nb; const uint16_t *vram; const uint16_t *pal; int np; } HdOut;

typedef struct {
    int n;  // 3 o 4 vértices (orden de la PS1: el cuadrilátero son 0-1-2 y 1-2-3)
    int x[4], y[4], u[4], v[4];
    uint32_t rgb[4];
    int textured, raw, semi, mode, depth, tpx, tpy, cx, cy;
    uint32_t twin;
    int clip_x0, clip_y0, clip_x1, clip_y1;  // área de dibujo (inclusive), con márgenes
    int da_x, da_y, da_w, da_h;              // búfer: origen y tamaño del área de dibujo
} HdPrim;

void hd_prim(const HdPrim *p);
void hd_fill(int x, int y, int w, int h, uint32_t rgb);
void hd_taint(int x, int y, int w, int h);
void hd_display(int x, int y);
int hd_frame(int disp_x, int disp_y, HdOut *out);
void hd_known(const uint32_t *pairs, int n);
uint64_t hd_id_hash(int id);
int hd_id_count(void);
extern uint32_t hd_new_ids[];  // por cada id nuevo: id, huella baja, huella alta
extern uint8_t hd_new_px[];    // y sus texels (16x16 índices)
extern int hd_new_n, hd_capture_on;
void hd_emit_captures(const uint8_t *buf, int len);  // la aporta la plataforma
