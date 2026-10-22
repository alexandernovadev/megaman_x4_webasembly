// SPU de la PS1 a nivel de registros (0x1F801C00-0x1F801FFF): 24 voces ADPCM con envolvente
// ADSR, 512 KB de RAM de sonido y la entrada de audio del CD (XA). Genera a 44,1 kHz, la
// frecuencia nativa del chip, sincronizada con el reloj emulado (768 ciclos por muestra).
// Referencia: psx-spx, "Sound Processing Unit (SPU)".
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

#define CYCLES_PER_SAMPLE 768
#define NVOICES 24

static uint8_t spu_ram[512 * 1024];
static uint16_t regs[0x200];  // copia de lo escrito, en medias palabras
static uint32_t transfer_addr;
static uint32_t endx;
static uint64_t synced;  // ciclo hasta el que ya se generó audio

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    uint32_t addr, repeat;   // en bytes
    uint32_t counter;        // posición fraccionaria (12 bits) dentro del bloque
    int16_t block[28];
    int idx;                 // muestra actual dentro de `block`
    int hist[2];
    int16_t win[4];          // últimas muestras, para la interpolación de 4 puntos
    int env, level, wait;
} Voice;

static Voice voices[NVOICES];
static int16_t noise_level = 1;
static int noise_count;

static uint16_t vreg(int v, int r) { return regs[(v * 16 + r) >> 1]; }

// Volumen fijo (bit 15 = 0): 15 bits con signo, la mitad del rango. Los barridos se
// aproximan con su valor máximo.
static int vol(uint16_t r) { return (r & 0x8000) ? 0x7FFF : (int16_t)(r << 1); }

// ---------------------------------------------------------------- ADPCM
static const int K0[5] = {0, 60, 115, 98, 122}, K1[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice *v) {
    const uint8_t *b = spu_ram + (v->addr & 0x7FFF0);
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filter > 4) filter = 4;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int s = ((int16_t)(nib << 12) >> shift) + ((v->hist[0] * K0[filter] + v->hist[1] * K1[filter] + 32) >> 6);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v->hist[1] = v->hist[0];
        v->hist[0] = s;
        v->block[i] = (int16_t)s;
    }
}

// Pasa al siguiente bloque según las banderas del actual (fin / repetición / inicio de bucle).
static void next_block(int n) {
    Voice *v = &voices[n];
    uint8_t flags = spu_ram[(v->addr & 0x7FFF0) + 1];
    if (flags & 4) v->repeat = v->addr;
    if (flags & 1) {
        endx |= 1u << n;
        v->addr = v->repeat;
        if (!(flags & 2)) { v->env = ENV_OFF; v->level = 0; }  // fin sin bucle: se calla
    } else {
        v->addr = (v->addr + 16) & 0x7FFFF;
    }
    decode_block(v);
}

// ---------------------------------------------------------------- envolvente ADSR
static void env_step(Voice *v, int shift, int step, int exp, int dec) {
    if (v->wait > 0) { v->wait--; return; }
    int s = shift - 11 > 0 ? shift - 11 : 0;
    v->wait = (1 << s) - 1;
    int amt = step << (11 - shift > 0 ? 11 - shift : 0);
    if (exp && !dec && v->level > 0x6000) v->wait = v->wait * 4 + 3;
    if (exp && dec) amt = amt * v->level >> 15;
    v->level += amt;
    if (v->level > 0x7FFF) v->level = 0x7FFF;
    if (v->level < 0) v->level = 0;
}

static void envelope(int n) {
    Voice *v = &voices[n];
    uint16_t a1 = vreg(n, 8), a2 = vreg(n, 10);
    switch (v->env) {
        case ENV_ATTACK:
            env_step(v, (a1 >> 10) & 31, 7 - ((a1 >> 8) & 3), a1 >> 15, 0);
            if (v->level >= 0x7FFF) v->env = ENV_DECAY;
            break;
        case ENV_DECAY: {
            env_step(v, ((a1 >> 4) & 15) << 2, -8, 1, 1);
            int sustain = ((a1 & 15) + 1) * 0x800;
            if (v->level <= sustain) v->env = ENV_SUSTAIN;
            break;
        }
        case ENV_SUSTAIN: {
            int dec = (a2 >> 14) & 1, st = (a2 >> 6) & 3;
            env_step(v, (a2 >> 8) & 31, dec ? -8 + st : 7 - st, a2 >> 15, dec);
            break;
        }
        case ENV_RELEASE:
            env_step(v, (a2 & 31) << 2, -8, (a2 >> 5) & 1, 1);
            if (v->level == 0) v->env = ENV_OFF;
            break;
    }
}

