// Runtime del port: memoria, registros de hardware, BIOS en alto nivel (HLE) y despacho.
#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t ram[0x200000];
uint8_t scratch[0x400];
static uint8_t io[0x2000];  // copia de los registros 0x1F801000-0x1F802FFF

int log_level = 1;
long frame_count;
CPU *cur_cpu;
long max_frames = 5;

#define LOG(...) do { if (log_level) fprintf(stderr, __VA_ARGS__); } while (0)

static void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

// ---------------------------------------------------------------- E/S
static uint32_t io_get(uint32_t off, int size) {
    uint32_t v = 0;
    memcpy(&v, io + off, size);
    return v;
}

static uint8_t seen_read[0x2000], seen_write[0x2000];

static void dma_run(int ch) {
    uint32_t base = 0x80 + ch * 0x10;
    uint32_t madr = io_get(base, 4) & 0x1FFFFC;
    uint32_t bcr = io_get(base + 4, 4);
    uint32_t chcr = io_get(base + 8, 4);
    if (ch == 6) {
        // OTC: tabla de orden enlazada hacia atrás, terminada en 0x00FFFFFF.
        uint32_t n = bcr & 0xFFFF;
        if (!n) n = 0x10000;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = (madr - i * 4) & 0x1FFFFC;
            uint32_t v = (i == n - 1) ? 0x00FFFFFF : ((a - 4) & 0x1FFFFC);
            memcpy(ram + a, &v, 4);
        }
    } else if (ch == 2) {
        int sync = (chcr >> 9) & 3;
        if (sync == 2) {  // lista enlazada de paquetes GP0
            uint32_t a = madr, guard = 0;
            while (guard++ < 0x80000) {
                uint32_t hdr;
                memcpy(&hdr, ram + (a & 0x1FFFFC), 4);
                uint32_t n = hdr >> 24;
                gpu_pkt_addr = a & 0x1FFFFC;  // la GPU recoloca el HUD por su origen
                for (uint32_t i = 1; i <= n; i++) {
                    uint32_t w;
                    memcpy(&w, ram + ((a + i * 4) & 0x1FFFFC), 4);
                    gpu_gp0(w);
                }
                if (hdr & 0x800000) break;
                a = hdr & 0x1FFFFC;
            }
            gpu_pkt_addr = 0;
        } else {  // bloques: RAM <-> GPU
            uint32_t words = sync == 0 ? (bcr & 0xFFFF) : (bcr & 0xFFFF) * (bcr >> 16);
            if (sync == 0 && !words) words = 0x10000;
            for (uint32_t i = 0; i < words; i++) {
                uint32_t a = (madr + i * 4) & 0x1FFFFC;
                if (chcr & 1) { uint32_t w; memcpy(&w, ram + a, 4); gpu_gp0(w); }
                else { uint32_t w = gpu_read(); memcpy(ram + a, &w, 4); }
            }
        }
    }
    else if (ch == 0 || ch == 1) {
        uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
        if (ch == 0) mdec_dma_in(madr, words); else mdec_dma_out(madr, words);
    }
    else if (ch == 3) {
        uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
        cd_dma(madr, words);
    }
    else if (ch == 4) {
        uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
        spu_dma(madr, words, chcr & 1);
    }
    chcr &= ~0x11000000u;  // transferencia completada al instante
    memcpy(io + base + 8, &chcr, 4);
    uint32_t dicr = io_get(0xF4, 4);
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
        if (dicr & 0x00800000) { dicr |= 0x80000000; memcpy(io + 0xF4, &dicr, 4); raise_irq(3); return; }
        memcpy(io + 0xF4, &dicr, 4);
    }
}

uint32_t mem_read_slow(uint32_t a, int size) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p >= 0x1F800000 && p < 0x1F800400) {
        uint32_t v = 0;
        memcpy(&v, scratch + (p & 0x3FF), size);
        return v;
    }
    if (p >= 0x1F801000 && p < 0x1F803000) {
        uint32_t off = p - 0x1F801000;
        if (!seen_read[off]) { seen_read[off] = 1; LOG("[io] lee  %08x (%d)\n", p, size); }
        clock_advance(40);
        if (p >= 0x1F801800 && p < 0x1F801804) return cd_read_reg(p & 3);
        if (p == 0x1F801820 || p == 0x1F801824) return mdec_read((p >> 2) & 1);
        if (p >= 0x1F801C00 && p < 0x1F802000) return spu_read(p, size);
        switch (p) {
            case 0x1F801814: { static int line; return gpu_stat(line ^= 1); }  // GPUSTAT
            case 0x1F801810: return gpu_read();                                // GPUREAD
            case 0x1F801100: case 0x1F801110: case 0x1F801120: {          // contadores
                static uint32_t t;
                return (t += 0x100) & 0xFFFF;
            }
        }
        return io_get(off, size);
    }
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;  // ROM de la BIOS (no la usamos)
    if (p == 0x1FFE0130) return 0;                    // control de caché
    fatal("lectura fuera de mapa %08x", a);
    return 0;
}

