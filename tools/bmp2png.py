#!/usr/bin/env python3
"""bmp2png.py in.bmp out.png — converts the 24-bit bottom-up BMPs written by SHADOWGUN_SHOT_EVERY."""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack_from('<ii', d, 18)
row = w * 3
raw = bytearray()
for y in range(h - 1, -1, -1):
    line = d[54 + y * row: 54 + (y + 1) * row]
    rgb = bytearray(line)
    rgb[0::3], rgb[2::3] = line[2::3], line[0::3]
    raw += b'\0' + rgb
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
