// Plataforma web: corre dentro de un Web Worker (ver web/worker.js). El disco del usuario
// llega montado con WORKERFS, cada frame se envía al hilo principal y el ritmo se marca
// durmiendo con Asyncify hasta la hora del siguiente vblank.
#include <emscripten.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"
#include "hd.h"

extern long max_frames;

// 60 Hz justos: el reloj emulado da 564480 ciclos por frame y la SPU 735 muestras por frame,
// así que a este ritmo el audio sale exactamente a 44,1 kHz.
#define FRAME_MS (1000.0 / 60.0)

static uint16_t web_pad;
static int paused;
// Ritmo marcado por la pantalla: la página concede "créditos" (frames del juego) desde
// requestAnimationFrame; -1 = sin créditos aún, se usa el reloj propio.
static int credits = -1;
EMSCRIPTEN_KEEPALIVE void x4_add_credits(int n) {
    if (credits < 0) credits = 0;
    credits += n;
    if (credits > 3) credits = 3;  // no acumular ráfagas (pestaña en segundo plano)
}

EMSCRIPTEN_KEEPALIVE void x4_set_pad(int buttons) { web_pad = (uint16_t)buttons; }
EMSCRIPTEN_KEEPALIVE void x4_set_paused(int p) { paused = p; }
EMSCRIPTEN_KEEPALIVE void x4_set_widescreen(int on) { ws_set(on); }
EMSCRIPTEN_KEEPALIVE void x4_set_aa(int on) { extern int gpu_aa; gpu_aa = on; }
// Tarjeta de memoria: la página copia aquí la guardada antes de arrancar (o pide una nueva).
EMSCRIPTEN_KEEPALIVE uint8_t *x4_card(void) { return memcard; }
EMSCRIPTEN_KEEPALIVE void x4_card_new(void) { memcard_new(); }
EMSCRIPTEN_KEEPALIVE void x4_test_save(int on) { card_test_save = on; }
EMSCRIPTEN_KEEPALIVE void x4_full_toggle(void) { cheat_full_toggle(); }

// HD (ver hd.c y web/hd.js): la página lo activa si su GPU puede y si el servidor local tiene
// el paquete HD; entonces cada frame se manda como lista de dibujo en vez de como imagen.
static int hd_on;
static uint32_t hd_known_buf[65536 * 2];
EMSCRIPTEN_KEEPALIVE void x4_hd_enable(int on, int capture) { hd_on = on; hd_capture_on = capture; }
EMSCRIPTEN_KEEPALIVE uint32_t *x4_hd_buf(void) { return hd_known_buf; }
EMSCRIPTEN_KEEPALIVE void x4_hd_known(int n) { hd_known(hd_known_buf, n); }

EM_JS(void, js_hd_frame, (const void *v, int nv, const void *b, int nb, const uint16_t *vr, const uint16_t *pal, int np,
                          const uint32_t *ids, const uint8_t *px, int nids, int w, int h, int margin, long frame), {
    if (typeof WorkerGlobalScope === 'undefined') return;
    const verts = HEAPU8.slice(v, v + nv * 32), batches = HEAPU8.slice(b, b + nb * 28);
    const vram = HEAPU8.slice(vr, vr + 1024 * 512 * 2), pals = HEAPU8.slice(pal, pal + np * 512);
    const idl = HEAPU8.slice(ids, ids + nids * 12), idpx = HEAPU8.slice(px, px + nids * 256);
    postMessage({type: 'hd', w, h, margin, frame, nv, nb, np, verts: verts.buffer, batches: batches.buffer,
                 vram: vram.buffer, pals: pals.buffer, ids: idl.buffer, idpx: idpx.buffer},
                [verts.buffer, batches.buffer, vram.buffer, pals.buffer, idl.buffer, idpx.buffer]);
});

EM_JS(void, js_hd_capture, (const uint8_t *buf, int len), {
    if (typeof WorkerGlobalScope === 'undefined') return;
    fetch('hd/capture', {method: 'POST', body: HEAPU8.slice(buf, buf + len)}).catch(() => {});
});
void hd_emit_captures(const uint8_t *buf, int len) { js_hd_capture(buf, len); }

