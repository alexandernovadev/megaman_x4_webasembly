"""Recompilador estático MIPS I (PS1) -> C.

Lee el PS-X EXE, determina los límites de cada función (semillas de Ghidra +
destinos de jal + punteros a código en los datos) y genera una función C por
cada función MIPS. Cada instrucción se traduce literalmente sobre el estado
`CPU` (registros, hi/lo, cop0, GTE) y la memoria emulada del runtime.

Uso: python3 tools/recomp.py extracted/SLUS_005.61 data/functions_ghidra.json build/gen
"""
import json
import re
import os
import struct
import sys

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
     't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
     't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']

# Funciones sustituidas por implementaciones de alto nivel en el runtime.
HLE = {
    0x800e4db0: 'hle_VSync',
}

# Parches: la instrucción de esa dirección se sustituye por este C. Panorámica (runtime/
# widescreen.c): con ws_reveal = ws_cols = 0 cada parche equivale a la instrucción original.
# Direcciones de las rutinas del juego tomadas del análisis de MegaManX4Recomp (MIT).
PATCHES = {
    # Fondos (0x80026AA0): empezar ws_cols columnas antes y dibujar 21 + 2*ws_cols.
    0x80026b10: 'c->r[22] = 0 - c->r[2] - (uint32_t)(ws_cols * 16);',   # subu s6,zero,v0
    0x80026b20: 'c->r[21] = (c->r[2] - (uint32_t)ws_cols) & 0x1fu;',    # andi s5,v0,0x1f
    0x80026c98: 'c->r[2] = c->r[10] < (uint32_t)(21 + 2 * ws_cols);',   # sltiu v0,t2,21
    # Descarte de objetos (0x800D46F4): x de pantalla dentro de la anchura panorámica.
    **{a: 'c->r[2] = c->r[2] + (uint32_t)ws_reveal < (uint32_t)(320 + 2 * ws_reveal);'
       for a in (0x800d4774, 0x800d4804, 0x800d4894, 0x800d4924)},
    # Ventana de aparición de enemigos (0x80028E24): [x-48, x+368] ensanchada.
    0x80028e64: 'c->r[5] = c->r[5] + (uint32_t)(368 + ws_reveal);',
    0x80028f34: 'c->r[5] = c->r[5] + (uint32_t)(368 + ws_reveal);',
    0x80028e80: 'c->r[4] = c->r[5] + (uint32_t)(-48 - ws_reveal);',
    0x80028f28: 'c->r[4] = c->r[5] + (uint32_t)(-48 - ws_reveal);',
    # Armas de los jefes (0x80172219, que también marca los jefes vencidos): en el modo Completo
    # el jugador (0x80035240) y el menú de pausa (0x8002FD70) las ven todas (ver cheats.c).
    0x8003525c: 'c->r[4] = cheat_weapons(RD8(c->r[4] + (uint32_t)8729));',
    0x8002ff54: 'c->r[2] = cheat_weapons(RD8(c->r[2] + (uint32_t)8729));',
}

# Ganchos al entrar en una función: `if (gancho(c, dirección)) return;` (1 = ya resuelta).
HOOKS = {
    0x80026aa0: 'ws_hook_bg_layer',        # dibujo de una capa de fondo: rellena las columnas extra
    0x8002b1e8: 'ws_hook_actor_bounds',    # ventanas de actividad con margen por parámetro
    0x8002b318: 'ws_hook_actor_bounds',
    0x8002b160: 'ws_hook_fixed_window',    # variantes con margen fijo
    0x8002b288: 'ws_hook_fixed_window',
    0x8002b3c0: 'ws_hook_fixed_window',
    0x8001a9ec: 'card_hook_menu',          # menú de cargar (atajo de prueba: abre el de guardar)
    0x80029a48: 'cheat_hook_select',       # selección de personaje, cursor en X
    0x80029bd8: 'cheat_hook_select',       # selección de personaje, cursor en Zero
    0x80035240: 'cheat_hook_player',       # inicio del jugador al empezar un nivel (modo Completo)
}

NUM_FILES = 48


def reg(n):
    return '0' if n == 0 else f'c->r[{n}]'


