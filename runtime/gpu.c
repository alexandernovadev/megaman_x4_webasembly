// GPU de la PS1 por software: comandos GP0/GP1 sobre una VRAM de 1024x512 a 16 bits.
// Referencia: psx-spx, "GPU". Prioriza lo que usa un juego 2D: sprites, polígonos
// texturizados con CLUT, rellenos, transparencias y transferencias de VRAM.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "hd.h"

#define LOG(...) do { if (log_level) fprintf(stderr, __VA_ARGS__); } while (0)

uint16_t vram[512 * 1024];

// Color completo ("true color"): junto a cada píxel dibujado se guarda su color a 24 bits y
// el valor de 15 bits que produjo. Al mostrar la imagen se usa el de 24 bits si la VRAM no
// ha cambiado por otra vía (transferencias, vídeo...). La VRAM sigue siendo exacta.
static uint32_t vram24[512 * 1024];  // R | G << 8 | B << 16
static uint16_t vram24_tag[512 * 1024];
static inline int x5to8(int c) { return c << 3 | c >> 2; }


static inline uint32_t c15to24(uint16_t c) {
    return (uint32_t)x5to8(c & 31) | (uint32_t)x5to8((c >> 5) & 31) << 8 | (uint32_t)x5to8((c >> 10) & 31) << 16;
}

// ---- estado de dibujo
static int tp_x, tp_y, tp_semi, tp_depth, tex_disable;  // página de textura (E1)
static int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;    // ventana de textura (E2)
static int da_x1, da_y1, da_x2 = 1023, da_y2 = 511;     // área de dibujo (E3/E4)
static int off_x, off_y;                                // desplazamiento (E5)
static int mask_set, mask_check;                        // E6
static int dither;

// ---- estado de pantalla (GP1)
int disp_x, disp_y, disp_w = 320, disp_h = 240, disp_24bit, disp_enabled;
static int dma_dir, gpu_irq;

// ---- FIFO de comandos
static uint32_t fifo[64];
static int fifo_n, fifo_need;
static int polyline;

// ---- polilíneas (GP0 48h/58h): se dibujan segmento a segmento
typedef struct { int x, y, r, g, b, u, v; } Vtx;
static int poly_gour, poly_semi, poly_have_prev, poly_expect_color;
static uint32_t poly_color;
static Vtx poly_prev;
static void draw_line(Vtx a, Vtx b, int gouraud, int semi);

// ---- transferencias CPU<->VRAM
static int xfer_mode;  // 0 nada, 1 CPU->VRAM, 2 VRAM->CPU
static int xfer_x, xfer_y, xfer_w, xfer_h, xfer_i;

// Panorámica: lienzos de (320 + 2*ws_reveal) x 240, uno por búfer de pantalla del juego
// (identificado por el origen de su área de dibujo). El centro es copia de la VRAM; los
// márgenes solo existen aquí. Ver widescreen.c.
#define WS_MAX 64
#define WS_W (320 + 2 * WS_MAX)
typedef struct {
    int ox, oy, used;
    int displayed;     // ya se mostró: el próximo dibujo en él empieza un frame nuevo
    int margin_drawn;  // se dibujó algo en los márgenes en este frame
    uint32_t px[240][WS_W];
} Wide;
static Wide wide[2];
static Wide *cur_wide;  // lienzo del área de dibujo actual (NULL si no es 320x240 o sin panorámica)
uint32_t gpu_pkt_addr;  // dirección del paquete GP0 en curso (DMA), para recolocar el HUD

static void select_wide(void) {
    cur_wide = NULL;
    if (!ws_reveal || da_x2 - da_x1 != 319 || da_y2 - da_y1 != 239) return;
    Wide *w = NULL;
    for (int i = 0; i < 2; i++) if (wide[i].used && wide[i].ox == da_x1 && wide[i].oy == da_y1) w = &wide[i];
    if (!w) {
        static int next;
        w = &wide[next ^= 1];
        memset(w, 0, sizeof *w);
        w->ox = da_x1; w->oy = da_y1; w->used = 1;
    }
    if (w->displayed) {  // frame nuevo: márgenes en negro
        for (int y = 0; y < 240; y++) memset(w->px[y], 0, sizeof w->px[y]);
        w->displayed = w->margin_drawn = 0;
    }
    cur_wide = w;
}

static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

// Campos comunes de una primitiva para la lista de dibujo HD (ver hd.c).
static void hd_begin(HdPrim *p) {
    int extra = cur_wide ? ws_reveal : 0;
    p->clip_x0 = da_x1 - extra; p->clip_x1 = da_x2 + extra;
    p->clip_y0 = da_y1; p->clip_y1 = da_y2;
    p->da_x = da_x1; p->da_y = da_y1; p->da_w = da_x2 - da_x1 + 1; p->da_h = da_y2 - da_y1 + 1;
    p->twin = (uint32_t)(tw_mask_x | tw_mask_y << 5 | tw_off_x << 10 | tw_off_y << 15);
}

