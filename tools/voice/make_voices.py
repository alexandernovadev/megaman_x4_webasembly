"""Genera el paquete de voces en español (voice_pack/) a partir de un guion de diálogos.

Uso (con el Python del motor de voz, ver tools/voice/README.md):
  ~/tools/voice/cb/bin/python -I tools/voice/make_voices.py voice_lines/poc.json voice_pack

El guion dice qué mensajes del disco se doblan y cómo. Cada mensaje es una lista de cajas, en el
orden del juego, con el subtítulo en español ("es") y, si la caja empieza una frase, lo que se
dice ("say"; el subtítulo si falta). Una caja sin "say" ni "es" propio continúa la frase de la
anterior: comparte subtítulo y no corta el audio. La voz de cada personaje sale de "voices":

  - "engine": "chatterbox" (por defecto): imita la grabación "refs" (voice_refs/, se graban en
    la página /grabar); una caja puede pedir otra toma con "ref" (por ejemplo la "fuerte" para un
    grito). "exaggeration" y "cfg" son los parámetros de Chatterbox. Antes se limpia la toma
    (voice_refs/clean/): sin silencios en los extremos, sin ruido de fondo y a volumen común.
  - "engine": "piper": voz sintética con el modelo "model".
  - "fx": efectos para que la voz no quede igual que la grabación: "pitch" (semitonos),
    "radio" (filtro de transmisión), "chorus", "drive" (dB de distorsión) y "reverb" (0-1).

Cada caja se reconoce por la huella de su texto original (como runtime/voice.c), así que el
paquete queda: voice_pack/<huella>.wav y voice_pack/index.json {huella: {who, es, audio}}.
Todo sale del disco del usuario: es de uso local y no se distribuye.
"""
import json
import os
import struct
import sys

import numpy as np
import soundfile as sf

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
MODELS = os.path.expanduser('~/tools/voice/models')
ARC_DIR = os.path.join(ROOT, 'extracted', 'ARC')
REFS = os.path.join(ROOT, 'voice_refs')


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


def clean_ref(path):
    """Toma limpia para imitar: recorta los silencios, quita el ruido de fondo y normaliza."""
    out = os.path.join(REFS, 'clean', os.path.basename(path))
    if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(path):
        return out
    import noisereduce
    from pedalboard import HighpassFilter, Pedalboard
    audio, sr = sf.read(path, dtype='float32')
    if audio.ndim > 1:
        audio = audio.mean(1)
    audio = Pedalboard([HighpassFilter(80)])(audio, sr)
    audio = noisereduce.reduce_noise(y=audio, sr=sr, stationary=True, prop_decrease=0.85)
    frame = sr // 50
    level = np.sqrt(np.convolve(audio ** 2, np.ones(frame) / frame, 'same'))
    loud = np.flatnonzero(level > level.max() * 0.06)
    pad = sr // 8
    audio = audio[max(0, loud[0] - pad):loud[-1] + pad]
    audio *= 10 ** (-20 / 20) / (np.sqrt(np.mean(audio ** 2)) or 1)
    audio = np.clip(audio, -0.99, 0.99)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sf.write(out, audio, sr, subtype='PCM_16')
    return out


class Chatterbox:
    def __init__(self):
        import torch
        from chatterbox.mtl_tts import ChatterboxMultilingualTTS
        dev = 'mps' if torch.backends.mps.is_available() else 'cpu'
        self.tts = ChatterboxMultilingualTTS.from_pretrained(device=dev)
        self.sr = self.tts.sr

    def say(self, text, v, box):
        ref = clean_ref(os.path.join(REFS, '%s_%d.wav' % (v['refs'], box.get('ref', v.get('ref', 1)))))
        w = self.tts.generate(text, language_id='es', audio_prompt_path=ref,
                              exaggeration=box.get('exaggeration', v.get('exaggeration', 0.5)),
                              cfg_weight=v.get('cfg', 0.5))
        return w.squeeze(0).cpu().numpy()


class Piper:
    def __init__(self):
        self.voices = {}
        self.sr = None

    def say(self, text, v, box):
        from piper import PiperVoice, SynthesisConfig
        if v['model'] not in self.voices:
            self.voices[v['model']] = PiperVoice.load(os.path.join(MODELS, v['model'] + '.onnx'))
        voice = self.voices[v['model']]
        self.sr = voice.config.sample_rate
        syn = SynthesisConfig(speaker_id=v.get('speaker'), length_scale=v.get('speed', 1.0),
                              noise_scale=v.get('noise', 0.667), noise_w_scale=v.get('noise_w', 0.8))
        return np.concatenate([c.audio_float_array for c in voice.synthesize(text, syn_config=syn)])


def effects(audio, sr, fx):
    """Efectos de la voz (pedalboard): los de "fx" y un realce de presencia para que suene nítida.
    Termina normalizando a un volumen común."""
    from pedalboard import (Chorus, Compressor, Distortion, HighpassFilter, HighShelfFilter, LowpassFilter,
                            PeakFilter, Pedalboard, PitchShift, Reverb)
    chain = [HighpassFilter(70)]
    if fx.get('pitch'):
        chain.append(PitchShift(semitones=fx['pitch']))
    if fx.get('drive'):
        chain.append(Distortion(drive_db=fx['drive']))
    if fx.get('radio'):
        chain += [HighpassFilter(320), LowpassFilter(3600), Distortion(drive_db=6)]
    if fx.get('chorus'):
        chain.append(Chorus(rate_hz=0.8, depth=0.15, centre_delay_ms=9, feedback=0.1, mix=fx['chorus']))
    chain += [PeakFilter(250, -2.5, 1.0), PeakFilter(3500, 3.0, 0.8), HighShelfFilter(8000, 2.0),
              Compressor(threshold_db=-20, ratio=3, attack_ms=5, release_ms=80)]
    if fx.get('reverb'):
        chain.append(Reverb(room_size=0.35, damping=0.6, wet_level=fx['reverb'], dry_level=1.0, width=0.6))
    out = Pedalboard(chain)(audio.astype(np.float32), sr)
    peak = np.abs(out).max() or 1.0
    return out * (0.89 / peak)


def main():
    script, pack = sys.argv[1], sys.argv[2]
    only = set(sys.argv[3:])  # opcional: solo estos personajes
    os.makedirs(pack, exist_ok=True)
    with open(script, encoding='utf-8') as f:
        cfg = json.load(f)
    engines = {}
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
                if only and who not in only:
                    entry['audio'] = index.get(k, {}).get('audio', k + '.wav')
                else:
                    v = cfg['voices'][who]
                    name = v.get('engine', 'chatterbox')
                    if name not in engines:
                        engines[name] = Chatterbox() if name == 'chatterbox' else Piper()
                    eng = engines[name]
                    audio = eng.say(box.get('say', es), v, box)
                    sf.write(os.path.join(pack, k + '.wav'), effects(audio, eng.sr, v.get('fx', {})), eng.sr,
                             subtype='PCM_16')
                    entry['audio'] = k + '.wav'
            index[k] = entry
            print(k, entry['who'], '|', entry['es'], '(audio)' if 'audio' in entry else '', flush=True)
    with open(index_path, 'w', encoding='utf-8') as f:
        json.dump(index, f, ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