class Exe:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.pc0, _gp, self.base, size = struct.unpack('<4I', d[16:32])
        self.code = d[0x800:0x800 + size]
        self.end = self.base + len(self.code)

    def word(self, addr):
        o = addr - self.base
        return struct.unpack('<I', self.code[o:o + 4])[0]

    def inside(self, addr):
        return self.base <= addr < self.end and addr % 4 == 0


# Falsos inicios de función que parten una rutina en dos (el salto de vuelta cruzaría la
# frontera). 0x8001cd40 es el final del bucle firstfile/nextfile de 0x8001cc5c; 0x8005cd40,
# el final de 0x8005ccfc (lo "llama" un jal falso en datos, 0x800edc6c); salía en el modo demo.
NOT_FUNCS = {0x8001cd40, 0x8005cd40}


def find_functions(exe, seeds, code_end):
    """Semillas de Ghidra + destinos de jal + punteros alineados a función."""
    code_start = min(a for a in seeds if exe.inside(a))  # antes hay datos (cadenas, tablas)
    funcs = set(a for a in seeds if exe.inside(a))
    funcs.add(exe.pc0)
    for a in range(exe.base, code_end, 4):
        w = exe.word(a)
        if w >> 26 == 3:
            t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if code_start <= t < code_end:
                funcs.add(t)
    # Constantes de dirección construidas con lui+addiu/ori que apuntan a código: callbacks.
    for a in range(exe.base, code_end - 4, 4):
        w = exe.word(a)
        if w >> 26 != 0x0F:
            continue
        rt, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
        for b in range(a + 4, min(a + 32, code_end), 4):
            w2 = exe.word(b)
            op2, rs2 = w2 >> 26, (w2 >> 21) & 31
            if op2 in (0x09, 0x0D) and rs2 == rt:
                lo = w2 & 0xFFFF
                if op2 == 0x09 and lo & 0x8000:
                    lo -= 0x10000
                t = (hi + lo) & 0xFFFFFFFF
                if code_start <= t < code_end and t % 4 == 0:
                    funcs.add(t)
                break
    # Punteros a código en datos: palabra que apunta justo después de un `jr ra`+delay.
    data_words = list(range(exe.base, code_start, 4)) + list(range(code_end, exe.end, 4))
    for a in data_words:
        v = exe.word(a)
        if code_start + 8 <= v < code_end and v % 4 == 0 and exe.word(v - 8) == 0x03E00008:
            funcs.add(v)
    return sorted(funcs - NOT_FUNCS)


