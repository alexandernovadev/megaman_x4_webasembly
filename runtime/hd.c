// HD: además de dibujar por software en la VRAM, cada primitiva se anota en una "lista de
// dibujo" por búfer de pantalla. La página web la vuelve a dibujar con la GPU a la resolución
// de la pantalla, cambiando cada pieza de textura por su versión HD (8x, hecha con IA) cuando
// existe. Las piezas se reconocen por su contenido (huella de los índices de color), no por su
// posición en VRAM, porque el juego sube los fotogramas de animación sobre la marcha. La
// versión HD guarda índices de paleta, así que los cambios de paleta del juego siguen valiendo.
//
// Piezas nuevas: al mostrar un frame, cada pieza que aún no tiene versión HD se "captura" con
// sus vecinas en pantalla (8 píxeles alrededor) para que la IA vea el contexto y no queden
// costuras entre piezas. La captura se manda al servidor local (web) o a una carpeta (nativo).
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "runtime.h"
#include "hd.h"

#define MAXV (1 << 16)
#define MAXB 8192
#define MAXR 4096
#define NLISTS 4
#define EDGE_MARGIN 16   // una pieza a menos de esto del borde de la pantalla aún no tiene todas sus vecinas
#define EDGE_WAIT 120    // frames que se espera antes de capturarla igual (títulos, HUD: nunca se apartan del borde)
#define MAXP 2048  // paletas distintas por frame

typedef struct {
    uint64_t hash;
    int16_t x, y;          // esquina, en píxeles de textura (pantalla / escala; relativa al búfer)
    uint16_t sx, sy;       // escala de dibujo (pantalla / textura, en 1/256)
    uint8_t rw, rh;        // tamaño de la pieza en texels
    uint8_t axis, fx, fy;  // dibujada sin girar (puede estar agrandada); volteada en x / y
    uint8_t depth, capture, edge;  // edge: cerca del borde de la pantalla (le faltan vecinas)
    int order;
    uint16_t pal[256];
    uint8_t px[16 * 16];   // índices, en la orientación de la textura
} Rec;

typedef struct {
    int ox, oy, w, h, used, displayed, tainted, overflow, captured, snap;
    long stamp;
    uint16_t vram[512 * 1024];  // la VRAM cuando el juego pasó a mostrar este búfer
    int nv, nb, nr;
    HdVert v[MAXV];
    HdBatch b[MAXB];
    Rec r[MAXR];
    // Paletas tal como estaban al dibujar (el juego las cambia antes de mostrar el frame).
    int np;
    uint16_t pal[MAXP][256];
    uint64_t pal_key[MAXP * 2];
    uint16_t pal_idx[MAXP * 2];
} List;

static List *lists;
static long stamp;

// ---------------------------------------------------------------- tablas de huellas
#define SET_BITS 17
#define SET_SIZE (1u << SET_BITS)
static uint64_t *known;        // ya tienen versión HD o ya se capturaron
static uint64_t *id_keys;      // huella -> id
static uint16_t *id_vals;
static uint64_t id_hash[65536];
static int next_id = 1;
static int known_n;
uint32_t hd_new_ids[2048 * 3];  // id, huella baja, huella alta
uint8_t hd_new_px[2048 * 256];  // y sus texels (índices, 16x16 rellenando con 0)
int hd_new_n;
int hd_capture_on;  // mandar capturas (la web lo activa si hay servidor HD)

static void init(void) {
    if (lists) return;
    lists = calloc(NLISTS, sizeof *lists);
    known = calloc(SET_SIZE, 8);
    id_keys = calloc(SET_SIZE, 8);
    id_vals = calloc(SET_SIZE, 2);
}

static uint32_t slot_of(uint64_t h) { return (uint32_t)(h ^ (h >> 29)) & (SET_SIZE - 1); }

static int set_has(const uint64_t *t, uint64_t h) {
    for (uint32_t i = slot_of(h);; i = (i + 1) & (SET_SIZE - 1)) {
        if (t[i] == h) return 1;
        if (!t[i]) return 0;
    }
}
static void set_add(uint64_t *t, uint64_t h) {
    if (known_n > (int)(SET_SIZE * 3 / 4)) return;
    for (uint32_t i = slot_of(h);; i = (i + 1) & (SET_SIZE - 1)) {
        if (t[i] == h) return;
        if (!t[i]) { t[i] = h; known_n++; return; }
    }
}

