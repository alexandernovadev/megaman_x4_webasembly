// Controlador de CD-ROM de la PS1 a nivel de registros (0x1F801800-0x1F801803).
// Referencia: psx-spx, "CDROM Controller". Lee sectores de la imagen .bin del usuario.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime.h"

#define LOG(...) do { if (log_level) fprintf(stderr, __VA_ARGS__); } while (0)

static FILE *disc;
static int cd_debug = 0;
static long disc_sectors;

static uint8_t index_reg;
static uint8_t params[16], nparams;
static uint8_t resp[16], nresp, resp_pos;
static uint8_t int_enable = 0x1F, int_flag;
static uint8_t mode, stat_byte = 0x02;  // motor encendido
static uint32_t setloc_lba, read_lba;
static int reading;
static uint8_t xa_file, xa_chan, muted;  // Setfilter / Mute

// Sector actual (2352 bytes crudos) y FIFO de datos.
static uint8_t sector[2352];
static uint8_t next[2352];  // siguiente sector leído, a la espera de la CPU si es de datos
static int peeked;
static uint8_t data_fifo[2352];
static int data_len, data_pos;
static int sector_ready;
static uint8_t last_header[8];

// Cola de respuestas pendientes: cada una genera una interrupción INTn.
typedef struct { int type; uint8_t bytes[16]; int n; int64_t delay; } Event;
static Event queue[8];
static int qlen;
// Horario absoluto del disco: el siguiente sector pasa bajo el cabezal en `slot` (ciclos).
// Nunca se adelanta, así que la lectura no supera la velocidad real del lector.
static uint64_t slot;
static uint64_t data_after;  // margen tras cada confirmación antes del siguiente sector de datos

// Tiempos en ciclos de CPU (33,8688 MHz). Aproximados a los de psx-spx.
#define ACK_DELAY 20000LL     // primera respuesta (INT3)
#define DONE_DELAY 300000LL   // segunda respuesta (INT2) / búsqueda
#define SECTOR_2X 225792LL    // 150 sectores/s

static int bcd(int v) { return (v / 10) << 4 | (v % 10); }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

int cd_open(const char *path) {
    disc = fopen(path, "rb");
    if (!disc) return 0;
    fseek(disc, 0, SEEK_END);
    disc_sectors = ftell(disc) / 2352;
    LOG("[cd] disco abierto: %ld sectores\n", disc_sectors);
    return 1;
}

long cd_reads;

