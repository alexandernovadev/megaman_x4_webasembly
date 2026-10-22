#pragma once
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t cop0[32];
    uint32_t gte_d[32], gte_c[32];
    uint32_t dummy;
} CPU;

typedef void (*FuncPtr)(CPU *);
typedef struct { uint32_t addr; FuncPtr fn; } FuncEntry;

extern const FuncEntry func_table[];
extern const int func_count;
extern const uint32_t code_end, exe_entry;

extern uint8_t ram[0x200000];
extern uint8_t scratch[0x400];

uint32_t mem_read_slow(uint32_t a, int size);
void mem_write_slow(uint32_t a, uint32_t v, int size);

static inline uint32_t RD32(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF, v;
    if (p < 0x800000) { memcpy(&v, ram + (p & 0x1FFFFF), 4); return v; }
    return mem_read_slow(a, 4);
}
static inline uint32_t RD16(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF; uint16_t v;
    if (p < 0x800000) { memcpy(&v, ram + (p & 0x1FFFFF), 2); return v; }
    return mem_read_slow(a, 2);
}
static inline uint32_t RD8(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x800000) return ram[p & 0x1FFFFF];
    return mem_read_slow(a, 1);
}
static inline void WR32(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x800000) { memcpy(ram + (p & 0x1FFFFF), &v, 4); return; }
    mem_write_slow(a, v, 4);
}
static inline void WR16(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFF; uint16_t h = (uint16_t)v;
    if (p < 0x800000) { memcpy(ram + (p & 0x1FFFFF), &h, 2); return; }
    mem_write_slow(a, v, 2);
}
static inline void WR8(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x800000) { ram[p & 0x1FFFFF] = (uint8_t)v; return; }
    mem_write_slow(a, v, 1);
}

void dispatch(CPU *c, uint32_t target);
extern uint32_t resume_pc;  // reanudación tras setjmp (ver recomp.py)
#define DISPATCH(t) dispatch(c, (t))
extern int unwinding;  // el runtime pide deshacer la pila nativa (ReturnFromException)
#define UNWIND_CHECK if (__builtin_expect(unwinding, 0)) return

#ifdef RECOMP_TRACE
void trace_fn(uint32_t addr);
#define TRACE(a) trace_fn(a)
#else
#define TRACE(a) ((void)0)
#endif

void op_div(CPU *c, uint32_t a, uint32_t b);
void op_divu(CPU *c, uint32_t a, uint32_t b);
void op_syscall(CPU *c, uint32_t pc);
void op_break(CPU *c, uint32_t pc);
void op_rfe(CPU *c);
void op_unknown(CPU *c, uint32_t pc, uint32_t w);
uint32_t op_lwl(CPU *c, uint32_t a, uint32_t old);
uint32_t op_lwr(CPU *c, uint32_t a, uint32_t old);
void op_swl(CPU *c, uint32_t a, uint32_t v);
void op_swr(CPU *c, uint32_t a, uint32_t v);
void bad_return(CPU *c, uint32_t pc, uint32_t target, uint32_t expected);

uint32_t gte_read_data(CPU *c, int r);
uint32_t gte_read_ctrl(CPU *c, int r);
void gte_write_data(CPU *c, int r, uint32_t v);
void gte_write_ctrl(CPU *c, int r, uint32_t v);
void gte_command(CPU *c, uint32_t w);

void hle_VSync(CPU *c);

// Reloj emulado (ver runtime.c).
extern uint64_t cycles, clock_deadline;
void clock_sync(void);
void clock_advance(uint64_t n);
#define CLOCK_TICK do { cycles += 24; if (__builtin_expect(cycles >= clock_deadline, 0)) clock_sync(); } while (0)
int64_t cd_next_due(void);

extern int log_level;
extern long frame_count;  // vblanks desde el arranque
extern CPU *cur_cpu;
void raise_irq(int n);
void irq_poll(void);

int cd_open(const char *path);
int cd_read_raw(uint32_t lba, uint8_t *out);
int cd_find_file(const char *name, uint32_t *lba, uint32_t *size);
int boot_from_disc(void);  // carga SLUS_005.61 desde el disco abierto y salta a la entrada
void cd_tick(int64_t amount);
uint8_t cd_read_reg(int r);
void cd_write_reg(int r, uint8_t v);
void cd_dma(uint32_t madr, uint32_t words);

uint32_t bios_OpenThread(uint32_t pc, uint32_t sp, uint32_t gp);
int bios_CloseThread(uint32_t h);
void bios_ChangeThread(CPU *c, uint32_t h);