uint64_t hd_id_hash(int id) { return id > 0 && id < next_id ? id_hash[id] : 0; }
int hd_id_count(void) { return next_id; }

void hd_known(const uint32_t *pairs, int n) {
    init();
    for (int i = 0; i < n; i++) set_add(known, (uint64_t)pairs[2 * i] | (uint64_t)pairs[2 * i + 1] << 32);
}

static int id_for(uint64_t h, const uint8_t *px, int rw, int rh) {
    uint32_t i = slot_of(h);
    for (;; i = (i + 1) & (SET_SIZE - 1)) {
        if (id_keys[i] == h) return id_vals[i];
        if (!id_keys[i]) break;
    }
    if (next_id >= 65536 || hd_new_n >= 2048) return 0;
    id_keys[i] = h;
    id_vals[i] = (uint16_t)next_id;
    id_hash[next_id] = h;
    hd_new_ids[hd_new_n * 3] = (uint32_t)next_id;
    hd_new_ids[hd_new_n * 3 + 1] = (uint32_t)h;
    hd_new_ids[hd_new_n * 3 + 2] = (uint32_t)(h >> 32);
    uint8_t *o = hd_new_px + hd_new_n * 256;
    memset(o, 0, 256);
    for (int j = 0; j < rh; j++) memcpy(o + j * 16, px + j * rw, (size_t)rw);
    hd_new_n++;
    return next_id++;
}

static uint64_t fnv(uint64_t h, uint32_t v) { return (h ^ v) * 0x100000001b3ULL; }

// ---------------------------------------------------------------- listas por búfer
static List *list_at(int ox, int oy, int w, int h) {
    init();
    List *l = NULL, *old = &lists[0];
    for (int i = 0; i < NLISTS; i++) {
        if (lists[i].used && lists[i].ox == ox && lists[i].oy == oy) l = &lists[i];
        if (lists[i].stamp < old->stamp) old = &lists[i];
    }
    if (!l) {
        l = old;
        l->used = 1; l->ox = ox; l->oy = oy; l->displayed = 1;
    }
    l->w = w; l->h = h;
    l->stamp = ++stamp;
    if (l->displayed) {  // primer dibujo tras mostrarse: empieza un frame nuevo
        l->displayed = l->tainted = l->overflow = l->captured = l->snap = 0;
        l->nv = l->nb = l->nr = l->np = 0;
        memset(l->pal_key, 0, sizeof l->pal_key);
    }
    return l;
}

static void add_batch(List *l, int blend, int x0, int y0, int x1, int y1) {
    HdBatch *b = l->nb ? &l->b[l->nb - 1] : NULL;
    if (b && b->blend == blend && b->sx0 == x0 && b->sy0 == y0 && b->sx1 == x1 && b->sy1 == y1 &&
        b->first + b->count == (uint32_t)l->nv)
        return;
    if (l->nb >= MAXB) { l->overflow = 1; return; }
    b = &l->b[l->nb++];
    b->first = (uint32_t)l->nv; b->count = 0; b->blend = blend;
    b->sx0 = x0; b->sy0 = y0; b->sx1 = x1; b->sy1 = y1;
}

static void put(List *l, float x, float y, float u, float v, uint32_t col, uint32_t tex, uint32_t hd, uint32_t tw) {
    HdVert *p = &l->v[l->nv++];
    p->x = x - l->ox; p->y = y - l->oy; p->u = u; p->v = v;
    p->col = col; p->tex = tex; p->hd = hd; p->twin = tw;
}

// Índice en la tabla de paletas del frame de la paleta que hay ahora en (cx, cy).
static int palette_index(List *l, int depth, int cx, int cy) {
    int colors = depth == 0 ? 16 : 256;
    uint16_t c[256];
    uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)depth;
    for (int i = 0; i < colors; i++) { c[i] = vram[cy * 1024 + ((cx + i) & 1023)]; h = (h ^ c[i]) * 0x100000001b3ULL; }
    if (!h) h = 1;
    uint32_t i = (uint32_t)(h ^ (h >> 31)) & (MAXP * 2 - 1);
    for (;; i = (i + 1) & (MAXP * 2 - 1)) {
        if (l->pal_key[i] == h) return l->pal_idx[i];
        if (!l->pal_key[i]) break;
    }
    if (l->np >= MAXP) return -1;
    l->pal_key[i] = h;
    l->pal_idx[i] = (uint16_t)l->np;
    memcpy(l->pal[l->np], c, (size_t)colors * 2);
    if (colors < 256) memset(l->pal[l->np] + colors, 0, (size_t)(256 - colors) * 2);
    return l->np++;
}

