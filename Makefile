EXE     := extracted/SLUS_005.61
GEN     := build/gen
CFLAGS  := -O1 -std=c11 -Iruntime -I$(GEN) -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality -Wno-tautological-compare
EMSDK   := $(HOME)/tools/emsdk

GEN_SRC := $(wildcard $(GEN)/gen_*.c) $(GEN)/table.c
CORE_SRC := runtime/runtime.c runtime/cdrom.c runtime/threads.c runtime/gpu.c runtime/mdec.c runtime/audio.c runtime/spu.c runtime/widescreen.c runtime/memcard.c runtime/texdump.c runtime/hd.c
RT_SRC  := $(CORE_SRC) runtime/main.c

SHELL   := /bin/bash

.PHONY: all gen native wasm web clean

all: native

gen: $(GEN)/table.c

$(GEN)/table.c: tools/recomp.py $(EXE) data/functions_ghidra.json
	python3 -I tools/recomp.py $(EXE) data/functions_ghidra.json $(GEN)

# Binario nativo para depurar en la Mac.
NATIVE_OBJ := $(patsubst %.c,build/native/%.o,$(notdir $(GEN_SRC) $(RT_SRC)))
native: gen
	$(MAKE) build/x4
build/x4: $(NATIVE_OBJ)
	cc -o $@ $^
build/native/%.o: $(GEN)/%.c runtime/runtime.h | build/native
	cc $(CFLAGS) -c $< -o $@
build/native/%.o: runtime/%.c runtime/runtime.h runtime/hd.h | build/native
	cc $(CFLAGS) -c $< -o $@
build/native:
	mkdir -p $@

# WebAssembly (Emscripten). `wasm` es la versión de línea de comandos para Node (pruebas);
# `web` es la del navegador, que corre en un Web Worker y lee el disco con WORKERFS.
EMCC := source $(EMSDK)/emsdk_env.sh >/dev/null 2>&1 && emcc
WASM_FLAGS := -O2 -sINITIAL_MEMORY=256MB -sSTACK_SIZE=8MB -sFORCE_FILESYSTEM=1 -sASYNCIFY -sASYNCIFY_STACK_SIZE=1048576
WASM_CORE := $(patsubst %.c,build/wasm/%.o,$(notdir $(GEN_SRC) $(CORE_SRC)))
wasm: gen
	$(MAKE) build/x4node.js
build/x4node.js: $(WASM_CORE) build/wasm/main.o
	$(EMCC) -o $@ $^ $(WASM_FLAGS) -sEXIT_RUNTIME=1 -sNODERAWFS=1
web: gen
	$(MAKE) web/x4.js
web/x4.js: $(WASM_CORE) build/wasm/web.o
	$(EMCC) -o $@ $^ $(WASM_FLAGS) -sENVIRONMENT=worker -lworkerfs.js -sINVOKE_RUN=0 \
		-sEXPORTED_RUNTIME_METHODS=callMain,FS,WORKERFS,HEAPU8 -sEXPORTED_FUNCTIONS=_main,_x4_set_pad,_x4_set_paused,_x4_set_widescreen,_x4_set_aa,_x4_add_credits,_x4_card,_x4_card_new,_x4_test_save,_x4_hd_enable,_x4_hd_buf,_x4_hd_known
build/wasm/%.o: $(GEN)/%.c runtime/runtime.h | build/wasm
	$(EMCC) $(CFLAGS) -c $< -o $@
build/wasm/%.o: runtime/%.c runtime/runtime.h runtime/hd.h | build/wasm
	$(EMCC) $(CFLAGS) -c $< -o $@
build/wasm:
	mkdir -p $@

clean:
	rm -rf build web/x4.js web/x4.wasm