int cd_read_raw(uint32_t lba, uint8_t *out) {
    cd_reads++;
    if (!disc || (long)lba >= disc_sectors) return 0;
    fseek(disc, (long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, disc) == 2352;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Busca un fichero en el directorio raíz ISO9660 del disco (p. ej. "SLUS_005.61;1").
// Los sectores son MODE2 Form 1: los datos de usuario empiezan en el byte 24.
int cd_find_file(const char *name, uint32_t *lba, uint32_t *size) {
    uint8_t s[2352];
    if (!cd_read_raw(16, s) || memcmp(s + 25, "CD001", 5)) return 0;
    const uint8_t *root = s + 24 + 156;
    uint32_t dir_lba = le32(root + 2), dir_size = le32(root + 10);
    size_t len = strlen(name);
    for (uint32_t off = 0; off < dir_size; off += 2048) {
        if (!cd_read_raw(dir_lba + off / 2048, s)) return 0;
        const uint8_t *d = s + 24;
        for (int p = 0; p < 2048 && d[p];) {
            const uint8_t *r = d + p;
            if (r[32] == len && !memcmp(r + 33, name, len)) {
                *lba = le32(r + 2);
                *size = le32(r + 10);
                return 1;
            }
            p += r[0];
        }
    }
    return 0;
}

static void push(int type, const uint8_t *b, int n, int64_t delay) {
    if (qlen == 8) { LOG("[cd] cola llena\n"); return; }
    Event *e = &queue[qlen++];
    e->type = type;
    e->n = n;
    memcpy(e->bytes, b, n);
    e->delay = delay;
    clock_deadline = 0;  // el reloj debe recalcular su próximo plazo
}

static void push_stat(int type, int64_t delay) {
    uint8_t s = stat_byte;
    push(type, &s, 1, delay);
}

static void command(uint8_t cmd) {
    if (cd_debug > 0 && cmd == 0x09) LOG("[cd] Pause: cola=%d int_flag=%d reading=%d head_delay=%ld ciclos=%llu\n",
                                         qlen, int_flag, reading, qlen ? queue[0].delay : -1, (unsigned long long)cycles);
    static uint8_t seen[256];
    if (!seen[cmd]) { seen[cmd] = 1; LOG("[cd] comando %02X (%d parámetros)\n", cmd, nparams); }
    if (getenv("X4_CDLOG")) fprintf(stderr, "[cdcmd] %02X p=%02x %02x %02x ciclo %llu lba %u modo %02x\n", cmd, params[0], params[1], params[2], (unsigned long long)cycles, read_lba, mode);
    // Un comando nuevo cancela las respuestas de lectura pendientes.
    switch (cmd) {
        case 0x01: push_stat(3, ACK_DELAY); break;                                  // Getstat
        case 0x02:                                                           // Setloc
            setloc_lba = (unbcd(params[0]) * 60 + unbcd(params[1])) * 75 + unbcd(params[2]) - 150;
            LOG("[cd] Setloc lba %u\n", setloc_lba);
            push_stat(3, ACK_DELAY);
            break;
        case 0x03: push_stat(3, ACK_DELAY); break;                                  // Play (CD-DA: no hay)
        case 0x06: case 0x1B:                                                // ReadN / ReadS
            read_lba = setloc_lba;
            peeked = 0;
            reading = 1;
            clock_deadline = 0;
            stat_byte = 0x22;
            slot = cycles + DONE_DELAY;
            push_stat(3, ACK_DELAY);
            break;
        case 0x07: stat_byte = 0x02; push_stat(3, ACK_DELAY); push_stat(2, DONE_DELAY); break;  // MotorOn
        case 0x08: case 0x09:                                                // Stop / Pause
            reading = 0;
            peeked = 0;
            stat_byte = 0x02;
            push_stat(3, ACK_DELAY);
            push_stat(2, DONE_DELAY);
            break;
        case 0x0A:                                                           // Init
            reading = 0;
            peeked = 0;
            mode = 0;
            stat_byte = 0x02;
            push_stat(3, ACK_DELAY);
            push_stat(2, DONE_DELAY);
            break;
        case 0x0B: muted = 1; push_stat(3, ACK_DELAY); break;                       // Mute
        case 0x0C: muted = 0; push_stat(3, ACK_DELAY); break;                       // Demute
        case 0x0D: xa_file = params[0]; xa_chan = params[1]; xa_reset();            // Setfilter
            push_stat(3, ACK_DELAY); break;
        case 0x0E: mode = params[0]; push_stat(3, ACK_DELAY); break;                // Setmode
        case 0x0F: { uint8_t b[5] = {stat_byte, mode, 0, 0, 0}; push(3, b, 5, ACK_DELAY); break; }  // Getparam
        case 0x10: push(3, last_header, 8, ACK_DELAY); break;                       // GetlocL
        case 0x11: {                                                         // GetlocP
            uint32_t l = read_lba + 150;
            uint8_t b[8] = {1, 1, bcd(read_lba / 4500), bcd(read_lba / 75 % 60), bcd(read_lba % 75),
                            bcd(l / 4500), bcd(l / 75 % 60), bcd(l % 75)};
            push(3, b, 8, ACK_DELAY);
            break;
        }
        case 0x13: { uint8_t b[3] = {stat_byte, 1, 1}; push(3, b, 3, ACK_DELAY); break; }  // GetTN
        case 0x14: {                                                         // GetTD
            uint32_t l = params[0] ? 150 : (uint32_t)disc_sectors + 150;
            uint8_t b[3] = {stat_byte, bcd(l / 4500), bcd(l / 75 % 60)};
            push(3, b, 3, ACK_DELAY);
            break;
        }
        case 0x15: case 0x16:                                                // SeekL / SeekP
            read_lba = setloc_lba;
            peeked = 0;
            push_stat(3, ACK_DELAY);
            push_stat(2, DONE_DELAY);
            break;
        case 0x19: {                                                         // Test
            uint8_t b[4] = {0x94, 0x09, 0x19, 0xC0};
            push(3, b, 4, ACK_DELAY);
            break;
        }
        case 0x1A: {                                                         // GetID: disco con licencia SCEA
            uint8_t b[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
            push_stat(3, ACK_DELAY);
            push(2, b, 8, DONE_DELAY);
            break;
        }
        case 0x1E: push_stat(3, ACK_DELAY); push_stat(2, DONE_DELAY); break;               // ReadTOC
        default:
            LOG("[cd] comando %02X sin implementar\n", cmd);
            push_stat(3, ACK_DELAY);
    }
    nparams = 0;
}

static void deliver_next(void) {
    if (int_flag || !qlen) return;
    Event *e = &queue[0];
    if (e->delay > 0) return;
    memcpy(resp, e->bytes, e->n);
    nresp = e->n;
    resp_pos = 0;
    int_flag = e->type;
    memmove(queue, queue + 1, sizeof(Event) * --qlen);
    if (cd_debug > 0) { cd_debug--; LOG("[cd] INT%d (resp %02x, cola %d, enable %02x)\n", int_flag, resp[0], qlen, int_enable); }
    if (int_flag & int_enable) raise_irq(2);
}

double max_seconds;

void cd_tick(int64_t amount) {
    for (int i = 0; i < qlen; i++)
        if (queue[i].delay > 0) { queue[i].delay -= amount; break; }  // solo cuenta la primera
    if (reading) {
        uint64_t period = (mode & 0x80) ? SECTOR_2X : SECTOR_2X * 2;  // velocidad doble/simple
        // El disco gira a velocidad constante: los sectores de audio se consumen a su hora
        // aunque la CPU no haya atendido el anterior (si no, la música XA se queda corta);
        // los de datos esperan a que lo atienda.
        while (reading && cycles >= slot) {
            if (!peeked) {
                if (!cd_read_raw(read_lba, next)) {
                    reading = 0;
                    uint8_t b[2] = {stat_byte | 1, 0x80};
                    push(5, b, 2, 0);
                    break;
                }
                peeked = 1;
            }
            // Sectores XA de audio (submodo bit 2) con XA activado: no llegan a la CPU.
            int is_audio = (next[18] & 0x04) && (mode & 0x40);
            if (!is_audio && (int_flag || qlen || cycles < data_after)) break;
            peeked = 0;
            read_lba++;
            slot += period;
            memcpy(last_header, next + 12, 8);
            if (is_audio) {
                // Con el filtro (modo bit 3) solo suena el archivo/canal elegido.
                if (!muted && (!(mode & 0x08) || (next[16] == xa_file && next[17] == xa_chan)))
                    xa_decode_sector(next);
                continue;
            }
            memcpy(sector, next, sizeof sector);
            sector_ready = 1;
            push_stat(1, 0);
        }
        if (slot + period < cycles) slot = cycles - period;  // como mucho un sector de retraso
    }
    deliver_next();
}

// Ciclos que faltan para el próximo suceso del CD (para que el reloj no se lo salte).
int64_t cd_next_due(void) {
    int64_t due = (int64_t)1 << 40;
    for (int i = 0; i < qlen; i++)
        if (queue[i].delay > 0) { due = queue[i].delay; break; }
    if (reading) {
        int64_t d = slot > cycles ? (int64_t)(slot - cycles) : data_after > cycles ? (int64_t)(data_after - cycles) : due;
        if (d < due) due = d;
    }
    return due;
}

uint8_t cd_read_reg(int r) {
    switch (r) {
        case 0: {
            uint8_t s = index_reg;
            if (nparams == 0) s |= 0x08;
            if (nparams < 16) s |= 0x10;
            if (resp_pos < nresp) s |= 0x20;
            if (data_pos < data_len) s |= 0x40;
            return s;
        }
        case 1: return resp_pos < nresp ? resp[resp_pos++] : 0;
        case 2: return data_pos < data_len ? data_fifo[data_pos++] : 0;
        case 3: return (index_reg & 1) ? (0xE0 | int_flag) : int_enable;
    }
    return 0;
}

void cd_write_reg(int r, uint8_t v) {
    if (r == 0) { index_reg = v & 3; return; }
    switch (index_reg << 4 | r) {
        case 0x01: command(v); break;
        case 0x02: if (nparams < 16) params[nparams++] = v; break;
        case 0x03:  // pedido de datos: carga el sector en la FIFO
            if (v & 0x80) {
                if (sector_ready) {
                    if (mode & 0x20) { memcpy(data_fifo, sector + 12, 2340); data_len = 2340; }
                    else { memcpy(data_fifo, sector + 24, 2048); data_len = 2048; }
                    data_pos = 0;
                    sector_ready = 0;
                }
            } else {
                data_len = data_pos = 0;
            }
            break;
        case 0x12: int_enable = v & 0x1F; break;
        case 0x13:
            if (cd_debug > 0 && (v & 0x1F)) { cd_debug--; LOG("[cd] ack %02x (flag %d)\n", v, int_flag); }
            int_flag &= ~(v & 0x1F);
            if (v & 0x40) nparams = 0;
            if (!int_flag) {
                nresp = resp_pos = 0;
                // Margen para que la CPU copie el sector antes de que llegue el siguiente.
                if (reading) data_after = cycles + SECTOR_2X / 4;
                deliver_next();
                clock_deadline = 0;
            }
            break;
        default: break;  // volumen de audio y mapa de sonido: se ignoran por ahora
    }
}

// DMA canal 3: CD-ROM -> RAM.
void cd_dma(uint32_t madr, uint32_t words) {
    for (uint32_t i = 0; i < words * 4; i++) {
        uint8_t b = data_pos < data_len ? data_fifo[data_pos++] : 0;
        ram[(madr + i) & 0x1FFFFF] = b;
    }
}