// Índice de color del texel (u, v) de la página.
static int index_at(int depth, int tpx, int tpy, int u, int v) {
    int y = (tpy + (v & 255)) & 511;
    u &= 255;
    if (depth == 0) return (vram[y * 1024 + ((tpx + u / 4) & 1023)] >> ((u & 3) * 4)) & 15;
    return (vram[y * 1024 + ((tpx + u / 2) & 1023)] >> ((u & 1) * 8)) & 255;
}

// Pieza grande dibujada tal cual (sin escalar ni girar, con o sin volteo): se parte en trozos de
// 16x16, que se ven igual y así tienen versión HD como las demás (letreros, logotipos...).
static int split_large(const HdPrim *p) {
    if (!p->textured || p->n != 4 || p->depth > 1 || (p->twin & 0xFFFFF)) return 0;
    int xs0 = p->x[0], xs1 = p->x[0], ys0 = p->y[0], ys1 = p->y[0];
    int u0 = p->u[0], u1 = p->u[0], v0 = p->v[0], v1 = p->v[0];
    for (int i = 1; i < 4; i++) {
        if (p->x[i] < xs0) xs0 = p->x[i];
        if (p->x[i] > xs1) xs1 = p->x[i];
        if (p->y[i] < ys0) ys0 = p->y[i];
        if (p->y[i] > ys1) ys1 = p->y[i];
        if (p->u[i] < u0) u0 = p->u[i];
        if (p->u[i] > u1) u1 = p->u[i];
        if (p->v[i] < v0) v0 = p->v[i];
        if (p->v[i] > v1) v1 = p->v[i];
        if (p->rgb[i] != p->rgb[0]) return 0;
    }
    int w = xs1 - xs0, h = ys1 - ys0;
    if ((w <= 16 && h <= 16) || w != u1 - u0 || h != v1 - v0) return 0;
    int ux0 = -1, vy0 = -1;
    for (int i = 0; i < 4; i++) {
        if (p->x[i] != xs0 && p->x[i] != xs1) return 0;
        if (p->y[i] != ys0 && p->y[i] != ys1) return 0;
        if (p->x[i] == xs0) ux0 = p->u[i];
        if (p->y[i] == ys0) vy0 = p->v[i];
    }
    int fx = ux0 != u0, fy = vy0 != v0;  // la u crece hacia la izquierda: volteada
    for (int y = ys0; y < ys1; y += 16)
        for (int x = xs0; x < xs1; x += 16) {
            int tw = xs1 - x < 16 ? xs1 - x : 16, th = ys1 - y < 16 ? ys1 - y : 16;
            HdPrim q = *p;
            for (int i = 0; i < 4; i++) {
                int dx = (i & 1) ? tw : 0, dy = (i >> 1) ? th : 0;
                q.x[i] = x + dx; q.y[i] = y + dy;
                q.u[i] = fx ? ux0 - (x - xs0) - dx : ux0 + (x - xs0) + dx;
                q.v[i] = fy ? vy0 - (y - ys0) - dy : vy0 + (y - ys0) + dy;
            }
            hd_prim(&q);
        }
    return 1;
}