void mem_write_slow(uint32_t a, uint32_t v, int size) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p >= 0x1F800000 && p < 0x1F800400) { memcpy(scratch + (p & 0x3FF), &v, size); return; }
    if (p >= 0x1F801000 && p < 0x1F803000) {
        uint32_t off = p - 0x1F801000;
        if (!seen_write[off]) { seen_write[off] = 1; LOG("[io] escribe %08x = %08x (%d)\n", p, v, size); }
        clock_advance(40);
        if (p >= 0x1F801800 && p < 0x1F801804) { cd_write_reg(p & 3, (uint8_t)v); return; }
        if (p == 0x1F801820 || p == 0x1F801824) { mdec_write((p >> 2) & 1, v); return; }
        if (p >= 0x1F801C00 && p < 0x1F802000) { spu_write(p, v, size); return; }
        if (p == 0x1F8010F4) {  // DICR: escribir 1 en los bits 24-30 los reconoce
            uint32_t d = io_get(0xF4, 4);
            d = (v & 0x00FFFFFF) | (d & ~v & 0x7F000000);
            if ((d & 0x00800000) && (d & (d << 8) & 0x7F000000)) d |= 0x80000000;
            memcpy(io + 0xF4, &d, 4);
            return;
        }
        if (p == 0x1F801810) { gpu_gp0(v); return; }
        if (p == 0x1F801814) { gpu_gp1(v); return; }
        if (p == 0x1F801074) { memcpy(io + off, &v, size); irq_poll(); return; }
        if (p == 0x1F801070) {  // I_STAT: escribir 0 en un bit lo reconoce
            uint32_t s = io_get(0x70, 4) & v;
            memcpy(io + 0x70, &s, 4);
            return;
        }
        memcpy(io + off, &v, size);
        if (p >= 0x1F801088 && p <= 0x1F8010E8 && (p & 0xF) == 8 && (v & 0x01000000))
            dma_run((p - 0x1F801080) >> 4);
        return;
    }
    if (p == 0x1FFE0130) return;
    fatal("escritura fuera de mapa %08x = %08x", a, v);
}

// ---------------------------------------------------------------- interrupciones
// libetc registra su despachador con setjmp + HookEntryInt. Al llegar una IRQ, la BIOS
// real restaura ese jmp_buf y salta a su ra; el despachador termina con ReturnFromException.
static uint32_t hook_buf;
static int in_irq;
int unwinding;
static int crit_section;

static void irq_deliver(void) {
    CPU *c = cur_cpu;
    CPU saved = *c;
    in_irq = 1;
    c->r[31] = RD32(hook_buf + 0);
    c->r[29] = RD32(hook_buf + 4);
    c->r[30] = RD32(hook_buf + 8);
    for (int i = 0; i < 8; i++) c->r[16 + i] = RD32(hook_buf + 12 + i * 4);
    c->r[28] = RD32(hook_buf + 44);
    uint32_t handler = c->r[31];
    c->r[31] = 0;
    dispatch(c, handler);
    unwinding = 0;
    *c = saved;
    in_irq = 0;
    irq_poll();  // otra IRQ pudo quedar pendiente mientras se atendía esta
}

void irq_poll(void) {
    if (in_irq || crit_section || !hook_buf || !cur_cpu) return;
    if (io_get(0x70, 4) & io_get(0x74, 4) & 0x7FF) irq_deliver();
}

void raise_irq(int n) {
    uint32_t s = io_get(0x70, 4) | (1u << n);
    memcpy(io + 0x70, &s, 4);
    irq_poll();
}

// ---------------------------------------------------------------- CPU
void op_div(CPU *c, uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0) { c->hi = a; c->lo = x >= 0 ? 0xFFFFFFFF : 1; return; }
    if (x == INT32_MIN && y == -1) { c->hi = 0; c->lo = 0x80000000; return; }
    c->lo = (uint32_t)(x / y);
    c->hi = (uint32_t)(x % y);
}