// ------------------------------------------------------------ píxeles
static inline uint32_t blend24(uint32_t back, uint32_t c24, int mode) {
    int out[3];
    for (int k = 0; k < 3; k++) {
        int b8 = (back >> (k * 8)) & 255, f8 = (c24 >> (k * 8)) & 255, v;
        switch (mode) {
            case 0: v = (b8 + f8) >> 1; break;
            case 1: v = b8 + f8; break;
            case 2: v = b8 - f8; break;
            default: v = b8 + (f8 >> 2); break;
        }
        out[k] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
    return (uint32_t)out[0] | (uint32_t)out[1] << 8 | (uint32_t)out[2] << 16;
}

static inline void plot(int x, int y, uint16_t col, uint32_t c24, int semi, int mode) {
    if (y < da_y1 || y > da_y2) return;
    if (x < da_x1 || x > da_x2) {
        // Fuera del área: solo existe en el lienzo panorámico.
        if (!cur_wide) return;
        int wx = x - da_x1 + ws_reveal;
        if (wx < 0 || wx >= 320 + 2 * ws_reveal) return;
        uint32_t *w = &cur_wide->px[y - da_y1][wx];
        *w = semi ? blend24(*w, c24, mode) : c24;
        cur_wide->margin_drawn = 1;
        return;
    }
    int i = (y & 511) * 1024 + (x & 1023);
    uint16_t *p = &vram[i];
    if (mask_check && (*p & 0x8000)) return;
    if (semi) {
        // Misma mezcla a 8 bits por canal sobre el color de 24 bits del fondo.
        c24 = blend24(vram24_tag[i] == *p ? vram24[i] : c15to24(*p), c24, mode);
        int br = *p & 31, bg = (*p >> 5) & 31, bb = (*p >> 10) & 31;
        int fr = col & 31, fg = (col >> 5) & 31, fb = (col >> 10) & 31;
        int r, g, b;
        switch (mode) {
            case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
            case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
            case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
            default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
        }
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 31 ? 31 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
        col = (col & 0x8000) | r | g << 5 | b << 10;
    }
    *p = col | (mask_set ? 0x8000 : 0);
    vram24[i] = c24;
    vram24_tag[i] = *p;
    if (cur_wide) cur_wide->px[y - da_y1][x - da_x1 + ws_reveal] = c24;
}

static inline uint32_t modulate24(uint16_t t, int r, int g, int b) {
    int c[3] = {r, g, b}, out[3];
    for (int k = 0; k < 3; k++) {
        int v = x5to8((t >> (k * 5)) & 31) * c[k] >> 7;
        out[k] = v > 255 ? 255 : v;
    }
    return (uint32_t)out[0] | (uint32_t)out[1] << 8 | (uint32_t)out[2] << 16;
}
static inline uint32_t rgb24(int r, int g, int b) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16; }

// Lee un texel; devuelve 0 si es transparente (valor 0x0000).
static inline uint16_t texel(int u, int v, int tpx, int tpy, int depth, int clut_x, int clut_y) {
    u = (u & ~(tw_mask_x * 8)) | ((tw_off_x & tw_mask_x) * 8);
    v = (v & ~(tw_mask_y * 8)) | ((tw_off_y & tw_mask_y) * 8);
    u &= 255;
    v &= 255;
    int y = (tpy + v) & 511;
    switch (depth) {
        case 0: {
            uint16_t w = vram[y * 1024 + ((tpx + u / 4) & 1023)];
            int idx = (w >> ((u & 3) * 4)) & 15;
            return vram[clut_y * 1024 + ((clut_x + idx) & 1023)];
        }
        case 1: {
            uint16_t w = vram[y * 1024 + ((tpx + u / 2) & 1023)];
            int idx = (w >> ((u & 1) * 8)) & 255;
            return vram[clut_y * 1024 + ((clut_x + idx) & 1023)];
        }
        default:
            return vram[y * 1024 + ((tpx + u) & 1023)];
    }
}

static inline uint16_t modulate(uint16_t t, int r, int g, int b) {
    int tr = (t & 31) * r >> 7, tg = ((t >> 5) & 31) * g >> 7, tb = ((t >> 10) & 31) * b >> 7;
    if (tr > 31) tr = 31;
    if (tg > 31) tg = 31;
    if (tb > 31) tb = 31;
    return (t & 0x8000) | tr | tg << 5 | tb << 10;
}