// La PS1 lee el texel exacto del vértice de partida y avanza un texel por píxel; la GPU muestrea
// en el centro del píxel. Sin volteo da lo mismo, pero en un sprite volteado (la u baja hacia la
// derecha) la GPU leería un texel más a la izquierda en cada píxel. Se corrige sumando 1 a las
// coordenadas del eje volteado: así la región de la pieza (y su huella) es la misma que sin
// voltear y no se cuela una columna de la pieza vecina.
static void fix_flip(HdPrim *q) {
    if (!q->textured || q->n != 4) return;
    int xs0 = q->x[0], xs1 = q->x[0], ys0 = q->y[0], ys1 = q->y[0];
    for (int i = 1; i < 4; i++) {
        if (q->x[i] < xs0) xs0 = q->x[i];
        if (q->x[i] > xs1) xs1 = q->x[i];
        if (q->y[i] < ys0) ys0 = q->y[i];
        if (q->y[i] > ys1) ys1 = q->y[i];
    }
    int ul = -1, ur = -1, vt = -1, vb = -1;
    for (int i = 0; i < 4; i++) {
        if (q->x[i] != xs0 && q->x[i] != xs1) return;  // girado: se deja como está
        if (q->y[i] != ys0 && q->y[i] != ys1) return;
        int *pu = q->x[i] == xs0 ? &ul : &ur, *pv = q->y[i] == ys0 ? &vt : &vb;
        if (*pu >= 0 && *pu != q->u[i]) return;
        if (*pv >= 0 && *pv != q->v[i]) return;
        *pu = q->u[i]; *pv = q->v[i];
    }
    if (xs1 > xs0 && ul > ur) for (int i = 0; i < 4; i++) q->u[i]++;
    if (ys1 > ys0 && vt > vb) for (int i = 0; i < 4; i++) q->v[i]++;
}

void hd_prim(const HdPrim *orig) {
    if (split_large(orig)) return;
    HdPrim fixed = *orig;
    fix_flip(&fixed);
    const HdPrim *p = &fixed;
    List *l = list_at(p->da_x, p->da_y, p->da_w, p->da_h);
    int nv = p->n == 4 ? 6 : 3;
    if (l->nv + nv > MAXV) { l->overflow = 1; return; }
    int blend = p->semi ? 1 + p->mode : 0;
    add_batch(l, blend, p->clip_x0 - l->ox, p->clip_y0 - l->oy, p->clip_x1 - l->ox, p->clip_y1 - l->oy);
    if (l->overflow) return;
    uint32_t flags = (p->textured ? 1u : 0) | (p->raw ? 2u : 0) | (p->semi ? 4u : 0) | (uint32_t)p->mode << 3;
    uint32_t tex = 0, hd = 0, tw = p->twin & 0xFFFFF;
    if (p->textured) {
        int pi = p->depth <= 1 ? palette_index(l, p->depth, p->cx, p->cy) : 0;
        if (pi < 0) { l->overflow = 1; return; }
        tex = (uint32_t)(p->tpx / 64) | (uint32_t)(p->tpy / 256) << 4 | (uint32_t)p->depth << 5 | (uint32_t)pi << 7;
        // Región de texels que lee la pieza.
        int u0 = p->u[0], u1 = p->u[0], v0 = p->v[0], v1 = p->v[0];
        for (int i = 1; i < p->n; i++) {
            if (p->u[i] < u0) u0 = p->u[i];
            if (p->u[i] > u1) u1 = p->u[i];
            if (p->v[i] < v0) v0 = p->v[i];
            if (p->v[i] > v1) v1 = p->v[i];
        }
        int rw = u1 - u0, rh = v1 - v0;
        if (rw < 1) rw = 1;
        if (rh < 1) rh = 1;
        if (p->depth <= 1 && !tw && rw <= 16 && rh <= 16 && u0 + rw <= 256 && v0 + rh <= 256) {
            Rec tmp, *r = l->nr < MAXR ? &l->r[l->nr] : &tmp;
            uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)(p->depth << 20 | rw << 10 | rh);
            for (int j = 0; j < rh; j++)
                for (int i = 0; i < rw; i++) {
                    int idx = index_at(p->depth, p->tpx, p->tpy, u0 + i, v0 + j);
                    r->px[j * rw + i] = (uint8_t)idx;
                    h = fnv(h, (uint32_t)idx);
                }
            int id = id_for(h, r->px, rw, rh);
            if (id) hd = (uint32_t)id | (uint32_t)u0 << 16 | (uint32_t)v0 << 24;
            tw |= (uint32_t)(rw - 1) << 20 | (uint32_t)(rh - 1) << 24;
            // Para capturarla: posición en pantalla y orientación si se dibuja tal cual.
            int xs0 = p->x[0], xs1 = p->x[0], ys0 = p->y[0], ys1 = p->y[0];
            for (int i = 1; i < p->n; i++) {
                if (p->x[i] < xs0) xs0 = p->x[i];
                if (p->x[i] > xs1) xs1 = p->x[i];
                if (p->y[i] < ys0) ys0 = p->y[i];
                if (p->y[i] > ys1) ys1 = p->y[i];
            }
            r->hash = h; r->rw = (uint8_t)rw; r->rh = (uint8_t)rh; r->depth = (uint8_t)p->depth;
            // Piezas agrandadas (p. ej. la bola de la carga de X): su posición se pasa a píxeles
            // de textura, así las de un mismo objeto encajan entre sí como si no lo estuvieran.
            int sw = xs1 - xs0, sh = ys1 - ys0;
            r->sx = (uint16_t)(sw > 0 ? sw * 256 / rw : 256); r->sy = (uint16_t)(sh > 0 ? sh * 256 / rh : 256);
            r->x = (int16_t)floor((xs0 - l->ox) * 256.0 / r->sx + 0.5);
            r->y = (int16_t)floor((ys0 - l->oy) * 256.0 / r->sy + 0.5);
            r->axis = 0; r->fx = r->fy = 0;
            r->edge = xs0 - l->ox - EDGE_MARGIN < -ws_reveal || xs1 - l->ox + EDGE_MARGIN > l->w + ws_reveal ||
                      ys0 - l->oy - EDGE_MARGIN < 0 || ys1 - l->oy + EDGE_MARGIN > l->h;
            if (p->n == 4 && sw > 0 && sh > 0) {
                int axis = 1, ux0 = -1, ux1 = -1, vy0 = -1, vy1 = -1;
                for (int i = 0; i < 4; i++) {
                    if (p->x[i] != xs0 && p->x[i] != xs1) axis = 0;
                    if (p->y[i] != ys0 && p->y[i] != ys1) axis = 0;
                    if (p->x[i] == xs0) { if (ux0 >= 0 && ux0 != p->u[i]) axis = 0; ux0 = p->u[i]; }
                    else { if (ux1 >= 0 && ux1 != p->u[i]) axis = 0; ux1 = p->u[i]; }
                    if (p->y[i] == ys0) { if (vy0 >= 0 && vy0 != p->v[i]) axis = 0; vy0 = p->v[i]; }
                    else { if (vy1 >= 0 && vy1 != p->v[i]) axis = 0; vy1 = p->v[i]; }
                }
                if (axis) { r->axis = 1; r->fx = ux0 > ux1; r->fy = vy0 > vy1; }
            }
            r->capture = hd_capture_on && !set_has(known, h);
            if (hd_capture_on) {
                int colors = p->depth == 0 ? 16 : 256;
                for (int i = 0; i < colors; i++) r->pal[i] = vram[p->cy * 1024 + ((p->cx + i) & 1023)];
            }
            r->order = l->nr;
            if (r != &tmp) l->nr++;
        }
    }
    static const int quad[6] = {0, 1, 2, 1, 2, 3};
    for (int k = 0; k < nv; k++) {
        int i = p->n == 4 ? quad[k] : k;
        put(l, (float)p->x[i], (float)p->y[i], (float)p->u[i], (float)p->v[i], (p->rgb[i] & 0xFFFFFF) | flags << 24, tex, hd, tw);
    }
    l->b[l->nb - 1].count += (uint32_t)nv;
}