void op_divu(CPU *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->hi = a; c->lo = 0xFFFFFFFF; return; }
    c->lo = a / b;
    c->hi = a % b;
}

uint32_t op_lwl(CPU *c, uint32_t a, uint32_t old) {
    uint32_t w = RD32(a & ~3u), sh = (a & 3) * 8;
    return (old & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}

uint32_t op_lwr(CPU *c, uint32_t a, uint32_t old) {
    uint32_t w = RD32(a & ~3u), sh = (a & 3) * 8;
    return (old & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}

void op_swl(CPU *c, uint32_t a, uint32_t v) {
    uint32_t al = a & ~3u, w = RD32(al), sh = (a & 3) * 8;
    WR32(al, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (v >> (24 - sh)));
}

void op_swr(CPU *c, uint32_t a, uint32_t v) {
    uint32_t al = a & ~3u, w = RD32(al), sh = (a & 3) * 8;
    WR32(al, (w & ~(0xFFFFFFFFu << sh)) | (v << sh));
}

void op_syscall(CPU *c, uint32_t pc) {
    switch (c->r[4]) {
        case 1: c->r[2] = !crit_section; crit_section = 1; break;  // EnterCriticalSection
        case 2: crit_section = 0; irq_poll(); break;                // ExitCriticalSection
        default: LOG("[cpu] syscall %u en %08x\n", c->r[4], pc);
    }
}

void op_break(CPU *c, uint32_t pc) { fatal("break en %08x", pc); }
void op_rfe(CPU *c) {}
void op_unknown(CPU *c, uint32_t pc, uint32_t w) { fatal("instrucción desconocida %08x en %08x", w, pc); }

void bad_return(CPU *c, uint32_t pc, uint32_t target, uint32_t expected) {
    LOG("[cpu] retorno anómalo en %08x: va a %08x, se esperaba %08x\n", pc, target, expected);
}

// ---------------------------------------------------------------- GTE (pendiente)
static long gte_ops;
uint32_t gte_read_data(CPU *c, int r) { return c->gte_d[r]; }
uint32_t gte_read_ctrl(CPU *c, int r) { return c->gte_c[r]; }
void gte_write_data(CPU *c, int r, uint32_t v) { c->gte_d[r] = v; }
void gte_write_ctrl(CPU *c, int r, uint32_t v) { c->gte_c[r] = v; }
void gte_command(CPU *c, uint32_t w) {
    if (gte_ops++ < 5) LOG("[gte] comando %08x (sin implementar)\n", w);
}

// ---------------------------------------------------------------- mandos
// La BIOS escribe el estado de los mandos en los búferes de InitPAD en cada vblank.
// Formato: estado (0 = ok), id (0x41 = mando digital), botones en lógica negativa.
static uint32_t pad_buf[2];
uint16_t pad_buttons[2];  // lógica positiva; los pone la plataforma (teclado, gamepad)

static void pad_update(void) {
    for (int i = 0; i < 2; i++) {
        if (!pad_buf[i]) continue;
        if (i == 1) { WR8(pad_buf[i], 0xFF); continue; }  // sin mando 2
        uint16_t b = (uint16_t)~pad_buttons[i];
        WR8(pad_buf[i], 0x00);
        WR8(pad_buf[i] + 1, 0x41);
        WR8(pad_buf[i] + 2, b & 0xFF);
        WR8(pad_buf[i] + 3, b >> 8);
    }
}

// ---------------------------------------------------------------- BIOS (HLE)
// Tablas falsas que libapi pide a la BIOS para engancharse (GetC0Table/GetB0Table).
#define FAKE_TABLE 0x00000200u

static uint32_t heap_ptr, heap_end;

static void bios_printf(CPU *c) {
    // printf mínimo: %d %u %x %X %s %c %p con anchura opcional.
    char out[512];
    int o = 0, argi = 1;
    uint32_t fmt = c->r[4];
    uint32_t sp = c->r[29];
    for (;;) {
        char ch = (char)RD8(fmt++);
        if (!ch || o > 480) break;
        if (ch != '%') { out[o++] = ch; continue; }
        char spec[16] = "%";
        int s = 1;
        while ((ch = (char)RD8(fmt++)) && strchr("0123456789-.lh", ch) && s < 12) spec[s++] = ch;
        uint32_t arg = argi < 4 ? c->r[4 + argi] : RD32(sp + argi * 4);
        argi++;
        if (ch == 's') {
            char str[256];
            int k = 0;
            while (k < 255 && (str[k] = (char)RD8(arg + k))) k++;
            str[k] = 0;
            spec[s++] = 's'; spec[s] = 0;
            o += snprintf(out + o, sizeof out - o, spec, str);
        } else if (ch == '%') {
            out[o++] = '%';
            argi--;
        } else {
            spec[s++] = ch; spec[s] = 0;
            o += snprintf(out + o, sizeof out - o, spec, arg);
        }
    }
    out[o] = 0;
    fprintf(stderr, "[printf] %s", out);
}

static void bios_call(CPU *c, uint32_t table, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    uint32_t *v0 = &c->r[2];
    static uint8_t seen[3][256];
    int t = (table - 0xA0) >> 4;
    static int all = -1;
    if (all < 0) all = getenv("X4_BIOSLOG") != NULL;
    if (all && !(table == 0xB0 && (fn == 0x17 || (fn == 0x0B && a0 > 0xF1000006u))))
        fprintf(stderr, "[bios] %ld %02X(%02X) a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x\n", frame_count, table, fn, a0, a1, a2, c->r[7], c->r[31]);
    else if (!seen[t][fn & 0xFF]) { seen[t][fn & 0xFF] = 1; LOG("[bios] %02X(%02X) a0=%08x a1=%08x a2=%08x a3=%08x\n", table, fn, a0, a1, a2, c->r[7]); }
    if (table == 0xA0) {
        switch (fn) {
            case 0x13:                                                           // setjmp
                WR32(a0, c->r[31]); WR32(a0 + 4, c->r[29]); WR32(a0 + 8, c->r[30]);
                for (int i = 0; i < 8; i++) WR32(a0 + 12 + i * 4, c->r[16 + i]);
                WR32(a0 + 44, c->r[28]);
                *v0 = 0;
                return;
            case 0x17: { uint32_t i = 0; int d;                                  // strcmp
                do { d = (int)RD8(a0 + i) - (int)RD8(a1 + i); } while (!d && RD8(a0 + i++));
                *v0 = (uint32_t)d; return; }
            case 0x19: { uint32_t i = 0; uint32_t b;                            // strcpy
                do { b = RD8(a1 + i); WR8(a0 + i, b); i++; } while (b);
                *v0 = a0; return; }
            case 0x1B: { uint32_t i = 0; while (RD8(a0 + i)) i++; *v0 = i; return; }  // strlen
            case 0x2A: for (uint32_t i = 0; i < a2; i++) WR8(a0 + i, RD8(a1 + i)); *v0 = a0; return;  // memcpy
            case 0x2B: for (uint32_t i = 0; i < a2; i++) WR8(a0 + i, a1); *v0 = a0; return;          // memset
            case 0x33: *v0 = heap_ptr; heap_ptr += (a0 + 7) & ~7u;               // malloc
                if (heap_ptr > heap_end) fatal("malloc sin memoria"); return;
            case 0x34: return;                                                   // free
            case 0x39: heap_ptr = a0; heap_end = a0 + a1; return;                // InitHeap
            case 0x3F: bios_printf(c); return;                                   // printf
            case 0x44: return;                                                   // FlushCache
            case 0x49: return;                                                   // GPU_cw
            case 0x72: return;                                                   // CdRemove
            case 0x9F: return;                                                   // SetMem
            case 0x70: *v0 = 1; return;                                          // _bu_init
            case 0xAB: card_info(a0); *v0 = 1; return;                           // _card_info
            case 0xAC: card_load(a0); *v0 = 1; return;                           // _card_load
            case 0xA1: case 0xA2: case 0xA3: *v0 = 0; return;                    // varios CD
        }
    } else if (table == 0xB0) {
        static uint32_t next_event = 0xF1000000;
        switch (fn) {
            case 0x07: case 0x0D: case 0x20: *v0 = 1; return;                    // Deliver/Disable/UnDeliver
            case 0x08: *v0 = card_open_event(next_event++, a0, a1); return;      // OpenEvent
            case 0x09: case 0x0C: *v0 = 1; return;                               // Close/EnableEvent
            case 0x0A: case 0x0B: { int r = card_test_event(a0);                 // Wait/TestEvent
                *v0 = r < 0 ? 1 : (uint32_t)r; return; }
            case 0x0E: *v0 = bios_OpenThread(a0, a1, a2); return;               // OpenThread
            case 0x0F: *v0 = bios_CloseThread(a0); return;                       // CloseThread
            case 0x10: bios_ChangeThread(c, a0); return;                         // ChangeThread
            case 0x12: pad_buf[0] = a0; pad_buf[1] = a2; *v0 = 1;              // InitPAD
                if (getenv("X4_PADLOG")) fprintf(stderr, "[pad] InitPAD %08x %08x frame %ld\n", a0, a2, frame_count);
                return;
            case 0x13: case 0x14: *v0 = 1;                                       // Start/StopPAD
                if (getenv("X4_PADLOG")) fprintf(stderr, "[pad] %sPAD frame %ld\n", fn == 0x13 ? "Start" : "Stop", frame_count);
                return;
            case 0x17: if (in_irq) unwinding = 1; return;                        // ReturnFromException
            case 0x18: hook_buf = 0; return;                                     // ResetEntryInt
            case 0x19: hook_buf = a0; return;                                    // HookEntryInt
            case 0x3D: fputc((int)a0, stderr); return;                           // putchar
            case 0x3F: { uint32_t i = 0; int ch;                                 // puts
                fprintf(stderr, "[puts] ");
                while ((ch = RD8(a0 + i++))) fputc(ch, stderr);
                fputc('\n', stderr); return; }
            case 0x4A: case 0x4B: case 0x4C: *v0 = 1; return;                    // Init/Start/StopCARD
            // Archivos de la tarjeta de memoria ("bu00:..."), ver memcard.c.
            case 0x32: *v0 = card_open(a0, a1); return;                          // open
            case 0x33: *v0 = card_seek(a0, a1, a2); return;                      // lseek
            case 0x34: *v0 = card_rw(a0, a1, a2, 0); return;                     // read
            case 0x35: *v0 = card_rw(a0, a1, a2, 1); return;                     // write
            case 0x36: *v0 = card_close(a0); return;                             // close
            case 0x41: *v0 = card_format(a0); return;                            // format
            case 0x42: *v0 = card_firstfile(a0, a1); return;                     // firstfile
            case 0x43: *v0 = card_nextfile(a0); return;                          // nextfile
            case 0x45: *v0 = card_erase(a0); return;                             // erase
            case 0x4E: *v0 = card_sector(a0, a1, a2, 1); return;                 // write_card_sector
            case 0x4F: *v0 = card_sector(a0, a1, a2, 0); return;                 // read_card_sector
            case 0x50: return;                                                   // allow_new_card
            case 0x56: case 0x57: *v0 = FAKE_TABLE; return;                      // GetC0/B0Table
            case 0x5B: return;                                                   // ChangeClearPad
        }
    } else {
        switch (fn) {
            case 0x02: case 0x03: *v0 = 0; return;                               // SysEnq/DeqIntRP
            case 0x0A: return;                                                   // ChangeClearRCnt
        }
    }
    LOG("[bios] %02X(%02X) sin implementar, devuelve 0\n", table, fn);
    *v0 = 0;
}

// ---------------------------------------------------------------- despacho
uint32_t resume_pc;
void dispatch(CPU *c, uint32_t target) {
    uint32_t t = target & 0x1FFFFFFF;
    if (t == 0xA0 || t == 0xB0 || t == 0xC0) { bios_call(c, t, c->r[9]); return; }
    if (t < 0x10000) {
        // Rutinas internas de la BIOS a las que libcard salta a través de GetB0Table.
        static uint8_t seen[0x4000];
        if (!seen[t >> 2] || getenv("X4_BIOSLOG")) { seen[t >> 2] = 1; LOG("[bios] rutina interna %08x (ignorada) frame %ld ra=%08x\n", target, frame_count, c->r[31]); }
        c->r[2] = 1;
        return;
    }
    int lo = 0, hi = func_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t a = func_table[mid].addr;
        if (a == target) { func_table[mid].fn(c); return; }
        if (a < target) lo = mid + 1; else hi = mid - 1;
    }
    // Destino a mitad de función: solo válido si esa función tiene un punto de reanudación.
    if (hi >= 0 && target > func_table[hi].addr && target < code_end) {
        resume_pc = target;
        func_table[hi].fn(c);
        if (resume_pc == 0) return;
        resume_pc = 0;
    }
    fatal("salto indirecto a %08x, que no es inicio de función (ra=%08x)", target, c->r[31]);
}

// Arranque desde el disco del usuario: lee el PS-X EXE por ISO9660, lo copia a la RAM
// emulada y salta al punto de entrada recompilado. Solo vuelve si algo falla.
int boot_from_disc(void) {
    uint32_t lba, size;
    if (!cd_find_file("SLUS_005.61;1", &lba, &size) || size < 0x800 || size > 0x200000) {
        fprintf(stderr, "No se encontró SLUS_005.61 en el disco (¿es Mega Man X4 USA?)\n");
        return 0;
    }
    static uint8_t buf[0x200000];
    uint8_t s[2352];
    for (uint32_t off = 0; off < size; off += 2048) {
        if (!cd_read_raw(lba + off / 2048, s)) return 0;
        memcpy(buf + off, s + 24, size - off < 2048 ? size - off : 2048);
    }
    uint32_t t_addr, t_size;
    memcpy(&t_addr, buf + 24, 4);
    memcpy(&t_size, buf + 28, 4);
    if (memcmp(buf, "PS-X EXE", 8) || size < 0x800 + t_size) { fprintf(stderr, "EXE inválido\n"); return 0; }
    memcpy(ram + (t_addr & 0x1FFFFF), buf + 0x800, t_size);

    static CPU cpu;
    cur_cpu = &cpu;
    cpu.r[29] = 0x801FFF00;  // STACK de SYSTEM.CNF
    cpu.r[30] = cpu.r[29];
    cpu.r[31] = 0;
    fprintf(stderr, "Cargado SLUS_005.61: %u bytes en %08x, entrada %08x, %d funciones\n",
            t_size, t_addr, exe_entry, func_count);
    dispatch(&cpu, exe_entry);
    fprintf(stderr, "La entrada volvió (el juego terminó)\n");
    return 1;
}

#ifdef RECOMP_TRACE
long trace_from = -1, trace_left = 300000;
void trace_fn(uint32_t addr) {
    if (trace_from < 0 || frame_count < trace_from || trace_left <= 0) return;
    trace_left--;
    fprintf(stderr, "> %08x\n", addr);
}
#endif

// ---------------------------------------------------------------- reloj
// Tiempo emulado en ciclos de CPU. Avanza en cada llamada a función (CLOCK_TICK) y en
// cada acceso a hardware; cada FRAME_CYCLES hay un vblank con su interrupción.
#define FRAME_CYCLES 564480ULL  // 33,8688 MHz / 60
#define HSYNC_CYCLES 2152ULL

uint64_t cycles, clock_deadline;
static uint64_t next_vblank = FRAME_CYCLES, cd_last;

static void vblank(void) {
    frame_count++;
    LOG("[frame] %ld\n", frame_count);
    frame_hook(frame_count);
    pad_update();
    if (frame_count >= max_frames) {
        extern long cd_reads;
        LOG("Se alcanzaron %ld frames; fin. Sectores leídos: %ld\n", frame_count, cd_reads);
        exit(0);
    }
    raise_irq(0);
}

// Puede anidarse (un callback de IRQ que espera al CD): el CD sigue avanzando y los
// vblank anidados solo marcan la IRQ como pendiente, porque in_irq bloquea la entrega.
void clock_sync(void) {
    for (;;) {
        if (cycles > cd_last) { cd_tick((int64_t)(cycles - cd_last)); cd_last = cycles; }
        if (cycles >= next_vblank) { next_vblank += FRAME_CYCLES; vblank(); continue; }
        break;
    }
    irq_poll();
    uint64_t due = cycles + (uint64_t)cd_next_due();
    clock_deadline = due < next_vblank ? due : next_vblank;
}

void clock_advance(uint64_t n) {
    cycles += n;
    if (cycles >= clock_deadline) clock_sync();
}

// ---------------------------------------------------------------- HLE de libetc
// VSync(n): n<0 devuelve el contador de vblanks; 1 devuelve las líneas desde el último
// vblank; 0 o n>1 espera hasta el siguiente vblank (n vblanks desde la última llamada).
void hle_VSync(CPU *c) {
    int32_t mode = (int32_t)c->r[4];
    static long last_frame;
    if (mode < 0) { clock_advance(200); c->r[2] = RD32(0x8011dc50); return; }
    if (mode == 1) {
        c->r[2] = (uint32_t)((cycles - (next_vblank - FRAME_CYCLES)) / HSYNC_CYCLES);
        return;
    }
    long target = last_frame + (mode > 1 ? mode : 1);
    if (target < frame_count + 1) target = frame_count + 1;
    // Se avanza de evento en evento (no de golpe al vblank) para que el CD siga leyendo
    // mientras el juego espera; si no, durante VSync solo llegaría un sector por frame.
    while (frame_count < target) {
        if (clock_deadline <= cycles) clock_sync();
        else clock_advance(clock_deadline - cycles);
    }
    last_frame = frame_count;
    c->r[2] = 0;
}
