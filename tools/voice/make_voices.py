"""Genera el paquete de voces en español (voice_pack/) a partir de un guion de diálogos.

Uso (con el Python que tiene piper-tts, ver tools/voice/README.md):
  ~/tools/voice/venv/bin/python -I tools/voice/make_voices.py voice_lines/poc.json voice_pack

El guion dice qué mensajes del disco se doblan y cómo. Cada mensaje es una lista de cajas, en el
orden del juego, con el subtítulo en español ("es") y, si la caja empieza una frase, lo que se
dice ("say"; el subtítulo si falta). Una caja sin "say" ni "es" propio continúa la frase de la
anterior: comparte subtítulo y no corta el audio. La voz de cada personaje sale de "voices".

Cada caja se reconoce por la huella de su texto original (como runtime/voice.c), así que el
paquete queda: voice_pack/<huella>.wav y voice_pack/index.json {huella: {who, es, audio}}.
Todo sale del disco del usuario: es de uso local y no se distribuye.
"""
import json
import os
import struct
import sys
import wave

from piper import PiperVoice, SynthesisConfig

MODELS = os.path.expanduser('~/tools/voice/models')
ARC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'extracted', 'ARC')


def box_end(w):
    return w >> 12 in (0x2, 0xa)


def boxes(arc, table, msg):
    """Cajas del mensaje `msg` (lista de listas de caracteres de 16 bits)."""
    with open(os.path.join(ARC_DIR, arc), 'rb') as f:
        b = f.read()
    start = table + struct.unpack_from('<H', b, table + 2 * msg)[0]
    out, cur, o = [], [], start
    while True:
        w, = struct.unpack_from('<H', b, o)
        o += 2
        cur.append(w)
        if box_end(w):
            out.append(cur)
            cur = []
            if w >> 12 == 0xa:
                return out


def key(words):
    h = 2166136261
    for w in words:
        h = ((h ^ (w & 0xff)) * 16777619) & 0xffffffff
        h = ((h ^ (w >> 8)) * 16777619) & 0xffffffff
    return '%08x' % (h or 1)


def speaker(words):
    """Nombre de la primera línea de la caja (caracteres con el byte alto 0x00/0x01/0x50/0x51)."""
    name = []
    for w in words:
        c = w & 0xff
        name.append(' ' if c == 0xff else chr(c - 9) if 0x4a <= c <= 0x63 else '.' if c == 0x86 else '?')
        if w >> 12 == 0x5:
            return ''.join(name).strip()
        if w >> 8 not in (0x00, 0x01, 0x08):
            return ''
    return ''


def main():
    script, pack = sys.argv[1], sys.argv[2]
    os.makedirs(pack, exist_ok=True)
    with open(script, encoding='utf-8') as f:
        cfg = json.load(f)
    voices = {}
    index_path = os.path.join(pack, 'index.json')
    index = json.load(open(index_path, encoding='utf-8')) if os.path.exists(index_path) else {}
    for m in cfg['messages']:
        orig = boxes(m['arc'], int(m['table'], 16), m['msg'])
        if len(orig) != len(m['boxes']):
            sys.exit('%s mensaje %d: el disco tiene %d cajas y el guion %d' % (m['arc'], m['msg'], len(orig), len(m['boxes'])))
        es = who = ''
        for words, box in zip(orig, m['boxes']):
            k = key(words)
            who = speaker(words) or who
            entry = {'who': cfg.get('names', {}).get(who, who)}
            if 'es' in box:
                es = box['es']
            entry['es'] = es
            if 'say' in box or 'es' in box:
                v = cfg['voices'][who]
                if v['model'] not in voices:
                    voices[v['model']] = PiperVoice.load(os.path.join(MODELS, v['model'] + '.onnx'))
                syn = SynthesisConfig(speaker_id=v.get('speaker'), length_scale=v.get('speed', 1.0),
                                      noise_scale=v.get('noise', 0.667), noise_w_scale=v.get('noise_w', 0.8))
                with wave.open(os.path.join(pack, k + '.wav'), 'wb') as wf:
                    voices[v['model']].synthesize_wav(box.get('say', es), wf, syn_config=syn)
                entry['audio'] = k + '.wav'
            index[k] = entry
            print(k, entry['who'], '|', entry['es'], '(audio)' if 'audio' in entry else '')
    with open(index_path, 'w', encoding='utf-8') as f:
        json.dump(index, f, ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