void hd_fill(int x, int y, int w, int h, uint32_t rgb) {
    init();
    for (int i = 0; i < NLISTS; i++) {
        List *l = &lists[i];
        if (!l->used || x > l->ox || y > l->oy || x + w < l->ox + l->w || y + h < l->oy + l->h) {
            // Relleno parcial de un búfer: se anota igual, sin estirar a los márgenes.
            if (!l->used || x >= l->ox + l->w || y >= l->oy + l->h || x + w <= l->ox || y + h <= l->oy) continue;
        }
        int full = x <= l->ox && x + w >= l->ox + l->w;
        l = list_at(l->ox, l->oy, l->w, l->h);
        if (l->nv + 6 > MAXV) { l->overflow = 1; continue; }
        int ex = full ? ws_reveal : 0;
        add_batch(l, 0, -ex, 0, l->w - 1 + ex, l->h - 1);
        if (l->overflow) continue;
        float x0 = (float)(x - ex), x1 = (float)(x + w + ex), y0 = (float)y, y1 = (float)(y + h);
        uint32_t c = rgb & 0xFFFFFF;
        put(l, x0, y0, 0, 0, c, 0, 0, 0); put(l, x1, y0, 0, 0, c, 0, 0, 0); put(l, x0, y1, 0, 0, c, 0, 0, 0);
        put(l, x1, y0, 0, 0, c, 0, 0, 0); put(l, x0, y1, 0, 0, c, 0, 0, 0); put(l, x1, y1, 0, 0, c, 0, 0, 0);
        l->b[l->nb - 1].count += 6;
    }
}