static void key_on(uint32_t mask) {
    for (int n = 0; n < NVOICES; n++) {
        if (!(mask & (1u << n))) continue;
        Voice *v = &voices[n];
        v->addr = (uint32_t)vreg(n, 6) * 8;
        v->repeat = (uint32_t)vreg(n, 14) * 8;
        v->counter = 0;
        v->idx = 0;
        v->hist[0] = v->hist[1] = 0;
        memset(v->win, 0, sizeof v->win);
        v->win[3] = v->block[0];
        v->env = ENV_ATTACK;
        v->level = 0;
        v->wait = 0;
        endx &= ~(1u << n);
        decode_block(v);
        if (getenv("X4_SPULOG")) fprintf(stderr, "[spu] key on %d addr %05x pitch %04x adsr %04x %04x vol %04x %04x\n", n, v->addr, vreg(n, 4), vreg(n, 8), vreg(n, 10), vreg(n, 0), vreg(n, 2));
    }
}

static void key_off(uint32_t mask) {
    for (int n = 0; n < NVOICES; n++)
        if ((mask & (1u << n)) && voices[n].env != ENV_OFF) { voices[n].env = ENV_RELEASE; voices[n].wait = 0; }
}

// ---------------------------------------------------------------- interpolación
// La SPU interpola con 4 puntos y una curva gaussiana (tabla de 512 entradas en el chip).
// Aproximación: núcleo gaussiano con sigma 0,57, que reproduce los pesos de la tabla real
// en los extremos (~0,15 / 0,70 / 0,15 con fracción 0), normalizado.
static int16_t gauss[256][4];

static void gauss_init(void) {
    for (int f = 0; f < 256; f++) {
        double x = f / 256.0, w[4], sum = 0;
        for (int k = 0; k < 4; k++) { double d = x - (k - 1); w[k] = exp(-d * d / (2 * 0.57 * 0.57)); sum += w[k]; }
        for (int k = 0; k < 4; k++) gauss[f][k] = (int16_t)(w[k] / sum * 32767);
    }
}

static int interpolate(const int16_t *w, uint32_t counter) {
    const int16_t *g = gauss[(counter >> 4) & 255];
    return (w[0] * g[0] + w[1] * g[1] + w[2] * g[2] + w[3] * g[3]) >> 15;
}

// ---------------------------------------------------------------- reverb
// Se procesa a 22,05 kHz sobre un búfer circular en la RAM de sonido (de mBASE al final),
// con las direcciones relativas en unidades de 8 bytes. psx-spx, "SPU Reverb Formula".
enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME,
       mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3,
       mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };

static uint32_t rev_pos;  // posición actual dentro del búfer, en bytes
static int rev_in_l, rev_in_r, rev_out_l, rev_out_r, rev_phase;

static uint16_t rr(int i) { return regs[(0x1C0 >> 1) + i]; }
static int rv(int i) { return (int16_t)rr(i); }  // registros de volumen (con signo)

static uint32_t rev_addr(int32_t rel) {
    uint32_t base = (uint32_t)regs[0x1A2 >> 1] * 8, size = 0x80000 - base;
    if (!size) return base;
    int64_t off = ((int64_t)rev_pos - base + rel) % size;
    if (off < 0) off += size;
    return (base + (uint32_t)off) & 0x7FFFE;
}
static int rev_rd(int reg, int delta) {
    uint32_t a = rev_addr((int32_t)rr(reg) * 8 + delta);
    return (int16_t)(spu_ram[a] | spu_ram[a + 1] << 8);
}
static int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static void rev_wr(int reg, int v) {
    if (!(regs[0x1AA >> 1] & 0x80)) return;  // escritura en el búfer deshabilitada
    uint32_t a = rev_addr((int32_t)rr(reg) * 8);
    v = clamp16(v);
    spu_ram[a] = (uint8_t)v;
    spu_ram[a + 1] = (uint8_t)(v >> 8);
}
static int mul(int a, int b) { return a * b >> 15; }

