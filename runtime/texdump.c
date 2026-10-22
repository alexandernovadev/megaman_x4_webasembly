// Modo "grabar" (solo nativo, X4_TEXDUMP=carpeta): guarda una vez cada trozo de textura que el
// juego dibuja, para el HD con IA. La clave es el contenido (índices de color del rectángulo
// de VRAM usado), no su posición: el juego sube los fotogramas de los personajes a VRAM sobre
// la marcha. Cada combinación forma+paleta se guarda en <carpeta>/<forma>_<paleta>.tex:
//   "X4T2", u16 ancho, u16 alto, u16 profundidad (0=4 bits, 1=8, 2=15), u16 colores,
//   u32 primer frame, u16 página x, u16 página y, u8 u, u8 v, u16 clut x, u16 clut y,
//   u16 paleta[colores], y los texels (índices de 1 byte, o u16 en 15 bits).
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

static const char *dir;
static int inited;
static long shapes, combos;

#define SLOTS (1 << 20)
static uint64_t *seen_combo, *seen_shape;

static int insert(uint64_t *t, uint64_t h) {
    if (!h) h = 1;
    for (uint32_t i = (uint32_t)h & (SLOTS - 1);; i = (i + 1) & (SLOTS - 1)) {
        if (t[i] == h) return 0;
        if (!t[i]) { t[i] = h; return 1; }
    }
}

static uint64_t fnv(uint64_t h, uint32_t v) { return (h ^ v) * 0x100000001b3ULL; }

void tex_record(int depth, int tpx, int tpy, int cx, int cy, int u0, int v0, int w, int h) {
    if (!inited) {
        inited = 1;
        dir = getenv("X4_TEXDUMP");
        if (dir) { seen_combo = calloc(SLOTS, 8); seen_shape = calloc(SLOTS, 8); }
    }
    if (!dir || w <= 0 || h <= 0 || w > 256 || h > 256 || shapes > SLOTS / 2) return;
    static uint16_t px[256 * 256];
    uint64_t sh = 0xcbf29ce484222325ULL ^ (uint64_t)(depth << 20 | w << 10 | h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int u = (u0 + i) & 255, y = (tpy + ((v0 + j) & 255)) & 511, p;
            if (depth == 0) p = (vram[y * 1024 + ((tpx + u / 4) & 1023)] >> ((u & 3) * 4)) & 15;
            else if (depth == 1) p = (vram[y * 1024 + ((tpx + u / 2) & 1023)] >> ((u & 1) * 8)) & 255;
            else p = vram[y * 1024 + ((tpx + u) & 1023)];
            px[j * w + i] = (uint16_t)p;
            sh = fnv(sh, (uint32_t)p);
        }
    int colors = depth == 0 ? 16 : depth == 1 ? 256 : 0;
    uint64_t ph = 0xcbf29ce484222325ULL;
    for (int i = 0; i < colors; i++) ph = fnv(ph, vram[cy * 1024 + ((cx + i) & 1023)]);
    if (!insert(seen_combo, sh ^ (ph * 31))) return;
    combos++;
    shapes += insert(seen_shape, sh);
    char path[512];
    snprintf(path, sizeof path, "%s/%016llx_%08x.tex", dir, (unsigned long long)sh, (uint32_t)(ph ^ (ph >> 32)));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint16_t hdr[4] = {(uint16_t)w, (uint16_t)h, (uint16_t)depth, (uint16_t)colors};
    uint32_t fr = (uint32_t)frame_count;
    uint16_t where[4] = {(uint16_t)tpx, (uint16_t)tpy, (uint16_t)(u0 | v0 << 8), 0};
    uint16_t clut[2] = {(uint16_t)cx, (uint16_t)cy};
    fwrite("X4T2", 1, 4, f); fwrite(hdr, 2, 4, f); fwrite(&fr, 4, 1, f);
    fwrite(where, 2, 3, f); fwrite(clut, 2, 2, f);
    for (int i = 0; i < colors; i++) fwrite(&vram[cy * 1024 + ((cx + i) & 1023)], 2, 1, f);
    for (int i = 0; i < w * h; i++) {
        if (depth == 2) fwrite(&px[i], 2, 1, f);
        else { uint8_t b = (uint8_t)px[i]; fwrite(&b, 1, 1, f); }
    }
    fclose(f);
    if (combos % 500 == 0) fprintf(stderr, "[texdump] %ld formas, %ld con paleta (frame %ld)\n", shapes, combos, frame_count);
}