static inline uint16_t rgb15(int r, int g, int b) {
    return (uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
}

// ------------------------------------------------------------ triángulos

typedef struct {
    int textured, gouraud, semi, raw, semi_mode;
    int tpx, tpy, depth, clut_x, clut_y;
} Shade;

static inline long edge(const Vtx *a, const Vtx *b, int x, int y) {
    return (long)(b->x - a->x) * (y - a->y) - (long)(b->y - a->y) * (x - a->x);
}

// Antialiasing de polígonos (solo en el color de 24 bits, la VRAM sigue exacta): cada píxel
// del borde se mezcla con el fondo según cuántas de sus 4 submuestras cubre el polígono.
// Equivale a la resolución interna aumentada en los bordes, que es donde este juego lo nota
// (focos, piezas que giran), sin multiplicar el trabajo de los sprites.
int gpu_aa = 1;

static inline int coverage(const Vtx *v0, const Vtx *v1, const Vtx *v2, int x, int y) {
    static const int q[4][2] = {{1, 1}, {3, 1}, {1, 3}, {3, 3}};
    int n = 0;
    for (int k = 0; k < 4; k++) {
        long px = 4L * x + q[k][0], py = 4L * y + q[k][1];
        long e0 = (long)(v2->x - v1->x) * (py - 4L * v1->y) - (long)(v2->y - v1->y) * (px - 4L * v1->x);
        long e1 = (long)(v0->x - v2->x) * (py - 4L * v2->y) - (long)(v0->y - v2->y) * (px - 4L * v2->x);
        long e2 = (long)(v1->x - v0->x) * (py - 4L * v0->y) - (long)(v1->y - v0->y) * (px - 4L * v0->x);
        n += e0 >= 0 && e1 >= 0 && e2 >= 0;
    }
    return n;
}

// Color de 24 bits que se ve ahora en (x, y) del área de dibujo, o en el margen panorámico.
static inline uint32_t *margin_px(int x, int y) {
    if (!cur_wide || y < da_y1 || y > da_y2) return NULL;
    int wx = x - da_x1 + ws_reveal;
    return wx >= 0 && wx < 320 + 2 * ws_reveal ? &cur_wide->px[y - da_y1][wx] : NULL;
}
static inline int read24(int x, int y, uint32_t *out) {
    if (y < da_y1 || y > da_y2) return 0;
    if (x >= da_x1 && x <= da_x2) {
        int i = (y & 511) * 1024 + (x & 1023);
        if (mask_check && (vram[i] & 0x8000)) return 0;
        *out = vram24_tag[i] == vram[i] ? vram24[i] : c15to24(vram[i]);
        return 1;
    }
    uint32_t *m = margin_px(x, y);
    if (!m) return 0;
    *out = *m;
    return 1;
}
static inline void write24(int x, int y, uint32_t c) {
    if (x >= da_x1 && x <= da_x2) {
        int i = (y & 511) * 1024 + (x & 1023);
        vram24[i] = c;
        vram24_tag[i] = vram[i];
        if (cur_wide) cur_wide->px[y - da_y1][x - da_x1 + ws_reveal] = c;
        return;
    }
    uint32_t *m = margin_px(x, y);
    if (m) { *m = c; cur_wide->margin_drawn = 1; }
}
static inline uint32_t lerp24(uint32_t a, uint32_t b, int cov) {
    uint32_t out = 0;
    for (int k = 0; k < 24; k += 8) {
        int ca = (a >> k) & 255, cb = (b >> k) & 255;
        out |= (uint32_t)(ca + (cb - ca) * cov / 4) << k;
    }
    return out;
}

static void draw_triangle(Vtx v0, Vtx v1, Vtx v2, const Shade *s) {
    long area = edge(&v0, &v1, v2.x, v2.y);
    if (area == 0) return;
    if (area < 0) { Vtx t = v1; v1 = v2; v2 = t; area = -area; }
    // La PS1 descarta polígonos enormes.
    int minx = v0.x, maxx = v0.x, miny = v0.y, maxy = v0.y;
    int minu = v0.u, maxu = v0.u, minv = v0.v, maxv = v0.v;
    const Vtx *vs[3] = {&v0, &v1, &v2};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
        if (vs[i]->u < minu) minu = vs[i]->u;
        if (vs[i]->u > maxu) maxu = vs[i]->u;
        if (vs[i]->v < minv) minv = vs[i]->v;
        if (vs[i]->v > maxv) maxv = vs[i]->v;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    int extra = cur_wide ? ws_reveal : 0;
    if (minx < da_x1 - extra) minx = da_x1 - extra;
    if (miny < da_y1) miny = da_y1;
    if (maxx > da_x2 + extra) maxx = da_x2 + extra;
    if (maxy > da_y2) maxy = da_y2;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            long w0 = edge(&v1, &v2, x, y), w1 = edge(&v2, &v0, x, y), w2 = edge(&v0, &v1, x, y);
            int inside = w0 >= 0 && w1 >= 0 && w2 >= 0;
            // Regla de arista: excluir aristas derecha/inferior (aprox.: excluir w==0 en dos).
            if (inside && ((w0 == 0 && (v2.y > v1.y || (v2.y == v1.y && v2.x < v1.x))) ||
                           (w1 == 0 && (v0.y > v2.y || (v0.y == v2.y && v0.x < v2.x))) ||
                           (w2 == 0 && (v1.y > v0.y || (v1.y == v0.y && v1.x < v0.x)))))
                inside = 0;
            int cov = inside ? 4 : 0;
            if (gpu_aa) {
                cov = coverage(&v0, &v1, &v2, x, y);
                if (inside && cov == 0) cov = 1;  // no hacer desaparecer polígonos finos
            }
            if (!inside && !cov) continue;
            int r = v0.r, g = v0.g, b = v0.b;
            if (s->gouraud) {
                r = (int)((w0 * v0.r + w1 * v1.r + w2 * v2.r) / area);
                g = (int)((w0 * v0.g + w1 * v1.g + w2 * v2.g) / area);
                b = (int)((w0 * v0.b + w1 * v1.b + w2 * v2.b) / area);
                if (!inside) {  // fuera del polígono la interpolación se extrapola: acotar
                    r = r < 0 ? 0 : r > 255 ? 255 : r;
                    g = g < 0 ? 0 : g > 255 ? 255 : g;
                    b = b < 0 ? 0 : b > 255 ? 255 : b;
                }
            }
            uint16_t col;
            uint32_t c24;
            int semi = s->semi;
            if (s->textured) {
                int u = (int)((w0 * v0.u + w1 * v1.u + w2 * v2.u) / area);
                int v = (int)((w0 * v0.v + w1 * v1.v + w2 * v2.v) / area);
                if (!inside) {
                    u = u < minu ? minu : u > maxu ? maxu : u;
                    v = v < minv ? minv : v > maxv ? maxv : v;
                }
                uint16_t t = texel(u, v, s->tpx, s->tpy, s->depth, s->clut_x, s->clut_y);
                if (t == 0) continue;
                semi = semi && (t & 0x8000);
                col = s->raw ? t : modulate(t, r, g, b);
                c24 = s->raw ? c15to24(t) : modulate24(t, r, g, b);
            } else {
                col = rgb15(r, g, b);
                c24 = rgb24(r, g, b);
            }
            if (cov == 4) { plot(x, y, col, c24, semi, s->semi_mode); continue; }
            uint32_t back, fin;
            if (!read24(x, y, &back)) continue;
            if (inside) {
                plot(x, y, col, c24, semi, s->semi_mode);
                if (!read24(x, y, &fin)) continue;
            } else {
                fin = semi ? blend24(back, c24, s->semi_mode) : c24;
            }
            write24(x, y, lerp24(back, fin, cov));
        }
    }
}

