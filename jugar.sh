#!/usr/bin/env bash
# Todo en un comando: prepara Mega Man X4 con TU disco y abre el juego en el navegador.
#
#   ./jugar.sh "ruta/a/Mega Man X4 (USA).bin"    # la primera vez
#   ./jugar.sh                                   # las siguientes (usa el disco de disco/)
#
# Pone el disco en disco/ (un enlace, sin copiar 700 MB si se puede), extrae el ejecutable y los
# datos (extracted/), compila la versión web y arranca el servidor local. En la página basta con
# pulsar "Iniciar juego". Nada sale de tu equipo: disco/, extracted/ y lo compilado están en
# .gitignore.
#
# Hace falta: python3, un compilador de C (en macOS: xcode-select --install) y Emscripten
# (https://emscripten.org/docs/getting_started/downloads.html), en ~/tools/emsdk, en $EMSDK o
# con emcc ya en el PATH.
set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-8765}"
EMSDK="${EMSDK:-$HOME/tools/emsdk}"
export EMSDK

say() { printf '\033[1;34m▶\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m✗\033[0m %s\n' "$*" >&2; exit 1; }

# 1. Herramientas
command -v python3 >/dev/null || fail "Falta python3."
command -v cc >/dev/null || fail "Falta un compilador de C (en macOS: xcode-select --install)."
if [ ! -f "$EMSDK/emsdk_env.sh" ] && ! command -v emcc >/dev/null; then
  fail "Falta Emscripten. Instálalo así y vuelve a ejecutar este script:
    git clone https://github.com/emscripten-core/emsdk.git ~/tools/emsdk
    cd ~/tools/emsdk && ./emsdk install latest && ./emsdk activate latest"
fi

# 2. El disco
mkdir -p disco
if [ $# -ge 1 ]; then
  [ -f "$1" ] || fail "No encuentro el disco: $1"
  rm -f disco/*.bin disco/*.img disco/*.iso
  name="$(basename "$1")"
  ln "$1" "disco/$name" 2>/dev/null || ln -s "$(cd "$(dirname "$1")" && pwd)/$name" "disco/$name" 2>/dev/null \
    || cp "$1" "disco/$name"
  rm -rf extracted  # disco nuevo: se vuelve a extraer
fi
disc="$(ls disco/*.bin disco/*.img disco/*.iso 2>/dev/null | head -n 1 || true)"
[ -n "$disc" ] || fail "Dime dónde está tu disco: ./jugar.sh \"ruta/a/Mega Man X4 (USA).bin\""
say "Disco: $disc"

# 3. Extraer el ejecutable y los datos
if [ ! -f extracted/SLUS_005.61 ]; then
  say "Extrayendo el disco…"
  python3 -I tools/iso_extract.py "$disc" extracted >/dev/null
fi
[ -f extracted/SLUS_005.61 ] || fail "Ese disco no es Mega Man X4 (USA): falta SLUS_005.61. Hace falta la versión USA en .bin (2352 bytes por sector)."

# 4. Compilar (la primera vez tarda varios minutos)
if [ ! -f web/x4.wasm ] || [ -n "$(find runtime tools/recomp.py web/worker.js -newer web/x4.wasm -print -quit)" ]; then
  say "Compilando el juego para el navegador (la primera vez tarda unos minutos)…"
  make web EMSDK="$EMSDK" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)" >/dev/null
fi

# 5. Servidor y navegador
url="http://localhost:$PORT"
if curl -s -o /dev/null "$url/"; then
  say "El servidor ya estaba en marcha."
else
  say "Arrancando el servidor en ${url}…"
  python3 tools/serve.py "$PORT" &
  server=$!
  trap 'kill $server 2>/dev/null' EXIT
  for _ in $(seq 50); do curl -s -o /dev/null "$url/" && break; sleep 0.2; done
fi
if command -v open >/dev/null; then open "$url"; elif command -v xdg-open >/dev/null; then xdg-open "$url"; fi
say "Listo: en la página pulsa «Iniciar juego». Ctrl+C aquí para cerrar el servidor."
wait "${server:-}" 2>/dev/null || true