// Poses armadas (X4_POSECLUT="x,y"): junta en una imagen, por frame, todas las piezas dibujadas
// con esa paleta, en sus posiciones de pantalla. Es lo que se ve (p. ej. X entero), y es lo que
// conviene pasar por la IA para que no queden costuras entre piezas. Se guardan en
// <carpeta>/pose_<huella>.tex con el mismo formato (página 0, uv = esquina en pantalla).
#define PW 256
static uint8_t pose[PW * PW];
static int pose_on = -1, pose_cx, pose_cy, pose_n, pose_x0, pose_y0, pose_x1, pose_y1, pose_ox, pose_oy;
static uint16_t pose_pal[16];
static long poses;

static int pose_enabled(void) {
    if (pose_on < 0) {
        const char *s = getenv("X4_POSECLUT");
        pose_on = s && getenv("X4_TEXDUMP") && sscanf(s, "%d,%d", &pose_cx, &pose_cy) == 2;
    }
    return pose_on;
}

// Una pieza de 4 bits: rectángulo de pantalla (x,y,w,h) que lee la textura desde (u0,v0)
// avanzando su/sv por píxel (-1 si está volteada).
void tex_pose_piece(int tpx, int tpy, int cx, int cy, int x, int y, int w, int h, int u0, int v0, int su, int sv) {
    if (!pose_enabled() || cx != pose_cx || cy != pose_cy || w <= 0 || h <= 0 || w > PW || h > PW) return;
    if (!pose_n) {
        memset(pose, 0, sizeof pose);
        pose_ox = x - PW / 2; pose_oy = y - PW / 2;
        pose_x0 = pose_y0 = PW; pose_x1 = pose_y1 = 0;
        for (int i = 0; i < 16; i++) pose_pal[i] = vram[cy * 1024 + ((cx + i) & 1023)];
    }
    pose_n++;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x - pose_ox + i, py = y - pose_oy + j;
            if (px < 0 || py < 0 || px >= PW || py >= PW) continue;
            int u = (u0 + i * su) & 255, v = (v0 + j * sv) & 255, yy = (tpy + v) & 511;
            int idx = (vram[yy * 1024 + ((tpx + u / 4) & 1023)] >> ((u & 3) * 4)) & 15;
            if (!idx) continue;
            pose[py * PW + px] = (uint8_t)idx;
            if (px < pose_x0) pose_x0 = px;
            if (py < pose_y0) pose_y0 = py;
            if (px >= pose_x1) pose_x1 = px + 1;
            if (py >= pose_y1) pose_y1 = py + 1;
        }
}

// Fin de frame: guarda la pose si es nueva.
void tex_pose_frame(void) {
    if (!pose_enabled() || !pose_n) return;
    pose_n = 0;
    if (pose_x1 <= pose_x0) return;
    int w = pose_x1 - pose_x0, h = pose_y1 - pose_y0;
    uint64_t hh = 0xcbf29ce484222325ULL ^ (uint64_t)(w << 10 | h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) hh = fnv(hh, pose[(pose_y0 + j) * PW + pose_x0 + i]);
    if (!insert(seen_shape, hh ^ 0x9e3779b97f4a7c15ULL)) return;
    poses++;
    char path[512];
    snprintf(path, sizeof path, "%s/pose_%016llx.tex", dir, (unsigned long long)hh);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint16_t hdr[4] = {(uint16_t)w, (uint16_t)h, 0, 16};
    uint32_t fr = (uint32_t)frame_count;
    uint16_t where[3] = {0, 0, 0}, clut[2] = {(uint16_t)pose_cx, (uint16_t)pose_cy};
    fwrite("X4T2", 1, 4, f); fwrite(hdr, 2, 4, f); fwrite(&fr, 4, 1, f);
    fwrite(where, 2, 3, f); fwrite(clut, 2, 2, f); fwrite(pose_pal, 2, 16, f);
    for (int j = 0; j < h; j++) fwrite(&pose[(pose_y0 + j) * PW + pose_x0], 1, (size_t)w, f);
    fclose(f);
}

void tex_summary(void) {
    if (dir && poses) fprintf(stderr, "[texdump] %ld poses distintas\n", poses);
    if (dir) fprintf(stderr, "[texdump] total: %ld formas distintas, %ld combinaciones forma+paleta\n", shapes, combos);
}