// ------------------------------------------------------------ rectángulos
static void draw_rect(int x, int y, int w, int h, int r, int g, int b, int textured, int raw,
                      int semi, int u0, int v0, int clut_x, int clut_y) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint16_t col;
            uint32_t c24;
            int sm = semi;
            if (textured) {
                uint16_t t = texel(u0 + i, v0 + j, tp_x, tp_y, tp_depth, clut_x, clut_y);
                if (t == 0) continue;
                sm = sm && (t & 0x8000);
                col = raw ? t : modulate(t, r, g, b);
                c24 = raw ? c15to24(t) : modulate24(t, r, g, b);
            } else {
                col = rgb15(r, g, b);
                c24 = rgb24(r, g, b);
            }
            plot(x + i, y + j, col, c24, sm, tp_semi);
        }
    }
}

static void draw_line(Vtx a, Vtx b, int gouraud, int semi) {
    int dx = abs(b.x - a.x), dy = abs(b.y - a.y);
    int n = dx > dy ? dx : dy;
    if (dx >= 1024 || dy >= 512) return;
    // HD: la línea como un cuadrilátero de un píxel de grosor.
    HdPrim hp = {.n = 4, .semi = semi, .mode = tp_semi};
    hd_begin(&hp);
    hp.twin = 0;
    Vtx e = b;
    int horiz = dx >= dy;
    if (horiz) e.x += b.x >= a.x ? 1 : -1; else e.y += b.y >= a.y ? 1 : -1;
    for (int i = 0; i < 4; i++) {
        const Vtx *q = i & 1 ? &e : &a;
        hp.x[i] = q->x + (horiz ? 0 : (i >> 1)); hp.y[i] = q->y + (horiz ? (i >> 1) : 0);
        const Vtx *c = i & 1 && gouraud ? &b : &a;
        hp.rgb[i] = rgb24(c->r, c->g, c->b);
    }
    hd_prim(&hp);
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (b.x - a.x) * i / n : a.x;
        int y = n ? a.y + (b.y - a.y) * i / n : a.y;
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud && n) {
            r = a.r + (b.r - a.r) * i / n;
            g = a.g + (b.g - a.g) * i / n;
            bl = a.b + (b.b - a.b) * i / n;
        }
        plot(x, y, rgb15(r, g, bl), rgb24(r, g, bl), semi, tp_semi);
    }
}