static void reverb_tick(void) {
    int lin = mul(rev_in_l, rv(vLIN)), rin = mul(rev_in_r, rv(vRIN));
    int iir = rv(vIIR), wall = rv(vWALL);
    // Reflexiones del mismo lado y del contrario, con filtro IIR.
    int ls = rev_rd(mLSAME, -2), rs = rev_rd(mRSAME, -2), ld = rev_rd(mLDIFF, -2), rd = rev_rd(mRDIFF, -2);
    int nls = mul(lin + mul(rev_rd(dLSAME, 0), wall) - ls, iir) + ls;
    int nrs = mul(rin + mul(rev_rd(dRSAME, 0), wall) - rs, iir) + rs;
    int nld = mul(lin + mul(rev_rd(dRDIFF, 0), wall) - ld, iir) + ld;
    int nrd = mul(rin + mul(rev_rd(dLDIFF, 0), wall) - rd, iir) + rd;
    rev_wr(mLSAME, nls); rev_wr(mRSAME, nrs); rev_wr(mLDIFF, nld); rev_wr(mRDIFF, nrd);
    // Filtro peine.
    int l = mul(rv(vCOMB1), rev_rd(mLCOMB1, 0)) + mul(rv(vCOMB2), rev_rd(mLCOMB2, 0)) +
            mul(rv(vCOMB3), rev_rd(mLCOMB3, 0)) + mul(rv(vCOMB4), rev_rd(mLCOMB4, 0));
    int r = mul(rv(vCOMB1), rev_rd(mRCOMB1, 0)) + mul(rv(vCOMB2), rev_rd(mRCOMB2, 0)) +
            mul(rv(vCOMB3), rev_rd(mRCOMB3, 0)) + mul(rv(vCOMB4), rev_rd(mRCOMB4, 0));
    // Dos filtros pasa-todo.
    int d1 = (int32_t)rr(dAPF1) * -8, d2 = (int32_t)rr(dAPF2) * -8;
    int la = rev_rd(mLAPF1, d1), ra = rev_rd(mRAPF1, d1);
    l = clamp16(l - mul(rv(vAPF1), la)); r = clamp16(r - mul(rv(vAPF1), ra));
    rev_wr(mLAPF1, l); rev_wr(mRAPF1, r);
    l = mul(l, rv(vAPF1)) + la; r = mul(r, rv(vAPF1)) + ra;
    la = rev_rd(mLAPF2, d2); ra = rev_rd(mRAPF2, d2);
    l = clamp16(l - mul(rv(vAPF2), la)); r = clamp16(r - mul(rv(vAPF2), ra));
    rev_wr(mLAPF2, l); rev_wr(mRAPF2, r);
    l = mul(l, rv(vAPF2)) + la; r = mul(r, rv(vAPF2)) + ra;
    rev_out_l = mul(clamp16(l), (int16_t)regs[0x184 >> 1]);
    rev_out_r = mul(clamp16(r), (int16_t)regs[0x186 >> 1]);
    uint32_t base = (uint32_t)regs[0x1A2 >> 1] * 8;
    rev_pos = (rev_pos + 2) & 0x7FFFE;
    if (rev_pos < base) rev_pos = base;
}

// ---------------------------------------------------------------- mezcla
static void noise_tick(void) {
    uint16_t cnt = regs[0x1AA >> 1];
    int shift = (cnt >> 10) & 15, step = ((cnt >> 8) & 3) + 4;
    noise_count -= step;
    if (noise_count >= 0) return;
    noise_count += 0x20000 >> shift;
    int bit = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
    noise_level = (int16_t)((noise_level << 1) | bit);
}

