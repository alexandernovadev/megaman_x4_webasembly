// Audio XA del CD (música y voces): decodificador ADPCM y remuestreo a 44,1 kHz, más la
// cola de salida que la SPU llena ya mezclada (spu.c) y la plataforma vacía.
// Referencia: psx-spx, "CDROM XA Audio ADPCM Compression".
#include "runtime.h"

#define RING 32768  // muestras estéreo (~0,74 s a 44,1 kHz)

typedef struct { int16_t s[RING][2]; unsigned r, w; } Ring;
static Ring xa, out;

static void ring_put(Ring *q, int16_t l, int16_t r) {
    if (q->w - q->r >= RING) return;
    q->s[q->w % RING][0] = l;
    q->s[q->w % RING][1] = r;
    q->w++;
}

static const int K0[4] = {0, 60, 115, 98}, K1[4] = {0, 0, -52, -55};
static int hist[2][2];  // [canal][s-1, s-2]

// Remuestreo lineal de la frecuencia del sector a AUDIO_RATE.
static double phase;
static int16_t last[2];

static void emit(int16_t l, int16_t r, int in_rate) {
    double step = (double)in_rate / AUDIO_RATE;
    while (phase < 1.0) {
        ring_put(&xa, (int16_t)(last[0] + (l - last[0]) * phase), (int16_t)(last[1] + (r - last[1]) * phase));
        phase += step;
    }
    phase -= 1.0;
    last[0] = l;
    last[1] = r;
}

static int16_t decode_nibble(int nib, int shift, int filter, int *h) {
    int s = (int16_t)(nib << 12) >> shift;
    s += (h[0] * K0[filter] + h[1] * K1[filter] + 32) >> 6;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    h[1] = h[0];
    h[0] = s;
    return (int16_t)s;
}

// `sector` es el sector crudo de 2352 bytes; el subencabezado está en el byte 16.
void xa_decode_sector(const uint8_t *sector) {
    uint8_t coding = sector[19];
    int stereo = coding & 1, rate = (coding & 4) ? 18900 : 37800;
    if (coding & 0x10) return;  // ADPCM de 8 bits: no lo usa este juego
    const uint8_t *data = sector + 24;
    static int16_t buf[2][28 * 8];
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = data + g * 128;
        int n[2] = {0, 0};
        for (int u = 0; u < 8; u++) {
            int param = grp[4 + u], shift = param & 15, filter = (param >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            for (int j = 0; j < 28; j++) {
                uint8_t b = grp[16 + j * 4 + u / 2];
                int nib = (u & 1) ? b >> 4 : b & 15;
                buf[ch][n[ch]++] = decode_nibble(nib, shift, filter, hist[ch]);
            }
        }
        if (stereo)
            for (int i = 0; i < n[0]; i++) emit(buf[0][i], buf[1][i], rate);
        else
            for (int i = 0; i < n[0]; i++) emit(buf[0][i], buf[0][i], rate);
    }
}

void xa_reset(void) {
    memset(hist, 0, sizeof hist);
}

// La SPU toma una muestra XA por cada muestra que genera. El XA llega a ráfagas (un sector
// son ~53 ms), así que tras vaciarse la cola espera a tener un sector entero de colchón.
int xa_pull(int16_t *l, int16_t *r) {
    static int primed;
    unsigned avail = xa.w - xa.r;
    if (!avail) { primed = 0; return 0; }
    if (!primed && avail < 2400) return 0;
    primed = 1;
    *l = xa.s[xa.r % RING][0];
    *r = xa.s[xa.r % RING][1];
    xa.r++;
    return 1;
}

void audio_out(int16_t l, int16_t r) { ring_put(&out, l, r); }

// La plataforma recoge las muestras listas (estéreo intercalado); devuelve cuántas.
int audio_take(int16_t *pcm, int max) {
    spu_sync();
    int n = 0;
    while (n < max && out.r != out.w) {
        pcm[n * 2] = out.s[out.r % RING][0];
        pcm[n * 2 + 1] = out.s[out.r % RING][1];
        out.r++;
        n++;
    }
    return n;
}