// Escrituras directas en VRAM (transferencias, copias, vídeo): si caen en un búfer de pantalla,
// ese frame no se puede redibujar desde la lista y se muestra tal cual.
void hd_taint(int x, int y, int w, int h) {
    if (!lists) return;
    for (int i = 0; i < NLISTS; i++) {
        List *l = &lists[i];
        if (!l->used || l->displayed) continue;
        if (x < l->ox + l->w && x + w > l->ox && y < l->oy + l->h && y + h > l->oy) l->tainted = 1;
    }
}

// El juego pasa a mostrar el búfer de (x, y) (GP1 05): su frame está completo. Se guarda la VRAM
// de ese momento, porque enseguida empieza a subir texturas y paletas del frame siguiente y el
// redibujado HD necesita las de este.
void hd_display(int x, int y) {
    if (!lists) return;
    for (int i = 0; i < NLISTS; i++) {
        List *l = &lists[i];
        if (l->used && l->ox == x && l->oy == y && !l->snap && l->nv) {
            memcpy(l->vram, vram, sizeof l->vram);
            l->snap = 1;
        }
    }
}

// ---------------------------------------------------------------- capturas
static uint8_t cap_buf[1 << 23];
static int cap_len;

static inline uint32_t c15to24(uint16_t c) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (uint32_t)(r << 3 | r >> 2) | (uint32_t)(g << 3 | g >> 2) << 8 | (uint32_t)(b << 3 | b >> 2) << 16;
}

// Pinta la pieza `r` en el lienzo de contexto (cw x ch, origen en pantalla ox,oy).
static void paint(uint8_t *ctx, int cw, int ch, int ox, int oy, const Rec *r, const uint16_t *pal, int clear) {
    for (int j = 0; j < r->rh; j++)
        for (int i = 0; i < r->rw; i++) {
            int cx = r->x + i - ox, cy = r->y + j - oy;
            if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) continue;
            int ti = r->fx ? r->rw - 1 - i : i, tj = r->fy ? r->rh - 1 - j : j;
            uint16_t c = pal[r->px[tj * r->rw + ti]];
            uint8_t *o = ctx + (cy * cw + cx) * 4;
            if (!c) { if (clear) o[0] = o[1] = o[2] = o[3] = 0; continue; }
            uint32_t c24 = c15to24(c);
            o[0] = c24 & 255; o[1] = (c24 >> 8) & 255; o[2] = (c24 >> 16) & 255; o[3] = 255;
        }
}

// Destellos: el juego pinta a veces una pieza con una paleta casi de un solo color (blanco al
// recibir daño). Así la IA no ve el dibujo: se espera a verla con su paleta normal. Las
// paletas normales sí pueden repetir colores; lo que delata un destello es que muchos índices
// distintos queden en uno o dos colores.
static int palette_ok(const Rec *r) {
    uint8_t used[256] = {0};
    int n = 0, distinct = 0;
    uint16_t seen[256];
    for (int i = 0; i < r->rw * r->rh; i++) used[r->px[i]] = 1;
    for (int a = 0; a < 256; a++) {
        if (!used[a] || !r->pal[a]) continue;
        n++;
        int dup = 0;
        for (int k = 0; k < distinct; k++) if (seen[k] == r->pal[a]) dup = 1;
        if (!dup) seen[distinct++] = r->pal[a];
    }
    return n < 4 || distinct > 2;
}