static void render_sample(void) {
    uint16_t cnt = regs[0x1AA >> 1];
    uint32_t pmon = regs[0x190 >> 1] | (uint32_t)regs[0x192 >> 1] << 16;
    uint32_t non = regs[0x194 >> 1] | (uint32_t)regs[0x196 >> 1] << 16;
    uint32_t eon = regs[0x198 >> 1] | (uint32_t)regs[0x19A >> 1] << 16;
    int l = 0, r = 0, el = 0, er = 0, last_out = 0;
    noise_tick();
    for (int n = 0; n < NVOICES; n++) {
        Voice *v = &voices[n];
        if (v->env == ENV_OFF) { last_out = 0; continue; }
        int s = (non & (1u << n)) ? noise_level : interpolate(v->win, v->counter);
        envelope(n);
        s = s * v->level >> 15;
        int vl = s * vol(vreg(n, 0)) >> 15, vr = s * vol(vreg(n, 2)) >> 15;
        l += vl;
        r += vr;
        if (eon & (1u << n)) { el += vl; er += vr; }
        // Avance según el tono (4096 = 44,1 kHz); con modulación usa la voz anterior.
        int pitch = vreg(n, 4);
        if (n > 0 && (pmon & (1u << n))) pitch = pitch * (last_out + 0x8000) >> 15;
        if (pitch > 0x4000) pitch = 0x4000;
        last_out = s;
        v->counter += pitch;
        while (v->counter >= 0x1000) {
            v->counter -= 0x1000;
            if (++v->idx == 28) { v->idx = 0; next_block(n); if (v->env == ENV_OFF) break; }
            memmove(v->win, v->win + 1, 3 * sizeof v->win[0]);
            v->win[3] = v->block[v->idx];
        }
    }
    // Entrada del CD (música y voces XA) con su propio volumen; puede ir también a la reverb.
    int16_t xl, xr;
    int cl = 0, cr = 0;
    if (xa_pull(&xl, &xr) && (cnt & 1)) {
        cl = xl * (int16_t)regs[0x1B0 >> 1] >> 15;
        cr = xr * (int16_t)regs[0x1B2 >> 1] >> 15;
        if (cnt & 4) { el += cl; er += cr; }
    }
    // La reverb trabaja a la mitad de frecuencia: acumula dos muestras de entrada.
    rev_in_l += el; rev_in_r += er;
    if (++rev_phase == 2) {
        rev_phase = 0;
        rev_in_l = clamp16(rev_in_l / 2); rev_in_r = clamp16(rev_in_r / 2);
        reverb_tick();
        rev_in_l = rev_in_r = 0;
    }
    l = (l + rev_out_l) * vol(regs[0x180 >> 1]) >> 15;
    r = (r + rev_out_r) * vol(regs[0x182 >> 1]) >> 15;
    l += cl;
    r += cr;
    if (!(cnt & 0x4000)) l = r = 0;  // silenciado
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    audio_out((int16_t)l, (int16_t)r);
}

// Genera el audio pendiente hasta el ciclo actual.
void spu_sync(void) {
    static int init;
    if (!init) { init = 1; gauss_init(); }
    while (synced + CYCLES_PER_SAMPLE <= cycles) {
        synced += CYCLES_PER_SAMPLE;
        render_sample();
    }
}

// ---------------------------------------------------------------- registros
uint32_t spu_read(uint32_t off, int size) {
    spu_sync();
    off &= 0x3FE;
    uint32_t v;
    if (off < 0x180 && (off & 15) == 12) v = (uint32_t)voices[off >> 4].level;     // volumen ADSR actual
    else if (off < 0x180 && (off & 15) == 14) v = voices[off >> 4].repeat / 8;
    else if (off == 0x19C) v = endx & 0xFFFF;
    else if (off == 0x19E) v = endx >> 16;
    else if (off == 0x1AE) v = regs[0x1AA >> 1] & 0x3F;  // SPUSTAT: nunca ocupada
    else v = regs[off >> 1];
    if (size == 4) v |= (uint32_t)spu_read(off + 2, 2) << 16;
    return v;
}

void spu_write(uint32_t off, uint32_t v, int size) {
    spu_sync();
    off &= 0x3FE;
    if (size == 4) { spu_write(off, v & 0xFFFF, 2); spu_write(off + 2, v >> 16, 2); return; }
    v &= 0xFFFF;
    regs[off >> 1] = (uint16_t)v;
    switch (off) {
        case 0x188: key_on(v); break;
        case 0x18A: key_on(v << 16); break;
        case 0x18C: key_off(v); break;
        case 0x18E: key_off(v << 16); break;
        case 0x1A6: transfer_addr = v * 8; break;
        case 0x1A2: rev_pos = v * 8; break;  // nuevo búfer de reverb
        case 0x1A8:  // FIFO de transferencia manual
            spu_ram[transfer_addr & 0x7FFFF] = (uint8_t)v;
            spu_ram[(transfer_addr + 1) & 0x7FFFF] = (uint8_t)(v >> 8);
            transfer_addr = (transfer_addr + 2) & 0x7FFFF;
            break;
    }
}

// DMA canal 4: RAM <-> RAM de sonido.
void spu_dma(uint32_t madr, uint32_t words, int to_spu) {
    for (uint32_t i = 0; i < words * 4; i++) {
        uint8_t *m = &ram[(madr + i) & 0x1FFFFF];
        uint8_t *s = &spu_ram[(transfer_addr + i) & 0x7FFFF];
        if (to_spu) *s = *m; else *m = *s;
    }
    transfer_addr = (transfer_addr + words * 4) & 0x7FFFF;
}
