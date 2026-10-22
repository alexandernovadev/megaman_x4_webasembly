// Arranque: carga el PS-X EXE en la RAM emulada y salta al punto de entrada recompilado.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "runtime.h"
#include "hd.h"

extern long max_frames;
extern int log_level;
extern double max_seconds;

static const char *dump_dir;
static FILE *wav;
static uint32_t wav_frames;

// X4_WAV=salida.wav guarda el audio XA (estéreo, 16 bits, AUDIO_RATE). La cabecera se
// completa al salir, porque el programa termina con exit() al llegar a max_frames.
static void wav_header(void) {
    uint32_t data = wav_frames * 4, rate = AUDIO_RATE, brate = AUDIO_RATE * 4, fmt = 16, riff = 36 + data;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    fwrite(&fmt, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav);
    fwrite(&brate, 4, 1, wav); fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav);
    fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
}
static void wav_close(void) { wav_header(); fclose(wav); }
static long dump_every = 30;

// Guarda la pantalla visible (y la VRAM entera) como PPM cada `dump_every` frames.
// X4_INPUT="300:start,600:start" pulsa botones durante 5 frames a partir de ese frame;
// "300-420:right" los mantiene pulsados en ese intervalo.
static void scripted_input(long frame) {
    const char *spec = getenv("X4_INPUT");
    pad_buttons[0] = 0;
    if (!spec) return;
    static char *buf;
    static size_t cap;
    size_t len = strlen(spec) + 1;
    if (len > cap) { buf = realloc(buf, len); cap = len; }
    memcpy(buf, spec, len);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        long f = atol(tok), until = f + 5;
        char *dash = strchr(tok, '-'), *name = strchr(tok, ':');
        if (dash && name && dash < name) until = atol(dash + 1) + 1;
        if (!name || frame < f || frame >= until) continue;
        name++;
        if (!strcmp(name, "start")) pad_buttons[0] |= PAD_START;
        else if (!strcmp(name, "cross")) pad_buttons[0] |= PAD_CROSS;
        else if (!strcmp(name, "circle")) pad_buttons[0] |= PAD_CIRCLE;
        else if (!strcmp(name, "right")) pad_buttons[0] |= PAD_RIGHT;
        else if (!strcmp(name, "left")) pad_buttons[0] |= PAD_LEFT;
        else if (!strcmp(name, "up")) pad_buttons[0] |= PAD_UP;
        else if (!strcmp(name, "down")) pad_buttons[0] |= PAD_DOWN;
        else if (!strcmp(name, "square")) pad_buttons[0] |= PAD_SQUARE;
        else if (!strcmp(name, "triangle")) pad_buttons[0] |= PAD_TRIANGLE;
        else if (!strcmp(name, "l1")) pad_buttons[0] |= PAD_L1;
        else if (!strcmp(name, "r1")) pad_buttons[0] |= PAD_R1;
        else if (!strcmp(name, "l2")) pad_buttons[0] |= PAD_L2;
        else if (!strcmp(name, "r2")) pad_buttons[0] |= PAD_R2;
        else if (!strcmp(name, "select")) pad_buttons[0] |= PAD_SELECT;
    }
}

// X4_HDCAPTURE=carpeta: guarda las capturas de piezas para el HD (ver hd.c y tools/hd/).
static FILE *hd_cap;
void hd_emit_captures(const uint8_t *buf, int len) { if (hd_cap) fwrite(buf, 1, (size_t)len, hd_cap); }

// X4_CARD=archivo.mcr: tarjeta de memoria de la ranura 1 (se crea si no existe).
static const char *card_file;

