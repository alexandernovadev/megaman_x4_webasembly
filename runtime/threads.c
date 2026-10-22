// Hilos de la BIOS (OpenThread/ChangeThread/CloseThread) sobre corrutinas nativas.
// Cada hilo PS1 tiene su propio juego de registros y su propia pila C, porque el código
// recompilado anida llamadas en la pila nativa.
#ifndef __EMSCRIPTEN__
#define _XOPEN_SOURCE 700  // ucontext en macOS
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

#define LOG(...) do { if (log_level) fprintf(stderr, __VA_ARGS__); } while (0)
#define MAX_THREADS 8
#define NATIVE_STACK (1024 * 1024)
// Las pilas deben ir alineadas a 16: LLVM lo da por hecho (p. ej. calcula `pila + 8` como
// `pila | 8`) y el malloc de Emscripten solo garantiza 8. Una pila desalineada corrompe
// direcciones en silencio según cómo caiga el heap.
#define STACK_ALLOC() aligned_alloc(16, NATIVE_STACK)

#ifdef __EMSCRIPTEN__
#include <emscripten/fiber.h>
typedef emscripten_fiber_t Coro;
#else
#include <ucontext.h>
typedef ucontext_t Coro;
#endif

typedef struct {
    int used, started;
    uint32_t pc, sp, gp;
    CPU regs;
    Coro coro;
    void *stack;
#ifdef __EMSCRIPTEN__
    void *astack;
#endif
} Thread;

static Thread threads[MAX_THREADS];
static int current;  // 0 = hilo principal

static void thread_entry(void *arg) {
    Thread *t = &threads[(intptr_t)arg];
    CPU *c = cur_cpu;
    c->r[29] = t->sp;
    c->r[30] = t->sp;
    c->r[28] = t->gp;
    c->r[31] = 0;
    LOG("[hilo] arranca %d en %08x\n", (int)(intptr_t)arg, t->pc);
    dispatch(c, t->pc);
    fprintf(stderr, "FATAL: el hilo %d terminó su función de entrada\n", (int)(intptr_t)arg);
    exit(1);
}

#ifndef __EMSCRIPTEN__
static void thread_entry_uc(int idx) { thread_entry((void *)(intptr_t)idx); }
#endif

uint32_t bios_OpenThread(uint32_t pc, uint32_t sp, uint32_t gp) {
    threads[0].used = threads[0].started = 1;
    for (int i = 1; i < MAX_THREADS; i++) {
        if (threads[i].used) continue;
        Thread *t = &threads[i];
        t->used = 1;
        t->started = 0;
        t->pc = pc;
        t->sp = sp;
        t->gp = gp;
        // Pilas nuevas en cada apertura: la fibra anterior de este hueco puede seguir
        // suspendida y Asyncify no admite reinicializar una pila en uso. Las viejas se
        // liberan en la siguiente apertura, cuando ya no puede quedar nada apuntándolas.
        static void *zombie[2];
        free(zombie[0]);
        free(zombie[1]);
        zombie[0] = t->stack;
        t->stack = STACK_ALLOC();
#ifdef __EMSCRIPTEN__
        zombie[1] = t->astack;
        t->astack = STACK_ALLOC();
        emscripten_fiber_init(&t->coro, thread_entry, (void *)(intptr_t)i,
                              t->stack, NATIVE_STACK, t->astack, NATIVE_STACK);
#else
        getcontext(&t->coro);
        t->coro.uc_stack.ss_sp = t->stack;
        t->coro.uc_stack.ss_size = NATIVE_STACK;
        t->coro.uc_link = NULL;
        makecontext(&t->coro, (void (*)(void))thread_entry_uc, 1, i);
#endif
        LOG("[hilo] OpenThread(%08x, sp=%08x) -> %d\n", pc, sp, i);
        return 0xFF000000u | i;
    }
    return 0xFFFFFFFFu;
}

int bios_CloseThread(uint32_t h) {
    int i = h & 0xFF;
    if (i > 0 && i < MAX_THREADS) threads[i].used = 0;
    return 1;
}

void bios_ChangeThread(CPU *c, uint32_t h) {
    int next = h & 0xFF;
    if (next >= MAX_THREADS || !threads[next].used || next == current) { c->r[2] = 1; return; }
    if (!threads[0].used) threads[0].used = threads[0].started = 1;
#ifdef __EMSCRIPTEN__
    if (!threads[0].coro.stack_base && current == 0) emscripten_fiber_init_from_current_context(
        &threads[0].coro, threads[0].astack = STACK_ALLOC(), NATIVE_STACK);
#endif
    Thread *from = &threads[current], *to = &threads[next];
    LOG("[hilo] ChangeThread %d -> %d\n", current, next);
    c->r[2] = 1;  // ChangeThread devuelve 1 cuando el hilo vuelve a ejecutarse
    from->regs = *c;
    current = next;
    if (to->started) {
        *c = to->regs;
    } else {
        to->started = 1;
    }
#ifdef __EMSCRIPTEN__
    emscripten_fiber_swap(&from->coro, &to->coro);
#else
    swapcontext(&from->coro, &to->coro);
#endif
}