// Las piezas nuevas se capturan por capas: una "capa" es una tanda de piezas seguidas en el
// orden de dibujo cuyos píxeles opacos no se tapan entre sí (un plano del fondo, un
// personaje...). Se manda la imagen de la capa entera alrededor de las piezas nuevas, la IA la
// mejora de una vez y luego se recortan las piezas: las uniones salen de la misma imagen y no
// quedan costuras. Registro "X4S1": u32 longitud, u16 ancho, alto, piezas, 0; lienzo RGBA;
// por pieza: u64 huella, i16 x, y, u8 ancho, alto, profundidad, volteo, u16 colores, 0;
// paleta, texels.
#define MARGIN 16  // contexto alrededor de las piezas nuevas
#define COV_W 768
#define COV_H 512
#define COV_X 192  // las piezas de los márgenes panorámicos tienen x negativa
static uint16_t cov[COV_H][COV_W], cov_run;

// Sin marcar: devuelve si la pieza tapa a las de la tanda en más de un cuarto de sus píxeles
// opacos (otra capa encima); los solapes pequeños son normales entre las piezas de un mismo
// sprite (filas que se pisan uno o dos píxeles) y no deben partirlo.
static int cov_test_mark(const Rec *r, int mark) {
    int opaque = 0, hit = 0;
    for (int j = 0; j < r->rh; j++)
        for (int i = 0; i < r->rw; i++) {
            int x = r->x + i + COV_X, y = r->y + j;
            if (x < 0 || y < 0 || x >= COV_W || y >= COV_H) continue;
            int ti = r->fx ? r->rw - 1 - i : i, tj = r->fy ? r->rh - 1 - j : j;
            if (!r->pal[r->px[tj * r->rw + ti]]) continue;
            opaque++;
            if (!mark && cov[y][x] == cov_run) hit++;
            if (mark) cov[y][x] = cov_run;
        }
    return !mark && hit * 4 > opaque;
}

// Piezas que entran por el borde de la pantalla (el fondo al desplazarse): se espera a que
// estén más adentro, con sus vecinas alrededor; si no, la IA les dibuja un borde que luego se
// ve como una línea en medio de la pantalla. Lo que nunca se aparta del borde se captura igual
// pasados EDGE_WAIT frames.
#define EW_SLOTS (1u << 16)
static uint64_t ew_key[EW_SLOTS];
static uint16_t ew_count[EW_SLOTS];
static int edge_ready(uint64_t h) {
    for (uint32_t i = slot_of(h) & (EW_SLOTS - 1), n = 0; n < EW_SLOTS; i = (i + 1) & (EW_SLOTS - 1), n++) {
        if (ew_key[i] == h) return ++ew_count[i] >= EDGE_WAIT;
        if (!ew_key[i]) { ew_key[i] = h; ew_count[i] = 1; return 0; }
    }
    return 1;  // tabla llena
}

static int is_new(const Rec *r) {
    if (!r->capture || set_has(known, r->hash) || !palette_ok(r)) return 0;
    return !r->edge || edge_ready(r->hash);
}

static void emit_scene(int cx0, int cy0, int cw, int ch, Rec *const *paintv, int np, Rec *const *tiles, int nt, int solo) {
    int need = 16 + cw * ch * 4;
    for (int k = 0; k < nt; k++) need += 20 + (tiles[k]->depth == 0 ? 16 : 256) * 2 + tiles[k]->rw * tiles[k]->rh;
    if (cap_len + need > (int)sizeof cap_buf) return;
    uint8_t *o = cap_buf + cap_len;
    uint32_t len = (uint32_t)need;
    uint16_t hdr[4] = {(uint16_t)cw, (uint16_t)ch, (uint16_t)nt, 0};
    memcpy(o, "X4S1", 4); memcpy(o + 4, &len, 4); memcpy(o + 8, hdr, 8);
    uint8_t *canvas = o + 16;
    memset(canvas, 0, (size_t)(cw * ch * 4));
    for (int k = 0; k < np; k++) {
        Rec self = *paintv[k];
        if (solo) { self.x = (int16_t)MARGIN; self.y = (int16_t)MARGIN; self.fx = self.fy = 0; }
        else { self.x = (int16_t)(self.x - cx0); self.y = (int16_t)(self.y - cy0); }
        paint(canvas, cw, ch, 0, 0, &self, self.pal, 0);
    }
    uint8_t *t = canvas + cw * ch * 4;
    for (int k = 0; k < nt; k++) {
        const Rec *r = tiles[k];
        int colors = r->depth == 0 ? 16 : 256;
        int16_t xy[2] = {(int16_t)(solo ? MARGIN : r->x - cx0), (int16_t)(solo ? MARGIN : r->y - cy0)};
        uint8_t b[4] = {r->rw, r->rh, r->depth, (uint8_t)(solo ? 0 : r->fx | r->fy << 1)};
        uint16_t c2[2] = {(uint16_t)colors, 0};
        memcpy(t, &r->hash, 8); memcpy(t + 8, xy, 4); memcpy(t + 12, b, 4); memcpy(t + 16, c2, 4);
        t += 20;
        memcpy(t, r->pal, (size_t)colors * 2); t += colors * 2;
        memcpy(t, r->px, (size_t)(r->rw * r->rh)); t += r->rw * r->rh;
        set_add(known, r->hash);
    }
    cap_len += need;
}

