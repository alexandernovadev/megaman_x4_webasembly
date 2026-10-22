"""Servidor local de la versión web: como `python3 -m http.server`, pero pide al navegador que
revalide cada archivo (Cache-Control: no-cache). Así, tras recompilar, nunca se queda con un
x4.wasm o worker.js viejos mezclados con la página nueva.

También sirve el paquete HD (carpeta hd_pack/, ver web/hd.js) y recibe las capturas de piezas
nuevas que manda el juego mientras se juega (hd_queue/); si existe el entorno de la IA
(~/tools/hd-ai/venv), arranca tools/hd/upscale.py para convertirlas a HD sobre la marcha.
Todo es local: el paquete sale del disco del usuario y no se distribuye.

Uso: python3 tools/serve.py [puerto]   (sirve la carpeta web/)
"""
import functools
import http.server
import os
import struct
import subprocess
import sys
import threading
import time
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PACK = os.path.join(ROOT, 'hd_pack')
QUEUE = os.path.join(ROOT, 'hd_queue')
AI_PY = os.path.expanduser('~/tools/hd-ai/venv/bin/python')

hd_list = []          # huellas en el orden en que estuvieron listas
hd_lock = threading.Lock()


def scan_pack():
    """Añade a la lista las piezas nuevas del paquete (también las que pone otro proceso)."""
    os.makedirs(PACK, exist_ok=True)
    os.makedirs(QUEUE, exist_ok=True)
    found = sorted(f[:16] for f in os.listdir(PACK) if f.endswith('.x4h'))
    with hd_lock:
        have = set(hd_list)
        hd_list.extend(h for h in found if h not in have)


def rescan():
    while True:
        time.sleep(5)
        scan_pack()


def run_upscaler():
    if not os.path.exists(AI_PY):
        print('HD: sin entorno de IA en', AI_PY, '- solo se sirven las piezas ya hechas', flush=True)
        return
    script = os.path.join(ROOT, 'tools', 'hd', 'upscale.py')
    while True:
        p = subprocess.Popen([AI_PY, '-I', script, '--watch', QUEUE, PACK], stdout=subprocess.PIPE, text=True)
        for line in p.stdout:
            if line.startswith('HD '):
                with hd_lock:
                    hd_list.append(line[3:19])
            elif line.strip() == 'LISTO':
                print('HD: IA lista, convirtiendo piezas nuevas a 8x mientras juegas', flush=True)
        p.wait()
        print('HD: la IA se detuvo (código %s); reintento en 5 s' % p.returncode, flush=True)
        time.sleep(5)


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def log_message(self, fmt, *args):
        if not self.path.startswith('/hd/'):
            super().log_message(fmt, *args)

    def reply(self, body, ctype='application/octet-stream'):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith('/hd/list'):
            since = 0
            if '?since=' in self.path:
                try:
                    since = int(self.path.split('?since=')[1])
                except ValueError:
                    pass
            with hd_lock:
                part = hd_list[since:]
                total = len(hd_list)
            return self.reply(('%d\n' % total + '\n'.join(part)).encode(), 'text/plain')
        super().do_GET()

    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n)
        if self.path == '/hd/capture':
            name = '%d_%d.bin' % (time.time_ns(), threading.get_ident())
            with open(os.path.join(QUEUE, name + '.tmp'), 'wb') as f:
                f.write(body)
            os.replace(os.path.join(QUEUE, name + '.tmp'), os.path.join(QUEUE, name))
            return self.reply(b'ok', 'text/plain')
        if self.path == '/hd/batch':
            out = bytearray()
            for h in body.decode().split('\n'):
                h = h.strip()
                if len(h) != 16 or not all(c in '0123456789abcdef' for c in h):
                    continue
                try:
                    with open(os.path.join(PACK, h + '.x4h'), 'rb') as f:
                        data = zlib.decompress(f.read())
                except OSError:
                    continue
                out += struct.pack('<QI', int(h, 16), len(data)) + data
            return self.reply(bytes(out))
        self.send_error(404)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    scan_pack()
    print('HD: %d piezas en el paquete' % len(hd_list), flush=True)
    threading.Thread(target=run_upscaler, daemon=True).start()
    threading.Thread(target=rescan, daemon=True).start()
    web = os.path.join(ROOT, 'web')
    handler = functools.partial(Handler, directory=web)
    http.server.ThreadingHTTPServer(('', port), handler).serve_forever()