// ------------------------------------------------------------ comandos GP0
static void set_texpage(uint32_t w) {
    tp_x = (w & 15) * 64;
    tp_y = ((w >> 4) & 1) * 256;
    tp_semi = (w >> 5) & 3;
    tp_depth = (w >> 7) & 3;
    dither = (w >> 9) & 1;
    tex_disable = (w >> 11) & 1;
}

static int cmd_length(uint32_t w) {
    int op = w >> 24;
    if (op >= 0x20 && op < 0x40) {
        int n = (op & 8) ? 4 : 3, tex = (op & 4) ? 1 : 0, gour = (op & 0x10) ? 1 : 0;
        return 1 + n * (1 + tex) + (gour ? n - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) {
        if (op & 8) return -1;  // polilínea: hasta el terminador
        return (op & 0x10) ? 4 : 3;
    }
    if (op >= 0x60 && op < 0x80) {
        int len = 2 + ((op & 4) ? 1 : 0);
        if (((op >> 3) & 3) == 0) len++;
        return len;
    }
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xE0) return 3;
    return 1;
}

static int hud_shift(int x) {
    if (!cur_wide || gpu_pkt_addr < 0x139800 || gpu_pkt_addr >= 0x13A000) return 0;
    int rel = x - da_x1;
    return rel < 107 ? -ws_reveal : rel > 213 ? ws_reveal : 0;
}

// Panorámica: un relleno sin textura que ocupa todo el ancho de la pantalla (fundidos,
// destellos, tintes, aunque vayan en franjas) se estira para cubrir también los márgenes.
static int covers_screen(int x0, int x1, int y0, int y1) {
    (void)y0; (void)y1;
    return cur_wide && x0 <= da_x1 && x1 >= da_x2;
}

static void exec_polygon(void) {
    uint32_t op = fifo[0] >> 24;
    int n = (op & 8) ? 4 : 3, tex = (op & 4) != 0, gour = (op & 0x10) != 0;
    Shade s = {tex, gour, (op & 2) != 0, (op & 1) != 0, tp_semi, tp_x, tp_y, tp_depth, 0, 0};
    Vtx v[4];
    int k = 0;
    uint32_t color = fifo[k++] & 0xFFFFFF;
    for (int i = 0; i < n; i++) {
        if (gour && i > 0) color = fifo[k++] & 0xFFFFFF;
        uint32_t xy = fifo[k++];
        v[i].x = sx11(xy) + off_x;
        v[i].y = sx11(xy >> 16) + off_y;
        v[i].r = color & 255;
        v[i].g = (color >> 8) & 255;
        v[i].b = (color >> 16) & 255;
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t uv = fifo[k++];
            v[i].u = uv & 255;
            v[i].v = (uv >> 8) & 255;
            if (i == 0) { s.clut_x = ((uv >> 16) & 63) * 16; s.clut_y = (uv >> 22) & 511; }
            if (i == 1) {
                uint32_t tp = uv >> 16;
                s.tpx = tp_x = (tp & 15) * 64;
                s.tpy = tp_y = ((tp >> 4) & 1) * 256;
                s.semi_mode = tp_semi = (tp >> 5) & 3;
                s.depth = tp_depth = (tp >> 7) & 3;
            }
        }
    }
    if (!gour && tex && s.raw) { v[0].r = v[0].g = v[0].b = 128; }
    int dx = hud_shift(v[0].x);
    for (int i = 0; i < n; i++) v[i].x += dx;
    if (!tex) {
        int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (int i = 1; i < n; i++) {
            if (v[i].x < x0) x0 = v[i].x;
            if (v[i].x > x1) x1 = v[i].x;
            if (v[i].y < y0) y0 = v[i].y;
            if (v[i].y > y1) y1 = v[i].y;
        }
        if (covers_screen(x0, x1 - 1, y0, y1 - 1))
            for (int i = 0; i < n; i++) v[i].x += v[i].x == x0 ? -ws_reveal : v[i].x == x1 ? ws_reveal : 0;
    }
    if (tex) {
        int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
        for (int i = 0; i < n; i++) {
            if (v[i].u < u0) u0 = v[i].u;
            if (v[i].u > u1) u1 = v[i].u;
            if (v[i].v < v0) v0 = v[i].v;
            if (v[i].v > v1) v1 = v[i].v;
        }
        tex_record(s.depth, s.tpx, s.tpy, s.clut_x, s.clut_y, u0, v0, u1 - u0 + 1, v1 - v0 + 1);
        // Pieza de pose: cuadrilátero alineado con los ejes (los sprites volteados lo son).
        if (n == 4 && s.depth == 0 && v[0].y == v[1].y && v[0].x == v[2].x && v[3].x == v[1].x && v[3].y == v[2].y) {
            int w = v[1].x - v[0].x, h = v[2].y - v[0].y;
            int su = v[1].u >= v[0].u ? 1 : -1, sv = v[2].v >= v[0].v ? 1 : -1;
            int x = v[0].x, y = v[0].y, ub = v[0].u, vb = v[0].v;
            if (w < 0) { w = -w; x = v[1].x; ub = v[1].u; su = -su; }
            if (h < 0) { h = -h; y = v[2].y; vb = v[2].v; sv = -sv; }
            // Con u decreciente el primer texel es u-1 (la PS1 no incluye el borde derecho).
            if (su < 0) ub = (ub - 1) & 255;
            if (sv < 0) vb = (vb - 1) & 255;
            tex_pose_piece(s.tpx, s.tpy, s.clut_x, s.clut_y, x, y, w, h, ub, vb, su, sv);
        }
    }
    HdPrim hp = {.n = n, .textured = tex, .raw = s.raw, .semi = s.semi, .mode = s.semi_mode, .depth = s.depth,
                 .tpx = s.tpx, .tpy = s.tpy, .cx = s.clut_x, .cy = s.clut_y};
    hd_begin(&hp);
    if (!tex) hp.twin = 0;
    for (int i = 0; i < n; i++) {
        hp.x[i] = v[i].x; hp.y[i] = v[i].y; hp.u[i] = v[i].u; hp.v[i] = v[i].v;
        hp.rgb[i] = rgb24(v[i].r, v[i].g, v[i].b);
    }
    hd_prim(&hp);
    draw_triangle(v[0], v[1], v[2], &s);
    if (n == 4) draw_triangle(v[1], v[2], v[3], &s);
}