// Capa l->r[a..b): lienzo alrededor de sus piezas nuevas, con todas las de la capa pintadas.
static void capture_run(List *l, int a, int b) {
    static Rec *tiles[MAXR], *paintv[MAXR];
    int nt = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int k = a; k < b; k++) {
        Rec *r = &l->r[k];
        if (!is_new(r)) continue;
        tiles[nt++] = r;
        if (r->x < x0) x0 = r->x;
        if (r->y < y0) y0 = r->y;
        if (r->x + r->rw > x1) x1 = r->x + r->rw;
        if (r->y + r->rh > y1) y1 = r->y + r->rh;
    }
    if (!nt) return;
    x0 -= MARGIN; y0 -= MARGIN; x1 += MARGIN; y1 += MARGIN;
    int np = 0;
    for (int k = a; k < b; k++) {
        Rec *r = &l->r[k];
        if (r->x < x1 && r->y < y1 && r->x + r->rw > x0 && r->y + r->rh > y0) paintv[np++] = r;
    }
    emit_scene(x0, y0, x1 - x0, y1 - y0, paintv, np, tiles, nt, 0);
}

static void capture_frame(List *l) {
    int start = 0;
    if (++cov_run == 0) { memset(cov, 0, sizeof cov); cov_run = 1; }
    for (int k = 0; k < l->nr; k++) {
        Rec *r = &l->r[k];
        if (!r->axis) {
            // Pieza girada: sola, en su orientación, y corta la capa.
            capture_run(l, start, k);
            start = k + 1;
            if (++cov_run == 0) { memset(cov, 0, sizeof cov); cov_run = 1; }
            if (is_new(r)) {
                Rec *one = r;
                emit_scene(0, 0, r->rw + 2 * MARGIN, r->rh + 2 * MARGIN, &one, 1, &one, 1, 1);
            }
            continue;
        }
        const Rec *first = &l->r[start];
        if ((k > start && (first->sx != r->sx || first->sy != r->sy)) || cov_test_mark(r, 0)) {
            // Tapa a otra de la tanda, o va a otra escala: empieza una capa nueva.
            capture_run(l, start, k);
            start = k;
            if (++cov_run == 0) { memset(cov, 0, sizeof cov); cov_run = 1; }
        }
        cov_test_mark(r, 1);
    }
    capture_run(l, start, l->nr);
}

// ---------------------------------------------------------------- frame mostrado
int hd_frame(int disp_x, int disp_y, HdOut *out) {
    if (!lists) return 0;
    List *l = NULL;
    for (int i = 0; i < NLISTS; i++)
        if (lists[i].used && lists[i].ox == disp_x && lists[i].oy == disp_y) l = &lists[i];
    static long st[4];
    if (getenv("X4_HDSTAT") && frame_count % 300 == 0)
        fprintf(stderr, "[hd] frame %ld: ok %ld, sin lista %ld, manchado %ld, lleno %ld; ids %d\n", frame_count, st[0], st[1], st[2], st[3], next_id);
    if (!l) { st[1]++; return 0; }
    l->displayed = 1;
    if (!l->captured && hd_capture_on) {
        l->captured = 1;
        cap_len = 0;
        capture_frame(l);
        if (cap_len) hd_emit_captures(cap_buf, cap_len);
    }
    if (l->tainted || l->overflow || !l->nv) { st[l->tainted ? 2 : 3]++; return 0; }
    st[0]++;
    out->v = l->v; out->nv = l->nv;
    out->b = l->b; out->nb = l->nb;
    out->vram = l->snap ? l->vram : vram;
    out->pal = &l->pal[0][0]; out->np = l->np;
    return 1;
}
