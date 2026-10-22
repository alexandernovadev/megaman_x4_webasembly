"""Lista y extrae archivos de una imagen de CD de PS1 (MODE2/2352, ISO9660).

Uso:
  python3 iso_extract.py DISCO.bin                  # solo lista
  python3 iso_extract.py DISCO.bin SALIDA [PREFIJO] # extrae (p.ej. PREFIJO=/ARC)

Solo copia datos Form 1 (2048 bytes por sector). Los .STR y .XA usan
sectores Form 2 y requieren otra extracción.
"""
import os
import struct
import sys

SECTOR = 2352
USER_OFFSET = 24  # sync(12) + header(4) + subheader(8)


class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')

    def sector(self, lba):
        self.f.seek(lba * SECTOR + USER_OFFSET)
        return self.f.read(2048)

    def read(self, lba, size):
        data = bytearray()
        for i in range((size + 2047) // 2048):
            data += self.sector(lba + i)
        return bytes(data[:size])

    def walk(self, lba=None, size=None, path=''):
        if lba is None:
            pvd = self.sector(16)
            assert pvd[1:6] == b'CD001', 'no es ISO9660'
            root = pvd[156:190]
            lba, = struct.unpack('<I', root[2:6])
            size, = struct.unpack('<I', root[10:14])
        d = self.read(lba, size)
        i = 0
        while i < len(d):
            length = d[i]
            if length == 0:
                i = (i // 2048 + 1) * 2048
                continue
            e = d[i:i + length]
            elba, = struct.unpack('<I', e[2:6])
            esize, = struct.unpack('<I', e[10:14])
            flags, nlen = e[25], e[32]
            name = e[33:33 + nlen]
            if name not in (b'\x00', b'\x01'):
                p = path + '/' + name.decode(errors='replace').split(';')[0]
                if flags & 2:
                    yield from self.walk(elba, esize, p)
                else:
                    yield p, elba, esize
            i += length


def main():
    disc = Disc(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else None
    prefix = sys.argv[3] if len(sys.argv) > 3 else ''
    for p, lba, size in disc.walk():
        print(f'{size:>11,}  lba {lba:>6}  {p}')
        if out and p.startswith(prefix) and not p.endswith(('.STR', '.XA', '.DAT')):
            dst = os.path.join(out, p.lstrip('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as fh:
                fh.write(disc.read(lba, size))


if __name__ == '__main__':
    main()