static void exec_rect(void) {
    uint32_t op = fifo[0] >> 24;
    int k = 1, tex = (op & 4) != 0;
    uint32_t xy = fifo[k++];
    int x = sx11(xy) + off_x, y = sx11(xy >> 16) + off_y;
    x += hud_shift(x);
    int u = 0, v = 0, cx = 0, cy = 0;
    if (tex) {
        uint32_t uv = fifo[k++];
        u = uv & 255;
        v = (uv >> 8) & 255;
        cx = ((uv >> 16) & 63) * 16;
        cy = (uv >> 22) & 511;
    }
    int w, h;
    switch ((op >> 3) & 3) {
        case 0: w = fifo[k] & 0x3FF; h = (fifo[k] >> 16) & 0x1FF; break;
        case 1: w = h = 1; break;
        case 2: w = h = 8; break;
        default: w = h = 16; break;
    }
    if (!tex && covers_screen(x, x + w - 1, y, y + h - 1)) { x -= ws_reveal; w += 2 * ws_reveal; }
    uint32_t c = fifo[0];
    if (tex) tex_record(tp_depth, tp_x, tp_y, cx, cy, u, v, w, h);
    if (tex && tp_depth == 0) tex_pose_piece(tp_x, tp_y, cx, cy, x, y, w, h, u, v, 1, 1);
    HdPrim hp = {.n = 4, .textured = tex, .raw = op & 1, .semi = (op & 2) != 0, .mode = tp_semi, .depth = tp_depth,
                 .tpx = tp_x, .tpy = tp_y, .cx = cx, .cy = cy};
    hd_begin(&hp);
    if (!tex) hp.twin = 0;
    for (int i = 0; i < 4; i++) {
        hp.x[i] = x + (i & 1) * w; hp.y[i] = y + (i >> 1) * h;
        hp.u[i] = u + (i & 1) * w; hp.v[i] = v + (i >> 1) * h;
        hp.rgb[i] = (op & 1) && tex ? rgb24(128, 128, 128) : (c & 0xFFFFFF);
    }
    hd_prim(&hp);
    draw_rect(x, y, w, h, c & 255, (c >> 8) & 255, (c >> 16) & 255, tex, op & 1, (op & 2) != 0,
              u, v, cx, cy);
}

static void exec_line(void) {
    uint32_t op = fifo[0] >> 24;
    int gour = (op & 0x10) != 0;
    Vtx a = {0}, b = {0};
    uint32_t c = fifo[0];
    a.r = c & 255; a.g = (c >> 8) & 255; a.b = (c >> 16) & 255;
    a.x = sx11(fifo[1]) + off_x; a.y = sx11(fifo[1] >> 16) + off_y;
    uint32_t c2 = gour ? fifo[2] : c;
    uint32_t xy2 = gour ? fifo[3] : fifo[2];
    b.r = c2 & 255; b.g = (c2 >> 8) & 255; b.b = (c2 >> 16) & 255;
    b.x = sx11(xy2) + off_x; b.y = sx11(xy2 >> 16) + off_y;
    draw_line(a, b, gour, (op & 2) != 0);
}