def find_setjmp_resumes(exe, funcs, code_end):
    """Puntos tras `jal setjmp`: el código puede reanudarse ahí cuando la BIOS restaura el jmp_buf."""
    stubs = {f for f in funcs if f + 12 <= code_end and exe.word(f) == 0x240A00A0
             and exe.word(f + 4) == 0x01400008 and exe.word(f + 8) == 0x24090013}
    resumes = set()
    for a in range(exe.base, code_end, 4):
        w = exe.word(a)
        if w >> 26 == 3 and ((a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) in stubs:
            resumes.add(a + 8)
    return resumes


class FuncGen:
    def __init__(self, exe, start, end, funcs_set, resumes=()):
        self.exe, self.start, self.end, self.funcs = exe, start, end, funcs_set
        self.resumes = sorted(r for r in resumes if start <= r < end)
        self.out = []

    def emit(self, s):
        self.out.append(s)

    def in_func(self, t):
        return self.start <= t < self.end

    def goto_or_tail(self, t, pc=None):
        if self.in_func(t):
            # Los saltos hacia atrás (bucles) también hacen avanzar el reloj emulado.
            if pc is not None and t <= pc:
                return f'{{ CLOCK_TICK; goto L_{t:08x}; }}'
            return f'goto L_{t:08x};'
        return f'{{ CALL(0x{t:08x}); return; }}'

    # ---- traducción de una instrucción no-salto ----
    def simple(self, pc, w):
        if pc in PATCHES:
            return PATCHES[pc]
        op = w >> 26
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        sa, fn = (w >> 6) & 31, w & 63
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        S, T, D = reg(rs), reg(rt), reg(rd)

        def wr(dst, expr):
            return f'{dst} = {expr};' if dst != '0' else f'(void)({expr});'

        if w == 0:
            return '/* nop */'
        if op == 0:
            ops = {
                0x00: f'{T} << {sa}', 0x02: f'{T} >> {sa}', 0x03: f'(uint32_t)((int32_t){T} >> {sa})',
                0x04: f'{T} << ({S} & 31)', 0x06: f'{T} >> ({S} & 31)',
                0x07: f'(uint32_t)((int32_t){T} >> ({S} & 31))',
                0x20: f'{S} + {T}', 0x21: f'{S} + {T}', 0x22: f'{S} - {T}', 0x23: f'{S} - {T}',
                0x24: f'{S} & {T}', 0x25: f'{S} | {T}', 0x26: f'{S} ^ {T}', 0x27: f'~({S} | {T})',
                0x2A: f'(int32_t){S} < (int32_t){T}', 0x2B: f'{S} < {T}',
            }
            if fn in ops:
                return f'{D} = {ops[fn]};' if rd else '/* write to $zero */'
            if fn == 0x10:
                return wr(D, 'c->hi')
            if fn == 0x12:
                return wr(D, 'c->lo')
            if fn == 0x11:
                return f'c->hi = {S};'
            if fn == 0x13:
                return f'c->lo = {S};'
            if fn == 0x18:
                return f'{{ int64_t m = (int64_t)(int32_t){S} * (int32_t){T}; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); }}'
            if fn == 0x19:
                return f'{{ uint64_t m = (uint64_t){S} * {T}; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); }}'
            if fn == 0x1A:
                return f'op_div(c, {S}, {T});'
            if fn == 0x1B:
                return f'op_divu(c, {S}, {T});'
            if fn == 0x0C:
                return f'op_syscall(c, 0x{pc:08x});'
            if fn == 0x0D:
                return f'op_break(c, 0x{pc:08x});'
        if op == 0x0F:
            return wr(T, f'0x{imm << 16:08x}u')
        alu = {
            0x08: f'{S} + (uint32_t){simm}', 0x09: f'{S} + (uint32_t){simm}',
            0x0A: f'(int32_t){S} < {simm}', 0x0B: f'{S} < (uint32_t){simm}',
            0x0C: f'{S} & 0x{imm:x}u', 0x0D: f'{S} | 0x{imm:x}u', 0x0E: f'{S} ^ 0x{imm:x}u',
        }
        if op in alu:
            return wr(T, alu[op])
        A = f'{S} + (uint32_t){simm}'
        loads = {0x20: f'(uint32_t)(int8_t)RD8({A})', 0x21: f'(uint32_t)(int16_t)RD16({A})',
                 0x23: f'RD32({A})', 0x24: f'RD8({A})', 0x25: f'RD16({A})'}
        if op in loads:
            return wr(T, loads[op])
        if op == 0x22:
            return f'{T if rt else "c->dummy"} = op_lwl(c, {A}, {T});'
        if op == 0x26:
            return f'{T if rt else "c->dummy"} = op_lwr(c, {A}, {T});'
        if op == 0x28:
            return f'WR8({A}, {T});'
        if op == 0x29:
            return f'WR16({A}, {T});'
        if op == 0x2B:
            return f'WR32({A}, {T});'
        if op == 0x2A:
            return f'op_swl(c, {A}, {T});'
        if op == 0x2E:
            return f'op_swr(c, {A}, {T});'
        if op == 0x10:  # COP0
            if rs == 0:
                return wr(T, f'c->cop0[{rd}]')
            if rs == 4:
                return f'c->cop0[{rd}] = {T};'
            if rs == 0x10 and fn == 0x10:
                return 'op_rfe(c);'
        if op == 0x12:  # COP2 (GTE)
            if rs == 0:
                return wr(T, f'gte_read_data(c, {rd})')
            if rs == 2:
                return wr(T, f'gte_read_ctrl(c, {rd})')
            if rs == 4:
                return f'gte_write_data(c, {rd}, {T});'
            if rs == 6:
                return f'gte_write_ctrl(c, {rd}, {T});'
            if rs & 0x10:
                return f'gte_command(c, 0x{w:08x});'
        if op == 0x32:
            return f'gte_write_data(c, {rt}, RD32({A}));'
        if op == 0x3A:
            return f'WR32({A}, gte_read_data(c, {rt}));'
        return f'op_unknown(c, 0x{pc:08x}, 0x{w:08x});'

    def gen(self):
        exe = self.exe
        name = f'f_{self.start:08x}'
        self.emit(f'void {name}(CPU *c) {{')
        self.emit('  uint32_t entry_ra = c->r[31]; (void)entry_ra;')
        self.emit(f'  TRACE(0x{self.start:08x});')
        self.emit('  CLOCK_TICK;')
        if self.start in HOOKS:
            self.emit(f'  if ({HOOKS[self.start]}(c, 0x{self.start:08x}u)) return;')
        if self.resumes:
            cases = ' '.join(f'case 0x{r:08x}: goto L_{r:08x};' for r in self.resumes)
            self.emit(f'  if (resume_pc) {{ uint32_t t = resume_pc; resume_pc = 0; c->r[2] = 1; switch (t) {{ {cases} }} }}')
        pc = self.start
        all_pcs = list(range(self.start, self.end, 4))
        while pc < self.end:
            w = exe.word(pc)
            op = w >> 26
            rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            fn = w & 63
            simm = (w & 0xFFFF) - 0x10000 if w & 0x8000 else (w & 0xFFFF)
            S, T = reg(rs), reg(rt)
            self.emit(f'L_{pc:08x}:;')
            has_delay = pc + 4 < self.end
            delay = self.simple(pc + 4, exe.word(pc + 4)) if has_delay else ''
            btarget = (pc + 4 + (simm << 2)) & 0xFFFFFFFF
            cond = None
            if op == 4:
                cond = f'{S} == {T}'
            elif op == 5:
                cond = f'{S} != {T}'
            elif op == 6:
                cond = f'(int32_t){S} <= 0'
            elif op == 7:
                cond = f'(int32_t){S} > 0'
            elif op == 1 and rt in (0, 1, 0x10, 0x11):
                cond = f'(int32_t){S} < 0' if rt & 1 == 0 else f'(int32_t){S} >= 0'
            if cond is not None:
                link = f' c->r[31] = 0x{pc + 8:08x};' if op == 1 and rt & 0x10 else ''
                if link:
                    self.emit(f'  {{ int cnd = {cond};{link} {delay} if (cnd) {{ CALL(0x{btarget:08x}); }} goto L_{pc + 8:08x}; }}')
                else:
                    self.emit(f'  {{ int cnd = {cond}; {delay} if (cnd) {self.goto_or_tail(btarget, pc)} {self.goto_or_tail(pc + 8) if pc + 8 >= self.end else f"goto L_{pc + 8:08x};"} }}')
                pc += 4
                self.emit(f'L_{pc:08x}:;')
                self.emit(f'  {delay}')
                pc += 4
                continue
            if op in (2, 3):
                t = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if op == 2:
                    self.emit(f'  {delay} {self.goto_or_tail(t, pc)}')
                else:
                    self.emit(f'  {delay} c->r[31] = 0x{pc + 8:08x}; CALL(0x{t:08x});')
                    if pc + 8 >= self.end:
                        self.emit(f'  {{ CALL(0x{pc + 8:08x}); return; }}')
                    else:
                        self.emit(f'  goto L_{pc + 8:08x};')
                pc += 4
                self.emit(f'L_{pc:08x}:;')
                self.emit(f'  {delay}')
                pc += 4
                continue
            if op == 0 and fn in (8, 9):
                if fn == 9:
                    self.emit(f'  {{ uint32_t tgt = {S}; {delay} {reg(rd)} = 0x{pc + 8:08x}; DISPATCH(tgt); }}'.replace('0 = ', '(void)'))
                    if pc + 8 >= self.end:
                        self.emit(f'  {{ CALL(0x{pc + 8:08x}); return; }}')
                    else:
                        self.emit(f'  goto L_{pc + 8:08x};')
                elif rs == 31:
                    self.emit(f'  {{ uint32_t tgt = c->r[31]; {delay} if (tgt != entry_ra) bad_return(c, 0x{pc:08x}, tgt, entry_ra); return; }}')
                else:
                    cases = ' '.join(f'case 0x{a:08x}: goto L_{a:08x};' for a in all_pcs)
                    self.emit(f'  {{ uint32_t tgt = {S}; {delay} switch (tgt) {{ {cases} default: DISPATCH(tgt); return; }} }}')
                pc += 4
                self.emit(f'L_{pc:08x}:;')
                self.emit(f'  {delay}')
                pc += 4
                continue
            self.emit(f'  {self.simple(pc, w)}')
            pc += 4
        # Caída al final del rango: continúa en la siguiente función (cola).
        self.emit(f'  CALL(0x{self.end:08x}); return;')
        self.emit('}')
        text = '\n'.join(self.out)

        def call(m):
            t = int(m[1], 16)
            return f'f_{m[1]}(c)' if t in self.funcs else f'DISPATCH(0x{m[1]}u)'
        text = re.sub(r'CALL\(0x([0-9a-f]{8})\)', call, text)
        # Tras cada llamada: si el runtime está deshaciendo la pila (fin de IRQ), salir.
        text = re.sub(r'(?<!void )(f_[0-9a-f]{8}\(c\);|DISPATCH\([^;]*\);)', r'\1 UNWIND_CHECK;', text)
        return text


def main():
    exe_path, seeds_path, out_dir = sys.argv[1:4]
    exe = Exe(exe_path)
    seeds = [int(x, 16) for x in json.load(open(seeds_path))]
    code_end = 0x800EE400  # fin del código (inicio de datos), observado en Ghidra
    # Ajustar al final real de la última función: primer `jr ra`+delay tras la última semilla.
    last = max(s for s in seeds if s < 0x80100000)
    a = last
    while exe.word(a) != 0x03E00008:
        a += 4
    code_end = a + 8
    extra_path = os.path.join(os.path.dirname(seeds_path), 'extra_entries.json')
    if os.path.exists(extra_path):
        seeds += [int(x, 16) for x in json.load(open(extra_path))]
    funcs = find_functions(exe, seeds, code_end)
    funcs = [f for f in funcs if f < code_end]
    resumes = find_setjmp_resumes(exe, funcs, code_end)
    os.makedirs(out_dir, exist_ok=True)
    bounds = funcs + [code_end]
    hdr = ['#pragma once', '#include "runtime.h"']
    for f in funcs:
        hdr.append(f'void f_{f:08x}(CPU *c);')
    open(os.path.join(out_dir, 'funcs.h'), 'w').write('\n'.join(hdr) + '\n')
    chunks = [[] for _ in range(NUM_FILES)]
    for i, f in enumerate(funcs):
        end = bounds[i + 1]
        if f in HLE:
            body = f'void f_{f:08x}(CPU *c) {{ {HLE[f]}(c); }}'
        else:
            body = FuncGen(exe, f, end, set(funcs), resumes).gen()
        chunks[i * NUM_FILES // len(funcs)].append(body)
    for n, ch in enumerate(chunks):
        with open(os.path.join(out_dir, f'gen_{n:02d}.c'), 'w') as fh:
            fh.write('#include "funcs.h"\n\n' + '\n\n'.join(ch) + '\n')
    tbl = ['#include "funcs.h"', 'const FuncEntry func_table[] = {']
    tbl += [f'  {{0x{f:08x}u, f_{f:08x}}},' for f in funcs]
    tbl += ['};', f'const int func_count = {len(funcs)};',
            f'const uint32_t code_end = 0x{code_end:08x}u;',
            f'const uint32_t exe_entry = 0x{exe.pc0:08x}u;']
    open(os.path.join(out_dir, 'table.c'), 'w').write('\n'.join(tbl) + '\n')
    print(f'{len(funcs)} funciones (Ghidra: {len(seeds)}), código hasta 0x{code_end:08x}, {NUM_FILES} archivos C')


if __name__ == '__main__':
    main()
