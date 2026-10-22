// Tarjeta de memoria HLE: las funciones de tarjeta de la BIOS (sistema de archivos "bu00:",
// _card_info/_card_load y los eventos SwCARD/HwCARD) sobre una imagen de 128 KB con el
// formato estándar de la PS1 (la misma ".mcr" que usan los emuladores). Solo hay tarjeta en
// la ranura 1; la plataforma la carga al arrancar y la guarda cuando cambia.
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

#define CARD_SIZE (128 * 1024)
#define FRAME 128
#define BLOCK 8192

uint8_t memcard[CARD_SIZE];
int memcard_dirty;
static int present = 1;

// ---------------------------------------------------------------- eventos
// Las ranuras de eventos que abre libcard (clase + tipo). TestEvent solo devuelve 1 en las de
// tarjeta cuando la operación pendiente ha terminado con ese resultado.
#define EV_SW 0xF4000001u
#define EV_HW 0xF0000011u
enum { SP_IOE = 0x0004, SP_TIMEOUT = 0x0100, SP_NEW = 0x2000, SP_ERROR = 0x8000 };
#define MAX_EV 64
static struct { uint32_t cls, spec; int pending; } events[MAX_EV];

uint32_t card_open_event(uint32_t id, uint32_t cls, uint32_t spec) {
    uint32_t i = id & 0xFFFF;
    if (i < MAX_EV) { events[i].cls = cls; events[i].spec = spec; events[i].pending = 0; }
    return id;
}

static void deliver(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < MAX_EV; i++)
        if (events[i].cls == cls && events[i].spec == spec) events[i].pending = 1;
}

// -1 = no es un evento de tarjeta (el llamador decide); 0/1 = estado.
int card_test_event(uint32_t id) {
    uint32_t i = id & 0xFFFF;
    if (i >= MAX_EV || (events[i].cls != EV_SW && events[i].cls != EV_HW)) return -1;
    int p = events[i].pending;
    events[i].pending = 0;
    return p;
}

// ---------------------------------------------------------------- formato
static uint8_t *frame_at(int n) { return memcard + n * FRAME; }
static void checksum(uint8_t *f) { uint8_t x = 0; for (int i = 0; i < FRAME - 1; i++) x ^= f[i]; f[FRAME - 1] = x; }
static int formatted(void) { return memcard[0] == 'M' && memcard[1] == 'C'; }

static void format_card(void) {
    memset(memcard, 0, CARD_SIZE);
    memcard[0] = 'M'; memcard[1] = 'C';
    checksum(frame_at(0));
    for (int n = 1; n < 16; n++) {
        uint8_t *f = frame_at(n);
        f[0] = 0xA0; f[8] = f[9] = 0xFF;
        checksum(f);
    }
    for (int n = 16; n < 36; n++) {  // lista de sectores defectuosos: vacía
        uint8_t *f = frame_at(n);
        memset(f, 0xFF, 4); f[8] = f[9] = 0xFF;
        checksum(f);
    }
    memcpy(frame_at(63), frame_at(0), FRAME);
    memcard_dirty = 1;
}

// Tarjeta nueva: se entrega ya formateada, como hacen los emuladores.
void memcard_new(void) { format_card(); }

static uint8_t *dir(int b) { return frame_at(b); }  // b = 1..15
static uint32_t dir_size(int b) { uint32_t v; memcpy(&v, dir(b) + 4, 4); return v; }
static int dir_next(int b) { uint16_t v; memcpy(&v, dir(b) + 8, 2); return v == 0xFFFF ? -1 : v + 1; }

static int find_file(const char *name) {
    for (int b = 1; b < 16; b++)
        if (dir(b)[0] == 0x51 && !strncmp((char *)dir(b) + 10, name, 20)) return b;
    return 0;
}

// Bloque físico del bloque lógico `k` del archivo que empieza en `b`.
static int chain(int b, int k) {
    while (k-- > 0 && b > 0) b = dir_next(b);
    return b;
}