static void exec_command(void) {
    uint32_t w = fifo[0], op = w >> 24;
    if (op >= 0x20 && op < 0x40) { exec_polygon(); return; }
    if (op >= 0x40 && op < 0x60) { exec_line(); return; }
    if (op >= 0x60 && op < 0x80) { exec_rect(); return; }
    switch (op) {
        case 0x02: {  // relleno de rectángulo (sin área de dibujo ni desplazamiento)
            int x = fifo[1] & 0x3F0, y = (fifo[1] >> 16) & 0x1FF;
            int ww = ((fifo[2] & 0x3FF) + 15) & ~15, hh = (fifo[2] >> 16) & 0x1FF;
            uint16_t col = rgb15(w & 255, (w >> 8) & 255, (w >> 16) & 255);
            hd_fill(x, y, ww, hh, rgb24(w & 255, (w >> 8) & 255, (w >> 16) & 255));
            for (int j = 0; j < hh; j++)
                for (int i = 0; i < ww; i++) vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = col;
            for (int k = 0; k < 2; k++) {
                Wide *wd = &wide[k];
                if (!ws_reveal || !wd->used || x > wd->ox || y > wd->oy || x + ww < wd->ox + 320 || y + hh < wd->oy + 240) continue;
                uint32_t c24 = rgb24(w & 255, (w >> 8) & 255, (w >> 16) & 255);
                for (int j = 0; j < 240; j++)
                    for (int i = 0; i < 320 + 2 * ws_reveal; i++) wd->px[j][i] = c24;
            }
            return;
        }
        case 0x80: {  // copia VRAM -> VRAM
            int sx = fifo[1] & 1023, sy = (fifo[1] >> 16) & 511;
            int dx = fifo[2] & 1023, dy = (fifo[2] >> 16) & 511;
            int ww = ((fifo[3] - 1) & 1023) + 1, hh = (((fifo[3] >> 16) - 1) & 511) + 1;
            hd_taint(dx, dy, ww, hh);
            for (int j = 0; j < hh; j++)
                for (int i = 0; i < ww; i++)
                    vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)] =
                        vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
            return;
        }
        case 0xA0: case 0xC0:
            xfer_mode = op == 0xA0 ? 1 : 2;
            xfer_x = fifo[1] & 1023;
            xfer_y = (fifo[1] >> 16) & 511;
            xfer_w = ((fifo[2] - 1) & 1023) + 1;
            xfer_h = (((fifo[2] >> 16) - 1) & 511) + 1;
            xfer_i = 0;
            if (xfer_mode == 1) hd_taint(xfer_x, xfer_y, xfer_w, xfer_h);
            return;
        case 0xE1: set_texpage(w); return;
        case 0xE2:
            tw_mask_x = w & 31; tw_mask_y = (w >> 5) & 31;
            tw_off_x = (w >> 10) & 31; tw_off_y = (w >> 15) & 31;
            return;
        case 0xE3: da_x1 = w & 1023; da_y1 = (w >> 10) & 511; select_wide(); return;
        case 0xE4: da_x2 = w & 1023; da_y2 = (w >> 10) & 511; select_wide(); return;
        case 0xE5: off_x = sx11(w); off_y = sx11(w >> 11); return;
        case 0xE6: mask_set = w & 1; mask_check = (w >> 1) & 1; return;
        case 0x00: case 0x01: return;
        case 0x1F: gpu_irq = 1; return;
    }
    static uint8_t seen[256];
    if (!seen[op]) { seen[op] = 1; LOG("[gpu] GP0 %02X sin implementar\n", op); }
}

static void xfer_pixel(uint16_t p) {
    int x = xfer_x + xfer_i % xfer_w, y = xfer_y + xfer_i / xfer_w;
    vram[(y & 511) * 1024 + (x & 1023)] = p | (mask_set ? 0x8000 : 0);
    if (++xfer_i >= xfer_w * xfer_h) xfer_mode = 0;
}

void gpu_gp0(uint32_t w) {
    if (xfer_mode == 1) {
        xfer_pixel(w & 0xFFFF);
        if (xfer_mode == 1) xfer_pixel(w >> 16);
        return;
    }
    if (polyline) {
        if ((w & 0xF000F000) == 0x50005000 && poly_have_prev && !poly_expect_color) { polyline = 0; return; }
        if (poly_gour && poly_expect_color) { poly_color = w; poly_expect_color = 0; return; }
        Vtx v = {sx11(w) + off_x, sx11(w >> 16) + off_y,
                 poly_color & 255, (poly_color >> 8) & 255, (poly_color >> 16) & 255, 0, 0};
        if (poly_have_prev) draw_line(poly_prev, v, poly_gour, poly_semi);
        poly_prev = v;
        poly_have_prev = 1;
        poly_expect_color = poly_gour;
        return;
    }
    if (fifo_n == 0) {
        fifo_need = cmd_length(w);
        if (fifo_need < 0) {
            polyline = 1;
            poly_gour = (w >> 28) & 1;
            poly_semi = (w >> 25) & 1;
            poly_color = w & 0xFFFFFF;
            poly_have_prev = poly_expect_color = 0;
            return;
        }
    }
    fifo[fifo_n++] = w;
    if (fifo_n >= fifo_need) {
        exec_command();
        fifo_n = 0;
    }
}

