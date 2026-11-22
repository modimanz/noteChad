"""Regenerate the original page-and-hash application icon (Python stdlib only)."""
from pathlib import Path
import struct
import zlib


def png(size):
    def pixel(x, y):
        x = (x + .5) * 32 / size; y = (y + .5) * 32 / size
        bg = (0, 0, 0, 0)
        if 4 <= x < 28 and 2 <= y < 30 and (x < 21 or y >= x - 19):
            bg = (36, 68, 66, 255) if x <= 21 or y >= 9 else (103, 171, 151, 255)
            if 10 <= y < 24 and (11 <= x < 13.5 or 17 <= x < 19.5): bg = (241, 239, 215, 255)
            if 8 <= x < 23 and (13 <= y < 15 or 19 <= y < 21): bg = (241, 239, 215, 255)
        return bytes(bg)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + b''.join(pixel(x,y) for x in range(size)) for y in range(size))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',size,size,8,6,0,0,0)) + chunk(b'IDAT', zlib.compress(rows,9)) + chunk(b'IEND',b'')

sizes = (16, 32, 48); images = [png(s) for s in sizes]; offset = 6 + len(sizes) * 16
entries = []
for s, image in zip(sizes, images):
    entries.append(struct.pack('<BBBBHHII',s,s,0,0,1,32,len(image),offset)); offset += len(image)
(Path(__file__).resolve().parents[1] / 'src/notechad.ico').write_bytes(struct.pack('<HHH',0,1,len(entries))+b''.join(entries+images))