static int create_file(const char *name, int blocks) {
    if (blocks < 1) blocks = 1;
    int list[15], n = 0;
    for (int b = 1; b < 16 && n < blocks; b++)
        if ((dir(b)[0] & 0xF0) == 0xA0) list[n++] = b;
    if (n < blocks) return 0;
    for (int i = 0; i < n; i++) {
        uint8_t *f = dir(list[i]);
        memset(f, 0, FRAME);
        f[0] = i == 0 ? 0x51 : i == n - 1 ? 0x53 : 0x52;
        if (i == 0) { uint32_t sz = (uint32_t)blocks * BLOCK; memcpy(f + 4, &sz, 4); strncpy((char *)f + 10, name, 20); }
        uint16_t next = i == n - 1 ? 0xFFFF : (uint16_t)(list[i + 1] - 1);
        memcpy(f + 8, &next, 2);
        checksum(f);
    }
    memcard_dirty = 1;
    return list[0];
}

static void erase_file(int b) {
    while (b > 0) {
        int next = dir_next(b);
        uint8_t *f = dir(b);
        f[0] = 0xA0;
        f[8] = f[9] = 0xFF;
        checksum(f);
        b = next;
    }
    memcard_dirty = 1;
}

// ---------------------------------------------------------------- archivos
// "bu00:NOMBRE": puerto 0 (ranura 1). Devuelve el nombre o NULL si no es la tarjeta 1.
static const char *card_path(uint32_t a, char *buf, int *port) {
    int i = 0;
    while (i < 63 && (buf[i] = (char)RD8(a + i))) i++;
    buf[i] = 0;
    if (strncmp(buf, "bu", 2) || !buf[2] || !buf[3] || buf[4] != ':') return NULL;
    *port = buf[2] - '0';
    return buf + 5;
}

#define FD_BASE 2
static struct { int used, head, pos, size; } fds[4];

int card_is_path(uint32_t a) { char b[64]; int p; return card_path(a, b, &p) != NULL; }

uint32_t card_open(uint32_t a, uint32_t mode) {
    char buf[64]; int port;
    const char *name = card_path(a, buf, &port);
    if (!name || port != 0 || !present || !formatted()) return 0xFFFFFFFF;
    int b = find_file(name);
    if (mode & 0x200) {  // crear: el número de bloques va en los 16 bits altos
        if (b) return 0xFFFFFFFF;
        b = create_file(name, (int)(mode >> 16));
        if (!b) return 0xFFFFFFFF;
    }
    if (!b) return 0xFFFFFFFF;
    for (int i = 0; i < 4; i++)
        if (!fds[i].used) {
            fds[i].used = 1; fds[i].head = b; fds[i].pos = 0; fds[i].size = (int)dir_size(b);
            if (log_level) fprintf(stderr, "[card] open %s modo %x -> %d\n", name, mode, FD_BASE + i);
            return FD_BASE + i;
        }
    return 0xFFFFFFFF;
}

int card_owns_fd(uint32_t fd) { return fd >= FD_BASE && fd < FD_BASE + 4 && fds[fd - FD_BASE].used; }

uint32_t card_rw(uint32_t fd, uint32_t addr, uint32_t len, int write) {
    if (!card_owns_fd(fd)) return 0xFFFFFFFF;
    __typeof__(fds[0]) *f = &fds[fd - FD_BASE];
    if ((uint32_t)f->pos + len > (uint32_t)f->size) len = (uint32_t)(f->size - f->pos);
    for (uint32_t i = 0; i < len; i++) {
        int p = f->pos + (int)i;
        uint8_t *d = memcard + chain(f->head, p / BLOCK) * BLOCK + p % BLOCK;
        if (write) *d = (uint8_t)RD8(addr + i); else WR8(addr + i, *d);
    }
    f->pos += (int)len;
    if (write) memcard_dirty = 1;
    // Las lecturas y escrituras terminan al momento; quien espere el evento lo ve ya.
    deliver(EV_SW, SP_IOE);
    deliver(EV_HW, SP_IOE);
    if (log_level) fprintf(stderr, "[card] %s fd %d %u bytes\n", write ? "write" : "read", fd, len);
    return len;
}