EM_JS(void, js_card_save, (const uint8_t *card, int n), {
    if (typeof WorkerGlobalScope === 'undefined') return;
    const b = HEAPU8.slice(card, card + n);
    postMessage({type: 'card', buf: b.buffer}, [b.buffer]);
});

EM_JS(void, js_present, (const uint8_t *rgba, int w, int h, long frame, int video, int wide), {
    if (typeof WorkerGlobalScope === 'undefined') return;  // pruebas en Node
    const px = HEAPU8.slice(rgba, rgba + w * h * 4);
    postMessage({type: 'frame', w, h, frame, video, wide, buf: px.buffer}, [px.buffer]);
});

// Caja de diálogo en pantalla (voice.c): la página pone su voz y su subtítulo.
EM_JS(void, js_voice, (uint32_t key), {
    if (typeof WorkerGlobalScope === 'undefined') return;
    postMessage({type: 'voice', key: (key >>> 0).toString(16).padStart(8, '0')});
});

// Selección de personaje y modo Completo (cheats.c), para el aviso de la página.
EM_JS(void, js_cheat_state, (int state), {
    if (typeof WorkerGlobalScope === 'undefined') return;
    postMessage({type: 'cheat', state});
});

EM_JS(void, js_audio, (const int16_t *pcm, int n), {
    if (typeof WorkerGlobalScope === 'undefined') return;
    const s = HEAP16.slice(pcm >> 1, (pcm >> 1) + n * 2);
    postMessage({type: 'audio', buf: s.buffer}, [s.buffer]);
});

void frame_hook(long frame) {
    static uint8_t rgba[640 * 480 * 4];
    static double t0;
    static long f0;
    int w, h;
    gpu_framebuffer(rgba, &w, &h);
    extern int disp_24bit, disp_x, disp_y;  // los vídeos MDEC usan el modo de pantalla de 24 bits
    HdOut o;
    if (!disp_24bit && hd_frame(disp_x, disp_y, &o) && hd_on) {
        js_hd_frame(o.v, o.nv, o.b, o.nb, o.vram, o.pal, o.np, hd_new_ids, hd_new_px, hd_new_n, w, h, gpu_wide_frame ? ws_reveal : 0, frame);
        hd_new_n = 0;
    } else {
        js_present(rgba, w, h, frame, disp_24bit, gpu_wide_frame);
    }
    static int16_t pcm[4096 * 2];
    int n = audio_take(pcm, 4096);
    if (n) js_audio(pcm, n);
    if (memcard_dirty) { memcard_dirty = 0; js_card_save(memcard, sizeof memcard); }
    static uint32_t last_voice;
    uint32_t vk = voice_frame();
    if (vk != last_voice) { last_voice = vk; js_voice(vk); }
    static int last_cheat = -1;
    int cs = cheat_state();
    if (cs != last_cheat) { last_cheat = cs; js_cheat_state(cs); }
    // Dormir también procesa los mensajes del hilo principal (mando, pausa, créditos).
    if (credits >= 0) {
        while (credits == 0 || paused) emscripten_sleep(paused ? 50 : 1);
        credits--;
        pad_buttons[0] = web_pad;
        return;
    }
    do {
        double now = emscripten_get_now();
        // Si vamos más de 100 ms tarde (pestaña en segundo plano, pausa), se resincroniza
        // en vez de correr para recuperar.
        if (!t0 || paused || now - (t0 + (frame - f0) * FRAME_MS) > 100) { t0 = now; f0 = frame; }
        double wait = t0 + (frame - f0) * FRAME_MS - now;
        emscripten_sleep(paused ? 50 : wait > 0 ? (unsigned)wait : 0);
    } while (paused);
    pad_buttons[0] = web_pad;
}

int main(int argc, char **argv) {
    log_level = argc > 2 ? atoi(argv[2]) : 0;  // 1 = registro detallado (pruebas)
    max_frames = LONG_MAX;
#ifdef RECOMP_TRACE
    extern long trace_from;
    if (argc > 3) trace_from = atol(argv[3]);
#endif
    if (argc < 2 || !cd_open(argv[1])) { fprintf(stderr, "No se pudo abrir el disco\n"); return 1; }
    return boot_from_disc() ? 0 : 1;
}
