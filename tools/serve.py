"""Servidor local de la versión web: como `python3 -m http.server`, pero pide al navegador que
revalide cada archivo (Cache-Control: no-cache). Así, tras recompilar, nunca se queda con un
x4.wasm o worker.js viejos mezclados con la página nueva.

También sirve las voces en español (voice_pack/, en /voice/), la página /grabar, que guarda las
grabaciones de referencia para las voces en voice_refs/, y el paquete HD (carpeta hd_pack/, ver web/hd.js) y recibe las capturas de piezas
nuevas que manda el juego mientras se juega (hd_queue/); si existe el entorno de la IA
(~/tools/hd-ai/venv), arranca tools/hd/upscale.py para convertirlas a HD sobre la marcha.
Si hay un disco en la carpeta disco/ (lo pone jugar.sh), la página lo ofrece en /disco/ para
empezar con un clic. Todo es local: el disco y el paquete son del usuario y no se distribuyen.

Uso: python3 tools/serve.py [puerto]   (sirve la carpeta web/)
"""
import functools
import http.server
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PACK = os.path.join(ROOT, 'hd_pack')
VOICE = os.path.join(ROOT, 'voice_pack')  # voces en español (tools/voice/make_voices.py)
DISC_DIR = os.path.join(ROOT, 'disco')   # el disco del usuario (jugar.sh)
REFS = os.path.join(ROOT, 'voice_refs')   # grabaciones de referencia (web/grabar.html)
REF_NAME = r'[a-z]+_[0-9]{1,2}\.wav'
QUEUE = os.path.join(ROOT, 'hd_queue')
AI_PY = os.path.expanduser('~/tools/hd-ai/venv/bin/python')

hd_list = []          # huellas en el orden en que estuvieron listas
hd_lock = threading.Lock()


def local_disc():
    """Primer disco de disco/ (.bin, .img o .iso), o None."""
    try:
        names = sorted(f for f in os.listdir(DISC_DIR) if f.lower().endswith(('.bin', '.img', '.iso')))
    except OSError:
        return None
    return os.path.join(DISC_DIR, names[0]) if names else None


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
        if self.path.startswith('/disco/'):
            disc = local_disc()
            if not disc:
                return self.send_error(404)
            if self.path.split('?')[0] == '/disco/info':
                info = {'name': os.path.basename(disc), 'size': os.path.getsize(disc)}
                return self.reply(json.dumps(info).encode(), 'application/json')
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(os.path.getsize(disc)))
            self.end_headers()
            with open(disc, 'rb') as f:
                try:
                    shutil.copyfileobj(f, self.wfile, 1 << 20)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return
        if self.path.split('?')[0] in ('/grabar', '/grabar/'):
            self.path = '/grabar.html'
        if self.path.startswith('/refs/'):
            name = self.path[len('/refs/'):].split('?')[0]
            if name == 'list':
                os.makedirs(REFS, exist_ok=True)
                have = {f: True for f in os.listdir(REFS) if re.fullmatch(REF_NAME, f)}
                return self.reply(json.dumps(have).encode(), 'application/json')
            if not re.fullmatch(REF_NAME, name):
                return self.send_error(404)
            try:
                with open(os.path.join(REFS, name), 'rb') as f:
                    return self.reply(f.read(), 'audio/wav')
            except OSError:
                return self.send_error(404)
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
        if self.path.startswith('/voice/'):
            name = self.path[len('/voice/'):].split('?')[0]
            if not re.fullmatch(r'[0-9a-f]{8}\.wav|index\.json', name):
                return self.send_error(404)
            try:
                with open(os.path.join(VOICE, name), 'rb') as f:
                    body = f.read()
            except OSError:
                return self.send_error(404)
            return self.reply(body, 'application/json' if name.endswith('.json') else 'audio/wav')
        super().do_GET()

    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n)
        if self.path.startswith('/refs/'):
            name = self.path[len('/refs/'):]
            if not re.fullmatch(REF_NAME, name) or body[:4] != b'RIFF' or body[8:12] != b'WAVE':
                return self.send_error(400)
            os.makedirs(REFS, exist_ok=True)
            with open(os.path.join(REFS, name + '.tmp'), 'wb') as f:
                f.write(body)
            os.replace(os.path.join(REFS, name + '.tmp'), os.path.join(REFS, name))
            print('Voz: guardada voice_refs/%s (%.1f s)' % (name, (len(body) - 44) / 2 / max(1, struct.unpack_from('<I', body, 24)[0])), flush=True)
            return self.reply(b'ok', 'text/plain')
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