uint32_t gpu_read(void) {
    if (xfer_mode != 2) return 0;
    uint32_t r = 0;
    for (int k = 0; k < 2 && xfer_mode == 2; k++) {
        int x = xfer_x + xfer_i % xfer_w, y = xfer_y + xfer_i / xfer_w;
        r |= (uint32_t)vram[(y & 511) * 1024 + (x & 1023)] << (16 * k);
        if (++xfer_i >= xfer_w * xfer_h) xfer_mode = 0;
    }
    return r;
}

void gpu_gp1(uint32_t w) {
    switch (w >> 24) {
        case 0x00:
            fifo_n = 0; xfer_mode = 0; polyline = 0;
            disp_enabled = 0; set_texpage(0);
            da_x1 = da_y1 = 0; da_x2 = 1023; da_y2 = 511; off_x = off_y = 0;
            break;
        case 0x01: fifo_n = 0; xfer_mode = 0; polyline = 0; break;
        case 0x02: gpu_irq = 0; break;
        case 0x03: disp_enabled = !(w & 1); break;
        case 0x04: dma_dir = w & 3; break;
        case 0x05: disp_x = w & 1023; disp_y = (w >> 10) & 511; hd_display(disp_x, disp_y); break;
        case 0x08: {
            static const int widths[4] = {256, 320, 512, 640};
            disp_w = (w & 0x40) ? 368 : widths[w & 3];
            disp_h = (w & 4) && (w & 0x20) ? 480 : 240;
            disp_24bit = (w >> 4) & 1;
            break;
        }
        default: break;  // rangos horizontal/vertical, info: no afectan al render
    }
}

uint32_t gpu_stat(int odd_line) {
    uint32_t s = (tp_x / 64) | (tp_y / 256) << 4 | tp_semi << 5 | tp_depth << 7 | dither << 9;
    s |= mask_set << 11 | mask_check << 12 | tex_disable << 15;
    s |= (disp_24bit << 21) | (!disp_enabled << 23) | (gpu_irq << 24);
    s |= 1u << 26 | 1u << 28;               // listo para comandos y bloques DMA
    if (xfer_mode == 2) s |= 1u << 27;      // listo para enviar VRAM -> CPU
    s |= (uint32_t)dma_dir << 29;
    if (dma_dir == 1 || dma_dir == 2) s |= 1u << 25;
    if (odd_line) s |= 1u << 31;
    return s;
}

// Convierte el área visible de la VRAM a RGBA8888 (w x h). Devuelve la resolución.
int gpu_wide_frame;  // el último frame entregado es panorámico

void gpu_framebuffer(uint8_t *rgba, int *w, int *h) {
    // Panorámica: si el búfer mostrado tiene lienzo y este frame dibujó en los márgenes.
    Wide *wd = NULL;
    if (ws_reveal && !disp_24bit && disp_w == 320 && disp_h == 240)
        for (int k = 0; k < 2; k++)
            if (wide[k].used && wide[k].ox == disp_x && wide[k].oy == disp_y) wd = &wide[k];
    int margin = wd && wd->margin_drawn ? ws_reveal : 0;
    if (wd) wd->displayed = 1;
    gpu_wide_frame = margin > 0;
    *w = disp_w + 2 * margin;
    *h = disp_h;
    for (int y = 0; y < disp_h; y++) {
        for (int ox = 0; ox < *w; ox++) {
            uint8_t *o = rgba + (y * *w + ox) * 4;
            int x = ox - margin;
            if (x < 0 || x >= disp_w) {
                uint32_t c = wd->px[y][ox];
                o[0] = c & 255; o[1] = (c >> 8) & 255; o[2] = (c >> 16) & 255;
            } else if (disp_24bit) {
                int bx = disp_x * 2 + x * 3, row = (disp_y + y) & 511;
                const uint8_t *b = (const uint8_t *)&vram[row * 1024];
                o[0] = b[bx % 2048]; o[1] = b[(bx + 1) % 2048]; o[2] = b[(bx + 2) % 2048];
            } else {
                int i = ((disp_y + y) & 511) * 1024 + ((disp_x + x) & 1023);
                uint16_t p = vram[i];
                uint32_t c = vram24_tag[i] == p ? vram24[i] : c15to24(p);
                o[0] = c & 255; o[1] = (c >> 8) & 255; o[2] = (c >> 16) & 255;
            }
            o[3] = 255;
        }
    }
}