extern uint16_t vram[512 * 1024];
void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t gpu_read(void);
uint32_t gpu_stat(int odd_line);
void gpu_framebuffer(uint8_t *rgba, int *w, int *h);
extern uint32_t gpu_pkt_addr;
extern int gpu_wide_frame;
// Modo "grabar" texturas para el HD (texdump.c; activo con X4_TEXDUMP).
void tex_record(int depth, int tpx, int tpy, int cx, int cy, int u0, int v0, int w, int h);
void tex_summary(void);
void tex_pose_piece(int tpx, int tpy, int cx, int cy, int x, int y, int w, int h, int u0, int v0, int su, int sv);
void tex_pose_frame(void);
void frame_hook(long frame);  // la aporta main.c (nativo) o la plataforma web

// Botones del mando 1 (lógica positiva): bit 0 Select, 3 Start, 4 Arriba, 5 Derecha,
// 6 Abajo, 7 Izquierda, 8 L2, 9 R2, 10 L1, 11 R1, 12 Triángulo, 13 Círculo, 14 X, 15 Cuadrado.
extern uint16_t pad_buttons[2];
enum { PAD_SELECT = 1, PAD_START = 8, PAD_UP = 16, PAD_RIGHT = 32, PAD_DOWN = 64, PAD_LEFT = 128,
       PAD_L2 = 256, PAD_R2 = 512, PAD_L1 = 1024, PAD_R1 = 2048, PAD_TRIANGLE = 4096,
       PAD_CIRCLE = 8192, PAD_CROSS = 16384, PAD_SQUARE = 32768 };

// Audio (audio.c, spu.c). La plataforma recoge muestras estéreo de 16 bits a AUDIO_RATE.
#define AUDIO_RATE 44100
void xa_decode_sector(const uint8_t *sector);
void xa_reset(void);
int xa_pull(int16_t *l, int16_t *r);
void audio_out(int16_t l, int16_t r);
int audio_take(int16_t *out, int max);
void spu_sync(void);
uint32_t spu_read(uint32_t off, int size);
void spu_write(uint32_t off, uint32_t v, int size);
void spu_dma(uint32_t madr, uint32_t words, int to_spu);

// Panorámica (widescreen.c): píxeles extra por lado y columnas de tiles extra por lado.
extern int ws_reveal, ws_cols;
void ws_set(int on);
int ws_hook_bg_layer(CPU *c, uint32_t addr);
int ws_hook_actor_bounds(CPU *c, uint32_t addr);
int ws_hook_fixed_window(CPU *c, uint32_t addr);

void mdec_write(int reg, uint32_t v);
uint32_t mdec_read(int reg);
void mdec_dma_in(uint32_t madr, uint32_t words);
void mdec_dma_out(uint32_t madr, uint32_t words);

// Tarjeta de memoria (memcard.c): imagen de 128 KB de la ranura 1 y llamadas de la BIOS.
extern uint8_t memcard[128 * 1024];
extern int memcard_dirty;  // la plataforma la guarda y lo pone a 0
void memcard_new(void);    // tarjeta vacía ya formateada
extern int card_test_save; // atajo de prueba del menú de guardar
int card_hook_menu(CPU *c, uint32_t addr);

// Modo Completo (cheats.c): todo recogido desde el principio.
extern int full_mode;
int cheat_hook_select(CPU *c, uint32_t addr);
int cheat_hook_player(CPU *c, uint32_t addr);
int cheat_state(void);
int cheat_full_toggle(void);
uint32_t cheat_weapons(uint32_t defeated);

// Voces (voice.c): huella de la caja de diálogo en pantalla (0 = ninguna).
uint32_t voice_frame(void);
uint32_t card_open_event(uint32_t id, uint32_t cls, uint32_t spec);
int card_test_event(uint32_t id);
uint32_t card_open(uint32_t path, uint32_t mode);
uint32_t card_seek(uint32_t fd, uint32_t off, uint32_t whence);
uint32_t card_rw(uint32_t fd, uint32_t addr, uint32_t len, int write);
uint32_t card_close(uint32_t fd);
uint32_t card_format(uint32_t path);
uint32_t card_firstfile(uint32_t path, uint32_t direntry);
uint32_t card_nextfile(uint32_t direntry);
uint32_t card_erase(uint32_t path);
void card_info(uint32_t port);
void card_load(uint32_t port);
uint32_t card_sector(uint32_t port, uint32_t sector, uint32_t addr, int write);
