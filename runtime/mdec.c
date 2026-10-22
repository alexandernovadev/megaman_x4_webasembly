// MDEC: decodificador de macrobloques (videos STR). Registros 0x1F801820/0x1F801824,
// DMA0 (entrada) y DMA1 (salida). Referencia: psx-spx, "MDEC".
#include <stdio.h>
#include <string.h>

#include "runtime.h"

#define LOG(...) do { if (log_level) fprintf(stderr, __VA_ARGS__); } while (0)

static const uint8_t zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static uint8_t zagzig[64];

static uint8_t qt_y[64], qt_c[64];
static int16_t scale_table[64];

// Comando en curso y sus parámetros.
static uint32_t cmd;
static uint32_t in_buf[0x40000];
static uint32_t in_need, in_have;

// Salida decodificada.
static uint32_t out_buf[0x40000];
static uint32_t out_len, out_pos;

static inline int sign10(int v) { return (v << 22) >> 22; }

// Decodifica un bloque 8x8 de coeficientes; devuelve el índice de media palabra siguiente.
static const uint16_t *decode_block(int16_t *blk, const uint16_t *src, const uint16_t *end, const uint8_t *qt) {
    memset(blk, 0, 64 * sizeof(int16_t));
    while (src < end && *src == 0xFE00) src++;
    if (src >= end) return end;
    uint16_t n = *src++;
    int k = 0;
    int q = n >> 10;
    int val = sign10(n & 0x3FF) * qt[0];
    for (;;) {
        if (q == 0) val = sign10(n & 0x3FF) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        if (q > 0) blk[zagzig[k]] = (int16_t)val;
        else blk[k] = (int16_t)val;
        if (src >= end) break;
        n = *src++;
        k += (n >> 10) + 1;
        if (k > 63) break;
        val = (sign10(n & 0x3FF) * qt[k] * q + 4) / 8;
    }
    // IDCT con la tabla de escala del juego (dos pasadas, como el hardware).
    int64_t tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++) sum += (int64_t)blk[u * 8 + x] * scale_table[u * 8 + y];
            tmp[x + y * 8] = sum;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++) sum += tmp[u + y * 8] * scale_table[u * 8 + x];
            int v = (int)((sum >> 32) + ((sum >> 31) & 1));
            v = (v << 23) >> 23;  // extensión de signo de 9 bits
            if (v < -128) v = -128;
            if (v > 127) v = 127;
            blk[x + y * 8] = (int16_t)v;
        }
    return src;
}

static void put_rgb(int r, int g, int b, int depth, int is_signed, int set15, uint8_t *rgb24, uint32_t *n24) {
    if (!is_signed) { r ^= 0x80; g ^= 0x80; b ^= 0x80; }
    r &= 255; g &= 255; b &= 255;
    if (depth == 2) {
        rgb24[(*n24)++] = (uint8_t)r;
        rgb24[(*n24)++] = (uint8_t)g;
        rgb24[(*n24)++] = (uint8_t)b;
    } else {
        uint16_t p = (uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | (set15 ? 0x8000 : 0));
        rgb24[(*n24)++] = p & 255;
        rgb24[(*n24)++] = p >> 8;
    }
}

static void decode_macroblocks(void) {
    int depth = (cmd >> 27) & 3, is_signed = (cmd >> 26) & 1, set15 = (cmd >> 25) & 1;
    const uint16_t *src = (const uint16_t *)in_buf, *end = src + in_have * 2;
    static uint8_t bytes[sizeof out_buf];
    uint32_t nb = 0;
    int16_t cr[64], cb[64], y[4][64];
    if (depth < 2) {
        static int warned;
        if (!warned++) LOG("[mdec] salida monocroma (%d) sin implementar\n", depth);
        out_len = out_pos = 0;
        return;
    }
    while (src < end) {
        src = decode_block(cr, src, end, qt_c);
        src = decode_block(cb, src, end, qt_c);
        for (int i = 0; i < 4; i++) src = decode_block(y[i], src, end, qt_y);
        if (nb + 16 * 16 * 3 > sizeof bytes) break;
        for (int py = 0; py < 16; py++)
            for (int px = 0; px < 16; px++) {
                int R = cr[(px / 2) + (py / 2) * 8], B = cb[(px / 2) + (py / 2) * 8];
                int G = (-88 * B - 183 * R) >> 8;  // -0.3437*B - 0.7143*R
                R = (359 * R) >> 8;                // 1.402*R
                B = (454 * B) >> 8;                // 1.772*B
                int Y = y[(py / 8) * 2 + (px / 8)][(px & 7) + (py & 7) * 8];
                int r = Y + R, g = Y + G, b = Y + B;
                r = r < -128 ? -128 : r > 127 ? 127 : r;
                g = g < -128 ? -128 : g > 127 ? 127 : g;
                b = b < -128 ? -128 : b > 127 ? 127 : b;
                put_rgb(r, g, b, depth, is_signed, set15, bytes, &nb);
            }
    }
    memcpy(out_buf, bytes, nb);
    out_len = (nb + 3) / 4;
    out_pos = 0;
}

static void execute(void) {
    switch (cmd >> 29) {
        case 1: decode_macroblocks(); break;
        case 2: {  // tablas de cuantización
            const uint8_t *b = (const uint8_t *)in_buf;
            memcpy(qt_y, b, 64);
            if (cmd & 1) memcpy(qt_c, b + 64, 64);
            break;
        }
        case 3: memcpy(scale_table, in_buf, 128); break;
    }
}

static void param(uint32_t w) {
    if (in_have < sizeof in_buf / 4) in_buf[in_have++] = w;
    if (in_have >= in_need) { execute(); in_need = in_have = 0; }
}

void mdec_write(int reg, uint32_t v) {
    if (!zagzig[1]) for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
    if (reg == 0) {
        if (in_need) { param(v); return; }
        cmd = v;
        in_have = 0;
        switch (v >> 29) {
            case 1: in_need = v & 0xFFFF; break;
            case 2: in_need = (v & 1) ? 32 : 16; break;
            case 3: in_need = 32; break;
            default: in_need = 0; break;
        }
        if (!in_need) execute();
    } else if (v & 0x80000000u) {  // reset
        in_need = in_have = 0;
        out_len = out_pos = 0;
    }
}

uint32_t mdec_read(int reg) {
    if (reg == 0) return out_pos < out_len ? out_buf[out_pos++] : 0;
    uint32_t s = 0;
    if (out_pos >= out_len) s |= 1u << 31;  // FIFO de salida vacía
    if (in_need) s |= 1u << 29 | 1u << 28;  // ocupado, pide datos
    if (out_pos < out_len) s |= 1u << 27;   // hay datos de salida
    s |= (in_need - in_have - 1) & 0xFFFF;
    return s;
}

void mdec_dma_in(uint32_t madr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w;
        memcpy(&w, ram + ((madr + i * 4) & 0x1FFFFC), 4);
        mdec_write(0, w);
    }
}

void mdec_dma_out(uint32_t madr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = out_pos < out_len ? out_buf[out_pos++] : 0;
        memcpy(ram + ((madr + i * 4) & 0x1FFFFC), &w, 4);
    }
}