uint32_t card_seek(uint32_t fd, uint32_t off, uint32_t whence) {
    if (!card_owns_fd(fd)) return 0xFFFFFFFF;
    __typeof__(fds[0]) *f = &fds[fd - FD_BASE];
    f->pos = whence == 0 ? (int)off : whence == 1 ? f->pos + (int)off : f->size + (int)off;
    return (uint32_t)f->pos;
}

uint32_t card_close(uint32_t fd) {
    if (!card_owns_fd(fd)) return 0xFFFFFFFF;
    fds[fd - FD_BASE].used = 0;
    return fd;
}

// firstfile/nextfile: rellenan una struct DIRENTRY {char name[20]; long attr, size;
// DIRENTRY *next; long head; char system[4];} con el siguiente archivo que encaje.
static char pattern[24];
static int search_pos;

static int match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') return 1;
        if (!*s || (*pat != '?' && *pat != *s)) return 0;
    }
    return !*s;
}

uint32_t card_nextfile(uint32_t de) {
    for (; search_pos < 16; search_pos++) {
        int b = search_pos;
        if (dir(b)[0] != 0x51) continue;
        char name[21];
        memcpy(name, dir(b) + 10, 20); name[20] = 0;
        if (!match(pattern, name)) continue;
        search_pos++;
        for (int i = 0; i < 20; i++) WR8(de + i, (uint8_t)name[i]);
        WR32(de + 20, 0x50);
        WR32(de + 24, dir_size(b));
        WR32(de + 28, 0);
        WR32(de + 32, (uint32_t)b);
        WR32(de + 36, 0);
        return de;
    }
    return 0;
}

uint32_t card_firstfile(uint32_t a, uint32_t de) {
    char buf[64]; int port;
    const char *name = card_path(a, buf, &port);
    if (!name || port != 0 || !present || !formatted()) return 0;
    snprintf(pattern, sizeof pattern, "%s", *name ? name : "*");
    search_pos = 1;
    return card_nextfile(de);
}

uint32_t card_erase(uint32_t a) {
    char buf[64]; int port;
    const char *name = card_path(a, buf, &port);
    if (!name || port != 0 || !formatted()) return 0;
    int b = find_file(name);
    if (!b) return 0;
    erase_file(b);
    return 1;
}

uint32_t card_format(uint32_t a) {
    char buf[64]; int port;
    if (!card_path(a, buf, &port) || port != 0 || !present) return 0;
    format_card();
    if (log_level) fprintf(stderr, "[card] format\n");
    return 1;
}

// ---------------------------------------------------------------- funciones de bajo nivel
// _card_info(puerto): ¿hay tarjeta? _card_load(puerto): lee el directorio (EvSpNEW si la
// tarjeta no está formateada). El resultado llega como evento SwCARD.
void card_info(uint32_t port) {
    int slot = (int)(port & 0x10 ? 1 : 0);
    deliver(EV_SW, slot == 0 && present ? SP_IOE : SP_TIMEOUT);
}
void card_load(uint32_t port) {
    int slot = (int)(port & 0x10 ? 1 : 0);
    deliver(EV_SW, slot != 0 || !present ? SP_TIMEOUT : formatted() ? SP_IOE : SP_NEW);
}

// write_card_sector / read_card_sector (B0 4E/4F): acceso directo por sectores de 128 bytes.
uint32_t card_sector(uint32_t port, uint32_t sector, uint32_t addr, int write) {
    if ((port & 0x10) || !present || sector >= CARD_SIZE / FRAME) { deliver(EV_HW, SP_TIMEOUT); return 1; }
    uint8_t *f = frame_at((int)sector);
    for (int i = 0; i < FRAME; i++) {
        if (write) f[i] = (uint8_t)RD8(addr + (uint32_t)i); else WR8(addr + (uint32_t)i, f[i]);
    }
    if (write) memcard_dirty = 1;
    deliver(EV_HW, SP_IOE);
    return 1;
}

// Atajo de prueba: con card_test_save, el menú de cargar partida (CONTINUE -> tarjeta) abre el
// de guardar, que normalmente solo aparece tras pasar un nivel. Mismo objeto de menú.
int card_test_save;
int card_hook_menu(CPU *c, uint32_t addr) {
    (void)addr;
    if (!card_test_save) return 0;
    dispatch(c, 0x800193d8u);
    return 1;
}