void frame_hook(long frame) {
    scripted_input(frame);
    tex_pose_frame();
    extern int disp_x, disp_y;
    HdOut ho;
    int hd_ok = hd_frame(disp_x, disp_y, &ho);
    hd_new_n = 0;
    // X4_HDLIST="dir:f1,f2": guarda la lista de dibujo HD y la VRAM de esos frames (pruebas).
    const char *hl = getenv("X4_HDLIST");
    if (hd_ok && hl && strchr(hl, ':')) {
        char want[32];
        snprintf(want, sizeof want, ",%ld,", frame);
        char list[1024];
        snprintf(list, sizeof list, ",%s,", strchr(hl, ':') + 1);
        if (strstr(list, want)) {
            char path[512];
            snprintf(path, sizeof path, "%.*s/hdlist_%05ld.bin", (int)(strchr(hl, ':') - hl), hl, frame);
            static uint8_t tmp[640 * 480 * 4];
            int tw, th;
            gpu_framebuffer(tmp, &tw, &th);  // decide si el frame es panorámico
            FILE *f = fopen(path, "wb");
            if (f) {
                extern int gpu_wide_frame;
                int32_t hdr[4] = {ho.nv, ho.nb, gpu_wide_frame ? ws_reveal : 0, ho.np};
                fwrite(hdr, 4, 4, f);
                fwrite(ho.v, sizeof *ho.v, (size_t)ho.nv, f);
                fwrite(ho.b, sizeof *ho.b, (size_t)ho.nb, f);
                fwrite(ho.vram, 2, 1024 * 512, f);
                fwrite(ho.pal, 512, (size_t)ho.np, f);
                int32_t nid = hd_id_count();
                fwrite(&nid, 4, 1, f);
                for (int i = 0; i < nid; i++) { uint64_t h = hd_id_hash(i); fwrite(&h, 8, 1, f); }
                fclose(f);
            }
        }
    }
    if (card_file && memcard_dirty) {
        FILE *f = fopen(card_file, "wb");
        if (f) { fwrite(memcard, 1, sizeof memcard, f); fclose(f); }
        memcard_dirty = 0;
    }
    static int16_t pcm[4096 * 2];
    int n = audio_take(pcm, 4096);
    if (wav) { fwrite(pcm, 4, n, wav); wav_frames += n; }
    // X4_AUDIOSTAT: muestras de audio producidas en cada segundo emulado (60 frames).
    static long acc;
    acc += n;
    if (getenv("X4_AUDIOSTAT") && frame % 60 == 0) { extern long cd_reads; static long last_reads; fprintf(stderr, "[audio] segundo %ld: %ld muestras, %ld sectores\n", frame / 60, acc, cd_reads - last_reads); last_reads = cd_reads; acc = 0; }
    // X4_POKE="2000:801721f7=1,..." escribe ese byte en ese frame (pruebas).
    const char *poke = getenv("X4_POKE");
    if (poke) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", poke);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            uint32_t a; unsigned v; long f;
            if (sscanf(t, "%ld:%x=%x", &f, &a, &v) == 3 && f == frame) WR8(a, v);
        }
    }
    // X4_VOICELOG: muestra la huella de cada caja de diálogo nueva (ver voice.c).
    if (getenv("X4_VOICELOG")) {
        static uint32_t last_voice;
        uint32_t k = voice_frame();
        if (k != last_voice) { fprintf(stderr, "[voz] %ld %08x\n", frame, k); last_voice = k; }
    }
    // X4_RAMDUMP="frame:archivo": guarda los 2 MB de RAM en ese frame (pruebas).
    const char *rd = getenv("X4_RAMDUMP");
    if (rd && atol(rd) == frame && strchr(rd, ':')) {
        FILE *f = fopen(strchr(rd, ':') + 1, "wb");
        if (f) { fwrite(ram, 1, sizeof ram, f); fclose(f); }
    }
    // X4_PEEK="80139530,8013953c" imprime esas palabras de RAM cada X4_PEEK_EVERY frames (60).
    const char *peek = getenv("X4_PEEK");
    long every = getenv("X4_PEEK_EVERY") ? atol(getenv("X4_PEEK_EVERY")) : 60;
    if (peek && frame % every == 0) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", peek);
        fprintf(stderr, "[peek] %ld pad=%04x", frame, pad_buttons[0]);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            uint32_t a = (uint32_t)strtoul(t, NULL, 16);
            fprintf(stderr, " %08x=%08x", a, RD32(a));
        }
        fputc('\n', stderr);
    }
    if (!dump_dir || frame % dump_every) return;
    static uint8_t rgba[640 * 480 * 4];
    int w, h;
    gpu_framebuffer(rgba, &w, &h);
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05ld.ppm", dump_dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int i = 0; i < w * h; i++) fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/vram_%05ld.ppm", dump_dir, frame);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 1024 512 255\n");
    for (int i = 0; i < 1024 * 512; i++) {
        uint16_t p = vram[i];
        uint8_t px[3] = {(uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
#ifdef RECOMP_TRACE
    extern long trace_from;
    if (argc > 5) trace_from = atol(argv[5]);
#endif
    dump_dir = getenv("X4_DUMP");
    atexit(tex_summary);
    card_test_save = getenv("X4_TESTSAVE") != NULL;
    if (getenv("X4_HDCAPTURE")) {
        char path[512];
        snprintf(path, sizeof path, "%s/cap_%ld.bin", getenv("X4_HDCAPTURE"), (long)time(NULL));
        if ((hd_cap = fopen(path, "wb"))) hd_capture_on = 1;
    }
    if ((card_file = getenv("X4_CARD"))) {
        FILE *f = fopen(card_file, "rb");
        if (f) { fread(memcard, 1, sizeof memcard, f); fclose(f); }
        else memcard_new();
    }
    if (getenv("X4_WIDE")) ws_set(1);
    if (getenv("X4_FULL")) full_mode = 1;  // modo Completo (ver cheats.c)
    extern int gpu_aa;
    if (getenv("X4_NOAA")) gpu_aa = 0;
    if (getenv("X4_WAV") && (wav = fopen(getenv("X4_WAV"), "wb"))) { wav_header(); atexit(wav_close); }
    if (getenv("X4_DUMP_EVERY")) dump_every = atol(getenv("X4_DUMP_EVERY"));
    const char *exe = argc > 1 ? argv[1] : "extracted/SLUS_005.61";
    if (argc > 2) max_frames = atol(argv[2]);
    const char *bin = argc > 3 ? argv[3] : "../../../Downloads/megaman-x4/Mega Man X4 (USA).bin";
    if (!cd_open(bin)) { perror(bin); return 1; }
    if (argc > 4) max_seconds = atof(argv[4]);
    if (!strcmp(exe, "disc")) return boot_from_disc() ? 0 : 1;  // igual que la versión web
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[0x200000];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    uint32_t t_addr, t_size;
    memcpy(&t_addr, buf + 24, 4);
    memcpy(&t_size, buf + 28, 4);
    if (n < 0x800 + t_size) { fprintf(stderr, "EXE truncado\n"); return 1; }
    memcpy(ram + (t_addr & 0x1FFFFF), buf + 0x800, t_size);

    static CPU cpu;
    cur_cpu = &cpu;
    cpu.r[29] = 0x801FFF00;  // STACK de SYSTEM.CNF
    cpu.r[30] = cpu.r[29];
    cpu.r[31] = 0;
    fprintf(stderr, "Cargado %s: %u bytes en %08x, entrada %08x, %d funciones\n",
            exe, t_size, t_addr, exe_entry, func_count);
    dispatch(&cpu, exe_entry);
    fprintf(stderr, "La entrada volvió (el juego terminó)\n");
    return 0;
}
